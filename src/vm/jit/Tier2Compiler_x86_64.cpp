/*
 * Tier2Compiler_x86_64.cpp - asmjit-based optimizing JIT, x86_64 sibling
 *
 * Copyright (c) 2026 Aaron Wohl. Licensed under the MIT License.
 *
 * Per-arch sibling of Tier2Compiler_arm64.cpp.  Selected by CMakeLists
 * on non-arm64 hosts (Linux x86_64, macOS x86_64).
 *
 * STATUS — op-by-op port in progress.  Currently emits inline x86_64
 * for:
 *   ReturnReceiver (0x58)        — single-byte method body
 *
 * Every other method shape returns nullptr; runtime falls through to
 * tier-1.  Adding ops: mirror the arm64 sibling's matcher / emitter
 * for that op, swapping a64::* for asmjit::x86::*.  Each new op should
 * land with a SUnit run as the correctness gate.
 *
 * Calling convention matches Tier 1 / Tier 2 arm64:
 *   void fn(JITState* state)   — state in rdi (SysV) / rcx (Win64);
 *                                 asmjit Compiler abstracts the ABI.
 *   exit via state->returnValue + state->exitReason = EXIT_RETURN
 */

#include "Tier2Compiler.hpp"
#include "JITRuntime.hpp"
#include "JITMethod.hpp"
#include "SistaV1.hpp"
#include "../ObjectMemory.hpp"

#include <cstdio>
#include <cstring>

#if PHARO_JIT_ENABLED

#include <asmjit/x86.h>
#include <asmjit/core/jitruntime.h>

