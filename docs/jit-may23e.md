# jit-may23e — remaining JIT work, designed for `/goal` + fresh sessions

## Why this doc exists separate from jit-may23d

The `/goal` hook evaluates the conversation transcript each time it
checks for completion.  When a session grows long (many tool calls,
big doc references, accumulated file edits), the transcript exceeds
the API context limit and you get:

    Stop hook error: Hook evaluator API error: Prompt is too long

The fix is design, not infrastructure: keep each `/goal` invocation
to **one atomic chunk** that fits in a **fresh session**.  Don't try
to execute a whole multi-chunk plan inside one conversation —
restart between chunks.

## Plan structure (atomic chunks)

Each chunk:
- Has a 1-line `/goal` invocation that fits in one prompt.
- Is self-contained — reads only this doc + named source files.
- Has a binary done-when condition.
- Sized for ≤30 min of execution; if it balloons, the chunk's
  failure mode IS to split itself further into a follow-up chunk
  and stop.
- Pushes its work + updates this doc with the result before
  ending.

When starting a new chunk, **begin a fresh session** (not
continuation).  Use:

    /goal execute chunk Eα from docs/jit-may23e.md

Or for autonomous mode:

    /goal execute one chunk from docs/jit-may23e.md; pick the
    highest-priority unblocked one

The hook then sees only that turn's transcript plus the doc, not
megabytes of prior exploration.

## State at start of jit-may23e

- Pre-session (commit `9b74bebd`): 1625 ms bench-suite avg.
- After jit-may23c+d work: ~1349 ms median (3-run).
- Gap to Cog: ~10× (1349 / 136).
- Shipped infrastructure: W1-W3 IC HIT inline, W5 Sista select:
  splice (with kSendUnspeculated extension), W6/W7 even/odd
  shortcuts + upgradeICToJ2J fix.

## Chunks (in priority order)

### Eα — Stabilize collect 10x100K bimodality (60 min)

Bench-suite has bistable Sista compile timing: sometimes collect =
97 ms (good case), sometimes = 235 ms (bad case).  ~140 ms swing.
Bias toward good case = real win.

Investigate via:
1. Read `/Users/wohl/src/iospharo/src/vm/jit/sista/SistaRuntime.cpp`
   compile gates + cache invalidation.
2. Compare a good-run log vs bad-run log at `[SISTA-COLLECT-EMIT]`
   timing — what's compiled first.
3. Hypothesis: hint propagation timing.  When the iter block of
   collect is hot before its enclosing method, Sista compiles with
   hints → kCountedLoopArrayCollect fires.  When enclosing method
   compiles first, hints are empty → splice doesn't fire.

DONE WHEN: 10-run bench-suite median ≤ 1250 ms (i.e., consistently
hits the good case).  OR: documented why the bimodality can't be
nudged without a different attack.

`/goal`: `execute chunk Eα from docs/jit-may23e.md. push without
asking. stop after Eα is done or you hit a real blocker.`

### Eβ — Add Sista-side pattern recognition for `even`/`odd` (90 min)

When Sista's lift sees `[:e | e even]`, the block body becomes
`pushTemp 0; send #even; returnTop`.  Currently `send #even`
lifts to `kSendUnspeculated`.  My W5.4-ext lowering invokes
`jit_rt_sista_call_send` per iteration — but the W7 shortcut in
that helper means the actual cost is small.

But: a true Sista-level inline would replace kSendUnspeculated
with a new Op like `kPrimEvenOddCheck(operand)` that lowers to
3-4 inline instructions (tag-check + bitand + cmp + csel).  This
saves the helper call overhead entirely.

Sub-chunks:
- Eβ.1: Add `Op::kPrimEvenOddCheck` to `SistaIR.hpp` (5 min).
- Eβ.2: In `SistaBuilder.cpp`, after a Send1 to a method that
  matches the even/odd shape (use `TrivialMethodInfo`), emit
  `kPrimEvenOddCheck` instead of `kSendUnspeculated` (20 min).
- Eβ.3: Add lowering in `SistaLowering_arm64.cpp` — inline arith
  + csel.  Bail to helper on non-SmI receiver (40 min).
- Eβ.4: Test.  3-run bench-suite (15 min).

DONE WHEN: select 10x100K SUM 3-run median ≤ 150 ms (down from
~190).  bench-correctness fib 20/28/30 5/5 PASS.

`/goal`: `execute chunk Eβ from docs/jit-may23e.md. push without
asking.`

### Eγ — Sista splice for `Array>>sort:` or `sortBlock:` (2-3 h)

