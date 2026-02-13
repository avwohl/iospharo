/*
 * FFI.cpp - Foreign Function Interface using libffi
 *
 * Portable FFI implementation supporting all calling conventions.
 */

#include "FFI.hpp"
#include "../platform/EventQueue.hpp"
#include "../platform/DisplaySurface.hpp"
#include <iostream>
#include <algorithm>
#include <cstring>
#include <chrono>
#include <dlfcn.h>
#include <ffi.h>
#include <unistd.h>
#include <sys/mman.h>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

// SDL2 event types
#define SDL_QUIT            0x100
#define SDL_WINDOWEVENT     0x200
#define SDL_KEYDOWN         0x300
#define SDL_KEYUP           0x301
#define SDL_TEXTINPUT       0x303
#define SDL_MOUSEMOTION     0x400
#define SDL_MOUSEBUTTONDOWN 0x401
#define SDL_MOUSEBUTTONUP   0x402
#define SDL_MOUSEWHEEL      0x403

// SDL2 window event subtypes
#define SDL_WINDOWEVENT_SHOWN         1
#define SDL_WINDOWEVENT_EXPOSED       3
#define SDL_WINDOWEVENT_FOCUS_GAINED  12
#define SDL_WINDOWEVENT_SIZE_CHANGED  6

// SDL2 mouse button codes
#define SDL_BUTTON_LEFT     1
#define SDL_BUTTON_MIDDLE   2
#define SDL_BUTTON_RIGHT    3

// SDL2 event structures (simplified versions matching what OSWindow expects)
struct SDL_CommonEvent {
    uint32_t type;
    uint32_t timestamp;
};

struct SDL_MouseMotionEvent {
    uint32_t type;        // SDL_MOUSEMOTION
    uint32_t timestamp;
    uint32_t windowID;
    uint32_t which;       // Mouse instance id
    uint32_t state;       // Button state
    int32_t x;
    int32_t y;
    int32_t xrel;
    int32_t yrel;
};

struct SDL_MouseButtonEvent {
    uint32_t type;        // SDL_MOUSEBUTTONDOWN or SDL_MOUSEBUTTONUP
    uint32_t timestamp;
    uint32_t windowID;
    uint32_t which;       // Mouse instance id
    uint8_t button;       // SDL_BUTTON_LEFT/MIDDLE/RIGHT
    uint8_t state;        // SDL_PRESSED or SDL_RELEASED
    uint8_t clicks;       // Click count
    uint8_t padding1;
    int32_t x;
    int32_t y;
};

struct SDL_MouseWheelEvent {
    uint32_t type;        // SDL_MOUSEWHEEL
    uint32_t timestamp;
    uint32_t windowID;
    uint32_t which;       // Mouse instance id
    int32_t x;            // Horizontal scroll
    int32_t y;            // Vertical scroll
    uint32_t direction;   // Normal or flipped
};

struct SDL_WindowEvent {
    uint32_t type;        // SDL_WINDOWEVENT
    uint32_t timestamp;
    uint32_t windowID;
    uint8_t event;        // SDL_WindowEventID
    uint8_t padding1;
    uint8_t padding2;
    uint8_t padding3;
    int32_t data1;        // event-dependent data (e.g. width)
    int32_t data2;        // event-dependent data (e.g. height)
};

struct SDL_Keysym {
    int32_t scancode;     // SDL_Scancode
    int32_t sym;          // SDL_Keycode (SDLK_*)
    uint16_t mod;         // Key modifiers (SDL_Keymod)
    uint32_t unused;
};

struct SDL_KeyboardEvent {
    uint32_t type;        // SDL_KEYDOWN or SDL_KEYUP
    uint32_t timestamp;
    uint32_t windowID;
    uint8_t state;        // SDL_PRESSED or SDL_RELEASED
    uint8_t repeat;       // Non-zero if key repeat
    uint8_t padding2;
    uint8_t padding3;
    SDL_Keysym keysym;
};

// SDL_Event union
union SDL_Event {
    uint32_t type;
    SDL_CommonEvent common;
    SDL_WindowEvent window;
    SDL_MouseMotionEvent motion;
    SDL_MouseButtonEvent button;
    SDL_MouseWheelEvent wheel;
    SDL_KeyboardEvent key;
    uint8_t padding[56];  // SDL_Event is 56 bytes
};

