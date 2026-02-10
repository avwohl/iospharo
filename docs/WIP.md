# iOS Pharo VM — Status

Last verified: 2026-02-10

---

## How to Run Tests

```bash
# 1. Fresh Pharo 13 image
cd /tmp && curl -sL https://get.pharo.org/64/130 | bash

# 2. Load test runner (standard Pharo VM)
/Users/wohl/Downloads/pharo /tmp/Pharo.image eval --save \
  "'/Users/wohl/src/iospharo/scripts/run_sunit_tests.st' asFileReference fileIn"

# 3. Run with custom VM
./build/test_load_image /tmp/Pharo.image

# 4. Results
cat /tmp/sunit_test_results.txt        # summary per class
cat /tmp/sunit_test_detail.txt         # per-test with run number
cat /tmp/sunit_run_number.txt          # current run counter
```

---

## GC Implementation — In Progress (2026-02-06)

### What's done (commits efae312, a6901a0, 77ed223, 5f73cd8)

A Spur-compatible mark-compact GC has been implemented following the plan in
`.claude/plans/atomic-pondering-bee.md`. Phases 1-4 of 6 are coded:

1. **Phase 1 (Infrastructure)**: ObjectScanner, address-range space detection,
   remembered set vector, segregated free lists
2. **Phase 2 (Root Enumeration)**: IP-to-offset conversion (prepareForGC/afterGC),
   forEachRoot template visiting all ~6200+ interpreter Oops, forEachMemoryRoot
3. **Phase 3 (Mark Phase)**: BFS worklist mark-and-trace, CompiledMethod literal
   scanning, weak object/ephemeron handling
4. **Phase 4 (Planning Compactor)**: Spur-style plan/update/copy with saved first
   fields in eden scratch space, grey bit tracking for forwarding addresses

### GC compactor — Working and Verified (2026-02-09)

Full mark-compact GC works correctly with 32MB headroom. Test suite:
11472/11488 pass with ~5 GC compaction cycles. Mac Catalyst app:
runs 60M+ steps with 8+ GC cycles, no crashes.

Key bugs fixed:
1. **Zero-slot forwarding** (commit 5f73cd8): Objects with 0 slots weren't getting forwarding addresses.
2. **Saved first fields** (commit 5f73cd8): Saved fields in eden scratch space not resolved.
3. **Interior pointer corruption** (commit 7b8675c): `markAndTrace()` called `setMarked()` on
   interior pointers, ORing MarkedBit (0x40000000) into slot values. Fixed with valid object
   start address set built at the beginning of markPhase.
4. **Stale grey bits** (commit 1ca8fb6): Pre-compaction pass only cleared mark bits, not grey.
   Stale grey on pinned objects could desync savedFieldPtr. Fixed by clearing both bits.
5. **Missing setInterpreter in PlatformBridge** (commit 502549e): `vm_loadImage()` in
   PlatformBridge.cpp created the Interpreter but never called `gMemory->setInterpreter()`.
   GC compaction skipped all interpreter root updates (stack, saved frames, method_, receiver_).
   After compaction, stale pointers pointed to zeroed memory → classIdx=0 crash.
   Only affected Mac Catalyst app; test_load_image.cpp already had the call.

### Remaining GC phases (not started)

- **Phase 5 (Scavenger)**: Cheney copy collector for new space (eden → survivor)
- **Phase 6 (Integration)**: Eden allocation, write barrier audit

### Reference implementation
- `src/ios/cointerp-cpp.c` — Spur GC algorithms
  - `fullGC()` at line 52066
  - `planCompactSavingForwarders` at line 63242
  - `updatePointers` around line 66019
  - `copyAndUnmark` around line 64091
  - `mapInterpreterOops` at line 72583

---

## FFI/Display Pipeline — VERIFIED WORKING (2026-02-08)

The SDL display pipeline is fully operational with real SDL2.

