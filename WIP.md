# WIP — JIT Cog-speed: inline-J2J DEFAULT-ON (2026-06-09/10)

Goal (active /goal): **fix this jit to work and be as fast as cog.**
Branch: `jit`. Build: `cmake --build build-opt` (optimized; the plain `build/` is -O0).
Test VM: `./build-opt/test_load_image /tmp/harness/Pharo.image eval "<expr>"`.
Stock Cog baseline: `cd /tmp/harness && ./pharo Pharo.image eval "<expr>"`.

## NEWEST (2026-06-10 PM): sp-depth instrument -> staticJ2JArgCount fold bug FIXED

- **PHARO_SP_DEPTH_CHECK** (BcDepthMap.{hpp,cpp}, knob in debug_vars.h):
  per-bcOffset static operand-depth map (worklist walk, side table per
  JITMethod) verified against `state.sp` at every checkable JIT exit
  (Send/SendCached/MustBool/ArithOverflow/Block/ArrayCreate/Yield; 8 call
  sites incl. both chain loops).  754K checks / 0 false positives healthy.
  KEY FACT it documented: **sp convention is one-past-TOS everywhere**
  (stackTop() = sp[-1]; the JITState.hpp "points to TOS" comment was stale).
- **Found in its FIRST failing-config run**: [SP-DEPTH] delta == nArgs at
  handle:offset: -> the J2J return prelude's staticJ2JArgCount fold
  (AsmjitT1 ~7318) is UNSOUND cross-method: the CALLEE's prelude pops the
  CALLER's save but folds the sp-adjust from its OWN send sites
  (initializeHandle:offset:'s only send is 0-arg `self initialize` ->
  sp-adjust folded to 0 while popping a 2-arg save -> sp high by nArgs,
  retval in an arg slot).  This was the lever-(c) MAX_IC=1 corruptor the
  selector bisect named.  FIX: force dynamic load-from-save when xmethod
  or inline-block-value is enabled (every push site writes
  save.sendArgCount correctly, so dynamic is always sound).
- **Validation**: MAX_IC=1 repro 0/4 -> ~85-90% pass; benchFib unchanged
  (10x fib28 123ms); 200-class suite vs abdet_default: **12 stable
  FAIL->PASS** (the entire *DictionaryTest>>testIncludes family!) and 0
  stable regressions (3 flickers = FIFOQueue heavyContention2,
  ProcessTerminate nestedUnwindS1, WeakIdentityKey clearing/includes —
  all re-run flaky).  Default config was previously safe only by the
  leaf-only accident (MAX_IC=0 callees have no sends -> fold already -1).
- **RESIDUAL (~8-20% by layout): MAX_IC=1 still loses the eval** with a
  nextPutAll: DNU in OpalCompiler>>evaluate (catch_FAIL.log).  Caught a
  failing run WITH the detector on: **mismatches=0 in 767K checks** ->
  NOT an sp-depth desync at checkable exits.  Value corruption (wrong
  receiver), or desync confined to unmappable methods (~10%) /
  uncheckable exits (ExitReturn/J2JCall).  Per-run variance under
  DET_SCHED = ASLR slide is part of the knife-edge (lldb's no-ASLR
  hides it).
- **RESIDUAL DIAGNOSED + FIX BUILT (catch2 anatomy -> stale
  bcStartCache):** the caught DNU shows `#,` sent to a StdioStream — the
  receiver one slot below (the 'EVAL-RESULT=' string) was REPLACED by a
  wrong VALUE at CONSISTENT depth, with adjacent duplicated slots and
  scavengesSoFar=3.  Wrong-values-at-right-depth = a materialized frame
  resumed at a WRONG-BUT-IN-RANGE bytecode offset: JITMethod::
  bcStartCache (used by the xmethod inline-J2J emit for state.ip,
  AsmjitT1 4392/4873) is set once at compile from compiledMethodOop and
  NEVER refreshed — a scavenge moving the CompiledMethod (fresh eden
  DoIt / OpalCompiler infra) leaves it at the old address; sp-depth
  validates ip-vs-sp CONSISTENCY so it stays clean at a wrong ip.
  FIX: refresh bcStartCache in forEachRoot's JIT-zone walk (zone already
  writable there; idempotent across mark/update phases; covers scavenge
  + fullGC).  This was also "latent suspect #2" from the 3-agent map —
  yesterday's lldb only ruled it out at ONE fire.  Validation: catch3
  40-rep loop on the fixed binary (in flight), then 200-class suites
  default + MAX_IC=1.
- **Lever (c) PRIZE CONFIRMED on the fold-fixed binary**: incs-shape
  bench (callee with a send: `sfib ... incs`, `incs ^self incc`)
  default 370-378ms vs MAX_IC=1 **51-52ms (7.3x)** — raising MAX_IC
  is the next big cross-method win once the residual is confirmed dead.
