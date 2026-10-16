# WIP — 2026-08-23

Current state only. This file was 4882 lines of stacked session entries going
back to 2026-05-27; on 2026-08-23 it was pruned the same way `docs/deferred.md`
was on 2026-08-12 — the history moved to
`docs/history/wip-2026-05-27--2026-08-23.md`, nothing was deleted, and the full
text is also in `git log -p docs/WIP.md`.

47 entries were classified before pruning. Five had anything still live in them;
the other 42 were superseded or finished. The facts that were still true and
recorded in **no other file** were pulled forward into "Carried forward" at the
bottom of this file, each re-verified against the tree on 2026-08-23 with a
file:line. If you are looking for something that is not here, read the archive
before concluding it was never known.

Routing to the durable docs is in the header of `docs/deferred.md`.

## Right now (2026-10-17, 15:00) -- Cog differential checks, two flakes fixed

- Pushed through `09c030db` (sweep L73 clean).  Sweep L74 runs on
  `d27dd451`.
- Speed: perform: of a primitive runs in machine code, also at frame
  depth 0 (`3ba27cc7`, 106 to 66 ms per 10M, Cog 45).  compareWith: runs
  in the primitive stub (`c8c7fef1`).  The non-local return path is
  shorter (`797efdc0`, detect: -11% instructions).
- Cog fidelity: scripted diffs against Cog (`scripts/cogdiff/`, see
  docs/vm-compat-bugs.md 2026-10-17) found and fixed compareWith:'s
  answer, SmallFloat64 identity hash, exponent of 0.0, become error
  codes, perform: of a non-symbol, dead contexts after a non-local
  return, and stale pc/stackp reads of a live context.  Primitives
  (58345 calls), processes, exceptions, objects, files and clock queries
  now agree with Cog.
- Flakes: ProcessSpecificTest>>testLocalVariable was a VM race in
  primitive 230 (`d27dd451`, 0 of 12 fail, 6 of 12 before).  The
  in-memory event ring `PHARO_EVENT_RING` caught it.  Next:
  FreeTypeCacheTest, with the same tool.
- Harness: test_load_image eval doubles `!` (chunk format) and prints the
  result as UTF-8 (`e9f2fd0d`).
- Not done: `thisContext` in Exception>>signal materializes every frame
  (13% of a signal); Cog marries only the top frame.
- Later the same day: LocalePlugin and FilePlugin primitives registered
  under the image's names; reverse DNS lookup and the SocketPlugin
  address-info API (IPv6 connect, bind, accept) added in Cog's layout;
  exit without fixed sleeps (an eval of 1: 1.33 s to 0.34 s, Cog 1.0);
  inline allocation fills two slots per step; full GC skips pointers below
  the first mover (42 to 37 ms, Cog 23 under Rosetta).
- FreeTypeCacheTest testGlyphAccessIsThreadSafe: 5 of 6 alone fail on L73,
  3 of 6 on L74 and L75, 0 of 12 on L76.  The test races its own
  renderers by design; faster code (L76 compiles faster) leaves fewer
  preemption windows.  Treat it as timing, not fixed.
- TraitFileOutTest fails in every full sweep since L73 and passes in any
  shorter run: an IdentitySet-order dependency in the image
  (docs/image_issues.md).
- Full GC 42 to 32 ms (Cog 23 under Rosetta): the compaction update skips
  pointers below the first mover, the mark loop filters inline and marks
  plain objects without a call, and the clear walk is folded into the
  bitmap walk (30 GCs: 14.5 to 11.3 billion instructions).  The skip
  exposed purgeDeadCacheRoots leaving the code zone writable (SIGBUS after
  ~100 GCs), fixed in `815e03a9`.  Sweeps L75-L77 found nothing new; L78
  runs on `fd13c3b7`.
- Micro set vs Cog now (machine loaded): 15 of 24 at or under 1.1x.  Left
  over 1.2x: perform: of a primitive 2.1x, detect: 1.5x, exceptions 1.4x,
  isLetter 1.3x.  The last three come down to context creation and
  materialization (a married context is nil-filled and gets its temps
  copied; `thisContext` materializes every frame).

