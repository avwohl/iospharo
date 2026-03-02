#!/usr/bin/env python3
"""
Apply a device screen mask to a simulator screenshot.

Generates squircle corners, Dynamic Island / notch cutout, and optional
safe area boundary lines per the device-geometry skill algorithm (Part 4).

Auto-detects device from screenshot pixel dimensions.

Usage:
    python3 scripts/apply_device_mask.py screenshot.png [masked.png]
    python3 scripts/apply_device_mask.py --safe-areas screenshot.png
    python3 scripts/apply_device_mask.py --device iphone16pro screenshot.png
"""

import argparse
import math
import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw
except ImportError:
    print("ERROR: Pillow required.  pip3 install Pillow", file=sys.stderr)
    sys.exit(1)

# ---------------------------------------------------------------------------
# Device database (from device-geometry skill Part 1 + Part 4)
# ---------------------------------------------------------------------------

# Each entry: (portrait_w_pt, portrait_h_pt, scale, R, SA_top, SA_bottom,
#              cutout_type, cutout_info)
# cutout_type: "di", "notch_wide", "notch_narrow", "none"
# cutout_info for DI: (width_pt=126, height_pt=37, corner_r=18.5, gap_from_edge=11)
# cutout_info for notch: (depth_pt, span_pt)

DI_INFO = (126, 37, 18.5, 11)
NOTCH_WIDE = (30, 209)    # X/XS/12-era
NOTCH_NARROW = (32, 166)  # 13/14/16e-era

DEVICES = {
    # (landscape_w_px, landscape_h_px): (name, w_pt, h_pt, scale, R, SA_lead, SA_bot, cutout_type, cutout_info)

    # Dynamic Island phones
    (2556, 1179): ("iPhone 14P/15/15P/16",       852, 393, 3, 55.0,  59, 21, "di", DI_INFO),
    (2796, 1290): ("iPhone 14PM/15+/15PM/16+",   932, 430, 3, 55.0,  59, 21, "di", DI_INFO),
    (2622, 1206): ("iPhone 16 Pro",               874, 402, 3, 62.0,  62, 21, "di", DI_INFO),
    (2868, 1320): ("iPhone 16 Pro Max",           956, 440, 3, 62.0,  62, 21, "di", DI_INFO),

    # Notch phones
    (2436, 1125): ("iPhone X/XS/11 Pro",          812, 375, 3, 39.0,  44, 21, "notch_wide",   NOTCH_WIDE),
    (2688, 1242): ("iPhone XS Max/11 Pro Max",    896, 414, 3, 39.0,  44, 21, "notch_wide",   NOTCH_WIDE),
    (1792,  828): ("iPhone XR/11",                896, 414, 2, 41.5,  48, 21, "notch_wide",   NOTCH_WIDE),
    (2340, 1080): ("iPhone 12 mini/13 mini",      812, 375, 3, 44.0,  50, 21, "notch_narrow", NOTCH_NARROW),
    (2532, 1170): ("iPhone 12/13/14/16e",         844, 390, 3, 47.33, 47, 21, "notch_narrow", NOTCH_NARROW),
    (2778, 1284): ("iPhone 12PM/13PM/14 Plus",    926, 428, 3, 53.33, 47, 21, "notch_narrow", NOTCH_NARROW),

    # No cutout
    (1334,  750): ("iPhone SE 2/3",               667, 375, 2, 0,     20,  0, "none", None),
}


def detect_device(w_px, h_px):
    """Auto-detect device from pixel dimensions (landscape or portrait)."""
    # Try landscape first
    key = (w_px, h_px)
    if key in DEVICES:
        return DEVICES[key], "landscape"
    # Try portrait (swap)
    key = (h_px, w_px)
    if key in DEVICES:
        return DEVICES[key], "portrait"
    return None, None


def squircle_outside(x_pt, y_pt, R):
    """True if (x_pt, y_pt) from corner origin is outside the squircle."""
    if x_pt >= R or y_pt >= R:
        return False
    return (R - x_pt)**5 + (R - y_pt)**5 > R**5


def draw_squircle_corners(mask, w_px, h_px, R, scale):
    """Black out pixels outside the squircle in all four corners."""
    if R <= 0:
        return
    r_px = int(math.ceil(R * scale))
    pixels = mask.load()

    for py in range(r_px):
        y_pt = py / scale
        for px in range(r_px):
            x_pt = px / scale
            if squircle_outside(x_pt, y_pt, R):
                # Top-left
                pixels[px, py] = (0, 0, 0, 255)
                # Top-right
                pixels[w_px - 1 - px, py] = (0, 0, 0, 255)
                # Bottom-left
                pixels[px, h_px - 1 - py] = (0, 0, 0, 255)
                # Bottom-right
                pixels[w_px - 1 - px, h_px - 1 - py] = (0, 0, 0, 255)


