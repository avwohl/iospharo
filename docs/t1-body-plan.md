# T1 method bodies: cutting operand-stack and dispatch overhead

Status (2026-10-15): plan from a design panel (a measurement of three
lexicon traces, three designs, a judge).  No step is done.  The
measurement found about 1050 of 2991 traced body instructions
removable: about 590 in send dispatch, about 430 in operand-stack
traffic.  The measurement is in the panel transcript
(workflow wf_5f36465f-40b).

Short answer: build the plan on the superinstructions design's "bind the next bytecode, then pop" mechanism. Add the sp-elision design's x25 writeback for code that still runs through memory. Drop the simstack-sends design's two risky parts: deferring the operand spill, and the version of the register result that loses re-entry. The largest remaining waste is send dispatch, which none of the three designs cover; it is a separate track at the end.

**What I verified**

1. Post-send re-entry is load-bearing. `mcfEntryFor` (`MachineCodeFrames.cpp:332-341`) resumes at `codeOffsetForBC(bcOff)` and returns nullptr when the offset is 0. Any position marked in `g_simDeferredAt` gets offset 0 (`AsmjitT1.cpp:15041`).
   - So the simstack-sends design's B3 kills re-entry at every post-send position whose result stays in a register. Each send that C++ completes then leaves the rest of the method interpreted. Tests still pass, so only the speed shows the loss.
   - The `resumeOverrides` fix in that design covers only retval-carrying resumes. Non-retval resumes go through bcToCode, which is the shape of the 2026-06-11 bug.
   - The superinstructions design avoids the loss: bind `bcLabels[j]` while the memory stack is still exact, then `ldr xR,[x25,#-8]!`.
2. The resume path is as the superinstructions design says. `resumeAfterCall` stores x1 and falls into `endOfSend` (12905-12912). Jumps to the post-send position use `bcLabels`. So every arrival passes the bound label and then the reload.
3. x26 is safe for that reload. `g_sim.enabled` requires `!g_useTos` (13749), and `PHARO_T1_TB_IN_X26` is a hard `#error` (line 111). `PHARO_MCF_KEEP_TOS_MIRROR` writes x26 only at returns and entries, which come before the reload, so the writes do no harm.
4. The sp shims are plain register moves when `PHARO_T1_SP_IN_X25=1` (4179-4194): 24 uses of `emitLoadSp(a, x2)` and 46 of `emitStoreSp(a, x2)`.
   - The Lprobe/LmissNoX5 contract does require x2 = the sp copy (comment at 8934). The send-arm conversion therefore has to update that contract, as the sp-elision design says.
   - The conditional jump (8466-8510) really emits mov, ldur, ldp, cmp, b, cmp, b, b, then sub, mov, b on each arm.
   - The image contract holds: on mustBoolBail the boolean is still on the stack.
5. The `\\` range check is dead. The emitter's own comment at 8346 says overflow is "impossible for mod". The asr/cmp/b.ne at 8408-8410 can be skipped when `op == 0x6A`. Note: the emitter compares against raw `0x6A`, so new code should use the SistaV1 name.
6. Monomorphic specialization sites never link. `linkSendSiteMcf` refuses on `extras & ~(kJ2J|kB56|kAddr)` (`JITRuntime.cpp:3634-3636`). The measurement's item 2 is real.
7. The x3/x8 survival claim holds. Between 8930 and 11015, x3 and x8 are touched only by debug save/restore blocks.
8. `emitRememberBarrierA64` appends to `g_jitRememberBuf` with no blr, so holding operands in registers across the barrier is safe. One more check is needed at the buffer-overflow edge.
9. Interpreter: `0x76` and `0x78` (== and ~~) are inlined, and `0x77` (class) goes through `commonSend` (`Interpreter.cpp:8031-8042`). The simstack-sends design is right that class must stay IC-driven.
10. x27 holds nil only when `mcfPinnedRegs()` is on (5153, 5245). The "simSrc returns x27 for nil" idea must be gated on that, or it stores garbage when `PHARO_MCF_NO_PINNED_REGS` is set. Neither design that proposes the idea mentions the gate.
11. `BcDepthMap.hpp` exists and can be the depth oracle. The existing knobs are reusable: `PHARO_T1_SP_TRACE`, `PHARO_T1_EMIT_SIZE_CENSUS` and `PHARO_T1_INLINE_PRIM_COUNTERS`.

