# Performance Optimizations

Roadmap for interpreter performance improvements. JIT is not possible on iOS
due to Apple restrictions, so all gains must come from interpreter optimization.

## 1. Flat switch dispatch (DONE)
- Replaced if-else chain in `dispatchBytecode()` with a flat `switch(bytecode)`
- Removed V3PlusClosures bytecode paths (only Sista V1 for Pharo 10+)
- Compiler emits a direct jump table — O(1) dispatch instead of O(n) comparisons
- Expected gain: 15-30% on tight loops

## 2. Slim down step() hot path (DONE)
- Rewrote `interpret()` with fast inner loop: fetch→dispatch→countdown pattern
- All periodic checks consolidated behind a single `--checkCountdown <= 0` gate
- Tiered checks: every 1024 (GC, timer, yield), every ~64K (preemption, watchdog), every ~100K (input, display)
- Extracted `handleForceYield()` from inline step() code
- `test_load_image` now calls `interpret()` directly with a monitoring thread
- Measured gain: **12.7%** (5731ms → 5002ms on full test suite, build 111)

## 3. Clean up sendSelector() hot path (DONE)
- Moved selector byte extraction and receiver class name logging behind sendCount_ guard
- Removed per-send `g_watchdogPrimIndex` writes (was dead code, never read)
- Moved corruption check behind `__builtin_expect`
- Diagnostics now at bottom of function, only on the cache-miss fallthrough path
- Measured gain: **~9% CPU reduction** (isolated)

## 4. Multi-probe method cache (DONE)
- Expanded cache from 2048 to 4096 entries
- Added 2-way set-associative probing (primary + rotated secondary hash)
- On miss: two probes before falling through to full lookupMethod()
- On insert: primary slot preferred, secondary used for eviction
- Combined with #3, measured gain: **~19% CPU reduction** vs build 112 baseline
  (20.81s → 16.88s user time on full test suite)

## 5. Reduce syscalls in periodic checks (SKIPPED)
- Tried gating `checkTimerSemaphore()` behind 8x sub-counter (every 8192 bytecodes)
- Reduced CPU time but caused Delay scheduler latency issues (tests measure elapsed time)
- The timer syscall is already cheap on macOS (VDSO). Not worth the tradeoff.

## 7. Computed goto dispatch (DONE)
- Replaced while loop + dispatchBytecode() call in interpret() with
  GCC/Clang computed goto labels (&&label / goto *table[bc])
- Fast-path handlers for ~200 of 256 bytecodes:
  push/pop/store/dup, short jumps with inline boolean check,
  arithmetic sends with inline SmallInt fast path,
  FullBlockClosure value/value: fast path,
  literal sends direct to sendSelector
- Slow path for extensions, returns, closures via dispatchBytecode()
- Benefits: per-handler branch prediction, no running_/bytecodeEnd_ check
  between bytecodes, SmallInt arithmetic inlined at dispatch site
- Measured gain: **~2.5% CPU** (18.44s → 18.00s avg across 3 runs)
  Matches research prediction of 2-5% on Apple M1 (good branch predictor)

## 6. Inline caching (PICs) — investigated, not viable without JIT
- Implemented monomorphic IC using instructionPointer_ as send-site key
- 75% IC hit rate achieved, but requires selector validation (IC key doesn't
  encode selector — `cannotReturn`/`doesNotUnderstand` sends at arbitrary PCs
  pollute the cache with wrong selectors for that site)
- No measurable CPU improvement over the 2-way global method cache (16.88s
  both with and without IC). The global cache already handles 90%+ of sends;
  IC just avoids the hash computation, which isn't the bottleneck
- True PICs require per-send-site dispatch stubs (i.e., generated native code),
  which is fundamentally a JIT technique. Not possible on iOS.
- **Conclusion**: The 2-way 4096-entry global method cache is the right design
  for a pure interpreter. Further send optimization would need bytecode
  superinstructions (combining push+send into one dispatch).

## 8. Speculative superinstructions (DONE)
- Profile-guided: instrumented DISPATCH_NEXT to count all 65536 bytecode pairs
  over 100M bytecodes of the full test suite
- Implemented speculative fusion in computed goto handlers (peek at next bytecode):
  - SmallInt comparison + conditional jump: 5.3M pairs → branch directly,
    skip boolean object creation (eliminate push+pop+identity-check)
  - push1+arith+ (2.2M, 65% hit), push1+arith- (183K), push0+arith= (356K):
    increment/decrement without pushing constant
  - pushNil + spec== (1.73M, 46% hit): nil identity check without pushing nil
  - dup + pushNil + spec== + jump (1.45M, 96% hit): full nil-check idiom
  - spec== (0x76) fast handler with branch fusion (2.4M total)
