/*
 * Oop.hpp - Type-safe Object-Oriented Pointer for Pharo VM
 *
 * Copyright (c) 2025-2026 Aaron Wohl. Licensed under the MIT License.
 *
 * This class provides a type-safe wrapper for Smalltalk object pointers
 * that is compatible with iOS ASLR (Address Space Layout Randomization).
 * The Spur tagging scheme is defined by the Pharo project (https://pharo.org);
 * this implementation uses low bits instead of high bits for iOS compatibility.
 * See THIRD_PARTY_LICENSES for upstream license details.
 *
 * TAGGING SCHEME (uses LOW bits only - iOS compatible):
 *
 * Bit 0 = 1: IMMEDIATE VALUE (no heap allocation)
 *   Bits 2-1 = 00: SmallInteger (tag 001)
 *     - Bits 63-3: 61-bit signed integer value
 *     - Range: -2^60 to 2^60-1
 *
 *   Bits 2-1 = 01: Character (tag 011)
 *     - Bits 32-3: 30-bit Unicode codepoint
 *     - Supports full Unicode range (0 to 0x3FFFFFFF)
 *
 *   Bits 2-1 = 10: SmallFloat (tag 101)
 *     - Bits 63-3: 61-bit rotated double representation
 *     - Covers most common floating point values
 *
 * Bit 0 = 0: OBJECT POINTER (heap-allocated object)
 *   Raw 8-byte aligned heap address (no space encoding in pointer bits).
 *   Memory space (old/new/perm) is determined by address range checks
 *   in ObjectMemory, not by tag bits.
 *
 * DESIGN PRINCIPLES:
 * 1. No implicit conversions to/from integers
 * 2. No arithmetic operators (prevents accidental pointer math)
 * 3. All operations are explicit and type-safe
 * 4. Space encoding in low bits works with iOS ASLR
 */

#ifndef PHARO_OOP_HPP
#define PHARO_OOP_HPP

#include <cstdint>
#include <cassert>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace pharo {

// Forward declarations
class ObjectHeader;

// Memory spaces
enum class Space : uint8_t {
    Old = 0,       // 00 - Tenured objects
    New = 1,       // 01 - Young objects (eden, survivors)
    Perm = 2,      // 10 - Permanent objects (never collected)
    Reserved = 3   // 11 - Reserved for future use
};

class Oop {
private:
    uint64_t bits_;

    // Tag constants
    static constexpr uint64_t TagMask = 0x7;          // Low 3 bits
    static constexpr uint64_t ImmediateBit = 0x1;     // Bit 0
    // Immediate tags (bit 0 = 1)
    // Spur 64-bit immediate tags:
    // Tag 0 (000) = object pointer
    // Tag 1 (001) = SmallInteger
    // Tag 2 (010) = Character (some sources say 011, but image uses 010)
    // Tag 4 (100) = SmallFloat (Clément Béra's blog)
    // Tag 5 (101) = SmallFloat (what the image actually uses)
    static constexpr uint64_t SmallIntegerTag = 0x1;  // 001
    static constexpr uint64_t CharacterTag = 0x3;     // 011 - Pharo 9+ uses tag 3 for Characters
    static constexpr uint64_t SmallFloatTag = 0x5;    // 101 - what the Pharo image uses

    // SmallInteger limits (61-bit signed)
    static constexpr int64_t SmallIntegerMin = -(1LL << 60);
    static constexpr int64_t SmallIntegerMax = (1LL << 60) - 1;

    // The actual nil Oop bits after image relocation.
    // Set once by ObjectMemory::initialize() after loading the image.
    // Before that, remains 0 which is correct for pre-relocation nil.
    static inline uint64_t s_nilBits = 0;

    // Character max (30-bit codepoint, matches Pharo's (2**30)-1)
    static constexpr uint32_t CharacterMax = 0x3FFFFFFF;

    // Private constructor from raw bits
    explicit constexpr Oop(uint64_t bits) : bits_(bits) {}

public:
    // Default constructor - creates nil-like zero value
    constexpr Oop() : bits_(0) {}

    // ===== TYPE PREDICATES =====

    /// Is this an immediate value (SmallInteger, Character, or SmallFloat)?
    /// In Spur 64-bit, tags 1, 2, and 5 are immediates; tag 0 is object pointer
    bool isImmediate() const {
        uint64_t tag = bits_ & TagMask;
        return tag == SmallIntegerTag || tag == CharacterTag || tag == SmallFloatTag;
    }

