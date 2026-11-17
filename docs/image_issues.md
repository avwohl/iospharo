# Pharo 13 and Pharo 14 image issues we patch

This is the consolidated list of bugs in the standard Pharo 13 and Pharo 14 images
(`/tmp/harness/Pharo.image`, fetched via `https://get.pharo.org/64/130`)
that we need to patch locally for our VM to work the same way as
stock Cog.  See also `docs/vm-compat-bugs.md` for open VM-side defects, and
`docs/deferred.md` for intentional platform gaps.


## TraitFileOutTest depends on IdentitySet order (2026-10-17, not patched)

Sweeps L73, L74 and L75 each failed TraitFileOutTest (one timeout, then
`MessageNotUnderstood: TraitedMetaclass >> #getName`).  The test passes
alone and in its own batch, run the sweep's way, 8 of 8 times.  The stack:
`Package>>removeTag:` removes trait TD; `Trait>>removeFromSystem` walks
TD's class-trait users, an IdentitySet, and for a user that is not yet
obsolete calls `removeFromComposition:`.  The remake then sends
`getName` to a TraitedMetaclass, which only classes understand.  Whether
that branch runs depends on the set's iteration order, so on identity
hash values.  Our identity hashes come from a fixed-seed generator, so
the order changes only when process timing changes which objects get a
hash first.  The defect is in the image's removal order, not in the VM.

## TaskIt worker pool kills a busy worker (2026-11-12, repaired 2026-11-17)

Repair of 2026-11-17: `scripts/image-fixes/25-taskit-worker-pool-race.st`.
The pool now stops a worker only when the process of the worker waits on the
semaphore of the task queue (`AtomicSharedQueue>>isProcessWaiting:`).  A
process in that state holds no task.  The pool takes the process out of the
wait in the same step (`suspendIfWaitingForTask`): the test and the suspend
run at the highest priority, so the worker cannot leave the wait between
the two.

A first repair tested two facts, "the worker is free" and "the process of
the worker waits".  That repair left 4 failures in 3000 runs.  The event log
of `scripts/cogdiff/taskit_pool_diag.st` showed the cause: between the two
reads a worker took a task, started the task and then waited for the mutex
of the pool.

A second repair tested one fact, "the process waits on the semaphore of the
queue", and then stopped the worker.  That repair is correct while each
signal of the queue comes under the mutex of the pool.  The repair of the
queue (the next section) makes a reader give a signal too, so the test and
the suspend became one step.

Failures of the test in a loop, with a process at priority 60 that wakes
each millisecond (`scripts/cogdiff/taskit_pool_stress.st`):

    our VM, before the repair        83 in 600 runs
    stock Cog, before the repair      2 in 600 runs
    our VM, first repair              4 in 3000 runs
    our VM, second repair             0 in 3000 runs
    stock Cog, second repair          0 in 600 runs

The same defect made `TKTFutureTest` fail one time in the sweep of
2026-11-17 ("Got nil instead of 5"): the pool of the test profile stopped
the worker that held a callback.

A second cause of that failure text is in four tests, not in TaskIt.
`testFutureFirstCompleteOfSuccessIfFirstSuccessFaster` and three tests like
it give the two futures their results one after the other, and expect the
result of the first.  The two callbacks are tasks of a pool with 4 workers,
so the callbacks have no fixed order.  The entries of
`26-taskit-first-complete-tests.st` make each test wait for the result of
the combined future before the second future gets its result.  Our VM: 7
failures in 900 runs before, 0 in 1500 after.

The text below is the analysis of 2026-11-12.

### AtomicSharedQueue loses a wake-up with two readers (2026-11-17, repaired)

Found by a review of the TaskIt repair.  A TaskIt worker pool is an
`AtomicSharedQueue` with many readers.  The repair is
`scripts/image-fixes/27-atomic-shared-queue-wakeups.st`, with a new test
class `AtomicSharedQueueTest`.  The released image has no test class for
this queue.

- A reader that wakes for a second item can find the queue locked by a
  first reader (`WaitfreeQueue>>nextIfNone:` locks the queue for one read).
  The second reader waits again, and the signal for its item is used.  The
  first reader gave no signal after its read.  Now a reader gives a signal
  after its read when items are left.
- A reader that finds only removed items resets the signals of the queue.
  A writer can put an item in at that moment, and the reader then waits
  with an item in the queue.  Now `AtomicSharedQueue>>next` looks again
  before it waits.
- A reader that is terminated in `nextIfNone:` left the queue locked for
  each later reader.  Now the read runs in a block with `ifCurtailed:`.

Each of the three tests brings the queue into the state by steps of a
suspended process (`Process>>step`), so the tests do not depend on time.
On an image without the repair, the three tests fail on our VM and on
stock Cog.  With the repair, the three pass on the two VMs.

`TKTCommonQueueWorkerPoolTest>>testWorkerPoolDoesNotExceedPoolSizeWhenSchedulingTasksInParallel`
failed with `TKTTimeoutException` in the Pharo 13 and the Pharo 14 sweeps
at d312bd40.  (The sweeps list the class under this name.
`TKTWorkerPoolTest` passes 9 of 9.)  The test fails about 1 to 2 times in
100 on arm64 with the JIT on and with `PHARO_NO_JIT=1`.  The test is a race
in TaskIt, not a defect of the VM.

The cause is in `TKTCommonQueueWorkerPool>>privateRemoveIdleWorkerIfNeeded`.
The method builds the list of free workers (`currentTaskExecution isNil`).
It then sends a few more messages (`size`, `anyOne`, `workers remove:`).
Last, it sends `stop` to the worker that it chose.

