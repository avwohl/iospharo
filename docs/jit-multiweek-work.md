when doing items in this list, take as long as you need to per item
when doing  /loop sleep the minimum 1min before picking the next item
 but keep per item time as large as needed for these items. Do not assume
 1min max per item because /loop sleep is 1min.
pick items in any order you think best
if you are stuck on something that needs a human do something else

# Multi-week JIT work choices

The single-iteration JIT improvements (splice extensions, IC bit-packing,
default-on flag flips, gate refinements) have largely been picked over.
The remaining bench-suite gaps and structural roughness need bigger,
focused efforts.  Each item below is multi-week — too big for one /loop
iteration, but bounded enough that one of them per session is realistic.

Items are roughly ordered by expected impact-per-week.  Each section
notes WHAT, WHY blocked, ESTIMATE, and RELATED memory/docs.

---

## 1. HELPER_SENDS scheduler architecture rework — **DONE 2026-05-02** (`2e274c7d`)

Shipped via three precondition fixes in this session:
- `bd7adb87` Sista deopt-path operand double-stacking fix
- `2a7e2a4e` skip Sista compile for Array-do splice + helper-send methods
- `2e274c7d` flip `PHARO_SISTA_HELPER_SENDS` default-on (opt-out via
  `PHARO_NO_SISTA_HELPER_SENDS=1`)

Result: `1M blocks` drops from 13ms to **0 ms** (math-simplification
splice fires).  10/10 reliable under default flags.  All other benches
within ±2ms variance.

**2026-05-02 follow-up — `2a7e2a4e` workaround is cosmetic only:**
investigated whether the runSum bail+resume corruption can be fixed.
Empirically: with `PHARO_SISTA_ALLOW_ARRAYDO_HELPER=1` (gate disabled),
runSum is the ONLY method that triggers (per `[SISTA-ARRAYDO-HELPER]`
diagnostic — only `PharoBenchmarkRunner class>>#runSum` matches both
Array-do splice + kSendCallHelper).  runSum still produces the correct
result (`sum=500000500000`) and the bench-suite reaches DONE.  The
"do: nil" symptom is a 31-byte file-overwrite at the start of
`/tmp/pharo_benchmarks.txt` from `Smalltalk exitSuccess` shutdown
catching an error elsewhere.  Bench results intact.  Conclusion: the
narrowly-scoped 2a7e2a4e workaround is the right level of fix; further
investigation isn't justified by the cosmetic-only impact.

**Original analysis (kept for context):**



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

**2026-05-02 sub-investigation + partial fix:**

- **Bug 1 (FIXED, commit `bd7adb87`):** kSendCallHelper deopt path was
  pushing operands TWICE.  Pre-helper code pushed v.operands onto
  state->sp.  When helper returned 0 (depth check, step()=false),
  the deopt's framepoint replay pushed values starting at state->sp
  (post-pre-helper-push), leaving operands DOUBLE-STACKED.
  Verified via SISTA-SEND trace: spDelta went 4 → 2 (correct).

- **Bug 2 (REMAINING, runSum-specific):** even with bug 1 fixed, sum
  1M still DNUs with `#do:` on nil receiver in runSum.  Workaround:
  `PHARO_SISTA_EXCLUDE_SELS=runSum` makes the bench-suite COMPLETE
  with HELPER_SENDS=1 + 1M blocks = 0 ms (was 13 ms — the math-
  simplification kCountedLoopWhileTrueAccum splice now fires).  So
  the HELPER_SENDS architecture works in general; runSum specifically
  hits another bug we haven't isolated.  Possibly in how Sista's bail
  for kCountedLoopArrayDoAccum splice (with `asArray` setup pattern)
  interacts with interp resume.

- **Stability under HELPER_SENDS=1 + EXCLUDE runSum (5 runs):** 5/5
  complete with sum 1M ~103 ms (interp), 1M blocks 0 ms, others
  match baseline within ±2 ms.

**Cycle guard status (2026-05-02 commit `3d5b53fa`):** the
materializeFrameStack cycle-break walk is now gated under
HELPER_SENDS=1.  Without helper-sends, the 200-deep walk per
context creation is wasted work.  Doesn't fix the underlying
HELPER_SENDS=1 bug but removes overhead from the default path.

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

