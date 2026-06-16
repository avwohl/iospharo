# x86 Cog gap — re-measured 2026-06-16 (cross-method default-on era)

Fair, same-machine, same-image (Pharo 13, `get.pharo.org/64/130+vm`), OPTIMIZED
build (`build-opt`, RelWithDebInfo) vs stock Cog x86 (`pharo-vm-Linux-x86_64-stable`,
v10.3.9). m6a.4xlarge (AMD EPYC 7R13). Both VMs pinned to core 0, min-of-9, thorough
warmup. Bench: `/tmp/cogbench3.st` (inline loops dodge the method-wrapped Time
artifact). arm64 column = local Apple-silicon, `build/test_load_image` (-O2) vs
macOS stock Cog.

## THE headline: the x86 tier-1 JIT is OFF BY DEFAULT

Runtime banner on the box:

    [JIT] x86_64 tier-1 JIT off by default (bit-rotted vs arm64;
          set PHARO_X86_JIT=1 to enable) — running interpreted

So EVERY default-config x86 run is the **interpreter**. JIT Stats confirm
`compiled: 0 methods` unless `PHARO_X86_JIT=1`. The memory's "cross-method
inline-J2J DEFAULT-ON" / "cfibx 8.7x" refer to the cross-method FEATURE being
default-on *within* the JIT — not to the JIT itself, which is gated off. Earlier
SUnit A/Bs run without the gate compared two interpreter configs (so "ON==OFF"
only validated the interpreter, not the cross-method JIT path).

## Numbers (ms; lower is better)

    bench       x86 interp  x86 JIT-on  x86 Cog   JIT/Cog   JITspeedup   arm64 JIT/Cog
    loopD100M      6736         672        226      3.0x      10.0x         0.55x
    loopS100M      7940        8206        254      32x        0.97x        0.44x
    fib33          1430         242         42      5.8x       5.9x         2.96x
    cfibx32        1220         187         32      5.8x       6.5x         2.70x
    cfibs32        1584        2208         41      54x        0.72x        3.18x

  loopD = `[1 to: 1e8 do: [:i | i + i]]`         (discard; no closure write)
  loopS = `[s:=0. 1 to: 1e8 do: [:i | s:=s+i]]`  (closure-temp write each iter)
  fib33 = self-recursive benchFib
  cfibx = recursive fib calling `incc ^self+1`   (cross-method, nArgs 0, SmI recv)
  cfibs = recursive fib calling `incs ^(self+1) max: 0` (cross-method + `max:` send)

## What the x86 JIT does and does NOT accelerate

WORKS (JIT clearly faster than interp, lands 3-5.8x Cog):
- loopD: 10x over interp → 3.0x Cog. Plain inlined `to:do:`.
- fib33 (self-rec inline-J2J): 5.9x → 5.8x Cog.
- cfibx32 (cross-method inline-J2J, the 2026-06-16 literalsCache fix): 6.5x →
  5.8x Cog. cfibx=187ms is exactly the memory's prior "185ms" — the fix is real
  *when the JIT is enabled*.

REGRESSES (JIT same-or-slower than interp; 32-54x Cog):
- cfibs32: JIT 0.72x = SLOWER than interpreter (1584→2208). The callee does a
  `max:` send (not a bare arith leaf like cfibx's incc).
- loopS: JIT 0.97x = no benefit (7940→8206). The loop body writes a closure temp
  each iteration.

## Diagnosis

The regressions are the documented x86 caller-resume bit-rot (`debug_vars.h`):
`PHARO_T1_NO_CALLER_RESUME` — "the x86 tier-1 caller-resume re-entry has a
pre-existing ~1-word-per-send operand-stack leak (frame-state-residency protocol
bit-rotted vs arm64 V2)". When a method's post-send continuation must re-enter
JIT with live operand-stack state (cfibs's `max:` result, loopS's accumulator),
the x86 path either strands in the interpreter or thrashes — so those cases get
no speedup or regress. The arm64 V2 frame-state-residency path (FSR M2 cursor
residency, resume-sends, PMS) is what makes arm64 0.44-3.18x across the board;
x86 never got the working V2 port. THIS is why the JIT is gated off by default.

## The lever toward x86 Cog parity

Port arm64's V2 frame-state-residency caller-resume to x86 (the
`PHARO_J2J_SAVE_V2` path is already on for x86 self-rec; the gap is the post-send
operand-stack resume protocol). Once cfibs/loopS stop regressing and match the
cfibx/fib quality (~3-5x Cog), the `PHARO_X86_JIT` gate can flip default-on.
Target: arm64-parity (sub-3x Cog) on all five benches.

