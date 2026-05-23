# jit-may23c — plan to beat Cog on bench-suite

## State at start (2026-05-23 end of session)

Bench-suite: **1.6 seconds total**, down from 3.7s at start of
the jit-may23 work.  Specific benches vs Cog estimates:

    bench                 ours      cog est    gap
    fib(28)               109 ms      3 ms      36×
    sieve x100              8 ms     10 ms     0.8×   ← already faster
    sort 100K             318 ms     17 ms      19×
    dict 50K              305 ms     13 ms      23×
    sum 1M                100 ms      3 ms      33×
    factorial 5K           23 ms      2 ms      11×
    instVar 1M            103 ms      2 ms      52×
    100K alloc              5 ms      3 ms     1.7×   ← close
    floatSum 1M           118 ms      8 ms      15×
    stringHash 100K       104 ms      2 ms      52×
    collect 10x100K       243 ms     63 ms      4×
    select 10x100K        278 ms      8 ms      35×
    block 1M                1 ms      3 ms     0.3×   ← already faster

To get UNDER Cog overall, we need bench-suite total under ~150 ms.
That requires roughly a 10× speedup on the prim-heavy benches.

## Where the time goes (measured via Q11-Q12, R71 instrumentation)

Of the current 1.6s bench-suite:
- ~1.3 s: fullGC (13 invocations × ~100 ms each).  Mark phase
  dominates each fullGC at ~60 ms.
- ~0.5 s: block invocation (primitiveFullClosureValue + activateBlock,
  5.2M calls × ~94 cycles).
- ~0.3 s: residual (JIT execution, other).

(Numbers overlap — GC runs between bench iterations, not during.)

## Why F1-F5 from jit-may23b stalled

Each "F" task in jit-may23b had an estimated effort of 1-2 weeks
because of a real engineering dependency, not because the
generational GC change itself is hard.  The dependencies:

- F1 (generational GC) needs scavenge to update JIT-baked Oops.
  Without this, copying eden→survivor leaves stale pointers in
  asmjit-emitted machine code.  Bisected to PASS=0/5 at startup
  every time.
- F2 (parallel mark) needs threading infrastructure + race-safe
  mark visit.
- F3 (block-value direct dispatch) infrastructure is already in
  tree behind PHARO_T1_INLINE_BLOCK_VALUE, but the chain-break
  protocol has a wrong-result bug for N≥17 that hasn't been
  root-caused yet.
- F4 (Eden bump-allocate inline) needs full asmjit-emitted alloc
  including header setup + slot-zero.
- F5 (more inline emits) is incremental — R80-R84 landed this
  session but each gains <1 ms on bench-suite.

## Realistic queue (session-sized tasks)

Each task: **WHAT** / **HOW** / **DONE WHEN**.

### W1 — Inventory JIT-baked Oops in asmjit-T1

WHAT: list every `Imm((uint64_t)oop)` and `Imm(...Bits)` site in
src/vm/jit/asmjit/AsmjitT1.cpp + the Sista + Tier2 lowering files.
For each: is the baked value perm-stable (nilBits), an old-space
literal (classOop in compile-time literal), or potentially a
young-space oop?

HOW: grep, audit each site, classify in this doc.

DONE: classification table here.  Each Imm site labelled
{PERM/OLD/YOUNG/SMI/CODE-ADDR}.

### W2 — Add per-JITMethod "baked-Oop-locations" list

WHAT: extend JITMethod to track byte offsets within its code
where Oop immediates were baked.  Required so scavenge can find
and update them.

HOW: in each emit site that bakes an Oop (from W1 classification),
record the offset.  Add `std::vector<uint32_t> bakedOopOffsets`
to JITMethod or a side-channel map.

DONE: a JITMethod has a populated `bakedOopOffsets` for every
Oop-typed Imm baked into its code.  Verified: a print walking
this list at JIT-method allocation shows N entries matching the
grep from W1 for that method.

### W3 — Implement scavenge JIT-Oop update

WHAT: scavenge's forEachMemoryRoot already gets called.  Add a
new visitor pass that walks every JITMethod and, for each
bakedOopOffset, reads the 8 bytes from the JIT code page,
calls visitor (which forwards via the forward map), and writes
the new bits back.

HOW: requires W2.  Read+write is in MAP_JIT pages (W^X); use
jit::makeWritable/Executable around the rewrites (same pattern as
updatePointersAfterCompact in fullGC, see Interpreter.hpp:3159).

DONE: 5/5 PASS on bench-correctness with the rewrites enabled.

### W4 — Two-half survivor management

WHAT: split survivor space into S0/S1 halves; scavenge flips
which is "from" vs "to".  Eden objects copy to "to" → become
age 1.  "From" objects (age 1, survived last scavenge) tenure
to old space.

HOW: requires W3.  Mirror my failed attempts (jit-may23b R77/R79)
but now JIT-Oop updates work, so corruption goes away.  All ~13
"is in eden" check sites need to also include "is in from".

