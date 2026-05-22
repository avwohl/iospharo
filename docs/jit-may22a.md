# jit-may22a — B1 full lowering of kSendInlineSelf

Plan to take the `kSendInlineSelf` IR scaffolding (landed in
commits `ca6bfdfb` + `e68aa61d`) from "falls through to
kSendCallHelper" to a real inline tail-call that bypasses
`jit_rt_sista_call_send` entirely.

## Implementation progress (2026-05-22)

| Sub-step | Status | Commits |
|---|---|---|
| 1: Save-stack data structures | **DONE** | `6e0177cc` |
| 2: Helper lowering (gated) | **DONE** | `2be9875f` |
| 3: Full fib(28) perf measurement | **blocked** by GC-hint loss | — |
| 4: Deopt walking | **implicit** (each save level self-pops) | — |
| 5: GC visiting save receivers | **DONE** | `7f606a05` |
| 6: Default-on flag flip | **deferred** until 3 works | — |

What's done: full IR-to-emit-to-helper plumbing.
`PHARO_SISTA_INLINE_SELF=1` enables the new helper; off (default)
falls back to `jit_rt_sista_call_send`.  Bench-correctness fib
20/28/30 PASS in all 3 modes (default, bail-only without
inline-self, bail-only with inline-self).

What's blocking Sub-step 3 (the actual perf win):
- Sista's cache is cleared on every GC (raw oop keys go stale
  across Spur compaction).  After GC, benchFib's hints-bearing
  compile gets re-compiled without hints (because the recompile
  fires before T1 IC re-extracts hints), losing `kSendInlineSelf`
  emit.  Net: my helper rarely gets routed even with the flag on.

Resolving Sub-step 3 requires either:
1. Sista cache survival across GC (Phase 5-style oop-update
   walking).
2. Forcing immediate re-extract of hints after GC.
3. Caching the per-method `kSendInlineSelf` site list
   externally — re-applied on every compile regardless of hints.

Each is multi-day Sista-side work.  Out of scope for this iteration.

The hardest part of B1 — the save-stack contract that doesn't
clobber Sista's existing deopt path — is solved by **using a
runtime helper instead of a true inline BR**.  The helper's
recursive `fn(state)` call grows the C stack by one frame per
level (~10 KB max for fib(28) depth 28), but avoids the asmjit
Compiler API complexity of self-recursive inline branches.


## Where we are

Status when this plan was written:

- `Op::kSendInlineSelf` declared in `SistaIR.hpp` with OpInfo
  entries.
- `LinearLifter::setSelfMethodBits(rawBits)` wired into
  `Builder::build`; the lifter knows what "self" is.
- Recogniser fires at both `kSendCallHelper` and `kSendUnspeculated`
  (Send0/1/2 default path) emit sites — when an `InlineHint` at the
  current `bcOffset` has `targetMethod == selfMethodBits`, the
  builder emits `kSendInlineSelf` instead.
- `asmjit-T1` populates `sendSiteMap_` via
  `JITCompiler::setSendSiteBCOffsets` so
  `extractInlineHintsForMethod` works for asmjit-T1-compiled
  callers.
- Lowering (arm64 + x86_64): `kSendInlineSelf` falls through to
  the same case as `kSendCallHelper` — calls
  `jit_rt_sista_call_send` via `cc.invoke`.

End-to-end verified: `benchFib`'s recursive send at `bcOff=10`
fires `isSelfRec=1` with the hint pointing back to benchFib's own
oop bits.  The IR op shows up as `send_inline_self` in dumps.

No perf change yet — Sista's per-send helper at 9 ns/send is still
on the critical path.

## Why this is the only remaining piece

Bench numbers as of jit-may20b's final state, gate-OFF steady-state:

    benchmark         ours       cog      gap
    fib(28)            8 ms      3 ms     2.5×

Cog's win comes from collapsing the benchFib recursion into
machine-code that handles each level without leaving the JIT.
Inline-J2J already does this via the J2JSave protocol — and on
asmjit-T1's path, fib(28) runs at 2.7 ns/send (within striking
distance of Cog's 1 ns/send).

Sista has no equivalent.  Its `kSendCallHelper` goes through
`jit_rt_sista_call_send`, which calls `sendSelector` + drives
`step()` to completion.  Even with `kMaxSistaHelperDepth=64`
(jit-84 B1 partial), each level costs the helper's frame-save +
sendSelector overhead.

