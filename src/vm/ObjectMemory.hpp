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
/// These must match VMMaker/SpurMemoryManager >>initializeSpecialObjectIndices
enum class SpecialObjectIndex : size_t {
    NilObject = 0,
    FalseObject = 1,
    TrueObject = 2,
    SchedulerAssociation = 3,
    ClassBitmap = 4,
    ClassSmallInteger = 5,
    ClassByteString = 6,           // Was: ClassString
    ClassArray = 7,
    SmalltalkDictionary = 8,       // May be unused in modern images but we try it
    ClassFloat = 9,
    ClassMethodContext = 10,
    SuspendedProcessInCallout = 11, // NOT ClassBlockClosure (that's unused by VM)
    // ClassBlockClosure is NOT in special objects array in Spur/Pharo 12+
    // Use ClassFullBlockClosure (index 59) or look up by name
    ClassBlockClosure = 11,         // WARNING: Returns SuspendedProcessInCallout, not a class!
    ClassProcess = 27,              // WARNING: May be unused/nil in modern images
    ClassPoint = 12,
    ClassLargePositiveInteger = 13,
    // 14 is unused
    ClassMessage = 15,             // Was incorrectly 14
    // ClassCompiledMethod = 16,   // Unused by VM
    TheLowSpaceSemaphore = 17,     // Semaphore signaled when memory is low
    ClassSemaphore = 18,
    ClassCharacter = 19,
    SelectorDoesNotUnderstand = 20,
    SelectorCannotReturn = 21,     // Was incorrectly 36
    ProcessSignalingLowSpace = 22, // Was: TheInputSemaphore
    SpecialSelectorsArray = 23,    // Array of special selectors (+, -, at:, at:put:, etc.)
    // 24 is unused
    SelectorMustBeBoolean = 25,
    ClassByteArray = 26,
    // ClassProcess = 27,          // Unused by VM
    CompactClasses = 28,
    TheTimerSemaphore = 29,        // Semaphore signaled by timer
    TheInterruptSemaphore = 30,    // Semaphore signaled on user interrupt
    ClassFloat32Register = 31,
    ClassFloat64Register = 32,
    // 33 is unused
    SelectorCannotInterpret = 34,
    // 35-37 were context protos, now unused
    ExternalObjectsArray = 38,     // External semaphores/objects
    ClassMutex = 39,
    ProcessInExternalCodeTag = 40, // Was: ClassTranslatedMethod
    TheFinalizationSemaphore = 41,
    ClassLargeNegativeInteger = 42,
    ClassExternalAddress = 43,
    // 44-47 unused
    SelectorAboutToReturn = 48,    // Was incorrectly 40
    SelectorRunWithIn = 49,
    // ... more indices up to 59
    ClassFullBlockClosure = 59,
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

    /// Create a ByteString from a C++ string
    Oop createString(const std::string& str);

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
            // Detect if class 1 is being overwritten with nil
            if (index == 1) {
                Oop oldValue = classTable_[1];
                if (oldValue.rawBits() != 0 && classOop.rawBits() != oldValue.rawBits()) {
                    fprintf(stderr, "[CLASS-OVERWRITE-1] old=0x%llx new=0x%llx\n",
                            (unsigned long long)oldValue.rawBits(),
                            (unsigned long long)classOop.rawBits());
                }
            }
            classTable_[index] = classOop;
            // Verify for class 1
            if (index == 1) {
                Oop verify = classTable_[1];
                fprintf(stderr, "[CLASS-SET-1] set=0x%llx verify=0x%llx match=%d\n",
                        (unsigned long long)classOop.rawBits(),
                        (unsigned long long)verify.rawBits(),
                        classOop.rawBits() == verify.rawBits());
            }
            // Log registration of low-numbered classes (these are often core classes)
            if (index < 100) {
                static FILE* classRegLog = nullptr;
                if (!classRegLog) classRegLog = fopen("/tmp/class_registration.log", "w");
                if (classRegLog) {
                    fprintf(classRegLog, "[CLASS-REG] index=%u classOop=0x%llx\n",
                            index, (unsigned long long)classOop.rawBits());
                    fflush(classRegLog);
                }
            }
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

    /// Look up an interned Symbol by string content.
    /// Searches through the SymbolTable or symbol cache.
    /// Returns the Symbol object, or nil if not found.
    Oop lookupSymbol(const std::string& name);

    /// Set a global in SmalltalkDictionary.
    /// If the global exists, updates its value. Otherwise creates new binding.
    /// Returns true if successful.
    bool setGlobal(const std::string& name, Oop value);

    /// Create a minimal MethodContext for startup.
    /// @param method The CompiledMethod to execute
    /// @param receiver The object to receive the message (self)
    /// @return The new Context object, or nil on failure
    Oop createStartupContext(Oop method, Oop receiver);

    /// Create a Context for executing a startup method with one argument
    /// @param method The CompiledMethod to execute (should take 1 argument)
    /// @param receiver The object to receive the message (self)
    /// @param arg The argument to pass to the method
    /// @return The new Context object, or nil on failure
    Oop createStartupContextWithArg(Oop method, Oop receiver, Oop arg);

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

    /// Iterate over all objects in old space (for GC)
    void forEachObjectInOldSpace(std::function<void(ObjectHeader*)> callback);

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

    /// Check if an Oop points to a valid location within any heap space
    bool isValidPointer(Oop oop) const {
        if (!oop.isObject()) return false;
        uint8_t* ptr = reinterpret_cast<uint8_t*>(oop.asObjectPtr());
        // Check if it's in old space
        if (ptr >= oldSpaceStart_ && ptr < oldSpaceEnd_) return true;
        // Check if it's in new space
        if (ptr >= newSpaceStart_ && ptr < newSpaceEnd_) return true;
        // Check if it's in perm space
        if (ptr >= permSpaceStart_ && ptr < permSpaceEnd_) return true;
        return false;
    }

    /// Debug: Get address of class table entry for detecting corruption
    void* classTableEntryAddress(uint32_t index) const {
        if (index >= classTable_.size()) return nullptr;
        return const_cast<void*>(static_cast<const void*>(&classTable_[index]));
    }

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

    // Identity hash counter (must be non-zero for LCG to work)
    uint32_t lastHash_ = 2166136261;

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
