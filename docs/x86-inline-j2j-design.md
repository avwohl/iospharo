# x86 inline-J2J — design (synthesized 2026-06-14)

Synthesis of a 5-subsystem map + 3 design approaches + 3 adversarial critiques
(workflow `x86-inline-j2j-design`, 11 agents). It supersedes the WIP's
"materialization is the blocker" framing with the *actual* corruption mechanism
and a corrected, gated, incremental plan.

## Goal

Make **self-recursive** JIT→JIT calls run JIT→JIT **directly** on x86 (push a
save + branch to the callee body in the JIT), instead of returning to the C++
chain loop for every send. x86 JIT recursion currently measures ~interp speed
because each self-send round-trips through the chain loop's per-send
`JIT_CALL` + C++ bookkeeping (~90 insn/send). arm64 gets its larger J2J win
this way (trampoline-driven). Scope: **self-recursive only** (`cached
methodBits == OFF_METHOD`), one opted-in method at a time, behind the existing
default-off gates.

## What already exists on x86 (and must NOT regress)

x86 already HAS J2J via the **C++ chain loop** in `Interpreter::tryJITActivation`
(`Interpreter.cpp:24718-25008`, the `#else` of the arm64-only trampoline branch
at :24688). It is load-bearing — 2242 SUnit tests pass through it. Key facts
(from the subsystem map):

- The chain loop converts `ExitSendCached`→`ExitJ2JCall` (`upgradeICToJ2J` +
  `pharo_jit_convert_send`), then on `ExitJ2JCall` pushes a **V1 J2JSave** into
  a **C++ local** `j2jStack[j2jDepth++]` (`:24776`), increments only
  `localFrameDepth` (lazy frame — no `SavedFrame` write), sets up the callee in
  `state`, and `JIT_CALL`s the callee body. On `ExitReturn` it pops, restores
  the caller, does the V1 stack fixup (`sp[-(n+1)]=retVal; sp-=n`), and resumes.
- The loop's depth counter is a **C++ local `int j2jDepth = 0`** (`:24675`).
  On x86 it is **never seeded from `state.j2jDepth`** — the
  `localFrameDepth - j2jBaseFrameDepth` recovery (`:24706`) is inside
  `#if __aarch64__` only.
- On a genuine bail it **materializes** pending saves into `savedFrames_` via
  the arch-neutral walk at `:25047-25119`, which calls
  `materializeJ2JSaveIntoFrame` (`:23889`). That converter is already
  **V1/x86-ready** (reads `save.jitMethod` or falls back to `state.jitMethod`,
  `save.ip`, `save.sendArgCount`; writes `savedIP/savedMethod/savedReceiver`,
  `savedFP = save.tempBase-1`, `materializedRetSlot = save.sp-(n+1)`).
- The save pool is one `std::array<J2JSave,1024> j2jPool_` with a C++ index
  `j2jPoolCursor_` (GC floor: `forEachRoot` scans `j2jPool_[0..j2jPoolCursor_)`,
  `Interpreter.hpp:3599`) AND in-JIT byte pointers `state.j2jSaveCursor/Limit/
  EntryCursor`. With split-pool OFF (default) `j2jStateBase == j2jPoolBase`, so
  **the in-JIT cursor view and the chain loop's `j2jStack` index view alias the
  SAME backing slots.**

## The corruption mechanism (corrected)

The WIP "cursor reset / recursive tryJITActivation" diagnosis was wrong in its
specifics (`:24290` is trace code; `tryJITActivation` is not called recursively
from the chain loop). The critiques pinned the real cause:

1. **Inline saves are invisible to the C++ side.** The inline push bumps only
   `state.j2jSaveCursor`, never `state.j2jDepth`. But every C++ consumer
   iterates `state.j2jDepth` (the int32 at OFF=160) — the exit merge (`:25016
   if state.j2jDepth > j2jDepth`), the materialize-on-bail walk (`:25047`), the
   GC ip-rebuild bound. And `j2jDepthFromCursor()` hardcodes `/32` (V2 size),
   wrong for x86's 56-byte V1. So even on a clean exit the inline saves are
   silently dropped.
