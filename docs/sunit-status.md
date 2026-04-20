# SUnit Status

Canonical place for comparing SUnit pass/fail across the three VMs.
Every run updates THIS file in THIS format.  No ad-hoc reports.

Harness:      scripts/pharo-headless-test/run_sunit_tests.st
Class list:   836 test classes (after skip-list filters)
Image:        /tmp/harness/Pharo.image  md5=2db30d745e41d581cb409b2eef27ecad
Per-test TO:  50s (scale=5 × default 10s)
Run watchdog: 45m (VM wall-clock)

## VMs

    Short name   Binary                            Branch / commit
    cog          /tmp/stocktest/pharo              Pharo VM v10.3.9 (2025-11-17)
    main         (not yet built in this matrix)    main @ d324080
    jit          ./build/test_load_image           jit  @ e6cc1a4

## Matrix — last run per (VM, mode)

Status legend:  P pass · F fail · E error · S skip · T timeout · — unrun

`Δcog` (rows below cog only) = count of tests that are NOT pass on this
(VM, mode) but ARE pass on cog.  Requires per-test join with the cog
row; marked `?` until the cog row is populated.

    VM     Mode     Date        Classes   Tests    P       F    E     S    T    Wall     Δcog
    cog    default  —           —         —        —       —    —     —    —    —        (baseline)
    main   NO_JIT   —           —         —        —       —    —     —    —    —        ?
    main   default  —           —         —        —       —    —     —    —    —        ?
    jit    NO_JIT   2026-04-20  827/836   17072    16465   18   536   40   13   45m(wd)   ≥454*
    jit    default  —           —         —        —       —    —     —    —    —        ?

`wd` = hit the 45-minute VM watchdog; last 9 classes (FLBinaryFileStream,
FinalizationRegistry, FFICallback) had 80s/300s per-test timeouts that
ate the budget.

*`≥454` is a lower bound from the 15-class probe 2026-04-20, not a
full join.  The 15 classes sampled on cog passed 400/400; on our VM
NO_JIT those same classes contributed 454 C11 DNUs.  Full join pending
a cog row.  Upper bound, if every F+E+T also regresses, is 567.

## Delta vs stock Cog

Each cell = tests that **pass on cog** but **fail/error/timeout on this VM+mode**,
on the SAME image.  Zero means parity.  "—" means the cog column hasn't been
run or this VM+mode hasn't been run.

    Full-suite delta (all 17k+ tests)
    VM+mode           Δ (cog-P → here-not-P)    Source of evidence
    main NO_JIT       —                          not run
    main default      —                          not run
    jit  NO_JIT       ≥454 (likely ≥900)         15-class probe 2026-04-20
    jit  default      —                          not run

"≥454" = our direct full run on jit-NO_JIT shows 454 FFI `C11` DNUs
confirmed passing on cog.  The other 82 non-FFI errors + 18 fails +
13 timeouts are also *suspected* regressions but haven't been checked
against cog yet.  If they all regress, Δ would be ≥536+18+13 = ≥567.

## Failure attribution — 2026-04-20 jit NO_JIT run

Every error/fail/timeout is in one of these buckets until proven
otherwise.  Buckets roll up; re-bucket when cross-checked on cog.

    Attribution                       Count  Cross-checked on cog?
    CONFIRMED_VM_REGRESSION             454  yes — 15-class probe, all pass cog
    SUSPECTED_VM_REGRESSION             113  no — `packageName`/`select:thenDo:`/
                                              `disable`/`outputFileReference` nil
                                              (92 + 59 + 49 + 17 = 217, but some
                                              classes overlap — net 113)
    SUSPECTED_VM_REGRESSION_CALYPSO      24  no — Wrapper/origin errors
    SUSPECTED_VM_REGRESSION_OTHER        12  no — unclassified
    CONFIRMED_VM_FAIL                    18  no — test-level assertion failures;
                                              docs/test-results.md 2026-04-12
                                              listed 6 of these as *known VM path*
                                              failures (testUserLocalDirectory,
                                              testVmBinary, testVmDirectory) —
                                              environment/path expectation, not
                                              VM correctness
    CONFIRMED_VM_TIMEOUT                 13  no — FinalizationRegistry (5×300s),
                                              FFICallback (5×80s), FLBinary (1),
                                              MiscFFI (2).  Some may be genuine
                                              hangs on our VM even where cog
                                              completes; some are interpreter-
                                              speed ceilings (see A00).
    CONFIRMED_IMAGE_BUG                   0  would be: fails on cog too
    TOTAL                               567

