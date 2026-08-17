# VM dossiers carried over from the `main` branch

**Date:** 2026-08-17
**Source:** `docs/known-issues.md` on the `main` branch, at commit 22451a75,
immediately before `main` was reset to `origin/main`. The 86 commits that built
it are preserved on the ref `backup-main-2026-08-17` and in
`~/iospharo-backups/main-86-commits-2026-08-17.bundle`.

This file exists so that work does not vanish with the branch. Read it as a
record of the `main` VM, not of this one. The two diverged on 2026-04-01 and
`main`'s VM is a much smaller, different codebase; several defects described
below as open or newly fixed were fixed on `jit` months earlier, independently.
Line numbers and file offsets refer to `main`'s tree and do not transfer.

What is genuinely new here, with no counterpart on this branch:

  - the Mac Catalyst toolbar abort, including the four attempted fixes that did
    NOT work, which is the expensive half of that investigation
  - the VM-thread / autorelease investigation, including the negative result
    that draining the pool at a healthier time crashes identically
  - the dead-primitive audit (~370 of 814 `Interpreter::primitive*` methods are
    never installed in `primitiveTable_`) and its repro script
  - the colour-fidelity measurement methodology, which explains why a correct
    VM can look broken — now also summarised in `docs/graphics-testing.md`
  - the working screenshot recipe, which contradicts the one this repo's
    CLAUDE.md carried; CLAUDE.md has been corrected

---

# Known Issues

Last updated: 2026-08-16

## iOS-Specific

- ~~Taskbar selected button text (e.g. "Welcome") has slight rendering
  artifacts~~ Does not reproduce (2026-08-16). Captured the Mac Catalyst app's
  rendering and inspected the selected taskbar button at 12x: sharp button
  edges, clean window icon, well-formed anti-aliased glyphs, nothing missing or
  doubled. Measured too — restricted to glyph pixels, the taskbar text has 0%
  chromatic pixels and a maximum channel delta of 3, identical to the menu bar
  text, so there is no colour fringing.

  Note the capture method, because the one in CLAUDE.md did not work here:
  `screencapture -x -l <windowID>` answered "could not create image from
  window", and the Metal layer is absent from a full-screen capture (the window
  area comes out solid black). What does work is asking Pharo for its own
  pixels. Inject a forked process with `eval --save`, launch the app on that
  image with `--image`, and have the fork write `World imageForm` out:

      [ (Delay forSeconds: 40) wait.
        | w form bits |
        w := Smalltalk at: #World ifAbsent: [ nil ].
        form := w imageForm.
        "... write form width/height/depth and form bits to files ..."
      ] forkAt: 30.

  Two gotchas: the whole `eval` argument is compiled before any of it runs, so
  reach globals through `Smalltalk at:` rather than naming them; and `Display`
  is nil in the app (unlike under `test_load_image`), so go through `World`.

- ~~The Mac Catalyst app aborts whenever a running image quits~~ **Fixed
  2026-08-17.** It fired on every image quit, so it was worth chasing properly.

  `ContentView` switches on `bridge.isRunning`, so a quitting image swaps the
  Pharo canvas back for `ImageLibraryView`, which rebuilds the Catalyst toolbar
  bridge. AppKit then aborted:

      NSInternalInconsistencyException, reason: 'Index out of bounds'
        -[NSToolbarItemGroupPickerView _configureCollapsedSubitemAtIndex:]
        -[NSToolbarItemGroupPickerView _configureCollapsedSubitems]
        -[NSToolbarItemGroupPickerView setSubitems:]
        -[NSToolbarItemGroup _updateViewRepresentation]     <- re-entered
        -[NSToolbarItemGroup _forceSetView:]
        -[NSToolbarItemGroup _updateViewRepresentation]     <- from here
        -[NSToolbarItemGroup setBordered:]

  `_updateViewRepresentation` re-enters itself through `_forceSetView:`, and the
  group's subitems are inconsistent with the picker view during that re-entrancy.
  That is an AppKit defect, not malformed input from us, and is worth reporting to
  Apple.

  **Found by bisection rather than guesswork**, which matters because four
  plausible-sounding fixes had already failed. Removing the second toolbar item
  stopped the abort outright; putting it back — in *either* placement, leading or
  trailing — brought it straight back. So the trigger is the item *group*, which
  only forms with two or more items, not the position of any one item.

  Fixed by having one toolbar item on Mac Catalyst: Settings moves into the same
  menu as the download and import actions. Other platforms keep the separate gear
  button. Verified over two runs — the app survives the quit, returns to the
  library, and the assertion never fires.

  Not visually verified: this machine's screen is locked, so the abort was
  confirmed gone from process exit codes and stderr rather than by looking. The
  functional change is small and exactly described above, so it is easy to check
  or move once someone can see it.

  The four fixes that did **not** work, so nobody repeats them: distinct `.id()`
  on the `ContentView` branches; explicit `ToolbarItem(id:placement:)`; moving the
  blocking `vm_stop()` teardown off the main thread; and deferring the toolbar by a
  runloop turn (abandoned — conditional `.toolbar` content needs iOS 16 and this
  target is lower).

