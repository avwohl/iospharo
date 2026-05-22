# jit-may23 — remaining work after jit-may22b

State at start of this doc (2026-05-22 end of session, branch `jit`
at `bd3939c0`):

    benchmark         current (ms)   vs Cog    target
    fib(28)              179             60×    ≤  3×
    sieve x100             8            0.8×   ✓ faster
    sort 100K            835             49×    ≤  3×
    dict 50K             447             34×    ≤  3×
    sum 1M               284             95×    ≤  5×
    factorial 5K          21             10×    ≤  3×
    block 1M               1            0.3×   ✓ faster
    instVar 1M           260            130×    ≤  3×
    100K alloc            13              4×    ≤ 1.5×
    floatSum 1M          402             50×    ≤  3×
    stringHash 100K      169             85×    ≤  3×
    collect 10x100K      510             12×    ≤  2×
    select 10x100K       643             80×    ≤  3×

3 benches now faster than Cog (sieve, block, and informally — block).
Most others still 30-130× slower.  The session's bit-57/58 inline
emits gave ~3-5% across 6+ benches; the **40-130× gaps remain**.

## What's left from jit-may22b

### Step 1 — Sista cache GC integration (PARTIAL)

✅ Cache rekey via forwarders (compact + scavenge).
✅ Sista deopt path GC-safe (bytecodeBase dynamic from state.method
   + startBcOffset).

❌ **Bail-only mode still has stale-state issue** — preserves
   buggy Sista compiles across GC.  Bisected to a diagnostic-mode
   issue; not a production blocker but limits Sista debugging.

❌ **No perf benefit yet** — production mode doesn't enable
   PHARO_SISTA_REKEY_AFTER_GC=1 because nothing useful gets
   preserved (Sista doesn't make worthwhile compiles for default
   workloads).  Will become valuable once Steps 2/3 land.

### Step 2 — Real BLR emit at SISTA_BIT (INFRASTRUCTURE ONLY)

✅ asmjit-T1 BLR site at trySistaCall.
✅ jitT1SistaDispatch C++ helper with full pushFrame/popFrame.
✅ Hit/attempt counters.
✅ 0-arg fast-path works correctly.

❌ **0-arg path is perf-NEGATIVE** — C++ helper overhead (~20
   stores + frame push/pop) exceeds savings for short methods.
   Measured: alloc 12→10422 ms when enabled.  Default-OFF via
   PHARO_T1_SISTA_DISPATCH_ALLOW=1.

❌ **N-arg fast-path corrupts state** after ~64K calls.  Bisected
   to "args allowed but no local temps" subset.  Even simple
   #max: fails.  Needs more lldb-level debugging.

❌ **True inline emit** (no C++ helper) — multi-day refactor.
   Would write the JITState init in asmjit code directly, avoiding
   the round-trip cost.

### Step 4 — IC poly walk (DEFAULT-OFF, NO WIN)

Wired the asmjit-T1 IC probe to walk slots 0-2 inline.  Empirical:
1-3% slowdown across most benches because the slot-0 monomorphic
hit path picks up extra instructions paid on every warm hit.

Stays gated behind `PHARO_T1_IC_POLY_WALK=1` until a poly-heavy
workload is found that benefits.

### Step 11 — Trivial-forwarder collapse (INFRASTRUCTURE, ZERO HITS)

4 patterns supported (1-arg Send1, 0-arg Send0, 2-arg Send2,
0-arg SpecialSend).  **Fires ZERO times on Pharo 13's image**
because the canonical `Behavior>>new:` uses a cascade
(`^ self basicNew: x; initialize`) not a pure forwarder.

The collapse code is correct; just doesn't match Pharo's
actual patterns.  Extension to handle cascades would need
two-method IC dispatch encoding — multi-day, doesn't fit
the 64-bit IC extras layout trivially.

### Step 12 (asmjit-T1 partial) — multi-slot + retLit (WINS!)

