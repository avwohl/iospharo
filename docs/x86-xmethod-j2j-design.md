# x86 cross-method inline-J2J — vetted design (Increment 1)

Source: design workflow `wf_1a7414dd-94b` (2026-06-15, 9 agents, 4 maps +
adversarial verify, 25 holes / 7 critical). Verdict: **go-incremental**.

## The lever

x86 has SELF-recursive inline-J2J only (benchFib 3.2x). Cross-method sends
(cfibx's `incc`, getter sends everywhere) run full IC dispatch (222ns/call vs
arm64's 8ns) → cfibx 27x. arm64 has cross-method (xmethod) inline-J2J +
bail-time J2J-save materialization; x86 does not.

## Increment 1 = canSkipJ2JSave callees only (NOT bit 57)

THE load-bearing correction (3 skeptics converged): the obvious gate
`bt r8,57` (kXGateOkBit) is **fatally wrong** — xmethodGateOk
(JITRuntime.hpp:109-114) admits up to `PHARO_T1_XMETHOD_MAX_IC` (default **8**,
not 0) IC sites AND canBailMidMethod callees by default. So bit 57 fires for
exactly the non-leaf, mid-bailing callees Increment 1 cannot support.

GATE on `callee->canSkipJ2JSave` (JITMethod.hpp:249) instead. It is
`isReal && (forceSaveless || (!canBailMidMethod && numICEntries==0))`
(AsmjitT1.cpp:10851). Means no-send + no-cond-jump-bail. `incc` (`^self+1`)
qualifies → **cfibx benefits**; getters/constant-returners qualify → broad
real-code win.

RESIDUAL CAVEAT (verified, beyond the workflow's "provably clean" claim):
canSkipJ2JSave does NOT exclude **arith-overflow** bails (ExitArithOverflow is
not a cond jump, so canBailMidMethod misses it). A qualifying callee with `+`/
`-`/`*` CAN still exit-to-C++ on overflow. For cfibx's bounded range incc never
overflows (clean path only). The GENERAL default-on flip needs the open
ExitArithOverflow-materialize bug fixed first → validate the overflow path
explicitly (deliberate cross-method overflow test + SUnit + DET_SCHED).

## Increment 2 (bailmid / numIC>0 callees) — BLOCKED, do not implement

The ExitArithOverflow chain handler (Interpreter.cpp:26322-26361) does NOT call
materializeJ2J and returns with j2jDepth>0 — a documented OPEN arm64 bug
(comments 26339-26350: "LEAF_ALL flicker root cause remains OPEN"; two prior
fix attempts regressed). Increment 2's soak (overflow inside a cross-method
callee) drives straight into it. DEFERRED until that handler materializes
correctly + a per-call entry-cursor pin is added.

## Critical facts (verified against source)

- `save.jitMethod = CALLER JM` (resume identity), NOT callee — the workflow's
  own APPROACH prose contradicted itself; source proves caller
  (Interpreter.cpp:23912 materialize reads it as resumer; 24798 round-trip
  writes callerJM). Writing calleeJM = historical V1 corruption class.
- state.jitMethod = CALLEE JM after the push (callee runs).
- argCount is `uint8_t` (JITMethod.hpp:215) → restore with **byte load (movzx)**,
  not dword (arm64 uses ldrb, AsmjitT1.cpp:4026). tempCount also uint8_t (off 35).
- nil-fill + newSp MUST use runtime `calleeJM->tempCount` via a LABELED loop
  with the no-fill edge (mirror arm64 6984-6997); the x86 self-rec push bakes a
  compile-time callerTempCount immediate (2804-2806) — WRONG for cross-method
  (callee tempCount != caller).
- OFF_LITERALS=16, OFF_JITMETHOD=56, OFF_ARGCOUNT=72 (AsmjitT1.cpp:854-859);
  caller literals+method+argCount MUST be restored in the return prelude (resumed
  caller reads OFF_LITERALS/OFF_METHOD directly).
- codeStart() == this + sizeof(JITMethod) (JITMethod.hpp:338) → calleeJM =
  entryAddr - sizeof(JITMethod) is valid; add a static_assert.
- E2 ordering: write callee state AFTER the save push + depth publish, so a
  save-room bail leaves state.method=callerCM (the X+BV SIGSEGV class).
- V1 only: static_assert(!PHARO_J2J_SAVE_V2) in the x86 block.
- Byte-offset off-by-one class CANNOT recur (x86 consumes a precomputed bit /
  offsetof, no raw byte cascade).

## Knob / kill-switch

`PHARO_T1_X86_XMETHOD` (DEBUG_BOOL, debug_vars.h, default OFF). When off, the
send-site gate + return prelude emit BYTE-IDENTICAL self-rec-only x86 (the
baseline is the hot DEFAULT-ON self-rec path — capstone-diff is mandatory).
Default-ON flip (opt-out) only after full 565-class suite shows 0 deterministic
regressions on the x86 box.

## Validation gate (x86 AWS box; x86 can't build locally)

1. Knob-OFF capstone-diff send site + return prelude vs current jit branch →
   prove byte-identical baseline.
2. Knob-ON capstone-disasm one cross-method send: canSkipJ2JSave byte test (NOT
   bt 57); save[32]=callerJM; callee writes AFTER push; calleeJM=entryAddr-
   sizeof; dynamic nil-fill from calleeJM->tempCount; argCount byte-restore; jmp.
3. eval micro-tests 5/5: a canSkipJ2JSave cross-method pair, mutual-recursion,
   3+4=7 canary, factorial/gcd vs baseline. g_x86_xmethod_fires>0.
4. Benchmarks: cfibx (key — does it drop from 599?), benchFib + loop + battery
   no-regress vs self-rec baseline. PLUS a deliberate cross-method arith-overflow
   test (the residual-caveat path).
5. SUnit subset (inline-J2J-sensitive classes) A/B knob-on vs off per-test
   IDENTICAL; then under PHARO_DET_SCHED=1 + coarse quantum.
6. Full curated 565-class suite knob-on vs self-rec baseline: 0 deterministic
   regressions before any default-ON flip. (x86 JIT base is bit-rotted,
   debug_vars.h:208 + per-send sp leak 200 — repro any new failure knob-OFF
   before blaming this change.)

## Increment 1 IMPLEMENTED + 3 validation boxes (2026-06-15) — KNOWN-BROKEN

Commits 1bba9021 (Increment 1) + cc1caa18 (tighter gate: +!hasPrimPrologue
+!hasHeapWrites). Behind PHARO_T1_X86_XMETHOD, default OFF.

Box results:
- KNOB-OFF: byte-identical baseline CONFIRMED (cogRunBench 133/51/571, cfibx20
  =17710). The hot self-rec default-on path is untouched.
- KNOB-ON: CORRUPTS STARTUP. The path FIRES (counter: ~245K cross-method
  inline-J2J during image startup — getters everywhere), but EVERY eval returns
  nil via FullBlockClosure>>onErrorDo: -> SnapshotOperation>>executeStoringError:,
  including the `3 + 4` canary (which uses no cross-method send) -> the
  corruption is during STARTUP, not the bench. The tighter prim/heap gate did
  NOT fix it -> it is a COMMON-PATH emit bug (getter cross-method sends), not the
  prim/heap bail path.

Verified CORRECT by inspection (ruled out): calleeJM = entryAddr - sizeof(JITMethod)
(matches arm64 6014-6018: entryAddr = extra & 0x0000FFFFFFFFFFFF, calleeJM =
entryAddr - sizeof); the save push is byte-copied from the working self-rec push
(save[32]=callerJM); literalsCache/bcStartCache/tempCount/canSkipJ2JSave read via
offsetof; the return-prelude restore preserves rax/rdi and uses only
already-clobbered rcx/rdx. The bug is inspection-resistant.

THREE blockers, in order:
1. A subtle COMMON-PATH emit bug in the cross-method send/return (needs runtime
   debugging — but x86 can't build locally and the AWS box has no lldb wired up;
   next step = a capstone-disasm box (workflow validation step 2) to SEE the
   emitted cross-method site, or wire lldb on the box).
2. DEFAULT-ON additionally needs the open ExitArithOverflow-materialize bug fixed
   (Interpreter.cpp:26339-26350; 2 prior arm64 attempts regressed): canSkipJ2JSave
   is NOT truly bail-free — arith-overflow (and alloc/GC) escape the canBailMidMethod
   count, so a qualifying callee can still bail mid-method. cfibx's bounded incc
   never overflows, but startup callees do.
3. The x86 tier-1 base is bit-rotted (debug_vars.h:208: prescan/emit disagree on
   ~10 bytecodes + a Corrupt-stackPointer miscompile; :200: ~1-word/send caller-
   resume sp leak). Cross-method sends multiply send activity -> amplify the
   pre-existing leak/miscompiles. A clean cross-method run likely needs the base
   hardened first.

NET: the cfibx lever (x86 27x) is a genuine MULTI-SESSION effort (emit-debug +
open-bug-fix + base-harden), not a single-increment win. Implementation kept
default-OFF as a safe foundation; arm64 + x86-default are untouched.

## objdump diagnostic (box 4, 2026-06-15): THE EMIT IS CORRECT

Dumped xm1 (cross-method caller `^self xm2`, 716B) + xm2 (callee `^41`, 163B) via
PHARO_T1_DUMP_SEL with PHARO_T1_X86_J2J_SEL=xm1 (keeps startup clean so the dump
reliably fires), objdump'd both. EVERY instruction matches intent:
- gate: bt $0x3c (bit60); self-rec cmp vs OFF_METHOD(0x40); cross: entryAddr =
  extras & 0xffffffffffff; canSkipJ2JSave @ calleeJM+49, hasPrimPrologue @ +41,
  hasHeapWrites @ +37 (all movzbl+test).
- save push: save[32]=OFF_JITMETHOD (caller JM) BEFORE the overwrite; ip=method+0x2a.
- callee state: calleeJM = entryAddr-0x80 (sizeof=128); method=calleeJM[0]
  (compiledMethodOop); literals=calleeJM[0x78] (literalsCache); ip=calleeJM[0x68]
  (bcStartCache); argCount=nArgs; dynamic nil-fill reads tempCount @ calleeJM[0x23].
- return prelude: restores callerJM=save[32], method/literals/argCount(byte @ +0x22)
  from it; preserves rax(retval)/rdi(state). All correct.
VERIFIED the load-bearing assumption: Interpreter.cpp:23142-23144 sets the IC J2J
entry = target->codeStart(), so calleeJM = entryAddr - sizeof is exactly right.

ROOT CAUSE (by elimination, strongly indicated): the emit is correct, so getters /
constant-returners (no mid-method exit) inline-J2J fine. The ONLY mid-method exit a
canSkipJ2JSave callee can take is SmallInteger ARITH-OVERFLOW -> ExitArithOverflow,
which is the OPEN materialize bug (blocker 2). During startup, arith canSkipJ2JSave
callees overflow and hit it -> corruption. So knob-on breaking startup is NOT an
emit bug — it is the open ExitArithOverflow-materialize bug, reached because
canSkipJ2JSave is not truly bail-free (arith escapes canBailMidMethod).

CONCLUSION: x86 cross-method inline-J2J is CORRECTLY IMPLEMENTED (objdump-proven) but
gated behind the open ExitArithOverflow-materialize bug. To ship it: (a) fix that
handler to materializeJ2J when j2jDepth>0 (the known-hard arm64 bug, 2 prior fails) —
this ALSO unlocks arm64 Increment 2; OR (b) add a compile-time "no-arith / no-alloc"
truly-bail-free flag (excludes incc -> no cfibx benefit). Immediate next step for a
focused session: a confirmation box (count ExitArithOverflow-with-j2jDepth>0 during
knob-on startup) then attempt the materialize fix with PHARO_DET_SCHED for determinism.
Implementation stays default-OFF (correct foundation; arm64 + x86-default untouched).

## ROOT CAUSE CONFIRMED + DETERMINISTIC REPRO (box 5, 2026-06-15)

KNOB-ON + PHARO_JIT_FAIL_REASONS=1 on `(3+4) printString`: **30/30 [AO-DIVERGED]
with j2jD=1**, all `sel=#to:` at ipOff=44, `tb-1 == fp` (frame pointer MATCHES —
so NOT the framePointer-mismatch class, issue (1); it is purely pending-save,
issue (2)). KNOB-OFF: 0 AO-DIVERGED, EVAL='7'. (30 is the diagnostic's print cap;
real count is higher.)

So the open bug is DEFINITIVELY: `ExitArithOverflow` fires with a pending
cross-method J2J save (j2jDepth>0) and does NOT materialize it. Method M (entered
via cross-method inline-J2J) hits SmI arith-overflow -> ExitArithOverflow returns
false to the interpreter, but M's caller frame lives only in the unpopped J2J save
-> M's eventual interpreter return corrupts. `#to:` (inlined to:do:) is the
deterministic trigger during startup.

KEY UPGRADE vs the 2 prior failed fixes: they faced the INTERMITTENT "LEAF_ALL
flicker"; PHARO_T1_X86_XMETHOD=1 gives a **DETERMINISTIC repro** (every run, #to:,
j2jD=1) — the missing enabler. Repro recipe:
    PHARO_X86_JIT=1 PHARO_T1_X86_XMETHOD=1 PHARO_JIT_FAIL_REASONS=1 \
      ./build/test_load_image <image> eval "(3+4) printString"
    # -> [AO-DIVERGED] j2jD=1 spam + corrupt (empty) result; knob-off = clean '7'

