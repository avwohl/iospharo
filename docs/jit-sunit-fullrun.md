# Full-coverage SUnit run under JIT

Goal: run **every** concrete `TestCase` subclass in a clean Pharo 13 image
under the JIT VM (`test_load_image`), recording per-class results, even though
a single class can hard-hang the VM.

Clean image: ~2051 concrete (non-abstract) `TestCase` subclasses discovered by
the runner, ~14600 test methods.

## The in-image runner already works

`scripts/pharo-headless-test/run_sunit_tests.st` (submodule, ~818 lines) is the
canonical runner: it discovers all non-abstract `TestCase` subclasses, supports
batch ranges (`/tmp/sunit_batch.txt` = "`start end`" indices) and a class/method
filter, has per-test watchdogs and a scheduler-death logger, and registers a
`SessionManager` startup handler so `test_load_image` auto-runs it on resume.
The SparseLargeTable/Unicode bug that once made every test report 0/0 is fixed
(`DIAG: Unicode isLetter:$t=true`).

A single launch writes per-test detail to `/tmp/sunit_test_results.txt`
(CR-delimited — normalise with `tr '\r' '\n'`) and `/tmp/sunit_test_detail.txt`,
and a `/tmp/sunit_run_completed.txt` marker when its batch finishes.

## Why a driver is still needed

A monolithic run of all 2051 classes **stalls partway** — some class hard-hangs
the VM in a way the in-image per-test watchdog cannot interrupt (observed: the
run froze with no output for >90 s during a `testAverageWith*` class, ~4404
tests in). After the hang nothing more runs.

`scripts/run_all_tests.sh` is the project's batch-relaunch driver for exactly
this, but it `curl`s a fresh image from the network each batch and uses
`/tmp/Pharo.image` + `/tmp/pharo` — it does not fit an offline run against a
local prepped image.

## Offline driver: `scripts/run_all_sunit.sh`

Drives the canonical in-image runner over its batch-range mechanism, fully
offline, with a fresh VM process per window for isolation:

    SKIP_PREP=0 WINDOW=50 STALL_SECONDS=150 \
      scripts/run_all_sunit.sh /tmp/harness/Pharo-jit.image

- Runs window `[start, start+WINDOW-1]`; a STALL-based watchdog kills a window
  only when `/tmp/sunit_test_results.txt` stops growing for `STALL_SECONDS`
  (a clean window exits early via the `run_completed` marker).
- After each window it counts completed classes (`Total:` lines). If the window
  stalled, the class right after the last completed one is the hanger: it is
  appended to `/tmp/sunit_hangers.txt` and skipped (`start += completed + 1`),
  so only genuine hangers are dropped and the run continues to the end.
- Outputs: `/tmp/sunit_all_results.txt`, `/tmp/sunit_all_detail.txt`,
  `/tmp/sunit_hangers.txt`, `/tmp/sunit_all_summary.txt` (aggregate P/F/E/S +
  hanger count).

This is the tool that gets *all* SUnit tests to run under the JIT: every class
is attempted; the handful that hang the VM are isolated in `sunit_hangers.txt`
for follow-up root-causing rather than blocking the whole suite.

## History note

Earlier in this work I mis-concluded (from a transiently garbled tool-output
channel) that the in-image runner was a non-functional stub and that
`LinkedListTest` hangs the VM. Both were wrong: the runner is the mature
818-line harness, and the apparent `LinkedListTest` "hang" was an artifact of a
first-draft custom driver whose watchdog watched a progress file the real
runner never writes, so it killed the full pass every 120 s and restarted it
from class 1. That custom runner was removed; this driver wraps the real one.

## Results — complete passes (measured, re-counted from combined results)

Both passes ran all 2051 class names end-to-end (one Total: kept per class).

    pass                 classes   P      F     E      S    rate
    single-image (53 win) 2021     21614  478   5378   132   78.3%
    fresh-image  (48 win) 2022     21541  731   5026   139   78.5%

