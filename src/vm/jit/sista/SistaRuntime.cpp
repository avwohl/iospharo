/*
 * SistaRuntime.cpp - Implementation: lift + lower on demand, cache.
 */
#include "SistaRuntime.hpp"
#include "../../ObjectMemory.hpp"

namespace pharo {
namespace sista {

Lowering::CompiledFn Runtime::compile(Oop method, ObjectMemory& memory,
                                       const std::vector<InlineHint>* hints) {
    // Cache hit?
    uint64_t key = method.rawBits();
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;

    // Track methods compiled without hints so invalidateIfHintless()
    // can re-compile them once their IC populates.
    bool hadHints = (hints != nullptr && !hints->empty());
    if (!hadHints) compiledHintless_.insert(key);

    // Lift bytecode → IR.  Use buildWithHints when hints are present
    // so Sista can identify monomorphic sites for Phase 4 inlining.
    Method m;
    uint32_t failedBc = UINT32_MAX;
    LiftResult r = hints
        ? Builder::buildWithHints(method, memory, m, hints, &failedBc)
        : Builder::build(method, memory, m, &failedBc);
    if (r != LiftResult::kOk) {
        cache_[key] = nullptr;  // negative cache
        return nullptr;
    }

    // Find the bytecodes pointer — same offset calculation as
    // Builder::build.  We re-derive rather than returning it from the
    // builder so the Runtime stays decoupled from Method-internal
    // layout.
    if (!method.isObject()) {
        cache_[key] = nullptr;
        return nullptr;
    }
    ObjectHeader* mh = method.asObjectPtr();
    Oop hdrOop = memory.fetchPointer(0, method);
    if (!hdrOop.isSmallInteger()) {
        cache_[key] = nullptr;
        return nullptr;
    }
    uint32_t numLiterals = (uint32_t)(hdrOop.asSmallInteger() & 0x7FFF);
    const uint8_t* bytecodes = mh->bytes() + (1 + numLiterals) * 8;

    // Lower IR → native.
    uint32_t failedVal = UINT32_MAX;
    Lowering::CompiledFn fn = lowering_.lower(m, &failedVal, bytecodes);
    cache_[key] = fn;
    return fn;
}

}  // namespace sista
}  // namespace pharo
