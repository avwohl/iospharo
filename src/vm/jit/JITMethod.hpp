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

// ===== INLINE CACHE LAYOUT (shared between compiler, runtime, stencils, GC) =====
//
// Each IC site has IC_ENTRIES_PER_SITE entries laid out as:
//   [key0, method0, extra0, key1, method1, extra1, ..., selectorBits, hitCount]
// keys are classIndex (or tagged immediate marker, not an Oop).
// methods are CompiledMethod Oop (needs GC update).
// extras are flags + slot/address (not an Oop).
// selectorBits is the site's selector Symbol Oop (needs GC update),
// lives at slot IC_SELBITS_SLOT (immediately after all entries).
// hitCount is a per-site warm counter — incremented on each IC hit; when it
// crosses the recompile threshold the site's owner method is queued for
// recompile.  Lives at slot IC_HITCOUNT_SLOT (after selectorBits).  Added
// 2026-05-02 PM as the foundation for cross-path warm-IC detection (see
// docs/jit-multiweek-work.md item #2).
//
// These constants MUST match the stencil source (stencils.cpp) —
// changing them requires regenerating stencils via extract_stencils.py.
// Stencils only access icData[0..18] (the existing layout); the new
// hitCount slot at offset 152 is invisible to them in this initial
// step — counter increments will be wired in in a subsequent commit.
static constexpr uint32_t IC_ENTRIES_PER_SITE = 6;
static constexpr uint32_t IC_BYTES_PER_ENTRY  = 24;  // key(8) + method(8) + extra(8)
static constexpr uint32_t IC_BYTES_PER_SITE   =
    IC_ENTRIES_PER_SITE * IC_BYTES_PER_ENTRY + 16;   // +8 selectorBits, +8 hitCount
static constexpr uint32_t IC_SELBITS_SLOT     =
    IC_ENTRIES_PER_SITE * 3;                         // slot index of selectorBits
static constexpr uint32_t IC_HITCOUNT_SLOT    =
    IC_SELBITS_SLOT + 1;                             // slot index of hitCount

// IC extras primKind (bits 52:48) for prim 111 #class — the B6 inline
// snippet (classTable[classIndex] load, callee-less).  Shared by the
// inlinePrimKind writer, the upgradeICToJ2J classifier, and the T1
// dispatch compares (was a bare 24 at several sites).
static constexpr uint8_t kPrimKindClass = 24;

// ===== JIT METHOD STATS (side-table) =====
//
// Frequently-mutated per-method counters that USED to live inside JITMethod
// (in MAP_JIT) but were moved to a heap-allocated side-table on
// 2026-04-26 so the W^X discipline can move to default-X without
// per-write APRR flips.  Allocated once per JITMethod by CodeZone::
// allocate, freed by CodeZone::freeMethod.  Lives in regular heap
// (always writable) — no W^X management needed.
struct JITMethodStats {
    uint32_t executionCount;     // Incremented on each entry (for hot method detection)
    uint32_t lastUsedEpoch;      // Updated on entry; compared against global epoch for LRU
    uint8_t  j2jDepthLimit;      // Per-method stencil J2J depth limit (default 2, max 8)
    uint8_t  j2jCleanRuns;       // Consecutive no-bail stencil re-entries (promotes at 8)
    uint8_t  lateSpecCount;      // Specialization-bearing IC bits added since last recompile.
                                 // upgradeICToJ2J bumps this on a tier=2 caller method when
                                 // a previously-empty slot fills with classification bits;
                                 // when it crosses kLateSpecRecompileThreshold the method
                                 // is queued for a one-shot re-recompile so
                                 // applyICSpecialization can pick up the new bits.
    uint8_t  flags;              // bit 0 = LATE_SPEC_RECOMPILED_ONCE (one-shot cap)
                                 // bit 1 = RECOMPILE_FAILED (don't retry recompile)
};
static_assert(sizeof(JITMethodStats) == 12, "JITMethodStats layout");

