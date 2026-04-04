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
 *   │     Old Space      │  All allocations go here (bump pointer)
 *   │                    │
 *   ├────────────────────┤  ← oldSpaceEnd_ / newSpaceStart_
 *   │  Eden (scratch)    │  Used by compacting GC for saved-first-fields
 *   ├────────────────────┤
 *   │   Survivor Space   │  Reserved for future generational GC
 *   └────────────────────┘  ← newSpaceEnd_
 *
 * Generational GC is not implemented. All allocations go to old space.
 * Eden exists as scratch memory for the compacting GC's planCompactSavingForwarders().
 * The eden/survivor fields, write barrier, remembered set, and isYoung/isOld helpers
 * are retained for future generational GC implementation.
 *
 * Memory space is determined by address range, not by Oop tag bits.
 * ObjectMemory provides isYoung/isOld/isPerm based on address checks.
 */

#ifndef PHARO_OBJECT_MEMORY_HPP
#define PHARO_OBJECT_MEMORY_HPP

#include "Oop.hpp"
#include "ObjectHeader.hpp"
#include <array>
#include <vector>
#include <cstdint>
#include <string>
#include <memory>
#include <functional>
#include <unordered_set>

namespace pharo {

// Forward declaration
class Interpreter;

/// Linear scanner for iterating objects in a heap region.
/// Follows the Spur reference implementation's two-step pattern:
///   1. objectStartingAt_: if byte7==0xFF, skip 8 (overflow word → header)
///   2. addressAfter_: advance from header by slot count (reading overflow word if needed)
class ObjectScanner {
public:
    ObjectScanner(uint8_t* start, uint8_t* end)
        : scan_(start), end_(end) {}

    /// Returns next object header, or nullptr when exhausted.
    ObjectHeader* next() {
        while (scan_ + 8 <= end_) {  // Need at least 8 bytes for header read
            // Skip zero padding (free space / segment bridges)
            while (scan_ + 8 <= end_ && *reinterpret_cast<uint64_t*>(scan_) == 0)
                scan_ += 8;
            if (scan_ + 8 > end_) return nullptr;

            // objectStartingAt_: if byte7 is 0xFF, this is an overflow word — skip it.
            // In Spur, both the overflow word and the real header have 0xFF in byte 7.
            // The scan always arrives at the overflow word first (start of the object
            // in memory), so one skip positions us at the real header.
            if (scan_[7] == 0xFF) {
                scan_ += 8;
                if (scan_ + 8 > end_) return nullptr;
            }

            // Now scan_ points to the main object header
            ObjectHeader* obj = reinterpret_cast<ObjectHeader*>(scan_);

            // Compute advance from header position
            uint8_t numSlots = scan_[7];  // slot count byte of the header
            size_t advance;
            if (numSlots == 0xFF) {
                // Overflow header: real slot count is in the word before the header
                uint64_t overflowWord = *reinterpret_cast<uint64_t*>(scan_ - 8);
                size_t realSlots = static_cast<size_t>((overflowWord << 8) >> 8);
                advance = 8 + realSlots * 8;
            } else if (numSlots == 0) {
                advance = 16;  // minimum Spur object size (header + 8 bytes padding)
            } else {
                advance = 8 + static_cast<size_t>(numSlots) * 8;
            }

            if (advance > static_cast<size_t>(end_ - scan_)) return nullptr;
            scan_ += advance;
            return obj;
        }
        return nullptr;
    }

    /// Reset to scan a different region.
    void reset(uint8_t* start, uint8_t* end) {
        scan_ = start;
        end_ = end;
    }

private:
    uint8_t* scan_;
    uint8_t* end_;
};

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
    SuspendedProcessInCallout = 11, // Used by FFI callbacks (not a class)
    // Note: In Pharo 12+ this slot is SuspendedProcessInCallout, not ClassBlockClosure.
    // Use ClassFullBlockClosure (index 59) for closures.
    // Alias kept for code that checks "is this a block closure?" against class index.
    ClassBlockClosure = 11,
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
    SelectorAttemptToAssign = 50,   // attemptToAssign:withIndex:
    PrimErrTableIndex = 51,         // Array of primitive error objects
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

