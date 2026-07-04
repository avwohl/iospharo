# WIP — x86/Windows JIT fix list: COMPLETE (2026-07-04)

The 2026-07-03 fix list (#14 repeat-run wedge, #11 ObsoleteTest one-cycle
pin) is DONE, the full-suite goal gate passed above baseline, and the
teardown-segfault family found en route is fixed too.  All committed and
pushed on branch `jit`:

- e9a7e984  sched: callback-return requeue must preserve same-priority
  order — THE #14 wedge fix (with dcacc401's exactly-once hand-out
  underneath).  x4 gauntlet 5/5 clean runs, ~75s vs wedging forever.
  Full mechanism: docs/deferred.md (#14 entry) + memory
  `scheduler-order-invariant.md`.
- 27e4ca74  gc: weak-root treatment for VM caches (#11) — RootScope
  StrongOnly mark + purgeDeadCacheRoots; ObsoleteTest 3/3 x4,
  testFixObsoleteSharedPools at stock parity; dead classes no longer
  pinned by VM caches at all.
- 56997740  tffi: teardown segfault family — never free in-flight FFI
  resources (entry-captured retSize; cif graveyard w/ gated drain;
  unregistered-callback thunks leaked immortal; xtcb shutdown unparking;
  immortal cross-thread statics; DNS drain).  TFCallbacksTest exit-loop
  8/8 exit 0 (was 2/6).

FULL-SUITE GOAL GATE (run #25, on 27e4ca74): 2047/2047 classes,
27967 tests — 27674 pass (99.0%) / 52 F / 77 E / 155 skip / 9 timeout,
exit 0, ~5700s.  Baseline run #9: 27441 (98.1%); run #7: 377 E /
25 timeouts.  Net: +233 passes, errors -300, timeouts -16, ~45 min
faster.

## Deferred-items sweep COMPLETE (2026-07-04, second goal)

Every fix-shaped deferred item is now closed (commits bceefb37, af653a46,
ef61b868).  Highlights:
- TFCallbacksTest: 1/8+3F+4E -> **8/8+2skip STOCK PARITY**, five root
  causes (TestLibrary fixture arg+1 semantics; release-while-parked join
  freeze; buried-dead invocation hand-out; reentrant callouts needing
  the parked worker to service its own queue; missing xtcb adoption
  drain in nested callback loops — the last one also cured the warm
  UFFI in-suite flake).  Full story in deferred.md's TFCallbacksTest
  section.
- Verified-stale entries closed with evidence: WeakAnnouncer warm parity
  (fixed by 27e4ca74), NetNameResolver localhost (hostname prims),
  MicText HugeFont 21/21 (Cairo stack), InLoop(UsingWorker) 13/13,
  TFFI v2 (landed), SDL2/Morphic GUI parent entry (on-screen + input
  verified with screenshots 07-01/02).
- Silent-cap residue batch: loud-not-silent tripwires (STORE-OOB,
  FWD-CHAIN-CAP, NS-SCAN-TERM, BV-SAVE-GUARD, rate-limited SP-CORRUPT
  family), 3 stale callback-polling interceptions removed, fetchPointer
  nil-answer documented as API semantic (tripwire attempt false-posed).
- arc4random_buf -> BCryptGenRandom (links bcrypt); UUIDs verified
  distinct across runs.
- Closed by design: SIGSEGV recovery (dump-then-crash is the tool that
  solved the teardown family), chown ENOSYS, ARM64-Windows trampoline.

Remaining open (features/blocked, NOT fixes — see deferred.md):
CONC UsingWorker pacing (needs quiet-machine profiling; data captured),
IME, MIDI backend (unverifiable: no image-side MIDI classes),
WorldRenderer native fast path, old-space commit-ahead (design note
written; own-milestone risk), Authenticode signing (needs user cert
decision).

ENVIRONMENT CAVEAT for this session's numbers: the machine was degraded
3-4x from ~03:30 (WmiPrvSE at 12 CPU-hours from tasklist polling loops +
ESET scanning; benchFib 12ms -> 40-55ms) — ObsoleteTest's in-suite 0/3
during this window is the time-limit artifact (test body passes via
direct performTest); see memory wmi-polling-hazard.

## Environment quick-reference

- Build: `/c/temp/src/iospharo-jit/scripts/build-windows.sh` (MSYS2
  CLANG64).  Kill test_load_image.exe before rebuild (link EPERM).
- Probe image: /c/tmp/probe-img/Pharo.image.  Eval mode REQUIRES the
  `eval` keyword: `test_load_image.exe <image> eval "<expr>"` (bare
  launch boots the GUI idle and deletes the staged startup.st).
- Suite env: /c/temp/pharo-win-test/Pharo-sunit.image; run
  `... eval "(Smalltalk at: #SUnitRunner) runAllTests"`; results in
  /c/tmp/sunit_test_results.txt; clear /c/tmp/sunit_class_names.txt /
  sunit_batch.txt / sunit_run_completed.txt first.
- Crash triage: [WIN-CRASH] backtraces in the log; symbolize with
  `llvm-addr2line -f -C -e test_load_image.exe 0x14XXXXXXX`
  (0x140000000 + printed exe offset), against the SAME binary.
