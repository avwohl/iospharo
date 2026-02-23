# FFI Callbacks to Smalltalk

## What callbacks are

When C code needs to call back into Smalltalk — e.g., `qsort()` takes a
comparison function pointer, and you want that function to execute a Smalltalk
block — the VM needs to create a native "thunk" (a small machine code
trampoline) that C can call, which then re-enters the Smalltalk interpreter.

## Current state: partially implemented

### What works

1. **`primitiveRegisterCallback`** — uses `ffi_prep_closure_loc()` from libffi
   to allocate an executable thunk. Works on iOS too — libffi has a special
   trampoline pool mechanism for iOS's W^X restriction.

2. **Thunk allocation and C invocation** — C code successfully calls the thunk.

### What doesn't work

3. **`callbackClosureHandler`** (`Primitives.cpp:26992`) — when C calls back,
   this fires. Instead of re-entering the interpreter to run the Smalltalk
   block, it **queues the callback and returns 0 immediately**.

4. **`primitiveCallbackReturn`** (`Primitives.cpp:27188`) — no-op. In a real
   implementation it would use `longjmp` to resume the C call with the actual
   return value computed by the Smalltalk block.

## Why tests fail

- `testCqsort` passes a Smalltalk comparator block to C's `qsort()`. Our
  handler returns 0 for every comparison instead of the real result, so the
  array doesn't get sorted.
- `testIntegerParameters`, `testFloatParameters`, `testCharacterParameters`
  etc. fail because the callback always returns 0 instead of the computed value.
- `testPassingStructureInTheStack` and similar structure tests also return 0.

### Test results (both Mac Catalyst and iOS)

- FFICallbackTest: 0/2 pass (2 fail)
- FFICallbackParametersTest: 2/12 pass (9 fail, 1 skip)
- All other FFI tests pass: FFICalloutAPITest 18/18, FFICalloutTest 6/6,
  FFICompilerPluginTest 5/5

## Not an iOS limitation

This is **not** an iOS-specific limitation. The thunk allocation works on iOS
(libffi handles W^X with a pre-allocated trampoline page). The problem is our
interpreter doesn't support **re-entering interpretation from inside a C call**.

The standard Pharo VM (Cog/JIT) handles this with its own callback mechanism
built into the JIT compiler.

## Implementation options

To make callbacks fully work, our interpreter-only VM needs one of:

1. **setjmp/longjmp approach** — save the C stack state, run the Smalltalk
   block in the interpreter, then longjmp back to resume C with the return
   value. This is the most straightforward approach but requires careful stack
   management.

2. **Second interpreter thread** — run the callback block on a separate
   interpreter thread while the FFI call blocks waiting for the result. More
   complex but avoids longjmp.

3. **Cooperative coroutine** — use `ucontext` or platform-specific fiber APIs
   to switch between the C call context and the interpreter context.

All are doable but non-trivial. The `TODO` comment at `Primitives.cpp:27008`
acknowledges this gap.

## Affected functionality

Most Pharo image functionality does **not** use FFI callbacks. The main use
cases are:

- Sorting with custom comparators via C `qsort` (rare — Pharo usually sorts
  in Smalltalk)
- ObjC/Cocoa callback handlers (menu actions, delegate methods)
- C library callbacks (e.g., libgit2 progress callbacks)

Menu handling works despite incomplete callbacks because the handler returns 0
(void), which is the correct return for ObjC action methods.
