# PharoVM - Clean C++ VM for macOS and iOS
cmake_minimum_required(VERSION 3.20)
project(PharoVM LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# VM Core Library
set(VM_SOURCES
    src/vm/ObjectMemory.cpp
    src/vm/Interpreter.cpp
    src/vm/Primitives.cpp
    src/platform/EventQueue.cpp
    src/platform/PlatformBridge.cpp
)

add_library(PharoVMCore STATIC ${VM_SOURCES})

target_include_directories(PharoVMCore PUBLIC
    ${CMAKE_SOURCE_DIR}/src/vm
    ${CMAKE_SOURCE_DIR}/src/platform
)

target_compile_options(PharoVMCore PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)

# Test executables
add_executable(test_load_image src/vm/test_load_image.cpp)
target_link_libraries(test_load_image PharoVMCore)

add_executable(test_platform src/platform/test_platform.cpp)
target_link_libraries(test_platform PharoVMCore)

message(STATUS "PharoVM Clean C++ configured")
message(STATUS "  Build tests with: make test_load_image test_platform")
message(STATUS "  For Swift apps, use Xcode with the bridging header")
