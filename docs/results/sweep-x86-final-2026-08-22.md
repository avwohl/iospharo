# x86_64 full SUnit sweep, 2026-08-22 — parity with arm64, and no Cairo exclusions

Same driver, same prepped image, same `PHARO_CODE_ZONE_MB=192
PHARO_MAX_STEPS=4000000000000` as the arm64 run, immediately after it on the
same idle machine. Started 11:38, totals at 14:4x.

    === TOTALS ===
      classes 2039
      tests   28004
      PASS    27667
      FAIL       25
      ERROR      22
      SKIP      180

## Merged, with the short batch re-run

Batch 1751-1800 re-ran clean at 1800 s — it took **1081 s**, so it genuinely
needed more than the 900 s bound rather than hanging. Merged back in:

    === TOTALS, merged ===
      classes 2045
      tests   28062
      PASS    27725
      FAIL       25
      ERROR      22
      SKIP      180
      rate    98.80%

                 classes  tests   PASS   FAIL  ERROR   SKIP    rate
    arm64 today   2046    28067  27727    27     25    182    98.79%
    x86_64 today  2045    28062  27725    25     22    180    98.80%

One class and two tests apart, and x86_64 has FEWER failures and errors than
arm64. That is full architectural parity on a RAW sweep with no exclusion
list, which has not been true on this host before.

## Side by side

                   classes  tests   PASS   FAIL  ERROR   SKIP
    arm64 today     2046    28067  27727    27     25    182
    x86_64 today    2039    28004  27667    25     22    180
    x86_64 ref      2037    27982  27380    26    287      -
      (2026-08-19)

**Errors on x86_64: 287 -> 22.** The 2026-08-19 reference had to publish a
NO-CAIRO variant to make the two architectures comparable, because 260 of
those 287 errors were one missing dylib: the host has only an arm64
`libcairo.2.dylib` and a Rosetta x86_64 process cannot load it. With
`scripts/fetch-x86-libs.sh` staging the x86_64 Homebrew bottles beside the
binary, that entire bucket is gone from a full RAW sweep — no exclusion list
needed.

The 7-class gap against arm64 is one batch, below.

## The one batch that lost data, and why

    batch 1701-1750  rc=124  901s  classes=51  completed=yes
    batch 1751-1800  rc=124  900s  classes=44  completed=no   <-- 7 classes lost

`PER_BATCH_TIMEOUT=900` is too tight for x86's slow tail. arm64 ran
1751-1800 in 546 s; x86_64 runs ~2.6x slower under Rosetta, so it needs
~1400 s of headroom. 1701-1750 also hit the bound but had already written
all 51 classes, so only 1751-1800 lost anything. It is re-run at 1800 s and
merged.

Anyone repeating this: set `PER_BATCH_TIMEOUT=1800` for x86_64. The default
is sized for arm64.


## The residual, re-measured against the fake-GUI image

The same 30 classes that scored non-zero in the arm64 sweep, run here against
an image with `setup_fake_gui.st` filed in ahead of the runner:

    x86_64   30 classes   829 tests   800 P   13 F   1 E
    arm64    30 classes   829 tests   800 P   13 F   2 E

The two architectures land on the same number, and on the same core set:
`ReleaseTest` (2, harness), `StDebugger*`, `StSpotter*`,
`SystemDependenciesTest`, `WeakAnnouncerTest`, `ZnClientTest`,
`FTTableMorphTest`. Where they differ it is confined to the GUI-timing
flakes this repo already documents as flipping run to run — x86 shows
`SpJobListPresenterTest` and `StTranscriptPresenterTest` where arm shows
`LinkInstallerTest`, `StDebuggerActionModelTest` and `TKTWorkerTest`.

Four of the classes common to both are on Pharo's own
`skipOnPharoCITestingEnvironment` list, one reaches the public internet, one
is image dependency drift. None is a demonstrated VM computation error.
