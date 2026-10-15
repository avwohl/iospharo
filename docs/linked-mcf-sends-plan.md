# Linked MCF sends: implementation plan

Status: plan, 2026-10-15.  Made by a design panel (three independent designs:
PMS reuse, per-callee prologue, record diet) and a judge that checked each
claim against the code.  Read with docs/send-path-plan.md (last section) and
docs/patched-ic-design.md.  $S below means the session scratchpad with the
probe scripts.

## Verdict on the three designs

### Design 1, "pms-reuse" (MCF-linked sends with a per-callee linked entry)

Scores:
- correctness/safety: 8
- expected gain: 6
- incremental testability: 9

Claims that hold up in the code:
- `JITMethod::_pad_resv` (src/vm/jit/JITMethod.hpp:577) is unused anywhere in src/. `sizeof(JITMethod)` is locked at 128 (JITMethod.hpp:765-767), so the field can become `mcfEntryOffset` with no layout change.
- PMS is off under MCF exactly as the design says. AsmjitT1.cpp:8118-8126 has `siteNoJ2J = mcfEnabled()`, and `patchedShape` requires `!siteNoJ2J`.
- The existing PMS head (AsmjitT1.cpp:8334-8341) is `movz w6` / `movk w6` / `cmp` / `b.ne Lprobe` / `b Lprobe`. linkSendSite hard-codes W2 at keyMovzOffset+16 (JITRuntime.cpp:3496, 3542). It also refuses `rec.tailOffset == 0` (JITRuntime.cpp:3417). An MCF branch therefore has to be dispatched before both of those, as the design says.
- **Dropping the tier gate is necessary, not optional.** `JITCompiler::recompile` sets `newMethod->tier = 2` on every recompiled T1 method (JITCompiler.cpp:1497, "Mark as recompiled"). The existing `callee->tier != 1` gate (JITRuntime.cpp:3476) would refuse the hottest methods.
- **The purgeDeadCacheRoots hole is real.** IC entries are weak (Interpreter.cpp:25716-25720 zeroes IC slots whose method died), and the same function invalidates dead JMs (:25688). The comment at :25802 confirms that sweepGC has no recoverAfterGC tail, so a linked site can outlive both its slot 0 and its callee JM.
- **The eviction-starvation hole is real.** scrubEvictedCodeRanges calls unlinkEverything (JITRuntime.cpp:5070). Afterwards a warm IC hits forever and never reaches a rederive hook.
- The LE's refusal ordering matches emitMcfPushRecord (AsmjitT1.cpp:5395-5434). Nothing but the countdown is written before the last refusal check. The depth-0 pc store is idempotent.

Flaws found:
- **(a) The bit-60 requirement under-links.** The link predicate requires extras bit 60. Slot-0 entries filled while the callee was still uncompiled get `extra == 0` (Interpreter.cpp:30407 onwards). Under MCF those entries hit forever, through `cbz x7, dispatchCached` (AsmjitT1.cpp:8558), and are never upgraded. They would never link, so f is lower than it could be.
- **(b) Two call-cache fills, not one.** The design factors the predicate only out of mcfActivate (MachineCodeFrames.cpp:937-960). There is a second, slightly different fill in mcfNotePrimitiveTarget (MachineCodeFrames.cpp:1333-1377), which checks isStubOnEntry and applies no bisect/marker rules. "One predicate" has to cover both fills.
- **(c) The depth-0 block is duplicated per method.** The LE copies the out-of-line depth-0 block (about 13 words) into every method. That is per-method code growth that can be avoided.
- **(d)** A per-site `mov w12` is kept where the LE could derive the value.

### Design 2, "per-callee-prologue"

Scores:
- correctness/safety: 5
- expected gain: 8 (if repaired)
- incremental testability: 7

Verified defects:
- **The link predicate keeps `callee->tier == 1`.** Recompiled methods are tier 2 (JITCompiler.cpp:1497), so the hottest callees would never link.
- **Trigger T2 does not exist as described.** mcfFillICAtExit runs only on ExitSend with an empty IC slot (MachineCodeFrames.cpp:1856-1873), never on ExitSendCached. A countdown refusal therefore does not re-derive anything today.
- **U3 has no relink path.** U3 calls unlinkEverything on every mcfCodeChanged: recompile at JITCompiler.cpp:1510, eviction at :1790, flushCaches. Combined with the missing T2 trigger, sites would stay unlinked after the first recompile or eviction.
- **Step 5 is poor value.** The refusal map (exit at mid-site, binary search of the patch map) saves 1 instruction for a new exit protocol.
- **Step 8 is risky.** Making receiver/tempBase/closure authoritative in registers needs a publish at every GC-capable helper.

