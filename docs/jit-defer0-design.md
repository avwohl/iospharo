# Making JIT work at PHARO_JIT_DEFER=0 — Design

Status: design document.  Tracking the multi-week effort to remove
the 4s defer clamp (deferred.md A1) so JIT compile is safe during
SessionManager startup.

## Problem statement

At `PHARO_JIT_DEFER=4s` (current default in headless mode) the system
boots reliably and JIT compile fires after image init is complete.
At any value `<4s` the system crashes or hangs intermittently.  The
4s floor is a workaround for JIT correctness bugs that manifest when
compile fires during startup.

Goal: make `PHARO_JIT_DEFER=0` work reliably (10/10 success across
eval, bench-suite, sunit), then drop the clamp.

## What's already correct (NOT the bug)

Investigation 2026-05-05 found:

**IC keys ARE classIndex, not raw class oops.**  See
`Interpreter.cpp:15655`:
```cpp
lookupKey = receiver.asObjectPtr()->classIndex();
```
The IC's `key` slot stores the 22-bit class identityHash (which IS
the classTable index in Spur), or a tagged-immediate marker (bit 31
set) for SmallInt/Character/SmallFloat.  GC-promotion of a class
doesn't invalidate IC keys — the index stays the same; only the
class oop in `classTable_[index]` updates.

So the originally-suspected "IC stores raw class oops, GC moves
them, lookup walks into freed memory" failure mode does NOT apply.

## Where the actual corruption lives (under investigation)

At `PHARO_JIT_DEFER=2`, the crash signature is:

```
Fault addr=0x100000004
x1=0x100000000      ← out-of-heap "class oop"
PC: lookupMethod +732..+852
```

Path: `interpret() send dispatch` → `classOf(rcvr)` → `classAtIndex(rcvr.header->classIndex())`
→ `classTable_[idx]` → returns 0x100000000 (corrupt) → `lookupMethod(sel, 0x100000000)`
→ crash inside the loop body before/during `methodDictOf`.

The `0x100000000` value (= `1 << 32`, no other bits) doesn't look like
a real heap address.  It looks like a partial overwrite — possibly
the high half of an IC `extra` word being misread as a separate value,
or an oop with classification bits stripped that left only a single
bit set.

**Hypothesis (not yet verified)**: a method's CompiledMethod
header gets JIT-compiled with a class oop or class binding baked as
an immediate operand, and that operand's encoding leaks bit 32 set
when the relevant class moves through GC promotion.  Or
`classTable_` itself is being trampled by some startup-window code.

Defensive guards I shipped today (`db8e914d`, `252ef523`) catch
this specific oop pattern — `isValidPointer` returns false for
`0x100000000` (below `oldSpaceStart_=0x300000000`) — but the crash
PC suggests the optimizer hoists a load above the guard.  The
binary's actual code at `lookupMethod +852` needs disassembly to
confirm what's being dereferenced.

## The 4 architectural items, re-prioritized

After today's investigation, the priorities have shifted from my
original list:

**P0: Find what corrupts the class oop / classTable in startup
window.**  This is the immediate blocker.  Until we know the source,
guards are whack-a-mole.  Investigation tools needed:
- Watchpoint on `classTable_[i]` writes during startup
- Trace of `header->classIndex()` per receiver in send dispatch
- Disassembly of the actual crash PC offset

**P1: Validate every heap fetch in JITCompiler::compile.**  Our crash
data shows ~4-5 distinct crash sites; defensive validation +
graceful bail would silence them.  Not architectural but high-value.
Estimated 1-2 days.

**P2: Refcount JITMethod by active frame count.**  The
"sender chain corruption" failure mode at low DEFER comes from
recompile evicting code while a frame still holds a return PC.
Real fix: don't free old code until all frames have returned past
it.  3-5 days.

**P3: Co-installed forwarding stubs for J2J recompile.**  Overlaps
with P2 — addresses the same in-flight-call problem from the J2J
side.  2-4 days.

**P4: Class-oop relocation table for JIT immediates.**  If JIT bakes
class oops into machine code as immediates (item #5 in the original
list), GC promotion invalidates them.  Need an indirection: emit
loads through a per-method or global relocation table that GC fixes.
Verifying this is even happening is part of P0 investigation.
3-5 days.

## What's NOT a design priority

- IC key encoding (already classIndex, not raw oop)
- IC slot validation (already heap-range checked in writers)
- Sender chain cycle detection (already `setSenderSafe` lambda)
- Queue-compile timing (already shipped, working)

## Migration / rollout strategy

