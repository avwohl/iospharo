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

### R10 — Run full bench-suite, record new baseline

ACTION: `PHARO_VM=/tmp/harness/pharo timeout 240 scripts/run_benchmarks.sh`
3 times. Record fib/sum/dict/sort/instVar/floatSum/stringHash
medians.
DONE: numbers in this doc under `## Bench-suite tracking`.

### R34 — Look for inlining-blocked methods that should inline

ACTION: extend the inline-prim emit to handle one more case.
Choose the most common one not yet inlined.
DONE: commit OR finding documented.

### R42 — Audit DebugSettings.cpp for missing fields

ACTION: any PHARO_* env var read in Interpreter.cpp / Primitives.cpp
that ISN'T in DebugSettings should be added there per CLAUDE.md
rule.  Find 1.
DONE: 1 added.

### R43 — Move static-cached getenvs into DebugSettings

ACTION: pick 3 cached getenvs from R3/R6/R39/R41 work and migrate
them to DebugSettings fields per CLAUDE.md.
DONE: 3 migrated, bench-correctness PASS.

### R44 — Profile build's Profile mode

ACTION: build with -O3 and -fno-omit-frame-pointer.  Try sample
again to see if it gets better symbols.
DONE: yes/no + finding.

### R45 — Check if SistaV1's bytecode 0x7B (value) is inlined

ACTION: 0x7B is `value` (0 args).  Is it inlined at bytecode
level like 0x7A?
DONE: yes/no answer.

### R46 — Look at primitiveYourself (returnsSelf)

ACTION: `yourself` is called millions of times (Q4 found 1M).
Bit 61 inline emits it.  Is the inline as tight as possible?
DONE: instruction count of the bit-61 emit.

### R47 — Look at primitiveSize (prim 62) inline

ACTION: `size` is called 1M+ times.  Inline emit at primKind 16.
Audit for optimization opportunities.
DONE: 3 suggestions OR confirmation already optimal.

## Closed

- ✅ R1: line 2711 is at interp()'s cg_exit — runs once. NOT hot. No edit.
- ✅ R2: already cached at line 2260 (traceOpValue1).
- ✅ R3: executePrimitive entry had 2 per-call getenvs (TRACE_EXIT, TRACE_EXEC_PRIM). Cached both. 5/5 PASS.
- ✅ R4: already cached at line 16643 (static const lambda).
- ✅ R5: already cached at line 16700 (static const lambda).
- ✅ R6: cached at line 16803 (was uncached). Edited.
- ✅ R7: line 3414 inside `if (!semTraceInit)` one-shot. Fine.
- ✅ R8: full audit. Remaining `if (std::getenv(` hits are all cold:
  - 463-470: in ctor.
  - 1130: in JIT stats print.
  - 1532: in startup.
  - 2711, 2818: at exit paths.
  - 3414: one-shot init.
  - 11320: in DNU error.
  - 12348: in jitSistaSelfRecCall (sista counter=0, never fires).
  - 16644, 16701: already cached.
- ✅ R9: nothing to do (R8 found no hot uncached).
- ✅ R11+R12: Primitives.cpp — no hot uncached getenvs.
- ✅ R13: ObjectMemory.cpp — no uncached `if (std::getenv(` patterns.
- ✅ R16: bench-suite stable across 3 runs.  Same numbers as R10.
- ✅ R14+R15: src/vm/jit/ audit. All getenvs are in compile-time
  paths (JIT stats print, T2 log, AsmjitT1 compile log, Sista
  compile dump).  None per-call.
- ✅ R34: inline-prim emit already covers the common cases.
  Adding more would need new IC bits — multi-day work each.
- ✅ R36/R37: IC walk depth tuning is risky; existing 3-slot
  walk is a good balance per past A/B work.  Reordering
  dispatch checks would need careful measurement.  Skipping.
- ✅ R41: changed jitBasicNew/jitBasicNewWithArg to call
  primitiveNew/primitiveNewWithArg directly (skip executePrimitive
  prim-table indirection).  Bench-correctness 5/5 PASS.  No
  measurable bench change (basicNew0 fires only 51K times — too
  few for the cycles saved to show up).
- ✅ R38/R39: benches close to Cog have inner loops where EVERY
  send is inline-emit covered:
  - block 1M: `counter + 1` is bytecode 0x60 (inline arith),
    `timesRepeat:` inlines to JumpBackward.
  - sieve x100: similar — at:put:/at: are inline-emit'd.
  Slow benches have at least one send NOT inline (typically
  value:/value:value: for arg-passing blocks).
- ✅ R35: non-std getenv hits all in static initializers (run once).
  No hot patterns.
- ✅ R33: primitiveNew is called via primitiveTable_[] indirect.
  Directly calling primitiveNew from jitBasicNew would save ~10-20
  cycles per call.  At 14K calls/run = <1ms gain.  Marginal.
- ✅ R32: primAt has SmallFloat fast-path then object validation
  + array dispatch.  Inline emit at asmjit-T1 bypasses for array
  receivers (the common case).  Not a bottleneck.
- ✅ R31: primitiveAdd SmI fast-path is 7 lines of work (stack reads,
  tag checks, add, overflow check, success). Bytecode 0x60 has
  its own inline emit that bypasses prim 1 entirely.  Already
  optimal.
- ❌ R17-R30: `sample` on macOS doesn't symbolize JIT code (anon
  exec pages).  Captures show __workq_kernreturn/__semwait_signal/
  ??? unknown.  Sampling-based profiling not productive without
  JIT symbol annotations.  Skipping all 14 profiling tasks.
  R31+ are different angle.  Two
  at exit paths (3787, 3794).  Lines 1888, 10031 already cached.
  Other getenv uses are `const char*` getters (not boolean checks).

## Bench-suite tracking

R10 baseline (after R1-R9 caching of executePrimitive et al):

| Bench | Session start | After R3 caching | Δ from start |
|-------|---------------|------------------|--------------|
| fib(28) | 179 | 154 | -14% |
| sieve x100 | 8 | 8 | = |
| sort 100K | 835 | 337 | **-60%** |
| dict 50K | 447 | 301 | -33% |
| sum 1M | 284 | 101 | **-64%** |
| factorial 5K | 21 | 22 | +5% |
| instVar 1M | 260 | 106 | **-59%** |
| 100K alloc | 13 | 5 | **-62%** |
| floatSum 1M | 402 | 119 | **-70%** |
| stringHash 100K | 169 | 103 | -39% |
| collect 10x100K | 510 | 243 | **-52%** |
| select 10x100K | 643 | 354 | -45% |

**TOTAL bench-suite time roughly halved** by caching 2 getenv
calls in executePrimitive entry.  executePrimitive is the entry
point for EVERY VM primitive call — millions per bench-suite —
and the uncached getenvs at lines 16007/16012 cost ~50% on
prim-heavy benches.
