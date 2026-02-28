/*
 * FFI.cpp - Foreign Function Interface for Pharo VM
 *
 * Implements UFFI/TFFI primitives and SDL2 stub functions.
 * Uses libffi for native callouts on ARM64.
 */

#include "FFI.hpp"
#include "../platform/EventQueue.hpp"
#include "../platform/DisplaySurface.hpp"
#include "../platform/PlatformBridge.h"
#include <algorithm>
#include <cstring>
#include <chrono>
#include <dlfcn.h>
#include <ffi.h>
#include <unistd.h>
#include <vector>

#ifdef __APPLE__
#include <TargetConditionals.h>
#include <CoreFoundation/CoreFoundation.h>
#endif

#include <atomic>

// Flag set when Emergency Debugger window is created — checked by interpreter to dump stack
std::atomic<bool> g_emergencyDebuggerTriggered{false};

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

#define SDL_TEXTINPUTEVENT_TEXT_SIZE 32

struct SDL_TextInputEvent {
    uint32_t type;        // SDL_TEXTINPUT
    uint32_t timestamp;
    uint32_t windowID;
    char text[SDL_TEXTINPUTEVENT_TEXT_SIZE];  // UTF-8 encoded text
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
    SDL_TextInputEvent text;
    uint8_t padding[56];  // SDL_Event is 56 bytes
};

namespace pharo {
namespace ffi {

// Forward declarations
void registerSDL2Stubs();
void registerFreeTypeStubs();
void registerLibgit2Stubs();

} // close namespace ffi/pharo temporarily for extern "C" declarations
}

// VM marker symbols (defined later, declared here for initializeFFI)
extern "C" {
void* primitiveLoadSymbolFromModule(void* symbol, void* module);
void* primitiveLoadModule(void* moduleName);
}

