# Device Screen Geometry Reference

Precise point values for screen dimensions, safe area insets, corner radii,
and notch/Dynamic Island positions for every Face ID iPhone, plus iPads and
iPhone SE.  Used for building mask overlays to evaluate UI layouts without
testing on physical hardware.

All values in **points** (UIKit/SwiftUI logical units), portrait unless noted.
Corner radii from `UIScreen._displayCornerRadius` (private API); all use
`CALayerCornerCurve.continuous` (squircle), not circular arcs.

Safe area insets are for a full-screen view controller with no navigation
bars -- just the bare system chrome (status bar + home indicator).

---

## Dynamic Island iPhones

All Dynamic Island models: status bar 54pt, home indicator 34pt portrait /
21pt landscape, bottom safe area 34pt portrait / 21pt landscape.

The DI hardware cutout is ~126pt wide x ~37pt tall, centered horizontally,
with its top edge ~11pt below the top of the screen.  The safe area top
inset (59 or 62pt) is the official exclusion zone.

### iPhone 14 Pro / iPhone 15 / iPhone 15 Pro / iPhone 16

    Same geometry (6.1" OLED @3x, Dynamic Island)

    Screen:           393 x 852 pt   @3x (1179 x 2556 px)  460 PPI
    Corner radius:    55.0 pt
    Status bar:       54 pt
    Home indicator:   34 pt portrait, 21 pt landscape

    Safe area (portrait):   top=59  bottom=34  left=0   right=0
    Safe area (landscape):  top=0   bottom=21  left=59  right=59

    Dynamic Island:   ~126 pt wide x ~37 pt tall, centered, ~11 pt from top

### iPhone 14 Pro Max / iPhone 15 Plus / iPhone 15 Pro Max / iPhone 16 Plus

    Same geometry (6.7" OLED @3x, Dynamic Island)

    Screen:           430 x 932 pt   @3x (1290 x 2796 px)  460 PPI
    Corner radius:    55.0 pt
    Status bar:       54 pt
    Home indicator:   34 pt portrait, 21 pt landscape

    Safe area (portrait):   top=59  bottom=34  left=0   right=0
    Safe area (landscape):  top=0   bottom=21  left=59  right=59

    Dynamic Island:   ~126 pt wide x ~37 pt tall, centered, ~11 pt from top

### iPhone 16 Pro

    New screen size (6.3" OLED @3x, Dynamic Island)

    Screen:           402 x 874 pt   @3x (1206 x 2622 px)  460 PPI
    Corner radius:    62.0 pt
    Status bar:       54 pt
    Home indicator:   34 pt portrait, 21 pt landscape

    Safe area (portrait):   top=62  bottom=34  left=0   right=0
    Safe area (landscape):  top=0   bottom=21  left=62  right=62

    Dynamic Island:   ~126 pt wide x ~37 pt tall, centered, ~11 pt from top

### iPhone 16 Pro Max

    New screen size (6.9" OLED @3x, Dynamic Island)

    Screen:           440 x 956 pt   @3x (1320 x 2868 px)  460 PPI
    Corner radius:    62.0 pt
    Status bar:       54 pt
    Home indicator:   34 pt portrait, 21 pt landscape

    Safe area (portrait):   top=62  bottom=34  left=0   right=0
    Safe area (landscape):  top=0   bottom=21  left=62  right=62

    Dynamic Island:   ~126 pt wide x ~37 pt tall, centered, ~11 pt from top

---

## Notch iPhones

All notch models: home indicator 34pt portrait / 21pt landscape, bottom
safe area 34pt portrait / 21pt landscape.  The notch is centered
horizontally, flush with the top edge of the screen.

iPhone 12-era notch: ~35mm wide (~209pt on 6.1" @3x screens).
iPhone 13-era notch: ~20% narrower (~26-28mm), very slightly taller.
iPhone 14 / 14 Plus / 16e reuse the narrower 13-era notch.

### iPhone X / iPhone XS / iPhone 11 Pro

    Same geometry (5.8" OLED @3x, wide notch)

    Screen:           375 x 812 pt   @3x (1125 x 2436 px)  458 PPI
    Corner radius:    39.0 pt
    Status bar:       44 pt
    Home indicator:   34 pt portrait, 21 pt landscape
    Notch:            ~209 pt wide x ~30 pt tall (wide, 35mm)

    Safe area (portrait):   top=44  bottom=34  left=0   right=0
    Safe area (landscape):  top=0   bottom=21  left=44  right=44

### iPhone XS Max / iPhone 11 Pro Max

    Same geometry (6.5" OLED @3x, wide notch)

    Screen:           414 x 896 pt   @3x (1242 x 2688 px)  458 PPI
    Corner radius:    39.0 pt
    Status bar:       44 pt
    Home indicator:   34 pt portrait, 21 pt landscape
    Notch:            ~209 pt wide x ~30 pt tall (wide, 35mm)

    Safe area (portrait):   top=44  bottom=34  left=0   right=0
    Safe area (landscape):  top=0   bottom=21  left=44  right=44

### iPhone XR / iPhone 11

    Same geometry (6.1" LCD @2x, wide notch)

    Screen:           414 x 896 pt   @2x (828 x 1792 px)   326 PPI
    Corner radius:    41.5 pt
    Status bar:       48 pt
    Home indicator:   34 pt portrait, 21 pt landscape
    Notch:            ~209 pt wide x ~33 pt tall (wider bezel, LCD panel)

    Safe area (portrait):   top=48  bottom=34  left=0   right=0
    Safe area (landscape):  top=0   bottom=21  left=48  right=48

    Note: Taller status bar / top inset (48 vs 44) and larger corner
    radius (41.5 vs 39.0) due to thicker LCD bezels.

### iPhone 12 mini / iPhone 13 mini

    Same geometry (5.4" OLED @3x, notch)

    Screen:           375 x 812 pt   @3x (1080 x 2340 px)  476 PPI
    Corner radius:    44.0 pt
    Status bar:       50 pt
    Home indicator:   34 pt portrait, 21 pt landscape
    Notch:            12 mini: wide (~35mm), 13 mini: narrow (~26mm)

    Safe area (portrait):   top=50  bottom=34  left=0   right=0
    Safe area (landscape):  top=0   bottom=21  left=50  right=50

    Note: Same logical size as iPhone X (375x812) but higher PPI (476).
    Native pixel ratio is ~2.88x, rendered at @3x and downsampled.
    Higher top inset (50) than iPhone X (44) due to different sensor layout.

### iPhone 12 / iPhone 12 Pro / iPhone 13 / iPhone 13 Pro

    Same geometry (6.1" OLED @3x, notch)

    Screen:           390 x 844 pt   @3x (1170 x 2532 px)  460 PPI
    Corner radius:    47.33 pt
    Status bar:       47 pt
    Home indicator:   34 pt portrait, 21 pt landscape
    Notch:            12/12 Pro: wide (~35mm), 13/13 Pro: narrow (~26mm)

    Safe area (portrait):   top=47  bottom=34  left=0   right=0
    Safe area (landscape):  top=0   bottom=21  left=47  right=47

### iPhone 12 Pro Max / iPhone 13 Pro Max

    Same geometry (6.7" OLED @3x, notch)

    Screen:           428 x 926 pt   @3x (1284 x 2778 px)  458 PPI
    Corner radius:    53.33 pt
    Status bar:       47 pt
    Home indicator:   34 pt portrait, 21 pt landscape
    Notch:            12 PM: wide (~35mm), 13 PM: narrow (~26mm)

    Safe area (portrait):   top=47  bottom=34  left=0   right=0
    Safe area (landscape):  top=0   bottom=21  left=47  right=47

### iPhone 14 / iPhone 16e

    Same geometry (6.1" OLED @3x, narrow notch)

    Screen:           390 x 844 pt   @3x (1170 x 2532 px)  460 PPI
    Corner radius:    47.33 pt
    Status bar:       47 pt
    Home indicator:   34 pt portrait, 21 pt landscape
    Notch:            narrow (~26-28mm), same as iPhone 13

    Safe area (portrait):   top=47  bottom=34  left=0   right=0
    Safe area (landscape):  top=0   bottom=21  left=47  right=47

    Note: iPhone 16e (2025) uses a notch, NOT a Dynamic Island.
    Same panel and geometry as iPhone 14.

### iPhone 14 Plus

    6.7" OLED @3x, narrow notch

    Screen:           428 x 926 pt   @3x (1284 x 2778 px)  458 PPI
    Corner radius:    53.33 pt
    Status bar:       47 pt
    Home indicator:   34 pt portrait, 21 pt landscape
    Notch:            narrow (~26-28mm), same as iPhone 13

    Safe area (portrait):   top=47  bottom=34  left=0   right=0
    Safe area (landscape):  top=0   bottom=21  left=47  right=47

---

## iPhone SE (Home Button)

### iPhone SE 2nd gen / iPhone SE 3rd gen

    Same geometry (4.7" LCD @2x, no notch, physical home button)

    Screen:           375 x 667 pt   @2x (750 x 1334 px)   326 PPI
    Corner radius:    0 pt (square display corners)
    Status bar:       20 pt
    Home indicator:   none (physical home button)

    Safe area (portrait):   top=20  bottom=0   left=0  right=0
    Safe area (landscape):  top=0   bottom=0   left=0  right=0

---

## iPads

All modern iPads: no notch, no Dynamic Island.  Status bar 24pt (both
orientations), home indicator 20pt (both orientations).

### iPad Pro 13" (M4/M5)

    Screen:           1032 x 1376 pt  @2x (2064 x 2752 px)
    Corner radius:    18.0 pt
    Status bar:       24 pt (both orientations)
    Home indicator:   20 pt (both orientations)

    Safe area (portrait):   top=24  bottom=20  left=0  right=0
    Safe area (landscape):  top=24  bottom=20  left=0  right=0

### iPad Pro 11" (M4)

    Screen:           834 x 1210 pt   @2x (1668 x 2420 px)
    Corner radius:    18.0 pt
    Status bar:       24 pt (both orientations)
    Home indicator:   20 pt (both orientations)

    Safe area (portrait):   top=24  bottom=20  left=0  right=0
    Safe area (landscape):  top=24  bottom=20  left=0  right=0

### iPad Air 13" (M2/M3)

    Screen:           1024 x 1366 pt  @2x (2048 x 2732 px)
    Corner radius:    18.0 pt
    Status bar:       24 pt (both orientations)
    Home indicator:   20 pt (both orientations)

    Safe area (portrait):   top=24  bottom=20  left=0  right=0
    Safe area (landscape):  top=24  bottom=20  left=0  right=0

---

## Quick-Reference: All Unique iPhone Geometries

    Screen (pt)    Pixels           @   PPI  Radius  Top   Bot  Cutout   Models
    -----------    ------           --  ---  ------  ---   ---  ------   ------
    375 x 667      750 x 1334      2x  326   0       20    0   none     SE 2, SE 3
    375 x 812     1125 x 2436      3x  458  39.0     44   34   notch    X, XS, 11 Pro
    414 x 896     1242 x 2688      3x  458  39.0     44   34   notch    XS Max, 11 Pro Max
    414 x 896      828 x 1792      2x  326  41.5     48   34   notch    XR, 11
    375 x 812     1080 x 2340      3x  476  44.0     50   34   notch    12 mini, 13 mini
    390 x 844     1170 x 2532      3x  460  47.33    47   34   notch    12, 12 Pro, 13, 13 Pro, 14, 16e
    428 x 926     1284 x 2778      3x  458  53.33    47   34   notch    12 PM, 13 PM, 14 Plus
    393 x 852     1179 x 2556      3x  460  55.0     59   34   DI       14 Pro, 15, 15 Pro, 16
    430 x 932     1290 x 2796      3x  460  55.0     59   34   DI       14 PM, 15 Plus, 15 PM, 16 Plus
    402 x 874     1206 x 2622      3x  460  62.0     62   34   DI       16 Pro
    440 x 956     1320 x 2868      3x  460  62.0     62   34   DI       16 Pro Max

    Landscape safe area pattern (all Face ID iPhones):
      top=0  bottom=21  left=(portrait top)  right=(portrait top)

---

## Corner Radius by Model

    Radius (pt)  Models
    -----------  ------
      0          iPhone SE (2nd, 3rd gen)
     39.0        iPhone X, XS, XS Max, 11 Pro, 11 Pro Max
     41.5        iPhone XR, 11
     44.0        iPhone 12 mini, 13 mini
     47.33       iPhone 12, 12 Pro, 13, 13 Pro, 14, 14 Plus, 16e
     53.33       iPhone 12 Pro Max, 13 Pro Max
     55.0        iPhone 14 Pro, 14 Pro Max, 15 (all), 16, 16 Plus
     62.0        iPhone 16 Pro, 16 Pro Max
     18.0        All modern iPad Pro / Air

---

## Notch Dimensions

The notch is centered horizontally, flush with the top edge of the screen.
Apple does not publish exact notch dimensions in points; these are
community-measured values.

    Generation         Physical Width   Pt Width (6.1")  Height   Notes
    ---------------    --------------   ---------------  ------   -----
    X / XS / XS Max     ~35mm           ~209 pt          ~30 pt  Original wide notch
    XR / 11              ~36-37mm        ~209 pt          ~33 pt  Slightly taller (LCD)
    11 Pro / 11 PM       ~35mm           ~209 pt          ~30 pt  Same as X/XS
    12 (all)             ~34.6-34.8mm    ~209 pt          ~30 pt  Unchanged
    13 (all)             ~26-28mm        ~166 pt          ~32 pt  20% narrower, slightly taller
    14 / 14 Plus / 16e   ~26-28mm        ~166 pt          ~32 pt  Same as 13-era

    Notch width in points scales with screen size: ~209pt on 6.1" @3x,
    proportionally wider/narrower on 5.4"/5.8"/6.5"/6.7" screens.
    Safe area top inset is the reliable layout value; notch pts are approximate.

---

## Dynamic Island Dimensions

The DI is two hardware cutouts (pill camera + circle Face ID) with the gap
blacked out by OLED.  The visible software pill in compact (idle) state:

    All DI models (14 Pro through 16 Pro Max):
      Width:     ~126 pt (378 px @3x)
      Height:    ~37 pt (112 px @3x)
      Position:  centered horizontally, ~11 pt from screen top to DI bottom
      Corner radius of pill: ~44 pt

    The physical hardware cutout has been ~20.76mm wide across all DI
    generations (unchanged from iPhone 14 Pro through iPhone 16 Pro Max).

    Live Activity compact region is wider than the hardware cutout
    (iOS draws expanded black around it).  Max expanded height: ~160 pt.

---

## Dynamic Island in Landscape

In landscape, the DI rotates to the near edge (left or right depending on
rotation direction).  Portrait "width" becomes landscape "height" and vice
versa:

    DI models with 55pt corners (14 Pro, 15 series, 16, 16 Plus):
      DI on near edge:     ~37 pt horizontal depth x ~126 pt vertical span
      DI vertical center:  screen midpoint (196-220 pt from nearest corner)
      Leading safe area:   59 pt

    DI models with 62pt corners (16 Pro, 16 Pro Max):
      DI on near edge:     ~37 pt horizontal depth x ~126 pt vertical span
      DI vertical center:  screen midpoint (201-220 pt from nearest corner)
      Leading safe area:   62 pt

    The leading/trailing safe area inset (59 or 62 pt) covers the
    horizontal depth of the DI.  There is NO additional vertical safe
    area inset for the DI in landscape -- content can be placed at any
    Y position.  The DI simply occludes whatever is behind it.

    This means a vertical strip of buttons on the DI side WILL be
    hidden behind the DI at the vertical center of the screen, even
    though the safe area system doesn't warn about it.

---

## Notch in Landscape

In landscape, the notch rotates to the near edge.  The safe area inset on
that side equals the portrait top inset:

    Notch models with top=44 (X, XS, XS Max, 11 Pro, 11 Pro Max):
      Notch on near edge:  ~30 pt horizontal depth x ~209 pt vertical span
      Leading safe area:   44 pt

    Notch models with top=48 (XR, 11):
      Notch on near edge:  ~33 pt horizontal depth x ~209 pt vertical span
      Leading safe area:   48 pt

    Notch models with top=47 (12-14 era, 16e):
      Notch on near edge:  ~30-32 pt horizontal depth x notch-width vertical
      Leading safe area:   47 pt

    Notch models with top=50 (12 mini, 13 mini):
      Leading safe area:   50 pt

    Landscape left/right safe area = portrait top inset (both sides,
    since iOS doesn't know which way you rotated).

---

## Key Observations for Layout

  1. Status bar height != safe area top inset on many models.
     DI phones: status bar 54pt, top inset 59 or 62pt (extra padding below).
     Mini phones: status bar ~44-50pt varies.

  2. Bottom safe area is always 34pt portrait / 21pt landscape on all
     Face ID iPhones (home indicator zone).  SE has 0 (physical button).

  3. Landscape safe area left/right always equals portrait top inset.
     This is symmetric -- both sides get the full inset regardless of
     which side the notch/DI is actually on.

  4. The DI hardware cutout size has not changed across generations.
     The safe area increase from 59 to 62pt on 16 Pro/Pro Max comes from
     the larger corner radius (62 vs 55), not a larger DI.

  5. Only 11 unique iPhone screen geometries exist (see quick-reference
     table).  Many "different" models share the exact same layout.

---

## Mask Overlay Project

TODO: Build tooling to overlay device masks on simulator screenshots.
See task in project task list.

Approach options:
  1. Swift script using CoreGraphics to draw rounded rect + notch/DI cutout
     mask over a screenshot PNG
  2. Pre-rendered mask PNGs for each device at @2x/@3x
  3. SwiftUI preview overlay that shows the mask in Xcode previews
  4. Python/ImageMagick script for CLI use

The mask should show:
  - Rounded corners (filled black outside the display area)
  - Notch or Dynamic Island cutout (filled black)
  - Safe area boundary lines (thin colored lines)
  - Home indicator zone (translucent overlay)

---

## Sources

  useyourloaf.com/blog/iphone-16-screen-sizes/
  useyourloaf.com/blog/iphone-15-screen-sizes/
  useyourloaf.com/blog/iphone-14-screen-sizes/
  useyourloaf.com/blog/iphone-13-screen-sizes/
  useyourloaf.com/blog/iphone-12-screen-sizes/
  useyourloaf.com/blog/iphone-17-screen-sizes/  (includes 16e and master table)
  useyourloaf.com/blog/supporting-iphone-x/
  useyourloaf.com/blog/ipad-2024-screen-sizes/
  kylebashour.com/posts/finding-the-real-iphone-x-corner-radius
  github.com/kylebshr/ScreenCorners
  iosref.com/res
  mjtsai.com/blog/2018/09/14/screens-of-the-2018-iphones/  (notch heights)
  developer.apple.com/accessories/Accessory-Design-Guidelines.pdf  (physical mm)
  developer.apple.com/forums/thread/662466  (status bar heights)
