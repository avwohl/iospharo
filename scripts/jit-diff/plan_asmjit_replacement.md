# Plan: replace stencils with direct asmjit emission

## Why

The current copy-and-patch stencil JIT is 41,400 lines across 31
files.  Across two architectures it duplicates stack-discipline,
register-allocation, and patching logic.  The recently-found bugs
(MUSTBOOL cascade in Set>>fullCheck, findElementOrNil: stack
corruption, privHandlerContext DNU loop) all share a root cause:
a stack-discipline invariant was violated *somewhere* across hundreds
of stencil templates and their per-arch generated code.  Tracking
down the violation site by site is the work that bogged this branch
down for six weeks.

asmjit is already a dependency.  Sista's Tier2 lowering (the
"better" path) already emits via asmjit on both arches
(`Tier2Compiler_arm64.cpp:2267 lines`, `Tier2Compiler_x86_64.cpp:1755 lines`).
There is prior art in this repo for what good asmjit-based emission
looks like.

## End-state shape (target)

```
src/vm/jit/
    JITRuntime.cpp           runtime helpers (sendSlow, primAt, IC fill, ...)
    JITRuntime.hpp
    JITState.hpp             (unchanged) compact state struct
    JITMethod.hpp            (unchanged) code-zone JIT method header
    asmjit/
        Compiler.cpp         single per-bytecode emitter
        Compiler.hpp
        StackHelper.cpp      stack discipline (push/pop/save/restore) — ONE place
        StackHelper.hpp
        BytecodeEmit.cpp     dispatch table from opcode → emit fn
        Decoder.cpp          (unchanged) bytecode decode helpers
```

What goes away:
  - `stencils/stencils.cpp` (5045 lines)
  - `generated_stencils_x86_64.hpp` (2765 lines)
  - `generated_stencils_arm64.hpp` (4561 lines)
  - `extract_stencils.py` and the build-time stencil-extraction pipeline
  - `JITCompiler.cpp` (2862 lines) → replaced by ~1500 lines in
    `asmjit/Compiler.cpp`

What stays:
  - `JITRuntime.cpp` (3246 lines): runtime helpers don't change
  - IC table layout, JITMethod struct, code-zone management
  - Interp↔JIT bailout protocol (`ExitArithOverflow` etc. handled
    in `Interpreter.cpp`)
  - GC root scanning for JIT frames
  - The Sista IR + Tier2 paths can stay or get the same treatment
    later (lower priority — Sista isn't yet enabled by default on
    Linux x86_64 anyway)

Estimated final size: 12-15K lines of JIT code (down from 41K).

## Why this works where stencils didn't

1. **Stack discipline lives in ONE place.**  `StackHelper` is the
   single source of truth for push/pop/SP-save-on-call/SP-restore-on-
   return.  Every emit fn calls into it.  No more "bytecode 28's
   variant of pop forgot to update the cached TOS register" bugs.

2. **No cross-section invariants.**  Stencils share invariants
   across files (the saved-frame format set in stencils.cpp:1750
   must match what J2J_INLINE_RETURN_IMPL reads at line 491+).
   With asmjit emission, the producer and consumer are in the same
   function or right next to each other.

3. **The diff fuzzer is the test suite.**  Each new bytecode
   handler is added under a feature flag and validated by running
   `scripts/jit-diff/run.sh` until the failure count drops.  No more
   "fix one bug, surface three" because regressions are caught
   immediately.

4. **Disassembly is free.**  asmjit can dump the emitted machine
   code as text with addresses, alongside the source bytecode that
   produced it.  Debugging "why does this opcode produce wrong
   code?" becomes "read 6 lines of asm vs 6 lines of source," not
   "find which 50-byte stencil got patched with which 4-byte
   relocation operand."

## Migration path (incremental, fuzzer-driven)

### Phase 0: gate
- Add `PHARO_USE_ASMJIT_T1=1` env flag.  Default OFF.  When ON,
  `JITRuntime::compileMethod` routes to `asmjit::Compiler` instead
  of the stencil JIT.  Both paths compile and link, so we can A/B.

### Phase 1: emit-and-bail skeleton
- Create `asmjit::Compiler` that:
  - For ANY method, emits a single trampoline that immediately
    returns `ExitSend` (back to interp).
  - Allocates a JITMethod struct so the rest of the runtime sees
    "yes, this is JIT-compiled."
  - This lets us prove the asmjit↔runtime plumbing works without
    needing a single bytecode emitter.
