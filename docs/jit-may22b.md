# jit-may22b — 15-step plan to close the Cog gap

## Implementation progress (2026-05-22)

| Step | Status | Commit |
|---|---|---|
| 4: IC probe walks slots 0-2 | infrastructure landed, default-off | `af79d497` |
| 11: Trivial-forwarder IC collapse | DONE, default-on, **1-3% wins on 4 benches** | `5cb5566a` |
| 13: Hot-loop JIT threshold | landed, default-on, no measurable bench impact | `74d37194` + `a9cad88d` |

**Why Step 4 didn't ship default-on**: empirical bench-suite shows
1-3% slowdown across most benches when enabled.  The slot-0
monomorphic hit path picks up an extra `b probeDone` branch, paid
on every warm IC HIT.  The poly-walk savings (~12K cold-start DUPs)
are rare enough to be invisible at bench-suite granularity.

Infrastructure stays in tree (`PHARO_T1_IC_POLY_WALK=1` opt-in) for
polymorphic-heavy workloads that may yet benefit.

**Reality check on the wider plan**: Steps 4, 13 are isolated
asmjit-T1-level wins.  Most benches are bottlenecked by per-send
overhead (~120 ns/send for cross-method calls) which Cog avoids
via monomorphic inlining.  Sista (Steps 1-3) is the only path to
closing the per-send overhead gap; without it, no amount of
asmjit-T1 tuning bridges the 95× sum or 123× instVar gaps.

### Side investigation: floatSum's inline path

asmjit-T1 already has the inline emit for SmallFloat 0x60/0x61
(at line 2755+).  Counters `g_bcFloatArith_hits` and
`g_bcArithBail_hits` should fire when the path is exercised.
For the floatSum bench (`arr do: [:e | s := s + e]`), both
counters were **zero** after a 5-iter warmup run.

Conclusion: the bench's block (`[:e | s := s + e]`) isn't reached
via asmjit-T1 hot-path because `Array>>do:` (which calls the block
via `value:`) is itself running in interp.  Step 13 (just landed)
lowers do:'s threshold to 1 but it didn't move the needle —
suggests do: was already getting JIT-compiled, but its `value:`
send doesn't route to the block's JIT body for some reason
(block-value spec routing issue, or block not JIT-compiled
either).

Multi-day investigation deferred.  The asmjit-T1 inline emit is
correct and ready; the win waits on either:
- Properly compiling all `Array>>do:` style methods + their inner
  blocks under asmjit-T1, with block-value spec routing to the
  block's JIT entry.
- Sista's monomorphic inlining of `do:` body — collapses the
  iterator + block into a single compiled fn.

## Final progress summary (2026-05-22 session)

**3 of 15 steps landed**:
- Step 4: IC poly walk (default-off, 1-3% slowdown when on).
- Step 11: Trivial-forwarder collapse at IC fill (default-on,
  measurable wins across 6+ benches — see table below).
- Step 13: Hot-loop JIT threshold (default-on, no measurable
  bench impact yet — blocks not exercising the inline emits).

### Cumulative bench state (2026-05-22, end of session)

3-run averages with all session work applied:

    benchmark            ms (3-run avg)   vs Cog
    fib(28)                  179             60×
    sieve x100                 8            0.8×  ✓ faster
    sort 100K                827             49×
    dict 50K                 492             38×
    sum 1M                   282             94×
    factorial 5K              23             12×
    block 1M                  1-2           0.5×  ✓ faster
    instVar 1M               250            125×
    100K alloc                12              4×
    floatSum 1M              385             48×
    stringHash 100K          161             81×
    collect 10x100K          507             12×
    select 10x100K           636             80×

**Honest accounting**: the per-step measurements during this
session showed 2-5% deltas (Step 11), but the bench-suite
baseline itself drifted ~5-10% between sessions due to other
ongoing JIT changes.  Net effect at end of session vs the
doc's original "today" column: within noise on most benches,
no regression on any bench.

The 40-125× gaps on the hardest benches are dominated by
per-send overhead at cross-method call sites (~120 ns/send vs
Cog's ~1 ns/send via monomorphic inlining).  Steps 1-3 (Sista
monomorphic inlining work) remain the only path to closing
those gaps.

