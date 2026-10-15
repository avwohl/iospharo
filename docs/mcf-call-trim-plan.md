# Machine-code frames: trimming the native call and return

Status (2026-10-15): plan from a design panel (three designs, a judge
that checked them against the code at `5d591c5f`).  Step 0 is done:
the push oracle derives the lazy view, runs every check, and checks the
record (method against its JM, ctxSynced, home marker, ret slot,
INV-FP0, INV-AC); INV-AC is also checked at each native return.  It
found that executeFromContext left a resumed block frame's receiver in
fp[0] (fixed: the closure, as every other block frame).  Lexicon runs
with PHARO_MCF_VERIFY and no report.
Today a native call plus return is about 67 instructions (push 41,
return 26); Cog needs about 20.  Lexicon makes about 1e9 native calls a
run.

None of the three designs reaches 40 instructions alone. Combining them does: about 67 -> 40 per native call pair if all of steps 1-8 below land, and about 38 with optional step 9. Lexicon makes about 1e9 native calls a run, so the saving is about 27e9 of 270e9 instructions (about 10%). Nothing was edited, built or run.

**Claims checked against the code (HEAD 5d591c5f)**

1. **Push and return counts.** I re-counted both sequences and got the stated 41 and 26.
   - Push: AsmjitT1.cpp:5569-5667 plus emitMcfMethodEntry 5874-5906.
   - Return: emitMcfReturn_arm64 5174-5333.

2. **The push oracle is broken today. Design 1 is right, but its fix is incomplete.** In MachineCodeFrames.cpp:643, under lazy view the push stores a null savedBytecodeEnd (AsmjitT1.cpp:5636-5639). So every push reports "savedBytecodeEnd", and the else-if chain skips every later check. The oracle also only prints (at most 10 reports) and never calls stopVM.
   - Missed by Design 1: mcfVerifyPush also reads st->method and st->argCount, and lazy view never writes those per call (emitMcfCalleeView skips them).
   - emitMcfVerifyPushCall (5700) only calls emitSyncSpToState, which publishes x19. Nothing calls mcfDeriveView.
   - So fixing line 643 alone would surface false "callee argCount" / "callee sp" reports. mcfVerifyPush must call mcfDeriveView(*st) first.

3. **homeMethod_ does not affect execution: confirmed.** Its readers are 2377 (dump), 7880 (census), 11857 (trace) and 14774 (an empty if body). 19868 is a save/restore pair.
   - savedHomeMethod writers are Interpreter.cpp 16842, 20161, 30897, 34694 and Interpreter.hpp 2229, 2383. Restores are at 17389 and 17552, and the GC visit is at Interpreter.hpp:4368.
   - Deleting the field is safe.

4. **The entry TOS reload is dead: confirmed, and there is one more dead instruction that all three designs missed.**
   - PHARO_T1_TOS_REG is a plain DEBUG_BOOL (debug_vars.h:417), so `g_useTos` (AsmjitT1.cpp:12992) is constant false in release builds. Every reader of x26 is behind `tosFam()` (line 251).
   - The entry `ldur x26` (5901, plus 5796 and 5862 in the call and block stubs) is dead in every build, because compile start is `tos={}`.
   - The return's `mov x26,x1` (5322) is also dead whenever the global PHARO_T1_TOS_REG is off. That is -1 more per call pair.

5. **Depth limit.** The value is set at MachineCodeFrames.cpp:2088 (Design 1 cites 2082) to StackOverflowLimit-64 = 55936 (Interpreter.hpp:724). Its only other reader is mcfPerformTarget (1131).
   - The bit-15 test is correct: depth 0 wraps to all ones, depths 32769-65536 have bit 15 set, and MaxFrameDepth is 65536.

