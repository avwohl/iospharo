/*
 * ObjectHeader.hpp - Spur 64-bit Object Header Format
 *
 * Copyright (c) 2025-2026 Aaron Wohl. Licensed under the MIT License.
 *
 * Every heap-allocated Smalltalk object starts with a 64-bit header word
 * that encodes metadata about the object. The Spur header format is defined
 * by the Pharo project (https://pharo.org) and the Spur memory manager
 * designed by Eliot Miranda. See THIRD_PARTY_LICENSES for upstream license details.
 *
 * SPUR 64-BIT HEADER LAYOUT:
 *
 *   Bits 0-21:   Class index (22 bits, index into class table)
 *   Bits 22-23:  Reserved (2 bits)
 *   Bits 24-28:  Object format (5 bits, see Format enum)
 *   Bits 29-31:  Reserved (3 bits)
 *   Bits 32-53:  Identity hash (22 bits, 0 = unhashed)
 *   Bits 54-55:  Reserved (2 bits)
 *   Bits 56-63:  Slot count (0-254), or 255 = overflow (extended header)
 *
 * OVERFLOW HEADER:
 *   When slot count is 255, the previous 64-bit word contains the
 *   actual slot count. This supports objects with up to 2^64 slots.
 *
 * OBJECT FORMAT VALUES (5 bits):
 *   0:  Zero-sized (e.g., UndefinedObject, nil, true, false)
 *   1:  Fixed-size pointer fields only
 *   2:  Indexable pointer fields only (e.g., Array)
 *   3:  Both fixed and indexable pointer fields
 *   4:  Weak fields (indexable)
 *   5:  Ephemeron (weak reference with fixed fields)
 *   6:  Unused (reserved)
 *   7:  Reserved
 *   8:  Reserved
 *   9:  64-bit indexable
 *   10-11: 32-bit indexable
 *   12-15: 16-bit indexable
 *   16-23: 8-bit indexable (strings, byte arrays)
 *   24-31: Compiled methods
 */

#ifndef PHARO_OBJECT_HEADER_HPP
#define PHARO_OBJECT_HEADER_HPP

#include "Oop.hpp"
#include <cstdint>
#include <cstring>

namespace pharo {

/// Object format types
enum class ObjectFormat : uint8_t {
    // Fixed-size objects
    ZeroSized = 0,
    FixedSize = 1,

    // Variable-size pointer objects
    Indexable = 2,
    IndexableWithFixed = 3,

    // Weak references
    Weak = 4,
    WeakWithFixed = 5,

    // Reserved
    Reserved6 = 6,
    Reserved7 = 7,
    Reserved8 = 8,

    // Word indexable (64-bit elements)
    Indexable64 = 9,

    // Long indexable (32-bit elements)
    Indexable32 = 10,
    Indexable32Odd = 11,  // Odd number of 32-bit words

    // Short indexable (16-bit elements)
    Indexable16 = 12,
    Indexable16_1 = 13,
    Indexable16_2 = 14,
    Indexable16_3 = 15,

    // Byte indexable (8-bit elements) - Strings, ByteArrays
    Indexable8 = 16,
    Indexable8_1 = 17,
    Indexable8_2 = 18,
    Indexable8_3 = 19,
    Indexable8_4 = 20,
    Indexable8_5 = 21,
    Indexable8_6 = 22,
    Indexable8_7 = 23,

    // Compiled methods
    CompiledMethod = 24,
    CompiledMethod_1 = 25,
    CompiledMethod_2 = 26,
    CompiledMethod_3 = 27,
    CompiledMethod_4 = 28,
    CompiledMethod_5 = 29,
    CompiledMethod_6 = 30,
    CompiledMethod_7 = 31
};

/// Object header flags
struct ObjectFlags {
    bool isImmutable : 1;
    bool isPinned : 1;
    bool isRemembered : 1;
    bool isMarked : 1;
    bool isGrey : 1;
    uint8_t reserved : 2;
};

class ObjectHeader {
private:
    uint64_t header_;

    // CORRECT Spur 64-bit field positions and masks
    // classIndex: bits 0-21 (22 bits)
    static constexpr uint64_t ClassIndexMask = 0x3FFFFFULL;       // Bits 0-21
    static constexpr uint64_t ClassIndexShift = 0;

    // format: bits 24-28 (5 bits)
    static constexpr uint64_t FormatMask = 0x1FULL << 24;         // Bits 24-28
    static constexpr uint64_t FormatShift = 24;

    // identityHash: bits 32-53 (22 bits)
    static constexpr uint64_t HashMask = 0x3FFFFFULL << 32;       // Bits 32-53
    static constexpr uint64_t HashShift = 32;

    // numSlots: bits 56-63 (8 bits)
    static constexpr uint64_t SlotCountMask = 0xFFULL << 56;      // Bits 56-63
    static constexpr uint64_t SlotCountShift = 56;

    // Overflow indicator (when numSlots == 255)
    static constexpr uint64_t OverflowSlots = 255;

