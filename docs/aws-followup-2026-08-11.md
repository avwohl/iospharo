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
the reference.

### Second tool: `PHARO_WATCH_HEAP_CLASS` — the parent during MARK

The heap-level half. `PHARO_WATCH_HEAP_CLASS=<ClassName>` (or
`PHARO_WATCH_HEAP_CLASSIDX=<n>`) makes the mark phase report the PARENT object
and slot that reaches each instance of the watched class:

    [HEAP-WATCH] Duration => classIndex 5169
    [HEAP-WATCH] 0x3000bb7c0 <- parent 0x3012b4ba8 cls=DateAndTime slot=1/4

The class name is resolved to a classIndex once by scanning `classTable_`, and
the watching path is a separate loop so the non-watching mark keeps the bare
`markAndTrace` call. `PHARO_WATCH_HEAP_MAXLOG` (default 400) caps lines per GC.

What it says so far:

- **Duration is a red herring.** Every traced reference is the single shared
  `0x3000bb7c0` reached from `DateAndTime` slot 1 — the timezone offset carried
  by the 1000 `Date today` objects the test creates, not the test's own
  `Duration new`.
- **OrderedCollection**, over one suite run, 29002 traced references: the bulk
  are ordinary image structure (`ObservableValueHolder` 8960, `MCWorkingCopy`
  6160, `MCRepositoryGroup` 5504, ...), but **42-50 have a `Context` parent**.
  That is the population to sift next: a Context still holding the test's
  dropped `anArray` is consistent with every observation, and it is a heap
  object, so `PHARO_WATCH_ROOT_CLASS` correctly saw nothing.

Note both probes perturb timing enough to flip the result occasionally (one run
with a small log budget went 1 P / 1 F) — the same Heisenbug sensitivity as
adding statements to the test body. Read them for provenance, not for pass/fail.

### Third tool: `PHARO_WEAK_SURVIVOR_PATHS` — and the answer

Records, for every object, the first parent that reached it during the mark,
then in `processWeaklings` walks that chain back for each weak referent that
SURVIVED, annotating Context parents with slot, stackp and selector:

    [WEAK-ALIVE] OrderedCollection in WeakArray slot 0:
      <- Context(0x303eaaf58)[6]{stackp=4 liveSlots=6..9 slot=live
           method=testWeakOrderedCollectionAllGarbageCollected}
      <- FullBlockClosure(0x303eb17e8)[0]
      <- Context(0x303eaaf58)[12]{... slot=DEAD-RESIDUE ...} <- (cycle)

The holder is the test method's OWN context, at indexed slot 6 = temp 0 =
`anArray` — the variable the test assigns nil before its three
`garbageCollect`s. The context is legitimately reachable, because
`3 timesRepeat: [Smalltalk garbageCollect]` is a real send whose
`FullBlockClosure` holds it as `outerContext`. (Slot 12, above stackp, is
separately the dead-tail residue described earlier — real, but not the cause.)

`PHARO_MAT_FULL_RESYNC=1` changes neither the trace nor the result, so the
context is not a skipped re-sync.

### Fourth tool: `PHARO_TRACE_FRAME_TEMPS` — the frame is stale, not the context

Dumps the C++ frame slots `materializeFrameStack` reads, as it reads them:

    [FRAME-TEMPS] #testWeakOrderedCollectionAllGarbageCollected fp=0x6ff80ab50
                  numTemps=4: t0=OrderedCollection t1=Time
                              t2=WeakOrderedCollection t3=nil

`t0` and `t1` are exactly the two variables the test nils. **The JIT's stores
never landed in `savedFP + 1 + t`**, which is where the materializer reads
temps. That explains the whole shape:

- `PHARO_NO_JIT=1` passes — the interpreter writes where the materializer reads.
- `PHARO_MAT_FULL_RESYNC=1` is inert — it re-copies the same stale slots.
- The direct 40x loop passes — nothing materializes, so the disagreement never
  becomes visible.
- Adding statements to the test body "fixes" it — it changes the frame layout.