- Stack balance bugs found and fixed: pushNil+==+jump missing pop,
  dup+pushNil+== fallback pushing extra value
- Measured gain: **~2.8% CPU** (18.00s → 17.49s avg across 3 runs)
  Below Phase 3 estimate (15-30%) because M1 branch predictor already handles
  the dispatch efficiently. Dispatch is a smaller fraction of total time than
  the gap breakdown estimated.

## 9. Trivial getter/setter inlining (DONE)
- At method cache time, detect trivial accessor methods:
  - Getter: `pushRecvVar N + returnTop` (2 bytes) or `extPushRecvVar + returnTop`
  - Setter: `popStoreRecvVar N + returnReceiver` or `extPopStoreRecvVar + returnReceiver`
- Store accessor/setter index in MethodCacheEntry alongside method/primitive
- On cache hit: bypass full method activation (frame push/pop/IP switch)
  - Getter: replace receiver on stack with inst var value
  - Setter: store arg in receiver inst var, pop arg, leave receiver
- Profile data: 7.2M send0 calls per 100M bytecodes — many are accessors
- Measured gain: **~6.4% CPU** (17.49s → 16.37s avg across 3 runs)
  This is the largest single optimization since the method cache improvements
  (build 113, 19%). Eliminates real work (frame setup/teardown) rather than
  just dispatch overhead.

## 10. returnsSelf trivial method inlining (DONE — no measurable gain)
- Extend trivial method detection to methods whose only bytecode is
  `returnReceiver` (0x58) — covers `yourself` and similar identity methods
- On cache hit with argCount == 0: return immediately (receiver already on stack)
- Measured gain: **within noise** (~0.8%, A/B test 3 pairs)
  These methods are too rare in hot paths to move the needle.

## 11. primitiveQuit cleanup (DONE)
- Removed test-runner-specific guard from primitiveQuit that blocked all quit
  attempts when /tmp/sunit_test_results.txt existed without a BATCH COMPLETE
  marker. The 10-second startup grace period is sufficient.
- Benchmark runs now exit cleanly (~14s) instead of waiting for 600s timeout.

## 12. Unchecked memory accessors + hot path hints (DONE — ~1% improvement)
- Added `fetchPointerUnchecked()` / `storePointerUnchecked()` that skip
  isObject/isValidPointer validation for objects already known valid.
  Used in: getter/setter fast paths, pushRecvVar handler, method dict
  lookup, class hierarchy walk, literal access, activateMethod,
  activateBlock, pushFrame method header fetch.
- Added `__builtin_expect` hints to activateMethod (CompiledBlock check),
  pushFrame (overflow check, context sync), primitiveFullClosureValue.
- Cached classOf investigated — caused 37000x regression (all cache misses).
  Reverted. The issue: classOf() called before initCachedClasses() during
  early init, returning nil class Oops.
- A/B test (3 pairs, CPU-bound startup.st benchmark):
  Baseline: 6.31, 6.30, 6.35 (avg 6.32s)
  Optimized: 6.26, 6.28, 6.24 (avg 6.26s)
  ~1% improvement — M1 branch predictor handles validation branches near-free.

---

## Planned

### Phase 2: Quickening / specialized bytecodes (revised est. 3-10%)
Replace generic bytecodes with type-specialized variants at runtime.
Originally estimated 20-40%, but tight loop analysis shows our computed goto
already achieves ~4.6 cycles/bytecode for SmallInt arithmetic — competitive
with LuaJIT/Wasm3 interpreters. Quickening would help non-arithmetic sends
(monomorphic send caching, skip classOf+cache probe). The 69x test suite gap
is dominated by per-class startup overhead, not per-bytecode interpreter cost.
See `docs/pseudo-jit.md` for analysis.

### Phase 4: Pre-decoded IR / flat record (revised est. 1-3%)
On method activation, translate Sista V1 bytecodes to fixed-width records.
Originally estimated 10-20%, but extension bytes are only 1.17% of execution,
and dispatch is already ~4.6 cycles/bytecode. Pre-decoded IR would eliminate
literal table indirection but fetchPointerUnchecked already handles this
efficiently. The benefit doesn't justify the memory and complexity cost.
