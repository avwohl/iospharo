# JIT branch retrospective (2026-06-09)

Forensic review of the `jit` branch (2,677 commits ahead of `main`, ~2 months).
Method: 12-agent workflow (git forensics, bug taxonomy, architecture, x86-breakage,
validation loop, redefinition-hypothesis test, production-JIT comparison) +
3 adversarial critics + independent verification of every load-bearing claim.

## Bottom line

The branch is NOT doomed; the way of working is. Do NOT reset — the interpreter was
already ~99.9-100% SUnit-correct (12489/0, `fd62d4a6`, 2026-02-11) SEVEN WEEKS before
the JIT push began (`31751891`, 2026-04-02). Cache invalidation on redefinition is
correct; the ASLR header design is sound. The entire JIT effort is a discretionary
speed project layered on a finished, correct VM.

Worst-of-60 root, in one line: optimized 38 days (58% of commits) with NO correctness
oracle, then built the wrong one, then diagnosed its own architectural root cause in
writing and never implemented the fix.

## The user's hypothesis ("Smalltalk redefines everything > token space")

REFUTED (literal form). 0 of 2677 commits mention "redefin"; become/metaclass ~2 each.
The predicted failure (redefined method -> stale JIT code) appears nowhere because
every cache-flush primitive in Primitives.cpp pairs flushMethodCache()+flushJITCaches()
(472, 8357, 9450, 9761, 11067, 13301). Every hard bug had a TINY repro (NLR = 3 lines;
asmjit-imm = one `orr x,#5`). Bugs were DEEP and timing-dependent, not WIDE.

PARTLY CONFIRMED (two misattributed instincts survive):
- Token wall is real: ran on 1M-context Opus and STILL hit "Prompt is too long"
  (`b96446ab`, `244bb289`). 775/2677 commits (29%) are pure `docs:` — more than the
  ~395 that fix code. State-externalization to survive session resets. Cause was the
  debugging-transcript length of multi-session hunts, not Smalltalk syntax.
- Whole-system entanglement is real but RUNTIME-semantic: the det-sched/runner-only
  bug family (no standalone repro for ~25 commits) where the image reflectively
  re-implements its own execution (Context>>step, lookupSelector:) under a forked,
  preemptively-scheduled test. Metacircular concurrent runtime, not source redefinability.

## Fundamental problems (ranked, all independently verified)

1. Optimized first, validated never — then validated wrong. jit-diff (`932cab5c`,
   day 38) compared only `tail -1` of two whole-image runs over a 56-line corpus,
   using the BUGGY interpreter as ground truth (run.sh:5), never stock Cog. Corpus
   SHRANK 677->56; tool abandoned in 5 days. Master cause — sets bug-find time, makes
   "fixed" unfalsifiable.
2. Diagnosed the architectural root cause, didn't implement it. plan_asmjit_replacement.md
   prescribes one StackHelper owning stack discipline (12-15K line target). VERIFIED at
   HEAD: StackHelper.cpp does NOT exist; JITCompiler.cpp still 152KB (Phase 7 never cut
   over); AsmjitT1.cpp = 7280 lines (5x target); jumps default-OFF (`:883`);
   FORCE_RESUME_FOR_SENDS "KNOWN BROKEN" (`:6934`). The rewrite became a FOURTH backend.
3. Measurement-integrity collapse. 102 retract/fabricate commits (19 in final 2 weeks);
   J2J merged-on-speed then reverted-on-correctness (`da4bafd0` -> `10aa6c80`); dev build
   accidentally -O0 (~9x slow), so every prior custom-vs-Cog number was on a crippled VM.
4. Timing Heisenbugs in a metacircular concurrent runtime — largest hard-bug class,
   subset has no standalone repro. PHARO_DET_SCHED was a late opt-in retrofit.
5. Flag-bisection substituting for understanding: 448 PHARO_* knobs (verified), no IR
   verifier, zero assert() in the Sista IR path.

Note: the "4 jit branches" are ONE trunk — jit-x86 is fully contained in jit (0 unique
commits, jit +78); jit is only 46 ahead of jit-arm-linux. Freezing them is cheap.

## How to work differently (survived the red-team)

