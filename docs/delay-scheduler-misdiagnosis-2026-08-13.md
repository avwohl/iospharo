# "Delay scheduler dies" is one SYMPTOM with three distinct causes

**Date:** 2026-08-13
**Status:** diagnosis corrected. One cause fixed (`b808d1b5`), one already dead,
one still open.

> **Correction, same day.** An earlier revision of this file said the P80
> startup boost was "the direct cause of the starvation misdiagnosed for
> months". That is wrong and the dates refute it: the boost was added
> 2026-04-08 (`29a7bbb6`), while the misdiagnosis was written 2026-02-14
> (`9688241c`). The boost is a *second, later* cause of the same symptom, not
> the original one. The original is mechanism 1 below. The A/B measurement in
> this file stands; the causal claim attached to it did not.

The symptom every diagnostic keys on is a single VM state: `timerSemaphore_`
nil and `nextWakeupUsec_ == INT64_MAX` with nobody re-arming
(`Interpreter.cpp:4785-4790`, one-shot fire-and-clear). `[DELAY-DEATH]`, the
futile re-signal, `[DELAY-RESTART]` and VM-TIMEOUT all trigger on that state
alone and **cannot tell the causes apart**. At least three unrelated mechanisms
produce it, each dominant in a different era. That is why no single fix ever
closed it.

    rank  mechanism                          status              discriminator
    1     poisoned Delay -> SUnit suspends    image half fixed    isSuspended=true,
          the P80 ticker                      2026-07-06; the     myList=nil, ticker
                                              T1-JIT nil source   NOT on schedLists[80]
                                              is NOT fixed
    2     VM boosts startup to P80 ->         fixed b808d1b5      ticker READY and FIRST
          priority contagion -> starvation    (this file)         in schedLists[80]; a
                                                                  DIFFERENT P80 ACTIVE
    3     VM stuck-process watchdog kills     already dead        [TERM-P80] /
          the ticker (the ORIGINAL cause)     (see below)         [STUCK-GUARD]

`dumpTimerWedgeState()` (`Interpreter.cpp:2424-2600`) already prints all three
shapes distinctly. The instrumentation was never missing. What was missing was
noticing that `[DELAY-DEATH]` names a *conclusion* rather than an observation,
so every dump was read as confirming the name.


## Mechanism 3 — the original cause, and it was self-inflicted

The timeline settles what "root cause still unknown" actually was:

    98c77c7b  2026-02-14 07:01  add watchdog: 30s without global step progress
                                => terminate WHATEVER process is active
    caa1864c  2026-02-14 07:26  fix that watchdog
    9688241c  2026-02-14 08:54  "When the Delay scheduler dies (root cause
                                 still unknown)..." => adds VM-TIMEOUT

The "unknown root cause" was written **113 minutes after** the VM was given a
mechanism that kills whatever process is active when bytecodes stop advancing.
A Delay ticker parked in `waitForUserSignalled:orExpired:` executes no
bytecodes, so it looks exactly like a stuck process. Confirmed four months
later in `0634b733` (2026-06-05):

> Observed in the full SUnit run at ~class 508: **[TERM-P80] of
> `DelaySemaphoreScheduler>>runBackendLoopAtTimingPriority` via
> terminateAndSwitchProcess** (fd=0, not an overflow), then [DELAY-DEATH].

That killer is now defanged: `maybeTerminateStuckProcess()`
(`Interpreter.cpp:11793-11822`) returns false unconditionally, its own comment
conceding "every kill this path ever performed was a false positive".

**The killer is gone. The workaround it spawned is still here.**

Since ~2026-06 the tree has carried a recurring VM watchdog whose stated reason
is, verbatim from commit `9688241c`:

> When the Delay scheduler dies (root cause still unknown), Smalltalk-level
> timeouts stop working. This VM backstop terminates any user-level process
> (pri < 80) that runs for > 30 seconds without switching.

That premise — that the scheduler *dies* — appears to be wrong. The scheduler
does not die. It is alive, runnable, and starved.


## The evidence

