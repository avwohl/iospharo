# WIP — parked-bug fix wave COMPLETE (2026-07-05)

Both deferred deterministic bugs from the 07-04 verification are FIXED, plus
a scheduler starvation bug found while closing the CONC pacing item:

- e40cd65b  jit: prim-100 simulation cascade — pk-24 arity aliasing into
  the W3 IntArithReturn inline (3 conspiring defects: arity-blind
  inlinePrimKind classification of the prim-111 MIRROR form
  Context>>objectClass:, a single-bit tbnz(52) dispatch stolen by
  pk 16-31, and an OR-combined SmI tag check that accepted (heap,SmI)
  pairs — lookupClass = contextOop+arg-1, dead young pointer, DNU
  cascade).  Stepping family recovered: 156 P / 0 F (was ~50 fails:
  StepOver/Into/Through, simulate/tally trio, testBlockCannotReturn,
  testTerminateInTerminate).  Hunt ledger: scripts/cascade-hunt/README.md
  rounds 1-11 (the decisive probe was the IC-site dump now living in the
  DNU cascade forensics).
- 8cfee28c  interp/jit: WKD testClearing warm deviation — the JIT
  activation-exit one-shot woke the P50 mourner mid-statement when the
  GC prim ran INSIDE that activation; now fires only when armed at
  activation ENTRY (interp parity).  testClearing 6/6 warm;
  WeakAnnouncer 3x clean; weak batch 1003 P / 0 F.
- 3940b62c  sched: idle-band relinquish is a preemption point — the
  heartbeat force-yield hands the CPU DOWN to P10 idle but the route
  back UP only ran at 1024-step periodics (~10-20 bytecodes per 10ms
  sleep quantum = ~1s starvation for ready waiters).  TFFI worker
  callouts 13035ms -> 95ms per 500; TFUFFIConcurrencyTest(UsingWorker)
  10s-FAIL -> 995ms PASS.  Gated to pri<=10 (ungated resurrects the
  P80<->P60 voluntary-yield bounce and TIMEOUTs whole batches).
- 709d8a43  debug-vars: 215 legacy DebugSettings bools -> debug_vars.h;
  envPresent ratchet 250 -> 31.
- Full-catalog re-run 2026-07-05: stopped externally at ~80% coverage
  (22,494 verdicts of ~28k; binary included all three fixes).  Result
  on the covered portion: 22,283 PASS = 99.06%, 149 non-pass
  (19 F / 55 E / 75 T) — ALL in the known environmental buckets
  (Rub*/Renraku/Ring2/Release cold-context timeouts at the runner's
  5-per-class cap, Roassal/Cairo, LibTTY, FL WideString, Cly async,
  OSEnvironment/NonInteractiveTranscript).  Every previously-failing
  stepping/simulation/context family PASSES in catalog context; the
  two weak/finalization flakes observed mid-run (WeakOrderedCollection
  AllGarbageCollected, FinalizationRegistry testFinalizationWithOnFork)
  pass 3/3 in isolation.  Detail file: /tmp/sunit_test_detail.txt
  (lines 1-22494 cover class-list order up to RubTextFieldAreaTest);
  tail (~5.5k tests: Spec2/S*/T*/U*/W*/Z*) not run — re-run with
  /tmp/sunit_batch.txt if the exact full-catalog number is wanted.

---

# WIP — ARM re-verification of the Windows merge: fix wave COMPLETE (2026-07-04)

The Windows-session changes (dcacc401..155d9bc4) came back to ARM and broke
startup + several suites.  All blocking regressions root-caused and fixed on
`jit` (this session, ARM/macOS):

- fb1e0d85  interp: block-NLR dynamic home is a gated FALLBACK (fae4edc1
  override let a stale closure_ redirect NLRs on JIT block returns —
  10 varying wrong-receiver DNUs killed StPharo startup before
  SUnitRunner ever registered; ownership gate = closure_'s compiledBlock
  slot must BE method_).
- d6e88006  interp: image-protocol NLR unwind targets the C++-resolved
  home (ec631963's aboutToReturn: re-derived home via `self home`; an
  inline-J2J block callee has no frame, so startCtx was the CALLER and
  delivery landed one frame short — every ensure-crossing FreeTypeCache
  glyph hit returned the CACHE; now sends homeCtx return:value through:).
- 3d83430c  sista: flush interpHints_ on every moving GC (raw oop bits,
  no lifecycle — dangling targetMethod SEGV'd LinearLifter during
  WeakKeyDictionaryTest, even under PHARO_NO_JIT).
- fa02c604 + FFI exe-dir search + fixtures: libTestLibrary.dylib now
  built/staged next to test_load_image on macOS AND the FFI bare-name
  search covers the exe dir; TestLibrary.c gained sum_*enum +
  dereferencing unref_pointer; new primitiveGetObjectFromAddress
  (PointerUtils inverse).  TFFI batch: was 17E+1F+1T scattered -> 
  134 P / 0 F / 0 E / 7 skip.
- 829ddfbe  docs: the stale startup.st CWD trap (a Jun-20 leftover
  hijacked every stock-VM prep from the repo root — looked exactly like
  a scheduler wedge; cost half a day).

ARM verification COMPLETE (2026-07-04 evening).  Additional fixes en route:
- 94d462e5  NLR protocol shape (aboutToReturn: on homeCtx — stepping
  machinery pattern-match preserved) + dead-home liveness gate
  (BlockCannotReturn semantics).
- SIGPIPE ignored globally (SocketStreamTest write-after-peer-close
  killed the VM with exit 141; Windows has no SIGPIPE).
- vmParameterAt: 9 reports the real scavenge count (statisticsReport
  divides by it unguarded; ZnServer /status 500'd ZeroDivide — 28/31
  ZnServerTest errors + the Zn/Zdc timeout cluster, ~75 tests recovered).
- runner submodule: categorized vmRegisterAsDelayRecovery (SemaphoreTest
  lint), plus primitiveGetObjectFromAddress + TestLibrary enum-sum/
  unref_pointer fixtures.

FINAL ARM NUMBERS:
- Full catalog (2031 classes / 28023 tests, 3-part run): 27280 P = 97.35%
  BEFORE the Zn/SIGPIPE fixes; the re-measured Zn/Zdc cluster alone
  recovers ~75 (ZnServer 29/31, ZnClient 49/50, ZnEasy 10/10,
  ZnStaticFile 6/6, Zdc 4x15/15, SocketStream 22/24) -> ~97.6%+.
  Remaining non-passes are (a) context/env behaviors reproducing
  BYTE-IDENTICALLY on the Jun-25 pre-Windows baseline binary (Rub*/
  Renraku/Ring2/stepping cold-context timeouts), (b) environment gaps
  (no HTTPS => Zn remote-URL tests; Spec/GUI env; LibTTY; Roassal/
  Cairo partials; FL WideString runner artifact x5), (c) two parked
  deterministic items with full evidence in deferred.md (WKD
  testClearing warm JIT timing; ProcessTest testTerminateInTerminate —
  note baseline scores 37P/5T vs our 45P/1T there).
- soogle: STON 310/310 parity; Fuel failure-set BYTE-IDENTICAL to Cog
  (719/10/19 both); PolyMath 776 vs 777 (1 slow-test timeout); NeoJSON
  parity except one network-dependent test.  Bench suite healthy
  (fib(28)=7ms), GC purges verified live on ARM incl. the jitMethods
  W^X-flip path, ZERO tripwire firings all session.


---

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
written; own-milestone risk), Authenticode signing (DONE 2026-07-05: user set up Azure Trusted Signing; latest Windows build ships signed) (previously: needs user cert
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
