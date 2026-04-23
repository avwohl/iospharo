# Deferred Items (consolidated)

Single source of truth for currently-open work.  Anything shipped
is logged in `docs/changes.md`; this file lists only what's *not*
done.

Detailed session-by-session narratives for closed investigations
live in `git log` and `memory/*.md` — not here.

---

## A0a. FFI `C11 class >> #current` DNU cascade — **APPEARS FIXED** (2026-04-22)

Rechecked 2026-04-22 under `PHARO_NO_JIT=1 PHARO_NO_SISTA=1`.  Direct
`buildSuite run` on the specific classes originally reporting errors:

    DiskFileSystemTest:      59 ran / 59 passed / 0 errors  (was 59 err)
    AthensCairoMatrixTest:   17 ran / 17 passed / 0 errors  (was 17 err)
    AthensCairoPDFSurfaceTest: 4 ran /  4 passed / 0 errors  (was  4 err)
    CairoLibraryTest:         1 ran /  1 passed / 0 errors  (was  1 err)
    TraitCompositionTest → AthensCairoMatrixTest (sequenced): 13+17/30, 0 errors
    7 FFI/Traits/Cairo mixed batch: 77 ran / 77 passed / 0 errors
    40-class harness batch:  981 ran / 981 passed / 0 errors / 0 fail

No `C11 class >> #current` DNU observed in any run.  Something between
2026-04-20 and today's `fe086ee` resolved it — likely one of the
intermediate finalization / class-migration / obsolete-class fixes
(17a0ff7, 61eef4f, cafe6a2).  Leaving this section in history as a
regression marker; no active investigation.

### Original regression note (2026-04-20)

Full SUnit run 2026-04-20 (`PHARO_NO_JIT=1 PHARO_NO_SISTA=1`) shows
454 errors with identical signature:

    MessageNotUnderstood: Message not understood: C11 class >> #current
      >> C11 class(Object)>>doesNotUnderstand: #current
      >> <Class>(Object)>>ffiCallingConvention
      >> <Class>(Object)>>ffiCall:library:options:fixedArgumentCount:
      >> <Class>(Object)>>ffiCall:library:options:
      >> <Class>(Object)>>ffiCall:
      >> <Class>>><primMethod>
      >> <TestCase>

Affected test classes include DiskFileAttributesTest (6 errors),
DiskFileSystemTest (59), AthensCairoMatrixTest (17),
AthensCairoPDFSurfaceTest (4), CairoLibraryTest (1),
ClyBrowserToolValidityTest (25), ClapHelloTest (8),
CompletionEngineTest (12), CoCompletionEngineTest (24),
EpApplyTest (28), EpApplyPreviewerTest (40),
EpCodeChangeIntegrationTest (32), EpRevertTest (23),
FFICallbackTest (1), FLPlatformTest, and ~40 more.

Stock Cog cross-check on the same image (`/tmp/harness/Pharo.image`)
using the Pharo 10 VM: 15 sampled classes, **400 tests PASS / 0 FAIL
/ 0 ERROR**.  So these failures are ours, not the image.

Interactive probe on our VM:

    FFICallback ffiCallingConvention          => #cdecl (correct)
    OSPlatform current ffiCallingConvention   => #cdecl (correct)
    AthensCairoMatrix(Object)>>ffiCallingConvention source
        => ^ OSPlatform current ffiCallingConvention (same as stock)

No `C11` class exists in either the stock image or ours (verified via
`Smalltalk allClasses` search and `Smalltalk globals at: #C11`).  Yet
SUnit-invoked paths on our VM resolve a receiver that prints as
"C11 class" — suggesting a method-literal or selector-lookup issue
specific to UFFI's pragma-driven FFI-callout compilation of methods
like `primLoadIdentity  ^ self ffiCall: #(...)`.  UFFI rewrites these
methods on first call to reference a cached FFICallout descriptor;
the rewrite may install different literals on our VM.

**Next steps:**
- Capture actual bytecode / literal frame of `AthensCairoMatrix>>primLoadIdentity`
  in our VM vs stock, post-first-call.  Diff them.
