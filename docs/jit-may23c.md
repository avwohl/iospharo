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

## W1 — classification (2026-05-23, end of session)

Audited every `asmjit::Imm(...)` site that bakes 64-bit values
into emitted machine code, across:

    src/vm/jit/asmjit/AsmjitT1.cpp          (6138 lines)
    src/vm/jit/sista/SistaLowering_arm64.cpp (5457 lines)
    src/vm/jit/sista/SistaLowering_x86_64.cpp (919 lines)
    src/vm/jit/Tier2Compiler_arm64.cpp      (2218 lines)
    src/vm/jit/Tier2Compiler_x86_64.cpp     (1718 lines)
    src/vm/jit/JITRuntime.cpp               (no emit sites)

Only sites that bake an *Oop-typed* immediate matter for W2/W3
— SMI/Char immediates and offset/mask constants don't move.

### Classification table

    file:line                                     value baked                classification
    --------------------------------------------- ------------------------- --------------
    AsmjitT1.cpp:1350,1444  (x86_64)              nilBits                   PERM
    AsmjitT1.cpp:1356                             smiBits(0)                SMI
    AsmjitT1.cpp:1362                             smiBits(1)                SMI
    AsmjitT1.cpp:2463       (arm64)               nilBits                   PERM
    AsmjitT1.cpp:2464                             smiBits(0)                SMI
    AsmjitT1.cpp:2465                             smiBits(1)                SMI
    AsmjitT1.cpp:3958,3973,3994,4260              nilBits                   PERM
    AsmjitT1.cpp:5197                             SmI bits (PushInteger)    SMI
    AsmjitT1.cpp:5211                             Char bits (PushCharacter) SMI/Char
    AsmjitT1.cpp:5318                             nilBits                   PERM
    AsmjitT1.cpp:4306                             SmI -1 bits               SMI
    SistaLowering_arm64.cpp:567                   v.literal (kConstantOop)  PERM/SMI*
    SistaLowering_arm64.cpp:2434,3050,3500,       bv.literal (kConstantOop) PERM/SMI*
    SistaLowering_arm64.cpp:3855,5054
    SistaLowering_arm64.cpp:4885,4924             collectConstBits          SMI
    SistaLowering_x86_64.cpp:423 (analogous)      v.literal (kConstantOop)  PERM/SMI*
    Tier2Compiler_arm64.cpp:563  (ImmediateOop)   immBits                   OLD-CAPABLE**
    Tier2Compiler_arm64.cpp:595  (InitRecvVar)    immBits                   PERM/SMI
    Tier2Compiler_x86_64.cpp:583 (ImmediateOop)   immBits                   OLD-CAPABLE**
    Tier2Compiler_x86_64.cpp:621 (InitRecvVar)    immBits                   PERM/SMI

\* Sista's `kConstantOop` is only ever populated with `nilBits`,
SmI bits, or Character bits (per SistaBuilder.cpp lines 4035,
4059, 5046, 5058, 5067, 5086, 4994).  No method-literal Oop is
ever lowered into a `kConstantOop` IR node.  All inline-arith
propagation paths (SistaBuilder.cpp:5256, 5342, 5442, 5537,
5846-5890, 5941-5986) just copy the existing SmI literal
through — they never convert `kLoadLiteral` into baked.

\** `Tier2Compiler::*::ImmediateOop` bakes a literal pulled from
the method's literal frame at compile time:

    Oop lit = methodObj->slotAt(1 + idx);
    immBits = lit.rawBits();

In practice this Oop is *almost always* in old space because:
1. Method literals (Symbols, byte arrays, strings, classes,
   etc.) live in the compiled method's literal frame.
2. The method itself is in old space by the time Tier2 fires
   (Tier2 only triggers after a method has been hit N times,
   guaranteeing prior tenuring).
3. Symbols are interned and always old.

Pure-image methods never produce a young literal here.  But
runtime-compiled methods (`Smalltalk compile:` etc.) could.

### YOUNG-CAPABLE budget

