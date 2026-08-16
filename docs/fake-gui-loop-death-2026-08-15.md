# Why the fake-GUI World-cycle loop dies — settled by instrumentation

Closes the open question left by `9970550b` ("still open: why the Calypso
completion tests terminate a forked P40 process they do not own").  The answer
is now measured rather than inferred, and the inferred part was wrong in two
ways worth recording.

## The question

`setup_fake_gui.st` forks a P40 loop that drives `MorphicRenderLoop doOneCycle`
every 25 ms.  Deferred UI work only runs if that loop is alive.  It was dying
partway through the suite, and 143 subsequent classes ran without UI cycles —
the cause of a 15-error `SubscriptOutOfBounds` cluster in Sp*/FT* adapter tests.
`3bd7e29` revives the loop between classes, which cut the cluster to 2.  But a
revival is a recovery, not a diagnosis: nothing established *how* it died.

Two candidate mechanisms, and they call for opposite fixes:

    escaped exception   something non-Error (Halt, Warning, a Notification)
                        slips past the loop's `on: Error do:` guard and ends
                        the `repeat`.  Fix belongs in the harness: widen the
                        guard.

    external terminate  another process sends #terminate to the loop.  Fix
                        belongs wherever that process is — possibly the VM,
                        if a process is terminating one it does not own.

## The instrument

Both are directly observable, and they are distinguishable, because they leave
the process by different doors.  The loop body was wrapped as

    [[[[[ ...body... ] repeat]
        on: Exception do: [:ex | record 'ESCAPED-EXCEPTION ', ex class name]]
       ensure: [ record 'UNWIND' ]] forkAt: 40

An escaping exception of any class trips the `on: Exception` arm.  An external
`#terminate` runs unwind blocks, so it trips the `ensure:` arm and never the
other.  Each record also captures `Processor activeProcess`, so a terminator
that is a *different* process identifies itself as such.

## Result — full 896-class suite, instrumented

    UNWIND               4
    ESCAPED-EXCEPTION    0

Unambiguous: the loop is **externally terminated every time**.  Nothing escapes
the error guard, so widening the guard would have been wasted work — that whole
branch is closed.

## Correction: it is not "the Calypso completion tests"

The four terminations happen before these classes:

    CoFetcherWithNoResultsTest
    DTTestProfilingTest
    DebugPointBrowserPresenterTest
    DrTestsTestRunnerUITest

Only the first is Calypso completion.  The earlier framing generalised from the
single transition that had been measured (the one that produced the largest
victim run) to a claim about a package.  The honest pattern across all four is
that they are **test-runner and debugger UI tests** — classes whose subject
matter is running or suspending other processes.  A class of test that sweeps
or terminates processes wholesale would hit a forked P40 loop it never created,
which fits every observation here.

That is a hypothesis, not a result: which call does the terminating is still
unidentified.  The remaining step is a hook on `Process>>terminate` that logs
when the receiver is the cycle loop — the victim's own `ensure:` stack cannot
show it, because unwind blocks run on the victim's frames, not the caller's.

## Suite totals, instrumented run

    Pass    17956
    Fail        2
    Error       1        testCreateNormalClassWithTraitComposition
                         (OCCodeError: Undeclared variable) — unrelated
    Skip       75
    Total   18035        899 class sections, 4 loop restarts

`SubscriptOutOfBounds` events this run: **0**, against 15 before `3bd7e29` and
2 in the run that validated it.

Do NOT read that 0 as "the last 2 are fixed".  Nothing was changed between the
validating run and this one except adding instrumentation, and the recorded
deaths show the instrumentation never fired its exception arm — so the most
likely explanation is that the residual 2 are timing-sensitive and simply did
not reproduce.  Extra `ensure:` frames perturb scheduling.  Two runs is not
enough to call a race fixed; it needs repetition before anyone claims it.

## ANSWERED — the terminator is Pharo's own test isolation

Hooked the caller side (`Process>>terminate` wrapped, original preserved under a
private selector; wrapper validated locally first — `terminated=true` — since
Pharo's terminate does stack surgery and must not be paraphrased).  Full
896-class run, 7 hits.  Four are self-inflicted: `startCycleLoop`'s own "always
replace" line terminating the previous loop during a revival.  The other three
are the answer:

    Process>>terminate
    WeakSet>>do:
    ProcessMonitorTestService>>terminateRunningProcesses
    ProcessMonitorTestService>>cleanUpAfterTest
    OrderedCollection>>do:
    TestExecutionEnvironment>>cleanUpAfterTest
    [ ... self cleanUpAfterTest ] in TestExecutionEnvironment>>runTestCase:
    TestExecutionEnvironment>>runTestCase:
    DTCoverageMockTest(TestCase)>>runCaseManaged

It is not a VM bug and it is not a rogue test.  `TestExecutionEnvironment`
tracks processes in a `WeakSet` and `ProcessMonitorTestService` terminates
everything still running when a test case ends — standard SUnit isolation,
doing exactly its job.  Our fake-GUI loop is a process forked outside any test
but alive during one, which is indistinguishable from a process the test
leaked.

That also explains the class pattern honestly, where the earlier "Calypso
completion tests" and then "test-runner and debugger UI tests" framings were
both wrong-ish.  The terminating class is incidental: ANY test running under
`TestExecutionEnvironment` can sweep the loop.  The DrTests/DebugPoint classes
show up because they run whole nested suites, giving many more `runTestCase:`
cleanups per class and so many more chances to catch the loop alive.

CORRECTION to this file's own earlier conclusion: `UNWIND 4, ESCAPED 0` was read
as proving external termination.  An `ensure:` also fires on a NORMAL block
exit, so on its own it does not distinguish "terminated" from "finished".  The
conclusion happened to be right — the caller hook proves termination directly —
but the evidence offered for it did not establish it.

### The fix belongs in the harness, and it is not a workaround

The right fix is to stop the loop looking test-owned, not to defeat the sweep:
fork it so `TestExecutionEnvironment` does not adopt it, or register it as a
system process the monitor should ignore.  Defeating the sweep would break the
isolation that keeps genuinely leaked test processes from accumulating.

The revival in `3bd7e29` remains correct as a safety net and measurably works
(15 subscript errors -> 0), but it is a recovery: the loop still dies and up to
one class runs without UI cycles before the next revival.  Removing the cause
removes that window.

## Validation of the un-adopt fix — PARTIAL, measured

Full 896-class run with the fix in (submodule `66ea6ee`, presence of the fix
verified on the box) and the `Process>>terminate` hook still installed:

                        before fix      after fix
    terminate hits      7 (3 real +     1 (revival path only —
                        4 revival)         ZERO real terminations)
    loop restarts       4               1
    subscript errors    0 (that run)    1
    totals              P17956 F2 E1    P17955 F2 E2

The targeted half WORKED: no test terminates the loop through
`Process>>terminate` any more.  The one remaining hit is `startCycleLoop`'s own
"always replace" line, i.e. self-inflicted by the revival.

But the loop STILL DIES ONCE, at the original spot (restart before
`CoFetcherWithNoResultsTest`), through a path that is NOT `Process>>terminate`
— the hook would have caught it.  So `ProcessMonitorTestService`'s terminate
sweep was a real cause but not the only one.

Best remaining candidate, from the same class read earlier and NOT yet tested:
`handleNewProcess:` does two things, and un-adopting only undid one of them.

    handleNewProcess: aProcess
        super handleNewProcess: aProcess.
        forkedProcesses add: aProcess.                      <- fixed
        aProcess on: UnhandledException do: [ :err |        <- STILL ATTACHED
            self handleBackgroundException: err]

That handler is installed ON the process at fork time and survives removal from
`forkedProcesses`.  Its path ends in `suspendBackgroundFailure:`, which does
`activeProcess suspend` — a death with no `#terminate` anywhere, which fits the
evidence exactly.  Next step is to confirm by recording whether the loop is
suspended rather than terminated at the moment the revival fires, and if so to
fork the loop outside the environment entirely rather than un-adopting it after
the fact.

Net effect so far: 15 subscript errors before any of this work, 1 now, with the
revival still carrying the last case.

## Validation of the fork-outside fix — the hypothesis was WRONG

Full run with `forkedOutsideTestEnvironmentAt:` in (submodule `4fda356`,
presence verified on the box, `forkFix=2`):

    terminate hits    1  — and it is the revival path, i.e. ZERO real terminations
    loop restarts     1  — UNCHANGED from the un-adopt-only run
    restart point     before CoFetcherWithNoResultsTest — the SAME place as ever

So forking with `DefaultExecutionEnvironment` active did NOT remove the last
death.  The `on: UnhandledException` -> `suspendBackgroundFailure:` -> `suspend`
theory predicted it would, so that theory is REFUTED, not merely unconfirmed.
Recorded here rather than quietly dropped, because it was written into the
commit message for `4fda356` as the reason for the change.

Where that leaves the loop, precisely:

    ProcessMonitorTestService terminate sweep    REAL cause, FIXED (3 -> 0 hits)
    the last death at CoFetcherWithNoResultsTest CAUSE UNKNOWN — survives both
                                                 un-adoption and forking outside
                                                 the environment, and does not
                                                 go through Process>>terminate

Both fixes are worth keeping — the terminate sweep was genuinely killing the
loop three times per run and no longer does — but the headline "root cause
found" applies to that sweep only, and one death per run remains unexplained.

What the next probe should NOT be: another guess at which image code does it.
Two guesses have now failed the same way.  Make the loop report its own state
instead — have the revival check record `isTerminated` / `isSuspended` / whether
`suspendedContext` is nil at the moment it decides the loop is dead.  That
distinguishes terminated from suspended from finished-normally, which is the
distinction every theory so far has assumed rather than measured.

## Two more candidates tested, both refuted — read this before guessing again

    fork outside the test environment   Error 2 -> 18, restarts unchanged at 1
    guard every iteration vs Exception  Error 2 -> 4,  restarts unchanged at 1

Both reverted.  The second is the more informative failure: it raised the death
from cycles 15453 to 27061, so it DID swallow exceptions the old shape let
through — and the loop still ended, at the same class, with the same
`term=true susp=false ctxNil=false`.  So whatever ends the loop is **not** an
exception propagating out of the repeat, and is not the process being suspended,
and does not go through `Process>>terminate`.  Three mechanisms excluded by
measurement rather than argument.

Standing tally on this loop: the ProcessMonitorTestService terminate sweep was
real and is fixed (3 -> 0).  One death per run remains, at
`CoFetcherWithNoResultsTest`, cause unknown, with the revival covering it.

Anyone picking this up: FIVE guesses have now been tested and refuted, at ~55
minutes a run.  Stop proposing mechanisms.  The next move is to catch the death
as it happens — e.g. have the loop write a marker file at the TOP of every
iteration, so the last marker before death names the exact statement it did not
return from.

## NAMED — the loop dies parked in `Delay>>wait`

The phase marker answers it in one run:

    CYCLE-LOOP-RESTART before CoFetcherWithNoResultsTest
      [state term=true susp=false ctxNil=false cycles=15508 phase=#delay]

`phase=#delay` means the last statement the loop entered was
`(Delay forMilliseconds: 25) wait`, and it never reached `#looped`.  So the
process ends while parked on a Delay''s semaphore.

Put together with what is already excluded by measurement, the constraints are
now tight and, importantly, they point back at the VM rather than the harness:

    dies inside Delay>>wait          phase=#delay
    ends TERMINATED                  term=true (the revival check requires it)
    not suspended                    susp=false
    no #terminate sent to it         caller hook, zero real hits
    not an escaping exception        guarding the wait moved the death
                                     (cycles 15453 -> 27061) without stopping it

A process waiting on a semaphore that ends without anyone terminating it and
without an exception is not ordinary image behaviour.  The suspicious neighbour
is our own semaphore/scheduler code: `primitiveWait`'s rollback path and
`removeProcessFromList` were changed earlier in this same session
(`primitiveWait`/`primitiveYield` were rolling back with
`removeFirstLinkOfList` instead of removing the specific process).  A waiter
being dropped from the wrong list is exactly the shape that would make a parked
process disappear.

NEXT, and it is now a VM question, not a harness one: instrument
`removeProcessFromList` / the Delay-semaphore path to log whenever the process
removed is not the one intended, and re-run.  The Delay-recovery half of this lead is
already REFUTED, checked against the run log rather than left for a future
session: `DELAY-RECOVERY-REGISTERED` appears once, at line 13, as registration
at startup, and never fires again — no `restartTimerEventLoop`, no
`TIMER-NOT-REARMED` — while the death is at line 19063.  So the runner's Delay
recovery is not stranding the waiter.  Do not spend a run on it.

That leaves the semaphore/scheduler path as the single remaining candidate, and
it is the one with priors: `primitiveWait`/`primitiveYield` were found rolling
back with `removeFirstLinkOfList` instead of removing the specific process
earlier in this same session, which is precisely the bug shape that makes a
parked waiter vanish.  The fix landed there; whether every such path was
corrected was never audited.

### The removeFirstLinkOfList audit is DONE, and it is clean

Flagged above as never audited, so it was audited — it is a local grep, not a
run.  21 call sites (16 in Interpreter.cpp, 5 in Primitives.cpp).  None is the
wrong-waiter shape:

  * the semaphore signal paths (4925, 5009, 5343) take the FIRST waiter, which
    is correct signalling semantics, not a bug;
  * the scheduler sites (19811, 19907) remove the same `firstProcess` they then
    `transferTo:`, so the process removed is the process used;
  * `primitiveWait` / `primitiveYield` / `primitiveExitCriticalSection` already
    use `removeProcessFromList(process, list)` after the fixes earlier in this
    session, and Primitives.cpp:5070 and :9465 carry comments explaining
    exactly why `removeFirstLinkOfList` would be wrong there.

So "another site drops the wrong waiter" is weakened as a theory — not
impossible (a wrong-waiter bug could live inside `removeProcessFromList` or the
Delay-semaphore bookkeeping rather than at a call site), but the cheap version
of it is ruled out and should not be re-audited.

Which leaves, for whoever picks this up: the loop ends while parked in
`Delay>>wait`, terminated, un-suspended, with no `#terminate` and no exception,
and with no obvious wrong-waiter call site.  The next probe has to catch the
transition itself — log every `removeProcessFromList` / `addLastLinkToList`
touching the loop process oop, and re-run.
