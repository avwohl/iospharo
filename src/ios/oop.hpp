/*
 * oop.hpp - Oop wrapper class for iOS Pharo VM
 *
 * This class wraps 64-bit object pointers (oops) and encodes the memory
 * space (new/old/perm) explicitly in the low bits instead of relying on
 * address ranges. This makes the VM compatible with iOS ASLR.
 *
 * Bit layout:
 *   Bit 0:     isImmediate flag (1 = SmallInteger/Character, 0 = object pointer)
 *   Bits 1-2:  Space encoding (00=new, 01=old, 10=perm, 11=reserved)
 *              Only meaningful when bit 0 = 0
 *   Bits 3-63: Payload
 *              - For immediates: the immediate value (e.g., SmallInteger)
 *              - For pointers: actual memory address (low 3 bits always 0 due to alignment)
 *
 * This matches Spur's existing 3-bit tag scheme but repurposes bits 1-2
 * for explicit space encoding instead of deriving space from address ranges.
 */

#pragma once
#include <cstdint>
#include <cassert>

#ifdef __cplusplus

namespace pharo {

// Memory space identifiers
enum class Space : uint8_t {
    New  = 0,   // Eden/survivor (young generation)
    Old  = 1,   // Tenured objects
    Perm = 2,   // Permanent space (read-only after boot)
    Code = 3    // Reserved for future use
};

class Oop {
private:
    uint64_t bits_;

    // Tag bit masks
    static constexpr uint64_t kImmediateTag   = 0x1ULL;
    static constexpr uint64_t kSpaceMask      = 0x6ULL;   // bits 1-2
    static constexpr uint64_t kSpaceShift     = 1;
    static constexpr uint64_t kTagMask        = 0x7ULL;   // bits 0-2
    static constexpr uint64_t kPointerMask    = ~kTagMask;

    // Spur immediate tags (when bit 0 = 1)
    // Bits 0-2 encode the immediate type:
    //   xx1 = SmallInteger (bits 1-2 encode sign/magnitude info)
    //   010 = Character
    //   110 = SmallFloat (on 64-bit)
    static constexpr uint64_t kSmallIntTag    = 0x1ULL;
    static constexpr uint64_t kCharacterTag   = 0x2ULL;
    static constexpr uint64_t kSmallFloatTag  = 0x6ULL;

public:
    // Default constructor - creates nil (0)
    constexpr Oop() : bits_(0) {}

    // Explicit construction from raw bits
    explicit constexpr Oop(uint64_t raw) : bits_(raw) {}

    // Copy/move
    Oop(const Oop&) = default;
    Oop(Oop&&) = default;
    Oop& operator=(const Oop&) = default;
    Oop& operator=(Oop&&) = default;

    // Raw bit access
    uint64_t bits() const { return bits_; }
    static Oop fromBits(uint64_t b) { return Oop(b); }

    // ========== Immediate Tests ==========

    bool isImmediate() const {
        return (bits_ & kImmediateTag) != 0;
    }

    bool isNonImmediate() const {
        return (bits_ & kImmediateTag) == 0;
    }

    bool isSmallInteger() const {
        // SmallIntegers have bit 0 set
        return (bits_ & kImmediateTag) != 0;
        // Note: More precise check would be (bits_ & 0x3) == 1
    }

    bool isCharacter() const {
        return (bits_ & kTagMask) == kCharacterTag;
    }

    bool isSmallFloat() const {
        return (bits_ & kTagMask) == kSmallFloatTag;
    }

    // ========== SmallInteger Operations ==========

    // Spur SmallInteger encoding: (value << 3) | 1
    // This gives 61 bits of signed integer range
    static Oop fromSmallInteger(int64_t value) {
        return Oop((static_cast<uint64_t>(value) << 3) | kSmallIntTag);
    }

    int64_t toSmallInteger() const {
        assert(isSmallInteger());
        return static_cast<int64_t>(bits_) >> 3;  // Arithmetic shift preserves sign
    }

    // Aliases matching Spur naming
    static Oop integerObjectOf(int64_t value) { return fromSmallInteger(value); }
    int64_t integerValueOf() const { return toSmallInteger(); }

    // ========== Space Queries ==========