**Architecture**: Real SDL2 is statically linked via `-Wl,-force_load` in CMakeLists.txt.
When the Pharo image calls `SDL_Init`, `SDL_CreateWindow`, `SDL_PollEvent` etc. via FFI,
`dlsym(RTLD_DEFAULT, ...)` finds the real SDL2 symbols. The stub functions in FFI.cpp are
dead code (never called by TFFI).

**Verified pipeline** (frame capture proves real pixels):
1. OSSDL2Driver selected as display driver
2. SDL_Init, SDL_CreateWindow, SDL_CreateRenderer called via TFFI → succeed
3. SDL_LockTexture → primitiveCopyBits (BitBlt) → SDL_UnlockTexture → texture filled
4. SDL_RenderCopy → SDL_RenderPresent → frame rendered
5. SDL_PollEvent called ~939 times in 45s (event loop running)
6. Frame capture shows: Pharo logo, menu bar, desktop content

**Remaining display issue**: `GrafPort(Object)>>error:` renders red X pattern over
the main desktop area. Root cause: `SpStyleEnvironmentColorProxy` (a `ProtoObject`
subclass) forwards unknown messages via DNU to `Smalltalk ui theme perform:
colorSelector`. The `isTransparent` and `fillRectangle:on:` messages are forwarded
correctly in principle, but fail because `UITheme current` is not properly initialized
when rendering starts. This is a **session startup ordering issue**: rendering begins
before `SessionManager >> installNewSession` completes all startup handlers (which
include `PharoLightTheme beCurrent`). Not a VM bug — the DNU forwarding mechanism
and Color methods work correctly when the theme is initialized.

**Earlier FFI fix** (commit 491d515): `primitiveFFIIntegerAt` treated all bytes objects
as ExternalAddresses. Fixed to distinguish ByteArray (read directly) from
ExternalAddress (dereference pointer).

---

## Test Results — Run #158 (2026-02-10, with GC)

**576 test classes, 12384 tests. Pass: 12368, Fail: 0, Error: 0, Skip: 16.**

**99.87% pass rate** (12368/12384). GC compaction cycles active.

### New VM features in this run (commits 813c377, 8eaea4c, 2caf7af, 673dc00)

**Primitive 188 mirror fix** (commit 813c377): `primitiveExecuteMethodArgsArray`
3-argument "mirror" form (`CM receiver: rcvr withArguments: args executeMethod: method`)
was ignoring the desired receiver. The CompiledMethod (message receiver) was used instead,
causing instance variable reads/writes on the wrong object. Fix: when argCount > 2, write
desired receiver from stackValue(2) into the message receiver position. Also executes
the target method's primitive if it has one. Unblocked 29 OCAST test failures and 5
decompiler/IR test classes.

**invokeObjectAsMethod**: Non-CompiledMethod objects in method dictionaries
(metalinks, ReflectiveMethod) now correctly get `#run:with:in:` sent to them
instead of crashing the VM with abort(). Matches reference Cog VM behavior.

**SmallInteger arithmetic fast paths**: `arithmeticSend` for bytecodes 0x60-0x6F
now inlines +, -, <, >, <=, >=, =, ~=, *, /, \\, bitShift:, //, bitAnd:, bitOr:
for SmallInteger operands. Bypasses method dictionary entirely.

**FFI 64-bit integer support**: `primitiveFFIIntegerAt` and `primitiveFFIIntegerAtPut`
now handle LargePositiveInteger/LargeNegativeInteger for values outside SmallInteger range.

**Write barrier fix**: `primitiveObjectSetReadOnly` (544) now correctly handles
`setIsReadOnly: false` (making objects mutable again).

**Cache flush primitives**: `primitiveFlushCacheByMethod` (119) and
`primitiveFlushCacheBySelector` (120) now properly clear matching cache entries.

**Class table registration**: `primitiveChangeClass` (115) now registers new classes
in the class table instead of failing.

