/*
 * JITCompiler.hpp - Copy-and-patch JIT compiler
 *
 * Copyright (c) 2026 Aaron Wohl. Licensed under the MIT License.
 *
 * Compiles Sista V1 bytecodes into machine code by copying and patching
 * pre-compiled stencils. This is the core of the Tier 1 JIT.
 *
 * COMPILATION FLOW:
 *
 *     1. Interpreter calls compile(compiledMethod)
 *     2. Compiler walks the bytecodes, selecting a stencil for each
 *     3. First pass: compute total code size (sum of stencil sizes + literal pool)
 *     4. Allocate in the CodeZone
 *     5. Second pass: copy stencils and patch relocations
 *     6. Flush icache, mark method as compiled
 *     7. Register in MethodMap
 *
 * GOT-STYLE PATCHING:
 *
 * Stencils compiled with Clang use GOT-relative addressing for holes
 * (adrp + ldr from GOT entry). We allocate a "literal pool" at the end
 * of the compiled method and patch each adrp+ldr pair to load from our
 * literal pool entries instead of a real GOT.
 */

#ifndef PHARO_JIT_COMPILER_HPP
#define PHARO_JIT_COMPILER_HPP

#include "JITConfig.hpp"
#include "CodeZone.hpp"
#include "JITMethod.hpp"
#include "JITState.hpp"
#include "Stencil.hpp"
#include "generated_stencils.hpp"
#include "../Oop.hpp"
#include <cstdint>
#include <cstddef>
#include <unordered_map>
#include <vector>

#if PHARO_JIT_ENABLED

namespace pharo {

// Forward declarations
class ObjectMemory;
class Interpreter;

namespace jit {

// Represents a bytecode instruction decoded for JIT compilation
struct DecodedBC {
    uint8_t   opcode;       // Raw bytecode value
    uint16_t  stencilIdx;   // Index into stencilTable
    int       operand;      // First operand (-1 if none)
    int       operand2;     // Second operand (-1 if none)
    uint64_t  operand2Ptr;  // 64-bit pointer operand (if non-zero, used instead of operand2)
    int       branchTarget; // Target bytecode index for jumps (-1 if not a jump)
    int       bcOffset;     // Offset in the bytecode array
    int       bcLength;     // Number of bytecodes consumed
};

class JITCompiler {
public:
    JITCompiler(CodeZone& zone, MethodMap& methodMap,
                ObjectMemory& memory, Interpreter& interp);

    // Compile a CompiledMethod. Returns the JITMethod, or nullptr on failure.
    // The method is registered in the MethodMap on success.
    // If oldVersion is provided, IC data from that compilation guides
    // specialization (inline getters/setters at monomorphic send sites).
    JITMethod* compile(Oop compiledMethod, JITMethod* oldVersion = nullptr);

    // Recompile a method using its accumulated IC data.
    // Replaces the old version in the MethodMap. Returns new JITMethod or nullptr.
    JITMethod* recompile(Oop compiledMethod);

    size_t recompilations() const { return recompilations_; }

    // Runtime helper function addresses (set once at init)
    struct RuntimeHelpers {
        void* sendSlow;         // _HOLE_RT_SEND handler
        void* returnToInterp;   // _HOLE_RT_RETURN handler
        void* arithOverflow;    // _HOLE_RT_ARITH_OVERFLOW handler
        void* nilOopAddr;       // Address of a memory location holding nil Oop
        void* trueOopAddr;      // Address of a memory location holding true Oop
        void* falseOopAddr;     // Address of a memory location holding false Oop
        void* megaCacheAddr;    // Address of mega cache entries array
        void* pushFrame;        // jit_rt_push_frame (J2J direct call)
        void* popFrame;         // jit_rt_pop_frame (J2J direct call)
        void* j2jCall;          // jit_rt_j2j_call (merged push+call+pop)
        void* arrayPrim;        // jit_rt_array_prim (out-of-line at:/at:put:/size)
        void* newPrim;          // jit_rt_new_prim (out-of-line new/new:)
        void* icMiss;           // jit_rt_ic_miss (out-of-line megacache probe + exit-state)
        void* j2jTrace;         // jit_rt_j2j_trace (B5 diagnostic — logs J2J save/restore)
        void* primAtPtr;        // jit_rt_primat_ptr (basicAt: for fmt 3/4/5/9)
        void* primAtPutPtr;     // jit_rt_primatput_ptr (basicAt:put: for fmt 3/4/5)
        void* recompileQueue;   // jit_rt_recompile_queue (queues callee for safe-point recompile)
        void* fillIC;           // jit_rt_fill_ic (cold-IC fill on mega-cache hit)
    };

