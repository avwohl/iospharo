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
    // `startBcOffset` selects the entry point.  0 = method entry
    // (default — preserves existing behavior).  Non-zero = mid-method
    // entry, used by the per-bytecode hook at backward-jump targets
    // (loop headers).  See docs/jit-multiweek-work.md item #8.
    //
    // The result is cached by (method, startBcOffset) pair.  Callers
    // must call reset() after a GC compaction — see GC SAFETY note in
    // the header comment.
    //
    // Optional `hints` (Phase 4 Step 1): inline hints from T1 IC.
    // Caller-owned, must outlive the call.
    Lowering::CompiledFn compile(Oop method, ObjectMemory& memory,
                                  const std::vector<InlineHint>* hints = nullptr,
                                  uint32_t startBcOffset = 0);

    // Clear the method→function cache.  Call this after a GC
    // compaction, since raw oop bits become unstable.  The cached
    // machine code stays allocated in the asmjit runtime; only the
    // lookup table is cleared.
    void reset() {
        cache_.clear();
        bcOffsetCache_.clear();
        compiledHintless_.clear();
        spliceMethods_.clear();
        backwardJumpCounters_.clear();
        bcDispatchBailCounter_.clear();
    }

    // jit-may22a B1 Sub-step 3: re-key the cache by translating old
    // oop bits → new oop bits via a follow-forwarders callback.
    // Called from Interpreter::recoverSistaAfterGC instead of reset()
    // when PHARO_SISTA_REKEY_AFTER_GC=1.  Preserves benchFib's
    // hints-bearing compile across compaction so kSendInlineSelf
    // emits aren't lost.
    template<typename FollowFn>
    void rekeyAfterGC(FollowFn follow) {
        std::unordered_map<uint64_t, Lowering::CompiledFn> newCache;
        newCache.reserve(cache_.size());
        for (auto& kv : cache_) {
            uint64_t newKey = follow(kv.first);
            if (newKey != 0) {
                newCache[newKey] = kv.second;
            }
        }
        cache_ = std::move(newCache);
        // bcOffsetCache_: similar shape; rekey if non-empty.
        if (!bcOffsetCache_.empty()) {
            std::unordered_map<uint64_t,
                std::unordered_map<uint32_t,
                                    Lowering::CompiledFn>> newBcCache;
            for (auto& kv : bcOffsetCache_) {
                uint64_t newKey = follow(kv.first);
                if (newKey != 0) {
                    newBcCache[newKey] = std::move(kv.second);
                }
            }
            bcOffsetCache_ = std::move(newBcCache);
        }
        // compiledHintless_ + spliceMethods_: re-translate sets too.
        std::unordered_set<uint64_t> newHintless;
        for (uint64_t k : compiledHintless_) {
            uint64_t n = follow(k);
            if (n != 0) newHintless.insert(n);
        }
        compiledHintless_ = std::move(newHintless);
        std::unordered_set<uint64_t> newSplice;
        for (uint64_t k : spliceMethods_) {
            uint64_t n = follow(k);
            if (n != 0) newSplice.insert(n);
        }
        spliceMethods_ = std::move(newSplice);
        // The counter maps don't survive — they're rebuilt naturally.
        backwardJumpCounters_.clear();
        bcDispatchBailCounter_.clear();
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

    // ===== Per-bytecode hook (item #8 in jit-multiweek-work.md) =====
    //
    // Look up a cached lowered fn for entering `method` mid-method at
    // bcOffset (a backward-jump target / loop header).  Returns
    // nullptr on miss — caller (the backward-jump stencil) decides
    // whether to bump the counter and trigger a compile.
    Lowering::CompiledFn lookupBcEntry(Oop method, uint32_t bcOffset) const {
        auto outer = bcOffsetCache_.find(method.rawBits());
        if (outer == bcOffsetCache_.end()) return nullptr;
        auto inner = outer->second.find(bcOffset);
        if (inner == outer->second.end()) return nullptr;
        return inner->second;
    }

    // jit-may20b Step 8.4: const lookup into the method→fn cache.
    // Used by patchJITICAfterSend to decide whether to set SISTA_BIT.
    // Returns nullptr on miss; does NOT trigger compilation.
    Lowering::CompiledFn lookupCompiled(Oop method) const {
        auto it = cache_.find(method.rawBits());
        if (it == cache_.end()) return nullptr;
        return it->second;
    }

    // Increment the per-(method, bcOffset) backward-jump counter.
    // Returns the new count.  Used by the backward-jump hook to
    // decide when to trigger a per-bytecode Sista compile.
    uint32_t bumpBackwardJumpCounter(Oop method, uint32_t bcOffset) {
        uint64_t key = ((uint64_t)method.rawBits() << 16)
                     | ((uint64_t)bcOffset & 0xFFFF);
        return ++backwardJumpCounters_[key];
    }

    // Adaptive blacklist (item #8 — multi-entry dispatch).  When a
    // method's dispatched fn consistently bails close to entry, skip
    // future dispatches for ALL bcoffs of that method.  Avoids paying
    // the dispatch + sstate-init cost on methods where the compiled
    // fn doesn't deliver a win.  Method-level (not per-(method, bcOff))
    // because per-(method, bcOff) lets each new sample-bcOff burn one
    // dispatch before blacklisting kicks in — for sort's mergeFirst
    // (~tens of distinct sample bcoffs), that's tens of wasted
    // dispatches per outer iter.  Method-level kills the second
    // dispatch onward.
    bool isBcDispatchBlacklisted(Oop method, uint32_t /*bcOffset*/) const {
        auto it = bcDispatchBailCounter_.find(method.rawBits());
        return it != bcDispatchBailCounter_.end()
            && it->second >= kBcDispatchBlacklistThreshold;
    }
    void noteBcDispatchBail(Oop method, uint32_t /*bcOffset*/) {
        uint64_t key = method.rawBits();
        if (bcDispatchBailCounter_[key] < UINT16_MAX) {
            bcDispatchBailCounter_[key]++;
        }
    }
    void noteBcDispatchSuccess(Oop method, uint32_t /*bcOffset*/) {
        bcDispatchBailCounter_.erase(method.rawBits());
    }
    static constexpr uint16_t kBcDispatchBlacklistThreshold = 1;

    // Threshold at which the per-bytecode compile is queued.  Bumped
    // only on every-64-sample fires, so threshold N = ~64*N actual
    // backward jumps before compile.  Lower = compile sooner (more
    // wins on shorter loops); higher = avoid compile cost on cold
    // loops.
    static constexpr uint32_t kBackwardJumpThreshold = 100;

private:
    Lowering lowering_;
    std::unordered_map<uint64_t, Lowering::CompiledFn> cache_;
    // Per-bytecode compile cache: method.rawBits() → bcOffset → fn.
    // Populated when caller passes startBcOffset != 0 to compile().
    // The outer map is method-keyed for efficient invalidation when
    // a method's IR changes.
    std::unordered_map<uint64_t,
        std::unordered_map<uint32_t, Lowering::CompiledFn>> bcOffsetCache_;
    // Per-(method, bcOffset) backward-jump counter.  Composite key:
    // (methodBits << 16) | (bcOffset & 0xFFFF).  bcOffset for backward
    // jumps fits in 16 bits in practice (Pharo methods rarely exceed
    // 64KB of bytecodes).
    std::unordered_map<uint64_t, uint32_t> backwardJumpCounters_;
    // Per-(method, bcOffset) consecutive-bail counter for adaptive
    // blacklisting.  Same composite key as backwardJumpCounters_.
    // Bumped on ExitSend, cleared on ExitReturn; once >= threshold,
    // dispatch is skipped for that key.
    mutable std::unordered_map<uint64_t, uint16_t> bcDispatchBailCounter_;
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
