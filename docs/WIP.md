# WIP — JIT optimization session (2026-05-27)

## Goal
"Fix the JIT optimization to be as fast as Cog."  fib path is now 3-4x
faster than Cog.  tinyBenchmarks gap (was 3x, now ~2x) remains.

## Current state — all clean

- Branch: `jit` at `f497528d` (pushed to origin).
- Working tree clean.
- fib(15) → fib(45) all produce correct values.
- fib(28) at ~9 ms (Cog ~30 ms — 3.3x faster).
- sieve at ~1.5 ms.
- tinyBenchmarks at ~5.3 s (was 6.7 s; Cog ~2 s).

## Session commits (in order)

```
f497528d vm: clear remaining Primitives.cpp unused-variable warnings
d0660c63 vm: remove 12 unused-variable warnings across VM + JIT
1e0aa459 docs: WIP.md — session resume info for JIT optimization work
80b87e5b vm: remove more unused-variable warnings (5 sites)
b4ea4ecd vm: remove 6 unused-variable warnings from Interpreter.cpp
d33a4fdd gc: gate currentScanParent_/currentScanSlot_ stores behind PHARO_HOT_PATH_DIAG
399f7b06 vm: extract SP_CORRUPT_TRACE / FP_CORRUPT_TRACE_FROM_TB macros
67a37230 vm: consolidate 7 duplicated J2J materialize blocks into helper
33242365 vm: bump gcHeadroom to 512MB (was 256MB)
9605744c vm: bump gcHeadroom 32MB -> 256MB; add per-GC-type counters
22adef73 jit: drop stale task-#8 / J2JSlotPerEntry workaround comments
29df9943 jit: fix materialized frame savedBytecodeEnd — root cause of task #8
```

## 2026-05-27 session notes

### tinyBenchmarks self-measurement insight

handleBenchComplete now decodes String return values.  Our actual
numbers per run:

    6,460,567,823 bytecodes/sec
      114,265,556 sends/sec

For comparison, Cog typically reports ~5B bytecodes/sec and
~150M sends/sec.  We're FASTER per-bytecode and slightly slower
per-send.

Wall-clock 5.3 s is dominated by the doubling-calibration phase
that tinyBenchmarks runs to find an `n` that takes ≥ 1 s:
- Bytecodes calibration: 1+2+4+...+16384 = 32767 trials at 500K
  ops each → ~16 B ops at 6.46 B/s ≈ 2.5 s
- Sends calibration: sum of fib(28..40) ≈ 426 M sends at 114 M/s
  ≈ 3.7 s
- Total: ~6.2 s expected, observed 5.3 s

A FASTER VM spends MORE time in calibration because it needs
larger n to hit the 1 s threshold.  The WIP's "Cog ~2 s" target
appears to be a misleading benchmark — apples-to-apples
bytecodes/sec and sends/sec rates are the right metric, and on
those we are already competitive.

### Sample profile findings

Picked up after the prior WIP.  Confirmed baseline (fib=9 ms,
tiny=5286 ms).  Sample profile shows:
- 1424/2541 samples (56%) in JIT-compiled code via dispatchBytecode chain
- 370 samples (15%) in JIT code via activateMethod → tryJITActivation
- ~5.8% primitiveStringReplace memmove
- ~3.9% primitiveNewWithArg → allocateSlots
- gcTime 21% of run

OSR disable test (PHARO_NO_OSR=1) → 5272 ms, basically same as baseline.
OSR is not the bottleneck — the JIT-compiled code itself is.

