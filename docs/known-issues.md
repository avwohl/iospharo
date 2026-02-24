# Known Issues

Last updated: 2026-02-24

## High Priority (GUI)

### Event Loop Wiring
OSSDL2Driver event loop + FFI is working, but end-to-end event flow
(touch/click → Pharo morphs) needs verification. Menu clicks and world
menu may still be unreliable.

### Touch-to-Mouse Translation
iPad touch events need more work for proper Pharo interaction.

### SpStyleEnvironmentColorProxy DNU
`GrafPort(Object)>>error:` renders a red X over the desktop during early
startup. Caused by SpStyleEnvironmentColorProxy (ProtoObject subclass)
forwarding DNU to `Smalltalk ui theme` before UITheme is initialized.
Currently mitigated by delaying EXPOSED events 3 seconds after window
creation. Root cause fix needed.

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
