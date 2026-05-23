# jit-may23b — JIT optimization queue (smaller chunks)

## /goal protocol

This doc drives a /goal loop. The rules below are LOAD-BEARING.
Past sessions stopped despite the previous doc's rules; these
are stricter.

### When the user sends a message mid-loop

1. Answer in ≤2 sentences.
2. Do NOT recap progress, restate the plan, or write
   "continuing now"/"keep going"/etc.
3. Immediately call the next tool to advance the next task.

The user reads diffs and commits to see progress. Don't reprint
what they can already see. After a user interrupt, the very next
thing you output should be a tool call, not prose.

### When a task completes

- Edit the doc to move the task to `## Closed` (1 line).
- Commit + push.
- IMMEDIATELY start the next task. No "Q5 done. Moving to Q6."
  preamble.

### When the queue is empty

DO NOT STOP. Instead:

1. Run `grep -n "if (std::getenv(\"" src/vm/Interpreter.cpp
   src/vm/Primitives.cpp src/vm/ObjectMemory.cpp` and check each
   hit for per-call pattern in a hot function (search for callers
   of the enclosing function in step/activateMethod/IC dispatch).
   If any found, add as tasks.
2. Profile + identify the next ≥100ms bench gap. Add tasks to
   address it.
3. Goal NEVER auto-clears. Keep working until the user explicitly
   says stop.

### Task sizing

Each task ≤15 min. If a task is bigger, split it. Sub-task names
get letter suffixes (e.g., R5a, R5b).

### "DONE WHEN" must be binary

- ✅ Binary: "file X line Y edited", "counter Z is non-zero",
  "bench-correctness 5/5 PASS".
- ❌ Open-ended: "audit ...", "identify ...", "characterize ...".

Open-ended tasks invite summary responses. Binary tasks force
action.

## Queue

### R1 — Cache PHARO_TRACE_TOTAL_STEPS at Interpreter.cpp:2711

ACTION: convert `if (std::getenv("PHARO_TRACE_TOTAL_STEPS"))` to
the `static const bool` pattern.
DONE: 1 line edited, builds, default 5/5 PASS.

### R2 — Cache PHARO_TRACE_OP_VALUE1 if not yet cached

ACTION: grep `PHARO_TRACE_OP_VALUE1` to verify cached. If not,
cache it.
DONE: either confirmation that it's cached, or 1 line edited.

### R3 — Cache PHARO_TRACE_EXEC_PRIM at Interpreter.cpp:16009

ACTION: see if this is in a hot function. Cache if so. If cold,
mark CLOSED with finding.
DONE: edit + commit OR finding recorded.

### R4 — Cache PHARO_NO_OSR_RECOMPILE at line 16640

ACTION: this is checked per-IC-recompile decision. Cache the
bool.
DONE: 1 line edited.

### R5 — Cache PHARO_NO_SISTA_PER_BC at line 16697

ACTION: same pattern.
DONE: 1 line edited.

### R6 — Cache PHARO_TRACE_SISTA_PERBC at line 16799

ACTION: same.
DONE: 1 line edited.

### R7 — Cache PHARO_SEM_SIGNAL_TRACE at line 3414

ACTION: same.
DONE: 1 line edited.

### R8 — Audit ALL remaining `if (std::getenv(` in Interpreter.cpp

ACTION: `grep -n "if (std::getenv(" src/vm/Interpreter.cpp` and
list ALL hits NOT already converted. For each, determine: hot
(in step/activateMethod/activateBlock/IC-dispatch/per-bytecode)
or cold (init/exit/error). List in this doc.
DONE: list pasted into `## Closed`.

### R9 — Cache every hot getenv from R8

ACTION: convert each one in R8's hot list.
DONE: all hot ones cached, build passes, bench-correctness 5/5.

### R10 — Run full bench-suite, record new baseline

ACTION: `PHARO_VM=/tmp/harness/pharo timeout 240 scripts/run_benchmarks.sh`
3 times. Record fib/sum/dict/sort/instVar/floatSum/stringHash
medians.
DONE: numbers in this doc under `## Bench-suite tracking`.

### R11 — Audit Primitives.cpp `if (std::getenv(` patterns

ACTION: list all hits. Identify hot vs cold (Primitives is mostly
called from IC HIT, so most are hot).
DONE: list pasted.

### R12 — Cache hot Primitives.cpp getenvs

ACTION: convert each hot hit from R11.
DONE: edits committed.

### R13 — Audit ObjectMemory.cpp `if (std::getenv(` patterns

ACTION: list and classify.
DONE: list pasted.

### R14 — Audit src/vm/jit/ for `if (std::getenv(` patterns

ACTION: `grep -rn "if (std::getenv(" src/vm/jit/` and list.
DONE: list pasted.

### R15 — Cache hot JIT-runtime getenvs

ACTION: convert any per-call ones.
DONE: edits committed.

### R16 — Run benchmark, check for regressions

ACTION: 3 bench runs. Compare to R10 baseline.
DONE: deltas recorded.

### R17 — Profile sum 1M with `sample` (macOS dtrace alt)

ACTION: `sample test_load_image 5` while bench-suite is running
sum. Capture top 10 symbols.
DONE: list pasted.

### R18 — Identify top non-JIT hot symbol from R17

ACTION: from R17 output, pick the hottest C++ symbol that ISN'T
JIT-emitted code. Look at its body for any optimizations.
DONE: 1 finding recorded.

### R19 — Implement 1 optimization from R18

ACTION: based on R18 finding, make 1 focused change.
DONE: edited + committed OR documented why not applicable.

### R20 — Profile floatSum 1M with `sample`

ACTION: same as R17 but for floatSum.
DONE: list pasted.

### R21 — Identify boxedFloat overhead

ACTION: floatSum uses SmallFloat for the running total, but each
+ might create a BoxedFloat. Look at Primitives.cpp prim 541
result encoding.
DONE: yes/no answer to "do we box".

### R22 — Profile instVar 1M with `sample`

ACTION: same.
DONE: list pasted.

### R23 — Profile collect 10x100K with `sample`

ACTION: same.
DONE: list pasted.

### R24 — Profile dict 50K with `sample`

ACTION: same.
DONE: list pasted.

### R25 — Profile sort 100K with `sample`

ACTION: same.
DONE: list pasted.

### R26 — Identify pattern across profiles (R17,R20-R25)

ACTION: which C++ symbol appears in ≥3 of the 6 bench profiles?
That's the highest-leverage fix target.
DONE: top shared symbol named.

### R27 — Pick one R26 symbol and audit body

ACTION: read the function. List any sub-operations that could be
inlined or skipped.
DONE: list of 3 candidates.

### R28 — Implement smallest R27 candidate

ACTION: make the edit. Bench-correctness 5/5 PASS.
DONE: commit.

### R29 — Measure R28's bench impact

ACTION: 3 runs of bench-suite. Compare to R16.
DONE: delta recorded; reverted if regressed.

### R30 — Move to next R27 candidate

ACTION: implement.
DONE: commit.

## Closed

(Moves go here. 1 line per task.)

## Bench-suite tracking

(R10 will populate this.)
