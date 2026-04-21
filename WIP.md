# WIP — 2026-04-21

Current focus: pushing Δcog toward zero on the `jit` branch.

## Δcog progress (jit NO_JIT vs stock Cog on Pharo 13.1)

    2026-04-20     548   baseline before bug-11-layer-5
    2026-04-21a     27   JM_SIZE off-by-8 + other layer fixes
    2026-04-21b     19   bucket 15.A-C (paths, NFC/NFD, filesystem)
    2026-04-21c     21   watchdog-truncated rerun
    2026-04-21d     17   + decompiler NLR fix
    2026-04-21e     62   + finalization bucket — BUT +45 new Cly/ED regressions
    2026-04-21f    ≈60   + prim 188 method_ fix (closes OCSpecial + 2 Traits)

⚠️  **The 4-21e finalization commits caused a major net regression.**
    8 finalization tests pass, but ~45 Cly/ED tests now DNU with
    `AnObsoleteSlotTestsClassA` because finalization timing changes
    delay obsolete-class cleanup.  See memory
    `project_fin_commits_obsolete_class`.

    Options for next session:
      (a) revert finalization commits → Δcog ≈ 14 (clean win from prim 188)
      (b) find specific fin change that delays obsolete-class cleanup
          and adjust while preserving 8 fin test fixes

## Fixes committed this session (21 total)

Bucket 15.A — paths (4):
  - `test_load_image` uses `_NSGetExecutablePath` / `/proc/self/exe` for VMPath (25aa4f8).
    Fixes 3 SystemResolver tests + `testIsExecutable`.

Bucket 15.B — Darwin path normalization (2):
  - `primitivePlatToStPath` / `primitiveStToPlatPath` use `CFStringNormalize`
    NFC↔NFD on Apple (1c6d6e8).
    Fixes `testFromPlatformPath`, `testToPlatformPath`.

Bucket 15.C — filesystem primitives (4):
  - `primitiveDirectoryCreate` / `primitiveFileRename` fail with `osErrorCode_=errno`
    instead of returning `false` (e517a3e).  Lets image-side raise
    `DirectoryExists` / `DirectoryDoesNotExist` exceptions.
  - `primitiveReaddir` populates statAttributes via `fstatat` (c380165).
    Fixes `testEntriesHaveAttributes`.

Bucket 15.E — finalization / weak (9):  ← biggest session chunk
  - `primitiveFullGC` arms `finalizationCheckAfterGC_` instead of signaling inline.
    Consumption moved to END of `activateMethod` after setup is complete.
    (39bfe98, 95b0c03).
  - `signalFinalizationIfNeededDeferred`: non-preempting variant for `primitiveWait`
    (55f68eb).  Preserves `put-self-on-waitlist` atomicity vs FP preemption.
  - `drainMournQueueNatively` filter-drains: WKAs in C++, non-WKAs re-pushed for
    image-side prim 172 dispatch (6efe091).  The previous drop-on-floor behavior
    starved `ObjectFinalizer` / `FinalizationRegistryEntry` finalizers.

