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

    Eα   DEFERRED     Stabilize collect bimodality — previous interp-mode
                      attempt invalidated.  Now that JIT runs (E2 fix,
                      xmethod-default-OFF workaround landed 5190c72f),
                      this should be re-attempted with real measurements.
                      First step: capture variance across 10 runs of
                      "collect 10x100K" — current single run = 242 ms,
                      vs Cog 5 ms.
    Eβ   NEEDS-VERIFY "Shipped" Sista-side even/odd inline (kPrimEvenOddCheck)
                      now executes under JIT; whether it actually fires on
                      the bench is unknown.  Re-measure.
    Eγ   DEFERRED     Array>>sort: splice — unblocked, fresh session.
    Eδ.1 ✅ DONE      Added canSkipJ2JSave flag to JITMethod.  20.3% of
                      real-compiled methods qualify (312/1539 on image boot).
                      Commit 0e6e9808.
    Eδ.2a ✅ DONE    Counter for qualifying methods.  Commit 96277cc1.
    Eδ.2b ✅ DONE    Counter for IC-HITs that route through a
                      canSkipJ2JSave callee.  56K hits per image boot.
                      Commit 0beec702.
    Eδ.2c ✅ DONE-as-INFRA  Saveless inline-J2J emit landed (commits
                      bfff3b6d + 69faa3b2).  Positioned BEFORE the warm-
                      J2J gate (safe because canSkipJ2JSave callees have
                      numICEntries==0, can't trigger the materialize-bail
                      wrong-result bug the gate guards against).  Bit-56
                      self-rec gate inside saveless emit prevents
                      cross-method corruption (OSPlatform DNU bug fixed).

                      Outcome: emit infrastructure works but fires=0 on
                      bench-suite because canSkipJ2JSave and self-rec are
                      mutually exclusive in practice:
                        - canSkipJ2JSave requires numICEntries==0
                        - Self-recursive methods (fib, etc.) have ICs
                          for their own recursive sends
                      So the saveless path never matches a real call.

                      To make it useful, would need either:
                        (a) Cross-method saveless with state.{method,
                            literals,jitMethod,argCount} save/restore
                            around blr (≈8 extra ldr/str, but enables
                            leaf-method calls from warm callers — the
                            real intended use case).
                        (b) Extend canSkipJ2JSave to allow methods whose
                            ICs only target other canSkipJ2JSave methods
                            (transitive leaf analysis).

                      Both deferred to fresh sessions.  Default config
                      bench-suite: identical perf, no regression.
    Eε   PARTIAL      First-ever real JIT-on bench-suite numbers
                      obtained this session, with PHARO_JIT_THRESHOLD=1100
                      workaround for the SessionManager-handler-dispatch
                      regression (otherwise handler never fires →
                      bench code never runs → output file empty).

                      Single-run numbers (Cog 10.3.9 ref / our VM):

                        bench                    Cog       Ours      ratio
                        tinyBenchmarks (M s/s)   462M      92M       5.0×
                        fibonacci(28)              2 ms   110 ms    55×
                        sieve x100                 7 ms     7 ms     1×
                        sort 100K                 16 ms   318 ms    20×
                        dict 50K put+get          20 ms   342 ms    17×
                        sum 1M                     4 ms    99 ms    25×
                        5000 factorial             3 ms    22 ms     7×
                        1M blocks                  5 ms     2 ms    0.4×
                        1M getter+yourself         1 ms   104 ms   104×
                        100K allocations           2 ms     5 ms   2.5×
                        floatSum 1M                8 ms   117 ms    15×
                        stringHash 100K            7 ms    76 ms    11×
                        collect 10x100K            5 ms   111 ms    22×
                        select 10x100K            36 ms   281 ms     8×

                      Totals: ~113 ms Cog / ~1626 ms ours = **14× gap.**

                      Previous doc claim of "1359 ms median bench-suite"
                      was interp-mode (JIT silently disabled).  Real
                      JIT-on number now anchored at ~1626 ms.

                      Caveats:
                      - Threshold=1100 leaves boot-warm methods in
                        interp; full-JIT would be different (and is
                        blocked on the handler-dispatch regression).
                      - Single run, no run-to-run variance recorded.
                      - 1M getter+yourself = 104ms is anomalously bad
                        (Cog=1ms, 104× gap) — worth a focused look at
                        the getter-inline emit when next session runs.

                      Side-effect of the workaround: at threshold=1100,
                      block-value inline never fires (1214 tries, 0
                      hits during a focused 1M-iter getter bench).
                      Block compilation lags far behind method
                      compilation because blocks share the threshold.
                      So the 14× gap is INFLATED — under real JIT
                      (threshold=2, no handler regression) BV inline
                      would fire and many benches would close
                      substantially.  How much is unknown until the
                      handler-dispatch regression is fixed and we can
                      measure with threshold=2.

                      Possible quick win once unblocked: a SEPARATE
                      JIT threshold for blocks (lower than methods)
                      so the loop-body block in `1000000 timesRepeat:
                      [...]` compiles after a few dozen iterations
                      rather than after 1100.