**2026-05-02 — refined target.**  `1M blocks` now hits 0ms via
the whileTrue: math splice (item #4), so the remaining bench-suite
gap is `1M getter+yourself`: 16-20ms ours vs Cog 3ms.  Profile of
runInstVar shows the bottleneck is the multi-send body inside the
inlined whileTrue: loop:

    pushTemp 1; send size; pop;       <- multi-slot getter shape
    pushTemp 1; send yourself; pop;   <- universal Object>>yourself
    pushTemp 2; pushOne; +; popIntoTemp 2

The whileTrue: splice rejects multi-send bodies; the math shortcut
isn't applicable when the body has side effects.  The structural
fix here isn't full block inlining at `value` sites (Pharo inlines
timesRepeat:/to:do: at compile time, so there is no `value` send
to intercept).  It's an extension to the whileTrue: splice's body
whitelist to admit `pushTemp obj; sendSel; pop` triplets where the
send is IC-monomorphic + selector inlinable (yourself → no-op,
size → multi-slot getter), with class-guard + deopt-on-miss.

Cleared INLINE_YOURSELF for the universal-no-op case (re-tested
2026-05-02: 5000-factorial regression no longer reproduces — was
likely flaky timing in the original A/B).  But INLINE_YOURSELF
alone moves no needle on `1M getter+yourself` (still 20ms) because
runInstVar's bytecode body executes via interp/T1, never reaching
Sista's send-byte (the whileTrue: splice rejects the method).

**Real next step:** new IR op `kCountedLoopBodyExec` — counted
whileTrue: with body containing K side-effect-bounded sends.
Pre-pass admits when each send has IC hint + matches an inline-able
selector pattern.  Lowering emits a real iteration loop with
inlined body code per iter.  Estimated 1-2 weeks for runInstVar
(2 send sites, both inlinable); generalizing to handle arbitrary
IC-monomorphic sends is full Phase 6.

**2026-05-02 — foundation in tree (`c5389a47` + `98b42bb1`).**
Extended the existing whileTrue: math splice's body recognizer to
admit K leading purely-elidable triplets `pushTemp T; sendByte; pop`
followed by the canonical 4-byte arith — plus a no-accum variant
where the body is ONLY triplets (matches `n timesRepeat: [obj size.
obj yourself]` shape).  yourself triplets need no class guard
(universal Object>>yourself); size triplets emit kLoadTemp +
kGuardClass.  The class for the guard comes from either (a) IC hint
at the size bcOffset, or (b) a compile-time dataflow trace through
the method prologue for `pushLitVar X; SpecialSend new; popIntoTemp
T` — extracts the class from literals[X] (the class binding).  Lets
bench-shape one-shot methods splice without IC warmup.

Default-flags bench-suite is unchanged (5/5 runs).  Recognizer is
gated behind `PHARO_SISTA_INLINE_YOURSELF=1` (opt-in).  Op::
kCountedLoopBodyExec added as scaffolding for the real-iteration
variant (currently unused).

**Verified:** for runInstVar, the recognizer + dataflow correctly
identifies the pattern (preLoop=6 triplets=2 bodyTemp=1
guardCls=OrderedCollection).  Pre-pass logs:
`[SISTA-WHILETRUE-CAND] preLoop=6 endPop=28 accumT=2 loopT=2
limitLit=3 arith=0 const=1 method_len=46`.

**Remaining blocker for the actual runInstVar win:**  Sista's lift
terminates at byte 1 (`SpecialSend new` in the prologue) before
reaching preLoopStart=6.  SpecialSend (0x70-0x7F) always emits
kSendUnspeculated and returns kOk — the helper-sends path that lets
the lift continue past Send0/1/2 doesn't apply.  Next step: extend
the SpecialSend lift to emit a `kSendCallHelper`-style continuation
when HELPER_SENDS=1.  The selector resolves at compile time via
SpecialSelectorsArray (slot `(ssIdx + 16) * 2` is the selector Oop);
that raw Oop can be baked into the IR or routed through a new
kSendCallHelperSpecial variant.

Estimated 1-2 days for the SpecialSend extension.  Once that lands,
the runInstVar splice should fire and 1M getter+yourself should
collapse to math-simplification time (sub-millisecond).

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

## 4. Sista pre-pass for inlined whileTrue: counter loops — **DONE 2026-05-01**

Shipped via commits `3b9d5f08`, `3cb1ced0`, `51f57d32`, `5448b564`.

`Op::kCountedLoopWhileTrueAccum` IR op, byte-pattern pre-pass that
recognizes `<pushLitConst LIMIT, pushOne, popIntoTemp count,
pushTemp count, pushLitConst LIMIT, send <=, jumpFalse, BODY,
pushTemp count, pushOne, send +, popIntoTemp count, jumpTo, pop>`,
lifter intercept that emits the IR op and skips the entire loop
bytecode range, lowering does math simplification:
`accum += (LIMIT - countInit + 1) * const` with overflow check.

