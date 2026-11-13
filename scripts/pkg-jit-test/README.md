# pkg-jit-test — test the JIT against real third-party packages

The kernel SUnit suite under-covers real-world bytecode. This harness loads
third-party Pharo packages (parsers, serializers, numerics) and runs their test
suites + benchmark kernels on BOTH the custom JIT VM and stock Cog, to find JIT
correctness bugs and measure speed vs Cog.

See `docs/jit-test-packages.md` for the original ranking, load expressions, the
measured results, and the bugs found so far.

## The 200-package sweep

`packages-200.tsv` is a manifest of **200 maintained Pharo packages with tests**
(mined from the soogle index — see `soogle-mining/` for provenance, and
`docs/jit-test-packages-200.md` for the human-readable list + funnel). Columns
include the verified Metacello `load_expr`, the SUnit `test_prefix`, a `test_path`
(`sunit` headless or `gui` via `pharo-headless-test`), and a headless-load risk.

Drive the whole sweep (resumable) with `run-manifest.sh` — per package it has the
stock Cog VM Metacello-load+save a clean image, then runs the suite on both VMs
and records load status + JIT-only failures:

```
PHARO=/tmp/h3/pharo BASE_IMAGE=/tmp/h3/Pharo.image \
  CUSTOM_VM=$PWD/build-rel/test_load_image \
  scripts/pkg-jit-test/run-manifest.sh        # writes /tmp/pkg200/summary.tsv
```

Best run on the AWS build box (16 vCPU) as a long backgrounded job — the
keep-alive lease keeps the box up while a Claude drives it. The 27 `gui` packages
(Spec/Bloc/Roassal/Morphic/…) get the `pharo-headless-test` fake-GUI prelude
automatically.

## Runner output and log reading

The test code writes to stdout without a newline at the end. The runner line
can then follow other output on the same line, for example
`Resources SetUpRESULT pass=34 ...`. Our VM also echoes the image stdout with
the prefix `[IMAGE] `, and some output has CR line endings. Therefore:

- The runner prints a newline before each CLASSES, RUN, FAIL, ERR, TIMEOUT
  and RESULT line.
- The scripts do not use `grep '^RESULT'`. They use the functions in
  `lib-extract.sh`. Each function changes CR to LF, finds the line text
  with `grep -aoE` anywhere in a line, removes duplicates, and sorts with
  `LC_ALL=C`. Sort both inputs the same way before you use `comm`.
- The runner prints `RUN Class>>selector` before each test. The runner counts a test that sends
  `halt` as an ERR, because Halt is not an Error.
- When a VM gives no RESULT line, the summary shows
  `NO-RESULT last-RUN=Class>>selector exit=N`. The last RUN line is the test
  that was running when the run ended. The exit status is 124 for a timeout,
  137 for a kill (often the OOM killer) and 139 for SIGSEGV.
- `summary.tsv` has the columns `label load cog_RESULT jit_RESULT
  jit_only_fails cog_exit jit_exit`.

## Fake-GUI prelude

For `gui` packages the driver sets the env var `PKG_PRELUDE` to the path of
`setup_fake_gui.st`. The runner files the prelude in itself. Do not put the
prelude text before the runner text: Pharo 14 rejects a temp declaration that
follows a statement. If `PKG_PRELUDE` names a file that does not exist, the
runner prints `PRELUDE-MISSING <path>` and exits with a failure status.

## Load step

`run-manifest.sh` runs each package load in a scratch working directory
(`$OUT/loadwd-<label>`), and removes the directory after the load. A package
load can write files to the current directory. The load log ends with a line
`[load exit=N]`, so a load that ends with no error message still has a cause.

## Why this exists

The custom VM cannot do HTTPS, so it cannot Metacello-load packages. The STOCK
Cog VM does the network load + saves the image; both VMs then run the loaded
`TestCase` subclasses against that one saved image. The only variable is the VM,
so any pass-rate divergence is a VM/JIT bug.

## Files

```
bench_spectrum.st     cross-VM speed spectrum (int/float/recursion/sends/
                      collections/alloc/strings) — emits `BENCH <name> <ms>` lines
run_pkg_tests.st      generic SUnit runner: runs every non-abstract TestCase
                      whose package name starts with a prefix in PKG_PREFIXES
                      (or /tmp/pkg_prefixes.txt); emits RUN lines, a RESULT line
                      and FAIL/ERR/TIMEOUT detail lines
run-pkg-jit-test.sh   driver: runs a loaded image on both VMs, diffs failures
run-manifest.sh       driver: loads and tests the packages in a manifest
run-sweep-parallel.sh driver: runs run-manifest.sh in N workers
lib-extract.sh        shell functions that read the runner lines out of a VM log
load_*.st             Metacello load scripts (run on stock pharo, eval --save)
results/              captured benchmark logs
```

## Workflow

1. Get a stock Pharo image (13.1 or 14.0) + `pharo` launcher (e.g. `curl get.pharo.org/64/140+vm | bash`; use 130 for Pharo 13.1).
2. Load a package on the stock VM and save:
   ```
   cp Pharo.image polymath.image
   pharo polymath.image eval --save "$(cat load_polymath.st)"
   ```
   (or use the raw Metacello expression from `docs/jit-test-packages.md`)
3. Run the suite on both VMs and diff:
   ```
   PHARO=./pharo CUSTOM_VM=../../build-rel/test_load_image \
     ./run-pkg-jit-test.sh ./polymath.image polymath Math-Tests
   ```
   Output: each VM's `RESULT pass=.. fail=.. err=..` line, then the JIT-only
   failures (candidate JIT regressions) and Cog-only failures (baseline).

4. Confirm a JIT-only failure is really JIT (not a VM-core or image issue) by
   re-running it with `PHARO_NO_JIT=1`; if it then passes, it is a JIT codegen
   bug. If it still fails, it is a VM-core defect (compare against Cog).
   For timing-sensitive failures, re-run under `PHARO_DET_SCHED=1` first.

## Speed

```
PHARO=./pharo ./run-pkg-jit-test.sh ...   # correctness
# speed: run bench_spectrum.st on both VMs and compare BENCH lines
pharo polymath.image eval "$(cat bench_spectrum.st)"                       # Cog
PHARO_MAX_STEPS=2000000000000 ../../build-rel/test_load_image polymath.image \
  eval "$(cat bench_spectrum.st)"                                         # custom JIT
```

ALWAYS use the `-O2` `build-rel/test_load_image` for timing — the default
`build/` is -O0 (~9x slower) and inflates every number.