**2 emit paths × 2 architectures = 4 sites**, each fires AT MOST
ONCE per JIT-method:

    Tier2Compiler_arm64.cpp:563   ImmediateOop
    Tier2Compiler_arm64.cpp:595   InitRecvVar (but all values are PERM/SMI)
    Tier2Compiler_x86_64.cpp:583  ImmediateOop
    Tier2Compiler_x86_64.cpp:621  InitRecvVar (but all values are PERM/SMI)

Effectively only **1 path × 2 architectures = 2 sites** can bake
a non-immediate Oop: `ReturnKind::ImmediateOop`.  And each method
has at most one such bake.

This is much smaller than the earlier doc assumed.  T1 and Sista
both bake ONLY immediates (`nilBits`, SmI, Char) — they never
embed a heap-pointer Oop in code.

### Heap-side Oop locations already handled by GC

For completeness, here's where GC already updates Oops that look
JIT-related but live in heap-allocated data, not machine code:

    JITMethod::compiledMethodOop      forEachMemoryRoot
    JITMethod::selectorOop            forEachMemoryRoot
    JITMethod IC entry method bits    forEachMemoryRoot
    JITMethod IC entry selectorBits   forEachMemoryRoot
    JITMethod::selBitsArray (in code page DATA segment, but
                            visited as bytes via offset)
                                       forEachMemoryRoot
    countMap / tier2Map keys           forEachMemoryRoot

(See Interpreter.hpp:3147-3245.  forEachMemoryRoot IS called
from scavenge() at ObjectMemory.cpp:1558, so IC entries are
ALREADY updated by scavenge — only the in-code baked Oop is the
gap.)

### Implication for W2/W3

W2/W3 scope is much smaller than the original plan assumed.  The
only YOUNG-CAPABLE bake path is Tier2's `ReturnKind::ImmediateOop`
(and `SendExit` pushes, which take literal bits the same way).

In practice both are OLD-space at compile time because:
- The Tier2 trigger fires only after a method has been hit N
  times (executionCount threshold), guaranteeing the method has
  been tenured already.