    /// Allocate a CompiledMethod with slots for header+literals and bytes for bytecodes.
    /// Returns nil if allocation fails.
    Oop allocateCompiledMethod(uint32_t classIndex, size_t numSlots, size_t bytecodeSize);

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
            if (index >= nextClassIndex_) {
                nextClassIndex_ = index + 1;
            }
        }
    }

    /// Follow forwarding pointers (created by become:).
    /// If the Oop points to a forwarded object (classIndex == 8),
    /// return the forwarding target. Otherwise return the Oop unchanged.
    Oop followForwarded(Oop oop) const {
        if (!oop.isObject()) return oop;
        ObjectHeader* hdr = oop.asObjectPtr();
        // Follow chain of forwarding pointers (usually just one level)
        int limit = 10;  // Prevent infinite loops on corrupt heap
        while (hdr->isForwarded() && limit-- > 0) {
            oop = hdr->slotAt(0);  // Forwarding target is in first slot
            if (!oop.isObject()) return oop;
            hdr = oop.asObjectPtr();
        }
        return oop;
    }

    /// Get the class of an object (follows class index to class table)
    Oop classOf(Oop obj) const;

    /// Get the number of fixed (strong) instance variable slots for an object.
    /// For WeakWithFixed objects, these are the strong fields before the weak
    /// variable part. Reads the instance specification from the object's class.
    size_t fixedFieldCountOf(ObjectHeader* obj) const;
    size_t fixedFieldCountOf(Oop obj) const;

    /// Extract the bytes of a byte-format Oop as a std::string.
    /// Returns empty string if obj is not a bytes object.
    std::string oopToString(Oop obj) const;

    /// Get the name of a class object as a string.
    /// Handles metaclasses (fetches thisClass, appends " class"). Returns "?" on failure.
    std::string nameOfClass(Oop classObj) const;

    /// Get the name of an Oop's class as a string (calls classOf then nameOfClass).
    /// Returns "?" on failure.
    std::string classNameOf(Oop obj) const;

    /// Extract the number of literals from a CompiledMethod's header (slot 0).
    /// Returns 0 if method is not a valid CompiledMethod.
    size_t numLiteralsOf(Oop method) const;

    /// Get the selector (penultimate literal) of a CompiledMethod as a string.
    /// Returns "?" on failure.
    std::string selectorOf(Oop method) const;

    /// Patch a class-side method to be a no-op (return self).
    /// Finds `selectorName` in the metaclass's method dictionary and overwrites
    /// the first bytecode with 0x58 (Sista V1 "return receiver").
    /// Returns true if the method was found and patched.
    bool patchClassMethodToReturnSelf(Oop classObj, const char* selectorName);

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

    /// Cache class indices that need special handling during GC
    /// (call after class table is fully built)
    void cacheGCClassIndices();

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

    /// Fast fetch — caller guarantees obj is a valid heap object.
    /// Skips isObject/isValidPointer checks. Still bounds-checks.
    inline Oop fetchPointerUnchecked(size_t index, Oop obj) const {
        ObjectHeader* header = obj.asObjectPtr();
        if (__builtin_expect(index >= header->slotCount(), 0)) return nilObject_;
        return header->slotAt(index);
    }

    /// Store a pointer field in an object (0-based index)
    void storePointer(size_t index, Oop obj, Oop value);

    /// Fast store — caller guarantees obj is a valid heap object.
    /// Skips isObject check. Still does bounds + remembered set.
    inline void storePointerUnchecked(size_t index, Oop obj, Oop value) {
        ObjectHeader* header = obj.asObjectPtr();
        if (__builtin_expect(index >= header->slotCount(), 0)) return;
        if (isOld(obj) && value.isObject() && isYoung(value)) {
            rememberObject(obj);
        }
        header->slotAtPut(index, value);
    }

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

    /// Address-range checks for GC (inline for performance)
    inline bool isYoungObject(const void* ptr) const {
        auto p = reinterpret_cast<const uint8_t*>(ptr);
        return p >= newSpaceStart_ && p < newSpaceEnd_;
    }
    inline bool isOldObject(const void* ptr) const {
        auto p = reinterpret_cast<const uint8_t*>(ptr);
        return p >= oldSpaceStart_ && p < oldSpaceEnd_;
    }
    inline bool isPermObject(const void* ptr) const {
        auto p = reinterpret_cast<const uint8_t*>(ptr);
        return p >= permSpaceStart_ && p < permSpaceEnd_;
    }

    /// Is this object in young (new) space?
    bool isYoung(Oop obj) const;

    /// Is this object in old space?
    bool isOld(Oop obj) const;

    /// Is this object in permanent space?
    bool isPerm(Oop obj) const;

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

    /// Run a full compacting GC (only safe at known safe points, NOT from allocation)
    /// skipEphemerons: if true, skip ephemeron firing and weak processing.
    /// Used by auto-compact GC to emulate scavenge behavior (don't mourn old-space objects).
    GCResult fullGC(bool skipEphemerons = false);

    /// Run a non-compacting mark-sweep GC (safe to call from allocations)
    void sweepGC();

    /// Check if GC is needed
    bool needsGC() const;

    /// Force a GC on next allocation
    void forceGC() { forceGCFlag_ = true; }

    /// Check if compacting GC is needed at next safe point
    bool needsCompactGC() const { return needsCompactGC_; }
    void clearCompactGCFlag() { needsCompactGC_ = false; }

    // ===== FINALIZATION / MOURNING =====

    /// Pop a mourner from the queue (for primitive 172)
    Oop popMourner() {
        if (mournQueue_.empty()) return nilObject_;
        Oop mourner = mournQueue_.back();
        mournQueue_.pop_back();
        return mourner;
    }

    /// Check if there are mourners waiting
    bool hasMourners() const { return !mournQueue_.empty(); }
    size_t mournQueueSize() const { return mournQueue_.size(); }

    /// Get/clear pending finalization signal count
    int pendingFinalizationSignals() const { return pendingFinalizationSignals_; }
    void clearPendingFinalizationSignals() { pendingFinalizationSignals_ = 0; }

    /// Register a root for GC (interpreter stack, etc.)
    void addRoot(Oop* root);
    void removeRoot(Oop* root);

    /// Set the interpreter (for GC root enumeration)
    void setInterpreter(Interpreter* interp) { interpreter_ = interp; }

    /// Visit every Oop root in ObjectMemory (special objects, class table, etc.)
    /// Visitor signature: void(Oop&)
    /// If includeClassTable is false, class table entries are skipped
    /// (used during mark phase where class table entries should NOT be strong roots).
    template<typename Visitor>
    void forEachMemoryRoot(Visitor&& visitor, bool includeClassTable = true);

    /// Sweep the class table after mark phase: nil entries for unmarked classes.
    void sweepClassTable();

    /// Iterate over all objects in the heap
    void allObjectsDo(std::function<void(Oop)> callback);

    /// Return the first accessible object in heap (perm → old → eden)
    Oop firstObject();

    /// Return the next accessible object after the given one, or SmallInteger 0 if none
    Oop objectAfter(Oop obj);

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
    uint8_t* oldSpaceFree() const { return oldSpaceFree_; }
    uint8_t* newSpaceStart() const { return newSpaceStart_; }
    uint8_t* permSpaceStart() const { return permSpaceStart_; }
    uint8_t* permSpaceEnd() const { return permSpaceEnd_; }

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
    bool oldSpaceUseMmap_ = false;       // true if old space was allocated with mmap
    size_t oldSpaceMmapSize_ = 0;        // Size of mmap'd region
    uint8_t* newSpaceStart_ = nullptr;
    uint8_t* newSpaceEnd_ = nullptr;
    uint8_t* edenStart_ = nullptr;
    uint8_t* edenFree_ = nullptr;       // Next allocation in eden
    uint8_t* survivorStart_ = nullptr;

    // Class table
    std::vector<Oop> classTable_;
    uint32_t nextClassIndex_ = 1;  // Updated during image loading to be past highest used index

    // In-heap class table page Oops (populated during image load).
    // These are the Array objects inside hiddenRootsObj that hold class pointers.
    // Stored as C++ Oops so forEachMemoryRoot keeps them updated through GC.
    std::vector<Oop> classTablePages_;  // index = page number

    // Special objects
    Oop specialObjectsArray_;
    Oop nilObject_;
    Oop trueObject_;
    Oop falseObject_;

    // Hidden heap roots: freeListsObj and hiddenRootsObj live at the start of
    // old space (objects 4 and 5). They must survive GC so the image can be
    // saved in valid Spur format.  Set during image loading.
    Oop freeListsObj_;
    Oop hiddenRootsObj_;
