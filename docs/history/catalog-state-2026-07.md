# Cross-platform catalog state (2026-07-06/07) — archived

> Archived 2026-08-12 from `docs/deferred.md` (lines 693-1054).
> Historical. The headline metrics here ("27,760 P / 99.92%", "FINAL") are the
July picture and are NOT current — see `docs/test-results.md` and
`docs/results/` for measurements next to their artifacts. Two verdicts in here
were later falsified and are called out in `docs/vm-compat-bugs.md`.

---

catalog.  Reproduction genuinely requires building the 200-pkg catalog image
(Roassal3+Spec2+~198 more via stock-Cog Metacello; our VM can't HTTPS) + a
long run.  This session started a Roassal3 load toward that — but stock Cog
SEGFAULTS loading Roassal3's Cairo FFI (primLoadSymbol:module: / primDefineFunctionWith:returnType:) in this headless macOS env, so the
full-Roassal catalog image CANNOT be built here.  Catalog runs must be done
on a properly-provisioned box (the docs/results catalog#7-10 environment).

## Cross-platform catalog state — FINAL 2026-07-07 (see WIP.md, docs/changes.md)

Catalog #10 (macOS/ARM, all 9 fixes, lean binary + low-space net):
**27,760 P / 16 F / 2 E / 7 T = 25 non-pass = 99.91%** — COMPLETE, no
storm.  Every non-pass is a known accepted-residual (see the per-family
list below + the storm dossier above); zero new regressions from the
2026-07-07 fix wave.  The candidate-queue PACKAGE fixes (soil/sauco/
illimani/methodproxies/redistick/myprecious/restore) are validated on
their own loaded images, not in this base-catalog number.

## Cross-platform catalog state — earlier (2026-07-06/07)

Catalog #8 (macOS/ARM, everything): **27,763 P / 15 F / 3 E / 4 T / 182 S
= 22 non-pass = 99.92%.**  Day arc: 199 → 113 → 84 → 59 → 33 → 22.
Box (Linux/x86) run #3: 99.54% with every platform family individually
re-verified fixed on-box.  Details: docs/results/catalog-2026-07-06/.
All 22 residuals classified by the 12-agent adversarial workflow;
superseded per-family notes from catalog #5 below, updated:

- [~] **Reflective-slowness TIMEOUTs** — image-wide scan/recompile tests
  genuinely >80s on our VM (stock ~8s — the 15-30x reflective gap):
  StSpotter x2, NoUnusedVariablesLeft-style scans x3.  Owner: the queued
  activation-wall perf project (scripts/perf-activation/).  Also owns
  lexicon/famixreplication/deeptraverser TIMEOUTs from the pkg sweep if
  the 2026-07-07 hunt confirms perf-gap.
  **PARTIAL 2026-07-08 (aad03bc0 VM + submodule e5fdf9d):** the two
  hottest inner scans of `allSendersOf:`/`allReferencesTo:` are now VM
  primitives — `CompiledCode>>refersToLiteral:` -> `primitiveRefersToLiteral`
  (literal-array walk, recurses into nested method/array literals) and
  `scanFor:` -> `primitiveScanForByte` (SistaV1 bytecode-length walk).  Both
  keep the EXACT Smalltalk fallback (fire only via the pragma; any type
  uncertainty fails back).  Inert in the VM until the runner-prep installs
  the pragmas (submodule run_sunit_tests.st).  Measured ~1.5-2x on
  senders/implementors scans (allSendersOf: ~2.7s -> ~1.4s; special
  selectors ~2x), and the win SCALES WITH METHOD COUNT (bigger in the
  200k-method catalog than the 138k-method harness).  Validated: 0
  mismatches over 1,319,322 refersToLiteral: comparisons + identical
  allSendersOf: results across 21 selectors; broad batch[1-150] 7876 tests
  Fail 0/Error 0 byte-identical to baseline.  RESIDUAL FLOOR is still the
  activation wall (per-send ~70ns arm64 vs Cog ~2ns) — the block-per-method
  outer `thoroughWhichMethodsReferTo:` loop stays interpreted, so the >80s
  timeouts (which scale with method count) are reduced, not eliminated.
  Closing them fully needs the activation-wall project (inlined method
  activation / body-inlining), not more primitives.
  **Why not a class-level scan primitive** (considered + rejected
  2026-07-08): the next lever is collapsing the outer `self methods select:
  [:m | m hasSelector: lit specialSelectorIndex: idx]` into one C++ call per
  CLASS (removing the block-per-method activation).  Rejected because
  `CompiledMethod>>hasSelector:specialSelectorIndex:` first consults the
  `#ffiNonCompiledMethod` property and, when present, answers from a
  SUBSTITUTE method (not the wrapper in `methodDict values`).  Replicating
  that per-method property semantics in C++ is a whole-image
  reflective-correctness landmine (silent divergence on FFI-callout methods)
  for shaving one already-classified residual timeout — the wrong trade.
  Collapsing per-method activation is the activation-wall project's literal
  job; do it there with the inlining infra, not a bespoke reflective primitive.
  **Full 565-class harness re-swept 2026-07-08 with the primitives ACTIVE
  (Pass ~12,700): 0 Fail, 1 Error (OCClassBuilderTest trait-composition —
  stock-Cog-identical upstream, image_issues.md:184), 1 Timeout (this item).
  Zero genuine unfixed VM bugs remain in the base harness.**
- [x] **Roassal RS* family** — FIXED 2026-07-06: 64-slot manual-surface
  registry exhaustion (growable deque, Primitives.cpp); the "rendering
  diffs" were downstream of failed surface allocation.
- [x] **Sp* tree/table adapters + FTTableMorph (~18)** — FIXED by the
  fake-GUI paced World-cycle loop (runner submodule c42cc6d).
- [x] **StDebugger residue** — classified env both-fail (stock with the
  same prep fails identically; IDE-context layout/dynamic-variable
  family).  Accepted residual.
- [x] **RBBrowserEnvironmentTest flake** — root-caused 2026-07-06: the
  runner's nil-protocol stuffed selector polluted environment scans
  (isolation controls on a pristine image had masked it); runner fixed.
- [x] **ReleaseTest hygiene** — split: harness-env parts fixed (orphan
  packages via BaselineOfSUnitHarness, protocol reclassifications,
  pharo.version write); rest is stock-identical run-order pollution
  (literal-pin, PackageOrganizer, in-suite testObsoleteClasses) +
  upstream image bugs now in docs/image_issues.md wishlist
  (SystemDependenciesTest UI-deps Reflectivity drift).
- [x] **Singles** — classified: ZnClientTest (network), TKT (upstream-
  flaky), SUnitTest watchdog self-test (our hardened watchDogLoop, by
  design), MorphicWindowManager taskbar (upstream MorphicNativeWindow
  hasProperty: — image_issues.md wishlist), GC catalog-context flakes
  (FinalizationRegistry, roving Trait timeouts — repro notes in WIP).
- [x] **jitpkg external packages — 200-package sweep COMPLETE 2026-07-07**
  (box #2, run-manifest.sh; summary + per-test fails archived in
  docs/results/catalog-2026-07-06/pkg200*): 157 packages at clean
  cog-parity, 29 load-failures (Metacello/network, not VM), aigraph
  176/176 (May bug conclusively dead), gitlab+bitbucket mock suites
  90-error family FIXED by the proxy-protocol VM work (cannotInterpret: +
  become classTable).
- [x] **SlotIntegrationTest>>testAddAndAddInstVarNamedWithTrait2 — FIXED
  2026-07-07 (b04a0015 gated -> default-on).**  Root cause (fully traced;
  the entire dossier below was the multi-session hunt, most of its earlier
  theories WRONG): the first add's instance-grow `becomeForward(old@X 1-slot
  -> new@Y 2-slot)` is scan-and-replace (allObjectsDo heap + scanStackReplace
  /forEachRoot roots) and MISSES a reference to old@X that GC's mark keeps
  alive (a JIT operand under materialization) — so old@X stays a STALE VALID
  instance.  The second add's `allInstances` re-finds old@X and migrates it
  at ivar 2 -> slotCount 1 < class instSize 2 -> prim 73 fails ->
  `SubscriptOutOfBounds: 0 in object:instVarAt:` -> the rebuild aborts before
  its class-install become -> the new slot is silently dropped.  Needs BOTH
  JIT and materialization (NO_JIT / NO_J2J-independent: base JIT execution +
  preempt).  FIX: `becomeForward` now leaves obj1 as a forwarder to obj2
  (setClassIndex 8, slot0=obj2) after the scan — any scan-missed ref resolves
  via followForwarded and `allInstances` (skips isForwarded) stops re-finding
  the husk.  Validated: SlotIntegrationTest 17/17; slot/become batch (220
  tests) 1 fixed / 0 regressed; broad batch[1-150] (7876 tests) byte-identical
  results.  Opt-out: PHARO_NO_BECOME_FORWARDER.  Deterministic oracle for
  regressions: PHARO_MAT_AT_CHECKPOINT=3 on /tmp/fail2.st (scratchpad/fixrig).
  NOTE: the ARM context-storm and TF-callback tail hang were hypothesized to
  share this root — re-test them against this fix.
- [ ] ~~SlotIntegrationTest (old dossier, kept for method/evidence)~~ —
  pre-existing (fails standalone at ffca1841, before the 2026-07-07 fix
  wave); 18-probe dossier from 2026-07-07 (scripts in the session
  scratchpad /tmp/slotprobe*.st, gui.image = pkgbase + fake-GUI bake):
  - MINIMAL REPRO (variant C): make class with trait TOne + slot x;
    `c new one. c addInstVarNamed: #y. c new one. c addInstVarNamed: #z`
    -> #z SILENTLY dropped (ivars stay #(x y), env copy identical, no
    error, no announcement-subscriber change).  BOTH trait-method sends
    are required (A: send-before-first-add only, B: between only,
    D: `new` without the send — all pass).  Stock: passes all variants.
  - The add's inputs are correct (localSlots + copyWith: produce x y z);
    the no-op is the installer taking its no-changes path (inner
    ShNoChangesInClass handler in ShiftClassInstaller>>make — outer
    handlers can't see it).
  - HEAL MATRIX: retrying the same add works; `Smalltalk garbageCollect`
    heals; `garbageCollectMost` (scavenge) heals; recompiling
    compareWithOldClass (any image recompile?) heals.  NOT prevented by
    PHARO_NO_JIT=1 nor PHARO_NO_METHOD_CACHE=1 — poison is VM state
    outside both, cleared by ANY GC pass.
  - Ruled out: traitUsers weak-set staleness (1 clean user, == c),
    metaclass-class identity (TraitedMetaclass checks hold), builder
    enhancer selection (TraitBuilderEnhancer engaged when instrumented),
    subscriptions (51->51).
  - '2 ran' doubling = body TestFailure + tearDown error double-count,
    secondary.
  - NEXT: VM-side — trace which become prims (72/128/249) each of the
    two rebuilds calls and diff interpreter/memory state across a
    healing scavenge inside the poisoned window (candidates: remembered
    set, purge/rekey passes, forwarding remnants readable pre-GC).
  - DETERMINISTIC REPRO + TIGHT MECHANISM (2026-07-07, major refinement):
    the trigger is a LIVE INSTANCE of the class between the two adds.
    5x-loop or a single `inst := c new` before the `#z` add => the `#z`
    rebuild is SILENTLY SKIPPED (traced via PHARO_TRACE_BECOME: the
    passing no-instance case does the `#z` class+metaclass becomes; the
    failing instance-present case does ZERO becomes for `#z`).  The skip
    is `ShNoChangesInClass` inside ShiftClassInstaller>>make: the
    ShSlotChangeDetector compares oldClass allSlots vs builder allSlots
    (ShAbstractClassChangeDetector>>compareVariables:with: — `a size = b
    size` then per-slot hasSameDefinitionAs:) and wrongly returns "equal"
    => #() changes => no rebuild.  Pre-Z state is BYTE-IDENTICAL to stock
    (allSlots=2, allInstances=2, inst class==c, not obsolete); the
    divergence is entirely inside the instance-present `#z` execution.
  - RULED OUT (deterministic, both VMs stock-identical): weak-clearing,
    becomeForward completeness (slot/Dict/IdentitySet/OrderedColl, no GC
    needed), identityHash + slot-key stability + Set membership,
    DynamicVariable, JIT (PHARO_NO_JIT), method cache
    (PHARO_NO_METHOD_CACHE), and SCAVENGE-avoidance (huge
    PHARO_NEWSPACE_MB=512 does NOT fix it).  Healed ONLY by: a full/
    scavenge GC before the `#z` add (collects the instances), or ANY
    image-side recompile of the comparer (triggers a GC).  So: live
    instances of the class corrupt some state that the slot-comparer
    reads, and any GC clears it — but NOT the method cache/JIT/young-size.
  - DEFINITIVE (2026-07-07, VM-level PHARO_TRACE_BECOME — the ONLY faithful
    observer, since it logs via fprintf without allocating image memory
    so it cannot trigger the heal-GC): the truly-uninstrumented loop is
    DETERMINISTIC #(2 2 2) and the trace shows the `#z` class-become
    (2->3 slots) NEVER RUNS — only three `1->2` becomes (the `#y` adds).
    So the `#z` rebuild IS SKIPPED (ShNoChangesInClass): in the pure
    case the comparer really does see no change (builder allSlots reads
    size 2, missing z).  CORRECTION: an earlier "comparer exonerated"
    note was itself PERTURBED — even a ZERO-ALLOC recompile of
    compareVariables:with: heals iter1 (recompiling mutates the detector
    class's methodDict, which is enough of a GC nudge to flip the
    outcome).  LESSON: for these GC-state Heisenbugs, ANY image-side
    change — even a zero-allocation method recompile — perturbs the
    result; only VM-level (fprintf/lldb) observation is faithful.  So the
    root is: the image's `builder allSlots` / `localSlots copyWith: z`
    computation drops z when uninstrumented under accumulated heap state,
    healed by any GC.  Next step MUST be VM-level: a targeted, allocation-
    free hook logging the size of the OrderedCollection that allSlots/
    copyWith builds, or lldb breaking in the allocateSlots/copy path
    during the deterministic failing `#z` add — image-side probes cannot
    see the failing state.
  - EXHAUSTIVE VM-INTEGRITY RULING-OUT (2026-07-07, all perturbation-free
    VM-level, all CLEAN=0 hits while the bug persists #(2 2 2)):
    PHARO_SCAV_DANGLE_CHECK (scavenge missed-root / dropped young ref) =
    0; PHARO_ALLOC_SIZE_CHECK (allocateSlots slotCount != requested) = 0;
    PHARO_SHADOW_SLOTS (receiver-ivar store via untracked path / GC-mover)
    = 0.  CONCLUSION: there is NO detectable VM memory corruption — not a
    lost reference, not a mis-sized allocation, not a lost ivar store.
    The builder's slot collection genuinely computes to size 2 via
    correct VM memory operations.  So the failure is a HEAP-STATE-
    DEPENDENT CONTROL-FLOW divergence in the image's addSlot:/copyWith:/
    allSlots path (a conditional or comparison/hash/identity PRIMITIVE
    returning inconsistently under accumulated heap state — cf. the
    `self size` in copyWith's `copyEmpty: self size + 1`, or the
    anySatisfy: name-check), NOT a memory bug.  This is the hardest
    class: needs lldb single-stepping the failing `#z` build to find the
    primitive whose result diverges — every higher-level observation
    heals it, and every VM memory-integrity checker is clean.
  - Requires ACCUMULATED heap state (a warmup or loop that leaves a prior
    OBSOLETE same-named class version in the heap): iter1 of a loop
    passes, iter2+ fail; a single fresh sequence with the zero-alloc
    probe passes.  Heals on any GC (which sweeps the obsolete versions).
  - CONCRETE LEAD: PHARO_TRACE_BECOME shows the SlotTestsClassA class
    become during rebuild has ctHits=0 — the class being becomeForward'd
    is NOT found in classTable_ (a class's identityHash == its classTable
    index in Spur, so a class SHOULD be there).  Suspect: with multiple
    obsolete same-named class versions accumulated, the rebuild become +
    classTable redirect targets/updates the wrong version, or the new
    class isn't classTable-registered, so `c` keeps resolving to the old
    2-slot version.  Next: trace whether the Z-add's class become (2->3)
    runs at all in the failing case and why ctHits=0; check for stale
    obsolete-class entries in classTable_ / sweepClassTable timing.
  - **DECISIVE CORRECTION (2026-07-07, session cont.): this is a VM
    FRAME-MATERIALIZATION bug, NOT an image primitive divergence and NOT a
    heap/classTable/GC-accumulation bug.**  The prior "image control-flow
    divergence, VM-integrity clean, needs lldb on a primitive" conclusion
    was reading the symptom, not the cause.  New decisive facts:
    * Reproduces DETERMINISTICALLY under `PHARO_MAT_AT_CHECKPOINT=3` —
      forcing materializeFrameStack() every 3rd interpreter checkpoint
      with NO scheduling, NO process switch, NO second process.  N=1
      (materialize every checkpoint) PASSES; N=3 FAILS.  So the pure
      materialize->executeFromContext round-trip is the defect; scheduling
      only supplied the timing.  DET_SCHED=1 also repros ('1|2|2'); it is
      razor-sharp on quantum (Q=2 and Q=4 both PASS).
    * NEEDS >=2 materializations spanning the op: `PHARO_MAT_ONCE=N`
      (exactly one forced materialization, swept N=1..5000) NEVER fails.
      Single switch-to-context-mode runs clean to completion.  So the bug
      is in RE-materializing a frame that was itself restored from a
      context (materialize -> context -> inline -> re-materialize).
    * NEEDS a LIVE INSTANCE only for TIMING: `c new` before the add adds
      bytecodes that shift the deterministic schedule so a preemption
      lands inside the `#z` rebuild's deep call stack (fd up to 35-41 in
      MATFS trace).  Remove `c new` (fail3.st) => PASSES even under
      DET_SCHED.  The instance does NOT causally affect slot computation.
    * The class is left GENUINELY {x,y} (fail4.st: hasSlotNamed:#z=false,
      localSlots=#(#x #y), instSize=2, global==c) — so `#z`'s rebuild
      produced a slot list missing z, the ShSlotChangeDetector saw
      {x,y}=={x,y} and skipped.  z was dropped from the builder's slot
      collection while it sat on a deep frame's expression stack across a
      preempt/re-materialize cycle.
    * RULED OUT this session: capacity guard (zero CTX-CAPACITY/STATE-LOST
      hits in the fail run); the ctxSynced incremental-materialization skip
      (`PHARO_MAT_FULL_RESYNC=1` does NOT fix — so the drop is NOT the
      18708 skip); leaf/current-frame save (re-saves operands fully, no cap
      warning).  Remaining suspects: the frame[0]-reuse re-materialization
      path (Interpreter.cpp ~18563, updates activeContext_ in place, NOT
      covered by FULL_RESYNC) and the saved-frame exprEnd/nextFrameStart
      bookkeeping for a mid-stack frame whose savedFP came from a lazily-
      restored sender context.
    * REPRO SCRIPTS (scratchpad/fixrig/gui.image): /tmp/fail2.st (per-step
      counts '1|2|3' pass / '1|2|2' fail), /tmp/fail3.st (no instance,
      always passes), /tmp/fail4.st (post-hoc class-state dump — safe
      because DET_SCHED schedule up to the failing add is unchanged by
      trailing diagnostics).  DIAGNOSTIC KNOBS added (debug_vars.h):
      PHARO_MAT_AT_CHECKPOINT (force-mat every N cp), PHARO_MAT_ONCE
      (single-shot at cp N) + PHARO_MAT_ONCE_DUMP, PHARO_MAT_STEP_LO/HI
      (gate forcing to a g_stepNum window — used to bisect), PHARO_MAT_SEL
      (leaf-selector gate), PHARO_TRACE_SLOTBUILD (return-into-context
      origSp/retval/savedStack for slot-build methods).
    * NEXT: bisect the frame[0]-reuse path — add PHARO_NO_FRAME0_REUSE to
      force fresh-context creation at 18563 and test whether it fixes the
      deterministic MAT_AT_CHECKPOINT=3 repro; if so the reuse path's expr
      re-save (18616-18641) is the off-by-one.  Same shared root as the
      ARM context-storm and TF-callback tail hang (all materialize/restore
      fidelity under deep-stack repeated preemption).
  - **FURTHER REFINEMENT (2026-07-07 cont.): the drop is a POINTER/COUNT
    fidelity bug, NOT element-loss from a built collection.**
    * frame[0] is `#make` (the rebuild's root block), exprCount=0 — the
      new slots array is NOT on frame0; it lives on a MID-STACK frame at
      fd 27-47 (the addSlot:/copyWith:/slots: region).
    * KEY: the {x,y,z} Array is a HEAP object; frame save/restore moves
      POINTERS to it, never its contents — so z cannot be lost *from* an
      already-built array by materialization.  The corruption is either
      (a) a STALE pointer to the pre-copyWith {x,y} array restored into a
      temp/expr slot, or (b) an UNDERCOUNTED stackp at save (exprEnd too
      low) so the new-array pointer is dropped and restore brings back a
      short stack — the builder then computes allSlots=2, ShNoChangesInClass
      fires, rebuild skipped.
    * PHARO_NO_FRAME0_REUSE=1 is NOT a usable bisector: disabling the
      reuse path aborts the eval (load-bearing for Context>>jump, per its
      own comment) — it does not complete to a result.
    * FULL_RESYNC exoneration of save is INCOMPLETE: FULL_RESYNC re-saves
      but reuses the SAME exprEnd=savedFrames_[i+1].savedFP count, so an
      exprEnd/savedFP miscount is NOT ruled out by it — still the prime
      suspect.  SAVE-FRM trace of slot-named frames shows exprCount=0 for
      the read-side accessors (visibleSlots/localSlots/lastSlotsOn:) — the
      array rides on copyWith:/addSlot:/slots:/builder frames not yet
      caught (they complete in too few checkpoints to be materialized as
      SAVED frames often).
    * **ROOT CAUSE FOUND (2026-07-07 cont.) — JIT-FRAME MATERIALIZATION
      corrupts a slot index to 0; the "ShNoChangesInClass skip" narrative
      (all of the above) is WRONG.**  Decisive evidence chain:
      - PHARO_TRACE_SLOTCMP (dump collection sizes at the change detector):
        the z-add's ShSlotChangeDetector>>compareVariables:with: correctly
        sees OrderedCollection(sz=2) vs OrderedCollection(sz=3) — the change
        IS detected — and the builder's slots: correctly receives
        Array(sz=3).  So slot-building + change-detection are CORRECT; the
        rebuild PROCEEDS.
      - PHARO_TRACE_BECOME (+ step#): the y-add does its class become
        (SlotTestsClassA 1->2 slots, step 189333038); the z-add's 2->3
        become NEVER fires (zero becomes after the z-compare).  So the
        z-rebuild aborts between compare and install.
      - The abort is 'Error: SubscriptOutOfBounds: 0 in
        InstanceVariableSlot(IndexedSlot)>>object:instVarAt:' during instance
        migration — a slot index reads 0, the read primitive fails, the
        error unwinds past the class-install become, z is never installed,
        class stays {x,y}.  Present ONLY in failing runs (0 in clean /
        MAT_AT_CHECKPOINT=1).
      - **The bug requires BOTH JIT and materialization:**
        MAT_AT_CHECKPOINT=3 + JIT => FAIL; MAT_AT_CHECKPOINT=3 + NO_JIT =>
        PASS; DET_SCHED + NO_JIT => PASS.  So materializing a JIT-compiled
        instance-migration frame corrupts a slot-index operand to 0.
        Explains the second-add specificity: the migration methods are
        JIT-compiled (warmed by the first add) by the second add.
      - The index-0 read is JIT-inlined: neither primitiveInstVarAt (73) nor
        primitiveSlotAt (173) index<1 trap fires with JIT on.  Interpreted
        migration calls (which DO fire the activateMethod hook) show correct
        indices (object:instVarAt: SmI=2 for y); the failing index-0 call is
        the JIT-compiled read:/object:instVarAt: whose `self index` reads 0.
      - NEXT: the JIT-frame materialize/restore fidelity (J2JSave /
        materializedRetSlot path in materializeFrameStack).  Find where a
        JIT frame's operand (the slot ref or its index) is dropped/zeroed
        across preempt->materialize->resume.  Fix must restore JIT-frame
        operand state faithfully (NOT "don't JIT migration").
    * PROBE-IMAGE SHORTCUT IS DEAD (2026-07-07): `Smalltalk saveAs:` inside
      an eval FREEZES the pending-eval result — a reloaded probe.image
      returns the frozen `'#(#x #y)'` for EVERY eval (even `(3+4)`), because
      the SnapshotOperation context resumes on load and replays.  Also, a
      class saved mid-build is left permanently unmodifiable (z-add always
      no-ops, GC does NOT heal — a DIFFERENT, save-induced structural state,
      not the timing bug).  So the timing bug cannot be shrunk to a small
      saved-image repro; it needs the full 200K-step fresh build.
    * NEXT (lldb on the FULL DET_SCHED repro, per CLAUDE.md — do NOT punt):
      the deterministic MAT_AT_CHECKPOINT=3 / DET_SCHED=1 repro on gui.image
      + /tmp/fail2.st is the oracle.  Save paths are faithful (FULL_RESYNC
      re-saves, zero CTX-CAPACITY); the remaining suspect is RESTORE-side:
      executeFromContext's leaf restore + the return-into-context re-inlining
      chain (returnValue ~line 7470) for the deep (fd 27-47) trait-rebuild
      stack.  Break there conditional on g_stepNum in the failing window
      (bisected: materializations must span the whole 2nd add, needs >=2
      far apart) and sender method in {addSlot:, copyWith:, slots:, the
      builder} and inspect whether the restored stackp/operands drop the
      new-slots-array pointer or restore a stale {x,y} array pointer.
- [x] **Candidate-queue hunt COMPLETE 2026-07-07** (14-agent workflow
  wf_214bdc82, one agent per package, local fresh-image parity+shrink;
  five VM fixes committed e5570688/38c457f7/ac87599f/f9e49453/4c4e13e6,
  details in docs/changes.md 2026-07-07): soil 9/9 (SQFile handle shape),
  sauco 6/6 (profiler deadline), illimani 15/15 (ephemeron tenure +
  param 34), methodproxies 39/39 (NLR aboutToReturn: protocol),
  redistick stock-parity (POLLHUP), myprecious testArgPassByCopy
  (readSema storm).  Remaining, classified no-VM-change: famix/lexicon/
  deeptraverser/polymath/hera perf-gap (kernels quantified: block
  invocation 32x, reflective scan 15x, min:/max: send 74x — activation-
