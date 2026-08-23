# arm64 full SUnit sweep, 2026-08-22 — after the day's fixes

`scripts/sunit-sweep.sh` equivalent at STEP=50, `PHARO_CODE_ZONE_MB=192
PHARO_MAX_STEPS=4000000000000`, on the prepped SUnit-runner image, machine
otherwise idle. Started 09:31, totals at 11:23.

    === TOTALS, with both damaged batches re-run and merged ===
      classes 2046
      tests   28067
      PASS    27727
      FAIL       27
      ERROR      25
      SKIP      182
      rate    98.79%

    (the raw sweep, before the two re-runs: 1982 classes, 27538 tests,
     27210 P, 27 F, 24 E, 178 S)

Against the 2026-08-19 reference in `docs/test-results.md` — arm64 RAW,
2043 classes, 28058 tests, P=27725, F=23, E=22, 98.81% — this is three more
classes, nine more tests, two more passes, and F+E of 52 against 45. That
gap is inside the run-to-run band the same file documents ("this harness
flips a few tests per run in either direction";
`WeakOrderedCollectionTest` alone gave 3 FAIL / 1 PASS across four identical
isolated runs). **Parity with the reference — but reached with the JIT
compiling throughout rather than freezing when the code zone filled**, which
is what the 2026-08-19 run did.

## Two batches in that number are not clean, and both are known

  * **batch 601-650 crashed** (`rc=134`, 36 of 51 classes): old space
    exhausted by an `Error>>freeze` context storm — 11M Contexts, 2.2M
    Errors. It does **not** reproduce: re-run on an idle machine it is
    36 s / 37 s clean on the current binary, 53 s clean on `26ad35b3` (the
    pre-fix baseline), and 21 s clean with `PHARO_NO_JIT=1`, against 158 s
    and a crash inside the sweep. One occurrence in this run's 41 batches
    against zero in the pre-fix run's 34 is not a distinguishable
    regression, so it is on the record as a single observed storm with a
    good log, not as something introduced today.
  * **batch 801-850 reports 2 classes instead of 51** because I started a
    single-class run while the sweep was in flight and every SUnit runner
    invocation reads the same `/tmp/sunit_class_names.txt`. My interference,
    not the VM's.

Together they accounted for the 64 missing classes (1982 against 2046). Both
were re-run afterwards on an idle machine — 601-650 clean in 33 s, 801-850
clean in 184 s — and merged into the totals above, so those totals are the
figure, not a floor. The per-class table below is from the raw sweep and so
still omits those two batches' classes.

## Every class with a non-zero F or E, and what it is

    class                                            F    E   bucket
    FTTableMorphTest                                 0    1   no display
    SpAthensAdapterTest                              0    2   no display
    SpColorPickerTest                                0    2   no display
    SpComponentListAdapterTest                       0    1   no display
    SpDatePresenterTest                              1    0   no display
    SpJobListPresenterTest                           1    0   no display + CI-skipped
    SpListCommonPropertiestTest                      0    5   no display
    SpTableCommonPropertiestTest                     0    3   no display
    SpTreeAdapterMultipleSelectionTest               0    2   no display
    SpTreeAdapterSingleSelectionTest                 0    1   no display
    SpTreeTableAdapterMultiColumnMultiSelectionTest  1    1   no display
    SpTreeTableAdapterMultiColumnTest                1    1   no display
    SpTreeTableAdapterSingleColumnMultiSelectionTest 1    1   no display
    SpTreeTableAdapterSingleColumnTest               1    1   no display
    SpTreeTableSearchTest                            0    1   no display
    StDebuggerActionModelTest                        1    0   no display
    StDebuggerInspectorTest                          1    0   no display
    StDebuggerTest                                   3    0   no display + CI-skipped
    StSpotterModelTest                               2    0   no display + CI-skipped
    StSpotterTest                                    1    0   no display + CI-skipped
    StTranscriptPresenterTest                        3    0   no display
                                                    --   --
                                                    17   22   21 classes

    ReleaseTest                                      4    0   3 harness + 1 defect (+1 T)
    WeakAnnouncerTest                                1    0   the same defect; CI-skipped
    LinkInstallerTest                                1    0   GC-history, NOT the JIT
    EpFileOutModificationsTest                       0    1   17/17 in isolation
    ZnClientTest                                     1    0   testQueryGoogle wants the internet
    SystemDependenciesTest                           1    0   image dependency drift
    TraitTest                                        1    0   untriaged (carried from 2026-08-11)
    TKTWorkerTest                                    0    1   task-kernel timing (+1 T)
    WeakKeyDictionaryTest                            1    0   weak/GC timing
                                                    --   --
                                                    10    2   9 classes

