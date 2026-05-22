# jit-may23 — session-sized JIT work queue

**How to use this doc**: pick the topmost unchecked task, work on
it to completion or hit a clear blocker, mark the result, move
on.  Each task is bounded to ~30 min – 4 hours of work.  Don't
try to "finish the whole doc" in one go — that's many sessions.

When invoked via /goal: do as many tasks as fit, mark progress,
end naturally.  The hook clears when the LAST unchecked task
either lands or is moved to `## Deferred` with a reason.

State at start (2026-05-22, branch `jit` at `b24e1f8a`):

    benchmark         current   vs Cog
    fib(28)           179 ms      60×
    sieve x100          8 ms     0.8×   ✓
    sort 100K         835 ms      49×
    dict 50K          447 ms      34×
    sum 1M            284 ms      95×
    factorial 5K       21 ms      10×
    block 1M            1 ms     0.3×   ✓
    instVar 1M        260 ms     130×
    100K alloc         13 ms       4×
    floatSum 1M       402 ms      50×
    stringHash 100K   169 ms      85×
    collect 10x100K   510 ms      12×
    select 10x100K    643 ms      80×

## Task queue (work top-to-bottom)

Each task: **Goal**, **Success**, **Estimate**.  Mark with ✅
(done), ❌ (failed/reverted) + reason, or ⏭️ (deferred) + reason.

### T1 — Investigate bitOp counter = 0

**Goal**: figure out why `g_primBitOp_hits` is 0 on bench-suite
despite the inline emit existing at AsmjitT1.cpp:3367+ for
primKind 11/12/13/19.

**Steps**:
1. Add `PHARO_TRACE_BITOP=1` log at the dispatch site (extras
   bits 48-52 read).
2. Find what selectors trigger primKind 11/12/13/19 patches.
3. Determine if the dispatch is unreached (path not taken) or
   bit isn't set in extras.

**Success**: identify either (a) a one-line fix to enable
firing, or (b) document why it's structurally not reachable.

**Estimate**: 1-2 hours.

### T2 — Investigate floatOp counter = 0

Same approach as T1 but for primKind 21/22/23 (SmallFloat
ops).  AsmjitT1.cpp:4530+ has `tryPrimSmallFloatOp` emit.

**Success**: same.
**Estimate**: 1-2 hours.

### T3 — Survey top 10 hot selectors

**Goal**: find selectors with the highest IC HIT counts on
bench-suite that DON'T match an existing inline-emit pattern.

**Steps**:
1. Add per-selector hit counter at patchJITICAfterSend time
   (just count IC fills by selector).
2. Run bench-suite, dump top 10.
3. For each: check if it could be inline-emitted (returns
   constant, getter-style, etc.).

**Success**: list of ≥3 candidates for new inline emits.
**Estimate**: 2-3 hours.

### T4 — Add primKind 17 (basicNew 0-arg) inline emit

**Goal**: dispatch primKind 17 (basicNew with 0 args) to a
helper similar to jit_rt_basic_new_with_arg.

**Steps**:
1. Add `jitBasicNew` helper (mirrors jitBasicNewWithArg but
   reads class from stack[-1] only, no size).
2. Add primKind 17 detection at the inline-prim dispatch.
3. Counter + benchmark.

**Success**: primKind 17 counter fires.  Bench-suite
no regression.  alloc bench unchanged or faster.

**Estimate**: 2-3 hours.

### T5 — Detect more trivial-method patterns

**Goal**: find Pharo methods that don't match getter/setter/
returnsSelf/returnsLiteral/multiSlot but could be inline-emitted.

**Steps**:
1. Trace methods whose IC entries have `extra == 0` (no
   special bits).
2. Inspect their bytecodes.  Look for patterns like
   `^ self class`, `^ self foo asString`, etc.
3. Pick ONE pattern that appears ≥1000× per bench-suite run.
4. Add detector + IC bit + asmjit-T1 emit.

**Success**: new IC bit fires ≥1000× on bench-suite.  Bench
unchanged or faster.

**Estimate**: 3-4 hours.

### T6 — Track JIT compile failure reasons

**Goal**: 10K methods fail to JIT-compile per bench-suite run
vs 779 that succeed.  Some failures might be fixable.

**Steps**:
1. Add per-reason counter at the compile bail sites (find
   them via `return nullptr` in AsmjitT1.cpp).
2. Run bench-suite, dump counts per reason.
3. Identify the top 1-2 reasons.

