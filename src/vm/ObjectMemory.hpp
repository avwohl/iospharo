/*
 * ObjectMemory.hpp - Heap Management for Pharo VM
 *
 * This class manages the Smalltalk heap, including:
 * - Memory spaces (old space, new space, permanent space)
 * - Object allocation
 * - Class table (maps 22-bit class indices to class objects)
 * - Special objects (nil, true, false, etc.)
 * - Garbage collection hooks
 *
 * MEMORY LAYOUT:
 *
 *   ┌────────────────────┐  ← permSpaceStart_
 *   │   Permanent Space  │  Special objects that never move
 *   ├────────────────────┤  ← permSpaceEnd_ / oldSpaceStart_
 *   │                    │
 *   │     Old Space      │  Tenured objects (survivors of GC)
 *   │                    │
 *   ├────────────────────┤  ← oldSpaceEnd_ / newSpaceStart_
 *   │      Eden          │  New allocations go here
 *   ├────────────────────┤
 *   │   Survivor Space   │  Objects that survived 1 GC
 *   └────────────────────┘  ← newSpaceEnd_
 *
 * The Oop class encodes which space an object is in using bits 1-2:
 *   00 = Old space
 *   01 = New space
 *   10 = Permanent space
 *   11 = Reserved
 */

#ifndef PHARO_OBJECT_MEMORY_HPP
#define PHARO_OBJECT_MEMORY_HPP

#include "Oop.hpp"
#include "ObjectHeader.hpp"
#include <vector>
#include <cstdint>
#include <string>
#include <memory>
#include <functional>

namespace pharo {

/// Configuration for memory allocation
struct MemoryConfig {
    size_t oldSpaceSize = 128 * 1024 * 1024;   // 128 MB default
    size_t newSpaceSize = 16 * 1024 * 1024;    // 16 MB default
    size_t permSpaceSize = 8 * 1024 * 1024;    // 8 MB default
    size_t classTableSize = 1 << 22;           // 4M entries (22-bit index)
    size_t edenRatio = 80;                     // Eden is 80% of new space
};

/// Indices into special objects array
/// These must match the Smalltalk image layout
enum class SpecialObjectIndex : size_t {
    NilObject = 0,
    FalseObject = 1,
    TrueObject = 2,
    SchedulerAssociation = 3,
    ClassBitmap = 4,
    ClassSmallInteger = 5,
    ClassByteString = 6,
    ClassArray = 7,
    // Smalltalk dictionary
    SmalltalkDictionary = 8,
    ClassFloat = 9,
    ClassMethodContext = 10,
    ClassBlockClosure = 11,
    ClassPoint = 12,
    ClassLargePositiveInteger = 13,
    ClassMessage = 14,
    ClassCompiledMethod = 15,
    ClassSemaphore = 18,
    ClassCharacter = 19,
    SelectorDoesNotUnderstand = 20,
    SpecialSelectorsArray = 23,  // Array of 32 special selectors (+, -, at:, at:put:, etc.)
    SelectorMustBeBoolean = 25,
    ClassByteArray = 26,
    ClassProcess = 27,
    CompactClasses = 28,
    SelectorCannotReturn = 36,
    SelectorAboutToReturn = 40,
    ClassLargeNegativeInteger = 42,
    ExternalSemaphoreTable = 38,  // Array of external semaphores (signaled by index)
    ClassFullBlockClosure = 59,
    // Add more as needed
    Count = 60
};

/// Result of a GC operation
struct GCResult {
    size_t bytesReclaimed;
    size_t objectsMoved;
    size_t milliseconds;
};

class ObjectMemory {
public:
    // ===== INITIALIZATION =====

    /// Create an uninitialized ObjectMemory
    ObjectMemory();

    /// Destructor - frees all allocated memory
    ~ObjectMemory();

    /// Initialize memory spaces with given configuration
    /// Returns false if allocation fails
    bool initialize(const MemoryConfig& config = MemoryConfig{});

    /// Check if memory has been initialized
    bool isInitialized() const { return oldSpaceStart_ != nullptr; }

    // ===== OBJECT ALLOCATION =====

    /// Allocate an object with pointer slots.
    /// Returns nil if allocation fails.
    Oop allocateSlots(uint32_t classIndex, size_t slotCount,
                      ObjectFormat format = ObjectFormat::FixedSize);

    /// Allocate a byte object (String, ByteArray).
    /// Returns nil if allocation fails.
    Oop allocateBytes(uint32_t classIndex, size_t byteCount);

