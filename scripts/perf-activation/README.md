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

Status 2026-07-05: harness written; real measurement deferred until the
machine is quiet (full-catalog SUnit run in flight).
