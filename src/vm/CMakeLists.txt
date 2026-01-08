# Clean C++ VM for iOS Pharo
# This builds the new type-safe VM implementation

cmake_minimum_required(VERSION 3.20)
project(PharoVMClean LANGUAGES CXX)

set(VM_SOURCES
    Oop.hpp
    ObjectHeader.hpp
    ObjectMemory.hpp
    ObjectMemory.cpp
    ImageLoader.hpp
    ImageLoader.cpp
    Interpreter.hpp
    Interpreter.cpp
    Primitives.cpp
)

add_library(PharoVMClean STATIC ${VM_SOURCES})

target_include_directories(PharoVMClean PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(PharoVMClean PUBLIC cxx_std_17)

set_target_properties(PharoVMClean PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

# Warnings
target_compile_options(PharoVMClean PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
)

# Test executable
add_executable(test_load_image test_load_image.cpp)
target_link_libraries(test_load_image PharoVMClean)
