/*
 * JITMethod.hpp - JIT-compiled method representation
 *
 * Copyright (c) 2026 Aaron Wohl. Licensed under the MIT License.
 *
 * Defines the in-memory layout of JIT-compiled methods in the code zone.
 * Each JITMethod is a fixed header followed by variable-length machine code.
 *
 * LAYOUT IN CODE ZONE:
 *
 *     +---------------------------+  <-- JITMethod* (aligned to MethodAlignment)
 *     | JITMethod header (64 B)   |
 *     +---------------------------+  <-- codeStart()
 *     | Machine code              |
 *     | ...                       |
 *     +---------------------------+
 *     | Inline cache entries      |  (appended after code, each is an ICEntry)
 *     | ...                       |
 *     +---------------------------+  <-- codeStart() + totalSize_
 *
 * STATE MACHINE:
 *
 *     Interpreted ──(compile threshold)──> Compiled
 *     Compiled ──(invalidation)──> Invalidated ──(GC reclaim)──> (freed)
 *     Compiled ──(recompile/optimize)──> Compiled (new version)
 *
 * INLINE CACHE ENTRIES:
 *
 * Each send site in the compiled code has a corresponding ICEntry. The
 * machine code stencil references the ICEntry by offset. On a cache miss,
 * the runtime patches the ICEntry (monomorphic) or extends it to a PIC.
 */

#ifndef PHARO_JIT_METHOD_HPP
#define PHARO_JIT_METHOD_HPP

#include "JITConfig.hpp"
#include "../Oop.hpp"
#include <cstdint>
#include <cstddef>
#include <atomic>
#include <new>

#if PHARO_JIT_ENABLED

namespace pharo {
namespace jit {

// ===== METHOD STATE =====

enum class MethodState : uint8_t {
    Interpreted = 0,   // Not yet compiled; executing via interpreter
    Compiled    = 1,   // Machine code is valid and executable
    Invalidated = 2,   // Machine code exists but must not be entered
                       // (e.g., class hierarchy changed, become:, GC moved things)
};

// ===== INLINE CACHE ENTRY =====
//
// Each message send site has one ICEntry in the compiled method.
// Monomorphic: single (class, target) pair checked inline.
// Polymorphic: up to MaxPICEntries (class, target) pairs.
// Megamorphic: falls back to the global method cache.

struct ICEntry {
    enum class Kind : uint8_t {
        Empty       = 0,  // Not yet seen a send (will miss immediately)
        Monomorphic = 1,  // Single cached class
        Polymorphic = 2,  // Multiple cached classes (closed PIC)
        Megamorphic = 3,  // Too many classes, use global cache
    };

    struct CacheSlot {
        uint64_t classOop;   // Raw bits of the receiver class Oop
        uint64_t targetAddr; // Address of compiled target, or Oop of method
    };

    Kind     kind;
    uint8_t  numEntries;            // Number of valid entries (0-MaxPICEntries)
    uint16_t bytecodeOffset;        // Offset in source bytecodes (for deopt)
    uint32_t codeOffset;            // Offset from method codeStart() to this send's patch point
    CacheSlot slots[MaxPICEntries]; // Cache entries

    void reset() {
        kind = Kind::Empty;
        numEntries = 0;
        for (size_t i = 0; i < MaxPICEntries; i++) {
            slots[i].classOop = 0;
            slots[i].targetAddr = 0;
        }
    }
};

// ===== JIT METHOD HEADER =====
//
// Lives at the start of each compiled method's allocation in the code zone.
// Immediately followed by the machine code bytes.

struct JITMethod {
    // --- Identity (link back to Smalltalk objects) ---
    uint64_t  compiledMethodOop;  // The CompiledMethod this was compiled from (raw Oop bits)
    uint64_t  selectorOop;        // Cached selector (raw Oop bits) for debugging/profiling
    uint64_t  methodHeader;       // Cached method header bits (arg count, temps, etc.)

    // --- Code geometry ---
    uint32_t  codeSize;           // Size of machine code in bytes (after this header)
    uint16_t  numICEntries;       // Number of inline cache entries
    uint16_t  numBytecodes;       // Source bytecode count (for sizing heuristics)

    // --- State ---
    MethodState state;
    uint8_t     tier;             // 0 = interpreter, 1 = copy-and-patch, 2 = optimizing
    uint8_t     argCount;         // Cached from method header
    uint8_t     tempCount;        // Cached from method header
    bool        hasSends;         // Contains actual send stencils (need deopt, can't execute)
    bool        hasHeapWrites;    // Writes to heap objects (need write barrier, can't execute)
    bool        hasRecvFieldAccess; // Reads/writes receiver instance variables
    bool        hasRecvFieldWrite;  // Writes to receiver instance variables
    bool        hasLitVarWrite;     // Writes to literal variables (Associations)
    bool        hasPrimPrologue;    // Has machine-code primitive fast path at entry
    uint8_t     maxRecvFieldIndex;  // Max receiver slot index accessed (for bounds checking)

    // --- Statistics ---
    uint32_t  executionCount;     // Incremented on each entry (for hot method detection)
    uint32_t  totalSize;          // Total allocation size including header + code + IC entries

    // --- Navigation ---
    JITMethod* nextInZone;        // Next method in code zone (for iteration/compaction)
    JITMethod* prevInZone;        // Previous method in code zone

    // --- LRU eviction ---
    uint32_t  lastUsedEpoch;      // Updated on entry; compared against global epoch for LRU

    // --- Re-entry table ---
    uint32_t  bcToCodeTableOffset; // Offset from codeStart() to uint32_t[numBytecodes+1] table

