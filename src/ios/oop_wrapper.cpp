/*
 * oop_wrapper.cpp - C++ Oop wrapper functions callable from C
 *
 * This provides C-callable functions that use the Oop class for
 * space-aware memory operations. The C interpreter calls these
 * instead of using address-range-based space detection.
 */

#include "oop_wrapper.h"

// Include oop.hpp from pharo-vm-ios
#if __has_include("../../../pharo-vm-ios/src/ios/oop.hpp")
#include "../../../pharo-vm-ios/src/ios/oop.hpp"
#else
#include "oop.hpp"
#endif

using namespace pharo;

extern "C" {

// Space encoding during swizzle
uint64_t oop_encode_with_space(uint64_t rawAddress, int space) {
    Oop oop = Oop::fromBits(rawAddress);
    return oop.withSpace(static_cast<Space>(space)).bits();
}

// Space checking - returns 1 for true, 0 for false
int oop_is_young(uint64_t bits) {
    return Oop::fromBits(bits).isYoung() ? 1 : 0;
}

int oop_is_old(uint64_t bits) {
    return Oop::fromBits(bits).isOld() ? 1 : 0;
}

int oop_is_permanent(uint64_t bits) {
    return Oop::fromBits(bits).isPermanent() ? 1 : 0;
}

int oop_is_immediate(uint64_t bits) {
    return Oop::fromBits(bits).isImmediate() ? 1 : 0;
}

int oop_is_non_immediate(uint64_t bits) {
    return Oop::fromBits(bits).isNonImmediate() ? 1 : 0;
}

// Integer operations
int oop_is_small_integer(uint64_t bits) {
    return Oop::fromBits(bits).isSmallInteger() ? 1 : 0;
}

int64_t oop_integer_value_of(uint64_t bits) {
    return Oop::fromBits(bits).integerValueOf();
}

uint64_t oop_integer_object_of(int64_t value) {
    return Oop::integerObjectOf(value).bits();
}

// Get raw pointer from oop (clears space bits)
void* oop_to_pointer(uint64_t bits) {
    return Oop::fromBits(bits).rawPointer();
}

// Get space as int
int oop_get_space(uint64_t bits) {
    return static_cast<int>(Oop::fromBits(bits).space());
}

} // extern "C"
