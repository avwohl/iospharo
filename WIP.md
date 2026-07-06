# WIP — /goal "fix all non-Windows bugs" session 2026-07-06 (day)

STATE ~14:00: **CATALOG #4 (ARM/macOS) COMPLETE: 27,704 P / 39 F / 66 E /
8 T / 144 S = 99.60%** (99.70% after subtracting the 29 FL/Cly
expectedFailures bogeys whose runner fix landed post-prep).  NO wedge
carpets, single VM, no batch splitting — vs catalog #2's 98.78%/199 and
the four-batch workaround.  Box (x86) run #2: 27,525 P / 41 F / 240 E /
13 T = 98.95%; its extra ~170 TF*/TFUFFI errors + LibTTY + DiskFileAttr +
Athens/RS were ALL x86-environment bugs, all fixed since:
- exe-dir bare-name library search now works on Linux (/proc/self/exe) +
  .so candidates + fixture libs are test_load_image deps (cfe00911) —
  TFBasicTypeSize 48/48 on box (was 0/48).
- libtty _GNU_SOURCE: glibc's undeclared ptsname() truncated the pointer
  (EFAULT in child; parent read() on never-opened master blocked the VM)
  — LibTTY 5/5 on box (cb53b45f).
- DiskFileAttributesTest 24/24 on box (platform-name fix 7df34d01).
- Athens 17/17 + Cairo/FreeType on box (cairo stub gate + resolution
  order + basename fallback).
FINAL VERIFICATION RUNS IN FLIGHT: local catalog #5 (final runner
b67db07) + box suite #3 (fresh image, all fixes).  Remaining known
non-env singles: FTTableMorph alternate-row-colors (SubscriptOutOfBounds,
World-cycle family), RBBrowserEnvironment flake, Sp* tree/table adapters
(~21, headless World-cycle project), reflective-slowness TIMEOUTs x8
(activation-wall perf project), ReleaseTest hygiene (prep pollution).

Machine rebooted (stock-VM eval hang CLEARED; /tmp wiped — harness rebuilt
from fresh Pharo 13 download).  Two full-suite runs in flight:
- LOCAL ARM catalog #3: fresh image + fake-GUI + hardened runner baked,
  2052 classes, PHARO_MAX_STEPS=4e12 + CODE_ZONE_MB=192
  (results/detail in /tmp, log /tmp/catalog3_run.log).
- AWS x86 spot box i-01a4ea1f7bea4bc74 (3.17.153.16, m6a.4xlarge,
  ~/x86-fullsuite.sh, results in ~/results/): full suite at HEAD.
  Manual lease beats needed (./scripts/aws/lease.sh beat <iid>) —
  AWS_LEASE_IID isn't in this Claude's env so the hook no-ops.

FIXED so far this session (committed + pushed):
- 259ad3fc interp: missing <condition_variable> include broke Linux
  (libstdc++) builds — found by the box build.
- ef9cfa59 prims: GCC rejects pharo::-qualified definitions inside
  namespace pharo (g_prim111Ring).
- 4e6bb7d0 plugins: ship libtty (tty_spawn clean-room) — LibTTYTest
  0/5 -> 5/5 (stock ships Plugins/libtty.dylib; we never had it).
- 2527c2c (runner submodule, parent 8c1ae366): honor expectedFailures-
  METHOD declarations (not just the pragma) — kills 30 bogus non-passes
  (FL* x15, ClyAsync/ClyFilter x14, ClySemiAsync x1).  NOT in the baked
  image of the in-flight catalog #3 — subtract those families manually.
- TLS/HTTPS VERIFIED WORKING on macOS (sqMacSSL; ZnClient https 200) —
  catalog #2's "ZnHTTPS (no TLS)" label was stale.

