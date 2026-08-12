# ARM (macOS) — post-Windows-merge verification — archived

> Archived 2026-08-12 from `docs/deferred.md` (lines 1055-1295).
> Closed changelog: three items, all `[x]`, all re-confirmed green in the
2026-08-11 full suite. ~157 of these lines are explicitly-superseded debugging
narrative kept for the falsification trail.

---

  wall project); porpoise = upstream test-design GC race (both-VM
  empirical signature); p3 = Linux-only flavor (resolver latency —
  verify on next box); myprecious x2 multi-roundtrip = perf-composite
  (Fuel serialize; passes on x86 box).  restoreforpharo: FIXED
  (a99eee86, two-way become C++ stack swap) — SSWReStoreAggregateQuery
  15/15, was the 11-TIMEOUT family.

## ARM (macOS) — post-Windows-merge verification 2026-07-04

- [x] **Delay-ticker DEATH in Spec2/StDebugger catalog context — ROOT
  CAUSE FOUND + FIXED 2026-07-06 (runner submodule 0316143).**  Seven-
  probe hunt (probe images baked via prim-97 snapshot; VM-side P80
  suspend trap; prim-242 disarm trace):
  1. The [DELAY-DEATH]s are the DelayMicrosecondTicker dying on
     MessageNotUnderstood: `nil * 1000` inside tickAfterMilliseconds: —
     a Delay with a NIL duration reached scheduling.  The
     DelayBasicScheduler loop PASSES errors, so the UnhandledError hit
     SUnit's ProcessMonitorTestService, which SUSPENDED the P80 ticker
     (the same suspend-background-failures mechanism from the Zn hunt).
  2. The nil delay comes from SUnit's own watchdog:
     TestExecutionEnvironment>>watchDogLoop passes
     `maxTimeForTest asMilliSeconds` into waitTimeoutMilliseconds:.
     The watchdog two-signal/two-wait protocol DESYNCS (probe: the
     trapped watchdog's env `self == false` vs the live env; completed
     already true at the wake), and a spurious wake on a deactivated/
     between-tests environment reads maxTimeForTest = nil.  Every
     image-side writer was exonerated by probes (assignment always got
     a valid Duration; the setter never saw nil): the nil is the
     post-lifecycle state of a STALE env read by a desynced watchdog.
  3. Interp-only does NOT wedge (timing-dependent; our scheduler timing
     surfaces the image-protocol race).  Bisected pre-existing (fires
     identically on the pre-afternoon Jul-5 binary).
  FIX: hardened watchDogLoop in the runner prep
  (scripts/pharo-headless-test/run_sunit_tests.st): a nil limit
  re-parks on the semaphore instead of building the poison delay.
  Baked into the harness image (guard-only fileIn + snapshot; NOTE the
  full runner re-fileIn on our VM aborts nondeterministically mid-file
  and a post-snapshot image resumes its snapshot continuation on first
  eval — verify preps BEHAVIORALLY, and beware `sourceCode`-based
  checks: without the .changes file, decompiled source loses temp
  names).  VERIFIED: batch A (Sp*..StDebugger*, the wedge zone)
  823 P / 0 timeouts / 0 DELAY-DEATHs, twice (both guard variants);
  was 5-101 deaths + timeout carpet on every previous run.
  UPSTREAM-WORTHY: the watchdog nil-guard + a nil-argument guard in
  waitTimeoutMilliseconds: + making the Delay scheduler loop survive
  per-delay errors (see docs/image_issues.md).
