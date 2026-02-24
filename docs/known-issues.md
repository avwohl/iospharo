# Known Issues

Last updated: 2026-02-24

## Verified Working

### SpStyleEnvironmentColorProxy DNU (Startup Timing) — Fixed
OSSDL2Driver's event loop (priority 60) preempts SessionManager (priority 40).
Our SDL2 stubs are instantaneous while real SDL2 has natural latency from OS
window creation. Without delay, EXPOSED triggers rendering before UITheme
initializes → SpStyleEnvironmentColorProxy DNU → Emergency Debugger.

**Fix**: Poll-count countdown (300 polls × 5ms yield ≈ 1.5s) in
`stub_SDL_PollEvent` before delivering window events. This simulates the
real SDL2 window creation latency, giving SessionManager CPU time to
finish initialization. No wall-clock timers — purely event-loop driven.

### B2DPlugin Rendering
Both long-standing TODOs in B2DPlugin.c resolved (unresolved upstream for 25+ years):
- `findNextExternalFillFromAET`: Added `prevRightX` tracking to clamp leftX
  when edges cross between scan lines, preventing overlapping span fills.
- `stepToFirstBezierInat`: Kept `maxSteps = 2*deltaY` — the 2x oversampling
  provides quality that AA alone cannot compensate for, and is negligible
  on modern hardware.

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

### Event Loop and GUI — Visually Verified (2026-02-24)
OSSDL2Driver event loop via FFI stubs handles mouse clicks correctly.
The two-path routing (SDL2 vs primitive 264) resolves correctly: standard
Pharo images use OSSDL2Driver which activates the SDL2 path.

Verified via `screencapture -x -l` (Metal-aware window capture):
- Pharo 13 desktop renders correctly (dark theme, no red X, Welcome window)
- Menu bar visible and clickable (world menu → Browse → System Browser works)
- World menu opens on left-click desktop, right-click shows context menus
- System Browser opens and displays package list correctly

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
