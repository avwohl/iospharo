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

### BISECTION RESULT — boundary at compile #13

`JIT_MAX_COMPILE=N` bisection at PHARO_JIT_DEFER=0:

```
MAX=0..12:  5/5 success at JIT speed (eval = 1028457)
MAX=13+:    hangs at startup
```

**Compile #13 = `#nextPut:`** (WriteStream>>nextPut: family)

The first 12 methods that compile (all simple Object/Collection
accessors) are JIT-compiled correctly.  Compile #13 is the FIRST
one whose JIT'd code triggers the cascade.

This required fixing JITCompiler::compile to honor `JIT_MAX_COMPILE`
on the recompile path too (was only honored on noteMethodEntry path).

```c
// JITCompiler.cpp:1503 — added at function entry
if (maxCompile >= 0 && (int)methodsCompiled_ >= maxCompile) {
    compilationsFailed_++;
    return nullptr;
}
```

**Caveat**: excluding `nextPut:` alone via JIT_EXCLUDE doesn't fix
the hang — other methods compiled in its slot also have buggy
codegen.  There's a long tail of buggy compiles, not a single
isolated bug.

**Further refinement (same session)**: even excluding `nextPut:`
by exact methodOop (JIT_EXCLUDE_OOP=0x30046ce80) still hangs — a
DIFFERENT method takes the #13 slot and triggers the cascade.

**Even more revealing**: excluding the FIRST 12 selector names
(basicNew:, new:, /, size, on:, reset:, max:, at:, do:, at:put:,
key:) lets 718 OTHER methods compile.  Still hangs.

**Conclusion**: the bug is NOT in any specific method's JIT
codegen.  It's an AGGREGATE state issue — once compile count
crosses ~12 during startup, some shared resource breaks.

**Stats comparison** (MAX=12 vs MAX=20):
```
                    MAX=12       MAX=20
Code zone           102 KB       193 KB     (+89 KB, modest)
IC: hits/total      91/92        95/96      (similar)
J2J-r: chains/tot   0/11         0/11       (same)
J2J-a: hits/tot     1/80         5/84       (+4 successful J2J activations)
J2J-d: direct       2            2          (same)
J2J-s: stencil      1/2          1/2        (same)
methodMap tracked   374          374        (same — count of seen oops)
```

The notable difference is J2J-a (J2J activations succeeded): 1
at MAX=12, 5 at MAX=20.  When more methods are JIT-compiled, more
of them become reachable as J2J callees from already-compiled
callers.

**But disabling J2J entirely (PHARO_NO_J2J=1) STILL hangs at
DEFER=0.**  So J2J transitions aren't the unique culprit either.

Candidates remaining:
- Code zone allocation hits a boundary at ~150 KB
  (JIT page transitions, mprotect activity)
- A specific JIT compile triggers OS-level state change (W^X
  transition?) that doesn't compose with subsequent compiles
- Aggregate IC patches accumulate past a threshold for some
  global cache (megaCache, methodCache_)
- The JIT compile work + startup process scheduling crosses
  some invariant that only holds when defer keeps compile inactive
- Something in `applyICSpecialization` triggers when enough sites
  have IC data — maybe a specific specialization reaches a buggy
  pattern

### Universality + viability of MAX=12 as fallback

**Same boundary across workloads**:
- `eval "28 benchFib"`: MAX=12 success, MAX=13+ hangs
- `eval "100 benchmark"`: MAX=12 success (=1028), MAX=15+ hangs
- bench-suite at DEFER=0 unbounded: 822 compiles, hangs
- bench-suite at DEFER=0 MAX=12: ran through tinyBench, fib,
  sieve, sort, dict before hitting the pre-existing floatSum
  failure (sieve 7ms = JIT speed; sort 273ms ≈ JIT speed)

So MAX=12 + DEFER=0 IS a working configuration — just limited
to the 12 hottest methods getting JIT'd.  fib(28) eval timing:

```
DEFER=4 (default, no clamp bypass): 5s
DEFER=0 + JIT_MAX_COMPILE=12       : 6s  (12 methods JIT'd)
DEFER=0 unbounded                   : hang
```

Higher PHARO_JIT_THRESHOLD doesn't help (5..500 all hang
eventually — they just delay reaching the 13-compile boundary).

**Pragmatic interim** (not shipped — would need wider
validation): default to JIT_MAX_COMPILE=12 in headless eval mode.
Loses most JIT perf but unlocks DEFER=0.  Probably not worth it
since DEFER=4 works fine and the 4s wait is acceptable for the
workloads we care about.

### lldb attempt + breakthrough on what the "hang" actually is

