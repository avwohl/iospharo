# Claude Code Instructions

---
## ⛔️ STOP: NO WORKAROUNDS - FIX ROOT CAUSES ⛔️
---

**DO NOT add workarounds, hacks, or band-aids to bypass problems.**

When something doesn't work:
1. **STOP** and understand WHY it doesn't work
2. **Find the root cause** - trace the problem to its source
3. **Fix the actual bug** - not the symptom

### Examples of BAD workarounds we've added:
- C++ `showWorldMenu()` call because InputEventSensor isn't running → **FIX**: Figure out why InputEventSensor isn't starting
- SwiftUI contextMenu overlay because clicks don't reach UIKit → **FIX**: Understand Mac Catalyst event routing properly
- Direct slot manipulation because primitives fail → **FIX**: Implement the primitives correctly

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

### Current Priority: Fix Morphic Processes
The image's Morphic event loop and InputEventSensor aren't running. This is THE problem to fix:
- Primitive 264 (getNextEvent) is registered but never called
- Only primitive 230 (relinquishProcessor) runs in a loop
- The scheduler works but only the idle process is active
- Event sensor process never starts

**FIX THIS. Don't wander off to other issues. Don't add workarounds.**

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

3. **Event injection to HandMorph works** (but is a WORKAROUND)
   - Events appear in /tmp/iospharo-events.log
   - Input semaphore is signaled
   - HandMorph's lastMouseEvent is updated

### Known Problems (need root cause fix)
1. **OrderedCollection >> #new causes DNU** (~99 times at startup)
   - This is FIRST thing that fails
   - If basic class instantiation doesn't work, nothing will work
   - TODO: Check metaclass lookup, check if Behavior>>new is found

2. **UndefinedObject >> #platformName** (~194 times)
   - Something returns nil when it should return a platform object
   - primitiveGetAttribute(1001) returns "iOS" or "Mac OS" - so that's working
   - TODO: Find what code calls platformName and why receiver is nil

3. **UndefinedObject >> #privSender:** (hundreds)
   - Context chains are broken - contexts have nil senders
   - This breaks process scheduling

4. **Cascade of exception handling failures**
   - #signal, #message:, #receiver:, #reachedDefaultHandler all on nil
   - Exception objects are nil because earlier failures corrupted state

### Workarounds We've Added (need to be removed)
1. `installOSiOSDriver()` - creates raw driver instance, bypasses normal initialization
2. `updateMouseEvent()` / `processInputEvents()` - direct HandMorph manipulation
3. Direct semaphore signaling - bypasses normal event flow
4. DNU handlers for privSender:, copyTo:, resume:through: on nil - hide root cause

### NOT the problem
- SDL2 availability (verified working)
- primitiveGetAttribute (returns correct values)
- Class table (classOf returns valid classes)
