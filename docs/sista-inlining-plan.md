# Sista-style Method Inlining — Plan

Written 2026-04-19.  Architectural rewrite of the JIT to actually close
the 30× Cog gap.  Complementary to (not replacing) the docs in
`docs/jit-toolkit-evaluation.md` and `docs/benchmark-results.md`.

## Status (2026-04-19)

- **Phase 0: architecture decisions** → DONE (`14d7b8f`).  See
  `docs/sista-phase0-decisions.md` — eight decisions locked:
  bespoke SSA-lite IR, two-tier with Tier 1 kept as fallback, lazy
  deopt reconstruction, per-method dependency tracking for
  invalidation, scope-locked Phase 4 to monomorphic only,
  `PHARO_FORCE_DEOPT_FRACTION` random-deopt tester, Phase-4
  fib(28) ≥3× speedup exit criterion.
- **Phase 1: IC profiling** → DONE (`6539046`).  Existing ICs are
  already 6-way polymorphic — no widening needed.  Added
  `JITRuntime::dumpICHistogram()` for empirical validation;
  tinyBenchmarks shows 100 % of non-empty sites are monomorphic
  (10 / 10).  Sista premise validated for this workload.
- **Phase 2: method-level SSA-lite IR** → IN PROGRESS (2026-04-19).
  Multi-block control flow + inline arith + unspeculated sends +
  phi nodes + medium-distance jumps now work.  21 round-trip tests
  passing:

      Bytecodes lifted & lowered:
      - PushReceiver, PushTemp 0..11, PushRecvVar 0..15,
        PushLitConst 0..31
      - PushTrue, PushFalse, PushNil, PushZero, PushOne
      - PopStoreTemp 0..7, PopStoreRecvVar 0..7, Pop, Dup
      - ReturnReceiver, ReturnTrue, ReturnFalse, ReturnNil, ReturnTop
      - Short jumps 0xB0-0xC7 (unconditional + branch-if-true/false)
      - ExtendA / ExtendB (0xE0 / 0xE1) prefix bytes
      - ExtJump / ExtJumpTrue / ExtJumpFalse (0xED-0xEF) forward only
      - Arith + - * on SmallInt operands (tag-preserving, no overflow
        check yet)
      - Send0/1/2 (0x80-0xAF) as ExitSend bail — interpreter takes
        over at the send; compiled code never resumes

      IR ops implemented:
      - kConstantOop, kLoadReceiver, kLoadTrueOop, kLoadFalseOop
      - kLoadTemp, kLoadLiteral, kLoadInstVar
      - kStoreTemp, kStoreInstVar
      - kReturn, kBranch, kBranchIfTrue, kBranchIfFalse
      - kPrimAddInt, kPrimSubInt, kPrimMulInt
      - kSendUnspeculated (via ExitSend bail; real speculation is Phase 4)
      - kPhi (SSA merge at blocks with non-empty entry stacks)

  Commits: `c638471` (IR), `42e382c` (lifter MVP), `7fa2fab`
  (lowerer + first round-trip), `2e5d61e` (push-constants +
  push-literal), `a09b5c4` (ivar + store-temp + stack ops),
  `6badd08` (setter pattern), `8680ccb` (multi-block via short
  jumps), `7c57dbe` (arith + - *), `eed3d37` (PHARO_JIT_ENABLED
  guard for xcframework link), `379e3f7` (unspeculated sends),
  `6053428` (phi nodes for merge blocks),
  `7b98ee5` (ExtendB + ExtJump forward for medium-distance jumps).

  Lifter now does two passes: pre-scan for branch targets and
  post-terminator boundaries, create one block per offset, then
  lift each block.  Lowerer creates per-block asmjit labels and
  emits branches against state->trueOop comparison.  Phi-node
  support is deferred — current restriction is empty simulated
  stack at every block boundary, which covers the common
  if-then-else-return pattern.

  Remaining Phase 2:
  - Arith overflow / non-SmallInt bail.  Today's lowerer assumes
    SmallInt and no overflow.  Needs a guarded fast path: tag
    check on inputs + overflow flag (`adds` / `subs`) on the
    math, bail to `kSendUnspeculated` on miss.  Gated on Phase 3
    deopt.
  - Backward jumps (loops).  Currently forward-short-jump-only;
    loops use ExtJumpLong.  Needs BFS-style entry-depth
    propagation instead of the current offset-order pass.
  - mustBeBoolean bail on conditional branch (placeholder today
    silently falls through on non-true cond; fix requires Phase 3
    deopt).
  - Runtime integration (Phase 2.3).  Hook the Sista compiler into
    the tier-up path so it's actually called by the VM.  Currently
    the IR exists and produces machine code that passes unit
    tests, but nothing in the runtime invokes it.  `state.ip` on
    send bail is set to the bytecode offset as a uint64_t, not the
    absolute pointer the interpreter expects — that translation
    needs to happen at hookup time when we know the CompiledMethod
    base.

  Each remaining item gates on a round-trip test + SUnit
  no-regression.  Catalyst + all three iOS xcframework slices
  build clean as of `eed3d37` (Sista lowerer guarded behind
  `PHARO_JIT_ENABLED` to match Tier 2's pattern; stubs returned
  on JIT-off slices).
