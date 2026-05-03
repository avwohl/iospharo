when doing items in this list, take as long as you need to per item
when doing  /loop sleep the minimum 1min before picking the next item
 but keep per item time as large as needed for these items. Do not assume
 1min max per item because /loop sleep is 1min.
pick items in any order you think best
if you are stuck on something that needs a human do something else

# Multi-week JIT work choices

The single-iteration JIT improvements (splice extensions, IC bit-packing,
default-on flag flips, gate refinements) have largely been picked over.
The remaining bench-suite gaps and structural roughness need bigger,
focused efforts.  Each item below is multi-week — too big for one /loop
iteration, but bounded enough that one of them per session is realistic.

Items are roughly ordered by expected impact-per-week.  Each section
notes WHAT, WHY blocked, ESTIMATE, and RELATED memory/docs.

---

## 1. HELPER_SENDS scheduler architecture rework — **FULLY DONE 2026-05-02** (`31f1c640`)

The deeper rework wasn't actually needed — the symptom that motivated
it (sum 1M / runSum bailing out of helper-sends) was a single missing
line: helper-send entry didn't clear the stale `relinquishSlept_`
signal, so the very first step() call returned false unconditionally.

Fixed in `31f1c640`: clear `relinquishSlept_` on entry to
`jitSistaCallSend`, OR-restore on exit.  sum 1M now runs the closure-
accum splice cleanly: 100 ms → 0-1 ms (>100×, 5× faster than Cog 5
ms).  The `2a7e2a4e` workaround gate is removed (renamed to
`PHARO_SISTA_BLOCK_ARRAYDO_HELPER` opt-in for diagnostics).

**Original analysis (kept for context):**



Shipped via three precondition fixes in this session:
- `bd7adb87` Sista deopt-path operand double-stacking fix
- `2a7e2a4e` skip Sista compile for Array-do splice + helper-send methods
- `2e274c7d` flip `PHARO_SISTA_HELPER_SENDS` default-on (opt-out via
  `PHARO_NO_SISTA_HELPER_SENDS=1`)

Result: `1M blocks` drops from 13ms to **0 ms** (math-simplification
splice fires).  10/10 reliable under default flags.  All other benches
within ±2ms variance.

**2026-05-02 follow-up — `2a7e2a4e` workaround is cosmetic only:**
investigated whether the runSum bail+resume corruption can be fixed.
Empirically: with `PHARO_SISTA_ALLOW_ARRAYDO_HELPER=1` (gate disabled),
runSum is the ONLY method that triggers (per `[SISTA-ARRAYDO-HELPER]`
diagnostic — only `PharoBenchmarkRunner class>>#runSum` matches both
Array-do splice + kSendCallHelper).  runSum still produces the correct
result (`sum=500000500000`) and the bench-suite reaches DONE.  The
"do: nil" symptom is a 31-byte file-overwrite at the start of
`/tmp/pharo_benchmarks.txt` from `Smalltalk exitSuccess` shutdown
catching an error elsewhere.  Bench results intact.  Conclusion: the
narrowly-scoped 2a7e2a4e workaround is the right level of fix; further
investigation isn't justified by the cosmetic-only impact.

**Original analysis (kept for context):**



**What:** the helper-driven `step()` loop in `jitSistaCallSend` runs
nested interpretation under `inSyncSend_=true` to suppress process
switches.  This breaks the timer/scheduler invariant during long inner
sends — the Delay scheduler doesn't re-arm and the system idles to
death.  A clean fix needs either:

- (a) re-architect the helper to NOT drive `step()` directly — instead,
  schedule the inner activation as a normal frame and yield, with a
  callback marker so the helper continuation runs after the inner
  returns; OR
- (b) allow process switches mid-helper with proper save/restore of
  the helper's frame bookkeeping per-process (each process gets its
  own helper-state stack).

**Why blocked:**  3 single-iteration approaches were tried 2026-05-01
and all caused regressions: per-method auto-on (sum 1M hangs), step()-
budget bail (startup process termination), periodic-check pacing
(170× bench panel regression).  See
`memory/project_b1_helpersends_2026_05_01.md`.  The intrinsic issue is
that `inSyncSend_` is a global boolean — fine for a single helper-send
but breaks under recursion / process switches.

**Payoff:** unblocks default-on `PHARO_SISTA_HELPER_SENDS=1`.
Current opt-in gate (commit `ee3daf70`) gets 7/10 bench-suite
stability with `sum 1M` 100ms → 1ms (98×).  Default-on stable
would expand splice firing to all user methods with do:/inject:/collect:
patterns and prior setup-sends.

**Estimate:**  1-2 weeks for option (a), 2-3 weeks for option (b).
Option (a) is cleaner but requires touching `tryJITActivation`,
`pushFrameForJIT`, and the J2J save chain.  Option (b) bolts onto
existing helper but needs careful save/restore design.

**2026-05-02 sub-investigation + partial fix:**

- **Bug 1 (FIXED, commit `bd7adb87`):** kSendCallHelper deopt path was
  pushing operands TWICE.  Pre-helper code pushed v.operands onto
  state->sp.  When helper returned 0 (depth check, step()=false),
  the deopt's framepoint replay pushed values starting at state->sp
  (post-pre-helper-push), leaving operands DOUBLE-STACKED.
  Verified via SISTA-SEND trace: spDelta went 4 → 2 (correct).

- **Bug 2 (REMAINING, runSum-specific):** even with bug 1 fixed, sum
  1M still DNUs with `#do:` on nil receiver in runSum.  Workaround:
  `PHARO_SISTA_EXCLUDE_SELS=runSum` makes the bench-suite COMPLETE
  with HELPER_SENDS=1 + 1M blocks = 0 ms (was 13 ms — the math-
  simplification kCountedLoopWhileTrueAccum splice now fires).  So
  the HELPER_SENDS architecture works in general; runSum specifically
  hits another bug we haven't isolated.  Possibly in how Sista's bail
  for kCountedLoopArrayDoAccum splice (with `asArray` setup pattern)
  interacts with interp resume.

- **Stability under HELPER_SENDS=1 + EXCLUDE runSum (5 runs):** 5/5
  complete with sum 1M ~103 ms (interp), 1M blocks 0 ms, others
  match baseline within ±2 ms.

**Cycle guard status (2026-05-02 commit `3d5b53fa`):** the
materializeFrameStack cycle-break walk is now gated under
HELPER_SENDS=1.  Without helper-sends, the 200-deep walk per
context creation is wasted work.  Doesn't fix the underlying
HELPER_SENDS=1 bug but removes overhead from the default path.

**Related:** `memory/project_b1_helpersends_2026_05_01.md`,
`memory/project_helper_sends_gate.md`, `docs/deferred.md` §B7.

---

## 2. Phase 6 block inlining — **FIRST WIN 2026-05-02** (`aa6eaf97`); **2-arg scaffolding 2026-05-02 PM** (`8a0fcd77`)

**2026-05-02 PM scaffolding**: added `stencil_sendBlockValue2Arg`
for the `block value: a value: b` send shape.  Mirrors the existing
0-arg and 1-arg block-value stencils.  Wired into JITCompiler.cpp's
specialization loop (gated on BLOCK_VALUE_BIT, same as 0/1-arg).

**Bench-suite impact:** unchanged — sort 100K stays at 216 ms.

**2026-05-02 PM profiling findings:**

1. `BLOCK_VALUE_BIT` (extra bit 59) IS being set for `value:value:`
   IC sites — verified by `PHARO_TRACE_IC_PRIM=1` instrumentation in
   patchJITICAfterSend.  Trace shows
   `primIdx=207 sel=#value:value: extra=0x800000000000000` for the
   patch event.

2. The new `stencil_sendBlockValue2Arg` IS being chosen at
   compile time when `applyICSpecialization` finds a site with
   BLOCK_VALUE_BIT + nArgs=2.  block1>0 fires for at least one
   method per warmup run.

3. `mergeFirst:middle:last:into:by:` (the hot sort callee) is
   JIT-compiled (entry 2, ~28 KB) but NEVER reaches
   `applyICSpecialization` because it isn't recompiled.  Even at
   `PHARO_RECOMPILE_AT=1` (force recompile after 1 call), running
   sort 100K shows mergeFirst's IC has 9 sites but `0/9` are
   specializable — at recompile time the IC is empty (entries
   not yet filled with classKey/method).