6. **The info word: confirmed.** Every native-push site sets w12 with a 32-bit `mov` of a value below 65536 (6908, 8594, 8734, 12371, 12443, 12490, 12590). A 32-bit mov zero-extends, so bytes 2-7 are 0. The sites at 12750 and 12783 only write OFF_ARGCOUNT; they are not push paths.
   - The only native readers of record fields are in the push and return emitters (5216-5315, 5639-5665). TrampolineAsm.S does not use SavedFrame offsets.

7. **INV-AC (every live record's savedActiveContext equals activeContext_).** I checked all 16 writers of `activeContext_ =`. Every one either materializes and sets frameDepth_=0, or is a popFrame restore.
   - The one real exception: tryRestoreCollapsedFrames (Interpreter.cpp:10892-10945) puts records back while activeContext_ is the widowed returning context. Today the following C++ popFrame repairs this from record.savedActiveContext. Design 2 found this; Design 3 keeps correctness only because its restored records stay FULL.
   - INV-AC fails outside MCF: popFrameForJIT (17555) restores nil for J2J-pushed records. So the field can only be bypassed when mcfEnabled() is true, and not deleted.

8. **Lazy receiver/tempBase (Design 2 part A, Design 1 B1, Design 3 step 6).**
   - emitSyncSpToState (4187-4219) has 81 call sites. emitReloadSpFromState (4240-4253) already reloads x21/x22 at its 13 sites. OFF_SP=0 and OFF_RECEIVER=8, so `stp x25,x21` costs nothing extra.
   - Emitted code that still reads the state copy, at AsmjitT1.cpp 4383, 4407, 7337, 14562, 14628 (and J2J 8812, 9852, 10344, 10353): 7337 is PopStoreRecv. With a stale st.receiver it would store into the wrong object, which is silent heap corruption.
   - The VERIFY return call to pharo_mcf_verify_ret (around 5250-5262) has no emitSyncSpToState in front of it, so mcfVerifyRet -> mcfVerifyReceiver would read stale state. Design 2 caught this; Design 1 did not.

9. **Design 3's eviction hazard is real.** JITCompiler.cpp:1786-1790 frees code (evictLRU) before calling mcfCodeChanged, and pinLiveMethods does not pin a record's mcJM.
   - Today a dangling mcJM is harmless because a native return reads mcJM only after a non-null mcReturn.
   - Any C++ derivation that goes through a record's mcJM (Design 3's method() and argCount()) is a use-after-free in code-zone memory, which ASan cannot see.
   - Rule for the plan: derive from savedMethod or savedFP, never from a record's mcJM.

10. **GC inside an MC session.** Allocation never collects (ObjectMemory.cpp:205-212). The forEachRoot record loop at Interpreter.hpp:4363-4373 visits only indices below frameDepth_. Design 2's in-place slot at index frameDepth_ is therefore not a GC root: it relies on a pull at prepareForGC.

**Scores (correctness / gain / testability, out of 5)**

- **Design 1, trim: 4 / 3 / 5.**
  - All local claims check out. Missed: mcfDeriveView in the oracle fix, the dead `mov x26,x1`, and the sync before verify_ret for B1.
  - B3 (record-pointer register) is underpriced. frameDepth_ is read by hundreds of C++ sites, and deriving it at each exit costs about 3 instructions.
  - Gain is 67 -> 52 locally.
- **Design 2, lazy-state: 3 / 4 / 4.**
  - Part A is sound once the x21 readers are converted. Part C is sound with the tryRestore fix.
  - Part B (in-place frame slots with an ownership flag) is fragile, for four reasons:
    - A C++ pushFrame into the slot at frameDepth_ while machine code owns the slot writes a stale closure_ and currentFrameMaterializedCtx_.
    - Nested mcfRun entries make one global owner flag ambiguous.
    - currentFrameMaterializedCtx_ has 77 references.
    - The slot is not a GC root.
  - Part B gives only about -3 more than step 8 below, at the highest risk.
