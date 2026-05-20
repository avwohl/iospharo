# WIP — JIT perf session (2026-05-20, post-reboot iter)

## Session progress 2026-05-20 post-reboot (19+ commits added)

Cumulative measured perf gains (M1, PHARO_BENCH=fib, best-of-5):

    bench    pre-session    post     cog    gap     delta
    fib(28)  13.0 ms        11.0 ms  3 ms   3.7×    -15%
    fib(30)  35.7 ms        28.4 ms  ~8 ms  3.5×    -20%
    fib(32)  86 ms          72 ms    —      —       -16%
    sieve    2.3 ms         2.3 ms   ~1 ms  2.3×    flat

J2J save protocol iteration adds (newest first):
- `67ced5c6` skip add x5 for siteIdx==0 IC probes
- `91868c5e` hoist j2jDepthInc to x20 callee-saved reg
- `8bca7b15` reuse x2 (sp from IC HIT) in callee setup
- `951b1f10` hoist JM load to IC probe, share with J2J path
- `c7b4cf22` skip sub x10 calleeJM compute when not needed
- `82fa2d58` skip redundant sp write for nArgs==tempCount
- `97001c13` pre-index ldp folds cursor decrement in return prelude
- `397a09f2` post-index stp folds cursor bump into save push
- `ad6a34a7` skip save.jitMethod write in xmethod-off J2J push
- `04f8e5fc` point J2J resumeAddr directly at endOfSend
- `48c75e09` cache j2jDepth/totalCalls inc constant in JITState

Per-send instr count: ~42 (pre-session) → ~22 (now).
Per-return instr count: ~22 (pre-session) → ~16 (now).

Shipped (in order):
- `84dfad61` shrink inline-J2J self-recursive callee setup (skip
  redundant recv load + static tempCount + unrolled temp-init)
- `e77a518f` SELF_REC_BIT in IC extra bit 56 (replaces CM-oop
  compare with single tbz/tbnz; perf flat but smaller emit)
- `11bd7003` skip method/literals/jitMethod restore in J2J return
  prelude (xmethod-off path — biggest single win this session, ~8-9%)
- `ce4bcf4b` stur/wzr fold in return prelude (3 instr saved/site)
- `59aa301a` wzr fold for nArgs=0 J2J save argCount write
- `44e30412` specialize return prelude for uniform J2J argCount
  (pre-scan method bytecodes; if all sends have same nArgs, use
  the value as immediate and skip ldr+lsl+sub in return prelude)

## Sista Phase 4 — starting work

Commit `1c311666` ships the first Phase 4 step for this session:
extend `tryInlineConstReturn` with 5- and 6-value recognizers for
`^ ivar OP const` (the single-ivar simpler cousin of the existing
10-value `^ ivarA OP ivarB OP const` chain).

**State of Phase 4 (in tree):**
- `tryInlineConstReturn` handles shape-based monomorphic inlining
  for: 2-value const-return / param-passthrough / self-send chain;
  3-value getter / chain; 4-value setter; 5-value setter w/ return
  (multi-shape); 5/6-value ivar+const (NEW this session); 10-value
  ivar+ivar+const chain.

**What stops Phase 4 from helping fib (and most benchmarks):**

1. Sista's hook fires in `activateMethod` which is BYPASSED by T1's
   inline-J2J path.  For fib/benchFib (100% catch rate inline-J2J),
   Sista never runs.  Bench output confirms: `[SISTA] hits=0/1`.
2. Recognizer-based inlining only handles trivial methods (≤10
   values, fixed shapes).  benchFib has multi-block control flow +
   recursive sends — outside any recognizer's scope.
3. Even if Sista ran on benchFib, recursive self-inlining isn't
   implemented (depth limit 2 in `tryInlineConstReturn`).  And
   recursive inlining is exponential in code size — practically
   limited to 1-2 levels deep.

**Path to actually closing the fib gap (multi-session):**

- General callee-splicing IR transform (not shape recognizers):
  lift callee, substitute kLoadReceiver/kLoadArg with caller values,
  splice all blocks into caller IR, map kReturn to caller flow.
- Recursive-self handling: detect SELF_REC_BIT at compile time
  (via T1 IC hints) and inline N levels with the Nth level still
  going through J2J.
- Wire Sista into T1's dispatch path so monomorphic-inlined
  versions can supersede T1's J2J emit.

Per `docs/sista-inlining-plan.md`, the full Phase 4 estimate is
4-5 weeks of focused work, and prerequisite Phase 3 (deopt
infrastructure) is 4-6 weeks.

## Architectural conclusion

Closing the remaining ~4× fib gap to Cog requires Sista Phase 4
method inlining (multi-session per `docs/sista-inlining-plan.md`).
Each micro-shrink ships 2-10% but is bounded by the inherent
send-machinery overhead.  At 100% catch rate, fib(28)'s ~11.7 ms
is essentially the floor for the current inline-J2J architecture.

