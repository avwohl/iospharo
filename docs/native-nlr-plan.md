# Non-local return in machine code: plan

Status (2026-10-15): from a design panel of three designs and a judge
that checked each claim against the code.  Steps 1-5 are done
(lexicon 272.6 -> 270.3 billion instructions).  As built, the stub
address is a 64-bit immediate at each site: JITState offset 488 is not
free (machine code reads interpreter fields at x0 + 488 and up, because
the state is embedded in the Interpreter object).  The other steps are
open.

The problem: a `^` in a block exits machine code to the interpreter.
Lexicon does this about 1.3M times per run: about 892K with no ensure:
frame between the block and its home (OCProgramNode>>errorNotices,
`ifEmpty: [^ #()]`), and about 408K through `critical:` (Symbol
class>>intern:), which collapses the whole frame stack into contexts.
An lldb single-step count of one errorNotices NLR: about 1,700
instructions in C++.

## Verdict on each design

### Design 1, "helper" (pharo_mcf_nlr)

Scores (1 to 5): correctness 5, gain 3, testability 5.

Claims the code confirms:
- **Block return exit.** The exit is at `src/vm/jit/asmjit/AsmjitT1.cpp:7406` (`g_emitIsBlock && SistaV1::isReturn(op)`). It stores ip, then `EXIT_ARITH_OVERFLOW` (reason 6), then syncs sp.
- **Free stub slot.** `mcfBlockCreateStubAddr` is the last field of JITState, at offset 480 (`JITState.hpp:248`), so offset 488 is free. The trampolines hard-code only offsets 344 and 392 (`TrampolineAsm.S:638`, `JITState.hpp:448/496`).
- **Interpreter path.** `returnFromMethodSlow` (`Interpreter.cpp:9912`) validates the home with `validateNLRHomeFrame` (`:9851`). It then calls `nlrDelegateToImageProtocol`, which scans records `[h, d)` for primitive 198 (`:10945-10956`). Then it runs the `widowUnwoundContext` + `popFrame` loop and `returnValue`. A helper that copies this sequence adds no new semantics.
- **popFrame.** `popFrame` (`:17113`) fails only at depth 0. It scrubs `homeFrameDepth` (`:17333`) and decrements `materializedFrameCount_` (`:17337`).
- **Precedent for a frame-changing helper.** `mcfThisContext` (`MachineCodeFrames.cpp:109`) already changes frame state from inside machine code.
- **Deopt answer.** The ExitDeopt answer (`MachineCodeFrames.cpp:2004`) is a valid way out.

**B1, a memo-loss bug: CONFIRMED.** This is the most valuable finding of the three designs.
- `popFrame` sets `frame.mcReturn = nullptr` on the popped record (`Interpreter.cpp:17318`, added by `05c84ed4` today). `popFrameForJIT` does the same at `:17466`.
- `returnValue` then calls `mcfResumeAfterReturn(&savedFrames_[frameDepth_])` (`:9174`). `mcfRun`'s ExitReturn then reads `popped.mcReturn` (`MachineCodeFrames.cpp:~2153`).
- Both readers now always see null. So every return into machine code through the interpreter pays a `methodMap().lookup` plus `mcfIsMethodFrame` / `mcfReceiverOk` / `mcfEntryFor`.

**B1b: CONFIRMED.** After `tryRestoreCollapsedFrames` succeeds inside `mcfRun`'s depth-0 ExitReturn, the code does not continue in machine code.
- `mcfResumeAfterReturn` returns at once because `mcfActive_` is set (`MachineCodeFrames.cpp:1707`).
- `mcfReturnedIntoContext_` is false, so `mcfRun` returns, and frame b runs interpreted until its next send.

**Overstated claim.** Design 1 says "(b) cannot avoid the full collapse without married contexts". The global-flag argument is correct, and it defeats design 2's case (b). Design 3 gets around the flag with a split flag plus a marker rule, so the claim is too strong.

**B2 (calling `nlrDelegateToImageProtocol` from inside a helper):** low gain (about 1K of the about 40K instructions per event). Risk: the send runs from a C++ helper while machine code is suspended. Keep B2 optional and last.

### Design 2, "inline-cutback" (assembly stub)

Scores: case (a) correctness 4, gain 4, testability 4. Case (b) correctness 2, gain 1 as written.