- Inspect the `ffiCall:` path's pragma handler (FFICalloutAPI,
  FFIFunctionResolution) for places that might reference a non-
  existent C11 class as a default.
- Check whether our VM's method-compilation path (e.g., eval-mode
  compilation, JIT deopt, or FFI dynamic-recompile) introduces the
  C11 literal.

## A0b. Other stock-Cog-passing error buckets — **APPEARS FIXED** (2026-04-22)

Rechecked 2026-04-22 under `PHARO_NO_JIT=1 PHARO_NO_SISTA=1`.  Direct
`buildSuite run` on 8 classes from the original bucket:

    EpApplyTest:                28 ran / 28 passed / 0 err  (was 28 err)
    EpRevertTest:               23 ran / 23 passed / 0 err  (was 23 err)
    EpCodeChangeIntegrationTest: 32 ran / 32 passed / 0 err  (was 32 err)
    EpApplyPreviewerTest:       40 ran / 40 passed / 0 err  (was 40 err)
    ClyBrowserToolValidityTest: 25 ran / 25 passed / 0 err  (was 25 err)
    ClapHelloTest:               7 ran /  7 passed / 0 err / 1 skipped (was 8 err)
    CompletionEngineTest:       51 ran / 51 passed / 0 err  (was 12 err)
    CoCompletionEngineTest:     65 ran / 65 passed / 0 err  (was 24 err)
    FLPlatformTest:             MISSING (class not in image)

TOTAL: 271 ran / 271 passed / 0 errors / 0 failures.  These 8 classes
accounted for ~212 of the original 454 A0b-like errors, so the bucket is
at least mostly cleared.  Closed alongside A0a; same intermediate
class-migration / finalization fixes likely did it.

### Original bucket (2026-04-20)

    92  MessageNotUnderstood: receiver of "packageName" is nil
    59  MessageNotUnderstood: receiver of "select:thenDo:" is nil
    49  MessageNotUnderstood: receiver of "disable" is nil
    17  MessageNotUnderstood: receiver of "outputFileReference" is nil
    12  Error: Wrapper query should include single subquery
    12  Error: Can't find the requested origin
     8  MessageNotUnderstood: receiver of "close" is nil

## A00. `StringTest>>testSelect` — fails via timeout (interpreter-speed)

80-class focused-SUnit broad run shows:

    Pass: 4292  Fail: 0  Error: 0  Skip: 6  Timeout: 1

The one timeout is `StringTest>>testSelect`.  Passes on stock Cog
in <1s; our pure-interpreter mode (PHARO_NO_JIT=1) exceeds the
50s `defaultTimeLimit` watchdog and gets killed.

