# MCF native call record: trim plan

**Done 2026-10-16.** All four stages are in (`ea1bc3a2`, `db63fa46`,
`ddaf2fb3` and the stage 4 commit).  Lexicon 226.3 -> 220.5 billion
instructions.  Stage 4 differs from the plan in one point: a site whose ip
offset needs more than 16 bits still makes a native call; it passes the old
pair and marks the record recFull 2 (raw ip and argument count, no tail
fields).

**Stage 1 is off by default (opt-in `PHARO_MCF_PAIRED_DEPTH`).**  It
removes 4 instructions per call pair, but `32 benchFib` takes 53 ms with it
and 38 ms without, and lexicon wall time is about 4% worse.  The depth and
the context are also read and written one word at a time elsewhere, and a
load that is wider or narrower than the store it reads cannot take the
store's value directly on this core: it waits for the store to reach the
cache.  Splitting only the loads or only the stores did not help; splitting
both restored 38 ms.  Measure wall time as well as instructions for any
change that pairs memory accesses on a hot store-to-load path.

Status (2026-10-15): this plan comes from a judge pass over two designs (A, minimal risk; B, maximum gain) and three census reports. The judge read the cited code at HEAD `ee7f1201`. Nothing was edited, built or run. All AsmjitT1.cpp line numbers below are for `ee7f1201`.

The plan saves 9 instructions per linked monomorphic call pair: 49 now, 40 after all four stages. Lexicon makes about 970M native calls (docs/linked-mcf-sends-plan.md:107), so the saving is about 8.7e9 of about 246e9 instructions retired (about 3.5%). Stages 1-4 keep every rule in docs/mcf-call-trim-plan.md. No C++ code derives anything through a record's mcJM.

## Baseline, counted from the emitters

- **Caller, linked site: 10 instructions** (AsmjitT1.cpp:8935-8949).
  - W0, W1, cmp, b.ne.
  - `ldr x13,[x19,#bcStartCache]` and `add x13` (8940-8941). These two produce savedIP and nothing else.
  - adr x14, adr x15, `mov w12`, W2 `b`.
- **Push and method entry: 23 instructions** (emitMcfPushRecord 5702-5825, emitMcfMethodEntry 6052-6101).
  - Countdown: 4.
  - Depth: ldur, sub, tbnz, add x8, add, stur. That is 6.
  - `ldr x16,[x19]`, then `stp x13,x16`, `stp xzr,xzr` and `stp x21,x12` (5790-5805). That is 4.
  - Context: `ldr x17`, `str x27`, `stp x19,x17` (5819-5821). That is 3.
  - `sub x16,x22,#8` and `stp x14,x16` (5822-5824). That is 2.
  - adr x19, sub x22, ldur x21, b codeStart. That is 4.
- **Return: 16 instructions** (emitMcfReturn_arm64 5264-5444).
  - ldur depth, ldr x5 (the context), sub, add x7.
  - ldp x9,x11 (5324), cbz.
  - cmp, b.eq (the widow test).
  - stur depth (5372).
  - ldp x19,x14 and str of the context (5377-5378).
  - stur x1, mov x25, add x22 (5390), ldr x21 (5403), br.

## What the judge checked

### Confirmed

1. **The baseline counts above.** Under default knobs the return never reads savedIP, savedMethod or savedArgCount (5322-5326, 5394-5431).
2. **w12 is only data for the record and the stubs.**
   - Readers: the record store (5805), the call-stub nArgs `lsr w11,w12,#8` (5832) and the block stub's `lsr w17,w12,#8` (5983).
   - linkSendSiteMcf takes nArgs from `rec.flags` (JITRuntime.cpp:3639), not from code. The decode at JITRuntime.cpp:3544 reads the PMS x14 pack word, not w12.
   - Patch offsets come from labels (AsmjitT1.cpp:15207-15211, 16498-16502). Deleting the ldr/add pair moves no patch word.
