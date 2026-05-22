# jit-may23 — JIT optimization task queue

**TO THE GOAL RUNNER**: this is a queue of small tasks. The
work pattern is:

1. Pick the topmost task in `## Queue`.
2. Do the ONE concrete action it specifies.
3. Move the task to `## Closed` with a one-line result.
4. Pick the next task.

**Hard rules** (these matter — past sessions stopped due to violating them):

- **DO NOT write "session totals" or "progress summary"
  messages.** Those are stop signals.  Just move to the next
  task silently.
- **DO NOT batch updates to this doc.**  After EACH task,
  commit + push, then immediately pick the next one.
- **DO NOT mark a task "deferred" because of tool/time
  excuses.**  If the task says "use perf" but you're on macOS,
  substitute `sample`/`dtrace`/timed counters.  Pick whatever
  works.
- **DO NOT write closing/wrap-up messages between tasks.**  No
  "Branch clean", no "Real progress made", no "Now T7 is next".
  Just do T7.
- **The /goal hook NEVER clears.** The goal is to make the JIT
  as fast as Cog.  If the queue empties, add more tasks based
  on the latest findings and keep working.

## Queue

Each task has: `WHAT`, `HOW (one concrete action)`, `DONE WHEN`.

### Q21 — Reduce primFullClosureValue overhead (Q1 finding)

WHAT: prim207 is 94 cycles × 5.2M calls = dominant cost.  Look
at the function body and identify any work that can be inlined
or skipped for the common case.
HOW: read primitiveFullClosureValue.  Identify the 3 most
expensive sub-operations.  Document them in this doc.  Pick the
cheapest-to-remove and remove it.
DONE WHEN: a sub-op is removed AND bench-correctness 5/5 PASS.

### Q22 — Reduce activateBlock overhead (Q2 finding)

WHAT: activateBlock is 57 cycles = 60% of prim207.  Identify
the most expensive line in its body.
HOW: read activateBlock.  Look for loops, slot iterations,
ObjectMemory calls.
DONE WHEN: top-3 expensive operations documented.

### Q23 — Identify why fullGC fires 13 times (Q12 finding)

WHAT: bench-suite triggers fullGC 13 times.  Cog probably
triggers it 0-2 times for the same workload.  Why?
HOW: add a backtrace log when fullGC is called — show what
triggered it (low memory? explicit call? compact threshold?).
DONE WHEN: top trigger reason recorded.

### Q24 — Reduce fullGC trigger frequency

WHAT: based on Q23 finding, try to reduce the trigger.
HOW: adjust the threshold or eliminate spurious calls.
DONE WHEN: fullGC count drops to ≤5 on bench-suite without
losing bench-correctness.

### Q25 — Verify outer method JIT compile (Q13 finding)

WHAT: focused bitShift bench's outer method `shiftLoop`
doesn't reach JIT.  Why?
HOW: add trace at JIT compile decision (countMap_ threshold
check).  Run the focused bench, see what threshold shiftLoop
hits.
DONE WHEN: threshold-hit count + decision recorded.

### Q26 — Lower JIT compile threshold for non-block methods

WHAT: if Q25 shows shiftLoop got close to threshold but didn't
reach, lower the threshold for methods (not blocks).
HOW: find compile-threshold constant.  Test lowered value.
DONE WHEN: focused bitShift bench has bitOp > 0 AND
bench-suite shows no regression.

### Q27 — Inline self-send when receiver type is known

WHAT: benchFib has 3.5M activations — most are recursive self-
sends.  J2J handles some but not all.  Quantify the unhandled
fraction.
HOW: add counter at the recursive-self-send path in asmjit-T1.
DONE WHEN: hit count recorded.

### Q28 — Investigate `at:` IC misses

WHAT: `at:` is the #1 selector by IC patches (8.5M).  Are these
all hits, or are there many polymorphic misses?
HOW: add a trace at at: IC patch site logging the receiver
class.  Count distinct classes.
DONE WHEN: list of classes + count.

### Q29 — Audit OSR (on-stack replacement) overhead

WHAT: `[JIT] Stats: OSR=215195`.  OSR fires 215K times per
bench-suite.  What's the per-OSR cost?
HOW: add a timed counter around the OSR entry point.
DONE WHEN: avg cycles recorded.

### Q30 — Reduce IC miss rate (currently 14%)

WHAT: IC hit rate is 86% — 14% miss.  Each miss costs ~100
cycles in the chain-loop path.  At 9.5M sends, that's 1.3M
misses × 100 = 130M cycles overhead.
HOW: from Q16, top miss selectors are indexOf:/next:putAll:/
objectAt: — polymorphic call sites.  Add a 2nd-class entry to
the IC for polymorphic sites.
DONE WHEN: miss rate drops, bench-correctness PASS.

## Closed

(Move tasks here as you finish them.)

- ✅ T1: bitOp dispatch before bit 60.  Fires on synthetic bench.
- ✅ T2: floatOp dispatch before bit 60.  Same fix.
- ✅ T3: top-10 selector survey.  at:/at:put: dominate.
- ✅ T4: basicNew 0-arg inline.  **3-6% wins** on multiple benches
  (51K hits per bench-suite run).
- ❌ T5: even-like predicate emit (15% regression, reverted).
- ✅ T6: per-reason compile-fail counters (1M unsuppPrim found).
- ✅ T7: SmI prims 10-13 added (100K fewer fails).
- ✅ T14: cull: investigated (Step 9-10 territory).
- ✅ T15: Sista bail rates irrelevant (negative cache).
- ✅ T17: hot-block detection proposal documented.
- ✅ T19: no remaining Sista baked pointers.
- ✅ T20-partial: SmI mul 0x68 inline.
- ✅ **Q1**: primitiveFullClosureValue fires 5.2M+ times per bench-suite
  run, avg 94 cycles per call (~3.9µs).  Block invocation is the
  dominant bench-suite overhead.
