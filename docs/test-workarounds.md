# Test Runner Workarounds

Status of running `./test_load_image Pharo.image test "Kernel-Tests"` on a
stock, unmodified Pharo 13 image — matching what the standard Pharo VM does.

## Goal

    pharo Pharo.image test "Kernel-Tests"        # standard Pharo VM
    ./test_load_image Pharo.image test "Kernel-Tests"   # our VM, same behavior

No test injection, no startup.st patches, no setup_fake_gui.st.

## Current Status (2026-04-07)

The Mac Catalyst app starts up fine in a few seconds — startup speed is NOT
the problem.

**JIT resume is working correctly.** The previous tryResume corruption bug
at 119+ compiled methods was fixed by two commits:
  - Countdown starvation fix (ee0cc57): charge checkCountdown_ after each tryResume
  - selectorBits preservation fix (f4d5a7e): flushCaches preserves slot 12

Verified 2026-04-07: JIT with resume enabled runs 1000+ compiled methods,
no errors, 3x throughput improvement over no-resume, 95% IC hit rate.
The test runner starts and processes test classes successfully.

The test harness (`test_load_image`) calls `interpret()` which runs the
Morphic world loop. The injected test runner (run_sunit_tests.st) triggers
automatically after startup completes.

## Remaining Workarounds

### 1. Test runner injection — KEEP (standard path not working yet)

We inject `run_sunit_tests.st` via the standard Pharo VM (`pharo eval --save`)
instead of using the standard CommandLineHandler flow.

    Standard: pharo Pharo.image test "Kernel-Tests"
              → CommandLineHandler reads args → TestCommandLineHandler runs tests
    Ours: pre-inject test runner via pharo eval --save, then run with our VM

The CLI plumbing exists (setImageArguments, primitiveGetAttribute). The
standard CommandLineHandler path doesn't work yet — likely requires
primitives or session handling that our VM doesn't fully implement.

The injected test runner works: run_sunit_tests.st executes after session
startup, discovers test classes, and runs them with progress tracking.

### 2. startup.st image patches — UPSTREAM IMAGE BUGS

PharoBridge.writeStartupScript() patches ~10 bugs in the stock Pharo 13 image
on every launch. These are image-side bugs, not VM bugs:

    FreeTypeSettings bitBltSubPixelAvailable := false    font probe PrimitiveFailed
    MicGitHubRessourceReference beAnonymous              401 from placeholder token
    MicDocumentBrowserModel document error handler       wrong API (message vs messageText)
    KMShortcutPrinter symbolTable                        Unicode glyphs missing from font
    WarpBlt mixPix:                                      alpha channel lost in fallback
    SystemWindow openInWorld:                             repositions for iOS layout

These patches are harmless and don't affect test results. The standard Pharo
VM doesn't need them because it has a native FreeType plugin, network access
for GitHub resources, and desktop font support.

Status: KEEP. These are genuine image/platform issues, not VM workarounds.
Filed in docs/image_issues.md with upstream requests.

### 3. setup_fake_gui.st — HEADLESS TESTING ADAPTATION

Patches Morph>>activate/passivate to skip nil submorphs in headless mode.
In interactive mode, the layout engine initializes submorphs arrays before
any morph activation. In headless mode, morphs can be created before layout
runs, leaving nil entries. This is a Pharo image issue — not our VM.

The standard Pharo VM has the same problem in headless mode. Cog's test
runner uses a different headless strategy (no Morphic at all for non-GUI
tests), but Spec presenter tests DO need Morphic.

Status: KEEP for GUI/Spec tests. Not needed for Kernel-Tests, Collection-Tests,
or other non-GUI test packages.

### 4. Skipped test classes (~100)

Categories of skipped tests:

    Legitimate (missing features):
      TFFI callback tests — no callback support yet
      Cairo/Athens FFI tests — native libs not available
      Network socket tests — blocking primitives
      Epicea file watcher tests — no inotify/kqueue support

    Needs investigation:
      ProcessTest (kills Delay scheduler) — scheduler robustness
      Filesystem persistence tests (timeout) — may be real bugs
      Tests that modify traits/classes (corrupt state) — may be GC/become issue

Status: REVIEW PERIODICALLY as VM matures.

## Fixed Workarounds (removed)

### primitiveFlushCache skipping flushJITCaches — FIXED (f4d5a7e)

Prim 89 was skipping flushJITCaches() because flushing zeroed
icData[12] (selectorBits), permanently disabling megamorphic cache
probes. Root cause: flushCaches() used memset on the entire 104-byte
IC area. Fixed: zero only the 4 IC entries (slots 0-11), preserve
selectorBits (slot 12). Now prim 89 calls flushJITCaches() normally.

### tryJITResumeInCaller countdown starvation — FIXED (ee0cc57)

Resume loop never charged checkCountdown_, starving periodic checks
when 119+ methods were JIT-compiled. Fixed: charge countdown after
each tryResume call.

### tryResume corruption at 119+ methods — FIXED (ee0cc57 + f4d5a7e)

The two fixes above together resolved the tryResume corruption that
prevented startup.st from loading at 119+ compiled methods. Verified
2026-04-07: no difference between 118 and 119 compiled methods with
resume enabled. Resume provides 3x throughput improvement.

## Test Runner Scripts Reference

    scripts/pharo-headless-test/run_sunit_tests.st    test runner (injected via pharo eval)
    scripts/pharo-headless-test/setup_fake_gui.st     headless Morphic setup (GUI tests only)

## Workarounds in run_sunit_tests.st

### UndefinedObject >> findNextHandlerContext

Safety net for exception handler chain traversal. Probably no longer needed
since prim 197 handles this in C++. Keep until verified unnecessary.

### relinquishProcessorForMicroseconds: instead of Processor yield

Legitimate adaptation — yield is a process switch, not a CPU idle.
primitiveYield has early-exit check for empty priority queues.

### Sort infinite loop detection (200K comparison limit)

Defensive code in test runner. No evidence of triggering in practice.

### Delay scheduler health checks

Detects and reports scheduler corruption between test classes.
Diagnostic tool, not a workaround.

### TestExecutionEnvironment manual reset on timeout

Standard test framework cleanup when watchdog kills a test.

### Session exit fallback (quitPrimitive direct call)

May indicate session shutdown bug. Needs investigation.

### Symbol table corruption detection

Probably obsolete after NLR fix. Keep for safety.
