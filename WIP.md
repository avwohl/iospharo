# WIP — JIT perf session (2026-05-20, post-reboot iter)

## Session progress 2026-05-20 post-reboot (7 commits added)

Cumulative measured perf gains (M1, PHARO_BENCH=fib, best-of-5):

    bench    pre-session    post     cog    gap     delta
    fib(28)  13.0 ms        11.7 ms  3 ms   3.9×    -10%
    fib(30)  35.7 ms        31.5 ms  ~8 ms  3.9×    -12%
    fib(32)  86 ms          82 ms    —      —       -5%
    sieve    2.3 ms         2.3 ms   ~1 ms  2.3×    flat

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
