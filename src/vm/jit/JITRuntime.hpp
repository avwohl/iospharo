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
#include "../Oop.hpp"

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

    // Called by the interpreter on each method activation. Increments the
    // execution counter and triggers compilation at the threshold.
    void noteMethodEntry(Oop compiledMethod);

    // Flush all inline caches and mega cache (called on become:, GC, method changes)
    void flushCaches();

    // Full recovery after GC compaction: flush caches, rebuild MethodMap from
    // updated JITMethod headers, update special Oops, clear count map.
    // Call AFTER forEachRoot has updated compiledMethodOop in JITMethod headers.
    void recoverAfterGC(ObjectMemory& memory);

    // Access to subsystems
    CodeZone&     codeZone()   { return codeZone_; }
    MethodMap&    methodMap()  { return methodMap_; }
    JITCompiler*  compiler()   { return compiler_; }

    // Special Oop storage (stencils load from these addresses)
    uint64_t nilOopBits;
    uint64_t trueOopBits;
    uint64_t falseOopBits;

    // Update special Oops (call after GC moves objects)
    void updateSpecialOops(ObjectMemory& memory);

    bool isInitialized() const { return initialized_; }

    // Count map entry access (for GC root scanning)
    struct CountEntry {
        uint64_t key;
        uint32_t count;
    };
    CountEntry& countMapEntry(size_t i) { return countMap_[i]; }

    // Megamorphic method cache — probed by stencils after PIC miss
    MegaCacheEntry* megaCache() { return megaCache_; }

    // Add entry to mega cache (called by interpreter after method lookup)
    void megaCacheAdd(uint64_t selectorBits, uint64_t classIndex, uint64_t methodBits) {
        // Primary probe (matches stencil hash)
        size_t h = static_cast<size_t>(selectorBits ^ classIndex) & (MegaCacheSize - 1);
        // Secondary probe (rotated hash, matches stencil)
        size_t h2 = static_cast<size_t>((selectorBits >> 3) ^ (classIndex << 2) ^ classIndex) & (MegaCacheSize - 1);
        // Insert into whichever slot is empty or has a different entry
        // Prefer primary; use secondary if primary is occupied by different entry
        if (megaCache_[h].selectorBits == selectorBits && megaCache_[h].classIndex == classIndex) {
            megaCache_[h].methodBits = methodBits;  // Update existing
        } else if (megaCache_[h2].selectorBits == selectorBits && megaCache_[h2].classIndex == classIndex) {
            megaCache_[h2].methodBits = methodBits;  // Update existing in secondary
        } else if (megaCache_[h].selectorBits == 0) {
            megaCache_[h] = {selectorBits, classIndex, methodBits};
        } else if (megaCache_[h2].selectorBits == 0) {
            megaCache_[h2] = {selectorBits, classIndex, methodBits};
        } else {
            // Both occupied — evict primary
            megaCache_[h] = {selectorBits, classIndex, methodBits};
        }
    }

private:
    MegaCacheEntry megaCache_[MegaCacheSize] = {};
    CodeZone    codeZone_;
    MethodMap   methodMap_;
    JITCompiler* compiler_ = nullptr;
    Interpreter* interp_ = nullptr;
    bool        initialized_ = false;

    // Execution count tracking for compilation triggering
    CountEntry countMap_[CountMapSize];
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

} // namespace jit
} // namespace pharo

#endif // PHARO_JIT_ENABLED
#endif // PHARO_JIT_RUNTIME_HPP