- [x] **ProcessTest>>testTerminateInTerminate / prim-100 simulation cascade —
  FIXED 2026-07-05, commit e40cd65b.**  ROOT CAUSE (three conspiring
  defects, found via an IC-site dump at the DNU-cascade point — full
  hunt ledger in scripts/cascade-hunt/README.md rounds 1-11):
  (1) inlinePrimKind classified prim 111 as pk-24 (#class inline) with
  no arity check, so `Context>>objectClass:` — the 1-ARG mirror form of
  prim 111 — got 0-arg #class extras at its send site inside
  send:to:with:super:;  (2) the W3 IntArithReturn dispatch was a
  single-bit tbnz on extras bit 52 at 1-arg sites — unsound since
  primKinds 16-31 all set bit 52 (same B6-F1 unsoundness fixed earlier
  for W6/bit-51) — so pk-24 was stolen as "W3 kind 0" (tagged add);
  (3) the W3/W2 both-SmI check OR-combined the operand tags,
  accepting (heap, SmI) pairs, so the stolen dispatch computed
  lookupClass = contextOop + 44*8 - 1 — a fabricated young pointer that
  dangled after the next scavenge and cascaded in doesNotUnderstand:.
  FIXES: arity-aware inlinePrimKind(prim, methodNumArgs) at all 4
  classify sites; W3 dispatch decodes the full 5-bit field (16..18);
  strict per-operand SmI tag checks in W3+W2 emits.  VERIFIED: both
  repros pass (steps=99999, 0 cascades); stepping family (StepOver/
  StepInto/StepThrough/ContextTest/BlockClosureTest/ProcessTest)
  156 P / 0 F / 0 E — recovers the ~50-test cascade family including
  testTerminateInTerminate, testRunSimulated, testTallyInstructions,
  testTallyMethods, testBlockCannotReturn; 12-class arith/kernel canary
  1353 P / 0 F.  Kept from the hunt (real bugs fixed en route):
  scanStackReplace→forEachRoot delegation, JITState.prevState GC chain,
  pk-15 at:put: + IC_HIT setter write barriers, pk-24 forwarder guard.
  Historical investigation log follows (superseded but kept for the
  falsification methodology):
  ROOT SHARPENED (2026-07-04 evening): stepping the terminator
  (`terminator step` loop) dies in a doesNotUnderstand: CASCADE inside
  #send:super:numArgs: (prim 100, the simulation send) at fd=10 — the
  simulated execution hands prim 100 a receiver so corrupt that even
  DNU can't dispatch.  The SAME cascade appears in the ContextTest
  runner batch and (by symptom) underlies the whole cold-context
  stepping family — INCLUDING on the Jun-25 baseline binary (its
  StepOver family scores 0P/5T in identical context).  So this is a
  LONG-STANDING ARM simulation-machinery bug (prim 100 + JIT-warm
  frames?), pre-dating the Windows merge; the merge only shifted which
  tests trip it.  Repro: the terminator-step loop from
  testTerminateInTerminate in a bare eval → [DNU] CASCADE
  caller=#send:super:numArgs:.  Next step: lldb on prim 100's dispatch
  with that repro; check receiver provenance in the simulated frame.
  INVESTIGATION LOG (2026-07-05, hypotheses tested & falsified):
  - Interp-only (PHARO_NO_JIT): repro fully PASSES (reaches the target
    selector) — strictly a JIT-path leak.
  - Cascade forensics: the object handed #lookupSelector: (the "class"
    from Context>>send:to:with:super:'s objectClass:) is a CORPSE —
    run A: valid-pointer with header nil-scrubbed (reclaimed memory),
    run B: INVALID-PTR entirely.  classIdx=0 both times.
  - scanStackReplace (become's VM-side fixup) HAD drifted from
    forEachRoot coverage (missed j2jPool receivers/closures,
    bvClosureSaveStack_, sista save pool) — FIXED by delegating to
    forEachRoot (commit this session).  Cascade persists -> not (only)
    that.
  - sweepClassTable forensics (PHARO_GC_EPH_DEBUG): NO classes swept in
    the repro — not a dead-classTable-entry.
  - pk-24 stencil #class fast path: forwarder guard added (idx<=8 bails
    to generic send).  Guard never fires in the repro — the corpse does
    NOT come from the stencil pk-24 path.
  - Class inlines (PHARO_T1_NO_INLINE_CLASS + _IMM_CLASS): cascade
    persists — the asm tryPrimClass twin is ALSO exonerated (its
    IC-guard invariant claim stands).
  - Getter/setter inlines (PHARO_T1_NO_INLINE_GETTER/_SETTER): cascade
    persists.  Combined with the earlier PHARO_NO_J2J /
    PHARO_T1_NO_INLINE_J2J results, every cheap emit-knob is falsified.
  - Provenance forensics (2026-07-05 late, all landed in-tree): the
    corpse is the lookupClass ARGUMENT (operand slots @46/@50, live
    frames = the classic step->send:super:numArgs: chain).  Region
    classification: YOUNG new-space, nil-scrubbed (post-scavenge eden
    recycling) in some runs, INVALID-PTR (outside heap) in others —
    one mechanism, read at different eden lifecycle points.  prim-111
    ring (32-deep): the corpse was NOT produced by prim 111 => the
    super-branch (method-literal association) or a >32-window fetch.
    PHARO_CORPSE_PUSH_TRAP (interp push() tripwire) never fires =>
    the writer is JIT-STENCIL-side (raw s->sp writes bypass push()).
    DET_SCHED does NOT stabilize the corpse address (ASLR + residual
    async), so cross-process watchpoints are out — must be in-process.
  - BREAKTHROUGH (2026-07-05 03:20): the mechanism is the KNOWN
    2026-06-10 IP-ROUND-TRIP family (see the comment in
    ObjectMemory::scavenge): a scavenge MOVES young CompiledMethods and
    raw bytecode POINTERS keep executing the stale eden copy until eden
    refills with new objects — then execution reads recycled bytes
    ("wrong-receiver-DNU family, ~10%/layout").  The 06-10 fix wrapped
    the ACTIVE interpreter ip (prepareForGC offset round-trip).  THE
    RESIDUE: processes SUSPENDED MID-JIT (the repro's terminatee parks
    inside [[suspend] ensure:], a YOUNG CompiledBlock) keep their parked
    JIT resume-IPs as raw eden pointers; the stepping machinery later
    resumes them onto recycled eden.  Fits every falsification: interp
    -only clean (suspended interp state = heap contexts with SmInt pcs),
    both binaries affected, corpse = recycled-eden flavored, no barrier/
    root involvement (write barriers were red herrings — scavenge full-
    scans old space anyway; remembered set is populated but unused).
    Two barrier hardenings landed anyway (pk-15 at:put:, IC_HIT setter).
    FIX DIRECTION: prepareForGC must round-trip bytecode-derived ips of
    ALL parked JIT chains (suspended processes' JITStates), not just the
    active frame — or scavenge must pin/tenure methods referenced by
    parked chains.  Find where suspended-process JIT state stores its
    resume ip (JITState.ip / j2j save layout / process-switch park path).
  - ROUND 3 (2026-07-05 04:00): quarantine bisect — PHARO_SCAV_QUARANTINE_AT
    N<=8 makes the FULL repro PASS (steps=99999, zero cascades); N>=9 does
    not.  Confirms eden-reuse-after-scavenge-~8 is load-bearing.  Falsified
    in addition: PHARO_NO_OSR, PHARO_NO_SISTA (each still cascades) — with
    PHARO_NO_JIT passing, the vehicle is BASE-T1 machinery.  Landed a real
    structural GC bug fix en route: parked OUTER JITStates (nested
    tryJITActivation) were INVISIBLE to the GC — new JITState.prevState
    chain + forEachRoot/prepareForGC/afterGC now walk the whole chain
    (method/receiver/closure roots, per-state ip/literals round-trip,
    per-state Sista pools).  Necessary but NOT sufficient — cascade
    persists.
  - KEY RUNBOOK UPGRADE: the cascade is NON-FATAL (recovers via
    transferTo) and RECURS within one process — so lldb can break at the
    first [DNU] CASCADE fprintf, read the corpse slot addresses from the
    dump (stack@46/@50-style), set hardware watchpoints on those slots,
    CONTINUE, and catch the writer at the next occurrence IN THE SAME
    RUN.  No cross-run address stability needed (that was the blocker:
    ASLR-off lldb still varies heap layout).
  - ROUND 4 (2026-07-05 ~04:40): PHARO_EDEN_POISON (new knob, retired
    eden filled with 0x5CAFED sentinel at reset): the corpse stays a
    VALID EDEN ADDRESS, never the sentinel — the stale POINTER is held
    outside eden across the scavenge (not fabricated from recycled
    storage), and its target is now interior to a fresh allocation
    (hdr reads as nil-fill => classIdx 0).  Perm space IS covered by the
    scavenge old->young full scan (scanRegionForYoung old + perm) — perm
    holder ruled out.  Same-run watchpoint attempt: the batch-lldb
    pipeline WORKS end to end (pharo_cascade_bp anchor fn passes
    corpse/stackBase/sp in x0-x2; python arms hw watchpoints on the
    corpse slots) BUT the first cascade kills the eval process and the
    idle loop never rewrites those high stack slots — arm-at-cascade is
    too late.  Refined recipe for the next session: modify the repro to
    RESTART the scenario after the first cascade (loop the whole
    fork/step scenario, swallowing errors) so the armed watchpoints
    catch iteration 2's writes; expect hot-slot noise — use
    `watchpoint modify -c` with an eden-range value condition, or
    accept ~dozens of stops and scan bt's for non-interp writers.
    Scripts: scratchpad cascade_watch.py + watch-driver.lldb (recreate
    from this entry if the scratchpad is gone; the anchor fn
    pharo_cascade_bp is committed in Interpreter.cpp).
  - NEXT (superseded): lldb + PHARO_DET_SCHED on the repro; suspects remaining:
    the T1 dispatch-A tryPrimClass EMITTED-ASM twin (AsmjitT1 ~9526,
    "no bounds check needed" — same missing forwarder handling), the
    JIT dispatch classIndex fetch handing a stale value through IC
    machinery, or a JIT-native-frame receiver slot that become cannot
    see (JITState/native stack, unreachable from forEachRoot).
    Breakpoint recipe: the [DNU] CASCADE fprintf in Interpreter.cpp
    (search "CASCADE rcvr="), then inspect the simulated context chain
    for who computed the class value.
  Old suspects (prim-196 pc-kill / cannotReturn-to-returning-ctx) are
  ruled out.  NOT a net regression: the Jun-25 baseline binary in the
  same context scores ProcessTest 37P/5T (testResumeAfterBCR,
  testSchedulingHigherPriorityServedFirst + 3 more) vs HEAD 45P/1T — the
  Windows-range work fixed four ARM timeouts and introduced this one.
  Also noted for the ledger: the whole SIMULATION/stepping family
  (BlockClosureTest simulate/tally trio, ContextTest testBlockCannotReturn,
  StepOver/StepInto/StepThrough) times out in COLD small-batch runner
  context BYTE-IDENTICALLY on Jun-25 and HEAD binaries — context-dependent
  slowness/hang, not a merge regression; June's green kernel records ran
  these classes warm inside the full 565-class order.  Judge via the full
  catalog, not small batches.

- [x] **WeakKeyDictionaryTest>>testClearing warm-run deviation — FIXED
  2026-07-05, commit 8cfee28c.**  ROOT CAUSE: the f96cb69b one-shot at the
  JIT-activation early-return fired at activation EXIT — but
  tryJITActivation runs the whole method including the GC prim prologue,
  so when prim 130 executed INSIDE that activation the P50 wake landed
  mid-statement in the caller (after `Smalltalk garbageCollect` returned,
  before the pre-drain `dict size` read).  Interp never does this (a
  successful GC prim doesn't activate; its one-shot fires at the end of
  the NEXT activation).  FIX: snapshot the flag before tryJITActivation
  and fire at exit only when armed at ENTRY; arming inside the activation
  waits for the next activation, whose entry drain native-processes the
  WKA mourners first — the exit signal then only dispatches keeper
  mourners to P50 (gating it off entirely regressed WeakAnnouncerTest
  testNoDeadWeakSubscriptions — mourn starvation — so armed-at-entry is
  the reconciliation).  Plus: interpret()'s periodic
  signalFinalizationIfNeeded gated on !finalizeDeferred (parity with the
  step() twin).  VERIFIED: testClearing 6/6 warm; WeakAnnouncerTest 3x
  suite clean; weak/finalization batch 1003 P / 0 F; canary 1353 P / 0 F.
  Original entry follows:
  Repro: `3 timesRepeat: [(WeakKeyDictionaryTest run: #testClearing)]`
  in one VM — runs 1-2 pass, run 3 fails "Got 1 instead of 1001" (the
  size assert before/right after the explicit garbageCollect sees the
  1000 weak entries ALREADY finalized).  Also fails in batch context when
  WKD follows GC-heavy classes (12-class weak/GC batch).  Evidence chain:
  stock Cog 3/3; Jun-25 ARM baseline 3/3; HEAD PHARO_NO_JIT 3/3; HEAD
  default fails run 3; PHARO_NO_J2J / PHARO_T1_NO_INLINE_J2J do NOT cure
  (not the J2J save-pool liveness family).  Mechanism: an INCIDENTAL
  ephemeron-firing GC lands inside the test's keys:=nil → at:put: →
  asserts window on warm heaps, and the deferred finalization signal
  (f96cb69b fires the one-shot at every JIT activation) wakes the pri-50
  mourner inside the window, draining the 1000 slots before the assert
  reads size.  The interpreter path deliberately preserves the invariant
  (see the activateMethod comment: "dict size is prim 264 (quick)...");
  stock's signal empirically lands after the window (same 79/50
  finalizer priorities).  Whether the in-window GC happens is
  allocation-volume threshold luck — chasing the commit that shifted