WEDGE **RESOLVED** (~12:20): runner race, NOT a VM bug — the watchdog set
testDone/printed the verdict BEFORE killing the test; the P80 runner main
woke on testDone and its watchdog-cleanup path suspended the P60 watchdog
between the flush and the suspend/terminate — the runaway test was never
killed and starved its whole priority band (front-of-queue after
preemptions).  FIX: kill first, announce after (runner submodule b1a783e,
parent c3376e7c).  Verified on the wedge class-list: 50 P / 5 F / 1 T
(only testNoShadowedVariablesInMethods genuinely >80s, killed cleanly).
The 5 F are harness-hygiene (fake-GUI prep classes trip ReleaseTest
package/selector checks; testPharoVersionFileExists fails on stock too).
Memory: sunit-runner-kill-race.md.  CATALOG #4 RUNNING locally (fixed
runner + fake GUI, fresh prep); box full-suite #2 RUNNING (fixed runner,
fresh image; cairo now resolves via basename fallback aade2dde —
AthensCairoMatrix 17/17 on x86, was 0/17).

Historical hunt notes follow.

WEDGE HUNT STATE (~12:00): catalog #3 KILLED at 22,463 verdicts (unrecoverable
carpet; artifacts in scratchpad catalog3_*_partial.*).  Repro IN HAND:
class-list [ReleaseTest, NonInteractiveTranscriptTest, OSEnvironmentTest] +
PHARO_CODE_ZONE_MB=32 on the prepped harness image wedges identically
(first ReleaseTest scan test times out at 80s, its process NEVER dies,
everything at P40 starves behind it).  EVIDENCE CHAIN:
- lldb chain-walk of wedged victim: test frames (testNoShadowedVariables ←
  performTest ← ensure: x3 ← runCase ← on:do: x8) sitting ON TOP of
  terminateRealActive ← jump — i.e. Pharo-13 termination machinery
  (doTerminationFromAnotherProcess → parallel stack → Context>>unwindTo: →
  runUntilReturnFrom: → jump) RESUMED THE WHOLE TEST instead of just the
  unwind blocks.
- unwindTo: resumes-to-outerMost only when some ensure frame's
  unwindComplete (tempAt: 2) is non-nil.  Bare-eval probes (also with
  8MB zone): materialized ensure frames all have complete=nil and correct
  tempAt:1 (cleanup block) — static state is CLEAN; corruption/decision
  happens during the runner-context kill (suspect: env-watchdog
  TestTookTooMuchTime signalException at ~60s starts a legit unwind, our
  80s watchdog kills MID-UNWIND → resume-to-outerMost path → but then the
  'unwind completion' re-runs the test = the divergence to find).
- PROBE IMAGE BAKED: /tmp/harness/Pharo-probe.image has traced
  Context>>unwindTo: + Process>>doTerminationFromAnotherProcess ([UWT]/
  [DTAP] stderr lines; bake script scratchpad/trace_unwind.st).  Wedge
  repro on it running -> /tmp/wedge_probe.log; read the [UWT] decision
  (outerMost? which ctx? complete flags?) to pin the VM bug.
ALSO both platforms affected: the x86 box hit the same carpet natively.

Earlier session state follows.