public:
    void setFreeListsObj(Oop obj) { freeListsObj_ = obj; }
    void setHiddenRootsObj(Oop obj) { hiddenRootsObj_ = obj; }
    Oop freeListsObj() const { return freeListsObj_; }
    Oop hiddenRootsObj() const { return hiddenRootsObj_; }
    void setClassTablePage(size_t pageNum, Oop pageOop) {
        if (pageNum >= classTablePages_.size())
            classTablePages_.resize(pageNum + 1);
        classTablePages_[pageNum] = pageOop;
    }
    const std::vector<Oop>& classTablePages() const { return classTablePages_; }
private:

    // Identity hash counter (must be non-zero for LCG to work)
    uint32_t lastHash_ = 2166136261;
public:
    uint32_t lastHash() const { return lastHash_; }
private:

    // GC state
    bool forceGCFlag_ = false;
    bool needsCompactGC_ = false;  // Set by allocator when compaction needed at safe point
    size_t lastCompactedSize_ = 0;  // Old space used bytes after last compacting GC
    size_t gcHeadroom_ = 32ULL * 1024 * 1024;  // 32MB headroom before triggering compaction GC
    Interpreter* interpreter_ = nullptr;  // For root enumeration during GC
    std::vector<Oop*> roots_;
    std::vector<ObjectHeader*> rememberedSet_;  // Old-space objects with young pointers

    // Segregated free lists for old space (Spur-style)
    // Index 0: large free chunks (linked list)
    // Index 1-63: exact-size free chunks (size in 8-byte units = index)
    static constexpr size_t NumFreeLists = 64;
    std::array<ObjectHeader*, NumFreeLists> freeLists_ = {};
    uint64_t freeListsMask_ = 0;  // Bit i set if freeLists_[i] non-empty

    // Mark phase data structures
    std::vector<ObjectHeader*> markStack_;     // BFS worklist
    std::vector<ObjectHeader*> weakList_;      // Deferred weak objects
    std::vector<ObjectHeader*> ephemeronList_; // Deferred ephemerons

    std::unordered_set<uintptr_t> validObjectStarts_; // Valid object boundaries for mark validation

    // Finalization / mourning
    std::vector<Oop> mournQueue_;              // Objects needing finalization (ephemerons + weak)
    int pendingFinalizationSignals_ = 0;       // Count of signals to send post-GC
    size_t ephemeronEncounterCount_ = 0;       // Debug: ephemerons encountered during mark
    size_t ephemeronInactiveCount_ = 0;        // Debug: ephemerons with alive keys
    size_t ephemeronActiveCount_ = 0;          // Debug: ephemerons with dead keys

    // Context class index (cached for GC - Context objects need special
    // handling to avoid tracing garbage in unused stack slots)
    uint32_t contextClassIndex_ = 0;

    // Debug: track parent object during scanning (for BAD pointer diagnosis)
    ObjectHeader* currentScanParent_ = nullptr;
    size_t currentScanSlot_ = 0;

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

    /// Mark an object as remembered
    void rememberObject(Oop obj);

    // ===== FREE LIST HELPERS =====

    /// Initialize a region as a free chunk (classIndex=0).
    /// size includes header. Returns the chunk header.
    ObjectHeader* makeFreeChunk(uint8_t* addr, size_t size);

    /// Add a free chunk to the appropriate free list.
    void addToFreeList(ObjectHeader* chunk, size_t size);

    /// Try to allocate from free lists. Returns nullptr if no fit found.
    ObjectHeader* allocateFromFreeList(size_t size);

    /// Clear all free lists.
    void clearFreeLists();

    // ===== MARK PHASE =====

    /// Mark an Oop as reachable. If unmarked non-immediate non-perm, set mark
    /// and push onto markStack (or weakList/ephemeronList based on format).
    void markAndTrace(Oop oop);

    /// Process the mark stack until empty (BFS drain).
    void processMarkStack();

    /// Scan pointer fields of a marked object, calling markAndTrace on each.
    void scanPointerFields(ObjectHeader* obj);

    /// Return the number of pointer slots in an object (based on format).
    size_t pointerSlotsOf(ObjectHeader* obj) const;

    /// Process weak objects: nil out slots pointing to unmarked objects.
    /// Queues as mourners any weak object that had slots nilled.
    void processWeaklings();

    /// Scan ephemeron list; mark those whose keys are now marked.
    /// Returns true if any ephemerons became inactive (keys were marked).
    bool markInactiveEphemerons();

    /// Fire all remaining active ephemerons (dead keys): change format 5→1,
    /// queue as mourners, mark all their fields so values stay alive for mourning.
    void fireAllEphemerons();

    /// Mark hiddenRootsObj and all in-heap class table page objects.
    /// Without this, compaction treats the pages as dead and destroys them,
    /// corrupting the class table on save/reload.
    void markClassTablePages();

