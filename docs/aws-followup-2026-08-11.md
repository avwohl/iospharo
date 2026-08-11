# Closing out the x86 / ARM AWS runs — 2026-08-11

Follow-up to `docs/x86-test-run-2026-08-10.md` and `docs/arm-vs-x86-2026-08-11.md`.
Those two documents ended with an open list; this one records what happened to it.

Every item here was measured locally on this Mac (arm64 native, plus x86_64 via
the `build-x86` tree under Rosetta) against a freshly downloaded Pharo 13 image.

## Summary

    item                                          outcome
    x86 multi-entry dispatch divergence           FIXED   c5332248 (both arches)
    CWD-relative file resolution                  FIXED   d1cd608e
    weak-reference test re-triage                 RE-CLASSIFIED — see below
    arm package sweep                             NOT RUN (needs an AWS box)
    build-hunt history rewrite                    NOT DONE (needs a force-push)
    xcode-select                                  NOT DONE (needs a password)

## 1. Multi-entry dispatch — two bugs, one on each arch (`c5332248`)

The x86 doc listed this as "secondary x86 divergence, observed but unverified":
the multi-entry dispatch prologue exists only in `SistaLowering_arm64.cpp`, yet
`SistaRuntime.cpp` registers the compiled fn at every dispatchable bcOffset
regardless of arch. Verified, and there were two defects meeting in the middle.

**x86_64 took the wrong entry.** With no prologue, a per-bytecode trigger at an
interior loop top looked up the cache, got a fn compiled for a *different*
bcOffset, and jumped to it — which began executing at the region start with the
operand stack of another bytecode. Measured with the new unit test, prologue
removed, three runs: exit 139 / 1 / 139 — i.e. sometimes the wrong value,
sometimes SIGSEGV.

**arm64 lost the compile entirely.** `bb158a7f` (2026-06-04) added an
unreachable-block prune seeded only from block 0. The loader pseudo-blocks are
reached exclusively by the dispatch chain — an edge the IR successor graph does
not model — so the prune dropped every loader and the chain branched at labels
that were never bound. `runtime_->add()` rejected the broken encode, so this was
lost optimisation rather than miscompilation, but it meant arm64 had had *no*
working multi-entry dispatch for two months.

Restoring it is worth a lot on bytecode-bound code. Bench suite, same image,
five runs each, before = `c5332248^`:

    tinyBenchmarks bytecodes/sec
      before   272M  294M  262M  264M  267M
      after   1514M  496M 1344M 1372M 1486M      ~5x

    sends/sec unchanged (~130M both) — only the bytecode loop benefits.

Nothing else in the bench suite moved outside noise. Two apparent deltas in the
full-suite numbers (`1M blocks` +24%, `dict 50K` -15%) did NOT survive isolation:
run on their own, before/after are 220-240 vs 233-242 ms and 149-155 vs 147-153
ms. They are suite-order coupling — the whole suite runs in one VM and
tinyBenchmarks, which runs first, now behaves completely differently. Only one
method in the bench workload compiles with a dispatchable entry at all
(`#benchmark` at bcOff 23), and it is neither of those benchmarks.

**The drift itself is the thing that got fixed.** `Lowering::lower()` now reports
which region-local bcOffsets it actually emitted dispatch arms for, and
`SistaRuntime` keys the multi-key cache off that report rather than assuming
every `dispatchableBlocks` entry is routable. A backend with no prologue reports
nothing and gets no extra keys. `cc.finalize()`'s status, which is what fails on
an unbound label, was also being discarded in both backends; it is now checked.

Regression gate: `test_sista_ir` round-trip 10 builds a per-bc region with an
interior backward-jump target and calls the ONE compiled fn twice — entry@0 must
return the literal block 0 pushes, entry@4 must return the runtime stack slot its
loader materialises. Passes on arm64 and x86_64; fails on both if either half of
the fix is reverted. 200-class SUnit batch after the fix: 8365 P / 0 F / 0 E /
1 T (the T is the known FinalizationRegistry flake).

## 2. CWD-relative file resolution (`d1cd608e`)

Confirmed, then fixed. Same launch directory (`/tmp/cwdtest`), image in
`/tmp/harness`, evaluating `FileSystem workingDirectory pathString` and
`'probe.txt' asFileReference exists`:

    stock Cog   /private/tmp/cwdtest   probe=true
    ours (was)  /private/tmp/harness   probe=false
    ours (now)  /private/tmp/cwdtest   probe=true

