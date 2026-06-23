# Plan: T1 baseline-JIT codegen — static stack-to-register mapping (option E)

Scoped via a 6-agent design workflow (emit-model + simStack-failure + Cog-reference
+ validation-surface readers → synthesis → adversarial verify). The verify returned
**needs-revision**; this document is the CORRECTED plan incorporating its findings.
Read alongside `docs/results-perfdb.md` (the measured facts that motivate this).

## TL;DR — do the cheap measurement first; it probably says STOP

The technically-right approach (mirror Cog's `StackToRegisterMappingCogit`: a
compile-time static stack-to-register map driven by the EXISTING `BcDepthMap`
depth analysis, replacing the refuted dynamic single-slot `g_tos` cache) is sound.
BUT the adversarial verify established it **shares the flush-at-send ceiling that
refuted the dynamic version**: every send/merge must flush live operand registers
to the frame, so register-residency only helps operands consumed *within a basic
block between sends* — which is rare in send/block-heavy real code (SUnit/soogle).
The honest expected win is **1.5–2.5x on bytecode-bound code, ~0–10% on the goal's
send-heavy workloads.** So Phase 1 is a pure static MEASUREMENT that decides
go/no-go before committing the 11–16 session rewrite. The prior evidence
(`results-perfdb.md`: TOS net-negative on send-heavy code) predicts a likely
NO-GO — and a measured NO-GO is itself a valuable, direction-closing result.

## The approach (if Phase 1 says GO)

Replace the dynamic 1-slot `g_tos`/`g_tosIn` TOS cache (refuted, off by default,
`AsmjitT1.cpp:9849`) with a per-basic-block COMPILE-TIME simStack of deferred-value
descriptors `{BaseOffset(frame slot), Constant(literal), Register(xN), Spilled}`,
exactly like Cog's `CogSimStackEntry`. `pushTemp`/`pushLiteral` emit NO code (push a
descriptor); the load folds into the consuming arith/send. Validity is a
compile-time fact (descriptor identity at a statically-known depth), so we DELETE
every runtime validity bit, `emitTosVerify` (`AsmjitT1.cpp:3916`), the per-bytecode
snapshot-then-clear (`10436`), and the ~30 per-send `tosLrearm` reloads (`9582`) —
that bookkeeping is precisely why the dynamic cache lost.