Harness: `scripts/aws/sunit-ab.sh` style; bench `/tmp/cogbench3.st`; stock Cog via
`get.pharo.org/64/130+vm` (NOT `/64/vm`, which returns an HTML error page — the
historical install blocker). Run our VM with `PHARO_X86_JIT=1` or every number is
the interpreter.

## Root cause of the regressions (workflow, 2026-06-16)

The x86 in-JIT J2J return tail is CORRECT (cfibx/fib prove it): the V2 prelude
(AsmjitT1.cpp:1651-1727) restores caller sp from save[JSV_SP] and jmps to
resumeAfterCall (3327-3346) which does the static-nArgs arg-pop + retval write.

The leak is on the OTHER return edge: when the callee returns to C++ via
EXIT_RETURN (callee bailed mid-method — e.g. cfibs's inner `max:` send, loopS's
closure-temp write — so no in-JIT J2J tail fires) and `Interpreter::
tryJITResumeInCaller` (Interpreter.cpp:20404) re-enters JIT. That C++ path sets
`state.sp = stackPointer_` (20583) where the interpreter has ALREADY popped the
args and pushed the retval, then lands at the codeOffsetForResume entry (21223)
which on x86 resolves to `resumeAfterCall` — but resumeAfterCall ASSUMES the
V2-prelude contract (sp == caller's pre-send sp, retval in rax, args NOT yet
popped) and re-does `sp -= 8*nArgs; *(sp-8)=retval`. The args+retval are accounted
TWICE → exactly the documented ~1-word/send operand-stack leak. arm64 avoids it
because its resume entry and continuation share one emitLoadSp/emitStoreSp
residency contract that the C++ re-entry feeds consistently.

## CORRECTION — step-1 empirical root cause (2026-06-16, box #18)

The double-pop hypothesis above is **REFUTED** by both code reading and on-box
measurement. Do NOT implement the step-3 sp fix below as written.

Evidence:
- `PHARO_SP_DEPTH_CHECK` reports IDENTICAL counts (49) for cfibs and cfibx — no
  per-send sp drift. No leak.
- Code: `tryResume` (top-level resume) uses `codeOffsetForBC` (PLAIN label) with
  post-send sp — self-consistent. `resumeAfterCall` (the continuation) is only
  ever fed PRE-send sp (chain-loop `save.sp` + the in-JIT V2 prelude). Both
  consumers match their contract. There is no double-accounting.

REAL cause = **C++ caller-resume round-trip per inner send of a J2J-entered
callee** (resume-thrash, not a leak). Controlled leaf-shape A/B (N=30, min-of-5,
PHARO_X86_JIT=1, build-opt):

    leaf shape                         JIT   interp  no-caller-resume
    cfibx  incc = ^self+1 (no send)     72     460        71      JIT 6.4x WIN
    cfibt  inct = ^(self+1) incc        672    568        567     JIT NET-NEGATIVE
    cfibs  incs = ^(self+1) max: 0      809     —          —

cfibt's leaf sends incc (a trivial J2J-able method) and is STILL ~9x slower than
cfibx — so it is NOT max:-specific; ANY inner send inside a J2J-entered callee
thrashes. cfibt JIT (672) is SLOWER than interp (568); forcing interp-after-send
(`PHARO_T1_NO_CALLER_RESUME=1`) drops it to 567 ≈ interp. So the heavy C++
`tryJITResumeInCaller` round-trip after each inner send costs MORE than it saves.
Stat deltas confirm it: cfibs32 bench triggers ~4.88M J2J-resume events
(≈2.2×fib(32)) vs cfibx32's 476K — roughly one C++ resume per inner send.

So: JIT is a big win for methods whose callees return without an intervening send
(cfibx 6.4x); it is net-negative for callees that send-then-continue, because the
post-send continuation exits to C++ instead of staying in JIT. This IS the arm64
"resume-sends / FSR residency" the x86 path lacks — but the fix is the in-JIT
post-send continuation, NOT an sp-arithmetic change.

## Revised lever (task #20) — in-JIT post-send resume for send-bearing J2J callees

Make a J2J-entered callee's continuation AFTER an inner send re-enter JIT cheaply
(in-JIT resume continuation, as arm64 does via resume-sends), instead of bailing
to the heavy C++ `tryJITResumeInCaller`. Reference: arm64's resume-sends emit +
the `resumeAfterCall`/`codeOffsetForResume` continuation already present on x86
for the chain-loop — extend it so the C++-return edge of a send-bearing J2J
callee lands on an in-JIT continuation rather than rebuilding a full JITState in
C++ per send. Validate: cfibt/cfibs approach cfibx (beat interp, ~3-5x Cog); then
the gate-flip criteria below. This SUPERSEDES the double-pop step 3.

## (SUPERSEDED — double-pop hypothesis) Port plan (task #20) — ordered, arm64-safe