Running a single test class on the custom VM (macOS arm64, a stock Pharo 13
image prepped with the SUnit runner) reproduces the wedge in about five
seconds. The VM's own one-shot `[WEDGE]` dump says it plainly:

    [WEDGE] timerSemaphore_=nil lastKnownTimerSemaphore_=0x700042d258
            nextWakeupUsec_=9223372036854775807 timerWasArmed_=0
    [WEDGE] timingSem 0x700042d258 excessSignals=0 firstWaiter=nil
    [WEDGE]   (timing semaphore wait list EMPTY -> scheduler not waiting here = likely DEAD)
    [WEDGE] P80 proc=0x70033b7bf0 ACTIVE   susp=nil top=?>>(nil)
    [WEDGE] P80 proc=0x7002c751c0 ready    top=DelayMicrosecondTicker>>waitForUserSignalled:orExpired:
    [WEDGE] *** TIMER-RUNNER ALIVE: proc=0x7002c751c0 P80 (runner at chain depth 3) ***
    [WEDGE]   chain[3] DelaySemaphoreScheduler>>runBackendLoopAtTimingPriority
    [WEDGE]   schedLists[P80]=0x7000089fc8 first=0x7002c751c0

Read those last four lines together:

  - the Delay scheduler process is **ready**, and is **first in the P80 ready
    list** — it is not dead, not suspended, and not waiting on a semaphore;
  - a **different** process, also at **P80**, is ACTIVE;
  - the VM is executing ~484 million bytecodes per 10s, so the active process
    is spinning, not blocked.

Pharo's scheduler is cooperative *within* a priority level: a P80 process runs
until it yields, waits, or is suspended. A P80 process that does long work
therefore starves every other P80 process behind it — including the Delay
ticker. The ticker never runs, so it never re-arms the timer, so
`nextWakeupUsec_` stays at `INT64_MAX` and `timerSemaphore_` stays nil. Every
Delay-based timeout in the image stops working.

That is exactly what the VM reports one line earlier:

    [DELAY-DEATH] Timer semaphore signaled 5s ago but scheduler never re-armed.

It never re-armed because it never got the CPU. The diagnostic's own name
("DELAY-DEATH") encodes the wrong conclusion.

The line `(timing semaphore wait list EMPTY -> scheduler not waiting here =
likely DEAD)` is the specific inference that is wrong. An empty wait list means
the scheduler is *not blocked on the semaphore* — which is equally consistent
with "ready and waiting its turn", and that is what the very next lines show.


## What the starving process actually was, in this reproduction

    [DIAG] P80 ProtoObject class>>ifNotNil: ip=43 fd=9
    [DIAG]   [-3] SDL2 class>>bindingOf:
    [DIAG]   [-5] OrderedCollection>>do:
    [DIAG]   [-9] SDL_TextEditingEvent class>>defineFieldOffset:value:

SDL2 FFI structure field-offset resolution, running at priority 80. Long work
at timing priority is the defect. The scheduler behaved correctly throughout.


## ProcessTest is not the reproducer it was believed to be

Commit `d58be9b3` added ProcessTest to a skip list with the note "kills Delay
scheduler". In this environment that attribution does not hold: running
**ArrayTest** alone — which manipulates no processes at all — produces a
byte-identical wedge (same 106-line dump, same single `[DELAY-DEATH]`, same
zero results) as running ProcessTest alone.

    ProcessTest   exit=0  results=0  wedge=106  delay-death=1
    ArrayTest     exit=0  results=0  wedge=106  delay-death=1

The wedge is a *startup* condition reached before any test-specific code runs.
Skip-listing ProcessTest therefore suppressed a symptom that ProcessTest did
not cause.


## Why the workaround keeps coming back

Because each iteration treats a starvation symptom as a liveness failure:

    9688241c  add VM-TIMEOUT: terminate any process running >30s
    f7faed06  remove it -- it killed the test runner (which legitimately
              spins at P40 using Processor yield)
    ac09c283  re-add, now capped at 5 terminations "to avoid killing ALL
              processes", and skipping idle
    d58be9b3  add a spin-wait watchdog explicitly designed to have "no Delay
              dependency"; skip-list ProcessTest
    0a293966  gate on prio > 10 to silence the idle false positive

Every one of these is a response to damage caused by the previous one. None
addresses a P80 process doing long work, because nobody had established that
was the mechanism.


## The cause: the VM boosts its own startup process to P80

`src/vm/Interpreter.cpp` (pre-fix, ~line 1204) did this, in headless mode:

    // In headless mode, boost the startup process to timingPriority (80).
    // Session handlers may fork processes at timingPriority (80) ...
    // If the startup process runs at its default priority (79), the forked
    // P80 process preempts it and the remaining session handlers never execute.
    memory_.storePointer(ProcessPriorityIndex, activeProcess, Oop::fromSmallInteger(80));

