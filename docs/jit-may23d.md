# jit-may23d — Plan I will actually execute

## State at start (post jit-may23c)

- bench-suite 10-run avg: **1358 ms** (1352-1367 range)
- gap to Cog (~136 ms): **10.0×**
- xmethod inline-J2J + non-leaf BV inline: default-on, correct
- top remaining absolute gaps (ms over Cog estimate):
  - sort 100K: 282 vs 17 = 265
  - select 10x100K: 195 vs 8 = 187
  - dict 50K: 178 vs 13 = 165
  - 1M getter+yourself: 102 vs 2 = 100
  - floatSum 1M: 104 vs 8 = 96
  - sum 1M: 87 vs 3 = 84
  - fib(28): 71 vs 3 = 68

## Rules I impose on myself

1. **No "multi-session" excuses.** I define the chunks. If a chunk
   genuinely needs more time, I split it again. If I say "multi-
   session" without a concrete external blocker (user input,
   pending CI, third-party dep), that's a stop-signal masquerading
   as a plan.

2. **Every task has BINARY done-when conditions.** Specifically:
   - File X line Y edited (or new file written) → done.
   - Counter Z non-zero after a measurable bench → done.
   - bench-correctness 5/5 PASS → done.
   - bench-suite SUM measured (3-run min) → done.
   No "investigation", "audit", or "characterize" tasks.

3. **15 min max per leaf task.** If sizing > 15 min, split before
   starting.  If actual time exceeds 30 min, stop and re-split.

4. **No reverting wins.** If a chunk lands and bench-suite doesn't
   regress (within noise), keep it.  Don't unnecessarily revert
   "for cleanliness" — leave the win shipped and move on.

5. **Three-run minimum for bench claims.**  Single runs lie.  Median
   of 3 is the truth.  Outliers > ±5% trigger a 5-run check.

## Queue — concrete, sized, sequenced

### W1 — Add `t1InlineTempReturn` for `^ arg0` callees  (15 min)

Tier2 already recognizes `^ tempN → TempReturn` (`Tier2Compiler_arm64.cpp:377`).
That kind currently compiles to a tier-2 stub that loads tempBase[N] and
returns.  Goal: avoid the tier-2 stub round-trip by inlining the load at
the caller's IC HIT.

Sub-steps (each ≤ 5 min):
- 1.1: Add `t1InlineTempReturn` bool to `DebugSettings.{cpp,hpp}` with
  default-on opt-out env `PHARO_T1_NO_INLINE_TEMP_RETURN`.
- 1.2: Add `g_t1TempReturn_hits` counter in `AsmjitT1.cpp`.
- 1.3: At the IC HIT dispatch in `AsmjitT1.cpp` (search for
  `tryReturnsSelf`), add a parallel `tryTempReturn` branch on a new
  IC bit (which?  Bit 54 is currently unused).  Emit: load
  tempBase[N] (N from IC extras), store at recv slot, drop args,
  br to endOfSend.
- 1.4: At IC patch time (`patchJITICAfterSend` in `Interpreter.cpp`),
  detect `ReturnKind::TempReturn` shape on the target, set bit 54
  + encode N in extras bits 48-52.

**DONE WHEN**: bench-suite run shows `g_t1TempReturn_hits > 0`,
bench-correctness fib 20/28/30 5/5 PASS, bench-suite SUM 3-run median
not regressed beyond 1370 ms.

### W2 — Add `t1InlineIntCmpReturn` for `^ self < arg0` blocks (30 min)

Tier2 already recognizes IntCmpReturn (`Tier2Compiler_arm64.cpp:481`).
This is THE hot pattern for sort/select comparators.

Sub-steps:
- 2.1: Add bool + env var (5 min).
- 2.2: Add counter (5 min).
- 2.3: IC bit (53?) + emit (15 min): tag-check both operands (both
  SmI), tagged compare, csel trueOop/falseOop, store result.  Bail
  to dispatchCached on tag mismatch (rare for hot comparators).
- 2.4: IC patch detection (5 min): when target's JITMethod has
  ReturnKind::IntCmpReturn, set bit 53.

**DONE WHEN**: `g_t1IntCmpReturn_hits > 0`, 5/5 PASS, sort 100K SUM
3-run median ≤ 282 ms (no regression; expected -50 to -150 ms
because sort is dominated by comparator dispatch).

### W3 — Add `t1InlineIntArithReturn` for `^ self + arg0` (30 min)