Bucket 15.F — decompiler NLR (2):
  - `activateBlock`'s home-method lookup falls back to closure `outerContext.method`
    when the static chain (block's last-literal) isn't on `savedFrames_` (5b59f1c).
    FBIRBytecodeDecompiler reuses the original block as a literal of the regen
    method, so the block's outerCode points at the ORIGINAL method — home lookup
    via static chain would miss the regen's frame.

Bucket 15.F — OCSpecialSelector (1):
  - `primitiveExecuteMethodArgsArray` (prim 188 3-arg mirror) no longer restores
    `method_` after a nested primitive that activates a new frame (56b0fb8).
    Prim 207 (primitiveFullClosureValue for BlockClosure>>value) is the common
    case.  Minimal repro: `(BlockClosure >> #value) valueWithReceiver: blk
    arguments: #()` crashed with "only integers should be used as indices"
    on the old binary; returns the block value with the fix.  This is the
    OCCalledMethodProxy path that OCSpecialSelectorTest>>testUnoptimised
    ValueSpecialSendsMessageCapturesSend exercises.

Harness (not a Δcog count, but defensive):
  - `setup_fake_gui.st` patches OCCalledMethodProxy with `#selector` / `#methodClass`
    delegating to `originalMethod`; forces `Symbol selectorTable` to build at
    startup (39929ea + submodule bump).

## Known-remaining Δcog entries (likely 6-8)

Real VM bugs:
  - ~~OCSpecialSelector~~ FIXED 2026-04-21 (56b0fb8) — prim 188 was
    restoring `method_` after a child activating prim (207); with the
    restore disabled for frame-depth-changed cases the full chain works.

Bug-14 family (scheduler):
  - `FFICallbackParametersTest>>testCharacterParameters` — test-ordering
    artifact; passes in isolation.
  - `FFICallbackParametersTest>>testFloatParameters` — hangs after the 1st
    FFI callback due to pri-60 runner ↔ pri-80 delay-ticker ping-pong.
  - `FFICallbackParametersTest>>testIntegerParameters` — same family.

Test-ordering artifacts (pass in isolation, fail only in full sunit):
  - `ClassQueryTest>>testAllCallsOn`
  - `ProcessMonitorTestServiceTest>>testFailTest...`
  - `TestExecutionEnvironmentTest>>testHandleForkedProcessesByAllServices`
  - `TraitTest>>testTraitsMethodClassSanity`
  - `TraitTest>>testTraitsUsersSanity`

Possibly new:
  - `EDDebuggingAPITest` / `EDEmergencyDebuggerTest` entries seen in prior runs,
    may reappear in 4-21e.  Usually ordering artifacts.

## Open bug list (see `docs/jit-uncovered-bugs.md`)

  Bug 14     jit-default idle-hang (scheduler preemption in DelayMicrosecondTicker
             re-arm path).  Root cause for FFI callback ordering, above.
             Needs focused multi-day session.

  Bug 15.D   ~~FFI callback return-value marshalling~~  reclassified to bug-14
             scheduler ping-pong after diagnostic.

  Bug 15.F residual  OCSpecialSelectorTest, above.

## Next targets

1. **Fix the finalization obsolete-class cleanup delay.** The four
   finalization commits (39bfe98, 55f68eb, 95b0c03, 6efe091) deferred
   FinalizationSemaphore signaling to activateMethod/primitiveWait
   entry.  Net effect: `ObjectFinalizationService` (P50) no longer
   wakes promptly, and WeakFinalizerItem cleanup of `AnObsolete*`
   classes is delayed across test boundaries.  The obsolete classes
   persist in `Metaclass>>subclassesDo:` iteration, poisoning every
   later test that walks the class hierarchy.
   Suspect: the `signalFinalizationIfNeededDeferred` non-preempt
   variant — previously `synchronousSignal` transferTo'd FP
   immediately, so the finalizer ran at once.  Now FP waits in the
   ready queue until the scheduler picks it.  Between SlotIntegration
   tests that's apparently "never."
   Two possible fixes:
     (a) keep activateMethod as the signal site but ALSO call
         signalFinalizationIfNeeded (preempting variant) between
         test cases — requires harness cooperation.
     (b) re-enable the preempting signal for primitiveFullGC only
         when the FP queue is non-empty AFTER the drain (i.e. only
         when a finalizer was enqueued that needs prompt execution).

2. Bug 14 investigation (FFI scheduler deadlock + jit-default hang
   share the same root cause).  Cuts 4 Δcog entries + unblocks
   jit-default.  Deferred — multi-day effort.

3. Check if prim 188 fix also unblocks any other tests (some
   reflection / DoIt paths use valueWithReceiver:arguments:).

## Notable decisions / reverts this session

  7c2fda3, 2dad9bc, 4b4f7f8 — finalization queue split/signal attempts.
  Reverted in 536e60c because they broke testClearing; superseded by the
  3-part fix above which threads the needle correctly.
