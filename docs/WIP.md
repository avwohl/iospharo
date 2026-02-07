# iOS Pharo VM — Status

Last verified: 2026-02-07

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

### Current bug: Compactor corrupts pointers

**The forced GC test (fullGC after image load, before execution) crashes the VM.**

Symptoms:
- Mark phase works: marks 63,308 objects in ~62MB, takes 33ms
- Compactor reports reclaiming ~296 bytes (a few dead objects exist in fresh image)
- After GC, execution crashes with SIGSEGV in `lookupInMethodDict` — a method
  dictionary pointer was corrupted during compaction

**Root cause analysis (partially debugged):**

Two bugs were found and fixed in commit 5f73cd8:

1. **Zero-slot forwarding**: `planCompactSavingForwarders()` only stored forwarding
   addresses for objects with `slotCount() > 0`. Zero-slot objects that moved got
   no forwarding → dangling pointers. Fixed by using raw `(obj + 1)` pointer.

2. **Saved first fields not updated**: The saved first fields in eden scratch space
   weren't being scanned for forward resolution. If slot 0 of object A pointed to
   moved object B, the saved copy of A's slot 0 wouldn't be updated. Added a
   lockstep scan pass to resolve forwarding in saved fields.

**These fixes reduced the corruption but did NOT eliminate it.** The VM still
crashes after forced GC. There may be additional pointer update issues.

### How to finish debugging the compactor

