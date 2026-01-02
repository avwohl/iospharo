# iOS Platform Configuration for Pharo VM
# Based on Darwin.cmake pattern

set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum iOS deployment target")

# Get iOS SDK path
execute_process(
    COMMAND xcrun --sdk iphoneos --show-sdk-path
    OUTPUT_VARIABLE IOS_SDK_PATH
    OUTPUT_STRIP_TRAILING_WHITESPACE)

# Platform headers - reuse macOS/Unix headers where applicable
function(add_platform_headers)
    target_include_directories(${VM_LIBRARY_NAME}
        PUBLIC
            ${PHARO_VM_DIR}/extracted/vm/include/osx
            ${PHARO_VM_DIR}/extracted/vm/include/unix
            ${PHARO_VM_DIR}/extracted/vm/include/common
            ${CMAKE_CURRENT_SOURCE_DIR}/src/include/iospharo
    )
endfunction()

# Source files for iOS build
set(EXTRACTED_SOURCES
    # Common VM sources
    ${PHARO_VM_DIR}/extracted/vm/src/common/sqHeapMap.c
    ${PHARO_VM_DIR}/extracted/vm/src/common/sqVirtualMachine.c
    ${PHARO_VM_DIR}/extracted/vm/src/common/sqNamedPrims.c
    ${PHARO_VM_DIR}/extracted/vm/src/common/sqExternalSemaphores.c
    ${PHARO_VM_DIR}/extracted/vm/src/common/sqTicker.c

    # Platform sources - reuse macOS kqueue-based AIO
    ${PHARO_VM_DIR}/extracted/vm/src/osx/aioOSX.c

    # iOS-specific sources
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ios/iosDisplay.m
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ios/iosEvents.m
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ios/iosParameters.m
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ios/iosUtils.mm

    # Virtual Memory functions - reuse Unix implementation
    ${PHARO_VM_DIR}/src/memoryUnix.c

    # Debug support
    ${PHARO_VM_DIR}/src/debugUnix.c
)

# No frontend sources for iOS - Swift app provides entry point
set(VM_FRONTEND_SOURCES "")

# Third-party dependencies - disable desktop-specific features
macro(add_third_party_dependencies_per_platform)
    # Disable SDL2 for iOS - using Metal instead
    set(FEATURE_LIB_SDL2 OFF CACHE BOOL "" FORCE)

    # Disable file dialogs - iOS has different file access model
    set(FEATURE_FILE_DIALOG OFF CACHE BOOL "" FORCE)

    # Keep FFI if needed
    if(${FEATURE_LIB_FFI})
        include(${PHARO_VM_DIR}/cmake/importLibFFI.cmake)
    endif()
endmacro()

# iOS-specific library linking
macro(add_required_libs_per_platform)
    # iOS is always ARM64, requires read-only code zone handling
    target_compile_definitions(${VM_LIBRARY_NAME} PRIVATE READ_ONLY_CODE_ZONE=1)

    # iOS-specific compile definitions
    target_compile_definitions(${VM_LIBRARY_NAME} PRIVATE
        IOS=1
        TARGET_OS_IPHONE=1
    )

    # Link iOS frameworks
    target_link_libraries(${VM_LIBRARY_NAME}
        "-framework UIKit"
        "-framework CoreGraphics"
        "-framework Metal"
        "-framework MetalKit"
        "-framework Foundation"
        "-framework CoreFoundation"
        "-framework Security"
        "-framework QuartzCore"
    )
endmacro()

# Installation configuration for iOS
macro(configure_installables INSTALL_COMPONENT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/build/dist")

    # Install static library for embedding
    install(
        TARGETS ${VM_LIBRARY_NAME}
        ARCHIVE DESTINATION lib
        COMPONENT ${INSTALL_COMPONENT})

    # Install headers
    install(
        DIRECTORY "${PHARO_VM_DIR}/include/pharovm/"
        DESTINATION include/pharovm
        COMPONENT include
        FILES_MATCHING PATTERN "*.h")

    install(
        DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src/include/iospharo/"
        DESTINATION include/iospharo
        COMPONENT include
        FILES_MATCHING PATTERN "*.h")
endmacro()