The obvious advice is a trap: "finish the consolidation" IS plan_asmjit_replacement.md,
already executed = the churn under review; "build a CSmith fuzzer" hands a big artifact
to a loop that abandons big artifacts (corpus 677->56). The failure mode is an executor
that doesn't sustain artifacts and grades its own homework. So: behavioral, small-first,
external forcing functions over self-policed rules.

0. MEASURE whether the JIT is even on the hot path before building anything (jumps
   default-OFF, sends KNOWN BROKEN). Two counters in AsmjitT1 (emit vs bail), one SUnit
   class, print the ratio. If <50% hot-bytecode coverage, fix coverage before optimizing.
1. Make the EXISTING stock-Cog SUnit run the merge gate; stop merging on speed. Make
   fabricated numbers impossible: results-writer asserts child exit 0 + expected line count.
2. Add a stock-Cog third column to the existing jit-diff with a NORMALIZED compare
   (strip identity-hash, canonicalize Float/Set/Dict printString). Catches deterministic
   single-expr bugs; will NOT catch concurrency Heisenbugs. Re-grow corpus toward known
   bug families first. Do NOT aim for "bit-identical to Cog" (unwinnable — two VMs never
   produce identical heaps).
3. Run PHARO_DET_SCHED as a SECOND mode, not the default (it freezes one interleaving and
   hides wall-clock bugs). Run each gate twice; flag classes that fail under one only.
4. FREEZE x86/arm-linux; finish arm64. Start by actually building StackHelper.cpp. Success
   = conditional jumps flipped default-ON, passing the Cog-diff subset. Cross-arch fuzzing
   is premature (x86 ~52% compile-rate, 92% store_ivar bail -> divergence is bail-noise).
5. Reframe goal from "as fast as Cog" (~18x activation gap is structural) to binary
   correctness done-conditions ("class X 5/5 deterministically", "0 jit!=cog on corpus").

Deepest point: a CLAUDE.md policy line is the weakest lever — the existing "No workarounds"
rule is already contradicted by 448 knobs. Only EXTERNAL forcing functions (a merge gate
that mechanically rejects regressions; a writer that refuses unverified numbers) and
radically smaller scope (one finished backend) actually hold across token-limited sessions.

## Step 0 EXECUTED 2026-06-09: is the JIT even on the hot path? NO.

Method: build-opt (RelWithDebInfo, NOT the -O0 trap). `test_load_image <img> eval "<expr>"`
with JIT default vs `PHARO_NO_JIT=1`. Probe = recursive `benchFib` (DCE-proof: ~7M real
activations, sends + arithmetic + a conditional jump; cannot be inlined away). Reference =
stock Cog on /tmp/harness/pharo. Three steady-state runs each.

    fib32, ms (lower = faster)        run1   run2   run3
      Stock Cog JIT                     15     16     15
      OUR interpreter (PHARO_NO_JIT)   459    469    468     ~30x slower than Cog
      OUR JIT (default)                470    491    486     ~31x slower; SLOWER than our own interp

Verdict: the JIT provides ZERO net speedup over the pure interpreter — it is 3-5% SLOWER on
fib, every run — and both our paths are ~30x off the reference JIT for this same VM family.
The "step 0" done-condition (JIT-execution coverage <50% of hot bytecodes => stop optimizing)
is met decisively: effective coverage is ~0% for any method with control flow.

Supporting stats (dumpJITStats, real bench run): compiled 3474 / FAILED 70069 (95% compile-fail);
code cache MAXED 16383/16384 KB; `resume (J2J-r): 0/0` (resume-after-send never fires);
`chain: actChain=7 actFall=12048` (activation chaining falls back to interp 99.9% of the time);
IC hit 61.8%.

Root mechanism, confirmed at source (not inferred):
- AsmjitT1.cpp:882 — "Conditional jumps remain DISABLED by default. Enabling them
  (PHARO_ASMJIT_T1_ENABLE_JUMPS=1) causes ... DNU on garbage class indices — a JIT-side
  emit bug (corrupted stack or wrong jump target)." So a method with ANY `ifTrue:`/`and:`/
  `whileTrue:` bails to the interpreter at its first branch.
- AsmjitT1.cpp:6934 — FORCE_RESUME_FOR_SENDS tried 2026-05-15, left OFF. After a send,
  native execution does not resume.
