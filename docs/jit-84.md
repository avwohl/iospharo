# Step 8.4 — Sista dispatch from asmjit-T1 IC HIT

Plan to take the SISTA_BIT dispatch infrastructure (landed
2026-05-21, commit `753e9f9c`) from dormant scaffolding to a real
perf win for fib + send-heavy benches.

## Implementation progress (2026-05-21)

| Blocker | Status | Commits |
|---|---|---|
| B1: Sista self-recursive inline lowering | **PARTIAL** (depth-cap raised + IR op scaffolded) | `712fdcb9`, `ca6bfdfb` |
| B2: Compile trigger from patchJITICAfterSend | **DONE** | `a9799b60` |
| B3: VM shutdown hang in bail-only | **DONE** (tactical blacklist) | `4ac71d2f`, `5d6b6f87` |
| B4: Real BLR emit at trySistaCall | **stub kept** (reassessed not worth it) | `6f96a365`, `c5ce9d4c` |

**Surprise win**: raising `kMaxSistaHelperDepth` from 1 to 64
(B1 partial) unblocked Sista helper-chain execution for benches
with deep recursion that previously bailed early.  Bench-suite
ratios vs Cog:

    benchmark           before      after
    sieve x100         100ms (10×)  8ms  (0.8× — we're FASTER!)
    5000 factorial     148ms (74×)  23ms (11.5×)
    fib(28)            144ms (48×)  146ms (48× — no change)

Sieve went from 10× slower to **faster than Cog**.  Factorial
got 6× faster.  fib unchanged because inline-J2J already handles
its self-recursion at near-Cog speed; Sista doesn't see the
recursive calls (they're handled by asmjit-T1's bit-60 J2J path
before Sista's IC dispatch is reached).

So while B1 in its full form (`kSendInlineSelf` IR op) is still
multi-week, the depth-cap change alone delivers significant wins
across several benches.

**2026-05-21 follow-up**: IR scaffolding for `kSendInlineSelf`
landed (commits `ca6bfdfb`, `e68aa61d`).  The op is declared in
`SistaIR.hpp`, wired through `OpInfo`, and the builder recognises
self-recursive sends via inline hints at BOTH the `kSendCallHelper`
emit site AND the `kSendUnspeculated` (default Send0/1/2) emit
site.  Lowering falls through to `kSendCallHelper` behaviour for
now (no perf change yet); the IR op shows up in dumps as
`send_inline_self` when the recogniser fires.

End-to-end verification with PHARO_TRACE_SELF_REC instrumentation:
benchFib's recursive send at bcOff=10 fires `isSelfRec=1` with the
inline hint correctly pointing back to benchFib's own oop bits.

Also fixed `sendSiteMap_` population for asmjit-T1-compiled methods.
`extractInlineHintsForMethod` requires `getSendSiteBCOffsets` to map
sendIdx → bcOffset; previously only the stencil-based JITCompiler
populated this map.  asmjit-T1 now calls
`JITCompiler::setSendSiteBCOffsets` after the send-site loop in
`compileViaAsmjit`.

What remains for the full B1 win: replace the lowering of
`kSendInlineSelf` from "fall through to kSendCallHelper" with a
real inline tail-call (BR with save-stack protocol mirroring
inline-J2J's J2JSave).  All builder + IC + IR wiring is in place;
the patch surface is now isolated to `SistaLowering_arm64.cpp` (and
its x86 counterpart).



Status when this plan was written:
- `bench-correctness.sh fib 20/28/30` PASS in both default and
  `PHARO_T1_NO_WARM_J2J_GATE=1` modes.
- Warm steady-state fib(28) = 8 ms (gate OFF, Sista OFF), 22 ms
  (gate OFF, Sista default), 36 ms (Sista bail-only).
- Cog reference fib(28) = 3 ms.

## What's in place

| Piece | File | Status |
|---|---|---|
| SISTA_BIT in IC extras | `Interpreter.cpp:18172`+ | landed |
| `sista::Runtime::lookupCompiled` const accessor | `SistaRuntime.hpp:121` | landed |
| `t1InlineSistaCall` debug flag | `DebugSettings.{hpp,cpp}` | landed (default off) |
| `trySistaCall` label + stub | `AsmjitT1.cpp:4068` | landed (bails to chain-loop) |
| Bit-55 dispatch before bit-60 | `AsmjitT1.cpp:3279` | landed (gated by flag) |

The bit-55 path is currently never taken because:
1. `PHARO_T1_INLINE_SISTA_CALL=1` is the only way to enable the
   dispatch (default off).
2. Even if enabled, `lookupCompiled` returns null for send-having
   methods because `sista::Runtime::compile` skips them (`hasSend &&
   !hasSplice && !compileBailOnly`).

## The four real blockers

### B1 — Sista's per-send helper overhead

For `benchFib`, Sista (with `PHARO_SISTA_COMPILE_BAIL_ONLY=1`) lowers
each recursive send via `Op::kSendCallHelper` → `jit_rt_sista_call_send`.
That helper sets up `argCount`, calls `sendSelector`, observes the
result, and returns to Sista's caller.  Measured: ~24-36 ms / fib(28)
vs 8 ms inline-J2J.

Need: a `Op::kSendCallSelf` or `Op::kSendCallInline` variant that
recognises self-recursive calls (same CompiledMethod) and emits an
inline tail-call to Sista's entry — bypassing the helper entirely.

Acceptance: Sista bail-only fib(28) ≤ 12 ms (within 50% of inline-J2J).

### B2 — Sista compile triggered only at warmup

`Sista::compile` is called from `tryActivateSistaDispatch`
(Interpreter.cpp:8724) which fires on every method entry once
`executionCount >= t1Warmup` (default 100).  Each compile attempt
costs a few ms per method.

Need: `sistaRuntimeForGCHook_->compile(method)` triggered ONCE per
method (cached afterwards) when T1's IC patcher first sees a hot
target.  Move the trigger from `tryActivateSistaDispatch` to
`patchJITICAfterSend` so the compile happens at IC-fill time and
the result is immediately reachable by asmjit-T1 emit via
`lookupCompiled`.

Acceptance: post-warmup `[SISTA] compile=N` count matches the
number of distinct hot methods (not the number of activations).

### B3 — VM shutdown hang in Sista bail-only

`PHARO_SISTA_COMPILE_BAIL_ONLY=1` `timeout 30` exits with code 124
(timeout) even though `/tmp/bench_correctness_result.txt` contains
the correct fib values.

**2026-05-21 investigation (commit `5d6b6f87`):**

Added PHARO_TRACE_EXIT=1 tracing at primitiveQuit, executePrimitive,
interpret() end, and main exit.  Default mode shows the full
EXIT-TRACE chain.  Bail-only mode shows ZERO output — `interpret()`
never returns; primitiveQuit (prim 113) is never reached.

Root cause hypothesis: Sista bail-only compiles methods that the
gate `(hasSend && !hasSplice && !compileBailOnly)` would otherwise
reject — including `Smalltalk class>>exitSuccess` and similar
multi-send wrappers.  Inside Sista's compiled fn, the helper-send
chain (exitSuccess → exitSuccess: → exit:) hits
`kMaxSistaHelperDepth=1` cap.  The 2-deep helper returns 0
(deopt-on-zero), Sista's lowering bails to interp at the source
bcOffset, but the deopt path apparently doesn't actually re-execute
the prim that quits.

Partial fix landed in `SistaRuntime.cpp`: in bail-only mode, also
skip methods with the hasPrimitive flag.  Sista's lifter always
skips the CallPrimitive (0xF8) bytecode (assuming the prim was
already tried in interp); compiling a prim method produces an fn
that runs ONLY the fallback bytecodes — disastrous for prims like
113 (quit) whose fallback never quits.

Prim-skip alone doesn't fix the hang.  The deeper issue is the
helper-depth interaction: even non-prim methods on the exit chain
(`Smalltalk exitSuccess`, etc.) fail to reach prim 113 because the
deopt re-execution doesn't actually fire the prim.

Root fix requires one of:
1. Detect non-recursive helper chains and allow deeper nesting
   (raises kMaxSistaHelperDepth selectively).
2. Fix the deopt-to-interp re-execution to actually fire the prim
   (likely a bug in Sista's deopt path post-helper-bail).
3. Blacklist a fixed set of selectors (exit, exitSuccess, ...) from
   Sista compile in bail-only mode (hack).

Acceptance: `PHARO_SISTA_COMPILE_BAIL_ONLY=1 ./build/test_load_image
/tmp/bench_correctness.image` exits with code 0 within 30s.

**Effort revised: 3-7 days** with lldb-level deopt-path debugging.

### B4 — `trySistaCall` stub needs a real emit (2026-05-21 attempted)

**Attempted commit `6f96a365`**, then reverted to stub:

Tried the naive emit:
```
a.and_(x6, x7, asmjit::Imm(0x0000FFFFFFFFFFFFULL));
a.blr(x6);
```

Result: SIGSEGV at fault addr 0x331 inside Sista's emitted code on
the second instruction.

Root cause: Sista's `CompiledFn` expects state to be the CALLEE's
frame (receiver = new rcvr, tempBase = sp - nArgs*8, literals =
method+16, method = callee, argCount = nArgs, ip = bcStart) — same
contract `tryActivateSista` honours via `pushFrame` + state mutation.

For asmjit-T1's IC HIT path to invoke Sista directly, we'd need to:
1. Save caller's state.method, state.receiver, state.tempBase,
   state.literals, state.argCount, state.ip somewhere (C stack via
   push/pop?  Or a per-thread save area?).
2. Set state fields to callee's values (receiver = sp[-(nArgs+1)*8],
   tempBase = sp - nArgs*8, literals = methodOop+16, etc.).
3. BLR to Sista's fn.
4. On return, restore caller's state.
5. Pop nArgs+1, push state.returnValue.

That's ~30+ instructions per send site AND requires a save area
Sista's deopt path also has to honour (currently it doesn't).

Re-evaluated: this is essentially **replicating `activateMethod` in
JIT-emitted code**, which is what `tryActivateSista` already does
via the chain-loop bail.  The current stub (bail to dispatchCached
→ chain-loop → tryActivateSista) is the same observable behaviour
without the duplication risk.

For Step 8.4 to provide a real perf win over the stub, you need a
shared CALLEE-FRAME setup contract that asmjit-T1's send emit,
inline-J2J, and Sista all use.  Inline-J2J's J2JSave fits 56 bytes
on a per-thread stack — Sista could be made to use the same save
on entry, but currently it just clobbers state directly.

**Recommendation: skip B4 for fib.**  Inline-J2J already handles
self-recursion at near-Cog speed (2.7 ns/send) — Sista's per-send
helper (24-36 ms / 3M sends = 9 ns/send) is 3× slower for the same
workload.  Step 8.4 only pays off if Sista's lowering can do
something inline-J2J can't (monomorphic inlining of `benchFib(K-1)
+ benchFib(K-2) + 1` collapsed into one specialised expression).

That's the B1 "kSendInlineSelf" work — multi-week.

Current stub (`AsmjitT1.cpp:4068`):

    a.bind(trySistaCall);
    a.b(dispatchCached);

After B1+B2+B3, replace with:

    a.bind(trySistaCall);
    // x7 = extras (bit 55 set, bits 47:0 = fn ptr)
    // x1 = receiver, x2 = sp (already loaded at IC HIT entry)
    // x0 = state (always; not touched in IC HIT path)
    //
    // Sista's fn signature: void (*)(JITState*).  It reads/writes
    // state.sp, state.receiver, state.returnValue, state.exitReason.
    a.and_(x6, x7, asmjit::Imm(0x0000FFFFFFFFFFFFULL));
    a.blr(x6);
    // After return, x0 unchanged.  Read state.exitReason.
    a.ldr(w3, ptr(x0, OFF_EXIT));
    a.cmp(w3, asmjit::Imm(EXIT_RETURN));
    a.b_ne(dispatchCached);  // any non-return exit → bail
    // Normal return: pop callee args/recv, push returnValue.
    a.ldr(x1, ptr(x0, OFF_RETURNVALUE));
    a.ldr(x2, ptr(x0, OFF_SP));
    a.sub(x2, x2, asmjit::Imm(8 * (nArgs + 1)));  // pop args+recv
    a.str(x1, ptr(x2));  // push retval at where receiver was
    a.add(x2, x2, asmjit::Imm(8));
    a.str(x2, ptr(x0, OFF_SP));
    // Clear exitReason so the caller doesn't see stale EXIT_RETURN.
    a.str(wzr, ptr(x0, OFF_EXIT));
    a.b(endOfSend);

Acceptance: with `PHARO_T1_INLINE_SISTA_CALL=1`, fib(28) cold-start
passes bench-correctness AND warm steady-state ≤ inline-J2J's 8 ms.

## Execution order

```
B3 (shutdown hang)  ─┐
                    ├─►  B2 (compile trigger move) ─┐
B1 (helper inline) ─┘                              ├─► B4 (real emit)
                                                   │
                                       SISTA_BIT works on fib
```

- **B1 + B3 are independent** and can be tackled in parallel.
- **B2 depends on B3** (without a clean exit, you can't measure
  whether B2's once-per-method change made things better).
- **B4 depends on B1 + B2** (the emit isn't worth wiring up until
  Sista is both correct AND fast enough to beat inline-J2J).

## Why this is multi-week, not multi-session

- **B1** is the hardest.  Sista's IR + lowering has no concept of
  "this send target is my own method's entry."  Adding it requires:
  (a) a builder pass that recognises self-recursive sends from the
  CompiledMethod oop, (b) an IR op (`kSendInlineSelf`) and lowering,
  (c) a save/restore protocol matching inline-J2J's J2JSave, (d)
  correctness validation across the deopt + framepoint replay path.
  Probably 2-3 weeks for one engineer, including the LLDB time to
  debug the inevitable register/stack-corruption bugs.
- **B2** is 2-3 days.  The compile trigger move + cache state audit.
- **B3** is 2-7 days, depending on how hidden the hang is.
- **B4** is 1-2 days once B1-B3 are done.  Mostly mechanical
  asmjit emit work mirroring inline-J2J's tail-call.

Total: 4-6 weeks for one engineer to fully realize Step 8.4.

## Suggested first commit (when work resumes)

Implement **B3 (shutdown hang)** first — it's the lowest risk and
makes B1/B2 measurable.  Strategy:

1. Add `PHARO_TRACE_EXIT=1` log points at: SessionManager shutdown,
   JITRuntime destructor, SistaRuntime destructor, image-save end,
   process_exit syscall.
2. Run with `PHARO_SISTA_COMPILE_BAIL_ONLY=1 PHARO_TRACE_EXIT=1
   timeout 60 ./build/test_load_image /tmp/bench_correctness.image`.
3. The last log line before the hang identifies the responsible
   subsystem.

## References

- Existing Sista dispatch from interp: `Interpreter.cpp:9047-9162`.
- Sista IR + send helper: `SistaLowering_arm64.cpp:1140-1280` for
  `kSendCallHelper`.
- inline-J2J save layout (model for B1's inline-self-send): the
  `J2JSave` struct at `Interpreter.hpp:580+` + push/pop in
  `AsmjitT1.cpp:3676-3801` (push) and `2557-2634` (return prelude).
- IC extras encoding: `Interpreter.cpp:18012-18172` (bits 48:63).
- Step 4 dependency: `docs/jit-may20.md:140-167`.

## Acceptance criteria

Step 8.4 is "done" when ALL of these hold:

1. `bench-correctness.sh fib 20/28/30` PASS in default + gate-OFF +
   `PHARO_T1_INLINE_SISTA_CALL=1` modes.
2. Warm steady-state fib(28) ≤ 6 ms (currently 8 ms inline-J2J,
   3 ms Cog).
3. `PHARO_SISTA_COMPILE_BAIL_ONLY=1` runs exit cleanly (code 0).
4. `[SISTA] compile=N` shows N matches distinct hot methods, not
   activations.
5. SISTA_BIT default-on (`PHARO_T1_NO_INLINE_SISTA_CALL` opt-out).
