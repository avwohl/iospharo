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
- **The /goal hook clears only when the queue is empty.**
  Period.  Keep working.

## Queue

Each task has: `WHAT`, `HOW (one concrete action)`, `DONE WHEN`.

### Q19 — Check if 0x6E/0x6F (bitAnd/bitOr) bytecode is inlined

WHAT: `n bitAnd: m` as a 0x6E special selector.  Does asmjit-T1
inline it at the bytecode level (like 0x60-0x67)?
HOW: search AsmjitT1.cpp for "0x6E" in emit code.
DONE WHEN: yes/no answer + finding recorded.

### Q20 — Add 0x6E/0x6F inline if missing

WHAT: if Q19 found these aren't inlined at bytecode level, add
them.  SmI bitwise (no overflow check needed).
HOW: extend the isPhase3 op range and add tagged-bitwise emits
to the switch.
DONE WHEN: bench-correctness PASS, counter fires.

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