Case (a) checks against the code:
- **Correct claims:**
  - The record semantics: `savedFrames_[k]` is frame k suspended, and the memo is `savedFrames_[H-1].mcReturn`.
  - The result goes into the home's receiver slot at `savedFrames_[H].savedFP`.
  - The tail register contract matches `emitMcfReturn_arm64` (`AsmjitT1.cpp:5173-5326`): x3, x5, x6, x7, x9, x11, x12, x22.
  - hasPrimitive is raw bit 19 and numLits is raw bits 3-17 (`Oop.hpp:204`, `primitiveIndexOf` at `Interpreter.cpp:19404`).
  - Lazy context stubs fill slot 4 (closure) (`lazyOuterContext`, `:21456`).
  - The block stub stores `OFF_CLOSURE` (`AsmjitT1.cpp:5823`).
  - The native return does not scrub `homeFrameDepth`.
  - The `widowInline` condition (`:5186`) is as stated.
- **Small bugs to fix if the stub is built:**
  - `ldrh` of the primitive index must be masked with `0x1FFF`. The high byte holds flag bits (see `primitiveIndexOf`).
  - The walk out to the home context must refuse a nil `outerContext`.
- **Extra protection:** the context-identity check is stronger than `validateNLRHomeFrame`, which compares only the method. The check refuses more, but it never accepts a wrong home.

**Case (b) is FLAWED.** The safety net sets `lazyCtxPending_ = true`, and mediation fires on "flag set and the object is any Context" (`Interpreter.hpp:4090`, the hook at `AsmjitT1.cpp:14507`, primitives 195/196 in `kCtxPrimBits`). The protocol reads Context fields at once (`isDead` / pc, primitives 195/196, `thisContext`). So the floor thaws on almost every NLR and saves nothing.

### Design 3, "ensure-protocol" (frozen segment)

Scores: correctness 4 (complex), gain 4 (unproven), testability 4.

Claims the code confirms:
- **Today's case (b) cost.** `materializeFrameStack` walks every record, though synced records only relink their sender (`:21965`). `captureCollapsedFrames` copies records 0..h (`:10851`). `tryRestoreCollapsedFrames` (`:10892`) checks every record and copies every frame's temps and stack back.
- **Scavenge failure.** The restore fails on any epoch bump (restoreFail 2). `g_objectMotionEpoch++` runs on scavenges and full GCs (`ObjectMemory.cpp:2667`, `:3760`) and also at six other sites.
- **GC root scan.** The operand-stack root scan starts at `stackBase_` (`Interpreter.hpp:4306`). This is why raising `stackBase_` needs a new root loop.
- **Single writer.** The lazy flag has one machine-code writer, the block-create stub (`AsmjitT1.cpp:6691-6693`).
- **x24 reload.** x24 is reloaded from offset 344 at every entry.
- **Guard record.** `mcfGuardFrame_` sits right before `savedFrames_` (`Interpreter.hpp:899`).

The protocol analysis (Ctx(b) receives `resume:through:`; primitives 195/196 stop at their stop or target context) matches Pharo 13's `Context>>resume:through:`.

Risks:
- About 240 `savedFrames_` uses go through the new view: 189 in `Interpreter.cpp`, 21 in `Primitives.cpp`, 20 in `MachineCodeFrames.cpp`, 12 in `Interpreter.hpp`. There are also about 70 `stackBase_` uses.
- A missed mediation path would show the marker to image code.
- The gain depends on the frozen depth, and Step 0 must measure that depth first.

## Synthesized plan

Every knob goes in `src/vm/debug_vars.h`. Opt-outs are `DEBUG_BOOL_RT`, so release builds can compare arms. Oracles and censuses are plain `DEBUG_BOOL`.

"L" means lexicon instructions retired with cbench: 5 interleaved runs per arm, plus the `[JIT] Stats` line. "P" means the probe suite: md5 against `PHARO_NO_JIT`, under `PHARO_DET_SCHED=1`, on build-dbg with `PHARO_MCF_VERIFY=1`.

**Step 0: measure (no behaviour change)**
- Run lexicon on a `-DPHARO_DIAG_KNOBS=ON` tree with `PHARO_MCF_CENSUS`, `PHARO_NLR_CENSUS`, `PHARO_FRAME_RESTORE_CENSUS` and `PHARO_MATFS_CENSUS`.
- Record:
  - the reason=6 count;
  - avgDepth and avgHops for intern: (frozen depth = avgDepth - avgHops);
  - the restoreFail histogram (code 2 = a scavenge killed the restore);
  - walked and synced frames per materialize call.
