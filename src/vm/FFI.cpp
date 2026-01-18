/*
 * FFI.cpp - Foreign Function Interface using libffi
 *
 * Portable FFI implementation supporting all calling conventions.
 */

#include "FFI.hpp"
#include <iostream>
#include <cstring>
#include <chrono>
#include <dlfcn.h>
#include <ffi.h>

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
        if (dlsym(RTLD_DEFAULT, "SDL_Init") != nullptr) {
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

    // Look up the function
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

static bool sSDL2Initialized = false;
static void* sFakeWindowHandle = reinterpret_cast<void*>(0xDEADBEEF);
static void* sFakeRendererHandle = reinterpret_cast<void*>(0xCAFEBABE);

extern "C" {

// SDL_Init returns 0 on success
int stub_SDL_Init(uint32_t flags) {
    fprintf(stderr, "[SDL2-STUB] SDL_Init(0x%x) -> 0 (success)\n", flags);
    sSDL2Initialized = true;
    return 0;
}

void stub_SDL_Quit() {
    fprintf(stderr, "[SDL2-STUB] SDL_Quit()\n");
    sSDL2Initialized = false;
}

// Returns version info - version 2.0.20
void stub_SDL_GetVersion(void* ver) {
    fprintf(stderr, "[SDL2-STUB] SDL_GetVersion()\n");
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
    fprintf(stderr, "[SDL2-STUB] SDL_CreateWindow('%s', %d, %d, %dx%d, 0x%x) -> %p\n",
            title ? title : "(null)", x, y, w, h, flags, sFakeWindowHandle);
    return sFakeWindowHandle;
}

void stub_SDL_DestroyWindow(void* window) {
    fprintf(stderr, "[SDL2-STUB] SDL_DestroyWindow(%p)\n", window);
}

void stub_SDL_GetWindowSize(void* window, int* w, int* h) {
    // Return a reasonable default size
    if (w) *w = 1024;
    if (h) *h = 768;
    fprintf(stderr, "[SDL2-STUB] SDL_GetWindowSize(%p) -> %dx%d\n", window, 1024, 768);
}

void stub_SDL_SetWindowSize(void* window, int w, int h) {
    fprintf(stderr, "[SDL2-STUB] SDL_SetWindowSize(%p, %dx%d)\n", window, w, h);
}

void stub_SDL_SetWindowTitle(void* window, const char* title) {
    fprintf(stderr, "[SDL2-STUB] SDL_SetWindowTitle(%p, '%s')\n", window, title ? title : "(null)");
}

void stub_SDL_ShowWindow(void* window) {
    fprintf(stderr, "[SDL2-STUB] SDL_ShowWindow(%p)\n", window);
}

void stub_SDL_HideWindow(void* window) {
    fprintf(stderr, "[SDL2-STUB] SDL_HideWindow(%p)\n", window);
}

void stub_SDL_RaiseWindow(void* window) {
    fprintf(stderr, "[SDL2-STUB] SDL_RaiseWindow(%p)\n", window);
}

uint32_t stub_SDL_GetWindowID(void* window) {
    return 1;  // Window ID 1
}

void* stub_SDL_GetWindowFromID(uint32_t id) {
    return sFakeWindowHandle;
}

int stub_SDL_SetWindowFullscreen(void* window, uint32_t flags) {
    fprintf(stderr, "[SDL2-STUB] SDL_SetWindowFullscreen(%p, 0x%x)\n", window, flags);
    return 0;
}

void stub_SDL_GetWindowPosition(void* window, int* x, int* y) {
    if (x) *x = 0;
    if (y) *y = 0;
}

void stub_SDL_SetWindowPosition(void* window, int x, int y) {
    fprintf(stderr, "[SDL2-STUB] SDL_SetWindowPosition(%p, %d, %d)\n", window, x, y);
}

// Renderer stubs
void* stub_SDL_CreateRenderer(void* window, int index, uint32_t flags) {
    fprintf(stderr, "[SDL2-STUB] SDL_CreateRenderer(%p) -> %p\n", window, sFakeRendererHandle);
    return sFakeRendererHandle;
}

void stub_SDL_DestroyRenderer(void* renderer) {
    fprintf(stderr, "[SDL2-STUB] SDL_DestroyRenderer(%p)\n", renderer);
}

int stub_SDL_RenderClear(void* renderer) {
    return 0;
}

void stub_SDL_RenderPresent(void* renderer) {
    // No-op
}

int stub_SDL_SetRenderDrawColor(void* renderer, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return 0;
}

// Event stubs - critical for InputEventSensor
// We return 0 (no events) since we handle events through primitive 264
int stub_SDL_PollEvent(void* event) {
    // Return 0 = no event available
    // The real events are handled by our primitiveGetNextEvent
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
        r[2] = 1024;  // w
        r[3] = 768;   // h
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
    fprintf(stderr, "[SDL2-STUB] SDL_WasInit(0x%x) -> 0x%x\n", flags, sSDL2Initialized ? flags : 0);
    return sSDL2Initialized ? flags : 0;
}

int stub_SDL_VideoInit(const char* driver) {
    fprintf(stderr, "[SDL2-STUB] SDL_VideoInit('%s') -> 0\n", driver ? driver : "(null)");
    return 0;
}

void stub_SDL_VideoQuit() {
    fprintf(stderr, "[SDL2-STUB] SDL_VideoQuit()\n");
}

int stub_SDL_InitSubSystem(uint32_t flags) {
    fprintf(stderr, "[SDL2-STUB] SDL_InitSubSystem(0x%x) -> 0\n", flags);
    return 0;
}

void stub_SDL_QuitSubSystem(uint32_t flags) {
    fprintf(stderr, "[SDL2-STUB] SDL_QuitSubSystem(0x%x)\n", flags);
}

int stub_SDL_SetHint(const char* name, const char* value) {
    fprintf(stderr, "[SDL2-STUB] SDL_SetHint('%s', '%s') -> 1\n",
            name ? name : "(null)", value ? value : "(null)");
    return 1;
}

int stub_SDL_GL_SetAttribute(int attr, int value) {
    fprintf(stderr, "[SDL2-STUB] SDL_GL_SetAttribute(%d, %d) -> 0\n", attr, value);
    return 0;
}

void* stub_SDL_GL_CreateContext(void* window) {
    fprintf(stderr, "[SDL2-STUB] SDL_GL_CreateContext(%p) -> %p\n", window, sFakeRendererHandle);
    return sFakeRendererHandle;
}

void stub_SDL_GL_DeleteContext(void* context) {
    fprintf(stderr, "[SDL2-STUB] SDL_GL_DeleteContext(%p)\n", context);
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

__attribute__((weak)) int SDL_Init(uint32_t flags) { return stub_SDL_Init(flags); }
__attribute__((weak)) void SDL_Quit() { stub_SDL_Quit(); }
__attribute__((weak)) void SDL_GetVersion(void* ver) { stub_SDL_GetVersion(ver); }
__attribute__((weak)) const char* SDL_GetError() { return stub_SDL_GetError(); }
__attribute__((weak)) void* SDL_CreateWindow(const char* t, int x, int y, int w, int h, uint32_t f) { return stub_SDL_CreateWindow(t, x, y, w, h, f); }
__attribute__((weak)) void SDL_DestroyWindow(void* w) { stub_SDL_DestroyWindow(w); }
__attribute__((weak)) void SDL_GetWindowSize(void* w, int* x, int* y) { stub_SDL_GetWindowSize(w, x, y); }
__attribute__((weak)) void SDL_SetWindowSize(void* w, int x, int y) { stub_SDL_SetWindowSize(w, x, y); }
__attribute__((weak)) void SDL_SetWindowTitle(void* w, const char* t) { stub_SDL_SetWindowTitle(w, t); }
__attribute__((weak)) void SDL_ShowWindow(void* w) { stub_SDL_ShowWindow(w); }
__attribute__((weak)) void SDL_HideWindow(void* w) { stub_SDL_HideWindow(w); }
__attribute__((weak)) void SDL_RaiseWindow(void* w) { stub_SDL_RaiseWindow(w); }
__attribute__((weak)) uint32_t SDL_GetWindowID(void* w) { return stub_SDL_GetWindowID(w); }
__attribute__((weak)) void* SDL_GetWindowFromID(uint32_t id) { return stub_SDL_GetWindowFromID(id); }
__attribute__((weak)) int SDL_SetWindowFullscreen(void* w, uint32_t f) { return stub_SDL_SetWindowFullscreen(w, f); }
__attribute__((weak)) void SDL_GetWindowPosition(void* w, int* x, int* y) { stub_SDL_GetWindowPosition(w, x, y); }
__attribute__((weak)) void SDL_SetWindowPosition(void* w, int x, int y) { stub_SDL_SetWindowPosition(w, x, y); }
__attribute__((weak)) void* SDL_CreateRenderer(void* w, int i, uint32_t f) { return stub_SDL_CreateRenderer(w, i, f); }
__attribute__((weak)) void SDL_DestroyRenderer(void* r) { stub_SDL_DestroyRenderer(r); }
__attribute__((weak)) int SDL_RenderClear(void* r) { return stub_SDL_RenderClear(r); }
__attribute__((weak)) void SDL_RenderPresent(void* r) { stub_SDL_RenderPresent(r); }
__attribute__((weak)) int SDL_SetRenderDrawColor(void* r, uint8_t rr, uint8_t g, uint8_t b, uint8_t a) { return stub_SDL_SetRenderDrawColor(r, rr, g, b, a); }
__attribute__((weak)) int SDL_PollEvent(void* e) { return stub_SDL_PollEvent(e); }
__attribute__((weak)) int SDL_WaitEvent(void* e) { return stub_SDL_WaitEvent(e); }
__attribute__((weak)) int SDL_PushEvent(void* e) { return stub_SDL_PushEvent(e); }
__attribute__((weak)) char* SDL_GetClipboardText() { return stub_SDL_GetClipboardText(); }
__attribute__((weak)) int SDL_SetClipboardText(const char* t) { return stub_SDL_SetClipboardText(t); }
__attribute__((weak)) int SDL_HasClipboardText() { return stub_SDL_HasClipboardText(); }
__attribute__((weak)) void* SDL_CreateSystemCursor(int id) { return stub_SDL_CreateSystemCursor(id); }
__attribute__((weak)) void SDL_SetCursor(void* c) { stub_SDL_SetCursor(c); }
__attribute__((weak)) void SDL_FreeCursor(void* c) { stub_SDL_FreeCursor(c); }
__attribute__((weak)) int SDL_ShowCursor(int t) { return stub_SDL_ShowCursor(t); }
__attribute__((weak)) uint32_t SDL_GetMouseState(int* x, int* y) { return stub_SDL_GetMouseState(x, y); }
__attribute__((weak)) uint32_t SDL_GetGlobalMouseState(int* x, int* y) { return stub_SDL_GetGlobalMouseState(x, y); }
__attribute__((weak)) int SDL_GetNumVideoDisplays() { return stub_SDL_GetNumVideoDisplays(); }
__attribute__((weak)) int SDL_GetDisplayBounds(int d, void* r) { return stub_SDL_GetDisplayBounds(d, r); }
__attribute__((weak)) uint32_t SDL_GetTicks() { return stub_SDL_GetTicks(); }
__attribute__((weak)) uint64_t SDL_GetPerformanceCounter() { return stub_SDL_GetPerformanceCounter(); }
__attribute__((weak)) uint64_t SDL_GetPerformanceFrequency() { return stub_SDL_GetPerformanceFrequency(); }
__attribute__((weak)) uint32_t SDL_WasInit(uint32_t f) { return stub_SDL_WasInit(f); }
__attribute__((weak)) int SDL_VideoInit(const char* d) { return stub_SDL_VideoInit(d); }
__attribute__((weak)) void SDL_VideoQuit() { stub_SDL_VideoQuit(); }
__attribute__((weak)) int SDL_InitSubSystem(uint32_t f) { return stub_SDL_InitSubSystem(f); }
__attribute__((weak)) void SDL_QuitSubSystem(uint32_t f) { stub_SDL_QuitSubSystem(f); }
__attribute__((weak)) int SDL_SetHint(const char* n, const char* v) { return stub_SDL_SetHint(n, v); }
__attribute__((weak)) int SDL_GL_SetAttribute(int a, int v) { return stub_SDL_GL_SetAttribute(a, v); }
__attribute__((weak)) void* SDL_GL_CreateContext(void* w) { return stub_SDL_GL_CreateContext(w); }
__attribute__((weak)) void SDL_GL_DeleteContext(void* c) { stub_SDL_GL_DeleteContext(c); }
__attribute__((weak)) int SDL_GL_MakeCurrent(void* w, void* c) { return stub_SDL_GL_MakeCurrent(w, c); }
__attribute__((weak)) void SDL_GL_SwapWindow(void* w) { stub_SDL_GL_SwapWindow(w); }

} // extern "C"

void registerSDL2Stubs() {
    fprintf(stderr, "[SDL2-STUB] Registering SDL2 stub functions\n");

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

    // Renderer
    registerFunction("SDL_CreateRenderer", reinterpret_cast<void*>(stub_SDL_CreateRenderer));
    registerFunction("SDL_DestroyRenderer", reinterpret_cast<void*>(stub_SDL_DestroyRenderer));
    registerFunction("SDL_RenderClear", reinterpret_cast<void*>(stub_SDL_RenderClear));
    registerFunction("SDL_RenderPresent", reinterpret_cast<void*>(stub_SDL_RenderPresent));
    registerFunction("SDL_SetRenderDrawColor", reinterpret_cast<void*>(stub_SDL_SetRenderDrawColor));

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

    fprintf(stderr, "[SDL2-STUB] Registered %zu stub functions\n", sFunctionCache.size());
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
