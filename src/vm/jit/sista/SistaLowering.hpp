/*
 * SistaLowering.hpp - Sista IR -> native code
 *
 * Phase 2 of docs/sista-inlining-plan.md — takes a SistaMethod in IR
 * form and emits ARM64 machine code via asmjit, using the same
 * calling convention as the Tier 1 copy-and-patch JIT so the runtime
 * can invoke either tier transparently.
 *
 * Calling convention (ARM64):
 *   x0 = JITState* — input and output.
 *   Return value is placed in state->returnValue.
 *   state->exitReason is set before return.
 *
 * Current status: interface only.  Implementation pending — Phase 2
 * will land the lowerer for the same minimal subset the builder
 * supports (load_recv, load_temp, return).  When that lands, a
 * round-trip test (lift -> lower -> execute -> compare to stencil
 * JIT result) becomes possible and gates further Phase 2 work.
 */
#ifndef PHARO_SISTA_LOWERING_HPP
#define PHARO_SISTA_LOWERING_HPP

#include "SistaIR.hpp"

#include <cstddef>

namespace pharo {
namespace sista {

// Where to write the emitted code.  The Sista JIT will reuse the
// same CodeZone the Tier 1 JIT uses — keeps GC / W^X handling
// identical.  For unit tests, the caller can pass any buffer.
struct CodeSink {
    uint8_t* buffer;
    size_t   capacity;
    size_t   written;     // bytes produced so far (out)
};

// Lower a SistaMethod into the given sink.  Returns true on success.
// On failure the sink's `written` may be non-zero; the caller should
// discard the partial output.
//
// Not yet implemented (returns false).  When implemented:
//   1. Emit a prologue that saves callee-saved registers matching
//      the Tier 1 convention (keeps J2J call sites compatible).
//   2. Walk the IR in topological order, materializing each value
//      into a register (or spill slot for now).  Phase 2 uses a
//      trivial register allocator — each value gets a fresh reg
//      or a stack slot.
//   3. Emit an epilogue that sets state->exitReason = EXIT_RETURN,
//      writes state->returnValue, and returns.
bool lower(const Method& method, CodeSink& sink);

}  // namespace sista
}  // namespace pharo

#endif  // PHARO_SISTA_LOWERING_HPP
