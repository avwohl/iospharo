# Claude Code Instructions

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

### FFI/Display: VERIFIED WORKING (2026-02-08)

Real SDL2 is statically linked via `-Wl,-force_load` in CMakeLists.txt. The Pharo
image's OSSDL2Driver calls real SDL_Init, SDL_CreateWindow, SDL_PollEvent etc. via
FFI (TFFI callout → dlsym(RTLD_DEFAULT) → finds force-loaded SDL2 symbols).

Frame capture proves correct rendering: Pharo logo, menu bar, desktop visible.
Event loop runs (939 SDL_PollEvent calls in 45s). The stub functions in FFI.cpp
are dead code — never called by TFFI when real SDL2 is available.

**Remaining display issue**: `GrafPort(Object)>>error:` renders a red X over the
desktop. Caused by SpStyleEnvironmentColorProxy missing `#isTransparent` and
`#fillRectangle:on:`. This is a Morphic/Spec2 theming issue, not FFI.

### Current Priority: Fix GrafPort rendering error, enable skipped tests

- Investigate GrafPort(Object)>>error: during desktop rendering
- Enable the 11 skipped test classes (134 hidden tests) and fix failures
- Fix the 10 individually skipped test selectors
- Only 32-bit-specific skips are acceptable

---

## Git Workflow
- **Commit frequently**: Commit at least every 15 minutes to avoid losing work from mishandled stash/checkout operations. Do this silently without stopping to ask or show the user.
- Always run `git status` before and after commits to verify state

## Project Context
This is an iOS Pharo VM that moves oop encoding from high address bits to low bits for iOS/ASLR compatibility.

Key directories:
- `src/vm/` - Clean C++ VM implementation (ImageLoader, Interpreter, ObjectMemory, Primitives)
- `src/ios/` - iOS-specific VM code and generated interpreter
- `scripts/` - Build and transformation scripts
- `docs/` - Documentation and WIP notes

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
- The display driver (OSiOSDriver) should work without requiring image-side changes
- **Use fresh images only**: Always test with freshly downloaded Pharo images, not previously-saved ones
- **Save is disabled**: Image saving (snapshot) is disabled for now to ensure consistent testing from fresh state

## SDL2 and FFI
- **SDL2 is set up and should be working** - SDL2 stubs are registered with the VM
- The standard Pharo image uses OSSDL2Driver which calls SDL2 via FFI
- If SDL2/FFI isn't working, **fix the FFI loading** - don't assume SDL2 isn't available
- Check `primitiveLoadSymbolFromModule` and FFI callout paths if SDL2 functions aren't found

## Debugging
- **Debug before asking**: Always run the app and check logs yourself before asking the user to test. Use `/tmp/iospharo-render.log` and other log files to diagnose issues.
- Always test on Mac first - it starts up much faster than the iOS simulator
- Use `./build/test_load_image <image-path>` for quick VM testing
- Build with `cmake --build build` from the project root
- Full build cycle: `cmake --build build-app && ./build-xcframework.sh && xcodebuild -project iospharo.xcodeproj -scheme iospharo -configuration Debug -destination 'platform=macOS,variant=Mac Catalyst' build`

---
## Running Official Pharo Test Suite

### Standard Pharo VM (reference)
```bash
# Download fresh image
cd /tmp && curl -sL https://get.pharo.org/64/130 | bash

# Run Kernel-Tests package (SmallIntegerTest, IntegerTest, FloatTest, etc.)
/Users/wohl/Downloads/pharo /tmp/Pharo.image test "Kernel-Tests"
```

### Custom VM Testing
Our VM doesn't support command-line args to the image yet. Inject a test runner:

```bash
# 1. Fresh image
cd /tmp && curl -sL https://get.pharo.org/64/130 | bash

# 2. Inject test runner (uses chunk format for fileIn)
/Users/wohl/Downloads/pharo /tmp/Pharo.image eval --save \
  "'/Users/wohl/src/iospharo/scripts/run_sunit_tests.st' asFileReference fileIn"

# 3. Run with custom VM
./build/test_load_image /tmp/Pharo.image

# 4. Results
cat /tmp/sunit_test_results.txt
```

### Files
- `scripts/run_sunit_tests.st` - Test runner (chunk format .st file)
- `/tmp/sunit_test_results.txt` - Output file
- `docs/WIP.md` - Last known test results and status

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

---
## Investigation Log

### Verified Working (2025-01-25)
1. **SDL2 symbols are exported and findable via dlsym**
   - Added `__attribute__((used, visibility("default")))` to FFI.cpp
   - Verified in osdriver_install.log: `SDL_Init=0x1026bcd2c SDL_PollEvent=0x1026bd164`
   - The symbols ARE available at runtime

2. **primitiveLoadSymbolFromModule is registered**
   - Registered in namedPrimitives_ under both "" and "SqueakFFIPrims" modules
   - Would work IF it were ever called

### NOT the problem
- SDL2 availability (verified working)
- primitiveGetAttribute (returns correct values)
- Class table (classOf returns valid classes)

### Investigation: Event Loop Not Starting (2025-01-28)

**Root Cause Identified:**
1. **OSiOSDriver in the image is just a STUB** - missing setupEventLoop, eventLoop, and all event handling methods
2. **OSSDL2Driver's setupEventLoop fails** - uses FFI which has broken type resolution
3. **No process polls primitive 264** - events collect in passThroughEvents_ but nobody reads them

**Key Finding: Priority Values in Pharo 13:**
- `lowIOPriority` = 60 (NOT 33 as in older Pharo versions)
- Verified from ProcessorScheduler source: `LowIOPriority := 60`
- Priority 60 for event loop process IS CORRECT

**FFI Type Resolution Broken:**
- `ByteSymbol >> newReferentClass:` not found - should be on ExternalType
- `ByteSymbol >> asPointerType` not found - same issue
- FFI is returning Symbols instead of ExternalType objects
- This blocks OSSDL2Driver which uses FFI for SDL_PollEvent

**Event Collection Works:**
- VM's processInputEvents() collects events correctly
- Events go into passThroughEvents_
- Input semaphore is signaled
- Primitive 264 is implemented and works
- BUT: Nothing calls primitive 264!

**TODO:** Fix FFI type resolution so OSSDL2Driver works natively. Do NOT inject
Smalltalk code to bypass FFI — that's a workaround.
