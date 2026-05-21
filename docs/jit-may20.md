# JIT plan — post pure-J2J gate (2026-05-21)

Below are proposed fixes for JIT performance.
If one of the fixes when implemented doesnt help, leave code in but disabled.

## Where we are

**Correctness restored** (commit `ca2c54c3`). Pure-J2J gate prevents
inline-J2J from firing when any IC site is cold, which sidesteps the
materialize-bail wrong-result bug.

**Honest perf baseline** — measured under correctness, no env vars:

    fib(20)  ~ correct           21,891
    fib(28)  ~ 170 ms            1,028,457
    fib(30)  ~ 495 ms            2,692,537

The session's earlier "fib(28) = 11 ms" numbers were on **wrong
results**. Treat iter N+22..N+30 perf claims as void until re-measured.

**Cog reference** (on the same M1, fib(28)): ~3 ms. So we're 57× off.

## Why the gate is currently a no-op for fib

The gate checks each IC site's `extras & (1ULL << 60)`. After warmup
that bit IS set. But `JITRuntime.cpp:3392` memsets the entire icBuffer
(all 6 entries × 3 slots = 144 bytes per site) on every GC. GC fires
many times during fib's tight loop, so bit 60 is cleared, gate bails,
inline-J2J never fires.

So the gate is shipped but does nothing useful on fib — perf is the
same as `PHARO_T1_NO_INLINE_J2J=1`.

## Sequenced plan

### 1. Preserve bit 60 across GC  *(quick win, days — SHIPPED 2026-05-20)*

**Status: shipped, semantically correct, but does NOT unlock the fib
fast path. Investigation revealed the original diagnosis was incomplete.**

Implementation (`JITRuntime.cpp:recoverAfterGC`): per-entry loop that
zeroes key/method/selectorBits/hitCount but preserves the upper 16
bits of `extras` (bits 48–63: J2J_ENTRY_BIT, BLOCK_VALUE_BIT,
RETURNS_LITERAL, MULTI_SLOT, SELF_REC_BIT, primKind).
`static_assert((1ULL << 60) & 0xFFFF000000000000ULL)` keeps the
mask honest.

**Measured impact on fib (default settings, 3-run median):**

    bench         baseline (no fix)    with Step-1 fix
    fib(28)         161 ms              161 ms     (no change)
    fib(30)         346 ms              347 ms     (no change)

Why no benefit, despite preserving bit 60: the AsmjitT1 pure-J2J gate
already wasn't bailing because of GC. With the *gate disabled*
(`PHARO_T1_NO_PURE_J2J_GATE=1`), single-shot fib(20/28/30) measures
30/30/36 ms with CORRECT results — a 5× speedup with no observed
correctness regression. So the gate is the bottleneck, not GC clearing
of bit 60.

Step 1 ships anyway: the selective memset is the right hygiene
regardless (preserves static markers that survive the recompile cycle
correctly), and removes a class of post-GC false-negative on the gate.

The "fib(28) ~ 11 ms" prediction was based on the deferred A6 N+30k
note conflating "gate always bails" with "GC clears bit 60." The real
story: even with bit 60 preserved, the gate may be bailing for
other reasons (e.g., interaction with prim-call IC sites, runtime
caller/callee asymmetry). Diagnosed root cause moves to Step 2.

### 2. Fix the underlying materialize-bail wrong-result bug  *(weeks)*

Pure-J2J gate is a safety net. Even with it, any future workload that
hits cold ICs mid-flight will bail through the broken materialize path.
Three options (recap from iter N+30j):

- **Pure-J2J static gate** — at compile time, scan callee bytecode and
  refuse to emit inline-J2J if any send opcode can target a cold IC.
  Stricter than the runtime gate; eliminates the bail scenario at
  compile time. Doesn't allow the perf benefit either.
- **Recursive-safe materialize** — fix the materialize-unwind so the
  chain-loop callee's eventual retVal lands at the caller's receiver
  slot. Requires adding a `savedReceiverSlot*` to the OUTER SavedFrame
  pushed at `Interpreter.cpp:21147`, and making `popFrame` write the
  pushed value there for materialized frames (vs. the current `FP[0]`
  semantics). Complex because `popFrame` is shared with normal returns.