Body must be the canonical 4-byte arith shape (`pushTemp X, pushOne,
ArithBase Y, popIntoTemp X`) on a temp distinct from the loop counter.
Multi-send bodies (1M getter+yourself's `[obj size. obj yourself]`)
are detected but rejected — those need a different lowering or a
combination with item #2 (Phase 6 block inlining).

Result on bench-suite: 1M blocks 16ms → 0ms (math simplification turns
1M iterations into 1 multiply + 1 add).  Requires PHARO_SISTA_WHILETRUE=1
opt-in plus HELPER_SENDS=1 (because runBlock's `Time
millisecondClockValue` setup-send terminates the lift without it) and
the class-based HELPER_SENDS gate from item #6 (otherwise UI cascade
DNU breaks the bench-suite before runBlock runs).

---

## 5. J2J-only callee recompile triggering — **DONE 2026-05-02** (`9572b019`)

Past three attempts hit the splice race; the working design is bump-only
in the inline path + dual splice gate + safe-point recompile drain.
`benchFib` now recompiles (was stuck tier=1 forever).  fib(28) -6%,
sieve -4-6%, others within variance.  `memory/project_j2j_inline_bump_drain_2026_05_02.md`.



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

## 6. Class-based HELPER_SENDS gate — **DONE 2026-05-01**

Shipped via commit `5448b564`.

Builder::build derives the method's defining class name from the last
literal (Pharo CompiledMethod convention: last lit is the class
binding, slot 1 = the class itself; `memory.nameOfClass()` resolves
the name string).  LinearLifter stores it on `methodClassName_` and
the HELPER_SENDS emission gate at kSendCallHelper checks
`sistaClassIsHelperSafe(className)` which rejects classes whose name
starts with: World, Form, Morph, Sp, Snapshot, Session, Process,
Semaphore, Delay, Exception, Error, FileReference, FileSystem,
DiskFile, File, Source, Pharo (with PharoBenchmarkRunner allowed-list
override).

Empirical ordering: started with Collection/Array/etc. all skipped
(too restrictive — sum 1M errored "do: receiver is nil"), narrowed to
just UI/system (above list) and bench-suite returned to 8/10 stability
with both 1M blocks and sum 1M dropping to 0ms.

Future widening (if needed): selector-class allow-list rather than
class-prefix skip; or a runtime feedback loop that disables
HELPER_SENDS for classes that produced DNUs in the past N runs.

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
  remaining bench-suite gap (after #9: 1M getter+yourself 16-19ms vs
  Cog 3ms).  Also the most work.

- **Speculative:** #7 (T1/T2 interaction).  Don't start unless a
  measured workload shows T2 beating T1 — currently none does.

Items #4, #5, #6, #9 shipped 2026-05-01..02 (commits `5448b564`,
`d5332e48`, `9572b019`).  Items #1, #3, #8 are alternative paths to
similar outcomes.  #1 unblocks default-on of an existing opt-in.
#3 unblocks more splices.  #8 is the cleanest long-term direction
but the highest cost.

**Bench-suite snapshot (2026-05-02, post-session, best-of-20):**

```
                  Ours    Cog    Ratio
fib(28) ms        15      6      2.5×
sieve x100 ms     44      ?      ?
sort 100K ms      210     60     3.5×
dict 50K ms       155     50     3×
sum 1M ms         99      5      20×    ← needs HELPER_SENDS / Phase 6
factorial 5K ms   21      27     0.78× (we win)
1M blocks ms      12      1      12×    ← needs Phase 6
1M getter ms      16      3      5.3×   ← was 33×, now 5.3× after multi-slot
100K alloc ms     4       ?      ?
```

The `factorial` bench beats Cog already (we use LargeInteger primitives
that are tuned for our VM).  `1M getter+yourself` was a 33× gap and is
now 5.3× after multi-slot (item #9).  `sum 1M`, `1M blocks`, and the
3× gaps on sort/dict/fib all need block-dispatch / HELPER_SENDS work —
multi-week structural items.

---

## 9. Multi-slot getter IC pattern — **DONE 2026-05-01**

`d5332e48`: Recognize `^ ivarA op1 ivarB op2 const` (op ∈ {+,-},
const ∈ {-1,0,1}) and dispatch inline via IC bit 57.  Two methods
in the image match (OrderedCollection>>size and SocketStream>>inBufferSize),
so the dispatch table impact is contained.

**Bench:** `1M getter+yourself` (= `obj size. obj yourself` × 1M):
98ms baseline → 20ms (5× speedup) when bench-suite runs cleanly.
Other benches stable.  Opt-in via `PHARO_MULTISLOT_GETTER=1`.

**Encoding:** new bit 57 in IC `extra` word.  Decoder in IC_HIT
macro and stencil_sendJ2J's IC-hit handler unboxes both slots, does
scalar math with `__builtin_*_overflow` checks, re-tags, pushes.
Bails to slow send on non-SmI / overflow.

**Future extension candidates:** could add 3-slot patterns like
`^ ivarA + ivarB + ivarC` (no const) using bit 56, but only one
image method matches that shape — not worth the encoding work.
