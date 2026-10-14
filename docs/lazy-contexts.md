# Lazy outer contexts

Status: the default on all architectures since 2026-10-14.  Opt-out:
`PHARO_NO_LAZY_OUTER=1` (works on release builds).

## Problem

A non-clean block (a block that has `^` or that reads `self`, an instance
variable or `thisContext` through its outer context) needs an outer
context: a Context object for the activation that creates the block.

Before this change, `createFullBlockWithLiteral` called
`materializeFrameStack()` for each non-clean block made at frame depth
greater than 0.  That call does these steps:

1. It walks every saved frame.
2. It makes a Context for each frame that has none.
3. It links the senders.
4. It copies the temps and the operand stack of each unsynced frame
   into the Context of that frame.

The sender chain must be complete, because a Context's sender must be a
Context.  In the lexicon package (CodeAnalysisRunnerTest), the VM made
51 million of these calls.  Each call walked 16.6 frames.  The calls
used 10.9% of the samples.

## Design

Cog marries a Context to its frame and reads the frame when image code
reads the Context.  This VM does a smaller version of the same idea.

1. **The stub.**  `lazyOuterContext()` gives the new block the Context
   of the current frame and does not walk the frames:
   - If the frame already has a Context (`currentFrameMaterializedCtx_`),
     the block gets that Context.  The Context must belong to this
     activation (method, closure and receiver agree).  If it does not,
     the VM uses `materializeFrameStack()` as before.
   - Otherwise the VM allocates a Context with the size that
     `materializeFrameStack` uses.  It fills in method, closure,
     receiver, pc and the temps (arguments included), and sets stackp
     to the number of temps.  The sender stays nil.  The operand-stack
     slots stay nil.

   The flag `lazyCtxPending_` then records that some Context can have a
   nil sender or old contents.

2. **The sync.**  When the flag is set, a full `materializeFrameStack()`
   fills in the Context of every live frame and links the senders.  A
   complete walk clears the flag.  Each path that drops the frames of a
   process already calls `materializeFrameStack()` (thisContext,
   non-local return through the context chain, process switch, FFI
   callbacks, snapshot).  `terminateCurrentProcess` and the callback
   time-out also call it now.

3. **Mediation.**  Image code must not see a stub.  Thus each place
   where image code can read or write a Context field does the sync
   first.  It does this when the flag is set and the object is a
   Context:
   - the long-form receiver-variable bytecodes of the interpreter
     (`ExtPushRecvVar`, `ExtPopStoreRecv`, `ExtStoreRecv`) and the
     legacy `extendedPush`/`extendedStore`.  The compiler uses the long
     forms for Context fields.
   - primitives that read or write raw Context fields: 60, 61, 62, 72,
     73, 74, 76, 128, 132, 148, 168, 173, 174, 195, 196, 197, 210, 211,
     212, 213, 248 and 249 (`Interpreter::primTouchesContextFields`).
     The check is in `executePrimitiveInner`, in the method-cache hit
     path of `sendSelector`, and in `primitiveDoPrimitiveWithArgs`.
   - primitive 105 (`replaceFrom:to:with:startingAt:`) fails for a
     Context operand.  The Smalltalk fallback then copies with
     `Context>>at:`.
   - native primitive calls from compiled code (`mcfCallPrimitive`)
     fail for a Context operand.  The send then runs in the interpreter,
     where the registers are correct for the sync.
   - methods of Context or of its superclasses with fields
     (`Interpreter::isContextFieldMethod`): on arm64 under machine-code
     frames, T1 compiles them, and before a read of sender, pc or stackp,
     and before a field store, the code tests `lazyCtxPending_` and calls
     `pharo_mcf_ctx_sync` when it is set.  On x86 and in the stencil JIT,
     the JIT does not compile them, nor code that reads a field with the
     long form at an index from 0 to 15 (`SistaV1::readsContextFields`).
   - the trivial-method shortcuts (the interpreter getter and setter
     fast paths, and the T1 inline getter) do not classify long-form
     accessors at an index that the short form can reach.

4. **Dead frames.**  When a frame returns, its Context becomes dead
   (sender nil, pc nil).  A normal return did this before.  Now the
   frames that a non-local return unwinds also do it
   (`widowUnwoundContext`).  The native return of machine-code frames
   also does it, so a frame that made a block does not return through
   the interpreter.

## Results

Instructions retired, in billions (Apple M-series, arm64 build-rel):

| Workload | Before | After |
|---|---|---|
| lexicon CodeAnalysisRunnerTest | 1081 | 954 |
| 3M printString | 38.1 | 38.7 |

lexicon wall time on a quiet machine: 57.0 s before, about 50.6 s
after (time limit 50 s, Cog 11 s).  The machine-code-frame fixes that
followed (docs/mc-frames-design.md, exit census) took lexicon to 838
billion instructions.

Sweep L4 (`3c5cf644`, with the later machine-code-frame work): 27878 /
11 / 2, only a TraitTestCase timeout outside the baseline lists.

Validation: sweep L2 (`d90a1150`): 27875 pass, 13 fail, 1 error, against
27879 / 9 / 2 for the baseline.  The three non-passes outside the
baseline lists (StDebuggerTest testDynamicVariableEvaluation, TestCaseTest
testAnnouncement timeout, TraitTest timeout) pass alone 3 of 3.

## Behaviour changes

- A Context read through a mediated path now shows the state of the frame
  at the time of the read.  Before, it showed the state at the time the
  last block was made.  The new behaviour agrees with Cog.
- A Context of a frame unwound by a non-local return is now dead.
  Before, it kept a pc and a sender.  The new behaviour agrees with Cog.
- A Context of a frame that returned before any sync keeps the temps
  from the time the block was made.  Its operand stack is not saved.

## Known gaps

- The T1 `at:`, `at:put:` and `size` helpers for format-3 objects
  (`jit_rt_primat_ptr` and the other two) read raw slots.  A Context
  reaches them only through `Object>>basicAt:` and similar methods run
  on a Context with `withArgs:executeMethod:`, because Context
  overrides these selectors with primitives 210-212.
- The Sista tier (opt-in, off by default) can inline Context getters
  from interpreter hints.
- The non-MCF chain and resume loops do not sync `closure_` before a
  sync point.  That was true for block creation before this change.

## Debugging

- `PHARO_NO_LAZY_OUTER=1`: use the old full materialization (A/B test).
- `PHARO_MCF_NO_CTX_JIT=1` (diagnostic build): keep Context methods out
  of the JIT on arm64 too.
- `PHARO_THISCTX_CENSUS=1` (diagnostic build): count `thisContext` by
  method.  In lexicon, all collapses come from `Context>>resume:through:`
  (non-local returns through `ensure:`).
- `PHARO_EFC_CENSUS=1` (diagnostic build): count `executeFromContext` by
  caller.
- `PHARO_MATFS_CENSUS=1` (diagnostic build): count
  `materializeFrameStack` calls, frames walked, and calls by caller.
- `PHARO_MCF_NO_INLINE_WIDOW=1` (diagnostic build): the machine-code
  return falls back to the interpreter for a frame with a Context.
