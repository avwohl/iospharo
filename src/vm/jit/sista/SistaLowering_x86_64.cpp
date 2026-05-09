/*
 * SistaLowering_x86_64.cpp - IR -> asmjit x86_64
 *
 * Per-arch sibling of SistaLowering_arm64.cpp.  Sista is the tier-2
 * JIT path: takes a SistaMethod IR (built by SistaBuilder) and emits
 * native machine code via asmjit.
 *
 * Status (op-by-op port in progress).  Supported IR ops:
 *   kLoadReceiver, kLoadTrueOop, kLoadFalseOop
 *   kLoadTemp, kStoreTemp
 *   kLoadLiteral
 *   kConstantOop
 *   kReturn
 *
 * Any other op causes lowering to bail via *failedAtValue — the
 * containing method falls back to tier-1.  Add the next op when it
 * actually appears as a Sista-bail in profiling.
 *
 * Calling convention matches Tier 1 / Tier 2 arm64:
 *   void fn(JITState* state)   — state in rdi (SysV) / rcx (Win64)
 *   exit via state->returnValue + state->exitReason = EXIT_RETURN
 */
#include "SistaLowering.hpp"

#if PHARO_JIT_ENABLED

#include <asmjit/x86.h>
#include <asmjit/core/jitruntime.h>

