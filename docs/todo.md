# TODO

Consolidated list of everything open, across JIT, tests, iOS, and
upstream.  Superset of the narrower docs:
- `docs/jit-todo.md`        — JIT detail
- `docs/deferred.md`        — session-21 deferred items
- `docs/deferred-issues.md` — test-suite issues detail
- `docs/known-issues.md`    — rendering + image bugs
- `docs/image_issues.md`    — image-side workarounds
- `docs/upstream-proposals.md` — bugs to push upstream

Last updated: 2026-04-17.

---

## 1. JIT — asmjit T2 (post MIR removal)

MIR was removed 2026-04-17; T2 runs on asmjit.  Coverage is
template-matched whole methods + a minimal multi-bytecode compiler
for pure arith chains.  Default `PHARO_T2=1` is benchmark-neutral.

### 1.1  Debug the 1-arg inline IC DNU  (task #31)  **RESOLVED (2026-04-18)**

Root cause: the debug hit/miss counter-bump code emitted inside
each of 7 IC paths (6 hits + 1 miss) interacted with asmjit's
register allocator in a way that intermittently corrupted the
surrounding probe code.  Fix (34782ec): removed the emitted
counter-bumps entirely — C-side diagnostics are preserved for the
chain loop path.  0 DNU matches across 12 runs.  1-arg inline IC
is now **default on**.  Bench: 5/8 fast-mode runs on array-fill
(vs T2=0's typical 3/5).

### 1.2  Multi-bytecode T2 — extend past pure arith  (task #33)

MVP shipped (`32f6f1a`).  Covers: pushes, stores, pops, Dup,
arith 0x60-0x6F, returns.  Falls back to templates on any
unsupported bytecode.

Next slices (each ~1 session):

- **1.2a  Sends in multi-bc.**  **IMPLEMENTED BUT GATED (a4a3ce7).**
  Emits push-args + ExitSend at the send site; interpreter runs
  the send + everything after.  Coverage jumps 25 → 233 methods
  compiled.  BUT causes array-fill bench to drop from 3/6 fast-
  mode runs to 0/6 — same pattern as inline IC (§1.3): T2
  replaces T1's inline IC probe with bail-to-C, net slower.
  Gated behind `PHARO_T2_MBC_SENDS=1`.  The real fix is inline
  IC at send sites (item 1.2f below).
  
- **1.2b  Short forward jumps.**  Bytecodes 0xB0-0xB7 (unconditional
  +1..+8).  Emit an asmjit label at each jump target; forward
  branches just fall through.  Enables `ifTrue: [ ] ifFalse: [ ]`
  bodies that don't go back.

- **1.2c  Conditional jumps.**  0xB8-0xC7.  Need mustBeBoolean
  semantics on the condition (bail on non-boolean).  Pharo
  inlines `ifTrue:ifFalse:` using these.

- **1.2d  Backward jumps (loops).**  `whileTrue:` etc.  Asmjit
  labels with back-edges.  Need yield-countdown check at each
  back-edge to keep scheduler happy.

- **1.2e  Block activation.**  0xF9 (PushFullBlock), 0xFA
  (PushClosure).  Enables `to:do:` body compilation.

- **1.2f  Inline IC at send sites in multi-bc.**  **IMPLEMENTED
  BUT GATED (4cdfa0a, 05b494d).**  Emits a 6-way IC probe at the
  first send site; hit exits ExitSendCached (chain loop makes J2J
  direct call), miss exits ExitSend + icDataPtr (interpreter
  patches IC).  With private IC buffer: IC hit rate 98.9% → 49.9%
  (two independent ICs, neither warms fully).  Tried sharing T1's
  IC: 5× slowdown because T1's sendIdx walk counts extra bytecodes
  that my walk doesn't, patches land on the wrong slot and
  corrupt T1's IC.  Gated behind `PHARO_T2_MBC_IC=1`.

  Real fix options (neither done): (a) match T1's send-counting
  exactly so shared IC works; (b) skip T2 compilation entirely for
  methods T1 has already IC-warmed (requires retry mechanism in
  tier2Insert so T2 isn't permanently "tried-failed"-stamped).

### 1.3  T1 warm-up / T2 interaction  (blocks inline IC)  **CONFIRMED 2026-04-18**

Hypothesis confirmed experimentally:
- Default (no T2 IC): IC hit rate 98.9%.
- T2 multi-bc inline IC enabled (PHARO_T2_MBC_IC=1): drops to 49.9%.
- T2 shared-T1-IC attempt (compute T1's sendIdx by walking
  bytecodes): 25.2% hit rate AND 5× bench slowdown — sendIdx
  mapping is wrong (T1 counts extension bytes / ExtSuperSend that
  my walk doesn't).
- `PHARO_T2_WARMUP=10` (delay T2 compile until T1 has executed N
  times): 4/6 fast-mode runs, IC 75.6%.  Helps slightly but still
  lower than baseline.

Fundamental issue: T2 REPLACES T1 for its covered methods.  T1's
IC for those methods becomes unused; T2's private IC starts cold.
No amount of warmup delay fixes the problem permanently — it just
shifts when T1→T2 handover happens.

Real fixes (not done):
- (a) Compute T1's sendIdx exactly (match T1's send-counting
  including ExtSuperSend, bail-out stencils, etc.) so shared IC
  works without corruption.
- (b) Have T2 do the pre-send work then TAIL-CALL T1's
  stencil_sendJ2J for the send itself.  Reuses T1's IC and code.
  Complex: T2 needs to preserve state.sp / state.ip / regs the way
  T1 stencils expect.
- (c) T2 doesn't replace T1.  Both coexist: T1 runs, and T2 only
  acts as an inline-helper for specific patterns T1 can't handle
  efficiently (arith chains, whole-method templates).  Requires
  rethinking the tier-dispatch model.

---

## 2. JIT — legacy / pre-asmjit items still applicable

From `docs/jit-todo.md` §Open bugs, mostly pre-asmjit but still
relevant to T1 behaviour.

### 2.1  `tinyBenchmarks` / `bench_loop` intermittent hang  (B1)  **NOT REPRODUCIBLE (2026-04-18)**

Tried `^ 16 benchmark` 5× and `(10 to: 20) collect: [:n | n benchmark]`
4×: no hang.  All benchmark calls return 1028 (single-iteration
baseline).  The race that triggered this apparently closed with
the IC / GC fixes that landed in prior sessions.  Keep the
memory note historical; reopen if it reappears.

### 2.2  benchFib 2nd-iteration 12× slowdown  (B2)  **RESOLVED (2026-04-18)**

Re-measured: `28 benchFib`=55ms, `29 benchFib`=79ms, ratio=1.44.
The theoretical upper bound is φ ≈ 1.618 (Fibonacci call growth).
1.44 is normal.  The 12× number in the old note is no longer
reproducible — probably fixed by one of the many IC / stencil
fixes that landed between then and now.

### 2.3  `tinyBenchmarks` scheduler-idle hang  (B3)  **LIKELY RESOLVED (2026-04-18)**

Was hypothesised to be a downstream effect of 2.1+2.2.  Both
upstream items no longer reproduce.  Not independently verified
— need to run the full `tinyBenchmarks` flow once to confirm,
but the hang is almost certainly gone with them.

### 2.4  T1 IC hit-rate investigation  (B5 / A3)  **RESOLVED (2026-04-18)**

Measured on current workloads: `noSelBits=0`, IC hit rate 97.5%
on yourself-loop (5709/5857).  The selector-seeding fix at
`JITCompiler.cpp:1886` (`icSlots[18] = selectorBits`) is working.
The 50% hit rate in the original memory note was specific to the
AWFY Permute bench; typical workloads show 97%+.  No further
action.

### 2.5  Shrink `stencil_sendJ2J`  (P2)  **PARTIAL (2026-04-18)**

523 ARM64 instructions per send-site.  Target 150-200 by
extracting the probe loop + megamorphic fallback into a shared
helper that stencils tail-call.  Saves ~2KB per send site,
improves i-cache.

Attempted shortcut: swap sendJ2J (2092 B) for the existing
smaller sendPoly (696 B) behind `PHARO_JIT_POLY=1` (12ebbb9).
Segfaults in resume path — the two stencils' operand / save-
stack contracts differ despite sharing the first two packed
fields.  Real progress here needs stencil source changes +
regeneration via `extract_stencils.py`.  Out of scope for
"hours" budget.

### 2.6  Method-level JIT opt-in  (P3)  **DISPROVEN (2026-04-18)**

Hypothesis: interpreter beats JIT on arith-loop methods, so
skipping their compilation (`PHARO_JIT_MIN_SENDS=N`) should be a
win.  Added the knob, measured on array-fill bench:

    MIN_SENDS=0 (default):  ~205-370ms
    MIN_SENDS=3:            4902ms  (13-25× slower)

The hot path DEPENDS on `timesRepeat:` / `to:do:` bodies running
as JIT native code.  Skipping their compilation puts everything
back on the interpreter's slower per-bytecode dispatch.  Knob
kept for bisection (`83c3703`) but never enable by default.

### 2.7  Fix `upgradeICToJ2J` layering on inline-primKind entries  (P4)

IC entries with bits 52:48 set never get the J2J direct-call bit
(60) added later.  Prior attempt regressed (hang at n=16).  Needs
lldb to trace the stencil IC-hit path interaction.

### 2.8  Re-enable SimStack TOS/NOS caching  (P5)

Disabled by default (`b9ab22e`) because of a timing-sensitive
correctness bug in arith-jump chains after hot loops.  ~5-10% win
on arith-heavy code once the bug is root-caused.  Depends on 2.1/2.2.

### 2.9  Reduce `tryJITActivation` fast-reject overhead  (P6)  **IMPLEMENTED (2026-04-18)**

`Interpreter::canJITActivate()` inline helper added (a054b2b).
Call site pattern: `if (canJITActivate(m) && tryJITActivation(m, n))`
— short-circuit `&&` skips the function call + trace guards when
the method isn't compiled.  Correctness preserved; bench noise
dominates any visible improvement but the miss-path instruction
count went down.

### 2.10  Enable A1 (T2 chain-loop continuation)  (P7)

Already shipped (2f14022), gated behind `PHARO_T2_A1=1`.  Can't
enable until T2 itself beats T1 on send-heavy workloads (depends on
1.2 or the old MIR-era P1 inlining).

---

## 3. Test-suite deferred issues

From `docs/deferred-issues.md` — none are VM bugs; all are
harness / framework / upstream artifacts.  Next-step is the
harness submodule, not the VM.

### 3.1  SemaphoreTest / valueWithin timing  (4 remaining residuals)

Harness-specific.  Passes 100% standalone (`tc runCase`).  Fails
in the double-watchdog SUnit wrapper (P40 fork + P60 watchdog +
4 nested exception handlers).  Harness fast-path landed for 3 of
4 classes; 4 residuals still fail in SUnit.

Fix direction: harness's `runSingleTest:` should detect timing
classes and drop the P60 watchdog (or run `tc run: result`
directly).  Owner: `scripts/pharo-headless-test` submodule.

### 3.2  Reflection-walk timeouts

`testFastPointersTo` + `testPointersToCycle` now pass under JIT.
`testPointersTo` still >60s due to O(heap×N) traversal —
reclassified as known-slow.

### 3.3  Weak-reference / finalization timing  (4 residuals)

Test-framework retention during `Smalltalk garbageCollect` — not
VM bugs.

### 3.4  JIT eval-mode `MAX=50+` hang

`PHARO_JIT_DEFER=4` default works.  `PHARO_JIT_DEFER=0` hangs in
Morphic boot — scheduling not correctness.  Documented; not
scheduled for fix.

---

## 4. iOS — project mission work

The project's actual goal is iOS.  Mac Catalyst is verified
working (2026-02-24) but iOS device work needs hardware.

### 4.1  iOS device testing

- Physical iOS device(s) for build verification.
- Apple Developer signing cert setup.
- TestFlight or direct-device deploy.
- UI touch/pinch/pan end-to-end (not just Mac mouse).

### 4.2  iOS app-store readiness

- App icons, launch screen, metadata.
- Privacy manifest (iOS 17+ requirement).
- Crash reporting integration.
- Remote logging for device debugging.

### 4.3  Image preparation for device

- Verify standard Pharo image works unmodified on real iOS.
  Currently we use `startup.st` injection — confirm that path
  survives Snapshot round-trip on device.
- Touch-based Morphic input path.  Image is still desktop-mouse-
  oriented; propose upstream + local workaround.
- Portrait-aware layout (see 5.3).

### 4.4  W^X + signing on iOS

JIT is force-disabled on iOS (`PHARO_JIT_ENABLED=0` on
`TARGET_OS_IOS && !TARGET_OS_MACCATALYST`) because Apple's kernel
forbids W^X pages outside MAP_JIT, and MAP_JIT requires a specific
entitlement.  For the first iOS ship, stay interpreted.  A future
JIT-on-iOS pass would need:

- `com.apple.security.cs.allow-jit` entitlement.
- MAP_JIT + `pthread_jit_write_protect_np` (already used on Mac
  Catalyst).
- Entitlement request + review process.

---

## 5. Rendering + known display issues

From `docs/known-issues.md`.

### 5.1  Taskbar selected button text renders with artifacts

Slight rendering glitches on "selected" state.  Low priority.

### 5.2  VM single-process lifecycle

- VM thread sleeps forever after `interpret()` returns (avoids a
  pthread TSD crash).
- VM cannot be re-launched after quit without restarting the
  process.

These are live issues with the current iOS bridge; see
`docs/known-issues.md`.

### 5.3  Portrait layout + touch input (image side)

Morphic is desktop-oriented.  For iOS we need:
- Safe-area-aware layout.
- Touch event primitives on the standard input path (not
  HandMorph hacks).

Tracked as upstream proposals (section 7).

---

## 6. Image bugs we patch at startup

From `docs/image_issues.md`.  All applied via
`PharoBridge::writeStartupScript()`.  Upstream fixes listed in
section 7; as long as the image has them we keep patching.

6.1  `MicGitHubRessourceReference >> githubApi` — nil token → KeyNotFound.  
6.2  `MicDocumentBrowserModel >> document` — sends `#message` instead of `#messageText`.  
6.3  `MicDocumentBrowserPresenter >> childrenOf:` — missing outer error handler.  
6.4  Menu shortcut symbols render as "?" — embedded Source Sans Pro v2.020 missing glyphs.  
6.5  `WarpBlt >> mixPix:` Smalltalk fallback drops alpha channel.  
6.6  Doc browser bullets render as "?" — same font glyph gap as 6.4.

---

## 7. Upstream proposals (image, not VM)

From `docs/upstream-proposals.md`.  These are image-side
submissions to the Pharo project.

- Portrait-aware Morphic layout.
- Touch event primitives on the standard input path.
- Startup-preferences path that survives `Smalltalk snapshot:andQuit:`
  round-trips.
- Bug fixes 6.1-6.6 above (as pull requests to Pharo).

---

## 8. Recommendation — what to do next

**Status after 2026-04-18:** 2.4, 2.2, 2.9 shipped/resolved; 2.6
disproven.  Remaining VM work is either long (1.2 multi-bc
extensions, 2.5 stencil shrink) or blocked on lldb (1.1 one-arg
IC DNU).

For the VM codebase:

1. **Fix the 1-arg IC DNU (1.1).**  Requires an lldb watchpoint
   session to catch the stack-corruption moment.  Unblocks inline
   IC coverage expansion and 1.2f (multi-bc inline IC at sends).

2. **Multi-bc 1.2a (sends).**  Emit push-args + ExitSend inside
   the existing multi-bc walker.  Opens up arith + send bodies
   like `^ self at: idx + 1` for T2 compilation.

3. **Shrink `stencil_sendJ2J` (2.5).**  523 ARM64 instructions →
   target ~150-200 by factoring the IC probe + megamorphic path
   into a shared helper.  ~2KB saved per send site, i-cache win.

For the project mission:

4. **Pivot to iOS (§4).**  Mac Catalyst works today.  iOS device
   testing is blocked on hardware + signing, not code.  JIT perf
   has eaten 20+ sessions with diminishing returns; the VM is
   correct and runs the image.  Ship it.