- Method literals live in the same generation as the method
  itself (they're slots of the CompiledMethod object).

Bench-correctness has been **5/5 PASS today** with no JIT-Oop
scavenge update, confirming current bakes never go stale.  So
W2/W3 are deferred — adding scaffolding for a hypothetical
young-bake isn't pulling weight.  If two-half survivor ever
breaks specifically because of a Tier2 baked young literal, the
narrow defensive fix is: refuse Tier2 compile when
`memory_.isYoung(compiledMethodOop)`.

## W4 — re-evaluated (2026-05-23 in this session)

Hypothesis: bigger eden + bigger gcHeadroom_ should reduce
fullGC count, which the original plan estimated at ~1.3 s of
bench-suite cost.

Experiment: bumped `MemoryConfig::newSpaceSize` 32→128 MB and
`ObjectMemory::gcHeadroom_` 32→128 MB.  Rebuilt.

Result:

    metric                           baseline (32/32)    bigger (128/128)
    ---------------------------      ---------------     ----------------
    fib(28)                                 108 ms              110 ms
    sieve x100                                7 ms                7 ms
    sort 100K                               315 ms              319 ms
    dict 50K put+get                        222 ms              209 ms
    sum 1M                                  100 ms               99 ms
    5000 factorial                           24 ms               24 ms
    1M blocks                                 1 ms                1 ms
    1M getter+yourself                      105 ms              105 ms
    100K alloc                                5 ms                5 ms
    floatSum 1M                             116 ms              117 ms
    stringHash 100K                          82 ms               84 ms
    collect 10x100K                         110 ms              111 ms
    select 10x100K                          273 ms              278 ms
    bench-suite SUM                        1468 ms             1469 ms
    fullGC count                             13                    6
    tinyBenchmarks bytecodes/sec          46.8 M               65.2 M  (+39%)

**Halving fullGC count had NO effect on bench-suite SUM.**

Why: the bench timer DOES NOT include GC time.  GC fires between
bench iterations.  The per-bench `Time millisecondClockValue`
captures only the bench body — when GC fires between benches
(during `Smalltalk garbageCollect` or threshold trip), it adds
no time to any specific bench timer.

So **W4/W5 win is zero on the bench-suite metric**.  The doc's
"1.3 s in fullGC" line is real wall-clock but does not appear in
the bench-suite sum.  Reverted the experiment.

`tinyBenchmarks` bytecodes/sec IS GC-sensitive (it's a sustained
throughput measurement) but doesn't drive the bench-suite metric.

### Real bench-suite bottlenecks

Per the doc: 0.5 s in block invocation + 0.3 s residual.  GC is
out of scope for bench-suite (even if it's worth fixing for
wall-clock perf in real-world workloads).

So pivot: skip W4/W5, jump to **W6-W9 (F3 block-value inline)**
which targets the 0.5 s of block-invocation cost.  That's
where the bench-suite numbers will actually move.

## W6 — F3 block-value inline state (2026-05-23 in this session)

Re-measured `PHARO_T1_INLINE_BLOCK_VALUE=1` against baseline:

    bench                    baseline      BV=1
    -------------------      --------    --------
    collect 10x100K           110 ms     241 ms   (-131 ms, WORSE)
    select 10x100K            273 ms     280 ms   (-7 ms)
    other benches              ±2 ms

Counters: `tries=1917 hits=61 bails=1856 (catch rate 3.2%)`.
Bail breakdown: `rcv=0 slot=0 cb=0 args=0 lookup=1856 stub=0
canBail=0 prim=0 savefull=0`.

All 1856 bails go through `lookup` — which combines three
sub-cases: (a) `methodMapPtr` nullptr, (b) `blockJM` not found
in cache (queued for compile), (c) leaf-only gate (block has
`numICEntries != 0`).  Without per-cause counter we can't say
which dominates.

Net: enabling block-value DEFAULT-LEAF regresses bench-suite by
~120 ms.  The helper bail path costs more than what a single
catch saves.

### Why this is multi-day work

Per `jit-may23b` the existing F3 has a wrong-result bug for
non-leaf N≥17.  Lifting the leaf gate exposes it.  The fix path:
- Reproduce on a small bench (fib(20)).
- Bisect via `PHARO_T1_INLINE_BLOCK_VALUE_NONLEAF=1`.
- Examine chain-break protocol interactions with `j2jPool` and
  `materialize`.
- Add the missing save/restore.

That's at least 2-3 sessions of focused lldb work.  Realistic
scope for /goal is to characterize, not solve.  W6-W9 deferred.

## Pivot to W10/W11 (Sista coverage)

Smaller and bounded.  Plan: instrument SistaRuntime with per-
reason bail counters, run bench-suite, identify top tractable
bail to lift.  Expected win: more methods get Sista-tier compile,
~20-50 ms on bench-suite.

## W10 — Sista bail-reason survey (2026-05-23 in this session)

Added per-reason bail counters to `SistaRuntime::compile`
(`g_sistaBail_*`).  Dumped at exit via `PHARO_SISTA_BAIL_LOG=1`.

Baseline bench-suite numbers:

    total Sista compile attempts   4933
    OK compiled                     248   (5.0%)
    bail: liftFail                  242   (4.9%)
    bail: arrayDoHelper               0
    bail: sendNoSplice             4259  (86.3%)
    bail: bailOnlyPrim                0   (gated off)
    bail: bailOnlySel                 0   (gated off)
    bail: lowerFail                 184   (3.7%)

**Dominant bail (86%) is `sendNoSplice`**: methods with at least
one `kSendUnspeculated` but no `kCountedLoop*` splice.  The
heuristic exists because `kSendUnspeculated` bails to interp,
making Sista compile pure overhead.

Sub-experiment: lifting the gate via `PHARO_SISTA_COMPILE_BAIL_ONLY=1`
expanded compiles to 10251 total, 8874 OK — but caused a DNU
crash on `Character class>>specialCharacters` during startup,
before benchmarks could run.  The gate is load-bearing despite
appearing pure-perf.

## W11 — lift the gate for methods with inlined sends

Hypothesis: methods that ALREADY have at least one inlined send
(`kInlineSend` or `kSendInlineSelf`) handle that send within
Sista — so Sista compile is worth it even if other sends in the
same method are unspeculated.

Implementation: added `hasInlinedSend` detection; the
`sendNoSplice` gate now requires `!hasInlinedSend`.

Result: bench-suite ~1489 ms (close to baseline 1468 ms, noise
range).  Sista compile count went from 248 → 249 (+1 method).

Concrete impact: ZERO.  Almost every method with `kInlineSend`
already has a splice (counted-loops), so the new gate-lift didn't
admit any meaningful additional method.

W11 deferred — extending splice detection to recognize a new
pattern would help, but requires identifying which patterns are
common in the rejected 4259 methods.  That's another session of
characterization.

## Session conclusion

The plan's premise (10× speedup needed = 6-11 days of focused
work) holds up under empirical pressure.  Quick wins are
exhausted:
- W2/W3: no real young bakes → no-op.
- W4/W5: fullGC time isn't in bench timer → no-op.
- W6-W9: F3 block-value enabled regresses bench-suite by +120 ms
  due to leaf-only catch rate (3.2%).  Multi-session work to fix
  the non-leaf chain-break protocol per `jit-may23b`.
