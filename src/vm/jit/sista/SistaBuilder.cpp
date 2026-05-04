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

// Phase 5 Step 1 counters: IC-site polymorphism histogram.  Indexed
// by entry count (0 unused; 1..6 are valid IC degrees).  Filled by
// recordPolyDegree() called from extractInlineHintsForMethod().
static uint64_t g_polyDegreeHisto[8]    = {0};

// Phase 4 sizing: callees we successfully lifted but couldn't recognize.
// Key = (size << 32) | (op0 << 24) | (op1 << 16) | (op2 << 8) | op3.
// Dump prints the top entries so we know which shapes to add next.
static std::map<uint64_t, uint64_t> g_unrecognizedCalleeShapes;
static void recordUnrecognizedShape(const Method& m) {
    if (m.values.size() == 0 || m.values.size() > 255) return;
    uint8_t op0 = m.values.size() > 0 ? (uint8_t)m.values[0].op : 0;
    uint8_t op1 = m.values.size() > 1 ? (uint8_t)m.values[1].op : 0;
    uint8_t op2 = m.values.size() > 2 ? (uint8_t)m.values[2].op : 0;
    uint8_t op3 = m.values.size() > 3 ? (uint8_t)m.values[3].op : 0;
    uint64_t key = ((uint64_t)m.values.size() << 32)
                 | ((uint64_t)op0 << 24)
                 | ((uint64_t)op1 << 16)
                 | ((uint64_t)op2 << 8)
                 | (uint64_t)op3;
    g_unrecognizedCalleeShapes[key]++;
}

// Hint pointer set by buildWithHints() and consumed by build() when
// constructing LinearLifter.  Single-threaded; static suffices.
static const std::vector<InlineHint>* g_currentBuildHints = nullptr;

// Memory pointer set by buildWithHints() so recordFramepoint can probe-lift
// callees referenced by inline hints.  Single-threaded; static suffices.
static ObjectMemory* g_currentBuildMemory = nullptr;

// Per-bytecode entry: when non-zero, build() lifts only the method
// suffix [g_buildStartBcOffset..end) instead of the full [0..end).
// Set by buildFromOffset; build() consumes it on the way down to
// LinearLifter and then resets to 0.  Single-threaded; static
// suffices.
static uint32_t g_buildStartBcOffset = 0;

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