namespace pharo {
namespace ffi {

static bool sInitialized = false;
static std::unordered_map<std::string, void*> sFunctionCache;

bool initializeFFI() {
    if (sInitialized) return true;
    sInitialized = true;

    // Register SDL2 stub functions — these MUST win over any real SDL2
    // because we use fake window handles that real SDL2 can't handle.
    registerSDL2Stubs();

    // Register VM marker symbols so TFFIBackend finds them via lookupFunction()
    // instead of relying on dlsym(RTLD_DEFAULT) which fails on App Store builds
    // where symbol visibility is stripped.
    registerFunction("primitiveLoadSymbolFromModule",
                     reinterpret_cast<void*>(primitiveLoadSymbolFromModule));
    registerFunction("primitiveLoadModule",
                     reinterpret_cast<void*>(primitiveLoadModule));

    // FreeType and libgit2 stubs are NOT registered at init time.
    // lookupFunction() tries real libraries first (via dlsym/dlopen),
    // and only falls back to generic stubs if the real libs aren't available
    // (e.g., on iOS where Homebrew doesn't exist).

    return true;
}

void shutdownFFI() {
    sFunctionCache.clear();
    sInitialized = false;
}

bool isModuleLoaded(const std::string& moduleName) {
    if (moduleName == "SDL2" || moduleName == "libSDL2" ||
        moduleName.find("SDL2") != std::string::npos) {
        void* sdlInit = dlsym(RTLD_DEFAULT, "SDL_Init");
        if (sdlInit != nullptr) {
            return true;
        }
        auto it = sFunctionCache.find("SDL_Init");
        return it != sFunctionCache.end();
    }

    return true;  // Assume available; will fail on individual symbol lookup if not
}

// App bundle Frameworks path — set at init from main bundle.
// Libraries must be bundled and re-signed (Mac Catalyst requires team ID match).
static std::string sAppFrameworksPath;

void setAppBundlePath(const std::string& bundlePath) {
    sAppFrameworksPath = bundlePath + "/Contents/Frameworks";
}

const std::string& getAppFrameworksPath() {
    return sAppFrameworksPath;
}

// Library search paths — checked in order when dlsym(RTLD_DEFAULT) fails.
// App bundle Frameworks is checked first (bundled, re-signed libs).
// Homebrew is tried as fallback for development (only works for non-sandboxed).
static std::vector<std::string> getLibSearchPaths() {
    std::vector<std::string> paths;
    if (!sAppFrameworksPath.empty()) {
        paths.push_back(sAppFrameworksPath);
    }
#if !(TARGET_OS_IPHONE && !TARGET_OS_MACCATALYST)
    // Mac Catalyst / macOS: try Homebrew as fallback (won't work if
    // code-signature team ID mismatch, but useful for ad-hoc builds)
    paths.push_back("/opt/homebrew/lib");
    paths.push_back("/usr/local/lib");
#endif
    return paths;
}

// Cache of dlopen handles so we don't re-open the same library repeatedly
static std::unordered_map<std::string, void*> sModuleHandleCache;

// Try to load a symbol by searching common library paths.
// moduleName is a bare name like "libcairo.2.dylib" or "libgit2.dylib".
static void* tryLoadFromSearchPaths(const std::string& moduleName, const std::string& funcName) {
    auto mit = sModuleHandleCache.find(moduleName);
    if (mit != sModuleHandleCache.end()) {
        if (mit->second) {
            return dlsym(mit->second, funcName.c_str());
        }
        return nullptr;  // Previously failed to load
    }

    std::vector<std::string> candidates;
    candidates.push_back(moduleName);
    if (moduleName.compare(0, 3, "lib") != 0) {
        candidates.push_back("lib" + moduleName);
    }
    if (moduleName.find(".dylib") == std::string::npos && moduleName.find(".so") == std::string::npos) {
        candidates.push_back(moduleName + ".dylib");
        if (moduleName.compare(0, 3, "lib") != 0) {
            candidates.push_back("lib" + moduleName + ".dylib");
        }
    }

    auto searchPaths = getLibSearchPaths();
    for (const auto& dir : searchPaths) {
        for (const auto& name : candidates) {
            std::string fullPath = dir + "/" + name;
            void* handle = dlopen(fullPath.c_str(), RTLD_NOW | RTLD_GLOBAL);
            if (handle) {
                sModuleHandleCache[moduleName] = handle;
                void* sym = dlsym(handle, funcName.c_str());
                if (sym) return sym;
                // Module loaded but symbol not found — still cache the handle
                return nullptr;
            }
        }
    }

    sModuleHandleCache[moduleName] = nullptr;
    return nullptr;
}

void* lookupFunction(const std::string& moduleName, const std::string& funcName) {
    auto it = sFunctionCache.find(funcName);
    if (it != sFunctionCache.end()) {
        return it->second;
    }

    // For SDL_ functions not in our stub cache, return a generic no-op
    // instead of falling through to dlsym (which finds force-loaded real SDL2
    // that crashes on our fake 0xDEADBEEF window handles).
    if (funcName.compare(0, 4, "SDL_") == 0) {
        static auto genericSDLNoOp = +[]() -> intptr_t { return 0; };
        void* func = reinterpret_cast<void*>(genericSDLNoOp);
        sFunctionCache[funcName] = func;
        return func;
    }

    // On iOS, system() calls abort() — return a stub that returns -1 (failure).
    // This prevents FFI tests (e.g., DeleteVisitorTest) from crashing the app.
#if TARGET_OS_IOS
    if (funcName == "system") {
        static auto systemStub = +[](const char*) -> int { return -1; };
        void* func = reinterpret_cast<void*>(systemStub);
        sFunctionCache[funcName] = func;
        return func;
    }
#endif

    // Try real library first via dlsym — this finds symbols from libraries
    // already loaded by primitiveLoadModule (Homebrew, bundled, system).
    void* func = dlsym(RTLD_DEFAULT, funcName.c_str());
    if (func) {
        sFunctionCache[funcName] = func;
        return func;
    }

    // If the module name is a path, try dlopen + dlsym on it directly.
    // The image often passes full paths like '/opt/homebrew/lib/libcairo.2.dylib'.
    if (!moduleName.empty() && moduleName[0] == '/') {
        void* handle = dlopen(moduleName.c_str(), RTLD_NOW | RTLD_GLOBAL);
        if (handle) {
            func = dlsym(handle, funcName.c_str());
            if (func) {
                sFunctionCache[funcName] = func;
                return func;
            }
        }
    }

    // If the module name is a bare library name, try common search paths.
    if (!moduleName.empty() && moduleName[0] != '/' && moduleName.find("SDL") == std::string::npos) {
        func = tryLoadFromSearchPaths(moduleName, funcName);
        if (func) {
            sFunctionCache[funcName] = func;
            return func;
        }
    }

    // Fallback stubs for when real libraries are not available (e.g., iOS).
    // These return error codes so the image handles failure gracefully.
    if (funcName.compare(0, 3, "FT_") == 0) {
        static auto genericFTError = +[]() -> intptr_t { return 1; };
        func = reinterpret_cast<void*>(genericFTError);
        sFunctionCache[funcName] = func;
        return func;
    }
    if (funcName.compare(0, 4, "git_") == 0 || funcName.compare(0, 7, "giterr_") == 0) {
        if (funcName == "giterr_last" || funcName == "git_error_last") {
            static auto gitErrNull = +[]() -> intptr_t { return 0; };
            func = reinterpret_cast<void*>(gitErrNull);
        } else {
            static auto genericGitError = +[]() -> intptr_t { return -1; };
            func = reinterpret_cast<void*>(genericGitError);
        }
        sFunctionCache[funcName] = func;
        return func;
    }
    if (funcName.compare(0, 6, "cairo_") == 0) {
        static auto genericCairoNull = +[]() -> intptr_t { return 0; };
        func = reinterpret_cast<void*>(genericCairoNull);
        sFunctionCache[funcName] = func;
        return func;
    }

    return nullptr;
}

void registerFunction(const std::string& funcName, void* funcPtr) {
    sFunctionCache[funcName] = funcPtr;
}

// SDL2 stub functions -- make OSWindow think SDL2 is available.
// Supports multiple windows/textures (Emergency Debugger creates separate ones).

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
    bool usesDisplaySurface;  // True when LockTexture returned the display surface buffer
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

// Poll-count based EXPOSED delay — simulates real SDL2 window creation latency.
// Real SDL2 takes 200-500ms for OS window creation, during which SDL_PollEvent
// returns 0 and the event loop yields. We simulate this: after SDL_CreateWindow,
// SDL_PollEvent returns 0 for sPollCountdown calls. Each call triggers a 5ms yield
// in OSSDL2Driver's event loop (Delay forMilliseconds: 5), giving SessionManager
// CPU time to initialize UITheme before EXPOSED triggers rendering.
// Without this, SpStyleEnvironmentColorProxy DNU fires an Emergency Debugger.
static bool sWindowCreated = false;
static int sPollCountdown = 0;     // Calls remaining before delivering EXPOSED
static bool g_firstExposedDelivered = false;

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

// Query whether SDL2 event polling is active (OSSDL2Driver's event loop has started).
// Events injected before this returns true will be consumed by processInputEvents()
// and NOT reach OSSDL2Driver, so mouse/keyboard events will be lost.
bool ffi_isSDLEventPollingActive() {
    return pharo::gEventQueue.isSDL2EventPollingActive();
}

bool ffi_isFirstExposedDelivered() {
    return g_firstExposedDelivered;
}

// Called from vm_setDisplaySize when the Metal view resizes.
// Pushes SIZE_CHANGED + EXPOSED into the SDL event queue so Pharo re-layouts
// its Forms to match the new display dimensions.
void ffi_notifyDisplayResize(int width, int height) {
    if (!sMainWindow) return;

    // Update the SDL window state to match
    auto wit = sWindows.find(sMainWindow);
    if (wit != sWindows.end()) {
        wit->second.width = width;
        wit->second.height = height;
    }

    // Don't push events during poll countdown — SessionManager needs time
    // to initialize UITheme before receiving SIZE_CHANGED. The countdown
    // expiry will deliver SIZE_CHANGED + EXPOSED with the current dimensions.
    if (sPollCountdown > 0) return;

    sPendingWindowEvents.push(SDL_WINDOWEVENT_SIZE_CHANGED);
    sPendingWindowEvents.push(SDL_WINDOWEVENT_EXPOSED);
}

int stub_SDL_Init(uint32_t flags) {
    sSDL2Initialized = true;
    return 0;
}

void stub_SDL_Quit() {
    sSDL2Initialized = false;
}

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
    if (title && strstr(title, "Emergency") != nullptr) {
        g_emergencyDebuggerTriggered.store(true, std::memory_order_release);
    }
    void* handle = reinterpret_cast<void*>(sNextHandle++);
    SDLWindowState state;
    state.width = w;
    state.height = h;
    state.title = title ? title : "";
    sWindows[handle] = state;
    if (!sMainWindow) sMainWindow = handle;