namespace pharo {
namespace ffi {

// Forward declarations
void registerSDL2Stubs();

// Module handles
static bool sInitialized = false;

// Function pointer cache
static std::unordered_map<std::string, void*> sFunctionCache;

bool initializeFFI() {
    if (sInitialized) return true;
    sInitialized = true;

    // Register SDL2 stub functions for iOS
    // This makes OSWindow think SDL2 is available
    registerSDL2Stubs();

    return true;
}

void shutdownFFI() {
    sFunctionCache.clear();
    sInitialized = false;
}

bool isModuleLoaded(const std::string& moduleName) {
    // We support SDL2 and general dlsym lookup
    if (moduleName == "SDL2" || moduleName == "libSDL2" ||
        moduleName.find("SDL2") != std::string::npos) {
        // Check both real SDL2 and our stubs
        void* sdlInit = dlsym(RTLD_DEFAULT, "SDL_Init");
        if (sdlInit != nullptr) {
            return true;
        }
        // Check if our stubs are registered
        auto it = sFunctionCache.find("SDL_Init");
        return it != sFunctionCache.end();
    }

    // For other modules, check if any function from that module is available
    return true;  // Assume available, will fail on lookup if not
}

void* lookupFunction(const std::string& moduleName, const std::string& funcName) {
    // Check cache first
    auto it = sFunctionCache.find(funcName);
    if (it != sFunctionCache.end()) {
        return it->second;
    }

    // For SDL_ functions not in our stub cache, return a generic no-op
    // instead of falling through to dlsym (which finds force-loaded real SDL2
    // that crashes on our fake 0xDEADBEEF window handles).
    if (funcName.compare(0, 4, "SDL_") == 0) {
        fprintf(stderr, "[SDL-STUB] MISSING stub for: %s (returning no-op)\n", funcName.c_str());
        // Generic no-op: returns 0 (safe for int/void/pointer returns).
        // On ARM64, extra arguments are in registers and harmlessly ignored.
        static auto genericSDLNoOp = +[]() -> intptr_t { return 0; };
        void* func = reinterpret_cast<void*>(genericSDLNoOp);
        sFunctionCache[funcName] = func;
        return func;
    }

    // Look up the function via dlsym
    void* func = dlsym(RTLD_DEFAULT, funcName.c_str());
    if (func) {
        sFunctionCache[funcName] = func;
    }

    return func;
}

void registerFunction(const std::string& funcName, void* funcPtr) {
    sFunctionCache[funcName] = funcPtr;
}

// SDL2 stub functions for iOS
// These make OSWindow think SDL2 is available
// Support multiple windows/textures (Emergency Debugger creates separate windows)

static bool sSDL2Initialized = false;

// Per-window state
struct SDLWindowState {
    int width;
    int height;
    std::string title;
};

// Per-texture state
struct SDLTextureState {
    uint32_t* pixels;
    int width;
    int height;
    int pitch;
    void* renderer;  // Which renderer owns this texture
};

// Per-renderer state
struct SDLRendererState {
    void* window;
    void* currentTexture;  // Most recently used texture
};

static uintptr_t sNextHandle = 0x10000;  // Incrementing unique handles
static std::unordered_map<void*, SDLWindowState> sWindows;
static std::unordered_map<void*, SDLTextureState> sTextures;
static std::unordered_map<void*, SDLRendererState> sRenderers;
static void* sMainRenderer = nullptr;  // First renderer is the "main" one (renders to display)
static void* sMainWindow = nullptr;    // First window is the "main" one (receives events)
static bool sSDLRenderingActive = false;  // Set when SDL_RenderPresent first copies to display

// Pending synthetic window events (SDL2 sends these when window is created/shown)
static std::queue<uint8_t> sPendingWindowEvents;

// Mouse state tracking - updated by SDL_PollEvent, queried by SDL_GetMouseState/SDL_GetModState
static int sMouseX = 0;
static int sMouseY = 0;
static uint32_t sMouseButtons = 0;  // SDL button mask (bit 0=left, bit 1=middle, bit 2=right)
static uint32_t sKeyModState = 0;   // SDL keyboard modifier state

extern "C" {

// Query whether SDL2 has started rendering (SDL_RenderPresent was called)
bool ffi_isSDLRenderingActive() {
    return sSDLRenderingActive;
}

// SDL_Init returns 0 on success
int stub_SDL_Init(uint32_t flags) {
    fprintf(stderr, "[SDL-STUB] SDL_Init(0x%x)\n", flags);
    sSDL2Initialized = true;
    return 0;
}

void stub_SDL_Quit() {
    sSDL2Initialized = false;
}

// Returns version info - version 2.0.20
void stub_SDL_GetVersion(void* ver) {
    if (ver) {
        uint8_t* v = static_cast<uint8_t*>(ver);
        v[0] = 2;   // major
        v[1] = 0;   // minor
        v[2] = 20;  // patch
    }
}

const char* stub_SDL_GetError() {
    return "No error";
}

void* stub_SDL_CreateWindow(const char* title, int x, int y, int w, int h, uint32_t flags) {
    // Override dimensions with actual display surface size when available
    // so Pharo creates textures/Forms at the correct resolution
    if (pharo::gDisplaySurface) {
        w = pharo::gDisplaySurface->width();
        h = pharo::gDisplaySurface->height();
    }
    fprintf(stderr, "[SDL-STUB] SDL_CreateWindow('%s', %dx%d, flags=0x%x)\n",
            title ? title : "(null)", w, h, flags);
    void* handle = reinterpret_cast<void*>(sNextHandle++);
    SDLWindowState state;
    state.width = w;
    state.height = h;
    state.title = title ? title : "";
    sWindows[handle] = state;
    if (!sMainWindow) sMainWindow = handle;
    return handle;
}

void stub_SDL_DestroyWindow(void* window) {
    sWindows.erase(window);
}

void stub_SDL_GetWindowSize(void* window, int* w, int* h) {
    // For the main window, always return the display surface dimensions
    // so Pharo tracks the actual UIView size (handles resize)
    if (window == sMainWindow && pharo::gDisplaySurface) {
        if (w) *w = pharo::gDisplaySurface->width();
        if (h) *h = pharo::gDisplaySurface->height();
        static int winSizeCount = 0;
        winSizeCount++;
        if (winSizeCount <= 5 || winSizeCount % 500 == 0) {
            fprintf(stderr, "[SDL-STUB] SDL_GetWindowSize #%d (main) -> %dx%d\n",
                    winSizeCount, w ? *w : -1, h ? *h : -1);
        }
        return;
    }
    auto it = sWindows.find(window);
    if (it != sWindows.end()) {
        if (w) *w = it->second.width;
        if (h) *h = it->second.height;
    } else if (pharo::gDisplaySurface) {
        if (w) *w = pharo::gDisplaySurface->width();
        if (h) *h = pharo::gDisplaySurface->height();
    } else {
        if (w) *w = 1024;
        if (h) *h = 768;
    }
}

void stub_SDL_SetWindowSize(void* window, int w, int h) {
    auto it = sWindows.find(window);
    if (it != sWindows.end()) {
        it->second.width = w;
        it->second.height = h;
    }
}

void stub_SDL_SetWindowTitle(void* window, const char* title) {
}

void stub_SDL_ShowWindow(void* window) {
}

void stub_SDL_HideWindow(void* window) {
}

void stub_SDL_RaiseWindow(void* window) {
}

uint32_t stub_SDL_GetWindowID(void* window) {
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(window) & 0xFFFFFFFF);
}

void* stub_SDL_GetWindowFromID(uint32_t id) {
    // Find window by ID (lower 32 bits of handle)
    for (auto& kv : sWindows) {
        if ((reinterpret_cast<uintptr_t>(kv.first) & 0xFFFFFFFF) == id)
            return kv.first;
    }
    return nullptr;
}

int stub_SDL_SetWindowFullscreen(void* window, uint32_t flags) {
    return 0;
}

void stub_SDL_GetWindowPosition(void* window, int* x, int* y) {
    if (x) *x = 0;
    if (y) *y = 0;
}

void stub_SDL_SetWindowPosition(void* window, int x, int y) {
}

void stub_SDL_SetWindowIcon(void* window, void* icon) {
}

int stub_SDL_GetWindowWMInfo(void* window, void* info) {
    // Return failure — we don't have real WM info on Mac Catalyst.
    // Returning success with zeroed data causes SDLOSXPlatform>>afterSetWindowTitle:
    // to access zeroed struct fields as Smalltalk objects (classIdx=0 crash),
    // which kills the OSSDL2Driver setup process before the event loop starts.
    return 0;  // SDL_FALSE
}

// Renderer stubs
void* stub_SDL_CreateRenderer(void* window, int index, uint32_t flags) {
    void* handle = reinterpret_cast<void*>(sNextHandle++);
    SDLRendererState state;
    state.window = window;
    state.currentTexture = nullptr;
    sRenderers[handle] = state;
    // Track main renderer: prefer the renderer for the main window
    if (window == sMainWindow || !sMainRenderer) {
        sMainRenderer = handle;
    }
    fprintf(stderr, "[SDL-STUB] SDL_CreateRenderer(win=%p) -> %p (main=%p)\n",
            window, handle, sMainRenderer);
    return handle;
}

void stub_SDL_DestroyRenderer(void* renderer) {
    // Don't clear sMainRenderer if this is a secondary renderer
    if (renderer != sMainRenderer) {
        sRenderers.erase(renderer);
    }
}

int stub_SDL_RenderClear(void* renderer) {
    static int clearCount = 0;
    clearCount++;
    if (clearCount <= 5 || clearCount % 200 == 0) {
        fprintf(stderr, "[SDL-RC] RenderClear #%d renderer=%p\n", clearCount, renderer);
    }
    return 0;
}

void stub_SDL_RenderPresent(void* renderer) {
    static int totalCalls = 0;
    totalCalls++;

    // Log first call
    if (!sSDLRenderingActive) {
        sSDLRenderingActive = true;
        fprintf(stderr, "[SDL-RP] first call renderer=%p main=%p (rendering active)\n",
                renderer, sMainRenderer);
    }

    // Only copy to gDisplaySurface from the main renderer.
    // Secondary renderers (Emergency Debugger, etc.) have their own windows
    // and should not overwrite the main display.
    auto rit = sRenderers.find(renderer);
    if (rit == sRenderers.end() || !rit->second.currentTexture) {
        if (totalCalls <= 5)
            fprintf(stderr, "[SDL-RP] #%d SKIP: renderer %p not found or no texture\n", totalCalls, renderer);
        return;
    }

    // Skip non-main renderers
    if (renderer != sMainRenderer) {
        static int skipCount = 0;
        if (++skipCount <= 5) {
            fprintf(stderr, "[SDL-RP] #%d SKIP: secondary renderer %p (main=%p)\n",
                    totalCalls, renderer, sMainRenderer);
        }
        return;
    }

    auto tit = sTextures.find(rit->second.currentTexture);
    if (tit == sTextures.end() || !tit->second.pixels) {
        if (totalCalls <= 5)
            fprintf(stderr, "[SDL-RP] #%d SKIP: texture not found or no pixels\n", totalCalls);
        return;
    }

    if (pharo::gDisplaySurface) {
        uint32_t* src = tit->second.pixels;
        int srcW = tit->second.width;
        int srcH = tit->second.height;

        uint32_t* dst = pharo::gDisplaySurface->pixels();
        int dstW = pharo::gDisplaySurface->width();
        int dstH = pharo::gDisplaySurface->height();
        int copyW = std::min(srcW, dstW);
        int copyH = std::min(srcH, dstH);

        if (totalCalls <= 5 || totalCalls % 200 == 0) {
            uint32_t srcCenter = (srcH > 384 && srcW > 512) ? src[384 * srcW + 512] : 0;
            fprintf(stderr, "[SDL-RP] #%d copy %dx%d -> %dx%d srcCenter=%08x\n",
                    totalCalls, srcW, srcH, dstW, dstH, srcCenter);
        }

        for (int y = 0; y < copyH; y++) {
            memcpy(dst + y * dstW, src + y * srcW, copyW * sizeof(uint32_t));
        }

        pharo::gDisplaySurface->update();
    }
}

int stub_SDL_GetRendererOutputSize(void* renderer, int* w, int* h) {
    // Return display surface dimensions so Pharo creates Forms at the right size
    int rw = 1024, rh = 768;
    if (pharo::gDisplaySurface) {
        rw = pharo::gDisplaySurface->width();
        rh = pharo::gDisplaySurface->height();
    }
    if (w) *w = rw;
    if (h) *h = rh;
    static int outputSizeCount = 0;
    outputSizeCount++;
    if (outputSizeCount <= 5 || outputSizeCount % 500 == 0) {
        fprintf(stderr, "[SDL-STUB] SDL_GetRendererOutputSize #%d (renderer=%p) -> %dx%d\n",
                outputSizeCount, renderer, rw, rh);
    }
    return 0;
}

int stub_SDL_SetRenderDrawColor(void* renderer, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return 0;
}

// Texture stubs
void* stub_SDL_CreateTexture(void* renderer, uint32_t format, int access, int w, int h) {
    // DO NOT override dimensions! Pharo's Form uses the same extent for BitBlt stride.
    // If we change the texture size without Pharo knowing, the stride mismatches and
    // rendering appears as diagonal garbage.
    fprintf(stderr, "[SDL-STUB] SDL_CreateTexture(%dx%d, fmt=0x%x, access=%d, renderer=%p)\n",
            w, h, format, access, renderer);
    void* handle = reinterpret_cast<void*>(sNextHandle++);
    SDLTextureState state;
    state.pixels = static_cast<uint32_t*>(calloc(w * h, 4));
    state.width = w;
    state.height = h;
    state.pitch = w * 4;  // 32bpp XRGB
    state.renderer = renderer;
    sTextures[handle] = state;
    // Track as current texture for this renderer
    auto rit = sRenderers.find(renderer);
    if (rit != sRenderers.end()) {
        rit->second.currentTexture = handle;
    }
    return handle;
}

void stub_SDL_DestroyTexture(void* texture) {
    auto it = sTextures.find(texture);
    if (it != sTextures.end()) {
        if (it->second.pixels) {
            free(it->second.pixels);
        }
        // Clear currentTexture reference from the renderer
        auto rit = sRenderers.find(it->second.renderer);
        if (rit != sRenderers.end() && rit->second.currentTexture == texture) {
            rit->second.currentTexture = nullptr;
        }
        sTextures.erase(it);
    }
}

int stub_SDL_LockTexture(void* texture, void* rect, void** pixels, int* pitch) {
    auto it = sTextures.find(texture);
    if (it == sTextures.end() || !it->second.pixels) {
        return -1;
    }
    if (pixels) {
        *pixels = it->second.pixels;
    }
    if (pitch) {
        *pitch = it->second.pitch;
    }
    static int lockCount = 0;
    lockCount++;
    if (lockCount <= 5 || lockCount % 200 == 0) {
        fprintf(stderr, "[SDL-LOCK] #%d texture=%p pixels=%p pitch=%d (%dx%d)\n",
                lockCount, texture, it->second.pixels, it->second.pitch,
                it->second.width, it->second.height);
    }
    return 0;
}

void stub_SDL_UnlockTexture(void* texture) {
}

int stub_SDL_RenderCopy(void* renderer, void* texture, void* srcrect, void* dstrect) {
    static int copyCount = 0;
    copyCount++;
    // Track which texture was last rendered
    auto rit = sRenderers.find(renderer);
    if (rit != sRenderers.end()) {
        rit->second.currentTexture = texture;
    }
    if (copyCount <= 5 || copyCount % 200 == 0) {
        fprintf(stderr, "[SDL-RCOPY] #%d renderer=%p texture=%p\n", copyCount, renderer, texture);
    }
    return 0;
}

int stub_SDL_UpdateTexture(void* texture, void* rect, void* pixels, int pitch) {
    auto it = sTextures.find(texture);
    if (it == sTextures.end() || !it->second.pixels || !pixels) {
        return -1;
    }

    static int updateCount = 0;
    updateCount++;

    int texW = it->second.width;
    int texH = it->second.height;
    int texPitch = it->second.pitch;  // bytes per row

    if (!rect) {
        // Full texture update
        int srcBytesPerRow = pitch;
        int dstBytesPerRow = texPitch;
        int copyBytes = std::min(srcBytesPerRow, dstBytesPerRow);
        uint8_t* src = static_cast<uint8_t*>(pixels);
        uint8_t* dst = reinterpret_cast<uint8_t*>(it->second.pixels);

        for (int y = 0; y < texH; y++) {
            memcpy(dst + y * dstBytesPerRow, src + y * srcBytesPerRow, copyBytes);
        }

        if (updateCount <= 5 || updateCount % 200 == 0) {
            uint32_t* srcPx = static_cast<uint32_t*>(pixels);
            uint32_t center = (texH > 384 && texW > 512) ? srcPx[384 * (pitch/4) + 512] : 0;
            fprintf(stderr, "[SDL-UT] #%d full update: %dx%d pitch=%d center=%08x\n",
                    updateCount, texW, texH, pitch, center);
        }
    } else {
        // Partial rect update (SDL_Rect = {x, y, w, h})
        int* r = static_cast<int*>(rect);
        int rx = r[0], ry = r[1], rw = r[2], rh = r[3];
        int srcBytesPerRow = pitch;
        int dstBytesPerRow = texPitch;
        int copyBytes = std::min(rw * 4, std::min(srcBytesPerRow, dstBytesPerRow));
        uint8_t* src = static_cast<uint8_t*>(pixels);
        uint8_t* dst = reinterpret_cast<uint8_t*>(it->second.pixels);

        for (int y = 0; y < rh && (ry + y) < texH; y++) {
            memcpy(dst + (ry + y) * dstBytesPerRow + rx * 4,
                   src + y * srcBytesPerRow,
                   copyBytes);
        }

        if (updateCount <= 5 || updateCount % 200 == 0) {
            fprintf(stderr, "[SDL-UT] #%d rect update: (%d,%d,%d,%d) pitch=%d\n",
                    updateCount, rx, ry, rw, rh, pitch);
        }
    }
    return 0;
}

// Event stubs - critical for InputEventSensor
// Forward events from our queue to SDL event structure
int stub_SDL_PollEvent(void* event) {
    static bool flagSet = false;

    // Mark SDL2 event polling as active - this prevents processInputEvents
    // from draining gEventQueue (we handle it here instead)
    static int totalPollCalls = 0;
    totalPollCalls++;

    if (!flagSet) {
        flagSet = true;
        pharo::gEventQueue.setSDL2EventPollingActive(true);
        fprintf(stderr, "[SDL-STUB] SDL_PollEvent: first call, event ptr=%p\n", event);
    }

    // Log the first few calls and periodic calls to track pointer stability
    if (totalPollCalls <= 5 || totalPollCalls % 5000 == 0) {
        fprintf(stderr, "[SDL-POLL] #%d event ptr=%p queueSize=%zu\n",
                totalPollCalls, event, pharo::gEventQueue.size());
    }

    // Monitor gDisplaySurface for unexpected content changes
    if (pharo::gDisplaySurface && (totalPollCalls % 1000 == 0)) {
        uint32_t* px = pharo::gDisplaySurface->pixels();
        int w = pharo::gDisplaySurface->width();
        int h = pharo::gDisplaySurface->height();
        uint32_t center = (h > 384 && w > 512) ? px[384 * w + 512] : px[0];
        static uint32_t lastCenter = 0;
        if (center != lastCenter) {
            fprintf(stderr, "[SURF-MON] poll#%d surface center changed: %08x -> %08x\n",
                    totalPollCalls, lastCenter, center);
            lastCenter = center;
        }
    }

    // Validate event pointer - FFI may pass stale heap address after GC compaction
    if (!event || reinterpret_cast<uintptr_t>(event) < 0x10000) {
        static int badPtrCount = 0;
        badPtrCount++;
        if (badPtrCount <= 5) {
            fprintf(stderr, "[SDL-STUB] SDL_PollEvent: bad event ptr=%p (count=%d)\n", event, badPtrCount);
        }
        return !pharo::gEventQueue.isEmpty() ? 1 : 0;
    }

#ifdef __APPLE__
    // Check if the event pointer's page is actually mapped in memory.
    // After GC compaction, ByteArray data pointers can become stale if the
    // FFI argument resolution doesn't properly follow forwarding pointers.
    {
        char vec;
        caddr_t pageAddr = reinterpret_cast<caddr_t>(reinterpret_cast<uintptr_t>(event) & ~0xFFFUL);
        if (mincore(pageAddr, 4096, &vec) != 0) {
            static int unmappedCount = 0;
            unmappedCount++;
            if (unmappedCount <= 10) {
                fprintf(stderr, "[SDL-STUB] SDL_PollEvent: UNMAPPED event ptr=%p page=%p (count=%d)\n",
                        event, pageAddr, unmappedCount);
            }
            // Don't write to unmapped memory - just drain queue and return 0
            pharo::Event discard;
            pharo::gEventQueue.pop(discard);  // discard one event if available
#ifdef __APPLE__
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.001, true);
#endif
            return 0;
        }
    }
#endif

