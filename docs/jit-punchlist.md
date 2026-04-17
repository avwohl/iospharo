# JIT Punch List

Remaining work after session 23's T2 Permute miscompile fix (f279fd4).
Ordered by descending expected ROI.

## JIT perf

1. **T2 chain-loop continuation**
   When `jit_t2_send`'s callee bails, push a `SavedFrame` for the T2
   caller and let the interpreter continue the callee from its partial
   `state.ip` (instead of restoring the caller and letting interpreter
   re-activate, which caused the Permute double-execution bug). Restores
   the callee-invocation speedup on top of the correctness fix.
   Medium refactor.

2. **T1 J2J memory-op reduction**
   Current J2JSave is 72 bytes (9 fields). Some fields are derivable
   from `jitMethod` (`literals`, `argCount`, `bcStart`). Skipping those
   saves 3 stores per send; restore pays 2-3 loads to derive. Net small
   per-send win, compounded over millions of sends on AWFY. Big lever
   for send-heavy workloads where T1 is currently 0.45-0.57x the
   interpreter.

3. **T2 GC safety re-validation**
   Commit b18e71e disabled T2 by default citing "MIR holds stale oops
   across GC". Post-f279fd4 smoke tests (Richards + tinyBenchmarks
   under PHARO_T2=1) completed without crashes — the rationale may be
   stale. If confirmed safe, T2 can be default-on (though perf is now
   ≈T1, so not a big unlock).

4. **IC hit rate on AWFY**
   47% hit rate observed. `MaxPICEntries=6`. Investigate whether the
   cap is the bottleneck vs. pattern-specific polymorphism.

## Deferred test-suite issues (docs/deferred-issues.md)

5. **#1 Harness SemaphoreTest / valueWithin timing interaction**
   10 tests pass standalone but fail inside the SUnit fork+harness.

6. **#2 Reflection-walk timeouts**
   ProtoObjectTest / ByteSymbolTest select: over allSymbols/allObjects.
   Interpreter speed; JIT should fix but needs re-check post-fix.

7. **#3 Weak-reference / finalization timing**
   5 residuals in WeakKeyDictionaryTest / WeakAnnouncerTest.

8. **#4 JIT eval-mode MAX=50+ hang**
   Classified scheduling, not correctness. Low priority.

## Verification

9. **Regression check — higher-level tests**
   Full re-run of docs/higher_level_tests.md packages (NeoJSON, Mustache,
   XMLParser, PolyMath, DataFrame) after the T2 fix.

10. **Regression check — SUnit Kernel-Tests**
    Full 64-class GUI/Spec batch + hash-collections batch, verify no
    regressions from f279fd4.

## iOS

11. **iOS device testing** (project's actual mission; deferred during
    JIT work)