namespace pharo {
namespace jit {

namespace {

// JITState field offsets — must match Tier 1 / Tier 2 arm64 so the
// runtime can invoke either tier transparently.  See JITState.hpp.
constexpr int OFF_RECEIVER = 8;
constexpr int OFF_EXIT     = 76;
constexpr int OFF_RETVAL   = 80;

// ExitReason values (JITState.hpp).
constexpr int EXIT_RETURN = 1;

// IC data layout (mirror JITMethod.hpp).  Used by flushAllICs().
constexpr int IC_ENTRIES = 6;
constexpr int IC_SLOTS   = IC_ENTRIES * 3 + 1;

// Bail / compile counters.  Per-arch (matches the SistaLowering
// per-arch g_lowerStats pattern).
size_t g_compiled = 0;
size_t g_bailed   = 0;

}  // namespace

Tier2Compiler::Tier2Compiler(CodeZone& zone, MethodMap& methodMap,
                             ObjectMemory& memory, Interpreter& interp)
    : methodMap_(methodMap), memory_(memory) {
    (void)zone;
    (void)interp;
}

Tier2Compiler::~Tier2Compiler() = default;

bool Tier2Compiler::initialize() {
    runtime_ = std::make_unique<asmjit::JitRuntime>();
    return true;
}

void Tier2Compiler::dumpBailStats() {
    fprintf(stderr, "  T2 (asmjit): compiled=%zu bailed=%zu\n",
            g_compiled, g_bailed);
}

void Tier2Compiler::flushAllICs() {
    for (auto& buf : icBuffers_) {
        std::memset(buf.get(), 0, IC_SLOTS * sizeof(uint64_t));
    }
}

void* Tier2Compiler::compile(Oop compiledMethod, JITMethod* oldVersion) {
    (void)oldVersion;
    static bool trace = std::getenv("PHARO_T2_X86_TRACE") != nullptr;
    if (trace) fprintf(stderr, "[T2-x86] compile entry: method=%p\n",
                       (void*)compiledMethod.rawBits());
    if (!runtime_) {
        if (!initialize()) {
            compilationsFailed_++;
            return nullptr;
        }
    }

    // Walk method header.  Format mirrors the arm64 sibling — keep in
    // sync if either changes.
    ObjectHeader* methodObj =
        reinterpret_cast<ObjectHeader*>(compiledMethod.rawBits());
    Oop headerOop = methodObj->slotAt(0);
    if (!headerOop.isSmallInteger()) {
        compilationsFailed_++;
        g_bailed++;
        return nullptr;
    }
    int64_t headerBits = headerOop.asSmallInteger();
    int  numLiterals  = (int)(headerBits & 0x7FFF);
    bool hasPrimitive = (headerBits >> 16) & 1;

    if (hasPrimitive) {
        compilationsFailed_++;
        g_bailed++;
        return nullptr;
    }

    uint8_t* bytes = methodObj->bytes();
    size_t bcStart = (1 + numLiterals) * 8;
    size_t totalBytes = methodObj->slotCount() * 8;
    uint8_t fmt = static_cast<uint8_t>(methodObj->format());
    int unusedBytes = (fmt >= 24) ? (fmt - 24) : 0;
    if (bcStart + (size_t)unusedBytes >= totalBytes) {
        compilationsFailed_++;
        g_bailed++;
        return nullptr;
    }
    size_t bodyLen = totalBytes - bcStart - (size_t)unusedBytes;

    // Pattern match: ^ self.  Two equivalent encodings — match either:
    //   ReturnReceiver         (0x58)             1 byte
    //   PushReceiver ReturnTop (0x4C 0x5C)        2 bytes
    // Anything after the return bytecode is unreachable, so we don't
    // care about trailing bytes (arm64 sibling does the same).
    uint8_t b0 = bytes[bcStart];
    uint8_t b1 = (bodyLen >= 2) ? bytes[bcStart + 1] : 0;
    bool matchReturnSelf =
        (b0 == SistaV1::ReturnReceiver)
        || (bodyLen >= 2 && b0 == SistaV1::PushReceiver
                         && b1 == SistaV1::ReturnTop);
    if (!matchReturnSelf) {
        if (trace) fprintf(stderr, "[T2-x86]   bail: bodyLen=%zu b0=0x%02x\n",
                           bodyLen, b0);
        compilationsFailed_++;
        g_bailed++;
        return nullptr;
    }
    if (trace) fprintf(stderr, "[T2-x86]   match ReturnReceiver (b0=0x%02x)\n", b0);

    // Emit:  retval = state.receiver;  exit = EXIT_RETURN;  ret;
    using namespace asmjit;
    using namespace asmjit::x86;

    CodeHolder code;
    code.init(runtime_->environment(), runtime_->cpu_features());

    Compiler cc(&code);
    FuncNode* fn = cc.add_func(FuncSignature::build<void, void*>());
    Gp state = cc.new_gp64("state");
    fn->set_arg(0, state);

    Gp recv = cc.new_gp64("recv");
    cc.mov(recv, ptr(state, OFF_RECEIVER));
    cc.mov(ptr(state, OFF_RETVAL), recv);

    Gp exitVal = cc.new_gp32("exitVal");
    cc.mov(exitVal, Imm(EXIT_RETURN));
    cc.mov(ptr(state, OFF_EXIT), exitVal);

    cc.ret();
    cc.end_func();

    Error err = cc.finalize();
    if (err != kErrorOk) {
        compilationsFailed_++;
        g_bailed++;
        return nullptr;
    }

    void* func = nullptr;
    err = runtime_->add(&func, &code);
    if (err != kErrorOk) {
        compilationsFailed_++;
        g_bailed++;
        return nullptr;
    }

    methodsCompiled_++;
    g_compiled++;
    return func;
}

void* Tier2Compiler::tryCompileMultiBC(Oop compiledMethod,
                                        const uint8_t* bytes,
                                        size_t bcStart,
                                        size_t bodyLen) {
    (void)compiledMethod;
    (void)bytes;
    (void)bcStart;
    (void)bodyLen;
    return nullptr;
}

}  // namespace jit
}  // namespace pharo

#endif  // PHARO_JIT_ENABLED
