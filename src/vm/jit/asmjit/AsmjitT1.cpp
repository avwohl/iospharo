/*
 * AsmjitT1.cpp - Phase 1 of the asmjit-based Tier-1 JIT compiler.
 *
 * Per scripts/jit-diff/plan_asmjit_replacement.md.  See AsmjitT1.hpp
 * for the high-level role.  This file:
 *
 *   1. Asmjit-emits a tiny `void fn(JITState* state)` function whose
 *      whole body is `state->exitReason = ExitSend; return;`.
 *   2. Allocates a JITMethod in the existing code zone, sized to hold
 *      the emitted bytes (no IC entries, no bcToCode table — Phase 1
 *      doesn't dispatch any sends).
 *   3. Copies the emitted bytes into codeStart() so the existing
 *      `JIT_CALL(jm->codeStart(), state)` dispatch path invokes them
 *      unchanged.
 *   4. Registers the JITMethod in the MethodMap.
 *
 * No relocations needed because the emitted code has no external
 * references (no helper calls, no rip-relative loads — just an
 * immediate store and a return).
 *
 * Once Phase 2 starts emitting real bytecodes (pushes, returns), it
 * will need to coordinate with the runtime helpers (push_frame,
 * arith_overflow, etc.) and that's where relocations / runtime
 * helper addresses come back into play.  See the plan doc.
 */

#include "AsmjitT1.hpp"

#if PHARO_JIT_ENABLED

#include "../CodeZone.hpp"
#include "../JITState.hpp"
#include "../PlatformJIT.hpp"
#include "../../ObjectMemory.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
#include <asmjit/x86.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <asmjit/a64.h>
#else
#error "Unsupported architecture for AsmjitT1"
#endif

#include <asmjit/core/codeholder.h>

