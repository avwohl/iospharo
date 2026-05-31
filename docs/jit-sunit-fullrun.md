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


## Results — first complete pass (2026-05-31)

Driver: `WINDOW=50`, stall-watchdog 150 s, **53 windows (33 advanced, 20 skipped
on a hang)**. The driver reached the end of the 2051-class list. Aggregate
(one `Total:` kept per distinct class name):

    classes attempted          2051
    classes with results       2048
    classes that hang the VM     20   (docs/sunit-hangers.txt)
    test methods              27631   (P 21643 / F 478 / E 5378 / S 132)
    raw pass rate            78.32%

**Caveat — the 78% is depressed by cumulative image-state degradation, not by
that many real failures.** This driver reuses ONE prepped image across all 53
VM launches, and after a skipped window it re-runs an overlapping slice on a
progressively dirtier image (E=5378 ≈ 19% is the signature of the
"cumulative-state artifact" this repo repeatedly documents elsewhere). The true
per-class rate is much higher: the earlier single-process run scored
4367 P / 12 F / 19 E (99.3%) over its first ~360 classes before it hit the
StringTest-area hang. Getting a clean per-class pass rate needs fresh-image
isolation per class (future work); this pass's value is **coverage** — every
class was attempted and the VM-hanging ones are now enumerated.

### The 20 classes that hard-hang the VM (skipped to let the suite finish)

    RGMethodDefinitionTest        ColorTest                    RSKernelDensityTest
    BlockClosuresTestCase         FileAttributesPluginPrimsTest SimpleTestResourceTestCase
    FFICalloutAPITest             MailMessageTest              SpFontStyleTest
    GPointTest                    ManyTestResourceTestCase     SpLabelPresenterTest
    GTriangleTest                 RBRefactoringChangeTest      SpMorphicBoxLayoutTest
    ClyBrowserToolValidityTest    CodeSimulationTest           SpPaginatorMorphTest
    TFUFFIDerivedTypeMarshallingInCallbackTest                 WindowsStoreTest

These are the next root-cause targets. Reproduce one in isolation:

    printf 'GPointTest\n' > /tmp/sunit_class_names.txt
    ./build/test_load_image /tmp/harness/Pharo-jit.image      # hangs (~150s no output)
    PHARO_NO_JIT=1 ./build/test_load_image /tmp/harness/Pharo-jit.image
    # JIT-on hang + JIT-off pass  => JIT bug; both hang => VM/interpreter.
