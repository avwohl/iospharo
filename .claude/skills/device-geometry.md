---
name: device-geometry
description: iPhone/iPad screen geometry and the exact algorithm for computing screen-edge masks and UI element clearance. Use when adjusting strip button positioning, building mask overlays, or any layout work near rounded corners or the Dynamic Island.
---

# Device Screen Geometry & Mask Computation

This skill is the **single source of truth** for iOS device screen geometry
and the **exact algorithm** for computing screen-edge
visibility.  Every Claude session MUST use these formulas — no heuristics,
no "about 45%", no guessing.

---

## Part 1: Device Lookup Table

Landscape orientation (the primary concern for strip buttons).
All Face ID iPhones use @3x.  Multiply pt by 3 for pixels.

    Device group              Screen (landscape pt)  R (pt)  SA leading  SA bottom
    ------------------------  ---------------------  ------  ----------  ---------
    X / XS / 11 Pro           812 x 375              39.0   44          21
    XS Max / 11 Pro Max       896 x 414              39.0   44          21
    XR / 11 (@2x)             896 x 414              41.5   48          21
    12 mini / 13 mini         812 x 375              44.0   50          21
    12/12P / 13/13P / 14/16e  844 x 390              47.33  47          21
    12 PM / 13 PM / 14 Plus   926 x 428              53.33  47          21
    14P / 15 / 15P / 16       852 x 393              55.0   59          21
    14PM / 15+ / 15PM / 16+   932 x 430              55.0   59          21
    16 Pro                    874 x 402              62.0   62          21
    16 Pro Max                956 x 440              62.0   62          21

    R = display corner radius (squircle)
    SA leading = safe area inset on the notch/DI side (left or right)
    SA bottom = home indicator inset

    iPads: R=18pt, no notch/DI, SA top=24, SA bottom=20 (both orientations)
    iPhone SE 2/3: R=0, no notch, SA top=20, SA bottom=0

---

## Part 2: Squircle Corner Intrusion — THE Formula

Apple's continuous corners are a superellipse with exponent n.
The exact exponent is not published.  Use **n = 5** (matches
`CALayerCornerCurve.continuous` within 0.5pt for all radii 18-62pt).

### Problem statement

Given a UI element whose nearest edge is at (x, y) from a screen corner,
determine whether the corner's squircle curve clips it.

### Coordinate system

Origin at the corner of the screen.  x increases toward screen center
horizontally, y increases toward screen center vertically.

    (0,0)-----> x (toward center)
      |
      |
      v y (toward center)

### The curve

For a squircle corner of radius R with exponent n=5:

    (R - x)^5 + (R - y)^5 = R^5

Points where the left side <= R^5 are ON SCREEN (visible).
Points where the left side >  R^5 are OFF SCREEN (clipped by bezel).

### Minimum y clearance at a given x

Given x (distance from left edge of screen), the minimum y to be
on screen:

    if x >= R:
        y_min = 0          (no corner intrusion this far from edge)
    else:
        y_min = R - (R^5 - (R-x)^5) ^ (1/5)

This is **exact**.  Do not approximate, round early, or substitute
a simpler formula.  Compute in floating point, then ceil() once at
the end when converting to padding.

### Worked examples (MANDATORY cross-check)

