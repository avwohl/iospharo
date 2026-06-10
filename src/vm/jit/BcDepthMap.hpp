/*
 * BcDepthMap.hpp - static operand-stack depth verification for JIT exits
 *
 * Copyright (c) 2026 Aaron Wohl. Licensed under the MIT License.
 *
 * Sista bytecode has a static stack discipline: the operand-stack depth
 * at every bytecode offset is a compile-time constant (merge points must
 * agree).  That makes sp-desync corruption — the class behind every
 * root-caused J2J corruption so far (stale j2jDepth resync skip, saveless
 * return hijack): all victims were temp/operand slots shifted by a wrong
 * state.sp — detectable at ANY JIT exit whose ip points at a bytecode:
 *
 *     expected sp == state.tempBase + tempCount - 1 + depth[bcOffset]
 *
 * spDepthCheck() verifies that invariant.  It is value-independent, so a
 * desynced frame trips it at the FIRST subsequent checkable exit (every
 * IC-miss/IC-hit send boundary), not only when a shifted operand happens
 * to be an implausible receiver.  Gated on PHARO_SP_DEPTH_CHECK; no-op
 * (one array-indexed bool test) otherwise.  Like the shadow-slot
 * instrument, it is layout-independent evidence — see WIP.md on the
 * knife-edge epistemology.
 *
 * Depth maps are computed lazily per JITMethod by a worklist walk over
 * the bytecodes (BcDepthMap.cpp) and cached in a side table keyed by
 * JITMethod*.  Methods containing opcodes with non-static effects
 * (InlinedPrimitive 0xEC, legacy PushClosure 0xFA, unassigned ranges)
 * or merge conflicts are marked unmappable and skipped (counted).
 */

#pragma once

namespace pharo {
namespace jit {

struct JITState;

// Verify state.sp against the static depth for state.ip's bytecode
// offset.  Call sites pass `where` (a static string) for the report.
// Only acts on exit reasons where ip points AT a bytecode boundary
// with operands still on the stack (Send, SendCached, MustBool,
// ArithOverflow, BlockCreate, ArrayCreate, Yield).
void spDepthCheck(JITState& state, const char* where);

// Print check/mismatch/unmappable counters (called from dumpJITStats).
void spDepthStatsDump();

}  // namespace jit
}  // namespace pharo