def draw_dynamic_island(mask, w_pt, h_pt, scale, orientation, di_info):
    """Draw the Dynamic Island cutout as a black rounded rectangle."""
    di_span, di_depth, di_corner_r, di_gap = di_info
    draw = ImageDraw.Draw(mask)

    if orientation == "landscape":
        # DI on left edge, centered vertically
        # In landscape: di_depth is horizontal (into screen), di_span is vertical
        x0 = 0
        x1 = int((di_gap + di_depth) * scale)
        y_center = h_pt * scale / 2
        y0 = int(y_center - di_span * scale / 2)
        y1 = int(y_center + di_span * scale / 2)
        r = int(di_corner_r * scale)
        draw.rounded_rectangle([x0, y0, x1, y1], radius=r, fill=(0, 0, 0, 255))
    else:
        # DI at top, centered horizontally
        x_center = w_pt * scale / 2
        x0 = int(x_center - di_span * scale / 2)
        x1 = int(x_center + di_span * scale / 2)
        y0 = 0
        y1 = int((di_gap + di_depth) * scale)
        r = int(di_corner_r * scale)
        draw.rounded_rectangle([x0, y0, x1, y1], radius=r, fill=(0, 0, 0, 255))


def draw_notch(mask, w_pt, h_pt, scale, orientation, notch_info):
    """Draw the notch cutout as a black rectangle."""
    depth_pt, span_pt = notch_info
    draw = ImageDraw.Draw(mask)

    if orientation == "landscape":
        # Notch on left edge, centered vertically
        y_center = h_pt * scale / 2
        x0 = 0
        x1 = int(depth_pt * scale)
        y0 = int(y_center - span_pt * scale / 2)
        y1 = int(y_center + span_pt * scale / 2)
        draw.rectangle([x0, y0, x1, y1], fill=(0, 0, 0, 255))
    else:
        # Notch at top, centered horizontally
        x_center = w_pt * scale / 2
        x0 = int(x_center - span_pt * scale / 2)
        x1 = int(x_center + span_pt * scale / 2)
        y0 = 0
        y1 = int(depth_pt * scale)
        draw.rectangle([x0, y0, x1, y1], fill=(0, 0, 0, 255))


def draw_safe_areas(img, w_pt, h_pt, scale, sa_lead, sa_bot, orientation):
    """Draw safe area boundary lines on the image."""
    draw = ImageDraw.Draw(img)
    w_px = int(w_pt * scale)
    h_px = int(h_pt * scale)
    red = (255, 0, 0, 180)
    blue = (0, 100, 255, 180)
    green = (0, 200, 0, 180)

    if orientation == "landscape":
        # Leading safe area (left, for DI/notch)
        x = int(sa_lead * scale)
        draw.line([(x, 0), (x, h_px)], fill=blue, width=2)
        # Trailing safe area (right, symmetric)
        x2 = w_px - int(sa_lead * scale)
        draw.line([(x2, 0), (x2, h_px)], fill=blue, width=2)
        # Bottom safe area (home indicator)
        if sa_bot > 0:
            y = h_px - int(sa_bot * scale)
            draw.line([(0, y), (w_px, y)], fill=green, width=2)
    else:
        # Top safe area
        y = int(sa_lead * scale)  # SA_top = SA_lead in portrait for notch/DI
        draw.line([(0, y), (w_px, y)], fill=red, width=2)
        # Bottom safe area
        if sa_bot > 0:
            y2 = h_px - int(sa_bot * scale)
            draw.line([(0, y2), (w_px, y2)], fill=green, width=2)


