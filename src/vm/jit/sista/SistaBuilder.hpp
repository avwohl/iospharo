/*
 * SistaBuilder.hpp - Bytecode -> Sista IR lifter
 *
 * Phase 2 of docs/sista-inlining-plan.md.  Walks a CompiledMethod's
 * bytecodes and produces a SistaMethod in SSA-lite form.
 *
 * Currently supports only the trivial subset needed to validate the
 * IR shape end-to-end:
 *   - PushReceiver  (0x4C)
 *   - PushTemp N    (0x40-0x4B)
 *   - PushLiteral N (0x50-0x53 single-byte, limited literal index)
 *   - ReturnReceiver (0x58)
 *   - ReturnTop      (0x5C)
 *
 * Anything else bails.  Expansion is incremental — each bytecode
 * lands with its own test case.
 */
#ifndef PHARO_SISTA_BUILDER_HPP
#define PHARO_SISTA_BUILDER_HPP

#include "SistaIR.hpp"

#include <functional>
#include <memory>
#include <vector>

namespace pharo {

class ObjectMemory;

namespace sista {

// Result of a lifting attempt.
enum class LiftResult {
    kOk,
    kUnsupportedBytecode,
    kMalformedMethod,
};

class Builder {
public:
    // Build IR for the given CompiledMethod.  On success, `out` is
    // populated with a usable Method.  On failure, `out` is left in an
    // indeterminate state and `failedAtBytecode` is set.
    static LiftResult build(Oop compiledMethod,
                             ObjectMemory& memory,
                             Method& out,
                             uint32_t* failedAtBytecode = nullptr);

    // Direct-byte variant for unit tests that don't want to allocate
    // a real CompiledMethod.  `bytecodes` is a span of Sista V1 ops,
    // `numArgs` / `numTemps` describe the method frame.
    static LiftResult buildFromBytes(const uint8_t* bytecodes,
                                      size_t length,
                                      uint32_t numArgs,
                                      uint32_t numTemps,
                                      Method& out,
                                      uint32_t* failedAtBytecode = nullptr);

    // Per-bytecode entry lift (item #8 in jit-multiweek-work.md).
    // Lifts the method tail starting at `startBcOffset` (a backward-
    // jump target / loop header).  The lifter sees the suffix
    // [startBcOffset..end) as if it were a complete method:
    //   - Block 0 starts at bcOffset 0 (= method bcOffset startBcOffset).
    //   - Backward jumps within the suffix work normally.
    //   - Backward jumps that escape (target before startBcOffset)
    //     bail the lift.
    //
    // The Method's `entryBcOffset` field records startBcOffset so
    // lowering can pass `bytecodeBase = methodBytes + startBcOffset`
    // to its deopt sequences (deopt resumes at the correct method-
    // relative ip).
    //
    // Returns kOk on success; the caller uses this with
    // Lowering::lower() the same way build() output is used.
    static LiftResult buildFromOffset(Oop compiledMethod,
                                       ObjectMemory& memory,
                                       Method& out,
                                       uint32_t startBcOffset,
                                       uint32_t* failedAtBytecode = nullptr);

    // Phase 4 Step 1: profile-guided inline hints.  When non-null,
    // tells the lifter which send sites are monomorphic (per T1 IC
    // observation).  Today the lifter only counts matches via the
    // inline-hint stats; future steps will use them to splice
    // callee IR.  Caller-owned, must outlive the build call.
    static LiftResult buildWithHints(Oop compiledMethod,
                                      ObjectMemory& memory,
                                      Method& out,
                                      const std::vector<InlineHint>* hints,
                                      uint32_t* failedAtBytecode = nullptr);

    // Cumulative inline-hint statistics across all builds.
    // dumpInlineHintStats() writes them to stderr.  Counters
    // are zeroed by resetInlineHintStats().
    static uint64_t totalSendsLifted();
    static uint64_t totalMonomorphicHints();
    static uint64_t totalHintsConsumed();
    static void resetInlineHintStats();
    static void dumpInlineHintStats();

    // Phase 5 Step 1: per-IC-site polymorphism histogram.  Called
    // from extractInlineHintsForMethod once per IC site with the
    // observed entry count (1 = monomorphic, 2-6 = polymorphic).
    // Drives capacity planning for chain emission (Step 2).
    static void recordPolyDegree(uint8_t degree);

    // Phase 4 Step 5: callback the inliner uses to get inline hints
    // for an arbitrary CompiledMethod oop.  Callee must extract the
    // method's JITMethod IC table and turn its monomorphic entries
    // into InlineHints — same logic the Interpreter applies for the
    // outer caller, factored out so the Builder can recurse into
    // inner sends without depending on the JIT runtime directly.
    //
    // Set once at VM startup; nullptr disables recursive inlining.
    using HintProvider =
        std::function<std::vector<InlineHint>(Oop method)>;
    static void setHintProvider(HintProvider provider);
};

}  // namespace sista
}  // namespace pharo

#endif  // PHARO_SISTA_BUILDER_HPP
