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

## 2026-08-23: the x86_64 TIMEOUTs are the watchdog, not defects

The sweep's own outcome breakdown keeps TIMEOUT separate from FAIL/ERROR, so
these were never counted as failures — but they were not passes either, and
x86_64 had roughly twice arm64's count, which is the Rosetta factor against a
fixed bound rather than anything about the VM:

    arch     PASS    SKIP  FAIL  ERROR  TIMEOUT
    arm64   27225     178    27     24        7
    x86_64  27689     180    25     22       13

The runner already scales this: `timeoutScale` (default 5) sets
`TestCase>>defaultTimeLimit` to 50 s and the watchdog to 80 s, read from
`/tmp/sunit_timeout_scale.txt`. Re-running x86_64's nine timed-out classes at
`timeoutScale=20` (200 s per test, 230 s watchdog):

    TraitFileOutTest            4 P   0 F  0 E     was TIMEOUT
    TKTWorkerTest               7 P   0 F  0 E     was TIMEOUT
    TKTWorkerPoolTest           9 P   0 F  0 E     was TIMEOUT
    STONTest                    9 P   0 F  0 E     was TIMEOUT
    RBBrowserEnvironmentTest   27 P   0 F  0 E     was TIMEOUT
    IntegerTest                80 P   0 F  0 E     was TIMEOUT
    FreeTypeCacheTest          25 P   0 F  0 E     was TIMEOUT (2 tests)
    SelfVariableTest            4 P   + 1 TIMEOUT  still
    NoUnusedVariablesLeftTest         TIMEOUT      still (>230 s)

    batch total: 167 pass, 0 fail, 0 error, 3 skip, 2 timeout

Seven of the nine pass completely once given time. Anyone comparing the two
architectures should raise `timeoutScale` for x86_64 the same way
`PER_BATCH_TIMEOUT` and the package tier's `PER_TEST_TIMEOUT` are raised.

### CORRECTIONS: all 13 are bound artifacts — no x86-only defect

This section has been wrong twice, both times from reading summary COUNTS
instead of per-test NAMES — the exact trap
`scripts/package-tests-selfhosted.sh`'s header warns about. The verified
per-test picture, from `/tmp/sunit_test_detail.txt`:

    test                                  arm sweep  x86 sweep  x86 @scale60  x86 alone
    testUsingMethods                      PASS       TIMEOUT    PASS          PASS (48s)
    (arm64 "LOST" = class header present, zero per-test rows: a cycle restart)
    testNoUnusedInstanceVariablesLeft     PASS       TIMEOUT    PASS          -
    testNoUnusedTemporaryVariablesLeft    LOST       TIMEOUT    TIMEOUT       PASS (378s)
    testUsingMethodsFFI                   PASS       PASS       FAIL          FAIL

  * **Twelve of x86_64's thirteen TIMEOUTs are the bound.** They pass once
    given time. `testUsingMethods` in particular takes only 48 s run alone —
    it was never close to a real hang.

  * **The thirteenth is not a defect either — it is the same story, larger.**
    `NoUnusedVariablesLeftTest>>testNoUnusedTemporaryVariablesLeft` run alone
    with a 900 s limit **PASSES on both architectures**:

        arm64    230358 ms   3.34 billion sends   PASS
        x86_64   378238 ms   3.64 billion sends   PASS

    1.64x wall clock with send counts within 9% — ordinary Rosetta slowdown on
    a genuinely enormous test, not an x86 pathology.

    Its `TIMEOUT(prim-stuck)` label is a misnomer here. Sampling the x86_64
    process mid-run shows it at 99% CPU in ordinary interpretation
    (`interpret` -> `dispatchBytecode` -> `returnValue` ->
    `tryJITResumeInCaller` -> `primitiveFullClosureValue` -> `activateBlock`),
    with the send counter advancing ~11 M/s. Nothing is blocked in a C
    primitive; the watchdog simply never gets to run either.

    **arm64 did not pass this class in the sweep — it lost it.** `all_results`
    has the `=== NoUnusedVariablesLeftTest ===` header preceded by
    `CYCLE-LOOP-RESTART`, and `all_detail.txt` has ZERO per-test rows for it.
    Absence from the non-PASS list is not a pass, which is how it briefly got
    written up here as "passes on arm64".

    So: **all thirteen x86_64 TIMEOUTs are bound artifacts, and no x86_64-only
    SUnit defect was found.** A test needing 230 s on the fast arch cannot fit
    a 50 s limit or an 80 s watchdog on either.