def apply_mask(screenshot_path, output_path, device_override=None,
               show_safe_areas=False):
    """Load screenshot, generate mask, composite, save."""
    img = Image.open(screenshot_path).convert("RGBA")
    w_px, h_px = img.size

    if device_override:
        # Find by name prefix
        match = None
        for key, dev in DEVICES.items():
            if device_override.lower().replace("-", "").replace("_", "").replace(" ", "") in \
               dev[0].lower().replace("/", "").replace(" ", "").replace("-", ""):
                match = (dev, None)
                break
        if not match:
            print(f"ERROR: Unknown device '{device_override}'", file=sys.stderr)
            print("Known devices:", file=sys.stderr)
            for dev in DEVICES.values():
                print(f"  {dev[0]}", file=sys.stderr)
            sys.exit(1)
        dev_info = match[0]
        # Detect orientation from aspect ratio
        orientation = "landscape" if w_px > h_px else "portrait"
    else:
        dev_info, orientation = detect_device(w_px, h_px)

    if dev_info is None:
        print(f"ERROR: Cannot detect device from {w_px}x{h_px} pixels",
              file=sys.stderr)
        print("Known landscape sizes:", file=sys.stderr)
        for (lw, lh), dev in sorted(DEVICES.items()):
            print(f"  {lw}x{lh}  {dev[0]}", file=sys.stderr)
        sys.exit(1)

    name, w_pt, h_pt, scale, R, sa_lead, sa_bot, cutout_type, cutout_info = dev_info

    # In landscape, the image is w_pt x h_pt; in portrait it's h_pt x w_pt...
    # Actually w_pt and h_pt in our table are portrait dimensions.
    # Swap for landscape.
    if orientation == "landscape":
        disp_w_pt, disp_h_pt = w_pt, h_pt  # already landscape in our table
    else:
        disp_w_pt, disp_h_pt = h_pt, w_pt  # our table has landscape; swap for portrait
        # Actually our table stores landscape w x h. For portrait, swap.
        # Wait — the table says "Screen (landscape pt)" so w_pt is landscape width.
        # Portrait: width = h_pt, height = w_pt
        disp_w_pt, disp_h_pt = h_pt, w_pt

    expected_w = int(disp_w_pt * scale)
    expected_h = int(disp_h_pt * scale)

    print(f"Device: {name}")
    print(f"Orientation: {orientation}")
    print(f"Screen: {disp_w_pt}x{disp_h_pt} pt  @{scale}x  ({expected_w}x{expected_h} px)")
    print(f"Image:  {w_px}x{h_px} px")
    print(f"Corner radius: {R} pt")
    print(f"Cutout: {cutout_type}")

    # Create mask (white = visible, black = occluded)
    mask = Image.new("RGBA", (w_px, h_px), (255, 255, 255, 0))

    # Step 2: Squircle corners
    draw_squircle_corners(mask, w_px, h_px, R, scale)

    # Step 3: Cutout
    if cutout_type == "di" and cutout_info:
        draw_dynamic_island(mask, disp_w_pt, disp_h_pt, scale, orientation, cutout_info)
    elif cutout_type.startswith("notch") and cutout_info:
        draw_notch(mask, disp_w_pt, disp_h_pt, scale, orientation, cutout_info)

    # Composite: where mask is black, darken the screenshot heavily
    # so occluded areas are visible but clearly marked as off-screen
    import numpy as np
    img_arr = np.array(img)
    mask_arr = np.array(mask)
    # Black pixels in mask: R=0, G=0, B=0, A=255
    occluded = (mask_arr[:, :, 0] == 0) & (mask_arr[:, :, 1] == 0) & \
               (mask_arr[:, :, 2] == 0) & (mask_arr[:, :, 3] == 255)
    # Darken occluded regions to ~20% brightness
    img_arr[occluded, 0] = img_arr[occluded, 0] // 5
    img_arr[occluded, 1] = img_arr[occluded, 1] // 5
    img_arr[occluded, 2] = img_arr[occluded, 2] // 5
    img = Image.fromarray(img_arr)

    # Step 4: Safe area lines
    if show_safe_areas:
        draw_safe_areas(img, disp_w_pt, disp_h_pt, scale, sa_lead, sa_bot, orientation)

    # Save
    img.save(output_path)
    print(f"Saved: {output_path}")


def main():
    parser = argparse.ArgumentParser(
        description="Apply device screen mask to simulator screenshot")
    parser.add_argument("input", help="Screenshot PNG file")
    parser.add_argument("output", nargs="?", help="Output file (default: input_masked.png)")
    parser.add_argument("--device", help="Device name override (auto-detect if omitted)")
    parser.add_argument("--safe-areas", action="store_true",
                        help="Draw safe area boundary lines")
    args = parser.parse_args()

    input_path = Path(args.input)
    if not input_path.exists():
        print(f"ERROR: {input_path} not found", file=sys.stderr)
        sys.exit(1)

    if args.output:
        output_path = Path(args.output)
    else:
        output_path = input_path.with_stem(input_path.stem + "_masked")

    apply_mask(str(input_path), str(output_path),
               device_override=args.device,
               show_safe_areas=args.safe_areas)


if __name__ == "__main__":
    main()
