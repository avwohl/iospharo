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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>
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

// Phase 4 Step 1 cumulative counters.  Declared before LinearLifter so
// the lifter's recordFramepoint() can reference them.  Survive across
// Builder::build() invocations within a process.
static uint64_t g_totalSendsLifted      = 0;
static uint64_t g_totalMonomorphicHints = 0;
static uint64_t g_totalHintsConsumed    = 0;

// Phase 4 Step 2 counters: probe-lift the callee at each consumed hint
// and report whether Sista's lifter can actually handle its bytecode.
// No IR is spliced — these are pure observability for prioritizing
// Step 3 splicing work.
static uint64_t g_calleeLiftAttempts    = 0;
static uint64_t g_calleeLiftSuccess     = 0;
static uint64_t g_calleeBytecodesLifted = 0;

// Phase 4 Step 3 counters: actual inlines emitted (kGuardClass +
// substituted constant) replacing kSendUnspeculated.
static uint64_t g_inlinesEmitted        = 0;

// Hint pointer set by buildWithHints() and consumed by build() when
// constructing LinearLifter.  Single-threaded; static suffices.
static const std::vector<InlineHint>* g_currentBuildHints = nullptr;

// Memory pointer set by buildWithHints() so recordFramepoint can probe-lift
// callees referenced by inline hints.  Single-threaded; static suffices.
static ObjectMemory* g_currentBuildMemory = nullptr;

// Recursion guard: probe-lifting a callee re-enters Builder::build, which
// re-enters recordFramepoint via its own sends.  Without a guard the
// counters explode geometrically and the build wedges.  Bumped to 2 to
// allow one level of recursive inlining (caller → callee → inner callee).
static int g_calleeLiftDepth = 0;

// B2 splice: when set, sub-lifts treat BlockReturnTop / BlockReturnNil
// as a local kReturn rather than bailing.  Set only across recursive
// Builder::build calls from the splice pre-pass.
static bool g_subLiftAsBlockReturnLocal = false;

// Phase 4 Step 5: callback that turns a CompiledMethod oop into its
// inline-hint vector by reading the JIT runtime's IC table.  Set once
// at VM startup by the Interpreter; nullptr disables recursive inlining.
static Builder::HintProvider g_hintProvider;

// RAII saver: temporarily clear g_currentBuildHints across a nested
// build so the inner build doesn't apply OUTER caller hints to its
// own bcOffsets (which refer to a different bytecode stream).
struct ClearOuterHints {
    const std::vector<InlineHint>* saved;
    ClearOuterHints() : saved(g_currentBuildHints) {
        g_currentBuildHints = nullptr;
    }
    ~ClearOuterHints() { g_currentBuildHints = saved; }
};

class LinearLifter {
public:
    LinearLifter(const uint8_t* bc, size_t len,
                  uint32_t numArgs, uint32_t numTemps,
                  Method& out)
        : bc_(bc), len_(len), out_(out) {
        out_.numArgs  = numArgs;
        out_.numTemps = numTemps;
    }

    // For B2 splice's block-body sub-lift.  Passed in by Builder::build
    // so the peephole has memory access without setting the
    // process-wide g_currentBuildMemory (which would also enable
    // probe-lifting in tryInlineConstReturn / recordFramepoint and
    // add unwanted compile-time overhead).
    void setMemory(ObjectMemory* memory) { memory_ = memory; }

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

    // B2 splice: bitmap of literals that hold the #inject:into:
    // selector.  The splice pre-pass uses this to detect Send2
    // bytecodes whose selector matches.
    void setInjectIntoSelectorBitmap(uint16_t mask) {
        injectIntoSelectorMask_ = mask;
    }

    // B2 splice (Interval variant): bitmap of literals that hold #to:.
    void setToSelectorBitmap(uint16_t mask) {
        toSelectorMask_ = mask;
    }

    // Phase 3 deopt: when set, builder emits kPrimTagCheckInt
    // before each kPrimAddInt etc.  Lets the unsafe-arith gate
    // lift safely (non-SmallInt operands deopt to interpreter).
    void setTypeCheckArith(bool v) { typeCheckArith_ = v; }

    // B2 splice sub-lift mode: treat BlockReturnTop / BlockReturnNil
    // as a local kReturn rather than bailing.  Safe because the splice
    // intercept discards the block's return value (do: ignores it).
    // NLR cases (^ from inside a block, escaping the outer method) are
    // still caught — a `^` compiles to ReturnTop within the block's
    // bytecode but not as BlockReturnNil/Top.
    void setBlockReturnAsLocalReturn(bool v) {
        blockReturnAsLocalReturn_ = v;
    }

    // Phase 4 Step 1: profile-guided inline hints from T1 IC.
    void setInlineHints(const std::vector<InlineHint>* hints) {
        inlineHints_ = hints;
    }

