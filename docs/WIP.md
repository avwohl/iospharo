# iOS Pharo VM — Work In Progress

## Current Status (2026-02-16, commit d82c201)

### Test Results — Full Batch Run

**Previous baseline (commit 2265fb6):** 10779/11732 pass (91.9%), 83 fail, 835 error, 21 skip, 14 timeout

**Current (commit d82c201):** **11639/12500 pass (93.1%)**, 56 fail, 777 error, 20 skip, 8 timeout

| Metric | Previous | Current | Delta |
|---|---|---|---|
| Total tests | 11,732 | 12,500 | +768 |
| Pass | 10,779 (91.9%) | 11,639 (93.1%) | **+860** |
| Fail | 83 | 56 | -27 |
| Error | 835 | 777 | -58 |
| Skip | 21 | 20 | -1 |
| Timeout | 14 | 8 | -6 |

41 commits since last baseline. 12 batches of 50 classes ran successfully.

### Error Breakdown by Category

| Category | Count | Root Cause |
|---|---|---|
| WeakKeyDictionaryTest | ~69 | Process switch corruption when forked; pass 100% synchronously |
| WeakIdentityKeyDictionaryTest | ~87 | Same as above |
| SystemEnvironmentTest | ~48 | `#>` DNU — SystemEnvironment doesn't implement comparison |
| PackageOnModelTest | ~18 | Package system class restructuring |
| PackageAndClassesTest | ~20 | Package system class restructuring |
| PackageAnnouncementsTest | ~8 | Package system class restructuring |
| ClassDescriptionProtocolsTest | ~28 | Protocol/package system issues |
| ClassTest | ~24 | Class modification primitives |
| ClassAnnotationTest + subtypes | ~30 | Annotation system, class creation in tests |
| FFICalloutAPITest | ~14 | FFI type resolution broken |
| FBDDecompilerTest | ~26 | Bytecode decompiler (test infra) |
| FinalizationRegistryTest | 3 TO | Finalization timing issues |
| MonitorTest | 2 TO | Monitor/semaphore timing |
| ContinuationTest | 1 TO | Continuation test infrastructure |
| IntegerTest | 7 F, 3 E | testFactorial, testLargeShift, testModulo |
| OC* (compiler tests) | ~15 | Compiler test infrastructure |
| Misc | ~60 | Various smaller categories |

### Fixes Since Last Baseline (41 commits)

**Process switching / scheduling:**
- `70fa3b9` Fix process switching temp corruption: sync C++→context (not reverse)
- `8150a5c` Fix primitiveYield: remove checkTimerSemaphore() call (was corrupting processes)
- `d82c201` Simplify test timeout: yield-loop time check
- `65bc5e1` Fix cannotReturn for blocks returning from dead methods
- `a7059dd` Fix BlockReturn bytecodes (5D/5E) for inlined blocks

**Primitives / arithmetic:**
- `712a05e` Fix comparison primitives for large integers + identity hash for immediates
- `17b84b0` Fix identityHashOf for SmallFloat64
- `dc2bc85` Fix long-form conditional jumps (0xEE/0xEF) mustBeBoolean enforcement
- `42ed810` Fix primitiveFindFirstInString
- `f783b8f` Fix bit-reverse and byte-swap for LargeInteger results
- `0c29aac` Implement ByteArray data access primitives 600-629
- `dddde65` Fix primitiveAt/primitiveAtPut: reject non-indexable objects

**Overflow / safety:**
- `51d47fd` Fix remaining fromSmallInteger overflows
- `2d738b5` Fix FFI callout address/time fromSmallInteger overflow
- `949e2b4` Fix executeFromContext: guard SmallInteger corruption in unrelocated fixup

**GC / finalization:**
- `70f6b3d` Fix signalFinalizationIfNeeded: standard semaphore signaling
- `1bf6287` Run weak collection tests at priority 40

### Key Findings This Session

1. **WeakKeyDictionary tests pass 100% synchronously**: 207/207 and 209/209 when run without forking
2. **Tests fail only when forked**: With `forkAt: 40` + yield loop, 34+ errors appeared
3. **After recent fixes, behavior changed**: testAdd now HANGS instead of erroring with "Instances of SmallInteger are not indexable" — the identity hash fix may have changed the failure mode from crash to infinite loop
4. **Minimal fork test**: 9/10 tests pass (including WeakKeyDictionary basics: at, sort, associations, copy). Only `testAdd` hangs
5. **No SmallInteger-at: diagnostic fired**: The `primitiveAt` on SmallInteger diagnostic added to this build never triggered — the original error cause may be resolved, but a new hang appeared

### What To Do Next (Priority Order)

1. **Fix WeakKeyDict forked test failures (~156 errors)**: The single biggest category. Tests pass synchronously but fail/hang when forked. Root cause is in process switching — likely in `materializeFrameStack` / `executeFromContext` roundtrip or GC interaction during process switch. This would eliminate ~20% of all remaining errors.

2. **Fix SystemEnvironment `#>` DNU (~48 errors)**: SystemEnvironment doesn't implement the `#>` comparison operator. Either implement it in the VM's method lookup or ensure the image-side Comparable protocol is working.

3. **Fix Package/Class system tests (~80+ errors)**: These are all related to class creation/modification in tests. Package system expects certain behaviors from `Smalltalk organization`, protocol management, etc. May need fixes to class creation primitives or package-related primitives.

4. **Fix IntegerTest failures (7 fail, 3 error)**: testFactorial (returns 94 instead of 94!), testLargeShift, testModulo. The factorial bug is likely a remaining conditional jump issue in the optimized algorithm.

5. **GUI menus**: After test suite is stable, focus on making menu actions work.

### Known Remaining Issues

**GUI Status:**
- Desktop renders, top menu draws, world menu draws
- Dragging startup window makes it disappear (window management issue)
- Menus don't execute actions (likely event handling / morphic issue)

### Architecture Notes

- C++ inline stack: `stackBase_` to `stackPointer_`, `framePointer_` for current frame
- Process switch: `materializeFrameStack()` saves C++ state → context object, `executeFromContext()` restores
- GC traces C++ stack via `forEachRoot()` which iterates `stackBase_..stackPointer_`
- Saved frames (inline calls) stored in `savedFrames_[]` array, materialized to context objects on switch
- Context layout: slot 0=sender, 1=pc, 2=stackp, 3=method, 4=closure, 5=receiver, 6+=temps+stack
