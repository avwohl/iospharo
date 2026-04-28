/*
 * SistaRuntime.cpp - Implementation: lift + lower on demand, cache.
 */
#include "SistaRuntime.hpp"
#include "../../ObjectMemory.hpp"

#include <cstdio>

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

    // Trace lowering only for splice methods — otherwise the volume
    // would drown out everything else.  Limited to 16 lines.
    bool hasSplice = false;
    bool hasSend   = false;
    for (const auto& v : m.values) {
        if (v.op == Op::kCountedLoopDo
         || v.op == Op::kCountedLoopInjectInto
         || v.op == Op::kCountedLoopIntervalInjectInto
         || v.op == Op::kCountedLoopIntervalDo
         || v.op == Op::kCountedLoopArrayDoAccum
         || v.op == Op::kCountedLoopIntervalDoAccum) {
            hasSplice = true;
        }
        if (v.op == Op::kSendUnspeculated) {
            hasSend = true;
        }
    }

    // Skip-dispatch heuristic: a method that contains kSendUnspeculated
    // but no splice will bail on the first send.  Sista's compile +
    // dispatch overhead (~80 cycles per call) is pure waste.  The
    // bail-blacklist (sistaBailCounter_) doesn't catch this because
    // successful leaf returns reset the counter — common for recursive
    // methods like benchFib (~30% leaf rate).  Negative-cache by
    // returning nullptr; the dispatch hook checks `fn != nullptr`.
    //
    // Opt-out: PHARO_SISTA_COMPILE_BAIL_ONLY=1 (for diagnosis).
    static const bool compileBailOnly =
        std::getenv("PHARO_SISTA_COMPILE_BAIL_ONLY") != nullptr;
    if (hasSend && !hasSplice && !compileBailOnly) {
        cache_[key] = nullptr;
        return nullptr;
    }

    // Lower IR → native.
    uint32_t failedVal = UINT32_MAX;
    Lowering::CompiledFn fn = lowering_.lower(m, &failedVal, bytecodes);
    if (hasSplice) {
        static int spliceLogCount = 0;
        if (spliceLogCount++ < 16) {
            if (fn) {
                fprintf(stderr,
                        "[SISTA-SPLICE-LOWER-OK] method=0x%llx\n",
                        (unsigned long long)key);
            } else {
                const char* opName = (failedVal != UINT32_MAX
                                      && failedVal < m.values.size())
                    ? OpInfo::name(m.values[failedVal].op)
                    : "(unknown)";
                fprintf(stderr,
                        "[SISTA-SPLICE-LOWER-FAIL] method=0x%llx "
                        "failedAt=v%u op=%s\n",
                        (unsigned long long)key, failedVal, opName);
            }
        }
    }
    cache_[key] = fn;
    return fn;
}

}  // namespace sista
}  // namespace pharo