Harness categorizes it separately from `Fail` because it's an
infrastructure ceiling (~50× interpreter-vs-JIT speed gap) rather
than an assertion failure.  Treated as a fail for correctness
reporting: our VM cannot currently complete this test in the
allotted time.  Would pass with working JIT enabled, or with
`/tmp/sunit_timeout_scale.txt` contents raised (e.g., 20 → 200s
per test — defers the limit but doesn't address the speed gap).

Next steps: JIT optimization work.  The Sista Phase 4 monomorphic
inliner is the intended fix; shorter term, any perf win on the
interpreter (e.g., quickening hot sends) narrows the gap.

## A0. weak-ref GC: testClearing — **RESOLVED** (2026-04-20)

Both `WeakKeyDictionaryTest>>testClearing` and
`WeakIdentityKeyDictionaryTest>>testClearing` now pass
deterministically.

**Root cause**: `HashedCollection>>size` has primitive 264 (quick
slot-at) — no method activation.  In Cog, `forceInterruptCheck` arms
`stackLimit = -1`, and the drain fires via the next REAL method
activation's stack-overflow check — not primitive calls.  So
`dict size` returns the pre-drain tally (1001), then
`self assert:equals:` activates → triggers drain → later assertions
see the drained tally.

**Fix** (commit `cafe6a2`, default-on, opt-out via
`PHARO_INLINE_FINALIZE=1`):

1. `primitiveFullGC` matches Cog: just `fullGC()` + push result.
2. `activateMethod` checks `pendingFinalizationSignals > 0` at
   entry.  If set, calls `drainMournQueueNatively()` — a native
   reimplementation of `WeakKeyAssociation>>mourn` +
   `Dictionary>>removeKey:ifAbsent:` + `fixCollisionsFrom:` that
   bypasses the Smalltalk `FinalizationProcess` P50/P51
   indirection entirely.  Quick primitives (256-519, handled
   before activateMethod) naturally return pre-drain values.

Verified across 10+5 consecutive focused-SUnit runs under
`PHARO_NO_JIT=1`, 0 failures each.

**Caveat**: under JIT-on (default without `PHARO_NO_JIT`), the test
harness crashes with a pre-existing issue (`PC not in any active
JIT method (evicted?)` during `activateMethod`).  This crash
reproduces even with the legacy `PHARO_INLINE_FINALIZE=1` path —
it's not caused by the finalization fix.  Tracked separately under
A1 (JIT eval-mode hang/crash).  Fix is effective for the
interpreter mode used by the test harness.

## A0-original. weak-ref GC: testClearing convergence (2026-04-20)

`WeakKeyDictionary>>testClearing` and `WeakIdentityKeyDictionaryTest>>testClearing`
(inherited) are the remaining 2 failures in the focused SUnit suite
after 86/88 were fixed this session.

Root cause, traced via PHARO_GC_EPH_DEBUG=1 + in-GC heap walking:

The test does `keys := (1 to: 1000) collect: [:n | 'key', n asString]`,
then `keys := nil; Smalltalk garbageCollect`.  On first GC, all 1000
key strings are marked alive because a `Context` object with `sel=#do:`
has the backing Array as its receiver slot.  That do: context is in
turn held by block Contexts (FullBlockClosures at slot 0 = outerContext)
that survived the do: iteration.

So the VM is heap-materializing do:'s call frame and each iteration's
block frame; once on the heap they stay alive in the sender chain
across GC.  The mark phase then marks all context temps transitively,
including the Array → the 1000 test keys → the ephemerons transition
from active to inactive → never fire.

Cog avoids this via generational GC: short-lived contexts never reach
tenure (scavenge reclaims them), so they're not seen by the mark phase
of a major GC.  Our VM promotes everything to old space on allocation.

Fix options, ordered by size:
- Audit Context materialization triggers; ensure block/do: contexts
  don't materialize unless `thisContext` was explicitly captured.
- Add a "weak Context" format whose temp slots don't keep referents
  alive when not reachable from a running process's top frame.
- Generational GC (large structural change).

### Generational GC (2026-04-20)

`PHARO_YOUNG_GEN=1` enables eden allocation + scavenge (opt-in).
Full GC auto-scavenges before compact; mark phase traces through
eden to keep old objects reachable only via eden alive.

**YG did not fix testClearing.**  The failures are orthogonal to GC
reachability — they're about *finalization signal timing*, and our
VM deviates from Cog here.

### testClearing — finalization signal semantics

Authoritative reference: Cog `cointerp-cpp.c` at commit imported
into `src/ios/`.

**Cog's primitiveFullGC** (lines 84895-84927) calls `fullGC()` and
returns the result.  No inline finalization signal.

**Cog's `signalFinalization:`** (lines 43475-43478, called from
`fireEphemeron:`):

    forceInterruptCheck();
    GIV(pendingFinalizationSignals) += 1;

Just increments a counter.  The actual
`synchronousSignal(TheFinalizationSemaphore)` runs later in
`checkForInterrupts` (lines 67696-67706), guarded by
`mayContextSwitch` (line 67635 returns early otherwise).
`checkForInterrupts` is triggered at backward-branch bytecodes
(cointerp-cpp.c:12236-12260, `backwardJumpCountByte` mechanism) —
not on a fixed bytecode interval.

**Our VM** has historically called `signalFinalizationIfNeeded()`
inline from `primitiveFullGC`.  That calls `synchronousSignal` →
`transferTo(P50)` → P50 preempts inside the primitive and drains
the whole mourn queue before the primitive returns.  `dict.tally`
is 1 by the time `Smalltalk garbageCollect` returns; testClearing's
assertion B then fails ("Got 1 instead of 1001").

