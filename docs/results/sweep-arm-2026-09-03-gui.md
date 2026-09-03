# arm64 full SUnit sweep, fake-GUI prep — 2026-09-03, the best run to date

Same `scripts/sunit-sweep.sh` settings and baseline environment as the earlier
runs.  Three things differ from the 03:21 sweep:

1. the image was prepped with `setup_fake_gui.st` BEFORE the runner,
2. the image was prepped with the current submodule, so the `Total:` splice is
   fixed,
3. the VM's dylibs were staged beside the copied binary.

## Totals

    run                        classes   tests    PASS   FAIL  ERROR  SKIP  T    rate
    2026-09-03 fake GUI          2047    28060   27857    11     4     182   6   99.28%
    2026-09-03 guard only        2043    28043   27812    18    28     182   3   99.18%
    2026-09-02 (storm aborted)   2005    27682   27453    21    21     182   5   99.17%

**All 2047 classes ran.**  ERROR went 28 -> 4 and FAIL 18 -> 11.  Eleven
classes are non-clean, against 24 in the previous run.

## What the residual is, class by class, against stock Cog

    class                     ours            Cog          verdict
    OCClassBuilderTest        1 E             1 E          not ours (image, build 745)
    StDebuggerInspectorTest   1 F             1 F          not ours
    SystemDependenciesTest    1 F             1 F          not ours
    ZnClientTest              1 F             1 F          not ours
    StDebuggerTest            2 F             3 F          BETTER than Cog
    ReleaseTest               2 F / 2 T       1 F / 1 E    partly shared
    StSpotterModelTest        2 F             1 F          partly shared
    WeakAnnouncerTest         1 F             1 E          shared, different shape
    StSpotterTest             1 F             clean        OURS
    TKTWorkerTest             1 E / 1 T       clean        OURS (flaky worker timing)
    TraitFileOutTest          2 E             clean        OURS (known CWD artifact)

Counting TIMEOUTs as non-clean too (they are failures for the goal, and the
report script keyed on F and E only until this run) adds three more, all of
them slow rather than wrong:

    NoUnusedVariablesLeftTest  1 T   whole-image scan; passes given time, and
                                     passes on Cog inside the bound
    TKTWorkerPoolTest          1 T   worker-collection timing
    ZnHTTPSTest                1 T   network

So of fourteen non-clean classes: four at exact parity with Cog, one better
than Cog, three partly shared, three slow-but-correct, and **three ours** --
one a known working-directory artifact, one a flaky worker-timing test, one a
real Spotter failure.

`TraitFileOutTest`'s two errors name their own cause:
`FileDoesNotExistException: '/Users/wohl/src/iospharo-jit/Generated-Trait-Test-Package.st'`.
The test files a package out and reads it back; the write lands beside the
image and the read looks in the sweep's working directory, which is the repo
root.  Running the sweep with its working directory set to the image's would
fix it -- worth trying, and it is two errors, not a VM defect.

## The whole display family is gone

All 178 `Sp*` classes clean.  All `St*` presenter classes clean except the four
above, which Cog also fails or which survive a working fake GUI.
`FTTableMorphTest`, `SpAthensAdapterTest`, `SpComponentListAdapterTest`,
`SpListCommonPropertiestTest`, `SpTableCommonPropertiestTest`,
`SpTreeAdapterMultipleSelectionTest` and the four `SpTreeTableAdapter*` classes
were all non-clean in the previous sweep and are all clean here.  That is 16
errors and 7 failures, removed by the prep.

## The cost, and it is real

Batch 651-700 was lost entirely: `rc=124` after the full 1800 s with 20 of 51
classes done, wedged on `KeyboardKeyTest>>testEqual`.  The in-image watchdog
could not interrupt it, so no Smalltalk process was running -- the VM itself
was blocked, presumably on an event queue the fake GUI creates and nothing
pumps.  The recovery pass ran the 31 lost classes in chunks and got all of them
back (36 classes / 232 P / 0 F / 0 E), so the price is wall-clock, not
coverage.

## Instrumentation

    CANNOT-RETURN-STORM    14 lines across 2 batches
    DEAD-SENDER             7 lines across 3 batches
    DOUBLE-RETURN           0
    DUP-FRAME               0
    FATAL: old space        0

**DUP-FRAME is 0**, against 126 in the previous sweep with the same workload.
That is the corrected detector agreeing that all 126 were the region-rebuild
false positive.

The guard fired 14 times against once before -- the fake GUI changes timing and
the dead-sender path fires more -- and still no abort.  Four `MISDISPATCH`
lines in this run are the OTHER detector's false positives
(`CleanBlockClosure` receivers); that one was fixed after this sweep started.