Correctness contract (Cog's two invariants, verbatim): FLUSH-ALL-TO-FRAME at every
send/exit, FLUSH-ALL-TO-SPILL at every merge/jump-target. Rollout is **per-METHOD
opt-in** (`g_useRegStack = knob && real && mappable`), not per-bytecode-family — a
register stack is only coherent across a whole block. Unmappable methods (inlined
prims `0xEC`, closures, `BcDepthMap` merge conflicts `BcDepthMap.cpp:194`) fall back
to today's exact eager emit, so the binary is green at knob-off and bisectable by
method.

### Corrections the adversarial verify forced (do NOT skip)

1. **`computeDepthMap` is file-private** (anonymous namespace, `BcDepthMap.cpp:34`);
   the header exposes only the `spDepthCheck*` verifiers. Phase 1 must EXPOSE it
   (visibility-only change) before anything can call it — the original plan assumed
   a ready API.
2. **`emitSyncSpToState` (`3839`) is NOT a usable single chokepoint** — it's a
   stateless helper that writes only `state.sp`. Spilling live operands needs the
   per-block simStack threaded into all ~62 arm64 JIT→C++ edge sites; the spill
   logic is DISTRIBUTED, not centralized. The "one chokepoint" correctness argument
   is wrong.
3. **A byte-identical refactor saves nothing.** The arith consumer (`5246-5293`) and
   push stencils (`4910-4967`) are hardwired to memory operands; the real win
   requires rewriting the STENCIL BODIES to take operands from the register map —
   that is the bulk of the 12,581-line emitter (~24 `tosFam` sites, ~10 operand
   families), NOT the push/pop sites. The rewrite is the middle of the project, not
   a late add-on; scope it as such.
4. **`PHARO_SP_DEPTH_CHECK` does NOT cover the dominant risk.** It is
   value-INDEPENDENT (proves sp arithmetic consistency only). The corruption class
   here is value-DEPENDENT: a register-resident operand live across a GC safepoint
   in an inlined prim is invisible to GC (GC/J2J-save read operands from FRAME
   memory). Bring-up needs a value-level differential test vs Cog on an arithmetic
   corpus + a generalized per-slot verify net, not just SP_DEPTH_CHECK.
5. **Phase 4's register pool isn't established** — it must coexist with x25(sp),
   x26, tempBase, x19 (JM identity, asserted every exit `3854`), and send arg regs.
   Confirm a stable callee-saved pool exists BEFORE relying on multi-slot residency.

## Phase 1 (REVISED) — measure the addressable ceiling on REAL workloads (1 session, decisive)

Pure instrumentation, knob-gated `PHARO_T1_REGSTACK_CENSUS`, zero codegen change.
This is the cheap go/no-go the original plan lacked.

- **Expose** `computeDepthMap`/`DepthEntry` + a `depthVectorForMethodOop()` from
  `BcDepthMap` (visibility only); assert it reproduces the existing `g_depthMaps`
  entries bit-for-bit (don't fork the decoder).
- In `emitMethodBytes` (arm64, near the jump-target prescan `9784-9820` and bcLabels
  `10423`), call it, classify mappable/unmappable, and **reconcile against the emit
  loop's own `g_tosJumpTargets` decode** under the census knob. (This directly tests
  the known ExtJump decoder divergence flagged at `BcDepthMap.cpp:196-200` — T1 reads
  int8, the depthmap reads byte+extB<<8. If the assert fires it's a REAL latent T1
  jump bug surfaced for free.)
- Emit a census weighted by COMPILE/EXECUTION FREQUENCY (not static method count):
  (a) hot-mappable %; (b) **removable-load fraction** — operands consumed within the
  same basic block WITHOUT crossing a send/merge (the ONLY operands a static reg
  stack keeps out of memory after the mandatory flush); (c) **nos-resident-arith** —
  binary-arith sites where BOTH operands are block-local (the only win the dynamic
  TOS structurally could never get). Print in `dumpJITStats`, weighted across the
  perfdb benches AND a SUnit-harness run.
- **DONE (binary):** build green; the SUnit harness + perfdb benches print
  `REGSTACK-CENSUS: hot-mappable=P% removable-loads=Q% nos-resident-arith=R per Kbc`;
  the ExtJump-reconciliation assert fires 0 times (or names the divergent method);
  `report-sunit` diff vs the stored Cog baseline = 0; cpu_ms unchanged (REPEAT=5).
- **GO/NO-GO:** proceed to Phase 2 ONLY IF, on the SEND/BLOCK-HEAVY workloads (not
  just a bytecode-bound microbench), `removable-loads Q% × per-load cost ≥ 10% cpu_ms`
  AND nos-resident-arith R is non-trivial. If Q% is high only on the microbench and
  collapses on SUnit/soogle (sends dominate, flush-at-send eats the win — the
  documented case), **STOP**: this reproduces the TOS refutation statically and the
  `results-perfdb.md` "deeper rethink, not this incremental" verdict stands.

## Phases 2–5 (only if Phase 1 is GO)

- **Phase 2 (2–3 sess):** introduce the deferred-value `T1SimStack` and route
  push/pop/arith through it in EAGER mode (materialize immediately). Done =
  byte-identical emitted code for a 50-method corpus (disasm diff) + 0 SUnit
  regressions + 0 SP_DEPTH reports. (Structural refactor; saves nothing by itself —
  proves the model expresses current behavior.)
- **Phase 3 (3–5 sess):** lazy pushes within a block; flush-all at send + block
  boundary. First real memory-traffic win. Done = pushes emit zero loads (disasm) +
  **≥10% cpu_ms on bytecode-heavy bench** (REPEAT=5, non-overlapping CI) + full SUnit
  + SP_DEPTH_CHECK + `PHARO_DET_SCHED=1`: 0 new failures, 0 reports. HIGH risk
  (frame-corruption class re-enters: unspilled descriptor at an unmodeled edge — GC
  safepoint in inlined prim, NLR/exception unwind through a block, J2J save).
- **Phase 4 (3–4 sess):** K=2 register residency to kill the NOS load (the
  `ldr x1,[x2,-16]` the dynamic TOS structurally could never remove) + demand
  spilling. Done = `a+b` in a block emits zero stack memory + additional ≥8% cpu_ms.
  HIGH risk — register-pool coexistence (correction #5) must be settled first.
- **Phase 5 (2–3 sess):** static tempBase residency (kills the per-temp OFF_TEMPBASE
  reload `3889`) + constant-fold special-selector arith on Constant SmallInt
  operands. Needs a value-level differential vs Cog (SP_DEPTH_CHECK can't catch a
  wrong VALUE).

## Horizon, kill criteria, overall risk

- **Horizon:** 11–16 sessions across 5 phases IF Phase 1 is GO. End-state ~1.5–2.5x
  faster than today's T1 on BYTECODE-bound code; **~0–10% on send-heavy SUnit/soogle**
  (flush-at-send ceiling). **NOT Cog parity** — Cog also has decades of stencil/IC/
  inlining tuning untouched here. Honest framing: moves T1 from "2x slower than our
  interpreter" toward "faster than our interpreter on bytecode-heavy code."
- **Kill criteria:** (1) Phase 1 removable-loads collapses on send-heavy workloads →
  STOP (likely, per prior evidence). (2) Phase 3 < 5% cpu_ms despite correct lazy
  pushes → the memory traffic wasn't the bottleneck (same refutation that killed
  dynamic simStack) → abandon. (3) Any phase produces un-root-caused frame
  corruption within one lldb+DET_SCHED session → revert that phase's knob (binary
  stays green via per-method fallback), stop escalating. (4) Phase 4 can't find a
  stable register pool → cap at K=1 (Phase 3) and ship.
- **Overall risk:** HIGH but contained by per-METHOD opt-in with unmappable-fallback
  to today's exact emit (any regression degrades to known-good, never miscompiles),
  the existing VERIFY/POISON/SP_DEPTH nets + a NEW value-level differential, and
  `PHARO_DET_SCHED=1` to defeat the timing-Heisenbug class.

## If Phase 1 is NO-GO (the likely outcome) — where the real gap is

A measured NO-GO closes the operand-stack-codegen direction for good and redirects
to the levers that target the SEND/BLOCK cost the register stack cannot touch (which
is where send-heavy real code spends its time):
- **The send path itself.** Our JIT sends are 3.5x slower than Cog (vs 21x on
  bytecodes) — on send-heavy real code that 3.5x dominates the 7.8x SUnit gap.
  Profiling/optimizing the IC dispatch + activation path is the untouched lever for
  real-code speed.
- **Block activation (fix #2)** — `1M blocks` JIT ~197ms vs interp 33ms; mirror the
  interpreter's zero-alloc `primitiveFullClosureValue` pattern (`results-perfdb.md`).
- Reducing Tier-2's send deopts is NOT viable for tests (it doesn't fire — warmup vs
  activation model; see `results-perfdb.md`).