**Problems found in the designs**

Simstack-sends (the register-operand design):
- B3 as written defers the post-send position and loses re-entry (finding 1). Replace B3 with the bind-then-pop mechanism.
- B5 (operands only in registers past the head) needs a spill on six or more generic edges and a refusal landing for x15. A missed edge leaves a short stack, and the gain is only about 40-60 of 2991. Reject B5 for now.
- A1 needs the x27 gate (finding 10).
- A3 pops memory operands before the forwarder check, so the slow path must push them back and restore x25. The design says "stores the operands back"; state the x25 restore explicitly.

Sp-elision:
- The design is sound and keeps x25 exact at every bytecode boundary.
- The 150-instruction claim double-counts work the other designs also claim: popInto, conditional jumps and send-head forwarding. Only about 85 movs (`mov x2,x25` and `mov x25,x2`) are unique to this design. Wall-time gain from those movs is small, because the core removes them at rename, but they still count as instructions retired.
- In direct mode, `simEmitStores` uses x2 as a scratch register, so any later reader of x2 as the sp copy gets garbage. The design's poison knob must be on while the send arm is converted.

Superinstructions:
- Best correctness argument of the three.
- Step 6 keeps the `stur` and only saves the reload. The better version, combined with the simstack-sends B2, lands direct arrivals after the pop: `sub x25,x2,#8(n+1)` with the result in a register and no stur. That is still exact, because the compile-time model holds a Reg item and the arrival skips the `bcLabels[j]` label.
- Step 3c (jump threading): the target of the and:/or: join is a jump target, so the threading must happen before the jump-target flush in the main loop (14132).

**Scores** (correctness / gain / testability, each out of 5)
- Superinstructions: 5 / 3 / 4.
- Sp-elision: 5 / 2 / 5. It is mechanical and has a poison oracle.
- Simstack-sends: 3 / 4 / 3. A and B1-B4 are good; B3 as written and B5 are the risks.

**Synthesized plan, ordered by gain per risk**

Expected deltas are in trace body instructions (out of 2991), then converted to lexicon. I assume about 65% of lexicon instructions are in method bodies, so 100 trace instructions ≈ 2.2% ≈ 5.4B of 246B. Step 0 replaces this assumption with real numbers.

Step 0: re-trace and run a census.
- The traces were taken with the x26 mirror on (finding 3), so re-trace scanFor:, isEmpty/ifEmpty: and errorNotices on the default build first.
- On build-dbg, run `PHARO_T1_INLINE_PRIM_COUNTERS` for specialization hits per family.
- Add `DEBUG_BOOL(PHARO_T1_FUSE_CENSUS)`: a dynamic count of the consumer kind after each send (pop/popInto, conditional jump, ReturnTop, push+binop, feeds another send).
- Baseline: lexicon `time -l` instructions retired, 5 interleaved runs. Record the `[JIT] Stats` line and the EMIT_SIZE totals.
- Gate: if `count × saving` < 1.2B for a kind, skip that kind in step 4.

Step 1: oracle first.
- Add `DEBUG_BOOL(PHARO_T1_SIM_SP_CHECK)` in `debug_vars.h`, emitted in `compileViaAsmjit`.
- At each bcLabel bind with no items pending, at every `endOfSend` arrival, at every `simGenericExit` and at every new bail edge: compare `(x25 - x22)/8 - numTemps` with the BcDepthMap depth minus the pending items, and brk on a mismatch.
- Prove the oracle by sabotage: add `DEBUG_BOOL(PHARO_T1_SIM_SABOTAGE)`, which drops one store in `simEmitStores`; the brk must fire on a sweep batch.

Step 2: trivial cleanups.
- `simSrc` (7191): return x21 for `SimK::Rcvr`, and x27 for nil `Const` only when `mcfPinnedRegs()` is on.
- `isPhase3ModOp` (8407-8410): skip the range check for `\\`.
- Knobs: `DEBUG_BOOL_RT(PHARO_T1_NO_SIM_FIXED_SRC)` and `PHARO_T1_NO_MOD_DEAD_CHECK`.
- Expected: about 20 trace instructions, about -0.4% lexicon.
- Test: SUnit sweep A/B on the knob, and md5 of probe output against `PHARO_NO_JIT`.