OPEN — live-caught wedge window (catalog #3, ~09:35-09:44): 
NoUnusedVariablesLeftTest>>testNoUnusedTemporaryVariablesLeft (image-wide
scan; stock 8.5s, OURS >120s = the reflective perf gap) timed out at 80s,
then 7 subsequent trivial tests (NonInteractiveTranscript x4,
OSEnvironment x3) EACH timed out at 80s while the VM spun 100% CPU in
JIT/scavenge work; recovered on its own at ~09:44:45.  FALSIFIED so far:
slow terminate of the deep scan stack (instant at 5s and 80s depth in
bare forks).  Suspects: SUnit env machinery (runCase wrapping/
ProcessMonitor), timer-subsystem death window not caught by
[DELAY-DEATH] (only 1 firing all run, early + recovered).  NEXT: after
catalog frees /tmp, rerun wedge zone (NoUnusedVariablesLeft +
NonInteractiveTranscript + OSEnvironment) via /tmp/sunit_class_names.txt
with PHARO_DELAY_DEBUG=1 and watch the window live.

Prev shutdown state follows.

---

# WIP — SHUTDOWN STATE 2026-07-06 ~02:00 (all work committed + pushed, HEAD a53318c5)

Session arc COMPLETE — three debugging hunts finished, all validated:

1. **prim-100 simulation cascade** (e40cd65b) — pk-24 arity aliasing into
   the W3 IntArithReturn inline; stepping family 156 P / 0 F.
2. **WKD testClearing** (8cfee28c) + **TFFI idle-band starvation**
   (3940b62c) + **Zn socket hunt** (7478887d + ce4419af): MPSC-unsafe
   external-semaphore ring (lost wakeups), param-49 >65535 failure
   killing server processes via ProcessMonitorTestService suspension,
   socketError-on-stale; ring adversarially hardened (f7ed6703:
   ABA-proof counters, longjmp-safe tail, VM-thread overflow).
   ZnServerTest 22-28/31-every-run -> 31/31 x 10 consecutive.
3. **StDebugger delay-ticker wedge** (runner submodule 0316143, parent
   7eb962bd + a53318c5) — SUnit watchDogLoop desync passes nil into
   waitTimeoutMilliseconds:, nil-duration Delay kills the ticker
   (nil*1000 MNU), ProcessMonitorTestService suspends it, whole catalog
   sections stall.  Fixed via hardened watchDogLoop in the runner prep;
   validated: wedge zone 823 P / 0 timeouts / 0 ticker deaths (was
   5-101 deaths every run).  Full chain in deferred.md +
   docs/image_issues.md + memory stdebugger-ticker-death-wedge.md.

Also this session: stencil extraction had been SILENTLY BROKEN since
Jun-2 (fprintf in a stencil body) — fixed, regenerated, CMake-wired as a
hard build dependency (5accaddc + 4d97b675); IC_HIT arity gates; 215
debug knobs migrated to debug_vars.h (ratchet 250 -> 31).

**HARNESS STATE (for next session):**
- /tmp/harness/Pharo.image = Jul-4 prepped runner image + the hardened
  watchDogLoop guard baked in (Jul-6 01:47).  Pre-guard backup:
  /tmp/harness/Pharo-jul4-preguard.image.  Pristine eval image:
  /tmp/harness/Pharo.image.bak.
- The STOCK Cog VM (/tmp/harness/pharo) currently HANGS on any eval on
  this machine (worked Jul 4; environmental, undiagnosed) — preps were
  done with OUR VM instead (guard-only fileIn + prim-97 snapshot; the
  FULL runner re-fileIn on our VM aborts nondeterministically — bake
  single chunks; verify preps BEHAVIORALLY, never via sourceCode
  without the .changes file).
- Catalog detail files in /tmp: catalog2_part1_detail.txt +
  batch_{A,B,C,D}_detail.txt (98.78% composite, runs 19-22).

**QUEUED NEXT:** activation-wall perf project (harness + quiet-machine
baseline in scripts/perf-activation/, ablation order in its README);
remaining deferred items are Windows-side.  Optional: rerun the full
catalog with the hardened runner (expect the 3 batch-A wedge timeouts
back + steadier StDebugger family, marginal % change).

---

# WIP — catalog after the Zn socket hunt: 98.78% (2026-07-05 night)

**Full catalog #2 (all 2026-07-05 fixes): 27,859 verdicts — 27,519 PASS
= 98.78%, 199 non-pass (35 F / 72 E / 92 T) + 141 skips.**
Progression: July-4 ~743 non-pass (97.35%) -> morning fix wave 295
(98.44%) -> socket/ring fixes **199 (98.78%)** — 33% further cut, 73%
total from July 4.  Zn family: 460 P / 1 F (the network GeoIP test);
Zdc 80/80; SocketStream 22/22; TKT 107/1.  Remaining non-pass families
are exactly the known environmental set: Release/Cly/Renraku/Ring2/Rub
cold-context timeouts (runner caps at 5/class), LibTTY, Roassal/Cairo,
Spec2/StDebugger GUI env, FL WideString, OSEnvironment/
NonInteractiveTranscript, ZnHTTPS (no TLS).
Method note: the tail (Sp*..Z*) ran as FOUR fresh-VM batches to contain
the StDebuggerActionModelTest Delay-ticker-death wedge (see deferred.md
— pre-existing, bisected against the pre-afternoon binary, poisons
everything downstream when the death-loop recovery doesn't stick; batch
A absorbed it with only 3 timeouts).  Detail files:
/tmp/catalog2_part1_detail.txt + /tmp/batch_{A,B,C,D}_detail.txt
(runs 19-22).

---

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
- **Full-catalog 2026-07-05 COMPLETE (two-part run, all fixes in the
  binary): 27,716 verdicts, 27,283 PASS = 98.44%**, 295 non-pass
  (40 F / 125 E / 130 T) + 138 skips — vs July-4's ~743 non-passes,
  a ~60% reduction.  Every remaining family is environmental/known:
  Spec2 + StDebugger (GUI env, no fake-GUI in catalog prep), Zn/Zdc
  (network + the flaky local-server family below), Rub*/Renraku/
  Ring2/Release cold-context timeouts (runner caps at 5/class),
  LibTTY, FL WideString, Cly async, Roassal/Cairo, OSEnvironment/
  NonInteractiveTranscript, VariableBreakpointTest (8).  All
  previously-failing stepping/simulation families PASS in catalog
  context.  Detail preserved: /tmp/catalog_20260705_partial_detail.txt
  + /tmp/catalog_20260705_tail_detail.txt.
- **Zn socket-timing hunt COMPLETE 2026-07-05 (commits 7478887d +
  ce4419af): ZnServerTest 22-28/31-every-run -> 31/31 x 10 consecutive.**
  THREE root causes, none of them "timing" in the suspected sense:
  1. `signalExternalSemaphore`'s ring was MULTI-PRODUCER-UNSAFE
     (load/store/store head — single-producer only, but socket I/O
     thread + per-lookup DNS threads + TFFI workers all produce):
     concurrent producers overwrote each other's slot = silently lost
     semaphore wakeups at any occupancy; full ring also dropped
     silently.  Fixed: MPSC CAS-reserved slots, value-as-publish-flag,
     loud bounded-retry.  The Delay machinery was PROVEN INNOCENT
     (stalled DelayWaitTimeout sat correctly in the heap, ticker armed
     1s idles throughout).
  2. `vmParameterAt: 49 put:` (maxExternalSemaphores) FAILED above
     65535 and wrongly cloned the image-owned ExternalObjectsArray —
     the image's table-doubling past 64k raised 'Not enough space for
     external objects' inside a background Zn SERVER process, which
     SUnit's ProcessMonitorTestService then SUSPENDED (dead server ->
     client stall -> TestTookTooMuchTime shadowing the real error).
     Now pure bookkeeping (stock semantics; our ring is index-agnostic).
  3. sp_primitiveSocketError failed on stale handles from error-
     REPORTING paths ('Cannot access socket error code' replacing the
     real error).  Now never fails.  Plus: accept() now wakes the IO
     thread after resetting the listener (throughput was capped ~10/s
     by the 100ms select tick) and the accept-FAIL path resets the
     promoted listener (was left stuck CONNECTED = permanently deaf
     server, latent).
  Hunt method that cracked it: Error>>sunitAnnounce:toResult: override
  captured the ORIGINAL in-suite exceptions (all TestTookTooMuchTime),
  signalerContext stack walks located the stalls, PHARO_DNS_TRACE +
  PHARO_DELAY_DEBUG exonerated resolver+ticker, the unfiltered
  process-table dump at kill exposed ProcessMonitorTestService
  suspensions, and overriding handleUnhandledException: named the
  hidden errors.  Verification: Zn/Zdc family all green (ZnClient
  49/50 = network GeoIP only; SocketStreamTest 24/24 vs July-4's
  22/24); 22-class scheduler batch 1482 P / 0 F / 0 E.

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