- **Design 3, minimal record: 2.5 / 4 / 3.**
  - Problems: derivations through mcJM (the eviction use-after-free), the method becoming a weak root, a 340-reference rename, and the OFF_IP/OFF_TEMPBASE swap in JITState, which touches JIT_CALL, TrampolineAsm.S and the x86 backend to save one store per exit.
  - Worth keeping: deriving savedClosure from savedFP[0] (INV-FP0), and the poison/expand-check ideas.

**Synthesized plan, ordered by gain per risk**

Running totals are instructions per call pair. Each emitter step gets a DEBUG_BOOL_RT opt-out in src/vm/debug_vars.h. Every step gates on:
- the probes compared byte for byte with PHARO_NO_JIT, with and without PHARO_DET_SCHED=1;
- PHARO_MCF_VERIFY on build-dbg, silent, with each oracle first proven by sabotage;
- the full SUnit sweep compared with the step-0 baseline (classify-sunit.py, LC_ALL=C sorted);
- lexicon instructions retired (time -l, arms interleaved), with PHARO_MCF_COUNT_CALLS counts unchanged so the saving is not a rise in refusals, and the [JIT] Stats line checked for refused compiles;
- an lldb single-step of one call+return to confirm the count.

**Step 0: oracles and baseline. No behaviour change, gain 0. -- DONE** Files: MachineCodeFrames.cpp, AsmjitT1.cpp.
- mcfVerifyPush:
  - call mcfDeriveView first;
  - accept a null bcEnd under lazy view;
  - make every check independent, count failures and stopVM;
  - add checks: savedMethod == mcJM->compiledMethodOop, ctxSynced==0, homeFrameDepth==SIZE_MAX at method entry, retSlot==nullptr.
- Put emitSyncSpToState in front of the pharo_mcf_verify_ret call.
- New VERIFY checks:
  - (a) INV-AC: at each native return, record.savedActiveContext == activeContext_.
  - (b) INV-FP0: closure == (savedMethod is a CompiledBlock ? savedFP[0] : nil) at push and return.
  - (c) At each sync point, x21 == st.receiver and x22 == st.tempBase (brk with its own code, like the 0xF14 oracle).
- Sabotage knobs: emit ctxSynced=1, skip one popFrame activeContext_ restore, skip one `mov x21`.
- Run lexicon, myprecious and the full sweep under VERIFY + DET_SCHED. Fix tryRestoreCollapsedFrames now if (a) fires there.

**Step 1: register shortcuts, -6 (67 -> 61). -- DONE (lexicon 268.1 -> 263.0).** Knob PHARO_MCF_NO_ENTRY_SHORTCUTS.
- Files: emitMcfPushRecord, emitMcfMethodEntry, emitMcfCallStub, emitMcfBlockStub, emitMcfReturn_arm64.
- Changes: use x27 directly (no `mov x4`); `stp x21` directly; `sub x22,x25,#n*8` then `ldur x21,[x22,#-8]`; on return, compare and store x27 directly (no `mov x6`) and `ldp x21,x14` for the receiver and closure.
- Non-pinned paths keep the old forms.
- Test: the gate above plus VERIFY.

**Step 2: dead TOS mirror, -2 (-> 59). -- DONE (lexicon -0.7%; also the helper-send result reloads).** Knob PHARO_MCF_KEEP_TOS_MIRROR.
- Drop the entry and stub `ldur x26` always.
- Drop the return's `mov x26,x1` when the global PHARO_T1_TOS_REG is off. Key on the global knob, not on g_useTos, because the caller's compile decides whether x26 is trusted.
- Test: a PHARO_T1_TOS_REG + TOS_VERIFY + TOS_POISON run on build-dbg must stay clean, because the mov is kept there. Sabotage: poison at endOfSend must crash.

**Step 3: one adr for the callee JM, -1 (-> 58). -- DONE (steps 3+4: lexicon 260.3 -> 257.1).** Knob PHARO_MCF_NO_ADR_JM.
- Raw-encode `adr x19, -(curOffset + sizeof(JITMethod))` in emitMcfMethodEntry.
- Test: the g_fsrX19Verify exit oracle. Sabotage: a delta off by 8 must hit brk 0xF14.