## Earlier (2026-10-17, 08:00) -- sweeps L67-L69 clean, exception returns at Cog speed

- Pushed through `aa9d98c8`.  Sweeps L67, L68 and L69 are clean.  L69:
  27812 of 27996 pass.  The two non-passes are SpAthensAdapterTest (UI
  race) and ZnClientTest testQueryGoogle (live network, Cog fails it too).
  Sweep L70 runs on `797efdc0`.
- `963006ad`: an FFI callback ends with a return status, not a C++
  exception (90.7 to 83.1 us per callback).
- `4d445bc9`: the resume shortcut.  `Context>>resume:` with no unwind
  frame before its receiver resumes the receiver directly, with no
  thisContext materialization (docs/native-nlr-plan.md).  Exception
  signal + return: 100 ms per 100000, Cog 191-242 cold and 45 warm.
- `797efdc0`: a non-local return over heap contexts now kills each
  context it unwinds (sender and pc nil), as Cog does.  Before, a context
  captured in a `do:` block stayed alive after `^`.  The same commit
  trims the machine-code non-local return (detect: -11% instructions).
- FreeTypeCacheTest testGlyphAccessIsThreadSafeWhenAskingWidth timed out
  once in L67 and passed in L68 and L69.  The test says it "breaks 95% of
  times".  Alone, it fails about half the runs on several builds.
- Micro set vs Cog (warm, machine loaded by a sweep): writeStream 13 vs
  11 ms (an earlier 38 was a one-off).  Left: perform: of a primitive 3x,
  detect: 1.7x (a closure creation marries a context, and the non-local
  return goes through C++), symbol compare 1.6x.

## Earlier (2026-10-17, 01:00) -- sweeps L63-L66 clean; perform cache complete

