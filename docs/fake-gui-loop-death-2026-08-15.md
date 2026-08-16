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