Any implementation MUST reproduce these values.  If your code gives
different numbers, it has a bug.

    R=55 (iPhone 16), n=5:
      x= 4pt:  y_min = 11.4 pt   (button left edge in 40pt strip)
      x= 6pt:  y_min =  8.4 pt
      x=10pt:  y_min =  4.8 pt
      x=20pt:  y_min =  1.2 pt   (strip center)
      x=26pt:  y_min =  0.5 pt
      x=40pt:  y_min =  0.0 pt
      x=55pt:  y_min =  0.0 pt

    R=62 (iPhone 16 Pro), n=5:
      x= 4pt:  y_min = 13.8 pt
      x= 6pt:  y_min = 10.4 pt
      x=10pt:  y_min =  6.3 pt
      x=20pt:  y_min =  1.9 pt
      x=26pt:  y_min =  0.8 pt
      x=40pt:  y_min =  0.1 pt
      x=62pt:  y_min =  0.0 pt

    R=47.33 (iPhone 14/16e), n=5:
      x= 4pt:  y_min =  8.8 pt
      x= 6pt:  y_min =  6.3 pt
      x=10pt:  y_min =  3.3 pt
      x=20pt:  y_min =  0.6 pt

    R=39 (iPhone X/XS), n=5:
      x= 4pt:  y_min =  6.2 pt
      x= 6pt:  y_min =  4.2 pt
      x=10pt:  y_min =  2.0 pt
      x=20pt:  y_min =  0.2 pt

    R=18 (iPad), n=5:
      x= 4pt:  y_min =  1.2 pt
      x= 6pt:  y_min =  0.5 pt
      x=10pt:  y_min =  0.1 pt
      x=18pt:  y_min =  0.0 pt

---

## Part 3: Computing Strip Button Padding

The modifier strip is a vertical column of buttons on the leading edge
in landscape.  The strip's left edge is at x=0 (screen edge).

### Inputs (read from SwiftUI at runtime)

    stripWidth      = width of the strip view (currently 40pt on iPhone)
    buttonSize      = width/height of a button (currently 32pt on iPhone)
    buttonSpacing   = gap between buttons (currently 4pt on iPhone)
    R               = device corner radius (from lookup table or _displayCornerRadius)
    SA_leading      = safe area leading inset (from window.safeAreaInsets)
    SA_bottom       = safe area bottom inset (home indicator, from window.safeAreaInsets)

### Step 1: Find the x coordinate that matters

The button is centered in the strip.  Its left edge is at:

    button_left_x = (stripWidth - buttonSize) / 2

For current values: (40 - 32) / 2 = 4pt.

The tightest constraint is the button's leftmost edge (smallest x = most
corner intrusion).  Use x = button_left_x.

### Step 2: Compute top padding (clear top-left corner)

    if button_left_x >= R:
        top_padding = 0
    else:
        y_min = R - (R^5 - (R - button_left_x)^5) ^ (1/5)
        top_padding = ceil(y_min + 2)     // +2pt visual breathing room

    Result for current layout (button_left_x = 4pt):
      R=55:    y_min = 11.4pt  → top_padding = 14pt
      R=62:    y_min = 13.8pt  → top_padding = 16pt
      R=47.33: y_min =  8.8pt  → top_padding = 11pt
      R=39:    y_min =  6.2pt  → top_padding =  9pt

### Step 3: Compute bottom padding (clear bottom-left corner + home indicator)

    if button_left_x >= R:
        bottom_corner_clear = 0
    else:
        bottom_corner_clear = ceil(y_min + 2)   // same formula as top

    bottom_padding = bottom_corner_clear + SA_bottom

    Result for current layout:
      R=55,  SA_bottom=21:  14 + 21 = 35pt
      R=62,  SA_bottom=21:  16 + 21 = 37pt
      R=47.33, SA_bottom=21: 11 + 21 = 32pt

### Step 4: Verify DI does not overlap top buttons

In landscape, the Dynamic Island is centered vertically on the leading
edge.  Its vertical center is at screen_height/2.  Its vertical extent
is ~126pt / 2 = 63pt above and below center.

    di_top_y  = screen_height/2 - 63
    di_bot_y  = screen_height/2 + 63

The top button's bottom edge is at:

    top_button_bottom = top_padding + buttonSize

If top_button_bottom < di_top_y, the top button clears the DI.

    iPhone 16 landscape: screen_height=393, di_top_y=393/2-63=133.5
    Top button bottom: 13 + 32 = 45.  45 < 133.5.  Clears easily.

    Three buttons stacked: 13 + 32 + 4 + 32 + 4 + 32 = 117.
    117 < 133.5.  All three top buttons clear the DI.

The DI is never a concern for top/bottom buttons on any current device.
It only matters for elements near the vertical center of the screen.

---

## Part 4: Generating a Pixel Bitmask Image

For the mask overlay tool (`tools/apply_device_mask.py` in the claude-skills
repo) or any other visualization that needs a device-shaped mask.