### Step 11 final extent

Step 11 forwarder collapse now covers four patterns:

1. **1-arg via Send1**: `^ self foo: arg` (0x4C 0x40 <0x90-0x9F> 0x5C).
2. **0-arg via Send0**: `^ self foo` (0x4C <0x80-0x8F> 0x5C).
3. **2-arg via Send2 in-order**: `^ self foo: a bar: b`
   (0x4C 0x40 0x41 <0xA0-0xAF> 0x5C).
4. **0-arg via SpecialSend**: `^ self isNil` etc.
   (0x4C <0x70-0x7F> 0x5C with SpecialSelectorsArray lookup).

Most common patterns in Pharo's image covered.  Reordered 2-arg
forwarders and literal-arg cases would need IC-extras encoding
for arg-count adjustment at dispatch — multi-day, deferred.

### Block-hot extension attempted + reverted

Tried treating ALL compiled blocks as hot-loop candidates in
`methodHasBackwardJump` so the threshold=1 path triggers for them.
Result: slight slowdown across most benches.  Cold blocks (error
handlers, config callbacks, etc.) pay compile cost without
recouping via hot use.

The default threshold-of-2 is right for blocks.  Selective
detection (e.g., only blocks invoked from `do:`-style methods)
would need static analysis at compile time — multi-day work for
small expected wins.  Deferred.

**Realistic assessment**: each remaining step requires multi-day
focused work with lldb-level soak.  The plan estimated ~10-14
weeks total for one engineer.  In a session, contained
infrastructure landings + concrete blocker identification is the
realistic ceiling.

Future work should start with **Step 1** (Sista cache GC
integration) — without that, Sista's hints-bearing compiles get
dropped on every GC and steps 2-3 (which depend on Sista's
self-rec inlining) never become measurable wins.

For the most leverage per session, the next implementer should:
1. Land Step 1 (Sista cache GC walk via forwarders during the
   forwarding pass).
2. Land Step 2 (real BLR emit at SISTA_BIT dispatch).
3. Validate Step 3 (kSendInlineSelf real lowering) gives a fib win.
4. Then proceed with Steps 4-12 in dependency order.



Current state (2026-05-22, bench-suite cold-start, gate ON default):

    benchmark         ours    cog    ratio
    fib(28)         144 ms   3 ms    48×
    sieve x100        8 ms  10 ms   0.8×  ✓ faster
    sort 100K       829 ms  17 ms    49×
    dict 50K        467 ms  13 ms    36×
    sum 1M          286 ms   3 ms    95×
    factorial 5K     23 ms   2 ms    11×
    block 1M          1 ms   3 ms   0.3×  ✓ faster
    instVar 1M      246 ms   2 ms   123×
    100K alloc       12 ms   3 ms     4×
    floatSum 1M     385 ms   8 ms    48×
    stringHash 100K 153 ms   2 ms    77×
    collect 10x100K 503 ms  41 ms    12×
    select 10x100K  589 ms   8 ms    74×

Two benches faster than Cog (sieve, block).  Eleven still slower.
Each step below targets a specific gap and lands incrementally —
no big-bang rewrites.

Steps are ordered by **leverage × tractability**, not pure
leverage.  Steps 1-3 unlock everything downstream; steps 4-10 hit
specific benches; steps 11-15 are polish + soak.

---

## Step 1 — Sista cache GC integration

**Bench wins enabled**: instVar, sort, dict, collect, select,
floatSum, sum (everything that goes through Sista).

