# JIT plan B — closing the gap to Cog (2026-05-21)

Below are proposed fixes for JIT performance.
If one of the fixes when implemented doesnt help, leave code in but disabled.

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

## Step 6. Diagnose the 96% gate bail mystery  *(2026-05-21 — diagnosed)*

Both gate variants (pure bit-60 and warmth) bail ~96% of inline-J2J
attempts on fib. For benchFib (numICEntries = 2, self-recursive) this
shouldn't happen — after warmup, both IC sites have entry-0 filled
with bit 60 set, so both gates should pass.

### 6.1 (done) — per-caller histogram

Added `PHARO_T1_BAIL_GATE_HISTO=1` (and the implied
`PHARO_T1_BAIL_GATE_TRACE=1`) in `src/vm/jit/asmjit/AsmjitT1.cpp`.
Emits a `bl jit_rt_bail_gate_log(callerJM, kind)` call around the gate
exits; `dumpJITStats` prints a top-20 histogram at VM exit. On
`fib(28)`:

    bail-gate BAIL histogram (3 callers, 513208 total events):
      0x… #benchFib                tier=2 nIC=2 count=512552
      0x… #benchFib                tier=1 nIC=2 count=364
      0x… #allVisibleSlots         tier=1 nIC=2 count=292
    bail-gate PASS histogram: (empty)

Answer: benchFib is **100% in the bail cohort**. The gate never passes.

### 6.2 (done) — IC site dump

Per-site dump (`PHARO_T1_BAIL_GATE_TRACE=1`) at first bail AND at exit
shows benchFib's two IC sites:

    site=0 entry=0 key=0x80000001 method=#benchFib extras=bit60|bit56|J2Jaddr
    site=1 <empty> selBits=#benchFib hitCount=0

Site 0 is monomorphic-warm (SmI receiver, J2J entry bit set, self-rec
bit set). Site 1 is *completely cold for the entire run of fib(28)*,
across both tier=1 and tier=2 JITMethods. Even with `PHARO_RECOMPILE_AT=99999999`
(no recompile), site 1 stays empty.

### 6.3 — root cause: bcToCode=0 gap, not the listed candidates

None of the three candidates fit. The actual root cause is the
**asmjit-T1 resume protocol gap for sends-containing methods**
(`src/vm/jit/asmjit/AsmjitT1.cpp:5391`):

    bool advertiseResume = isReal && !noNumBc && !noBcToCode && bcLen > 0;
    if (numSendSites > 0 && !forceResumeForSends) advertiseResume = false;
    jm->numBytecodes = advertiseResume ? (uint16_t)bcLen : 0;

Every benchFib JM has `numBytecodes=0` and `bcToCodeTableOffset=0`. The
trampoline's `Ltramp_call` resume-address computation reads
`bcToCodeTable[bcOffset]`, gets zero, and falls through to `Lresume_null`,
which exits the trampoline. Concretely:

1. bc 59 (first `benchFib` send): IC HIT site 0, gate iterates both
   sites, site 1 cold → bail to `dispatchCached` → exit via
   `EXIT_SEND_CACHED`.
2. Trampoline converts to `EXIT_J2JCALL`, sets up callee, BLRs to
   tier=2 entry (callee = self).
3. Callee runs bc 0..bc 59 in JIT. At bc 59 it bails again (same
   path), recursing deeper. Eventually the base case returns
   `EXIT_RETURN`.
4. Trampoline pops the save, but the save's `resumeAddr` was computed
   from `bcToCode[bc 60] = 0` → null. `Lret_null_resume` sets
   `state.ip = bc 60`, exits the trampoline with `EXIT_RETURN`.
5. Interp resumes at bc 60 *in interp mode*. Runs bc 60..67 (including
   the second `benchFib` send at bc 63) entirely in interp.
6. Interp's send-resolution path does call `patchJITICAfterSend`, BUT
   `pendingICPatch_` was never armed for a JIT IC site at bc 63
   (because the JIT IC probe at bc 63 was never executed).

So tier=2 benchFib's icBuffer site 1 cannot fill. The gate iterates
all `numICEntries` sites and finds site 1 cold → bails. **Forever.**

`PHARO_ASMJIT_T1_FORCE_RESUME_FOR_SENDS=1` (toggle the
`numSendSites > 0` guard off) crashes immediately — confirmed; the
post-resume protocol mismatch noted in the AsmjitT1.cpp comment is
real. Fixing it is multi-week (out of scope for this step).

### What this means for Steps 7+

The gate is doing exactly what it's supposed to (block inline-J2J
when any callee IC site is cold to avoid the materialize-bail
wrong-result bug). The "cold site" precondition is *unreachable* under
the current resume protocol for any method with ≥2 sends — so the gate
never lets fib through.

Options to unblock the gate:
- **Fix the bcToCode resume protocol for sends-containing methods**
  (multi-week, but unlocks much more than just fib parity).
- **Implement Step 7's "no-bail design"**: inline-J2J pre-validates ALL
  callee IC sites and refuses to push the save if any might bail. Same
  semantics as the current gate, but correct unwind on the corner cases
  the gate currently fails to catch. (Doesn't help the cold-IC scenario
  by itself.)
- **Implement Step 7's "recursive-safe materialize"**: extend
  `SavedFrame` with `savedReceiverSlot*` so the unwind writes the
  return value to the right caller slot even under nested bails. Then
  the gate is no longer needed — gate-disabled fib runs correctly and
  fast.

Recommendation per the doc: do Step 7's recursive-safe materialize.
The gate-bail isn't a separate bug to fix in Step 6; it's a symptom of
the same root cause as the wrong-result bug, just on a different
codepath.

Diagnostic toggles (kept in-tree, default-off):

- `PHARO_T1_BAIL_GATE_HISTO=1` — per-caller bail/pass histogram dumped
  in `dumpJITStats`, plus an icBuffer dump of every #benchFib /
  #allVisibleSlots JM and the top-5 bail callers.
- `PHARO_T1_BAIL_GATE_TRACE=1` — one-shot trace of the bailing
  caller's icBuffer at the first gate bail (implies HISTO).

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

## Step 9. Bench-suite reliability  *(resolved 2026-05-21)*

Status: **does not hang**.  Tested 3/3 runs of
`PHARO_VM=/tmp/harness/pharo scripts/run_benchmarks.sh --ours-only`
on the jit branch — every run completes through all 14 benchmarks
and writes the full `/tmp/pharo_benchmarks_ours.txt`.

The hang Step 9 described was the historical `PHARO_NO_SISTA_DO_SPLICE`
scheduler race already fixed by the 2026-05-18 inline-J2J +
bytecode-coverage shipping (see the comment in
`scripts/run_benchmarks.sh:106`).  No further action needed; Step 10
is unblocked.

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
