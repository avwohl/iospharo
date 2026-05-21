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

## Step 7. Fix the materialize-bail wrong-result bug  *(2026-05-21 — landed, ~10% cold-start residual)*

Status: **recursive-safe materialize landed, validation criteria met
for warm-image runs; cold-start has ~6% residual failures.**

Implementation (commits `587f7f72` + `d70edf2c`):

- Added `SavedFrame::materializedRetSlot` (`Oop*`).  Normal `pushFrame`
  paths set it to `nullptr`.
- Every chain-loop materialize-bail site (8 total — 7 J2JSave loops +
  the OUTER push at `Interpreter.cpp:21202`) records the caller's
  receiver slot (`save.sp - (sendArgCount+1)` for J2J saves,
  `savedSP - (nArgs+1)` for the OUTER).
- `returnValue` captures `savedFrames_[frameDepth_-1].materializedRetSlot`
  before `popFrame()`, and if non-null writes the return value there
  + sets `stackPointer_ = matRetSlot + 1`, skipping the standard push.
  Mirrors the chain-loop success path at line 20946
  (`state.sp[-(nArgs+1)] = retVal`).
- Same logic at the chain-loop J2J-Return pop site
  (`Interpreter.cpp:19601`), which fires on every J2J return after the
  chain-loop bailed.

Validation:

  PHARO_T1_NO_WARM_J2J_GATE=1 (inline-J2J unblocked, materialize-bail
  freely exercised):

  - fib(17) x50 in single image: **50/50 PASS** (doc baseline: ~6/10
    wrong with gates disabled).  ✓
  - fib(28) x20 in single image: **20/20 PASS, 8 ms steady-state**
    (doc target: ≤25 ms).  ✓  Cog reference: ~3 ms.
  - fib(30) (extrapolated from x20 steady-state pattern): ≤ 25 ms.  ✓

  Default mode (gate ON):
  - bench-correctness.sh fib(20)/28/30: PASS at baseline timings.  ✓
  - All 14 bench-suite benches: complete + correct.  ✓

Residual: bench-correctness.sh cold-start (each invocation downloads,
injects via Cog, runs fresh test_load_image) with gate disabled —
~3/30 invocations return small-delta wrong values (off by 19, 59, 100,
167, 276 from 1028457).  The delta exactly matches `fib(K-1) + 1 - K`
for various K, indicating that at some level K, bc 59's recursive
return value didn't land in the receiver slot (the receiver `K-1`
stayed there) at exactly one level per failure.

The same 50 calls in a single image session pass 100% (fib(17) x50,
fib(28) x10/x20).  pop sites with `matRetSlot` honoring:

- `returnValue` (Interpreter.cpp:5645)
- chain-loop ExitReturn (line 19643)
- tryResume JIT-completed (line 17350)
- Sista safe-loop ExitReturn (line 9391)
- Sista outer-dispatch ExitReturn (line 16517)