- Take an lldb single-step histogram of one errorNotices NLR and one intern: NLR.
- A/B `PHARO_NO_FRAME_RESTORE=1` on build-rel.
- Write the numbers in `docs/WIP.md`.
- Decision gate: if the frozen depth is below about 20, the segment work (Steps 6-9) drops to about 1% gain. Then do Step 5 only.

**Step 1: keep the return memo across popFrame (design 1, B1a) -- DONE `19a730c0`**
- Files: `Interpreter.cpp` `returnValue` (`:8970-9174`) and `MachineCodeFrames.cpp` `mcfRun` ExitReturn (`~2150`).
- Change: read `savedFrames_[frameDepth_-1].mcReturn` and `mcJM` before `popFrame`, and pass them to `mcfResumeAfterReturn`. Change the `mcfResumeAfterReturn` signature to take the memo, the JM and the key ip. Keep every push-site clear and the clear in `popFrame`.
- Knob: `PHARO_MCF_NO_POP_MEMO_KEEP`.
- Expected L: -0.1 to -0.3% (more returns reach this path than the NLR ones).
- Proof:
  - `PHARO_MCF_VERIFY`'s `mcfVerifyMemo` stays silent in P;
  - `PHARO_MCF_COUNT_CALLS` shows memo hits above zero, where today the count is about 0;
  - build-x86 smoke run.

**Step 2: skeleton helper and stub for case (a), refusing only -- DONE**
- Files:
  - `JITState.hpp`: `mcfNlrStubAddr` at offset 488, with a static_assert;
  - `AsmjitT1.cpp`: `OFF_MCF_NLR_STUB`, `emitMcfNlrStub` in `buildMcfStubPage` / `mcfBuildStubs` (`~16448`), and the 4-instruction site at `:7406`;
  - `MachineCodeFrames.cpp`: `Interpreter::mcfNonLocalReturn`, next to `mcfThisContext`.
- The helper does design 1's Phase A only and always answers 0.
- Add design 2's context-identity check to Phase A: walk `outerContext` / closureOrNil out to the method context, refuse on nil, and compare with `savedFrames_[h].materializedContext`.
- Knobs: `PHARO_MCF_NO_NATIVE_NLR` (with the knob set, emission is byte-identical to today; check with `PHARO_T1_DUMP_SEL`) and `PHARO_MCF_NLR_CENSUS` (a refusal-reason histogram).
- Expected L: about 0.
- Proof: on lexicon the census shows about 892K "ok" and about 408K "prim198", and the reason=6 exit count is unchanged.

**Step 3: commit, then deopt -- DONE**
- Phase B: `mcfSyncRegisters`, pop the value, read the memo of `savedFrames_[h-1]` BEFORE the pops, run the `widowUnwoundContext` + `popFrame` loop, then the `returnValue` frame tail. The helper then answers 1 (ExitDeopt).
- Knobs: `PHARO_MCF_NLR_NO_CONT` and the oracle `PHARO_MCF_NLR_VERIFY`.
- The oracle checks:
  - h agrees with `validateNLRHomeFrame` and `findBlockHomeFrame`;
  - afterwards, depth == h-1, the result is at the old home fp, and the home context has nil pc.
- Prove the oracle by sabotage twice: once skip the home widow, once pop to h+1.
- Probes:
  - `ifEmpty: [^x]`;
  - a ^ through 1, 2, 5 and 50 frames;
  - a nested-block ^;
  - a recursive home;
  - a dead home (BlockCannotReturn);
  - a home at depth 0 after thisContext;
  - a ^ through `on:do:`;
  - a held intermediate context that must answer `isDead`;
  - `[^7] ensure: [2]`, which must still send aboutToReturn: once.
- Run ExceptionTest, ContextTest, BlockClosureTest, StepOverTest, StepIntoTest, ProcessTest, MutexTest and SemaphoreTest twice each.

