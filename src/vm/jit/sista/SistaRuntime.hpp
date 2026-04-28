/*
 * SistaRuntime.hpp - Compile-on-demand wrapper around Builder + Lowering
 *
 * Phase 2.3 of docs/sista-inlining-plan.md.  Not yet wired into the
 * VM's tier-up path — this header exists so callers can request a
 * compiled function pointer for a specific CompiledMethod and cache
 * the result.  Tier-up integration lands once we've verified that
 * a Runtime::compile call on a real image method produces a runnable
 * function and that the state.ip convention works end-to-end.
 *
 * LIFECYCLE
 *   Runtime owns a single Lowering (and thus a single asmjit::JitRuntime)
 *   for a compilation domain.  Construct once per VM; reuse forever.
 *
 * GC SAFETY (KNOWN GAP — Phase 3)
 *   The cache keys by raw oop bits, which go stale on GC compaction.
 *   A proper implementation keys by a stable-across-GC id — likely a
 *   per-method slot in a JIT-side table rebuilt during recoverAfterGC
 *   (mirroring the T1 flush pattern in JITRuntime::recoverAfterGC).
 *   For now the cache is cleared whenever a caller calls `reset()`.
 */
#ifndef PHARO_SISTA_RUNTIME_HPP
#define PHARO_SISTA_RUNTIME_HPP

#include "SistaIR.hpp"
#include "SistaBuilder.hpp"
#include "SistaLowering.hpp"

#include <unordered_map>
#include <unordered_set>

namespace pharo {

class ObjectMemory;

namespace sista {

class Runtime {
public:
    Runtime() = default;
    ~Runtime() = default;

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    // Compile the method.  On success, returns a function pointer whose
    // calling convention is `void fn(JITState* state)` (same as Tier 1
    // / Tier 2 — the runtime can invoke any tier transparently).
    // Returns nullptr on any failure (unsupported bytecode, malformed
    // method, lowerer bail).
    //
    // The result is cached by raw oop bits.  Callers must call reset()
    // after a GC compaction — see GC SAFETY note in the header comment.
    //
    // Optional `hints` (Phase 4 Step 1): inline hints from T1 IC.
    // Caller-owned, must outlive the call.
    Lowering::CompiledFn compile(Oop method, ObjectMemory& memory,
                                  const std::vector<InlineHint>* hints = nullptr);

    // Clear the method→function cache.  Call this after a GC
    // compaction, since raw oop bits become unstable.  The cached
    // machine code stays allocated in the asmjit runtime; only the
    // lookup table is cleared.
    void reset() {
        cache_.clear();
        compiledHintless_.clear();
        spliceMethods_.clear();
    }

    // Drop the cache entry for one method — but only if its compile
    // was done WITHOUT inline hints.  That way a method's first
    // (cold) Sista compile (which couldn't see the IC) is replaced
    // by a second (warm) compile after the IC populates.  Methods
    // compiled with hints stay cached: hints are monotonic, so
    // subsequent IC patches don't change peephole eligibility.
    void invalidateIfHintless(Oop method) {
        uint64_t key = method.rawBits();
        if (compiledHintless_.erase(key) > 0) {
            cache_.erase(key);
        }
    }

    // Statistics — for the Sista survey / diagnostics.
    size_t compiledCount() const { return cache_.size(); }

    // True if the cached compile for `method` includes a counted-loop
    // splice (kCountedLoopArrayDoAccum etc.).  Used by JITRuntime to
    // suppress T1 compilation for methods Sista already handles
    // optimally — prevents the T1-vs-Sista race where T1 wins the
    // activation and falls back to per-iter IC speed.  See
    // memory/project_t1_vs_sista_race.md.
    bool hasSplice(Oop method) const {
        return spliceMethods_.count(method.rawBits()) > 0;
    }

private:
    Lowering lowering_;
    std::unordered_map<uint64_t, Lowering::CompiledFn> cache_;
    // Methods whose Sista compile happened with empty/null hints.
    // Used by invalidateIfHintless() to target only those entries
    // without re-compiling everyone on every IC patch.
    std::unordered_set<uint64_t> compiledHintless_;
    // Methods whose lifted IR contains a counted-loop splice op.
    // Populated in compile() when hasSplice is detected.  Read by
    // hasSplice() above.  Cleared by reset() (post-GC).
    std::unordered_set<uint64_t> spliceMethods_;
};

}  // namespace sista
}  // namespace pharo

#endif  // PHARO_SISTA_RUNTIME_HPP
