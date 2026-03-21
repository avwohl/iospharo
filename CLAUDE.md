# Claude Code Instructions

## Formatting
- **Never use markdown tables** (pipes and dashes). They look terrible when
  copy-pasted into email. Use indented plain-text columns or bullet lists instead.
  A fixed-width font can be assumed.

## MANDATORY: Save Progress Every Hour

**Update docs/*.md files and memory files at least once per hour during long-running tasks.**

Long sessions (12+ hours) have crashed without leaving any notes, losing all progress.
This is non-negotiable:

1. **Every ~60 minutes**, update relevant docs/ files with current status, findings, and results
2. **After completing any significant milestone** (test run finishes, bug fixed, etc.), immediately write results to docs/ and/or memory
3. **Before starting a long operation** (full test suite, large build), save what you know so far
4. **Commit work-in-progress** to git at least every 15 minutes (per existing rule below)
5. Use `docs/test-results.md`, `docs/changes.md`, and memory files as appropriate
6. A crash that loses 12 hours of work because nothing was written down is unacceptable

## GUI Testing: MANDATORY Timeouts

**Every GUI test MUST have a hard timeout that actually kills the process.**
GUI testing repeatedly hangs/locks up Claude Code sessions. Never launch the
Mac Catalyst app or interact with it without a kill mechanism.

### Rules:
1. **Always launch with `timeout`**: Use `timeout 60 <command>` (or similar)
   for ANY process that runs the Mac Catalyst app. Adjust seconds as needed
   but NEVER omit it.
2. **Background launches need a kill timer**: If launching in background,
   immediately set up a `sleep N && kill` safeguard.
3. **AppleScript/UI automation**: Wrap ALL osascript and UI interaction
   sequences in a timeout. Use `timeout 10 osascript -e '...'` for clicks.
4. **Screenshot loops**: If taking screenshots in a loop waiting for state,
   limit iterations (max 10) AND add `sleep` between them.
5. **Never use `open -W`**: It waits forever. Use `open` (no -W) + sleep + screenshot.
6. **Test one thing, then kill**: Don't leave the app running between tests.
   Kill it, relaunch fresh for the next test.

### Pattern for GUI testing:
```bash
# Launch app in background with hard kill after 90 seconds
timeout 90 open /path/to/app &
APP_PID=$!
sleep 15  # wait for startup

# Take screenshot with timeout
timeout 5 screencapture -x /tmp/pharo-screenshot.png

# Do UI interaction with timeout
timeout 10 osascript -e 'tell application "System Events" to ...'

# Kill app when done
kill $APP_PID 2>/dev/null
killall iospharo 2>/dev/null
```

### Why this matters:
Previous sessions hung for 28+ minutes clicking menus in a loop with no
timeout. The app locks up, osascript blocks forever, and the entire Claude
Code session becomes unresponsive. **A 60-second timeout that kills a working
test is infinitely better than no timeout that hangs forever.**

---
## ⛔️ STOP: NO WORKAROUNDS - FIX ROOT CAUSES ⛔️
---

**DO NOT add workarounds, hacks, or band-aids to bypass problems.**

When something doesn't work:
1. **STOP** and understand WHY it doesn't work
2. **Find the root cause** - trace the problem to its source
3. **Fix the actual bug** - not the symptom

### Specific patterns that are ALWAYS wrong:
- **Silently swallowing errors**: pushing nil and returning instead of stopVM() or letting Smalltalk handle it
- **Silently terminating processes**: `suspendActiveProcess_` or removing processes from scheduler queues to hide bugs
- **Skipping method lookup**: hardcoded class/selector checks to avoid calling methods that "cause problems"
- **Treating non-booleans as false**: conditional jumps must send `mustBeBoolean` per the spec
- **Loop/depth detectors that silently recover**: if DNU recurses infinitely, stopVM() — don't silently push nil
- **C++ code doing Smalltalk's job**: direct HandMorph/InputEventSensor manipulation, C++ event dispatch, etc.

### Why this matters:
- Workarounds **hide bugs** that will bite us later
- Workarounds **add complexity** that makes debugging harder
- Workarounds **diverge from standard Pharo** behavior
- The image should work like it does on any other VM

### Before adding ANY workaround, ask:
1. What is the ACTUAL problem?
2. Where in the code path does it fail?
3. What would the REAL fix be?
4. Is the workaround just avoiding understanding the problem?

**If you find yourself writing code that "works around" something, STOP and investigate the root cause instead.**

### ⛔️ VERIFY VISUALLY BEFORE CLAIMING ANYTHING WORKS ⛔️

**NEVER claim display, menus, or interaction "works" without taking a screenshot
and examining it.** Run the Mac Catalyst app, take a screenshot, and READ the
screenshot with the Read tool to confirm what's actually on screen.

### CRITICAL: Metal Window Capture

`screencapture -x` CANNOT capture Metal layer content from Mac Catalyst apps.
The window appears invisible/transparent in full-screen captures. You MUST use
window-specific capture with the `-l` flag:

```bash
# Get window IDs for our process
PID=$(pgrep -f "iospharo" | head -1)
swift -e "
import CoreGraphics
let windowList = CGWindowListCopyWindowInfo(.optionAll, kCGNullWindowID) as? [[String: Any]] ?? []
for w in windowList {
    guard let ownerPID = w[kCGWindowOwnerPID as String] as? Int, ownerPID == $PID else { continue }
    let windowID = w[kCGWindowNumber as String] as? Int ?? -1
    let name = w[kCGWindowName as String] as? String ?? \"\"
    print(\"id=\(windowID) name='\(name)'\")
}
"

# Capture specific window by ID
screencapture -x -l <WINDOW_ID> /tmp/pharo-screenshot.png
```

Checklist before claiming a GUI fix works:
1. Build and launch the Mac Catalyst app
2. Get window ID using the Swift snippet above
3. Take a screenshot: `screencapture -x -l <WINDOW_ID> /tmp/pharo-screenshot.png`
4. Read the screenshot with the Read tool to see what's actually rendered
5. Verify EACH specific claim (menu visible? clickable? world menu opens?)

**Do NOT rely on log output, test pass rates, or code analysis alone.**
Two weeks of "verified working" claims were wrong because nobody looked at the screen.

### GUI — Verified Working (2026-02-24)

Display, menus, and interaction all visually verified. SDL2 stubs in FFI.cpp
bridge between the Pharo image's OSSDL2Driver and the Metal rendering pipeline.

---

## Git Workflow
- **Update `docs/changes.md`** before committing user-visible changes (bug fixes, features, behavior changes). Add entries under the current build number at the top of the file.
- **Commit frequently**: Commit at least every 15 minutes to avoid losing work from mishandled stash/checkout operations. Do this silently without stopping to ask or show the user.
- Always run `git status` before and after commits to verify state

## Project Context
This is an iOS Pharo VM that moves oop encoding from high address bits to low bits for iOS/ASLR compatibility.

Key directories:
- `src/vm/` - Clean C++ VM implementation (ImageLoader, Interpreter, ObjectMemory, Primitives)
- `src/ios/` - Reference files (cointerp-cpp.c, primitives.json, generated_primitives.inc, oop.hpp)
- `scripts/` - Build and transformation scripts
- `docs/` - Documentation

## iOS Screen Layout
For any work involving iOS device screen geometry (safe areas, corner radii,
squircle math, Dynamic Island positioning, strip button layout, mask overlays),
use the `device-geometry` skill (`.claude/skills/device-geometry.md`).  It
contains all device data, the exact superellipse formula, and worked examples.
Source repo: `../claude-skills`

## Bytecode Reference
The Sista V1 bytecode spec (used by Pharo 10+) is notoriously hard to find online.
A local copy is at: `docs/SistaV1-Bytecode-Spec.md`

The authoritative source is the class comment in the Pharo source:
`src/Kernel-BytecodeEncoders/EncoderForSistaV1.class.st` (in the main pharo repo)

**Warning:** Online resources often document the older V3PlusClosures bytecode set, not Sista V1.
The bytecode ranges 0xE0-0xFF are completely different between the two sets.

## Image Compatibility
- The VM must work with standard Pharo images that other VM clients use for release
- Do NOT create iOS-specific images that require special preparation
- Any testing with modified images is fine, but the goal is normal image compatibility
- The display driver (OSSDL2Driver) should work without requiring image-side changes
- **Use fresh images only**: Always test with freshly downloaded Pharo images, not previously-saved ones
- **Image saving works**: Primitive 97 (snapshot) saves the heap in standard Spur format via ImageWriter. Converts iOS immediate tags back to standard Spur encoding. Both `Smalltalk snapshot:andQuit:` (standard SessionManager path with session stop/start) and raw `<primitive: 97>` work. Save-reload roundtrip verified.

## Image Issues and Upstream Wishlist
- `docs/image_issues.md` tracks Pharo 13 image bugs we patch via startup.st
  and feature requests (portrait layout, touch support) for upstream
- The startup.st patches are written by `PharoBridge.writeStartupScript()`
  and auto-loaded by Pharo's `StartupPreferencesLoader` on every image start

## SDL2 and FFI
- **SDL2 and FFI are working** — SDL2 stubs are registered, TFFI primitives implemented,
  type auto-fill bootstraps correctly, and all FFI tests pass (23/23)
- The standard Pharo image uses OSSDL2Driver which calls SDL2 via FFI
- FFI test failures in batch runs are test-runner forking artifacts, not FFI bugs

## Debugging
- **Debug before asking**: Always run the app and check logs yourself before asking the user to test.
- Always test on Mac first - it starts up much faster than the iOS simulator
- Use `./build/test_load_image <image-path>` for quick VM testing
- Build with `cmake --build build` from the project root
- Full build cycle: `xcodebuild -project iospharo.xcodeproj -scheme iospharo -configuration Debug -destination 'platform=macOS,variant=Mac Catalyst' build`
  (Xcode's "Check XCFramework Freshness" build phase auto-runs `build-xcframework.sh` if VM sources changed — no need to run it manually)

## Auto-Launch and CLI `--image` Flag
- The app supports a `--image` CLI flag to launch directly with a specific image file:
  ```bash
  timeout 60 open /path/to/iospharo.app --args --image /tmp/Pharo.image
  ```
  This bypasses the image library and splash screen — useful for automated testing.
- Users can also right-click an image in the library and choose "Set as Auto-Launch"
  to get a 3-second countdown splash on next app launch (with a "Show Library" cancel button).

---
## Running Official Pharo Test Suite

### Standard Pharo VM (reference)
```bash
# Download fresh image
cd /tmp && curl -sL https://get.pharo.org/64/130 | bash

# Run Kernel-Tests package (SmallIntegerTest, IntegerTest, FloatTest, etc.)
pharo /tmp/Pharo.image test "Kernel-Tests"
```

### Custom VM Testing
Our VM doesn't support command-line args to the image yet. Inject a test runner:

```bash
# 1. Fresh image
cd /tmp && curl -sL https://get.pharo.org/64/130 | bash

# 2. Inject test runner (uses chunk format for fileIn)
pharo /tmp/Pharo.image eval --save \
  "'scripts/pharo-headless-test/run_sunit_tests.st' asFileReference fileIn"

# 3. Run with custom VM
./build/test_load_image /tmp/Pharo.image

# 4. Results
cat /tmp/sunit_test_results.txt
```

### Running GUI/Spec Tests (Fake Head)
Spec presenter tests (Sp*AdapterTest, Sp*PresenterTest, St*PresenterTest)
need a running Morphic world loop. Inject `setup_fake_gui.st` BEFORE the
test runner to enable headless GUI testing:

```bash
# Inject fake GUI + test runner
pharo /tmp/Pharo.image eval --save \
  "'scripts/pharo-headless-test/setup_fake_gui.st' asFileReference fileIn. \
   'scripts/pharo-headless-test/run_sunit_tests.st' asFileReference fileIn"
```

What `setup_fake_gui.st` does:
- Installs `MorphicUIManager` (starts the UI process + MorphicRenderLoop)
- Ensures `Display` Form and `WorldMorph` exist
- Patches `Morph>>activate`/`passivate` to skip nil submorphs
- Provides `FakeGUI` helper class for programmatic interaction:
  - `FakeGUI clickMenuItemNamed: 'Save'` — find and click by label
  - `FakeGUI clickButtonNamed: 'OK' in: aPresenter` — click button in a window
  - `FakeGUI allMorphsNamed: 'Tools'` — find all morphs with label
  - `FakeGUI findWidgetOfType: SpButtonMorph in: aPresenter`
  - `FakeGUI openPresenter: aPresenter` — open + step world
  - `FakeGUI ensureWorldStepping` — force world cycles
  - `FakeGUI screenshotToFile: '/tmp/screenshot.png'` — save display as PNG
  - `FakeGUI screenshotOf: aMorph toFile: '/tmp/m.png'` — capture one morph

Without this, ~350 Spec tests fail with "receiver of activate is nil".
With it, 94.6% pass rate (1054/1113) on 64 GUI test classes.

### Files
- `scripts/pharo-headless-test/` - Submodule: https://github.com/avwohl/pharo-headless-test
- `/tmp/sunit_test_results.txt` - Output file
- `docs/test-results.md` - Test results and compatibility analysis

## Primitive Table Reference
The **one true source** for the primitive table is in VMMaker:
`~/src/pharo-vm/smalltalksrc/VMMaker/StackInterpreter.class.st`
in `initializePrimitiveTable` (lines ~1000-1400)

This Smalltalk source has structured data: `(number primitiveName)` with category comments.
VMMaker generates `cointerp.c` from this, so `src/ios/cointerp-cpp.c:2094-2756` is a usable reference.

**CRITICAL**: The clean C++ VM (`src/vm/Interpreter.cpp`) primitive table MUST match. When adding or fixing primitives:
1. Check VMMaker's StackInterpreter.class.st or cointerp-cpp.c for correct mappings
2. Many slots are null/unused (primitiveFail) - don't invent primitives that don't exist
3. Primitives 256-519 are external primitive indices (plugins), not VM primitives

**Generation**: Run VMMaker to regenerate `src/ios/primitives.json` and `generated_primitives.inc`:
```smalltalk
'scripts/PrimitiveTableExporter.st' asFileReference fileIn.
PrimitiveTableExporter exportTo: 'src/ios/primitives.json'.
PrimitiveTableExporter exportCppTo: 'src/ios/generated_primitives.inc'.
```
The `generated_primitives.inc` can be `#include`d directly in Interpreter.cpp to replace hand-written table entries.

## Agent Usage Guidelines
To avoid context pollution from large files (Interpreter.cpp: 8K lines, Primitives.cpp: 14K lines):

### Delegate to Agents
- **Primitive table audits**: "Compare primitiveTable_ entries N-M against cointerp-cpp.c and list discrepancies"
- **Cross-file verification**: "Find all places primitive X is referenced and check consistency"
- **Large grep/search tasks**: When searching across 20K+ lines of code
- **Reference extraction**: "Parse cointerp-cpp.c primitive table into a structured list"

### Keep in Main Context
- Small, focused edits to specific functions
- Reading individual primitive implementations
- Debugging specific runtime failures

### Why This Matters
With 577 manual primitive table entries, even 1% error rate = 5-6 wrong primitives. The repeated "fix 40+ incorrect mappings" commits show this is a real problem. Agents can do systematic verification without context limits causing drift.