    /// Is this a heap-allocated object pointer?
    /// Tag 0 = object pointer (and non-nil)
    bool isObject() const { return (bits_ & TagMask) == 0 && bits_ != 0; }

    /// Is this a SmallInteger immediate?
    bool isSmallInteger() const { return (bits_ & TagMask) == SmallIntegerTag; }

    /// Is this a Character immediate?
    bool isCharacter() const { return (bits_ & TagMask) == CharacterTag; }

    /// Is this a SmallFloat immediate?
    bool isSmallFloat() const { return (bits_ & TagMask) == SmallFloatTag; }

    /// Is this nil? Compares against the actual nil Oop (which has a real
    /// heap address after image relocation, not necessarily 0).
    bool isNil() const { return bits_ == s_nilBits; }

    // ===== IMMEDIATE VALUE EXTRACTION =====

    /// Extract SmallInteger value. Caller must verify isSmallInteger() first.
    int64_t asSmallInteger() const {
        if (__builtin_expect(!isSmallInteger(), 0)) {
            fprintf(stderr, "[FATAL] asSmallInteger on non-SmallInt: bits=0x%llx tag=%d\n",
                    (unsigned long long)bits_, (int)(bits_ & 7));
            abort();
        }
        return static_cast<int64_t>(bits_) >> 3;
    }

    /// Extract Character codepoint. Caller must verify isCharacter() first.
    uint32_t asCharacter() const {
        assert(isCharacter());
        return static_cast<uint32_t>((bits_ >> 3) & 0x3FFFFFFF);
    }

    /// Extract SmallFloat value. Caller must verify isSmallFloat() first.
    double asSmallFloat() const {
        assert(isSmallFloat());
        // Spur SmallFloat64 encoding (from Clément Béra's blog):
        // https://clementbera.wordpress.com/2018/11/09/64-bits-immediate-floats/
        //
        // Encoding moves sign bit to LSB (rotate left 1), subtracts exponent offset,
        // then shifts left 3 and adds tag.
        //
        // Decoding reverses this:
        // 1. Right-shift by 3 to remove tag
        // 2. Check for ±0 (shifted <= 1)
        // 3. Add exponent offset 0x7000000000000000 (this is 896 << 53, in rotated position)
        // 4. Rotate right by 1 to restore sign bit to MSB

        uint64_t shifted = bits_ >> 3;  // Remove tag

        // Handle ±0 special cases: after removing tag, 0 means +0, 1 means -0
        if (shifted <= 1) {
            if (shifted == 0) return 0.0;
            return -0.0;
        }

        // Add exponent offset (in rotated position where sign is at LSB)
        uint64_t withOffset = shifted + 0x7000000000000000ULL;

        // Rotate right by 1 to restore sign bit to position 63
        uint64_t doubleBits = (withOffset >> 1) | (withOffset << 63);

        double result;
        std::memcpy(&result, &doubleBits, sizeof(double));
        return result;
    }

    // ===== OBJECT POINTER ACCESS =====

    /// Get pointer to object header. Caller must verify isObject() first.
    ObjectHeader* asObjectPtr() const {
        assert(isObject());
        // Object pointers in Spur 64-bit have tag 0, so no bits to clear
        return reinterpret_cast<ObjectHeader*>(bits_);
    }

    /// Get raw address value for debugging/hashing
    uint64_t rawBits() const { return bits_; }

    // ===== IMMEDIATE VALUE CONSTRUCTORS =====

    /// Create a SmallInteger Oop from an integer value.
    /// Returns false if value is out of range.
    static bool tryFromSmallInteger(int64_t value, Oop& result) {
        if (value < SmallIntegerMin || value > SmallIntegerMax) {
            return false;
        }
        result = Oop((static_cast<uint64_t>(value) << 3) | SmallIntegerTag);
        return true;
    }

    /// Create a SmallInteger Oop. Asserts if out of range.
    static Oop fromSmallInteger(int64_t value) {
        assert(value >= SmallIntegerMin && value <= SmallIntegerMax);
        return Oop((static_cast<uint64_t>(value) << 3) | SmallIntegerTag);
    }

    /// Create a Character Oop from a Unicode codepoint.
    static Oop fromCharacter(uint32_t codepoint) {
        assert(codepoint <= CharacterMax);
        return Oop((static_cast<uint64_t>(codepoint) << 3) | CharacterTag);
    }

