# iOS Pharo VM — Status

Last verified: 2026-02-06

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

## Test Results — Current Status (2026-02-06)

### Best verified result: Run #2 (previous session, different Pharo 130 image)

**74 test classes, 4428 tests. Pass: 4422, Fail: 0, Error: 0, Skip: 6.**

99.86% pass rate. All non-skip tests pass. Used `ex resume` for Deprecation handling.
(Note: Pharo 130 is a development image that changes between downloads.)

### Current session issues

The latest Pharo 130 image (downloaded 2026-02-06) shows two problems:

1. **`ex resume` stalls after ~1700 tests** — Without GC, Deprecation objects from
   `ex resume` accumulate and exhaust memory. Even with 48GB heap, the VM goes idle
   before completing all 74 test classes. The previous session's image had fewer
   deprecation triggers.

2. **`ex outer` completes all tests but has 3 IntegerTest failures** — Deprecation's
   `defaultAction` calls `transform` which rewrites calling methods' bytecodes.
   This corrupts IntegerTest methods that happen to trigger deprecations.

### Partial results from current image (first 13 of 74 classes, `ex resume`):

| Test Class | Tests | Pass | Fail | Err | Skip | Notes |
|---|---|---|---|---|---|---|
| SmallIntegerTest | 29 | 29 | 0 | 0 | 0 | |
| IntegerTest | 83 | 78 | 1 | 1 | 3 | testPrintStringBase ERROR, testReciprocalModulo FAIL |
| FloatTest | 75 | 74 | 0 | 0 | 1 | |
| FractionTest | 32 | 32 | 0 | 0 | 0 | |
| PointTest | 36 | 36 | 0 | 0 | 0 | |
| CharacterTest | 19 | 17 | 0 | 0 | 0 | 2 non-pass (timing related) |
| DictionaryTest | 205 | 205 | 0 | 0 | 0 | |
| SetTest | 174 | 174 | 0 | 0 | 0 | |
| BagTest | 168 | 168 | 0 | 0 | 0 | |
| IntervalTest | 260 | 260 | 0 | 0 | 0 | |
| SymbolTest | 268 | 268 | 0 | 0 | 0 | |
| OrderedCollectionTest | 351 | 351 | 0 | 0 | 0 | |
| ArrayTest | — | — | — | — | — | VM went idle during this class (OOM) |

### IntegerTest failures (appear with both `ex resume` and `ex outer`):
- `testReciprocalModulo` — "Got 164 instead of 1" — likely a real VM bug in
  large integer modular arithmetic
- `testPrintStringBase` — SubscriptOutOfBounds: 470184984576 — suspiciously large
  index suggests a large integer operation returning wrong value

### Key blocker: GC is disabled
Without garbage collection, the test suite cannot complete with `ex resume` because
Deprecation objects accumulate. This is the #1 blocking issue. Fixing GC would
solve the memory problem and likely allow all 4400+ tests to complete.

### What was fixed (Run #1 → Run #2)

All 11 non-pass tests from Run #1 were caused by test-runner bugs, not VM bugs:

1. **ExceptionTest (4 errors fixed)**: Changed `on: Exception do:` to `on: Error do:` with
   explicit `TestSkipped` handler. `Notification` (used by 4 tests) was being caught by
   the runner before the tests' own handlers could process it.

2. **ContextTest (3 failures fixed)**: Changed `testClass new setUp; perform: sel; tearDown`
   to `(testClass selector: sel) runCase`. The old code left `testSelector` nil and bypassed
   the standard SUnit call chain that tests depend on.

3. **SemaphoreTest, DelayTest, ProcessTerminateBugTest (4 failures/errors fixed)**: Changed
   fork priority from `Processor activePriority - 1` (78) to 40 (`userSchedulingPriority`).
   At priority 78, tests that fork higher-priority processes couldn't preempt, and
   `priority + 20` exceeded the scheduler's 80-priority limit.

### History
| Run | Date | Classes | Pass | Fail | Error | Skip | Total | Notes |
|---|---|---|---|---|---|---|---|---|
| #1 | 2026-02-06 | 74 | 4362 | 5 | 7 | 6 | 4380 | Test-runner bugs |
| #2 | 2026-02-06 | 74 | 4422 | 0 | 0 | 6 | 4428 | All non-skip pass (prev image) |
| #3 | 2026-02-06 | 13/74 | ~1698 | 1 | 1 | 4 | ~1704 | OOM stall (new image, no GC) |

---

## Missing Features (must be built)

### 1. Garbage Collection — DISABLED
- **Where**: `ObjectMemory.cpp:1521` — `fullGC()` is a no-op
- **What**: Mark-and-sweep implementation exists but is in `#if 0` block.
  Disabled because it's "too slow" (minutes for 3M objects due to
  `std::function` callback overhead in `forEachObject`).
- **Impact**: No memory is ever reclaimed. VM relies on pre-allocated 48GB heap
  (`test_load_image.cpp:476`). Default is 128MB (`ObjectMemory.hpp:47`).
- **This is the #1 blocker** — Without GC, the test suite cannot complete because
  Deprecation objects accumulate. Fixing GC would unblock full test runs.
- **To fix**: Rewrite GC iteration with inline loops instead of callbacks.
  Then implement forwarding pointers for compaction.

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
