# perfdb — VM test/benchmark results tracking

Records every test/benchmark run into the `vmperf` MySQL DB on awohl.com, keyed by
(VM binary, test source+url, machine, knobs) with CPU + wall time — so **unchanged
Cog results never need re-running** and JIT-vs-Cog progress is tracked over time
and across machines (laptop + AWS spot instances).

## Files

    schema.sql        DB schema (machine / vm_build / test_source / run /
                      test_result / bench_result). Apply once:
                        ssh -p 24 wohl@awohl.com mysql < schema.sql
    perfdb.py         recorder CLI (talks to remote via ssh+mysql — no local
                      driver). register-{machine,vm,source}, run-and-record,
                      record, have-correctness, query, report-bench.
    record-bench.sh   bench-suite on Cog + our VM, recorded.
    record-sunit.sh   SUnit on Cog + our VM, per-test recorded; skips Cog when
                      a prior result exists.
    bench_inject.st   the benchmark harness (emits in-image CPU ms + wall ms).

## Connection

`perfdb.py` shells out to `ssh -p 24 wohl@awohl.com mysql vmperf`. Override:

    PERFDB_SSH="ssh -p 24 wohl@awohl.com"   # set "" to use a local mysql
    PERFDB_DB="vmperf"

Works unchanged from any host with ssh access to awohl.com (AWS spot instances
included — they self-register their machine row, instance type, and whether the
host exposes hardware perf counters).

## Identity / dedup

- **vm_build** is fingerprinted on the **binary's sha256** (`--binary`), not git
  HEAD — committing unrelated scripts/docs must not forge a new "VM build". The
  git sha is kept as metadata. Cog is identified by its version string.
- **machine** is fingerprinted on hostname+arch+cpu_model+cpu_count (so reused
  AWS hostnames don't collide).
- **knobs** (the env vars that change JIT behaviour) live on the run, not the
  build — same binary under different knobs = different runs.

## Usage

    # bench both VMs, record, print JIT-vs-Cog (wall ratio + jit cpu):
    scripts/perfdb/record-bench.sh
    scripts/perfdb/record-bench.sh --ours-only --jit-knobs "PHARO_T1_TOS_REG=1"

    # SUnit, a subset (both VMs run the SAME classes):
    scripts/perfdb/record-sunit.sh --classes "ArrayTest SetTest"
    # full suite, skip Cog if already recorded for this image:
    scripts/perfdb/record-sunit.sh \
      --classes-file scripts/pharo-headless-test/test_classes.txt \
      --skip-cog-if-present

    # reports / ad-hoc:
    python3 scripts/perfdb/perfdb.py report-bench --source bench-suite
    python3 scripts/perfdb/perfdb.py query "SELECT ..."
    python3 scripts/perfdb/perfdb.py have-correctness --source <id> --vm <cog_id>

## Codegen A/B (multi-sample — single runs are too noisy)

`tinyBenchmarks` wall rate varies ±7% run-to-run; do not A/B codegen on it. Use
the in-image `cpu_ms` per fixed benchmark (stable ±1-2%) and take several
samples:

    REPEAT=5 JIT_KNOBS="PHARO_JIT_DEFER=15"                scripts/perfdb/record-bench.sh --ours-only
    REPEAT=5 JIT_KNOBS="PHARO_JIT_DEFER=15 PHARO_T1_TOS_REG=1" scripts/perfdb/record-bench.sh --ours-only
    # compare min/median cpu_ms per benchmark across the samples of each config:
    python3 scripts/perfdb/perfdb.py query "
      SELECT b.bench_name, r.knobs, COUNT(*) n,
             MIN(b.cpu_ms) min_cpu, ROUND(AVG(b.cpu_ms),1) avg_cpu, MAX(b.cpu_ms) max_cpu
      FROM bench_result b JOIN run r ON r.id=b.run_id JOIN vm_build v ON v.id=r.vm_build_id
      WHERE v.kind='jit' AND b.bench_name IN ('sum 1M','sort 100K','dict 50K')
      GROUP BY b.bench_name, r.knobs ORDER BY b.bench_name, r.knobs;"

A change is real only if its min/median CPU separates from the other config's
range across the samples.

## Metrics

Two timings per run: in-image (the benched block only — CPU ms via prim 247 on
our VM, wall ms via the image timer on both) and process-level (child CPU + wall
via rusage, wrapping the whole VM invocation). **CPU is the cross-machine-stable
metric** (the user's guidance: CPU varies less with machine/load than wall). Cog
lacks the CPU primitive, so per-benchmark cross-VM comparison uses wall; our VM's
own progress is tracked on CPU.

## AWS spot instances

`perfdb.py` runs unchanged on a spot instance (it only needs `ssh -p 24
wohl@awohl.com`). `register-machine` auto-detects `cloud=aws`, the instance
type/id (via IMDS), and whether the host exposes hardware perf counters
(`supports_perf_counters` — some instance types don't), so cross-machine results
are distinguishable. The CPU metric is the one to compare across machines (it
varies far less than wall with instance type / neighbor load).

To wire the existing spot pipeline (`scripts/aws/` — bootstrap builds the Linux
arm64/x86 VM, `sunit-ab-box.sh` etc. run suites): have those run scripts invoke
`record-bench.sh` / `record-sunit.sh` instead of (or alongside) their ad-hoc
result files. The Linux VM build is a distinct `vm_build` (different binary
sha256 + arch), so its results never collide with the macOS rows.

See `docs/results-perfdb.md` for the current JIT-vs-Cog baseline + worklist.
