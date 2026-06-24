# Native-x86 SUnit scheduler-halt (PolyMath 455/942) — root cause + fix plan

Investigated 2026-06-24 (workflow `scheduler-halt-analysis`). The native-x86 PolyMath
run halts at 455/942 with the VM CPU-pinned; Rosetta x86 (Mac) completes 938/942.

## Symptom chain (reproduced locally)

- `PMArbitraryPrecisionFloatTest>>testPrintAndEvaluate` is a genuinely SLOW ~60s test
  (64s arm64-JIT, 54s arm64-interp — heavy LargeInteger/float work, JIT-neutral; >120s
  on Rosetta x86 = Rosetta's 2-5x tax). It blows past the runner's **10s JIT watchdog
  default** (run_sunit_tests.st:421) on every platform, firing the Delay watchdog.
- `testRaisedToNegativeInteger` PASSES in isolation (1-4s, all configs). Its suite-time
  TIMEOUT is a *symptom* of an already-dead scheduler, not a cause.
- So a slow test fires the Delay watchdog; the watchdog-fire corrupts the Delay scheduler;
  arm64/Mac recover, native Linux does not → the run freezes at ~455 (alphabetically ~J,
  PMJacobiTransformationTest).

## Root cause

The VM scheduler/timer code is **byte-identical on Linux and Mac** (the only `#if __APPLE__`
near it is a CoreFoundation include, Interpreter.cpp:59-63). The Linux-vs-Mac split is a
**timing race** on top of a **latent recovery defect** — native Linux's faster, differently
phased scheduling loses the race; Mac wins it by luck.

The defect, in `checkTimerSemaphore()` (Interpreter.cpp:4264-4356):

1. **The death-detector is gated off in the wedge state.** Recovery is gated by
   `lastTimerSignalTime_ > 0 && !timerWasArmed_` (Interpreter.cpp:4296). `timerWasArmed_`
   is only cleared when the VM ITSELF fires the timer (4280). When the image's
   DelayBasicScheduler arms a deadline and its backend dies/blocks *before* that deadline,
   `timerWasArmed_` stays true → the `&& !timerWasArmed_` gate is false → recovery never
   runs. Confirmed by captured wedge dumps: `[DIAG-TIMER] usecArmed=1 timerWasArmed=1`
   (docs/results/athens_run.log:1387, morph_run.log:1994).

2. **Recovery is futile even when it fires.** It only does
   `synchronousSignal(lastKnownTimerSemaphore_)` (the *timing* semaphore). The wedged
   scheduler is blocked on a DIFFERENT semaphore (`suspendBackgroundFailure:`,
   test-results.md:118-122), so re-signaling just bumps excessSignals (4460-4465) and
   wakes nobody.

3. **Contributing: stale disarm bookkeeping.** The prim-242 (Primitives.cpp:16276) and
   prim-136 (Primitives.cpp:5670) disarm paths null `timerSemaphore_`/wakeup but never
   clear `timerWasArmed_` or seed `lastTimerSignalTime_`, leaving the detector's gate
   inconsistent with reality.

4. **Contributing: watchdog can't kill the scheduler.** `maybeTerminateStuckProcess()`
   (Interpreter.cpp:10687) refuses to act on priority >=50, so the heartbeat watchdog can't
   terminate the P80 Delay scheduler to force a clean restart.

5. **Contributing: runner BAIL is unreachable.** The per-class BAIL (5 consecutive / 8 total
   TIMEOUTs, run_sunit_tests.st:719-726) keys on TIMEOUT *count*, but the per-test watchdog
   uses `Delay>>wait` (P60) which never fires once the scheduler is dead → no test TIMEOUTs →
   BAIL never trips → the class-boundary `restartTimerEventLoop` (758, 559) is never reached.
   So nothing — VM or runner — recovers, and the run freezes until the 6h batch deadline.

## Fix plan (NOT yet implemented — needs native-box verification)

Highest value = #1 + #2 (gate + escalate-to-restart):

1. **Fix the gate** (Interpreter.cpp:4296): arm recovery when a timer is OUTSTANDING-AND-
   OVERDUE, not only when the VM last fired it. Track `lastTimerProgressTime_` (update on
   every arm in prims 242/136 AND every fire at 4280/4352); trigger when
   `timerWasArmed_ && nextWakeupUsec_ != INT64_MAX && currentUsec >= nextWakeupUsec_ + 5e6`
   has held >5s, OR the existing fired-but-never-rearmed case.
2. **Make recovery non-futile**: after N (~3) failed re-signals, escalate to driving the
   image's `Delay scheduler restartTimerEventLoop` from C++ at a safe point (same mechanism
   `handleStackOverflow` uses to drive `terminate`, Interpreter.cpp:10726-10742). Behind a
   default-on opt-out knob (`debug_vars.h: DEBUG_BOOL(PHARO_NO_DELAY_HARD_RESTART)`).
