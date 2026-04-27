# Sista optimizing JIT — consolidated plan, 2026-04-27

Updates `sista-inlining-plan.md` (646 lines, 2026-04-19).  This
document re-anchors against today's state and re-orders phases by
expected per-bench impact rather than dependency order.

## Context

Bench is now 4-27× slower than stock Cog (sum 1M is 27×).  Stencil
quality + lack of register allocation is the gap.  We have a real
optimizing JIT scaffold (SistaIR + SistaBuilder + SistaLowering,
~5500 LOC) that is 50-60% complete.  Finishing it is the path to
sub-Cog perf — not a clean-sheet rewrite.

## State summary (what's done vs not)

```
Phase                                       Status            Where
0  Foundation decisions                     DONE              docs/sista-phase0-decisions.md
1  IC profiling diagnostic                  DONE              extractInlineHintsForMethod (Interpreter.cpp:14119)
2  Method-level IR + round-trip             DONE              SistaIR.hpp/.cpp, SistaBuilder.cpp (2078 LOC)
3  Deopt infrastructure                     STEP 1 done       Framepoint struct + capture sites
                                                              src/vm/jit/sista/SistaIR.hpp:221+
4  Monomorphic inlining                     STEP 1-3 done     gated PHARO_SISTA_INLINE_CONST=1
                                                              kGuardClass + const-return only;
                                                              pattern coverage is bottleneck
5  Polymorphic inlining + IC specialization NOT STARTED
6  Block inlining                           NOT STARTED       biggest perf lever for sum/sieve
7  Method-redef invalidation                NOT STARTED       blocks safe shipping of 4-6
8  IR optimization passes                   NOT STARTED       constant folding, DCE, peephole
```

Lowering already produces real machine code via asmjit.  Today
SistaLowering handles unspeculated sends, inline arith, branches,
returns.  Inline kGuardClass + kInlineSend works for const-return
callees.

## Re-ordered plan by per-bench impact

The original plan ordered phases by dependency (3→4→5→6).  Reality:
**block inlining (Phase 6) gives the largest single win** because
hot benches (sum 1M, sieve, sort) all use `do:` / `to:do:` /
`whileTrue:` blocks.  Each block invocation today costs 100+ instr
through stencil_sendJ2J + J2J save/restore; inlined it's 5-10 instr.

### Track A: ship the fast path that already exists  (1-2 weeks)

Goal: get current Sista from "compiled, not-default" to "default-on,
visibly faster than T1 for hot methods."

A1. **Phase 3 Step 2 + 3 — finish deopt infrastructure.**  Required
    so guards (kGuardClass, kPrimTagCheckInt) can bail safely when
    speculation fails.  Today the bail mechanism IS deopt for
    unspeculated sends; need to extend to guards with stack
    reconstruction.  Estimate: 1 week.
    Files: SistaLowering.cpp emits landing pads;
    SistaRuntime.cpp handles bail.

A2. **Phase 4 finish — broaden monomorphic inline patterns.**
    Today only const-return callees inline.  Add:
    - 1-bytecode-body callees (`^ self foo`, `^ instVar`,
      `^ aLiteral`)
    - Inline trivial getters/setters (already done by stencil; add
      to Sista IR layer)
    - Inline `^ self` (returnsSelf) callees
    Estimate: 1 week.

A3. **Sista default-on for stable methods.**  Today
    PHARO_SISTA_DISPATCH=1 is opt-in (with PHARO_NO_SISTA opt-out
    after default-on at commit history).  Verify bench passes with
    Sista default and ship.

Exit criterion: 30%+ improvement on dict 50K (which exercises MD
lookup heavily and is the closest current bench to the inlining
sweet spot).

### Track B: block inlining  (4-6 weeks)

Goal: `do:` / `to:do:` / `whileTrue:` runs as a tight loop, not as
a sequence of block invocations.

NOTE (2026-04-27): the original B1 wording assumed `to:do:` shows
up as a real send to lift.  In Pharo, the compiler macro-inlines
`to:do:` / `whileTrue:` / `ifTrue:` at BYTECODE level — they're
already a sequence of branches when the lifter sees them.  So the
actual remaining work in B-track is:

- **B0 (NEW, prereq).**  Stop bailing on `PushFullBlock` /
  `PushClosure`.  Emit `kBlockCreate` in IR instead.  Today the
  lifter bails to interpreter the moment it sees a block literal,
  which prevents any block-aware optimization downstream.
  Estimate: 1 week.

- **B1 (revised).**  SSA-promote loop counters lifted from the
  compiler-inlined `to:do:` pattern: replace
  `kLoadTemp + kPrimAddInt + kStoreTemp` chains with a phi
  induction variable.  Shrinks the per-iteration overhead for
  loops the compiler already emitted.  Bench impact small (the
  cost is in the inner send, not the counter), but unblocks
  later passes.  Estimate: 1-2 weeks.

- **B2.**  Inline `Array do:` (and `OrderedCollection do:`).
  Receiver-class IC says "always Array", block arg is a literal
  with no copied vals → lift `do:` to a counted `at:` loop with
  the block body spliced inline.  This is what actually moves
  sum 1M.  Depends on B0.  Estimate: 2 weeks.

- **B3.**  General non-escaping block inline — block created and
  consumed in the same method.  Generalizes B2.  Depends on B0
  + framepoint plumbing.  Estimate: 2 weeks.

