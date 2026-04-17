# Deferred Items (consolidated)

Single source of truth for everything deferred after session 23.
Items are grouped by whether they're code to write, code to keep
as-is, or project-mission work.

---

## A. JIT performance — code to write

### A1. T2 chain-loop continuation
**Design:** `docs/jit-t2-chainloop-plan.md` (commit f2e3f0f).
**Status:** Design-only. Not implemented.
**Effort:** 1-2 days.
**Value:** Restores T2's callee-invocation speedup (lost in f279fd4 safety fix). Target: T2 on send-heavy AWFY beats T1.

### A2. T1 J2J memory-op reduction
**Design:** `docs/jit-j2j-reduction-plan.md` (commit cb145f3).
**Status:** Design-only. Not implemented.
**Effort:** 4-6 hours, 5-file coordinated change.
**Value:** ~7% per-send improvement. J2JSave shrinks 72→56 bytes.

### A3. IC hit rate investigation — **DIAGNOSTIC LANDED, FIX DEFERRED**
**Analysis:** `memory/project_ic_hit_rate_investigation.md` +
`memory/project_ic_selbits_mystery.md`.
**Status:** Miss-breakdown counters landed (commit b381525). They
expose a real bug: 100% of miss events report `noSelBits` (icData[18]
is 0 at runtime). The compile-time write at JITCompiler.cpp:1926 goes
to a DIFFERENT address than what the stencil reads at runtime — only
33 compile-time icBase addresses, and the ~4 runtime icData pointers
sampled don't match any of them. Root cause: `_HOLE_OPERAND2` likely
resolves to the pool-slot address (one level of indirection wrong).
**Effort:** ~half day to fix properly — need to inspect the actual
patched ARM64 code and reconcile the compile-time / runtime icBase
understanding. Unblocks the megacache fast path.
**Value:** High — likely ~90% IC hit rate after fix (vs current 50%).

---

## B. Deferred test-suite issues — accepted

These are documented in `docs/deferred-issues.md`. No VM-side fix in
scope; each is either a harness artifact, a known-slow class, or a
test-framework retention pattern.

### B1. #1 Harness SemaphoreTest / valueWithin timing
Standalone runs pass 100%. Failures only in SUnit fork-based harness
due to GC pauses (140-300 ms) during P40 test activations.
**Owner:** harness submodule (`scripts/pharo-headless-test`).

### B2. #2 Reflection-walk timeouts
`testFastPointersTo` and `testPointersToCycle` now pass under JIT.
`testPointersTo` still >60s due to O(heap×N) traversal. Reclassified
as known-slow.

### B3. #3 Weak-reference / finalization timing
4 residuals; all test-framework retention during
`Smalltalk garbageCollect`. Not VM bugs.

### B4. #4 JIT eval-mode MAX=50+ hang
`PHARO_JIT_DEFER=4` default works. `PHARO_JIT_DEFER=0` still hangs
during Morphic boot — scheduling, not correctness.

---

## C. Code state in-tree — experimental / opt-in

### C1. `PHARO_T2=0` (default off)
T2 is off by default. Reasons:
- Historical: `b18e71e` cited GC register staleness. Session 23 stress
  tests ran through without crashes (see
  `memory/project_t2_gc_safety_revalidated.md`) — this reason is
  probably stale.
- Current: after `f279fd4`, T2 always bails on sends. Perf ≈ T1 (no
  longer faster). Until A1 (chain-loop) lands, no reason to flip
  the default.

### C2. `PHARO_T2_UNSAFE_CALLEE=1`
Restores the pre-`f279fd4` buggy callee-invocation path (double-
execution bug) for benchmarking. Keep as a flag; do not remove.

### C3. `PHARO_RESUME_J2J=1`
External J2J trampoline in resume path. Works but 18% slower; opt-in
only. See `memory/project_resume_j2j_trampoline.md`.

### C4. T2 diagnostic env vars (keep)
`PHARO_T2_BAIL_OP`, `PHARO_T2_NO_ARITH_FAST`,
`PHARO_T2_NO_ARITH_OPS`, `PHARO_T2_OPT`, `PHARO_DUMP_MIR`,
`PHARO_T2_TRACE`, `T2_LIMIT`, `T2_VERBOSE`. Session 22/23 bisect
tooling. All cheap when not enabled. Keep.

---

## D. Project mission — out of scope for recent sessions

The project's actual purpose is an iOS Pharo VM. JIT work has
dominated recent sessions; iOS proper is what's queued next.

### D1. iOS device testing
Mac Catalyst is verified working (2026-02-24). Device testing needs:
- Physical iOS device(s) for build verification
- Apple Developer signing cert setup
- TestFlight or direct-device deploy
- UI touch/pinch/pan exercised end-to-end (not just Mac mouse events)

### D2. iOS app-store readiness
- App icons, launch screen, metadata
- Privacy manifest (iOS 17+ requirement)
- Crash reporting integration
- Remote logging for device debugging (can't tail -f a device easily)

### D3. Image preparation
- Do iOS images need different startup? Currently uses standard Pharo
  images with `startup.st` injection. Verify behavior on real device.
- Touch-based Morphic input path (`docs/image_issues.md` has wishlist
  for portrait layout + touch upstream proposals).

---

## E. Upstream proposals

`docs/upstream-proposals.md` tracks wishlist items for the Pharo image
that would make iOS work cleaner. These are out-of-process from VM
changes; they're image-side issues to propose upstream.

- Portrait-aware layout in Morphic
- Touch event primitives on the standard input path
- Startup preferences path that survives
  `Smalltalk snapshot:andQuit:` round-trips cleanly

---

## F. What's left for a future session

Practical next-session starting points, in suggested order:

1. **Implement A3 (IC hit-rate counter).** Cheap, informative. Likely
   reveals a real fixable problem.
2. **Implement A2 (T1 J2J reduction).** Concrete, bounded, high-
   confidence win on send-heavy AWFY.
3. **Implement A1 (T2 chain-loop).** Biggest perf unlock but riskiest
   and most invasive.
4. **Pivot to D (iOS device work).** Needs physical hardware, not
   just code.

Once A1/A2/A3 are all done, JIT reaches diminishing returns and D is
where the project's actual value delivery happens.