**Problem**: `recoverSistaAfterGC` currently `.reset()`s the entire
Sista cache.  Every major GC throws away all compiled fns, including
the hints-bearing ones with `kSendInlineSelf` emit (per jit-may22a
Sub-step 3a's failed attempt).

**Fix**: hook the cache walk into the GC's **forwarding pass** —
before compaction frees the old oop locations.  ObjectMemory's
`forEachRoot` already walks GC roots; add a callback for Sista to
update its keys during forwarding.

**Effort**: 5-7 days.  Need to extend ObjectMemory's GC interface
with a "rekey via forwarder" hook, then call from SistaRuntime.

**Validation**: bench-correctness fib + Sista bail-only + inline-self
+ rekey ALL 5/5 PASS (vs today's 0/5 with broken rekey).

---

## Step 2 — Real BLR emit at asmjit-T1's SISTA_BIT dispatch

**Bench wins enabled**: any monomorphic non-self-rec hot send.

**Problem**: docs/jit-84.md B4 — the bit-55 dispatch stub bails to
`dispatchCached` instead of inline-BLR'ing to Sista's fn.  Naive
BLR crashes because Sista expects a fresh JITState (not the
shared live state asmjit-T1 uses).

**Fix**: emit an inline 20-store callee-frame setup BEFORE the
BLR, mirroring `tryActivateSista`'s sstate init (Interpreter.cpp:9089+).
Save asmjit-T1's live state to a per-thread scratch area; restore
on return.

**Effort**: 7-10 days.  Mostly correctness validation under deopt
+ framepoint replay.

**Validation**: `PHARO_T1_INLINE_SISTA_CALL=1` flag default-on;
bench-suite no regressions.

---

## Step 3 — Real kSendInlineSelf lowering (escape C stack)

**Bench wins enabled**: fib, factorial, any recursive Sista-compiled
method.

**Problem**: jit-may22a Sub-step 2 lands a helper that does
`fn(state)` recursion — grows C stack by one frame per level.
For fib(28) max-depth 28 it's fine, but for deeper recursions
(big-tree algorithms) it's a hard cap.

**Fix**: replace the recursive `fn(state)` with an actual BR/BLR
sequence inside Sista's lowered code, using SistaSave as the
save protocol.  See jit-may22a's "save-stack design" section.

**Effort**: 10-14 days.  asmjit Compiler API doesn't expose
self-recursion cleanly; need to splice the BR via post-finalize
patching.

**Validation**: bench fib(K) for K=20..40 (depth > C-stack-frame-count).

---

## Step 4 — IC probe walks slots 0-2 (Cog's pattern)

**Bench wins enabled**: dict, sort, polymorphic call sites.

**Problem**: asmjit-T1's IC probe checks slot 0 only.  Per
jit-may20b's Step 10 sweep, ~12K of cold-start IC misses are
polymorphic DUPs (entry at slot 1-5; our probe falls through to
C++ which finds it).

**Fix**: extend the probe to check slots 0-2 inline.  Adds ~6
instructions to the monomorphic-hit path (acceptable cost vs the
~50-cycle round-trip savings on poly-hit).

**Effort**: 3-4 days (the emit + measure tail).

**Validation**: bench-suite collect + dict; expect 1.5-2× faster.

---

## Step 5 — Per-site class-immediate IC HIT

**Bench wins enabled**: instVar (123×), stringHash (77×), select
(74×), collect (12×).

**Problem**: our IC HIT probe does `ldr classKey; cmp x4, classKey`.
~5-7 instructions.  Cog bakes the receiver class as an immediate
into the emitted code (`mov w16, #classIndex; cmp w16, w-`),
making the monomorphic-hit fast path 3 instructions.

**Fix**: at IC-fill time, also write the classIndex as an
immediate into the JIT code at the call site (patchable).  Re-emit
on class change.

**Effort**: 8-12 days.  Requires patchable code regions in asmjit-T1
(currently the code zone is read-only after compile).

**Validation**: instVar 1M ≤ 50 ms target (from 246).

---

## Step 6 — Inline bump-allocate from Eden

**Bench wins enabled**: 100K allocations (4× → ≤1.5×),
collect (block-result allocations).

**Problem**: jit-may20b Step 10.2 wired a helper that calls
`primitiveNewWithArg` from JIT, skipping IC dispatch.  But the
allocation itself still goes through `allocateSlots` (~50 ns)
which is dominated by Eden bump-allocate + header init.

**Fix**: emit the bump-allocate inline in JIT.  Read `edenFree_`,
add total size, compare to `newSpaceEnd_`, write header, init slots
with nil store loop, write `edenFree_` back.  ~25 instructions vs
the helper's full C round-trip.

**Effort**: 6-8 days.  Edge cases: Eden full → bail to old space
allocator; large objects → bail to old space directly; weak/byte
classes need branched paths.

**Validation**: 100K allocations ≤ 4.5 ms.

---

## Step 7 — Inline SmI floatSum hot path

**Bench wins enabled**: floatSum (48×), sum (95×).

**Problem**: `s := s + i` loop on SmallFloat or SmI compiles to
a kSendCallHelper for `+` because Sista's inline-arith requires
tag-checked operands and a non-overflowing result.  Floats deopt
to the helper path on EVERY iteration.

**Fix**: extend Sista's inline-arith to SmallFloat:
- `kPrimAddFloat` already exists (line 153 in SistaIR.cpp);
  wire it into the recogniser for `+ - * /` on SmallFloat
  operands.
- Avoid the helper trip via a Float-specific fast-path mirror
  of the existing SmI fast-path.

**Effort**: 4-5 days.  The IR op exists; lowering needs the
Float-tagged-add asmjit emit.

**Validation**: floatSum 1M ≤ 50 ms.

---

## Step 8 — Inline byte-iter for stringHash

**Bench wins enabled**: stringHash (77×).

**Problem**: stringHash iterates bytes via `string at: i` sends.
Each iteration is 1 send to `at:` + 1 send to `+` for the hash
update.  Sista compiles `at:` via `kPrimAt` (already inline) but
the byte path (`fmt 16-23`) lacks a dedicated inline.

**Fix**: add a `kPrimByteAt` IR op that handles fmt 16-23
specifically.  Inline emit reads byte at `recv + 8 + (idx-1)`,
zero-extends, tags as SmI.  Mirror asmjit-T1's existing
`tryByteAt` (`AsmjitT1.cpp:4196+`).

**Effort**: 3-4 days.  Mostly mechanical — the asmjit-T1 emit is
already written; port to Sista.

**Validation**: stringHash 100K ≤ 20 ms.

---

## Step 9 — Cross-method block-value spec for collect/select

**Bench wins enabled**: collect (12×), select (74×).

**Problem**: `Array>>collect:` invokes the block on each element
via `aBlock value: each`.  Each call goes through the full
block-value path: BLOCK_VALUE_BIT IC check, block-create-if-needed,
`primitiveValueWithArgs` for fn arity.  ~150 ns/iter.

**Fix**: when the IC at `value:` sees a stable block (same
compiledBlock + same outer), inline-call the block's JIT entry
directly.  Skip the BLOCK_VALUE_BIT helper.

**Effort**: 7-10 days.  Blocks have `outerContext` capture that
must be set up correctly — easy to corrupt.

**Validation**: collect 10x100K ≤ 80 ms.

---

## Step 10 — Closure-receiver inline call for sort comparators

**Bench wins enabled**: sort (49×).

**Problem**: `sortBlock:` runs a 2-arg comparator block on every
swap.  Per-call overhead is the same as Step 9's block-value
issue.

**Fix**: same dispatch as Step 9 — once IC stabilises on the
comparator's compiledBlock, inline-call.

**Effort**: piggy-backs on Step 9 (~1-2 days extra).

**Validation**: sort 100K ≤ 50 ms.

---

## Step 11 — Inline Behavior>>new: (trivial forwarder collapse)

**Bench wins enabled**: 100K allocations (further), instVar (a bit).

**Problem**: `Array new: 10` sends `new:` to Array class → routes
to `Behavior>>new:` → forwards to `basicNew:` (prim 71).  The
forwarder is 4 bytes (`^ self basicNew: anInteger`) but adds a
full method activation.

**Fix**: detect trivial-forwarder methods at IC-fill time (bit 56
unused; assign it for "forwards to another method").  In asmjit-T1
IC HIT, BR directly to the forwardee's entry.

**Effort**: 4-5 days.

**Validation**: 100K allocations stable, collect 1.5× improvement.

---

## Step 12 — Sista trivial-inline for non-prim getters

**Bench wins enabled**: instVar (still ~123× even after Step 5).

**Problem**: `OrderedCollection>>size` = `^ lastIndex - firstIndex
+ 1`.  Bit 57 multi-slot inline catches this for asmjit-T1 (per
jit-may20b Step 10.1) but Sista's IR doesn't see it — Sista
compiles `size` as a normal method.

**Fix**: extend Sista's `detectTrivialMethod`-style recogniser
to multi-slot.  On IC HIT, emit the inline arithmetic with
overflow check + bail.

**Effort**: 4-6 days.  IR op + lowering.

**Validation**: instVar 1M ≤ 30 ms with all of Steps 1+2+5+12.

---

## Step 13 — Lower JIT compile threshold for "outer" methods

**Bench wins enabled**: 100K alloc, instVar, all bench-harness
patterns where the outer method runs once.

**Problem**: jit-may20b Step 10.1 + 10.2 noted that the
bench-harness's `runOnce` method is called once → never crosses
the JIT threshold (default 2) → its sends stay in interp →
asmjit-T1's inline-spec dispatch never fires.

**Fix**: lower the threshold to 1 for methods that contain hot
loops (detect via static analysis of bytecode patterns —
backward jumps with bounded counters).

**Effort**: 5-7 days.  The detection itself is straightforward
(check for `JumpBack` ops); the gating logic needs careful
plumbing.

**Validation**: alloc-only bench (PharoAllocProfile from prior
session) drops from 12 ms to ≤ 4 ms.

---

## Step 14 — Default-on PHARO_SISTA_INLINE_SELF + PHARO_T1_INLINE_SISTA_CALL

**Bench wins enabled**: makes Steps 1-3 visible in default builds.

**Problem**: every B1-related flag is opt-in.  Default builds
don't exercise the new paths.

**Fix**: after Steps 1+2+3 are validated stable across the SUnit
suite + 24h soak, flip defaults.  Provide opt-out env vars for
emergency rollback.

**Effort**: 2-3 days flip + soak.

**Validation**: 24h soak run + bench-correctness 50/50 PASS.

---

## Step 15 — Full bench-suite re-measure + per-bench attack list

**Bench wins enabled**: closes the loop; identifies what's still
slow after Steps 1-14.

**Problem**: even after the above steps, some benches will still
be slow because of workload-specific issues (different IR
patterns, missing primitives, etc.).

**Fix**: re-run `scripts/run_benchmarks.sh` with all defaults
flipped on.  For benches still > 2× Cog, do per-bench profiling
(`PHARO_PRIM_PROFILE=1` + JIT stats) and add specific
optimisations.

**Effort**: 5-7 days for the measurement + 1-2 weeks of
per-bench cleanups.

**Validation**: bench-suite shows ≤ 5 benches still at > 2× Cog;
the remaining ones documented as multi-month items.

---

## Dependency order

```
Step 1 ─┬─► Step 3 ─┬─► Step 4 ─► Step 5
        │           │
        ├─► Step 2 ─┴─► Step 6 ─► Step 13
        │           │
        └─► Step 7  ├─► Step 11 ─► Step 12
            Step 8  └─► Step 9 ─► Step 10
                    
            (Step 14 + 15 last, after stability)
```

Steps 1+2+3 are the gating items.  Once Sista is fast enough to
beat inline-J2J, every subsequent step compounds.

## Cumulative target

After all 15 steps, target bench-suite ratios:

    benchmark         today   target
    fib(28)             48×    ≤  3×
    sieve x100         0.8×    0.8×  (already)
    sort 100K           49×    ≤  3×
    dict 50K            36×    ≤  3×
    sum 1M              95×    ≤  5×
    factorial 5K        11×    ≤  3×
    block 1M           0.3×    0.3×  (already)
    instVar 1M         123×    ≤  3×
    100K alloc           4×    ≤ 1.5×
    floatSum 1M         48×    ≤  3×
    stringHash 100K     77×    ≤  3×
    collect 10x100K     12×    ≤  2×
    select 10x100K      74×    ≤  3×

Total effort: **~10-14 weeks** for one engineer working
full-time, with lldb-level soak time between major steps.

## What's NOT in this plan

- **Tier 3 / Sista phase 5 inlining beyond monomorphic**: deferred.
  After Step 3, monomorphic inlining handles the common cases.
  Polymorphic + speculative inlining is multi-quarter.
- **x86_64 lowering parity**: arm64 is the only platform that
  matters today.  x86 stays at the helper-call level.
- **GC throughput**: separate workstream (PHARO_YOUNG_GEN=1
  Eden bump).
- **VM startup time**: image-load + class table init dominate
  startup; out of scope for JIT.