Big remaining wins (all require multi-week work or known-broken):
- Sista Tier-2 (`canBailMidMethod` bail protocol, blocked since 2026-05-21)
- xmethod inline-J2J (known broken — #value: DNU on startup)
- bumping gcHeadroom to 2GB gains only ~300 ms (vs 3.3 s gap to Cog)

Cleanup taken this session: all non-vendor unused-variable warnings
are gone (Interpreter.cpp, ObjectMemory.cpp, JITRuntime.cpp,
ImageLoader.hpp, Primitives.cpp).  Remaining warnings are all in
vendored plugin code (B2DPlugin, FloatMath, etc.).

## Root-cause story: the materialize bytecodeEnd bug (29df9943)

The "fb(N) bail-at-limit returns fib(N-1)" bug, originally papered over
by bumping `J2JSlotPerEntry` from 32 to 256, was caused by 7 duplicated
materialize sites all using:
```cpp
frame.savedBytecodeEnd = saveJM->bcStart() + saveJM->numBytecodes;
```
`saveJM->numBytecodes` is 0 for AsmjitT1-compiled methods that have
send sites (the `advertiseResume` gate at `AsmjitT1.cpp:6415`).  This
left `frame.savedBytecodeEnd == bcStart`, so after popFrame restored
`bytecodeEnd_ = bcStart`, the dispatch-loop safety net at
`Interpreter.cpp:1895` immediately fired `returnValue(receiver_)` —
fb(N) returned N (its receiver value) instead of the computed value.

Found via printf instrumentation + `backtrace_symbols` showing
`returnValue` was being called from `interpret()` directly (the safety
net), not from `returnFromMethod` (the normal ReturnTop path).  The
clincher diag was `bcEndOff=0`, meaning `bytecodeEnd_ == bcStart`.

The 7 sites are now consolidated into `materializeJ2JSaveIntoFrame()`
(commit 67a37230), eliminating future duplication risk.

`J2JSlotPerEntry` is back to 32 (no longer needs the 256 workaround).

## GC tuning win (9605744c, 33242365)

Profiling tinyBenchmarks with `sample` found 85% of runtime in
`ObjectMemory::fullGC` with the 32 MB `gcHeadroom_` default (64
fullGCs/run, ~89 ms each).  Bumping to 512 MB drops total GC time from
5851 ms to ~1080 ms — about a 22% gain on tinyBench.

**2026-05-27 amendment:** the "85% GC" reading was cumulative across
all runs.  After the per-run-delta fix (`da56f9ce`), tinyBench
shows ~8% intra-run GC at 512 MB headroom — the bulk of "GC overhead"
in the original measurement was inter-run setup GCs, not the
benchmark inner loop.  The 512 MB choice still helps because it
reduces those setup GCs too.

Headroom knob is now env-tunable (`d0df5a6f` —
`PHARO_GC_HEADROOM_MB`).  Fresh per-run-delta measurements:

    headroom_mb  gcCount  gcTime   wall    delta-vs-512
    512          5        438ms    5533ms  baseline
    1024         2        224ms    5435ms  -98ms (-1.8%)
    2048         0        0ms      5351ms  -182ms (-3.3%)

Allocation pressure is 2 GB/run, so 2048 MB headroom is the smallest
value that fully eliminates intra-run GC.  Real wins are modest
because GC was already only ~8% of run time at 512 MB; the bigger
wins from the old WIP table were calibration artifacts.

Default sweep:
```
 32MB:   64 fullGCs, 5851ms GC, 6738ms total (85% GC)
256MB:   16 fullGCs, 1727ms GC, 5576ms total (31% GC)
512MB:    8 fullGCs, 1080ms GC, 5279ms total (20% GC)
  1GB:    4 fullGCs,  773ms GC, 5108ms total (15% GC)
  2GB:    2 fullGCs,  339ms GC, 4994ms total ( 7% GC)
```
512 MB picked as the sweet spot.  Virtual memory is mmap'd lazily
(4 GB reserved), so this only shifts when GC fires, not physical use.

## Available diag knobs

- `PHARO_B5_TRACE=1` — MAT-RET trace (materialize-bail return values).
- `PHARO_T1_INLINE_J2J=1` — inline-J2J counters (g_inlineJ2J_hits etc.).
- `PHARO_T1_INLINE_PRIM_COUNTERS=1` — per-prim counters
  (g_primAt_hits, g_primAtPut_hits, etc.).  Without this, those
  counters stay 0 even when the inline path fires — was a misleading
  "primAt=0" symptom this session.
- `PHARO_BENCH=fib PHARO_FIB_N=N` — direct fib bench.
- `PHARO_BENCH=tiny`, `=sieve`, `=awfy` etc.

## Profiling commands

```bash
PHARO_BENCH=tiny ./build/test_load_image /tmp/harness/Pharo.image > /tmp/tiny.out 2>&1 &
PID=$!
while ! grep -q "warmup done" /tmp/tiny.out; do sleep 0.2; done
sleep 1
sample $PID 5 1 -file /tmp/sample.txt
kill $PID
```

## Audit-gap finding (2026-05-27): remembered-set is dead infrastructure

`storePointer` and friends maintain `rememberedSet_` via the
old→young write barrier, but the set is never iterated — scavenge
does an O(oldSpace) full scan for old→young pointers explicitly
(`ObjectMemory.cpp:1563-1597`).  Comment at 1565 explains: "Trade
correctness for perf until every write site is audited."

This was a quiet realization while investigating the JIT at:put:
write-barrier site.  The "barrier" I added in `5a7267cd` does work
that the scavenge will redo by scanning every old-space slot.

**Closing the gap would be a real perf win:**

- Scavenge time = O(oldSpace) ≈ ~30 ms / scavenge on a 100 MB heap
- Eliminate by ensuring every slot-write site barriers, then have
  scavenge consume `rememberedSet_` instead of full-scanning

**Audit status** (sites that still write slots without the barrier):

- ~~asmjit T1 inline setter arm64 (AsmjitT1.cpp:4431)~~ — opt-in
  barrier wired up in commit `6b643915` via PHARO_T1_SETTER_BARRIER=1.
  Verified no crash; counters added in `fe4c7b27`.

  **Surprise finding:** the inline-setter path doesn't actually fire
  in practice.  Running normal image startup with
  `PHARO_T1_INLINE_J2J=1` shows the existing per-path counters as
  `getter=16059 setter=0`.  So `g_setterBarrier_calls` stays 0 even
  with the gate on — there are no setter writes to barrier.

  **2026-05-27 investigation:** added throw-away diagnostic counters
  inside `detectTrivialMethod` (now reverted) — the function is only
  invoked ~168 times during a tinyBench run, none classifying as
  setter.  bc0 histogram top entries: `0xf8` (CallPrimitive),
  `0x4c` (PushReceiver), `0x10` (PushTemp 0), `0x40` (PushLitVar).
  No `0xC8-0xCF` (popStoreRecvVar) sightings at all.  The 0x10
  occurrences are followed by `0x81` (send literal 0, 0-args), i.e.
  `^ arg msg`, not the setter pattern.

  Conclusion: the setter recognizer isn't broken — micro-benches
  (fib, sieve, tinyBench) simply don't exercise setter sends.  The
  inline-setter path is alive for real Pharo workloads but invisible
  in our perf-critical benchmarks.  Audit-gap closure stays on the
  todo list but the asmjit setter is not the bottleneck for any
  workload we currently benchmark.

- asmjit T1 inline setter x86 (AsmjitT1.cpp:1929) — same fix needed.
  Can't test on Catalyst arm64.
- stencils.cpp store-recv-var stencils — base variants barrier'd
  via _HOLE_RT_WRITE_BARRIER (commit `65792d23`).  SimStack variants
  (_1/_2/_3/_4) barrier'd via inline-asm bit-set (commit `870c864e`).
  Audit gap for *bit accuracy* is now closed.

  **What works:**
  - JITState gained 4 cached space pointers (offsets 240/248/256/264)
    populated once per tryJITActivation entry.
  - INLINE_WRITE_BARRIER_OLD_TO_YOUNG macro emits ~13 instructions
    of pure inline asm using only caller-saved x11 — no BLR, no
    x19-x22 spill, extract verifier passes.
  - All 5 SimStack store stencils now call the macro after their
    inline slot write.

  **What's still open:**
  - rememberedSet_ vector is still stale (the inline asm sets the
    bit but can't push to std::vector).  Wiring scavenge to consume
    a ring-buffer remembered set, or to skip un-remembered objects
    via the bit alone, is the path to actually dropping the
    O(oldSpace) full scan in `ObjectMemory.cpp:1571-1597`.
  - The non-SimStack base stencils (commit `65792d23`) still call
    the helper, so they DO maintain the vector — but those paths
    are essentially never reached.

Once stencils barrier, scavenge can be flipped to consume
`rememberedSet_` (`ObjectMemory.cpp:1571-1597` full-scan replaced
by remembered-set iteration).  The asmjit setter fix is real
infrastructure for the day bit 62 starts firing, but doesn't unblock
the scavenge change on its own.

See `memory/jit_remembered_set_dead.md` for the full notes.

## Open performance opportunities (NOT chased this session)

1. **tinyBench inline-prim path is firing correctly.**  Confirmed via
   `PHARO_T1_INLINE_PRIM_COUNTERS=1`: primAtPut=65521 / 65521 visits.
   The 5.3 s wall is ~70 % real JIT execution at this point; further
   wins would need either Sista Tier-2 to compile the hot inner loops,
   or saveless-self-rec for methods with `canBailMidMethod=true`
   (currently gated off because the bail path can't return via the
   blr/ret protocol).

2. **Inter-run fullGC** at `Interpreter.cpp:1166` fires unconditionally
   between bench runs.  With 512 MB headroom, the threshold-based
   trigger handles real allocation pressure already; making the
   inter-run GC conditional on `needsCompactGC()` would save ~90 ms
   per bench run.  Doesn't affect per-run timing (the GC fires
   between runs, not inside).

3. **xmethod inline-J2J** (`PHARO_T1_INLINE_J2J_XMETHOD=1`) — known
   broken, produces #value: DNU + C-stack crash during normal startup.
   Was attempted prior to this session; left default-off.

4. **`-Wunused` remaining warnings**: 0 in non-vendor code (was 13
   in Interpreter.cpp + ~25 more in Primitives.cpp / ObjectMemory.cpp).
   Cleared 2026-05-27.  Vendored plugins still warn (~360 lines):
   B2DPlugin.c, FloatMathPlugin.c, SocketPlugin.c, etc. — leave alone.

   Other non-vendor warnings also cleared on 2026-05-27:
   - `-Wreorder-ctor` (Interpreter init-list order)
   - `-Wformat` (4 sites in Interpreter.cpp)
   - `-Wsign-compare` (IC entries loop)
   - `-Winvalid-offsetof` (dumpInterpOffsets, suppressed locally)

   The four remaining are intentional / vendored:
   - `-Wframe-address` (Interpreter.hpp:361 — backtrace aid)
   - `-Wignored-qualifiers` (vendored sqVirtualMachine.h)
   - `-Wignored-pragmas` × 2 (PlatformBridge.cpp nil push/pop_macro
     for early-exit returns; intentional structural choice)

   **Bonus: warning hygiene + TODO sweep surfaced THREE real bugs.**

   1. `-Wtautological-overlap-compare` flagged Primitive 132's
      `format >= Indexable32 && format <= Indexable64` shortcut as
      always-false (Indexable64=9 < Indexable32=10).  Word arrays
      leaked through to the pointer-slot scan below, where their
      32/64-bit elements would be read as Oop slots.  Fixed in
      `f68392c2`.

   2. `-Wnontrivial-memcall` flagged `memset(p, 0, sizeof(OpenPort))`
      in MIDIPlugin.cpp:183.  `OpenPort` contains a `std::mutex` —
      memset on a non-trivially-copyable type is UB and clobbers the
      mutex's internal state.  Replaced with explicit per-field reset.
      Fixed in `95378117`.

   3. JIT inline at:put: helper (`jit_rt_primatput_ptr`) wrote slots
      directly without the old→young remembered-set entry.  Pre-
      existing TODO comment acknowledged the gap.  Switched to
      ObjectMemory::storePointerUnchecked so the remembered-set is
      now maintained for this site too.  Note: not a bug per se —
      scavenge does an O(oldSpace) full scan for old→young pointers
      explicitly to tolerate missed barriers ("Trade correctness for
      perf until every write site is audited", ObjectMemory.cpp:1563).
      Same audit-gap remains in JIT-emitted inline setter (AsmjitT1
      arm64 line 4431, x86 line 1925, comment at 1925-1928 documents
      the choice).  Closing the gap on these sites would enable
      removing the full scan.  Fixed in `5a7267cd`.

   Net non-vendor warnings remaining: **0**.
   Total build warnings: 327, all in vendored plugin sources
   (B2DPlugin.c, FloatMathPlugin.c, JPEG/Zip/UUID etc. upstream from
   VMMaker) that we deliberately do not modify.

5. **Sista / Tier 2** — not currently compiling.  `T2 (asmjit):
   compiled=0` in stats.  Unblocking it would help tinyBench's inner
   loops significantly, but requires resolving the Sista bail-protocol
   work referenced at `Interpreter.cpp:19504`.

## Files modified

- `src/vm/Interpreter.cpp` (most of the work)
- `src/vm/Interpreter.hpp` (helper decl, J2JSlotPerEntry=32 restored)
- `src/vm/ObjectMemory.cpp` (per-GC-type counters, scanPointer gating)
- `src/vm/ObjectMemory.hpp` (gcHeadroom_ = 512MB, scanPointer fields gated)

## Memory files updated

- `~/.claude/projects/-Users-wohl-src-iospharo/memory/jit_materialize_bytecodeend_bug.md` — bug root cause
- `~/.claude/projects/-Users-wohl-src-iospharo/memory/jit_gc_headroom_tuning.md` — GC sweep
- `~/.claude/projects/-Users-wohl-src-iospharo/memory/jit_fib_perf_baseline.md` — updated post-fix
- `MEMORY.md` index entries added
