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
#include "DebugVars.hpp"
#include "ShadowSlots.hpp"
#include <array>
#include <vector>
#include <cstdint>
#include <string>
#include <memory>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace pharo {

// Forward declaration
class Interpreter;

// Stable base of the class table, captured once at init (ObjectMemory.cpp).
// Used by the JIT `class` inline-prim (AsmjitT1.cpp tryPrimClass).
extern uint64_t g_classTableBase;

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
///
/// Two old-space knobs (Linux-style "configure at launch"):
///   - `oldSpaceInitialSize`: lower bound / GC-tuning hint.  Old-space starts
///     empty; this is the point at which we first consider triggering a full
///     GC (rather than just waiting for a physical OOM).
///   - `oldSpaceMaxSize`: virtual-address-space reservation via mmap.  The
///     OS lazy-commits pages only when we write to them, so reserving a
///     large range on iPhone 8 (1 GB physical) costs nothing — actual
///     physical use is capped by the platform's OOM kill (jetsam on iOS).
///
/// Platform defaults chosen so the VM works on small-memory devices without
/// any configuration AND scales up on desktops without a recompile.
struct MemoryConfig {
    // Defaults: 128 MB initial / 4 GB max.  Virtual-address reservation is
    // free on all Apple 64-bit targets; the OS lazy-commits pages and caps
    // actual physical use via jetsam (iOS) / OOM killer (macOS).  Override
    // via Info.plist (PharoInitialOldSpace / PharoMaxOldSpace) or env vars
    // (PHARO_INITIAL_OLD_SPACE / PHARO_MAX_OLD_SPACE).  Desktop users who
    // want a bigger ceiling just bump the env var at launch.
    size_t oldSpaceInitialSize = 128ULL * 1024 * 1024;
    size_t oldSpaceMaxSize     = 4ULL * 1024 * 1024 * 1024;
    // Back-compat: callers setting oldSpaceSize still work; init copies it
    // to oldSpaceMaxSize if max is zero.
    size_t oldSpaceSize = 0;
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
    /// Compaction passes a full GC needed.  More than one means the
    /// saved-first-fields scratch space could not hold the whole plan.
    size_t compactPasses = 0;
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

    /// Get the runtime size of the class table (number of valid indices).
    size_t classTableSize() const { return classTable_.size(); }

