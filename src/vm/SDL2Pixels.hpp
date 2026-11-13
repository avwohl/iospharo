/*
 * SDL2Pixels.hpp - pixel format, palette and colour mapping calls of
 * SDL_pixels.h for every SDL pixel format (packed, array and indexed).
 *
 * The surfaces and textures of the stub layer hold only the 32-bit packed
 * formats (SDL2Raster.hpp); the functions here work for any format, because
 * the image can ask about a 16-bit or an indexed format without making a
 * surface of it.  Written from the SDL 2.x documentation.
 *
 * Copyright (c) 2025-2026 Aaron Wohl. Licensed under the MIT License.
 */
#pragma once

#include "SDL2Raster.hpp"
#include "SDL2Sys.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace pharo {
namespace sdl2 {
namespace pix {

#include "SDL2PixelFormats.inc"

enum PixelType : uint32_t {
    kTypeUnknown = 0, kTypeIndex1 = 1, kTypeIndex4 = 2, kTypeIndex8 = 3, kTypePacked8 = 4,
    kTypePacked16 = 5, kTypePacked32 = 6, kTypeArrayU8 = 7, kTypeArrayU16 = 8, kTypeArrayU32 = 9,
    kTypeArrayF16 = 10, kTypeArrayF32 = 11
};
enum PackedLayout : uint32_t {
    kLayoutNone = 0, kLayout332 = 1, kLayout4444 = 2, kLayout1555 = 3, kLayout5551 = 4, kLayout565 = 5,
    kLayout8888 = 6, kLayout2101010 = 7, kLayout1010102 = 8
};
// The order field of a packed format.
enum PackedOrder : uint32_t {
    kPackedNone = 0, kPackedXRGB = 1, kPackedRGBX = 2, kPackedARGB = 3, kPackedRGBA = 4,
    kPackedXBGR = 5, kPackedBGRX = 6, kPackedABGR = 7, kPackedBGRA = 8
};

// Field layout of a packed SDL pixel format value (SDL_DEFINE_PIXELFORMAT).
constexpr uint32_t kFormatNibbleMask = 0x0Fu;
constexpr uint32_t kFormatByteMask = 0xFFu;
constexpr uint32_t kFormatFlagShift = 28, kFormatTypeShift = 24, kFormatOrderShift = 20,
                   kFormatLayoutShift = 16, kFormatBitsShift = 8;
constexpr uint32_t pixelType(uint32_t f) { return (f >> kFormatTypeShift) & kFormatNibbleMask; }
constexpr uint32_t pixelOrder(uint32_t f) { return (f >> kFormatOrderShift) & kFormatNibbleMask; }
constexpr uint32_t pixelLayout(uint32_t f) { return (f >> kFormatLayoutShift) & kFormatNibbleMask; }
constexpr uint32_t bitsPerPixel(uint32_t f) { return (f >> kFormatBitsShift) & kFormatByteMask; }
constexpr uint32_t flagFourCC = 1;

// SDL_ISPIXELFORMAT_FOURCC: a format that is not a flagged enumeration.
constexpr bool isFourCCValue(uint32_t f) { return f != 0 && ((f >> kFormatFlagShift) & kFormatNibbleMask) != flagFourCC; }
constexpr uint32_t bytesPerPixel(uint32_t f) { return isFourCCValue(f) ? 0u : (f & kFormatByteMask); }
constexpr bool isIndexed(uint32_t f) { return !isFourCCValue(f) && (pixelType(f) == kTypeIndex1 || pixelType(f) == kTypeIndex4 || pixelType(f) == kTypeIndex8); }

// The channel masks of each packed layout, for the first, second, third and fourth
// channel of the layout; the pixel order says which colour each one is.
struct LayoutMasks { uint32_t m[4]; };
constexpr LayoutMasks kMasks332 = { { 0x00000000u, 0x000000E0u, 0x0000001Cu, 0x00000003u } };
constexpr LayoutMasks kMasks4444 = { { 0x0000F000u, 0x00000F00u, 0x000000F0u, 0x0000000Fu } };
constexpr LayoutMasks kMasks1555 = { { 0x00008000u, 0x00007C00u, 0x000003E0u, 0x0000001Fu } };
constexpr LayoutMasks kMasks5551 = { { 0x0000F800u, 0x000007C0u, 0x0000003Eu, 0x00000001u } };
constexpr LayoutMasks kMasks565 = { { 0x00000000u, 0x0000F800u, 0x000007E0u, 0x0000001Fu } };
constexpr LayoutMasks kMasks8888 = { { 0xFF000000u, 0x00FF0000u, 0x0000FF00u, 0x000000FFu } };
constexpr LayoutMasks kMasks2101010 = { { 0xC0000000u, 0x3FF00000u, 0x000FFC00u, 0x000003FFu } };
constexpr LayoutMasks kMasks1010102 = { { 0xFFC00000u, 0x003FF000u, 0x00000FFCu, 0x00000003u } };
constexpr uint32_t kByteLow = 0x000000FFu, kByteMid = 0x0000FF00u, kByteHigh = 0x00FF0000u;

inline const char* formatName(uint32_t format) {
    for (const PixelFormatName& n : kPixelFormatNames)
        if (n.value == format) return n.name;
    return "SDL_PIXELFORMAT_UNKNOWN";
}

// SDL_PixelFormatEnumToMasks: depth and channel masks; false for a format that
// has no masks to give.
inline int enumToMasks(uint32_t f, int* bpp, uint32_t* rm, uint32_t* gm, uint32_t* bm, uint32_t* am) {
    if (isFourCCValue(f)) { sys::setError("FOURCC pixel formats are not supported"); return 0; }
    const uint32_t bytes = f & kFormatByteMask;
    *bpp = bytes <= 2 ? static_cast<int>(bitsPerPixel(f)) : static_cast<int>(bytes * 8);
    *rm = *gm = *bm = *am = 0;
    constexpr uint32_t kRgb24 = 386930691u, kBgr24 = 390076419u;   // SDL_PIXELFORMAT_RGB24, BGR24
    if (f == kRgb24) { *rm = kByteLow; *gm = kByteMid; *bm = kByteHigh; return 1; }
    if (f == kBgr24) { *rm = kByteHigh; *gm = kByteMid; *bm = kByteLow; return 1; }
    if (pixelType(f) != kTypePacked8 && pixelType(f) != kTypePacked16 && pixelType(f) != kTypePacked32) return 1;
    const LayoutMasks* lm = nullptr;
    switch (pixelLayout(f)) {
    case kLayout332: lm = &kMasks332; break;
    case kLayout4444: lm = &kMasks4444; break;
    case kLayout1555: lm = &kMasks1555; break;
    case kLayout5551: lm = &kMasks5551; break;
    case kLayout565: lm = &kMasks565; break;
    case kLayout8888: lm = &kMasks8888; break;
    case kLayout2101010: lm = &kMasks2101010; break;
    case kLayout1010102: lm = &kMasks1010102; break;
    default: sys::setError("Unknown pixel format"); return 0;
    }
    const uint32_t* m = lm->m;
    switch (pixelOrder(f)) {
    case kPackedXRGB: *rm = m[1]; *gm = m[2]; *bm = m[3]; break;
    case kPackedRGBX: *rm = m[0]; *gm = m[1]; *bm = m[2]; break;
    case kPackedARGB: *am = m[0]; *rm = m[1]; *gm = m[2]; *bm = m[3]; break;
    case kPackedRGBA: *rm = m[0]; *gm = m[1]; *bm = m[2]; *am = m[3]; break;
    case kPackedXBGR: *bm = m[1]; *gm = m[2]; *rm = m[3]; break;
    case kPackedBGRX: *bm = m[0]; *gm = m[1]; *rm = m[2]; break;
    case kPackedBGRA: *bm = m[0]; *gm = m[1]; *rm = m[2]; *am = m[3]; break;
    case kPackedABGR: *am = m[0]; *bm = m[1]; *gm = m[2]; *rm = m[3]; break;
    default: sys::setError("Unknown pixel format"); return 0;
    }
    return 1;
}

// SDL_MasksToPixelFormatEnum: the format whose depth and masks match; 0
// (SDL_PIXELFORMAT_UNKNOWN) when none does.  Masks of zero ask for the
// default format of the depth.
inline uint32_t masksToEnum(int bpp, uint32_t rm, uint32_t gm, uint32_t bm, uint32_t am) {
    constexpr uint32_t kIndex1Msb = 287310080u, kIndex4Msb = 304087040u, kIndex8 = 318769153u;
    constexpr uint32_t kRgb332 = 336660481u, kRgb555 = 353570562u, kRgb565 = 353701890u, kRgb24 = 386930691u, kRgb888 = 370546692u;
    if (bpp == 1) return kIndex1Msb;
    if (bpp == 4) return kIndex4Msb;
    if (bpp == 8 && rm == 0) return kIndex8;
    if (rm == 0 && gm == 0 && bm == 0 && am == 0) {
        switch (bpp) {
        case 8: return kRgb332;
        case 15: return kRgb555;
        case 16: return kRgb565;
        case 24: return kRgb24;
        case 32: return kRgb888;
        default: return 0;
        }
    }
    for (const PixelFormatName& n : kPixelFormatNames) {
        int b;
        uint32_t r, g, bl, a;
        if (isFourCCValue(n.value) || n.value == 0 || isIndexed(n.value)) continue;
        if (!enumToMasks(n.value, &b, &r, &g, &bl, &a)) continue;
        // Depth 15 and 12 are the 16-bit formats with an unused top bit.
        const int want = (b == 16 && a == 0 && (n.value == kRgb555 || pixelLayout(n.value) == kLayout1555)) ? 15 : b;
        if ((want == bpp || b == bpp) && r == rm && g == gm && bl == bm && a == am) return n.value;
    }
    return 0;
}

// ----- Palettes and pixel formats -----

using sdl2::Color;
using sdl2::PaletteRaw;

constexpr uint8_t kOpaqueWhiteByte = 255;

struct FormatList {
    std::mutex mx;
    PixelFormatRaw* head = nullptr;
};
inline FormatList& formatList() {
    static FormatList* list = new FormatList;
    return *list;
}

inline PaletteRaw* allocPalette(int ncolors) {
    if (ncolors < 1) { sys::invalidParam("ncolors"); return nullptr; }
    auto* p = static_cast<PaletteRaw*>(calloc(1, sizeof(PaletteRaw)));
    if (!p) { sys::setError("Out of memory"); return nullptr; }
    p->colors = static_cast<Color*>(malloc(static_cast<size_t>(ncolors) * sizeof(Color)));
    if (!p->colors) { free(p); sys::setError("Out of memory"); return nullptr; }
    p->ncolors = ncolors;
    memset(p->colors, kOpaqueWhiteByte, static_cast<size_t>(ncolors) * sizeof(Color));   // every colour starts opaque white
    p->version = 1;
    p->refcount = 1;
    return p;
}

inline void freePalette(PaletteRaw* p) {
    if (!p) { sys::invalidParam("palette"); return; }
    releasePalette(p);   // drops one reference, frees at zero
}

inline int setPaletteColors(PaletteRaw* p, const Color* colors, int first, int n) {
    if (!p) return sys::invalidParam("palette");
    if (!colors) return sys::invalidParam("colors");
    int status = 0;
    if (first < 0) { colors -= first; n += first; first = 0; status = -1; }
    if (first + n > p->ncolors) { n = p->ncolors - first; status = -1; }
    if (n > 0) { memcpy(p->colors + first, colors, static_cast<size_t>(n) * sizeof(Color)); ++p->version; if (!p->version) p->version = 1; }
    return status;
}

using sdl2::initChannel;

inline PixelFormatRaw* allocFormat(uint32_t format) {
    FormatList& list = formatList();
    std::lock_guard<std::mutex> lk(list.mx);
    for (PixelFormatRaw* f = list.head; f; f = f->next)
        if (f->format == format) { ++f->refcount; return f; }
    int bpp;
    uint32_t rm, gm, bm, am;
    if (!enumToMasks(format, &bpp, &rm, &gm, &bm, &am)) return nullptr;
    auto* f = static_cast<PixelFormatRaw*>(calloc(1, sizeof(PixelFormatRaw)));
    if (!f) { sys::setError("Out of memory"); return nullptr; }
    f->format = format;
    f->bitsPerPixel = static_cast<uint8_t>(bpp);
    f->bytesPerPixel = static_cast<uint8_t>(bytesPerPixel(format) ? bytesPerPixel(format) : (bpp + 7) / 8);
    if (isIndexed(format)) f->bytesPerPixel = 1;   // the palette is set by SDL_SetPixelFormatPalette; a new format has none
    f->rMask = rm; f->gMask = gm; f->bMask = bm; f->aMask = am;
    initChannel(rm, f->rLoss, f->rShift);
    initChannel(gm, f->gLoss, f->gShift);
    initChannel(bm, f->bLoss, f->bShift);
    initChannel(am, f->aLoss, f->aShift);
    f->refcount = 1;
    f->next = list.head;
    list.head = f;
    return f;
}

inline void freeFormat(PixelFormatRaw* f) {
    if (!f) { sys::invalidParam("format"); return; }
    FormatList& list = formatList();
    std::lock_guard<std::mutex> lk(list.mx);
    if (--f->refcount > 0) return;
    for (PixelFormatRaw** p = &list.head; *p; p = &(*p)->next)
        if (*p == f) { *p = f->next; break; }
    if (f->palette) freePalette(static_cast<PaletteRaw*>(f->palette));
    free(f);
}

inline int setFormatPalette(PixelFormatRaw* f, PaletteRaw* pal) {
    if (!f) return sys::invalidParam("SDL_SetPixelFormatPalette(): format");
    if (pal && !isIndexed(f->format)) return sys::setError("SDL_SetPixelFormatPalette() passed a palette that doesn't match the format");
    if (pal && pal->ncolors > (1 << f->bitsPerPixel)) return sys::setError("SDL_SetPixelFormatPalette() passed a palette that doesn't match the format");
    if (f->palette == pal) return 0;
    if (f->palette) freePalette(static_cast<PaletteRaw*>(f->palette));
    f->palette = pal;
    if (pal) ++pal->refcount;
    return 0;
}

using sdl2::findColor;

inline uint32_t mapRGBA(const PixelFormatRaw* f, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (f->palette) return findColor(static_cast<const PaletteRaw*>(f->palette), r, g, b, a);
    return (channelFrom8(r, f->rLoss) << f->rShift) | (channelFrom8(g, f->gLoss) << f->gShift) |
           (channelFrom8(b, f->bLoss) << f->bShift) | ((channelFrom8(a, f->aLoss) << f->aShift) & f->aMask);
}

inline uint32_t mapRGB(const PixelFormatRaw* f, uint8_t r, uint8_t g, uint8_t b) {
    if (f->palette) return findColor(static_cast<const PaletteRaw*>(f->palette), r, g, b, 255);
    return (channelFrom8(r, f->rLoss) << f->rShift) | (channelFrom8(g, f->gLoss) << f->gShift) |
           (channelFrom8(b, f->bLoss) << f->bShift) | f->aMask;
}

using sdl2::expandChannel;

inline void getRGBA(uint32_t pixel, const PixelFormatRaw* f, uint8_t* r, uint8_t* g, uint8_t* b, uint8_t* a) {
    if (f->palette) {
        const PaletteRaw* pal = static_cast<const PaletteRaw*>(f->palette);
        const Color c = pixel < static_cast<uint32_t>(pal->ncolors) ? pal->colors[pixel] : Color{ kOpaqueWhiteByte, kOpaqueWhiteByte, kOpaqueWhiteByte, kOpaqueWhiteByte };
        if (r) *r = c.r;
        if (g) *g = c.g;
        if (b) *b = c.b;
        if (a) *a = c.a;
        return;
    }
    if (r) *r = channelTo8((pixel & f->rMask) >> f->rShift, f->rLoss);
    if (g) *g = channelTo8((pixel & f->gMask) >> f->gShift, f->gLoss);
    if (b) *b = channelTo8((pixel & f->bMask) >> f->bShift, f->bLoss);
    if (a) *a = f->aMask ? channelTo8((pixel & f->aMask) >> f->aShift, f->aLoss) : 255;
}

inline void calculateGammaRamp(float gamma, uint16_t* ramp) {
    if (!ramp) { sys::invalidParam("ramp"); return; }
    if (gamma < 0.0f) { sys::invalidParam("gamma"); return; }
    if (gamma == 0.0f) { memset(ramp, 0, 256 * sizeof(uint16_t)); return; }
    if (gamma == 1.0f) { for (int i = 0; i < 256; i++) ramp[i] = static_cast<uint16_t>((i << 8) | i); return; }
    const double inv = 1.0 / gamma;
    for (int i = 0; i < 256; i++) {
        double v = std::pow(i / 256.0, inv) * 65535.0 + 0.5;   // SDL divides by 256, so the last entries stay below 65535
        if (v > 65535.0) v = 65535.0;
        ramp[i] = static_cast<uint16_t>(v);
    }
}

} // namespace pix
} // namespace sdl2
} // namespace pharo
