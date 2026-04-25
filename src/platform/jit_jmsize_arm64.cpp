/*
 * jit_jmsize_arm64.cpp - JIT trampoline JM_SIZE sentinel (arm64)
 *
 * On arm64, src/vm/jit/TrampolineAsm.S has a hand-written J2J trampoline
 * that hardcodes #JM_SIZE in `add Xn, methodHdr, #JM_SIZE` instructions.
 * That assembly defines `pharo_jit_jm_size_check` so the C++ side can
 * verify the constant matches sizeof(JITMethod) at startup.  If they
 * drift, every trampoline computes a wrong field offset.
 *
 * This file links into arm64 builds (Apple arm64, Linux arm64, iOS).
 * Non-arm64 architectures get jit_jmsize_other.cpp instead — same
 * jitTrampolineJMSize() symbol, different definition.  CMake selects
 * by CMAKE_SYSTEM_PROCESSOR.
 */

#include "Platform.hpp"

namespace pharo {
namespace platform {

extern "C" const uint64_t pharo_jit_jm_size_check;

uint64_t jitTrampolineJMSize() {
    return pharo_jit_jm_size_check;
}

}  // namespace platform
}  // namespace pharo
