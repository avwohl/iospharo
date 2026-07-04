# Handoff: Windows session 2026-07-03/04 → ARM (macOS/iOS) + x86-Linux sessions

Written on the Windows box after the x86 fix-list + deferred-items sweep.
Everything below is committed and pushed on branch `jit`
(`dcacc401..155d9bc4`).  Almost all of it is CROSS-PLATFORM core code
(Interpreter.cpp / Primitives.cpp / ObjectMemory / TestLibrary.c), so it
is live in your ARM and Linux builds the moment you pull — it was only
ever *tested on Windows x86-64*.  Re-verify per the checklist.

## What changed (by commit)

1. `e9a7e984` **Scheduler ordering (core)** — the nested callback-return
   requeue (`enterInterpreterFromCallback`) now front-appends
   (`putToSleepPreempted`): involuntary displacement must never reorder
   same-priority processes, because image code relies on
   `interpriorityYield:`'s `[p resume] fork. p suspend` dance
   (`valueUnpreemptively` runs it in every callback executor's
   termination path).  Back-append parked executors forever while they
   held the TFCallbackQueue stackProtect mutex → the repeat-run x4
   wedge.  Also `primitiveExitCriticalSection` is now Cog-parity:
   preempt only on STRICTLY higher priority; preempted active goes to
   the FRONT (was `>=` + back-append).  Aging displacement deliberately
   KEEPS back-append (front-append starved lower-priority workers ~40%;
   rationale comment in `handleForceYield`).

2. `27e4ca74` **GC weak-root treatment (core + JIT)** — fullGC/sweepGC
   mark interpreter roots with `RootScope::StrongOnly`: method-cache
   entries, JITMethod header oops, IC entries + selBitsArray, and JIT
   count/failed/tier2 map keys are no longer strong marks, so dead
   classes/methods stop being pinned for an extra cycle (ObsoleteTest
   one-cycle pin; also the WeakAnnouncer JIT-warm flake).  New
   `Interpreter::purgeDeadCacheRoots()` runs at end of markPhase (mark
   bits final, pre-compact): voids dead cache slots, invalidates
   JITMethods whose CompiledMethod died (zeroing their IC buffers), and
   `rebuildMethodMap()`.  Scavenge keeps all roots strong.  Knob:
   `PHARO_GC_PURGE_LOG=1` prints per-GC purge counts.

3. `56997740` **TFFI teardown safety (core)** — never free in-flight FFI
   resources: `callbackClosureHandler` captures retSize at entry;
   `primitiveFreeDefinition` defers `free(cif)` to a graveyard drained
   at checkpoints (gates: `callbackDepth_==0`, no xtcb pendings, no
   worker tasks in flight, >5s age); `primitiveUnregisterCallback` marks
   `CallbackInfo->unregistered` and LEAKS thunk+cif+struct (C libraries
   may hold the thunk forever; handler answers zeroes).  Cross-thread
   statics (xtcb queue/maps, worker registry, SocketPlugin lists) are
   now immortal (construct-on-first-use).  `pharo_xtcbShutdown` +
   `pharo_dnsLookupDrain` wired into test_load_image's exit path.

4. `af653a46` + `ef61b868` **TF-plain callback suite → STOCK PARITY
   (core + TestLibrary)** — five root causes:
   - **TestLibrary.c fixture semantics changed**: `singleCallToCallback`
     is now `return cb(value + 1)` (arg+1, result passed through) — the
     old `cb(value)+1` satisfied only TFUFFICallbackTest by coincidence.
     **REBUILD TestLibrary on ARM/Linux** or the TF-plain tests keep
     failing there.
   - `primitiveReleaseWorker` aborts that worker's parked forwarded
     callbacks before joining (`xtcb::abortCallbacksForWorkerThread`) —
     joining a thread parked in the 120s callback wait froze the VM.
   - Dead (aborted/timed-out) invocations buried on
     `callbackContextStack_` are skipped by the hand-out scan
     ([XTCB-DEAD-SKIP]) and lazily popped at return time too.
   - Reentrant callouts during a forwarded callback: the parked worker
     thread now SERVICES ITS OWN TASK QUEUE while waiting
     (`Worker::runOneTask`, nested ffi_call frames — stock pThreadedFFI
     reentrancy model).
   - The nested interpret loop now drains `pendingXtcbAdoption_` — it
     previously never adopted worker callbacks while a nested callback
     loop hosted execution (this also cured the warm UFFI in-suite
     flake).