4. The natural recompile trigger (executionCount >= 500) doesn't
   fire for mergeFirst — its callers (mergeSortFrom:to:src:dst:by:)
   don't hit the inline-bump in stencil_sendJ2J's j2j_direct_call
   often enough.  Likely the IC at mergeSortFrom's mergeFirst-call
   site stays at stencil_send / cold-IC dispatch rather than getting
   J2J-classified.

**Conclusion:** the 2-arg stencil scaffolding is correct.  The win
requires solving the recompile-trigger gap for callees of
recursive-J2J-driven hot paths.  Same architectural issue as
documented in `project_specialization_misses_doit.md` — IC
specialization fires too late for once-deep methods, where "once-
deep" means "called from a function that recurses heavily and
doesn't itself reach the threshold via non-recursive callers."

**IC-fill-triggered recompile boost attempt 2026-05-02 PM (reverted).**
Tried boosting the OWNER method's executionCount by 100 per
specializable IC fill in patchJITICAfterSend.  The boost code DOES
fire (verified via PHARO_TRACE_IC_FILL_BOOST) but only for methods
where `pendingICOwnerMethod_` is correctly set — typically generic
hot methods (#benchmark, #atAllPut:, #from:to:put:, etc.).
mergeFirst's value:value: IC patches DO NOT show up in the trace,
suggesting the IC-patch path for that specific dispatch doesn't
populate `pendingICOwnerMethod_` to mergeFirst.  Reverted; bench-
suite parity preserved.

**Real next step (multi-week):** warm-IC-detection that triggers
recompile when an IC site has been hit > N times, regardless of
the OWNING method's executionCount.  Plus tracking `pendingICOwner-
Method_` correctly across all IC patch dispatch paths (incl. the
JIT-exit-to-interp `Send2` + cache-hit + executePrimitive path
that handles value:value:).

**2026-05-02 PM per-site counter implementation attempt (reverted —
W^X blocker found):**

Tried the recommended (b) approach — extend IC stride from 152 to
160 bytes, adding a per-site hit-count slot.  Stencil bumps the
counter on each J2J hit; threshold-cross queues the OWNING method
(caller) for recompile.

Result: SIGSEGV on first JIT-compiled-method invocation.  Crash trace
points at the stencil's STR instruction (the counter increment's write).

Root cause: the IC area lives INSIDE the JIT code zone, which is W^X
protected on Apple Silicon.  Stencil execution mode is X (executable);
writes to the code zone require an explicit `pthread_jit_write_protect_np`
flip to W mode + restoration.  The existing IC patch path
(`Interpreter::patchJITICAfterSend`) does this via RAII guard.  But
inline stencil writes can't afford the per-call flip cost.

The per-CALLEE inline-bump that already exists (stencil_sendJ2J's
`*((uint8_t*)_calleeJM + 80)` write) WORKS because it writes to
`JITMethod::stats`, which is a heap pointer to a separately-allocated
JITMethodStats struct in regular (writable) memory — NOT the code
zone.

To make per-SITE counters work, the counter array must live OUTSIDE
the code zone:
- Side-table per JIT method: allocate `numICEntries × uint32_t` of
  writable memory; JITMethod gets a new `siteHitCounts` pointer.
  Stencil increments via `siteHitCounts[siteIdx]`.  Needs either a
  new HOLE per IC-using stencil (OPERAND3 = siteCountsBase + idx*4)
  OR derived computation from existing OPERAND2 (icBase) → JM →
  siteCountsBase.

This is several days of plumbing — every IC-using stencil
(sendJ2J, sendInlineMonoJ2J, sendInlineGetter/Setter/MultiSlot,
sendBlockValue0/1/2, etc.) needs the new HOLE wired in.

**Conclusion:** the cleanest architectural answer (per-site counter
in IC layout) is blocked by W^X.  The next-best is a side-table with
a new per-stencil HOLE — still cleaner than (a)'s patch-the-flag-
everywhere, but the up-front implementation cost is comparable.

**What's left in tree:** the IC layout extension (`521275aa`) stays
— it's harmless scaffolding (8 bytes per IC site, ~80 KB total
overhead, no functional change since no stencil writes to the new
slot).  Future per-site counter work can use `IC_HITCOUNT_SLOT`
once the W^X plumbing is figured out (likely as a side-table with
a stencil-side helper-call to increment).  Layout staying in tree
means the side-table approach won't need a stride re-extension
later.

**What got reverted:** the stencil increment + threshold-queue
code in `stencil_sendJ2J`'s `j2j_direct_call`.  That write was
the SIGSEGV trigger.

**2026-05-02 PM deeper investigation: the actual root cause is
mega-cache bypass.**

After dumping mergeFirst's IC sites at recompile time
(`PHARO_DUMP_MERGE_IC=1`), discovered:

  Site 0 (bcOff=8, send #at:)        → empty
  Site 1 (bcOff=12, send #at:)       → empty
  Site 2 (bcOff=34, send #value:value:) → EMPTY
  Site 3 (bcOff=44, send #at:put:)   → classKey=0x33 (SmI), J2J set
  Site 4 (bcOff=52, send #at:)       → classKey=0x33 (SmI), J2J set
  ... [other sites partially filled]

Site 2 (the value:value: site) **stays empty** even after sort
runs many calls through it.  Why?

The first call from mergeFirst's compiled code goes through
`stencil_sendJ2J`.  Cold IC → ic_miss → mega-cache probe.  Since
`#value:value:` is invoked from MANY methods (any sort, any
do-with-block, etc.), the mega cache has it.  Mega-cache HIT →
`exit_send_cached` → tryResume's `case ExitSendCached` (line
14960) — which calls `upgradeICToJ2J` to fill the IC.

**`upgradeICToJ2J` fills with `J2J_ENTRY_BIT` but NOT
`BLOCK_VALUE_BIT`** (it only sets J2J + optional inline primKind
bits, see Interpreter.cpp:15820).  Even when target is
BlockClosure>>value:value:, the BLOCK_VALUE_BIT path is only
reached via `patchJITICAfterSend`'s primitive-207/209 check,
which is on the COLD path (mega-cache miss → ExitSend →
patchJITICAfterSend).

So at sort's scale, the mega-cache rescues every value:value:
dispatch and the IC site never gets BLOCK_VALUE_BIT.
Specialization can't fire because the bit isn't there.

**Fixes SHIPPED (4 commits this session):**

1. `b05e7651` — caller-bump alongside callee-bump in stencil_sendJ2J.
   Both caller and callee accumulate executionCount on each J2J hit
   (with shared splice gate).  Closes one half of the
   "callee-recompiles-fast-skips-callee-bump" gap.

2. `d1ef537a` — save+restore `pendingICPatch_` around primitive in
   sendSelector (both cache-hit and full-lookup paths).  When
   primitive 207/209 calls `activateBlock`, it clears
   `pendingICPatch_` to keep the block's inner sends from
   re-patching the OUTER send's IC.  Save+restore preserves the
   outer state across the primitive call.

3. `8bd0d9ea` — `upgradeICToJ2J` sets BLOCK_VALUE_BIT for primitive
   207/209 (both upgrade and fill paths).  Mega-cache hits arrive
   here without going through patchJITICAfterSend; previously they
   left BLOCK_VALUE_BIT unset.

4. `f24a48d3` — eager-compile in `upgradeICToJ2J` extends to block-
   value primitives (primIdx 207, 209 — was 0..199 only).  Lets the
   IC fill earlier, before the target's natural compile threshold.

All 4 are architecturally correct.  All gated by PHARO_NO_* opt-out
flags.  Bench-suite parity (best-of-10 ±2 ms across 9 benches).