### Newly enabled classes (Run #151 → #158)
- **All 20 OCAST translator test classes**: 89/89 pass (prim 188 mirror fix)
- **OCIRBuilderTest**: 34/34 pass
- **OCIRPrinterTest**: 18/18 pass
- **OCIRVisitorTest**: 18/18 pass
- **FBIRBytecodeDecompilerTest**: 33/33 pass
- **FBDBytecodeDecompilerExamplesTest**: 49/51 pass
- **SelfVariableTest**: 2/5 pass (3 individually skipped)
- **CodeSimulationTest**: 6/9 pass (SistaV1 simulation missing)
- **OCBytecodeGeneratorTest**: 2/2 pass

### Newly enabled classes (Run #129 → #151)
- **ClassTest**: 41/46 pass (5 class-modification tests excluded)
- **BooleanSlotTest**: 3/3 pass
- **MethodConstantTest**: 5/5 pass (non-method in method dict fixed)
- **OCCacheResetTest**: 1/1 pass (non-method in method dict fixed)
- **OCSpecialSelectorTest**: 2/4 pass (optimized tests pass with fast paths)
- **FFITypesTest**: testSignedLongLong, testUnsignedLongLong now pass (15/15)

### Previously enabled classes (Run #113 → #129)
- **MethodAnnouncementsTest**: 6/10 pass (4 trait timeouts skipped)
- **ProtocolAnnouncementsTest**: 13/14 pass (1 trait timeout skipped)
- **SystemNavigationTest**: 10/11 pass (testAllGlobalNames skipped)
- **RGReadOnlyImageBackendTest**: 19/19 pass (testBehavior timeout resolved)
- **SystemEnvironmentTest**: 217/217 pass
- **GEllipseTest**: 21/23 pass (testEmpty no longer hangs)
- **testNoWeakBlock** (AnnouncerTest): fixed by GC context sync
- **testAddIncludesSizeReclaim**: fixed by GC context sync
- **testWeakOrderedCollectionAllGarbageCollected**: fixed by GC context sync
- **testWeakOrderedCollectionSomeGarbageCollected**: fixed by GC context sync

### Materialized context GC sync fix (Run #109, commit 7670089)

Root cause of testAsArray failure: `materializeFrameStack()` creates heap
Context objects that snapshot C++ stack temps. These contexts are GC roots
(scanned via `forEachRoot`), but their temp slots were never updated when
the C++ stack changed. When `item := nil` nilled a temp, the materialized
Context still held the old Object reference, keeping it alive through GC.

Fix: `prepareForGC()` now syncs all materialized context temps from the
C++ stack before the mark phase. +25 tests from un-skipping testAsArray.

### Weak collection classes (Run #107-#109)
- **WeakSetTest**: 50/50 pass (testAsArray fixed!)
- **WeakIdentitySetTest**: 51/51 pass (testAsArray fixed!)
- **WeakIdentityKeyDictionaryTest**: 206/209 pass (3 reclaim-dependent skipped)
- **WeakOrderedCollectionTest**: 0/2 (both tests are reclaim-dependent, individually skipped)

### GC Compaction — Verified Working
- gcHeadroom_=32MB triggers GC compaction during both test suite and Mac Catalyst
- Test suite: 11754/11769 pass with GC cycles
- Mac Catalyst app: runs 60M+ steps with 8+ GC cycles, no crashes
- Interpreter roots properly updated during compaction via setInterpreter()
- Key fix (commit 502549e): PlatformBridge.cpp was missing setInterpreter() call,
  causing GC to skip all interpreter root updates in Mac Catalyst app

