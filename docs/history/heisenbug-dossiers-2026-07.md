# Heisenbug dossiers (weak refs, preemption/materialization, catalog triage) — archived

> Archived 2026-08-12 from `docs/deferred.md` (lines 251-427).
> ARCHIVED NARRATIVE — read the resolutions first.

* The weak-reference dossier is CLOSED: `88ce3fee` (executeFromContext disowned
  the context it restored) and `da9159e9` (a Context's dead stack residue was a
  GC root). Its "ROOT CAUSE FOUND: the JIT's stores never landed" is WRONG — no
  JIT emitter bug ever existed.
* The 2026-07-07 PREEMPTION/MATERIALIZATION entry is STILL OPEN and is the same
  defect as the WarpBlt expression-stack displacement now tracked as bug #1 in
  `docs/vm-compat-bugs.md`. Its code-path trilemma is the best lead in this
  file and has been carried over there.
* "Exhaustive triage came up EMPTY" and "our VM matches stock on weak-clearing"
  were both falsified later.

The imperative-present "NEXT STEP" blocks below are historical. Do not action
them from here.

---

came from a window when this Mac was concurrently driving AWS builds; two
failures in ten is not a rate.  Nil-ing the slots above stackp in all three
context-reuse paths is a real stale-root cleanup but is NOT needed for this and
costs a tail sweep per materialisation, so it is not shipped.

Original dossier follows.

## (dossier) weak references not cleared under the JIT — root-caused above

`WeakOrderedCollectionTest`'s two garbage-collected tests fail
DETERMINISTICALLY under our VM (10/10 runs) and pass deterministically on
stock Cog (3/3), in-suite under `scripts/pharo-headless-test`.  Earlier
sessions recorded these as "flaky"; they are not.  `PHARO_NO_JIT=1` makes
them pass, so the JIT keeps the weak collection's referents alive across
three `Smalltalk garbageCollect` calls.

Ruled out: Sista (`PHARO_NO_SISTA`, `PHARO_NO_SISTA_PER_BC`), the inline
eden allocation (`PHARO_T1_NO_INLINE_NEW_ASM`, `PHARO_T1_NO_EDEN_NEW`),
generational clones, the J2J save-pool GC roots (skipping the walk
entirely still fails), and nil-ing context slots above `stackp` on the
frame0-reuse path.  `PHARO_NO_FRAME0_REUSE=1` is unusable as a bisect —
it hangs.

Reproduces ONLY under our runner: a direct 40x loop (JIT-warmed), a fork
at `userBackgroundPriority`, and SUnit's own `TestCase>>run` all pass.
Adding two diagnostic statements to the test body makes it pass — the
dead-slot-residue signature.

ROOT CAUSE FOUND (2026-08-11): **the JIT frame is stale, not the context.**
`PHARO_WEAK_SURVIVOR_PATHS` names the holder as the test method's OWN context
at indexed slot 6 = temp 0 = `anArray`, the variable the test nils; the context
is reachable because `3 timesRepeat: [...]` is a real send whose closure holds
it as outerContext.  `PHARO_TRACE_FRAME_TEMPS` then shows the C++ frame slots
`materializeFrameStack` reads still hold `t0=OrderedCollection t1=Time` at the
moment it reads them — so the JIT's `anArray := nil` stores never landed in
`savedFP + 1 + t`.  `PHARO_MAT_FULL_RESYNC=1` is inert (same stale slots) and
`PHARO_NO_JIT=1` passes (interpreter writes where the materializer reads).

