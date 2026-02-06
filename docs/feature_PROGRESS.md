# Progress: feature

Started: Thu Feb  5 16:02:01 EST 2026

## Status

IN_PROGRESS

## Task List
- [x] Task 1: Fix millisecond clock mask mismatch causing semaphore timer failures in long runs
- [x] Task 2: Document Float NaN skip finding (test explicitly calls `self skip`, not a VM bug)
- [ ] Task 3: Rebuild and rerun full test suite with testPrintingRecursive re-enabled
- [ ] Task 4: Analyze test results - identify any skipped/failing tests beyond known 32-bit skips

## Completed This Iteration
- Task 1: Fixed `primitiveMillisecondClock` mask from `0x1FFFFFFF` (29-bit) to `0x3FFFFFFF` (30-bit) to match `ioMSecs()` and timer comparison logic. The mismatch caused timer semaphores to fail after extended execution, breaking `waitTimeoutSeconds:` and causing test timeouts.
- Task 2: Investigated testNaNCompare - the test method starts with `self skip` unconditionally. This is identical behavior on both standard Pharo VM and our VM. Not a bug, just a disabled test in Pharo's test suite.
- Re-enabled `testPrintingRecursive` in the test runner since the semaphore fix should allow the timeout mechanism to work properly.

## Notes
- The mask mismatch: `primitiveMillisecondClock` (prim 135) used `0x1FFFFFFF` (29 bits) while `ioMSecs()`, timer comparison, and `nextWakeupTime_` all use `0x3FFFFFFF` (30 bits). When Smalltalk computed a target time from the 29-bit clock value and the real clock had 30-bit values, wrap-around arithmetic would fail.
- Float NaN: `FloatTest>>testNaNCompare` explicitly calls `self skip` before any test logic. The Pharo developers disabled this test intentionally. Our float primitives handle NaN correctly per IEEE 754.
- 4 TestSkipped errors are expected and match standard Pharo VM: 3x IntegerTest 32-bit-only tests + 1x FloatTest testNaNCompare