### Skipped test classes (~63)
Categories:
- **Delay-dependent**: ProcessTerminateBugTest, DelayTest, ProcessTest, StopwatchTest, TTLCacheTest
- **GC finalization**: FinalizationRegistryTest, WeakAnnouncerTest
- **Class restructuring (hangs, no Delay timeout)**: SlotIntegrationTest, PropertySlotTest, SlotAnnouncementsTest, SlotLayoutEqualityTest, SlotTraitsTest, SlotLayoutExtensionTest, SlotMigrationTest, ExampleSlotWithStateTest, SlotMethodRecompilationTest
- **Trait class modifications (hangs)**: TraitTest, TraitCompositionTest, ClassTraitTest, MOPTraitTest, TraitPureBehaviorTest, TraitPrecedenceCompositionTest, TraitWithAliasTest, TraitWithConflictsTest, TraitChangesTest, TraitMethodDescriptionTest, TraitOverloadingOfMethodsInTraitedClassTest, TraitPackagingTest, TraitInTraitClassTest, TraitWithMethodsInProtocolsTest, TraitSlotScopeTest, TraitWithSlotsTest, TraitWithComplexSlotsTest
- **Abstract classes**: CollectionRootTest, CDBehaviorParserTest, CDClassDefinitionParserTest
- **Compiler AST/IR**: OCASTVariableTranslatorTest, OCASTSpecialLiteralTranslatorTest, OCASTAndOrTranslatorTest, OCASTBasicTranslatorTest, OCASTBlockTranslatorTest, OCASTSingleBranchConditionalTranslatorTest, OCASTDoubleBranchConditionalTranslatorTest, OCIRBuilderTest, OCIRPrinterTest, OCIRVisitorTest, OCBytecodeGeneratorTest
- **Reflectivity/coverage**: CoverageCollectorTest, CoverageDemoTest
- **FFI callbacks**: FFICallbackParametersTest, FFICallbackTest
- **Network**: EpLogTest, EpCommentTest, GlobalIdentifierMergerTest, GlobalIdentifierWithDefaultConfigurationTest
- **Display**: BitBltTest, ObjectWithPrintingRaisingHaltTest
- **Debugger**: FastStepThroughTest
- **Other**: CodeSimulationTest, SelfVariableTest, BuilderManifestTest, ExecutionCounterTest, ProcessMonitorTestServiceTest, OCCodeReparatorTest, PackageTest, FBDBytecodeDecompilerExamplesTest, FBIRBytecodeDecompilerTest, TestExecutionEnvironmentTest

### Previously enabled classes
- SharedPoolTest, MetaClassTest, MonitorTest, GeneratorTest, BlockClosureTest,
  ClassHierarchyTest, AllocationTest, WeakSetTest, WeakIdentitySetTest,
  WeakIdentityKeyDictionaryTest, WeakOrderedCollectionTest, and many more

### Context identity fix (commit 85810e4)

`materializeFrameStack()` was called multiple times for the same frame stack
(once during block closure creation, once during `thisContext`). Each call
created NEW context objects, breaking `aBlock home == contextOfaBlock`.

Fix: Cache materialized contexts in `SavedFrame.materializedContext` and
`currentFrameMaterializedCtx_`. Subsequent calls reuse cached contexts.
This fixed `BlockClosureTest >> testSetUp` and `testTallyMethods`.

### Recent fixes (Run #258 → #261)

**GC mark phase interior pointer fix (commit 7b8675c)**: `markAndTrace()` could
receive interior pointers (pointing into the middle of another object's slots
rather than at a header). When slot data happened to have bits 0-21 matching a
valid class table entry, `setMarked(true)` was called on it, ORing the MarkedBit
(0x40000000) into the slot VALUE — corrupting the Oop. Fix: build a set of all
valid object start addresses at the beginning of markPhase, check in markAndTrace
before calling setMarked. Source: Context objects (cls=36) with raw stack data
in slots that pass classIndex validation but point mid-object.

**primitiveInputSemaphore fix (commit c66ebe6)**: P153 was mapped to wrong
function. Fixed mapping so InputEventSensor can register for input events.

### Earlier fixes (Run #118 → #258)

**primitiveCopyObject fix (commit 9230cfb)**: Primitive 168 (`Object>>copyFrom:`)
was implemented as a clone (create new object) instead of copy-from (copy argument
contents into receiver). This broke `MethodDictionary>>removeKey:ifAbsent:` which
makes a copy, modifies it, then uses `self copyFrom: copy` to atomically update
the original. Fixed 2 tests:
- BehaviorTest: testLocalMethods, testLocalSelectors