    LiftResult run(uint32_t* failedAtBytecode) {
        // --- Pre-pass: bytecode-level pattern detection -----------------
        //
        // Scans the method's bytecode for `PushFullBlock + SpecialSend(do:)`.
        // The IR-level detector (detectDoBlockPattern) misses this on
        // methods like `runSum` because Sista terminates each lifted
        // block at the first kSendUnspeculated — methods with earlier
        // sends never reach the do: site at lift time.  The bytecode
        // scan finds the pattern regardless of basic-block structure.
        //
        // Today: detection only (logs the pattern + IC hint when found).
        // Future B2 splice work: when matched + IC class is Array, emit
        // a specialized counted at: loop with the lifted block body
        // spliced inline, in place of the entire (or post-setup
        // portion of) method's IR.
        {
            static const bool detect =
                std::getenv("PHARO_SISTA_DO_DETECT") != nullptr;
            if (detect) {
                static int patternCount = 0;
                // Find sequences `PushFullBlock(0xF9 _ _) + SpecialSend(do:=0x7B)`.
                // Other selectors are ignored — `value:`/`value`/etc. ARE
                // also bench-relevant but do: is the canonical "iterate
                // a collection" selector and the highest-impact target.
                size_t i = 0;
                int lastFullBlockEnd = -1;
                int lastFullBlockLitIdx = -1;
                int lastFullBlockFlags = -1;
                int extA = 0;
                int extB = 0;
                while (i < len_) {
                    uint8_t op = bc_[i];
                    if (op == jit::SistaV1::ExtendA) {
                        if (i + 1 >= len_) break;
                        extA = bc_[i + 1];
                        i += 2;
                        continue;
                    }
                    if (op == jit::SistaV1::ExtendB) {
                        if (i + 1 >= len_) break;
                        extB = (int8_t)bc_[i + 1];
                        i += 2;
                        continue;
                    }
                    if (op == jit::SistaV1::PushFullBlock) {
                        if (i + 2 >= len_) break;
                        lastFullBlockLitIdx = (extA << 8) | bc_[i + 1];
                        lastFullBlockFlags = bc_[i + 2];
                        lastFullBlockEnd = (int)(i + 3);
                        extA = 0;
                        extB = 0;
                        i += 3;
                        continue;
                    }
                    // Reset block-tracking on any non-Push that intervenes
                    // before the send — but only consider the immediate
                    // [PushFullBlock; specialSend do:] adjacency for now.
                    if (op == 0x7B  // SpecialSend do:
                        && lastFullBlockEnd == (int)i) {
                        if (patternCount++ < 32) {
                            int numCopied = lastFullBlockFlags & 0x3F;
                            bool recvOnStack = ((lastFullBlockFlags >> 7) & 1) != 0;

                            // Look up block oop in method literals.
                            // out_.literals was populated by Builder::build
                            // before run() was called.
                            uint64_t blockOopBits = 0;
                            int blockBytecodeLen = -1;
                            int blockNumArgs = -1;
                            int blockNumTemps = -1;
                            if (lastFullBlockLitIdx >= 0
                                && (size_t)lastFullBlockLitIdx
                                   < out_.literals.size()) {
                                Oop blockOop =
                                    out_.literals[lastFullBlockLitIdx];
                                blockOopBits = blockOop.rawBits();
                                if (blockOop.isObject()) {
                                    ObjectHeader* blockHdr =
                                        blockOop.asObjectPtr();
                                    if (blockHdr->isCompiledMethod()) {
                                        // CompiledBlock has same shape:
                                        // header SmI + literals + bytecode.
                                        // Read slot 0 directly from the
                                        // byte body (first 8 bytes is the
                                        // header Oop bits).
                                        const uint8_t* bs =
                                            blockHdr->bytes();
                                        size_t total =
                                            blockHdr->byteSize();
                                        if (total >= 8) {
                                            uint64_t hbits = 0;
                                            std::memcpy(&hbits, bs, 8);
                                            Oop hdr = Oop::fromRawBits(hbits);
                                            if (hdr.isSmallInteger()) {
                                                int64_t hb =
                                                    hdr.asSmallInteger();
                                                uint32_t numLits =
                                                    (uint32_t)(hb & 0x7FFF);
                                                blockNumArgs =
                                                    (int)((hb >> 24) & 0x0F);
                                                int storedTemps =
                                                    (int)((hb >> 18) & 0x3F);
                                                blockNumTemps =
                                                    storedTemps
                                                    - blockNumArgs;
                                                size_t headerBytes =
                                                    (1 + numLits) * 8;
                                                blockBytecodeLen =
                                                    (int)(total > headerBytes
                                                          ? total
                                                            - headerBytes
                                                          : 0);
                                            }
                                        }
                                    }
                                }
                            }

                            // Look up IC hint at do: bcOffset for the
                            // receiver class.  Stored as classIndex
                            // (low 22 bits — see extractInlineHintsForMethod).
                            const InlineHint* hit = nullptr;
                            if (inlineHints_) {
                                for (const auto& h : *inlineHints_) {
                                    if (h.bcOffset == (uint32_t)i) {
                                        hit = &h; break;
                                    }
                                }
                            }
                            uint32_t icClassIdx =
                                hit ? (uint32_t)(hit->classOop & 0x3FFFFFu)
                                    : 0;

                            // Try to sub-lift the block body via Builder.
                            // If lift succeeds, the block is a splice
                            // candidate.  Recursion-guarded — block lift
                            // re-enters Builder::build, which would
                            // recurse into our peephole and try to
                            // lift again.  g_calleeLiftDepth gates this.
                            int blockLiftOk = -1;
                            int blockIRSize = -1;
                            int blockIRBlocks = -1;
                            int blockSpliceSimple = -1;
                            if (memory_ != nullptr
                                && g_calleeLiftDepth < 1
                                && lastFullBlockLitIdx >= 0
                                && (size_t)lastFullBlockLitIdx
                                   < out_.literals.size()) {
                                Oop blockOop =
                                    out_.literals[lastFullBlockLitIdx];
                                if (blockOop.isObject()
                                    && blockOop.rawBits() > 0x10000) {
                                    Method blockIR;
                                    uint32_t blockFailedAt = UINT32_MAX;
                                    g_calleeLiftDepth++;
                                    LiftResult br;
                                    {
                                        ClearOuterHints g;
                                        br = Builder::build(blockOop,
                                            *memory_,
                                            blockIR, &blockFailedAt);
                                    }
                                    g_calleeLiftDepth--;
                                    blockLiftOk = (br == LiftResult::kOk)
                                                   ? 1 : 0;
                                    if (blockLiftOk) {
                                        blockIRSize =
                                            (int)blockIR.values.size();
                                        blockIRBlocks =
                                            (int)blockIR.blocks.size();
                                        // Splice-eligibility: block body
                                        // is "simple" if every op is
                                        // splice-friendly.  No remote
                                        // temps, no nested kBlockCreate,
                                        // no kSendUnspeculated, no NLR.
                                        blockSpliceSimple = 1;
                                        for (const auto& bv :
                                             blockIR.values) {
                                            switch (bv.op) {
                                            case Op::kLoadReceiver:
                                            case Op::kLoadTrueOop:
                                            case Op::kLoadFalseOop:
                                            case Op::kLoadTemp:
                                            case Op::kLoadLiteral:
                                            case Op::kConstantOop:
                                            case Op::kPhi:
                                            case Op::kReturn:
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
                                            default:
                                                blockSpliceSimple = 0;
                                                break;
                                            }
                                            if (!blockSpliceSimple) break;
                                        }
                                    }
                                }
                            }

                            std::fprintf(stderr,
                                "[SISTA-DO-PATTERN] doBcOffset=%zu "
                                "blockLit=%d numCopied=%d rcvOnStack=%d "
                                "methodLen=%zu blockOop=0x%llx "
                                "blockNumArgs=%d blockNumTemps=%d "
                                "blockBcLen=%d icClassIdx=%u hasHint=%d "
                                "blockLiftOk=%d blockIRValues=%d "
                                "blockIRBlocks=%d spliceSimple=%d\n",
                                i, lastFullBlockLitIdx, numCopied,
                                (int)recvOnStack, len_,
                                (unsigned long long)blockOopBits,
                                blockNumArgs, blockNumTemps,
                                blockBytecodeLen, icClassIdx,
                                hit ? 1 : 0, blockLiftOk,
                                blockIRSize, blockIRBlocks,
                                blockSpliceSimple);
                        }
                        lastFullBlockEnd = -1;
                    } else {
                        // Adjacency lost — block was consumed by some
                        // other op (a store, push, etc.).  Reset.
                        if (op != jit::SistaV1::ExtendA
                            && op != jit::SistaV1::ExtendB) {
                            lastFullBlockEnd = -1;
                        }
                    }
                    extA = 0;
                    extB = 0;
                    // Skip the bytecode by its length.  Conservative:
                    // 1 for normal, 2 for ext2byte, 3 for the f8-fd
                    // range.  Out-of-range escape opcodes are 1-byte.
                    if (jit::SistaV1::isThreeByteBytecode(op)) i += 3;
                    else if (jit::SistaV1::isExtended2Byte(op)) i += 2;
                    else i++;
                }
            }
        }

        // --- B2 splice pre-pass: identify Array do: candidates ----------
        //
        // Same scan as the detector above, but always-on under
        // PHARO_SISTA_DO_SPLICE=1.  For each PushFullBlock+SpecialSend(do:)
        // adjacency where:
        //   - the IC hint at the do: bcOffset says Array receiver
        //   - the block sub-lifts to splice-simple IR
        // we sub-lift the block, store its IR into out_.inlinedBlocks,
        // and record the PushFullBlock's bcOffset → block-IR-slot
        // mapping in spliceAtPushFullBlock_.  The main lift's
        // PushFullBlock arm consults this map and emits kCountedLoopDo
        // when it matches.
        {
            static const bool splice =
                std::getenv("PHARO_SISTA_DO_SPLICE") != nullptr;
            if (splice && memory_ != nullptr) {
                size_t i = 0;
                int lastPushFullBlockEnd = -1;
                int lastPushFullBlockStart = -1;
                int lastFullBlockLitIdx = -1;
                int lastFullBlockFlags = -1;
                int extA = 0;
                // Track whether a "lift-terminator" bytecode (one that
                // makes the main lifter return kOk early) has been seen.
                // Once true, no candidate further in the stream can
                // actually compile — the main lift won't reach it.
                bool sawLiftTerminator = false;
                while (i < len_) {
                    uint8_t op = bc_[i];
                    // Detect terminators that end the lift before our
                    // PushFullBlock could be reached.  Today the lifter
                    // returns kOk on:
                    //   - regular literal sends (Send0/1/2: 0x80-0xAF)
                    //   - SpecialSend other than the do: peephole (0x70-0x7F)
                    //   - extended sends (ExtendedSend 0xEA, ExtendedSendSuper 0xEB)
                    //   - non-inlined arith ops (0x60-0x6F) when the
                    //     PHARO_SISTA_INLINE_ARITH gate is OFF
                    //   - PushClosure (0xFA), PushArray (0xF8), PushThisContext
                    //   - MUSTBOOL (0xFD)
                    // Conservative: mark all of these as terminators.
                    // This means we'll skip a few candidates that arith-
                    // inlining would actually allow, but it's the safe
                    // direction.
                    if ((op >= 0x60 && op <= 0xAF)
                        || op == 0xEA  // ExtendedSend
                        || op == 0xEB  // ExtendedSendSuper
                        || op == 0xF8  // PushArray
                        || op == 0xFA  // PushClosure
                        || op == 0xFD  // MUSTBOOL
                        || op == 0xFC) // PushThisContext
                    {
                        // Allowed: do: itself (0x7B) when we're in the
                        // adjacency window — that's the candidate
                        // signal, not a terminator.
                        if (!(op == 0x7B
                              && lastPushFullBlockEnd == (int)i)) {
                            sawLiftTerminator = true;
                        }
                    }
                    if (op == jit::SistaV1::ExtendA) {
                        if (i + 1 >= len_) break;
                        extA = bc_[i + 1];
                        i += 2;
                        continue;
                    }
                    if (op == jit::SistaV1::ExtendB) {
                        if (i + 1 >= len_) break;
                        i += 2;
                        continue;
                    }
                    if (op == jit::SistaV1::PushFullBlock) {
                        if (i + 2 >= len_) break;
                        lastPushFullBlockStart = (int)i;
                        lastFullBlockLitIdx = (extA << 8) | bc_[i + 1];
                        lastFullBlockFlags = bc_[i + 2];
                        lastPushFullBlockEnd = (int)(i + 3);
                        extA = 0;
                        i += 3;
                        continue;
                    }
                    if (op == 0x7B  // SpecialSend do:
                        && lastPushFullBlockEnd == (int)i) {
                        // Found a PushFullBlock+SpecialSend(do:) pair.
                        // Trace the rejection reason for the first 16
                        // patterns so we can diagnose 0 candidates.
                        static int diagCount = 0;
                        bool diag = (diagCount < 16);
                        // Check the block's bytecode length sanity, then
                        // sub-lift.
                        bool ok = true;
                        const char* rejectReason = nullptr;

                        // Reject candidates that the main lifter cannot
                        // reach (a terminator-send appears earlier).  No
                        // point lifting the block sub-IR if the splice
                        // intercept will never fire.
                        if (sawLiftTerminator) {
                            ok = false;
                            rejectReason = "lift-terminator before do:";
                        }

                        // Look up the block's CompiledBlock literal.
                        Oop blockOop = Oop::nil();
                        if (lastFullBlockLitIdx < 0
                            || (size_t)lastFullBlockLitIdx
                               >= out_.literals.size()) {
                            ok = false;
                            rejectReason = "bad litIdx";
                        } else {
                            blockOop = out_.literals[lastFullBlockLitIdx];
                            if (!blockOop.isObject()) {
                                ok = false;
                                rejectReason = "block not Object";
                            }
                        }

                        // numCopied is the lower 6 bits of flags;
                        // recv-on-stack is bit 7 (we don't yet handle).
                        bool receiverOnStack =
                            ((lastFullBlockFlags >> 7) & 1) != 0;
                        if (ok && receiverOnStack) {
                            ok = false;
                            rejectReason = "recvOnStack";
                        }

                        // IC hint check is preferred but not required.
                        // When PHARO_SISTA_DO_SPLICE_NO_HINT=1, we
                        // splice optimistically and let the lowering's
                        // runtime class check + deopt-on-miss handle
                        // non-Array receivers.  When unset, require
                        // an IC hint that says Array.
                        static const bool noHintMode = []() {
                            const char* v = std::getenv(
                                "PHARO_SISTA_DO_SPLICE_NO_HINT");
                            return v && v[0] == '1';
                        }();
                        const InlineHint* hit = nullptr;
                        uint32_t hintClassIdx = 0;
                        if (!noHintMode) {
                            if (ok && inlineHints_) {
                                for (const auto& h : *inlineHints_) {
                                    if (h.bcOffset == (uint32_t)i) {
                                        hit = &h;
                                        break;
                                    }
                                }
                                if (!hit) {
                                    ok = false;
                                    rejectReason = "no IC hint";
                                }
                            } else if (ok && !inlineHints_) {
                                ok = false;
                                rejectReason = "no hints provided";
                            }
                            if (ok && hit) {
                                hintClassIdx =
                                    (uint32_t)(hit->classOop & 0x3FFFFFu);
                                // 47 = Array's classIdx in standard
                                // Pharo class table.
                                if (hintClassIdx != 47) {
                                    ok = false;
                                    rejectReason = "non-Array IC class";
                                }
                            }
                        }

                        // Sub-lift the block.  Recursion-guarded
                        // through the same g_calleeLiftDepth gate the
                        // detector uses.  The blockReturnAsLocal flag
                        // turns BlockReturnTop / BlockReturnNil into a
                        // local kReturn (the splice intercept discards
                        // the block's return value anyway).
                        std::unique_ptr<Method> blockIR;
                        if (ok && g_calleeLiftDepth < 1) {
                            blockIR = std::make_unique<Method>();
                            g_calleeLiftDepth++;
                            bool savedBR = g_subLiftAsBlockReturnLocal;
                            g_subLiftAsBlockReturnLocal = true;
                            uint32_t failedBc = UINT32_MAX;
                            LiftResult r = Builder::build(
                                blockOop, *memory_, *blockIR, &failedBc);
                            g_subLiftAsBlockReturnLocal = savedBR;
                            g_calleeLiftDepth--;
                            if (r != LiftResult::kOk) {
                                ok = false;
                                rejectReason = "block sub-lift failed";
                            }
                        } else if (ok) {
                            ok = false;  // recursion guard
                            rejectReason = "recursion guard";
                        }

                        // Splice-simple check: every value in the
                        // block must be a "splice-friendly" op so the
                        // lowering can emit it inline without
                        // reconstructing frame state.
                        Op rejectedOp = Op::kReturn;
                        if (ok && blockIR) {
                            for (const auto& bv : blockIR->values) {
                                switch (bv.op) {
                                case Op::kLoadReceiver:
                                case Op::kLoadTrueOop:
                                case Op::kLoadFalseOop:
                                case Op::kLoadTemp:
                                case Op::kLoadLiteral:
                                case Op::kConstantOop:
                                case Op::kPhi:
                                case Op::kReturn:
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
                                case Op::kPrimTagCheckInt:
                                    // Tag-check is harmless at the splice
                                    // level (lowering passes it through
                                    // and does its own check at the do:
                                    // bcOffset).
                                    break;
                                default:
                                    ok = false;
                                    rejectReason = "non-simple block op";
                                    rejectedOp = bv.op;
                                    break;
                                }
                                if (!ok) break;
                            }
                        }

                        if (diag) {
                            diagCount++;
                            std::fprintf(stderr,
                                "[SISTA-SPLICE-DIAG] doBC=%zu litIdx=%d "
                                "icCls=%u verdict=%s reject=%s rejOp=%s\n",
                                i, lastFullBlockLitIdx, hintClassIdx,
                                ok ? "OK" : "REJECT",
                                rejectReason ? rejectReason : "(none)",
                                ok ? "n/a" : OpInfo::name(rejectedOp));
                        }

                        // Eligible: stash the block IR and remember
                        // this PushFullBlock's offset so the main lift
                        // can consult.
                        if (ok && blockIR) {
                            uint32_t slot = static_cast<uint32_t>(
                                out_.inlinedBlocks.size());
                            out_.inlinedBlocks.push_back(
                                std::move(blockIR));
                            spliceAtPushFullBlock_[
                                (size_t)lastPushFullBlockStart] = slot;
                            static int spliceCount = 0;
                            if (spliceCount++ < 16) {
                                std::fprintf(stderr,
                                    "[SISTA-SPLICE-CAND] pushFullBlock=%d "
                                    "doBC=%zu litIdx=%d slot=%u\n",
                                    lastPushFullBlockStart, i,
                                    lastFullBlockLitIdx, slot);
                            }
                        }

                        lastPushFullBlockEnd = -1;
                        i++;
                        continue;
                    }
                    if (op != jit::SistaV1::ExtendA
                        && op != jit::SistaV1::ExtendB) {
                        lastPushFullBlockEnd = -1;
                    }
                    if (jit::SistaV1::isThreeByteBytecode(op)) i += 3;
                    else if (jit::SistaV1::isExtended2Byte(op)) i += 2;
                    else i++;
                }
            }
        }

        // --- B2 splice pre-pass: closure-accumulator (Array do:) -------
        //
        // Detects the dominant accumulator idiom: `arr do: [:e | s := s + e]`
        // where s is an outer-method temp captured via TempVector.
        //
        // Block must have numCopied=1 and EXACTLY this 5-instruction shape:
        //   0xFB slot 0x01      pushTempAtInVec(slot, vecAt 1)
        //   0x40                pushTemp 0  (block arg `e`)
        //   0x60 / 0x61 / 0x68  ArithAdd / Sub / Mul
        //   0xFC slot 0x01      storeIntoTemp inVec (slot, vecAt 1)
        //   0x5D or 0x5E        BlockReturnNil / BlockReturnTop
        //
        // Outer must:
        //   - Push the receiver (any single-byte op, e.g. PushTemp T_arr)
        //   - Push the TempVector via PushTemp T_vec (single-byte 0x40-0x4B)
        //   - PushFullBlock(numCopied=1) at offset pfbOff
        //   - SpecialSend do: at pfbOff+3
        //
        // We extract:
        //   outerVecTemp = (op_at_pfbOff_minus_1 - 0x40)
        //   slot         = block byte 1
        //   arithOp      = block byte 2 (mapped: 0x60→0, 0x61→1, 0x68→2)
        //
        // Records spliceAccumAtPushFullBlock_[pfbOff] = packed metadata
        // (slot 0-7 in low 8 bits, arithCode 0-2 in next 8 bits, outerTemp
        // 0-15 in next 8 bits).
        static const bool accumSplice =
            std::getenv("PHARO_SISTA_DO_SPLICE") != nullptr;
        if (accumSplice && memory_ != nullptr) {
            size_t i = 0;
            int extA = 0;
            while (i < len_) {
                uint8_t op = bc_[i];
                if (op == jit::SistaV1::ExtendA) {
                    if (i + 1 >= len_) break;
                    extA = bc_[i + 1];
                    i += 2;
                    continue;
                }
                if (op == jit::SistaV1::ExtendB) {
                    if (i + 1 >= len_) break;
                    i += 2;
                    continue;
                }
                if (op == jit::SistaV1::PushFullBlock) {
                    if (i + 3 >= len_) break;
                    size_t pfbOff = i;
                    int litIdx = (extA << 8) | bc_[i + 1];
                    int flags = bc_[i + 2];
                    extA = 0;
                    // Need numCopied = 1 and not recvOnStack.
                    bool recvOnStack = ((flags >> 7) & 1) != 0;
                    int numCopied = flags & 0x3F;
                    if (recvOnStack || numCopied != 1) {
                        i += 3;
                        continue;
                    }
                    // Need SpecialSend do: at pfbOff+3.
                    if (pfbOff + 3 >= len_ || bc_[pfbOff + 3] != 0x7B) {
                        i += 3;
                        continue;
                    }
                    // Need PushTemp T at pfbOff-1 (single byte 0x40..0x4B).
                    if (pfbOff < 1) { i += 3; continue; }
                    uint8_t prevOp = bc_[pfbOff - 1];
                    if (prevOp < 0x40 || prevOp > 0x4B) {
                        // Could also accept ExtPushTemp (0xE5 idx) but
                        // skip for MVP.
                        i += 3;
                        continue;
                    }
                    uint32_t outerVecTemp = prevOp - 0x40;

                    // Look up the block's bytecode and validate the
                    // 5-instruction accumulator shape.
                    if (litIdx < 0
                        || (size_t)litIdx >= out_.literals.size()) {
                        i += 3;
                        continue;
                    }
                    Oop blockOop = out_.literals[litIdx];
                    if (!blockOop.isObject()) { i += 3; continue; }
                    ObjectHeader* blockHdr = blockOop.asObjectPtr();
                    if (!blockHdr->isCompiledMethod()) { i += 3; continue; }
                    const uint8_t* bs = blockHdr->bytes();
                    size_t total = blockHdr->byteSize();
                    if (total < 8) { i += 3; continue; }
                    uint64_t hbits = 0;
                    std::memcpy(&hbits, bs, 8);
                    Oop hdr = Oop::fromRawBits(hbits);
                    if (!hdr.isSmallInteger()) { i += 3; continue; }
                    int64_t hb = hdr.asSmallInteger();
                    uint32_t bNumLits = (uint32_t)(hb & 0x7FFF);
                    size_t bHdrBytes = (1 + bNumLits) * 8;
                    if (total <= bHdrBytes) { i += 3; continue; }
                    const uint8_t* bbc = bs + bHdrBytes;
                    size_t bLen = total - bHdrBytes;

                    // Pattern: FB slot 01 | 40 | 60|61|68 | FC slot 01
                    //          | 5D or 5E [optional alignment pad]
                    if (bLen < 9 || bLen > 10) {
                        static int dCount = 0;
                        if (dCount++ < 16) {
                            std::fprintf(stderr,
                                "[SISTA-ACCUM-DIAG] pfbBC=%zu "
                                "blockLen=%zu (want 9-10)\n",
                                pfbOff, bLen);
                        }
                        i += 3;
                        continue;
                    }
                    if (bbc[0] != 0xFB || bbc[2] != 0x01) {
                        i += 3; continue;
                    }
                    if (bbc[3] != 0x40) { i += 3; continue; }
                    uint8_t arithB = bbc[4];
                    int arithCode = -1;
                    if (arithB == 0x60) arithCode = 0;       // +
                    else if (arithB == 0x61) arithCode = 1;  // -
                    else if (arithB == 0x68) arithCode = 2;  // *
                    if (arithCode < 0) { i += 3; continue; }
                    if (bbc[5] != 0xFC) { i += 3; continue; }
                    if (bbc[7] != 0x01) { i += 3; continue; }
                    // bbc[6] (slot in store) must equal bbc[1] (slot in load).
                    if (bbc[6] != bbc[1]) { i += 3; continue; }
                    if (bbc[8] != 0x5D && bbc[8] != 0x5E) {
                        i += 3; continue;
                    }
                    uint32_t slot = bbc[1];

                    uint64_t packed = (uint64_t)slot
                                    | ((uint64_t)arithCode << 8)
                                    | ((uint64_t)outerVecTemp << 16);
                    spliceAccumAtPushFullBlock_[pfbOff] = packed;
                    static int aCount = 0;
                    if (aCount++ < 16) {
                        std::fprintf(stderr,
                            "[SISTA-ACCUM-CAND] pfbBC=%zu doBC=%zu "
                            "outerTemp=%u slot=%u arith=%d\n",
                            pfbOff, pfbOff + 3, outerVecTemp, slot,
                            arithCode);
                    }
                    i += 3;
                    continue;
                }
                if (op != jit::SistaV1::ExtendA
                    && op != jit::SistaV1::ExtendB) {
                    extA = 0;
                }
                if (jit::SistaV1::isThreeByteBytecode(op)) i += 3;
                else if (jit::SistaV1::isExtended2Byte(op)) i += 2;
                else i++;
            }
        }

        // --- B2 splice pre-pass: identify `inject:into:` candidates -----
        //
        // Pattern: PushFullBlock + Send2 with the literal selector
        // matching #inject:into: (literal index in injectIntoSelectorMask_
        // bitmap).  Block has 2 args (acc, elem); body must be
        // splice-simple and produce a return value (the new acc).
        //
        // No closure-store complications since inject:into:'s
        // accumulator is the block's RETURN value, not a captured
        // mutable temp.
        static const bool injectSplice =
            std::getenv("PHARO_SISTA_DO_SPLICE") != nullptr;
        if (injectSplice && memory_ != nullptr && injectIntoSelectorMask_) {
            size_t i = 0;
            int lastPushFullBlockEnd = -1;
            int lastPushFullBlockStart = -1;
            int lastFullBlockLitIdx = -1;
            int lastFullBlockFlags = -1;
            int extA = 0;
            while (i < len_) {
                uint8_t op = bc_[i];
                if (op == jit::SistaV1::ExtendA) {
                    if (i + 1 >= len_) break;
                    extA = bc_[i + 1];
                    i += 2;
                    continue;
                }
                if (op == jit::SistaV1::ExtendB) {
                    if (i + 1 >= len_) break;
                    i += 2;
                    continue;
                }
                if (op == jit::SistaV1::PushFullBlock) {
                    if (i + 2 >= len_) break;
                    lastPushFullBlockStart = (int)i;
                    lastFullBlockLitIdx = (extA << 8) | bc_[i + 1];
                    lastFullBlockFlags = bc_[i + 2];
                    lastPushFullBlockEnd = (int)(i + 3);
                    extA = 0;
                    i += 3;
                    continue;
                }
                // Send2 with selector == #inject:into: at the
                // adjacency point.
                if (op >= jit::SistaV1::Send2Base
                    && op <= jit::SistaV1::Send2Last
                    && lastPushFullBlockEnd == (int)i) {
                    uint32_t selIdx = op & 0x0F;
                    bool isInjectInto =
                        ((injectIntoSelectorMask_ >> selIdx) & 1) != 0;
                    if (!isInjectInto) {
                        lastPushFullBlockEnd = -1;
                        i++;
                        continue;
                    }
                    bool ok = true;
                    const char* rejectReason = nullptr;
                    Op rejectedOp = Op::kReturn;

                    Oop blockOop = Oop::nil();
                    if (lastFullBlockLitIdx < 0
                        || (size_t)lastFullBlockLitIdx
                           >= out_.literals.size()) {
                        ok = false;
                        rejectReason = "bad litIdx";
                    } else {
                        blockOop = out_.literals[lastFullBlockLitIdx];
                        if (!blockOop.isObject()) {
                            ok = false;
                            rejectReason = "block not Object";
                        }
                    }

                    bool receiverOnStack =
                        ((lastFullBlockFlags >> 7) & 1) != 0;
                    if (ok && receiverOnStack) {
                        ok = false;
                        rejectReason = "recvOnStack";
                    }
                    uint32_t numCopied =
                        (uint32_t)(lastFullBlockFlags & 0x3F);
                    if (ok && numCopied != 0) {
                        // Future: handle captured vars.  MVP only.
                        ok = false;
                        rejectReason = "numCopied != 0";
                    }

                    // Sub-lift the block (with blockReturnAsLocal so
                    // BlockReturnTop becomes kReturn).
                    std::unique_ptr<Method> blockIR;
                    if (ok && g_calleeLiftDepth < 1) {
                        blockIR = std::make_unique<Method>();
                        g_calleeLiftDepth++;
                        bool savedBR = g_subLiftAsBlockReturnLocal;
                        g_subLiftAsBlockReturnLocal = true;
                        uint32_t failedBc = UINT32_MAX;
                        LiftResult r = Builder::build(
                            blockOop, *memory_, *blockIR, &failedBc);
                        g_subLiftAsBlockReturnLocal = savedBR;
                        g_calleeLiftDepth--;
                        if (r != LiftResult::kOk) {
                            ok = false;
                            rejectReason = "block sub-lift failed";
                        }
                    } else if (ok) {
                        ok = false;
                        rejectReason = "recursion guard";
                    }

                    // Verify block is splice-simple (same whitelist as
                    // do: splice — loads + arith + tag-check + return).
                    if (ok && blockIR) {
                        for (const auto& bv : blockIR->values) {
                            switch (bv.op) {
                            case Op::kLoadReceiver:
                            case Op::kLoadTrueOop:
                            case Op::kLoadFalseOop:
                            case Op::kLoadTemp:
                            case Op::kLoadLiteral:
                            case Op::kConstantOop:
                            case Op::kPhi:
                            case Op::kReturn:
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
                            case Op::kPrimTagCheckInt:
                                break;
                            default:
                                ok = false;
                                rejectReason = "non-simple block op";
                                rejectedOp = bv.op;
                                break;
                            }
                            if (!ok) break;
                        }
                    }

                    if (ok && blockIR) {
                        uint32_t slot = static_cast<uint32_t>(
                            out_.inlinedBlocks.size());
                        out_.inlinedBlocks.push_back(
                            std::move(blockIR));
                        spliceInjectAtPushFullBlock_[
                            (size_t)lastPushFullBlockStart] = slot;
                        static int spliceInjectCount = 0;
                        if (spliceInjectCount++ < 16) {
                            std::fprintf(stderr,
                                "[SISTA-INJECT-SPLICE-CAND] "
                                "pushFullBlock=%d send2BC=%zu "
                                "selIdx=%u litIdx=%d slot=%u\n",
                                lastPushFullBlockStart, i,
                                selIdx, lastFullBlockLitIdx, slot);
                        }
                    } else {
                        static int diagCount = 0;
                        if (diagCount++ < 16) {
                            std::fprintf(stderr,
                                "[SISTA-INJECT-SPLICE-DIAG] "
                                "send2BC=%zu selIdx=%u verdict=%s "
                                "reject=%s rejOp=%s\n",
                                i, selIdx, ok ? "OK" : "REJECT",
                                rejectReason ? rejectReason : "(none)",
                                ok ? "n/a" : OpInfo::name(rejectedOp));
                        }
                    }

                    lastPushFullBlockEnd = -1;
                    i++;
                    continue;
                }
                if (op != jit::SistaV1::ExtendA
                    && op != jit::SistaV1::ExtendB) {
                    lastPushFullBlockEnd = -1;
                }
                if (jit::SistaV1::isThreeByteBytecode(op)) i += 3;
                else if (jit::SistaV1::isExtended2Byte(op)) i += 2;
                else i++;
            }
        }

        // --- B2 splice pre-pass: Interval-inject pattern --------------
        //
        // Detects: <push start> <push stop> Send1#to:
        //          <push init>  PushFullBlock Send2#inject:into:
        //
        // Records the offset of Send1#to: so the main lift emits
        // kInterval (instead of kSendUnspeculated) at that point.
        // The existing inject:into: intercept at PushFullBlock then
        // upgrades kCountedLoopInjectInto to
        // kCountedLoopIntervalInjectInto when it sees the kInterval
        // marker as receiver.
        //
        // Only emits kInterval if the entire pattern is verified —
        // a stray kInterval that doesn't get consumed by the
        // inject:into: intercept would have no lowering and yield
        // garbage downstream.
        if (injectSplice && memory_ != nullptr
            && injectIntoSelectorMask_ && toSelectorMask_) {
            size_t i = 0;
            while (i < len_) {
                uint8_t op = bc_[i];
                // Look for Send1 with selIdx in toSelectorMask_.
                if (op >= jit::SistaV1::Send1Base
                    && op <= jit::SistaV1::Send1Last) {
                    uint32_t selIdx = op & 0x0F;
                    if (((toSelectorMask_ >> selIdx) & 1) != 0) {
                        // Possible to: send.  Check if followed by
                        // <push> + PushFullBlock + Send2#inject:into:.
                        size_t pushInitOff = i + 1;
                        if (pushInitOff >= len_) { i++; continue; }
                        // Push init can be 1 byte (PushZero/One/Receiver
                        // /etc.) or multi-byte (PushIntegerExtended,
                        // ExtPushLit, etc.).  For simplicity we only
                        // handle 1-byte pushes here — covers PushZero,
                        // PushOne, PushReceiver, PushTrue/False/Nil,
                        // PushTempBase 0..15, etc.  Multi-byte init is
                        // future work.
                        uint8_t initOp = bc_[pushInitOff];
                        size_t initLen = jit::SistaV1::isThreeByteBytecode(initOp)
                                       ? 3
                                       : (jit::SistaV1::isExtended2Byte(initOp)
                                          ? 2 : 1);
                        size_t pfbOff = pushInitOff + initLen;
                        if (pfbOff + 3 >= len_
                            || bc_[pfbOff] != jit::SistaV1::PushFullBlock) {
                            i++; continue;
                        }
                        size_t injectOff = pfbOff + 3;
                        uint8_t injectOp = bc_[injectOff];
                        if (injectOp < jit::SistaV1::Send2Base
                            || injectOp > jit::SistaV1::Send2Last) {
                            i++; continue;
                        }
                        uint32_t injectSelIdx = injectOp & 0x0F;
                        if (((injectIntoSelectorMask_ >> injectSelIdx) & 1) == 0) {
                            i++; continue;
                        }

                        // Verify the block IR is splice-simple
                        // (mirrors the existing inject:into:
                        // pre-pass).  If not, skip.
                        int litIdx = bc_[pfbOff + 1];
                        int flags = bc_[pfbOff + 2];
                        bool ok = true;
                        const char* rejectReason = nullptr;
                        Op rejectedOp = Op::kReturn;

                        Oop blockOop = Oop::nil();
                        if (litIdx < 0
                            || (size_t)litIdx
                               >= out_.literals.size()) {
                            ok = false;
                            rejectReason = "bad litIdx";
                        } else {
                            blockOop = out_.literals[litIdx];
                            if (!blockOop.isObject()) {
                                ok = false;
                                rejectReason = "block not Object";
                            }
                        }
                        bool receiverOnStack =
                            ((flags >> 7) & 1) != 0;
                        if (ok && receiverOnStack) {
                            ok = false;
                            rejectReason = "recvOnStack";
                        }
                        uint32_t numCopied =
                            (uint32_t)(flags & 0x3F);
                        if (ok && numCopied != 0) {
                            ok = false;
                            rejectReason = "numCopied != 0";
                        }

                        std::unique_ptr<Method> blockIR;
                        if (ok && g_calleeLiftDepth < 1) {
                            blockIR = std::make_unique<Method>();
                            g_calleeLiftDepth++;
                            bool savedBR = g_subLiftAsBlockReturnLocal;
                            g_subLiftAsBlockReturnLocal = true;
                            uint32_t failedBc = UINT32_MAX;
                            LiftResult r = Builder::build(
                                blockOop, *memory_, *blockIR, &failedBc);
                            g_subLiftAsBlockReturnLocal = savedBR;
                            g_calleeLiftDepth--;
                            if (r != LiftResult::kOk) {
                                ok = false;
                                rejectReason = "block sub-lift failed";
                            }
                        } else if (ok) {
                            ok = false;
                            rejectReason = "recursion guard";
                        }

                        if (ok && blockIR) {
                            for (const auto& bv : blockIR->values) {
                                switch (bv.op) {
                                case Op::kLoadReceiver:
                                case Op::kLoadTrueOop:
                                case Op::kLoadFalseOop:
                                case Op::kLoadTemp:
                                case Op::kLoadLiteral:
                                case Op::kConstantOop:
                                case Op::kPhi:
                                case Op::kReturn:
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
                                case Op::kPrimTagCheckInt:
                                    break;
                                default:
                                    ok = false;
                                    rejectReason = "non-simple block op";
                                    rejectedOp = bv.op;
                                    break;
                                }
                                if (!ok) break;
                            }
                        }

                        if (ok && blockIR) {
                            uint32_t slot = static_cast<uint32_t>(
                                out_.inlinedBlocks.size());
                            out_.inlinedBlocks.push_back(
                                std::move(blockIR));
                            // Record both the to: offset (so the lifter
                            // emits kInterval there) and the
                            // PushFullBlock offset (so the inject
                            // intercept finds the block-IR slot).
                            intervalInjectAtTo_[(size_t)i] = slot;
                            spliceInjectAtPushFullBlock_[
                                (size_t)pfbOff] = slot;
                            static int ivCount = 0;
                            if (ivCount++ < 4) {
                                std::fprintf(stderr,
                                    "[SISTA-IVINJECT-CAND] toBC=%zu "
                                    "pfbBC=%zu injectBC=%zu litIdx=%d "
                                    "slot=%u\n",
                                    i, pfbOff, injectOff, litIdx, slot);
                            }
                        } else {
                            static int ivDiag = 0;
                            if (ivDiag++ < 16) {
                                std::fprintf(stderr,
                                    "[SISTA-IVINJECT-DIAG] toBC=%zu "
                                    "verdict=%s reject=%s rejOp=%s\n",
                                    i, ok ? "OK" : "REJECT",
                                    rejectReason ? rejectReason : "(none)",
                                    ok ? "n/a" : OpInfo::name(rejectedOp));
                            }
                        }
                        // Skip past the to: send so we don't re-scan it.
                        i++;
                        continue;
                    }
                }
                if (jit::SistaV1::isThreeByteBytecode(op)) i += 3;
                else if (jit::SistaV1::isExtended2Byte(op)) i += 2;
                else i++;
            }
        }

        // --- B2 splice pre-pass: Interval-do pattern ------------------
        //
        // Detects: <push start> <push stop> Send1#to:
        //          PushFullBlock SpecialSend(do:=0x7B)
        //
        // Records the offset of Send1#to: so the main lift emits
        // kInterval (intervalDoAtTo_) AND records the PushFullBlock
        // offset so the do: intercept emits kCountedLoopIntervalDo
        // (spliceDoAtPushFullBlock_).
        //
        // Block must be 1-arg (each), numCopied=0, splice-simple.
        if (injectSplice && memory_ != nullptr && toSelectorMask_) {
            size_t i = 0;
            while (i < len_) {
                uint8_t op = bc_[i];
                if (op >= jit::SistaV1::Send1Base
                    && op <= jit::SistaV1::Send1Last) {
                    uint32_t selIdx = op & 0x0F;
                    if (((toSelectorMask_ >> selIdx) & 1) != 0) {
                        // Possible to: send.  Check if followed by
                        // PushFullBlock + SpecialSend(do:).
                        size_t pfbOff = i + 1;
                        if (pfbOff + 3 >= len_
                            || bc_[pfbOff] != jit::SistaV1::PushFullBlock) {
                            i++; continue;
                        }
                        size_t doOff = pfbOff + 3;
                        if (bc_[doOff] != 0x7B) {  // SpecialSend do:
                            i++; continue;
                        }
                        // Result-discard guard: the IV-do splice's
                        // result is a placeholder (startReg).  Only
                        // splice when the next bytecode discards it,
                        // so the wrong-type result is never observed.
                        // Pop (0xD8), ReturnReceiver (0x58), or
                        // ReturnTop following a Pop are all "discard".
                        if (doOff + 1 >= len_) { i++; continue; }
                        uint8_t afterDo = bc_[doOff + 1];
                        if (afterDo != 0xD8
                            && afterDo != 0x58) {
                            i++; continue;
                        }

                        int litIdx = bc_[pfbOff + 1];
                        int flags = bc_[pfbOff + 2];
                        bool ok = true;
                        const char* rejectReason = nullptr;
                        Op rejectedOp = Op::kReturn;

                        Oop blockOop = Oop::nil();
                        if (litIdx < 0
                            || (size_t)litIdx
                               >= out_.literals.size()) {
                            ok = false;
                            rejectReason = "bad litIdx";
                        } else {
                            blockOop = out_.literals[litIdx];
                            if (!blockOop.isObject()) {
                                ok = false;
                                rejectReason = "block not Object";
                            }
                        }
                        bool receiverOnStack =
                            ((flags >> 7) & 1) != 0;
                        if (ok && receiverOnStack) {
                            ok = false;
                            rejectReason = "recvOnStack";
                        }
                        uint32_t numCopied =
                            (uint32_t)(flags & 0x3F);
                        if (ok && numCopied != 0) {
                            ok = false;
                            rejectReason = "numCopied != 0";
                        }

                        std::unique_ptr<Method> blockIR;
                        if (ok && g_calleeLiftDepth < 1) {
                            blockIR = std::make_unique<Method>();
                            g_calleeLiftDepth++;
                            bool savedBR = g_subLiftAsBlockReturnLocal;
                            g_subLiftAsBlockReturnLocal = true;
                            uint32_t failedBc = UINT32_MAX;
                            LiftResult r = Builder::build(
                                blockOop, *memory_, *blockIR, &failedBc);
                            g_subLiftAsBlockReturnLocal = savedBR;
                            g_calleeLiftDepth--;
                            if (r != LiftResult::kOk) {
                                ok = false;
                                rejectReason = "block sub-lift failed";
                            }
                        } else if (ok) {
                            ok = false;
                            rejectReason = "recursion guard";
                        }

                        if (ok && blockIR) {
                            for (const auto& bv : blockIR->values) {
                                switch (bv.op) {
                                case Op::kLoadReceiver:
                                case Op::kLoadTrueOop:
                                case Op::kLoadFalseOop:
                                case Op::kLoadTemp:
                                case Op::kLoadLiteral:
                                case Op::kConstantOop:
                                case Op::kReturn:
                                case Op::kPrimAddInt:
                                case Op::kPrimSubInt:
                                case Op::kPrimMulInt:
                                case Op::kPrimTagCheckInt:
                                    break;
                                default:
                                    ok = false;
                                    rejectReason = "non-simple block op";
                                    rejectedOp = bv.op;
                                    break;
                                }
                                if (!ok) break;
                            }
                        }

                        if (ok && blockIR) {
                            uint32_t slot = static_cast<uint32_t>(
                                out_.inlinedBlocks.size());
                            out_.inlinedBlocks.push_back(
                                std::move(blockIR));
                            intervalDoAtTo_[(size_t)i] = slot;
                            spliceDoAtPushFullBlock_[
                                (size_t)pfbOff] = slot;
                            static int ivdCount = 0;
                            if (ivdCount++ < 4) {
                                std::fprintf(stderr,
                                    "[SISTA-IVDO-CAND] toBC=%zu "
                                    "pfbBC=%zu doBC=%zu litIdx=%d "
                                    "slot=%u\n",
                                    i, pfbOff, doOff, litIdx, slot);
                            }
                        } else {
                            static int ivdDiag = 0;
                            if (ivdDiag++ < 16) {
                                std::fprintf(stderr,
                                    "[SISTA-IVDO-DIAG] toBC=%zu "
                                    "verdict=%s reject=%s rejOp=%s\n",
                                    i, ok ? "OK" : "REJECT",
                                    rejectReason ? rejectReason : "(none)",
                                    ok ? "n/a" : OpInfo::name(rejectedOp));
                            }
                        }
                        i++;
                        continue;
                    }
                }
                if (jit::SistaV1::isThreeByteBytecode(op)) i += 3;
                else if (jit::SistaV1::isExtended2Byte(op)) i += 2;
                else i++;
            }
        }

        // --- B2 splice pre-pass: Interval-do closure-accumulator ------
        //
        // Detects: <push start> <push stop> Send1#to:
        //          PushTemp T_vec PushFullBlock(numCopied=1)
        //          SpecialSend(do: 0x7B)
        //
        // The block must match the 5-instruction closure-accum shape
        // (mirrors the kCountedLoopArrayDoAccum recognizer):
        //   PushTempAtInVec(slot,1) PushTemp 0 <arith> PopStoreTempInVec
        //   BlockReturnTop|Receiver
        //
        // Records:
        //   intervalDoAccumAtTo_[toOff]            = packed(slot|arith|outerVecTemp)
        //   spliceIvDoAccumAtPushFullBlock_[pfbOff] = same packed
        //
        // The main lift's Send1#to: handler emits kInterval at the
        // recorded offset, and the PushFullBlock arm intercepts to emit
        // kCountedLoopIntervalDoAccum when both maps match.
        if (accumSplice && memory_ != nullptr && toSelectorMask_) {
            size_t i = 0;
            while (i < len_) {
                uint8_t op = bc_[i];
                if (op >= jit::SistaV1::Send1Base
                    && op <= jit::SistaV1::Send1Last) {
                    uint32_t selIdx = op & 0x0F;
                    if (((toSelectorMask_ >> selIdx) & 1) != 0) {
                        // Possible to: send.  Need:
                        //   bc_[i+1]   = PushTemp T_vec (0x40-0x4B)
                        //   bc_[i+2]   = PushFullBlock (0xF9)
                        //   bc_[i+5]   = SpecialSend do: (0x7B)
                        //   numCopied  = 1
                        //   block-IR   = closure-accum 5-instruction shape
                        size_t pushTOff = i + 1;
                        size_t pfbOff   = i + 2;
                        size_t doOff    = i + 5;
                        if (doOff >= len_) { i++; continue; }
                        uint8_t pushTOp = bc_[pushTOff];
                        if (pushTOp < 0x40 || pushTOp > 0x4B) {
                            i++; continue;
                        }
                        uint32_t outerVecTemp = pushTOp - 0x40;
                        if (bc_[pfbOff] != jit::SistaV1::PushFullBlock) {
                            i++; continue;
                        }
                        if (bc_[doOff] != 0x7B) {
                            i++; continue;
                        }
                        // Result-discard guard: the IV-do-accum splice
                        // returns a placeholder (startReg) for the
                        // Interval; only splice when the next bc
                        // discards it (Pop or ReturnReceiver).
                        if (doOff + 1 >= len_) { i++; continue; }
                        uint8_t afterDo = bc_[doOff + 1];
                        if (afterDo != 0xD8 && afterDo != 0x58) {
                            i++; continue;
                        }
                        int litIdx = bc_[pfbOff + 1];
                        int flags  = bc_[pfbOff + 2];
                        bool recvOnStack = ((flags >> 7) & 1) != 0;
                        int  numCopied   = flags & 0x3F;
                        if (recvOnStack || numCopied != 1) {
                            i++; continue;
                        }

                        // Validate block bytecode shape (mirrors the
                        // closure-accum pre-pass at the top of this
                        // function).
                        if (litIdx < 0
                            || (size_t)litIdx >= out_.literals.size()) {
                            i++; continue;
                        }
                        Oop blockOop = out_.literals[litIdx];
                        if (!blockOop.isObject()) { i++; continue; }
                        ObjectHeader* blockHdr = blockOop.asObjectPtr();
                        if (!blockHdr->isCompiledMethod()) {
                            i++; continue;
                        }
                        const uint8_t* bs = blockHdr->bytes();
                        size_t total = blockHdr->byteSize();
                        if (total < 8) { i++; continue; }
                        uint64_t hbits = 0;
                        std::memcpy(&hbits, bs, 8);
                        Oop hdr = Oop::fromRawBits(hbits);
                        if (!hdr.isSmallInteger()) { i++; continue; }
                        int64_t hb = hdr.asSmallInteger();
                        uint32_t bNumLits = (uint32_t)(hb & 0x7FFF);
                        size_t bHdrBytes = (1 + bNumLits) * 8;
                        if (total <= bHdrBytes) { i++; continue; }
                        const uint8_t* bbc = bs + bHdrBytes;
                        size_t bLen = total - bHdrBytes;
                        if (bLen < 9 || bLen > 10) { i++; continue; }
                        if (bbc[0] != 0xFB || bbc[2] != 0x01) {
                            i++; continue;
                        }
                        if (bbc[3] != 0x40) { i++; continue; }
                        uint8_t arithB = bbc[4];
                        int arithCode = -1;
                        if (arithB == 0x60) arithCode = 0;
                        else if (arithB == 0x61) arithCode = 1;
                        else if (arithB == 0x68) arithCode = 2;
                        if (arithCode < 0) { i++; continue; }
                        if (bbc[5] != 0xFC) { i++; continue; }
                        if (bbc[7] != 0x01) { i++; continue; }
                        if (bbc[6] != bbc[1]) { i++; continue; }
                        if (bbc[8] != 0x5D && bbc[8] != 0x5E) {
                            i++; continue;
                        }
                        uint32_t slot = bbc[1];
                        uint64_t packed = (uint64_t)slot
                                        | ((uint64_t)arithCode << 8)
                                        | ((uint64_t)outerVecTemp << 16);

                        intervalDoAccumAtTo_[(size_t)i] = packed;
                        spliceIvDoAccumAtPushFullBlock_[
                            (size_t)pfbOff] = packed;
                        static int ivacCount = 0;
                        if (ivacCount++ < 4) {
                            std::fprintf(stderr,
                                "[SISTA-IVDOACC-CAND] toBC=%zu "
                                "pfbBC=%zu doBC=%zu slot=%u arith=%d "
                                "outerTemp=%u\n",
                                i, pfbOff, doOff, slot, arithCode,
                                outerVecTemp);
                        }
                        i++;
                        continue;
                    }
                }
                if (jit::SistaV1::isThreeByteBytecode(op)) i += 3;
                else if (jit::SistaV1::isExtended2Byte(op)) i += 2;
                else i++;
            }
        }

        // --- B2 minimal peephole: `^ self size` ------------------------
        //
        // Recognize the method shape:
        //   PushReceiver (0x4C), SpecialSend size (0x72), ReturnTop (0x5C)
        // and emit kLoadReceiver + kGuardClass + kPrimSize + kReturn
        // directly.  This is the smallest end-to-end test of the
        // cc.invoke + kPrimSize pipeline.
        //
        // Gated PHARO_SISTA_SIZE_PEEPHOLE=1 default off so we can
        // bisect any regressions cleanly.  The IC must say the
        // receiver is monomorphic for a class jit_rt_sista_basic_size
        // handles (Array, ByteArray, etc.); without that hint we
        // can't safely speculate.
        {
            static const bool sizePeephole =
                std::getenv("PHARO_SISTA_SIZE_PEEPHOLE") != nullptr;
            if (sizePeephole && len_ >= 3
                && bc_[0] == jit::SistaV1::PushReceiver
                && bc_[1] == 0x72  // SpecialSend size
                && bc_[2] == jit::SistaV1::ReturnTop) {
                // IC-guided gate: only emit when the IC at the size
                // send has observed at least one class.  Without an
                // IC hint, the receiver could be anything — speculating
                // and paying per-call deopt costs hurts more than the
                // bail (verified: sort regressed 231→433ms when this
                // gate was off).  When present, the IC class doesn't
                // need to match exactly; helper does its own check.
                // The hint just confirms "this site has actually run
                // and is hot enough to bother optimizing."
                bool hasIC = false;
                if (inlineHints_) {
                    for (const auto& h : *inlineHints_) {
                        if (h.bcOffset == 1) { hasIC = true; break; }
                    }
                }
                if (!hasIC) goto sizePeepholeSkip;
                out_.blocks.clear();
                out_.values.clear();
                out_.framepoints.clear();
                out_.blocks.push_back(Block{});
                out_.blocks[0].id = 0;
                out_.blocks[0].sourceBytecodeOffset = 0;
                currentBlock_ = 0;

                uint32_t recvId = out_.newValue(0,
                    Op::kLoadReceiver, Type::kOop);
                // kPrimSize.literal = bcOffset for deopt resume —
                // the size send is at offset 1 in this 3-byte method.
                uint32_t sizeId = out_.newValue(0,
                    Op::kPrimSize, Type::kOopSmallInt,
                    {recvId}, /*literal=*/1);
                out_.newValue(0, Op::kReturn, Type::kVoid,
                               {sizeId});

                static int peepholeCount = 0;
                if (peepholeCount++ < 8) {
                    std::fprintf(stderr,
                        "[SISTA-SIZE-PEEPHOLE] matched (IC-guided)\n");
                }
                return LiftResult::kOk;
            }
            sizePeepholeSkip:;
        }

        // --- B2 minimal peephole: `^ self at: i` ----------------------
        //
        // Recognize the method shape:
        //   PushReceiver (0x4C), PushTemp 0 (0x40),
        //   SpecialSend at: (0x70), ReturnTop (0x5C)
        // and emit kLoadReceiver + kLoadTemp(0) + kPrimAt + kReturn.
        //
        // Same gate logic as the size peephole: only emit when IC at
        // the at: send (offset 2) has observed at least one class.
        // Lowering's deopt-on-zero handles non-indexable / OOB / bad
        // index — bails to the interpreter at the at: send.
        {
            static const bool atPeephole =
                std::getenv("PHARO_SISTA_AT_PEEPHOLE") != nullptr;
            if (atPeephole && len_ >= 4
                && bc_[0] == jit::SistaV1::PushReceiver
                && bc_[1] == 0x40  // PushTemp 0 (the index arg)
                && bc_[2] == 0x70  // SpecialSend at:
                && bc_[3] == jit::SistaV1::ReturnTop) {
                bool hasIC = false;
                if (inlineHints_) {
                    for (const auto& h : *inlineHints_) {
                        if (h.bcOffset == 2) { hasIC = true; break; }
                    }
                }
                if (!hasIC) goto atPeepholeSkip;

                out_.blocks.clear();
                out_.values.clear();
                out_.framepoints.clear();
                out_.blocks.push_back(Block{});
                out_.blocks[0].id = 0;
                out_.blocks[0].sourceBytecodeOffset = 0;
                currentBlock_ = 0;

                uint32_t recvId = out_.newValue(0,
                    Op::kLoadReceiver, Type::kOop);
                uint32_t idxId = out_.newValue(0,
                    Op::kLoadTemp, Type::kOop, /*operands=*/{},
                    /*literal=*/0);  // temp 0 = the arg
                uint32_t atId = out_.newValue(0,
                    Op::kPrimAt, Type::kOop,
                    {recvId, idxId},
                    /*literal=*/2);  // bcOffset of at: send
                out_.newValue(0, Op::kReturn, Type::kVoid,
                               {atId});

                static int atPeepholeCount = 0;
                if (atPeepholeCount++ < 8) {
                    std::fprintf(stderr,
                        "[SISTA-AT-PEEPHOLE] matched (IC-guided)\n");
                }
                return LiftResult::kOk;
            }
            atPeepholeSkip:;
        }

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
            // Diagnostic — track every opcode the lifter sees.
            // PHARO_SISTA_DO_DETECT=1 enables.
            {
                static const bool detect =
                    std::getenv("PHARO_SISTA_DO_DETECT") != nullptr;
                if (detect) {
                    static uint64_t opcodeHist[256] = {0};
                    static uint64_t totalOps = 0;
                    opcodeHist[op]++;
                    totalOps++;
                    if (totalOps == 200000 || totalOps == 1000000) {
                        std::fprintf(stderr, "[SISTA-LIFTER-OPHIST]");
                        for (int i = 0; i < 256; i++) {
                            if (opcodeHist[i] > 0
                                && (i == 0xF8 || i == 0xF9 || i == 0xFA
                                    || i == 0xFB)) {
                                std::fprintf(stderr,
                                    " op=0x%02x:%llu",
                                    i,
                                    (unsigned long long)opcodeHist[i]);
                            }
                        }
                        std::fprintf(stderr, " total=%llu\n",
                                      (unsigned long long)totalOps);
                    }
                }
            }
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

            // PushFullBlock — emit kBlockCreate IR (bail-only path).
            //
            // For now kBlockCreate has the same operand layout as
            // kSendUnspeculated: operands are the FULL IR stack
            // snapshot at this point (so the bail to interpreter sees
            // every value compiled code pushed).  The literal encodes
            // litIndex / flags / stackSize / bcOffset.
            //
            // The PHARO_SISTA_BLOCK_HELPER=1 lowering path that calls
            // jit_rt_sista_block_create is currently SIGSEGV — asmjit
            // invoke-node setup needs more work in the Sista function
            // frame.  Default path bails identically to old generic
            // behavior, so net runtime is unchanged.
            if (op == jit::SistaV1::PushFullBlock) {
                if (stack_.size() > 255) {
                    if (failedAtBytecode) *failedAtBytecode = bcOffset;
                    return LiftResult::kMalformedMethod;
                }
                if (ip + 2 >= len_) {
                    if (stack_.empty()) return LiftResult::kOk;
                    return bailToInterpreter(3);
                }

                // B2 splice intercept (Interval closure-accumulator):
                // kInterval(start,stop) marker on stack, then PushTemp T_vec,
                // then PushFullBlock(numCopied=1), then SpecialSend do:.
                // Block matches closure-accum 5-instruction shape.
                // → kCountedLoopIntervalDoAccum.
                {
                    auto ivacIt =
                        spliceIvDoAccumAtPushFullBlock_.find(ip);
                    if (ivacIt != spliceIvDoAccumAtPushFullBlock_.end()
                        && stack_.size() >= 2
                        && ip + 3 < len_
                        && bc_[ip + 3] == 0x7B) {
                        uint64_t packed = ivacIt->second;
                        uint32_t vecRef = stack_.back();
                        uint32_t rcv = stack_[stack_.size() - 2];
                        bool rcvIsInterval =
                            (rcv < out_.values.size()
                             && out_.values[rcv].op == Op::kInterval);
                        if (rcvIsInterval) {
                            stack_.pop_back();         // vecRef
                            stack_.pop_back();         // kInterval marker
                            uint32_t startV =
                                out_.values[rcv].operands[0];
                            uint32_t stopV =
                                out_.values[rcv].operands[1];
                            std::vector<uint32_t> ops{
                                startV, stopV, vecRef};
                            uint32_t vid = out_.newValue(currentBlock_,
                                  Op::kCountedLoopIntervalDoAccum,
                                  Type::kOop,
                                  std::move(ops),
                                  /*literal=*/packed);
                            recordFramepoint(vid, bcOffset);
                            stack_.push_back(vid);
                            pendingExtA_ = 0;
                            pendingExtB_ = 0;
                            static int ivacEmitCount = 0;
                            if (ivacEmitCount++ < 16) {
                                std::fprintf(stderr,
                                    "[SISTA-IVDOACC-EMIT] bc=%zu vid=%u "
                                    "packed=0x%llx\n",
                                    ip, vid,
                                    (unsigned long long)packed);
                            }
                            ip += 4;
                            continue;
                        }
                    }
                }
                // B2 splice intercept (closure accumulator):
                // PushFullBlock(numCopied=1) + SpecialSend do: where
                // outer pushed [rcv, vecRef] and block matches the
                // 5-instruction accum pattern → kCountedLoopArrayDoAccum.
                {
                    auto aIt = spliceAccumAtPushFullBlock_.find(ip);
                    if (aIt != spliceAccumAtPushFullBlock_.end()
                        && stack_.size() >= 2
                        && ip + 3 < len_
                        && bc_[ip + 3] == 0x7B) {
                        uint64_t packed = aIt->second;
                        uint32_t vecRef = stack_.back();
                        stack_.pop_back();
                        uint32_t rcv = stack_.back();
                        stack_.pop_back();
                        std::vector<uint32_t> ops{rcv, vecRef};
                        uint32_t vid = out_.newValue(currentBlock_,
                                       Op::kCountedLoopArrayDoAccum,
                                       Type::kOop,
                                       std::move(ops),
                                       /*literal=*/packed);
                        recordFramepoint(vid, bcOffset);
                        stack_.push_back(vid);
                        pendingExtA_ = 0;
                        pendingExtB_ = 0;
                        static int accumEmitCount = 0;
                        if (accumEmitCount++ < 16) {
                            std::fprintf(stderr,
                                "[SISTA-ACCUM-EMIT] bc=%zu vid=%u "
                                "packed=0x%llx\n",
                                ip, vid,
                                (unsigned long long)packed);
                        }
                        ip += 4;
                        continue;
                    }
                }
                // B2 splice intercept (Interval-do): kInterval rcv
                // + PushFullBlock + SpecialSend do: → kCountedLoopIntervalDo.
                {
                    auto dIt = spliceDoAtPushFullBlock_.find(ip);
                    if (dIt != spliceDoAtPushFullBlock_.end()
                        && stack_.size() >= 1
                        && ip + 3 < len_
                        && bc_[ip + 3] == 0x7B) {
                        uint32_t blockSlot = dIt->second;
                        uint32_t rcv = stack_.back();
                        bool rcvIsInterval =
                            (rcv < out_.values.size()
                             && out_.values[rcv].op == Op::kInterval);
                        if (rcvIsInterval) {
                            stack_.pop_back();
                            uint32_t startV =
                                out_.values[rcv].operands[0];
                            uint32_t stopV =
                                out_.values[rcv].operands[1];
                            std::vector<uint32_t> ops{startV, stopV};
                            uint32_t vid = out_.newValue(currentBlock_,
                                  Op::kCountedLoopIntervalDo,
                                  Type::kOop,
                                  std::move(ops),
                                  /*literal=*/blockSlot);
                            recordFramepoint(vid, bcOffset);
                            stack_.push_back(vid);
                            pendingExtA_ = 0;
                            pendingExtB_ = 0;
                            static int ivdEmitCount = 0;
                            if (ivdEmitCount++ < 16) {
                                std::fprintf(stderr,
                                    "[SISTA-IVDO-SPLICE-EMIT] bc=%zu "
                                    "slot=%u vid=%u\n",
                                    ip, blockSlot, vid);
                            }
                            ip += 4;
                            continue;
                        }
                    }
                }
                // B2 splice intercept: if this PushFullBlock starts a
                // pre-validated `[block] do:` pattern, emit
                // kCountedLoopDo and skip both bytecodes.  The pre-pass
                // populated spliceAtPushFullBlock_ with eligible
                // offsets + their block-IR slot.
                {
                    auto sIt = spliceAtPushFullBlock_.find(ip);
                    if (sIt != spliceAtPushFullBlock_.end()
                        && stack_.size() >= 1
                        // Make sure SpecialSend do: really follows.
                        && ip + 3 < len_
                        && bc_[ip + 3] == 0x7B) {
                        uint32_t blockSlot = sIt->second;
                        uint32_t rcv = stack_.back();
                        stack_.pop_back();
                        std::vector<uint32_t> ops{rcv};
                        uint32_t vid = out_.newValue(currentBlock_,
                                       Op::kCountedLoopDo, Type::kOop,
                                       std::move(ops),
                                       /*literal=*/blockSlot);
                        recordFramepoint(vid, bcOffset);
                        stack_.push_back(vid);
                        pendingExtA_ = 0;
                        pendingExtB_ = 0;
                        static int emitCount = 0;
                        if (emitCount++ < 16) {
                            std::fprintf(stderr,
                                "[SISTA-SPLICE-EMIT] bc=%zu slot=%u vid=%u\n",
                                ip, blockSlot, vid);
                        }
                        ip += 4;  // PushFullBlock (3) + SpecialSend (1)
                        continue;
                    }
                }
                // B2 splice (inject:into: variant): PushFullBlock +
                // Send2(#inject:into:) → kCountedLoopInjectInto, OR
                // kCountedLoopIntervalInjectInto if the receiver is
                // a kInterval marker (which means the receiver came
                // from an elided `(start to: stop)` Send1).
                {
                    auto iIt = spliceInjectAtPushFullBlock_.find(ip);
                    if (iIt != spliceInjectAtPushFullBlock_.end()
                        && stack_.size() >= 2
                        // Verify Send2 really follows (op in 0xA0-0xAF).
                        && ip + 3 < len_
                        && bc_[ip + 3] >= 0xA0
                        && bc_[ip + 3] <= 0xAF) {
                        uint32_t blockSlot = iIt->second;
                        uint32_t init = stack_.back();
                        stack_.pop_back();
                        uint32_t rcv = stack_.back();
                        stack_.pop_back();

                        // Check if rcv is a kInterval marker.
                        bool rcvIsInterval =
                            (rcv < out_.values.size()
                             && out_.values[rcv].op == Op::kInterval);
                        uint32_t vid;
                        if (rcvIsInterval) {
                            uint32_t startV =
                                out_.values[rcv].operands[0];
                            uint32_t stopV =
                                out_.values[rcv].operands[1];
                            std::vector<uint32_t> ops{startV, stopV, init};
                            vid = out_.newValue(currentBlock_,
                                  Op::kCountedLoopIntervalInjectInto,
                                  Type::kOop,
                                  std::move(ops),
                                  /*literal=*/blockSlot);
                        } else {
                            std::vector<uint32_t> ops{rcv, init};
                            vid = out_.newValue(currentBlock_,
                                  Op::kCountedLoopInjectInto,
                                  Type::kOop,
                                  std::move(ops),
                                  /*literal=*/blockSlot);
                        }
                        recordFramepoint(vid, bcOffset);
                        stack_.push_back(vid);
                        pendingExtA_ = 0;
                        pendingExtB_ = 0;
                        static int injEmitCount = 0;
                        if (injEmitCount++ < 16) {
                            std::fprintf(stderr,
                                "[SISTA-INJECT-SPLICE-EMIT] bc=%zu "
                                "slot=%u vid=%u kind=%s\n",
                                ip, blockSlot, vid,
                                rcvIsInterval ? "interval" : "array");
                        }
                        ip += 4;  // PushFullBlock (3) + Send2 (1)
                        continue;
                    }
                }

                uint32_t litIndex =
                    (uint32_t)((pendingExtA_ << 8) | bc_[ip + 1]);
                uint32_t flags = (uint32_t)bc_[ip + 2];
                uint32_t numCopied = flags & 0x3Fu;
                bool receiverOnStack = ((flags >> 7) & 1u) != 0;
                uint32_t needed = numCopied + (receiverOnStack ? 1u : 0u);
                if (stack_.size() < needed) {
                    return bailToInterpreter(3);
                }
                uint32_t stackSize = (uint32_t)stack_.size();
                std::vector<uint32_t> ops(stack_.begin(), stack_.end());
                stack_.clear();
                uint64_t bailOffset = (uint64_t)currentInstrStart_;
                uint64_t lit = ((uint64_t)litIndex      &  0xFFFFu)
                             | (((uint64_t)flags        &  0xFFu)   << 16)
                             | (((uint64_t)stackSize    &  0xFFu)   << 24)
                             |  (bailOffset                          << 32);
                uint32_t vid = out_.newValue(currentBlock_,
                               Op::kBlockCreate, Type::kOop,
                               std::move(ops), lit);
                recordFramepoint(vid, static_cast<uint32_t>(bailOffset));
                static const bool detect =
                    std::getenv("PHARO_SISTA_DO_DETECT") != nullptr;
                if (detect) {
                    static int blockCreates = 0;
                    if (blockCreates++ < 32) {
                        std::fprintf(stderr,
                            "[SISTA-BLOCKCREATE] bc=%llu litIdx=%u "
                            "numCopied=%u flags=0x%02x stackSize=%u\n",
                            (unsigned long long)bailOffset,
                            litIndex, numCopied, flags, stackSize);
                    }
                }
                pendingExtA_ = 0;
                pendingExtB_ = 0;
                ip += 3;
                continue;
            }

            if (op == jit::SistaV1::PushClosure
             || op == jit::SistaV1::PushThisContext) {
                static const bool detect =
                    std::getenv("PHARO_SISTA_DO_DETECT") != nullptr;
                if (detect) {
                    static int otherBails = 0;
                    if (otherBails++ < 32) {
                        std::fprintf(stderr,
                            "[SISTA-OTHER-BAIL] op=0x%02x bc=%u\n",
                            op, (uint32_t)bcOffset);
                    }
                }
                return bailToInterpreter(instructionSize(op));
            }
            // PushArray (0xE7) is handled below (kAllocArray for j=0 case;
            // bail for j=1 popInto case).

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

            // PushNewArray (0xE7): the j=0 form pushes a fresh Array
            // of `size = desc & 0x7F`.  The j=1 form (popInto, top bit
            // of desc set) pops `size` values from the stack and stores
            // them in the new Array — defer for now.
            if (op == 0xE7) {
                if (ip + 1 >= len_) return bailToInterpreter(2);
                uint8_t desc = bc_[ip + 1];
                bool popInto = (desc >> 7) != 0;
                if (popInto) {
                    return bailToInterpreter(2);
                }
                uint32_t size = desc & 0x7F;
                uint32_t vid = out_.newValue(currentBlock_,
                               Op::kAllocArray, Type::kOop,
                               /*operands=*/{}, /*literal=*/size);
                stack_.push_back(vid);
                pendingExtA_ = 0;
                pendingExtB_ = 0;
                ip += 2;
                continue;
            }

            // Remote-temp ops: emit kLoadTempInVec / kStoreTempInVec.
            //   0xFB indexInVec tempIdxOfVec — push slot
            //   0xFC indexInVec tempIdxOfVec — store top, leave on stack
            //   0xFD indexInVec tempIdxOfVec — pop into slot
            // Encoding: literal = (tempIdxOfVec << 32) | indexInVec.
            if (op == jit::SistaV1::PushTempAtInVec) {
                if (ip + 2 >= len_) return bailToInterpreter(3);
                uint32_t indexInVec  = bc_[ip + 1];
                uint32_t tempIdxOfVec = bc_[ip + 2];
                uint64_t lit = ((uint64_t)tempIdxOfVec << 32) | indexInVec;
                uint32_t vid = out_.newValue(currentBlock_,
                               Op::kLoadTempInVec, Type::kOop,
                               /*operands=*/{}, lit);
                stack_.push_back(vid);
                pendingExtA_ = 0;
                pendingExtB_ = 0;
                ip += 3;
                continue;
            }
            if (op == 0xFC) {
                // storeTemp:inVectorAt: — leaves value on stack.
                if (ip + 2 >= len_) return bailToInterpreter(3);
                if (stack_.empty()) return bailToInterpreter(3);
                uint32_t indexInVec  = bc_[ip + 1];
                uint32_t tempIdxOfVec = bc_[ip + 2];
                uint64_t lit = ((uint64_t)tempIdxOfVec << 32) | indexInVec;
                uint32_t topV = stack_.back();
                out_.newValue(currentBlock_,
                              Op::kStoreTempInVec, Type::kVoid,
                              /*operands=*/{topV}, lit);
                // value stays on stack
                pendingExtA_ = 0;
                pendingExtB_ = 0;
                ip += 3;
                continue;
            }
            if (op == 0xFD) {
                // popStoreTemp:inVectorAt: — consumes top of stack.
                if (ip + 2 >= len_) return bailToInterpreter(3);
                if (stack_.empty()) return bailToInterpreter(3);
                uint32_t indexInVec  = bc_[ip + 1];
                uint32_t tempIdxOfVec = bc_[ip + 2];
                uint64_t lit = ((uint64_t)tempIdxOfVec << 32) | indexInVec;
                uint32_t topV = stack_.back();
                stack_.pop_back();
                out_.newValue(currentBlock_,
                              Op::kStoreTempInVec, Type::kVoid,
                              /*operands=*/{topV}, lit);
                pendingExtA_ = 0;
                pendingExtB_ = 0;
                ip += 3;
                continue;
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
                detectDoBlockPattern(nArgs, bailOffset);
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
                detectDoBlockPattern(nArgs, bcOffset);
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

                // B2 splice (Interval-inject): if this is a Send1 #to:
                // at a recorded pre-pass offset, emit kInterval as a
                // marker and DO NOT terminate the lift.  The
                // inject:into: intercept at PushFullBlock will then
                // upgrade kCountedLoopInjectInto to
                // kCountedLoopIntervalInjectInto when it sees
                // kInterval as the receiver.
                if (nArgs == 1) {
                    bool ivMatch =
                        (intervalInjectAtTo_.find(ip)
                         != intervalInjectAtTo_.end())
                     || (intervalDoAtTo_.find(ip)
                         != intervalDoAtTo_.end())
                     || (intervalDoAccumAtTo_.find(ip)
                         != intervalDoAccumAtTo_.end());
                    if (ivMatch) {
                        uint32_t stop = stack_.back();
                        stack_.pop_back();
                        uint32_t start = stack_.back();
                        stack_.pop_back();
                        uint32_t vid = out_.newValue(currentBlock_,
                                       Op::kInterval, Type::kOop,
                                       std::vector<uint32_t>{start, stop});
                        recordFramepoint(vid, bcOffset);
                        stack_.push_back(vid);
                        pendingExtA_ = 0;
                        pendingExtB_ = 0;
                        static int ivEmitCount = 0;
                        if (ivEmitCount++ < 4) {
                            std::fprintf(stderr,
                                "[SISTA-IV-INTERVAL-EMIT] "
                                "bc=%zu vid=%u\n", ip, vid);
                        }
                        ip++;  // Send1 #to: is 1 byte.
                        continue;
                    }
                }

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

                // Phase 4 Step 3: monomorphic-inline const-return
                // callees.  When the IC hint at this bcOffset matches
                // a callee whose IR is exactly `kLoad{True,False,Recv}
                // / kConstantOop` followed by `kReturn`, replace the
                // send with kGuardClass + the constant load.  Caller's
                // stack effect: pop nArgs+1, push 1 (the constant).
                // Lifter keeps going through subsequent bytecodes.
                //
                // Gated behind PHARO_SISTA_INLINE_CONST=1 until we have
                // soak time on the deopt path under load.
                if (tryInlineConstReturn(nArgs, bcOffset)) {
                    ip++;
                    continue;
                }

                detectDoBlockPattern(nArgs, bcOffset);

                // B-1: PHARO_SISTA_HELPER_SENDS=1 → emit kSendCallHelper
                // and continue lifting past the send.  Helper invokes
                // the send synchronously; result is pushed onto stack_.
                // On NLR, helper returns 0 → lowering deopts.
                static const bool helperSends = []() {
                    const char* v = std::getenv("PHARO_SISTA_HELPER_SENDS");
                    return v && v[0] == '1';
                }();
                if (helperSends) {
                    // Pop only rcvr + args (the send consumes them).
                    // Other live IR-stack values stay in their
                    // registers — kSendCallHelper is a producing op,
                    // not a terminator.
                    std::vector<uint32_t> sendOps;
                    sendOps.reserve(nArgs + 1);
                    for (uint32_t i = 0; i < nArgs + 1; i++) {
                        sendOps.push_back(
                            stack_[stack_.size() - nArgs - 1 + i]);
                    }
                    for (uint32_t i = 0; i < nArgs + 1; i++) {
                        stack_.pop_back();
                    }
                    // Literal layout: low 32 = selIdx, mid 16 = nArgs,
                    // high 16 = bcOffset.  Different from kSendUnspeculated
                    // because we need bcOffset for the deopt-on-NLR path.
                    uint64_t lit =
                        static_cast<uint64_t>(selIdx)
                      | (static_cast<uint64_t>(nArgs)    << 32)
                      | (static_cast<uint64_t>(bcOffset) << 48);
                    uint32_t vid = out_.newValue(currentBlock_,
                                                  Op::kSendCallHelper,
                                                  Type::kOop,
                                                  std::move(sendOps),
                                                  lit);
                    recordFramepoint(vid, bcOffset);
                    stack_.push_back(vid);
                    ip++;
                    continue;
                }

                // Default path: bail flushes the ENTIRE IR stack to
                // state.sp so the interpreter sees every value
                // simulated-pushed by compiled code.  Compiled
                // execution ends here.
                uint32_t stackSize = static_cast<uint32_t>(stack_.size());
                std::vector<uint32_t> ops(stack_.begin(), stack_.end());
                stack_.clear();
                uint64_t lit = static_cast<uint64_t>(selIdx)
                             | (static_cast<uint64_t>(nArgs)      << 16)
                             | (static_cast<uint64_t>(bcOffset)   << 24);
                (void)stackSize;
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
            //
            // EXCEPT: when sub-lifting a block for B2 splice, treat
            // block-return as a local kReturn (the splice intercept
            // discards the block's value).
            if ((op == jit::SistaV1::BlockReturnNil
              || op == jit::SistaV1::BlockReturnTop)
                && blockReturnAsLocalReturn_) {
                if (op == jit::SistaV1::BlockReturnNil) {
                    // Push nil and return.  No special "load nil"
                    // op in our IR; use kConstantOop with 0 (nil's
                    // raw bits in Spur) — but our splice lowering
                    // won't see this anyway since kReturn's value
                    // is unused.
                    uint32_t nilVid = out_.newValue(currentBlock_,
                                                     Op::kConstantOop,
                                                     Type::kOop,
                                                     /*operands=*/{},
                                                     /*literal=*/0);
                    stack_.push_back(nilVid);
                }
                if (stack_.empty()) {
                    if (failedAtBytecode) *failedAtBytecode = bcOffset;
                    return LiftResult::kMalformedMethod;
                }
                uint32_t retVal = stack_.back();
                stack_.pop_back();
                out_.newValue(currentBlock_, Op::kReturn, Type::kVoid,
                              /*operands=*/{retVal});
                return LiftResult::kOk;
            }
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
    //
    // Also tracks Phase 4 inline-hint matches as a side effect.
    // Phase 4 Step 3: try to inline a monomorphic send whose callee
    // is a const-return method (`^ true`, `^ false`, `^ self`,
    // `^ <literal>`).  Returns true if inlined; the caller must pop
    // nArgs+1 from the simulated stack and continue lifting.
    //
    // Pattern recognition is intentionally conservative — only the 2-
    // value `kLoadXxx kReturn` shape is accepted, and only when
    // B2 detection: at any send site with nArgs==1, check if the
    // arg is a kBlockCreate.  If so, log the receiver IC class +
    // block literal index.  Used to size B2 (Array do: with literal
    // block → counted at: loop with block body inlined).
    //
    // PHARO_SISTA_DO_DETECT=1 enables the dump.  Pure observation —
    // does not change runtime behavior.
    void detectDoBlockPattern(uint32_t nArgs, uint32_t bcOffset) {
        static const bool detect =
            std::getenv("PHARO_SISTA_DO_DETECT") != nullptr;
        if (!detect) return;
        if (nArgs != 1) return;
        if (stack_.size() < 2) return;
        uint32_t blockArgId = stack_[stack_.size() - 1];
        if (blockArgId >= out_.values.size()) return;
        const Value& blockVal = out_.values[blockArgId];
        if (blockVal.op != Op::kBlockCreate) return;
        // Probe IC hint for receiver class.
        const InlineHint* hit = nullptr;
        if (inlineHints_) {
            for (const auto& h : *inlineHints_) {
                if (h.bcOffset == bcOffset) { hit = &h; break; }
            }
        }
        uint32_t litIdx = (uint32_t)(blockVal.literal & 0xFFFFu);
        uint32_t flags = (uint32_t)((blockVal.literal >> 16) & 0xFFu);
        uint32_t numCopied = flags & 0x3Fu;
        bool receiverOnStack = ((flags >> 7) & 1u) != 0;
        bool ignoreOuterCtx = ((flags >> 6) & 1u) != 0;
        static int dumpCount = 0;
        if (dumpCount++ < 32) {
            std::fprintf(stderr,
                "[SISTA-DO-BLOCK] bc=%u blockLit=%u numCopied=%u "
                "rcvOnStack=%d ignOuter=%d icClass=0x%llx hasHint=%d\n",
                bcOffset, litIdx, numCopied,
                (int)receiverOnStack, (int)ignoreOuterCtx,
                hit ? (unsigned long long)hit->classOop : 0ULL,
                hit ? 1 : 0);
        }
    }

    // there's exactly one block with both values in it.  Anything more
    // complex still bails to the unspeculated send.
    bool tryInlineConstReturn(uint32_t nArgs, uint32_t bcOffset) {
        if (!inlineHints_) return false;
        static const bool inlineConst =
            std::getenv("PHARO_SISTA_INLINE_CONST") != nullptr;
        if (!inlineConst) return false;
        if (g_currentBuildMemory == nullptr) return false;
        if (g_calleeLiftDepth >= 2) return false;
        if (stack_.size() < nArgs + 1) return false;

        const InlineHint* hit = nullptr;
        for (const auto& h : *inlineHints_) {
            if (h.bcOffset == bcOffset) { hit = &h; break; }
        }
        if (!hit) return false;
        if (hit->classOop == 0 || hit->targetMethod == 0) return false;
        if ((hit->targetMethod & 0x7) != 0) return false;
        if (hit->targetMethod < 0x10000) return false;

        // Probe-lift the callee.  Same recursion guard as the
        // measurement-only path in recordFramepoint.
        Method calleeIR;
        uint32_t calleeFailedAt = UINT32_MAX;
        Oop calleeOop = Oop::fromRawBits(hit->targetMethod);
        g_calleeLiftDepth++;
        LiftResult cr;
        {
            ClearOuterHints g;
            cr = Builder::build(calleeOop,
                *g_currentBuildMemory, calleeIR, &calleeFailedAt);
        }
        g_calleeLiftDepth--;
        if (cr != LiftResult::kOk) return false;

        // Receiver value at stack_[size - nArgs - 1] — used by
        // kLoadReceiver substitution and kGuardClass.
        uint32_t recvId = stack_[stack_.size() - nArgs - 1];

        // Two recognized shapes:
        //
        //   2 values: kLoad{TrueOop,FalseOop,ConstantOop,Receiver}
        //             + kReturn → inlined value is the load output.
        //
        //   3 values: kLoadReceiver + kLoadInstVar(v0, ivarIdx)
        //             + kReturn → inlined value is a new
        //             kLoadInstVar(callerRecv, ivarIdx).  Same
        //             effect T1's IC inline-getter fast-path
        //             produces, minus the IC probe overhead.
        Op   inlineOp;
        Type inlineTy;
        uint64_t inlineLit = 0;
        std::vector<uint32_t> inlineOps;

        if (calleeIR.values.size() == 2) {
            const Value& v0 = calleeIR.values[0];
            const Value& v1 = calleeIR.values[1];
            // 2-value const-return: kLoadXxx + kReturn(v0).
            if (v1.op == Op::kReturn
             && v1.operands.size() == 1 && v1.operands[0] == v0.id) {
                switch (v0.op) {
                case Op::kLoadTrueOop:  inlineOp = Op::kLoadTrueOop;  inlineTy = Type::kOopBool; break;
                case Op::kLoadFalseOop: inlineOp = Op::kLoadFalseOop; inlineTy = Type::kOopBool; break;
                case Op::kConstantOop:  inlineOp = Op::kConstantOop;  inlineTy = Type::kOop;
                                         inlineLit = v0.literal; break;
                case Op::kLoadReceiver: inlineOp = Op::kLoadReceiver; inlineTy = Type::kOop; break;
                case Op::kLoadTemp: {
                    // 2-value parameter-passthrough: `^ arg` where the
                    // method body is `pushTemp N; returnTop`.  Inlined
                    // value is the caller's arg at index v0.literal.
                    // Caller's stack at the send-site has rcvr + args
                    // pushed; arg N is at offset (nArgs-1-N) below TOS,
                    // i.e. stack_[size-nArgs+N].
                    uint32_t tempIdx = static_cast<uint32_t>(v0.literal);
                    if (tempIdx >= nArgs) return false;
                    if (stack_.size() < nArgs + 1) return false;
                    inlineOp = Op::kLoadReceiver; // unused — caller-arg substitution
                    inlineTy = Type::kOop;
                    // Special handling: the inlined value IS one of the
                    // caller's stack values.  Skip the normal newValue()
                    // path and re-use the existing arg id directly.
                    uint32_t argId = stack_[stack_.size() - nArgs + tempIdx];
                    // Emit just the guard; result is argId.
                    std::vector<uint32_t> guardOps;
                    guardOps.reserve(stack_.size() + 1);
                    guardOps.push_back(recvId);
                    for (uint32_t s : stack_) guardOps.push_back(s);
                    uint64_t guardLit = (hit->classOop & 0x3FFFFFu)
                                      | (static_cast<uint64_t>(bcOffset) << 32);
                    out_.newValue(currentBlock_, Op::kGuardClass, Type::kOop,
                                  std::move(guardOps), guardLit);
                    for (uint32_t i = 0; i < nArgs + 1; i++) stack_.pop_back();
                    stack_.push_back(argId);
                    g_inlinesEmitted++;
                    g_totalHintsConsumed++;
                    return true;
                }
                default: return false;
                }
            }
            // 2-value self-send chain: kLoadReceiver + kSendUnspeculated.
            // The lifter terminates at the send, so callee `^ self foo`
            // reads as exactly this 2-value sequence with no kReturn
            // because kSendUnspeculated IS the block terminator.
            // Recursively inline the inner self-send.
            else if (v0.op == Op::kLoadReceiver
                  && v1.op == Op::kSendUnspeculated
                  && !v1.operands.empty() && v1.operands[0] == v0.id) {
                if (!g_hintProvider) return false;
                uint32_t innerNArgs = (uint32_t)((v1.literal >> 16) & 0xFF);
                uint32_t innerBcOff = (uint32_t)(v1.literal >> 24);
                if (innerNArgs != 0) return false;  // self-send only
                std::vector<InlineHint> innerHints =
                    g_hintProvider(calleeOop);
                const InlineHint* innerHit = nullptr;
                for (const auto& ih : innerHints) {
                    if (ih.bcOffset == innerBcOff) {
                        innerHit = &ih; break;
                    }
                }
                if (!innerHit) return false;
                if (innerHit->targetMethod == 0) return false;
                if ((innerHit->targetMethod & 0x7) != 0) return false;
                if (innerHit->targetMethod < 0x10000) return false;
                if ((innerHit->classOop & 0x3FFFFFu)
                    != (hit->classOop & 0x3FFFFFu)) return false;
                Method innerIR;
                uint32_t innerFailedAt = UINT32_MAX;
                Oop innerOop = Oop::fromRawBits(innerHit->targetMethod);
                g_calleeLiftDepth++;
                LiftResult ir;
                {
                    ClearOuterHints g;
                    ir = Builder::build(innerOop,
                        *g_currentBuildMemory, innerIR, &innerFailedAt);
                }
                g_calleeLiftDepth--;
                if (ir != LiftResult::kOk) return false;
                if (innerIR.values.size() == 2) {
                    const Value& iv0 = innerIR.values[0];
                    const Value& iv1 = innerIR.values[1];
                    if (iv1.op != Op::kReturn) return false;
                    if (iv1.operands.size() != 1
                        || iv1.operands[0] != iv0.id) return false;
                    switch (iv0.op) {
                    case Op::kLoadTrueOop:  inlineOp = Op::kLoadTrueOop;
                                            inlineTy = Type::kOopBool; break;
                    case Op::kLoadFalseOop: inlineOp = Op::kLoadFalseOop;
                                            inlineTy = Type::kOopBool; break;
                    case Op::kConstantOop:  inlineOp = Op::kConstantOop;
                                            inlineTy = Type::kOop;
                                            inlineLit = iv0.literal; break;
                    case Op::kLoadReceiver: inlineOp = Op::kLoadReceiver;
                                            inlineTy = Type::kOop; break;
                    default: return false;
                    }
                } else if (innerIR.values.size() == 3) {
                    const Value& iv0 = innerIR.values[0];
                    const Value& iv1 = innerIR.values[1];
                    const Value& iv2 = innerIR.values[2];
                    if (iv0.op != Op::kLoadReceiver) return false;
                    if (iv1.op != Op::kLoadInstVar) return false;
                    if (iv1.operands.size() != 1
                        || iv1.operands[0] != iv0.id) return false;
                    if (iv2.op != Op::kReturn) return false;
                    if (iv2.operands.size() != 1
                        || iv2.operands[0] != iv1.id) return false;
                    inlineOp = Op::kLoadInstVar;
                    inlineTy = Type::kOop;
                    inlineLit = iv1.literal;
                    inlineOps.push_back(recvId);
                } else {
                    return false;
                }
            }
            else {
                return false;
            }
        } else if (calleeIR.values.size() == 3) {
            const Value& v0 = calleeIR.values[0];
            const Value& v1 = calleeIR.values[1];
            const Value& v2 = calleeIR.values[2];
            if (v0.op != Op::kLoadReceiver) return false;
            // 3-value getter: kLoadReceiver + kLoadInstVar + kReturn.
            if (v1.op == Op::kLoadInstVar
             && v1.operands.size() == 1 && v1.operands[0] == v0.id
             && v2.op == Op::kReturn
             && v2.operands.size() == 1 && v2.operands[0] == v1.id) {
                inlineOp = Op::kLoadInstVar;
                inlineTy = Type::kOop;
                inlineLit = v1.literal;
                inlineOps.push_back(recvId);
            }
            // 3-value (self ivar) foo: kLoadReceiver + kLoadInstVar +
            // kSendUnspeculated(ivar).  Send terminates lifting so no
            // trailing kReturn.  Inline: outer guard on caller's recv,
            // load ivar, INNER guard on the ivar's class (different
            // class → must guard separately), then inline inner const-
            // return body.
            else if (v1.op == Op::kLoadInstVar
                  && v1.operands.size() == 1 && v1.operands[0] == v0.id
                  && v2.op == Op::kSendUnspeculated
                  && !v2.operands.empty() && v2.operands[0] == v1.id) {
                if (!g_hintProvider) return false;
                uint32_t innerNArgs = (uint32_t)((v2.literal >> 16) & 0xFF);
                uint32_t innerBcOff = (uint32_t)(v2.literal >> 24);
                if (innerNArgs != 0) return false;  // self-send only
                std::vector<InlineHint> innerHints =
                    g_hintProvider(calleeOop);
                const InlineHint* innerHit = nullptr;
                for (const auto& ih : innerHints) {
                    if (ih.bcOffset == innerBcOff) {
                        innerHit = &ih; break;
                    }
                }
                if (!innerHit) return false;
                if (innerHit->targetMethod == 0) return false;
                if ((innerHit->targetMethod & 0x7) != 0) return false;
                if (innerHit->targetMethod < 0x10000) return false;
                Method innerIR;
                uint32_t innerFailedAt = UINT32_MAX;
                Oop innerOop = Oop::fromRawBits(innerHit->targetMethod);
                g_calleeLiftDepth++;
                LiftResult ir;
                {
                    ClearOuterHints g;
                    ir = Builder::build(innerOop,
                        *g_currentBuildMemory, innerIR, &innerFailedAt);
                }
                g_calleeLiftDepth--;
                if (ir != LiftResult::kOk) return false;
                if (innerIR.values.size() != 2) return false;
                const Value& iv0 = innerIR.values[0];
                const Value& iv1 = innerIR.values[1];
                if (iv1.op != Op::kReturn) return false;
                if (iv1.operands.size() != 1
                    || iv1.operands[0] != iv0.id) return false;
                Op  innerInlineOp;
                Type innerInlineTy;
                uint64_t innerInlineLit = 0;
                switch (iv0.op) {
                case Op::kLoadTrueOop:  innerInlineOp = Op::kLoadTrueOop;
                                        innerInlineTy = Type::kOopBool; break;
                case Op::kLoadFalseOop: innerInlineOp = Op::kLoadFalseOop;
                                        innerInlineTy = Type::kOopBool; break;
                case Op::kConstantOop:  innerInlineOp = Op::kConstantOop;
                                        innerInlineTy = Type::kOop;
                                        innerInlineLit = iv0.literal; break;
                default: return false;
                }
                // Emit: outer guard, load ivar, inner guard on ivar.
                std::vector<uint32_t> outerGuardOps;
                outerGuardOps.reserve(stack_.size() + 1);
                outerGuardOps.push_back(recvId);
                for (uint32_t s : stack_) outerGuardOps.push_back(s);
                uint64_t outerGuardLit = (hit->classOop & 0x3FFFFFu)
                                       | (static_cast<uint64_t>(bcOffset) << 32);
                out_.newValue(currentBlock_, Op::kGuardClass, Type::kOop,
                              std::move(outerGuardOps), outerGuardLit);
                uint32_t ivarId = out_.newValue(currentBlock_,
                                                 Op::kLoadInstVar,
                                                 Type::kOop,
                                                 /*operands=*/{recvId},
                                                 /*literal=*/v1.literal);
                std::vector<uint32_t> innerGuardOps;
                innerGuardOps.reserve(stack_.size() + 1);
                innerGuardOps.push_back(ivarId);
                for (uint32_t s : stack_) innerGuardOps.push_back(s);
                uint64_t innerGuardLit = (innerHit->classOop & 0x3FFFFFu)
                                       | (static_cast<uint64_t>(bcOffset) << 32);
                out_.newValue(currentBlock_, Op::kGuardClass, Type::kOop,
                              std::move(innerGuardOps), innerGuardLit);
                uint32_t innerInlineId = out_.newValue(currentBlock_,
                                                        innerInlineOp,
                                                        innerInlineTy,
                                                        {}, innerInlineLit);
                for (uint32_t i = 0; i < nArgs + 1; i++) stack_.pop_back();
                stack_.push_back(innerInlineId);
                g_inlinesEmitted++;
                g_totalHintsConsumed++;
                return true;
            }
            // 3-value chain: kLoadReceiver + kSendUnspeculated(v0)
            //                + kReturn(v1).  Recursively inline the
            //                inner self-send when its target is also
            //                a const-return / getter via the callee's
            //                own JIT IC.  No inner guard needed: the
            //                outer guard already proves the receiver
            //                is hit->classOop, and the inner send's
            //                receiver IS that same value.
            else if (v1.op == Op::kSendUnspeculated
                  && !v1.operands.empty() && v1.operands[0] == v0.id
                  && v2.op == Op::kReturn
                  && v2.operands.size() == 1 && v2.operands[0] == v1.id) {
                if (!g_hintProvider) return false;
                // The send literal packs (selIdx | nArgs<<16 | bcOffset<<24).
                uint32_t innerNArgs = (uint32_t)((v1.literal >> 16) & 0xFF);
                uint32_t innerBcOff = (uint32_t)(v1.literal >> 24);
                if (innerNArgs != 0) return false;  // self-send only
                std::vector<InlineHint> innerHints =
                    g_hintProvider(calleeOop);
                const InlineHint* innerHit = nullptr;
                for (const auto& ih : innerHints) {
                    if (ih.bcOffset == innerBcOff) {
                        innerHit = &ih; break;
                    }
                }
                if (!innerHit) return false;
                if (innerHit->targetMethod == 0) return false;
                if ((innerHit->targetMethod & 0x7) != 0) return false;
                if (innerHit->targetMethod < 0x10000) return false;
                // The outer guard proved rcvr is class hit->classOop.
                // The inner IC observed that same class — sanity-check.
                if ((innerHit->classOop & 0x3FFFFFu)
                    != (hit->classOop & 0x3FFFFFu)) return false;
                Method innerIR;
                uint32_t innerFailedAt = UINT32_MAX;
                Oop innerOop = Oop::fromRawBits(innerHit->targetMethod);
                g_calleeLiftDepth++;
                LiftResult ir;
                {
                    ClearOuterHints g;
                    ir = Builder::build(innerOop,
                        *g_currentBuildMemory, innerIR, &innerFailedAt);
                }
                g_calleeLiftDepth--;
                if (ir != LiftResult::kOk) return false;
                // Inner pattern: accept either 2-value const-return
                // or 3-value ivar-getter.
                if (innerIR.values.size() == 2) {
                    const Value& iv0 = innerIR.values[0];
                    const Value& iv1 = innerIR.values[1];
                    if (iv1.op != Op::kReturn) return false;
                    if (iv1.operands.size() != 1
                        || iv1.operands[0] != iv0.id) return false;
                    switch (iv0.op) {
                    case Op::kLoadTrueOop:  inlineOp = Op::kLoadTrueOop;
                                            inlineTy = Type::kOopBool; break;
                    case Op::kLoadFalseOop: inlineOp = Op::kLoadFalseOop;
                                            inlineTy = Type::kOopBool; break;
                    case Op::kConstantOop:  inlineOp = Op::kConstantOop;
                                            inlineTy = Type::kOop;
                                            inlineLit = iv0.literal; break;
                    case Op::kLoadReceiver: inlineOp = Op::kLoadReceiver;
                                            inlineTy = Type::kOop; break;
                    default: return false;
                    }
                } else if (innerIR.values.size() == 3) {
                    const Value& iv0 = innerIR.values[0];
                    const Value& iv1 = innerIR.values[1];
                    const Value& iv2 = innerIR.values[2];
                    if (iv0.op != Op::kLoadReceiver) return false;
                    if (iv1.op != Op::kLoadInstVar) return false;
                    if (iv1.operands.size() != 1
                        || iv1.operands[0] != iv0.id) return false;
                    if (iv2.op != Op::kReturn) return false;
                    if (iv2.operands.size() != 1
                        || iv2.operands[0] != iv1.id) return false;
                    inlineOp = Op::kLoadInstVar;
                    inlineTy = Type::kOop;
                    inlineLit = iv1.literal;
                    inlineOps.push_back(recvId);
                } else {
                    return false;
                }
            }
            else {
                return false;
            }
        } else {
            return false;
        }

        // Emit kGuardClass.  Operands: receiver, then full simulated
        // stack (so deopt can re-push everything for the interpreter).
        // Literal: lo32 = expectedClassIdx, hi32 = bcOffset.
        std::vector<uint32_t> guardOps;
        guardOps.reserve(stack_.size() + 1);
        guardOps.push_back(recvId);
        for (uint32_t s : stack_) guardOps.push_back(s);
        uint64_t guardLit = (hit->classOop & 0x3FFFFFu)
                          | (static_cast<uint64_t>(bcOffset) << 32);
        out_.newValue(currentBlock_, Op::kGuardClass, Type::kOop,
                      std::move(guardOps), guardLit);

        // Emit the inlined value.
        uint32_t inlineId = out_.newValue(currentBlock_, inlineOp,
                                           inlineTy, std::move(inlineOps),
                                           inlineLit);

        // Pop rcvr+args, push the inlined value.
        for (uint32_t i = 0; i < nArgs + 1; i++) stack_.pop_back();
        stack_.push_back(inlineId);

        g_inlinesEmitted++;
        g_totalHintsConsumed++;
        return true;
    }

    void recordFramepoint(uint32_t valueId, uint32_t bcOffset) {
        out_.framepoints.push_back({
            valueId,
            static_cast<uint16_t>(bcOffset),
            out_.values[valueId].operands,
        });
        g_totalSendsLifted++;
        if (!inlineHints_) return;
        for (const auto& h : *inlineHints_) {
            if (h.bcOffset != bcOffset) continue;
            g_totalHintsConsumed++;
            // Phase 4 Step 2: probe-lift the callee for measurement.
            // Skip if memory not available, recursion deep, or hint
            // missing a target.  Only attempt when the targetMethod
            // looks like a tagged Oop (immediates have low bits set).
            if (g_currentBuildMemory == nullptr) break;
            if (g_calleeLiftDepth >= 1) break;
            if (h.targetMethod == 0) break;
            // h.targetMethod comes from T1 IC slot 1 — that slot
            // holds a CompiledMethod Oop only for the J2J path; for
            // inlined getters/setters/returns-self it's a flag-encoded
            // value, not an Oop.  Reject anything that isn't a
            // plausible heap object Oop (low bits clear, address-like).
            if ((h.targetMethod & 0x7) != 0) break;
            if (h.targetMethod < 0x10000) break;
            g_calleeLiftAttempts++;
            g_calleeLiftDepth++;
            Method calleeIR;
            uint32_t calleeFailedAt = UINT32_MAX;
            Oop calleeOop = Oop::fromRawBits(h.targetMethod);
            LiftResult cr;
            {
                ClearOuterHints g;
                cr = Builder::build(calleeOop,
                    *g_currentBuildMemory, calleeIR, &calleeFailedAt);
            }
            g_calleeLiftDepth--;
            if (cr == LiftResult::kOk) {
                g_calleeLiftSuccess++;
                g_calleeBytecodesLifted += calleeIR.values.size();
                if (std::getenv("PHARO_SISTA_INLINE_DUMP")) {
                    static int dumpCount = 0;
                    if (dumpCount++ < 8) {
                        std::fprintf(stderr,
                            "[SISTA-CALLEE-OK] target=0x%llx values=%zu blocks=%zu",
                            (unsigned long long)h.targetMethod,
                            calleeIR.values.size(),
                            calleeIR.blocks.size());
                        for (const auto& v : calleeIR.values) {
                            std::fprintf(stderr, " %s",
                                OpInfo::name(v.op));
                        }
                        std::fprintf(stderr, "\n");
                    }
                }
            } else if (std::getenv("PHARO_SISTA_INLINE_DBG")) {
                static int dbgCount = 0;
                if (dbgCount++ < 16) {
                    int fmt = -1;
                    int isObj = calleeOop.isObject() ? 1 : 0;
                    if (isObj) {
                        fmt = (int)calleeOop.asObjectPtr()->format();
                    }
                    std::fprintf(stderr,
                        "[SISTA-CALLEE-FAIL] result=%d failedAt=%u "
                        "target=0x%llx isObj=%d fmt=%d\n",
                        (int)cr, calleeFailedAt,
                        (unsigned long long)h.targetMethod,
                        isObj, fmt);
                }
            }
            break;
        }
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
    // B2 splice: bitmap of literals[0..15] that hold #inject:into:.
    uint16_t               injectIntoSelectorMask_ = 0;
    // B2 splice: bitmap of literals[0..15] that hold #to:.  Used for
    // the Interval-inject pattern.
    uint16_t               toSelectorMask_ = 0;
    // Phase 3: emit kPrimTagCheckInt before inlined arith ops.
    bool                   typeCheckArith_ = false;
    // B2 splice: when sub-lifting a block for splice, treat block-
    // return bytecodes as local kReturn rather than bailing.
    bool                   blockReturnAsLocalReturn_ = false;
    // Phase 4 Step 1: profile-guided inline hints (or nullptr).
    const std::vector<InlineHint>* inlineHints_ = nullptr;
    // B2 splice: ObjectMemory used by the peephole's block sub-lift.
    // Set by Builder::build so we don't have to use the global.
    ObjectMemory*          memory_ = nullptr;

    // B2 splice: map from bcOffset-of-PushFullBlock to the block-IR
    // slot index in out_.inlinedBlocks.  Populated by the pre-pass
    // when PHARO_SISTA_DO_SPLICE=1; consumed by the main lifter's
    // PushFullBlock arm (which emits kCountedLoopDo and skips both
    // bytecodes).  Empty when splice is disabled or no eligible
    // patterns matched.
    std::unordered_map<size_t, uint32_t> spliceAtPushFullBlock_;
    // B2 splice (inject:into: variant): same shape but for Send2
    // #inject:into: instead of SpecialSend(do:).  The intercept emits
    // kCountedLoopInjectInto and skips PushFullBlock+Send2 (4B).
    std::unordered_map<size_t, uint32_t> spliceInjectAtPushFullBlock_;
    // B2 splice (Interval-inject variant): map from to:_offset to the
    // block-IR slot.  Pre-pass records this when the entire pattern
    // <push start> <push stop> Send1#to: <push init> PushFullBlock
    // Send2#inject:into: is recognized.  Lifter at to: emits
    // kInterval; inject:into: intercept upgrades to
    // kCountedLoopIntervalInjectInto.
    std::unordered_map<size_t, uint32_t> intervalInjectAtTo_;
    // B2 splice (Interval-do variant): same idea — intervalDoAtTo_ maps
    // a recognized to: bcOffset to the block-IR slot, and
    // spliceDoAtPushFullBlock_ tells the PushFullBlock intercept to
    // emit kCountedLoopIntervalDo when receiver is a kInterval marker.
    std::unordered_map<size_t, uint32_t> intervalDoAtTo_;
    std::unordered_map<size_t, uint32_t> spliceDoAtPushFullBlock_;
    // Closure-accumulator splice (Array do:): pfbOff → packed metadata
    // (low 8 bits = slot, next 8 = arithCode 0/1/2, next 8 = outerTemp).
    std::unordered_map<size_t, uint64_t> spliceAccumAtPushFullBlock_;
    // Closure-accumulator splice (Interval do:): same packed layout as
    // spliceAccumAtPushFullBlock_, but the receiver came from
    // <push start> <push stop> Send1#to:.  intervalDoAccumAtTo_ marks
    // the to:-offset so the lifter emits kInterval there;
    // spliceIvDoAccumAtPushFullBlock_ tells the PushFullBlock arm to
    // emit kCountedLoopIntervalDoAccum.
    std::unordered_map<size_t, uint64_t> intervalDoAccumAtTo_;
    std::unordered_map<size_t, uint64_t> spliceIvDoAccumAtPushFullBlock_;
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
    static const bool injectIntoSplice =
        std::getenv("PHARO_SISTA_DO_SPLICE") != nullptr;
    uint16_t injectIntoMask = 0;
    uint16_t toMask = 0;
    if (inlineYourself || inlineIdentityEq || injectIntoSplice) {
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
            // #inject:into: is 12 bytes.
            if (injectIntoSplice && bs == 12
                && std::memcmp(bytes, "inject:into:", 12) == 0) {
                injectIntoMask |= (1u << i);
            }
            // #to: is 3 bytes — needed for Interval-inject splice.
            if (injectIntoSplice && bs == 3
                && std::memcmp(bytes, "to:", 3) == 0) {
                toMask |= (1u << i);
            }
        }
    }

    static const bool typeCheckArith =
        std::getenv("PHARO_SISTA_INLINE_ARITH") != nullptr;

    LinearLifter l(bytecodes, bytecodeSize, numArgs, numTemps, out);
    l.setMemory(&memory);
    if (haveSS) l.setSpecialSendArgCounts(ssArgCounts);
    if (inlineableMask) l.setInlineableSelectorBitmap(inlineableMask);
    if (identityEqMask) l.setIdentityEqSelectorBitmap(identityEqMask);
    if (identityNeqMask) l.setIdentityNeqSelectorBitmap(identityNeqMask);
    if (injectIntoMask) l.setInjectIntoSelectorBitmap(injectIntoMask);
    if (toMask) l.setToSelectorBitmap(toMask);
    if (typeCheckArith) l.setTypeCheckArith(true);
    if (g_subLiftAsBlockReturnLocal) {
        l.setBlockReturnAsLocalReturn(true);
    }
    if (g_currentBuildHints) {
        l.setInlineHints(g_currentBuildHints);
    }
    return l.run(failedAtBytecode);
}

// Phase 4 Step 1: profile-guided wrapper.  Sets a thread-local hint
// pointer that the main build() picks up when constructing
// LinearLifter, then invokes the regular build path.
LiftResult Builder::buildWithHints(Oop compiledMethod, ObjectMemory& memory,
                                    Method& out,
                                    const std::vector<InlineHint>* hints,
                                    uint32_t* failedAtBytecode) {
    if (hints) {
        g_totalMonomorphicHints += hints->size();
    }
    g_currentBuildHints = hints;
    g_currentBuildMemory = &memory;
    LiftResult r = build(compiledMethod, memory, out, failedAtBytecode);
    g_currentBuildHints = nullptr;
    g_currentBuildMemory = nullptr;
    return r;
}

void Builder::setHintProvider(HintProvider p) {
    g_hintProvider = std::move(p);
}

// Phase 4 Step 1 stats accessors.
uint64_t Builder::totalSendsLifted()      { return g_totalSendsLifted; }
uint64_t Builder::totalMonomorphicHints() { return g_totalMonomorphicHints; }
uint64_t Builder::totalHintsConsumed()    { return g_totalHintsConsumed; }
void Builder::resetInlineHintStats() {
    g_totalSendsLifted = 0;
    g_totalMonomorphicHints = 0;
    g_totalHintsConsumed = 0;
    g_calleeLiftAttempts = 0;
    g_calleeLiftSuccess = 0;
    g_calleeBytecodesLifted = 0;
    g_inlinesEmitted = 0;
}
void Builder::dumpInlineHintStats() {
    std::fprintf(stderr,
        "[SISTA-INLINE] sends-lifted=%llu hints-provided=%llu hints-consumed=%llu "
        "callees-attempted=%llu callees-lifted=%llu callee-values=%llu "
        "inlines-emitted=%llu\n",
        (unsigned long long)g_totalSendsLifted,
        (unsigned long long)g_totalMonomorphicHints,
        (unsigned long long)g_totalHintsConsumed,
        (unsigned long long)g_calleeLiftAttempts,
        (unsigned long long)g_calleeLiftSuccess,
        (unsigned long long)g_calleeBytecodesLifted,
        (unsigned long long)g_inlinesEmitted);
}

}  // namespace sista
}  // namespace pharo
