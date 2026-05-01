# Multi-week JIT work choices

The single-iteration JIT improvements (splice extensions, IC bit-packing,
default-on flag flips, gate refinements) have largely been picked over.
The remaining bench-suite gaps and structural roughness need bigger,
focused efforts.  Each item below is multi-week — too big for one /loop
iteration, but bounded enough that one of them per session is realistic.

Items are roughly ordered by expected impact-per-week.  Each section
notes WHAT, WHY blocked, ESTIMATE, and RELATED memory/docs.

---

## 1. HELPER_SENDS scheduler architecture rework

**What:** the helper-driven `step()` loop in `jitSistaCallSend` runs
nested interpretation under `inSyncSend_=true` to suppress process
switches.  This breaks the timer/scheduler invariant during long inner
sends — the Delay scheduler doesn't re-arm and the system idles to
death.  A clean fix needs either:

- (a) re-architect the helper to NOT drive `step()` directly — instead,
  schedule the inner activation as a normal frame and yield, with a
  callback marker so the helper continuation runs after the inner
  returns; OR
- (b) allow process switches mid-helper with proper save/restore of
  the helper's frame bookkeeping per-process (each process gets its
  own helper-state stack).

**Why blocked:**  3 single-iteration approaches were tried 2026-05-01
and all caused regressions: per-method auto-on (sum 1M hangs), step()-
budget bail (startup process termination), periodic-check pacing
(170× bench panel regression).  See
`memory/project_b1_helpersends_2026_05_01.md`.  The intrinsic issue is
that `inSyncSend_` is a global boolean — fine for a single helper-send
but breaks under recursion / process switches.

**Payoff:** unblocks default-on `PHARO_SISTA_HELPER_SENDS=1`.
Current opt-in gate (commit `ee3daf70`) gets 7/10 bench-suite
stability with `sum 1M` 100ms → 1ms (98×).  Default-on stable
would expand splice firing to all user methods with do:/inject:/collect:
patterns and prior setup-sends.

**Estimate:**  1-2 weeks for option (a), 2-3 weeks for option (b).
Option (a) is cleaner but requires touching `tryJITActivation`,
`pushFrameForJIT`, and the J2J save chain.  Option (b) bolts onto
existing helper but needs careful save/restore design.

**Related:** `memory/project_b1_helpersends_2026_05_01.md`,
`memory/project_helper_sends_gate.md`, `docs/deferred.md` §B7.

---

## 2. Phase 6 block inlining

**What:** inline block bodies into the caller's compiled code at hot
sites.  Cog does this after PIC stabilizes — the per-iter `aBlock value`
becomes a direct branch into the inlined body, eliminating closure
allocation, captured-temp-vector dispatch, and method-map lookup.

