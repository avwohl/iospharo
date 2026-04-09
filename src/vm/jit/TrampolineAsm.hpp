/*
 * TrampolineAsm.hpp - Declaration for the hand-written J2J trampoline.
 *
 * Copyright (c) 2026 Aaron Wohl. Licensed under the MIT License.
 *
 * Implemented in src/vm/jit/TrampolineAsm.S. Selected at compile time via
 * the PHARO_ASM_TRAMPOLINE CMake option; the C++ fallback lives in
 * Interpreter::tryJITActivation.
 */

#ifndef PHARO_JIT_TRAMPOLINE_ASM_HPP
#define PHARO_JIT_TRAMPOLINE_ASM_HPP

#include "JITConfig.hpp"

#if PHARO_JIT_ENABLED && defined(PHARO_ASM_TRAMPOLINE) && defined(__aarch64__)

#include <cstddef>
#include <cstdint>

namespace pharo {
namespace jit {
struct JITState;
}
}

extern "C" {

// Hot loop for J2J sends, hand-written in ARM64 asm. The caller must have
// already populated `state` (notably state.exitReason) and toggled W^X to
// executable. On exit, writes localFrameDepth / calls / returns back through
// the out-pointers and leaves state.exitReason pointing at the reason the
// loop stopped (ExitReturn / ExitSendCached / ExitStackOverflow / etc.).
//
// saveStack must point to a buffer of at least 256*72 bytes on the C stack.
void pharo_jit_j2j_trampoline(
    pharo::jit::JITState* state,
    void*                 saveStack,
    size_t*               frameDepthInOut,
    size_t*               callsCountOut,
    size_t*               returnsCountOut,
    uint64_t              nilOopBits);

} // extern "C"

#endif // PHARO_JIT_ENABLED && PHARO_ASM_TRAMPOLINE && __aarch64__

#endif // PHARO_JIT_TRAMPOLINE_ASM_HPP