A worker takes a task from the queue and sets `currentTaskExecution`
outside the pool mutex.  If a process switch falls between the two steps,
the chosen worker is busy when `stop` runs.  `stop` terminates the worker
process, and the task never ends.  The test then waits 100 ms for a future
that cannot finish.

Proof: a probe replaced `TKTWorkerProcess>>stop` with a copy that records
`currentTaskExecution`.  In 10 of 11 failed runs on our VM, the worker had a
`TKTFutureExecution` when `privateRemoveIdleWorkerIfNeeded` called `stop`.
Every other worker waited in `AtomicSharedQueue>>next`.  One failed run
shows a task that stopped without a `stop` record.

Stock Cog v12.0.6
(Pharo 14, x86_64 under Rosetta) shows the same record in 2 of 5 failed
runs (2500 runs).  Two other Cog failures were tasks that ended after the
100 ms limit.

Failures in a loop that runs the test body (JIT on, same image):

    ours, 2 x 1000 runs, load from other work   43 failures
    ours, 3 x 400 runs, build 8a799eef          13 failures in 1200
    ours, 3 x 400 runs, build e96d044d          18 failures in 1200
    ours, PHARO_NO_JIT=1, 300 runs               7 failures
    stock Cog v12.0.6, 2 x 1000 runs            14 failures
    stock Cog v12.0.6, 1 x 2500 runs             5 failures

The old build and the new build fail at the same rate, so no commit since
8a799eef caused the failure.  Earlier sweeps did not show the test because
one run of the test passes about 98 times in 100.  Our VM fails more often
than Cog (about 1.5 times in 100 against 0.2 to 0.7).  Our VM has more process
switches inside the window than Cog.  Cog fails too, so no VM change was
made.  The higher failure rate of our VM is an open lead in
[deferred.md](deferred.md).

## StTarjanAlgorithmTest times out in the runner, not in the VM (2026-11-12, runner fixed)

`StTarjanAlgorithmTest>>testNoOutgoingDependenciesAfterTarjan` printed
`TIMEOUT (exceeded 80s)` in two Pharo 14 sweeps.  It passed alone in 0.4 s
when run by `eval`.  Run through the runner it timed out on each try, alone
or after `StSpotterTest`.  The earlier classes of the batch do not matter.

The test calls `StPackageRelationGraph>>computeStaticDependencies` on the
`*Collections*` packages.  For `Collections-Unordered` the image signals
`StPotentialOutDatedDependencyWarning`: the manifest lists `System-Support`
as a manual dependency that the analysis does not find.  A pristine
Pharo 14 image under stock Cog signals the same warning.

The runner forks each test at priority 40.  The runner main process
(priority 79) had activated the `TestExecutionEnvironment`, so
`mainTestProcess` was the runner main.  `ProcessMonitorTestService` wraps
each new process of the environment.  It treated the unhandled warning in the
forked test process as a failure of a background process.  It suspended the
test process in `suspendBackgroundFailure:`.

A test in the main test process is different.  The monitor passes the
warning on, and the headless default action prints the warning and resumes.

Proof on stock Cog v12.0.6 with the same shape: `CurrentExecutionEnvironment
runTestsBy: [ [ test runCase ] forkAt: 40 ... ]` leaves the test process
suspended (`isSuspended` true, no result in 15 s).  Our VM gives the same
answer.  When the forked process first sets `mainTestProcess` to itself,
Cog finishes the test and prints the warning.

The fix is in `scripts/pharo-headless-test/run_sunit_tests.st`, method
`runSingleTest:selector:timeout:priority:on:`.  Right after the fork, the
runner main sets `mainTestProcess` of the batch environment to the forked test
process.  The runner main has priority 79, so the test has not started.  At the
end of the method the runner restores `mainTestProcess` and
`shouldSuspendBackgroundFailures`.  The monitor switches the suspension off for good after a failure of the
main process.  The runner never calls `cleanUpAfterTest`, so the restore keeps
the later tests as they were.

The new temporaries must not share a name with a block temporary of the
method.  `ReleaseTest>>testNoShadowedVariablesInMethods` fails when they do.

## `SDLOSXPlatform` over-releases autoreleased NSStrings

`ObjCLibrary>>nsStringOf:` answers `+[NSString stringWithUTF8String:]`, which is
**autoreleased** — the caller does not own it and must not release it.  Two
callers release it anyway:

    SDLOSXPlatform>>allowTouchpadInertia
        key := ObjCLibrary uniqueInstance nsStringOf: 'AppleMomentumScrollSupported'.
        ...
        ObjCLibrary uniqueInstance release: key.          "<-- over-release"

    SDLOSXPlatform>>afterSetWindowTitle:onWindow:
        aParam := ObjCLibrary uniqueInstance nsStringOf: aString.
        ...
        ObjCLibrary uniqueInstance release: aParam        "<-- over-release"

The string is then released a second time when the thread's autorelease pool
drains, which segfaults:

    *** -[CFString release]: message sent to deallocated instance

Upstream never notices.  The reference VM has no autorelease-pool handling
anywhere in its sources and runs the interpreter on the main thread of a
command-line process, so the pool is never drained and the second release never
happens.  Our VM runs the interpreter on a secondary thread of a GUI app, where
a thread's implicit pool *is* drained by pthread TSD cleanup at thread exit — so
the latent bug becomes a crash.

Found on `main` by tracing every FFI call the image makes for release-like
selectors (`PHARO_TRACE_RELEASE=1`, a temporary hook in `doFFICall` that logged
the object, its class, and the Smalltalk method responsible).  In a whole
session there were exactly two release calls and both were these, both on
`__NSCFString`.  The hook is not carried on this branch: it has served its
purpose and would sit on the FFI hot path for no further benefit.  Recover it
from `git show 159241d1 -- src/vm/Primitives.cpp` if it is ever needed again.