    // Deliver pending synthetic window events one at a time
    if (!sPendingWindowEvents.empty()) {
        uint8_t windowEventType = sPendingWindowEvents.front();
        sPendingWindowEvents.pop();
        SDL_Event* sdlEvent = reinterpret_cast<SDL_Event*>(event);
        memset(sdlEvent, 0, sizeof(SDL_Event));
        uint32_t windowID = sMainWindow ? stub_SDL_GetWindowID(sMainWindow) : 1;
        sdlEvent->window.type = SDL_WINDOWEVENT;
        sdlEvent->window.timestamp = 0;
        sdlEvent->window.windowID = windowID;
        sdlEvent->window.event = windowEventType;
        fprintf(stderr, "[SDL-PE] Delivering synthetic window event type=%d at poll#%d\n",
                windowEventType, totalPollCalls);
        return 1;
    }

    // After startup fully settles (~2min of polling), send SHOWN + EXPOSED + FOCUS_GAINED
    // to trigger the initial full repaint. Real SDL2 sends these when the window
    // first becomes visible. We delay because the event handler must be set up first
    // and the Emergency Debugger cascade must complete.
    if (sMainWindow && totalPollCalls == 10000) {
        sPendingWindowEvents.push(SDL_WINDOWEVENT_SHOWN);
        sPendingWindowEvents.push(SDL_WINDOWEVENT_EXPOSED);
        sPendingWindowEvents.push(SDL_WINDOWEVENT_FOCUS_GAINED);
        fprintf(stderr, "[SDL-PE] Queuing deferred SHOWN+EXPOSED+FOCUS at poll#%d\n", totalPollCalls);
    }

