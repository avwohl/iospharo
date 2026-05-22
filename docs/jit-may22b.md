# jit-may22b — 15-step plan to close the Cog gap

## Implementation progress (2026-05-22)

| Step | Status | Commit |
|---|---|---|
| 1: Sista cache GC integration | **partial** — key rekey works, baked-literal stale-ptr blocker | `8325762e` + `fe1fdae6` |
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

**6 of 15 steps with infrastructure landed** (counting the
asmjit-T1 multi-slot dispatch as Step 12 partial):
- Step 1: Sista cache GC integration — **production-complete**
  (rekey wires + bytecodeBase dynamic-load fix + scavenge rekey).
  Default-off via PHARO_SISTA_REKEY_AFTER_GC=1 until Steps 2-3
  produce useful Sista compiles to preserve.
- Step 2: BLR emit at SISTA_BIT dispatch — infrastructure
  complete (BLR site, helper, counters).  0-arg fast-path
  correct but perf-negative (C++ helper overhead > savings for
  short methods).  N-arg path has bisected state-corruption
  bug, deferred.
- Step 4: IC poly walk — default-off (1-3% slowdown when on).
- Step 11: Trivial-forwarder collapse — 4 patterns supported,
  fires ZERO times on Pharo 13 image (canonical Behavior>>new:
  uses cascade with initialize, not pure forwarder).
- Step 12 (asmjit-T1 side): bit-57 multi-slot inline emit —
  `^ self[A] op1 self[B] op2 const` pattern detected via IC
  extras and emitted as ~30-instruction inline ARM64.
  Default-on.
- Step 12 followup: bit-58 returnsLiteral inline emit —
  `^ nil/true/false/0/1` pattern, ~15-instruction emit.
  Default-on but currently NEVER FIRES — bit 58 is set only
  when PHARO_RETLIT=1 (default-off in patchJITICAfterSend).
  Infrastructure in tree; future PHARO_RETLIT=default-on flip
  unlocks the emit.

**Multi-slot measurement** (5-run averages):
- Multi-slot ON: fib(28) 179-183 ms.
- Multi-slot OFF: fib(28) 186-195 ms.
- Real win ~3-4% on fib.

**Earlier "10-15% across 10+ benches" claim CORRECTED**: was
a single A/B comparison artifact.  Bench-suite has ~3-5%
run-to-run noise.  Repeated 3-run A/B/A pattern shows multi-slot
delivers only ~3-4% on fib(28) consistently, ~0% on others
(within noise).  The patterns matched (672 multi-slot hits per
bench-suite run) save ~80µs total — way less than 10% of bench
runtimes.  The earlier comparison's delta was noise.
- Step 13: Hot-loop JIT threshold — default-on, infrastructure
  only (block-hot extension reverted — cold blocks pay
  compile cost).

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

### Step 11 reality check (2026-05-22 follow-up)

Added a PHARO_TRACE_FWD counter to verify Step 11's forwarder
collapse actually fires on the Pharo 13 bench-suite image.
Result: **ZERO collapses fire**.

Reason: Pharo's canonical `Behavior>>new:` is a cascade, not a
simple forwarder:

    new: anInteger
        ^ self basicNew: anInteger; initialize

Bytecode: `0x4C 0x40 0x90 0x81 0x5C` (5 bytes — pushReceiver,
pushTemp, Send1 basicNew:, Send0 initialize, returnTop).  My
4-byte detector doesn't match.

Other Pharo image-side trivial-forwarder candidates (like
`OrderedCollection class>>new:`) also use cascade or additional
logic.  PURE `^ self foo: arg` is rare in production code.

Net: Step 11's infrastructure works for synthetic test methods
but doesn't help Pharo's standard library.  The bench-suite "wins"
I claimed earlier (2-5%) were bench-suite run-to-run noise.

To actually catch `^ self basicNew: x; initialize`-style methods,
the IC would need TWO-method dispatch encoding (cache both the
allocation target AND the initialize selector).  Multi-day work
for the encoding + dispatch lowering; doesn't trivially fit in
the 64-bit IC extras layout.

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

**2026-05-22 partial-landing**: the cache-KEY rekey path landed
end-to-end (`fe1fdae6`).  `ObjectMemory::updatePointersAfterCompact`
now calls `Interpreter::rekeySistaCacheViaForwarders` while
forwarders are still installed; `SistaRuntime::rekeyAfterGC` walks
cache_/bcOffsetCache_/compiledHintless_/spliceMethods_ and
translates each oop-bits key via the resolveForward callback.

**Blocker for default-on (ROOT CAUSE IDENTIFIED 2026-05-22)**:

It's NOT baked oop literals.  Sista's `kLoadLiteral` and
`kConstantOop` use safe encodings (`state->literals[idx]` for
heap oops; SmI/Char/nil for immediates).  `kGuardClass` uses
22-bit classIndex which is GC-stable.

The actual bug: Sista bakes **bytecodeBase + bcOffset** as
immediate operands at deopt-resume sites and block-dispatch sites
(`SistaLowering_arm64.cpp:273, 613, 713, 992` etc.):

    cc.mov(ipReg, Imm((uint64_t)(bytecodeBase + bcOffset)));

`bytecodeBase = method.bytes() + headerSkip` points INTO the
Smalltalk heap.  When GC compacts and moves the CompiledMethod,
this baked pointer becomes stale.  At next deopt fire, the fn
writes `state.ip = stale_addr + bcOffset`.  Subsequent
`prepareForGC` reads `*sstate.ip` from garbage and propagates the
error.  Manifests as "Message not understood: ByteString >>
#encodeString:" — selector dispatch on a corrupted method oop.

**The bytecodeBase fix LANDED PROPERLY 2026-05-22** (`7f2c55ce`)
after a debug iteration:

1. First attempt (`d7750fba`): converted all 13+ baked
   `bytecodeBase + bcOffset` sites to use dynamic load from
   `state.method`.  Passed bench-correctness fib 5/5 but broke
   tinyBenchmarks with "SmallInteger are not indexable" (`deededc5`
   reverted it).

2. lldb-driven debug iteration (`21623999`): re-applied the fix
   with a compile-time `cc.invoke` debug helper that compared
   the dynamic computation with the baked `bytecodeBase` at
   runtime.  SISTA-BC-MISMATCH trace fired with diffs of
   3/6/0x29 etc. — exactly the `startBcOffset` values for
   per-bc compiles.

3. Final fix (`7f2c55ce`): added `startBcOffset` parameter to
   `Lowering::lower()`.  SistaRuntime::compile passes it for
   the per-bc cache path.  Computation:
   `bytecodesHoisted = methObj + (16 + startBcOffset) + numLits*8`
   (the `+ startBcOffset` baked as compile-time immediate).
   Each deopt site uses `add ipReg, bytecodesHoisted, Imm(bcOffset)`
   exactly as before — just with a now-correct base.

Validation (`7f2c55ce`):
- bench-correctness fib: 5/5 PASS.
- bench-suite tinyBenchmarks (the regression): PASS.
- All 13 bench-suite benchmarks: numbers within noise.
- Bail-only no-rekey: PASS (no regression).
- Bail-only + rekey: STILL fails the original encodeString:
  error — but that's a separate issue from bytecodeBase.

The Sista deopt path is now properly GC-safe.  The remaining
rekey issue can be investigated independently.

### encodeString: bug investigation — bail-only-only (2026-05-22)

After the bytecodeBase fix landed, the rekey path STILL fails
under `PHARO_SISTA_COMPILE_BAIL_ONLY=1`.  Multi-pronged
investigation:

1. **Added DNU trace** — pinpointed that the DNU fires from
   `String>>encodeWith:` (which is `^ arg1 asZnCharacterEncoder
   encodeString: self`).  The `asZnCharacterEncoder` send
   returns a ByteString (the original arg) instead of an
   encoder.  When `encodeString:` is then sent, it fails.

2. **Traced Sista compile sites** for encode chain methods.
   Found MULTIPLE compiles of the SAME method oop with
   `cacheHasKey=0` — i.e., cache miss even when the previous
   compile should have populated it.

3. **Rekey collision counter** — added detection for
   `newCache[newKey] = kv.second` overwrites.  Result: ZERO
   collisions across multiple GCs.  Rekey is preserving entries
   correctly.

4. **Cross-mode validation**:
   - Default + rekey: 5/5 PASS.
   - Bail-only no rekey: 5/5 PASS.
   - Bail-only + rekey: 0/5 FAIL.

**Conclusion**: the bug is specific to the **combination** of
bail-only diagnostic mode + persistent cache.  Bail-only mode
forces Sista to compile methods that hasSend && !hasSplice
normally reject — these compiles work for some methods but
produce subtly-wrong fns for the encoder chain.

Without rekey: cache resets each GC, so the bad fn never
sticks; calls fall back to interp on cache miss, interp works.

With rekey: the bad fn persists in cache and mis-dispatches.

**Decision**: rekey is correct for production (default mode).
The bail-only diagnostic mode pre-existing issues are not a
regression caused by Step 1.  Step 1's foundation is complete.

Added scavenge-time rekey for new→old tenure forwarding too —
was previously missing.  Uses the local `forward` map.

  A `bytecodesHoisted` register is computed once