Good, verified ideas:
- Run the generic path through the prologue too, so polymorphic calls also gain.
- In the prologue, take the caller's arg count from `ldrb [x19, #argCount]` instead of a per-site `mov w12`. JITMethod::argCount (JITMethod.hpp:441) is cached from the same header that gives callerArgCount (AsmjitT1.cpp:906).
- A shared depth-0 stub instead of a per-method copy.
- The guard record at index -1.
- `x24` and `x27` are referenced nowhere in AsmjitT1.cpp, and JIT_CALL does not clobber them (JITState.hpp:405-447).

### Design 3, "record-diet"

Scores:
- correctness/safety: 7
- expected gain: 6 (applies to every native call, including polymorphic sends and block calls)
- incremental testability: 9

Verified:
- **The return count is right.** emitMcfReturn_arm64 is 35 instructions on the default path (counted from AsmjitT1.cpp:5011-5148).
- **Latent bug 1, real:** tryRestoreCollapsedFrames copies whole records back, memos included (Interpreter.cpp:10925-10926). Its only guard is g_objectMotionEpoch (:10888), not the code epoch. mcfClearMemos clears only `d < frameDepth_` (MachineCodeFrames.cpp:1424-1427). A recompile followed by an eviction between collapse and restore can therefore revive a memo into freed code.
- **Latent bug 2, real:** some push sites set savedIP but never clear mcReturn, so a stale memo from an older occupant of the slot survives:
  - Interpreter.cpp:20123 (jitSistaSelfRecCall)
  - Interpreter.hpp:2187 and :2339
  - the materializeJ2JSaveIntoFrame pushes at Interpreter.cpp:27199, 27915, 32250, 32559, 34448, 34570, 34715

  Today only the `mcKeyIP == savedIP` compare hides these stale memos.
- **The INV-M fold is sound in principle.** The only homeFrameDepth writers that describe method frames are Interpreter.cpp:10058, 10251, 10469, 10657. The block writers are 16398 and MachineCodeFrames.cpp:1062, 1078. The non-push savedIP writers are 25453 (afterGC), 30773, Interpreter.hpp:2188, 2340.

Problems:
- **Not adjacent today.** `frameDepth_` and `checkCountdown_` sit on opposite sides of mcfStateStorage_ (Interpreter.hpp:897-905). The ldp/stp pair needs a member move.
- **The countdown/depth fold (S6) is unsafe as stated.** C++ raises frameDepth_ without re-arming the countdown: tryRestoreCollapsedFrames sets `frameDepth_ = n`, and executeFromContext rebuilds frames. The bound would silently stop holding. The gain is 3 instructions.
- **Register discipline at entry.** TrampolineAsm.S already uses x24/x27/x28 for its own state (lines 147-162). Under AAPCS the trampoline saves and restores them, so pinning is still possible, but every entry into T1 code must load them.

## Synthesized plan

Base: Design 1's linking machinery and LE, with three changes:
- Design 3's record/return diet goes first. The diet applies to every call, including the block stub, and adds no per-site code.
- Design 2's prologue shortcuts are grafted onto the LE: arg count from x19, the shared depth-0 stub, the generic path via the LE, and the guard record.
- The four verified holes are fixed in the same batch as linking.

Everything is arm64 only, because the MCF emitters exist only in AsmjitT1.cpp. The SavedFrame changes are shared C++, so every step that changes the record also needs a build-x86 smoke test.

Baseline numbers per call: today caller 34, stub 49 (push 33), return 35.

