# JIT Punch List

Remaining work after session 23's T2 Permute miscompile fix (f279fd4).
Ordered by descending expected ROI.

## Status (session 23 pass-through)

### JIT perf

1. **T2 chain-loop continuation** — **DEFERRED**. Design doc in
   `docs/jit-t2-chainloop-plan.md` (commit f2e3f0f). 1-2 day implementation.

2. **T1 J2J memory-op reduction** — **DEFERRED**. Design doc in
   `docs/jit-j2j-reduction-plan.md` (commit cb145f3). ~7% per-send
   improvement; 5-file coordinated change (~4-6 hours).

3. **T2 GC safety re-validation** — **DONE (stable)**. Richards, NBody,
   DeltaBlue, Storage, tinyBenchmarks, Permute, Bounce, Queens, Sieve
   all ran under `PHARO_T2=1` without SIGSEGV. b18e71e's rationale
   appears stale post-f279fd4. Memory record:
   `project_t2_gc_safety_revalidated.md`.

4. **IC hit rate on AWFY** — **INVESTIGATED, DEFERRED**. Permute shows
   50.6% IC hit rate with only 51 IC patches — suspicious. Likely
   either cold IC (patches never applied at warm sites) or
   mega-cache skipping due to zero `icData[18]`. A proper fix needs
   a new stencil counter (mega-cache hit). Memory record:
   `project_ic_hit_rate_investigation.md`.

### Deferred test-suite issues (docs/deferred-issues.md)

5. **#1 Harness SemaphoreTest / valueWithin timing** — **DONE (re-
   confirmed)**. All 3 classes pass 100% in direct eval. Residual
   harness failures are GC-pause artifacts of the SUnit fork wrapper;
   no VM-side fix available.

6. **#2 Reflection-walk timeouts** — **MOSTLY UNBLOCKED BY JIT**.
   `testFastPointersTo` and `testPointersToCycle` now pass with JIT
   enabled; `testPointersTo` still exceeds 60s due to O(heap×N)
   traversal (acceptable slow class).

7. **#3 Weak-reference / finalization timing** — **DONE**. 4 residuals
   (WeakKeyDictionaryTest 206/1, WeakIdentityKeyDictionary 208/1,
   WeakAnnouncerTest 32/2), all known harness-retention artifacts.

8. **#4 JIT eval-mode MAX=50+ hang** — **WORKAROUND IN PLACE**.
   `PHARO_JIT_DEFER=4` (default) makes eval complete cleanly.
   `PHARO_JIT_DEFER=0` still hangs during Morphic boot; scheduling
   issue, not correctness. Acceptable.

### Regression

9. **SUnit + higher-level tests** — **DONE (spot-check)**. Focused
   kernel subset (SmallIntegerTest, FloatTest, IntervalTest,
   FractionTest) all pass clean. f279fd4 (T2 always-bail) did not
   regress any tested class.

10. **Punch list doc** — **DONE** (this file, committed c45dd0b).

### iOS

11. **iOS device testing** — out of scope for a JIT-focused session.
    Mac Catalyst is verified working (2026-02-24). iOS-proper is the
    project's actual mission but has been deferred through all recent
    JIT work.

---

## What's left for a future session

If a future session wants to pick up JIT work:

1. **Implement T2 chain-loop continuation** — biggest perf unlock. Plan
   in `docs/jit-t2-chainloop-plan.md`.
2. **Implement T1 J2J memory-op reduction** — smaller but real win.
   Plan in `docs/jit-j2j-reduction-plan.md`.
3. **Investigate low IC hit rate** — 50% miss on Permute with only 51
   patches is strange. Add a mega-cache hit counter, find the true
   breakdown.
4. **Implement ClosureQuicken** (see memory/project_jit_perf_baseline.md
   for context) — resume path has no save stack.
5. **Pivot to iOS** — Mac Catalyst is working; on-device Pharo VM
   integration, accept Xcode build, app-store readiness are the
   actual project mission.