B1's full lowering closes this gap by making Sista's recursive
call as cheap as inline-J2J's: BR to the method's own JIT entry
with a save-stack push on the way in and a pop on the way out.

Total estimated effort: **2-3 weeks** for one engineer, mostly
correctness validation under deopt + framepoint replay.

## Where the inline call must land

Sista's `kSendCallHelper` arm64 lowering at
`SistaLowering_arm64.cpp:1140`-`1280` ends with:

    invokeNode = cc.invoke(state, selReg, nArgsReg)
        → returns dst (the result or 0 on deopt)
    cbnz dst, noDeopt
    [deopt path: framepoint replay → ExitSend at bcOffset]
    bind(noDeopt)
    push dst onto IR stack

The new `kSendInlineSelf` path replaces the `cc.invoke` with an
inline BR sequence, but **must preserve** the same exits:

1. **Normal return**: result pushed; lift continues past the send.
2. **Deopt (callee bailed, returned 0-equivalent)**: framepoint
   replay; ExitSend at the source bcOffset.

The simplest reading: the recursive callee is the same Sista fn
that's currently executing.  So "call self's entry" means
"recurse into our own compiled body".  The save-stack protocol
makes this work without blowing the C stack.

## Save-stack design

Mirror inline-J2J's `J2JSave` discipline.  Each entry holds:

    struct SistaSave {
        Oop* sp;             // caller's state.sp at push time
        Oop  receiver;       // caller's state.receiver
        Oop* tempBase;       // caller's state.tempBase
        uint8_t* ip;         // caller's state.ip (for null-resume bail)
        uint32_t bcOffset;   // source bcOffset (for deopt replay)
        uint32_t _pad;
        uint8_t* resumeAddr; // arm64 address to BR back to on return
    };  // 56 bytes (aligned to 16)

Allocated as a fixed pool inside `JITState` (mirroring `j2jPool_`
or carved as a slice).  Push at inline-call entry, pop at inline-
return prelude.

Design choices that need pinning down before code:

1. **Shared with J2J save stack?**  Pro: less memory.  Con:
   inline-J2J's prelude could pop a Sista save (and vice versa).
   Recommend: separate pools for clear semantics.

2. **Pool size**: inline-J2J's pool is 1024 entries.  fib(28) max
   depth 28, plenty of margin.  Start at 256, opt-in larger via
   env.

3. **GC visiting**: `SistaSave::receiver` is an Oop root.
   `forEachRoot` must visit live saves (mirror line 3223 in
   `Interpreter.hpp`).

4. **Deopt walking**: when an inner call deopts, the outer
   compiled-fn's framepoint replay must restore its caller's
   state.  Sista's existing framepoint walker doesn't traverse
   the save stack — extending it is the trickiest sub-step.

## Lowering sketch (arm64)

For `case Op::kSendInlineSelf:` in `SistaLowering_arm64.cpp`:

    Label normalReturn = cc.new_label();
    Label deoptHere = cc.new_label();

    // Save-stack overflow check
    Gp save = cc.new_gp64("sista_save");
    cc.ldr(save, ptr(state, OFF_SISTA_SAVE_CURSOR));
    Gp limit = cc.new_gp64();
    cc.ldr(limit, ptr(state, OFF_SISTA_SAVE_LIMIT));
    cc.cmp(save, limit);
    cc.b_hs(deoptHere);  // full → deopt

    // Push save: state → save
    cc.ldr(scratch, ptr(state, OFF_SP));
    cc.str(scratch, ptr(save, offsetof(SistaSave, sp)));
    cc.ldr(scratch, ptr(state, OFF_RECEIVER));
    cc.str(scratch, ptr(save, offsetof(SistaSave, receiver)));
    cc.ldr(scratch, ptr(state, OFF_TEMPBASE));
    cc.str(scratch, ptr(save, offsetof(SistaSave, tempBase)));
    cc.ldr(scratch, ptr(state, OFF_IP));
    cc.str(scratch, ptr(save, offsetof(SistaSave, ip)));
    cc.mov(scratch, bcOffset);
    cc.str_w(scratch, ptr(save, offsetof(SistaSave, bcOffset)));
    cc.adr(scratch, normalReturn);
    cc.str(scratch, ptr(save, offsetof(SistaSave, resumeAddr)));
    cc.add(save, save, sizeof(SistaSave));
    cc.str(save, ptr(state, OFF_SISTA_SAVE_CURSOR));

    // Set up callee state.  For self-rec: receiver = arg[0],
    // tempBase = caller_sp - nArgs*8.  state.method, state.ip,
    // state.literals stay (same method).
    cc.ldr(rcvr, ptr(state, OFF_SP), - (nArgs + 1) * 8);
    cc.str(rcvr, ptr(state, OFF_RECEIVER));
    cc.ldr(sp, ptr(state, OFF_SP));
    cc.sub(newTempBase, sp, nArgs * 8);
    cc.str(newTempBase, ptr(state, OFF_TEMPBASE));
    // state.ip = bcStart of this method
    // (compute from state.method + offsetof bytecode start)
    ...

    // BR to our own entry.
    cc.br(method_entry_label);

    bind(normalReturn);
    // Caller resumes here.  state.sp has been adjusted by the
    // return prelude to the post-send position; state.receiver/
    // tempBase/ip have been restored from save.  Push the
    // returnValue onto the IR stack.
    cc.ldr(resultReg, ptr(state, OFF_RETVAL));
    push_to_ir_stack(resultReg);

    bind(deoptHere);
    // Pool-full or other failure → framepoint replay + ExitSend
    [standard kSendCallHelper deopt path]