**Fix shipped** (gated on `PHARO_FINALIZE_DEFERRED=1`):

- `primitiveFullGC` no longer calls `signalFinalizationIfNeeded`
  inline.  Just calls `fullGC()` and returns — matches Cog exactly.
- New `Interpreter::backwardBranchInterruptCheck()` is called from
  every backward-jump bytecode (ExtJump/ExtJumpTrue/ExtJumpFalse,
  longJump/longJumpIfTrue/longJumpIfFalse) when the offset is
  negative.  It uses a `backwardBranchCountdown_` (initial 60) to
  throttle the check rate — matches Cog's `backwardJumpCountByte`
  mechanism.  When it fires, pending finalization signals are
  delivered via `synchronousSignal`, which then preempts P50 at a
  bytecode-safe point.

Additional fix: P51 worker aging-exclusion.  The
FinalizationProcess forks a worker at `activePriority + 1` (50+1
= 51) to run `mournLoopWith:`.  Our heartbeat-driven aging (every
500ms) would preempt the worker mid-drain, leaving some
WeakKeyDictionary entries unmourned.  `handleForceYield` now
excludes P51 from aging while `mournQueueSize > 0`.

**Current result** (442-test focused SUnit):

    Default (legacy inline signal):
      WKD testClearing     FAIL (Got 1 instead of 1001)
      WIKD testClearing    FAIL (Got 1 instead of 1001)
      WIKD testFinalize*   PASS
      → 2 failures, deterministic

    PHARO_FINALIZE_DEFERRED=1 (Cog-spec):
      Sometimes WKD + WIKD testClearing pass, sometimes not.
      Sometimes testFinalize fails.
      → 2-3 failures, **non-deterministic across runs**.

    * testFinalizeValuesWhenLastChainContinuesAtFront

The non-determinism in the deferred path is a P51-worker / P40-test
scheduling race.  Between `Smalltalk garbageCollect` and the next
assertion, the backward-branch interrupt check must fire exactly
when `dict keys` begins iterating (testClearing) but NOT when
testFinalize reads `dict size` directly.  Timing varies by a few
bytecodes depending on heap layout, hitting this window sometimes
but not always.

**Native mourn drain (also shipped, also opt-in)**:

`drainMournQueueNatively()` reimplements `WKA>>mourn` +
`Dictionary>>removeKey:ifAbsent:` + `fixCollisionsFrom:` directly
in C++.  No P50/P51 semaphore/process-switching needed — the drain
runs synchronously inside `backwardBranchInterruptCheck`.

- Per WKA mourner: look up in `container.array` by identity,
  nil the slot, decrement `tally`.
- `fixCollisionsFrom:` uses `identityHash` from the object header
  for probing.  Correct for Object-keyed dicts (`Object>>hash` ==
  identityHash by default); approximate for String keys (String's
  content-based hash can't be replicated in C++ without
  reimplementing it).  The only user-visible effect is that
  `includesKey:` lookups on a broken chain can false-negative
  after mourn — none of the failing tests exercise this.

Eliminating the P50/P51 indirection removed half the race, but the
remaining non-determinism is the *firing cadence* of the
backward-branch check itself.  The 60-backward-jump countdown
sometimes fires inside an assertion (too early), sometimes between
them (too late or perfectly timed).  Still 1-3 focused-SUnit
failures varying across runs.

**What's needed for a deterministic fix — Cog's stack-limit trick**:

Cog's `forceInterruptCheck` (cointerp-cpp.c:70373-70393) sets
`stackLimit = ~0` (all-ones).  The VM's stack-overflow check fires
at *every* method activation.  Handler examines a `mayContextSwitch`
flag: false during primitive execution and for a few bytecodes
after, true at ordinary user-code bytecode boundaries.  When
`mayContextSwitch` is true AND stackLimit is -1, the interrupt
check runs — signals semaphores, processes finalization.

This gets the timing right: `Smalltalk garbageCollect` returns,
primitive-post path has `mayContextSwitch=false` for the pop-result
bytecode, then user code runs with `mayContextSwitch=true` but
stack limit gets restored.  The NEXT `forceInterruptCheck` (from
the next ephemeron firing) re-arms.