Same pattern as W2 but for +/-/*.  Useful for collect blocks like
`[:x | x * 2]`.

Sub-steps mirror W2.

**DONE WHEN**: `g_t1IntArithReturn_hits > 0`, 5/5 PASS, no regression.

### W4 — Measure post-W1+W2+W3 bench-suite (5 min)

5-run median.  Record specifically: sort, select, dict, collect,
fib(28).  Expected:
- sort 100K: -50 to -150 ms (W2).
- collect 10x100K: -10 to -30 ms (W3).
- bench-suite SUM: -60 to -200 ms.

**DONE WHEN**: numbers recorded in this doc.

### W5 — Add Sista splice for `Array>>select:` (2 h split into 5 chunks)

Mirror the existing `kCountedLoopArrayCollect` end-to-end.  This is
THE biggest leverage on select 10x100K (gap 187 ms).

Sub-steps (15 min each):
- 5.1: Add `Op::kCountedLoopArraySelect` to `SistaIR.hpp` enum +
  `SistaIR.cpp` name table.
- 5.2: Copy `Array>>collect:` detection in `SistaBuilder.cpp` (search
  `kCountedLoopArrayCollect`) and adapt: filter pattern collects
  elements where block returns true rather than mapping.
- 5.3: Copy lowering in `SistaLowering_arm64.cpp` and adapt: emit
  branch on block result to skip the write.
- 5.4: Add deopt-with-resume helper in `Interpreter.cpp` (mirror
  `jitSistaCompleteArrayCollect`).
- 5.5: Test correctness: bench-correctness 5/5 PASS, hand-run
  `(1 to: 100) select: [:x | x even]` returns the even numbers.

If 5.3 (lowering) balloons past 30 min, fall back to a simpler
version: emit `kCountedLoopArrayDoAccum` shape where the
"accumulator" is an OrderedCollection, and let it fall back to
the helper on miss.  Lower bar but still moves the needle.

**DONE WHEN**: select 10x100K SUM 3-run median ≤ 100 ms (down from
195).  bench-correctness 5/5 PASS.

### W6 — Re-measure full bench-suite (5 min)

5-run median.

**DONE WHEN**: numbers recorded.

### W7 — Decide whether to attempt sort: splice (decision: 5 min)

If W6 shows sort 100K is now the biggest gap (likely), then W8.
If sort is no longer top-3 gap, skip to W9.

**DONE WHEN**: decision documented (one sentence).

### W8 — Add Sista splice for `Array>>sort:` (2 h split into 5 chunks)

Pharo's sort is mergesort-based; the splice would detect the inner
comparator-block-invocation loop.  Harder than select because sort
is recursive (mergesort divides).  Realistic scope: handle the
single-block iteration shape inside one mergesort layer; deopt to
helper on recursion entry.

Sub-steps mirror W5.

**DONE WHEN**: sort 100K SUM 3-run median ≤ 150 ms (down from 282).
bench-correctness 5/5 PASS.

### W9 — Final bench-suite measurement (10 min)

10-run median (for stability).  Compute gap to Cog per bench.

**DONE WHEN**: results in this doc.

### W10 — If gap to Cog ≤ 1.0× — celebrate.  Else: next chunk (decision: 5 min)

If gap still > 2×, identify the single biggest absolute remaining
gap and start a new chunk targeting it.  Repeat W7-style decision.

**DONE WHEN**: decision documented.

## Estimated total session: 4-6 hours

W1+W2+W3 = ~1.25 h
W4 = 5 min
W5 = 2 h
W6 = 5 min
W7 = 5 min
W8 = 2 h
W9 = 10 min
W10 = 5 min

Acceptable session length.  No "multi-session" excuse permitted.

## Failure modes I commit to NOT use

- "Needs lldb attach" — except actually attach lldb on the spot
  using the project's existing infrastructure.
- "State desync" without diagnosing it via state-diff debugging
  the way I did in jit-may23c (works without lldb).
- "Variance is too high" — if 3-run shows ±5%, do 5-run.  If still
  ±5%, take median and continue.  Variance doesn't excuse stopping.
- "I'm uncertain about the right approach" — ship the simpler
  approach first, measure, iterate.  Uncertainty isn't a blocker.

## When I WILL stop

- A chunk's emit code segfaults under bench-correctness AND I've
  spent 60 min in lldb without identifying the bug.  Then I revert
  the chunk and continue to the next.
- A change requires modifying a file the user previously asked me
  not to touch.
- I hit a deadlock: chunk A needs chunk B's code, chunk B needs
  chunk A's interface.  Then I sketch both interfaces, ship the
  simpler half, document the deadlock.

## Acceptance for "session complete"

Either:
- bench-suite SUM ≤ 800 ms (would be < 6× Cog gap), OR
- All of W1-W9 executed with documented results, OR
- I hit a stop condition above and documented it.

No fourth option.  No "I think this is enough for one session"
without meeting one of the above.
