# B6 step 1 — prim-111 `#class` inline (dispatch-A, no patching): concrete edit plan

All anchors verified at HEAD `124877dd` (branch `jit`). The two compare sites `cmp x6,#24 -> tryPrimClass` already exist at AsmjitT1.cpp:5467-5470 (j2jBail) and 5541-5544 (NO_INLINE_J2J else-branch), and the snippet body exists at 6745-6761 — but the default-config live path (bit-60-clear fall-through) has no pk compare, and hot ICs never get pk=24. Two independent fixes are both required (empirical: `class=7963` of 3M with the else-branch live; `class=0` default).

## Phase 1a — heap receivers (the bench lever)

### Edit 1: emit-side dispatch, `AsmjitT1.cpp` : `emitOne_arm64`, send case, inlineJ2J fall-through

Insert in the nArgs==0 heap-receiver block, AFTER the bit-57 multiSlot test (line 4260-4262), BEFORE `a.b(dispatchCached)` at line 4263:

```cpp
// B6: primKind 16/20/24 dispatch for bit-60-clear entries (size /
// identityHash / class).  Mirrors the j2jBail compares at 5458-5470;
// without this, pk-classified entries whose callee has no JITMethod
// (prim 111/75 can't compile -> bit 60 never set) always exited via
// dispatchCached.  Same "wired but unreached" pattern as 4150-4155.
if (nArgs == 0 && g_debug.t1InlinePrimAt) {
    a.lsr(x6, x7, asmjit::Imm(48));
    a.and_(x6, x6, asmjit::Imm(0x1F));
    if (!GET_DEBUG_BOOL(PHARO_T1_NO_INLINE_SIZE)) {
        a.cmp(x6, asmjit::Imm(16)); a.b_eq(tryPrimSize);          // optional companion
    }
    if (!GET_DEBUG_BOOL(PHARO_T1_NO_INLINE_IDH)) {
        a.cmp(x6, asmjit::Imm(20)); a.b_eq(tryPrimIdentityHash);  // optional companion
    }
    if (!GET_DEBUG_BOOL(PHARO_T1_NO_INLINE_CLASS)) {
        a.cmp(x6, asmjit::Imm(24)); a.b_eq(tryPrimClass);         // MANDATORY
    }
}
```

Register contract at this point (verified): x0=JITState, x1=receiver (loaded 3885, heap-proven by `tst x1,#7; b_ne` 4217-4221), x2=SP copy (emitLoadSp 3883), x5=icDataPtr (live; tryPrimClass may clobber — it never bails; tryPrimIdentityHash's unhashed bail leaves x5 untouched per 6702 comment, so its contract also holds here), x7=extras (loaded 4078; the tbnz bit tests don't clobber), x6=established scratch. Ordering constraint kept: bit-58 retLit is dispatched at 4193 BEFORE this block (bit 58 reuses bits 50:48, and the patcher never sets both — comment stencils.cpp:1571-1575).

The snippet body (6745-6761) is unchanged in 1a. Emitted sequence for the record (entry x1=heap receiver, x2=SP; result overwrites the receiver stack slot; returns via `b endOfSend`, no sp adjust since nArgs==0):

```
tryPrimClass:
  [emitIncPrimCounter &g_primClass_hits]      ; only emitted under PHARO_T1_INLINE_PRIM_COUNTERS=1
  ldr  x4, [x1]                               ; header word
  and  x4, x4, ObjectHeader::ClassIndexMask   ; bits 0-21 (0x3FFFFF = 22 contiguous bits, valid AArch64 imm)
  lsl  x4, x4, #3
  mov  x5, &pharo::g_classTableBase
  ldr  x5, [x5]                               ; flat table base (malloc, resize-once: ObjectMemory.cpp:111-115)
  add  x5, x5, x4
  ldr  x5, [x5]                               ; class oop — fresh load each send => GC-current
  stur x5, [x2, rcvrOffsetBytes]              ; rcvrOffsetBytes = -8; receiver slot := result
  b    endOfSend
```

