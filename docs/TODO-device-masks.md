# TODO: Device Screen Mask Overlays

## Problem

Simulator screenshots are pure rectangles — no rounded corners, no Dynamic
Island cutout, no camera hole.  Layout bugs (buttons clipped by corners,
content hidden behind the Dynamic Island) are invisible until tested on a
real device.  Claude Code can't see these issues when reviewing screenshots.

## Goal

Build a tool that composites a device mask over a simulator screenshot so
the actual visible area is clear.  The mask blacks out:
- Rounded screen corners (squircle, not circular)
- Dynamic Island / camera cutout
- Optionally: translucent overlays for safe area boundaries and home indicator

## Reference Data

All device dimensions, safe area insets, corner radii, and Dynamic Island
positions are documented in `docs/device-screen-geometry.md`.

Key values:
  iPhone 16:          393x852 pt, 55pt corners, DI yes, safe area 59pt
  iPhone 16 Pro:      402x874 pt, 62pt corners, DI yes, safe area 62pt
  iPhone 16 Pro Max:  440x956 pt, 62pt corners, DI yes, safe area 62pt
  iPad Pro 13":       1032x1376 pt, 18pt corners, no DI
  iPad Pro 11":       834x1210 pt, 18pt corners, no DI
  iPad Air 13":       1024x1366 pt, 18pt corners, no DI

## Deliverable

A script at `scripts/apply_device_mask.py` (or similar) that:

    scripts/apply_device_mask.py --device iphone16 screenshot.png masked.png

Priority devices (in order):
  1. iPhone 16 landscape (most layout issues happen here)
  2. iPhone 16 Pro landscape
  3. iPhone 16 portrait
  4. iPad Pro 13" landscape
  5. iPad Pro 11" landscape

## Implementation Steps

### 1. Squircle corner mask

Apple uses continuous corners (squircles), not circular arcs.  The
formula is a superellipse: |x|^n + |y|^n = r^n with n ≈ 4-5.

For practical purposes, iOS uses `UIBezierPath(roundedRect:cornerRadius:)`
with `CALayerCornerCurve.continuous`.  A good approximation:
- Use a superellipse with exponent n=5
- Or use the Figma/Sketch "smooth corners" formula
- Or just use circular arcs — close enough for mask purposes

Draw filled black in the four corners outside the squircle curve.

### 2. Dynamic Island cutout

Only for iPhones.  In portrait, the DI is a rounded rectangle centered
horizontally near the top of the screen.  In landscape, it rotates to
the near edge (left or right) centered vertically.

Dimensions (from pixel measurements, see device-screen-geometry.md):
  iPhone 16:       ~126 x 37 pt (portrait W x H)
  iPhone 16 Pro:   ~162 x 36 pt
  Pro Max:         ~256 x 36 pt

The DI cutout itself has rounded corners (~18-22pt radius).

Draw filled black over the DI area.

### 3. Safe area boundary lines (optional)

Draw thin colored lines at the safe area boundaries:
  - Red line at top safe area (59/62pt from top in portrait)
  - Red line at bottom safe area (34pt from bottom in portrait)
  - Blue lines at leading/trailing safe areas in landscape
  - Green line at home indicator zone

### 4. Scale handling

Simulator screenshots are at @2x or @3x pixel density.  The script needs
to know the scale factor to convert point values to pixels:
  iPhone: @3x (multiply all pt values by 3)
  iPad: @2x (multiply all pt values by 2)

Or auto-detect from screenshot dimensions:
  1179x2556 px → iPhone 16 @3x
  2064x2752 px → iPad Pro 13" @2x
  etc.

### 5. Orientation detection

Auto-detect portrait vs landscape from aspect ratio:
  width < height → portrait
  width > height → landscape

For landscape, detect which side the DI is on:
  - Default: DI on left (landscape-left orientation)
  - Flag: --di-right to flip

### 6. CLI interface

    usage: apply_device_mask.py [options] input.png [output.png]

    options:
      --device DEVICE     Device model (auto-detect from dimensions if omitted)
                          iphone16, iphone16pro, iphone16promax,
                          ipadpro13, ipadpro11, ipadair13
      --safe-areas        Draw safe area boundary lines
      --di-right          Put Dynamic Island on right side (landscape)
      --no-di             Skip Dynamic Island cutout
      -o, --output FILE   Output file (default: input_masked.png)

### 7. Integration with Claude Code

Add a note to CLAUDE.md under the screenshot verification section:

    After capturing a simulator screenshot, apply the device mask:
      python3 scripts/apply_device_mask.py --device iphone16 screenshot.png
    Then read the masked image to check for layout issues.

## Dependencies

- Python 3 with Pillow (PIL) — standard on macOS with `pip3 install Pillow`
- No other dependencies

## Testing

1. Take a simulator screenshot of the strip layout in landscape
2. Apply the iPhone 16 mask
3. Verify the rounded corners and DI cutout are visible
4. Confirm the top strip button is now visibly clipped in the masked image
   (validating that the mask correctly shows the problem)

## Stretch Goals

- Pre-rendered mask PNGs checked into the repo (avoids Python dependency)
- SwiftUI preview overlay for Xcode (see device frame in previews)
- Support for older devices (iPhone SE, iPad mini)
- Support for Stage Manager window sizes on iPad
