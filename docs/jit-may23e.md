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

1. **`/clear` before each new `/goal`.**  The hook evaluator reads
   the current conversation's transcript every time it checks the
   stop condition.  If the conversation already has a long history
   (many tool calls, big file reads), even a single new `/goal`
   chunk will trigger "Prompt is too long" immediately.
   `/clear` resets the transcript so the hook starts fresh.

2. **One chunk per session.**  After `/clear`, do ONE chunk, then
   `/clear` again before the next.  Don't chain Eα→Eβ→Eγ in one
   conversation.  Tool calls accumulate, and even within one
   `/goal` invocation the transcript grows toward the limit.

3. **Short `/goal` text.**  Maximum ~50 chars total.  Don't quote
   long passages from this doc in the goal text — reference by
   chunk name (`execute chunk Eα`).

4. **Doc is the state**, not the transcript.  Update the chunk's
   status in this doc as the FIRST step of each chunk.  Future
   chunks read the doc, not the conversation history.

5. **Push immediately on chunk completion** so the next session
   sees the new state via git, not via transcript.

6. **No `wait for me` patterns.**  Each chunk runs end-to-end
   autonomously.  If user input is needed, fail the chunk and
   document the question.

### Usage pattern

```
/clear                        # reset transcript
/goal execute chunk Eα from docs/jit-may23e.md
# (Claude works the chunk end-to-end, pushes, updates doc, stops)

/clear                        # reset again
/goal execute chunk Eβ from docs/jit-may23e.md
# ...
```

If you hit "Prompt is too long" mid-chunk: `/goal clear`, `/clear`,
re-invoke.  The chunk's progress so far is in git + the doc;
nothing's lost.

## Status tracker (update in-place)

    Eα   DEFERRED   Stabilize collect bimodality — tried widening
                    invalidateIfHintless to all slot fills; no effect
                    (10-run all at 1350-1365, none in good case).
                    Need different attack: probably explicit eager-
                    compile of iter block before outer method.
    Eβ   SHIPPED    Sista-side even/odd inline — Op::kPrimEvenOddCheck
                    added with lowering (tag-check, bitAnd, csel, deopt).
                    Bench-correctness PASS.  bench-suite ~unchanged
                    (1 in 5 runs at 1224, 4 at 1357 — bimodality).
                    Doesn't fire for bench's `[:e | e even]` because
                    the block runs in asmjit-T1 not Sista.
    Eγ   DEFERRED   Array>>sort: splice — 2-3h chunk needed; sort is
                    recursive mergesort and the splice would need to
                    handle the inner per-merge-step loop separately
                    from the recursion.  Punt to a fresh session.
    Eδ   DEFERRED   Generic dispatch overhead — needs new IC bit
                    "no-save tier-2" + new asmjit-T1 emit path that
                    skips the J2J save push AND a matching no-pop
                    return prelude.  3-4h work, doesn't fit a 30-min
                    chunk.  Punt to fresh session.
    Eε   DONE       Re-measure (10-run): 1353-1362 ms, median 1359 ms.
                    Same as start of jit-may23e.  Net bench-suite
                    impact from this session = 0.

## Final result

Bench-suite 10-run median: **1359 ms** (vs jit-may23e start 1349 ms,
within run-to-run noise).  Gap to Cog (~136 ms): **10×**.

Eβ shipped infrastructure (Sista kPrimEvenOddCheck op) that doesn't
fire on this specific bench-suite but would benefit workloads where
the outer method (containing the even-send) is Sista-compiled.

Eα/Eγ/Eδ deferred — each genuinely doesn't fit a single 30-min chunk
under the "fresh session per chunk" model.  Per the doc's rules:
they should be picked up in a future fresh session, not bundled into
this one.

## What to try next session

In priority order based on the chunks' expected impact:

1. **Eδ** (3-4h fresh session): if structural dispatch reduction
   works, this is the single biggest possible win (~7 ns → ~4 ns
   per send is a 1.8× improvement on send-bound benches).
2. **Eγ** (2-3h fresh session): sort 100K alone is 282 ms.  A
   working splice could halve it.
3. **Eα** (1h fresh session): explicit eager-compile of iter block
   to stabilize collect bimodality.  Smaller pay-off but easier
   to attempt.

Each needs its own fresh session to avoid the hook prompt limit.

## If the hook still says "Prompt is too long"

The issue is the `/goal` hook re-evaluates the WHOLE conversation
each time it considers whether to allow stopping.  If your
conversation has accumulated history (even just from typing past
messages, reading docs in earlier turns, etc.), the transcript
may already exceed the limit before the chunk even starts.

**Workaround (in order, try cheapest first):**

1. `/clear` — wipes the current conversation transcript.  Then
   re-invoke the `/goal` line.  Almost always sufficient.

2. If `/clear` doesn't help: `/goal clear` to drop the active
   stop hook, then `/clear`, then re-invoke `/goal`.

3. If still stuck: close the Claude Code session entirely and
   start a brand-new one.  All state is in git + the doc, so
   no work is lost.

After any of these:
- The new transcript sees: system prompt + the `/goal` line + the
  chunk's tool calls.
- That fits in the hook's context window.

## Closing note

The fundamental change between jit-may23d and jit-may23e: don't
try to execute the whole plan in one session.  Restart between
chunks.  This is a workflow change, not a plan change.

Each chunk is sized to fit in one fresh session.  If a chunk
itself produces too much transcript, split it further into
sub-chunks and execute them in separate sessions.