THIS IS BIGGER THAN WEAK REFS: any materialization of a JIT frame (preemption,
block creation, thisContext) can capture pre-store temp values, and a resumed
frame continues from them.  Very likely the same root as the SlotIntegration
materialization Heisenbug above ("a live temp comes back with the wrong VALUE
after the build is preempted and its frames are materialized to a context and
restored").

NEXT: instrument the frame push to record the tempBase in effect and compare it
against `savedFP + 1`.  Note two conventions coexist —
`Interpreter.cpp:17189` uses `state->tempBase = callerSP - nArgs` (Sista
self-rec inline; pushes no SavedFrame, so not itself the culprit).

Earlier narrowing, kept because it rules things out:
THE PIN IS TRANSITIVE, NOT A ROOT.  New tool `PHARO_WATCH_ROOT_CLASS=<Class>`
makes forEachRoot report which root category visits each instance of that
class (categories: vm-registers, nlr-saved-states, world-renderer,
operand-stack, saved-frames, method-cache, jit-code-zone, jit-count-map,
jit-state, j2j-save-pool, bv-closure-stack, sista-save-pool).  Over a whole
suite run: ZERO visits for Duration and ZERO for OrderedCollection, with the
probe verified working (14 Process visits via nlr-saved-states in the same
run).  So no root-category over-scan is responsible; some rooted heap object
REACHES them.  Next step: parent provenance during the mark phase, to walk a
surviving referent back to its root.

Also ruled out and NOT committed: nil-ing the slots above stackp in all
THREE of materializeFrameStack's context-reuse paths (frame.materializedContext,
currentFrameMaterializedCtx_, and the frame[0]==activeContext_ re-sync).  None
of them nils the tail today, so a frame re-materialized at a shallower depth
does leave a deeper snapshot's operands in a process-reachable heap object —
a real stale-root bug worth fixing on its own evidence — but fixing all three
does not fix these tests (5 runs, 0 P / 2 F each).

  (2026-08-11 postscript: that reading was right about the stale residue and
  wrong about the remedy.  Nil-ing at the sync sites does not reach residue
  written afterwards; what fixes it is not tracing above stackp at all, the
  way Spur does — `da9159e9`.  The `[HEAP-CHAIN]` probe added there is the
  "parent provenance during the mark phase" this section asked for, and it
  now names the terminating ROOT category too.)

Very likely the same defect as porpoise's
`PropertyManagerTest>>testPropertyManagerValueWeakness`, the one finding
the x86 package sweep saw correlate across suites.

Full evidence + repro recipe: `docs/aws-followup-2026-08-11.md` section 3.

## BREAKTHROUGH: the Heisenbug cluster is a PREEMPTION/MATERIALIZATION bug (2026-07-07)

SlotIntegration is a SCHEDULING Heisenbug, and DET_SCHED cracks it open:
- PHARO_DET_SCHED=1 => DETERMINISTIC FAIL (result '2'), and — the whole
  point of the tool — it reproduces WITH instrumentation attached
  (PHARO_TRACE_BECOME confirms: only the `#y` class-become runs, the `#z`
  become is skipped, deterministically observable).
- PHARO_DET_SCHED_QUANTUM bisect: q=1 FAILS, q>=2 PASS.  So a periodic
  checkpoint firing at the q=1 rate lands at a specific bytecode window in
  the class build and corrupts it; a coarser quantum misses that window.
- What healed it earlier was NOT allocation but TIMING: PHARO_PRIM_SEQ (a
  per-primitive ring-buffer write) shifted checkpoint timing and healed
  it, while per-become / per-scavenge traces did not.  That is the
  signature of a scheduling Heisenbug, not a memory one.
- ALL VM memory-integrity checkers are CLEAN during the deterministic
  failure (dangle / alloc-size / shadow-slots = 0).  So the corruption is
  LOGICAL, not memory: a live temp (the in-progress slot collection in
  `copyWith:`/`allSlots`) comes back with the wrong VALUE after the
  build is preempted and its frames are materialized to a context and
  restored.

=> ROOT CLASS: process-switch / materializeFrameStack temp handling when
   a preemption hits mid-operation — the SAME area as the ARM context
   storm (preemption during the debugger recursion) and the NLR/
   materialization work.  The Heisenbug cluster (SlotIntegration, storm,
   WeakOC/finalization flakes) very likely shares THIS root: our VM's
   frame materialization on preemption does not perfectly preserve an
   in-progress computation's temps/intermediate, so on resume the
   computation continues from a subtly wrong state.