Each fix should be:
1. **Default-off behind a flag** until validated
2. **Validated against all 4 test domains**: eval-mode (DEFER=0), bench-suite, SUnit smoke, bench-panel (when image is fixed)
3. **Flipped default-on** only after 10/10 stability at the target DEFER
4. **Gates each fix sequentially** — no flipping P1 default-on until P0 is complete

Total realistic estimate to land all 4: **3-4 weeks**.  Each item
gets its own session with focused investigation, design, implementation,
and validation.

## Today's session deliverables

- `db8e914d`: lookupInMethodDict heap-range guards
- `252ef523`: lookupMethod heap-range guards + `PHARO_NO_DEFER_CLAMP` test handle
- This design doc
- Confirmation that the originally-suspected IC encoding bug is NOT
  the root cause — the IC layer is fine
- Identification of `0x100000000` as the consistent corrupt-oop
  signature, suggesting a specific bit-pattern bug rather than
  generic memory corruption

## P0 investigation log

### Session 2026-05-05 (initial probe)

Tried 3 instrumentation approaches:

1. **`classOf` return-value diagnostic** (`PHARO_DIAG_BAD_CLASSOF=1`).
   Captures cases where `classAtIndex(header->classIndex())` returns
   an out-of-heap oop.  Result: **never fired** at DEFER=2 across
   multiple runs.  So either (a) `classOf` isn't the source, or
   (b) the inlined `classOf` body skips the path with the diagnostic.

2. **`lookupMethod` entry guard** (`PHARO_DIAG_LOOKUP_BAD_CLASS=1`)
   with volatile-load to defeat optimizer hoisting.  Result: **never
   fired** at DEFER=2.  Crashes happened before the diagnostic ran
   (different code paths reached the SIGSEGV first).

3. **Crash-site profiling**: 5 runs each at DEFER=0..4, captured PC
   offsets.  Crashes occur in DIFFERENT functions per run:
   - `JITCompiler::compile +232` (entry path)
   - `JITCompiler::compile +21568` (~end of compile)
   - `JITCompiler::compile +21700` (~end of compile)
   - `lookupMethod +652..+852` (inside loop)
   - `lookupInMethodDict +236..+300` (slot access)

   Each run hits a different one — confirms multiple corruption
   paths active simultaneously, not one root bug.

**Tactical fix shipped this session**: `JITCompiler::compile` now
validates the input `compiledMethod` oop before `asObjectPtr()`.
At low DEFER, the safe-point queue can hand us oops whose objects
have been freed/moved — without the guard, the entry deref crashes
in `methObj->slotAt(0)`.  Bails to `compilationsFailed_++` instead.

**Doesn't reach goal**: 0/5 success at DEFER=0..3 (same as before),
but reduces the variety of crash sites.  The +21700 crash (deep in
compile, near codegen finalization) is the next thing to chase.

### What WASN'T productive

- Adding diagnostics that the compiler then optimizes away or that
  never reach the failing path.
- Chasing different crash PCs — they're symptoms, not causes.
- Trying to deduce the root cause from the `0x100000000` bit pattern
  alone without bisecting WHICH operation produces it.

### What WOULD be productive (next session)

The previous probes were too high-level.  Need lower-level tools:

1. **lldb single-stepping** at the actual crash PC.  Get the
   register state and walk back the data flow to find where
   `0x100000000` enters the program.  Requires interactive debug
   session; not productive via grep/edit cycles.

2. **GC write-barrier instrumentation**: log every classTable write
   AND every header.classIndex change for objects in old space.
   Look for patterns where an object's classIndex shifts from a
   small value to one that decodes to 0x100000000.

3. **JIT compile output capture**: dump every method's compiled
   machine code at low DEFER.  Look for class-oop immediates that
   fall in the bit pattern leading to 0x100000000.  The single-bit
   pattern strongly suggests an arithmetic mistake (shift by 32?
   `1ULL << x` where x is wrong?), not memory corruption.

4. **`PHARO_NO_QUEUE_COMPILE=1` at low DEFER**: does the legacy
   inline-compile path crash at the same PCs?  If yes, the bug
   is in the compile pipeline.  If no, the bug is in the queue
   safe-point drain (timing-dependent invalidation).

These need 2-4 hours of focused interactive debugging per item,
not a session of grep+edit.

## Status

**P0 not solved.**  Investigation produced one shipped guard
(`JITCompiler::compile` entry validation) and confirmed the bug
is multi-source rather than single-source.  4s clamp stays.

Next session: start with item (4) above (queue vs inline at low
DEFER) — that's the cheapest investigation and would narrow scope
significantly.
