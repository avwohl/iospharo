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
#include <mutex>
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
    if (r == 0) return kFormatXRGB8888;   // no masks given: SDL chooses RGB888
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

// ---------------------------------------------------------------------------
// General pixel description (SDL_PixelFormat, SDL_Palette) and access for any
// packed or indexed format of 1, 4, 8, 16, 24 and 32 bits per pixel.
// ---------------------------------------------------------------------------

struct Color { uint8_t r, g, b, a; };

struct PaletteRaw {                 // SDL_Palette
    int ncolors;
    Color* colors;
    uint32_t version;
    int refcount;
};

struct PixelFormatRaw {
    uint32_t format;
    void* palette;
    uint8_t bitsPerPixel, bytesPerPixel, pad[2];
    uint32_t rMask, gMask, bMask, aMask;
    uint8_t rLoss, gLoss, bLoss, aLoss, rShift, gShift, bShift, aShift;
    int refcount;
    PixelFormatRaw* next;
};

constexpr int kBitsPerByte = 8;
constexpr uint32_t kBitmapOrder4321 = 1;   // INDEX1LSB and INDEX4LSB: the first pixel is in the low bits
constexpr uint32_t kPixelOrderShift = 20, kPixelOrderMask = 0x0F;

// A channel with fewer than 8 bits expands to 8 bits by scaling the range.
inline uint8_t expandChannel(uint32_t value, int loss) {
    if (loss == 0) return static_cast<uint8_t>(value);
    if (loss >= kBitsPerByte) return 255;   // a channel with no bits reads as full
    const uint32_t max = (1u << (kBitsPerByte - loss)) - 1;
    return static_cast<uint8_t>(value * 255 / max);   // the value scaled to 0..255, truncated
}

// A channel of more than 8 bits (ARGB2101010) has a loss below zero, which the
// 8-bit field of SDL_PixelFormat holds as a wrapped value.
constexpr int kWideChannelFirstLoss = 128;
inline bool isWideChannel(uint8_t loss) { return loss >= kWideChannelFirstLoss; }
inline int channelWidth(uint8_t loss) { return kBitsPerByte - static_cast<int8_t>(loss); }

// The 8-bit value of the channel bits `value` of a channel with the given loss.
inline uint8_t channelTo8(uint32_t value, uint8_t loss) {
    if (isWideChannel(loss)) return static_cast<uint8_t>(value >> (channelWidth(loss) - kBitsPerByte));
    return expandChannel(value, loss);
}

// The channel bits for an 8-bit value: fewer bits drop the low bits, more bits
// repeat the high bits into the low bits.
inline uint32_t channelFrom8(uint8_t c, uint8_t loss) {
    if (isWideChannel(loss)) {
        const int width = channelWidth(loss);
        return (static_cast<uint32_t>(c) << (width - kBitsPerByte)) | (static_cast<uint32_t>(c) >> (2 * kBitsPerByte - width));
    }
    return static_cast<uint32_t>(c >> loss);
}

// The index of the palette colour nearest to (r, g, b, a).
inline uint8_t findColor(const PaletteRaw* pal, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    uint32_t smallest = ~0u;
    uint8_t best = 0;
    for (int i = 0; i < pal->ncolors; i++) {
        const int rd = static_cast<int>(pal->colors[i].r) - r, gd = static_cast<int>(pal->colors[i].g) - g;
        const int bd = static_cast<int>(pal->colors[i].b) - b, ad = static_cast<int>(pal->colors[i].a) - a;
        const uint32_t d = static_cast<uint32_t>(rd * rd + gd * gd + bd * bd + ad * ad);
        if (d < smallest) { smallest = d; best = static_cast<uint8_t>(i); if (d == 0) break; }
    }
    return best;
}

inline RGBA unpackGeneric(const PixelFormatRaw& f, uint32_t px) {
    if (f.palette) {
        const PaletteRaw* pal = static_cast<const PaletteRaw*>(f.palette);
        const Color c = px < static_cast<uint32_t>(pal->ncolors) ? pal->colors[px] : Color{ 255, 255, 255, 255 };
        return RGBA{ c.r, c.g, c.b, c.a };
    }
    RGBA c;
    c.r = channelTo8((px & f.rMask) >> f.rShift, f.rLoss);
    c.g = channelTo8((px & f.gMask) >> f.gShift, f.gLoss);
    c.b = channelTo8((px & f.bMask) >> f.bShift, f.bLoss);
    c.a = f.aMask ? channelTo8((px & f.aMask) >> f.aShift, f.aLoss) : 255;
    return c;
}