    // Pop event from our queue
    pharo::Event pharoEvent;
    if (!pharo::gEventQueue.pop(pharoEvent)) {
        // No events available. Return 0 quickly so the event loop process
        // yields via Processor yield → relinquishProcessor, which pumps
        // CFRunLoop via the relinquish callback. DO NOT call
        // CFRunLoopRunInMode here — the event loop process tight-loops on
        // PollEvent at priority 60, and blocking 1ms per call starves the
        // interpreter and all lower-priority processes.
        return 0;
    }

    // Log every event pop (first 20 + periodic)
    static int popCount = 0;
    popCount++;
    if (popCount <= 20 || popCount % 100 == 0) {
        fprintf(stderr, "[SDL-POP] #%d event type=%d arg1=%d arg2=%d arg3=%d arg5=%d\n",
                popCount, pharoEvent.type, pharoEvent.arg1, pharoEvent.arg2,
                pharoEvent.arg3, pharoEvent.arg5);
    }

    SDL_Event* sdlEvent = reinterpret_cast<SDL_Event*>(event);
    memset(sdlEvent, 0, sizeof(SDL_Event));

    // Use the main window's ID so OSSDL2BackendWindow accepts the event
    uint32_t windowID = sMainWindow ? stub_SDL_GetWindowID(sMainWindow) : 1;

    // Convert Pharo event to SDL event
    if (pharoEvent.type == static_cast<int>(pharo::EventType::Mouse)) {
        // pharoEvent.arg5 is the mouse event subtype:
        // 0 = move, 1 = down, 2 = up, 3 = drag (move with button held)
        int subtype = pharoEvent.arg5;
        fprintf(stderr, "[SDL-EVT] Mouse event: subtype=%d x=%d y=%d buttons=%d\n",
                subtype, pharoEvent.arg1, pharoEvent.arg2, pharoEvent.arg3);

        // Update tracked mouse position
        sMouseX = pharoEvent.arg1;
        sMouseY = pharoEvent.arg2;

        // Convert Pharo button mask to SDL button mask for state tracking
        uint32_t sdlButtonMask = 0;
        if (pharoEvent.arg3 & 4) sdlButtonMask |= (1 << 0);  // Left/Red → SDL_BUTTON_LMASK
        if (pharoEvent.arg3 & 1) sdlButtonMask |= (1 << 1);  // Middle/Blue → SDL_BUTTON_MMASK
        if (pharoEvent.arg3 & 2) sdlButtonMask |= (1 << 2);  // Right/Yellow → SDL_BUTTON_RMASK

        if (subtype == 0 || subtype == 3) {
            // Mouse motion (move or drag)
            sdlEvent->motion.type = SDL_MOUSEMOTION;
            sdlEvent->motion.timestamp = pharoEvent.timeStamp;
            sdlEvent->motion.windowID = windowID;
            sdlEvent->motion.which = 0;  // Touch or mouse
            sdlEvent->motion.x = pharoEvent.arg1;
            sdlEvent->motion.y = pharoEvent.arg2;
            sdlEvent->motion.state = sdlButtonMask;
        } else if (subtype == 1) {
            // Mouse button down — update tracked state
            sMouseButtons |= sdlButtonMask;

            sdlEvent->button.type = SDL_MOUSEBUTTONDOWN;
            sdlEvent->button.timestamp = pharoEvent.timeStamp;
            sdlEvent->button.windowID = windowID;
            sdlEvent->button.which = 0;
            sdlEvent->button.x = pharoEvent.arg1;
            sdlEvent->button.y = pharoEvent.arg2;
            sdlEvent->button.state = 1;  // SDL_PRESSED
            sdlEvent->button.clicks = 1;
            // Convert Pharo button to SDL button
            if (pharoEvent.arg3 & 4) {
                sdlEvent->button.button = SDL_BUTTON_LEFT;
            } else if (pharoEvent.arg3 & 2) {
                sdlEvent->button.button = SDL_BUTTON_RIGHT;
            } else if (pharoEvent.arg3 & 1) {
                sdlEvent->button.button = SDL_BUTTON_MIDDLE;
            } else {
                sdlEvent->button.button = SDL_BUTTON_LEFT;  // Default
            }
        } else if (subtype == 2) {
            // Mouse button up — update tracked state
            sMouseButtons &= ~sdlButtonMask;

            sdlEvent->button.type = SDL_MOUSEBUTTONUP;
            sdlEvent->button.timestamp = pharoEvent.timeStamp;
            sdlEvent->button.windowID = windowID;
            sdlEvent->button.which = 0;
            sdlEvent->button.x = pharoEvent.arg1;
            sdlEvent->button.y = pharoEvent.arg2;
            sdlEvent->button.state = 0;  // SDL_RELEASED
            sdlEvent->button.clicks = 1;
            // Convert Pharo button to SDL button
            if (pharoEvent.arg3 & 4) {
                sdlEvent->button.button = SDL_BUTTON_LEFT;
            } else if (pharoEvent.arg3 & 2) {
                sdlEvent->button.button = SDL_BUTTON_RIGHT;
            } else if (pharoEvent.arg3 & 1) {
                sdlEvent->button.button = SDL_BUTTON_MIDDLE;
            } else {
                sdlEvent->button.button = SDL_BUTTON_LEFT;  // Default
            }
        }
        return 1;  // Event available
    } else if (pharoEvent.type == static_cast<int>(pharo::EventType::MouseWheel)) {
        // Mouse wheel
        sdlEvent->wheel.type = SDL_MOUSEWHEEL;
        sdlEvent->wheel.timestamp = pharoEvent.timeStamp;
        sdlEvent->wheel.windowID = windowID;
        sdlEvent->wheel.which = 0;
        sdlEvent->wheel.x = pharoEvent.arg3;  // Horizontal scroll (deltaX)
        sdlEvent->wheel.y = pharoEvent.arg4;  // Vertical scroll (deltaY)
        sdlEvent->wheel.direction = 0;  // Normal
        return 1;
    } else if (pharoEvent.type == static_cast<int>(pharo::EventType::Keyboard)) {
        // Keyboard event
        // Pharo keyboard event layout:
        //   arg1 = charCode, arg2 = subtype (0=down, 1=up, 2=keystroke)
        //   arg3 = modifiers, arg4 = keyCode (scancode)
        int subtype = pharoEvent.arg2;
        if (subtype == 0 || subtype == 2) {
            sdlEvent->key.type = SDL_KEYDOWN;
            sdlEvent->key.state = 1;  // SDL_PRESSED
        } else {
            sdlEvent->key.type = SDL_KEYUP;
            sdlEvent->key.state = 0;  // SDL_RELEASED
        }
        sdlEvent->key.timestamp = pharoEvent.timeStamp;
        sdlEvent->key.windowID = windowID;
        sdlEvent->key.repeat = 0;
        sdlEvent->key.keysym.scancode = pharoEvent.arg4;  // keyCode
        sdlEvent->key.keysym.sym = pharoEvent.arg1;       // charCode
        // Convert Pharo modifiers to SDL modifiers
        uint16_t sdlMod = 0;
        if (pharoEvent.arg3 & 1) sdlMod |= 0x0001;  // KMOD_LSHIFT
        if (pharoEvent.arg3 & 2) sdlMod |= 0x0040;  // KMOD_LCTRL
        if (pharoEvent.arg3 & 4) sdlMod |= 0x0100;  // KMOD_LALT
        if (pharoEvent.arg3 & 8) sdlMod |= 0x0400;  // KMOD_LGUI (Cmd)
        sdlEvent->key.keysym.mod = sdlMod;
        return 1;
    } else if (pharoEvent.type == static_cast<int>(pharo::EventType::WindowMetrics)) {
        // Convert to SDL_WINDOWEVENT so OSSDL2Driver can handle resize
        sdlEvent->window.type = SDL_WINDOWEVENT;
        sdlEvent->window.timestamp = pharoEvent.timeStamp;
        sdlEvent->window.windowID = windowID;
        sdlEvent->window.event = SDL_WINDOWEVENT_SIZE_CHANGED;
        sdlEvent->window.data1 = pharoEvent.arg1;  // width
        sdlEvent->window.data2 = pharoEvent.arg2;  // height
        return 1;
    }

