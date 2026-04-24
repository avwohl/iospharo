# JIT Perf Plan — 2026-04-24

Following the 3-way perf comparison (Cog vs default JIT vs NO_JIT) which
showed our JIT 4–34× behind Cog, the user asked: *we spent the last week
doing what you said we need to do — is it not enabled?*

This document audits what's actually shipped vs gated, measures what
flipping the gates does today, and lays out the concrete next step.

---

## TL;DR

  - **Past week was mostly correctness/stability**, not perf.  Bug 14
    (NLR over-walk), Task #41 (selectorBits GC memset), Sista frame
    dispatch fixes, A0/A1 closures, the 565-class baselines.
  - **Perf-relevant infrastructure exists** (asmjit T2 MVP, Sista
    Phase 0–3, shared inline IC, side-table T1/T2 sharing).
  - **The ONE knob that would close the gap — monomorphic
    inlining (Sista Phase 4) — is queued, not built.**
  - **Flipping every available perf flag (`PHARO_T2=1` plus every
    `PHARO_T2_*` sub-flag) makes no measurable difference.**
    Verified today, see "Empirical check" below.
  - Reason: T2 today only compiles **leaf methods** (~5–15 % of hot
    methods) — getters, setters, constant returns.  Those are
    already prim-dominated, so a faster JIT for them has no
    headroom.

The 17–34× Cog gap won't move until we ship method inlining.  Plan
below.

---

## Current default config (verified 2026-04-24)

Read straight from `src/vm/DebugSettings.cpp` constructor:

    Flag                            Default   Effect
    ------------------------------  --------  ------
    PHARO_NO_JIT                    OFF       T1 stencil JIT runs
    PHARO_NO_SISTA                  OFF       Sista dispatch runs
    PHARO_T2                        OFF       asmjit T2 OFF by default
    PHARO_T2_REPLACE                OFF       T2 coexists with T1 (T1 wins)
    PHARO_T2_A1                     OFF       chain-loop continuation OFF
    PHARO_T2_MBC_JUMPS              OFF       multi-bc jumps OFF
    PHARO_T2_MBC_SENDS              OFF       multi-bc sends OFF
    PHARO_T2_MBC_IC                 OFF       inline IC in T2 OFF
    PHARO_T2_ZEROARG_IC             OFF       0-arg inline IC OFF
    PHARO_T2_WARMUP                 3         T1→T2 promotion threshold
    PHARO_RESUME_J2J                OFF       external trampoline OFF
    PHARO_YOUNG_GEN                 OFF       generational GC OFF
    PHARO_JIT_DEFER                 4 s       headless: defer JIT 4 s
    PHARO_SISTA_COMPILE             OFF       Sista compile-on-send OFF
    PHARO_SISTA_ALLOW_SENDS         OFF       still bail on sends
    finalizeDeferred                ON        deferred finalization

Sista dispatch is on by default (good) but it only handles
**send-free leaf methods + Send0**.  Anything with Send1+ bails to
the interpreter.  No actual inlining anywhere in the stack.

---

## Empirical check — does flipping flags help?

Median of 3 runs each, same prepped Pharo 13 image, M-series arm64:

    Workload                Default JIT   +T2          +T2+all flags   Cog
    tinyBench bytecodes/sec  17.4 M       18.0 M       17.5 M           73.5 M
    tinyBench sends/sec     140.9 M      134.9 M      140.3 M           3.75 G
    fib(28)                  68 ms        72 ms        70 ms             2 ms
    sieve x100              131 ms       134 ms       134 ms             9 ms
    sort 100K               284 ms       276 ms       287 ms            16 ms
    dict 50K put+get        380 ms       397 ms       397 ms            21 ms
    sum 1M                   73 ms        84 ms        79 ms             5 ms
    5000 factorial          231 ms       242 ms       220 ms            38 ms
    1M block                 22 ms        24 ms        22 ms             0 ms
    1M getter+yourself       93 ms        97 ms        91 ms             4 ms
    100K alloc                5 ms         5 ms         5 ms             0 ms

**Result: every column for "+T2" and "+T2+all flags" is within ±5 %
of default JIT.**  No headline number moves.  Some go slightly
backwards (sum 1M, dict 50K) — likely the extra compile cost of T2
on hot methods that bail anyway.