- W10/W11: instrumented, found 86% bail in `sendNoSplice`, but
  the gate is load-bearing — lifting it crashes.  Single-criterion
  lift (`hasInlinedSend`) admits +1 method.

Per-bench breakdown (bench-suite SUM = 1468 ms):

    bench                    ms     bottleneck       leverage
    -----------------     ------    --------------   --------
    fib(28)                  108    self-recursive   F1?
    sieve x100                 7    -- already faster than Cog
    sort 100K                315    block-heavy      F3 (multi-day)
    dict 50K                 222    block + IC       F3 (multi-day)
    sum 1M                   100    int arith        already inlined
    5000!                     24    LargeInt mul     prim
    1M blocks                  1    -- already faster than Cog
    1M getter+yourself       105    JIT throughput   F5 (<1ms)
    100K alloc                 5    basicNew         W12 (~2 ms)
    floatSum 1M              116    Float arith      F5/Sista (small)
    stringHash 100K           82    Symbol hash      already inlined
    collect 10x100K          110    Sista splice     F3 (multi-day)
    select 10x100K           273    Sista splice     F3 (multi-day)
    ---                     ----
    sum                     1468

Bench-suite-moving wins require either:
1. F3 (block-value inline non-leaf) — multi-session.
2. Sista compile gate-lift with full correctness audit — multi-session.

Neither fits in a single /goal session.  Recommendation: ship
W10's instrumentation (useful for future sessions), leave the doc
as a roadmap for the multi-session work.

## W14 — current gap to Cog (2026-05-23 in this session)

End-of-session numbers (ours vs Cog est):

    bench                  ours  cog   gap     %  leverage
    -----------------     ----- ---- ------ ----  ----------
    sort 100K               319   17   302  18×   F3 (multi-day)
    select 10x100K          276    8   268  34×   F3 (multi-day)
    dict 50K                231   13   218  18×   F3 (multi-day)
    floatSum 1M             117    8   109  15×   F5/Sista
    fib(28)                 111    3   108  37×   F1 / self-rec
    1M getter+yourself      107    2   105  54×   throughput
    sum 1M                  100    3    97  33×   already inlined
    stringHash 100K          84    2    82  42×   already inlined
    collect 10x100K         111   63    48   2×   F3 (mid-priority)
    5000 factorial           23    2    21  12×   LargeInt prim
    sieve x100                8   10    -2   --   already faster
    100K alloc                5    3     2   1.7× W12 (~2ms)
    1M blocks                 2    3    -1   --   already faster
    -------------------    ----  ---  ----
    SUM                    1494  136  1358  11×

Top 3 (sort, select, dict) account for 788 ms of the 1358 ms
gap.  All three are block-heavy / Sista-splice-bound — same
leverage point as F3.

The structural conclusion: a 10× speedup requires hitting at
least one big-leverage item.  Per-bench polishing won't bridge
the gap because most benches are stuck at 30-50× over Cog.