**Step 4: fused depth check, -2 (-> 56). -- DONE (steps 3+4: lexicon 260.3 -> 257.1).** Knob PHARO_MCF_NO_FUSED_DEPTH.
- `sub x16,x11,#1; tbnz x16,#15,slow`, with the depth-0 body out of line (`cbnz x11,fail`).
- Set mcfDepthLimit (MachineCodeFrames.cpp:2088) to 32768 when the knob is off, and keep it at 55936 when the knob is on. Add a static_assert that MaxFrameDepth <= 65536.
- Tests: recursion probes at 30000, 40000 and 60000 deep compared with NO_JIT (same overflow signal), and a depth-0-after-yield probe that shows native calls still happen at depth 0.

**Step 5: SavedFrame layout, -5 (-> 51). -- DONE (lexicon 257.1 -> 254.2).** Separate struct commit; knob PHARO_MCF_NO_PAIRED_PUSH for the emitter part.
- 5a: factor homeMethodFor() out of mcfSyncRegisters (MachineCodeFrames.cpp:978-990) and use it at popFrame 17389 and 17552. Sabotage: if homeMethodFor returns nil, nothing may change. Then delete savedHomeMethod.
- 5b: pack an info word: savedArgCount u8, siteNArgs u8, ctxSynced u8, pad u8, homeFramePlus1 u32. homeFrameDepth()/setHomeFrameDepth() accessors at about 30 sites; the compiler flags the ones not yet converted.
- 5c: choose the FINAL pair order now, so later steps drop whole pairs: (mcReturn,savedFP) (mcJM,materializedContext) (savedIP,savedMethod) (savedReceiver,info) (savedClosure,savedActiveContext) (bcEnd,retSlot), then pushSite. Push = 6 stp. Return = ldp mcReturn,savedFP; ldp x19,matCtx; ldr x21; ldp clo,act.
- Update mcfInit (1665-1683), the !fold home test (5216: 32-bit load + cbz) and the NO_LAZY_VIEW argc load (5313 becomes ldrb).
- Add static_asserts for every pair and for 128 bytes.
- Tests: the extended push oracle (info bytes 2-7 == 0); MAT_FULL_RESYNC A/B; MCF_NLR_VERIFY plus the exception, ensure and NLR classes; build-x86 under Rosetta; the ASan tree. Sabotage: a w12 with bit 16 set must be reported.

**Step 6: lazy receiver/tempBase, -4 (-> 47), +1 store per executed sync. -- DONE (lexicon 254.3 -> 250.4; poison oracle proved by dropping one sync: 35 probes differ).** Knob PHARO_MCF_NO_LAZY_RT.
- Before the flip: convert 4383, 4407, 7337, 14562 and 14628 to x21, plus a grep gate that counts `ptr(x0, OFF_RECEIVER)` reads in MCF-reachable arm64 emitters and must find 0.
- emitSyncSpToState becomes `stp x25,x21,[x0]` + `str x22,[x0,#24]`. Drop the state stores in the entry, the stubs and the return (5289, 5301).
- Oracles: Step 0's (c) while the stores still run; then PHARO_MCF_POISON_STATE (plain DEBUG_BOOL: every native call and return writes poison into st.receiver and st.tempBase). Sabotage: drop one sync.
- First take a census of how many syncs execute on lexicon, to confirm the net gain.

**Step 7: INV-AC, -2 (-> 45). -- DONE (steps 7+8 together: lexicon 251.8 -> 246.1).** Knob PHARO_MCF_NO_LAZY_ACTCTX.
- Native code stops writing and restoring savedActiveContext. Rename the field so C++ goes through recordActiveCtx(f) = (mcf && knob on) ? activeContext_ : field.
  - Readers to convert: popFrame 17392 and 17555; materializeFrameStack 21768-21802; Primitives.cpp 4001 and 13676; captureCollapsedFrames.
  - The GC visitor skips the field under the knob.