## Return prelude

When Sista's compiled fn is about to execute `kReturn`, check
whether we're inside a save-stack frame (j2jDepth-style
comparison, but Sista-side).  If yes, pop the save, restore
caller's state, write the return value at the receiver slot,
and BR to `save.resumeAddr` — bypassing the trampoline.

The asmjit-T1 inline-J2J return prelude at
`AsmjitT1.cpp:2557-2634` is the template.

## Deopt path correctness

When an inner self-rec deopts (helper-NLR, kGuardClass miss,
overflow), the framepoint walker needs to:

1. Identify that we're inside a `kSendInlineSelf` frame.
2. Pop ALL accumulated Sista save entries down to the framepoint's
   recorded save-cursor depth.
3. Restore caller's state from the bottom save.
4. ExitSend at the OUTERMOST send's bcOffset (so the interpreter
   re-executes from there).

Without this walking, a deep deopt leaves stale Sista saves on
the stack — next push misuses them.

Implementation in `SistaLowering_arm64.cpp`'s deopt fall-through
path: emit a small loop that decrements `state.sistaSaveCursor`
until it matches the framepoint's saved cursor.

## Validation plan

Each sub-step paired with a measurement:

1. **Save-stack push/pop without recursion** — emit the save but
   never BR; verify state restored correctly on return.  Run
   bench-correctness `fib 20/28/30`; expect PASS.

2. **One-level recursion** — BR to method entry, but the entry
   immediately returns without re-recursing.  Verify the save's
   `sp/receiver/tempBase/ip` round-trip correctly.