DONE: 5/5 PASS on bench-correctness.  Bench-suite shows fullGC
count drops from 13 → ~3-5 per run.  Mark phase total drops
proportionally.

### W5 — Measure W4's actual win

WHAT: bench-suite 3-run median before W4 vs after.

DONE: numbers recorded.  Expected: -300 to -500 ms on bench-suite.

### W6 — Inventory chain-break protocol callers

WHAT: F3 (block-value direct dispatch) infrastructure in tree
gated by PHARO_T1_INLINE_BLOCK_VALUE has a chain-break protocol
bug for N≥17.  Identify all sites participating in the chain-break
protocol.

HOW: grep for chain-break / chain-loop bail sites in AsmjitT1.cpp
and Interpreter.cpp.  Document interactions with j2jPool /
materialize / saved frames.

DONE: list of chain-break participants documented here.

### W7 — Reproduce the F3 N≥17 bug

WHAT: write a focused fib(20) bench under
PHARO_T1_INLINE_BLOCK_VALUE=1, confirm wrong result.  Bisect via
PHARO_T1_INLINE_BLOCK_VALUE_NONLEAF or similar to narrow.

HOW: prepared bench script + lldb attach if needed.

DONE: a single deterministic failure case identified.

### W8 — Fix the F3 bug

WHAT: based on W7's diagnosis, apply the fix.  May involve
extending the materialize path, fixing return-value placement,
or adding a save/restore the protocol missed.

DONE: PHARO_T1_INLINE_BLOCK_VALUE=1 PASSes bench-correctness
5/5, plus the fib(20) reproducer.

### W9 — Default-on F3

WHAT: flip the default to PHARO_T1_INLINE_BLOCK_VALUE=1
unconditionally.  Verify bench-suite gains the expected -300 ms.

DONE: bench-suite shows -200 to -300 ms reduction from F3.

### W10 — Identify Sista compile bail reasons

WHAT: Sista (tier-2) compiles fewer methods than expected.
Many methods that COULD be Sista'd bail at "hasSend && !hasSplice".
Survey the bail reasons.

HOW: add per-reason bail counter to SistaRuntime.

DONE: top 5 bail reasons + counts recorded.

### W11 — Lift one Sista bail constraint

WHAT: pick the most-common Sista bail from W10 that has a
tractable fix.  Probably: extending splice detection to cover
one more loop pattern.

HOW: read SistaBuilder.cpp's lift logic; add the missing pattern.

DONE: Sista compile count increases by ≥100 methods on
bench-suite; bench-correctness 5/5 PASS.

### W12 — Eden bump-allocate inline in JIT body

WHAT: for basicNew at IC HIT, inline the alloc directly in JIT
emit instead of calling jit_rt_basic_new helper.  Saves the
C++ call overhead per allocation.

HOW: in tryPrimBasicNewZero (and BasicNew with arg), emit:
  - load class oop from receiver slot
  - load class slot 2 (instSpec)
  - extract instSize + format
  - bump-pointer alloc from edenFree_ (or oldSpaceFree_ if no W4)
  - write header (slotCount + classIndex + format)
  - zero slots to nil
  - store result Oop at receiver slot

DONE: bench-correctness 5/5 PASS.  Counter shows the inline path
fires.  100K alloc bench should drop further (currently 5 ms,
target ≤3 ms).

### W13 — Re-measure full bench-suite vs Cog

WHAT: with W4+W9+W12 done, what's the gap to Cog?

DONE: table comparing all benches vs Cog estimates.

### W14 — Identify next 10× gap

WHAT: from W13, pick the bench with the biggest remaining gap
and identify the structural reason.

DONE: characterization here.

### W15 — Address W14's structural issue

WHAT: depends on W14.

DONE: bench improves measurably.

## Acceptance criteria for "faster than Cog"

The bench-suite total ours/cog ratio < 1.0 across all 13 benches,
OR within 5% of Cog on each individual bench.  Realistic since
we're already faster on sieve and block 1M.

## Estimated effort (honest)

- W1-W5 (generational GC done right): **2-4 days** of focused work.
- W6-W9 (F3 block-value): **2-3 days**.
- W10-W11 (Sista coverage): **1-2 days**.
- W12 (Eden inline): **1-2 days**.
- W13-W15 (whatever's next): unknown until we get there.

Total: **6-11 days** of focused engineering to beat Cog.  This
is honest — far from "1-2 weeks per F" because we now know the
real prerequisites and the prerequisites are bounded.

The session-sized tasks (W1-W15) are individually 30 min – 2 h
each.  None requires multi-day grinding.  The doc avoids the
trap from earlier docs where I wrote "1-2 weeks" then refused to
execute.

## What this doc does NOT do

- It doesn't promise wins from F5-style "add inline emit for prim
  X".  Those are <1 ms each on bench-suite.
- It doesn't promise success — W3 (JIT-Oop scavenge update) is
  the make-or-break task.  If that fails, W4-W5 don't matter.
- It doesn't reserve "session boundaries" as an excuse.  W3
  itself is grind-through-N-sites; you just do them.