- ✅ **Q2**: activateBlock avg 57 cycles per call (5.2M+ calls).
  60% of primFullClosureValue's time is in activateBlock itself
  (the C++ frame setup); 40% in the rest of prim207 (numArgs check,
  receiver fetch, primitive dispatch).
- ✅ **Q19**: 0x6E/0x6F (bitAnd/bitOr) IS inlined for x86_64
  (AsmjitT1.cpp:1568) but NOT for ARM64 — ARM emit checks only
  isPhase3ArithOp at line 2705, not isPhase3BitOp.
- ❌ **Q20**: attempted ARM inline emit for 0x6E/0x6F.  Default
  5/5 PASS but the bail path used EXIT_RETURN incorrectly (would
  cause early method exit on non-SmI operands, not chain-loop
  fallback).  Reverted.  Needs careful study of the existing
  isPhase3ArithOp bail-via-end pattern to implement correctly.
- ✅ **Q18**: multi-slot fires ONLY on `size` selector.
  Pattern `^ slot2 - slot1 + 1` = Interval>>size (last-first+1).
  All 668 multi-slot bench-suite hits are interval size
  computations.  Narrow but real.
- ✅ **Q17**: retLit's 465K bench-suite hits come ENTIRELY from
  quick-prim path (primIdx 257-263).  Bytecode-detected
  `^ true/false/nil/0/1` (tmi.returnsLiteral) never fires —
  Pharo's standard predicates use quick prims.
  Counter at primIdx==257 fired ~16 times (IC fills).
  Counter at tmi.returnsLiteral path: 0.
- ✅ **Q16**: top 5 IC-miss selectors:
    indexOf: 9509  next:putAll: 4876  objectAt: 1894
    size 1629  signFlag 1138
  Small absolute miss counts (vs 8M IC hits = <1% miss rate).
  Not a major perf lever.
- ✅ **Q15**: step() runs < 65536 times during bench-suite —
  same finding as Q3.  Interp-level bytecode dispatch isn't a
  bottleneck.  bench-suite runs almost entirely in JIT-compiled
  code.
- ✅ **Q14**: audited inlinePrimKind list (Interpreter.cpp:18271+).
  Covers SmI arith (1-9), bitOps (14/15/16/17 = primKind 11-13/19),
  identityHash (75), Array at/atPut/size (60-62), basicNew(:) (70/71),
  SmallFloat +/-/* (541/542/549).  Missing common ones:
  - prim 10 (SmI `\\`), prim 11 (SmI `//`)  — not in Q4 top 10
  - BoxedFloat prims 41-42  — different code path, complex
  - FFI/system prims  — can't be JIT-inlined
  None worth adding at session-scope (bench-suite doesn't
  exercise them in hot paths).
- ✅ **Q13**: focused bitShift bench shows bitOp=0 despite my
  T1 fix.  Cause: the outer method (`shiftLoop`) doesn't reach
  JIT compile.  Even with `1 to: do:` inlined to a backward
  jump in shiftLoop's body, the method isn't being JIT-compiled
  by asmjit-T1.  Same root cause as Issue 4 — needs blocks/
  outer methods compiled.
- ✅ **Q12**: fullGC fires 13 times, total 1.3 BILLION cntvct
  cycles.  13× more expensive than scavenge.  GC is a significant
  but not dominant overhead source.  Cog handles benches with
  far less GC pressure — investigate why our GC fires so often.
- ✅ **Q11**: scavenge fires ~8 times per bench-suite, 12-15M
  cntvct cycles each, total ~100M cycles.  At cntvct's 24MHz
  freq that's ~4 seconds — but cntfrq may be higher; needs
  verification.  Order of magnitude: minor but measurable
  fraction of bench-suite runtime.
- ✅ **Q9/Q10**: 2-byte unrecognized patterns survey.
  Result: bench-suite has NO unrecognized 2-byte methods.
  Existing detectors (getter/setter/returnsSelf/retLit) catch
  all 2-byte patterns.  No follow-up emit needed.
- ✅ **Q7**: setter-increment pattern detector added.  Only 1
  method matches on bench-suite — far below the 100-method
  threshold for justifying an emit.  Q8 (the emit) skipped.
  Detector stays in tree (uses 0 dispatch cycles when no match).
- ✅ **Q6**: 0x62 (`<`) inline emit is at AsmjitT1.cpp:2763 —
  csel-based, ~5 instructions in the comparison branch.  Already
  optimal (the `+` arith path has more instructions for the
  overflow check + retag).  No change needed.
- ✅ **Q4/Q5**: Top 10 activations on bench-suite:
    benchFib 3.5M  at: 1.4M  between:and: 1.3M  size 1.0M
    value: 1.0M  digitValue: 728K  byteAt:put: 728K  new 603K
    abs 600K  hash 400K
  value: is 1M — confirms Q1's 5.2M block invocations include
  value:, value:value:, etc. across the suite.
- ✅ **Q3**: step() bytecode dispatch is NOT a bottleneck.
  Timed counter never fires during bench-suite — bench-suite is
  almost entirely JIT-executed.  step() only runs for startup
  code + interp-only paths.  Confirms the bench-suite hot path
  goes through JIT, not interp.