    /// Allocate a word object (64-bit indexable).
    /// Returns nil if allocation fails.
    Oop allocateWords(uint32_t classIndex, size_t wordCount);

    /// Clone an existing object
    Oop shallowCopy(Oop original);

    // ===== CLASS TABLE =====

    /// Get the class object for a given class index
    Oop classAtIndex(uint32_t index) const {
        if (index >= classTable_.size()) return Oop::nil();
        return classTable_[index];
    }

    /// Set the class object at a given index
    void setClassAtIndex(uint32_t index, Oop classOop) {
        if (index < classTable_.size()) {
            classTable_[index] = classOop;
        }
    }

    /// Get the class of an object (follows class index to class table)
    Oop classOf(Oop obj) const;

    /// Register a class in the class table, returns assigned index
    uint32_t registerClass(Oop classOop);

    /// Find the index of a class object
    uint32_t indexOfClass(Oop classOop) const;

    // ===== SPECIAL OBJECTS =====

    /// Get a special object by index
    Oop specialObject(SpecialObjectIndex index) const;

    /// Set a special object
    void setSpecialObject(SpecialObjectIndex index, Oop value);

    /// Get the special objects array
    Oop specialObjectsArray() const { return specialObjectsArray_; }

    /// Set the special objects array (during image loading)
    void setSpecialObjectsArray(Oop array) { specialObjectsArray_ = array; }

    /// Convenience accessors for common special objects
    Oop nil() const { return nilObject_; }
    Oop trueObject() const { return trueObject_; }
    Oop falseObject() const { return falseObject_; }

    /// Set up special object cache (call after loading image)
    void cacheSpecialObjects();

    // ===== SYMBOL AND GLOBAL LOOKUP =====

    /// Compare a Symbol object's content to a C string.
    /// Returns true if they match.
    bool symbolEquals(Oop symbol, const char* str) const;

    /// Look up a global in SmalltalkDictionary by string name.
    /// Returns the value (not the Association), nil if not found.
    Oop findGlobal(const std::string& name) const;

    /// Create a minimal MethodContext for startup.
    /// @param method The CompiledMethod to execute
    /// @param receiver The object to receive the message (self)
    /// @return The new Context object, or nil on failure
    Oop createStartupContext(Oop method, Oop receiver);

    // ===== OBJECT ACCESS =====

    /// Fetch a pointer field from an object (0-based index)
    Oop fetchPointer(size_t index, Oop obj) const;

    /// Store a pointer field in an object (0-based index)
    void storePointer(size_t index, Oop obj, Oop value);

    /// Fetch a byte from a byte object
    uint8_t fetchByte(size_t index, Oop obj) const;

    /// Store a byte in a byte object
    void storeByte(size_t index, Oop obj, uint8_t value);

    /// Fetch a 32-bit word from a word object
    uint32_t fetchWord32(size_t index, Oop obj) const;

    /// Store a 32-bit word in a word object
    void storeWord32(size_t index, Oop obj, uint32_t value);

    /// Fetch a 64-bit word from a word object
    uint64_t fetchWord64(size_t index, Oop obj) const;

    /// Store a 64-bit word in a word object
    void storeWord64(size_t index, Oop obj, uint64_t value);

    /// Get the number of slots in an object
    size_t slotCountOf(Oop obj) const;

    /// Get the byte size of an object's content
    size_t byteSizeOf(Oop obj) const;

    /// Get the total size of an object including header
    size_t totalSizeOf(Oop obj) const;

    // ===== OBJECT QUERIES =====

    /// Is this object in young (new) space?
    bool isYoung(Oop obj) const;

    /// Is this object in old space?
    bool isOld(Oop obj) const;

    /// Is this object pinned (won't be moved by GC)?
    bool isPinned(Oop obj) const;

    /// Is this object immutable?
    bool isImmutable(Oop obj) const;

    /// Is this object remembered (has old->young pointer)?
    bool isRemembered(Oop obj) const;

    /// Check if an address is within the heap
    bool isValidHeapAddress(void* addr) const;

    /// Check if an Oop points to a valid object
    bool isValidObject(Oop obj) const;

    // ===== OBJECT MODIFICATION =====

    /// Pin an object so it won't be moved by GC
    void pinObject(Oop obj);

    /// Make an object immutable
    void makeImmutable(Oop obj);

    /// Become: swap identity of two objects
    bool become(Oop obj1, Oop obj2);

    /// One-way become: all references to obj1 become references to obj2
    bool becomeForward(Oop obj1, Oop obj2);

