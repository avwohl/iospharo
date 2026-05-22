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

### Q3 — Measure step() bytecode dispatch overhead

WHAT: how much time is in the interp's switch-on-bytecode
dispatch?
HOW: add a timed counter that fires every 65K bytecodes
(measure time between fires).  Run bench-suite, dump avg
ns/bytecode.
DONE WHEN: a number is printed.

### Q4 — List top 10 methods by activation count

WHAT: which methods are activated most frequently?  We're
optimizing for the hot path.
HOW: extend the existing PHARO_TRACE_TOP_SELECTORS to count
ACTIVATIONS (not IC patches) per method.  Run bench-suite,
dump top 10.
DONE WHEN: top 10 list is in this doc under `## Closed`.

### Q5 — Find activation count of `value:` method

WHAT: extracted from Q4 — `value:` activations are the block
invocation count.  Quantify.
HOW: from Q4's output.
DONE WHEN: number recorded.

### Q6 — Add inline emit for SmI `<` (0x62) as full arith

WHAT: 0x62 is currently in the comparison branch (csel
true/false).  Pattern `^ self < n` would benefit from skipping
activation.  Verify current emit is fast enough.
HOW: read AsmjitT1.cpp:2716+ comparison emit.  Compare bytecode
count vs `+`.  If comparable, no change.  If longer, document
why.
DONE WHEN: finding recorded.

### Q7 — Detect setter increment pattern

WHAT: `incrementX  x := x + 1` style mutators.  Bytecode:
pushRcvrA, pushOne, send+, popStoreRcvrA, returnReceiver (5 bytes).
HOW: add to `detectTrivialMethod` after the multi-slot detection.
Encode in a new IC bit (bit 53 unused).  Don't add the emit yet —
just verify the detector matches ≥100 methods on bench-suite.
DONE WHEN: counter shows ≥100 detections.

### Q8 — Add emit for Q7 setter-increment pattern

WHAT: ARM emit that does `rcv.slot[A] = (rcv.slot[A] + 1)`
with overflow check, returns receiver.
HOW: mirror the existing setter emit at AsmjitT1.cpp:4101+
but with the arith op.  ~12 instructions.
DONE WHEN: bench-correctness still PASS, setter-increment
counter fires on bench-suite.

### Q9 — Find common 2-bytecode method bodies

WHAT: methods with bcLen==2 that aren't yet covered by an
inline emit.
HOW: add a debug log in detectTrivialMethod that records
methods with `bcLen == 2` and no match (no getter, no setter,
etc.).  Run bench-suite, dump top patterns.
DONE WHEN: top 5 unrecognized 2-byte patterns recorded.

### Q10 — Implement top pattern from Q9

WHAT: if Q9 found a 2-byte pattern with ≥1000 hits, add a
detector + emit.
HOW: depends on the pattern Q9 found.
DONE WHEN: pattern fires on bench-suite without regression.

### Q11 — Measure scavenge GC time

WHAT: how much bench-suite time is in scavenge?
HOW: add a timed counter around `ObjectMemory::scavenge()`.
DONE WHEN: ms total recorded.

### Q12 — Measure fullGC time

WHAT: how much bench-suite time is in full GC?
HOW: timed counter around `fullGC()`.
DONE WHEN: ms total recorded.

### Q13 — Find Pharo's bitShift: bench

WHAT: stringHash uses `bitShift:` heavily.  Does the current
inline emit for primKind 13 fire?
HOW: write a focused bench (3-5 line script) that hammers
bitShift: in a JIT-compilable method.  Run with
PHARO_T1_INLINE_PRIM_COUNTERS=1, dump bitOp counter.
DONE WHEN: bitOp count recorded for the focused bench.

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