    // Unknown event type - skip
    return 0;
}

int stub_SDL_WaitEvent(void* event) {
    // Return 0 = no event available
    return 0;
}

int stub_SDL_PushEvent(void* event) {
    return 1;  // Success
}

// Clipboard stubs
char* stub_SDL_GetClipboardText() {
    return strdup("");
}

int stub_SDL_SetClipboardText(const char* text) {
    return 0;
}

int stub_SDL_HasClipboardText() {
    return 0;
}

// Cursor stubs
void* stub_SDL_CreateSystemCursor(int id) {
    return reinterpret_cast<void*>(static_cast<uintptr_t>(0x20000 + id));
}

void* stub_SDL_CreateCursor(void* data, void* mask, int w, int h, int hot_x, int hot_y) {
    // Return a unique handle for each custom cursor
    static uintptr_t nextCursorHandle = 0x30000;
    void* handle = reinterpret_cast<void*>(nextCursorHandle++);
    fprintf(stderr, "[SDL-STUB] SDL_CreateCursor(%dx%d, hot=%d,%d) -> %p\n",
            w, h, hot_x, hot_y, handle);
    return handle;
}

void stub_SDL_SetCursor(void* cursor) {
}

void stub_SDL_FreeCursor(void* cursor) {
}

int stub_SDL_ShowCursor(int toggle) {
    return toggle;
}

uint32_t stub_SDL_GetWindowFlags(void* window) {
    // Return SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    return 0x00000004 | 0x00000020;
}

void* stub_SDL_CreateRGBSurfaceFrom(void* pixels, int width, int height, int depth,
                                      int pitch, uint32_t Rmask, uint32_t Gmask,
                                      uint32_t Bmask, uint32_t Amask) {
    // Return a handle - surfaces are mainly used for cursor and icon creation
    static uintptr_t nextSurfHandle = 0x40000;
    void* handle = reinterpret_cast<void*>(nextSurfHandle++);
    fprintf(stderr, "[SDL-STUB] SDL_CreateRGBSurfaceFrom(%dx%d depth=%d) -> %p\n",
            width, height, depth, handle);
    return handle;
}

void stub_SDL_FreeSurface(void* surface) {
}

uint32_t stub_SDL_GetMouseState(int* x, int* y) {
    static int callCount = 0;
    callCount++;
    if (callCount <= 10 || callCount % 5000 == 0) {
        fprintf(stderr, "[SDL-STATE] GetMouseState #%d: x=%d y=%d buttons=0x%x\n",
                callCount, sMouseX, sMouseY, sMouseButtons);
    }
    if (x) *x = sMouseX;
    if (y) *y = sMouseY;
    return sMouseButtons;
}

uint32_t stub_SDL_GetGlobalMouseState(int* x, int* y) {
    if (x) *x = sMouseX;
    if (y) *y = sMouseY;
    return sMouseButtons;
}

uint32_t stub_SDL_GetModState() {
    static int callCount = 0;
    callCount++;
    if (callCount <= 10 || callCount % 5000 == 0) {
        fprintf(stderr, "[SDL-STATE] GetModState #%d: modState=0x%x\n",
                callCount, sKeyModState);
    }
    return sKeyModState;
}

void stub_SDL_SetModState(uint32_t state) {
    sKeyModState = state;
}

// Video subsystem stubs
int stub_SDL_GetNumVideoDisplays() {
    return 1;
}

int stub_SDL_GetDisplayBounds(int displayIndex, void* rect) {
    if (rect) {
        int* r = static_cast<int*>(rect);
        r[0] = 0;     // x
        r[1] = 0;     // y
        if (pharo::gDisplaySurface) {
            r[2] = pharo::gDisplaySurface->width();
            r[3] = pharo::gDisplaySurface->height();
        } else {
            r[2] = 1024;
            r[3] = 768;
        }
    }
    return 0;
}

// Timer stubs
uint32_t stub_SDL_GetTicks() {
    static auto start = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count()
    );
}

uint64_t stub_SDL_GetPerformanceCounter() {
    return std::chrono::steady_clock::now().time_since_epoch().count();
}

uint64_t stub_SDL_GetPerformanceFrequency() {
    return 1000000000ULL;  // nanoseconds
}

// Additional commonly needed SDL2 functions
uint32_t stub_SDL_WasInit(uint32_t flags) {
    return sSDL2Initialized ? flags : 0;
}

int stub_SDL_VideoInit(const char* driver) {
    return 0;
}

void stub_SDL_VideoQuit() {
}

int stub_SDL_InitSubSystem(uint32_t flags) {
    return 0;
}

void stub_SDL_QuitSubSystem(uint32_t flags) {
}

int stub_SDL_SetHint(const char* name, const char* value) {
    return 1;
}

int stub_SDL_GL_SetAttribute(int attr, int value) {
    return 0;
}

void* stub_SDL_GL_CreateContext(void* window) {
    void* handle = reinterpret_cast<void*>(sNextHandle++);
    return handle;
}

void stub_SDL_GL_DeleteContext(void* context) {
}

int stub_SDL_GL_MakeCurrent(void* window, void* context) {
    return 0;
}

void stub_SDL_GL_SwapWindow(void* window) {
    // No-op
}

// Real SDL function names as aliases to stubs
// These are what dlsym will find when Pharo's FFI looks for SDL2 functions
// Using weak symbols so we don't conflict if real SDL2 is ever linked
// Using 'used' attribute to prevent linker from stripping them
// Using 'visibility("default")' to ensure they're exported to dynamic symbol table

#define SDL_EXPORT __attribute__((weak, used, visibility("default")))