    void setHelpers(const RuntimeHelpers& h) { helpers_ = h; }

    // Statistics
    size_t methodsCompiled() const { return methodsCompiled_; }
    size_t compilationsFailed() const { return compilationsFailed_; }

    // §1.3a shared-IC helper — returns the list of bytecode offsets
    // where T1 assigned a sendJ2J stencil, one entry per IC site.
    // `sites[sendIdx] = bcOffset`.  Tier2Compiler uses this to find
    // the matching T1 IC slot for a send at bytecode offset X:
    //   sendIdx = (find X in sites) ; icBase = T1.codeStart + ...
    // Returns nullptr if the method was never T1-compiled or was
    // evicted.  Populated during T1 compile; invalidated on GC
    // compaction via clearSendSiteMap().
    const std::vector<uint16_t>* getSendSiteBCOffsets(uint64_t compiledMethodBits) const;

    // Drop every entry — called from JITRuntime::recoverAfterGC after
    // Oop bits change.  The map is an optimization only; T2 falls
    // back to private IC buffers when lookup returns nullptr.
    void clearSendSiteMap() { sendSiteMap_.clear(); }

private:
    // Decode all bytecodes in a method.
    // On failure, failedOpcode is set to the bytecode that caused the bail-out.
    bool decodeBytecodes(const uint8_t* bytecodes, size_t length,
                         std::vector<DecodedBC>& decoded, uint8_t& failedOpcode,
                         bool isFullBlock = false);

    // Select the stencil for a given bytecode
    uint16_t selectStencil(uint8_t opcode, int operand) const;

    // SimStack register caching: replace base stencils with SimStack variants
    // where profitable. Inserts flush stencils before sends/returns/branch-targets.
    // Populates entryState[i] = SimStack state (0..4) at entry to decoded[i].
    // Callers use entryState to reject tryResume at bytecodes whose stencil
    // expects live x19..x22 — those registers are garbage after an interpreter
    // round-trip.
    void applySimStack(std::vector<DecodedBC>& decoded,
                       std::vector<int>& entryState);

    // IC-guided specialization: replace sendJ2J with inline stencils at
    // monomorphic send sites using IC data from a previous compilation.
    void applyICSpecialization(std::vector<DecodedBC>& decoded,
                                JITMethod* oldVersion,
                                ObjectHeader* methObj,
                                int numLiterals);

    // Patch all relocations for one stencil instance
    bool patchStencilInstance(uint8_t* codeBase, uint32_t stencilOffset,
                              const StencilDef& stencil,
                              const DecodedBC& bc,
                              uint8_t* methodCode, uint32_t totalCodeSize,
                              const std::vector<uint32_t>& bcToCodeOffset,
                              const std::vector<uint32_t>& bcToBranchOffset,
                              uint64_t* literalPool, uint32_t literalPoolOffset,
                              uint32_t& nextLiteralSlot);

    CodeZone&      zone_;
    MethodMap&     methodMap_;
    ObjectMemory&  memory_;
    Interpreter&   interp_;
    // §1.3a: compiledMethodOop → [bcOffset per sendIdx].
    // Populated during T1 compile.  Tier2 queries it to share IC
    // with T1 instead of allocating a private buffer.
    std::unordered_map<uint64_t, std::vector<uint16_t>> sendSiteMap_;
    RuntimeHelpers helpers_;

    size_t methodsCompiled_ = 0;
    size_t compilationsFailed_ = 0;
    size_t recompilations_ = 0;
    uint32_t bailoutCounts_[256] = {};  // Indexed by bytecode that caused bail-out
};

} // namespace jit
} // namespace pharo

#endif // PHARO_JIT_ENABLED
#endif // PHARO_JIT_COMPILER_HPP