**But sort still doesn't speed up.**  Discovered a SECOND timing
issue when validating the fix.  Even with BLOCK_VALUE_BIT correctly
plumbed through both patch paths, mergeFirst's recompile happens
TOO EARLY:

  1. mergeFirst's compiled code calls value:value: many times
  2. Each call: cold IC → mega-cache hit → ExitSendCached →
     upgradeICToJ2J(target=BlockClosure>>value:value:)
  3. **Target not yet JIT-compiled** (eager-compile in
     upgradeICToJ2J only fires for primIdx 0..199; 207 is excluded).
     upgradeICToJ2J early-returns at line 15694
     `if (!target || !target->isExecutable()) return;`.  IC stays
     empty.
  4. mergeFirst's executionCount accumulates from caller-bump
     (`b05e7651`) and other paths.  At ~251, OSR fires and
     mergeFirst recompiles.  applyICSpecialization runs on the
     OLD IC — site 2 still empty.  No spec.
  5. AFTER mergeFirst's recompile, BlockClosure>>value:value:
     finally hits its own threshold via noteMethodEntry and
     compiles.  Subsequent value:value: calls from mergeFirst
     succeed in upgradeICToJ2J → IC site 2 fills with J2J +
     BLOCK_VALUE_BIT.
  6. **But mergeFirst is now tier=2.**  applyICSpecialization
     won't run again unless something forces a SECOND recompile.

**The remaining gap is "warm-IC late recompile" — multi-day work.**
Possible approaches:
  - Eager-compile block-value primitives (extend upgradeICToJ2J's
    primIdx check to include 207, 209).  Risk: causes timing
    cascades, more methods compiled per startup.
  - Track "specializable bits added since last recompile" per
    method; force a second recompile when a threshold of new
    classifications appears.  Adds per-method state.
  - Defer first recompile until target methods are compiled.
    Hard to detect without a graph dependency tracker.

### 2026-05-03: Option A "specializable-bit accounting" implementation
Implemented the per-method late-spec-count + one-shot re-recompile
infrastructure (Option A from the proposal above):
  - `JITMethodStats.lateSpecCount` (uint8) + `flags` bit
    `kLateSpecRecompiledOnce` — fits in the existing 12-byte
    side-table (replaced reserved1/reserved2).
  - `JITRuntime::noteLateSpecBit(callerJM, newExtra)` — bumps the
    caller's count when an empty IC slot fills with classifying
    bits AND caller is tier=2.  Weight 2 for high-value bits
    (BLOCK_VALUE/multi-slot/returnsLiteral), 1 for plain J2J.
    Threshold = 2.  One-shot cap via flag.
  - Hooked into both IC-fill paths: upgradeICToJ2J (existing-empty
    extra + fill-empty) and patchJITICAfterSend (cold-IC path).
  - Relaxed `maybeRecompileForOSR`'s `tier != 1` gate to also
    accept `tier == 2 && lateSpecCount >= threshold && !
    recompiledOnce`.  Sets the flag after successful re-recompile.
  - Gated by `PHARO_LATE_SPEC_RECOMPILE=1` (opt-in).

**Result on sort 100K bench: NEUTRAL (215ms with flag vs 214ms
without).**  Many other methods get late-spec'd (shuffleBy:, scanFor:,
findElementOrNil:, =, printStringBase:nDigits:, etc.) but mergeFirst
itself doesn't qualify.

**Why mergeFirst doesn't qualify (deeper bottleneck):**
With `PHARO_TRACE_LATE_SPEC_DBG=1`, mergeFirst sees 4 IC-fill events
total — ALL at tier=1, ALL for two distinct callees with primKind=14
(at:) and primKind=15 (at:put:).  After OSR-recompile to tier=2, NO
further IC-fill events occur for mergeFirst.  In particular, the
`value:value:` site that we intended to hit with stencil_sendBlockValue2Arg
NEVER fills — neither at tier=1 nor tier=2.

Two paths could fill mergeFirst's value:value: IC site:
  - `upgradeICToJ2J` from `tryResume`'s ExitSendCached (requires
    the slot to have already been filled with at least the class
    key — needs an earlier patchJITICAfterSend).
  - `patchJITICAfterSend` from sendSelector's primitive-success
    path (line 6709), gated by `pendingICPatch_ != nullptr`.

Tracing with `PHARO_IC_PATCH_DEBUG=1` shows `patchJITICAfterSend` is
called millions of times, but `noPending == call` for every range
sample — i.e., `pendingICPatch_` is null on every entry.  The save+
restore around `executePrimitive` (line 6697-6707) does restore the
pointer when activateBlock cleared it — but that path requires
pendingICPatch_ to have been set FIRST by tryResume's ExitSend
handler.  And tryResume's ExitSend only fires on cold-IC (empty slot)
exits.

Hypothesis: mergeFirst's value:value: stencil_sendJ2J does NOT exit
to runtime — it consults the mega-cache directly (selector hash
lookup outside the IC), finds BlockClosure>>value:value:, and
tail-calls into it without ever updating mergeFirst's IC.  This is
the "mega-cache bypass" we partially fixed in `8bd0d9ea` for
upgradeICToJ2J's BLOCK_VALUE_BIT — but the actual IC-FILL path stays
broken for sites where the mega-cache hits before the JIT exit ever
captures pendingICPatch_.

**Real fix candidates for sort:**
  1. Make stencil_sendJ2J set pendingICPatch_ via a direct write
     before consulting the mega-cache, so subsequent
     patchJITICAfterSend calls on the cache-hit path can patch.
  2. Add a path in the mega-cache hit code (Cache::executeMethod
     siblings) to patch the calling IC slot directly.
  3. Track caller-IC pointer per mega-cache miss separately.

The Option A infra is in place but doesn't unblock sort 100K
because the bottleneck is upstream (the IC fill itself, not the
re-spec).  Leaving Option A opt-in for now.

### 2026-05-03: Mega-hit IC fill attempt — REVERTED

Tried adding an inline IC fill from stencil_sendJ2J's mega-cache hit
path via `_HOLE_RT_FILL_IC` runtime helper.  Goal: when mega-cache
hits with a JIT'd callee, fill the caller's IC slot 0 with the
classification bits (J2J_ENTRY_BIT + BLOCK_VALUE_BIT for 207/209) so
applyICSpecialization (or late-spec re-recompile) can pick them up.

**Result: 2× regression on sieve x100 (44 → 87 ms), sort unchanged.**

Root cause: per-fill `pthread_jit_write_protect_np` flip is too
expensive on Apple Silicon.  Even with a `tier == 2 && icData[0] == 0`
inline gate (so the helper only fires on cold tier=2 IC slots), the
fill cost dominated.  Each flip is a thread-wide MSR toggle —
amortising it across many sites doesn't help when there are many
distinct sites.

Reverted the stencil call.  Helper + extract-stencils wiring stay in
tree as scaffolding (see `jit_rt_fill_ic` header comment for re-
activation notes).  Real fix likely requires moving IC entries out
of MAP_JIT into a separate RW zone so the per-fill flip becomes
free.

### 2026-05-03 PM: IC entries moved to heap — sort 100K 214 → 175 ms

Implemented the architectural fix: ICs now live in a heap-allocated
side-buffer (`JITMethod::icBuffer`) instead of inside the in-zone
allocation.  Heap is always RW so per-IC-write `pthread_jit_write_
protect_np` flips disappear entirely — every IC patch site becomes
just three direct stores.

`898ca79f`: Move IC entries out of MAP_JIT into heap (default-on).

Mechanical changes:
- `JITMethod::icBuffer` (uint64_t*) added; CodeZone::allocate
  calloc()s, freeMethod free()s.  JITMethod grew 8 bytes, JM_SIZE
  bumped 88 → 96 in TrampolineAsm.S + stencils.cpp's mirror.
- JITCompiler emits operand2Ptr pointing at `icBuffer + sendIdx
  * IC_BYTES_PER_SITE` instead of the in-zone offset.  Recompile
  copies old->icBuffer into new->icBuffer.
- All consumers of `codeStart() + codeSize - N*IC_BYTES_PER_SITE`
  (applyICSpecialization, upgradeICToJ2J, patchJITICAfterSend,
  rewriteIcEntriesAfterRecompile, flushCaches, recoverAfterGC,
  Tier2Compiler IC plumbing) switched to `icZoneStart()`.
- `jit::makeWritable`/`makeExecutable` calls around IC writes
  deleted in upgradeICToJ2J + patchJITICAfterSend + flushCaches +
  recoverAfterGC + jit_rt_fill_ic.

Bench-suite (default flags):

```
                     Before      After       Delta
fib(28)              15          15          —
sieve x100           44          45          —
sort 100K            214         175         -18 %
dict 50K             158         149         -6 %
sum 1M               1           1           —
5000 factorial       22          23          —
1M blocks            21          0 (already)
1M getter+yourself   0           0           —
100K allocations     5           5           —
```

