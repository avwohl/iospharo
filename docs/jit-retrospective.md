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

## inline-J2J ENGAGEMENT GATE MAP (verified 2026-06-09, supersedes the "multi-gated" note above)

Mapped via workflow (3 parallel readers + synthesis + adversarial verify) and hand-verified +
empirically confirmed. CORRECTION to the note above: engagement does NOT need many knobs — the
earlier 2-site experiment failed due to an unwired g_t1InlineJ2JActive isolation WRAPPER (now
reverted), not because the gates are unreachable. PHARO_T1_INLINE_J2J=1 alone DOES engage inline-J2J
(empirically: inline-J2J hits=1237 during startup; and the repo's own 633/634 SUnit / 13ms fib28 =
the engagement). The real barrier is CORRECTNESS, not engagement.

The complete AND-chain — ALL must hold for the direct caller->callee branch to fire at a send site:

  COMPILE-EMIT (at JIT-compile of the caller):
    E0  op is a Phase-4 send                                  [AsmjitT1.cpp:3444]   structural
    E1  g_debug.t1ICProbe ON (whole IC-probe block wraps J2J) [DebugSettings.hpp:73] default ON; opt-out PHARO_T1_NO_IC_PROBE  <-- synthesis MISSED this; verified
    E2  g_debug.t1InlineJ2J ON -> emits tbnz x7,#60           [AsmjitT1.cpp:3621-3622,3692; DebugSettings.cpp:174] default OFF; flip PHARO_T1_INLINE_J2J=1  <-- THE primary blocker
    E3  same flag emits the return-prelude J2J pop            [AsmjitT1.cpp:2782-2792]
    E4  callerJM reg provenance (x11 if xmethod else x19)     [AsmjitT1.cpp:3455-3459]
  IC-LIFECYCLE (runtime, BEFORE the recursive send; gated by !g_debug.noJ2J = default ON, NOT by t1InlineJ2J):
    L1  callee JIT-compiled (codeStart exists)                [Interpreter.cpp:20796]
    L2  caller IC slot extra==0 then gets bit60(J2J_ENTRY) + bit56(SELF_REC) via 1 C++ round-trip [Interpreter.cpp:20934->20940,20958; also fill path 20639]  <-- extra==0 precondition: synthesis MISSED
    L3  canJITActivate true: no active materialize-bail unwind [Interpreter.hpp:1236; t1AllowNestedJitBail default false]
  RUNTIME-TAKE (each send):
    R1  IC slot-0 key == receiver class                        [AsmjitT1.cpp:3541-3570]
    R2  extras x7 != 0                                          [3600]
    R3  bit 60 set -> tryInlineJ2J                              [3692]
    R4  no precedence-bit divert (59/55/primKinds)             [3628-3689] (none for bytecode-only fib)
    R5  self-rec discriminator bit56 (self-rec passes both xmethod on/off; cross-method needs xmethod + leaf sub-gates 3951-3971) [3945 / 4007]
    R6  saveless gate skipped (t1CanSkipJ2JSave default OFF)    [4033]
    R7  warm/pure caller-IC gate NOT emitted (BOTH default OFF) [4177-4211; DebugSettings.cpp:199,210]  <-- both default OFF; "warm default-on" comment is STALE
    R8  J2J save-stack not full                                 [4253-4259]
    R9  push 56B save, j2jDepth++ (x20), g_inlineJ2J_hits++, br x9 -> callee entry [4374,4518,4521]
  RETURN-SIDE:
    T1  j2jDepth>j2jEntryDepth -> pop save, tail-call resumeAddr(=post-send). entry baseline seeded in Interpreter.cpp (19329 etc.), NOT TrampolineAsm.S as one map claimed.

  DEFAULT BLOCKERS (why it's off): only E2/E3 (t1InlineJ2J default OFF). Everything else holds by
  default for self-rec fib. So `PHARO_T1_INLINE_J2J=1` is sufficient to ENGAGE.

  THE ACTUAL BARRIER (correctness, not engagement): global PHARO_T1_INLINE_J2J=1 miscompiles — a
  garbage receiver (rcvr cls=0) surfaces as `#extent` DNU inside BitBlt copyBits during startup.
  EMPIRICALLY: skipping copyBits via PHARO_J2J_SKIP_SELECTORS does NOT stop the crash -> the
  corrupting method is UPSTREAM, not copyBits itself; the bug is NOT selector-localized. Same
  cond-jump+J2J operand class as the resume desync. Isolation knobs (J2J_SKIP_SELECTORS skips a
  comma-list; there is no inverse "only-this-selector" knob) can't cleanly confine it -> a working
  inverse-isolation (emit J2J for ONE method, correctly threaded through every compile path) is the
  prerequisite for a clean benchFib repro, then lldb the operand corruption.

## inline-J2J ENGAGED + fib A/B PASSED + corruption narrowed (2026-06-09, committed 660c1473)

Built PHARO_J2J_ONLY_SELECTORS (inverse of SKIP: fill J2J bit60 ONLY for listed callee selectors)
at both fill sites. With PHARO_T1_INLINE_J2J=1 (emit global; harmless without bit60) it confines
ENGAGEMENT to one method, dodging the startup crash. RESULTS:

  fib30 (clean): interpreter ~210ms · default-JIT ~191ms · inline-J2J(benchFib) ~30-34ms · Cog ~15ms
  => JIT beats interpreter ~6x on fib, CORRECT (val 2178309; #(55 6765 75025 832040)), startup intact,
     inline-J2J hits=28M, zero bails.  FIRST TIME THE JIT BEATS THE INTERPRETER ON FIB.

This OVERTURNS the "fib A/B fails on architecture" finding: the per-send cost is NOT the wall —
inline-J2J's direct native call/return wins ~6x.  The architecture can win; inline-J2J is off-by-default
only because of a CORRECTNESS corruption (not an architectural limit).

Corruption characterized (toward GLOBAL enablement):
 - Self-recursive inline-J2J is CORRECT across shapes: benchFib, jsumr: (1-arg+cond-jump, 9.2M hits),
   jack: (Ackermann, nested cond-jumps, 1.8M hits) — all correct. The bug is NOT in self-rec.
 - Cross-method (xmethod) inline-J2J only ENGAGES for narrow callee shapes (leaf gates 3951-3971:
   no prim, numICEntries==0, not stub, no cond-jump); arithmetic/cond-jump callees bail_self.
 - The corruption is in the inline-J2J SETUP path (runs when bit60 set + emit on, BEFORE the taken
   branch — confirmed: when global+SELFREC_ONLY crashed, hits=0, so NO branch was taken yet). It is
   SHAPE-DEPENDENT (benchFib's setup is fine; copyBits-path shapes corrupt), NOT self-rec-vs-cross:
   a SELFREC_ONLY gate (bail all cross-method) did NOT prevent the startup crash (reverted).
 - copyBits is the VICTIM, not the corruptor: engaging inline-J2J for ONLY the copyBits send
   (PHARO_J2J_ONLY_SELECTORS=copyBits...) does NOT crash; the corrupting send is a DIFFERENT upstream
   selector during startup graphics (also: SKIP copyBits didn't stop the global crash).
 - Safe-subset mechanism = the ONLY_SELECTORS allow-list (engage inline-J2J for curated hot/recursive
   selectors), NOT a general self-rec gate.

NEXT (global enablement): bisect WHICH startup selector's inline-J2J setup corrupts — capture the
~1237 engaged selectors under global inline-J2J, binary-search by disabling halves via
PHARO_J2J_SKIP_SELECTORS until the crash flips, then lldb that one method's tryInlineJ2J setup
(AsmjitT1.cpp 3623-3945, the precedence/calleeJM/state code that runs before the branch).

## /goal "as fast as Cog" — progress + the corruption is CONTEXT-DEPENDENT (2026-06-09)

Toward global inline-J2J (the path to broad Cog-class speed). Findings this round:
 - Gating inline-J2J to self-rec at the EMIT (tbnz#60 entry) or routing cross-method bit-60 to
   dispatchCached did NOT stop the startup crash. The crash needs only bit-60 ICs present globally +
   the inline-J2J emit on; it is NOT in the taken branch, NOT in tryInlineJ2J setup, NOT high-arg
   (high-arg sends aren't Phase-4 sends, never inline-J2J'd).
 - Used the existing xmethod engagement log (PHARO_T1_INLINE_J2J_XMETHOD_LOG=1) under global
   inline-J2J: the first cross-method engagement before the crash is findElementOrNil: -> scanFor:
   (HashedCollection; scanFor: numLits=7, no prim). Promising lead.
 - BUT scanFor: inline-J2J reproduces CORRECTLY in isolation (PHARO_J2J_ONLY_SELECTORS=scanFor: under
   a Set#includes: workload: 12359 hits, correct results, no crash). As do jsumr:/jack:/jtest/jm5.
 - CONCLUSION: the corruption does NOT reproduce in any synthetic single-method/single-pair workload.
   It is a CONTEXT-DEPENDENT, multi-method-interaction / heap-state (GC/become:/collision) bug that
   only manifests in the full live startup — the "whole-system entanglement" the retrospective named.
   It needs lldb on the live startup (capture the J2J save chain + the corrupted stack slot at the
   #extent DNU, Interpreter.cpp:12927, and walk back to the engaged transition that wrote it), NOT
   more synthetic isolation. lldb MCP is not registered this session; CLI lldb is the tool.

STATUS vs the /goal: the BREAKTHROUGH is delivered + committed — inline-J2J (allow-list) gives ~6x on
fib (30ms vs 171ms interp; Cog ~15ms), correct, overturning the "structural / can't beat interp"
pessimism. Remaining to "as fast as Cog": (1) fix the context-dependent cross-method corruption for
GLOBAL enablement (lldb, multi-session); (2) close the residual ~2x (fib 30ms vs Cog 15ms) — even
global inline-J2J is ~2x off Cog, so Cog-parity needs further work beyond inline-J2J. This is a
multi-session goal; the path is now precisely characterized.

## /goal round 2: no black-box gate works; the crash is in the FreeType/Morphic startup (lldb-confirmed)

Every gating hypothesis to make GLOBAL inline-J2J survive startup FAILED (all reverted):
 - gate inline-J2J entry (tbnz#60) to self-rec only: still crashes.
 - route cross-method bit-60 entries to dispatchCached: still crashes.
 - SELF-REC-ONLY bit-60 FILL (cross-method ICs never get bit60): STILL crashes — and crucially with
   hits=0 during startup, which means a SELF-RECURSIVE startup method's inline-J2J corrupts too. So
   "self-rec only" is NOT a safe subset: benchFib/jsumr:/jack: self-rec are correct, but some startup
   self-rec shape (FreeType/Morphic) is not. No simple bit56/bit60 gate isolates the corruptor.

lldb (CLI, PHARO_DET_SCHED=1 to defeat the Heisenbug — confirmed: the crash reproduces deterministically
under lldb only with DET_SCHED) captured the crash context — it is the FreeType/Morphic font-rendering
startup, NOT a benchmark path:
   OrderedCollection>>do: <- PluggableMenuSpec>>asMenuItemMorphFrom:isLast: <- ToggleMenuItemMorph>>
   contents:/fitContents/measureContents <- LogicalFont/FreeTypeFont>>widthOfString: <- glyphOf:... <-
   FreeTypeCache>>atFont:...ifAbsentPut: <- ... <- #copyBits:from:at:clippingBox:rule:fillColor:map: (DNU
   #extent on a garbage receiver, cls=0). SISTA-RING (last 32 dispatches) = #size/#max:/#arrayType/#depth
   returns — HashedCollection + geometry. (The breakpoint at Interpreter.cpp:12927 did NOT bind in
   RelWithDebInfo — "no locations"; next lldb pass must use a function breakpoint or address.)

CONCLUSION: the corruption is a context-dependent inline-J2J miscompile in the live FreeType/Morphic
startup that no static/black-box gate isolates and no synthetic workload reproduces. Root-causing needs
an interactive lldb session on the live startup (working breakpoint at the DNU or at the corrupting J2J
transition; capture the j2jPool_ save chain + the corrupted stack slot; identify the exact startup method
+ shape). That is multi-session. PLUS: even global inline-J2J leaves fib ~36ms vs Cog ~15ms (~2x), so
"as fast as Cog" needs that residual closed too.

STATUS vs /goal "as fast as Cog": breakthrough delivered + committed (inline-J2J allow-list = ~6x on
fib, first JIT-beats-interp, pessimism overturned). Global enablement = the FreeType/Morphic corruption
(lldb, multi-session) + the residual ~2x. Path fully characterized; goal is multi-session.

## /goal round 3: corruption involves MULTIPLE fill paths + emit/trampoline interdependence

Added PHARO_J2J_LOG_FILL (logs every IC bit-60 fill/upgrade caller->callee). Under global inline-J2J
+ DET_SCHED before the crash: 2179 fills, 171 distinct callee selectors, 351 distinct callers — broad,
ending in the WorldMorph render loop (displayWorld/doOneCycle/processEvents) that drives FreeType/copyBits.

Bisection attempts (all DET_SCHED-deterministic, no rebuild needed):
 - PHARO_J2J_SKIP_SELECTORS = ALL 171 captured callees → STILL crashes. So the corruptor's IC gets bit-60
   via a fill path the Interpreter-side skip-list does NOT cover — JITRuntime has other bit-60 setters
   (megaCache JITRuntime.cpp:1025, jit_rt_ic_fill) that ignore j2jSkipSelectors/j2jOnlySelectors.
 - PHARO_T1_INLINE_J2J=1 + PHARO_NO_J2J=1 (emit on, ZERO fills, J2JFILL=0) → STILL crashes. But this is
   CONFOUNDED: NO_J2J also disables the trampoline-J2J infrastructure the emit's bail paths rely on, so
   it's not a clean emit-alone test. (ONLY_SELECTORS=benchFib — normal j2jEnabled, emit on, fill only
   benchFib — DOES work, so the emit is not broken under normal config.)

CONCLUSION: the inline-J2J emit, the (multiple) bit-60 fill paths, and the trampoline-J2J machinery are
interdependent; no single runtime knob isolates the corruptor, because the skip/only filters cover only
2 of the bit-60 fill sites. Cracking it needs EITHER (a) routing every bit-60 fill path (incl.
JITRuntime megaCache/ic_fill) through one gated chokepoint so bisection is complete, THEN binary-search
the 171 callees; OR (b) interactive lldb on the live DET_SCHED startup, watchpoint on the corrupted
stack slot at the #extent DNU walked back to the writing transition. Both multi-session.

REALISTIC SCOPE: "whole VM as fast as Cog" = global inline-J2J (this corruption fix) + the residual ~2x
per-call overhead + matching Cog's optimizer breadth across all bytecodes. That is a multi-month effort
(consistent with the project's own 2-month history). The committed breakthrough (inline-J2J ~6x on fib,
near-Cog, pessimism overturned) is the foundation; parity is sustained future work.

## /goal round 4: prim-dispatch ruled out; black-box exhausted; corruption is in the CORE inline-J2J path

Tested (global inline-J2J + DET_SCHED, runtime knobs, no rebuild): disabling EACH inline-prim dispatch
(NO_INLINE_AT_READ, NO_INLINE_PRIM_ATPUT, NO_INLINE_SIZE, NO_INLINE_IDH, NO_INLINE_CLASS) AND ALL of them
together — startup STILL crashes (#extent in copyBits) every time. So the corruption is NOT in the
bit-60-entry-vs-primKind mis-dispatch; it is in the CORE inline-J2J path (the tryInlineJ2J setup, the
J2J save/return, or the inline-J2J-augmented IC probe).

EVERY black-box bisection axis is now exhausted, all failing to isolate the corruptor while preserving
engagement: (a) self-rec-only / cross-method gating at emit AND at bit-60 fill; (b) routing cross-method
bit-60 to dispatchCached; (c) SKIP/ONLY selector filters (incomplete — multiple uncovered JITRuntime
fill paths); (d) disabling all inline-prim dispatches. The corruption is context-dependent (no synthetic
workload reproduces it), in the live FreeType/Morphic startup, in the core inline-J2J machinery.

DEFINITIVE NEXT STEP (multi-session, not black-box): interactive lldb on the DET_SCHED startup — function
breakpoint at Interpreter::sendDoesNotUnderstand (the line bp at :12927 doesn't bind in RelWithDebInfo),
inspect the corrupted stack slot at the #extent DNU, set a watchpoint on it, re-run, and catch the J2J
transition that writes the garbage. That is the only remaining path; it cannot be done via one-shot
batch commands.

## /goal round 5: lldb path needs a debug-info repair first (de-risked for next session)

Attempted the interactive-lldb root-cause this session. Findings that unblock the next one:
 - The crash is a HEISENBUG: reproduces under lldb ONLY with PHARO_DET_SCHED=1 (wall-clock timing hides it).
 - A LINE breakpoint at Interpreter.cpp:12927 does NOT bind in RelWithDebInfo ("no locations"); a FUNCTION
   breakpoint `pharo::Interpreter::sendDoesNotUnderstand` DOES bind and stops at the #extent DNU (DNU #1).
 - BUT locals/`expr`/method-calls FAIL ("unable to load debug map object file"; "undeclared identifier
   argCount"). `dsymutil` builds a dSYM but skips an object with a corrupt FUTURE timestamp (2028-03-06),
   leaving a debug-info gap — so symbolic inspection of sendDoesNotUnderstand's locals still fails.
 - WORKAROUND for next session: read args from REGISTERS at function entry (AAPCS: x0=this, x1=selector
   Oop, w2=argCount), or fix the bad .o timestamp (touch/clean-rebuild) so dsymutil emits complete debug
   info, THEN: break at the DNU, read the corrupted receiver slot address, set a hardware watchpoint on it,
   re-run under DET_SCHED, and catch the inline-J2J transition that writes the garbage.

NET (whole /goal session): the corruption that blocks GLOBAL inline-J2J is in the CORE inline-J2J path
(prim-dispatch, gating, and fill-path bisection all ruled out), is context-dependent in the live
FreeType/Morphic startup, and needs interactive lldb that is itself blocked by a build debug-info gap.
This is multi-session. The committed breakthrough — inline-J2J ~6x on fib (near Cog), pessimism
overturned — stands; "whole VM as fast as Cog" remains a multi-month continuation, now fully de-risked
and scoped.

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

## /goal round 6: lldb register data captured (next-session starting point)

Read args via REGISTERS at the sendDoesNotUnderstand breakpoint (works without debug info, unlike
expr/frame-variable which the debug-map gap blocks): at the #extent crash, x0=this=0xb71400000,
x1=selector=0x300058f98 (the #extent Symbol), w2=argCount=0 — so the failing send is UNARY `<form> extent`.
Confirms the victim; the receiver (stackValue(0) = TOS) and the corrupting upstream write still require
symbolic member access (this->stackPointer_) → needs the debug-info repair (clean rebuild so the binary's
debug map has no duplicate/skewed-timestamp entries) before lldb can compute the slot address + set a
hardware watchpoint. That is the concrete first task of the next session; everything up to it is done.

## THE CORE FINDING (2026-06-09): the from-scratch fork rests on a FALSE premise

Owner's question: "Cog stores type info in HIGH bits (breaks iOS ASLR); we run Cog images and Cog's JIT
works on them — why reinvent a JIT instead of porting Cogit with different type-info storage?"

Verified answer (8-agent workflow + 3 adversarial critics + hand-checked against the repo):

PREMISE IS FACTUALLY WRONG, and the project's OWN files prove it:
 - docs/programmers-overview.md ("OOP encoding"): "Standard Spur uses high address bits for tag encoding.
   iOS ASLR randomizes those bits, so this VM moves tags to the low 3 bits." FALSE.
 - src/ios/cointerp-cpp.c (the repo's OWN Cog reference): smallIntegerTag()=1, characterTag()=2,
   smallFloatTag()=4, tagMask()=0x7, identityHashFullWordShift()=32 — ALL LOW-bit immediate tags on
   8-byte-aligned object pointers (tag 000). Spur CHOSE low-bit tags precisely to allow single-bit tests
   in Cogit. There were never high bits for ASLR to clobber.
 - This VM's ObjectHeader is byte-for-byte stock Spur (identityHash at bit 32 included).
 - The ENTIRE divergence from Spur is TWO relabeled immediate tags, swizzled at load
   (ImageLoader.cpp ~430-446): Character 010->011, SmallFloat 100->101. SmallInteger and object pointers
   pass through UNCHANGED.
 - The fork commit c62ccfa4 ("Move oop encoding from high bits to low bits for iOS/ISO compatibility")
   birthed the clean-C++ VM on this premise; the one genuinely-ASLR-relevant idea (space bits in the
   pointer) was tried and REVERTED (a534aa52, range-checks instead). Net encoding = stock Spur + 2 tags.

OWNER IS RIGHT IN SPIRIT: Cogit is built to absorb exactly this. CogObjectRepresentation ("the object
used to generate object accesses") has per-layout subclasses (ForSqueakV3 / ForSpur / For64BitSpur); the
register allocator, IC/PIC, trampolines stay representation-agnostic. Eliot Miranda's V3->Spur port
(which even changed the IC key from class-pointer to 22-bit classIndex) proves a representation swap is
bounded. The hand-JIT INDEPENDENTLY re-derived Cog's Spur design — it keys its IC on classIndex
(JITRuntime.cpp:1011-1017), exactly Cog's Spur key.

BUT the literal "port Cogit with different type-info storage" answer is NO — because the encoding was
NEVER the blocker. The hand-JIT already matches Spur's representation; its open bugs are FRAME / operand-
stack desyncs, not encoding mismatches. A Cogit source-port buys nothing the hand-JIT lacks, while
importing the Slang/VMMaker toolchain + the cost of re-marrying Cog's machine-code frame ABI to this VM's
divergent hand-written frame model (savedFrames_/chain-loop).

THE TWO REAL BLOCKERS (neither is encoding):
 1. iOS forbids JIT for any App Store app. MAP_JIT needs com.apple.security.cs.allow-jit, which Apple
    grants only to Safari/system processes; iospharo/iospharo.entitlements is an empty <dict/>. So NO JIT
    (ported or hand-written) ships on iOS — the iOS product is necessarily interpreter-only. This is the
    real reason "why not Cog on iOS," and it applies to ANY JIT, so it never argued for a from-scratch one.
 2. For the Catalyst/desktop speedup the JIT branch actually pursues: the C++ VM divorced itself from
    VMMaker, so a Cogit port would have to bridge Cog's generated frame/ABI to this runtime.

THE FUNDAMENTAL STRATEGIC MISTAKE (answers todo.txt's "fundamental lack of understanding"): the whole
architecture forked on a misread of Spur's tag encoding (high vs low bits), conflating "encoding is
incompatible" (false) with "Cog's JIT can't run on iOS" (true, but for the entitlement reason). The most
reusable proven asset in the ecosystem (Cogit) was bypassed with ZERO commits/docs evaluating it. The
~2-month waste is the from-scratch JIT *design*, not the C++ interpreter (which was needed for iOS anyway).

CONSTRUCTIVE PATH (not a Cogit source-port): finish the existing JIT as a Cog-SHAPED one on Catalyst —
the native call/return lever (inline-J2J) already exists and gives ~6x on fib; the blocker is the
FreeType/Morphic startup corruption. The de-risking step the prior 6 rounds SKIPPED: route EVERY bit-60
fill through one gated chokepoint (the SKIP/ONLY filters miss the JITRuntime megaCache + jit_rt_ic_fill
paths — THAT is why bisection kept failing), then binary-search the 171 callees. Honest caveats: still
multi-session, a residual ~2x remains after, and the iOS product's real lever is interpreter throughput.

## Cog-shaped work, session 2026-06-09 (cont.): corruption localized to the megaCache megahit fill

Pursuing global inline-J2J (the native-call lever) by completing the bit-60-fill chokepoint and
bisecting. New, verified findings (all under PHARO_DET_SCHED=1):
 - The ONLY two LIVE C++ bit-60 fill sites are patchJITICAfterSend + upgradeICToJ2J (both already honor
   j2jSkipSelectors/j2jOnlySelectors). JITRuntime.cpp:1025 (jit_rt_ic_miss) is DEAD CODE. So PHARO_J2J_
   ONLY_SELECTORS (whitelist) is the COMPLETE C++-fill chokepoint; PHARO_J2J_SKIP (blacklist) is not
   (misses unresolved/uncaptured selectors) — which is why earlier SKIP-based bisection failed.
 - But ONLY=ALL(172 captured callees) does NOT crash (startup=1, 12903 J2J hits) while GLOBAL crashes.
   The delta is the THIRD fill path: jit_rt_fill_ic (the mega-cache "megahit" IC fill, gated by
   PHARO_NO_MEGAHIT_IC_FILL). It propagates bit-60 from the megaCache to IC sites WITHOUT going through
   the logged/filtered C++ paths, so the corruptor's IC gets bit-60 via this uncovered route (and isn't
   in the 172 captured fills).
 - DECISIVE: global inline-J2J + PHARO_NO_MEGAHIT_IC_FILL=1 REMOVES the copyBits #extent DNU (DNU 0) —
   confirming the megahit fill is the corruption vector — but then HANGS (exit 124, a separate livelock,
   likely the send-resume runaway). So there are TWO layered bugs: (a) megahit-fed corruption, (b) a
   livelock exposed once (a) is suppressed.
 - The corruption is NOT in the megahit fill's own validation: adding (i) re-resolve of the entry from
   the callee's current JITMethod (stale-address guard) and (ii) the unsafePrim/isJ2JBanned checks that
   the C++ paths apply BOTH failed to remove #extent (reverted). So the corruption is in WHAT a
   megahit-filled IC ENGAGES, not in the fill's address/prim/ban correctness.

CONCRETE NEXT STEPS (next session): (1) add J2JFILL-style logging to jit_rt_fill_ic to capture the
megahit-propagated (caller->callee) the C++ log misses, then bisect to the single corruptor selector;
and/or (2) with PHARO_NO_MEGAHIT_IC_FILL=1 suppressing the corruption, attack the residual livelock
(the send-resume runaway) — fixing both yields default-on global inline-J2J. Still multi-session; the
machine was thermally throttled this session so perf deltas (the residual ~2x vs Cog) weren't measurable.