1. Lock baseline: PHARO_X86_JIT=1 min-of-9 for all 5 benches on build-opt;
   arm64 battery -> /tmp/battery_golden.txt; build a -DPHARO_X86_FORCE_V1 escape.
2. Add opt-in knob `PHARO_T1_X86_RESUME_V2` (debug_vars.h) + wire
   PHARO_SP_DEPTH_CHECK at the x86 resume re-entry (per-send sp-drift detector).
3. **The fix** (Interpreter.cpp tryJITResumeInCaller 20404-21243): make the C++
   resume entry obey resumeAfterCall's contract — when the resume lands on the
   continuation, set state.sp to the caller's PRE-send sp (un-pop args + the
   already-pushed retval) and load retval into the rax slot the continuation
   reads, so the continuation's nArgs-pop+retval-write reproduces the stack
   exactly ONCE. Gate behind PHARO_T1_X86_RESUME_V2 (this is SHARED C++ arm64
   runs too) until arm64 byte-parity is proven. Target: cfibs 2208->~130ms (3x Cog).
4. (AsmjitT1.cpp emitOne_x86) add the x86 twin of arm64's PHARO_T1_INLINE_SYNC
   endOfSend republish (OFF_SENDARGCOUNT/OFF_IP) — x86's endOfSend (3348) lacks it.
5. (AsmjitT1.cpp) wire emitClosurePush x86 (currently a knob-gated no-op) to store
   interp->closure_ (OFF_INTERP+closureFieldOffset(), NOT JITState.closure@320 —
   J2JSaveLayout.h:46 comment is wrong per docs/x86-v2-save-port.md) at the 3 V2
   push sites, mirroring arm64 925-933. Fixes loopS block-frame resume.
6. Re-measure: require cfibs AND loopS <=3x Cog, no regression on loopD/cfibx/fib.
7. Flip the gate: initializeJIT (19597-19611) default x86 JIT ON; convert
   PHARO_X86_JIT -> opt-out PHARO_X86_NO_JIT. ONLY after a clean full x86 SUnit
   A/B (the ~10 prescan/emit disagree bytecodes + the 'Corrupt stackPointer_'
   miscompile from commit 7af58fdfa are independent of the leak and only surface
   under load — bench-pass is NOT sufficient).

arm64 safety: emit changes (4,5) are in emitOne_x86 / x86-twin emit fns — cannot
alter an arm64 byte. The only arm64-touching surface is shared C++ (step 3); gate
every changed sp computation behind PHARO_T1_X86_RESUME_V2 and re-run arm64
battery==/tmp/battery_golden.txt after EVERY commit to a file arm64 compiles.

## Increment 2 attempt — gate relaxation CORRUPTS (box #19, 2026-06-16)

Confirmed root cause on box, then tried the obvious fix: relax the x86 cross-method
admit from `canSkipJ2JSave` (numIC==0 leaf-only) to `!canBailMidMethod && numIC <=
PHARO_T1_XMETHOD_MAX_IC` (opt-in `PHARO_T1_X86_XMETHOD_SENDS`), mirroring arm64's
xmethodGateOk. Kept the prim-prologue/heap-write exclusions. x86-only emit, opt-in
-> arm64 byte-identical (battery==golden verified locally).

RESULT: **CORRUPTS.** With the knob on:
- correctness probe (cfibx28/cfibt28/cfibs28 printString) -> NO EVAL-RESULT.
- cfibt(28) alone runs **787,959,599 bytecode steps** (correct is ~6M) with
  `chain: actChain=11 actFall=6728` — wrong/runaway recursion, then a result that
  never materializes. SENDS=0 (default) is correct (cfibt28=1346267) and fast
  where applicable (cfibx30=71ms).

CONCLUSION: the leaf-only gate guards a REAL nested-cross-method-J2J bug. The
failing shape is three distinct methods chained (cfibt -> inct -> incc): inct is
admitted as a cross-method callee (save pushed, callee state set, jmp inct), but
inct's OWN inner send (incc) pushes a second save / resumeAfterCall on top of the
caller's cross-method save, and the nested save-stack/frame bookkeeping corrupts
(receiver/sp drift -> wrong recursion). Naive gate-relaxation is INSUFFICIENT;
arm64 handles this via its full frame-state-residency path. The real fix is the
cross-method-send-bearing callee frame + save/resume correctness (lldb on box:
break at the inct cross-method admit + the incc resumeAfterCall, watch the save
cursor/depth + receiver across the nested unwind). The knob is kept as the
reproducer. Self-rec nesting works (cfibx); cross-method nesting is the gap.

## Increment 2 — bug LOCALIZED by save-stack trace (box #20, 2026-06-16)

