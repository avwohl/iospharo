/*
 * oop.hpp - Type-safe Oop class for iOS Pharo VM
 *
 * Pure C++ implementation. No C fallbacks, no gradual migration.
 * VMMaker generates C++ code that uses these types directly.
 *
 * DESIGN PRINCIPLES:
 * 1. NO implicit conversions - all conversions must be explicit
 * 2. NO arithmetic operators on Oop - prevents tag bit corruption
 * 3. Compile errors for incorrect usage, not runtime bugs
 *
 * iOS TAGGING SCHEME (modified from Spur to allow space encoding):
 *
 *   Bit 0 determines immediate vs object pointer:
 *     Bit 0 = 1: Immediate (value in bits 3-63)
 *       001 = SmallInteger
 *       011 = Character (was 010 in original Spur)
 *       101 = SmallFloat (was 100 in original Spur)
 *     Bit 0 = 0: Object pointer
 *       Bits 1-2: Space encoding (00=new, 01=old, 10=perm, 11=code)
 *       Bits 3-63: Heap address (always 8-byte aligned)
 *
 *   During image load (swizzle), original Spur tags are converted:
 *     Character: 010 -> 011
 *     SmallFloat: 100 -> 101
 *     Object pointers: 000 -> space-encoded
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <cassert>

namespace pharo {

// ============================================================================
// Memory Space
// ============================================================================

enum class Space : uint8_t {
    New  = 0,   // Young generation  - bits 1-2 = 00
    Old  = 1,   // Tenured objects   - bits 1-2 = 01
    Perm = 2,   // Permanent space   - bits 1-2 = 10
    Code = 3    // Reserved          - bits 1-2 = 11
};

// ============================================================================
// RawAddress - Actual memory address, no tag bits
// ============================================================================

class RawAddress {
    char* ptr_;

public:
    constexpr RawAddress() : ptr_(nullptr) {}
    explicit constexpr RawAddress(void* p) : ptr_(static_cast<char*>(p)) {}
    explicit constexpr RawAddress(char* p) : ptr_(p) {}
    explicit RawAddress(uint64_t addr) : ptr_(reinterpret_cast<char*>(addr)) {}

    char* ptr() const { return ptr_; }
    void* voidPtr() const { return ptr_; }
    uint64_t bits() const { return reinterpret_cast<uint64_t>(ptr_); }

    bool isNull() const { return ptr_ == nullptr; }
    explicit operator bool() const { return ptr_ != nullptr; }

    // Arithmetic - allowed on RawAddress
    RawAddress operator+(int64_t offset) const { return RawAddress(ptr_ + offset); }
    RawAddress operator-(int64_t offset) const { return RawAddress(ptr_ - offset); }
    int64_t operator-(RawAddress other) const { return ptr_ - other.ptr_; }
    RawAddress& operator+=(int64_t offset) { ptr_ += offset; return *this; }
    RawAddress& operator-=(int64_t offset) { ptr_ -= offset; return *this; }

    // Comparison
    bool operator==(RawAddress other) const { return ptr_ == other.ptr_; }
    bool operator!=(RawAddress other) const { return ptr_ != other.ptr_; }
    bool operator<(RawAddress other) const { return ptr_ < other.ptr_; }
    bool operator<=(RawAddress other) const { return ptr_ <= other.ptr_; }
    bool operator>(RawAddress other) const { return ptr_ > other.ptr_; }
    bool operator>=(RawAddress other) const { return ptr_ >= other.ptr_; }

    // Memory access
    int64_t longAt() const { return *reinterpret_cast<int64_t*>(ptr_); }
    uint64_t ulongAt() const { return *reinterpret_cast<uint64_t*>(ptr_); }
    int32_t intAt() const { return *reinterpret_cast<int32_t*>(ptr_); }
    uint32_t uintAt() const { return *reinterpret_cast<uint32_t*>(ptr_); }
    int16_t shortAt() const { return *reinterpret_cast<int16_t*>(ptr_); }
    uint16_t ushortAt() const { return *reinterpret_cast<uint16_t*>(ptr_); }
    int8_t byteAt() const { return *reinterpret_cast<int8_t*>(ptr_); }
    uint8_t ubyteAt() const { return *reinterpret_cast<uint8_t*>(ptr_); }
    double doubleAt() const { return *reinterpret_cast<double*>(ptr_); }
    float floatAt() const { return *reinterpret_cast<float*>(ptr_); }

    void longAtPut(int64_t v) { *reinterpret_cast<int64_t*>(ptr_) = v; }
    void ulongAtPut(uint64_t v) { *reinterpret_cast<uint64_t*>(ptr_) = v; }
    void intAtPut(int32_t v) { *reinterpret_cast<int32_t*>(ptr_) = v; }
    void uintAtPut(uint32_t v) { *reinterpret_cast<uint32_t*>(ptr_) = v; }
    void shortAtPut(int16_t v) { *reinterpret_cast<int16_t*>(ptr_) = v; }
    void ushortAtPut(uint16_t v) { *reinterpret_cast<uint16_t*>(ptr_) = v; }
    void byteAtPut(int8_t v) { *reinterpret_cast<int8_t*>(ptr_) = v; }
    void ubyteAtPut(uint8_t v) { *reinterpret_cast<uint8_t*>(ptr_) = v; }
    void doubleAtPut(double v) { *reinterpret_cast<double*>(ptr_) = v; }
    void floatAtPut(float v) { *reinterpret_cast<float*>(ptr_) = v; }

    template<typename T> T* as() const { return reinterpret_cast<T*>(ptr_); }
};

// ============================================================================
// Oop - Tagged Object Pointer
// ============================================================================

class Oop {
    uint64_t bits_;

    // Tag constants - iOS scheme where all immediates have bit 0 = 1
    static constexpr uint64_t kTagMask       = 0x7ULL;
    static constexpr uint64_t kTagBits       = 3;
    static constexpr uint64_t kImmediateBit  = 0x1ULL;  // bit 0 = 1 for all immediates
    static constexpr uint64_t kSmallIntTag   = 0x1ULL;  // 001
    static constexpr uint64_t kCharacterTag  = 0x3ULL;  // 011 (was 010 in Spur)
    static constexpr uint64_t kSmallFloatTag = 0x5ULL;  // 101 (was 100 in Spur)
    static constexpr uint64_t kSpaceMask     = 0x6ULL;  // bits 1-2 (only for non-immediates)
    static constexpr uint64_t kSpaceShift    = 1;
    static constexpr uint64_t kAddressMask   = ~kTagMask;

    // SmallFloat constants
    static constexpr uint64_t kSmallFloatExponentOffset = 896ULL;
    static constexpr int kSmallFloatMantissaBits = 52;

    explicit constexpr Oop(uint64_t raw) : bits_(raw) {}

public:
    // ========== Construction ==========

    constexpr Oop() : bits_(0) {}
    Oop(const Oop&) = default;
    Oop(Oop&&) = default;
    Oop& operator=(const Oop&) = default;
    Oop& operator=(Oop&&) = default;

    // Raw bits access - for generated code interop
    uint64_t bits() const { return bits_; }
    static Oop fromBits(uint64_t raw) { return Oop(raw); }

    // ========== Type Tests ==========

    // Bit 0 = 1 means immediate, bit 0 = 0 means object pointer
    bool isImmediate() const { return (bits_ & kImmediateBit) != 0; }
    bool isNonImmediate() const { return (bits_ & kImmediateBit) == 0; }

    // Specific immediate types (exact tag match)
    bool isSmallInteger() const { return (bits_ & kTagMask) == kSmallIntTag; }
    bool isCharacter() const { return (bits_ & kTagMask) == kCharacterTag; }
    bool isSmallFloat() const { return (bits_ & kTagMask) == kSmallFloatTag; }

    // ========== SmallInteger ==========

    static Oop fromSmallInteger(int64_t value) {
        return Oop((static_cast<uint64_t>(value) << kTagBits) | kSmallIntTag);
    }

    int64_t toSmallInteger() const {
        assert(isSmallInteger());
        return static_cast<int64_t>(bits_) >> kTagBits;
    }

    static bool canBeSmallInteger(int64_t value) {
        int64_t top = value >> 60;
        return top == 0 || top == -1;
    }

    // ========== Character ==========

    static Oop fromCharacter(uint32_t codepoint) {
        return Oop((static_cast<uint64_t>(codepoint) << kTagBits) | kCharacterTag);
    }

    uint32_t toCharacter() const {
        assert(isCharacter());
        return static_cast<uint32_t>(bits_ >> kTagBits);
    }

    // ========== SmallFloat ==========

    static bool canBeSmallFloat(double value) {
        uint64_t raw;
        memcpy(&raw, &value, sizeof(raw));
        uint64_t exp = (raw >> 52) & 0x7FF;
        uint64_t mantissaLow = raw & 0x7;
        if (mantissaLow != 0) return false;
        if (exp == 0 || exp == 0x7FF) return false;
        if (exp < 896 || exp > 1151) return false;
        return true;
    }

    static Oop fromSmallFloat(double value) {
        assert(canBeSmallFloat(value));
        uint64_t raw;
        memcpy(&raw, &value, sizeof(raw));
        uint64_t rotated = ((raw >> 63) & 1) | (raw << 1);
        if (rotated > 1) {
            rotated -= (kSmallFloatExponentOffset << (kSmallFloatMantissaBits + 1));
        }
        return Oop((rotated << kTagBits) | kSmallFloatTag);
    }

    double toSmallFloat() const {
        assert(isSmallFloat());
        uint64_t rotated = bits_ >> kTagBits;
        if (rotated > 1) {
            rotated += (kSmallFloatExponentOffset << (kSmallFloatMantissaBits + 1));
        }
        uint64_t raw = (rotated >> 1) | ((rotated & 1) << 63);
        double result;
        memcpy(&result, &raw, sizeof(result));
        return result;
    }

    // ========== Space (non-immediates only) ==========

    Space space() const {
        assert(isNonImmediate());
        return static_cast<Space>((bits_ & kSpaceMask) >> kSpaceShift);
    }

    bool isYoung() const { return isNonImmediate() && space() == Space::New; }
    bool isOld() const { return isNonImmediate() && space() == Space::Old; }
    bool isPermanent() const { return isNonImmediate() && space() == Space::Perm; }

    Oop withSpace(Space s) const {
        assert(isNonImmediate());
        return Oop((bits_ & kAddressMask) | (static_cast<uint64_t>(s) << kSpaceShift));
    }

    // ========== Address (non-immediates only) ==========

    RawAddress rawAddress() const {
        assert(isNonImmediate());
        return RawAddress(bits_ & kAddressMask);
    }

    static Oop fromPointer(void* ptr, Space space) {
        uint64_t addr = reinterpret_cast<uint64_t>(ptr);
        assert((addr & kTagMask) == 0);
        return Oop(addr | (static_cast<uint64_t>(space) << kSpaceShift));
    }

    static Oop fromPointer(RawAddress addr, Space space) {
        return fromPointer(addr.voidPtr(), space);
    }

    template<typename T> T* as() const { return rawAddress().as<T>(); }

    // ========== Object Field Access ==========

    static constexpr int64_t BaseHeaderSize = 8;
    static constexpr int64_t BytesPerSlot = 8;

    uint64_t header() const {
        return rawAddress().ulongAt();
    }

    void headerPut(uint64_t h) const {
        rawAddress().ulongAtPut(h);
    }

    // Slot access (oop fields, 0-indexed from after header)
    Oop slotAt(int64_t index) const {
        assert(index >= 0);
        return Oop::fromBits((rawAddress() + BaseHeaderSize + index * BytesPerSlot).ulongAt());
    }

    void slotAtPut(int64_t index, Oop value) const {
        assert(index >= 0);
        (rawAddress() + BaseHeaderSize + index * BytesPerSlot).ulongAtPut(value.bits());
    }

    // Raw byte access (offset from object start)
    int64_t longAt(int64_t offset) const { return (rawAddress() + offset).longAt(); }
    uint64_t ulongAt(int64_t offset) const { return (rawAddress() + offset).ulongAt(); }
    int32_t intAt(int64_t offset) const { return (rawAddress() + offset).intAt(); }
    uint32_t uintAt(int64_t offset) const { return (rawAddress() + offset).uintAt(); }
    int16_t shortAt(int64_t offset) const { return (rawAddress() + offset).shortAt(); }
    uint16_t ushortAt(int64_t offset) const { return (rawAddress() + offset).ushortAt(); }
    int8_t byteAt(int64_t offset) const { return (rawAddress() + offset).byteAt(); }
    uint8_t ubyteAt(int64_t offset) const { return (rawAddress() + offset).ubyteAt(); }

    void longAtPut(int64_t offset, int64_t v) const { (rawAddress() + offset).longAtPut(v); }
    void ulongAtPut(int64_t offset, uint64_t v) const { (rawAddress() + offset).ulongAtPut(v); }
    void intAtPut(int64_t offset, int32_t v) const { (rawAddress() + offset).intAtPut(v); }
    void uintAtPut(int64_t offset, uint32_t v) const { (rawAddress() + offset).uintAtPut(v); }
    void shortAtPut(int64_t offset, int16_t v) const { (rawAddress() + offset).shortAtPut(v); }
    void ushortAtPut(int64_t offset, uint16_t v) const { (rawAddress() + offset).ushortAtPut(v); }
    void byteAtPut(int64_t offset, int8_t v) const { (rawAddress() + offset).byteAtPut(v); }
    void ubyteAtPut(int64_t offset, uint8_t v) const { (rawAddress() + offset).ubyteAtPut(v); }

    // Oop at offset (for reading oop fields at arbitrary offsets)
    Oop oopAt(int64_t offset) const {
        return Oop::fromBits((rawAddress() + offset).ulongAt());
    }

    void oopAtPut(int64_t offset, Oop value) const {
        (rawAddress() + offset).ulongAtPut(value.bits());
    }

    // ========== Special Values ==========

    static Oop nil() { return Oop(0); }
    bool isNil() const { return bits_ == 0; }
    bool notNil() const { return bits_ != 0; }

    // Valid object pointer (non-nil and non-immediate)
    bool isObjectPointer() const { return notNil() && isNonImmediate(); }

    // ========== Comparison ==========

    bool operator==(Oop other) const { return bits_ == other.bits_; }
    bool operator!=(Oop other) const { return bits_ != other.bits_; }
    bool operator<(Oop other) const { return bits_ < other.bits_; }
    bool operator<=(Oop other) const { return bits_ <= other.bits_; }
    bool operator>(Oop other) const { return bits_ > other.bits_; }
    bool operator>=(Oop other) const { return bits_ >= other.bits_; }

    // ========== NO ARITHMETIC OPERATORS ==========
    // Deliberately omitted. Use rawAddress() for pointer math.

    // ========== Legacy Conversion ==========

    static Oop fromLegacy(uint64_t legacyBits, Space space) {
        if (legacyBits & kTagMask) return Oop(legacyBits);
        return fromPointer(reinterpret_cast<void*>(legacyBits), space);
    }
};

static_assert(sizeof(Oop) == 8, "Oop must be 8 bytes");
static_assert(sizeof(RawAddress) == 8, "RawAddress must be 8 bytes");

} // namespace pharo

// ============================================================================
// Global using declarations for generated code
// ============================================================================

using pharo::Oop;
using pharo::RawAddress;
using pharo::Space;