The VM chdir'ed to the image directory so Pharo's StartupPreferencesLoader would
find the `startup.st` eval mode writes. That was never necessary: the loader
reads `FileSystem workingDirectory` — `StartupPreferencesHandler>>lookInImageFolder`
is misnamed — so the script belongs in the CWD and nothing has to move.

A second bug fell out: a *relative* image path could not be opened at all,
because the image is loaded after the chdir moved out from under it.
`test_load_image harness/Pharo.image eval "42 factorial printString"` went from
"Cannot open image file" to answering.

Writing `startup.st` into the CWD would otherwise make the "startup.st CWD trap"
(CLAUDE.md) more likely, so that got closed at the same time: the generated
script deletes itself as its first statement (verified a `timeout`-killed eval
leaves nothing behind — the C++-side cleanup was skipped on any abnormal exit),
the stale-marker sweep runs in both modes rather than only non-eval, and eval
mode refuses to overwrite a `startup.st` that is not ours.

`PHARO_CHDIR_IMAGE_DIR=1` restores the old behaviour.

Validation: the whole file/path/resolver family — 26 classes, `FileReference`,
`FileLocator`, `DiskFileSystem`, `Path`, `{System,Unix,Windows,Platform,
FileSystem,Interactive}Resolver`, `MCFileIn`, ... — 512 P / 0 F / 0 E, run both
from the image directory and from an unrelated CWD. SUnit bare launch also
re-verified from a non-image directory.

**Not claimed:** that this fixes soccertheory's `XMLFileException: soccerML.dtd`.
That attribution in the x86 doc is reasoned, not measured. The CWD divergence was
real and is now gone; whether it was soccertheory's cause needs the package
loaded to confirm.

## 3. Weak-reference tests — NOT flaky. Deterministic, and JIT-only.

`docs/x86-test-run-2026-08-10.md` and WIP.md both recorded these as "confirmed
flaky rather than confirmed broken", from 3 FAIL / 1 PASS over four isolated x86
runs. Re-measured on arm64:

    WeakOrderedCollectionTest, isolated, in-suite
      ours          10 runs    2 FAIL every run    (0 P / 2 F, 10/10)
      stock Cog      3 runs    2 PASS every run    (2 P / 0 F, 3/3)

There is nothing flaky about it. Both `testWeakOrderedCollectionAllGarbageCollected`
and `testWeakOrderedCollectionSomeGarbageCollected` fail deterministically under
our VM and pass deterministically under Cog. The tests fill a
`WeakOrderedCollection`, drop the strong references, `Smalltalk garbageCollect`
three times, and assert the weak entries were cleared — so something in our VM is
keeping the referents alive.

### What has been ruled out

    PHARO_NO_JIT=1                    2 PASS      <-- the JIT is required
    PHARO_NO_SISTA=1                  2 FAIL
    PHARO_NO_SISTA_PER_BC=1           2 FAIL
    PHARO_T1_NO_INLINE_NEW_ASM=1      2 FAIL      (not the inline eden alloc)
    PHARO_T1_NO_EDEN_NEW=1            2 FAIL
    PHARO_NO_GEN_CLONE=1              2 FAIL
    skip the J2J save-pool GC roots   2 FAIL      (temporary unsound diagnostic)
    nil context slots above stackp    2 FAIL      (all THREE reuse paths)

`materializeFrameStack` reuses contexts in three places — the saved-frame loop's
`frame.materializedContext`, `currentFrameMaterializedCtx_`, and the
`frame[0] == activeContext_` re-sync — and NONE of them nils the slots above the
new `stackp`, so a frame re-materialized at a shallower depth leaves the deeper
snapshot's operands in a heap object the process still reaches. That is a real
stale-root bug and matches every symptom, but nil-ing all three tails does not
fix these tests (5 runs: 0 P / 2 F each), so it is not the retention path here.
Not committed — a real correctness improvement, but unproven and it costs a
tail sweep per materialization, so it needs its own evidence first.

The J2J save pool was the obvious suspect — `forEachRoot` deliberately visits the
FULL reservation `[0, j2jPoolCursor_)` and the comment there already names a
"stale-slot pin ... one-chain-lifetime cosmetic" with WeakAnnouncer as the
observed symptom. Skipping that walk entirely still fails 2/2, so it is not this.

