# Full-coverage SUnit run under JIT

Goal: run **every** concrete `TestCase` subclass in a clean Pharo 13 image
under the JIT VM (`test_load_image`), to completion, recording per-class
results — and survive any class that hard-hangs or segfaults the VM.

In a clean image: ~2092 concrete (non-abstract) `TestCase` subclasses,
~14593 test methods (`TestCase allSubclasses reject: isAbstract`, summed
`testSelectors size`).

## Two runners

### In-image runner (submodule, canonical)

`scripts/pharo-headless-test/run_sunit_tests.st` (~818 lines) is the mature,
heavily-tuned harness: dynamic discovery of all non-abstract `TestCase`
subclasses, batch ranges (`/tmp/sunit_batch.txt`), class/method filters,
per-test watchdogs, a scheduler-death exception logger, Morphic-process
suspension, and a wall-clock batch deadline. It registers a `SessionManager`
startup handler so `test_load_image` auto-runs it on image resume.

Limitation: everything runs **inside one VM process**. A class that segfaults
the VM, or a hard hang the in-image Delay-based watchdog can't interrupt
(the Delay scheduler itself is buggy on this VM — see the systime
`valueWithin:` note), takes the whole run down with it. Its `/tmp` completion
marker then short-circuits the next launch (`SUnitRunner: previous run
completed, skipping auto-restart`).

### External crash-resilient driver (this work)

`scripts/run_all_sunit.st` + `scripts/run_all_sunit.sh` add a layer the
in-image runner can't provide: **forward progress across hard VM crashes**,
driven entirely from the shell.

- `run_all_sunit.st` redefines `SUnitRunner` with a minimal, durable loop.
  Class list = curated order (`/tmp/sunit_test_classes.txt`) first, then every
  remaining concrete `TestCase` subclass sorted by name. Optional restriction
  via `/tmp/sunit_class_names.txt`.
- State lives in `/tmp` so progress survives a crash:
      sunit_done.txt        classes whose result is durably recorded
      sunit_blacklist.txt   classes that crashed/hung the VM (skipped)
      sunit_current.txt     class currently running (crash marker)
      sunit_test_detail.txt per-class `CLS <name> P p F f E e S s` + FAIL/ERR lines
      sunit_test_results.txt final totals
      sunit_ALL_DONE.txt    sentinel: every class processed
  Each class's result is appended+flushed *before* moving on, so a crash on
  class N loses nothing already recorded.
- On each launch, `runAllTests` blacklists the previous crasher (named in
  `sunit_current.txt` but never recorded in `sunit_done.txt`), then runs every
  class not in done|blacklist.
- `run_all_sunit.sh` relaunches the VM until `sunit_ALL_DONE.txt` appears or
  `MAXITERS` is spent. Watchdog is **stall-based**: a launch is killed only
  when `sunit_done.txt` stops growing for `STALL_SECONDS` (default 120) — a
  slow-but-progressing run is never wrongly blacklisted; only a genuinely
  stuck class trips it.
- Idempotent `SessionManager` registration (`unregisterClassNamed:` first) so
  `startUp:` fires exactly once per launch.

Run it:

    SKIP_PREP=0 STALL_SECONDS=120 MAXITERS=150 \
      scripts/run_all_sunit.sh /tmp/harness/Pharo-jit.image
    # resume after interruption (keeps done/blacklist):
    SKIP_PREP=1 scripts/run_all_sunit.sh /tmp/harness/Pharo-jit.image --resume

Verified: `startUp:` fires once, a 2-class filter records correct totals
(`classes 2 passed 59`), `ALL_DONE` sentinel written. A full run blacklists
classes that hang the VM and continues past them.

## Known hard-hang under JIT

- `LinkedListTest` — observed to hang the JIT VM (no forward progress for
  >120 s; blacklisted by the driver). Classification (JIT-on vs JIT-off in
  isolation) is the next step. Repro:
      printf 'LinkedListTest\n' > /tmp/sunit_class_names.txt
      ./build/test_load_image /tmp/harness/Pharo-jit.image    # hangs
      PHARO_NO_JIT=1 ./build/test_load_image /tmp/harness/Pharo-jit.image