**GC root scanning off-by-one fix (commit c1a2fa0)**: `forEachRoot` scanned
`stackBase_ <= p <= stackPointer_` but push uses post-increment
(`*stackPointer_++ = value`), so stackPointer_ points one past the last live
value. The dead slot contained stale references, keeping weak referents alive.
Changed to `p < stackPointer_` in forEachRoot and two become stack scans.
Fixed 1 test:
- WeakMessageSendTest: testReceiverWithGC

**Context>>restart fix (commit fe288f2)**: `materializeFrameStack` was
overwriting the startpc with the current pc, preventing restart from resetting
to the method's beginning. Fixed by preserving the original startpc. Fixed 1 test:
- ContextTest: testClosureRestart

### Earlier fixes (Run #92 → #118)

**primitiveClass fix (commit 65e551b)**: `primitiveClass` (primitive 111) used
`stackValue(argCount)` (gets receiver) instead of `stackValue(0)` (gets top of
stack = argument). For `Context >> objectClass:` (1-arg), this returned the
Context's class instead of the argument's class. Broke `isFailToken:` checks
in Context stepping simulation. Fixed by always using stackTop, matching
standard VM behavior. Fixed 4 tests:
- ContextTest: testNoStepIntoQuickMethod, testStepIntoQuickMethod, testSteppingAQuickMethod, testBlockCannotReturn