Patched in the generated startup scripts (see `PharoBridge.writeStartupScript`,
which puts it in `commonPatches`, so both `startup-13.st` and `startup-14.st`
carry it): both methods are recompiled without the release, which leaks the
string exactly as upstream does.

**The patch cannot cover the first call of each.**  Both run during SDL platform
init, before `StartupPreferencesLoader` loads `startup.st` — visible in the
trace, where both releases precede the `[startup] Pharo 13` line.  The patch
prevents later occurrences, such as subsequent window-title changes.  Fixing the
startup ones needs the fix upstream, or a hook earlier than `startup.st`.

Worth reporting upstream: the fix is to delete both `release:` sends, or to have
`nsStringOf:` answer a retained string and document it as owned.

## `Context >> copyTo:` is recursive — overflows on deep stacks (timer-scheduler-wedge)

**Symptom:** the full SUnit suite deadlocked at ~class 511 (after a run of
mass-erroring `Package*` tests) with only the idle process runnable and the
Delay/timer scheduler never re-arming (`[DELAY-DEATH]` recovery looping
forever). The VM log showed a stack overflow at `fd=4096` inside
`Context>>freeze` → `freezeUpTo:` → `copyTo:` (4094+ `#copyTo:` frames) while
capturing a `SubscriptOutOfBounds` signaler context.

**Cause:** stock Pharo's `Context>>copyTo:` copies the sender chain
**recursively**:

       copyTo: aContext
           self == aContext ifTrue: [ ^ nil ].
           tmp1 := self copy.
           self sender ifNotNil: [ tmp1 privSender: (self sender copyTo: aContext) ].
           ^ tmp1

So freezing/copying a deep stack (e.g. an error signalled ~4000 frames deep)
recurses ~chain-deep and overflows our VM's frame stack at the
`StackOverflowLimit` (4096). Stock Cog tolerates much deeper VM stacks, so it
never trips. The overflow *termination* then skipped the process's
`ensure:`/`ifCurtailed:` unwind blocks, LEAKING any held `Semaphore>>critical:`
mutex — and the Delay/timer scheduler subsequently blocked on that leaked mutex
forever. Two-part defect: the recursion (image) + the unwind-skipping
termination (VM).

**Patch (image):** `scripts/pharo-headless-test/run_sunit_tests.st` replaces
`copyTo:` with an **iterative** copy (identical result, no recursion) so no deep
stack overflows during freeze/copy. Verified: a 5000-deep recursion that errors
is now caught cleanly with 0 overflows; normal catch + `ensure:`/`ifCurtailed:`
unwinding unaffected.

**Patch (VM, defense-in-depth):** commit `73eb8947` — `handleStackOverflow` now
drives `activeProcess terminate` (runs the unwind blocks, releasing held
mutexes) instead of a C++ hard-kill, with frame headroom. Covers genuine
infinite recursion that overflows even without the recursive `copyTo:`.

## `Character >>` numeric coercion missing — blocks Metacello load

**Symptom:** any Metacello fetch of a non-preinstalled package fails
deep inside `SHA1>>processBuffer:` → `expandedBlock:` →
`ThirtyTwoBitRegister>>loadFrom:at:` with

       Instance of Character did not understand #bitShift:

**Cause:** `ThirtyTwoBitRegister>>loadFrom:at:` does
`(arg1 at: arg2) bitShift: 8` where `arg1` is a ByteString.  In Pharo
`ByteString>>at:` returns a `Character`, not an `Integer`, so the
pure-Smalltalk SHA1 path needs `Character>>bitShift:` to be defined.
Stock Pharo gets away with this because `SHA1>>primHasSecureHashPrimitive`
returns true (the C plugin handles the whole hash) and the
pure-Smalltalk fallback never runs.

**Patch:** `scripts/patches/character_numeric_coercion.st` adds:

* `Character>>bitShift:` `bitAnd:` `bitOr:` `bitXor:` — convert
  receiver to Integer then forward.
* `Character>>adaptToInteger:andSend:`,
  `adaptToFloat:andSend:`, `adaptToNumber:andSend:` — same trick
  for `10 + $A` style coercion (PolyMath needs these once SHA1 is
  past).

**Verified:** with both sets of patches in place, the following
Metacello load completes:

       Metacello new
         baseline: 'PolyMath';
         repository: 'github://PolyMathOrg/PolyMath/src';
         load.

309 PM classes installed, no errors.

**To apply to a fresh image:**

       cp /tmp/harness/Pharo.image /tmp/harness/Pharo-patched.image
       cp /tmp/harness/Pharo.changes /tmp/harness/Pharo-patched.changes
       /tmp/harness/pharo --headless /tmp/harness/Pharo-patched.image \
         eval --save \
         "'scripts/patches/character_numeric_coercion.st' \
           asFileReference fileIn"

**Upstream wishlist:** the right fix is for `ThirtyTwoBitRegister`
(or the surrounding code) to use `asInteger`/`codePoint`/`asciiValue`
explicitly so it works without depending on either VM behaviour.

## `SHA1 >> hashStream:` corrupts on character streams > 64 bytes (Smalltalk fallback)

**Symptom:** `SHA1Test>>testLargeCharacterStream` (a 260-byte String) fails on any
build WITHOUT the native DSAPlugin SHA primitive — i.e. crypto-OFF builds. SHA1
of any String stream > 64 bytes errors / yields a wrong digest; ByteArray streams
and inputs <= 64 bytes are fine. (Surfaced by the x86 SUnit comparison, where the
x86 box was built `PHARO_WITH_CRYPTO=OFF`; see `docs/sunit-3way-comparison.md`.)

