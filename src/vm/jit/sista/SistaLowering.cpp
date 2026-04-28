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

// Sista runtime helpers called from compiled code via asmjit cc.invoke.
// Defined in src/vm/jit/JITRuntime.cpp.
extern "C" uint64_t jit_rt_sista_block_create(void* state,
                                                uint64_t litIndex,
                                                uint64_t numCopied,
                                                uint64_t flags);
extern "C" uint64_t jit_rt_sista_basic_size(void* state,
                                              uint64_t rcvBits);
extern "C" uint64_t jit_rt_sista_basic_at(void* state,
                                            uint64_t rcvBits,
                                            uint64_t idxBits);
extern "C" uint64_t jit_rt_sista_call_send(void* state,
                                             uint64_t selBits,
                                             uint64_t nArgs);

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

    // PHARO_SISTA_ASMJIT_LOG=1 — dump asmjit IR + emitted machine
    // code to stderr.  Useful when diagnosing emit-time failures
    // (e.g., cc.invoke not generating a `bl` instruction).
    static asmjit::FileLogger* asmjitLogger = []() -> asmjit::FileLogger* {
        if (getenv("PHARO_SISTA_ASMJIT_LOG"))
            return new asmjit::FileLogger(stderr);
        return nullptr;
    }();
    if (asmjitLogger) {
        code.set_logger(asmjitLogger);
    }

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

            case Op::kGuardClass: {
                // Phase 4 monomorphic-inline guard: receiver's
                // classIndex must match the inlined target's expected
                // class.  On miss, deopt to interpreter at the source
                // bytecode (the unspeculated send re-runs).
                //
                // operand[0]      = receiver value
                // operand[1..N]   = simulated stack at deopt point
                // literal lo32    = expectedClassIndex (22 bits used)
                // literal hi32    = bcOffset to deopt to
                if (v.operands.empty()) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                auto itRcv = regFor.find(v.operands[0]);
                if (itRcv == regFor.end()) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }

                using namespace asmjit::a64;
                Label missLabel = cc.new_label();
                Label contLabel = cc.new_label();

                uint32_t expectedIdx = static_cast<uint32_t>(v.literal & 0x3FFFFF);
                uint32_t bcOffset = static_cast<uint32_t>(v.literal >> 32);

                // Tag check: immediates have low 3 bits != 0; we only
                // guard object receivers.  Non-object → deopt.
                Gp tagBits = cc.new_gp64("tag");
                cc.and_(tagBits, itRcv->second, Imm(7));
                cc.cmp(tagBits, Imm(0));
                cc.b_ne(missLabel);

                // ldr header word from receiver
                Gp hdr = cc.new_gp64("hdr");
                cc.ldr(hdr, ptr(itRcv->second, 0));
                Gp idx = cc.new_gp64("idx");
                cc.and_(idx, hdr, Imm(0x3FFFFF));
                Gp expected = cc.new_gp64("exp");
                cc.mov(expected, Imm((uint64_t)expectedIdx));
                cc.cmp(idx, expected);
                cc.b_eq(contLabel);

                // -- Miss: deopt sequence (mirrors kPrimTagCheckInt).
                cc.bind(missLabel);

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

                Gp ipReg = cc.new_gp64("ip");
                if (bytecodeBase) {
                    uintptr_t addr = reinterpret_cast<uintptr_t>(bytecodeBase)
                                   + bcOffset;
                    cc.mov(ipReg, Imm((uint64_t)addr));
                } else {
                    cc.mov(ipReg, Imm(bcOffset));
                }
                cc.str(ipReg, ptr(state, OFF_IP));

                Gp zero64 = cc.new_gp64("zero");
                cc.mov(zero64, Imm(0));
                cc.str(zero64, ptr(state, OFF_ICDATAPTR));

                Gp exit = cc.new_gp32("exit");
                cc.mov(exit, Imm(EXIT_SEND));
                cc.str(exit, ptr(state, OFF_EXIT));
                cc.ret();

                // -- Hit: receiver passthrough.
                cc.bind(contLabel);
                regFor[v.id] = itRcv->second;
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
            case Op::kSendCallHelper: {
                // B-1: invoke jit_rt_sista_call_send via cc.invoke.
                // Helper does the send synchronously and returns the
                // result.  On NLR / abnormal exit, helper returns 0
                // and we deopt to the source bcOffset (interpreter
                // takes over — its NLR machinery resumes correctly).
                //
                // Operand layout: [rcvr, arg0, ..., arg_{n-1}].
                // Literal: low 32 = selIdx, mid 16 = nArgs,
                //          high 16 = bcOffset (for deopt resume).
                if (v.operands.empty()) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                uint32_t selIdx   = (uint32_t)(v.literal & 0xFFFFFFFFu);
                uint32_t nArgs    = (uint32_t)((v.literal >> 32) & 0xFFFFu);
                uint32_t bcOffset = (uint32_t)((v.literal >> 48) & 0xFFFFu);

                // Push rcvr + args onto interp stack at state->sp.
                // Helper expects them there to do the send.
                Gp sp = cc.new_gp64("send_sp");
                cc.ldr(sp, ptr(state, OFF_SP));
                for (size_t opIdx = 0; opIdx < v.operands.size(); opIdx++) {
                    auto opIt = regFor.find(v.operands[opIdx]);
                    if (opIt == regFor.end()) {
                        if (failedAtValue) *failedAtValue = v.id;
                        return nullptr;
                    }
                    cc.str(opIt->second,
                           ptr(sp, static_cast<int>(opIdx) * 8));
                }
                cc.add(sp, sp, Imm(static_cast<int>(v.operands.size()) * 8));
                cc.str(sp, ptr(state, OFF_SP));

                // Resolve selector: state->literals[selIdx].
                Gp litBase = cc.new_gp64("sendLitBase");
                Gp selReg  = cc.new_gp64("sendSel");
                cc.ldr(litBase, ptr(state, OFF_LITERALS));
                cc.ldr(selReg,
                       ptr(litBase, static_cast<int>(selIdx) * 8));

                Gp nArgsReg = cc.new_gp64("sendNArgs");
                cc.mov(nArgsReg, Imm((uint64_t)nArgs));

                // cc.invoke pattern: load helper addr to Gp, call.
                Gp fnReg = cc.new_gp64("sendHelper");
                cc.mov(fnReg,
                       Imm((uint64_t)&jit_rt_sista_call_send));
                asmjit::InvokeNode* invokeNode = nullptr;
                asmjit::Error invErr = cc.invoke(
                    asmjit::Out(invokeNode), fnReg,
                    asmjit::FuncSignature::build<
                        uint64_t, void*, uint64_t, uint64_t>());
                if (invErr != asmjit::kErrorOk || !invokeNode) {
                    std::fprintf(stderr,
                        "[SISTA-INVOKE-ERR] kSendCallHelper err=%u\n",
                        (unsigned)invErr);
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                invokeNode->set_arg(0, state);
                invokeNode->set_arg(1, selReg);
                invokeNode->set_arg(2, nArgsReg);
                Gp dst = cc.new_gp64("sendResult");
                invokeNode->set_ret(0, dst);

                // Deopt-on-zero: helper returns 0 on NLR / abnormal.
                // Push the original operands BACK onto interp stack
                // (helper consumed them) and bail at source bcOffset.
                Label noDeopt = cc.new_label();
                cc.cbnz(dst, noDeopt);
                {
                    // Restore operands to interp stack.
                    Gp sp2 = cc.new_gp64("send_sp_dz");
                    cc.ldr(sp2, ptr(state, OFF_SP));
                    for (size_t opIdx = 0;
                         opIdx < v.operands.size(); opIdx++) {
                        auto opIt = regFor.find(v.operands[opIdx]);
                        if (opIt == regFor.end()) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        cc.str(opIt->second,
                               ptr(sp2,
                                   static_cast<int>(opIdx) * 8));
                    }
                    cc.add(sp2, sp2,
                           Imm(static_cast<int>(v.operands.size()) * 8));
                    cc.str(sp2, ptr(state, OFF_SP));

                    Gp ipReg = cc.new_gp64("send_ip_dz");
                    if (bytecodeBase) {
                        uintptr_t addr =
                            reinterpret_cast<uintptr_t>(bytecodeBase)
                            + bcOffset;
                        cc.mov(ipReg, Imm((uint64_t)addr));
                    } else {
                        cc.mov(ipReg, Imm(bcOffset));
                    }
                    cc.str(ipReg, ptr(state, OFF_IP));
                    Gp argCountReg = cc.new_gp32("send_argc_dz");
                    cc.mov(argCountReg, Imm(nArgs));
                    cc.str(argCountReg, ptr(state, OFF_SENDARGCOUNT));
                    Gp zero64 = cc.new_gp64("send_zero_dz");
                    cc.mov(zero64, Imm(0));
                    cc.str(zero64, ptr(state, OFF_ICDATAPTR));
                    Gp exitR = cc.new_gp32("send_exit_dz");
                    cc.mov(exitR, Imm(EXIT_SEND));
                    cc.str(exitR, ptr(state, OFF_EXIT));
                    cc.ret();
                }
                cc.bind(noDeopt);
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
            case Op::kPrimSize: {
                // Calls jit_rt_sista_basic_size(state, rcv) via cc.invoke.
                // Returns SmI Oop on success, 0 on guard miss.  On 0
                // result, deopts to interpreter at the source bcOffset
                // (passed via v.literal).
                if (v.operands.size() != 1) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                auto itRcv = regFor.find(v.operands[0]);
                if (itRcv == regFor.end()) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                Gp fnReg = cc.new_gp64("sizeHelper");
                cc.mov(fnReg,
                       Imm((uint64_t)&jit_rt_sista_basic_size));
                asmjit::InvokeNode* invokeNode = nullptr;
                asmjit::Error invErr = cc.invoke(
                    asmjit::Out(invokeNode), fnReg,
                    asmjit::FuncSignature::build<
                        uint64_t, void*, uint64_t>());
                if (invErr != asmjit::kErrorOk || !invokeNode) {
                    std::fprintf(stderr,
                        "[SISTA-INVOKE-ERR] kPrimSize err=%u\n",
                        (unsigned)invErr);
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                invokeNode->set_arg(0, state);
                invokeNode->set_arg(1, itRcv->second);
                Gp dst = cc.new_gp64("size");
                invokeNode->set_ret(0, dst);

                // Deopt-on-zero check.  v.literal carries bcOffset to
                // resume at.  If size == 0, push the receiver back
                // onto interp stack (since the original send expected
                // it on top), set state.ip to the source send, set
                // state.exitReason = ExitSend, and ret.
                Label noDeopt = cc.new_label();
                cc.cbnz(dst, noDeopt);  // nonzero → skip deopt
                {
                    // Push receiver onto interp stack at state.sp[0],
                    // bump state.sp by 8.
                    Gp sp = cc.new_gp64("sp_dz");
                    cc.ldr(sp, ptr(state, OFF_SP));
                    cc.str(itRcv->second, ptr(sp));
                    cc.add(sp, sp, Imm(8));
                    cc.str(sp, ptr(state, OFF_SP));
                    // state.ip = bcOffset (or absolute address if base).
                    Gp ipReg = cc.new_gp64("ip_dz");
                    uint64_t bcOff = v.literal;
                    if (bytecodeBase) {
                        uintptr_t addr =
                            reinterpret_cast<uintptr_t>(bytecodeBase)
                            + bcOff;
                        cc.mov(ipReg, Imm((uint64_t)addr));
                    } else {
                        cc.mov(ipReg, Imm(bcOff));
                    }
                    cc.str(ipReg, ptr(state, OFF_IP));
                    Gp argCount = cc.new_gp32("argc_dz");
                    cc.mov(argCount, Imm(0));
                    cc.str(argCount, ptr(state, OFF_SENDARGCOUNT));
                    Gp zero64 = cc.new_gp64("zero_dz");
                    cc.mov(zero64, Imm(0));
                    cc.str(zero64, ptr(state, OFF_ICDATAPTR));
                    Gp exitR = cc.new_gp32("exit_dz");
                    cc.mov(exitR, Imm(EXIT_SEND));
                    cc.str(exitR, ptr(state, OFF_EXIT));
                    cc.ret();
                }
                cc.bind(noDeopt);
                regFor[v.id] = dst;
                break;
            }
            case Op::kPrimAt: {
                // Calls jit_rt_sista_basic_at(state, rcv, idx) via
                // cc.invoke.  Returns element Oop, or 0 on guard miss
                // / OOB / non-SmI index — deopts on 0 result.
                // v.literal carries bcOffset for deopt resume.
                if (v.operands.size() != 2) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                auto itRcv = regFor.find(v.operands[0]);
                auto itIdx = regFor.find(v.operands[1]);
                if (itRcv == regFor.end() || itIdx == regFor.end()) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                Gp fnReg = cc.new_gp64("atHelper");
                cc.mov(fnReg,
                       Imm((uint64_t)&jit_rt_sista_basic_at));
                asmjit::InvokeNode* invokeNode = nullptr;
                asmjit::Error invErr = cc.invoke(
                    asmjit::Out(invokeNode), fnReg,
                    asmjit::FuncSignature::build<
                        uint64_t, void*, uint64_t, uint64_t>());
                if (invErr != asmjit::kErrorOk || !invokeNode) {
                    std::fprintf(stderr,
                        "[SISTA-INVOKE-ERR] kPrimAt err=%u\n",
                        (unsigned)invErr);
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                invokeNode->set_arg(0, state);
                invokeNode->set_arg(1, itRcv->second);
                invokeNode->set_arg(2, itIdx->second);
                Gp dst = cc.new_gp64("at");
                invokeNode->set_ret(0, dst);

                // Deopt-on-zero: helper returns 0 on miss.  Push
                // receiver + index back onto interp stack (the at:
                // send expects them), bail at source bcOffset.
                Label noDeopt = cc.new_label();
                cc.cbnz(dst, noDeopt);
                {
                    Gp sp = cc.new_gp64("sp_az");
                    cc.ldr(sp, ptr(state, OFF_SP));
                    cc.str(itRcv->second, ptr(sp));
                    cc.str(itIdx->second, ptr(sp, 8));
                    cc.add(sp, sp, Imm(16));
                    cc.str(sp, ptr(state, OFF_SP));
                    Gp ipReg = cc.new_gp64("ip_az");
                    uint64_t bcOff = v.literal;
                    if (bytecodeBase) {
                        uintptr_t addr =
                            reinterpret_cast<uintptr_t>(bytecodeBase)
                            + bcOff;
                        cc.mov(ipReg, Imm((uint64_t)addr));
                    } else {
                        cc.mov(ipReg, Imm(bcOff));
                    }
                    cc.str(ipReg, ptr(state, OFF_IP));
                    Gp argCount = cc.new_gp32("argc_az");
                    cc.mov(argCount, Imm(1));  // at: takes 1 arg
                    cc.str(argCount, ptr(state, OFF_SENDARGCOUNT));
                    Gp zero64 = cc.new_gp64("zero_az");
                    cc.mov(zero64, Imm(0));
                    cc.str(zero64, ptr(state, OFF_ICDATAPTR));
                    Gp exitR = cc.new_gp32("exit_az");
                    cc.mov(exitR, Imm(EXIT_SEND));
                    cc.str(exitR, ptr(state, OFF_EXIT));
                    cc.ret();
                }
                cc.bind(noDeopt);
                regFor[v.id] = dst;
                break;
            }
            case Op::kBlockCreate: {
                // Default path: call jit_rt_sista_block_create via
                // asmjit cc.invoke.  Compiled execution continues
                // past PushFullBlock — block oop is in regFor[v.id]
                // for subsequent IR.  Verified end-to-end (commit
                // c1b813ea trace counter).
                //
                // PHARO_SISTA_BLOCK_BAIL=1 — bisect: revert to the
                // pre-Path-3 bail-only path (interpreter handles
                // PushFullBlock).  Same runtime effect as
                // kSendUnspeculated.  Kept for diagnostics.
                //
                // Operand layout (set by SistaBuilder PushFullBlock arm):
                //   operands[0..N-1] = full IR-stack snapshot at bail
                // Literal layout:
                //   bits 0-15  = litIndex
                //   bits 16-23 = flags  (bit7=recvOnStack, bit6=ignoreOuter)
                //   bits 24-31 = stackSize at bail
                //   bits 32+   = bcOffset of the PushFullBlock bytecode.
                static bool noSends =
                    getenv("PHARO_SISTA_NO_LOWER_SENDS") != nullptr;
                if (noSends) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                static bool useBail =
                    getenv("PHARO_SISTA_BLOCK_BAIL") != nullptr;
                bool useHelper = !useBail;
                uint32_t litIndex = (uint32_t)(v.literal & 0xFFFFu);
                uint32_t flags = (uint32_t)((v.literal >> 16) & 0xFFu);
                uint64_t bcOffset = v.literal >> 32;

                // Push consumed operands to interp stack.
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
                cc.add(sp, sp,
                       Imm(static_cast<int>(v.operands.size()) * 8));
                cc.str(sp, ptr(state, OFF_SP));

                if (useHelper) {
                    // Path 1 — helper invoke.
                    Gp argLit = cc.new_gp64("blockLitIdx");
                    cc.mov(argLit, Imm((uint64_t)litIndex));
                    Gp argFlags = cc.new_gp64("blockFlags");
                    cc.mov(argFlags, Imm((uint64_t)flags));
                    Gp argNumCopied = cc.new_gp64("blockNumCopied");
                    cc.mov(argNumCopied,
                           Imm((uint64_t)(flags & 0x3Fu)));

                    // ARM64 `blr` (branch-with-link, register form) needs
                    // the target in a register — passing an Imm makes
                    // asmjit silently emit nothing for the call.  Load
                    // the helper address into a Gp first.
                    Gp fnReg = cc.new_gp64("blockHelper");
                    cc.mov(fnReg,
                           Imm((uint64_t)&jit_rt_sista_block_create));
                    asmjit::InvokeNode* invokeNode = nullptr;
                    asmjit::Error invErr = cc.invoke(
                        asmjit::Out(invokeNode), fnReg,
                        asmjit::FuncSignature::build<
                            uint64_t, void*, uint64_t,
                            uint64_t, uint64_t>());
                    if (invErr != asmjit::kErrorOk || !invokeNode) {
                        std::fprintf(stderr,
                            "[SISTA-INVOKE-ERR] cc.invoke err=%u "
                            "node=%p\n", (unsigned)invErr,
                            (void*)invokeNode);
                        if (failedAtValue) *failedAtValue = v.id;
                        return nullptr;
                    }
                    invokeNode->set_arg(0, state);
                    invokeNode->set_arg(1, argLit);
                    invokeNode->set_arg(2, argNumCopied);
                    invokeNode->set_arg(3, argFlags);
                    Gp dst = cc.new_gp64("block");
                    invokeNode->set_ret(0, dst);
                    regFor[v.id] = dst;
                    break;
                }

                // Path 2 — bail-to-interpreter (default).
                Gp ipReg = cc.new_gp64("ip");
                if (bytecodeBase) {
                    uintptr_t addr =
                        reinterpret_cast<uintptr_t>(bytecodeBase)
                        + bcOffset;
                    cc.mov(ipReg, Imm((uint64_t)addr));
                } else {
                    cc.mov(ipReg, Imm(bcOffset));
                }
                cc.str(ipReg, ptr(state, OFF_IP));
                Gp argCountReg = cc.new_gp32("argc");
                cc.mov(argCountReg, Imm(0));
                cc.str(argCountReg, ptr(state, OFF_SENDARGCOUNT));
                Gp zero64 = cc.new_gp64("zero64");
                cc.mov(zero64, Imm(0));
                cc.str(zero64, ptr(state, OFF_ICDATAPTR));
                Gp exit = cc.new_gp32("exit");
                cc.mov(exit, Imm(EXIT_SEND));
                cc.str(exit, ptr(state, OFF_EXIT));
                cc.ret();
                break;
            }
            case Op::kCountedLoopDo: {
                // B2 splice: counted at: loop with inlined block body.
                //
                // Emits:
                //   sizeReg = basicSize(rcv)              ; deopt on 0
                //   iReg = SmI(1)
                //   loopHead:
                //     if iReg > sizeReg, goto loopExit
                //     eachReg = basicAt(rcv, iReg)        ; deopt on 0
                //     <inline block body — minimal whitelist: kLoadTemp,
                //      kLoadReceiver, kConstantOop, kReturn.  Effects
                //      on outer state are NOT yet supported (kStoreTemp
                //      to closure-captured slot is the bench-relevant
                //      case but requires escape analysis to map block
                //      slot N back to outer's slot M)>
                //     iReg = iReg + 8                     ; SmI add
                //     b loopHead
                //   loopExit:
                //     result = rcv                         ; do: returns receiver
                using namespace asmjit::a64;
                if (v.operands.size() != 1) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                auto itRcv = regFor.find(v.operands[0]);
                if (itRcv == regFor.end()) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }

                uint32_t blockSlot = (uint32_t)v.literal;
                if (blockSlot >= method.inlinedBlocks.size()
                    || !method.inlinedBlocks[blockSlot]) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                const Method& blockIR = *method.inlinedBlocks[blockSlot];

                // Verify block has exactly one basic block.  Multi-block
                // bodies need a separate lowering pass with its own
                // block-label map.
                if (blockIR.blocks.size() != 1) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }

                // Verify every block op is in our minimal whitelist.
                // Anything else (stores, sends, primops) needs more
                // lowering machinery; refuse for now.
                for (const auto& bv : blockIR.values) {
                    switch (bv.op) {
                    case Op::kLoadTemp:
                    case Op::kLoadReceiver:
                    case Op::kLoadTrueOop:
                    case Op::kLoadFalseOop:
                    case Op::kConstantOop:
                    case Op::kLoadLiteral:
                    case Op::kReturn:
                        break;
                    default:
                        if (failedAtValue) *failedAtValue = v.id;
                        return nullptr;
                    }
                }

                Gp rcvReg = itRcv->second;

                // Source bcOffset for deopt resume — recorded as the
                // framepoint when the lifter emitted kCountedLoopDo.
                uint32_t deoptBC = 0;
                for (const auto& fp : method.framepoints) {
                    if (fp.valueId == v.id) {
                        deoptBC = fp.bcOffset;
                        break;
                    }
                }

                auto emitDeopt = [&](Gp savedRcv, uint32_t bc) {
                    Gp sp = cc.new_gp64("loop_dz_sp");
                    cc.ldr(sp, ptr(state, OFF_SP));
                    cc.str(savedRcv, ptr(sp));
                    cc.add(sp, sp, Imm(8));
                    cc.str(sp, ptr(state, OFF_SP));
                    Gp ipReg = cc.new_gp64("loop_dz_ip");
                    if (bytecodeBase) {
                        uintptr_t addr =
                            reinterpret_cast<uintptr_t>(bytecodeBase) + bc;
                        cc.mov(ipReg, Imm((uint64_t)addr));
                    } else {
                        cc.mov(ipReg, Imm((uint64_t)bc));
                    }
                    cc.str(ipReg, ptr(state, OFF_IP));
                    Gp argc = cc.new_gp32("loop_dz_argc");
                    cc.mov(argc, Imm(0));
                    cc.str(argc, ptr(state, OFF_SENDARGCOUNT));
                    Gp z64 = cc.new_gp64("loop_dz_zero");
                    cc.mov(z64, Imm(0));
                    cc.str(z64, ptr(state, OFF_ICDATAPTR));
                    Gp exitR = cc.new_gp32("loop_dz_exit");
                    cc.mov(exitR, Imm(EXIT_SEND));
                    cc.str(exitR, ptr(state, OFF_EXIT));
                    cc.ret();
                };

                // Step 1: sizeReg = basicSize(rcv).  Deopt on 0 (non-
                // indexable receiver — bails to interp at the do:
                // bytecode, where the original send re-runs).
                Gp sizeFn = cc.new_gp64("loop_szfn");
                cc.mov(sizeFn,
                       Imm((uint64_t)&jit_rt_sista_basic_size));
                asmjit::InvokeNode* sizeInvoke = nullptr;
                asmjit::Error sErr = cc.invoke(
                    asmjit::Out(sizeInvoke), sizeFn,
                    asmjit::FuncSignature::build<
                        uint64_t, void*, uint64_t>());
                if (sErr != asmjit::kErrorOk || !sizeInvoke) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                sizeInvoke->set_arg(0, state);
                sizeInvoke->set_arg(1, rcvReg);
                Gp sizeReg = cc.new_gp64("loop_size");
                sizeInvoke->set_ret(0, sizeReg);

                Label sizeOk = cc.new_label();
                cc.cbnz(sizeReg, sizeOk);
                emitDeopt(rcvReg, deoptBC);
                cc.bind(sizeOk);

                // Step 2: iReg = SmI(1) = (1 << 3) | 1 = 9.
                Gp iReg = cc.new_gp64("loop_i");
                cc.mov(iReg, Imm(9));

                // Step 3: loop head.
                Label loopHead = cc.new_label();
                Label loopExit = cc.new_label();
                cc.bind(loopHead);

                // Step 4: compare iReg, sizeReg as SmI Oops.  Both have
                // tag 1 in low 3 bits; comparing raw bits as unsigned
                // is equivalent to comparing the underlying integers
                // (same tag, same shift).  Exit when iReg > sizeReg.
                cc.cmp(iReg, sizeReg);
                cc.b_hi(loopExit);

                // Step 5: eachReg = basicAt(rcv, iReg).  Deopt on 0.
                Gp atFn = cc.new_gp64("loop_atfn");
                cc.mov(atFn,
                       Imm((uint64_t)&jit_rt_sista_basic_at));
                asmjit::InvokeNode* atInvoke = nullptr;
                asmjit::Error aErr = cc.invoke(
                    asmjit::Out(atInvoke), atFn,
                    asmjit::FuncSignature::build<
                        uint64_t, void*, uint64_t, uint64_t>());
                if (aErr != asmjit::kErrorOk || !atInvoke) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                atInvoke->set_arg(0, state);
                atInvoke->set_arg(1, rcvReg);
                atInvoke->set_arg(2, iReg);
                Gp eachReg = cc.new_gp64("loop_each");
                atInvoke->set_ret(0, eachReg);

                Label atOk = cc.new_label();
                cc.cbnz(eachReg, atOk);
                emitDeopt(rcvReg, deoptBC);
                cc.bind(atOk);

                // Step 6: inline block body.  For our minimal whitelist
                // (loads + return + constants), there are no
                // side-effecting ops to emit — every value is a no-op
                // at the asm level (we just need to "compute" them but
                // they don't escape the iteration).  kReturn ends the
                // body; we don't actually return — we just continue
                // the loop.
                //
                // Future: handle kPrimAddInt etc. by emitting tag-
                // preserving SmI math.  Handle kStoreTemp(N) for N >=
                // numCopied via remote-temp store to closure context.

                // Step 7: iReg += 8 (tag-preserving SmI add: SmI(a) +
                // SmI(b) = (a<<3 | 1) + (b<<3 | 1) - 1.  For b=1:
                // result = iReg + 9 - 1 = iReg + 8.).
                cc.add(iReg, iReg, Imm(8));

                // Step 8: branch back to loopHead.
                cc.b(loopHead);

                // Step 9: loop exit.
                cc.bind(loopExit);

                // Step 10: kCountedLoopDo's result is the receiver.
                regFor[v.id] = rcvReg;
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