3. **Full benchFib(28)** — run the canonical bench with this
   path active.  Target: ≤ 4 ms (within ~30% of Cog's 3 ms).
   Compare against inline-J2J's 8 ms.

4. **Deopt under recursion** — force a deopt inside the recursive
   call (PHARO_SISTA_HELPER_FORCE_BAIL=1 or equivalent).  Verify
   the save stack unwinds correctly and interp resumes at the
   outer bcOffset.

5. **GC during recursion** — trigger a scavenge mid-call.  Verify
   `forEachRoot` visits the save-stack receivers (otherwise stale
   oops surface as DNUs on resume).

## Risk register

| Risk | Mitigation |
|---|---|
| Save stack races with Sista's existing framepoint walker | Add save-stack walking to framepoint replay (deopt path); validate every Sista test passes |
| C-stack growth via Sista's nested `step()` callers if any path still hits the helper | Audit: with `kSendInlineSelf` lowered, no caller of `jit_rt_sista_call_send` for self-rec sends.  Confirm via PHARO_SISTA_HELPER_FORCE_BAIL=1 hits zero |
| `state.ip` lost across the recursive call | Save `state.ip` in the SistaSave; restore on return prelude (template: AsmjitT1.cpp:2555-2580) |
| State corruption when `kSendInlineSelf` is wrong (polymorphic site falsely classified as self-rec) | The recogniser already checks `hint.targetMethod == selfMethodBits` — false positive impossible unless the IC entry is mis-filled.  Add a runtime assert on first activation (debug build) |
| Bench-suite reliability regression | Run `scripts/run_benchmarks.sh` after each sub-step; gate the whole feature behind `PHARO_SISTA_INLINE_SELF=1` until stable |

## Execution order

```
Sub-step 1 (save-stack types + push/pop without recursion)
  ↓
Sub-step 2 (single-level recursion + return prelude)
  ↓
Sub-step 3 (full fib(28) — first perf measurement)
  ↓
Sub-step 4 (deopt walking)
  ↓
Sub-step 5 (GC visiting)
  ↓
Sub-step 6 (default-on the env flag; run full bench-suite)
```

Estimated wall-clock for each sub-step:

- Sub-step 1: 2-3 days (data structures, RAII guards, header fixups).
- Sub-step 2: 3-5 days (return prelude is non-trivial).
- Sub-step 3: 1-2 days (mostly waiting for lldb crashes).
- Sub-step 4: 3-5 days (deopt walking is the riskiest).
- Sub-step 5: 1-2 days.
- Sub-step 6: 1 day for the flag flip; soak time on top.

Total: 11-18 days of focused engineering + soak time.

## Files this touches

Files to be modified:

- `src/vm/jit/sista/SistaLowering_arm64.cpp` — main lowering work.
- `src/vm/jit/sista/SistaLowering_x86_64.cpp` — parallel emit (or
  keep the fall-through if x86 isn't a target right now).
- `src/vm/jit/JITState.hpp` — add `sistaSaveCursor` / `sistaSaveLimit`
  fields + offset asserts.
- `src/vm/Interpreter.cpp`'s `forEachRoot` — visit Sista save
  receivers.
- `src/vm/jit/sista/SistaRuntime.{hpp,cpp}` — pool sizing /
  initialization (mirror j2jPool_).
- `src/vm/DebugSettings.{hpp,cpp}` — `PHARO_SISTA_INLINE_SELF=1`
  flag, default off until validated.
- `docs/jit-may20b.md` and `docs/jit-84.md` — close out the
  Step 8.4 / B1 sections when this lands.

Files to read (no modifications):

- `src/vm/jit/asmjit/AsmjitT1.cpp:3676-3801` (inline-J2J push:
  template for save-stack push).
- `src/vm/jit/asmjit/AsmjitT1.cpp:2555-2634` (inline-J2J return
  prelude: template for return).
- `src/vm/Interpreter.hpp:580+` (`SavedFrame` + `materializedRetSlot`:
  what Step 7's matRetSlot fix added that the deopt path may need).

## Open questions for the next implementer

1. Do we want the Sista save pool to share j2jPool_ slots (single
   carving + handover discipline) or have its own backing?
   Recommendation: own backing — the discipline is simpler and the
   memory is cheap.

2. Should `kSendInlineSelf` lower the same way on x86_64 + arm64,
   or x86 stays on the helper path?  asmjit-T1 is arm64-only
   today; if the priority is just fib(28), arm64-only is fine.

3. The recogniser today only fires at `kSendCallHelper` and the
   default Send0/1/2 emit.  Should we extend to `kSendCallHelperSpecial`
   (SpecialSend variant) too?  benchFib's recursive send is a
   regular literal send, not SpecialSend, so this is optional.

## How to validate the win

When B1 fully lowers, expected results:

    benchmark        before    after    target
    fib(28) gate-OFF   8 ms    ≤ 4 ms   3 ms (Cog)

    bench-suite reliability  20/20 PASS  same  (Step 7 stays)

If after Sub-step 3 fib(28) is still > 6 ms, the helper-overhead
hypothesis was wrong and B1's value is smaller than estimated —
re-evaluate before continuing to Sub-steps 4-6.

## What this UNLOCKS

After B1 lands, additional follow-ups become cheap:

- **Cross-method inlining** in Sista: same save-stack protocol
  generalises from "BR to own entry" to "BR to callee's entry"
  with a JITMethod-lookup step.  Step 8.4's SISTA_BIT dispatch
  becomes a real win.

- **Polymorphic self-rec**: relax the `targetMethod == selfMethodBits`
  check to also recognise IC hints to a small set of known-stable
  methods (e.g., type-tested integer ops in a loop body).
