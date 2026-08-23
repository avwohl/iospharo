# Pharo 13 image issues we patch

This is the consolidated list of bugs in the standard Pharo 13 image
(`/tmp/harness/Pharo.image`, fetched via `https://get.pharo.org/64/130`)
that we need to patch locally for our VM to work the same way as
stock Cog.  See also `docs/vm-compat-bugs.md` for open VM-side defects, and
`docs/deferred.md` for intentional platform gaps.

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

## Upstream wishlist — verified image bugs we do NOT patch (2026-07-07)

Accepted residuals from the 2026-07-06 catalog (#8, 99.92%).  Each was
re-verified on the PRISTINE Pharo 13 image (get.pharo.org/64/130, build
737 sha b6a64c69) under stock Cog — they fail identically there, so they
are upstream image defects, not our-VM or harness artifacts.  We leave
the tests failing rather than patch semantics the IDE depends on.

### `OCClassBuilderTest>>testCreateNormalClassWithTraitComposition`

Errors with `OCCodeError: Undeclared variable` while building a normal
class with a trait composition through the OpalCompiler class-builder
API.  Stock Cog, pristine image: `1 ran ... 1 error`.  Ours: identical.

### `SystemDependenciesTest>>testExternalUIDependencies`

Fails with

    TestFailure: Given Collections do not match!
        additions : #('Reflectivity')
        missing: #()

The computed external-dependency set of the UI layer now includes
`Reflectivity`, which the test's expected allowlist predates.  Pure
dependency-drift in the image; the fix is a one-line allowlist update
upstream.

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

## `DefaultExecutionEnvironment` does not implement `#watchDogProcess` (2026-08-23)

`ReleaseTest>>testUnknownProcesses` fails with

    MessageNotUnderstood: DefaultExecutionEnvironment >> #watchDogProcess

on both architectures. The test asks the current execution environment for its
watchdog process; `DefaultExecutionEnvironment` in this Pharo 13 image has no
such method. Nothing to do with the VM, and — despite the name — nothing to do
with the test runner's own processes either, which is how it was previously
lumped in with the harness self-pollution failures.

Upstream wishlist: either implement `#watchDogProcess` on
`DefaultExecutionEnvironment` or have the test tolerate an environment that
does not provide one.

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
