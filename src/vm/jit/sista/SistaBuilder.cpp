/*
 * SistaBuilder.cpp - Bytecode -> Sista IR lifter
 *
 * Implements the minimal subset documented in SistaBuilder.hpp.
 * The lifter walks bytecodes linearly, maintaining a simulated
 * operand stack of IR value ids.  Each bytecode pops operands,
 * emits IR, and pushes the result.
 *
 * For a trivial method there's exactly one basic block and no
 * branch resolution — that scaffolding lands later with jumps.
 */
#include "SistaBuilder.hpp"
#include "../SistaV1.hpp"
#include "../../ObjectMemory.hpp"

#include <map>
#include <set>
#include <vector>

namespace pharo {
namespace sista {

namespace {

// ---- Short-jump encoding ---------------------------------------------------
//
// Sista V1:  opcode 0xB0+N (uncond), 0xB8+N (if-true), 0xC0+N (if-false)
//            where N = 0..7.  Offset added to IP AFTER the byte is
//            consumed is (N + 1), per Interpreter.cpp line 1419.
//
// So at bytecode offset X with opcode B0+N, target absolute offset is:
//     X + 1 + (N + 1)  =  X + N + 2
static inline size_t shortJumpTarget(size_t bcOffset, uint8_t op) {
    return bcOffset + (op & 0x07) + 2;
}
static inline bool isShortUncondJump(uint8_t op) {
    return op >= jit::SistaV1::ShortJumpBase
        && op <= jit::SistaV1::ShortJumpLast;
}
static inline bool isShortJumpTrue(uint8_t op) {
    return op >= jit::SistaV1::ShortJumpTrueBase
        && op <= jit::SistaV1::ShortJumpTrueLast;
}
static inline bool isShortJumpFalse(uint8_t op) {
    return op >= jit::SistaV1::ShortJumpFalseBase
        && op <= jit::SistaV1::ShortJumpFalseLast;
}
static inline bool isShortJump(uint8_t op) {
    return isShortUncondJump(op) || isShortJumpTrue(op) || isShortJumpFalse(op);
}

// Return true if `op` is a terminator bytecode (in SistaV1).  Terminators
// end a basic block; the next bytecode starts a new block.
static inline bool isTerminatorBC(uint8_t op) {
    return op == jit::SistaV1::ReturnReceiver
        || op == jit::SistaV1::ReturnTrue
        || op == jit::SistaV1::ReturnFalse
        || op == jit::SistaV1::ReturnNil
        || op == jit::SistaV1::ReturnTop
        || isShortUncondJump(op);  // unconditional jumps end a block
}

class LinearLifter {
public:
    LinearLifter(const uint8_t* bc, size_t len,
                  uint32_t numArgs, uint32_t numTemps,
                  Method& out)
        : bc_(bc), len_(len), out_(out) {
        out_.numArgs  = numArgs;
        out_.numTemps = numTemps;
    }