namespace pharo {
namespace jit {

namespace {

// JITState field offset for `exitReason` — guarded by static_assert
// in JITState.hpp.
constexpr int OFF_EXIT = 76;

// ExitReason value the trampoline writes.  The interpreter sees
// ExitSend and re-runs the method via the interp dispatch path.
constexpr int EXIT_SEND = 2;

size_t g_compiled = 0;
size_t g_failed   = 0;

// Emit the Phase 1 bail-to-interp trampoline into a CodeHolder and
// return the raw bytes via `out`/`outSize`.  Bytes are copied into
// `out` (caller-owned, must be at least `kBufSize` bytes).  Returns
// true on success.
constexpr size_t kBufSize = 64;  // generous; actual stub is ~6-12 bytes

bool emitTrampolineBytes(uint8_t* out, size_t* outSize) {
    using namespace asmjit;

    // Use host environment (matches the running architecture).
    Environment env = Environment::host();

    CodeHolder code;
    Error err = code.init(env);
    if (err != kErrorOk) {
        std::fprintf(stderr, "[asmjit-t1] code.init: %s\n",
                     DebugUtils::error_as_string(err));
        return false;
    }

#if defined(__x86_64__) || defined(_M_X64)
    x86::Assembler a(&code);
    // SysV AMD64: state* in rdi (callee saves nothing — we touch
    // only rax-class, no restore needed here).
    // mov DWORD PTR [rdi + 76], 2
    // ret
    a.mov(x86::dword_ptr(x86::rdi, OFF_EXIT), Imm(EXIT_SEND));
    a.ret();
#elif defined(__aarch64__) || defined(_M_ARM64)
    a64::Assembler a(&code);
    // AArch64: state* in x0; exitReason is int32_t.
    // mov w1, #2
    // str w1, [x0, #76]
    // ret
    a.mov(a64::w1, Imm(EXIT_SEND));
    a.str(a64::w1, a64::ptr(a64::x0, OFF_EXIT));
    a.ret(a64::x30);
#endif

    err = code.flatten();
    if (err != kErrorOk) {
        std::fprintf(stderr, "[asmjit-t1] code.flatten: %s\n",
                     DebugUtils::error_as_string(err));
        return false;
    }

    size_t total = code.code_size();
    if (total == 0 || total > kBufSize) {
        std::fprintf(stderr,
                     "[asmjit-t1] code.code_size=%zu out of [1, %zu]\n",
                     total, kBufSize);
        return false;
    }
    err = code.copy_flattened_data(out, kBufSize, CopySectionFlags::kPadSectionBuffer);
    if (err != kErrorOk) {
        std::fprintf(stderr, "[asmjit-t1] copy_flattened_data: %s\n",
                     DebugUtils::error_as_string(err));
        return false;
    }
    *outSize = total;
    return true;
}

// Cached emitted bytes — every compile produces the SAME bytes since
// the trampoline has no per-method specialization.  Emit once,
// memcpy on every compile.
struct CachedBytes {
    uint8_t data[kBufSize];
    size_t  size;
    bool    ready;
};
CachedBytes g_cached = { {}, 0, false };

bool ensureCachedBytes() {
    if (g_cached.ready) return true;
    if (!emitTrampolineBytes(g_cached.data, &g_cached.size)) return false;
    g_cached.ready = true;
    return true;
}

}  // namespace

JITMethod* compileViaAsmjit(CodeZone& zone, MethodMap& methodMap,
                             ObjectMemory& memory, Interpreter& interp,
                             Oop compiledMethod) {
    (void)memory;
    (void)interp;

    if (!ensureCachedBytes()) {
        g_failed++;
        return nullptr;
    }

    // Pull method header / arg count etc. so the JITMethod's
    // bookkeeping fields are correct.  The runtime depends on
    // argCount/tempCount being consistent with the CompiledMethod.
    if (!compiledMethod.isObject() || compiledMethod.rawBits() < 0x10000) {
        g_failed++;
        return nullptr;
    }
    ObjectHeader* methObj = compiledMethod.asObjectPtr();
    Oop headerOop = methObj->slotAt(0);
    if (!headerOop.isSmallInteger()) {
        g_failed++;
        return nullptr;
    }
    int64_t headerBits = headerOop.asSmallInteger();

    // Allocate code region in the existing JIT code zone, sized to
    // the cached trampoline bytes.  No IC entries (Phase 1 has no
    // sends).
    JITMethod* jm = zone.allocate(static_cast<uint32_t>(g_cached.size), 0);
    if (!jm) {
        g_failed++;
        return nullptr;
    }

    // Initialize fields the runtime reads.  Many are already zeroed
    // by allocate()'s memset.
    jm->compiledMethodOop = compiledMethod.rawBits();
    jm->methodHeader      = static_cast<uint64_t>(headerBits);
    jm->argCount          = static_cast<uint8_t>((headerBits >> 24) & 0x0F);
    jm->tempCount         = static_cast<uint8_t>((headerBits >> 18) & 0x3F);
    jm->numBytecodes      = 0;       // Phase 1 stub doesn't track bytecodes
    jm->numICEntries      = 0;
    jm->tier              = 1;
    jm->hasPrimPrologue   = false;
    jm->isBlock           = false;
    jm->pinned            = false;
    jm->hasSends          = false;
    jm->hasHeapWrites     = false;
    jm->hasRecvFieldAccess= false;
    jm->hasRecvFieldWrite = false;
    jm->hasLitVarWrite    = false;
    jm->maxRecvFieldIndex = 0;
    jm->isSpliceTarget    = false;

    // Copy the emitted bytes into the code region.
    std::memcpy(jm->codeStart(), g_cached.data, g_cached.size);

    // Flush icache for the newly written code, then mark
    // executable + Compiled.  Mirrors the stencil JIT's tail in
    // JITCompiler::compile around line 2720.
    platform::flushICache(jm->codeStart(), g_cached.size);
    jm->state = MethodState::Compiled;
    platform::makeExecutable(jm, jm->totalSize);

    // Register so dispatch can find it.
    methodMap.insert(compiledMethod.rawBits(), jm);

    g_compiled++;

    static const bool trace = std::getenv("PHARO_USE_ASMJIT_T1_TRACE") != nullptr;
    if (trace && (g_compiled <= 10 || (g_compiled % 100 == 0))) {
        std::fprintf(stderr,
                     "[asmjit-t1] #%zu compiled %llu -> jm=%p code=%p (%zu bytes)\n",
                     g_compiled,
                     static_cast<unsigned long long>(compiledMethod.rawBits()),
                     (void*)jm, (void*)jm->codeStart(), g_cached.size);
    }

    return jm;
}

size_t asmjitT1Compiled() { return g_compiled; }
size_t asmjitT1Failed()   { return g_failed;   }

}  // namespace jit
}  // namespace pharo

#endif  // PHARO_JIT_ENABLED
