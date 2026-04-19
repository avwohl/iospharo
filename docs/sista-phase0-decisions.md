# Sista-Inlining Phase 0 — Architectural Decisions

Written 2026-04-19.  Locks the decisions before coding Phase 1 per the
plan in `docs/sista-inlining-plan.md`.  Referenced by every subsequent
phase — change only with commit message + rationale.

## Decision 1 — IR: bespoke SSA-lite, not asmjit's IR

Use a Smalltalk-aware intermediate representation.  Nodes:
`Send`, `Return`, `BranchIfTrue`, `BranchIfFalse`, `LoadRecv`,
`LoadTemp`, `StoreTemp`, `BlockCreate`, `BlockValue`, `Constant`,
`PrimOp` (for inlined arith / comparison), `Guard` (class check that
deopts on miss).

SSA-lite means:
- Each IR value has one definition (classic SSA).
- Phi nodes at basic-block joins, but kept implicit in simple cases
  (single-predecessor blocks don't need phis).
- No full dominance-tree bookkeeping yet — rebuild on demand from
  predecessor lists.

Rationale vs alternatives:
- asmjit's MI IR is C-flavored — doesn't know about Smalltalk contexts,
  sends, or block closures.  We'd build almost as much adaptation
  around it as a fresh IR costs.
- MIR was the last attempt to use an external IR; its GC-unawareness
  (spilling live oops across calls without barrier) was the reason
  we abandoned it.  Bespoke IR lets us make oop liveness part of the
  type system.

## Decision 2 — Two-tier: keep copy-and-patch as fallback

Tier 1: the current stencil JIT.  Stays on for everything.  Cold code,
megamorphic sites, code we haven't taught the optimizing JIT about.

Tier 2 (new): the Sista-style optimizing JIT.  Triggered at a higher
threshold than Tier 1 — e.g. 1000 calls + the IC is monomorphic or
biased-polymorphic.

Rationale: the stencil JIT is correct (took months to get there) and
handles edge cases the optimizer won't reach for quarters.  Throwing
it out is a distraction; keeping it shrinks the optimizer's
correctness surface.

## Decision 3 — Deopt: lazy reconstruction from side table

Emit compact deopt records in a per-method side table.  On deopt, walk
the table, rebuild the interpreter frame chain, resume.

Rationale vs eager (insert full save-state at every possible deopt
point):
- Eager inflates compiled code ~2× and gives up most of the inlining
  win.
- Lazy cost is paid only on actual deopt, which should be rare
  (speculation proves wrong, primitive fails, class hierarchy changed).
- Cog's Sista also uses lazy reconstruction.

## Decision 4 — Inline-cache data structure

Already in place: 6-entry polymorphic IC per send site, format
`[key, method, extra, …, selectorBits]` (144 bytes per site).  Good
starting point — don't shrink or widen.

What's missing: per-entry call counter for inlining heuristics.
Allocate 16 bits of the `extra` field to a saturating counter.
Increment on IC hit; saturation at 65535 is plenty for "this class
dominates" signal.

Additional:
- Per-site aggregate count (16 bits) so the JIT knows how hot the
  site is without summing entries.
- "Monomorphic / polymorphic / megamorphic" classification becomes
  a property of the counter distribution, computable on demand.

## Decision 5 — What phase-4 inlining will do (scope lock)

Phase 4 inlines ONLY:
- Monomorphic sites (exactly one entry in the IC).
- Callees ≤ 20 bytecodes.
- Non-recursive.  No self-calls from caller to callee class.
- No primitive methods (those stay non-inlined; primitive dispatch
  is already fast).

Anything more complex is later-phase.  Resisting scope creep here is
critical — the simpler phase 4 is, the faster we know if the approach
works.

## Decision 6 — Invalidation granularity

Per-JITMethod dependency set: list of `(classIdx, selectorOopBits)`
pairs this compiled method has inlined against.

On method redefinition (`compile:` / `removeMethod:`):
- Walk all compiled methods; check dependency lists.
- Match → mark compiled method as `MethodState::Invalidated`.
- Next execution falls through to Tier 1 or interpreter; Tier 2 will
  recompile from fresh bytecodes eventually.

Not per-inline-site, not per-class-hierarchy.  Method-level is coarse
but easy to get correct, and invalidation is rare enough that
over-invalidating isn't a perf concern.

## Decision 7 — Testing strategy

Every phase ships with:
- Unit tests in `test_load_image` (short scripts that exercise the new
  code path).
- Regression gate: full SUnit suite (post `6029846`) must hold at
  ≥99.4 % pass rate from the run in commit `a2b99f7`.
- Performance gate: benchmarks in `docs/performance/` — don't allow
  regressions; each phase documents the expected (if any) speedup.

Phase 3 (deopt) additionally needs a **randomized deopt forcer**:
env var `PHARO_FORCE_DEOPT_FRACTION=N` that makes N% of speculative
sites bail immediately.  The SUnit run must pass with
`PHARO_FORCE_DEOPT_FRACTION=100` — if not, the deopt path has silent
miscompile.  This is the most important test in the project.

## Decision 8 — Exit condition after Phase 4

After Phase 4 lands stable:
- Run `fib(28)` benchmark.
- If Tier 2 JIT is ≥3× faster than Tier 1 on `fib(28)`:  continue with
  Phase 5.
- If Tier 2 JIT is within 20 % of Tier 1 on `fib(28)`:  the approach
  isn't working; stop, write a post-mortem, consider alternatives.

The ≥3× threshold is generous — stock Cog Sista is ~10× faster than
Pharo's non-JIT interpreter on fib.  If monomorphic inlining isn't at
least 3×, more inlining won't save it.

---

## Scope-creep watch list

Things to NOT do during this project, even if tempting:

- Deoptimize-then-reoptimize with new profile data (OSR-deopt-OSR
  loop).  Single-shot tier-up only.
- Dynamic inlining (inlining decisions changing mid-method).
  Everything decides at compile time.
- Guard elimination across SafePoints.  Guards stay on every inlined
  site until Phase 8 at earliest.
- Tailcall optimization.  Orthogonal; not on critical path.
- Regalloc coalescing beyond what asmjit provides.  If we hit regalloc
  pressure, first fix is spill-to-stack, not smart allocation.