2. **The chain loop OVERWRITES inline saves (the decisive hole).** When a deep
   self-recursive inline frame hits a cold / IC-miss / non-self send, the JIT
   `ret()`s to C++ with `ExitSendCached` and the inline saves pending in the
   slice. Control enters the chain loop with its local `int j2jDepth = 0`
   (never seeded from `state.j2jDepth` on x86) and writes
   `j2jStack[j2jDepth++]` = `j2jStack[0]` — which, split-pool OFF, **is the same
   backing slot as inline save #0**. The caller chain is clobbered. This is one
   `state`, one un-advanced local depth, two writers to `&j2jPool_[base]` — no
   recursive `tryJITActivation` involved. This is why rfib collapses toward
   rfib(n-1).

## The design (synthesized — the correct parts of B+C, with the critique fix)

Three couplings, all inside the existing default-off gates
(`g_emitX86J2JOk` = master knob && selector match). The existing materializer
is reused unchanged.

> STATUS 2026-06-14: **Coupling 1 DONE (commit 8e26d711) — and it alone made the
> self-recursive inline-J2J CORRECT**, better than this design predicted (we
> expected coupling 2 needed for correctness). rfib(20/25/28/30) and `tak`
> (3-arg, 3-site) are correct COLD and under DET_SCHED; the WIP previously
> collapsed them. Likely because the published depth lets the existing
> materialize-on-bail (:25047) convert inline saves, and the `j2jStack[0]`
> clobber the critiques feared does not manifest for these interleavings (cold
> ExitSend sends land at C++ boundaries with no pending inline saves). Coupling 2
> may still be needed for other call shapes / wider coverage. PERF: marginal —
> inline fires (~830K fewer chain-loop calls on rfib(30)) but x86's baseline is
> the efficient chain loop, not full activation, so the per-send win is below
> wall-clock noise; ~30% of sends go inline. Knob-off byte-identical (no
> regression); arm64 clean.

**Coupling 1 — publish depth (small, verified knob-off-safe).**
The inline send-site push (`AsmjitT1.cpp` ~:2774) increments `state.j2jDepth`
(`dword[rdi+OFF_J2J_DEPTH(160)] += 1`); the return prelude
(`emitJ2JReturnPrelude_x86`) decrements it on pop. Emitted ONLY inside the
`g_emitX86J2JOk` blocks, so non-opted methods are byte-identical. This makes the
existing merge (`:25016`) and materialize-on-bail (`:25047`) SEE the inline
saves. (No x86 stencil writes `state.j2jDepth` today, so any
`if (state.j2jDepth>0)` guard is a guaranteed no-op when the knob is off.)

**Coupling 2 — stop the chain loop clobbering inline saves (the real fix).**
On x86 entry to the tight chain loop, **seed the local `j2jDepth` from
`state.j2jDepth`** (and `localFrameDepth` accordingly), so
`j2jStack[j2jDepth++]` starts ABOVE the live inline saves instead of at
`j2jStack[0]`. Equivalently: have the chain loop treat `state.j2jDepth` as the
single source of truth for the current J2J depth on x86 (the arm64 path already
recovers depth from `localFrameDepth`; x86 must recover it from
`state.j2jDepth`). Also fix `j2jDepthFromCursor()` to use the V1 56-byte stride
on x86 (it hardcodes 32).

**Coupling 3 — bail materialization (reuse, no new code).**
With couplings 1+2, a bail from an inline chain leaves `state.j2jDepth = N`
inline saves in the slice below `j2jPoolCursor_` (GC-covered), and the existing
`:25047` walk + `materializeJ2JSaveIntoFrame` convert them to real
`savedFrames_`. The inline push must write the **real** `save.jitMethod`
(`OFF_JITMETHOD`, not 0) — the current WIP already does. The
`entryDepth/entryCursor` pin (`:24898`) already prevents a callee's return
prelude from popping an outer frame's saves; keep that contract (the return
prelude's only discriminator stays `cursor > entryCursor`).

