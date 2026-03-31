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

## 6. Inline caching (PICs) — longer term
- Monomorphic inline cache at each send bytecode site
- Store (classIndex, compiledMethod) pair in side table indexed by PC
- Major step up from global method cache
- High impact but high effort
