# Benchmark comparison 2026-04-25

Custom bench, 5-iteration warmup, best-of-3 timing.  Stock Pharo 13
fresh image (`/tmp/bench4`).  Mac M-series ARM64 Catalyst.

```
                  StockCog  NoJit    Default  Defer=1  Defer=1+   Defer=1+
                  (cog jit) (interp) (4s def) (eager)  No_Sista   InlineConst
  fib(28)         2 ms      68 ms    75 ms    19 ms    19 ms      19 ms
  fib(32)         17 ms     471 ms   132 ms   132 ms   134 ms     130 ms
  sum(1M)         1 ms      23 ms    13 ms    13 ms    13 ms      13 ms
  block(500K)     1 ms      26 ms    86 ms    87 ms    113 ms     110 ms
  pointX(500K)    0 ms      17 ms    5 ms     5 ms     5 ms       5 ms
  create(100K)    0 ms      6 ms     7 ms     7 ms     7 ms       8 ms
  dict(10K)       0 ms      21 ms    47 ms    46 ms    52 ms      54 ms
```

## Key findings

**block(500K) is SLOWER under JIT than interpreter (86 vs 26 ms)**:
the JIT block-dispatch path has a 3.3x regression vs the interpreter.
Cog does it in 1 ms.  This is the single biggest perf gap to close.

**fib(28) gets 4x speedup from eager JIT** (75 vs 19 ms) but is still
~10x slower than Cog.  fib(32) doesn't speed up (defer=1 same as
default), suggesting JIT was already warm by then.  Once warm we top
out at 8-10x slower than Cog on send-heavy code.

**Sista helps blocks (+30%)** even at default settings, suggesting
the bypass it provides past T1's IC probe is real.  But it doesn't
help arith-heavy benches.

**INLINE_CONST=1 + AFTER_T1=1 doesn't move the needle.**  block(500K)
even regresses 30% under AFTER_T1=1 because it changes Sista's
compile timing and the bench's hot block class never reaches the
warmup threshold.

## What to optimize next, ranked by impact

1. **Block-dispatch fast path in JIT** — biggest single gap.  87x
   slower than Cog, and JIT is somehow worse than the interpreter
   here.  Either the block-value stencil bails frequently, or the
   J2J trampoline overhead per iteration dominates.  Diagnostics:
   add a counter for "block stencil entered" vs "block stencil
   bailed to interp" in stencil_sendJ2J's BLOCK_VALUE_BIT branch.

2. **Send overhead in fib** — 8-10x gap on pure send recursion.
   Likely a per-send fixed cost (frame setup, IC probe).

3. **Dict / Create / Sum** — 13-50x slower on hot loops with
   inline arith and ivar access.  pointX(500K) is only 5x slower
   so the gap depends heavily on what's inside the loop.

The existing INLINE_CONST work (15+ commits this session) is in the
noise on these benches.  Without fixing the block-dispatch slowdown
it can't show up.