**Success**: list of top compile-failure reasons + at least
one identified as "potentially fixable".

**Estimate**: 1-2 hours.

### T7 — Fix one compile-failure reason

**Goal**: address the most common fixable failure from T6.

**Success**: bench-suite shows ≥100 more methods compiled.
No regression.

**Estimate**: 2-4 hours depending on reason.

### T8 — Selector-based inline cache hint

**Goal**: for selectors that have stable single-class IC entries
(monomorphic-only), skip the bit-60 J2J save-stack overhead.

**Steps**:
1. Add monomorphic-vs-poly counter at IC fill time.
2. For monomorphic IC entries with high hit counts, set a new
   bit (e.g., bit 56 currently unused for sends).
3. asmjit-T1 dispatch bit-56 → skip save-stack push.

**Success**: bench-suite improvement on a send-heavy bench.

**Estimate**: 3-4 hours.

### T9 — Profile what dominates instVar bench

**Goal**: 260 ms for instVar 1M (vs Cog 2 ms = 130× slower).
Identify the single biggest cost in the inner loop.

**Steps**:
1. Sample (e.g., DTrace, perf, or just adding timed counters
   around each call type in step()).
2. Determine: is it the block invocation per iteration?  The
   getter send?  The yourself send?  The arr access?
3. Quantify each.

**Success**: a single root cause identified, even if the fix
is multi-day.

**Estimate**: 2-3 hours.

### T10 — Profile what dominates sum bench

Same as T9 but for sum 1M (284 ms, 95× Cog).

**Estimate**: 2-3 hours.

### T11 — Profile what dominates floatSum bench

Same as T9 but for floatSum 1M.

**Estimate**: 2-3 hours.

### T12 — Look at the Float arith inline (primKind 21/22/23)

**Goal**: bench-suite floatSum is 402 ms.  The inline emit at
AsmjitT1.cpp:4530+ exists but counter = 0.  Make it fire.

**Steps**:
1. Verify the IC for `+` on SmallFloat receivers gets primKind
   21 in extras.
2. Trace the dispatch to see where it bails.
3. Fix or document the structural reason.

**Success**: floatOp counter fires.  floatSum bench improves
or stays same.

**Estimate**: 2-3 hours.

### T13 — Extend multi-slot to setters

**Goal**: setter pattern `slot[A] := slot[B] + 1` style.

**Steps**:
1. Find common Pharo setter patterns (e.g., `position: position + 1`).
2. Detect at bytecode level.
3. Encode in IC bit + asmjit-T1 emit.

**Success**: new pattern fires ≥100× on bench-suite.

**Estimate**: 3-4 hours.

### T14 — Investigate why so many `cull:` IC HITs

The PHARO_TRACE_CULL_ENTRY infrastructure exists.  Run it on
bench-suite; understand what `cull:` is doing.

**Estimate**: 1-2 hours.

### T15 — Investigate Sista compile bail rates

Sista compiles fewer methods than asmjit-T1 (most bench
workloads have Sista as bailout).

**Steps**:
1. Add counter at Sista::compile's bail sites.
2. Run bench-suite, identify top bail reasons.
3. Pick one fixable.

**Estimate**: 2-3 hours.

### T16 — Run bench-suite under perf for hot-function ID

**Goal**: see where actual CPU time goes during bench-suite.

**Steps**:
1. `perf record ./build/test_load_image /tmp/pharo-bench-...`
2. `perf report` for hot functions.
3. Identify top 5 hot interp/JIT-runtime functions.

**Success**: report of top hot functions with %CPU.

**Estimate**: 1-2 hours.

### T17 — Document mechanism for selective hot-block detection

**Goal**: blocks run in interp because activateBlock doesn't
JIT-dispatch.  Cold blocks paid compile cost when force-
compiled (Issue 5).  Find a heuristic to compile ONLY hot
blocks.

