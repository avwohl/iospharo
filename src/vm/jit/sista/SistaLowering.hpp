/*
 * SistaLowering.hpp - Sista IR -> native code (asmjit ARM64)
 *
 * Phase 2 of docs/sista-inlining-plan.md.  Takes a SistaMethod in IR
 * form and emits ARM64 machine code via asmjit.  Calling convention
 * matches Tier 1's copy-and-patch JIT so the runtime can invoke
 * either tier transparently:
 *
 *   Signature: void fn(JITState* state)
 *   x0 = state
 *   Return via state->returnValue + state->exitReason = EXIT_RETURN.
 *
 * Current scope: the same minimal bytecode subset that SistaBuilder
 * handles — load_recv, load_temp, return.  Expand incrementally; each
 * new op lands with a round-trip test (lift → lower → execute →
 * assert result matches Tier 1).
 */
#ifndef PHARO_SISTA_LOWERING_HPP
#define PHARO_SISTA_LOWERING_HPP

#include "SistaIR.hpp"

// asmjit inline-namespaces JitRuntime (asmjit::v13::...).  Forward
// declaration in plain `namespace asmjit` doesn't match.  Pull in the
// real header; it's small.
#include <asmjit/core/jitruntime.h>

namespace pharo {
namespace sista {

// Owns an asmjit::JitRuntime so the emitted code stays resident for
// the lifetime of the Lowering object.  Keep one per compilation
// domain (Sista runtime); don't recreate per-method.
class Lowering {
public:
    Lowering();
    ~Lowering();

    Lowering(const Lowering&) = delete;
    Lowering& operator=(const Lowering&) = delete;

    // Lower the IR to native code.  Returns a callable function pointer
    // on success, nullptr if the method contains ops the current
    // lowerer doesn't handle.  The returned pointer is valid until this
    // Lowering is destroyed (the asmjit runtime manages the code).
    //
    // `failedAtValue`, if non-null, is set to the id of the first
    // unsupported value encountered on failure.
    using CompiledFn = void (*)(void* state);
    CompiledFn lower(const Method& method, uint32_t* failedAtValue = nullptr);

private:
    asmjit::JitRuntime* runtime_ = nullptr;
};

}  // namespace sista
}  // namespace pharo

#endif  // PHARO_SISTA_LOWERING_HPP
