/*
 * SistaLowering.cpp - IR -> asmjit ARM64
 */
#include "SistaLowering.hpp"

#include <asmjit/asmjit.h>
#include <asmjit/a64.h>

#include <unordered_map>
#include <vector>

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

    // Pre-create a Label for every block, in block-id order.  Branch
    // ops need to be able to refer to labels for successor blocks
    // that haven't been emitted yet.
    std::vector<Label> blockLabels;
    blockLabels.reserve(method.blocks.size());
    for (size_t i = 0; i < method.blocks.size(); i++) {
        blockLabels.push_back(cc.new_label());
    }

    // Walk blocks in ID order.  The builder orders blocks by source
    // bytecode offset, so block 0 is entry.  Phi-node support isn't
    // here yet; multi-block patterns must have empty simulated stacks
    // at boundaries (already enforced in the lifter).
    for (const Block& b : method.blocks) {
        cc.bind(blockLabels[b.id]);
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
            case Op::kStoreInstVar: {
                if (v.operands.size() != 2) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                auto recvIt = regFor.find(v.operands[0]);
                auto valIt  = regFor.find(v.operands[1]);
                if (recvIt == regFor.end() || valIt == regFor.end()) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                // Slot N at byte offset 8 + N*8 from object pointer.
                cc.str(valIt->second,
                        ptr(recvIt->second,
                            8 + static_cast<int>(v.literal) * 8));
                break;
            }
            case Op::kConstantOop: {
                Gp dst = cc.new_gp64("const");
                cc.mov(dst, Imm(v.literal));
                regFor[v.id] = dst;
                break;
            }

            case Op::kPrimAddInt:
            case Op::kPrimSubInt:
            case Op::kPrimMulInt: {
                // Tag-preserving arithmetic on SmallInt Oops (tag 1).
                //   a + b → correct tagged result is a + b - 1
                //   a - b → correct tagged result is a - b + 1
                //   a * b → untag a (>> 3), multiply by b's value
                //           (also untagged), re-tag ((result << 3) | 1)
                // Phase 2 omits overflow / non-SmallInt checks —
                // caller must supply SmallInt inputs within safe range.
                if (v.operands.size() != 2) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                auto ita = regFor.find(v.operands[0]);
                auto itb = regFor.find(v.operands[1]);
                if (ita == regFor.end() || itb == regFor.end()) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                Gp dst = cc.new_gp64("arith");
                if (v.op == Op::kPrimAddInt) {
                    cc.add(dst, ita->second, itb->second);
                    cc.sub(dst, dst, Imm(1));
                } else if (v.op == Op::kPrimSubInt) {
                    cc.sub(dst, ita->second, itb->second);
                    cc.add(dst, dst, Imm(1));
                } else {
                    // Multiply: untag both operands, multiply, re-tag.
                    Gp au = cc.new_gp64("au");
                    Gp bu = cc.new_gp64("bu");
                    cc.lsr(au, ita->second, Imm(3));
                    cc.lsr(bu, itb->second, Imm(3));
                    Gp prod = cc.new_gp64("prod");
                    cc.mul(prod, au, bu);
                    cc.lsl(dst, prod, Imm(3));
                    cc.orr(dst, dst, Imm(1));
                }
                regFor[v.id] = dst;
                break;
            }
            case Op::kReturn: {
                // Operand[0] = value to return.  Emits a tailored
                // epilogue right here instead of jumping to a shared
                // one — the return path is short enough that inlining
                // is cheaper than branching.
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
                cc.ret();
                break;  // Per-block return; keep emitting other blocks.
            }

            case Op::kBranch: {
                // literal = successor block id.  One successor.
                if (b.successors.empty()) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                cc.b(blockLabels[b.successors[0]]);
                break;
            }

            case Op::kBranchIfTrue:
            case Op::kBranchIfFalse: {
                // operand[0] = condition Oop.  successors[0] = taken,
                // successors[1] = fallthrough.  Compare the condition
                // against state->trueOop / state->falseOop; branch on
                // equality.  For Phase 2 we don't yet implement the
                // Smalltalk "non-boolean" mustBeBoolean bail — if the
                // cond is neither true nor false, fall through (incorrect
                // for cond=nil; correct fix adds a deopt in Phase 3).
                if (v.operands.size() != 1 || b.successors.size() != 2) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                auto it = regFor.find(v.operands[0]);
                if (it == regFor.end()) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                Gp trueOop = cc.new_gp64("true");
                cc.ldr(trueOop, ptr(state, OFF_TRUEOOP));
                cc.cmp(it->second, trueOop);
                if (v.op == Op::kBranchIfTrue) {
                    cc.b_eq(blockLabels[b.successors[0]]);  // taken
                    cc.b   (blockLabels[b.successors[1]]);  // fallthrough
                } else {
                    // BranchIfFalse: taken when cond != true.
                    // Simpler: branch to fallthrough when cond == true,
                    // else branch to taken.
                    cc.b_eq(blockLabels[b.successors[1]]);
                    cc.b   (blockLabels[b.successors[0]]);
                }
                break;
            }
            default:
                // Unsupported in this phase — punt.
                if (failedAtValue) *failedAtValue = v.id;
                return nullptr;
            }
        }
    }

    // All blocks emitted.  Finalize and register with the runtime.
    cc.end_func();
    cc.finalize();
    CompiledFn out = nullptr;
    Error err = runtime_->add(&out, &code);
    if (err != kErrorOk) return nullptr;
    return out;
}

}  // namespace sista
}  // namespace pharo
