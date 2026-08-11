/*
 * FFI.hpp - Foreign Function Interface for SDL2
 *
 * Minimal FFI implementation to support Pharo's OSSDL2Driver.
 * Only implements the SDL2 functions needed for display.
 */

#ifndef PHARO_FFI_HPP
#define PHARO_FFI_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include "Oop.hpp"

// Forward declarations
class ObjectMemory;
class Interpreter;

namespace pharo {
namespace ffi {

// FFI type codes (matching Pharo's type system)
enum class FFIType {
    Void,
    Bool,
    Int8,
    Int16,
    Int32,
    Int64,
    UInt8,
    UInt16,
    UInt32,
    UInt64,
    Float,
    Double,
    Pointer,
    String,
    Struct,
    Unknown
};

// Result of an FFI call
struct FFIResult {
    bool success;
    uint64_t intValue;
    double floatValue;
    void* ptrValue;
    FFIType type;
    std::string error;
};

// Initialize FFI system (loads SDL2)
bool initializeFFI();

// Shutdown FFI system
void shutdownFFI();

// Set the app bundle path so libraries in Contents/Frameworks can be found.
// Must be called before any FFI lookups (typically at app startup).
void setAppBundlePath(const std::string& bundlePath);

// Get the app's Frameworks search path (empty if not set)
const std::string& getAppFrameworksPath();

// Check if a module/function is available
bool isModuleLoaded(const std::string& moduleName);
void* lookupFunction(const std::string& moduleName, const std::string& funcName);

// Register a stub function (for iOS SDL2 replacement)
void registerFunction(const std::string& funcName, void* funcPtr);

// Look up ONLY explicitly-registered functions (registerFunction wrappers/
// stubs like safe_objc_registerClassPair) — never lazily-cached dlsym hits
// or the missing-library fallback stubs.  Symbol resolution with a REAL
// dlopen handle must consult this first (wrappers intercept), then the
// handle, and only then the full lookupFunction fallback chain; otherwise
// the FT_/cairo_ fallback stubs preempt a perfectly loadable library
// (x86-Linux box: real libfreetype present, FT_* still stubbed).
void* lookupRegisteredFunction(const std::string& funcName);

// Register all SDL2 stub functions for iOS
void registerSDL2Stubs();

// Parse FFI type from Pharo type name
FFIType parseType(const std::string& typeName);

// Call an FFI function
// Returns the result as an Oop
FFIResult callFunction(
    void* funcPtr,
    const std::vector<FFIType>& argTypes,
    const std::vector<uint64_t>& argValues,
    FFIType returnType
);

// Higher-level: call from Pharo FFI specification
// spec is the array from ffiCall: #( returnType funcName ( argType argName, ... ) )
FFIResult callFromSpec(
    ObjectMemory& memory,
    Interpreter& interp,
    Oop specArray,
    Oop receiver,
    int argCount
);

// Candidate file names to try for an FFI module name, most specific first.
//
// Lives here because BOTH `primitiveLoadModule` (Primitives.cpp) and
// `tryLoadFromSearchPaths` (FFI.cpp) need it and they had drifted: the latter
// had an `#ifdef __APPLE__` .dylib/.so split while the former appended ONLY
// ".dylib", on every platform.  On Linux that meant a module named "sqlite3"
// was looked for as sqlite3 / libsqlite3 / sqlite3.dylib / libsqlite3.dylib and
// never as libsqlite3.so.0 — 111 "Error: Module not found." in the arm package
// sweep's pharo-rdbms-pharo-sqlite3, where stock Cog passes 122/122.
std::vector<std::string> moduleCandidates(const std::string& moduleName);

} // namespace ffi
} // namespace pharo

// C-linkage functions called from PlatformBridge.cpp
extern "C" {
    void ffi_notifyDisplayResize(int width, int height);
}

#endif // PHARO_FFI_HPP