- Pushed through `411004ba`.  Sweeps L63, L64, L65, L66 clean (L65:
  27812 of 27996 pass; the non-passes are SpAthensAdapterTest's UI race,
  ZnClientTest's live Google query, and known timing flakes).  Sweep L67
  runs on `f4ecac2f`.
- perform: cache: native calls, quick methods, primitives
  (`02087199`, `a642af4f`, `3578d86a`); `f4ecac2f` clears it in
  flushJITCaches (the selective flushes after a method change).
  docs/mc-frames-design.md, "Speed work, 2026-10-16".
- The ReStore package rerun passes SSWReStoreProxyNonForwardingTest 31/31
  (the pc3 22/31 was a one-off; its Preempted tests depend on process
  timing).  The x86_64 tree builds and passes this session's semantic
  checks under Rosetta.
- Measured, recorded in docs/deferred.md: FFI callbacks 85 us vs Cog 9
  (step() loop, process switches, C++ unwind); full GC 1.5x Cog (marking).

## Earlier (2026-10-16, 21:00) -- perform cache; per-class run 714 s vs Cog 1546 s

- Pushed through `046bfe7c` (sweep L63 clean: 27996 tests, 27811 pass).
  Sweep L64 runs on `e4c7e238`.
- Since L63: `02087199` machine-code perform: cache (perform: #size 32
  -> 15 ms, Cog 14), `a642af4f` quick methods answer from it (collect:
  #accessor 2x faster), `e4c7e238` primitive 197 callable natively
  (exception signal + return -6.6% instructions), `41f1738a` MethodMap
  hash folds the high half (lexicon -0.5%).
- Per-class revalidation of the 93 package images (pc3, build
  `046bfe7c`): ours 714 s, Cog 1546 s.  Pass-count differences: pharo-cig
  (package bug), StSpotterModelTest and StTranscriptPresenterTest (ours
  better), SSWReStoreProxyNonForwardingTest 22/31 in package order (31/31
  alone and after all 69 classes before it in one eval; a rerun of the
  package is in progress).
- Micro set vs Cog (machine loaded): faster on loops, streams, String =,
  copies; perform: of a primitive 4.6x, exceptions 2.1x, the rest within
  1.5x.

## Earlier (2026-10-16, 17:00) -- sweep L62: 3 non-passes, none a VM defect

- Pushed through `49ce55e5`.  Sweep L62 (image sunit8, runner
  `1bbfc99`): 27996 tests, 27811 pass.  Left: SpAthensAdapterTest (UI
  scheduling race, docs/deferred.md), ZdcSecureSocketStreamTest testPlain
  (network flake), ZnClientTest testQueryGoogle (Cog fails it too).
- Runner: StDebuggerTest runs through `runCaseManaged` (its
  testIsInSelectedContextPackage reads the standard SUnit frames).
- Not yet swept: `284e9ba4` lazy become path compression, unforward every
  4096 pairs (SSWReStoreSpeedTest 2.3 -> 0.74 s, Cog 2.3); `046bfe7c`
  lazy become accepts overflow-header objects (microdown HTML tests 1.7x
  faster).
- Running: per-class revalidation of all 93 package images with the
  newest build (scratchpad pc3/, failing selectors recorded by per2.st).
- Measured, not fixed: an exception signal + return is ~25500
  instructions, 20000 in C++ (three full frame materializations for
  thisContext, a lazy-context sync for `sender`, four exits).  Cog marries
  one frame per `sender` step; doing that here means incremental lazy
  contexts.  perform: of a compiled method is ~330 instructions (Cog ~40);
  a machine-code perform cache would cut most of it.  tinyBenchmarks:
  bytecodes at parity with Cog, sends 68% of Cog.

## Earlier (2026-10-16, 12:00) -- lexicon faster than Cog; package tests 2x faster than Cog

- Pushed through `af483604`; sweeps L59, L60, L61 clean (L61: 27996
  tests, 27810 pass, 4 known failures).
- `9e929309`: a non-local return through one `ensure:` frame skips the
  image unwind protocol (docs/native-nlr-plan.md, "Ensure shortcut").
  Lexicon does 1.7M of them (Symbol class>>intern: via critical:):
  222.5 -> 187.5 billion instructions, 13.4 -> 11.4 s.  Cog: 11.6-12 s.
- `8c0b1283`: immediate literals are code constants (benchFib -9%
  instructions).
- `8348ea18`, `89006f0f`: a scavenge visits only the class-table entries
  written since the last one.  The full walk was ~80% of a small
  scavenge; Soil pins 46k buffers (one scavenge each with 4a4fd340) and
  went 4 s -> 236 s with 15 failures in package order.  Found by the
  per-class timing run (scratchpad pc2/).
- `374ec1b7`: pin of a young object by forwarding (Spur), not a scavenge
  in the primitive.  The scavenge hung ReStore's SQLite tests since
  4a4fd340.  Named primitives and the FFI callouts follow forwarders.
- Per-class package timing (93 package images, Cog side from the 10-15
  run): ours 740.7 s, Cog 1545.5 s on the same classes.  Remaining
  pass-count differences: pharo-cig (a package binding bug, see
  docs/vm-compat-bugs.md), PropertyManagerTest (finalization timing after
  a test that fails on Cog too), StSpotterTest (not yet looked at).
- Memory notes: async-writer-breaks-check-then-act,
  fix-that-adds-gcs-needs-a-gc-heavy-timing.

## Earlier (2026-10-16, 06:00) -- read-only countdown, perform fixes; sweep L58

- Pushed through `42440da8` (sweep L56 clean; its one new entry,
  StPlaygroundPageTest, passes 3 of 3 alone).  Since L55: MCF record trim
  stages 1-4 (docs/mcf-record-trim-plan.md; stage 1, the paired depth and
  context, is opt-in `PHARO_MCF_PAIRED_DEPTH`: the ldp/stp pair caused
  store-forwarding stalls, benchFib 38 -> 53 ms), body plan step 5
  (binary operations pop memory operands into registers), a pending
  method result goes to x1 for ReturnTop.
- `e04ed3b2`: native calls only read the checkpoint countdown; the
  heartbeat, external signals and a full eden set it to 0.  fibA 33 ->
  25 ms, benchFib 38 -> 35 ms.  Sweep L57 clean.  It opened a race:
  `f48b7172` gives perform: a call stub without the second countdown
  test (a perform loop failed after ~1.5 s with "selector argument must
  be a Symbol").
- `be44a84b`: perform: of a quick-primitive method answers in place
  (52 -> 27 ms for 3M; Cog 10).  perform: #+ with: is still 53 ms (Cog
  10): lookup, call-entry hash and primitive call, no single hot spot.
- Tried, no gain, reverted: predicted returns (bl/ret), frameDepth_ in a
  register (docs/deferred.md).  Memory note
  per-call-memory-chains-cost-wall-not-instructions.
- NLR through ensure: is 2.3x Cog (exA micro 260 vs 115 ms): two full
  frame materializations per return, the second for thisContext in
  resume:through:.  Plain ensure: and on:do: are 2-3x faster than Cog.
- Running: sweep L58 on `be44a84b`; a fresh per-class timing run of our
  VM over the 93 package images (scratchpad pc2/, Cog side reused from
  pc/) to find the current outliers.

## Earlier (2026-10-16, 01:00) -- pin fix, image test fixes, call-record design panel

- Pushed through `9d3489bc`.  Sweeps: L49 (segment on by default) clean;
  L50 (leaf primitives) clean apart from FFICallbackTest, which found a
  real VM bug: `primitivePin` left a pinned young object in eden, so a
  scavenge inside a qsort callback moved the buffer under C (`4a4fd340`:
  pin now scavenges first; repro 16 of 20 wrong before, 0 after; memory
  pinned-young-objects-must-tenure).  L51 clean apart from load flakes.
- `a56aab58`: leaf primitives called from machine code skip the register
  sync and executePrimitive; primitive 158 (String/Symbol compare) runs
  in place.  Symbol `<` 30 -> 14 ms (Cog 9).
- `0c70c9d4`: binary bytecodes load operands with one ldp and push the
  result through x25 (body plan step 3 part 2).
- `9d3489bc`: `scripts/pharo13-image-fixes.st` fixes six tests that fail
  identically on stock Cog (renamed trait, stale allowlist, a shadowed
  instance variable, a racy debugger provider, two swapped-index layout
  tests).  docs/image_issues.md lists them; CLAUDE.md prep files it in.
  Sweep L52 runs on the new image `sunit7` (scratchpad prep7.sh).
- Speed against Cog (Cog under Rosetta): tinyBenchmarks bytecodes 4.7G
  vs 5.0G per second, sends 280M vs 405M.  Lexicon 226 billion
  instructions.  Microbenchmarks: we are faster on loops, String =,
  copies; slower on exceptions (2.3x), perform: (now 4x), isLetter
  (1.6x, shared primitive stubs), Dictionary at: (1.25x).
- The send gap is the native call record: ~58 instructions per call pair
  against Cog's ~25.  A design panel (workflow mcf-record-trim-2) is
  mapping the C++ readers of each record field for a staged trim.
- Tried and reverted (no gain): Context-primitive sync skipped for
  non-running contexts; eden-full early checkpoint; inline basicNew: in
  the 1-argument primitive stub (the IC path already allocates inline).

## Older entries

Entries from 2026-08-23 to 2026-10-15, and the "three tiers", "Landed
today" and "Open" sections of 2026-09, are in
`docs/history/wip-2026-08-23--2026-10-16.md` (moved 2026-10-17,
unchanged).

## Carried forward from the archive

Still true, and recorded in no other file. Each was re-verified against the tree
on 2026-08-23; line numbers are from that day. Grouped, terse, and deliberately
not expanded — the full narrative is in
`docs/history/wip-2026-05-27--2026-08-23.md`.

### JIT / code zone

 1. **evictLRU pass 2 is address order, not recency.** Pass 1 needs ~650K
    method entries untouched (64K per epoch, `epoch_-10` threshold,
    `CodeZone.hpp:313`), so pass 2 usually runs and walks `firstMethod_`
    (`:331-344`), evicting the earliest-compiled kernel methods that stay hot.
    `763f7f7b` fixed the epoch never advancing; the fallback is unfixed.
    Planned: progressively younger thresholds (`epoch_-5, -2, -1`, then all).
 2. **The evictLRU second-pass change was measured and DROPPED** — 1 MB zone
    8 s/277 methods vs 7 s/286; 192 MB zone (XMLParser load) 344 s/5769 vs
    340 s/5542. Re-open only with a workload that fills the zone *and* advances
    the epoch.
 3. **248,066 methods compiled during one XMLParser load** against ~176k
    CompiledMethods+CompiledBlocks in the whole image (recompile churn).
    `evictTarget` is 1/64th of the zone (`JITCompiler.cpp:1780-1782`) and may
    want raising now that a round is cheaper.
 4. **Bit-60 J2J precedence makes five inline-return specializations dead on
    arm64** — the bit-60 `tbnz` is emitted before the extras dispatch
    (`AsmjitT1.cpp:6488`, dispatch at ~6498/6506/6512/6525/6540). Derived
    upside ~3.9% on a 356 ms workload. **The fix is the reorder**;
    `PHARO_NO_J2J=1` alone makes things slower.
 5. **arm64 emits 2.7-2.9x more bytes per compiled method than x86_64** — 7.89
    vs 2.76 KB/method; 8.91 vs 3.31 across full sweeps. This is why arm64 fills
    the code zone first.
 6. **35% of arm64's code footprint is inline-J2J** — 45,176 KB drops to
    29,260 KB with `PHARO_T1_NO_INLINE_J2J=1` (`debug_vars.h:92`).
 7. **IC hit rates are not comparable across arches**: arm walks IC slots 0-2
    inline (`AsmjitT1.cpp:6331-6363`), x86 probes slot 0 and exits
    (`:3185-3189`), so the two builds count different event populations.
 8. **`[JIT] Stats:` lines are periodic checkpoints, not totals** — one every
    65,536 method entries (`JITRuntime.cpp:~3896`). Two mid-run samples were
    once published twice as evidence of a structural arm-vs-x86 gap; real
    end-of-run totals are 0.56% apart.
 9. **`PHARO_T1_INLINE_PRIM_COUNTERS=1` gotcha** — without it `g_primAt_hits`
    and friends stay 0 even when the inline path is firing, giving a misleading
    "primAt=0".
10. **The asmjit T1 inline-SETTER path never fires in micro-benchmarks**
    (getter=16059, setter=0; `detectTrivialMethod` runs ~168 times per
    tinyBench and never classifies a setter). The recognizer is not broken —
    fib/sieve/tinyBench contain no setter sends.
11. **DEAD END, do not re-walk**: adding the inline-asm write barrier to the six
    temp-store stencils (`storeTemp_1/_2`, `popStoreTemp_1/_2/_3/_4`,
    `stencils.cpp:~4496-4540`) left the miss count unchanged at 230 and
    regressed tinyBench ~11% (5350 -> 5950 ms). JIT code runs `frameDepth_ > 0`,
    so that path is virtually never exercised. Reverted.

### GC / memory

12. **The remembered set is dead infrastructure.** `rememberedSet_` is populated
    (`ObjectMemory.cpp:3712`) but never iterated — only `.clear()`ed (`:2321`);
    scavenge still does an O(oldSpace) full scan ("Trade correctness for perf
    until every write site is audited", `:2221-2226`). Closing it would drop
    ~30 ms/scavenge on a 100 MB heap. The audit reached 260 -> 228/230 misses at
    99.997% bit accuracy; residual misses are C++ paths, not the JIT path.
13. **`PHARO_WATCH_ROOT_CLASS` (`debug_vars.h:201`) is an exact class-name
    compare** — it does not follow subclasses, so check `allSubclasses` before
    reading a zero-hit result as meaningful.
14. **Raw-heap-scan gotcha**: `PHARO_SCAV_RAWSCAN`
    (`ObjectMemory.cpp:2434-2513`) scans format-agnostically and hits
    `ExternalAddress` objects (fmt 16, ptrSlots=0) that legitimately store
    native addresses — all 54 hits in one run were such false positives.
15. **`[SCAV-DANGLE]` shares the scavenger's blind spot** — both walk the same
    `forEachRoot` set (`ObjectMemory.cpp:2188` and `:2421-2425`), so a holder
    outside that set makes the diagnostic report clean while the object dies.
    A clean dangle scan is not proof the roots are complete.
16. **`ObjectMemory::setGlobal`'s tail** (`ObjectMemory.cpp:~1063-1073`) walks
    the dictionary array for the first nil/zero slot and stores there, with no
    hash placement and no tally update. Unresolved, recorded nowhere else.
    (The `68` DNU that led here *is* fixed; see `ObjectMemory.cpp:1029-1035`.)

### Scheduler / VM control

17. **`Interpreter::running_` is a plain `bool`** (`Interpreter.hpp:1168`),
    written by `stop()` (`:285`) and read by the dispatch loop — a real data
    race, and the leading candidate for the `vm_stop` timeout. Left standing
    **deliberately**: `std::atomic` puts a real load in the hot dispatch loop,
    so it needs a bench-suite before/after, not a blind edit.
18. **`removeProcessFromList`'s return value is discarded**
    (`Interpreter.cpp:18685`, `Primitives.cpp:5017`) and the caller then
    unconditionally nils `ProcessNextLinkIndex`, truncating the run queue if the
    search failed. Wants a repro before touching the scheduler.
19. **`vm_stop` timed out on roughly one relaunch cycle in thirty** before the
    later fixes (10 runs: 9 pass, 1 fail, one stop-timeout, one destroy-leak).
    Post-fix runs show zero. Budget that rate for any repro attempt.
    Instrumentation is live at `PlatformBridge.cpp:618` and `:669`.

### Measurement traps

20. **We do NOT set `PHARO_CI_TESTING_ENVIRONMENT`**, so our sweeps run the 17
    tests Pharo's own CI skips via `skipOnPharoCITestingEnvironment` —
    including `AllocationTest` testOneGB/testOneGWordAllocation,
    `RandomTest>>testUnixRandomGeneratorSeed`, `StDebuggerTest`, `StSpotter*`,
    `StPlaygroundPageTest>>testContents`, `TKTNewProcessTaskRunnerTest`, and
    `WeakAnnouncerTest>>testNoDeadWeakSubscriptions`. Check the guard before
    calling any of them a VM defect.
21. **Classes that time out emit no `Total:` line** and drop out of the
    per-class tally (`run_sunit_tests.st:582-598`) — which is why arm counted
    2043 and x86 2037 against 2047 declared. A class-count difference between
    runs is not evidence of a crash.
22. **Package-tier noise floor is ±2**: four PolyMath runs on one day scored arm
    err = 16, 16, 17, 18. Any arm-vs-x86 package delta needs two runs per side.
    `sunit-sweep.sh:95-102` states the equivalent rule for SUnit;
    `package-tests-selfhosted.sh` does **not**.
23. **A killed run reads as a passing one.** A bisect reported
    `noyg rc=124 mnu68=0` and the zero was a TIMEOUT — 900 s is not enough for a
    no-young-generation run (`PHARO_NO_YG`, `DebugSettings.cpp:379`). Record rc
    and an explicit completion marker alongside every count.
24. **"It did not reproduce in N runs" is not "it does not happen"** unless N
    suits the rate — 9 consecutive clean runs were once cited as proof against a
    ~10% event, and a correct conclusion was retracted on that basis.
25. **Verify the binary contains the change before blaming the architecture.**
    `build-x86` had not been rebuilt after the class-registration fix, so x86
    reported the pre-fix signature and read as an arch-specific GC defect. Both
    trees must be rebuilt (`build-rel` arm64, `build-x86` x86_64);
    `strings <binary> | grep <marker>` settles it.
26. **`bash -n` passing does not mean a driver script works** — a Smalltalk
    `"comment"` inside a double-quoted eval string terminates the string.
    Smoke-test every generated eval.

### Host & tooling

27. **`scripts/sunit-sweep.sh` still defaults `PER_BATCH_TIMEOUT=600`**
    (`:111`), which is too tight — batch 1501-1550 legitimately takes 613 s and
    is not a hang. `scripts/rerun/fullsweep.sh:9` sets 900.
28. **`scripts/rerun/fullsweep.sh:4` and `scripts/rerun/pkgrun.sh:9` have a
    session scratchpad path baked in** — fix that line before rerunning either.
29. **After a reboot clears `/private/tmp`, run `git worktree prune`** — git
    otherwise still believes the branch is checked out in the deleted worktree
    and refuses `git checkout jit`. There is a live scratchpad worktree today.
30. **x86-box constraints**: gdb there cannot read member vars (no full DWARF),
    so instrument in C++ instead; `gdb -p` needs
    `sysctl kernel.yama.ptrace_scope=0`; `PHARO_T1_TRACE_HANDLER`
    (`debug_vars.h:288`) dumps the handler-search chain.
31. **Cog-x86 baseline** measured on the x86 box (18.221.159.216), recorded
    nowhere else: loop20M ~44-50 ms, fib30 ~9-10 ms, cfibx30 ~13 ms.
32. **arm64 cost baselines** (ad-hoc evals, no committed script): INTADD3M
    4 ns/iter; BLOCKVAL3M 72 ns per block `value:`; ALLOCARR2M 93 ns per
    `Array new: 4`; COMPILE300 1.54 ms per `Compiler evaluate: '1+1'`. Block
    activation and small-object allocation sit 18-23x above the fast integer
    path.
33. ~~**Whether this VM is slower than Cog on these workloads is unanswerable
    on this host**~~ — SUPERSEDED 2026-09-02. The abort is arch-specific: the
    **x86_64** Cog runs under Rosetta (`arch -x86_64`, see CLAUDE.md), so the
    comparison IS available, and it is fair against `build-x86` since both are
    x86_64 under Rosetta. Separately, Rosetta overhead vs x86_64 backend code
    quality has still never been split.
34. **`-Wnontrivial-memcall` has an array-form blind spot** — it fires on
    `memset(p, 0, sizeof(OpenPort))` but not on
    `memset(gPorts, 0, sizeof(gPorts))`. Any warning-driven sweep inherits the
    hole. (Re-reproduced on Apple clang 21.0.0.)
35. **`LinkInstallerTest>>testPropagateNewClassScopedLinksOnMethodNode` is
    GC-pressure-dependent, not the JIT** — 40/40 alone with JIT on or off; fails
    with any predecessor class; `PHARO_NEWSPACE_MB=1` makes it fail alone but
    only sometimes (39/40 and 40/40 on identical re-runs). Needs repetition
    counts before believing any knob bisect.
36. **Do not re-chase the `cos(2)` 1-ULP claim.** The image defines
    `Float>>cos` as `^ (self + Halfpi) sin`, and computing *that* formula with
    this host's libm reproduces the VM's bits exactly. No discrepancy exists.

### Corrected while pruning

37. **The WideString-WriteStream bug is FIXED** (`d5608fd4`, 2026-06-01), but
    `docs/results-jitpkg.md:519-521` still cites "the already-documented
    WideString-WriteStream bug (WIP.md)" as if open. Fix that citation rather
    than re-investigating.
38. **`indexOfClass` returning 0 for not-found** (`ObjectMemory.cpp:628,649`) is
    still true, but the "any new caller is unguarded" half is now stale — 5 of
    the 10 enumerated callers were guarded on 2026-08-23 (`b0ea7721`).
