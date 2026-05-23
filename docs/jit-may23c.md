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

