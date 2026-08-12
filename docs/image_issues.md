# Pharo 13 image issues we patch

This is the consolidated list of bugs in the standard Pharo 13 image
(`/tmp/harness/Pharo.image`, fetched via `https://get.pharo.org/64/130`)
that we need to patch locally for our VM to work the same way as
stock Cog.  See also `docs/vm-compat-bugs.md` for open VM-side defects, and
`docs/deferred.md` for intentional platform gaps.

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
