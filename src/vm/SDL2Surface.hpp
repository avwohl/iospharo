/*
 * SDL2Surface.hpp - the surface calls of SDL_surface.h that work on the 32-bit
 * surfaces of the stub layer: fill, blit, scaled blit, clip, colour key,
 * modulation, blend mode, conversion, duplication and BMP files.
 *
 * A surface keeps its blend mode, colour key and modulation in a side table
 * (the SDL_Surface structure has no field for them), so that the structure
 * stays as the image reads it.  Written from the SDL 2.x documentation; the
 * blend arithmetic is shared with the texture copy (SDL2Raster.hpp).
 *
 * Copyright (c) 2025-2026 Aaron Wohl. Licensed under the MIT License.
 */
#pragma once

#include "SDL2Blit.hpp"
#include "SDL2Pixels.hpp"
#include "SDL2RW.hpp"

#include <unordered_map>

namespace pharo {
namespace sdl2 {
namespace surf {

using sys::setError;
using sys::invalidParam;

struct Extra {
    bool hasColorKey = false;
    uint32_t colorKey = 0;
    uint32_t blend = kBlendNone;
    uint8_t aMod = 255, rMod = 255, gMod = 255, bMod = 255;
    bool rle = false;
};

struct ExtraTable {
    std::mutex mx;
    std::unordered_map<const void*, Extra> map;
};
inline ExtraTable& extraTable() {
    static ExtraTable* t = new ExtraTable;
    return *t;
}

// A surface with an alpha channel starts in blend mode BLEND, the others in NONE.
inline Extra& extraOf(const SurfaceRaw* s) {
    ExtraTable& t = extraTable();
    auto it = t.map.find(s);
    if (it == t.map.end()) {
        Extra e;
        if (s->format && s->format->aMask) e.blend = kBlendBlend;
        it = t.map.emplace(s, e).first;
    }
    return it->second;
}

inline void forget(const SurfaceRaw* s) {
    ExtraTable& t = extraTable();
    std::lock_guard<std::mutex> lk(t.mx);
    t.map.erase(s);
}

inline bool valid(const SurfaceRaw* s) { return isSurface(s) && s->pixels; }

inline TextureSampling samplingFor(const SurfaceRaw* s, bool linear = false) {
    const Extra e = [&] { std::lock_guard<std::mutex> lk(extraTable().mx); return extraOf(s); }();
    TextureSampling t;
    t.rMod = e.rMod; t.gMod = e.gMod; t.bMod = e.bMod; t.aMod = e.aMod;
    t.blend = e.blend;
    t.linear = linear;
    return t;
}

// ----- clip -----

inline int setClipRect(SurfaceRaw* s, const Rect* r) {
    if (!s) return 0;
    const Rect bounds = { 0, 0, s->w, s->h };
    if (!r) { s->clipRect = bounds; return 1; }
    Rect out;
    const int hit = rect::intersectRect<int>(reinterpret_cast<const rect::RectT<int>*>(r), reinterpret_cast<const rect::RectT<int>*>(&bounds),
                                             reinterpret_cast<rect::RectT<int>*>(&out));
    s->clipRect = out;
    return hit ? 1 : 0;
}

inline void getClipRect(const SurfaceRaw* s, Rect* r) {
    if (s && r) *r = s->clipRect;
}

// ----- fill -----

inline int fillRect(SurfaceRaw* s, const Rect* r, uint32_t color) {
    if (!s) return invalidParam("SDL_FillRect(): dst");
    if (!isSurface(s) || !s->pixels) return setError("SDL_FillRect(): You must lock the surface");
    const Pixels p = surfacePixels(s);
    if (!p.valid()) return setError("SDL_FillRect(): Unsupported surface format");
    Rect target = r ? *r : Rect{ 0, 0, s->w, s->h };
    Rect clipped;
    if (!rect::intersectRect<int>(reinterpret_cast<const rect::RectT<int>*>(&target), reinterpret_cast<const rect::RectT<int>*>(&s->clipRect),
                                  reinterpret_cast<rect::RectT<int>*>(&clipped)))
        return 0;
    for (int y = clipped.y; y < clipped.y + clipped.h; y++) {
        uint32_t* row = p.at(clipped.x, y);
        for (int x = 0; x < clipped.w; x++) row[x] = color;
    }
    return 0;
}

inline int fillRects(SurfaceRaw* s, const Rect* rects, int count, uint32_t color) {
    if (!s) return invalidParam("SDL_FillRects(): dst");
    if (!rects) return invalidParam("SDL_FillRects(): rects");
    for (int i = 0; i < count; i++)
        if (fillRect(s, rects + i, color) < 0) return -1;
    return 0;
}

// ----- blit -----

// The pixels of one surface as a Pixels view; the colour key compares the raw value without the alpha bits.
inline bool keyedOut(const Pixels& src, const Extra& e, uint32_t raw) {
    if (!e.hasColorKey) return false;
    const uint32_t mask = ~src.fmt->aMask;
    return (raw & mask) == (e.colorKey & mask);
}

// SDL_UpperBlit: copy a rectangle of `src` to `dst`.  The source rectangle is
// cut to the source, the destination to the clip of the destination; the
// destination rectangle answers with the size of what was copied.
inline int upperBlit(SurfaceRaw* src, const Rect* srcrect, SurfaceRaw* dst, Rect* dstrect) {
    if (!src) return invalidParam("src");
    if (!dst) return invalidParam("dst");
    if (!valid(src) || !valid(dst)) return setError("Surfaces must not be locked during blit");
    Rect srect = srcrect ? *srcrect : Rect{ 0, 0, src->w, src->h };
    Rect drect = dstrect ? Rect{ dstrect->x, dstrect->y, srect.w, srect.h } : Rect{ 0, 0, srect.w, srect.h };
    // The source rectangle inside the source.
    if (srcrect) {
        int dx = 0, dy = 0;
        if (srect.x < 0) { dx = -srect.x; srect.w += srect.x; srect.x = 0; }
        if (srect.y < 0) { dy = -srect.y; srect.h += srect.y; srect.y = 0; }
        if (srect.x + srect.w > src->w) srect.w = src->w - srect.x;
        if (srect.y + srect.h > src->h) srect.h = src->h - srect.y;
        drect.x += dx; drect.y += dy;
    }
    // The destination inside its clip.
    const Rect clip = dst->clipRect;
    int cut = clip.x - drect.x;
    if (cut > 0) { srect.x += cut; srect.w -= cut; drect.x += cut; drect.w -= cut; }
    cut = drect.x + srect.w - (clip.x + clip.w);
    if (cut > 0) srect.w -= cut;
    cut = clip.y - drect.y;
    if (cut > 0) { srect.y += cut; srect.h -= cut; drect.y += cut; drect.h -= cut; }
    cut = drect.y + srect.h - (clip.y + clip.h);
    if (cut > 0) srect.h -= cut;
    if (srect.w <= 0 || srect.h <= 0) {
        if (dstrect) { dstrect->w = 0; dstrect->h = 0; }
        return 0;
    }
    drect.w = srect.w;
    drect.h = srect.h;
    if (dstrect) *dstrect = drect;

    const Pixels sp = surfacePixels(src), dp = surfacePixels(dst);
    if (!sp.valid() || !dp.valid()) return setError("Unsupported surface format");
    Extra e;
    { std::lock_guard<std::mutex> lk(extraTable().mx); e = extraOf(src); }
    const TextureSampling t = samplingFor(src);
    BlendSpec spec;
    if (!decodeBlendMode(t.blend, spec)) return setError("Unsupported blend mode");
    for (int y = 0; y < srect.h; y++) {
        for (int x = 0; x < srect.w; x++) {
            const uint32_t raw = *sp.at(srect.x + x, srect.y + y);
            if (keyedOut(sp, e, raw)) continue;
            const RGBA texel = modulate(unpackPixel(*sp.fmt, raw), t);
            const RGBA old = dp.get(drect.x + x, drect.y + y);
            RGBA out;
            if (t.blend == kBlendNone) {
                out = texel;
                if (!dp.fmt->aMask) out.a = 255;
            } else if (t.blend == kBlendBlend) {
                out = blitBlendPixel(texel, old, t.modulated());
            } else {
                out = blendPixel(spec, texel, old, false);
            }
            dp.put(drect.x + x, drect.y + y, out);
        }
    }
    return 0;
}

// SDL_UpperBlitScaled: the source rectangle stretched to the destination
// rectangle (nearest pixel).  The destination rectangle keeps its size.
inline int upperBlitScaled(SurfaceRaw* src, const Rect* srcrect, SurfaceRaw* dst, Rect* dstrect, bool linear) {
    if (!src || !dst) return invalidParam("SDL_UpperBlitScaled(): src/dst");
    if (!valid(src) || !valid(dst)) return setError("Surfaces must not be locked during blit");
    Rect srect = srcrect ? *srcrect : Rect{ 0, 0, src->w, src->h };
    Rect drect = dstrect ? *dstrect : Rect{ 0, 0, dst->w, dst->h };
    if (srect.w <= 0 || srect.h <= 0 || drect.w <= 0 || drect.h <= 0) return 0;
    if (srect.w == drect.w && srect.h == drect.h) return upperBlit(src, srcrect, dst, dstrect);
    const Pixels sp = surfacePixels(src), dp = surfacePixels(dst);
    if (!sp.valid() || !dp.valid()) return setError("Unsupported surface format");
    // The source rectangle must lie inside the source.
    if (srect.x < 0 || srect.y < 0 || srect.x + srect.w > src->w || srect.y + srect.h > src->h) return setError("Source rectangle is outside the surface");
    Extra e;
    { std::lock_guard<std::mutex> lk(extraTable().mx); e = extraOf(src); }
    const TextureSampling t = samplingFor(src, linear);
    BlendSpec spec;
    if (!decodeBlendMode(t.blend, spec)) return setError("Unsupported blend mode");
    std::vector<RGBA> scaled;
    if (linear) scaled = bilinearScale(sp, srect, drect.w, drect.h);
    for (int y = 0; y < drect.h; y++) {
        for (int x = 0; x < drect.w; x++) {
            const int dx = drect.x + x, dy = drect.y + y;
            if (!inRect(dst->clipRect, dx, dy)) continue;
            RGBA texel;
            if (linear) {
                texel = scaled[static_cast<size_t>(y) * drect.w + x];
            } else {
                const int sx = srect.x + std::min(nearestSourceIndex(x, srect.w, drect.w), srect.w - 1);
                const int sy = srect.y + std::min(nearestSourceIndex(y, srect.h, drect.h), srect.h - 1);
                const uint32_t raw = *sp.at(sx, sy);
                if (keyedOut(sp, e, raw)) continue;
                texel = unpackPixel(*sp.fmt, raw);
            }
            texel = modulate(texel, t);
            const RGBA old = dp.get(dx, dy);
            RGBA out;
            if (t.blend == kBlendNone) { out = texel; if (!dp.fmt->aMask) out.a = 255; }
            else if (t.blend == kBlendBlend) out = blitBlendPixel(texel, old, t.modulated());
            else out = blendPixel(spec, texel, old, false);
            dp.put(dx, dy, out);
        }
    }
    return 0;
}

// ----- conversion, duplication -----

inline SurfaceRaw* convert(const SurfaceRaw* src, uint32_t format) {
    const FormatInfo* f = formatInfo(format);
    if (!f) { setError("Unknown pixel format: only 32-bit surfaces are supported"); return nullptr; }
    const Pixels sp = surfacePixels(src);
    if (!sp.fmt) { setError("Unknown pixel format of the source surface"); return nullptr; }
    SurfaceRaw* out = createOwnedSurface(src->w, src->h, *f);
    if (!out) { setError("Out of memory"); return nullptr; }
    if (src->w > 0 && src->h > 0) convertRect(sp, { 0, 0, src->w, src->h }, *f, out->pixels, out->pitch);
    // The converted surface keeps the blend state of the source.
    Extra e;
    { std::lock_guard<std::mutex> lk(extraTable().mx); e = extraOf(src); }
    {
        std::lock_guard<std::mutex> lk(extraTable().mx);
        Extra& o = extraOf(out);
        o.blend = e.blend; o.aMod = e.aMod; o.rMod = e.rMod; o.gMod = e.gMod; o.bMod = e.bMod;
        o.hasColorKey = false;   // the key is a raw value of the old format
    }
    return out;
}

inline SurfaceRaw* duplicate(const SurfaceRaw* src) {
    SurfaceRaw* out = convert(src, src->format->format);
    if (!out) return nullptr;
    std::lock_guard<std::mutex> lk(extraTable().mx);
    Extra e = extraOf(src);
    extraOf(out) = e;
    return out;
}

// ----- BMP files -----

constexpr uint16_t kBmpMagic = 0x4D42;            // "BM"
constexpr uint32_t kMaskRed = 0x00FF0000u, kMaskGreen = 0x0000FF00u, kMaskBlue = 0x000000FFu, kMaskAlpha = 0xFF000000u;
constexpr uint32_t kBmpRgb = 0, kBmpBitfields = 3;
constexpr uint32_t kBmpV4HeaderSize = 108, kBmpInfoHeaderSize = 40, kBmpFileHeaderSize = 14;
constexpr uint32_t kBmpWinColorSpace = 0x57696E20;   // "Win "

inline void put16(std::vector<uint8_t>& b, uint32_t v) { b.push_back(v & 255); b.push_back((v >> 8) & 255); }
inline void put32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; i++) b.push_back((v >> (8 * i)) & 255); }

// A surface with an alpha channel is saved as a 32-bit file with a V4 header
// and bit masks; any other surface as a 24-bit file with a plain header.
inline int saveBMP(const SurfaceRaw* s, rw::RWops* dst, int freedst) {
    auto finish = [&](int status) { if (freedst && dst) rw::rwClose(dst); return status; };
    if (!s || !dst) return finish(-1);   // SDL sets no error text for a null surface or stream
    const Pixels p = surfacePixels(s);
    if (!p.valid()) return finish(setError("Unsupported surface format"));
    const bool alpha = p.fmt->aMask != 0;
    const uint32_t bytes = alpha ? 4 : 3;
    const uint32_t rowBytes = (static_cast<uint32_t>(s->w) * bytes + 3) & ~3u;
    const uint32_t imageSize = rowBytes * static_cast<uint32_t>(s->h);
    const uint32_t headerSize = alpha ? kBmpV4HeaderSize : kBmpInfoHeaderSize;
    const uint32_t offset = kBmpFileHeaderSize + headerSize;
    std::vector<uint8_t> out;
    put16(out, kBmpMagic); put32(out, offset + imageSize); put32(out, 0); put32(out, offset);
    put32(out, headerSize); put32(out, static_cast<uint32_t>(s->w)); put32(out, static_cast<uint32_t>(s->h));
    put16(out, 1); put16(out, bytes * 8); put32(out, alpha ? kBmpBitfields : kBmpRgb); put32(out, imageSize);
    put32(out, 0); put32(out, 0); put32(out, 0); put32(out, 0);
    if (alpha) {
        put32(out, kMaskRed); put32(out, kMaskGreen); put32(out, kMaskBlue); put32(out, kMaskAlpha);
        put32(out, kBmpWinColorSpace);
        for (int i = 0; i < 12; i++) put32(out, 0);   // end points and gamma
    }
    for (int y = s->h - 1; y >= 0; y--) {
        const size_t rowStart = out.size();
        for (int x = 0; x < s->w; x++) {
            const RGBA c = p.get(x, y);
            out.push_back(c.b); out.push_back(c.g); out.push_back(c.r);
            if (alpha) out.push_back(c.a);
        }
        while (out.size() - rowStart < rowBytes) out.push_back(0);
    }
    if (rw::rwWrite(dst, out.data(), 1, out.size()) != out.size()) return finish(-1);
    return finish(0);
}

inline uint32_t rd16(const uint8_t* p) { return p[0] | (p[1] << 8); }
inline uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

// 24 and 32 bit files without compression and 32 bit files with bit masks.
// A 24-bit file loads as an XRGB8888 surface (SDL answers a 24-bit surface,
// which this layer does not hold).
inline SurfaceRaw* loadBMP(rw::RWops* src, int freesrc) {
    auto finish = [&](SurfaceRaw* s) { if (freesrc && src) rw::rwClose(src); return s; };
    if (!src) { invalidParam("src"); return nullptr; }
    uint8_t head[kBmpFileHeaderSize + kBmpV4HeaderSize];
    if (rw::rwRead(src, head, 1, kBmpFileHeaderSize + kBmpInfoHeaderSize) != kBmpFileHeaderSize + kBmpInfoHeaderSize) {
        setError("Error reading from datastream");
        return finish(nullptr);
    }
    if (rd16(head) != kBmpMagic) { setError("File is not a Windows BMP file"); return finish(nullptr); }
    const uint32_t offset = rd32(head + 10), headerSize = rd32(head + 14);
    if (headerSize < kBmpInfoHeaderSize) { setError("Unsupported BMP header"); return finish(nullptr); }
    const int32_t w = static_cast<int32_t>(rd32(head + 18)), hRaw = static_cast<int32_t>(rd32(head + 22));
    const uint32_t bits = rd16(head + 28), compression = rd32(head + 30);
    uint32_t masks[4] = { kMaskRed, kMaskGreen, kMaskBlue, 0 };
    if (headerSize >= kBmpV4HeaderSize && bits == 32 && compression == kBmpBitfields) {
        uint8_t extra[kBmpV4HeaderSize - kBmpInfoHeaderSize];
        if (rw::rwRead(src, extra, 1, sizeof(extra)) != sizeof(extra)) { setError("Error reading from datastream"); return finish(nullptr); }
        for (int i = 0; i < 4; i++) masks[i] = rd32(extra + 4 * i);
    }
    if (!(bits == 24 || bits == 32) || (compression != kBmpRgb && compression != kBmpBitfields)) {
        setError("Compressed BMP files are not supported");
        return finish(nullptr);
    }
    const bool topDown = hRaw < 0;
    const int h = topDown ? -hRaw : hRaw;
    if (w <= 0 || h <= 0) { setError("Invalid BMP size"); return finish(nullptr); }
    const uint32_t bytes = bits / 8;
    const uint32_t rowBytes = (static_cast<uint32_t>(w) * bytes + 3) & ~3u;
    if (rw::rwSeek(src, offset, rw::kSeekSet) < 0) return finish(nullptr);
    uint32_t format = bits == 32 && masks[3] ? 0 : kFormatXRGB8888;
    if (bits == 32) format = formatFromMasks(32, masks[0], masks[1], masks[2], masks[3]);
    const FormatInfo* f = formatInfo(format ? format : kFormatXRGB8888);
    if (!f) { setError("Unsupported BMP bit masks"); return finish(nullptr); }
    SurfaceRaw* out = createOwnedSurface(w, h, *f);
    if (!out) { setError("Out of memory"); return finish(nullptr); }
    std::vector<uint8_t> row(rowBytes);
    const Pixels p = surfacePixels(out);
    for (int i = 0; i < h; i++) {
        if (rw::rwRead(src, row.data(), 1, rowBytes) != rowBytes) { setError("Error reading from datastream"); freeSurface(out); return finish(nullptr); }
        const int y = topDown ? i : h - 1 - i;
        for (int x = 0; x < w; x++) {
            const uint8_t* q = row.data() + x * bytes;
            if (bits == 24) p.put(x, y, RGBA{ q[2], q[1], q[0], 255 });
            else *p.at(x, y) = rd32(q);   // the file holds the pixel in the masks it names
        }
    }
    return finish(out);
}

} // namespace surf
} // namespace sdl2
} // namespace pharo
