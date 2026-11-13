// SDL2StubsSurface.inc - pixel format and palette calls, the surface family,
// and the render and texture calls that read or complete the state of
// SDL2StubsCore.inc (render getters, texture user data, textures from
// surfaces).
//
// Included by FFI.cpp inside namespace pharo::ffi.

namespace sdlsurf = sdl2::surf;
namespace sdlpix = sdl2::pix;

static sdl2::SurfaceRaw* asSurface(void* s) { return static_cast<sdl2::SurfaceRaw*>(s); }
static bool surfaceOk(void* s, const char* param) {
    if (sdl2::isSurface(s)) return true;
    sdl2::sys::invalidParam(param);
    return false;
}

// ----- Pixel formats and palettes (SDL_pixels.h) -----

static const char* stub_SDL_GetPixelFormatName(uint32_t f) { return sdlpix::formatName(f); }
static int stub_SDL_PixelFormatEnumToMasks(uint32_t f, int* bpp, uint32_t* r, uint32_t* g, uint32_t* b, uint32_t* a) {
    int dummyBpp;
    uint32_t d0, d1, d2, d3;
    return sdlpix::enumToMasks(f, bpp ? bpp : &dummyBpp, r ? r : &d0, g ? g : &d1, b ? b : &d2, a ? a : &d3);
}
static uint32_t stub_SDL_MasksToPixelFormatEnum(int bpp, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    return sdlpix::masksToEnum(bpp, r, g, b, a);
}
static void* stub_SDL_AllocFormat(uint32_t f) { return sdlpix::allocFormat(f); }
static void stub_SDL_FreeFormat(void* f) { sdlpix::freeFormat(static_cast<sdl2::PixelFormatRaw*>(f)); }
static void* stub_SDL_AllocPalette(int n) { return sdlpix::allocPalette(n); }
static void stub_SDL_FreePalette(void* p) { sdlpix::freePalette(static_cast<sdlpix::PaletteRaw*>(p)); }
static int stub_SDL_SetPaletteColors(void* p, const void* c, int first, int n) {
    if (!p || !c) return -1;   // SDL sets no error text for these
    return sdlpix::setPaletteColors(static_cast<sdlpix::PaletteRaw*>(p), static_cast<const sdlpix::Color*>(c), first, n);
}
static int stub_SDL_SetPixelFormatPalette(void* f, void* p) {
    return sdlpix::setFormatPalette(static_cast<sdl2::PixelFormatRaw*>(f), static_cast<sdlpix::PaletteRaw*>(p));
}
static uint32_t stub_SDL_MapRGB(const void* f, uint8_t r, uint8_t g, uint8_t b) {
    if (!f) { sdl2::sys::invalidParam("format"); return 0; }
    return sdlpix::mapRGB(static_cast<const sdl2::PixelFormatRaw*>(f), r, g, b);
}
static uint32_t stub_SDL_MapRGBA(const void* f, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (!f) { sdl2::sys::invalidParam("format"); return 0; }
    return sdlpix::mapRGBA(static_cast<const sdl2::PixelFormatRaw*>(f), r, g, b, a);
}
static void stub_SDL_GetRGB(uint32_t px, const void* f, uint8_t* r, uint8_t* g, uint8_t* b) {
    if (!f) { sdl2::sys::invalidParam("format"); return; }
    sdlpix::getRGBA(px, static_cast<const sdl2::PixelFormatRaw*>(f), r, g, b, nullptr);
}
static void stub_SDL_GetRGBA(uint32_t px, const void* f, uint8_t* r, uint8_t* g, uint8_t* b, uint8_t* a) {
    if (!f) { sdl2::sys::invalidParam("format"); return; }
    sdlpix::getRGBA(px, static_cast<const sdl2::PixelFormatRaw*>(f), r, g, b, a);
}
static void stub_SDL_CalculateGammaRamp(float gamma, uint16_t* ramp) { sdlpix::calculateGammaRamp(gamma, ramp); }