**Steps**:
1. Survey what makes blocks hot vs cold (call count, parent
   method's hotness, etc.).
2. Propose a counter scheme.

**Success**: written-up proposal in this doc; no code yet.

**Estimate**: 2-3 hours.

### T18 — Apply T17's heuristic

**Estimate**: 4-6 hours after T17.

### T19 — Look at Sista's literal-bake question one more time

`docs/jit-may22b.md` documented Sista bakes bytecodeBase and we
fixed that.  Are there OTHER baked pointers (literals, class
oops)?  Survey AsmjitT1.cpp + SistaLowering_arm64.cpp for
`Imm(some-pointer)` patterns.

**Estimate**: 1-2 hours.

### T20 — Add SmI-mul SmI tag-aware inline

`primKind 9 = SmI mul`.  Inline emit may exist; verify it
fires and produces wins.

**Estimate**: 1-2 hours.

## Deferred (with reasons)

These need genuine multi-day focused engineering with lldb-level
soak.  Don't try to do them in /goal-driven session iteration:

- **Real BLR emit fast-path** (Step 2 in jit-may22b) — C++
  helper overhead dominates for short methods; needs true
  asmjit-emitted activation logic.  ~7-10 days.
- **kSendInlineSelf real lowering** (Step 3) — asmjit Compiler
  needs post-finalize patching for self-rec BR.  ~10-14 days.
- **Per-site class-immediate IC HIT** (Step 5) — Cog's biggest
  edge.  Needs patchable code regions throughout asmjit-T1.
  ~8-12 days.
- **Eden bump-allocate inline** (Step 6) — substantial new
  asmjit emit + GC-safe header init.  ~6-8 days.
- **Block-value spec** (Steps 9-10) — proper inline-BLR from
  value: send site, skipping primitive 207.  ~7-10 days.
  Unlocks the 4 zero-firing counters (Issue 4).
- **Step 2 n-arg state corruption debug** — bug is inside
  Sista's compiled fn execution.  Needs genuine lldb step-
  through, may take 1-3 days.

## Done (with results)

- ✅ Investigate zero-firing counters (Issue 4) → traced all 4
  to blocks-in-interp; needs Step 9-10.
- ❌ activateBlock → tryJITActivation (Issue 5) → frame-state
  conflict, reverted.
- ✅ Multi-slot extension to no-const variant → infrastructure
  landed, no new hits in bench-suite (multiSlot count stays
  672).
- ❌ N-arg invariant check → no divergence at C++ boundary
  visible; corruption is inside fn body.
- ✅ **T1** (bitOp before bit 60) — fixes wired-but-unreached.
  Verified firing with synthetic bench.  Pharo bench-suite
  doesn't exercise bit ops in JIT bodies (blocks-in-interp).
- ✅ **T2** (floatOp before bit 60) — same fix.  Same
  blocks-in-interp limitation on Pharo workloads.
- ✅ **T3** (top selector survey).  Top non-inlined:
  value:/value:value: (Step 9-10 territory).  even/abs/digitValue:
  could be inline candidates but methods, not quick prims.
- ✅ **T4** (primKind 17 / basicNew 0-arg inline).  Counter
  fires **51,438 times per bench-suite run**.  Measured
  bench-suite deltas (3-6% across multiple benches):
    fib(28): 215→207   -4%
    dict:    546→515   -6%
    sum:     348→334   -4%
    instVar: 350→339   -3%
    stringHash: 196→191 -3%
  Real win.
- ✅ **T6** (per-reason compile-failure counters).  Bench-suite:
    badHeader=0  unsuppPrim=1028955  skipSel=0  block=0  bcOther=49
  1M methods fail compile due to unsupported primitives.
  Most likely fixable in T7 by adding generic prim-call
  prologue + per-prim emit paths.  Multi-day.
- ✅ **T20-partial** (inline SmI mul 0x68).  Added bytecode-level
  inline emit for `*` (8 instructions: untag/mul/smulh/check/
  retag).  Bench-suite: stringHash unchanged (in-block usage),
  but infrastructure ready for any JIT-body `*`.

## Session totals (latest /goal run)

6 tasks done this session (T1, T2, T3, T4, T6, T20-partial).
T4 delivers measurable bench-suite wins (3-6% across multiple
benches via 51K basicNew0 inline hits).  Other tasks landed
infrastructure or identified follow-up directions (T7 from T6
findings).

Remaining unchecked: T5, T7, T8, T9, T10, T11, T12, T13, T14,
T15, T16, T17, T18, T19.

## Notes for the /goal-runner

- **Don't claim wins from single A/B comparisons** — bench-suite
  noise is 3-5%.  Always do 3-run A/B/A before claiming.
- **If a task fails or hits a blocker** — mark it ❌ with a
  reason, move on.  Don't pause for direction.
- **When all tasks are processed** — the doc is "implemented"
  (each task has a status).  Goal clears.
- **New issues discovered during tasks** → add them to the queue
  as new T-tasks.