- tryRestoreCollapsedFrames assigns activeContext_ = the captured pre-collapse activeContext_ before it returns true.
- stopVM asserts in the J2J/T2 nil writers (Interpreter.hpp:2233, 2386) under MCF.
- Tests: Step 0's (a) stays at 0 with the knob off; sabotage via tryRestore; the DET_SCHED repro set (AIPrimTest, the weak-ref classes); the full sweep.

**Step 8: derived closure, -5 (-> 40). -- DONE (steps 7+8 together: lexicon 251.8 -> 246.1).** Knob PHARO_MCF_NO_LAZY_CLOSURE.
- st.closure is derived in C++: jitMethod->isBlock ? tempBase[-1] : nil, inside mcfDeriveView. Every C++ reader of st->closure goes through it: mcfSyncRegisters, mcfRun 2161, mcfVerify*, about 40 JITRuntime.cpp references.
- Record closure: recordClosure(f) = (savedMethod is a CompiledBlock) ? savedFP[0] : nil (INV-FP0). Derive from savedMethod, never from mcJM. The GC skips the field, because the stack scan covers fp[0].
- The push drops the closure load and store, the entry drops its closure store, and the return drops the closure load and store.
- The block-create stub (6611) derives the closure with ldrb isBlock + ldur + csel: +3 per closure created.
- Refuse the combination with PHARO_T1_INLINE_BLOCK_VALUE and NO_PINNED_REGS at mcfInit.
- Tests: Step 0's (b) INV-FP0 oracle; a poison fill of the record's closure field in debug builds; ContextTest, BlockClosureTest, exception and debugger classes with setup_fake_gui; the full sweep.

**Step 9 (optional, -2 -> 38): checkpoint countdown in w23. -- DEFERRED (25 C++ writers of checkCountdown_; a stale register copy is a scheduling hazard).** Knob PHARO_MCF_NO_CTDN_REG. Under MCF, x23 is clobbered by JIT_CALL and unused.
- Machine code holds the countdown in w23. w23 is stored back at every sync point and reloaded after a helper and after mcfRun's `checkCountdown_ -= 1`.
- The back-edge countdown (14176) uses w23 too.
- Test: DET_SCHED determinism, meaning an identical AIPrimTest failure point and identical COUNT_CALLS counts with the knob on and off.

**Deferred:** Design 1's B3 (record-pointer register; the frameDepth_ exit sync costs about 3 per exit), Design 2 part B (in-place slots), Design 3's mcJM derivations and its JITState offset swap.

**Final shape after steps 1-8:** push about 23 instructions, return about 17.

**New invariants**
- I1: sizeof(SavedFrame)==128, and every stp pair is adjacent and 8-aligned.
- I2: a native-written info word is exactly the zero-extended w12.
- I3: homeMethod_ is derived, never stored.
- I4: native calls happen only at depth <= 32768.
- I5: x26 is not live at codeStart.
- INV-R: x21 and x22 are the truth; state copies are valid only after a sync.
- INV-AC.
- INV-FP0.
- No C++ derivation goes through a record's mcJM.

**Open questions**
1. How many emitSyncSpToState executions per lexicon run? This decides step 6's net gain.
2. Can stencil-compiled or Tier2 code (stencils.cpp has 106 state-field references) ever run inside an MCF session, where a lazy st.receiver or st.closure would be stale? The answer is assumed no; verify with the poison run.
3. Is native recursion between 32768 and 55936 frames needed for any benchmark?
4. Does any C++ reader of savedClosure run on records that break INV-FP0 (old non-full BlockClosure, materialize-bail frames)? Run Step 0 oracle (b) before step 8.
5. Is the window inside tryRestoreCollapsedFrames, before its activeContext_ assignment, observed by anything besides the following popFrame?