**Cause:** `SHA1>>hashStream:` reads each middle block with
`buf := aPositionableStream next: 64` and passes it straight to `processBuffer:`.
For a *character* stream `next: 64` returns a **String** (Characters). The native
path (`primExpandBlock:`) reads the String's bytes fine, but the Smalltalk
fallback `expandedBlock:` does `ThirtyTwoBitRegister loadFrom: buf` which expects
byte *integers* — a Character errors. Only the middle blocks are affected;
`processFinalBuffer:` copies the last partial block into a `ByteArray` first
(which is why <=64-byte inputs and `'abc'` pass even crypto-OFF).

**Patch (image):** `scripts/pharo-headless-test/run_sunit_tests.st` recompiles
`SHA1>>hashStream:` to convert the middle block to bytes —
`buf := (aPositionableStream next: 64) asByteArray` — the same bytes the native
primitive reads; the rest of the upstream method is unchanged. Verified
crypto-OFF: SHA1 of 'a'×{65,100,128,260} now match `shasum`, and
`testLargeCharacterStream` passes. Moot on crypto-ON builds (native primitive
used). Upstream-worthy (a one-line fix to Pharo's `SHA1>>hashStream:`).

## Not bugs — expected image behaviors seen while GUI-testing on Windows

Investigated 2026-07-02 after interactive Windows GUI reports; both verified
byte-identical on the stock Cog reference VM, so neither is a VM defect.

- **"Illegal dependency of Bootstrap Layer" on AST-Core** (System Browser →
  select AST-Core): that string is `PharoBootstrapRule class>>ruleName`, a
  Renraku *architectural critique* Calypso evaluates for packages in the
  bootstrap layer (`BaselineOfPharoBootstrap kernelPackageNames , #('AST-Core')`).
  Upstream Pharo 13 genuinely ships AST-Core depending on 2 packages outside
  that layer — both VMs compute `StDependencyChecker new dependenciesOf:
  'AST-Core'` → 14 deps, extra = `#('Debugging-Utils' 'OpalCompiler-Core')` —
  so the critique fires on stock Pharo too. Upstream-worthy report, not ours.

- **System → Startup "does nothing" on click**: `#SystemStartup` is a submenu
  header (children: Run startup scripts / Define a preference file / Version
  Preferences folder / General Preferences folder, registered via `<worldMenu>`
  on `StartupPreferencesLoader`). Morphic expands submenus on HOVER; clicking
  the header is a no-op by design. Hover-expansion verified working in our
  Windows GUI (screenshot evidence in session 2026-07-02).

## Test-dir gotcha: the dummy SDL2.dll breaks the stock reference VM (RESOLVED)

The zero-real-symbol `SDL2.dll` staged in the image dir (to flip
`SDL2 isAvailable` for OUR stub-based GUI) makes the STOCK
`refvm/pharo-vm/PharoConsole.exe` fail at startup: it resolves real symbols
(`#SDL_SetHint`) from that DLL and errors.

RESOLVED 2026-07-02: the dummy no longer lives in the image dir at all. CMake
stages the marker next to test_load_image.exe and `FFIWindowsLibraryFinder`
locates it via `Smalltalk vm directory` (primitive 142 fixed to return the
directory, not the exe path). The image dir stays pristine, so the stock
reference VM runs there unmodified.

## TestExecutionEnvironment watchdog can kill the Delay scheduler (found 2026-07-06)

`TestExecutionEnvironment>>watchDogLoop` passes `maxTimeForTest
asMilliSeconds` straight into `Semaphore>>waitTimeoutMilliseconds:`.  The
watchdog's two-signal/two-wait protocol can desync (the inner timed wait
races the completion signal; the leftover excess signal makes the next
outer wait fire spuriously), and a spurious wake on a deactivated or
between-tests environment reads `maxTimeForTest` as nil.
`waitTimeoutMilliseconds: nil` constructs a DelayWaitTimeout whose nil
duration reaches `DelayMicrosecondTicker>>tickAfterMilliseconds:` and
raises MNU on `nil * 1000` INSIDE the timer event loop; the loop's
handler passes the error, and under SUnit the ProcessMonitorTestService
suspends the ticker process — after which every Delay/timeout in the
image stalls (observed: whole catalog-suite sections timing out after
StDebuggerActionModelTest).

We patch the runner prep (pharo-headless-test submodule,
run_sunit_tests.st): hardened watchDogLoop re-parks on the semaphore
when the limit is nil.  Upstream wishlist: (a) that guard; (b) a nil
check in waitTimeoutMilliseconds:; (c) make the Delay scheduler's
backend loop survive per-delay errors instead of dying with the first
poisoned delay.

## Test defects fixed by `scripts/pharo-image-fixes.st` (2026-10-15)

Seven tests failed in every sweep, and each failed the same way on stock Cog
with the pristine image.  The defects are in the image's tests or base code,
not in a VM.  `scripts/pharo-image-fixes.st` recompiles one method for
each.  File it in when you prepare the sweep image, after
`setup_fake_gui.st`.  With the fixes, all seven tests pass on both VMs.
(`scripts/pharo13-image-fixes.st` is a one-line file that files in the new
name.  The relative path in it needs the repository root as the working
directory.)

Every fix checks the current method source first.  It recompiles the
method only when the defect is present.  If the class is absent or Pharo
fixed the defect, the fix prints `SKIP` and changes nothing.  So one file
serves Pharo 13 and Pharo 14.  On Pharo 14 (e649a7f) fixes 1 to 6 find
nothing to do: `SystemDependenciesTest` does not exist, and the other five
defects are repaired upstream.  Fix 7 applies on both versions.