21 of the 30 classes are the missing display. Measured, not assumed: the
same 11 of them run 167 tests with **0 F, 0 E** once
`scripts/pharo-headless-test/setup_fake_gui.st` is filed in ahead of the
runner (`docs/WIP.md`).

Four of the GUI/tool failures — `SpJobListPresenterTest`, `StDebuggerTest`,
`StSpotterModelTest`, `StSpotterTest` — carry
`skipOnPharoCITestingEnvironment`, so Pharo's own CI does not run them and
they cannot be read as "Cog passes this".

`OCClassBuilderTest` and `RSLinesTest`, both in the pre-reboot residual, are
clean in this run.


## The residual, re-measured against the fake-GUI image

All 30 classes that scored a non-zero F or E in the sweep above, re-run
against an image with `scripts/pharo-headless-test/setup_fake_gui.st` filed
in ahead of the runner:

    == 30 classes   829 tests   800 P   13 F   2 E

against **27 F + 25 E = 52** for those same classes in the plain sweep. The
prelude removes 39 of the 52. Every `Sp*` adapter passes; what remains is:

    class                        F   E   what it is
    LinkInstallerTest            1   0   GC-history dependent, NOT the JIT
    WeakAnnouncerTest            1   0   dead weak subscriptions; CI-skipped upstream
    StDebuggerTest               2   0   one of them CI-skipped upstream
    StSpotterModelTest           2   0   one of them CI-skipped upstream
    StSpotterTest                1   0   CI-skipped upstream
    StDebuggerActionModelTest    1   0   GUI/debugger timing
    StDebuggerInspectorTest      1   0   GUI/debugger timing
    ReleaseTest                  2   0   harness artifacts (was 4 without the prelude)
    SystemDependenciesTest       1   0   image dependency drift, filed upstream
    ZnClientTest                 1   0   testQueryGoogle reaches the public internet
    FTTableMorphTest             0   1   still fails with a display present
    TKTWorkerTest                0   1   task-kernel timing
                                --  --
                                13   2

Nothing in that list is a demonstrated VM computation error. Four are tests
Pharo's own CI does not run, one needs the internet, one is image drift, two
are harness artifacts, and `LinkInstallerTest` is separately characterised as
GC-history dependent with the JIT ruled out on both settings.

So a full sweep run against a fake-GUI image would land near **13 F / 2 E**
rather than 27 F / 25 E. That has NOT been run end to end — this is the 30
residual classes measured directly, which is the same population but not the
same experiment.

## 2026-08-23: ReleaseTest's failures, each cause named