- **Phase 3: deopt infrastructure** → queued.
- **Phase 4: monomorphic inlining** → queued.  First expected
  speedup.  Exit-condition gate.
- **Phase 5-8** → later.

---

The current JIT (copy-and-patch stencils via asmjit) is architecturally
capped near interpreter speed on arith-bound workloads.  Root cause:
~460 bytes of native code per bytecode, ~200+ indirect-branch targets
per hot method, per-stencil register save/restore.  No code-generator
swap (MIR → asmjit) addressed that, because the code generator isn't
the bottleneck — the *shape* of what we emit is.

Sista-style (Pharo's post-2018 production JIT) closes the gap by
inlining hot callees into callers at JIT time.  This erases send
boundaries: no IC probe, no stencil prologue / epilogue, no indirect
branch between bytecodes of inlined regions.  For a `do:` loop over
an Array with a 3-bytecode block body, Cog compiles the entire loop
to ~20 ARM instructions; our current JIT compiles it to ~13 KB.


## What we'd reuse from existing work

- **asmjit backend.** Code emission stays.  Inlining is an IR-level
  transform above code gen.
- **Inline-cache infrastructure.**  99 %+ hit rate on hot paths.  The
  type feedback needed for speculative inlining is a small extension
  (polymorphic slots instead of monomorphic).
- **GC safety.**  We already track live oops at JIT boundaries for
  GC compaction.  Inlining doesn't change GC semantics.
- **Existing deopt mechanism.**  `ExitSend` / `ExitArithOverflow` /
  primitive-failure unwinding already reconstruct interpreter state
  at send boundaries.  Extending to inlined-frame deopt is building
  on that skeleton, not starting from scratch.
- **Stencil JIT.**  Kept as the fallback for cold code.  Only hot
  methods get the optimizing compiler.  Two-tier arrangement is
  standard (Cog has Sista + the copy-and-patch fallback).

## What's architecturally new

- A **method-level IR** (currently we compile bytecode-by-bytecode; no
  representation of the method as a whole).
- A **dependency tracker** for invalidation: compiled method → methods
  inlined.  When any inlined-from method is redefined, dependent
  compiled code gets invalidated.
- A **deopt framepoint table**: for every program point in inlined
  code, enough information to rebuild the interpreter's view of the
  call chain (method, pc, temp values, stack contents).

---

## Phase 0 — Foundation decisions (1 week)

Decisions to make before writing code:

- IR shape.  Bespoke (SSA over Smalltalk-aware opcodes) vs reuse
  asmjit's IR (MI-style).  Recommendation: **bespoke SSA-lite**.
  asmjit's IR is designed for C-like code, not Smalltalk sends /
  block closures / NLR.  Reusing it would fight us at every turn.
- One tier or two.  Keep the stencil JIT for cold paths, add the
  optimizing JIT for hot paths?  Or replace entirely?
  Recommendation: **keep stencils**.  They're correct, fast-enough
  for IC-warmup, and removing them is a distraction.  The
  optimizing JIT triggers at a higher threshold (e.g. 1 000 calls)
  after IC is already populated.
- Deopt strategy.  Eager (materialize on every possible failure
  point) or lazy (reconstruct on demand from a side table)?
  Recommendation: **lazy with side table**.  Eager makes inlined
  code nearly as big as non-inlined.

Deliverable: a short architecture doc in this directory, 1–2 pages,
locking the above decisions before coding.

---

## Phase 1 — IC profiling diagnostic (DONE 2026-04-19, commit `6539046`)

Reality check: our ICs are already **6-way polymorphic**
(`IC_ENTRIES_PER_SITE = 6` in JITMethod.hpp), not monomorphic as
the original plan text assumed.  The IC structure itself needs no
change for Phase 4 (monomorphic inlining) — "is this site
monomorphic" is answerable today by counting populated entries.

Delivered:

- `JITRuntime::dumpICHistogram()` classifies every compiled IC site
  by populated-entry count: empty / monomorphic / 2-way / 3-way /
  4+-way.  Gated on `PHARO_IC_HISTOGRAM=1`; runs at bench complete.

Measured on tinyBenchmarks with JIT warmed up:

    compiledMethods=107  totalSites=313
    empty:        303 (96.8%)
    monomorphic:  10  (3.2%)
    2-way poly:   0
    3+-way poly:  0
    inlinable (mono or 2-way) / non-empty: 100 %

**Every non-empty hot site is monomorphic.**  Sista's core premise
holds for this workload — Phase 4 is worth building.  Also: 97 %
of compiled IC sites never fire, suggesting we over-compile — a
separate observation to revisit after Phase 4.

What the original Phase 1 text said to add but no longer needed
for Phase 4:
- Per-entry saturating counter.  The 6-way IC tells us "is this
  monomorphic" via populated count.  Counter only matters for
  Phase 5 (polymorphic inlining) where we'd need to rank classes
  by hit frequency — deferred to Phase 5.

Still TODO (low-priority):
- Mirror primitive to expose histogram to the image.  The stderr
  dump covers the dev need; image-side inspection is nice-to-have.

---

## Phase 2 — Method-level IR and round-trip (3–4 weeks)

Build the IR, lift bytecodes into it, lower IR back to asmjit for
the same methods the stencil JIT already handles.  No inlining yet.

Tasks:

- Define IR opcodes: `Send`, `Return`, `BranchIfTrue`, `LoadReceiver`,
  `LoadTemp`, `StoreTemp`, `BlockCreate`, `BlockActivate`, etc.
  Smalltalk-aware but not bytecode-level.
- Build IR from a method's bytecode stream.
- Emit asmjit from the IR.  Fresh code gen — but the shape is the
  same as the current stencil approach (per-IR-op prologue / epilogue)
  so the output will be within noise of the current JIT.
- Run on same IC infrastructure.  Same fallback path.

Correctness gate: benchmark suite passes with the IR-based JIT
enabled.  Performance gate: within 10 % of stencil JIT (we're not
inlining yet, so no expected speedup).

Risk: medium.  Building a new compiler backend for code we can
already compile is a lot of code for zero user-visible change.
Temptation to skip and jump to inlining is strong — resist.  Without
a clean IR, inlining is a pile of ad-hoc transforms that will
regress correctness.

---

## Phase 3 — Deopt infrastructure (3–4 weeks)

Before inlining, we need the deopt machinery that will unwind an
inlined frame back to individual interpreter frames when speculation
fails.

Tasks:

- **Framepoint side table**: at every potential deopt site (send,
  primitive, class guard), emit a record with:
  - Original method + bytecode index for each logical interpreter
    frame represented by this IR point.
  - Mapping from IR values to interpreter temp / stack slots.
- **Reconstruction routine**: given a framepoint ID and the current
  physical stack, rebuild the expected interpreter frame chain and
  resume at the specified bytecode.
- **Guard emission**: when code needs to bail, call a runtime helper
  that consults the framepoint table and does the rebuild.
- **Testing**: hand-write a few deopt scenarios (primitive failure
  mid-method, class guard failure, stack overflow in inlined code)
  and verify correctness.

Correctness gate: deopt tests pass.  Regression run on full SUnit
suite with deopt forced on every send (worst case — every inlined
call bails to interpreter).

Risk: **highest in the project**.  Deopt bugs are subtle — wrong
temp restored, wrong bytecode index — and produce silent corruption.
Budget 50 % more time than the phase estimate and expect to write
a lot of invariant-checking assertions.

---

## Phase 4 — Monomorphic inlining (4–5 weeks)

With IR + deopt in place, implement the simplest inlining:
monomorphic callees, one level deep, small callee size.

Tasks:

- IR transform: replace a `Send` with the callee's IR spliced in.
- Guard emission at the splice point: if receiver class ≠ observed
  class, jump to deopt.
- Heuristics: inline only if (a) IC has exactly one observed class,
  (b) callee size ≤ N bytecodes, (c) no recursion to the callee in
  the caller.
- Register allocation handles the combined region.
- Framepoint table records the virtual frame for the inlined method.

Correctness gate: benchmark suite, SUnit run, no regressions.
Performance gate: measurable speedup on fib, sum-loop,
arith-in-a-loop benchmarks — these are the canonical inlining
wins.  Target: 3–5× speedup on fib(28) vs current JIT.

Risk: medium.  Easier than deopt (Phase 3) but requires all the
framepoint bookkeeping to be correct.

---

## Phase 5 — Polymorphic inlining + inline cache specialization (2–3 weeks)

Extend to 2–3 observed classes.  Emit a dispatch chain at the
inlined site: class check, jump to the inlined body for that class,
or fall through to the next check, or deopt after last.

Tasks:

- Multi-class guards.
- Cost model: inlining 3 polymorphic variants is expensive — only
  do it for very hot sites.
- Fallback: megamorphic sites stay non-inlined (keep the current IC
  probe path).

Performance gate: AWFY benchmark suite geomean improves vs Phase 4.

---

## Phase 6 — Block inlining (4–6 weeks)

Biggest structural win and biggest risk.  `to:do:`, `whileTrue:`,
`select:`, `do:` — all heavily use blocks.  If we can inline the
block body into its enclosing method, the loop becomes tight native
code with no block-activation overhead.

Tasks:

- Detect full-block closures passed to known callees (e.g. `do:`
  always calls its block once per element).
- Splice the block's IR into the callee's IR at the call site.
- Handle non-local return from inlined blocks: if the block has
  `^` back to the enclosing method, emit NLR as a direct branch.
- Handle `ensure:` / `ifCurtailed:` — these require stack-unwind
  awareness even when inlined.

Performance gate: `1 to: 1000 do: [...]` becomes ~4 ARM instructions
per iteration (matches Cog).  Real-world Smalltalk code (which uses
blocks heavily) speeds up dramatically.

Risk: high.  NLR semantics are subtle.  One wrong unwind = memory
corruption.

---

## Phase 7 — Method-redefinition invalidation (1–2 weeks)

Once methods are inlined, re-defining a method must invalidate all
compiled code that inlined it.

Tasks:

- Each compiled method records a set of `(class, selector)` pairs it
  depends on (methods whose identity it inlined against).
- On `compile:`, `removeMethod:`, class-dict changes: walk dependency
  map, invalidate compiled code that depends on the changed method.
- "Invalidate" means: unhook from IC chains, flag for
  recompilation, mark as executable=false.  First next execution
  falls through to the stencil JIT or interpreter.

Correctness gate: develop-in-the-image workflows (save a method,
immediately run it) give the new code, not the old.

Risk: low-medium.  Mechanically straightforward but test coverage
matters — this is the kind of bug that only shows up in interactive
use, not benchmarks.

---

## Phase 8 — Optimizations in IR (ongoing)

With inlining working, actual peephole optimizations become
meaningful for the first time.

- Constant folding across inlined boundaries.
- Dead code elimination (unused results of inlined sends).
- Loop-invariant code motion.
- Integer-specific fast paths (tag check elision when we know both
  operands are SmallInt).
- Value numbering / CSE.

Each of these is independently a 1–2 week task; pick based on where
the benchmark profiler points.

---

## Weakest points / biggest risks

1. **Deopt correctness (Phase 3).**  Single biggest project risk.
   A deopt bug produces silent miscompile — test suite won't catch
   it reliably.  Mitigation: build a stress tester that randomly
   forces deopt at every possible site, compares result to
   interpreter-only run.
2. **Block-inlining NLR (Phase 6).**  Subtle semantics, hard tests.
   Mitigation: delay until the suite from Phases 1–5 is stable
   enough to catch regressions fast.
3. **Invalidation coverage (Phase 7).**  Bugs show up only in
   development-style workflows, not in batch benchmarks.
   Mitigation: integration test that saves / re-saves hot methods
   and verifies semantics.
4. **Scope creep.**  The temptation to add features (on-stack
   replacement, tier-switching, profile-guided heuristics, etc.)
   between phases.  Hard no on cross-phase work until the current
   phase passes all its gates.

---

## Realistic total estimate

Phase 0: 1 week
Phase 1: 2–3 weeks
Phase 2: 3–4 weeks
Phase 3: 4–6 weeks  (the hard one)
Phase 4: 4–5 weeks
Phase 5: 2–3 weeks
Phase 6: 4–6 weeks
Phase 7: 1–2 weeks
Phase 8: ongoing

Sum of ranges: **21–30 weeks** (5–7 months) focused work to get a
working end-to-end Sista-style JIT.  Each phase has a correctness
gate and a rollback point, so early-phase investment isn't lost if
the project is paused.

---

## Exit conditions (when to stop)

After **Phase 4** (monomorphic inlining) we'll know if the approach
actually closes the gap.  If fib(28) is still 10× slower than Cog
after Phase 4 is stable, more inlining won't help — the bottleneck
is something else and this plan is wrong.  Stop and reconsider.

If Phase 4 delivers the expected 3–5× speedup, continue; the
project is on track.

---

## Alternative: don't do this

Given the project goal (iOS Pharo VM, not a compute benchmark
champion), a reasonable alternative is to accept current performance
and redirect effort to:

- Finishing iOS device deployment (D1 in `deferred.md`).
- Working with real iOS apps to see where actual users hit the
  performance floor.
- Narrow perf wins in primitives that real apps hit
  (Morphic drawing, UTF-8 encode / decode, text layout).

Most of a Pharo IDE's wall-clock time is in primitives that run at
C speed regardless of JIT.  A 30× arith-bench gap translates to
maybe 5 % of wall-clock time in real use.  Before committing 5–7
months to a JIT rewrite, measure what the actual user-visible
bottleneck is on a real iOS workload.