An earlier revision of this section claimed `testUsingMethods` "does not run
out of time at 600 s — it fails". That was wrong: the 1 FAIL in that batch's
counts was `testUsingMethodsFFI`, a different test.

### `testUsingMethodsFFI` is a precondition, not an x86 defect

It PASSES in both sweeps, so it contributes nothing to the published numbers.
It fails only in a bare clean eval on x86_64 — and it passes there on arm64 —
because of what it asserts:

    var := self class lookupVar: #self.
    self assert: (var usingMethods anySatisfy: [:method |
        method isFFIMethod and: [ method readsSelf not ]])

i.e. "some FFI method has already been RUN", since Pharo rewrites an FFI
method on first call and the rewritten form no longer pushes self. Same image,
same clean eval, identical method populations:

    arch     usingMethods   isFFIMethod   FFI with readsSelf=false
    arm64         103942           744          7
    x86_64        103942           744          0

The seven arm64 rewrites are `FT2Library>>ffiInitFreeType:`,
`FT2Library>>ffiNewFace:fromMemory:size:index:`,
`FT2Library>>ffiGetBitmap:fromOutline:`, `FT2Face>>ffiLoadChar:flags:`,
`FT2Face>>ffiSetPixelWidth:height:`, `LGitLibrary>>libgit2_init` and
`LibC>>memCopy:to:size:` — five FreeType, one libgit2, one libc.

Not a missing-library problem: `build-x86/` stages x86_64 `libfreetype` and
`libgit2` (and the FFI stubs in `FFI.cpp` only register when the real library
is absent), and `LibC>>memCopy:to:size:` is plain libc either way. So the
difference is which FFI calls the image makes during startup on each arch, not
whether the rewrite works. Worth understanding, but it is not a failure — and
it means a bare `eval` is NOT a valid environment for this test.

## 2026-08-23: post-fix residual on x86_64, against arm64

Same prepped image as the arm64 run (Spur images are architecture-neutral):
`setup_fake_gui.st` + the fixed runner, snapshot, bare-launched through
`SUnitRunner>>startUp:`. `timeoutScale=60` to give x86_64 the equivalent
headroom of arm64's 30, since Rosetta runs ~2x slower.

    arch     classes  pass  fail  error  skip  timeout
    arm64      30      800    13     3    12      1
    x86_64     30      798    15     4    12      1

**x86_64's set is arm64's plus exactly three**, and none is a demonstrated VM
defect:

    TKTWorkerTest>>testWorkerProcessIsWorkingUntilAllTasksAreDone     ERROR
        TaskIt worker timing; PASSES run alone with a 600 s limit (measured on
        arm64 earlier today, same test).

    SpJobListPresenterTest>>testJobIsFinishedWhenWaitingMoreThanWorkBlockDuration
        FAIL. A duration test by its own name, and one of the five on
        upstream's `skipOnPharoCITestingEnvironment` list.

    EpFileOutModificationsTest>>testMethodModificationWithWideString  FAIL
        PASSES run alone on BOTH arches. Not an encoding difference.

    (Isolating that last one needs `on: Notification do: [ :n | n resume ]`:
     the test raises `ProvideAnswerNotification: 'Filed out to: ...'`, and a
     bare `on: Exception do:` catches it and reports it as though it were the
     failure. It is not.)

So the two architectures' residuals differ only by three timing/context-
dependent cases, and the shared 17 are the set attributed by name in
`sweep-arm-final-2026-08-22.md` — 6 established not-ours, 2 that pass in
isolation, 1 slow whole-image scan, 8 known-flaky GUI/debugger cases.