// ----- Surfaces (SDL_surface.h) -----

static void* stub_SDL_CreateRGBSurfaceWithFormatFrom(void* pixels, int w, int h, int depth, int pitch, uint32_t format) {
    (void)depth;
    if (w < 0) { sdl2::sys::invalidParam("width"); return nullptr; }
    if (h < 0) { sdl2::sys::invalidParam("height"); return nullptr; }
    return createSurfaceOver(pixels, w, h, pitch, format);
}

static void* stub_SDL_ConvertSurface(void* surface, const void* fmt, uint32_t flags) {
    (void)flags;
    if (!surfaceOk(surface, "surface")) return nullptr;
    if (!fmt) return sdlFailNull("Parameter 'format' is invalid");
    return sdlsurf::convert(asSurface(surface), static_cast<const sdl2::PixelFormatRaw*>(fmt)->format);
}
static void* stub_SDL_DuplicateSurface(void* surface) {
    if (!surfaceOk(surface, "surface")) return nullptr;
    return sdlsurf::duplicate(asSurface(surface));
}

static int stub_SDL_FillRect(void* dst, const void* rect, uint32_t color) {
    return sdlsurf::fillRect(asSurface(dst), static_cast<const sdl2::Rect*>(rect), color);
}
static int stub_SDL_FillRects(void* dst, const void* rects, int count, uint32_t color) {
    return sdlsurf::fillRects(asSurface(dst), static_cast<const sdl2::Rect*>(rects), count, color);
}
static int stub_SDL_SetClipRect(void* s, const void* rect) {
    if (!s) return 0;
    return sdlsurf::setClipRect(asSurface(s), static_cast<const sdl2::Rect*>(rect));
}
static void stub_SDL_GetClipRect(void* s, void* rect) {
    if (!s || !rect) return;   // SDL sets no error for a null surface or rectangle
    sdlsurf::getClipRect(asSurface(s), static_cast<sdl2::Rect*>(rect));
}

static int stub_SDL_SetColorKey(void* s, int flag, uint32_t key) {
    if (!surfaceOk(s, "surface")) return -1;
    if (flag && s && static_cast<sdl2::SurfaceRaw*>(s)->format->aMask == 0 && false) return -1;
    std::lock_guard<std::mutex> lk(sdlsurf::extraTable().mx);
    sdlsurf::Extra& e = sdlsurf::extraOf(asSurface(s));
    e.hasColorKey = flag != 0;
    e.colorKey = flag ? key : 0;
    return 0;
}
static int stub_SDL_GetColorKey(void* s, uint32_t* key) {
    if (!surfaceOk(s, "surface")) return -1;
    std::lock_guard<std::mutex> lk(sdlsurf::extraTable().mx);
    const sdlsurf::Extra& e = sdlsurf::extraOf(asSurface(s));
    if (!e.hasColorKey) return sdl2::sys::setError("Surface doesn't have a colorkey");
    if (key) *key = e.colorKey;
    return 0;
}
static int stub_SDL_HasColorKey(void* s) {
    if (!sdl2::isSurface(s)) return 0;
    std::lock_guard<std::mutex> lk(sdlsurf::extraTable().mx);
    return sdlsurf::extraOf(asSurface(s)).hasColorKey ? 1 : 0;
}