**Step 4: native continuation -- DONE**
- Phase C publishes the view (`sp`, `receiver`, `closure`, `tempBase`, `jitMethod`; in the non-lazy view also method, literals, argCount and `mcfBcEndOff`) and answers the memo.
- When the memo is null, fall back to `mcfEntryFor`; when that also fails, deopt.
- The stub reloads x19, x25, x22, x21 and x26.
- Expected L: about -0.3 to -0.5%, with about 892K fewer reason=6 exits.
- Proof:
  - the full 565-class sweep with the fake-GUI prep, against a `PHARO_MCF_NO_NATIVE_NLR=1` arm;
  - `classify-sunit.py` on `LC_ALL=C`-sorted lists;
  - a class that differs is re-run 3 times.
- Then amend INV-2 in `docs/mc-frames-design.md` and commit.

**Step 5: native continuation after a collapsed restore (design 1, B1b) -- DONE**
- File: `MachineCodeFrames.cpp`, the depth-0 ExitReturn branch (`~2128`).
- Change: when `tryRestoreCollapsedFrames` succeeded (frameDepth_ > 0 afterwards) and the record of frame b had a memo, `continue` into that memo. Step 1 must land first, or the memo is already null.
- Knob: `PHARO_MCF_NO_RESTORE_CONT`.
- Expected L: -0.1 to -0.3%.
- Proof: the census for "reason=1 op=216 in #resume:through:" shows continuations, the `dacc0400` restore probes pass, and a sweep is clean.

