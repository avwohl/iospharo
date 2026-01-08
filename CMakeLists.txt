# iospharo - iOS Pharo Smalltalk VM
cmake_minimum_required(VERSION 3.20)
project(iospharo VERSION 1.0.0 LANGUAGES C CXX OBJC OBJCXX)

# Path to pharo-vm source (use iOS fork if available)
set(PHARO_VM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../pharo-vm-ios" CACHE PATH "Path to pharo-vm source")
if(NOT EXISTS "${PHARO_VM_DIR}/CMakeLists.txt")
    # Fall back to original pharo-vm
    set(PHARO_VM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../pharo-vm" CACHE PATH "Path to pharo-vm source" FORCE)
endif()

if(NOT EXISTS "${PHARO_VM_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "pharo-vm not found at ${PHARO_VM_DIR}. Set PHARO_VM_DIR correctly.")
endif()

# Options
option(BUILD_IOS "Build for iOS" ON)
option(BUILD_CATALYST "Build for Mac Catalyst" OFF)
option(FEATURE_LIB_FFI "Enable FFI support" OFF)  # Disabled for initial iOS build
option(USE_CPP_OOP "Use C++ Oop wrapper for ASLR compatibility" OFF)

# Include iOS platform configuration
include(cmake/iOS.cmake)

# VM library name
set(VM_LIBRARY_NAME "PharoVMCore")
set(APPNAME "iospharo")

# Collect generated VM sources from pharo-vm build directory
# Generated sources are created during pharo-vm build
set(PHARO_VM_BUILD_DIR "${PHARO_VM_DIR}/build-gen" CACHE PATH "Path to pharo-vm build directory")

# Use only the interpreter, not the JIT compiler (cogit*.c)
# iOS doesn't support JIT due to code signing restrictions
# We use a patched cointerp.c that removes strict address checks (iOS uses ASLR)
if(USE_CPP_OOP)
    # C++ version with Oop wrapper class for full ASLR compatibility
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/ios/cointerp.cpp")
        set(GENERATED_VM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/ios/cointerp.cpp")
        message(STATUS "Using C++ interpreter with Oop wrapper (cointerp.cpp)")
    else()
        message(WARNING "cointerp.cpp not found, falling back to C version")
        set(GENERATED_VM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/ios/cointerp.c")
    endif()
    # Add C++ specific definitions
    add_compile_definitions(PHARO_IOS_OOP_WRAPPER=1)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
else()
    set(GENERATED_VM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/ios/cointerp.c")
    message(STATUS "Using C interpreter (cointerp.c)")
endif()

# Collect plugin sources
file(GLOB_RECURSE PLUGIN_SOURCES
    "${PHARO_VM_DIR}/extracted/plugins/*/src/common/*.c"
    "${PHARO_VM_DIR}/extracted/plugins/*/src/osx/*.c"
)

# Exclude plugins/sources that are incompatible with iOS
list(FILTER PLUGIN_SOURCES EXCLUDE REGEX "SqueakSSL")
list(FILTER PLUGIN_SOURCES EXCLUDE REGEX "BitBltArm")  # ARM-specific optimizations have type issues on 64-bit iOS

# FFI sources if enabled
if(FEATURE_LIB_FFI)
    file(GLOB FFI_SOURCES
        "${PHARO_VM_DIR}/ffi/src/*.c"
        "${PHARO_VM_DIR}/ffi/src/sameThread/*.c"
        "${PHARO_VM_DIR}/ffi/src/callbacks/*.c"
    )
endif()

# iOS-specific sources
set(IOS_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ios/iosStubs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ios/cogStubs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ios/iosMemory.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ios/iosDisplay.m
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ios/iosEvents.m
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ios/iosParameters.m
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ios/iosUtils.mm
)

# C++ wrapper for ASLR-compatible space detection (hybrid approach)
# This provides C-callable functions that use the C++ Oop class
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/ios/oop_wrapper.cpp")
    set(OOP_WRAPPER_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ios/oop_wrapper.cpp
    )
    message(STATUS "Including C++ Oop wrapper for ASLR compatibility")
    add_compile_definitions(PHARO_IOS_OOP_WRAPPER=1)
    # Ensure C++ is enabled
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
else()
    set(OOP_WRAPPER_SOURCES "")
endif()

# Core VM support sources from pharo-vm
set(VM_SUPPORT_SOURCES
    ${PHARO_VM_DIR}/src/client.c
    ${PHARO_VM_DIR}/src/debug.c
    ${PHARO_VM_DIR}/src/externalPrimitives.c
    ${PHARO_VM_DIR}/src/fileUtils.c
    ${PHARO_VM_DIR}/src/imageAccess.c
    ${PHARO_VM_DIR}/src/pathUtilities.c
    ${PHARO_VM_DIR}/src/parameters/parameters.c
    ${PHARO_VM_DIR}/src/parameters/parameterVector.c
    ${PHARO_VM_DIR}/src/semaphores/platformSemaphore.c
    ${PHARO_VM_DIR}/src/semaphores/pharoSemaphore.c
    ${PHARO_VM_DIR}/src/threadSafeQueue/threadSafeQueue.c
    ${PHARO_VM_DIR}/src/utils.c
    # Common VM sources
    ${PHARO_VM_DIR}/extracted/vm/src/common/heartbeat.c
    ${PHARO_VM_DIR}/extracted/vm/src/common/sqExternalSemaphores.c
    ${PHARO_VM_DIR}/extracted/vm/src/common/sqHeapMap.c
    ${PHARO_VM_DIR}/extracted/vm/src/common/sqNamedPrims.c
    ${PHARO_VM_DIR}/extracted/vm/src/common/sqVirtualMachine.c
    # OSX/Unix sources
    ${PHARO_VM_DIR}/extracted/vm/src/osx/aioOSX.c
    ${PHARO_VM_DIR}/extracted/vm/src/unix/sqUnixCharConv.c
)

# Build static library for iOS embedding
add_library(${VM_LIBRARY_NAME} STATIC
    ${GENERATED_VM_SOURCES}
    ${EXTRACTED_SOURCES}
    ${VM_SUPPORT_SOURCES}
    ${IOS_SOURCES}
    ${OOP_WRAPPER_SOURCES}
    ${PLUGIN_SOURCES}
    ${FFI_SOURCES}
)

# Apply platform configuration
add_platform_headers()
add_required_libs_per_platform()

# Include directories - local includes FIRST to override upstream files
target_include_directories(${VM_LIBRARY_NAME}
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include          # LOCAL FIRST - overrides sqNamedPrims.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include/pharovm  # LOCAL pharovm headers
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ios              # For oop.hpp
        ${PHARO_VM_DIR}/include
        ${PHARO_VM_DIR}/include/pharovm
        ${PHARO_VM_DIR}/extracted/vm/include
        ${PHARO_VM_DIR}/extracted/vm/include/common      # For sqMemoryAccessIOS.h
        ${PHARO_VM_DIR}/ffi/include
    PRIVATE
        ${PHARO_VM_DIR}/src
        ${PHARO_VM_BUILD_DIR}/generated/64/vm/include
        ${PHARO_VM_BUILD_DIR}/build/include
        ${PHARO_VM_BUILD_DIR}/build/include/pharovm
)

# Also check if the fork has oop.hpp
if(EXISTS "${PHARO_VM_DIR}/src/ios/oop.hpp")
    target_include_directories(${VM_LIBRARY_NAME} PUBLIC ${PHARO_VM_DIR}/src/ios)
endif()

# Plugin include directories
file(GLOB PLUGIN_INCLUDE_DIRS "${PHARO_VM_DIR}/extracted/plugins/*/include/common")
file(GLOB PLUGIN_OSX_INCLUDE_DIRS "${PHARO_VM_DIR}/extracted/plugins/*/include/osx")
file(GLOB PLUGIN_UNIX_INCLUDE_DIRS "${PHARO_VM_DIR}/extracted/plugins/*/include/unix")
target_include_directories(${VM_LIBRARY_NAME} PRIVATE
    ${PLUGIN_INCLUDE_DIRS}
    ${PLUGIN_OSX_INCLUDE_DIRS}
    ${PLUGIN_UNIX_INCLUDE_DIRS}
)

# Compile definitions
target_compile_definitions(${VM_LIBRARY_NAME} PRIVATE
    SQUEAK_BUILTIN_PLUGIN
    PharoVM=1
    IMMUTABILITY=1
    COGMTVM=0
    STACKVM=1
    SPURVM=1
    _FILE_OFFSET_BITS=64
)

# C standard
set_target_properties(${VM_LIBRARY_NAME} PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)

# C++ compilation options for legacy C code with pointer/integer mixing
if(USE_CPP_OOP)
    target_compile_options(${VM_LIBRARY_NAME} PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-fpermissive>
        $<$<COMPILE_LANGUAGE:CXX>:-Wno-writable-strings>
        $<$<COMPILE_LANGUAGE:CXX>:-Wno-int-to-pointer-cast>
        $<$<COMPILE_LANGUAGE:CXX>:-Wno-pointer-to-int-cast>
        $<$<COMPILE_LANGUAGE:CXX>:-Wno-incompatible-pointer-types>
        $<$<COMPILE_LANGUAGE:CXX>:-Wno-int-conversion>
        $<$<COMPILE_LANGUAGE:CXX>:-Wno-void-pointer-to-int-cast>
        $<$<COMPILE_LANGUAGE:CXX>:-Wno-pointer-sign>
        $<$<COMPILE_LANGUAGE:CXX>:-Wno-incompatible-pointer-types-discards-qualifiers>
        $<$<COMPILE_LANGUAGE:CXX>:-Wno-implicit-function-declaration>
        $<$<COMPILE_LANGUAGE:CXX>:-Wno-return-type>
        $<$<COMPILE_LANGUAGE:CXX>:-ferror-limit=0>
    )
endif()

# iOS-specific settings
set_target_properties(${VM_LIBRARY_NAME} PROPERTIES
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "${CMAKE_OSX_DEPLOYMENT_TARGET}"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"  # iPhone and iPad
    XCODE_ATTRIBUTE_ENABLE_BITCODE "NO"
)

# Install rules
configure_installables(runtime)

# Export for Xcode project
export(TARGETS ${VM_LIBRARY_NAME}
    FILE "${CMAKE_CURRENT_BINARY_DIR}/${VM_LIBRARY_NAME}Targets.cmake"
)

message(STATUS "iospharo configured for iOS")
message(STATUS "  VM Library: ${VM_LIBRARY_NAME}")
message(STATUS "  Pharo VM source: ${PHARO_VM_DIR}")
message(STATUS "  iOS Deployment Target: ${CMAKE_OSX_DEPLOYMENT_TARGET}")