static int stub_SDL_SetSurfaceColorMod(void* s, uint8_t r, uint8_t g, uint8_t b) {
    if (!surfaceOk(s, "surface")) return -1;
    std::lock_guard<std::mutex> lk(sdlsurf::extraTable().mx);
    sdlsurf::Extra& e = sdlsurf::extraOf(asSurface(s));
    e.rMod = r; e.gMod = g; e.bMod = b;
    return 0;
}
static int stub_SDL_GetSurfaceColorMod(void* s, uint8_t* r, uint8_t* g, uint8_t* b) {
    if (!sdl2::isSurface(s)) return sdl2::sys::invalidParam("surface");
    std::lock_guard<std::mutex> lk(sdlsurf::extraTable().mx);
    const sdlsurf::Extra& e = sdlsurf::extraOf(asSurface(s));
    if (r) *r = e.rMod;
    if (g) *g = e.gMod;
    if (b) *b = e.bMod;
    return 0;
}
static int stub_SDL_SetSurfaceAlphaMod(void* s, uint8_t a) {
    if (!surfaceOk(s, "surface")) return -1;
    std::lock_guard<std::mutex> lk(sdlsurf::extraTable().mx);
    sdlsurf::extraOf(asSurface(s)).aMod = a;
    return 0;
}
static int stub_SDL_GetSurfaceAlphaMod(void* s, uint8_t* a) {
    if (!sdl2::isSurface(s)) return sdl2::sys::invalidParam("surface");
    std::lock_guard<std::mutex> lk(sdlsurf::extraTable().mx);
    if (a) *a = sdlsurf::extraOf(asSurface(s)).aMod;
    return 0;
}
// A surface supports the five named blend modes, and no custom mode.
static int stub_SDL_SetSurfaceBlendMode(void* s, uint32_t mode) {
    if (!surfaceOk(s, "surface")) return -1;
    if (mode != sdl2::kBlendNone && mode != sdl2::kBlendBlend && mode != sdl2::kBlendAdd && mode != sdl2::kBlendMod && mode != sdl2::kBlendMul)
        return sdl2::sys::setError(kUnsupported);
    std::lock_guard<std::mutex> lk(sdlsurf::extraTable().mx);
    sdlsurf::extraOf(asSurface(s)).blend = mode;
    return 0;
}
static int stub_SDL_GetSurfaceBlendMode(void* s, uint32_t* mode) {
    if (!sdl2::isSurface(s)) return sdl2::sys::invalidParam("surface");
    std::lock_guard<std::mutex> lk(sdlsurf::extraTable().mx);
    if (mode) *mode = sdlsurf::extraOf(asSurface(s)).blend;
    return 0;
}
// Run length encoding is only a speed hint: the flag is kept and answered.
static int stub_SDL_SetSurfaceRLE(void* s, int flag) {
    if (!surfaceOk(s, "surface")) return -1;
    std::lock_guard<std::mutex> lk(sdlsurf::extraTable().mx);
    sdlsurf::extraOf(asSurface(s)).rle = flag != 0;
    return 0;
}
static int stub_SDL_HasSurfaceRLE(void* s) {
    if (!sdl2::isSurface(s)) return 0;
    std::lock_guard<std::mutex> lk(sdlsurf::extraTable().mx);
    return sdlsurf::extraOf(asSurface(s)).rle ? 1 : 0;
}
static int stub_SDL_SetSurfacePalette(void* s, void* palette) {
    if (!surfaceOk(s, "surface")) return -1;
    return sdlpix::setFormatPalette(asSurface(s)->format, static_cast<sdlpix::PaletteRaw*>(palette));
}