- **Interp-side sp-depth extension** (same commit family): the residual
  DNU fires at a `,` that dispatches via **ExtSend 0xEA** (the eval DoIt
  has >15 literals) — op_send0/1/2 hooks alone NEVER see it.  Hooks now:
  threaded op_send0/1/2 + the ExtSend switch case.  Known noise floor:
  6 FFI/stdio sites report stable +1 (special activation frame variant,
  deduped).  Internal resends (adaptTo*, value-retry) and primitive
  methods are skipped (argCount-vs-site filter + prim-bit filter).
  Catch-loop protocol: /tmp/catch_fail5.sh — rep until no EVAL-RESULT,
  keep the log, grep SP-DEPTH-INTERP minus the noise selectors.
- **ROOT CAUSE FOUND (catch loop iterations 4-9): SCAVENGE NEVER CALLED
  prepareForGC/afterGC.**  Only fullGC wraps object motion in the
  ip->offset round-trip.  A scavenge moves young CompiledMethods —
  including the one the interpreter is CURRENTLY EXECUTING (fresh eval
  DoIt / freshly-compiled methods): forEachRoot updates method_ oops,
  but instructionPointer_/savedIP/state.ip keep pointing at the OLD
  eden copy.  Execution continues on the stale copy — CORRECT until
  eden refills and overwrites those bytes, then the interp executes
  the new objects' raw bytes (string-as-method, `,`-to-StdioStream,
  silent early return — every observed mode).  Explains the ~10%
  rate (race: DoIt completion vs eden refill), the scavengesSoFar=3
  signature, the ASLR knife-edge, and the detectors' silence (stale
  ip is out-of-range vs the MOVED method -> interp check skipped it
  silently).  FIX: scavenge() now wraps in prepareForGC/afterGC(false)
  (skips the full-GC-only methodCache/IC flush tail; gcPrepared_
  guards the fullGC-internal scavenge from double-prepare).
  En-route fixes that were ALSO real bugs: stale bcStartCache (commit
  earlier), megaCacheAdd tenure guard (young oops in a non-root,
  stencil-probed cache = same hazard class, distinct path), walker
  ExtJump semantics (operand is UNSIGNED + extB sign — T1's emit reads
  int8: LATENT T1 MISCOMPILE for naked forward long-jumps >= 128,
  AsmjitT1.cpp ~6965/1221/846 — STILL UNFIXED, next task).
  Validation in flight: catch10 60-rep loop.  Scavenge-wrap perf cost
  TBD (prepareForGC is O(frameDepth) per scavenge; if measurable, only
  wrap when any executing/saved method is YOUNG — usually none are).
