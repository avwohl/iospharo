/*
 * jit_jmsize_other.cpp - JIT trampoline JM_SIZE sentinel (non-arm64)
 *
 * On non-arm64 architectures (currently x86_64), TrampolineAsm.S is
 * excluded from the build — the asm trampoline is arm64-only.  There's
 * no asm-side JM_SIZE constant to compare against, so the runtime check
 * becomes a tautology that always passes.
 *
 * When the asmjit x86_64 codegen lands and an x86_64 trampoline appears,
 * replace this with an arch-specific file that exports the trampoline's
 * actual constant.
 *
 * Linked into non-arm64 builds.  Selected by CMAKE_SYSTEM_PROCESSOR.
 */

#include "Platform.hpp"
#include "../vm/jit/JITMethod.hpp"

namespace pharo {
namespace platform {

uint64_t jitTrampolineJMSize() {
    // No asm trampoline on this arch; report sizeof so the runtime
    // check trivially holds.
    return sizeof(::pharo::jit::JITMethod);
}

}  // namespace platform
}  // namespace pharo
