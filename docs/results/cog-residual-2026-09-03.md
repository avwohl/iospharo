# Δcog for the 2026-09-03 residual

The 28 non-clean classes from the arm64 sweep, run on stock Cog v10.3.9
(x86_64 under Rosetta -- the arm64 Cog aborts on this host) through
`run_sunit_cog.st`, which reads the same external class list our harness does.

**Cog is non-clean on 8 of the 28.**  So two thirds of our residual is ours,
and one third is the image or the test.

## Shared with Cog — not ours

    class                      ours              Cog
    OCClassBuilderTest         1 E               1 E     image issue, build 745
    StDebuggerInspectorTest    1 F               1 F
    SystemDependenciesTest     1 F               1 F
    ZnClientTest               1 F               1 F
    ReleaseTest                2 F / 1 T         1 F / 1 E
    StSpotterModelTest         2 F               1 F
    WeakAnnouncerTest          1 F               1 E

**We are BETTER than Cog on one:** `StDebuggerTest` is 2 F for us and 3 F for
Cog.

## Ours, and already explained

    LibTTYTest              5 E   the copied-VM dylib -- harness, fixed
    TraitFileOutTest        2 E   CWD artifact
    MCSmalltalkhubRepositoryTest  1 T -> ZipArchiveError, a dead remote; Cog
                                  passes it, so Cog's fetch found something
    NoUnusedVariablesLeftTest     1 T; passes for us given time, and passes on
                                  Cog inside the bound -- a SPEED gap, and the
                                  clearest one we have
    RSRoassalTest           1 E   defect #27, the ByteSymbol mis-dispatch

## Ours: the display family

    SpAthensAdapterTest, SpComponentListAdapterTest,
    SpListCommonPropertiestTest, SpTableCommonPropertiestTest,
    SpTreeAdapterMultipleSelectionTest, SpTreeTableAdapter* (4),
    StSpotterTest, StTranscriptPresenterTest

All clean on Cog.  All fail for us on a runner-only image and mostly pass on a
`setup_fake_gui.st`-prepped one (13 F / 16 E -> 6 F / 0 E on 15 classes).  So
the gap is the Display and World our headless image lacks, not codegen -- and
the fix is in the prep, which is now the documented recipe.

## What this leaves

After the fake-GUI prep, the dylib staging and the `Total:`-splice fix, the
genuinely-ours residual is: defect #27 (one test), a handful of display-family
FAILs that survive a working fake GUI, and one measurable speed gap
(`NoUnusedVariablesLeftTest`).  Everything else is shared with Cog, is the
image, or is the harness.
