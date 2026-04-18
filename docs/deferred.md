# Deferred Items (consolidated)

Single source of truth for everything deferred after session 23.
Items are grouped by whether they're code to write, code to keep
as-is, or project-mission work.

---

## A. JIT performance — code to write

### A1. T2 chain-loop continuation — **IMPLEMENTED, GATED**
**Design:** `docs/jit-t2-chainloop-plan.md`.
**Status:** Implemented 2026-04-16. Gated behind `PHARO_T2_A1=1`.
Default is still always-bail (f279fd4) because T2 itself is disabled
(b18e71e, stale oops across GC).
**Effort to unblock:** Fix T2 GC oops issue first (separate bug).
**Value:** Restores T2's callee-invocation speedup (lost in f279fd4 safety fix). Target: T2 on send-heavy AWFY beats T1.

### A2. T1 J2J memory-op reduction — **LANDED** (commit 415d899)
**Design:** `docs/jit-j2j-reduction-plan.md`.
**Status:** Shipped 2026-04-16.  J2JSave struct went 72→56 bytes, 3
stores dropped per send; ASM+stencils+Interpreter materialization all
updated in lockstep.
**Value achieved:** Core intent landed; benchmarks pending next full
run.  Helps T1 send-heavy workloads.

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
T2 is off by default.  Current rationale (2026-04-18):
- `PHARO_T2=1` regresses IC hit rate 90.8% → 81.9% on array-fill
  without affecting raw bench time (22ms both ways).  The IC
  regression is the §1.3 T1/T2 interaction issue — T2 intercepts
  methods before T1 warms its IC.
- Until §1.3 is resolved, flipping the default saves nothing and
  costs IC hit rate on send-heavy workloads.
- Historical b18e71e "GC register staleness" no longer applies
  (MIR removed 2026-04-17).

### C2. `PHARO_T2_UNSAFE_CALLEE=1`
Restores the pre-`f279fd4` buggy callee-invocation path (double-
execution bug) for benchmarking. Keep as a flag; do not remove.

### C3. `PHARO_RESUME_J2J=1`
External J2J trampoline in resume path. Works but 18% slower; opt-in
only. See `memory/project_resume_j2j_trampoline.md`.

### C4. T2 diagnostic env vars (keep)
`PHARO_T2_BAIL_OP`, `PHARO_T2_NO_ARITH_FAST`,
`PHARO_T2_NO_ARITH_OPS`, `PHARO_T2_OPT`, `PHARO_T2_TRACE`,
`T2_LIMIT`, `T2_VERBOSE`, `PHARO_T2_MBC_JUMPS`,
`PHARO_T2_MBC_SENDS`, `PHARO_T2_MBC_IC`, `PHARO_T2_WARMUP`,
`PHARO_T2_A1`, `PHARO_JIT_NO_SIMSTACK`.  All cheap when not
enabled.  (Legacy `PHARO_DUMP_MIR` removed when MIR was deleted
2026-04-17.)  Keep the rest.

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

**Post 2026-04-18 update:**  A1, A2, A3 diag, SimStack re-enable,
and multi-bc jumps (forward + backward) have all shipped.  MIR was
removed in 2026-04-17; T2 now runs on asmjit with no GC staleness
concerns.  Remaining practical work:

1. **Architectural T1/T2 interaction (§1.3).**  T2 intercepting
   methods still breaks T1's inline-IC warmup — neither shared-IC,
   warmup delay, nor self-only narrowing has solved this.  The
   fundamental issue gates §1.2a (sends in multi-bc), §1.2f
   (inline IC at multi-bc send sites), and the regression path
   that forced A1 off.  Needs a design rethink (shared IC table
   across T1/T2? patch-T1-when-T2-compiles?).
2. **Finish A3 (IC hit-rate).** Diagnostics shipped, actual fix needs
   lldb watchpoint session — something clears `icData[18]` between
   compile and first `ExitSend`, invisible to static analysis.
3. **Multi-bc §1.2e block activation** (0xF9/0xFA PushFullBlock/
   PushClosure).  Uses the existing `ExitBlockCreate` mechanism
   (the chain loop already handles it).  Enables non-inlined
   blocks to be T2-compiled; marginal benefit since Pharo inlines
   to:do:/whileTrue: at compile time.
4. **Pivot to D (iOS device work).** Needs physical hardware, not
   just code.

Once §1.3 is sorted + A3 fixed, JIT reaches diminishing returns
and D is where the project's actual value delivery happens.
