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
        // Trace first few SDL lookups
        static int sdlLookupCount = 0;
        if (funcName.compare(0, 4, "SDL_") == 0 && sdlLookupCount++ < 15) {
            fprintf(stderr, "[FFI-LOOKUP] %s (cached) -> %p\n", funcName.c_str(), it->second);
        }
        return it->second;
    }

    // For SDL_ functions not in our stub cache, return a generic no-op
    // instead of falling through to dlsym (which finds force-loaded real SDL2
    // that crashes on our fake 0xDEADBEEF window handles).
    if (funcName.compare(0, 4, "SDL_") == 0) {
        fprintf(stderr, "[FFI-LOOKUP] %s -> GENERIC NO-OP (not in stub cache)\n", funcName.c_str());
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

extern "C" {

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
    fprintf(stderr, "[SDL-STUB] SDL_CreateWindow('%s', %dx%d, flags=0x%x)\n",
            title ? title : "(null)", w, h, flags);
    void* handle = reinterpret_cast<void*>(sNextHandle++);
    SDLWindowState state;
    state.width = w;
    state.height = h;
    state.title = title ? title : "";
    sWindows[handle] = state;
    return handle;
}

void stub_SDL_DestroyWindow(void* window) {
    sWindows.erase(window);
}

void stub_SDL_GetWindowSize(void* window, int* w, int* h) {
    // Return the window's actual dimensions (from SDL_CreateWindow)
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

// Renderer stubs
void* stub_SDL_CreateRenderer(void* window, int index, uint32_t flags) {
    void* handle = reinterpret_cast<void*>(sNextHandle++);
    SDLRendererState state;
    state.window = window;
    state.currentTexture = nullptr;
    sRenderers[handle] = state;
    if (!sMainRenderer) {
        sMainRenderer = handle;  // First renderer is the "main" one
    }
    return handle;
}

void stub_SDL_DestroyRenderer(void* renderer) {
    // Don't clear sMainRenderer if this is a secondary renderer
    if (renderer != sMainRenderer) {
        sRenderers.erase(renderer);
    }
}

int stub_SDL_RenderClear(void* renderer) {
    return 0;
}

void stub_SDL_RenderPresent(void* renderer) {
    // Only copy main renderer's texture to the Metal display surface
    if (renderer != sMainRenderer) {
        return;
    }

    // Mark SDL2 rendering as active on first main renderer present.
    // This stops syncDisplayToSurface from overwriting the real SDL texture
    // content with the stale Display Form. OSSDL2Driver calls RenderPresent
    // BEFORE PollEvent in each frame, so we must set this here (not in PollEvent).
    static bool renderingActive = false;
    if (!renderingActive) {
        renderingActive = true;
        pharo::gEventQueue.setSDL2EventPollingActive(true);
        fprintf(stderr, "[SDL-STUB] SDL_RenderPresent: first call, sdlActive=true\n");
    }

    auto rit = sRenderers.find(renderer);
    if (rit == sRenderers.end() || !rit->second.currentTexture) return;

    auto tit = sTextures.find(rit->second.currentTexture);
    if (tit == sTextures.end() || !tit->second.pixels) return;

    if (pharo::gDisplaySurface) {
        uint32_t* src = tit->second.pixels;
        int srcW = tit->second.width;
        int srcH = tit->second.height;

        uint32_t* dst = pharo::gDisplaySurface->pixels();
        int dstW = pharo::gDisplaySurface->width();
        int dstH = pharo::gDisplaySurface->height();
        int copyW = std::min(srcW, dstW);
        int copyH = std::min(srcH, dstH);
        for (int y = 0; y < copyH; y++) {
            memcpy(dst + y * dstW, src + y * srcW, copyW * sizeof(uint32_t));
        }
        pharo::gDisplaySurface->update();
    }
}

int stub_SDL_SetRenderDrawColor(void* renderer, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return 0;
}

// Texture stubs
void* stub_SDL_CreateTexture(void* renderer, uint32_t format, int access, int w, int h) {
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
    return 0;
}

void stub_SDL_UnlockTexture(void* texture) {
}

int stub_SDL_RenderCopy(void* renderer, void* texture, void* srcrect, void* dstrect) {
    // Track which texture was last rendered
    auto rit = sRenderers.find(renderer);
    if (rit != sRenderers.end()) {
        rit->second.currentTexture = texture;
    }
    return 0;
}

int stub_SDL_UpdateTexture(void* texture, void* rect, void* pixels, int pitch) {
    return 0;
}

// Event stubs - critical for InputEventSensor
// Forward events from our queue to SDL event structure
int stub_SDL_PollEvent(void* event) {
    static bool flagSet = false;

    // Mark SDL2 event polling as active - this prevents processInputEvents
    // from draining gEventQueue (we handle it here instead)
    if (!flagSet) {
        flagSet = true;
        pharo::gEventQueue.setSDL2EventPollingActive(true);
        fprintf(stderr, "[SDL-STUB] SDL_PollEvent: first call\n");
    }

    if (!event) {
        // Just check if events available
        return !pharo::gEventQueue.isEmpty() ? 1 : 0;
    }

    // Pop event from our queue
    pharo::Event pharoEvent;
    if (!pharo::gEventQueue.pop(pharoEvent)) {
        return 0;  // No events available
    }

    SDL_Event* sdlEvent = reinterpret_cast<SDL_Event*>(event);
    memset(sdlEvent, 0, sizeof(SDL_Event));

    // Convert Pharo event to SDL event
    if (pharoEvent.type == static_cast<int>(pharo::EventType::Mouse)) {
        // pharoEvent.arg5 is the mouse event subtype:
        // 0 = move, 1 = down, 2 = up, 3 = drag (move with button held)
        int subtype = pharoEvent.arg5;

        if (subtype == 0 || subtype == 3) {
            // Mouse motion (move or drag)
            sdlEvent->motion.type = SDL_MOUSEMOTION;
            sdlEvent->motion.timestamp = pharoEvent.timeStamp;
            sdlEvent->motion.windowID = pharoEvent.windowIndex;
            sdlEvent->motion.which = 0;  // Touch or mouse
            sdlEvent->motion.x = pharoEvent.arg1;
            sdlEvent->motion.y = pharoEvent.arg2;
            // Convert Pharo button mask to SDL state
            uint32_t sdlState = 0;
            if (pharoEvent.arg3 & 4) sdlState |= (1 << 0);  // Left/Red button
            if (pharoEvent.arg3 & 1) sdlState |= (1 << 1);  // Middle/Blue
            if (pharoEvent.arg3 & 2) sdlState |= (1 << 2);  // Right/Yellow
            sdlEvent->motion.state = sdlState;
        } else if (subtype == 1) {
            // Mouse button down
            sdlEvent->button.type = SDL_MOUSEBUTTONDOWN;
            sdlEvent->button.timestamp = pharoEvent.timeStamp;
            sdlEvent->button.windowID = pharoEvent.windowIndex;
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
            // Mouse button up
            sdlEvent->button.type = SDL_MOUSEBUTTONUP;
            sdlEvent->button.timestamp = pharoEvent.timeStamp;
            sdlEvent->button.windowID = pharoEvent.windowIndex;
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
        sdlEvent->wheel.windowID = pharoEvent.windowIndex;
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
        sdlEvent->key.windowID = pharoEvent.windowIndex;
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
        // Skip window metrics events - we handle display separately
        // Recursively try to get next event
        return stub_SDL_PollEvent(event);
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
    return reinterpret_cast<void*>(0x12345678);
}

void stub_SDL_SetCursor(void* cursor) {
}

void stub_SDL_FreeCursor(void* cursor) {
}

int stub_SDL_ShowCursor(int toggle) {
    return toggle;
}

// Mouse stubs
uint32_t stub_SDL_GetMouseState(int* x, int* y) {
    if (x) *x = 0;
    if (y) *y = 0;
    return 0;
}

uint32_t stub_SDL_GetGlobalMouseState(int* x, int* y) {
    if (x) *x = 0;
    if (y) *y = 0;
    return 0;
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
SDL_EXPORT void* SDL_CreateRenderer(void* w, int i, uint32_t f) { return stub_SDL_CreateRenderer(w, i, f); }
SDL_EXPORT void SDL_DestroyRenderer(void* r) { stub_SDL_DestroyRenderer(r); }
SDL_EXPORT int SDL_RenderClear(void* r) { return stub_SDL_RenderClear(r); }
SDL_EXPORT void SDL_RenderPresent(void* r) { stub_SDL_RenderPresent(r); }
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
SDL_EXPORT void SDL_SetCursor(void* c) { stub_SDL_SetCursor(c); }
SDL_EXPORT void SDL_FreeCursor(void* c) { stub_SDL_FreeCursor(c); }
SDL_EXPORT int SDL_ShowCursor(int t) { return stub_SDL_ShowCursor(t); }
SDL_EXPORT uint32_t SDL_GetMouseState(int* x, int* y) { return stub_SDL_GetMouseState(x, y); }
SDL_EXPORT uint32_t SDL_GetGlobalMouseState(int* x, int* y) { return stub_SDL_GetGlobalMouseState(x, y); }
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

    // Renderer
    registerFunction("SDL_CreateRenderer", reinterpret_cast<void*>(stub_SDL_CreateRenderer));
    registerFunction("SDL_DestroyRenderer", reinterpret_cast<void*>(stub_SDL_DestroyRenderer));
    registerFunction("SDL_RenderClear", reinterpret_cast<void*>(stub_SDL_RenderClear));
    registerFunction("SDL_RenderPresent", reinterpret_cast<void*>(stub_SDL_RenderPresent));
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
    registerFunction("SDL_SetCursor", reinterpret_cast<void*>(stub_SDL_SetCursor));
    registerFunction("SDL_FreeCursor", reinterpret_cast<void*>(stub_SDL_FreeCursor));
    registerFunction("SDL_ShowCursor", reinterpret_cast<void*>(stub_SDL_ShowCursor));

    // Mouse
    registerFunction("SDL_GetMouseState", reinterpret_cast<void*>(stub_SDL_GetMouseState));
    registerFunction("SDL_GetGlobalMouseState", reinterpret_cast<void*>(stub_SDL_GetGlobalMouseState));

    // Video
    registerFunction("SDL_GetNumVideoDisplays", reinterpret_cast<void*>(stub_SDL_GetNumVideoDisplays));
    registerFunction("SDL_GetDisplayBounds", reinterpret_cast<void*>(stub_SDL_GetDisplayBounds));

    // Timer
    registerFunction("SDL_GetTicks", reinterpret_cast<void*>(stub_SDL_GetTicks));
    registerFunction("SDL_GetPerformanceCounter", reinterpret_cast<void*>(stub_SDL_GetPerformanceCounter));
    registerFunction("SDL_GetPerformanceFrequency", reinterpret_cast<void*>(stub_SDL_GetPerformanceFrequency));

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
