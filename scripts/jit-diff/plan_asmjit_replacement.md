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

### Phase 4: sends — DONE 2026-05-11 (bail-to-interp)

After steps 1-3 fixed the GC-staleness bug that blocked the first
attempt, send bail-to-interp works the same as arith bail.

Single-byte sends now real-emit:
  - 0x70..0x7F: special selectors 16..31 (at:, at:put:, size, etc.)
  - 0x80..0x8F: literal send 0 args
  - 0x90..0x9F: literal send 1 arg
  - 0xA0..0xAF: literal send 2 args

Emit pattern (x86; ARM64 same shape):
  mov r8, [rdi+OFF_METHOD]
  add r8, Imm(bcOffsetFromMethObj)
  mov [rdi+OFF_IP], r8
  mov dword [rdi+OFF_EXIT], EXIT_ARITH_OVERFLOW
  ret

The interp catches the exit, dispatches the send via its normal
machinery, then continues at the next bytecode.  Same contract as
arith bail — and now correct because state.method is GC-tracked.

What's NOT included yet:
  - 2-byte ExtSend (0xEA) and ExtSuperSend (0xEB): need
    multi-byte decode in pre-scan + bcOffsetBase math for the
    second byte.  Methods containing them bail-on-entry.
  - Real IC dispatch + J2J chaining (the perf-win Phase 4b).
    Every send still pays the interp-dispatch cost; we just
    win on the prefix bytecodes (pushReceiver, pushTemp,
    pushLitConst, returnTop, arith) before the first send.

Results: 37/37 PASS, mini 3/3 PASS.

### Phase 4: original "real send" notes (deferred to Phase 4b)


First attempt (naive bail-to-interp-on-send) didn't work.  Reverted
to Phase 3 baseline.  What was tried and what blocked it:

**The naive approach.**  When the JIT pre-scan saw a send opcode
(0x70..0xAF), real-emit the method up to but not past the send;
when control reached the send byte, set `state.ip = bcAddrAbs`,
`state.exitReason = ExitArithOverflow` (= 6, used as a generic
"re-execute at ip" marker), and `ret`.  The interp catches the
exit and dispatches the send normally.  This sidesteps all IC and
J2J infrastructure for v1.

**Two coupled problems.**

(a) Bytecode-trailer pollution.  The method's
`totalBytes - bcStart - unusedBytes` includes selector/temp-name
trailer bytes past the last return.  In Phases 2+3 those bytes
were almost always rejected by the pre-scan (they're random
literal bytes that don't match the strict allowlist), so the
method bailed-on-entry and the trailer never mattered.  In Phase
4, the send range 0x70..0xAF accepts ~25% of random byte values,
so the pre-scan started passing methods whose post-return
trailers contained "send-looking" bytes.  The emit then generated
bail-to-interp-at-trailer-byte sequences that confused the runtime.

Fix attempt: walk forward to the first return and trim there
(`liveBytecodeLength()`).  This is correct in principle for the
Phase 4 opcode set (no forward jumps yet means no branch targets
past the return).

(b) Even with (a) fixed, the bcLen trim alone produces a SIGSEGV
~1400 compiles in.  Some method whose live bytecodes WERE in the
push/return/arith allowlist but whose trailers had previously
disqualified it now passes the pre-scan and real-emits — and the
real-emit path has a latent bug that didn't surface in Phases
2+3 (because that exact methodshape had been rejected for the
wrong reason all along).  Couldn't pin the exact mechanism
within the Phase 4 session.

(c) With sends enabled (no bcLen trim, just trust the allowlist),
the eval hangs from compile #1 (`Object>>on:`).  Likely the
interp dispatcher loops re-bailing on the same send because some
JIT-side state (the JIT prefix on the Smalltalk stack? state.sp?)
is in a shape the interp's send dispatch doesn't tolerate.

**What the next Phase 4 attempt should do differently.**

Two options, both bigger than the bail-to-interp shortcut:

1. **Real IC dispatch + J2J chaining.**  Emit the IC probe inline
   (5-6 cmp+je against cached classes), J2J chain to the IC's
   target on hit, fall through to `jit_rt_ic_miss` on miss.  No
   bail semantics needed mid-method.  This is the proper Phase 4
   from the original plan, and matches what the stencil JIT does.

2. **Real bcToCode table + new ExitReason.**  Build the bcToCode
   array (per-bytecode entry-point offsets) and add a new
   `ExitJITResume` exit reason that the runtime understands as
   "JIT bailed mid-method; here's the bc index to resume at."
   The interp runs the send, then re-enters the JIT at the
   bcToCode[ip+sendlen] address.  Lets us bail-to-interp for
   sends while keeping the rest of the method JIT-emitted.

Option 1 is the long-term right answer; option 2 is a stepping
stone.  Either way, the trailer/`bcLen` issue from (a) needs
resolving first — the pre-scan should explicitly use the
stencil decoder's `maxBranchTarget` logic (see
`JITCompiler.cpp:648`) rather than the byte-count subtraction.

**Memory:** see `memory/jit_pivot_2026_05_10.md` for context.

