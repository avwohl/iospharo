# JIT plan B — closing the gap to Cog (2026-05-21)

Companion to `jit-may20.md`. That doc covered Step 1 (bit-60 preservation),
Step 2 (warmth gate), Step 3 (correctness harness) — all shipped; Step 4
(Sista bail) + Step 5 (other hot loops) deferred. None of those touched
the dominant cost. This doc sequences what's actually left between us
and Cog parity.

## Where we are (2026-05-21, jit branch)

Single-shot fib, our VM (asmjit-T1, default settings):

    fib(20)     ~     7 ms          21,891
    fib(28)     ~   155 ms       1,028,457
    fib(30)     ~   331 ms       2,692,537
    fib(32)     ~   917 ms       7,049,155

With both gates disabled (correctness broken — 4/10 fib(17) returns
5067/5133 instead of 5167):

    fib(20)     ~     1 ms
    fib(28)     ~    22 ms       (7× faster than gated)
    fib(30)     ~    35 ms       (10×)
    fib(32)     ~    60 ms       (15×)

Cog reference (same M1):

    fib(28)     ~     3 ms

So two gaps to close:
- **Gate-overhead gap** (~7×): inline-J2J's 96% bail rate. Fix the
  materialize-bail bug, drop the gates entirely.
- **Post-gate gap** (~7×): even with no gate we're 22 ms vs Cog's 3 ms.
  That's the per-activation cost gap.

## Step 6. Diagnose the 96% gate bail mystery  *(days — start here)*

Both gate variants (pure bit-60 and warmth) bail ~96% of inline-J2J
attempts on fib. For benchFib (numICEntries = 2, self-recursive) this
shouldn't happen — after warmup, both IC sites have entry-0 filled
with bit 60 set, so both gates should pass.

The mystery is data, not theory. Steps to land:

- **6.1** Add per-method counters keyed on `callerJM` to `g_inlineJ2J_bail_gate`:
  when the gate fires, increment a hash map keyed on `state.jitMethod`.
  Print a top-20-callers histogram at VM exit. Is benchFib in the bail
  cohort or the pass cohort?
- **6.2** If benchFib is bailing: emit a one-shot trace at gate bail
  for benchFib's `JITMethod*` — print `icBuffer[site].{key, method, extras}`
  for all `numICEntries` sites. Look for which site fails the check
  and why.