## Validation gates (each keeps the working JIT correct; A/B vs interp oracle)

- **G0 — knob-OFF regression floor.** Build, run the battery + a 7-class SUnit
  batch (wall-clock AND `PHARO_DET_SCHED`). MUST equal the current 0-fail
  baseline byte-for-byte. Proves all new code is dead when off.
- **G1 — coupling 1 only, forced clean exit.** Knob on, `SEL=rfib`, a clean
  no-cold-send scenario; confirm `state.j2jDepth` tracks and rfib(20)/rfib(28)
  match the known Fibonacci values. Value oracle: rfib(28)=317811 (collapse
  shows rfib(28)=1).
- **G2 — coupling 2, warmup mix.** Full rfib(28) (forces cold/warm site mixing);
  MUST equal 317811, proving the chain loop no longer clobbers inline saves.
- **G3 — bail materialization.** A self-rec method that DOES bail mid-chain
  (e.g. allocates, or `PHARO_DET_SCHED` yields) — confirm correct result +
  no `[WEDGE]`/DNU, proving `:25047` materializes inline saves.
- **G4 — broad soak.** SEL pointed at a hot startup self-rec method; full SUnit
  batch + DET_SCHED; 0 new failures. Then measure rfib/cfib speedup vs knob-off.
- **G5 — default-on flip** (only after G0-G4 clean over multiple methods):
  drop the per-selector gate, keep the master knob; full suite + benches.

## Risk to the working JIT

Three layers, all verified by the critiques:
1. Compile-time gate `g_emitX86J2JOk` (`AsmjitT1.cpp:2749, 1585`) — false unless
   selector matches; the send-site fast path AND return prelude (incl. the
   depth RMWs) are **not emitted** for non-opted methods → byte-identical
   codegen. Strongest layer.
2. The coupling-2 chain-loop change must be x86-only and a no-op when
   `state.j2jDepth == 0` (always true for chain-only methods), so seeding
   `j2jDepth = state.j2jDepth` = `j2jDepth = 0` — inert for the 2242-test path.
3. `j2jDepthFromCursor` V1-stride fix is x86-only (`#if !PHARO_J2J_SAVE_V2`).

## Effort & honest sequencing

Critiques' realistic estimate: **5-8 focused sessions** for a correct, soaked,
default-on result (not the proposals' optimistic 1-2 days — those under-scoped
the `j2jStack[0]` clobber). Coupling 1 alone is ~1-2 hours and is a safe,
self-contained first step. Coupling 2 is the hard part (touching the
load-bearing loop) and needs the most validation. This is the non-shipping arch
with an incremental win over already-working chain-loop J2J, so it should be
run as a dedicated effort with the gates above, not rushed.

## Open risks the critiques flagged (must be designed around)

- **GC window:** the depth RMW + cursor bump must keep `j2jPoolCursor_ >=` the
  written slot at all times so `forEachRoot` covers in-flight saves (GC can't
  fire mid-JIT today — "no allocation in stencils" — but coupling 2's seeding
  runs in C++ where it can).
- **NLR / exceptions** unwinding through an inline chain: the materialized
  frames must present the same `savedFrames_` shape the unwinder expects
  (`materializeJ2JSaveIntoFrame` already builds these; verify for inline
  origin).
- **`materializedRetSlot`** must be `save.sp-(nArgs+1)` for the return value to
  land correctly after materialization (the converter does this; the inline
  save's `sp` must be the pre-call sp, which the WIP push stores).
- The outer resume loop's "inline one-shot J2J" (`:26566-27228`) is a SEPARATE
  J2J driver; ensure the inline path and it don't both fire for one site.
