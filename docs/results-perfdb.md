# Perf tracking DB (vmperf) + JIT-vs-Cog baseline

The `vmperf` MySQL DB (on awohl.com, schema `scripts/perfdb/schema.sql`) records
every test/benchmark run keyed by (vm_build git-sha, test_source+url, machine,
knobs) with CPU + wall time, so unchanged Cog results are never re-run.  Tooling:
`scripts/perfdb/{perfdb.py,record-bench.sh,bench_inject.st}`.  See
`scripts/perfdb/README` (this doc) for the recorder CLI.

## Baseline — 2026-06-23, machine `neoA18` (Apple A18 Pro arm64), image Pharo 13.1 (45e803d)

`tinyBenchmarks` (the cleanest single number; in-image wall via the image's own timer):

    vm               bytecodes/sec     sends/sec
    Cog v10.3.9      6,385,000,000     445,000,000
    our JIT            303,000,000     128,000,000     (21x / 3.5x slower than Cog)
    our interp         651,000,000      16,000,000     (JIT OFF, PHARO_NO_JIT=1)

### THE headline finding (surfaced immediately by the tracking DB)

**Our JIT is SLOWER than our own interpreter on bytecode-heavy and block-heavy
code.  It only wins on sends.**

    benchmark (best-of-3 wall ms)   JIT-on   interp(JIT off)   verdict
    tinyBenchmarks bytecodes/sec     303M        651M          JIT 2.1x SLOWER
    1M blocks                        181ms        33ms         JIT 5.5x SLOWER
    dict 50K                         124ms        91ms         JIT slower
    stringHash 100K                   82ms        59ms         JIT slower
    sort 100K                        178ms       163ms         JIT slightly slower
    sum 1M / floatSum 1M             ~equal      ~equal         no win
    fib(28)                           10ms        62ms         JIT 6x faster (sends)
    tinyBenchmarks sends/sec         128M         16M          JIT 8x faster

Interpretation: the T1 JIT helps **send-bound** code (fib: 8x) but its compiled
straight-line / loop / arithmetic code is no better — often worse — than
interpreting it, and its **block-activation path is ~5x heavier than the
interpreter's**.  Cog's 21x bytecode-rate lead comes from compiling loops to
tight native code (operand stack in registers, inlined arithmetic with no
per-bytecode memory traffic); our stencil-per-bytecode codegen round-trips the
operand stack through memory, so it loses to a good threaded interpreter.

### Knobs swept — none close the gap (all recorded in vmperf)

    config                                   bytecodes/sec   sends/sec   note
    default (PHARO_JIT_DEFER=15)             303M            128M        baseline
    + INLINE_BLOCK_VALUE (+BV_MAX_CAP)       318M             11M        sends COLLAPSE (BV disables inline-J2J)
    + method-pattern inlines (bit 51-58)     308M            141M        marginal
    + PHARO_JIT_DEFER=0 (eager)              307M            139M        marginal
    + PHARO_T1_TOS_REG=1                     291M            148M        bytecodes WORSE (mid-rollout, incomplete)

The opt-in inlinings that dominate WIP history (block-value, J2J, getters) are
correctness levers, not the speed lever.  Most speed-relevant inlinings
(inline-J2J, at:/at:put:/size, getter/setter, basicNew, class) are already
DEFAULT-ON, and the gap persists with them on.

## SUnit — Cog baseline (permanent; never re-run) + the SUnit perf gap

Full kernel suite recorded once on Cog and stored per-test, so it never needs
re-running (`record-sunit.sh --skip-cog-if-present` honors it):

    Cog v10.3.9, 544 classes / 12,898 tests:  P12778 F4 E96 S20   25.8s CPU / 31.1s wall

Full kernel suite, both VMs (same class list), both completed clean:

    vm            pass    fail  error  skip    CPU      wall
    Cog          12778     4     96    20     25.8s    31.1s
    our JIT      12671     0      4    29    200.7s   278.6s    = 7.8x CPU / 9x wall slower

CPU (7.8x) is the stable cross-machine metric. The SUnit gap (7.8x) is milder
than the microbench bytecode gap (21x) because real code mixes sends (JIT wins)
with bytecode/blocks (JIT loses). Same codegen-quality story.

### JIT correctness vs Cog (per-test diff, `report-sunit`)

12,898 common tests, 12,692 agree. Only **4 JIT regressions** (Cog passes, JIT
doesn't) — all in compiler/closure/crypto code, concrete leads for the
correctness side:

    OCASTAndOrTranslatorTest>>testFalseAndAnythingReturnsFalse   cog=pass jit=error
    OCBlockNodeTest>>testIsClean                                 cog=pass jit=error
    OCClosureCompilerTest>>testDebuggerTempAccess                cog=pass jit=error
    SHA256Test>>testFips180Example3                              cog=pass jit=timeout

9 tests the JIT passes that Cog fails/errors; 193 cog-only (JIT run didn't reach
them). Diff stored — re-checkable any time without re-running Cog.

NOTE: `OCBlockNodeTest>>testIsClean` PASSES when run in isolation on the JIT — so
these 4 are context/timing-dependent (full-suite forked-process interleaving),
the known JIT Heisenbug class (see CLAUDE.md `PHARO_DET_SCHED`), not clean
reproducible bugs. The diff flags candidates; confirming needs DET_SCHED + the
full-suite interleaving. Not on the perf path.

## Worklist (the real perf project, in impact order)

1. **JIT codegen quality for straight-line/loop bytecode** — the 21x bytecode
   gap.  Keep the operand stack / TOS in registers across a basic block instead
   of load/op/store per bytecode; inline SmallInt arithmetic without the
   per-op memory round-trip.  TOS_REG is the start of this but is incomplete and
   currently a net loss — finish or replace it.  This lifts EVERY bytecode-heavy
   benchmark and is the only path to Cog parity.
2. **Block activation cost** — `1M blocks` JIT 181ms vs interp 33ms.  The JIT's
   block `value`/activation path is ~5x heavier than the interpreter's.  Make
   block invocation cheap (the BLOCK_VALUE inline is the intended fix but
   currently sacrifices cross-method inline-J2J; needs the both-on fix from
   docs/block-value-inline-debug.md UPDATE 31).
3. **Interpreter send rate** — interp sends are 16M/s (~60ns/send).  Not on the
   Cog-parity path (the JIT handles sends) but explains why JIT-off is unusable
   for send-heavy code.

## How to reproduce / extend

    # bench both VMs, record into vmperf, print JIT-vs-Cog:
    scripts/perfdb/record-bench.sh
    # our VM only, custom knobs:
    JIT_KNOBS="PHARO_JIT_DEFER=15 PHARO_T1_TOS_REG=1" scripts/perfdb/record-bench.sh --ours-only
    # compare configs:
    python3 scripts/perfdb/perfdb.py report-bench --source bench-suite
    python3 scripts/perfdb/perfdb.py query "SELECT ..."

Cog correctness/timing for a fixed image never needs re-running:
`perfdb.py have-correctness --source <id> --vm <cog_id>` returns a prior run if
present.  `record-bench.sh --skip-cog-if-present` honors it.