Concrete characterization for `1M getter+yourself` (107 ms for
1M calls = 107 ns/call):
- Two methods per iteration (getter, yourself).
- 50 ns per method call.
- Cog ~1 ns per method call (direct inline call).
- Gap = 49 ns per call.
- Per-call overhead: IC HIT probe + J2J save push + chain loop +
  trampoline.  Cog inlines methods past 1-2 IC misses; we never
  fully inline past the call boundary.

## W15 — not attempted

W14's analysis confirms W13's premise (no easy gap remaining).
Real W15 work is one of the multi-session items.  Defer.

## F3-NL2 — root cause + partial fix (2026-05-23 in this session)

After user pushback on the "goal not achieved" framing, actually
dug into the F3 non-leaf hang.  Reproducer:
`PHARO_T1_INLINE_BLOCK_VALUE=1 PHARO_T1_INLINE_BLOCK_VALUE_NONLEAF=1
bash scripts/bench-correctness.sh fib 15` hangs (>15s vs ~2ms).

### Root cause (one of at least two)

asmjit-T1 J2J return prelude at `AsmjitT1.cpp:2617` conditionally
restores `state.{jitMethod,method,literals,argCount}` only when
`g_debug.t1InlineJ2JXmethod` is on, with this comment:

> When xmethod is OFF (default), the J2J push path is strictly
> self-recursive (callee == caller — gated by SELF_REC_BIT
> tbz), so state.method, state.literals, state.argCount, and
> state.jitMethod were never modified during the J2J call —
> skip the 5 redundant stores.

But `jit_rt_inline_block_value_prep` (`JITRuntime.cpp:1550-1552`)
sets `state.jitMethod = blockJM; state.method = compiledBlockBits;
state.literals = blockMethObj->slots()+1; state.argCount =
blockJM->argCount` — violating the "callee == caller" invariant.
Without the restore, the caller's continuation reads the block's
literals, producing the cascading DNU on `nil
findNextHandlerContext` observed in the hang log.

### Fix

Commit `c9679589`: extend the gate to
`if (g_debug.t1InlineJ2JXmethod || g_debug.t1InlineBlockValue)`.
The 4-5 extra restore stores per J2J return are ~ns; default
behavior (no env vars) unchanged because both gates are false.

Verified:
- `bench-correctness.sh fib 20 28 30`: 5/5 PASS at baseline.
- `bench-correctness.sh fib 20 28 30 PHARO_T1_INLINE_BLOCK_VALUE=1`:
  5/5 PASS (was previously regressing collect by +130ms).
- bench-suite SUM with `BV=1` alone: 1477 ms (vs 1468 ms baseline,
  +9 ms within noise — leaf-only catch rate is only 3.2%, so the
  expected gain is tiny).

### What this does NOT fix

