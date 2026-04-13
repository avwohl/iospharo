# TODO

## 1. Investigate 18 test errors
Look at the 18 errors from the test suite, understand root causes, fix what's fixable.

## 2. T2 decoder: support 0xEB (ExtSuperSend) and 0xF8 (callPrimitive) ✅
These opcodes cause the T2 decoder to bail before reaching backward jumps.
Adding them would let sieve's inner loop and other hot methods T2-compile.

## 3. Fix MIR opt level 2 subtraction bug ✅
MIR_SUBO gives wrong results at opt level 2. All registers spill at opt 0,
negating T2's register allocation advantage. Fixing this is the single
biggest T2 performance unlock.

## 4. Fix T2/chain-loop SavedFrame interaction (remove backward-jump workaround) ✅
jit_t2_send's SavedFrame fallback causes mismatched T2/T1 resume through
the chain loop. The backward-jump-only filter is a workaround. Fixing
the root cause would let T2 compile all methods.

## 5. Float boxing elimination
NBody/Mandelbrot are ~10-26x vs Cog, dominated by Float allocation.
Specialize Float +/* to avoid sends, reduce boxing.

## 6. Block value/value: as stencils (prim 207/209) ✅
Inline FullBlockClosure>>value/value: in stencil_sendJ2J. IC extra bit 59
(BLOCK_VALUE_BIT) marks block value send sites. Stencil extracts compiledBlock
from closure, does inline MethodMap lookup, and J2J-calls the block's JIT code.

## 7. Adaptive J2J depth ✅
Per-method j2jDepthLimit (default 2, max 8). TCP slow-start: 8 consecutive
clean runs promote by 1, immediate reset to 2 on materialization bail.
No regression on existing benchmarks.

## 8. Richards/DeltaBlue polymorphic dispatch (groundwork)
Added jitEntry field to MegaCacheEntry with prim-safety validation.
IC hit rate is already 98% for current benchmarks (fib/sieve/tinyBenchmarks).
Inline stencil mega J2J crashes due to trampoline state interaction (needs
investigation). Trampoline already converts ExitSendCached→J2J for mega cache
hits. Real testing needs SMB benchmark harness injection.

## 9. Investigate EXIT_J2J_CALL crash from mega cache stencil path
The stencil EXIT_J2J_CALL path for mega cache hits causes "Stack overflow in
push()" in sieve. Trampoline's Ltramp_convert path for ExitSendCached works.
Direct ExitJ2JCall from stencil doesn't. Root cause unclear — may be related
to trampoline state assumptions about who sets returnValue/cachedTarget.
