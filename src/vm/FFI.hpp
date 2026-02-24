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

} // namespace ffi
} // namespace pharo

// C-linkage functions called from PlatformBridge.cpp
extern "C" {
    void ffi_notifyDisplayResize(int width, int height);
}

#endif // PHARO_FFI_HPP
