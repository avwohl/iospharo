# x86 V2 J2J-save port — vetted implementation plan

> **2026-06-15 RESULT (box, HEAD b10d2597): V2 self-rec PASSES, cross-method
> still corrupts — the receiver swap is NOT in the save mechanism.**
> STEP 4 (V2 self-rec, no XMETHOD): battery==golden, and the nArgs>0 benches are
> exact — sumTo:acc:=5050, xsum:=5050, rfib(24)=46368, cfibx(24)=121392 (=fib(26)-1,
> correct; cfibx adds 1/node). So the V1->V2 conversion is clean: no self-rec
> regression, no arg-pop double-pop. STEP 5 (XMETHOD+ALLARGS): the SAME
> `ShouldNotImplement #new: ByteSymbol class` as V1. **V1 and V2 cross-method
> corrupt IDENTICALLY**, so the class-receiver swap lives in the SHARED cross-
> method admit / callee-setup (AsmjitT1.cpp ~2862-3001: newReceiver=sp[-8*(nArgs+1)],
> callee method/literals/jitMethod writes, dynamic nil-fill), which is byte-
> identical V1/V2 — NOT in the save/restore. The reconcile's "PC-relative caller
> re-derivation is the cure" hypothesis is DISPROVEN. The V2 port stands as a
> validated-correct foundation (kept; default-on for x86, escape -DPHARO_X86_FORCE_V1),
> but the cross-method fix is a SEPARATE task: debug the admit callee-setup (add a
> trace logging newReceiver's class when the callee is class-side, under full
> startup; the minimal repro doesn't fire cross-method). The save-mechanism red
> herring is now ruled out by construction.


Source: design workflow `wf_ca97bfc2-967` (2026-06-15, 12 agents: 6 maps → synthesis →
4 adversarial verifiers → reconcile). Verdict: **go-with-revisions**.

## Why (the bug)
x86 cross-method inline-J2J swaps CLASS receivers during startup (String class →
Symbol class ⇒ `ShouldNotImplement #new: ... ByteSymbol class`). Ruled out:
bail-leak, recompile, nArgs, frame-size, isolated repro. The fix is to put x86 on
arm64's proven **V2 packed save** (40B) instead of the bit-rotted V1 (56B).

## Two corrections the review nailed (do NOT relitigate)
1. **The closure is a RED HERRING for the default path.** The closure CONSUME
   (Interpreter.cpp:24059) is gated `saveJM->isBlock && PHARO_T1_RESUME_INTERNAL_J2J`,
   and every x86 inline-J2J SENDER is a non-block method (`!g_emitIsBlock`), so
   `saveJM->isBlock` is false and the branch never runs. It is also validate-and-
   degrade (can't turn a wrong receiver right). The REAL cure is V2's **PC-relative
   caller re-derivation in a `resumeAfterCall` continuation + static-immediate
   arg-pop**, replacing V1's stored-pointer restore (AsmjitT1.cpp:1653-1662) +
   dynamic save[48] arg-pop (1666-1676) — the plausible corruption site.
2. **Closure source is `interp->closure_`** (OFF_INTERP=40 + `Interpreter::closureFieldOffset()`),
   NOT `JITState.closure@320`. Match the arm64 codegen (emitClosurePush:923-931),
   not the J2JSaveLayout.h:46 comment.

## C++ consume = ZERO x86 work (already V2-ready, arch-neutral, `#if PHARO_J2J_SAVE_V2`)
materializeJ2JSaveIntoFrame (Interpreter.cpp:23894; derives saveJM via
findMethodByPC(save.addr()), saveIp via bcStart()+bcOff(), nArgs via save.nArgs()),
the bail-materialize loop (25085-25120, the x86 default mechanism), findMethodByPC
(CodeZone.hpp:420 binary-search), the GC round-trip (prepareForGC/afterGC are
`#if !PHARO_J2J_SAVE_V2` → SKIPPED on V2; packed bcOff is GC-stable), forEachRoot.
The J2JSave struct + JSV_* asserts auto-switch to 40B on the gate flip.
**There is NO default-config C++ J2J push on x86** (the rj2j path 21111-21476 is the
opt-in PHARO_T1_RESUME_INTERNAL_J2J path, already dual-protocol).

## All work is in AsmjitT1.cpp emit (V2 layout: [0]sp [8]recv [16]tempBase [24]resumeAddr-PACKED [32]closure, SIZE=40)
Packed resumeAddr = `addr(bits0-47) | bcOff<<48 | nArgs<<60`, bcOff=globalIdx+1.
x86 has no `movk` → build high bits with `movabs(rTmp, (nArgs<<60)|(bcOff<<48))` +
`or_(rAddr, rTmp)`. Emit-time gate: refuse admission if `(globalIdx+1)>0xFFF || nArgs>0xF`.

## Ordered steps (each compiles on arm64 + is individually verifiable)

- **STEP 0** (no behavior change, V1): add an x86 twin of `emitClosurePush`
  (AsmjitT1.cpp:922-932 is arm64-only); knob-gated PHARO_T1_RESUME_INTERNAL_J2J,
  source `interp->closure_`. Dead code until STEP 3. Verify arm64 byte-identical +
  battery==golden.
- **STEP 1** (no behavior change, V1): bind `g_codeStartLabel` at code offset 0 in
  the x86 compile branch (mirror arm64 9401-9402). The `resumeOverrides` vector +
  `g_resumeOverridesPtr` (8927/8931) + the override-apply loop (10171-10178) are
  ALREADY shared — only the emplace_back call-site + bind are arm64-only. No
  overrides emitted yet. Verify x86 battery==golden, self-rec 5/5 (box).
- **STEP 2** (no behavior change, V1): add tripwire `static_assert(!PHARO_J2J_SAVE_V2)`
  inside the DEFAULT self-rec push (near 2980) AND in emitJ2JReturnPrelude_x86
  (near 1636), matching the existing one at 2831. Forces the eye to the default-on
  path at flip time.
- **STEP 3** (the conversion, still `#if`-guarded so x86 builds as V1): re-author all
  three x86 pushes (2882 xmethod, 2947 selfRecAdmit, 2980 default self-rec) AND the
  prelude (1633) with a V1 path and a V2 path under `#if PHARO_J2J_SAVE_V2`/`#else`,
  replacing every literal 24/32/40/48/56 with JSV_SP/JSV_RECEIVER/JSV_TEMPBASE/
  JSV_RESUMEADDR/JSV_CLOSURE/JSV_SIZE. Add the x86 `resumeAfterCall` continuation
  (mirror arm64 8777-8804): pop `8*nArgs` static, write retval (rax) at [sp-8], store
  sp, clear OFF_EXIT, and IF g_emitX86Xmethod re-establish caller context PC-relatively
  (`lea r9,[rip+g_codeStartLabel]; sub sizeof(JITMethod)`; restore OFF_JITMETHOD/
  OFF_METHOD/OFF_LITERALS/OFF_ARGCOUNT), then `g_resumeOverridesPtr->emplace_back(globalIdx+1, resumeAfterCall)`;
  fall through to endOfSend (label split so inline-spec fall-throughs skip it). V2 push
  resumeAddr points at resumeAfterCall (NOT bcLabels[globalIdx+1]). Also write a V2
  path for the x86 C++ chain-loop push (Interpreter.cpp:24814-24840) + null guard at
  the 24976/24982 pop (fall back to state.jitMethod). Verify x86 still V1-green.
- **STEP 4** (THE FLIP): J2JSaveLayout.h:34-38 set PHARO_J2J_SAVE_V2=1 for x86
  (per-arch form for bring-up; runtime A/B impossible — compile-time struct). DELETE
  the 2831 static_assert + the two STEP-2 tripwires. Change `#if !PHARO_J2J_SAVE_V2`
  at AsmjitT1.cpp:11096 → `#if defined(__x86_64__) || defined(_M_X64)` (keep the
  x86HasMidBail ExtSend exclusion alive under V2). Fix the stale J2JSaveLayout.h
  comments (13-17 "56→32" → 40; 27-33 "x86 stencil tier" → false). FIRST VALIDATION:
  x86 self-rec ONLY (XMETHOD unset) — cfib/rfib/benchFib 5/5, battery==golden, PLUS a
  NEW nArgs>0 self-rec bench (2-arg recursive accumulator) matching interp. arm64
  battery==golden re-run.
- **STEP 5** (cross-method bug-fix gate): enable PHARO_T1_X86_XMETHOD **AND
  PHARO_T1_X86_XMETHOD_ALLARGS** (without ALLARGS the test exercises only the
  already-working nArgs==0 path → false pass). Full startup under PHARO_DET_SCHED=1.
  BINARY done: NO ShouldNotImplement #new:, NO String→Symbol class swap. Add an
  explicit nArgs>0 cross-method bench; confirm cfibx/cfibs ≈ arm64 ratio. If residual:
  A/B-isolate the continuation arg-pop vs findMethodByPC staleness BEFORE the closure knob.
- **STEP 6** (closure, ONLY if STEP 5 leaves a block-CALLEE residual): test
  PHARO_T1_RESUME_INTERNAL_J2J=1; widen the 24059 gate only with a repro + GC-root audit.
- **STEP 7** (cleanup): full SUnit A/B (x86 V2 vs x86 V1 vs arm64), 0 deterministic
  regressions; remove V1 `#else` branches; collapse the per-arch gate; optionally
  delete selfRecAdmit (route to default push).

## Critical holes (RESOLVED into the steps above) + residual risks
- **Validate at ALLARGS config** or the test is a false pass (nArgs>0 is the broken shape).
- **Tripwires + JSV_ macros** on the default-on self-rec push + prelude (they use hardcoded
  literals today → silent breakage on flip).
- **x86 C++ chain-loop push (24814-24840) uses V1 fields** → won't compile under V2; convert
  + null-guard the 24976/24982 pop.
- **canSkip x86HasMidBail exclusion is `#if !PHARO_J2J_SAVE_V2`** → vanishes under V2 →
  ExtSend leaf leak; change to x86-arch guard.
- **findMethodByPC null-deref on eviction** (24982, 21348): add fallback-to-state.jitMethod.
  Eviction-LIVENESS (pushed caller must stay live until pop) must be audited at x86
  eviction/full-flush sites.
- **nArgs>0 static-immediate arg-pop double-pop**: prelude must write OFF_SP = raw save[0]
  with NO nArgs subtraction; ALL subtraction once, in resumeAfterCall (cross-check arm64
  4191 + 8779). cfib/rfib (nArgs==0) CANNOT detect a double-pop → the nArgs>0 bench is a HARD gate.
- **x86 packed-addr build cost** (movabs+or per push, no movk): if cfib/cfibx regress,
  precompute the loop-invariant high constant.
- **48-bit addr pack** (kAddrMask): add an init-time assert that the code zone maps in low 48 bits.
- **g_codeStartLabel RIP-relative lea**: verify asmjit emits clean `lea r9,[rip+disp]`
  (capstone via PHARO_T1_DUMP_SEL).
- **prim-prologue J2J-return shim is arm64-only** (9438-9455); exclude hasPrimPrologue from
  x86 default self-rec admit OR mirror the shim.
