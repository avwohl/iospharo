/*
 * cointerp.cpp - C++ wrapper for the Pharo VM interpreter
 *
 * This file compiles the C interpreter as C++ to enable full
 * type-safe Oop class from oop.hpp.
 *
 * The Oop class provides compile-time safety:
 * - No arithmetic on Oop (prevents tag bit corruption)
 * - Explicit conversion to RawAddress for pointer math
 * - Space encoding in low bits for iOS ASLR compatibility
 */

// Include the type-safe Oop class
#include "oop.hpp"

// The generated C interpreter is compiled as C++
// All C functions will have C linkage for compatibility
extern "C" {

// Include the C++-compatible C interpreter (transformed from cointerp.c)
#include "cointerp-cpp.c"

} // extern "C"
