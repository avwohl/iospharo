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

#include <algorithm>
#include <cstdlib>
#include <cstring>
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
        || isShortUncondJump(op)
        || op == jit::SistaV1::ExtJump;    // unconditional extended jump
}

// ExtJump family: 2-byte instruction.  Offset = offsetByte + (extB<<8),
// signed.  Target = (ip after 2-byte instruction) + offset.  Both
// forward and backward (loops) supported.
static inline bool isExtJump(uint8_t op) {
    return op == jit::SistaV1::ExtJump
        || op == jit::SistaV1::ExtJumpTrue
        || op == jit::SistaV1::ExtJumpFalse;
}

// Return the length in bytes of the instruction starting with `op`.
// 1 byte for most; 2 for 0xE0-0xF7; 3 for 0xF8-0xFD.  Keeps pass 1
// from misinterpreting a wide op's byte-arg as a fresh opcode.
static inline size_t instructionSize(uint8_t op) {
    if (op >= 0xF8 && op <= 0xFD) return 3;  // CallPrim, PushClosure, etc.
    if (op >= 0xE0 && op <= 0xF7) return 2;  // extended pushes / stores / jumps
    return 1;
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

    // If non-null, specialSendArgCount[N] gives the arg count for
    // opcode 0x70+N (SpecialSend index 16+N in Pharo's selectors
    // array).  When null, SpecialSend bails as unsupported.
    void setSpecialSendArgCounts(const uint8_t* argCounts) {
        specialSendArgCount_ = argCounts;
    }

    // Phase 4 PROOF-OF-CONCEPT: bitmap of literal indices for
    // selectors that we statically inline as no-op (receiver stays
    // on stack).  Today: only #yourself.  Limit: 16 because Send0
    // / Send1 / Send2 opcodes only use literal indices 0-15.
    // Set bit N => literals[N] == #yourself.
    // Set ONLY when PHARO_SISTA_INLINE_YOURSELF=1.  Unsafe for any
    // class that overrides #yourself; gated behind env var until
    // class-hierarchy invalidation lands (Phase 7).
    void setInlineableSelectorBitmap(uint16_t mask) {
        inlineableSelectorMask_ = mask;
    }

    // Phase 4 POC #2: bitmap of literal indices for #== (universal
    // identity-equality semantics).  Send1 to a literal in this
    // bitmap is replaced with kPrimIdentityEq.  Gated behind
    // PHARO_SISTA_INLINE_IDENTITY_EQ=1.
    void setIdentityEqSelectorBitmap(uint16_t mask) {
        identityEqSelectorMask_ = mask;
    }

    // Phase 4 POC #3: bitmap of literal indices for #~~ (identity
    // inequality).  Same gate as #== via PHARO_SISTA_INLINE_IDENTITY_EQ.
    void setIdentityNeqSelectorBitmap(uint16_t mask) {
        identityNeqSelectorMask_ = mask;
    }

    // Phase 3 deopt: when set, builder emits kPrimTagCheckInt
    // before each kPrimAddInt etc.  Lets the unsafe-arith gate
    // lift safely (non-SmallInt operands deopt to interpreter).
    void setTypeCheckArith(bool v) { typeCheckArith_ = v; }

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

        // Pass 1 tracks extB state because ExtJump computes its offset
        // using (offsetByte + extB*256).  extB is set by a preceding
        // ExtendB (0xE1) and consumed by the next non-Extend bytecode.
        int pass1ExtB = 0;

        for (size_t i = 0; i < len_;) {
            uint8_t op = bc_[i];
            if (op == jit::SistaV1::ExtendB) {
                // Truncated at end = method trailer.  Skip silently.
                if (i + 1 >= len_) break;
                pass1ExtB = (int)(int8_t)bc_[i + 1];
                i += 2;
                continue;
            }
            if (op == jit::SistaV1::ExtendA) {
                if (i + 1 >= len_) break;
                i += 2;
                continue;
            }
            if (isExtJump(op)) {
                if (i + 1 >= len_) break;
                int offset = (int)bc_[i + 1] + (pass1ExtB << 8);
                long longTarget = (long)(i + 2) + (long)offset;
                // Out-of-range target: don't fail the whole method.
                // The jump is likely in a dead-code region (padding
                // after a return) that orphan-skip will elide in
                // pass 3.  Skip registering this jump's target /
                // fallthrough as block starts.  If a live block
                // actually reaches this jump, pass 3's short-jump
                // handler catches the unknown-target and bails that
                // block — but the rest of the method still lifts.
                if (longTarget >= 0 && (size_t)longTarget <= len_) {
                    blockStarts.insert((size_t)longTarget);
                    if (i + 2 < len_) blockStarts.insert(i + 2);
                }
                pass1ExtB = 0;
                i += 2;
                continue;
            }
            if (isShortJump(op)) {
                size_t target = shortJumpTarget(i, op);
                if (target <= len_) {
                    blockStarts.insert(target);
                    if (i + 1 < len_) blockStarts.insert(i + 1);
                }
                pass1ExtB = 0;
                i++;
                continue;
            }
            size_t sz = instructionSize(op);
            if (isTerminatorBC(op) && i + sz < len_) {
                blockStarts.insert(i + sz);
            }
            pass1ExtB = 0;
            i += sz;
        }

        // --- Pass 2: create blocks in offset order ---------------------
        std::map<size_t, uint32_t> offsetToBlock;
        for (size_t offset : blockStarts) {
            offsetToBlock[offset] = out_.newBlock((int32_t)offset);
        }
        out_.entryBlock = offsetToBlock[0];

        // --- Pass 3: lift each block ---------------------------------
        //
        // Blocks are processed in offset (= id) order.  Short jumps
        // are forward-only, so every predecessor has a lower id than
        // its successors — i.e. by the time we lift block B, all of
        // B's predecessors have their `outgoingStack` populated from
        // their terminators.
        //
        // For blocks with non-empty entry stack (= predecessor
        // outgoing stack of length N > 0), we pre-create N phi
        // values at the block's head.  The phi operands are wired up
        // in pass 4 once every block's outgoingStack is known.
        for (auto& kv : offsetToBlock) {
            size_t blockStart = kv.first;
            uint32_t blockId  = kv.second;
            stack_.clear();
            currentBlock_ = blockId;

            const Block& thisBlock = out_.blockAt(blockId);

            // Skip orphan blocks: any non-entry block with no
            // predecessors is unreachable (no forward edge points
            // to it, and any loop header would have at least one
            // forward entry).  Pass 1 sometimes creates block
            // starts for post-terminator bytes that aren't real
            // code — lifting them would walk off the end and
            // malform the whole method.  Leave them empty.
            if (blockId != 0 && thisBlock.predecessors.empty()) {
                continue;
            }

            // Determine entry stack depth from FORWARD predecessors
            // (blocks with lower id, already lifted).  Backward
            // predecessors (loops) aren't lifted yet and their
            // outgoingStack is empty — skip them here.  Pass 4's phi
            // wiring validates that their outgoing depth matches this
            // block's entry depth.
            size_t entryDepth = 0;
            bool haveDepth = false;
            for (uint32_t pred : thisBlock.predecessors) {
                if (pred >= blockId) continue;  // backward edge — validate later
                const Block& pb = out_.blockAt(pred);
                if (!haveDepth) {
                    entryDepth = pb.outgoingStack.size();
                    haveDepth = true;
                } else if (pb.outgoingStack.size() != entryDepth) {
                    // Forward predecessors disagree — malformed.
                    if (failedAtBytecode)
                        *failedAtBytecode = (uint32_t)blockStart;
                    return LiftResult::kMalformedMethod;
                }
            }
            // Pre-create a phi for each stack slot the block inherits.
            // Operands wired in pass 4.  Type is generic Oop for now;
            // refinement comes with the type system.
            for (size_t i = 0; i < entryDepth; i++) {
                uint32_t phiId = out_.newValue(blockId, Op::kPhi,
                                                Type::kOop);
                stack_.push_back(phiId);
            }

            LiftResult r = liftFromOffset(blockStart, offsetToBlock,
                                           failedAtBytecode);
            if (r != LiftResult::kOk) return r;

            // Record outgoing stack for phi wiring.  The lifter may
            // have left values on stack_ at a branch terminator; at
            // returns / sends stack_ ends up empty.
            out_.blocks[blockId].outgoingStack = stack_;
        }

        // --- Pass 4: wire phi operands ---------------------------------
        for (Block& b : out_.blocks) {
            // Phis are always at the head of the block.
            for (uint32_t vid : b.values) {
                Value& v = out_.values[vid];
                if (v.op != Op::kPhi) break;  // phis come first; stop at first non-phi
                // Find this phi's slot index within the block's phis.
                // Since phis are created in order matching the entry
                // stack slots (bottom-up), we can use the v.id - b.values[0]
                // offset.
                size_t slotIdx = vid - b.values.front();
                for (uint32_t pred : b.predecessors) {
                    const Block& pb = out_.blockAt(pred);
                    if (slotIdx >= pb.outgoingStack.size()) {
                        // Predecessor didn't supply this slot.  Pass 3
                        // should have caught this, but guard anyway.
                        // Report the block's source bytecode offset so
                        // the caller / survey can identify the spot.
                        if (failedAtBytecode) {
                            *failedAtBytecode = (b.sourceBytecodeOffset >= 0)
                                ? (uint32_t)b.sourceBytecodeOffset
                                : 0;
                        }
                        return LiftResult::kMalformedMethod;
                    }
                    v.operands.push_back(pb.outgoingStack[slotIdx]);
                }
            }
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
            // If we're not mid-Extend-prefix, this ip starts a new
            // logical instruction.
            if (pendingExtA_ == 0 && pendingExtB_ == 0) {
                currentInstrStart_ = ip;
            }

            // Crossed into a new block — fall-through.  The current
            // stack_ contents become the outgoing stack and feed the
            // successor's phis (wired in pass 4).
            if (ip != startOffset && offsetToBlock.count(ip)) {
                uint32_t nextBlock = offsetToBlock.at(ip);
                out_.addEdge(currentBlock_, nextBlock);
                return LiftResult::kOk;
            }

            // CallPrimitive (0xF8): 3-byte primitive declaration that
            // only appears at method start.  The interpreter runs the
            // primitive; if it fails or returns, the fallback bytecodes
            // (which start right after CallPrimitive) execute.  Sista
            // is called only for the fallback path — so we skip the
            // 3-byte declaration at lift time.
            if (op == jit::SistaV1::CallPrimitive && ip == 0) {
                if (ip + 2 >= len_) {
                    if (failedAtBytecode) *failedAtBytecode = bcOffset;
                    return LiftResult::kMalformedMethod;
                }
                ip += 3;
                continue;
            }

            // Mid-method bail-to-interpreter: the full IR stack is
            // pushed to the interpreter stack and control transfers
            // via ExitSend at `currentInstrStart_`.  The interpreter
            // then runs the bytecode normally (allocating objects,
            // creating closures, etc.) and continues past it.  This
            // is the generic escape hatch for ops we don't lift —
            // correct because the IR stack exactly mirrors the
            // Smalltalk operand stack.
            //
            // Used for PushFullBlock, PushClosure, PushArray,
            // PushThisContext — all mid-method object-creating or
            // context-reflection ops that don't fit into pure-
            // register IR without runtime allocation support.
            //
            // Note: at most 255 items on the IR stack (nArgs field
            // is 8 bits).  Real methods never exceed this.
            auto bailToInterpreter = [&](size_t instrLen) -> LiftResult {
                if (stack_.size() > 255) {
                    if (failedAtBytecode) *failedAtBytecode = bcOffset;
                    return LiftResult::kMalformedMethod;
                }
                uint32_t stackSize = (uint32_t)stack_.size();
                std::vector<uint32_t> ops(stack_.begin(), stack_.end());
                stack_.clear();
                uint64_t bailOffset = (uint64_t)currentInstrStart_;
                uint64_t lit = 0
                             | ((uint64_t)stackSize << 16)
                             | (bailOffset         << 24);
                uint32_t vid = out_.newValue(currentBlock_, Op::kSendUnspeculated,
                               Type::kOop, std::move(ops), lit);
                recordFramepoint(vid, static_cast<uint32_t>(bailOffset));
                pendingExtA_ = 0;
                pendingExtB_ = 0;
                (void)instrLen;  // bail consumes the whole instruction
                return LiftResult::kOk;
            };

            if (op == jit::SistaV1::PushFullBlock
             || op == jit::SistaV1::PushClosure
             || op == jit::SistaV1::PushArray
             || op == jit::SistaV1::PushThisContext) {
                return bailToInterpreter(instructionSize(op));
            }

            // ExtendA / ExtendB prefix: stash the byte arg and let the
            // next op consume it.  Does not emit IR.  If the next op
            // is unsupported, pass 3 bails and we stay correct.
            if (op == jit::SistaV1::ExtendA) {
                // ExtendA at the very last byte = trailer boundary
                // (Pharo trailer-byte happens to be 0xE0).  Real
                // bytecodes end here; stop lifting this block.
                if (ip + 1 >= len_) {
                    if (stack_.empty()) return LiftResult::kOk;
                    return bailToInterpreter(1);
                }
                pendingExtA_ = (int)bc_[ip + 1];
                ip += 2;
                continue;
            }
            if (op == jit::SistaV1::ExtendB) {
                if (ip + 1 >= len_) {
                    if (stack_.empty()) return LiftResult::kOk;
                    return bailToInterpreter(1);
                }
                pendingExtB_ = (int)(int8_t)bc_[ip + 1];
                ip += 2;
                continue;
            }

            // ExtJump family — 2-byte jump.  Offset = offsetByte + extB*256,
            // signed.  Both forward and backward (loop) jumps supported;
            // pass 3 iteration order computes loop-header entry depths
            // from forward predecessors and pass 4 validates that
            // backward predecessors agree.
            if (isExtJump(op)) {
                if (ip + 1 >= len_) {
                    // Truncated 2-byte instruction at end of method —
                    // likely reading into the method trailer.  Bail
                    // rather than fail the whole method's lift.
                    if (stack_.empty()) return LiftResult::kOk;
                    return bailToInterpreter(1);
                }
                int offset = (int)bc_[ip + 1] + (pendingExtB_ << 8);
                pendingExtA_ = 0;
                pendingExtB_ = 0;
                long longTarget = (long)(ip + 2) + (long)offset;
                if (longTarget < 0 || (size_t)longTarget > len_) {
                    // Out-of-range: bail this block to the interpreter
                    // rather than malforming the whole method.
                    return bailToInterpreter(2);
                }
                size_t target = (size_t)longTarget;
                auto tIt = offsetToBlock.find(target);
                if (tIt == offsetToBlock.end()) {
                    return bailToInterpreter(2);
                }
                if (op == jit::SistaV1::ExtJump) {
                    out_.newValue(currentBlock_, Op::kBranch, Type::kVoid,
                                   /*operands=*/{}, /*literal=*/tIt->second);
                    out_.addEdge(currentBlock_, tIt->second);
                    return LiftResult::kOk;
                }
                // Conditional extended jump.  Needs condition on stack.
                if (stack_.empty()) {
                    if (failedAtBytecode) *failedAtBytecode = bcOffset;
                    return LiftResult::kMalformedMethod;
                }
                uint32_t cond = stack_.back();
                stack_.pop_back();
                auto fIt = offsetToBlock.find(ip + 2);
                if (fIt == offsetToBlock.end()) {
                    if (failedAtBytecode) *failedAtBytecode = bcOffset;
                    return LiftResult::kMalformedMethod;
                }
                Op brOp = (op == jit::SistaV1::ExtJumpTrue)
                            ? Op::kBranchIfTrue
                            : Op::kBranchIfFalse;
                out_.newValue(currentBlock_, brOp, Type::kVoid,
                               /*operands=*/{cond});
                out_.addEdge(currentBlock_, tIt->second);
                out_.addEdge(currentBlock_, fIt->second);
                return LiftResult::kOk;
            }

            // Extended pushes (2-byte).  index = (extA<<8) | byteArg
            // for recv-var / lit-const; for temp it's the raw byte.
            if (op == jit::SistaV1::ExtPushRecvVar) {
                if (ip + 1 >= len_) {
                    if (failedAtBytecode) *failedAtBytecode = bcOffset;
                    return LiftResult::kMalformedMethod;
                }
                uint32_t ivarIdx = (uint32_t)((pendingExtA_ << 8) | bc_[ip + 1]);
                pendingExtA_ = 0;
                pendingExtB_ = 0;
                uint32_t recv = out_.newValue(currentBlock_,
                                               Op::kLoadReceiver, Type::kOop);
                uint32_t v = out_.newValue(currentBlock_, Op::kLoadInstVar,
                                            Type::kOop,
                                            /*operands=*/{recv},
                                            /*literal=*/ivarIdx);
                stack_.push_back(v);
                ip += 2;
                continue;
            }
            // ExtPushLitVar (0xE3): wide-index version of PushLitVar.
            // Same composition: load literal (Association), load slot 1.
            if (op == jit::SistaV1::ExtPushLitVar) {
                if (ip + 1 >= len_) {
                    if (failedAtBytecode) *failedAtBytecode = bcOffset;
                    return LiftResult::kMalformedMethod;
                }
                uint32_t litIdx = (uint32_t)((pendingExtA_ << 8) | bc_[ip + 1]);
                pendingExtA_ = 0;
                pendingExtB_ = 0;
                uint32_t assoc = out_.newValue(currentBlock_,
                                                Op::kLoadLiteral, Type::kOop,
                                                /*operands=*/{},
                                                /*literal=*/litIdx);
                uint32_t val = out_.newValue(currentBlock_,
                                              Op::kLoadInstVar, Type::kOop,
                                              /*operands=*/{assoc},
                                              /*literal=*/1);
                stack_.push_back(val);
                ip += 2;
                continue;
            }

            if (op == jit::SistaV1::ExtPushLitConst) {
                if (ip + 1 >= len_) {
                    if (failedAtBytecode) *failedAtBytecode = bcOffset;
                    return LiftResult::kMalformedMethod;
                }
                uint32_t litIdx = (uint32_t)((pendingExtA_ << 8) | bc_[ip + 1]);
                pendingExtA_ = 0;
                pendingExtB_ = 0;
                uint32_t v = out_.newValue(currentBlock_, Op::kLoadLiteral,
                                            Type::kOop,
                                            /*operands=*/{},
                                            /*literal=*/litIdx);
                stack_.push_back(v);
                ip += 2;
                continue;
            }
            if (op == jit::SistaV1::ExtPushTemp) {
                // ExtPushTemp uses raw byte as index (no extA per the
                // interpreter — see Interpreter.cpp:3733).
                if (ip + 1 >= len_) {
                    if (failedAtBytecode) *failedAtBytecode = bcOffset;
                    return LiftResult::kMalformedMethod;
                }
                uint32_t tempIdx = (uint32_t)bc_[ip + 1];
                pendingExtA_ = 0;
                pendingExtB_ = 0;
                uint32_t v = out_.newValue(currentBlock_, Op::kLoadTemp,
                                            Type::kOop,
                                            /*operands=*/{},
                                            /*literal=*/tempIdx);
                stack_.push_back(v);
                ip += 2;
                continue;
            }

            // Extended pop-stores (2-byte).
            if (op == jit::SistaV1::ExtPopStoreRecv) {
                if (ip + 1 >= len_ || stack_.empty()) {
                    if (failedAtBytecode) *failedAtBytecode = bcOffset;
                    return LiftResult::kMalformedMethod;
                }
                uint32_t ivarIdx = (uint32_t)((pendingExtA_ << 8) | bc_[ip + 1]);
                pendingExtA_ = 0;
                pendingExtB_ = 0;
                uint32_t val = stack_.back();
                stack_.pop_back();
                uint32_t recv = out_.newValue(currentBlock_,
                                               Op::kLoadReceiver, Type::kOop);
                out_.newValue(currentBlock_, Op::kStoreInstVar, Type::kVoid,
                               /*operands=*/{recv, val},
                               /*literal=*/ivarIdx);
                ip += 2;
                continue;
            }
            if (op == jit::SistaV1::ExtPopStoreTemp) {
                if (ip + 1 >= len_ || stack_.empty()) {
                    if (failedAtBytecode) *failedAtBytecode = bcOffset;
                    return LiftResult::kMalformedMethod;
                }
                uint32_t tempIdx = (uint32_t)bc_[ip + 1];
                pendingExtA_ = 0;
                pendingExtB_ = 0;
                uint32_t val = stack_.back();
                stack_.pop_back();
                out_.newValue(currentBlock_, Op::kStoreTemp, Type::kVoid,
                               /*operands=*/{val},
                               /*literal=*/tempIdx);
                ip += 2;
                continue;
            }

            // No-pop extended stores (ExtStoreRecv 0xF3, ExtStoreLitVar
            // 0xF4, ExtStoreTemp 0xF5) — store the top of stack without
            // consuming it.  Same as the pop variants but peek rather
            // than pop.
            if (op == jit::SistaV1::ExtStoreRecv) {
                if (ip + 1 >= len_ || stack_.empty()) {
                    if (failedAtBytecode) *failedAtBytecode = bcOffset;
                    return LiftResult::kMalformedMethod;
                }
                uint32_t ivarIdx = (uint32_t)((pendingExtA_ << 8) | bc_[ip + 1]);
                pendingExtA_ = 0;
                pendingExtB_ = 0;
                uint32_t val = stack_.back();  // peek, don't pop
                uint32_t recv = out_.newValue(currentBlock_,
                                               Op::kLoadReceiver, Type::kOop);
                out_.newValue(currentBlock_, Op::kStoreInstVar, Type::kVoid,
                               /*operands=*/{recv, val},
                               /*literal=*/ivarIdx);
                ip += 2;
                continue;
            }
            if (op == jit::SistaV1::ExtStoreTemp) {
                if (ip + 1 >= len_) {
                    if (stack_.empty()) return LiftResult::kOk;
                    return bailToInterpreter(1);
                }
                if (stack_.empty()) {
                    // Common pattern: method starts with
                    // `CallPrimitive` (we skip it) + `ExtStoreTemp` to
                    // capture a caller-pushed arg into a temp.  Our IR
                    // stack doesn't model args-on-entry, so bail to
                    // the interpreter at this bytecode — it has the
                    // real stack and will execute correctly.
                    return bailToInterpreter(2);
                }
                uint32_t tempIdx = (uint32_t)bc_[ip + 1];
                pendingExtA_ = 0;
                pendingExtB_ = 0;
                uint32_t val = stack_.back();  // peek, don't pop
                out_.newValue(currentBlock_, Op::kStoreTemp, Type::kVoid,
                               /*operands=*/{val},
                               /*literal=*/tempIdx);
                ip += 2;
                continue;
            }
            // ExtStoreLitVar (0xF4): no-pop store to Association.value.
            // Semantically: literals[N].value := TOS.  Our IR doesn't
            // yet have a "store into ivar of Oop operand" equivalent
            // for Associations — we have kStoreInstVar which takes the
            // receiver as an operand.  Bail to interpreter.
            if (op == jit::SistaV1::ExtStoreLitVar
             || op == jit::SistaV1::ExtPopStoreLitVar) {
                return bailToInterpreter(2);
            }

            // Remote-temp ops (0xFB PushTempAtInVec, 0xFC StoreTempInVec,
            // 0xFD PopStoreTempInVec) access closure-captured variables
            // via a shared temp vector.  Not yet modeled in the IR;
            // bail to interpreter.
            if (op == jit::SistaV1::PushTempAtInVec
             || op == 0xFC  // StoreTempInVec
             || op == 0xFD) // PopStoreTempInVec
            {
                return bailToInterpreter(3);
            }

            // InlinedPrimitive (0xEC): VMMaker inlines specific
            // primitive operations into the bytecode stream for small
            // perf wins (e.g., SmallInt arith with explicit overflow
            // check).  Bail to interpreter.
            if (op == jit::SistaV1::InlinedPrimitive) {
                return bailToInterpreter(3);
            }

            // Short jumps: unconditional and conditional.
            if (isShortJump(op)) {
                size_t target = shortJumpTarget(ip, op);
                auto tIt = offsetToBlock.find(target);
                if (tIt == offsetToBlock.end()) {
                    // Target not a block start — dead-code jump.  Bail
                    // this block to the interpreter instead of failing
                    // the whole method.
                    return bailToInterpreter(1);
                }
                if (isShortUncondJump(op)) {
                    // stack_ contents become outgoing stack, feeding
                    // the target block's phis.
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

            // PushInteger (0xE8): inline signed integer literal.
            // Value = byteArg + extB*256, pushed as SmallInt Oop.
            if (op == jit::SistaV1::PushInteger) {
                if (ip + 1 >= len_) {
                    if (failedAtBytecode) *failedAtBytecode = bcOffset;
                    return LiftResult::kMalformedMethod;
                }
                int value = (int)bc_[ip + 1] + (pendingExtB_ << 8);
                pendingExtA_ = 0;
                pendingExtB_ = 0;
                uint64_t oopBits = ((uint64_t)(int64_t)value << 3) | 1;
                uint32_t v = out_.newValue(currentBlock_, Op::kConstantOop,
                                            Type::kOopSmallInt,
                                            /*operands=*/{},
                                            /*literal=*/oopBits);
                stack_.push_back(v);
                ip += 2;
                continue;
            }

            // PushCharacter (0xE9): codepoint = byteArg + extB*256.
            // Oop bits = (codepoint << 3) | 3 (Character tag).
            if (op == jit::SistaV1::PushCharacter) {
                if (ip + 1 >= len_) {
                    if (failedAtBytecode) *failedAtBytecode = bcOffset;
                    return LiftResult::kMalformedMethod;
                }
                int codepoint = (int)bc_[ip + 1] + (pendingExtB_ << 8);
                pendingExtA_ = 0;
                pendingExtB_ = 0;
                if (codepoint < 0) {
                    if (failedAtBytecode) *failedAtBytecode = bcOffset;
                    return LiftResult::kMalformedMethod;
                }
                uint64_t oopBits = ((uint64_t)codepoint << 3) | 3;
                uint32_t v = out_.newValue(currentBlock_, Op::kConstantOop,
                                            Type::kOopChar,
                                            /*operands=*/{},
                                            /*literal=*/oopBits);
                stack_.push_back(v);
                ip += 2;
                continue;
            }

            // ExtSend (0xEA): wider selector/arg-count range than Send0/1/2.
            //   selector = (extA << 5) | (desc >> 3)
            //   nArgs    = (extB << 3) | (desc & 0x07)
            // Treated as a bail-to-interpreter just like Send0/1/2.
            if (op == jit::SistaV1::ExtSend) {
                if (ip + 1 >= len_) {
                    if (failedAtBytecode) *failedAtBytecode = bcOffset;
                    return LiftResult::kMalformedMethod;
                }
                uint8_t desc = bc_[ip + 1];
                uint32_t selIdx = (uint32_t)(((pendingExtA_ << 5)
                                               | (desc >> 3)) & 0xFFFF);
                uint32_t nArgs  = (uint32_t)(((pendingExtB_ << 3)
                                               | (desc & 0x07)) & 0xFF);
                uint32_t bailOffset = (uint32_t)currentInstrStart_;
                pendingExtA_ = 0;
                pendingExtB_ = 0;
                if (stack_.size() < nArgs + 1) {
                    if (failedAtBytecode) *failedAtBytecode = bcOffset;
                    return LiftResult::kMalformedMethod;
                }
                // Flush entire IR stack (see Send0/1/2 comment) —
                // the send's rcvr+args are the top nArgs+1 entries,
                // and earlier values below must reach state.sp too.
                std::vector<uint32_t> ops(stack_.begin(), stack_.end());
                stack_.clear();
                uint64_t lit = (uint64_t)selIdx
                             | ((uint64_t)nArgs      << 16)
                             | ((uint64_t)bailOffset << 24);
                uint32_t vid = out_.newValue(currentBlock_, Op::kSendUnspeculated,
                               Type::kOop, std::move(ops), lit);
                recordFramepoint(vid, bailOffset);
                return LiftResult::kOk;
            }

            // ExtSuperSend (0xEB): like ExtSend but with super-lookup.
            // Two encodings:
            //   extB <  64: regular super-send (ends block cleanly).
            //   extB >= 64: directed super — pops an extra definingClass
            //               operand on top of rcvr+args.
            // Either way the bail just transfers the operands to the
            // interpreter stack in Smalltalk order; the interpreter
            // handles the super lookup.
            if (op == jit::SistaV1::ExtSuperSend) {
                if (ip + 1 >= len_) {
                    if (stack_.empty()) return LiftResult::kOk;
                    return bailToInterpreter(1);
                }
                uint8_t desc = bc_[ip + 1];
                uint32_t selIdx;
                uint32_t nArgs;
                uint32_t extras;  // extra operand count beyond rcvr+args
                if (pendingExtB_ >= 64) {
                    // Directed super: stack = [..., rcvr, args..., definingClass]
                    int ebRelative = pendingExtB_ - 64;
                    selIdx = (uint32_t)(((pendingExtA_ << 5)
                                          | (desc >> 3)) & 0xFFFF);
                    nArgs  = (uint32_t)(((ebRelative << 3)
                                          | (desc & 0x07)) & 0xFF);
                    extras = 1;
                } else {
                    selIdx = (uint32_t)(((pendingExtA_ << 5)
                                          | (desc >> 3)) & 0xFFFF);
                    nArgs  = (uint32_t)(((pendingExtB_ << 3)
                                          | (desc & 0x07)) & 0xFF);
                    extras = 0;
                }
                uint32_t bailOffset = (uint32_t)currentInstrStart_;
                pendingExtA_ = 0;
                pendingExtB_ = 0;
                uint32_t totalOps = nArgs + 1 + extras;
                if (stack_.size() < totalOps) {
                    if (failedAtBytecode) *failedAtBytecode = bcOffset;
                    return LiftResult::kMalformedMethod;
                }
                // Flush entire IR stack (super-send + extras + below).
                std::vector<uint32_t> ops(stack_.begin(), stack_.end());
                stack_.clear();
                uint64_t lit = (uint64_t)selIdx
                             | ((uint64_t)nArgs      << 16)
                             | ((uint64_t)bailOffset << 24);
                uint32_t vid = out_.newValue(currentBlock_, Op::kSendUnspeculated,
                               Type::kOop, std::move(ops), lit);
                recordFramepoint(vid, bailOffset);
                return LiftResult::kOk;
            }

            // SpecialSend (0x70-0x7F): send well-known selector with
            // fixed arg count (from the image's SpecialSelectorsArray).
            // If argCount table isn't provided (unit tests), bail.
            if (op >= jit::SistaV1::SpecialSendBase
                && op <= jit::SistaV1::SpecialSendLast) {
                if (!specialSendArgCount_) {
                    if (failedAtBytecode) *failedAtBytecode = bcOffset;
                    return LiftResult::kUnsupportedBytecode;
                }
                uint32_t ssIdx = op - jit::SistaV1::SpecialSendBase;
                uint32_t nArgs = specialSendArgCount_[ssIdx];
                if (stack_.size() < nArgs + 1) {
                    if (failedAtBytecode) *failedAtBytecode = bcOffset;
                    return LiftResult::kMalformedMethod;
                }
                // Selector literal index encoded as special-selector
                // marker: we use the opcode as selIdx (unused in the
                // bail path since the interpreter dispatches via the
                // bytecode directly).  Flush entire IR stack so
                // pre-send values aren't lost (see Send0/1/2 note).
                std::vector<uint32_t> ops(stack_.begin(), stack_.end());
                stack_.clear();
                uint64_t lit = (uint64_t)op
                             | ((uint64_t)nArgs    << 16)
                             | ((uint64_t)bcOffset << 24);
                uint32_t vid = out_.newValue(currentBlock_, Op::kSendUnspeculated,
                               Type::kOop, std::move(ops), lit);
                recordFramepoint(vid, bcOffset);
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

                // Phase 4 POC: inline #yourself as no-op.  Only for
                // Send0 (yourself takes no args).  Receiver stays on
                // simulated stack as the result.  Unsafe if rcvr's
                // class overrides #yourself; gated behind
                // PHARO_SISTA_INLINE_YOURSELF=1.
                if (nArgs == 0 && selIdx < 16
                    && (inlineableSelectorMask_ & (1u << selIdx))) {
                    // No IR change — receiver was on stack_ already
                    // and that's the correct result of yourself.
                    ip++;
                    continue;
                }

                // Phase 4 POC #2/#3: inline #== / #~~ as identity
                // comparison.  Only for Send1 (both take 1 arg).
                // Pop a, b; emit kPrimIdentityEq/Neq; push result.
                // Universal semantics — never overridden in
                // well-behaved code.  Gated behind
                // PHARO_SISTA_INLINE_IDENTITY_EQ=1.
                if (nArgs == 1 && selIdx < 16) {
                    Op identityOp = Op::kPrimIdentityEq;
                    bool isIdentity = false;
                    if (identityEqSelectorMask_ & (1u << selIdx)) {
                        identityOp = Op::kPrimIdentityEq;
                        isIdentity = true;
                    } else if (identityNeqSelectorMask_ & (1u << selIdx)) {
                        identityOp = Op::kPrimIdentityNeq;
                        isIdentity = true;
                    }
                    if (isIdentity) {
                        uint32_t b_op = stack_.back(); stack_.pop_back();
                        uint32_t a_op = stack_.back(); stack_.pop_back();
                        uint32_t v = out_.newValue(currentBlock_,
                                                    identityOp,
                                                    Type::kOopBool,
                                                    /*operands=*/{a_op, b_op});
                        stack_.push_back(v);
                        ip++;
                        continue;
                    }
                }

                // Bail flushes the ENTIRE IR stack to state.sp so the
                // interpreter sees every value simulated-pushed by
                // compiled code.  Older revisions only emitted the
                // send's rcvr+args, leaking values below them (e.g.,
                // `PushReceiver PushTemp Send0 Send1 ...` at the
                // first Send0 left `self` in a virtual register that
                // never reached state.sp, and the next Send1 in the
                // interpreter's run popped garbage).  The stackSize
                // field encodes the total pushes — nArgs is the send
                // arg count (what sendSelector pops); any extras
                // below them are caller-frame values the interpreter
                // will consume via later bytecodes.
                uint32_t stackSize = static_cast<uint32_t>(stack_.size());
                std::vector<uint32_t> ops(stack_.begin(), stack_.end());
                stack_.clear();
                uint64_t lit = static_cast<uint64_t>(selIdx)
                             | (static_cast<uint64_t>(nArgs)      << 16)
                             | (static_cast<uint64_t>(bcOffset)   << 24);
                (void)stackSize;  // nArgs suffices for sendSelector;
                                  // extras come from total push count
                                  // vs operands.size() on the lowerer
                                  // side (operands pushed in order).
                uint32_t vid = out_.newValue(currentBlock_, Op::kSendUnspeculated,
                               Type::kOop, std::move(ops), lit);
                recordFramepoint(vid, bcOffset);
                return LiftResult::kOk;
            }

            // Arith sends 0x60-0x6F — inline the common ones on
            // SmallInt operands.  Phase 3 deopt: when
            // PHARO_SISTA_INLINE_ARITH=1, emit kPrimTagCheckInt
            // before each kPrimAddInt etc. so non-SmallInt operands
            // safely deopt to the interpreter (which re-executes
            // the original arith bytecode handling all types).
            //
            // Arithmetic (+ - *) produces a SmallInt result; comparisons
            // (< <= > >= = ~=) produce a boolean.  Without
            // PHARO_SISTA_INLINE_ARITH=1, methods with these opcodes
            // are excluded from Sista dispatch entirely (handled in
            // Interpreter.cpp).
            const bool isArith = (op == jit::SistaV1::ArithBase + 0   // +
                              ||  op == jit::SistaV1::ArithBase + 1   // -
                              ||  op == jit::SistaV1::ArithBase + 8); // *
            const bool isArithCmp = (op >= jit::SistaV1::ArithBase + 2
                                  && op <= jit::SistaV1::ArithBase + 7);
            if (isArith || isArithCmp) {
                if (stack_.size() < 2) {
                    if (failedAtBytecode) *failedAtBytecode = bcOffset;
                    return LiftResult::kMalformedMethod;
                }
                // Snapshot deopt stack BEFORE popping operands —
                // when type check misses, the interpreter resumes
                // at this bcOffset and expects the original [..., a, b]
                // on its stack.
                std::vector<uint32_t> deoptStack = stack_;

                uint32_t b = stack_.back(); stack_.pop_back();
                uint32_t a = stack_.back(); stack_.pop_back();

                // Emit type checks if PHARO_SISTA_INLINE_ARITH=1.
                // Each check operand[0] = value to test;
                // operand[1..N] = deopt stack snapshot.
                if (typeCheckArith_) {
                    std::vector<uint32_t> checkA{a};
                    checkA.insert(checkA.end(), deoptStack.begin(), deoptStack.end());
                    std::vector<uint32_t> checkB{b};
                    checkB.insert(checkB.end(), deoptStack.begin(), deoptStack.end());
                    out_.newValue(currentBlock_, Op::kPrimTagCheckInt,
                                  Type::kOopSmallInt, std::move(checkA),
                                  /*literal=*/bcOffset);
                    out_.newValue(currentBlock_, Op::kPrimTagCheckInt,
                                  Type::kOopSmallInt, std::move(checkB),
                                  /*literal=*/bcOffset);
                }

                Op primOp;
                Type resultType;
                if (isArith) {
                    primOp = Op::kPrimAddInt;
                    if (op == jit::SistaV1::ArithBase + 1) primOp = Op::kPrimSubInt;
                    if (op == jit::SistaV1::ArithBase + 8) primOp = Op::kPrimMulInt;
                    resultType = Type::kOopSmallInt;
                } else {  // isArithCmp
                    switch (op - jit::SistaV1::ArithBase) {
                      case 2: primOp = Op::kPrimLtInt;  break;  // <
                      case 3: primOp = Op::kPrimGtInt;  break;  // >
                      case 4: primOp = Op::kPrimLeInt;  break;  // <=
                      case 5: primOp = Op::kPrimGeInt;  break;  // >=
                      case 6: primOp = Op::kPrimEqInt;  break;  // =
                      case 7: primOp = Op::kPrimNeqInt; break;  // ~=
                      default: primOp = Op::kPrimEqInt; break;
                    }
                    resultType = Type::kOopBool;
                }

                // Under typeCheckArith_, attach deopt-stack so
                // lowering emits overflow detection + bail.
                // Layout: operands = [a, b, ...deoptStack],
                // literal = bcOffset.  Comparisons can't overflow;
                // arith add/sub/mul all get deopt info.
                std::vector<uint32_t> arithOperands{a, b};
                uint64_t arithLiteral = 0;
                if (typeCheckArith_
                    && (primOp == Op::kPrimAddInt
                     || primOp == Op::kPrimSubInt
                     || primOp == Op::kPrimMulInt)) {
                    arithOperands.insert(arithOperands.end(),
                                          deoptStack.begin(),
                                          deoptStack.end());
                    arithLiteral = bcOffset;
                }
                uint32_t v = out_.newValue(currentBlock_, primOp,
                                            resultType,
                                            std::move(arithOperands),
                                            arithLiteral);
                stack_.push_back(v);
                ip++;
                continue;
            }
            // Other arith opcodes in 0x60-0x6F (<, >, <=, >=, =, ~=,
            // /, \\, @, bitShift:, //, bitAnd:, bitOr:) are 1-arg
            // sends.  Bail to interpreter; real inlining (with deopt
            // on type miss) lands with Phase 3.
            if (op >= jit::SistaV1::ArithBase
                && op <= jit::SistaV1::ArithBase + 15) {
                uint32_t nArgs = 1;
                if (stack_.size() < nArgs + 1) {
                    if (failedAtBytecode) *failedAtBytecode = bcOffset;
                    return LiftResult::kMalformedMethod;
                }
                // Flush entire IR stack (see Send0/1/2 note).
                std::vector<uint32_t> ops(stack_.begin(), stack_.end());
                stack_.clear();
                uint64_t lit = (uint64_t)op
                             | ((uint64_t)nArgs    << 16)
                             | ((uint64_t)bcOffset << 24);
                uint32_t vid = out_.newValue(currentBlock_, Op::kSendUnspeculated,
                               Type::kOop, std::move(ops), lit);
                recordFramepoint(vid, bcOffset);
                return LiftResult::kOk;
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

            // Push-lit-var N (0x10-0x1F): literals[N] is an Association
            // object; push its .value slot (slot 1, at byte offset 16).
            //
            // Composes existing kLoadLiteral + kLoadInstVar(slot=1).
            // No new IR op needed.
            if (op >= jit::SistaV1::PushLitVarBase
                && op <= jit::SistaV1::PushLitVarLast) {
                uint32_t litIdx = op - jit::SistaV1::PushLitVarBase;
                uint32_t assoc = out_.newValue(currentBlock_,
                                                Op::kLoadLiteral, Type::kOop,
                                                /*operands=*/{},
                                                /*literal=*/litIdx);
                uint32_t val = out_.newValue(currentBlock_,
                                              Op::kLoadInstVar, Type::kOop,
                                              /*operands=*/{assoc},
                                              /*literal=*/1);  // slot 1 = .value
                stack_.push_back(val);
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

            // 0x54-0x57, 0x5F, 0xDA-0xDF: 1-byte unassigned/no-op.
            // Interpreter treats these as no-ops; skip in the lifter.
            if ((op >= 0x54 && op <= 0x57) || op == 0x5F
                || (op >= 0xDA && op <= 0xDF)) {
                ip++;
                continue;
            }
            // 0xE6, 0xF6, 0xF7: 2-byte unassigned (interpreter consumes
            // opcode + 1 arg byte and no-ops).
            if (op == 0xE6 || op == 0xF6 || op == 0xF7) {
                if (ip + 1 >= len_) {
                    if (failedAtBytecode) *failedAtBytecode = bcOffset;
                    return LiftResult::kMalformedMethod;
                }
                pendingExtA_ = 0;
                pendingExtB_ = 0;
                ip += 2;
                continue;
            }
            // 0xFE, 0xFF: 3-byte unassigned.
            if (op == 0xFE || op == 0xFF) {
                if (ip + 2 >= len_) {
                    if (failedAtBytecode) *failedAtBytecode = bcOffset;
                    return LiftResult::kMalformedMethod;
                }
                pendingExtA_ = 0;
                pendingExtB_ = 0;
                ip += 3;
                continue;
            }

            // BlockReturnNil / BlockReturnTop (0x5D / 0x5E) — non-local
            // returns from within a block.  Semantically complex; bail
            // to the interpreter with the full IR stack transferred.
            // UnconditionalTrap (0xD9) — `self halt` / debugger trap.
            if (op == jit::SistaV1::BlockReturnNil
             || op == jit::SistaV1::BlockReturnTop
             || op == 0xD9) {
                return bailToInterpreter(1);
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

    // Phase 3 deopt support: snapshot frame state at a potential
    // deopt site.  Today every kSendUnspeculated emission calls this
    // immediately after newValue.  The framepoint records the
    // information needed to bail/deopt back to the interpreter.
    void recordFramepoint(uint32_t valueId, uint32_t bcOffset) {
        out_.framepoints.push_back({
            valueId,
            static_cast<uint16_t>(bcOffset),
            // Copy the operand list (which IS the simulated stack
            // bottom→top at this point — that's how the existing
            // ExitSend bail mechanism transfers it to the interpreter).
            out_.values[valueId].operands,
        });
    }

private:
    const uint8_t*         bc_;
    size_t                 len_;
    Method&                out_;
    uint32_t               currentBlock_ = 0;
    std::vector<uint32_t>  stack_;  // Value ids in stack order
    // ExtendA / ExtendB prefix state.  Cleared after the consuming op.
    int                    pendingExtA_ = 0;
    int                    pendingExtB_ = 0;
    // Start offset of the current "logical instruction" — includes any
    // leading ExtendA/B prefix bytes.  Used for send-bail state.ip so
    // the interpreter re-processes the Extend prefix when resuming
    // (otherwise it re-dispatches the send with stale ext regs = 0).
    size_t                 currentInstrStart_ = 0;
    // Arg counts for SpecialSend opcodes 0x70-0x7F (if known).
    const uint8_t*         specialSendArgCount_ = nullptr;
    // Phase 4 POC: bitmap of literals[0..15] that hold #yourself.
    uint16_t               inlineableSelectorMask_ = 0;
    // Phase 4 POC #2: bitmap of literals[0..15] that hold #==.
    uint16_t               identityEqSelectorMask_ = 0;
    // Phase 4 POC #3: bitmap of literals[0..15] that hold #~~.
    uint16_t               identityNeqSelectorMask_ = 0;
    // Phase 3: emit kPrimTagCheckInt before inlined arith ops.
    bool                   typeCheckArith_ = false;
};

}  // namespace

LiftResult Builder::buildFromBytes(const uint8_t* bc, size_t len,
                                     uint32_t numArgs, uint32_t numTemps,
                                     Method& out,
                                     uint32_t* failedAtBytecode) {
    LinearLifter l(bc, len, numArgs, numTemps, out);
    return l.run(failedAtBytecode);
}

// Read SpecialSelectorsArray from the image and extract arg counts for
// the 16 SpecialSend opcodes (0x70-0x7F → selector indices 16..31).
// Layout: [sel0, argCount0, sel1, argCount1, ...] — 2 slots per index.
// Returns false if the array is missing or malformed.
static bool readSpecialSelectorArgCounts(ObjectMemory& memory,
                                          uint8_t out[16]) {
    Oop ssArray = memory.specialObject(SpecialObjectIndex::SpecialSelectorsArray);
    if (!ssArray.isObject()) return false;
    ObjectHeader* hdr = ssArray.asObjectPtr();
    size_t slots = hdr->slotCount();
    for (int N = 0; N < 16; N++) {
        size_t argSlot = (16 + N) * 2 + 1;
        if (argSlot >= slots) return false;
        Oop ac = hdr->slotAt(argSlot);
        if (!ac.isSmallInteger()) return false;
        int64_t v = ac.asSmallInteger();
        if (v < 0 || v > 255) return false;
        out[N] = (uint8_t)v;
    }
    return true;
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
    size_t totalBytes = mh->byteSize();
    size_t slotBytes  = (1 + numLiterals) * 8;
    size_t bytecodeSize = (totalBytes > slotBytes) ? (totalBytes - slotBytes) : 0;

    out.compiledMethodOop = compiledMethod;
    // Cache literals (for now just the raw Oop array).
    out.literals.clear();
    out.literals.reserve(numLiterals);
    for (uint32_t i = 0; i < numLiterals; i++) {
        out.literals.push_back(memory.fetchPointer(1 + i, compiledMethod));
    }

    // Resolve SpecialSend arg counts from the image so the lifter can
    // handle 0x70-0x7F correctly.  If the array is missing / malformed,
    // SpecialSend ops will bail as unsupported (fallback behavior).
    uint8_t ssArgCounts[16];
    bool haveSS = readSpecialSelectorArgCounts(memory, ssArgCounts);

    // Phase 4 POC: scan first 16 literals for #yourself.  Send0 / Send1
    // / Send2 use 4-bit literal indices (0-15), so this bitmap is
    // sufficient.  Behind PHARO_SISTA_INLINE_YOURSELF=1 because it is
    // unsafe for any class that overrides #yourself; class-hierarchy
    // analysis lands later (Phase 7).
    uint16_t inlineableMask = 0;
    uint16_t identityEqMask = 0;
    uint16_t identityNeqMask = 0;
    static const bool inlineYourself =
        std::getenv("PHARO_SISTA_INLINE_YOURSELF") != nullptr;
    static const bool inlineIdentityEq =
        std::getenv("PHARO_SISTA_INLINE_IDENTITY_EQ") != nullptr;
    if (inlineYourself || inlineIdentityEq) {
        const uint32_t scanLimit = std::min(numLiterals, 16u);
        for (uint32_t i = 0; i < scanLimit; i++) {
            Oop lit = out.literals[i];
            if (!lit.isObject()) continue;
            ObjectHeader* litHdr = lit.asObjectPtr();
            size_t bs = litHdr->byteSize();
            const uint8_t* bytes = litHdr->bytes();
            if (inlineYourself && bs == 8
                && std::memcmp(bytes, "yourself", 8) == 0) {
                inlineableMask |= (1u << i);
            }
            // #== / #~~ are 2 bytes each.  Symbols stored verbatim;
            // compare raw bytes.  Same env gate covers both.
            if (inlineIdentityEq && bs == 2) {
                if (std::memcmp(bytes, "==", 2) == 0) {
                    identityEqMask |= (1u << i);
                } else if (std::memcmp(bytes, "~~", 2) == 0) {
                    identityNeqMask |= (1u << i);
                }
            }
        }
    }

    static const bool typeCheckArith =
        std::getenv("PHARO_SISTA_INLINE_ARITH") != nullptr;

    LinearLifter l(bytecodes, bytecodeSize, numArgs, numTemps, out);
    if (haveSS) l.setSpecialSendArgCounts(ssArgCounts);
    if (inlineableMask) l.setInlineableSelectorBitmap(inlineableMask);
    if (identityEqMask) l.setIdentityEqSelectorBitmap(identityEqMask);
    if (identityNeqMask) l.setIdentityNeqSelectorBitmap(identityNeqMask);
    if (typeCheckArith) l.setTypeCheckArith(true);
    return l.run(failedAtBytecode);
}

}  // namespace sista
}  // namespace pharo