inline uint32_t packGeneric(const PixelFormatRaw& f, RGBA c) {
    if (f.palette) return findColor(static_cast<const PaletteRaw*>(f.palette), c.r, c.g, c.b, c.a);
    return (channelFrom8(c.r, f.rLoss) << f.rShift) | (channelFrom8(c.g, f.gLoss) << f.gShift) |
           (channelFrom8(c.b, f.bLoss) << f.bShift) | ((channelFrom8(c.a, f.aLoss) << f.aShift) & f.aMask);
}

// A writable or readable pixel area.  `fmt` is set for a 32-bit packed format
// without palette (the fast path of textures); `pf` describes any other format.
struct Pixels {
    uint8_t* data = nullptr;
    int w = 0, h = 0, pitch = 0;
    const FormatInfo* fmt = nullptr;
    const PixelFormatRaw* pf = nullptr;
    bool valid() const { return data && (fmt || pf) && w > 0 && h > 0; }
    int bitsPerPixel() const { return fmt ? 32 : pf->bitsPerPixel; }
    int bytes() const { return fmt ? 4 : pf->bytesPerPixel; }
    bool hasAlpha() const { return fmt ? fmt->aMask != 0 : pf->aMask != 0; }
    uint32_t aMask() const { return fmt ? fmt->aMask : pf->aMask; }
    bool subByte() const { return !fmt && pf->bitsPerPixel < kBitsPerByte; }
    uint32_t* at(int x, int y) const {   // 32-bit formats only
        return reinterpret_cast<uint32_t*>(data + static_cast<size_t>(y) * pitch + static_cast<size_t>(x) * 4);
    }
    // The pixel as stored, in the low bits of the answer.
    uint32_t raw(int x, int y) const {
        const uint8_t* row = data + static_cast<size_t>(y) * pitch;
        if (fmt) return *at(x, y);
        const int bits = pf->bitsPerPixel;
        if (bits < kBitsPerByte) {
            const int perByte = kBitsPerByte / bits;
            const uint32_t mask = (1u << bits) - 1;
            const uint32_t slot = static_cast<uint32_t>(x % perByte);
            const uint32_t order = (pf->format >> kPixelOrderShift) & kPixelOrderMask;
            const uint32_t shift = (order == kBitmapOrder4321 ? slot : static_cast<uint32_t>(perByte - 1) - slot) * bits;
            return (row[x / perByte] >> shift) & mask;
        }
        uint32_t v = 0;
        const uint8_t* q = row + static_cast<size_t>(x) * pf->bytesPerPixel;
        for (int i = 0; i < pf->bytesPerPixel; i++) v |= static_cast<uint32_t>(q[i]) << (kBitsPerByte * i);
        return v;
    }
    void putRaw(int x, int y, uint32_t v) const {
        uint8_t* row = data + static_cast<size_t>(y) * pitch;
        if (fmt) { *at(x, y) = v; return; }
        const int bits = pf->bitsPerPixel;
        if (bits < kBitsPerByte) {
            const int perByte = kBitsPerByte / bits;
            const uint32_t mask = (1u << bits) - 1;
            const uint32_t slot = static_cast<uint32_t>(x % perByte);
            const uint32_t order = (pf->format >> kPixelOrderShift) & kPixelOrderMask;
            const uint32_t shift = (order == kBitmapOrder4321 ? slot : static_cast<uint32_t>(perByte - 1) - slot) * bits;
            uint8_t& byte = row[x / perByte];
            byte = static_cast<uint8_t>((byte & ~(mask << shift)) | ((v & mask) << shift));
            return;
        }
        uint8_t* q = row + static_cast<size_t>(x) * pf->bytesPerPixel;
        for (int i = 0; i < pf->bytesPerPixel; i++) q[i] = static_cast<uint8_t>(v >> (kBitsPerByte * i));
    }
    RGBA unpack(uint32_t rawValue) const { return fmt ? unpackPixel(*fmt, rawValue) : unpackGeneric(*pf, rawValue); }
    uint32_t pack(RGBA c) const { return fmt ? packPixel(*fmt, c) : packGeneric(*pf, c); }
    RGBA get(int x, int y) const { return unpack(raw(x, y)); }
    void put(int x, int y, RGBA c) const { putRaw(x, y, pack(c)); }
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
    const uint32_t px = dst.pack(c);
    for (int y = 0; y < dst.h; y++)
        for (int x = 0; x < dst.w; x++) dst.putRaw(x, y, px);
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
    bool modulated() const { return rMod != 255 || gMod != 255 || bMod != 255 || aMod != 255; }
};