- **No-bail design** — change inline-J2J to never bail. At PUSH time,
  if the callee has any send whose IC isn't promoted, fall back to
  chain-loop *before* pushing. Same as a stricter runtime gate but
  scoped to per-callee instead of per-method.

Recommendation: **start with no-bail design**. The runtime gate already
proves the technique works; tightening it to a per-callee check is a
small extension and fully unlocks inline-J2J perf when ICs are warm.

### 3. Re-measure the 28 session perf commits  *(days)*

Every shrink committed in iter N+22..N+29 was measured on wrong
results. Some shrinks are genuinely correct optimizations (post-index
stp folds, x19 hoist, etc.), but the *measured* impact is meaningless
without correctness.

Solutions to use:

- **A/B harness**. Add a `scripts/bench-correctness.sh` that runs each
  benchmark with and without inline-J2J, asserts the output matches,
  then reports the timing.
- **Bisect with the harness**. For each shrink commit, run the harness,
  verify correctness, record perf. Drop commits that no longer pull
  weight; keep the ones that do.

### 4. Sista phase 4 monomorphic inlining  *(multi-week)*

From iter N+29: Sista's `activateMethod` hook is bypassed by both T1's
inline-J2J and chain-loop's J2JCall. For fib's 317K activations, only
the outer call reaches activateMethod. To make Phase 4 inlining help:

- **Sista lookup in T1's IC HIT emit** — when IC patcher sees a method
  has a Sista fn, set a `SISTA_BIT` in extras; emit `blr` to Sista's
  fn. Blocked on Sista's bail-protocol correctness (iter N+29 found
  Sista compiles benchFib wrong when forced via
  `PHARO_SISTA_COMPILE_BAIL_ONLY=1`).
- **Sista lookup in chain-loop's J2JCall handler** — same correctness
  blocker.
- **Generalize Sista's recognizer** — currently shape-recognizers only
  handle trivial getter/setter/arith-on-ivar patterns. Generalizing
  needs Phase 3 deopt infrastructure first.

Recommendation: **fix Sista bail correctness first** (single issue at
`SistaRuntime.cpp:226` gate), then option 1.

### 5. Identify and optimize other hot loops  *(open-ended)*

Beyond fib, the bench-suite has other benchmarks (sieve, sort, dict,
sum, factorial, etc.). Once correctness is solid:

Solutions to use:

- **Profile each benchmark**. `PHARO_PRIM_PROFILE=1` shows primitive
  hot spots. Look for sends that miss IC, prims that bail, etc.
- **Pattern-match to existing infra**. Sieve uses `bitAt:put:` heavily
  (already inline-prim'd). Sort uses comparator blocks (could benefit
  from block-value specialization). Dict uses hash sends.
- **Target the bottom of the gap**. fib's 57× gap is the most dramatic.
  Other benches may have 1.5–3× gaps that are easier wins.

## Dependencies and ordering

1. → 2.a → 3. → 4 → 5. (linear).

Steps 1 and 2.a can ship independently. Step 3 (re-measure) gates any
new perf claim. Step 4 is multi-week. Step 5 is the long tail.

## Out of scope for this plan

- iOS/Catalyst GUI work (CLAUDE.md domain, not JIT).
- Sista x86 lowering (paused per iter N+29; arm64 is the platform).
- Image-side changes (Pharo upstream).

## How to validate any future "perf win"

The session's lesson: any inline-J2J change can silently produce wrong
results that look fast. Before claiming a perf win, run the harness:

    scripts/bench-correctness.sh            # default: fib 20 28 30, asserts each
    scripts/bench-correctness.sh fib 28     # one bench
    scripts/bench-correctness.sh --ab fib 20  # also re-run with NO_INLINE_J2J=1

Default mode asserts against a known-good table (21891/1028457/2692537
for fib 20/28/30) and prints timing. `--ab` runs the second mode and
also asserts the two values agree. Be aware: `PHARO_T1_NO_INLINE_J2J=1`
currently runs pathologically slow on fib (even fib 20 doesn't complete
in 180 s) — separate from this plan, but documented here so reviewers
don't spend time on it.

A perf claim that doesn't include the assert is void.