- **6.3** Likely candidates (from least to most invasive):
  - `callerJMReg2` (= `x19` in xmethod-off) is stale or clobbered
    between the trampoline hoist and the gate eval. Re-load
    `state.jitMethod` into a fresh reg right before the gate.
  - Recompilation: benchFib gets re-JIT'd mid-run; the new `JITMethod`
    starts cold-IC, runs hot for one iteration, then the next gate eval
    sees the just-allocated cold state. `JITRuntime` recompile log
    will say.
  - IC fill ordering: polymorphic refill writes to a slot other than
    entry 0; the just-hit site keeps entry-0 stale. (Unlikely for
    benchFib's monomorphic shape, but check.)

Validation: after the fix, `bail_gate` drops from 5.5M to <100K on the
`big_fib` workload, and `inline-J2J: hits` rises from 0 to ≥10M. fib(28)
should drop from 155 ms to ~22 ms with correctness intact.

## Step 7. Fix the materialize-bail wrong-result bug  *(weeks)*

Step 6 might just hide the gate bail, but the underlying bug remains:
when inline-J2J's callee body hits a cold IC mid-flight, the chain-loop
materialize-bail corrupts the value chain (per deferred A6 iter N+30c..i).
Once the gates can pass, we MUST fix the unwind.

Two options from `jit-may20.md` §2:

- **Recursive-safe materialize**: extend `SavedFrame` with
  `savedReceiverSlot*`, make `popFrame` write the popped value there
  for materialized frames. `Interpreter.cpp:21147` is the OUTER push
  site. The hard part is that `popFrame` is shared with normal returns
  — splitting the materialize-return path is the meat of the work.
- **No-bail design**: inline-J2J pre-validates ALL of the callee's
  bytecode-reachable IC sites at PUSH time. If any is cold (key == 0)
  OR any send opcode could go cold (e.g., a megamorphic site that
  evicted), fall back to chain-loop BEFORE pushing. This is essentially
  the warmth gate done correctly — see Step 6, the warmth check is
  fine; what fails is the per-PUSH overhead.

Recommendation: **start with no-bail design** once Step 6 has the
gate passing for fib. The per-callee gate is then a hot-method
specialization. Recursive-safe materialize is the more correct fix
but multi-week and shares the same end state.

Validation:
- `scripts/bench-correctness.sh` passes with the chosen gate disabled
  (replaced by the no-bail logic).
- `fib(17)` is correct in 50/50 runs (current gate-off: ~6/10 wrong).
- `fib(28)` ≤ 25 ms; `fib(30)` ≤ 40 ms.

## Step 8. Close the per-activation gap  *(7× — multi-week)*

After Steps 6+7, we're ~22 ms / fib(28) vs Cog's 3 ms. That's the
per-activation cost gap. Breakdown:

- fib(28) does ~1.03 M activations + 2× that many sends = ~3 M sends.
- 22 ms / 3 M = 7 ns per send = ~21 cycles on a 3 GHz M1.
- Cog at 3 ms / 3 M = 1 ns per send = ~3 cycles.

So roughly 18 extra cycles per send. That's the IC HIT path + J2J
save + tail-call + return. Subsystems likely to move the needle:

### 8.1 Tighter IC HIT path
- The current IC probe walks 6 entries with full ldp/cmp per slot.
  Cog uses 3 entries with class-index encoded as immediate.
- Bytecount audit: AsmjitT1.cpp's send-emit is dozens of instructions
  per site. Aim for ≤12 in the monomorphic-hit fast path.

### 8.2 J2J save shrinkage
- The save currently writes 7 fields (sp, receiver, tempBase, ip,
  jitMethod, resumeAddr, sendArgCount) = 56 bytes. Cog's JIT doesn't
  push half of this on self-rec.
- Self-recursive specialization: when `SELF_REC_BIT` is set, skip
  `jitMethod` and `tempBase` save (they're invariant).

### 8.3 Inline IC patch
- IC fills currently go via `jit_rt_fill_ic` (C runtime call).
  Inlining the fill into the IC MISS path would save the call+ret
  cost on cold sends.

### 8.4 Sista phase 4 (depends on Step 4)
- The `jit-may20.md` Step 4 path: fix Sista's bail correctness, then
  hook Sista's compiled fn into T1's IC HIT emit via a new `SISTA_BIT`.
  Sista's monomorphic inlining can collapse `benchFib(n-1) + benchFib(n-2) + 1`
  into a single specialized op.

Validation: at each substep, measure fib(28) + bench-correctness.sh.
Target: ≤ 5 ms fib(28) total.

## Step 9. Bench-suite reliability  *(days, blocker for Step 10)*

`scripts/run_benchmarks.sh` on our VM currently hangs (≥10 min, no
output) on the headless image produced by `scripts/pharo-headless-test`.
Step 5 from the original plan needs this fixed to make per-benchmark
profiling possible.

Likely cause: a scheduler/SessionManager race between the bench's
`forkAt: highestPriority` and our VM's startup handler. The
`jit_loop` test in this session reproduced the hang trivially via a
file-IO-using startup handler.

Diagnosis path:
- Run the bench-suite under `lldb` attach, interrupt after 30 s,
  inspect which process is on-CPU.
- Check whether `[PROGRESS] Ns: ~K steps` continues after the apparent
  hang point. If yes, it's a deadlock not an infinite loop.
- Patch suspect priorities; consider isolating one bench at a time.

## Step 10. Per-bench profiling  *(open-ended, after Step 9)*

Once the suite runs reliably:

- Profile each bench with `PHARO_PRIM_PROFILE=1` + JIT stats.
- Target the smaller gaps first (sieve, sort, dict at 1.5–3×) before
  the big ones (fib at 50×). Smaller gaps are cheaper to close, and
  the wins compound.
- Pattern-match each bench's hot path to existing infrastructure
  (inline prim, block-value spec, monomorphic Sista) and propose
  the smallest possible change.

## Dependency graph

      6 (gate-bail mystery)
       │
       └─► 7 (materialize-bail fix)
            │
            ├─► 8.1, 8.2, 8.3  (per-activation cost)
            │
            └─► (Sista Step 4) ─► 8.4

      9 (bench-suite reliability)
       │
       └─► 10 (per-bench profiling)

Steps 6, 7, 8 are the critical path to fib parity. Step 9+10 is
parallel work that closes other benches.

## How to validate any change

    scripts/bench-correctness.sh                 # asserts fib 20/28/30
    scripts/bench-correctness.sh --ab fib 20     # cross-mode check

Add a perf line per step to the table at the top of this doc. If a
perf claim doesn't include a row in that table AND the harness pass,
it's void.