static int stub_SDL_UpperBlit(void* src, const void* srcrect, void* dst, void* dstrect) {
    return sdlsurf::upperBlit(asSurface(src), static_cast<const sdl2::Rect*>(srcrect), asSurface(dst), static_cast<sdl2::Rect*>(dstrect));
}
static int stub_SDL_LowerBlit(void* src, void* srcrect, void* dst, void* dstrect) {
    return sdlsurf::upperBlit(asSurface(src), static_cast<const sdl2::Rect*>(srcrect), asSurface(dst), static_cast<sdl2::Rect*>(dstrect));
}
static int stub_SDL_UpperBlitScaled(void* src, const void* srcrect, void* dst, void* dstrect) {
    return sdlsurf::upperBlitScaled(asSurface(src), static_cast<const sdl2::Rect*>(srcrect), asSurface(dst), static_cast<sdl2::Rect*>(dstrect), false);
}
static int stub_SDL_LowerBlitScaled(void* src, void* srcrect, void* dst, void* dstrect) {
    return sdlsurf::upperBlitScaled(asSurface(src), static_cast<const sdl2::Rect*>(srcrect), asSurface(dst), static_cast<sdl2::Rect*>(dstrect), false);
}
static int stub_SDL_SoftStretch(void* src, const void* srcrect, void* dst, const void* dstrect) {
    if (!src) return sdl2::sys::invalidParam("src");
    if (!dst) return sdl2::sys::invalidParam("dst");
    sdl2::Rect d = dstrect ? *static_cast<const sdl2::Rect*>(dstrect) : sdl2::Rect{ 0, 0, asSurface(dst)->w, asSurface(dst)->h };
    return sdlsurf::upperBlitScaled(asSurface(src), static_cast<const sdl2::Rect*>(srcrect), asSurface(dst), &d, false);
}
static int stub_SDL_SoftStretchLinear(void* src, const void* srcrect, void* dst, const void* dstrect) {
    if (!src) return sdl2::sys::invalidParam("src");
    if (!dst) return sdl2::sys::invalidParam("dst");
    sdl2::Rect d = dstrect ? *static_cast<const sdl2::Rect*>(dstrect) : sdl2::Rect{ 0, 0, asSurface(dst)->w, asSurface(dst)->h };
    return sdlsurf::upperBlitScaled(asSurface(src), static_cast<const sdl2::Rect*>(srcrect), asSurface(dst), &d, true);
}

// SDL_PremultiplyAlpha: each colour channel times the alpha, for two 32-bit images.
static int stub_SDL_PremultiplyAlpha(int w, int h, uint32_t srcFormat, const void* src, int srcPitch,
                                     uint32_t dstFormat, void* dst, int dstPitch) {
    if (!src) return sdl2::sys::invalidParam("src");
    if (!dst) return sdl2::sys::invalidParam("dst");
    const sdl2::FormatInfo* sf = sdl2::formatInfo(srcFormat);
    const sdl2::FormatInfo* df = sdl2::formatInfo(dstFormat);
    if (!sf || !df) return sdl2::sys::setError("Unknown pixel format: only 32-bit surfaces are supported");
    for (int y = 0; y < h; y++) {
        const uint32_t* s = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(src) + static_cast<size_t>(y) * srcPitch);
        uint32_t* d = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(dst) + static_cast<size_t>(y) * dstPitch);
        for (int x = 0; x < w; x++) {
            sdl2::RGBA c = sdl2::unpackPixel(*sf, s[x]);
            c.r = static_cast<uint8_t>(c.r * c.a / 255);
            c.g = static_cast<uint8_t>(c.g * c.a / 255);
            c.b = static_cast<uint8_t>(c.b * c.a / 255);
            d[x] = sdl2::packPixel(*df, c);
        }
    }
    return 0;
}

