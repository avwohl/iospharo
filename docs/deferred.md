# Deferred Items (consolidated)

Single source of truth for everything deferred after session 23.
Items are grouped by whether they're code to write, code to keep
as-is, or project-mission work.

---

## A. JIT performance — code to write

### A1. T2 chain-loop continuation — **IMPLEMENTED, GATED**
**Design:** `docs/jit-t2-chainloop-plan.md`.
**Status:** Implemented 2026-04-16. Gated behind `PHARO_T2_A1=1`.
Under coexist default (2026-04-18), T2 rarely executes anyway, so
A1's behavior difference is moot unless PHARO_T2_REPLACE=1.
**Effort to unblock:** Need a demonstrable T2 win (bench where T2
beats T1 ignoring send cost) to justify re-examining A1.
**Value:** Restores T2's callee-invocation speedup (lost in
f279fd4).  Not observed on current benches.

### A2. T1 J2J memory-op reduction — **LANDED** (commit 415d899)
**Design:** `docs/jit-j2j-reduction-plan.md`.
**Status:** Shipped 2026-04-16.  J2JSave struct went 72→56 bytes, 3
stores dropped per send; ASM+stencils+Interpreter materialization all
updated in lockstep.
**Value achieved:** Core intent landed; benchmarks pending next full
run.  Helps T1 send-heavy workloads.

### A3. IC hit rate investigation — **RESOLVED (2026-04-18)**
**Analysis:** `memory/project_ic_hit_rate_investigation.md` +
`memory/project_ic_selbits_mystery.md` (both historical).
**Final status:** `noSelBits=0` on every workload reproduced in
this session — startup, short loops, mixed send workloads.  IC hit
rate 97.5% on startup-dominated runs (5697/5845).  See also
`docs/todo.md` §2.4 (RESOLVED 2026-04-18) which reached the same
conclusion via the yourself-loop measurement.
**Root-cause review:** the `_HOLE_OPERAND2` GOT-indirect reloc
pattern (`adrp+ldr` → `x1 = *poolSlot = icBase`) is correct by
construction; `icData[18]` at runtime reads `icBase + 144`, which
is where the compiler writes `selectorBits` at
`JITCompiler.cpp:1957`.
**Historical 74%/100% observations** came from specific benches
(AWFY Permute, array-fill) that no longer reproduce.  Not worth
chasing without a live reproducer, and the fix ceiling was
already marginal (89% → 94% IC hit, ~5% on the marginal workload).
**Closed.

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

### B5. JIT `PHARO_JIT_DEFER=0` eager-compile DNU cascade
At `PHARO_JIT_DEFER=0`, JIT compiles startup-path methods immediately
and hits multiple DNUs during `SmalltalkImage>>isInteractiveGraphic`:
- `#atEnd` on SmallInteger inside `ZnUTF8Encoder>>decodeBytes:`
- `#do:` not understood on `Symbol class` in `#nextPutAll:`
- `#isNumber` not understood on a SmallFloat in `#=`

**Root cause (2026-04-18):** cold-IC JIT compilation of the
`PositionableStream class>>on:` → `PositionableStream>>on:` chain
corrupts the J2J return stack, leaving `aCollection size`
(SmallInt) where the stream instance should be.  The wrong SmallInt
gets assigned to `byteStream` by `decodeBytes:` bc[2]
`popStoreTemp 1`.

**Narrowing (exhaustive bisection):**
- `PHARO_JIT_SKIP_SELECTORS='on:'` → gone.  Individual
  `basicNew`/`size`/`reset`/`readStream`/`isHeadless` don't help.
- `PHARO_J2J_SKIP_SELECTORS='on:'` → gone.
- `PHARO_JIT_NO_BLOCKS=1` → still fires (not a CompiledBlock bug).
- `PHARO_NO_EAGER_COMPILE=1` → still fires.
- `PHARO_JIT_THRESHOLD=100` → gone.  Confirms it's a COLD-IC bug.
- `PHARO_JIT_DEFER=1` (1s warm-up) → gone.  Default is 4s.

**Why deferral fixes it:** interpreter-only pre-warming populates
ICs via `patchJITICAfterSend` + mega cache.  When JIT finally
compiles the methods, ICs already have filled slots; the
IC-hit path runs on first JIT invocation, bypassing the
corruption in the cold ic_miss / J2J-init path.