- **catch10/11 (post scavenge-wrap): STILL FAILS (~5%).**  [DNU-METHODREG]
  probe (knob-gated, in sendDoesNotUnderstand) corrected an earlier
  misread: method_ at DNU time IS valid heap — an EDEN CompiledBlock
  (the DoIt's on:Error block; selectorOf prints `,` for blocks —
  quirk), ip correctly inside it, bytecode window sane (`...9b 96 >d8
  53 87 d8 88 5e`).  The failure shape from the window: `Stdio stderr`
  (send0) RETURNED THE WRONG OBJECT (the 'EVAL-RESULT=' string) ->
  cascade dup'd it -> nextPutAll: DNU on ByteString.  Earlier variants
  (`,` to StdioStream) = the same class with the wrong object appearing
  one send earlier.  VALUES wrong at CORRECT depth at every check.
  **DICTIONARY REPRO CHAIN (supersedes the MAX_IC attribution!):**
  single-class DictionaryTest>>testIncludes FAILS with NO KNOBS AT ALL
  (the suite-pair MAX_IC attribution was run-context coincidence: batch
  200-class PASSES, single-class FAILS, same binary same env).
  PHARO_NO_JIT -> PASS.  Bisect panel: NO_INLINE_J2J / NO_INLINE_GETTER
  / NO_J2J / NO_TIER2 / NO_SISTA all FAIL; **PHARO_ASMJIT_T1_STUB_ONLY
  -> PASS = the bug is in T1's REAL codegen**, independent of the J2J/
  IC superstructure.  Compile-cap bisect: PASS<=3242 FAIL>=3246, but
  the window selectors (slot-reflection family: write:to:,
  instVarNamed:put:, slotNamed:ifFound:ifNone:,
  instanceVariablesToKeep) don't cure individually with uncapped JIT —
  cap-boundary not identity-stable (known trap, rediscovered).
  IN FLIGHT: automated identity-stable halving bisect over all 1759
  compiled selectors (/tmp/halve_dict.sh, result /tmp/halve_result.txt,
  ~30s/run): finds the minimal skip set that cures.  Repro recipe:
  printf 'DictionaryTest\n' > /tmp/sunit_class_names.txt; run
  build/test_load_image Pharo.image; grep testIncludes detail.
  **MAX_IC FLIP GATE (suite pair on the final binary): MAX_IC=1
  RE-BREAKS the *DictionaryTest>>testIncludes family** (the exact 12
  tests the fold fix cured in default config; default run PASSES them,
  MAX_IC=1 run FAILS them, 26 diff lines total = just this family +
  flakies).  Eval-level (catch25 80-rep) is CLEAN — this is a
  DETERMINISTIC suite-level repro: stage `DictionaryTest` in
  /tmp/sunit_class_names.txt, run with PHARO_T1_XMETHOD_MAX_IC=1 +
  detectors (SP_DEPTH_CHECK, VERIFY_GETTER) — same bisect protocol,
  far easier than the eval Heisenbug.  Suspect shape: another
  cross-method assumption (same class as the fold bug) in a path that
  with-send-callee admission newly exercises (includes: -> hashed
  collection scan loops).  The MAX_IC default flip is BLOCKED until
  this family is clean; the 7.3x incs prize stays opt-in meanwhile.
  **RESIDUAL KILLED (catch22-25, controlled):** the corruptor is the
  DISPATCH-A-SIDE entry into the shared tryGetter label (AsmjitT1
  ~3880).  Bisect: disabling only that tbnz -> 30/30 clean; same-length
  dummy env var -> fail rep 1 (real, not layout).  x2/x1 contract
  reload REDUCED (fail rep 31) but didn't eliminate -> the dispatch-A
  bail paths leave MORE than registers inconsistent (suspect: partial
  state commits, e.g. state.ip = callee bcStart at the J2J callee-setup
  ~4392 before a late bail).  FIX SHIPPED: that entry is DEFAULT OFF
  (opt-in PHARO_T1_GETTER_IN_J2J); getter sends on that path take
  dispatchCached (they were already on a bail path).  VALIDATION:
  80-rep catch25 on the full failing config -> ZERO failures.
  PROPER ROOT-CAUSE (later): audit dispatch-A bail paths for partial
  state commits; verify-getter v1/v2/v3 (all clean) prove the IC
  entries were never poisoned — it was always execution-state, which is
  why every entry-validity instrument stayed silent.
  catch19 (verify-on-fire v1, bounds check only): CAUGHT rep 2 with
  ZERO [VERIFY-GETTER] flags — the poisoned slotIdx is IN-BOUNDS
  (wrong-but-small index, e.g. ivar 2 instead of 0 -> wrong object,
  no bounds trip).  v2 NEEDED: full re-derivation in the helper —
  expected slotIdx from the IC entry's METHOD slot (entry base reg:
  see the probe emit at AsmjitT1 ~5011-5088 where x7=extra is loaded;
  pass the entry pointer as arg4) -> primitiveIndexOf(methodBits)-264
  vs extra&0xFFFF; ALSO compare entry key vs recv classIndex.  Any
  mismatch = the poisoned entry with full provenance.
  catch16/17/18 (ExtJump fix, pendingICPatch GC-clear, icBuffer
  ownership guard — all real bugs, all committed): residual STILL
  reproduces (~rep 6-16).  GETTER CLASSIFICATION MACHINERY now fully
  mapped (upgradeICToJ2J ~21436): bit63|slotIdx derives from
  cachedMethod's QUICK-PRIM index (264+N), keyed by receiver =
  stackPointer_[-(sendArgCount+1)].  TWO REMAINING CANDIDATES:
  (a) stale cachedMethod oop at classification time (wrong method's
  header read -> wrong slotIdx, valid-looking entry);
  (b) stale state.sendArgCount at the upgrade call (Interpreter.cpp
  20035) -> WRONG RECEIVER fetched -> wrong class lookupKey -> getter
  classification poisoned onto an unrelated class.
  NEXT INSTRUMENT (decisive): verify-on-fire — knob-gated BLR in the
  T1 inline-getter emit calling a C++ helper that re-derives (recv
  class -> selector via selBitsArray -> lookup -> quick-prim classify)
  and compares slotIdx + re-read value; mismatch prints the IC entry +
  both classifications.  IMPLEMENTATION SHORTCUT: the arm64 getter emit
  (AsmjitT1 ~5135-5172) ALREADY has the exact BLR scaffolding — the
  FINDNODE_WATCH g_emitGetterTrace block (sp-48 save of
  x0/x1/x2/x6/x7/x30, BLR jit_rt_atrec_getter(state, recv, val,
  bcOffsetFromMethObj), restore).  Clone it under a new knob
  (PHARO_VERIFY_GETTER), pass x7 (the extra word, already saved at
  sp+32) so the helper has slotIdx; helper mirrors
  jit_rt_atrec_getter's plumbing to find the site selector from
  state.jitMethod's selBitsArray.
  catch15 (methodMap-rebuild-per-scavenge fix in): CAUGHT at rep 25 —
  the patchJITICAfterSend guard hole was REAL (fix committed, sound)
  but the getter poisoning has ANOTHER path.  NEXT hygiene candidates:
  (a) clear pendingICPatch_ in prepareForGC (any pending patch carries
  pre-GC assumptions); (b) selector equality is NOT site identity —
  same-selector foreign sites still pass the guard (poison a getter
  classification across RECEIVER CLASSES with different ivar indices);
  (c) **T1 ExtJump MISCOMPILE CONFIRMED WIDER**: the pre-scan ACCEPTS
  ExtendB+ExtJump bundles (t1EnableJumps), but the emit at ~6965 reads
  ONLY the operand byte as int8, ignoring extB.  Quadrants: extB=-1 &
  operand>=128 (backward 1-128 bytes — common loop bodies) is correct
  BY COINCIDENCE; backward >128 bytes, naked forward >=128, and all
  extB=+1 forward jumps MISCOMPILE (branch to a wrong in-range label =
  wrong-position execution, locally depth-consistent — fits the
  residual!).  OPEN: check whether the prefix BUNDLE handler (~6719)
  intercepts ExtendB+ExtJump before the 6965 site (if so the naked
  >=128 case is the only live miscompile).  Fix all decode sites
  (computeLiveLength ~846, first-pass ~1221, emit ~6965) to interp
  semantics: offset = byte + (extB << 8), byte UNSIGNED.
  catch13/14 VERDICT (KNIFE-EDGE-CONTROLLED): **PHARO_T1_NO_INLINE_GETTER
  -> 30/30 PASS; same-length DUMMY var (PHARO_T1_NO_INLINE_GETTEX) ->
  FAILED at rep 8 — the bit-63 INLINE GETTER emit is the corruptor
  route, NOT layout luck.**  Next: root-cause the getter staleness —
  audit every IC extra-word writer (jit_rt_fill_ic rewrites extras
  fresh ✓; check patchJITICAfterSend / upgradeICToJ2J /
  applyICSpecialization for partial writes), and the getter emit's
  ivar-index/receiver assumptions under xmethod (MAX_IC=1).  Suspect
  family: recoverAfterGC's selective IC clear PRESERVES upper-16 extra
  flags (bit 63 + classification) while zeroing key/method — sound only
  if every refill path fully rewrites the extra word.
  catch12 VERDICT: **PHARO_NO_JIT -> 30/30 PASS (same env) — the
  residual is JIT-SIDE.**  A JIT-executed send (the `Stdio stderr`
  class-side accessor, xmethod-admitted at MAX_IC=1) returns the WRONG
  OBJECT with correct sp.  PRIME SUSPECTS (wrong value, right depth):
  the IC-hit INLINE specializations that produce a result WITHOUT
  calling — TRIVIAL_BITS (63/62/61/57), RETURNS_LITERAL (58), and the
  bit-63 inline getter (stats show getter=325K fires in failing runs;
  retLit=0).  These read CACHED values/slot addresses from IC extras —
  a stale cached value = exactly this symptom.  NEXT (catch13): bisect
  with the spec knobs on the failing config — PHARO_T1_NO_INLINE_GETTER
  and the trivial/retLit opt-outs (see debug_vars.h / DebugSettings) —
  one knob per 30-rep catch loop; the knob that cures it names the
  emit.  Then root-cause that emit's staleness (likely: IC extras not
  GC-visited for the cached-value bits, the SAME hazard class as the
  megaCache — young value cached in a non-root location).
  Catch logs: /tmp/catch11_FAIL.log.
  NOTE: all catch reps run in /tmp/harness on Pharo-base.image; evals
  share /tmp/harness/startup.st — NEVER run a second eval there while
  a catch loop is live (use /tmp/harness2).