The VM overwrites the image's own priority for the startup process, moving it
to timing priority. Every startup activity then runs at P80 — including the
SDL2 structure work above — and starves the Delay ticker parked behind it.

The boost was itself a workaround, for "session handlers never execute". It
traded a startup-ordering problem for a timer-liveness problem, and the second
problem then grew five more workarounds on top of it.

### Confirmed by controlled A/B

The boost is now behind `PHARO_STARTUP_P80_BOOST`, default OFF. Same binary,
same image, same single test class, only the flag differs:

    boost OFF (new default)   wedge=0     delay-death=0   startup completed
    boost ON  (old behaviour) wedge=106   delay-death=1   startup completed

The wedge appears and disappears exactly with the boost. Both configurations
reach `=== Test Complete ===`, so disabling the boost does not break startup in
this scenario — the condition the boost was originally added for did not
recur here.


## What is NOT yet established

Be careful with this result; it is narrower than it looks.

  - **The Linux full-suite run does not exhibit this at all.** A run on x86_64
    Linux (m6a.4xlarge, jit @ `7ce18503`) reached 29,000+ results with WEDGE 0,
    DELAY-DEATH 0, VM-TIMEOUT 0. So this reproduction is macOS-side, and the
    Linux suite was already healthy before the fix.
  - **The original reason for the boost is not disproven.** "Session handlers
    never execute" may still occur in some image/startup configuration that a
    fresh Pharo 13 image plus one test class does not reach. That is exactly
    why the boost is flag-gated rather than deleted: `PHARO_STARTUP_P80_BOOST=1`
    restores the old behaviour if a regression appears.
  - **The local runner still produces zero test results** on macOS in this
    setup, with or without the boost. That is a separate unexplained problem
    and is not addressed here.


## Also fixed here

### VM-TIMEOUT is now OFF by default (`PHARO_VM_TIMEOUT_KILL`)

Both sites (`Interpreter.cpp:4104`, `:5873`) are gated off. Beyond its premise
being gone (mechanism 3 above), the mechanism is broken on its own terms:

    4108   memory_.storePointer(ProcessSuspendedContextIndex, currentActive, nil)
    4111   transferTo(nextProc)
    19745    -> storePointer(ProcessSuspendedContextIndex, oldProcess, contextToSave)

`transferTo` re-saves the materialized stack into the very slot that was just
nil'd, one line later. So it does not "clear suspendedContext" as its comment
claims. And neither site calls `putToSleep()` on the victim, so the process ends
up on no ready queue and not active — a silent, permanent removal from
scheduling with its context intact. It manufactures precisely the unrunnable
state it exists to escape.

The 600000ms threshold has also drifted (30s -> 90s -> 600s) while the comment
at `:5834` still says 90s.

### `checkForPreemption` scanned the wrong direction

`Interpreter.cpp:19939` ran **upward** from the active priority and took the
first hit, which selects the LOWEST ready priority above the active one. With a
P80 ticker ready and a P41 process also ready while P40 runs, it preempted to
P41 and left the timer runner waiting. Repeated, that starves the process every
Delay in the image depends on.

Both sibling scans already walk downward (`Interpreter.cpp:4545`, `:6044`:
`for (int pri = maxPri; pri > activePriority; pri--)`), as does Cog. Now fixed
to scan downward.


## What should happen next

  - Run the Linux full suite with the fix and compare against the
    `7ce18503` baseline. Equal-or-better results are the gate for keeping the
    new default.
  - Re-test ProcessTest on Linux without the skip-list entry. Its skip note
    ("kills Delay scheduler") is not supported by this reproduction: running
    **ArrayTest** alone wedges identically, so ProcessTest was not the trigger.
  - Only once the Linux suite confirms the new default, retire the workaround
    inventory: the two `VM-TIMEOUT` sites, the `lastKnownTimerSemaphore_`
    recovery/deferred-signal paths, and the ProcessTest skip entry. Any fix
    that does not let those go is another layer on the same treadmill.
  - Rename the `[DELAY-DEATH]` diagnostic. It asserts a conclusion the evidence
    does not support, and that name is a large part of why the wrong mechanism
    was believed for so long.
  - The neighbouring startup hacks deserve the same scrutiny: nil'ing the active
    process's `suspendedContext` (`Interpreter.cpp` ~1186) and bulk-demoting
    every P40 process to P10 while rewriting scheduler queues (~1216). Both are
    C++ doing Smalltalk's job, in the same function, for similar reasons.