    Space space() const {
        assert(isNonImmediate());
        return static_cast<Space>((bits_ & kSpaceMask) >> kSpaceShift);
    }

    bool isInNewSpace() const {
        return isNonImmediate() && space() == Space::New;
    }

    bool isInOldSpace() const {
        return isNonImmediate() && space() == Space::Old;
    }

    bool isInPermSpace() const {
        return isNonImmediate() && space() == Space::Perm;
    }

    // VM-style aliases
    bool isYoung() const { return isInNewSpace(); }
    bool isOld() const { return isInOldSpace(); }
    bool isPermanent() const { return isInPermSpace(); }

    // ========== Pointer Operations ==========

    // Get the actual memory address (strips tag bits)
    void* rawPointer() const {
        assert(isNonImmediate());
        return reinterpret_cast<void*>(bits_ & kPointerMask);
    }

    // Typed pointer access
    template<typename T>
    T* as() const {
        return static_cast<T*>(rawPointer());
    }

    // Create Oop from pointer + space
    // The pointer must be 8-byte aligned (low 3 bits = 0)
    static Oop fromPointer(void* ptr, Space space) {
        uint64_t addr = reinterpret_cast<uint64_t>(ptr);
        assert((addr & kTagMask) == 0 && "Pointer must be 8-byte aligned");
        return Oop(addr | (static_cast<uint64_t>(space) << kSpaceShift));
    }

    // Change space without changing pointer
    Oop withSpace(Space newSpace) const {
        assert(isNonImmediate());
        uint64_t newBits = (bits_ & kPointerMask) | (static_cast<uint64_t>(newSpace) << kSpaceShift);
        return Oop(newBits);
    }

    // ========== Legacy Oop Conversion ==========

    // Convert from legacy Spur oop (address-based space detection)
    // Used during image loading
    static Oop fromLegacy(uint64_t legacyOop, Space space) {
        // Immediates pass through unchanged
        if (legacyOop & kImmediateTag) {
            return Oop(legacyOop);
        }
        // Re-encode pointer with explicit space
        void* ptr = reinterpret_cast<void*>(legacyOop & kPointerMask);
        return fromPointer(ptr, space);
    }

    // ========== Special Values ==========

    static Oop nil() { return Oop(0); }  // Adjust if nil has different encoding
    bool isNil() const { return bits_ == 0; }
    bool notNil() const { return bits_ != 0; }

    // ========== Comparison ==========

    bool operator==(const Oop& other) const { return bits_ == other.bits_; }
    bool operator!=(const Oop& other) const { return bits_ != other.bits_; }
    bool operator<(const Oop& other) const { return bits_ < other.bits_; }
    bool operator<=(const Oop& other) const { return bits_ <= other.bits_; }
    bool operator>(const Oop& other) const { return bits_ > other.bits_; }
    bool operator>=(const Oop& other) const { return bits_ >= other.bits_; }

    // ========== Arithmetic (for pointer math) ==========

    Oop operator+(int64_t offset) const {
        return Oop(bits_ + offset);
    }

    Oop operator-(int64_t offset) const {
        return Oop(bits_ - offset);
    }

    int64_t operator-(const Oop& other) const {
        return static_cast<int64_t>(bits_ - other.bits_);
    }
};

// Ensure Oop is exactly 8 bytes (same as sqInt)
static_assert(sizeof(Oop) == 8, "Oop must be 8 bytes");

} // namespace pharo

// C interface for gradual migration
extern "C" {

typedef pharo::Oop sqOop;

inline bool sqOopIsImmediate(sqOop oop) { return oop.isImmediate(); }
inline bool sqOopIsYoung(sqOop oop) { return oop.isYoung(); }
inline bool sqOopIsOld(sqOop oop) { return oop.isOld(); }
inline bool sqOopIsPermanent(sqOop oop) { return oop.isPermanent(); }
inline int64_t sqOopIntegerValue(sqOop oop) { return oop.integerValueOf(); }
inline sqOop sqOopFromInteger(int64_t val) { return pharo::Oop::fromSmallInteger(val); }

}

#else // Pure C fallback

// For C code that can't use C++, provide minimal compatibility
typedef uint64_t sqOop;
#define sqOopIsImmediate(oop) ((oop) & 1)

#endif // __cplusplus
