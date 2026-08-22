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
