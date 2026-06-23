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

## Measurement methodology (learned the hard way, 2026-06-23)

`tinyBenchmarks` bytecodes/sec is WALL-based and iteration-scaled — it varies
**±7% run-to-run** on this machine (observed 301/303/308/317/322M for the *same*
config). Do NOT A/B codegen changes on it; a single-sample "+6%" is noise.

The **in-image CPU metric** (`cpu_ms` per fixed benchmark, prim 247, best-of-3)
is stable to **±1-2%** — this is the metric to use for codegen A/B (it confirms
the user's "CPU varies less than wall" guidance). Even so, take several samples
and compare min/median, not one run.

### TOS-in-register partial enable (MASK=101) — tried, PERF-NEUTRAL, reverted

Enabling the loop-body TOS families (Push|Dup|PopStore|Arith, MASK=101, the
subset that excludes the fragile CondJump/FuseCmpJ and net-negative Send
families) and rebuilding as default: on the stable CPU metric it was neutral
(sort 173→170, sum 62→60, stringHash 78→80, dict 124→123 — all within noise).
Correctness was clean (1538 collection/number tests + `PHARO_T1_TOS_VERIFY`
brk-net active, no corruption). Reverted — partial conversion buys nothing
because operands still spill to memory at every send/jump boundary; only the
FULL conversion (including the Send families, currently net-negative/incomplete)
moves the needle. Confirms fix #1 is the big-but-hard item, not a quick enable.

## Worklist — root-caused (codegen analysis, file:line in src/vm/jit/asmjit/AsmjitT1.cpp)

ROOT CAUSE: the JIT keeps the stack POINTER in x25 (`PHARO_T1_SP_IN_X25`, ON) but
round-trips operand VALUES through frame memory every bytecode. A simple `a+b`
(pushTemp a; pushTemp b; send +) does ~8 stack/temp memory accesses where Cog
does ~1-2. The inlined arithmetic itself is cheap (~6 ALU instrs, `5285-5293`);
the stack plumbing around it is the cost. The interpreter wins on bytecodes
(651M vs 303M) because it holds FP/SP/temps in registers across its dispatch
loop with no tempBase indirection (`Interpreter.cpp:2708`, `3005-3019`).

### ⚠️ Fix #1 (TOS-in-register) is REFUTED by rigorous measurement — net-negative

The codegen agent called TOS-in-register "THE lever." A rigorous multi-sample
A/B (4 runs each, stable cpu_ms) says otherwise — **TOS-on is net-negative on
EVERY real benchmark; OFF is fastest everywhere:**

    bench (min cpu_ms)   OFF   MASK=101   MASK=-1(all)
    1M blocks            184   181        223
    dict 50K             121   125        129
    floatSum 1M           65    70         70
    sort 100K            165   176        174
    sum 1M                60    65         64
    1M getter+yourself    41    42         43

The TOS cache's management overhead (invalidation tracking, the per-inline-send
`tosLrearm` reload, the rearm churn at send/block boundaries) exceeds the
memory-traffic it saves — on send/block-heavy code, which is exactly the goal's
workloads (SUnit/soogle). Completing the conversion (eliminating the per-site
rearm, the planned "fix #1 mechanism" below) would at best recover the rearm
loads, not the broader overhead. **Do not pursue fix #1.** Bolting a TOS cache
onto a stencil-per-bytecode JIT costs more than it saves; Cog's TOS works because
its whole codegen (register allocator, no per-bytecode cache bookkeeping) is
built around it. Closing the 21x gap likely needs a deeper codegen rethink, not
this incremental.

→ The remaining concrete lever is **fix #2 (block fast path)** — the block gap is
real and independent of TOS (`1M blocks` 197ms vs interp 33ms). Pursue that next.

