/*
 * SistaLowering.cpp - IR -> asmjit ARM64
 */
#include "SistaLowering.hpp"

#if PHARO_JIT_ENABLED

// Match Tier2Compiler's asmjit include pattern — pulling in
// <asmjit/asmjit.h> instantiates a 2-arg CodeHolder::init inline
// that references a symbol the xcframework's asmjit.a doesn't
// export (3-arg form).
#include <asmjit/a64.h>
#include <asmjit/core/jitruntime.h>

#include <unordered_map>
#include <vector>

namespace pharo {
namespace sista {

namespace {

// JITState offsets — must match Tier 1 / Tier 2 so runtime can invoke
// either transparently.  See src/vm/jit/JITState.hpp.
constexpr int OFF_SP           = 0;
constexpr int OFF_RECEIVER     = 8;
constexpr int OFF_LITERALS     = 16;
constexpr int OFF_TEMPBASE     = 24;
constexpr int OFF_IP           = 48;
constexpr int OFF_EXIT         = 76;
constexpr int OFF_RETVAL       = 80;
constexpr int OFF_ICDATAPTR    = 96;
constexpr int OFF_SENDARGCOUNT = 104;
constexpr int OFF_TRUEOOP      = 128;
constexpr int OFF_FALSEOOP     = 136;

// ExitReason values (src/vm/jit/JITState.hpp).
constexpr int EXIT_RETURN  = 1;
constexpr int EXIT_SEND    = 2;

}  // namespace

Lowering::Lowering() {
    runtime_ = new asmjit::JitRuntime();
}

Lowering::~Lowering() {
    delete runtime_;
}

Lowering::CompiledFn Lowering::lower(const Method& method,
                                       uint32_t* failedAtValue,
                                       const uint8_t* bytecodeBase) {
    using namespace asmjit;
    using namespace asmjit::a64;

    // PHARO_SISTA_NO_LOWER=1 — short-circuit lowering for bisect.
    // Confirms whether the residual jit-default crash is in the asmjit
    // emit path itself (corrupted code zone, runtime allocator issue,
    // W^X leak) versus higher-level Sista state.
    static bool noLower = getenv("PHARO_SISTA_NO_LOWER") != nullptr;
    if (noLower) {
        if (failedAtValue) *failedAtValue = 0;
        return nullptr;
    }

    CodeHolder code;
    code.init(runtime_->environment(), runtime_->cpu_features());

    // PHARO_SISTA_NO_LOWER_BODY=1 — bisect: emit a function that
    // immediately returns (just ret), without any IR-driven body.
    // If this still crashes the run, the bug is in asmjit's
    // CodeHolder/Compiler/Runtime::add infrastructure (or its
    // MAP_JIT W^X interaction with our CodeZone), not in any
    // specific IR-op emission.
    static bool noBody = getenv("PHARO_SISTA_NO_LOWER_BODY") != nullptr;
    if (noBody) {
        Compiler ccx(&code);
        FuncNode* fnx = ccx.add_func(FuncSignature::build<void, void*>());
        Gp statex = ccx.new_gp64("state");
        fnx->set_arg(0, statex);
        // state.exitReason = ExitSend so caller falls back to interpreter.
        Gp exitx = ccx.new_gp32("exit");
        ccx.mov(exitx, Imm(EXIT_SEND));
        ccx.str(exitx, ptr(statex, OFF_EXIT));
        ccx.ret();
        ccx.end_func();
        ccx.finalize();
        CompiledFn outx = nullptr;
        Error errx = runtime_->add(&outx, &code);
        if (errx != kErrorOk) return nullptr;
        return outx;
    }

    // PHARO_SISTA_NO_LOWER_ADD=1 — bisect: build the asmjit code
    // graph but DO NOT call runtime_->add().  If this is clean, the
    // bug is specifically in JitRuntime's allocator + W^X dance.
    static bool noAdd = getenv("PHARO_SISTA_NO_LOWER_ADD") != nullptr;
    (void)noAdd;

    // (PHARO_SISTA_FRESH_RUNTIME bisect tried — recreates asmjit
    // JitRuntime per compile.  Result: VM hangs after ~10K steps
    // because previous runtime's emitted fns are still referenced
    // from sista::Runtime::cache_ and now point to freed memory.
    // Removing the gate; not a useful avenue.)
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

    // Pre-allocate virtual regs for every phi in every block.  These
    // regs are written by predecessors (as MOVs before their branch
    // terminators) and read by the phi's block as its entry stack
    // contents.  Allocating them up front means predecessors can
    // reference them even though their owning block hasn't been
    // visited yet.
    for (const Block& b : method.blocks) {
        for (uint32_t vid : b.values) {
            const Value& v = method.valueAt(vid);
            if (v.op != Op::kPhi) break;  // phis are always block-leading
            regFor[v.id] = cc.new_gp64("phi");
        }
    }

    // Helper: emit MOVs that copy this block's outgoingStack into
    // `succ`'s phi regs.  Called right before each terminator branch
    // (and at end-of-block for implicit fall-through) so that the
    // phi regs hold the right values on entry to the successor.
    auto fillPhis = [&](const Block& from, uint32_t succId) {
        const Block& succ = method.blockAt(succId);
        size_t phiIdx = 0;
        for (uint32_t phiVid : succ.values) {
            const Value& pv = method.valueAt(phiVid);
            if (pv.op != Op::kPhi) break;
            if (phiIdx >= from.outgoingStack.size()) break;
            auto srcIt = regFor.find(from.outgoingStack[phiIdx]);
            if (srcIt != regFor.end()) {
                cc.mov(regFor[phiVid], srcIt->second);
            }
            phiIdx++;
        }
    };

    // Walk blocks in ID order.  The builder orders blocks by source
    // bytecode offset, so block 0 is entry.
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
                // SAFETY BAIL: the historic lowering emitted a direct
                // `str val, [recv + 8 + N*8]` with NO immutability check
                // and NO write-barrier callout — the interpreter's
                // setReceiverInstVar does both (raises ModificationForbidden
                // on immutable receivers, calls rememberObject when an old
                // object gets a young oop slot).  Without those, lowered
                // code can:
                //   (a) silently mutate a frozen object, breaking the
                //       Pharo readOnly contract, or
                //   (b) leave an unremembered old→young pointer, which
                //       scavenge will then fail to update on object move.
                // Both are silent miscompiles.  An activate-time gate
                // (Interpreter.cpp ~6841) marks methods with PopStoreRecv*
                // bytecodes as unsafe so they never *dispatch*, but the
                // unsafe code was still being emitted into the asmjit
                // zone at compile time.  We refuse to lower instead — if
                // the dispatch gate ever loosens, this is the second line
                // of defense against silent miscompile.
                //
                // Plan to re-enable: emit the same `attemptToAssign:withIndex:`
                // callout the interpreter uses, plus the rememberObject
                // write-barrier when isOld(recv) && isYoung(val).
                if (failedAtValue) *failedAtValue = v.id;
                return nullptr;
            }
            case Op::kConstantOop: {
                Gp dst = cc.new_gp64("const");
                cc.mov(dst, Imm(v.literal));
                regFor[v.id] = dst;
                break;
            }