`FRESH_IMAGE=1` (pristine image copy per 50-class window) was expected to lift
the rate by removing cumulative image-state degradation. It did NOT: 78.5% vs
78.3%. **So the ~5000 errors are mostly NOT cumulative-state artifacts** — they
reproduce on a clean image. An earlier "true per-class rate ~99%" note was an
over-extrapolation from the first ~360 kernel classes; the kernel passes ~99%,
but the broader image (reflectivity, serialization, tooling, graphics, network)
has many genuine errors. Coverage is the real achievement: every class attempted.

### The errors are mostly REAL VM bugs (vs Cog), NOT image/environment

CORRECTION (2026-06-01): an earlier draft called these a "VM-compat ceiling" and
"not fixable". That was wrong — I had only compared JIT vs our-own-interpreter,
which cannot distinguish our-VM bugs from image issues. Compared against **stock
Cog (Pharo 10.3.9) on the SAME image**, many pass on Cog and fail on ours:

    class                        Cog            our VM (interp)   verdict
    SystemEnvironmentTest        217P/0F/0E     79P/0F/138E       OUR VM BUG
    TraitTest                    54P/0F/0E      (errors)          OUR VM BUG
    ReflectivityReificationTest  81P/2F/29E     60P/16F/36E       partly ours
    ReflectivityControlTest      44P/2F/25E     ~71E              partly ours
    StDebuggerTest               3P/1F/58E      ~57E              ~Cog too (debugger/UI)
    ZnClientTest                 3P/4F/43E      1P/48E            ~Cog too (network)

The single failing test `SystemEnvironmentTest>>testCollectThenSelectOnEmpty`
run directly on Cog: 1 run, 0 errors. On our VM: NonBooleanReceiver. So these are
genuine fixable VM defects (a boolean is mis-evaluated inside an Iceberg event
listener that fires as a harness side effect), not environment gaps. Only a
subset (network ZnClient, UI StDebugger) also fail on Cog and are out of scope.
The 78% is therefore NOT a fixed ceiling — fixing the VM bugs would raise it.

### Classes the driver flagged (watchdog stalls, not necessarily hangs)

15 in the fresh run: SplitJoinTest, ScheduleTest, OCClosureCompilerTest,
CDClassDefinitionParserTest, PackageTest, GArcTest, GRayTest,
FileAttributesPluginPrimsTest, ManyTestResourceTestCase, SocketStreamTest,
SpFontStyleTest, SpTreeAdapterSingleSelectionTest,
SpTreeTableAdapterMultipleSelectionTest,
StDebuggerToolbarCommandTreeBuilderTest, TimespanDoSpanAYearTest. The flag set
differs run-to-run (timing-sensitive); none verified as a true infinite loop.

## Confirmed JIT bug (the one real codegen finding)

RGMethodDefinitionTest: JIT 22P/9F/1E vs interpreter 31P/1F (3/3 each, stable).
`PHARO_T1_NO_IC_PROBE=1` restores the interpreter result (known-good, non-
regressing on kernel classes); the inline IC probe is the cause. Details and the
deferred next-step (probe-path value-store instrumentation) in
docs/sunit-hangers-classified.txt.

## Full 2051-class run with Sista inliner fix (2026-06-03)

Post-`4b446bf4` (Sista 2-value `^self` inliner) + `0a293966` (watchdog skips
IDLE) re-baseline using `scripts/run_all_sunit.sh`. The driver runs each window
of 50 classes in a fresh `test_load_image` subprocess with STALL=150s and
HARD_CAP=1200s, so VM-level hangs in one test class can't poison the next
window.

Class list source: `gen_sunit_class_list.sh` from a clean cogfresh image →
`/tmp/sunit_all_class_names.txt` (2051 entries — every concrete `TestCase`
subclass, curated `test_classes.txt` first then alphabetical).

Wall time: 7h05m (one m1-class Mac, JIT + Sista on, default settings, no env
overrides). 56 windows total.

    classes_covered = 2011
    tests_run       = 27522
    P=22063  F=374  E=4952  S=133
    pass_rate       = 80.16%

