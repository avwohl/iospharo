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
  - 23 commits ahead of origin/jit pre-push, all pushed

## Next concrete next step

Step 2 (lowering-time landing pad emission) is the lightest of
the remaining Phase 3 work and unblocks Phase 4.  Estimated 1
focused session.  Will need to decide between option (a) side
table vs option (b) in-line emission first.