5. `bceefb37` **Silent-cap hardening (mostly core)** — new loud
   tripwires: `[STORE-OOB]` (dropped storePointer), `[FWD-CHAIN-CAP]`,
   `[NS-SCAN-TERM]` (compact new-space walk terminated early),
   `[BV-SAVE-GUARD]`, unblock-snapshot cap notice; the SP-CORRUPT/
   BLOCKRET tripwire family now logs first-5 + every-4096th instead of
   going silent.  THREE pre-callback-era interceptions that answered
   nil/0 for `primNextPendingCallback`/`primNumberOfCallbacks` literal
   scans were REMOVED — lookups that reach those scanners now fail
   loudly.  (fetchPointer OOB deliberately has NO tripwire — nil-answer
   is a relied-upon probe semantic; comment at the site.)
   Windows-only bits: arc4random_buf → BCryptGenRandom, bcrypt link.

6. `155d9bc4` **Windows installer (Windows-only)** —
   `packaging/windows/` builds `dist/iospharo-0.1.0-setup.exe`, signed
   via the z80cpmw Trusted Signing kit.  No ARM/Linux impact; exists so
   you know why `packaging/` appeared.

Also: full 2047-class suite on Windows after (2): 27674/27967 = 99.0%,
exit 0 (run #25) — +233 passes over the prior baseline, errors 377→77.

## What to TEST on ARM (macOS Catalyst / iOS) and x86-Linux

Priority order; all of this passed on Windows x86-64.

1. **Build**: clean rebuild INCLUDING TestLibrary (its .c changed — on
   macOS remember `ninja TestLibrary` if your build script skips it,
   same as Windows).  Watch for new warnings in Primitives.cpp
   (xtcb/tffiworker/cbgrave sections) and ObjectMemory.
2. **GC weak-root + ARM64 JIT interplay (the riskiest port surface)**:
   `purgeDeadCacheRoots` writes JITMethod headers inside the code zone
   at end-of-mark — on Apple Silicon that is a MAP_JIT W^X flip
   (`jit::makeWritable`) at a NEW point in the GC cycle.  Run:
   ObsoleteTest x4-in-one-VM (want 3/3 every iteration), WeakSetTest,
   WeakAnnouncerTest x4 warm (want 33/34+1EF every run), WeakMessageSend,
   FinalizationRegistry, then a JIT-heavy workload (bench suite) with
   `PHARO_GC_PURGE_LOG=1` and confirm purges happen without crashes and
   fib/ensure benches are unchanged.
3. **Callback/TFFI parity**: TFCallbacksTest (want 8/8 + 2 skipped =
   stock parity; was ~1/8 before), TFUFFICallbackTest x4-in-one-VM
   gauntlet (want 4x13/13, no wedge), both InCallbacks suites (36/36),
   Derived (12/12), TFUFFIConcurrencyTest.  Then the exit-code loop:
   run TFCallbacksTest solo ~6x and check EVERY run exits 0 (this was
   the Windows teardown-segfault repro; macOS uses the same
   free-in-flight paths via different allocators — ASan run worthwhile).
4. **Scheduler suites**: ProcessTest (46/46), SemaphoreTest (18/18),
   DelayTest, StepOverTest — exitCriticalSection semantics changed
   (Mutex/Monitor users).
5. **Tripwire canaries**: grep long-run logs for STORE-OOB /
   FWD-CHAIN-CAP / NS-SCAN-TERM / BV-SAVE-GUARD / XTCB-DEAD-SKIP /
   GC-PURGE-SELBITS.  Zero firings on Windows; any firing on ARM/Linux
   is a real latent bug surfaced by the new loudness — investigate, do
   not re-silence.
6. **Removed nil-interceptions fallout**: if any Mac/Linux-only FFI path
   relied on the silent nil for `primNextPendingCallback`-style literal
   scans, it will now fail visibly.  Watch early-startup FFI errors.
7. **Full suite**: run the 2047-class catalog and compare against your
   platform's last baseline; Windows moved ~+1% (98.1 → 99.0).  Expect
   the same families to improve (Weak*, Obsolete*, TFFI callback
   suites).

## Gotchas / context you'll want

- The Windows box was DEGRADED 3-4x during part of this session
  (WmiPrvSE thrashed by 10-second `tasklist` polling loops + AV scans).
  Do NOT poll with process-listing commands in tight loops on any
  platform; watch output files for completion markers instead.  Any
  "regression" that is really a time-limit trip: re-run the baseline
  binary in-session (stash-bisect) before blaming code.
- ObsoleteTest showed in-suite 0/3 ONLY under that degradation (test
  body passes; it is GC-heavy and sits near the ~10s SUnit limit).  On
  a healthy machine it is 3/3 x4.
- deferred.md is the canonical status ledger — every entry closed this
  session carries its evidence and every remaining `- [ ]` item carries
  a disposition/plan (remaining: CONC UsingWorker pacing profiling, IME,
  MIDI backend, WorldRenderer native draw, old-space commit-ahead
  design, and that's it).
- WIP.md has the condensed session summaries + environment
  quick-reference (eval-mode invocation gotcha, crash-symbolizing
  recipe, suite-run recipe).
