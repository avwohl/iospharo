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
#include <unordered_set>
#include <vector>
#include <cstdint>
#include <cstring>

#if PHARO_JIT_ENABLED

namespace pharo {

class Interpreter;
class ObjectMemory;

namespace jit {

// 2026-05-08 image-init-complete signal (5th attempt): set by interp
// and JIT stencil dispatch when storing a NON-NIL value into a
// class-variable binding whose key is the Symbol #Resolver.
//
// The actual FileLocator class>>startUp: does:
//   Resolver := InteractiveResolver new.
//   Resolver addResolver: SystemResolver new.
//   Resolver addResolver: PlatformResolver forCurrentPlatform.
// The first store flips g_resolverClassVarSet, but the addResolver:
// calls happen AFTER.  So the signal alone isn't enough — defer-lift
// also waits g_resolverSetAtStep + buffer steps to allow those calls
// to complete.
extern volatile bool g_resolverClassVarSet;
extern volatile uint64_t g_resolverSetAtStep;

// Inline-checkable predicate.  Caller should test
// g_resolverClassVarSet first to avoid the function-call overhead
// after the flag is set.
void noteLitVarStore(uint64_t assocBits, ObjectMemory& memory);

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

    // OSR-driven recompile (2026-04-30).  When OSR fires for a method that
    // was already T1-compiled but never reached the recompile threshold,
    // its IC entries may have been populated during interp execution but
    // the JIT code is still the IC-empty initial compile.  This trigger
    // forces a recompile so the specialization pass picks up the IC data
    // before OSR enters JIT.  Idempotent — only fires when method is tier 1
    // AND has populated IC entries AND hasn't been recompiled yet.
    // Returns true if a recompile happened.
    bool maybeRecompileForOSR(Oop compiledMethod);

    // Drain the J2J inline-bump recompile queue.  Called at safe points
    // from Interpreter (e.g. periodic preemption check) where no Sista
    // splice is mid-execution — avoids the past T1-vs-Sista race.
    // Pops queued method oops, calls maybeRecompileForOSR for each.
    // Returns the number of methods processed.
    size_t drainRecompileQueue();

    // Push a method onto the initial-compile queue.  Called by
    // noteMethodEntry when PHARO_QUEUE_COMPILE=1 is set; otherwise the
    // direct compile path runs.  Bounded ring buffer; overflow drops
    // silently (re-queue happens on next threshold-bump).
    void queueInitialCompile(Oop compiledMethod);

    // Drain the initial-compile queue.  Methods that crossed the JIT
    // threshold during interp dispatch are enqueued here (instead of
    // being compiled inline in noteMethodEntry).  Drained at the same
    // safe point as drainRecompileQueue so compile happens between
    // bytecodes — never mid-bytecode while local interp state is
    // in flux.  Gated by PHARO_QUEUE_COMPILE=1 (opt-in).
    // Returns the number of methods compiled.
    size_t drainInitialCompileQueue();

    // Late-spec re-recompile: account for an IC-classification bit added
    // to an empty slot in callerJM after callerJM has already been
    // recompiled (tier=2).  When enough bits accumulate, queue callerJM
    // for a one-shot re-recompile so applyICSpecialization picks them up.
    // Closes the warm-IC late-recompile gap (sort 100K's mergeFirst).
    // Gated by PHARO_LATE_SPEC_RECOMPILE=1 (opt-in until validated).
    void noteLateSpecBit(JITMethod* callerJM, uint64_t newExtra);

    // Rewrite J2J entry-addr bits in every IC site whose methodBits matches
    // the recompiled method.  After recompile() returns a new JITMethod,
    // callers' IC.extra still holds the OLD entry address — left untouched,
    // their stencil_sendJ2J / stencil_sendInlineMonoJ2J / sendBlockValue*
    // tail-calls keep entering the OLD (unspecialized) code.  This walk
    // patches them to the new code.  Two-pass: read-only detect first to
    // avoid the W^X flip when no rewrite is needed.  Cheap: only the low
    // 47 bits (J2J_ADDR_MASK) of `extra` are rewritten when J2J_ENTRY_BIT
    // (bit 60) is set; classification bits preserved.
    void rewriteIcEntriesAfterRecompile(uint64_t methodBits,
                                        uint64_t newEntryAddr);

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

    // Young-space bounds for megaCacheAdd's tenure guard.  Set once by
    // the Interpreter after memory init (the mmap'd region is fixed for
    // the process; objects move within it).
    void setYoungSpaceBounds(const void* start, const void* end) {
        youngStart_ = reinterpret_cast<uint64_t>(start);
        youngEnd_   = reinterpret_cast<uint64_t>(end);
    }

