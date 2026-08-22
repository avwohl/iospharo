# arm64 full SUnit sweep, 2026-08-22 — after the day's fixes

`scripts/sunit-sweep.sh` equivalent at STEP=50, `PHARO_CODE_ZONE_MB=192
PHARO_MAX_STEPS=4000000000000`, on the prepped SUnit-runner image, machine
otherwise idle. Started 09:31, totals at 11:23.

    === TOTALS ===
      classes 1982
      tests   27538
      PASS    27210
      FAIL       27
      ERROR      24
      SKIP      178

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

Together they account for the ~65 missing classes (1982 against 2047). Both
are re-run separately; the totals above are a floor, not a final figure.

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