SDL_EXPORT int SDL_Init(uint32_t flags) { return stub_SDL_Init(flags); }
SDL_EXPORT void SDL_Quit() { stub_SDL_Quit(); }
SDL_EXPORT void SDL_GetVersion(void* ver) { stub_SDL_GetVersion(ver); }
SDL_EXPORT const char* SDL_GetError() { return stub_SDL_GetError(); }
SDL_EXPORT void* SDL_CreateWindow(const char* t, int x, int y, int w, int h, uint32_t f) { return stub_SDL_CreateWindow(t, x, y, w, h, f); }
SDL_EXPORT void SDL_DestroyWindow(void* w) { stub_SDL_DestroyWindow(w); }
SDL_EXPORT void SDL_GetWindowSize(void* w, int* x, int* y) { stub_SDL_GetWindowSize(w, x, y); }
SDL_EXPORT void SDL_SetWindowSize(void* w, int x, int y) { stub_SDL_SetWindowSize(w, x, y); }
SDL_EXPORT void SDL_SetWindowTitle(void* w, const char* t) { stub_SDL_SetWindowTitle(w, t); }
SDL_EXPORT void SDL_ShowWindow(void* w) { stub_SDL_ShowWindow(w); }
SDL_EXPORT void SDL_HideWindow(void* w) { stub_SDL_HideWindow(w); }
SDL_EXPORT void SDL_RaiseWindow(void* w) { stub_SDL_RaiseWindow(w); }
SDL_EXPORT uint32_t SDL_GetWindowID(void* w) { return stub_SDL_GetWindowID(w); }
SDL_EXPORT void* SDL_GetWindowFromID(uint32_t id) { return stub_SDL_GetWindowFromID(id); }
SDL_EXPORT int SDL_SetWindowFullscreen(void* w, uint32_t f) { return stub_SDL_SetWindowFullscreen(w, f); }
SDL_EXPORT void SDL_GetWindowPosition(void* w, int* x, int* y) { stub_SDL_GetWindowPosition(w, x, y); }
SDL_EXPORT void SDL_SetWindowPosition(void* w, int x, int y) { stub_SDL_SetWindowPosition(w, x, y); }
SDL_EXPORT void SDL_SetWindowIcon(void* w, void* icon) { stub_SDL_SetWindowIcon(w, icon); }
SDL_EXPORT int SDL_GetWindowWMInfo(void* w, void* info) { return stub_SDL_GetWindowWMInfo(w, info); }
SDL_EXPORT void* SDL_CreateRenderer(void* w, int i, uint32_t f) { return stub_SDL_CreateRenderer(w, i, f); }
SDL_EXPORT void SDL_DestroyRenderer(void* r) { stub_SDL_DestroyRenderer(r); }
SDL_EXPORT int SDL_RenderClear(void* r) { return stub_SDL_RenderClear(r); }
SDL_EXPORT void SDL_RenderPresent(void* r) { stub_SDL_RenderPresent(r); }
SDL_EXPORT int SDL_GetRendererOutputSize(void* r, int* w, int* h) { return stub_SDL_GetRendererOutputSize(r, w, h); }
SDL_EXPORT int SDL_SetRenderDrawColor(void* r, uint8_t rr, uint8_t g, uint8_t b, uint8_t a) { return stub_SDL_SetRenderDrawColor(r, rr, g, b, a); }
SDL_EXPORT void* SDL_CreateTexture(void* r, uint32_t fmt, int acc, int w, int h) { return stub_SDL_CreateTexture(r, fmt, acc, w, h); }
SDL_EXPORT void SDL_DestroyTexture(void* t) { stub_SDL_DestroyTexture(t); }
SDL_EXPORT int SDL_LockTexture(void* t, void* rect, void** px, int* pitch) { return stub_SDL_LockTexture(t, rect, px, pitch); }
SDL_EXPORT void SDL_UnlockTexture(void* t) { stub_SDL_UnlockTexture(t); }
SDL_EXPORT int SDL_RenderCopy(void* r, void* t, void* sr, void* dr) { return stub_SDL_RenderCopy(r, t, sr, dr); }
SDL_EXPORT int SDL_UpdateTexture(void* t, void* r, void* px, int p) { return stub_SDL_UpdateTexture(t, r, px, p); }
SDL_EXPORT int SDL_PollEvent(void* e) { return stub_SDL_PollEvent(e); }
SDL_EXPORT int SDL_WaitEvent(void* e) { return stub_SDL_WaitEvent(e); }
SDL_EXPORT int SDL_PushEvent(void* e) { return stub_SDL_PushEvent(e); }
SDL_EXPORT char* SDL_GetClipboardText() { return stub_SDL_GetClipboardText(); }
SDL_EXPORT int SDL_SetClipboardText(const char* t) { return stub_SDL_SetClipboardText(t); }
SDL_EXPORT int SDL_HasClipboardText() { return stub_SDL_HasClipboardText(); }
SDL_EXPORT void* SDL_CreateSystemCursor(int id) { return stub_SDL_CreateSystemCursor(id); }
SDL_EXPORT void* SDL_CreateCursor(void* data, void* mask, int w, int h, int hx, int hy) { return stub_SDL_CreateCursor(data, mask, w, h, hx, hy); }
SDL_EXPORT void SDL_SetCursor(void* c) { stub_SDL_SetCursor(c); }
SDL_EXPORT void SDL_FreeCursor(void* c) { stub_SDL_FreeCursor(c); }
SDL_EXPORT int SDL_ShowCursor(int t) { return stub_SDL_ShowCursor(t); }
SDL_EXPORT uint32_t SDL_GetWindowFlags(void* w) { return stub_SDL_GetWindowFlags(w); }
SDL_EXPORT void* SDL_CreateRGBSurfaceFrom(void* px, int w, int h, int d, int p, uint32_t rm, uint32_t gm, uint32_t bm, uint32_t am) { return stub_SDL_CreateRGBSurfaceFrom(px, w, h, d, p, rm, gm, bm, am); }
SDL_EXPORT void SDL_FreeSurface(void* s) { stub_SDL_FreeSurface(s); }
SDL_EXPORT uint32_t SDL_GetMouseState(int* x, int* y) { return stub_SDL_GetMouseState(x, y); }
SDL_EXPORT uint32_t SDL_GetGlobalMouseState(int* x, int* y) { return stub_SDL_GetGlobalMouseState(x, y); }
SDL_EXPORT uint32_t SDL_GetModState() { return stub_SDL_GetModState(); }
SDL_EXPORT void SDL_SetModState(uint32_t state) { stub_SDL_SetModState(state); }
SDL_EXPORT int SDL_GetNumVideoDisplays() { return stub_SDL_GetNumVideoDisplays(); }
SDL_EXPORT int SDL_GetDisplayBounds(int d, void* r) { return stub_SDL_GetDisplayBounds(d, r); }
SDL_EXPORT uint32_t SDL_GetTicks() { return stub_SDL_GetTicks(); }
SDL_EXPORT uint64_t SDL_GetPerformanceCounter() { return stub_SDL_GetPerformanceCounter(); }
SDL_EXPORT uint64_t SDL_GetPerformanceFrequency() { return stub_SDL_GetPerformanceFrequency(); }
SDL_EXPORT uint32_t SDL_WasInit(uint32_t f) { return stub_SDL_WasInit(f); }
SDL_EXPORT int SDL_VideoInit(const char* d) { return stub_SDL_VideoInit(d); }
SDL_EXPORT void SDL_VideoQuit() { stub_SDL_VideoQuit(); }
SDL_EXPORT int SDL_InitSubSystem(uint32_t f) { return stub_SDL_InitSubSystem(f); }
SDL_EXPORT void SDL_QuitSubSystem(uint32_t f) { stub_SDL_QuitSubSystem(f); }
SDL_EXPORT int SDL_SetHint(const char* n, const char* v) { return stub_SDL_SetHint(n, v); }
SDL_EXPORT int SDL_GL_SetAttribute(int a, int v) { return stub_SDL_GL_SetAttribute(a, v); }
SDL_EXPORT void* SDL_GL_CreateContext(void* w) { return stub_SDL_GL_CreateContext(w); }
SDL_EXPORT void SDL_GL_DeleteContext(void* c) { stub_SDL_GL_DeleteContext(c); }
SDL_EXPORT int SDL_GL_MakeCurrent(void* w, void* c) { return stub_SDL_GL_MakeCurrent(w, c); }
SDL_EXPORT void SDL_GL_SwapWindow(void* w) { stub_SDL_GL_SwapWindow(w); }

// ==== VM FFI Primitives Export ====
// These are exported so TFFIBackend can detect FFI availability via dlsym
// TFFIBackend checks: loadSymbol: #primitiveLoadSymbolFromModule module: nil
// The actual primitive implementations are in Primitives.cpp