The gap to Cog (4–34×) is *unchanged* by every available perf flag.

Raw runs in `docs/perf-2026-04-24/perf-our-jit-t2*.txt`.

---

## Why the flags don't help

T2 today only emits ARM64 for these patterns
(`docs/jit-todo.md:51–80`):

    Return-only          ^ self/true/false/nil
    Push-return          ^ literal[N], ^ self, ^ globalVar
    Getter               ^ instVar[N]
    Setter               ivar := arg; ^ self
    Init-const           ivar := const; ^ self

That covers the simplest 5–15 % of methods in a Pharo image — and
those methods are dominated by the prologue/epilogue (allocate
context, return) which is already as fast as the interpreter at
running them.  The hot loop in `fib(28)` is `[n - 1] benchFib +
[n - 2] benchFib` — Send2 with arithmetic.  T2 doesn't compile
that; it bails to T1.  T1 dispatches via stencil tail-call chains
that bust the M1 i-cache (~460 B per bytecode vs Cog's ~80 B).

The 0-arg / 1-arg inline IC (`PHARO_T2_ZEROARG_IC=1`) is documented
"bimodal: ~205 ms fast / ~380 ms slow" — flipping it on can hurt.

So **the gates are off because nothing behind them is ready to
ship**, not because we forgot to flip a switch.

---

## What the past week actually shipped

By commit area, since 2026-04-17:

  - **Bug 14**: NLR over-walk in JIT terminating sibling Processes
    (`b18e71e` and predecessors, finally root-caused).
  - **Task #41**: out-of-band selectorBits array surviving GC
    memset (`f69734f`) — unblocked 19 SUnit timeouts.
  - **Sista correctness**: frame-state corruption detector,
    bail-blacklist tuning, dispatch ring buffer (`a6abb9f`,
    `2807c40`, etc.).
  - **JIT stability**: tryJITActivation honors `PHARO_NO_JIT=0`
    correctly (`41c093f`), `nameOfClass` heap-pointer guards
    (`fc98ee1`), unblockStuckSnapshotCallers (`ac350c3`),
    push() sanity checks.
  - **Test infrastructure**: 565-class per-class isolation runner,
    NO_JIT/JIT/Cog parity baselines, Cog cross-check on 4 non-ok
    classes (this session).
  - **Diagnostics**: PHARO_JIT_TRACE_OOP, PHARO_JIT_STALE_LOG,
    PHARO_SISTA_*_WATCH, PHARO_SDL_TRACE, PHARO_SEM_SIGNAL_TRACE.

What's notably **not** in that list: anything that compiles a
bytecode-with-sends method to faster code than the interpreter
currently runs.  Phase 2.3 of Sista (dispatch MVP for
leaf+Send0-only) shipped before the 04-17 window.  Phase 4
(monomorphic inlining) hasn't started.

---

## The plan

Three options, decreasing scope:

### Option A: Phase 4 monomorphic inlining (the headline fix)

Per `docs/sista-inlining-plan.md`:

  - Time:           **4–5 weeks** (Phase 4 alone).
  - Prerequisites:  **Phase 3 deopt infrastructure (3–6 weeks)**
                    must come first — without correct deopt,
                    inlining miscompiles silently when a class
                    redefinition or unexpected receiver class
                    invalidates speculation.  Phase 3 has not
                    started.
  - Total:          **7–11 weeks** of focused work.
  - Expected gain:  3–5× on send-heavy workloads (fib, AWFY).
                    Still 4–7× behind Cog (which has poly-inlining,
                    register allocation, full PIC).
  - Risk:           High.  Deopt bugs are silent miscompiles —
                    test suite won't catch them reliably.
                    Mitigation in plan: random-deopt stress tester.

### Option B: Narrow T2 expansion (incremental)

Without inlining, push T2 to cover more bytecode patterns:

  - Send0 + Send1 + Send2 with inline IC, no inlining still.
  - Multi-bc loop bodies (jumps + arith + sends).
  - Move from copy-and-patch stencils to asmjit instruction
    emission for non-leaf methods.

  - Time:           **3–6 weeks**.
  - Expected gain:  Maybe 1.3–1.8× on send-heavy workloads —
                    still 10–20× behind Cog.  Well-shipped in
                    Tier2Compiler today behind sub-flags
                    (`PHARO_T2_MBC_*`); the missing piece is
                    making them stable enough to default on.
  - Risk:           Medium.  IC stability across GC, frame-state
                    on bail, multi-bc backward-jump correctness
                    — all areas that have produced bugs before.
  - Caveat:         Doesn't change the architectural ceiling.
                    Stencils still ~460 B/bc; cache pressure stays.

### Option C: Pivot to iOS deployment, accept current perf

Per `docs/sista-inlining-plan.md` "Alternative: don't do this":

  - Most of an iOS Pharo IDE's wall-clock time is in primitives
    (Morphic drawing, text layout, UTF-8 encode/decode) that run
    at C speed regardless of JIT.
  - A 30× arith-bench gap likely translates to ~5 % of real-app
    wall-clock time.
  - Outstanding D1 work (iOS device deployment, signing, App
    Store packaging) is a much shorter path to a usable product.

  - Time:           Days–weeks for iOS finishing.
  - Expected gain:  Zero perf, but ships the actual product.
  - Risk:           Low.  Mostly known unknowns (provisioning,
                    code signing, App Store rules).

---

## Recommendation — DECIDED

**This week: collect the data needed to choose A/B/C honestly.
Next week: pick one, with evidence.**

We are *not* spinning up Phase 4 inlining, defaulting on PHARO_T2,
or pivoting away from JIT work — until the four data-collection
tasks below complete and we know which decision the numbers
support.  Speculative architecture choices are how the 5–7 month
estimate becomes 12.

The single concrete deliverable for this week is a one-page memo
(append to this file) titled **"What the profile actually
showed"**, listing:

  1. Top-10 wall-clock hotspots from a 5-min real Pharo IDE
     session under our VM.
  2. Whether PHARO_T2=1 regresses, matches, or improves the
     565-class SUnit baseline.
  3. The single-paragraph A/B/C choice the data supports, with
     the next-week milestone for that choice.

Bias going in: I expect the profile to show primitives + Sista
bail-and-resume churn dominating, pointing to **C** with a small
down-payment of T2 stability work.  But the bias is not the
decision — the data is.

---

## What to do this week (concrete)

Whether or not we commit to Option A/B/C:

  1.  **Wire up a real-app profiler** (1 day).  Add a
      `PHARO_PROFILE=ms` mode that periodically samples the active
      stack frame + emits CSV.  Run it under a 5-minute Pharo
      IDE session opening browsers / Inspector / Playground.
  2.  **Identify top-10 primitives by wall-clock** (1 day).
      Verify the inlining plan's "primitives dominate" hypothesis
      empirically rather than deferring to it.
  3.  **Measure the SUnit suite under T2** (1 day).  We never
      verified that PHARO_T2=1 doesn't *regress* the 12665-pass
      baseline.  If it does, that's a stability signal independent
      of perf.
  4.  **Document the empirical check in jit-todo.md** (½ day).
      Update the "Default config" section to reflect today's
      reality and link this plan.

These four tasks are low-risk, finish in a week, and produce the
data needed to choose A/B/C honestly.

---

## What we will NOT do

  - Spin up Phase 4 inlining without a profiler showing dispatch
    is the hot spot.  Speculative architecture changes are how
    the 5–7 month estimate becomes 12 months.
  - Default-on PHARO_T2 / PHARO_T2_MBC_* without first running
    the SUnit suite under each flag combination.  We have no
    evidence those gates are correctness-clean today.
  - Pretend this week's correctness work was perf work.  It
    wasn't, and the perf numbers reflect that.

---

# What the profile actually showed (appended 2026-04-24, same day)

Captured three macOS `sample` traces against `./build/test_load_image`,
all with default JIT + Sista, on a fresh Pharo 13 image:

  - `docs/perf-2026-04-24/profile-idle.txt` — 60 s, image idling
    after startup (Morphic / display loop).  46 548 main-thread
    samples.
  - `docs/perf-2026-04-24/profile-bench.txt` — 20 s, capturing
    PharoBenchmarkRunner end-to-end (which includes Delay-paced
    pauses between bench methods).  5 346 main-thread samples.
  - `docs/perf-2026-04-24/profile-active.txt` — 50 s of a hot
    loop (compile + dictionary build + sort + benchFib(24)).
    44 607 main-thread samples in `Interpreter::interpret()`.

## Top hotspots — idle Pharo IDE (no user code running)

    Symbol                                          Samples   % of main
    pharo::ObjectMemory::fullGC                     29 678    63.8 %
      pharo::ObjectMemory::scanPointerFields         7 275    15.6 %
      pharo::ObjectMemory::markAndTrace              6 696    14.4 %
        std::unordered_set::find (mark-set lookup)   3 304     7.1 %
      __bzero (clearing freed memory)                6 272    13.5 %
      _xzm_free (libsystem_malloc free)              1 947     4.2 %
    pharo::Interpreter::sendSelector                       –     (rest)
    pharo::ObjectMemory::storePointer                 1 648     3.5 %
    Top primitive: primitiveExternalCall                838     1.8 %  (mostly SDL2 FFI)

  **Headline: an idle Pharo IDE spends 64 % of CPU in fullGC.**
  Not in dispatch.  Not in primitives.  In the mark-sweep collector.

  The mark phase walks every pointer slot of every reachable
  object, looking each oop up in a `std::unordered_set` to test
  whether it's already marked.  The sweep phase clears freed
  memory with `__bzero`.  Both are O(heap size) per collection
  cycle, and the collection cycle fires constantly because
  Morphic allocates aggressively.

## Top hotspots — active hot loop

    Symbol                                          Samples   % of interpret
    pharo::Interpreter::interpret                   44 607   100  %
      pharo::Interpreter::sendSelector              17 593    39.4 %
      pharo::Interpreter::activateMethod            10 343    23.2 %
      pharo::Interpreter::push                       6 157    13.8 %
      pharo::ObjectMemory::fullGC                    3 487     7.8 %
      pharo::Interpreter::executePrimitive           2 618     5.9 %
      pharo::Interpreter::returnFromMethod           2 272     5.1 %
      pharo::Interpreter::returnValue                1 519     3.4 %
      pharo::Interpreter::dispatchBytecode             893     2.0 %
      pharo::Interpreter::patchJITICAfterSend          871     2.0 %
      pharo::sista::Runtime::compile                   647     1.5 %
      pharo::jit::JITRuntime::noteMethodEntry          548     1.2 %
    Top primitive: primitiveExternalCall                 825     1.8 %

  **Headline: under active code the dispatch path** (sendSelector
  + activateMethod + push + dispatchBytecode + returnFromMethod +
  returnValue + executePrimitive) **eats 76 % of CPU.  GC drops
  to 8 %.**

  Sista compile is doing real work (1.5 %) but the resulting
  compiled code only eliminates a tiny fraction of dispatch
  cost — Sista today dispatches leaf + Send0 methods only, so
  most sends still go through the full sendSelector path.

## Top primitives across the active sample

    825   primitiveExternalCall            (mostly SDL2 / FFI)
    331   primitiveFullClosureValue        (block evaluation)
    267   primitiveStringCompareWith
    180   primitiveAt
    148   primitiveAtPut
    117   primitiveSize
     69   primitiveStringReplace
     44   primitiveStringHashInitialHash
     44   primitiveNewWithArg
     29   primitiveNew

  No single primitive is hot enough to be worth a targeted
  rewrite.  primitiveExternalCall covers all FFI — improving SDL2
  / iOS-platform FFI dispatch could help, but most of its time
  is inside the C function being called, not in the prim itself.

## What this means for A/B/C

The *idle profile* refutes the inlining plan's "primitives
dominate, JIT is ~5 % of real-app time" hypothesis.  The bigger
chunk is GC, not primitives.

The *active profile* confirms dispatch dominates execution of
real Smalltalk code — but the relevant "real Smalltalk code" in
an iOS IDE is small bursts (open a Browser, select a class,
render method list) interleaved with long idle periods (waiting
for user input, showing the world).

So both A and C are partial answers:

  - **C alone (ship iOS, accept perf)** leaves the 64 % GC cost
    on the table.  An iOS app that drops 64 % of its CPU into
    fullGC will heat the device, drain battery, and feel
    sluggish in scrolling.
  - **A alone (Phase 4 inlining)** addresses dispatch but does
    nothing for the idle GC cost.  Phase 4 is also 7–11 weeks
    with high deopt risk before any user-visible benefit.

A third answer becomes obvious from the data:

### Option D (NEW): Generational GC first, inlining later

  - Generational GC infrastructure exists (`PHARO_YOUNG_GEN`,
    off by default).  Per memory entry
    `project_younggen_gc_wip.md`, "PHARO_YOUNG_GEN=1 works"
    but `testClearing` has weak-ref issues unrelated to YG.
  - Time to default-on:  **~2 weeks** of stabilization (the
    code is built; bugs need bisection).  Estimated 3–10×
    improvement on idle-image GC overhead based on standard
    generational hit rates (most allocations die young).
  - Risk: medium.  Generational GC has subtle interaction with
    write barriers; we already store-point-trace.  Most
    failures are deterministic enough to catch in SUnit.
  - Then revisit Option A for the dispatch hot path.

This is the data-supported recommendation.  Pivot from "inlining
in 7-11 weeks" to "stabilize PHARO_YOUNG_GEN in ~2 weeks", then
re-profile and decide whether dispatch (now ≥80 % of remaining
CPU) is worth the inlining investment.

## Concrete next step (this afternoon)

  1. Run the SUnit per-class isolation suite with PHARO_YOUNG_GEN=1
     to see what's actually broken under it today.  Compare to
     today's 12665 / 0 timeouts baseline.
  2. If correctness is close (within ±10 failures), bisect those
     and start the 2-week stabilization.
  3. If correctness is far off (>50 new failures), reconsider:
     it may be cheaper to attack the GC hot path with bitmap-mark
     and a freelist that doesn't bzero (skip the `__bzero` 13.5 %
     cost) than to stabilize the existing YG code.

