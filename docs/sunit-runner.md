# The SUnit sweep runner

One script runs the SUnit sweep on our VM and on stock Cog:
`scripts/pharo-headless-test/run_sunit.st`.

## What the runner does

The runner changes nothing in the image under test. It defines no class and
it installs no method. It runs every test with `TestCase>>runCaseManaged` in
the calling process, inside the SUnit execution environment. It sorts the
result as `TestResult>>runCase:` does. The time limit of a test is the SUnit
limit (10 s by default, or the limit of the class or of the test). A test that
runs longer than its limit gets `TestTookTooMuchTime` from the SUnit watchdog.
The runner writes the result as TIMEOUT.

The runner takes the tests of a class from `buildSuite tests`. A
`ParametrizedTestCase` class gives one test for each selector and each
parameter set. The name of a test is `nameForReport` with each white space
character replaced by `_`.

## Run it

Make the image once. The image needs the fake GUI and the image fixes:

    <vm> fresh.image eval "'scripts/pharo-headless-test/setup_fake_gui.st' asFileReference fileIn. 'scripts/pharo-image-fixes.st' asFileReference fileIn. Smalltalk snapshot: true andQuit: true"

Stock Cog saves with `eval --save`. Our VM does not know `--save`: use the
snapshot as above.

Run some classes on either VM:

    SUNIT_PREFIX=/tmp/mine <vm> image eval "'/path/run_sunit.st' asFileReference fileIn"

Put the class names, one for each line, in `<prefix>_class_names.txt`. Put the
index range in `<prefix>_batch.txt` (`first last`). Put selectors in
`<prefix>_method_names.txt`. The runner writes `<prefix>_test_results.txt` and
`<prefix>_test_detail.txt`.

Full sweeps: `scripts/sunit-sweep.sh` (our VM) and `scripts/cog-sweep.sh`
(stock Cog). Both use the same runner file. Always set `SUNIT_PREFIX` to a
private path. On our VM set `PHARO_EVAL_NO_SUNIT_MARKER=1` for each `eval`.

## Special cases that remain

- The Morphic UI manager is installed (`MorphicUIManager new beDefault`). A
  headless image uses the non-interactive manager and the fake-GUI tests need
  the Morphic one.
- The fake-GUI cycle loop is started (`FakeGUI startCycleLoop`), when the image
  has it. Spec and Morph tests wait for a World cycle.
- The file `pharo.version` is written beside the image. A Pharo installation
  has this file. `ReleaseTest>>testPharoVersionFileExists` reads it.
- For `ReleaseTest>>testUnknownProcesses` the runner stops the fake-GUI cycle
  loop and starts it again after the test. The test lists all processes.
- An expected failure runs. A failure or an error is XFAIL. A pass is a FAIL
  (unexpected pass). SUnit has the same rule.
- Option `SUNIT_TIMEOUT_MULT=N` (off by default) sets `TestCase
  defaultTimeLimit:` to N times 10 s for a slow build. Classes with a limit of
  their own keep it.
- `SUNIT_COUNT_ONLY=1` writes the class count to `<prefix>_class_count.txt`.

## Image fixes

A defect of the image or of a test that also fails on stock Cog goes in
`scripts/pharo-image-fixes.st`. The file is filed in on both VMs. Entries 8 and
9 came from this runner:

- 8: `OutOfMemoryTest>>setUp` installs the Low Space Watcher. An earlier test
  (`OupsDebuggerSelectorTest`) leaves no watcher, because the process monitor
  of SUnit ends the process that `DebugSession>>terminate` installs.
- 9: The manifest of `Collections-Unordered` names a dependency that the
  analyser does not find. The warning is a test failure in SUnit.

## Differences from Cog that this work found

- Primitive 230 `relinquishProcessorForMicroseconds:` lets lower priority
  processes run on our VM. On stock Cog the primitive only sleeps. The old
  runner polled at priority 80 with this primitive, so on Cog the test process
  never ran (TIMEOUT(prim-stuck)).
- A recursion 100000 frames deep raises an error on our VM (20000 frames work). Cog works at 100000 and is
  limited only by memory.
