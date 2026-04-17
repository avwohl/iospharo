# T2 Chain-Loop Continuation — Refined Design

**Status:** Design-only; not implemented. Commit f279fd4 (T2 always-bail)
remains in place for correctness. This doc updated session post-A2.

## Goal

Restore T2's callee-invocation speedup (lost in f279fd4) while keeping
the correctness guarantee. Current T2 always bails on sends because the
callee may do partial side effects before its own inner send bails, and
the interpreter can't safely re-activate the whole call chain.

## Core idea

When `jit_t2_send`'s callee bails with non-`ExitReturn`:
1. **Don't restore caller state.** Leave callee's `JITState` intact.
2. **Push a `SavedFrame`** for the T2 caller onto `savedFrames_`.
3. **Set a new `ExitReason` — `ExitT2CalleeContinue`** — that tells the
   interpreter "state is a callee; dispatch from state.ip without
   re-activating anything."
4. **Interpreter resumes at callee's current bytecode.** When the callee
   eventually `returnReceiver`s, the existing `normalReturn` path pops
   the SavedFrame and restores the caller (also in interpreter mode).

## Key simplifications discovered

- **The T1 J2J chain loop already does something similar**: T1 stencils
  push saves onto `j2jSaveCursor`, the chain loop materializes them to
  `savedFrames_` on bail. We don't have to build that machinery from
  scratch; we can co-opt the same materialization path.
- **After A2 (commit 415d899)**, `save.jitMethod` is the only piece
  needed to derive `literals`/`argCount`/`bcStart`. So a T2-pushed
  J2J save with just `jitMethod`, `sp`, `receiver`, `tempBase`, `ip`,
  `sendArgCount`, `resumeAddr` is all we need — exactly matches the
  existing 56-byte `J2JSave` struct.
- **Resume address:** T2 methods use a single entry point + dispatch
  table keyed on `state.ip`. So `resumeAddr` for a T2 save = the T2
  method's `codeStart()` — the dispatch table handles the rest.

## Cleanest implementation plan

### Step 1: make jit_t2_send push onto the J2J save stack

Currently `jit_t2_send` sets `j2jSaveCursor = nullptr` before calling
the callee (disabling the T1 stencil J2J chain). Instead:

- On entry: push a caller-save J2J entry using the SAME format as T1's
  stencil_sendJ2J does (sp, receiver, tempBase, ip, jitMethod,
  sendArgCount, resumeAddr = T2 entry).
- Leave `j2jSaveCursor` pointing PAST that new entry so the callee can
  push further.
- On successful callee return (`ExitReturn`): pop our pushed entry,
  restore sp+retval as before.
- On bail: DO NOT pop our entry. DO NOT restore caller. Just return
  with `state.exitReason = ExitSend`. The chain loop will see
  `state.j2jDepth > 0`, materialize our entry (and any deeper ones
  the callee pushed) into `savedFrames_`, and continue the callee in
  interpreter.

### Step 2: handle materialization of T2-style saves

The existing materialization code (`src/vm/Interpreter.cpp` has 3-4
sites) reads `save.jitMethod` and derives everything. A T2-pushed save
has the same shape, so nothing changes.

### Step 3: interpreter continues from callee's state

On `ExitSend` with `j2jMaterialized`, the interpreter already syncs
`method_`/`ip_`/`framePointer_`/`receiver_` from state and continues
dispatch. Nothing new needed.

### Step 4: ensure resume doesn't re-execute the send

The callee's current `state.ip` is at the SEND BYTECODE that bailed.
Interpreter will dispatch that bytecode normally — so the send
happens exactly once (in interpreter), not duplicated.

## Expected performance

- Successful T2-T2 callee return: same as today (ExitReturn fast path).
- T2-T1 callee that completes: same as today.
- T2 callee that bails after partial work: goes from "re-activate
  everything and double-count" (current bug, prevented by always-bail)
  to "materialize one SavedFrame, continue in interpreter" (fast and
  correct).

For send-heavy AWFY (Bounce/Permute/Queens), this removes the primary
reason T2 = T1 performance. Target: T2 on these benchmarks matches or
beats T1.

## Files touched (estimate 3, ~200 lines)

1. `src/vm/jit/JITRuntime.cpp` — `jit_t2_send`: push J2J save at entry,
   pop on fast-path return, skip pop on bail.
2. `src/vm/Interpreter.cpp` — verify the existing chain-loop
   materialization handles a T2-style save correctly (likely yes, it
   already uses `save.jitMethod` and derives).
3. `src/vm/jit/JITState.hpp` — probably no changes (can reuse ExitSend).

## Gotchas

- **j2jSaveCursor ownership:** T2 needs a place to write its save. The
  existing `j2jPool_` in `Interpreter` is sized for T1 J2J chains; T2
  can use the same pool. Just don't set cursor to nullptr.
- **GC roots:** SavedFrames_ is a GC root. J2JSave pushed by
  jit_t2_send must have all oop fields (receiver, jitMethod→
  compiledMethod) GC-reachable. Already handled via the J2JSave struct.
- **Yield / preemption during callee:** callee might yield mid-bail.
  The saved frame is already in the pool so it survives.
- **Non-local returns from callee:** if the callee does a block return
  up past the T2 caller, the saved frame needs to be unwound too.
  The existing `returnFromMethod` + `popFrame` handles SavedFrame
  stack already — should just work if we use the same
  mechanism.

## Estimated effort

- Implementation: 4-6 hours with testing.
- Verification: full AWFY pass to confirm no regressions.
- Highest risk: the T2 caller's `state.ip` needs to be set correctly
  so that on return from callee, the caller resumes at post-send.
  `jit_t2_send` already sets `state.ip = bcBase + bcOffset` via
  emitSendCall; the caller's resumeAddr = T2 entry uses state.ip to
  dispatch to the matching resume label.

## Why still deferred

This session did A3 (diag) + A2 (J2J shrink) + wanted to do this too,
but A1 needs a real code change across jit_t2_send + verification, and
the time box was used up on A2. The plan is now concrete enough that a
future session can pick it up directly.
