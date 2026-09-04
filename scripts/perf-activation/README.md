# perf-activation — the J2J activation-cost wall (~210 cycles vs Cog ~5)

Context: memory `block-invocation-perf.md` + `perfdb-jit-slower-than-interp.md`.
All real-method activations (method sends AND block value:) cost ~70ns on
arm64 vs Cog ~2ns; primitives without frames (e.g. `size`) are at parity.
Falsified already: FSR/lazy-frame store deletion (wash — OoO absorbs stores),
J2JSave push size, methodMap lookup, closure side-stack sync, home-frame
computation, noteMethodEntry. Shipped reductions: young-gen 32→128MB (6%,
REVERTED later for Delay-timing wedge), Sista recompile churn fix (~8%).

Remaining hypothesis (2026-06-25): the cost is LOADS on the critical path —
candidates, in test order:

1. bcToCode resume-address TABLE LOOKUP per call (TrampolineAsm.S:408-411,
   a dependent load chain) — ablation: hack a build where the J2J return
   path uses a hardcoded/register-carried resume address for the bench
   shape, measure delta.
2. IC probe loads at the send site (icData entry scan: 1-6 key compares +
   extras load) — ablation: PHARO_T1_MAX_IC=1-style knob or force
   monomorphic-J2J spec (`monoJ2JSpec` path) and compare.
3. Callee JITMethod field reads during callee setup (jitMethod, methodOop,
   tempBase, literals, bcStart — each a dependent load off the JM pointer)
   — ablation: pack the hot fields into one cache line (JITMethod field
   reorder) and measure; or prefetch JM in the caller stencil.
4. Return-path reload of the J2JSave area (56-byte load burst with use
   dependency) — ablation: keep caller state in callee-saved registers for
   leaf J2J calls (deep emit change; estimate first via 1-3).

Method: run `bench_activation.st` (see header for invocation) on a QUIET
machine, 4 runs, then apply ONE ablation at a time and re-run. The bench
callee defeats every trivial-inline classification (two statements + temp)
so the send takes the true J2J activation path. per-send-ns in the output
is (send5M - loop-baseline)/5M.

For instruction-level attribution without ablations: the hot path is the
inline save/BLR/restore emitted around stencils.cpp:1569+ (j2j_direct_call)
and the AsmjitT1 send-site emit — native code, so `sample` attributes to the
enclosing JIT region only. Use Instruments "CPU Counters" (cycles + L1D
miss + branch-mispredict per instruction) on the bench process for real
per-instruction data:

    xctrace record --template 'CPU Counters' --launch -- \
      ./build/test_load_image /tmp/harness/Pharo.image.bak eval "$(cat scripts/perf-activation/bench_activation.st)"

Status 2026-07-05: harness written; QUIET-MACHINE BASELINE recorded
(arm64, M-series, 4 runs):

    send5M    724 / 746 / 792 / 855 ms   -> per-send ~140-165 ns
    block5M   522 / 463 / 489 / 488 ms   -> per-block ~93-100 ns
    baseline  24-26 ms (empty +-loop)

Note per-send here (~140-165ns) is HIGHER than the memory's earlier
~70-100ns isolated numbers — this callee has 2 temp-writing statements
(to defeat trivial-inline classification) vs the earlier `^x+1` shape,
so it carries a slightly bigger body on top of the activation cost.
Blocks are FASTER than method sends on this bench (the BV-inline path
is more optimized than plain J2J method send, matching the memory's
2026-06-25 finding).  Ablation deltas should be read against THESE
numbers with the SAME bench.


## 2026-09-04: a REAL-workload profile and knob triage (not the microbench)

Everything above is `bench_activation.st`.  This section is the same question
asked of the workload that actually costs test results — the whole-image
reflective scan behind defect #6's TIMEOUTs
(`NoUnusedVariablesLeftTest>>testNoUnusedTemporaryVariablesLeft`, run outside
SUnit so it completes rather than being killed).  Native arm64, quiet machine.

Baseline, interleaved reps:

    stock Cog (x86_64 Rosetta)     5.76 s
    ours, JIT on                 216.8 / 217.3 / 218.2 s
    ours, JIT off                177.4 / 160.9 / 161.2 s

**The JIT is 35% slower than our own interpreter here.**  That is consistent
with `docs/results-perfdb.md`'s "1M blocks: JIT 5.5x SLOWER" — a reflective
scan is `allMethodsDo: [...]`, i.e. block-activation-bound.

`sample`, 10 s mid-scan, top of stack (JIT-emitted code carries no symbols so
it is excluded; these are the C++ frames):

    724  Interpreter::tryJITResumeInCaller
    560  Interpreter::activateBlock
    443  Interpreter::tryJITActivation
    436  ObjectMemory::scavenge()::$_3
    386  JITRuntime::noteMethodEntry
    327  Interpreter::interpret
    319  JITRuntime::tryResume
    295  Interpreter::push
    254  Interpreter::upgradeICToJ2J
    196  Interpreter::primitiveFullClosureValue

Knob triage, one rep each, same expression, same machine:

    knob                          time      delta vs default
    (default)                    217.3 s      --
    PHARO_T1_NO_CALLER_RESUME    187.2 s     -14%
    PHARO_NO_BLOCK_VALUE_SPEC    216.9 s       0
    PHARO_NO_EAGER_BLOCK_VALUE   217.5 s       0
    PHARO_T1_NO_INLINE_J2J       220.8 s      +2%
    PHARO_NO_JIT                 161.0 s     -26%

**Only the caller-resume path has a measurable share**, and it is 14% — which
matches `tryJITResumeInCaller` topping the profile, and makes hypothesis 1 (the
bcToCode resume-address lookup on the J2J return path) the one with evidence
behind it.  The other 12% of the gap to the interpreter is spread across
`activateBlock` / `tryJITActivation` / `noteMethodEntry` and does not fall to
any single existing knob.

Two things NOT to re-test:

  * the block-value specialisations — `PHARO_NO_BLOCK_VALUE_SPEC` and
    `PHARO_NO_EAGER_BLOCK_VALUE` are each within noise of the default, so the
    block cost is in the generic activation path, not in those emits;
  * `noteMethodEntry`'s `hasSplice()` — it looks like the per-call
    `unordered_set` lookup CLAUDE.md warns about, but `sistaRuntimeForGCHook_`
    is null unless `PHARO_SISTA_COMPILE`/`sistaDispatch` is set, so the lookup
    never runs in a default build.

`PHARO_T1_NO_CALLER_RESUME` is NOT a fix — it is the x86 sp-leak workaround
knob, it does not close the gap to the interpreter (187 s vs 161 s), and
disabling caller re-entry costs send-bound code where the JIT does win.  It is
useful here only as an attribution instrument.