    // ===== IDENTITY HASH =====

    /// Get the identity hash of an object, generating one if needed
    uint32_t identityHashOf(Oop obj);

    /// Ensure an object has an identity hash
    void ensureIdentityHash(Oop obj);

    // ===== GARBAGE COLLECTION =====

    /// Run a scavenge (minor GC of new space)
    GCResult scavenge();

    /// Run an incremental GC step
    GCResult incrementalGC();

    /// Run a full compacting GC
    GCResult fullGC();

    /// Check if GC is needed
    bool needsGC() const;

    /// Force a GC on next allocation
    void forceGC() { forceGCFlag_ = true; }

    /// Register a root for GC (interpreter stack, etc.)
    void addRoot(Oop* root);
    void removeRoot(Oop* root);

    /// Iterate over all objects in the heap
    void allObjectsDo(std::function<void(Oop)> callback);

    /// Find the first instance of a class (by class index)
    Oop firstInstanceOf(uint32_t classIndex);

    /// Find the next instance after a given object
    Oop nextInstanceAfter(Oop object, uint32_t classIndex);

    // ===== MEMORY STATISTICS =====

    struct Statistics {
        size_t bytesAllocated;
        size_t bytesFree;
        size_t objectCount;
        size_t gcCount;
        size_t totalGCTime;
    };

    Statistics statistics() const;

    // ===== LOW-LEVEL ACCESS (for image loader) =====

    /// Get raw memory pointers (use with caution)
    uint8_t* oldSpaceStart() const { return oldSpaceStart_; }
    uint8_t* oldSpaceEnd() const { return oldSpaceEnd_; }
    uint8_t* newSpaceStart() const { return newSpaceStart_; }
    uint8_t* permSpaceStart() const { return permSpaceStart_; }

    /// Get free bytes in old space
    size_t freeOldSpaceBytes() const {
        return static_cast<size_t>(oldSpaceEnd_ - oldSpaceFree_);
    }

    /// Set the free pointer (for image loading)
    void setOldSpaceFreePointer(uint8_t* ptr) { oldSpaceFree_ = ptr; }

    /// Wrap a raw pointer as an Oop with correct space encoding
    Oop oopFromPointer(ObjectHeader* ptr) const;

private:
    // Memory regions
    uint8_t* permSpaceStart_ = nullptr;
    uint8_t* permSpaceEnd_ = nullptr;
    uint8_t* oldSpaceStart_ = nullptr;
    uint8_t* oldSpaceEnd_ = nullptr;
    uint8_t* oldSpaceFree_ = nullptr;   // Next allocation in old space
    uint8_t* newSpaceStart_ = nullptr;
    uint8_t* newSpaceEnd_ = nullptr;
    uint8_t* edenStart_ = nullptr;
    uint8_t* edenFree_ = nullptr;       // Next allocation in eden
    uint8_t* survivorStart_ = nullptr;

    // Class table
    std::vector<Oop> classTable_;
    uint32_t nextClassIndex_ = 1;  // 0 is reserved

    // Special objects
    Oop specialObjectsArray_;
    Oop nilObject_;
    Oop trueObject_;
    Oop falseObject_;

    // Identity hash counter
    uint32_t lastHash_ = 0;

    // GC state
    bool forceGCFlag_ = false;
    std::vector<Oop*> roots_;

    // Statistics
    size_t bytesAllocated_ = 0;
    size_t gcCount_ = 0;
    size_t totalGCTime_ = 0;

    // ===== PRIVATE HELPERS =====

    /// Allocate raw memory in the specified space
    ObjectHeader* allocateRaw(size_t size, Space space);

    /// Set up a new object header
    void initializeHeader(ObjectHeader* obj, uint32_t classIndex,
                          size_t slotCount, ObjectFormat format);

    /// Generate a new identity hash
    uint32_t generateHash();

    /// Determine which space a pointer is in
    Space spaceForPointer(void* ptr) const;

    /// Try to allocate in eden, trigger scavenge if needed
    ObjectHeader* allocateInEden(size_t size);

    /// Promote an object from new space to old space
    Oop promoteObject(Oop obj);

    /// Copy an object's bytes to new location
    void copyObjectBytes(ObjectHeader* from, ObjectHeader* to);

    /// Update a pointer during GC
    void updatePointer(Oop& ptr);

    /// Mark an object as remembered
    void rememberObject(Oop obj);
};

} // namespace pharo

#endif // PHARO_OBJECT_MEMORY_HPP