## Headline: inline-J2J is DEFAULT-ON (lever e, commit after 0a48a0e1).
DEFAULT config now: cfib(30) 344ms -> 44ms (Cog 8), benchFib(30) 296ms -> 32ms
(Cog 5).  60-class SUnit A/B default-vs-J2J per-test identical (4130/4140;
two known-flaky tests flip one each way; the historical flip-blocker
CharacterTest>>testStoreStringAll PASSES).  Opt-out: PHARO_T1_NO_INLINE_J2J=1.
Confirmation COMPLETE: 1-60 re-run 4131/3 (one flaky better than the
pre-flip A/B); 61-200 (4319 tests) J2J-on 4297 vs J2J-off 4299 — only
per-test diffs are LIFOQueueTest>>testHeavyContention and
WeakAnnouncerTest>>testWeakDoubleAnnouncer, both PASS under J2J in
isolation on a quiet machine (load-flaky).  No reproducible regression
across 200 classes / 8459 tests.  The 61-200 SHA256Test TIMEOUT (both
configs) is the 64MB zone filling (~14.6K methods) -> late-hot methods
interpreted; PHARO_CODE_ZONE_MB=192 works around it until lever (d)
shrinks the ~6KB/method emit.  Next perf checkpoints vs Cog: re-run
the full classify-sunit Δcog comparison on build-opt, and watch
inline-J2J chain lengths vs preemption fairness (testHeavyContention
under load is the canary).

## DONE this session (committed)