The tool auto-detects the device from screenshot pixel dimensions and renders
squircle corners, Dynamic Island / notch cutout, and safe area lines:

    python3 tools/apply_device_mask.py screenshot.png              # basic mask
    python3 tools/apply_device_mask.py --safe-areas screenshot.png # + safe area lines
    python3 tools/apply_device_mask.py --device iphone16pro screenshot.png

Requires: `pip3 install Pillow numpy`

### Inputs

    device_key     identifies the device (from lookup table)
    orientation    portrait or landscape
    scale          @2x or @3x (auto-detect from image dimensions)

### Step 1: Create a white image (all visible)

    width_px  = screen_width_pt * scale
    height_px = screen_height_pt * scale
    mask = new white image(width_px, height_px)

### Step 2: Black out four corners

For each corner, iterate over the R*scale x R*scale pixel region.
For each pixel at (px, py) from that corner's origin:

    x_pt = px / scale      (convert pixel to points)
    y_pt = py / scale

    inside = (R - x_pt)^5 + (R - y_pt)^5 <= R^5

    if NOT inside:
        mask[pixel] = black

Apply to all four corners (mirror x and/or y as needed):
  - Top-left:     px from 0,          py from 0
  - Top-right:    px from width-R*s,  py from 0           (mirror x)
  - Bottom-left:  px from 0,          py from height-R*s  (mirror y)
  - Bottom-right: px from width-R*s,  py from height-R*s  (mirror x+y)

### Step 3: Black out notch or Dynamic Island

