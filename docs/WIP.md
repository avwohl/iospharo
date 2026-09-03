# WIP — 2026-08-23

Current state only. This file was 4882 lines of stacked session entries going
back to 2026-05-27; on 2026-08-23 it was pruned the same way `docs/deferred.md`
was on 2026-08-12 — the history moved to
`docs/history/wip-2026-05-27--2026-08-23.md`, nothing was deleted, and the full
text is also in `git log -p docs/WIP.md`.

47 entries were classified before pruning. Five had anything still live in them;
the other 42 were superseded or finished. The facts that were still true and
recorded in **no other file** were pulled forward into "Carried forward" at the
bottom of this file, each re-verified against the tree on 2026-08-23 with a
file:line. If you are looking for something that is not here, read the archive
before concluding it was never known.

Routing to the durable docs is in the header of `docs/deferred.md`.

## Right now (2026-09-02)

Fresh clone on the Mac (only the Command Line Tools installed, no Xcode).
Infrastructure rebuilt from nothing: `scripts/build-macos-slices.sh` makes the
two `macos-arm64_x86_64` slices the CMake trees need (the full xcframework
scripts cannot run without iOS SDKs); `build-rel` (arm64) and `build-x86`
(x86_64) configured and built with the JIT on; x86_64 cairo/libgit2 bottles
staged beside `build-x86/test_load_image`; `libTestLibrary.dylib` built for
both arches; a fresh Pharo 13.1 image (`4f7563d`) prepped with the runner via
explicit snapshot (54 -> 79 MB, `SUnitRunner` present).

The `scripts/pharo-headless-test` submodule pointer (`749ce14`) was never
pushed; a fresh clone cannot fetch it.  Re-created as `ed885c8` from the
write-up below (same change: the selector skip list is empty).  Still local
until the submodule is pushed.

**First defect found by the C++ tier: a heap-use-after-free in the shared
lifter** (`SistaBuilder.cpp` Pass 5) -- `test_sista_ir` rc=139 on x86_64,
passing by luck on arm64.  Fixed and ASan-verified on both arches; see
`docs/changes.md` 2026-09-02.

