# Sista Phase 3+4 progress — 2026-04-24

Tracking implementation of `docs/sista-inlining-plan.md` Phases 3
(deopt infrastructure) and 4 (monomorphic inlining).

## Phase 3 — Deopt infrastructure (target 3-6 weeks)

### Step 1 — Framepoint data structure + capture (DONE 2026-04-24)

Commits: `9fb8698`, `36f3e7d`.

- `Framepoint { valueId, bcOffset, stack snapshot }` struct
  in `src/vm/jit/sista/SistaIR.hpp`
- `Method::framepoints` vector for the side table
- `LinearLifter::recordFramepoint()` helper
- All 6 `kSendUnspeculated` emission sites in `SistaBuilder.cpp`
  call it
- 3 unit tests in `test_sista_ir.cpp` verify Send0/Send1/no-send
  cases

No behavior change today; framepoints captured but unused.

### Step 2 — Serialize / persist framepoints (NOT STARTED)

Two design choices, both viable:

  a. **Side-table on JITMethod analog**: serialize the framepoint
     vector to a binary blob, store on a per-Sista-compiled-method
     handle.  Lookup at runtime via valueId → framepoint.  Heavier
     but keeps deopt code path clean.
  b. **In-line at lower time**: SistaLowering reads the framepoint
     vector during lowering and emits deopt landing pads
     directly into the generated code, with the framepoint data
     baked in as constants.  Lighter, no runtime side table, but
     each landing pad is duplicated per inline site.

  Lean toward (b) — Sista already lowers per-method, no separate
  runtime metadata layer.  Estimate: 1 week.

### Step 3 — Use framepoints in deopt path (NOT STARTED)

Today every `kSendUnspeculated` lowering emits a bail sequence
using the literal-encoded `bcOffset` + the value's `operands`
list.  This effectively IS deopt today (the simulated stack maps
1:1 to interpreter stack slots).

For Phase 4 inlining we need to extend this to:
- A guard miss (kGuardClass with wrong receiver class) jumps to
  the OUTER send's framepoint, restoring the pre-inline stack
  state
- The interpreter then re-executes the original send unspeculated

Estimate: 1 week given step 2 is done.

### Step 4 — Random-deopt stress tester (NOT STARTED)

Per the plan: a `PHARO_FORCE_DEOPT_FRACTION` env var that randomly
forces every kSendUnspeculated to bail (instead of executing
speculatively).  Current Sista almost always bails anyway, so
this tester only becomes meaningful AFTER Phase 4 ships.  Defer.

## Phase 4 — Monomorphic inlining (target 4-5 weeks)

### Step 1 — IR-level inline transform (NOT STARTED)

Pseudo-code:

  for each Send opcode in source bytecode:
    if IC has exactly one observed receiver class C:
      look up method M = C >> selector
      if M is small (<= N bytecodes) and not recursive:
        lift M's bytecode to a SistaIR fragment
        replace kSendUnspeculated with:
          kGuardClass(receiver, C, deopt_target)
          ... spliced fragment ...
        deopt_target := framepoint of original kSendUnspeculated
        (= re-execute the unspeculated send in the interpreter)

The hard part: where does the IC info come from?  Three options:

  a. Sista runs AFTER T1 has warmed up; queries T1's IC tables.
     Couples Sista to T1.
  b. Sista does its own profiling phase.  Doubles compile cost.
  c. Hardcode common patterns: `^ self yourself`, `==`, `~~`,
     `class`, certain getters.  Fast but limited coverage.

Recommend starting with (c) as a proof-of-concept — pick
`#yourself` (universally `^ self`), inline as no-op.  Real perf
win would need (a) for broader coverage.

### Step 2 — Heuristics

Already largely sketched in the plan.  Estimate: 1 week.

### Step 3 — Register allocation across spliced regions

May need rework of how SistaLowering tracks live values across
inline boundaries.  Estimate: 1-2 weeks.

### Step 4 — Bench gate

Target: 3-5× on `fib(28)`.  Currently 70 ms vs Cog's 2 ms (35×
behind).  Phase 4 alone won't close that — would still be ~7-10×
behind Cog after Phase 4.  Phase 5 (polymorphic) + Phase 6
(block inlining) needed to close further.

## Realistic timeline

  Phase 3 step 2-4: ~3 weeks
  Phase 4 step 1-4: ~5 weeks
  Total: ~8 weeks of focused work for Phase 4 ship

Per the plan's risk note, deopt bugs are the highest project
risk.  Allow 50% buffer: **realistic 10-12 weeks total**.

## Today's net deliverable

Step 1 of 4 in Phase 3.  ~1/13 of the total project.

Branch state at end of session:
  - Class-table identity-hash collision fix shipped (0ca2b58)
  - YG default-on shipped (3b37bd2)
  - Framepoint capture infrastructure (9fb8698)
  - Framepoint tests (36f3e7d)
  - iOS framing dropped (a67f6a4)
  - All pushed to origin/jit

## Findings on the path to Phase 4

Investigated whether the simpler "lift unsafe-arith gate" path
gives a real perf win without full Phase 4.  **Answer: no.**