    // --- Accessors ---

    // Pointer to the start of machine code (immediately after header)
    uint8_t* codeStart() {
        return reinterpret_cast<uint8_t*>(this) + sizeof(JITMethod);
    }

    const uint8_t* codeStart() const {
        return reinterpret_cast<const uint8_t*>(this) + sizeof(JITMethod);
    }

    // Pointer to the inline cache entries (after machine code)
    ICEntry* icEntries() {
        return reinterpret_cast<ICEntry*>(codeStart() + codeSize);
    }

    const ICEntry* icEntries() const {
        return reinterpret_cast<const ICEntry*>(codeStart() + codeSize);
    }

    // Pointer to the bcToCode re-entry table
    const uint32_t* bcToCodeTable() const {
        return reinterpret_cast<const uint32_t*>(codeStart() + bcToCodeTableOffset);
    }

    // Look up the code offset for a given bytecode offset.
    // Returns the code offset, or codeSize if bcOffset is out of range.
    uint32_t codeOffsetForBC(uint32_t bcOffset) const {
        if (bcOffset >= numBytecodes) return codeSize;
        return bcToCodeTable()[bcOffset];
    }

    // Function pointer to compiled code entry point
    using EntryFunc = void(*)(void* interpreterState);

    EntryFunc entryPoint() const {
        return reinterpret_cast<EntryFunc>(
            const_cast<uint8_t*>(codeStart()));
    }

    // Check if this method is valid to execute
    bool isExecutable() const {
        return state == MethodState::Compiled;
    }

    // Mark for invalidation (e.g., class hierarchy change)
    void invalidate() {
        state = MethodState::Invalidated;
    }

    // Total bytes consumed in the code zone
    size_t allocationSize() const {
        return totalSize;
    }
};

// JITMethod header is 72 bytes (fits in 2 cache lines at 64-byte alignment)
static_assert(sizeof(JITMethod) <= 128,
              "JITMethod header should fit in 2 cache lines");

// ===== METHOD MAP =====
//
// Maps CompiledMethod Oops to their JIT-compiled versions.
// This is how the interpreter finds compiled code for a method.
//
// We use a simple open-addressing hash table. The Interpreter checks this
// table on method activation: if a compiled version exists and is valid,
// jump to it instead of interpreting.

class MethodMap {
public:
    static constexpr size_t DefaultCapacity = 8192;

    MethodMap() : entries_(nullptr), capacity_(0), count_(0) {}
    ~MethodMap() { delete[] entries_; }

    bool initialize(size_t capacity = DefaultCapacity) {
        capacity_ = capacity;
        entries_ = new(std::nothrow) Entry[capacity_];
        if (!entries_) return false;
        clear();
        return true;
    }

    // Look up the JIT method for a CompiledMethod Oop.
    // Returns nullptr if not compiled or invalidated.
    JITMethod* lookup(uint64_t compiledMethodBits) const {
        if (!entries_) return nullptr;
        size_t mask = capacity_ - 1;
        size_t idx = hash(compiledMethodBits) & mask;

        for (size_t probe = 0; probe < capacity_; probe++) {
            Entry& e = entries_[idx];
            if (e.key == 0) return nullptr;  // Empty slot
            if (e.key == compiledMethodBits && e.value && e.value->isExecutable()) {
                return e.value;
            }
            idx = (idx + 1) & mask;
        }
        return nullptr;
    }

    // Register a compiled method.
    bool insert(uint64_t compiledMethodBits, JITMethod* jitMethod) {
        if (!entries_) return false;
        if (count_ * 4 >= capacity_ * 3) return false;  // 75% load factor

        size_t mask = capacity_ - 1;
        size_t idx = hash(compiledMethodBits) & mask;

        for (size_t probe = 0; probe < capacity_; probe++) {
            Entry& e = entries_[idx];
            if (e.key == 0 || e.key == compiledMethodBits) {
                if (e.key == 0) count_++;
                e.key = compiledMethodBits;
                e.value = jitMethod;
                return true;
            }
            idx = (idx + 1) & mask;
        }
        return false;
    }

    // Remove a method (on invalidation or eviction).
    void remove(uint64_t compiledMethodBits) {
        if (!entries_) return;
        size_t mask = capacity_ - 1;
        size_t idx = hash(compiledMethodBits) & mask;

        for (size_t probe = 0; probe < capacity_; probe++) {
            Entry& e = entries_[idx];
            if (e.key == 0) return;
            if (e.key == compiledMethodBits) {
                // Tombstone: set value to null but keep key for probe chain
                e.value = nullptr;
                return;
            }
            idx = (idx + 1) & mask;
        }
    }

    void clear() {
        if (entries_) {
            for (size_t i = 0; i < capacity_; i++) {
                entries_[i].key = 0;
                entries_[i].value = nullptr;
            }
        }
        count_ = 0;
    }

    size_t count() const { return count_; }

private:
    struct Entry {
        uint64_t   key;    // CompiledMethod Oop raw bits (0 = empty)
        JITMethod* value;  // Compiled version (nullptr = tombstone)
    };

    Entry* entries_;
    size_t capacity_;
    size_t count_;

    static size_t hash(uint64_t bits) {
        // Fibonacci hashing (good distribution for pointer-like values)
        return static_cast<size_t>((bits >> 3) * 11400714819323198485ULL);
    }
};

} // namespace jit
} // namespace pharo

#endif // PHARO_JIT_ENABLED
#endif // PHARO_JIT_METHOD_HPP
