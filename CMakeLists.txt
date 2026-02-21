# PharoVM - Clean C++ VM for macOS and iOS
cmake_minimum_required(VERSION 3.20)
project(PharoVM LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

# VM Core Library
set(VM_SOURCES
    src/vm/ObjectMemory.cpp
    src/vm/ImageLoader.cpp
    src/vm/Interpreter.cpp
    src/vm/Primitives.cpp
    src/vm/FFI.cpp
    src/vm/InterpreterProxy.cpp
    src/platform/EventQueue.cpp
    src/platform/PlatformBridge.cpp
    src/platform/iOSBridge.cpp
)

# B2DPlugin (BalloonEngine) - compiled as C, not C++
set(B2D_SOURCES src/vm/plugins/B2DPlugin.c)
set_source_files_properties(${B2D_SOURCES} PROPERTIES LANGUAGE C)
list(APPEND VM_SOURCES ${B2D_SOURCES})

add_library(PharoVMCore STATIC ${VM_SOURCES})

# Include directories - use platform-appropriate xcframework slice
target_include_directories(PharoVMCore PUBLIC
    ${CMAKE_SOURCE_DIR}/src/vm
    ${CMAKE_SOURCE_DIR}/src/vm/plugins
    ${CMAKE_SOURCE_DIR}/src/platform
    ${CMAKE_SOURCE_DIR}/SDL2.xcframework/${XCFRAMEWORK_PLATFORM}/Headers
    ${CMAKE_SOURCE_DIR}/libffi.xcframework/${XCFRAMEWORK_PLATFORM}/Headers
)

target_compile_options(PharoVMCore PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)

# Link SDL2 static library with force_load to ensure all symbols are available for dlsym
set(SDL2_LIBRARY ${CMAKE_SOURCE_DIR}/SDL2.xcframework/${XCFRAMEWORK_PLATFORM}/libSDL2.a)
if(EXISTS ${SDL2_LIBRARY})
    # Force load all symbols from SDL2 so they're available via dlsym at runtime
    target_link_libraries(PharoVMCore "-Wl,-force_load,${SDL2_LIBRARY}")
    message(STATUS "Using SDL2 (force_load): ${SDL2_LIBRARY}")
endif()

# Link libffi static library
set(LIBFFI_LIBRARY ${CMAKE_SOURCE_DIR}/libffi.xcframework/${XCFRAMEWORK_PLATFORM}/libffi.a)
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
        )
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
        )
    endif()
endif()

# Test executables (macOS only)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_executable(test_load_image src/vm/test_load_image.cpp)
    target_link_libraries(test_load_image PharoVMCore)

    add_executable(test_platform src/platform/test_platform.cpp)
    target_link_libraries(test_platform PharoVMCore)

    add_executable(test_ios_bridge src/platform/test_ios_bridge.cpp)
    target_link_libraries(test_ios_bridge PharoVMCore)
endif()

message(STATUS "PharoVM Clean C++ configured")
message(STATUS "  Build tests with: make test_load_image test_platform")
message(STATUS "  For Swift apps, use Xcode with the bridging header")