3. **Fix disarm bookkeeping** (Primitives.cpp:16276, 5670): set `timerWasArmed_ = false` and
   seed `lastTimerSignalTime_` if still zero. NOTE: doing this alone risks spurious
   death-detection on a normal Delay-cancel — must ship with #1's overdue-aware gate.
4. **Defense-in-depth** (lower risk, runner-side): add a wallclock-based per-test ceiling in
   run_sunit_tests.st that does NOT depend on Delay (poll `Time millisecondClockValue` +
   `relinquishProcessorForMicroseconds:`, like the liveness probe at 547-552), so a dead-Delay
   test still counts as a TIMEOUT and trips the BAIL → the run completes instead of freezing.

## IMPLEMENTED: runner keepalive watchdog (#4), verified on native x86 (2026-06-24)

Implemented fix #4 (the stack-safe, runner-side option) rather than the VM-core #1+#2.
Rationale: driving `Delay scheduler restartTimerEventLoop` from C++ in checkTimerSemaphore is
NOT stack-safe — `push + sendSelector` from a side-path leaves the send's result on the
running process's operand stack (handleStackOverflow only works because it *replaces* a failed
send). The runner fix is pure Smalltalk, needs no VM rebuild, and was verifiable on the box.

`scripts/pharo-headless-test/run_sunit_tests.st`: fork an independent keepalive process
(`forkAt: 70`) that busy-polls the WALLCLOCK (not Delay), probes whether a 100ms Delay still
fires within 4s, and drives `Delay scheduler restartTimerEventLoop` when it doesn't — recovering
the wedge mid-class instead of waiting for an unreachable class boundary. Installed once per run
(guarded by `Smalltalk globals at: #SUnitDelayKeepaliveRunning`).

VERIFICATION (native AWS x86_64, AMD EPYC 7R13):
- Baseline (no fix): PolyMath FREEZES at 166-462 tests, marker=NO; VM [DELAY-DEATH] recovery
  fires 18+ times, every re-signal futile.
- With keepalive: PolyMath completes **942/942** (941 PASS + 1 TIMEOUT for the genuinely-slow
  testPrintAndEvaluate), marker=YES, and the VM [DELAY-DEATH] recovery fires **0 times** (the
  keepalive keeps the scheduler alive). perfdb run 97.
- No regression: kernel SUnit with the keepalive active = **12673 PASS / 0 F / 2 E** — identical
  to the no-keepalive baseline (run 93); keepalive logged 1 (benign) restart. perfdb run 98.

REMAINING (optional, deeper): the VM-core root-cause fix #1+#2 (overdue-aware gate + non-futile
recovery via a fork-based or semaphore-signalled restart) would make the VM self-heal for
non-SUnit Delay users too. It needs a stack-safe way to drive the restart from C++ (e.g. signal
a dedicated semaphore that an image-side recovery process waits on). Not required for the SUnit
harness, which the keepalive now makes robust.
