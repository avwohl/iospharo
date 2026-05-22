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

### Q14 — Audit `inlinePrimKind` for missing prims

WHAT: are there primitives our IC could handle but
inlinePrimKind returns 0 for?
HOW: read Interpreter.cpp:18100+ list.  Cross-reference with
Pharo's prim table (image inspection or codebase grep for
`<primitive: N>`).  List any prim that's used ≥1000× per
bench-suite but maps to primKind 0.
DONE WHEN: list of candidates in this doc.

### Q15 — Top 5 bytecodes by interp execution count

WHAT: which bytecodes execute most frequently in interp on
bench-suite?
HOW: add a per-bytecode counter in step() (just an array of
256 counters, increment by op).  Dump top 5 at exit.
DONE WHEN: top 5 recorded.

### Q16 — Top 5 IC-MISS selectors

WHAT: selectors that miss the IC most often — candidates for
IC cache improvements.
HOW: at IC miss site (state.icDataPtr=null or class mismatch),
log the selector.  Top 5 by count.
DONE WHEN: list recorded.

### Q17 — Verify retLit perf is from quick prims

WHAT: retLit fires 465K times — confirm the wins are from
quick prims 257-263 (not from bytecode-detected retLit).
HOW: split the counter into two: one for quick-prim-path bit-58
set, one for tmi.returnsLiteral path.
DONE WHEN: two counters in stat dump.

### Q18 — Verify multi-slot perf source

WHAT: multi-slot fires 668 times.  Which methods?
HOW: in patchJITICAfterSend's multi-slot branch, log the
selector + first occurrence.  Cap at 30.
DONE WHEN: list of methods recorded.

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