Re-attempted the stencil-side mega-hit IC fill on top of heap-IC
(now that the W^X cost is gone).  Still regressed sieve 45 → 129 ms
— the per-call indirect cost into `jit_rt_fill_ic` stays expensive
because it fires on every cold-IC mega-hit.  Reverted the stencil
call again; helper + wiring stay in tree as scaffolding.

Late-spec re-recompile (`7c1c7fd7`, opt-in) still doesn't change
sort 100K appreciably (174 ms with PHARO_LATE_SPEC_RECOMPILE=1).
mergeFirst's value:value: IC still stays cold because the inline
mega-cache hit path keeps bypassing IC writes — different problem
that needs a separate fix.

### 2026-05-03 PM diagnostic: why mergeFirst's value:value: site stays cold

Added PHARO_DUMP_RECOMPILE_IC + PHARO_TRACE_UPGRADE_VV probes to
identify the path.  Findings:

1. **upgradeICToJ2J IS called** with mergeFirst as caller and
   BlockClosure>>value:value: as cached method (many times).
2. **It early-returns at line 15743** ("genuinely unsafe
   primitive"): primitive 207 doesn't have `hasPrimPrologue`
   (no prologue stencil for it — see primitivePrologueStencil),
   it isn't in 256-519 quick-prim range, and it isn't in 257-263
   quick-constant range.  All three checks fail → bail.
3. **mergeFirst's IC dump confirms** site layout at recompile:
   ```
   site=0 sel=at: key0=0x0  extra0=0x0  (cold)
   site=1 sel=at: key0=0x0  extra0=0x0  (cold)
   site=2 sel=value:value: key0=0x0 extra0=0x0  (cold) ← target
   site=3 sel=at:put: key0=0x33 extra0=...  (hot, primKind=15)
   site=4 sel=at: key0=0x33 extra0=...  (hot, primKind=14)
   site=5 sel=at:put: key0=0x33 extra0=...  (hot)
   site=6 sel=at: key0=0x33 extra0=...  (hot)
   site=7 sel=replaceFrom:to:with:startingAt: key0=0x0 (cold)
   site=8 sel=replaceFrom:to:with:startingAt: key0=0x0 (cold)
   ```
   value:value: site stays cold — never observed at recompile.

**Attempted fix (REVERTED, opt-in PHARO_BLOCK_VALUE_FILL):** allow
target to remain non-null for prim 207/209 in upgradeICToJ2J's
eager-compile section, AND bypass the unsafe-prim return for them
so the fill code below runs.  The fill already handles 207/209 by
adding BLOCK_VALUE_BIT.

**Result: hang during Morphic startup before sort even ran.**
Bench process couldn't make progress; DNU-STACK errors on
`OSWorldRenderer>>displayExtentChanged` and similar Morphic
methods.  Filling IC with `J2J_ENTRY_BIT | BLOCK_VALUE_BIT` for
207/209 must be triggering some downstream path that doesn't
work — possibly the inline IC probe matching for value:value:
sites whose receivers aren't FullBlockClosure (e.g.,
ConstantBlockClosure2Arg overrides value:value: with its own
non-primitive method), making the BLOCK_VALUE_BIT branch
dereference a closure layout that doesn't match.

Need more careful investigation before re-attempting.  Reverted.

**Alternate approach (not yet tried):** apply
stencil_sendBlockValue2Arg from applyICSpecialization based on
the SELECTOR (`#value:value:` + nArgs==2) rather than on
BLOCK_VALUE_BIT in IC data.  Statically resolve
FullBlockClosure's classIndex via the classTable at compile time
and bake it into operand2.  Spec stencil's slow path falls
through to ExitSend cleanly for non-FullBlockClosure receivers
(ConstantBlockClosure subclasses).  Risk: still needs validation
that the slow path is correct for all polymorphic receivers seen
in the wider workload.

### 2026-05-03 PM: selector-based block-value spec — SHIPPED (default-on, sort 100K -30%)

After three failed attempts (documented below), the selector-based
spec is now default-on as `a515adcf`.  Sort 100K: 174ms → 121ms.

**The fix** that made it stable: extending the spec stencil's slow
path to consult the mega-cache like sendJ2J does.  Earlier attempts
applied the spec but the slow path bailed to ExitSend with
icDataPtr=nullptr — for cold IC sites where mega-cache hits
commonly bypass IC writes (mergeFirst's value:value: pattern),
EVERY call ate full method lookup + interp dispatch.  3000×
slowdown.

The new slow path probes the mega-cache, computes lookupKey from
receiver class, and bails to EXIT_SEND_CACHED with cachedTarget
= mega-hit method.  Mirrors sendJ2J's mega-cache hit path — slow
path cost now comparable to sendJ2J's slow path.

**applyICSpecialization branch** (compile-time, fires at recompile
when classKey0 == 0):
  - Detects Send2 with #value:value: selector via literal frame
  - Resolves FullBlockClosure classIndex via JITRuntime cache
  - Sets stencil to stencil_sendBlockValue2Arg with packed
    operand2Ptr = (litIndex << 48) | (fbcIdx << 16)

**Cumulative session win on sort 100K:**
  Original baseline:  214 ms
  Heap-IC (898ca79f): 174 ms  (-19%)
  Spec (a515adcf):    121 ms  (additional -30%)
  Total:              -43%
  Cog gap:            2.0× (was 3.6×)

**Files changed:** JITCompiler.cpp (new applyICSpecialization
branch), stencils.cpp (sendBlockValue2Arg slow-path mega-cache
probe), generated stencils regenerated.  PHARO_NO_BLOCK_VALUE_SPEC=1
to opt out.

### Earlier failed attempts (kept for context)

### 2026-05-03 PM: selector-based block-value spec attempt — REVERTED

Implemented the alternate approach:
1. `JITRuntime::resolveFullBlockClosureClassIndex()` — lazy
   resolver using Interpreter's pre-cached value (class table
   resolved by name during VM init).
2. New compile()-time pass: walks `decoded` after
   applyICSpecialization, replaces stencil_sendJ2J with
   stencil_sendBlockValue{0,1,2}Arg when the send selector is
   `value`/`value:`/`value:value:` and FullBlockClosure's
   classIndex is known.  operand2Ptr packed identically to the
   IC-driven path: `(litIndex << 48) | (fbcIdx << 16)`.

**Result:** bench-suite hangs in Morphic startup — DNU cascade
on `OSWorldRenderer>>displayExtentChanged`, `isTransparent`, etc.
Same symptom as the upgradeICToJ2J 207/209 fix attempt.

Even narrowing to JUST argCount==2 (only #value:value: spec, not
#value or #value:) still hangs.  Same with restricting to
literal-send opcodes (0x80-0xAF + ExtSend) only.

**Hypothesis:** sendBlockValue{1,2}Arg's slow-path bail
(`s->cachedTarget = s->literals[litIndex]; s->icDataPtr = nullptr;
... EXIT_SEND`) interacts badly with the interp's resume path for
some specific call.  Possibly Morphic's `value:value:` calls
include receivers that aren't closures at all (e.g., a plain
Object that uses `valueOf:value:` via the special-selectors
table), making `s->literals[litIndex]` not actually a
selector — so the interp re-dispatch goes to a wrong target.

Reverted the spec branch.  Infra retained (`9a336d45`):
`resolveFullBlockClosureClassIndex()` + accessor.  Future work
should:
- Reproduce the hang on a minimal test case (eval'ing a tiny
  Morphic operation that triggers it).
- Inspect the exact bytecode and receiver class at the failing
  send site.
- Fix the slow path correctness OR add receiver-class gating to
  the spec's eligibility (e.g., only apply when the IC has
  observed at least one FullBlockClosure receiver).
`pendingICOwnerMethod_` is set in only TWO places today:
  - `tryResume`'s ExitSend handler (Interpreter.cpp:14884)
  - Sista's `executeMethod` Send2 path (Interpreter.cpp:16992)

`patchJITICAfterSend` is called from many MORE places (lines 4233,
4239, 4263, 4269, 6639, 6664, 6677, 6691, 6705, 6761, 6772) — in
particular the regular `sendSelector` cache-hit + `executePrimitive`
+ Success path at 6691.

