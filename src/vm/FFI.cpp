/*
 * FFI.cpp - Foreign Function Interface for Pharo VM
 *
 * Implements UFFI/TFFI primitives and SDL2 stub functions.
 * Uses libffi for native callouts on ARM64.
 *
 * Copyright (c) 2025-2026 Aaron Wohl. Licensed under the MIT License.
 *
 * The FFI primitive interface is defined by the Pharo project
 * (https://pharo.org). Uses libffi (MIT) and SDL2 (zlib).
 * See THIRD_PARTY_LICENSES for upstream license details.
 */

#include <cctype>
#include "FFI.hpp"
#include "../platform/EventQueue.hpp"
#include "../platform/DisplaySurface.hpp"
#include "../platform/PlatformBridge.h"
#include "DebugSettings.hpp"
#include "DebugVars.hpp"
#include "SDL2Raster.hpp"
#include "SDL2Blit.hpp"
#include "SDL2Sys.hpp"
#include "SDL2Base.hpp"
#include "SDL2RW.hpp"
#include "SDL2Rect.hpp"
#include "SDL2Pixels.hpp"
#include "SDL2Surface.hpp"
#include "SDL2Audio.hpp"
#include "SDL2Iconv.hpp"
#include "SDL2Utf.hpp"
#include "SDL2Events.hpp"
#include <algorithm>
#include <cstdio>     // fopen — dladdr'd to locate the platform's libc directory
#include <cstdlib>
#include <cstring>
#include <chrono>
#ifdef __APPLE__
#include <mach-o/dyld.h>   // _NSGetExecutablePath (exe-dir library search)
#endif
#include <dlfcn.h>
#include <ffi.h>
#include <unistd.h>
#include <vector>
#include <functional>
#include <string>
#include <unordered_map>

#ifdef __APPLE__
#include <TargetConditionals.h>
#include <CoreFoundation/CoreFoundation.h>
#include <objc/runtime.h>
#endif

#include <atomic>
#include <unordered_set>

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

// ========== ObjC runtime wrappers for image relaunch ==========
// When the Pharo image restarts within the same app process (quit without
// save, then relaunch), ObjC classes registered in the first run persist
// in the runtime. objc_allocateClassPair returns NULL for duplicate names,
// and passing NULL to objc_registerClassPair causes SIGSEGV.
// These wrappers handle the relaunch case transparently.
#ifdef __APPLE__
static std::unordered_set<Class> sPreexistingClasses;

static Class safe_objc_allocateClassPair(Class superclass, const char* name, size_t extraBytes) {
    Class cls = objc_allocateClassPair(superclass, name, extraBytes);
    if (cls) return cls;

    // Allocation failed — check if the class already exists (relaunch case)
    cls = objc_getClass(name);
    if (cls) {
        sPreexistingClasses.insert(cls);
        return cls;
    }
    return nullptr;  // Genuine failure (bad superclass, etc.)
}

static void safe_objc_registerClassPair(Class cls) {
    if (!cls) return;  // Guard against NULL
    if (sPreexistingClasses.count(cls)) return;  // Already registered from previous run
    objc_registerClassPair(cls);
}

static void safe_objc_disposeClassPair(Class cls) {
    if (!cls) return;
    if (sPreexistingClasses.count(cls)) {
        sPreexistingClasses.erase(cls);
        return;  // Don't dispose classes we didn't allocate this run
    }
    objc_disposeClassPair(cls);
}

static void registerObjCRuntimeWrappers() {
    registerFunction("objc_allocateClassPair",
                     reinterpret_cast<void*>(safe_objc_allocateClassPair));
    registerFunction("objc_registerClassPair",
                     reinterpret_cast<void*>(safe_objc_registerClassPair));
    registerFunction("objc_disposeClassPair",
                     reinterpret_cast<void*>(safe_objc_disposeClassPair));
}
#endif

#if defined(__linux__)
// Give the IMAGE the library search path stock Cog gives it.
//
// A Pharo distribution's `pharo` is a shell WRAPPER, not the VM: it runs `ldd`
// on the real binary to find the directory holding its libc, then execs it with
//
//     LD_LIBRARY_PATH="$PLUGINS:$PLATFORMLIBDIR:/lib:/usr$PLATFORMLIBDIR:/usr/lib:$LD_LIBRARY_PATH"
//
// which on aarch64 Ubuntu contributes /lib/aarch64-linux-gnu and
// /usr/lib/aarch64-linux-gnu.  Image-side library finders READ that variable:
// `FFIUnix64LibraryFinder>>knownPaths` is hardcoded x86_64-only
// ('/lib/x86_64-linux-gnu', '/lib64', '/usr/lib64', '/usr/lib',
// '/usr/lib/x86_64-linux-gnu', '/usr/local/lib') and so is
// `SQLite3Library>>unix64LibraryName`, which checks `libraryPath exists` and
// signals 'Module not found.' before the VM is ever consulted.
//
// We are a bare ELF binary with no wrapper, so on aarch64 the image saw no
// arch directory at all: the 2026-08-11 arm sweep scored pharo-rdbms-pharo-
// sqlite3 at pass=122/err=0 on Cog and pass=11/err=111 on ours, every error
// 'Error: Module not found.'  That is not an FFI-loader gap — c1d6eef7 fixed
// the candidate list and changed nothing here — it is a missing environment.
// Reproducing the wrapper's variable is the fix, and it also feeds the image's
// own finders, which no VM-side search path can do.
//
// PHARO_NO_PLATFORM_LIB_PATH=1 leaves the environment as inherited.
// Platform library directories, filled in by ensurePlatformLibraryPath() at
// startup and reused by getLibSearchPaths() so the environment we hand the
// image and the paths we search ourselves come from one computation.
static std::vector<std::string> sPlatformLibDirs;

static void ensurePlatformLibraryPath() {
    if (GET_DEBUG_BOOL(PHARO_NO_PLATFORM_LIB_PATH)) return;

    // dladdr on a libc symbol is the in-process equivalent of the wrapper's
    // `ldd | fgrep /libc.` — it answers the path the loader actually resolved.
    std::string platformLibDir;
    Dl_info info;
    if (dladdr(reinterpret_cast<void*>(&fopen), &info) && info.dli_fname) {
        std::string p = info.dli_fname;
        size_t slash = p.find_last_of('/');
        if (slash != std::string::npos && slash > 0) platformLibDir = p.substr(0, slash);
    }

    std::vector<std::string> dirs;
    if (!platformLibDir.empty() && platformLibDir != "/lib" && platformLibDir != "/usr/lib") {
        dirs.push_back(platformLibDir);
        dirs.push_back("/lib");
        dirs.push_back("/usr" + platformLibDir);
    }
    dirs.push_back("/usr/lib");
    sPlatformLibDirs = dirs;   // getLibSearchPaths searches these directly

    // Existing value wins for anything it already names (Cog appends it last).
    const char* cur = GET_DEBUG_STR(LD_LIBRARY_PATH);
    std::string value;
    for (const auto& d : dirs) {
        if (cur) {
            // Skip a directory the inherited value already lists, so repeated
            // in-process image loads can't grow the variable without bound.
            std::string needle = std::string(":") + cur + ":";
            if (needle.find(":" + d + ":") != std::string::npos) continue;
        }
        value += d;
        value += ":";
    }
    if (cur && *cur) value += cur;
    else if (!value.empty()) value.pop_back();   // drop the trailing ':'
    if (!value.empty()) setenv("LD_LIBRARY_PATH", value.c_str(), 1);
}
#endif  // __linux__

bool initializeFFI() {
    if (sInitialized) return true;
    sInitialized = true;

#if defined(__linux__)
    ensurePlatformLibraryPath();
#endif

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

#ifdef __APPLE__
    // ObjC runtime wrappers — handle image relaunch within same process
    registerObjCRuntimeWrappers();
#endif

    // FreeType and libgit2 stubs are NOT registered at init time.
    // lookupFunction() tries real libraries first (via dlsym/dlopen),
    // and only falls back to generic stubs if the real libs aren't available
    // (e.g., on iOS where Homebrew doesn't exist).

    return true;
}

static void resetAllFFIState();  // Forward declaration — defined after all statics

void shutdownFFI() {
    sFunctionCache.clear();
    sInitialized = false;
    resetAllFFIState();
}

static std::string moduleBaseName(const std::string& moduleName) {
    size_t slash = moduleName.find_last_of('/');
    return slash == std::string::npos ? moduleName : moduleName.substr(slash + 1);
}

bool isSDL3ModuleName(const std::string& moduleName) {
    return moduleBaseName(moduleName).find("SDL3") != std::string::npos;
}

bool isSDL2ModuleName(const std::string& moduleName) {
    const std::string base = moduleBaseName(moduleName);
    return base.find("SDL") != std::string::npos && !isSDL3ModuleName(moduleName);
}

static std::unordered_set<void*> sSDL3Handles;
void noteSDL3ModuleHandle(void* handle) { if (handle) sSDL3Handles.insert(handle); }
bool isSDL3ModuleHandle(void* handle) { return handle && sSDL3Handles.count(handle) != 0; }

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
std::vector<std::string> getLibSearchPaths() {
    std::vector<std::string> paths;
    if (!sAppFrameworksPath.empty()) {
        paths.push_back(sAppFrameworksPath);
    }
#if (defined(__APPLE__) && !(TARGET_OS_IPHONE && !TARGET_OS_MACCATALYST)) || defined(__linux__)
    // The executable's own directory FIRST: `Smalltalk vm directory` is where
    // the image-side library finders expect VM-staged libraries, and the
    // build stages fixture libs there (libTestLibrary.dylib for the TFUFFI*
    // suites — mirrors TestLibrary.dll next to the exe on Windows).  Without
    // this, a bare-name lookup like 'libTestLibrary.dylib' only resolved
    // symbols the exe happens to re-export (-Wl,-u list) and every
    // dylib-only fixture failed with SymbolNotFoundError (2026-07-04).
    // Linux too (2026-07-06): the x86 box failed all ~170 TF*/TFUFFI*
    // fixture tests + LibTTYTest because libTestLibrary.so/libtty.so next
    // to the exe were unreachable from a bare-name lookup.
    {
        static std::string sExeDir;  // resolved once
        if (sExeDir.empty()) {
            std::string p;
#if defined(__APPLE__)
            char buf[4096]; uint32_t sz = sizeof(buf);
            if (_NSGetExecutablePath(buf, &sz) == 0) {
                if (char* rp = realpath(buf, nullptr)) {
                    p = rp;
                    free(rp);
                }
            }
#else
            char buf[4096];
            ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
            if (n > 0) { buf[n] = '\0'; p = buf; }
#endif
            size_t slash = p.find_last_of('/');
            if (slash != std::string::npos) sExeDir = p.substr(0, slash);
            if (sExeDir.empty()) sExeDir = ".";  // sentinel: don't retry
        }
        if (sExeDir != ".") paths.push_back(sExeDir);
    }
#endif
#if defined(__linux__)
    // The platform library dirs ensurePlatformLibraryPath() published.  They
    // are needed as SEARCH PATHS and not only as an environment variable:
    // glibc snapshots LD_LIBRARY_PATH at process start, so the setenv() we do
    // afterwards steers the IMAGE's finders but not our own bare-name dlopen.
    for (const auto& d : sPlatformLibDirs) paths.push_back(d);
#endif
#if !(TARGET_OS_IPHONE && !TARGET_OS_MACCATALYST)
    // Mac Catalyst / macOS: try Homebrew as fallback (won't work if
    // code-signature team ID mismatch, but useful for ad-hoc builds)
    paths.push_back("/opt/homebrew/lib");
    paths.push_back("/usr/local/lib");
#endif
    return paths;
}