Step 3: writeback pops and direct x25 addressing in the non-send emitters (sp-elision parts A and B).
- Add `spBase` and `emitSpDrop` helpers next to `emitLoadSp` (4179).
- Convert these emitters:
  - popIntoTemp (7487-7496) becomes `ldr x1,[x25,#-8]!; str` (5 → 2).
  - ExtStoreTemp.
  - The ReturnTop non-MCF path (7674).
  - The conditional jump (8456-8510): `ldr x1,[x25,#-8]!`, branch to the target, fall through to the next bytecode, and `add x25,x25,#8` on mustBoolBail.
  - The `ldp` heads in arith (7697), == (8524) and 0x68-0x6F (8197-8420).
- Knobs: `DEBUG_BOOL_RT(PHARO_T1_NO_SP_DIRECT)` and `PHARO_T1_NO_SP_WRITEBACK`. With `SP_IN_X25=0` the helpers emit the old code byte for byte.
- Expected: about 70 trace instructions, about -1.5% lexicon.
- Tests:
  - `PHARO_T1_SP_SABOTAGE` (drops the bail `add`) must show in an SP_TRACE diff on a NonBooleanReceiver probe.
  - The SP_CHECK oracle and a full sweep.
  - EMIT_SIZE must show the conditional-jump bytes per site shrinking.

