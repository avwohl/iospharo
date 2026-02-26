# What's New in Build 22

Build 22 — 2026-02-26

## Bug Fixes

### Two-Finger Scroll Now Works on iPad
Previously, scrolling with two fingers on the iPad screen did nothing — you had
to use the tiny Pharo scrollbar widgets. Two-finger scroll now sends proper
scroll wheel events to Pharo, so lists, code browsers, and text editors all
scroll naturally.

### Stage Manager Window Resize Fixed
Resizing the app window via iPad Stage Manager (or dragging the window edge on
Mac) could produce a garbled, scrambled display. Fixed a race condition where
the display buffer could be reallocated while the VM was still drawing into it.

### App Properly Exits When You Quit Pharo
On iPhone, quitting Pharo (via the World menu) would drop you back to the image
library in a broken state instead of closing the app. The app now exits cleanly
when Pharo quits, matching the Cmd+Q behavior on Mac.

## Known Issues

- Onboarding/help overlay needed — new users don't discover gestures
  (long-press for right-click, two-finger scroll, Ctrl button, etc.)