// Item #6 helper: returns true if HELPER_SENDS-style activation is
// SAFE for methods of this class.  Skips UI/system classes whose
// short methods produce DNU cascades when their blocks splice
// (documented in project_helper_sends_gate.md).  Forward-declared
// here; defined after LinearLifter class.
static bool sistaClassIsHelperSafe(const std::string& className);

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

    // B2 splice (collect: variant): bitmap of literals that hold #collect:.
    void setCollectSelectorBitmap(uint16_t mask) {
        collectSelectorMask_ = mask;
    }

    // Item #6 (class-based HELPER_SENDS gate): name of the defining
    // class for the method being lifted.  Used to skip UI/system
    // classes from HELPER_SENDS activation.  Empty string = unknown
    // (treat as helper-unsafe, conservative).
    void setMethodClassName(std::string name) {
        methodClassName_ = std::move(name);
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
            // Default-on as of 2026-05-01.  Earlier opt-in gate was due
            // to a now-fixed FFI #oopForObject: crash.  Set
            // PHARO_NO_SISTA_DO_SPLICE=1 to opt out.
            static const bool splice =
                std::getenv("PHARO_NO_SISTA_DO_SPLICE") == nullptr;
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
                        //
                        // 2026-05-01: skip this rejection when
                        // PHARO_SISTA_HELPER_SENDS=1.  Helper-sends
                        // makes the main lift continue past
                        // kSendUnspeculated by emitting kSendCallHelper
                        // (interp drives the send to completion, lift
                        // continues).  The splice intercept at
                        // PushFullBlock CAN fire even with prior sends
                        // — unblocks bench-suite runSum-style patterns
                        // (sends to asArray / millisecondClockValue
                        // before the do:).
                        // Match the default-on flip in the main gate.
                        static const bool helperSends = []() {
                            return std::getenv("PHARO_NO_SISTA_HELPER_SENDS")
                                   == nullptr;
                        }();
                        if (sawLiftTerminator && !helperSends) {
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
                        // 2026-05-01: closure-capture support.  If
                        // numCopied=1, the byte before PushFullBlock
                        // must push the TempVector via PushTemp T_vec
                        // (single-byte 0x40-0x4B).  Stash the outer
                        // temp index so the emit path can pop the
                        // vec from the simulator stack and pass it as
                        // operand[1] to kCountedLoopDo.  Lowering uses
                        // it for kLoadTempInVec / kStoreTempInVec
                        // emission with deopt-buffered writes (accReg
                        // pre-loaded once, committed only on
                        // successful loop exit).
                        uint32_t doNumCopied =
                            (uint32_t)(lastFullBlockFlags & 0x3F);
                        uint32_t doOuterVecTemp = 0;
                        bool doHasCapture = false;
                        if (ok && doNumCopied == 1) {
                            if (lastPushFullBlockStart < 1) {
                                ok = false;
                                rejectReason = "no room for vec push";
                            } else {
                                uint8_t prevOp =
                                    bc_[lastPushFullBlockStart - 1];
                                if (prevOp < 0x40 || prevOp > 0x4B) {
                                    ok = false;
                                    rejectReason = "vec source not PushTemp";
                                } else {
                                    doOuterVecTemp =
                                        (uint32_t)(prevOp - 0x40);
                                    doHasCapture = true;
                                }
                            }
                        } else if (ok && doNumCopied != 0) {
                            ok = false;
                            rejectReason = "numCopied > 1";
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
                        // Multi-block IR (block contains internal
                        // jumps with phi merges) is not supported by
                        // the kCountedLoopDo lowering.  Reject up
                        // front to avoid cache-poisoning the entire
                        // method's Sista compile.
                        if (ok && blockIR && blockIR->blocks.size() != 1) {
                            ok = false;
                            rejectReason = "multi-block IR";
                        }
                        // 2026-05-01: closure-vec op check.  If
                        // doHasCapture, scan for kLoadTempInVec /
                        // kStoreTempInVec.  All such ops must reference
                        // the SAME slot — multi-slot needs separate
                        // accReg per slot which the current lowering
                        // doesn't track.  All ops must use vec-temp 1
                        // (the only TempVector accessible from a
                        // numCopied=1 block — the captured slot is at
                        // block-temp index numArgs == 1).
                        uint32_t doVecSlot = UINT32_MAX;
                        if (ok && doHasCapture && blockIR) {
                            for (const auto& bv : blockIR->values) {
                                if (bv.op == Op::kLoadTempInVec
                                    || bv.op == Op::kStoreTempInVec) {
                                    // literal layout (per SistaIR.hpp):
                                    // high 32 = tempIdxOfVec, low 32 = slot
                                    uint32_t tempIdx =
                                        (uint32_t)(bv.literal >> 32);
                                    uint32_t slot =
                                        (uint32_t)(bv.literal & 0xFFFFFFFFu);
                                    if (tempIdx != 1) {
                                        ok = false;
                                        rejectReason = "vec tempIdx != 1";
                                        rejectedOp = bv.op;
                                        break;
                                    }
                                    if (doVecSlot == UINT32_MAX) {
                                        doVecSlot = slot;
                                    } else if (doVecSlot != slot) {
                                        ok = false;
                                        rejectReason = "multi-slot vec";
                                        rejectedOp = bv.op;
                                        break;
                                    }
                                }
                            }
                        }
                        // Narrowed to ops the kCountedLoopDo lowering
                        // actually handles.  Earlier admission of
                        // comparison ops (kPrimLtInt etc.) caused
                        // cache-poisoning: pre-pass admits the block
                        // but lowering rejects + caches a permanent
                        // nullptr for the method.  do: discards the
                        // block result so comparison ops are dead-code
                        // anyway; their fused-branch use case requires
                        // a kBranchIfTrue/False which terminates the
                        // sub-lift.
                        if (ok && blockIR) {
                            for (const auto& bv : blockIR->values) {
                                switch (bv.op) {
                                case Op::kLoadReceiver:
                                case Op::kLoadInstVar:
                                case Op::kLoadTrueOop:
                                case Op::kLoadFalseOop:
                                case Op::kLoadLiteral:
                                case Op::kConstantOop:
                                case Op::kPhi:
                                case Op::kReturn:
                                case Op::kPrimAddInt:
                                case Op::kPrimSubInt:
                                case Op::kPrimMulInt:
                                case Op::kPrimTagCheckInt:
                                    // Tag-check is harmless at the splice
                                    // level (lowering passes it through
                                    // and does its own check at the do:
                                    // bcOffset).
                                    break;
                                case Op::kLoadTemp:
                                    // do: passes 1 block arg (e) at
                                    // temp index 0.  Higher indices
                                    // are block-local temps; admit
                                    // them — the lowering binds them
                                    // to fresh registers via the
                                    // kStoreTemp emit.  Without this,
                                    // any block that needs an
                                    // intermediate (`[:e | | t | t :=
                                    // e + 1. ...]`) is rejected.
                                    break;
                                case Op::kStoreTemp:
                                    // 2026-05-01: closure-capture
                                    // support.  do: discards the block's
                                    // return value, so writes to block-
                                    // local temps are dead-code from the
                                    // outer's view — safe to admit.
                                    // Lowering tracks the value in a
                                    // register but emits no store.
                                    // Writes to captured slots (numArgs
                                    // <= idx < numArgs+numCopied) would
                                    // be VISIBLE — those still need
                                    // kStoreTempInVec / write-buffering
                                    // for deopt safety, which this
                                    // change does NOT add.  Reject
                                    // captured-slot stores by checking
                                    // the index against the block's
                                    // numArgs+numCopied bound at lower
                                    // time (here we admit and let
                                    // lowering enforce).
                                    break;
                                case Op::kLoadTempInVec:
                                case Op::kStoreTempInVec:
                                    // 2026-05-01: closure-vec accumulator
                                    // pattern (sum := sum + e).  Only
                                    // admit when doHasCapture and the
                                    // single-slot constraint above is
                                    // satisfied (validated post-loop).
                                    if (!doHasCapture) {
                                        ok = false;
                                        rejectReason = "vec op without capture";
                                        rejectedOp = bv.op;
                                    }
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
                            // 2026-05-01: stash capture metadata so
                            // the splice intercept knows to pop vec
                            // off the simulator stack and pass it as
                            // operand[1] to kCountedLoopDo.  Lowering
                            // reads the same map (or, equivalently,
                            // operand presence) to emit pre-load /
                            // post-store of accReg around the loop.
                            if (doHasCapture) {
                                uint64_t packed =
                                    (uint64_t)doOuterVecTemp
                                  | ((uint64_t)doVecSlot << 16)
                                  | ((uint64_t)1 << 32);
                                doVecCaptureAtPushFullBlock_[
                                    (size_t)lastPushFullBlockStart]
                                    = packed;
                            }
                            static int spliceCount = 0;
                            if (spliceCount++ < 16) {
                                std::fprintf(stderr,
                                    "[SISTA-SPLICE-CAND] pushFullBlock=%d "
                                    "doBC=%zu litIdx=%d slot=%u "
                                    "capture=%d outerVec=%u vecSlot=%u\n",
                                    lastPushFullBlockStart, i,
                                    lastFullBlockLitIdx, slot,
                                    (int)doHasCapture,
                                    doOuterVecTemp,
                                    doHasCapture
                                        ? doVecSlot
                                        : 0u);
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
            std::getenv("PHARO_NO_SISTA_DO_SPLICE") == nullptr;
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
                                "blockLen=%zu lead=%02x (want 9-10, "
                                "lead=fb)\n",
                                pfbOff, bLen,
                                bLen > 0 ? bbc[0] : 0);
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
            std::getenv("PHARO_NO_SISTA_DO_SPLICE") == nullptr;
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
                    // numCopied: 0 (no capture) or 1 (single TempVec).
                    // For numCopied=1 the byte before PushFullBlock
                    // must be PushTemp T_vec (single-byte 0x40-0x4B);
                    // outerVecTemp is recorded for the splice intercept
                    // (which pops vec off simulator stack) and the
                    // lowering's block-body kLoadTempInVec emission.
                    uint32_t numCopied =
                        (uint32_t)(lastFullBlockFlags & 0x3F);
                    uint32_t injectOuterVecTemp = 0;
                    bool injectHasCapture = false;
                    if (ok && numCopied == 1) {
                        if (lastPushFullBlockStart < 1) {
                            ok = false;
                            rejectReason = "no room for vec push";
                        } else {
                            uint8_t prevOp =
                                bc_[lastPushFullBlockStart - 1];
                            if (prevOp < 0x40 || prevOp > 0x4B) {
                                ok = false;
                                rejectReason = "vec source not PushTemp";
                            } else {
                                injectOuterVecTemp =
                                    (uint32_t)(prevOp - 0x40);
                                injectHasCapture = true;
                            }
                        }
                    } else if (ok && numCopied != 0) {
                        ok = false;
                        rejectReason = "numCopied > 1";
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

                    // Multi-block IR is rejected by lowering — match
                    // up front to avoid cache-poisoning.
                    if (ok && blockIR && blockIR->blocks.size() != 1) {
                        ok = false;
                        rejectReason = "multi-block IR";
                    }
                    // Verify block is splice-simple.  Comparison and
                    // identity ops are admitted now that the
                    // inject:into: lowering emits cmp + csel for them
                    // (boolean result can become the new acc).
                    if (ok && blockIR) {
                        for (const auto& bv : blockIR->values) {
                            switch (bv.op) {
                            case Op::kLoadReceiver:
                            case Op::kLoadInstVar:
                            case Op::kLoadTrueOop:
                            case Op::kLoadFalseOop:
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
                            case Op::kLoadTemp:
                                // inject:into: passes 2 args:
                                // temp 0 = acc, temp 1 = elem.  With
                                // numCopied=1 and direct capture, the
                                // captured value is at block temp 2.
                                // Lowering binds blockRegs[bv.id] =
                                // injectVecReg in that case.
                                if (bv.literal != 0
                                    && bv.literal != 1
                                    && !(injectHasCapture
                                         && bv.literal == 2)) {
                                    ok = false;
                                    rejectReason = "non-simple block op";
                                    rejectedOp = bv.op;
                                }
                                break;
                            case Op::kLoadTempInVec:
                                // Read-only access to captured
                                // TempVector (mutated-capture form).
                                // Only when pre-pass identified
                                // numCopied=1 with PushTemp T_vec.
                                if (!injectHasCapture
                                    || (bv.literal >> 32) != 1) {
                                    ok = false;
                                    rejectReason = "non-simple block op";
                                    rejectedOp = bv.op;
                                }
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
                        if (injectHasCapture) {
                            outerVecTempForInject_[
                                (size_t)lastPushFullBlockStart] =
                                injectOuterVecTemp;
                        }
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

        // --- B2 splice pre-pass: arr collect: [block] -------------------
        //
        // Pattern: PushFullBlock + Send1 #collect:
        //   - block has 1 arg (e), numCopied=0, splice-simple body.
        //   - receiver is whatever was pushed before PushFullBlock (a
        //     general Oop expected to be Array at runtime; tag-checked
        //     in lowering).
        //
        // Records spliceCollectAtPushFullBlock_[pfbOff] = block-IR slot.
        // The main lift's Send1 arm intercepts and emits
        // kCountedLoopArrayCollect.
        //
        // Default-on (2026-05-02): PHARO_NO_SISTA_COLLECT=1 disables.
        // 10-run bench-suite soak under COLLECT=1 was 10/10 clean, within
        // ±1 ms of default-flag baseline.  Real-world synthetic
        // (`(1 to: 100000) asArray collect: [:e | e + 1]` ×5) collapses
        // 53 ms → 0 ms.  Outer gate PHARO_NO_SISTA_DO_SPLICE still
        // applies (collect uses inject:into: infrastructure as well).
        static const bool collectSplice =
            std::getenv("PHARO_NO_SISTA_COLLECT") == nullptr;
        if (collectSplice && injectSplice && memory_ != nullptr
            && collectSelectorMask_) {
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
                // Send1 with selector matching #collect: at adjacency.
                if (op >= jit::SistaV1::Send1Base
                    && op <= jit::SistaV1::Send1Last
                    && lastPushFullBlockEnd == (int)i) {
                    uint32_t selIdx = op & 0x0F;
                    bool isCollect =
                        ((collectSelectorMask_ >> selIdx) & 1) != 0;
                    if (!isCollect) {
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
                    // numCopied: 0 (no capture) or 1 (single TempVec).
                    // For numCopied=1 we additionally require the byte
                    // immediately before PushFullBlock to be PushTemp T_vec
                    // (single byte 0x40-0x4B) so we know which outer
                    // temp the vec lives in.  outerVecTemp is recorded
                    // for the splice intercept + lowering.
                    uint32_t numCopied =
                        (uint32_t)(lastFullBlockFlags & 0x3F);
                    uint32_t outerVecTemp = 0;
                    bool hasCapture = false;
                    if (ok && numCopied == 1) {
                        if (lastPushFullBlockStart < 1) {
                            ok = false;
                            rejectReason = "no room for vec push";
                        } else {
                            uint8_t prevOp =
                                bc_[lastPushFullBlockStart - 1];
                            if (prevOp < 0x40 || prevOp > 0x4B) {
                                ok = false;
                                rejectReason = "vec source not PushTemp";
                            } else {
                                outerVecTemp = (uint32_t)(prevOp - 0x40);
                                hasCapture = true;
                            }
                        }
                    } else if (ok && numCopied != 0) {
                        ok = false;
                        rejectReason = "numCopied > 1";
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

                    // Multi-block IR rejected by collect: lowering.
                    if (ok && blockIR && blockIR->blocks.size() != 1) {
                        ok = false;
                        rejectReason = "multi-block IR";
                    }
                    // Splice-simple whitelist: loads + arith + cmp +
                    // identity + tag-check + return.  No sends, no
                    // stores.  kLoadLiteral is excluded because of a
                    // hang seen on multi-arith blocks like `(e * 2) + 1`;
                    // root cause unclear, leave restricted until
                    // investigated.  Comparison + identity ops produce
                    // Boolean Oops that become the new element value
                    // (e.g. `arr collect: [:e | e > 10]` makes a Bool
                    // array; `[:e | e == nil]` checks identity).
                    if (ok && blockIR) {
                        for (const auto& bv : blockIR->values) {
                            switch (bv.op) {
                            case Op::kLoadReceiver:
                            case Op::kLoadInstVar:
                            case Op::kLoadTrueOop:
                            case Op::kLoadFalseOop:
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
                            case Op::kLoadTemp:
                                // collect: passes 1 block arg (e) at
                                // temp index 0.  When numCopied=1 and
                                // direct (non-TempVector) capture is
                                // used, the captured value is at
                                // block temp 1.  Lowering binds
                                // blockRegs[bv.id] = collectVecReg in
                                // that case.  Reject other indices.
                                if (bv.literal != 0
                                    && !(hasCapture
                                         && bv.literal == 1)) {
                                    ok = false;
                                    rejectReason = "non-simple block op";
                                    rejectedOp = bv.op;
                                }
                                break;
                            case Op::kLoadTempInVec:
                                // Read-only access to the captured
                                // TempVector (Pharo's mutated-capture
                                // form).  Only valid when numCopied=1
                                // with a PushTemp T_vec source; vecIdx
                                // (high 32 bits of literal) must be 1
                                // (the single capture).
                                if (!hasCapture
                                    || (bv.literal >> 32) != 1) {
                                    ok = false;
                                    rejectReason = "non-simple block op";
                                    rejectedOp = bv.op;
                                }
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
                        spliceCollectAtPushFullBlock_[
                            (size_t)lastPushFullBlockStart] = slot;
                        if (hasCapture) {
                            outerVecTempForCollect_[
                                (size_t)lastPushFullBlockStart] =
                                outerVecTemp;
                        }
                        static int collCandCount = 0;
                        if (collCandCount++ < 16) {
                            std::fprintf(stderr,
                                "[SISTA-COLLECT-CAND] pfbBC=%d "
                                "send1BC=%zu selIdx=%u litIdx=%d slot=%u "
                                "outerVecTemp=%d\n",
                                lastPushFullBlockStart, i,
                                selIdx, lastFullBlockLitIdx, slot,
                                hasCapture ? (int)outerVecTemp : -1);
                        }
                    } else {
                        static int collDiag = 0;
                        if (collDiag++ < 16) {
                            std::fprintf(stderr,
                                "[SISTA-COLLECT-DIAG] send1BC=%zu "
                                "selIdx=%u verdict=%s reject=%s rejOp=%s\n",
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
                        // numCopied=1 capture, same as Array inject.
                        // Records into the SHARED outerVecTempForInject_
                        // map (consumed by the splice intercept).
                        uint32_t numCopied =
                            (uint32_t)(flags & 0x3F);
                        uint32_t ivInjectOuterVecTemp = 0;
                        bool ivInjectHasCaptureLocal = false;
                        if (ok && numCopied == 1) {
                            if (pfbOff < 1) {
                                ok = false;
                                rejectReason = "no room for vec push";
                            } else {
                                uint8_t prevOp = bc_[pfbOff - 1];
                                if (prevOp < 0x40 || prevOp > 0x4B) {
                                    ok = false;
                                    rejectReason = "vec source not PushTemp";
                                } else {
                                    ivInjectOuterVecTemp =
                                        (uint32_t)(prevOp - 0x40);
                                    ivInjectHasCaptureLocal = true;
                                }
                            }
                        } else if (ok && numCopied != 0) {
                            ok = false;
                            rejectReason = "numCopied > 1";
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

                        // Multi-block IR rejected by IV-inject lowering.
                        if (ok && blockIR && blockIR->blocks.size() != 1) {
                            ok = false;
                            rejectReason = "multi-block IR";
                        }
                        // Comparison + identity ops admitted now that
                        // the IV-inject lowering emits cmp + csel for
                        // them (boolean result can become the new acc).
                        if (ok && blockIR) {
                            for (const auto& bv : blockIR->values) {
                                switch (bv.op) {
                                case Op::kLoadReceiver:
                                case Op::kLoadInstVar:
                                case Op::kLoadTrueOop:
                                case Op::kLoadFalseOop:
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
                                case Op::kLoadTemp:
                                    // IV-inject passes 2 args:
                                    // temp 0 = acc, temp 1 = i.  With
                                    // numCopied=1 + direct capture,
                                    // captured value is at temp 2.
                                    if (bv.literal != 0
                                        && bv.literal != 1
                                        && !(ivInjectHasCaptureLocal
                                             && bv.literal == 2)) {
                                        ok = false;
                                        rejectReason = "non-simple block op";
                                        rejectedOp = bv.op;
                                    }
                                    break;
                                case Op::kLoadTempInVec:
                                    // TempVector capture variant.
                                    if (!ivInjectHasCaptureLocal
                                        || (bv.literal >> 32) != 1) {
                                        ok = false;
                                        rejectReason = "non-simple block op";
                                        rejectedOp = bv.op;
                                    }
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
                            if (ivInjectHasCaptureLocal) {
                                outerVecTempForInject_[
                                    (size_t)pfbOff] =
                                    ivInjectOuterVecTemp;
                            }
                            static int ivCount = 0;
                            if (ivCount++ < 16) {
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

                        // Multi-block IR rejected by IV-do lowering.
                        if (ok && blockIR && blockIR->blocks.size() != 1) {
                            ok = false;
                            rejectReason = "multi-block IR";
                        }
                        if (ok && blockIR) {
                            for (const auto& bv : blockIR->values) {
                                switch (bv.op) {
                                case Op::kLoadReceiver:
                                case Op::kLoadInstVar:
                                case Op::kLoadTrueOop:
                                case Op::kLoadFalseOop:
                                case Op::kLoadLiteral:
                                case Op::kConstantOop:
                                case Op::kReturn:
                                case Op::kPrimAddInt:
                                case Op::kPrimSubInt:
                                case Op::kPrimMulInt:
                                case Op::kPrimTagCheckInt:
                                    break;
                                case Op::kLoadTemp:
                                    // IV-do passes 1 arg (i) at temp 0.
                                    // Higher indices not supported.
                                    if (bv.literal != 0) {
                                        ok = false;
                                        rejectReason = "non-simple block op";
                                        rejectedOp = bv.op;
                                    }
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
                            if (ivdCount++ < 16) {
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
        //
        // Default-on (2026-05-02): PHARO_NO_SISTA_IV_DO_ACCUM=1 disables.
        // The earlier 2026-05-01 regression (10/10 → 6/10 + FFI's
        // #oopForObject: stop) no longer reproduces — re-soaked at HEAD
        // as 10/10 clean bench-suite, bench panel parity, large-Interval
        // sum 500000500000 correct, 138K-method system-navigation eval
        // succeeds.  Either the original cause was fixed by intervening
        // splice/IC commits or it was bench-image-state-specific.
        // Outer gate PHARO_NO_SISTA_DO_SPLICE still applies.
        static const bool ivDoAccumSplice =
            std::getenv("PHARO_NO_SISTA_IV_DO_ACCUM") == nullptr;
        if (ivDoAccumSplice && accumSplice && memory_ != nullptr && toSelectorMask_) {
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
                        if (ivacCount++ < 16) {
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

        // --- Detect Pharo-inlined whileTrue: counter loop (PROBE) -----
        //
        // Pharo's bytecode compiler inlines `n timesRepeat: [block]` and
        // `(start to: stop) do: [block]` (literal SmallInt args) as a
        // counted whileTrue: loop with no Send to #timesRepeat:/#do:.
        // Pattern:
        //   PRE_LOOP: pushLitConst LIMIT      <- left on stack
        //             pushOne                 <- 0x51
        //             popIntoTemp X           <- 0xD0+X
        //   LOOP_HEAD: pushTemp X             <- 0x40+X
        //              pushLitConst LIMIT     <- same literal
        //              send <=                 <- 0x64 (ArithBase + 4)
        //              jumpFalse END           <- 0xEF or short jump
        //              ; BLOCK BODY (any whitelist of ops)
        //              pushTemp X
        //              pushOne
        //              send +                  <- 0x60 (ArithBase + 0)
        //              popIntoTemp X
        //              jumpTo LOOP_HEAD        <- ExtendB + ExtJump (0xE1, 0xED)
        //   END:       pop                     <- 0xD8 (discard pre-loop limit)
        //
        // For the bench-suite's `1M blocks` (`1000000 timesRepeat:
        // [counter := counter + 1]`), this pattern emerges with no
        // closure-vec involvement (counter is a plain local temp).
        // Recognizing it lets Sista lift counter+limit-counter to
        // registers, matching simpleLoop's panel speed (~0.7ns/iter).
        //
        // PROBE PHASE 2026-05-01: detect candidates and record into
        // whileTrueAccumPattern_ so the main lift can intercept.  The
        // env-gated diagnostic remains as a kill-switch.  Validation
        // ONLY admits the canonical body=4 shape:
        //   pushTemp X, pushOne/Zero, ArithBase Y, popIntoTemp X
        // where the body's accumTempIdx is the SAME temp on both
        // sides of the arith.  Multi-send bodies (1M getter+yourself)
        // are detected but rejected here — they need helper-sends or
        // a different lowering strategy.
        // Default-on as of 2026-05-01.  Splice only fires when the
        // main lifter reaches preLoopStart (no setup-sends before).
        // Without HELPER_SENDS, methods with prior sends won't trigger
        // — safe.  Set PHARO_NO_SISTA_WHILETRUE=1 to opt out.
        static const bool whileTrueSpliceEnabled =
            std::getenv("PHARO_NO_SISTA_WHILETRUE") == nullptr;
        static const bool probeWhileTrue =
            std::getenv("PHARO_SISTA_PROBE_WHILETRUE") != nullptr;
        if (whileTrueSpliceEnabled || probeWhileTrue) {
            size_t i = 0;
            while (i + 4 <= len_) {
                // Look for ExtendB + ExtJump (4-byte long jump back).
                if (bc_[i] == jit::SistaV1::ExtendB
                    && bc_[i + 2] == jit::SistaV1::ExtJump) {
                    int8_t  extB = (int8_t)bc_[i + 1];
                    int8_t  off8 = (int8_t)bc_[i + 3];
                    int32_t off  = ((int32_t)extB << 8) | (uint8_t)off8;
                    int32_t target = (int32_t)(i + 4) + off;
                    if (target >= 0 && target < (int32_t)i) {
                        size_t t = (size_t)target;
                        if (t + 4 < len_) {
                            uint8_t b0 = bc_[t];
                            uint8_t b1 = bc_[t + 1];
                            uint8_t b2 = bc_[t + 2];
                            uint8_t b3 = bc_[t + 3];
                            bool isPushTemp =
                                b0 >= jit::SistaV1::PushTempBase
                                && b0 <= 0x4B;
                            bool isLitConst = jit::SistaV1::isPushLitConst(b1);
                            bool isLeq = (b2 == jit::SistaV1::ArithBase + 4);
                            bool isJumpFalse =
                                (b3 == jit::SistaV1::ExtJumpFalse)
                                || (b3 >= jit::SistaV1::ShortJumpFalseBase
                                    && b3 <= jit::SistaV1::ShortJumpFalseLast);
                            uint32_t loopTemp = (uint32_t)(b0 - jit::SistaV1::PushTempBase);
                            // Loop-counter increment before jumpTo.
                            bool incrOk = false;
                            if (i >= 4
                                && isPushTemp && isLitConst
                                && isLeq && isJumpFalse) {
                                uint8_t i0 = bc_[i - 4];
                                uint8_t i1 = bc_[i - 3];
                                uint8_t i2 = bc_[i - 2];
                                uint8_t i3 = bc_[i - 1];
                                if (i0 == (uint8_t)(jit::SistaV1::PushTempBase + loopTemp)
                                    && i1 == 0x51
                                    && i2 == jit::SistaV1::ArithBase
                                    && i3 == (uint8_t)(0xD0 + loopTemp)) {
                                    incrOk = true;
                                }
                            }
                            if (incrOk) {
                                size_t bodyStart =
                                    (b3 == jit::SistaV1::ExtJumpFalse)
                                        ? t + 5 : t + 4;
                                size_t bodyEnd = i - 4;
                                size_t bodyLen = bodyEnd > bodyStart
                                    ? bodyEnd - bodyStart : 0;
                                // PRE_LOOP: 3 bytes before t.
                                bool preLoopOk = false;
                                size_t preLoopStart = 0;
                                int countInit = 0;
                                if (t >= 3) {
                                    preLoopStart = t - 3;
                                    uint8_t p0 = bc_[t - 3];
                                    uint8_t p1 = bc_[t - 2];
                                    uint8_t p2 = bc_[t - 1];
                                    // timesRepeat:-style:
                                    //   pushLitConst LIMIT, pushOne,
                                    //   popIntoTemp loopT.  Leftover
                                    //   on stack: LIMIT.
                                    if (p0 == b1
                                        && (p1 == 0x50 || p1 == 0x51)
                                        && p2 == (uint8_t)(0xD0 + loopTemp)) {
                                        preLoopOk = true;
                                        countInit = (p1 == 0x51) ? 1 : 0;
                                    }
                                    // to:do:-style attempt 2026-05-01
                                    // (pushOne + ExtStoreIntoTemp) was
                                    // tested and rolled back: the
                                    // recognizer false-positived on
                                    // forEachIv (Interval do:) and
                                    // produced 4ms → 1087ms regression.
                                    // The leftover-on-stack semantics
                                    // differ between to:do: (countInit
                                    // left) and timesRepeat: (LIMIT
                                    // left), so the IR op's deopt-stack
                                    // assumption (push back the limit)
                                    // doesn't match for to:do:.
                                }
                                // END pop at i+4.
                                bool endPopOk = (i + 4 < len_
                                    && bc_[i + 4] == jit::SistaV1::Pop);
                                // Body shape:
                                //   [K leading purely-elidable triplets]
                                //   followed by canonical 4-byte arith
                                //   on a single accum temp.
                                //
                                // Each leading triplet is
                                //   pushTemp T (0x40-0x4B)
                                //   sendByte   (yourself)
                                //   pop        (0xD8)
                                // where T is the SAME temp across all
                                // triplets (= bodyTemp, loop-invariant
                                // receiver loaded once before the loop)
                                // and the send is universally side-
                                // effect-free + non-erroring on any
                                // class.  yourself is the only such
                                // selector today (Object>>yourself
                                // returns self; never overridden in
                                // standard Pharo classes; cannot raise
                                // for any receiver including nil).
                                //
                                // Math splice still applies because the
                                // triplets have no observable effect —
                                // running them N times is observably
                                // identical to running them 0 times.
                                bool bodyOk = false;
                                bool bodyShapeIsSeries = false;
                                uint32_t accumTemp = 0;
                                int arithCode = -1;
                                int constValue = 0;
                                size_t bodySkippedTriplets = 0;
                                size_t bodyArithStart = bodyStart;
                                int   bodyTempIdx = -1;
                                uint64_t bodyGuardClassOop = 0;
                                uint32_t bodyGuardBcOffset = 0;
                                {
                                    // Scan leading triplets (max 8 so
                                    // we don't walk into pathological
                                    // long bodies — runInstVar-shape
                                    // has 2 triplets).
                                    size_t scanIp = bodyStart;
                                    int firstBodyTemp = -1;
                                    while (scanIp + 3 <= bodyEnd
                                           && bodySkippedTriplets < 8) {
                                        uint8_t t0 = bc_[scanIp];
                                        uint8_t t1 = bc_[scanIp + 1];
                                        uint8_t t2 = bc_[scanIp + 2];
                                        bool tIsTemp =
                                            t0 >= jit::SistaV1::PushTempBase
                                            && t0 <= 0x4B;
                                        bool tIsPop = (t2 == 0xD8);
                                        // Send0 N (0x80-0x8F): selector
                                        // index = N — admit when the
                                        // selector is in the inlineable
                                        // bitmap (yourself).  Universally
                                        // safe for any class.
                                        // SpecialSend size (0x72) — admit
                                        // when an IC hint is available
                                        // for this bcOffset; emit a class
                                        // guard before the splice so a
                                        // class mismatch deopts cleanly.
                                        bool sendIsElidable = false;
                                        bool sendNeedsClassGuard = false;
                                        if (t1 >= jit::SistaV1::Send0Base
                                            && t1 <= jit::SistaV1::Send0Last) {
                                            uint32_t selIdx =
                                                (uint32_t)(t1 - jit::SistaV1::Send0Base);
                                            if (selIdx < 16
                                                && (inlineableSelectorMask_
                                                    & (1u << selIdx))) {
                                                sendIsElidable = true;
                                            }
                                        } else if (t1 == 0x72) {  // SpecialSend size
                                            uint32_t hintBc =
                                                (uint32_t)(scanIp + 1);
                                            // First try IC hints (the
                                            // safe path for warm
                                            // methods).
                                            if (inlineHints_) {
                                                for (const auto& h : *inlineHints_) {
                                                    if (h.bcOffset == hintBc
                                                        && h.classOop != 0) {
                                                        sendIsElidable = true;
                                                        sendNeedsClassGuard = true;
                                                        if (bodyGuardClassOop == 0) {
                                                            bodyGuardClassOop = h.classOop;
                                                            bodyGuardBcOffset = hintBc;
                                                        } else if (bodyGuardClassOop != h.classOop) {
                                                            sendIsElidable = false;
                                                        }
                                                        break;
                                                    }
                                                }
                                            }
                                            // Fallback: dataflow trace
                                            // through method prologue
                                            // for `pushLitVar X;
                                            // SpecialSend new (0x7C);
                                            // popIntoTemp T` where T
                                            // is the bodyTemp.  If
                                            // found, extract the
                                            // class from literals[X]
                                            // (a class binding —
                                            // slot 1 is the class
                                            // itself).  Lets bench
                                            // methods (one-shot, IC
                                            // not yet warmed) splice.
                                            if (!sendIsElidable
                                                && memory_ != nullptr
                                                && tIsTemp) {
                                                int targetTemp =
                                                    (int)(t0 - jit::SistaV1::PushTempBase);
                                                for (size_t pi = 0;
                                                     pi + 2 < preLoopStart
                                                     && pi + 2 < len_;
                                                     pi++) {
                                                    uint8_t p0 = bc_[pi];
                                                    uint8_t p1 = bc_[pi + 1];
                                                    uint8_t p2 = bc_[pi + 2];
                                                    bool p0IsLitVar =
                                                        (p0 >= 0x10
                                                         && p0 <= 0x1F);
                                                    bool p1IsNew =
                                                        (p1 == 0x7C);
                                                    bool p2IsPopT =
                                                        (p2 == (uint8_t)
                                                         (0xD0 + targetTemp));
                                                    if (p0IsLitVar
                                                        && p1IsNew
                                                        && p2IsPopT) {
                                                        uint32_t litIdx =
                                                            (uint32_t)(p0 - 0x10);
                                                        if (litIdx
                                                            < out_.literals.size()) {
                                                            Oop binding =
                                                                out_.literals[litIdx];
                                                            if (binding.isObject()
                                                                && binding.rawBits()
                                                                   > 0x10000) {
                                                                ObjectHeader* bhdr =
                                                                    binding.asObjectPtr();
                                                                if (bhdr->slotCount() >= 2) {
                                                                    Oop cls = bhdr->slotAt(1);
                                                                    if (cls.isObject()
                                                                        && cls.rawBits() > 0x10000) {
                                                                        // Match IC-hint
                                                                        // convention: store
                                                                        // classIndex (the
                                                                        // 22-bit Spur key),
                                                                        // not the class
                                                                        // Oop's raw bits.
                                                                        // kGuardClass
                                                                        // compares against
                                                                        // the receiver's
                                                                        // header.classIndex.
                                                                        uint32_t clsIdx =
                                                                            memory_->indexOfClass(cls);
                                                                        if (clsIdx != 0) {
                                                                            sendIsElidable = true;
                                                                            sendNeedsClassGuard = true;
                                                                            if (bodyGuardClassOop == 0) {
                                                                                bodyGuardClassOop = (uint64_t)clsIdx;
                                                                                bodyGuardBcOffset = hintBc;
                                                                            }
                                                                        }
                                                                    }
                                                                }
                                                            }
                                                        }
                                                        break;
                                                    }
                                                }
                                            }
                                        }
                                        if (!(tIsTemp && tIsPop && sendIsElidable)) {
                                            break;
                                        }
                                        int tempIdx =
                                            (int)(t0 - jit::SistaV1::PushTempBase);
                                        if (firstBodyTemp < 0) {
                                            firstBodyTemp = tempIdx;
                                        } else if (tempIdx != firstBodyTemp) {
                                            // All triplets must share
                                            // the same bodyTemp.  Reject.
                                            break;
                                        }
                                        if (tempIdx == (int)loopTemp) {
                                            // bodyTemp must not collide
                                            // with the loop counter.
                                            break;
                                        }
                                        bodySkippedTriplets++;
                                        (void)sendNeedsClassGuard;
                                        scanIp += 3;
                                    }
                                    if (bodySkippedTriplets > 0
                                        && firstBodyTemp >= 0) {
                                        bodyTempIdx = firstBodyTemp;
                                    }
                                    bodyArithStart = scanIp;
                                }
                                size_t bodyArithLen = bodyEnd > bodyArithStart
                                    ? bodyEnd - bodyArithStart : 0;
                                // Case: body is K triplets ONLY (no
                                // user accum).  This is the
                                // `n timesRepeat: [obj size. obj yourself]`
                                // shape — Pharo's inliner generates an
                                // internal loop counter (loopTemp) but
                                // no separate user accumulator.  Treat
                                // loopTemp as the "accum" with const=1,
                                // arith=+: post-loop value is limit+1
                                // which matches the natural exit value
                                // of the timesRepeat counter.
                                bool bodyShapeIsNoAccum = false;
                                if (bodyArithLen == 0
                                    && bodySkippedTriplets > 0
                                    && bodyTempIdx >= 0) {
                                    bodyOk = true;
                                    bodyShapeIsNoAccum = true;
                                    accumTemp = loopTemp;
                                    arithCode = 0;     // Add
                                    constValue = 1;
                                } else if (bodyArithLen == 4 && bodyArithStart < len_) {
                                    uint8_t y0 = bc_[bodyArithStart];
                                    uint8_t y1 = bc_[bodyArithStart + 1];
                                    uint8_t y2 = bc_[bodyArithStart + 2];
                                    uint8_t y3 = bc_[bodyArithStart + 3];
                                    bool y0IsTemp = y0 >= jit::SistaV1::PushTempBase && y0 <= 0x4B;
                                    bool y2IsAddSub = (y2 == jit::SistaV1::ArithBase
                                            || y2 == jit::SistaV1::ArithBase + 1);
                                    bool y3IsPopMatch = y3 == (uint8_t)(0xD0 + (y0 - jit::SistaV1::PushTempBase));
                                    bool y0NotLoop = (y0 - jit::SistaV1::PushTempBase) != (int)loopTemp;
                                    // Shape A: y1 = pushZero/One
                                    if (y0IsTemp && (y1 == 0x50 || y1 == 0x51)
                                        && y2IsAddSub && y3IsPopMatch && y0NotLoop) {
                                        bodyOk = true;
                                        accumTemp = (uint32_t)(y0 - jit::SistaV1::PushTempBase);
                                        arithCode = (y2 == jit::SistaV1::ArithBase) ? 0 : 1;
                                        constValue = (y1 == 0x51) ? 1 : 0;
                                    }
                                    // Shape B (arithmetic-series, e.g.
                                    // simpleLoop's `s := s + n`):
                                    //   pushTemp accum, pushTemp loopT,
                                    //   ArithBase Y, popTemp accum
                                    // Math: accum_final = accum_init +
                                    //   limit*(limit+1)/2 - (cI-1)*cI/2
                                    else if (y0IsTemp
                                        && y1 == (uint8_t)(jit::SistaV1::PushTempBase + loopTemp)
                                        && y2IsAddSub && y3IsPopMatch && y0NotLoop) {
                                        bodyOk = true;
                                        bodyShapeIsSeries = true;
                                        accumTemp = (uint32_t)(y0 - jit::SistaV1::PushTempBase);
                                        arithCode = (y2 == jit::SistaV1::ArithBase) ? 0 : 1;
                                        constValue = 0;  // unused for series
                                    }
                                }
                                if (probeWhileTrue) {
                                    static int probeCount = 0;
                                    if (probeCount++ < 32) {
                                        std::fprintf(stderr,
                                            "[SISTA-WHILETRUE-PROBE] "
                                            "preLoop=%zu loopHead=%zu "
                                            "jumpTo=%zu endPop=%zu "
                                            "loopTemp=%u limitLitIdx=%u "
                                            "body_len=%zu preLoopOk=%d "
                                            "endPopOk=%d bodyOk=%d "
                                            "method_len=%zu\n",
                                            preLoopStart, t, i, i + 4,
                                            loopTemp,
                                            (unsigned)(b1 - jit::SistaV1::PushLitConstBase),
                                            bodyLen, preLoopOk ? 1 : 0,
                                            endPopOk ? 1 : 0, bodyOk ? 1 : 0,
                                            len_);
                                        std::fprintf(stderr,
                                            "[SISTA-WHILETRUE-PROBE]   body:");
                                        for (size_t bi = bodyStart;
                                             bi < bodyEnd && bi < len_; bi++) {
                                            std::fprintf(stderr, " %02x",
                                                bc_[bi]);
                                        }
                                        std::fprintf(stderr, "\n");
                                    }
                                }
                                if (whileTrueSpliceEnabled
                                    && preLoopOk && endPopOk && bodyOk
                                    && constValue >= 0 && constValue <= 1
                                    && arithCode >= 0 && arithCode <= 1
                                    && countInit >= 0 && countInit <= 1
                                    && accumTemp <= 0xFF
                                    && loopTemp <= 0xFF
                                    && (b1 - jit::SistaV1::PushLitConstBase) <= 0xFF
                                    && preLoopStart <= 0xFFFF) {
                                    uint32_t limitLitIdx =
                                        (uint32_t)(b1 - jit::SistaV1::PushLitConstBase);
                                    uint64_t packed =
                                        ((uint64_t)accumTemp & 0xFF)
                                      | (((uint64_t)arithCode & 0xF) << 8)
                                      | (((uint64_t)(uint8_t)constValue & 0xFF) << 12)
                                      | (((uint64_t)limitLitIdx & 0xFF) << 20)
                                      | (((uint64_t)loopTemp & 0xFF) << 28)
                                      | (((uint64_t)(uint8_t)countInit & 0xFF) << 36)
                                      | (((uint64_t)preLoopStart & 0xFFFF) << 44)
                                      | ((uint64_t)(bodyShapeIsSeries ? 1 : 0) << 60)
                                      | ((uint64_t)(bodyShapeIsNoAccum ? 1 : 0) << 61);
                                    // i+4 is the END pop offset; lifter
                                    // resumes at i+4 (the pop) so sim
                                    // stack consumer (pop) sees our
                                    // pushed value.
                                    WhileTruePatternInfo info;
                                    info.endOffset = i + 4;
                                    info.metadata = packed;
                                    info.bodyTriplets =
                                        (uint8_t)bodySkippedTriplets;
                                    info.bodyTempIdx = (bodyTempIdx >= 0)
                                        ? (uint8_t)bodyTempIdx : 0;
                                    info.guardClassOop = bodyGuardClassOop;
                                    info.guardBcOffset = bodyGuardBcOffset;
                                    whileTrueAccumPattern_[preLoopStart] = info;
                                    static int wtCandCount = 0;
                                    if (wtCandCount++ < 16) {
                                        std::fprintf(stderr,
                                            "[SISTA-WHILETRUE-CAND] "
                                            "preLoop=%zu endPop=%zu "
                                            "accumT=%u loopT=%u "
                                            "limitLit=%u arith=%d const=%d "
                                            "method_len=%zu\n",
                                            preLoopStart, i + 4,
                                            accumTemp, loopTemp,
                                            limitLitIdx, arithCode,
                                            constValue, len_);
                                    }
                                }
                            }
                        }
                    }
                }
                i++;
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
        //
        // OPT-IN.  Default-on attempt 2026-04-30 didn't move bench
        // suite — the per-method bench delta (sort 215 vs 219) was
        // within run-to-run variance; subsequent 3-run avg showed no
        // benefit.  Combining with SIZE_PEEPHOLE hangs bench suite
        // at 100% CPU (cause unknown).  See
        // memory/feedback_at_peephole_hangs.md.
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
            // 2026-05-01: inlined whileTrue: counter-loop splice.
            // When the pre-pass identified this offset as the start
            // of a `n timesRepeat: [accum := accum (+/-) const]` loop,
            // emit kCountedLoopWhileTrueAccum and skip past the entire
            // loop bytecode range.  The op produces a placeholder
            // value (typed Oop) that's pushed to the simulator stack;
            // the END pop bytecode (which we resume at) consumes it
            // so the post-loop sim stack matches the pre-pass
            // expectation.
            {
                auto wtIt = whileTrueAccumPattern_.find(ip);
                if (wtIt != whileTrueAccumPattern_.end()
                    && pendingExtA_ == 0 && pendingExtB_ == 0) {
                    const WhileTruePatternInfo& info = wtIt->second;
                    uint64_t packed = info.metadata;
                    size_t endOffset = info.endOffset;
                    // If the body has elidable triplets that need a
                    // class guard (e.g., `obj size; pop` for OC), emit
                    // kLoadTemp(bodyTemp) + kGuardClass BEFORE the
                    // splice IR op.  On guard miss, deopt resumes at
                    // the size send's bcOffset; interp executes the
                    // triplets normally (size returns the right value
                    // for the actual class, yourself returns self).
                    if (info.bodyTriplets > 0
                        && info.guardClassOop != 0) {
                        uint32_t loadV = out_.newValue(currentBlock_,
                            Op::kLoadTemp, Type::kOop,
                            /*operands=*/{},
                            /*literal=*/(uint64_t)info.bodyTempIdx);
                        // kGuardClass operands: [valueToCheck,
                        // ...IR-stack snapshot].  At this lift point
                        // stack_ may already have values; include them
                        // for deopt-stack rebuild.  literal: low 24
                        // bits = classOop; high 32 bits = bcOffset.
                        std::vector<uint32_t> guardOps;
                        guardOps.reserve(stack_.size() + 1);
                        guardOps.push_back(loadV);
                        for (uint32_t s : stack_) guardOps.push_back(s);
                        uint64_t guardLit =
                            (info.guardClassOop & 0x3FFFFFu)
                          | ((uint64_t)info.guardBcOffset << 32);
                        out_.newValue(currentBlock_, Op::kGuardClass,
                                      Type::kOop, std::move(guardOps),
                                      guardLit);
                    }
                    uint32_t vid = out_.newValue(currentBlock_,
                                                  Op::kCountedLoopWhileTrueAccum,
                                                  Type::kOop,
                                                  std::vector<uint32_t>{},
                                                  /*literal=*/packed);
                    recordFramepoint(vid, static_cast<uint32_t>(ip));
                    stack_.push_back(vid);
                    static int wtEmitCount = 0;
                    if (wtEmitCount++ < 16) {
                        std::fprintf(stderr,
                            "[SISTA-WHILETRUE-EMIT] preLoop=%zu "
                            "endPop=%zu vid=%u packed=0x%llx "
                            "triplets=%u guardCls=0x%llx\n",
                            ip, endOffset, vid,
                            (unsigned long long)packed,
                            (unsigned)info.bodyTriplets,
                            (unsigned long long)info.guardClassOop);
                    }
                    ip = endOffset;
                    continue;
                }
            }
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
                            // Framepoint resumes at the to: offset (=
                            // PushFullBlock - 2 bytes for PushTemp T_vec).
                            // Deopt pushes [start, stop] only and lets
                            // the interpreter re-run the to: send.
                            recordFramepoint(vid,
                                (uint32_t)(bcOffset - 2));
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
                // B2 splice intercept (collect:): PushFullBlock +
                // Send1 #collect: → kCountedLoopArrayCollect.  The
                // receiver is whatever was below the FullBlock on the
                // sim stack (an Array, runtime-checked in lowering).
                {
                    auto cIt = spliceCollectAtPushFullBlock_.find(ip);
                    bool collectHasCapture =
                        outerVecTempForCollect_.count(ip) > 0;
                    size_t collectNeeded = collectHasCapture ? 2 : 1;
                    if (cIt != spliceCollectAtPushFullBlock_.end()
                        && stack_.size() >= collectNeeded
                        && ip + 3 < len_) {
                        uint8_t nextOp = bc_[ip + 3];
                        if (nextOp >= jit::SistaV1::Send1Base
                            && nextOp <= jit::SistaV1::Send1Last) {
                            uint32_t selIdx = nextOp & 0x0F;
                            if (((collectSelectorMask_ >> selIdx) & 1) != 0) {
                                uint32_t blockSlot = cIt->second;
                                std::vector<uint32_t> ops;
                                if (collectHasCapture) {
                                    uint32_t vec = stack_.back();
                                    stack_.pop_back();
                                    uint32_t rcv = stack_.back();
                                    stack_.pop_back();
                                    ops = {rcv, vec};
                                } else {
                                    uint32_t rcv = stack_.back();
                                    stack_.pop_back();
                                    ops = {rcv};
                                }
                                uint32_t vid = out_.newValue(currentBlock_,
                                      Op::kCountedLoopArrayCollect,
                                      Type::kOop,
                                      std::move(ops),
                                      /*literal=*/blockSlot);
                                recordFramepoint(vid, bcOffset);
                                stack_.push_back(vid);
                                pendingExtA_ = 0;
                                pendingExtB_ = 0;
                                static int collEmitCount = 0;
                                if (collEmitCount++ < 16) {
                                    std::fprintf(stderr,
                                        "[SISTA-COLLECT-EMIT] bc=%zu "
                                        "slot=%u vid=%u capture=%d\n",
                                        ip, blockSlot, vid,
                                        collectHasCapture ? 1 : 0);
                                }
                                ip += 4;
                                continue;
                            }
                        }
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
                // offsets + their block-IR slot.  When the block
                // captures (numCopied=1) the simulator stack also has
                // the TempVector ref above the receiver — pop it as
                // operand[1] and pack vecSlot into the literal.
                {
                    auto sIt = spliceAtPushFullBlock_.find(ip);
                    bool doHasCaptureNow =
                        doVecCaptureAtPushFullBlock_.count(ip) > 0;
                    size_t doNeeded = doHasCaptureNow ? 2 : 1;
                    if (sIt != spliceAtPushFullBlock_.end()
                        && stack_.size() >= doNeeded
                        // Make sure SpecialSend do: really follows.
                        && ip + 3 < len_
                        && bc_[ip + 3] == 0x7B) {
                        uint32_t blockSlot = sIt->second;
                        uint32_t vec = 0;
                        uint32_t vecSlotForLit = 0;
                        if (doHasCaptureNow) {
                            vec = stack_.back();
                            stack_.pop_back();
                            uint64_t packed =
                                doVecCaptureAtPushFullBlock_[ip];
                            vecSlotForLit =
                                (uint32_t)((packed >> 16) & 0xFFFF);
                        }
                        uint32_t rcv = stack_.back();
                        stack_.pop_back();
                        std::vector<uint32_t> ops;
                        ops.push_back(rcv);
                        if (doHasCaptureNow) ops.push_back(vec);
                        // Literal layout (extended for capture):
                        //   bits  0-31 = blockSlot
                        //   bits 32-47 = vecSlot (when capture)
                        //   bit  48    = 1 if has capture
                        uint64_t literal = (uint64_t)blockSlot;
                        if (doHasCaptureNow) {
                            literal |= ((uint64_t)vecSlotForLit << 32)
                                    |  ((uint64_t)1ULL << 48);
                        }
                        uint32_t vid = out_.newValue(currentBlock_,
                                       Op::kCountedLoopDo, Type::kOop,
                                       std::move(ops),
                                       literal);
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
                    bool injectHasCaptureNow =
                        outerVecTempForInject_.count(ip) > 0;
                    size_t injectNeeded = injectHasCaptureNow ? 3 : 2;
                    if (iIt != spliceInjectAtPushFullBlock_.end()
                        && stack_.size() >= injectNeeded
                        // Verify Send2 really follows (op in 0xA0-0xAF).
                        && ip + 3 < len_
                        && bc_[ip + 3] >= 0xA0
                        && bc_[ip + 3] <= 0xAF) {
                        uint32_t blockSlot = iIt->second;
                        uint32_t vec = 0;
                        if (injectHasCaptureNow) {
                            vec = stack_.back();
                            stack_.pop_back();
                        }
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
                            if (injectHasCaptureNow) ops.push_back(vec);
                            vid = out_.newValue(currentBlock_,
                                  Op::kCountedLoopIntervalInjectInto,
                                  Type::kOop,
                                  std::move(ops),
                                  /*literal=*/blockSlot);
                        } else {
                            std::vector<uint32_t> ops{rcv, init};
                            if (injectHasCaptureNow) ops.push_back(vec);
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
                                "slot=%u vid=%u kind=%s capture=%d\n",
                                ip, blockSlot, vid,
                                rcvIsInterval ? "interval" : "array",
                                injectHasCaptureNow ? 1 : 0);
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

                // Phase 6 helper-sends extension: when HELPER_SENDS=1
                // is on and this method has splice candidates that
                // need the lift to continue past prologue sends, emit
                // kSendCallHelperSpecial (helper resolves the
                // SpecialSelector at runtime) and keep lifting.
                // Mirrors the Send0/1/2 helper-emit path below.
                static const bool helperSends = []() {
                    return std::getenv("PHARO_NO_SISTA_HELPER_SENDS")
                           == nullptr;
                }();
                bool hasSpliceCandidate =
                    !spliceAtPushFullBlock_.empty()
                 || !spliceAccumAtPushFullBlock_.empty()
                 || !spliceCollectAtPushFullBlock_.empty()
                 || !spliceInjectAtPushFullBlock_.empty()
                 || !spliceDoAtPushFullBlock_.empty()
                 || !spliceIvDoAccumAtPushFullBlock_.empty()
                 || !intervalDoAtTo_.empty()
                 || !intervalDoAccumAtTo_.empty()
                 || !intervalInjectAtTo_.empty()
                 || !whileTrueAccumPattern_.empty();
                bool methodIsShort = (len_ < 100);
                bool classIsHelperSafe =
                    sistaClassIsHelperSafe(methodClassName_);
                if (helperSends && hasSpliceCandidate && methodIsShort
                    && classIsHelperSafe) {
                    static int specHelperEmitCount = 0;
                    if (specHelperEmitCount++ < 16) {
                        std::fprintf(stderr,
                            "[SISTA-SPEC-HELPER-EMIT] class=%s "
                            "ssIdx=%u nArgs=%u bcOff=%u\n",
                            methodClassName_.c_str(), ssIdx, nArgs,
                            bcOffset);
                    }
                    // Snapshot full deopt stack before popping
                    // operands.
                    std::vector<uint32_t> deoptStack = stack_;
                    std::vector<uint32_t> sendOps;
                    sendOps.reserve(nArgs + 1);
                    for (uint32_t i = 0; i < nArgs + 1; i++) {
                        sendOps.push_back(
                            stack_[stack_.size() - nArgs - 1 + i]);
                    }
                    for (uint32_t i = 0; i < nArgs + 1; i++) {
                        stack_.pop_back();
                    }
                    uint64_t lit =
                        static_cast<uint64_t>(ssIdx)
                      | (static_cast<uint64_t>(nArgs)    << 32)
                      | (static_cast<uint64_t>(bcOffset) << 48);
                    uint32_t vid = out_.newValue(currentBlock_,
                                                  Op::kSendCallHelperSpecial,
                                                  Type::kOop,
                                                  std::move(sendOps),
                                                  lit);
                    out_.framepoints.push_back({
                        vid,
                        static_cast<uint16_t>(bcOffset),
                        std::move(deoptStack),
                    });
                    stack_.push_back(vid);
                    ip++;  // SpecialSend is 1 byte.
                    continue;
                }

                // Default path: bail-and-exit, same as before.
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
                        if (ivEmitCount++ < 16) {
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
                //
                // 2026-05-01: GATED to methods that have at least one
                // splice candidate.  Without this gate, every method
                // compiles with kSendCallHelper, including system code
                // like SnapshotOperation>>performSnapshot,
                // FileDoesNotExistException>>signal, ProcessorScheduler
                // class>>startUp.  Their helper-driven step() interacts
                // with materializeFrameStack to produce sender-chain
                // cycles (see project_b1_helpersends_2026_05_01.md
                // depth-21 cycle dump from helper-sends.log).  Methods
                // without splice candidates get NO benefit from
                // helper-sends, so gating on candidacy preserves the
                // win for splice-eligible methods while removing the
                // fragility for everything else.
                // 2026-05-02: flipped to default-on (opt-out via
                // PHARO_NO_SISTA_HELPER_SENDS=1) after deopt-path fix
                // (commit `bd7adb87`) + Array-do/helper coexistence
                // gate (commit `2a7e2a4e`).  Validation: 5/5 stable
                // bench-suite runs with `1M blocks = 0 ms` (math-
                // simplification splice fires).  runSum-style methods
                // (Array-do splice + kSendCallHelper) are now skipped
                // by SistaRuntime so they run in interp without DNU.
                static const bool helperSends = []() {
                    return std::getenv("PHARO_NO_SISTA_HELPER_SENDS")
                           == nullptr;
                }();
                bool hasSpliceCandidate =
                    !spliceAtPushFullBlock_.empty()
                 || !spliceAccumAtPushFullBlock_.empty()
                 || !spliceCollectAtPushFullBlock_.empty()
                 || !spliceInjectAtPushFullBlock_.empty()
                 || !spliceDoAtPushFullBlock_.empty()
                 || !spliceIvDoAccumAtPushFullBlock_.empty()
                 || !intervalDoAtTo_.empty()
                 || !intervalDoAccumAtTo_.empty()
                 || !intervalInjectAtTo_.empty()
                 || !whileTrueAccumPattern_.empty();
                // Narrow further: only short methods.  runSum is ~30
                // bytecodes; UI methods like WorldState>>drawWorld: are
                // hundreds.  Long methods exercise splice + helper-sends
                // through more code paths, surfacing latent bugs (e.g.
                // DNU on #isTransparent traced back to splice
                // misbehavior in WorldState methods).  100-byte cap is
                // empirical: keeps the win for runSum/sumArr/etc.,
                // skips most UI/system code.
                bool methodIsShort = (len_ < 100);
                // Item #6 (class-based gate, 2026-05-01): also skip
                // UI/system classes whose short methods trigger DNU
                // cascades downstream.  Without this, runBlock + the
                // whileTrue: splice WORK (after the methodIsShort gate
                // and HELPER_SENDS), but a bunch of WorldState/SpWindow
                // /Morph methods ALSO compile with kSendCallHelper and
                // their blocks splice, producing wrong types that
                // surface as DNU on #isTransparent.  Skipping these
                // classes preserves the win for user benches.
                bool classIsHelperSafe =
                    sistaClassIsHelperSafe(methodClassName_);
                if (helperSends && hasSpliceCandidate && methodIsShort
                    && classIsHelperSafe) {
                    // Diagnostic: log first 32 distinct classes that
                    // pass the gate.  Helps identify classes that
                    // need to be added to the skip list.
                    static int helperEmitCount = 0;
                    if (helperEmitCount++ < 32) {
                        std::fprintf(stderr,
                            "[SISTA-HELPER-EMIT] class=#%s method_len=%zu\n",
                            methodClassName_.c_str(), len_);
                    }
                    // Pop only rcvr + args (the send consumes them).
                    // Other live IR-stack values stay in their
                    // registers — kSendCallHelper is a producing op,
                    // not a terminator.
                    //
                    // 2026-04-29 fix (B7): snapshot the FULL IR stack
                    // (including values BELOW rcvr+args) so the deopt
                    // path can flush everything to interp.sp on bail.
                    // Without this, alive values below the send slot
                    // stay in registers and the resumed interpreter
                    // sees a truncated stack.
                    std::vector<uint32_t> deoptStack = stack_;
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
                    // Framepoint carries the full deopt stack snapshot,
                    // not just the send's operands — lowering uses it
                    // to flush every live IR value to interp.sp on
                    // helper-returned-zero.
                    out_.framepoints.push_back({
                        vid,
                        static_cast<uint16_t>(bcOffset),
                        std::move(deoptStack),
                    });
                    g_totalSendsLifted++;
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
                //
                // Skip the check when the operand is already known to
                // be SmI (PushOne/PushZero/PushInteger/PushLitConst-of-SmI,
                // result of a previous prim arith).  Saves the IR node
                // and the runtime AND+CMP+BEQ cost.
                if (typeCheckArith_) {
                    bool aSmI = (out_.values[a].type == Type::kOopSmallInt);
                    bool bSmI = (out_.values[b].type == Type::kOopSmallInt);
                    if (!aSmI) {
                        std::vector<uint32_t> checkA{a};
                        checkA.insert(checkA.end(), deoptStack.begin(), deoptStack.end());
                        out_.newValue(currentBlock_, Op::kPrimTagCheckInt,
                                      Type::kOopSmallInt, std::move(checkA),
                                      /*literal=*/bcOffset);
                    }
                    if (!bSmI) {
                        std::vector<uint32_t> checkB{b};
                        checkB.insert(checkB.end(), deoptStack.begin(), deoptStack.end());
                        out_.newValue(currentBlock_, Op::kPrimTagCheckInt,
                                      Type::kOopSmallInt, std::move(checkB),
                                      /*literal=*/bcOffset);
                    }
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
                // Narrow type when the literal is a SmallInteger — lets
                // the inline-arith tag-check pass skip the emit.
                Type ty = Type::kOop;
                if (litIdx < out_.literals.size()
                    && out_.literals[litIdx].isSmallInteger()) {
                    ty = Type::kOopSmallInt;
                }
                uint32_t v = out_.newValue(currentBlock_,
                                            Op::kLoadLiteral, ty,
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
        // Default ON 2026-04-29 after the kGuardClass optimization
        // (90b0356e) brought the hot-path guard from 8 to 6
        // instructions, eliminating the prior runtime overhead that
        // had blocked default-on.  Bench panel + setter bench at
        // parity with no-inline; accessor-heavy code wins.
        // Opt-out: PHARO_SISTA_NO_INLINE_CONST=1.
        static const bool inlineConst =
            std::getenv("PHARO_SISTA_NO_INLINE_CONST") == nullptr;
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
                default: recordUnrecognizedShape(calleeIR); return false;
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
                recordUnrecognizedShape(calleeIR);
                return false;
            }
        } else if (calleeIR.values.size() == 3) {
            const Value& v0 = calleeIR.values[0];
            const Value& v1 = calleeIR.values[1];
            const Value& v2 = calleeIR.values[2];
            if (v0.op != Op::kLoadReceiver) {
                recordUnrecognizedShape(calleeIR);
                return false;
            }
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
                recordUnrecognizedShape(calleeIR);
                return false;
            }
        } else if (calleeIR.values.size() == 4) {
            // Setter inline emits kStoreInstVar with bit 63 of the
            // literal set, which SistaLowering routes to the
            // jit_rt_store_inst_var helper (immutability + bounds +
            // write-barrier-aware).  Bytecode-emitted kStoreInstVar
            // (popStoreRcv*) leaves bit 63 clear and stays SAFETY-BAILed.
            //
            // Default ON 2026-04-29 after the helper-invoke lowering
            // landed in caed8d8c and the kGuardClass hot path shrank
            // from 8 to 6 instructions in 90b0356e.  Per-store helper
            // cost is roughly the same as the send-bail it replaces;
            // win comes from skipping the IC dispatch + push frame.
            static const bool inlineSetters =
                std::getenv("PHARO_SISTA_NO_INLINE_SETTERS") == nullptr;
            if (!inlineSetters) return false;
            // 4-value return-value setter pattern.  Body `foo: x` with
            //   ^ foo := x
            // compiles to bytecodes (using no-pop ExtStoreRecv)
            //   pushTemp 0; ExtStoreRecv N; returnTop
            // which lift to
            //   v0 = kLoadTemp(N0)
            //   v1 = kLoadReceiver
            //   v2 = kStoreInstVar(v1, v0, ivarIdx)        [void]
            //   v3 = kReturn(v0)                            [void]
            //
            // Inline as: kGuardClass + kStoreInstVar(callerRecv,
            // callerArg[N0], ivarIdx); result is callerArg (the value).
            const Value& v0 = calleeIR.values[0];
            const Value& v1 = calleeIR.values[1];
            const Value& v2 = calleeIR.values[2];
            const Value& v3 = calleeIR.values[3];
            if (v0.op != Op::kLoadTemp) {
                // Size==4 but not the setter shape — record so the
                // histogram surfaces the non-setter 4-value patterns.
                recordUnrecognizedShape(calleeIR);
                return false;
            }
            if (v1.op != Op::kLoadReceiver) return false;
            if (v2.op != Op::kStoreInstVar) return false;
            if (v2.operands.size() != 2) return false;
            if (v2.operands[0] != v1.id || v2.operands[1] != v0.id) return false;
            if (v3.op != Op::kReturn) return false;
            if (v3.operands.size() != 1 || v3.operands[0] != v0.id) return false;
            uint32_t tempIdx4 = static_cast<uint32_t>(v0.literal);
            if (tempIdx4 >= nArgs) return false;
            if (stack_.size() < nArgs + 1) return false;
            uint32_t argId = stack_[stack_.size() - nArgs + tempIdx4];

            std::vector<uint32_t> setterGuardOps;
            setterGuardOps.reserve(stack_.size() + 1);
            setterGuardOps.push_back(recvId);
            for (uint32_t s : stack_) setterGuardOps.push_back(s);
            uint64_t setterGuardLit = (hit->classOop & 0x3FFFFFu)
                                    | (static_cast<uint64_t>(bcOffset) << 32);
            out_.newValue(currentBlock_, Op::kGuardClass, Type::kOop,
                          std::move(setterGuardOps), setterGuardLit);
            // Mark inline-emitted store with bit 63 so SistaLowering
            // takes the helper-invoke path (immutability + bounds +
            // barrier-aware store).  Bytecode-emitted kStoreInstVar
            // (popStoreRcv*) leaves bit 63 clear and stays SAFETY-BAILed.
            out_.newValue(currentBlock_, Op::kStoreInstVar, Type::kVoid,
                          /*operands=*/{recvId, argId},
                          /*literal=*/(v2.literal | (1ULL << 63)));
            for (uint32_t i = 0; i < nArgs + 1; i++) stack_.pop_back();
            stack_.push_back(argId);  // returns the assigned value
            g_inlinesEmitted++;
            g_totalHintsConsumed++;
            return true;
        } else if (calleeIR.values.size() == 5) {
            // See PHARO_SISTA_NO_INLINE_SETTERS gate above (4-value
            // branch); same default-on rationale applies here.
            static const bool inlineSetters5 =
                std::getenv("PHARO_SISTA_NO_INLINE_SETTERS") == nullptr;
            if (!inlineSetters5) return false;
            // Two 5-value shapes share the prefix
            //   v0 = kLoadTemp(N0)
            //   v1 = kLoadReceiver
            //   v2 = kStoreInstVar(v1, v0, ivarIdx)        [void]
            // and differ in the return:
            //
            //   Shape A (returnSelf — implicit ^ self):
            //     v3 = kLoadReceiver
            //     v4 = kReturn(v3)
            //
            //   Shape B (return-temp — `popStoreRcv N; pushTemp 0; ^`):
            //     v3 = kLoadTemp(N0)
            //     v4 = kReturn(v3)
            //
            // Inline as: kGuardClass + kStoreInstVar(callerRecv,
            // callerArg[N0], ivarIdx); result is callerRecv (Shape A) or
            // callerArg (Shape B).
            const Value& v0 = calleeIR.values[0];
            const Value& v1 = calleeIR.values[1];
            const Value& v2 = calleeIR.values[2];
            const Value& v3 = calleeIR.values[3];
            const Value& v4 = calleeIR.values[4];
            if (v0.op != Op::kLoadTemp) {
                // Size==5 but not a setter; record the shape so the
                // histogram surfaces non-setter 5-value methods (e.g.
                // OrderedCollection>>size: rcvr+ivar+rcvr+ivar+send).
                recordUnrecognizedShape(calleeIR);
                return false;
            }
            if (v1.op != Op::kLoadReceiver) return false;
            if (v2.op != Op::kStoreInstVar) return false;
            if (v2.operands.size() != 2) return false;
            if (v2.operands[0] != v1.id || v2.operands[1] != v0.id) return false;
            if (v4.op != Op::kReturn) return false;
            if (v4.operands.size() != 1 || v4.operands[0] != v3.id) return false;
            uint32_t tempIdx = static_cast<uint32_t>(v0.literal);
            if (tempIdx >= nArgs) return false;
            if (stack_.size() < nArgs + 1) return false;
            uint32_t argId = stack_[stack_.size() - nArgs + tempIdx];

            uint32_t resultId;
            if (v3.op == Op::kLoadReceiver) {
                resultId = recvId;            // Shape A: returnSelf
            } else if (v3.op == Op::kLoadTemp
                    && static_cast<uint32_t>(v3.literal) == tempIdx) {
                resultId = argId;             // Shape B: ^ <assigned value>
            } else {
                return false;
            }

            std::vector<uint32_t> setterGuardOps;
            setterGuardOps.reserve(stack_.size() + 1);
            setterGuardOps.push_back(recvId);
            for (uint32_t s : stack_) setterGuardOps.push_back(s);
            uint64_t setterGuardLit = (hit->classOop & 0x3FFFFFu)
                                    | (static_cast<uint64_t>(bcOffset) << 32);
            out_.newValue(currentBlock_, Op::kGuardClass, Type::kOop,
                          std::move(setterGuardOps), setterGuardLit);
            // Mark inline-emitted store with bit 63 so SistaLowering
            // takes the helper-invoke path (immutability + bounds +
            // barrier-aware store).  Bytecode-emitted kStoreInstVar
            // (popStoreRcv*) leaves bit 63 clear and stays SAFETY-BAILed.
            out_.newValue(currentBlock_, Op::kStoreInstVar, Type::kVoid,
                          /*operands=*/{recvId, argId},
                          /*literal=*/(v2.literal | (1ULL << 63)));
            for (uint32_t i = 0; i < nArgs + 1; i++) stack_.pop_back();
            stack_.push_back(resultId);
            g_inlinesEmitted++;
            g_totalHintsConsumed++;
            return true;
        } else if (calleeIR.values.size() == 10) {
            // 10-value `^ ivar OP1 ivar OP2 const` arith chain.
            // Canonical case: `OrderedCollection>>size` is
            // `^ lastIndex - firstIndex + 1` which lifts to:
            //   v0 = kLoadReceiver
            //   v1 = kLoadInstVar(v0, lit=ivarA)
            //   v2 = kLoadReceiver
            //   v3 = kLoadInstVar(v2, lit=ivarB)
            //   v4 = kPrimTagCheckInt(v1, ...)
            //   v5 = kPrimTagCheckInt(v3, ...)
            //   v6 = kPrim{Add,Sub,Mul}Int(v1, v3, ...deoptStack)
            //   v7 = kConstantOop(lit=K)
            //   v8 = kPrim{Add,Sub,Mul}Int(v6, v7, ...deoptStack)
            //   v9 = kReturn(v8)
            //
            // Inline as: kGuardClass + LoadInstVar(callerRecv, ivarA) +
            // LoadInstVar(callerRecv, ivarB) + PrimTagCheckInt×2 +
            // primOp1 + ConstantOop + primOp2.  Result is primOp2's id.
            //
            // Default-on (2026-05-02): PHARO_NO_SISTA_INLINE_ARITHIVAR=1
            // disables.  Best-of-10 A/B confirmed parity (bench-suite
            // doesn't hit the canonical site — it's in a block — but
            // image code with `^ ivarA OP ivarB OP const` getters
            // (e.g., OC>>size's lastIndex - firstIndex + 1 shape)
            // benefits).  The inlined body has 2 deopt points (tag
            // checks) that each fall back to a normal send, matching
            // the callee's own deopt behavior.
            static const bool inlineArithIvar =
                std::getenv("PHARO_NO_SISTA_INLINE_ARITHIVAR") == nullptr;
            if (!inlineArithIvar) {
                recordUnrecognizedShape(calleeIR);
                return false;
            }
            const Value& cv0 = calleeIR.values[0];
            const Value& cv1 = calleeIR.values[1];
            const Value& cv2 = calleeIR.values[2];
            const Value& cv3 = calleeIR.values[3];
            const Value& cv4 = calleeIR.values[4];
            const Value& cv5 = calleeIR.values[5];
            const Value& cv6 = calleeIR.values[6];
            const Value& cv7 = calleeIR.values[7];
            const Value& cv8 = calleeIR.values[8];
            const Value& cv9 = calleeIR.values[9];
            // Validate exact operand chain.
            if (cv0.op != Op::kLoadReceiver) {
                recordUnrecognizedShape(calleeIR); return false;
            }
            if (cv1.op != Op::kLoadInstVar
                || cv1.operands.size() != 1
                || cv1.operands[0] != cv0.id) {
                recordUnrecognizedShape(calleeIR); return false;
            }
            if (cv2.op != Op::kLoadReceiver) {
                recordUnrecognizedShape(calleeIR); return false;
            }
            if (cv3.op != Op::kLoadInstVar
                || cv3.operands.size() != 1
                || cv3.operands[0] != cv2.id) {
                recordUnrecognizedShape(calleeIR); return false;
            }
            if (cv4.op != Op::kPrimTagCheckInt
                || cv4.operands.empty() || cv4.operands[0] != cv1.id) {
                recordUnrecognizedShape(calleeIR); return false;
            }
            if (cv5.op != Op::kPrimTagCheckInt
                || cv5.operands.empty() || cv5.operands[0] != cv3.id) {
                recordUnrecognizedShape(calleeIR); return false;
            }
            // First arith op must be one of Add/Sub/Mul on the two
            // ivars (NOT the tag-check results — the lifter passes the
            // original loads through; the tag-checks are side-effecting
            // deopt points only).  Under INLINE_ARITH default-on the
            // operands are [a, b, ...deoptStack], so size >= 2.
            if ((cv6.op != Op::kPrimAddInt
              && cv6.op != Op::kPrimSubInt
              && cv6.op != Op::kPrimMulInt)
                || cv6.operands.size() < 2
                || cv6.operands[0] != cv1.id
                || cv6.operands[1] != cv3.id) {
                recordUnrecognizedShape(calleeIR); return false;
            }
            if (cv7.op != Op::kConstantOop) {
                recordUnrecognizedShape(calleeIR); return false;
            }
            // Constant must be a SmI for the second arith op.  (Bit 0
            // set on the rawBits is the SmI tag in this Oop encoding.)
            if ((cv7.literal & 0x7) != 1) {
                recordUnrecognizedShape(calleeIR); return false;
            }
            // Second arith op must combine the first arith result with
            // the constant.  Same deopt-stack append → size >= 2.
            if ((cv8.op != Op::kPrimAddInt
              && cv8.op != Op::kPrimSubInt
              && cv8.op != Op::kPrimMulInt)
                || cv8.operands.size() < 2
                || cv8.operands[0] != cv6.id
                || cv8.operands[1] != cv7.id) {
                recordUnrecognizedShape(calleeIR); return false;
            }
            if (cv9.op != Op::kReturn
                || cv9.operands.size() != 1
                || cv9.operands[0] != cv8.id) {
                recordUnrecognizedShape(calleeIR); return false;
            }
            // Shape OK.  Emit kGuardClass on the caller's receiver
            // first (matches existing 3-value branch).
            std::vector<uint32_t> aiGuardOps;
            aiGuardOps.reserve(stack_.size() + 1);
            aiGuardOps.push_back(recvId);
            for (uint32_t s : stack_) aiGuardOps.push_back(s);
            uint64_t aiGuardLit = (hit->classOop & 0x3FFFFFu)
                                | (static_cast<uint64_t>(bcOffset) << 32);
            out_.newValue(currentBlock_, Op::kGuardClass, Type::kOop,
                          std::move(aiGuardOps), aiGuardLit);
            // Materialize the two ivar loads in the caller.
            uint32_t aiIvarA = out_.newValue(currentBlock_,
                                              Op::kLoadInstVar, Type::kOop,
                                              {recvId},
                                              cv1.literal);
            uint32_t aiIvarB = out_.newValue(currentBlock_,
                                              Op::kLoadInstVar, Type::kOop,
                                              {recvId},
                                              cv3.literal);
            // Tag checks — copy deopt stack pattern from the existing
            // arith builder path (operand[0]=value, then deopt stack
            // snapshot from caller's stack_).
            std::vector<uint32_t> aiCheckA{aiIvarA};
            for (uint32_t s : stack_) aiCheckA.push_back(s);
            uint32_t aiTaggedA = out_.newValue(currentBlock_,
                                               Op::kPrimTagCheckInt,
                                               Type::kOopSmallInt,
                                               std::move(aiCheckA),
                                               /*literal=*/bcOffset);
            std::vector<uint32_t> aiCheckB{aiIvarB};
            for (uint32_t s : stack_) aiCheckB.push_back(s);
            uint32_t aiTaggedB = out_.newValue(currentBlock_,
                                               Op::kPrimTagCheckInt,
                                               Type::kOopSmallInt,
                                               std::move(aiCheckB),
                                               /*literal=*/bcOffset);
            // First arith op.
            uint32_t aiOp1 = out_.newValue(currentBlock_, cv6.op,
                                            Type::kOopSmallInt,
                                            {aiTaggedA, aiTaggedB},
                                            /*literal=*/0);
            // Constant.
            uint32_t aiConst = out_.newValue(currentBlock_,
                                              Op::kConstantOop,
                                              Type::kOopSmallInt,
                                              {}, cv7.literal);
            // Second arith op.
            uint32_t aiOp2 = out_.newValue(currentBlock_, cv8.op,
                                            Type::kOopSmallInt,
                                            {aiOp1, aiConst},
                                            /*literal=*/0);
            // Pop receiver+args, push the inlined result.
            for (uint32_t i = 0; i < nArgs + 1; i++) stack_.pop_back();
            stack_.push_back(aiOp2);
            g_inlinesEmitted++;
            g_totalHintsConsumed++;
            static int aivCount = 0;
            if (++aivCount <= 10
                && std::getenv("PHARO_SISTA_INLINE_DUMP")) {
                std::fprintf(stderr,
                    "[ARITHIVAR-EMIT] callee=0x%llx op1=%s op2=%s "
                    "ivarA=%llu ivarB=%llu const=0x%llx bcOff=%u\n",
                    (unsigned long long)hit->targetMethod,
                    cv6.op == Op::kPrimAddInt ? "+" :
                    cv6.op == Op::kPrimSubInt ? "-" : "*",
                    cv8.op == Op::kPrimAddInt ? "+" :
                    cv8.op == Op::kPrimSubInt ? "-" : "*",
                    (unsigned long long)cv1.literal,
                    (unsigned long long)cv3.literal,
                    (unsigned long long)cv7.literal,
                    bcOffset);
            }
            return true;
        } else {
            // values.size() not in {2,3,4,5,10} — record the shape so the
            // dump can show which sizes/op-prefixes dominate the
            // unrecognized callees.
            recordUnrecognizedShape(calleeIR);
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
                    // Bumped from 8 → 64 (2026-04-30) to find non-trivial
                    // shapes (e.g. OC>>size which is 9-10 values).  The 8
                    // limit caught only the simplest 2-3 value callees.
                    if (dumpCount++ < 64) {
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
    // B2 splice: bitmap of literals[0..15] that hold #collect:.  Used
    // for the array-collect pattern.
    uint16_t               collectSelectorMask_ = 0;
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
    // Array-collect splice: pfbOff → block-IR slot.  Pre-pass detects
    // PushFullBlock + Send1#collect: with splice-simple block; lift's
    // PushFullBlock arm emits kCountedLoopArrayCollect.
    std::unordered_map<size_t, uint32_t> spliceCollectAtPushFullBlock_;
    // Array-collect with numCopied=1 capture: pfbOff → outer's temp
    // index that holds the captured TempVector.  Used by the lifter
    // splice intercept to know the vec is on the simulator stack
    // and by the lowering to read captured slots via kLoadTempInVec.
    std::unordered_map<size_t, uint32_t> outerVecTempForCollect_;
    // Same machinery for inject:into: + IV-inject when block has
    // numCopied=1.  Same map serves both Array and Interval inject
    // paths since the splice intercept is the same Send2 site.
    std::unordered_map<size_t, uint32_t> outerVecTempForInject_;
    // 2026-05-01: do: splice with numCopied=1 capture.  pfbOff →
    // packed (low 16 = outerVecTemp, mid 16 = vecSlot, high 16 = 1
    // for "has capture" sentinel).  Used by the splice intercept
    // (which pops vec off simulator stack) and by the kCountedLoopDo
    // lowering to emit pre-load + post-store of the captured slot
    // around the loop.
    std::unordered_map<size_t, uint64_t> doVecCaptureAtPushFullBlock_;
    // 2026-05-01: inlined whileTrue: counter-loop splice candidates.
    // Key = pre-loop start offset (where pushLitConst LIMIT begins).
    // Value: endOffset = the END pop bytecode (lifter resumes there
    // so the pop consumes our pushed placeholder); metadata = packed
    // (accumTemp, arithCode, constValue, limitLitIdx, loopTemp,
    //  countInit, preLoopStart).  See pre-pass.
    struct WhileTruePatternInfo {
        size_t endOffset;
        uint64_t metadata;
        // Body-extension fields (Phase 6 first cut, 2026-05-02): when
        // the body has K leading purely-elidable triplets that consume
        // a loop-invariant temp, record the temp index + (optional)
        // class guard so the emit code can wrap the splice with
        // kLoadTemp + kGuardClass.  guardClassOop=0 means no guard
        // needed (yourself-only triplets).
        uint8_t  bodyTriplets = 0;
        uint8_t  bodyTempIdx  = 0;
        uint64_t guardClassOop = 0;       // raw bits of the IC-observed class (0 = none)
        uint32_t guardBcOffset = 0;       // resume bc on guard miss
    };
    std::unordered_map<size_t, WhileTruePatternInfo> whileTrueAccumPattern_;

    // Item #6: name of the defining class (e.g. "PharoBenchmarkRunner
    // class", "WorldState").  Used by the HELPER_SENDS gate to skip
    // UI/system classes whose short methods trigger DNU cascades.
    std::string methodClassName_;
};

// Item #6 helper: returns true if HELPER_SENDS-style activation is
// SAFE for methods of this class.  Skips UI/system classes whose
// short methods produce DNU cascades when their blocks splice
// (documented in project_helper_sends_gate.md).
static bool sistaClassIsHelperSafe(const std::string& className) {
    if (className.empty()) return false;  // unknown — conservative
    static const char* skipPrefixes[] = {
        "World",         // WorldState (drawWorld:submorphs:invalidAreasOn:)
        "Form",          // FormCanvas (fillRectangle:on:, fullDraw:)
        "Morph",         // Morph and subclasses (fullDrawOn:)
        "Sp",            // SpWindow, Spec*
        "Snapshot",      // SnapshotOperation
        "Session",       // SessionManager, ClassSessionHandler
        "Process",       // Process, ProcessorScheduler
        "Semaphore",
        "Delay",
        "Exception",     // exception handling chain
        "Error",
        "FileReference",
        "FileSystem",
        "DiskFile",
        "File",          // File, FileHandle, FileDoesNotExistException
        "Source",        // SourceFile, SourceFileArray
        "Pharo",         // PharoFilesOpener, except PharoBenchmarkRunner
        nullptr
    };
    // Allow PharoBenchmarkRunner (overrides the "Pharo" prefix skip).
    if (className.compare(0, 21, "PharoBenchmarkRunner ") == 0
        || className == "PharoBenchmarkRunner") {
        return true;
    }
    if (className.compare(0, 21, "PharoBenchmarkRunner_") == 0) {
        return true;
    }
    for (const char** p = skipPrefixes; *p; p++) {
        size_t plen = std::strlen(*p);
        if (className.size() >= plen
            && className.compare(0, plen, *p) == 0) {
            return false;
        }
    }
    return true;
}

}  // namespace

// Narrow kLoadTemp value types to kOopSmallInt when every kStoreTemp
// into that slot stores an SmI-typed value.  Iterates to a fixed
// point because narrowing a load can flow through prim arith into
// further stores and unblock other slots.  Used by the lowering's
// kPrimTagCheckInt skip — eliminates per-iter tag checks in tight
// SmI counted loops (plainTo, simpleLoop).
static void narrowTempTypes(Method& m) {
    bool changed = true;
    int passes = 0;
    while (changed && passes < 8) {
        changed = false;
        passes++;
        // First, group stores by slot.
        std::unordered_map<uint64_t, std::vector<uint32_t>> storesBySlot;
        for (const Value& sv : m.values) {
            if (sv.op == Op::kStoreTemp && !sv.operands.empty()) {
                storesBySlot[sv.literal].push_back(sv.id);
            }
        }
        // For each slot: SmI-only iff every stored value is SmI-typed.
        std::unordered_map<uint64_t, bool> slotIsSmI;
        for (const auto& kv : storesBySlot) {
            uint64_t slot = kv.first;
            const std::vector<uint32_t>& storeIds = kv.second;
            if (storeIds.empty()) { slotIsSmI[slot] = false; continue; }
            bool allSmI = true;
            for (uint32_t storeId : storeIds) {
                const Value& sv = m.values[storeId];
                if (sv.operands.empty()
                    || m.values[sv.operands[0]].type != Type::kOopSmallInt) {
                    allSmI = false;
                    break;
                }
            }
            slotIsSmI[slot] = allSmI;
        }
        // Narrow loads of SmI-only slots.
        for (Value& lv : m.values) {
            if (lv.op == Op::kLoadTemp
                && lv.type == Type::kOop) {
                auto it = slotIsSmI.find(lv.literal);
                if (it != slotIsSmI.end() && it->second) {
                    lv.type = Type::kOopSmallInt;
                    changed = true;
                }
            }
        }
    }
}

LiftResult Builder::buildFromBytes(const uint8_t* bc, size_t len,
                                     uint32_t numArgs, uint32_t numTemps,
                                     Method& out,
                                     uint32_t* failedAtBytecode) {
    LinearLifter l(bc, len, numArgs, numTemps, out);
    LiftResult res = l.run(failedAtBytecode);
    if (res == LiftResult::kOk) narrowTempTypes(out);
    return res;
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

    // Per-bytecode entry: shift the lifter's view of the method to
    // start at g_buildStartBcOffset (set by Builder::buildFromOffset).
    // The lifter then sees a "method" whose bytecode 0 is the loop
    // header, and produces IR rooted there.  Reset the flag here so
    // recursive sub-lifts (block bodies, inlined callees) start
    // from offset 0 as before.
    uint32_t startBcOffset = g_buildStartBcOffset;
    g_buildStartBcOffset = 0;
    if (startBcOffset > 0) {
        if (startBcOffset >= bytecodeSize) {
            return LiftResult::kMalformedMethod;
        }
        bytecodes += startBcOffset;
        bytecodeSize -= startBcOffset;
        out.entryBcOffset = startBcOffset;
    }

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
    // sufficient.  Inlines #yourself as no-op (receiver stays on stack).
    //
    // Default-on (2026-05-02).  Re-tested under default flags after
    // Phase 6 + DOACCUM_RESUME landed: factorial 22-23ms either way
    // (the original "10× factorial regression" no longer reproduces —
    // was likely flaky timing before Phase 6 stabilization).  1M
    // getter+yourself 17→0ms (17×).  PHARO_NO_SISTA_INLINE_YOURSELF=1
    // opts out.  Still technically unsafe for classes that override
    // #yourself; class-hierarchy invalidation lands in Phase 7.  In
    // practice no production class overrides #yourself, so risk is
    // minimal.
    uint16_t inlineableMask = 0;
    uint16_t identityEqMask = 0;
    uint16_t identityNeqMask = 0;
    static const bool inlineYourself =
        std::getenv("PHARO_NO_SISTA_INLINE_YOURSELF") == nullptr;
    // INLINE_IDENTITY_EQ default-on (2026-05-01): #== / #~~ are universal
    // identity ops — never overridden in well-behaved code.  Best-of-3
    // bench A/B (with default flags + this on) showed parity-to-small-win
    // (sieve -2%, dict -1%, sum -1%, blocks -8%).  Set
    // PHARO_NO_SISTA_INLINE_IDENTITY_EQ=1 to opt out.
    static const bool inlineIdentityEq =
        std::getenv("PHARO_NO_SISTA_INLINE_IDENTITY_EQ") == nullptr;
    static const bool injectIntoSplice =
        std::getenv("PHARO_NO_SISTA_DO_SPLICE") == nullptr;
    // 2026-05-01 diagnostic: dump bench method bytecodes to trace
    // what shape Pharo emits for inlined timesRepeat:.  Gated.
    if (std::getenv("PHARO_SISTA_DUMP_BENCH") != nullptr) {
        std::string sel = memory.selectorOf(compiledMethod);
        if (sel == "runBlock" || sel == "runInstVar"
            || sel == "runFibonacci" || sel == "runFactorial"
            || sel == "runSum") {
            std::fprintf(stderr,
                "[SISTA-BENCH-DUMP] method=#%s numLits=%u bcLen=%zu\n",
                sel.c_str(), numLiterals, bytecodeSize);
            std::fprintf(stderr, "[SISTA-BENCH-DUMP]   bc:");
            for (size_t i = 0; i < bytecodeSize; i++) {
                std::fprintf(stderr, " %02x", bytecodes[i]);
            }
            std::fprintf(stderr, "\n");
        }
    }
    uint16_t injectIntoMask = 0;
    uint16_t toMask = 0;
    uint16_t collectMask = 0;
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
            // #collect: is 8 bytes — for the collect: splice.
            if (injectIntoSplice && bs == 8
                && std::memcmp(bytes, "collect:", 8) == 0) {
                collectMask |= (1u << i);
            }
        }
    }

    // Default ON 2026-04-29 (re-flip after entry-path fixes —
    // see Interpreter.cpp gate site for rationale).  Opt-out:
    // PHARO_SISTA_NO_INLINE_ARITH=1.
    static const bool typeCheckArith =
        std::getenv("PHARO_SISTA_NO_INLINE_ARITH") == nullptr;

    LinearLifter l(bytecodes, bytecodeSize, numArgs, numTemps, out);
    l.setMemory(&memory);
    if (haveSS) l.setSpecialSendArgCounts(ssArgCounts);
    if (inlineableMask) l.setInlineableSelectorBitmap(inlineableMask);
    if (identityEqMask) l.setIdentityEqSelectorBitmap(identityEqMask);
    if (identityNeqMask) l.setIdentityNeqSelectorBitmap(identityNeqMask);
    if (injectIntoMask) l.setInjectIntoSelectorBitmap(injectIntoMask);
    if (toMask) l.setToSelectorBitmap(toMask);
    if (collectMask) l.setCollectSelectorBitmap(collectMask);
    // Item #6: derive the method's defining class name from the last
    // literal (Pharo CompiledMethod convention: last lit is class
    // binding Association, slot 1 = class).  Used by the HELPER_SENDS
    // class-based gate to skip UI/system classes.
    {
        std::string className;
        if (numLiterals > 0) {
            Oop classBinding = memory.fetchPointer(numLiterals, compiledMethod);
            if (classBinding.isObject()
                && classBinding.rawBits() > 0x10000) {
                ObjectHeader* bindHdr = classBinding.asObjectPtr();
                if (bindHdr->slotCount() >= 2) {
                    Oop classObj = bindHdr->slotAt(1);
                    if (classObj.isObject()
                        && classObj.rawBits() > 0x10000) {
                        className = memory.nameOfClass(classObj);
                    }
                }
            }
        }
        l.setMethodClassName(className);
    }
    if (typeCheckArith) l.setTypeCheckArith(true);
    if (g_subLiftAsBlockReturnLocal) {
        l.setBlockReturnAsLocalReturn(true);
    }
    if (g_currentBuildHints) {
        l.setInlineHints(g_currentBuildHints);
    }
    LiftResult res = l.run(failedAtBytecode);
    if (res == LiftResult::kOk) narrowTempTypes(out);
    return res;
}

// Find the outermost lift point that's >= 0 and <= triggerBcOffset
// such that the suffix [T..end) is self-contained (no backward
// jumps escape).  See header comment for full semantics.
//
// Algorithm: scan all backward jumps in the method.  For candidate
// T = each backward-jump target ordered ascending, check whether
// ANY backward jump with source >= T has target < T.  If yes, T is
// not self-contained — try next.  First T that passes is the
// answer; falls back to triggerBcOffset if none found.
uint32_t Builder::findOutermostLiftPoint(Oop compiledMethod,
                                          ObjectMemory& memory,
                                          uint32_t triggerBcOffset) {
    if (!compiledMethod.isObject()) return triggerBcOffset;
    ObjectHeader* mh = compiledMethod.asObjectPtr();
    if (!mh->isCompiledMethod()) return triggerBcOffset;
    Oop hdrOop = memory.fetchPointer(0, compiledMethod);
    if (!hdrOop.isSmallInteger()) return triggerBcOffset;
    int64_t headerBits = hdrOop.asSmallInteger();
    uint32_t numLiterals = static_cast<uint32_t>(headerBits & 0x7FFF);
    const uint8_t* bytecodes = mh->bytes() + (1 + numLiterals) * 8;
    size_t totalBytes = mh->byteSize();
    size_t slotBytes = (1 + numLiterals) * 8;
    size_t bytecodeSize = (totalBytes > slotBytes)
                          ? (totalBytes - slotBytes) : 0;

    // Collect (source, target) pairs for every backward jump.
    struct BackJump { uint32_t source; uint32_t target; };
    std::vector<BackJump> backJumps;
    {
        size_t scan = 0;
        int extB = 0;
        while (scan < bytecodeSize) {
            uint8_t op = bytecodes[scan];
            if (op == jit::SistaV1::ExtendB) {
                if (scan + 1 >= bytecodeSize) break;
                extB = static_cast<int8_t>(bytecodes[scan + 1]);
                scan += 2;
                continue;
            }
            if (op == jit::SistaV1::ExtJump
             || op == jit::SistaV1::ExtJumpTrue
             || op == jit::SistaV1::ExtJumpFalse) {
                if (scan + 1 >= bytecodeSize) break;
                int offset = bytecodes[scan + 1] + (extB << 8);
                if (offset < 0) {
                    int64_t target = (int64_t)(scan + 2) + offset;
                    if (target >= 0 && target < (int64_t)bytecodeSize) {
                        backJumps.push_back({(uint32_t)scan,
                                              (uint32_t)target});
                    }
                }
                extB = 0;
                scan += 2;
                continue;
            }
            extB = 0;
            scan++;
        }
    }
    if (backJumps.empty()) return triggerBcOffset;

    // Build candidate lift points: distinct targets, sorted ascending.
    std::vector<uint32_t> candidates;
    candidates.reserve(backJumps.size());
    for (const auto& bj : backJumps) candidates.push_back(bj.target);
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()),
                      candidates.end());

    // Find the smallest candidate T <= triggerBcOffset such that no
    // backward jump in [T..end) escapes.
    for (uint32_t T : candidates) {
        if (T > triggerBcOffset) break;  // candidates is sorted
        bool selfContained = true;
        for (const auto& bj : backJumps) {
            if (bj.source >= T && bj.target < T) {
                selfContained = false;
                break;
            }
        }
        if (selfContained) return T;
    }
    return triggerBcOffset;
}

// Per-bytecode entry lift (item #8 in jit-multiweek-work.md).
//
// Lifts the suffix [startBcOffset..end) of a method as if it were a
// standalone method.  The lifter sees Block 0 starting at lifted-
// region offset 0 (= method bcOffset startBcOffset), walks bytecodes
// looking for block boundaries, and produces an IR rooted at that
// block.
//
// Bcoffsets recorded in the IR are LOCAL to the lifted region.  The
// caller (lowering) is responsible for adding entryBcOffset back when
// computing absolute interpreter ip's for deopt — done by passing
// methodBytes + entryBcOffset as bytecodeBase to lower().
//
// Bails (returns kMalformedMethod) if any backward jump within the
// lifted region targets BEFORE the lifted region — those would
// underflow size_t in pass 1 and the resulting block-start set would
// be malformed.  Detection: the existing pass 1 silently drops such
// jumps (target <= len_ check), so the lifter would produce IR that
// references a non-existent block at the underflowed offset.  We add
// an explicit check here before lifting.
LiftResult Builder::buildFromOffset(Oop compiledMethod, ObjectMemory& memory,
                                     Method& out, uint32_t startBcOffset,
                                     uint32_t* failedAtBytecode) {
    if (startBcOffset == 0) {
        return build(compiledMethod, memory, out, failedAtBytecode);
    }

    // Set the per-bytecode entry hook that build() consumes.  build()
    // does the full lifter setup (special-send arg counts, inline
    // hint masks, class-name lookup, etc.) — this avoids duplicating
    // all that into a separate code path.  Reset on early-return.
    g_buildStartBcOffset = startBcOffset;
    LiftResult r = build(compiledMethod, memory, out, failedAtBytecode);
    g_buildStartBcOffset = 0;
    return r;
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
    for (auto& h : g_polyDegreeHisto) h = 0;
}
void Builder::recordPolyDegree(uint8_t degree) {
    if (degree >= sizeof(g_polyDegreeHisto) / sizeof(g_polyDegreeHisto[0])) {
        degree = static_cast<uint8_t>(
            sizeof(g_polyDegreeHisto) / sizeof(g_polyDegreeHisto[0]) - 1);
    }
    g_polyDegreeHisto[degree]++;
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
    std::fprintf(stderr,
        "[SISTA-POLY] poly-degree empty=%llu mono=%llu bi=%llu tri=%llu "
        "quad=%llu 5=%llu 6=%llu\n",
        (unsigned long long)g_polyDegreeHisto[0],
        (unsigned long long)g_polyDegreeHisto[1],
        (unsigned long long)g_polyDegreeHisto[2],
        (unsigned long long)g_polyDegreeHisto[3],
        (unsigned long long)g_polyDegreeHisto[4],
        (unsigned long long)g_polyDegreeHisto[5],
        (unsigned long long)g_polyDegreeHisto[6]);
    // Top unrecognized callee shapes — sized only by values.size() not
    // in {2,3,4,5}, so anything caught here is a "we lifted it but the
    // recognizer can't handle this size" miss.  Print top 8 by count.
    if (!g_unrecognizedCalleeShapes.empty()) {
        std::vector<std::pair<uint64_t, uint64_t>> entries(
            g_unrecognizedCalleeShapes.begin(),
            g_unrecognizedCalleeShapes.end());
        std::sort(entries.begin(), entries.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
        size_t topN = entries.size() < 8 ? entries.size() : 8;
        std::fprintf(stderr, "[SISTA-UNRECOG]");
        for (size_t i = 0; i < topN; i++) {
            uint64_t key = entries[i].first;
            uint8_t sz   = (key >> 32) & 0xFF;
            uint8_t op0  = (key >> 24) & 0xFF;
            uint8_t op1  = (key >> 16) & 0xFF;
            uint8_t op2  = (key >>  8) & 0xFF;
            uint8_t op3  = (key      ) & 0xFF;
            std::fprintf(stderr, " sz=%u/%02x.%02x.%02x.%02x:%llu",
                sz, op0, op1, op2, op3,
                (unsigned long long)entries[i].second);
        }
        std::fprintf(stderr, "\n");
    }
}

}  // namespace sista
}  // namespace pharo