MATERIALIZE MACHINERY (Interpreter.cpp:25237 `materializeJ2J` lambda): for each
pending save it builds a SavedFrame (materializeJ2JSaveIntoFrame, :23894),
chainCallDepth += depth, resets j2jDepth/cursor, syncs live regs to the callee.
This is what ExitArithOverflow needs. BUT the prior eager-materialize there
"double-handled" with a DOWNSTREAM save handler (in the chain-loop's caller, after
`return false`) -> the real open sub-problem is reconciling the bail-time
materialize with that downstream handler. The naive `materializeJ2J()` in
ExitArithOverflow regresses (proven 2026-06-12). Next focused session: with the
deterministic repro, trace the chain-loop caller's downstream j2jDepth handling
after an ExitArithOverflow false-return, find why it doesn't materialize the
cross-method save (and why doing it eagerly double-handles), then a knob-gated fix
validated against the repro (must show 0 AO-DIVERGED corruption) + arm64
no-regression (the handler is SHARED). Likely needs the fix to ALSO gate arm64
Increment 2.

## Code-path map for the fix (next session roadmap)

The bail/materialize machinery involved in the open bug:
- `tryJITActivation` (Interpreter.cpp:24119) = the chain loop. Its `case
  ExitArithOverflow` (:26322) does `return false` WITHOUT materializing when
  j2jDepth>0. Sibling handlers (PushArray create :26317, :26179, :27138/27336/
  27400) DO call the `materializeJ2J` lambda (:25237) and resume into JIT.