| Test | Defect | Fix |
|---|---|---|
| `OCClassBuilderTest>>testCreateNormalClassWithTraitComposition` | The class definition names `TAsStringCommaAndDelimiterTest`, which the image renamed to `TAsStringOnDelimiterTest`.  The compiler refuses the undeclared name. | Use the new name. |
| `SystemDependenciesTest>>testExternalUIDependencies` | The UI layer now depends on `Reflectivity`, and the allowlist (`knownUIDependencies`) does not include it. | Add `#Reflectivity` to the allowlist. |
| `ReleaseTest>>testNoShadowedVariablesInMethods` | In `FastStepThroughController>>#findNextContext:having:fixing:`, the argument `homeContext` shadows the instance variable `homeContext`. | Rename the argument to `aHomeContext`.  The method used the argument. |
| `StDebuggerTest>>testDynamicVariableEvaluation` | The provider forks a process that sets the dynamic variable when it first runs, and `fork` does not run it.  The test passed only if something preempted the test process while the debugger was built (a Delay that fired).  It failed on our VM in every sweep and passed when run alone. | `StTestDebuggerProvider>>debuggerWithDynamicVariableSuspendedContext` yields until the forked process has set the variable. |
| `StDebuggerTest>>testUpdateLayoutForContextsIfAssertionFailure` | The test reads actual and expected from the assertion's description block by index (actual at 2, expected at 1).  The block's copied values are (actual, expected), so the test compared swapped values: "Got 4 instead of 2". | Swap the indexes. |
| `StDebuggerInspectorTest>>testUpdateLayoutForContextsWithAssertionFailure` | The same swapped indexes. | Swap the indexes. |
| `TFUFFICallbackTest>>testCallbackWithErrorHanlded` | `UIManager>>nonInteractiveManager` answers `NonInteractiveUIManager beDefault`.  That sends `beDefault` to the class, and the class has no such method.  `UIManager class>>nonInteractiveDuring:` calls it, so the test fails whenever the default manager is a `MorphicUIManager`, which the fake-GUI prep and the runner install.  Stock Cog fails the same way when `MorphicUIManager new beDefault` runs first (verified on Pharo 14).  The test passes when the default is `NonInteractiveUIManager`. | Send `new beDefault`. |

The 2026-09-02 analysis of the first test, kept for its evidence: the
definition names fifteen test traits, and this check finds the one that is
missing:

    (#(TAddForUniquenessTest ... TSetArithmetic)
        reject: [:n | Smalltalk globals includesKey: n ])
    -> #(#TAsStringCommaAndDelimiterTest)

## Image and test repairs of 2026-11-17 (Pharo 14)

The Pharo 14 image (build 791) had 220 tests that did not pass on our VM and on stock Cog.  The repairs are in the entries files in `scripts/image-fixes/`.  The generator `scripts/gen-image-fixes.py` joins the entries files with the helper blocks of `00-header.st` and writes `scripts/pharo-image-fixes.st`.  Each entry has a guard that reads the current method source, so the entry applies only when the defect is present.  A pristine Pharo 14 build 791 image gives 151 FIXED, 40 ADDED and 6 SKIP lines.  Our VM and stock Cog give the same counts.

The record of each repair (cause, fix and proof) is in `docs/upstream/pharo-image-repairs-2026-11-17.md`.  The table gives one row for each entries file.  The `10-` file is in the section above.  The `20-NA-platform-windows.st` file repairs 3 tests, and the record shows that the other 16 tests of that group cannot apply on macOS arm64.