The build has a compile error in debug logging added to `planCompactSavingForwarders`
(variables `deadCount`, `deadBytes` etc. used before the function's `return`).
Fix the compile error, then:

1. **Find what's dead**: The debug logging (once compiling) will show which objects
   are not marked. In a fresh image, almost nothing should be dead. If objects ARE
   dead, the mark phase is missing them — check root enumeration.

2. **Verify pointer update completeness**: After `updatePointersAfterCompact`, scan
   ALL objects and check that every pointer field points to a valid marked object
   at its new location. Any pointer to the old location of a moved object = bug.

3. **Check overflow headers**: Objects with >254 slots have an 8-byte overflow word
   before the header. The compactor must move both together. Check `totalSize()`
   includes the overflow word. Check that forwarding address computation accounts
   for overflow headers when the target location has one.

4. **Check class table**: The class table contains raw Oop pointers. `forEachMemoryRoot`
   must visit ALL class table entries. If a class object moves, its class table entry
   must be updated.

5. **Check Oop::setNilBits()**: After GC, if nil moved, `Oop::setNilBits()` is called
   to update the nil singleton. Verify this actually works — check that `Oop::nil()`
   returns the new address.

### Key test: Run GC then verify execution

```bash
# The forced GC test is in test_load_image.cpp (currently commented out)
# Uncomment the "Forced GC Test" block before the execution loop
# If GC works, the test suite should run identically before and after GC
```

### Remaining GC phases (not started)

- **Phase 5 (Scavenger)**: Cheney copy collector for new space (eden → survivor)
- **Phase 6 (Integration)**: Eden allocation, GC safe point in bytecode loop,
  write barrier audit, heap reduction to 256MB

### Reference implementation
- `src/ios/cointerp-cpp.c` — Spur GC algorithms
  - `fullGC()` at line 52066
  - `planCompactSavingForwarders` at line 63242
  - `updatePointers` around line 66019
  - `copyAndUnmark` around line 64091
  - `mapInterpreterOops` at line 72583

---

## Test Results — Run #44 (2026-02-07)

**62 test classes, 4301 tests. Pass: 4236, Fail: 22, Error: 29, Skip: 4.**

**98.49% pass rate** (4236/4301). Clean exit (no crashes). 8 classes skipped (process/timer-dependent).

### Key fix this run: primitiveAt heap bounds check (commit 3cfcc80)
The heap bounds check in `primitiveAt` assumed perm space starts before old space
(`ptr < permSpaceStart`). But the actual memory layout has old space at lower
addresses (0x300M-0x400M) and perm space at higher addresses (0x4D6M+). This
caused ALL old-space objects to be falsely rejected by primitive 60, triggering
constant `SubscriptOutOfBounds` exceptions during startup and preventing the VM
from ever reaching idle state. Fixed to use proper `isOldObject/isYoungObject/isPermObject` checks.

### Per-class results

| Class | Total | Pass | Fail | Error | Skip | Notes |
|---|---|---|---|---|---|---|
| SortedCollectionTest | 287 | 287 | 0 | 0 | 0 | |
| IdentitySetTest | 176 | 176 | 0 | 0 | 0 | |
| SmallIntegerTest | 29 | 29 | 0 | 0 | 0 | |
| IntegerTest | 83 | 78 | 0 | 0 | 3 | Skips: testCreationFromBytes |
| FloatTest | 75 | 74 | 0 | 0 | 1 | Skip: testNaNCompare |
| FractionTest | 32 | 32 | 0 | 0 | 0 | |
| PointTest | 36 | 36 | 0 | 0 | 0 | |
| CharacterTest | 19 | 17 | 0 | 0 | 0 | 2 missing (not counted) |
| DictionaryTest | 205 | 205 | 0 | 0 | 0 | |
| SetTest | 174 | 174 | 0 | 0 | 0 | |
| BagTest | 168 | 168 | 0 | 0 | 0 | |
| IntervalTest | 260 | 260 | 0 | 0 | 0 | |
| SymbolTest | 268 | 268 | 0 | 0 | 0 | |
| OrderedCollectionTest | 351 | 351 | 0 | 0 | 0 | |
| ArrayTest | 324 | 324 | 0 | 0 | 0 | |
| StringTest | 438 | 438 | 0 | 0 | 0 | |
| HeapTest | 148 | 148 | 0 | 0 | 0 | |
| ContextTest | 34 | 25 | 7 | 1 | 0 | Context manipulation |
| ExceptionTest | 47 | 37 | 1 | 9 | 0 | 9 "Timeout for block execution" |
| BecomeTest | 8 | 8 | 0 | 0 | 0 | **Fixed** (was 5/8) |
| BooleanTest | 5 | 5 | 0 | 0 | 0 | |
| TrueTest | 17 | 17 | 0 | 0 | 0 | |
| FalseTest | 17 | 17 | 0 | 0 | 0 | |
| ProtoObjectTest | 17 | 15 | 0 | 0 | 0 | |
| ObjectTest | 28 | 24 | 4 | 0 | 0 | adopt/readOnly/changeClass |
| UndefinedObjectTest | 19 | 19 | 0 | 0 | 0 | |
| RecursionStopperTest | 4 | 3 | 1 | 0 | 0 | |
| LocalRecursionStopperTest | 4 | 3 | 1 | 0 | 0 | |
| LargePositiveIntegerTest | 19 | 18 | 0 | 0 | 0 | |
| LargeNegativeIntegerTest | 15 | 15 | 0 | 0 | 0 | |
| IntegerDigitLogicTest | 7 | 7 | 0 | 0 | 0 | |
| NumberTest | 23 | 23 | 0 | 0 | 0 | |
| MagnitudeTest | 7 | 7 | 0 | 0 | 0 | |
| ScaledDecimalTest | 36 | 36 | 0 | 0 | 0 | |
| BehaviorTest | 45 | 40 | 2 | 2 | 0 | |
| CompiledCodeTest | 32 | 31 | 0 | 1 | 0 | |
| CompiledBlockTest | 2 | 2 | 0 | 0 | 0 | |
| ClassDescriptionTest | 29 | 19 | 0 | 10 | 0 | nil >> #handlesAnnouncement: |
| BasicBehaviorClassMetaclassTest | 9 | 9 | 0 | 0 | 0 | **Fixed** (was 8/9) |
| PragmaTest | 10 | 8 | 0 | 1 | 0 | |
| DeprecationTest | 2 | 1 | 1 | 0 | 0 | |
| MessageNotUnderstoodTest | 2 | 1 | 1 | 0 | 0 | nil receiver in DNU msg |
| WeakMessageSendTest | 11 | 5 | 1 | 5 | 0 | GC-related failures |
| ObjectLayoutTest | 1 | 1 | 0 | 0 | 0 | |
| DependentsArrayTest | 1 | 1 | 0 | 0 | 0 | |
| LinkedListTest | 255 | 254 | 1 | 0 | 0 | |
| OrderedDictionaryTest | 67 | 67 | 0 | 0 | 0 | **Fixed** (was 65/67) |
| IdentityDictionaryTest | 206 | 206 | 0 | 0 | 0 | |
| StackTest | 13 | 13 | 0 | 0 | 0 | |
| DoubleLinkedListTest | 22 | 22 | 0 | 0 | 0 | |
| ByteArrayTest | 12 | 12 | 0 | 0 | 0 | |
| RunArrayTest | 35 | 35 | 0 | 0 | 0 | |
| AssociationTest | 13 | 13 | 0 | 0 | 0 | |
| ReduceTest | 8 | 8 | 0 | 0 | 0 | |
| WideStringTest | 19 | 19 | 0 | 0 | 0 | |
| ByteSymbolTest | 13 | 13 | 0 | 0 | 0 | |
| ReadStreamTest | 12 | 12 | 0 | 0 | 0 | **New** |
| WriteStreamTest | 19 | 18 | 1 | 0 | 0 | **New** |
| ReadWriteStreamTest | 19 | 19 | 0 | 0 | 0 | **New** |
| LimitedWriteStreamTest | 23 | 22 | 1 | 0 | 0 | **New** |
| RandomTest | 16 | 16 | 0 | 0 | 0 | **New** |
| NumberParserTest | 25 | 25 | 0 | 0 | 0 | **New** |
| NumberParsingTest | 13 | 13 | 0 | 0 | 0 | **New** |

Skipped: BlockClosureTest, SemaphoreTest, ProcessTerminateBugTest,
ProcessSpecificTest, MonitorTest, DelayTest, GeneratorTest, AllocationTest,
ClassHierarchyTest, MetaClassTest, SharedPoolTest.

### Known issues

1. **Context manipulation tests** (8 failures): ContextTest failures in testActiveHome,
   testHome, testTempNamed etc. - our inline frame stack doesn't perfectly match
   Pharo's expected Context object layout for debugging/stepping.

2. **ExceptionTest timeouts** (9 errors): Tests rely on `UnhandledError` process
   which requires process scheduling to deliver errors to a handler process.

3. **ClassFactory/Announcements** (10+ errors): `nil >> #handlesAnnouncement:` in
   ClassDescriptionTest, SharedPoolTest. The announcement system has a nil reference
   somewhere in the class modification notification chain.

4. **MessageNotUnderstoodTest**: Error message shows `nil >> #a` instead of
   `SmallInteger >> #a` — the receiver in DNU message is nil.

5. **WeakMessageSend** (5 errors): GC-related - weak references not being cleared
   properly without a working scavenger.

### History
| Run | Date | Classes | Pass | Fail | Error | Skip | Total | Notes |
|---|---|---|---|---|---|---|---|---|
| #1 | 2026-02-06 | 74 | 4362 | 5 | 7 | 6 | 4380 | Test-runner bugs |
| #2 | 2026-02-06 | 74 | 4422 | 0 | 0 | 6 | 4428 | All non-skip pass (prev image) |
| #3 | 2026-02-06 | 13/74 | ~1698 | 1 | 1 | 4 | ~1704 | OOM stall (new image, no GC) |
| #43 | 2026-02-06 | 59 | 4119 | 26 | 39 | 4 | 4199 | 98.1% pass, GC corruption issue |
| #44 | 2026-02-07 | 62 | 4236 | 22 | 29 | 4 | 4301 | **98.49%** Fix primitiveAt heap bounds |

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

### 10. Timer/Semaphore Reliability — POSSIBLY RESOLVED
- **Where**: Timer primitives, semaphore signaling
- **What**: `waitTimeoutSeconds:` timer was thought to not fire correctly
  after extended execution. However, `testPrintingRecursive` now passes
  in the full test suite with the 2B step limit, suggesting the issue
  was the 200M step limit rather than a timer bug.
- **Status**: Needs more investigation to confirm timers are fully reliable.

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
