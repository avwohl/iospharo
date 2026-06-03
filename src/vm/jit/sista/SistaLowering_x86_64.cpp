/*
 * SistaLowering_x86_64.cpp - IR -> asmjit x86_64
 *
 * Per-arch sibling of SistaLowering_arm64.cpp.  Sista is the tier-2
 * JIT path: takes a SistaMethod IR (built by SistaBuilder) and emits
 * native machine code via asmjit.
 *
 * Status (op-by-op port in progress).  Supported IR ops:
 *   Loads/stores:   kLoadReceiver, kLoadTrueOop, kLoadFalseOop,
 *                   kLoadTemp, kStoreTemp, kLoadLiteral, kLoadInstVar,
 *                   kStoreInstVar (helper-emit branch only),
 *                   kConstantOop
 *   Arith (with overflow deopt):
 *                   kPrimAddInt, kPrimSubInt, kPrimMulInt
 *                   (uses x86 OF flag + jo for the deopt branch)
 *   Compare:        kPrimLtInt, kPrimLeInt, kPrimGtInt, kPrimGeInt,
 *                   kPrimEqInt, kPrimNeqInt
 *                   kPrimIdentityEq, kPrimIdentityNeq
 *                   (cmp + cmovcc against trueOop/falseOop, with
 *                    fused cmp→branch optimisation)
 *   Tag check:      kPrimTagCheckInt
 *   Control:        kBranch, kBranchIfTrue, kBranchIfFalse
 *   Phi:            kPhi (pre-alloc; fillPhis at predecessor terminators)
 *   Block stack:    kLoadStackSlot
 *   Return:         kReturn
 *
 * Any op without an emit branch above bails via *failedAtValue —
 * the containing method falls back to tier-1.  Add the next op when
 * profiling shows it as a Sista bail bottleneck.
 *
 * Calling convention matches Tier 1 / Tier 2 arm64:
 *   void fn(JITState* state)   — state in rdi (SysV) / rcx (Win64)
 *   exit via state->returnValue + state->exitReason = EXIT_RETURN
 */
#include "SistaLowering.hpp"
#include "../../DebugSettings.hpp"

#if PHARO_JIT_ENABLED

#include <asmjit/x86.h>
#include <asmjit/core/jitruntime.h>

#include <atomic>
#include <cstdio>
#include <unordered_map>
#include <vector>

// Sista runtime helpers.  Defined in src/vm/jit/JITRuntime.cpp.
extern "C" uint64_t jit_rt_store_inst_var(void* state,
                                            uint64_t recvBits,
                                            uint64_t ivarIdx,
                                            uint64_t valBits);
extern "C" uint64_t jit_rt_sista_call_send(void* state,
                                             uint64_t selBits,
                                             uint64_t nArgs);
extern "C" uint64_t jit_rt_sista_special_call_send(void* state,
                                                    uint64_t ssIdx,
                                                    uint64_t nArgs);
extern "C" uint64_t jit_rt_sista_alloc_array(void* state,
                                               uint64_t size);

