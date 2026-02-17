# iOS Pharo VM — Work In Progress

## Current Status (2026-02-17, commit 29dd7ea)

### Test Results — Full Batch Run (commit d82c201)

**11639/12500 pass (93.1%)**, 56 fail, 777 error, 20 skip, 8 timeout

| Metric | Count |
|---|---|
| Total tests | 12,500 |
| Pass | 11,639 (93.1%) |
| Fail | 56 |
| Error | 777 |
| Skip | 20 |
| Timeout | 8 |

### GUI Status

- Desktop renders correctly (Pharo world with morphs)
- Top menu bar visible and clickable, dropdowns open
- World menu opens on right-click
- Dragging startup window makes it disappear (window management issue)
- Menu actions don't execute (likely event handling / morphic issue)

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
| IntegerTest | 7 F, 3 E | testLargeShift, testModulo, etc. |
| OC* (compiler tests) | ~15 | Compiler test infrastructure |
| Misc | ~60 | Various smaller categories |

### What To Do Next (Priority Order)

1. **Fix WeakKeyDict forked test failures (~156 errors)**: The single biggest category.
   Tests pass synchronously but fail/hang when forked. Root cause is in process
   switching — likely `materializeFrameStack` / `executeFromContext` roundtrip or
   GC interaction during process switch. Would eliminate ~20% of remaining errors.

2. **Fix SystemEnvironment `#>` DNU (~48 errors)**: SystemEnvironment doesn't
   implement the `#>` comparison operator.

3. **Fix Package/Class system tests (~80+ errors)**: Related to class
   creation/modification in tests. Package system expects certain behaviors from
   `Smalltalk organization`, protocol management, etc.

4. **GUI menu actions**: Make menu items execute their actions.

### Architecture Notes

- C++ inline stack: `stackBase_` to `stackPointer_`, `framePointer_` for current frame
- Process switch: `materializeFrameStack()` saves C++ state → context object, `executeFromContext()` restores
- GC traces C++ stack via `forEachRoot()` which iterates `stackBase_..stackPointer_`
- Saved frames (inline calls) stored in `savedFrames_[]` array, materialized to context objects on switch
- Context layout: slot 0=sender, 1=pc, 2=stackp, 3=method, 4=closure, 5=receiver, 6+=temps+stack
- SDL2 stubs in FFI.cpp bridge between Pharo image's OSSDL2Driver and our Metal rendering pipeline
