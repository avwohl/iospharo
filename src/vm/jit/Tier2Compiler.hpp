/*
 * Tier2Compiler.hpp - MIR-based optimizing JIT compiler
 *
 * Copyright (c) 2026 Aaron Wohl. Licensed under the MIT License.
 *
 * Compiles hot Sista V1 methods into native ARM64 code using MIR.
 * MIR provides register allocation and machine code generation.
 *
 * COMPILATION FLOW:
 *
 *     1. JITRuntime detects hot method (executionCount > threshold)
 *     2. Tier2Compiler walks bytecodes, emits MIR IR:
 *        - Temps and expression stack values are MIR virtual registers
 *        - SmallInteger arithmetic uses overflow-checked MIR ops
 *        - Sends exit to interpreter (for now)
 *     3. MIR does register allocation + ARM64 code generation
 *     4. Result: a native function void(*)(JITState*)
 *
 * KEY DIFFERENCE FROM TIER 1:
 *
 *     Tier 1 (copy-and-patch)          Tier 2 (MIR)
 *     Values in JITState memory        Values in ARM64 registers
 *     ~4 ops per bytecode              ~1-2 ops per bytecode
 *     No cross-bytecode optimization   Full register allocation
 *     Pre-compiled stencils            Generated per-method
 */

#ifndef PHARO_TIER2_COMPILER_HPP
#define PHARO_TIER2_COMPILER_HPP

#include "JITConfig.hpp"
#include "JITState.hpp"
#include "JITMethod.hpp"
#include "../Oop.hpp"
#include <cstdint>
#include <cstddef>
#include <vector>

#if PHARO_JIT_ENABLED

// MIR headers (C API, wrapped in extern "C")
extern "C" {
#include "mir.h"
#include "mir-gen.h"
}

namespace pharo {

class ObjectMemory;
class Interpreter;

namespace jit {

class CodeZone;
class MethodMap;

class Tier2Compiler {
public:
    Tier2Compiler(CodeZone& zone, MethodMap& methodMap,
                  ObjectMemory& memory, Interpreter& interp);
    ~Tier2Compiler();

    // Initialize MIR context. Returns false on failure.
    bool initialize();

    // Compile a hot method to Tier 2. Returns function pointer or nullptr.
    // The method is NOT registered in the MethodMap — caller does that.
    // Uses IC profiling data from oldVersion for type specialization.
    void* compile(Oop compiledMethod, JITMethod* oldVersion);

    // Statistics
    size_t methodsCompiled() const { return methodsCompiled_; }
    size_t compilationsFailed() const { return compilationsFailed_; }

private:
    // Emit MIR IR for one bytecode. Returns false on unsupported bytecode.
    bool emitBytecode(uint8_t opcode, int operand, int operand2,
                      int bcOffset, int bcLength);

    // Emit prologue: load JITState fields into MIR registers
    void emitPrologue(int tempCount, int argCount);

    // Emit epilogue for EXIT_RETURN
    void emitReturn();

    // Emit send exit (flush stack to memory, set exitReason)
    void emitSendExit(int nArgs, int bcOffset, bool cached, MIR_reg_t cachedMethod);

    // SmallInteger arithmetic fast-path
    void emitSmallIntArith(int primKind, int bcOffset);

    // SmallInteger comparison fast-path
    void emitSmallIntCompare(int primKind);

    // Flush virtual expression stack to JITState sp
    void flushVStack();

    // Load a value from the Smalltalk stack at offset from sp
    MIR_reg_t loadStackAt(int offset);

    // --- Virtual expression stack ---
    // Instead of pushing to memory (JITState->sp), values live in MIR regs.
    static constexpr int MaxVStack = 32;
    MIR_reg_t vstack_[MaxVStack];
    int vstackDepth_ = 0;

    void vpush(MIR_reg_t r) { vstack_[vstackDepth_++] = r; }
    MIR_reg_t vpop() { return vstack_[--vstackDepth_]; }
    MIR_reg_t vpeek() { return vstack_[vstackDepth_ - 1]; }

    // --- MIR state ---
    MIR_context_t mirCtx_ = nullptr;  // Current compilation context (temporary)
    MIR_item_t    mirFunc_ = nullptr;
    MIR_module_t  mirModule_ = nullptr;

    // Keep MIR contexts alive so generated code stays valid.
    // Each context is ~50KB; for ~100 hot methods that's ~5MB.
    std::vector<MIR_context_t> liveContexts_;

    // Well-known registers (loaded in prologue)
    MIR_reg_t reg_statePtr_;  // JITState* (function argument)
    MIR_reg_t reg_sp_;        // Oop* sp
    MIR_reg_t reg_receiver_;  // Oop receiver
    MIR_reg_t reg_tempBase_;  // Oop* tempBase
    MIR_reg_t reg_literals_;  // Oop* literals
    MIR_reg_t reg_trueOop_;   // true Oop (constant for image lifetime)
    MIR_reg_t reg_falseOop_;  // false Oop
    MIR_reg_t reg_nilOop_;    // nil Oop
    MIR_reg_t reg_bcBase_;    // bytecode start address (for ip computation)

    // Temp variable registers (loaded lazily from tempBase)
    static constexpr int MaxTemps = 64;
    MIR_reg_t tempRegs_[MaxTemps];
    bool      tempLoaded_[MaxTemps];
    int       tempCount_ = 0;

    // Scratch register counter (for unique names)
    int scratchCounter_ = 0;
    MIR_reg_t newScratch();

    // Branch target labels (indexed by bytecode offset)
    static constexpr int MaxBCLabels = 1024;
    MIR_label_t bcLabels_[MaxBCLabels];
    bool        bcLabelUsed_[MaxBCLabels];

    // Resume points: after each send exit, the function can be re-entered
    // at a resume label. The dispatch table in the prologue jumps to the
    // right label based on (state.ip - bcStart).
    static constexpr int MaxResume = 64;
    struct ResumePoint {
        int postSendBC;          // Bytecode offset AFTER the send
        MIR_label_t label;       // MIR label to jump to
    };
    ResumePoint resumePoints_[MaxResume];
    int resumeCount_ = 0;
    int bcStartFromObj_ = 0;     // Compile-time: offset from object start to bytecodes
    MIR_label_t resumeDispatchLabel_;  // Label for the resume dispatch block

    // MIR items for inline send helper (created per-compilation)
    MIR_item_t sendProto_ = nullptr;
    MIR_item_t sendImport_ = nullptr;

    // --- Dependencies ---
    CodeZone&     zone_;
    MethodMap&    methodMap_;
    ObjectMemory& memory_;
    Interpreter&  interp_;

    // --- Statistics ---
    size_t methodsCompiled_ = 0;
    size_t compilationsFailed_ = 0;
};

} // namespace jit
} // namespace pharo

#endif // PHARO_JIT_ENABLED
#endif // PHARO_TIER2_COMPILER_HPP