## ✅ JIT silently disabled / X+BV crash — RESOLVED 2026-05-24 (session E2)

Two stacked root causes, both fixed in working tree:

1. `JM_SIZE`/`JM_BCTOCODEOFF` drift in `TrampolineAsm.S` since commit
   `3e625350` (hasNLR field add) — JIT silently failed init since then.
2. `AsmjitT1.cpp` cross-method `state.method` update happening BEFORE
   the J2J save-full bail check — bails left state.method = calleeCM,
   which the bail emits then combined with `bcOffsetFromMethObj` to
   land state.ip at an unrelated heap address.

Verified by full image boot (~126K xmethod inline-J2J fires) and
`scripts/bench-correctness.sh fib 20 28` (both PASS).  See
`docs/deferred.md` "✅ JIT silently disabled — RESOLVED" entry for
the full diagnosis.

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

## 2026-05-24 — interp block-body fast paths (cumulative)

Stacking five fast paths in primitiveFullClosureValue + commonSend
that recognize common trivial block bodies and short-circuit them:

1. **evenOdd** cache fast path (edcf20bb): `Integer>>even/odd` for
   SmI receivers decided from tagged-bit pattern.
2. **2-arg cmp** block (35bb73d2): `[:a :b | a cmpOp b]`.
3. **2-arg arith** block (3e6e0f1c): `[:a :b | a +/-/* b]`.
4. **1-arg arith** block (1ebc4238): `[:x | x op K]` for SmI lit K.
5. **1-arg SEND0** block (01415c5d): `[:x | x SEND]` via
   sendSelector, skips activateBlock.

Cumulative bench-suite wins (median of 5 runs, default config):

```
bench              before   after   delta
sum 1M              150 ms  108 ms  -28%
collect 10x100K     106 ms   63 ms  -41%
select 10x100K      554 ms  425 ms  -23%
sort 100K           ~250 ms ~265 ms  bimodal — variance dominates
dict 50K            ~210 ms ~155 ms  bimodal — variance dominates
stringHash 100K      73 ms   68 ms   -7%
1M blocks           358 ms  375 ms   slight regression (~5%)
fib(28)             110 ms  110 ms    0
```

~250 ms saved per bench-suite run (~15% of total bench-suite time).
Cog gap closing: select 10x100K 12× → 9×, collect 21× → 12×.

The slight regression on `1M blocks` may be a fast-path-overhead
issue on blocks that fall through to activateBlock, OR run-to-run
noise on this bench (closure allocation dominates).  Worth a focused
investigation next session.

## 2026-05-24 — interp evenOdd + returnsLiteral fast paths

Now that the OSR-disabled root cause is identified (see below), the
next-best lever is interp dispatch speed.  Most bench-suite execution
happens in interp because methods with sends can't OSR.

Added two new fast paths to commonSend's method-cache dispatch:

1. **evenOddKind fast path** (edcf20bb): for SmI receivers, decide
   `x even` / `x odd` from the tagged-bit pattern without method
   activation.  Hits ~1M times on `select 10x100K`.

   **Real win**: 549-563ms → 464-466ms (~15% improvement, stable
   across 5 runs).

2. **returnsLiteral fast path** (a21968c2): for `^ nil/true/false/0/
   1/-1/2` shapes, push the literal directly.  Guarded against
   methods with primitives (the literal is a fallback if prim
   fails — Time millisecondClockValue caught this).

   No measured bench impact yet; useful for general code paths.