✅ **bit-57 multi-slot inline emit** — `^ self[A] op1 self[B]
   op2 const`, ~3-4% on fib(28).  Fires 672× per bench-suite run.
✅ **bit-58 returnsLiteral inline emit** — `^ nil/true/false/0/1`
   patterns, 3-5% across 6 benches.  Fires 53070× per fib(28).

The session's only **demonstrated cumulative perf wins**.

### Step 13 — Hot-loop JIT threshold (INFRASTRUCTURE)

Method bytecode scan in `noteMethodEntry` for ExtJump backward
offsets; lowers threshold to 1.  Amortised O(1) per method
(scan caches on first sighting).

Default-on but no measurable bench impact in default mode —
the runOnce-style harness methods don't have internal backward
jumps; the loops live inside callee methods that already
compile via the default threshold of 2.

## Multi-day-to-multi-week remaining work

### Step 3 — kSendInlineSelf real lowering (10-14 days)

The IR scaffolding is in (kSendInlineSelf op, recogniser at
kSendCallHelper + kSendUnspeculated sites).  Sub-step 2 helper
uses recursive `fn(state)` which grows C stack by one frame per
level — fine for fib(28) depth 28, hard cap for deeper recursions.

Real fix: replace recursive fn-call with BR/BLR inside Sista's
lowered code using SistaSave as the save protocol.  asmjit
Compiler doesn't expose self-recursion cleanly; needs post-
finalize patching of the call sites.

### Step 5 — Per-site class-immediate IC HIT (8-12 days)

**Cog's biggest perf edge.**  Cog bakes the receiver classIndex
as an immediate into the emitted code at IC fill time:

    cmp w4, #classIndex          ; 1 instruction

vs our current:

    ldr x6, [x5]                 ; load icData[0]
    cmp x4, x6                   ; 2 instructions

The savings stack: every IC HIT loses 1 instruction.  At 60M+
sends per bench-suite run, that's ~60M cycles = ~20 ms across
the whole suite.

Implementation needs **patchable code regions** in asmjit-T1.
Currently the code zone is read-only after compile.  Would
require:
1. Reserve scratch bytes in each IC-HIT emit for the patchable
   classIndex immediate.
2. At IC-fill time, patch the bytes via mprotect→write→mprotect.
3. Handle class changes: re-emit on class update.

Substantial work but high-leverage.

### Step 6 — Eden bump-allocate inline (6-8 days)

