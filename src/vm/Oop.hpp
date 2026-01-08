/*
 * Oop.hpp - Type-safe Object-Oriented Pointer for Pharo VM
 *
 * This class provides a type-safe wrapper for Smalltalk object pointers
 * that is compatible with iOS ASLR (Address Space Layout Randomization).
 *
 * TAGGING SCHEME (uses LOW bits only - iOS compatible):
 *
 * Bit 0 = 1: IMMEDIATE VALUE (no heap allocation)
 *   Bits 2-1 = 00: SmallInteger (tag 001)
 *     - Bits 63-3: 61-bit signed integer value
 *     - Range: -2^60 to 2^60-1
 *
 *   Bits 2-1 = 01: Character (tag 011)
 *     - Bits 31-3: 29-bit Unicode codepoint
 *     - Supports full Unicode range (0 to 0x10FFFF)
 *
 *   Bits 2-1 = 10: SmallFloat (tag 101)
 *     - Bits 63-3: 61-bit rotated double representation
 *     - Covers most common floating point values
 *
 * Bit 0 = 0: OBJECT POINTER (heap-allocated object)
 *   Bits 2-1: Memory space encoding
 *     00 = Old space (tenured objects)
 *     01 = New space (young objects)
 *     10 = Permanent space (system objects)
 *     11 = Reserved
 *   Bits 63-3: Object address (8-byte aligned)
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
    static constexpr uint64_t SpaceMask = 0x6;        // Bits 2-1
    static constexpr uint64_t SpaceShift = 1;

    // Immediate tags (bit 0 = 1)
    static constexpr uint64_t SmallIntegerTag = 0x1;  // 001
    static constexpr uint64_t CharacterTag = 0x3;     // 011
    static constexpr uint64_t SmallFloatTag = 0x5;    // 101

    // SmallInteger limits (61-bit signed)
    static constexpr int64_t SmallIntegerMin = -(1LL << 60);
    static constexpr int64_t SmallIntegerMax = (1LL << 60) - 1;

    // Character max (29-bit codepoint)
    static constexpr uint32_t CharacterMax = 0x1FFFFFFF;

    // Private constructor from raw bits
    explicit constexpr Oop(uint64_t bits) : bits_(bits) {}

public:
    // Default constructor - creates nil-like zero value
    constexpr Oop() : bits_(0) {}

    // ===== TYPE PREDICATES =====

    /// Is this an immediate value (SmallInteger, Character, or SmallFloat)?
    bool isImmediate() const { return bits_ & ImmediateBit; }

    /// Is this a heap-allocated object pointer?
    bool isObject() const { return !isImmediate() && bits_ != 0; }

    /// Is this a SmallInteger immediate?
    bool isSmallInteger() const { return (bits_ & TagMask) == SmallIntegerTag; }

    /// Is this a Character immediate?
    bool isCharacter() const { return (bits_ & TagMask) == CharacterTag; }

    /// Is this a SmallFloat immediate?
    bool isSmallFloat() const { return (bits_ & TagMask) == SmallFloatTag; }

    /// Is this nil (zero pointer)?
    bool isNil() const { return bits_ == 0; }

    // ===== IMMEDIATE VALUE EXTRACTION =====

    /// Extract SmallInteger value. Caller must verify isSmallInteger() first.
    int64_t asSmallInteger() const {
        assert(isSmallInteger());
        // Mask off bit 63 which may be used as a GC/marking flag in the image
        // Then arithmetic right shift to extract the 60-bit signed value
        uint64_t masked = bits_ & 0x7FFFFFFFFFFFFFFFULL;
        return static_cast<int64_t>(masked) >> 3;
    }

    /// Extract Character codepoint. Caller must verify isCharacter() first.
    uint32_t asCharacter() const {
        assert(isCharacter());
        return static_cast<uint32_t>((bits_ >> 3) & 0x1FFFFFFF);
    }

    /// Extract SmallFloat value. Caller must verify isSmallFloat() first.
    double asSmallFloat() const {
        assert(isSmallFloat());
        // Rotate the stored bits back to get the double
        uint64_t rotated = bits_ >> 3;
        // Undo the rotation (rotate right by 3 to restore)
        uint64_t doubleBits = (rotated >> 61) | (rotated << 3);
        double result;
        std::memcpy(&result, &doubleBits, sizeof(double));
        return result;
    }

    // ===== OBJECT POINTER ACCESS =====

    /// Get the memory space of this object. Caller must verify isObject() first.
    Space space() const {
        assert(isObject());
        return static_cast<Space>((bits_ & SpaceMask) >> SpaceShift);
    }

    /// Get pointer to object header. Caller must verify isObject() first.
    ObjectHeader* asObjectPtr() const {
        assert(isObject());
        // Clear the low 3 bits to get aligned address
        return reinterpret_cast<ObjectHeader*>(bits_ & ~TagMask);
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

        // Rotate left by 3 to make room for tag
        uint64_t rotated = (doubleBits << 61) | (doubleBits >> 3);

        // Check that we can recover the original (no info lost in low bits)
        uint64_t recovered = (rotated >> 61) | (rotated << 3);
        if (recovered != doubleBits) {
            return false;
        }

        result = Oop((rotated << 3) | SmallFloatTag);
        return true;
    }

    // ===== OBJECT POINTER CONSTRUCTOR =====

    /// Create an Oop from an object pointer and its memory space.
    static Oop fromObject(ObjectHeader* obj, Space space) {
        if (obj == nullptr) {
            return Oop(0);  // nil
        }
        uint64_t addr = reinterpret_cast<uint64_t>(obj);
        assert((addr & TagMask) == 0 && "Object must be 8-byte aligned");
        return Oop(addr | (static_cast<uint64_t>(space) << SpaceShift));
    }

    // ===== SPECIAL VALUES =====

    /// Create nil Oop
    static constexpr Oop nil() { return Oop(0); }

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

    // ===== SPACE MANAGEMENT =====

    /// Create a new Oop with the same pointer but different space encoding.
    /// Used during GC when promoting objects between spaces.
    Oop withSpace(Space newSpace) const {
        assert(isObject());
        uint64_t addr = bits_ & ~SpaceMask;
        return Oop(addr | (static_cast<uint64_t>(newSpace) << SpaceShift));
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
