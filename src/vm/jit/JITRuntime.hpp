/*
 * JITRuntime.hpp - Runtime support for JIT-compiled code
 *
 * Copyright (c) 2026 Aaron Wohl. Licensed under the MIT License.
 *
 * Provides:
 * - Entry point: interpreter -> JIT code transition
 * - Exit stubs: JIT code -> interpreter (for sends, returns, deopt)
 * - Runtime helper functions called from JIT stencils
 * - Special Oop storage for nil/true/false (used by stencil patching)
 */

#ifndef PHARO_JIT_RUNTIME_HPP
#define PHARO_JIT_RUNTIME_HPP

#include "JITConfig.hpp"
#include "JITState.hpp"
#include "JITMethod.hpp"
#include "CodeZone.hpp"
#include "JITCompiler.hpp"
#include "Tier2Compiler.hpp"
#include "../Oop.hpp"
#include <unordered_map>
#include <vector>
#include <cstdint>

#if PHARO_JIT_ENABLED

namespace pharo {

class Interpreter;
class ObjectMemory;

namespace jit {

static constexpr size_t CountMapSize = 16384;

class JITRuntime {
public:
    JITRuntime();
    ~JITRuntime();

    // Initialize the JIT subsystem. Call once after ObjectMemory is loaded.
    bool initialize(ObjectMemory& memory, Interpreter& interp);

    // Attempt to execute a compiled method. Returns true if JIT code ran
    // (the interpreter should inspect state.exitReason). Returns false if
    // the method isn't compiled yet.
    bool tryExecute(Oop compiledMethod, JITState& state);

    // Fast overload: skip methodMap lookup when caller already has the JITMethod.
    bool tryExecute(Oop compiledMethod, JITState& state, JITMethod* jm);

    // Resume JIT execution at a specific bytecodeOffset (for on-stack re-entry
    // after a send returns). Returns true if JIT code ran.
    bool tryResume(Oop compiledMethod, uint32_t bcOffset, JITState& state);

    // Fast resume: skip method map lookup, IC validation, receiver bounds check,
    // and LRU touch. For prim-in-place where we know the JITMethod is the same
    // one we just exited from. Caller must ensure jm is still valid/executable.
    bool tryResumeFast(JITMethod* jm, uint32_t bcOffset, JITState& state);

    // Called by the interpreter on each method activation. Increments the
    // execution counter and triggers compilation at the threshold.
    void noteMethodEntry(Oop compiledMethod);

    // Per-block call counter, separate from countMap_ so blocks can use
    // a higher threshold than methods (default 200 vs method's 2).
    // Without this, inner blocks like [:x | x+1] in a tight loop never
    // reach the JIT compile threshold (interpreter's activateBlock path
    // doesn't bump countMap_), forcing every iteration through the
    // primitive 207 → activateBlock C++ path — 24% of CPU on Pi 5
    // bench post-findMethodByPC-fix.
    void noteBlockEntry(Oop compiledBlock);

    // Flush all inline caches and mega cache (called on become:, GC, method changes)
    void flushCaches();

    // Diagnostic: walk every compiled method's IC sites and report the
    // distribution of site polymorphism (empty / monomorphic / 2-way /
    // 3-way / 4+-way / megamorphic).  Used to validate Phase 1 of the
    // Sista inlining plan — if most sites are monomorphic, speculative
    // inlining is worth the implementation cost.  Triggered by env var
    // PHARO_IC_HISTOGRAM=1 during VM shutdown, or on-demand via the
    // image-side mirror primitive primitiveJITICHistogram.
    void dumpICHistogram() const;

    // Full recovery after GC compaction: flush caches, rebuild MethodMap from
    // updated JITMethod headers, update special Oops, clear count map.
    // Call AFTER forEachRoot has updated compiledMethodOop in JITMethod headers.
    void recoverAfterGC(ObjectMemory& memory);

    // Access to subsystems
    CodeZone&     codeZone()   { return codeZone_; }
    MethodMap&    methodMap()  { return methodMap_; }
    JITCompiler*  compiler()   { return compiler_; }
    Tier2Compiler* tier2Compiler() { return tier2Compiler_; }