`PHARO_NO_FRAME0_REUSE=1` could not be used as a bisect: it hangs the run.

### Where it does and does not reproduce

    direct call, 40x in a loop (so JIT-warmed)             40/40 OK
    forked at userBackgroundPriority, 6x                    6/6 OK
    SUnit's own TestCase>>run, 6x                           6/6 OK
    our runner (scripts/pharo-headless-test)               FAILS every time

So it needs the full runner: the fork at a test priority, the P60 watchdog that
preempts on every wake-up, and the nested `on:do:` chain around `runCase`.

### The observation that points at the mechanism

Recompiling the test with two extra `Stdio stderr` diagnostic statements and one
extra temp — printing the surviving set before asserting — makes it PASS under
our runner (`[WPROBE-A] size=1 classes=#(#UndefinedObject)`), matching Cog. Adding
statements that overwrite frame slots is exactly what makes a dead-slot retention
bug disappear.

Reading that together with "JIT required" and "runner-only", the shape is
**dead-slot residue** somewhere the collector still reaches.

### The pin is TRANSITIVE, not a root — new tool: `PHARO_WATCH_ROOT_CLASS`

Knob-bisecting was the wrong instrument, so this session added a real one.
`PHARO_WATCH_ROOT_CLASS=<ClassName>` makes `forEachRoot` report which ROOT
CATEGORY is visiting each instance of that class:

    [ROOT-WATCH] Process oop=0x3009d1a30 via nlr-saved-states

Categories tagged: `vm-registers`, `nlr-saved-states`, `world-renderer`,
`operand-stack`, `saved-frames`, `method-cache`, `jit-code-zone`,
`jit-count-map`, `jit-state`, `j2j-save-pool`, `bv-closure-stack`,
`sista-save-pool`. Costs one predictable branch per root visit when unset.

Result for this bug, over a whole suite run: **zero visits** for `Duration` and
**zero** for `OrderedCollection`, with the probe verified working (14 `Process`
visits via `nlr-saved-states` in the same run). Neither the dropped
`OrderedCollection` nor the `Duration` inside it is ever a direct GC root — the
pin is transitive, through some rooted object that reaches them.

That rules out the whole "a root category over-scans" family, including the
frame-slot-residue hypothesis above, and points at a *heap object* that holds
the reference: a materialized Context, a JIT-side structure, or an image-side
cache. Next step is provenance during the MARK phase (record, for each object,
the parent that first reached it) so the surviving referent can be walked back
to its root — `PHARO_WATCH_ROOT_CLASS` is the root-level half of that tool.

### Reproducing

    cd /tmp/harness
    printf 'WeakOrderedCollectionTest\n' > /tmp/sunit_class_names.txt
    rm -f /tmp/sunit_run_completed.txt /tmp/sunit_test_results.txt
    ./build/test_load_image /tmp/harness/Pharo-sunit.image
    grep -E '^(Pass|Fail):' /tmp/sunit_test_results.txt      # 0 P / 2 F

    # Cog control (same image, external class list)
    printf 'WeakOrderedCollectionTest\n' > /tmp/sunit_test_classes.txt
    /tmp/harness/pharo --headless /tmp/harness/Pharo-sunit.image eval \
      "'$PWD/scripts/pharo-headless-test/run_sunit_cog.st' asFileReference fileIn"

Note this also gives a *deterministic* handle on the weak-reference family that
the x86 doc flagged as "the one finding that correlates across suites" —
porpoise's `PropertyManagerTest>>testPropertyManagerValueWeakness` is very likely
the same defect.

## 4. Still open

- **arm package sweep** (~1 h, ~$0.60 on a fresh box). Needs an AWS box; not
  launched. `f034b896` and `d1cd608e` both landed since the lost run, so it
  would now survive and would also re-test the CWD change against the
  soccertheory case.
- **Weak-reference frame-slot pin** — diagnosed above, not yet fixed.
- **`build-hunt`'s 21 MB in git history** — needs a history rewrite plus a
  force-push that invalidates every clone. Not done unilaterally.
- **`sudo xcode-select -s /Applications/Xcode.app/Contents/Developer`** — needs
  a password. Build scripts self-correct via `DEVELOPER_DIR`.
- **Re-run the five timeout-only packages with `WORKERS=1`** and **run
  `scripts/classify-sunit.py` against a stock-Cog baseline** — both from the x86
  doc's next-steps list, both still unrun.
