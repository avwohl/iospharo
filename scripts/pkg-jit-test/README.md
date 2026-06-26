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
                      whose package name starts with a prefix in
                      /tmp/pkg_prefixes.txt; emits a RESULT line + FAIL/ERR detail
run-pkg-jit-test.sh   driver: runs a loaded image on both VMs, diffs failures
load_*.st             Metacello load scripts (run on stock pharo, eval --save)
results/              captured benchmark logs
```

## Workflow

1. Get a stock Pharo 13.1 image + `pharo` launcher (e.g. `curl get.pharo.org/64/130+vm | bash`).
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