- `materializeJ2J` lambda (:25237): per pending save -> SavedFrame via
  materializeJ2JSaveIntoFrame (:23894); chainCallDepth += depth; reset depth/
  cursor; sync live regs to callee. Correct reconstruction.
- DOWNSTREAM handler (:20769, gated `resumeInternalJ2J && j2jDepth>0`): a SECOND
  materialize site in the resume path (callers of tryJITActivation at activateMethod
  :12092/:12102). The prior eager-materialize in ExitArithOverflow double-handled
  with THIS.
- THE FIX QUESTION: when ExitArithOverflow returns false with j2jDepth>0, does the
  downstream (:20769) path run for that return, or not? If it runs but doesn't
  materialize the cross-method save correctly -> fix :20769. If it doesn't run for
  the ExitArithOverflow false-return path -> materialize at :26322 but guard against
  the double-handle (a flag, or only when the downstream won't). Use the
  DETERMINISTIC repro to bisect: add a one-shot trace at :20769 and :26322 under the
  repro, see which fires for the #to: j2jD=1 case, then fix that path. Knob-gate +
  validate 0 AO-DIVERGED on the repro + arm64 full-suite no-regress (SHARED handler).

## AO materialize fix: SAVE-READ correct, RESUME-FLOW is the bug (probe box, 2026-06-15)

Knob-gated fix PHARO_T1_AO_MAT_J2J (#if !PHARO_J2J_SAVE_V2, x86/V1) calls
materializeJ2J in case ExitArithOverflow when j2jDepth>0. arm64-SAFE: compiles out
on V2; knob-OFF AND knob-ON both battery==golden (verified). BUT x86 box: knob-ON
does NOT fix (3+4) (still nil, same as control) -> NECESSARY-BUT-INSUFFICIENT.

AO-MAT-PROBE (FAIL_REASONS-gated PRE/POST trace) PROVED the materialize is CORRECT:
  [AO-MAT-PRE] j2jD=1 save0.jm=<CALLER> state.jm=<CALLEE, different> save0.ip=<valid> sel=#to:
  [AO-MAT-POST] j2jD=0 fd=<incremented>   (one CALLER SavedFrame built, depth reset)
So the save-read is RIGHT (CALLER, non-null, != CALLEE; V1-direct-read, not the
danger-zone fallback). Bailing selectors: #next:putAll: (21x) + #to: (9x), all AO
j2jD=1 (no non-AO leak). The bug is now purely the RESUME FLOW after materialize:
the interp resumes at the CALLEE's arith bytecode (correct) + does the LargeInteger
arith, but when the callee RETURNS it does not land in the materialized CALLER frame
-> corruption. Hypothesis: materializeJ2J builds the frame in savedFrames_ (the
JIT-chain resume structure) + the sibling handlers re-enter JIT (continue) to consume
it, whereas the AO handler return-false hands control to the INTERPRETER, whose
method-return uses framePointer_/its own frame stack and may not consume the
savedFrames_ entry. Resume-flow workflow wf_8b29daf3 analyzing the exact divergence +
the correct resume primitive (interp-does-arith-then-returns-into-CALLER).

## REDIRECT (workflow wf_8b29daf3, 2026-06-15): the AO path is NOT the (3+4) corruptor

Two airtight facts overturn the AO-fix framing:
1. (3+4)=7 is in SmallInteger range -> the interp fast path (Interpreter.cpp:3003)
   returns directly; the JIT inline-add only bails to ExitArithOverflow on OVERFLOW.
   So (3+4) NEVER reaches case ExitArithOverflow -> the AO-materialize fix CANNOT
   be what turns (3+4) into nil. The AO bails seen during startup (30/30, #to:/
   #next:putAll:) are a separate, real-but-secondary bug.
2. activeContext_ is ALWAYS nil during a J2J chain (Interpreter.hpp:1616-1620
   invariant; pushFrameForJIT skips writing it). So the resume-flow design's
   nil-ing steps would be no-ops. The AO-MAT-PROBE already proved the
   materialize/save-read CORRECT (save0.jm=CALLER, j2jD->0, fd++).

VERDICT needs-more-investigation: the PHARO_T1_X86_XMETHOD startup corruption
((3+4)->nil) is the cross-method NORMAL send/return EMIT (blocker #1, lines
111-118), UPSTREAM of and independent from the AO path. An ordinary, non-
overflowing cross-method send returns a corrupted (nil) result. objdump (box 4)
showed the emit matches INTENT, so the bug is SEMANTIC (the intent/design is
subtly wrong, or a runtime interaction), not an encoding/offset error — objdump
is insufficient; this needs single-stepping the x86 resume (box-lldb, not wired:
lines 121-122) or a value-level probe of one normal cross-method send.

STATE: PHARO_T1_AO_MAT_J2J committed (arm64-safe, default-OFF, V1-scoped) as a
real partial fix for the secondary AO bug; its sufficiency for the AO case is
unverifiable until blocker #1 is fixed. The AO sub-thread (workflows wf_ec90a67e
+ wf_8b29daf3, ~3 boxes) peeled the AO layer but the PRIMARY cfibx blocker is the
normal cross-method emit semantics. NEXT (focused session): value-probe a single
clean cross-method send (does `xm1 ^self xm2` / `xm2 ^41` return 41 under
X86_XMETHOD?) to confirm the normal emit is the corruptor, then capstone-diff +
box-lldb the send-site/return-prelude semantics. This is the genuine multi-session
blocker; it needs x86 debug infrastructure objdump alone can't substitute for.

## TRIANGULATION (boxes GATE0 + AO-bail + cfibx-isolation, 2026-06-15): LEVER IS CORRECT

Using PHARO_T1_X86_J2J_SEL trailing-* prefix scoping (keeps startup clean), every
isolated cross-method pattern produces the CORRECT value:
- normal send: (7 xm1)=41 (getter callee), (7 xm4)=8 (arith callee). GATE 0.
- AO-bailing send: (SmallInteger maxVal) xm6 = maxVal+1 (correct LargeInteger),
  WITH AND WITHOUT PHARO_T1_AO_MAT_J2J. So the AO bail in isolation is handled by
  the EXISTING machinery; my AO fix is NEITHER needed (isolated works without it)
  NOR sufficient (startup still broke) -> the AO fix is REVERTABLE.
- self-rec + cross-method MIX (the real cfibx: zcfibx -> zcfincc): zcfibx(20)=17710
  CORRECT (knob-on, SEL=zcf*).

CONCLUSION: the cross-method inline-J2J EMIT + RESUME logic is SOUND for all isolated
patterns. The PHARO_T1_X86_XMETHOD startup corruption ((3+4)->nil) is a SCALE/
INTERACTION effect that only manifests with the full 245K-fire startup across MANY
diverse methods + GC -- NOT the core cross-method mechanism, NOT the normal emit
(GATE 0 refutes the workflow wf_8b29daf3 "normal emit" redirect), NOT the AO bail.

This is a big positive: the cfibx lever WORKS. Remaining blocker = the scale bug.
NEXT: (1) re-measure the cfibx speedup with a correct timer (Time millisecondsToRun:)
to confirm cross-method inline-J2J actually beats self-rec-only for cfibx. (2) Find
the scale bug: bisect which method PATTERN or interaction (complex args/temps/blocks,
GC-during-fire, IC/save-stack at scale, deep self-rec+cross mix) corrupts at startup
that the simple isolated pairs don't exercise -- e.g. progressively widen the SEL
prefix from a safe set toward the full startup set, or capture the first corrupting
cross-method method. (3) Revert PHARO_T1_AO_MAT_J2J (unnecessary) once the scale bug
is understood (keep for now; default-off, arm64-safe). The lever could ship scoped to
validated patterns even before the scale bug is fully fixed.


## SPEEDUP CONFIRMED + SCALE BUG ISOLATED (timing box, 2026-06-15)

cfibx30 cross-method inline-J2J = **7.9x speedup**: self-rec-only (incc via IC)
569ms -> cross-method (incc inline-J2J) 72ms. The lever delivers.

BUT the scale bug is now reproducible IN ISOLATION (no startup needed): cfibx(30)
cross-method = 2178308 vs self-rec truth 2178303 (OFF BY 5). cfibx(20) was correct
(17710). All values stay in SmallInteger range -> NOT the AO path; it is a RARE
miscompile in the normal cross-method incc that accumulates with depth/count (~5
wrong in ~1.3M incc calls). This is the same bug that corrupts startup at scale,
now deterministically reproducible via SEL=zcf* at high n. Bisecting: find the n
where cross-method first diverges from self-rec (off-by-1 = single isolatable
miscompile), determinism (3x), and the GC/timing hypothesis (DET_SCHED).


## CORRECTION: the "scale bug / off-by-5" was a PHANTOM (2026-06-15)

The earlier "cfib(30) off by 5" was a BROKEN BASELINE artifact, not a real bug.
The "self-rec truth" used SEL=zcfibx, which scopes inline-J2J to ONLY zcfibx, so
the callee zcfincc gets NO return prelude (g_emitX86J2JOk false for it) -> that
config is itself broken (17706/2178303). Verified against the REAL truth (arm64,
no x86 knobs): cfib(20)=17710, (24)=121392, (28)=832039, (30)=2178308 -- and x86
cross-method (SEL=zcf*, both methods get the prelude) MATCHES ALL of them.

So the cfibx cross-method lever is FULLY CORRECT (every n) and 7.9x faster
(569->72ms). There is NO scale bug in the cfibx pattern. The startup corruption
(X86_XMETHOD no-SEL, all methods get preludes) is therefore a DIFFERENT, untested
method PATTERN -- not cfibx-like. All isolated tests used 0-ARG callees; the prime
suspect is the nArgs>0 cross-method path (the receiver=sp[-1-nArgs] / arg-shuffle /
save geometry). Bisecting arg-taking callees next (arm64 truth: 1-arg=101, 2-arg=30,
3-arg=6, temps+arg=15).

LESSON: when SEL-scoping a cross-method pair for isolation, BOTH caller and callee
must match the SEL (use the trailing-* prefix), else the callee loses its prelude
and the result is a scoping artifact, not a VM bug. Always compare to arm64/interp
truth, never to a SEL-scoped "self-rec" baseline.


## SCALE-BUG BISECTION COMPLETE (2026-06-15): lever CORRECT, startup blocker narrowed

EXHAUSTIVE isolation via SEL=prefix-* (compare to arm64 truth, never a SEL baseline):
ALL of these cross-method patterns are VALUE-CORRECT on x86 + 7.9x faster (cfibx30
569->72ms):
  0/1/2/3-arg callees; getter/constant/arith callees; caller-with-temps;
  self-rec+cross MIX (cfibx, all n match arm64 17710..2178308); polymorphic;
  multiple cross-method sends per caller; deep cross+self-rec mix.
So the cross-method emit/save/resume is CORRECT. cfibx lever WORKS.

The startup corruption (X86_XMETHOD no-SEL, (3+4)->nil) is a SCALE-ONLY effect.
RULED OUT: (a) every method pattern above; (b) code-zone EVICTION — CODE_ZONE_MB
4/64/256/512 ALL still corrupt, so zone size / eviction is NOT it. The only remaining
difference between SEL=z* (works) and no-SEL (corrupts) is WHICH methods inline-J2J.
REMAINING SUSPECTS: (1) a specific SYSTEM method with a bytecode shape the synthetic
patterns don't cover (e.g. ^special / ^GlobalLitVar / a particular store/push combo);
(2) code-zone GC/COMPACTION moving methods -> stale cached J2J entries (independent
of zone size). NEXT (focused): a capture-first-corruption probe — validate each
cross-method return's resumed state (caller method == save's caller) and log+abort on
the FIRST violation to name the corrupting (caller,callee) selectors; OR test a
code-zone-GC-disable knob. With the corrupting method named, the fix is targeted.

SHIPPABILITY: the lever already delivers 7.9x and is correct per-pattern; it could
ship SCOPED via PHARO_T1_X86_J2J_SEL to a validated hot-method whitelist even before
the scale blocker is root-caused. The AO fix (PHARO_T1_AO_MAT_J2J) is UNNECESSARY
(isolated AO-bail works without it) — revert candidate; kept default-off, arm64-safe.

## Revised plan (steps 1-10) — see workflow result for full text

1. debug_vars.h: DEBUG_BOOL(PHARO_T1_X86_XMETHOD) + _COUNTERS.
2. AsmjitT1.cpp ~95: g_emitX86Xmethod flag + static_assert(!V2).
3. compileViaAsmjit ~10177: g_emitX86Xmethod = g_emitX86J2JOk && knob (x86 ifdef).
4. send site 2766-2814: knob-off byte-identical; knob-on admit self-rec OR
   canSkipJ2JSave (byte test, NOT bit 57).
5. save push 2786: keep save[32]=CALLER JM, ip from caller.
6. after depth publish: callee-state writes (calleeJM, method, literals,
   argCount=nArgs) + dynamic nil-fill from calleeJM->tempCount; recompute
   entryAddr before jmp (nil-fill clobbers r10).
7. emitJ2JReturnPrelude_x86 1596-1625: restore caller jitMethod/method/literals/
   argCount (argCount BYTE load) from save[32]=callerJM.
8. counters (fold-independent).
9. this doc.
10. JITMethod.hpp static_assert codeStart()-this==sizeof(JITMethod).