Test kit used by every step:
- **(P) probes:** run $S/*.st on the default build and with PHARO_NO_JIT, and compare md5 sums. Run each probe also under PHARO_DET_SCHED=1.
- **(L) lexicon instruction count:** `/usr/bin/time -l` instructions retired, release tree, interleaved A/B on the step's knob, 5 runs per arm. Record the [JIT] Stats line each time (compile refusals, code bytes, evictions).
- **(S) SUnit sweep:** the full 565-class sweep with fake-GUI prep. Arm A is default, arm B is the step's opt-out, on the same binary. Diff with classify-sunit.py, both lists sorted with LC_ALL=C. Re-run any differing class 3 times.
- **(V) oracles:** run on build-dbg (`-DPHARO_DIAG_KNOBS=ON`) with PHARO_MCF_VERIFY=1, plus the step's own oracle. Prove each oracle once by sabotage.

### Step 0: census and baseline (no codegen change)

Files: pharo_mcf_call_census (MachineCodeFrames.cpp:150) and its call in emitMcfCallStub (AsmjitT1.cpp:5567-5580). Pass x5, which the stub preserves, and x19 as well. Classify each native call as linkable or not, with a reason histogram:
- slot 0 matched, slots 1-5 empty
- extras is 0, or is bit 60 plus optional bit 56 and the address
- the callee passes the future predicate

Knob: existing PHARO_MCF_CALL_CENSUS.

Done when f is printed for lexicon, fib and polymath. Also take an lldb single-step histogram of one call+return. Kill or rescope the linking steps (5-6) if lexicon f < 0.3; steps 1-4 do not depend on f.

**Step 0 result (2026-10-15, PHARO_MCF_CALL_CENSUS on build-dbg3):**

    lexicon  970M native calls: slot 0 of a monomorphic site 69.6%
             (64.6% extras j2j, 5.0% extras 0); slot 0 of a site that is
             polymorphic now 12.8%; other slots 15%; entry not in the
             caller's IC (mega cache) 2.6%.  Callee shape ok for all.
    fib      2.70G native calls: slot 0 of a monomorphic site 99.9%.

So f is 0.70 (first link needs a monomorphic site) to 0.82 (a link made
before the site went polymorphic keeps slot 0's class).  Steps 5-6 go ahead.

### Step 1: correctness fixes and refactors (C++ only)

Changes:
- Add `SavedFrame& claimFrameSlot()` (clears mcReturn and pushSite) and use it at every push site listed in Design 3.
- Fix the collapsed restore: clear `collapsed_.recs[k].mcReturn` in mcfClearMemos (MachineCodeFrames.cpp:1424), or store g_mcCodeEpoch in CollapsedFrames and check it at Interpreter.cpp:10888.
- Factor `mcfNativeCallable(method, jm, keyIsImmediate)` out of mcfActivate (:937-960) and mcfNotePrimitiveTarget (:1333-1377). Both fills call the new function.

Knob: none (bug fixes and a pure refactor).

Expected: L within noise. Tests: S on the default arm, plus P.

### Step 2: return fold and guard record (Design 3 S2-S4)

Changes:
- C++, unconditional: the method-frame home writers at Interpreter.cpp:10058/10251/10469/10657 also clear mcReturn. The materializedRetSlot writers (30903, 34598) also clear mcReturn.
- Add `SavedFrame mcfGuardFrame_` immediately before savedFrames_ (Interpreter.hpp:889). mcfInit checks adjacency at runtime and sets L.guardOk; otherwise T1 emits the old `cbz`.
- emitMcfReturn_arm64 drops the depth `cbz`, the homeFrameDepth/closure test, the retSlot test and the keyIP compare. One `cbz mcReturn` remains.
- Extend pharo_mcf_verify_ret into an oracle that re-checks every removed condition.

Knobs: DEBUG_BOOL_RT(PHARO_MCF_NO_RET_FOLD) and DEBUG_BOOL_RT(PHARO_MCF_NO_GUARD_FRAME).

Expected: about -9 per return, about -8.9G instructions on lexicon.

Tests:
- V with sabotage: drop the clear at 10058 and the oracle must fire on an ensure:/NLR probe.
- S, with the NLR/ensure/Context/Process classes run twice.
- L.

### Step 3: record layout and packed stores (Design 3 S1+S5, plus Design 2's immediate limit)

Changes to SavedFrame:
- Make it 128 bytes. Move savedIPOffset, savedBytecodeEndOffset and gcVerifyByte to a GC-time side array (prepareForGC :25075-25098, afterGC :25450-25460).
- Delete mcEpoch (writer :16815, census MachineCodeFrames.cpp:1829) and mcKeyIP. Readers change at MachineCodeFrames.cpp:518, 540, 1494, 1828, 1934; 1494 and 1934 now compare savedIP.
- Pack frameBits = {u8 savedArgCount, u8 spare, bool ctxSynced, u8, u32 homeP1}, behind homeFrameDepth()/setHomeFrameDepth() accessors at the sites Design 3 lists.

Changes to the push (shared by the method and block stubs):
- index with `add x8, base, x11, lsl #7`
- 7 `stp` stores
- depth limit as `cmp x11, #(13<<12)` (53248 instead of 55936; speed-only difference)

Changes to the return: matching `ldp` loads.

Knobs: DEBUG_BOOL_RT(PHARO_MCF_NO_PACKED_RECORD), which keeps field-by-field stores into the new layout, and DEBUG_BOOL_RT(PHARO_MCF_NO_POW2_STRIDE).

Expected: about -7 per push and -1 per return, about -7.9G on lexicon.

Tests:
- mcfVerifyPush updated and silent.
- PHARO_GC_FRAME_VERIFY=1 sweep.
- build-x86 smoke.
- S and L.

### Step 4: per-callee linked entry, used by the generic path

LE emission:
- At the end of emitMethodBytes, for CompiledMethods only (not CompiledBlocks), under mcfLazyView, emit the LE inside the method's code range.
- Store the offset in `_pad_resv`, renamed `mcfEntryOffset`.
- The LE contains: the countdown; depth 0 through a shared stub (Design 2's `adr x17, back; ldr x16, [x0, #OFF_MCF_DEPTH0_STUB]; br x16`); the limit; the Step 3 record push with savedArgCount from `ldrb w17, [x19, #argCount]`; `adr x19` (own JM, PC-relative); tempBase/receiver/closure stores; unrolled nil stores; TOS; then `b codeStart`.
- Refusal: `br x15`.

Generic path (AsmjitT1.cpp:11969-11976), after a call-cache hit with prim 0:
- `ldr w16, [x9, #mcfEntryOffset]; cbz w16, Lstub; add x16, x9, x16; br x16`, with the stored offset already biased by sizeof(JITMethod).
- The block stub, perform:, tiny-shape and ExtSend paths keep the shared stubs.

Knobs: DEBUG_BOOL_RT(PHARO_MCF_NO_LE), which emits no LE so the generic path keeps the stub.

Expected: about -6 per native call (stub specialization minus +2 dispatch), about -6G on lexicon.

Gate: zone bytes and the eviction count on lexicon must not rise materially (PHARO_CODE_ZONE_MB stays at default).

Tests: V (the push oracle inside the LE), P, S, L.

### Step 5: MCF patched head, shape only

Head emission (AsmjitT1.cpp:8118-8126 gate, 8334 head), when mcfNative && !PHARO_MCF_NO_LINK:
- W0, W1, cmp, b.ne Lprobe
- x13 (2 instructions), `adr x14, mcfCont`, `adr x15, Lprobe`
- W2 = `b Lprobe`

Patch-map bookkeeping:
- tailBranchOffset = W2
- tailOffset = W2, so the existing zero test stays meaningful
- flags bit 1 = MCF shape, bits 8-11 = nArgs
- Extend the BL/BLR scan (:15678-15710) to the MCF W2.

linkSendSite refuses MCF records in this step.

Knob: DEBUG_BOOL_RT(PHARO_MCF_NO_LINK), which emits no head. With the knob set, emission must be byte-identical to Step 4 (check with PHARO_T1_DUMP_SEL dumps).

Expected: +4 per native call that goes through the head, about +3.9G temporarily.

Gate: the eviction count is unchanged. Tests: S, L.

### Step 6: linking, with the invalidation fixes in the same commit

linkSendSiteMcf, entered from linkSendSite on flags bit 1, before the tailOffset test. The gates:
- slot 0 is set; first link only when slots 1-5 are empty
- extras is 0, or is bit 60 with optional bit 56 (graft (a) above)
- the key fits 32 bits and is not kImpossibleKeyHi16
- the callee: `callee = methodMap_.lookup(slot1)`, Compiled, mcfEntryOffset != 0, !isSpliceTarget, !isStubOnEntry, **no tier gate**
- arity from flags bits 8-11
- the selector cross-check
- mcfNativeCallable (from Step 1)
- refuse the kMcfTinyShape and kMcfReturnsValue shapes
- reach for W2

Patch order: W1 = impossible if relinking, then W2, then W0, then W1 = key (COMMIT), all in ScopedPatchWriteAccess, with flags written inside the window.

Also in this step:
- verifyAllPatchedSites gets an MCF branch: the W2 target must equal codeStart + mcfEntryOffset.
- **Holes closed:**
  - purgeDeadCacheRoots calls unlinkEverything when jmPurged or icPurged is non-zero.
  - scrubEvictedCodeRanges replaces unlinkEverything with a range-targeted unlink: decode each linked W2 and test inEvicted.
  - rewriteIcEntriesAfterRecompile (JITRuntime.cpp:3740-3762) re-derives MCF-shape sites whose slot 0 names the method even without bit 60. Otherwise extras-0 links would stay on the old JM.
  - New relink sampling hook: in mcfRun's ExitSendCached case (MachineCodeFrames.cpp:1878), if st.icDataPtr is slot 0 of an unlinked MCF record, call rederiveSiteForICData. This fires on countdown refusals only, about once per 1024 calls.

Knobs:
- PHARO_MCF_NO_LINK (RT)
- existing PHARO_T1_PATCHED_SENDS_NOLINK
- DEBUG_BOOL(PHARO_MCF_LINK_VERIFY): per linked call, lookup(class, selector) == callee
- g_mcfCountLinkedCalls under PHARO_MCF_COUNT_CALLS

Expected: linked calls save about 20 at the caller (34+2 dispatch down to 14-15), so about f × 986M × 20, about 16G at f = 0.8.

Tests:
- V with PHARO_T1_PATCH_VERIFY=1 and LINK_VERIFY=1. Sabotage once by skipping the eviction unlink.
- Eviction stress with PHARO_CODE_ZONE_MB=16 on lexicon and on SUnit batches 1-100 and 501-550.
- Method-redefine, become, and deep-recursion probes.
- linked/native ≥ 0.9 f.
- S (arm B: NO_LINK), L.

### Step 7 (optional): pinned frames base and nil register

Use x24 (savedFrames_.data()) and x27 (nil). Do not use x28: the doc comment at AsmjitT1.cpp:6330 and the trampolines use x28.
- Load both in JIT_CALL and JIT_RESUME_CALL, and in pharo_jit_osr_resume (TrampolineAsm.S:611); add both to the clobber lists.
- The verify stub checks both registers.

Knobs: DEBUG_BOOL_RT(PHARO_MCF_NO_FRAMES_REG) and DEBUG_BOOL_RT(PHARO_MCF_NO_NIL_REG).

Expected: -2 per push and -2 per return, about -3.9G.

Tests: OSR, resume and process-switch probes; S.

### Not adopted, or deferred

- **Countdown/depth fold:** C++ changes frameDepth_ without re-arming the countdown.
- **Refusal map without x15:** saves 1 instruction for a new exit protocol.
- **Register-authoritative state mirrors:** high risk.
- **Lazy savedIP:** 49 readers.
- **bl/ret x28:** requires moving the C++ link register out of x30.
- **Frameless primitive-first entry:** listed as an open question below.

### Expected end state and validation

Per call after Steps 0-6:
- linked monomorphic send+return: about 14 + 34 + 24 = about 72 (about 68 with Step 7)
- unlinked or polymorphic: about 98

Lexicon: about 17.7G + f × 19.7G saved out of about 300G (986M × 116 / 0.38), so about 12% at f = 0.8. This counts instructions, not time: lexicon's IPC is 4.3 (send-path-plan.md), so measure wall time separately.

Fib is the best case: f ≈ 1 and self-recursion links to the method's own LE.

Polymath: mostly the f-independent Steps 2-4.

Final gate:
- The full sweep compared with the M6 baseline (27876 P / 11 F / 3 E).
- Polymath full.
- An ASan tree run of the probes before blaming any backend.

## Open questions the code does not settle

1. **Lexicon baseline:** Design 2 quotes 614G instructions, but 986M × 116 / 0.38 ≈ 300G. Which tree and test was the 614G?
2. **Extras-0 slots:** how many slot-0 entries under MCF hold `extra == 0` with a compiled callee? Step 0 must split f by extras class.
3. **pendingICOwnerMethod_ in the new hook:** is `method_` (the caller) set correctly at an ExitSendCached exit? The rederive in the new sampling hook takes the owner from `method_`.
4. **Code inside old JMs:** C++ may assume that no new activation enters a superseded (old) JM's code after a recompile, for example in mcfDeriveView or the exit/resume lookup through methodMap. Re-deriving at recompile avoids most cases. An extras-0 site linked before the recompile must not keep entering old code if the re-derive misses it.
5. **ldp/stp reach:** can frameDepth_ and checkCountdown_ be moved next to each other within ldp/stp imm7 reach of x0 without disturbing the dir* offsets that mcfInit validates (MachineCodeFrames.cpp:1450 onwards)? Only relevant if Design 3 S7 is revived.
6. **Class-index reuse:** class indices are monotonic today (nextClassIndex_++, ObjectMemory.cpp:610/636). If index reuse ever lands, a full GC must keep unlinkEverything (PMS §13 Q4). Keep it in v1.
7. **Frameless primitive entry:** can emitPrimProlog_arm64 (AsmjitT1.cpp:4248) run before any record push and without the callee tempBase? If so, a frameless primitive entry would help the polymath arithmetic callees, which today pay the full stub. Not verified.
8. **Code-size cost:** what is the real per-site cost (Step 5 head: +10 words) and per-method cost (LE: about 35-40 words) against the 192 MB zone? Only the Step 4 and Step 5 measurements can say whether eviction rises.