**The stock Cog baseline is back** (2026-09-02).  The `codeZone` abort that
made this host "no Δcog possible" is arch-specific: the **x86_64** Cog runs
fine under Rosetta.  Installed at `/tmp/harness-x86`, v10.3.9, `eval` and
`eval --save` both verified; every command needs the `arch -x86_64` prefix.
Three things fell out of it the same evening, none of which touched our VM or
the running sweep:

  * **The trait-test storm is ours** — Cog runs all 27 `Trait*Test` classes in
    seconds, 270 tests, 0 F / 0 E, where our batch exhausted 12 GB.  Filed as
    defect #23.
  * **Defect #22 (`RSLinesTest` `BlockCannotReturn`) is root-caused** — Cog
    passes 18/18, and the image says `RSAbstractLine`'s copy of the trait
    method carries a block whose `outerCode` is still
    `RSTMarkeable>>#markersIncludesPoint:`.  677 installed methods carry that
    shape with a non-local return in the block.
  * **Fourteen of the sixteen "no display" classes are root-caused, and the
    defect is the render loop** (#24, root #25).  That is 9 of the sweep's 21
    FAILs and 17 of its 21 ERRORs; three more FAILs are parity with Cog.  That
    leaves three genuinely ours, and the x86 sweep narrows even those:
    `StDebuggerActionModelTest` is the ONLY class in 1800 covered classes that
    is non-clean on arm and clean on x86, so its one failure is timing —
    leaving `StSpotterModelTest`'s two — and reading those closes them out too:
    one carries `skipOnPharoCITestingEnvironment` (Pharo's CI does not run it,
    ours does), and the other asserts a forked search has not started for
    5 x 50 ms and has started 300 ms later, i.e. a 250-550 ms scheduling
    window.  **Every non-pass in the arm64 residual is now attributed, and none
    of the GUI ones is a VM computation error.**  On stock Cog, installing `MorphicUIManager` and suspending
    its UI process reproduces our result for `SpListCommonPropertiestTest`
    exactly — 18 P / 5 E, same five selectors, same messages; leave the process
    running and Cog is 23 / 23.  Our runner suspends it as its first startup
    action because on our VM the headless resume restarts `MorphicRenderLoop`,
    which busy-spins with Morphic DNUs until the Delay scheduler dies.  Fix the
    DNUs and nine classes of residual go with them.  Two earlier explanations
    (the missing display; a `ParametrizedTestCase` parameterisation bug) were
    filed and refuted the same evening and are recorded under #24.
  * **A separate, real coverage gap: 10,567 test cases** — 241 of the 2047
    concrete test classes are parameterised, their selectors total 2074 and
    their suites 12641, so the reported test count understates the suite by
    ~38%.  This is a denominator fact, not a failure cause.

  * **The threaded-FFI batch never exits** (#26).  Batch 1801-1850 writes all
    its results and `=== BATCH COMPLETE ===`, then hangs until the harness
    kills it at 1800 s — on both arches.  Stock Cog runs the whole block
    (TF/TFUFFI + Step* + TCPSocket*, 429 tests) in **7.4 s**, 0 F / 0 E.  Only
    two statements run after `BATCH COMPLETE`, `[Smalltalk exitSuccess] on:
    Error do:` and `Smalltalk quitPrimitive`; stderr landmarks were added
    around both so the next sweep says which one hangs.  This is 30 minutes of
    every sweep on every arch.

Full Δcog for the residual in
`docs/results/sweep-arm-2026-09-02/cog-residual-baseline.txt`.

Also cleaned up: the runner sent `#suspend` to **nil once per test**
(`backupWatchdog`, assigned nil right after the watchdog fork and never set
again, with its own temp declared "unused").  Each one costs a 17-line `[DNU]`
frame dump plus `[DNU-RCVR]` and `[SISTA-RING]` traces — together the single
largest source of stderr in a sweep, and the same trace a past session misread
as forwarded-object MNUs.  Guarded with `ifNotNil:`, along with the three other
suspend/terminate sites.

**The x86_64 backend is not producing wrong answers.**  Comparing the two
sweeps class by class while the x86 one was still running (both at **99.17%**),
every class non-clean on x86_64 and clean on arm64 was a timeout or a
wall-clock assertion, not a codegen difference:

    IntegerTest                     1 TIMEOUT (80 s)
    STONWritePrettyPrinterReadTest  1 TIMEOUT
    SpTreePresenterExpandTest       1 TIMEOUT
    RSSequentialAnimationTest       Error: Time up
    SpJobListPresenterTest          Got 68 instead of 100

Rosetta runs that build at roughly half arm64's speed against a fixed 80 s
per-test bound — the same 2x rule `scripts/package-tests-selfhosted.sh` already
documents for the package tier.  The runner now reads `SUNIT_TIMEOUT_MULT`
(submodule `db42c37`), so the next x86 sweep should use `SUNIT_TIMEOUT_MULT=2`.

**Verified overnight**, from the chained post-sweep scripts:

  * `test_relaunch` **3/3 PASS on `base.image`** — the earlier 2/3 was the
    prepped image, as diagnosed; not a regression and not machine load.
  * The **low-space breaker fires**: 462 `[LOW-SPACE]` lines and **zero** FATAL
    aborts on the storm repro at `PHARO_MAX_OLD_SPACE_MB=512`, with the P60
    watcher preempting the P40 hog.  In a bare `eval`, which the 2026-07
    dossier said was impossible.
  * The **x86_64 sweep finished at 99.15%** on 2046 classes — see above.
  * The **storm does not reproduce in chunks**: all eight covering 1912-1950
    ran clean in 135 s total.  It needs one image; the bisect must be by class
    count, not by fresh chunks.
  * Batch 1001-1050, which I damaged mid-sweep, recovered clean in ten chunks.

### Defects filed tonight

    #22  BlockCannotReturn on a trait-copied block's non-local return
         FIXED + VERIFIED — RSLinesTest 18/18 with the JIT and without, from
         16 P / 2 E (arm) and 17 P / 1 E (x86).  The class's copy of a trait
         method shares the trait's CompiledBlock, whose outerCode names only
         the trait's method, so the home match by method oop could not succeed.
    #23  The Context storm — ours, and NOT the trait tests.  Reproduces in
         **18 s** at PHARO_MAX_OLD_SPACE_MB=1024, dying at index 1909
         TonelWriterV1Test; 1912 was only where a 12 GB heap ran out.  The 27
         trait classes are clean in isolation on our VM (0 traitUsers
         violations, TraitTest 54/54, no storm).  Bisect forward from 1901
         through the Tonel block, 18 s a run.
    #24  Fourteen classes fail because the runner must suspend the UI process.
         REPRODUCED on Cog; two earlier explanations refuted and recorded.
         Our PRISTINE headless environment is identical to Cog's, so the
         difference is the prepped image's live Morphic world.
    #25  The resumed MorphicRenderLoop busy-spins on Morphic DNUs and kills
         the Delay scheduler — the root behind #24
    #26  The threaded-FFI batch never exits.  FIXED + VERIFIED —
         1800 s -> 54 s, rc=0, 51 of 51 classes.  ~29 minutes per sweep per
         arch.
    LEAD 19  The low-space breaker could not fire.  FIXED + VERIFIED —
         462 firings, zero FATAL aborts in a bare eval.  But the new FATAL
         diagnostic shows the SWEEP never arms it at all
         ("threshold=0 bytes ... the image never installed a
         LowSpaceWatcher"), so that run had two causes and the second is in
         the image's startup.  Open.

The C++ tier is green on both arches with all of tonight's commits:
`test_sista_ir`, `test_class_table`, `test_sista_survey` and `test_relaunch`
3/3 on arm64; `test_sista_ir` and `test_class_table` on x86_64.

**The arm64 SUnit sweep is done** (`STEP=50 PER_BATCH_TIMEOUT=1800`,
`PHARO_CODE_ZONE_MB=192 PHARO_MAX_STEPS=4000000000000
PHARO_MAX_OLD_SPACE_MB=12288`): 2007 classes, 27692 tests, 27461 P / 21 F /
21 E / 7 T / 182 S, 99.17%.  Full write-up and the per-class residual in
`docs/results/sweep-arm-2026-09-02.md`, artifacts in
`docs/results/sweep-arm-2026-09-02/`.  Two things came out of it that are not
pass-rate numbers:

  * **A write splice in the runner dropped two classes from every
    aggregation** -- the P60 watchdog published `testDone` before printing its
    verdict, so the P80 main wrote the class `Total:` line into the middle of
    it and then suspended the watchdog mid-line.  Fixed in the submodule
    (compose off-stream, publish `testDone` after the flush).  NOT yet
    verified by a run, because a targeted run would collide with the x86_64
    sweep's `/tmp` state.
  * **Batch 1901-1950 died of a Context storm** (`rc=134`, 33M Contexts / 6.6M
    Errors / 12.3 GB) and took 39 classes with it.  Same signature as the
    2026-08-22 batch 601-650 crash and the open "Monticello runaway", but this
    time the missing range is identified without re-running anything: it is
    the **trait tests**.  The batch died at index 1912, batch 1951 starts at
    `UndefinedPackageTest`, and the classes between those two names in this
    image are 27 `Trait*Test` plus `TrueTest`, `Tutorial*`, `UDPSocket*`,
    `UUID*` and `Undefined*` — 41 candidates against the 39-40 that never ran.
    `TraitTest` ("Cog 54 P / 0 E, ours errors, not yet drilled", carried since
    2026-06-01) is one of them, and today's `OCClassBuilderTest` newcomer also
    fails on a trait composition.

The **x86_64 sweep is in flight** with the same settings, output under the
session scratchpad `sweep-x86/`.  **Its batch 1001-1050 is damaged and must be
re-run**: `test_relaunch` was run on the PREPPED image while the sweep was
going, and a prepped image auto-starts `SUnitRunner` on resume, so it wrote
`/tmp/sunit_test_results.txt` underneath the batch, which then reported
`classes=1` instead of 51.  The recovery is automated in the post-sweep script.
The rule this broke is wider than the one written down: it is not "do not run
`test_load_image`", it is "do not start ANY VM binary on the prepped image".

### Queued behind the x86_64 sweep — do these in order when it finishes

Everything here is blocked for one of two reasons: every SUnit runner
invocation reads and writes the same fixed `/tmp/sunit_*` paths (a targeted run
hijacks the sweep's next batch; `test_load_image ... eval` touches
`/tmp/sunit_run_completed.txt`, which makes `SUnitRunner>>startUp:` skip a batch
entirely; and ANY binary started on the PREPPED image does the same, which is
how batch 1001-1050 was lost tonight), or the sweep is running the binary that
would be replaced.

Steps 0-4 are automated in the session scratchpad, chained on each other
(`post-sweep-running.sh` -> `-2-` -> `-3-` -> `-4-` -> `-5-`, each waiting on
the previous one's "done" line):
`post-sweep-running.sh` (waits on the sweep's pid; recovers the damaged x86
batch, `test_relaunch` on `base.image`, the low-space control, the storm hunt
in chunks of 5, rebuild x86), then `post-sweep-2-running.sh` (rebuild both with
tonight's commits, C++ tier on both arches, the #24/#25 environment probe,
`RSLinesTest` with and without the JIT, the defect-#22 block probe), then
`post-sweep-3-running.sh` (re-run batch 1801-1850 and check it no longer idles
1730 s).  The rest are by hand.

 0. **Neither build matches the tree.**  Everything from `77f12ac6` on is
    syntax-checked only — the `!inExtension_` guard, the bounded trace, the
    headroom cap, the FATAL diagnostic, and the three dead-code removals.
    Rebuild BOTH arches and re-run the C++ tier before trusting any number
    from either.  `build-x86` is further behind: it has none of the day's
    changes, starting at `2c2c4616`.
    Two edits to `scripts/sunit-sweep.sh`, both deliberately deferred because
    editing a running bash script shifts the byte offsets it resumes reading
    from:
      * the "no VM binary on the prepped image" note in its header;
      * **kill a batch once it has finished.**  The VM writes
        `/tmp/sunit_run_completed.txt` when the batch is done; the driver
        currently waits for the process to exit and pays
        `PER_BATCH_TIMEOUT` in full when it does not.  Polling for that marker
        and killing caps the damage from ANY shutdown hang, not just defect
        #26's — and would have saved 30 minutes on each of tonight's two
        sweeps even before that fix existed.
 1. **Name the class that storms.**  Re-run arm batch 1901-1950:
    `printf '1901 1950' > /tmp/sunit_batch.txt` and launch the prepped image.
    The x86 sweep will also pass through that range and may name it for free.
    Index 1911 is `TonelWriterV3Test`; 1951 is `UndefinedPackageTest`; the
    classes in between are the trait tests (see the sweep report).  Start with
    the `Trait*` block, not with a blind batch re-run.

    **Run `scripts/pharo-headless-test/storm_repro_husk_freeze.st` first** — it
    is a one-command answer to the most likely hypothesis.
    `docs/history/arm-context-storm-2026-07.md` identifies the storm's trigger
    as a **trait-class rebuild with live instances leaving a stale husk** that a
    `freeze` handler re-hits, eliminated by `62417f43`
    (becomeForward-leaves-forwarder) and `296bba26`
    (classOf-follows-forwarders).  Both are still default-on.  The repro must
    answer `NO-HUSK`; anything else says the trigger fix has regressed and
    explains the trait batch dying.  If it does answer `NO-HUSK`, there is a
    second trigger and the per-class hunt is the way in.

    **Hunt with a small heap.**  The storm took 758 s to reach the FATAL at
    `PHARO_MAX_OLD_SPACE_MB=12288`; at 1024 it should abort in roughly a
    twelfth of that, which turns a bisection over 39 classes from an evening
    into minutes.  The census shape is the same either way — it is the ratios
    (1.00 closures and 5.01 Contexts per Error) that identify it, not the
    absolute counts.

    Two halves of that fix were re-checked by reading, 2026-09-02, so do not
    re-derive them: `collectInstancesOfClass` still skips forwarded objects
    (`ObjectMemory.cpp:3386`, `!obj->isForwarded()`), and both
    `becomeForward`-leaves-forwarder and classOf-follows-forwarders are still
    default-on with opt-out knobs only (`ObjectMemory.cpp:1803`, `:1895`,
    `:558`).  So "the 2026-07 trigger fix silently reverted" is already
    partly refuted; a second trigger is the likelier reading.
 2. ~~**Verify the low-space breaker actually fires**~~ — DONE 2026-09-03:
    462 `[LOW-SPACE]` lines and zero FATAL aborts on
    `storm_repro_freeze_recursion.st` at `PHARO_MAX_OLD_SPACE_MB=512`, with the
    P60 watcher preempting the P40 hog.  And it worked in a bare `eval`, which
    the 2026-07 dossier said was impossible.  Original note kept below.

 2b. (superseded) **Verify the low-space breaker actually fires — NOT with a bare `eval`.**
    The "before" evidence is on record (12 GB consumed, zero `[LOW-SPACE]`
    lines).  The obvious "after" test is
    `PHARO_MAX_OLD_SPACE_MB=512 ./build-rel/test_load_image <image> eval
    "<storm_repro_freeze_recursion.st>"`, and it is **expected to print
    nothing**: `docs/history/arm-context-storm-2026-07.md` records that the
    mitigation is disarmed in bare eval mode, because the image never runs
    `installLowSpaceWatcher` there, so prim 125 is never armed and neither the
    old sampled check nor the new latch can arm either.  The post-sweep script
    runs it anyway as a control; a silent result there is NOT evidence against
    the fix.

    The real test is the runner path, which does install the watcher (the
    2026-09-02 batch log shows the VM inside `#lowSpaceWatcher` at step 161M).
    So verification and investigation are the same run: re-run the trait block
    on the rebuilt binary.  If the storm recurs, `[LOW-SPACE]` lines must
    appear — plural, because the image re-arms after each one and its action
    (`signalException: OutOfMemory`) is an `Error` the hog's handler can
    swallow.
 3. **Verify the runner watchdog fix.**  Re-prep an image with the current submodule
    (`ad78260`) — both changes compile (filed into a pristine image under stock
    Cog) but neither has been run.

      a. The watchdog splice: run a class with a known timeout
         (`MCSmalltalkhubRepositoryTest`); its `Total:` line must be at line
         start.
      b. (The parameterised-instance change that was here is reverted — its
         premise was refuted, see defect #24.  Nothing to verify.)
 4. **Defect #22 is root-caused; what is left is the fix.**  Cog runs
    `RSLinesTest` 18/18, and the image says
    `RSAbstractLine>>markersIncludesPoint:`'s inner block has
    `outerCode = RSTMarkeable>>#markersIncludesPoint:` — the trait's method,
    not the class's copy — so our method-oop home match cannot succeed.  677
    installed methods carry that shape with a non-local return in the block.
    Run `PHARO_NO_JIT=1` on the test first: it separates "the dynamic
    `outerContext` fallback is never reached" from "it is reached with a stale
    `closure_`".  (Superseded detail below.)  It was clean in
    the 2026-08-22 sweep and dirty in this one, so treat it as timing-sensitive
    and reach for `PHARO_DET_SCHED=1` first (CLAUDE.md).  Both errors are
    `BlockCannotReturn` on a 4-frame non-local return
    (`markersIncludesPoint:` -> `markerShapesInPositionDo:` ->
    `withEnd:controlPoints:do:` -> `setPositionTo:vector:do:` -> `aBlock value:`,
    all synchronous), which is VM-visible, not display.  **There is a
    mechanism-level hypothesis in defect #22 already** — both selectors are
    defined twice, in `RSAbstractLine` and in the trait `RSTMarkeable`, and the
    inline NLR matches the home by method oop against the block's static
    `outerCode`.  Bisect `PHARO_NO_JIT=1` first: passing there implicates the
    stale `closure_` on JIT-resident block returns that `Interpreter.cpp:8432`
    already warns about.
 5. **Re-run `test_relaunch` on `base.image`.**  It failed 2 of 3 cycles
    tonight (`[VM-STOP-TIMEOUT] interpret() has not returned 2s after stop()`,
    the worker still executing -- `[JIT] Stats:` sends kept climbing, so not a
    wedge) and that was **my doing, not a regression**: I pointed it at the
    PREPPED image.  A prepped image auto-starts `SUnitRunner` on resume, whose
    P80 processes hold the worker well past a 2 s deadline.  The 3/3 run
    earlier the same day used `base.image`.  Re-run there for the record; the
    C++ tier is not being claimed green for `test_relaunch` until it is.
 6. x86_64 sweep write-up plus the arm-vs-x86 residual diff.  Re-run it with
    `SUNIT_TIMEOUT_MULT=2` if the timeout-class failures are to be excluded.  **Recover its
    damaged batches first**: 1001-1050 (my own interference; automated in the
    post-sweep script) and, if it storms there too, 1901-1950.  The recovery
    pass added to `sunit-sweep.sh` will NOT have run for this sweep — the
    script was edited while that instance of it was executing, so bash may
    resume after the batch loop at a stale byte offset.  Re-run those ranges
    by hand, in chunks of 5, the way the post-sweep script does for arm.  A ready
    comparison script is in the session scratchpad (`compare-sweeps.sh`);
    the arm side is staged at `sweep-arm/all_results.txt`.
 6b. **Consume `needsScavenge_` in `step()`** — considered and deliberately NOT
    done tonight.  `step()` already handles `needsCompactGC()` at its declared
    "GC safe point: between bytecodes, no C++ locals hold Oops", so the shape
    is established:

        if (memory_.needsScavenge()) {
            memory_.clearScavengeFlag();
            prepareForGC(); memory_.scavenge(); afterGC();
        }

    (the `interpret()` checkpoint's version, minus its bisect knobs).  That
    would end the whole-batch no-scavenge mode described in `docs/deferred.md`.
    Left undone because the benefit is small — the batch it affects is clean
    apart from one unrelated failure, and defect #26's fix makes it exit
    quickly anyway — while a wrong GC call corrupts the heap in every later
    run, and tonight there is no way to verify it properly.  Do it when a GC
    A/B can be run.
 7. Package tier on both arches.  The arm64 run is chained as
    `post-sweep-5-running.sh` (baseline to beat: 9382 P / 16 F / 18 E from
    2026-08-22); x86_64 follows, and should use `REUSE_FROM=<arm work dir>`
    since the loads cannot run there.
 7b. **Chase defect #25's DNUs** — the highest-value single item to come out of
    tonight.  Our headless resume restarts `MorphicRenderLoop` at pri-80, it
    busy-spins `WorldState>>drawWorld:` on Morphic DNUs, and the Delay
    scheduler dies; the runner's first startup action is to suspend it, and
    that suspension is what makes nine classes fail (#24).  The runner
    suspends the loop before the DNUs can be observed, so resume WITHOUT the
    suspension and read what `drawWorld:` sends that the image does not
    understand.  A cheaper harness-side lever is worth trying first: our
    prepped image is snapshotted with a live Morphic world, where a stock
    headless boot has none — prep with `NonInteractiveUIManager` and no world
    before `snapshot:andQuit:` and there is nothing for the runner to suspend.
    (Merely switching the manager back AFTER suspending does not work; tested
    on Cog, still 18 P / 5 E.)
 8. **Run the full Δcog sweep.**  `scripts/cog-sweep.sh` (new) drives stock Cog
    in batches with `SUNIT_PREFIX` namespacing, so its `/tmp` state cannot
    collide with ours:

        scripts/cog-sweep.sh docs/results/sweep-arm-2026-09-02/class-index-map.txt \
            <scratch>/cog-sweep

    That produces the first full-suite Δcog baseline this host has ever had.
    Do it on an idle machine — namespacing solves the `/tmp` collision, not the
    wall-clock sensitivity.  Then `scripts/classify-sunit.py cog.txt ours.txt`.
 8. **Use the Cog baseline that is now available.**  DONE 2026-09-02: the
    stock VM's `codeZone` abort was arch-specific, not host-specific, and the
    **x86_64** Cog under Rosetta runs here.  Installed at `/tmp/harness-x86`,
    Cog v10.3.9, `eval` and `eval --save` both verified.  Every stock-VM
    command needs the `arch -x86_64` prefix.  See CLAUDE.md.

    What this unblocks, in rough order of value:

      * **`TraitTest`** — "Cog 54 P / 0 F / 0 E, ours errors, not yet drilled",
        carried since 2026-06-01, and now one of the classes in the batch the
        storm killed.  A Cog run of the whole trait block says at once whether
        the storm is ours.
      * **Defect #19, glyph drawing 12-25x** — needs a Cog timing number.  Use
        `build-x86` for the comparison, not `build-rel`: both it and the Cog
        are x86_64 under Rosetta, so the ratio means something.
      * **The XMLParser ~8.8 s question** in the open list, which is written up
        as "needs a stock-Cog number this host cannot produce".
      * **Defect #2's six remaining packages**, which have no current Cog
        numbers at all.
      * The whole `scripts/classify-sunit.py cog.txt ours.txt` Δcog path in
        `docs/vm-compat-bugs.md`, and `run_sunit_cog.st`.

## Where the three tiers stand

    tier       arm64                            x86_64
    VM C++     test_relaunch 3/3 on base.image;  4/4 as of 10f5f330;
               rest as of 10f5f330, the day's    the day's commits unbuilt
               later commits unbuilt
    packages   9382 P / 16 F / 18 E             9382 P / 16 F / 18 E   (2026-08-23, identical)
    SUnit      27453 P / 21 F / 21 E / 5 T      27833 P / 22 F / 26 E / 10 T
               2005 classes, 99.17%             2046 classes, 99.15%
               (39 classes lost to the storm)   (ran the block arm lost)

Both sweeps are 2026-09-02/03 and use the same prepped image.  The x86_64 run
is the more complete of the two: batch 1901-1950 storms on arm64 and runs clean
on x86_64 in 270 s.  Every arch-only difference is a Rosetta timeout, a
wall-clock assertion, a missing Windows-only symbol, a CWD artifact, or a class
the other arch never reached — **no codegen divergence in 28071 tests**.  The
denominator on both is ~38% short of the real suite (see #24).

The 2026-08-23 SUnit line this replaces read `800 P / 13 F / 3 E / 1 T` on
arm and `798 P / 15 F / 4 E / 1 T` on x86 -- that was the 30-class residual
re-measured against a fake-GUI image, not a full sweep, and the two are not
comparable.

Every non-pass is attributed by name in
`docs/results/sweep-arm-2026-09-02.md` (and, for the 2026-08-22 runs,
`docs/results/sweep-{arm,x86}-final-2026-08-22.md`). One newcomer is
untriaged: `RSLinesTest` errors twice with `BlockCannotReturn`, which is a
VM-visible condition rather than a display one. Everything else is display,
network, image drift, or the two known GC/finalization items.

VM C++ tier re-verified green today on both arches after the JIT changes:
`test_class_table`, `test_sista_ir`, `test_sista_survey`, `test_relaunch` (3/3
cycles) on arm64; `test_class_table`, `test_sista_ir` on x86_64.

## Landed today

**Two JIT recompile defects, both found by reading the eviction/recompile/IC
paths rather than by reproduction, and both only reachable since eviction was
wired up on 2026-08-22.**

1. *A superseded method could reclaim its MethodMap entry after GC*
   (`21047517`). `recompile()` deliberately does not free the old JITMethod —
   its code can be live on the stack and other methods' ICs may hold a J2J
   entry into it — but it left the old version `state == Compiled` under the
   same `compiledMethodOop`. `rebuildMethodMap()` runs after every GC and
   re-inserts every Compiled method under that key; `MethodMap::insert` updates
   in place and the zone list is ordered by **address**, so the winner was
   whichever version sat higher in memory. Bump allocation always puts the
   replacement higher, which is why it never showed; once eviction recycles the
   free list a replacement can land *below* what it replaced and the stale
   tier-1 version reclaims the entry, permanently undoing the recompile.
   Fixed with `kSuperseded` (bit 2 of `JITMethodStats::flags` — a heap
   side-table, because `JITMethod` is size-locked to `TrampolineAsm.S JM_SIZE`
   and cannot grow), skipped in `rebuildMethodMap`. Measured, varying only zone
   size:

        zone 64MB   599 superseded    0 outrank their replacement (no eviction)
        zone 12MB    36 superseded    8 outrank
        zone  6MB    13 superseded    1 outrank
        x86  12MB   267 superseded   19 outrank
        real suite  899 superseded  207 outrank      <- 23%

2. *Recompiling never redirected existing callers* (`7c764f09`). A monomorphic
   J2J site branches straight to the entry address in `icData[2]` and never
   consults the MethodMap, so callers kept running the superseded body forever
   — including the site whose count triggered the recompile, which cannot
   re-queue it either because that counter fires on `== 500` exactly. Probed:
   597 recompiles stranded **1242** IC entries across 369 methods; in the live
   suite the scrub fires ~1360 times for ~2820 entries. `recompile()` now
   scrubs them, mirroring the eviction scrub including the PMS slot-0
   re-derive. It **clears** rather than retargets, because the extra word
   carries specialization bits derived from the old JITMethod and per-version
   properties (`canSkipJ2JSave`) need not match. Opt out:
   `PHARO_NO_RECOMPILE_IC_RETARGET`.

   This is a consistency fix, **not a speedup** — A/B flat (inject:into: loop
   4.38 vs 4.38 s; `28 benchFib` 3.37 vs 3.39 s), and it does not increase
   dropped IC patches (`PHARO_IC_PATCH_DEBUG=1` reports zero "STALE pending
   slot dropped"). Its cost is a **full zone scan per recompile**, now
   instrumented under `PHARO_TRACE_RECOMPILE_FLOW` (`67fe25dc`) rather than
   assumed — the number is still to be read off a suite run.

**The SUnit harness was silently broken** (`12ad7265`). The documented prep ran
through the stock Cog VM, which cannot start on this machine at all — it wants
its code zone at a fixed address Darwin 27 will not grant, aborts, and
segfaults (rc=255) on *any* image including a pristine one. So the prep did
nothing, the image saved without `SUnitRunner`, and every suite launch wrote no
results file — indistinguishable from a scheduler wedge, which is how it
presented. Compounding it, `eval --save` is a stock-VM flag ours does not
implement, so even under our VM the fileIn ran and was never persisted. Prep now
uses our VM onto a copy, ending in an explicit snapshot, verified by size
(~54 MB -> ~73 MB). Also means **no live Δcog baseline is obtainable here.**

**The runner's last silent skips are gone** (submodule `749ce14`). The selector
skip list is now empty. Its final two entries were the inherited `ClassTestCase`
AST-lint rules, held back on a cost argument the comment itself flagged for
re-measurement: only **50** classes inherit `ClassTestCase`, not the ~640
assumed, and both selectors across all 50 is **100 tests, 100 PASS, 6473 ms**.
They were omitted *silently* — a skipped selector was never counted in S — so
they appeared in no report.

**Seven never-investigated SUnit residual failures diagnosed** (`2b90dc6e`) —
two pass in isolation; two "Got 4 instead of 2" are Spec *layout* tests counting
layout children, driven by the class-side `maximizeAssertionSpec:` global set
*after* the layout is captured; one inspects its own enclosing call stack and so
reports on whatever wraps it; two are a Spotter timing assertion and a
UI-dependency list. None is a VM computation error.

## Open

  * **`cull:` DNU — 0 reproductions in 24 runs.** `b887d81f` matches the symptom
    and measurably moved `canSkipJ2JSave` (22.4% -> 1.3%), but with no repro
    that is a hypothesis, not a result. Do not write it up as fixed.
  * **The Monticello runaway that ends the sweep.** `MCDictionaryRepositoryTest`
    reproduces it alone (rc=134). The tests are innocent: all five pass when
    driven directly, and `MCRepository fromUrl: 'file:///tmp'` returns the right
    object. Under the runner, P40 sits in `MCRepositoryTest class>>pragmasDo:`
    while P79 spins in `Error>>signal` / `signalerContext` /
    `findContextSuchThat:` / `freeze`, minting errors until the heap is gone.
    `on:do:` catches correctly in both the main process and a `forkAt: 40`, so
    handler visibility is refuted; the fork+watchdog machinery is the remaining
    suspect.
  * **`WeakAnnouncerTest>>testNoDeadWeakSubscriptions`.** Our GC nils the weak
    slots correctly (they print `nil subscribes to MethodAdded`); what does not
    happen is the dead subscriptions being swept from the registry, i.e.
    finalization timing. `PHARO_WEAK_SURVIVOR_PATHS` prints why a referent
    lived. The old caveat here — "this test is one of the 17 Pharo's own CI
    skips, so it is not evidence Cog passes anything" — **is now settled by
    measurement (2026-09-02): stock Cog runs the class 33 P / 0 F / 0 E with
    the same one skip, so Cog does pass it and this is ours.**
  * **Old-space fragmentation (12x).** `makeFreeChunk` was necessary, not
    sufficient. Four 32-byte pins strand 146 MB; they are pinned while already
    old, so the fix needs either a safe alternative to `becomeForward` at pin
    time or GC-safepoint relocation when `callbackDepth_ == 0`. Feeding
    relocation from the arena at pin time crashes (rc=133) — `becomeForward`
    rewrites references mid-primitive. The tenure path is safe because scavenge
    already owns that contract.
  * **`PMArbitraryPrecisionFloatTest>>testPrintAndEvaluate`** passes standalone
    and with its whole class, fails only in the 117-class run, and not on the
    bound (300 s and 900 s both unchanged); 19 preceding classes do not
    reproduce it.
  * Whether ~8.8 s is reasonable for those XMLParser tests at all — needs a
    stock-Cog number, and as of 2026-09-02 this host CAN produce one (x86_64
    Cog under Rosetta, see CLAUDE.md); it needs XMLParser loaded into a Cog
    image first.

`PHARO_OLDSPACE_FREELIST` is **no longer** on this list. It works as of
2026-08-23 after a sixth bug: `collectInstancesOfClass` matched `classIndex 0`,
so `allInstances` of a class with no class-table entry answered an Array of free
chunks — which is what killed class-shape migration. Read the knob's own comment
in `src/vm/debug_vars.h:42`; ten hypotheses are closed by test in there.

## Carried forward from the archive

Still true, and recorded in no other file. Each was re-verified against the tree
on 2026-08-23; line numbers are from that day. Grouped, terse, and deliberately
not expanded — the full narrative is in
`docs/history/wip-2026-05-27--2026-08-23.md`.

### JIT / code zone

 1. **evictLRU pass 2 is address order, not recency.** Pass 1 needs ~650K
    method entries untouched (64K per epoch, `epoch_-10` threshold,
    `CodeZone.hpp:313`), so pass 2 usually runs and walks `firstMethod_`
    (`:331-344`), evicting the earliest-compiled kernel methods that stay hot.
    `763f7f7b` fixed the epoch never advancing; the fallback is unfixed.
    Planned: progressively younger thresholds (`epoch_-5, -2, -1`, then all).
 2. **The evictLRU second-pass change was measured and DROPPED** — 1 MB zone
    8 s/277 methods vs 7 s/286; 192 MB zone (XMLParser load) 344 s/5769 vs
    340 s/5542. Re-open only with a workload that fills the zone *and* advances
    the epoch.
 3. **248,066 methods compiled during one XMLParser load** against ~176k
    CompiledMethods+CompiledBlocks in the whole image (recompile churn).
    `evictTarget` is 1/64th of the zone (`JITCompiler.cpp:1780-1782`) and may
    want raising now that a round is cheaper.
 4. **Bit-60 J2J precedence makes five inline-return specializations dead on
    arm64** — the bit-60 `tbnz` is emitted before the extras dispatch
    (`AsmjitT1.cpp:6488`, dispatch at ~6498/6506/6512/6525/6540). Derived
    upside ~3.9% on a 356 ms workload. **The fix is the reorder**;
    `PHARO_NO_J2J=1` alone makes things slower.
 5. **arm64 emits 2.7-2.9x more bytes per compiled method than x86_64** — 7.89
    vs 2.76 KB/method; 8.91 vs 3.31 across full sweeps. This is why arm64 fills
    the code zone first.
 6. **35% of arm64's code footprint is inline-J2J** — 45,176 KB drops to
    29,260 KB with `PHARO_T1_NO_INLINE_J2J=1` (`debug_vars.h:92`).
 7. **IC hit rates are not comparable across arches**: arm walks IC slots 0-2
    inline (`AsmjitT1.cpp:6331-6363`), x86 probes slot 0 and exits
    (`:3185-3189`), so the two builds count different event populations.
 8. **`[JIT] Stats:` lines are periodic checkpoints, not totals** — one every
    65,536 method entries (`JITRuntime.cpp:~3896`). Two mid-run samples were
    once published twice as evidence of a structural arm-vs-x86 gap; real
    end-of-run totals are 0.56% apart.
 9. **`PHARO_T1_INLINE_PRIM_COUNTERS=1` gotcha** — without it `g_primAt_hits`
    and friends stay 0 even when the inline path is firing, giving a misleading
    "primAt=0".
10. **The asmjit T1 inline-SETTER path never fires in micro-benchmarks**
    (getter=16059, setter=0; `detectTrivialMethod` runs ~168 times per
    tinyBench and never classifies a setter). The recognizer is not broken —
    fib/sieve/tinyBench contain no setter sends.
11. **DEAD END, do not re-walk**: adding the inline-asm write barrier to the six
    temp-store stencils (`storeTemp_1/_2`, `popStoreTemp_1/_2/_3/_4`,
    `stencils.cpp:~4496-4540`) left the miss count unchanged at 230 and
    regressed tinyBench ~11% (5350 -> 5950 ms). JIT code runs `frameDepth_ > 0`,
    so that path is virtually never exercised. Reverted.

### GC / memory

12. **The remembered set is dead infrastructure.** `rememberedSet_` is populated
    (`ObjectMemory.cpp:3712`) but never iterated — only `.clear()`ed (`:2321`);
    scavenge still does an O(oldSpace) full scan ("Trade correctness for perf
    until every write site is audited", `:2221-2226`). Closing it would drop
    ~30 ms/scavenge on a 100 MB heap. The audit reached 260 -> 228/230 misses at
    99.997% bit accuracy; residual misses are C++ paths, not the JIT path.
13. **`PHARO_WATCH_ROOT_CLASS` (`debug_vars.h:201`) is an exact class-name
    compare** — it does not follow subclasses, so check `allSubclasses` before
    reading a zero-hit result as meaningful.
14. **Raw-heap-scan gotcha**: `PHARO_SCAV_RAWSCAN`
    (`ObjectMemory.cpp:2434-2513`) scans format-agnostically and hits
    `ExternalAddress` objects (fmt 16, ptrSlots=0) that legitimately store
    native addresses — all 54 hits in one run were such false positives.
15. **`[SCAV-DANGLE]` shares the scavenger's blind spot** — both walk the same
    `forEachRoot` set (`ObjectMemory.cpp:2188` and `:2421-2425`), so a holder
    outside that set makes the diagnostic report clean while the object dies.
    A clean dangle scan is not proof the roots are complete.
16. **`ObjectMemory::setGlobal`'s tail** (`ObjectMemory.cpp:~1063-1073`) walks
    the dictionary array for the first nil/zero slot and stores there, with no
    hash placement and no tally update. Unresolved, recorded nowhere else.
    (The `68` DNU that led here *is* fixed; see `ObjectMemory.cpp:1029-1035`.)

### Scheduler / VM control

17. **`Interpreter::running_` is a plain `bool`** (`Interpreter.hpp:1168`),
    written by `stop()` (`:285`) and read by the dispatch loop — a real data
    race, and the leading candidate for the `vm_stop` timeout. Left standing
    **deliberately**: `std::atomic` puts a real load in the hot dispatch loop,
    so it needs a bench-suite before/after, not a blind edit.
18. **`removeProcessFromList`'s return value is discarded**
    (`Interpreter.cpp:18685`, `Primitives.cpp:5017`) and the caller then
    unconditionally nils `ProcessNextLinkIndex`, truncating the run queue if the
    search failed. Wants a repro before touching the scheduler.
19. **`vm_stop` timed out on roughly one relaunch cycle in thirty** before the
    later fixes (10 runs: 9 pass, 1 fail, one stop-timeout, one destroy-leak).
    Post-fix runs show zero. Budget that rate for any repro attempt.
    Instrumentation is live at `PlatformBridge.cpp:618` and `:669`.

### Measurement traps

20. **We do NOT set `PHARO_CI_TESTING_ENVIRONMENT`**, so our sweeps run the 17
    tests Pharo's own CI skips via `skipOnPharoCITestingEnvironment` —
    including `AllocationTest` testOneGB/testOneGWordAllocation,
    `RandomTest>>testUnixRandomGeneratorSeed`, `StDebuggerTest`, `StSpotter*`,
    `StPlaygroundPageTest>>testContents`, `TKTNewProcessTaskRunnerTest`, and
    `WeakAnnouncerTest>>testNoDeadWeakSubscriptions`. Check the guard before
    calling any of them a VM defect.
21. **Classes that time out emit no `Total:` line** and drop out of the
    per-class tally (`run_sunit_tests.st:582-598`) — which is why arm counted
    2043 and x86 2037 against 2047 declared. A class-count difference between
    runs is not evidence of a crash.
22. **Package-tier noise floor is ±2**: four PolyMath runs on one day scored arm
    err = 16, 16, 17, 18. Any arm-vs-x86 package delta needs two runs per side.
    `sunit-sweep.sh:95-102` states the equivalent rule for SUnit;
    `package-tests-selfhosted.sh` does **not**.
23. **A killed run reads as a passing one.** A bisect reported
    `noyg rc=124 mnu68=0` and the zero was a TIMEOUT — 900 s is not enough for a
    no-young-generation run (`PHARO_NO_YG`, `DebugSettings.cpp:379`). Record rc
    and an explicit completion marker alongside every count.
24. **"It did not reproduce in N runs" is not "it does not happen"** unless N
    suits the rate — 9 consecutive clean runs were once cited as proof against a
    ~10% event, and a correct conclusion was retracted on that basis.
25. **Verify the binary contains the change before blaming the architecture.**
    `build-x86` had not been rebuilt after the class-registration fix, so x86
    reported the pre-fix signature and read as an arch-specific GC defect. Both
    trees must be rebuilt (`build-rel` arm64, `build-x86` x86_64);
    `strings <binary> | grep <marker>` settles it.
26. **`bash -n` passing does not mean a driver script works** — a Smalltalk
    `"comment"` inside a double-quoted eval string terminates the string.
    Smoke-test every generated eval.

### Host & tooling

27. **`scripts/sunit-sweep.sh` still defaults `PER_BATCH_TIMEOUT=600`**
    (`:111`), which is too tight — batch 1501-1550 legitimately takes 613 s and
    is not a hang. `scripts/rerun/fullsweep.sh:9` sets 900.
28. **`scripts/rerun/fullsweep.sh:4` and `scripts/rerun/pkgrun.sh:9` have a
    session scratchpad path baked in** — fix that line before rerunning either.
29. **After a reboot clears `/private/tmp`, run `git worktree prune`** — git
    otherwise still believes the branch is checked out in the deleted worktree
    and refuses `git checkout jit`. There is a live scratchpad worktree today.
30. **x86-box constraints**: gdb there cannot read member vars (no full DWARF),
    so instrument in C++ instead; `gdb -p` needs
    `sysctl kernel.yama.ptrace_scope=0`; `PHARO_T1_TRACE_HANDLER`
    (`debug_vars.h:288`) dumps the handler-search chain.
31. **Cog-x86 baseline** measured on the x86 box (18.221.159.216), recorded
    nowhere else: loop20M ~44-50 ms, fib30 ~9-10 ms, cfibx30 ~13 ms.
32. **arm64 cost baselines** (ad-hoc evals, no committed script): INTADD3M
    4 ns/iter; BLOCKVAL3M 72 ns per block `value:`; ALLOCARR2M 93 ns per
    `Array new: 4`; COMPILE300 1.54 ms per `Compiler evaluate: '1+1'`. Block
    activation and small-object allocation sit 18-23x above the fast integer
    path.
33. ~~**Whether this VM is slower than Cog on these workloads is unanswerable
    on this host**~~ — SUPERSEDED 2026-09-02. The abort is arch-specific: the
    **x86_64** Cog runs under Rosetta (`arch -x86_64`, see CLAUDE.md), so the
    comparison IS available, and it is fair against `build-x86` since both are
    x86_64 under Rosetta. Separately, Rosetta overhead vs x86_64 backend code
    quality has still never been split.
34. **`-Wnontrivial-memcall` has an array-form blind spot** — it fires on
    `memset(p, 0, sizeof(OpenPort))` but not on
    `memset(gPorts, 0, sizeof(gPorts))`. Any warning-driven sweep inherits the
    hole. (Re-reproduced on Apple clang 21.0.0.)
35. **`LinkInstallerTest>>testPropagateNewClassScopedLinksOnMethodNode` is
    GC-pressure-dependent, not the JIT** — 40/40 alone with JIT on or off; fails
    with any predecessor class; `PHARO_NEWSPACE_MB=1` makes it fail alone but
    only sometimes (39/40 and 40/40 on identical re-runs). Needs repetition
    counts before believing any knob bisect.
36. **Do not re-chase the `cos(2)` 1-ULP claim.** The image defines
    `Float>>cos` as `^ (self + Halfpi) sin`, and computing *that* formula with
    this host's libm reproduces the VM's bits exactly. No discrepancy exists.

### Corrected while pruning

37. **The WideString-WriteStream bug is FIXED** (`d5608fd4`, 2026-06-01), but
    `docs/results-jitpkg.md:519-521` still cites "the already-documented
    WideString-WriteStream bug (WIP.md)" as if open. Fix that citation rather
    than re-investigating.
38. **`indexOfClass` returning 0 for not-found** (`ObjectMemory.cpp:628,649`) is
    still true, but the "any new caller is unguarded" half is now stale — 5 of
    the 10 enumerated callers were guarded on 2026-08-23 (`b0ea7721`).
