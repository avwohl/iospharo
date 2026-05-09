/*
 * SistaLowering_x86_64.cpp - IR -> asmjit x86_64
 *
 * Per-arch sibling of SistaLowering_arm64.cpp.  Sista is the tier-2
 * JIT path: takes a SistaMethod IR (built by SistaBuilder) and emits
 * native machine code via asmjit.
 *
 * Status (2026-05-09): SCAFFOLD ONLY.  Always reports lowering failure
 * via `*failedAtValue = 0`, returns nullptr.  Methods that reach Sista
 * compilation fall back to tier-1 — same end-state as
 * `PHARO_SISTA_NO_LOWER=1` on the arm64 build.
 *
 * The op-by-op port plan lives in docs/linux-x86-port-2026-05-09.md
 * (see "Tier-2 / Sista x86_64 port").  Each op gets implemented here,
 * with the IR-walker shape preserved from the arm64 file.  When an op
 * with no x86_64 emit branch is encountered, set `*failedAtValue` and
 * return nullptr — the compiler will retry next time the method warms
 * up; meanwhile tier-1 handles execution.
 */
#include "SistaLowering.hpp"

#if PHARO_JIT_ENABLED

#include <asmjit/x86.h>
#include <asmjit/core/jitruntime.h>

namespace pharo {
namespace sista {

Lowering::Lowering() {
    runtime_ = new asmjit::JitRuntime();
}

Lowering::~Lowering() {
    delete runtime_;
}

Lowering::CompiledFn Lowering::lower(const Method& /*method*/,
                                       uint32_t* failedAtValue,
                                       const uint8_t* /*bytecodeBase*/) {
    // Scaffold: always defer to tier-1.  Op-by-op port pending.
    if (failedAtValue) *failedAtValue = 0;
    return nullptr;
}

}  // namespace sista
}  // namespace pharo

#endif  // PHARO_JIT_ENABLED