1. **dd0fa6f2 — cross-method inline-J2J gate off-by-one.** The xmethod gate
   chain in AsmjitT1.cpp read JITMethod bytes 47/46 as isStubOnEntry/
   canBailMidMethod; real layout 46=hasNLR 47=canBailMidMethod 48=isStubOnEntry.
   Since t1NlrTailOnly (default ON) sets hasNLR for EVERY method containing a
   return opcode 0x58-0x5C, the byte-46 read rejected ~all real cross-method
   callees (574K/930K in cfib) — this WAS the entire cross-method send gap.
   The real isStubOnEntry was never checked (stubs slipped through = the old
   "xmethod corrupts state" reputation). All JM reads now use offsetof().
   Also: per-gate bail counters (`xmethod gates:` line in dumpJITStats, only
   emitted when PHARO_T1_INLINE_J2J is set), and the 30000 default fire cap
   (bisection leftover) lifted to unlimited.

2. **24175466 — stale state.j2jDepth corruption (xmethod at scale).** With
   876K+ fires/run, ~50% of startups corrupted (SettingTree mergeSort
   sortBlock -> #value:value: DNU). Root cause: materializeJ2J (chain loop,
   Interpreter.cpp ~22556) consumed pending saves into SavedFrames but never
   reset state.j2jDepth; the ExitBlockCreate/ExitArrayCreate handlers check
   `state.j2jDepth > 0` BEFORE enableJ2J resets it, so the
   `state.sp = stackPointer_` resync was skipped on consumed saves -> fresh
   closure above stale sp -> overwritten -> shifted operand passed as the
   sortBlock. Fix: materializeJ2J resets depth+cursor. 10/10 clean startups.
   Also hardened: prepareForGC/afterGC now round-trip live J2J pool saves'
   raw `ip` through J2JSave.ipOffset (was a real GC hole: forEachRoot visits
   only save.receiver); PHARO_J2J_MAT_LOG diagnostic knob.

3. **(uncommitted at write time) rj2j slice reservation** — the opt-in
   PHARO_RESUME_J2J chain loop's save slice was never reserved in
   j2jPoolCursor_, so GC receiver walk / ip round-trip / eviction pinning
   all missed those saves. Latent (rj2j default-off); fixed anyway.

## Key corrections to the previous WIP (premise was WRONG)

- "cfib->incc gets 0 bit-60 fills" was an ARTIFACT: PHARO_J2J_LOG_FILL caps
  at 4000 lines and startup burns them. The IC fill always worked
  (PHARO_DUMP_RECOMPILE_IC=cfib shows bit-60 + entryAddr on site 2).
- The slow path was the SEND-SITE DISPATCH: with the gates broken, every
  cross-method bit-60 send bailed to the trampoline activation
  (2.5M C++ activations/run @ ~340ns).
- incc DOES JIT-compile (T1-COMPILE right after cfib). `^self+1` is NOT
  classified by detectTrivialMethod (intArith needs a method ARG, 0x40).

## Current state / next levers toward Cog parity (~5x left on cfib)

