# WIP: JIT Bugs During Session Startup

**Date**: 2026-04-03
**Status**: All three bugs found and fixed

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

## Bug #3: Super Send Megacache Conflation (FIXED - commit 54fd5af)

After fixing bugs #1 and #2, session startup progressed further but hit
infinite recursion in `#initialize` (same method calling itself 4000+
times). The bytecodes of the recursive method:
```
4c eb 00 d8 4e cf 58  (push self; super initialize; pop; push false;
                        store instVar7; return self)
```

**Root cause**: The JIT compiler upgraded super sends (0xEB) to
`stencil_sendPoly`, which uses a global megamorphic cache. The megacache
key is `(selectorBits, classIndex)` and does NOT distinguish normal sends
from super sends. A prior normal send of `#initialize` populated the
megacache with `(#initialize, classIndex -> same #initialize method)`.
When the JIT hit `super initialize`, the megacache returned the SAME
method instead of the superclass's method, causing infinite recursion.

**Fix**: Exclude `ExtSuperSend` (0xEB) from the `stencil_sendPoly`
upgrade. Super sends stay as `stencil_send` (deopt to interpreter),
which does the correct super-class-based lookup. Also added
`patchJITICAfterSend` to the 0xEB interpreter handler to prevent
IC patch pointer leakage.

## Next Steps

1. Remove diagnostic traces (sendMustBeBoolean 3-entry log can stay)
2. Run JIT-on test suite and compare to JIT-off baseline
3. Record both wall clock time and CPU time per user request