RULED OUT via VM-level checks under the DET_SCHED-deterministic failure
(all CLEAN, bug still #(2 2 2)): context-capacity operand loss
([CTX-CAPACITY]/STATE-LOST = 0, stackp-clamp = 0 => materialization does
NOT drop operands); PC round-trip (save `(ip-bytes)+1`, restore
`bytes+(pc-1)` — symmetric, no off-by-one); + earlier: memory corruption
(dangle/alloc/shadow clean), simple-frame save/restore (inspected
correct).  So across the preemption the operands, PC, and memory are all
PRESERVED — yet the build still diverges to size 2.  That leaves ONE
suspect: the resumed build runs in MATERIALIZED-CONTEXT EXECUTION MODE
(frameDepth_=0, executeFromContext, senders restored lazily) — a
different, far-less-exercised interpreter path than inline frames — and
a specific bytecode in the copyWith:/allSlots/slots: sequence behaves
differently there than inline.  Same path as the ARM context storm and
the NLR/materialization work.  NEXT: run the copyWith:/allSlots build
ONCE inline (no preemption) and ONCE forced through materialized-context
execution (preempt-then-resume at the same point), and diff the bytecode
outcomes to find the bytecode that diverges in context mode.

CODE-PATH LOCALIZATION (2026-07-07, by inspection against the repro):
the defect is in the preemption save/restore of a DEEP frame stack.  On
preemption mid-build, materializeFrameStack (Interpreter.cpp:18301) saves
ALL live frames to a context chain (save path inspected — numTemps
`>>18 & 0x3F` matches the canonical decoders, temp + expr-stack saves
look correct for simple frames).  The build is then resumed via
executeFromContext (20103) which restores only the TOP context as a live
frame (fd=0); the SENDER frames (the copyWith/allSlots frames holding the
in-progress slot collection) stay materialized and are restored LAZILY
on return, in returnValue's fd==0 branch (6977+).  That return-into-
materialized-sender path has subtle stackp semantics (e.g. line ~7166:
`executeFromContext(homeSender); framePointer_[1 + hsOrigSp] = value` —
placing the return value relative to the restored operand stack), and is
the prime suspect for dropping the in-progress operand.  The bug is in
ONE of {materialize expr-stack extent for a mid-send frame, the
executeFromContext stackp restore, the returnValue fd==0 sender-restore
value placement} — NOT in a simple-frame path (those are exercised
constantly and work).

DETERMINISTIC REPRO (next session — no more flaky probing):
  gui.image (pkgbase + fake-GUI), /tmp/fail.st = warmup + instance-present
  double addInstVarNamed; PHARO_DET_SCHED=1 => '2' (fail), q>=2 => '3'.
  Under DET_SCHED you can attach ANY instrumentation without healing.
  NEXT: trace materializeFrameStack during the DET_SCHED q=1 failing
  build (it will be called mid-`copyWith:`/slot-build); diff the
  materialized context's temps against the live-frame values to find the
  temp that materialization mis-captures; fix the materialization of
  that operand/temp.  If found, it likely also fixes the storm.

## Catalog non-pass triage — deterministic-bug search (2026-07-07)

Exhaustive search for remaining DETERMINISTIC our-VM correctness bugs in
the 25 catalog non-passes came up EMPTY — every one is (a) an upstream
image bug (OCClassBuilder trait-composition, SystemDependencies
Reflectivity, rename — image_issues.md wishlist), (b) env/harness (the
StDebugger family: testDynamicVariableEvaluation, testIsInSelected-
ContextPackage, testUpdateLayoutForContexts — stock-with-same-prep fails
identically; ZnClient network), (c) perf, not correctness (reflective-
slowness timeouts — activation-wall project), (d) run-order pollution
that is stock-IDENTICAL (ReleaseTest testObsoleteClasses / testPackage-
Organizer / testUnknownProcesses / testNoLiteralIsPinnedInMemory), or
(e) a GC/become/heap-state HEISENBUG (SlotIntegration trait-add,
WeakOrderedCollection all-GC'd, EventAfterProceed).  Deterministic
counter-probes CONFIRMED our VM matches stock on: weak-clearing
(WeakArray/WeakOrderedCollection/keep-1), becomeForward completeness
(slot/Dictionary/IdentitySet/OrderedCollection, no GC needed), and