Exit criterion: sum 1M within 5× of Cog (today 27×).

### Track C: polymorphic + IC specialization  (3-4 weeks)

Goal: when an IC site sees 2-3 classes, generate a chained
type-test rather than dispatching through stencil_sendJ2J.

C1. **Phase 5 polymorphic inlining.**  Builder reads multi-entry
    IC and emits a chain of kGuardClass.  Each branch inlines the
    matching callee.  Estimate: 2 weeks.

C2. **IC specialization in IR.**  Today every send becomes
    `kSendUnspeculated` if not in a hint.  Add `kSendCachedClass`
    for IC-known monomorphic sites that aren't worth inlining
    (e.g. cold callees) — emit just the guard + direct call, skip
    the megamorphic probe.  Estimate: 1-2 weeks.

Exit criterion: dict bench within 2× of Cog.

### Track D: invalidation + safety  (2-3 weeks)

Goal: it's safe to keep Sista compilations across image edits.

D1. **Phase 7 method-redef invalidation.**  When a method is
    recompiled (image edit, classBuilder, etc.), invalidate Sista
    compilations that inlined it.  Today this isn't a problem
    because Sista is opt-in for benches; required before default-on
    in production use.  Estimate: 1-2 weeks.

D2. **GC integration audit.**  SistaIR already declares Oop liveness
    in the type system; verify SistaLowering emits proper
    GC-discoverable spill slots (not stack temporaries that escape
    the GC root list).  1 week.

### Track E: IR optimization passes  (ongoing, parallel)

Phase 8 work.  Each pass shrinks generated code or removes work:

- E1 Constant folding (1-2 weeks): kPrimAddInt(C1, C2) → kConstantOop
- E2 Dead-code elimination (1 week): values with no uses + no
  effects get dropped
- E3 Range analysis (2-4 weeks): `i in [1, length]` removes index
  bounds checks in hot loops
- E4 Loop-invariant code motion (2-3 weeks): hoist kLoadInstVar /
  kGuardClass out of loop bodies
- E5 SmallInt narrowing (2 weeks): when both operands are
  kOopSmallInt, generate untagged-int math + retag

## Critical-path ordering

```
A1 (deopt finish, DONE)
  ↓
A2 (mono pattern coverage, DONE — bench impact 0; only 14
     monomorphic IC hints exist across full bench, hot code
     lives in block dispatch not leaf sends)
  ↓
A3 (default-on, DONE)
  ↓
B0 (stop bailing on PushFullBlock — prereq for any block work)
  ↓
B2 (Array do: with literal block) ←─ the actual sum/sieve win
  ↓
C1 (poly inline)             ←─ dict win
  ↓
B3 (general block inline)
  ↓
D1 (method-redef invalidation) ← required before shipping for daily use
```

E* passes can run in parallel with any track from any week.

## Risk register

1. **Sista default-on regressions.**  Memory `project_sista_dispatch_mvp.md`
   notes 4% perf overhead and a bail-blacklist.  Some methods just
   shouldn't go through Sista (large literal frames, weird primitive
   patterns).  Need to keep the blacklist healthy.

2. **Framepoint reconstruction bugs.**  Deopt at a guard miss has to
   reconstruct the interpreter's exact view of stack + temps from the
   IR-level Framepoint.  Off-by-one here causes mysterious crashes;
   memory `project_t2_permute_miscompile.md` has an analogue.

3. **GC during Sista-compiled code.**  Most of our GC bugs in 2026
   were JIT-related stale-pointer issues.  Sista must declare every
   live oop as a GC root via the Type system; SistaLowering must spill
   those to GC-walkable slots before any potential GC point (sends,
   allocs).  An audit pass before Track A3 is mandatory.

4. **Block inlining scope.**  B3 (general block inlining) requires
   escape analysis to know a block doesn't outlive its caller.  Too-
   conservative analysis = no inlining; too-aggressive = miscompile
   on stored-block patterns.  Plan to start with literal-block
   `do:`/`to:do:` (always non-escaping) and grow from there.

## Cost / time estimate

```
Track A       1-2 weeks       (default-on existing work)
Track B       4-6 weeks       (block inlining — biggest win)
Track C       3-4 weeks       (polymorphic + IC specialization)
Track D       2-3 weeks       (invalidation, GC audit)
Track E       ongoing         (IR opts in parallel)
TOTAL         3-4 months wall-clock for a single dev
```

This is consistent with the original plan's "3-6 months" estimate.

## Stop conditions

Worth stopping if:
- Track A doesn't yield 30%+ on dict — means scaffold has a deeper
  bug than worth fixing piecewise.
- Track B yields <2× on sum — means stencil-mode is closer to
  optimal than thought, and gains beyond block-inline are modest.
- Bench within 1.5× of Cog on all benchmarks — diminishing returns.

## Why this plan is real, not aspirational

The repository ALREADY contains:
- 282 lines of IR shape definitions (SistaIR.hpp)
- 2078 lines of bytecode→IR lifter (SistaBuilder.cpp)
- 905 lines of IR→asmjit lowering (SistaLowering.cpp)
- 1377 lines of unit tests (test_sista_ir.cpp)
- 5 sista-* design documents in docs/

This isn't "let's build a JIT" — it's "the JIT exists, finish it."
The phases ordered above are the concrete remaining work.
