# Test runs: one VM at a time

Each VM for a test starts through `scripts/vmrun.sh`.  The script has a lock,
so only one VM runs on the machine.  The script checks the free memory
before each start and stops a VM that uses too much memory.  No VM is left
when a run ends.  The header of the script has all options.

`<vm>` is `ours` (the tree `build-rel`), `nojit`, `x86`, `dbg`, `cog` (stock
Cog for Pharo 14), `cog13`, or `cogp` (stock Cog with the changes of
`docs/upstream/`).

    scripts/vmrun.sh status
    scripts/vmrun.sh stop
    scripts/vmrun.sh build build-rel

## One class or one test

    scripts/vmrun.sh test ours ZdcSocketStreamTest
    scripts/vmrun.sh test cog  FloatTest testNaNCompare
    scripts/vmrun.sh test --repeat 25 ours ZdcSocketStreamTest

Each test gives one line: `PASS`, `FAIL`, `ERROR`, `SKIP` or `TIMEOUT`, the
name and the time.  `--repeat N` runs the class N times, each time in a new
VM, and prints the count of each line.  `--interactive` gives the test an
interactive session.  `--image <path>` uses another image.

## An expression file

    scripts/vmrun.sh eval ours scripts/cogdiff/taskit_pool_stress.st

The file is one Smalltalk expression sequence.  The printed result is the
answer of its last statement.  The image is not saved.

## The image for the test runs

The test image is a pristine Pharo image with the fake GUI and the image
fixes.  It has no runner inside.

    scripts/vmrun.sh prep ours <pristine.image> <out.image> \
        scripts/pharo-headless-test/setup_fake_gui.st scripts/pharo-image-fixes.st

Use `cog` in place of `ours` for the image of the stock VM.  The script
prints the number of `FIXED`, `ADDED` and `SKIP` lines of the image fixes.
A generator writes `scripts/pharo-image-fixes.st`: edit the files in
`scripts/image-fixes/`, then run `python3 scripts/gen-image-fixes.py`.

## The full suite

    scripts/vmrun.sh sweep ours <test.image> <outdir>
    scripts/vmrun.sh sweep cogp <test.image> <outdir>
    scripts/vmrun.sh interactive ours <test.image> <outdir>

A sweep runs all test classes of the image in batches of 50 classes, with
the runner `scripts/pharo-headless-test/run_sunit.st`
([sunit-runner.md](sunit-runner.md)).  The totals are at the end of
`<outdir>/sweep.log`.  `<outdir>/all_detail.txt` has one line for each test.
A sweep of our VM takes about 8 minutes on an M4 Mac.  `interactive` runs
the classes of `scripts/interactive-classes.txt` in an interactive session.

To run a part of the class list again (for a batch that stopped):

    scripts/vmrun.sh sweep --from 1301 --to 1400 ours <test.image> <outdir>

This form has no retry pass and no interactive pass, so a batch that stops
shows as stopped (`rc=139` for a segmentation fault).

## The 200 packages

    scripts/vmrun.sh pkgs <outdir> <first-row> [last-row]
    scripts/vmrun.sh pkgs --redo  <outdir> <row>
    scripts/vmrun.sh pkgs --reuse <outdir> <row>

A row is a line of `scripts/pkg-jit-test/packages-200.tsv`.  For each row:

1. Stock Cog loads the package from GitHub with Metacello and saves the
   image.
2. Stock Cog files in the fake GUI, the image fixes and the fixes file of
   the package.
3. Stock Cog writes the list of the test classes of the package.
4. Stock Cog runs the list.  Our VM runs the same list on a copy of the same
   image.

`<outdir>/summary.tsv` has one line for each package: the state, the totals
of each VM, and the number of tests that differ between the VMs.
`<outdir>/res/<label>.diff` has the tests that differ.  A package with a
final line does not run again.  `--redo` runs the package again with a new
load.  `--reuse` runs the package again from the image of the earlier load,
with no download.

A package that has a defect can have these files in `scripts/pkg-jit-test/`:

- `patches/<label>.patch`: a git diff of the repository of the package.  The
  load then comes from a local clone with the patch.
- `locks/<label>.txt`: dependencies that need the patch of another row.  Each
  line has the baseline name of the dependency and the label of that row.
  The load takes the dependency from a local clone with the patch.
- `fixes/<label>.st`: entries in the form of `scripts/image-fixes/`.
- `fixtures/<label>.txt`: the names of test classes that are fixtures of
  other tests.

The record of the package repairs is
[upstream/package-repairs.md](upstream/package-repairs.md).