Replace the `jit_rt_basic_new_with_arg` helper call (current
basicNew: emit) with inline bump-allocate code:

    ldr edenFree, [memory, OFF_EDEN_FREE]
    add newOop, edenFree, #totalSize
    ldr edenEnd, [memory, OFF_NEW_SPACE_END]
    cmp newOop, edenEnd
    b.hi bail_to_helper        ; eden full
    str newOop, [memory, OFF_EDEN_FREE]
    ; init header
    mov w3, #classIndex
    ; ... format + slotCount fields ...
    str w3, [edenFree]
    ; nil-init slots (loop unrolled for small N)
    ...
    ; tag and push as new oop
    orr resultOop, edenFree, #0  ; tag-0 object
    stur resultOop, [sp, #-rcvr]

~25 instructions vs the helper's full C round-trip (~150 ns).
For 100K alloc bench: saves ~50µs (100K * 0.5ns) — not huge
unless allocations are in tight loops.

Edge cases: Eden full → bail to old space allocator; large
objects (≥255 slots) → bail to old space directly; weak/byte
classes need branched paths.

### Step 7 — Inline SmI float (4-5 days, blocked)

The asmjit-T1 inline emit for `0x60 + 0x61` (SmI/SmallFloat
arith) EXISTS at line 2755+ and is correct.  The issue: the
floatSum bench's hot loop is inside a block invoked from
`Array>>do:`, and the bench's outer `runFloatSum` method
calls do: only once → never JIT-compiled by default.

Fix needs either:
- Compile `runFloatSum` via Step 13's hot-loop detection
  (doesn't trigger — no backward jump in runFloatSum's body).
- Selective hot-block detection (Step 13b — attempted and
  reverted because cold blocks paid compile cost).

The proper solution: when a JIT-compiled method's IC at a
`value:` send fires N times against the same compiledBlock,
mark that block as hot and compile it.  ~4-5 days.

### Step 8 — byte-iter via Sista (3-4 days)

asmjit-T1 has `tryByteAt` (fmt 16-23, line 4196+).  Sista IR
lacks a `kPrimByteAt` op.  Adding it + lowering would let
Sista emit inline byte reads for stringHash-style workloads.

Stringhash bench: 169 ms, gap to Cog ~80×.  Most of that gap
is the byte iteration loop running in interp.

### Steps 9-10 — Block-value spec for collect/select/sort (7-10 days)

When IC stabilises on a single compiledBlock at a `value:`/
`value:value:` send site, inline-BLR to the block's JIT entry
instead of going through the BLOCK_VALUE_BIT helper.

Currently each iteration of `arr do: [:e | ...]` pays the
helper overhead (~150ns).  For 1M-iter loops that's 150 ms
of pure helper time — explains the 12-80× gaps on collect /
select / sort.

Blocks have outerContext capture that must be set up
correctly.  Easy to corrupt.

### Step 14 — Default-on Sista flags (waits for Steps 1-3)

Once Steps 2 and 3 produce useful Sista compiles AND the
n-arg path is fixed, flip:
- `PHARO_T1_INLINE_SISTA_CALL=1` default-on.
- `PHARO_SISTA_REKEY_AFTER_GC=1` default-on.

Currently both are opt-in pending the underlying paths working.

### Step 15 — Final measurement + per-bench cleanup (5-7 days)

Re-run bench-suite with all defaults flipped.  For benches
still > 2× Cog, do per-bench profiling and add specific
optimisations.

## New issues discovered during jit-may22b session

### Issue 1: bench-suite noise floor is ~3-5%

Single A/B comparisons routinely show 5-10% deltas on the
bench-suite that disappear on re-run.  Earlier session work
claimed "10-15% across 10+ benches" from multi-slot inline,
which turned out to be a noise artifact (multi-slot only
fires 672× per run; mathematically can't produce 100+ ms
savings).

**Going forward**: always do 3-run A/B/A measurement before
claiming a win.  Single-run deltas under 5% are noise.

### Issue 2: returnsLiteral was wired but unreached

The `tmi.returnsLiteral` detection works at the BYTECODE
level (detects `^ true`, `^ nil`, etc.).  But Pharo's actual
predicate methods use the quick-prim shortcut (`<primitive:
257>` for `^ true`).  Those prims fell through the IC
patching without setting bit 58.

Fixed in `992a5669` — quick prims 257-263 now map to bit 58
explicitly.  Watch for similar "wired but unreached" patterns
in other inline emits.

### Issue 3: Step 2's n-arg state corruption (UNRESOLVED)

`jitT1SistaDispatch` with nArgs > 0 corrupts something after
~64K calls.  Manifests as a downstream JIT method reading
[x1] where x1 is a stale receiver.

Bisected to "n-arg path", "no local temps" still triggers,
even simple #max: fails.  Means the bug is in arg handling
specifically.  Next debug step: run under lldb with a
breakpoint at the crashing instruction, single-step backward
through the state setup to find the corruption.

### Issue 5 (NEW 2026-05-22): activateBlock JIT dispatch attempt

Tried adding `tryJITActivation` at the end of `activateBlock`
(after `pushFrame` + closure_ + homeFrameDepth setup) to
unblock Issue 4's counter family.  bench-correctness 0/5 PASS
after the change — block-specific state (closure_, etc.)
conflicts with JIT's frame expectations.

Reverted (`20a3c149`).  Proper fix needs either:
1. Reorder activateBlock to call tryJITActivation BEFORE the
   block-specific setup, and have JIT handle that setup too.
2. Inline-BLR from value: send site (the "real" Step 9-10
   block-value spec).

Path 2 is the canonical fix per the original plan.  Multi-day.

### Issue 4: counters that fire ZERO (INVESTIGATED 2026-05-22)

Inline-prim stat dump shows several inline paths that never
fire on Pharo bench workloads:
- `bcFloat` (bytecode-level SmallFloat arith)
- `bcArithBail` (counter when SmI arith bails)
- `bitOp` (bitwise prim ops 11/12/13/19)
- `floatOp` (SmallFloat prim 21/22/23)

**Investigation conclusion**: all four counters share the SAME
underlying cause, NOT separate wired-but-unreached patterns.
They live in asmjit-T1 inline emits for specific bytecodes
(0x60/0x61 SmI arith at line 2705+, primFloatOp at line 4530+).
These emits fire only when the JIT-compiled method body
**executes** the bytecode.

For bench-suite workloads, the hot 0x60 / float-arith sends
live in BLOCKS (e.g., floatSum's `[:e | s := s + e]`).  Blocks
RUN VIA INTERP — `activateBlock` doesn't call
`tryJITActivation`, so even when blocks compile via Step 13's
threshold-of-2, the JIT body never executes.  interp's step()
handles their bytecodes.

This is **different** from the retLit unblock, which fired
from JIT-compiled callers via the IC HIT machinery.  Those
methods (`Object>>isInteger` etc.) were dispatched FROM JIT
methods that DID compile and ran their IC HIT paths.

**Fix**: Step 9-10 (block-value spec) — multi-day.  Either:
- Add JIT entry to activateBlock (mirror activateMethod's
  pattern), OR
- Inline-BLR to block's JIT entry from the value: send site
  in JIT-compiled callers, skipping primitive 207's
  activateBlock entirely.

The latter is the proper Step 9-10 implementation.  ~7-10
days of work.

## Recommended priority order for next session

1. **Investigate the zero counters** (Issue 4) — short
   investigations (a few hours each), potentially several
   small wins.  Pattern: emit exists, dispatch may exist,
   but IC encoding doesn't reach those bits.

2. **Step 5 (per-site class-immediate IC)** — Cog's biggest
   edge.  Multi-day but bounded.

3. **Step 8 (byte-iter via Sista)** — stringHash is 85× Cog.
   Concrete bench win.

4. **Steps 9-10 (block-value spec)** — collect/select/sort
   are 12-80× Cog.  Multi-day but well-defined.

5. **Step 2 n-arg debug** (Issue 3) — unlocks the BLR emit's
   full potential.

6. **Step 6 (Eden bump-allocate)** — alloc bench is the
   easiest win to verify.

Sista-side work (Steps 3, 7, 12-Sista) needs the n-arg path
fixed first because Sista-compiled fns route through
jitT1SistaDispatch from asmjit-T1.

## What's NOT worth doing

- **Step 4 (IC poly walk)** — measured 1-3% slowdown.  Keep
  default-off.

- **Step 11 (forwarder collapse)** — fires zero on Pharo.
  Cascade-handling would need multi-method IC encoding.

- **Step 13 (hot-loop threshold)** — runOnce-style harness
  methods don't have backward jumps in their bodies; the
  loops are in callees that already compile.

- **Sista bail-only diagnostic mode** — pre-existing issues
  beyond this plan's scope.

## Bench targets after each step lands

Cumulative target deltas (rough estimates from the plan):

    after Step 5:   instVar/getter benches -5-10× (immediate IC).
    after Step 6:   100K alloc 12→4 ms.
    after Step 8:   stringHash 169→30 ms.
    after Step 9:   collect 510→100 ms, select 643→80 ms.
    after Step 2 working: fib/factorial -2× (real Sista wins).
    after Step 3:   fib 179→30 ms (true self-rec inline).

Total realistic projection: ~6 weeks of focused work delivers
the plan's "all benches ≤ 3× Cog" cumulative target for the
2-12× range.  The 40-130× benches (sum, instVar, floatSum)
require Steps 5 + 6 + 8 + 9 to compound.
