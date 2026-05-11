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

### Phase 0: gate ✓ DONE 2026-05-10 (commit b508c089)
- Standalone test `test_asmjit_t1_stub` proves asmjit emit + call
  works on Linux x86_64.

### Phase 1: emit-and-bail skeleton ✓ DONE 2026-05-10 (commit 715fead5)
- `PHARO_USE_ASMJIT_T1=1` env flag routes every method through
  `compileViaAsmjit` in `src/vm/jit/asmjit/AsmjitT1.cpp`.
- Trampoline is 8 bytes (`mov dword [rdi+76], 2; ret`) — sets
  state.exitReason = ExitSend; the interp catches the exit and
  runs the method.
- Differential fuzzer: **0/37 PASS → 37/37 PASS** (every test
  now functionally interp-equivalent, but the JIT runtime layers
  are exercised).
- Goal exceeded: instead of "no crashes," everything works (because
  bail-to-interp on every entry equals interp-only behavior).

### Phase 2: pure pushes/pops/returns ✓ DONE 2026-05-10 (commit bcc4b26b)
- Implemented in `src/vm/jit/asmjit/AsmjitT1.cpp`:
  pushRecvVar, pushLitConst, pushTemp, pushReceiver,
  pushTrue/False/Nil/Zero/One, pop, returnReceiver/Top/True/False/Nil.
- Methods with anything else (sends, arith, jumps, ext-prefixes,
  pushLitVar, pushThisContext, blockReturn, popStore) compile to
  the Phase 1 bail stub.
- ~5-10% of methods qualify for real codegen during eval startup
  (mostly getters/setters and trivial ^self / ^literal).
- Differential fuzzer: 37/37 PASS, ~4 real-codegen compiles per eval.
- Critical fix: keep JITMethod.numBytecodes = 0 (the runtime expects
  a bcToCode table when non-zero).

### Phase 3: arithmetic ✓ DONE 2026-05-11 (commit b121788a)
- 8 ops: + - < > <= >= = ~=  (skip * / // \\ bitAnd:|Or:|Shift: @
  for Phase 3.5 — they have edge cases needing dedicated emit).
- SmI check via XOR-with-1 trick (`(a^1) | (b^1)` has low 3 bits 0
  iff both are SmI).
- +/- have signed-overflow check; compares use cmov.
- Bail sets state.ip = absolute bytecode addr; ExitArithOverflow=6;
  ret.  Interp catches ExitArithOverflow and re-executes via the
  regular dispatch.
- Fuzzer: 37/37 PASS held; ~5 methods/eval get real codegen (one
  more than Phase 2).

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
