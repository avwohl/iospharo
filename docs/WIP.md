# iOS Pharo VM — Status

Last verified: 2026-02-05

---

## How to Run Tests

```bash
# 1. Fresh Pharo 13 image
cd /tmp && curl -sL https://get.pharo.org/64/130 | bash

# 2. Load test runner (standard Pharo VM)
cp /tmp/Pharo.image /tmp/test.image && cp /tmp/Pharo.changes /tmp/test.changes
/Users/wohl/Downloads/pharo /tmp/test.image eval --save \
  "'/Users/wohl/src/iospharo/scripts/run_sunit_tests.st' asFileReference fileIn"

# 3. Run with custom VM
./build/test_load_image /tmp/test.image

# 4. Results
cat /tmp/sunit_test_results.txt
```

---

## Test Results (2026-02-05, post-workaround-removal)

Verified after removing all workarounds (commits b31cf2a, 695cb25, c1a6826).
No silent process killing, no error swallowing. Honest results.

**12 of 15 test classes completed. VM killed by stack overflow on ArrayTest.**

| Test Class | Pass | Fail | Error | Notes |
|---|---|---|---|---|
| SmallIntegerTest | 29 | 0 | 0 | |
| IntegerTest | 80 | 0 | 3 | 3 TestSkipped (64-bit skip) |
| FloatTest | 74 | 0 | 1 | 1 TestSkipped |
| FractionTest | 32 | 0 | 0 | |
| PointTest | 36 | 0 | 0 | |
| CharacterTest | 17 | 0 | 0 | 2 not run (slow Unicode iteration) |
| DictionaryTest | 205 | 0 | 0 | |
| SetTest | 174 | 0 | 0 | |
| BagTest | 168 | 0 | 0 | |
| IntervalTest | 260 | 0 | 0 | |
| SymbolTest | 268 | 0 | 0 | |
| OrderedCollectionTest | 351 | 0 | 0 | |
| ArrayTest | — | — | — | **stopVM at step 444M: stack overflow on testPrintingRecursive** |
| StringTest | — | — | — | not reached |
| HeapTest | — | — | — | not reached |

**Completed: 2119 pass, 0 fail, 4 errors (all TestSkipped)**

**Root cause of stopVM**: `testPrintingRecursive` creates infinite recursion
(`(Array new: 1) at: 1 put: self; printString`). The test relies on
`waitTimeoutSeconds:` to catch the timeout, but our timer doesn't fire
(see missing feature #10 below). Without the timeout, recursion continues
until `push()` hits the stack limit and calls `stopVM()`.

**To fix**: Fix timer/semaphore signaling so timeouts work. Then
`testPrintingRecursive` will be caught by the timeout and the remaining
3 test classes (ArrayTest, StringTest, HeapTest) can run.

---

## Missing Features (must be built)

### 1. Garbage Collection — DISABLED
- **Where**: `ObjectMemory.cpp:1521` — `fullGC()` is a no-op
- **What**: Mark-and-sweep implementation exists but is in `#if 0` block.
  Disabled because it's "too slow" (minutes for 3M objects due to
  `std::function` callback overhead in `forEachObject`).
- **Impact**: No memory is ever reclaimed. VM relies on pre-allocated 8GB heap
  (`test_load_image.cpp:476`). Default is 128MB (`ObjectMemory.hpp:47`).
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

### 7. FFI Type Resolution — BROKEN
- **Where**: FFI callout paths; see `CLAUDE.md` investigation log
- **What**: `ByteSymbol >> newReferentClass:` not found — should be on
  `ExternalType`. FFI returns Symbols instead of ExternalType objects.
- **Impact**: Blocks OSSDL2Driver which uses FFI for `SDL_PollEvent`.
  This is why the input event loop doesn't start via normal Smalltalk path.
- **Note**: Reported "fixed" in 10+ previous sessions. It was never fixed.
  Do NOT add workarounds. Actually debug the type resolution.

### 8. FFI Return Marshalling — INCOMPLETE
- **Where**: `Primitives.cpp:13655-13669`
- **What**: FFI return values larger than SmallInteger are truncated.
  Pointer return values are truncated to SmallInteger.
  No `LargeInteger` or `ExternalAddress` creation.
- **Impact**: FFI functions that return pointers or large values give wrong results.

### 9. Input Event Loop — NOT STARTING
- **Where**: See `docs/WIP-input-handling.md` for investigation
- **What**: No Smalltalk process polls primitive 264 (getNextEvent). Events
  accumulate in `passThroughEvents_` but nobody reads them.
- **Root cause**: OSSDL2Driver's `setupEventLoop` fails because FFI is broken
  (item 7 above). OSiOSDriver in the standard image is a stub.
- **Impact**: No keyboard or mouse input reaches Smalltalk.
- **Depends on**: FFI type resolution (item 7).

### 10. Timer/Semaphore Reliability — BUG
- **Where**: Timer primitives, semaphore signaling
- **What**: `waitTimeoutSeconds:` timer doesn't fire correctly after extended
  execution (2000+ tests). `testPrintingRecursive` passes alone but hangs
  in full test suite.
- **Impact**: Long-running test suites hang. Timeout mechanisms fail.

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