// Stub functions that TFFIBackend can find to confirm FFI is available
// The actual functionality goes through the VM's primitive dispatch
__attribute__((used, visibility("default")))
void* primitiveLoadSymbolFromModule(void* symbol, void* module) {
    // This is just a marker function so TFFIBackend can detect FFI support
    // Actual primitive is implemented in Interpreter::primitiveLoadSymbolFromModule
    return nullptr;
}

__attribute__((used, visibility("default")))
void* primitiveLoadModule(void* moduleName) {
    // Marker function for TFFIBackend
    return nullptr;
}

#undef SDL_EXPORT

} // extern "C"

void registerSDL2Stubs() {
    // Core initialization
    registerFunction("SDL_Init", reinterpret_cast<void*>(stub_SDL_Init));
    registerFunction("SDL_Quit", reinterpret_cast<void*>(stub_SDL_Quit));
    registerFunction("SDL_GetVersion", reinterpret_cast<void*>(stub_SDL_GetVersion));
    registerFunction("SDL_GetError", reinterpret_cast<void*>(stub_SDL_GetError));

    // Window management
    registerFunction("SDL_CreateWindow", reinterpret_cast<void*>(stub_SDL_CreateWindow));
    registerFunction("SDL_DestroyWindow", reinterpret_cast<void*>(stub_SDL_DestroyWindow));
    registerFunction("SDL_GetWindowSize", reinterpret_cast<void*>(stub_SDL_GetWindowSize));
    registerFunction("SDL_SetWindowSize", reinterpret_cast<void*>(stub_SDL_SetWindowSize));
    registerFunction("SDL_SetWindowTitle", reinterpret_cast<void*>(stub_SDL_SetWindowTitle));
    registerFunction("SDL_ShowWindow", reinterpret_cast<void*>(stub_SDL_ShowWindow));
    registerFunction("SDL_HideWindow", reinterpret_cast<void*>(stub_SDL_HideWindow));
    registerFunction("SDL_RaiseWindow", reinterpret_cast<void*>(stub_SDL_RaiseWindow));
    registerFunction("SDL_GetWindowID", reinterpret_cast<void*>(stub_SDL_GetWindowID));
    registerFunction("SDL_GetWindowFromID", reinterpret_cast<void*>(stub_SDL_GetWindowFromID));
    registerFunction("SDL_SetWindowFullscreen", reinterpret_cast<void*>(stub_SDL_SetWindowFullscreen));
    registerFunction("SDL_GetWindowPosition", reinterpret_cast<void*>(stub_SDL_GetWindowPosition));
    registerFunction("SDL_SetWindowPosition", reinterpret_cast<void*>(stub_SDL_SetWindowPosition));
    registerFunction("SDL_SetWindowIcon", reinterpret_cast<void*>(stub_SDL_SetWindowIcon));
    registerFunction("SDL_GetWindowWMInfo", reinterpret_cast<void*>(stub_SDL_GetWindowWMInfo));

    // Renderer
    registerFunction("SDL_CreateRenderer", reinterpret_cast<void*>(stub_SDL_CreateRenderer));
    registerFunction("SDL_DestroyRenderer", reinterpret_cast<void*>(stub_SDL_DestroyRenderer));
    registerFunction("SDL_RenderClear", reinterpret_cast<void*>(stub_SDL_RenderClear));
    registerFunction("SDL_RenderPresent", reinterpret_cast<void*>(stub_SDL_RenderPresent));
    registerFunction("SDL_GetRendererOutputSize", reinterpret_cast<void*>(stub_SDL_GetRendererOutputSize));
    registerFunction("SDL_SetRenderDrawColor", reinterpret_cast<void*>(stub_SDL_SetRenderDrawColor));

    // Texture
    registerFunction("SDL_CreateTexture", reinterpret_cast<void*>(stub_SDL_CreateTexture));
    registerFunction("SDL_DestroyTexture", reinterpret_cast<void*>(stub_SDL_DestroyTexture));
    registerFunction("SDL_LockTexture", reinterpret_cast<void*>(stub_SDL_LockTexture));
    registerFunction("SDL_UnlockTexture", reinterpret_cast<void*>(stub_SDL_UnlockTexture));
    registerFunction("SDL_RenderCopy", reinterpret_cast<void*>(stub_SDL_RenderCopy));
    registerFunction("SDL_UpdateTexture", reinterpret_cast<void*>(stub_SDL_UpdateTexture));

    // Events
    registerFunction("SDL_PollEvent", reinterpret_cast<void*>(stub_SDL_PollEvent));
    registerFunction("SDL_WaitEvent", reinterpret_cast<void*>(stub_SDL_WaitEvent));
    registerFunction("SDL_PushEvent", reinterpret_cast<void*>(stub_SDL_PushEvent));

    // Clipboard
    registerFunction("SDL_GetClipboardText", reinterpret_cast<void*>(stub_SDL_GetClipboardText));
    registerFunction("SDL_SetClipboardText", reinterpret_cast<void*>(stub_SDL_SetClipboardText));
    registerFunction("SDL_HasClipboardText", reinterpret_cast<void*>(stub_SDL_HasClipboardText));

    // Cursor
    registerFunction("SDL_CreateSystemCursor", reinterpret_cast<void*>(stub_SDL_CreateSystemCursor));
    registerFunction("SDL_CreateCursor", reinterpret_cast<void*>(stub_SDL_CreateCursor));
    registerFunction("SDL_SetCursor", reinterpret_cast<void*>(stub_SDL_SetCursor));
    registerFunction("SDL_FreeCursor", reinterpret_cast<void*>(stub_SDL_FreeCursor));
    registerFunction("SDL_ShowCursor", reinterpret_cast<void*>(stub_SDL_ShowCursor));

    // Window flags
    registerFunction("SDL_GetWindowFlags", reinterpret_cast<void*>(stub_SDL_GetWindowFlags));

    // Surface
    registerFunction("SDL_CreateRGBSurfaceFrom", reinterpret_cast<void*>(stub_SDL_CreateRGBSurfaceFrom));
    registerFunction("SDL_FreeSurface", reinterpret_cast<void*>(stub_SDL_FreeSurface));

    // Mouse
    registerFunction("SDL_GetMouseState", reinterpret_cast<void*>(stub_SDL_GetMouseState));
    registerFunction("SDL_GetGlobalMouseState", reinterpret_cast<void*>(stub_SDL_GetGlobalMouseState));
    registerFunction("SDL_GetModState", reinterpret_cast<void*>(stub_SDL_GetModState));
    registerFunction("SDL_SetModState", reinterpret_cast<void*>(stub_SDL_SetModState));

    // Video
    registerFunction("SDL_GetNumVideoDisplays", reinterpret_cast<void*>(stub_SDL_GetNumVideoDisplays));
    registerFunction("SDL_GetDisplayBounds", reinterpret_cast<void*>(stub_SDL_GetDisplayBounds));

    // Timer
    registerFunction("SDL_GetTicks", reinterpret_cast<void*>(stub_SDL_GetTicks));
    registerFunction("SDL_GetPerformanceCounter", reinterpret_cast<void*>(stub_SDL_GetPerformanceCounter));
    registerFunction("SDL_GetPerformanceFrequency", reinterpret_cast<void*>(stub_SDL_GetPerformanceFrequency));

    // Init/subsystem (SDL_WasInit is critical for SDL2 isAvailable check)
    registerFunction("SDL_WasInit", reinterpret_cast<void*>(stub_SDL_WasInit));
    registerFunction("SDL_InitSubSystem", reinterpret_cast<void*>(stub_SDL_InitSubSystem));
    registerFunction("SDL_QuitSubSystem", reinterpret_cast<void*>(stub_SDL_QuitSubSystem));
    registerFunction("SDL_VideoInit", reinterpret_cast<void*>(stub_SDL_VideoInit));
    registerFunction("SDL_VideoQuit", reinterpret_cast<void*>(stub_SDL_VideoQuit));
    registerFunction("SDL_SetHint", reinterpret_cast<void*>(stub_SDL_SetHint));

    // OpenGL context
    registerFunction("SDL_GL_SetAttribute", reinterpret_cast<void*>(stub_SDL_GL_SetAttribute));
    registerFunction("SDL_GL_CreateContext", reinterpret_cast<void*>(stub_SDL_GL_CreateContext));
    registerFunction("SDL_GL_DeleteContext", reinterpret_cast<void*>(stub_SDL_GL_DeleteContext));
    registerFunction("SDL_GL_MakeCurrent", reinterpret_cast<void*>(stub_SDL_GL_MakeCurrent));
    registerFunction("SDL_GL_SwapWindow", reinterpret_cast<void*>(stub_SDL_GL_SwapWindow));
}

