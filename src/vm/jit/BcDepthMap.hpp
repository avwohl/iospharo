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

#include <cstdint>

namespace pharo {

class ObjectMemory;

namespace jit {

struct JITState;

// Verify state.sp against the static depth for state.ip's bytecode
// offset.  Call sites pass `where` (a static string) for the report.
// Only acts on exit reasons where ip points AT a bytecode boundary
// with operands still on the stack (Send, SendCached, MustBool,
// ArithOverflow, BlockCreate, ArrayCreate, Yield).
void spDepthCheck(JITState& state, const char* where);

// Same invariant for INTERPRETED frames, checked at send dispatch:
// catches a JIT return that delivered a wrong sp into an interp frame
// at the very next interpreted send.  ipPastSend = instructionPointer_
// after operand fetch (backscanned to the send opcode); sp/fp are the
// interpreter registers (sp one-past-TOS, fp at the receiver slot).
// Out-of-range/ambiguous sites are skipped silently (blocks, odd ips).
void spDepthCheckInterpSend(uint64_t methodOopBits,
                            const uint8_t* ipPastSend,
                            void* spRaw, void* fpRaw, int argCount,
                            pharo::ObjectMemory* memory,
                            const char* where);

// Consistency check for a J2JSave at materialize/pop time: the saved
// (ip, sp) pair must satisfy the depth invariant
//   save.sp == save.tempBase + tempCount + depth[save.ip - bcStart]
//              + save.sendArgCount
// (save.ip is the post-send resume offset; recv+args of the in-flight
// send are still on the stack above the post-send depth).  Catches
// wrong-but-in-range save.ip (stale caches, stale registers, slot
// reuse) at the moment of consumption — the mechanism behind the
// wrong-offset resumes that execute valid bytecode from the wrong
// position (silent eval loss / StdioStream-receiver DNUs).
void spDepthCheckJ2JSave(uint64_t saveJmMethodOop, const uint8_t* saveIp,
                         void* saveSp, void* saveTempBase,
                         int tempCount, int sendArgCount,
                         pharo::ObjectMemory* memory, const char* where);

// Interp-side maps are keyed by method oop bits — invalidate after a
// moving GC (call from Interpreter's afterGC).
void bcDepthMapsClearAfterGC();

// Print check/mismatch/unmappable counters (called from dumpJITStats).
void spDepthStatsDump();

}  // namespace jit
}  // namespace pharo