3. **The depth-0 body reads x13** (5733-5741) to write the pc of activeContext_. The formula is pc = savedIP - bytes + 1, where bytes = oop + 8 (Interpreter.cpp:17236-17240). With `ipOff = savedIP - oop`, the pc is `ipOff - 7`.
4. **jm->argCount equals header numArgs on every verified push, blocks included.**
   - mcfDeriveView sets `st.argCount = jm->argCount` (MachineCodeFrames.cpp:1064).
   - mcfVerifyPush then compares that value with the header (MachineCodeFrames.cpp:750-753).
   - So Design A's "not verified" item is already covered by an existing check.
5. **The perform target's argument count is gated** (header numArgs == nArgs-1, MachineCodeFrames.cpp:1229).
6. **No reverse map exists from code offset to bytecode** (JITMethod.hpp:692-723). Deriving savedIP from mcReturn is rejected, because popFrame (Interpreter.cpp:17679), afterGC (25893) and mcfClearMemos all clear the memo.
7. **Eviction frees code before mcfCodeChanged runs.**
   - freeMethod runs before onEvict (CodeZone.hpp:323-326, 337-341).
   - mcfCodeChanged follows evictLRU (JITCompiler.cpp:1786-1790).
   - A record's mcJM can therefore dangle. Any derivation through mcJM is a use-after-free in code-zone memory.
8. **A stale retSlot is a real hazard. Dropping the zero store needs a flag.**
   - popFrame only reads retSlot (17696) and never clears it.
   - returnValue writes through it (8985-8987).
   - nlrSegmentFreeze refuses any record whose retSlot is non-null (10993). A stale raw value would make every freeze refuse.
   - prepareForGC fills a null bcEnd into live records (25509).
9. **The J2J writers store `bytes + byteSize()`** (Interpreter.cpp:35088, Interpreter.hpp:2499). This differs from bytecodeEndOf, which uses `compiledCodeByteSize` (Interpreter.hpp:2128-2131). Records that C++ writes must keep the raw value.
10. **Member layout.** frameDepth_ is the member just before mcfStateStorage_. currentFrameMaterializedCtx_ comes after activeContext_ (Interpreter.hpp:944-952). mcfInit computes the direct offsets (MachineCodeFrames.cpp:1817-1830).
11. **x10 is free at all three push heads.** No stub or entry reads x10 before emitMcfCalleeView writes it (5845). The depth-0 body clobbers only x16 and x17.
12. **Only AsmjitT1.cpp, MachineCodeFrames.cpp and JITMethod.hpp use McfFrameLayout.** No other file takes `offsetof` on SavedFrame.

### Rejected or corrected

- **Design B's own-slot context (B2): rejected.** Pairing frameDepth_ with currentFrameMaterializedCtx_ (Stage 1) gives the same count.
  - Push: 19 instructions under either scheme without B4.
  - Return: 13 instructions under either scheme.
  - Own-slot also needs about 76 renames, about 50 frameDepth_ writers classified, a `MaxFrameDepth+1` array, and memmove fixes in nlrSegmentFreeze.
- **Design B's B4 (savedMethod and argCount from mcJM): deferred.**
  - The gain is -2. It breaks the plan rule "No C++ derivation goes through a record's mcJM" (docs/mcf-call-trim-plan.md, invariants).
  - It needs six fill sites (F1-F6), including one inside forEachRoot, which become (ObjectMemory.cpp:2109) and seven Primitives.cpp callers also run.
  - A missed fill gives a wrong method silently, and ASan cannot see a stale mcJM read in code-zone memory.
- **Design B's ipOff relative to bcStartCache: replaced by ipOff relative to the method oop.**
  - The native cost is the same, because both are immediates.
  - The C++ derivation then needs no literal count.
  - The convention matches prepareForGC (25507) and materialize (22363-22367), minus the 8-byte header.