**Why blocked:**  this is the structural fix for the bench-suite block-
dispatch gap.  `1M getter+yourself` 99ms vs Cog ~3ms (33×) and
`1M blocks` 14ms vs ~1ms (14×) are dominated by per-iter block
dispatch.  Sista's existing block handling is "lift block IR into
inlinedBlocks slot, splice intercepts at PushFullBlock+Send".  That
covers the splice family but not standalone `value`/`value:` sends to
captured blocks (which is what `timesRepeat:`-style call patterns
produce after Pharo's bytecode inlining).

**Payoff:**  closes `1M getter+yourself` and `1M blocks` gaps.
Probably also helps `sort 100K` (block comparator in sort) and
`dict 50K` (block in `at:put:` resolution).

**Estimate:**  4-6 weeks.  Needs IR-level support for block
specialization at IC sites (recognize monomorphic block from IC
profile, inline the body, deopt-on-mismatch), and the lowering needs
to handle nested IR with deopt-stack reconstruction across the inlined
boundary.

**Related:** `memory/project_cog_gap_2026_04_29.md` ("Block dispatch
dominates — Phase 6 is the big lever"),
`memory/project_phase6_investigation_2026_05_01.md`,
`memory/project_sista_skips_blocks.md`.

---

## 3. Sista deopt-with-resume

**What:** today, Sista deopt is "rebuild interp stack, jump to source
bytecode, re-run from scratch."  For loops, this means re-iterating
already-completed iterations.  Resume means: pause Sista mid-iteration,
hand control to interp at the EXACT next bytecode, with the iteration
counter and accumulator in interp-visible form.

**Why blocked:**  currently the splice family deopts conservatively to
the OUTER receiver expression (e.g., `to:` for IV-do, PushFullBlock for
do-accum).  This is correct but wasteful when an inner deopt happens
on iteration N of a million.  A real resume needs:

- per-iteration framepoint (currently we have one framepoint per splice op)
- machinery to write iteration counter + accumulator back to interp
  temps mid-loop
- interp-side resume that picks up at "the next iteration" rather than
  "from the top"

**Payoff:**  splices that currently rejection-cache or refuse to fire
because of complex block bodies could fire with a deopt-safety net
that survives mid-iteration unwind.  Specifically unblocks blocks
that contain conditional sends (e.g. `[:e | e foo ifTrue: [...] ifFalse: [...]]`)
where today the splice rejects "multi-block IR".

**Estimate:**  3-4 weeks.  Per-iter framepoint is the bulk of work.
Interp-side resume is a smaller patch.

**Related:** `memory/project_b1_helpersends_2026_05_01.md` (mentions as
alternative to HELPER_SENDS), `docs/deferred.md` §B8.

---

## 4. Sista pre-pass for inlined whileTrue: counter loops

**What:** Pharo's bytecode compiler inlines `n timesRepeat: [block]` and
`(start to: stop) do: [block]` (when start/stop are literal SmallInts)
as `whileTrue:` counter loops at AST-emit time.  No Send1 #timesRepeat:
or Send1 #to: bytecode is emitted — the loop body and increment are
inline bytecodes.  Sista's existing splice family (do:, inject:into:,
collect:, IV-do, IV-do-accum, IV-inject) can't intercept these
because there's no PushFullBlock+Send pair to hook.  A new pre-pass
must recognize the inlined whileTrue: pattern at the BYTECODE level
and emit a counted-loop IR op.

**Why blocked:**  attempted 2026-05-01 as a `kCountedLoopTimesRepeatConst`
splice — pre-pass scanned for Send1 #timesRepeat: which doesn't appear
in the bench bytecode.  The 400-line splice was reverted (see
`memory/feedback_pharo_inlines_timesrepeat.md`).  A real fix recognizes
the inlined pattern: `StoreTemp counter, [PushTemp counter, Push limit,
SpecialSend <=, jumpFalse end, BLOCK_BODY, PushTemp counter, Push 1,
Add, StoreTemp counter, jumpBack head, end:]`.  This is a more complex
pattern match than the splice family does.

**Payoff:**  `1M blocks` benchmark (currently 14ms) — the body is
`[counter := counter + 1]` which compiles to a closure-vec-stored
counter.  Recognizing the loop and lifting `counter` to a register
gives the same speed as the panel's `simpleLoop` (7ms).  Smaller win
than other items but bounded.

**Estimate:**  1-2 weeks.  Pattern recognition is straightforward
since the inlined shape is rigid.  The trickier part is closure-vec
register promotion (hoist load before loop, store after) under deopt
constraints.

**Related:** `memory/feedback_pharo_inlines_timesrepeat.md`.

---

## 5. J2J-only callee recompile triggering

**What:** methods that are only called via the J2J fast path
(`stencil_sendJ2J`'s direct-call site) bypass `tryExecute` and
`tryJITActivation`, so their `executionCount` never bumps and OSR-
recompile never fires.  This means tier=1 methods stay unspecialized
forever.  IC sites in those methods (e.g., `obj size`, `obj yourself`)
never get specialized to inline-getter / inline-returnsSelf even when
they're consistently monomorphic.

**Why blocked:**  3 attempts 2026-04-30 and 2026-05-01 reverted because
caller-bump in J2J broke the splice race (sumArr 7ms → 1037ms in panel
run 2/4).  See `memory/feedback_caller_bump_breaks_splice.md`.  The
issue: bumping the caller's count can trigger recompile of a method
that's currently splice-active, and the recompile races with the
running splice.

**Payoff:**  every block body in the system gets IC specialization.
Block-heavy benches (1M getter+yourself, sort, dict) all benefit.
Probably 30-50% improvement on block-heavy real workloads.

**Estimate:**  2-3 weeks.  Requires either:

- a callee-bump variant that mirrors `noteMethodEntry`'s `hasSplice`
  gate without the splice race; OR
- IC-patch-at-callsite (when a J2J site sees consistent class, patch
  the IC to use specialized stencil even without recompile of the
  callee).

The IC-patch approach is cleaner but needs new IC bits.  Callee-bump
is simpler but the gate has been tricky to get right.

**Related:** `memory/project_jit_recompile_gap.md`,
`memory/project_j2j_bump_2026_05_01.md`,
`memory/feedback_caller_bump_breaks_splice.md`.

---

## 6. Class-based HELPER_SENDS gate

**What:** the current HELPER_SENDS per-method gate (commit `ee3daf70`)
narrows by `methodIsShort` (len_<100) and `hasSpliceCandidate`.  This
catches user bench methods but also catches some short UI methods
whose blocks contain sends that get spliced incorrectly (e.g., DNU on
#isTransparent in `WorldState>>drawWorld:submorphs:invalidAreasOn:`
when its splice fires).  A class-name gate would skip UI/system
classes (Morph, FormCanvas, WorldState, SpWindow, FileReference,
SnapshotOperation, Process, etc.) entirely.

**Why blocked:**  the LinearLifter today has access to `memory_` and
the bytecodes but NOT to the method oop.  Plumbing the class name
requires touching `Builder::build()` to pass class info, then storing
it on the lifter.  Doable but invasive (changes the public API of
Builder).

**Payoff:**  bumps HELPER_SENDS bench-suite stability from 7/10
(current) to ~10/10 (matching default-flag baseline) by skipping the
UI cascades that surface DNU on #isTransparent.

**Estimate:**  1 week.  Mostly plumbing.  The list of skip classes
needs validation against full bench-suite + Cog's own code (some
exception infrastructure may also benefit from skip).

**Related:** `memory/project_helper_sends_gate.md`.

---

## 7. T1/T2 architectural interaction

**What:** when T2 (the optimizing tier) intercepts a method that T1
already compiled, T1's inline-IC warmup is broken.  Coexist mode
(default since §1.3c) sidesteps this by not replacing T1, but T2
becomes effectively dormant — it compiles but doesn't intercept.

**Why blocked:**  T2 has never demonstrably won on any measured bench.
The IC-warmup issue is the immediate blocker: replacing T1 with T2
mid-execution loses the populated IC state, which is where 80%+ of
T1's performance comes from.

**Payoff:**  potentially significant on workloads where T2's
optimizations beat T1's inline ICs (e.g., loops with heavy arith on
small SmI, or long-running monomorphic chains).  But no such workload
has been measured yet.

