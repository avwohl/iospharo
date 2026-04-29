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
4  Monomorphic inlining                     PATTERNS DONE     gated PHARO_SISTA_INLINE_CONST=1
                                                              + PHARO_SISTA_INLINE_SETTERS=1
                                                              for setter shapes.  Coverage:
                                                              const-return + getter + 4/5-value
                                                              setter + self-send chain + ivar-
                                                              self-send.  Setter lowering uses
                                                              bit-63-marked kStoreInstVar →
                                                              jit_rt_store_inst_var helper;
                                                              bytecode-emitted stores still
                                                              SAFETY-BAIL (status quo).  Default-
                                                              on still gated by soak (A3).
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
    **DONE 2026-04-29** (`7efbdac7`, `28efa2b7`, `9cac2634`,
    `6726f6f6`, `caed8d8c`).  All target shapes are now matched in
    `tryInlineConstReturn`:
    - `^ aLiteral` / `^ true|false` (2-value const)
    - `^ instVar` (3-value getter)
    - `^ self` (2-value returnSelf via kLoadReceiver+kReturn)
    - `^ self foo` (2-value self-send chain)
    - `^ self instVar foo` (3-value ivar-self-send)
    - `foo: x  foo := x` (5-value Shape A — implicit returnSelf)
    - `foo: x  ^ foo := x` (4-value return-value setter)
    - `popStoreRcv N; pushTemp 0; ^` (5-value Shape B)
    - `^ arg` parameter passthrough (2-value via kLoadTemp branch)
    Lift-only IR-shape tests in `test_sista_ir.cpp` give the matcher
    a tripwire if the lifter ever reorders kLoadReceiver synthesis
    around stores or returns.

    Setter-shape patterns are gated separately under
    PHARO_SISTA_INLINE_SETTERS=1 because they emit kStoreInstVar in
    the inlined IR.  SistaLowering's setter lowering path is
    selective: bit 63 of the literal marks "from inline" and routes
    to a `jit_rt_store_inst_var` helper invoke (immutability +
    bytes/CompiledMethod + OOB guards + barrier-aware
    storePointerUnchecked); bit-63-clear stores (bytecode-emitted
    via popStoreRcv*) keep the historic SAFETY BAIL.  This avoids
    regressing inner-loop methods that store to ivars while letting
    setter inline emit working code.

    Bench impact 0 on the panel because counted-loop splices already
    replace the hot sends; impact lands when default-on (A3) on
    setter/accessor-heavy workloads (image code, SUnit setUp etc.).

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

- **B-1 (NEW, prereq for everything).**  Helper-based
  `kSendUnspeculated`: today every send bails to interpreter and
  exits compiled code.  With Path 3's cc.invoke shipped, sends
  could become C-helper calls that invoke `sendSelector` and return
  the result, letting compiled code continue.  Without this, even
  optimizations that succeed (B0/B2/INLINE_ARITH) are wasted —
  the method still bails at the first send.  Concrete evidence
  (2026-04-27): with `PHARO_SISTA_INLINE_ARITH=1`, 90k+ bails in
  30s, zero miscompiles, image makes only 200K bytecode steps/sec
  (5× slower than pure interpreter).  Estimate: 1-2 weeks.

  **Design sketch:**

  ```cpp
  // Interpreter.hpp — public method.
  uint64_t jitSistaCallSend(jit::JITState* state,
                              uint64_t selBits,
                              uint64_t nArgs);

  // Interpreter.cpp — implementation.
  uint64_t Interpreter::jitSistaCallSend(jit::JITState* state,
                                          uint64_t selBits,
                                          uint64_t nArgs) {
      Oop* savedSP = stackPointer_;
      uint8_t* savedIP = instructionPointer_;
      Oop savedMethod = method_;
      size_t startFrameDepth = frameDepth_;

      stackPointer_ = state->sp;
      sendSelector(Oop::fromRawBits(selBits), (int)nArgs);

      // For non-primitive sends, sendSelector pushes a frame.
      // Drive the interp loop until that frame returns.
      // For primitive sends that succeed synchronously,
      // frameDepth_ is already back to start — loop is no-op.
      while (frameDepth_ > startFrameDepth && running_) {
          if (!step()) break;
          // Detect NLR-out-of-our-frame: frameDepth_ went BELOW
          // startFrameDepth.  Means a block return jumped past us.
          // Need special handling: signal back to compiled code
          // that we couldn't return normally.  TBD.
      }

      Oop result = stackTop();
      popN(1);
      state->sp = stackPointer_;
      stackPointer_ = savedSP;
      instructionPointer_ = savedIP;
      method_ = savedMethod;
      return result.rawBits();
  }

  // JITRuntime.cpp — extern "C" wrapper.
  extern "C" uint64_t jit_rt_sista_call_send(JITState* state,
                                               uint64_t selBits,
                                               uint64_t nArgs) {
      if (!state || !state->interp) return 0;
      return state->interp->jitSistaCallSend(state, selBits, nArgs);
  }

  // SistaLowering.cpp — replace kSendUnspeculated bail with cc.invoke
  // pattern (load helper to Gp first; ARM64 blr requires reg).
  ```

  **Pitfalls to handle:**

  1. **NLR (non-local return).**  A block in the called method may
     `^` out, popping frames past our `startFrameDepth`.  Detection:
     `frameDepth_ < startFrameDepth` after step().  Response: return
     a sentinel from helper (e.g., 0), JIT-side checks and bails to
     interp at our send bcOffset.  The interp's NLR handling
     continues past us correctly.

  2. **Process switch during step().**  step() does periodic checks
     (timer, signals, preemption every 1024 steps).  If a switch
     happens while we're driving the loop, the new active process
     runs — and our caller's compiled fn is still on the C stack.
     Need to defer process switches until our frame returns OR
     wrap in a non-switch flag.

  3. **GC during called method.**  Allocation in the callee may
     trigger GC.  JIT-side registers holding Oops become stale.
     Mitigation: caller spills all live Oops to interp stack before
     the helper invoke (same pattern as kBlockCreate's full-stack
     spill).  After the helper, reload from interp stack.  This is
     expensive — every send becomes a full-stack-spill point.

  4. **Exceptions.**  Smalltalk exceptions are essentially NLR
     through `signal` chains.  Mitigation: same as NLR — sentinel
     return + bail.

  5. **step() recursion depth.**  step() may itself trigger more
     sends (handled inside the called method).  Each recursion
     adds C stack frames.  For deep nesting (recursive Smalltalk
     methods), this could overflow the C stack before the
     Smalltalk stack overflow handler fires.  Mitigation: cap
     helper recursion depth, bail to interp on overflow.

  Path 3's cc.invoke + the documented helper pattern + Path 3's
  GC-safety lessons all transfer.  Implementation is a focused
  multi-day project once these pitfalls are addressed.

- **B0 (prereq for B2).**  Stop bailing on `PushFullBlock` /
  `PushClosure`.  Emit `kBlockCreate` in IR instead.  DONE
  (94cf819 + 30fa5e6) — kBlockCreate default-on, helper handles
  block creation via cc.invoke.  See
  project_sista_cc_invoke_sigsegv_2026_04_27.md.

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