    // Flag bits - must match standard Spur 64-bit header layout
    // (from cointerp-cpp.c: immutableBitShift=23, pinnedBitShift=30,
    //  rememberedBitShift=29, greyBitShift=31, markedBitFullShift=55)
    static constexpr uint64_t ImmutableBit = 1ULL << 23;   // Bit 23 (standard Spur)
    static constexpr uint64_t PinnedBit = 1ULL << 30;      // Bit 30 (standard Spur)
    static constexpr uint64_t RememberedBit = 1ULL << 29;  // Bit 29 (standard Spur)
    static constexpr uint64_t MarkedBit = 1ULL << 55;      // Bit 55 (standard Spur)
    static constexpr uint64_t GreyBit = 1ULL << 31;        // Bit 31 (standard Spur)

public:
    // ===== SLOT COUNT =====

    /// Get the number of pointer slots in this object.
    /// For overflow headers (numSlots==255), the previous word contains the
    /// actual slot count. The standard Spur VM masks off the top byte of the
    /// overflow word: (overflowWord << 8) >> 8, extracting the low 56 bits.
    /// This is because the overflow word has 0xFF in its top byte (matching
    /// the numSlots marker pattern).
    size_t slotCount() const {
        uint64_t count = (header_ & SlotCountMask) >> SlotCountShift;
        if (count == OverflowSlots) {
            const uint64_t* overflow = reinterpret_cast<const uint64_t*>(this) - 1;
            uint64_t rawWord = *overflow;
            // Mask off top byte: standard Spur uses (word << 8) >> 8
            uint64_t slotCount = (rawWord << 8) >> 8;
            return static_cast<size_t>(slotCount);
        }
        return static_cast<size_t>(count);
    }

    /// Does this object have an overflow slot count?
    bool hasOverflowSlots() const {
        return ((header_ & SlotCountMask) >> SlotCountShift) == OverflowSlots;
    }

    /// Set slot count. For values > 254, caller must set up overflow header.
    void setSlotCount(uint8_t count) {
        header_ = (header_ & ~SlotCountMask) | (static_cast<uint64_t>(count) << SlotCountShift);
    }

    // ===== IDENTITY HASH =====

    /// Get the identity hash (0 means not yet hashed).
    uint32_t identityHash() const {
        return static_cast<uint32_t>((header_ & HashMask) >> HashShift);
    }

    /// Set the identity hash.
    void setIdentityHash(uint32_t hash) {
        header_ = (header_ & ~HashMask) | (static_cast<uint64_t>(hash & 0x3FFFFF) << HashShift);
    }

    /// Does this object have an identity hash assigned?
    bool hasIdentityHash() const {
        return identityHash() != 0;
    }

    // ===== OBJECT FORMAT =====

    /// Get the object format.
    ObjectFormat format() const {
        return static_cast<ObjectFormat>((header_ & FormatMask) >> FormatShift);
    }

    /// Set the object format.
    void setFormat(ObjectFormat fmt) {
        header_ = (header_ & ~FormatMask) |
                  (static_cast<uint64_t>(fmt) << FormatShift);
    }

    /// Is this object byte-indexable (String, ByteArray)?
    bool isBytesObject() const {
        auto fmt = format();
        return fmt >= ObjectFormat::Indexable8 && fmt <= ObjectFormat::Indexable8_7;
    }

    /// Is this object word-indexable?
    bool isWordsObject() const {
        return format() == ObjectFormat::Indexable64;
    }

    /// Is this object pointer-indexable?
    bool isPointersObject() const {
        auto fmt = format();
        return fmt >= ObjectFormat::ZeroSized && fmt <= ObjectFormat::WeakWithFixed;
    }

    /// Is this a compiled method?
    bool isCompiledMethod() const {
        auto fmt = format();
        return fmt >= ObjectFormat::CompiledMethod && fmt <= ObjectFormat::CompiledMethod_7;
    }

    /// Is this object weak?
    bool isWeak() const {
        auto fmt = format();
        return fmt == ObjectFormat::Weak || fmt == ObjectFormat::WeakWithFixed;
    }

    // ===== CLASS INDEX =====

    /// Get the class index (index into the class table).
    uint32_t classIndex() const {
        return static_cast<uint32_t>((header_ & ClassIndexMask) >> ClassIndexShift);
    }

    /// Set the class index.
    void setClassIndex(uint32_t index) {
        header_ = (header_ & ~ClassIndexMask) |
                  (static_cast<uint64_t>(index & 0x3FFFFF) << ClassIndexShift);
    }

    /// Forwarded object class index (used during GC to indicate object was moved)
    static constexpr uint32_t ForwardedClassIndex = 8;

    /// Is this a forwarded object? (class index == 8)
    bool isForwarded() const {
        return classIndex() == ForwardedClassIndex;
    }

    // ===== FLAGS =====

    bool isImmutable() const { return header_ & ImmutableBit; }
    void setImmutable(bool value) {
        if (value) header_ |= ImmutableBit;
        else header_ &= ~ImmutableBit;
    }

    bool isPinned() const { return header_ & PinnedBit; }
    void setPinned(bool value) {
        if (value) header_ |= PinnedBit;
        else header_ &= ~PinnedBit;
    }