- **Design A Stage 4 ("needs Stage 3"): corrected.** The savedTB change is independent of the ipOff change. Choose the final layout in that stage (Stage 3 below) and put savedIPRaw at 0x08, paired with mcReturn. The ipOff stage then only turns `stp x14,x13` into `str x14`.
- **Design A Stage 2 reader list: incomplete.** Add nlrSegmentFreeze 10993 (a spurious refuse(5)) and the materializedFrameCount_ decrement in popFrame (17696). The rename catches both.
- **Not found: an explicit `jm->argCount == nArgs` gate in mcfExtSendTarget (MachineCodeFrames.cpp:1414) or mcfPrimFailBodyEntry (1651).** Selector lookup should guarantee the count, but Stage 4 makes the stub trust the callee JM's argCount. Add the gate (see Stage 4).

## Shared gate for every stage

Every stage must pass all of these:

- The probes compared by md5 with PHARO_NO_JIT, with and without `PHARO_DET_SCHED=1`.
- `PHARO_MCF_VERIFY` on build-dbg (`-DPHARO_DIAG_KNOBS=ON`), with no reports. Prove each new oracle first with its sabotage knob.
- The full SUnit sweep through `scripts/classify-sunit.py` against the current baseline. Sort both lists with `LC_ALL=C`.
- Lexicon instructions retired (`time -l`), with the arms interleaved. `PHARO_MCF_COUNT_CALLS` counts must be unchanged, so that the saving is not a rise in refusals. Check the `[JIT] Stats` line for refused compiles.
- An lldb single-step of one linked call pair, to confirm the count.
- build-x86 under Rosetta as a smoke test, because the struct and the accessors are shared code.

## Stage 1: pair frameDepth_ with currentFrameMaterializedCtx_ (-4: push -2, return -2)

**Risk: very low.** The change is member order plus codegen. C++ semantics do not change.

**C++ changes**
- Interpreter.hpp:944-952: declare `Oop currentFrameMaterializedCtx_;` directly after `size_t frameDepth_;`, before `mcfStateStorage_`.
- Interpreter.cpp:~1000-1002: reorder the constructor initializer list to match, or `-Wreorder` fires.
- mcfInit (MachineCodeFrames.cpp:1826-1835):
  - Add `L.pairDepthCtx = dirDepth != kMcfNoDirect && dirMatCtx == dirDepth + 8 && dirDepth % 8 == 0 && dirDepth >= -512 && dirDepth <= 496`.
  - Add the field to McfFrameLayout.
  - Print the field on the `[MCF-LAYOUT]` line.

**Native changes.** Use the paired form only when `L.pairDepthCtx` is true and the knob is off.
- emitMcfPushRecord:
  - Replace the depth `mcfFieldLdr` (5715) with `ldp x11, x10, [x0,#dirDepth]`.
  - Replace the depth store (5780) with `stp x11, x27, [x0,#dirDepth]`.
  - Delete the `ldr x17` and `str` at 5819-5820, and store `stp x19, x10` for (mcJM, materializedContext) at 5821.
  - Use x10, not x17, because the depth-0 body clobbers x16 and x17.
- emitMcfReturn_arm64:
  - Replace the two loads at 5274 and 5276 with `ldp x3, x5, [x0,#dirDepth]`.
  - Delete the depth store at 5372.
  - After `ldp x19,x14` (5377), replace the context store at 5378 with `stp x3, x14, [x0,#dirDepth]`.
  - Apply this only on the lazyCA path. The non-lazy branch (5380-5382) keeps its form.