// SDL_BLENDMODE_BLEND for a texture copy (a surface blit).  The software
// blitter has two code paths and they round differently.  A picture with
// per-pixel alpha and no colour or alpha modulation takes the fast path: each
// term is multiplied and shifted right by 8.  A modulated copy takes the
// general path: each term is multiplied and divided by 255.  A pixel of alpha 0
// is skipped and a pixel of alpha 255 is copied on both.  `s` is the source
// pixel after the modulation.  The result alpha is the source alpha plus the
// destination alpha that the source leaves.
inline RGBA blitBlendPixel(RGBA s, RGBA d, bool modulated) {
    const int A = s.a;
    if (A == 0) return d;
    if (A == 255) return s;
    const int B = 255 - A;
    auto mix = [&](int sc, int dc) {
        return static_cast<uint8_t>(modulated ? (sc * A) / 255 + (dc * B) / 255 : ((sc * A) >> 8) + ((dc * B) >> 8));
    };
    const int alpha = modulated ? A + (d.a * B) / 255 : A + ((d.a * B) >> 8);
    return RGBA{ mix(s.r, d.r), mix(s.g, d.g), mix(s.b, d.b), static_cast<uint8_t>(alpha) };
}

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

// The source index that a nearest-neighbour scale of `srcLen` pixels to `dstLen`
// pixels reads for destination index `i`.  This is the fixed point walk of the
// SDL software scaler: a 16.16 step, started half a step in, truncated.
inline int nearestSourceIndex(int i, int srcLen, int dstLen) {
    const uint32_t inc = (static_cast<uint32_t>(srcLen) << 16) / static_cast<uint32_t>(dstLen);
    const uint32_t pos = inc / 2 + static_cast<uint32_t>(i) * inc;
    return static_cast<int>(pos >> 16);
}

// Bilinear scale of `srcRect` to `dw` x `dh` pixels, in the fixed point of the
// SDL software scaler: 16.16 positions, 7 bit weights, a source of pixel
// centres, and edge pixels that repeat the border (the left and right "pad"
// pixels of each axis).
constexpr int kFixedShift = 16;                 // 16.16 fixed point
constexpr int kFixedFraction = 0xFFFF;          // its fractional bits
constexpr int64_t kFixedHalf = 0x8000;          // one half in that scale

struct ScalerAxis { int start, step, leftPad, rightPad; };

inline ScalerAxis scalerAxis(int srcNb, int dstNb) {
    ScalerAxis a;
    const int step = static_cast<int>((static_cast<uint32_t>(srcNb) << 16) / static_cast<uint32_t>(dstNb));
    int x0 = 1 << 15;
    const int64_t hi = static_cast<int64_t>(step) * (x0 >> 16);
    const int64_t lo = static_cast<int64_t>(step) * (x0 & kFixedFraction);
    x0 = static_cast<int>(hi + ((lo + kFixedHalf) >> kFixedShift));
    x0 -= 1 << 15;
    a.start = x0;
    a.step = step;
    a.leftPad = a.rightPad = 0;
    int sum = x0;
    for (int i = 0; i < dstNb; i++) {
        if (sum < 0) a.leftPad++;
        else if (static_cast<int>(static_cast<uint32_t>(sum) >> 16) > srcNb - 2) a.rightPad++;
        sum += step;
    }
    return a;
}