**Deprecation handler fix (commit 72c4010)**: Test runner's `on: Deprecation do:
[:ex | ex resume]` caught Deprecation signals before `defaultAction` could run,
preventing AST rewriting. Changed handler to call `shouldTransform`/`transform`
before resuming, matching standard SUnit behavior while preventing logTranscript
errors from missing stdout. Fixed testTransformingDeprecation.

### Earlier fixes (Run #77 → #92)

**Quick primitives in P118 (commit 4bfaa62)**: `primitiveDoPrimitiveWithArgs`
(primitive 118, used by Context stepping simulation) had no handler for quick
primitives (256-519). These have nullptr entries in primitiveTable_, so P118
failed when image-side `doPrimitive:method:receiver:args:` delegated to
`tryPrimitive:withArgs:`. Added inline handling: 256=return self, 257-263=return
constants, 264+=return instVar. Fixed 8 tests:
- ContextTest: testNoStepIntoQuickMethod, testStepIntoQuickMethod, testSteppingAQuickMethod, testBlockCannotReturn, testClosureRestart
- BehaviorTest: testLocalMethods, testLocalSelectors
- WeakMessageSendTest: testReceiverWithGC

**incrementalGC → fullGC (commit 1ec6d1d)**: `incrementalGC()` (primitive 131,
garbageCollectMost) was only doing a broken scavenge that never processed weak
references. Changed to call fullGC() which does proper mark-sweep with weak
reference processing. Fixed 14 failures in one change: WeakMessageSendTest (2),
ContextTest (8), BehaviorTest (2), ExceptionTest (1), LinkedListTest (1).

**materializeFrameStack bidirectional sync (commit d907f2e)**: First-frame
optimization was writing temps C++ → context, destroying Smalltalk modifications
(like tempNamed:put:). Changed to sync temps context → C++ instead. Fixed
testTempNamedPut.

**BlockClosure/FullBlockClosure format fix (commit 79a65ea)**: Closures were
allocated with ObjectFormat::Indexable (format 2) instead of IndexableWithFixed
(format 3). This caused at:, at:put:, basicSize to not account for fixed fields.
at:1 on a closure read outerContext (slot 0) instead of first copied value.
Fixed testTempNamed.

### Earlier fixes

**FullBlockClosure outerContext fix (commit 33caaca)**: Root cause of
RecursionStopperTest failures. Fix: materialize frame stack when
ignoreOuterContext=false to get correct enclosing context.

**Remove error: intercept (commit f484bcf)**: Removed workaround in sendSelector
that silently swallowed errors. Fixed WriteStreamTest and LimitedWriteStreamTest.

**cannotReturn fix (commit 88dbe8e)**: Send cannotReturn: instead of silent
fallback in returnFromBlock.

**primitiveChangeClass + immutability (commit 467ed76)**: Fix validation and
add immutability enforcement.

### Per-class results (Run #22)

276 test classes, 8939 pass, 0 fail, 0 error, 12 skip.
34 classes skipped at class level (process/delay, weak refs, abstract,
class restructuring, compiler IR/AST execution tests).
testMetaclassSuperclassHierarchy is flaky (passes most runs).

### History
| Run | Date | Classes | Pass | Fail | Error | Skip | Total | Notes |
|---|---|---|---|---|---|---|---|---|
| #1 | 2026-02-06 | 74 | 4362 | 5 | 7 | 6 | 4380 | Test-runner bugs |
| #2 | 2026-02-06 | 74 | 4422 | 0 | 0 | 6 | 4428 | All non-skip pass (prev image) |
| #3 | 2026-02-06 | 13/74 | ~1698 | 1 | 1 | 4 | ~1704 | OOM stall (new image, no GC) |
| #43 | 2026-02-06 | 59 | 4119 | 26 | 39 | 4 | 4199 | 98.1% pass, GC corruption issue |
| #44 | 2026-02-07 | 62 | 4236 | 22 | 29 | 4 | 4301 | **98.49%** Fix primitiveAt heap bounds |
| #33 | 2026-02-07 | 73 | 4278 | 1 | 9 | 4 | 4292 | **99.67%** GC weak fix resolved ~40 failures |
| #57 | 2026-02-07 | 73 | 4246 | 21 | 19 | 4 | 4291 | **98.9%** ExceptionTest fix (+9), DNU lookupClass fix (+1) |
| #72 | 2026-02-07 | 73 | 4270 | 16 | 1 | 4 | 4291 | **99.5%** outerContext fix, error: intercept removal |
| #77 | 2026-02-07 | 73 | 4286 | 1 | 0 | 4 | 4291 | **99.98%** incrementalGC fix, closure format fix, temp sync fix |
| #92 | 2026-02-07 | 73 | 4286+ | 1 | 0 | 4 | 4291 | **99.98%** quick primitive P118 fix — 8 more tests fixed |
| #111 | 2026-02-08 | 73 | 4282 | 5 | 0 | 4 | 4291 | **99.79%** primitiveClass fix — regression discovered |
| #118 | 2026-02-08 | 73 | 4283 | 4 | 0 | 0 | 4291 | **99.81%** Deprecation handler fix, testTransformingDeprecation passes |
| #10 | 2026-02-09 | 81 | 4992 | 0 | 0 | 6 | 4998 | **99.88%** +8 classes, yield fix, context identity fix |
| #13 | 2026-02-09 | 107 | 5934 | 0 | 0 | 6 | 5940 | **99.90%** +26 classes (Tier 7: weak, slot, queue, stream, native) |
| #15 | 2026-02-09 | 139 | 7258 | 0 | 0 | 6 | 7264 | **99.92%** +32 classes (Tier 8: time, hash, cache, compiler, kernel) |
| #22 | 2026-02-09 | 276 | 8939 | 0 | 0 | 12 | 8951 | **99.87%** +137 classes (Tier 9-10: regex, text, AST, OpalCompiler, ClassParser) |
| #32 | 2026-02-09 | 276 | 8939 | 0 | 0 | 12 | 8951 | **99.87%** file truncation fix, same classes |
| #45 | 2026-02-09 | 384 | 10024 | 0 | 0 | 12 | 10036 | **99.88%** +108 classes (Tier 11: ClassParser, Ring, Traits, ClassAnnotation, kernel) |
| #48 | 2026-02-09 | 434 | 10282 | 0 | 0 | 12 | 10294 | **99.88%** +50 classes (Tier 12: AI graph, slots, compiler, system) |
| #50 | 2026-02-09 | 483 | 10717 | 0 | 0 | 13 | 10730 | **99.88%** +49 classes (Tier 13: formatters, diff, text, visitors) |
| #53 | 2026-02-09 | 524 | 11105 | 0 | 0 | 15 | 11120 | **99.87%** +41 classes (Tier 14: FFI, decompiler, equivalence) |
| #56 | 2026-02-09 | 551 | 11246 | 0 | 0 | 15 | 11261 | **99.87%** +27 classes (Tier 15: SUnit self-tests, system, trait printers) |
| #59 | 2026-02-09 | 576 | 11474 | 0 | 0 | 15 | 11489 | **99.87%** +25 classes (Tier 16: geometry, fuzzy, history, commander) |
| #107 | 2026-02-09 | 580 | 11754 | 0 | 0 | 15 | 11769 | **99.87%** +4 weak collection classes enabled (303 new tests) |
| #109 | 2026-02-09 | 576 | 11779 | 0 | 0 | 15 | 11794 | **99.87%** Materialized context GC sync fix, testAsArray passes |

---

## Missing Features (must be built)

### 1. Garbage Collection — IN PROGRESS (see above)
- Mark-compact implemented (Phases 1-4), compactor has pointer update bugs
- Old `#if 0` code has been replaced with new implementation
- Heap set to 512MB (`test_load_image.cpp`), was 48GB

