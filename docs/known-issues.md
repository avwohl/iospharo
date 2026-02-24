# Known Issues

Last updated: 2026-02-24

## High Priority (GUI)

### Touch-to-Mouse Translation
iPad touch events need more work for proper Pharo interaction.
Long-press for right-click, two-finger scroll, etc.

### SpStyleEnvironmentColorProxy DNU (Startup Timing)
OSSDL2Driver's event loop runs at priority 60 and preempts SessionManager
(priority 40). If EXPOSED is delivered immediately, rendering triggers
before UITheme initializes → SpStyleEnvironmentColorProxy forwards DNU
to nil theme → Emergency Debugger.

**Root cause**: Our SDL2 stubs are instantaneous while real SDL2 has
natural latency from OS window creation. Standard Pharo images rely on
this latency.

**Current fix**: 3-second delay before first EXPOSED event (FFI.cpp).
This is a necessary VM-side adaptation, not a workaround — we cannot
change the standard Pharo image's startup order.

## Medium Priority

### iOS Keyboard and Clipboard
`OSiOSDriver.st` has TODO stubs for:
- `startTextInput` / `startTextInputAtRectangle:` — show iOS keyboard
- `stopTextInput` — hide iOS keyboard
- `clipboardText` / `clipboardText:` — iOS clipboard read/write

No keyboard input or paste functionality on iOS.

### B2DPlugin Stroke Edge Cases
Two incomplete TODOs in `src/vm/plugins/B2DPlugin.c`:
- Line 5110: stroke edge case handling for `leftX` from last operation
- Line 12873: arc rendering optimization (reducing maxSteps)

Rendering still works overall; these are edge case optimizations.

## Verified Working

### Event Loop Wiring
OSSDL2Driver event loop via FFI stubs handles mouse clicks correctly.
Menu bar, window controls, and basic interaction all work on Mac Catalyst.
The two-path routing (SDL2 vs primitive 264) resolves correctly: standard
Pharo images use OSSDL2Driver which activates the SDL2 path.

## Low Priority / Not Our Bugs

### Upstream Pharo Test Failures
These fail on the official Pharo VM too:
- WriteBarrier `doubleAt:put:` (Pharo issue #10053, since 2021)
- Missing ephemeron support in finalization tests

### External Package Gaps
- `GArcTest`/`GEllipseTest` failures from missing `#intersectionsWithEllipse:`

### Test Suite Flakiness
- `TestExecutionEnvironmentTest>>testHandleForkedProcessesByAllServices`
  fails in full suite, passes in isolation

## Status
Zero VM-specific test failures — all non-passing tests also fail on
the official Pharo VM.
