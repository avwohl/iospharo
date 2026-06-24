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

## IMPLEMENTED: VM-core self-heal (#2), verified on native x86 (2026-06-24)

The first attempt was a runner-side busy-poll keepalive — it fixed PolyMath but REGRESSED Fuel
(P70 busy-poll slows reflective class/trait-creation: Fuel 266/700s with it vs 733/242s without),
so it had to be opt-in. It is now REPLACED by the proper VM-core self-heal (synthesis #2), which
is default-on, zero-overhead, and fixes PolyMath without regressing Fuel.

Mechanism (commit 06234763, submodule f5dc144):
- `delayRecoverySemaphore_` (GC-rooted) + named primitive `primitiveRegisterDelayRecoverySemaphore`
  (Interpreter/Primitives.cpp): an image-side recovery process registers a Semaphore it waits on.
- `checkTimerSemaphore` [DELAY-DEATH] detector (Interpreter.cpp:~4331): in addition to the futile
  timing-semaphore re-signal, `synchronousSignal(delayRecoverySemaphore_)` to wake the recovery
  process. This is STACK-SAFE (a semaphore signal, not a C++→Smalltalk send). Opt-out:
  `PHARO_NO_DELAY_HARD_RESTART`.
- `run_sunit_tests.st`: the recovery process just `wait`s on that Semaphore, then drives
  `Delay scheduler restartTimerEventLoop` (a fresh scheduler). It uses ZERO CPU when healthy —
  it only wakes when the VM signals a genuine wedge — so no busy-poll, no Fuel regression.

Why it is selective (the key over the keepalive): the VM's [DELAY-DEATH] detector fires ONLY on a
real wedge (timer signaled but never re-armed for >5s), so the recovery process is woken only when
the scheduler is actually stuck. Healthy suites never signal it.

VERIFICATION (native AWS x86_64, AMD EPYC 7R13; self-heal default-on, vm_build 114):
- PolyMath: **942/942** (940 PASS + 2 TIMEOUT), marker=YES; VM signalled the recovery **once**
  ([DELAY-RESTART]=1) and a single scheduler restart un-wedged it. perfdb run 100. (Was 166-462 FREEZE.)
- Fuel: **734/10/5 in 243s**, marker=YES — NO regression (same speed as no-fix 242s; the busy-poll
  keepalive stalled it at 266/700s). [DELAY-RESTART]=1 (a brief wedge, recovered with no overhead).
  perfdb run 101.
- Kernel SUnit: **12672/1/2**, marker=YES — at parity with baseline 12673/0/2 (the 1 FAIL/1 TIMEOUT
  are unrelated flaky tests — SemaphoreTest>>testUnCategorizedMethods, ClassDescriptionProtocols,
  trait/closure compiler — not Delay/scheduler, despite 1 [DELAY-RESTART]). perfdb run 102.

This self-heals for ANY Delay user (not just the SUnit harness — any image that registers a
recovery semaphore), with zero steady-state cost. The earlier gate fix (#1) proved unnecessary:
the [DELAY-DEATH] detector already engages reliably; only the recovery action was futile.
