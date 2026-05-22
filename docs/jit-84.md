# Step 8.4 — Sista dispatch from asmjit-T1 IC HIT

Plan to take the SISTA_BIT dispatch infrastructure (landed
2026-05-21, commit `753e9f9c`) from dormant scaffolding to a real
perf win for fib + send-heavy benches.

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
the correct fib values.  Something in the shutdown path (image
save? scheduler termination? Sista cache cleanup?) hangs.

Need: bisect via `PHARO_TRACE_EXIT=1` or similar to find which
shutdown step holds.  Likely candidates:
- Sista's asmjit runtime not freed cleanly.
- A SessionManager handler that mutates state after `exitSuccess`.
- A weak reference path that scans the Sista cache.

Acceptance: `PHARO_SISTA_COMPILE_BAIL_ONLY=1 ./build/test_load_image
/tmp/bench_correctness.image` exits with code 0 within 30s.

### B4 — `trySistaCall` stub needs a real emit

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