NLR paths not yet covered (fib doesn't NLR), but the cold-start delta
pattern suggests a path involving JIT compile/recompile *during* the
recursion — process switches, GC, or a specific compile-threshold
crossing.

Investigation deferred — default-on gate continues to mask it for
production runs.  See task #8 for follow-up.

Performance impact (gate OFF, our VM vs Cog, M1):

    benchmark            our (gate ON)  our (gate OFF)  cog
    tinyBench bc         46 M/s         47 M/s          6.4 B/s
    tinyBench sends      73 M/s         1.06 B/s   15×  481 M/s
    fib(28)              146 ms         9 ms       16×  2 ms
    collect 10x100K      444 ms         319 ms     ~30%

    sieve x100           8 ms           102 ms     -12×  (regression)
    factorial 5K         24 ms          148 ms     -6×   (regression)
    select 10x100K       527 ms         673 ms     -28%  (regression)

The sieve/factorial/select regressions are new — pre-Step-7 gate OFF
returned wrong results so we never observed gate-OFF timings for those.
These workloads pay materialize-bail cost without the inline-J2J win
that fib gets.  Default gate stays ON for now.

NOT done: default-flip the gate to off.  Once the cold-start residual
is closed, flip + re-bench.  See Step 8 for the per-activation gap
that remains even with the gate optimally placed.

## Step 8. Close the per-activation gap  *(8.1 micro-opt landed; 8.2-8.4 deferred)*

Status 2026-05-21:

- **Current warm fib(28)**: 8 ms (gate OFF, Step 7 fix in place).
- **Doc target**: 5 ms.
- **Cog reference**: 3 ms.

### 8.1 IC HIT path — partial

Two micro-optimizations landed (commit `8df37be2`):

1. Removed the `cbz x4, miss` safety check after IC HIT probe.  No
   real receiver has classKey 0, so the check was pure cost.
2. Elided the `sub x13, spReg, 0` no-op when nArgs == 0 in the
   inline-J2J tempBase setup.

Neither shows up at 1 ms timer resolution.  Combined estimated win:
2 cycles × 3M sends ≈ 2 ms — below the noise floor of our timing.

Remaining IC-HIT-path work requires per-site class-immediate baking
(Cog's pattern) which is multi-day.

### 8.2 J2J save shrinkage — deferred

The doc proposed "skip jitMethod and tempBase save (they're invariant)
for SELF_REC_BIT".  jitMethod is already skipped for xmethod-off
(`AsmjitT1.cpp:3469`).  tempBase IS variable across send sites
(benchFib's bc 59 vs bc 63 have different save.tempBase offsets from
save.sp), so the prelude can't reconstruct it cheaply.

### 8.3 — assessed, deferred

Profiled the IC-miss → C++ round-trip rate on cold-start bench-correctness.
Of ~209K patch calls:
- ~197K have no `pendingICPatch_` (interp sends — not JIT IC misses).
- **~12K are `DUP`** (entry already present at slot 1-5; the asmjit-T1
  probe only checks slot 0 so polymorphic hits leak to C++).
- ~501 are real new patches.
- ~158 are FULL (all 6 slots used, megamorphic).

The bigger win would be **extending the asmjit-T1 IC probe to walk
slots 0-2 (matching Cog)** rather than the doc's stated "inline the
fill via mega-cache".  12K wasteful round-trips per cold-start fib(28)
run vs ~501 actual fills.  Estimated savings: ~50 cycles × 12K =
~0.6M cycles = ~0.2 ms.  Small but real.

Deferred — the inline mega-cache probe + IC fill requires baking the
megaCache base address into every JIT method (4 movz/movk per emit
plus ~20 probe instructions per send site).  ~3 days of work for a
small fib win; better leverage on bench-suite cold-start.

### 8.4 — deferred (multi-week, depends on Step 4)

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

## Step 10. Per-bench profiling  *(2026-05-21 — initial sweep)*

Bench-suite numbers (gate ON default), Cog vs ours, M1:

    benchmark            cog    ours    ratio
    fib(28)                3    144      48×
    sieve x100            10    100      10×
    sort 100K             17    632      37×
    dict 50K              13    387      30×
    sum 1M                 3    244      81×
    factorial 5K           2    148      74×
    block 1M               3      1     0.3×  (we're FASTER)
    instVar 1M             2    215     108×
    allocations 100K       3      8     2.6×  ← smallest gap
    floatSum 1M            8    300      38×
    stringHash 100K        2    131      66×
    collect 10x100K       41    307     7.4×
    select 10x100K         8    645      81×

Per doc's recommendation, smallest gaps first.  The 100K-allocations
bench (2.6×) is the most achievable.

**Hot path of 100K allocations**: `100000 timesRepeat: [Array new: 10]`.
The compiler inlines `timesRepeat:` to a bytecode-level loop, so the
hot path per iter is:

1. `Array new: 10` → `Behavior>>new:` (trivial wrapper).
2. `Behavior>>new:` → `self basicNew: anInteger` (= prim 71).
3. Prim 71 (`primitiveNewWithArg`) allocates the Array.

`PHARO_PRIM_PROFILE=1` confirms prim 71 dominates at 1.32M calls
(13 × 100K iters).

The asmjit-T1 inline-prim infrastructure exists for primKind 14
(at:), 15 (at:put:), 16 (size), 20 (identityHash), but NOT for
primKind 18 (basicNew:).  Adding it requires emitting an inline
allocator: header check + format validation + bump-allocate + slot
init + sp adjust.  ~50 instructions in the fast path + slow-path bail.

Sista's "trivial method inlining" (Behavior>>new: → basicNew:) would
collapse the 2-send chain into 1.  That's Step 8.4 / Step 4 territory.

**Smallest concrete change to close 100K-allocations**: inline-prim
primKind 18 in asmjit-T1's nArgs=1 IC HIT branch (mirrors the
existing primKind 14 emit).  Estimated 1-2 days of work to land
correctly with all the format-validation edge cases.  Deferred —
documented as the next concrete leverage point.

### Followup leverage list

In gap order (smaller = more achievable):

1. **100K allocations (2.6×)** — inline-prim 18 (basicNew:).  ~1-2 days.
2. **collect 10x100K (7.4×)** — inline `Array>>do:` + block-value spec?
3. **sieve x100 (10×)** — primCallCount on its inner loop.
4. **dict 50K (30×)** — likely `at:put:` polymorphic IC + hashtable
   rehash path.
5. **sort 100K (37×)** — block-comparator dispatch (Step 8.4 territory).
6. **fib (48×)** — Step 7 cold-start fix + Step 8 hot-path.
7. **sum 1M (81×)**, factorial, instVar, select — large gaps,
   harder to close.

### Important caveat: cold-start vs steady-state

Per-invocation bench-suite numbers include JIT compile time.  Warmed
steady-state numbers tell a different story:

- **sieve x100 steady-state (with 3× warmup): ours 7-9 ms.**
  Bench-suite cold: 100 ms.  So ~90 ms of the gap is JIT compile.
  Cog's bench-suite cold: 10 ms.  Cog likely has either much faster
  compile or already-cached compiles within a session.

So many of the "small gap" benches (sieve, etc.) are mostly **JIT
compile speed** in cold-start, not steady-state execution.  Closing
the cold-start gap requires faster JIT compilation, not bytecode
optimization.

Per-bench attack plan, refined:

- **Cold-start dominated (sieve, ...)**: optimize JIT compile speed.
  Profile via `scripts/run_benchmarks.sh` cold timing minus a
  warmed-image measurement.
- **Steady-state dominated (fib)**: per-activation cost (Step 8).
- **Both (sort, dict, collect)**: workload-specific — block-value
  spec, polymorphic IC walk (Step 8.3-shaped), trivial-method
  inlining (Step 8.4 territory).

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