    // Add entry to mega cache (called by interpreter after method lookup)
    void megaCacheAdd(uint64_t selectorBits, uint64_t classIndex,
                      uint64_t methodBits, uint64_t jitEntry = 0) {
        // TENURE GUARD (2026-06-10): the megaCache is NOT a GC root and
        // is only cleared on FULL GC — a scavenge moves young objects
        // and leaves the cached raw oops pointing at recycled eden
        // memory.  Caught live: a stale methodBits served the address
        // where the eval's 'EVAL-RESULT=' ByteString had been
        // reallocated, and the send path executed the string as a
        // CompiledMethod (stencils probe this cache directly — no C++
        // validation downstream).  Never cache young oops: young
        // methods/selectors re-lookup until tenured.  A young SELECTOR
        // is equally fatal as a key: a different symbol recycled at its
        // address would false-hit someone else's methodBits.
        if ((methodBits >= youngStart_ && methodBits < youngEnd_)
            || (selectorBits >= youngStart_ && selectorBits < youngEnd_)) {
            return;
        }
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

    // FullBlockClosure's classIndex, resolved lazily on first request
    // (initialize() runs before classTable_ is fully populated for some
    // images).  Cached for the lifetime of the runtime.  Returns 0 if
    // we can't find the class — caller should fall back to plain J2J.
    // Used by selector-based block-value specialization
    // (PHARO_BLOCK_VALUE_SPEC) — when compile() sees a Send{0,1,2} with
    // selector value/value:/value:value:, it applies stencil_sendBlockValue
    // {0,1,2}Arg with this classIndex baked in, even on cold IC sites.
    uint32_t resolveFullBlockClosureClassIndex();

private:
    MegaCacheEntry megaCache_[MegaCacheSize] = {};
    // Young-space bounds for the megaCacheAdd tenure guard.  Defaults
    // make the guard reject EVERYTHING until setYoungSpaceBounds runs
    // (safe: cache misses, never stale entries).
    uint64_t youngStart_ = 0;
    uint64_t youngEnd_ = ~0ULL;
    CodeZone    codeZone_;
    MethodMap   methodMap_;
    JITCompiler* compiler_ = nullptr;
    Tier2Compiler* tier2Compiler_ = nullptr;
    Interpreter* interp_ = nullptr;
    bool        initialized_ = false;
    uint32_t    fullBlockClosureClassIndex_ = 0;

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

    // Per-JITMethod bcOffset → SimStack entry state. Keyed by JITMethod*
    // (GC-stable). Used by tryResume to reject resume at register-reading
    // (_N) stencil offsets to avoid entering with garbage in x19-x22.
    std::unordered_map<JITMethod*, std::vector<uint8_t>> bcEntryStates_;

    // Negative cache for initial-compile failures, keyed by compiledMethod/
    // compiledBlock oop bits.  Without it, a BLOCK whose compiler_->compile()
    // bails (unsupported bytecode/prim) is never inserted into methodMap_, so
    // the op_value fast path's lookup stays null and re-queues the block on
    // every value — re-running the full asmjit T1 pipeline each safe-point
    // drain, forever (the method path is self-limited by the count-map; blocks
    // bypass it via queueInitialCompile).  Same bug class as the recompile
    // thrash fixed by kRecompileFailed.
    //
    // 2026-06-10: was a std::unordered_set cleared per-GC ("one fresh
    // attempt per GC, bounded") — at SUnit-suite scale that meant 1.4M+
    // failed asmjit pipeline runs per 60-class batch (~700/sec, each a
    // full compile attempt).  Now an open-addressed key array, GC-visited
    // in forEachRoot (Interpreter.hpp, next to the countMap_ walk) and
    // REHASHED in recoverAfterGC like countMap_ — entries survive GC, so
    // a failing method is attempted once, ever (until evicted by hash
    // pressure: insert overwrites the first probe slot when its 8-slot
    // window is full).
public:
    static constexpr size_t FailedMapSize = 16384;  // 128 KB of keys
    uint64_t& initialCompileFailedKey(size_t i) {
        return initialCompileFailedKeys_[i];
    }
    bool initialCompileFailedContains(uint64_t methBits) const {
        size_t idx = (methBits >> 3) % FailedMapSize;
        for (size_t probe = 0; probe < 8; probe++) {
            uint64_t k = initialCompileFailedKeys_[(idx + probe) % FailedMapSize];
            if (k == methBits) return true;
            if (k == 0) return false;
        }
        return false;
    }
    void initialCompileFailedInsert(uint64_t methBits) {
        size_t idx = (methBits >> 3) % FailedMapSize;
        for (size_t probe = 0; probe < 8; probe++) {
            uint64_t& slot = initialCompileFailedKeys_[(idx + probe) % FailedMapSize];
            if (slot == methBits) return;
            if (slot == 0) { slot = methBits; return; }
        }
        // Window full — overwrite the first probe slot (eviction: the
        // displaced method may thrash again briefly; bounded).
        initialCompileFailedKeys_[idx] = methBits;
    }
    void rehashInitialCompileFailed() {
        // Heap-allocate the temp (128 KB — too big for the stack; same
        // pattern as recoverAfterGC's countMap_ rehash).
        auto* temp = new uint64_t[FailedMapSize];
        std::memcpy(temp, initialCompileFailedKeys_,
                    sizeof(initialCompileFailedKeys_));
        std::memset(initialCompileFailedKeys_, 0,
                    sizeof(initialCompileFailedKeys_));
        for (size_t i = 0; i < FailedMapSize; i++) {
            if (temp[i] != 0) initialCompileFailedInsert(temp[i]);
        }
        delete[] temp;
    }
private:
    uint64_t initialCompileFailedKeys_[FailedMapSize] = {};

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