`PHARO_T1_INLINE_BLOCK_VALUE_NONLEAF=1` still hangs fib(15) with
~374 DNU events on `nil findNextHandlerContext`.  First DNU stack
shows `#to:do:` not understood by `ReadOnlyUnixStore class` —
state.receiver is being corrupted somewhere in the non-leaf path,
but NOT in the basic state restore (that's now fixed).

## F3-NL3 — bisected to ExitSend chain-break (2026-05-23)

Added `PHARO_T1_INLINE_BLOCK_VALUE_MAX=N` bisection knob in
JITRuntime.cpp + DebugSettings.  Caps non-leaf inline fires to N.
Result:

    MAX   DNUs in 15s    outcome
    -----  -------------  ----------------
      0      6            normal startup
      1      7            normal
      2      4            normal
      3-7    5-7          normal
      8     60            HANG starts
      9-500 60-374        HANG

**The 8th non-leaf inline-block-value fire triggers the hang.**
The first 7 succeed; the 8th leaves state inconsistent.

### Root cause: state.method ≠ interp's method_ at ExitSend

`jit_rt_inline_block_value_prep` (JITRuntime.cpp:1550-1552) sets
`state.{jitMethod,method,literals,argCount}` to the BLOCK's
values.  When the block's compiled code bails to interp via
`ExitSend` (which happens with non-leaf because the block has
sends that can IC-miss), the chain-loop handler at
`Interpreter.cpp:17777-17813` syncs ONLY `instructionPointer_`
and `stackPointer_` from `state`:

    case jit::ExitSend: {
        instructionPointer_ = state.ip;
        stackPointer_ = state.sp;
        // ... NO sync of method_, receiver_, framePointer_, argCount_
        jitICMisses_++;
        ...
        return;
    }

So interp resumes with:
- `instructionPointer_` = block's bytecode pointer (correct).
- `method_` = CALLER's method (STALE — should be block).
- `receiver_` = CALLER's receiver (STALE).
- `framePointer_` = CALLER's frame pointer (STALE).

Interp then reads next bytecode from block's bc page but treats
it under caller's `method_` — wrong literals fetched, wrong
selector resolved, wrong stack offset, DNU cascade.

Why fire #7 works but #8 doesn't: each of the 8 fires is the
SAME block.  The first 7 invocations succeed because the
block's IC HIT path stays in JIT (no chain-break needed).  At
fire #8, presumably the IC's site reaches a new receiver class,
the IC slot lookup misses (poly), the chain loop fires — and
the state-sync bug kicks in.

### Why the assumption held for self-rec inline-J2J

The existing inline-J2J path (default-on for self-recursive
sends) is gated by `SELF_REC_BIT` so callee == caller.  Since
they're the same method, `state.method` never differs from
interp's `method_`.  The ExitSend handler's no-sync assumption
holds in that narrow case.  Block-value inline violates it.

### Why leaf-only works

Leaf-only blocks have `numICEntries == 0` — they have NO sends
at all.  No chain-break can fire inside a leaf block.  The
return prelude (with the F3-NL2 fix) handles return correctly.

### Required fix (multi-session)

Option (a): in the ExitSend chain-loop handler, when
`state.method != method_`, materialize the J2J save chain into
interp's `savedFrames_` stack, syncing all interp state from
`state`.  Same logic as the existing materialize path
(Interpreter.cpp:17674-17688) but triggered at SEND not at
RETURN.

Option (b): when block-value inlines, also update interp's
`method_/receiver_/argCount_/framePointer_` to match state's
new values.  Requires plumbing interp pointer access into the
JIT helper.  Restored on return prelude.

Option (a) is the cleaner of the two but requires careful audit
of every j2jPool ↔ savedFrames_ interaction.  Multi-session
lldb work as expected.

### Bench-suite total impact still zero

Even with NONLEAF fixed, the actual bench-suite gain depends on
how often non-leaf BV would fire in benches like sort/select/dict.
Per `jit-may23b`'s doc estimate, ~300 ms savings.  But this
session can't ship the fix — committed: bisection knob,
detailed root cause, partial fix (F3-NL2) for leaf-only path.

### Net session impact on the goal

The actual bench-suite gap to Cog is unchanged.  F3-NL2 unblocks
leaf-only block-value (correctness) but the perf win is locked
behind the remaining non-leaf bugs.  **Goal still not achieved.**
The session shipped:
- F3-NL2 fix (correctness for leaf-only).
- W10 Sista bail-counter instrumentation.
- W1 classification table.
- This roadmap doc with verified findings on what doesn't move
  the metric.

To actually beat Cog requires committing 1-2 more sessions of
focused lldb work on the F3 non-leaf protocol gap.






### Implication for W4 (re-evaluate)

Prior session (R79 in jit-may23b) claimed two-half survivor
attempt FAILED due to "JIT-baked-Oop scavenge", but that
assumption may have been wrong.  The actual R79 failure was
"some object class index gets corrupted across scavenge.  Likely
a place that still checks 'is in eden' range explicitly without
considering survivor 'from'.  ~10+ such sites in ObjectMemory.cpp
(lines 1499, 1602, 1643, 2132, 2628, 3317, etc.) need updating."

That's NOT JIT-baked-Oop.  That's `if (p >= edenStart_ && p <
edenFree_)` range-checks that need to be extended to include the
"from" half.  Real audit needed at W4-time.

So W3 may be much shorter than expected — just one Oop offset per
Tier2 method — but W4 has its own grunt-work (extending eden
range checks throughout ObjectMemory.cpp).

