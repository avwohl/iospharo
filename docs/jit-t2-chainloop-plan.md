# T2 Chain-Loop Continuation — Design

**Status:** Design-only; not implemented in session 23.

## Goal

Restore T2's callee-invocation speedup (lost in fix `f279fd4`) while
preserving the correctness guarantee. Current T2 always bails on sends
because the callee may do partial side effects before bailing, and the
interpreter can't safely re-activate.

## Design

When `jit_t2_send`'s callee bails with non-`ExitReturn`:

1. **Don't restore caller state.** The callee's `JITState`
   (`sp`, `receiver`, `tempBase`, `ip`, `method`) is the source of truth
   for where execution stands.

2. **Push a `SavedFrame` for the T2 caller.** Snapshot enough to
   restore the caller when the callee's top-level `returnReceiver`
   eventually fires. Fields to save (parallels J1's chain loop):
   - `savedIP`   = caller's resume IP (the send bytecode + opcode size)
   - `savedMethod` = caller's CompiledMethod
   - `savedReceiver` = caller's receiver
   - `savedFP` = caller's tempBase - 1
   - `savedArgCount` = caller's argCount
   - `savedClosure` = nil (T2 doesn't touch closure yet)
   - `savedActiveContext` = nil (lazy materialization)

3. **Set `state.exitReason` to a new value, `ExitResumeCallee`.**
   The interpreter's dispatch loop recognizes this and, instead of
   re-activating a callee, continues at `state.ip` (which is the
   callee's current partial position).

4. **Update interpreter fields** (`method_`, `ip_`, `framePointer_`,
   etc.) from the callee's `JITState` before dropping into the main
   dispatch. This mirrors the `j2jMaterialized` code path
   (Interpreter.cpp:12898-12914).

5. **On the callee's final `returnReceiver`:** the interpreter's
   existing `normalReturn` path already pops `SavedFrames` and resumes
   the caller. Nothing new needed.

## Compared to T1's J2J chain

T1 already does this pattern via `ExitSendCached` + chain-loop
materialization. T2 needs to hook into the same materialization path
but with T2-specific frame metadata.

## Expected performance

- T2 inline arith fast path (current): unchanged
- T2 callee send that COMPLETES with ExitReturn: unchanged
- T2 callee send that BAILS (currently ALWAYS bails in the safe fix):
  was ~1 interpreter-activation per bail; becomes 1 SavedFrame push +
  interpreter continuation. Should match T1's cost on the same send.

Target: T2 on send-heavy AWFY moves from "slightly slower than T1"
back to "at least equal, potentially faster via arith fast path
compounding."

## Files touched (estimated 4)

1. `src/vm/jit/JITRuntime.cpp` — `jit_t2_send` fallback path
2. `src/vm/Interpreter.cpp` — new `ExitResumeCallee` handler +
   SavedFrame push helper
3. `src/vm/Interpreter.hpp` — helper declarations
4. `src/vm/jit/JITState.hpp` — new `ExitReason` enum value

## Gotchas

- The callee might have pushed temps beyond its receiver/args window
  (stack layout between caller's flushed args and callee's locals is
  continuous). Interpreter's frame conventions assume `framePointer_`
  points to the SAVED FP slot. Verify this matches `state.tempBase - 1`
  convention T2 uses.
- GC roots: during interpreter takeover, `savedFrames_[]` and
  `activeContext_` are the GC roots. The SavedFrame push must include
  all oop fields the caller holds.
- Nested T2 bails: if a T2 callee calls another T2 that also bails,
  we'd need to unwind multiple SavedFrame pushes. The materialization
  code handles this for T1 (`j2jDepth` counts).

## Estimated effort

Probably 1-2 days:
- Day 1: hook jit_t2_send into SavedFrame push, add ExitResumeCallee,
  get Permute returning true with callee running partial work.
- Day 2: iron out edge cases (nested bails, non-local returns, GC
  triggered mid-callee).

## Why deferred in session 23

The f279fd4 correctness fix already prevents the bug. T2 currently
matches T1 performance, which is good enough to not block any
feature. Perf recovery via chain-loop is a pure optimization.
Implementing it carefully needs a longer uninterrupted session.