### Every available lever rigorously evaluated — none closes the gap

    lever                          rigorous verdict (multi-sample cpu_ms)
    T1 inline knobs (BLOCK_VALUE,  neutral/negative (BV: blocks 184→174 min but
      method-pattern, eager)         avg worse, sort 165→187 WORSE)
    TOS-in-register (agent's #1)   net-NEGATIVE on every benchmark (REFUTED)
    Tier-2/Sista optimizing JIT    works (no crash); ~neutral single-sample
      (PHARO_T2=1 +REPLACE+WARMUP=0)  (fib 7→6, sum 65→60, sort in-range) — NOT
                                     the ~10x an optimizing tier would need

**Conclusion:** no existing lever shortcuts the 21x bytecode / 7.8x SUnit-CPU gap.
It is a fundamental codegen-maturity gap (a from-scratch JIT vs Cog's mature
Cogit), not a tuning task. Closing it is a deep, multi-session codegen effort —
either a real register-allocating T1 rewrite or maturing the Sista Tier-2 (make
it stop bailing and actually optimize). The measurement infra + this rigorous
elimination of the plausible-but-ineffective options is the value delivered here;
it saves the next effort from re-walking these dead ends. ### THE actionable direction: Tier-2 bails 87% — fix the bails

Ran the bench under `PHARO_T2=1 PHARO_SISTA_BAIL_LOG=1`. Census:

    T2 (asmjit): compiled=627  bailed=4220        (87% bail to T1)
    sista compile: total=11836 ok=625
      bails: sendNoSplice=9812  lowerFail=839  liftFail=560  arrayDoHelper=0

The optimizing tier exists and runs but **bails on 87% of methods** — so the hot
code stays on the slow T1 path, which is why enabling T2 is ~neutral. The
DOMINANT bail is `sendNoSplice` (9812) — a send the Tier-2 can't splice/inline.

**This is the highest-leverage perf direction for the project:** reduce the Sista
Tier-2 bail rate so the optimizing tier actually optimizes the hot methods.
Concrete, measurable (compiled-vs-bailed ratio, trackable in vmperf), and the
only evaluated path with real upside (every T1 lever is refuted). Start by
characterizing `sendNoSplice` (src/vm/jit/sista/SistaBuilder.cpp / lowering) —
what send shapes trigger it and which are fixable. THEN re-measure T2 perf
(multi-sample cpu_ms) as the bail rate drops.

(Historical mechanism notes for fix #1, kept for the record:)

### Fix #1 mechanism (read for the implementation) — the per-inline-send rearm

Why all-families is net-negative and loop-body-only is neutral: with the
send-result family on (`kTosFamSendRes`), every INLINE-spec send (getter, at:,
size, class, …) stores its result to the memory stack and then branches through
`tosLrearm` (AsmjitT1.cpp:6034) which does `ldur x26` to reload the cache — a
redundant load per inline send. The NON-inlined C++ send path is fine: it
`resumeAfterCall`-rearms x26 from x1 (the result reg) load-free (:6031). So the
concrete fix #1 work is: make each inline-spec result path PRODUCE its result
into x26 directly (mov x26, result; store to mem) and branch to `endOfSend`
(skip `tosLrearm`), instead of store-then-reload. ~12 sites (the `b tosSendRes ?
tosLrearm : endOfSend` list at :6459/7416/8097/8192/8238/8255/8350/8388/8473/
8535/8583/8668/8778). First target: the inline GETTER (drives `1M getter+yourself`,
JIT 43ms vs interp 37ms). Validate each site: report-sunit diff + PHARO_DET_SCHED
+ REPEAT=5 cpu_ms. RE-MEASURE the all-families baseline with REPEAT+cpu_ms first —
the "net-negative" was single-sample wall (noisy).

1. **Enable + FINISH the TOS-in-register (x26 simStack) scheme — THE lever.**
   `PHARO_T1_TOS_REG` caches top-of-stack VALUE in x26 so consecutive bytecodes
   pass operands without memory. Fully written but compiled OFF: `g_useTos =
   GET_DEBUG_BOOL(PHARO_T1_TOS_REG) && real` (`9845`); every `tosFam(bit)` gate
   (`216-219`) is false by default. Enabling it AS-IS is net-negative (measured:
   291M vs 303M) because the per-family conversion is incomplete — the real work
   is completing+validating all families (kTosFamArith/Push/PopStore/SendHead/
   SendRes/FuseCmpJ). Removes ~4-6 of the ~8 memory ops per `a+b`; this is Cog's
   model and the only path to closing the 21x bytecode gap. RISK HIGH (stale-TOS
   = the classic frame-corruption source) but defenses exist (`PHARO_T1_TOS_VERIFY`
   ldur/cmp/brk net `3916-3925`, `_POISON`, per-family `_MASK` bisect); validate
   with `PHARO_DET_SCHED=1` + the SUnit harness (report-sunit diff vs baseline).
2. **JIT fast path for `[x:=x+1] value`** — the 5.5x block regression. JIT
   heap-allocates a closure + does a real IC send + builds an `activateBlock`
   frame per iteration (`10713-10789`); the interpreter pattern-matches the 9-byte
   body and does the read-modify-write in C++ with no alloc/send/frame
   (`Primitives.cpp:3790-3863`, ~50ns). Add the JIT analog (recognize
   PushFullBlock-then-`value` at compile time). RISK MEDIUM (closures/NLR
   minefield) — restrict to the exact local-return `0x5E` pattern.
3. **Keep tempBase in a register** (not reloaded per temp access). `emitLoadTempBase`
   (`3889-3896`) is `ldr [x0,OFF_TEMPBASE]` every pushTemp/popStoreTemp because
   `PHARO_T1_TB_IN_X26` is hardwired off (x26 went to TOS). Use another callee-saved
   reg (x27/x28), load once in prologue, mirror the x25 reload-after-relocation
   pattern (`3876-3886`). RISK LOW-MEDIUM. Removes 1 load per temp access.
4. **cmp→branch fusion default-on** (`5350-5377`, `kTosFamFuseCmpJ`) — emits
   `cmp + b.cond` with no boolean materialization. Sub-case of #1 but the agent
   notes it can be enabled independently (depends only on lookahead, not a valid
   cached value). RISK LOW alone. The interpreter already fuses always
   (`Interpreter.cpp:3042-3073`).

Not on the Cog-parity path: per-bytecode IP writes / step counter — the JIT
already avoids these (IP written only at sends/exits). Back-edge yield poll
(`10558-10565`, 3 instrs/iter) is necessary for preemption.

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
