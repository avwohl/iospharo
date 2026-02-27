# What's New in Build 23

Build 23 — 2026-02-26

## Bug Fixes

### iPhone Forced to Landscape Mode
Portrait mode on iPhone (~390px wide) made Pharo essentially unusable:
  - The image library buttons wrapped into unreadable vertical text
  - Pharo's desktop windows and dialogs are designed for 1024px+ and
    didn't fit at all
  - The Save As dialog was completely broken — no visible text input field,
    keyboard covered half the window
  - Keyboard input reportedly didn't register

The app now locks iPhone to landscape orientation, giving ~844px of width on
modern iPhones. This is enough for Pharo's UI to work without any image-side
patching. iPad keeps all orientations (portrait and landscape). Mac is
unaffected.