**Dynamic Island (landscape, DI on left/leading edge):**

    di_width_pt   = 37        (portrait height becomes landscape width)
    di_height_pt  = 126       (portrait width becomes landscape height)
    di_corner_r   = 18.5      (the pill's own rounded corners)

    di_x          = 0         (flush with leading edge)
    di_y_center   = screen_height_pt / 2
    di_y_top      = di_y_center - di_height_pt / 2
    di_y_bottom   = di_y_center + di_height_pt / 2

    For DI on right edge: di_x = screen_width_pt - di_width_pt

    Draw a black rounded rectangle:
      origin:  (di_x * scale, di_y_top * scale)
      size:    (di_width_pt * scale, di_height_pt * scale)
      radius:  di_corner_r * scale
      Use circular arcs for the DI pill corners (not squircle).

    Note: The DI has ~11pt of screen above it in portrait, which
    means in landscape there's ~11pt of screen to the left of it.
    The 37pt dimension is the DI depth INTO the screen from the edge.
    Position it at x = 11pt from the leading edge (not at x=0).

    Corrected:
      di_x = 11                              (gap from edge)
      DI pill left edge:   di_x
      DI pill right edge:  di_x + di_width_pt

**Dynamic Island (portrait, DI at top):**

    di_width_pt   = 126
    di_height_pt  = 37
    di_corner_r   = 18.5

    di_x_center   = screen_width_pt / 2
    di_x_left     = di_x_center - di_width_pt / 2
    di_y           = 11        (gap from top edge)

    Draw a black rounded rectangle:
      origin:  (di_x_left * scale, di_y * scale)
      size:    (di_width_pt * scale, di_height_pt * scale)
      radius:  di_corner_r * scale

**Notch (landscape, notch on left/leading edge):**

    Notch is a trapezoid, but for mask purposes approximate as a
    rectangle with small corner radii:

    For wide notch (X/XS/12-era):
      notch_depth_pt  = 30       (33 for XR/11 LCD)
      notch_span_pt   = 209

    For narrow notch (13/14/16e-era):
      notch_depth_pt  = 32
      notch_span_pt   = 166

    notch_y_center = screen_height_pt / 2
    notch_y_top    = notch_y_center - notch_span_pt / 2
    notch_x        = 0           (flush with leading edge)

    Draw black rectangle:
      origin:  (0, notch_y_top * scale)
      size:    (notch_depth_pt * scale, notch_span_pt * scale)

### Step 4: Optional safe area boundary lines

    Draw 1px red lines at:
      Portrait:  y = SA_top * scale (from top), y = height - SA_bottom * scale
      Landscape: x = SA_leading * scale (from left), x = width - SA_leading * scale
                 y = height - SA_bottom * scale

### Step 5: Composite onto screenshot

    Load screenshot
    Verify dimensions match expected device (auto-detect from px size)
    For each black pixel in mask: set screenshot pixel to black
    (Or use alpha compositing with a semi-transparent mask)
    Save result

### Auto-detection from screenshot pixel dimensions

    Pixels (landscape)     Device group                    R     Scale
    ------------------     ------------                    --    -----
    2436 x 1125            X / XS / 11 Pro                 39    @3x
    2688 x 1242            XS Max / 11 Pro Max             39    @3x
    1792 x 828             XR / 11                         41.5  @2x
    2340 x 1080            12 mini / 13 mini               44    @3x
    2532 x 1170            12/13/14/16e (6.1" notch)       47.33 @3x
    2778 x 1284            12PM/13PM/14Plus (6.7" notch)   53.33 @3x
    2556 x 1179            14P/15/15P/16 (6.1" DI)         55    @3x
    2796 x 1290            14PM/15+/15PM/16+ (6.7" DI)     55    @3x
    2622 x 1206            16 Pro                          62    @3x
    2868 x 1320            16 Pro Max                      62    @3x
    1334 x 750             SE 2/3                           0    @2x

    Portrait: swap width/height.
    If neither orientation matches, error out.

---

## Part 5: What NOT To Do

These mistakes have been made before.  Do not repeat them.

  1. DO NOT use a percentage of the safe area inset as corner clearance.
     The safe area inset reflects the DI/notch exclusion zone, which is
     much larger than the corner radius intrusion at the strip's position.
     Use the squircle formula.

  2. DO NOT use circular arc math (sqrt).  Apple corners are squircles
     (superellipse n=5), not circles.  A circular arc overestimates
     intrusion in the middle range and underestimates near the edges.

  3. DO NOT hardcode padding values for specific devices.  Compute from
     R using the formula.  New devices get correct values automatically.

  4. DO NOT confuse the DI's safe area inset (59/62pt) with the corner
     radius (55/62pt).  They are independent values that happen to be
     close on some devices.  The safe area inset is for the DI; the
     corner radius is for the squircle.

  5. DO NOT assume the DI affects top/bottom buttons.  In landscape the
     DI is at the vertical center (~196pt from corner on a 393pt screen).
     Top buttons max out at ~120pt from the corner.  The DI is irrelevant
     for strip button positioning.

  6. DO NOT generate a mask at @1x and scale up.  Always generate at
     native resolution (@2x or @3x) for accurate sub-point rendering of
     the squircle curve.

  7. DO NOT use different squircle exponents for different devices.
     All Apple devices use the same continuous corner curve.  n=5 for all.

---

## Part 6: Swift Implementation Reference

For use in the actual strip positioning code:

```swift
/// Minimum distance from screen edge (y) to clear the squircle corner
/// at a given distance from the perpendicular edge (x).
/// Uses superellipse exponent n=5 matching CALayerCornerCurve.continuous.
func squircleIntrusion(cornerRadius R: CGFloat, atDistance x: CGFloat) -> CGFloat {
    guard x < R else { return 0 }
    let r5 = pow(R, 5)
    let d5 = pow(R - x, 5)
    return R - pow(r5 - d5, 0.2)
}

// Usage for strip button top padding:
let buttonLeftX = (stripWidth - buttonSize) / 2   // e.g. (40-32)/2 = 4pt
let intrusion = squircleIntrusion(cornerRadius: R, atDistance: buttonLeftX)
let topPadding = ceil(intrusion + 2)  // 2pt breathing room
```

This replaces any heuristic like `notchInset * 0.45`.  The function is
pure math with no device-specific constants.

---

## Sources

  docs/TODO-device-masks.md              (mask overlay tool spec)
  kylebashour.com/posts/finding-the-real-iphone-x-corner-radius
  Superellipse: en.wikipedia.org/wiki/Superellipse