    bool isRemembered() const { return header_ & RememberedBit; }
    void setRemembered(bool value) {
        if (value) header_ |= RememberedBit;
        else header_ &= ~RememberedBit;
    }

    bool isMarked() const { return header_ & MarkedBit; }
    void setMarked(bool value) {
        if (value) header_ |= MarkedBit;
        else header_ &= ~MarkedBit;
    }

    bool isGrey() const { return header_ & GreyBit; }
    void setGrey(bool value) {
        if (value) header_ |= GreyBit;
        else header_ &= ~GreyBit;
    }

    // ===== SIZE CALCULATIONS =====

    /// Get the size of this object in bytes (excluding header).
    size_t byteSize() const {
        auto fmt = format();
        size_t slots = slotCount();

        if (fmt <= ObjectFormat::WeakWithFixed) {
            // Pointer objects: each slot is 8 bytes
            return slots * sizeof(Oop);
        }
        else if (fmt == ObjectFormat::Indexable64) {
            return slots * 8;
        }
        else if (fmt >= ObjectFormat::Indexable32 && fmt <= ObjectFormat::Indexable32Odd) {
            // 32-bit words, packed 2 per slot
            size_t words = slots * 2;
            if (fmt == ObjectFormat::Indexable32Odd) words--;
            return words * 4;
        }
        else if (fmt >= ObjectFormat::Indexable16 && fmt <= ObjectFormat::Indexable16_3) {
            // 16-bit words, packed 4 per slot
            size_t shorts = slots * 4 - (static_cast<int>(fmt) - static_cast<int>(ObjectFormat::Indexable16));
            return shorts * 2;
        }
        else if (fmt >= ObjectFormat::Indexable8 && fmt <= ObjectFormat::Indexable8_7) {
            // 8-bit bytes, packed 8 per slot
            size_t bytes = slots * 8 - (static_cast<int>(fmt) - static_cast<int>(ObjectFormat::Indexable8));
            return bytes;
        }
        else if (fmt >= ObjectFormat::CompiledMethod) {
            // Compiled methods: similar to byte objects, format encodes unused bytes
            // Format 24 = 0 unused, 25 = 1 unused, ... 31 = 7 unused
            size_t bytes = slots * 8 - (static_cast<int>(fmt) - static_cast<int>(ObjectFormat::CompiledMethod));
            return bytes;
        }

        return slots * sizeof(Oop);
    }

    /// Get total size including header (and overflow if present).
    /// Per Spur spec, every object occupies at least 16 bytes (2 words)
    /// to guarantee space for a forwarding pointer during GC.
    size_t totalSize() const {
        size_t size = sizeof(ObjectHeader) + byteSize();
        if (hasOverflowSlots()) {
            size += sizeof(uint64_t);
        }
        // Round up to 8-byte alignment, minimum 16 bytes
        size = (size + 7) & ~7ULL;
        if (size < 16) size = 16;
        return size;
    }

    // ===== SLOT ACCESS =====

    /// Get pointer to first slot (after header).
    Oop* slots() {
        return reinterpret_cast<Oop*>(this + 1);
    }

    const Oop* slots() const {
        return reinterpret_cast<const Oop*>(this + 1);
    }

    /// Get slot at index.
    Oop slotAt(size_t index) const {
        assert(index < slotCount());
        return slots()[index];
    }

    /// Set slot at index.
    void slotAtPut(size_t index, Oop value) {
        assert(index < slotCount());
        slots()[index] = value;
    }

    // ===== BYTE ACCESS (for byte objects) =====

    /// Get pointer to first byte (for byte objects).
    uint8_t* bytes() {
        return reinterpret_cast<uint8_t*>(this + 1);
    }

    const uint8_t* bytes() const {
        return reinterpret_cast<const uint8_t*>(this + 1);
    }

    /// Get byte at index. Works for any non-pointer format (bytes, words, shorts).
    uint8_t byteAt(size_t index) const {
        assert(!isPointersObject() && index < byteSize());
        return bytes()[index];
    }

    /// Set byte at index. Works for any non-pointer format (bytes, words, shorts).
    void byteAtPut(size_t index, uint8_t value) {
        assert(!isPointersObject() && index < byteSize());
        bytes()[index] = value;
    }

    // ===== RAW HEADER ACCESS =====

    uint64_t rawHeader() const { return header_; }
    void setRawHeader(uint64_t raw) { header_ = raw; }

    // ===== HEADER CONSTRUCTION =====

    /// Create a new header with the given parameters.
    static uint64_t makeHeader(uint8_t slotCount, uint32_t hash,
                               ObjectFormat format, uint32_t classIndex) {
        return (static_cast<uint64_t>(slotCount) << SlotCountShift) |
               (static_cast<uint64_t>(hash & 0x3FFFFF) << HashShift) |
               (static_cast<uint64_t>(format) << FormatShift) |
               (static_cast<uint64_t>(classIndex & 0x3FFFFF) << ClassIndexShift);
    }
};

// Verify size
static_assert(sizeof(ObjectHeader) == 8, "ObjectHeader must be 64 bits");

} // namespace pharo

#endif // PHARO_OBJECT_HEADER_HPP
