/*
 * SDL2Raster.hpp - software raster model behind the SDL2 stub layer.
 *
 * FFI.cpp answers the SDL2 calls of the image (OSWindow-SDL2, SDL2 package,
 * pharo-sdl-experiments) without a real SDL2 library.  This header holds the
 * parts that need no window or texture state: pixel formats, blend modes,
 * surfaces, and the drawing operations (fill, line, copy, geometry, read).
 * Every function follows the SDL 2.0.x rule for the same call.  The header
 * is header-only so that no build file needs a new source entry.
 *
 * Copyright (c) 2025-2026 Aaron Wohl. Licensed under the MIT License.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

namespace pharo {
namespace sdl2 {

struct Rect  { int x, y, w, h; };
struct FRect { float x, y, w, h; };
struct RGBA  { uint8_t r, g, b, a; };

// ---------------------------------------------------------------------------
// Pixel formats.  SDL_DEFINE_PIXELFORMAT(type, order, layout, bits, bytes).
// ---------------------------------------------------------------------------

constexpr uint32_t makePixelFormat(uint32_t type, uint32_t order, uint32_t layout,
                                   uint32_t bits, uint32_t bytes) {
    return (1u << 28) | (type << 24) | (order << 20) | (layout << 16) | (bits << 8) | bytes;
}

constexpr uint32_t kPixelTypePacked32 = 6;
constexpr uint32_t kPackedLayout8888 = 6;
enum PackedOrder : uint32_t {
    kOrderXRGB = 1, kOrderRGBX = 2, kOrderARGB = 3, kOrderRGBA = 4,
    kOrderXBGR = 5, kOrderBGRX = 6, kOrderABGR = 7, kOrderBGRA = 8
};

constexpr uint32_t kFormatARGB8888 = makePixelFormat(kPixelTypePacked32, kOrderARGB, kPackedLayout8888, 32, 4);
constexpr uint32_t kFormatXRGB8888 = makePixelFormat(kPixelTypePacked32, kOrderXRGB, kPackedLayout8888, 24, 4);
constexpr uint32_t kFormatABGR8888 = makePixelFormat(kPixelTypePacked32, kOrderABGR, kPackedLayout8888, 32, 4);
constexpr uint32_t kFormatXBGR8888 = makePixelFormat(kPixelTypePacked32, kOrderXBGR, kPackedLayout8888, 24, 4);
constexpr uint32_t kFormatRGBA8888 = makePixelFormat(kPixelTypePacked32, kOrderRGBA, kPackedLayout8888, 32, 4);
constexpr uint32_t kFormatRGBX8888 = makePixelFormat(kPixelTypePacked32, kOrderRGBX, kPackedLayout8888, 24, 4);
constexpr uint32_t kFormatBGRA8888 = makePixelFormat(kPixelTypePacked32, kOrderBGRA, kPackedLayout8888, 32, 4);
constexpr uint32_t kFormatBGRX8888 = makePixelFormat(kPixelTypePacked32, kOrderBGRX, kPackedLayout8888, 24, 4);

// The values SDL2 headers give these names (SDL_pixels.h).  A mismatch here
// means makePixelFormat does not follow the SDL macro.
static_assert(kFormatARGB8888 == 372645892u, "SDL_PIXELFORMAT_ARGB8888");
static_assert(kFormatXRGB8888 == 370546692u, "SDL_PIXELFORMAT_RGB888");
static_assert(kFormatABGR8888 == 376840196u, "SDL_PIXELFORMAT_ABGR8888");
static_assert(kFormatRGBA8888 == 373694468u, "SDL_PIXELFORMAT_RGBA8888");
static_assert(kFormatBGRA8888 == 377888772u, "SDL_PIXELFORMAT_BGRA8888");

// Channel placement of one 32-bit packed format.
struct FormatInfo {
    uint32_t format;
    uint32_t aMask, rMask, gMask, bMask;   // 0 for a channel the format lacks
    int aShift, rShift, gShift, bShift;
};

constexpr FormatInfo kFormats[] = {
    { kFormatARGB8888, 255u << 24, 255u << 16, 255u << 8, 255u,       24, 16, 8, 0 },
    { kFormatXRGB8888, 0,          255u << 16, 255u << 8, 255u,       0,  16, 8, 0 },
    { kFormatABGR8888, 255u << 24, 255u,       255u << 8, 255u << 16, 24, 0,  8, 16 },
    { kFormatXBGR8888, 0,          255u,       255u << 8, 255u << 16, 0,  0,  8, 16 },
    { kFormatRGBA8888, 255u,       255u << 24, 255u << 16, 255u << 8, 0,  24, 16, 8 },
    { kFormatRGBX8888, 0,          255u << 24, 255u << 16, 255u << 8, 0,  24, 16, 8 },
    { kFormatBGRA8888, 255u,       255u << 8,  255u << 16, 255u << 24, 0, 8,  16, 24 },
    { kFormatBGRX8888, 0,          255u << 8,  255u << 16, 255u << 24, 0, 8,  16, 24 },
};

inline const FormatInfo* formatInfo(uint32_t format) {
    for (const FormatInfo& f : kFormats)
        if (f.format == format) return &f;
    return nullptr;
}

// SDL_MasksToPixelFormatEnum for the 32-bit formats this layer supports.
// Answers 0 (SDL_PIXELFORMAT_UNKNOWN) for any other combination.
inline uint32_t formatFromMasks(int depth, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    if (depth != 32) return 0;
    if (r == 0) return kFormatARGB8888;   // no masks given: SDL chooses ARGB8888
    for (const FormatInfo& f : kFormats)
        if (f.rMask == r && f.gMask == g && f.bMask == b && f.aMask == a) return f.format;
    return 0;
}

inline RGBA unpackPixel(const FormatInfo& f, uint32_t px) {
    RGBA c;
    c.r = static_cast<uint8_t>((px & f.rMask) >> f.rShift);
    c.g = static_cast<uint8_t>((px & f.gMask) >> f.gShift);
    c.b = static_cast<uint8_t>((px & f.bMask) >> f.bShift);
    c.a = f.aMask ? static_cast<uint8_t>((px & f.aMask) >> f.aShift) : 255;
    return c;
}

inline uint32_t packPixel(const FormatInfo& f, RGBA c) {
    uint32_t px = (static_cast<uint32_t>(c.r) << f.rShift) |
                  (static_cast<uint32_t>(c.g) << f.gShift) |
                  (static_cast<uint32_t>(c.b) << f.bShift);
    if (f.aMask) px |= static_cast<uint32_t>(c.a) << f.aShift;
    return px;
}

// A writable or readable 32-bit pixel area.
struct Pixels {
    uint8_t* data = nullptr;
    int w = 0, h = 0, pitch = 0;
    const FormatInfo* fmt = nullptr;
    bool valid() const { return data && fmt && w > 0 && h > 0; }
    uint32_t* at(int x, int y) const {
        return reinterpret_cast<uint32_t*>(data + static_cast<size_t>(y) * pitch + static_cast<size_t>(x) * 4);
    }
    RGBA get(int x, int y) const { return unpackPixel(*fmt, *at(x, y)); }
    void put(int x, int y, RGBA c) const { *at(x, y) = packPixel(*fmt, c); }
};

// ---------------------------------------------------------------------------
// Blend modes (SDL_blendmode.h).
// ---------------------------------------------------------------------------

enum BlendFactor : uint32_t {
    kFactorZero = 1, kFactorOne = 2, kFactorSrcColor = 3, kFactorOneMinusSrcColor = 4,
    kFactorSrcAlpha = 5, kFactorOneMinusSrcAlpha = 6, kFactorDstColor = 7,
    kFactorOneMinusDstColor = 8, kFactorDstAlpha = 9, kFactorOneMinusDstAlpha = 10
};
enum BlendOperation : uint32_t {
    kOpAdd = 1, kOpSubtract = 2, kOpRevSubtract = 3, kOpMinimum = 4, kOpMaximum = 5
};
constexpr uint32_t kBlendNone = 0;
constexpr uint32_t kBlendBlend = 1;
constexpr uint32_t kBlendAdd = 2;
constexpr uint32_t kBlendMod = 4;
constexpr uint32_t kBlendMul = 8;
constexpr uint32_t kBlendInvalid = static_cast<uint32_t>(INT32_MAX);

// SDL_ComposeCustomBlendMode: the six fields share one 32-bit value.
constexpr uint32_t composeBlendMode(uint32_t srcColor, uint32_t dstColor, uint32_t colorOp,
                                    uint32_t srcAlpha, uint32_t dstAlpha, uint32_t alphaOp) {
    return (colorOp << 0) | (srcColor << 4) | (dstColor << 8) |
           (alphaOp << 16) | (srcAlpha << 20) | (dstAlpha << 24);
}

struct BlendSpec {
    uint32_t srcColor, dstColor, colorOp, srcAlpha, dstAlpha, alphaOp;
};

// The six fields of any blend mode; false for a mode this layer rejects.
inline bool decodeBlendMode(uint32_t mode, BlendSpec& s) {
    switch (mode) {
    case kBlendNone:
        s = { kFactorOne, kFactorZero, kOpAdd, kFactorOne, kFactorZero, kOpAdd };
        return true;
    case kBlendBlend:
        s = { kFactorSrcAlpha, kFactorOneMinusSrcAlpha, kOpAdd, kFactorOne, kFactorOneMinusSrcAlpha, kOpAdd };
        return true;
    case kBlendAdd:
        s = { kFactorSrcAlpha, kFactorOne, kOpAdd, kFactorZero, kFactorOne, kOpAdd };
        return true;
    case kBlendMod:
        s = { kFactorZero, kFactorSrcColor, kOpAdd, kFactorZero, kFactorOne, kOpAdd };
        return true;
    case kBlendMul:
        s = { kFactorDstColor, kFactorOneMinusSrcAlpha, kOpAdd, kFactorZero, kFactorOne, kOpAdd };
        return true;
    default:
        break;
    }
    if (mode == kBlendInvalid) return false;
    s.colorOp  = (mode >> 0) & 15u;
    s.srcColor = (mode >> 4) & 15u;
    s.dstColor = (mode >> 8) & 15u;
    s.alphaOp  = (mode >> 16) & 15u;
    s.srcAlpha = (mode >> 20) & 15u;
    s.dstAlpha = (mode >> 24) & 15u;
    auto okFactor = [](uint32_t f) { return f >= kFactorZero && f <= kFactorOneMinusDstAlpha; };
    auto okOp = [](uint32_t o) { return o >= kOpAdd && o <= kOpMaximum; };
    return okFactor(s.srcColor) && okFactor(s.dstColor) && okFactor(s.srcAlpha) &&
           okFactor(s.dstAlpha) && okOp(s.colorOp) && okOp(s.alphaOp);
}

inline bool isValidBlendMode(uint32_t mode) {
    BlendSpec s;
    return decodeBlendMode(mode, s);
}

// One blend factor as an integer 0..255 (the SDL software renderer scale).
// `src` and `dst` hold r, g, b, a; channel 3 is alpha.
inline int blendFactor(uint32_t f, const int* src, const int* dst, int channel) {
    switch (f) {
    case kFactorZero: return 0;
    case kFactorOne: return 255;
    case kFactorSrcColor: return src[channel];
    case kFactorOneMinusSrcColor: return 255 - src[channel];
    case kFactorSrcAlpha: return src[3];
    case kFactorOneMinusSrcAlpha: return 255 - src[3];
    case kFactorDstColor: return dst[channel];
    case kFactorOneMinusDstColor: return 255 - dst[channel];
    case kFactorDstAlpha: return dst[3];
    case kFactorOneMinusDstAlpha: return 255 - dst[3];
    default: return 0;
    }
}

// Both terms are on the 255 x 255 scale; one division by 255 and a clamp bring
// the sum back, with the truncation the SDL software renderer uses.
inline uint8_t blendCombine(uint32_t op, int s, int d) {
    int v;
    switch (op) {
    case kOpSubtract: v = s - d; break;
    case kOpRevSubtract: v = d - s; break;
    case kOpMinimum: v = std::min(s, d); break;
    case kOpMaximum: v = std::max(s, d); break;
    default: v = s + d; break;
    }
    v /= 255;
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

// Combine one source colour with one destination colour under a blend mode.
inline RGBA blendPixel(const BlendSpec& spec, RGBA s, RGBA d, bool fastNone) {
    if (fastNone) return s;
    const int sf[4] = { s.r, s.g, s.b, s.a };
    const int df[4] = { d.r, d.g, d.b, d.a };
    uint8_t out[4];
    for (int c = 0; c < 3; c++) {
        out[c] = blendCombine(spec.colorOp, sf[c] * blendFactor(spec.srcColor, sf, df, c),
                              df[c] * blendFactor(spec.dstColor, sf, df, c));
    }
    out[3] = blendCombine(spec.alphaOp, sf[3] * blendFactor(spec.srcAlpha, sf, df, 3),
                          df[3] * blendFactor(spec.dstAlpha, sf, df, 3));
    return RGBA{ out[0], out[1], out[2], out[3] };
}

// ---------------------------------------------------------------------------
// Drawing state shared by the operations below.
// ---------------------------------------------------------------------------

struct Clip {
    bool enabled = false;
    Rect rect = { 0, 0, 0, 0 };
};

inline bool insideClip(const Pixels& dst, const Clip& clip, int x, int y) {
    if (x < 0 || y < 0 || x >= dst.w || y >= dst.h) return false;
    if (clip.enabled) {
        if (x < clip.rect.x || y < clip.rect.y ||
            x >= clip.rect.x + clip.rect.w || y >= clip.rect.y + clip.rect.h) return false;
    }
    return true;
}

inline void plot(const Pixels& dst, const Clip& clip, int x, int y, RGBA c,
                 const BlendSpec& spec, bool fastNone) {
    if (!insideClip(dst, clip, x, y)) return;
    dst.put(x, y, blendPixel(spec, c, dst.get(x, y), fastNone));
}

// SDL_RenderClear: the draw colour replaces every pixel; blend mode and clip
// rectangle do not apply.
inline void clearAll(const Pixels& dst, RGBA c) {
    const uint32_t px = packPixel(*dst.fmt, c);
    for (int y = 0; y < dst.h; y++) {
        uint32_t* row = dst.at(0, y);
        for (int x = 0; x < dst.w; x++) row[x] = px;
    }
}

inline void fillRect(const Pixels& dst, const Clip& clip, const Rect& r, RGBA c, uint32_t blend) {
    BlendSpec spec;
    if (!decodeBlendMode(blend, spec)) return;
    const bool fast = blend == kBlendNone;
    const int x1 = std::max(r.x, 0), y1 = std::max(r.y, 0);
    const int x2 = std::min(r.x + r.w, dst.w), y2 = std::min(r.y + r.h, dst.h);
    for (int y = y1; y < y2; y++)
        for (int x = x1; x < x2; x++) plot(dst, clip, x, y, c, spec, fast);
}

inline void drawPoint(const Pixels& dst, const Clip& clip, int x, int y, RGBA c, uint32_t blend) {
    BlendSpec spec;
    if (!decodeBlendMode(blend, spec)) return;
    plot(dst, clip, x, y, c, spec, blend == kBlendNone);
}

// Bresenham line, both end points included (SDL_RenderDrawLine).
inline void drawLine(const Pixels& dst, const Clip& clip, int x1, int y1, int x2, int y2,
                     RGBA c, uint32_t blend) {
    BlendSpec spec;
    if (!decodeBlendMode(blend, spec)) return;
    const bool fast = blend == kBlendNone;
    const int dx = std::abs(x2 - x1), dy = -std::abs(y2 - y1);
    const int sx = x1 < x2 ? 1 : -1, sy = y1 < y2 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        plot(dst, clip, x1, y1, c, spec, fast);
        if (x1 == x2 && y1 == y2) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x1 += sx; }
        if (e2 <= dx) { err += dx; y1 += sy; }
    }
}

inline void drawRectOutline(const Pixels& dst, const Clip& clip, const Rect& r, RGBA c, uint32_t blend) {
    if (r.w <= 0 || r.h <= 0) return;
    drawLine(dst, clip, r.x, r.y, r.x + r.w - 1, r.y, c, blend);
    if (r.h > 1) drawLine(dst, clip, r.x, r.y + r.h - 1, r.x + r.w - 1, r.y + r.h - 1, c, blend);
    if (r.h > 2) {
        drawLine(dst, clip, r.x, r.y + 1, r.x, r.y + r.h - 2, c, blend);
        if (r.w > 1) drawLine(dst, clip, r.x + r.w - 1, r.y + 1, r.x + r.w - 1, r.y + r.h - 2, c, blend);
    }
}

// ---------------------------------------------------------------------------
// Texture copy (SDL_RenderCopy / SDL_RenderCopyF).
// ---------------------------------------------------------------------------

struct TextureSampling {
    uint8_t rMod = 255, gMod = 255, bMod = 255, aMod = 255;
    uint32_t blend = kBlendNone;
    bool linear = false;
};

inline RGBA modulate(RGBA c, const TextureSampling& t) {
    auto mul = [](uint8_t a, uint8_t b) { return static_cast<uint8_t>((a * b) / 255); };
    return RGBA{ mul(c.r, t.rMod), mul(c.g, t.gMod), mul(c.b, t.bMod), mul(c.a, t.aMod) };
}

// Nearest or bilinear sample of the source rectangle at source position (u, v)
// in pixels, edges clamped.
inline RGBA samplePixels(const Pixels& src, const Rect& sr, float u, float v, bool linear) {
    auto fetch = [&](int ix, int iy) {
        ix = std::min(std::max(ix, sr.x), sr.x + sr.w - 1);
        iy = std::min(std::max(iy, sr.y), sr.y + sr.h - 1);
        return src.get(ix, iy);
    };
    if (!linear) return fetch(static_cast<int>(std::floor(u)), static_cast<int>(std::floor(v)));
    const float fx = u - 0.5f, fy = v - 0.5f;
    const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
    const float tx = fx - x0, ty = fy - y0;
    const RGBA p00 = fetch(x0, y0), p10 = fetch(x0 + 1, y0);
    const RGBA p01 = fetch(x0, y0 + 1), p11 = fetch(x0 + 1, y0 + 1);
    auto mix = [&](uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
        const float top = a + (b - a) * tx, bottom = c + (d - c) * tx;
        return static_cast<uint8_t>(std::lround(top + (bottom - top) * ty));
    };
    return RGBA{ mix(p00.r, p10.r, p01.r, p11.r), mix(p00.g, p10.g, p01.g, p11.g),
                 mix(p00.b, p10.b, p01.b, p11.b), mix(p00.a, p10.a, p01.a, p11.a) };
}

// Copy `srcRect` of `src` into the destination rectangle (float corners, so
// that RenderCopy and RenderCopyF share one routine).  Answers false when a
// blend mode of the texture is not valid.
inline bool copyTexture(const Pixels& dst, const Clip& clip, const Pixels& src,
                        const Rect& srcRect, float dx, float dy, float dw, float dh,
                        const TextureSampling& t) {
    BlendSpec spec;
    if (!decodeBlendMode(t.blend, spec)) return false;
    if (dw <= 0.f || dh <= 0.f || srcRect.w <= 0 || srcRect.h <= 0) return true;
    const bool fast = t.blend == kBlendNone;
    const int x1 = static_cast<int>(std::lround(dx)), x2 = static_cast<int>(std::lround(dx + dw));
    const int y1 = static_cast<int>(std::lround(dy)), y2 = static_cast<int>(std::lround(dy + dh));
    const float sx = srcRect.w / dw, sy = srcRect.h / dh;
    const bool unscaled = std::fabs(sx - 1.f) < 1e-6f && std::fabs(sy - 1.f) < 1e-6f;
    for (int y = y1; y < y2; y++) {
        for (int x = x1; x < x2; x++) {
            if (!insideClip(dst, clip, x, y)) continue;
            const float u = srcRect.x + (x + 0.5f - dx) * sx;
            const float v = srcRect.y + (y + 0.5f - dy) * sy;
            const RGBA texel = samplePixels(src, srcRect, u, v, t.linear && !unscaled);
            dst.put(x, y, blendPixel(spec, modulate(texel, t), dst.get(x, y), fast));
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Triangle geometry (SDL_RenderGeometry, SDL_RenderGeometryRaw).
// ---------------------------------------------------------------------------

struct Vertex2D { float x, y; RGBA color; float u, v; };

inline void drawTriangle(const Pixels& dst, const Clip& clip, const Vertex2D& a, const Vertex2D& b,
                         const Vertex2D& c, const Pixels* tex, const TextureSampling& t,
                         uint32_t blend) {
    BlendSpec spec;
    if (!decodeBlendMode(blend, spec)) return;
    const bool fast = blend == kBlendNone;
    const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (area == 0.f) return;
    const int minX = std::max(0, static_cast<int>(std::floor(std::min({ a.x, b.x, c.x }))));
    const int maxX = std::min(dst.w - 1, static_cast<int>(std::ceil(std::max({ a.x, b.x, c.x }))));
    const int minY = std::max(0, static_cast<int>(std::floor(std::min({ a.y, b.y, c.y }))));
    const int maxY = std::min(dst.h - 1, static_cast<int>(std::ceil(std::max({ a.y, b.y, c.y }))));
    for (int y = minY; y <= maxY; y++) {
        for (int x = minX; x <= maxX; x++) {
            const float px = x + 0.5f, py = y + 0.5f;
            float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) / area;
            float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) / area;
            float w2 = 1.f - w0 - w1;
            if (w0 < 0.f || w1 < 0.f || w2 < 0.f) continue;
            float col[4] = {
                w0 * a.color.r + w1 * b.color.r + w2 * c.color.r,
                w0 * a.color.g + w1 * b.color.g + w2 * c.color.g,
                w0 * a.color.b + w1 * b.color.b + w2 * c.color.b,
                w0 * a.color.a + w1 * b.color.a + w2 * c.color.a };
            if (tex && tex->valid()) {
                const float u = (w0 * a.u + w1 * b.u + w2 * c.u) * tex->w;
                const float v = (w0 * a.v + w1 * b.v + w2 * c.v) * tex->h;
                const Rect all = { 0, 0, tex->w, tex->h };
                const RGBA tx = modulate(samplePixels(*tex, all, u, v, t.linear), t);
                col[0] = col[0] * tx.r / 255.f;
                col[1] = col[1] * tx.g / 255.f;
                col[2] = col[2] * tx.b / 255.f;
                col[3] = col[3] * tx.a / 255.f;
            }
            // Truncated, like the interpolation of the SDL software renderer.
            RGBA out = { static_cast<uint8_t>(std::min(col[0], 255.f)),
                         static_cast<uint8_t>(std::min(col[1], 255.f)),
                         static_cast<uint8_t>(std::min(col[2], 255.f)),
                         static_cast<uint8_t>(std::min(col[3], 255.f)) };
            plot(dst, clip, x, y, out, spec, fast);
        }
    }
}

// ---------------------------------------------------------------------------
// Read back and convert.
// ---------------------------------------------------------------------------

// Copy the pixels of `src` inside `r` (already clipped to src) into a buffer
// of another 32-bit format.
inline void convertRect(const Pixels& src, const Rect& r, const FormatInfo& dstFmt,
                        void* dstPixels, int dstPitch) {
    for (int y = 0; y < r.h; y++) {
        uint32_t* out = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(dstPixels) + static_cast<size_t>(y) * dstPitch);
        for (int x = 0; x < r.w; x++) out[x] = packPixel(dstFmt, src.get(r.x + x, r.y + y));
    }
}

// ---------------------------------------------------------------------------
// Surfaces.  The two structs follow the SDL 2.0.x layout that the image's
// SDL_Surface and SDL_PixelFormat FFI structures read.
// ---------------------------------------------------------------------------

struct PixelFormatRaw {
    uint32_t format;
    void* palette;
    uint8_t bitsPerPixel, bytesPerPixel, pad[2];
    uint32_t rMask, gMask, bMask, aMask;
    uint8_t rLoss, gLoss, bLoss, aLoss, rShift, gShift, bShift, aShift;
    int refcount;
    PixelFormatRaw* next;
};

struct SurfaceRaw {
    uint32_t flags;
    PixelFormatRaw* format;
    int w, h;
    int pitch;
    void* pixels;
    void* userdata;
    int locked;
    void* lockData;
    Rect clipRect;
    void* map;
    int refcount;
};

static_assert(offsetof(SurfaceRaw, format) == 8, "SDL_Surface.format");
static_assert(offsetof(SurfaceRaw, w) == 16, "SDL_Surface.w");
static_assert(offsetof(SurfaceRaw, pitch) == 24, "SDL_Surface.pitch");
static_assert(offsetof(SurfaceRaw, pixels) == 32, "SDL_Surface.pixels");
static_assert(offsetof(SurfaceRaw, locked) == 48, "SDL_Surface.locked");
static_assert(offsetof(SurfaceRaw, clipRect) == 64, "SDL_Surface.clip_rect");
static_assert(offsetof(SurfaceRaw, refcount) == 88, "SDL_Surface.refcount");
static_assert(offsetof(PixelFormatRaw, rMask) == 20, "SDL_PixelFormat.Rmask");
static_assert(offsetof(PixelFormatRaw, rShift) == 40, "SDL_PixelFormat.Rshift");
static_assert(offsetof(PixelFormatRaw, refcount) == 44, "SDL_PixelFormat.refcount");

constexpr uint32_t kSurfacePreallocated = 1;   // SDL_PREALLOC
constexpr uint32_t kSurfaceSimdAligned = 8;    // SDL_SIMD_ALIGNED: the pixel buffer is 16-byte aligned

// One allocation holds the surface, its pixel format, and (unless the caller
// supplied pixels) the pixel buffer.
struct SurfaceBlock {
    SurfaceRaw surface;
    PixelFormatRaw format;
};

inline std::unordered_set<void*>& surfaceRegistry() {
    static std::unordered_set<void*> registry;
    return registry;
}

inline bool isSurface(const void* s) {
    return s && surfaceRegistry().count(const_cast<void*>(s)) != 0;
}

inline SurfaceRaw* createSurface(void* pixels, int w, int h, int pitch, const FormatInfo& f) {
    auto* block = static_cast<SurfaceBlock*>(calloc(1, sizeof(SurfaceBlock)));
    if (!block) return nullptr;
    if (!pixels) {
        pitch = w * 4;
        pixels = calloc(std::max<size_t>(static_cast<size_t>(h) * static_cast<size_t>(pitch), 1), 1);
        if (!pixels) { free(block); return nullptr; }
    }
    PixelFormatRaw& pf = block->format;
    pf.format = f.format;
    pf.bitsPerPixel = 32;   // the depth of the surface, whether or not the format has alpha
    pf.bytesPerPixel = 4;
    pf.rMask = f.rMask; pf.gMask = f.gMask; pf.bMask = f.bMask; pf.aMask = f.aMask;
    pf.rShift = static_cast<uint8_t>(f.rShift); pf.gShift = static_cast<uint8_t>(f.gShift);
    pf.bShift = static_cast<uint8_t>(f.bShift); pf.aShift = static_cast<uint8_t>(f.aShift);
    pf.aLoss = f.aMask ? 0 : 8;
    pf.refcount = 1;
    SurfaceRaw& s = block->surface;
    s.format = &pf;
    s.w = w; s.h = h; s.pitch = pitch; s.pixels = pixels;
    s.clipRect = { 0, 0, w, h };
    s.refcount = 1;
    surfaceRegistry().insert(&s);
    return &s;
}

// Create a surface with its own zeroed pixel buffer (SDL_CreateRGBSurface*).
inline SurfaceRaw* createOwnedSurface(int w, int h, const FormatInfo& f) {
    SurfaceRaw* s = createSurface(nullptr, w, h, w * 4, f);
    if (s) s->flags |= kSurfaceSimdAligned;   // calloc returns 16-byte aligned memory
    return s;
}

// Wrap pixels the caller owns (SDL_CreateRGBSurfaceFrom).
inline SurfaceRaw* createSurfaceFrom(void* pixels, int w, int h, int pitch, const FormatInfo& f) {
    SurfaceRaw* s = createSurface(pixels, w, h, pitch, f);
    if (s) s->flags |= kSurfacePreallocated;
    return s;
}

// SDL_FreeSurface: drop one reference; free at zero.  A pointer that this
// layer did not create is ignored, as the fake handles of older callers are.
inline void freeSurface(SurfaceRaw* s) {
    if (!isSurface(s)) return;
    if (--s->refcount > 0) return;
    surfaceRegistry().erase(s);
    if (!(s->flags & kSurfacePreallocated)) free(s->pixels);
    free(s);   // the block starts at the surface
}

inline Pixels surfacePixels(const SurfaceRaw* s) {
    Pixels p;
    if (!s || !s->format) return p;
    p.data = static_cast<uint8_t*>(s->pixels);
    p.w = s->w; p.h = s->h; p.pitch = s->pitch;
    p.fmt = formatInfo(s->format->format);
    return p;
}

} // namespace sdl2
} // namespace pharo