#include <atomic>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace pharo {
namespace sista {

namespace {

// Diagnostic: how many methods Sista-compiled vs bailed during this
// run.  Printed every 64 calls so we see it without a clean dtor.
// Removed once op coverage is complete and stats become uninteresting.
struct LowerStats {
    std::atomic<size_t> ok{0};
    std::atomic<size_t> bail{0};
    void tick() {
        size_t total = ok.load() + bail.load();
        if ((total & 63) == 0) {
            fprintf(stderr,
                    "[SISTA-x86] lower OK=%zu bail=%zu (total=%zu)\n",
                    ok.load(), bail.load(), total);
        }
    }
};
LowerStats g_lowerStats;

// JITState offsets — must match Tier 1 / Tier 2 so runtime can invoke
// either transparently.  See src/vm/jit/JITState.hpp.
constexpr int OFF_SP           = 0;
constexpr int OFF_RECEIVER     = 8;
constexpr int OFF_LITERALS     = 16;
constexpr int OFF_TEMPBASE     = 24;
constexpr int OFF_IP           = 48;
constexpr int OFF_EXIT         = 76;
constexpr int OFF_RETVAL       = 80;
constexpr int OFF_TRUEOOP      = 128;
constexpr int OFF_FALSEOOP     = 136;

// ExitReason values (src/vm/jit/JITState.hpp).
constexpr int EXIT_RETURN  = 1;

}  // namespace

Lowering::Lowering() {
    runtime_ = new asmjit::JitRuntime();
}

Lowering::~Lowering() {
    delete runtime_;
}

Lowering::CompiledFn Lowering::lower(const Method& method,
                                       uint32_t* failedAtValue,
                                       const uint8_t* /*bytecodeBase*/) {
    using namespace asmjit;
    using namespace asmjit::x86;

    static bool noLower = getenv("PHARO_SISTA_NO_LOWER") != nullptr;
    if (noLower) {
        if (failedAtValue) *failedAtValue = 0;
        return nullptr;
    }

    CodeHolder code;
    code.init(runtime_->environment(), runtime_->cpu_features());

    Compiler cc(&code);
    FuncNode* fn = cc.add_func(FuncSignature::build<void, void*>());
    Gp state = cc.new_gp64("state");
    fn->set_arg(0, state);

    // Hoist tempBase + litBase out of every per-access load; asmjit's
    // allocator can keep them in callee-saved registers across the
    // function.  Mirrors the arm64 lowerer's hoist.
    Gp tempBaseHoisted = cc.new_gp64("tempBaseH");
    cc.mov(tempBaseHoisted, ptr(state, OFF_TEMPBASE));
    Gp litBaseHoisted = cc.new_gp64("litBaseH");
    cc.mov(litBaseHoisted, ptr(state, OFF_LITERALS));

    // Map each SSA value id -> the virtual register holding its value.
    std::unordered_map<uint32_t, Gp> regFor;

    // Pre-create labels for every block.
    std::vector<Label> blockLabels;
    blockLabels.reserve(method.blocks.size());
    for (size_t i = 0; i < method.blocks.size(); i++) {
        blockLabels.push_back(cc.new_label());
    }

    auto bail = [&](uint32_t vid) -> CompiledFn {
        if (failedAtValue) *failedAtValue = vid;
        g_lowerStats.bail.fetch_add(1, std::memory_order_relaxed);
        g_lowerStats.tick();
        return nullptr;
    };

    // Walk blocks in ID order (builder emits them in source-bytecode order).
    for (const Block& b : method.blocks) {
        cc.bind(blockLabels[b.id]);

        // Per-block CSE for temp loads — same shape as arm64.
        std::unordered_map<uint64_t, Gp> tempCache;

        for (uint32_t vid : b.values) {
            const Value& v = method.valueAt(vid);
            switch (v.op) {

            case Op::kLoadReceiver: {
                Gp dst = cc.new_gp64("recv");
                cc.mov(dst, ptr(state, OFF_RECEIVER));
                regFor[v.id] = dst;
                break;
            }

            case Op::kLoadTrueOop: {
                Gp dst = cc.new_gp64("true");
                cc.mov(dst, ptr(state, OFF_TRUEOOP));
                regFor[v.id] = dst;
                break;
            }

            case Op::kLoadFalseOop: {
                Gp dst = cc.new_gp64("false");
                cc.mov(dst, ptr(state, OFF_FALSEOOP));
                regFor[v.id] = dst;
                break;
            }

            case Op::kLoadTemp: {
                auto cached = tempCache.find(v.literal);
                if (cached != tempCache.end()) {
                    regFor[v.id] = cached->second;
                    break;
                }
                Gp dst = cc.new_gp64("temp");
                cc.mov(dst, ptr(tempBaseHoisted,
                                static_cast<int>(v.literal) * 8));
                regFor[v.id] = dst;
                tempCache[v.literal] = dst;
                break;
            }

            case Op::kStoreTemp: {
                if (v.operands.size() != 1) return bail(v.id);
                auto it = regFor.find(v.operands[0]);
                if (it == regFor.end()) return bail(v.id);
                cc.mov(ptr(tempBaseHoisted,
                           static_cast<int>(v.literal) * 8),
                       it->second);
                tempCache[v.literal] = it->second;
                break;
            }

            case Op::kLoadLiteral: {
                Gp dst = cc.new_gp64("lit");
                cc.mov(dst, ptr(litBaseHoisted,
                                static_cast<int>(v.literal) * 8));
                regFor[v.id] = dst;
                break;
            }

            case Op::kConstantOop: {
                Gp dst = cc.new_gp64("const");
                cc.mov(dst, Imm(v.literal));
                regFor[v.id] = dst;
                break;
            }

            case Op::kReturn: {
                if (v.operands.size() != 1) return bail(v.id);
                auto it = regFor.find(v.operands[0]);
                if (it == regFor.end()) return bail(v.id);
                cc.mov(ptr(state, OFF_RETVAL), it->second);
                Gp exit = cc.new_gp32("exit");
                cc.mov(exit, Imm(EXIT_RETURN));
                cc.mov(ptr(state, OFF_EXIT), exit);
                cc.ret();
                break;  // Per-block return; keep emitting other blocks.
            }

            default:
                // Unsupported op — bail.  Caller falls back to tier-1.
                return bail(v.id);
            }
        }
    }

    cc.end_func();
    cc.finalize();

    CompiledFn out = nullptr;
    Error err = runtime_->add(&out, &code);
    if (err != kErrorOk) {
        if (failedAtValue) *failedAtValue = 0;
        g_lowerStats.bail.fetch_add(1, std::memory_order_relaxed);
        g_lowerStats.tick();
        return nullptr;
    }
    g_lowerStats.ok.fetch_add(1, std::memory_order_relaxed);
    g_lowerStats.tick();
    return out;
}

}  // namespace sista
}  // namespace pharo

#endif  // PHARO_JIT_ENABLED