    // Special Oop storage (stencils load from these addresses)
    uint64_t nilOopBits;
    uint64_t trueOopBits;
    uint64_t falseOopBits;
    uint32_t smallIntClassIdx = 0;  // classTable index for SmallInteger class

    // Update special Oops (call after GC moves objects)
    void updateSpecialOops(ObjectMemory& memory);

    bool isInitialized() const { return initialized_; }

    // Count map entry access (for GC root scanning)
    struct CountEntry {
        uint64_t key;
        uint32_t count;
    };
    CountEntry& countMapEntry(size_t i) { return countMap_[i]; }

    // Tier 2 map entry access (for GC root scanning)
    struct Tier2Entry {
        uint64_t key;   // CompiledMethod Oop bits (0 = empty)
        void*    func;  // MIR-generated function pointer
    };
    static constexpr size_t Tier2MapSize = 4096;
    Tier2Entry& tier2Entry(size_t i) { return tier2Map_[i]; }

    // Megamorphic method cache — probed by stencils after PIC miss
    MegaCacheEntry* megaCache() { return megaCache_; }

    // Public T2 lookup for jit_t2_send helper
    void* lookupTier2(uint64_t methodBits) const { return tier2Lookup(methodBits); }

    // --- T2 monomorphic send cache ---
    // Each send site in T2 code gets a 16-byte IC slot: (classIndex, resolvedMethodBits).
    // On IC hit, jit_t2_send skips the full method lookup (class hierarchy walk).
    // Flushed on GC (resolvedMethodBits becomes stale after compaction).
    struct T2ICSlot {
        uint64_t classIndex;     // Receiver classIndex (0 = empty/miss)
        uint64_t resolvedBits;   // Resolved CompiledMethod Oop bits
    };
    static constexpr int MaxT2ICSlots = 8192;
    T2ICSlot* allocT2ICSlots(int count);
    void flushT2ICs();

    // Per-bcOffset SimStack entry state, used by tryResume to detect when
    // an entry offset lands on a register-reading (_N) stencil. Keyed by
    // JITMethod* (GC-stable), so GC compaction doesn't invalidate entries.
    // Always populated at compile time.
    void setBcEntryStates(JITMethod* jm, std::vector<uint8_t>&& states) {
        bcEntryStates_[jm] = std::move(states);
    }
    uint8_t getBcEntryState(JITMethod* jm, uint32_t bcOffset) const {
        auto it = bcEntryStates_.find(jm);
        if (it == bcEntryStates_.end()) return 0;
        if (bcOffset >= it->second.size()) return 0;
        return it->second[bcOffset];
    }
    void clearBcEntryStates(JITMethod* jm) {
        bcEntryStates_.erase(jm);
    }

    // Add entry to mega cache (called by interpreter after method lookup)
    void megaCacheAdd(uint64_t selectorBits, uint64_t classIndex,
                      uint64_t methodBits, uint64_t jitEntry = 0) {
        // Primary probe (matches stencil hash)
        size_t h = static_cast<size_t>(selectorBits ^ classIndex) & (MegaCacheSize - 1);
        // Secondary probe (rotated hash, matches stencil)
        size_t h2 = static_cast<size_t>((selectorBits >> 3) ^ (classIndex << 2) ^ classIndex) & (MegaCacheSize - 1);
        // Insert into whichever slot is empty or has a different entry
        // Prefer primary; use secondary if primary is occupied by different entry
        if (megaCache_[h].selectorBits == selectorBits && megaCache_[h].classIndex == classIndex) {
            megaCache_[h].methodBits = methodBits;  // Update existing
            megaCache_[h].jitEntry = jitEntry;
        } else if (megaCache_[h2].selectorBits == selectorBits && megaCache_[h2].classIndex == classIndex) {
            megaCache_[h2].methodBits = methodBits;  // Update existing in secondary
            megaCache_[h2].jitEntry = jitEntry;
        } else if (megaCache_[h].selectorBits == 0) {
            megaCache_[h] = {selectorBits, classIndex, methodBits, jitEntry};
        } else if (megaCache_[h2].selectorBits == 0) {
            megaCache_[h2] = {selectorBits, classIndex, methodBits, jitEntry};
        } else {
            // Both occupied — evict primary
            megaCache_[h] = {selectorBits, classIndex, methodBits, jitEntry};
        }
    }

private:
    MegaCacheEntry megaCache_[MegaCacheSize] = {};
    CodeZone    codeZone_;
    MethodMap   methodMap_;
    JITCompiler* compiler_ = nullptr;
    Tier2Compiler* tier2Compiler_ = nullptr;
    Interpreter* interp_ = nullptr;
    bool        initialized_ = false;

