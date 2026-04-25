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

### Refined investigation (after deeper look)

The original "block dispatch fast path is bypassed" framing was
wrong.  Tracing showed:

  - The stencil's `BLOCK_VALUE_BIT` branch IS taken on every iteration
    (the IC patch path sets the bit; `j2jTotalCalls` counter confirms
    61M successful entries during the bench run).
  - The J2J return path is `J2J_INLINE_RETURN` in `stencils.cpp:484`,
    not the `Interpreter.cpp:13238` chain loop.  The chain loop is
    in fact dead code by default (`PHARO_RESUME_J2J=0`); the 1/61M
    counter only counts chain-loop returns and was misleading.
  - Block-only bench in isolation: 22-24 ms per 500K iterations =
    44 ns/call.  fib(28) at 19 ms / ~514K recursive calls =
    37 ns/call.  Per-call cost is essentially identical between
    block dispatch and method send — so the issue isn't
    block-specific.

### Real bottleneck: per-call setup cost

Cog dispatches each `value:`/method send in ~2 ns (~6 cycles); my
VM takes 37-44 ns (~120 cycles).  The ~115-cycle gap per call
breaks down approximately into:

  ic-probe + bit-decode               ~10 cycles
  6-probe methodMap lookup            ~25 cycles  (most-common pattern)
  state validation (cmp/branch chain) ~15 cycles
  J2JSave write (8 fields × 8 bytes)  ~20 cycles
  callee state setup (writes)         ~25 cycles
  capture-copy loop (often 0 iters)   ~5 cycles
  tail-call indirect branch           ~5 cycles
  return: J2J_INLINE_RETURN           ~30 cycles

Cog presumably collapses most of this into a single direct branch
plus ~5 cycles of stack save/restore.

### Concrete optimization targets, ranked

### Profiling deep-dive (`sample` mid-bench)

A 4-second `sample` taken mid-bench (long version of block(500K),
5000 outer iters) shows:

  - 2230 / 3114 samples in `Interpreter::returnValue`
  - 2046 / 3114 samples in `tryJITResumeInCaller`
  - **1733 / 3114 (55%) in `pthread_jit_write_protect_np`**

Every `value:` call exits JIT to the interpreter, calls
`returnValue` → `tryJITResumeInCaller` → `JITRuntime::tryResume`
which does 3 W^X flips per call.  500K iterations × 3 flips per
call adds up.

### Why each call exits JIT

The inner block `[:x | x + 1]` is **not JIT-compiled**, so the
stencil's BLOCK_VALUE_BIT branch's methodMap lookup returns null,
falls through to the slow path, and exits to the interpreter.

The interpreter activates the block via `primitiveBlockValue` →
`activateBlock`, runs the body, returns.  `returnValue` →
`tryJITResumeInCaller` re-enters JIT in the bench's caller.

The interpreter never bumps the inner block's JIT compile counter.
Tested: adding `noteMethodEntry(slot1)` to `primitiveBlockValue`
caused a **compile explosion** (85 → 1024 methods compiled), IC
hit rate collapsed from 99% → 19%, and bench got SLOWER (24 → 33
ms).  The default CompileThreshold=2 is too aggressive for blocks.

Counter-experiment: also tested W^X removal (`PHARO_NO_WX_RESUME=1`
gates the makeWritable/makeExecutable calls in tryResume).  No
perf improvement.  So either W^X is cheaper than its sample cost
suggests (the syscall is fast but counted heavily by sample), or
the bench's per-iteration cost is in something else inside
tryResume (methodMap lookup, validations, state setup).

### What actually needs fixing

For the JIT to stay in JIT through the bench's inner loop:

  - The inner block must be JIT-compiled, AND
  - The compile heuristic must NOT compile every cold block-of-
    block (which causes the IC churn observed above).

A two-tier counter design seems right: per-block call counter
that only triggers `noteMethodEntry` after N (say 1000) actual
block calls, separate from the method-level threshold of 2.
Implemented as a u32 field on the BlockClosure-receiver
side-table or on the CompiledBlock itself.

Without that, the per-call cost stays at ~44 ns (vs Cog's ~2 ns)
and block-heavy workloads stay 22x slower.

### Original target list (ordered by leverage)

1. **Cache the compiledBlock JIT entry per IC site** — for hot
   block sites, the receiver class is constant but the
   compiledBlock is also effectively constant (one closure object
   reused across iterations).  Add a per-site shadow slot that
   stores the compiledBlock's JIT entry pointer.  Cuts the 6-probe
   methodMap lookup (~25 cycles) when the cached pointer matches
   the receiver's compiledBlock slot.

   **Prototype attempted (reverted):** stored the cached entry in
   `icData[17]` (last entry's extra slot, unused for monomorphic
   sites) and added a fast-path read in the stencil's BLOCK_VALUE
   branch.  Two issues prevented a measurable win:

     - At IC-patch time the inner block hasn't yet been JIT-compiled,
       so the cache is stored as zero.  Logged "BLK-CACHE-MISS"
       entries confirmed this for the bench's hot blocks.
     - Stencil-side write-back of the cache after a successful
       methodMap lookup didn't change perf either, suggesting the
       write either doesn't land (W^X on Apple Silicon — IC area is
       in the JIT code zone, executable-only on the running thread
       after `makeExecutable`) or the next-call fast-path read still
       sees 0.

   The right shape needs a separate writable side-table per JITMethod
   (mirror of the Task #41 `selBitsArray`), not the W^X-protected IC
   data area.  Larger refactor than fits in a single iteration but
   the design is now clear.

2. **Specialize the no-capture block stencil** — `[:x | x + 1]` has
   no captured values, so the `numCopied` loop is wasted bookkeeping.
   A `stencil_blockValueNoCapture1Arg` variant could be ~20% smaller.

3. **Shrink J2JSave** — currently 8 fields × 8 bytes = 64 B.  The
   `resumeAddr` and `jitMethod` may be derivable from the caller's
   compile-time-known PC; if so, writing them on every call is
   redundant.

4. **Push-then-call vs argument-stack-frame** — every call writes
   nArgs+1 stack slots that the callee then re-reads.  An ABI that
   passes the first 1-2 args in registers would skip those memory
   trips.

The existing INLINE_CONST Phase 4 work is in the noise on these
benches because it only fires on a tiny pattern (3-5 sites per
compile).  Closing the per-call gap is the high-leverage path.
