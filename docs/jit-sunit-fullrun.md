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

## Results

Measured (re-counted from the combined results file), not extrapolated.

### Single-image pass (complete)

53 windows over all 2051 class names, ONE prepped image reused across every VM
launch. 2021 classes produced results; P21614 / F478 / E5378 / S132 = **78.3%**.
Driver flagged 20 classes as "hangs". Rate depressed by cumulative image-state
degradation (reused image dirties across 53 launches — the repo's known
cumulative-state-artifact pattern; E=5378 is its signature).

### Fresh-image pass (partial, INCOMPLETE)

`FRESH_IMAGE=1` (pristine image copy per 50-class window). Killed at ~window 18
(~900 classes) to free the VM for hang isolation: 857 classes, P15775 / F329 /
E1539 / S51 = **89.2%** (partial). A clean end-to-end fresh-image pass is TODO.

## Hang investigation — the "hangs" are not hangs

The driver's stall-watchdog ("no results growth for STALL_SECONDS") is an
unreliable hang signal. Verdicts below are confirmed on PRISTINE image copies,
each class alone, JIT-on then PHARO_NO_JIT=1 sequentially (NO concurrent VMs —
they share /tmp/sunit_* and corrupt each other). Every flagged class checked
COMPLETES — none actually hangs:

    class                    JIT-on        JIT-off       verdict
    GPointTest               18P/0F/2E     20P/0F/0E     JIT BUG (2 errors only under JIT)
    GTriangleTest            7P/0F/0E      7P/0F/0E      clean
    RGMethodDefinitionTest   17P/13F/2E    17P/13F/2E    NOT JIT (identical both ways)
    TestValueWithatHelpTest  completes,0 tests           not a hang
    StringTest               clean (fresh-image window 0)  artifact

**The one confirmed JIT-correctness bug: GPointTest.** Two tests error ONLY
under JIT, both "MessageNotUnderstood: receiver ... is nil":

    ERROR: testSetX            (receiver of "setX:setY:" is nil)
    ERROR: testPolygonClipping (receiver of ... is nil)

A receiver that is a valid object under the interpreter is `nil` under JIT — a
JIT value/slot not materialized correctly. This is the actionable next target.

**RGMethodDefinitionTest is NOT a JIT bug** (an earlier claim that it was came
from a stale/raced results read — corrected): it fails 13F/2E *identically* with
JIT off, so its failures are VM/interpreter or image-compat issues in the Ring
metamodel (e.g. `parentName` reflection returning nil), independent of the JIT.

So the "20 hangers" are: cumulative-state artifacts (vanish on fresh images) +
classes the watchdog mislabeled while their failing-test exception printout ran
slow/recursive ("Error printing blockClosure in: a CompiledBlock"). Remaining
flagged classes still need one-at-a-time isolation.

Repro the GPointTest JIT bug:

    printf 'GPointTest\n' > /tmp/sunit_class_names.txt
    cp /tmp/harness/Pharo-jit.image /tmp/t.image
    ./build/test_load_image /tmp/t.image                 # 18P/2E (JIT, errors)
    PHARO_NO_JIT=1 ./build/test_load_image /tmp/t.image  # 20P/0E (correct)

Per-class findings: docs/sunit-hangers-classified.txt. Raw flags: docs/sunit-hangers.txt.