26 classes deterministically lock the VM (driver kills the subprocess, advances
past). The hanger set forms three dense clusters and one long tail:

  - graphics  (G*): GEllipseTest, GMatrixTest, GRayTest, GRectangleTest,
                    GVectorTest
  - Roassal   (RS*): RSDSMTest, RSInspectorShapeTest, RSKernelDensityTest,
                     RSRoassalTest, RSSVGTest
  - Spec UI   (Sp*/Sy*): SpMorphicScrollableAdapterTest, SliderTest,
                         SycMethodNameEditorTest-area
  - misc:     SplitJoinTest, CollectionArithmeticTest, CDVariableClassParserTest,
              BlockClosuresTestCase, EpFileOutModificationsTest, FLMigrationTest,
              FileAttributesPluginPrimsTest, MetaLinkAnonymousClassBuilderTest,
              ReleaseTest, SimpleTestResourceTestCase, TKTWorkerPoolTest,
              TableLayoutTest, UnlimitedInstanceVariableSlotTest, ZnChunkedStreamTest

Full hanger list: `/tmp/sunit_hangers.txt`. Window-by-window outcomes:
`/tmp/sunit_driver.log`. Per-class results: `/tmp/sunit_all_results.txt` (54 MB).
Per-test detail: `/tmp/sunit_all_detail.txt`.

### What the run did NOT do — Cog comparison

The Cog full-suite baseline measured earlier on the same morning used the
curated `test_classes.txt` (565 classes), not the auto-generated 2051-list, so
the two cannot be cleanly diffed. To make a true post-fix vs Cog comparison,
Cog needs re-running against `/tmp/sunit_all_class_names.txt` with the same
subprocess driver. Tracked separately.

### What the run did show vs prior baselines

CDNormalClassParserTest 16/16 and SystemEnvironmentTest 199/217 (formerly
79P/138E, now Cog-parity), both first verified on the targeted 21-class re-run
(commit `97d0f4db`), reproduce here at the same numbers in their respective
windows — confirming the Sista inliner fix's impact at full-suite scale.

### The watchdog patch in this run

The earlier overnight attempt (pre-`0a293966`) emitted
`[VM-TIMEOUT] Process … at P10 stuck for 600s+ — terminating` every 10 minutes
when the IDLE process at priority 10 was legitimately sitting in
`primitiveRelinquishProcessor`. This run had ZERO P10 timeout noise; the
watchdog now gates on `prio > 10`. Real stuck low-/normal-priority processes
still fire — the driver's STALL-watchdog catches them externally regardless.

### Open: in-image per-test timeout doesn't always recover

`run_sunit_tests.st`'s 300s per-test timeout (`relinquishProcessorForMicroseconds:`
polling + force-kill via `testProcess suspend` + `terminate` fork) successfully
fires on most timeouts (`TIMEOUT: ...` entries appear in the log), but
some tests leave the scheduler in a state where post-timeout cleanup itself
blocks indefinitely. That's why the 26 hangers exist — without subprocess
isolation each one would freeze the whole run. Fixing the in-image
recovery path is the next lever for raising effective throughput per VM
instance; the watchdog fix `0a293966` only addresses misleading diagnostics,
not the underlying deadlock.


## 2026-06-07: two fixes unblock the full headless run

Two independent root-cause fixes, both required for the suite to run end-to-end:

1. **NLR block-inline fast-path bug** (`Primitives.cpp`, commit 542747d1).
   `primitiveFullClosureValue`'s ~8 block-inline fast paths accepted `0x5C`
   (ReturnTop = `^` non-local return) as if it were `0x5E` (BlockReturnTop =
   local block return), silently turning `^`-from-a-block into a local return.
   This corrupted `on:do:` / `ensure:` / `ifCurtailed:` and every helper that
   relies on `^` from a passed-in block — i.e. the entire SUnit error/failure
   handling stack. JIT-independent (reproduces with `PHARO_NO_JIT=1`). Fix:
   the fast paths now require the LOCAL terminator `0x5E` only; `^`-blocks fall
   through to the correct general `activateBlock` NLR path. Bench blocks
   (accumulators/comparators, all `0x5E`) are unaffected. Symptoms it fixed:
   `[:s | ^ s+1000] value: 42` returned 0 not 1042; `FileReference>>contents`
   returned `self`.