### Phase 4 (retry): bail-mid-method, with proper trailer trim

Findings from reading the interp dispatch in detail (2026-05-11):

- `Interpreter.cpp:15281-15287` — ExitSend handler just `return`s;
  the outer dispatch loop picks up at `instructionPointer_`.
- `Interpreter.cpp:18547-18551` — ExitArithOverflow handler does
  the same: `instructionPointer_ = state.ip; stackPointer_ =
  state.sp; return false`.
- `Interpreter.cpp:18041-18056` — both reasons share a state-
  validation check; both expect `state.ip` and `state.sp` valid.

**Bail-mid-method semantics ARE supported by the existing
contract.**  The Phase 4 hang was NOT caused by ExitArithOverflow
vs ExitSend confusion — both exits route through the same shape
of "set ip+sp, return to interp dispatch."

So what DID cause the hang at compile #1 (`Object>>on:`) and the
SIGSEGV at #1400?  Likely candidates, in order of suspicion:

1. **Trailer-byte miscompile.**  Without the `liveBytecodeLength()`
   trim, methods whose post-return trailer bytes happen to fall in
   0x70..0xAF emit bail-to-trailer-byte sequences.  A subsequent
   real bytecode then fall-throughs into the bail code.  Could
   produce both hang (loop re-bailing on the trailer byte's "ip")
   and SIGSEGV (executing past the emitted code into garbage).
2. **state.sp drift past method exit.**  If the bail's `ret`
   leaves state.sp at a position the interp dispatch doesn't
   expect (e.g. JIT pushed receiver before bailing, but interp
   re-runs the receiver-push too), every iteration of dispatch
   pushes another receiver until stack overflow.
3. **Exception machinery in Object>>on:**  `on:do:` uses the
   exception block + handler context.  If the JIT's bail-mid
   leaves the activeContext_ in the wrong shape, the exception
   raise/handle dance gets confused.

**Concrete next-session steps:**

1. ✓ **Build a 3-test mini-corpus** at `scripts/jit-diff/mini.txt`
   (commit `0d094765`).  Three tests: arith+`printString`,
   `1 class name`, `#a isString`.  Compiles within ~10 method
   compiles so bugs surface at compile #1 instead of #1400.

2. ✓ **Trim helper landed and enabled by default** (commit `<NEXT>`).
   `SistaV1::bytecodeLength()` + `computeLiveLength()` mirror the
   stencil decoder's `maxBranchTarget` logic at
   `JITCompiler.cpp:639-660`.  Set `PHARO_ASMJIT_T1_NO_TRIM=1`
   to disable for bisection.

3. ✓ **Resume-mid-method bug found and fixed** (same commit).
   Root cause: arith bail emitted `mov r8, Imm(bcAddrAbs); mov
   [rdi+OFF_IP], r8` — baking the **absolute** bytecode address
   into the JIT machine code at compile time.  When GC compaction
   moved the CompiledMethod, the baked address dangled into stale
   memory; on bail, `state.ip` pointed at garbage and interp
   dispatched random bytecodes from the old location, producing
   wrong return values that surfaced downstream as DNUs on
   selectors like `#isEmpty` with wrong-typed receivers.

   Fix: bail computes `state.ip = state.method.rawBits() +
   bcOffsetFromMethObj` at runtime.  `state.method` is a
   GC-tracked Oop that gets updated in-place during compaction
   (forEachRoot → afterGC), so the resulting address is always
   the post-GC bytecode location.  Mirrors the stencil JIT's
   approach (`s->ip = s->ip + bcOffset`).

   Two extra instructions per bail (load+add vs movabs).  Worth
   the correctness.

   Pre-fix: trim ON → 0/37 PASS.
   Post-fix: trim ON → 37/37 PASS, mini 3/3 PASS.

4. ✓ **Send bail-to-interp** (same commit as step 3 + follow-up).
   Single-byte sends (0x70..0xAF) now real-emit using the same
   GC-safe bail pattern as arith.  Methods like `^ self foo`,
   `^ self foo: x`, `^ self at: i` now have their prefix
   bytecodes (pushReceiver, pushTemp/pushLitConst, etc.) compiled
   to machine code; only the send itself bails to interp.
   - 37/37 PASS, mini 3/3 PASS.
   - ~30+ real-emit methods per eval (was ~5 in Phase 3).
   - Send still pays interp-dispatch cost; real IC + J2J in
     Phase 4b.

**Effort:** Steps 1-4 done in ~1 session.

### Phase 4b: real IC + J2J chaining

The current send emit bails on every send.  Real wins come from
inline IC + J2J chaining.  Approach:
  - Per-send 5-6 inline cmp+je probes against cached classes.
  - On hit, J2J chain to target's entry.
  - On miss, fall through to `jit_rt_ic_miss` runtime helper.
  - Keep IC table layout compatible with stencil JIT's existing
    runtime helpers (no churn in JITRuntime.cpp).
  - This is where the stencil JIT's `findElementOrNil:` stack-
    corruption bug lived — centralize SP save in a `StackHelper`
    so it's correct in exactly one place.

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