FFIType parseType(const std::string& typeName) {
    // Handle pointer types
    if (!typeName.empty() && (typeName.back() == '*' || typeName.find("*") != std::string::npos)) {
        return FFIType::Pointer;
    }

    // Basic types
    if (typeName == "void") return FFIType::Void;
    if (typeName == "bool" || typeName == "SDL_bool") return FFIType::Bool;
    if (typeName == "char" || typeName == "Sint8" || typeName == "int8") return FFIType::Int8;
    if (typeName == "short" || typeName == "Sint16" || typeName == "int16") return FFIType::Int16;
    if (typeName == "int" || typeName == "Sint32" || typeName == "int32") return FFIType::Int32;
    if (typeName == "long" || typeName == "Sint64" || typeName == "int64" || typeName == "long long") return FFIType::Int64;
    if (typeName == "uchar" || typeName == "Uint8" || typeName == "uint8" || typeName == "unsigned char") return FFIType::UInt8;
    if (typeName == "ushort" || typeName == "Uint16" || typeName == "uint16" || typeName == "unsigned short") return FFIType::UInt16;
    if (typeName == "uint" || typeName == "Uint32" || typeName == "uint32" || typeName == "unsigned int" || typeName == "unsigned") return FFIType::UInt32;
    if (typeName == "ulong" || typeName == "Uint64" || typeName == "uint64" || typeName == "unsigned long" || typeName == "size_t") return FFIType::UInt64;
    if (typeName == "float") return FFIType::Float;
    if (typeName == "double") return FFIType::Double;

    // SDL2 specific types - opaque pointers
    if (typeName == "SDL_Window" || typeName == "SDL_Renderer" ||
        typeName == "SDL_Texture" || typeName == "SDL_Surface" ||
        typeName == "SDL_Cursor" || typeName == "SDL_GLContext") {
        return FFIType::Pointer;
    }

    // SDL2 type aliases
    if (typeName == "SDL_AudioDeviceID") return FFIType::UInt32;
    if (typeName == "SDL_BlendMode" || typeName == "SDL_BlendFactor" ||
        typeName == "SDL_BlendOperation" || typeName == "SDL_WindowFlags") return FFIType::UInt32;
    if (typeName == "SDL_Keycode" || typeName == "SDL_Scancode") return FFIType::Int32;

    // Structs are passed as pointers
    if (typeName.find("SDL_") == 0) {
        return FFIType::Pointer;
    }

    return FFIType::Unknown;
}

// Convert our FFIType to libffi's ffi_type
static ffi_type* toFFIType(FFIType type) {
    switch (type) {
        case FFIType::Void:     return &ffi_type_void;
        case FFIType::Bool:     return &ffi_type_uint8;  // bool is typically 1 byte
        case FFIType::Int8:     return &ffi_type_sint8;
        case FFIType::Int16:    return &ffi_type_sint16;
        case FFIType::Int32:    return &ffi_type_sint32;
        case FFIType::Int64:    return &ffi_type_sint64;
        case FFIType::UInt8:    return &ffi_type_uint8;
        case FFIType::UInt16:   return &ffi_type_uint16;
        case FFIType::UInt32:   return &ffi_type_uint32;
        case FFIType::UInt64:   return &ffi_type_uint64;
        case FFIType::Float:    return &ffi_type_float;
        case FFIType::Double:   return &ffi_type_double;
        case FFIType::Pointer:  return &ffi_type_pointer;
        case FFIType::String:   return &ffi_type_pointer;  // Strings are char*
        default:                return &ffi_type_pointer;  // Default to pointer
    }
}

FFIResult callFunction(
    void* funcPtr,
    const std::vector<FFIType>& argTypes,
    const std::vector<uint64_t>& argValues,
    FFIType returnType
) {
    FFIResult result;
    result.success = false;
    result.intValue = 0;
    result.floatValue = 0.0;
    result.ptrValue = nullptr;
    result.type = returnType;

    if (!funcPtr) {
        result.error = "Null function pointer";
        return result;
    }

    size_t argc = argTypes.size();
    if (argc != argValues.size()) {
        result.error = "Argument count mismatch";
        return result;
    }

    // Prepare libffi types
    ffi_cif cif;
    std::vector<ffi_type*> ffiArgTypes(argc);
    std::vector<void*> ffiArgValues(argc);

    // Storage for argument values (need stable addresses)
    std::vector<uint64_t> argStorage = argValues;  // Copy to ensure stable addresses
    std::vector<void*> ptrStorage(argc);  // For pointer conversions
    std::vector<float> floatStorage(argc);  // For float conversions
    std::vector<double> doubleStorage(argc);  // For double conversions

    for (size_t i = 0; i < argc; i++) {
        ffiArgTypes[i] = toFFIType(argTypes[i]);

        // Set up argument value pointers based on type
        switch (argTypes[i]) {
            case FFIType::Float:
                floatStorage[i] = static_cast<float>(argStorage[i]);
                ffiArgValues[i] = &floatStorage[i];
                break;
            case FFIType::Double:
                doubleStorage[i] = static_cast<double>(argStorage[i]);
                ffiArgValues[i] = &doubleStorage[i];
                break;
            case FFIType::Pointer:
            case FFIType::String:
                ptrStorage[i] = reinterpret_cast<void*>(argStorage[i]);
                ffiArgValues[i] = &ptrStorage[i];
                break;
            default:
                ffiArgValues[i] = &argStorage[i];
                break;
        }
    }

    // Prepare the call interface
    ffi_type* ffiRetType = toFFIType(returnType);
    ffi_status status = ffi_prep_cif(&cif, FFI_DEFAULT_ABI,
                                      static_cast<unsigned int>(argc),
                                      ffiRetType,
                                      argc > 0 ? ffiArgTypes.data() : nullptr);

    if (status != FFI_OK) {
        result.error = "ffi_prep_cif failed: " + std::to_string(status);
        return result;
    }

    // Prepare return value storage
    union {
        uint64_t u64;
        int64_t s64;
        uint32_t u32;
        int32_t s32;
        uint16_t u16;
        int16_t s16;
        uint8_t u8;
        int8_t s8;
        float f;
        double d;
        void* ptr;
    } retValue;
    retValue.u64 = 0;

    // Make the call
    ffi_call(&cif, FFI_FN(funcPtr), &retValue,
             argc > 0 ? ffiArgValues.data() : nullptr);

    // Extract return value
    result.success = true;
    switch (returnType) {
        case FFIType::Void:
            break;
        case FFIType::Bool:
            result.intValue = retValue.u8 ? 1 : 0;
            break;
        case FFIType::Int8:
            result.intValue = static_cast<int64_t>(retValue.s8);
            break;
        case FFIType::Int16:
            result.intValue = static_cast<int64_t>(retValue.s16);
            break;
        case FFIType::Int32:
            result.intValue = static_cast<int64_t>(retValue.s32);
            break;
        case FFIType::Int64:
            result.intValue = retValue.s64;
            break;
        case FFIType::UInt8:
            result.intValue = static_cast<uint64_t>(retValue.u8);
            break;
        case FFIType::UInt16:
            result.intValue = static_cast<uint64_t>(retValue.u16);
            break;
        case FFIType::UInt32:
            result.intValue = static_cast<uint64_t>(retValue.u32);
            break;
        case FFIType::UInt64:
            result.intValue = retValue.u64;
            break;
        case FFIType::Float:
            result.floatValue = static_cast<double>(retValue.f);
            result.intValue = static_cast<uint64_t>(retValue.f);
            break;
        case FFIType::Double:
            result.floatValue = retValue.d;
            result.intValue = static_cast<uint64_t>(retValue.d);
            break;
        case FFIType::Pointer:
        case FFIType::String:
            result.ptrValue = retValue.ptr;
            result.intValue = reinterpret_cast<uint64_t>(retValue.ptr);
            break;
        default:
            result.intValue = retValue.u64;
            break;
    }

    return result;
}

} // namespace ffi
} // namespace pharo
