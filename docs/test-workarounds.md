# Test Runner Workarounds

Status of running `./test_load_image Pharo.image test "Kernel-Tests"` on a
stock, unmodified Pharo 13 image — matching what the standard Pharo VM does.

## Goal

    pharo Pharo.image test "Kernel-Tests"        # standard Pharo VM
    ./test_load_image Pharo.image test "Kernel-Tests"   # our VM, same behavior

No test injection, no startup.st patches, no setup_fake_gui.st.

## Current Status (2026-04-07)

The standard CLI path (`test_load_image Pharo.image test "Kernel-Tests"`)
is plumbed end-to-end: image args are passed via primitiveGetAttribute,
and Pharo's CommandLineHandler should process them after startup completes.

**The blocking issue is speed.** Pharo startup takes <1s on Cog, ~3-5 minutes
on our VM. The standard path hasn't been verified to completion because we
can't sit through a 5-minute startup + multi-hour test suite.

## Remaining Workarounds

### 1. Test runner injection — SPEED WORKAROUND

We inject `run_sunit_tests.st` via the standard Pharo VM (`pharo eval --save`)
because our startup is too slow to use the standard CommandLineHandler path
in practice. This isn't a correctness issue — the CLI plumbing exists and
args are passed. It's purely a speed problem.

    Standard: CommandLineHandler reads args, starts test runner
    Ours: pre-inject test runner, skip CommandLineHandler entirely

Fix: make the VM fast enough that standard startup completes in reasonable
time. JIT Phase 4 (J2J direct calls) and Phase 5 (register caching) are
the path to closing the 100-300x gap.

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
