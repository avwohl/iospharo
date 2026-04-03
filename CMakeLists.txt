# PharoVM - Clean C++ VM for macOS and iOS
cmake_minimum_required(VERSION 3.20)
project(PharoVM LANGUAGES C CXX OBJC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Crypto build option — controls DSAPrims, SqueakSSL, and bundled OpenSSL
option(PHARO_WITH_CRYPTO "Include cryptographic libraries (DSAPrims, SqueakSSL, OpenSSL)" ON)

# Detect platform for xcframework slice selection
# FORCE_XCFRAMEWORK_PLATFORM can be set externally (e.g., for Mac Catalyst builds
# where cmake configures for iOS but xcodebuild targets macabi)
if(FORCE_XCFRAMEWORK_PLATFORM)
    set(XCFRAMEWORK_PLATFORM "${FORCE_XCFRAMEWORK_PLATFORM}")
elseif(CMAKE_C_FLAGS MATCHES "macabi" OR CMAKE_CXX_FLAGS MATCHES "macabi")
    set(XCFRAMEWORK_PLATFORM "ios-arm64_x86_64-maccatalyst")
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    if(CMAKE_OSX_SYSROOT MATCHES "Simulator")
        set(XCFRAMEWORK_PLATFORM "ios-arm64_x86_64-simulator")
    else()
        set(XCFRAMEWORK_PLATFORM "ios-arm64")
    endif()
else()
    set(XCFRAMEWORK_PLATFORM "macos-arm64_x86_64")
endif()

message(STATUS "Building for platform: ${CMAKE_SYSTEM_NAME}")
message(STATUS "XCFramework slice: ${XCFRAMEWORK_PLATFORM}")
message(STATUS "Crypto support: ${PHARO_WITH_CRYPTO}")

# VM Core Library
set(VM_SOURCES
    src/vm/ObjectMemory.cpp
    src/vm/ImageLoader.cpp
    src/vm/ImageWriter.cpp
    src/vm/Interpreter.cpp
    src/vm/WorldRenderer.cpp
    src/vm/Primitives.cpp
    src/vm/FFI.cpp
    src/vm/InterpreterProxy.cpp
    src/vm/plugins/SocketPlugin.cpp
    src/vm/plugins/SoundPlugin.cpp
    src/vm/plugins/MIDIPlugin.cpp
    src/platform/EventQueue.cpp
    src/platform/PlatformBridge.cpp
    src/platform/iOSBridge.cpp
    src/platform/MotionData.cpp
    src/vm/ObjCExceptionGuard.m
    src/vm/jit/CodeZone.cpp
    src/vm/jit/JITCompiler.cpp
    src/vm/jit/JITRuntime.cpp
)

# Mark Objective-C sources explicitly
set_source_files_properties(src/vm/ObjCExceptionGuard.m PROPERTIES LANGUAGE OBJC)

# VMMaker-generated C plugins (compiled as C, not C++)
set(PLUGIN_C_SOURCES
    src/vm/plugins/B2DPlugin.c
    src/vm/plugins/JPEGReaderPlugin.c
)

# JPEGReadWriter2Plugin with bundled libjpeg 6b
file(GLOB JPEG_SOURCES src/vm/plugins/jpeg/*.c)
list(APPEND PLUGIN_C_SOURCES ${JPEG_SOURCES})

# FFI Test Library (force-loaded symbols available via dlsym)
file(GLOB TESTLIB_SOURCES src/vm/plugins/testlib/*.c)
list(APPEND PLUGIN_C_SOURCES ${TESTLIB_SOURCES})

# Crypto plugins (conditional)
if(PHARO_WITH_CRYPTO)
    list(APPEND PLUGIN_C_SOURCES
        src/vm/plugins/DSAPrims.c
        src/vm/plugins/SqueakSSL.c
        src/vm/plugins/sqMacSSL.c
    )
endif()

# Mark all plugin sources as C language and set include paths
set_source_files_properties(${PLUGIN_C_SOURCES} PROPERTIES LANGUAGE C)
list(APPEND VM_SOURCES ${PLUGIN_C_SOURCES})

add_library(PharoVMCore STATIC ${VM_SOURCES})

# JIT: respect command-line override, default to enabled on non-iOS
if(DEFINED PHARO_JIT_ENABLED)
    target_compile_definitions(PharoVMCore PUBLIC PHARO_JIT_ENABLED=${PHARO_JIT_ENABLED})
    message(STATUS "JIT: ${PHARO_JIT_ENABLED} (command-line override)")
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    target_compile_definitions(PharoVMCore PUBLIC PHARO_JIT_ENABLED=0)
    message(STATUS "JIT: disabled (iOS)")
else()
    target_compile_definitions(PharoVMCore PUBLIC PHARO_JIT_ENABLED=1)
    message(STATUS "JIT: enabled")
endif()

# Pass PHARO_WITH_CRYPTO as a compile definition
if(PHARO_WITH_CRYPTO)
    target_compile_definitions(PharoVMCore PUBLIC PHARO_WITH_CRYPTO=1)
else()
    target_compile_definitions(PharoVMCore PUBLIC PHARO_WITH_CRYPTO=0)
endif()

# Include directories - use platform-appropriate xcframework slice
target_include_directories(PharoVMCore PUBLIC
    ${CMAKE_SOURCE_DIR}/src/vm
    ${CMAKE_SOURCE_DIR}/src/vm/jit
    ${CMAKE_SOURCE_DIR}/src/vm/plugins
    ${CMAKE_SOURCE_DIR}/src/vm/plugins/jpeg
    ${CMAKE_SOURCE_DIR}/src/vm/plugins/testlib
    ${CMAKE_SOURCE_DIR}/src/platform
    ${CMAKE_SOURCE_DIR}/Frameworks/SDL2.xcframework/${XCFRAMEWORK_PLATFORM}/Headers
    ${CMAKE_SOURCE_DIR}/Frameworks/libffi.xcframework/${XCFRAMEWORK_PLATFORM}/Headers
)

target_compile_options(PharoVMCore PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)

# Suppress warnings in third-party libjpeg code
file(GLOB JPEG_ONLY_SOURCES src/vm/plugins/jpeg/j*.c src/vm/plugins/jpeg/Error.c)
if(JPEG_ONLY_SOURCES)
    set_source_files_properties(${JPEG_ONLY_SOURCES} PROPERTIES
        COMPILE_FLAGS "-w"
    )
endif()

# Suppress warnings in testlib code
if(TESTLIB_SOURCES)
    set_source_files_properties(${TESTLIB_SOURCES} PROPERTIES
        COMPILE_FLAGS "-w"
    )
endif()

# Suppress deprecation warnings in SqueakSSL (Apple SecureTransport deprecated but still works)
if(PHARO_WITH_CRYPTO)
    set_source_files_properties(src/vm/plugins/sqMacSSL.c PROPERTIES
        COMPILE_FLAGS "-w"
    )
endif()

# Link SDL2 static library with force_load to ensure all symbols are available for dlsym
set(SDL2_LIBRARY ${CMAKE_SOURCE_DIR}/Frameworks/SDL2.xcframework/${XCFRAMEWORK_PLATFORM}/libSDL2.a)
if(EXISTS ${SDL2_LIBRARY})
    # Force load all symbols from SDL2 so they're available via dlsym at runtime
    target_link_libraries(PharoVMCore "-Wl,-force_load,${SDL2_LIBRARY}")
    message(STATUS "Using SDL2 (force_load): ${SDL2_LIBRARY}")
endif()

# Link libffi static library
set(LIBFFI_LIBRARY ${CMAKE_SOURCE_DIR}/Frameworks/libffi.xcframework/${XCFRAMEWORK_PLATFORM}/libffi.a)
if(EXISTS ${LIBFFI_LIBRARY})
    target_link_libraries(PharoVMCore ${LIBFFI_LIBRARY})
    message(STATUS "Using libffi: ${LIBFFI_LIBRARY}")
endif()

# Platform-specific frameworks
if(APPLE)
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        # iOS frameworks for SDL2 and text rendering
        target_link_libraries(PharoVMCore
            "-framework UIKit"
            "-framework Foundation"
            "-framework CoreGraphics"
            "-framework CoreText"
            "-framework QuartzCore"
            "-framework Metal"
            "-framework CoreAudio"
            "-framework AudioToolbox"
            "-framework AVFoundation"
            "-framework GameController"
            "-framework CoreHaptics"
            "-framework CoreMotion"
            "-framework CoreMIDI"
        )
        # SqueakSSL uses Apple Security.framework
        if(PHARO_WITH_CRYPTO)
            target_link_libraries(PharoVMCore "-framework Security")
        endif()
    else()
        # macOS frameworks for SDL2 and text rendering
        target_link_libraries(PharoVMCore
            "-framework Cocoa"
            "-framework IOKit"
            "-framework CoreGraphics"
            "-framework CoreText"
            "-framework CoreAudio"
            "-framework AudioToolbox"
            "-framework CoreVideo"
            "-framework Metal"
            "-framework QuartzCore"
            "-framework GameController"
            "-framework CoreHaptics"
            "-framework ForceFeedback"
            "-framework Carbon"
            "-framework CoreMotion"
            "-framework CoreMIDI"
            "-liconv"
        )
        # SqueakSSL uses Apple Security.framework
        if(PHARO_WITH_CRYPTO)
            target_link_libraries(PharoVMCore "-framework Security")
        endif()
    endif()
endif()

# Third-party xcframeworks for iOS (built by scripts/build-third-party.sh)
# Each entry: xcframework_name:library_filename
foreach(LIB_ENTRY cairo:libcairo.a freetype:libfreetype.a pixman:libpixman-1.a harfbuzz:libharfbuzz.a libpng16:libpng16.a)
    string(REPLACE ":" ";" LIB_PARTS ${LIB_ENTRY})
    list(GET LIB_PARTS 0 XCF_NAME)
    list(GET LIB_PARTS 1 LIB_FILE)
    set(LIB_XCF ${CMAKE_SOURCE_DIR}/Frameworks/${XCF_NAME}.xcframework/${XCFRAMEWORK_PLATFORM}/${LIB_FILE})
    if(EXISTS ${LIB_XCF})
        target_link_libraries(PharoVMCore ${LIB_XCF})
        message(STATUS "Using ${XCF_NAME}: ${LIB_XCF}")
    endif()
endforeach()

# Crypto xcframeworks (conditional)
if(PHARO_WITH_CRYPTO)
    foreach(LIB_NAME ssl crypto ssh2 git2)
        set(LIB_XCF ${CMAKE_SOURCE_DIR}/Frameworks/lib${LIB_NAME}.xcframework/${XCFRAMEWORK_PLATFORM}/lib${LIB_NAME}.a)
        if(EXISTS ${LIB_XCF})
            target_link_libraries(PharoVMCore ${LIB_XCF})
            message(STATUS "Using lib${LIB_NAME}: ${LIB_XCF}")
        endif()
    endforeach()
endif()

# Test executables (macOS only)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_executable(test_load_image src/vm/test_load_image.cpp)
    target_link_libraries(test_load_image PharoVMCore)
    # Force export testlib symbols so they're available via dlsym
    target_link_options(test_load_image PRIVATE
        -Wl,-u,_singleCallToCallback
        -Wl,-u,_callbackInALoop
        -Wl,-u,_reentringCallback
        -Wl,-u,_callbackFromAnotherThread
        -Wl,-u,_getValue
        -Wl,-u,_simple_callback_f_int
        -Wl,-u,_simple_callback_f_float
        -Wl,-u,_simple_callback_f_double
        -Wl,-u,_simple_callback_f_char
        -Wl,-u,_simple_callback_f_short
        -Wl,-u,_simple_callback_f_long
        -Wl,-u,_simple_callback_f_int8_t
        -Wl,-u,_simple_callback_f_int16_t
        -Wl,-u,_simple_callback_f_int32_t
        -Wl,-u,_simple_callback_f_int64_t
        -Wl,-u,_simple_callback_f_uint
        -Wl,-u,_simple_callback_f_uint8_t
        -Wl,-u,_simple_callback_f_uint16_t
        -Wl,-u,_simple_callback_f_uint32_t
        -Wl,-u,_simple_callback_f_uint64_t
        -Wl,-u,_simple_callback_f_ushort
        -Wl,-u,_simple_callback_f_ulong
        -Wl,-u,_simple_callback_f_longlong
        -Wl,-u,_simple_callback_f_ulonglong
        -Wl,-u,_simple_callback_f_pointer
        -Wl,-u,_simple_callback_f_size_t
    )

    add_executable(test_class_table src/vm/test_class_table.cpp)
    target_link_libraries(test_class_table PharoVMCore)

    add_executable(test_platform src/platform/test_platform.cpp)
    target_link_libraries(test_platform PharoVMCore)

    add_executable(test_ios_bridge src/platform/test_ios_bridge.cpp)
    target_link_libraries(test_ios_bridge PharoVMCore)
endif()

message(STATUS "PharoVM Clean C++ configured")
message(STATUS "  Build tests with: cmake --build build -- test_load_image test_platform")
message(STATUS "  For Swift apps, use Xcode with the bridging header")
