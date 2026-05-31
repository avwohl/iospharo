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

Measured, re-counted from the combined results file; per-class verdicts use
3 runs per side on pristine image copies (single runs proved nondeterministic).

### Coverage passes

- Single-image pass (complete): 53 windows, all 2051 class names, ONE reused
  image. 2021 classes ran; P21614/F478/E5378/S132 = **78.3%**. 20 classes
  driver-flagged "hangs". Rate depressed by cumulative image-state degradation
  (E=5378 is its signature).
- Fresh-image pass (`FRESH_IMAGE=1`, pristine copy per window): PARTIAL — killed
  at ~window 18 (857 classes) for hang isolation; P15775/F329/E1539/S51 = 89.2%.
  A clean end-to-end fresh-image pass is still TODO.

## Hang investigation — outcome

The driver's stall-watchdog ("no results growth for STALL_SECONDS") is an
unreliable hang flag. **None of the spot-checked "hangers" is a real infinite
loop — they complete when run alone.** The 20 are a mix of cumulative-state
artifacts (vanish on a fresh image, e.g. StringTest) and slow/erroring classes
the watchdog mislabeled.

IMPORTANT: single-class results here are **nondeterministic** — a class can pass,
error, or time out across identical runs (the repo's known timing-sensitive JIT
Heisenbugs; use `PHARO_DET_SCHED=1` for stable repro). So verdicts below are from
3 runs per side; flaky cases are called out.

### Confirmed JIT-correctness bug: RGMethodDefinitionTest (stable, 3/3)

    JIT-on : Total 32  P22 F9 E1     (3/3 identical)
    JIT-off: Total 32  P31 F1 E0     (3/3 identical)

9 failures + 1 error appear ONLY under JIT, every run. Root-caused by knob
bisection (3/3 each):

    PHARO_T1_NO_IC_PROBE=1     -> 31P/1F/0E  == interpreter   ** FIX **
    PHARO_T1_NO_INLINE_GETTER=1-> 22P/9F/1E  (no effect)
    PHARO_NO_J2J=1            -> 22P/9F/1E  (no effect)

=> the bug is the **inline IC probe** (`t1ICProbe`, AsmjitT1.cpp:1817+,
default-on since 2026-05-16), NOT the inline getter or J2J. Failure shapes point
to a wrong/nil method dispatched via the monomorphic IC: "Got nil instead of
'Point'", ERROR "receiver of ',' is nil" (in `parentName,'>>',selector`), and
"Got OrderedCollection class instead of OrderedCollection class" (equal
printStrings comparing unequal — a wrong object from the IC). The IC key is the
22-bit classIndex (AsmjitT1.cpp:1864-1865); a stale/colliding slot-0 entry
dispatches the wrong method. Same IC-probe family as the documented aigraph/ring
JIT bugs. This is the actionable next fix.

Repro:

    printf 'RGMethodDefinitionTest\n' > /tmp/sunit_class_names.txt
    cp /tmp/harness/Pharo-jit.image /tmp/t.image
    ./build/test_load_image /tmp/t.image                      # 22P/9F/1E (JIT bug)
    PHARO_T1_NO_IC_PROBE=1 ./build/test_load_image /tmp/t.image  # 31P/1F   (fixed)

### Flaky (timing-sensitive, NOT a clean codegen bug): GPointTest

Flips across runs: JIT 18P/2E or "0 tests"; JIT-off passed earlier this session
but hung 3/3 later. Direction-unstable => a `PHARO_DET_SCHED` Heisenbug, not a
deterministic JIT-vs-interp diff. GTriangleTest/StringTest run clean.

Per-class detail: docs/sunit-hangers-classified.txt. Raw flags: docs/sunit-hangers.txt.

