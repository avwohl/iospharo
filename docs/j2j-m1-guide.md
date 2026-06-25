# M1 Implementation Guide (J2J chain-continuity)

Produced by the m1-impl-guide workflow. Companion to docs/j2j-chain-continuity.md M1.

All confirmed. Now I'll produce the complete implementation guide.

---

# M1 IMPLEMENTATION GUIDE — Site-1 ExitSend in-loop resolution (PHARO_T1_CHAIN_RESEND)

## 0. ARCHITECTURE CORRECTION (read this first — it changes the edit)

The three input maps describe two *different* ExitSend points and conflate them. Verified against `src/vm/Interpreter.cpp` HEAD:

- **The chain loop is** `for (int chainLimit = 0; chainLimit < maxChain; chainLimit++)` at **line 25881**, dispatching `switch (state.exitReason)` at **25922**.
- **`case jit::ExitSend` (26224)** already resolves the *caller's own* uncached send: it decodes the selector (26309–26332), reads the receiver (26339), runs `probeCache`/`lookupMethod` (26358–26367), patches the IC via `patchJITICAfterSend` (26390), sets `chainTarget = resolved` (26412), and `break`s into the shared send-chain code at 27199. So **that** ExitSend is *not* the M1 fall — it already chains.
- The **shared send-chain code (27199)** inline-J2J-enters `chainTarget` via `JIT_CALL(chainJM->codeStart(), &state)` at **27530**.
- **The M1 fall is reached when that inline-entered callee exits non-ExitReturn.** Control falls past the `ExitReturn` fast path (27621) into the FALLBACK at 28002, materializes the caller SavedFrame (28014–28036) + the callee's accrued J2J frames (28038–28056), and at **28078** does `jitJ2JActFalls_++`. The sub-counter switch at **28079–28084** attributes by `state.exitReason` — which here is the **inner callee's** exit reason. `jitStencilFallSend_++` (28081) fires precisely when **the callee that was just inline-entered itself exited `ExitSend`** (it hit its own uncached send mid-body that the stencil couldn't resolve).

So M1's prize at `jitStencilFallSend_` (28046/28081, ~675k/run) is: **the inline-J2J callee bailed out with an uncached send of its own. Resolve that send (selector+receiver are live on `state`) and re-enter the resolved grand-callee in JIT inside the loop, instead of materializing the whole stack and handing back to the interpreter.**

The good news: the resolution logic you need already exists verbatim in `case jit::ExitSend` (26309–26412). The M1 edit is essentially "run the 26224 resolver, then the 27199 inline-J2J entry, a second time on the callee's pending send" — but spliced in *before* the materialize at 28038, gated, and only for `state.exitReason == ExitSend`.

---

## 1. THE EDIT

**Insertion point:** `src/vm/Interpreter.cpp`, immediately after line **28036** (after the caller-SavedFrame push completes) and **before** line 28038 (`// Materialize any J2J frames the callee accumulated`). At this point `state` still holds the *callee's* post-bail register state (its `ip`, `sp`, `receiver`, `sendArgCount`, `icDataPtr`, `method`, `jitMethod`) and nothing has been materialized into the interpreter globals yet (the global sync happens at 28066–28076). That is the only window where re-entry is cheap.

**Knob declarations** — add to `src/vm/debug_vars.h` (X-macro list; NOT DebugSettings.cpp which is frozen):

```c
DEBUG_BOOL(PHARO_T1_CHAIN_RESEND)         // enable in-loop ExitSend re-resolution
DEBUG_BOOL(PHARO_T1_CHAIN_RESEND_VERIFY)  // dual-run oracle: resolve+compare, take OLD path
```

**Important sequencing problem with the caller-SavedFrame push:** the FALLBACK at 28014–28036 already pushed a SavedFrame for the *caller* (so the interpreter can return into it). If M1 instead re-enters in JIT, that caller frame must remain on `savedFrames_` (it is the J2J-save chain the resumed grand-callee will return through), which is exactly what we want — re-entering the grand-callee in JIT leaves the caller's materialized SavedFrame in place as its return target. So **do not** undo the 28014–28036 push. Insert M1 *after* it, treating that frame as the resume target. This matches how the existing `continue` paths (27885–27919) leave already-materialized J2J frames on the stack.

### C++ sketch (insert between 28036 and 28038)

```cpp
                        // ============================================================
                        // M1: PHARO_T1_CHAIN_RESEND — in-loop ExitSend re-resolution.
                        // The inline-J2J callee bailed with its OWN uncached send
                        // (state.exitReason==ExitSend). Resolve that send and re-enter
                        // the grand-callee in JIT here, instead of materializing the
                        // rest of the stack and handing back to the interpreter.
                        // Only ExitSend (NOT ExitSendCached) in M1: fresh lookup, no
                        // IC-already-populated nesting hazard (§3 of the plan doc).
                        // The caller SavedFrame pushed at 28014-28036 STAYS — it is the
                        // grand-callee's return target through the J2J chain.
                        if (state.exitReason == jit::ExitSend
                                && (GET_DEBUG_BOOL(PHARO_T1_CHAIN_RESEND)
                                    || GET_DEBUG_BOOL(PHARO_T1_CHAIN_RESEND_VERIFY))) {

                            // --- (a) RESOLVE: mirrors case jit::ExitSend, 26309-26412.
                            //     state.ip is at the send bytecode (callee bailed BEFORE
                            //     advancing past it — same as the top-level ExitSend
                            //     case, which reads *state.ip at 26342/26429).
                            Oop reSel;
                            if (state.icDataPtr) {
                                uint64_t selBits = state.icDataPtr[18];
                                if (selBits == 0) {
                                    // post-GC side-channel fallback (mirror 26312-26326)
                                    if (auto* jm = jitRuntime_.methodMap().lookup(state.method.rawBits())) {
                                        if (jm->numICEntries > 0) {
                                            uint8_t* icStart = jm->icZoneStart();
                                            ptrdiff_t off = reinterpret_cast<uint8_t*>(state.icDataPtr) - icStart;
                                            if (off >= 0 && (off % jit::IC_BYTES_PER_SITE) == 0) {
                                                uint32_t si = (uint32_t)(off / jit::IC_BYTES_PER_SITE);
                                                if (si < jm->numICEntries)
                                                    if (uint64_t* sba = jm->selBitsArray()) selBits = sba[si];
                                            }
                                        }
                                    }
                                }
                                reSel = Oop::fromRawBits(selBits);
                            } else {
                                reSel = state.cachedTarget;  // inline stencil bail / Tier 2
                            }

                            int reArgs = state.sendArgCount;
                            bool reElig = reSel.isObject() && reSel.rawBits() >= 0x10000;
                            Oop reRcvr, reResolved;
                            bool reSuper = (state.ip && *state.ip == 0xEB);
                            if (reElig) {
                                reRcvr = state.sp[-(reArgs + 1)];           // receiver under args
                                if (reSuper) {
                                    Oop mc = methodClassOf(state.method);
                                    Oop sc = (mc.isNil() || !mc.isObject())
                                        ? superclassOf(memory_.classOf(reRcvr))
                                        : superclassOf(mc);
                                    reResolved = lookupMethod(reSel, sc);
                                } else {
                                    Oop rc = memory_.classOf(reRcvr);
                                    MethodCacheEntry* ce = probeCache(reSel, rc);
                                    reResolved = ce ? ce->method : lookupMethod(reSel, rc);
                                    if (!ce && !reResolved.isNil()) cacheMethod(reSel, rc, reResolved);
                                }
                                // Standard CompiledMethod only (mirror 26369-26373);
                                // DNU / non-standard -> let the OLD path handle it.
                                reElig = reResolved.isObject() && reResolved.rawBits() >= 0x10000
                                    && reResolved.asObjectPtr()->classIndex() == compiledMethodClassIndex_;
                            }

                            // --- (b) JIT-ELIGIBILITY: must be JIT-compiled + safe to
                            //     inline-enter, the SAME predicate the shared chain code
                            //     gates on at 27232/27249-27251.
                            jit::JITMethod* reJM = reElig
                                ? jitRuntime_.methodMap().lookup(reResolved.rawBits()) : nullptr;
                            bool reJitOk = reJM && reJM->isExecutable();
                            if (reJitOk) {
                                bool hasPrim = (reJM->methodHeader >> 16) & 1;
                                reJitOk = (!hasPrim || reJM->hasPrimPrologue)
                                    && (!g_debug.inlineActivateNoBailMid || !reJM->canBailMidMethod)
                                    && (g_debug.inlineActivateStubs || !reJM->isStubOnEntry);
                            }

                            // ========================================================
                            // VERIFY ORACLE — see §3. Resolve-and-compare ONLY; do NOT
                            // re-enter the callee. Then fall through to the OLD path.
                            // ========================================================
                            if (GET_DEBUG_BOOL(PHARO_T1_CHAIN_RESEND_VERIFY)) {
                                chainResendVerify(state, reSel, reArgs, reRcvr,
                                                  reResolved, reSuper, reElig, reJitOk);
                                // INTENTIONAL: no re-entry. Drop to materialize at 28038.
                            }
                            else if (reJitOk) {
                                // ====================================================
                                // (c) RE-ENTER in JIT. Patch the callee's IC so the
                                //     next hit is ExitSendCached (mirror 26385-26409),
                                //     then set up callee-of-callee state exactly as the
                                //     shared chain code does at 27508-27530 and the
                                //     block-resume template at 28178-28196.
                                // ====================================================

                                if (!reSuper && state.icDataPtr) {
                                    // INV 2 (stale IC): patchJITICAfterSend rewrites the
                                    // *callee's* IC site (state.icDataPtr) with the
                                    // resolved target, so the NEXT hit is ExitSendCached
                                    // not ExitSend. Mirrors 26387-26390. We are about to
                                    // null state.icDataPtr below, so capture it first.
                                    pendingICPatch_      = state.icDataPtr;
                                    pendingICSendArgCount_ = reArgs;
                                    pendingICOwnerMethod_  = state.method;  // callee owns the site
                                    patchJITICAfterSend(reResolved, reRcvr, reSel);
                                }
                                jitRuntime_.noteMethodEntry(reResolved);

                                // INV 12 (frame-identity): advance the CALLEE's ip past
                                // its send bytecode so its materialized SavedFrame /
                                // resume lands AFTER the send, not on it. Mirrors the
                                // shared-code IP advance at 27204-27213, applied to
                                // state.ip (the callee's ip) rather than instructionPointer_.
                                {
                                    uint8_t so = *state.ip;
                                    state.ip += (so == 0xEA || so == 0xEB) ? 2 : 1;
                                }

                                // Set up grand-callee entry. Receiver+args are live on
                                // state.sp; tempBase/literals/ip are the grand-callee's.
                                // Mirrors 27508-27530 (cursor reset + JIT_CALL) and the
                                // block-resume template 28178-28196.
                                ObjectHeader* gMethObj = reResolved.asObjectPtr();
                                Oop* gFp   = state.sp - (reArgs + 1);
                                state.receiver  = reRcvr;
                                state.tempBase  = gFp + 1;
                                state.literals  = gMethObj->slots() + 1;
                                state.argCount  = reArgs;
                                state.method    = reResolved;
                                state.jitMethod = reJM;
                                {
                                    Oop hdr = gMethObj->slots()[0];
                                    int nl = hdr.isSmallInteger() ? (hdr.asSmallInteger() & 0x7FFF) : 0;
                                    state.ip = gMethObj->bytes() + (1 + nl) * 8;  // grand-callee bc start
                                }

                                // INV 2 (stale IC): clear identification BEFORE re-enter
                                // so the grand-callee never pairs the OLD send's IC with
                                // its own ip. Mirrors 21096-21106 / 28190-28192 / 27935-27937.
                                state.icDataPtr   = nullptr;
                                state.cachedTarget = Oop::fromRawBits(0);
                                state.sendArgCount = 0;
                                state.exitReason  = jit::ExitNone;

                                // INV 5 (packable resume) + INV 11 (GC cursor): reset the
                                // J2J save slice to its base for the grand-callee, exactly
                                // as 27508-27525. currentJITState_ is already == &state
                                // for the whole loop (set at loop entry, INV 11), so no
                                // re-registration is needed. The grand-callee's own sends,
                                // if packable (bcOff<=0xFFF && nArgs<=15), pack via the
                                // emitted prelude; the J2J-call conversion's packable
                                // predicate at 21476-21524 still gates them — we add no
                                // new save that bypasses it.
                                state.j2jSaveCursor = reinterpret_cast<uint8_t*>(&j2jPool_[j2jStateBase]);
                                state.j2jSaveLimit  = reinterpret_cast<uint8_t*>(&j2jPool_[j2jStateEnd]);
                                state.j2jDepth        = 0;
                                state.j2jEntryDepth   = 0;
                                state.j2jEntryCursor  = state.j2jSaveCursor;
                                state.j2jTotalCalls   = 0;
                                state.yieldCountdown  = 1000;

                                // --- Enter grand-callee JIT code (mirror 27530) ---
                                JIT_CALL(reJM->codeStart(), &state);
                                jit::fsrLazyRefresh(state);          // FSR M4 (mirror 27531)
                                chargeJITBytecodes(state);           // mirror 27555
                                jitJ2JStencilCalls_ += 1 + state.j2jTotalCalls;

                                if (state.exitReason == jit::ExitReturn) {
                                    // Grand-callee returned cleanly. Restore the CALLEE
                                    // as the resume frame: pop grand-callee args, write
                                    // retVal to the callee's send-result slot, and let
                                    // the callee resume in JIT via the shared-code
                                    // continue. We re-drive the callee through the same
                                    // post-send resume that the 27621 fast path uses.
                                    // Simplest correct M1 form: count the chain and fall
                                    // into the existing materialize+resume by NOT
                                    // re-materializing — write retVal then continue the
                                    // loop so the next iteration resumes the callee.
                                    Oop gRet = state.returnValue;
                                    state.sp[-(reArgs + 1)] = gRet;
                                    state.sp -= reArgs;
                                    // INV 12: restore CALLEE identity (its ip is already
                                    // past-send from the advance above; its method/recv/
                                    // tempBase were overwritten — re-derive from the
                                    // caller SavedFrame we left at 28014-28036).
                                    //   NOTE: see §4 RISK — this restore is the single
                                    //   easiest invariant to get wrong.
                                    state.method    = savedFrames_[frameDepth_ - 1].savedMethod;
                                    state.receiver  = savedFrames_[frameDepth_ - 1].savedReceiver;
                                    state.tempBase  = savedFrames_[frameDepth_ - 1].savedFP + 1;
                                    {
                                        ObjectHeader* cMO = state.method.asObjectPtr();
                                        state.literals = cMO->slots() + 1;
                                    }
                                    state.icDataPtr    = nullptr;   // INV 2
                                    state.cachedTarget = Oop::fromRawBits(0);
                                    state.sendArgCount = 0;
                                    state.exitReason   = jit::ExitReturn; // drive ExitReturn resume
                                    // We consumed the caller SavedFrame as the resume —
                                    // pop it so we don't double-return through it.
                                    frameDepth_--; if (chainCallDepth > 0) chainCallDepth--;
                                    materializeJ2J();
                                    jitJ2JActChains_++;             // NOT jitJ2JActFalls_
                                    if (checkCountdown_ <= 0) goto jit_loop_exit;
                                    continue;                       // resume callee in JIT
                                }
                                // Grand-callee itself bailed non-return. Materialize its
                                // J2J frames and fall through to the OLD path (which will
                                // materialize + return true). Do NOT re-recurse in M1 —
                                // one resolution hop per fall keeps the change bounded.
                                if (state.j2jDepth > 0) {
                                    J2JSave* gp = splitPool ? &j2jPool_[j2jStateBase] : j2jStack;
                                    for (int i = 0; i < state.j2jDepth; i++) {
                                        if (frameDepth_ >= StackOverflowLimit) break;
                                        SavedFrame& gf = savedFrames_[frameDepth_++];
                                        if (!materializeJ2JSaveIntoFrame(gf, gp[i], state.jitMethod, "site7-resend")) {
                                            if (frameDepth_ > 0) frameDepth_--;   // INV 9 rollback
                                            break;
                                        }
                                    }
                                    chainCallDepth += state.j2jDepth;
                                }
                                // state now holds the grand-callee's bail; the OLD path
                                // at 28038+ materializes it and bails. counter NOT bumped
                                // here (the OLD path's 28078 will do it for this leg).
                            }
                        }
                        // ============================================================
                        // (existing code resumes here, UNCHANGED)
                        // Materialize any J2J frames the callee accumulated
                        if (state.j2jDepth > 0) {
```

The grand-callee-bailed tail intentionally falls into the existing 28038 materialize + 28078 fall — so a *failed* resend costs one extra lookup+JIT_CALL but is otherwise byte-identical in outcome to the old path (still a fall, still `jitStencilFallSend_++`). Only the *successful* resend reaches the `continue` at the `jitJ2JActChains_++`.

---

## 2. INVARIANT CHECKLIST (inline above; consolidated here)

```
inv  where in sketch                       mirrors guard            protects against
---  --------------------------------       ----------------------   -------------------------------
2    clear icDataPtr/cachedTarget/          Interpreter.cpp:         wrong method @ wrong offset
     sendArgCount BEFORE re-enter             21096-21106            (minExtent width-coord corruption)
     (two sites: grand-callee setup +         28190-28192
     callee-resume restore)                    27935-27937
2    patchJITICAfterSend on the callee's    Interpreter.cpp:         next send hits ExitSendCached,
     IC site so the next hit is cached        26387-26390           not a perpetual ExitSend re-miss
5    j2jSaveCursor/Limit reset to slice     Interpreter.cpp:         frame re-execution on return /
     base; rely on emitted prelude's          27508-27525           RESUME-MISMATCH (whole-body re-run);
     packable predicate, add no new save      21476-21524 (pack)    M1 adds NO direct J2J save, so the
     that bypasses the 21476 gate                                   existing packable gate still holds
9    materializeJ2JSaveIntoFrame failure    Interpreter.cpp:         partial frame-stack corruption
     rolls back frameDepth_ (--) and breaks   24379-24404, 28045   (the `if(!...) {frameDepth_--;break;}`)
11   currentJITState_ already == &state     Interpreter.cpp:         stale receiver/ip after scavenge;
     for the whole loop; reserve cursor       21023-21029           reserving j2jPool slice before any
     slice before grand-callee sends          19595-19610           op-allocating grand-callee bytecode
12   IP advance on state.ip past the        Interpreter.cpp:         resume lands ON the send -> re-send
     callee's send before re-enter            27204-27213           loop; stale method_/receiver_
12   restore callee identity from the       Interpreter.cpp:         wrong method run on resume
     SavedFrame on grand-callee ExitReturn    28066-28076 (sync),  (§4 RISK — easiest to get wrong)
                                               21900-21919
13   DNU (reResolved.isNil) -> let OLD      Interpreter.cpp:         nil-PC infinite loop; M1 sets
     path handle (reElig=false, skip          26356/26364,         reElig=false so we never re-enter a
     re-enter, drop to materialize)            18832-18843          non-resolved send
```

The two `state.exitReason==ExitSend`-only gates (the outer `if` and the `else if (reJitOk)`) are what keep M1 from touching `ExitSendCached` (M3) and the correctness-required `ExitJ2JCall`/`ExitOther` (out of scope).

---

## 3. THE VERIFY ORACLE (`PHARO_T1_CHAIN_RESEND_VERIFY`)

Add a member helper (declared in `Interpreter.hpp`, defined near the chain loop). It **resolves and compares only** — it must NOT JIT_CALL the grand-callee (that would double-execute side effects). After it runs, control drops straight to the OLD materialize/fall path (the sketch's `if (VERIFY) {...}` arm has no re-enter and no `continue`), so the run's *behavior* is byte-identical to knob-off; only the trap fires on divergence.

```cpp
void Interpreter::chainResendVerify(const jit::JITState& state, Oop reSel, int reArgs,
                                    Oop reRcvr, Oop reResolved, bool reSuper,
                                    bool reElig, bool reJitOk) {
    // Independently recompute what the OLD path's eventual interpreter dispatch
    // WOULD resolve this same send to, and assert the M1 resolver agrees. The
    // OLD path materializes (28038-28090) and returns true; the interpreter then
    // re-decodes the send at state.ip in the materialized callee frame and does
    // its own lookupMethod. We replicate that lookup here from the SAME inputs.
    if (!reElig) return;  // M1 declined (DNU / non-standard) — OLD path also bails; agree by construction

    // (1) selector agreement: the interpreter decodes the selector from the
    //     send bytecode literal, not the IC. Cross-check our IC-derived reSel
    //     against the bytecode-decoded selector (mirror 26429-26437).
    Oop bcSel = Oop::nil();
    if (state.ip && state.method.isObject()) {
        uint8_t so = *state.ip;
        ObjectHeader* cmO = state.method.asObjectPtr();
        Oop hdr = cmO->slotAt(0);
        int nL = hdr.isSmallInteger() ? (int)(hdr.asSmallInteger() & 0x7FFF) : 0;
        if (so >= 0x80 && so <= 0xAF) {
            int li = so & 0x0F;
            if (li + 1 <= nL) bcSel = cmO->slotAt(1 + li);
        }
        // 0xEA/0xEB extended sends decode their literal index from the next
        // byte; for M1 the common 0x80-0xAF short sends cover the bulk —
        // extended-send verify is a follow-up if the trap shows gaps.
    }
    if (bcSel.isObject() && reSel.isObject() && bcSel.rawBits() != reSel.rawBits()) {
        static int n = 0;
        if (++n <= 40)
            fprintf(stderr, "[CHAIN-RESEND-VERIFY] selector divergence: ic=#%s bc=#%s caller=#%s\n",
                    memory_.oopToString(reSel).c_str(), memory_.oopToString(bcSel).c_str(),
                    memory_.selectorOf(state.method).c_str());
    }

    // (2) receiver agreement: receiver is sp[-(nArgs+1)] for BOTH paths; the OLD
    //     path materializes that exact slot (28031-28032 materializedRetSlot).
    Oop oldRcvr = state.sp[-(reArgs + 1)];
    if (oldRcvr.rawBits() != reRcvr.rawBits()) {
        static int n = 0;
        if (++n <= 40)
            fprintf(stderr, "[CHAIN-RESEND-VERIFY] receiver divergence: m1=0x%llx old=0x%llx sel=#%s\n",
                    (unsigned long long)reRcvr.rawBits(), (unsigned long long)oldRcvr.rawBits(),
                    memory_.selectorOf(state.method).c_str());
    }

    // (3) resolved-method agreement: re-run the lookup the interpreter dispatch
    //     would do (probeCache miss -> lookupMethod) and compare to reResolved.
    Oop oldResolved;
    Oop useSel = bcSel.isObject() ? bcSel : reSel;
    if (reSuper) {
        Oop mc = methodClassOf(state.method);
        Oop sc = (mc.isNil() || !mc.isObject())
            ? superclassOf(memory_.classOf(oldRcvr)) : superclassOf(mc);
        oldResolved = lookupMethod(useSel, sc);
    } else {
        Oop rc = memory_.classOf(oldRcvr);
        MethodCacheEntry* ce = probeCache(useSel, rc);
        oldResolved = ce ? ce->method : lookupMethod(useSel, rc);
    }
    if (oldResolved.rawBits() != reResolved.rawBits()) {
        static int n = 0;
        if (++n <= 40)
            fprintf(stderr, "[CHAIN-RESEND-VERIFY] method divergence: m1=#%s old=#%s sel=#%s reJitOk=%d\n",
                    memory_.selectorOf(reResolved).c_str(), memory_.selectorOf(oldResolved).c_str(),
                    memory_.selectorOf(useSel).c_str(), reJitOk);
    }

    // (4) ip agreement: the interpreter resumes the callee AT the send bytecode
    //     (it has NOT advanced — it re-dispatches the send). The OLD materialize
    //     records frame.savedIP = instructionPointer_ which is past-send for the
    //     CALLER, but the CALLEE's resume ip is state.ip (at-send). Assert state.ip
    //     lies within the callee method's bytecode range (mirror 27866 RESUME-MISMATCH).
    if (state.method.isObject()) {
        ObjectHeader* mO = state.method.asObjectPtr();
        size_t nL = memory_.numLiteralsOf(state.method);
        uint8_t* bcS = mO->bytes() + (1 + nL) * 8;
        uint8_t* bcE = mO->bytes() + mO->byteSize();
        if (state.ip < bcS || state.ip >= bcE) {
            static int n = 0;
            if (++n <= 40)
                fprintf(stderr, "[CHAIN-RESEND-VERIFY] ip out of callee range: ip=%p [%p,%p) m=#%s\n",
                        (void*)state.ip, (void*)bcS, (void*)bcE,
                        memory_.selectorOf(state.method).c_str());
        }
    }
}
```

Run-clean criterion: a full kernel-SUnit run under `PHARO_T1_CHAIN_RESEND_VERIFY=1` with **zero `[CHAIN-RESEND-VERIFY]` lines**. Because the verify arm never re-enters, the run's pass/fail and Δcog must be byte-identical to knob-off — confirm that *first* (it proves the oracle is observation-only), *then* trust the divergence count.

---

## 4. RISK CALLOUTS

**Easiest invariant to get wrong: INV 12 on the grand-callee-ExitReturn restore (the `state.method = savedFrames_[frameDepth_-1].savedMethod` block).** When the grand-callee returns, `state.method/receiver/tempBase/literals/jitMethod` all hold the *grand-callee's* values (the `JIT_CALL` ran with them). To resume the *callee* in JIT we must restore the callee's identity — but unlike the 27621 fast path, M1 has already overwritten `savedSP/savedRecv/savedTempBase/savedMethod` is NOT in scope here (those locals live in the 27257–27264 block, a different `{}` scope). The sketch reconstructs the callee from the SavedFrame pushed at 28014–28036, which is correct **only if that frame is the callee's** (it is — it was pushed for `savedMethod`, the caller... ).

**This is the specific failure mode to watch:** the SavedFrame at 28014–28036 records `savedMethod` = the *caller* of the inline-J2J callee, NOT the callee itself (see 28018 `frame.savedMethod = savedMethod`, and `savedMethod` = `state.method` captured at 27264 *before* the inline JIT_CALL, i.e. the caller). So `savedFrames_[frameDepth_-1].savedMethod` gives the **caller**, not the callee whose send we just resolved. Re-deriving callee identity from it runs the **wrong method on resume** — exactly INV 12's failure (wrong method_/receiver_ → wrong method run), and it will surface as a `[RESUME-MISMATCH]` at 27866 or a `mustBeBoolean`/DNU a few sends later, far from the actual bug.

**Mitigation:** capture the callee's identity explicitly *before* the `JIT_CALL(reJM->codeStart())` re-enter — save `Oop calleeMethod = state.method; Oop calleeRecv = state.receiver; Oop* calleeTempBase = state.tempBase; jit::JITMethod* calleeJM = state.jitMethod; uint8_t* calleeResumeIp = state.ip;` (after the IP-advance, before overwriting `state` for the grand-callee) and restore from *those* locals on grand-callee ExitReturn, not from the SavedFrame. This mirrors the 27257–27264 / 27628–27635 save-restore pair exactly. The SavedFrame stays purely as the caller's return target. Update the sketch's restore block accordingly — this is the one place the "mirror the block-resume template" shortcut is wrong, because block-resume has no nested callee to restore.

Second-tier risk: **INV 5 / x86.** M1 re-enters via `JIT_CALL` with a reset save-cursor (27518 pattern). On x86 with a send-bearing grand-callee this is the open `PHARO_T1_X86_XMETHOD_SENDS` orphaned-save hazard (plan §4). M1's grand-callee re-enter is a fresh leaf slice (depth 0, cursor==base) so it is *safe by the same argument as 27511* ("safe for leaf callers"), but if the grand-callee itself chains a send, the cursor-reset orphans nothing only because we don't re-recurse in M1 (the grand-callee-bail tail drops to the OLD materialize). Keep M1 arm64-first per the plan; the x86 path is exercised but bounded to one hop.

---

## 5. BUILD + MEASURE

```bash
cmake --build build

# stage the SUnit runner once, then capture the [M0-ATTRIB] dump (fires at
# shutdown, Interpreter.hpp:1906-1928) for three knob states.
cp scripts/pharo-headless-test/test_classes.txt /tmp/sunit_test_classes.txt
printf 'all\n' > /tmp/sunit_class_names.txt

# (A) baseline — knob off
./build/test_load_image /tmp/harness/Pharo.image 2>&1 | grep 'M0-ATTRIB' > /tmp/resend_off.txt

# (B) verify oracle — must be byte-identical behavior, zero divergence traps
PHARO_T1_CHAIN_RESEND_VERIFY=1 \
  ./build/test_load_image /tmp/harness/Pharo.image 2>&1 \
  | tee /tmp/resend_verify.log | grep -c 'CHAIN-RESEND-VERIFY'   # MUST print 0
grep 'M0-ATTRIB' /tmp/resend_verify.log > /tmp/resend_verify.txt

# (C) enabled
PHARO_T1_CHAIN_RESEND=1 \
  ./build/test_load_image /tmp/harness/Pharo.image 2>&1 | grep 'M0-ATTRIB' > /tmp/resend_on.txt
```

**Pass criteria, read off the `[M0-ATTRIB]` lines** (`site1 stencil-fall cached=.. send=.. j2j=.. other=..` and `continuity=..% (actChain=.. actFall=..)`):

```
metric (M0-ATTRIB field)            (A) off      (C) on            gate
---------------------------------   ----------   ---------------   ----------------------------------
site1 send  (jitStencilFallSend_)   ~675,599     drops by ΔN       ΔN large, positive
actChain    (jitJ2JActChains_)      2,210,222    +ΔN (same ΔN)     rises by ~the send drop
actFall     (jitJ2JActFalls_)       1,708,xxx    -ΔN              falls by the same ΔN
site1 cached/j2j/other              unchanged    UNCHANGED         no leakage into other sub-counters
site2 block falls / site3 activate  unchanged    UNCHANGED         M1 touches only ExitSend
continuity                          56.4%        up               in the predicted direction
```

The load-bearing check is the **conservation identity**: `Δsite1_send == Δactchain == −Δactfall`, with **cached/j2j/other and site2/site3 all flat**. If `site1_other` or `site2` rises, M1 mis-attributed a fall (a resolved-but-not-JIT target leaking into the wrong counter) — investigate before trusting the continuity bump. Confirm (B) prints `0` divergences and matches (A)'s `classify-sunit.py` Δcog before enabling (C). Bench gate: `build-opt` (-O2) rebuild, `benchFib`/`cfib` 5×5 within noise, `PHARO_DET_SCHED=1` A/B identical between (A) and (C).

---

**Files cited:** all line numbers are `/Users/wohl/src/iospharo-jit/src/vm/Interpreter.cpp` (chain loop 25881; switch 25922; `case ExitSend` resolver 26224–26414; shared send-chain 27199; inline-J2J JIT_CALL 27530; ExitReturn fast path 27621; save/restore locals 27257–27264 / 27628–27635; FALLBACK + caller-SavedFrame 28002–28036; **M1 insertion point 28036/28038**; fall counter 28078, sub-counter switch 28079–28084; block-resume template 28165–28203) and `/Users/wohl/src/iospharo-jit/src/vm/Interpreter.hpp` (counters 1471/1472/1489–1492; `[M0-ATTRIB]` dump 1906–1928). Knobs go in `/Users/wohl/src/iospharo-jit/src/vm/debug_vars.h`.