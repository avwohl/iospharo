# Known Issues

Last updated: 2026-02-24

## High Priority (GUI)

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

### B2DPlugin Stroke Edge Cases
Two incomplete TODOs in `src/vm/plugins/B2DPlugin.c`:
- Line 5110: stroke edge case handling for `leftX` from last operation
- Line 12873: arc rendering optimization (reducing maxSteps)

Rendering still works overall; these are edge case optimizations.

## Verified Working

### Touch-to-Mouse Translation
Full gesture handling implemented in PharoCanvasView.swift:
- Single tap → left click; long press → right click (with haptic)
- Two-finger tap → right click; Ctrl+tap → right click (hardware keyboard)
- Two-finger pan → scroll events; pinch → zoom (Cmd+scroll)
- Drag → mouse move with button held
- Floating middle-click button on iOS for blue button
- Hardware keyboard: modifier keys (Shift/Ctrl/Alt/Cmd) sent with events,
  special keys (arrows, escape, tab, backspace, etc.) mapped properly

### iOS Keyboard and Clipboard
SDL2 clipboard stubs delegate through PlatformBridge to Swift UIPasteboard.
SDL2 text input stubs trigger becomeFirstResponder/resignFirstResponder
on the MTKView. UIKeyInput conformance on iOS shows the soft keyboard
and sends keystroke events (down/stroke/up) for each character.

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
