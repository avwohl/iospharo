# Pharo 15: the defects of the Pharo 14 list (2026-11-20)

This file records the check of the list of
[nonpassing-tests.md](nonpassing-tests.md) on Pharo 15.  The question: is
each defect of the Pharo 14 list still a defect in Pharo 15?

- Image: Pharo 15.0 build 141 (`b4d2dfd`), 2026-11-20, from
  `get.pharo.org/64/150+vm`.  The image has 2254 test classes (Pharo 14:
  2160).
- Stock VM: v12.0.6-beta (`bebdbaca8`), the same VM as for Pharo 14.  The
  VM name in `scripts/vmrun.sh` is `cog15`.
- Data: [results/2026-11-20-pharo15/](results/2026-11-20-pharo15/) (the
  four sweeps, the comparison with the list, the comparison of the sources).

## Result

All 220 tests of the Pharo 14 list still do not pass on a clean Pharo 15
image, on stock Cog and on our VM.  Our fix entries repair them on Pharo 15
as on Pharo 14, with one exception (`RGMCClassTest`, below).  Pharo 15
repaired one defect of the list itself: the rename of a class in
`RBNamespace` (fix entry 31).

## Comparison of the sources

- 215 of the 220 tests have the same source in Pharo 15.  Two tests
  changed, and the defect stays:
  `ClyExtendingPackagesQueryTest>>testFromClassWhichTraitIsExtendedButNotItself`
  (format only, the expected-failure mark stays) and
  `TFUFFICallbackTest>>testCallbacksInOtherThread` (a new comment says that
  the skip of the same-thread runner is on purpose: the crash cannot be
  caught.  The Cog change of [upstream/](upstream/) repairs the crash).
- Of the methods that the fix entries replace, 153 have the same source in
  Pharo 15 and 5 differ.  Pharo 15 changed
  `RBNamespace>>renameClass:to:around:` and its two tests: the reference
  changes now go inside the rename change.  That repairs the defect of fix
  entry 31 in another way.
- The fix entries on Pharo 15: 161 `FIXED`, 51 `ADDED`, 9 `SKIP`.  The 9
  skips are the 6 Pharo 13 fixes of entry 10 (they skip on Pharo 14 too) and
  the 3 methods of entry 31.

## The runs

| Run | Total | Pass | Fail | Error | Skip | Timeout | Expected failure |
|---|---|---|---|---|---|---|---|
| Stock Cog, clean image (fake GUI only) | 40348 | 40072 | 5 | 2 | 219 | 1 | 49 |
| Our VM, clean image (fake GUI only) | 40348 | 40072 | 5 | 2 | 219 | 1 | 49 |
| Stock Cog, with the fix entries | 40361 | 40262 | 8 | 0 | 90 | 0 | 1 |
| Our VM, with the fix entries | 40361 | 40268 | 3 | 0 | 89 | 0 | 1 |
| Our VM, interactive pass, with the fix entries | 135 | 135 | 0 | 0 | 0 | 0 | 0 |
| Pharo CI, Pharo15 build 141, each of Linux, macOS, Windows | 40348 | 40348 | 0 | 0 | 0 reported | 0 | 0 reported |

The Pharo CI counts the same 40348 tests.  Its report records a skip and an
expected failure as a pass (see the first section of
`scripts/pharo-bugs-site/site/index.html`).

On the clean image, the 220 tests: the 132 of part A1 do not pass (84
skips, 46 expected failures, the failure of `ZnClientTest>>testQueryGoogle`,
and `TFUFFIFunctionCallTest>>testLongCalloutDoesNotBlockImage`, which skips for
the same-thread runner).  The 4 of part A2, the 56 of part A3 and the 28 of
part A4 do not pass.

With the fix entries, the tests that do not pass on stock Cog and pass on
our VM are the tests that need a Cog change: `CodeSimulationTest>>testErrorCodeNotFound`,
the four `WriteBarrierTest` tests of the typed stores, and the same-thread
variant of `TFUFFICallbackTest>>testCallbacksInOtherThread`.

## With all fixes, after the repairs of 2026-11-20

The image fix entries with the repairs below (`20-RGMCClassTest.st`
extended, new entries 37 and 38), each image prepared from a pristine image:

| Run | Total | Pass | Fail | Error | Skip | Timeout | Expected failure |
|---|---|---|---|---|---|---|---|
| Pharo 15, our VM | 40361 | 40273 | 0 | 0 | 87 | 0 | 1 |
| Pharo 15, stock Cog | 40361 | 40267 | 5 | 0 | 88 | 0 | 1 |
| Pharo 14, our VM | 39465 | 39377 | 0 | 0 | 87 | 0 | 1 |
| Pharo 14, stock Cog | 39465 | 39371 | 5 | 0 | 88 | 0 | 1 |
| Interactive pass, our VM, Pharo 14 and Pharo 15 each | 135 | 135 | 0 | 0 | 0 | 0 | 0 |

On the two versions, the 87 skips and the expected failure of our VM are
the tests of parts A3 and A4 of the list (they cannot apply on macOS arm64,
or need a display; the display tests pass in the interactive pass).  Stock
Cog has 6 more: the tests that need the Cog changes of
[upstream/](upstream/) (`CodeSimulationTest>>testErrorCodeNotFound`, four
`WriteBarrierTest` tests, and the same-thread variant of
`TFUFFICallbackTest>>testCallbacksInOtherThread`).

A harness defect showed in the first Pharo 15 sweep of our VM: 199 failures
and 685 errors, on source code that read as decompiled text and comments
that read nil.  The image directory had the `.sources` files of Pharo 14 and
Pharo 15, and `scripts/sunit-sweep.sh` took the first one.  The script now
takes the `.sources` file whose name is in the image, and stops when no file
or more than one file matches.  The sweep again with the repaired script is
the row "Pharo 15, our VM" above.

## New in Pharo 15

The clean image has 8 tests that do not pass and that are not in the list.
Four are defects that a fix entry repairs already (they pass on Pharo 14
because the sweep images have the entries):
`DocCommentsReleaseTest` for `Context class>>tallyInstructions:` (entry 24),
`StTarjanAlgorithmTest>>testNoOutgoingDependenciesAfterTarjan` and
`OutOfMemoryTest>>testSignalOfTheLowSpaceFromTheVM` (entry 23),
`TFUFFICallbackTest>>testCallbackWithErrorHanlded` (entry 10, fix 7).

Found on Pharo 15, on stock Cog and on our VM, and repaired (2026-11-20):

1. `ZnBufferedReadWriteStreamTest>>testReadThenWrite` and `testWriteThenRead`
   skip.  The guard is `SystemVersion current build < 690`.  The build
   counter starts again for each Pharo version (this image is build 141),
   so the two tests skip in each Pharo 15 build before 690.  A defect of
   the tests.  Fix entry `37-znbuffered-build-guard.st`: the 11 tests of
   the class pass on Pharo 14 and Pharo 15, on stock Cog and on our VM.
2. `MCStWriterTest>>testClassMethodDefinition` and `testMethodDefinition`
   fail in the sweep (the writer writes `stamp: 'nil'`) and pass when the
   class runs alone.  `MCPackageTest` runs before them: it unloads the mock
   package, and its `tearDown` installs the snapshot again.  The methods
   that the install makes have no stamp.  Pharo 14 has the same defect: the
   two classes ran in different batches (two images) of the Pharo 14
   sweeps, and in one batch of the Pharo 15 sweeps.  Two causes in the
   image: `MCMethodDefinition>>loadWithLoader:` gives the installer the
   instance variable `timeStamp`, which stays nil for a definition with a
   lazy stamp.  And the load runs inside `deferFlushDuring:`, where
   `FilesSourcesManager>>timeStampAt:for:` answers `''`.  Fix entry
   `38-mc-load-keeps-method-stamp.st`.
3. `RGMCClassTest>>testClassesWithTraits` fails with the fix entries:
   "Got `#(#'#IsExtentRememberingEnabled')` instead of
   `#(#IsExtentRememberingEnabled)`".  Pharo 15 has a class with traits and
   a class variable (`SpPresenter`).  The Ring definition keeps the `#` in
   the name of a class variable.  Fix entry `20-RGMCClassTest.st` repaired
   the slot names and now repairs the class variable names too: the 50
   tests of `RGMCClassTest`, `RGMCPackageTest`,
   `RGMCTraitCompositionVisitorTest` and `Ring2ChunkImporterTest` pass on
   Pharo 14 and Pharo 15, on stock Cog and on our VM.
