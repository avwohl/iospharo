# Handoff — state at end of 2026-08-15 session

Written because the remaining items each need a long run (a ~55 min instrumented
suite, or network package loads) rather than another edit, and starting one
without being able to collect it wastes the run.  Everything below is either
measured or explicitly flagged as not.

## Landed this session

    46f2ec29  class-table truncation on image save (validated FAIL->PASS)
    3bd7e29   revive the fake-GUI cycle loop when a test kills it (15 -> 0/2 errors)
    000e9f0e  #15 stack-overflow cap is now a CATCHABLE Error, fully gated

Suite baseline, and the number to diff against:

    Pass 17956   Fail 2   Error 1   Skip 75   899 sections

The single Error is `testCreateNormalClassWithTraitComposition
(OCCodeError: Undeclared variable)` and is unrelated to any of this.

## Retracted this session — do not rebuild on these

  * **#21 "unhandled exception in a forked process wedges the eval"** — there is
    no wedge.  The image logs and exits by design.
  * **The forwarding-pointer theory** for literals — a probe in
    `Interpreter::literal()` logging any forwarded literal fired ZERO times.
  * **The first #15 revert reason** ("leaks the mutex, takes down the Delay
    scheduler") — refuted by rerun; nothing leaks.

All three came from ONE mistake, which is the most useful thing in this file:

> "No EVAL-RESULT printed" was read as "hung".  It is also exactly what a
> correct quit looks like.  For anything that ends without output, record
> ELAPSED TIME and EXIT STATUS — `timeout` returns 124 on a real hang and 0 on a
> quit.  "Produced no result" and "did not terminate" are different findings
> that look identical in a log.

A file-based side channel (write markers with `writeStreamDo:` as they happen)
is the discriminator that survives a quit.  Note `appendStreamDo:` on a
non-existent file raises, so use `writeStreamDo:` re-writing an accumulated
collection.

## Next item, fully designed: who terminates the cycle loop

Settled already: the loop is ALWAYS externally terminated, never killed by an
escaping exception — instrumented full run gave `UNWIND 4, ESCAPED 0`.  The four
terminations precede `CoFetcherWithNoResultsTest`, `DTTestProfilingTest`,
`DebugPointBrowserPresenterTest`, `DrTestsTestRunnerUITest` — test-runner and
debugger UI tests, i.e. tests whose subject is running or suspending processes.

What is NOT known is which call does it.  The victim's own `ensure:` stack
cannot show it, because unwind blocks run on the victim's frames.  So hook the
CALLER:

    Process compile: 'terminate
       (self == (Smalltalk at: #FakeGUICycleLoop ifAbsent: [nil])) ifTrue: [
          FakeGUI recordTerminatorStack: thisContext ].
       ... original body ...'

Install it in `setup_fake_gui.st` after the loop is forked, record
`Processor activeProcess` plus a 30-frame walk of `thisContext`, and run the
full 896-class list.  The terminator's own frames will be in that walk.

Reproduction requires the full ordering — `CoCompletionEngineTest` passes 65/65
in isolation, so a narrow slice will not show it.

## Recipe for a full instrumented run (this works, use it verbatim)

    provision:  scripts/aws/provision.sh          (writes scripts/aws/state.env)
    box repo:   git clone --recurse-submodules; check the SUBMODULE sha, a plain
                pull leaves scripts/pharo-headless-test behind
    build:      cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)
    stock vm:   curl -sL https://get.pharo.org/64/vm130 | bash    (needed for prep)
    image:      curl -sL https://get.pharo.org/64/130   | bash
    prep:       ONE `pharo <img> eval --save` filing in setup_fake_gui.st then
                run_sunit_tests.st.  Do NOT prep twice — a double-prepped image
                gives TIMEOUT(prim-stuck) on every test.
    classes:    put the 896-name list at /tmp/sunit_class_names.txt
    GOTCHA:     rm -f /tmp/sunit_run_completed.txt first, or the runner silently
                does nothing and the run looks like "VM starts, no results"
    run:        setsid nohup ... test_load_image <img>   (survives SSH drops)
    collect:    FETCH ARTIFACTS BEFORE TEARDOWN, and gate teardown on the fetch

## Still open, untouched

    #2   13 packages our VM cannot run at all — HIGH, needs network package loads
    #5   residual: fedeloch-ume, moosetechnology-gitprojecthealth,
         tomooda-viennatalk still fail; the OSSubprocess half is FIXED
    #6   activation wall / 80 s reflective-scan timeouts — note the ONE
         measurement taken this session was 550 ms over 11 processes, i.e. that
         particular scan is NOT slow; #6 needs its own repro rather than
         inheriting this one
    #8   stepping hangs on native x86_64 Linux
    the 26 "hangers" — in-image 300 s per-test timeout does not recover them;
         two distinct hangs seen (TraitTestCase, ObjectWithPrintingRaisingHalt),
         both pass in isolation, so they are order-dependent like the loop death