**Step 5b: cheap case-(b) work that needs no rebase (my addition; gated on Step 0's restoreFail 2 count)**
- Change: walk `collapsed_.recs` in `prepareForGC` / `afterGC`, so their `savedIP` values get the same GC fix-up as live records. Then drop the epoch refusal in `tryRestoreCollapsedFrames`. The restore's per-record context checks still validate each frame.
- Knob: `PHARO_NO_COLLAPSED_GC_FIXUP`.
- Proof: probe 9 below (a forced scavenge and a full GC inside the ensure block). FRAME_RESTORE_CENSUS code 2 should go to 0.

**Step 6 (optional): assembly fast path for case (a)**
- Do this step only when Step 0's histogram shows the helper is still more than 30% of the per-event cost.
- Port design 2's stub, with the `0x1FFF` mask and the nil `outerContext` check.
- Split `emitMcfReturn_arm64` at its "Commit:" point into `emitMcfReturnTail`. The split must leave L identical.
- Keep the helper as the stub's fail path.
- Knob: `PHARO_MCF_NLR_NO_ASM`.
- Expected L: about -0.1%.

**Steps 7-10: the frozen segment for case (b) (design 3), only if Step 0 shows a frozen depth of 20 or more**
- **Step 7, refactor only.**
  - Add `fromFrame` / `baseSender` parameters to `materializeFrameStackImpl` (`:21519`), and factor out `materializeRecord`.
  - Gate: L unchanged, P, and the Context / Exception / Process / StepOver classes.
- **Step 8, the frame view with base fixed at 0.**
  - Add `frameStore_` plus a view for `savedFrames_` and `frameGCSide_`, and `realStackBase_`.
  - Change `forEachRoot` (`Interpreter.hpp:4306`, absolute range), `prepareForGC` / `afterGC`, `mcfClearMemos` and the depth limits to absolute indexes.
  - Gate: L unchanged and one full sweep. Prove the GC change by sabotage: a relative-only scan must fail probe 9.
- **Step 9, eager segment.**
  - In `nlrDelegateToImageProtocol` (`:10983-10991`): freeze, partially materialize from frame b with a SmallInteger marker, park frame b's memo as the guard, and rebase the view, `stackBase_`, `mcfFrames` and `mcfDepthLimit`.
  - Add the pop at the `returnValue` depth-0 hook (`:8963`, before `collapsed_`).
  - Add `segDeopt` and `segDeoptAndAbandon` in `transferTo`, `enterInterpreterFromCallback`, `terminateCurrentProcess`, snapshot, the callback time-out and `executeFromContext`.
  - Knobs: `PHARO_NO_NLR_SEGMENT`, `PHARO_NLR_SEG_EAGER` (on by default in this step), `PHARO_NLR_SEG_VERIFY` and `PHARO_NLR_SEG_CENSUS`.
  - Gate: design 3's probes 1-12 in all three arms, and one sweep.
- **Step 10, lazy boundary.**
  - Split `lazyCtxPending_` into bit 0 (stubs pending) and bit 1 (segment frozen). The stub at `AsmjitT1.cpp:6691` changes from a store to an OR.
  - Apply the marker rule in `mcfCtxSync`, `syncLazyContextsIfContext`, `lazyPrimNeedsSync` and a new `senderOf()` used by the VM's sender walks.
  - Turn `PHARO_NLR_SEG_EAGER` off by default.
  - Expected L: -2 to -3.5%.
  - Proof:
    - sabotage: remove the sender-read check, and the deep `thisContext sender` walk probe must trip VERIFY;
    - the census shows about 408K pops and about 0 deopts;
    - a full sweep, with the NLR, Context, Process, debugger and MethodProxies classes run twice;
    - an ASan tree;
    - build-x86.
- **Step 11, optional (design 3's B4).** Add a `pharo_mcf_seg_pop` helper in the depth-0 return path. Expected L: -0.2 to -0.4%.

Design 1's B2 (the ensure case through the helper) is last and optional. B2 gains about 0.15%, and it needs a proof that the send inside a helper is safe.

Expected total: Steps 1-5 give about 0.6-1.2%. With the segment, the total is about 3-5%.

## Open questions
1. What fraction of the 1.8% `materializeFrameStackImpl` cost is intern: NLRs, and what fraction is lazy syncs elsewhere? Step 0's MATFS census must split the two.
2. Can `handleContextNLRUnwind`'s send recurse into machine code (`tryJITActivation`) while the helper's caller is suspended? The answer decides whether B2 can ever be done.
3. Do the 408K case (b) restores mostly succeed today, or does restoreFail 2 kill most of them? The answer decides between Step 5b and the full segment.
4. Does any path outside the listed mediation points read a Context's sender raw (T1 format-3 `at:` / `size`, the inline getters, Fuel)? The marker rule needs the complete list before Step 10.
5. Under `PHARO_DET_SCHED`, A/B arms are not byte-comparable, because the native NLR skips `mcfRun`'s countdown charge. Is a fixed charge at the native NLR acceptable, or should arms be compared only by statistics?

## Ensure shortcut (2026-10-16, the default)

A non-local return through exactly one `ensure:` frame E does not use the
image unwind protocol (`aboutToReturn:through:`, `return:through:`,
`resume:through:`).  `mcfNonLocalReturn` does these steps instead:

1. It unwinds the frames above E's callee, as a plain non-local return does.
2. E's callee (the protected block) returns the value to E.  E then runs as
   if `self valueNoContextSwitch` answered the value: `ensure:` sets
   `complete` and runs the handler itself.
3. It removes the frames from the home H up to E (their contexts die) and
   moves E's frame down over H's.  E's `^ returnValue` then returns to H's
   sender.

The protocol has the same effect, but the frames from H up to E stay alive
until the handler returns.  The shortcut runs only when nothing can see
that difference:

- E runs `BlockClosure>>ensure:`, stopped after its `valueNoContextSwitch`
  send, with `complete` nil and no context.
- No frame from H up to E is a handler (primitive 199) or unwind
  (primitive 198) frame.
- The handler block has no `^`, no `thisContext` and no nested block.

Otherwise the protocol runs as before.  Lexicon (one such return per
`Symbol class>>intern:`, through `Semaphore>>critical:`, 1.7 million per
run) went from 222.5 to 187.5 billion instructions, 13.4 to 11.4 s.  A
return through ensure: took about 12000 instructions before.
Opt-out: `PHARO_MCF_NO_ENSURE_SHORTCUT`.

## Resume shortcut (2026-10-16, the default)

Every exception return runs `handlerContext return: value`, then `sender
resume: value`.  `Context>>resume:` is `self resume: value through:
(thisContext findNextUnwindContextUpTo: self)`: it materializes every
frame for `thisContext`, then `resume:through:` terminates the chain and
returns the value to the receiver C.

When `thisContext` runs in `Context>>resume:` (`mcfResumeShortcut`), the VM
does the same work itself if these conditions are true:

- C is a heap context on the chain of the running frames, and C is not
  dead.
- No frame or context between the current frame and C is an unwind
  (primitive 198) frame.

The steps are: pop the live frames, terminate the contexts between the
depth-0 frame's context and C, and return the value from that context into
C.  Machine code then exits, and the interpreter continues in C.  This is
what `resume:through:` does when the unwind list is empty.

An exception signal + return went from 30 to 20 ms per 30000 (Cog 20-24).  Instructions dropped 26%.  Opt-out: `PHARO_MCF_NO_RESUME_SHORTCUT`.