- Run fuzzer.  Should: zero pass (everything bails to interp every
  time, but no JIT crashes).  Goal: zero `JIT_TIMEOUT`, no segfaults.

### Phase 2: pure pushes/pops/returns
- Implement: pushReceiver, pushTemp, pushRecvVar, pushLitConst,
  pushTrue/False/Nil, pushZero/One, pop, returnReceiver, returnTop.
- About 12 opcodes.  Stack discipline lives in `StackHelper`.
- Fuzzer should now pass simple tests like `1 printString` (a single
  literal returned from the activation).

### Phase 3: arithmetic
- `addSmallInt`, `subSmallInt`, `mulSmallInt`, `lessThanSmallInt`,
  `equalSmallInt`, etc.  About 14 opcodes.
- Each emits the SmallInt fast path and bails to `arith_overflow`
  helper on non-SmI.  Bail must restore SP to point past the
  args (the same place the stencil version did, but explicit and
  in `StackHelper`).
- Fuzzer should now pass `(3+4) printString`, all the `arith_*`
  tests, and most `cond_*` tests.

### Phase 4: sends
- `stencil_sendJ2J` is the workhorse — implement equivalent.  It's
  the highest-risk emit because it has J2J inline frame save/restore.
- Approach: keep IC table layout; emit IC probe inline (5-6
  comparisons) and bail to `jit_rt_ic_miss` on miss.
- This is where the `findElementOrNil:` bug lived — get it right by
  centralizing the SP save in `StackHelper::beginCall(nArgs)` and
  restore in `StackHelper::endCall(nArgs, retReg)`.
- Fuzzer should pass `coll_*` and `iter_*` tests.

### Phase 5: control flow
- `jumpFalse`, `jumpTrue`, `jumpFalseBack`, `jumpTrueBack`,
  unconditional `jump`.  Conditional jumps must call `mustBeBoolean`
  on non-Boolean.
- Block creation, block evaluation.

### Phase 6: GC integration
- Pin the JIT frames so GC can walk them.  Use existing
  `pushFrameForJIT` / `popFrameForJIT` hooks.

### Phase 7: cutover
- Once fuzzer pass count under `PHARO_USE_ASMJIT_T1=1` matches or
  exceeds the stencil JIT (which is currently 0%), retire the
  stencil JIT.
- Delete: `stencils/`, `generated_stencils_*.hpp`,
  `extract_stencils.py`, `JITCompiler.cpp`.
- Promote `asmjit/Compiler.cpp` to be the only T1 path.

## Risks and mitigations

| Risk                                              | Mitigation |
|---------------------------------------------------|-----------|
| asmjit code size ≫ stencil code size              | Measure in Phase 3.  Stencil JIT averages ~50 bytes/bytecode; asmjit may be 100-200.  Acceptable up to 4x given correctness wins. |
| asmjit per-method compile time ≫ stencil          | Stencil JIT is fast because it's memcpy+patch.  asmjit assembles each instruction.  Should still be sub-ms per method.  Measure in Phase 4. |
| Sista lowering breaks if T1 changes               | Sista already uses asmjit — same codegen layer.  Tier2 calls `state.jitMethod->codeStart` in the same way.  No churn. |
| iOS/macOS toolchain breaks                        | asmjit already builds for Mac Catalyst (the asmjit_catalyst_fix.patch in memory/ is for that).  No new platform-portability concerns. |

## Effort estimate

- Phase 0+1 (skeleton, runtime plumbing): 1 session, ~500 lines
- Phase 2 (pushes/returns, ~12 opcodes): 1 session, ~600 lines
- Phase 3 (arithmetic, ~14 opcodes): 1 session, ~800 lines
- Phase 4 (sends, the hard one): 2 sessions, ~1000 lines
- Phase 5 (control flow, blocks): 1 session, ~600 lines
- Phase 6 (GC integration): 1 session, ~300 lines
- Phase 7 (cutover, delete dead code): 0.5 session

Total: ~7 sessions, ~3800 lines new code, deleting ~10K+ lines of
stencils and per-arch generators on cutover.

Compare to: 1581 commits on this branch over 6 weeks (~50 commits
of effort PER WORKING DAY) and the JIT still doesn't work.  Even if
the estimate above is 3x optimistic, the rebuild is bounded.

## Next concrete step

Ship Phase 0 (the env-flag gate + asmjit::Compiler stub that
returns ExitSend immediately).  Run the fuzzer.  Confirm zero
crashes / hangs.  Then start Phase 2.