Since nearly every real method has control flow and sends, the JIT only ever natively
executes the straight-line prefix up to the first branch/send, then bails — and the
prologue+bail overhead exceeds any gain, hence net-negative.

CONSEQUENCE for the plan: do NOT optimize codegen. The bottleneck is the send/return/branch
hot path (disabled jumps + dead resume), not codegen quality. Fix the hot path (conditional
jumps emitting correctly + resume-after-send) BEFORE any further optimization, OR accept the
interpreter as the product. Every prior perf optimization polished a component that, for any
method with an `if` in it, never runs.

## Send-resume investigation + fix EXECUTED 2026-06-09 (fib A/B gate: FAILS)

Goal: "make conditional jumps work, gate on the fib A/B." Findings, in order:

1. Conditional jumps ALREADY WORK. AsmjitT1 emits real native branches for conditional
   jumps on arm64 (AsmjitT1.cpp:3399-3441) and x86 (1772), `t1EnableJumps=true` by default
   (flipped on 2026-05-16). The line-882 "DISABLED" comment is stale. Not the bottleneck.

2. The real fib lever is SEND-RESUME (jm->numBytecodes/bcToCodeTable re-entry after a send).
   It's gated OFF for send-methods by default (AsmjitT1.cpp:6973). FORCE_RESUME proves it's
   the lever (J2J-r 0/0 -> 35.6%) but it was BROKEN (runaway recursion, "DNU on garbage
   class indices").

3. ROOT CAUSE of one resume desync FOUND + FIXED (TrampolineAsm.S:~248): the trampoline's
   mid-method J2J-return resume (`Lret_after_restore`, `blr x7`) re-entered the caller's
   native code WITHOUT reloading x19 (=state.jitMethod) / x20 (=j2jDepthInc). Those are
   hoisted once at the codeStart prologue, which mid-method resume bypasses; the just-returned
   callee clobbered them. So the resumed caller's IC path read a STALE jitMethod -> wrong
   icBuffer -> garbage class. Fix = reload x19/x20 before the blr, exactly as the already-
   correct `pharo_jit_osr_resume` (TrampolineAsm.S:476) does for the C++ resume path. Default
   path unaffected (send-methods don't advertise resume, J2J-r 0/0). Validated correct in
   isolation via new debug knob PHARO_T1_RESUME_ONLY_SEL=benchFib (startup survives, fib
   results correct).

4. A SECOND desync REMAINS (not fixed): with resume forced on broadly, startup still dies with
   an operand-stack desync (wrong receiver: "#delimiter sent to false", rcvr cls=False) in the
   send + conditional-jump combination. So general send-resume is still unsafe; the x19/x20 fix
   is necessary-but-not-sufficient.

5. fib A/B VERDICT: FAILS. Even with resume working (isolated to benchFib, correct), the JIT
   does NOT beat the interpreter on fib:

       fib32, ms (clean, 3 runs)        fib30 median (5 runs)
         default-JIT (resume off)  ~540        ~203
         isolated-resume (the fix) ~558        --      (marginally SLOWER than resume-off)
         pure interpreter          ~516        ~188    (FASTEST)
         stock Cog                 ~15

   Resume-ON is marginally SLOWER than resume-OFF — routing MORE of fib through the JIT's send
   path makes it slower, proving the per-send machinery itself is the bottleneck. Stats confirm:
   J2J stencil calls=650K against ~30M benchFib calls, inline-J2J=0 — fib's recursive calls
   mostly DON'T use the fast JIT-to-JIT path; they fall through to heavy chain-loop activation,
   which costs more than the interpreter's send. The bottleneck is UPSTREAM of resume.

DEEPEST CONCLUSION: the JIT's "exit to trampoline / C++ chain loop per send" + J2J-not-firing
architecture makes a send cost MORE than the interpreter's send. So the JIT cannot beat the
interpreter on send-heavy code (fib, and most real Smalltalk) regardless of resume or codegen.
Passing the fib A/B requires the SEND PATH itself to be cheap — true native call/return between
JIT methods (Cog-style: `bl` to callee codeStart, `ret` back, no trampoline round-trip), which
is a major architectural change, not a bug fix. The x19/x20 trampoline fix is a correct,
foundational prerequisite kept in tree; it is inert in the default build until send-resume is
made safe (fix desync #4) and worthwhile (fix the send-path cost).

## The native-call path ALREADY EXISTS (inline-J2J) — located 2026-06-09

The "Cog-style native call/return" lever is NOT a from-scratch rewrite. It already exists as
**inline-J2J + xmethod** (the JIT_CALL macro emits a direct caller->callee branch, bypassing the
trampoline dispatch loop). DebugSettings.cpp:156-173 documents its measured payoff and why it's
off:

    INLINE off + BV off  (current default):   634/634 SUnit PASS,  76 ms fib28
    INLINE on, XMETHOD on, BV off:            633/634 SUnit PASS,  13 ms fib28  (~6x; Cog ~15ms)

So inline-J2J gets fib to ~Cog speed — the fib A/B is PASSABLE with it. It is disabled by default
(t1InlineJ2J = envPresent("PHARO_T1_INLINE_J2J"), i.e. OFF unless set) purely to dodge correctness
bugs:
  - eval/headless STARTUP dies under inline-J2J with a GARBAGE RECEIVER (rcvr cls=0) in
    BitBlt #copyBits:from:at:clippingBox:rule:fillColor:map:. CONFIRMED PRE-EXISTING: the old
    build/ binary (predating the x19/x20 fix) fails identically, so the x19/x20 fix is exonerated.
  - the documented 1-test SUnit gap is CharacterTest>>testStoreStringAll: "parseAssignment gets a
    non-Boolean OCIdentifierToken from a JIT-emitted conditional".

These are the SAME corruption class as the resume desync #4 ("#delimiter sent to false"): a
conditional-jump + J2J/resume interaction corrupts the operand stack / a value, surfacing as a
wrong receiver or non-Boolean. Strong hypothesis: ONE shared root cause (SP/operand accounting at
the cond-jump boundary across a J2J save/restore or resume). Fixing it would unblock BOTH inline-J2J
(fib ~6x, near Cog -> fib A/B PASSES) AND general send-resume.

UPDATE 2026-06-09 (same session, after the x19/x20 commit 4e61de95): attempted to isolate inline-J2J
to one selector (a 2-site g_t1InlineJ2JActive gate over g_debug.t1InlineJ2J reads in emitOne_arm64).
RESULT: insufficient — inline-J2J did NOT engage (benchFib timing unchanged: fib30 ~208ms isolated vs
~204 default vs ~172 interp). inline-J2J's actual call-inlining is gated by SEVERAL interacting
conditions (warm gate, pure gate, IC bit-60 / J2J_ENTRY_BIT, xmethod, the j2jDepth machinery), not the
two sites a simple knob flips. The incomplete knob was reverted; only the committed x19/x20 fix +
PHARO_T1_RESUME_ONLY_SEL remain. Also re-confirmed: the trampoline resume path is CORRECT for
non-self-recursive J2J + cond-jump (a jtest->jbar+ifTrue:ifFalse: repro returns correct results under
PHARO_T1_RESUME_ONLY_SEL); the corruption is specific to the inline-J2J emit, not trampoline resume.
Net: enabling inline-J2J correctly = understanding/threading ALL its gates AND fixing the
non-self-recursive corruption (copyBits) — genuinely multi-session.

REVISED PLAN (native-call lever): it is a CORRECTNESS fix, not a rewrite.
  1. Map ALL inline-J2J engagement gates (warm/pure/bit-60/xmethod/j2jDepth) first, then build a
     working per-method isolation; get a minimal, non-graphics, deterministic repro of the inline-J2J
     garbage-receiver / non-Boolean bug (or drive CharacterTest>>testStoreStringAll; PHARO_DET_SCHED
     for any timing component).
  2. lldb the cond-jump + J2J boundary: confirm the SP/operand desync, find whether the cond-jump's
     boolean-pop or the J2J save/restore mis-accounts by a slot.
  3. Fix the shared root cause; gate on (a) fib A/B (JIT < interp, approaching 13ms) AND (b) the
     copyBits startup path AND CharacterTest both clean, then flip inline-J2J + send-resume default-on.
The x19/x20 trampoline fix is the first correct piece of this and stays in tree.
