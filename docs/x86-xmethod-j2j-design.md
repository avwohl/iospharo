# x86 cross-method inline-J2J — vetted design (Increment 1)

Source: design workflow `wf_1a7414dd-94b` (2026-06-15, 9 agents, 4 maps +
adversarial verify, 25 holes / 7 critical). Verdict: **go-incremental**.

## The lever

x86 has SELF-recursive inline-J2J only (benchFib 3.2x). Cross-method sends
(cfibx's `incc`, getter sends everywhere) run full IC dispatch (222ns/call vs
arm64's 8ns) → cfibx 27x. arm64 has cross-method (xmethod) inline-J2J +
bail-time J2J-save materialization; x86 does not.

## Increment 1 = canSkipJ2JSave callees only (NOT bit 57)

THE load-bearing correction (3 skeptics converged): the obvious gate
`bt r8,57` (kXGateOkBit) is **fatally wrong** — xmethodGateOk
(JITRuntime.hpp:109-114) admits up to `PHARO_T1_XMETHOD_MAX_IC` (default **8**,
not 0) IC sites AND canBailMidMethod callees by default. So bit 57 fires for
exactly the non-leaf, mid-bailing callees Increment 1 cannot support.

GATE on `callee->canSkipJ2JSave` (JITMethod.hpp:249) instead. It is
`isReal && (forceSaveless || (!canBailMidMethod && numICEntries==0))`
(AsmjitT1.cpp:10851). Means no-send + no-cond-jump-bail. `incc` (`^self+1`)
qualifies → **cfibx benefits**; getters/constant-returners qualify → broad
real-code win.

RESIDUAL CAVEAT (verified, beyond the workflow's "provably clean" claim):
canSkipJ2JSave does NOT exclude **arith-overflow** bails (ExitArithOverflow is
not a cond jump, so canBailMidMethod misses it). A qualifying callee with `+`/
`-`/`*` CAN still exit-to-C++ on overflow. For cfibx's bounded range incc never
overflows (clean path only). The GENERAL default-on flip needs the open
ExitArithOverflow-materialize bug fixed first → validate the overflow path
explicitly (deliberate cross-method overflow test + SUnit + DET_SCHED).

## Increment 2 (bailmid / numIC>0 callees) — BLOCKED, do not implement

The ExitArithOverflow chain handler (Interpreter.cpp:26322-26361) does NOT call
materializeJ2J and returns with j2jDepth>0 — a documented OPEN arm64 bug
(comments 26339-26350: "LEAF_ALL flicker root cause remains OPEN"; two prior
fix attempts regressed). Increment 2's soak (overflow inside a cross-method
callee) drives straight into it. DEFERRED until that handler materializes
correctly + a per-call entry-cursor pin is added.

## Critical facts (verified against source)

- `save.jitMethod = CALLER JM` (resume identity), NOT callee — the workflow's
  own APPROACH prose contradicted itself; source proves caller
  (Interpreter.cpp:23912 materialize reads it as resumer; 24798 round-trip
  writes callerJM). Writing calleeJM = historical V1 corruption class.
- state.jitMethod = CALLEE JM after the push (callee runs).
- argCount is `uint8_t` (JITMethod.hpp:215) → restore with **byte load (movzx)**,
  not dword (arm64 uses ldrb, AsmjitT1.cpp:4026). tempCount also uint8_t (off 35).
- nil-fill + newSp MUST use runtime `calleeJM->tempCount` via a LABELED loop
  with the no-fill edge (mirror arm64 6984-6997); the x86 self-rec push bakes a
  compile-time callerTempCount immediate (2804-2806) — WRONG for cross-method
  (callee tempCount != caller).
- OFF_LITERALS=16, OFF_JITMETHOD=56, OFF_ARGCOUNT=72 (AsmjitT1.cpp:854-859);
  caller literals+method+argCount MUST be restored in the return prelude (resumed
  caller reads OFF_LITERALS/OFF_METHOD directly).
- codeStart() == this + sizeof(JITMethod) (JITMethod.hpp:338) → calleeJM =
  entryAddr - sizeof(JITMethod) is valid; add a static_assert.
- E2 ordering: write callee state AFTER the save push + depth publish, so a
  save-room bail leaves state.method=callerCM (the X+BV SIGSEGV class).
- V1 only: static_assert(!PHARO_J2J_SAVE_V2) in the x86 block.
- Byte-offset off-by-one class CANNOT recur (x86 consumes a precomputed bit /
  offsetof, no raw byte cascade).

## Knob / kill-switch

`PHARO_T1_X86_XMETHOD` (DEBUG_BOOL, debug_vars.h, default OFF). When off, the
send-site gate + return prelude emit BYTE-IDENTICAL self-rec-only x86 (the
baseline is the hot DEFAULT-ON self-rec path — capstone-diff is mandatory).
Default-ON flip (opt-out) only after full 565-class suite shows 0 deterministic
regressions on the x86 box.

## Validation gate (x86 AWS box; x86 can't build locally)

1. Knob-OFF capstone-diff send site + return prelude vs current jit branch →
   prove byte-identical baseline.
2. Knob-ON capstone-disasm one cross-method send: canSkipJ2JSave byte test (NOT
   bt 57); save[32]=callerJM; callee writes AFTER push; calleeJM=entryAddr-
   sizeof; dynamic nil-fill from calleeJM->tempCount; argCount byte-restore; jmp.
3. eval micro-tests 5/5: a canSkipJ2JSave cross-method pair, mutual-recursion,
   3+4=7 canary, factorial/gcd vs baseline. g_x86_xmethod_fires>0.
4. Benchmarks: cfibx (key — does it drop from 599?), benchFib + loop + battery
   no-regress vs self-rec baseline. PLUS a deliberate cross-method arith-overflow
   test (the residual-caveat path).
5. SUnit subset (inline-J2J-sensitive classes) A/B knob-on vs off per-test
   IDENTICAL; then under PHARO_DET_SCHED=1 + coarse quantum.
6. Full curated 565-class suite knob-on vs self-rec baseline: 0 deterministic
   regressions before any default-ON flip. (x86 JIT base is bit-rotted,
   debug_vars.h:208 + per-send sp leak 200 — repro any new failure knob-OFF
   before blaming this change.)

## Increment 1 IMPLEMENTED + 3 validation boxes (2026-06-15) — KNOWN-BROKEN

Commits 1bba9021 (Increment 1) + cc1caa18 (tighter gate: +!hasPrimPrologue
+!hasHeapWrites). Behind PHARO_T1_X86_XMETHOD, default OFF.

Box results:
- KNOB-OFF: byte-identical baseline CONFIRMED (cogRunBench 133/51/571, cfibx20
  =17710). The hot self-rec default-on path is untouched.
- KNOB-ON: CORRUPTS STARTUP. The path FIRES (counter: ~245K cross-method
  inline-J2J during image startup — getters everywhere), but EVERY eval returns
  nil via FullBlockClosure>>onErrorDo: -> SnapshotOperation>>executeStoringError:,
  including the `3 + 4` canary (which uses no cross-method send) -> the
  corruption is during STARTUP, not the bench. The tighter prim/heap gate did
  NOT fix it -> it is a COMMON-PATH emit bug (getter cross-method sends), not the
  prim/heap bail path.

Verified CORRECT by inspection (ruled out): calleeJM = entryAddr - sizeof(JITMethod)
(matches arm64 6014-6018: entryAddr = extra & 0x0000FFFFFFFFFFFF, calleeJM =
entryAddr - sizeof); the save push is byte-copied from the working self-rec push
(save[32]=callerJM); literalsCache/bcStartCache/tempCount/canSkipJ2JSave read via
offsetof; the return-prelude restore preserves rax/rdi and uses only
already-clobbered rcx/rdx. The bug is inspection-resistant.

THREE blockers, in order:
1. A subtle COMMON-PATH emit bug in the cross-method send/return (needs runtime
   debugging — but x86 can't build locally and the AWS box has no lldb wired up;
   next step = a capstone-disasm box (workflow validation step 2) to SEE the
   emitted cross-method site, or wire lldb on the box).
2. DEFAULT-ON additionally needs the open ExitArithOverflow-materialize bug fixed
   (Interpreter.cpp:26339-26350; 2 prior arm64 attempts regressed): canSkipJ2JSave
   is NOT truly bail-free — arith-overflow (and alloc/GC) escape the canBailMidMethod
   count, so a qualifying callee can still bail mid-method. cfibx's bounded incc
   never overflows, but startup callees do.
3. The x86 tier-1 base is bit-rotted (debug_vars.h:208: prescan/emit disagree on
   ~10 bytecodes + a Corrupt-stackPointer miscompile; :200: ~1-word/send caller-
   resume sp leak). Cross-method sends multiply send activity -> amplify the
   pre-existing leak/miscompiles. A clean cross-method run likely needs the base
   hardened first.

NET: the cfibx lever (x86 27x) is a genuine MULTI-SESSION effort (emit-debug +
open-bug-fix + base-harden), not a single-increment win. Implementation kept
default-OFF as a safe foundation; arm64 + x86-default are untouched.

## Revised plan (steps 1-10) — see workflow result for full text

1. debug_vars.h: DEBUG_BOOL(PHARO_T1_X86_XMETHOD) + _COUNTERS.
2. AsmjitT1.cpp ~95: g_emitX86Xmethod flag + static_assert(!V2).
3. compileViaAsmjit ~10177: g_emitX86Xmethod = g_emitX86J2JOk && knob (x86 ifdef).
4. send site 2766-2814: knob-off byte-identical; knob-on admit self-rec OR
   canSkipJ2JSave (byte test, NOT bit 57).
5. save push 2786: keep save[32]=CALLER JM, ip from caller.
6. after depth publish: callee-state writes (calleeJM, method, literals,
   argCount=nArgs) + dynamic nil-fill from calleeJM->tempCount; recompute
   entryAddr before jmp (nil-fill clobbers r10).
7. emitJ2JReturnPrelude_x86 1596-1625: restore caller jitMethod/method/literals/
   argCount (argCount BYTE load) from save[32]=callerJM.
8. counters (fold-independent).
9. this doc.
10. JITMethod.hpp static_assert codeStart()-this==sizeof(JITMethod).
