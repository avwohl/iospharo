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

