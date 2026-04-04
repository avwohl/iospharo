# WIP: JIT Bugs During Session Startup

**Date**: 2026-04-03
**Status**: Two bugs found, first two fixed, third under investigation

## Bug #1: JIT Loop Exit (FIXED - commit 850b443)

The `tryJITActivation` loop broke before processing exit reasons when
`checkCountdown_ <= 0`. Fix: process exit reason at each `continue` site
via `goto jit_loop_exit` label.

## Bug #2: JIT state.ip Offset (FIXED - commit 6376387)

`tryJITActivation` set `state.ip = instructionPointer_`, but for methods
with `callPrimitive` (0xF8), `activateMethod` already advances IP by 3
bytes past the callPrimitive. JIT stencils compute exit IPs as
`state.ip + bcOffset` (where bcOffset is from bytecodeStart). This double-
counted the callPrimitive skip, causing IP to overshoot.

**Symptom**: `on:do:` blocks (primitive 199) had IP past their byteSize.
The interpreter's dispatch loop `IP >= bytecodeEnd_` check returned the
receiver (a Context) as the method's return value. This Context appeared
where a Boolean was expected, triggering `mustBeBoolean`. Every session
startup handler using `on:do:` for error recovery silently failed, so
the test runner at priority 90 never executed.

**Fix**: Compute `bytecodeStart` from method header instead of using
`instructionPointer_`.

## Bug #3: Infinite #initialize Recursion (INVESTIGATING)

After fixing bug #2, session startup progresses further but hits infinite
recursion in `#initialize` (same method 0x3002e13a0 calling itself 4000+
times). This occurs during `startUp` → block → `forContext:priority:` →
`new` → `initialize` → `initialize` → ...

This is likely another JIT bug where a send within `#initialize` is
incorrectly dispatching back to `#initialize` instead of to the intended
target. Possibly related to the JIT IC (inline cache) returning the wrong
cached method for a send inside `#initialize`.

### Next Steps

1. Identify which send inside `#initialize` is misdispatching
2. Check if the JIT IC has a stale or wrong entry
3. Fix the root cause
4. Then run JIT-on test suite and compare to JIT-off baseline