**Estimate:**  3-4 weeks of design + impl, plus the open question of
whether the wins exist.  Either:

- shared IC table across tiers (T1 and T2 both read/write the same
  IC entries); OR
- patch-T1-when-T2-compiles (T2's specialization gets folded into the
  T1 IC).

**Related:** `docs/deferred.md` §E.1.

---

## 8. Per-bytecode Sista hook

**What:** today Sista compiles whole methods.  Per-bytecode Sista would
let Sista take over a method mid-execution at any backward jump — a
form of OSR but at the IR level.  Combined with item #3 (deopt-with-
resume), this would let Sista specialize hot loops in methods it
hasn't fully analyzed.

**Why blocked:**  Sista's current cache is per-method.  Per-bytecode
needs a per-bcOffset cache, and the entry shape changes (state at
bytecode N vs at method entry).  Also requires the lowering to support
entering at arbitrary IR points, which is a major restructure of the
prologue.

**Payoff:**  unblocks the bench-suite one-shot problem — methods that
are called once but iterate millions of times (runSum, runFibonacci's
benchFib, etc.) could be Sista-specialized at the loop entry rather
than waiting for full-method compilation thresholds.

**Estimate:**  6-8 weeks.  The lowering rework alone is significant.

**Related:** `memory/project_specialization_misses_doit.md`,
`memory/project_eval_fib_gap.md`.

---

## How to choose

If picking ONE for a focused multi-week session:

- **Biggest payoff:** #2 (Phase 6 block inlining).  Closes the largest
  bench-suite gaps simultaneously.  But also the most work.

- **Best ratio:** #5 (J2J-only callee recompile).  2-3 weeks for
  ~30-50% on block-heavy workloads.  Past attempts hit specific bugs
  that have known fix shapes.

- **Smallest:** #6 (class-based HELPER_SENDS gate).  1 week to ship,
  immediate stability win.  Good warm-up before tackling #2 or #5.

- **Speculative:** #7 (T1/T2 interaction).  Don't start unless a
  measured workload shows T2 beating T1 — currently none does.

Items #1, #3, #4, #8 are alternative paths to similar outcomes.  #1
unblocks default-on of an existing opt-in.  #3 unblocks more splices.
#4 catches a specific Pharo idiom.  #8 is the cleanest long-term
direction but the highest cost.