            case Op::kPrimTagCheckInt: {
                // Phase 3 deopt: check operand[0]'s low 3 bits == 1
                // (SmallInt tag).  On miss, bail to interpreter.
                // operand[0]      = value to check
                // operand[1..N]   = simulated stack at deopt point
                //                   (rebuilt onto interpreter stack
                //                   so the interpreter can re-execute
                //                   the source bytecode)
                // literal         = bcOffset to deopt to
                //
                // Output (on hit) = operand[0] passthrough, retyped
                // OopSmallInt for downstream type-aware ops.
                if (v.operands.empty()) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                auto itVal = regFor.find(v.operands[0]);
                if (itVal == regFor.end()) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }

                using namespace asmjit::a64;
                Label missLabel = cc.new_label();
                Label contLabel = cc.new_label();

                // tst sets condition flags from (val & imm).
                // imm value = 7; result NZ if any low-3-bit set, but
                // we want SmallIntTag == 1.  Use AND + cmp instead.
                Gp tagBits = cc.new_gp64("tagbits");
                cc.and_(tagBits, itVal->second, Imm(7));
                cc.cmp(tagBits, Imm(1));
                cc.b_eq(contLabel);  // SmallInt — proceed

                // -- Miss: emit deopt sequence.  Same shape as
                //    kSendUnspeculated bail.
                cc.bind(missLabel);
                uint32_t bcOffset = static_cast<uint32_t>(v.literal);