    /// Diagnostic: dump class-table self-consistency + orphaned-instance count.
    void dumpClassTableConsistency(const char* when);

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
        if (hdr->isForwarded()) {
            // Cap hit with the chain still forwarded: either a >10-deep
            // legitimate chained-become: (raise the cap) or a forwarding
            // CYCLE from heap corruption.  Loud, never silent (silent-cap
            // audit residue, closed 2026-07-04).
            static int fwdCapLog = 0;
            if (fwdCapLog++ < 20) {
                fprintf(stderr, "[FWD-CHAIN-CAP] followForwarded stopped at "
                        "10 hops still forwarded — oop=0x%llx\n",
                        (unsigned long long)oop.rawBits());
            }
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
        // Shadow-slot detector (PHARO_SHADOW_SLOTS) — track the write so
        // verify-on-read doesn't false-positive on interpreter/C++ stores.
        if (__builtin_expect(GET_DEBUG_BOOL(PHARO_SHADOW_SLOTS), 0)) {
            shadowStore(obj.rawBits(), index, value.rawBits(), 2);
        }
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

    /// Does this oop point at a header the collector may READ?
    ///
    /// markAndTrace already applies exactly this bound before touching an
    /// object; the weak/ephemeron passes did not, so a slot holding a value
    /// that is not in any heap region (an unrelocated saved-image address,
    /// say) reached `->isMarked()` and segfaulted the whole VM inside
    /// markPhase.  Same bound, one name, so the two cannot drift.
    inline bool isReadableHeapObject(Oop o) const {
        if (!o.isObject()) return false;
        auto p = reinterpret_cast<const uint8_t*>(o.asObjectPtr());
        return (p >= oldSpaceStart_ && p < oldSpaceFree_)
            || (p >= edenAllocBase_ && p < edenFree_)
            || (p >= permSpaceStart_ && p < permSpaceEnd_);
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

    /// Public hook for JIT helpers that perform their own slot writes
    /// (e.g. jit_rt_setter_write_barrier) and need to record the
    /// resulting old→young reference.  storePointer / storePointerUnchecked
    /// already remember internally; use this only when the slot write
    /// happened outside ObjectMemory.
    void rememberObjectPublic(Oop obj) { rememberObject(obj); }

    /// Check if an address is within the heap
    bool isValidHeapAddress(void* addr) const;

    /// Check if an Oop points to a valid object
    bool isValidObject(Oop obj) const;

    // ===== OBJECT MODIFICATION =====

    /// Pin an object so it won't be moved by GC
    void pinObject(Oop obj);

    /// Move an old-space object DOWN into a reclaimed gap and forward the
    /// original to it, so that a subsequent pin cannot strand the space below
    /// it.  Answers the new oop, or the original unchanged if no low chunk is
    /// available.  Only safe to call BEFORE anything takes the object's
    /// address -- which is exactly what pinning is for.  See
    /// docs/gc-oldspace-fragmentation-2026-08-22.md.
    Oop relocateToLowSpace(Oop original);

    /// Carve-once arena for PINNED objects, kept as low in old space as
    /// possible so a pin never strands the region below it.  Answers nullptr
    /// when disabled or exhausted, in which case callers fall back to bump
    /// allocation.  See docs/gc-oldspace-fragmentation-2026-08-22.md.
    ObjectHeader* allocatePinnedLow(size_t size);
  private:
    uint8_t* pinArenaStart_ = nullptr;
    uint8_t* pinArenaFree_  = nullptr;
    uint8_t* pinArenaEnd_   = nullptr;
  public:

    /// Make an object immutable
    void makeImmutable(Oop obj);

    /// Become: swap identity of two objects
    bool become(Oop obj1, Oop obj2);

    /// One-way become: all references to obj1 become references to obj2
    bool becomeForward(Oop obj1, Oop obj2);

    /// Batch one-way become. Semantically N calls to becomeForward, but with a
    /// single heap scan instead of one per pair — becomeForward walks every
    /// object, so doing it per pair costs O(pairs * heap).
    /// elementsForwardIdentityTo: with a few hundred pairs against a 740k-object
    /// heap is hundreds of millions of visits.
    ///
    /// Performs the same classTable_ redirect and installs the same forwarder
    /// safety net as becomeForward, and clears the shadow table once for the
    /// batch. The forwarders go in after the scan, so every pair sees the
    /// pre-become heap — Spur defines become as simultaneous, not sequential.
    /// @param map old oop raw bits -> replacement oop
    bool becomeForwardAll(const std::unordered_map<uint64_t, Oop>& map);

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
    /// Class-name histogram of the heap, largest byte-footprint first
    /// (old-space-exhaustion FATAL path + PHARO_HEAP_CENSUS per-fullGC).
    void dumpHeapCensus(int topN);

    /// skipEphemerons: if true, skip ephemeron firing and weak processing.
    /// Used by auto-compact GC to emulate scavenge behavior (don't mourn old-space objects).
    GCResult fullGC(bool skipEphemerons = false);

    /// Run a non-compacting mark-sweep GC (safe to call from allocations)
    void sweepGC();

    /// Check if compacting GC is needed at next safe point
    bool needsCompactGC() const { return needsCompactGC_; }
    void clearCompactGCFlag() { needsCompactGC_ = false; }
    bool needsScavenge() const { return needsScavenge_; }
    void clearScavengeFlag() { needsScavenge_ = false; }

    // ===== FINALIZATION / MOURNING =====

    /// Pop a mourner from the queue (for primitive 172)
    Oop popMourner() {
        if (mournQueue_.empty()) return nilObject_;
        Oop mourner = mournQueue_.back();
        mournQueue_.pop_back();
        return mourner;
    }

    /// Push a mourner back onto the queue (for native-drain re-queue path)
    void pushMourner(Oop mourner) { mournQueue_.push_back(mourner); }

    /// Check if there are mourners waiting
    bool hasMourners() const { return !mournQueue_.empty(); }
    size_t mournQueueSize() const { return mournQueue_.size(); }
    /// Read-only view for GC-pin forensics (PHARO_PIN_DIAG)
    const std::vector<Oop>& mournQueueEntries() const { return mournQueue_; }

    /// Get/clear pending finalization signal count
    int pendingFinalizationSignals() const { return pendingFinalizationSignals_; }
    void clearPendingFinalizationSignals() { pendingFinalizationSignals_ = 0; }

    /// Register a root for GC (interpreter stack, etc.)
    void addRoot(Oop* root);

    // Compaction-plan counters (PHARO_GC_LOG diagnostic only).
    size_t planSeen_ = 0, planMarked_ = 0, planUnmarked_ = 0, planPinned_ = 0;
    size_t planInPlace_ = 0, planToMove_ = 0, planMarkedBytes_ = 0;
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

    /// Fast path: collect all objects with the given classIndex.
    /// Avoids std::function dispatch overhead of allObjectsDo by inlining
    /// the class check directly into the scan loop. Accelerates primitive
    /// 177 (allInstances:) and pointersTo: for small classes.
    void collectInstancesOfClass(uint32_t classIndex, std::vector<Oop>& out);

    /// As collectInstancesOfClass, but the YOUNG allocation area only.
    void collectInstancesOfClassInEden(uint32_t classIndex, std::vector<Oop>& out);

    /// Templated iteration that inlines the callback (no std::function overhead).
    /// For hot paths like primitiveFindRoots. F must take (Oop).
    template <typename F>
    void allObjectsDoInline(F&& callback) {
        auto scanRegion = [&](uint8_t* start, uint8_t* end) {
            uint8_t* scan = start;
            while (scan < end) {
                uint64_t* wordPtr = reinterpret_cast<uint64_t*>(scan);
                uint64_t word = *wordPtr;
                if (word == 0) {
                    scan += 8;
                    while (scan < end) {
                        wordPtr = reinterpret_cast<uint64_t*>(scan);
                        if (*wordPtr != 0) break;
                        scan += 8;
                    }
                    if (scan >= end) break;
                    word = *wordPtr;
                }
                uint64_t* headerPtr = wordPtr;
                uint8_t topByte = static_cast<uint8_t>((word >> 56) & 0xFF);
                if (topByte == 255 && scan + 8 < end) {
                    uint64_t nextWord = *(wordPtr + 1);
                    uint8_t nextNumSlots = static_cast<uint8_t>((nextWord >> 56) & 0xFF);
                    if (nextNumSlots == 255) {
                        uint64_t overflowCount = (word << 8) >> 8;
                        size_t remaining = end - scan;
                        size_t neededSize = 8 + 8 + overflowCount * 8;
                        if (overflowCount >= 255 && neededSize <= remaining) {
                            headerPtr = wordPtr + 1;
                        } else {
                            scan += 16;
                            continue;
                        }
                    }
                }
                ObjectHeader* obj = reinterpret_cast<ObjectHeader*>(headerPtr);
                size_t size = obj->totalSize();
                size_t remaining = end - scan;
                if (size == 0 || size > remaining) {
                    scan += 8;
                    continue;
                }
                callback(oopFromPointer(obj));
                scan += size;
            }
        };
        scanRegion(permSpaceStart_, permSpaceEnd_);
        scanRegion(oldSpaceStart_, oldSpaceFree_);
        scanRegion(edenAllocBase_, edenFree_);
    }

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
    size_t gcCount() const { return gcCount_; }

    // ===== LOW-LEVEL ACCESS (for image loader) =====

    /// Get raw memory pointers (use with caution)
    uint8_t* oldSpaceStart() const { return oldSpaceStart_; }
    uint8_t* oldSpaceEnd() const { return oldSpaceEnd_; }
    uint8_t* newSpaceEnd() const { return newSpaceEnd_; }
    uint8_t* oldSpaceFree() const { return oldSpaceFree_; }
    uint8_t* newSpaceStart() const { return newSpaceStart_; }
    // Diagnostic: is this oop a plausibly LIVE young object (inside the
    // active eden allocation window, i.e. below the bump pointer)?  An
    // eden-range pointer AT/ABOVE edenFree_ is stale by construction.
    bool isLiveYoung(Oop o) const {
        if (!o.isObject()) return false;
        uint8_t* p = reinterpret_cast<uint8_t*>(o.asObjectPtr());
        return p >= edenAllocBase_ && p < edenFree_;
    }
    /// Valid ONLY between the end of markPhase's fixpoint and the start
    /// of compaction/sweep (mark bits final): true iff o is an old-space
    /// object this GC cycle decided is dead.  Objects outside old space
    /// (young survivors, perm) and immediates answer false — this cycle
    /// doesn't reclaim them.  Used by Interpreter::purgeDeadCacheRoots().
    bool isDeadAfterMark(Oop o) const {
        if (!o.isObject()) return false;
        uint8_t* p = reinterpret_cast<uint8_t*>(o.asObjectPtr());
        if (p < oldSpaceStart_ || p >= oldSpaceFree_) return false;
        return !o.asObjectPtr()->isMarked();
    }
    uint8_t* permSpaceStart() const { return permSpaceStart_; }
    uint8_t* permSpaceEnd() const { return permSpaceEnd_; }

    /// Get free bytes in old space
    size_t freeOldSpaceBytes() const {
        return static_cast<size_t>(oldSpaceEnd_ - oldSpaceFree_);
    }

    // ---- Low-space circuit breaker (prim 125) -------------------------
    //
    // The threshold has to be tested WHERE OLD SPACE IS CONSUMED, not on a
    // bytecode counter.  Sampling it from the interpreter's per-1024-bytecode
    // checkpoint cannot work, and the 2026-09-02 arm64 sweep is the proof: a
    // storm ran the heap from 12 GB of free old space down to 16 bytes and
    // the breaker never fired once.  Old space does not drain smoothly --
    // essentially all of it goes out through scavenge tenure, in steps of up
    // to one full eden (22 MB on that image).  Pharo arms the threshold at
    // `SmalltalkImage>>lowSpaceThreshold` = 400000 bytes, so the sampled
    // check only sees the window if a 22 MB step happens to land inside the
    // last 400 KB: 400000/22003584 ~= 1.8% of the time.  The other 98.2% the
    // next tenure overruns oldSpaceEnd_ and aborts before any checkpoint runs.
    //
    // So: arm it here, test it at the two sites that advance oldSpaceFree_,
    // and let the interpreter consume a latched flag.  The effective
    // threshold is max(image threshold, min(one eden, reservation/16)) -- the
    // invariant being that the image gets its interrupt while at least one
    // more worst-case scavenge can still be absorbed.  A 400 KB threshold
    // cannot express that when the allocation granularity above it is 22 MB.
    void armLowSpaceThreshold(size_t bytes) {
        lowSpaceThresholdBytes_ = bytes;
        if (bytes == 0) lowSpaceCrossed_ = false;
    }
    /// Peek at the latch.  Cleared by disarming (armLowSpaceThreshold(0)),
    /// which is what the interpreter does once it has actually signalled --
    /// so a crossing with no semaphore registered yet is not thrown away.
    bool lowSpaceCrossed() const { return lowSpaceCrossed_; }

    /// Set the free pointer (for image loading)
    void setOldSpaceFreePointer(uint8_t* ptr) {
        oldSpaceFree_ = ptr;
        // Carve the pin arena the moment the image is in place, while
        // oldSpaceFree_ is still just past the loaded image.  Carving it
        // lazily on first demand put it at +199,499 KB on a NeoJSON load --
        // ~195 MB in, above most of what it was supposed to sit below -- which
        // is why the first version saved only ~6% instead of reclaiming the
        // stranded 146 MB.  Cheap and idempotent; a no-op unless
        // PHARO_PIN_RELOCATE is on.
        ensurePinArena();
    }
    void ensurePinArena();

    /// Address of the survivor-start cell.  NOTE the JIT does NOT come through
    /// here: the inline-alloc emit (PHARO_T1_INLINE_NEW_ASM) bakes
    /// `g_jitEdenFreeCell` / `g_jitSurvivorStartCell`, published in the
    /// constructor.  The matching `edenFreeCellAddr()` existed alongside this
    /// one with a comment saying the emit used it, and had no callers at all;
    /// removed 2026-09-02.
    uint8_t** survivorStartCellAddr() { return &survivorStart_; }

    /// Public young-gen bump for the JIT new fast-path (jit_rt_new_prim).
    /// Allocates `size` bytes in eden and returns the raw header pointer, or
    /// nullptr if young-gen is disabled or eden is full — in which case it sets
    /// needsScavenge_ so a scavenge runs at the NEXT SAFE POINT (never mid-call,
    /// so it is GC-safe to call from JIT-emitted code). Mirrors allocateRaw's
    /// Space::New case. The caller must fall back to old space on nullptr.
    ObjectHeader* allocateRawYoung(size_t size) {
        if (!enableYoungGen_) return nullptr;
        if (edenFree_ + size <= edenAllocLimit_) {
            ObjectHeader* obj = reinterpret_cast<ObjectHeader*>(edenFree_);
            edenFree_ += size;
            return obj;
        }
        needsScavenge_ = true;
        return nullptr;
    }

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


private:
    // Memory regions
    uint8_t* permSpaceStart_ = nullptr;
    uint8_t* permSpaceEnd_ = nullptr;
    uint8_t* oldSpaceStart_ = nullptr;
    uint8_t* oldSpaceEnd_ = nullptr;     // oldSpaceStart_ + oldSpaceMaxSize (virtual ceiling)
    uint8_t* oldSpaceFree_ = nullptr;    // Next allocation in old space
    uint8_t* oldSpaceInitialTarget_ = nullptr;  // GC trigger: first point past which full GC fires
    bool oldSpaceUseMmap_ = false;       // true if old space was allocated with mmap
    size_t oldSpaceMmapSize_ = 0;        // Size of mmap'd region
    uint8_t* newSpaceStart_ = nullptr;
    uint8_t* newSpaceEnd_ = nullptr;
    uint8_t* edenStart_ = nullptr;
    uint8_t* edenFree_ = nullptr;       // Next allocation in eden
    // Live eden allocation window.  Normally [edenStart_, survivorStart_);
    // under PHARO_EDEN_ROTATE it is one half of eden, alternating per
    // scavenge with the retired half page-protected (stale-young-ref
    // detector).  ALL live-eden walks and bounds checks use these, never
    // edenStart_/survivorStart_ directly.
    uint8_t* edenAllocBase_ = nullptr;
    uint8_t* edenAllocLimit_ = nullptr;
    uint8_t* survivorStart_ = nullptr;

    // Low-space breaker state; see armLowSpaceThreshold().
    size_t lowSpaceThresholdBytes_ = 0;   // 0 = disarmed
    bool   lowSpaceCrossed_ = false;      // latched at the crossing

    /// Called wherever oldSpaceFree_ advances.  Latches the crossing so the
    /// interpreter can signal TheLowSpaceSemaphore at its next safe point --
    /// signalling from inside a scavenge is not an option.
    void noteOldSpaceAdvance() {
        if (__builtin_expect(lowSpaceThresholdBytes_ == 0, 1)) return;
        // One worst-case scavenge's worth of headroom, but never more than a
        // sixteenth of the whole reservation: `newSpaceSize` is settable
        // (PHARO_NEWSPACE_MB, a bisect knob), and a big eden against a small
        // old space would otherwise turn "nearly exhausted" into "a quarter
        // used" and fire on every run.  A bisect knob must not quietly change
        // low-space semantics.  The image's own threshold always wins if it
        // asks for more than either.
        size_t edenCapacity = static_cast<size_t>(edenAllocLimit_ - edenAllocBase_);
        size_t reservation  = static_cast<size_t>(oldSpaceEnd_ - oldSpaceStart_);
        size_t headroom     = edenCapacity < reservation / 16 ? edenCapacity
                                                              : reservation / 16;
        size_t effective    = lowSpaceThresholdBytes_ > headroom
                                  ? lowSpaceThresholdBytes_ : headroom;
        if (freeOldSpaceBytes() < effective) lowSpaceCrossed_ = true;
    }

    // Class table.
    //
    // 4M-slot vector indexed by class's 22-bit identity hash
    // (Spur convention: classIndex == identityHash).  Hot reads via
    // classAtIndex() get one bounds check + one indexed load —
    // significantly faster than any hash-table alternative.
    //
    // Class objects' identity hashes are assigned SEQUENTIALLY by
    // registerClass() (matching Cog's enterIntoClassTable: behaviour);
    // identityHashOf() detects class objects via knownMetaclassIndices_
    // and routes them through registerClass() rather than generating
    // a random LCG hash.  This is the truly correct fix for the
    // class-vs-non-class hash collision: random hashes are the
    // bug, not the storage container.  See
    // `docs/class-table-container-analysis.md`.
    std::vector<Oop> classTable_;
    uint32_t nextClassIndex_ = 1;  // Updated during image loading to be past highest used index

    // Set of class table indices known to be metaclasses (i.e., classes
    // whose instances are themselves classes).  Populated as classes
    // are registered: registerClass(C) inserts classOf(C)'s index.
    // Used by identityHashOf() to detect class instances and route
    // them to sequential index allocation instead of random LCG hashing.
    std::unordered_set<uint32_t> knownMetaclassIndices_;

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
    bool needsCompactGC_ = false;  // Set by allocator when compaction needed at safe point
    bool needsScavenge_ = false;   // Set by allocator when eden is full

public:
    // Young-gen allocation enable — default off during image load
    // (so image-load allocations go straight to old), flipped on
    // after the interpreter is ready.
    bool enableYoungGen_ = false;
private:
    size_t lastCompactedSize_ = 0;  // Old space used bytes after last compacting GC
    // GC headroom: how much old space can grow beyond lastCompactedSize
    // before requesting a compaction.  Tuned 2026-05-27 from profiling:
    //
    //   32MB  (original):  64 fullGCs/run, 5851ms GC, 6738ms total (85%)
    //   256MB:             16 fullGCs/run, 1727ms GC, 5576ms total (31%)
    //   512MB:              8 fullGCs/run, 1080ms GC, 5279ms total (20%)
    //   1GB:                4 fullGCs/run,  773ms GC, 5108ms total (15%)
    //
    // Picked 512MB as the sweet spot — 1GB saves only 170ms more for
    // 2x memory pressure.  Virtual address is reserved up-front (4GB
    // lazy commit), so this only affects when GC fires, not physical
    // memory usage at idle.  Tunable via env if needed.
    size_t gcHeadroom_ = 512ULL * 1024 * 1024;
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
    // Keys of ephemerons FIRED this GC cycle: rescued (kept alive for the
    // mourn queue) but NOT strongly reachable — weak slots referencing them
    // must still be nil'd (stock Spur nils weaklings before retracing fired
    // keys).  Without this, a WeakMessageSend whose receiver is also a
    // FinalizationRegistry key kept DELIVERING for one extra GC cycle
    // (WeakAnnouncerTest>>testWeakObject warm flake, 2026-07-03).
    std::unordered_set<ObjectHeader*> ephemeronRescuedKeys_;

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

    // Debug: track parent object during scanning (for BAD pointer diagnosis).
    // Disabled in production builds — never read by any code; the per-slot
    // store was visible in GC profiles as ~6% of fullGC time.
#if PHARO_HOT_PATH_DIAG
    ObjectHeader* currentScanParent_ = nullptr;
    size_t currentScanSlot_ = 0;
#endif

    // Statistics
    size_t bytesAllocated_ = 0;
    size_t gcCount_ = 0;
    size_t totalGCTime_ = 0;
public:
    // Per-GC-type counters/timing for perf analysis
    size_t scavCount_ = 0;
    size_t scavTime_ = 0;
    size_t fullGCCount_ = 0;
    size_t fullGCTime_ = 0;
    size_t sweepCount_ = 0;
    size_t sweepTime_ = 0;
private:

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
    /// Build a free chunk (classIndex 0) at `addr`.  `zeroBody` writes the
    /// whole body; pass false when the memory is ALREADY a free chunk's body
    /// (splitting a chunk), because zeroing it there is O(chunk size) per
    /// allocation and the free list's largest chunk is the whole
    /// post-compaction gap -- measured as a 5x slowdown on a package load.
    ObjectHeader* makeFreeChunk(uint8_t* addr, size_t size, bool zeroBody = true);

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

public:
    /// Diagnostic (PHARO_HEAP_CHECK): walk every object; flag any pointer slot
    /// whose target is out-of-heap (dangling) or has an invalid classIndex
    /// (free chunk / garbage). Returns true if clean. For the test-execution
    /// heap-corruption hunt — localizes structural corruption deterministically.
    /// Public so the Interpreter's wild-write detector (PHARO_HEAP_SCAN_EVERY)
    /// can call it from the per-checkpoint hook.
    bool checkHeapIntegrity(const char* when);
private:

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

    /// [HEAP-WATCH] line budget for the current GC (PHARO_WATCH_HEAP_CLASSIDX).
    int heapWatchLogged_ = 0;

    /// Annotate a Context parent for the heap probes: name the activation and
    /// say whether the holding slot is inside the live region (6..6+stackp-1)
    /// or residue above stackp.  A Context is the one parent class where
    /// "reachable" does not imply "live" — pointerSlotsOf deliberately traces
    /// a Context's whole slot array — so the distinction is the entire answer
    /// to "why wasn't this collected?".  Diagnostic paths only.
    std::string describeContextSlot(Oop parent, uint32_t slot) const;

    /// PHARO_WEAK_SURVIVOR_PATHS: record each object's first-reaching parent
    /// during mark, so processWeaklings can print why a weak referent lived.
    bool recordMarkParents_ = false;
    bool logWeakSurvivorClasses_ = false;
    const char* weakPathFilter_ = nullptr;  // PHARO_WATCH_HEAP_CLASS narrows [WEAK-ALIVE] to one referent class
    struct MarkParent { uint64_t parent; uint32_t slot; };
    std::unordered_map<uint64_t, MarkParent> markParent_;

    /// Which forEachRoot category first visited each root object this mark.
    /// A chain that ends in "<- ROOT" otherwise stops exactly one hop short of
    /// the answer; with this it ends in "<- ROOT(saved-frames)".
    std::unordered_map<uint64_t, const char*> markRootTag_;

    /// Watched-class instances seen during this mark (PHARO_WATCH_HEAP_CLASS).
    /// Their full parent chains are printed after the fixpoint, when
    /// markParent_ is complete — the immediate parent alone stops one hop
    /// short of the root that is actually doing the retaining.
    std::vector<uint64_t> heapWatchHits_;

    /// Print "<- Parent(0x..)[slot]" back to a root for one object, using the
    /// parents recorded during this mark.  Shared by [WEAK-ALIVE] and
    /// [HEAP-CHAIN]; annotates Context hops with describeContextSlot.
    void printMarkParentChain(uint64_t oop);

    /// After the mark fixpoint: print a full chain for every watched-class
    /// instance that survived.  Requires PHARO_WEAK_SURVIVOR_PATHS for the
    /// parent map and PHARO_WATCH_HEAP_CLASS to pick the class.
    void reportWatchedChains();

    // ===== COMPACT PHASE =====

    /// Saved first fields space (uses eden as scratch during full GC).
    struct SavedFirstFieldsSpace {
        Oop* start = nullptr;
        Oop* limit = nullptr;
        Oop* top = nullptr;
    };
    SavedFirstFieldsSpace savedFirstFieldsSpace_;

    /// One plan/update/copy pass of the compactor.  The saved-first-fields
    /// scratch space is finite (one Oop per moving object, carved out of new
    /// space), so a heap with more movers than the scratch holds is compacted
    /// in several passes: each pass relocates the prefix it could plan and
    /// leaves the rest for the next one.
    struct CompactPass {
        uint8_t* srcStart = nullptr;  ///< in:  first source object of this pass
        uint8_t* srcEnd = nullptr;    ///< out: first object NOT planned (exclusive)
        uint8_t* dstStart = nullptr;  ///< in:  destination for srcStart
        uint8_t* dstEnd = nullptr;    ///< out: destination after the planned prefix
        bool complete = false;        ///< out: true when srcEnd reached oldSpaceFree_
    };

    /// Plan: compute forwarding addresses and save first fields for as many
    /// objects from pass.srcStart on as the scratch space holds.  Sets
    /// pass.complete when the whole remaining heap was planned; otherwise
    /// pass.srcEnd / pass.dstEnd say where the next pass must resume.
    void planCompactSavingForwarders(CompactPass& pass);

    /// Update all pointer fields in all live objects + all roots.
    /// Runs once per compaction pass, between that pass's plan and its copy,
    /// while that pass's forwarders are installed.  Under PHARO_JIT_ENABLED it
    /// also rekeys the Sista method->fn cache through those same forwarders,
    /// which is why it CANNOT be hoisted out of the pass loop: after the final
    /// copy every grey bit is clear and follow() would be the identity, leaving
    /// every cache key pointed at where its method used to be.  Running it once
    /// per pass composes correctly — see the comment at the loop in fullGC.
    void updatePointersAfterCompact();

    /// Slide the objects planned by @a pass to their forwarding addresses and
    /// restore their first fields.  Clears mark bits only when @a clearMarks:
    /// a partial pass must leave them set so the objects it relocated are still
    /// scanned by the next pass's updatePointersAfterCompact().
    /// Returns the number of objects actually relocated (pinned objects and
    /// objects that were already at the destination are not counted).
    size_t copyAndUnmark(const CompactPass& pass, bool clearMarks);

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

    // Class table entries AND in-heap page objects — only during pointer
    // updates (compaction/scavenge), NOT during mark.  In Spur, class table
    // entries are NOT strong roots — anonymous/transient classes are
    // collected when unreachable; classes survive the mark only via live
    // references.  The PAGE objects must not be visited during mark either:
    // markPhase's visitor is markAndTrace, which would DEEP-TRACE the pages
    // and thereby strongly mark every class they contain, silently pinning
    // dead/obsolete classes forever (ReleaseTest testObsoleteClasses,
    // catalog residue 2026-07-06).  Page LIVENESS during mark is guaranteed
    // separately by markClassTablePages() (shallow mark, page 0 deep).
    if (includeClassTable) {
        for (auto& pageOop : classTablePages_) {
            if (pageOop.isObject() && pageOop.rawBits() != 0) {
                visitor(pageOop);
            }
        }
        for (size_t i = 1; i < classTable_.size(); ++i) {
            if (classTable_[i].isObject()) {
                visitor(classTable_[i]);
            }
        }
    }

    // Registered roots (interpreter stack pointers, etc.).  EMPTY in practice:
    // nothing in this VM calls addRoot(), so this loop is a no-op today.  The
    // hook is kept because it is the right place to register a C++-side Oop
    // that must survive GC; if you find yourself pinning one some other way,
    // use this.
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
