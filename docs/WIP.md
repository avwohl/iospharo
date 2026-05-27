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