Tried `lldb -p $PID` to attach and `lldb -b -s ...` to launch.
Both hung.  macOS codesigning restricts lldb attachment without
proper entitlements; the full lldb session machinery isn't
available via Bash here.

But the lldb attempt led to a key observation: at MAX=13 +
DEFER=0, the process is **not actually hung** — it's running
at ~1.5M sends/sec (vs 30M/sec normal), with the JIT stats
showing:

```
J2J-a: 47610/56701  (47K successful J2J activations)
J2J-r: 0/47117      (ZERO successful J2J resumes — every
                     resume bails to interp)
13 compiled methods, 222K compilations failed (the MAX cap)
```

Run with longer timeout (120s instead of 25s):

```
After 120s: error reaches Smalltalk runtime —
   "Improper store into indexable object"
   CompiledMethod(Object)>>error:
   CompiledMethod(Object)>>errorImproperStore
```

So **the "hang" is actually a slow-cascade ending in
errorImproperStore** — Pharo's runtime check that an at:put:
target is a properly indexable object.  A JIT-compiled method
is passing the wrong receiver/arg to at:put:.

Likely candidates (all do at:put: codegen):
- nextPut: — the cascade-trigger method (compile #13)
- Method #11 at:put: itself
- Some inner at:put: call chain

The JIT codegen for nextPut:'s `bc[15]: sendJ2J at:put:` (the
inner `collection at: position put: aValue` call) probably has
wrong operand encoding or wrong receiver selection.  At MAX=12
this method runs in interp (correct), at MAX≥13 it runs JIT
(wrong) and eventually errors.

### Multiple failure modes — bug is timing-dependent corruption

Subsequent runs at PHARO_JIT_DEFER=0 produced DIFFERENT failure
modes per run:

```
Run A (~120s):  errorImproperStore cascade
                "Improper store into indexable object"
                CompiledMethod(Object)>>errorImproperStore

Run B (~38s):   process termination via #ensure: / FileLocator>>resolve
                sender=nil chain length 1 (sender-chain corruption)

Run C (~25s):   SIGSEGV in lookupMethod +840
                x1=0x100000000 (out-of-heap pointer)

Run D (timeout): no progress, JIT cycle loop
```

So the bug class is **timing-dependent corruption** that
manifests differently each run.  It's not ONE specific codegen
bug — it's a CLASS of bugs unmasked when JIT compile runs
during startup.

The PHARO_TRACE_AT_PUT_FAIL diagnostic added (then reverted) for
the errorImproperStore path didn't fire in subsequent runs —
the at:put: failure must happen with a DIFFERENT failure mode
(e.g., immutable check, range check) per run.

### Concrete next-session path

1. Set up lldb with proper codesigning (the project may have a
   debug entitlements plist somewhere).  Without lldb attaching,
   we can't observe the corruption window directly.
2. Build with sanitizers (ASAN/UBSAN) and replicate the bug.
   Sanitizers may pinpoint the corruption write.
3. Add a periodic "consistency check" that walks key data
   structures (active method's literal frame, scheduler queues,
   IC cache integrity) and logs when corruption appears.  Catches
   the corruption MOMENT rather than its consequences.
4. **Practical alternative**: ship `JIT_MAX_COMPILE=12` as the
   default for headless eval mode.  Loses most JIT perf for
   short evals (where defer mostly does the same thing anyway).
   For longer workloads, default DEFER=4 still works.  Not a
   real fix — just a workaround that exchanges 4s wait for
   12-method JIT cap.

### Additional ruled-out hypotheses (this session)

Tested various JIT-feature opt-out flags at PHARO_JIT_DEFER=0:

```
PHARO_NO_SISTA=1            still 0/3 success
PHARO_NO_OSR=1              still hangs (754 compiles)
PHARO_NO_IC_FILL=1          still hangs
PHARO_NO_CHAIN=1            still hangs (751 compiles)
PHARO_NO_EAGER_COMPILE=1    still hangs (760 compiles)
PHARO_JIT_NO_BLOCKS=1       still hangs
```

**Bonus surprise**: temporarily added `PHARO_NO_JIT_ACTIVATE=1`
(compile but never execute JIT code).  Result: STILL HANGS at
DEFER=0 (400 compiles, system grinds at 530K steps/sec).

So the bug isn't even in JIT execution — it's in JIT compile
itself stealing enough resources during startup that the system
can't make adequate progress.  Or the COMPILE side has a write
that corrupts shared state independently of execution.

This narrows further: even disabling all known opt-out paths,
DEFER=0 hangs.  The remaining bug class is:

- JIT compile-time work taking too much CPU during the critical
  startup window (scheduler can't make progress)
- JIT compile WRITES (IC patches, codeZone allocation, methodMap
  inserts) corrupt shared state
- Memory pressure / GC pattern from compile work disrupts startup

Without lldb / sanitizers, can't isolate further.

### What this tells us about the architecture

The 4s clamp WORKS because by the time defer expires, all the
buggy methods have already been "warmed up" in interp and
classified differently / IC patched / etc.  The compile happens
in a different state where the same code paths produce different
output.

Or, the system is past the windows where these buggy methods are
called critically — they get compiled "after the fact" and don't
affect correctness.

### Concrete next-session investigation

With the bisection narrowed to `#nextPut:` (compile #13):

1. **Look at nextPut: bytecodes** — what's special about its
   shape vs the 12 that compile fine?
2. **Disassemble the JIT output for nextPut:** — is there a wrong
   immediate, a missing barrier, an off-by-one?
3. **Check what specifically goes wrong** — does the compiled
   nextPut: get called and return wrong data, or does its
   compilation itself corrupt some shared state (IC, megacache)?
4. **Compare with same method compiled at DEFER=4** — if the JIT
   output differs, find why (different IC state at compile time?
   different inline-cache data?)

### Continuing the session — additional narrowing

**Item 4 done** (`PHARO_NO_QUEUE_COMPILE=1` at low DEFER): same
crash signature with both modes (lookupMethod +840, x1=0x100000000,
fault 0x100000004).  Bug is NOT queue safe-point timing — it's in
the compile pipeline OR runtime IC OR send dispatch path.

**Critical re-discovery**: x1=0x100000000 is the **selector**, not
the classOop.  ARM64 calling convention: x0=`this`, x1=arg1
(selector), x2=arg2 (classOop).  The fault is in
`selector.asObjectPtr()->identityHash()` (lookupInMethodDict line
6922) — reading at selector+4 (the hash field within ObjectHeader).

So the bad oop is being passed AS THE SELECTOR.  Selectors come
from `literal(idx)` in send dispatch, which reads from
`method_`'s literal frame.  Either:
- The active method's literal slot has been overwritten with 0x100000000
- The active method itself is a wrong/corrupt object

**Bench mode at DEFER=0 WORKS** (5/5 fib(28)=14ms, JIT speed).
Bench mode bypasses startup chain via `executeFromContext`.
This narrows the bug to **startup-chain code being JIT-compiled
incorrectly**.  Specifically:
- StartupPreferencesLoader
- FileLocator>>resolve
- Image-init methods touched between resume and eval-DoIt invocation

JIT_EXCLUDE for common startup selectors (basicNew:, at:, new:,
size, keysDo:, on:, /) didn't help — the buggy method is more
specific (or is one not in that list).

### Summary of narrowing across this session

```
Mode                              Result
-----------------------------     ------
DEFER=4 (default)                 5/5 success
DEFER=2 queue-OFF                 0/5 (5/5 SIGSEGV)
DEFER=2 queue-ON                  0/5 (3/5 SIGSEGV)
DEFER=0 queue-ON eval             0/5 (hang, no crash)
DEFER=0 PHARO_BENCH=fib28         5/5 success at JIT speed
DEFER=0 + JIT_EXCLUDE=common      0/5 (still hang/crash)
```

The bug is in startup-chain methods getting JIT-compiled.  Bench
mode skips those methods entirely (executeFromContext path).  Eval
mode runs them all and hits the bug.

### What WOULD be productive (next session)

The previous probes were too high-level.  Need lower-level tools:

1. **lldb single-stepping** at the actual crash PC.  Get the
   register state and walk back the data flow to find where
   `0x100000000` enters the active method's literal frame.

2. **Active-method tracking when bad selector enters dispatch**:
   when `selector.isObject() && !isValidPointer(selector)` in
   send dispatch, dump the active method's class>>selector AND the
   literal index being read.  This identifies WHICH method has
   the corrupt literal frame.

3. **Per-method JIT exclusion bisection**: enable JIT logging of
   every compile, then with bisection try `JIT_EXCLUDE` for halves
   of the compiled-during-startup set.  If excluding half fixes
   it, recurse.  Identifies the specific method whose JIT code
   corrupts state.

4. **GC write-barrier instrumentation on literal frames**: when
   an old-space CompiledMethod's literal slot is overwritten,
   log it.  Catches the actual moment the literal frame becomes
   0x100000000.

Items 2 and 3 are tractable in a 2-4 hour session each.

## Status

**P0 not solved.**  Investigation produced one shipped guard
(`JITCompiler::compile` entry validation) and confirmed the bug
is multi-source rather than single-source.  4s clamp stays.

Next session: start with item (4) above (queue vs inline at low
DEFER) — that's the cheapest investigation and would narrow scope
significantly.
