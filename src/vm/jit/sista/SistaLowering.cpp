/*
 * SistaLowering.cpp - IR -> asmjit ARM64
 */
#include "SistaLowering.hpp"

#include <asmjit/asmjit.h>
#include <asmjit/a64.h>

#include <unordered_map>

namespace pharo {
namespace sista {

namespace {

// JITState offsets — must match Tier 1 / Tier 2 so runtime can invoke
// either transparently.  See src/vm/jit/JITState.hpp.
constexpr int OFF_RECEIVER = 8;
constexpr int OFF_LITERALS = 16;
constexpr int OFF_TEMPBASE = 24;
constexpr int OFF_EXIT     = 76;
constexpr int OFF_RETVAL   = 80;
constexpr int OFF_TRUEOOP  = 128;
constexpr int OFF_FALSEOOP = 136;

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
                                       uint32_t* failedAtValue) {
    using namespace asmjit;
    using namespace asmjit::a64;

    CodeHolder code;
    code.init(runtime_->environment(), runtime_->cpu_features());
    Compiler cc(&code);
    FuncNode* fn = cc.add_func(FuncSignature::build<void, void*>());
    Gp state = cc.new_gp64("state");
    fn->set_arg(0, state);

    // Map each SSA value id to the virtual register holding its
    // materialized value.  Only records values whose type is produced
    // (kOop, kInt, …); terminators have no register.
    std::unordered_map<uint32_t, Gp> regFor;

    // Walk blocks in ID order.  Phase 2 doesn't support control flow
    // past a single block — the builder only produces one — so we
    // don't need a worklist yet.
    for (const Block& b : method.blocks) {
        for (uint32_t vid : b.values) {
            const Value& v = method.valueAt(vid);
            switch (v.op) {
            case Op::kLoadReceiver: {
                Gp dst = cc.new_gp64("recv");
                cc.ldr(dst, ptr(state, OFF_RECEIVER));
                regFor[v.id] = dst;
                break;
            }
            case Op::kLoadTrueOop: {
                Gp dst = cc.new_gp64("true");
                cc.ldr(dst, ptr(state, OFF_TRUEOOP));
                regFor[v.id] = dst;
                break;
            }
            case Op::kLoadFalseOop: {
                Gp dst = cc.new_gp64("false");
                cc.ldr(dst, ptr(state, OFF_FALSEOOP));
                regFor[v.id] = dst;
                break;
            }
            case Op::kLoadTemp: {
                Gp tempBase = cc.new_gp64("tempBase");
                Gp dst      = cc.new_gp64("temp");
                cc.ldr(tempBase, ptr(state, OFF_TEMPBASE));
                cc.ldr(dst, ptr(tempBase, static_cast<int>(v.literal) * 8));
                regFor[v.id] = dst;
                break;
            }
            case Op::kLoadLiteral: {
                Gp litBase = cc.new_gp64("litBase");
                Gp dst     = cc.new_gp64("lit");
                cc.ldr(litBase, ptr(state, OFF_LITERALS));
                cc.ldr(dst, ptr(litBase, static_cast<int>(v.literal) * 8));
                regFor[v.id] = dst;
                break;
            }
            case Op::kLoadInstVar: {
                // operand[0] is the object pointer (Oop).  Smalltalk
                // slots live at offsets 8 + N*8 from an object pointer
                // (slot 0 at +8 because sizeof(ObjectHeader) == 8).
                if (v.operands.size() != 1) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                auto it = regFor.find(v.operands[0]);
                if (it == regFor.end()) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                Gp dst = cc.new_gp64("ivar");
                cc.ldr(dst, ptr(it->second,
                                 8 + static_cast<int>(v.literal) * 8));
                regFor[v.id] = dst;
                break;
            }
            case Op::kStoreTemp: {
                if (v.operands.size() != 1) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                auto it = regFor.find(v.operands[0]);
                if (it == regFor.end()) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                Gp tempBase = cc.new_gp64("tempBase");
                cc.ldr(tempBase, ptr(state, OFF_TEMPBASE));
                cc.str(it->second, ptr(tempBase, static_cast<int>(v.literal) * 8));
                // kStoreTemp produces void — nothing to record.
                break;
            }
            case Op::kConstantOop: {
                Gp dst = cc.new_gp64("const");
                cc.mov(dst, Imm(v.literal));
                regFor[v.id] = dst;
                break;
            }
            case Op::kReturn: {
                // Operand[0] = value to return.
                if (v.operands.size() != 1) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                auto it = regFor.find(v.operands[0]);
                if (it == regFor.end()) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                cc.str(it->second, ptr(state, OFF_RETVAL));
                Gp exit = cc.new_gp32("exit");
                cc.mov(exit, EXIT_RETURN);
                cc.str(exit, ptr(state, OFF_EXIT));
                cc.end_func();
                cc.finalize();
                CompiledFn out = nullptr;
                Error err = runtime_->add(&out, &code);
                if (err != kErrorOk) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                return out;
            }
            default:
                // Unsupported in this phase — punt.
                if (failedAtValue) *failedAtValue = v.id;
                return nullptr;
            }
        }
    }

    // Ran through without hitting a return — malformed.
    if (failedAtValue && !method.values.empty())
        *failedAtValue = method.values.back().id;
    return nullptr;
}

}  // namespace sista
}  // namespace pharo
