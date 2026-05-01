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
#include <unordered_set>
#include <vector>

// Sista runtime helpers called from compiled code via asmjit cc.invoke.
// Defined in src/vm/jit/JITRuntime.cpp.
extern "C" uint64_t jit_rt_sista_block_create(void* state,
                                                uint64_t litIndex,
                                                uint64_t numCopied,
                                                uint64_t flags);
extern "C" uint64_t jit_rt_sista_basic_size(void* state,
                                              uint64_t rcvBits);
extern "C" uint64_t jit_rt_sista_alloc_array(void* state,
                                               uint64_t size);
extern "C" uint64_t jit_rt_sista_basic_at(void* state,
                                            uint64_t rcvBits,
                                            uint64_t idxBits);
extern "C" uint64_t jit_rt_sista_call_send(void* state,
                                             uint64_t selBits,
                                             uint64_t nArgs);
extern "C" uint64_t jit_rt_store_inst_var(void* state,
                                            uint64_t recvBits,
                                            uint64_t ivarIdx,
                                            uint64_t valBits);

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

    // Hoist tempBase + litBase loads out of every per-access ldr.
    // These pointers are set once when the JITState is initialised and
    // never change during the call; loading them once at entry lets
    // asmjit's allocator keep them in callee-saved registers across
    // the whole function — major win on tight loops that touch many
    // temps/literals (plainTo, simpleLoop, do:-bodies).
    Gp tempBaseHoisted = cc.new_gp64("tempBaseH");
    cc.ldr(tempBaseHoisted, ptr(state, OFF_TEMPBASE));
    Gp litBaseHoisted = cc.new_gp64("litBaseH");
    cc.ldr(litBaseHoisted, ptr(state, OFF_LITERALS));

    // Pre-load every literal that's actually referenced by a
    // kLoadLiteral.  Literals never change during a call, so caching
    // them in virtual regs lets asmjit keep hot ones (loop bounds,
    // literal constants) in a register across the whole function.
    // Saves one `ldr` per access in tight loops.
    std::unordered_map<uint64_t, Gp> litCache;
    {
        std::unordered_set<uint64_t> usedLitIndices;
        for (const Value& sv : method.values) {
            if (sv.op == Op::kLoadLiteral) {
                usedLitIndices.insert(sv.literal);
            }
        }
        for (uint64_t idx : usedLitIndices) {
            Gp r = cc.new_gp64("litH");
            cc.ldr(r, ptr(litBaseHoisted, (int)idx * 8));
            litCache[idx] = r;
        }
    }

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

        // Compare→branch fusion: if the block ends in
        // `kPrim<cmp>Int v_a v_b ; kBranchIf<true|false> v_cmp` and the
        // comparison's result feeds the branch directly, we can skip
        // materialising it as a true/false oop and just emit
        // `cmp; b.cond` — saves csel + two oop ldrs at the comparison
        // and a redundant cmp + trueOop ldr at the branch.  Big win on
        // inlined to:do: and whileTrue: counted-loop headers.
        uint32_t fusedCmpId = UINT32_MAX;
        asmjit::a64::CondCode fusedCond = asmjit::a64::CondCode::kEQ;
        if (b.values.size() >= 2) {
            uint32_t lastVid = b.values[b.values.size() - 1];
            uint32_t prevVid = b.values[b.values.size() - 2];
            const Value& lastV = method.valueAt(lastVid);
            const Value& prevV = method.valueAt(prevVid);
            if ((lastV.op == Op::kBranchIfTrue
              || lastV.op == Op::kBranchIfFalse)
                && lastV.operands.size() == 1
                && lastV.operands[0] == prevVid) {
                using namespace asmjit::a64;
                bool isCmp = true;
                CondCode trueCond;
                switch (prevV.op) {
                  case Op::kPrimLtInt: trueCond = CondCode::kLT; break;
                  case Op::kPrimLeInt: trueCond = CondCode::kLE; break;
                  case Op::kPrimGtInt: trueCond = CondCode::kGT; break;
                  case Op::kPrimGeInt: trueCond = CondCode::kGE; break;
                  case Op::kPrimEqInt: trueCond = CondCode::kEQ; break;
                  case Op::kPrimNeqInt: trueCond = CondCode::kNE; break;
                  default: isCmp = false; trueCond = CondCode::kEQ; break;
                }
                if (isCmp) {
                    fusedCmpId = prevVid;
                    fusedCond = (lastV.op == Op::kBranchIfTrue)
                        ? trueCond
                        // BranchIfFalse: invert the condition.
                        : (trueCond == CondCode::kLT ? CondCode::kGE
                         : trueCond == CondCode::kLE ? CondCode::kGT
                         : trueCond == CondCode::kGT ? CondCode::kLE
                         : trueCond == CondCode::kGE ? CondCode::kLT
                         : trueCond == CondCode::kEQ ? CondCode::kNE
                         : trueCond == CondCode::kNE ? CondCode::kEQ
                         : CondCode::kEQ);
                }
            }
        }

        // Per-block CSE for temp loads — avoid re-emitting `ldr` when
        // the same temp slot is loaded multiple times within a single
        // block.  After a kStoreTemp the cache is updated to the
        // stored value's reg so subsequent loads reuse it directly
        // (no memory round-trip).  Reset at block entry because the
        // value may differ on different paths into the block.
        std::unordered_map<uint64_t, Gp> tempCache;

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
                auto cached = tempCache.find(v.literal);
                if (cached != tempCache.end()) {
                    regFor[v.id] = cached->second;
                    break;
                }
                Gp dst = cc.new_gp64("temp");
                cc.ldr(dst, ptr(tempBaseHoisted,
                                 static_cast<int>(v.literal) * 8));
                regFor[v.id] = dst;
                tempCache[v.literal] = dst;
                break;
            }
            case Op::kLoadTempInVec: {
                // literal: high 32 = tempIdxOfVec, low 32 = indexInVec.
                uint32_t tempIdx = (uint32_t)(v.literal >> 32);
                uint32_t slot    = (uint32_t)(v.literal & 0xFFFFFFFFu);
                Gp vec = cc.new_gp64("vec");
                cc.ldr(vec, ptr(tempBaseHoisted, (int)tempIdx * 8));
                Gp dst = cc.new_gp64("vec_slot");
                cc.ldr(dst, ptr(vec, 8 + (int)slot * 8));
                regFor[v.id] = dst;
                break;
            }
            case Op::kStoreTempInVec: {
                if (v.operands.size() != 1) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                auto it = regFor.find(v.operands[0]);
                if (it == regFor.end()) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                uint32_t tempIdx = (uint32_t)(v.literal >> 32);
                uint32_t slot    = (uint32_t)(v.literal & 0xFFFFFFFFu);
                Gp vec = cc.new_gp64("vec");
                cc.ldr(vec, ptr(tempBaseHoisted, (int)tempIdx * 8));
                // No write barrier yet — this is fine for SmI stores
                // (typical accumulator), but if a heap-pointer is
                // stored into an old TempVec we'd need rememberObject.
                // Acceptable risk for splice MVP since the pre-pass
                // restricts patterns to SmI accumulation.
                cc.str(it->second, ptr(vec, 8 + (int)slot * 8));
                break;
            }
            case Op::kLoadLiteral: {
                auto it = litCache.find(v.literal);
                if (it != litCache.end()) {
                    regFor[v.id] = it->second;
                } else {
                    Gp dst = cc.new_gp64("lit");
                    cc.ldr(dst, ptr(litBaseHoisted,
                                     static_cast<int>(v.literal) * 8));
                    regFor[v.id] = dst;
                }
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
                cc.str(it->second,
                       ptr(tempBaseHoisted,
                           static_cast<int>(v.literal) * 8));
                // Update CSE cache so a subsequent kLoadTemp(slot)
                // reuses the stored value without re-reading memory.
                tempCache[v.literal] = it->second;
                break;
            }
            case Op::kStoreInstVar: {
                // Selective lowering:
                //   bit 63 of literal clear (lifter emission, popStoreRcv*) →
                //       SAFETY BAIL.  Methods with bytecode-level ivar
                //       stores still don't Sista-compile.  Same as before.
                //   bit 63 of literal set (setter-inline emission) →
                //       cc.invoke of jit_rt_store_inst_var which delegates
                //       to Interpreter::jitStoreInstVar (immutability /
                //       bytes / OOB guards + ObjectMemory's barrier-aware
                //       storePointerUnchecked).
                // The bit-63 marker keeps the historic safety bail for
                // bytecode-driven stores while letting Sista's setter
                // inline emit a working store.  The marker only costs the
                // setter-inline emission an OR; the lowering reads bit 63
                // once.  Low 32 bits stay = ivar index.
                if (v.operands.size() != 2) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                bool useHelper = (v.literal >> 63) & 1;
                if (!useHelper) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                auto recvIt = regFor.find(v.operands[0]);
                auto valIt  = regFor.find(v.operands[1]);
                if (recvIt == regFor.end() || valIt == regFor.end()) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                uint64_t ivarIdx = v.literal & 0x7FFFFFFFFFFFFFFFULL;
                Gp ivarReg = cc.new_gp64("siv_ivar");
                cc.mov(ivarReg, Imm(ivarIdx));
                Gp fnReg = cc.new_gp64("siv_helper");
                cc.mov(fnReg, Imm((uint64_t)&jit_rt_store_inst_var));
                asmjit::InvokeNode* invokeNode = nullptr;
                asmjit::Error invErr = cc.invoke(
                    asmjit::Out(invokeNode), fnReg,
                    asmjit::FuncSignature::build<
                        uint64_t, void*, uint64_t, uint64_t, uint64_t>());
                if (invErr != asmjit::kErrorOk || !invokeNode) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
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

                // Type narrowing: if the operand is already known-SmI
                // (kConstantOop SmI, kPrimAddInt result, or a kLoadTemp
                // narrowed by the post-build pass), the runtime check
                // is redundant.  Skip codegen — kPrimTagCheckInt's
                // output value isn't used by anyone (arith ops carry
                // their own deopt info via overflow check).
                if (method.values[v.operands[0]].type
                    == Type::kOopSmallInt) {
                    regFor[v.id] = itVal->second;  // unused but well-formed
                    break;
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
                // `tst x, #7` sets flags from (x & 7) in one instruction
                // — saves the explicit and+cmp pair.
                cc.tst(itRcv->second, Imm(7));
                cc.b_ne(missLabel);

                // ldr header word from receiver
                Gp hdr = cc.new_gp64("hdr");
                cc.ldr(hdr, ptr(itRcv->second, 0));
                Gp idx = cc.new_gp64("idx");
                cc.and_(idx, hdr, Imm(0x3FFFFF));
                // ARM64 cmp accepts a 12-bit imm (with optional shift).
                // 4096 covers ~all classIndexes seen in image bench code
                // (well below the 22-bit Spur cap).  Avoid the materialize
                // mov when the imm fits.
                if (expectedIdx <= 0xFFF) {
                    cc.cmp(idx, Imm(expectedIdx));
                } else {
                    Gp expected = cc.new_gp64("exp");
                    cc.mov(expected, Imm((uint64_t)expectedIdx));
                    cc.cmp(idx, expected);
                }
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
                using namespace asmjit::a64;
                cc.cmp(ita->second, itb->second);
                // If we're going straight into kBranchIfTrue/kBranchIfFalse,
                // the comparison's bool oop is never read — skip the
                // csel and the trueOop/falseOop loads.  The branch
                // emits `b.cond` directly using fusedCond.
                if (v.id == fusedCmpId) {
                    // Still record SOMETHING in regFor so anyone scanning
                    // doesn't trip on a missing entry; the value is
                    // never actually read, but the input regs are
                    // valid placeholders.
                    regFor[v.id] = ita->second;
                    break;
                }
                Gp trueOop = cc.new_gp64("true");
                Gp falseOop = cc.new_gp64("false");
                cc.ldr(trueOop, ptr(state, OFF_TRUEOOP));
                cc.ldr(falseOop, ptr(state, OFF_FALSEOOP));
                Gp dst = cc.new_gp64("intcmp");
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
                // Mul uses smulh + bits 60-63 check (see below).
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
                // Constant-fold tag adjustments when the second operand
                // is a kConstantOop SmI: encoded(a) + encoded(K) - 1
                // = a + (K<<3); encoded(a) - encoded(K) + 1 = a - (K<<3).
                // Skips the +/- 1 instruction (the mov-of-constant remains
                // because deopt-stack rebuild still needs it on the cold
                // path, but asmjit can sink it).
                bool didFold = false;
                if ((v.op == Op::kPrimAddInt || v.op == Op::kPrimSubInt)
                    && v.operands.size() >= 2) {
                    const Value& bv = method.valueAt(v.operands[1]);
                    if (bv.op == Op::kConstantOop
                        && (bv.literal & 7) == 1) {
                        int64_t kEnc = (int64_t)((bv.literal >> 3));
                        int64_t imm = kEnc * 8;
                        // ARM64 add/sub imm: 12-bit positive (0..4095),
                        // or 12-bit shifted by 12 (0..4095 << 12).
                        // Bail if imm doesn't fit either form.
                        if (imm >= 0 && (imm <= 4095
                                       || (imm % 4096 == 0
                                        && imm / 4096 <= 4095))) {
                            if (v.op == Op::kPrimAddInt) {
                                cc.add(dst, ita->second, Imm(imm));
                            } else {  // kPrimSubInt
                                cc.sub(dst, ita->second, Imm(imm));
                            }
                            didFold = true;
                        }
                    }
                }
                if (!didFold && v.op == Op::kPrimAddInt) {
                    cc.add(dst, ita->second, itb->second);
                    cc.sub(dst, dst, Imm(1));
                } else if (!didFold && v.op == Op::kPrimSubInt) {
                    cc.sub(dst, ita->second, itb->second);
                    cc.add(dst, dst, Imm(1));
                } else if (v.op == Op::kPrimMulInt) {
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
                        // Use tbz with the *continue* label so the
                        // hot path is the branch-taken path (no
                        // unconditional-branch follow-up); fall-through
                        // is the deopt sequence which ends in `ret`.
                        Gp ovcheck = cc.new_gp64("ovcheck");
                        Gp shifted = cc.new_gp64("ovshift");
                        cc.lsl(shifted, dst, Imm(1));
                        cc.eor(ovcheck, shifted, dst);
                        cc.tbz(ovcheck, Imm(63), contLabel);
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
                Gp selReg = cc.new_gp64("sendSel");
                cc.ldr(selReg,
                       ptr(litBaseHoisted, static_cast<int>(selIdx) * 8));

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
                // Flush the FULL framepoint stack to interp.sp so the
                // resumed interpreter sees every live IR value, then
                // bail at source bcOffset.  Helper already popped
                // rcvr+args from interp.sp before invoke, so we re-
                // push them as part of the framepoint replay.
                //
                // 2026-04-29 fix (B7): previously only operands
                // (rcvr+args) were re-pushed, leaving IR-stack values
                // below the send invisible to the resumed interpreter.
                const std::vector<uint32_t>* fpStack = nullptr;
                for (const auto& fp : method.framepoints) {
                    if (fp.valueId == v.id) {
                        fpStack = &fp.stackValueIds;
                        break;
                    }
                }
                Label noDeopt = cc.new_label();
                cc.cbnz(dst, noDeopt);
                {
                    // Restore framepoint stack (or operands if
                    // framepoint is missing — shouldn't happen with
                    // the fixed builder path).
                    Gp sp2 = cc.new_gp64("send_sp_dz");
                    cc.ldr(sp2, ptr(state, OFF_SP));
                    const std::vector<uint32_t>& flushIds =
                        fpStack ? *fpStack : v.operands;
                    for (size_t opIdx = 0;
                         opIdx < flushIds.size(); opIdx++) {
                        auto opIt = regFor.find(flushIds[opIdx]);
                        if (opIt == regFor.end()) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        cc.str(opIt->second,
                               ptr(sp2,
                                   static_cast<int>(opIdx) * 8));
                    }
                    cc.add(sp2, sp2,
                           Imm(static_cast<int>(flushIds.size()) * 8));
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
            case Op::kAllocArray: {
                using namespace asmjit::a64;
                uint32_t size = (uint32_t)v.literal;
                Gp argSize = cc.new_gp64("arrSize");
                cc.mov(argSize, Imm((uint64_t)size));
                Gp fnReg = cc.new_gp64("allocArrayFn");
                cc.mov(fnReg,
                       Imm((uint64_t)&jit_rt_sista_alloc_array));
                asmjit::InvokeNode* invokeNode = nullptr;
                asmjit::Error invErr = cc.invoke(
                    asmjit::Out(invokeNode), fnReg,
                    asmjit::FuncSignature::build<
                        uint64_t, void*, uint64_t>());
                if (invErr != asmjit::kErrorOk || !invokeNode) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                invokeNode->set_arg(0, state);
                invokeNode->set_arg(1, argSize);
                Gp dst = cc.new_gp64("newArr");
                invokeNode->set_ret(0, dst);
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

                // Verify every block op is in our extended whitelist.
                // Side-effecting ops (kStoreTemp, kStoreInstVar,
                // kSendUnspeculated etc.) would need deopt-with-resume
                // infrastructure to roll back partial work — we don't
                // have that yet, so reject anything with side effects.
                //
                // kPrimTagCheckInt is rejected because we do our own tag
                // check (deopting to the do: bytecode, not the block
                // body's bcOffset which would be wrong).
                bool blockHasArith = false;
                for (const auto& bv : blockIR.values) {
                    switch (bv.op) {
                    case Op::kLoadTemp:
                    case Op::kLoadReceiver:
                    case Op::kLoadInstVar:
                    case Op::kLoadTrueOop:
                    case Op::kLoadFalseOop:
                    case Op::kConstantOop:
                    case Op::kLoadLiteral:
                    case Op::kReturn:
                    case Op::kPrimTagCheckInt:
                        // Passthrough — our own tag check fires before
                        // each arith op in the splice context.
                        break;
                    case Op::kPrimAddInt:
                    case Op::kPrimSubInt:
                    case Op::kPrimMulInt:
                        blockHasArith = true;
                        break;
                    default:
                        if (failedAtValue) *failedAtValue = v.id;
                        return nullptr;
                    }
                }
                (void)blockHasArith;  // currently unused; reserved for
                                      // a future "pre-loop tag check"
                                      // optimization.

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

                // Format check: only fast-path format 2 (Indexable64).
                Gp loop_hdr = cc.new_gp64("loop_hdr");
                cc.ldr(loop_hdr, ptr(rcvReg));
                Gp loop_fmt = cc.new_gp64("loop_fmt");
                cc.ubfx(loop_fmt, loop_hdr, Imm(24), Imm(5));
                cc.cmp(loop_fmt, Imm(2));
                Label loopFmtOk = cc.new_label();
                cc.b_eq(loopFmtOk);
                emitDeopt(rcvReg, deoptBC);
                cc.bind(loopFmtOk);

                // Step 2: iReg = SmI(1) = (1 << 3) | 1 = 9.
                Gp iReg = cc.new_gp64("loop_i");
                cc.mov(iReg, Imm(9));

                // Step 3: rotated loop head — pre-check, fused back-edge.
                Label loopHead = cc.new_label();
                Label loopExit = cc.new_label();
                cc.cmp(iReg, sizeReg);
                cc.b_hi(loopExit);
                cc.bind(loopHead);

                // Element-use analysis: only emit per-iter element
                // load if the block actually references it via
                // kLoadTemp(0).  Blocks like `[:e | self foo]` skip
                // the load.  do: discards the block result so we
                // don't even need the SmI tag-check that arith ops
                // do — the existing kPrimTagCheckInt cases handle
                // that for the operands they actually use.
                bool blockUsesElement = false;
                for (const auto& bv : blockIR.values) {
                    if (bv.op == Op::kLoadTemp && bv.literal == 0) {
                        blockUsesElement = true;
                        break;
                    }
                }

                // Step 5: inline format-2 element load.  i*8 = iReg-1.
                Gp eachReg = cc.new_gp64("loop_each");
                if (blockUsesElement) {
                    Gp loop_off = cc.new_gp64("loop_off");
                    cc.sub(loop_off, iReg, Imm(1));
                    cc.ldr(eachReg, ptr(rcvReg, loop_off));
                }

                // Step 6: inline block body.  Each block IR value
                // produces a register; we track them in a block-local
                // map.  kLoadTemp(0) maps to eachReg (the block argument
                // = current array element).  kPrimAddInt and friends
                // emit tag check + SmI math, with miss deopting to the
                // do: bytecode.
                //
                // Side-effect-free guarantee: the whitelist above
                // forbids stores and sends, so a mid-loop deopt is
                // safe — no state has been mutated yet (the block's
                // return value is discarded by do:).
                std::unordered_map<uint32_t, Gp> blockRegs;
                for (const auto& bv : blockIR.values) {
                    switch (bv.op) {
                    case Op::kLoadTemp: {
                        // Block arg 0 = `e` = eachReg.  Block arg
                        // ordering: temp 0 is the first block argument.
                        // Block local temps (idx > numArgs - 1) need
                        // separate storage we don't have; reject.
                        uint32_t idx = (uint32_t)bv.literal;
                        if (idx == 0) {
                            blockRegs[bv.id] = eachReg;
                        } else {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        break;
                    }
                    case Op::kLoadTrueOop: {
                        Gp r = cc.new_gp64("blk_true");
                        cc.ldr(r, ptr(state, OFF_TRUEOOP));
                        blockRegs[bv.id] = r;
                        break;
                    }
                    case Op::kLoadFalseOop: {
                        Gp r = cc.new_gp64("blk_false");
                        cc.ldr(r, ptr(state, OFF_FALSEOOP));
                        blockRegs[bv.id] = r;
                        break;
                    }
                    case Op::kConstantOop: {
                        Gp r = cc.new_gp64("blk_const");
                        cc.mov(r, Imm(bv.literal));
                        blockRegs[bv.id] = r;
                        break;
                    }
                    case Op::kLoadLiteral: {
                        // Block's literal vector is the same memory as
                        // the outer method's literal vector (a
                        // CompiledBlock shares the outer CompiledMethod
                        // literals).  Use state.literals just like the
                        // main lift.
                        uint32_t idx = (uint32_t)bv.literal;
                        Gp r = cc.new_gp64("blk_lit");
                        cc.ldr(r, ptr(litBaseHoisted, (int)(idx * 8)));
                        blockRegs[bv.id] = r;
                        break;
                    }
                    case Op::kLoadReceiver: {
                        // Block's receiver is the outer-method's
                        // receiver (closures inherit `self`).  In the
                        // splice context we're inlined into the outer's
                        // frame; state.receiver IS the right one.
                        Gp r = cc.new_gp64("blk_recv");
                        cc.ldr(r, ptr(state, OFF_RECEIVER));
                        blockRegs[bv.id] = r;
                        break;
                    }
                    case Op::kLoadInstVar: {
                        // operand[0] is the receiver pointer; literal
                        // is the slot index.  Slots live at offset
                        // 8 + idx*8 from object pointer (header is 8B).
                        // Side-effect-free read; safe in splice context
                        // since the loop's deopt path doesn't depend on
                        // the loaded value being preserved.
                        if (bv.operands.empty()) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        auto it = blockRegs.find(bv.operands[0]);
                        if (it == blockRegs.end()) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        Gp dst = cc.new_gp64("blk_ivar");
                        cc.ldr(dst, ptr(it->second,
                                         8 + static_cast<int>(bv.literal) * 8));
                        blockRegs[bv.id] = dst;
                        break;
                    }
                    case Op::kPrimTagCheckInt: {
                        // Passthrough: output reg = input reg.  Our
                        // splice does its own tag check before arith.
                        if (bv.operands.empty()) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        auto it = blockRegs.find(bv.operands[0]);
                        if (it == blockRegs.end()) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        blockRegs[bv.id] = it->second;
                        break;
                    }
                    case Op::kPrimAddInt:
                    case Op::kPrimSubInt:
                    case Op::kPrimMulInt: {
                        if (bv.operands.size() < 2) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        auto ita = blockRegs.find(bv.operands[0]);
                        auto itb = blockRegs.find(bv.operands[1]);
                        if (ita == blockRegs.end()
                            || itb == blockRegs.end()) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        // Tag check both operands; on miss, deopt to
                        // the do: bytecode.  Block hasn't mutated state
                        // yet (whitelist forbids stores), so resume is
                        // safe.
                        Gp tagA = cc.new_gp64("blk_tagA");
                        cc.and_(tagA, ita->second, Imm(7));
                        cc.cmp(tagA, Imm(1));
                        Label tagAok = cc.new_label();
                        cc.b_eq(tagAok);
                        emitDeopt(rcvReg, deoptBC);
                        cc.bind(tagAok);
                        Gp tagB = cc.new_gp64("blk_tagB");
                        cc.and_(tagB, itb->second, Imm(7));
                        cc.cmp(tagB, Imm(1));
                        Label tagBok = cc.new_label();
                        cc.b_eq(tagBok);
                        emitDeopt(rcvReg, deoptBC);
                        cc.bind(tagBok);

                        Gp dst = cc.new_gp64("blk_arith");
                        if (bv.op == Op::kPrimAddInt) {
                            cc.add(dst, ita->second, itb->second);
                            cc.sub(dst, dst, Imm(1));
                        } else if (bv.op == Op::kPrimSubInt) {
                            cc.sub(dst, ita->second, itb->second);
                            cc.add(dst, dst, Imm(1));
                        } else {
                            // mul: untag, multiply, re-tag.  No overflow
                            // check yet — large products will silently
                            // wrap.  Acceptable in MVP since we only
                            // splice for blocks with no observable
                            // side effects.
                            Gp au = cc.new_gp64("blk_au");
                            Gp bu = cc.new_gp64("blk_bu");
                            cc.asr(au, ita->second, Imm(3));
                            cc.asr(bu, itb->second, Imm(3));
                            Gp prod = cc.new_gp64("blk_prod");
                            cc.mul(prod, au, bu);
                            cc.lsl(dst, prod, Imm(3));
                            cc.orr(dst, dst, Imm(1));
                        }
                        blockRegs[bv.id] = dst;
                        break;
                    }
                    case Op::kReturn:
                        // do: discards block result; nothing to emit.
                        break;
                    default:
                        // Should never get here — whitelist already
                        // checked.  Defensive bail.
                        if (failedAtValue) *failedAtValue = v.id;
                        return nullptr;
                    }
                }

                // Step 7: iReg += 8 (tag-preserving SmI add: SmI(a) +
                // SmI(b) = (a<<3 | 1) + (b<<3 | 1) - 1.  For b=1:
                // result = iReg + 9 - 1 = iReg + 8.).
                cc.add(iReg, iReg, Imm(8));

                // Step 8: fused cmp+b.ls back-edge (rotated loop).
                cc.cmp(iReg, sizeReg);
                cc.b_ls(loopHead);

                // Step 9: loop exit.
                cc.bind(loopExit);

                // Step 10: kCountedLoopDo's result is the receiver.
                regFor[v.id] = rcvReg;
                break;
            }
            case Op::kCountedLoopInjectInto: {
                // B2 splice (inject:into: variant): counted at: loop
                // with the accumulator held in a register (accReg).
                // Block has 2 args: temp 0 = acc, temp 1 = elem.  Block
                // returns the new acc as kReturn's value, which we
                // assign to accReg.
                //
                // operands[0] = rcv (the source Array)
                // operands[1] = init (initial accumulator)
                // operands[2] = vecRef (captured TempVector, only when
                //                       block has numCopied=1)
                using namespace asmjit::a64;
                if (v.operands.size() != 2 && v.operands.size() != 3) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                auto itRcv = regFor.find(v.operands[0]);
                auto itInit = regFor.find(v.operands[1]);
                if (itRcv == regFor.end() || itInit == regFor.end()) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                bool injectHasCapture = (v.operands.size() == 3);
                Gp injectVecReg;
                if (injectHasCapture) {
                    auto itVec = regFor.find(v.operands[2]);
                    if (itVec == regFor.end()) {
                        if (failedAtValue) *failedAtValue = v.id;
                        return nullptr;
                    }
                    injectVecReg = itVec->second;
                }

                uint32_t blockSlot = (uint32_t)v.literal;
                if (blockSlot >= method.inlinedBlocks.size()
                    || !method.inlinedBlocks[blockSlot]) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                const Method& blockIR = *method.inlinedBlocks[blockSlot];

                if (blockIR.blocks.size() != 1) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }

                // Same whitelist as kCountedLoopDo's body — loads,
                // arith, comparison, identity, tag-check passthrough,
                // return.  No stores.  Boolean-producing ops can
                // become the new acc.  kLoadTempInVec admitted only
                // when injectHasCapture (operand[2] = vecRef set).
                for (const auto& bv : blockIR.values) {
                    switch (bv.op) {
                    case Op::kLoadTemp:
                    case Op::kLoadReceiver:
                    case Op::kLoadInstVar:
                    case Op::kLoadTrueOop:
                    case Op::kLoadFalseOop:
                    case Op::kConstantOop:
                    case Op::kLoadLiteral:
                    case Op::kReturn:
                    case Op::kPrimTagCheckInt:
                    case Op::kPrimAddInt:
                    case Op::kPrimSubInt:
                    case Op::kPrimMulInt:
                    case Op::kPrimLtInt:
                    case Op::kPrimLeInt:
                    case Op::kPrimGtInt:
                    case Op::kPrimGeInt:
                    case Op::kPrimEqInt:
                    case Op::kPrimNeqInt:
                    case Op::kPrimIdentityEq:
                    case Op::kPrimIdentityNeq:
                        break;
                    case Op::kLoadTempInVec:
                        if (!injectHasCapture) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        break;
                    default:
                        if (failedAtValue) *failedAtValue = v.id;
                        return nullptr;
                    }
                }

                Gp rcvReg = itRcv->second;
                Gp initReg = itInit->second;

                // accReg holds the running accumulator.  Start from
                // initReg.  On mid-loop deopt, accReg is discarded —
                // the interpreter's stack is restored to [rcv, init,
                // block] and the inject:into: send re-runs from the
                // beginning with the original init.
                Gp accReg = cc.new_gp64("inject_acc");
                cc.mov(accReg, initReg);

                // Source bcOffset for deopt resume.
                uint32_t deoptBC = 0;
                for (const auto& fp : method.framepoints) {
                    if (fp.valueId == v.id) {
                        deoptBC = fp.bcOffset;
                        break;
                    }
                }

                // Re-create the block sub-IR's CompiledBlock byte ptr
                // — we don't have one here, but we don't need one for
                // the splice context (no kFrameState references the
                // block).  Lambda for deopt: rebuild interpreter
                // stack [rcv, init, block].  But we don't have the
                // block oop in the IR — so for now, deopt pushes only
                // [rcv, init] and lets the interpreter re-execute
                // PushFullBlock + Send2 from the bcOffset.  This works
                // because deoptBC points at PushFullBlock, which the
                // interpreter executes normally to allocate the block,
                // then Send2 dispatches inject:into:.
                auto emitDeopt = [&](Gp savedRcv, Gp savedInit,
                                      uint32_t bc) {
                    Gp sp = cc.new_gp64("inj_dz_sp");
                    cc.ldr(sp, ptr(state, OFF_SP));
                    cc.str(savedRcv, ptr(sp));
                    cc.str(savedInit, ptr(sp, 8));
                    cc.add(sp, sp, Imm(16));
                    cc.str(sp, ptr(state, OFF_SP));
                    Gp ipReg = cc.new_gp64("inj_dz_ip");
                    if (bytecodeBase) {
                        uintptr_t addr =
                            reinterpret_cast<uintptr_t>(bytecodeBase) + bc;
                        cc.mov(ipReg, Imm((uint64_t)addr));
                    } else {
                        cc.mov(ipReg, Imm((uint64_t)bc));
                    }
                    cc.str(ipReg, ptr(state, OFF_IP));
                    Gp argc = cc.new_gp32("inj_dz_argc");
                    cc.mov(argc, Imm(0));
                    cc.str(argc, ptr(state, OFF_SENDARGCOUNT));
                    Gp z64 = cc.new_gp64("inj_dz_zero");
                    cc.mov(z64, Imm(0));
                    cc.str(z64, ptr(state, OFF_ICDATAPTR));
                    Gp exitR = cc.new_gp32("inj_dz_exit");
                    cc.mov(exitR, Imm(EXIT_SEND));
                    cc.str(exitR, ptr(state, OFF_EXIT));
                    cc.ret();
                };

                // Step 1: sizeReg = basicSize(rcv).  Deopt on 0.
                Gp sizeFn = cc.new_gp64("inj_szfn");
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
                Gp sizeReg = cc.new_gp64("inj_size");
                sizeInvoke->set_ret(0, sizeReg);

                Label sizeOk = cc.new_label();
                cc.cbnz(sizeReg, sizeOk);
                emitDeopt(rcvReg, initReg, deoptBC);
                cc.bind(sizeOk);

                // Format check: only fast-path format 2 (Indexable64).
                Gp inj_hdr = cc.new_gp64("inj_hdr");
                cc.ldr(inj_hdr, ptr(rcvReg));
                Gp inj_fmt = cc.new_gp64("inj_fmt");
                cc.ubfx(inj_fmt, inj_hdr, Imm(24), Imm(5));
                cc.cmp(inj_fmt, Imm(2));
                Label injFmtOk = cc.new_label();
                cc.b_eq(injFmtOk);
                emitDeopt(rcvReg, initReg, deoptBC);
                cc.bind(injFmtOk);

                // Step 2: iReg = SmI(1).
                Gp iReg = cc.new_gp64("inj_i");
                cc.mov(iReg, Imm(9));

                // Rotated loop: pre-check, fused back-edge.
                Label loopHead = cc.new_label();
                Label loopExit = cc.new_label();
                cc.cmp(iReg, sizeReg);
                cc.b_hi(loopExit);
                cc.bind(loopHead);

                // Element-use analysis: temp 1 = element for
                // inject:into:.  Skip per-iter load if the block
                // doesn't reference temp 1 (rare but valid pattern,
                // e.g. inject:0 into: [:acc :_ | acc + 1] = count).
                bool blockUsesElement = false;
                for (const auto& bv : blockIR.values) {
                    if (bv.op == Op::kLoadTemp && bv.literal == 1) {
                        blockUsesElement = true;
                        break;
                    }
                }

                // Inline format-2 element load.  i*8 = iReg-1.
                Gp eachReg = cc.new_gp64("inj_each");
                if (blockUsesElement) {
                    Gp inj_off = cc.new_gp64("inj_off");
                    cc.sub(inj_off, iReg, Imm(1));
                    cc.ldr(eachReg, ptr(rcvReg, inj_off));
                }

                // Inline block body.  Block has 2 args:
                //   temp 0 = acc → accReg (READ from accReg, WRITE to
                //                          accReg via kReturn)
                //   temp 1 = elem → eachReg
                std::unordered_map<uint32_t, Gp> blockRegs;
                bool sawReturn = false;
                for (const auto& bv : blockIR.values) {
                    switch (bv.op) {
                    case Op::kLoadTemp: {
                        uint32_t idx = (uint32_t)bv.literal;
                        if (idx == 0) {
                            blockRegs[bv.id] = accReg;
                        } else if (idx == 1) {
                            blockRegs[bv.id] = eachReg;
                        } else {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        break;
                    }
                    case Op::kLoadTrueOop: {
                        Gp r = cc.new_gp64("ij_true");
                        cc.ldr(r, ptr(state, OFF_TRUEOOP));
                        blockRegs[bv.id] = r;
                        break;
                    }
                    case Op::kLoadFalseOop: {
                        Gp r = cc.new_gp64("ij_false");
                        cc.ldr(r, ptr(state, OFF_FALSEOOP));
                        blockRegs[bv.id] = r;
                        break;
                    }
                    case Op::kConstantOop: {
                        Gp r = cc.new_gp64("ij_const");
                        cc.mov(r, Imm(bv.literal));
                        blockRegs[bv.id] = r;
                        break;
                    }
                    case Op::kLoadLiteral: {
                        uint32_t idx = (uint32_t)bv.literal;
                        Gp r = cc.new_gp64("ij_lit");
                        cc.ldr(r, ptr(litBaseHoisted, (int)(idx * 8)));
                        blockRegs[bv.id] = r;
                        break;
                    }
                    case Op::kLoadReceiver: {
                        Gp r = cc.new_gp64("ij_recv");
                        cc.ldr(r, ptr(state, OFF_RECEIVER));
                        blockRegs[bv.id] = r;
                        break;
                    }
                    case Op::kLoadInstVar: {
                        // operand[0] is the receiver pointer; literal
                        // is the slot index.  Side-effect-free read,
                        // safe in inject:into: splice context (deopt
                        // rolls back to init regardless).
                        if (bv.operands.empty()) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        auto it = blockRegs.find(bv.operands[0]);
                        if (it == blockRegs.end()) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        Gp dst = cc.new_gp64("ij_ivar");
                        cc.ldr(dst, ptr(it->second,
                                         8 + static_cast<int>(bv.literal) * 8));
                        blockRegs[bv.id] = dst;
                        break;
                    }
                    case Op::kLoadTempInVec: {
                        // Read slot from captured TempVector.
                        // Pre-pass verified injectHasCapture and
                        // vec idx == 1.
                        if (!injectHasCapture) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        uint32_t slot =
                            (uint32_t)(bv.literal & 0xFFFFFFFFu);
                        Gp dst = cc.new_gp64("ij_vec");
                        cc.ldr(dst, ptr(injectVecReg,
                                         8 + static_cast<int>(slot) * 8));
                        blockRegs[bv.id] = dst;
                        break;
                    }
                    case Op::kPrimTagCheckInt: {
                        if (bv.operands.empty()) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        auto it = blockRegs.find(bv.operands[0]);
                        if (it == blockRegs.end()) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        blockRegs[bv.id] = it->second;
                        break;
                    }
                    case Op::kPrimAddInt:
                    case Op::kPrimSubInt:
                    case Op::kPrimMulInt: {
                        if (bv.operands.size() < 2) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        auto ita = blockRegs.find(bv.operands[0]);
                        auto itb = blockRegs.find(bv.operands[1]);
                        if (ita == blockRegs.end()
                            || itb == blockRegs.end()) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        Gp tagA = cc.new_gp64("ij_tagA");
                        cc.and_(tagA, ita->second, Imm(7));
                        cc.cmp(tagA, Imm(1));
                        Label tagAok = cc.new_label();
                        cc.b_eq(tagAok);
                        emitDeopt(rcvReg, initReg, deoptBC);
                        cc.bind(tagAok);
                        Gp tagB = cc.new_gp64("ij_tagB");
                        cc.and_(tagB, itb->second, Imm(7));
                        cc.cmp(tagB, Imm(1));
                        Label tagBok = cc.new_label();
                        cc.b_eq(tagBok);
                        emitDeopt(rcvReg, initReg, deoptBC);
                        cc.bind(tagBok);

                        Gp dst = cc.new_gp64("ij_arith");
                        if (bv.op == Op::kPrimAddInt) {
                            cc.add(dst, ita->second, itb->second);
                            cc.sub(dst, dst, Imm(1));
                        } else if (bv.op == Op::kPrimSubInt) {
                            cc.sub(dst, ita->second, itb->second);
                            cc.add(dst, dst, Imm(1));
                        } else {
                            Gp au = cc.new_gp64("ij_au");
                            Gp bu = cc.new_gp64("ij_bu");
                            cc.asr(au, ita->second, Imm(3));
                            cc.asr(bu, itb->second, Imm(3));
                            Gp prod = cc.new_gp64("ij_prod");
                            cc.mul(prod, au, bu);
                            cc.lsl(dst, prod, Imm(3));
                            cc.orr(dst, dst, Imm(1));
                        }
                        blockRegs[bv.id] = dst;
                        break;
                    }
                    case Op::kPrimLtInt:
                    case Op::kPrimLeInt:
                    case Op::kPrimGtInt:
                    case Op::kPrimGeInt:
                    case Op::kPrimEqInt:
                    case Op::kPrimNeqInt: {
                        // SmallInt comparison.  Tag-check both ops as
                        // SmI; deopt to the inject:into: bytecode on
                        // miss (rolls back to original init).  Boolean
                        // result becomes the new accumulator if used
                        // by kReturn.
                        if (bv.operands.size() < 2) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        auto ita = blockRegs.find(bv.operands[0]);
                        auto itb = blockRegs.find(bv.operands[1]);
                        if (ita == blockRegs.end()
                            || itb == blockRegs.end()) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        Gp tagA = cc.new_gp64("ij_cmpTagA");
                        cc.and_(tagA, ita->second, Imm(7));
                        cc.cmp(tagA, Imm(1));
                        Label cmpAok = cc.new_label();
                        cc.b_eq(cmpAok);
                        emitDeopt(rcvReg, initReg, deoptBC);
                        cc.bind(cmpAok);
                        Gp tagB = cc.new_gp64("ij_cmpTagB");
                        cc.and_(tagB, itb->second, Imm(7));
                        cc.cmp(tagB, Imm(1));
                        Label cmpBok = cc.new_label();
                        cc.b_eq(cmpBok);
                        emitDeopt(rcvReg, initReg, deoptBC);
                        cc.bind(cmpBok);

                        Gp trueOop = cc.new_gp64("ij_cmpTrue");
                        Gp falseOop = cc.new_gp64("ij_cmpFalse");
                        cc.ldr(trueOop, ptr(state, OFF_TRUEOOP));
                        cc.ldr(falseOop, ptr(state, OFF_FALSEOOP));
                        cc.cmp(ita->second, itb->second);
                        Gp dst = cc.new_gp64("ij_cmp");
                        CondCode cond;
                        switch (bv.op) {
                          case Op::kPrimLtInt:  cond = CondCode::kLT; break;
                          case Op::kPrimLeInt:  cond = CondCode::kLE; break;
                          case Op::kPrimGtInt:  cond = CondCode::kGT; break;
                          case Op::kPrimGeInt:  cond = CondCode::kGE; break;
                          case Op::kPrimEqInt:  cond = CondCode::kEQ; break;
                          case Op::kPrimNeqInt: cond = CondCode::kNE; break;
                          default:              cond = CondCode::kEQ; break;
                        }
                        cc.csel(dst, trueOop, falseOop, cond);
                        blockRegs[bv.id] = dst;
                        break;
                    }
                    case Op::kPrimIdentityEq:
                    case Op::kPrimIdentityNeq: {
                        // Identity: no tag check, semantics universal.
                        if (bv.operands.size() != 2) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        auto ita = blockRegs.find(bv.operands[0]);
                        auto itb = blockRegs.find(bv.operands[1]);
                        if (ita == blockRegs.end()
                            || itb == blockRegs.end()) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        Gp trueOop = cc.new_gp64("ij_idTrue");
                        Gp falseOop = cc.new_gp64("ij_idFalse");
                        cc.ldr(trueOop, ptr(state, OFF_TRUEOOP));
                        cc.ldr(falseOop, ptr(state, OFF_FALSEOOP));
                        cc.cmp(ita->second, itb->second);
                        Gp dst = cc.new_gp64("ij_idcmp");
                        cc.csel(dst, trueOop, falseOop,
                                bv.op == Op::kPrimIdentityEq
                                    ? CondCode::kEQ
                                    : CondCode::kNE);
                        blockRegs[bv.id] = dst;
                        break;
                    }
                    case Op::kReturn: {
                        // Block return = new accumulator value.
                        // Update accReg.  We don't actually exit the
                        // loop — kReturn here just commits the new
                        // acc; the loop continues to next iteration.
                        if (bv.operands.size() != 1) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        auto it = blockRegs.find(bv.operands[0]);
                        if (it == blockRegs.end()) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        cc.mov(accReg, it->second);
                        sawReturn = true;
                        break;
                    }
                    default:
                        if (failedAtValue) *failedAtValue = v.id;
                        return nullptr;
                    }
                }

                if (!sawReturn) {
                    // Block must have a kReturn for inject:into:
                    // (otherwise we don't know what the new acc is).
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }

                // i += 8 (SmI add of 1); fused cmp+b.ls back-edge.
                cc.add(iReg, iReg, Imm(8));
                cc.cmp(iReg, sizeReg);
                cc.b_ls(loopHead);

                cc.bind(loopExit);

                // Result = accReg.
                regFor[v.id] = accReg;
                break;
            }
            case Op::kInterval: {
                // Marker IR op produced by the to: handler when a
                // recognized Interval-inject pattern is in flight.
                // kCountedLoopIntervalInjectInto reads start/stop
                // directly from earlier loads (not via this value),
                // so the marker has nothing to lower.  Silent skip.
                break;
            }
            case Op::kCountedLoopIntervalInjectInto: {
                // (start to: stop) inject: init into: [block].
                // Iterates i from start to stop directly (no Interval
                // allocation, no basicAt) with the accumulator held
                // in a register.  Block has 2 args:
                //   temp 0 = acc → accReg
                //   temp 1 = i (SmI Oop) → iReg
                //
                // operands[0..2] = start, stop, init.  operands[3] =
                //   captured TempVector (only when block has numCopied=1).
                using namespace asmjit::a64;
                if (v.operands.size() != 3 && v.operands.size() != 4) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                auto itStart = regFor.find(v.operands[0]);
                auto itStop  = regFor.find(v.operands[1]);
                auto itInit  = regFor.find(v.operands[2]);
                if (itStart == regFor.end()
                    || itStop == regFor.end()
                    || itInit == regFor.end()) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                bool ivInjectHasCapture = (v.operands.size() == 4);
                Gp ivInjectVecReg;
                if (ivInjectHasCapture) {
                    auto itVec = regFor.find(v.operands[3]);
                    if (itVec == regFor.end()) {
                        if (failedAtValue) *failedAtValue = v.id;
                        return nullptr;
                    }
                    ivInjectVecReg = itVec->second;
                }

                uint32_t blockSlot = (uint32_t)v.literal;
                if (blockSlot >= method.inlinedBlocks.size()
                    || !method.inlinedBlocks[blockSlot]) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                const Method& blockIR = *method.inlinedBlocks[blockSlot];
                if (blockIR.blocks.size() != 1) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }

                // Same whitelist as kCountedLoopInjectInto's body.
                // kLoadTempInVec admitted only when ivInjectHasCapture.
                for (const auto& bv : blockIR.values) {
                    switch (bv.op) {
                    case Op::kLoadTemp:
                    case Op::kLoadReceiver:
                    case Op::kLoadInstVar:
                    case Op::kLoadTrueOop:
                    case Op::kLoadFalseOop:
                    case Op::kConstantOop:
                    case Op::kLoadLiteral:
                    case Op::kReturn:
                    case Op::kPrimTagCheckInt:
                    case Op::kPrimAddInt:
                    case Op::kPrimSubInt:
                    case Op::kPrimMulInt:
                    case Op::kPrimLtInt:
                    case Op::kPrimLeInt:
                    case Op::kPrimGtInt:
                    case Op::kPrimGeInt:
                    case Op::kPrimEqInt:
                    case Op::kPrimNeqInt:
                    case Op::kPrimIdentityEq:
                    case Op::kPrimIdentityNeq:
                        break;
                    case Op::kLoadTempInVec:
                        if (!ivInjectHasCapture) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        break;
                    default:
                        if (failedAtValue) *failedAtValue = v.id;
                        return nullptr;
                    }
                }

                Gp startReg = itStart->second;
                Gp stopReg  = itStop->second;
                Gp initReg  = itInit->second;

                // accReg starts as init.  Mid-loop deopt = no commit.
                Gp accReg = cc.new_gp64("iv_acc");
                cc.mov(accReg, initReg);

                uint32_t deoptBC = 0;
                for (const auto& fp : method.framepoints) {
                    if (fp.valueId == v.id) {
                        deoptBC = fp.bcOffset;
                        break;
                    }
                }

                // emitDeopt: rebuild interpreter stack [start, stop]
                // and set ip to the to: bytecode.  Interpreter then
                // re-runs to: → Interval allocation → inject:into:.
                auto emitDeopt = [&](Gp savedStart, Gp savedStop,
                                      uint32_t bc) {
                    Gp sp = cc.new_gp64("iv_dz_sp");
                    cc.ldr(sp, ptr(state, OFF_SP));
                    cc.str(savedStart, ptr(sp));
                    cc.str(savedStop, ptr(sp, 8));
                    cc.add(sp, sp, Imm(16));
                    cc.str(sp, ptr(state, OFF_SP));
                    Gp ipReg = cc.new_gp64("iv_dz_ip");
                    if (bytecodeBase) {
                        uintptr_t addr =
                            reinterpret_cast<uintptr_t>(bytecodeBase) + bc;
                        cc.mov(ipReg, Imm((uint64_t)addr));
                    } else {
                        cc.mov(ipReg, Imm((uint64_t)bc));
                    }
                    cc.str(ipReg, ptr(state, OFF_IP));
                    Gp argc = cc.new_gp32("iv_dz_argc");
                    cc.mov(argc, Imm(0));
                    cc.str(argc, ptr(state, OFF_SENDARGCOUNT));
                    Gp z64 = cc.new_gp64("iv_dz_zero");
                    cc.mov(z64, Imm(0));
                    cc.str(z64, ptr(state, OFF_ICDATAPTR));
                    Gp exitR = cc.new_gp32("iv_dz_exit");
                    cc.mov(exitR, Imm(EXIT_SEND));
                    cc.str(exitR, ptr(state, OFF_EXIT));
                    cc.ret();
                };

                // Tag check start.  Both start and stop must be SmI
                // for our SmI-only loop.
                Gp tagS = cc.new_gp64("iv_tagS");
                cc.and_(tagS, startReg, Imm(7));
                cc.cmp(tagS, Imm(1));
                Label sOk = cc.new_label();
                cc.b_eq(sOk);
                emitDeopt(startReg, stopReg, deoptBC);
                cc.bind(sOk);
                Gp tagE = cc.new_gp64("iv_tagE");
                cc.and_(tagE, stopReg, Imm(7));
                cc.cmp(tagE, Imm(1));
                Label eOk = cc.new_label();
                cc.b_eq(eOk);
                emitDeopt(startReg, stopReg, deoptBC);
                cc.bind(eOk);

                // iReg = start (as SmI Oop).
                Gp iReg = cc.new_gp64("iv_i");
                cc.mov(iReg, startReg);

                // Rotated loop: pre-check, fused back-edge.
                // SmI Oops with tag bit 0 set; underlying-integer
                // ordering is preserved by raw bit comparison for
                // non-negative SmIs (the common `(1 to: N)` pattern;
                // tag-check at loop entry rejects non-SmI bounds).
                Label loopHead = cc.new_label();
                Label loopExit = cc.new_label();
                cc.cmp(iReg, stopReg);
                cc.b_hi(loopExit);
                cc.bind(loopHead);

                // Inline block body.  temp 0 = acc → accReg,
                // temp 1 = i (the SmI Oop) → iReg.
                std::unordered_map<uint32_t, Gp> blockRegs;
                bool sawReturn = false;
                for (const auto& bv : blockIR.values) {
                    switch (bv.op) {
                    case Op::kLoadTemp: {
                        uint32_t idx = (uint32_t)bv.literal;
                        if (idx == 0) {
                            blockRegs[bv.id] = accReg;
                        } else if (idx == 1) {
                            blockRegs[bv.id] = iReg;
                        } else {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        break;
                    }
                    case Op::kLoadTrueOop: {
                        Gp r = cc.new_gp64("iv_true");
                        cc.ldr(r, ptr(state, OFF_TRUEOOP));
                        blockRegs[bv.id] = r;
                        break;
                    }
                    case Op::kLoadFalseOop: {
                        Gp r = cc.new_gp64("iv_false");
                        cc.ldr(r, ptr(state, OFF_FALSEOOP));
                        blockRegs[bv.id] = r;
                        break;
                    }
                    case Op::kConstantOop: {
                        Gp r = cc.new_gp64("iv_const");
                        cc.mov(r, Imm(bv.literal));
                        blockRegs[bv.id] = r;
                        break;
                    }
                    case Op::kLoadLiteral: {
                        uint32_t idx = (uint32_t)bv.literal;
                        Gp r = cc.new_gp64("iv_lit");
                        cc.ldr(r, ptr(litBaseHoisted, (int)(idx * 8)));
                        blockRegs[bv.id] = r;
                        break;
                    }
                    case Op::kLoadReceiver: {
                        Gp r = cc.new_gp64("iv_recv");
                        cc.ldr(r, ptr(state, OFF_RECEIVER));
                        blockRegs[bv.id] = r;
                        break;
                    }
                    case Op::kLoadInstVar: {
                        if (bv.operands.empty()) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        auto it = blockRegs.find(bv.operands[0]);
                        if (it == blockRegs.end()) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        Gp dst = cc.new_gp64("iv_ivar");
                        cc.ldr(dst, ptr(it->second,
                                         8 + static_cast<int>(bv.literal) * 8));
                        blockRegs[bv.id] = dst;
                        break;
                    }
                    case Op::kLoadTempInVec: {
                        // Read slot from captured TempVector.
                        // Pre-pass verified ivInjectHasCapture and
                        // vec idx == 1.
                        if (!ivInjectHasCapture) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        uint32_t slot =
                            (uint32_t)(bv.literal & 0xFFFFFFFFu);
                        Gp dst = cc.new_gp64("iv_vec");
                        cc.ldr(dst, ptr(ivInjectVecReg,
                                         8 + static_cast<int>(slot) * 8));
                        blockRegs[bv.id] = dst;
                        break;
                    }
                    case Op::kPrimTagCheckInt: {
                        if (bv.operands.empty()) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        auto it = blockRegs.find(bv.operands[0]);
                        if (it == blockRegs.end()) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        blockRegs[bv.id] = it->second;
                        break;
                    }
                    case Op::kPrimAddInt:
                    case Op::kPrimSubInt:
                    case Op::kPrimMulInt: {
                        // For Interval-inject we KNOW both operands are
                        // SmI: accReg starts as init (tag-checked at
                        // loop entry) and is reassigned only from
                        // SmI-arith results; iReg is SmI by
                        // construction (loop-entry tag check + add 8).
                        // Skip the redundant per-iteration tag check.
                        if (bv.operands.size() < 2) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        auto ita = blockRegs.find(bv.operands[0]);
                        auto itb = blockRegs.find(bv.operands[1]);
                        if (ita == blockRegs.end()
                            || itb == blockRegs.end()) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        Gp dst = cc.new_gp64("iv_arith");
                        if (bv.op == Op::kPrimAddInt) {
                            cc.add(dst, ita->second, itb->second);
                            cc.sub(dst, dst, Imm(1));
                        } else if (bv.op == Op::kPrimSubInt) {
                            cc.sub(dst, ita->second, itb->second);
                            cc.add(dst, dst, Imm(1));
                        } else {
                            Gp au = cc.new_gp64("iv_au");
                            Gp bu = cc.new_gp64("iv_bu");
                            cc.asr(au, ita->second, Imm(3));
                            cc.asr(bu, itb->second, Imm(3));
                            Gp prod = cc.new_gp64("iv_prod");
                            cc.mul(prod, au, bu);
                            cc.lsl(dst, prod, Imm(3));
                            cc.orr(dst, dst, Imm(1));
                        }
                        blockRegs[bv.id] = dst;
                        break;
                    }
                    case Op::kPrimLtInt:
                    case Op::kPrimLeInt:
                    case Op::kPrimGtInt:
                    case Op::kPrimGeInt:
                    case Op::kPrimEqInt:
                    case Op::kPrimNeqInt: {
                        // For Interval-inject we KNOW both operands are
                        // SmI (see kPrimAdd reasoning above).  Skip
                        // per-iter tag check.  Boolean result becomes
                        // the new acc if used by kReturn.
                        if (bv.operands.size() < 2) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        auto ita = blockRegs.find(bv.operands[0]);
                        auto itb = blockRegs.find(bv.operands[1]);
                        if (ita == blockRegs.end()
                            || itb == blockRegs.end()) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        Gp trueOop = cc.new_gp64("iv_cmpTrue");
                        Gp falseOop = cc.new_gp64("iv_cmpFalse");
                        cc.ldr(trueOop, ptr(state, OFF_TRUEOOP));
                        cc.ldr(falseOop, ptr(state, OFF_FALSEOOP));
                        cc.cmp(ita->second, itb->second);
                        Gp dst = cc.new_gp64("iv_cmp");
                        CondCode cond;
                        switch (bv.op) {
                          case Op::kPrimLtInt:  cond = CondCode::kLT; break;
                          case Op::kPrimLeInt:  cond = CondCode::kLE; break;
                          case Op::kPrimGtInt:  cond = CondCode::kGT; break;
                          case Op::kPrimGeInt:  cond = CondCode::kGE; break;
                          case Op::kPrimEqInt:  cond = CondCode::kEQ; break;
                          case Op::kPrimNeqInt: cond = CondCode::kNE; break;
                          default:              cond = CondCode::kEQ; break;
                        }
                        cc.csel(dst, trueOop, falseOop, cond);
                        blockRegs[bv.id] = dst;
                        break;
                    }
                    case Op::kPrimIdentityEq:
                    case Op::kPrimIdentityNeq: {
                        // Identity: no tag check, semantics universal.
                        if (bv.operands.size() != 2) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        auto ita = blockRegs.find(bv.operands[0]);
                        auto itb = blockRegs.find(bv.operands[1]);
                        if (ita == blockRegs.end()
                            || itb == blockRegs.end()) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        Gp trueOop = cc.new_gp64("iv_idTrue");
                        Gp falseOop = cc.new_gp64("iv_idFalse");
                        cc.ldr(trueOop, ptr(state, OFF_TRUEOOP));
                        cc.ldr(falseOop, ptr(state, OFF_FALSEOOP));
                        cc.cmp(ita->second, itb->second);
                        Gp dst = cc.new_gp64("iv_idcmp");
                        cc.csel(dst, trueOop, falseOop,
                                bv.op == Op::kPrimIdentityEq
                                    ? CondCode::kEQ
                                    : CondCode::kNE);
                        blockRegs[bv.id] = dst;
                        break;
                    }
                    case Op::kReturn: {
                        if (bv.operands.size() != 1) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        auto it = blockRegs.find(bv.operands[0]);
                        if (it == blockRegs.end()) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        cc.mov(accReg, it->second);
                        sawReturn = true;
                        break;
                    }
                    default:
                        if (failedAtValue) *failedAtValue = v.id;
                        return nullptr;
                    }
                }

                if (!sawReturn) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }

                // i += SmI(1) = i + 8; fused cmp+b.ls back-edge.
                cc.add(iReg, iReg, Imm(8));
                cc.cmp(iReg, stopReg);
                cc.b_ls(loopHead);

                cc.bind(loopExit);

                regFor[v.id] = accReg;
                break;
            }
            case Op::kCountedLoopIntervalDo: {
                // (start to: stop) do: [block].  Iterates i from start
                // to stop with no Interval allocation, no per-iteration
                // sends.  Block has 1 arg `e` → iReg (the SmI Oop).
                using namespace asmjit::a64;
                if (v.operands.size() != 2) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                auto itStart = regFor.find(v.operands[0]);
                auto itStop  = regFor.find(v.operands[1]);
                if (itStart == regFor.end()
                    || itStop == regFor.end()) {
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
                if (blockIR.blocks.size() != 1) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }

                // Block-body whitelist mirrors kCountedLoopIntervalInjectInto.
                for (const auto& bv : blockIR.values) {
                    switch (bv.op) {
                    case Op::kLoadTemp:
                    case Op::kLoadReceiver:
                    case Op::kLoadInstVar:
                    case Op::kLoadTrueOop:
                    case Op::kLoadFalseOop:
                    case Op::kConstantOop:
                    case Op::kLoadLiteral:
                    case Op::kReturn:
                    case Op::kPrimTagCheckInt:
                    case Op::kPrimAddInt:
                    case Op::kPrimSubInt:
                    case Op::kPrimMulInt:
                        break;
                    default:
                        if (failedAtValue) *failedAtValue = v.id;
                        return nullptr;
                    }
                }

                Gp startReg = itStart->second;
                Gp stopReg  = itStop->second;

                uint32_t deoptBC = 0;
                for (const auto& fp : method.framepoints) {
                    if (fp.valueId == v.id) {
                        deoptBC = fp.bcOffset;
                        break;
                    }
                }

                // Deopt: rebuild interp stack [start, stop] and resume
                // at the to: bytecode (which re-runs the to: send
                // → real Interval allocation → real do: dispatch).
                auto emitDeopt = [&](Gp savedStart, Gp savedStop,
                                      uint32_t bc) {
                    Gp sp = cc.new_gp64("ivd_dz_sp");
                    cc.ldr(sp, ptr(state, OFF_SP));
                    cc.str(savedStart, ptr(sp));
                    cc.str(savedStop, ptr(sp, 8));
                    cc.add(sp, sp, Imm(16));
                    cc.str(sp, ptr(state, OFF_SP));
                    Gp ipReg = cc.new_gp64("ivd_dz_ip");
                    if (bytecodeBase) {
                        uintptr_t addr =
                            reinterpret_cast<uintptr_t>(bytecodeBase) + bc;
                        cc.mov(ipReg, Imm((uint64_t)addr));
                    } else {
                        cc.mov(ipReg, Imm((uint64_t)bc));
                    }
                    cc.str(ipReg, ptr(state, OFF_IP));
                    Gp argc = cc.new_gp32("ivd_dz_argc");
                    cc.mov(argc, Imm(0));
                    cc.str(argc, ptr(state, OFF_SENDARGCOUNT));
                    Gp z64 = cc.new_gp64("ivd_dz_zero");
                    cc.mov(z64, Imm(0));
                    cc.str(z64, ptr(state, OFF_ICDATAPTR));
                    Gp exitR = cc.new_gp32("ivd_dz_exit");
                    cc.mov(exitR, Imm(EXIT_SEND));
                    cc.str(exitR, ptr(state, OFF_EXIT));
                    cc.ret();
                };

                // Tag-check both bounds at loop entry.
                Gp tagS = cc.new_gp64("ivd_tagS");
                cc.and_(tagS, startReg, Imm(7));
                cc.cmp(tagS, Imm(1));
                Label sOk = cc.new_label();
                cc.b_eq(sOk);
                emitDeopt(startReg, stopReg, deoptBC);
                cc.bind(sOk);
                Gp tagE = cc.new_gp64("ivd_tagE");
                cc.and_(tagE, stopReg, Imm(7));
                cc.cmp(tagE, Imm(1));
                Label eOk = cc.new_label();
                cc.b_eq(eOk);
                emitDeopt(startReg, stopReg, deoptBC);
                cc.bind(eOk);

                Gp iReg = cc.new_gp64("ivd_i");
                cc.mov(iReg, startReg);

                // Rotated loop: pre-check, fused back-edge.
                Label loopHead = cc.new_label();
                Label loopExit = cc.new_label();
                cc.cmp(iReg, stopReg);
                cc.b_hi(loopExit);
                cc.bind(loopHead);

                // Inline block body.  temp 0 = each → iReg.
                std::unordered_map<uint32_t, Gp> blockRegs;
                for (const auto& bv : blockIR.values) {
                    switch (bv.op) {
                    case Op::kLoadTemp: {
                        uint32_t idx = (uint32_t)bv.literal;
                        if (idx == 0) {
                            blockRegs[bv.id] = iReg;
                        } else {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        break;
                    }
                    case Op::kLoadTrueOop: {
                        Gp r = cc.new_gp64("ivd_true");
                        cc.ldr(r, ptr(state, OFF_TRUEOOP));
                        blockRegs[bv.id] = r;
                        break;
                    }
                    case Op::kLoadFalseOop: {
                        Gp r = cc.new_gp64("ivd_false");
                        cc.ldr(r, ptr(state, OFF_FALSEOOP));
                        blockRegs[bv.id] = r;
                        break;
                    }
                    case Op::kConstantOop: {
                        Gp r = cc.new_gp64("ivd_const");
                        cc.mov(r, Imm(bv.literal));
                        blockRegs[bv.id] = r;
                        break;
                    }
                    case Op::kLoadLiteral: {
                        uint32_t idx = (uint32_t)bv.literal;
                        Gp r = cc.new_gp64("ivd_lit");
                        cc.ldr(r, ptr(litBaseHoisted, (int)(idx * 8)));
                        blockRegs[bv.id] = r;
                        break;
                    }
                    case Op::kLoadReceiver: {
                        Gp r = cc.new_gp64("ivd_recv");
                        cc.ldr(r, ptr(state, OFF_RECEIVER));
                        blockRegs[bv.id] = r;
                        break;
                    }
                    case Op::kLoadInstVar: {
                        if (bv.operands.empty()) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        auto it = blockRegs.find(bv.operands[0]);
                        if (it == blockRegs.end()) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        Gp dst = cc.new_gp64("ivd_ivar");
                        cc.ldr(dst, ptr(it->second,
                                         8 + static_cast<int>(bv.literal) * 8));
                        blockRegs[bv.id] = dst;
                        break;
                    }
                    case Op::kPrimTagCheckInt: {
                        if (bv.operands.empty()) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        auto it = blockRegs.find(bv.operands[0]);
                        if (it == blockRegs.end()) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        blockRegs[bv.id] = it->second;
                        break;
                    }
                    case Op::kPrimAddInt:
                    case Op::kPrimSubInt:
                    case Op::kPrimMulInt: {
                        if (bv.operands.size() < 2) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        auto ita = blockRegs.find(bv.operands[0]);
                        auto itb = blockRegs.find(bv.operands[1]);
                        if (ita == blockRegs.end()
                            || itb == blockRegs.end()) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        // iReg is SmI by construction; literal/receiver
                        // operands aren't proven SmI but they're rare
                        // — emit a tag check to be safe.
                        Gp tagA = cc.new_gp64("ivd_tagA");
                        cc.and_(tagA, ita->second, Imm(7));
                        cc.cmp(tagA, Imm(1));
                        Label tagAok = cc.new_label();
                        cc.b_eq(tagAok);
                        emitDeopt(startReg, stopReg, deoptBC);
                        cc.bind(tagAok);
                        Gp tagB = cc.new_gp64("ivd_tagB");
                        cc.and_(tagB, itb->second, Imm(7));
                        cc.cmp(tagB, Imm(1));
                        Label tagBok = cc.new_label();
                        cc.b_eq(tagBok);
                        emitDeopt(startReg, stopReg, deoptBC);
                        cc.bind(tagBok);

                        Gp dst = cc.new_gp64("ivd_arith");
                        if (bv.op == Op::kPrimAddInt) {
                            cc.add(dst, ita->second, itb->second);
                            cc.sub(dst, dst, Imm(1));
                        } else if (bv.op == Op::kPrimSubInt) {
                            cc.sub(dst, ita->second, itb->second);
                            cc.add(dst, dst, Imm(1));
                        } else {
                            Gp au = cc.new_gp64("ivd_au");
                            Gp bu = cc.new_gp64("ivd_bu");
                            cc.asr(au, ita->second, Imm(3));
                            cc.asr(bu, itb->second, Imm(3));
                            Gp prod = cc.new_gp64("ivd_prod");
                            cc.mul(prod, au, bu);
                            cc.lsl(dst, prod, Imm(3));
                            cc.orr(dst, dst, Imm(1));
                        }
                        blockRegs[bv.id] = dst;
                        break;
                    }
                    case Op::kReturn:
                        // do: discards block result.
                        break;
                    default:
                        if (failedAtValue) *failedAtValue = v.id;
                        return nullptr;
                    }
                }

                // i += 8 (SmI(1) tag-preserving); fused cmp+b.ls back-edge.
                cc.add(iReg, iReg, Imm(8));
                cc.cmp(iReg, stopReg);
                cc.b_ls(loopHead);
                cc.bind(loopExit);

                // Result placeholder: startReg.  do:'s caller usually
                // discards.  If observed as an Interval, downstream
                // operations on a SmI start value will deopt at their
                // own bytecodes.
                regFor[v.id] = startReg;
                break;
            }
            case Op::kCountedLoopArrayDoAccum: {
                // arr do: [:e | s := s + e] — the dominant Pharo
                // accumulator idiom.  Block has numCopied=1 (TempVec
                // for s); we recognized the exact 5-instruction shape
                // at bytecode level and packed metadata in v.literal.
                //
                // operands[0] = rcv (the array)
                // operands[1] = vecRef (the TempVector Oop)
                // literal     = slot|arithCode|outerVecTemp packed
                using namespace asmjit::a64;
                if (v.operands.size() != 2) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                auto itRcv = regFor.find(v.operands[0]);
                auto itVec = regFor.find(v.operands[1]);
                if (itRcv == regFor.end() || itVec == regFor.end()) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                Gp rcvReg = itRcv->second;
                Gp vecReg = itVec->second;

                uint32_t slot      = (uint32_t)(v.literal & 0xFF);
                uint32_t arithCode = (uint32_t)((v.literal >> 8) & 0xFF);
                int slotByteOff = 8 + (int)slot * 8;

                uint32_t deoptBC = 0;
                for (const auto& fp : method.framepoints) {
                    if (fp.valueId == v.id) {
                        deoptBC = fp.bcOffset;
                        break;
                    }
                }

                // Deopt: rebuild interp stack [rcv, vecRef] and resume
                // at PushFullBlock's offset.  Interpreter creates the
                // block (PushFullBlock) and re-dispatches do:.  Since
                // we never wrote accReg back to vec[slot], the original
                // s value is preserved → re-running do: yields correct
                // accumulation from scratch.
                auto emitDeopt = [&](Gp savedRcv, Gp savedVec,
                                      uint32_t bc) {
                    Gp sp = cc.new_gp64("acc_dz_sp");
                    cc.ldr(sp, ptr(state, OFF_SP));
                    cc.str(savedRcv, ptr(sp));
                    cc.str(savedVec, ptr(sp, 8));
                    cc.add(sp, sp, Imm(16));
                    cc.str(sp, ptr(state, OFF_SP));
                    Gp ipReg = cc.new_gp64("acc_dz_ip");
                    if (bytecodeBase) {
                        uintptr_t addr =
                            reinterpret_cast<uintptr_t>(bytecodeBase) + bc;
                        cc.mov(ipReg, Imm((uint64_t)addr));
                    } else {
                        cc.mov(ipReg, Imm((uint64_t)bc));
                    }
                    cc.str(ipReg, ptr(state, OFF_IP));
                    Gp argc = cc.new_gp32("acc_dz_argc");
                    cc.mov(argc, Imm(0));
                    cc.str(argc, ptr(state, OFF_SENDARGCOUNT));
                    Gp z64 = cc.new_gp64("acc_dz_zero");
                    cc.mov(z64, Imm(0));
                    cc.str(z64, ptr(state, OFF_ICDATAPTR));
                    Gp exitR = cc.new_gp32("acc_dz_exit");
                    cc.mov(exitR, Imm(EXIT_SEND));
                    cc.str(exitR, ptr(state, OFF_EXIT));
                    cc.ret();
                };

                // Tag-check vecRef must be heap object (low 3 bits = 0).
                Gp tagV = cc.new_gp64("acc_tagV");
                cc.and_(tagV, vecReg, Imm(7));
                cc.cmp(tagV, Imm(0));
                Label vOk = cc.new_label();
                cc.b_eq(vOk);
                emitDeopt(rcvReg, vecReg, deoptBC);
                cc.bind(vOk);

                // Load accReg = vecRef[slot].  Tag-check must be SmI
                // (so our SmI-arith fast path is safe).
                Gp accReg = cc.new_gp64("acc_acc");
                cc.ldr(accReg, ptr(vecReg, slotByteOff));
                Gp tagA = cc.new_gp64("acc_tagA");
                cc.and_(tagA, accReg, Imm(7));
                cc.cmp(tagA, Imm(1));
                Label aOk = cc.new_label();
                cc.b_eq(aOk);
                emitDeopt(rcvReg, vecReg, deoptBC);
                cc.bind(aOk);

                // sizeReg = basicSize(rcv).  Deopt on 0 (non-indexable).
                Gp sizeFn = cc.new_gp64("acc_szfn");
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
                Gp sizeReg = cc.new_gp64("acc_size");
                sizeInvoke->set_ret(0, sizeReg);

                Label sizeOk = cc.new_label();
                cc.cbnz(sizeReg, sizeOk);
                emitDeopt(rcvReg, vecReg, deoptBC);
                cc.bind(sizeOk);

                // Format check: only fast-path format 2 (regular Object
                // Array — Indexable64).  Other formats (byte arrays at
                // 16-23, DoubleWord at 9, IndexableWithFixed at 3-5) take
                // the deopt path; basic_at does element-encoding work
                // (chars from bytes, etc.) we can't replicate inline.
                // bits 24-28 of the 64-bit header carry format.
                Gp acc_hdr = cc.new_gp64("acc_hdr");
                cc.ldr(acc_hdr, ptr(rcvReg));
                Gp acc_fmt = cc.new_gp64("acc_fmt");
                cc.ubfx(acc_fmt, acc_hdr, Imm(24), Imm(5));
                cc.cmp(acc_fmt, Imm(2));
                Label fmtOk = cc.new_label();
                cc.b_eq(fmtOk);
                emitDeopt(rcvReg, vecReg, deoptBC);
                cc.bind(fmtOk);

                // Track byte-offset directly: offReg counts 8, 16, ...
                // sizeReg encodes (size<<3)|1; (sizeReg-1) is the inclusive
                // end byte offset (= size*8).  Saves a per-iter sub.
                Gp acc_off = cc.new_gp64("acc_off");
                cc.mov(acc_off, Imm(8));
                Gp acc_endOff = cc.new_gp64("acc_endOff");
                cc.sub(acc_endOff, sizeReg, Imm(1));

                // Rotated loop: pre-check (acc_off > acc_endOff means
                // size==0, already filtered by basic_size's 0-check, but
                // be defensive).
                Label loopHead = cc.new_label();
                Label loopExit = cc.new_label();
                cc.cmp(acc_off, acc_endOff);
                cc.b_hi(loopExit);
                cc.bind(loopHead);

                // Inline format-2 element load: ldr [rcv, off].
                Gp eachReg = cc.new_gp64("acc_each");
                cc.ldr(eachReg, ptr(rcvReg, acc_off));

                // Tag-check eachReg as SmI.  accReg is uncommitted to
                // memory, so deopt is safe (vec[slot] still holds s_0).
                // The check looks like dead-weight on all-SmI arrays
                // but actually pays for itself: removing it (verified
                // 2026-04-28) made sumArr 2× SLOWER.  Branch prediction
                // already absorbs the predicted-taken b.eq, and the
                // explicit cmp+branch lets asmjit's allocator and the
                // CPU's load-use scheduler hide the load latency.
                Gp tagE = cc.new_gp64("acc_tagE");
                cc.and_(tagE, eachReg, Imm(7));
                cc.cmp(tagE, Imm(1));
                Label eOk = cc.new_label();
                cc.b_eq(eOk);
                emitDeopt(rcvReg, vecReg, deoptBC);
                cc.bind(eOk);

                // accReg = accReg <arith> eachReg, tag-preserving SmI.
                if (arithCode == 0) {
                    cc.add(accReg, accReg, eachReg);
                    cc.sub(accReg, accReg, Imm(1));
                } else if (arithCode == 1) {
                    cc.sub(accReg, accReg, eachReg);
                    cc.add(accReg, accReg, Imm(1));
                } else {
                    Gp au = cc.new_gp64("acc_au");
                    Gp bu = cc.new_gp64("acc_bu");
                    cc.asr(au, accReg, Imm(3));
                    cc.asr(bu, eachReg, Imm(3));
                    Gp prod = cc.new_gp64("acc_prod");
                    cc.mul(prod, au, bu);
                    cc.lsl(accReg, prod, Imm(3));
                    cc.orr(accReg, accReg, Imm(1));
                }

                // off += 8; fused cmp+b.ls back-edge.
                cc.add(acc_off, acc_off, Imm(8));
                cc.cmp(acc_off, acc_endOff);
                cc.b_ls(loopHead);

                cc.bind(loopExit);

                // Successful exit: commit accReg back to vec[slot].
                // Old TempVec has long since exited the nursery; for
                // SmI accReg there's no GC barrier needed.
                cc.str(accReg, ptr(vecReg, slotByteOff));

                // do:'s result is the receiver.
                regFor[v.id] = rcvReg;
                break;
            }
            case Op::kCountedLoopIntervalDoAccum: {
                // (start to: stop) do: [:e | s := s + e] — closure-accum
                // over an Interval receiver.  Same shape as
                // kCountedLoopArrayDoAccum but iterates SmI integers
                // start..stop directly (no basicAt).
                //
                // operands[0] = start (SmI Oop)
                // operands[1] = stop  (SmI Oop)
                // operands[2] = vecRef (TempVector Oop)
                // literal     = slot|arithCode|... packed (same layout)
                using namespace asmjit::a64;
                if (v.operands.size() != 3) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                auto itStart = regFor.find(v.operands[0]);
                auto itStop  = regFor.find(v.operands[1]);
                auto itVec   = regFor.find(v.operands[2]);
                if (itStart == regFor.end() || itStop == regFor.end()
                    || itVec == regFor.end()) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                Gp startReg = itStart->second;
                Gp stopReg  = itStop->second;
                Gp vecReg   = itVec->second;

                uint32_t slot      = (uint32_t)(v.literal & 0xFF);
                uint32_t arithCode = (uint32_t)((v.literal >> 8) & 0xFF);
                int slotByteOff = 8 + (int)slot * 8;

                uint32_t deoptBC = 0;
                for (const auto& fp : method.framepoints) {
                    if (fp.valueId == v.id) {
                        deoptBC = fp.bcOffset;
                        break;
                    }
                }

                // Deopt: rebuild interp stack [start, stop] and resume
                // at the to: bytecode (framepoint stores to:'s offset
                // = PushFullBlock - 2).  The interpreter re-runs to:
                // → real Interval allocation → PushTemp T_vec →
                // PushFullBlock → SpecialSend do: from scratch.
                // vec[slot] still holds original s (we never wrote
                // accReg back), so re-running do: yields the right sum.
                auto emitDeopt = [&](Gp savedStart, Gp savedStop,
                                      Gp /*savedVec*/, uint32_t bc) {
                    Gp sp = cc.new_gp64("ivacc_dz_sp");
                    cc.ldr(sp, ptr(state, OFF_SP));
                    cc.str(savedStart, ptr(sp));
                    cc.str(savedStop,  ptr(sp, 8));
                    cc.add(sp, sp, Imm(16));
                    cc.str(sp, ptr(state, OFF_SP));
                    Gp ipReg = cc.new_gp64("ivacc_dz_ip");
                    if (bytecodeBase) {
                        uintptr_t addr =
                            reinterpret_cast<uintptr_t>(bytecodeBase) + bc;
                        cc.mov(ipReg, Imm((uint64_t)addr));
                    } else {
                        cc.mov(ipReg, Imm((uint64_t)bc));
                    }
                    cc.str(ipReg, ptr(state, OFF_IP));
                    Gp argc = cc.new_gp32("ivacc_dz_argc");
                    cc.mov(argc, Imm(0));
                    cc.str(argc, ptr(state, OFF_SENDARGCOUNT));
                    Gp z64 = cc.new_gp64("ivacc_dz_zero");
                    cc.mov(z64, Imm(0));
                    cc.str(z64, ptr(state, OFF_ICDATAPTR));
                    Gp exitR = cc.new_gp32("ivacc_dz_exit");
                    cc.mov(exitR, Imm(EXIT_SEND));
                    cc.str(exitR, ptr(state, OFF_EXIT));
                    cc.ret();
                };

                // Tag-check: start, stop must be SmI; vecRef must be heap.
                Gp tag = cc.new_gp64("ivacc_tag");
                cc.and_(tag, startReg, Imm(7));
                cc.cmp(tag, Imm(1));
                Label sOk = cc.new_label();
                cc.b_eq(sOk);
                emitDeopt(startReg, stopReg, vecReg, deoptBC);
                cc.bind(sOk);
                cc.and_(tag, stopReg, Imm(7));
                cc.cmp(tag, Imm(1));
                Label tOk = cc.new_label();
                cc.b_eq(tOk);
                emitDeopt(startReg, stopReg, vecReg, deoptBC);
                cc.bind(tOk);
                cc.and_(tag, vecReg, Imm(7));
                cc.cmp(tag, Imm(0));
                Label vOk = cc.new_label();
                cc.b_eq(vOk);
                emitDeopt(startReg, stopReg, vecReg, deoptBC);
                cc.bind(vOk);

                // accReg = vecRef[slot]; tag-check SmI.
                Gp accReg = cc.new_gp64("ivacc_acc");
                cc.ldr(accReg, ptr(vecReg, slotByteOff));
                cc.and_(tag, accReg, Imm(7));
                cc.cmp(tag, Imm(1));
                Label aOk = cc.new_label();
                cc.b_eq(aOk);
                emitDeopt(startReg, stopReg, vecReg, deoptBC);
                cc.bind(aOk);

                // iReg = startReg.
                Gp iReg = cc.new_gp64("ivacc_i");
                cc.mov(iReg, startReg);

                // Rotated loop: pre-check skips body when start > stop;
                // back edge fuses cmp+branch so the hot path has one
                // conditional branch per iter instead of cond+uncond.
                Label loopHead = cc.new_label();
                Label loopExit = cc.new_label();
                cc.cmp(iReg, stopReg);
                cc.b_hi(loopExit);
                cc.bind(loopHead);

                // accReg = accReg <arith> iReg, tag-preserving SmI.
                if (arithCode == 0) {
                    cc.add(accReg, accReg, iReg);
                    cc.sub(accReg, accReg, Imm(1));
                } else if (arithCode == 1) {
                    cc.sub(accReg, accReg, iReg);
                    cc.add(accReg, accReg, Imm(1));
                } else {
                    Gp au = cc.new_gp64("ivacc_au");
                    Gp bu = cc.new_gp64("ivacc_bu");
                    cc.asr(au, accReg, Imm(3));
                    cc.asr(bu, iReg, Imm(3));
                    Gp prod = cc.new_gp64("ivacc_prod");
                    cc.mul(prod, au, bu);
                    cc.lsl(accReg, prod, Imm(3));
                    cc.orr(accReg, accReg, Imm(1));
                }

                // i += SmI(1).  SmI(1) = (1<<3)|1 = 9; +1 increment in
                // the integer field is +8 in the encoded Oop (preserves tag).
                cc.add(iReg, iReg, Imm(8));
                cc.cmp(iReg, stopReg);
                cc.b_ls(loopHead);

                cc.bind(loopExit);

                // Commit accReg to vec[slot].  SmI: no GC barrier needed.
                cc.str(accReg, ptr(vecReg, slotByteOff));

                // Result is a placeholder — do: returns the Interval but
                // we don't materialize it.  The IV-do-accum splice's
                // pre-pass guarantees the next bc discards the result.
                regFor[v.id] = startReg;
                break;
            }
            case Op::kCountedLoopArrayCollect: {
                // arr collect: [:e | expr] — allocate a fresh Array
                // of the receiver's size, iterate i = 1..size loading
                // arr[i], apply the inlined block body to produce the
                // new element, store into result[i].  Returns result.
                //
                // operands[0] = rcv (the source Array)
                // operands[1] = vecRef (captured TempVector, only when
                //                       block has numCopied=1)
                // literal     = block-IR slot
                using namespace asmjit::a64;
                if (v.operands.size() != 1 && v.operands.size() != 2) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                auto itRcv = regFor.find(v.operands[0]);
                if (itRcv == regFor.end()) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                Gp rcvReg = itRcv->second;
                bool collectHasCapture = (v.operands.size() == 2);
                Gp collectVecReg;
                if (collectHasCapture) {
                    auto itVec = regFor.find(v.operands[1]);
                    if (itVec == regFor.end()) {
                        if (failedAtValue) *failedAtValue = v.id;
                        return nullptr;
                    }
                    collectVecReg = itVec->second;
                }

                uint32_t blockSlot = (uint32_t)v.literal;
                if (blockSlot >= method.inlinedBlocks.size()
                    || !method.inlinedBlocks[blockSlot]) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                const Method& blockIR = *method.inlinedBlocks[blockSlot];
                if (blockIR.blocks.size() != 1) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }

                uint32_t deoptBC = 0;
                for (const auto& fp : method.framepoints) {
                    if (fp.valueId == v.id) {
                        deoptBC = fp.bcOffset;
                        break;
                    }
                }

                // Deopt: rebuild interp stack [arr, FullBlock] and
                // resume at PushFullBlock's offset.  Interpreter creates
                // the block (PushFullBlock) and dispatches Send1
                // #collect:, which re-allocates a fresh result and
                // populates it from scratch.  The partially-filled
                // result Array allocated by the splice is GC garbage.
                //
                // Note: PushFullBlock takes a literal-block oop and
                // creates the FullBlockClosure.  We push savedRcv
                // [arr] only; the interpreter will re-execute
                // PushFullBlock to push the FullBlock.
                auto emitDeopt = [&](Gp savedRcv, uint32_t bc) {
                    Gp sp = cc.new_gp64("col_dz_sp");
                    cc.ldr(sp, ptr(state, OFF_SP));
                    cc.str(savedRcv, ptr(sp));
                    cc.add(sp, sp, Imm(8));
                    cc.str(sp, ptr(state, OFF_SP));
                    Gp ipReg = cc.new_gp64("col_dz_ip");
                    if (bytecodeBase) {
                        uintptr_t addr =
                            reinterpret_cast<uintptr_t>(bytecodeBase) + bc;
                        cc.mov(ipReg, Imm((uint64_t)addr));
                    } else {
                        cc.mov(ipReg, Imm((uint64_t)bc));
                    }
                    cc.str(ipReg, ptr(state, OFF_IP));
                    Gp argc = cc.new_gp32("col_dz_argc");
                    cc.mov(argc, Imm(0));
                    cc.str(argc, ptr(state, OFF_SENDARGCOUNT));
                    Gp z64 = cc.new_gp64("col_dz_zero");
                    cc.mov(z64, Imm(0));
                    cc.str(z64, ptr(state, OFF_ICDATAPTR));
                    Gp exitR = cc.new_gp32("col_dz_exit");
                    cc.mov(exitR, Imm(EXIT_SEND));
                    cc.str(exitR, ptr(state, OFF_EXIT));
                    cc.ret();
                };

                // Tag-check rcv: must be heap object.
                Gp tagR = cc.new_gp64("col_tagR");
                cc.and_(tagR, rcvReg, Imm(7));
                cc.cmp(tagR, Imm(0));
                Label rOk = cc.new_label();
                cc.b_eq(rOk);
                emitDeopt(rcvReg, deoptBC);
                cc.bind(rOk);

                // sizeReg = jit_rt_sista_basic_size(state, rcv);
                // returns 0 on non-indexable receiver → deopt.
                Gp sizeFn = cc.new_gp64("col_szfn");
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
                Gp sizeReg = cc.new_gp64("col_size");
                sizeInvoke->set_ret(0, sizeReg);

                // sizeReg is SmI Oop encoding.  basic_size returns 0 (raw)
                // on miss — deopt.  Otherwise convert to int (sizeInt).
                Label sizeOk = cc.new_label();
                cc.cbnz(sizeReg, sizeOk);
                emitDeopt(rcvReg, deoptBC);
                cc.bind(sizeOk);
                // sizeInt = sizeReg >> 3
                Gp sizeInt = cc.new_gp64("col_sizeInt");
                cc.asr(sizeInt, sizeReg, Imm(3));

                // Allocate result Array via jit_rt_sista_alloc_array.
                // The helper accepts any size up to 0x100000 (1M slots);
                // larger sizes deopt to the interpreter to handle.
                Label allocOk = cc.new_label();
                Gp tmp = cc.new_gp64("col_tmp");
                cc.mov(tmp, Imm(0x100000));
                cc.cmp(sizeInt, tmp);
                cc.b_le(allocOk);
                emitDeopt(rcvReg, deoptBC);
                cc.bind(allocOk);

                Gp allocFn = cc.new_gp64("col_allocFn");
                cc.mov(allocFn,
                       Imm((uint64_t)&jit_rt_sista_alloc_array));
                asmjit::InvokeNode* allocInvoke = nullptr;
                asmjit::Error aErr = cc.invoke(
                    asmjit::Out(allocInvoke), allocFn,
                    asmjit::FuncSignature::build<
                        uint64_t, void*, uint64_t>());
                if (aErr != asmjit::kErrorOk || !allocInvoke) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }
                allocInvoke->set_arg(0, state);
                allocInvoke->set_arg(1, sizeInt);
                Gp resultReg = cc.new_gp64("col_result");
                allocInvoke->set_ret(0, resultReg);

                Label allocSucc = cc.new_label();
                cc.cbnz(resultReg, allocSucc);
                emitDeopt(rcvReg, deoptBC);
                cc.bind(allocSucc);

                // Format check: only fast-path format 2 (Indexable64).
                // Other formats deopt — the helper-based path knows how
                // to encode bytes/etc. but inline ldr can't.
                Gp col_hdr = cc.new_gp64("col_hdr");
                cc.ldr(col_hdr, ptr(rcvReg));
                Gp col_fmt = cc.new_gp64("col_fmt");
                cc.ubfx(col_fmt, col_hdr, Imm(24), Imm(5));
                cc.cmp(col_fmt, Imm(2));
                Label colFmtOk = cc.new_label();
                cc.b_eq(colFmtOk);
                emitDeopt(rcvReg, deoptBC);
                cc.bind(colFmtOk);

                // iReg = SmI(1) = 9.
                Gp iReg = cc.new_gp64("col_i");
                cc.mov(iReg, Imm(9));

                // Rotated loop: pre-check size; back edge fuses cmp+b.ls.
                Label loopHead = cc.new_label();
                Label loopExit = cc.new_label();
                cc.cmp(iReg, sizeReg);
                cc.b_hi(loopExit);
                cc.bind(loopHead);

                // Element-use analysis: only emit per-iter load +
                // SmI tag-check if the block actually references
                // the element via kLoadTemp(0).  Patterns like
                // `arr collect: [:e | <const>]` or
                // `[:e | self ivar0 + 1]` ignore the element — skip
                // the load entirely in that case.
                bool blockUsesElement = false;
                for (const auto& bv : blockIR.values) {
                    if (bv.op == Op::kLoadTemp && bv.literal == 0) {
                        blockUsesElement = true;
                        break;
                    }
                }

                // Inline format-2 element load.  iReg encodes (i<<3)|1;
                // slot (i-1) lives at byte offset 8 + (i-1)*8 = i*8 from
                // rcv.  i*8 = iReg - 1.  Use [rcv, off] addressing.
                Gp eachReg = cc.new_gp64("col_each");
                if (blockUsesElement) {
                    Gp col_off = cc.new_gp64("col_off");
                    cc.sub(col_off, iReg, Imm(1));
                    cc.ldr(eachReg, ptr(rcvReg, col_off));

                    // Tag-check eachReg as SmI (the splice's block is
                    // restricted to SmI arith).  Deopt before any commit
                    // so result Array is just GC garbage.
                    Gp tagE = cc.new_gp64("col_tagE");
                    cc.and_(tagE, eachReg, Imm(7));
                    cc.cmp(tagE, Imm(1));
                    Label eOk = cc.new_label();
                    cc.b_eq(eOk);
                    emitDeopt(rcvReg, deoptBC);
                    cc.bind(eOk);
                }
                // (when !blockUsesElement, eachReg is uninitialized
                //  but the block-body emission below never references
                //  it for that case — the kLoadTemp(0) case is what
                //  binds blockRegs[bv.id] = eachReg.)

                // Inline block body.  temp 0 = e (the array element)
                // → eachReg.  Block returns the new element value.
                std::unordered_map<uint32_t, Gp> blockRegs;
                bool sawReturn = false;
                Gp newElemReg;
                for (const auto& bv : blockIR.values) {
                    switch (bv.op) {
                    case Op::kLoadTemp: {
                        uint32_t idx = (uint32_t)bv.literal;
                        if (idx == 0) {
                            blockRegs[bv.id] = eachReg;
                        } else {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        break;
                    }
                    case Op::kLoadTrueOop: {
                        Gp r = cc.new_gp64("col_b_true");
                        cc.ldr(r, ptr(state, OFF_TRUEOOP));
                        blockRegs[bv.id] = r;
                        break;
                    }
                    case Op::kLoadFalseOop: {
                        Gp r = cc.new_gp64("col_b_false");
                        cc.ldr(r, ptr(state, OFF_FALSEOOP));
                        blockRegs[bv.id] = r;
                        break;
                    }
                    case Op::kConstantOop: {
                        Gp r = cc.new_gp64("col_b_const");
                        cc.mov(r, Imm(bv.literal));
                        blockRegs[bv.id] = r;
                        break;
                    }
                    case Op::kLoadLiteral: {
                        uint32_t idx = (uint32_t)bv.literal;
                        Gp r = cc.new_gp64("col_b_lit");
                        cc.ldr(r, ptr(litBaseHoisted, (int)(idx * 8)));
                        blockRegs[bv.id] = r;
                        break;
                    }
                    case Op::kLoadReceiver: {
                        Gp r = cc.new_gp64("col_b_recv");
                        cc.ldr(r, ptr(state, OFF_RECEIVER));
                        blockRegs[bv.id] = r;
                        break;
                    }
                    case Op::kLoadInstVar: {
                        // Side-effect-free read; result Array hasn't
                        // been populated past current i, so deopt is
                        // safe regardless of when the load fires.
                        if (bv.operands.empty()) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        auto it = blockRegs.find(bv.operands[0]);
                        if (it == blockRegs.end()) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        Gp dst = cc.new_gp64("col_b_ivar");
                        cc.ldr(dst, ptr(it->second,
                                         8 + static_cast<int>(bv.literal) * 8));
                        blockRegs[bv.id] = dst;
                        break;
                    }
                    case Op::kLoadTempInVec: {
                        // Read slot from captured TempVector.  Pre-pass
                        // verified collectHasCapture; vec idx (literal
                        // high 32) was checked == 1.  Slot is low 32.
                        if (!collectHasCapture) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        uint32_t slot =
                            (uint32_t)(bv.literal & 0xFFFFFFFFu);
                        Gp dst = cc.new_gp64("col_b_vec");
                        cc.ldr(dst, ptr(collectVecReg,
                                         8 + static_cast<int>(slot) * 8));
                        blockRegs[bv.id] = dst;
                        break;
                    }
                    case Op::kPrimTagCheckInt: {
                        if (bv.operands.empty()) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        auto it = blockRegs.find(bv.operands[0]);
                        if (it == blockRegs.end()) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        blockRegs[bv.id] = it->second;
                        break;
                    }
                    case Op::kPrimAddInt:
                    case Op::kPrimSubInt:
                    case Op::kPrimMulInt: {
                        if (bv.operands.size() < 2) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        auto ita = blockRegs.find(bv.operands[0]);
                        auto itb = blockRegs.find(bv.operands[1]);
                        if (ita == blockRegs.end()
                            || itb == blockRegs.end()) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        Gp dst = cc.new_gp64("col_b_arith");
                        if (bv.op == Op::kPrimAddInt) {
                            cc.add(dst, ita->second, itb->second);
                            cc.sub(dst, dst, Imm(1));
                        } else if (bv.op == Op::kPrimSubInt) {
                            cc.sub(dst, ita->second, itb->second);
                            cc.add(dst, dst, Imm(1));
                        } else {
                            Gp au = cc.new_gp64("col_b_au");
                            Gp bu = cc.new_gp64("col_b_bu");
                            cc.asr(au, ita->second, Imm(3));
                            cc.asr(bu, itb->second, Imm(3));
                            Gp prod = cc.new_gp64("col_b_prod");
                            cc.mul(prod, au, bu);
                            cc.lsl(dst, prod, Imm(3));
                            cc.orr(dst, dst, Imm(1));
                        }
                        blockRegs[bv.id] = dst;
                        break;
                    }
                    case Op::kPrimLtInt:
                    case Op::kPrimLeInt:
                    case Op::kPrimGtInt:
                    case Op::kPrimGeInt:
                    case Op::kPrimEqInt:
                    case Op::kPrimNeqInt: {
                        // SmallInt comparison.  Tag-preserving signed
                        // ordering (low 3 bits = tag are the same on
                        // both sides), so cmp + csel produces the
                        // correct boolean directly.
                        // Deopt on non-SmI either side: the result
                        // Array hasn't been written past the current
                        // i, so deopt rolls back cleanly to the
                        // collect: send.
                        if (bv.operands.size() < 2) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        auto ita = blockRegs.find(bv.operands[0]);
                        auto itb = blockRegs.find(bv.operands[1]);
                        if (ita == blockRegs.end()
                            || itb == blockRegs.end()) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        Gp tagA = cc.new_gp64("col_b_cmpTagA");
                        cc.and_(tagA, ita->second, Imm(7));
                        cc.cmp(tagA, Imm(1));
                        Label cmpAok = cc.new_label();
                        cc.b_eq(cmpAok);
                        emitDeopt(rcvReg, deoptBC);
                        cc.bind(cmpAok);
                        Gp tagB = cc.new_gp64("col_b_cmpTagB");
                        cc.and_(tagB, itb->second, Imm(7));
                        cc.cmp(tagB, Imm(1));
                        Label cmpBok = cc.new_label();
                        cc.b_eq(cmpBok);
                        emitDeopt(rcvReg, deoptBC);
                        cc.bind(cmpBok);

                        Gp trueOop = cc.new_gp64("col_b_true");
                        Gp falseOop = cc.new_gp64("col_b_false");
                        cc.ldr(trueOop, ptr(state, OFF_TRUEOOP));
                        cc.ldr(falseOop, ptr(state, OFF_FALSEOOP));
                        cc.cmp(ita->second, itb->second);
                        Gp dst = cc.new_gp64("col_b_cmp");
                        CondCode cond;
                        switch (bv.op) {
                          case Op::kPrimLtInt:  cond = CondCode::kLT; break;
                          case Op::kPrimLeInt:  cond = CondCode::kLE; break;
                          case Op::kPrimGtInt:  cond = CondCode::kGT; break;
                          case Op::kPrimGeInt:  cond = CondCode::kGE; break;
                          case Op::kPrimEqInt:  cond = CondCode::kEQ; break;
                          case Op::kPrimNeqInt: cond = CondCode::kNE; break;
                          default:              cond = CondCode::kEQ; break;
                        }
                        cc.csel(dst, trueOop, falseOop, cond);
                        blockRegs[bv.id] = dst;
                        break;
                    }
                    case Op::kPrimIdentityEq:
                    case Op::kPrimIdentityNeq: {
                        // Identity comparison.  No tag check, no deopt
                        // — semantics universal across all classes.
                        // Boolean result becomes the new element value.
                        if (bv.operands.size() != 2) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        auto ita = blockRegs.find(bv.operands[0]);
                        auto itb = blockRegs.find(bv.operands[1]);
                        if (ita == blockRegs.end()
                            || itb == blockRegs.end()) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        Gp trueOop = cc.new_gp64("col_b_idTrue");
                        Gp falseOop = cc.new_gp64("col_b_idFalse");
                        cc.ldr(trueOop, ptr(state, OFF_TRUEOOP));
                        cc.ldr(falseOop, ptr(state, OFF_FALSEOOP));
                        cc.cmp(ita->second, itb->second);
                        Gp dst = cc.new_gp64("col_b_idcmp");
                        cc.csel(dst, trueOop, falseOop,
                                bv.op == Op::kPrimIdentityEq
                                    ? CondCode::kEQ
                                    : CondCode::kNE);
                        blockRegs[bv.id] = dst;
                        break;
                    }
                    case Op::kReturn: {
                        if (bv.operands.size() != 1) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        auto it = blockRegs.find(bv.operands[0]);
                        if (it == blockRegs.end()) {
                            if (failedAtValue) *failedAtValue = v.id;
                            return nullptr;
                        }
                        newElemReg = it->second;
                        sawReturn = true;
                        break;
                    }
                    case Op::kPhi:
                        // Block has 1 block, no phi needed.
                        if (failedAtValue) *failedAtValue = v.id;
                        return nullptr;
                    default:
                        if (failedAtValue) *failedAtValue = v.id;
                        return nullptr;
                    }
                    if (sawReturn) break;
                }
                if (!sawReturn) {
                    if (failedAtValue) *failedAtValue = v.id;
                    return nullptr;
                }

                // Store newElemReg to result[iReg].  Use [result, off]
                // addressing — off = iReg - 1 already computed for the
                // load, but a fresh sub keeps register-allocator happy.
                Gp col_storeOff = cc.new_gp64("col_storeOff");
                cc.sub(col_storeOff, iReg, Imm(1));
                cc.str(newElemReg, ptr(resultReg, col_storeOff));

                // i += SmI(1); fused cmp+b.ls back-edge.
                cc.add(iReg, iReg, Imm(8));
                cc.cmp(iReg, sizeReg);
                cc.b_ls(loopHead);

                cc.bind(loopExit);

                // collect:'s result is the new Array.
                regFor[v.id] = resultReg;
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
                if (b.successors[0] != b.id + 1) {
                    cc.b(blockLabels[b.successors[0]]);
                }
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
                // Compare→branch fusion: if operand[0] is the prevVid
                // we marked above, the cmp flags are still set from
                // the kPrim<cmp>Int emit and we can branch on the
                // condition directly — no `cmp val, trueOop` round-trip.
                fillPhis(b, b.successors[0]);
                fillPhis(b, b.successors[1]);
                if (v.operands[0] == fusedCmpId) {
                    // successors[0] = taken, successors[1] = fallthrough.
                    // fusedCond was already adjusted for true/false in
                    // the pre-pass.
                    cc.b(fusedCond, blockLabels[b.successors[0]]);
                    // Skip the unconditional branch when the
                    // fallthrough successor is the very next block in
                    // emission order — saves one instruction per loop
                    // iteration in counted loops.
                    if (b.successors[1] != b.id + 1) {
                        cc.b(blockLabels[b.successors[1]]);
                    }
                    break;
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
                    if (b.successors[1] != b.id + 1) {
                        cc.b(blockLabels[b.successors[1]]);
                    }
                } else {
                    // BranchIfFalse: taken when cond != true.
                    // Simpler: branch to fallthrough when cond == true,
                    // else branch to taken.
                    cc.b_eq(blockLabels[b.successors[1]]);
                    if (b.successors[0] != b.id + 1) {
                        cc.b(blockLabels[b.successors[0]]);
                    }
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