No bail path, no forwarder/immediate handling needed in 1a: the IC key compare already excludes both (a forwarder's pun classIndex can never equal a cached key, and the 3905-3909 key computation routes immediates to tag|0x80000000 keys; same invariant size/idh rely on, comment 6740-6743).

### Edit 2: classifier, `Interpreter.cpp` : `upgradeICToJ2J` (the hot/megacache lane)

The callee needs NO JITMethod — the snippet replaces the callee entirely; extras pk-without-bit-60 is the desired terminal state (faster than any J2J call would be). Today prim-111 sites loop forever: eager compile fails -> negative-cache -> `return` at 21648 -> extras stay 0 -> per-send C++ round trip + 3 hash lookups.

Insert AFTER the debug selector-skip blocks, BEFORE the methodMap lookup at 21529:

```cpp
// B6 header-only prims: the dispatch-A snippet replaces the callee, so
// classify extras = pk<<48 (NO bit 60) and return WITHOUT compiling.
uint64_t headerPrimExtra = 0;
{
    ObjectHeader* methObj = cachedMethod.asObjectPtr();
    Oop hdr = methObj->slotAt(0);
    if (hdr.isSmallInteger() && ((hdr.asSmallInteger() >> 16) & 1)) {
        int primIdx = primitiveIndexOf(cachedMethod);
        if (primIdx == 111 && !GET_DEBUG_BOOL(PHARO_T1_NO_INLINE_CLASS))
            headerPrimExtra = (uint64_t)24 << 48;
        else if (primIdx == 75 && !GET_DEBUG_BOOL(PHARO_T1_NO_INLINE_IDH))
            headerPrimExtra = (uint64_t)20 << 48;   // optional companion (75 also can't compile)
    }
}
if (headerPrimExtra != 0) {
    // Key derivation = exact copy of 21689-21699.
    Oop receiver = stackPointer_[-(sendArgCount + 1)];
    uint64_t tag = receiver.rawBits() & 0x7;
    uint64_t lookupKey;
    if (tag == 0 && receiver.rawBits() >= 0x10000) lookupKey = receiver.asObjectPtr()->classIndex();
    else if (tag != 0) lookupKey = tag | 0x80000000ULL;
    else return;
    int firstEmpty = -1;
    for (int e = 0; e < (int)jit::IC_ENTRIES_PER_SITE; e++) {
        if (icData[e*3] == lookupKey) {
            if (icData[e*3+2] == 0) {              // never overwrite (matches 21708 policy)
                icData[e*3+2] = headerPrimExtra;
                if (e == 0 && callerMethod.isObject())   // PMS §6 hook, mirrors 21770-21773
                    jitRuntime_.rederiveSiteForICData(callerMethod.rawBits(), icData);
                // + noteLateSpecBit mirror of 21759-21765 so tier-2 recompile picks it up
            }
            return;
        }
        if (firstEmpty < 0 && icData[e*3] == 0) firstEmpty = e;
    }
    if (firstEmpty >= 0 && fillEnabled) {
        icData[firstEmpty*3]   = lookupKey;
        icData[firstEmpty*3+1] = cachedMethod.rawBits();
        icData[firstEmpty*3+2] = headerPrimExtra;
        // same slot-0 rederive + noteLateSpecBit hooks
    }
    return;
}
```

Notes: no eager compile happens on this path, so no eviction, so the icData-dangle re-validation (21581-21598) is not needed. Precedent for a "marker-bits-only, no target" early block: the 207/209 BLOCK_VALUE_BIT block at 21609-21639. Once classified, the site never exits to C++ again, so this code itself leaves the hot path.

`patchJITICAfterSend` needs NO change: cold fill already writes pk=24 regardless of compile status (21240-21243), and the unsafePrim guard already withholds bit 60 — pk-only extras are exactly what dispatch-A wants.

### Edit 3: stencil-tier hardening, `stencils.cpp:1580`

`if (primKind >= 17)` routes pk 19-24 into `_HOLE_RT_NEW_PRIM` (jit_rt_new_prim reads the receiver as a class object). Edit 2 makes pk=24 extras far more common; tighten to `if (primKind == 17 || primKind == 18)` so 19-24 fall to the generic slow send at stencil-compiled sites. (Misroute exists TODAY from cold-fill pk=24; this is a required safety companion, not optional.)

## Phase 1b — immediate receivers (`5 class`, `$a class`, `3.5 class`)

Cog steal: in `genGetClassObjectOf:` the tag bits ARE the class-table index. This VM's tags (Oop.hpp: SmI=1, Char=3, SmFloat=5) differ from stock Spur's pun layout (slot 3 = SmallInteger 32-bit pun, slot 5 contents unverified), and classOf's 3-way branch (ObjectMemory.cpp:428-437) is currently the only tag->class mapping. Make the flat table serve tags directly:

### Edit 4: `ObjectMemory.{hpp,cpp}` — one-time alias fixup + save restore

- New `ObjectMemory::installImmediateClassTableAliases()`, called once at the end of image load (after ImageLoader.cpp:352-398 flattening and specialObjectsArray_ set):
  - save `aliasOrig3_ = classAtIndex(3)`, `aliasOrig5_ = classAtIndex(5)` (new Oop fields, added to `forEachMemoryRoot` visitation next to `hiddenRootsObj_` so they stay GC-current);
  - `setClassAtIndex(1, specialObject(ClassSmallInteger))` (assert it already equals — stock layout); `setClassAtIndex(3, specialObject(ClassCharacter))`; `setClassAtIndex(5, classAtIndex(4))` (SmallFloat64).
  - GC keeps aliases current automatically: `forEachMemoryRoot(includeClassTable=true)` walks every non-nil flat entry i>=1 (ObjectMemory.hpp:1008-1014) on scavenge (1657) and compaction (3648). Weak-entry concern is moot — all three classes are strongly held via specialObjectsArray.
- `syncClassTableToHeap()` (ObjectMemory.cpp:3275, runs before save): write `aliasOrig3_`/`aliasOrig5_` back for indices 3 and 5 instead of `classTable_[i]`, so SAVED images stay byte-stock (image-compat hard rule; in-heap pages are never touched at runtime, so in-image reflective reads see stock values throughout).

### Edit 5: emit changes for immediates (gated by NEW opt-out knob)

- `debug_vars.h`: `DEBUG_BOOL(PHARO_T1_NO_INLINE_CLASS_IMM)` next to line 111, house default-ON-feature pattern.
- Dispatch: in the fall-through block only (the only live path for pk-only entries), add BEFORE the `a.tst(x1, Imm(0x7))` split at 4217 (after the bit-58/54 pre-split dispatches):
  ```cpp
  if (nArgs == 0 && g_debug.t1InlinePrimAt
          && !GET_DEBUG_BOOL(PHARO_T1_NO_INLINE_CLASS)
          && !GET_DEBUG_BOOL(PHARO_T1_NO_INLINE_CLASS_IMM)) {
      a.lsr(x6, x7, asmjit::Imm(48));
      a.and_(x6, x6, asmjit::Imm(0x1F));
      a.cmp(x6, asmjit::Imm(24));
      a.b_eq(tryPrimClass);            // now any-tag capable
  }
  ```
  (and then DROP the heap-block pk-24 compare from Edit 1 when the imm knob is on, to avoid a dead duplicate). j2jBail/else-branch sites keep their existing heap-only compares — pk-only entries never reach j2jBail (needs bit 60), and the else branch is non-default config; accepted gap.
- `tryPrimClass` body grows a 3-instruction tag branch:
  ```
  tryPrimClass:
    tst  x1, #7
    b.eq LclsHeap
    and  x4, x1, #7          ; tag IS the table index (aliases at 1/3/5)
    b    LclsIdx
  LclsHeap:
    ldr  x4, [x1]
    and  x4, x4, ObjectHeader::ClassIndexMask
  LclsIdx:
    lsl x4,#3 ; mov x5,&g_classTableBase ; ldr x5,[x5] ; add ; ldr x5,[x5]
    stur x5, [x2, rcvrOffsetBytes] ; b endOfSend
  ```
  (`#7` is a valid bitmask immediate; no asmjit silent-drop exposure. Knob-off emits today's heap-only body byte-identically.)

Fallback if alias validation fails: 3-way tag compare in the snippet loading from new GC-maintained `g_classSmallInteger/Character` mirrors — more machinery; only if aliases prove unsafe.

## Knob summary

```
PHARO_T1_NO_INLINE_CLASS      existing (debug_vars.h:111) — now master opt-out: gates Edit 1 compare,
                              Edit 2 classifier write, Edit 5 — knob-on = byte-identical emit + no new IC writes
PHARO_T1_NO_INLINE_CLASS_IMM  NEW DEBUG_BOOL — opt-out for 1b (dispatch position, tag branch, alias install)
PHARO_T1_NO_INLINE_SIZE/IDH   existing — gate the optional 16/20 companions in Edit 1 / Edit 2
PHARO_T1_INLINE_PRIM_COUNTERS must be set to see class= nonzero (counter compiled out otherwise)
```
No DebugSettings.cpp changes (frozen/ratchet).

## What we steal from Cog vs not

Steal: tag-bits-as-classIndex (genGetClassObjectOf, CogObjectRepresentationForSpur:842-889) via the alias fixup; unfailing/no-fallback semantics (genPrimitiveClass returns UnfailingPrimitive) — snippet has zero bail paths. Not stolen: the two-level table walk (our flat mirror + g_classTableBase is better) and compiling the callee (Cog cogs Object>>class frameless). Callee-less dispatch-A is strictly faster for linked sites; adding 111 to `supportedPrimIndex` + `emitPrimProlog_arm64` is step 2 (would set bit 60 and route via tryInlineJ2J — deliberately NOT done now so the inline snippet stays the winning path).

## Validation ladder

```
0  build; knob-off A/B (PHARO_T1_NO_INLINE_CLASS=1 [+_IMM]) — bench suite + startup identical
1  eval smokes (each `... class name` and identity):
     (Object new) class == Object        nil class name = 'UndefinedObject'
     5 class = SmallInteger              3.5 class name = 'SmallFloat64'
     $a class name = 'Character'         'x' class / #(1 2) class / true class
     JIT-forcing loop: | o | o := Object new. 1 to: 3000000 do: [:i | o class]. o class name
     polymorphic: | r | r := {1. $a. 3.5. Object new. nil}. 1 to: 1000000 do: [:i | (r at: i\\5+1) class]
2  PHARO_T1_INLINE_PRIM_COUNTERS=1, 3M loop: stats class= ~3,000,000 (was 0); chain primChain for
     prim 111 (jitPrimChainHisto_[111]) ~0 (was 833,528)
3  /tmp/bench2.st + bench3.st: class-send-3M-ms 149 -> noop-send territory (~10-30 ms expected;
     this is the B6 gate per patched-ic-design.md §11); other lines and 19/19 bench unchanged
4  SUnit sentinel: printf 'DictionaryTest\n' > /tmp/sunit_class_names.txt; per-test identical vs
     knob-off on a 60-class batch.  SILENT-RUNNER checklist: rm /tmp/sunit_run_completed.txt,
     rm <imagedir>/startup.st, batch file is 1-BASED, PHARO_MAX_STEPS=2e12, check PharoDebug.log
5  PHARO_DET_SCHED=1 AIPrimTest/AITarjanTest ERROR=0 (design §12 step 3)
6  GC stress: alloc-heavy loop interleaving fullGC + class sends (compaction rewrites entries; fresh
     ldr must track); 1b: snapshot via prim 97 -> reload saved image under STOCK pharo VM and eval
     `3 class name` + `$a class name` (proves syncClassTableToHeap alias restore)
7  full suite Δcog via scripts/classify-sunit.py; -O2 build for any headline numbers
     (dev build is -O0 — memory dev-build-is-unoptimized-O0)
```

## Open risks (all flagged)

1. The snippet body has near-zero production mileage (class=0 default, 7963 in one off-config run) — treat as new code despite existing emit; full ladder mandatory.
2. PMS interaction: confirm `rederiveSiteForICData`/`linkSendSite` treat pk-only extras as not-patchable (they already see pk-only from cold fill, but Edit 2 raises frequency); the new classifier mirrors the slot-0 rederive hook (21770-21773) — review with PMS owner of design §6.
3. Method redefinition: if Object>>class were recompiled to a non-prim-111 method, stale pk=24 extras inline old semantics. Same exposure class as existing pk 14/15/16 extras — verify the method-install IC flush clears extras words, not just keys.
4. Alias fixup (1b): slot 3 currently holds the stock SmallInteger pun, slot 5 unverified — assert/log contents at install; grep `classAtIndex(` callers for anything consuming indices 1-5 besides classOf(4); any VM primitive exposing the flat table would now answer Character at index 3.
5. Class-identityHash aliasing (1b): Character class sits at flat index 3 AND its real hash index — verify registerClass/become have no index==hash assert (stock Spur tolerates puns).
6. Saved-image purity (1b) hangs on syncClassTableToHeap being the ONLY flat→heap sync and running on every save path — verify prim 97 call chain.
7. Edit 3 changes stencil routing for pk 19-23 too (slow-send instead of jit_rt_new_prim fall-through) — A/B a stencil-tier config.
8. x86_64 mirror (AsmjitT1.cpp:2046-2074) has the same fall-through gap — out of scope here; queue for jit-x86 branch.
9. tryPrimClass clobbers x5 legally only because it cannot bail; document in the emit comment that any future bail must stash to OFF_ICDATAPTR and use dispatchCachedRestoreX5.
10. Optional 16/20 companions widen the blast radius of Edit 1 — land class-only first if bisectability is prized; each has its own existing opt-out knob.
11. Memory file `vm-speed-lever-dispatch.md` claim "prim 111 not mapped" is stale (Interpreter.cpp:20560 exists) — correct it when landing; also consider a shared `kPrimKindClass = 24` constant instead of bare 24 at the now-4 compare sites + 2 classifier sites.

Files touched: `src/vm/jit/asmjit/AsmjitT1.cpp` (emitOne_arm64 send case: ~4260, ~4217, 6745-6761), `src/vm/Interpreter.cpp` (upgradeICToJ2J ~21528), `src/vm/jit/stencils/stencils.cpp` (1580), `src/vm/debug_vars.h` (+1 knob), `src/vm/ObjectMemory.hpp/.cpp` + `src/vm/ImageLoader.cpp` (1b alias install/restore/visit), docs/patched-ic-design.md §11/§13.