    LiftResult run(uint32_t* failedAtBytecode) {
        // --- Pass 1: identify block boundaries --------------------------
        //
        // A block starts at:
        //   - offset 0 (method entry)
        //   - any branch target
        //   - the byte AFTER a terminator (so the else-branch /
        //     post-jump region is its own block)
        std::set<size_t> blockStarts;
        blockStarts.insert(0);

        for (size_t i = 0; i < len_;) {
            uint8_t op = bc_[i];
            if (isShortJump(op)) {
                size_t target = shortJumpTarget(i, op);
                if (target > len_) {
                    if (failedAtBytecode) *failedAtBytecode = (uint32_t)i;
                    return LiftResult::kMalformedMethod;
                }
                blockStarts.insert(target);
                if (i + 1 < len_) blockStarts.insert(i + 1);
                i++;
                continue;
            }
            if (isTerminatorBC(op) && i + 1 < len_) {
                blockStarts.insert(i + 1);
            }
            i++;
        }

        // --- Pass 2: create blocks in offset order ---------------------
        std::map<size_t, uint32_t> offsetToBlock;
        for (size_t offset : blockStarts) {
            offsetToBlock[offset] = out_.newBlock((int32_t)offset);
        }
        out_.entryBlock = offsetToBlock[0];

        // --- Pass 3: lift each block ---------------------------------
        //
        // Restriction for phase 2.1: the simulated stack must be empty
        // at every block boundary.  That's true for if-then-else where
        // each branch ends in a return; it's not for patterns where
        // control paths merge with values on the stack.  Phi-node
        // support comes later; for now we bail on stack-at-merge.
        for (auto& kv : offsetToBlock) {
            size_t blockStart = kv.first;
            uint32_t blockId  = kv.second;
            stack_.clear();
            currentBlock_ = blockId;

            LiftResult r = liftFromOffset(blockStart, offsetToBlock,
                                           failedAtBytecode);
            if (r != LiftResult::kOk) return r;
        }

        return LiftResult::kOk;
    }

private:
    LiftResult liftFromOffset(size_t startOffset,
                                const std::map<size_t, uint32_t>& offsetToBlock,
                                uint32_t* failedAtBytecode) {
        size_t ip = startOffset;
        while (ip < len_) {
            uint8_t op = bc_[ip];
            uint32_t bcOffset = static_cast<uint32_t>(ip);

            // Crossed into a new block?  Must be a fall-through.
            // Per the phase-2.1 restriction the simulated stack must
            // be empty at this point; otherwise we'd need phi nodes.
            if (ip != startOffset && offsetToBlock.count(ip)) {
                if (!stack_.empty()) {
                    if (failedAtBytecode) *failedAtBytecode = bcOffset;
                    return LiftResult::kUnsupportedBytecode;
                }
                uint32_t nextBlock = offsetToBlock.at(ip);
                out_.addEdge(currentBlock_, nextBlock);
                return LiftResult::kOk;
            }

            // Short jumps: unconditional and conditional.
            if (isShortJump(op)) {
                size_t target = shortJumpTarget(ip, op);
                auto tIt = offsetToBlock.find(target);
                if (tIt == offsetToBlock.end()) {
                    if (failedAtBytecode) *failedAtBytecode = bcOffset;
                    return LiftResult::kMalformedMethod;
                }
                if (isShortUncondJump(op)) {
                    if (!stack_.empty()) {
                        if (failedAtBytecode) *failedAtBytecode = bcOffset;
                        return LiftResult::kUnsupportedBytecode;
                    }
                    out_.newValue(currentBlock_, Op::kBranch, Type::kVoid,
                                   /*operands=*/{}, /*literal=*/tIt->second);
                    out_.addEdge(currentBlock_, tIt->second);
                    return LiftResult::kOk;
                }
                // Conditional: needs a condition on the stack.  After
                // the branch, both successors (target + fallthrough)
                // must exist as separate blocks (per pass 1).
                if (stack_.empty()) {
                    if (failedAtBytecode) *failedAtBytecode = bcOffset;
                    return LiftResult::kMalformedMethod;
                }
                uint32_t cond = stack_.back();
                stack_.pop_back();
                auto fIt = offsetToBlock.find(ip + 1);
                if (fIt == offsetToBlock.end()) {
                    // Couldn't find the fallthrough block — malformed
                    // or pass-1 boundary logic broke.
                    if (failedAtBytecode) *failedAtBytecode = bcOffset;
                    return LiftResult::kMalformedMethod;
                }
                Op brOp = isShortJumpTrue(op) ? Op::kBranchIfTrue
                                               : Op::kBranchIfFalse;
                // Encode successor order in literal high bits if we ever
                // need it; the successors vector carries the data.
                out_.newValue(currentBlock_, brOp, Type::kVoid,
                               /*operands=*/{cond});
                // successors[0] = taken-branch (target), [1] = fallthrough.
                out_.addEdge(currentBlock_, tIt->second);
                out_.addEdge(currentBlock_, fIt->second);
                return LiftResult::kOk;
            }

            // Literal-selector sends (Send0/1/2, 0x80-0xAF).
            //
            // MVP: treat as a one-way compiled exit.  The lifter emits
            // kSendUnspeculated with the operands on the simulated
            // stack, then returns kOk — the block ends here from
            // compiled code's perspective.  The lowerer writes an
            // ExitSend bail sequence; the interpreter resumes from
            // state.ip and runs the send + all subsequent bytecodes.
            //
            // Post-send bytes in the same block go unlifted (dead IR)
            // but that's fine — the interpreter sees the full stream.
            // Real speculation (kGuardClass + kInlineSend) is Phase 4.
            if (jit::SistaV1::isLiteralSend(op)) {
                uint32_t nArgs;
                if      (jit::SistaV1::isSend0(op)) nArgs = 0;
                else if (jit::SistaV1::isSend1(op)) nArgs = 1;
                else                                 nArgs = 2;
                if (stack_.size() < nArgs + 1) {
                    if (failedAtBytecode) *failedAtBytecode = bcOffset;
                    return LiftResult::kMalformedMethod;
                }
                uint32_t selIdx = op & 0x0F;

                // operands = {rcvr, arg0, arg1, ...}  (stack order bottom-up)
                std::vector<uint32_t> ops(nArgs + 1);
                for (uint32_t i = 0; i < nArgs + 1; i++) {
                    ops[nArgs - i] = stack_.back();
                    stack_.pop_back();
                }
                uint64_t lit = static_cast<uint64_t>(selIdx)
                             | (static_cast<uint64_t>(nArgs)    << 16)
                             | (static_cast<uint64_t>(bcOffset) << 24);
                out_.newValue(currentBlock_, Op::kSendUnspeculated,
                               Type::kOop, std::move(ops), lit);
                return LiftResult::kOk;
            }

            // Arith sends 0x60-0x6F — inline the common ones on
            // SmallInt operands.  No class guard or overflow check
            // yet; safety lands with Phase 3 deopt.  Tests today
            // ensure operands are small positive SmallInts.
            if (op == jit::SistaV1::ArithBase + 0    // + (0x60)
                || op == jit::SistaV1::ArithBase + 1 // - (0x61)
                || op == jit::SistaV1::ArithBase + 8 // * (0x68)
               ) {
                if (stack_.size() < 2) {
                    if (failedAtBytecode) *failedAtBytecode = bcOffset;
                    return LiftResult::kMalformedMethod;
                }
                uint32_t b = stack_.back(); stack_.pop_back();
                uint32_t a = stack_.back(); stack_.pop_back();
                Op primOp = Op::kPrimAddInt;
                if (op == jit::SistaV1::ArithBase + 1) primOp = Op::kPrimSubInt;
                if (op == jit::SistaV1::ArithBase + 8) primOp = Op::kPrimMulInt;
                uint32_t v = out_.newValue(currentBlock_, primOp,
                                            Type::kOopSmallInt,
                                            /*operands=*/{a, b});
                stack_.push_back(v);
                ip++;
                continue;
            }

            // Push-receiver: load self, push onto simulated stack.
            if (op == jit::SistaV1::PushReceiver) {
                uint32_t v = out_.newValue(currentBlock_,
                                            Op::kLoadReceiver, Type::kOop);
                stack_.push_back(v);
                ip++;
                continue;
            }

            // Push-temp N: load temp[i], push.
            if (op >= jit::SistaV1::PushTempBase
                && op <= 0x4B) {  // PushTempLast inclusive
                uint32_t tempIdx = op - jit::SistaV1::PushTempBase;
                uint32_t v = out_.newValue(currentBlock_,
                                            Op::kLoadTemp, Type::kOop,
                                            /*operands=*/{}, /*literal=*/tempIdx);
                stack_.push_back(v);
                ip++;
                continue;
            }

            // Push-lit-const N (0x20-0x3F): load literals[N], push.
            if (op >= jit::SistaV1::PushLitConstBase
                && op <= jit::SistaV1::PushLitConstLast) {
                uint32_t litIdx = op - jit::SistaV1::PushLitConstBase;
                uint32_t v = out_.newValue(currentBlock_,
                                            Op::kLoadLiteral, Type::kOop,
                                            /*operands=*/{}, /*literal=*/litIdx);
                stack_.push_back(v);
                ip++;
                continue;
            }

            // Push-recv-var N (0x00-0x0F): load receiver instVar[N].
            if (op >= jit::SistaV1::PushRecvVarBase
                && op <= jit::SistaV1::PushRecvVarLast) {
                uint32_t ivarIdx = op - jit::SistaV1::PushRecvVarBase;
                uint32_t recv = out_.newValue(currentBlock_,
                                               Op::kLoadReceiver, Type::kOop);
                uint32_t v = out_.newValue(currentBlock_,
                                            Op::kLoadInstVar, Type::kOop,
                                            /*operands=*/{recv},
                                            /*literal=*/ivarIdx);
                stack_.push_back(v);
                ip++;
                continue;
            }

            // Pop-store-temp N (0xD0-0xD7): pop TOS, store to temp[N].
            if (op >= jit::SistaV1::PopStoreTempBase
                && op <= jit::SistaV1::PopStoreTempLast) {
                if (stack_.empty()) {
                    if (failedAtBytecode) *failedAtBytecode = bcOffset;
                    return LiftResult::kMalformedMethod;
                }
                uint32_t tempIdx = op - jit::SistaV1::PopStoreTempBase;
                uint32_t val = stack_.back();
                stack_.pop_back();
                out_.newValue(currentBlock_, Op::kStoreTemp, Type::kVoid,
                               /*operands=*/{val},
                               /*literal=*/tempIdx);
                ip++;
                continue;
            }

            // Pop-store-recv-var N (0xC8-0xCF): pop TOS, store to
            // receiver.instVar[N].  Setter pattern.
            if (op >= jit::SistaV1::PopStoreRecvBase
                && op <= jit::SistaV1::PopStoreRecvLast) {
                if (stack_.empty()) {
                    if (failedAtBytecode) *failedAtBytecode = bcOffset;
                    return LiftResult::kMalformedMethod;
                }
                uint32_t ivarIdx = op - jit::SistaV1::PopStoreRecvBase;
                uint32_t val = stack_.back();
                stack_.pop_back();
                uint32_t recv = out_.newValue(currentBlock_,
                                               Op::kLoadReceiver, Type::kOop);
                out_.newValue(currentBlock_, Op::kStoreInstVar, Type::kVoid,
                               /*operands=*/{recv, val},
                               /*literal=*/ivarIdx);
                ip++;
                continue;
            }

            // Pop (0xD8): discard TOS, no other effect.
            if (op == 0xD8) {
                if (stack_.empty()) {
                    if (failedAtBytecode) *failedAtBytecode = bcOffset;
                    return LiftResult::kMalformedMethod;
                }
                stack_.pop_back();
                ip++;
                continue;
            }

            // Dup (0x53): duplicate TOS.
            if (op == jit::SistaV1::Dup) {
                if (stack_.empty()) {
                    if (failedAtBytecode) *failedAtBytecode = bcOffset;
                    return LiftResult::kMalformedMethod;
                }
                stack_.push_back(stack_.back());
                ip++;
                continue;
            }

            // Push-true / push-false: read from JITState.
            if (op == jit::SistaV1::PushTrue) {
                uint32_t v = out_.newValue(currentBlock_,
                                            Op::kLoadTrueOop, Type::kOopBool);
                stack_.push_back(v);
                ip++;
                continue;
            }
            if (op == jit::SistaV1::PushFalse) {
                uint32_t v = out_.newValue(currentBlock_,
                                            Op::kLoadFalseOop, Type::kOopBool);
                stack_.push_back(v);
                ip++;
                continue;
            }

            // Push-nil: const-oop with Oop::s_nilBits (known at compile time).
            if (op == jit::SistaV1::PushNil) {
                uint32_t v = out_.newValue(currentBlock_,
                                            Op::kConstantOop, Type::kOop,
                                            /*operands=*/{},
                                            /*literal=*/Oop::nil().rawBits());
                stack_.push_back(v);
                ip++;
                continue;
            }

            // Push-zero / push-one: SmallInt immediates.
            // Bit pattern: (value << 3) | 1.
            if (op == jit::SistaV1::PushZero) {
                uint32_t v = out_.newValue(currentBlock_,
                                            Op::kConstantOop, Type::kOopSmallInt,
                                            /*operands=*/{},
                                            /*literal=*/(0ULL << 3) | 1);
                stack_.push_back(v);
                ip++;
                continue;
            }
            if (op == jit::SistaV1::PushOne) {
                uint32_t v = out_.newValue(currentBlock_,
                                            Op::kConstantOop, Type::kOopSmallInt,
                                            /*operands=*/{},
                                            /*literal=*/(1ULL << 3) | 1);
                stack_.push_back(v);
                ip++;
                continue;
            }

            // Return-true/false/nil: ^ true, etc.  Same pattern as
            // ReturnReceiver but loading a different constant first.
            if (op == jit::SistaV1::ReturnTrue
                || op == jit::SistaV1::ReturnFalse
                || op == jit::SistaV1::ReturnNil) {
                Op loadOp; Type ty; uint64_t lit = 0;
                if (op == jit::SistaV1::ReturnTrue) {
                    loadOp = Op::kLoadTrueOop; ty = Type::kOopBool;
                } else if (op == jit::SistaV1::ReturnFalse) {
                    loadOp = Op::kLoadFalseOop; ty = Type::kOopBool;
                } else {
                    loadOp = Op::kConstantOop; ty = Type::kOop;
                    lit = Oop::nil().rawBits();
                }
                uint32_t val = out_.newValue(currentBlock_, loadOp, ty,
                                              /*operands=*/{}, lit);
                out_.newValue(currentBlock_, Op::kReturn, Type::kVoid,
                               /*operands=*/{val});
                ip++;
                return LiftResult::kOk;
            }

            // Return-receiver: ^ self; terminator, doesn't use the
            // simulated stack.
            if (op == jit::SistaV1::ReturnReceiver) {
                uint32_t recv = out_.newValue(currentBlock_,
                                               Op::kLoadReceiver, Type::kOop);
                out_.newValue(currentBlock_, Op::kReturn, Type::kVoid,
                               /*operands=*/{recv});
                ip++;
                if (ip != len_) {
                    // Return is a terminator; anything after is dead.
                    // Not an error — trailing bytes can legitimately
                    // exist.  Just stop.
                }
                return LiftResult::kOk;
            }

            // Return-top: ^ <TOS>; terminator, pops stack.
            if (op == jit::SistaV1::ReturnTop) {
                if (stack_.empty()) {
                    if (failedAtBytecode) *failedAtBytecode = bcOffset;
                    return LiftResult::kMalformedMethod;
                }
                uint32_t v = stack_.back();
                stack_.pop_back();
                out_.newValue(currentBlock_, Op::kReturn, Type::kVoid,
                               /*operands=*/{v});
                return LiftResult::kOk;
            }

            // Unsupported — bail cleanly so the JIT can fall back to
            // Tier 1.  This lifter will grow incrementally.
            if (failedAtBytecode) *failedAtBytecode = bcOffset;
            return LiftResult::kUnsupportedBytecode;
        }

        // Ran out of bytecodes without hitting a terminator — malformed.
        if (failedAtBytecode) *failedAtBytecode = static_cast<uint32_t>(len_);
        return LiftResult::kMalformedMethod;
    }

private:
    const uint8_t*         bc_;
    size_t                 len_;
    Method&                out_;
    uint32_t               currentBlock_ = 0;
    std::vector<uint32_t>  stack_;  // Value ids in stack order
};

}  // namespace

LiftResult Builder::buildFromBytes(const uint8_t* bc, size_t len,
                                     uint32_t numArgs, uint32_t numTemps,
                                     Method& out,
                                     uint32_t* failedAtBytecode) {
    LinearLifter l(bc, len, numArgs, numTemps, out);
    return l.run(failedAtBytecode);
}

LiftResult Builder::build(Oop compiledMethod, ObjectMemory& memory,
                           Method& out, uint32_t* failedAtBytecode) {
    if (!compiledMethod.isObject()) return LiftResult::kMalformedMethod;

    ObjectHeader* mh = compiledMethod.asObjectPtr();
    if (!mh->isCompiledMethod()) return LiftResult::kMalformedMethod;

    // Header layout (first slot is the method header SmallInteger).
    Oop hdrOop = memory.fetchPointer(0, compiledMethod);
    if (!hdrOop.isSmallInteger()) return LiftResult::kMalformedMethod;
    int64_t headerBits = hdrOop.asSmallInteger();
    uint32_t numLiterals = static_cast<uint32_t>(headerBits & 0x7FFF);
    uint32_t numArgs     = static_cast<uint32_t>((headerBits >> 24) & 0x0F);
    uint32_t numTemps    = static_cast<uint32_t>((headerBits >> 18) & 0x3F);
    numTemps = (numTemps > numArgs) ? numTemps - numArgs : 0;

    const uint8_t* bytecodes = mh->bytes() + (1 + numLiterals) * 8;
    size_t bytecodeSize = mh->byteSize();

    out.compiledMethodOop = compiledMethod;
    // Cache literals (for now just the raw Oop array).
    out.literals.clear();
    out.literals.reserve(numLiterals);
    for (uint32_t i = 0; i < numLiterals; i++) {
        out.literals.push_back(memory.fetchPointer(1 + i, compiledMethod));
    }

    return buildFromBytes(bytecodes, bytecodeSize, numArgs, numTemps, out,
                           failedAtBytecode);
}

}  // namespace sista
}  // namespace pharo