### 2. Heap Growth — NOT IMPLEMENTED
- **Where**: `Primitives.cpp:9444` — primitive 111 always returns 0
- **What**: When allocation fails and GC can't free space, the heap should grow
  via `mmap`/`mprotect`. Currently allocation just returns nil.
- **Impact**: If the 8GB pre-allocation runs out, cascading nil errors.
- **Related**: `ObjectMemory.cpp:147-181` — allocation failure returns `nilObject_`
  after failed GC retry, with counter-limited logging.

### 3. Low-Space Semaphore — NOT SIGNALED ON OOM
- **Where**: Allocation paths in `ObjectMemory.cpp`
- **What**: When memory is low, the VM should signal a semaphore so Smalltalk
  can respond (e.g., trigger GC, free caches). Currently OOM just returns nil.
- **Related**: `primitiveSignalAtBytesLeft` (prim 125) is implemented but never
  triggered by actual low-memory conditions.

### 4. Scavenger / New Space — NOT IMPLEMENTED
- **Where**: `Oop.hpp:178-181` — `space()` always returns `Space::Old`
- **What**: All objects are allocated directly in old space. There is no eden,
  no survivor space, no scavenge cycle. The new-space allocation code exists
  but eden fills up and is never reset (no forwarding pointers).
- **Impact**: No generational GC. Every object lives in old space forever.

### 5. Permanent Space — STUBBED
- **Where**: `Primitives.cpp:7629-7661` — prims 90-93 are no-ops
- **What**: `primitiveMoveToPermSpace` succeeds but does nothing.
  `primitiveIsInPermSpace` always returns false.
- **Impact**: Low — rarely used. Objects can't be made read-only at the GC level.

### 6. Image Saving (Snapshot) — DISABLED
- **Where**: `Primitives.cpp:4692-4707` — primitive 101 returns true immediately
- **What**: Full snapshot implementation exists (lines 4708-4837) but is in
  `#if 0`. The primitive returns true to trick the image into thinking it
  resumed from a saved state.
- **Impact**: Images cannot be saved. Intentionally disabled for consistent
  testing from fresh images.

### 7. TFFI Primitives — IMPLEMENTED (stale handle issue remains)
- **Where**: `Primitives.cpp` — 13 TFFI named primitives + 6 helpers
- **What**: Full ThreadedFFI primitive set implemented: `primitiveFillBasicType`,
  `primitiveTypeByteSize`, `primitiveDefineFunction`, `primitiveSameThreadCallout`,
  `primitiveGetSameThreadRunnerAddress`, etc.