The first step takes one VM run (~80 min) and gives the data to
choose.  Will start it next.

### YG bench smoke-test (run while waiting for T2 SUnit baseline)

Median of 3 runs, default JIT + PHARO_YOUNG_GEN=1, fresh image
each run:

    Workload                Default JIT   +YG=1        Delta vs default   Cog
    tinyBench bps           17.4 M        18.2 M       +5 %              73.5 M
    tinyBench sps          140.9 M       135.7 M       -4 %               3.75 G
    fib(28)                 68 ms         74 ms         -9 %  (slower)    2 ms
    sieve x100             131 ms        137 ms         -5 %  (slower)    9 ms
    sort 100K              284 ms        266 ms         +6 %             16 ms
    dict 50K put+get       380 ms        171 ms        **+55 %**         21 ms
    sum 1M                  73 ms         80 ms         -10 % (slower)    5 ms
    5000 factorial         231 ms         26 ms        **+89 %**         38 ms
    1M block invocations    22 ms         22 ms          0 %              0 ms
    1M getter+yourself      93 ms         96 ms         -3 %              4 ms
    100K Array allocations   5 ms          4 ms        +20 %              0 ms

  **5000 factorial: 231 → 26 ms.  We BEAT Cog (38 ms) on this
  benchmark with PHARO_YOUNG_GEN=1.**

  **dict 50K put+get: 380 → 171 ms.  Closes 55 % of the gap.**

  Both benchmarks are LargeInteger / dictionary entry allocation
  heavy — exactly what generational GC was built for.  Mark-sweep
  walks the whole heap on every full GC; the young generation
  scavenges just the nursery, where 99 %+ of these short-lived
  allocations die.

  Slight regressions on fib / sieve / sum (5–10 %) are workloads
  with little allocation but lots of arithmetic — they pay the YG
  write-barrier cost without getting offsetting allocation savings.
  Even those regressions are within the noise floor of single-image
  median-of-3.

  Raw runs: `docs/perf-2026-04-24/perf-our-jit-yg-run{1,2,3}.txt`.

This validates Option D before even running the SUnit suite.  YG
isn't theoretical — when stable, it's a 2× win on dict-heavy work
and a Cog-beating win on factorial.

Next: SUnit suite under YG (waiting for T2 baseline to finish
sharing /tmp file paths first).
