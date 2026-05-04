/*
 * SistaRuntime.cpp - Implementation: lift + lower on demand, cache.
 */
#include "SistaRuntime.hpp"
#include "../../ObjectMemory.hpp"

#include <cstdio>

namespace pharo {
namespace sista {

Lowering::CompiledFn Runtime::compile(Oop method, ObjectMemory& memory,
                                       const std::vector<InlineHint>* hints,
                                       uint32_t startBcOffset) {
    // Per-bytecode entry: separate cache keyed by (method, bcOffset).
    // For startBcOffset == 0 (the default / method-entry case), use
    // the per-method cache to keep behavior identical to before.
    uint64_t key = method.rawBits();
    if (startBcOffset != 0) {
        auto outer = bcOffsetCache_.find(key);
        if (outer != bcOffsetCache_.end()) {
            auto inner = outer->second.find(startBcOffset);
            if (inner != outer->second.end()) return inner->second;
        }
        // Per-bytecode entry: lift the suffix [startBcOffset..end)
        // via Builder::buildFromOffset.  Phase 3 — the lifter handles
        // the lifting, lowering uses the same path as method-entry
        // compile, but with bytecodeBase shifted forward so deopts
        // resume at the right method-relative ip.
        Method m;
        uint32_t failedBc = UINT32_MAX;
        LiftResult r =
            Builder::buildFromOffset(method, memory, m, startBcOffset, &failedBc);
        if (r != LiftResult::kOk) {
            bcOffsetCache_[key][startBcOffset] = nullptr;  // negative cache
            static const bool trace =
                std::getenv("PHARO_SISTA_PER_BC_TRACE") != nullptr;
            if (trace) {
                static int liftFailLog = 0;
                if (liftFailLog++ < 50) {
                    // Dump the failing byte to identify what shape
                    // the lifter doesn't yet handle.  failedBc is
                    // local to the lifted suffix, so original method
                    // bcOffset = startBcOffset + failedBc.
                    uint8_t failByte = 0;
                    if (method.isObject()) {
                        ObjectHeader* mh = method.asObjectPtr();
                        Oop hdr = mh->slots()[0];
                        if (hdr.isSmallInteger()) {
                            int nLits = hdr.asSmallInteger() & 0x7FFF;
                            const uint8_t* bcs =
                                mh->bytes() + (1 + nLits) * 8;
                            uint32_t orig = startBcOffset + failedBc;
                            if (orig < (uint32_t)mh->byteSize()
                                       - (1 + nLits) * 8) {
                                failByte = bcs[orig];
                            }
                        }
                    }
                    fprintf(stderr,
                        "[SISTA-PER-BC-COMPILE] method=0x%llx bcOff=%u "
                        "result=lift-failed (failedBc=%u origBc=%u "
                        "failByte=0x%02x kind=%d)\n",
                        (unsigned long long)key, startBcOffset,
                        failedBc, startBcOffset + failedBc,
                        (unsigned)failByte, (int)r);
                }
            }
            return nullptr;
        }
        // Recover bytecodes pointer for lowering's deopt sequences.
        if (!method.isObject()) {
            bcOffsetCache_[key][startBcOffset] = nullptr;
            return nullptr;
        }
        ObjectHeader* mh = method.asObjectPtr();
        Oop hdrOop = memory.fetchPointer(0, method);
        if (!hdrOop.isSmallInteger()) {
            bcOffsetCache_[key][startBcOffset] = nullptr;
            return nullptr;
        }
        uint32_t numLiterals = (uint32_t)(hdrOop.asSmallInteger() & 0x7FFF);
        const uint8_t* bytecodes = mh->bytes() + (1 + numLiterals) * 8;
        // bytecodeBase shifted to the lifted-region start.  IR's
        // bcOffsets are local; bytecodeBase + localBcOffset = method
        // bcOffset = correct ip for deopt resume.
        const uint8_t* lifedBase = bytecodes + startBcOffset;
        uint32_t failedVal = UINT32_MAX;
        Lowering::CompiledFn fn = lowering_.lower(m, &failedVal, lifedBase);
        bcOffsetCache_[key][startBcOffset] = fn;
        // Multi-key cache: register the same fn at every dispatchable
        // bcOff in the lifted region.  Triggers at interior loop tops
        // (middle, inner) reuse this single compile.
        if (fn) {
            for (const auto& entry : m.dispatchableBlocks) {
                uint32_t methodBcOff = startBcOffset + entry.first;
                bcOffsetCache_[key][methodBcOff] = fn;
            }
        }
        // PHARO_SISTA_PER_BC_TRACE=1: log the outcome.
        static const bool trace =
            std::getenv("PHARO_SISTA_PER_BC_TRACE") != nullptr;
        if (trace) {
            static int logCount = 0;
            if (logCount++ < 200) {
                fprintf(stderr,
                    "[SISTA-PER-BC-COMPILE] method=0x%llx bcOff=%u "
                    "result=%s%s\n",
                    (unsigned long long)key, startBcOffset,
                    fn ? "OK" : "fail",
                    fn ? "" : (failedVal != UINT32_MAX
                                ? " (lower-failed)"
                                : (r != LiftResult::kOk
                                    ? " (lift-failed)"
                                    : "")));
            }
        }
        return fn;
    }
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
    bool hasArrayDoSplice = false;  // kCountedLoopArrayDoAccum or kCountedLoopDo
    bool hasSendCallHelper = false;
    for (const auto& v : m.values) {
        if (v.op == Op::kCountedLoopDo
         || v.op == Op::kCountedLoopInjectInto
         || v.op == Op::kCountedLoopIntervalInjectInto
         || v.op == Op::kCountedLoopIntervalDo
         || v.op == Op::kCountedLoopArrayDoAccum
         || v.op == Op::kCountedLoopIntervalDoAccum
         || v.op == Op::kCountedLoopArrayCollect
         || v.op == Op::kCountedLoopWhileTrueAccum) {
            hasSplice = true;
        }
        if (v.op == Op::kCountedLoopDo
         || v.op == Op::kCountedLoopArrayDoAccum
         || v.op == Op::kCountedLoopArrayCollect) {
            hasArrayDoSplice = true;
        }
        if (v.op == Op::kSendUnspeculated) {
            hasSend = true;
        }
        if (v.op == Op::kSendCallHelper) {
            hasSendCallHelper = true;
        }
    }

    // 2026-05-02: the older Array-do/helper-send coexistence gate
    // (skip Sista compile when both appear) is no longer needed —
    // the underlying issue (stale relinquishSlept_ signal causing
    // the helper-send's first step() call to fail) was fixed in
    // jitSistaCallSend by clearing the signal on entry and
    // restoring it on exit.  Set PHARO_SISTA_BLOCK_ARRAYDO_HELPER=1
    // to re-enable the old gate (diagnostic).
    static const bool blockArrayDoHelper =
        std::getenv("PHARO_SISTA_BLOCK_ARRAYDO_HELPER") != nullptr;
    if (hasArrayDoSplice && hasSendCallHelper && blockArrayDoHelper) {
        cache_[key] = nullptr;
        return nullptr;
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
    if (hasSplice && fn) {
        // Mark method as Sista-owned: JITRuntime::noteMethodEntry will
        // skip counting this method, preventing T1 from compiling it
        // and racing with Sista's splice.  See
        // memory/project_t1_vs_sista_race.md.
        spliceMethods_.insert(key);
    }
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