- Validation in flight: SUnit batch 0-150 default-vs-INLINE_J2J
  (results /tmp/sunit_results_{default,j2j}.txt). MUST be equal-or-better
  before flipping PHARO_T1_INLINE_J2J default-on (lever (e), the big one —
  all of today's wins are behind that opt-in env).
- Residual xmethod bails (cfib run): bail_prim=105K (prim-bearing callees),
  bail_numic=237K (callees with sends — lever (c), needs nested-save
  correctness), bail_b47/stub+canBail ~17K. Each bail = slow trampoline.
  ALSO: any callee with conditional jumps has canBailMidMethod=1 -> b46
  bail (e.g. Number>>max: = 1.35M bails in the incs bench) — relaxing
  that is part of the same nested-correctness project.
- **Lever (c) status (PHARO_T1_XMETHOD_MAX_IC=N knob, commit 1238b606):**
  N>=1 (admit callees with sends) still corrupts after the j2jDepth fix
  AND after j2jEntryDepth hardening at 4 JIT_CALL sites.  DETERMINISTIC
  REPRO: `PHARO_T1_INLINE_J2J=1 PHARO_T1_XMETHOD_MAX_IC=1 PHARO_DET_SCHED=1`
  -> 4/4 runs lose the eval silently (clean exit, no DNU, no EVAL-RESULT;
  [STARTUP-ST-FIRED] never appears -> the corruption drops the startup
  action from SessionManager's startup list, no error raised);
  **BISECT CAVEAT (2026-06-10, supersedes the 'culprit pinned' claim):**
  the cap-bisect boundary is NOT call-identity-stable — capping changes
  inline-vs-trampoline charge counts downstream, so the boundary fire
  shifts with the binary (26705 = handle:offset:->initializeHandle:offset:
  on one build; 26683 = reset->resetTo: — a LEAF pair! — on the next).
  A leaf boundary proves the bisect finds a scheduling-sensitivity
  point, not the corruptor.  The corruptor is some 1-IC-callee fire
  (leaf-only is clean), identity unknown.  The watchpoint recipe still
  gives a deterministic stop + live registers at any chosen fire count,
  and `expr *(unsigned long long*)&g_xmethod_max = -1` at the stop
  un-caps live so the boundary call can be single-stepped.  RULED OUT at the fire (lldb raw-memory checks): tempCount setup (the
  xmethod path reads callee JM[35] dynamically; offsets 34/35 verified
  vs offsetof) and stale bcStartCache (JM[104] == compiledMethodOop+40,
  consistent -> CM had not moved by fire #26705).  NEXT: single-step
  the callee's FIRST mid-method C++ exit (its send sites are cold ->
  ExitSendCached) and its return path — the corruption mechanism lives
  in that exit/materialize/resume sequence, since leaf callees (which
  never exit mid-method) are immune.  Callee initializeHandle:offset:
  stores into receiver ivars (write-barrier path) — check the
  popStoreRecvVar interaction with a J2J-entered frame too.
  LOGGER FIXED (928df628): the XMETHOD_LOG wrapper now saves x0-x13+x30
  (was clobbering x1-x6 = receiver+args -> every logged call corrupted,
  the origin of the "xmethod corrupts state" lore).
  **SELECTOR-LEVEL BISECT DONE (2026-06-10, identity-stable):**
  PHARO_J2J_SKIP_SELECTORS halving over the 429 with-sends+filled
  callees converged on ONE selector: **initializeHandle:offset:**
  (ExternalData family: `self initialize. handle := aHandle.
  startOffset := aNumber - 1` — FIRST bytecode is a send -> immediate
  mid-method C++ exit on every cold call).  Skipping ONLY its fills
  cures the full failing config (ok + DNU=0).  Same selector the
  original fire-bisect named — that identification was genuine.
  **ROUTE NARROWED:** with the fixed XMETHOD_LOG, the failing run shows
  only 8 dispatch-A inline fires, NONE to this callee -> the corruptor
  route is the C++ J2J driver / stencil consumption of its bit-60 IC
  entry (ExitJ2JCall driver, Interpreter.cpp ~19302) — which has NO
  numICEntries gate and runs in the DEFAULT config too.  MAX_IC=1
  likely only shifts scheduling to expose it; the default config may
  carry the same latent bug.  NEXT: lift the 200-line PHARO_J2J_MAT_LOG
  cap (Interpreter.cpp materializeJ2JSaveIntoFrame) and/or conditional-
  break the ExitJ2JCall driver on calleeCM==initializeHandle's oop in
  the DET run; inspect the mid-callee-exit (its first `self initialize`
  send) -> materialize -> resume -> return sequence.
  GOTCHA: PHARO_J2J_ONLY_SELECTORS kills ALL IC fills for other
  selectors (0% IC hit rate) — runs under it DNU with ANY selector and
  are NOT a valid minimal repro.
  **EPISTEMOLOGY (2026-06-10, READ BEFORE TRUSTING ANY A/B ON THIS BUG):**
  the DNU-visibility is a LAYOUT KNIFE-EDGE: adding ANY env var (even
  AAAA=1, or byte-length-matched pads) makes the default+DET 7-DNU
  baseline go silent.  DebugSettings copies env strings at static init,
  so env content shifts allocation layout deterministically; the
  corrupt write lands in slack on most layouts.  CONSEQUENTLY:
  - POISONED (layout mirage): the heap-write-callee gate "cure"
    (PHARO_J2J_NO_HEAPWRITE_CALLEES — byte-matched pad cures equally),
    the skip-selector bisect convergence on initializeHandle:offset:
    (env length shrank monotonically with the candidate halving), the
    SCAV_DANGLE_CHECK "cure".
  - STILL VALID: the 7-DNU baseline itself (stable across reruns,
    exact env); cap=26682-pass vs 26683-fail (identical env length —
    ONE extra cross-method inline fire flips the severe outcome);
    NO_J2J_BRANCH keeping 7 DNUs DESPITE its env-length change (the
    corruption fires broadly; behavior knobs re-land it visibly).
  - The corruption is REAL and present in the shipping default config;
    wall-clock scheduling + layout luck hide it (200-class suites
    clean).
  - **KNIFE-EDGE EXTENDS TO BINARY DELTAS (2026-06-10 experiment
    series):** even the C helper's body size moves the visibility, so
    cross-binary DNU comparisons are NOISE.  Tested at the
    DET+NO_J2J_BRANCH 2-DNU deterministic repro: store-site nop sleds,
    register save/restore, empty calls, delay loops — DNUs persist
    within each binary; the one 0-DNU binary (full ring) is not
    attributable.  ONLY within-binary deterministic comparisons are
    valid evidence on this bug.
  - PHARO_T1_STORE_RING landed (knob-gated): store-provenance ring +
    DNU-time scan — useful once a config shows DNUs WITH the ring
    enabled (search configs; the scan prints planted-value provenance
    with selectors).
  - **SHADOW-SLOT INSTRUMENT BUILT (PHARO_SHADOW_SLOTS, committed
    2026-06-10):** ShadowSlots.{hpp,cpp} — 1M-entry (object,slot)->
    (value,writer) table; writers tracked: 3 JIT store emits,
    storePointer, storePointerUnchecked (covers setReceiverInstVar);
    readers verified: interpreter pushRecvVar (both paths) + JIT
    pushRecvVar/ExtPushRecvVar emits.  GC: forEachRoot visits entry
    oops, afterGC rehashes, becomeForward clears.  VALIDATED: ~28M
    stores / 3.45M checks / 0 mismatches / 0 false positives per
    startup across default+DET+MAX_IC=1+NOJB configs — writer coverage
    complete for exercised paths.  CAVEAT: those configs also showed
    no DNUs in the instrument's layout, so absence-of-mismatch is not
    yet absence-of-corruption.
  - **SUITE-SCALE VERDICT (200 classes): IVAR SLOTS EXONERATED.**
    974M tracked stores / 199M verified reads / exactly 4 mismatches —
    all the two-way-become family (prim 128 now clears the table; the
    only untracked writer found).  No ivar ever changed via an
    untracked path at scale.  (Suite 38 fails/6 timeouts vs 13/1
    uninstrumented = the ~4x instrumentation slowdown on
    timing-sensitive tests, not corruption.)
  - **REFRAME: the corruption class is STACK-FRAME SP-DESYNC, not heap
    stores.**  All victims in the original evidence were TEMPS
    (mergeFirst's `by` arg, OpalCompiler do: receiver), and both
    mechanisms actually root-caused (stale j2jDepth sp-resync skip;
    saveless return hijack) were sp desyncs that shift operand/temp
    slots.  NEXT INSTRUMENT: per-bcOffset expected-stack-depth table
    (computable at T1 compile from the bytecode stack effects) +
    verification of (state.sp - state.tempBase) at every JIT
    exit/resume boundary — catches the desync AT the boundary that
    produces it, with the method+bcOffset in hand.  The existing
    spAtLastJ2JCall/dispatchTraceLeak machinery in the chain loop is
    prior art for the check sites.
  - hasRecvFieldWrite now computed by T1 (commit 0bc7d7f9) — flag is
    real even though the gate experiment was inconclusive.
  Recipe (reusable): DET_SCHED makes the failure a deterministic
  function of the fire cap -> binary-search the cap (~20 runs), then
  watchpoint the cap-bail counter in the LAST PASSING run; x10=calleeJM
  x19/x11=callerJM x12=callerCM at the stop; resolve oops via
  PHARO_T1_TRACE_COMPILE log lines (lldb expression evaluator is
  useless under LTO).
  without DET_SCHED: intermittent startup DNUs (#do: in OpalCompiler
  evaluate / #key in WorkingSession startup), runs recover.  Control:
  DET_SCHED+leaf-only is clean.  Next session: lldb the deterministic
  repro; the send-bearing-callee bench is incs '^(self+1) max: 0' in a
  cfib loop (503ms today vs 41ms leaf — the lever-(c) prize).
- The xmethod fire path still bumps g_xmethod_count + cap-check per fire
  (2 ld + add + st + cmp); could be emitted only when a cap is set.
- J2J-s (stencil_sendJ2J out-of-line) handles 2.4M calls/run — dispatch-A
  inline-J2J only covers sites in recompiled (tier-2) callers? Verify
  coverage; tier-1-only callers may never get the inline emit.
- Saveless path (PHARO_T1_CAN_SKIP_J2J_SAVE), state after e6bb3809:
  - RETURN-HIJACK FIXED: the callee prelude tail-jumps past the
    post-blr restore whenever j2jDepth > j2jEntryDepth (callers
    mid-J2J-chain) -> sp-stash leaked 96B/call -> guard-page crash.
    Fix: pin entryDepth = depth across the blr (stash pad slot [40]).
  - Controlled single-site repro recipe: PHARO_T1_SAVELESS_MIN_COMPILE=N
    emits saveless only in compiles seq>=N (find the target caller's
    seq with PHARO_T1_TRACE_COMPILE; cfib lands ~#4699 in the bench
    eval).  cfib->incc via saveless: CORRECT (val=2692537).
  - PERF: as implemented it LOSES to the save-push (510 vs 412 ms 10x
    cfib) — the 96B stash + entryDepth pin cost more than they save.
    Diet plan: x25-x28 are callee-saved AND absent from JIT_CALL's
    clobber list (JIT code preserves them; trampoline relies on
    x23/x24 likewise) -> carry caller receiver/sp/tempBase/ip in
    x25-x28 across the blr, shrink the stash to {x30, entryDepth} +
    cross fields, and skip the OFF_RECEIVER/TEMPBASE/IP restores
    (write-back from regs).  Target: beat the ~23-op save-push.
  - **COMPLETE (a978a7f6)**: the at-scale bug was ExitArithOverflow
    bails — ANY SmI arith in a leaf can bail; canSkipJ2JSave only
    excluded cond-jump bailers (confirmed exit=6 via brk trap; the
    controlled cfib->incc site just never overflowed).  The non-return
    RECOVERY STUB retro-builds the elided pool save (post-send ip from
    callerCM + compile-time bcOffset; resumeAddr=endOfSend) and RETs
    with the callee's exit state -> C++ materializes as if save-push.
    Full scope, no bisect gates: 3/3 clean startups, 0 DNUs; 10x cfib
    420ms vs 440 save-push; benchFib + battery correct.
    REGISTER DIET VERDICT (3-agent audit, 2026-06-10): NOT VIABLE as a
    local change.  T1 emits use none of x22/x25-x27 (free there), but
    TrampolineAsm.S PINS ALL of x21-x28 across its BLRs into JIT code
    (x22=save base, x25=localCalls, x26=localReturns — LOAD-BEARING:
    checkCountdown_ -= (calls+returns)*10 — x27=overflow limit,
    x28=nilOop).  A diet register set inside a trampoline-entered
    caller corrupts the loop state it RETs back into; saving/restoring
    around the blr re-adds the eliminated memory ops (net zero).  A
    true win needs trampoline re-homing (counters -> frame slots,
    x27 -> recompute from x22+imm) freeing x25-x27 zone-wide + adding
    them to JIT_CALL clobbers — ~+4 mem ops per trampoline round-trip
    (NOT cold: all non-leaf J2J) for -6 per leaf call.  Marginal and
    risky; SUBSUMED by the register-resident state.sp project (the
    structural fix for the ~55-instr-vs-Cog-12 per-call gap).  Audit
    artifact: workflows task w5s5eg5y3 output.
    Default-on gate: full-suite A/B on the Graviton box (in flight at
    write time — default config 12674/12724 = 99.6%%; saveless run +
    per-test diff pending).  Bisect gates (MIN_COMPILE / MAX_ARGS /
    NO_EXTRAS) remain for future shape isolation.
- docs/jit-retrospective.md "Cog-speed MAP" numbers now stale for
  cross-method; tight-loop 2x-faster-than-Cog and self-rec 6x still hold.
- **First-compile fail thrash: FIXED (c79b97ab + 9dbe6a49).**  All
  failures were unsuppPrim (PHARO_JIT_FAIL_REASONS=1).  TWO paths:
  (a) initialCompileFailed_ negative cache was cleared every
  recoverAfterGC -> per-GC retries (now a GC-visited, rehashed 16K-key
  array like countMap_); (b) THE dominant one: upgradeICToJ2J's eager
  compile of prim-bearing callees runs per ExitSendCached send with no
  cache -> per-SEND asmjit pipeline re-runs.  Combined: 83.5K -> 694
  failed attempts per bare startup (120x); suite batches were burning
  ~700 failed compiles/sec.

## Workflow artifacts (multi-agent investigation maps, this session)

- /private/tmp/claude-501/-Users-wohl-src-iospharo/a6ed987d-559a-4f18-a43d-7bf2905adc05/tasks/wqrxey8av.output
  (send-path map: bit consumers, compile triggers, ExitSendCached flow, gates)
- .../tasks/wn7p9ib48.output (save-stack lifecycle: GC, recompile/eviction,
  protocol diff — contains the remaining suspect list w/ likelihoods+tests:
  matRetSlot-ignoring ExitReturn pops at ~22693 & jit_loop_exit, stale
  bcStartCache after GC, recoverAfterGC ghost-version methodMap rebuild)

## Rules reminders
- **SUnit runner invocations: rm TWO stale files first** or the run
  silently does nothing (ate three 90-min A/B attempts 2026-06-09/10):
  1. /tmp/sunit_run_completed.txt — runner's done-marker, ALSO touched
     by every test_load_image EVAL run (the A3 fix); blocks auto-start
     ("skipping auto-restart", one easy-to-miss line).
  2. <imagedir>/startup.st (e.g. /tmp/harness/startup.st) — eval mode
     writes it and non-eval runs EXECUTE the stale one -> evaluates the
     old expr and quits the image seconds after startup (0 SUnitRunner
     lines, "Test Complete" almost immediately).
  3. /tmp/sunit_batch.txt is 1-BASED (`1 60`, not `0 60`): index 0 makes
     the runner's copyFrom:to: raise, SessionManager swallows it, and the
     run is silent — the error appears ONLY in <imagedir>/PharoDebug.log
     (always check that file when a runner run is silent).
  4. The optimized build burns the 60G step budget in ~4-5 min; set
     PHARO_MAX_STEPS=2000000000000 for suite runs (knob added 0a48a0e1).
- EVAL-RESULT prints to stderr — capture with 2>&1.
- grep -o "ms=..." matches the arg echo first; grep EVAL-RESULT first.
- New env knobs -> src/vm/debug_vars.h (DEBUG_BOOL + GET_DEBUG_BOOL).
- JITMethod field reads in emit code: offsetof() ONLY, never raw ints.