- The VM thread sleeps forever after `interpret()` returns instead of exiting.
  `vm_run` in `PlatformBridge.cpp` ends the thread body with a 24-hour sleep
  loop. This costs a leaked thread and everything its autorelease pool holds on
  every image relaunch, and it is a workaround rather than a fix.

  **The crash it avoids is real.** Confirmed 2026-08-16 by removing the loop,
  rebuilding the Mac Catalyst app, and letting an image quit itself. The app
  died with EXC_BAD_ACCESS / KERN_INVALID_ADDRESS at 0x6e6f436c8889:

      objc_msgSend
      AutoreleasePoolPage::releaseUntil(objc_object**)
      objc_autoreleasePoolPop
      AutoreleasePoolPage::HotPageDealloc dtor_
      _pthread_tsd_cleanup
      _pthread_exit
      _pthread_start
      thread_start

  So the thread exits, pthread TSD cleanup drains this thread's implicit
  autorelease pool, and `objc_msgSend` on one of the pooled entries faults. A
  pointer that is not a live ObjC object is getting into that pool. The fault
  address reads as ASCII — 6e 6f 43 6c is "noCl" — which points at memory
  already reused by a string, i.e. an over-release or a non-object pointer
  being autoreleased somewhere in the FFI path, with the damage only surfacing
  at drain time.

  Earlier in the same session `test_relaunch` ran three launch/quit/relaunch
  cycles with the loop removed and did not crash. That is not evidence of
  safety: a headless boot barely calls into ObjC, so its pool is nearly empty.
  Only the app reproduces it. Do not rely on the headless result.

  **Status after 2026-08-16: this is not a VM correctness bug, and the workaround
  matches what the reference VM effectively does.** Three things establish that.

  The reference VM contains no autorelease pool handling at all — searching its
  entire tree for "autorelease" finds nothing. It runs the interpreter on the main
  thread of a command-line process, so objects autoreleased by FFI calls simply
  leak (the runtime says as much: "autoreleased with no pool in place - just
  leaking") and nothing ever drains them. Never draining is therefore the
  reference behaviour, not a dodge.

  The VM's own CoreFoundation code is correct, audited call by call, and the FFI
  layer does no retain or release of ObjC objects whatsoever — it only calls
  functions. So the over-release is image-side, in Pharo's own FFI bindings, where
  Smalltalk code can send `release` to an object it does not own. The reference VM
  has the same over-release; it just never drains, so nobody ever finds out.

  What is genuinely ours is narrower: we run the interpreter on a **secondary**
  thread of a GUI app, and a secondary thread's implicit pool is drained by
  pthread TSD cleanup when it exits. Hence the sleep-forever loop.

  So the remaining defect was not the crash — it was the cost of avoiding it: one
  leaked thread and its pool per image relaunch. **That part is now fixed.** The VM
  worker is created once and parks on a condition variable between images instead
  of a fresh thread sleeping for 24 hours after each one. The never-drain property
  is unchanged — the thread still never returns, so pthread TSD cleanup never runs
  on it — but the cost is now constant rather than per-launch.

  Measured with `test_relaunch` over six launch/quit cycles, counting live threads:

      cycle          1    2    3    4    5
      thread-per-run 4    6    7    8    9
      parked worker  4    4    4    4    4

  All six cycles pass either way; the difference is only what is left behind.

  **The bad pointer is an over-released CFString** (identified 2026-08-16). Running
  the app under `NSZombieEnabled=YES` and letting an image quit gives, at the
  moment the pool drains:

      *** -[CFString release]: message sent to deallocated instance 0x7765c5d800

  So the pool holds a string that something else already released. That matches
  the fault address in the original crash reading as ASCII — the memory had been
  reused. The VM's own CoreFoundation code was audited and is correct: every
  `Create`/`Copy` result is released exactly once, and the `Get`-rule results
  (`CFLocaleGetValue`, `CFDateFormatterGetFormat`) are correctly not released. So
  the over-release is very likely on the image side of the FFI, where Pharo code
  can send `release` to an object it does not own.

  **Draining at a different time does not help.** The theory that the crash was
  an artefact of pthread TSD cleanup — a constrained context where the ObjC
  runtime may be partly torn down — was tested directly and is wrong. Pushing an
  explicit pool at VM thread start and popping it after `interpret()` returns,
  while the thread and the runtime are both healthy, crashes in exactly the same
  way:

      EXC_BAD_ACCESS  KERN_INVALID_ADDRESS at 0x486fad7e6258
        objc_release
        objc_autoreleasePoolPop
        vm_run::$_1

  That is a cleaner stack than the original — no TSD cleanup frames, the drain is
  ours — and it is what made the zombie identification possible. But it is a worse
  state to ship than the current workaround, because it drains on every image quit
  where today nothing is drained, so it was reverted.

  The remaining work is to find what releases that string. The sequence that got
  this far, for whoever picks it up: build with an explicit pool push at VM thread
  start and a pop after `interpret()` returns, write a marker file either side of
  the pop (the app cannot be watched any other way), then run the app binary
  directly — not via `open`, which cannot pass environment variables — with
  `NSZombieEnabled=YES`, pointing `--image` at an image that quits itself after a
  delay.

  Attempted 2026-08-16 and reverted, with what was learned. The idea was to stop
  relying on the implicit pool: push one explicitly at thread start and pop it
  after `interpret()` returns, while the thread and the ObjC runtime are both
  still healthy, rather than letting pthread TSD cleanup drain it in a far more
  constrained context. It compiles and the app builds.

  It was reverted because it could not be validated where it matters. Headless
  does not reproduce the crash, so the app is the only test, and the app now
  aborts on the main thread in AppKit toolbar code (see above) before the quit
  path can be exercised. Shipping it unvalidated would be worse than the current
  workaround, because it drains on **every** image quit where today nothing is
  drained at all — turning a known leak into a possible new crash.

  Two things to keep from the attempt. The push alone is pointless: without a pop
  nothing is drained, which is exactly today's behaviour, so it is risk with no
  benefit. And the pop is only worth landing once someone can run the app through
  a quit — which needs the AppKit abort above dealt with first, and an unlocked
  screen.

  To fix it properly, find the bad pointer rather than the drain. Worth trying:
  run the app under `NSZombieEnabled=YES` and `MallocScribble=1` so the
  over-release reports at the point of release; and audit every place the FFI
  path hands an ObjC pointer to Smalltalk or takes one back, since an
  `ExternalAddress` holding a non-retained or already-freed object would
  produce exactly this.

  Note also that the claim in `Primitives.cpp` (~line 27305) that "the VM
  thread has a long-lived autorelease pool (pushed at startup)" is wrong —
  nothing calls `objc_autoreleasePoolPush`. Objects land in the thread's
  implicit pool, which is what TSD cleanup drains.

- ~~VM cannot be re-launched after quit without restarting the process~~
  Fixed. Relaunch support landed with `vm_destroy` and the remaining bugs were
  cleared through build 122. Verified 2026-08-16 with `test_relaunch`: three
  launch/quit/relaunch cycles on a fresh Pharo 13.1 image, each running ~53M
  bytecodes with input events consumed. See `src/platform/test_relaunch.cpp`.

## VM Defects Found But Not Yet Fixed

The seven defects found while clearing the clang 21 warnings (2026-08-09) were
all resolved on 2026-08-16. See `docs/changes.md` under build 122 for what each
fix does and why. In summary:

  1. fullGC ignoring a partial compaction plan — fixed. The compactor now runs
     multiple passes, the way Spur's planning compactor does.
  2. sweepGC overcounting overflow objects by 8 bytes — fixed. Latent, not
     live: sweepGC is public but has no callers in the tree.
  3. primitiveObjectPointsTo missing the 16-bit format guard — fixed, and
     widened to cover the reserved formats too.
  4. primitiveSetDisplayMode claiming success without acting — removed. It was
     dead code: modern Pharo reassigned 90-93 to PermSpace and leaves 232-234
     null, so it was never in primitiveTable_.
  5. primitiveWarpBits and colour maps — the sourceMap case cannot arise while
     the primitive is restricted to 32bpp source and destination, because
     `colormapIfNeededForDepth: 32` answers nil for a 32bpp form. The receiver's
     own colorMap field was the real gap and was being ignored outright; it now
     fails to Smalltalk.
  6. The crash dump's hardcoded `lastGCStep=0` — replaced with the real
     bytecode count and the bytecode count at the last GC.
  7. primitiveStringEncode/Decode stubs — removed as dead code. VMMaker reserves
     520-540, so neither was in the table, and Pharo encodes in Smalltalk.

## FFICallbackTest: fixed — the woken handler was immediately displaced

`FFICallbackTest` and `FFICallbackParametersTest` both pass now, in under a fifth
of a second each, where they used to run forever.

The earlier note here had the shape of it wrong. The handler was not looping in
argument marshalling; it never ran at all. Instrumenting the signal showed the
semaphore was found and signalled correctly, and then:

    [CB] semIdx=1 depth=1
    [CB] sem=obj cls=Semaphore
    [CB] woke process prio=40      <- should be 70

`TFCallbackQueue` runs its handler at `Processor highIOPriority`, 70, against a
test process at 40. `enterInterpreterFromCallback` did two things in sequence
that cancel each other out:

    signalSemaphoreDirectly(g_callbackSemaphoreIndex);   // step 5
    Oop readyProcess = wakeHighestPriority();            // step 6

Step 5 goes through `synchronousSignal`, which compares priorities and, when the
waiter wins, calls `putToSleep` on the **active** process and transfers to the
waiter. Both halves are wrong in this context. The active process is blocked in a
C callout and must not go back on a ready queue at all — it is already recorded on
`SuspendedProcessInCallout`. And having queued it, step 6 promptly found it there,
made it active and ran it, abandoning the handler that had just been woken.

So the handler never reached `primNextPendingCallback`, never answered, and the
10M-step timeout eventually abandoned the callback. `qsort` saw a comparator that
never returned a value and left the array untouched — which is why the result
looked like a sort failure rather than a callback failure. Worth noting the
earlier reading of the symptom was wrong too: the array came back in its original
descending order, not reversed, and the two look identical when the input is
already descending.

The fix is a `signalSemaphoreMakingRunnable` that only unlinks the waiter from the
semaphore and puts it on its ready queue, leaving the active process alone and
leaving the choice of what to run next to the `wakeHighestPriority` that follows.

### TFCallbacksTest: a missing fixture, not a hang

`TFCallbacksTest` appeared to hang too, and that is a different problem again. The
class calls C functions — `shortCallout`, `singleCallToCallback` — out of
`libTestLibrary.dylib`, which the Pharo VM distribution ships and we do not. It is
nowhere on this machine.

Tracing the callback needed unbuffered stdout from inside it, since the nested
interpreter never returns to report anything. The block exits abnormally between
"args marshalled" and the `ensure:`, and the reason is:

    SymbolNotFoundError: Could not find symbol named: 'shortCallout'
        searching in module: 'libTestLibrary.dylib'

The image raises inside the callback, so the callback never answers, so the VM
sits in its nested interpreter running the idle process until the callback
timeout. From outside that is indistinguishable from a hang, which is how it was
recorded.

`scripts/build-ffi-test-library.sh` builds the library from the `ffiTestLibrary`
sources in a pharo-vm checkout. With it present the class runs in about a quarter
of a second:

    8 ran, 4 passed, 2 skipped, 4 errors

and the errors are specific and honest. Run individually, all five tests pass on
the same-thread runner. The four errors are those same tests under the *worker
thread* runner, every one of them:

    PrimitiveFailed: primitive #primitiveCreateWorker in TFWorker failed

So what is actually missing is threaded-FFI worker support — a real gap, but a
narrow and named one, and nothing to do with callbacks or with hanging.

The class stays on the skip list, because without the library it still spins and a
batch runner cannot be assumed to have it. Anyone working on TFFI should build the
library and run the class directly.

One methodological note, because it cost time here: before the library existed,
`testReentrantCalloutsDuringCallback` "passed" when run on its own. It was not
passing — it was failing before it reached its assertion. A green result from a
test whose fixture is missing is worth nothing.

### Fixed along the way: the image quitting mid-callback

The nested interpreter runs the *highest-priority ready process*, which need not
be the callback handler. Dumping the Smalltalk stack at the quit shows what it
actually was:

    #defaultAction      rcvr=Exit
    #handleSignal:      rcvr=nil
    #pass               rcvr=Exit
    #handleExit:for:    rcvr=PharoCommandLineHandler

So `PharoCommandLineHandler` finished its `eval`, signalled `Exit`, nothing
handled it, and `Exit>>defaultAction` called `primitiveQuit` — while `qsort`
still had frames on the C stack expecting the comparator to return. Honouring
that quit set `running_ = false`, which made the nested `while (running_)` loop
*return* instead of unwinding, reaching the branch in `callbackClosureHandler`
commented "should never reach here": it zeroed the C return buffer, freed the
context and handed `qsort` a meaningless answer.

`primitiveQuit` now defers when `callbackDepth_ > 0` and `interpret()` honours
the quit once the callback stack has unwound. You cannot tear down the VM while
C frames are waiting on a return. Normal quit is unaffected (`callbackDepth_` is
zero, honoured immediately — measured at 17s for a plain `eval`, and relaunch
still passes).

That removes the stranding and the use-after-free, and the single test now runs
to a failure instead of being abandoned. It does **not** make the tests pass:
the handler still never calls `primitiveCallbackReturn`, so the nested loop
still does not exit on its own and only the 10M-step bail-out ends it.

Where to look next: why the handler process, once woken and made active, does
not reach `TFCallbackInvocation >> primCallbackReturn`. Entry points are
`Interpreter::enterInterpreterFromCallback` and `callbackClosureHandler`
(`Primitives.cpp`). Note also that the 10M-step bail-out is commented "~1s",
which only holds at full speed — parked in `relinquishProcessor` the same 10M
steps take over two minutes. It should be wall-clock.

## The 90-Second Stuck-Process Killer — Removed

`interpret()` and `step()` each carried a wall-clock guard that terminated any
process below priority 79 which stayed active for 90 seconds, by nil'ing its
`suspendedContext` and switching away. That is the silently-terminate-a-process
pattern CLAUDE.md forbids, and it was written to paper over the starvation bug
fixed on 2026-08-16.

Deleted, on evidence that it had never fired and structurally could not:

  - No `[VM-TIMEOUT]` line in any log from this work, including a full
    1,675-class suite run.
  - Before the preemption fix it could not fire at all: the guard lives inside
    the same periodic-check block the extension-byte alignment was starving, so
    in the one situation it targeted it was starved too.
  - After the fix it still does not fire. `trackStartTime_` resets whenever a
    *different* process is seen at the 64K checkpoint, so 90 seconds of one
    process being continuously active is required — and the 2ms force-yield
    round-robin plus the idle process means that does not happen. Tested
    directly: a single low-priority computation running 400 seconds straight
    was never touched.

So it was dead code that, had it ever become reachable, would have killed
legitimate long computations. Removed along with `trackedProcess_`,
`trackStartTime_`, `cumulativeMs_`, `lastResumeTime_` and
`startupGracePeriod_`, which nothing else used.

Checked after removal: WarpBlt 8/8, pointsTo 8/8, relaunch 2/2 cycles, GC heap
clean. `BlockClosureValueWithinDurationTest` is 11/12 over repeated runs, the
one failure being `testValueWithinTimingRepeat`, a wall-clock assertion
(`milli < 500` across three 100ms timeouts) that is load-sensitive — not
`testValueWithinNonLocalReturn`, which is the case the preemption fix
addressed and which passes consistently.

## BitBlt: colour fidelity of upward depth conversions

`primitiveCopyBits` was a chain of hand-written `destDepth == a && srcDepth == b`
blocks covering 21 of the 36 combinations of {1, 2, 4, 8, 16, 32}; the other 15
fell through and failed, which the image reports as the misleading
"Bad BitBlt arg (Fraction?)" — that is what its fallback raises when the
primitive fails and no recovery path applies, since on the reference VM BitBlt
handles every combination.

**Fixed 2026-08-16.** All fifteen were sub-32bpp to sub-32bpp, i.e. raw
pixel-value moves between palette depths rather than colour conversions, so one
generic loop covers them: read the source pixel, put it through the colour map
if the caller supplied one, write it at the destination depth. All 36
combinations now succeed.

Two layout facts the generic path depends on, both established by writing known
values through `pixelValueAt:put:` and reading the raw `bits`:

  - Depth 8 is MSB-first within each 32-bit word (1..8 gives 16r01020304
    16r05060708).
  - Depth 16 is too (1..4 gives 16r00010002 16r00030004).

The pre-existing `16->16` handler treats its bits as a little-endian `uint16`
array, which disagrees — it survives only because it reads and writes with the
same wrong convention, so the error cancels. Anything cross-depth cannot rely on
that.

Measured on upward conversions (`asFormOfDepth:`, colour compared per pixel),
current VM against the pre-session VM at 44df4146:

                                    44df4146   current
    pairs that error outright          7          0
    pairs with wrong colours           7          7
    1->8, 2->8, 4->8                   8 wrong    0
    total bad                         14          7

### Still open

### The seven "colour-fidelity gaps" are not defects (re-measured 2026-08-16)

The seven were conversions *to* 16 and 32 bpp (`2->16`, `2->32`, `4->16`,
`4->32`, `8->16`, `8->32`, `16->32`) where a colour did not survive. Re-measured
pixel by pixel after the conversion fixes, all of them reduce to three properties
of the reference algorithm itself. None is a bug, and the item is closed.

**Widening 5 bits to 8 zero-fills, so 31 becomes 248, not 255.** Every colour in
a `16 -> 32` conversion therefore comes back at 0.9717 rather than 1.0. This is
`rgbMap16To32:`, it is what `rgbMap:from:to:` means by "Expand to more bits by
zero-fill", and the golden PNGs stored in the image confirm it — they carry an
`sBIT` chunk of `5 5 5 1` declaring the low three bits padding, and their red
channel is 248.

**Black at 16bpp is pixel value 1, not 0.** Zero means transparent, so
`mapPixel:flags:` refuses to let a visible colour reduce to it. Widening that 1
gives blue = 8, so black round-trips as `(0, 0, 0.031)`. Also correct.

**Five bits cannot hold 0.5.** `Color gray` quantises to 16/31 = 0.516 rather
than 15/31 = 0.484. That is image-side rounding in `Color>>pixelValueForDepth:`,
not VM behaviour at all. It accounts for every mismatch in the `2->16`, `2->32`,
`4->*` and `8->*` rows — and the apparent count of five in `2->16` is one
discrepancy seen five times, because a depth-2 palette has already collapsed most
of the sample colours to grey.

The reason this looked like a defect for so long is that the measurement compared
`Form>>colorAt:`, which scales a 5-bit component by /31, against BitBlt's
conversion, which zero-fills to n<<3. Both are right in their own terms and they
disagree by construction, so any test that mixes them reports failures on a
correct VM. `scripts/test_bitblt_depth_matrix.st` avoids the trap by asserting
position independence on pixel values instead.

### PNGReadWriterTest: 16 passing to 28

Its failures turned out not to be the depth combinations at all. Instrumenting
every `return PrimitiveResult::Failure` in `primitiveCopyBits` — via a helper
taking only `__LINE__`, since a rewrite referencing locals does not compile in
all 53 places — named two further gaps, both now fixed:

  - **Negative depths.** A negative `Form` depth is Squeak's marker for raw byte
    order rather than MSB-first. PNG decodes into a depth **-8** destination,
    which no handler covered, so it fell to the failure tail. The generic path
    now takes the sign into account on both source and destination.
  - **Combination rule 40, `fixAlpha`, with no source form.** `Form>>fixAlpha`
    uses it to repair the alpha channel of a 32bpp form after compositing has
    left coloured pixels transparent: a pixel that is not wholly zero becomes
    opaque, a zero pixel stays transparent.

    42 ran, 16 passed, 26 errors        before
    42 ran, 28 passed, 3 fail, 11 err   after

### Negative depths — fixed

A negative depth means non-MSB pixel order. Squeak derives it from
`destMSB := depth > 0` and shifts by `(i \\ pixPerWord) * depth` instead of
`32 - ((i \\ pixPerWord) + 1) * depth`, so pixel index ascends within the word.
That is *not* byte-swapping the word, though the two coincide at depth 8 where a
pixel is a byte — so implementing from depth-8 evidence alone gets it wrong.

At depth 32 the pixel is the whole word, so MSB order says nothing about it and a
`32 -> -32` copy must leave the bits untouched. `PNGReadWriterTest` states this:

    original depth = 32
        ifTrue:  [ self assert: original bits equals: reversed bits ]
        ifFalse: [ self deny:   original bits equals: reversed bits ]

The reason this took three attempts is worth recording. `primitiveCopyBits`
normalises a negative *source* depth to positive early, keeping the sign in a
separate `srcNeedsByteSwap` flag:

    bool srcNeedsByteSwap = (srcDepth < 0);
    if (srcDepth < 0) srcDepth = -srcDepth;

So any later test of `srcDepth < 0` is dead, and a negated-depth source silently
matched the ordinary positive handlers, which know nothing about the ordering.
Only the *destination* kept its sign, which is why writing to a negated-depth
form appeared to work while reading one back did not. The generic path now reads
the flag rather than the sign, and runs ahead of the specific handlers whenever
raw order is involved, so those cannot claim it first.

All depths now round-trip: direct read/write on a negated-depth form is 32/32 at
1, 2, 4, 8, 16 and 32, conversion preserves every pixel, and the bits invariant
above holds. Covered by `scripts/test_bitblt_lowdepth.st` — 9/9 here, 2/9 on the
pre-session VM.

### The four PNG failures — all fixed (2026-08-16)

`PNGReadWriterTest` is now 42 of 42. The four were three unrelated defects plus
one shared one, and none of them was where the earlier note guessed.

**5-bit components were widened by scaling instead of zero-fill.** Converting
1-5-5-5 to 8-8-8 used `* 255 / 31`, so a full component came out 255. The
reference zero-fills — `rgbMap16To32:` is three shifts and `rgbMap:from:to:` says
"Expand to more bits by zero-fill" — giving 248. The golden PNGs stored in the
image settle it: they carry an `sBIT` chunk of `5 5 5 1` declaring the low bits
padding, and their red channel is 248. That was `testPngEncodingColors16`.

**16bpp rows were strided by pixel width rather than by the padded row.** Form
rows pad to whole 32-bit words, so at 16bpp an odd-width row owns one more pixel
slot than it has pixels. Striding by the width loses one pixel per row and shifts
the image progressively sideways. Even widths hide it exactly, which is why only
the 33-pixel-wide tests caught it. Seven index sites and three bounds checks were
wrong together — the checks had been made consistent with the wrong stride.
`BMPReadWriterTest>>testBmp16Bit` was a casualty of the same bug and now passes.

**The generic raw-order path ignored shift/mask colour maps.** It handled only an
indexed table. `PNGReadWriter>>writeType6DataOn:` installs a shift/mask map to
swap red and blue on the way out, so the encoder skipped the swap while the
decoder still undid it — every pixel returned with red and blue exchanged. That
was `test32BitReversed`.

**No implicit colour map was ever built.** `loadBitBltFrom:` installs one whenever
the supplied map is not a new-style `ColorMap` object, commented there as "Need
the implicit setup here in case of 16<->32 bit conversions". Without it a 16bpp
source copied to 32bpp with no map kept its raw 5-5-5 value unwidened. With the
stride fix this closed `test16BitReversed`.

**Alpha was being stashed in the unused top bit of a 1-5-5-5 pixel.** The 32-to-16
fallback did `pixel |= 0x8000` for any opaque source. The reference's implicit
8-to-5 map sets the alpha mask to zero, so that bit stays clear. Nothing in the VM
read it back, so it looked harmless — but PNG has nowhere to store it, so every
pixel returned from a roundtrip with bit 15 cleared and compared unequal to the
form it came from. That was `test16BitDisplay`, the last one.

The earlier lead recorded here — that encoding a form and its negated-depth copy
produced different byte counts — was real but pointed the wrong way. It was the
missing colour map in the raw-order path, not anything about `allocateForm:` or
`nativeDepth` propagation.

Two further rejections are left and both look correct rather than missing: a

no-source fill to a non-32bpp destination, and the guard refusing a ByteArray
source smaller than `width*height*4` (478 bytes for a 16x16x32 form, so copying
really would read out of bounds).

No visual regression: the Mac Catalyst app renders byte-for-byte identically
(0 differing pixels of 753,664), and `BitBltTest`, `FormTest`, `ColorTest`,
`BitBltClipBugsTest` and `scripts/test_bitblt_lowdepth.st` all pass.

## Most "hanging" tests were an unoptimised build (2026-08-16)

`build/` is configured `CMAKE_BUILD_TYPE=Debug`, and this project's Debug flags
are `-g` with **no `-O` at all**. Every test result and every timing ever recorded
here was measured on a completely unoptimised VM. An `-O3` build of the same
source, same commit:

    benchmark                      Debug      Release    ratio
    empty loop, 1M iterations       85 ms      12 ms      7.1x
    add loop, 1M                   147 ms      18 ms      8.2x
    ByteArray at:put:, 1M          397 ms      38 ms     10.4x
    Dictionary at:put:, 100k      1425 ms      75 ms       19x
    Dictionary at:, 100k          1031 ms     375 ms      2.7x
    OrderedCollection new, 200k   4350 ms     888 ms      4.9x

At 12 ms for a million iterations the interpreter is doing about 83M of them a
second, which is respectable. On the Debug build it looked ten times worse than
it is.

That matters because SUnit fails a test that runs longer than its time limit with
`TestTookTooMuchTime`, reported as an **error**. On an 8x-slow VM that fires on
tests which are merely big, and it is indistinguishable in a batch summary from a
crash or a hang. Re-run on `-O3`:

    class                          Debug                  Release
    ClyBrowserToolValidityTest     25 errors, 270 s       25 pass, 42 s
    StDebuggerTest                 did not finish, 280 s  60/61 pass, 59 s
    GIFReadWriterTest              1 error (42 s test)    3 pass (4.2 s test)
    NoUnusedVariablesLeftTest      did not finish         2/3, 3 m 50 s
    FastStepThroughTest            did not finish         still does not finish
    FFICallbackTest                did not finish         still does not finish

So the skip list was carrying three entries that were never broken. It also means
the earlier note about `become` — "~200 ms per heap scan, 650+ calls" — was
measuring `-O0`; the algorithmic fix there was still right and still needed, but
the residual cost quoted was inflated by the build.

**Run the suite against a Release build.** A Debug build is for debugging; using
it to judge whether a test passes produces false errors that look exactly like
real ones.

The two that still do not finish under `-O3` are genuine and unrelated to speed:
`FFICallbackTest` (the callback handler loops instead of reaching
`primitiveCallbackReturn`) and `FastStepThroughTest`.

## The external objects table defaulted to 20 slots (fixed)

VM parameter 49 is the ceiling the image uses for concurrently registered
external objects — semaphores handed to the VM for signalling. The reference
answers its own table size, which starts at `INITIAL_EXT_SEM_TABLE_SIZE`, defined
as **256** in `sq.h`. We answered the slot count of the array the image happens to
ship, and a fresh Pharo image ships **20**.

That is not merely a smaller number. On reaching it,
`VirtualMachine>>maxExternalSemaphores:` raises

    Not enough space for external objects, set a larger size at startup!

Growing on demand does work — setting parameter 49 enlarges the array and the
image asks for that — but only after the error has already been signalled. So any
test registering more than twenty external objects failed on a limit the
reference VM does not have.

Now brought up to 256 when smaller, preserving whatever the array already held.
`TFBasicTypeMarshallingInCallbacksTest` loses that error entirely; its remaining
18 errors are all one known gap, `primitiveCreateWorker`, which is threaded-FFI
worker support and unimplemented.

## Non-local return went to the wrong activation (fixed)

A `^` inside a block returns from the activation that lexically created the
block. The VM found that activation by walking the frame stack top-down for one
running the block's home *method*. Method identity cannot tell two activations
apart, and top-down picks the innermost — so whenever the home was an **outer**
activation of a method also on the stack deeper in, the return went to the wrong
frame and simply did not do what `^` means.

`BlockClosure>>valueWithExit` is that shape, and it is not exotic:

    valueWithExit
        ^self value: [ ^nil ]

The exit block's home is the `valueWithExit` activation, so any nested use puts
two activations of that one method on the stack. The outer exit then returned
from the inner activation, which does not stop the outer loop:

    [ :break |
       [ true ] whileTrue: [
          [ :continue | ... break value ... ] valueWithExit ] ] valueWithExit

Fixed by matching the closure's `outerContext`, which names the activation
exactly and which `createFullBlockWithLiteral` already materialises. The match is
accepted only when the frame is also running the home method, so it can never
land somewhere worse than the old search.

### How it surfaced

As `RSLinePlotTest` never finishing and taking the rest of the run with it.
`NSNiceLinearTicksGenerator` is built from four nested `valueWithExit` blocks; its
`break` never fired, so the tick search spun forever. The generator produced **no
ticks at all** for any ordinary axis range — 0..10, 0..1, 1..100, -1..1 — and
only terminated on the degenerate 0..0 case.

Getting there took ruling out the obvious suspects first, and it is worth
recording what was *not* wrong, because each looked plausible: Float semantics,
including NaN and infinity comparison, `isFinite`, `raisedTo:` overflow and
`ceiling`/`floor` on negatives; Fraction arithmetic and mixed-mode comparison;
and closure temp sharing, including writes to an enclosing block's temporary from
a nested block. All correct. The evidence that finally pointed at control flow was
instrumenting the generator and seeing its exponent `z` increment exactly once and
then stop while the loop kept re-entering — which no arithmetic bug explains.

    RSLinePlotTest   never finished, killed the run  ->  31 ran, 28 passed, 31 s

Covered by `scripts/test_nonlocal_return.st`: 5 of 5 here, 2 of 5 on the pre-fix
VM. The tests are time-guarded, so on a broken VM they fail rather than hang.

## Directory enumeration reported "." and ".." (fixed)

`primitiveReaddir` returned every entry `readdir` gave it. Filtering `.` and `..`
is the plugin's job — the reference `FileAttributesPlugin` loops until it has an
entry that is neither:

    if ((!(entry->d_name[0] == '.' && entry->d_name[1] == 0)) && strcmp(entry->d_name, ".."))
        haveEntry = 1;

That is not cosmetic. The image turns each entry into a `FileReference`, so `..`
becomes the parent directory and `.` becomes the directory itself under its own
name. A directory holding three files answered five children, one of them its
parent. Anything that walks a tree then walked upward too, and
`FileReference>>deleteAll` climbed toward the filesystem root and never
terminated.

**This is what the Fuel "hangs" were.** Every individual Fuel test passed; the
class hung in `TestSuite>>runWith:`'s `ensure: [ self tearDown ]`, which for those
classes is a `deleteAll` of the resource directory. Chasing it required noticing
that the tests all completed and the hang was after them, and that the hang was
CPU-busy rather than idle.

It was not only hangs. Anything touching the filesystem paid to walk more of it
than it asked for. Measured before and after, same commit otherwise:

    GlobalIdentifierFuelPersistenceTest    100.6 s   ->    28 ms
    GlobalIdentifierStonPersistenceTest    skipped   ->     2 ms
    OmSessionStoreTest                     skipped   ->   320 ms
    EpLogTest                              skipped   ->   295 ms
    MCDirectoryRepositoryTest              skipped   ->   436 ms
    FLContextSerializationTest             never finished -> 1.0 s
    FLProcessSerializationTest             never finished -> 0.4 s
    FLBlockClosureSerializationTest        never finished -> 1.9 s
    FLFullBlockClosureSerializationTest    never finished -> 1.0 s

Covered by `scripts/test_directory_enumeration.st`. On the pre-fix VM that class
cannot complete at all — its own `tearDown` is a `deleteAll` — so the control run
spins through 4.6 billion bytecodes and never returns.

### FileAttributesPluginPrimsTest crashed the VM (fixed)

A directory handle is a plain `ByteArray` holding a `DIR*`, which makes it
indistinguishable from any other bytes object — a `String` of the right length
looks exactly like one. `primitiveClosedir` therefore read whatever bytes it was
given as a pointer and passed it to `closedir()`. The test does this deliberately:
`testPrimCloseDirString` hands it a String, and the VM died in
`primitiveClosedir`.

The handle cannot be validated by class, so the VM now keeps the set of `DIR*`
values it has issued, and `readdir`, `rewinddir` and `closedir` only accept a
pointer from that set. Anything else fails the primitive. The reference plugin
does not need this because its handle is a struct it owns, but it still refuses a
corrupt one with `FA_CORRUPT_VALUE` rather than dereferencing it.

Failing was not quite enough: the tests check *which* error, and a bare failure
sends the image down its generic `signalError:for:` path. The primitives now set
`primFailCode_` to 3, "bad argument", which is what the image looks for. Same for
`primitiveFileExists` given a nil path.

    6 ran, 0 passed, 5 failures, 1 error, and a VM crash   before
    6 ran, 4 passed, 2 failures                            after

Worth noting for anyone adding an error code: the plugin headers define a
`PrimErrBadArgument` macro, so the constant has to be spelled with a trailing
underscore. That is why `PrimErrNoModification_` already had one.

The two that still fail need image classes this Pharo 13.1 image does not have
(`FileAttributesPluginPrims` is absent, so `primPathMax` answers KeyNotFound), so
they are not VM problems.

## Allocation-heavy work spent half its time collecting

GC headroom was a fixed 32MB, so a full GC ran every 32MB allocated however large
the heap was — and there is no generational collector, so every GC marks and
compacts the whole heap, about 250ms for a stock image's 740k objects. Headroom
now scales with the live set, collecting when the heap has grown by its own size.

    benchmark                 fixed 32MB    adaptive
    allocation-heavy loop        6.46 s      5.57 s
    array of 200k objects        3.99 s      2.86 s
    full GCs                       30          16
    peak heap used                58 MB       58 MB

Half the collections, 14-28% faster on allocation-heavy work, and no measured
memory cost.

## Both stepping problems were one bug: a 10000-context walk limit (fixed)

`StepThroughTest suite run` never finished and `FastStepThroughTest>>
testStepThroughLonger` failed its assertion. They were recorded as two stepping
bugs for months. They were one bug, and it is not about stepping.

Primitives 195 (`findNextUnwindContextUpTo:`) and 197
(`findNextHandlerOrSignalingContext`) walk a context's sender chain. Both stopped
after **10000** contexts and then answered **nil** — which does not mean "I
stopped looking", it means "there is no such context", and every caller believes
it.

10000 is not a large stack. `StepThroughTest>>stepC1` recurses exactly 10000 deep
as a matter of course, putting its chain at 10009 — one context past the limit.

The chain of consequences, each verified by tracing:

  - `Context>>unwindTo:` finds no unwind contexts, so it runs no `ensure:` or
    `ifCurtailed:` blocks at all.
  - `Process>>terminate` terminates another process by inserting an `ensure:` at
    the bottom of the target's stack and waiting on the semaphore that ensure
    signals. With the ensure never run, terminate kills its target and **never
    returns to its caller** — observed directly: `isTerminated` true within five
    seconds while `[p terminate] timeToRun` never returned.
  - `TestExecutionEnvironment`'s `cleanUpAfterTest` terminates processes a test
    left behind, so the whole test run stopped there. That is why the tests all
    passed under `runCase` and hung under `runCaseManaged`.
  - An exception raised deeper than the cutoff finds no handler and goes
    unhandled with a handler sitting right there.

Fixed by making the bound a corruption guard far above any real stack, and by
**failing** the primitive when it is exceeded rather than answering nil — the
Smalltalk fallback walks the same chain with no limit at all.

    StepThroughTest         never finished  ->  11 of 11, 0.44 s
    FastStepThroughTest     10 of 11        ->  11 of 11, 0.03 s

Covered by `scripts/test_deep_stack_unwind.st`, which checks all four
consequences at a depth of 12000: 4 of 4 here, and on the pre-fix VM the class
cannot complete at all, because its own terminate never returns.

Ruled out along the way, each of which looked plausible: a cyclic sender chain
(the chain is 10007 long, ends at nil, not cyclic); stack depth as such
(terminating a deliberately 5000-deep process runs its ensure and returns); the
synthesized ensure context being malformed (its `complete` temp is nil and
`isUnwindContext` is true, and resuming a process straight into one runs the
block); and plain termination generally (a Delay-blocked, a Semaphore-blocked and
a deep suspended process all unwind correctly).

## Deep recursion silently killed the process (fixed)

`StackOverflowLimit` was 4096 frames, and exceeding it made `handleStackOverflow`
terminate the running process without a word. Both halves of that were wrong.

**4096 is not a runaway; it is ordinary code.** The reference VM has no such limit
— Cog spills frames to the heap and is bounded only by memory, so recursion
thousands deep is unremarkable there. `StepThroughTest` recurses 10000 deep as a
matter of course. The VM's own `MaxFrameDepth` is 65536, so the arrays were
already sized for sixteen times what the limit allowed.

**Terminating silently made it undiagnosable.** The process simply stopped
existing. Anything waiting on it waited forever, and the VM sat in its idle
process — so from outside, a silently killed process and a deadlock look
identical. That is how `FastStepThroughTest` came to be recorded as a hang for
months. Bisecting the recursion depth is what finally exposed it: 1000, 2000 and
4000 passed, 5000 and 6000 did not, and a boundary that sharp is a constant, not
a race.

Fixed by raising the limit to 16384 and making the termination announce itself on
stderr with the process, its priority, and the depth reached. Raising it costs no
memory — `savedFrames_` was already sized for `MaxFrameDepth`. `MaxStackDepth`
went from 131072 to 524288 to keep 32 value-stack slots per frame at the new
limit; that ratio matters, because exhausting the value stack calls `stopVM` and
takes the whole VM down, whereas exhausting frames only ends one process, so the
frame limit must be the one that trips first.

This is still not what Cog does, and the comment in the code says so. Sending a
Smalltalk error would be better than terminating, but Pharo has no stack-overflow
exception to send — precisely because its VM never needs one — and inventing a
selector would diverge from what the image expects.

`FastStepThroughTest` now completes in about a second, 10 of 11 passing, with no
process terminated.

### What is left of FastStepThroughTest

`testStepThroughLonger` now fails an assertion instead of hanging:

    Got StepThroughTest>>#stepC1 instead of a CompiledBlock: [ self stepC2 ].

The VM-side machinery it depends on is working, all of it verified by tracing:
the block substitution takes (slot 2 holds the `HaltingBlock` and reads back
identical), `HaltingBlock>>valueWithArguments:` is reached, the process-specific
`DebuggerSteppingState` reads `true` inside the stepped process, and `Break break`
is signalled. So the interruption fires correctly and at the right moment.

What is wrong is where execution is reported afterwards: the session's
interrupted context is `stepC1`'s activation rather than a context for the block
that was about to run. That is `FastStepThroughController`'s reconstruction step —
`buildBlockContext:args:sender:` and `findNextContext:` — and it is where to look
next. The non-fast `StepThroughTest>>testStepThroughLonger` passes, so this is
specific to the fast path.

## becomeForward was quadratic: one heap scan per pair

`elementsForwardIdentityTo:` and friends (primitives 197, 248, 249) walked the
whole heap **once per element pair**. `ObjectMemory::becomeForward` scans every
object, so an array of N pairs cost N full scans — against a 740k-object heap
that is hundreds of millions of object visits for a single primitive call.

That is what made `StDebuggerTest` and `ClyBrowserToolValidityTest` look like
hangs. They are not deadlocked; sampling showed 571 of 598 samples inside
`primitiveArrayBecomeOneWay`. They were simply doing quadratic work.

Fixed 2026-08-16: all three call sites now collect their pairs into a map and
call a new `ObjectMemory::becomeForwardAll`, which does **one** scan resolving
every pair. Same traversal, same format guards, same semantics —
`BecomeTest` 8/8, `ObjectTest` 28/28, `MethodDictionaryTest` 36/36.

`ClyBrowserToolValidityTest` now completes (25 ran, 25 errors, ~270s) where it
previously ran past 240s without finishing. `StDebuggerTest` gets much further
but still does not finish inside 280s, because the remaining cost is real:
measured at ~200ms per heap scan with 650+ become calls in that one class.

Both stay on the skip list for now — a class that needs minutes still costs a
batch. What is left is the per-scan cost, not the algorithm. Two things to note
before optimising it further: these measurements are all Debug builds, and
`allObjectsDo` dispatches through `std::function` for every object, so the
per-object indirect call is a large share of that 200ms. Making the scan a
template would help both. The reference VM avoids the problem differently, using
forwarding objects with lazy resolution rather than scanning at all.

## Dead Primitive Implementations

Roughly 370 of the 814 `Interpreter::primitive*` methods are never installed in
`primitiveTable_` and never passed to `registerNamedPrimitive`, so they cannot
be reached. Several are shadowed by a real implementation elsewhere — for
example `Interpreter::primitiveSocketConnect` is dead while the live socket code
is `sp_primitiveSocketConnectToPort` in `src/vm/plugins/SocketPlugin.cpp`.

This matters because the dead copies carry primitive numbers in their comments
that no longer match the table, which is how defects 4 and 7 above came to be
described as live primitives. Reproduce the list with:

    python3 - <<'PY'
    import re, pathlib
    src = ''.join(p.read_text(errors='ignore')
                  for p in pathlib.Path('src').rglob('*')
                  if p.suffix in {'.cpp', '.hpp', '.inc', '.h'})
    defs = set(re.findall(r'PrimitiveResult\s+Interpreter::(\w+)\s*\(', src))
    print(sorted(d for d in defs if not re.search(r'&Interpreter::' + d + r'\b', src)))
    PY

Not yet cleaned up: it is a large mechanical deletion that wants its own pass.

## Build Toolchain

322 compiler warnings remain, all of them in VMMaker-generated plugin sources.
They are not suppressed. See `docs/vmmaker-issues.md` for the root-cause
analysis, which traces four of the five findings to Slang's inliner in
pharo-project/pharo-vm.

The fifth — the signed/unsigned comparison in DSA's big-integer division — was
described there as a genuine logic defect with the question of reachability
left open. Settled 2026-08-16: neither operand can be negative, because the
digits are `unsigned char` and the quotient estimate cannot fall below zero
(the correction step's test is false once `q` reaches 0). The comparison is
correct and the warning is a false positive; the proof is in
`docs/vmmaker-issues.md` section 5.

A full CMake build also requires `Frameworks/` to be populated. That directory
is gitignored, so a fresh checkout has to build the xcframeworks once, with all
three scripts — `build-third-party.sh` does not produce SDL2 or libffi:

    ./scripts/build-libffi.sh
    ./scripts/build-sdl2.sh
    ./scripts/build-third-party.sh    # --no-crypto skips OpenSSL/libssh2/libgit2

CMake now checks for the SDL2 and libffi slices at configure time and names
these scripts, rather than letting the build run on and die on a missing
`ffi.h` while compiling `Primitives.cpp`.

## Image Bugs We Patch via startup.st

See `docs/image_issues.md` for full details and workarounds.
See `docs/upstream-proposals.md` for proposed upstream fixes.

  1. MicGitHubRessourceReference >> githubApi — nil token causes KeyNotFound
  2. MicDocumentBrowserModel >> document — sends #message instead of #messageText
  3. MicDocumentBrowserPresenter >> childrenOf: — missing outer error handler
  4. Menu shortcut symbols render as "?" — embedded font too old (v2.020)
  5. WarpBlt >> mixPix: drops alpha channel — Smalltalk fallback only averages RGB
  6. Doc browser bullets render as "?" — same font issue as #4

## Upstream Test Bugs (not our problem, not patched)

  7. DebugPointTest >> testTranscriptDebugPoint — fails on all VMs (missing Transcript clear + headless incompatible)
  8. ProtoObject >> pointersToExcept:among: sends removeAllSuchThat: to the
     Array that select: answers, so ProtoObjectTest >> testFastPointersTo
     errors with ShouldNotImplement. Confirmed 2026-08-16 to be unrelated to
     the primitiveObjectPointsTo fix — identical error with and without it.

## Test Status

28,071 tests across 2,046 classes. Zero VM-specific failures.
See `docs/test-results.md` for full breakdown.