2. **Headless render-loop startup wedge** (runner submodule, commit ad111f8,
   bumped into the parent by 35c27b21). The custom VM's headless resume restarts
   the saved WorldMorph `MorphicRenderLoop` at pri-80; with no display it
   busy-spins `WorldState>>drawWorld:`, hits Morphic DNUs
   (`SpStyleEnvironmentColorProxy>>isTransparent`), starves the pri-80 Delay
   scheduler and the scheduler dies (`timerSem=nil`) → only-idle deadlock BEFORE
   any test runs. The runner's existing morphic-suspension lived ~111 lines into
   `runAllTests` (after class-list/batch setup) — too late, since the C++
   deferred-timer bootstrap fires at step 25M. Fix: moved the suspension to the
   first action of `SUnitRunner class>>startUp:`. Must re-prep the image after
   this change (stock headless `eval --save` fileIn).

After both fixes (fresh stock-headless-prepped Pharo 13 image): the suite starts
cleanly and runs, watchdog timeouts recover without wedging (e.g. 1 TIMEOUT at
~class 37 was handled and the run continued). 3-class smoke = 685 pass / 0 fail.
Full-suite measurement in progress.

## 2026-06-08 — gap reassessment (the gap is mostly already closed + VM-speed)

A clean full run (Pharo-final2, fake GUI, WSNextPut fix) reached ~663 classes at
~98% pass before dying. Investigating the named gaps from the earlier 252-pass
analysis:

- **Debugger (ED*, ~63): FIXED** earlier (WSNextPut / `format()==Indexable`).
- **Fuel FLCreateClassSerializationTest (~33, the biggest Fuel bucket): ALREADY
  PASSES 41/41** — closed by the WSNextPut/NLR fixes. Memory's "8 vs 41" is stale.
- **Fuel WideString/WideSymbol (3 tests): NOT a gap** — they're in
  `FLBasicSerializationTest>>expectedFailures` (known Fuel limits;
  `Smalltalk globals at: aWideString put:` legitimately errors "Only symbols
  accepted"). Fail on Cog too. (Caveat: both runners use bare `runCase`, which
  does not honor the `expectedFailures` METHOD — fair, both fail them.)
- **Fuel FLBlockClosure block-materialization (~5): real bug**, not yet fixed —
  materialized block's `sourceNode` returns an `OCReturnNode` (wrong AST node) so
  `BlockClosure>>isClean` (`^self sourceNode isClean`) DNUs. Hard to repro in pure
  eval (the eval DoIt confounds it into `FLMethodChanged: DoIt changed bytecodes`);
  needs runner-context or lldb.
- **Epicea (Ep*, ~20): a concurrency RACE, not a JIT codegen bug** — OmDeferrer
  forks a pri-40 background flush process that preempts the non-atomic
  `OmBlockFileStore>>entriesDo:` (file-read then buffer-read) and loses an entry.
  JIT scheduling timing exposes it (NO_JIT/DET_SCHED correct, JIT wrong); explicit
  `store flush` before read fixes it. Not cleanly fixable VM-side.
- **VM-speed timeouts (the dominant remaining bucket):** e.g. the full run dies at
  ClyNotebookPageRecyclerTest — FreeType/FFI text composition where every
  `platformLongAt:` hits uncached `FFIArchitecture>>forCurrentArchitecture`
  (8490/sec on our VM vs ~100x faster on Cog). Pathological slowness, not a loop.

**Bottom line:** the easy/medium gaps (~96 passes: debugger + FLCreateClass) are
already closed. The remaining gap to >= Cog is dominated by (a) the 10-100x
VM-speed deficit (timeout failures) and (b) run-survivability (one slow/hanging
test kills the whole run before measurement completes), plus a couple of hard
bugs (Epicea race, block-materialization). Highest-leverage next work is
run-completion / VM performance, not grinding individual test correctness.