std::vector<std::string> moduleCandidates(const std::string& moduleName) {
    std::vector<std::string> candidates;
    candidates.push_back(moduleName);
    const bool bare = moduleName.compare(0, 3, "lib") != 0;
    if (bare) candidates.push_back("lib" + moduleName);
    if (moduleName.find(".dylib") == std::string::npos
            && moduleName.find(".so") == std::string::npos) {
#ifdef __APPLE__
        candidates.push_back(moduleName + ".dylib");
        if (bare) candidates.push_back("lib" + moduleName + ".dylib");
#else
        candidates.push_back(moduleName + ".so");
        if (bare) candidates.push_back("lib" + moduleName + ".so");
        // Distributions ship the runtime as a VERSIONED soname and put the
        // bare `.so` symlink in the -dev package, which build boxes do not
        // install: Ubuntu has libsqlite3.so.0 but no libsqlite3.so.  Without
        // these, a bare module name is unloadable on a stock machine even
        // though the library is right there.
        for (const char* v : {".so.0", ".so.1", ".so.2", ".so.3"}) {
            candidates.push_back(moduleName + v);
            if (bare) candidates.push_back("lib" + moduleName + v);
        }
#endif
    }
    return candidates;
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

    std::vector<std::string> candidates = moduleCandidates(moduleName);

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
    if (isSDL3ModuleName(moduleName)) {
        // Real SDL3 only: the cache and the SDL_ no-op below belong to SDL2.
        if (!moduleName.empty() && moduleName[0] == '/') {
            if (void* h = dlopen(moduleName.c_str(), RTLD_NOW | RTLD_GLOBAL)) {
                noteSDL3ModuleHandle(h);
                return dlsym(h, funcName.c_str());
            }
        }
        return tryLoadFromSearchPaths(moduleBaseName(moduleName), funcName);
    }
    auto it = sFunctionCache.find(funcName);
    if (it != sFunctionCache.end()) {
        return it->second;
    }

    // An SDL_ symbol without a stub does not exist in this layer.  The lookup
    // fails, as it does on a real library that lacks the symbol, so that the image
    // raises its own error.  dlsym must not run: where a real SDL2 is linked in,
    // it would find a function that crashes on our fake window handles.  The
    // stubs of every SDL2 call the image can make are registered above.
    if (funcName.compare(0, 4, "SDL_") == 0) {
        return nullptr;
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
        // The absolute path did not yield the symbol.  Retry on its BASENAME
        // through our own search paths, which start at the executable's
        // directory (getLibSearchPaths above).
        //
        // This is what makes a wrong-ARCH host library recoverable.  The
        // image's own finder walks /opt/homebrew, /usr/local, /usr/lib and
        // hands us an absolute path; it has no way to know whether that file
        // matches the running process's architecture.  On an Apple Silicon
        // box with an arm64-only Homebrew, the x86_64 build under Rosetta
        // gets handed /opt/homebrew/lib/libcairo.2.dylib, dlopen refuses it,
        // and before 2026-08-22 the lookup stopped there -- the `[0] != '/'`
        // guard on the bare-name search below meant an absolute path was
        // never retried.  Every cairo_* lookup failed even with a perfectly
        // good x86_64 libcairo staged beside the binary.  Measured: 26 of
        // 26 x86_64 errors in SUnit batch 1-50, ~260 of ~287 across a full
        // sweep.
        //
        // The VM knows something the image cannot: which of several files
        // named libcairo.2.dylib this process can actually load.
        size_t slash = moduleName.find_last_of('/');
        std::string base = (slash == std::string::npos)
            ? moduleName : moduleName.substr(slash + 1);
        if (!base.empty() && base.find("SDL") == std::string::npos) {
            func = tryLoadFromSearchPaths(base, funcName);
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

    return missingLibraryStub(funcName);
}

// Fallback stubs for when real libraries are not available (e.g., iOS).
// These return error codes so the image handles failure gracefully.
void* missingLibraryStub(const std::string& funcName) {
    void* func = nullptr;
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
#if TARGET_OS_IPHONE && !TARGET_OS_MACCATALYST
    // Real-iOS-only: cairo can never be present and the image's world boot
    // touches cairo-adjacent paths, so a return-0 stub keeps startup alive.
    // NEVER enable this on desktop: unlike the FT_/git_ stubs above (whose
    // return values are error codes the image checks), cairo calls mutate
    // caller-owned structs / return pointers — a 0-stub silently fabricates
    // wrong DATA (AthensCairoMatrix reads back all-zero matrices, 26 Athens/
    // Cairo tests FAIL instead of ERRORing with SymbolNotFoundError; found
    // on the cairo-less x86 box 2026-07-06).  On desktop a missing cairo
    // must fail the symbol lookup so the image raises and tests error/skip.
    if (funcName.compare(0, 6, "cairo_") == 0) {
        static int cairoStubCount = 0;
        if (++cairoStubCount <= 5) {
            fprintf(stderr, "[CAIRO] stub registered: %s (returns 0, cairo not available)\n", funcName.c_str());
        } else if (cairoStubCount == 6) {
            fprintf(stderr, "[CAIRO] ... further cairo stub registrations suppressed\n");
        }
        static auto genericCairoNull = +[]() -> intptr_t { return 0; };
        func = reinterpret_cast<void*>(genericCairoNull);
        sFunctionCache[funcName] = func;
        return func;
    }
#endif

    return nullptr;
}

// Does the module name pin a library version -- libcrypto.1.1.dylib,
// libcairo.2.dylib, libcrypto.so.1.1?  Such a name asks for one ABI.
bool isVersionedLibraryName(const std::string& moduleName) {
    size_t slash = moduleName.find_last_of('/');
    std::string base = (slash == std::string::npos) ? moduleName : moduleName.substr(slash + 1);
    auto versionText = [](const std::string& v) {
        if (v.empty() || !std::isdigit(static_cast<unsigned char>(v[0]))) return false;
        for (char c : v)
            if (!std::isdigit(static_cast<unsigned char>(c)) && c != '.') return false;
        return true;
    };
    const std::string dylib = ".dylib";
    if (base.size() > dylib.size()
            && base.compare(base.size() - dylib.size(), dylib.size(), dylib) == 0) {
        std::string stem = base.substr(0, base.size() - dylib.size());
        size_t dot = stem.find('.');
        return dot != std::string::npos && versionText(stem.substr(dot + 1));
    }
    size_t so = base.find(".so.");
    return so != std::string::npos && versionText(base.substr(so + 4));
}

// A symbol from the named module or nothing: its own search paths, then the
// missing-library stubs.  Never the process-wide namespace, which may hold a
// DIFFERENT version of the same library (a LibreSSL libcrypto loaded by a
// system framework answering for libcrypto.1.1.dylib, 2026-10-13).
void* lookupSymbolInModuleOnly(const std::string& moduleName, const std::string& funcName) {
    if (void* f = tryLoadFromSearchPaths(moduleName, funcName)) return f;
    return missingLibraryStub(funcName);
}

// Names explicitly registered via registerFunction — distinguishable from
// lazily-cached dlsym results and missing-library fallback stubs, so that
// lookupRegisteredFunction can answer "wrapper or nothing".
static std::unordered_set<std::string> sRegisteredFunctionNames;

void registerFunction(const std::string& funcName, void* funcPtr) {
    sFunctionCache[funcName] = funcPtr;
    sRegisteredFunctionNames.insert(funcName);
}

void* lookupRegisteredFunction(const std::string& funcName) {
    if (sRegisteredFunctionNames.count(funcName) == 0)
        return nullptr;
    auto it = sFunctionCache.find(funcName);
    return it == sFunctionCache.end() ? nullptr : it->second;
}

// SDL2 stub functions -- make OSWindow think SDL2 is available.
// Supports multiple windows/textures (Emergency Debugger creates separate ones).

static bool sSDL2Initialized = false;

// Per-window state
struct SDLWindowState {
    int width;
    int height;
    std::string title;
    int x = 0;                  // position in desktop coordinates
    int y = 0;
    uint32_t flags = 0;         // SDL_WindowFlags as the image sees them
    sdl2::SurfaceRaw* surface = nullptr;  // SDL_GetWindowSurface, made on demand
    int minW = 0, minH = 0, maxW = 0, maxH = 0;   // SDL_SetWindowMinimumSize and MaximumSize (0: none)
    float opacity = 1.0f;                         // SDL_GetWindowOpacity
    float brightness = 1.0f;                      // SDL_GetWindowBrightness
    std::unordered_map<std::string, void*> data;  // SDL_SetWindowData
    bool mouseGrab = false, keyboardGrab = false; // SDL_SetWindowMouseGrab, SDL_SetWindowKeyboardGrab
    bool hasMouseRect = false;                    // SDL_SetWindowMouseRect
    sdl2::Rect mouseRect = { 0, 0, 0, 0 };
};

// Per-texture state
struct SDLTextureState {
    uint32_t* pixels;
    int width;
    int height;
    int pitch;
    void* renderer;  // Which renderer owns this texture
    bool usesDisplaySurface;  // True when LockTexture returned the display surface buffer
    bool lastLockWasFull = true;  // False when the last LockTexture named a rectangle
    uint32_t format = 0;          // SDL_PixelFormatEnum given to SDL_CreateTexture
    int access = 0;               // SDL_TextureAccess
    uint32_t blendMode = 0;       // SDL_BLENDMODE_NONE until SDL_SetTextureBlendMode
    uint8_t rMod = 255, gMod = 255, bMod = 255, aMod = 255;
    bool linear = false;          // SDL_ScaleModeLinear
    void* userData = nullptr;     // SDL_SetTextureUserData
    sdl2::SurfaceRaw* lockedSurface = nullptr;   // SDL_LockTextureToSurface
};

// Viewport, scale, clip and logical size of one render target.
struct RenderView {
    double vx = 0, vy = 0, vw = 0, vh = 0;   // viewport in target pixels
    bool viewportSet = false;                // false: the viewport is the whole target
    bool clipEnabled = false;
    double cx = 0, cy = 0, cw = 0, ch = 0;   // clip rectangle in target pixels
    float sx = 1.0f, sy = 1.0f;              // SDL_RenderSetScale
    int logicalW = 0, logicalH = 0;          // SDL_RenderSetLogicalSize
};

// Per-renderer state
struct SDLRendererState {
    void* window;
    void* currentTexture;  // Most recently used texture
    uint8_t drawR = 0, drawG = 0, drawB = 0, drawA = 255;  // SDL default draw colour
    uint32_t drawBlend = 0;          // SDL_BLENDMODE_NONE
    void* target = nullptr;          // SDL_SetRenderTarget texture, or null for the window
    RenderView view;                 // viewport, scale, clip and logical size of the current target
    RenderView viewBackup;           // the window state while a texture is the target
    bool integerScale = false;       // SDL_RenderSetIntegerScale
    uint32_t flags = 0;              // SDL_RendererFlags answered by SDL_GetRendererInfo
    std::vector<uint32_t> backbuffer;  // window pixels (ARGB8888) when no display surface serves
    int backW = 0, backH = 0;
};

constexpr uintptr_t kFirstHandle = 0x10000;   // handles below this value are not pointers
static uintptr_t sNextHandle = kFirstHandle;  // Incrementing unique handles
// An unknown window handle is "Invalid window" in SDL (the first line of
// nearly every window function).
#define WINDOW_CHECK(w, retval) \
    if (!sWindows.count(w)) { sdl2::sys::lastError() = "Invalid window"; return retval; }
static std::unordered_map<void*, SDLWindowState> sWindows;
static std::unordered_map<void*, SDLTextureState> sTextures;
static std::unordered_map<void*, SDLRendererState> sRenderers;
static void* sMainRenderer = nullptr;  // First renderer is the "main" one (renders to display)
static void* sMainWindow = nullptr;    // First window is the "main" one (receives events)
static bool sSDLRenderingActive = false;  // Set when SDL_RenderPresent first copies to display

// Pending synthetic window events (SDL2 sends these when window is created/shown)
static std::queue<uint8_t> sPendingWindowEvents;
static std::mutex sPendingWindowEventsMx;   // the UI thread pushes resize events, the VM thread pops

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
// Text input state (declared here for resetSDL2State).  SDL starts it on, unless
// the host has a screen keyboard (a phone): then the image starts it.
#if defined(__APPLE__) && TARGET_OS_IPHONE && !TARGET_OS_MACCATALYST
static bool sTextInputActive = false;
#else
static bool sTextInputActive = true;
#endif
static bool sSDL2PollEventFlagSet = false;  // Tracks whether SDL_PollEvent has activated event polling
constexpr int kScancodeCount = 512;           // SDL_NUM_SCANCODES
static uint8_t sKeyState[kScancodeCount];     // SDL_GetKeyboardState; updated by the host key events
static int sRelMouseDx = 0, sRelMouseDy = 0;  // motion since the last SDL_GetRelativeMouseState
static bool sScreenSaverEnabled = false;      // SDL_EnableScreenSaver, SDL_DisableScreenSaver (SDL disables it at video start)

static void resetAllFFIState() {
    sModuleHandleCache.clear();
    sAppFrameworksPath.clear();
    sSDL2Initialized = false;
    sNextHandle = 0x10000;
    for (auto& kv : sWindows) sdl2::freeSurface(kv.second.surface);
    for (auto& kv : sTextures) free(kv.second.pixels);
    sWindows.clear();
    sTextures.clear();
    sRenderers.clear();
    sdl2::base::clearHints();
    sdl2::sys::lastError().clear();
    sMainRenderer = nullptr;
    sMainWindow = nullptr;
    sSDLRenderingActive = false;
    {
        std::lock_guard<std::mutex> lk(sPendingWindowEventsMx);
        while (!sPendingWindowEvents.empty()) sPendingWindowEvents.pop();
    }
    sWindowCreated = false;
    sPollCountdown = 0;
    g_firstExposedDelivered = false;
    sMouseX = 0;
    sMouseY = 0;
    sMouseButtons = 0;
    sKeyModState = 0;
    sTextInputActive = false;
    sSDL2PollEventFlagSet = false;
    g_emergencyDebuggerTriggered.store(false, std::memory_order_relaxed);
}

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
// The host (a UI thread) reports a new display size here.  This function only
// records the size.  The VM thread applies it at the next poll
// (applyNotifiedResize), because the window table and the poll countdown belong
// to the VM thread.
static std::atomic<int> sNotifiedWidth{ 0 }, sNotifiedHeight{ 0 };
static std::atomic<bool> sResizeNotified{ false };

void ffi_notifyDisplayResize(int width, int height) {
    sNotifiedWidth.store(width, std::memory_order_relaxed);
    sNotifiedHeight.store(height, std::memory_order_relaxed);
    sResizeNotified.store(true, std::memory_order_release);
}

// VM thread: make the SDL window state match the new display size and queue the
// events that tell the image.
static void applyNotifiedResize() {
    if (!sResizeNotified.exchange(false, std::memory_order_acquire)) return;
    if (!sMainWindow) return;
    const int width = sNotifiedWidth.load(std::memory_order_relaxed);
    const int height = sNotifiedHeight.load(std::memory_order_relaxed);

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

    std::lock_guard<std::mutex> lk(sPendingWindowEventsMx);
    sPendingWindowEvents.push(SDL_WINDOWEVENT_SIZE_CHANGED);
    sPendingWindowEvents.push(SDL_WINDOWEVENT_EXPOSED);
}

// The subsystems that are initialized, as SDL_WasInit(0) answers them.  Video
// and joystick imply events, and the game controller implies the joystick.
static std::atomic<uint32_t> sInitMask{ 0 };
constexpr uint32_t kInitJoystick = 0x200, kInitGameController = 0x2000, kInitVideo = 0x20, kInitEvents = 0x4000;
static void initSubsystems(uint32_t flags) {
    if (flags & (kInitVideo | kInitJoystick | kInitGameController)) flags |= kInitEvents;
    if (flags & kInitGameController) flags |= kInitJoystick;
    sInitMask.fetch_or(flags);
}

int stub_SDL_Init(uint32_t flags) {
    sSDL2Initialized = true;
    initSubsystems(flags);
    return 0;
}

void stub_SDL_Quit() {
    sSDL2Initialized = false;
    sInitMask.store(0);
}

void stub_SDL_GetVersion(void* ver) {
    if (ver) {
        uint8_t* v = static_cast<uint8_t*>(ver);
        v[0] = 2;   // major
        v[1] = 26;  // minor
        v[2] = 5;   // patch (the SDL release that the headers and the stubs follow, docs/credits.md)
    }
}

// SDL_GetError answers the message of the last failed call, and an empty
// string before any call failed (the SDL2 rule).
static int sdlFail(int result, const char* message) {
    sdl2::sys::lastError() = message;
    return result;
}

static void* sdlFailNull(const char* message) {
    sdl2::sys::lastError() = message;
    return nullptr;
}

const char* stub_SDL_GetError() {
    return sdl2::sys::lastError().c_str();
}

// Window flags (SDL_video.h), named instead of written as hex.
enum SDLWindowFlag : uint32_t {
    kWindowFullscreen = 1u << 0, kWindowOpenGL = 1u << 1, kWindowShown = 1u << 2,
    kWindowHidden = 1u << 3, kWindowBorderless = 1u << 4, kWindowResizable = 1u << 5,
    kWindowMinimized = 1u << 6, kWindowMaximized = 1u << 7,
    kWindowFullscreenDesktop = kWindowFullscreen | (1u << 12),
    kWindowVulkan = 1u << 28, kWindowMetal = 1u << 29
};
// SDL_WINDOWPOS_UNDEFINED / SDL_WINDOWPOS_CENTERED carry a display index in the
// low 16 bits and a marker in the high 16 bits.
constexpr uint32_t kWindowPosHighBits = ~0u << 16;
constexpr uint32_t kWindowPosUndefinedMark = 8191u << 16;   // SDL_WINDOWPOS_UNDEFINED_MASK
constexpr uint32_t kWindowPosCenteredMark = 12287u << 16;   // SDL_WINDOWPOS_CENTERED_MASK

static int displayWidth() {
    return pharo::gDisplaySurface ? pharo::gDisplaySurface->width() : 1024;
}

static int displayHeight() {
    return pharo::gDisplaySurface ? pharo::gDisplaySurface->height() : 768;
}

// SDL_WINDOWPOS_UNDEFINED places the window at the display origin;
// SDL_WINDOWPOS_CENTERED centres it on the display; any other value is a position.
static int resolveWindowPosition(int pos, int windowExtent, int displayExtent) {
    const uint32_t bits = static_cast<uint32_t>(pos);
    if ((bits & kWindowPosHighBits) == kWindowPosCenteredMark) return (displayExtent - windowExtent) / 2;
    if ((bits & kWindowPosHighBits) == kWindowPosUndefinedMark) return 0;
    return pos;
}

const char* stub_SDL_GetCurrentVideoDriver();

// SDL_ContextNotSupported: the text for a graphics API the video driver lacks.
static std::string contextNotSupportedText(const char* what) {
    return std::string(what) + " support is either not configured in SDL or not available in current SDL video driver (" +
           stub_SDL_GetCurrentVideoDriver() + ") or platform";
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
    // SDL accepts one graphics API flag, and only when the video driver has
    // that API.  There is no Vulkan or Metal driver here, so each of those
    // flags fails as it fails on a video driver without the API.  The OpenGL
    // flag is accepted, as the cocoa driver of libSDL2 accepts it: the Bloc
    // window host of the image always sets it and needs a window handle.  The
    // later SDL_GL_CreateContext call is the one that fails.
    constexpr uint32_t kGraphicsApiFlags = kWindowOpenGL | kWindowMetal | kWindowVulkan;
    const uint32_t graphicsFlags = flags & kGraphicsApiFlags;
    if (graphicsFlags & (graphicsFlags - 1)) return sdlFailNull("Conflicting window flags specified");
    if (flags & kWindowVulkan) return sdlFailNull(contextNotSupportedText("Vulkan").c_str());
    if (flags & kWindowMetal) return sdlFailNull(contextNotSupportedText("Metal").c_str());
    void* handle = reinterpret_cast<void*>(sNextHandle++);
    SDLWindowState state;
    state.width = w;
    state.height = h;
    state.title = title ? title : "";
    // SDL_CreateWindow adds SDL_WINDOW_SHOWN unless the caller asked for
    // SDL_WINDOW_HIDDEN, and resolves the two position markers.
    state.flags = (flags & ~(kWindowShown | kWindowHidden)) |
                  ((flags & kWindowHidden) ? kWindowHidden : kWindowShown);
    state.x = resolveWindowPosition(x, w, displayWidth());
    state.y = resolveWindowPosition(y, h, displayHeight());
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
    WINDOW_CHECK(window, )
    auto it = sWindows.find(window);
    if (it == sWindows.end()) return;
    sdl2::freeSurface(it->second.surface);
    sWindows.erase(it);
}

void stub_SDL_GetWindowSize(void* window, int* w, int* h) {
    // PHARO_SDL_TRACE=1 — count calls so we can tell whether a hang
    // path runs through here heavily in headless eval.
    static uint64_t callCount = 0;
    const bool trace = GET_DEBUG_BOOL(PHARO_SDL_TRACE);
    if (trace) {
        callCount++;
        if ((callCount & 0xFFF) == 0) {
            fprintf(stderr, "[SDL-TRACE] GetWindowSize called %llu times\n",
                    (unsigned long long)callCount);
        }
    }

    if (!sWindows.count(window)) {
        sdl2::sys::lastError() = "Invalid window";
        if (!window) return;   // a null window leaves the outputs alone, as SDL does
    }
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
    WINDOW_CHECK(window, )
    auto it = sWindows.find(window);
    if (it != sWindows.end()) {
        // The size stays inside the minimum and maximum size of the window.
        SDLWindowState& s = it->second;
        if (s.minW && w < s.minW) w = s.minW;
        if (s.minH && h < s.minH) h = s.minH;
        if (s.maxW && w > s.maxW) w = s.maxW;
        if (s.maxH && h > s.maxH) h = s.maxH;
        s.width = w;
        s.height = h;
    }
}

void stub_SDL_SetWindowTitle(void* window, const char* title) {
    WINDOW_CHECK(window, )
    auto it = sWindows.find(window);
    if (it != sWindows.end()) it->second.title = title ? title : "";
}

const char* stub_SDL_GetWindowTitle(void* window) {
    WINDOW_CHECK(window, "")
    auto it = sWindows.find(window);
    if (it == sWindows.end()) return "";
    return it->second.title.c_str();
}

static void setWindowFlags(void* window, uint32_t clear, uint32_t set) {
    auto it = sWindows.find(window);
    if (it != sWindows.end()) it->second.flags = (it->second.flags & ~clear) | set;
}

// The flags the image sees.  The display window follows the size of the app
// view, so it is always resizable (the answer before window flags were stored).
static uint32_t windowFlagsOf(void* window) {
    auto it = sWindows.find(window);
    if (it == sWindows.end()) return 0;
    const bool displayWindow = window == sMainWindow && pharo::gDisplaySurface;
    return it->second.flags | (displayWindow ? kWindowResizable : 0);
}

void stub_SDL_ShowWindow(void* window) {
    WINDOW_CHECK(window, )
    setWindowFlags(window, kWindowHidden, kWindowShown);
}

void stub_SDL_HideWindow(void* window) {
    WINDOW_CHECK(window, )
    setWindowFlags(window, kWindowShown, kWindowHidden);
}

void stub_SDL_RaiseWindow(void* window) {
    WINDOW_CHECK(window, )
}

// A hidden window cannot be minimized, and only a resizable window can be
// maximized; SDL ignores the call otherwise.
void stub_SDL_MinimizeWindow(void* window) {
    WINDOW_CHECK(window, )
    if (windowFlagsOf(window) & kWindowHidden) return;
    setWindowFlags(window, kWindowMaximized, kWindowMinimized);
}

void stub_SDL_MaximizeWindow(void* window) {
    WINDOW_CHECK(window, )
    if (!(windowFlagsOf(window) & kWindowResizable)) return;
    setWindowFlags(window, kWindowMinimized, kWindowMaximized);
}

void stub_SDL_RestoreWindow(void* window) {
    WINDOW_CHECK(window, )
    setWindowFlags(window, kWindowMinimized | kWindowMaximized, 0);
}

void stub_SDL_SetWindowBordered(void* window, int bordered) {
    WINDOW_CHECK(window, )
    if (bordered) setWindowFlags(window, kWindowBorderless, 0);
    else setWindowFlags(window, 0, kWindowBorderless);
}

void stub_SDL_SetWindowResizable(void* window, int resizable) {
    WINDOW_CHECK(window, )
    if (resizable) setWindowFlags(window, 0, kWindowResizable);
    else setWindowFlags(window, kWindowResizable, 0);
}

// A hit test needs window system support for borderless window drag.  The app
// has none, and SDL answers a platform without it the same way.
int stub_SDL_SetWindowHitTest(void* window, void* callback, void* data) {
    if (!sWindows.count(window)) return sdlFail(-1, "Invalid window");
    return sdlFail(-1, "That operation is not supported");
}

int stub_SDL_SetWindowInputFocus(void* window) {
    if (!sWindows.count(window)) return sdlFail(-1, "Invalid window");
    return 0;
}

uint32_t stub_SDL_GetWindowID(void* window) {
    WINDOW_CHECK(window, 0)
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
    auto it = sWindows.find(window);
    if (it == sWindows.end()) return sdlFail(-1, "Invalid window");
    // The flags are 0, SDL_WINDOW_FULLSCREEN or SDL_WINDOW_FULLSCREEN_DESKTOP.
    it->second.flags = (it->second.flags & ~kWindowFullscreenDesktop) | (flags & kWindowFullscreenDesktop);
    return 0;
}

void stub_SDL_GetWindowPosition(void* window, int* x, int* y) {
    WINDOW_CHECK(window, )
    auto it = sWindows.find(window);
    if (x) *x = it == sWindows.end() ? 0 : it->second.x;
    if (y) *y = it == sWindows.end() ? 0 : it->second.y;
}

void stub_SDL_SetWindowPosition(void* window, int x, int y) {
    WINDOW_CHECK(window, )
    auto it = sWindows.find(window);
    if (it == sWindows.end()) return;
    it->second.x = resolveWindowPosition(x, it->second.width, displayWidth());
    it->second.y = resolveWindowPosition(y, it->second.height, displayHeight());
}

void stub_SDL_SetWindowIcon(void* window, void* icon) {
    WINDOW_CHECK(window, )
}

int stub_SDL_GetWindowWMInfo(void* window, void* info) {
    WINDOW_CHECK(window, 0)
    sdl2::sys::lastError() = "That operation is not supported";   // the answer of a driver without window manager info
    // Return failure — we don't have real WM info on Mac Catalyst.
    // Returning success with zeroed data causes SDLOSXPlatform>>afterSetWindowTitle:
    // to access zeroed struct fields as Smalltalk objects (classIdx=0 crash),
    // which kills the OSSDL2Driver setup process before the event loop starts.
    return 0;  // SDL_FALSE
}

// ----- Renderers, textures and software drawing -----
//
// A renderer draws into a software target: the texture named by
// SDL_SetRenderTarget, or else the window.  The window of the display
// renderer is the app display surface; the window of any other renderer is a
// private ARGB8888 buffer the size of the window.  SDL_RenderReadPixels reads
// that same target, so a drawing and its read-back agree.
//
// The display renderer (the first renderer, while the app has a display
// surface) keeps the path the app depends on.  The image locks its streaming
// texture, which returns the display surface bytes, and draws into them.
// SDL_RenderCopy of that texture only names it, SDL_RenderPresent shows the
// display surface, and SDL_RenderClear does nothing: the copy at present
// replaces every pixel, and a clear on the shared surface would show a black
// frame to the Metal thread between the clear and the copy.

enum SDLRendererFlag : uint32_t {
    kRendererSoftware = 1u << 0, kRendererAccelerated = 1u << 1,
    kRendererPresentVSync = 1u << 2, kRendererTargetTexture = 1u << 3
};
enum SDLTextureAccess : int { kAccessStatic = 0, kAccessStreaming = 1, kAccessTarget = 2 };
constexpr int kMaxTextureSize = 16384;      // limit of the Metal renderer in the app
constexpr int kRenderDriverCount = 1;
constexpr const char* kRenderDriverName = "metal";
constexpr const char* kSoftwareDriverName = "software";

struct SDLRendererInfoRaw {                 // SDL_RendererInfo
    const char* name;
    uint32_t flags;
    uint32_t numTextureFormats;
    uint32_t textureFormats[16];
    int maxTextureWidth;
    int maxTextureHeight;
};
static_assert(sizeof(SDLRendererInfoRaw) == 88, "SDL_RendererInfo");

struct SDLDisplayModeRaw {                  // SDL_DisplayMode
    uint32_t format;
    int w, h, refreshRate;
    void* driverData;
};
static_assert(sizeof(SDLDisplayModeRaw) == 24, "SDL_DisplayMode");

static SDLRendererState* findRenderer(void* handle) {
    auto it = sRenderers.find(handle);
    return it == sRenderers.end() ? nullptr : &it->second;
}

static SDLTextureState* findTexture(void* handle) {
    auto it = sTextures.find(handle);
    return it == sTextures.end() ? nullptr : &it->second;
}

// True when the renderer is the display renderer and draws to the window.
static bool isDisplayRenderer(void* handle, const SDLRendererState& rs) {
    return handle == sMainRenderer && pharo::gDisplaySurface && !rs.target;
}

static sdl2::Pixels texturePixels(const SDLTextureState& t) {
    sdl2::Pixels p;
    p.data = reinterpret_cast<uint8_t*>(t.pixels);
    p.w = t.width; p.h = t.height; p.pitch = t.pitch;
    p.fmt = sdl2::formatInfo(t.format);
    return p;
}

// The pixels a renderer draws into.  False when the target is gone.
static bool renderTargetPixels(void* handle, SDLRendererState& rs, sdl2::Pixels& out, bool* isDisplay = nullptr) {
    if (isDisplay) *isDisplay = false;
    if (rs.target) {
        SDLTextureState* t = findTexture(rs.target);
        if (!t || !t->pixels) return false;
        out = texturePixels(*t);
        return out.valid();
    }
    if (handle == sMainRenderer && pharo::gDisplaySurface) {
        out.data = reinterpret_cast<uint8_t*>(pharo::gDisplaySurface->pixels());
        out.w = pharo::gDisplaySurface->width();
        out.h = pharo::gDisplaySurface->height();
        out.pitch = static_cast<int>(pharo::gDisplaySurface->pitch());
        out.fmt = sdl2::formatInfo(sdl2::kFormatARGB8888);
        if (isDisplay) *isDisplay = true;
        return out.valid();
    }
    int w = displayWidth(), h = displayHeight();
    if (sWindows.count(rs.window)) stub_SDL_GetWindowSize(rs.window, &w, &h);
    if (w <= 0 || h <= 0) return false;
    if (rs.backW != w || rs.backH != h) {
        rs.backbuffer.assign(static_cast<size_t>(w) * h, 0);
        rs.backW = w; rs.backH = h;
    }
    out.data = reinterpret_cast<uint8_t*>(rs.backbuffer.data());
    out.w = w; out.h = h; out.pitch = w * 4;
    out.fmt = sdl2::formatInfo(sdl2::kFormatARGB8888);
    return true;
}

static void finishDrawing(bool isDisplay) {
    if (isDisplay && pharo::gDisplaySurface) pharo::gDisplaySurface->update();
}

static uint32_t rendererDriverFlags(uint32_t requested) {
    uint32_t flags = (requested & kRendererSoftware) ? kRendererSoftware : kRendererAccelerated;
    return flags | kRendererTargetTexture;
}

static void fillRendererInfo(SDLRendererInfoRaw* info, uint32_t flags) {
    memset(info, 0, sizeof(*info));
    info->name = (flags & kRendererSoftware) ? kSoftwareDriverName : kRenderDriverName;
    info->flags = flags;
    uint32_t n = 0;
    for (const sdl2::FormatInfo& f : sdl2::kFormats) info->textureFormats[n++] = f.format;
    info->numTextureFormats = n;
    info->maxTextureWidth = kMaxTextureSize;
    info->maxTextureHeight = kMaxTextureSize;
}

int stub_SDL_GetNumRenderDrivers() {
    return kRenderDriverCount;
}

int stub_SDL_GetRenderDriverInfo(int index, void* info) {
    if (index < 0 || index >= kRenderDriverCount) return sdlFail(-1, "index must be in the range of 0 - 0");
    if (!info) return sdlFail(-1, "Parameter 'info' is invalid");
    fillRendererInfo(static_cast<SDLRendererInfoRaw*>(info), rendererDriverFlags(0));
    return 0;
}

int stub_SDL_GetRendererInfo(void* renderer, void* info) {
    SDLRendererState* rs = findRenderer(renderer);
    if (!rs) return sdlFail(-1, "Parameter 'renderer' is invalid");
    if (!info) return sdlFail(-1, "Parameter 'info' is invalid");
    fillRendererInfo(static_cast<SDLRendererInfoRaw*>(info), rs->flags);
    return 0;
}

void* stub_SDL_CreateRenderer(void* window, int index, uint32_t flags) {
    if (!window) return sdl2::sys::invalidParam("window"), nullptr;
    if (!sWindows.count(window)) return sdlFailNull("Invalid window");
    if (index < -1 || index >= kRenderDriverCount) return sdlFailNull("Couldn't find matching render driver");
    void* handle = reinterpret_cast<void*>(sNextHandle++);
    SDLRendererState state;
    state.window = window;
    state.currentTexture = nullptr;
    state.flags = rendererDriverFlags(flags) | (flags & kRendererPresentVSync);
    sRenderers[handle] = state;
    if (window == sMainWindow || !sMainRenderer) {
        sMainRenderer = handle;
    }
    return handle;
}

static void freeTexture(void* handle) {
    auto it = sTextures.find(handle);
    if (it == sTextures.end()) return;
    free(it->second.pixels);
    for (auto& kv : sRenderers) {
        if (kv.second.currentTexture == handle) kv.second.currentTexture = nullptr;
        if (kv.second.target == handle) kv.second.target = nullptr;
    }
    sTextures.erase(it);
}

void stub_SDL_DestroyRenderer(void* renderer) {
    if (!sRenderers.count(renderer)) { sdl2::sys::invalidParam("renderer"); return; }
    if (renderer == sMainRenderer) return;
    // SDL destroys the textures of a renderer with the renderer.
    std::vector<void*> owned;
    for (auto& kv : sTextures)
        if (kv.second.renderer == renderer) owned.push_back(kv.first);
    for (void* t : owned) freeTexture(t);
    sRenderers.erase(renderer);
}

int stub_SDL_RenderClear(void* renderer) {
    SDLRendererState* rs = findRenderer(renderer);
    if (!rs) return sdlFail(-1, "Parameter 'renderer' is invalid");
    if (isDisplayRenderer(renderer, *rs)) return 0;   // see the note above
    sdl2::Pixels dst;
    if (!renderTargetPixels(renderer, *rs, dst)) return sdlFail(-1, "Invalid render target");
    sdl2::clearAll(dst, { rs->drawR, rs->drawG, rs->drawB, rs->drawA });
    return 0;
}

void stub_SDL_RenderPresent(void* renderer) {
    if (!sRenderers.count(renderer)) { sdl2::sys::invalidParam("renderer"); return; }
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
    int rw = displayWidth(), rh = displayHeight();
    SDLRendererState* rs = findRenderer(renderer);
    if (!rs) return sdlFail(-1, "Parameter 'renderer' is invalid");
    if (rs && rs->target) {
        if (SDLTextureState* t = findTexture(rs->target)) { rw = t->width; rh = t->height; }
    } else if (rs && !isDisplayRenderer(renderer, *rs) && sWindows.count(rs->window)) {
        stub_SDL_GetWindowSize(rs->window, &rw, &rh);
    }
    if (w) *w = rw;
    if (h) *h = rh;
    return 0;
}

int stub_SDL_SetRenderDrawColor(void* renderer, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    SDLRendererState* rs = findRenderer(renderer);
    if (!rs) return sdlFail(-1, "Parameter 'renderer' is invalid");
    rs->drawR = r; rs->drawG = g; rs->drawB = b; rs->drawA = a;
    return 0;
}

int stub_SDL_SetRenderDrawBlendMode(void* renderer, uint32_t mode) {
    SDLRendererState* rs = findRenderer(renderer);
    if (!rs) return sdlFail(-1, "Parameter 'renderer' is invalid");
    if (!sdl2::isValidBlendMode(mode)) return sdlFail(-1, "Unsupported blend mode");
    rs->drawBlend = mode;
    return 0;
}

// SDL_ComposeCustomBlendMode: the six fields share one value.  The answer is
// SDL_BLENDMODE_INVALID when a factor or an operation is outside its range.
uint32_t stub_SDL_ComposeCustomBlendMode(uint32_t srcColor, uint32_t dstColor, uint32_t colorOp,
                                         uint32_t srcAlpha, uint32_t dstAlpha, uint32_t alphaOp) {
    auto fits = [](uint32_t v) { return v < 16; };   // each field is 4 bits wide
    if (!(fits(srcColor) && fits(dstColor) && fits(colorOp) && fits(srcAlpha) && fits(dstAlpha) && fits(alphaOp)))
        return sdl2::kBlendInvalid;
    const uint32_t mode = sdl2::composeBlendMode(srcColor, dstColor, colorOp, srcAlpha, dstAlpha, alphaOp);
    if (!sdl2::isValidBlendMode(mode)) return sdl2::kBlendInvalid;
    // A combination that equals a named mode answers that name, as SDL does.
    const sdl2::BlendSpec want = { srcColor, dstColor, colorOp, srcAlpha, dstAlpha, alphaOp };
    for (uint32_t named : { sdl2::kBlendNone, sdl2::kBlendBlend, sdl2::kBlendAdd, sdl2::kBlendMod, sdl2::kBlendMul }) {
        sdl2::BlendSpec have;
        sdl2::decodeBlendMode(named, have);
        if (have.srcColor == want.srcColor && have.dstColor == want.dstColor && have.colorOp == want.colorOp &&
            have.srcAlpha == want.srcAlpha && have.dstAlpha == want.dstAlpha && have.alphaOp == want.alphaOp)
            return named;
    }
    return mode;
}

int stub_SDL_RenderTargetSupported(void* renderer) {
    return findRenderer(renderer) ? 1 : 0;
}

// ----- Render state: viewport, scale, logical size and clip -----
//
// The viewport, the clip rectangle and the logical size live in target pixels
// (the viewport and the clip are stored multiplied by the scale that was
// current when they were set, as SDL does).  When the renderer switches to a
// texture target it keeps the state of the window and starts the texture
// with a full viewport, no clip, scale 1 and a logical size equal to the
// texture; it restores the window state when the target goes back to the
// window.

static bool outputSizeOf(void* handle, const SDLRendererState& rs, int& w, int& h) {
    w = displayWidth();
    h = displayHeight();
    if (rs.target) {
        SDLTextureState* t = findTexture(rs.target);
        if (!t) return false;
        w = t->width; h = t->height;
    } else if (!isDisplayRenderer(handle, rs) && sWindows.count(rs.window)) {
        stub_SDL_GetWindowSize(rs.window, &w, &h);
    }
    return true;
}

// The viewport in target pixels as the draw calls use it (integer truncation).
static sdl2::Rect viewportOf(void* handle, const SDLRendererState& rs) {
    if (!rs.view.viewportSet) {
        int w = 0, h = 0;
        outputSizeOf(handle, rs, w, h);
        return { 0, 0, w, h };
    }
    return { static_cast<int>(rs.view.vx), static_cast<int>(rs.view.vy),
             static_cast<int>(rs.view.vw), static_cast<int>(rs.view.vh) };
}

// The surface clip of the draw calls: the viewport, and inside it the clip
// rectangle (which is relative to the viewport).
static sdl2::Rect drawClipOf(void* handle, const SDLRendererState& rs, const sdl2::Pixels& dst) {
    const sdl2::Rect vp = viewportOf(handle, rs);
    sdl2::Rect clip = vp;
    if (rs.view.clipEnabled) {
        const sdl2::Rect c = { static_cast<int>(rs.view.cx) + vp.x, static_cast<int>(rs.view.cy) + vp.y,
                               static_cast<int>(rs.view.cw), static_cast<int>(rs.view.ch) };
        sdl2::rect::intersectRect<int>(reinterpret_cast<const sdl2::rect::RectT<int>*>(&vp),
                                       reinterpret_cast<const sdl2::rect::RectT<int>*>(&c),
                                       reinterpret_cast<sdl2::rect::RectT<int>*>(&clip));
    }
    return sdl2::effectiveClip(dst, &clip);
}

static void setViewportNull(void* handle, SDLRendererState& rs) {
    rs.view.viewportSet = false;
}

static void setViewportRect(SDLRendererState& rs, const sdl2::Rect& r) {
    rs.view.viewportSet = true;
    rs.view.vx = static_cast<double>(r.x) * rs.view.sx;
    rs.view.vy = static_cast<double>(r.y) * rs.view.sy;
    rs.view.vw = static_cast<double>(r.w) * rs.view.sx;
    rs.view.vh = static_cast<double>(r.h) * rs.view.sy;
}

// SDL_SetRenderTarget keeps the window state and starts a texture clean.
int stub_SDL_SetRenderTarget(void* renderer, void* texture) {
    SDLRendererState* rs = findRenderer(renderer);
    if (!rs) return sdlFail(-1, "That operation is not supported");   // SDL asks RenderTargetSupported first
    if (texture) {
        SDLTextureState* t = findTexture(texture);
        if (!t) return sdlFail(-1, "Parameter 'texture' is invalid");
        if (t->renderer != renderer) return sdlFail(-1, "Texture was not created with this renderer");
        if (t->access != kAccessTarget) return sdlFail(-1, "Texture not created with SDL_TEXTUREACCESS_TARGET");
    }
    if (texture == rs->target) return 0;
    if (texture && !rs->target) rs->viewBackup = rs->view;
    rs->target = texture;
    if (texture) {
        SDLTextureState* t = findTexture(texture);
        rs->view = RenderView();
        rs->view.viewportSet = true;
        rs->view.vw = t->width;
        rs->view.vh = t->height;
        rs->view.logicalW = t->width;
        rs->view.logicalH = t->height;
    } else {
        rs->view = rs->viewBackup;
    }
    return 0;
}

int stub_SDL_RenderSetClipRect(void* renderer, const void* rect) {
    SDLRendererState* rs = findRenderer(renderer);
    if (!rs) return sdlFail(-1, "Parameter 'renderer' is invalid");
    if (!rect) {
        rs->view.clipEnabled = false;
        rs->view.cx = rs->view.cy = rs->view.cw = rs->view.ch = 0;
        return 0;
    }
    const sdl2::Rect* r = static_cast<const sdl2::Rect*>(rect);
    rs->view.clipEnabled = true;
    rs->view.cx = static_cast<double>(r->x) * rs->view.sx;
    rs->view.cy = static_cast<double>(r->y) * rs->view.sy;
    rs->view.cw = static_cast<double>(r->w) * rs->view.sx;
    rs->view.ch = static_cast<double>(r->h) * rs->view.sy;
    return 0;
}

void stub_SDL_RenderGetClipRect(void* renderer, void* rect) {
    SDLRendererState* rs = findRenderer(renderer);
    if (!rs) { sdlFail(0, "Parameter 'renderer' is invalid"); return; }
    if (!rect) return;
    sdl2::Rect* r = static_cast<sdl2::Rect*>(rect);
    r->x = static_cast<int>(std::floor(rs->view.cx / rs->view.sx));
    r->y = static_cast<int>(std::floor(rs->view.cy / rs->view.sy));
    r->w = static_cast<int>(std::floor(rs->view.cw / rs->view.sx));
    r->h = static_cast<int>(std::floor(rs->view.ch / rs->view.sy));
}

int stub_SDL_RenderIsClipEnabled(void* renderer) {
    SDLRendererState* rs = findRenderer(renderer);
    if (!rs) return sdlFail(0, "Parameter 'renderer' is invalid");
    return rs->view.clipEnabled ? 1 : 0;
}

int stub_SDL_RenderSetViewport(void* renderer, const void* rect) {
    SDLRendererState* rs = findRenderer(renderer);
    if (!rs) return sdlFail(-1, "Parameter 'renderer' is invalid");
    if (rect) setViewportRect(*rs, *static_cast<const sdl2::Rect*>(rect));
    else setViewportNull(renderer, *rs);
    return 0;
}

void stub_SDL_RenderGetViewport(void* renderer, void* rect) {
    SDLRendererState* rs = findRenderer(renderer);
    if (!rs) { sdlFail(0, "Parameter 'renderer' is invalid"); return; }
    if (!rect) return;
    double x = 0, y = 0, w = 0, h = 0;
    if (rs->view.viewportSet) { x = rs->view.vx; y = rs->view.vy; w = rs->view.vw; h = rs->view.vh; }
    else { int iw = 0, ih = 0; outputSizeOf(renderer, *rs, iw, ih); w = iw; h = ih; }
    sdl2::Rect* r = static_cast<sdl2::Rect*>(rect);
    r->x = static_cast<int>(std::floor(x / rs->view.sx));
    r->y = static_cast<int>(std::floor(y / rs->view.sy));
    r->w = static_cast<int>(std::floor(w / rs->view.sx));
    r->h = static_cast<int>(std::floor(h / rs->view.sy));
}

int stub_SDL_RenderSetScale(void* renderer, float sx, float sy) {
    SDLRendererState* rs = findRenderer(renderer);
    if (!rs) return sdlFail(-1, "Parameter 'renderer' is invalid");
    rs->view.sx = sx;
    rs->view.sy = sy;
    return 0;
}

void stub_SDL_RenderGetScale(void* renderer, float* sx, float* sy) {
    SDLRendererState* rs = findRenderer(renderer);
    if (!rs) { sdlFail(0, "Parameter 'renderer' is invalid"); return; }
    if (sx) *sx = rs->view.sx;
    if (sy) *sy = rs->view.sy;
}

// The logical size letterboxes (or, with the overscan mode, crops) the logical
// picture into the target: it sets a viewport and a scale.
static int updateLogicalSize(void* renderer, SDLRendererState& rs) {
    if (!rs.view.logicalW || !rs.view.logicalH) return 0;
    int w = 1, h = 1;
    if (!outputSizeOf(renderer, rs, w, h)) return sdlFail(-1, "Parameter 'texture' is invalid");
    const char* mode = sdl2::base::getHint("SDL_RENDER_LOGICAL_SIZE_MODE");
    const bool overscan = mode && (*mode == '1' || strcasecmp(mode, "overscan") == 0);
    const float wantAspect = static_cast<float>(rs.view.logicalW) / rs.view.logicalH;
    const float realAspect = static_cast<float>(w) / h;
    rs.view.sx = rs.view.sy = 1.0f;   // the viewport below is in target pixels
    float scale;
    sdl2::Rect vp = { 0, 0, 0, 0 };
    if (rs.integerScale) {
        scale = wantAspect > realAspect ? static_cast<float>(w / rs.view.logicalW)
                                        : static_cast<float>(h / rs.view.logicalH);
        if (scale < 1.0f) scale = 1.0f;
        vp.w = static_cast<int>(std::floor(rs.view.logicalW * scale));
        vp.x = (w - vp.w) / 2;
        vp.h = static_cast<int>(std::floor(rs.view.logicalH * scale));
        vp.y = (h - vp.h) / 2;
    } else if (std::fabs(wantAspect - realAspect) < 0.0001) {
        scale = static_cast<float>(w) / rs.view.logicalW;
        vp = { 0, 0, w, h };
    } else if (wantAspect > realAspect) {
        if (overscan) {
            scale = static_cast<float>(h) / rs.view.logicalH;
            vp.y = 0; vp.h = h;
            vp.w = static_cast<int>(std::floor(rs.view.logicalW * scale));
            vp.x = (w - vp.w) / 2;
        } else {
            scale = static_cast<float>(w) / rs.view.logicalW;
            vp.x = 0; vp.w = w;
            vp.h = static_cast<int>(std::floor(rs.view.logicalH * scale));
            vp.y = (h - vp.h) / 2;
        }
    } else {
        if (overscan) {
            scale = static_cast<float>(w) / rs.view.logicalW;
            vp.x = 0; vp.w = w;
            vp.h = static_cast<int>(std::floor(rs.view.logicalH * scale));
            vp.y = (h - vp.h) / 2;
        } else {
            scale = static_cast<float>(h) / rs.view.logicalH;
            vp.y = 0; vp.h = h;
            vp.w = static_cast<int>(std::floor(rs.view.logicalW * scale));
            vp.x = (w - vp.w) / 2;
        }
    }
    setViewportRect(rs, vp);
    rs.view.sx = scale;
    rs.view.sy = scale;
    return 0;
}

int stub_SDL_RenderSetLogicalSize(void* renderer, int w, int h) {
    SDLRendererState* rs = findRenderer(renderer);
    if (!rs) return sdlFail(-1, "Parameter 'renderer' is invalid");
    if (!w || !h) {
        rs->view.logicalW = rs->view.logicalH = 0;
        setViewportNull(renderer, *rs);
        rs->view.sx = rs->view.sy = 1.0f;
        return 0;
    }
    rs->view.logicalW = w;
    rs->view.logicalH = h;
    return updateLogicalSize(renderer, *rs);
}

void stub_SDL_RenderGetLogicalSize(void* renderer, int* w, int* h) {
    SDLRendererState* rs = findRenderer(renderer);
    if (!rs) { sdlFail(0, "Parameter 'renderer' is invalid"); return; }
    if (w) *w = rs->view.logicalW;
    if (h) *h = rs->view.logicalH;
}

int stub_SDL_RenderSetIntegerScale(void* renderer, int enable) {
    SDLRendererState* rs = findRenderer(renderer);
    if (!rs) return sdlFail(-1, "Parameter 'renderer' is invalid");
    rs->integerScale = enable != 0;
    return updateLogicalSize(renderer, *rs);
}

int stub_SDL_RenderGetIntegerScale(void* renderer) {
    SDLRendererState* rs = findRenderer(renderer);
    if (!rs) return sdlFail(0, "Parameter 'renderer' is invalid");
    return rs->integerScale ? 1 : 0;
}

void stub_SDL_RenderWindowToLogical(void* renderer, int windowX, int windowY, float* logicalX, float* logicalY) {
    SDLRendererState* rs = findRenderer(renderer);
    if (!rs) { sdlFail(0, "Parameter 'renderer' is invalid"); return; }
    double vx = 0, vy = 0;
    if (rs->view.viewportSet) { vx = rs->view.vx; vy = rs->view.vy; }
    if (logicalX) *logicalX = static_cast<float>((static_cast<float>(windowX) - vx) / rs->view.sx);
    if (logicalY) *logicalY = static_cast<float>((static_cast<float>(windowY) - vy) / rs->view.sy);
}

void stub_SDL_RenderLogicalToWindow(void* renderer, float logicalX, float logicalY, int* windowX, int* windowY) {
    SDLRendererState* rs = findRenderer(renderer);
    if (!rs) { sdlFail(0, "Parameter 'renderer' is invalid"); return; }
    double vx = 0, vy = 0;
    if (rs->view.viewportSet) { vx = rs->view.vx; vy = rs->view.vy; }
    if (windowX) *windowX = static_cast<int>(static_cast<float>((logicalX * rs->view.sx) + vx));
    if (windowY) *windowY = static_cast<int>(static_cast<float>((logicalY * rs->view.sy) + vy));
}

int stub_SDL_RenderSetVSync(void* renderer, int vsync) {
    SDLRendererState* rs = findRenderer(renderer);
    if (!rs) return sdlFail(-1, "Parameter 'renderer' is invalid");
    if (vsync != 0 && vsync != 1) return sdlFail(-1, "Unsupported vsync interval");
    rs->flags = vsync ? (rs->flags | kRendererPresentVSync) : (rs->flags & ~kRendererPresentVSync);
    return 0;
}

// ----- Draw calls -----
//
// Every draw call resolves the target, the viewport and the clip once, then
// draws in target pixels.  A renderer scale other than 1 turns points, lines
// and rectangles into scaled rectangles, as the SDL software renderer does.

struct DrawTarget {
    SDLRendererState* rs = nullptr;
    sdl2::Pixels dst;
    bool isDisplay = false;
    sdl2::Rect vp = { 0, 0, 0, 0 };
    sdl2::Rect clip = { 0, 0, 0, 0 };
    sdl2::RGBA color = { 0, 0, 0, 255 };
    bool scaled = false;
};

static int beginDraw(void* renderer, DrawTarget& t) {
    t.rs = findRenderer(renderer);
    if (!t.rs) return sdlFail(-1, "Parameter 'renderer' is invalid");
    if (!renderTargetPixels(renderer, *t.rs, t.dst, &t.isDisplay)) return sdlFail(-1, "Invalid render target");
    t.vp = viewportOf(renderer, *t.rs);
    t.clip = drawClipOf(renderer, *t.rs, t.dst);
    t.color = { t.rs->drawR, t.rs->drawG, t.rs->drawB, t.rs->drawA };
    t.scaled = t.rs->view.sx != 1.0f || t.rs->view.sy != 1.0f;
    return 0;
}

static void endDraw(const DrawTarget& t) { finishDrawing(t.isDisplay); }

// A float rectangle in scaled target pixels becomes an integer rectangle at
// least one pixel wide and high.
static void fillScaledRect(const DrawTarget& t, float x, float y, float w, float h) {
    sdl2::Rect r = { static_cast<int>(x), static_cast<int>(y),
                     std::max(static_cast<int>(w), 1), std::max(static_cast<int>(h), 1) };
    r.x += t.vp.x;
    r.y += t.vp.y;
    sdl2::fillRectClipped(t.dst, t.clip, r, t.color, t.rs->drawBlend);
}

static void drawPointsOn(const DrawTarget& t, const sdl2::FRect* pts, int count) {
    const float sx = t.rs->view.sx, sy = t.rs->view.sy;
    if (t.scaled) {
        for (int i = 0; i < count; i++) fillScaledRect(t, pts[i].x * sx, pts[i].y * sy, sx, sy);
        return;
    }
    std::vector<sdl2::IPoint> ip(count);
    for (int i = 0; i < count; i++) ip[i] = { static_cast<int>(pts[i].x) + t.vp.x, static_cast<int>(pts[i].y) + t.vp.y };
    sdl2::drawPointsClipped(t.dst, t.clip, ip.data(), count, t.color, t.rs->drawBlend);
}

// One segment of the point method: all pixels of the Bresenham line, as points.
static void bresenhamPoints(const DrawTarget& t, int x1, int y1, int x2, int y2, bool drawLast) {
    const int deltax = std::abs(x2 - x1), deltay = std::abs(y2 - y1);
    int numpixels, d, dinc1, dinc2, xinc1, xinc2, yinc1, yinc2;
    if (deltax >= deltay) {
        numpixels = deltax + 1; d = 2 * deltay - deltax; dinc1 = deltay * 2; dinc2 = (deltay - deltax) * 2;
        xinc1 = 1; xinc2 = 1; yinc1 = 0; yinc2 = 1;
    } else {
        numpixels = deltay + 1; d = 2 * deltax - deltay; dinc1 = deltax * 2; dinc2 = (deltax - deltay) * 2;
        xinc1 = 0; xinc2 = 1; yinc1 = 1; yinc2 = 1;
    }
    if (x1 > x2) { xinc1 = -xinc1; xinc2 = -xinc2; }
    if (y1 > y2) { yinc1 = -yinc1; yinc2 = -yinc2; }
    if (!drawLast) --numpixels;
    std::vector<sdl2::FRect> pts;
    pts.reserve(std::max(numpixels, 0));
    int x = x1, y = y1;
    for (int i = 0; i < numpixels; i++) {
        pts.push_back({ static_cast<float>(x), static_cast<float>(y), 0.f, 0.f });
        if (d < 0) { d += dinc1; x += xinc1; y += yinc1; }
        else { d += dinc2; x += xinc2; y += yinc2; }
    }
    drawPointsOn(t, pts.data(), static_cast<int>(pts.size()));
}

static int renderLineMethod() {
    const char* hint = sdl2::base::getHint("SDL_RENDER_LINE_METHOD");
    return hint ? atoi(hint) : 0;
}

static void drawLinesOn(const DrawTarget& t, const sdl2::FRect* pts, int count) {
    if (renderLineMethod() == 2) {   // SDL_RENDERLINEMETHOD_LINES: poly-line of the surface
        std::vector<sdl2::IPoint> ip(count);
        for (int i = 0; i < count; i++) ip[i] = { static_cast<int>(pts[i].x) + t.vp.x, static_cast<int>(pts[i].y) + t.vp.y };
        if (!t.scaled) {
            sdl2::drawPolyline(t.dst, t.clip, ip.data(), count, t.color, t.rs->drawBlend);
            return;
        }
    }
    const float sx = t.rs->view.sx, sy = t.rs->view.sy;
    const sdl2::Rect logicalView = { 0, 0, static_cast<int>(t.vp.w / sx), static_cast<int>(t.vp.h / sy) };
    std::vector<sdl2::FRect> rects;
    bool drewLine = false, drawLast = false;
    for (int i = 0; i < count - 1; i++) {
        const bool sameX = pts[i].x == pts[i + 1].x, sameY = pts[i].y == pts[i + 1].y;
        if (i == count - 2) {
            if (!drewLine || pts[i + 1].x != pts[0].x || pts[i + 1].y != pts[0].y) drawLast = true;
        } else if (sameX && sameY) {
            continue;
        }
        const float last = drawLast ? 1.0f : 0.0f;
        if (sameX) {
            const float minY = std::min(pts[i].y, pts[i + 1].y), maxY = std::max(pts[i].y, pts[i + 1].y);
            sdl2::FRect r = { pts[i].x * sx, minY * sy, sx, (maxY - minY + last) * sy };
            if (!drawLast && pts[i + 1].y < pts[i].y) r.y += sy;
            rects.push_back(r);
        } else if (sameY) {
            const float minX = std::min(pts[i].x, pts[i + 1].x), maxX = std::max(pts[i].x, pts[i + 1].x);
            sdl2::FRect r = { minX * sx, pts[i].y * sy, (maxX - minX + last) * sx, sy };
            if (!drawLast && pts[i + 1].x < pts[i].x) r.x += sx;
            rects.push_back(r);
        } else {
            // A segment is cut to the viewport (in unscaled coordinates) with the
            // integer Cohen-Sutherland clip before it is drawn, so a long line
            // that leaves the viewport keeps the pixels of the clipped line.
            int x1 = static_cast<int>(std::roundf(pts[i].x)), y1 = static_cast<int>(std::roundf(pts[i].y));
            int x2 = static_cast<int>(std::roundf(pts[i + 1].x)), y2 = static_cast<int>(std::roundf(pts[i + 1].y));
            const int ox2 = x2, oy2 = y2;
            if (sdl2::rect::intersectRectAndLine<int>(reinterpret_cast<const sdl2::rect::RectT<int>*>(&logicalView), &x1, &y1, &x2, &y2)) {
                const bool clippedEnd = x2 != ox2 || y2 != oy2;
                bresenhamPoints(t, x1, y1, x2, y2, drawLast || clippedEnd);
            }
        }
        drewLine = true;
    }
    for (const sdl2::FRect& r : rects) fillScaledRect(t, r.x, r.y, r.w, r.h);
}

static sdl2::FRect viewportSizeF(void* renderer, const SDLRendererState& rs) {
    const sdl2::Rect vp = viewportOf(renderer, rs);
    double w = vp.w, h = vp.h;
    if (rs.view.viewportSet) { w = rs.view.vw; h = rs.view.vh; }
    return { 0.f, 0.f, static_cast<float>(w / rs.view.sx), static_cast<float>(h / rs.view.sy) };
}

static void rectOutline(const DrawTarget& t, const sdl2::FRect& r) {
    const sdl2::FRect p[5] = { { r.x, r.y, 0, 0 }, { r.x + r.w - 1, r.y, 0, 0 }, { r.x + r.w - 1, r.y + r.h - 1, 0, 0 },
                               { r.x, r.y + r.h - 1, 0, 0 }, { r.x, r.y, 0, 0 } };
    drawLinesOn(t, p, 5);
}

static void fillRectsOn(const DrawTarget& t, const sdl2::FRect* rects, int count) {
    const float sx = t.rs->view.sx, sy = t.rs->view.sy;
    for (int i = 0; i < count; i++) fillScaledRect(t, rects[i].x * sx, rects[i].y * sy, rects[i].w * sx, rects[i].h * sy);
}

static std::vector<sdl2::FRect> pointsToF(const sdl2::Rect* ip, int count) {   // SDL_Point has the first two fields of SDL_Rect: use a stride-2 read
    std::vector<sdl2::FRect> out(count);
    const int* raw = reinterpret_cast<const int*>(ip);
    for (int i = 0; i < count; i++) out[i] = { static_cast<float>(raw[2 * i]), static_cast<float>(raw[2 * i + 1]), 0.f, 0.f };
    return out;
}

static std::vector<sdl2::FRect> fpointsToF(const float* fp, int count) {
    std::vector<sdl2::FRect> out(count);
    for (int i = 0; i < count; i++) out[i] = { fp[2 * i], fp[2 * i + 1], 0.f, 0.f };
    return out;
}

static std::vector<sdl2::FRect> rectsToF(const sdl2::Rect* r, int count) {
    std::vector<sdl2::FRect> out(count);
    for (int i = 0; i < count; i++) out[i] = { static_cast<float>(r[i].x), static_cast<float>(r[i].y), static_cast<float>(r[i].w), static_cast<float>(r[i].h) };
    return out;
}

#define SDL_RENDER_NEED_ARRAY(arr, fname) \
    if (!(arr)) { return sdlFail(-1, "Parameter '" fname "' is invalid"); }

int stub_SDL_RenderDrawPoints(void* renderer, const void* points, int count) {
    DrawTarget t;
    if (beginDraw(renderer, t) < 0) return -1;
    SDL_RENDER_NEED_ARRAY(points, "SDL_RenderDrawPoints(): points")
    if (count < 1) return 0;
    auto f = pointsToF(static_cast<const sdl2::Rect*>(points), count);
    drawPointsOn(t, f.data(), count);
    endDraw(t);
    return 0;
}

int stub_SDL_RenderDrawPointsF(void* renderer, const void* points, int count) {
    DrawTarget t;
    if (beginDraw(renderer, t) < 0) return -1;
    SDL_RENDER_NEED_ARRAY(points, "SDL_RenderDrawPointsF(): points")
    if (count < 1) return 0;
    auto f = fpointsToF(static_cast<const float*>(points), count);
    drawPointsOn(t, f.data(), count);
    endDraw(t);
    return 0;
}

int stub_SDL_RenderDrawPoint(void* renderer, int x, int y) {
    const int p[2] = { x, y };
    return stub_SDL_RenderDrawPoints(renderer, p, 1);
}

int stub_SDL_RenderDrawPointF(void* renderer, float x, float y) {
    const float p[2] = { x, y };
    return stub_SDL_RenderDrawPointsF(renderer, p, 1);
}

int stub_SDL_RenderDrawLines(void* renderer, const void* points, int count) {
    DrawTarget t;
    if (beginDraw(renderer, t) < 0) return -1;
    SDL_RENDER_NEED_ARRAY(points, "SDL_RenderDrawLines(): points")
    if (count < 2) return 0;
    auto f = pointsToF(static_cast<const sdl2::Rect*>(points), count);
    drawLinesOn(t, f.data(), count);
    endDraw(t);
    return 0;
}

int stub_SDL_RenderDrawLinesF(void* renderer, const void* points, int count) {
    DrawTarget t;
    if (beginDraw(renderer, t) < 0) return -1;
    SDL_RENDER_NEED_ARRAY(points, "SDL_RenderDrawLinesF(): points")
    if (count < 2) return 0;
    auto f = fpointsToF(static_cast<const float*>(points), count);
    drawLinesOn(t, f.data(), count);
    endDraw(t);
    return 0;
}

int stub_SDL_RenderDrawLine(void* renderer, int x1, int y1, int x2, int y2) {
    const int p[4] = { x1, y1, x2, y2 };
    return stub_SDL_RenderDrawLines(renderer, p, 2);
}

int stub_SDL_RenderDrawLineF(void* renderer, float x1, float y1, float x2, float y2) {
    const float p[4] = { x1, y1, x2, y2 };
    return stub_SDL_RenderDrawLinesF(renderer, p, 2);
}

int stub_SDL_RenderDrawRectF(void* renderer, const void* rect) {
    DrawTarget t;
    if (beginDraw(renderer, t) < 0) return -1;
    const sdl2::FRect r = rect ? *static_cast<const sdl2::FRect*>(rect) : viewportSizeF(renderer, *t.rs);
    rectOutline(t, r);
    endDraw(t);
    return 0;
}

int stub_SDL_RenderDrawRect(void* renderer, const void* rect) {
    if (!rect) return stub_SDL_RenderDrawRectF(renderer, nullptr);
    const sdl2::FRect r = rectsToF(static_cast<const sdl2::Rect*>(rect), 1)[0];
    return stub_SDL_RenderDrawRectF(renderer, &r);
}

int stub_SDL_RenderDrawRectsF(void* renderer, const void* rects, int count) {
    if (!findRenderer(renderer)) return sdlFail(-1, "Parameter 'renderer' is invalid");
    SDL_RENDER_NEED_ARRAY(rects, "SDL_RenderDrawRectsF(): rects")
    for (int i = 0; i < count; i++)
        if (stub_SDL_RenderDrawRectF(renderer, static_cast<const sdl2::FRect*>(rects) + i) < 0) return -1;
    return 0;
}

int stub_SDL_RenderDrawRects(void* renderer, const void* rects, int count) {
    if (!findRenderer(renderer)) return sdlFail(-1, "Parameter 'renderer' is invalid");
    SDL_RENDER_NEED_ARRAY(rects, "SDL_RenderDrawRects(): rects")
    for (int i = 0; i < count; i++)
        if (stub_SDL_RenderDrawRect(renderer, static_cast<const sdl2::Rect*>(rects) + i) < 0) return -1;
    return 0;
}

int stub_SDL_RenderFillRectsF(void* renderer, const void* rects, int count) {
    DrawTarget t;
    if (beginDraw(renderer, t) < 0) return -1;
    SDL_RENDER_NEED_ARRAY(rects, "SDL_RenderFillRectsF(): rects")
    if (count < 1) return 0;
    fillRectsOn(t, static_cast<const sdl2::FRect*>(rects), count);
    endDraw(t);
    return 0;
}

int stub_SDL_RenderFillRects(void* renderer, const void* rects, int count) {
    DrawTarget t;
    if (beginDraw(renderer, t) < 0) return -1;
    SDL_RENDER_NEED_ARRAY(rects, "SDL_RenderFillRects(): rects")
    if (count < 1) return 0;
    auto f = rectsToF(static_cast<const sdl2::Rect*>(rects), count);
    fillRectsOn(t, f.data(), count);
    endDraw(t);
    return 0;
}

int stub_SDL_RenderFillRectF(void* renderer, const void* rect) {
    DrawTarget t;
    if (beginDraw(renderer, t) < 0) return -1;
    const sdl2::FRect r = rect ? *static_cast<const sdl2::FRect*>(rect) : viewportSizeF(renderer, *t.rs);
    fillRectsOn(t, &r, 1);
    endDraw(t);
    return 0;
}

int stub_SDL_RenderFillRect(void* renderer, const void* rect) {
    if (!rect) return stub_SDL_RenderFillRectF(renderer, nullptr);
    const sdl2::FRect r = rectsToF(static_cast<const sdl2::Rect*>(rect), 1)[0];
    return stub_SDL_RenderFillRectF(renderer, &r);
}

void* stub_SDL_CreateTexture(void* renderer, uint32_t format, int access, int w, int h) {
    // DO NOT override dimensions -- Pharo's Form uses the same extent for BitBlt stride.
    // Changing the texture size without Pharo knowing causes stride mismatch (diagonal garbage).
    if (!findRenderer(renderer)) return sdlFailNull("Parameter 'renderer' is invalid");
    if (!format) format = sdl2::kFormats[0].format;   // SDL takes the first format the renderer lists
    // SDL_BYTESPERPIXEL is never 0 for a FOURCC (YUV) format.
    if (!sdl2::pix::isFourCCValue(format) && sdl2::pix::bytesPerPixel(format) == 0) return sdlFailNull("Invalid texture format");
    if (sdl2::isIndexedFormat(format)) return sdlFailNull("Palettized textures are not supported");
    if (w <= 0 || h <= 0) return sdlFailNull("Texture dimensions can't be 0");
    if (sdl2::pix::isFourCCValue(format) && !strcmp(sdl2::pix::formatName(format), "SDL_PIXELFORMAT_UNKNOWN"))
        return sdlFailNull("Unsupported YUV format");
    if (!sdl2::formatInfo(format)) return sdlFailNull("Unsupported pixel format");
    if (w > kMaxTextureSize || h > kMaxTextureSize) return sdlFailNull("Texture dimensions are limited to 16384");
    void* handle = reinterpret_cast<void*>(sNextHandle++);
    SDLTextureState state;
    state.pixels = static_cast<uint32_t*>(calloc(static_cast<size_t>(w) * h, 4));
    if (!state.pixels) return sdlFailNull("Out of memory");
    state.width = w;
    state.height = h;
    state.pitch = w * 4;  // 32bpp
    state.renderer = renderer;
    state.usesDisplaySurface = false;
    state.format = format;
    state.access = access;
    // SDL_HINT_RENDER_SCALE_QUALITY: "0"/"nearest" or "1"/"linear"/"2"/"best".
    // The hint is read through SDL_GetHint, so the environment variable of the
    // same name counts as in SDL.
    if (const char* quality = sdl2::base::getHint("SDL_RENDER_SCALE_QUALITY")) {
        const std::string q = quality;
        state.linear = q == "1" || q == "2" || q == "linear" || q == "best";
    }
    sTextures[handle] = state;
    auto rit = sRenderers.find(renderer);
    if (rit != sRenderers.end()) {
        rit->second.currentTexture = handle;
    }
    return handle;
}

void stub_SDL_DestroyTexture(void* texture) {
    if (!sTextures.count(texture)) { sdl2::sys::invalidParam("texture"); return; }
    freeTexture(texture);
}

int stub_SDL_QueryTexture(void* texture, uint32_t* format, int* access, int* w, int* h) {
    SDLTextureState* t = findTexture(texture);
    if (!t) return sdlFail(-1, "Parameter 'texture' is invalid");
    if (format) *format = t->format;
    if (access) *access = t->access;
    if (w) *w = t->width;
    if (h) *h = t->height;
    return 0;
}

int stub_SDL_SetTextureBlendMode(void* texture, uint32_t mode) {
    SDLTextureState* t = findTexture(texture);
    if (!t) return sdlFail(-1, "Parameter 'texture' is invalid");
    if (!sdl2::isValidBlendMode(mode)) return sdlFail(-1, "Unsupported blend mode");
    t->blendMode = mode;
    return 0;
}

int stub_SDL_SetTextureColorMod(void* texture, uint8_t r, uint8_t g, uint8_t b) {
    SDLTextureState* t = findTexture(texture);
    if (!t) return sdlFail(-1, "Parameter 'texture' is invalid");
    t->rMod = r; t->gMod = g; t->bMod = b;
    return 0;
}

int stub_SDL_SetTextureAlphaMod(void* texture, uint8_t a) {
    SDLTextureState* t = findTexture(texture);
    if (!t) return sdlFail(-1, "Parameter 'texture' is invalid");
    t->aMod = a;
    return 0;
}

int stub_SDL_SetTextureScaleMode(void* texture, int mode) {
    SDLTextureState* t = findTexture(texture);
    if (!t) return sdlFail(-1, "Parameter 'texture' is invalid");
    t->linear = mode != 0;   // SDL_ScaleModeNearest is 0; Linear and Best both filter
    return 0;
}

int stub_SDL_LockTexture(void* texture, void* rect, void** pixels, int* pitch) {
    auto it = sTextures.find(texture);
    if (it == sTextures.end() || !it->second.pixels) {
        return sdlFail(-1, "Parameter 'texture' is invalid");
    }
    if (it->second.access != kAccessStreaming) {
        return sdlFail(-1, "SDL_LockTexture(): texture must be created with SDL_TEXTUREACCESS_STREAMING");
    }
    const sdl2::Rect* r = static_cast<const sdl2::Rect*>(rect);
    if (r && (r->x < 0 || r->y < 0 || r->w < 0 || r->h < 0 ||
              r->x + r->w > it->second.width || r->y + r->h > it->second.height)) {
        return sdlFail(-1, "SDL_LockTexture(): Invalid rect");
    }

    // Return the display surface buffer directly instead of the texture's private buffer.
    // Pharo stores this pointer via setPointer: and writes to it continuously via BitBlt,
    // bypassing LockTexture/UnlockTexture after the first call. By returning the display
    // surface buffer, Pharo writes directly where the Metal renderer reads.
    //
    // Always re-check dimensions on every call — the display surface may have been
    // resized (e.g. iPad rotation or initial size mismatch). If dimensions don't match,
    // fall back to the texture's private buffer so Pharo doesn't write out-of-bounds.
    // Only a whole-texture lock of a display-renderer texture takes this path; a
    // lock of one rectangle (the sdl-experiments atlas) works on the texture.
    uint8_t* base = reinterpret_cast<uint8_t*>(it->second.pixels);
    int returnPitch = it->second.pitch;
    bool useDS = false;

    SDLRendererState* rs = findRenderer(it->second.renderer);
    if (!r && rs && it->second.renderer == sMainRenderer && pharo::gDisplaySurface) {
        int dsW = pharo::gDisplaySurface->width();
        int dsH = pharo::gDisplaySurface->height();
        if (dsW == it->second.width && dsH == it->second.height) {
            base = reinterpret_cast<uint8_t*>(pharo::gDisplaySurface->pixels());
            returnPitch = dsW * 4;
            useDS = true;
        }
    }

    it->second.usesDisplaySurface = useDS;
    it->second.lastLockWasFull = r == nullptr;
    // SDL answers a pointer to the first pixel of the locked rectangle.
    if (r) base += static_cast<size_t>(r->y) * returnPitch + static_cast<size_t>(r->x) * 4;
    if (pixels) *pixels = base;
    if (pitch) *pitch = returnPitch;
    return 0;
}

void stub_SDL_UnlockTexture(void* texture) {
    if (!sTextures.count(texture)) { sdl2::sys::invalidParam("texture"); return; }
    if (SDLTextureState* locked = findTexture(texture)) {
        if (locked->lockedSurface) { sdl2::freeSurface(locked->lockedSurface); locked->lockedSurface = nullptr; }
    }
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

    // Only a whole-texture lock of a display-renderer texture feeds the display.
    if (!it->second.lastLockWasFull || it->second.renderer != sMainRenderer) return;

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

static sdl2::TextureSampling samplingOf(const SDLTextureState& t) {
    sdl2::TextureSampling s;
    s.rMod = t.rMod; s.gMod = t.gMod; s.bMod = t.bMod; s.aMod = t.aMod;
    s.blend = t.blendMode;
    s.linear = t.linear;
    return s;
}

// Shared body of SDL_RenderCopy, CopyF, CopyEx and CopyExF.  `dst` is null for
// the whole viewport.  `rotated` is false for the two plain copies.
static int renderCopyAny(void* renderer, void* texture, const sdl2::Rect* srcRect, const sdl2::FRect* dst,
                         bool rotated, double angle, const sdl2::FPoint* center, int flip) {
    SDLRendererState* rs = findRenderer(renderer);
    if (!rs) return sdlFail(-1, "Parameter 'renderer' is invalid");
    SDLTextureState* t = findTexture(texture);
    if (!t) return sdlFail(-1, "Parameter 'texture' is invalid");
    if (t->renderer != renderer) return sdlFail(-1, "Texture was not created with this renderer");

    if (isDisplayRenderer(renderer, *rs)) {
        // The display renderer only records the texture; RenderPresent shows it.
        rs->currentTexture = texture;
        return 0;
    }
    rs->currentTexture = texture;
    if (texture == rs->target) return sdlFail(-1, "Can't render a target texture onto itself");

    // A source rectangle is cut to the texture; nothing remains when the two do not meet.
    sdl2::Rect src = { 0, 0, t->width, t->height };
    if (srcRect) {
        const int x1 = std::max(srcRect->x, 0), y1 = std::max(srcRect->y, 0);
        const int x2 = std::min(srcRect->x + srcRect->w, t->width), y2 = std::min(srcRect->y + srcRect->h, t->height);
        if (x2 <= x1 || y2 <= y1 || srcRect->w <= 0 || srcRect->h <= 0) return 0;
        src = { x1, y1, x2 - x1, y2 - y1 };
    }
    const sdl2::FRect view = viewportSizeF(renderer, *rs);
    sdl2::FRect d = view;
    if (dst) {
        if (!rotated) {
            // A plain copy of a rectangle outside the viewport draws nothing.
            const sdl2::rect::RectT<float> a = { dst->x, dst->y, dst->w, dst->h };
            const sdl2::rect::RectT<float> b = { view.x, view.y, view.w, view.h };
            if (!sdl2::rect::hasIntersection<float>(&a, &b)) return 0;
        }
        d = *dst;
    }

    DrawTarget dt;
    if (beginDraw(renderer, dt) < 0) return -1;
    const float sx = rs->view.sx, sy = rs->view.sy;
    const sdl2::TextureSampling sampling = samplingOf(*t);
    const sdl2::Pixels tex = texturePixels(*t);
    if (!rotated) {
        // The destination in target pixels: scaled, then cut to whole pixels.
        const float fx = d.x * sx, fy = d.y * sy, fw = d.w * sx, fh = d.h * sy;
        const float ix = static_cast<float>(static_cast<int>(fx)) + dt.vp.x, iy = static_cast<float>(static_cast<int>(fy)) + dt.vp.y;
        if (!sdl2::copyTexture(dt.dst, sdl2::Clip{ true, dt.clip }, tex, src, ix, iy,
                               static_cast<float>(static_cast<int>(fw)), static_cast<float>(static_cast<int>(fh)), sampling))
            return sdlFail(-1, "Unsupported blend mode");
    } else {
        sdl2::FPoint c = center ? *center : sdl2::FPoint{ d.w / 2.0f, d.h / 2.0f };
        sdl2::Rect finalRect = { static_cast<int>(d.x), static_cast<int>(d.y), static_cast<int>(d.w), static_cast<int>(d.h) };
        finalRect.x += dt.vp.x;
        finalRect.y += dt.vp.y;
        const bool hasAlpha = tex.fmt && tex.fmt->aMask != 0;
        if (!sdl2::copyRotated(dt.dst, dt.clip, tex, src, finalRect, angle, c, flip, sampling, hasAlpha, sx, sy))
            return sdlFail(-1, "Unsupported blend mode");
    }
    endDraw(dt);
    return 0;
}

int stub_SDL_RenderCopy(void* renderer, void* texture, void* srcrect, void* dstrect) {
    sdl2::FRect f;
    const sdl2::Rect* d = static_cast<const sdl2::Rect*>(dstrect);
    if (d) f = { static_cast<float>(d->x), static_cast<float>(d->y), static_cast<float>(d->w), static_cast<float>(d->h) };
    return renderCopyAny(renderer, texture, static_cast<const sdl2::Rect*>(srcrect), d ? &f : nullptr, false, 0.0, nullptr, 0);
}

int stub_SDL_RenderCopyF(void* renderer, void* texture, void* srcrect, void* dstrect) {
    return renderCopyAny(renderer, texture, static_cast<const sdl2::Rect*>(srcrect),
                         static_cast<const sdl2::FRect*>(dstrect), false, 0.0, nullptr, 0);
}

// With no flip and an angle that is a whole turn, SDL answers with a plain copy.
int stub_SDL_RenderCopyExF(void* renderer, void* texture, void* srcrect, void* dstrect, double angle,
                           const void* center, int flip) {
    if (flip == 0 && static_cast<int>(angle / 360) == angle / 360)
        return stub_SDL_RenderCopyF(renderer, texture, srcrect, dstrect);
    return renderCopyAny(renderer, texture, static_cast<const sdl2::Rect*>(srcrect),
                         static_cast<const sdl2::FRect*>(dstrect), true, angle,
                         static_cast<const sdl2::FPoint*>(center), flip);
}

int stub_SDL_RenderCopyEx(void* renderer, void* texture, void* srcrect, void* dstrect, double angle,
                          const void* center, int flip) {
    sdl2::FRect f;
    const sdl2::Rect* d = static_cast<const sdl2::Rect*>(dstrect);
    if (d) f = { static_cast<float>(d->x), static_cast<float>(d->y), static_cast<float>(d->w), static_cast<float>(d->h) };
    sdl2::FPoint fc;
    const int* c = static_cast<const int*>(center);
    if (c) fc = { static_cast<float>(c[0]), static_cast<float>(c[1]) };
    return stub_SDL_RenderCopyExF(renderer, texture, srcrect, d ? &f : nullptr, angle, c ? &fc : nullptr, flip);
}

// Triangles from vertex arrays with a stride, shared by the two geometry calls.
static int renderTriangles(void* renderer, void* texture, const uint8_t* xy, int xyStride,
                           const uint8_t* color, int colorStride, const uint8_t* uv, int uvStride,
                           int numVertices, const void* indices, int numIndices, int sizeIndices) {
    SDLRendererState* rs = findRenderer(renderer);
    if (!rs) return sdlFail(-1, "Parameter 'renderer' is invalid");
    SDLTextureState* t = nullptr;
    if (texture) {
        t = findTexture(texture);
        if (!t) return sdlFail(-1, "Parameter 'texture' is invalid");
        if (t->renderer != renderer) return sdlFail(-1, "Texture was not created with this renderer");
    }
    if (!xy) return sdlFail(-1, "Parameter 'xy' is invalid");
    if (!color) return sdlFail(-1, "Parameter 'color' is invalid");
    if (t && !uv) return sdlFail(-1, "Parameter 'uv' is invalid");
    if (numVertices < 3) return 0;
    if (indices && sizeIndices != 1 && sizeIndices != 2 && sizeIndices != 4)
        return sdlFail(-1, "Parameter 'size_indices' is invalid");
    const int count = indices ? numIndices : numVertices;
    if (count % 3 != 0) return sdlFail(-1, "The number of indices must be a multiple of 3");

    sdl2::Pixels target;
    bool isDisplay = false;
    if (!renderTargetPixels(renderer, *rs, target, &isDisplay)) return sdlFail(-1, "Invalid render target");
    sdl2::Pixels tex;
    if (t) tex = texturePixels(*t);
    const sdl2::TextureSampling sampling = t ? samplingOf(*t) : sdl2::TextureSampling{};
    const uint32_t blend = t ? t->blendMode : rs->drawBlend;
    // Vertex positions are scaled by the renderer scale and moved by the viewport.
    const sdl2::Rect clipRect = drawClipOf(renderer, *rs, target);
    const sdl2::Rect vp = viewportOf(renderer, *rs);
    const float scaleX = rs->view.sx, scaleY = rs->view.sy;

    auto vertexAt = [&](int i) {
        sdl2::Vertex2D v = {};
        const float* p = reinterpret_cast<const float*>(xy + static_cast<size_t>(i) * xyStride);
        v.x = p[0] * scaleX + vp.x; v.y = p[1] * scaleY + vp.y;
        const uint8_t* c = color + static_cast<size_t>(i) * colorStride;
        v.color = { c[0], c[1], c[2], c[3] };
        if (uv) {
            const float* q = reinterpret_cast<const float*>(uv + static_cast<size_t>(i) * uvStride);
            v.u = q[0]; v.v = q[1];
        }
        return v;
    };
    auto indexAt = [&](int i) -> int {
        if (!indices) return i;
        switch (sizeIndices) {
        case 1: return static_cast<const uint8_t*>(indices)[i];
        case 2: return static_cast<const uint16_t*>(indices)[i];
        default: return static_cast<const int32_t*>(indices)[i];
        }
    };
    for (int i = 0; i < count; i++) {
        const int ix = indexAt(i);
        if (ix < 0 || ix >= numVertices) return sdlFail(-1, "Values of 'indices' out of range");
    }
    for (int i = 0; i < count; i += 3) {
        sdl2::drawTriangle(target, sdl2::Clip{ true, clipRect }, vertexAt(indexAt(i)), vertexAt(indexAt(i + 1)),
                           vertexAt(indexAt(i + 2)), t ? &tex : nullptr, sampling, blend);
    }
    finishDrawing(isDisplay);
    return 0;
}

// SDL_Vertex: position (2 floats), colour (4 bytes), texture coordinate (2 floats).
struct SDLVertexRaw { float x, y; uint8_t r, g, b, a; float u, v; };
static_assert(sizeof(SDLVertexRaw) == 20, "SDL_Vertex");

int stub_SDL_RenderGeometry(void* renderer, void* texture, const void* vertices, int numVertices,
                            const void* indices, int numIndices) {
    if (!vertices) return sdlFail(-1, "Parameter 'vertices' is invalid");
    const uint8_t* base = static_cast<const uint8_t*>(vertices);
    return renderTriangles(renderer, texture,
                           base + offsetof(SDLVertexRaw, x), sizeof(SDLVertexRaw),
                           base + offsetof(SDLVertexRaw, r), sizeof(SDLVertexRaw),
                           base + offsetof(SDLVertexRaw, u), sizeof(SDLVertexRaw),
                           numVertices, indices, numIndices, sizeof(int32_t));
}

int stub_SDL_RenderGeometryRaw(void* renderer, void* texture, const float* xy, int xyStride,
                               const void* color, int colorStride, const float* uv, int uvStride,
                               int numVertices, const void* indices, int numIndices, int sizeIndices) {
    return renderTriangles(renderer, texture, reinterpret_cast<const uint8_t*>(xy), xyStride,
                           static_cast<const uint8_t*>(color), colorStride,
                           reinterpret_cast<const uint8_t*>(uv), uvStride,
                           numVertices, indices, numIndices, sizeIndices);
}

// SDL_RenderReadPixels reads the viewport: the read rectangle is cut to the
// viewport, and the first pixel of the answer is the first pixel of the cut.
int stub_SDL_RenderReadPixels(void* renderer, const void* rect, uint32_t format, void* pixels, int pitch) {
    SDLRendererState* rs = findRenderer(renderer);
    if (!rs) return sdlFail(-1, "Parameter 'renderer' is invalid");
    if (!pixels) return sdlFail(-1, "Parameter 'pixels' is invalid");
    sdl2::Pixels src;
    if (!renderTargetPixels(renderer, *rs, src)) return sdlFail(-1, "Invalid render target");
    // Format 0 asks for the native format of the target.
    const uint32_t outFormat = format ? format : src.fmt->format;
    sdl2::PixelFormatRaw outProto;
    if (sdl2::isIndexedFormat(outFormat) || !sdl2::surf::formatProto(outFormat, outProto))
        return sdlFail(-1, "Unsupported pixel format");
    sdl2::Rect r = viewportOf(renderer, *rs);
    uint8_t* dest = static_cast<uint8_t*>(pixels);
    if (rect) {
        const sdl2::Rect* want = static_cast<const sdl2::Rect*>(rect);
        sdl2::Rect cut;
        if (!sdl2::rect::intersectRect<int>(reinterpret_cast<const sdl2::rect::RectT<int>*>(want),
                                            reinterpret_cast<const sdl2::rect::RectT<int>*>(&r),
                                            reinterpret_cast<sdl2::rect::RectT<int>*>(&cut)))
            return 0;
        if (cut.y > want->y) dest += static_cast<size_t>(pitch) * (cut.y - want->y);
        if (cut.x > want->x) dest += static_cast<size_t>(outProto.bytesPerPixel) * static_cast<size_t>(cut.x - want->x);
        r = cut;
    }
    if (r.x < 0 || r.x + r.w > src.w || r.y < 0 || r.y + r.h > src.h)
        return sdlFail(-1, "Tried to read outside of surface bounds");
    sdl2::Pixels dp;
    dp.data = dest; dp.w = r.w; dp.h = r.h; dp.pitch = pitch; dp.pf = &outProto;
    if (outProto.bytesPerPixel == 4) dp.fmt = sdl2::formatInfo(outFormat);
    sdl2::convertRect(src, r, dp);
    return 0;
}

int stub_SDL_UpdateTexture(void* texture, void* rect, void* pixels, int pitch) {
    auto it = sTextures.find(texture);
    if (it == sTextures.end() || !it->second.pixels || !pixels) {
        return sdlFail(-1, "Parameter 'texture' is invalid");
    }

    int texW = it->second.width;
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
        if (rx < 0 || ry < 0 || rw < 0 || rh < 0 || rx + rw > texW || ry + rh > texH) {
            return sdlFail(-1, "Invalid rect");
        }
        int srcBytesPerRow = pitch;
        int dstBytesPerRow = texPitch;
        int copyBytes = std::min(rw * 4, std::min(srcBytesPerRow, dstBytesPerRow));
        uint8_t* src = static_cast<uint8_t*>(pixels);
        uint8_t* dst = reinterpret_cast<uint8_t*>(it->second.pixels);

        for (int y = 0; y < rh; y++) {
            memcpy(dst + (ry + y) * dstBytesPerRow + rx * 4,
                   src + y * srcBytesPerRow,
                   copyBytes);
        }
    }
    return 0;
}

uint32_t stub_SDL_GetTicks();

// Converts ONE pending host event (gEventQueue, or a synthetic window event)
// into the SDL event structure at `event`.  Answers 0 when none is pending.
// SDL_PollEvent and SDL_PumpEvents call this until it answers 0 and queue each
// event through the SDL filter, the watches and the SDL queue.
static int pollNativeEvent(void* event) {
    applyNotifiedResize();
    if (!sSDL2PollEventFlagSet) {
        sSDL2PollEventFlagSet = true;
        pharo::gEventQueue.setSDL2EventPollingActive(true);
        if (GET_DEBUG_BOOL(PHARO_WIN_EVENT_TRACE)) {
            fprintf(stderr, "[WIN-EVT] SDL2 event polling ACTIVE (first SDL_PollEvent)\n");
        }
    }

    // Window-frame resize (Win32): swap the surface buffer on THIS (VM)
    // thread, then queue SIZE_CHANGED+EXPOSED so the image re-layouts the
    // World and recreates its texture at the new extent.
    if (pharo::gDisplaySurface) {
        int newW, newH;
        if (pharo::gDisplaySurface->applyPendingResize(newW, newH)) {
            ffi_notifyDisplayResize(newW, newH);
            applyNotifiedResize();   // the VM thread applies it now
        }
    }

    // Reject null/low event pointers (stale heap address after GC compaction)
    if (!event || reinterpret_cast<uintptr_t>(event) < kFirstHandle) {
        return !pharo::gEventQueue.isEmpty() ? 1 : 0;
    }

    // Deliver pending synthetic window events one at a time
    uint8_t windowEventType = 0;
    bool haveWindowEvent = false;
    {
        std::lock_guard<std::mutex> lk(sPendingWindowEventsMx);
        if (!sPendingWindowEvents.empty()) {
            windowEventType = sPendingWindowEvents.front();
            sPendingWindowEvents.pop();
            haveWindowEvent = true;
        }
    }
    if (haveWindowEvent) {
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
            std::lock_guard<std::mutex> lk(sPendingWindowEventsMx);
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
        // arg3 is the buttons-CURRENTLY-PRESSED state (Pharo event convention),
        // not "the button this event is about". Which button went down/up is
        // the DELTA against the previous state (sMouseButtons). Deriving it
        // from arg3 directly made every UP (arg3=0) report SDL_BUTTON_LEFT, so
        // a right-click's release never reached the image and Morphic's hand
        // kept the right button pressed forever — killing all later clicks.
        uint32_t sdlButtonMask = 0;
        if (pharoEvent.arg3 & 4) sdlButtonMask |= (1 << 0);  // Red -> SDL_BUTTON_LMASK
        if (pharoEvent.arg3 & 1) sdlButtonMask |= (1 << 1);  // Blue -> SDL_BUTTON_MMASK
        if (pharoEvent.arg3 & 2) sdlButtonMask |= (1 << 2);  // Yellow -> SDL_BUTTON_RMASK

        auto buttonFromMask = [](uint32_t mask, uint8_t fallback) -> uint8_t {
            if (mask & (1 << 0)) return SDL_BUTTON_LEFT;
            if (mask & (1 << 2)) return SDL_BUTTON_RIGHT;
            if (mask & (1 << 1)) return SDL_BUTTON_MIDDLE;
            return fallback;
        };

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
            sMouseButtons = sdlButtonMask;
            sRelMouseDx += sdlEvent->motion.xrel;
            sRelMouseDy += sdlEvent->motion.yrel;
        } else if (subtype == 1) {
            uint32_t pressed = sdlButtonMask & ~sMouseButtons;
            sMouseButtons = sdlButtonMask;
            sdlEvent->button.type = SDL_MOUSEBUTTONDOWN;
            sdlEvent->button.timestamp = pharoEvent.timeStamp;
            sdlEvent->button.windowID = windowID;
            sdlEvent->button.which = 0;
            sdlEvent->button.x = pharoEvent.arg1;
            sdlEvent->button.y = pharoEvent.arg2;
            sdlEvent->button.state = 1;  // SDL_PRESSED
            sdlEvent->button.clicks = 1;
            sdlEvent->button.button = buttonFromMask(pressed ? pressed : sdlButtonMask,
                                                     SDL_BUTTON_LEFT);
        } else if (subtype == 2) {
            uint32_t released = sMouseButtons & ~sdlButtonMask;
            sMouseButtons = sdlButtonMask;
            sdlEvent->button.type = SDL_MOUSEBUTTONUP;
            sdlEvent->button.timestamp = pharoEvent.timeStamp;
            sdlEvent->button.windowID = windowID;
            sdlEvent->button.which = 0;
            sdlEvent->button.x = pharoEvent.arg1;
            sdlEvent->button.y = pharoEvent.arg2;
            sdlEvent->button.state = 0;  // SDL_RELEASED
            sdlEvent->button.clicks = 1;
            sdlEvent->button.button = buttonFromMask(released, SDL_BUTTON_LEFT);
        }
        if (GET_DEBUG_BOOL(PHARO_WIN_EVENT_TRACE) && (subtype == 1 || subtype == 2)) {
            fprintf(stderr, "[WIN-EVT] poll delivers mouse %s (%d,%d) sdlButton=%d to image\n",
                    subtype == 1 ? "DOWN" : "UP",
                    pharoEvent.arg1, pharoEvent.arg2, sdlEvent->button.button);
        }
        return 1;
    } else if (pharoEvent.type == static_cast<int>(pharo::EventType::MouseWheel)) {
        // Update mouse position so SDL_GetMouseState returns the scroll
        // gesture location. OSSDL2BackendWindow>>visitMouseWheelEvent: calls
        // SDL_GetMouseState to set the event position.  Without this update,
        // the position is stale (from the last single-finger touch) and wheel
        // events may be dispatched to the wrong morph.
        sMouseX = pharoEvent.arg1;
        sMouseY = pharoEvent.arg2;

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
            if (GET_DEBUG_BOOL(PHARO_WIN_EVENT_TRACE)) {
                fprintf(stderr, "[WIN-EVT] poll delivers TEXTINPUT '%s' to image\n",
                        sdlEvent->text.text);
            }
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
        if (pharoEvent.arg4 >= 0 && pharoEvent.arg4 < kScancodeCount) sKeyState[pharoEvent.arg4] = subtype == 0 ? 1 : 0;
        sdlEvent->key.keysym.sym = charCode;
        uint16_t sdlMod = 0;
        if (pharoEvent.arg3 & 1) sdlMod |= 0x0001;  // KMOD_LSHIFT
        if (pharoEvent.arg3 & 2) sdlMod |= 0x0040;  // KMOD_LCTRL
        if (pharoEvent.arg3 & 4) sdlMod |= 0x0100;  // KMOD_LALT
        if (pharoEvent.arg3 & 8) sdlMod |= 0x0400;  // KMOD_LGUI (Cmd)
        sdlEvent->key.keysym.mod = sdlMod;
        if (GET_DEBUG_BOOL(PHARO_WIN_EVENT_TRACE)) {
            fprintf(stderr, "[WIN-EVT] poll delivers KEY%s sym=%d scan=%d mod=%d to image\n",
                    subtype == 0 ? "DOWN" : "UP", charCode, pharoEvent.arg4, sdlMod);
        }
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

// Move every pending host event into the SDL queue.  Each event goes through
// the event filter and the event watches first (SDL2Events.hpp).  This runs on
// the VM thread, so a Smalltalk callback as filter or watch is a plain
// same-thread callback.
static void pumpHostEvents() {
    SDL_Event converted;
    while (pollNativeEvent(&converted) != 0) {
        sdl2::ev::Event queued;
        static_assert(sizeof(queued) == sizeof(converted), "SDL_Event size");
        memcpy(&queued, &converted, sizeof(queued));
        if (sdl2::ev::hostEventDisabled(queued.type)) continue;
        sdl2::ev::enqueue(queued);
    }
}

// SDL_PollEvent answers the oldest queued event.  A null event pointer only
// asks whether an event is waiting (the pointer check also rejects a stale
// low address).
int stub_SDL_PollEvent(void* event) {
    pumpHostEvents();
    if (!event || reinterpret_cast<uintptr_t>(event) < kFirstHandle)
        return sdl2::ev::queueNotEmpty() ? 1 : 0;
    sdl2::ev::Event next;
    if (!sdl2::ev::popFront(next)) return 0;
    memcpy(event, &next, sizeof(next));
    return 1;
}

int stub_SDL_WaitEvent(void* event) {
    // Used by modal event loops (menu dropdown grab loops).
    // Without this, menu dropdowns open and close immediately.
    fprintf(stderr, "[SDL-WAIT] WaitEvent called\n");
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

// SDL_PushEvent: filter, watches, then the queue.  1 queued, 0 dropped by the
// filter, -1 error.  The event timestamp is set to the SDL tick count.
int stub_SDL_PushEvent(void* event) {
    if (!event) return sdl2::sys::invalidParam("event");
    return sdl2::ev::pushEvent(static_cast<sdl2::ev::Event*>(event), stub_SDL_GetTicks());
}

// Clipboard — delegates to platform bridge (which calls Swift/ObjC)
extern "C" const char* vm_getClipboardText(void);
extern "C" void vm_setClipboardText(const char* text);

static char* sdlAllocString(const char* s);

// The host bridge may keep no text (test_load_image has none), so the last text
// that SDL_SetClipboardText got is kept here too.  The host text wins when it
// is not empty, because another application may have changed the clipboard.
static std::mutex sClipboardMutex;
static std::string sClipboardCopy;

static std::string clipboardText() {
    const char* text = vm_getClipboardText();
    if (text && text[0] != '\0') return text;
    std::lock_guard<std::mutex> lk(sClipboardMutex);
    return sClipboardCopy;
}

char* stub_SDL_GetClipboardText() {
    return sdlAllocString(clipboardText().c_str());
}

int stub_SDL_SetClipboardText(const char* text) {
    if (!text) text = "";
    {
        std::lock_guard<std::mutex> lk(sClipboardMutex);
        sClipboardCopy = text;
    }
    vm_setClipboardText(text);
    return 0;
}

int stub_SDL_HasClipboardText() {
    return clipboardText().empty() ? 0 : 1;
}

void stub_SDL_free(void* mem);   // defined with the memory table (SDL2StubsCore.inc)

// Text input — delegates to platform bridge for iOS keyboard show/hide
extern "C" void vm_startTextInput(void);
extern "C" void vm_stopTextInput(void);

void stub_SDL_StartTextInput() {
    sTextInputActive = true;
    sdl2::ev::setTextEventsEnabled(true);
    vm_startTextInput();
}

void stub_SDL_StopTextInput() {
    sTextInputActive = false;
    sdl2::ev::setTextEventsEnabled(false);
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
    if (hot_x < 0 || hot_y < 0 || hot_x >= w || hot_y >= h) return sdlFailNull("Cursor hot spot doesn't lie within cursor");
    static uintptr_t nextCursorHandle = 0x30000;
    return reinterpret_cast<void*>(nextCursorHandle++);
}

void stub_SDL_SetCursor(void* cursor) {
}

void stub_SDL_FreeCursor(void* cursor) {
}

// SDL_ShowCursor answers the state before the call: 1 shown, 0 hidden; -1 only asks.
static bool sCursorShown = true;
int stub_SDL_ShowCursor(int toggle) {
    const bool before = sCursorShown;
    if (toggle >= 0) sCursorShown = toggle != 0;
    return before ? 1 : 0;
}

uint32_t stub_SDL_GetWindowFlags(void* window) {
    WINDOW_CHECK(window, 0)
    return windowFlagsOf(window);
}

// ----- Surfaces -----
// Real SDL_Surface and SDL_PixelFormat structures (SDL2Raster.hpp): the image
// reads their fields and pixels through FFI structure accessors.  Only the
// 32-bit packed formats exist, as for textures.

// The smallest row size of `w` pixels (SDL_CalculatePitch with minimal set).
static uint64_t minimalSurfacePitch(const sdl2::PixelFormatRaw& pf, int w) {
    return pf.bitsPerPixel >= 8 ? static_cast<uint64_t>(w) * pf.bytesPerPixel
                                : (static_cast<uint64_t>(w) * pf.bitsPerPixel + 7) / 8;
}

static void* createSurfaceChecked(int width, int height, const sdl2::PixelFormatRaw& proto) {
    if (sdl2::surfacePitch(proto, width) > static_cast<uint64_t>(INT32_MAX)) return sdlFailNull("Out of memory");
    sdl2::SurfaceRaw* s = sdl2::createOwnedSurface(width, height, proto);
    return s ? static_cast<void*>(s) : sdlFailNull("Out of memory");
}

void* stub_SDL_CreateRGBSurfaceWithFormat(uint32_t flags, int width, int height, int depth, uint32_t format) {
    if (width < 0) { sdl2::sys::invalidParam("width"); return nullptr; }
    if (height < 0) { sdl2::sys::invalidParam("height"); return nullptr; }
    sdl2::PixelFormatRaw proto;
    if (!sdl2::surf::formatProto(format, proto)) return nullptr;
    return createSurfaceChecked(width, height, proto);
}

void* stub_SDL_CreateRGBSurface(uint32_t flags, int width, int height, int depth,
                                uint32_t Rmask, uint32_t Gmask, uint32_t Bmask, uint32_t Amask) {
    if (width < 0) { sdl2::sys::invalidParam("width"); return nullptr; }
    if (height < 0) { sdl2::sys::invalidParam("height"); return nullptr; }
    const uint32_t format = sdl2::pix::masksToEnum(depth, Rmask, Gmask, Bmask, Amask);
    if (!format) return sdlFailNull("Unknown pixel format");
    return stub_SDL_CreateRGBSurfaceWithFormat(flags, width, height, depth, format);
}

static void* createSurfaceOver(void* pixels, int width, int height, int pitch, uint32_t format) {
    sdl2::PixelFormatRaw proto;
    if (!sdl2::surf::formatProto(format, proto)) return nullptr;
    const uint64_t minimal = minimalSurfacePitch(proto, width);
    if (pitch < 0 || (pitch > 0 && static_cast<uint64_t>(pitch) < minimal)) { sdl2::sys::invalidParam("pitch"); return nullptr; }
    if (!pixels) return sdlFailNull("Parameter 'pixels' is invalid");
    sdl2::SurfaceRaw* s = sdl2::createSurfaceFrom(pixels, width, height, pitch, proto);
    return s ? static_cast<void*>(s) : sdlFailNull("Out of memory");
}

void* stub_SDL_CreateRGBSurfaceFrom(void* pixels, int width, int height, int depth,
                                      int pitch, uint32_t Rmask, uint32_t Gmask,
                                      uint32_t Bmask, uint32_t Amask) {
    if (width < 0) { sdl2::sys::invalidParam("width"); return nullptr; }
    if (height < 0) { sdl2::sys::invalidParam("height"); return nullptr; }
    const uint32_t format = sdl2::pix::masksToEnum(depth, Rmask, Gmask, Bmask, Amask);
    if (!format) return sdlFailNull("Unknown pixel format");
    return createSurfaceOver(pixels, width, height, pitch, format);
}

void stub_SDL_FreeSurface(void* surface) {
    sdl2::freeSurface(static_cast<sdl2::SurfaceRaw*>(surface));
}

void* stub_SDL_ConvertSurfaceFormat(void* surface, uint32_t format, uint32_t flags) {
    if (!sdl2::isSurface(surface)) return sdlFailNull("Parameter 'surface' is invalid");
    return sdl2::surf::convert(static_cast<const sdl2::SurfaceRaw*>(surface), format);
}

// Software surfaces need no locking (SDL_MUSTLOCK is false for them).
int stub_SDL_LockSurface(void* surface) {
    if (!sdl2::isSurface(surface)) return sdlFail(-1, "Parameter 'surface' is invalid");
    return 0;
}

void stub_SDL_UnlockSurface(void* surface) {
}

void* stub_SDL_CreateColorCursor(void* surface, int hotX, int hotY) {
    constexpr uintptr_t kColorCursorHandleBase = 5u << 16;   // above the system and plain cursor handles
    static uintptr_t nextColorCursorHandle = kColorCursorHandleBase;
    if (!sdl2::isSurface(surface)) return sdlFailNull("Parameter 'surface' is invalid");
    return reinterpret_cast<void*>(nextColorCursorHandle++);
}

// The window surface of the display window is the display surface, so a
// drawing into it shows at SDL_UpdateWindowSurface.  Any other window gets a
// private surface of its size.
void* stub_SDL_GetWindowSurface(void* window) {
    auto it = sWindows.find(window);
    if (it == sWindows.end()) return sdlFailNull("Invalid window");
    int w = 0, h = 0;
    stub_SDL_GetWindowSize(window, &w, &h);
    const bool display = window == sMainWindow && pharo::gDisplaySurface;
    uint8_t* shared = display ? reinterpret_cast<uint8_t*>(pharo::gDisplaySurface->pixels()) : nullptr;
    sdl2::SurfaceRaw*& s = it->second.surface;
    if (s && (s->w != w || s->h != h || (display && s->pixels != shared))) {
        sdl2::freeSurface(s);
        s = nullptr;
    }
    if (!s) {
        const sdl2::FormatInfo* f = sdl2::formatInfo(sdl2::kFormatARGB8888);
        s = display ? sdl2::createSurfaceFrom(shared, w, h, static_cast<int>(pharo::gDisplaySurface->pitch()), *f)
                    : sdl2::createOwnedSurface(w, h, *f);
        if (!s) return sdlFailNull("Out of memory");
    }
    return s;
}

int stub_SDL_UpdateWindowSurface(void* window) {
    auto it = sWindows.find(window);
    if (it == sWindows.end()) return sdlFail(-1, "Invalid window");
    if (!it->second.surface) return sdlFail(-1, "Use SDL_GetWindowSurface() to get the window surface first");
    if (window == sMainWindow && pharo::gDisplaySurface) pharo::gDisplaySurface->update();
    return 0;
}

int stub_SDL_UpdateWindowSurfaceRects(void* window, const void* rects, int numRects) {
    return stub_SDL_UpdateWindowSurface(window);
}

// ----- Display modes -----
// One display, one mode: the size of the display surface at 60 Hz.

static void fillDisplayMode(SDLDisplayModeRaw* mode) {
    mode->format = sdl2::kFormatXRGB8888;
    mode->w = displayWidth();
    mode->h = displayHeight();
    mode->refreshRate = 60;
    mode->driverData = nullptr;
}

int stub_SDL_GetNumDisplayModes(int displayIndex) {
    if (displayIndex != 0) return sdlFail(-1, "displayIndex must be in the range 0 - 0");
    return 1;
}

int stub_SDL_GetDisplayMode(int displayIndex, int modeIndex, void* mode) {
    if (displayIndex != 0) return sdlFail(-1, "displayIndex must be in the range 0 - 0");
    if (modeIndex != 0) return sdlFail(-1, "index must be in the range of 0 - 0");
    if (mode) fillDisplayMode(static_cast<SDLDisplayModeRaw*>(mode));
    return 0;
}

int stub_SDL_GetCurrentDisplayMode(int displayIndex, void* mode) {
    if (displayIndex != 0) return sdlFail(-1, "displayIndex must be in the range 0 - 0");
    if (mode) fillDisplayMode(static_cast<SDLDisplayModeRaw*>(mode));
    return 0;
}

int stub_SDL_GetWindowDisplayMode(void* window, void* mode) {
    if (!sWindows.count(window)) return sdlFail(-1, "Invalid window");
    if (!mode) return sdlFail(-1, "Parameter 'mode' is invalid");
    fillDisplayMode(static_cast<SDLDisplayModeRaw*>(mode));
    return 0;
}

int stub_SDL_GetWindowDisplayIndex(void* window) {
    if (!sWindows.count(window)) return sdlFail(-1, "Invalid window");
    return 0;
}

// The logical resolution of the display is 96 dots per inch, the base the
// image divides by (OSSDL2BackendWindow class>>screenScaleFactorBaseDPI), so the
// screen scale factor it computes is 1.0.
int stub_SDL_GetDisplayDPI(int displayIndex, float* ddpi, float* hdpi, float* vdpi) {
    if (displayIndex != 0) return sdlFail(-1, "displayIndex must be in the range 0 - 0");
    constexpr float kLogicalDpi = 96.0f;
    if (ddpi) *ddpi = kLogicalDpi;
    if (hdpi) *hdpi = kLogicalDpi;
    if (vdpi) *vdpi = kLogicalDpi;
    return 0;
}

const char* stub_SDL_GetCurrentVideoDriver() {
#if defined(__APPLE__) && TARGET_OS_IPHONE && !TARGET_OS_MACCATALYST
    return "uikit";
#elif defined(__APPLE__)
    return "cocoa";
#else
    return "dummy";
#endif
}

// ----- Devices: see SDL2StubsDevices.inc (generated) and SDL2StubsDeviceHand.inc -----

// ----- Small calls -----

void stub_SDL_DisableScreenSaver() { sScreenSaverEnabled = false; }

// SDL_EventState answers the previous state of an event type and sets the new
// one unless it is SDL_QUERY (-1).  DISABLE also drops the queued events of the
// type.  Host events are filtered only for a type that was disabled by a call
// of this function (SDL2Events.hpp explains why).
uint8_t stub_SDL_EventState(uint32_t type, int state) {
    return static_cast<uint8_t>(sdl2::ev::eventState(type, state));
}

static bool sRelativeMouseMode = false;
int stub_SDL_SetRelativeMouseMode(int enabled) {
    sRelativeMouseMode = enabled != 0;
    return 0;
}

void stub_SDL_SetTextInputRect(const void* rect) {}   // IME placement hint; the app keyboard has no candidate window

void* stub_SDL_memcpy(void* dest, const void* src, size_t n) {
    return memcpy(dest, src, n);
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
    if (displayIndex != 0) return sdlFail(-1, "displayIndex must be in the range 0 - 0");
    if (!rect) return sdl2::sys::invalidParam("rect");
    {
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
    if (displayIndex != 0) return sdlFail(-1, "displayIndex must be in the range 0 - 0");
    if (!rect) return sdl2::sys::invalidParam("rect");
    {
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
static uint64_t millisecondsSinceStart() {
    static auto start = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count()
    );
}

uint32_t stub_SDL_GetTicks() {
    return static_cast<uint32_t>(millisecondsSinceStart());   // wraps after 49 days, as in SDL
}

uint64_t stub_SDL_GetTicks64() {
    return millisecondsSinceStart();
}

void stub_SDL_Delay(uint32_t ms) {
    usleep(static_cast<useconds_t>(ms) * 1000);
}

uint64_t stub_SDL_GetPerformanceCounter() {
    return std::chrono::steady_clock::now().time_since_epoch().count();
}

uint64_t stub_SDL_GetPerformanceFrequency() {
    return 1000000000ULL;  // nanoseconds
}

// Additional commonly needed SDL2 functions
uint32_t stub_SDL_WasInit(uint32_t flags) {
    const uint32_t mask = sInitMask.load();
    return flags == 0 ? mask : (mask & flags);
}

int stub_SDL_VideoInit(const char* driver) {
    return 0;
}

void stub_SDL_VideoQuit() {
}

int stub_SDL_InitSubSystem(uint32_t flags) {
    initSubsystems(flags);
    return 0;
}

void stub_SDL_QuitSubSystem(uint32_t flags) {
    sInitMask.fetch_and(~flags);
}

// Hints follow SDL_hints.c: priorities, the environment override and the
// callbacks.  See SDL2Base.hpp for the environment snapshot.
int stub_SDL_SetHint(const char* name, const char* value) {
    return sdl2::base::setHint(name, value);
}

const char* stub_SDL_GetHint(const char* name) {
    return sdl2::base::getHint(name);
}

// SDL_GLattr values (SDL_video.h) and the defaults of SDL_GL_ResetAttributes.
// The stored configuration is what SDL keeps in its video device; no GL driver
// exists here, so an attribute that SDL reads back from a live GL context
// fails as it does on a video driver without GL.
enum SDLGLAttr : int {
    kGLRedSize, kGLGreenSize, kGLBlueSize, kGLAlphaSize, kGLBufferSize, kGLDoubleBuffer,
    kGLDepthSize, kGLStencilSize, kGLAccumRedSize, kGLAccumGreenSize, kGLAccumBlueSize,
    kGLAccumAlphaSize, kGLStereo, kGLMultisampleBuffers, kGLMultisampleSamples,
    kGLAcceleratedVisual, kGLRetainedBacking, kGLContextMajorVersion, kGLContextMinorVersion,
    kGLContextEGL, kGLContextFlags, kGLContextProfileMask, kGLShareWithCurrentContext,
    kGLFramebufferSRGBCapable, kGLContextReleaseBehavior, kGLContextResetNotification,
    kGLContextNoError, kGLFloatBuffers, kGLAttrCount
};
constexpr int kGLReleaseBehaviorFlush = 1;   // SDL_GL_CONTEXT_RELEASE_BEHAVIOR_FLUSH
constexpr int kGLAcceleratedAny = -1;        // "accelerated or not, both are fine"
constexpr int kGLDefaultRedSize = 3, kGLDefaultGreenSize = 3, kGLDefaultBlueSize = 2;
constexpr int kGLDefaultDepthSize = 16, kGLDefaultMajorVersion = 2, kGLDefaultMinorVersion = 1;

static std::mutex sGLAttrMutex;
static std::array<int, kGLAttrCount> sGLAttributes = {};

static void glAttributesReset() {   // caller holds sGLAttrMutex
    sGLAttributes.fill(0);
    sGLAttributes[kGLRedSize] = kGLDefaultRedSize;
    sGLAttributes[kGLGreenSize] = kGLDefaultGreenSize;
    sGLAttributes[kGLBlueSize] = kGLDefaultBlueSize;
    sGLAttributes[kGLDepthSize] = kGLDefaultDepthSize;
    sGLAttributes[kGLDoubleBuffer] = 1;
    sGLAttributes[kGLRetainedBacking] = 1;
    sGLAttributes[kGLAcceleratedVisual] = kGLAcceleratedAny;
    sGLAttributes[kGLContextMajorVersion] = kGLDefaultMajorVersion;
    sGLAttributes[kGLContextMinorVersion] = kGLDefaultMinorVersion;
    sGLAttributes[kGLContextReleaseBehavior] = kGLReleaseBehaviorFlush;
}
static void glAttributesEnsureInit() {   // caller holds sGLAttrMutex
    static bool done = false;
    if (!done) { glAttributesReset(); done = true; }
}

int stub_SDL_GL_SetAttribute(int attr, int value) {
    std::lock_guard<std::mutex> lk(sGLAttrMutex);
    glAttributesEnsureInit();
    if (attr < 0 || attr >= kGLAttrCount) return sdlFail(-1, "Unknown OpenGL attribute");
    sGLAttributes[attr] = value;
    return 0;
}

int stub_SDL_GL_GetAttribute(int attr, int* value) {
    if (!value) return sdlFail(-1, "Parameter 'value' is invalid");
    *value = 0;
    std::lock_guard<std::mutex> lk(sGLAttrMutex);
    glAttributesEnsureInit();
    switch (attr) {
    case kGLAcceleratedVisual: *value = sGLAttributes[attr] != 0; return 0;
    case kGLRetainedBacking: case kGLContextMajorVersion: case kGLContextMinorVersion:
    case kGLContextFlags: case kGLContextProfileMask: case kGLShareWithCurrentContext:
    case kGLFramebufferSRGBCapable: case kGLContextNoError:
        *value = sGLAttributes[attr];
        return 0;
    case kGLContextEGL:
        return 0;
    case kGLRedSize: case kGLGreenSize: case kGLBlueSize: case kGLAlphaSize: case kGLBufferSize:
    case kGLDoubleBuffer: case kGLDepthSize: case kGLStencilSize: case kGLAccumRedSize:
    case kGLAccumGreenSize: case kGLAccumBlueSize: case kGLAccumAlphaSize: case kGLStereo:
    case kGLMultisampleBuffers: case kGLMultisampleSamples: case kGLContextReleaseBehavior:
        // SDL asks a live GL context for these; there is no GL entry point.
        return sdlFail(-1, (std::string("No dynamic GL support in current SDL video driver (") +
                            stub_SDL_GetCurrentVideoDriver() + ")").c_str());
    default:
        // Reset notification and float buffers cannot be read back at all.
        return sdlFail(-1, "Unknown OpenGL attribute");
    }
}

void* stub_SDL_GL_GetProcAddress(const char* name) {
    // The stub layer loads no GL library, so no entry point can be found.
    (void)name;
    sdl2::sys::lastError() = std::string("No dynamic GL support in current SDL video driver (") + stub_SDL_GetCurrentVideoDriver() + ")";
    return nullptr;
}

void stub_SDL_GL_GetDrawableSize(void* window, int* w, int* h) {
    WINDOW_CHECK(window, )
    stub_SDL_GetWindowSize(window, w, h);   // the stub window has a 1:1 pixel scale
}

// OpenGL cannot exist here.  A window without the OpenGL flag is not an OpenGL
// window; a window with the flag asks for a GL driver that this video driver
// does not have.  Both answers are the SDL texts.
static std::string glNotConfiguredText() { return contextNotSupportedText("OpenGL"); }

void* stub_SDL_GL_CreateContext(void* window) {
    WINDOW_CHECK(window, nullptr)
    if (!(sWindows[window].flags & kWindowOpenGL)) return sdlFailNull("The specified window isn't an OpenGL window");
    sdl2::sys::lastError() = glNotConfiguredText();
    return nullptr;
}

void stub_SDL_GL_DeleteContext(void* context) {
    (void)context;   // no context ever exists
}

int stub_SDL_GL_MakeCurrent(void* window, void* context) {
    if (!window && !context) return 0;   // no window and no context: SDL drops the current context
    if (window) {
        WINDOW_CHECK(window, -1)
        if (!(sWindows[window].flags & kWindowOpenGL)) return sdlFail(-1, "The specified window isn't an OpenGL window");
    }
    return sdlFail(-1, glNotConfiguredText().c_str());
}

void stub_SDL_GL_SwapWindow(void* window) {
    WINDOW_CHECK(window, )
    if (!(sWindows[window].flags & kWindowOpenGL)) { sdl2::sys::lastError() = "The specified window isn't an OpenGL window"; return; }
    sdl2::sys::lastError() = "The specified window has not been made current";
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

// Registrations of the stubs that complete the SDL2 layer (renderer state,
// events, threads, text and so on).  Kept apart from registerSDL2Stubs so that
// each family is easy to find.
#define SDL_REG(name) registerFunction("SDL_" #name, reinterpret_cast<void*>(stub_SDL_##name))
#include "SDL2StubsCore.inc"
#include "SDL2StubsVideo.inc"
#include "SDL2StubsDeviceHand.inc"
#include "SDL2StubsDevices.inc"
#include "SDL2StubsSurface.inc"
static void registerSDL2FullStubs() {
#include "SDL2Register.inc"
}
static void registerSDL2RenderStubs() {
    SDL_REG(RenderGetClipRect); SDL_REG(RenderIsClipEnabled); SDL_REG(RenderSetViewport);
    SDL_REG(RenderGetViewport); SDL_REG(RenderSetScale); SDL_REG(RenderGetScale);
    SDL_REG(RenderSetLogicalSize); SDL_REG(RenderSetIntegerScale); SDL_REG(RenderGetIntegerScale);
    SDL_REG(RenderWindowToLogical); SDL_REG(RenderLogicalToWindow);
    SDL_REG(RenderDrawPoints); SDL_REG(RenderDrawPointsF); SDL_REG(RenderDrawPointF);
    SDL_REG(RenderDrawLines); SDL_REG(RenderDrawLinesF); SDL_REG(RenderDrawLineF);
    SDL_REG(RenderDrawRectF); SDL_REG(RenderDrawRects); SDL_REG(RenderDrawRectsF);
    SDL_REG(RenderFillRects); SDL_REG(RenderFillRectsF);
    SDL_REG(RenderCopyEx); SDL_REG(RenderCopyExF);
}

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

    // Added with the software renderer, surfaces and device stubs.
    registerFunction("SDL_GetHint", reinterpret_cast<void*>(stub_SDL_GetHint));
    registerFunction("SDL_ComposeCustomBlendMode", reinterpret_cast<void*>(stub_SDL_ComposeCustomBlendMode));
    registerFunction("SDL_GetTicks64", reinterpret_cast<void*>(stub_SDL_GetTicks64));
    registerFunction("SDL_Delay", reinterpret_cast<void*>(stub_SDL_Delay));
    registerFunction("SDL_QueryTexture", reinterpret_cast<void*>(stub_SDL_QueryTexture));
    registerFunction("SDL_GetRenderDriverInfo", reinterpret_cast<void*>(stub_SDL_GetRenderDriverInfo));
    registerFunction("SDL_GetNumRenderDrivers", reinterpret_cast<void*>(stub_SDL_GetNumRenderDrivers));
    registerFunction("SDL_GetRendererInfo", reinterpret_cast<void*>(stub_SDL_GetRendererInfo));
    registerFunction("SDL_RenderReadPixels", reinterpret_cast<void*>(stub_SDL_RenderReadPixels));
    registerFunction("SDL_RenderCopyF", reinterpret_cast<void*>(stub_SDL_RenderCopyF));
    registerFunction("SDL_RenderDrawPoint", reinterpret_cast<void*>(stub_SDL_RenderDrawPoint));
    registerFunction("SDL_RenderDrawLine", reinterpret_cast<void*>(stub_SDL_RenderDrawLine));
    registerFunction("SDL_RenderDrawRect", reinterpret_cast<void*>(stub_SDL_RenderDrawRect));
    registerFunction("SDL_RenderFillRect", reinterpret_cast<void*>(stub_SDL_RenderFillRect));
    registerFunction("SDL_RenderFillRectF", reinterpret_cast<void*>(stub_SDL_RenderFillRectF));
    registerFunction("SDL_RenderGeometry", reinterpret_cast<void*>(stub_SDL_RenderGeometry));
    registerFunction("SDL_RenderGeometryRaw", reinterpret_cast<void*>(stub_SDL_RenderGeometryRaw));
    registerFunction("SDL_SetRenderDrawBlendMode", reinterpret_cast<void*>(stub_SDL_SetRenderDrawBlendMode));
    registerFunction("SDL_RenderTargetSupported", reinterpret_cast<void*>(stub_SDL_RenderTargetSupported));
    registerFunction("SDL_SetRenderTarget", reinterpret_cast<void*>(stub_SDL_SetRenderTarget));
    registerFunction("SDL_RenderSetClipRect", reinterpret_cast<void*>(stub_SDL_RenderSetClipRect));
    registerFunction("SDL_RenderGetLogicalSize", reinterpret_cast<void*>(stub_SDL_RenderGetLogicalSize));
    registerFunction("SDL_RenderSetVSync", reinterpret_cast<void*>(stub_SDL_RenderSetVSync));
    registerFunction("SDL_SetTextureBlendMode", reinterpret_cast<void*>(stub_SDL_SetTextureBlendMode));
    registerFunction("SDL_SetTextureColorMod", reinterpret_cast<void*>(stub_SDL_SetTextureColorMod));
    registerFunction("SDL_SetTextureAlphaMod", reinterpret_cast<void*>(stub_SDL_SetTextureAlphaMod));
    registerFunction("SDL_SetTextureScaleMode", reinterpret_cast<void*>(stub_SDL_SetTextureScaleMode));
    registerFunction("SDL_GetWindowTitle", reinterpret_cast<void*>(stub_SDL_GetWindowTitle));
    registerFunction("SDL_MinimizeWindow", reinterpret_cast<void*>(stub_SDL_MinimizeWindow));
    registerFunction("SDL_MaximizeWindow", reinterpret_cast<void*>(stub_SDL_MaximizeWindow));
    registerFunction("SDL_RestoreWindow", reinterpret_cast<void*>(stub_SDL_RestoreWindow));
    registerFunction("SDL_SetWindowBordered", reinterpret_cast<void*>(stub_SDL_SetWindowBordered));
    registerFunction("SDL_SetWindowResizable", reinterpret_cast<void*>(stub_SDL_SetWindowResizable));
    registerFunction("SDL_SetWindowInputFocus", reinterpret_cast<void*>(stub_SDL_SetWindowInputFocus));
    registerFunction("SDL_SetWindowHitTest", reinterpret_cast<void*>(stub_SDL_SetWindowHitTest));
    registerFunction("SDL_CreateRGBSurface", reinterpret_cast<void*>(stub_SDL_CreateRGBSurface));
    registerFunction("SDL_CreateRGBSurfaceWithFormat", reinterpret_cast<void*>(stub_SDL_CreateRGBSurfaceWithFormat));
    registerFunction("SDL_ConvertSurfaceFormat", reinterpret_cast<void*>(stub_SDL_ConvertSurfaceFormat));
    registerFunction("SDL_LockSurface", reinterpret_cast<void*>(stub_SDL_LockSurface));
    registerFunction("SDL_UnlockSurface", reinterpret_cast<void*>(stub_SDL_UnlockSurface));
    registerFunction("SDL_CreateColorCursor", reinterpret_cast<void*>(stub_SDL_CreateColorCursor));
    registerFunction("SDL_GetWindowSurface", reinterpret_cast<void*>(stub_SDL_GetWindowSurface));
    registerFunction("SDL_UpdateWindowSurface", reinterpret_cast<void*>(stub_SDL_UpdateWindowSurface));
    registerFunction("SDL_UpdateWindowSurfaceRects", reinterpret_cast<void*>(stub_SDL_UpdateWindowSurfaceRects));
    registerFunction("SDL_GetNumDisplayModes", reinterpret_cast<void*>(stub_SDL_GetNumDisplayModes));
    registerFunction("SDL_GetDisplayMode", reinterpret_cast<void*>(stub_SDL_GetDisplayMode));
    registerFunction("SDL_GetCurrentDisplayMode", reinterpret_cast<void*>(stub_SDL_GetCurrentDisplayMode));
    registerFunction("SDL_GetWindowDisplayMode", reinterpret_cast<void*>(stub_SDL_GetWindowDisplayMode));
    registerFunction("SDL_GetWindowDisplayIndex", reinterpret_cast<void*>(stub_SDL_GetWindowDisplayIndex));
    registerFunction("SDL_GetDisplayDPI", reinterpret_cast<void*>(stub_SDL_GetDisplayDPI));
    registerFunction("SDL_GetCurrentVideoDriver", reinterpret_cast<void*>(stub_SDL_GetCurrentVideoDriver));
    registerFunction("SDL_DisableScreenSaver", reinterpret_cast<void*>(stub_SDL_DisableScreenSaver));
    registerFunction("SDL_EventState", reinterpret_cast<void*>(stub_SDL_EventState));
    registerFunction("SDL_SetRelativeMouseMode", reinterpret_cast<void*>(stub_SDL_SetRelativeMouseMode));
    registerFunction("SDL_SetTextInputRect", reinterpret_cast<void*>(stub_SDL_SetTextInputRect));
    registerFunction("SDL_memcpy", reinterpret_cast<void*>(stub_SDL_memcpy));
    registerFunction("SDL_GL_GetAttribute", reinterpret_cast<void*>(stub_SDL_GL_GetAttribute));
    registerFunction("SDL_GL_GetProcAddress", reinterpret_cast<void*>(stub_SDL_GL_GetProcAddress));
    registerFunction("SDL_GL_GetDrawableSize", reinterpret_cast<void*>(stub_SDL_GL_GetDrawableSize));

    registerSDL2RenderStubs();
    registerSDL2FullStubs();
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