                // Push operand[1..N] to interp stack.
                Gp sp = cc.new_gp64("sp");
                cc.ldr(sp, ptr(state, OFF_SP));
                for (size_t opIdx = 1; opIdx < v.operands.size(); opIdx++) {
                    auto opIt = regFor.find(v.operands[opIdx]);
                    if (opIt == regFor.end()) {
                        if (failedAtValue) *failedAtValue = v.id;
                        return nullptr;
                    }
                    cc.str(opIt->second,
                           ptr(sp, static_cast<int>(opIdx - 1) * 8));
                }
                size_t numStackOps = v.operands.size() - 1;
                if (numStackOps > 0) {
                    cc.add(sp, sp, Imm(static_cast<int>(numStackOps) * 8));
                }
                cc.str(sp, ptr(state, OFF_SP));

                // state.ip = bcOffset (or absolute pointer with base)
                Gp ipReg = cc.new_gp64("ip");
                if (bytecodeBase) {
                    uintptr_t addr = reinterpret_cast<uintptr_t>(bytecodeBase)
                                   + bcOffset;
                    cc.mov(ipReg, Imm((uint64_t)addr));
                } else {
                    cc.mov(ipReg, Imm(bcOffset));
                }
                cc.str(ipReg, ptr(state, OFF_IP));

                // state.icDataPtr = 0 (no IC speculation)
                Gp zero64 = cc.new_gp64("zero64");
                cc.mov(zero64, Imm(0));
                cc.str(zero64, ptr(state, OFF_ICDATAPTR));

                // state.exitReason = ExitSend
                Gp exit = cc.new_gp32("exit");
                cc.mov(exit, Imm(EXIT_SEND));
                cc.str(exit, ptr(state, OFF_EXIT));
                cc.ret();

                // -- Hit: passthrough output = input
                cc.bind(contLabel);
                regFor[v.id] = itVal->second;
                break;
            }

            case Op::kPrimLtInt:
            case Op::kPrimLeInt:
            case Op::kPrimGtInt:
            case Op::kPrimGeInt:
            case Op::kPrimEqInt:
            case Op::kPrimNeqInt: {
                // SmallInt comparison.  Tagged-int representation
                // preserves signed ordering (tag is +1 in low 3
                // bits; multiplication by 8 from value-to-encoded
                // doesn't change order).  No tag check, no overflow
                // — same Phase 2 caveat as kPrimAddInt: caller
                // must supply SmallInt operands.  Same env-var gate.
                static bool noArith =
                    getenv("PHARO_SISTA_NO_LOWER_ARITH") != nullptr;
                if (noArith) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
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
                Gp trueOop = cc.new_gp64("true");
                Gp falseOop = cc.new_gp64("false");
                cc.ldr(trueOop, ptr(state, OFF_TRUEOOP));
                cc.ldr(falseOop, ptr(state, OFF_FALSEOOP));
                Gp dst = cc.new_gp64("intcmp");
                cc.cmp(ita->second, itb->second);
                using namespace asmjit::a64;
                CondCode cond;
                switch (v.op) {
                  case Op::kPrimLtInt:  cond = CondCode::kLT; break;
                  case Op::kPrimLeInt:  cond = CondCode::kLE; break;
                  case Op::kPrimGtInt:  cond = CondCode::kGT; break;
                  case Op::kPrimGeInt:  cond = CondCode::kGE; break;
                  case Op::kPrimEqInt:  cond = CondCode::kEQ; break;
                  case Op::kPrimNeqInt: cond = CondCode::kNE; break;
                  default: cond = CondCode::kEQ; break;
                }
                cc.csel(dst, trueOop, falseOop, cond);
                regFor[v.id] = dst;
                break;
            }
            case Op::kPrimIdentityEq:
            case Op::kPrimIdentityNeq: {
                // Phase 4 inline of #== / #~~: compare a's bits to b's
                // bits, result is trueOop if matching the op's
                // condition, else falseOop.  No tag check, no deopt —
                // identity semantics are universal across all classes.
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
                Gp trueOop = cc.new_gp64("true");
                Gp falseOop = cc.new_gp64("false");
                cc.ldr(trueOop, ptr(state, OFF_TRUEOOP));
                cc.ldr(falseOop, ptr(state, OFF_FALSEOOP));
                Gp dst = cc.new_gp64("idcmp");
                cc.cmp(ita->second, itb->second);
                if (v.op == Op::kPrimIdentityEq) {
                    cc.csel(dst, trueOop, falseOop, asmjit::a64::CondCode::kEQ);
                } else {  // kPrimIdentityNeq
                    cc.csel(dst, trueOop, falseOop, asmjit::a64::CondCode::kNE);
                }
                regFor[v.id] = dst;
                break;
            }