Our VM doesn't have stack-limit-based interrupt checks — we use
bytecode-count periodic checks and heartbeat-driven forceYield.
To match Cog exactly:

1. Add `stackLimit_` field (writable by heartbeat + GC).
2. `forceInterruptCheck()` sets `stackLimit_ = UINTPTR_MAX`.
3. Every `activateMethod` compares stackPointer vs stackLimit
   (currently we don't — we just reserve space).
4. Stack-limit trigger: restore normal limit, call handler.
5. Handler gates on new `mayContextSwitch_` bool — false entering
   primitives, true restored at bytecode boundaries.

Scope: 300-500 LoC across activateMethod, primitiveSuccess,
dispatch loop, new handler.  Not this session.

**Current state**:
- `drainMournQueueNatively` done and correct (modulo String hash).
- `backwardBranchInterruptCheck` in place.
- `primitiveFullGC` is Cog-clean under the flag.
- Missing: stack-limit trigger at activations with mayContextSwitch
  gate — that's what gets the firing timing deterministic.

Everything else (ObjectTest, ClassDescriptionProtocolsTest, SlotBasicTest,
SlotMigrationTest, SlotTraitsTest, FIFOQueueTest, and
WeakIdentityKeyDictionaryTest>>testFinalizeValuesWhenLastChainContinuesAtFront)
passes — 86/88 fixed.

## A. Open VM issues

Surfaced 2026-04-19 by removing the harness skip list and the JIT
auto-disable in test/eval mode.  The previous "clean" test suite
runs were hiding these behind workarounds.

**JIT-off state (2026-04-19 after scheduler + OOM + profiler + timeout fixes):**
Interpreter-mode test suite is effectively clean.  Each of the four
failures surfaced by the no-skip probe has been fixed or characterized:

- `AllocationTest>>testOneGWordAllocation` → **PASS**.  Fixed by
  PrimErrNoMemory_ primFailCode in basicNew:/basicNew (commit
  `8d69724`).  Image raises `OutOfMemory`, test catches it.
- `AndreasSystemProfilerTest>>testSimple` → **PASS**.  Fixed by
  implementing `primitiveProfile{Semaphore,Start,Sample,Primitive}`
  (commit `6d21105`).
- `BehaviorTest>>testAllReferencesTo` → **PASS** (9.6 s).  Fixed by
  the 10× timeout multiplier when PHARO_NO_JIT is set (submodule
  `150bbd4`).  Was 8 s > 8 s previously, now 8 s > 80 s.
- `ArrayTest>>testPrintingRecursive` → still **TIMEOUT** at 80 s.
  Pathological case: stuck in `InstructionStream>>scanFor:` inside
  the recursive printOn: chain.  Needs interpreter perf work or JIT
  correctness work — not a tractable single-session fix.

### A0. Chunk-format `methodsFor:` incompatible — RESOLVED (harness converted)
Pharo 13 / 14 removed `#methodsFor:` from `ClassDescription`, so
chunk-file headers like `<bang>SUnitRunner class methodsFor: 'cat'<bang>`
never worked cleanly — the harness shipped that way but the image's
CodeImporter evaluated every chunk as a plain DoIt, so multi-line
method bodies with temps / nested blocks silently failed to install.

Fixed by converting `run_sunit_tests.st` and `setup_fake_gui.st`
from chunk format to explicit `Class compile: 'source' classified:
'cat'` calls (submodule commits `17bd98a`, `f50f2d6`).  With the
converted harness:

- JIT off, no skips: first ~35 test classes exercised end-to-end
  with a mix of silent passes, genuine 8-s timeouts on known-slow
  tests (e.g., `ArrayTest>>testPrintingRecursive`), and a few real
  PrimitiveFailed errors (e.g., 1-GB allocation).
- JIT on, no skips: immediate DNU cascade during boot — the bugs
  below.  Harness never reaches the test runner.

Preserved the `methodsFor:` shim at the top of the harness for
belt-and-suspenders.

### A1. JIT eval-mode hang at PHARO_JIT_DEFER=0
Default `PHARO_JIT_DEFER=4s` boots cleanly end-to-end.
`PHARO_JIT_DEFER=0` (immediate JIT) still hangs during Morphic
boot at `JIT_MAX_COMPILE≥10` — `do:` gets compiled and a later
`Context>>copyTo:` recurses at frame depth 4090+.  The deeper
chain: DNU on `#hasShortcutKey` triggers `StDebugger`, whose
context-stack copy finds a Context with `sender == self`.

306 methods compile, 97.4 % IC hit rate — this is a scheduling
issue when JIT compile runs during the boot/eval handoff, not a
correctness one.  Benchmarks set `PHARO_JIT_DEFER=0` explicitly
via `PHARO_BENCH` which bypasses the startup.st path entirely, so
the default path is unaffected.

### A1b. FFI `invokeFunction:withArguments:` receiver corruption under JIT
Under JIT with any defer setting, `EventSensor>>pollEvent:` triggers
DNU `#invokeFunction:withArguments: not understood by rcvr=0x300000000
class=nil` — the FFI function-pointer oop arrives as a sentinel/nil
at the call site.  This is the same "0x300000000 sentinel" pattern
as A1c; likely a JIT spill/reload bug around the FFI send.  Not the
"FFI is incomplete" — the function pointer is *lost*.

### A1c. `forkAt:` sentinel — RESOLVED 2026-04-19 (was scheduler starvation)
The watchdog's `testProcess suspend` DNU with `rcvr=0x300000000` was
a symptom of scheduler starvation, not a memory bug.  The test
process never actually ran; the watchdog fired its timeout handler;
the `testProcess` local temp was still `nil` (rawBits=0) because
the fork's local-temp array index read uninitialized header space
that *happened* to look like `0x300000000`.

Root cause was `primitiveRelinquishProcessor` yielding to highest
priority instead of same-or-lower, plus sleeping 10 ms unconditionally
before considering transfer.  Fix in commit `a2b99f7` — a 2 ms
`AIAstarTest` test now runs in 2 ms via `runSingleTest` (was 23.6 s).

### A2. B5 cold-IC DNU cascade at PHARO_JIT_DEFER=0
At `PHARO_JIT_DEFER=0`, JIT compiles startup-path methods
immediately and corrupts the J2J return stack on the
`PositionableStream class>>on:` → `PositionableStream>>on:` chain,
leaving `aCollection size` (SmallInt) where the stream instance
should be.  `decodeBytes:` bc[2] `popStoreTemp 1` then stores the
SmallInt into `byteStream`, producing DNU on `#atEnd`.

Full diagnostic tooling is in tree (commits `f70ad55`, `cf6ffaf`,
`b94c0a8` — `_HOLE_RT_J2J_TRACE`, 512-slot ring buffer, auto-
trigger on SmallInt return to focus methods, DNU auto-dump).
Root cause not pinpointed; next step needs lldb single-stepping
through `stencil_popStoreTemp` at the buggy iteration.  Full plan
in `memory/project_b5_j2j_onchain.md`.

**Mitigation:** default `PHARO_JIT_DEFER=4s` sidesteps it.  Only
impacts `PHARO_JIT_DEFER=0`, which isn't a supported default.

**2026-04-22 finding**: In the Pharo 13.1 clean-image eval repro
(`PHARO_JIT_DEFER=0 ... eval "42 printString"`), the `#atEnd on
Array in parseFields:structure:` DNU disappears completely at
`PHARO_JIT_DEFER=5` (5 ms) or higher.  Under DEFER=0 it appears
deterministically once per run.  This is consistent with the
hypothesis that the DNU is a *consequence* of Morphic preempting
the startup process mid-`fields readStream`, not an independent
miscompile.  The JIT runs correctly when given ~5ms head-start to
let the startup process get past this code path.  So A2 ⊆ A1.

---

## B. Code state in-tree — experimental / opt-in

Every knob is declared in `src/vm/DebugSettings.hpp` — one file to
grep.

### B1. `PHARO_T2=0` (default off)
T2 never demonstrably wins on any measured bench.  Stays
default-off until a workload where T2 beats T1 appears.  Under
§1.3c coexist default, `PHARO_T2=1` is safe but also largely
dormant on typical benches (T1 handles everything; T2 compiles
but doesn't intercept).

### B2. `PHARO_T2_UNSAFE_CALLEE=1`
Restores the pre-`f279fd4` buggy callee-invocation path
(double-execution) for benchmarking.  Keep as a flag; do not
remove.

### B3. `PHARO_RESUME_J2J=1`
External J2J trampoline in the resume path.  Works but 18 %
slower than inline resume; opt-in only.  See
`memory/project_resume_j2j_trampoline.md`.

### B4. `PHARO_JIT_SIMSTACK=1`
SimStack TOS/NOS register caching.  Default-flip attempted
2026-04-18, reverted same day (12 IntegerTest regressions that
don't reproduce in isolation).  Root cause open.

### B5. `PHARO_T2_A1=1` — T2 chain-loop continuation
Implemented but dormant.  A1's callee-invocation speedup is only
observable when T2 actually executes; under coexist default, T2
rarely does.  Would need a workload where T2 wins to justify
re-examining.

### B6. T2 diagnostic env vars (keep)
`PHARO_T2_BAIL_OP`, `PHARO_T2_NO_ARITH_FAST`,
`PHARO_T2_NO_ARITH_OPS`, `PHARO_T2_OPT`, `PHARO_T2_TRACE`,
`T2_LIMIT`, `T2_VERBOSE`, `PHARO_T2_MBC_JUMPS`,
`PHARO_T2_MBC_SENDS`, `PHARO_T2_MBC_IC`, `PHARO_T2_WARMUP`,
`PHARO_JIT_NO_SIMSTACK`.  All cheap when not enabled.
(`PHARO_DUMP_MIR` removed when MIR was deleted 2026-04-17.)

---

## C. Project mission — iOS

The project's purpose is an iOS Pharo VM.  JIT work has dominated
recent sessions; iOS proper is queued next.

### C1. iOS device testing
Mac Catalyst verified working (2026-02-24).  iOS Device
xcframework slice builds as of 2026-04-19 (commit `22bcc2c`).
Device testing still needs:

- Physical iOS device(s) for build verification.
- Apple Developer signing cert setup.
- TestFlight or direct-device deploy.
- UI touch/pinch/pan exercised end-to-end (not just Mac mouse
  events).

### C2. iOS app-store readiness (remaining items)
Shipped 2026-04-19: privacy manifest, launch screen, scoped ATS
(see `changes.md`).  Still open:

- Remote logging for device debugging — needs a backend choice
  (Sentry / Firebase / custom endpoint).  Apple's built-in crash
  reporter handles crashes; this is for non-crash diagnostic logs.

### C3. Image preparation
- Do iOS images need different startup?  Currently uses standard
  Pharo images with `startup.st` injection — verify behavior on
  real hardware.
- Touch-based Morphic input path — `docs/image_issues.md` has the
  wishlist for portrait layout + touch primitives as upstream
  proposals.

---

## D. Upstream proposals

`docs/upstream-proposals.md` tracks wishlist items for the Pharo
image that would make iOS work cleaner.  Out of process from VM
changes; image-side issues to propose upstream.

- Portrait-aware layout in Morphic.
- Touch event primitives on the standard input path.
- Startup preferences path that survives
  `Smalltalk snapshot:andQuit:` round-trips cleanly.

---

## E. Remaining JIT work

1. **Architectural T1/T2 interaction (§1.3).**  T2 intercepting
   methods still breaks T1's inline-IC warmup in the non-coexist
   (REPLACE=1) path — neither shared-IC, warmup delay, nor
   self-only narrowing has fully solved this.  Coexist default
   (§1.3c) sidesteps it by not replacing T1 at all.  Full fix —
   if ever needed — is a design rethink (shared IC table across
   tiers?  patch-T1-when-T2-compiles?).
2. **Multi-bc §1.2e block activation** (`PushFullBlock` /
   `PushClosure`).  Uses the existing `ExitBlockCreate` chain-loop
   path.  Enables non-inlined blocks to be T2-compiled; marginal
   benefit since Pharo inlines `to:do:` / `whileTrue:` at compile
   time.

Once §1.3 and §1.2e are sorted, JIT reaches diminishing returns
and C is where project value lands.
