# WIP: JIT Loop Exit Bug Fix (Task #55)

**Date**: 2026-04-03
**Status**: Fix implemented, partially tested

## Bug Found

**Root cause of KeyNotFound during SessionManager startup with JIT enabled.**

The `tryJITActivation` loop in Interpreter.cpp had a `checkCountdown_ <= 0` break
at the TOP of the loop (line 9906). After a successful `tryResume` + `continue`,
the JIT's new exit reason in `state` was never processed — the break skipped
straight past the switch statement. This left `instructionPointer_` and
`stackPointer_` stale, corrupting interpreter state.

### How it manifested

Dictionary `scanFor:` with key=50 during SessionManager startup:
1. JIT runs scanFor:, exits for `size` primitive (ExitSendCached)
2. Primitive succeeds, resume at bcOff=2
3. `chargeJITBytecodes` pushes `checkCountdown_` below 0
4. `continue` goes to top of loop
5. `checkCountdown_ <= 0` → `break` → `return true`
6. State has unprocessed ExitReturn (scanFor: completed)
7. Frame not popped, IP/SP stale → execution corrupted
8. Interpreter sees wrong method ("header" instead of "scanFor:")
9. Eventually causes KeyNotFound for dictionary key 50

Keys 20 and 30 worked because their probe sequences were shorter,
consuming less of the countdown budget.

### Fix Applied (src/vm/Interpreter.cpp)

1. Removed the `if (checkCountdown_ <= 0) break;` from the top of the JIT chain loop
2. Added `if (checkCountdown_ <= 0) goto jit_loop_exit;` at each `continue` site
   (4 locations: after prim success resume, after activateMethod resume,
   after ExitBlockCreate resume, after ExitArrayCreate resume)
3. Added `jit_loop_exit:` label after the loop with a switch that properly
   handles each exit reason:
   - ExitReturn: popFrame + push returnValue
   - ExitSend/ExitSendCached/ExitArithOverflow: sync IP/SP, return false
   - ExitBlockCreate/ExitArrayCreate: sync IP/SP, return false
   - default (ExitNone): return true

### Test Results

- Build: compiles cleanly (19 pre-existing warnings)
- Runtime: VM gets past SessionManager startup and into world loop
- No KeyNotFound error observed
- Still seeing an error caught by handleError:log: during startup — needs investigation
- Test results file not yet produced (may need longer run or test runner issue)

### Diagnostic traces still in code

These should be cleaned up before final commit:
- Send trace (first 1000 sends) at sendSelector()
- JIT-EXIT trace (first 300 exits) at tryJITActivation
- JIT-RESUME trace (first 100 resumes) at ExitSendCached primitive handler
- JIT-RESUME-FAIL trace (first 20) at tryResume failure
- RESUME context chain logging at initialize()

### Next Steps

1. Run with longer timeout to see if tests execute
2. Investigate the handleError:log: error during startup
3. Check if `tryJITResumeInCaller` has the same pattern (it doesn't — its structure processes exits inline before continuing)
4. Clean up diagnostic traces
5. Run full test suite comparison (JIT on vs JIT off)
6. Commit the fix properly
