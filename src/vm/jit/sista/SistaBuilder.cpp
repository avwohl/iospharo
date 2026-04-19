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

#include <vector>

namespace pharo {
namespace sista {

namespace {

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
        uint32_t entry = out_.newBlock(/*sourceBc=*/0);
        out_.entryBlock = entry;
        currentBlock_   = entry;

        size_t ip = 0;
        while (ip < len_) {
            uint8_t op = bc_[ip];
            uint32_t bcOffset = static_cast<uint32_t>(ip);

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
