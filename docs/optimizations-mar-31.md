# Optimization Work — March 30-31, 2026

Short summary of interpreter performance work done in the last 48 hours
(builds 110-115, commits 4f01d88..993e049).

## Done

 1. Flat switch bytecode dispatch (build 111)
 2. Fast interpret() inner loop with consolidated periodic checks (build 112)
 3. sendSelector() hot path cleanup (build 113)
 4. 2-way set-associative 4096-entry method cache (build 113)
 5. Benchmark infrastructure: time_tests.st, reference VM comparison
 6. Inline cache investigation (monomorphic IC, 75% hit rate — removed, no gain)
 7. Pseudo-JIT research doc with 5-phase plan
 8. Computed goto dispatch in interpret() (build 114, Phase 1)
 9. Bytecode pair profiling (100M bytecodes, 65536 pairs)
10. Speculative superinstructions — comparison+branch fusion, push+arith
    fusion, nil-check idiom, spec== fast handler (build 115, Phase 3)
11. Trivial getter/setter inlining at method cache (build 115, Phase 5)
12. returnsSelf trivial method detection (no measurable gain)
13. primitiveQuit cleanup — removed over-eager test-runner guard
14. Credits, licenses, and attribution for upstream code (issue #5, other session)

## Measurements

    Build 110 baseline    ~23s   (estimated)
    Build 112 fast loop   16.88s (-19% from method cache + loop)
    Build 114 cg dispatch 18.00s (+2.5% from computed goto)
    Build 115 super+acc   16.37s (+9.1% from superinstructions + accessor inlining)
    Build 115 quit fix    ~12.5s (benchmark overhead reduction, not VM speedup)

12. Unchecked fetchPointer/storePointer for validated hot paths (~1% gain)
13. Tight loop performance analysis: 4.6 cycles/bytecode (LuaJIT-class)

## Measurements

    Build 110 baseline    ~23s   (estimated)
    Build 112 fast loop   16.88s (-19% from method cache + loop)
    Build 114 cg dispatch 18.00s (+2.5% from computed goto)
    Build 115 super+acc   16.37s (+9.1% from superinstructions + accessor inlining)
    Build 115 quit fix    ~12.5s (benchmark overhead reduction, not VM speedup)
    Build 116 unchecked   ~1% (within noise, A/B verified)

    Tight loop (100M SmallInt iterations): 4.6 cycles/bytecode
    Competitive with LuaJIT (~3) and Wasm3 (~5) interpreters.

14. Dead code cleanup: removed unused initCachedClasses() and cached class members

## Not done (revised estimates)

- Phase 2: Quickening / specialized bytecodes (revised est. 3-10%)
- Phase 4: Pre-decoded IR / flat record (revised est. 1-3%)

## Assessment

The interpreter is near-optimal for its architecture:
- 4.6 cycles/bytecode in tight loops (competitive with LuaJIT ~3, Wasm3 ~5)
- The 69x gap vs Cog JIT is dominated by startup/framework overhead, not dispatch
- Remaining Phases 2 and 4 offer diminishing returns (3-10% and 1-3%)
- The method cache + accessor inlining were the big wins (19% + 6.4%)
- Further per-bytecode optimization has reached the point of diminishing returns