**This is much bigger than weak references.** Any materialization of a JIT
frame — preemption, block creation, `thisContext` — can capture pre-store temp
values, and a resumed frame then continues from them. That is very likely the
same root as the SlotIntegration materialization Heisenbug in `deferred.md`,
whose signature is "a live temp comes back with the wrong VALUE after the build
is preempted and its frames are materialized to a context and restored".

### FIXED — `88ce3fee`: `executeFromContext` disowned the context it restored

The frame-push trace resolved it. Same activation, two stack addresses:

    11 pushes  savedFP+1 = 0x44780ab60   t0 = OrderedCollection
    [FRAME-TEMPS]    frame materialised into a Context, t0 = OrderedCollection
    [EXEC-FROM-CTX]  activation restored from that Context — NEW stack address
     4 pushes  savedFP+1 = 0x44780a960   t0 = nil   <- `anArray := nil` ran here

`executeFromContext` rebuilds a suspended activation into a fresh C++ frame and
sets `activeContext_ = context`, but it also cleared
`currentFrameMaterializedCtx_`. The running frame no longer knew it owned that
context, so nothing synced back into it, while the activation carried on at a
different address. Anything still holding the context — the `timesRepeat:`
closure's `outerContext`, the sender chain — kept seeing pre-restore values, and
the GC marked them. It also meant `thisContext` would not answer the same object
the closure already held.

The fix is the invariant: the frame being built IS that context's activation, so
it owns it, and later materialisations re-sync that object instead of leaving it
frozen. That is also why `PHARO_MAT_FULL_RESYNC=1` was inert — it re-copied from
the stale `savedFP`.

    WeakOrderedCollectionTest in-suite
      before   0 P / 2 F  in 10/10 runs     (stock Cog: 2 P / 0 F, 3/3)
      after    2 P / 0 F  in 14 of 16 runs

Regression coverage — ~14,000 tests, zero new failures:

    classes 1-200         8365 P / 0 F / 0 E / 1 T   (T = known FinalizationRegistry flake)
    classes 201-565       4309 P / 0 F / 1 E         (E = OCClassBuilderTest
                                                      testCreateNormalClassWithTraitComposition,
                                                      the upstream image bug that
                                                      fails on stock Cog too)
    29 context-identity   1288 P / 0 F / 0 E / 0 T   (Context, BlockClosure,
      -sensitive classes                              Continuation, Become, Object,
                                                      Process*, StDebugger*, simulation,
                                                      reflectivity, weak)

**It costs ~9% on one benchmark.** Isolated, four runs each, `1M blocks`
(`1 to: 1000000 do: [:i | [x := x + 1] value]`): 220/228/229/235 ms before,
241/251/245/252 ms after — no overlap, so it is real and not the noise that
made the earlier full-suite deltas look meaningful. Every other bench is flat.
An owned context means materialisations re-sync an existing object rather than
taking the cheap path, which is the correct model (Cog's married context IS the
frame and a temp store writes through) but is not free. Worth optimising later;
not worth trading the correctness back for.

Two things remain open here:

- **~2 runs in 16 still fail one of the two tests**, so a second, smaller
  retention path exists.
- Nil-ing the slots above `stackp` in all three of `materializeFrameStack`'s
  context-reuse paths does NOT close that residual (tested on top of the fix:
  still 8/10) and costs a tail sweep per materialisation, so it is not shipped.
  The stale tail is real, but it is not this.

### Earlier next-step note (superseded by the fix above)

Find where the JIT writes temps for an activation that is a CALLER — this one
is in `savedFrames_`, suspended inside a `timesRepeat:` send — and why that
location differs from `savedFP + 1 + t`. There is at least one JIT path that
uses a different convention (`Interpreter.cpp:17189`,
`state->tempBase = callerSP - nArgs` on the Sista self-recursion inline; that
particular one pushes no SavedFrame, so it is not the culprit, but it shows the
two conventions coexist). Instrument the frame push to record the tempBase in
effect and compare it with `savedFP + 1`.

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
