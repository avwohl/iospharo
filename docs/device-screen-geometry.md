# Device Screen Geometry Reference

Precise point values for screen dimensions, safe area insets, corner radii,
and Dynamic Island positions.  Used for building mask overlays to evaluate
UI layouts without testing on physical hardware.

All values in **points** (UIKit/SwiftUI logical units), portrait unless noted.

## iPhone 16

    Screen:           393 x 852 pt   @3x (1179 x 2556 px)
    Corner radius:    55.0 pt (continuous/squircle)
    Status bar:       54 pt
    Home indicator:   34 pt portrait, 21 pt landscape
    Dynamic Island:   yes

    Safe area (portrait):   top=59  bottom=34  left=0   right=0
    Safe area (landscape):  top=0   bottom=21  left=59  right=59

    Dynamic Island cutout (estimated from pixel measurements):
      Position:    centered horizontally, ~18.5 pt from top edge
      Size:        ~126 pt wide x ~37 pt tall
      The 59 pt top safe area inset is the official exclusion zone

## iPhone 16 Pro

    Screen:           402 x 874 pt   @3x (1206 x 2622 px)
    Corner radius:    62.0 pt (continuous/squircle)
    Status bar:       54 pt
    Home indicator:   34 pt portrait, 21 pt landscape
    Dynamic Island:   yes

    Safe area (portrait):   top=62  bottom=34  left=0   right=0
    Safe area (landscape):  top=0   bottom=21  left=62  right=62

    Dynamic Island cutout (estimated):
      Position:    centered horizontally, ~19 pt from top edge
      Size:        ~162 pt wide x ~36 pt tall
      The 62 pt top safe area inset is the official exclusion zone

## iPhone 16 Pro Max

    Screen:           440 x 956 pt   @3x (1320 x 2868 px)
    Corner radius:    62.0 pt (continuous/squircle)
    Status bar:       54 pt
    Home indicator:   34 pt portrait, 21 pt landscape
    Dynamic Island:   yes

    Safe area (portrait):   top=62  bottom=34  left=0   right=0
    Safe area (landscape):  top=0   bottom=21  left=62  right=62

    Dynamic Island cutout (from 767x108 px teardown):
      Position:    centered horizontally, ~19 pt from top edge
      Size:        ~256 pt wide x ~36 pt tall
      The 62 pt top safe area inset is the official exclusion zone

## iPhone 15 / iPhone 14 Pro

    Screen:           393 x 852 pt   @3x (1179 x 2556 px)
    Corner radius:    55.0 pt (continuous/squircle)
    Status bar:       54 pt
    Home indicator:   34 pt portrait, 21 pt landscape
    Dynamic Island:   yes

    Safe area (portrait):   top=59  bottom=34  left=0   right=0
    Safe area (landscape):  top=0   bottom=21  left=59  right=59

    Same geometry as iPhone 16 standard.

## iPad Pro 13" (M4/M5)

    Screen:           1032 x 1376 pt  @2x (2064 x 2752 px)
    Corner radius:    18.0 pt (continuous/squircle)
    Status bar:       24 pt (both orientations)
    Home indicator:   20 pt (both orientations)
    Dynamic Island:   no

    Safe area (portrait):   top=24  bottom=20  left=0  right=0
    Safe area (landscape):  top=24  bottom=20  left=0  right=0

## iPad Pro 11" (M4)

    Screen:           834 x 1210 pt   @2x (1668 x 2420 px)
    Corner radius:    18.0 pt (continuous/squircle)
    Status bar:       24 pt (both orientations)
    Home indicator:   20 pt (both orientations)
    Dynamic Island:   no

    Safe area (portrait):   top=24  bottom=20  left=0  right=0
    Safe area (landscape):  top=24  bottom=20  left=0  right=0

## iPad Air 13" (M2/M3)

    Screen:           1024 x 1366 pt  @2x (2048 x 2732 px)
    Corner radius:    18.0 pt (continuous/squircle)
    Status bar:       24 pt (both orientations)
    Home indicator:   20 pt (both orientations)
    Dynamic Island:   no

    Safe area (portrait):   top=24  bottom=20  left=0  right=0
    Safe area (landscape):  top=24  bottom=20  left=0  right=0

## Dynamic Island in Landscape

In landscape orientation, the Dynamic Island rotates to the near edge
(left or right depending on rotation direction).  What was "width" in
portrait becomes "height" in landscape and vice versa:

    iPhone 16 (393pt screen, 55pt corners):
      Landscape screen:    852 x 393 pt
      DI on near edge:     ~37 pt horizontal depth x ~126 pt vertical span
      DI vertical center:  ~196 pt from nearest corner (screen midpoint)
      Leading safe area:   59 pt (the official exclusion zone)

    iPhone 16 Pro (402pt screen, 62pt corners):
      Landscape screen:    874 x 402 pt
      DI on near edge:     ~36 pt horizontal depth x ~162 pt vertical span
      DI vertical center:  ~201 pt from nearest corner
      Leading safe area:   62 pt

    iPhone 16 Pro Max (440pt screen, 62pt corners):
      Landscape screen:    956 x 440 pt
      DI on near edge:     ~36 pt horizontal depth x ~256 pt vertical span
      DI vertical center:  ~220 pt from nearest corner
      Leading safe area:   62 pt

    The leading/trailing safe area inset (59 or 62 pt) covers the
    horizontal depth of the DI.  There is NO additional vertical safe
    area inset for the DI in landscape — content can be placed at any
    Y position.  The DI simply occludes whatever is behind it.

    This means a vertical strip of buttons on the DI side WILL be
    hidden behind the DI at the vertical center of the screen, even
    though the safe area system doesn't warn about it.

## Corner Radius History

    iPhone X / Xs / 11 Pro:              39.0 pt
    iPhone Xr / 11:                      41.5 pt
    iPhone 12 mini:                      44.0 pt
    iPhone 12 / 12 Pro / 13 Pro:         47.33 pt
    iPhone 12 Pro Max / 13 Pro Max:      53.33 pt
    iPhone 14 Pro / 15 / 16:             55.0 pt
    iPhone 16 Pro / 16 Pro Max:          62.0 pt
    All modern iPad Pro / Air:           18.0 pt

    Values from UIScreen._displayCornerRadius (private API).
    All use CALayerCornerCurve.continuous (squircle), not circular arcs.

## Mask Overlay Project

TODO: Build tooling to overlay device masks on simulator screenshots.
See task in project task list.

Approach options:
  1. Swift script using CoreGraphics to draw rounded rect + DI cutout
     mask over a screenshot PNG
  2. Pre-rendered mask PNGs for each device at @2x/@3x
  3. SwiftUI preview overlay that shows the mask in Xcode previews
  4. Python/ImageMagick script for CLI use

The mask should show:
  - Rounded corners (filled black outside the display area)
  - Dynamic Island cutout (filled black)
  - Safe area boundary lines (thin colored lines)
  - Home indicator zone (translucent overlay)

Sources:
  useyourloaf.com/blog/iphone-16-screen-sizes/
  useyourloaf.com/blog/ipad-2024-screen-sizes/
  kylebashour.com/posts/finding-the-real-iphone-x-corner-radius
  github.com/kylebshr/ScreenCorners
  ios-resolution.com