- **Verified**: All primitives work when called (test_tffi.st confirms).
- **Remaining issue**: Saved Pharo images contain stale TFBasicType handles
  (non-null ExternalAddress pointers from the reference VM's `ffi_type*` globals).
  `TFBasicType>>validate` skips `primFillType` when `isValid` returns true.
  TFAbstractType has no session invalidation. Handles must be manually
  invalidated for TFFI to work on fresh startup.
- **Impact**: FFI works mechanically but requires handle invalidation at startup.
  Fixing this requires either image-side session handler or VM-side detection
  of stale pointers.

### 8. FFI Return Marshalling — IMPLEMENTED
- **Where**: `Primitives.cpp` — `primitiveSameThreadCallout`
- **What**: Full argument and return marshalling for all ffi_type categories:
  void, int8-64, uint8-64, float, double, pointer, struct.
  Returns SmallInteger, LargeInteger, Float, or ExternalAddress as appropriate.
- **Status**: Implemented and tested.

### 9. Input Event Loop — NOT STARTING
- **Where**: See `docs/WIP-input-handling.md` for investigation
- **What**: No Smalltalk process polls primitive 264 (getNextEvent). Events
  accumulate in `passThroughEvents_` but nobody reads them.
- **Root cause**: OSSDL2Driver's `setupEventLoop` fails because FFI is broken
  (item 7 above). OSiOSDriver in the standard image is a stub.
- **Impact**: No keyboard or mouse input reaches Smalltalk.
- **Depends on**: FFI type resolution (item 7).

### 10. Timer/Delay Mechanism — WORKING (with priority constraint)
- **Where**: Timer primitives (135/136), `DelaySemaphoreScheduler`
- **What**: `waitTimeoutSeconds:` works correctly when the calling process
  priority is below `TimingPriority` (80). The delay scheduler process runs
  at priority 80 and must be able to preempt the caller. At priority >= 80,
  `DelaySemaphoreScheduler >> schedule:` signals the scheduler but can't
  preempt, so `beingWaitedOn` is never set before the caller checks it,
  causing immediate false timeout.
- **Status**: Working. The test runner uses priority 79 for ExceptionTest
  (which uses delays internally via `runWithNoHandlers:`) and priority 80
  for all other tests.

### 11. Command-Line Args to Image — NOT IMPLEMENTED
- **Where**: `test_load_image.cpp` — no arg passing to Smalltalk
- **What**: Standard Pharo VM accepts `test "PackageName"` etc. Our VM
  requires injecting a test runner script via `fileIn` instead.
- **Impact**: Low — workaround exists (script injection). But limits
  standard Pharo tooling compatibility.

---

## SDL2 Stubs (intentional, not missing)

`FFI.cpp:171-636` — 140+ SDL2 function stubs. These are intentional shims
so the Pharo image's OSSDL2Driver doesn't crash when calling SDL2 via FFI.
`stub_SDL_PollEvent()` actually delivers events from `passThroughEvents_`.
These are not bugs — they're the iOS equivalent of linking against SDL2.

---

## Device/Plugin Primitives (not yet needed)

50+ primitives for hardware features return `PrimitiveResult::Failure`:
- Camera, MIDI, serial, networking, SSL, joystick, clipboard
- iOS sensors (accelerometer, gyroscope, magnetometer, location)
- iOS integration (biometric, IAP, notifications, social sharing)

These are platform features that can be implemented when needed.
Not blocking core VM functionality.

---

## Bugs Fixed (reference)

Key bugs fixed during Feb 2026 test suite work:

1. `nil = nil` returned false (arithmetic nil workaround removed)
2. Directed super sends (bytecode 0xEB) not dispatching primitives
3. `becomeForward:` not updating stack temps
4. `primitiveNewMethod` wrong literal count and missing bytecode space
5. `objectAt:/objectAtPut:` wrong SmallInteger tag decoding
6. 64-bit word array (DoubleWordArray) size/access all wrong
7. `shallowCopy` corrupted objects with >254 slots (overflow word)
8. Non-local returns from nested blocks failed
9. Super sends not dispatching primitives before method activation
10. Millisecond clock mask mismatch causing semaphore timer failures