namespace pharo {
namespace sista {

namespace {

// Diagnostic: how many methods Sista-compiled vs bailed during this
// run.  Printed every 64 calls so we see it without a clean dtor.
struct LowerStats {
    std::atomic<size_t> ok{0};
    std::atomic<size_t> bail{0};
    // Per-op bail histogram (indexed by Op enum) — shows which unported ops
    // dominate the bails, so the port targets the real bottleneck.
    std::atomic<size_t> bailByOp[256]{};
    void bailOp(uint8_t op) {
        bailByOp[op].fetch_add(1, std::memory_order_relaxed);
    }
    void tick() {
        size_t total = ok.load() + bail.load();
        if ((total & 63) == 0) {
            fprintf(stderr,
                    "[SISTA-x86] lower OK=%zu bail=%zu (total=%zu)\n",
                    ok.load(), bail.load(), total);
        }
        // Coarser cadence: dump the bail histogram so the last line printed
        // approximates the final tally (no clean dtor to hook).
        if ((total & 255) == 0 && total > 0) {
            fprintf(stderr, "[SISTA-BAILHISTO]");
            for (int i = 0; i < 256; i++) {
                size_t n = bailByOp[i].load();
                if (n) fprintf(stderr, " %s=%zu", name(static_cast<Op>(i)), n);
            }
            fprintf(stderr, "\n");
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
constexpr int OFF_ICDATAPTR    = 96;
constexpr int OFF_SENDARGCOUNT = 104;
constexpr int OFF_TRUEOOP      = 128;
constexpr int OFF_FALSEOOP     = 136;

// ExitReason values (src/vm/jit/JITState.hpp).
constexpr int EXIT_RETURN  = 1;
constexpr int EXIT_SEND    = 2;

// Map an integer-compare IR op to an x86 condition code.  Used by
// both kPrimLtInt-and-friends emit (cmovcc / setcc) and the
// fused-cmp-into-branch optimisation (jcc).
asmjit::x86::CondCode condOfIntCmp(Op op, bool* ok) {
    using namespace asmjit::x86;
    *ok = true;
    switch (op) {
      case Op::kPrimLtInt:  return CondCode::kL;
      case Op::kPrimLeInt:  return CondCode::kLE;
      case Op::kPrimGtInt:  return CondCode::kG;
      case Op::kPrimGeInt:  return CondCode::kGE;
      case Op::kPrimEqInt:  return CondCode::kE;
      case Op::kPrimNeqInt: return CondCode::kNE;
      default: *ok = false; return CondCode::kE;
    }
}

asmjit::x86::CondCode negateCond(asmjit::x86::CondCode c) {
    using namespace asmjit::x86;
    switch (c) {
      case CondCode::kL:  return CondCode::kGE;
      case CondCode::kLE: return CondCode::kG;
      case CondCode::kG:  return CondCode::kLE;
      case CondCode::kGE: return CondCode::kL;
      case CondCode::kE:  return CondCode::kNE;
      case CondCode::kNE: return CondCode::kE;
      default:            return c;
    }
}

}  // namespace

Lowering::Lowering() {
    runtime_ = new asmjit::JitRuntime();
}

Lowering::~Lowering() {
    delete runtime_;
}

Lowering::CompiledFn Lowering::lower(const Method& method,
                                       uint32_t* failedAtValue,
                                       const uint8_t* bytecodeBase,
                                       uint32_t startBcOffset) {
    (void)startBcOffset;  // jit-may22b Step 1: not yet supported on x86_64
    using namespace asmjit;
    using namespace asmjit::x86;

    if (pharo::g_debug.sistaNoLower) {
        if (failedAtValue) *failedAtValue = 0;
        return nullptr;
    }
    const bool noArith     = pharo::g_debug.sistaNoLowerArith;
    const bool noArithMath = pharo::g_debug.sistaNoLowerArithMath;
    const bool noArithCmp  = pharo::g_debug.sistaNoLowerArithCmp;
    const bool noBranch    = pharo::g_debug.sistaNoLowerBranch;
    const bool noFuse      = pharo::g_debug.sistaNoLowerFuse;

    CodeHolder code;
    code.init(runtime_->environment(), runtime_->cpu_features());

    // PHARO_SISTA_ASMJIT_LOG=1: dump every emitted method's asmjit IR
    // and final machine code to stderr.  Useful to inspect codegen
    // when chasing miscompiles.
    static asmjit::FileLogger* asmjitLogger = []() -> asmjit::FileLogger* {
        if (pharo::g_debug.sistaAsmjitLog)
            return new asmjit::FileLogger(stderr);
        return nullptr;
    }();
    if (asmjitLogger) code.set_logger(asmjitLogger);

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

    // SSA value id -> the virtual register holding its value.
    std::unordered_map<uint32_t, Gp> regFor;

    // Pre-create labels for every block.
    std::vector<Label> blockLabels;
    blockLabels.reserve(method.blocks.size());
    for (size_t i = 0; i < method.blocks.size(); i++) {
        blockLabels.push_back(cc.new_label());
    }

    // Pre-allocate phi regs so predecessors can reference them.
    for (const Block& b : method.blocks) {
        for (uint32_t vid : b.values) {
            const Value& v = method.valueAt(vid);
            if (v.op != Op::kPhi) break;  // phis are always block-leading
            regFor[v.id] = cc.new_gp64("phi");
        }
    }

    auto bail = [&](uint32_t vid) -> CompiledFn {
        if (failedAtValue) *failedAtValue = vid;
        g_lowerStats.bail.fetch_add(1, std::memory_order_relaxed);
        g_lowerStats.bailOp(static_cast<uint8_t>(method.valueAt(vid).op));
        g_lowerStats.tick();
        return nullptr;
    };

    // Copy this block's outgoingStack into successor's phi regs.
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

    // Emit a deopt sequence: spill v.operands[stackBase..] onto the
    // runtime stack, set state.ip to bcOffset, set exitReason = EXIT_SEND,
    // ret.  Used by overflow / tag-check deopt branches.
    //
    // stackBase varies by op: kPrim{Add,Sub,Mul}Int uses 2 (operand 0/1
    // are a/b, 2..N are the simulated stack); kPrimTagCheckInt and
    // kGuardClass use 1 (operand 0 is the value/receiver, 1..N are the
    // stack).  Wrong stackBase mis-spills the deopt frame.
    auto emitDeopt = [&](const Value& v, uint32_t bcOffset,
                         size_t stackBase) -> bool {
        Gp sp = cc.new_gp64("dpsp");
        cc.mov(sp, ptr(state, OFF_SP));
        for (size_t opIdx = stackBase; opIdx < v.operands.size(); opIdx++) {
            auto opIt = regFor.find(v.operands[opIdx]);
            if (opIt == regFor.end()) return false;
            cc.mov(ptr(sp, static_cast<int>(opIdx - stackBase) * 8),
                   opIt->second);
        }
        size_t numStackOps = v.operands.size() - stackBase;
        if (numStackOps > 0) {
            cc.add(sp, Imm(static_cast<int>(numStackOps) * 8));
        }
        cc.mov(ptr(state, OFF_SP), sp);
        Gp ipReg = cc.new_gp64("dpip");
        if (bytecodeBase) {
            uintptr_t addr = reinterpret_cast<uintptr_t>(bytecodeBase) + bcOffset;
            cc.mov(ipReg, Imm((uint64_t)addr));
        } else {
            cc.mov(ipReg, Imm(bcOffset));
        }
        cc.mov(ptr(state, OFF_IP), ipReg);
        Gp zero64 = cc.new_gp64("dpzero");
        cc.xor_(zero64, zero64);
        cc.mov(ptr(state, OFF_ICDATAPTR), zero64);
        Gp exit = cc.new_gp32("dpexit");
        cc.mov(exit, Imm(EXIT_SEND));
        cc.mov(ptr(state, OFF_EXIT), exit);
        cc.ret();
        return true;
    };

    // Walk blocks in ID order (builder emits them in source-bytecode order).
    for (const Block& b : method.blocks) {
        cc.bind(blockLabels[b.id]);

        // Compare-into-branch fusion: if block ends with kPrimXxxInt
        // followed by kBranchIf{True,False} on its result, skip the
        // cmovcc materialisation and emit cmp+jcc directly.
        uint32_t fusedCmpId = UINT32_MAX;
        CondCode fusedCond = CondCode::kE;
        if (b.values.size() >= 2) {
            uint32_t lastVid = b.values[b.values.size() - 1];
            uint32_t prevVid = b.values[b.values.size() - 2];
            const Value& lastV = method.valueAt(lastVid);
            const Value& prevV = method.valueAt(prevVid);
            if ((lastV.op == Op::kBranchIfTrue
              || lastV.op == Op::kBranchIfFalse)
                && lastV.operands.size() == 1
                && lastV.operands[0] == prevVid) {
                bool isCmp = false;
                CondCode trueCond = condOfIntCmp(prevV.op, &isCmp);
                if (noFuse) isCmp = false;
                if (isCmp) {
                    fusedCmpId = prevVid;
                    fusedCond = (lastV.op == Op::kBranchIfTrue)
                        ? trueCond
                        : negateCond(trueCond);
                }
            }
        }

        // Per-block CSE for temp loads.
        std::unordered_map<uint64_t, Gp> tempCache;

        for (uint32_t vid : b.values) {
            const Value& v = method.valueAt(vid);
            switch (v.op) {

            case Op::kPhi:
                // Pre-allocated; predecessors fill in via fillPhis.
                break;

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

            case Op::kLoadStackSlot: {
                // Load sstate.sp[-(slot+1)] — multi-entry-point
                // loaders use this to materialize phi inputs from
                // the runtime stack.
                uint32_t slot = (uint32_t)v.literal;
                Gp sp = cc.new_gp64("lsslot_sp");
                cc.mov(sp, ptr(state, OFF_SP));
                Gp dst = cc.new_gp64("lsslot");
                cc.mov(dst, ptr(sp, -(int)(slot + 1) * 8));
                regFor[v.id] = dst;
                break;
            }

            case Op::kLoadLiteral: {
                Gp dst = cc.new_gp64("lit");
                cc.mov(dst, ptr(litBaseHoisted,
                                static_cast<int>(v.literal) * 8));
                regFor[v.id] = dst;
                break;
            }

            case Op::kLoadInstVar: {
                if (v.operands.size() != 1) return bail(v.id);
                auto it = regFor.find(v.operands[0]);
                if (it == regFor.end()) return bail(v.id);
                Gp dst = cc.new_gp64("ivar");
                // Slots live at [obj + 8 + N*8]; +8 because
                // sizeof(ObjectHeader) == 8.
                cc.mov(dst, ptr(it->second,
                                8 + static_cast<int>(v.literal) * 8));
                regFor[v.id] = dst;
                break;
            }

            case Op::kStoreInstVar: {
                // Mirrors the arm64 selective lowering: bit 63 of the
                // literal must be set (setter-inline emission); plain
                // bytecode-driven stores still bail.
                if (v.operands.size() != 2) return bail(v.id);
                bool useHelper = (v.literal >> 63) & 1;
                if (!useHelper) return bail(v.id);
                auto recvIt = regFor.find(v.operands[0]);
                auto valIt  = regFor.find(v.operands[1]);
                if (recvIt == regFor.end() || valIt == regFor.end())
                    return bail(v.id);
                uint64_t ivarIdx = v.literal & 0x7FFFFFFFFFFFFFFFULL;
                Gp ivarReg = cc.new_gp64("siv_ivar");
                cc.mov(ivarReg, Imm(ivarIdx));
                Gp fnReg = cc.new_gp64("siv_helper");
                cc.mov(fnReg, Imm((uint64_t)&jit_rt_store_inst_var));
                InvokeNode* invokeNode = nullptr;
                Error invErr = cc.invoke(
                    Out(invokeNode), fnReg,
                    FuncSignature::build<
                        uint64_t, void*, uint64_t, uint64_t, uint64_t>());
                if (invErr != kErrorOk || !invokeNode)
                    return bail(v.id);
                invokeNode->set_arg(0, state);
                invokeNode->set_arg(1, recvIt->second);
                invokeNode->set_arg(2, ivarReg);
                invokeNode->set_arg(3, valIt->second);
                Gp dummyRet = cc.new_gp64("siv_ret");
                invokeNode->set_ret(0, dummyRet);
                break;
            }

            case Op::kConstantOop: {
                Gp dst = cc.new_gp64("const");
                cc.mov(dst, Imm(v.literal));
                regFor[v.id] = dst;
                break;
            }

            // ---- Integer compares (kPrim<cmp>Int) ----
            case Op::kPrimLtInt:
            case Op::kPrimLeInt:
            case Op::kPrimGtInt:
            case Op::kPrimGeInt:
            case Op::kPrimEqInt:
            case Op::kPrimNeqInt: {
                if (noArith || noArithCmp) return bail(v.id);
                if (v.operands.size() != 2) return bail(v.id);
                auto ita = regFor.find(v.operands[0]);
                auto itb = regFor.find(v.operands[1]);
                if (ita == regFor.end() || itb == regFor.end())
                    return bail(v.id);
                cc.cmp(ita->second, itb->second);
                if (v.id == fusedCmpId) {
                    // Result never read — the branch will use the flags.
                    regFor[v.id] = ita->second;
                    break;
                }
                bool ok = false;
                CondCode cond = condOfIntCmp(v.op, &ok);
                if (!ok) return bail(v.id);
                // Use branch-based select rather than cmov — asmjit's
                // RA can insert reg-allocation moves between cmp and a
                // following cmov (or reuse phys regs in a way that makes
                // the cmov read the wrong operand).  jcc + mov is more
                // resilient and only marginally larger code.
                Gp dst = cc.new_gp64("intcmp");
                cc.mov(dst, ptr(state, OFF_FALSEOOP));
                Label skip = cc.new_label();
                cc.j(negateCond(cond), skip);
                cc.mov(dst, ptr(state, OFF_TRUEOOP));
                cc.bind(skip);
                regFor[v.id] = dst;
                break;
            }

            case Op::kPrimIdentityEq:
            case Op::kPrimIdentityNeq: {
                if (v.operands.size() != 2) return bail(v.id);
                auto ita = regFor.find(v.operands[0]);
                auto itb = regFor.find(v.operands[1]);
                if (ita == regFor.end() || itb == regFor.end())
                    return bail(v.id);
                Gp dst = cc.new_gp64("idcmp");
                cc.cmp(ita->second, itb->second);
                CondCode neg = (v.op == Op::kPrimIdentityEq)
                    ? CondCode::kNE : CondCode::kE;
                cc.mov(dst, ptr(state, OFF_FALSEOOP));
                Label skip = cc.new_label();
                cc.j(neg, skip);
                cc.mov(dst, ptr(state, OFF_TRUEOOP));
                cc.bind(skip);
                regFor[v.id] = dst;
                break;
            }

            // ---- Tag check (deopts to bcOffset on miss) ----
            case Op::kPrimTagCheckInt: {
                if (v.operands.empty()) return bail(v.id);
                auto it = regFor.find(v.operands[0]);
                if (it == regFor.end()) return bail(v.id);
                // Type narrowing: if the operand is already known-SmI
                // (kConstantOop SmI, arith result, or a kLoadTemp narrowed
                // by the post-build pass), the runtime check is dead code.
                // Mirrors the arm64 fast-path.
                if (method.values[v.operands[0]].type
                    == Type::kOopSmallInt) {
                    regFor[v.id] = it->second;
                    break;
                }
                uint32_t bcOffset = static_cast<uint32_t>(v.literal);
                Gp tag = cc.new_gp64("tagchk");
                cc.mov(tag, it->second);
                cc.and_(tag, Imm(7));
                cc.cmp(tag, Imm(1));
                Label cont = cc.new_label();
                cc.je(cont);
                // TagCheck deopt-stack starts at operand[1] (operand[0] is
                // the value being checked, not part of the stack).
                if (!emitDeopt(v, bcOffset, /*stackBase=*/1))
                    return bail(v.id);
                cc.bind(cont);
                regFor[v.id] = it->second;
                break;
            }

            // ---- Inline arithmetic on tagged SmallInts ----
            case Op::kPrimAddInt:
            case Op::kPrimSubInt:
            case Op::kPrimMulInt: {
                if (noArith || noArithMath) return bail(v.id);
                if (v.operands.size() < 2) return bail(v.id);
                auto ita = regFor.find(v.operands[0]);
                auto itb = regFor.find(v.operands[1]);
                if (ita == regFor.end() || itb == regFor.end())
                    return bail(v.id);

                Gp dst = cc.new_gp64("arith");
                bool needOvCheck = (v.operands.size() > 2);
                Label miss = needOvCheck ? cc.new_label() : Label();
                Label cont = needOvCheck ? cc.new_label() : Label();

                if (v.op == Op::kPrimAddInt) {
                    // tagged a + tagged b - 1 = ((a+b)<<3) | 1.
                    // SmallInt fits in 60-bit signed (bits 60-63 must
                    // sign-extend bit 59).  After tagging, that means
                    // the encoded result's bit 62 must equal bit 63.
                    // 64-bit `jo` is too permissive (catches overflow
                    // of the tagged 64-bit value, not of the 60-bit
                    // SmallInt range), so use the bit-62/63 mismatch
                    // check.  See arm64 sibling.
                    cc.mov(dst, ita->second);
                    cc.add(dst, itb->second);
                    cc.sub(dst, Imm(1));
                } else if (v.op == Op::kPrimSubInt) {
                    cc.mov(dst, ita->second);
                    cc.sub(dst, itb->second);
                    cc.add(dst, Imm(1));
                } else {  // kPrimMulInt
                    // (a >> 3) * (b >> 3); imul sets OF on 64-bit
                    // signed overflow.  60-bit fit additionally
                    // checked via shl 4 / sar 4 round-trip.
                    Gp au = cc.new_gp64("au");
                    Gp bu = cc.new_gp64("bu");
                    cc.mov(au, ita->second);
                    cc.sar(au, Imm(3));
                    cc.mov(bu, itb->second);
                    cc.sar(bu, Imm(3));
                    Gp prod = cc.new_gp64("prod");
                    cc.mov(prod, au);
                    cc.imul(prod, bu);
                    if (needOvCheck) {
                        cc.jo(miss);
                        Gp shifted = cc.new_gp64("ovsh");
                        cc.mov(shifted, prod);
                        cc.shl(shifted, Imm(4));
                        cc.sar(shifted, Imm(4));
                        cc.cmp(shifted, prod);
                        cc.jne(miss);
                    }
                    cc.shl(prod, Imm(3));
                    cc.or_(prod, Imm(1));
                    cc.mov(dst, prod);
                }
                if (needOvCheck) {
                    if (v.op == Op::kPrimAddInt
                     || v.op == Op::kPrimSubInt) {
                        // Bit 62 != bit 63 ⇔ result outside SmallInt range.
                        //   sh = dst << 1   (bit 62 → bit 63)
                        //   xor sh, dst    (bit 63 of sh := bit62 ^ bit63)
                        //   js miss        (bit 63 set ⇔ mismatch)
                        Gp sh = cc.new_gp64("ovsh");
                        cc.mov(sh, dst);
                        cc.shl(sh, Imm(1));
                        cc.xor_(sh, dst);
                        cc.js(miss);
                    }
                    cc.jmp(cont);
                    cc.bind(miss);
                    uint32_t bcOffset = static_cast<uint32_t>(v.literal);
                    if (!emitDeopt(v, bcOffset, /*stackBase=*/2))
                        return bail(v.id);
                    cc.bind(cont);
                }
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
                break;
            }

            case Op::kBranch: {
                if (noBranch) return bail(v.id);
                if (b.successors.empty()) return bail(v.id);
                fillPhis(b, b.successors[0]);
                if (b.successors[0] != b.id + 1) {
                    cc.jmp(blockLabels[b.successors[0]]);
                }
                break;
            }

            case Op::kBranchIfTrue:
            case Op::kBranchIfFalse: {
                if (noBranch) return bail(v.id);
                if (v.operands.size() != 1 || b.successors.size() != 2)
                    return bail(v.id);
                fillPhis(b, b.successors[0]);
                fillPhis(b, b.successors[1]);
                if (v.operands[0] == fusedCmpId) {
                    // cmp's flags are still live from the kPrim<cmp>Int.
                    cc.j(fusedCond, blockLabels[b.successors[0]]);
                    if (b.successors[1] != b.id + 1) {
                        cc.jmp(blockLabels[b.successors[1]]);
                    }
                    break;
                }
                auto it = regFor.find(v.operands[0]);
                if (it == regFor.end()) return bail(v.id);
                Gp trueOop = cc.new_gp64("true");
                cc.mov(trueOop, ptr(state, OFF_TRUEOOP));
                cc.cmp(it->second, trueOop);
                if (v.op == Op::kBranchIfTrue) {
                    cc.je(blockLabels[b.successors[0]]);
                    if (b.successors[1] != b.id + 1) {
                        cc.jmp(blockLabels[b.successors[1]]);
                    }
                } else {
                    cc.je(blockLabels[b.successors[1]]);
                    if (b.successors[0] != b.id + 1) {
                        cc.jmp(blockLabels[b.successors[0]]);
                    }
                }
                break;
            }

            // ---- Sends ----
            // Common helper: emit a "bail to interpreter at bcOffset"
            // sequence that includes state.sendArgCount.  Used by
            // kSendUnspeculated terminator and kSend{,Special}CallHelper
            // deopt-on-zero branches.  spillIds = the values to push
            // onto interp.sp; spIsAlreadyShifted = whether the helper
            // pre-pushed the operands and we need to roll sp back first.
            // jit-84 B1: kSendInlineSelf routes through helper for now.
            case Op::kSendInlineSelf:
            case Op::kSendCallHelper:
            case Op::kSendCallHelperSpecial: {
                if (pharo::g_debug.sistaNoLowerSends) return bail(v.id);
                if (v.operands.empty()) return bail(v.id);

                bool isSpecial = (v.op == Op::kSendCallHelperSpecial);
                uint32_t selOrSsIdx = (uint32_t)(v.literal & 0xFFFFFFFFu);
                uint32_t nArgs      = (uint32_t)((v.literal >> 32) & 0xFFFFu);
                uint32_t bcOffset   = (uint32_t)((v.literal >> 48) & 0xFFFFu);

                // Push rcvr + args onto interp stack.  Helper expects
                // them at state.sp.
                {
                    Gp sp = cc.new_gp64("send_sp");
                    cc.mov(sp, ptr(state, OFF_SP));
                    for (size_t i = 0; i < v.operands.size(); i++) {
                        auto it = regFor.find(v.operands[i]);
                        if (it == regFor.end()) return bail(v.id);
                        cc.mov(ptr(sp, static_cast<int>(i) * 8),
                               it->second);
                    }
                    cc.add(sp,
                           Imm(static_cast<int>(v.operands.size()) * 8));
                    cc.mov(ptr(state, OFF_SP), sp);
                }

                // Resolve helper arg "selector" — for the regular helper
                // it's literals[selIdx]; for the special one it's the
                // ssIdx itself (the helper resolves the symbol).
                Gp selReg = cc.new_gp64("send_sel");
                if (isSpecial) {
                    cc.mov(selReg, Imm((uint64_t)selOrSsIdx));
                } else {
                    cc.mov(selReg,
                           ptr(litBaseHoisted,
                               static_cast<int>(selOrSsIdx) * 8));
                }
                Gp nArgsReg = cc.new_gp64("send_nargs");
                cc.mov(nArgsReg, Imm((uint64_t)nArgs));

                Gp fnReg = cc.new_gp64("send_fn");
                cc.mov(fnReg,
                       Imm(isSpecial
                           ? (uint64_t)&jit_rt_sista_special_call_send
                           : (uint64_t)&jit_rt_sista_call_send));
                InvokeNode* invokeNode = nullptr;
                Error invErr = cc.invoke(
                    Out(invokeNode), fnReg,
                    FuncSignature::build<
                        uint64_t, void*, uint64_t, uint64_t>());
                if (invErr != kErrorOk || !invokeNode) return bail(v.id);
                invokeNode->set_arg(0, state);
                invokeNode->set_arg(1, selReg);
                invokeNode->set_arg(2, nArgsReg);
                Gp dst = cc.new_gp64("send_result");
                invokeNode->set_ret(0, dst);

                // Deopt-on-zero: helper returns 0 on NLR / abnormal.
                // Replay the FULL framepoint stack to interp.sp so the
                // resumed interpreter sees every live IR value, then
                // bail at source bcOffset.
                const std::vector<uint32_t>* fpStack = nullptr;
                for (const auto& fp : method.framepoints) {
                    if (fp.valueId == v.id) {
                        fpStack = &fp.stackValueIds;
                        break;
                    }
                }
                Label noDeopt = cc.new_label();
                cc.test(dst, dst);
                cc.jnz(noDeopt);
                {
                    // Roll back the pre-helper push of v.operands
                    // (helper failure paths don't pop them) before
                    // overwriting with the framepoint replay.
                    Gp sp2 = cc.new_gp64("send_sp_dz");
                    cc.mov(sp2, ptr(state, OFF_SP));
                    cc.sub(sp2,
                           Imm(static_cast<int>(v.operands.size()) * 8));
                    const std::vector<uint32_t>& flushIds =
                        fpStack ? *fpStack : v.operands;
                    for (size_t i = 0; i < flushIds.size(); i++) {
                        auto it = regFor.find(flushIds[i]);
                        if (it == regFor.end()) return bail(v.id);
                        cc.mov(ptr(sp2, static_cast<int>(i) * 8),
                               it->second);
                    }
                    cc.add(sp2,
                           Imm(static_cast<int>(flushIds.size()) * 8));
                    cc.mov(ptr(state, OFF_SP), sp2);

                    Gp ipReg = cc.new_gp64("send_ip_dz");
                    if (bytecodeBase) {
                        uintptr_t addr =
                            reinterpret_cast<uintptr_t>(bytecodeBase)
                            + bcOffset;
                        cc.mov(ipReg, Imm((uint64_t)addr));
                    } else {
                        cc.mov(ipReg, Imm(bcOffset));
                    }
                    cc.mov(ptr(state, OFF_IP), ipReg);
                    Gp argCountReg = cc.new_gp32("send_argc_dz");
                    cc.mov(argCountReg, Imm(nArgs));
                    cc.mov(ptr(state, OFF_SENDARGCOUNT), argCountReg);
                    Gp zero64 = cc.new_gp64("send_zero_dz");
                    cc.xor_(zero64, zero64);
                    cc.mov(ptr(state, OFF_ICDATAPTR), zero64);
                    Gp exitR = cc.new_gp32("send_exit_dz");
                    cc.mov(exitR, Imm(EXIT_SEND));
                    cc.mov(ptr(state, OFF_EXIT), exitR);
                    cc.ret();
                }
                cc.bind(noDeopt);
                regFor[v.id] = dst;
                break;
            }

            case Op::kSendUnspeculated: {
                if (pharo::g_debug.sistaNoLowerSends) return bail(v.id);
                // Bail to the interpreter at the send bytecode.  Operands
                // are [rcvr, arg0, ..., arg_{nArgs-1}] for sends, or the
                // whole IR stack for generic mid-method bails.
                uint32_t nArgs    = (uint32_t)((v.literal >> 16) & 0xFFu);
                uint32_t bcOffset =
                    (uint32_t)((v.literal >> 24) & 0xFFFFFFFFu);

                Gp sp = cc.new_gp64("usp");
                cc.mov(sp, ptr(state, OFF_SP));
                for (size_t i = 0; i < v.operands.size(); i++) {
                    auto it = regFor.find(v.operands[i]);
                    if (it == regFor.end()) return bail(v.id);
                    cc.mov(ptr(sp, static_cast<int>(i) * 8), it->second);
                }
                cc.add(sp,
                       Imm(static_cast<int>(v.operands.size()) * 8));
                cc.mov(ptr(state, OFF_SP), sp);

                Gp ipReg = cc.new_gp64("uip");
                if (bytecodeBase) {
                    uintptr_t addr =
                        reinterpret_cast<uintptr_t>(bytecodeBase) + bcOffset;
                    cc.mov(ipReg, Imm((uint64_t)addr));
                } else {
                    cc.mov(ipReg, Imm(bcOffset));
                }
                cc.mov(ptr(state, OFF_IP), ipReg);
                Gp argCountReg = cc.new_gp32("uargc");
                cc.mov(argCountReg, Imm(nArgs));
                cc.mov(ptr(state, OFF_SENDARGCOUNT), argCountReg);
                Gp zero64 = cc.new_gp64("uzero");
                cc.xor_(zero64, zero64);
                cc.mov(ptr(state, OFF_ICDATAPTR), zero64);
                Gp exit = cc.new_gp32("uexit");
                cc.mov(exit, Imm(EXIT_SEND));
                cc.mov(ptr(state, OFF_EXIT), exit);
                cc.ret();
                break;
            }

            // ---- Allocations ----
            case Op::kAllocArray: {
                uint32_t size = (uint32_t)v.literal;
                Gp argSize = cc.new_gp64("aa_size");
                cc.mov(argSize, Imm((uint64_t)size));
                Gp fnReg = cc.new_gp64("aa_fn");
                cc.mov(fnReg, Imm((uint64_t)&jit_rt_sista_alloc_array));
                InvokeNode* invokeNode = nullptr;
                Error invErr = cc.invoke(
                    Out(invokeNode), fnReg,
                    FuncSignature::build<uint64_t, void*, uint64_t>());
                if (invErr != kErrorOk || !invokeNode) return bail(v.id);
                invokeNode->set_arg(0, state);
                invokeNode->set_arg(1, argSize);
                Gp dst = cc.new_gp64("aa_arr");
                invokeNode->set_ret(0, dst);
                regFor[v.id] = dst;
                break;
            }

            default:
                return bail(v.id);
            }
        }
        // Implicit fall-through: block didn't end in an explicit
        // terminator op (kBranch / kBranchIf{True,False} / kReturn /
        // kSendUnspeculated).  Control flows naturally to
        // b.successors[0]'s label, but its phi regs still need their
        // inputs filled from this block's outgoing stack first.
        // Without this, the successor reads stale (or never-written)
        // phi regs and produces garbage values that propagate as
        // cascading DNUs once they're sent #isInteger / #+ / etc.
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
    if (pharo::g_debug.sistaX86TraceOk) {
        // Dump op kinds present in the compiled method.
        bool hasCmp=false, hasBranch=false, hasArith=false;
        bool hasSend=false, hasAlloc=false;
        for (const auto& vv : method.values) {
            switch (vv.op) {
              case Op::kPrimLtInt: case Op::kPrimLeInt:
              case Op::kPrimGtInt: case Op::kPrimGeInt:
              case Op::kPrimEqInt: case Op::kPrimNeqInt:
                hasCmp = true; break;
              case Op::kBranch: case Op::kBranchIfTrue:
              case Op::kBranchIfFalse:
                hasBranch = true; break;
              case Op::kPrimAddInt: case Op::kPrimSubInt:
              case Op::kPrimMulInt:
                hasArith = true; break;
              case Op::kSendCallHelper:
              case Op::kSendInlineSelf:
              case Op::kSendCallHelperSpecial:
              case Op::kSendUnspeculated:
                hasSend = true; break;
              case Op::kAllocArray:
                hasAlloc = true; break;
              default: break;
            }
        }
        fprintf(stderr,
                "[SISTA-x86] OK fn=%p ops=%s%s%s%s%s blocks=%zu values=%zu\n",
                (void*)out,
                hasCmp ? "C" : "-",
                hasBranch ? "B" : "-",
                hasArith ? "A" : "-",
                hasSend ? "S" : "-",
                hasAlloc ? "L" : "-",
                method.blocks.size(), method.values.size());
    }
    return out;
}

}  // namespace sista
}  // namespace pharo

#endif  // PHARO_JIT_ENABLED