Step 4: the send result enters the simStack, using bind-then-pop (the superinstructions design's core).
- Add a `planSimConsumers` lambda next to `simTry` (13760). The planner applies only when the next position j is not a jump target and has no ExtendA/B prefix.
- After `emitOne` returns for such a send: `bind(bcLabels[j])`, then `ldr x26,[x25,#-8]!`, then push `SimItem{Reg,x26}`, and set `preBound=j` so the next iteration skips its bind, its flush and its deferral mark.
- Add `simTry` consumers for any pending top item:
  - Forward conditional jumps (0xB8-0xC7, ExtJumpTrue/False), with `simFlushBelow(1)` and a cold bail that does `str` then EXIT_MUST_BOOL with ip at the jump.
  - ReturnTop.
  - Compile-time threading of StateOop true/false into a conditional-jump target, decided before the jump-target flush.
- Knobs: `PHARO_T1_NO_SIM_SEND_RESULT`, `PHARO_T1_NO_SIM_COND_JUMP`, `PHARO_T1_NO_SIM_RETURN` and `PHARO_T1_NO_JUMP_THREAD` (all `DEBUG_BOOL_RT`).
- Expected: about 70 beyond step 3 (push+compare consumers, ReturnTop of pending temps, threading), about -1.5% lexicon.
- Tests:
  - SP_CHECK.
  - An EXIT_SEND-heavy probe (a megamorphic send then popInto, in a loop): the `mcfEntryFor` nullptr count and the OSR_TRACE "noentry" count must not rise.
  - A becomeForward probe for `a == nil` with a forwarded `a`, including forwarded to nil.
  - A mustBeBoolean probe.
  - Sweeps under `PHARO_DET_SCHED=1`.

Step 5: binops with operands in memory.
- Change the == gate (13839) and the arith gate (13978) from 2 or more pending items to 0 or more. Pop memory operands with `ldr`/`ldp` pre-index. Slow paths re-push the operands and restore x25.
- Add ~~ (`SpecialSendBase+8`) with kNE, and the 0x68-0x6F ops with immediate forms.
- Knobs: `PHARO_T1_NO_SIM_EQ_MEM` and `PHARO_T1_NO_SIM_WIDE_ARITH`.
- Expected: about 40, about -0.9% lexicon.
- Tests: `PHARO_MCF_VERIFY`, the forwarder probe and the sweep.

Step 6: receiver and argument feed into the send, store-through.
- In the main loop at 14154-14157, at a send with nArgs ≤ 2: `simFlushBelow(nArgs+1)`, load the receiver into x1 and the args into x3/x8 from the items, then store them with one `stp`/`str`. Set a one-shot `g_sendOpsInRegs`.
- The head (8790-8798) skips `ldur x1`.
- `tryPrimAt` and `tryPrimAtPut` (12108-12290) and float / (8834) read x3/x8. The byte path's temp moves from x8 to x16.
- Turn the feed off when any trace or verify knob that saves registers on sp is on.
- Knob: `PHARO_T1_NO_SIM_SEND_OPS`.
- Expected: about 45, about -1% lexicon.
- Tests: SP_CHECK, the sweep, and `PHARO_T1_VERIFY_AT` arms.

Step 7: direct specialization arrivals.
- Bodies for getter 11015, returnsSelf 11230, returnsLiteral 11264, multiSlot 11606, at: 12108, at:put:, size 12300, idHash 12379 and class 12428: compute the result into x26, then `sub x25,x2,#8(n+1)` (no stur), then branch to a label bound after the reload in step 4.
- Do this only for audited producers; every other arrival keeps the reload.
- In the same bodies, count slots in x16/x17 and drop the `OFF_ICDATAPTR` stash (12115, 12306). Bail paths then go to a re-materializing label.
- Knobs: `PHARO_T1_NO_SIM_RES_DIRECT` and `PHARO_T1_NO_SIM_PRIM_NOSTASH`.
- Oracle: `DEBUG_BOOL(PHARO_T1_SIM_RES_VERIFY)` checks that x26 matches what memory would hold, by keeping the stur in verify mode. Prove the oracle with a sabotage knob that skips x26 in the size body.
- Expected: about 35, about -0.8% lexicon.

Step 8: x2 removal in the send arm (sp-elision Part A for the send arm).
- One leaf per specialization kind.
- Update the Lprobe/LmissNoX5 contract comments (8899, 8934, 8951) and the J2J tail (9016).
- Knob: shares `PHARO_T1_NO_SP_DIRECT`. Verify knob: `DEBUG_BOOL(PHARO_T1_SP_DIRECT_POISON_X2)`, which must give zero faults on the 1-100 and 200-class batches under DET_SCHED, followed by a full sweep.
- Expected: about 40, about -0.9% lexicon.

Total for steps 2-8: about 320 of 2991 trace body instructions (11%), about -7% lexicon (about 15-18B). Code size per site must shrink or stay flat in EMIT_SIZE after every step.

**Separate track: send dispatch**

None of the three designs touches send dispatch, but it is the larger lever (measurement items 1, 2 and 5, about 590 in the traces):
- A. Link specialization sites. Add per-kind labels to `PatchSiteLabels`, and relax `linkSendSiteMcf` (`JITRuntime.cpp:3634`) per kind. The linked W2 enters with no x5/x7, so each kind needs its parameter (slot index, literal) either patched as an immediate or fetched from the IC by a body prelude. Knob: `PHARO_MCF_NO_LINK_SPECS`. About -240 in the traces.
- B. Keep the callee's MCF entry address in the IC entry. Code eviction must clear it. Knob: `PHARO_MCF_NO_IC_ENTRY_ADDR`.
- C. Accept formats 3 and 4 in `tryPrimAt`, with the fixed-field count in the IC extras, and use a jump table for primitive dispatch in the stub. Knob: `PHARO_T1_NO_INLINE_AT_FIXED`. About -100 per scanFor iteration.

**Rejected or deferred**
- The simstack-sends B5 (deferred operand spill): many edges for about 50 instructions. Revisit only after steps 6-7 are measured.
- The simstack-sends B3 as written (it defers the post-send position).
- A cross-bytecode sp delta, and fixed-base addressing from x22: I agree with the sp-elision design's rejection.
- Static inlining of #class.

**Validation rules for every step**
- Full SUnit sweep against a baseline from the same binary, compared with `classify-sunit.py`, both lists sorted with `LC_ALL=C`. A repro batch does not count.
- A DET_SCHED sweep.
- Lexicon instructions retired: 5 interleaved runs per arm, on a Release tree with `-DPHARO_DIAG_KNOBS=ON` (or `_RT` knobs).
- `[JIT] Stats` compile and refusal counts must match between arms.
- EMIT_SIZE bytes must not grow.
- Shared files change (`debug_vars.h`), so build-x86 must still compile.
- Commit each step with its knob, and update `docs/WIP.md` and `docs/simstack-design.md`.

**Open questions**
1. Census dynamic counts per consumer kind (step 0), which decide whether step 4 is worth doing.
2. How often C++ completes a send and then resumes at post-send positions in lexicon. This number measures what the simstack-sends B3 would have cost.
3. Whether any other JIT entry trampoline (`TrampolineAsm.S` 243, 312, 532) reaches the reload with an inexact stack.
4. Whether the barrier's overflow path (`g_jitRememberBuf` full) calls C++.
5. For the dispatch track A: how linked specialization bodies get their per-site parameter without x5/x7.
6. The real share of lexicon instructions in T1 bodies. The 68% figure is from wall-clock samples, not instructions retired.