Both fast paths require MethodCacheEntry field additions (evenOddKind,
returnsLiteralKind) populated from detectTrivialMethod.

## 2026-05-24 — Cached primitive function pointer + 0-arg block fast path

Two more small wins to round out the session:

1. **Cached primitive pointer** (commit ee95e3eb): MethodCacheEntry
   already had a `primitive` field but it was always nullptr.  Now
   populated with primitiveTable_[primIdx] in cacheMethod, and
   commonSend calls it directly bypassing executePrimitive's
   primitiveTable lookup + debug-flag branches.  Modest.

2. **0-arg block `[x := x op K]` fast path** (commit 8e58e7fe):
   recognizes 9-byte body PushTempAtInVec/push K/ArithSend/
   StoreTempAtInVec/return.  Used by `[x := x + 1] value` style
   blocks.  Reads/writes captured tempVec directly without
   activating the block; storePointer handles the write barrier.

Bench impact: modest, mostly within noise.  Both code-quality wins
that incidentally improve some cases.

## 2026-05-24 — OSR drive-from-asm investigation (no commit)

Investigated wiring up J2J save pool + chain-loop dispatch in the
OSR path so JIT-inlined sends don't fall through to activateMethod
(which pushes real frames).  Two attempts:

1. **Set state.j2jSaveCursor to a j2jPool_ slice** in tryJITResumeInCaller
   when FORCE_RESUME_FOR_SENDS=1.  Reverted (commit-free) — fib still
   hangs with frameDepth=4096 during session startup, not during the
   bench.  The 4096 frames are nested `Array>>do:` calls.

2. **Enable PHARO_RESUME_J2J=1** chain-loop (existing flag, default off)
   alongside FORCE_RESUME.  Same hang — Array>>do: still recurses 4096
   deep.

Root cause appears to be a JIT correctness issue specific to
mid-method OSR entry: JIT-compiled `Array>>do:` produces semantics
that differ from the interpreter, causing nested recursion that
doesn't terminate.  Most likely cause: the JIT emit for the
SistaV1 inlined `to:do:` counted-loop assumes a stack/temp state
that the OSR target doesn't quite match.

Conclusion: drive-from-asm dispatch alone isn't enough.  To fully
unblock OSR for send-containing methods needs:

1. Audit each bytecode emit's stack/temp assumptions at OSR entry
   points, OR
2. Re-emit a per-bytecode "stack reload" prologue when entered via
   OSR rather than method-start.

Both are multi-hour design tasks.  The state-sync fix (commit
2d9da6ee) plus the runtime guard against unsafe CallPrimitive-skip
(part of same commit) remain valid improvements.  Default config
unchanged.

## 2026-05-24 — OSR state-sync second pass (2d9da6ee)

Added missing JITState field init in tryJITResumeInCaller:
trueOop, falseOop, sistaSaveCursor/Limit/Depth/EntryDepth,
cachedTarget, returnValue, sendBCLength, simTOS, simNOS,
spliceSpill0/1.  Mirrors what tryJITActivation sets but the
OSR path was missing them.  Without these, JIT inline-prim emit
pushed NULL Oop for comparison results, crashing downstream.

Also tightened the detectTrivialMethod CallPrimitive-header skip
to fire only for quick prims (primIdx >= 256).  For lower prims
like 251 (millisecond clock), the fallback bytecodes
`pushZero; returnTop` don't match the prim's semantics — inlining
the fallback would make Time millisecondClockValue return 0 forever,
breaking timing benches.

With FORCE_RESUME_FOR_SENDS=1:
- Simple benches (getter+yourself) now complete WITHOUT crash.
- Recursive benches (fib 20/28) still hang — fib's 514k recursive
  calls overflow the frame stack because tryJITResumeInCaller
  sets state.j2jSaveCursor=null, forcing every inline-J2J push to
  bail to ExitSendCached → activateMethod → real frame push.
  Default mode avoids this via the trampoline + j2jPool_ inline
  save mechanism, which tryJITResumeInCaller does NOT wire up.