// Threshold for late-spec re-recompile.  Weight scheme in upgradeICToJ2J:
//   +2 per high-value bit (BLOCK_VALUE_BIT / multi-slot / returnsLiteral)
//   +1 per plain J2J_ENTRY_BIT classification
// A single 2-arg block-value classification (sort's mergeFirst case) is enough
// to trip the threshold; plain J2J fills need ≥2 to avoid noise.
static constexpr uint8_t kLateSpecRecompileThreshold = 2;
static constexpr uint8_t kLateSpecRecompiledOnce     = 0x01;
// bit 1: the profile-guided recompile (compile() with `old` for IC data) bailed.
// Set when recompile() returns null so tryExecute's recompile trigger doesn't
// re-attempt the full asmjit pipeline on every subsequent activation — a method
// whose initial T1 compile succeeded but whose profile-guided recompile bails
// would otherwise re-run compile() (early-bail → compilationsFailed_++) forever
// (~5000/sec asmjit thrash on reflective/Fuel workloads).
static constexpr uint8_t kRecompileFailed            = 0x02;

// ===== PMS SEND-SITE PATCH MAP RECORD =====
//
// Patched-monomorphic-send bookkeeping (docs/patched-ic-design.md §9).
// One record per IC site, in-zone after selBitsArray.  All other
// per-site link state is derived (heap IC slot 0 + decoding the patched
// words) — this record is the only second artifact, and it is
// write-once at compile except `flags`.
struct SendSitePatch {
    uint32_t keyMovzOffset;    // code offset of W0 (key movz); W1=+4, cmp=+8,
                               // b.ne=+12, W2=+16.  0 = site not patchable
                               // (emit fell back to the legacy shape)
    uint32_t tailOffset;       // T: start of the linked-J2J tail (W3=+0,
                               // W4=+4, W5=+8).  0 = no tail emitted
    uint32_t tailBranchOffset; // W6: the terminal direct `b` (varies with
                               // the nil-fill shape)
    uint32_t flags;            // bit 0 = linked (link truth readable
                               // without disassembly); rest telemetry
};
static_assert(sizeof(SendSitePatch) == 16, "SendSitePatch layout");

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
    // codeSize is misnamed for historical reasons.  It is the size of the
    // ENTIRE payload after this header (machine code + bcToCode table +
    // selBitsArray).  Used by `findMethodByPC` as the method's full
    // footprint.  Pre-2026-05-03 the payload also ended with the IC
    // zone (numICEntries * IC_BYTES_PER_SITE), but ICs are now in
    // heap-side `icBuffer` — do NOT compute icStart from codeSize.
    // Use icBuffer (or icZoneStart()) instead.
    uint32_t  codeSize;
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
    bool        isBlock;            // Compiled from a CompiledBlock (FullBlockClosure)
    bool        pinned;             // Temporarily protect from eviction (live on stack / j2jPool)
    uint8_t     maxRecvFieldIndex;  // Max receiver slot index accessed (for bounds checking)
    bool        isSpliceTarget;     // SistaRuntime registered a splice for this method —
                                    // stencils must skip executionCount bumps to avoid the
                                    // T1-vs-Sista race (project_t1_vs_sista_race).
    bool        hasNLR;             // F3-NL3: contains 0x5D/0x5E (BlockReturnNil/Top).
                                    // Computed at JIT-compile time, checked by
                                    // jit_rt_inline_block_value_prep to refuse BV
                                    // inline for blocks with non-local return.
    bool        canBailMidMethod;   // Emits ExitMustBool (and similar) mid-body.  The chain-loop
                                    // inline-activate path at Interpreter.cpp:18807-18809 invokes
                                    // a callee without pushing a C++ frame; a mid-method bail
                                    // through that path returns false into the caller's frame
                                    // and infinite-loops.  When this flag is set the chain loop
                                    // falls back to activateMethod-recursive activation, which
                                    // does push a frame.
    bool        isStubOnEntry;      // asmjit-T1 stub: the JIT code body is
                                    // just `mov [rdi+OFF_EXIT], ExitSend; ret`
                                    // (compiled when bytecodes are unsupported or
                                    // PHARO_ASMJIT_T1_STUB_ONLY).  Callers running
                                    // tryExecute on a stub block get a bogus
                                    // ExitSend with stale send-state from cull:'s
                                    // outer send → that's the (3+4) corruption.
                                    // The chain-loop block-resume path checks
                                    // this and bails to interp if set.
    bool        canSkipJ2JSave;     // Eδ.1 (2026-05-24): "no-bail tier-2"
                                    // callable.  Set true when the JIT-compiled
                                    // method neither bails mid-method nor performs
                                    // any send (numICEntries == 0).  Callers can
                                    // skip pushing a J2J save before calling such
                                    // a method — Eδ.2 will use this to emit a
                                    // direct br/ret pair at the IC HIT inline-J2J
                                    // site, saving ~12-15 instructions per call.

    // pad to 4-byte-align totalSize

    // --- Sizes / offsets ---
    uint32_t  totalSize;          // Total allocation size including header + code + IC entries
    uint32_t  bcToCodeTableOffset; // Offset from codeStart() to uint32_t[numBytecodes+1] table

    // --- Navigation ---
    JITMethod* nextInZone;        // Next method in code zone (for iteration/compaction)
    JITMethod* prevInZone;        // Previous method in code zone

    // --- Side-channel selectorBits ---
    // Task #41 (2026-04-23): out-of-band copy of each IC site's selector
    // Oop.  Lives after the IC data in the same allocation.  At IC miss,
    // the megacache lookup reads from here instead of icSlots[18] — which
    // is memset to zero on GC by recoverAfterGC for stencil-probe safety.
    // forEachRoot visits this array so the Oops are updated across GC.
    // Offset from codeStart() to uint64_t[numICEntries] array.  Zero if
    // the method has no send sites.
    uint32_t  selBitsArrayOffset;
    // --- PMS patch map (docs/patched-ic-design.md §9) ---
    // Offset from codeStart() to SendSitePatch[numICEntries], in-zone
    // after selBitsArray (same lifetime: written during compile's W
    // window, read-only after, NOT GC-visited — no oops).  Zero if the
    // method has no patchable sites.  Reuses the old _pad_76 padding —
    // zero layout growth.
    uint32_t  patchMapOffset;

    // --- Mutable stats (side-table) ---
    // Lives in regular heap so writes don't require W^X flips.
    // Allocated by CodeZone::allocate, freed by CodeZone::freeMethod.
    JITMethodStats* stats;

    // --- IC entries (heap-allocated side-table) ---
    // 2026-05-03: Moved out of MAP_JIT into regular heap so per-fill
    // pthread_jit_write_protect_np flips are no longer needed.  Lays
    // numICEntries × IC_BYTES_PER_SITE bytes at icBuffer.  Allocated
    // by CodeZone::allocate(), freed by freeMethod(), copied across
    // recompile (compile() with oldVersion).  selBitsArray stays in
    // MAP_JIT because it's read-only after compile (only zeroed by
    // GC).  See `jit_rt_fill_ic` re-enablement notes.
    uint64_t* icBuffer;

    // Cached bytecode start address (= compiledMethodOop + (2 + numLits) * 8).
    // Populated in compileViaAsmjit after construction.  Lets the inline-J2J
    // emit skip the 7-instruction computation per send site (load methodObj
    // + load methodHeader + mask numLits + add 1 + lsl 3 + add 8 + add).
    // Sourced from immutable fields (compiledMethodOop + methodHeader) so
    // no invalidation needed.
    uint64_t  bcStartCache;

    // --- V2 resume-override side table (send-resume fix, 2026-06-11) ---
    // Offset from codeStart() to: uint32_t count, then count pairs of
    // {bcOff, continuationCodeOff}.  The continuation (resumeAfterCall)
    // assumes the retval-in-x1 JIT_RESUME_CALL protocol; bcToCode now
    // stays PLAIN (next-bytecode labels) so non-retval resume entries
    // (tryResume, interp re-entry) are safe.  Only retval-carrying
    // consumers use codeOffsetForResume().  0 = no table.
    // APPENDED at struct end: the stencil-tier JITMethod_mirror
    // (stencils.cpp) models the first 96 bytes by fixed offsets —
    // never insert mid-struct.
    uint32_t  resumeOvOffset;
    uint32_t  _pad_resv;

    // --- Accessors ---

    // Pointer to the start of machine code (immediately after header)
    uint8_t* codeStart() {
        return reinterpret_cast<uint8_t*>(this) + sizeof(JITMethod);
    }

    const uint8_t* codeStart() const {
        return reinterpret_cast<const uint8_t*>(this) + sizeof(JITMethod);
    }

    // Pointer to the inline cache entries (after machine code).
    // NOTE (2026-05-03): legacy accessor, kept for ICEntry-shape
    // consumers.  The actual flat IC layout used by stencils +
    // applyICSpecialization + upgradeICToJ2J + patchJITICAfterSend
    // lives in the heap-side `icBuffer` field; use
    // `icBuffer + sendIdx * (IC_BYTES_PER_SITE / 8)` for IC-slot access.
    ICEntry* icEntries() {
        return reinterpret_cast<ICEntry*>(icBuffer);
    }

    const ICEntry* icEntries() const {
        return reinterpret_cast<const ICEntry*>(icBuffer);
    }

    // Flat per-site IC pointer used by stencils and runtime IC code.
    uint8_t* icSiteAt(uint32_t siteIdx) {
        return reinterpret_cast<uint8_t*>(icBuffer)
             + siteIdx * IC_BYTES_PER_SITE;
    }
    const uint8_t* icSiteAt(uint32_t siteIdx) const {
        return reinterpret_cast<const uint8_t*>(icBuffer)
             + siteIdx * IC_BYTES_PER_SITE;
    }
    // First byte of the IC zone for this method.  Useful when iterating
    // sites via offset arithmetic.
    uint8_t* icZoneStart() {
        return reinterpret_cast<uint8_t*>(icBuffer);
    }
    const uint8_t* icZoneStart() const {
        return reinterpret_cast<const uint8_t*>(icBuffer);
    }

    // Pointer to the side-channel selectorBits array (Task #41).
    // numICEntries u64 slots; entry i is the selector Oop bits for IC site i.
    uint64_t* selBitsArray() {
        if (selBitsArrayOffset == 0) return nullptr;
        return reinterpret_cast<uint64_t*>(codeStart() + selBitsArrayOffset);
    }

    const uint64_t* selBitsArray() const {
        if (selBitsArrayOffset == 0) return nullptr;
        return reinterpret_cast<const uint64_t*>(codeStart() + selBitsArrayOffset);
    }

    // PMS per-site patch map (docs/patched-ic-design.md §9).
    // numICEntries records; offsets come from asmjit labels ONLY —
    // never fixed-offset arithmetic (debug emit knobs shift layout).
    SendSitePatch* patchMap() {
        if (patchMapOffset == 0) return nullptr;
        return reinterpret_cast<SendSitePatch*>(codeStart() + patchMapOffset);
    }
    const SendSitePatch* patchMap() const {
        if (patchMapOffset == 0) return nullptr;
        return reinterpret_cast<const SendSitePatch*>(codeStart() + patchMapOffset);
    }

    // Resume lookup for RETVAL-CARRYING re-entries (the J2J/chain pop
    // protocol): the per-site resumeAfterCall continuation if one was
    // recorded for this post-send bcOffset, else the plain label.
    uint32_t codeOffsetForResume(uint32_t bcOffset) const {
        if (resumeOvOffset != 0) {
            const uint32_t* t = reinterpret_cast<const uint32_t*>(
                codeStart() + resumeOvOffset);
            uint32_t n = t[0];
            for (uint32_t k = 0; k < n; k++) {
                if (t[1 + 2 * k] == bcOffset) return t[2 + 2 * k];
            }
        }
        return codeOffsetForBC(bcOffset);
    }

    // Pointer to the bcToCode re-entry table
    const uint32_t* bcToCodeTable() const {
        return reinterpret_cast<const uint32_t*>(codeStart() + bcToCodeTableOffset);
    }

    // Look up the code offset for a given bytecode offset.
    //
    // Returns 0 ("no valid entry point") in any of these cases:
    //   - bcOffset out of range (>= numBytecodes)
    //   - bcOffset == numBytecodes is the sentinel "one past last
    //     bytecode" — its slot holds codeSize (past the entire
    //     machine-code region into literal pool / IC area), which
    //     is NOT a valid BLR target.  Returning that as a resume
    //     point caused SIGILL (PC in IC area, ARM64 UDF on the
    //     zero word).  See bug 11b in docs/jit-uncovered-bugs.md.
    //   - the slot's offset is past the real machine code (sentinel
    //     value or compiler bug)
    //
    // ALL callers (tryResume, J2J trampoline, chain loop, asm
    // trampoline) treat 0 as "not a valid entry point" and bail to
    // interpreter, so this is a safe centralization of the bound
    // check that was previously duplicated in only some sites.
    // Reverted to pre-bug-11-layer-2 behavior: return codeSize when
    // bcOffset is out of range.  The prior "safer" variant (return 0 and
    // reject off >= machineCodeEnd) broke the chain-loop fast path —
    // legitimate resumes whose offset was exactly at or just under the
    // machine-code boundary were being rejected, forcing actChain=0.
    // All callers already guard `off == 0 || off >= jm->codeSize`, so
    // the old sentinel returns the correct "bail" signal.
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

    // --- Derived from compiledMethodOop + methodHeader ---
    // Used by J2J return paths to recompute caller state without having to
    // save literals/bcStart redundantly. See docs/jit-j2j-reduction-plan.md.

    // Pointer to the method's literal frame (slots 1..numLits of the
    // CompiledMethod object). compiledMethodOop points at the ObjectHeader:
    //   +0  (8 bytes): ObjectHeader
    //   +8  (8 bytes): slot 0 = method header word (numLits|numArgs|...)
    //   +16 (8 bytes): slot 1 = first literal
    // So literals start at +16, not +8. Matches `methObj->slots() + 1` used
    // by tryJITActivation and every other literal-pointer setter.
    uint64_t* literals() const {
        return reinterpret_cast<uint64_t*>(compiledMethodOop + 16);
    }

    // Pointer to the first bytecode. Layout after the ObjectHeader:
    //   slot 0 = method header (1 uint64)
    //   slot 1..numLits = literals (numLits uint64s)
    //   byte 0 onward = bytecodes
    // So bcStart = compiledMethodOop + (1 + numLits + 1) * 8
    //            = compiledMethodOop + (2 + numLits) * 8
    uint8_t* bcStart() const {
        uint64_t numLits = methodHeader & 0x7FFFu;
        return reinterpret_cast<uint8_t*>(compiledMethodOop + (2 + numLits) * 8);
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
    //
    // Defensive: also reject if the JITMethod's compiledMethodOop field
    // doesn't match the lookup key.  The map can hold stale entries
    // pointing to a slot that was reallocated to a different method
    // (free + alloc reuse).  In that case e.value->isExecutable() may
    // still be true (state byte happens to be Compiled) but
    // e.value->codeStart() points to a different method's code (or to
    // garbage if the slot is now in the free list and overwritten by
    // FreeBlock metadata).  Calling such a stale code pointer SIGILLs.
    JITMethod* lookup(uint64_t compiledMethodBits) const {
        if (!entries_) return nullptr;
        size_t mask = capacity_ - 1;
        size_t idx = hash(compiledMethodBits) & mask;

        for (size_t probe = 0; probe < capacity_; probe++) {
            Entry& e = entries_[idx];
            if (e.key == 0) return nullptr;  // Empty slot
            if (e.key == compiledMethodBits
                && e.value
                && e.value->isExecutable()
                && e.value->compiledMethodOop == compiledMethodBits) {
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