Tracing showed:
1. For value/value:/valueNoContextSwitch (1-arg block-value
   primitives), `pendingICOwnerMethod_` IS set correctly when patches
   fire (#withAllSuperclassesDo:, #on:do:, #ensure:, etc.).
2. For value:value: (the sort comparator pattern), the patch path
   doesn't reliably reach the owner-bearing code: `patchJITICAfterSend`
   often early-returns because `pendingICPatch_` is null — i.e., no
   JIT exit captured the IC slot before this dispatch.

Why?  When the bench-suite is in pure-interp mode (e.g., during
warmup before mergeFirst is JIT-compiled), value:value: calls go
through `sendSelector` → cache-hit → executePrimitive → primitive-
FullClosureValue, but with no JIT exit having set `pendingICPatch_`.
The patch is silently dropped.

The fix would need:
- Initialize `pendingICPatch_` from the CALLER's JM IC slot when
  the call originates inside JIT'd code — even for primitive-
  successful sends.
- OR add a separate per-IC-site hit counter that sidesteps the
  patch-via-pendingICPatch_ mechanism entirely.

Both are multi-day plumbing changes touching every IC dispatch
path.  Documented for the future Phase 6 generalization session.



`1M getter+yourself` closes from 16-20 ms → **0 ms** (5/5 runs;
beats Cog's 3 ms).  Three pieces wired together:

1. **SpecialSend helper-sends** — new IR op kSendCallHelperSpecial
   resolves the selector via SpecialSelectorsArray at runtime,
   letting Sista lift continue past prologue sends like `OC new`
   that previously terminated the lift.

2. **No-accum splice shape** — when the body is K leading purely-
   elidable triplets followed by no canonical arith (the
   `timesRepeat: [obj size. obj yourself]` shape), the math splice
   computes `count_final = limit + 1` directly without trying to
   load an uninitialized accumulator.

3. **Class guard via dataflow** — when no IC hint is available
   (Sista compiles bench methods on first activation before T1
   warms ICs), scan the method prologue for
   `pushLitVar X; SpecialSend new; popIntoTemp T` and extract the
   class from the literal's class binding.

PHARO_SISTA_INLINE_YOURSELF=1 opt-in.  Default flags unchanged.

**Original analysis (kept for context):**



**What:** inline block bodies into the caller's compiled code at hot
sites.  Cog does this after PIC stabilizes — the per-iter `aBlock value`
becomes a direct branch into the inlined body, eliminating closure
allocation, captured-temp-vector dispatch, and method-map lookup.

**Why blocked:**  this is the structural fix for the bench-suite block-
dispatch gap.  `1M getter+yourself` 99ms vs Cog ~3ms (33×) and
`1M blocks` 14ms vs ~1ms (14×) are dominated by per-iter block
dispatch.  Sista's existing block handling is "lift block IR into
inlinedBlocks slot, splice intercepts at PushFullBlock+Send".  That
covers the splice family but not standalone `value`/`value:` sends to
captured blocks (which is what `timesRepeat:`-style call patterns
produce after Pharo's bytecode inlining).

**Payoff:**  closes `1M getter+yourself` and `1M blocks` gaps.
Probably also helps `sort 100K` (block comparator in sort) and
`dict 50K` (block in `at:put:` resolution).

**Estimate:**  4-6 weeks.  Needs IR-level support for block
specialization at IC sites (recognize monomorphic block from IC
profile, inline the body, deopt-on-mismatch), and the lowering needs
to handle nested IR with deopt-stack reconstruction across the inlined
boundary.

**2026-05-02 — refined target.**  `1M blocks` now hits 0ms via
the whileTrue: math splice (item #4), so the remaining bench-suite
gap is `1M getter+yourself`: 16-20ms ours vs Cog 3ms.  Profile of
runInstVar shows the bottleneck is the multi-send body inside the
inlined whileTrue: loop:

    pushTemp 1; send size; pop;       <- multi-slot getter shape
    pushTemp 1; send yourself; pop;   <- universal Object>>yourself
    pushTemp 2; pushOne; +; popIntoTemp 2

The whileTrue: splice rejects multi-send bodies; the math shortcut
isn't applicable when the body has side effects.  The structural
fix here isn't full block inlining at `value` sites (Pharo inlines
timesRepeat:/to:do: at compile time, so there is no `value` send
to intercept).  It's an extension to the whileTrue: splice's body
whitelist to admit `pushTemp obj; sendSel; pop` triplets where the
send is IC-monomorphic + selector inlinable (yourself → no-op,
size → multi-slot getter), with class-guard + deopt-on-miss.

Cleared INLINE_YOURSELF for the universal-no-op case (re-tested
2026-05-02: 5000-factorial regression no longer reproduces — was
likely flaky timing in the original A/B).  But INLINE_YOURSELF
alone moves no needle on `1M getter+yourself` (still 20ms) because
runInstVar's bytecode body executes via interp/T1, never reaching
Sista's send-byte (the whileTrue: splice rejects the method).

**Real next step:** new IR op `kCountedLoopBodyExec` — counted
whileTrue: with body containing K side-effect-bounded sends.
Pre-pass admits when each send has IC hint + matches an inline-able
selector pattern.  Lowering emits a real iteration loop with
inlined body code per iter.  Estimated 1-2 weeks for runInstVar
(2 send sites, both inlinable); generalizing to handle arbitrary
IC-monomorphic sends is full Phase 6.

**2026-05-02 — foundation in tree (`c5389a47` + `98b42bb1`).**
Extended the existing whileTrue: math splice's body recognizer to
admit K leading purely-elidable triplets `pushTemp T; sendByte; pop`
followed by the canonical 4-byte arith — plus a no-accum variant
where the body is ONLY triplets (matches `n timesRepeat: [obj size.
obj yourself]` shape).  yourself triplets need no class guard
(universal Object>>yourself); size triplets emit kLoadTemp +
kGuardClass.  The class for the guard comes from either (a) IC hint
at the size bcOffset, or (b) a compile-time dataflow trace through
the method prologue for `pushLitVar X; SpecialSend new; popIntoTemp
T` — extracts the class from literals[X] (the class binding).  Lets
bench-shape one-shot methods splice without IC warmup.

Default-flags bench-suite is unchanged (5/5 runs).  Recognizer is
gated behind `PHARO_SISTA_INLINE_YOURSELF=1` (opt-in).  Op::
kCountedLoopBodyExec added as scaffolding for the real-iteration
variant (currently unused).

**Verified:** for runInstVar, the recognizer + dataflow correctly
identifies the pattern (preLoop=6 triplets=2 bodyTemp=1
guardCls=OrderedCollection).  Pre-pass logs:
`[SISTA-WHILETRUE-CAND] preLoop=6 endPop=28 accumT=2 loopT=2
limitLit=3 arith=0 const=1 method_len=46`.

**Remaining blocker for the actual runInstVar win:**  Sista's lift
terminates at byte 1 (`SpecialSend new` in the prologue) before
reaching preLoopStart=6.  SpecialSend (0x70-0x7F) always emits
kSendUnspeculated and returns kOk — the helper-sends path that lets
the lift continue past Send0/1/2 doesn't apply.  Next step: extend
the SpecialSend lift to emit a `kSendCallHelper`-style continuation
when HELPER_SENDS=1.  The selector resolves at compile time via
SpecialSelectorsArray (slot `(ssIdx + 16) * 2` is the selector Oop);
that raw Oop can be baked into the IR or routed through a new
kSendCallHelperSpecial variant.

Estimated 1-2 days for the SpecialSend extension.  Once that lands,
the runInstVar splice should fire and 1M getter+yourself should
collapse to math-simplification time (sub-millisecond).

**Related:** `memory/project_cog_gap_2026_04_29.md` ("Block dispatch
dominates — Phase 6 is the big lever"),
`memory/project_phase6_investigation_2026_05_01.md`,
`memory/project_sista_skips_blocks.md`.

---

## 3. Sista deopt-with-resume — **FIRST FORM SHIPPED 2026-05-02**

Shipped as the do-accum completion helper (the cleanest of the three
forms outlined below).  The helper finishes a `kCountedLoopArrayDoAccum`
iteration in C++ when the per-iter SmI tag-check misses, instead of
rebuilding the interp stack and re-running do: from scratch.

**Implementation:**

- `Interpreter::jitSistaCompleteArrayDoAccum` (Interpreter.cpp): SmI
  fast-path with `__builtin_*_overflow` for SmI/SmI iters; non-SmI or
  overflowing iters dispatch through `jitSistaCallSend(`+`/`-`/`*`)`
  for full image-side coercion semantics.  rcv + vec are kept on
  `state->sp` as GC-rooted scratch slots so sends inside the loop
  can move the heap; loop re-fetches them per iter.  Final commit
  uses `storePointerUnchecked` so a young-gen LargeInteger result
  going into an old-gen TempVec gets the rememberSet barrier.

- `jit_rt_sista_complete_array_do_accum` (JITRuntime.cpp): thin
  `extern "C"` wrapper.

- Lowering (`SistaLowering.cpp` `kCountedLoopArrayDoAccum`): per-iter
  tag-check miss invokes the helper; on non-zero return the helper
  has already committed `vec[slot]` so we branch past the loopExit's
  `cc.str`; on zero return we fall through to the original
  conservative deopt (rebuild `[rcv, vec]`, resume at PushFullBlock).

**Gating:** default-on; opt-out via `PHARO_NO_SISTA_DOACCUM_RESUME=1`.
Bench-suite + bench panel parity within ±1 ms across all benches
under both default and opt-out.

**Synthetic-bench measurements (2026-05-02):**

```
all-Float 100K array, 5 reps:
  default off:        56 ms
  DOACCUM_RESUME=1:    8 ms      (7×)

1M array, Float at index 999999, 5 reps:
  default off:       481 ms
  DOACCUM_RESUME=1:    3 ms      (160×)

all-SmI 1M array, 5 reps:
  default off:         4 ms
  DOACCUM_RESUME=1:    4 ms      (no change — flag is zero-cost on
                                  the SmI fast-path)
```

Bench-suite default-flag run (3-run best-of) is at parity with
DOACCUM_RESUME=1, both within ±1ms of the pre-flag baseline:

```
                   default   RESUME=1
  fib(28) ms       15        15
  sieve x100 ms    45        45
  sort 100K ms     216       218
  dict 50K ms      155       155
  sum 1M ms          0         0
  factorial ms     22        23
  1M blocks ms       0         0
  1M getter ms     19        20
  100K alloc ms      4         5
```

**What this unblocks (real-world):** any image code that does
`collection do: [:e | s := s + e]` on a mixed-type collection — Pharo
IDE callbacks, Roassal coordinate accumulators, FFI struct-field
sums.  These were silently slow today because the splice deopts on
the first non-SmI element and re-runs from scratch.

**What this does NOT unblock (yet):**

- ~~inject:into:~~ — **SHIPPED 2026-05-02 (`57e58631`)** for canonical
  `[:acc :e | acc OP e]` shape.  Same architecture as do-accum but
  the canonical block has 6 IR values (INLINE_ARITH adds tag-check
  side-effects) and the arith op consumes the original kLoadTemp
  values directly, not the tag-check results.  Mixed-type 1M Float
  ×5: 738→3ms (245×).  All-SmI 1M ×5 wins 5→2ms from the specialized
  path bypassing generic block-IR emission.
- ~~collect:~~ — **SHIPPED 2026-05-02 (`3614f42c`)** for canonical
  `[:e | e OP <SmI const>]` shape.  5-IR-value canonical (const is
  type=kOopSmallInt so only e gets a tag check).  Twist vs do-accum:
  result Array is partially populated by compiled code before the
  helper fires; helper continues populating from startIdx.  Per-iter
  writes go through storePointerUnchecked for GC barrier on heap
  results.  Mixed-type 1M ×3: 458→1ms (458×).
- IV-inject variant.  Tried 2026-05-02 and reverted: the specialized
  canonical loop (algorithmically the same as inject:into:'s but
  iterating SmI start..stop directly) ran ~50% slower on `sumIv 10x`
  bench-panel target (4 ms → 6 ms).  Even after matching the generic
  path's b.eq fall-through pattern, asmjit's Compiler emits tighter
  code for the generic kPrim*Int + kReturn flow than for the manually-
  written specialized loop.  The canonical path's "skip dead i tag
  check" win is dominated by whatever the generic emission does
  better — likely register coalescing through the kReturn mov.
  Net: the regression on the all-SmI hot path (which dominates real
  workloads) hurts more than the helper helps for non-SmI accumulator
  cases.  Reverted; full attempt visible in the prior commit (which
  introduced helper + lowering, then was undone).  Could revisit if
  asmjit's RA model gets better visibility, or if a workload surfaces
  with measurable mixed-type-acc hits and the all-SmI cost is
  acceptable.
- Compile-time splice rejections (multi-block IR, non-simple block op,
  numCopied > 1).  These are pre-pass rejections that never enter
  the loop, so the runtime helper doesn't help.  Separate work.
- True mid-iter resume.  This first form runs the rest of the loop
  in C++ instead of resuming the interp at the next bytecode.  Real
  resume (per-iter framepoint + interp-side iteration counter) is
  still a multi-week item; the completion-helper form covers the
  common cases and is much simpler.

**Future variants (when needed):**

- `jit_rt_sista_complete_array_inject_into` — inject:into: with the
  same shape but a 2-arg block.  Block must still match the
  recognized canonical IR.
- `jit_rt_sista_complete_array_collect` — collect: that completes a
  partially-filled result Array.
- `jit_rt_sista_complete_interval_*` — Interval variants iterating
  start..stop.

**Related:** `memory/project_b1_helpersends_2026_05_01.md` (originally
mentioned as alternative to HELPER_SENDS), `docs/deferred.md` §B8.

---

## 4. Sista pre-pass for inlined whileTrue: counter loops — **DONE 2026-05-01**

Shipped via commits `3b9d5f08`, `3cb1ced0`, `51f57d32`, `5448b564`.

`Op::kCountedLoopWhileTrueAccum` IR op, byte-pattern pre-pass that
recognizes `<pushLitConst LIMIT, pushOne, popIntoTemp count,
pushTemp count, pushLitConst LIMIT, send <=, jumpFalse, BODY,
pushTemp count, pushOne, send +, popIntoTemp count, jumpTo, pop>`,
lifter intercept that emits the IR op and skips the entire loop
bytecode range, lowering does math simplification:
`accum += (LIMIT - countInit + 1) * const` with overflow check.

Body must be the canonical 4-byte arith shape (`pushTemp X, pushOne,
ArithBase Y, popIntoTemp X`) on a temp distinct from the loop counter.
Multi-send bodies (1M getter+yourself's `[obj size. obj yourself]`)
are detected but rejected — those need a different lowering or a
combination with item #2 (Phase 6 block inlining).

Result on bench-suite: 1M blocks 16ms → 0ms (math simplification turns
1M iterations into 1 multiply + 1 add).  Requires PHARO_SISTA_WHILETRUE=1
opt-in plus HELPER_SENDS=1 (because runBlock's `Time
millisecondClockValue` setup-send terminates the lift without it) and
the class-based HELPER_SENDS gate from item #6 (otherwise UI cascade
DNU breaks the bench-suite before runBlock runs).

---

## 5. J2J-only callee recompile triggering — **DONE 2026-05-02** (`9572b019`)

Past three attempts hit the splice race; the working design is bump-only
in the inline path + dual splice gate + safe-point recompile drain.
`benchFib` now recompiles (was stuck tier=1 forever).  fib(28) -6%,
sieve -4-6%, others within variance.  `memory/project_j2j_inline_bump_drain_2026_05_02.md`.



**What:** methods that are only called via the J2J fast path
(`stencil_sendJ2J`'s direct-call site) bypass `tryExecute` and
`tryJITActivation`, so their `executionCount` never bumps and OSR-
recompile never fires.  This means tier=1 methods stay unspecialized
forever.  IC sites in those methods (e.g., `obj size`, `obj yourself`)
never get specialized to inline-getter / inline-returnsSelf even when
they're consistently monomorphic.

**Why blocked:**  3 attempts 2026-04-30 and 2026-05-01 reverted because
caller-bump in J2J broke the splice race (sumArr 7ms → 1037ms in panel
run 2/4).  See `memory/feedback_caller_bump_breaks_splice.md`.  The
issue: bumping the caller's count can trigger recompile of a method
that's currently splice-active, and the recompile races with the
running splice.

**Payoff:**  every block body in the system gets IC specialization.
Block-heavy benches (1M getter+yourself, sort, dict) all benefit.
Probably 30-50% improvement on block-heavy real workloads.

**Estimate:**  2-3 weeks.  Requires either:

- a callee-bump variant that mirrors `noteMethodEntry`'s `hasSplice`
  gate without the splice race; OR
- IC-patch-at-callsite (when a J2J site sees consistent class, patch
  the IC to use specialized stencil even without recompile of the
  callee).

The IC-patch approach is cleaner but needs new IC bits.  Callee-bump
is simpler but the gate has been tricky to get right.

**Related:** `memory/project_jit_recompile_gap.md`,
`memory/project_j2j_bump_2026_05_01.md`,
`memory/feedback_caller_bump_breaks_splice.md`.

---

## 6. Class-based HELPER_SENDS gate — **DONE 2026-05-01**

Shipped via commit `5448b564`.

Builder::build derives the method's defining class name from the last
literal (Pharo CompiledMethod convention: last lit is the class
binding, slot 1 = the class itself; `memory.nameOfClass()` resolves
the name string).  LinearLifter stores it on `methodClassName_` and
the HELPER_SENDS emission gate at kSendCallHelper checks
`sistaClassIsHelperSafe(className)` which rejects classes whose name
starts with: World, Form, Morph, Sp, Snapshot, Session, Process,
Semaphore, Delay, Exception, Error, FileReference, FileSystem,
DiskFile, File, Source, Pharo (with PharoBenchmarkRunner allowed-list
override).

Empirical ordering: started with Collection/Array/etc. all skipped
(too restrictive — sum 1M errored "do: receiver is nil"), narrowed to
just UI/system (above list) and bench-suite returned to 8/10 stability
with both 1M blocks and sum 1M dropping to 0ms.

Future widening (if needed): selector-class allow-list rather than
class-prefix skip; or a runtime feedback loop that disables
HELPER_SENDS for classes that produced DNUs in the past N runs.

---

## 7. T1/T2 architectural interaction

**What:** when T2 (the optimizing tier) intercepts a method that T1
already compiled, T1's inline-IC warmup is broken.  Coexist mode
(default since §1.3c) sidesteps this by not replacing T1, but T2
becomes effectively dormant — it compiles but doesn't intercept.

**Why blocked:**  T2 has never demonstrably won on any measured bench.
The IC-warmup issue is the immediate blocker: replacing T1 with T2
mid-execution loses the populated IC state, which is where 80%+ of
T1's performance comes from.

**Payoff:**  potentially significant on workloads where T2's
optimizations beat T1's inline ICs (e.g., loops with heavy arith on
small SmI, or long-running monomorphic chains).  But no such workload
has been measured yet.

**Estimate:**  3-4 weeks of design + impl, plus the open question of
whether the wins exist.  Either:

- shared IC table across tiers (T1 and T2 both read/write the same
  IC entries); OR
- patch-T1-when-T2-compiles (T2's specialization gets folded into the
  T1 IC).

**Related:** `docs/deferred.md` §E.1.

---

## 8. Per-bytecode Sista hook

**What:** today Sista compiles whole methods.  Per-bytecode Sista would
let Sista take over a method mid-execution at any backward jump — a
form of OSR but at the IR level.  Combined with item #3 (deopt-with-
resume), this would let Sista specialize hot loops in methods it
hasn't fully analyzed.

**Why blocked:**  Sista's current cache is per-method.  Per-bytecode
needs a per-bcOffset cache, and the entry shape changes (state at
bytecode N vs at method entry).  Also requires the lowering to support
entering at arbitrary IR points, which is a major restructure of the
prologue.

**Payoff:**  unblocks the bench-suite one-shot problem — methods that
are called once but iterate millions of times (runSum, runFibonacci's
benchFib, etc.) could be Sista-specialized at the loop entry rather
than waiting for full-method compilation thresholds.

**Estimate:**  6-8 weeks.  The lowering rework alone is significant.

**Related:** `memory/project_specialization_misses_doit.md`,
`memory/project_eval_fib_gap.md`.

---

## How to choose

If picking ONE for a focused multi-week session:

- **Biggest payoff:** #2 (Phase 6 block inlining).  Closes the largest
  remaining bench-suite gap (after #9: 1M getter+yourself 16-19ms vs
  Cog 3ms).  Also the most work.

- **Speculative:** #7 (T1/T2 interaction).  Don't start unless a
  measured workload shows T2 beating T1 — currently none does.

Items #4, #5, #6, #9 shipped 2026-05-01..02 (commits `5448b564`,
`d5332e48`, `9572b019`).  Items #1, #3, #8 are alternative paths to
similar outcomes.  #1 unblocks default-on of an existing opt-in.
#3 unblocks more splices.  #8 is the cleanest long-term direction
but the highest cost.

**Bench-suite snapshot (2026-05-02 end-of-day, post-default-on cascade, best-of-10):**

```
                       Ours    Cog    Ratio   Notes
tinyBytecodes/s    18.7M       —       —    (no Cog comparison)
fib(28) ms             14      6      2.3×
sieve x100 ms          44      ?      ?
sort 100K ms          216     60      3.6×
dict 50K ms           158     50      3.2×
sum 1M ms               0      5      0×     (we win — splice + relinquish-fix)
factorial 5K ms        22     27      0.81×  (we win)
1M blocks ms            0      1      0×     (we win — whileTrue: math splice)
1M getter ms            0      3      0×     (we win — Phase 6 default-on)
100K alloc ms           4      ?      ?
```

**Four benches beat Cog by default** (sum 1M, factorial, 1M blocks,
1M getter+yourself).  Remaining gaps (sort, dict, fib) are all
block-dispatch dominated — full Phase 6 generalization — multi-week.

**End-of-day default-on cascade (8 commits, 2026-05-02 PM):**

- `49c4d91d` — kCountedLoopArrayDoAccum deopt-with-resume helper (item #3)
- `396c4448` — DOACCUM_RESUME default-on
- `035140c5` — INLINE_YOURSELF default-on (1M getter 20→0 ms)
- `a3db01b9` — COLLECT default-on (100K collect ×5 = 53→0 ms)
- `d9099ac8` — IV_DO_ACCUM default-on
- `d78c1be5` — INLINE_ARITHIVAR default-on
- `b327ded7` — materializeFrameStack cycle-guard gate aligned to
                HELPER_SENDS default-on (silent safety bug)

Synthetic mixed-type measurements (deopt-with-resume default-on):
- 1M Array, Float at index 999999, ×5 reps: 481 ms → 3 ms  (160×)
- 100K Array all-Float, ×5 reps:             56 ms → 8 ms  (7×)
- 100K collect: ×5 reps:                     53 ms → 0 ms  (∞×)

These are real-world wins for image code with mixed-type collections;
no bench-suite target exercises them.

---

## 9. Multi-slot getter IC pattern — **DONE 2026-05-01**

`d5332e48`: Recognize `^ ivarA op1 ivarB op2 const` (op ∈ {+,-},
const ∈ {-1,0,1}) and dispatch inline via IC bit 57.  Two methods
in the image match (OrderedCollection>>size and SocketStream>>inBufferSize),
so the dispatch table impact is contained.

**Bench:** `1M getter+yourself` (= `obj size. obj yourself` × 1M):
98ms baseline → 20ms (5× speedup) when bench-suite runs cleanly.
Other benches stable.  Opt-in via `PHARO_MULTISLOT_GETTER=1`.

**Encoding:** new bit 57 in IC `extra` word.  Decoder in IC_HIT
macro and stencil_sendJ2J's IC-hit handler unboxes both slots, does
scalar math with `__builtin_*_overflow` checks, re-tags, pushes.
Bails to slow send on non-SmI / overflow.

**Future extension candidates:** could add 3-slot patterns like
`^ ivarA + ivarB + ivarC` (no const) using bit 56, but only one
image method matches that shape — not worth the encoding work.

### 2026-05-03 PM: dict 50K gap analysis (3× behind Cog)

dict 50K put+get: us 148-154ms vs Cog 50ms (3.0× gap).

Hot path: Dictionary>>scanFor: probe loop calls `key = arg` per
probe.  Bench bench does ~3 probes × 100K ops = 300K probes ×
4-method `=` chain = 1.2M extra dispatches.

**Why the chain is slow:**
```
String>>=         (no primitive — bytecode method)
  → compare:with: (no primitive)
    → compare:with:collated: (no primitive)
      → ByteString>>compareWith:collated: (prim 158, fast)
```

Each method adds a stencil_sendJ2J dispatch (IC probe + tail
call).  Even with InlineMonoJ2J spec applied to each (default-on
since `1eed8b9c` post-heap-IC stability), 4 dispatches per `=`
adds up.  At our ~200M sends/s vs Cog ~700M, the ~3.5×
send-rate gap is where the 3× dict gap comes from.

**Investigated but didn't ship:**

- Recompile threshold sweep (PHARO_RECOMPILE_AT 100/250/500/1000)
  — all within noise, threshold isn't the lever.
- Inline at:/at:put: in stencil_sendJ2J's primKind dispatch —
  REVERTED, regressed sieve 44→47ms (icache pressure).

**Possible future approaches (all multi-day):**
1. Sista chain-inliner for `String>>=`: lift the 4-method chain
   into a single inlined sequence at recompile time.  Requires
   recognizer for the chain pattern + substitution machinery.
2. New stencil_sendByteStringEq: applyICSpecialization detects
   Send1 with #= selector + ByteString classKey, replaces with
   stencil that does inline byte-compare via prim 158 logic.
   Skips the 4-method dispatch chain entirely.
3. PrimKind for primitive 158 (compareWith:collated:): inline
   the byte-compare in stencil_sendJ2J.  Only saves the
   innermost dispatch; still 3 outer dispatches remain.

Path #2 is likely the best return — directly attacks the chain.
But the spec stencil's slow path needs the same mega-cache
fallback we added for sendBlockValueNArg or it'll hang at scale.

Not pursued in 2026-05-03 session due to time bound; documented
for future work.

### 2026-05-03 PM: dict 50K -13% — inline ByteString = in stencil_equalSmallInt

`fc301953` (default-on): dict 50K 148-154 → 128-133 ms.  Cog gap
3.0× → 2.6×.

**Root cause located by IC dump:** scanFor:'s `=` send is opcode
0x66 (arith special send), NOT a literal Send1 with #= selector.
0x66 goes directly to stencil_equalSmallInt — completely bypassing
the IC dispatch.  My initial selector-based applyICSpecialization
attempt found nothing because there was no #= IC site to
specialize.

**Diagnostic that found it:** added `PHARO_DUMP_RECOMPILE_IC=scanFor`
flag in JITRuntime.cpp.  Dump showed 6 IC sites — `size`, `hash`,
`at:`, `key`, `at:`, `key`.  No `=`.  That ruled out applyICSpec
as the lever.

**The fix:** extend stencil_equalSmallInt's slow path to detect
byte objects (format 16-23, no overflow word) and inline the byte
compare.  Skips Pharo's String>>= → compare:with: → compare:with:
collated: → ByteString>>compareWith:collated: (prim 158)
4-method chain.

```cpp
// stencil_equalSmallInt:
if (isSmallInteger(a) && isSmallInteger(b)) { ... fast SmI path ... }
// NEW: ByteString = inline byte compare
if (both byte objects, fmt 16-23, slotCount<255) {
    if (sizes differ) result = false;
    else { byte loop; result = eq; }
    return;
}
// existing slow path
```

Bench-suite 5/5 stable.  All other benches at parity.

**Cumulative session summary on bench gaps:**
```
                       Original   Final     Δ        Cog    Gap
fib(28)                15         15        —        15     1.0×
sieve x100             44         44        —        18     2.4×
sort 100K              214        121-122   -43%     60     2.0×
dict 50K               158        128-133   -18%     50     2.6×
sum 1M                 1          1         —        5      0.2× (we beat)
factorial              22         21-22     —        ?
1M blocks              21         0         —        16     0    (we beat)
1M getter+yourself     0          0         —        2      0    (we beat)
100K alloc             5          4-5       —        ?
```

### 2026-05-03 PM: Array-prim spec for sieve at:/at:put: — REVERTED

Attempted to close the sieve x100 gap (44 ms vs Cog ~18 ms, 2.4×) by
adding `stencil_sendInlineArrayAt` and `stencil_sendInlineArrayAtPut`
specialized stencils.  The plan: at recompile time, when `classKey0`
maps to a fmt=2 (variable pointer) class and primKind 14/15 is set,
swap `stencil_sendJ2J` for the spec — skipping the IC probe + indirect
`jit_rt_array_prim` helper-call sequence on every iter of sieve's
inner loop.

**Result:** dict 50K crashed with `Message not understood:
SmallInteger doesNotUnderstand: #key`.  The error originates in
`HashedCollection>>grow:` where `oldElements do: [:each | each
ifNotNil: [self noCheckAdd: each]]` calls `each key` on what should
be an Association — implying my spec returned a SmI somewhere where
an Association/nil was expected.

**Diagnostic steps:**
1. Inline fast path with full class/index/bounds checks: crash.
2. Stencil that JUST calls `_HOLE_RT_ARRAY_PRIM(s, info)` (1:1
   semantics with sendJ2J's primKind path, only difference is no IC
   probe upfront): ALSO crashes.
3. ARM64 disassembly of fast-path stencil shows correct stack reads
   (`ldr x10, [rcv, idx, lsl #3]` = rcv + i*8 = slot[i] for 1-indexed
   i), correct class check, correct overflow-encoding handling.
4. Restricted to at: only (no at:put: spec): still crashes.
5. Restricted via classFormatOfIndex helper to fmt=2 only: still
   crashes.

**Hypothesis (not confirmed):** the spec stencil's bail to ExitSend
differs from `stencil_sendJ2J`'s fall-through to J2J / SEND_CACHED in
some subtle way.  Stencil_sendJ2J's primKind 14 path falls through to
the J2J entry / mega-cache lookup on helper failure, then if all
fails to ExitSendCached.  My spec bails directly to ExitSend with
`cachedTarget = selector`.  Even though both should result in the
interp dispatching the send via `lookupMethod`, something downstream
in the bench's hot path produced a SmI where an Association should
be.

**Status:** all changes reverted.  New stencils + applyICSpec branch
+ `JITCompiler::classFormatOfIndex` helper are NOT in tree.  The
opportunity remains — sieve hot loop is `(tmp2 at: tmp8) ifTrue:` +
`tmp2 at: tmp4 put: false` — and the saving (skipping the IC probe +
helper indirect call) is real.  Future attempt should:

1. Reproduce in isolation (a minimal at:/at:put: bench, not full
   bench-suite) to catch the divergence with less interference.
2. Compare side-by-side with the working `stencil_sendInlineMonoJ2J`,
   which has the same bail pattern (`cachedTarget = selector;
   icDataPtr = nullptr; ExitSend`) and works.
3. Consider adding a "mega-cache fallback" to the bail path, mirroring
   the block-value spec stencils' fix that resolved a similar issue
   (cold IC sites + bail-to-full-lookup hangs).

### 2026-05-03 PM: bench-suite environment fix

The persistent `/tmp/bench_suite/PharoBenchSuite.image` accumulated
session state in `/tmp/bench_suite/pharo-local/ombu-sessions/` that
made the bench hang at sort 100K consistently (sieve also reported
280-290ms vs the 44ms baseline).  Fix: use a fresh image directory
per session.

```bash
rm -rf /tmp/bench_clean && mkdir /tmp/bench_clean
cp /tmp/bench_fresh/Pharo.image /tmp/bench_clean/PharoBenchSuite.image
cp /tmp/bench_fresh/Pharo.changes /tmp/bench_clean/PharoBenchSuite.changes
cp /tmp/bench_fresh/*.sources /tmp/bench_clean/
PHARO_JIT_DEFER=15 timeout 120 ./build/test_load_image \
    /tmp/bench_clean/PharoBenchSuite.image
```

5/5 stable at sieve 43-46ms, sort 122-124ms, dict 128-131ms post-fix.
The doc snapshot above remains valid; it was captured against a clean
image setup.

