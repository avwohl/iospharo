/*
 * SDL2Blit.hpp - exact line drawing, rotated copy and surface blits for the
 * software renderer of the SDL2 stub layer.
 *
 * Built on SDL2Raster.hpp.  The rules follow the SDL 2.x software renderer:
 *  - a poly-line draws each segment without its last point, then draws the
 *    last point of the whole line unless the line is closed;
 *  - a line is clipped to the clip rectangle with the integer
 *    Cohen-Sutherland method (SDL2Rect.hpp) before it is drawn;
 *  - a rotated copy scales the source rectangle to the destination size,
 *    rotates that picture about the centre into its bounding box, and then
 *    composes the box onto the target.  The rotation inverse-maps each
 *    destination pixel in 16.16 fixed point, so it samples the same source
 *    pixels as SDL for every angle; a multiple of 90 degrees takes the exact
 *    permutation path.
 *
 * Copyright (c) 2025-2026 Aaron Wohl. Licensed under the MIT License.
 */
#pragma once

#include "SDL2Raster.hpp"
#include "SDL2Rect.hpp"

#include <vector>

namespace pharo {
namespace sdl2 {

// The clip rectangle that a draw call uses: the clip inside the pixels.
inline Rect effectiveClip(const Pixels& dst, const Rect* clip) {
    Rect bounds = { 0, 0, dst.w, dst.h };
    if (!clip) return bounds;
    Rect out;
    rect::intersectRect<int>(reinterpret_cast<const rect::RectT<int>*>(&bounds),
                             reinterpret_cast<const rect::RectT<int>*>(clip),
                             reinterpret_cast<rect::RectT<int>*>(&out));
    return out;
}

inline bool inRect(const Rect& r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

struct IPoint { int x, y; };

// One segment of a poly-line, both ends included or the last one left out.
inline void lineSegment(const Pixels& dst, const Rect& clip, int x1, int y1, int x2, int y2,
                        RGBA c, const BlendSpec& spec, bool fast, bool drawEnd) {
    const int deltax = std::abs(x2 - x1), deltay = std::abs(y2 - y1);
    int numpixels, d, dinc1, dinc2, x, xinc1, xinc2, y, yinc1, yinc2;
    if (deltax >= deltay) {
        numpixels = deltax + 1; d = 2 * deltay - deltax; dinc1 = deltay * 2; dinc2 = (deltay - deltax) * 2;
        xinc1 = 1; xinc2 = 1; yinc1 = 0; yinc2 = 1;
    } else {
        numpixels = deltay + 1; d = 2 * deltax - deltay; dinc1 = deltax * 2; dinc2 = (deltax - deltay) * 2;
        xinc1 = 0; xinc2 = 1; yinc1 = 1; yinc2 = 1;
    }
    if (x1 > x2) { xinc1 = -xinc1; xinc2 = -xinc2; }
    if (y1 > y2) { yinc1 = -yinc1; yinc2 = -yinc2; }
    x = x1; y = y1;
    if (!drawEnd) --numpixels;
    for (int i = 0; i < numpixels; ++i) {
        if (inRect(clip, x, y)) dst.put(x, y, blendPixel(spec, c, dst.get(x, y), fast));
        if (d < 0) { d += dinc1; x += xinc1; y += yinc1; }
        else { d += dinc2; x += xinc2; y += yinc2; }
    }
}

inline void drawPolyline(const Pixels& dst, const Rect& clip, const IPoint* pts, int count,
                         RGBA c, uint32_t blend) {
    BlendSpec spec;
    if (!decodeBlendMode(blend, spec) || count < 1) return;
    const bool fast = blend == kBlendNone;
    for (int i = 1; i < count; ++i) {
        int x1 = pts[i - 1].x, y1 = pts[i - 1].y, x2 = pts[i].x, y2 = pts[i].y;
        if (!rect::intersectRectAndLine<int>(reinterpret_cast<const rect::RectT<int>*>(&clip), &x1, &y1, &x2, &y2))
            continue;
        // The end is drawn when the whole line is one point or the clip moved it.
        const bool drawEnd = (x1 == x2 && y1 == y2) || x2 != pts[i].x || y2 != pts[i].y;
        lineSegment(dst, clip, x1, y1, x2, y2, c, spec, fast, drawEnd);
    }
    if (pts[0].x != pts[count - 1].x || pts[0].y != pts[count - 1].y) {
        const int x = pts[count - 1].x, y = pts[count - 1].y;
        if (inRect(clip, x, y)) dst.put(x, y, blendPixel(spec, c, dst.get(x, y), fast));
    }
}

inline void drawPointsClipped(const Pixels& dst, const Rect& clip, const IPoint* pts, int count,
                              RGBA c, uint32_t blend) {
    BlendSpec spec;
    if (!decodeBlendMode(blend, spec)) return;
    const bool fast = blend == kBlendNone;
    for (int i = 0; i < count; i++)
        if (inRect(clip, pts[i].x, pts[i].y))
            dst.put(pts[i].x, pts[i].y, blendPixel(spec, c, dst.get(pts[i].x, pts[i].y), fast));
}

inline void fillRectClipped(const Pixels& dst, const Rect& clip, const Rect& r, RGBA c, uint32_t blend) {
    BlendSpec spec;
    if (!decodeBlendMode(blend, spec)) return;
    const bool fast = blend == kBlendNone;
    const int x1 = std::max(r.x, clip.x), y1 = std::max(r.y, clip.y);
    const int x2 = std::min(r.x + r.w, clip.x + clip.w), y2 = std::min(r.y + r.h, clip.y + clip.h);
    for (int y = y1; y < y2; y++)
        for (int x = x1; x < x2; x++) dst.put(x, y, blendPixel(spec, c, dst.get(x, y), fast));
}

// ---------------------------------------------------------------------------
// Rotated copy.
// ---------------------------------------------------------------------------

constexpr int kFractionMask = 0xFFFF;   // the fractional 16 bits of a 16.16 fixed point value
constexpr int kByteMask = 0xFF;

struct FPoint { float x, y; };
enum FlipMode : int { kFlipNone = 0, kFlipHorizontal = 1, kFlipVertical = 2 };

// The bounding box of the rotated picture and the (reversed) sine and cosine
// that the inverse mapping uses.
inline void rotationBox(int width, int height, double angle, const FPoint& center,
                        Rect& box, double& cangle, double& sangle) {
    const double rad = angle * (M_PI / 180.0);
    const double s = std::sin(rad), c = std::cos(rad);
    auto rot = [&](double sx, double sy, double& dx, double& dy) {
        sx -= center.x; sy -= center.y;
        dx = c * sx - s * sy + center.x;
        dy = s * sx + c * sy + center.y;
    };
    double x0, y0, x1, y1, x2, y2, x3, y3;
    rot(0.5, 0.5, x0, y0);
    rot(width - 0.5, 0.5, x1, y1);
    rot(0.5, height - 0.5, x2, y2);
    rot(width - 0.5, height - 0.5, x3, y3);
    const int minx = static_cast<int>(std::floor(std::min(std::min(x0, x1), std::min(x2, x3))));
    const int maxx = static_cast<int>(std::ceil(std::max(std::max(x0, x1), std::max(x2, x3))));
    const int miny = static_cast<int>(std::floor(std::min(std::min(y0, y1), std::min(y2, y3))));
    const int maxy = static_cast<int>(std::ceil(std::max(std::max(y0, y1), std::max(y2, y3))));
    box = { minx, miny, maxx - minx, maxy - miny };
    sangle = -s;   // the rotation is clockwise
    cangle = c;
    int a90 = static_cast<int>(angle / 90);
    if (a90 == angle / 90) {   // a multiple of 90 degrees: exact size and trigonometry
        a90 %= 4;
        if (a90 < 0) a90 += 4;
        if (a90 & 1) { box.w = height; box.h = width; cangle = 0; sangle = a90 == 1 ? -1 : 1; }
        else { box.w = width; box.h = height; cangle = a90 == 0 ? 1 : -1; sangle = 0; }
    }
}

struct RotatedPicture {
    Rect box = { 0, 0, 0, 0 };
    std::vector<RGBA> px;
    std::vector<uint8_t> valid;   // 1 where the rotation filled the pixel
};

// Rotate the picture `src` (sw x sh pixels) about `center` into its box.
inline RotatedPicture rotatePicture(const std::vector<RGBA>& src, int sw, int sh, double angle,
                                    bool smooth, bool flipx, bool flipy, const FPoint& center) {
    RotatedPicture out;
    double cangle, sangle;
    rotationBox(sw, sh, angle, center, out.box, cangle, sangle);
    const int dw = out.box.w, dh = out.box.h;
    if (dw <= 0 || dh <= 0) return out;
    out.px.assign(static_cast<size_t>(dw) * dh, RGBA{ 0, 0, 0, 0 });
    out.valid.assign(static_cast<size_t>(dw) * dh, 0);
    auto at = [&](int x, int y) -> const RGBA& { return src[static_cast<size_t>(y) * sw + x]; };
    int a90 = -1;
    if (static_cast<int>(angle / 90) == angle / 90) {
        a90 = static_cast<int>(angle / 90) % 4;
        if (a90 < 0) a90 += 4;
    }
    if (a90 >= 0) {
        // Exact permutations of the source pixels.
        for (int y = 0; y < dh; y++) {
            for (int x = 0; x < dw; x++) {
                int sx, sy;
                switch (a90) {
                case 0: sx = x; sy = y; break;
                case 1: sx = y; sy = sh - 1 - x; break;
                case 2: sx = sw - 1 - x; sy = sh - 1 - y; break;
                default: sx = sw - 1 - y; sy = x; break;
                }
                if (flipx) sx = sw - 1 - sx;
                if (flipy) sy = sh - 1 - sy;
                if (sx < 0 || sy < 0 || sx >= sw || sy >= sh) continue;
                out.px[static_cast<size_t>(y) * dw + x] = at(sx, sy);
                out.valid[static_cast<size_t>(y) * dw + x] = 1;
            }
        }
        return out;
    }
    const int isin = static_cast<int>(sangle * 65536.0), icos = static_cast<int>(cangle * 65536.0);
    const int cx = static_cast<int>(center.x * 65536.0), cy = static_cast<int>(center.y * 65536.0);
    const int half = 1 << 15;
    const int lastx = sw - 1, lasty = sh - 1;
    for (int y = 0; y < dh; y++) {
        const double srcX = out.box.x + 0.5 - center.x;
        const double srcY = out.box.y + y + 0.5 - center.y;
        int sdx = static_cast<int>((icos * srcX - isin * srcY) + cx - half);
        int sdy = static_cast<int>((isin * srcX + icos * srcY) + cy - half);
        for (int x = 0; x < dw; x++, sdx += icos, sdy += isin) {
            int dx = sdx >> 16, dy = sdy >> 16;
            const size_t o = static_cast<size_t>(y) * dw + x;
            if (smooth) {
                if (flipx) dx = lastx - dx;
                if (flipy) dy = lasty - dy;
                if (!(dx > -1 && dy > -1 && dx < sw - 1 && dy < sh - 1)) continue;
                RGBA c00 = at(dx, dy), c01 = at(dx + 1, dy), c10 = at(dx, dy + 1), c11 = at(dx + 1, dy + 1);
                if (flipx) { std::swap(c00, c01); std::swap(c10, c11); }
                if (flipy) { std::swap(c00, c10); std::swap(c01, c11); }
                const int ex = sdx & kFractionMask, ey = sdy & kFractionMask;
                auto mix = [&](int a00, int a01, int a10, int a11) {
                    const int t1 = ((((a01 - a00) * ex) >> 16) + a00) & kByteMask;
                    const int t2 = ((((a11 - a10) * ex) >> 16) + a10) & kByteMask;
                    return static_cast<uint8_t>((((t2 - t1) * ey) >> 16) + t1);
                };
                out.px[o] = RGBA{ mix(c00.r, c01.r, c10.r, c11.r), mix(c00.g, c01.g, c10.g, c11.g),
                                  mix(c00.b, c01.b, c10.b, c11.b), mix(c00.a, c01.a, c10.a, c11.a) };
                out.valid[o] = 1;
            } else {
                if (static_cast<unsigned>(dx) >= static_cast<unsigned>(sw) ||
                    static_cast<unsigned>(dy) >= static_cast<unsigned>(sh)) continue;
                if (flipx) dx = lastx - dx;
                if (flipy) dy = lasty - dy;
                out.px[o] = at(dx, dy);
                out.valid[o] = 1;
            }
        }
    }
    return out;
}

// SDL_RenderCopyEx on the software renderer.  `finalRect` is the destination
// rectangle in target pixels (viewport offset included, renderer scale not).
// `scaleX` and `scaleY` are the renderer scale, applied to the rotated result.
inline bool copyRotated(const Pixels& dst, const Rect& clip, const Pixels& src, const Rect& srcRect,
                        const Rect& finalRect, double angle, FPoint center, int flip,
                        const TextureSampling& t, bool srcHasAlpha, float scaleX, float scaleY) {
    BlendSpec spec;
    if (!decodeBlendMode(t.blend, spec)) return false;
    if (finalRect.w <= 0 || finalRect.h <= 0 || srcRect.w <= 0 || srcRect.h <= 0) return true;
    // 1. the source rectangle scaled to the destination size (no modulation, no blending)
    std::vector<RGBA> scaled(static_cast<size_t>(finalRect.w) * finalRect.h);
    {
        const bool same = srcRect.w == finalRect.w && srcRect.h == finalRect.h;
        if (t.linear && !same) {
            scaled = bilinearScale(src, srcRect, finalRect.w, finalRect.h);
        } else {
            for (int y = 0; y < finalRect.h; y++)
                for (int x = 0; x < finalRect.w; x++) {
                    const int sxi = std::min(nearestSourceIndex(x, srcRect.w, finalRect.w), srcRect.w - 1);
                    const int syi = std::min(nearestSourceIndex(y, srcRect.h, finalRect.h), srcRect.h - 1);
                    scaled[static_cast<size_t>(y) * finalRect.w + x] = src.get(srcRect.x + sxi, srcRect.y + syi);
                }
        }
    }
    // 2. the rotation
    RotatedPicture pic = rotatePicture(scaled, finalRect.w, finalRect.h, angle, t.linear,
                                       (flip & kFlipHorizontal) != 0, (flip & kFlipVertical) != 0, center);
    if (pic.px.empty()) return true;
    // 3. the box onto the target, with the renderer scale when it is not 1
    const bool fast = false;
    // SDL_BLENDMODE_NONE with a texture that has an alpha channel (or an alpha
    // modulation) first clears the destination inside the area that a
    // NEAREST rotation covers, then fills what the real rotation covers.  With
    // bilinear sampling the second area is smaller, so a ring of pixels ends
    // up cleared to transparent black.
    const bool clearsMask = t.blend == kBlendNone && (srcHasAlpha || t.aMod != 255) && t.linear;
    RotatedPicture maskPic;
    if (clearsMask) maskPic = rotatePicture(scaled, finalRect.w, finalRect.h, angle, false,
                                            (flip & kFlipHorizontal) != 0, (flip & kFlipVertical) != 0, center);
    auto compose = [&](int x, int y, RGBA s) {
        if (!inRect(clip, x, y) || x < 0 || y < 0 || x >= dst.w || y >= dst.h) return;
        s = modulate(s, t);
        const RGBA d = dst.get(x, y);
        if (t.blend == kBlendNone) { dst.put(x, y, s); return; }   // the rotated area is replaced, alpha included
        if (t.blend == kBlendBlend) { dst.put(x, y, blitBlendPixel(s, d, t.modulated())); return; }
        dst.put(x, y, blendPixel(spec, s, d, fast));
    };
    const int bx = finalRect.x + pic.box.x, by = finalRect.y + pic.box.y;
    if (scaleX == 1.0f && scaleY == 1.0f) {
        for (int y = 0; y < pic.box.h; y++)
            for (int x = 0; x < pic.box.w; x++) {
                const size_t o = static_cast<size_t>(y) * pic.box.w + x;
                if (pic.valid[o]) compose(bx + x, by + y, pic.px[o]);
                else if (clearsMask && maskPic.valid[o] && inRect(clip, bx + x, by + y) && bx + x >= 0 && by + y >= 0 && bx + x < dst.w && by + y < dst.h)
                    dst.put(bx + x, by + y, RGBA{ 0, 0, 0, 0 });
            }
        return true;
    }
    const int rx = static_cast<int>(static_cast<float>(bx) * scaleX), ry = static_cast<int>(static_cast<float>(by) * scaleY);
    const int rw = static_cast<int>(static_cast<float>(pic.box.w) * scaleX), rh = static_cast<int>(static_cast<float>(pic.box.h) * scaleY);
    if (rw <= 0 || rh <= 0) return true;
    for (int y = 0; y < rh; y++)
        for (int x = 0; x < rw; x++) {
            int sx = nearestSourceIndex(x, pic.box.w, rw), sy = nearestSourceIndex(y, pic.box.h, rh);
            sx = std::min(sx, pic.box.w - 1);
            sy = std::min(sy, pic.box.h - 1);
            const size_t o = static_cast<size_t>(sy) * pic.box.w + sx;
            if (pic.valid[o]) compose(rx + x, ry + y, pic.px[o]);
        }
    return true;
}

} // namespace sdl2
} // namespace pharo
