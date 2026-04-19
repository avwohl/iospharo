# Deferred Items (consolidated)

Single source of truth for currently-open work.  Everything that
shipped is in `docs/changes.md`; this file only lists what's *not*
done.

---

## A. Deferred test-suite issues — accepted

Documented in `docs/deferred-issues.md`.  No VM-side fix in scope;
each is either a harness artifact, a known-slow class, or a
test-framework retention pattern.

### A1. Harness SemaphoreTest / valueWithin timing
Standalone runs pass 100%.  Failures only in SUnit fork-based
harness due to GC pauses (140-300 ms) during P40 test activations.
**Owner:** harness submodule (`scripts/pharo-headless-test`).

### A2. Reflection-walk timeouts
`testFastPointersTo` and `testPointersToCycle` now pass under JIT.
`testPointersTo` still >60 s due to O(heap×N) traversal.
Reclassified as known-slow.

### A3. Weak-reference / finalization timing
4 residuals; all test-framework retention during
`Smalltalk garbageCollect`.  Not VM bugs.

### A4. JIT eval-mode MAX=50+ hang
`PHARO_JIT_DEFER=4` default works.  `PHARO_JIT_DEFER=0` still hangs
during Morphic boot — scheduling, not correctness.

### A5. B5 cold-IC DNU cascade at PHARO_JIT_DEFER=0
At `PHARO_JIT_DEFER=0`, JIT compiles startup-path methods
immediately and corrupts the J2J return stack on the
`PositionableStream class>>on:` → `PositionableStream>>on:` chain,
leaving `aCollection size` (SmallInt) where the stream instance
should be.  Full narrowing + diagnostic tooling is in tree
(commits f70ad55, cf6ffaf, b94c0a8 — `_HOLE_RT_J2J_TRACE`, ring
buffer, auto-trigger).  Root cause not pinpointed; fix requires
lldb single-stepping through `stencil_popStoreTemp` at
`decodeBytes:` bc[2] on the buggy iteration.

**Mitigation:** default `PHARO_JIT_DEFER=4s` already sidesteps it;
B5 only impacts aggressive `PHARO_JIT_DEFER=0`, which isn't a
supported default.  Not blocking user-visible behavior.  See
`memory/project_b5_j2j_onchain.md` for full plan.

---

## B. Code state in-tree — experimental / opt-in

### B1. `PHARO_T2=0` (default off)
T2 never demonstrably wins on any measured bench.  Kept default-off
until a workload where T2 beats T1 appears.  With the §1.3c
coexist default, `PHARO_T2=1` is safe but also largely dormant on
typical benches.

### B2. `PHARO_T2_UNSAFE_CALLEE=1`
Restores the pre-`f279fd4` buggy callee-invocation path
(double-execution bug) for benchmarking.  Keep as a flag; do not
remove.

### B3. `PHARO_RESUME_J2J=1`
External J2J trampoline in resume path.  Works but 18% slower;
opt-in only.  See `memory/project_resume_j2j_trampoline.md`.

### B4. `PHARO_JIT_SIMSTACK=1`
SimStack TOS/NOS register caching.  Default-flip attempted
2026-04-18, reverted same day (12 IntegerTest regressions that
don't reproduce in isolation).  Root cause open.

### B5. `PHARO_T2_A1=1` — T2 chain-loop continuation
Implemented but dormant.  A1's callee-invocation speedup is only
observable when T2 actually executes — under coexist default, T2
rarely does.  Would need a workload where T2 wins to justify
re-examining.

### B6. T2 diagnostic env vars (keep)
`PHARO_T2_BAIL_OP`, `PHARO_T2_NO_ARITH_FAST`,
`PHARO_T2_NO_ARITH_OPS`, `PHARO_T2_OPT`, `PHARO_T2_TRACE`,
`T2_LIMIT`, `T2_VERBOSE`, `PHARO_T2_MBC_JUMPS`,
`PHARO_T2_MBC_SENDS`, `PHARO_T2_MBC_IC`, `PHARO_T2_WARMUP`,
`PHARO_JIT_NO_SIMSTACK`.  All cheap when not enabled.  (Legacy
`PHARO_DUMP_MIR` removed when MIR was deleted 2026-04-17.)

All `PHARO_*` / `JIT_*` / `T2_*` knobs are now declared in one
place — `src/vm/DebugSettings.hpp` — so future audits can grep
there instead of scanning every source file.

---

## C. Project mission — iOS

The project's purpose is an iOS Pharo VM.  JIT work has dominated
recent sessions; iOS proper is what's queued next.

### C1. iOS device testing
Mac Catalyst is verified working (2026-02-24).  iOS Device
xcframework slice builds as of 2026-04-19 (commit `22bcc2c`).
Device testing still needs:

- Physical iOS device(s) for build verification.
- Apple Developer signing cert setup.
- TestFlight or direct-device deploy.
- UI touch/pinch/pan exercised end-to-end (not just Mac mouse
  events).

### C2. iOS app-store readiness (remaining items)
Shipped 2026-04-19 (see `changes.md`): privacy manifest, launch
screen, scoped ATS.  Still open:

- Remote logging for device debugging — needs a backend choice
  (Sentry / Firebase / custom endpoint).  Apple's built-in crash
  reporter handles crashes; this is for non-crash diagnostic logs.

### C3. Image preparation
- Do iOS images need different startup?  Currently uses standard
  Pharo images with `startup.st` injection — verify behavior on
  real hardware.
- Touch-based Morphic input path — `docs/image_issues.md` has the
  wishlist for portrait layout + touch primitives as upstream
  proposals.

---

## D. Upstream proposals

`docs/upstream-proposals.md` tracks wishlist items for the Pharo
image that would make iOS work cleaner.  Out of process from VM
changes; image-side issues to propose upstream.

- Portrait-aware layout in Morphic.
- Touch event primitives on the standard input path.
- Startup preferences path that survives
  `Smalltalk snapshot:andQuit:` round-trips cleanly.

---

## E. Remaining JIT work

1. **Architectural T1/T2 interaction (§1.3).**  T2 intercepting
   methods still breaks T1's inline-IC warmup in the non-coexist
   (REPLACE=1) path — neither shared-IC, warmup delay, nor
   self-only narrowing has fully solved this.  Coexist default
   sidesteps it by not replacing T1 at all.  Full fix — if ever
   needed — is a design rethink (shared IC table across tiers?
   patch-T1-when-T2-compiles?).
2. **Multi-bc §1.2e block activation** (`PushFullBlock` / `PushClosure`).
   Uses the existing `ExitBlockCreate` chain-loop path.  Enables
   non-inlined blocks to be T2-compiled; marginal benefit since
   Pharo inlines `to:do:` / `whileTrue:` at compile time.
3. **Pivot to iOS device work (C).**  Needs physical hardware, not
   just code.

Once §1.3 and §1.2e are sorted, JIT reaches diminishing returns
and C is where project value lands.