**Session 2026-04-19 progress:**
- `_HOLE_RT_J2J_TRACE` tooling shipped (commits f70ad55, cf6ffaf,
  b94c0a8).  Save-push + return trace events, 512-event ring
  buffer, auto-trigger on SmallInt return to focus methods, DNU
  handler auto-dumps ring.
- Confirmed: `[B5-RESUME]` shows readStream correctly returns a
  ReadStream for the first 2-4 iterations — interp return handling
  is fine.  Bug kicks in on the 3rd-5th call when J2J takes the
  full-chain fast path.
- NO J2J_INLINE_RETURN events fire with callerCM in the focus set
  (decodeBytes:, readStream, on:(class), on:(inst)).  The J2J chain
  bails mid-flight to interpreter, orphaning saves and leaving
  byteStream = SmallInt(aCollection size) instead of the stream.
- `jit_rt_array_prim` size prim NEVER sees sizes matching the bug
  values — so the inline size primitive isn't directly the cause.
- Disabling new-prim / size-prim inline via env knobs sometimes
  masks, sometimes not (timing-dependent).

**Still not fixed.**  Narrowing complete; fix requires deeper
runtime debugging (lldb batch script, single-step through
stencil_popStoreTemp at decodeBytes bc[2] on the buggy iteration).
See `memory/project_b5_j2j_onchain.md` for full analysis + plan.

**Mitigation:** default `PHARO_JIT_DEFER=4s` already sidesteps it
entirely.  B5 only impacts aggressive `PHARO_JIT_DEFER=0` path,
which isn't a supported default.  Not blocking any user-visible
behavior.

---

## C. Code state in-tree — experimental / opt-in

### C1. `PHARO_T2=0` (default off)
T2 is off by default.  Current rationale (2026-04-18):
- T2 never demonstrably wins on any measured bench.  Without a
  workload-level reason to enable, the default stays off.
- With coexist default (see §1.3c), `PHARO_T2=1` is safe — T2
  doesn't intercept T1's hot path — but also largely dormant
  (compiles nothing on typical benches because T1 handles every
  method).  So flipping the default is a no-op semantically.
- `PHARO_T2_REPLACE=1` restores "T2 takes over" behavior; with
  `PHARO_T2_MBC_IC=1` the shared-IC side-table (§1.3a) keeps IC
  at ~88%.  Still no bench win, just no regression.
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

**Post 2026-04-18 update:**  A1, A2, A3, SimStack re-enable,
and multi-bc jumps (forward + backward) have all shipped or closed.
MIR was removed in 2026-04-17; T2 now runs on asmjit with no GC
staleness concerns.  Remaining practical work:

1. **Architectural T1/T2 interaction (§1.3).**  T2 intercepting
   methods still breaks T1's inline-IC warmup — neither shared-IC,
   warmup delay, nor self-only narrowing has solved this.  The
   fundamental issue gates §1.2a (sends in multi-bc), §1.2f
   (inline IC at multi-bc send sites), and the regression path
   that forced A1 off.  Needs a design rethink (shared IC table
   across T1/T2? patch-T1-when-T2-compiles?).
2. **Multi-bc §1.2e block activation** (0xF9/0xFA PushFullBlock/
   PushClosure).  Uses the existing `ExitBlockCreate` mechanism
   (the chain loop already handles it).  Enables non-inlined
   blocks to be T2-compiled; marginal benefit since Pharo inlines
   to:do:/whileTrue: at compile time.
3. **B3 eval-mode fix shipped (2026-04-18).**  `test_load_image eval`
   works end-to-end at default settings (JIT on, PHARO_JIT_DEFER=4s).
   Root cause was a JIT deopt bug for `PushThisContext` — the
   resume IP skipped the preceding `ExtB 1` prefix, so the
   interpreter pushed `thisContext` instead of `thisProcess` on
   the `DebugSession>>exception` path.  Fix: `bc.operand =
   firstExtBCOffset` (JITCompiler.cpp:175).
   Remaining: B5 eager-compile DNU cascade at `PHARO_JIT_DEFER=0`.
4. **Pivot to D (iOS device work).** Needs physical hardware, not
   just code.

Once §1.3 is sorted, JIT reaches diminishing returns and D is
where the project's actual value delivery happens.