Measured PHARO_SISTA_UNSAFE_ARITH=1 on TraitTest + bench:
  - TraitTest: 54/54 pass (no regression)
  - fib(28): 75 ms (vs 74 ms baseline) — within noise
  - sieve x100: 136 ms (vs 137 ms baseline)

The bench is send-heavy, not pure-arith.  Even when Sista
admits + - *, the methods STILL bail at the first send.
Methods that benefit from inlined arith without sends are rare
(some math primitives).  Removing the unsafe-arith gate would
risk silent miscompiles for ~zero perf win.

So the real Phase 4 deliverable IS method inlining, not arith
inlining.  No shortcut.

## Next concrete next step

The hard part of Phase 4 — splicing a callee method's IR into
the caller's at a send site — needs:

  1. A way to determine the callee's class statically (Phase 1
     of plan: query T1 IC tables; or speculative inlining
     based on observed bytecode patterns)
  2. Lift the callee's bytecode to IR (Builder already does
     this for the entry method; refactor to handle nested)
  3. Splice the callee IR with kGuardClass bracketing
  4. Deopt landing pad: jump back to outer kSendUnspeculated
  5. Heuristics: callee size limit, recursion check

Estimated 4-6 weeks of focused work just for this piece.
Phase 3 step 2 (deopt infrastructure that the inlining needs)
is interlocked with this — easier to design them together than
independently.

**Recommendation for next session:** spend 1 session on a
focused PROOF-OF-CONCEPT inline of ONE specific callee
(e.g., `Object>>yourself` — universally `^ self`, simplest
possible target).  Hardcode the receiver-class detection.
Goal: end-to-end working inline with deopt landing pad.

That validates the architecture before committing to the
full T1-IC-query infrastructure.

## UPDATE (2026-04-24, evening): three POC inlines shipped

Inline POCs landed and validated:

  Commit       Selector        Shape                      Env var
  -----------  --------------  -------------------------  ------------------------
  b450e58      #yourself       no-op (skip emission)      PHARO_SISTA_INLINE_YOURSELF
  35ce72f      #==             cmp + csel(EQ)             PHARO_SISTA_INLINE_IDENTITY_EQ
  6763554      #~~             cmp + csel(NE)             PHARO_SISTA_INLINE_IDENTITY_EQ

All three opt-in.  Architecture validated for both inline
shapes (no-op + codegen).

### Full SUnit baseline under PHARO_SISTA_INLINE_YOURSELF=1
+ PHARO_SISTA_INLINE_IDENTITY_EQ=1

`docs/jit-baseline-2026-04-25-inline-poc.txt`:

                       YG-default       Inline POCs (yourself + == + ~~)
  Tests passed:        12664            12654   (-10 from PragmaTest flake)
  Tests failed:        3                3
  Tests errored:       1                1
  Tests timeout:       0                0
  Wall clock:          77 m             77 m

The -10 difference is `PragmaTest` reporting "no-tests" in
this batch (flake — passes 10/10 in isolation under both
modes).  The +1 fail (`ProcessTerminateBugTest`) from the
yg-default run did NOT recur this time.  Net: same effective
non-ok set.

**Inline POCs are correctness-clean across all 565 classes.**

### Bench impact: NEGLIGIBLE on current benchmarks

  Workload                 YG-default   Inline POCs
  fib(28)                  74 ms        75 ms
  sieve x100              137 ms       136 ms
  dict 50K put+get        178 ms       172 ms (+3% maybe)
  factorial               26 ms        25 ms
  1M getter+yourself      96 ms        99 ms

`yourself`, `==`, `~~` aren't hot in these microbenchmarks.
The 1M getter+yourself test does 1M `obj yourself` calls but
the timesRepeat: loop overhead dominates.

### Decision: keep opt-in

Defaulting these on is unjustified given:
  1. No measurable perf win in benchmarks
  2. `yourself` POC has a small correctness risk (any class
     overriding it would silently miscompile — needs CHA or
     class guard before default-on)
  3. `==`/`~~` POC is theoretically safer (universal
     identity semantics) but no perf benefit to justify the
     gate flip

The value is **architectural**: the POCs prove Sista can
do both no-op and codegen-style inlining end-to-end.  When
the harder cases land (getters with deopt, sends with class
guards), the patterns established here apply.

### What this teaches about what's next

Real perf wins from inlining need:
  1. Inline targets that are HOT (not yourself / ==)
  2. Class info to choose monomorphic candidates
  3. Deopt landing pads for receiver-class miss

Order of attack for next sessions:
  A. **Profile selector hotness in real workloads.** Add a
     send-counter keyed by selector to T1's IC-fill path.
     Run a representative workload, dump top-N selectors.
     Picks the right next inline targets.
  B. **Plumb T1 IC info into Sista compile.** When Sista
     compiles method M, query T1 for any IC sites in M
     that are monomorphic.  Use that info for inline
     decisions.
  C. **Implement getter inline with class guard** as the
     first non-trivial codegen inline.

(A) is half a day, (B) is 1-2 days, (C) is several days.
Total: ~1 week to a meaningful Phase 4 perf delivery.