Sort uses comparator block; per-iter dominated by block dispatch.
Pharo's `Array>>sort:` is mergesort-based — recursive structure.

Realistic v1: detect the inner per-merge-step iteration only.  The
recursive split is handled by interp/T1 normally.  The inner
merge-step iteration is a counted loop with the comparator block —
that's the splice-able shape.

Sub-chunks:
- Eγ.1: Read `Array>>mergeSort` or equivalent to understand the
  inner loop bytecode shape (10 min).
- Eγ.2: Add `Op::kCountedLoopArraySortMerge` + detection mirror
  (30 min).
- Eγ.3: Lowering — similar to kCountedLoopArrayCollect but with
  comparator semantics (60 min).
- Eγ.4: Helper for deopt (20 min).
- Eγ.5: Test (15 min).

DONE WHEN: sort 100K SUM 3-run median ≤ 150 ms (from ~280 ms).
bench-correctness 5/5 PASS.  If lowering balloons past 90 min,
fall back: emit the existing kSendUnspeculated for comparator
calls and use the splice only to fuse the iteration.

`/goal`: `execute chunk Eγ from docs/jit-may23e.md. push without
asking.`

### Eδ — Generic dispatch overhead reduction (estimate)

Per-send overhead is ~8.5 ns measured vs Cog ~2 ns.  Closing this
needs structural change: direct method-to-method calls without J2J
save for tier-2 callees that don't bail.

Sub-chunks:
- Eδ.1: Identify which JITMethods are "no-bail tier-2".  Use
  existing `canBailMidMethod == false && hasSends == false` as
  proxy.  Add a new flag `canSkipJ2JSave` to JITMethod (15 min).
- Eδ.2: At IC HIT inline-J2J emit, check the flag.  If set,
  emit a direct br/ret pair without save push (60 min).
- Eδ.3: Test (15 min).

DONE WHEN: tinyBenchmarks sends/sec ≥ 200 M/s (from ~138 M).
bench-correctness 5/5 PASS.

`/goal`: `execute chunk Eδ from docs/jit-may23e.md. push without
asking.`

### Eε — Re-measure full bench-suite (10 min)

After Eα-Eδ, take a clean 10-run bench-suite.  Update this doc
with new numbers and gap to Cog.

DONE WHEN: numbers recorded.

`/goal`: `execute chunk Eε from docs/jit-may23e.md. push without
asking.`

## Rules for the /goal hook to NOT explode

These are the design rules that keep each session's transcript
small enough for the hook evaluator:

1. **One chunk per session.**  Don't chain Eα→Eβ→Eγ in one
   `/goal` invocation.  The hook evaluator's prompt grows with
   each tool call.  Long chains = exceeds limit.

2. **Short `/goal` text.**  Maximum ~50 chars total.  Don't quote
   long passages from this doc in the goal text — reference by
   chunk name (`execute chunk Eα`).

3. **Fresh session per chunk.**  Restart the conversation between
   chunks.  Don't try to continue in a long-running thread.

4. **Doc is the state**, not the transcript.  Update the chunk's
   status in this doc as the FIRST step of each chunk.  Future
   chunks read the doc, not the conversation history.

5. **Push immediately on chunk completion** so the next session
   sees the new state via git, not via transcript.

6. **No `wait for me` patterns.**  Each chunk runs end-to-end
   autonomously.  If user input is needed, fail the chunk and
   document the question.

## Status tracker (update in-place)

    Eα   PENDING    Stabilize collect bimodality
    Eβ   PENDING    Sista-side even/odd inline
    Eγ   PENDING    Array>>sort: splice
    Eδ   PENDING    Generic dispatch overhead
    Eε   PENDING    Re-measure

## If the hook still says "Prompt is too long"

The issue is the `/goal` hook re-evaluates the WHOLE conversation
each time it considers whether to allow stopping.  If you've been
running multiple chunks in one session, the transcript may have
grown beyond the limit.

Workaround:
1. `/goal clear` to drop the current goal.
2. Start a fresh conversation.
3. Re-invoke `/goal execute chunk Eβ from docs/jit-may23e.md.`

The new session sees only:
- The system prompt.
- Your `/goal` invocation.
- The doc this references.

That fits in the hook's context window.

## Closing note

The fundamental change between jit-may23d and jit-may23e: don't
try to execute the whole plan in one session.  Restart between
chunks.  This is a workflow change, not a plan change.

Each chunk is sized to fit in one fresh session.  If a chunk
itself produces too much transcript, split it further into
sub-chunks and execute them in separate sessions.