These have been carried as "harness self-pollution" collectively. Run
individually with a 1200 s limit, they are three DIFFERENT causes and only one
is ours:

    testNoOrphanPackage
        TestFailure: an Array(a Package(Tests-Runner)) should have been empty

      OURS. The test asserts every package in the organizer is declared by some
      BaselineOf:

          self assertEmpty: (self packageOrganizer packages reject: [ :package |
              package isUndefined or: [ declaredPackages includes: package name ] ])

      and its own comment says it exists "to detect generated packages that are
      not removed by the #tearDowns" -- exactly what `run_sunit_tests.st:91`
      does by defining SUnitRunner into package 'Tests-Runner'. This is the
      harness contaminating a whole-image test, not a VM defect.

    testUnknownProcesses
        MessageNotUnderstood: DefaultExecutionEnvironment >> #watchDogProcess

      NOT ours and not about the runner's processes at all -- the image's
      DefaultExecutionEnvironment does not implement #watchDogProcess. An image
      bug; belongs in docs/image_issues.md.

    testNoDeadSubscriptions
        TestFailure: Got an Array(a WeakAnnouncementSubscription (nil subscribes
        to ClassRemoved) ... MethodRemoved ... MethodAdded ... MethodModified)
        instead of #().

      The known dead-weak-subscription/finalization issue, already tracked (see
      docs/vm-compat-bugs.md; WeakAnnouncerTest reproduces it in 13 s). Four
      subscriptions with nil subscribers survive. GENUINELY OURS.

### Attempted fix for testNoOrphanPackage, not landed

Giving 'Tests-Runner' a declaring baseline would make the harness conform to
the image's packaging contract rather than hide the failure. Two attempts both
failed on Pharo 13 class-definition API, in a scratch image:

    BaselineOf subclass: #BaselineOfTestsRunner ... package: ...
        -> Error: Instance of BaselineOf class did not understand
           #subclass:instanceVariableNames:classVariableNames:package:

    (BaselineOf << #BaselineOfTestsRunner) package: 'BaselineOfTestsRunner'
        -> the class is not installed; the next `Smalltalk at:` raises
           KeyNotFound: key #BaselineOfTestsRunner not found in SystemEnvironment

Whoever picks this up: find the API BaselineOf actually accepts in Pharo 13
(and note the new baseline's OWN package must be declared too, or the test just
moves to complaining about `BaselineOfTestsRunner`). Worth one test per arch,
so it is low priority -- but it is a real harness defect, not a VM one.

## 2026-08-23: arm64's 7 TIMEOUTs re-run at timeoutScale=20

Same treatment x86_64's nine got (see `sweep-x86-final-2026-08-22.md`):
200 s per test, 230 s watchdog, classes taken from the sweep's own TIMEOUT rows.

    IntervalTest                  260 P   0 F  0 E     was TIMEOUT
    MCSmalltalkhubRepositoryTest    1 P   0 F  0 E     was TIMEOUT
    STONTest                        9 P   0 F  0 E     was TIMEOUT
    TKTWorkerPoolTest               9 P   0 F  0 E     was TIMEOUT
    TraitMethodDescriptionTest      4 P   0 F  0 E     was TIMEOUT
    ReleaseTest                    38 P   4 F  0 E  1 S  + 1 TIMEOUT
    TKTWorkerTest                   5 P   0 F  2 E     was TIMEOUT

    batch: 326 pass, 4 fail, 2 error, 1 timeout

Five of seven pass completely. The bound was hiding real information in the
other two, which is the point of doing this on both arches:

  * `ReleaseTest` — the 4 FAILs are the ones characterized above by name
    (`testNoDeadSubscriptions`, `testNoOrphanPackage`,
    `testThatThereAreNoSelectorsRemainingThatAreSentButNotImplemented`,
    `testUnknownProcesses`), plus `testNoShadowedVariablesInMethods` still
    TIMEOUT at 230 s — another whole-image scan.

  * `TKTWorkerTest` — 2 ERRORs the watchdog had been masking:
    `testWorkerProcessDiesAfterWorkerAndAllFuturesAreCollected` and
    `testWorkerProcessIsWorkingUntilAllTasksAreDone`.

    **Both PASS run alone** with a 600 s limit, so they are context-dependent
    under the runner (TaskIt worker tests are sensitive to other live
    processes), not demonstrated VM defects. Same shape as
    `testUsingMethodsFFI`. Recorded because a TIMEOUT row hid them entirely,
    not because they are known to be ours.

Note the general lesson: TIMEOUT rows are not just "slow" — raising the bound
converts most to passes but can also expose F/E underneath. Neither arch's
TIMEOUT count should be read as "nothing to see".