To fully unblock FORCE_RESUME (and hence the ~5-10× perf opportunity):
1. Set up state.j2jSaveCursor/Limit to a valid slice of j2jPool_
   in tryJITResumeInCaller (mirror tryJITActivation's setup).
2. Preserve the save cursor across the resume loop iterations
   (currently the JITState is recreated per iter, losing cursor).
3. Drain ExitJ2JCall the same way the trampoline does (chain-loop
   push + JIT_CALL into callee).

Multi-hour fresh-session investigation.  Until then, FORCE_RESUME
remains off-by-default.

## 2026-05-24 — OSR resume wrapper (f771852a)

Added `pharo_jit_osr_resume` asm wrapper that hoists x19 = state.
jitMethod and x20 = state.j2jDepthInc before calling JIT entry code.
Without this, tryResume's bare `entry(&state)` left x19/x20 stale,
causing inline-J2J IC HIT loads to deref garbage.

Confirmed via test: with FORCE_RESUME_FOR_SENDS=1, crash address
changed from 0x69 (state-deref) to 0x0 (NULL recv), proving the
wrapper does its part.  But other state-sync issues remain — the
full FORCE_RESUME path crashes in #whileFalse: with NULL receiver
load.  Stack-state mismatch between interp and JIT at mid-method
OSR entry; a stack-shape audit + reconciliation is needed.

## 2026-05-24 — ROOT CAUSE of getter+yourself anomaly: OSR disabled for sends

Traced tryResume failures via diagnostic counters.  Found in
AsmjitT1.cpp:6403:

```
bool advertiseResume = isReal && !noNumBc && !noBcToCode && bcLen > 0;
if (numSendSites > 0 && !forceResumeForSends) advertiseResume = false;
jm->numBytecodes      = advertiseResume ? (uint16_t)bcLen : 0;
```

**Methods with ANY send sites get numBytecodes=0 → all tryResume
calls return `bad-codeOffset`.**  OSR can't transition into the
JIT'd version mid-method.  This means:

- Test methods like `runIt` (with `pt x` send sites) JIT-compile
  but the JIT'd version is unreachable via OSR.
- The bench's first activation runs entirely in interp because
  the JIT compile happens at safe-point AFTER the activation
  started.
- A second call would use the JIT'd entry point — but runAsync
  only calls runIt once.

Comment at AsmjitT1.cpp:6388 documents the cause: "Still breaks
the differential fuzzer (every test JIT_DIFF, MUSTBOOL cascade in
#encoderClass, eventual stack overflow).  More than state.method
is desync'd at the trampoline — investigation deferred."

`PHARO_ASMJIT_T1_FORCE_RESUME_FOR_SENDS=1` bypasses the gate but
crashes/hangs per the comment.

**Impact**: This is the major perf gap.  Bench-suite gap to Cog
(~10× median) is largely because OSR-to-JIT doesn't work for
methods with sends.  Once the state-sync bug is fixed,
single-activation benches would benefit dramatically.

Investigation needed (next session, fresh context):
1. Add OSR-success counter alongside jitOSREntries_ to verify how
   often OSR-into-sendless methods actually fires successfully.
2. Bisect the state-sync issue using FORCE_RESUME_FOR_SENDS=1 +
   the differential fuzzer.  Likely candidates: state.method
   vs jitMethod desync at J2J entry/return; some register left
   stale across the resume trampoline.
3. Once fixed, this could be the largest single perf unlock —
   potentially 5-10× on send-heavy benches.

## 2026-05-24 — detectTrivialMethod CallPrimitive prefix fix (445e49fd)

Found that methods like `Point>>x` declared with `<primitive: 264>`
weren't classified as trivial getters because the detection only
matched against the FIRST two bytes (bc0 = 0xF8 CallPrimitive, not
0x00 pushRecvVar).  Quick-prim slot getters use prim 264+N
(primitiveLoadInstVar variants) with a fallback `pushRecvVar N;
returnTop` body — same shape as a non-primitive getter, just
preceded by the 3-byte CallPrimitive header.

Fix: skip the 3-byte CallPrimitive header in `detectTrivialMethod`
when bc0 == 0xF8.  Existing shape matches then apply uniformly.

Verified via IC-PATCH trace: `Point>>x` now classifies as
getter=0 (slot 0), was getter=-1.

This unlocks bit-63 inline-getter classification for many trivial
methods across the system (anything declared with `<primitive: N>`
and a trivial fallback).  1M getter+yourself bench dropped from
~35 ms to ~33 ms (limited gain because the bench loop runs mostly
in interp; the fix benefits sites that do reach the JIT path).

## Investigation 2026-05-24 (this session) — getter+yourself anomaly

Bytecode dump (Pharo 13 compile of `1 to: 1000000 do: [:i | pt x]`) at
`docs/getter_bench_runIt_bytecode.txt`.  Key observation: byte 142 =
0x7E = SpecialSend `x` (special selector 30).  Loop body is bytecodes
141-143 (PushTemp pt, SpecialSend `x`, Pop).  Counted loop machinery
follows.

The send IS routed through asmjit-T1's IC HIT emit (isPhase4SendOp
covers 0x70-0xAF).  Inline-getter (bit 63) tryGetter fires only ~880
times for 3M iterations.

Hypotheses to investigate next session:
- The IC for `pt x` site never gets bit 63 classified (Pharo's
  `Point>>x` should be a trivial getter — verify via
  `PHARO_DUMP_RECOMPILE_IC=runIt` then look at the extra word).
- `runIt` is real-compiled (bcLen=67, not in BC-DUMP failures) but
  perhaps OSR doesn't actually transition INTO the JIT'd version,
  leaving the loop in interp.
- The 35 ms / 1M iter = 35 ns/iter looks more like interp than JIT
  (JIT would be ~5 ns/iter for this loop shape).

## Investigation 2026-05-24 (this session)

Spent extending Eδ.2c into a working saveless emit (3 commits:
bfff3b6d, 69faa3b2, 27c68e76) plus diagnostic counters (9f8e799d).
Outcome: infrastructure landed, but fires=0 on bench-suite in
default config because canSkipJ2JSave+self-rec are mutually
exclusive (qualifying methods have no ICs → can't be self-rec).

Then investigated the 1M getter+yourself anomaly (35× gap) and
discovered:
  - The bench's `pt x` loop runs MOSTLY in interp, not JIT.
  - Only ~880 inline-getter fires across 3M iterations.
  - Total IC HITs ~92K — way below the 3M expected.
  - PHARO_JIT_THRESHOLD=2, PHARO_JIT_DEFER=0, PHARO_QUEUE_COMPILE=1
    all produce identical timing → JIT compile delay isn't the
    fix.  Something more fundamental keeps the loop in interp.

Investigated bench-suite bimodality:
  - sort 100K alternates 198 ms vs 297 ms run-to-run.
  - dict 50K alternates 154 ms vs 257 ms — INVERSE to sort.
  - PHARO_NO_LATE_SPEC_RECOMPILE=1 + PHARO_NO_J2J_INLINE_BUMP=1
    (kill all recompile triggers) does NOT eliminate the bimodality.
  - mergeFirst:middle:last:into:by: compiles 3 times (#13 boot,
    #1000 first-recompile, #1001 late-spec recompile).  Expected
    per design but adds noise.
  - Bimodality is some other source.  Possibly L1/L2 cache state
    or scheduler-affected JIT compile ordering.

## What to try next session

In priority order based on the chunks' expected impact:

1. **1M getter+yourself investigation** (1-2h fresh session):
   the loop body should be inline-getter (4 instrs/iter) but the
   bench shows 35ms = ~12ns/iter, suggesting interp dispatch.
   Add a "send #x bytecode → tryGetter" trace counter to confirm
   what path is taken.  If interp, why does OSR not transition?

2. **Cross-method saveless** (2h fresh session): extend Eδ.2c
   to handle cross-method (non-self-rec) callees by saving+
   restoring caller's state.{method,literals,jitMethod,argCount}
   to sp-stash.  Would let saveless fire in default config since
   canSkipJ2JSave callees are leaf methods called from warm
   callers.  Net cost vs J2J save: +~10 instrs but +better branch
   prediction.  Likely modest win.

3. **Eα** (1h fresh session): explicit eager-compile of iter block
   to stabilize bimodality.  But this session found bimodality
   is broader than collect (sort, dict also bimodal) — investigation
   needed before action.

4. **Eγ** (2-3h fresh session): sort 100K alone is 197-300 ms.
   A working splice could halve it — assuming the bimodality
   doesn't dominate the signal.

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