    /// Create a SmallFloat Oop from a double.
    /// Returns false if the value cannot be represented as SmallFloat.
    static bool tryFromSmallFloat(double value, Oop& result) {
        // Check for values that don't fit in SmallFloat encoding
        if (std::isnan(value) || std::isinf(value)) {
            return false;
        }

        uint64_t doubleBits;
        std::memcpy(&doubleBits, &value, sizeof(double));

        // Handle ±0 special cases
        if (doubleBits == 0) {
            result = Oop(SmallFloatTag);  // +0.0
            return true;
        }
        if (doubleBits == 0x8000000000000000ULL) {
            result = Oop(SmallFloatTag | 8);  // -0.0 (tag + bit 3 set)
            return true;
        }

        // Spur SmallFloat64 encoding:
        // 1. Rotate left by 1 (move sign bit to LSB)
        uint64_t rotated = (doubleBits << 1) | (doubleBits >> 63);

        // 2. Subtract exponent offset - check for underflow (value out of range)
        if (rotated < 0x7000000000000000ULL) {
            return false;  // Exponent too small for SmallFloat
        }
        uint64_t adjusted = rotated - 0x7000000000000000ULL;

        // 3. Check that the adjusted value fits in 61 bits (after shifting left 3)
        if (adjusted > 0x1FFFFFFFFFFFFFFFULL) {
            return false;  // Exponent too large for SmallFloat
        }

        // 4. Shift left by 3 and add tag
        result = Oop((adjusted << 3) | SmallFloatTag);

        // Verify roundtrip
        if (result.asSmallFloat() != value) {
            return false;
        }
        return true;
    }

    // ===== OBJECT POINTER CONSTRUCTOR =====

    /// Create an Oop from an object pointer.
    /// In Spur 64-bit, object pointers have tag 0 (no bits set in low 3).
    /// Space is determined by address range, not by tag bits.
    static Oop fromObject(ObjectHeader* obj, Space /*space*/ = Space::Old) {
        if (obj == nullptr) {
            return nil();  // actual nil Oop
        }
        uint64_t addr = reinterpret_cast<uint64_t>(obj);
        assert((addr & TagMask) == 0 && "Object must be 8-byte aligned");
        // Object pointers in Spur have tag 0 - just use the raw address
        return Oop(addr);
    }

    // ===== SPECIAL VALUES =====

    /// Create Oop from raw bits (for interpreter proxy / plugin interface)
    static Oop fromRawBits(uint64_t bits) { return Oop(bits); }

    /// Create nil Oop (uses actual relocated nil address)
    static Oop nil() { return Oop(s_nilBits); }

    /// Set the actual nil bits after image relocation.
    /// Must be called once during ObjectMemory initialization.
    static void setNilBits(uint64_t bits) { s_nilBits = bits; }

    /// Get the current nil bits value.
    static uint64_t getNilBits() { return s_nilBits; }

    // ===== COMPARISON =====

    bool operator==(Oop other) const { return bits_ == other.bits_; }
    bool operator!=(Oop other) const { return bits_ != other.bits_; }

    // For use in ordered containers (arbitrary but consistent ordering)
    bool operator<(Oop other) const { return bits_ < other.bits_; }

    // ===== HASHING =====

    /// Hash value for use in hash tables
    std::size_t hash() const {
        // Simple but effective hash
        return static_cast<std::size_t>(bits_ * 0x9E3779B97F4A7C15ULL);
    }

    // ===== RANGE CHECKS =====

    /// Check if a value can be represented as SmallInteger
    static bool canBeSmallInteger(int64_t value) {
        return value >= SmallIntegerMin && value <= SmallIntegerMax;
    }

    /// Get the minimum SmallInteger value
    static constexpr int64_t smallIntegerMin() { return SmallIntegerMin; }

    /// Get the maximum SmallInteger value
    static constexpr int64_t smallIntegerMax() { return SmallIntegerMax; }
};

// Ensure Oop is exactly 64 bits and trivially copyable
static_assert(sizeof(Oop) == 8, "Oop must be 64 bits");
static_assert(std::is_trivially_copyable_v<Oop>, "Oop must be trivially copyable");

} // namespace pharo

// Hash function for std::unordered_map/set
namespace std {
    template<>
    struct hash<pharo::Oop> {
        std::size_t operator()(const pharo::Oop& oop) const {
            return oop.hash();
        }
    };
}

#endif // PHARO_OOP_HPP