public:
    /// Sync the C++ classTable_ vector back to the in-heap class table pages
    /// inside hiddenRootsObj so that changes (new classes) are saved to disk.
    void syncClassTableToHeap();
private:

    /// Complete mark phase: mark from all roots, drain mark stack,
    /// process ephemerons, process weaklings.
    /// Returns the count of marked objects.
    size_t markPhase(bool skipEphemerons = false);

    // ===== COMPACT PHASE =====

    /// Saved first fields space (uses eden as scratch during full GC).
    struct SavedFirstFieldsSpace {
        Oop* start = nullptr;
        Oop* limit = nullptr;
        Oop* top = nullptr;
    };
    SavedFirstFieldsSpace savedFirstFieldsSpace_;

    /// Plan: compute forwarding addresses, save first fields.
    /// Returns false if scratch space overflowed (need another pass).
    bool planCompactSavingForwarders();

    /// Update all pointer fields in all live objects + all roots.
    void updatePointersAfterCompact();

    /// Slide objects to forwarding addresses, restore first fields, clear marks.
    void copyAndUnmark();

    /// Rebuild the free list from the gap at the end of old space.
    void rebuildFreeListAfterCompact();
};

// ===== TEMPLATE IMPLEMENTATIONS =====

template<typename Visitor>
void ObjectMemory::forEachMemoryRoot(Visitor&& visitor, bool includeClassTable) {
    // Special objects
    visitor(specialObjectsArray_);
    visitor(nilObject_);
    visitor(trueObject_);
    visitor(falseObject_);

    // Hidden heap roots needed for image saving
    if (freeListsObj_.isObject()) visitor(freeListsObj_);
    if (hiddenRootsObj_.isObject()) visitor(hiddenRootsObj_);

    // In-heap class table page objects (kept updated through GC so
    // syncClassTableToHeap can find them at their current addresses)
    for (auto& pageOop : classTablePages_) {
        if (pageOop.isObject() && pageOop.rawBits() != 0) {
            visitor(pageOop);
        }
    }

    // Class table entries — only during compaction (pointer updates),
    // NOT during mark phase. In Spur, class table entries are NOT strong
    // roots — anonymous/transient classes can be collected when unreachable.
    // During mark phase, classes survive only if referenced from live objects
    // (via classIndex in headers of live objects, or from other live references).
    if (includeClassTable) {
        for (size_t i = 1; i < classTable_.size(); ++i) {
            if (classTable_[i].isObject()) {
                visitor(classTable_[i]);
            }
        }
    }

    // Registered roots (interpreter stack pointers, etc.)
    for (Oop* root : roots_) {
        if (root) {
            visitor(*root);
        }
    }

    // Mourner queue entries must survive GC (they're needed by prim 172)
    for (auto& mourner : mournQueue_) {
        visitor(mourner);
    }
}

} // namespace pharo

#endif // PHARO_OBJECT_MEMORY_HPP