**Knobs**
- Opt-out: `DEBUG_BOOL_RT(PHARO_MCF_NO_PAIRED_DEPTH)`.
- Sabotage: `DEBUG_BOOL(PHARO_MCF_PAIR_SABOTAGE)`. The push stores `stp x11, x10` (the caller's context instead of nil).
- Oracle: the existing `PHARO_MATCTX_OWNER_CHECK` (debug_vars.h:518, checkMatCtxOwner at Interpreter.cpp:3102). The sabotage must make the owner check fire.

**Tests**
- The shared gate.
- `PHARO_MCF_CENSUS=1` must show `pairDepthCtx=1` on both build-rel and build-dbg.
- Run the sabotage under `PHARO_MATCTX_OWNER_CHECK`, plus the exception, ensure and NLR classes, and ContextTest.

## Stage 2: a recFull flag replaces the zero store at +0x50 (-1, push)

**Risk: low.** The compiler finds every reader. A missed reader crashes at once under the poison.

**C++ changes**
- Interpreter.hpp:690: rename `infoPad` to `uint8_t recFull`. A native push writes 0 there for free, because w12 is below 65536.
- Rename `savedBytecodeEnd` to `savedBytecodeEndRaw` and `materializedRetSlot` to `materializedRetSlotRaw`, so the build lists every use.
- Add two accessors next to recClosure (Interpreter.hpp:~2072):
  - `recBcEnd(f)` = `f.recFull && raw ? raw : bytecodeEndOf(f.savedMethod)`
  - `recRetSlot(f)` = `f.recFull ? raw : nullptr`
- Add one helper, `initFullRecord(SavedFrame&)`, which sets `recFull = 1`. Call it from every C++ writer:
  - pushFrame (Interpreter.cpp:17170-17206)
  - 20504-20524
  - materializeJ2JSaveIntoFrame (31283-31413)
  - 35084-35116
  - Interpreter.hpp:2343-2360 and 2496-2509
- Convert these readers:
  - returnValue 8985-8987
  - nlrSegmentFreeze 10993
  - popFrame 17696 and 17741
  - popFrameForJIT 17904
  - Interpreter.hpp:2469
  - mcfVerifyPush (MachineCodeFrames.cpp:714, 739)
  - every other retSlot reader the rename turns up (census 3 lists 16 retSlot readers and 10 bcEnd readers)
- Change these writers into native records:
  - prepareForGC 25509-25510: fill or record the bcEnd offset only when recFull is set. Never write the raw field of a record whose recFull is clear.
  - afterGC 25890: rewrite the raw field only when recFull is set.
  - nlrSegmentPop 11112 and nlrSegmentDeopt 11200 and 11226: change the test `if (r.savedBytecodeEnd)` to `if (r.recFull)`.

**Native change.** In emitMcfPushRecord, drop `put2(xzr, L.savedBytecodeEnd, xzr, L.materializedRetSlot)` (5792) unless the knob is set.

**Knobs**
- Opt-out: `DEBUG_BOOL_RT(PHARO_MCF_KEEP_TAIL_ZERO)`, which puts the store back. The accessors are correct with or without it.
- Oracle: `DEBUG_BOOL(PHARO_MCF_TAIL_POISON)`.
  - The push writes `0xBAD0...` into both raw fields.
  - Under VERIFY, popFrame and prepareForGC stop the VM when a record with recFull clear no longer holds the poison.
- Sabotage: emit w12 with bit 24 set, so a native push claims recFull. The poison must then crash in returnValue.

**Tests**
- The shared gate.
- The NLR segment classes with `PHARO_NLR_SEG_VERIFY`.
- A materialize-bail probe, so that retSlot is used.
- A forced full GC deep in native recursion.

## Stage 3: the record keeps the caller's tempBase, and the final layout (-2: push -1, return -1)

**Risk: low to medium.** The change is a large mechanical rename. The null mapping is the one trap.

**Final layout (Interpreter.hpp:666-714)**

```
0x00 mcReturn            0x08 savedIPRaw
0x10 mcJM                0x18 materializedContext
0x20 savedTB             0x28 savedReceiver
0x30 savedMethod         0x38 info word
0x40 savedClosureRaw     0x48 savedActiveContextRaw
0x50 savedBytecodeEndRaw 0x58 materializedRetSlotRaw
0x60 pushSite            0x68 spare[3]
```

- mcReturn stays at 0x00, so the guard frame (MachineCodeFrames.cpp:1809) still holds.
- Replace the static_asserts at Interpreter.hpp:735-745 with asserts for these four native pairs: (mcReturn, savedIPRaw), (mcJM, materializedContext), (savedTB, savedReceiver), (savedMethod, info).
- mcfInit (1795-1813) takes the offsets automatically.

**C++ changes**
- Rename `savedFP` to `savedTB`. There are about 115 tokens, and the compiler finds every use.
- Add accessors `recFP(f)` and `setRecFP(f, fp)`.
  - When `jit::g_mcfTbRecord` is set, `recFP` returns `savedTB ? savedTB - 1 : nullptr`. Otherwise it returns the raw value.
  - Map null to null both ways. A null fp is tested at Primitives.cpp:4003, Interpreter.cpp:10993, 17210 and 22377, and Interpreter.hpp:2073.
- Writers to convert: 17199, 20517, 31382, 35106, and Interpreter.hpp:2354 and 2506.
- Also convert recClosure (Interpreter.hpp:2072-2076), the materialize loops, captureCollapsedFrames and tryRestoreCollapsedFrames (10932), and the NLR segment code.
- Set `g_mcfTbRecord` where `g_mcfLazyCloAct` is set (AsmjitT1.cpp ~16841).

**Native changes**
- Push:
  - Delete `sub x16,x22,#8` (5822).
  - Store `stp x14, x13, [x8,#0]`, `stp x19, x10, [x8,#0x10]`, `stp x22, x21, [x8,#0x20]` and `stp x16, x12, [x8,#0x30]`.
- Return:
  - `ldr x9, [x7]` replaces `ldp x9,x11` (5324).
  - After `mov x25, x22`, `ldp x22, x21, [x7,#0x20]` replaces the add at 5390 and the ldr at 5403.
  - x22 must stay intact until after `cbz x9, fall`.

**Knobs**
- Opt-out: `DEBUG_BOOL_RT(PHARO_MCF_NO_TB_RECORD)`. With the knob set, the record holds fp and the emitters keep sub and add.
- Sabotage: `DEBUG_BOOL(PHARO_MCF_TB_SABOTAGE)`, which stores x25 instead of x22.
- Oracle: the existing VERIFY checks "fp[0] is neither the receiver nor the closure" and INV-FP0 (MachineCodeFrames.cpp:716-735). Both now read through recFP, and the sabotage must make them fire.
- Also run `PHARO_GC_FRAME_VERIFY` and `PHARO_NLR_SEG_VERIFY`.

**Tests**
- The shared gate.
- The ASan tree.
- The collapsed-frame restore path (lexicon runs it about 1.7M times, MachineCodeFrames.cpp:2412-2415).

## Stage 4: the ip offset rides in w12 (-2, caller)

**Risk: medium.** The change touches about 98 savedIP references, the GC loops and the frozen NLR segment. Every reader is found by the compiler.

**Info word after Stages 2 and 4**
- Bytes 0-1: a union. When recFull is set, they hold `{savedArgCountRaw, siteNArgs}`. When recFull is clear, they hold `uint16_t ipOff16`.
- Byte 2: ctxSynced.
- Byte 3: recFull.
- Bytes 4-7: homeFramePlus1.
- Definition: `ipOff = savedIP - methodOop`, so the 1-based pc is `ipOff - 7`.

**Native changes**
- At the seven sites (7111-7115, 8796-8800, 8940-8944, 12577-12581, 12649-12653, 12696-12700, 12796-12800), delete `ldr x13` and `add x13`, and emit `mov w12, #ipOff`.
  - For six of the sites, ipOff = `bcOffsetFromMethObj + 1`.
  - For the extended-send site, ipOff = `g_bcStartDelta + globalIdx + sendLen`.
- A site with ipOff > 0xFFFF emits no native call.
  - Add the test to `mcfLinkShape` (8695), to `mcfNative` (8749), to the IC-path native call, and to the ext-send branch (7040-7120).
  - Count these sites in `[JIT] Stats`.
- The stubs take nArgs from the callee JM:
  - In emitMcfCalleeView (5832), `ldrb w11, [x9,#offsetof(JITMethod,argCount)]` replaces `lsr w11,w12,#8`.
  - In emitMcfBlockStub (5983), use the same change for w17.
  - The instruction count does not change.
- The depth-0 body (5733-5741) becomes `sub x17, x12, #7`, then `lsl #3`, then `add #1`. The method load goes away.
- The push stores `str x14, [x8]` instead of `stp x14, x13`. That saves no instruction; the gain is at the caller.
- Add gates `jm->argCount == nArgs` in mcfExtSendTarget (MachineCodeFrames.cpp:1414) and mcfPrimFailBodyEntry (1651). Without them the stub's derived nArgs is unproven on those two paths.

**C++ changes**
- Rename `savedIP` to `savedIPRaw` and `savedArgCount` to `savedArgCountRaw`.
- Add accessors:
  - `recIP(f)`: when `f.recFull || !g_mcfIpInInfo`, the raw value. Otherwise `(uint8_t*)f.savedMethod.rawBits() + f.ipOff16`.
  - `recArgCount(f)`: the raw value under the same test. Otherwise `(hdr >> 24) & 0xF` of savedMethod, as at MachineCodeFrames.cpp:709.
  - `setRecIP(f, ip)`: promotes the record to full. It writes the raw ip, the derived argCount, `recBcEnd`, a null retSlot and `recFull = 1`.
- Readers to convert:
  - popFrame 17740 and 17749, popFrameForJIT 17903 and 17912.
  - mcfPoppedMemo (Interpreter.hpp:2010). popFrame does not clear bytes 0-1 or savedMethod (17679-17690), so recIP still holds after the pop.
  - mcfResumeAfterReturn (MachineCodeFrames.cpp:1984) and mcfRun (2477).
  - materializeFrameStackImpl 22363-22367.
  - captureCollapsedFrames and tryRestoreCollapsedFrames 10931.
  - nlrSegmentFreeze 11008-11009.
  - nlrSegmentPop 11111 and nlrSegmentDeopt 11199 and 11225: rebase only when recFull is set. ipOff16 survives method motion.
  - nlrSegmentDeopt 11214, which reads the argument count.
- GC:
  - prepareForGC (25503-25528) and afterGC (25883-25893) skip the ip and bcEnd rebasing for records whose recFull is clear.
  - Keep the rule "a moved method drops the memo" (25891-25893). To do that, add `uint64_t methodBits` to FrameGCSide, record it in prepareForGC, and compare it in afterGC.

**Knobs**
- Opt-out: `DEBUG_BOOL_RT(PHARO_MCF_NO_IP_IN_INFO)`, which sets `jit::g_mcfIpInInfo`.
- Oracle: `DEBUG_BOOL(PHARO_MCF_IP_VERIFY)`.
  - The sites still emit the old ldr/add, and the push still stores x13 into savedIPRaw.
  - mcfVerifyPush and popFrame stop the VM when `recIP(f) != f.savedIPRaw` on a record whose recFull is clear.
- Sabotage: `DEBUG_BOOL(PHARO_MCF_IP_SABOTAGE)`, which emits ipOff + 1 at one site kind. The oracle must fire. The selector walk (MachineCodeFrames.cpp:765-790) also catches it.

**Tests**
- The shared gate.
- `PHARO_GC_FRAME_VERIFY`, with the byte check rebuilt from recIP.
- `PHARO_NLR_SEG_VERIFY`, plus the exception, ensure and NLR classes.
- The debugger and Context classes with setup_fake_gui.
- A recursion probe at depth 0 after a yield, so that the depth-0 body runs.
- A count of sites with ipOff > 0xFFFF on lexicon and on the sweep image. The expected count is near 0.

## Final shape

Push (method entry), 19 instructions:

```
ldur w11,[x0,#ctdn]; subs w11,w11,#1; b.le fail; stur w11,[x0,#ctdn]
ldp  x11,x10,[x0,#dirDepth]; sub x16,x11,#1; tbnz x16,#15,slow
add  x8,x24,x11,lsl #7; add x11,x11,#1; stp x11,x27,[x0,#dirDepth]
ldr  x16,[x19,#compiledMethodOop]; stp x16,x12,[x8,#0x30]
stp  x22,x21,[x8,#0x20]; stp x19,x10,[x8,#0x10]; str x14,[x8]
adr  x19,jm; sub x22,x25,#n*8; ldur x21,[x22,#-8]; b codeStart
```

Return, 13 instructions:

```
ldp x3,x5,[x0,#dirDepth]; sub x3,x3,#1; add x7,x24,x3,lsl #7
ldr x9,[x7]; cbz x9,fall; cmp x5,x27; b.eq noCtx; <widow, skipped>
ldp x19,x14,[x7,#0x10]; stp x3,x14,[x0,#dirDepth]
stur x1,[x22,#-8]; mov x25,x22; ldp x22,x21,[x7,#0x20]; br x9
```

Caller, linked site, 8 instructions: W0, W1, cmp, b.ne, `mov w12,#ipOff`, adr x14, adr x15, W2.

Total: 49 -> 40 per linked call pair. The stub-path sites save the same 9.

## New invariants

- **I-PAIR:** currentFrameMaterializedCtx_ is at frameDepth_ + 8, and both are in ldp range of x0. mcfInit checks this; the old emission is the fallback.
- **I-FULL:** recFull is set exactly on records that C++ wrote. On a record whose recFull is clear, the raw ip, argCount, bcEnd and retSlot fields are garbage and are read only through accessors.
- **I-IPOFF:** on a record whose recFull is clear, `savedMethod + ipOff16` is the caller's ip. No native call site has an ipOff above 0xFFFF.
- **I-TB:** savedTB is the caller's fp + 1, and null maps to null.
- **I-ARGC:** a native callee JM's argCount equals the site's nArgs. The link gate, the perform gate and the two new gates enforce this.
- The existing rule stays: no C++ derivation goes through a record's mcJM.

## Deferred, in order of what to measure first

1. **B0 census.** Measure syncs per native call (emitSyncSpToState executions), C++ pops of native records (native calls minus g_mcfCountRets), and sites with ipOff above 0xFFFF. These numbers decide items 2 and 3.
2. **x24 as a record pointer (Design B B5).** About -7, minus a frameDepth_ publish at every sync and a reload after every helper call. Do this only if B0 shows under about 0.5 syncs per call.
3. **savedMethod from mcJM (Design B B4).** -2. This needs fill sites F1-F6 (prepareForGC, forEachRoot, before evictLRU and the full flush, record copies, popFrame, materialize), a shadow-record oracle and eviction stress runs with `PHARO_CODE_ZONE_MB=16`. It breaks the current "no mcJM derivation" rule, so the rule must be rewritten first.
4. **Countdown in w23 (trim-plan step 9).** -2. There are 25 C++ writers of checkCountdown_.
5. **The method entry falls into the body (B7).** -1. Every consumer of `jm + sizeof(JITMethod)` must be checked.
6. **Widow test by memo swap (B8).** -2. Every C++ reader of the memo must undo the swap.
7. **An idea the judge did not check:** derive x15 from x14 by putting a `b refuse` word at a fixed offset before mcfCont. -1 at the caller. Code falls through into mcfCont today, so this needs a layout change at each of the seven sites.

## Could not verify

- The real values of dirDepth and dirMatCtx on build-rel and build-dbg. The Stage 1 runtime check decides; run `PHARO_MCF_CENSUS=1` first.
- Whether mcfExtSendTarget and mcfPrimFailBodyEntry check the argument count somewhere outside the lines read. Add the Stage 4 gates either way.
- The number of send sites with ipOff above 0xFFFF.
- The exact saving on lexicon. The 3.5% figure assumes 970M native calls and that every call saves all 9 instructions.
- Whether the "moved method drops the memo" rule (Interpreter.cpp:25891-25893) is needed at all. The memo points into JM code, which does not move. Stage 4 keeps the rule; removing it is a separate experiment.