Cog's 3 ms comes from inlining the recursive call body directly
into the caller, eliminating the entire push-save / restore /
tail-call sequence.  No instruction-level shrink in the current
emit can replicate that without changing the IR/lowering strategy.

## Original session goals (preserved below)

# WIP — JIT perf session (2026-05-20, pre-reboot)

## Goal (active session-scoped Stop hook)

**Make this JIT as fast as Cog.**  Architectural gap on fib(28) is
~4.3× (13 ms ours vs 3 ms Cog); the architectural fix is Sista
Phase 4 method inlining (multi-session — see
`docs/sista-inlining-plan.md` §"Phase 4").

## Current measured baseline (PHARO_BENCH)

    bench       ours      cog       gap
    fib(28)     13 ms     3 ms      4.3×
    fib(30)     35 ms     ~8 ms     4.4×
    sieve x3    2 ms      ~1 ms     ~2×       (was BROKEN, fixed this session)
    factorial   <1 ms     <1 ms     noise

Run with: `PHARO_BENCH=fib PHARO_FIB_N=28 ./build/test_load_image /tmp/harness/Pharo.image`

The standard run_benchmarks.st bench framework still crashes on
resume via SIGSEGV in `primitiveCopyBits` (Morphic render +
saved-image interaction).  Use `PHARO_BENCH=` for measurable perf
work; pre-existing bug, not in scope.

## What this session shipped (11 commits since 14a7fa73)

Correctness:
- `9bd381a6` revert prim 60/61/62 prologue (fix Improper Store eval crash)
- `1d8ee8a7` proper fix: prim 60/61/62 prologue with fmt 10-11 (WordArray)
- `de84c68e` sieve correctness: stub-compile prim 60/61/62 methods with cond-jumps

Inline-J2J emit shrinks (~8 instrs trimmed per site, ~42 instrs now):
- `8d983dff` fold depth+totalCalls into single 64-bit add
- `4da43536` stp-fold tempBase+ip stores
- `9240027f` bcStart cache (JITMethod 96→104 bytes; JM_SIZE in TrampolineAsm.S bumped)
- `e62a7b50` ldp-fold cursor+limit loads

Docs (`docs/deferred.md` A6 iter logs N+21 through N+24):
- `802a1bcb`, `b8a2ec05`, `ec884d43`, `a8f0f4e3`

## Open / next-step menu

1. **Root-cause cond-jump emit bug** so the prim 60/61/62 stub-gate
   in `de84c68e` can be lifted.  Bisect: 3rd cond-jump compile
   (Array>>at:put: in sieve order); `PHARO_ASMJIT_T1_JUMPS_SKIP_N=3`
   cures the symptom.  Suspect: interaction between prim prologue
   fall-through and bytecode-body cond-jump emit.  Not in
   straightforward asm — needs lldb breakpoint at the divergence.

2. **More inline-J2J emit shrinks.**  Diminishing returns; M1 pipeline
   hides most single-instruction savings.  Each shrink also tightens
   i-cache pressure which compounds.  Candidates:
   - Keep sp live across save-push (FAILED 2026-05-20 — actually
     regressed by 1-2ms; the longer live range hurt scheduling).
   - Literals cache (like bcStartCache).  Saves 1 instr in return
     prelude.

3. **Sista Phase 4 monomorphic inlining** (THE architectural fix).
   Multi-session.  See `docs/sista-inlining-plan.md` §"Phase 4".

4. **block-value inline default-on** (currently opt-in via
   `PHARO_T1_INLINE_BLOCK_VALUE=1`).  Helps `inject:into:` /
   `collect:` / `select:` benches.  No regressions observed
   on fib/sieve/eval; risk is broader SUnit workloads.
   Tested clean this session.

5. **inline-J2J xmethod uncap.**  Currently capped at MAX=30000 for
   chain-break safety.  Bench-suite clean at unlimited per docs;
   SUnit workloads still SEGV.  Needs the chain-break protocol
   investigated (deferred A6 iter N+10 onwards) — complex.

## Build + test invocation (post-reboot quick start)

    cmake --build build
    PHARO_BENCH=fib PHARO_FIB_N=28 ./build/test_load_image /tmp/harness/Pharo.image 2>&1 | grep "fib.*run"
    PHARO_BENCH=sieve ./build/test_load_image /tmp/harness/Pharo.image 2>&1 | grep -E "ret=0x|sieve.*run"

## Reboot reason

Remote Control hung in "connecting…" state — couldn't recover via
`/logout` + `/login` from inside the session; user is rebooting to
re-establish.

Branch: `jit` (138 commits ahead of `origin/jit`).  Submodule
`scripts/pharo-headless-test` has uncommitted ref bump from a prior
session — not touched here.