    // Tier 2 compiled method map: compiledMethodOop bits → MIR function pointer
    // Separate from MethodMap to avoid changing JITMethod layout.
    Tier2Entry tier2Map_[Tier2MapSize] = {};

    void* tier2Lookup(uint64_t methodBits) const {
        size_t mask = Tier2MapSize - 1;
        size_t idx = (size_t)((methodBits >> 3) * 11400714819323198485ULL) & mask;
        for (size_t p = 0; p < 16; p++) {
            if (tier2Map_[idx].key == methodBits) return tier2Map_[idx].func;
            if (tier2Map_[idx].key == 0) return nullptr;
            idx = (idx + 1) & mask;
        }
        return nullptr;
    }

    void tier2Insert(uint64_t methodBits, void* func) {
        size_t mask = Tier2MapSize - 1;
        size_t idx = (size_t)((methodBits >> 3) * 11400714819323198485ULL) & mask;
        for (size_t p = 0; p < 16; p++) {
            if (tier2Map_[idx].key == 0 || tier2Map_[idx].key == methodBits) {
                tier2Map_[idx].key = methodBits;
                tier2Map_[idx].func = func;
                return;
            }
            idx = (idx + 1) & mask;
        }
    }

    // Execution count tracking for compilation triggering
    CountEntry countMap_[CountMapSize];

    // Per-block counter — separate so blocks can use a high threshold
    // (default 200) without affecting method compilation cadence.
    static constexpr size_t BlockCountMapSize = 4096;
    CountEntry blockCountMap_[BlockCountMapSize] = {};

    // Per-JITMethod bcOffset → SimStack entry state. Keyed by JITMethod*
    // (GC-stable). Used by tryResume to reject resume at register-reading
    // (_N) stencil offsets to avoid entering with garbage in x19-x22.
    std::unordered_map<JITMethod*, std::vector<uint8_t>> bcEntryStates_;

    // T2 monomorphic IC slot pool (128KB, bump-allocated per compilation)
    T2ICSlot t2ICPool_[MaxT2ICSlots] = {};
    int t2ICNextSlot_ = 0;
};

// ===== RUNTIME HELPER FUNCTIONS =====
//
// These are called from JIT stencils via patched branch instructions.
// They have the same signature as stencils: void(JITState*).

// Send slow path: JIT code couldn't handle this send (no IC, megamorphic, etc.)
// Sets up state for the interpreter to do a full lookup+activate.
extern "C" void jit_rt_send(JITState* state);

// Return to interpreter: JIT code hit a return bytecode.
// The interpreter reads state->returnValue and unwinds.
extern "C" void jit_rt_return(JITState* state);

// Arithmetic overflow: SmallInteger operation overflowed or operands
// weren't SmallIntegers. Fall back to interpreter for full send.
extern "C" void jit_rt_arith_overflow(JITState* state);

// J2J direct call helpers: push/pop interpreter frames for GC root scanning.
// Called from stencil_sendJ2J before/after BLR to callee entry.
// Reads cachedTarget (method Oop), sendArgCount, ip from state.
extern "C" void jit_rt_push_frame(JITState* state);
extern "C" void jit_rt_pop_frame(JITState* state);

// T2 monomorphic IC counters (defined in JITRuntime.cpp)
extern int g_t2ICHits;
extern int g_t2ICMisses;

// Tier 2 inline send helper: called from MIR-generated code at each send site.
// Performs method lookup + callee JIT execution inline, avoiding exit/resume overhead.
// On ExitReturn from callee: pops args, pushes retval, sets exitReason = ExitNone.
// On non-ExitReturn: pushes SavedFrame for T2 caller, propagates callee exit reason.
// On lookup failure or non-compiled callee: sets exitReason = ExitSend (chain loop handles).
extern "C" void jit_t2_send(JITState* state);

} // namespace jit
} // namespace pharo

#endif // PHARO_JIT_ENABLED
#endif // PHARO_JIT_RUNTIME_HPP