            case Op::kPrimAddInt:
            case Op::kPrimSubInt:
            case Op::kPrimMulInt: {
                // PHARO_SISTA_NO_LOWER_ARITH=1 — bisect: refuse to
                // lower inline arithmetic ops.
                static bool noArith = getenv("PHARO_SISTA_NO_LOWER_ARITH") != nullptr;
                if (noArith) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                // Tag-preserving arithmetic on SmallInt Oops (tag 1).
                //   a + b → correct tagged result is a + b - 1
                //   a - b → correct tagged result is a - b + 1
                //   a * b → untag a (>> 3), multiply by b's value
                //           (also untagged), re-tag ((result << 3) | 1)
                //
                // PHASE 3 OVERFLOW DETECTION: when v.operands.size()
                // > 2, the trailing operands are a deopt-stack
                // snapshot and v.literal is the bcOffset to deopt to.
                // After tagged add/sub, check that bits 62, 63 of
                // the result match (no SmallInt overflow); on miss
                // bail to the interpreter at the source bytecode.
                // Mul still has no overflow detection (TODO).
                if (v.operands.size() < 2) {
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
                // Track whether the IR op carries deopt info (operands.size > 2).
                // Mul uses asr (not lsr) to correctly handle negative SmallInts.
                Gp mulProd64;  // For mul overflow path: holds untagged 60-bit product
                if (v.op == Op::kPrimAddInt) {
                    cc.add(dst, ita->second, itb->second);
                    cc.sub(dst, dst, Imm(1));
                } else if (v.op == Op::kPrimSubInt) {
                    cc.sub(dst, ita->second, itb->second);
                    cc.add(dst, dst, Imm(1));
                } else {
                    // Multiply: untag both operands (asr — signed shift —
                    // for negatives), multiply, re-tag.
                    Gp au = cc.new_gp64("au");
                    Gp bu = cc.new_gp64("bu");
                    cc.asr(au, ita->second, Imm(3));
                    cc.asr(bu, itb->second, Imm(3));
                    Gp prod = cc.new_gp64("prod");
                    cc.mul(prod, au, bu);
                    mulProd64 = prod;  // share with overflow check
                    cc.lsl(dst, prod, Imm(3));
                    cc.orr(dst, dst, Imm(1));
                    // Overflow check requires smulh too (full 128-bit
                    // product).  Saved for the deopt block below.
                }

                // Overflow detection (when deopt info present).
                // Add/sub use the bit-62/63-mismatch trick on the
                // tagged result.  Mul needs the full 128-bit product
                // (smulh) to verify the value fits in 60-bit signed.
                if (v.operands.size() > 2) {
                    using namespace asmjit::a64;
                    Label missLabel = cc.new_label();
                    Label contLabel = cc.new_label();

                    if (v.op == Op::kPrimMulInt) {
                        // Mul overflow: full product must fit in
                        // 60-bit signed (range [-2^59, 2^59-1]).
                        // 1. Compute high 64 bits via smulh.
                        // 2. The full 128-bit product fits in 60-bit
                        //    signed iff smulh equals (low asr 59) —
                        //    i.e., bits 60..127 all equal bit 59.
                        // Need au and bu in scope; recompute since
                        // they were locally scoped above.
                        Gp au = cc.new_gp64("ovau");
                        Gp bu = cc.new_gp64("ovbu");
                        cc.asr(au, ita->second, Imm(3));
                        cc.asr(bu, itb->second, Imm(3));
                        Gp high = cc.new_gp64("ovhigh");
                        cc.smulh(high, au, bu);
                        // expected high = sign-extension of bit 59 of mulProd64
                        Gp expected = cc.new_gp64("ovexp");
                        cc.asr(expected, mulProd64, Imm(59));
                        cc.cmp(high, expected);
                        cc.b_ne(missLabel);
                        // Also check: bits 60-63 of mulProd64 all
                        // equal bit 59 (they're in the same word).
                        // Otherwise smulh check passes but bits 60-63
                        // hold value bits.
                        Gp shifted2 = cc.new_gp64("ovsh2");
                        cc.lsl(shifted2, mulProd64, Imm(4));
                        cc.asr(shifted2, shifted2, Imm(4));
                        cc.cmp(shifted2, mulProd64);
                        cc.b_ne(missLabel);
                        cc.b(contLabel);
                    } else {
                        // Add/sub: bit-62/63 mismatch on tagged result.
                        // Tagged SmallInt valid iff bit 62 == bit 63.
                        //   ovcheck = (dst << 1) ^ dst
                        //   bit 63 of ovcheck = (dst[63] ^ dst[62])
                        //   tbnz on bit 63 → mismatch → overflow
                        Gp ovcheck = cc.new_gp64("ovcheck");
                        Gp shifted = cc.new_gp64("ovshift");
                        cc.lsl(shifted, dst, Imm(1));
                        cc.eor(ovcheck, shifted, dst);
                        cc.tbnz(ovcheck, Imm(63), missLabel);
                        cc.b(contLabel);
                    }

                    // Miss: same deopt sequence as kPrimTagCheckInt.
                    cc.bind(missLabel);
                    uint32_t bcOffset = static_cast<uint32_t>(v.literal);
                    Gp sp = cc.new_gp64("ovsp");
                    cc.ldr(sp, ptr(state, OFF_SP));
                    for (size_t opIdx = 2; opIdx < v.operands.size(); opIdx++) {
                        auto opIt = regFor.find(v.operands[opIdx]);
                        if (opIt == regFor.end()) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        cc.str(opIt->second,
                               ptr(sp, static_cast<int>(opIdx - 2) * 8));
                    }
                    size_t numStackOps = v.operands.size() - 2;
                    if (numStackOps > 0) {
                        cc.add(sp, sp, Imm(static_cast<int>(numStackOps) * 8));
                    }
                    cc.str(sp, ptr(state, OFF_SP));
                    Gp ipReg = cc.new_gp64("ovip");
                    if (bytecodeBase) {
                        uintptr_t addr = reinterpret_cast<uintptr_t>(bytecodeBase)
                                       + bcOffset;
                        cc.mov(ipReg, Imm((uint64_t)addr));
                    } else {
                        cc.mov(ipReg, Imm(bcOffset));
                    }
                    cc.str(ipReg, ptr(state, OFF_IP));
                    Gp zero64 = cc.new_gp64("ovzero");
                    cc.mov(zero64, Imm(0));
                    cc.str(zero64, ptr(state, OFF_ICDATAPTR));
                    Gp exit = cc.new_gp32("ovexit");
                    cc.mov(exit, Imm(EXIT_SEND));
                    cc.str(exit, ptr(state, OFF_EXIT));
                    cc.ret();

                    cc.bind(contLabel);
                }

                regFor[v.id] = dst;
                break;
            }
            case Op::kSendUnspeculated: {
                // PHARO_SISTA_NO_LOWER_SENDS=1 — bisect: refuse to
                // lower send-bail blocks, fall through to interpreter.
                static bool noSends = getenv("PHARO_SISTA_NO_LOWER_SENDS") != nullptr;
                if (noSends) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                // Bail to the interpreter at the send bytecode.
                // Operands carry rcvr + args (in stack order); we push
                // them onto the interpreter stack at state.sp, set
                // state.ip to the send bc, state.sendArgCount = nArgs,
                // state.icDataPtr = 0 (no IC speculation yet), and
                // state.exitReason = ExitSend.
                //
                // Pattern mirrors Tier2Compiler's ExitSend bail, minus
                // the IC-hit fast path.  The interpreter resumes at
                // state.ip and runs the send plus all subsequent
                // bytecodes; compiled code never continues past here.
                uint32_t selIdx   =  v.literal        & 0xFFFF;
                uint32_t nArgs    = (v.literal >> 16) & 0xFF;
                uint32_t bcOffset = (v.literal >> 24) & 0xFFFFFFFF;
                (void)selIdx;  // Selector resolution is the interpreter's job.

                // Push every operand onto the interpreter stack.  For a
                // send the operands are [rcvr, arg0, ..., arg_{nArgs-1}]
                // (so operands.size() == nArgs + 1).  For a generic
                // mid-method bail (PushFullBlock, PushArray, etc.), the
                // operands are the entire IR stack at bail time.
                //
                // Use indexed addressing [sp + i*8] rather than
                // iterating `add sp, sp, 8` per step so the register
                // allocator doesn't have to keep sp hot through every
                // push.  Final sp is written once at the end.
                Gp sp = cc.new_gp64("sp");
                cc.ldr(sp, ptr(state, OFF_SP));
                for (size_t opIdx = 0; opIdx < v.operands.size(); opIdx++) {
                    auto it = regFor.find(v.operands[opIdx]);
                    if (it == regFor.end()) {
                        if (failedAtValue) *failedAtValue = v.id;
                        return nullptr;
                    }
                    cc.str(it->second,
                           ptr(sp, static_cast<int>(opIdx) * 8));
                }
                cc.add(sp, sp, Imm(static_cast<int>(v.operands.size()) * 8));
                cc.str(sp, ptr(state, OFF_SP));

                // state.ip = absolute pointer into the method's bytecode.
                // When `bytecodeBase` was passed to lower(), we bake it
                // in and produce the real pointer the interpreter
                // expects.  Without a base (unit tests), we leave just
                // the bytecode offset as a placeholder so callers can
                // verify the encoding.
                Gp ipReg = cc.new_gp64("ip");
                if (bytecodeBase) {
                    uintptr_t addr = reinterpret_cast<uintptr_t>(bytecodeBase)
                                   + bcOffset;
                    cc.mov(ipReg, Imm((uint64_t)addr));
                } else {
                    cc.mov(ipReg, Imm(bcOffset));
                }
                cc.str(ipReg, ptr(state, OFF_IP));

                // state.sendArgCount = nArgs.
                Gp argCountReg = cc.new_gp32("argc");
                cc.mov(argCountReg, Imm(nArgs));
                cc.str(argCountReg, ptr(state, OFF_SENDARGCOUNT));

                // state.icDataPtr = 0 — no IC speculation yet.
                Gp zero64 = cc.new_gp64("zero64");
                cc.mov(zero64, Imm(0));
                cc.str(zero64, ptr(state, OFF_ICDATAPTR));

                // state.exitReason = ExitSend.
                Gp exit = cc.new_gp32("exit");
                cc.mov(exit, Imm(EXIT_SEND));
                cc.str(exit, ptr(state, OFF_EXIT));
                cc.ret();
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

            case Op::kPhi: {
                // Pre-allocated above; predecessors MOV into its reg
                // before their branch.  No code here.
                break;
            }
            case Op::kBranch: {
                // literal = successor block id.  One successor.
                if (b.successors.empty()) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                fillPhis(b, b.successors[0]);
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
                // Both successors may have phis; fill both sets of
                // phi regs before branching.  The untaken side's MOVs
                // are dead but cheap.
                fillPhis(b, b.successors[0]);
                fillPhis(b, b.successors[1]);
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
        // Implicit fall-through: block didn't end in an explicit
        // terminator op (kBranch / kBranchIfTrue / kBranchIfFalse /
        // kReturn / kSendUnspeculated).  Control flows naturally to
        // b.successors[0]'s label, but we still need to fill its phi
        // regs with our outgoing stack values first.
        bool endsInTerminator = false;
        if (!b.values.empty()) {
            Op lastOp = method.valueAt(b.values.back()).op;
            endsInTerminator = OpInfo::isTerminator(lastOp)
                            || lastOp == Op::kSendUnspeculated;
        }
        if (!endsInTerminator && !b.successors.empty()) {
            fillPhis(b, b.successors[0]);
        }
    }

    // All blocks emitted.  Finalize and register with the runtime.
    cc.end_func();
    cc.finalize();
    if (noAdd) {
        // Bisect: built the graph but skip runtime_->add to confirm
        // the JitRuntime allocator is the corruption source.  Return
        // nullptr so caller falls back to interpreter.
        return nullptr;
    }
    CompiledFn out = nullptr;
    Error err = runtime_->add(&out, &code);
    if (err != kErrorOk) return nullptr;
    // PHARO_SISTA_TRACE_ADD=1 — log every successful asmjit add()
    // address, so we can correlate addresses across compiles and
    // check for adjacency / aliasing with our T1 CodeZone.
    static bool traceAdd = getenv("PHARO_SISTA_TRACE_ADD") != nullptr;
    if (traceAdd) {
        static size_t addCount = 0;
        addCount++;
        if (addCount <= 50) {
            fprintf(stderr, "[SISTA-TRACE-ADD] #%zu fn=%p\n",
                    addCount, (void*)out);
        }
    }
    return out;
}

}  // namespace sista
}  // namespace pharo

#else  // !PHARO_JIT_ENABLED

// Provide stubs so callers link cleanly on JIT-disabled slices
// (xcframework / iOS).  Sista is a JIT feature; without the JIT
// backend the lowerer can't emit anything.
namespace pharo {
namespace sista {

Lowering::Lowering() {}
Lowering::~Lowering() {}

Lowering::CompiledFn Lowering::lower(const Method&, uint32_t* failedAtValue,
                                       const uint8_t*) {
    if (failedAtValue) *failedAtValue = 0;
    return nullptr;
}

}  // namespace sista
}  // namespace pharo

#endif  // PHARO_JIT_ENABLED