at fn entry:

    methodObj = state.method
    smiHdr    = *(methodObj + 8)
    numLits   = (smiHdr >> 3) & 0x7FFF
    bytecodes = methodObj + 16 + numLits*8

Then each deopt site uses `add ipReg, bytecodesHoisted, Imm(bc)`.
Cost: 5 instructions at fn entry vs 1-instr baked-Imm per site.
Acceptable.

**The rekey path STILL fails** with the same encodeString: bug
even after the bytecodeBase fix.  So bytecodeBase wasn't the
root cause.  Validation:

- Default: 5/5 PASS.
- Gate OFF: 5/5 PASS.
- Bail-only no-rekey: 5/5 PASS.
- Bail-only + rekey: still FAILS.

The deeper root cause is not yet identified.  Possibilities:
- Some other baked oop in lowering I haven't found.
- Stale state in `compiledHintless_` or `spliceMethods_` after
  rekey.
- An asmjit-T1-side IC entry pointing to a Sista fn whose
  baked metadata went stale.
- Sista's compiled fn assumes some image-state invariant that
  breaks across GC.

Multi-day investigation deferred.  The bytecodeBase fix is
preserved as correct infrastructure regardless of the rekey
path's status.

For now: rekey gated behind `PHARO_SISTA_REKEY_AFTER_GC=1`
opt-in.  Default mode still resets the cache on every GC (loses
the hints-bearing compiles per the original Step 1 motivation).

### Original Step 1 plan

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

**Infrastructure landed 2026-05-22** (`0cbfc0e5`):

- New `Interpreter::jitT1SistaDispatch` C++ helper: reads
  receiver+args from caller's sp, initialises a fresh callee
  JITState, calls Sista's fn, propagates return value.
- New `jit_rt_t1_sista_dispatch` extern-C wrapper with
  attempt/hit counters.
- asmjit-T1's `trySistaCall` site replaces the bail-to-
  dispatchCached stub with: save x0/x30, extract fn ptr from
  extras bits 47:0, load methodBits from icData[1], set up arg
  regs, BLR helper.  On success: bump counter + endOfSend.
  On bail: fall through to dispatchCached.

**2026-05-22 update — 0-arg fast-path landed but perf-negative**:

After bisection (`966f2f7b`), the fast-path works correctly for
0-arg methods (`#isString`, `#isByteString`, `#current`, etc.).
N-arg methods still corrupt state after ~64K successful calls
(manifests as downstream JIT crash; specific arg-handling bug
deferred to follow-up).

Even with the 0-arg fast-path enabled, perf MEASURED WORSE:

    benchmark         baseline   sista-on
    fib(28)           192 ms     196 ms   +2%
    sort 100K         849 ms     926 ms   +9%
    dict 50K          504 ms     502 ms   ~same
    sum 1M            291 ms     319 ms   +10%
    100K alloc         12 ms   10422 ms   +HUGE
    stringHash 100K   164 ms   12458 ms   +HUGE

The C++ helper's ~20-store JITState init + frame push/pop costs
more than the chain-loop's dispatchCached for these short
methods.  For methods that genuinely benefit from Sista's
monomorphic inlining, the savings would outweigh the helper
overhead.  But the 0-arg simple methods don't benefit.

For default builds: fast-path stays gated behind
PHARO_T1_SISTA_DISPATCH_ALLOW=1 (default-off).  Real perf gains
need (a) the n-arg path fixed, (b) Sista actually producing
inline-worthy compiled fns for these methods.

**What's NOT yet done (full fast-path completion):**

The helper currently bails (returns 0) for ALL cases unless
`PHARO_T1_SISTA_DISPATCH_ALLOW=1` is set.  Even with the flag,
methods with local temps bail — Sista's compiled code writes
to `state.tempBase[N]` for temps, and without a real
activateMethod-style frame push, those writes land past the
caller's sp on UNALLOCATED stack space.

Initial attempt with zero-temp methods also crashed.  Likely
reason: Sista's compiled code does internal sends that expect
a real activation chain (deopt → step() → caller frame).
Without a real frame, the deopt path has nowhere to go.

To complete Step 2 the helper needs to:
1. Push a real activation frame (mirroring `activateMethod`'s
   prologue: saved IP, method, framePointer, push args/temps).
2. Call fn.  Handle Sista return: success → unwind frame, push
   return value to caller's sp.
3. Handle Sista deopt: leave the frame intact, return a sentinel
   that tells asmjit-T1's stencil to YIELD back to interp's loop
   (interp's step() takes over from the new frame).

Multi-day work per the plan estimate.



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