    // Simulate real SDL2 window creation latency. Real SDL2 takes 200-500ms
    // for the OS to create and composite the window. During that time,
    // SDL_PollEvent returns 0 and the event loop yields. We return 0 for
    // sPollCountdown calls (~1.5s at 5ms/yield), giving SessionManager
    // enough CPU time to initialize UITheme before EXPOSED triggers rendering.
    if (!sWindowCreated) {
        sWindowCreated = true;
        sPollCountdown = 300;  // 300 polls × 5ms yield = ~1.5s of SessionManager CPU time
    }
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
    for (auto& kv : sWindows) {
        if ((reinterpret_cast<uintptr_t>(kv.first) & 0xFFFFFFFF) == id) {
            return kv.first;
        }
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

void* stub_SDL_CreateRenderer(void* window, int index, uint32_t flags) {
    void* handle = reinterpret_cast<void*>(sNextHandle++);
    SDLRendererState state;
    state.window = window;
    state.currentTexture = nullptr;
    sRenderers[handle] = state;
    if (window == sMainWindow || !sMainRenderer) {
        sMainRenderer = handle;
    }
    return handle;
}

void stub_SDL_DestroyRenderer(void* renderer) {
    if (renderer != sMainRenderer) {
        sRenderers.erase(renderer);
    }
}

int stub_SDL_RenderClear(void* renderer) {
    return 0;
}

void stub_SDL_RenderPresent(void* renderer) {
    if (!sSDLRenderingActive) {
        sSDLRenderingActive = true;
    }

    // Only the main renderer writes to gDisplaySurface
    auto rit = sRenderers.find(renderer);
    if (rit == sRenderers.end() || !rit->second.currentTexture) return;
    if (renderer != sMainRenderer) return;

    auto tit = sTextures.find(rit->second.currentTexture);
    if (tit == sTextures.end() || !tit->second.pixels) return;

    if (pharo::gDisplaySurface) {
        // When texture writes directly to display surface, skip the copy --
        // Pharo already wrote there via BitBlt.
        if (tit->second.usesDisplaySurface) {
            pharo::gDisplaySurface->update();
            return;
        }

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

int stub_SDL_GetRendererOutputSize(void* renderer, int* w, int* h) {
    int rw = 1024, rh = 768;
    if (pharo::gDisplaySurface) {
        rw = pharo::gDisplaySurface->width();
        rh = pharo::gDisplaySurface->height();
    }
    if (w) *w = rw;
    if (h) *h = rh;
    return 0;
}

int stub_SDL_SetRenderDrawColor(void* renderer, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return 0;
}

void* stub_SDL_CreateTexture(void* renderer, uint32_t format, int access, int w, int h) {
    // DO NOT override dimensions -- Pharo's Form uses the same extent for BitBlt stride.
    // Changing the texture size without Pharo knowing causes stride mismatch (diagonal garbage).
    void* handle = reinterpret_cast<void*>(sNextHandle++);
    SDLTextureState state;
    state.pixels = static_cast<uint32_t*>(calloc(w * h, 4));
    state.width = w;
    state.height = h;
    state.pitch = w * 4;  // 32bpp XRGB
    state.renderer = renderer;
    state.usesDisplaySurface = false;
    sTextures[handle] = state;
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

    // Return the display surface buffer directly instead of the texture's private buffer.
    // Pharo stores this pointer via setPointer: and writes to it continuously via BitBlt,
    // bypassing LockTexture/UnlockTexture after the first call. By returning the display
    // surface buffer, Pharo writes directly where the Metal renderer reads.
    //
    // Always re-check dimensions on every call — the display surface may have been
    // resized (e.g. iPad rotation or initial size mismatch). If dimensions don't match,
    // fall back to the texture's private buffer so Pharo doesn't write out-of-bounds.
    uint32_t* returnPixels = it->second.pixels;
    int returnPitch = it->second.pitch;
    bool useDS = false;

    if (pharo::gDisplaySurface) {
        int dsW = pharo::gDisplaySurface->width();
        int dsH = pharo::gDisplaySurface->height();
        if (dsW == it->second.width && dsH == it->second.height) {
            returnPixels = pharo::gDisplaySurface->pixels();
            returnPitch = dsW * 4;
            useDS = true;
        }
    }

    it->second.usesDisplaySurface = useDS;
    if (pixels) *pixels = returnPixels;
    if (pitch) *pitch = returnPitch;
    return 0;
}

void stub_SDL_UnlockTexture(void* texture) {
    // In real SDL2, UnlockTexture uploads modified pixels to the GPU texture.
    // In our stub architecture, copy texture pixels to gDisplaySurface here
    // because the deferred present (useDeferredUpdates → RenderCopy → RenderPresent)
    // is NOT reached when Smalltalk errors propagate past the ensure: block in
    // deferUpdatesWhile:. Without this, display never updates after drawing errors.
    auto it = sTextures.find(texture);
    if (it == sTextures.end() || !it->second.pixels) return;

    // Skip copy when texture writes directly to the display surface buffer —
    // Pharo already wrote there, copying the texture's private buffer would overwrite.
    if (it->second.usesDisplaySurface) return;

    if (pharo::gDisplaySurface) {
        uint32_t* src = it->second.pixels;
        int srcW = it->second.width;
        int srcH = it->second.height;
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

int stub_SDL_RenderCopy(void* renderer, void* texture, void* srcrect, void* dstrect) {
    auto rit = sRenderers.find(renderer);
    if (rit != sRenderers.end()) {
        rit->second.currentTexture = texture;
    }
    return 0;
}

int stub_SDL_UpdateTexture(void* texture, void* rect, void* pixels, int pitch) {
    auto it = sTextures.find(texture);
    if (it == sTextures.end() || !it->second.pixels || !pixels) {
        return -1;
    }

    int texH = it->second.height;
    int texPitch = it->second.pitch;

    if (!rect) {
        int srcBytesPerRow = pitch;
        int dstBytesPerRow = texPitch;
        int copyBytes = std::min(srcBytesPerRow, dstBytesPerRow);
        uint8_t* src = static_cast<uint8_t*>(pixels);
        uint8_t* dst = reinterpret_cast<uint8_t*>(it->second.pixels);

        for (int y = 0; y < texH; y++) {
            memcpy(dst + y * dstBytesPerRow, src + y * srcBytesPerRow, copyBytes);
        }
    } else {
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
    }
    return 0;
}

// Forwards events from gEventQueue to SDL event structures for OSSDL2Driver.
int stub_SDL_PollEvent(void* event) {
    static bool flagSet = false;
    if (!flagSet) {
        flagSet = true;
        pharo::gEventQueue.setSDL2EventPollingActive(true);
    }

    // Reject null/low event pointers (stale heap address after GC compaction)
    if (!event || reinterpret_cast<uintptr_t>(event) < 0x10000) {
        return !pharo::gEventQueue.isEmpty() ? 1 : 0;
    }

    // Deliver pending synthetic window events one at a time
    if (!sPendingWindowEvents.empty()) {
        uint8_t windowEventType = sPendingWindowEvents.front();
        sPendingWindowEvents.pop();
        SDL_Event* sdlEvent = reinterpret_cast<SDL_Event*>(event);
        memset(sdlEvent, 0, sizeof(SDL_Event));
        uint32_t windowID = sMainWindow ? stub_SDL_GetWindowID(sMainWindow) : 1;
        sdlEvent->window.type = SDL_WINDOWEVENT;
        // Incrementing timestamps required: OSWindowRenderer >> exposed:
        // discards events with timestamps <= lastExposeTime.
        {
            static auto start = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            sdlEvent->window.timestamp = static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count()
            );
        }
        sdlEvent->window.windowID = windowID;
        sdlEvent->window.event = windowEventType;
        // SIZE_CHANGED needs actual dimensions or Pharo resizes to 0x0
        if (windowEventType == SDL_WINDOWEVENT_SIZE_CHANGED) {
            int w = 1024, h = 768;
            if (pharo::gDisplaySurface) {
                w = pharo::gDisplaySurface->width();
                h = pharo::gDisplaySurface->height();
            } else if (sMainWindow) {
                auto wit = sWindows.find(sMainWindow);
                if (wit != sWindows.end()) {
                    w = wit->second.width;
                    h = wit->second.height;
                }
            }
            sdlEvent->window.data1 = w;
            sdlEvent->window.data2 = h;
        }
        if (windowEventType == SDL_WINDOWEVENT_EXPOSED) {
            g_firstExposedDelivered = true;
        }
        return 1;
    }

    // Poll-count countdown: simulate real SDL2 window creation latency.
    // Each call where we return 0 causes the event loop to yield for ~5ms,
    // giving SessionManager CPU time to initialize UITheme.
    if (sMainWindow && sWindowCreated && sPollCountdown > 0) {
        --sPollCountdown;
        if (sPollCountdown == 0) {
            // Window "creation" complete — deliver initial events like real SDL2
            sPendingWindowEvents.push(SDL_WINDOWEVENT_SHOWN);
            sPendingWindowEvents.push(SDL_WINDOWEVENT_FOCUS_GAINED);
            sPendingWindowEvents.push(SDL_WINDOWEVENT_SIZE_CHANGED);
            sPendingWindowEvents.push(SDL_WINDOWEVENT_EXPOSED);
        }
    }

    // Pop event from our queue. Return 0 quickly when empty so the event loop
    // process yields via Processor yield, which pumps CFRunLoop via the
    // relinquish callback. Do NOT block here -- the event loop tight-loops
    // at priority 60 and blocking starves all lower-priority processes.
    pharo::Event pharoEvent;
nextEvent:
    if (!pharo::gEventQueue.pop(pharoEvent)) {
        return 0;
    }

    SDL_Event* sdlEvent = reinterpret_cast<SDL_Event*>(event);
    memset(sdlEvent, 0, sizeof(SDL_Event));
    uint32_t windowID = sMainWindow ? stub_SDL_GetWindowID(sMainWindow) : 1;

    // Convert Pharo event to SDL event
    if (pharoEvent.type == static_cast<int>(pharo::EventType::Mouse)) {
        // arg5 subtypes: 0=move, 1=down, 2=up, 3=drag
        int subtype = pharoEvent.arg5;
        int prevX = sMouseX;
        int prevY = sMouseY;
        sMouseX = pharoEvent.arg1;
        sMouseY = pharoEvent.arg2;

        // Pharo: Red=4 Yellow=2 Blue=1 -> SDL: Left=bit0 Middle=bit1 Right=bit2
        // OSSDL2 convertButtonFromEvent: SDL 1->Red(4), SDL 2->Blue(1), SDL 3->Yellow(2)
        uint32_t sdlButtonMask = 0;
        if (pharoEvent.arg3 & 4) sdlButtonMask |= (1 << 0);  // Red -> SDL_BUTTON_LMASK
        if (pharoEvent.arg3 & 1) sdlButtonMask |= (1 << 1);  // Blue -> SDL_BUTTON_MMASK
        if (pharoEvent.arg3 & 2) sdlButtonMask |= (1 << 2);  // Yellow -> SDL_BUTTON_RMASK

        if (subtype == 0 || subtype == 3) {
            sdlEvent->motion.type = SDL_MOUSEMOTION;
            sdlEvent->motion.timestamp = pharoEvent.timeStamp;
            sdlEvent->motion.windowID = windowID;
            sdlEvent->motion.which = 0;
            sdlEvent->motion.x = pharoEvent.arg1;
            sdlEvent->motion.y = pharoEvent.arg2;
            sdlEvent->motion.xrel = pharoEvent.arg1 - prevX;
            sdlEvent->motion.yrel = pharoEvent.arg2 - prevY;
            sdlEvent->motion.state = sdlButtonMask;
        } else if (subtype == 1) {
            sMouseButtons |= sdlButtonMask;
            sdlEvent->button.type = SDL_MOUSEBUTTONDOWN;
            sdlEvent->button.timestamp = pharoEvent.timeStamp;
            sdlEvent->button.windowID = windowID;
            sdlEvent->button.which = 0;
            sdlEvent->button.x = pharoEvent.arg1;
            sdlEvent->button.y = pharoEvent.arg2;
            sdlEvent->button.state = 1;  // SDL_PRESSED
            sdlEvent->button.clicks = 1;
            if (pharoEvent.arg3 & 4)      sdlEvent->button.button = SDL_BUTTON_LEFT;
            else if (pharoEvent.arg3 & 2) sdlEvent->button.button = SDL_BUTTON_RIGHT;
            else if (pharoEvent.arg3 & 1) sdlEvent->button.button = SDL_BUTTON_MIDDLE;
            else                          sdlEvent->button.button = SDL_BUTTON_LEFT;
        } else if (subtype == 2) {
            sMouseButtons &= ~sdlButtonMask;
            sdlEvent->button.type = SDL_MOUSEBUTTONUP;
            sdlEvent->button.timestamp = pharoEvent.timeStamp;
            sdlEvent->button.windowID = windowID;
            sdlEvent->button.which = 0;
            sdlEvent->button.x = pharoEvent.arg1;
            sdlEvent->button.y = pharoEvent.arg2;
            sdlEvent->button.state = 0;  // SDL_RELEASED
            sdlEvent->button.clicks = 1;
            if (pharoEvent.arg3 & 4)      sdlEvent->button.button = SDL_BUTTON_LEFT;
            else if (pharoEvent.arg3 & 2) sdlEvent->button.button = SDL_BUTTON_RIGHT;
            else if (pharoEvent.arg3 & 1) sdlEvent->button.button = SDL_BUTTON_MIDDLE;
            else                          sdlEvent->button.button = SDL_BUTTON_LEFT;
        }
        return 1;
    } else if (pharoEvent.type == static_cast<int>(pharo::EventType::MouseWheel)) {
        sdlEvent->wheel.type = SDL_MOUSEWHEEL;
        sdlEvent->wheel.timestamp = pharoEvent.timeStamp;
        sdlEvent->wheel.windowID = windowID;
        sdlEvent->wheel.which = 0;
        sdlEvent->wheel.x = pharoEvent.arg3;
        sdlEvent->wheel.y = pharoEvent.arg4;
        sdlEvent->wheel.direction = 0;
        return 1;
    } else if (pharoEvent.type == static_cast<int>(pharo::EventType::Keyboard)) {
        // arg1=charCode, arg2=subtype (0=down, 1=up, 2=keystroke),
        // arg3=modifiers, arg4=keyCode (scancode)
        int subtype = pharoEvent.arg2;
        int charCode = pharoEvent.arg1;

        // Keystroke (subtype 2) with printable character → SDL_TEXTINPUT
        // Real SDL2 generates: KEYDOWN, TEXTINPUT, KEYUP for typed characters.
        // We get: down(0), stroke(2), up(1) — so stroke maps to TEXTINPUT.
        // Non-printable keys (arrows, backspace, enter, escape, tab) do NOT
        // get SDL_TEXTINPUT in real SDL2 — only KEYDOWN/KEYUP.
        bool isPrintable = (charCode >= 32 && charCode != 127);
        if (subtype == 2 && isPrintable && !(pharoEvent.arg3 & ~1)) {
            // Only generate TEXTINPUT for unmodified keys (or Shift only).
            // Cmd+C, Ctrl+X etc. are shortcuts, not text input.
            memset(sdlEvent, 0, sizeof(SDL_Event));
            sdlEvent->text.type = SDL_TEXTINPUT;
            sdlEvent->text.timestamp = pharoEvent.timeStamp;
            sdlEvent->text.windowID = windowID;
            // Encode charCode as UTF-8
            char* p = sdlEvent->text.text;
            if (charCode < 0x80) {
                *p++ = static_cast<char>(charCode);
            } else if (charCode < 0x800) {
                *p++ = static_cast<char>(0xC0 | (charCode >> 6));
                *p++ = static_cast<char>(0x80 | (charCode & 0x3F));
            } else if (charCode < 0x10000) {
                *p++ = static_cast<char>(0xE0 | (charCode >> 12));
                *p++ = static_cast<char>(0x80 | ((charCode >> 6) & 0x3F));
                *p++ = static_cast<char>(0x80 | (charCode & 0x3F));
            } else {
                *p++ = static_cast<char>(0xF0 | (charCode >> 18));
                *p++ = static_cast<char>(0x80 | ((charCode >> 12) & 0x3F));
                *p++ = static_cast<char>(0x80 | ((charCode >> 6) & 0x3F));
                *p++ = static_cast<char>(0x80 | (charCode & 0x3F));
            }
            *p = '\0';
            return 1;
        }

        if (subtype == 2) {
            // Stroke event for non-printable or modified key — no SDL event needed.
            // The down event (subtype 0) already generated SDL_KEYDOWN.
            // Skip this event and try the next one.
            goto nextEvent;
        }

        // Key down/up → SDL_KEYDOWN/KEYUP
        if (subtype == 0) {
            sdlEvent->key.type = SDL_KEYDOWN;
            sdlEvent->key.state = 1;
        } else {
            sdlEvent->key.type = SDL_KEYUP;
            sdlEvent->key.state = 0;
        }
        sdlEvent->key.timestamp = pharoEvent.timeStamp;
        sdlEvent->key.windowID = windowID;
        sdlEvent->key.repeat = 0;
        sdlEvent->key.keysym.scancode = pharoEvent.arg4;
        sdlEvent->key.keysym.sym = charCode;
        uint16_t sdlMod = 0;
        if (pharoEvent.arg3 & 1) sdlMod |= 0x0001;  // KMOD_LSHIFT
        if (pharoEvent.arg3 & 2) sdlMod |= 0x0040;  // KMOD_LCTRL
        if (pharoEvent.arg3 & 4) sdlMod |= 0x0100;  // KMOD_LALT
        if (pharoEvent.arg3 & 8) sdlMod |= 0x0400;  // KMOD_LGUI (Cmd)
        sdlEvent->key.keysym.mod = sdlMod;
        return 1;
    } else if (pharoEvent.type == static_cast<int>(pharo::EventType::WindowMetrics)) {
        sdlEvent->window.type = SDL_WINDOWEVENT;
        sdlEvent->window.timestamp = pharoEvent.timeStamp;
        sdlEvent->window.windowID = windowID;
        sdlEvent->window.event = SDL_WINDOWEVENT_SIZE_CHANGED;
        sdlEvent->window.data1 = pharoEvent.arg1;
        sdlEvent->window.data2 = pharoEvent.arg2;
        return 1;
    }

    return 0;
}

int stub_SDL_WaitEvent(void* event) {
    // Used by modal event loops (menu dropdown grab loops).
    // Without this, menu dropdowns open and close immediately.
    int result = stub_SDL_PollEvent(event);
    if (result != 0) return result;

    // Sleep briefly and retry (up to ~100ms) to avoid busy-waiting
    for (int i = 0; i < 10; i++) {
        usleep(10000);  // 10ms
        result = stub_SDL_PollEvent(event);
        if (result != 0) return result;
    }

    return 0;
}

int stub_SDL_PushEvent(void* event) {
    return 1;  // Success
}

// Clipboard — delegates to platform bridge (which calls Swift/ObjC)
extern "C" const char* vm_getClipboardText(void);
extern "C" void vm_setClipboardText(const char* text);

char* stub_SDL_GetClipboardText() {
    const char* text = vm_getClipboardText();
    return strdup(text ? text : "");
}

int stub_SDL_SetClipboardText(const char* text) {
    if (text) vm_setClipboardText(text);
    return 0;
}

int stub_SDL_HasClipboardText() {
    const char* text = vm_getClipboardText();
    return (text && text[0] != '\0') ? 1 : 0;
}

void stub_SDL_free(void* mem) {
    free(mem);
}

// Text input — delegates to platform bridge for iOS keyboard show/hide
extern "C" void vm_startTextInput(void);
extern "C" void vm_stopTextInput(void);
static bool sTextInputActive = false;

void stub_SDL_StartTextInput() {
    sTextInputActive = true;
    vm_startTextInput();
}

void stub_SDL_StopTextInput() {
    sTextInputActive = false;
    vm_stopTextInput();
}

int stub_SDL_IsTextInputActive() {
    return sTextInputActive ? 1 : 0;
}

// Cursor stubs
void* stub_SDL_CreateSystemCursor(int id) {
    return reinterpret_cast<void*>(static_cast<uintptr_t>(0x20000 + id));
}

void* stub_SDL_CreateCursor(void* data, void* mask, int w, int h, int hot_x, int hot_y) {
    static uintptr_t nextCursorHandle = 0x30000;
    return reinterpret_cast<void*>(nextCursorHandle++);
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
    static uintptr_t nextSurfHandle = 0x40000;
    return reinterpret_cast<void*>(nextSurfHandle++);
}

void stub_SDL_FreeSurface(void* surface) {
}

uint32_t stub_SDL_GetMouseState(int* x, int* y) {
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

int stub_SDL_GetDisplayUsableBounds(int displayIndex, void* rect) {
    // Returns usable display area (below Pharo menu bar).
    // Pharo's OSWorldRenderer calls this to determine where windows can be placed.
    // Without a proper offset, new windows (debugger, browser) appear under the
    // Pharo menu bar with unreachable title bars.
    static const int kMenuBarHeight = 28;  // Pharo TaskbarMorph default height
    if (rect) {
        int* r = static_cast<int*>(rect);
        r[0] = 0;     // x
        r[1] = kMenuBarHeight;     // y — below menu bar
        if (pharo::gDisplaySurface) {
            r[2] = pharo::gDisplaySurface->width();
            r[3] = pharo::gDisplaySurface->height() - kMenuBarHeight;
        } else {
            r[2] = 1024;
            r[3] = 768 - kMenuBarHeight;
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
}

// Exported SDL2 symbols (weak so they don't conflict with real SDL2 if linked)

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
SDL_EXPORT void SDL_free(void* mem) { stub_SDL_free(mem); }
SDL_EXPORT void SDL_StartTextInput() { stub_SDL_StartTextInput(); }
SDL_EXPORT void SDL_StopTextInput() { stub_SDL_StopTextInput(); }
SDL_EXPORT int SDL_IsTextInputActive() { return stub_SDL_IsTextInputActive(); }
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
SDL_EXPORT int SDL_GetDisplayUsableBounds(int d, void* r) { return stub_SDL_GetDisplayUsableBounds(d, r); }
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

// Marker symbols exported so TFFIBackend can detect FFI availability via dlsym
__attribute__((used, visibility("default")))
void* primitiveLoadSymbolFromModule(void* symbol, void* module) {
    return nullptr;  // Marker for TFFIBackend; actual primitive in Interpreter
}

__attribute__((used, visibility("default")))
void* primitiveLoadModule(void* moduleName) {
    return nullptr;  // Marker for TFFIBackend; actual primitive in Interpreter
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
    registerFunction("SDL_free", reinterpret_cast<void*>(stub_SDL_free));

    // Text input
    registerFunction("SDL_StartTextInput", reinterpret_cast<void*>(stub_SDL_StartTextInput));
    registerFunction("SDL_StopTextInput", reinterpret_cast<void*>(stub_SDL_StopTextInput));
    registerFunction("SDL_IsTextInputActive", reinterpret_cast<void*>(stub_SDL_IsTextInputActive));

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
    registerFunction("SDL_GetDisplayUsableBounds", reinterpret_cast<void*>(stub_SDL_GetDisplayUsableBounds));

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

// ========== FreeType stubs ==========
// FreeType is bundled as a static xcframework, but the Pharo image sometimes
// tries to load it as a shared library via FFI before finding the static
// symbols. These stubs return error codes, causing the image to fall back
// to the statically-linked FreeType (via the FT2Plugin path).

static int stub_FT_Init_FreeType(void** alibrary) {
    if (alibrary) *alibrary = nullptr;
    return 1;  // FT_Err_Cannot_Open_Resource
}

static int stub_FT_Done_FreeType(void* library) {
    return 0;
}

static int stub_FT_New_Face(void* library, const char* path, long index, void** face) {
    if (face) *face = nullptr;
    return 1;
}

static int stub_FT_New_Memory_Face(void* library, const void* data, long size, long index, void** face) {
    if (face) *face = nullptr;
    return 1;
}

static int stub_FT_Done_Face(void* face) { return 0; }
static int stub_FT_Set_Char_Size(void* face, long w, long h, int hres, int vres) { return 1; }
static int stub_FT_Set_Pixel_Sizes(void* face, int w, int h) { return 1; }
static int stub_FT_Load_Glyph(void* face, int idx, int flags) { return 1; }
static int stub_FT_Load_Char(void* face, unsigned long code, int flags) { return 1; }
static int stub_FT_Render_Glyph(void* slot, int mode) { return 1; }
static int stub_FT_Get_Char_Index(void* face, unsigned long code) { return 0; }
static int stub_FT_Get_Kerning(void* face, int l, int r, int mode, void* k) { return 1; }
static int stub_FT_Select_Charmap(void* face, int encoding) { return 1; }
static void* stub_FT_Library_Version(void* lib, int* maj, int* min, int* pat) {
    if (maj) *maj = 0; if (min) *min = 0; if (pat) *pat = 0;
    return nullptr;
}

// ========== libgit2 stubs ==========
// libgit2 is statically linked into PharoVMCore.a. These stubs catch FFI
// attempts to load it as a shared library, returning error codes so the
// image falls back to the statically-linked symbols (found via dlsym).

static int stub_git_libgit2_init() {
    return -1;  // GIT_ERROR
}

static int stub_git_libgit2_shutdown() { return 0; }

static void stub_git_libgit2_version(int* major, int* minor, int* rev) {
    if (major) *major = 0;
    if (minor) *minor = 0;
    if (rev) *rev = 0;
}

// Generic no-op for git_ functions we don't explicitly stub — returns -1 (GIT_ERROR)
static int stub_git_generic_error() { return -1; }

void registerLibgit2Stubs() {
    // Only register stubs if real libgit2 isn't statically linked
    if (dlsym(RTLD_DEFAULT, "git_libgit2_init")) return;
    registerFunction("git_libgit2_init", reinterpret_cast<void*>(stub_git_libgit2_init));
    registerFunction("git_libgit2_shutdown", reinterpret_cast<void*>(stub_git_libgit2_shutdown));
    registerFunction("git_libgit2_version", reinterpret_cast<void*>(stub_git_libgit2_version));
}

void registerFreeTypeStubs() {
    // Only register stubs if real FreeType isn't statically linked
    if (dlsym(RTLD_DEFAULT, "FT_Init_FreeType")) {
        #if DEBUG
        fputs("[FFI] Real FreeType found — skipping stubs\n", stderr);
        #endif
        return;
    }
    #if DEBUG
    fputs("[FFI] FreeType NOT found — registering stubs (bitmap fonts only)\n", stderr);
    #endif
    registerFunction("FT_Init_FreeType", reinterpret_cast<void*>(stub_FT_Init_FreeType));
    registerFunction("FT_Done_FreeType", reinterpret_cast<void*>(stub_FT_Done_FreeType));
    registerFunction("FT_New_Face", reinterpret_cast<void*>(stub_FT_New_Face));
    registerFunction("FT_New_Memory_Face", reinterpret_cast<void*>(stub_FT_New_Memory_Face));
    registerFunction("FT_Done_Face", reinterpret_cast<void*>(stub_FT_Done_Face));
    registerFunction("FT_Set_Char_Size", reinterpret_cast<void*>(stub_FT_Set_Char_Size));
    registerFunction("FT_Set_Pixel_Sizes", reinterpret_cast<void*>(stub_FT_Set_Pixel_Sizes));
    registerFunction("FT_Load_Glyph", reinterpret_cast<void*>(stub_FT_Load_Glyph));
    registerFunction("FT_Load_Char", reinterpret_cast<void*>(stub_FT_Load_Char));
    registerFunction("FT_Render_Glyph", reinterpret_cast<void*>(stub_FT_Render_Glyph));
    registerFunction("FT_Get_Char_Index", reinterpret_cast<void*>(stub_FT_Get_Char_Index));
    registerFunction("FT_Get_Kerning", reinterpret_cast<void*>(stub_FT_Get_Kerning));
    registerFunction("FT_Select_Charmap", reinterpret_cast<void*>(stub_FT_Select_Charmap));
    registerFunction("FT_Library_Version", reinterpret_cast<void*>(stub_FT_Library_Version));
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

    ffi_cif cif;
    std::vector<ffi_type*> ffiArgTypes(argc);
    std::vector<void*> ffiArgValues(argc);

    std::vector<uint64_t> argStorage = argValues;
    std::vector<void*> ptrStorage(argc);  // For pointer conversions
    std::vector<float> floatStorage(argc);  // For float conversions
    std::vector<double> doubleStorage(argc);  // For double conversions

    for (size_t i = 0; i < argc; i++) {
        ffiArgTypes[i] = toFFIType(argTypes[i]);

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

    ffi_type* ffiRetType = toFFIType(returnType);
    ffi_status status = ffi_prep_cif(&cif, FFI_DEFAULT_ABI,
                                      static_cast<unsigned int>(argc),
                                      ffiRetType,
                                      argc > 0 ? ffiArgTypes.data() : nullptr);

    if (status != FFI_OK) {
        result.error = "ffi_prep_cif failed: " + std::to_string(status);
        return result;
    }

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

    ffi_call(&cif, FFI_FN(funcPtr), &retValue,
             argc > 0 ? ffiArgValues.data() : nullptr);

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