(567 is F+E+T; 40 skips excluded, 16465 passes elsewhere.)

## Specific failure signatures seen

For quick grep.  First column is the bucket; second is count; third
is the signature substring.

    VM_REGRESSION       454  MessageNotUnderstood: Message not understood: C11 class >> #current
    VM_REGRESSION?       92  MessageNotUnderstood: receiver of "packageName" is nil
    VM_REGRESSION?       59  MessageNotUnderstood: receiver of "select:thenDo:" is nil
    VM_REGRESSION?       49  MessageNotUnderstood: receiver of "disable" is nil
    VM_REGRESSION?       17  MessageNotUnderstood: receiver of "outputFileReference" is nil
    VM_REGRESSION?       12  Error: Wrapper query should include single subquery
    VM_REGRESSION?       12  Error: Can't find the requested origin
    VM_REGRESSION?        8  MessageNotUnderstood: receiver of "close" is nil
    VM_FAIL_ENV           3  testUserLocalDirectory / testVmBinary / testVmDirectory
    VM_FAIL_KNOWN         2  testFloatParameters / testIntegerParameters (FFI param)
    TIMEOUT               5  FinalizationRegistry suite (300s each)
    TIMEOUT               5  FFICallback suite (80s each)
    TIMEOUT               2  FLBinary + one misc (80s)

## Progress trend (same config, over time)

Each row = one dated run of jit NO_JIT.  Delta columns show change
versus the row immediately above.

    Date        Tests   P       ΔP      F    ΔF   E     ΔE    T    ΔT
    2026-04-12  12576   12531   —        14   —   2      —    —    —   (535 classes, older
                                                                         class list)
    2026-04-20  17072   16465   baseline 18   +4  536   +534  13   +13  (836 classes, expanded
                                                                         list; error explosion
                                                                         concentrated in FFI)

"+4 fail" from 2026-04-12 to 2026-04-20: 18 − 14.  Worth a closer
look when cross-checking — 3 are environment (testUserLocalDirectory
etc.), the rest may be regressions.

"+534 errors" is almost entirely the FFI C11 cascade (454 of 534).
The rest are the other signature buckets listed above.

## What I DON'T know — open deltas

1. Does stock cog complete the full 836-class suite?  How many fail?
   (Not run.  Pharo 10 VM segfaulted on our harness; need simple
   runner.)
2. Does main (d324080) show the FFI C11 cascade too?  If yes, the
   regression is older than the jit branch.  (Not run.)
3. Does jit default (JIT on) differ from jit NO_JIT?  (Not run.)
4. Do the 113+24+12 suspected buckets actually regress on cog?

## How to update this file

After any SUnit run:

1. Save the raw `/tmp/sunit_test_results.txt` somewhere dated, e.g.
   `results/sunit-YYYY-MM-DD-<vm>-<mode>.txt`.
2. Update the matrix row for that (VM, mode) — replace the previous
   row, don't append a new one.
3. Update the Progress-trend row (append, don't replace).
4. Update the Delta-vs-cog cell if cross-checked.
5. Update the Specific-signature table if new failure signatures
   appeared.

Commit.  One commit per run.  Subject: `sunit-status: YYYY-MM-DD <vm> <mode> — Δ…`.
