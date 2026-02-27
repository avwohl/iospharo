# What's New in Build 24

Build 24 — 2026-02-27

## Bug Fixes

### iPhone Safe Area Insets
The Pharo display now respects iOS safe area insets on iPhone and iPad.
Previously, content rendered edge-to-edge, causing the menu bar text and
taskbar to be clipped by the device's rounded corners and hidden behind
the Dynamic Island camera cutout. The Metal view is now constrained to
the safe area, with a black background filling the margins to blend with
the device bezel. Mac Catalyst is unaffected (no rounded corners or
notch on Mac windows).

## Changes

### Softened Pharo.org Disclaimer
Changed the image library disclaimer from "not affiliated with or
endorsed by Pharo.org" to "not endorsed by Pharo.org". The project is
engaged with the Pharo community — the disclaimer just clarifies that
bug reports should come to us, not Pharo.org.
