# PharoVM - Clean C++ VM for macOS, iOS, and Linux
cmake_minimum_required(VERSION 3.20)

# OBJC language is Apple-only.  On Linux we drop it from the LANGUAGES
# list since there's no Objective-C compiler on a typical Linux host.
if(APPLE OR CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    project(PharoVM LANGUAGES C CXX OBJC ASM)
else()
    project(PharoVM LANGUAGES C CXX ASM)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Crypto build option — controls DSAPrims, SqueakSSL, and bundled OpenSSL
option(PHARO_WITH_CRYPTO "Include cryptographic libraries (DSAPrims, SqueakSSL, OpenSSL)" ON)

# Detect platform for xcframework slice selection (Apple builds only)
# FORCE_XCFRAMEWORK_PLATFORM can be set externally (e.g., for Mac Catalyst builds
# where cmake configures for iOS but xcodebuild targets macabi)
if(APPLE)
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
endif()

message(STATUS "Building for platform: ${CMAKE_SYSTEM_NAME}")
message(STATUS "XCFramework slice: ${XCFRAMEWORK_PLATFORM}")
message(STATUS "Crypto support: ${PHARO_WITH_CRYPTO}")

# VM Core Library
set(VM_SOURCES
    src/vm/DebugSettings.cpp
    src/vm/ObjectMemory.cpp
    src/vm/ImageLoader.cpp
    src/vm/ImageWriter.cpp
    src/vm/Interpreter.cpp
    src/vm/Primitives.cpp
    src/vm/FFI.cpp
    src/vm/InterpreterProxy.cpp
    src/vm/plugins/SocketPlugin.cpp
    src/platform/EventQueue.cpp
    src/platform/MotionData.cpp
    src/vm/jit/CodeZone.cpp
    src/vm/jit/JITCompiler.cpp
    src/vm/jit/JITRuntime.cpp
    src/vm/jit/TrampolineAsm.S
    src/vm/jit/sista/SistaIR.cpp
    src/vm/jit/sista/SistaBuilder.cpp
    src/vm/jit/sista/SistaRuntime.cpp
)

# Sista IR lowering (asmjit-emitted machine code) is per-arch.  The arm64
# implementation is the original full lowerer; the x86_64 implementation
# is being filled in op by op (start with stub that defers everything to
# tier-1).  Selected by CMAKE_SYSTEM_PROCESSOR / CMAKE_OSX_ARCHITECTURES,
# matching the same pattern used elsewhere in this file.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64" OR CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
    list(APPEND VM_SOURCES src/vm/jit/sista/SistaLowering_arm64.cpp)
else()
    list(APPEND VM_SOURCES src/vm/jit/sista/SistaLowering_x86_64.cpp)
endif()

# Tier-2 asmjit-emitted machine code, same arch-split pattern as
# SistaLowering above.  The arm64 path is the original Tier2Compiler.cpp;
# the x86_64 sibling starts as a stub that bails every method to tier-1
# and gets filled in op by op.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64" OR CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
    list(APPEND VM_SOURCES src/vm/jit/Tier2Compiler_arm64.cpp)
else()
    list(APPEND VM_SOURCES src/vm/jit/Tier2Compiler_x86_64.cpp)
endif()

# Ensure the .S file is treated as a C-preprocessed assembly source so the
# #define constants inside resolve at build time.
set_source_files_properties(src/vm/jit/TrampolineAsm.S PROPERTIES
    LANGUAGE ASM
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

# Tier 2 JIT backend: asmjit (replaced MIR 2026-04-17).
# See docs/jit-toolkit-evaluation.md for rationale.
# Configure asmjit as a static library, embedded from the submodule.
set(ASMJIT_STATIC ON CACHE BOOL "Build asmjit as static library")
set(ASMJIT_TEST OFF CACHE BOOL "Skip asmjit's own test suite")

# Apply local patches to asmjit submodule (idempotent).
# See third_party/patches/ for the patch set + rationale.
file(GLOB _asmjit_patches "${CMAKE_SOURCE_DIR}/third_party/patches/asmjit-*.patch")
list(SORT _asmjit_patches)
foreach(_patch IN LISTS _asmjit_patches)
    # Already-applied? `git apply --reverse --check` exits 0 if so.
    execute_process(
        COMMAND git apply --reverse --check "${_patch}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/third_party/asmjit"
        RESULT_VARIABLE _already_applied
        OUTPUT_QUIET ERROR_QUIET
    )
    if(NOT _already_applied EQUAL 0)
        execute_process(
            COMMAND git apply "${_patch}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/third_party/asmjit"
            RESULT_VARIABLE _apply_result
        )
        if(NOT _apply_result EQUAL 0)
            message(FATAL_ERROR "Failed to apply ${_patch}")
        endif()
        get_filename_component(_patch_name "${_patch}" NAME)
        message(STATUS "Applied asmjit patch: ${_patch_name}")
    endif()
endforeach()

add_subdirectory(third_party/asmjit EXCLUDE_FROM_ALL)

# Per-platform implementation files.  Each function declared in
# src/platform/Platform.hpp has exactly ONE definition per target;
# CMake selects the right .cpp here.  See docs/platform-port-design.md.
if(APPLE)
    list(APPEND VM_SOURCES
        src/platform/apple_shared.cpp
        src/platform/PlatformBridge.cpp
        src/platform/iOSBridge.cpp
        src/vm/ObjCExceptionGuard.m
        # Apple-specific framework callers — depend on CoreText,
        # CoreGraphics, CoreAudio, CoreMIDI.  No Linux equivalents
        # yet (will need ALSA / freetype / cairo direct bindings).
        src/vm/WorldRenderer.cpp
        src/vm/plugins/SoundPlugin.cpp
        src/vm/plugins/MIDIPlugin.cpp
    )
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        list(APPEND VM_SOURCES src/platform/ios.cpp)
    else()
        list(APPEND VM_SOURCES src/platform/mac.cpp)
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND VM_SOURCES
        src/platform/linux.cpp
        # WorldRenderer needs CoreText/CoreGraphics for glyph rasterization;
        # the Linux stub satisfies the linker (Interpreter.cpp constructs a
        # WorldRenderer member) without pulling in Apple frameworks.  When
        # a real Linux renderer is added, replace this with a freetype/cairo
        # impl that lives next to the Apple impl (one TU per platform per
        # design doc).
        src/vm/WorldRenderer_linux_stub.cpp
        # SoundPlugin.cpp / MIDIPlugin.cpp already have non-Apple stubs
        # (`#else` branch returning failure).  Compiling them here keeps
        # Primitives.cpp linkable; future Linux audio/MIDI will replace
        # the stubs with ALSA/JACK + ALSA-MIDI.
        src/vm/plugins/SoundPlugin.cpp
        src/vm/plugins/MIDIPlugin.cpp
    )
endif()

# Per-arch helper for the JIT trampoline JM_SIZE sentinel.  Only one of
# these is linked per build, selected by CMAKE_SYSTEM_PROCESSOR — keeps
# arch divergence out of the OS-specific platform .cpp files.
#
# Only relevant when JIT is enabled (the helpers reference JITMethod).
# iOS builds set PHARO_JIT_ENABLED=0 so neither file is needed there.
# CMake doesn't know the value of PHARO_JIT_ENABLED yet (it's set on
# the target below), so use the same source-of-truth gate cmake does.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" OR (DEFINED PHARO_JIT_ENABLED AND PHARO_JIT_ENABLED))
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64" OR CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
        list(APPEND VM_SOURCES src/platform/jit_jmsize_arm64.cpp)
    else()
        list(APPEND VM_SOURCES src/platform/jit_jmsize_other.cpp)
    endif()
endif()

add_library(PharoVMCore STATIC ${VM_SOURCES})
target_link_libraries(PharoVMCore asmjit::asmjit)

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

# PHARO_ASM_TRAMPOLINE: use the hand-written ARM64 J2J trampoline instead of
# the C++ while-loop inside Interpreter::tryJITActivation. Default OFF so
# baseline behavior is preserved; flip ON for benchmarking and correctness
# tests (`-DPHARO_ASM_TRAMPOLINE=ON`).
option(PHARO_ASM_TRAMPOLINE "Use hand-written ARM64 J2J trampoline" ON)
if(PHARO_ASM_TRAMPOLINE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    target_compile_definitions(PharoVMCore PUBLIC PHARO_ASM_TRAMPOLINE=1)
    message(STATUS "J2J trampoline: ARM64 hand-written")
else()
    message(STATUS "J2J trampoline: C++ fallback")
endif()

# Pass PHARO_WITH_CRYPTO as a compile definition
if(PHARO_WITH_CRYPTO)
    target_compile_definitions(PharoVMCore PUBLIC PHARO_WITH_CRYPTO=1)
else()
    target_compile_definitions(PharoVMCore PUBLIC PHARO_WITH_CRYPTO=0)
endif()

# Include directories
target_include_directories(PharoVMCore PUBLIC
    ${CMAKE_SOURCE_DIR}/src/vm
    ${CMAKE_SOURCE_DIR}/src/vm/jit
    ${CMAKE_SOURCE_DIR}/src/vm/plugins
    ${CMAKE_SOURCE_DIR}/src/vm/plugins/jpeg
    ${CMAKE_SOURCE_DIR}/src/vm/plugins/testlib
    ${CMAKE_SOURCE_DIR}/src/platform
)
# Apple builds: SDL2 + libffi headers from prebuilt xcframeworks.
# Linux: use system pkg-config (added later in this file).
if(APPLE)
    target_include_directories(PharoVMCore PUBLIC
        ${CMAKE_SOURCE_DIR}/Frameworks/SDL2.xcframework/${XCFRAMEWORK_PLATFORM}/Headers
        ${CMAKE_SOURCE_DIR}/Frameworks/libffi.xcframework/${XCFRAMEWORK_PLATFORM}/Headers
    )
endif()

target_compile_options(PharoVMCore PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)

# LTO for JIT performance — enables cross-TU inlining of pushFrameForJIT
# into j2j_call (2ms improvement on fib(28): 19ms → 17ms).
# Disabled for xcframework builds: xcodebuild -create-xcframework rejects
# archives containing LLVM bitcode objects from -flto=thin.
#
# CMP0069 NEW is required for check_ipo_supported() to actually run on
# CMake 3.9+; without it the check errors out and IPO is silently
# skipped.  On Linux this caused tryResume() to NOT inline into
# tryJITResumeInCaller, making block(500K) 45x slower than it should
# be (1000ms vs 22ms on Mac).  See docs/linux-port-status.md.
cmake_policy(SET CMP0069 NEW)
if(NOT DEFINED PHARO_DISABLE_LTO OR NOT PHARO_DISABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
    if(ipo_supported)
        set_target_properties(PharoVMCore PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
        message(STATUS "LTO/IPO: enabled")
    else()
        message(STATUS "LTO/IPO: NOT supported (${ipo_error})")
    endif()
endif()

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

if(APPLE)
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
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Linux: SDL2 + libffi via pkg-config (system packages).
    # Headless builds don't actually need SDL2 — leave it optional.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBFFI REQUIRED libffi)
    target_include_directories(PharoVMCore PUBLIC ${LIBFFI_INCLUDE_DIRS})
    target_link_libraries(PharoVMCore ${LIBFFI_LIBRARIES})
    message(STATUS "Using libffi from pkg-config: ${LIBFFI_LIBRARIES}")
    pkg_check_modules(SDL2 sdl2)
    if(SDL2_FOUND)
        target_include_directories(PharoVMCore PUBLIC ${SDL2_INCLUDE_DIRS})
        target_link_libraries(PharoVMCore ${SDL2_LIBRARIES})
        message(STATUS "Using SDL2 from pkg-config: ${SDL2_LIBRARIES}")
    else()
        message(STATUS "SDL2 not found; building without GUI support")
    endif()
    # Linux requires explicit pthread + dl for dlopen / mutex / etc.
    target_link_libraries(PharoVMCore pthread dl m)
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

# Third-party xcframeworks (Apple builds only — Linux uses system pkgs)
if(APPLE)
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

    # Crypto xcframeworks (conditional, Apple-only for now)
    if(PHARO_WITH_CRYPTO)
        foreach(LIB_NAME ssl crypto ssh2 git2)
            set(LIB_XCF ${CMAKE_SOURCE_DIR}/Frameworks/lib${LIB_NAME}.xcframework/${XCFRAMEWORK_PLATFORM}/lib${LIB_NAME}.a)
            if(EXISTS ${LIB_XCF})
                target_link_libraries(PharoVMCore ${LIB_XCF})
                message(STATUS "Using lib${LIB_NAME}: ${LIB_XCF}")
            endif()
        endforeach()
    endif()
endif()

# Test executables (macOS only)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_executable(test_load_image src/vm/test_load_image.cpp)
    target_link_libraries(test_load_image PharoVMCore)
    # Force export testlib symbols so they're available via dlsym.
    # The `-Wl,-u,SYM` syntax is Mach-O (Apple ld); Linux ld uses
    # `--undefined=SYM` (also accepts -u SYM).  Apple-only for now —
    # Linux test runs that don't exercise FFI callbacks won't notice.
    if(APPLE)
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
    endif()  # APPLE for test_load_image link options

    if(APPLE)
        # Re-sign the test_load_image binary with debug + JIT
        # entitlements after link.  Without get-task-allow, lldb
        # cannot attach (deferred.md A1 P0 investigation needed
        # this).  Re-signing also preserves the JIT entitlements
        # the binary needs at runtime.
        add_custom_command(TARGET test_load_image POST_BUILD
            COMMAND codesign --force --sign - --entitlements
                ${CMAKE_SOURCE_DIR}/scripts/test_load_image-entitlements.plist
                $<TARGET_FILE:test_load_image>
            VERBATIM
            COMMENT "Re-signing test_load_image with debug + JIT entitlements"
        )
    endif()

    add_executable(test_class_table src/vm/test_class_table.cpp)
    target_link_libraries(test_class_table PharoVMCore)

    if(APPLE)
        add_executable(test_platform src/platform/test_platform.cpp)
        target_link_libraries(test_platform PharoVMCore)

        add_executable(test_ios_bridge src/platform/test_ios_bridge.cpp)
        target_link_libraries(test_ios_bridge PharoVMCore)
    endif()

    # Sista IR self-test (Phase 2 of docs/sista-inlining-plan.md).
    # Keeps the IR compile-able while the rest of the optimizer is
    # under construction.  Linked against PharoVMCore so Oop.hpp and
    # friends resolve, but doesn't use the runtime.
    add_executable(test_sista_ir src/vm/jit/sista/test_sista_ir.cpp)
    target_link_libraries(test_sista_ir PharoVMCore asmjit::asmjit)

    # Sista coverage survey — loads an image and runs the lifter against
    # every CompiledMethod, reporting success rate + top blocking ops.
    add_executable(test_sista_survey src/vm/jit/sista/test_sista_survey.cpp)
    target_link_libraries(test_sista_survey PharoVMCore asmjit::asmjit)

    # asmjit-T1 Phase 0 stub: standalone proof that asmjit can emit a
    # void(JITState*) function whose write of state.exitReason is
    # observable.  Foundation for replacing the stencil JIT per
    # scripts/jit-diff/plan_asmjit_replacement.md.  No PharoVMCore
    # link — runs hermetic with a mock state struct.
    add_executable(test_asmjit_t1_stub src/vm/jit/asmjit/test_asmjit_t1_stub.cpp)
    target_link_libraries(test_asmjit_t1_stub asmjit::asmjit)
endif()

message(STATUS "PharoVM Clean C++ configured")
message(STATUS "  Build tests with: cmake --build build -- test_load_image test_platform")
message(STATUS "  For Swift apps, use Xcode with the bridging header")