// SDL_ConvertPixels between two pixel formats: any packed or indexed format of
// 8, 16, 24 or 32 bits per pixel.  The channels go through 8-bit RGBA.
static uint32_t readRawPixel(const uint8_t* p, int bytes) {
    uint32_t v = 0;
    for (int i = 0; i < bytes; i++) v |= static_cast<uint32_t>(p[i]) << (8 * i);
    return v;
}
static void writeRawPixel(uint8_t* p, int bytes, uint32_t v) {
    for (int i = 0; i < bytes; i++) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
static int stub_SDL_ConvertPixels(int w, int h, uint32_t srcFormat, const void* src, int srcPitch,
                                  uint32_t dstFormat, void* dst, int dstPitch) {
    if (!src) return sdl2::sys::invalidParam("src");
    if (!srcPitch) return sdl2::sys::invalidParam("src_pitch");
    if (!dst) return sdl2::sys::invalidParam("dst");
    if (!dstPitch) return sdl2::sys::invalidParam("dst_pitch");
    sdl2::PixelFormatRaw* sf = sdlpix::allocFormat(srcFormat);
    if (!sf) return -1;
    sdl2::PixelFormatRaw* df = sdlpix::allocFormat(dstFormat);
    if (!df) { sdlpix::freeFormat(sf); return -1; }
    for (int y = 0; y < h; y++) {
        const uint8_t* s = static_cast<const uint8_t*>(src) + static_cast<size_t>(y) * srcPitch;
        uint8_t* d = static_cast<uint8_t*>(dst) + static_cast<size_t>(y) * dstPitch;
        for (int x = 0; x < w; x++) {
            uint8_t r, g, b, a;
            sdlpix::getRGBA(readRawPixel(s + x * sf->bytesPerPixel, sf->bytesPerPixel), sf, &r, &g, &b, &a);
            writeRawPixel(d + x * df->bytesPerPixel, df->bytesPerPixel, sdlpix::mapRGBA(df, r, g, b, a));
        }
    }
    sdlpix::freeFormat(sf);
    sdlpix::freeFormat(df);
    return 0;
}

static void* stub_SDL_LoadBMP_RW(void* src, int freesrc) { return sdlsurf::loadBMP(static_cast<sdl2::rw::RWops*>(src), freesrc); }
static int stub_SDL_SaveBMP_RW(void* s, void* dst, int freedst) {
    return sdlsurf::saveBMP(asSurface(s), static_cast<sdl2::rw::RWops*>(dst), freedst);
}

static int sYuvConversionMode = 1;   // SDL_YUV_CONVERSION_BT601, the SDL default
static void stub_SDL_SetYUVConversionMode(int mode) { sYuvConversionMode = mode; }
static int stub_SDL_GetYUVConversionMode() { return sYuvConversionMode; }
static int stub_SDL_GetYUVConversionModeForResolution(int w, int h) {
    constexpr int kAutomatic = 3, kBt601 = 1, kBt709 = 2;
    if (sYuvConversionMode != kAutomatic) return sYuvConversionMode;
    return (h > 576 || w > 1024) ? kBt709 : kBt601;
}

// ----- Render and texture getters and the calls that make textures from surfaces -----

static int stub_SDL_GetRenderDrawColor(void* renderer, uint8_t* r, uint8_t* g, uint8_t* b, uint8_t* a) {
    SDLRendererState* rs = findRenderer(renderer);
    if (!rs) return sdlFail(-1, "Parameter 'renderer' is invalid");
    if (r) *r = rs->drawR;
    if (g) *g = rs->drawG;
    if (b) *b = rs->drawB;
    if (a) *a = rs->drawA;
    return 0;
}
static int stub_SDL_GetRenderDrawBlendMode(void* renderer, uint32_t* mode) {
    SDLRendererState* rs = findRenderer(renderer);
    if (!rs) return sdlFail(-1, "Parameter 'renderer' is invalid");
    if (mode) *mode = rs->drawBlend;
    return 0;
}
static void* stub_SDL_GetRenderTarget(void* renderer) {
    SDLRendererState* rs = findRenderer(renderer);
    if (!rs) return sdlFailNull("Parameter 'renderer' is invalid");
    return rs->target;
}
static void* stub_SDL_RenderGetWindow(void* renderer) {
    SDLRendererState* rs = findRenderer(renderer);
    if (!rs) return sdlFailNull("Parameter 'renderer' is invalid");
    return rs->window;
}
static void* stub_SDL_GetRenderer(void* window) {
    if (!findWindow(window)) { sdl2::sys::setError("Invalid window"); return nullptr; }
    void* found = nullptr;
    for (auto& kv : sRenderers)
        if (kv.second.window == window && (!found || kv.first > found)) found = kv.first;
    return found;
}
static int stub_SDL_RenderFlush(void* renderer) {
    if (!findRenderer(renderer)) return sdlFail(-1, "Parameter 'renderer' is invalid");
    return 0;   // drawing is immediate: nothing is batched
}
static void* stub_SDL_RenderGetMetalLayer(void* renderer) {
    if (!findRenderer(renderer)) return sdlFailNull("Parameter 'renderer' is invalid");
    sdl2::sys::setError(kUnsupported);
    return nullptr;
}
static void* stub_SDL_RenderGetMetalCommandEncoder(void* renderer) { return stub_SDL_RenderGetMetalLayer(renderer); }

static int stub_SDL_GetTextureColorMod(void* texture, uint8_t* r, uint8_t* g, uint8_t* b) {
    SDLTextureState* t = findTexture(texture);
    if (!t) return sdlFail(-1, "Parameter 'texture' is invalid");
    if (r) *r = t->rMod;
    if (g) *g = t->gMod;
    if (b) *b = t->bMod;
    return 0;
}
static int stub_SDL_GetTextureAlphaMod(void* texture, uint8_t* a) {
    SDLTextureState* t = findTexture(texture);
    if (!t) return sdlFail(-1, "Parameter 'texture' is invalid");
    if (a) *a = t->aMod;
    return 0;
}
static int stub_SDL_GetTextureBlendMode(void* texture, uint32_t* mode) {
    SDLTextureState* t = findTexture(texture);
    if (!t) return sdlFail(-1, "Parameter 'texture' is invalid");
    if (mode) *mode = t->blendMode;
    return 0;
}
static int stub_SDL_GetTextureScaleMode(void* texture, int* mode) {
    SDLTextureState* t = findTexture(texture);
    if (!t) return sdlFail(-1, "Parameter 'texture' is invalid");
    if (mode) *mode = t->linear ? 1 : 0;
    return 0;
}
static int stub_SDL_SetTextureUserData(void* texture, void* userdata) {
    SDLTextureState* t = findTexture(texture);
    if (!t) return sdlFail(-1, "Parameter 'texture' is invalid");
    t->userData = userdata;
    return 0;
}
static void* stub_SDL_GetTextureUserData(void* texture) {
    SDLTextureState* t = findTexture(texture);
    if (!t) return sdlFailNull("Parameter 'texture' is invalid");
    return t->userData;
}

// The textures of this layer have 32-bit formats, never a YUV format.
static int notYuv(void* texture) {
    SDLTextureState* t = findTexture(texture);
    if (!t) return sdlFail(-1, "Parameter 'texture' is invalid");
    return sdl2::sys::setError((std::string("Texture format ") + sdlpix::formatName(t->format) + " is not YUV").c_str());
}
static int stub_SDL_UpdateYUVTexture(void* t, const void*, const void*, int, const void*, int, const void*, int) { return notYuv(t); }
static int stub_SDL_UpdateNVTexture(void* t, const void*, const void*, int, const void*, int) { return notYuv(t); }

// OpenGL textures do not exist here: the renderer has no GL backend.
static int stub_SDL_GL_BindTexture(void* texture, float* w, float* h) {
    (void)w; (void)h;
    if (!findTexture(texture)) return sdlFail(-1, "Parameter 'texture' is invalid");
    return sdl2::sys::setError(kUnsupported);
}
static int stub_SDL_GL_UnbindTexture(void* texture) {
    if (!findTexture(texture)) return sdlFail(-1, "Parameter 'texture' is invalid");
    return sdl2::sys::setError(kUnsupported);
}

// A texture from a surface: a static texture of the surface size with the
// pixels of the surface, its blend mode and its modulation.  A surface with a
// colour key gives transparent pixels where the key matches.
static void* stub_SDL_CreateTextureFromSurface(void* renderer, void* surface) {
    if (!findRenderer(renderer)) return sdlFailNull("Parameter 'renderer' is invalid");
    if (!surface) return sdlFailNull("SDL_CreateTextureFromSurface(): surface");
    if (!sdl2::isSurface(surface)) return sdlFailNull("Parameter 'surface' is invalid");
    sdl2::SurfaceRaw* s = asSurface(surface);
    const sdl2::Pixels sp = sdl2::surfacePixels(s);
    if (!sp.valid()) return sdlFailNull("Unsupported pixel format");
    const uint32_t format = !sp.hasAlpha() ? sdl2::kFormatXRGB8888
                          : (sdl2::formatInfo(sp.pf->format) ? sp.pf->format : sdl2::kFormatARGB8888);
    void* tex = stub_SDL_CreateTexture(renderer, format, kAccessStatic, s->w, s->h);
    if (!tex) return nullptr;
    SDLTextureState* t = findTexture(tex);
    sdlsurf::Extra e;
    { std::lock_guard<std::mutex> lk(sdlsurf::extraTable().mx); e = sdlsurf::extraOf(s); }
    const sdl2::Pixels tp = texturePixels(*t);
    for (int y = 0; y < s->h; y++) {
        for (int x = 0; x < s->w; x++) {
            const uint32_t raw = sp.raw(x, y);
            sdl2::RGBA c = sp.unpack(raw);
            if (sdlsurf::keyedOut(sp, e, raw)) c.a = 0;
            tp.put(x, y, c);
        }
    }
    t->blendMode = e.blend;
    t->rMod = e.rMod; t->gMod = e.gMod; t->bMod = e.bMod; t->aMod = e.aMod;
    if (e.hasColorKey && e.blend == sdl2::kBlendNone) t->blendMode = sdl2::kBlendBlend;
    return tex;
}

static void* stub_SDL_CreateSoftwareRenderer(void* surface) {
    if (!sdl2::isSurface(surface)) return sdlFailNull("Parameter 'surface' is invalid");
    // The renderer draws into a window-sized buffer of its own; the surface only names the size.
    constexpr uint32_t kFlagHidden = 8;   // SDL_WINDOW_HIDDEN
    void* window = stub_SDL_CreateWindow("", 0, 0, asSurface(surface)->w, asSurface(surface)->h, kFlagHidden);
    void* r = stub_SDL_CreateRenderer(window, -1, kRendererSoftware);
    if (!r) stub_SDL_DestroyWindow(window);
    return r;
}

static int stub_SDL_CreateWindowAndRenderer(int w, int h, uint32_t flags, void** window, void** renderer) {
    if (!window) return sdl2::sys::invalidParam("window");
    if (!renderer) return sdl2::sys::invalidParam("renderer");
    void* win = stub_SDL_CreateWindow("", 0, 0, w, h, flags);
    if (!win) return -1;
    void* r = stub_SDL_CreateRenderer(win, -1, 0);
    if (!r) { stub_SDL_DestroyWindow(win); return -1; }
    *window = win;
    *renderer = r;
    return 0;
}

// A surface that wraps the pixels of a streaming texture, until SDL_UnlockTexture.
static int stub_SDL_LockTextureToSurface(void* texture, const void* rect, void** surface) {
    SDLTextureState* t = findTexture(texture);
    if (!t) return -1;   // SDL sets no error text for an unknown texture here
    if (!surface) return sdl2::sys::invalidParam("surface");
    *surface = nullptr;
    void* pixels = nullptr;
    int pitch = 0;
    if (stub_SDL_LockTexture(texture, const_cast<void*>(rect), &pixels, &pitch) < 0) return -1;
    sdl2::Rect r = rect ? *static_cast<const sdl2::Rect*>(rect) : sdl2::Rect{ 0, 0, t->width, t->height };
    const sdl2::FormatInfo* f = sdl2::formatInfo(t->format);
    t->lockedSurface = sdl2::createSurfaceFrom(pixels, r.w, r.h, pitch, *f);
    *surface = t->lockedSurface;
    return t->lockedSurface ? 0 : sdl2::sys::setError("Out of memory");
}
