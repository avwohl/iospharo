/*
 * AsmjitT1.hpp - asmjit-based Tier-1 JIT compiler (Phase 1 skeleton)
 *
 * Per scripts/jit-diff/plan_asmjit_replacement.md, this is the
 * eventual replacement for the stencil-based JITCompiler.  Phase 1
 * lands the integration plumbing only — every method compiles to a
 * single trampoline that immediately bails to the interpreter via
 * `state->exitReason = ExitSend`.  The fuzzer should hold at 0/N
 * PASS but with zero crashes/hangs (every JIT entry routes through
 * interp on first call).
 *
 * Wired in via `JITCompiler::compile`'s top-of-function check on the
 * env flag `PHARO_USE_ASMJIT_T1=1`.  When that flag is unset the
 * stencil path runs unchanged.
 *
 * Phase 2+ will add real per-bytecode emit functions here (push/pop,
 * arithmetic, sends, control flow).
 */

#ifndef PHARO_JIT_ASMJIT_T1_HPP
#define PHARO_JIT_ASMJIT_T1_HPP

#if PHARO_JIT_ENABLED

#include "../JITMethod.hpp"

namespace pharo {

class ObjectMemory;
class Interpreter;

namespace jit {

class CodeZone;
class MethodMap;

// Phase 1 entry point.  Compiles `compiledMethod` via asmjit to a
// minimal "set ExitSend; ret" trampoline; allocates a JITMethod in
// `zone`; copies emitted bytes into codeStart(); registers in
// `methodMap`.  Returns the JITMethod*, or nullptr on failure.
//
// The returned JITMethod is `Compiled` and `isExecutable()` so the
// existing dispatch (JIT_CALL(jm->codeStart(), state)) invokes the
// asmjit-emitted trampoline.  Trampoline writes state->exitReason
// = ExitSend (= 2) and returns; the interp loop then sees the
// ExitSend and runs the method via the interpreter as if the JIT
// had bailed mid-execution.
JITMethod* compileViaAsmjit(CodeZone& zone, MethodMap& methodMap,
                             ObjectMemory& memory, Interpreter& interp,
                             Oop compiledMethod);

// Stats accessors (incremented per call to compileViaAsmjit).
size_t asmjitT1Compiled();
size_t asmjitT1Failed();

}  // namespace jit
}  // namespace pharo

#endif  // PHARO_JIT_ENABLED

#endif  // PHARO_JIT_ASMJIT_T1_HPP