Built the FIRST minimal deterministic repro of the cross-method corruption (prior
sessions could only repro at full-startup scale): two-level, NON-recursive —
`cleaf ^self+100`, `cmid ^(self+10) cleaf`, `[N timesRepeat: [5 cmid]]` under
PHARO_X86_JIT=1 PHARO_T1_X86_XMETHOD_SENDS=1 runs away (793M steps; `5 cmid`
should be ~10 bytecodes). So the bug is NOT recursion-specific; the minimal unit
is a send-bearing cross-method callee making ONE inner J2J send.

PHARO_T1_X86_J2J_DBG trace (saves=(cursor-entry)/40 at admit-PUSH + prelude-POP):

  CONTROL cfibx (leaf incc): BALANCED — PUSH saves=1 -> POP saves=0, every time.
    incc returns via the in-JIT V2 return prelude (the POP fires).

  BROKEN cmid (send-bearing): almost ALL PUSH, NO in-JIT POP. saves oscillate
    1<->2 (nested cmid->cleaf reaches 2) but the return-prelude POP NEVER fires
    for the send-bearing callees. The cursor is instead reset by a C++ bail/
    resume path. (Traced callees are findElementOrNil:/hash — SENDS=1 corrupts
    STARTUP itself, so the cmid repro is representative.)

ROOT CAUSE (localized): a send-bearing cross-method callee is admitted (save
pushed by the in-JIT admit at AsmjitT1 ~2970), but it does NOT return via the
in-JIT V2 return prelude (emitJ2JReturnPrelude_x86 ~1655) the way leaf callees do.
Its return/bail exits to C++, and the C++ resume of a cross-method-INLINED callee
frame is what mishandles the frame -> runaway. This is exactly the "re-enter C++
mid-method" hazard the canSkipJ2JSave gate was guarding (comment AsmjitT1 ~2933);
the gate is correct given the C++ resume can't yet reconstruct the cross-method
frame. The leaf-only restriction works precisely because leaf callees always
return via the in-JIT prelude (never the C++ path).

NEXT (scoped): find WHY the send-bearing callee's return doesn't take the in-JIT
prelude — add an RSUM trace at resumeAfterCall + a trace at the callee's return
opcode; then fix the C++ cross-method-frame resume (the chain loop / materialize
in tryJITResumeInCaller, Interpreter.cpp ~20404) to reconstruct an inlined
cross-method callee's caller correctly (mirror arm64, which admits these via
xmethodGateOk + a working materialize). Reproducer + trace knobs committed.

## Increment 2 — deeper diagnosis (box #21, 2026-06-16): fundamental, not an edge case

PHARO_J2J_MAT_LOG + PHARO_SP_DEPTH_CHECK on the SENDS=1 startup corruption:
- SENDS=0: 49 flags (all benign startup sp-depth delta=1, e.g. getSystemAttribute:).
- SENDS=1: 126 flags, incl. 40 `tryJITActivation-exit IP-OUT-OF-RANGE` on send-
  bearing callees (e.g. sel=on: bcOff=16==bcLen=16, a resume-past-end), plus extra
  sp-depth delta=1.

Hypothesis tests (ruled out narrow causes):
- NOT recursion-specific: two-level non-recursive (block->cmid->cleaf) runs away.
- NOT tail-send-specific: cmid=^(self+10) cleaf (tail) AND cmidnt=^((self+10) cleaf)+0
  (non-tail) BOTH run away under SENDS=1. The bcOff==bcLen IP-OUT flags are partly
  benign (normal return-point over-flagging).

CONCLUSION: the corruption is FUNDAMENTAL to admitting a send-bearing cross-method
callee — when such a callee bails to C++ mid-method (which send-bearing methods do:
cold/poly inner sends, cond-jumps, etc.), the C++ resume/materialize of the cross-
method-INLINED callee frame is wrong (sp-depth drift + bad resume ip) -> runaway.
The x86 cross-method admit + save/return path was built for canSkipJ2JSave (leaf)
callees, whose entire execution stays in-JIT (return via the V2 prelude, never the
C++ path). Leaf-only is the correct gate for the CURRENT machinery.

SCOPE: the real fix is porting arm64's cross-method-callee materialize/resume (the
full frame-state-residency path arm64 uses behind xmethodGateOk/MAX_IC=8) to x86 —
a substantial, bug-prone effort, NOT a localized edit. AND it is only ONE of the
blockers to flipping PHARO_X86_JIT default-on: the gate banner also cites ~10
prescan/emit disagree bytecodes + a "Corrupt stackPointer_" miscompile (commit
7af58fdfa), independent of cross-method. Realistically a multi-session campaign.

STATUS: Increment 1 (leaf-only cross-method) is the validated cross-method win
(cfibx 6.4x with PHARO_X86_JIT=1). Increment 2 (send-bearing) needs the arm64
materialize port. Reproducer + PHARO_T1_X86_J2J_DBG/MAT_LOG knobs committed for
that work.