inline std::vector<RGBA> bilinearScale(const Pixels& src, const Rect& sr, int dw, int dh) {
    constexpr int kPrecision = 7, kFracOne = 1 << kPrecision;
    std::vector<RGBA> out(static_cast<size_t>(dw) * dh);
    if (sr.w < 2 || sr.h < 2) {   // a source of one pixel in a direction: repeat it
        for (int y = 0; y < dh; y++)
            for (int x = 0; x < dw; x++)
                out[static_cast<size_t>(y) * dw + x] = src.get(sr.x + std::min(x * sr.w / dw, sr.w - 1), sr.y + std::min(y * sr.h / dh, sr.h - 1));
        return out;
    }
    const ScalerAxis ah = scalerAxis(sr.h, dh), aw = scalerAxis(sr.w, dw);
    auto mix = [&](RGBA c0, RGBA c1, int f0, int f1) {
        return RGBA{ static_cast<uint8_t>((f1 * c0.r + f0 * c1.r) >> kPrecision), static_cast<uint8_t>((f1 * c0.g + f0 * c1.g) >> kPrecision),
                     static_cast<uint8_t>((f1 * c0.b + f0 * c1.b) >> kPrecision), static_cast<uint8_t>((f1 * c0.a + f0 * c1.a) >> kPrecision) };
    };
    auto texel = [&](int x, int y) { return src.get(sr.x + x, sr.y + y); };
    int sumH = ah.start;
    for (int i = 0; i < dh; i++) {
        const bool noPad = !(i < ah.leftPad || i > dh - 1 - ah.rightPad);
        int row = noPad ? static_cast<int>(static_cast<uint32_t>(sumH) >> 16) : (i < ah.leftPad ? 0 : sr.h - 1);
        const int fh0 = noPad ? static_cast<int>((static_cast<uint32_t>(sumH) >> (16 - kPrecision)) & (kFracOne - 1)) : 0;
        const int row1 = noPad ? row + 1 : row;
        sumH += ah.step;
        const int fh1 = kFracOne - fh0;
        int sumW = aw.start + aw.leftPad * aw.step;
        auto bilinear = [&](int x, int fw0) {
            const int fw1 = kFracOne - fw0;
            const RGBA left = mix(texel(x, row), texel(x, row1), fh0, fh1);
            const RGBA right = mix(texel(x + 1, row), texel(x + 1, row1), fh0, fh1);
            return mix(left, right, fw0, fw1);
        };
        int o = 0;
        for (int k = 0; k < aw.leftPad; k++) out[static_cast<size_t>(i) * dw + o++] = bilinear(0, 0);
        const int middle = dw - aw.leftPad - aw.rightPad;
        for (int k = 0; k < middle; k++) {
            const int x = static_cast<int>(static_cast<uint32_t>(sumW) >> 16);
            const int fw = static_cast<int>((static_cast<uint32_t>(sumW) >> (16 - kPrecision)) & (kFracOne - 1));
            sumW += aw.step;
            out[static_cast<size_t>(i) * dw + o++] = bilinear(x, fw);
        }
        for (int k = 0; k < aw.rightPad; k++) out[static_cast<size_t>(i) * dw + o++] = bilinear(sr.w - 2, kFracOne);
    }
    return out;
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
    std::vector<RGBA> linearPixels;
    if (t.linear && !unscaled && x2 > x1 && y2 > y1) linearPixels = bilinearScale(src, srcRect, x2 - x1, y2 - y1);
    for (int y = y1; y < y2; y++) {
        for (int x = x1; x < x2; x++) {
            if (!insideClip(dst, clip, x, y)) continue;
            RGBA texel;
            if (t.linear && !unscaled) {
                texel = linearPixels[static_cast<size_t>(y - y1) * (x2 - x1) + (x - x1)];
            } else {
                const int sxi = std::min(nearestSourceIndex(x - x1, srcRect.w, x2 - x1), srcRect.w - 1);
                const int syi = std::min(nearestSourceIndex(y - y1, srcRect.h, y2 - y1), srcRect.h - 1);
                texel = src.get(srcRect.x + sxi, srcRect.y + syi);
            }
            const RGBA m = modulate(texel, t);
            dst.put(x, y, t.blend == kBlendBlend ? blitBlendPixel(m, dst.get(x, y), t.modulated())
                                                  : blendPixel(spec, m, dst.get(x, y), fast));
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

// Copy the pixels of `src` inside `r` (already clipped to src) into `dst`,
// which starts at its own origin.  Each pixel goes through 8-bit RGBA.
inline void convertRect(const Pixels& src, const Rect& r, const Pixels& dst) {
    for (int y = 0; y < r.h; y++)
        for (int x = 0; x < r.w; x++) dst.put(x, y, src.get(r.x + x, r.y + y));
}

// ---------------------------------------------------------------------------
// Surfaces.  The two structs follow the SDL 2.0.x layout that the image's
// SDL_Surface and SDL_PixelFormat FFI structures read.
// ---------------------------------------------------------------------------

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

struct SurfaceRegistry {
    std::mutex mx;
    std::unordered_set<void*> set;
};
inline SurfaceRegistry& surfaceRegistry() {
    static SurfaceRegistry* registry = new SurfaceRegistry;
    return *registry;
}

inline bool isSurface(const void* s) {
    if (!s) return false;
    SurfaceRegistry& r = surfaceRegistry();
    std::lock_guard<std::mutex> lk(r.mx);
    return r.set.count(const_cast<void*>(s)) != 0;
}

// The shift and the loss (8 minus the width) of one channel mask.
inline void initChannel(uint32_t mask, uint8_t& loss, uint8_t& shift) {
    shift = 0;
    loss = kBitsPerByte;
    if (!mask) return;
    for (; !(mask & 1); mask >>= 1) ++shift;
    for (; (mask & 1); mask >>= 1) --loss;
}

// A pixel format description for one of the 32-bit packed formats of kFormats.
inline PixelFormatRaw formatRawFromInfo(const FormatInfo& f) {
    PixelFormatRaw pf = {};
    pf.format = f.format;
    pf.bitsPerPixel = 32;   // the depth of the surface, whether or not the format has alpha
    pf.bytesPerPixel = 4;
    pf.rMask = f.rMask; pf.gMask = f.gMask; pf.bMask = f.bMask; pf.aMask = f.aMask;
    initChannel(f.rMask, pf.rLoss, pf.rShift);
    initChannel(f.gMask, pf.gLoss, pf.gShift);
    initChannel(f.bMask, pf.bLoss, pf.bShift);
    initChannel(f.aMask, pf.aLoss, pf.aShift);
    return pf;
}

inline void releasePalette(PaletteRaw* p) {
    if (!p || --p->refcount > 0) return;
    free(p->colors);
    free(p);
}

constexpr uint32_t kPixelTypeShift = 24, kPixelTypeMask = 0x0F;
constexpr uint32_t kPixelTypeIndex1 = 1, kPixelTypeIndex8 = 3;
constexpr uint32_t kRowAlignmentMask = 3;   // surface rows start on 4-byte boundaries

inline bool isIndexedFormat(uint32_t format) {
    const uint32_t type = (format >> kPixelTypeShift) & kPixelTypeMask;
    return type >= kPixelTypeIndex1 && type <= kPixelTypeIndex8;
}

// SDL_CalculatePitch with 4-byte row alignment; the 64-bit answer is checked by the caller.
inline uint64_t surfacePitch(const PixelFormatRaw& pf, int w) {
    uint64_t pitch = pf.bitsPerPixel >= kBitsPerByte ? static_cast<uint64_t>(w) * pf.bytesPerPixel
                                                      : (static_cast<uint64_t>(w) * pf.bitsPerPixel + (kBitsPerByte - 1)) / kBitsPerByte;
    return (pitch + kRowAlignmentMask) & ~static_cast<uint64_t>(kRowAlignmentMask);
}

// Build a surface for the format `proto`.  An indexed format gets a palette of
// 2^bits entries (opaque white; the 1-bit palette is white and black), as SDL
// does.  With `pixels` null the surface owns a zeroed pixel buffer and the
// pitch is computed; a zero-sized surface has no pixel buffer.
inline SurfaceRaw* createSurface(void* pixels, int w, int h, int pitch, const PixelFormatRaw& proto) {
    auto* block = static_cast<SurfaceBlock*>(calloc(1, sizeof(SurfaceBlock)));
    if (!block) return nullptr;
    PixelFormatRaw& pf = block->format;
    pf = proto;
    pf.palette = nullptr;
    pf.next = nullptr;
    pf.refcount = 1;
    if (isIndexedFormat(pf.format)) {
        const int entries = 1 << pf.bitsPerPixel;
        auto* pal = static_cast<PaletteRaw*>(calloc(1, sizeof(PaletteRaw)));
        Color* colors = static_cast<Color*>(malloc(static_cast<size_t>(entries) * sizeof(Color)));
        if (!pal || !colors) { free(pal); free(colors); free(block); return nullptr; }
        memset(colors, 255, static_cast<size_t>(entries) * sizeof(Color));
        if (entries == 2) colors[1] = Color{ 0, 0, 0, 255 };   // a bitmap palette: white, then black
        pal->ncolors = entries; pal->colors = colors; pal->version = 1; pal->refcount = 1;
        pf.palette = pal;
    }
    bool owned = false;
    if (!pixels) {
        const uint64_t rowBytes = surfacePitch(pf, w);
        if (rowBytes > static_cast<uint64_t>(INT32_MAX)) { releasePalette(static_cast<PaletteRaw*>(pf.palette)); free(block); return nullptr; }
        pitch = static_cast<int>(rowBytes);
        pixels = calloc(std::max<size_t>(static_cast<size_t>(h) * static_cast<size_t>(pitch), 1), 1);
        if (!pixels) { releasePalette(static_cast<PaletteRaw*>(pf.palette)); free(block); return nullptr; }
        owned = true;
    }
    SurfaceRaw& s = block->surface;
    s.format = &pf;
    s.w = w; s.h = h; s.pitch = pitch; s.pixels = pixels;
    s.clipRect = { 0, 0, w, h };
    s.refcount = 1;
    if (owned) s.flags |= kSurfaceSimdAligned;   // calloc returns 16-byte aligned memory
    {
        SurfaceRegistry& r = surfaceRegistry();
        std::lock_guard<std::mutex> lk(r.mx);
        r.set.insert(&s);
    }
    return &s;
}

inline SurfaceRaw* createSurface(void* pixels, int w, int h, int pitch, const FormatInfo& f) {
    return createSurface(pixels, w, h, pitch, formatRawFromInfo(f));
}

// Create a surface with its own zeroed pixel buffer (SDL_CreateRGBSurface*).
inline SurfaceRaw* createOwnedSurface(int w, int h, const PixelFormatRaw& proto) {
    return createSurface(nullptr, w, h, 0, proto);
}
inline SurfaceRaw* createOwnedSurface(int w, int h, const FormatInfo& f) {
    return createSurface(nullptr, w, h, 0, formatRawFromInfo(f));
}

// Wrap pixels the caller owns (SDL_CreateRGBSurfaceFrom).
inline SurfaceRaw* createSurfaceFrom(void* pixels, int w, int h, int pitch, const PixelFormatRaw& proto) {
    SurfaceRaw* s = createSurface(pixels, w, h, pitch, proto);
    if (s) s->flags |= kSurfacePreallocated;
    return s;
}
inline SurfaceRaw* createSurfaceFrom(void* pixels, int w, int h, int pitch, const FormatInfo& f) {
    return createSurfaceFrom(pixels, w, h, pitch, formatRawFromInfo(f));
}

// Code that keeps data per surface (SDL2Surface.hpp) sets this to learn when a
// surface goes away.
inline void (*&surfaceFreeHook())(const SurfaceRaw*) {
    static void (*hook)(const SurfaceRaw*) = nullptr;
    return hook;
}

// SDL_FreeSurface: drop one reference; free at zero.  A pointer that this
// layer did not create is ignored, as the fake handles of older callers are.
inline void freeSurface(SurfaceRaw* s) {
    if (!isSurface(s)) return;
    if (--s->refcount > 0) return;
    {
        SurfaceRegistry& r = surfaceRegistry();
        std::lock_guard<std::mutex> lk(r.mx);
        r.set.erase(s);
    }
    if (surfaceFreeHook()) surfaceFreeHook()(s);
    if (!(s->flags & kSurfacePreallocated)) free(s->pixels);
    releasePalette(static_cast<PaletteRaw*>(s->format->palette));
    free(s);   // the block starts at the surface
}

inline Pixels surfacePixels(const SurfaceRaw* s) {
    Pixels p;
    if (!s || !s->format) return p;
    p.data = static_cast<uint8_t*>(s->pixels);
    p.w = s->w; p.h = s->h; p.pitch = s->pitch;
    p.pf = s->format;
    // Surfaces of a 32-bit packed format without palette take the fast path.
    if (s->format->bytesPerPixel == 4 && !s->format->palette) p.fmt = formatInfo(s->format->format);
    return p;
}

} // namespace sdl2
} // namespace pharo