| Entries file | Repairs | Defect | Where | Record |
|---|---|---|---|---|
| `10-pharo13-test-defects.st` | seven tests of the section above | see the section above | test and image code | none |
| `20-BlockClosureTest.st` | `BlockClosureTest` (2 tests) | Unconditional `self skip` | test | [link](upstream/pharo-image-repairs-2026-11-17.md#blockclosuretest) |
| `20-CavWindowStrategyTest.st` | `CavWindowStrategyTest` | Stale skip and flag | test | [link](upstream/pharo-image-repairs-2026-11-17.md#cavwindowstrategytest) |
| `20-ClapHelloTest.st` | `ClapHelloTest`, `ClapCommandLineExamples` | `hasFlag:` loses the order, so the latest flag does not win | image code | [link](upstream/pharo-image-repairs-2026-11-17.md#claphellotest) |
| `20-Cly-async-results.st` | `ClyAsyncQueryResultTest` and the semi-async subclass | Expected failure calls a super test that contradicts the async contract | test | [link](upstream/pharo-image-repairs-2026-11-17.md#cly-async-results) |
| `20-Cly-misc-queries.st` | five `Cly*QueryTest` tests | Expected-failure overrides in place of the real check | test | [link](upstream/pharo-image-repairs-2026-11-17.md#cly-misc-queries) |
| `20-Cly-wrapper-queries.st` | `ClyWrapQueryTest` (7 selectors, 14 tests) | Composite tests that a wrapper query refuses by design | test | [link](upstream/pharo-image-repairs-2026-11-17.md#cly-wrapper-queries) |
| `20-Debugger-kernel-skips.st` | `DebugInfoTest` (3 tests), `CodeSimulationTest` | Stop points too wide; the skip of `testErrorCodeNotFound` hides a VM difference (not accepted in review) | image code and test | [link](upstream/pharo-image-repairs-2026-11-17.md#debugger-kernel-skips) |
| `20-FloatTest.st` | `FloatTest>>testNaNCompare` | Unconditional `self skip` | test | [link](upstream/pharo-image-repairs-2026-11-17.md#floattest) |
| `20-Fuel-cleanblock.st` | nine `FL*` tests | Obsolete clean-block skip guard; one wrong premise | test | [link](upstream/pharo-image-repairs-2026-11-17.md#fuel-cleanblock) |
| `20-Fuel-widestring-globals.st` | `FLGlobalCluster`, `FL*BasicSerializationTest` (15 tests) | Global names written one byte for each character | image code and test | [link](upstream/pharo-image-repairs-2026-11-17.md#fuel-widestring-globals) |
| `20-Geometry-intersections.st` | `GEllipse`, `GArcTest`, `GEllipseTest` | `intersectionsWithEllipse:` is a stub | image code | [link](upstream/pharo-image-repairs-2026-11-17.md#geometry-intersections) |
| `20-MicMetaDataBlockTest.st` | `MicMetaDataBlockTest` | Skipped test with an obsolete expectation | test | [link](upstream/pharo-image-repairs-2026-11-17.md#micmetadatablocktest) |
| `20-NA-platform-windows.st` | `Win32EnvironmentTest` (3 tests) | Skip on a platform-independent contract | test | [link](upstream/pharo-image-repairs-2026-11-17.md#na-platform-windows) |
| `20-NavigationInteractionTest.st` | `NavigationInteractionTest` | Unbuilt browser in the test; the spawned tool does not match | test and image code | [link](upstream/pharo-image-repairs-2026-11-17.md#navigationinteractiontest) |
| `20-network-tests.st` | `NetNameResolverTest`, `ZnHTTPSTest`, `ZnClientTest` | `localHostAddress` answers 0.0.0.0; skipped or stale network tests | image code and test | [link](upstream/pharo-image-repairs-2026-11-17.md#network-tests) |
| `20-OCArrayLiteralTest.st` | `OCArrayLiteralTest` (pharo issue 19058) | A nested brace array needs more stack slots than a frame has | image code | [link](upstream/pharo-image-repairs-2026-11-17.md#ocarrayliteraltest) |
| `20-OCParserTest.st` | `OCParserTest` (2 tests) | Skip and expected failure without an assertion | test | [link](upstream/pharo-image-repairs-2026-11-17.md#ocparsertest) |
| `20-OCProgramNodeTest.st` | `OCProgramNodeTest`, `OCAssignmentNode` | `bestNodeFor:` answers the value in place of the assignment | image code | [link](upstream/pharo-image-repairs-2026-11-17.md#ocprogramnodetest) |
| `20-OCRengineTest.st` | `OCRengineTest` | Leftover `self skip` | test | [link](upstream/pharo-image-repairs-2026-11-17.md#ocrenginetest) |
| `20-platform-unix-resolver.st` | `UnixResolverTest` (3 tests) | Guard skips on macOS, and the code is plain POSIX | test | [link](upstream/pharo-image-repairs-2026-11-17.md#platform-unix-resolver) |
| `20-presenter-examples.st` | eight presenter test classes | Presenter classes have no class-side `example` | image code | [link](upstream/pharo-image-repairs-2026-11-17.md#presenter-examples) |
| `20-ReflectivityOnStackTest.st` | `ReflectivityOnStackTest`, `ReflectiveMethod` | Saved bytecode cache has the old layout (pharo issue 20072) | image code | [link](upstream/pharo-image-repairs-2026-11-17.md#reflectivityonstacktest) |
| `20-ReleaseTest.st` | `ReleaseTest>>testRatioOfCommentedClasses` | Absolute count of uncommented classes grows with the loaded projects | test | [link](upstream/pharo-image-repairs-2026-11-17.md#releasetest) |
| `20-RGMCClassTest.st` | `RGMCClassTest`, Ring and Monticello code | Trait slots compared; `#` kept in a slot name | test and image code | [link](upstream/pharo-image-repairs-2026-11-17.md#rgmcclasstest) |
| `20-Ring2ChunkImporterTest.st` | `Ring2ChunkImporterTest` (2 tests) | The chunk parser loses `classTrait` | image code | [link](upstream/pharo-image-repairs-2026-11-17.md#ring2chunkimportertest) |
| `20-Rubric-anchors.st` | `RubEditingAreaTest` (3 tests) | Leftover `self skip` ("fails in CI") | test | [link](upstream/pharo-image-repairs-2026-11-17.md#rubric-anchors) |
| `20-RunnerLongDurationTest.st` | `RunnerLongDurationTest` (2 tests) | Unconditional `self skip` | test | [link](upstream/pharo-image-repairs-2026-11-17.md#runnerlongdurationtest) |
| `20-SettingsStonReaderTest.st` | `SettingsStonReaderTest`, `SettingsStonReader` | `load` does not handle a missing instance variable | image code and test | [link](upstream/pharo-image-repairs-2026-11-17.md#settingsstonreadertest) |
| `20-SlotErrorsTest.st` | `SlotErrorsTest>>testCannotBeRecompiled` | Expectation of the old class builder | test | [link](upstream/pharo-image-repairs-2026-11-17.md#sloterrorstest) |
| `20-SpCodePresenterTest.st` | `SpCodePresenterTest` (3 tests) | Skips with three different causes, including a modal dialogue | test and image code | [link](upstream/pharo-image-repairs-2026-11-17.md#spcodepresentertest) |
| `20-SpEasy-skips.st` | ten Spec2 Easy presenter tests | Easy presenters lack API that the shared tests call | image code and test | [link](upstream/pharo-image-repairs-2026-11-17.md#speasy-skips) |
| `20-SpMorphPresenterTest.st` | `SpMorphPresenterTest`, `SpMorphicMorphAdapter` | The adapter ignores the presenter help | image code | [link](upstream/pharo-image-repairs-2026-11-17.md#spmorphpresentertest) |
| `20-SpRadioButtonInteractionTest.st` | `SpRadioButtonInteractionTest` (2 tests) | Inherited test needs an adapter that a composable presenter lacks | test | [link](upstream/pharo-image-repairs-2026-11-17.md#spradiobuttoninteractiontest) |
| `20-SpTree-selection-skips.st` | six tree presenter tests | Stub tests with a stale reason | test | [link](upstream/pharo-image-repairs-2026-11-17.md#sptree-selection-skips) |
| `20-St-evaluate-declare-global.st` | two `St*` presenter tests | Tests rely on a warning that the UI path does not raise | test and image code | [link](upstream/pharo-image-repairs-2026-11-17.md#st-evaluate-declare-global) |
| `20-StDebugger-skips.st` | nine `StDebugger*` tests | Obsolete expectations, empty placeholders, a test that runs the test again | test and image code | [link](upstream/pharo-image-repairs-2026-11-17.md#stdebugger-skips) |
| `20-StObjectTranscriptPresenterTest.st` | `StObjectTranscriptPresenterTest` | Remembered window extent from another test | test | [link](upstream/pharo-image-repairs-2026-11-17.md#stobjecttranscriptpresentertest) |
| `20-StSettingsBrowserTest.st` | `StSettingsBrowserTest` | Stale body calls removed API | test | [link](upstream/pharo-image-repairs-2026-11-17.md#stsettingsbrowsertest) |
| `20-TFFI-disabled-tests.st` | `FFICallbackParametersTest`, `TFCallbacksTest`, `TFUFFIFunctionCallTest` | Disabled with an obsolete reason | test | [link](upstream/pharo-image-repairs-2026-11-17.md#tffi-disabled-tests) |
| `20-WeakAnnouncerTest.st` | `WeakAnnouncerTest>>testNoWeakBlock`, `AnnouncementSubscription` | `makeWeak` raises for a block action | image code | [link](upstream/pharo-image-repairs-2026-11-17.md#weakannouncertest) |
| `20-WriteBarrierTest.st` | `WriteBarrierTest` (2 tests), `TFFIBackend` | Test compares a byte with a Float; the fallback does not signal `ModificationForbidden` | test and image code | [link](upstream/pharo-image-repairs-2026-11-17.md#writebarriertest) |
| `21-write-barrier-integer-stores.st` | `ByteArray>>integerAt:put:size:signed:`, `WriteBarrierTest` | A store into a read-only `ByteArray` ends in `PrimitiveFailed` | image code and test | none |
| `22-interactive-session-tests.st` | 28 tests that skip in a headless run | Deprecated `isHeadless` guard always true; `MorphicNativeWindow` lacks `isClosed` | test and image code | none |
| `23-defects-the-standard-runner-shows.st` | `OutOfMemoryTest`, `Collections-Unordered` manifest | Low Space Watcher ended by another test; stale manual dependency | test and image code | none |
| `24-stale-executable-comment.st` | `Context class>>tallyInstructions:` | Comment holds an exact bytecode count that went out of date | image code | none |
| `25-taskit-worker-pool-race.st` | `TKTCommonQueueWorkerPool`, `TKTCommonQueueWorkerPoolTest`, `TKTFutureTest` | The pool stops a worker that holds a task | image code | section "TaskIt worker pool" above |
| `26-taskit-first-complete-tests.st` | `TKTFutureTest` (4 tests of `firstCompleteOf:`) | Test depends on the order of two callbacks in a worker pool | test | section "TaskIt worker pool" above |
| `27-atomic-shared-queue-wakeups.st` | `AtomicSharedQueue`, `WaitfreeQueue`, new `AtomicSharedQueueTest` | A reader loses a wake-up; a terminated reader leaves the queue locked | image code | section "TaskIt worker pool" above |
| `28-simple-formatter-comments.st` | `OCSimpleFormatter`, `OCParseCodeSnippetTest>>testSimpleFormattedCode` (23 skips) | The simple formatter dropped the comments of an expression, of a block and of a brace array | image code and test | none |
| `29-code-importer-snippets.st` | `CodeImporterCodeSnippetTest>>testCodeImporter` (3 skips) | Skip for an empty source, and a stale skip mark for the source ` ]` | test | none |
| `30-ffi-callback-from-other-thread.st` | `TFUFFICallbackTest>>testCallbacksInOtherThread` (1 skip) | Stock Cog stops when a callback of the same-thread runner comes from another thread; the skip now is only for a VM without the repair | Cog, test | `upstream/README.md` |

### The code snippet skips (2026-11-17, repaired)

`OCCodeSnippet` has a list of tests to skip for each snippet, and its class
comment says that these tests "should be fixed".  26 tests of the Pharo 14
suite skipped for that list, on our VM and on stock Cog.

- 23 skips of `OCParseCodeSnippetTest>>testSimpleFormattedCode`, for the
  snippets with comments.  The test compares the output of
  `OCSimpleFormatter` with the expected text of the other formatter.
  `OCSimpleFormatter` printed a comment only at a statement of a sequence,
  at a temporary, at a block argument and at a method, and dropped each
  other comment.  Entry 28 makes the formatter print each comment of the
  AST one time, and gives the snippets an expected text for the simple
  formatter where the two formatters differ
  (`OCCodeSnippet>>simpleFormattedCode`).  Six snippets outside the group
  passed before only because the two formatters dropped the same comments.
  They have an expected text now too.
- 3 skips of `CodeImporterCodeSnippetTest>>testCodeImporter` (entry 29).

Open, found on the way: the parser loses comments.  The source
`1 "A" true "B" false "C" nil "D" self "E" super "F" thisContext "G"` gives
an AST with the comments A, E, F and G only.  The formatter cannot print a
comment that the AST does not have.

## Upstream wishlist — verified image bugs we do NOT patch (2026-07-07)

Accepted residuals from the 2026-07-06 catalog (#8, 99.92%).  Each was
re-verified on the PRISTINE Pharo 13 image (get.pharo.org/64/130, build
737 sha b6a64c69) under stock Cog — they fail identically there, so they
are upstream image defects, not our-VM or harness artifacts.  We leave
the tests failing rather than patch semantics the IDE depends on.

### `MorphicNativeWindow` lacks `hasProperty:` (taskbar MNU)

`MorphicNativeWindow` subclasses `Object` directly, so it inherits none
of Morph's property protocol, yet `TaskbarMorph>>updateTasks` (and
`TShowInTaskbar classTrait>>findOrigin{Class,Method}Of:`) send
`hasProperty:` to every registered window.  In a native-window session a
`MorphicNativeWindow` in that list MNUs the taskbar refresh.  Verified
pristine: `MorphicNativeWindow canUnderstand: #hasProperty:` -> false.
Surfaced by `MorphicWindowManagerTest>>testDeleteAWindowAndTaskBarActualized`
during the `--interactive` default investigation (see docs/changes.md
2026-07-06: bare launches no longer default to interactive).

## `ProtoObject >> pointersToExcept:among:` sends `removeAllSuchThat:` to an Array

Carried over from the `main` branch on 2026-08-17.

Surfaces as `ProtoObjectTest >> testFastPointersTo` erroring with
`ShouldNotImplement: #remove:ifAbsent: should not have been implemented in Array`.

Stock source (Pharo 13.1), abbreviated:

    pointersToExcept: objectsToExclude among: aCollectionOfObjects
        | pointers objectsToAlwaysExclude |
        pointers := OrderedCollection new.
        pointers := aCollectionOfObjects select: [ :e | e pointsTo: self ].
        ...
        ^ (pointers removeAllSuchThat: [ :ea | ... ]) asArray

The first assignment is dead — the next line overwrites it. Callers pass
`SystemNavigation default allObjects`, which is an Array, and `select:` on an
Array answers an Array. `Collection>>removeAllSuchThat:` then sends `remove:`
for each match, and `Array` answers `shouldNotImplement`.

It only raises when the block actually matches, i.e. when one of the excluded
contexts is itself among the pointers. On Cog that rarely happens, because live
activations are stack frames rather than heap objects, so `allObjects` does not
see them — which is why upstream does not notice.

Verified independent of the VM: built with and without the
`primitiveObjectPointsTo` raw-format guard, `testFastPointersTo` raises the
identical error both times. Outside SUnit the same code is fine — an eval that
reproduces the test body verbatim finds exactly one pointer and never enters the
failing branch.

Fix upstream is to make the collection removable before removing from it:

    pointers := (aCollectionOfObjects select: [ :e | e pointsTo: self ])
        asOrderedCollection.

and drop the dead line above it. **Not patched here** — it affects one reflection
method and its test, not runtime behaviour.

## ~~`DefaultExecutionEnvironment` does not implement `#watchDogProcess`~~ — WITHDRAWN 2026-09-02

Filed 2026-08-23 because `ReleaseTest>>testUnknownProcesses` failed with

    MessageNotUnderstood: DefaultExecutionEnvironment >> #watchDogProcess

on both architectures.  **It is not an image bug; it is how the test was
being run.**  `#watchDogProcess` is defined on `TestExecutionEnvironment` and
nowhere else in this image (checked against the `.sources` of Pharo 13.1 build
745), and SUnit installs a `TestExecutionEnvironment` for the duration of a
test run.  So the send only reaches `DefaultExecutionEnvironment` when the
test is driven outside that installation — which is exactly what the
2026-08-23 investigation did, running the four `ReleaseTest` failures
individually with a 1200 s limit.

Under the sweep runner the test gets past that line: in the 2026-09-02 arm64
sweep it fails later and for a different reason, on a leftover
`'CommandLine handler process'` parked in `SessionManager>>snapshot:andQuit:`
— the prep step (`12ad7265`) showing up in the image it saved.  That one is
ours, and it is a harness artifact rather than a VM defect.  See
`docs/results/sweep-arm-2026-09-02.md`.

Kept rather than deleted because the wrong verdict is the lesson: a harness
artifact was filed as an upstream image bug, one level up from the usual
direction.

## XMLParser calls `whichCategoryIncludesSelector:`, removed in Pharo 13 (2026-08-23)

Eight `XMLWriterTest` tests fail identically on BOTH architectures — the single
largest cluster in the package tier's residual:

    testOnFormatter, testFormattingProlog, testFormattingElementDeclarations,
    testFormattingAttributeDeclarations, testFormattingContent,
    testFormattingEntityDelcarations, testFormattingPIs,
    testFormattingUnsafeTagWriters

all with

    MessageNotUnderstood: XMLWriterFormatter class >> #whichCategoryIncludesSelector:

**Not a VM defect, and proven so rather than assumed.** The selector is
implemented by NOTHING in the image:

    allImplementorsOf: #whichCategoryIncludesSelector:  ->  #()
    XMLWriterFormatter class respondsTo:  ->  false
    ClassDescription/Behavior includesSelector:  ->  false

so the MNU is the correct answer. Pharo 13 renamed it in the
category -> protocol sweep; the replacements present in the image are

    #protocolNameOfSelector:      (the direct equivalent)
    #protocolOfSelector:

**Upstream fix belongs in XMLParser**, not here: its test helper should call
`protocolNameOfSelector:`. We deliberately do NOT shim the old selector back
into the image — re-adding an API Pharo removed, to make a third-party test
pass, would improve the score without making this VM any more correct.

Counted in the package tier as 8 of the 18 errors per architecture.
