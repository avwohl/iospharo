# PHARO_SISTA_INLINE_ARITH=1 perf measurement — 2026-04-25

Phase 3+4 deliverable: kPrimTagCheckInt deopt + arith admission.
Commit `8bf2397`.

## Method

Median of 3 bench runs each, default JIT settings vs same +
PHARO_SISTA_INLINE_ARITH=1.  Same prepped Pharo 13 image,
M-series arm64, both warm-startup.

## Results

    Workload            Default    INLINE_ARITH=1   Δ
    fib(28)             75 ms      74 ms            +1 %  (noise)
    sieve x100         123 ms     136 ms            -10 %  (slower)
    sort 100K          290 ms     271 ms             +7 %  (faster)
    dict 50K put+get   180 ms     177 ms             +2 %  (noise)
    sum 1M              84 ms      83 ms             +1 %  (noise)

## Verdict — keep flag opt-in

Run-to-run variance ~10 % on these microbenchmarks; the
PHARO_SISTA_INLINE_ARITH win/loss numbers are within that
noise floor.  The earlier single-run measurement showing
"fib 66 vs 74 = 11 % faster" was cold-cache noise, not a
real win.

What's actually happening:
- PHARO_SISTA_INLINE_ARITH=1 admits methods with arith opcodes
  to Sista dispatch (gate previously blocked them).
- Methods get the type-check inline + deopt path on operand
  miss.
- Type-check overhead (3 instructions per check, 2 checks per
  arith) adds ~6 instructions per arith op.
- Saved cost: T1 stencil dispatch for the same arith ~equal.
- Net: roughly wash for arith-heavy loops.  Marginal benefit
  for methods where arith is a small fraction of body and
  the rest gets Sista-compiled too.

## Correctness

Most tests pass under INLINE_ARITH (TraitTest 54/54,
IntervalTest 260/260, StringTest 438/438).

Known regression:
  IntegerTest>>testIsProbablyPrime — Miller-Rabin overflow
  pre-existing limitation: kPrimAddInt/Mul don't detect SmallInt
  overflow.  Same gap exists under PHARO_SISTA_UNSAFE_ARITH=1.

## What this teaches

The deopt INFRASTRUCTURE is working — kPrimTagCheckInt with
deopt landing pad is end-to-end functional.  But the
inline-arith-with-type-check pattern alone doesn't move
benchmarks because:

1. The type-check overhead matches the savings.
2. The methods that benefit most are arith-heavy, but those are
   already fast under T1 stencil dispatch.
3. The real perf gap is in METHOD INLINING (Phase 4 proper),
   not arith inlining.

## What would actually help

For real wins, the next move is:
1. **Type inference**: when both operands of arith are statically
   provable SmallInts (e.g., from prior tag checks or constants),
   skip the redundant check.
2. **Method inlining**: inline a hot getter so the SEND vanishes
   entirely.  This eliminates per-call dispatch overhead, which
   is the actual hot spot per profile data.

(1) is a quick optimization; (2) is the big-but-multi-week
deliverable from the original plan.

Raw bench data: this directory.
