// ZoneEviction.hpp — code-zone allocation with cold-method eviction.
//
// Why this is a free function and not a JITCompiler member: BOTH compile
// paths need it.  The eviction machinery (live-method pinning, LRU evict,
// J2J IC scrubbing, full flush) lived inline in the stencil path of
// JITCompiler::compile, below an unconditional `return jm` in the
// asmjit-T1 branch.  Since asmjit-T1 is the DEFAULT path
// (DebugSettings.hpp: useAsmjitT1 = true, opt-out only), the whole block
// was unreachable in production: AsmjitT1.cpp gave up on a full zone with
// no retry, so the JIT stopped compiling a few percent into any long run
// and never resumed.  Measured on a full arm64 SUnit sweep: the zone
// filled at 160M method entries with 22,057 methods compiled, and the run
// continued to 12.4 BILLION entries with the count frozen at 22,060 —
// i.e. 98.7% of the run interpreted, and "Incremental evict" appearing
// zero times in the log.
//
// Extracting it here is a RE-ROUTE of existing, already-debugged code, not
// a new subsystem.

#ifndef PHARO_JIT_ZONE_EVICTION_HPP
#define PHARO_JIT_ZONE_EVICTION_HPP

#include "JITConfig.hpp"

#if PHARO_JIT_ENABLED

#include <cstddef>
#include <cstdint>

namespace pharo {

class Interpreter;

namespace jit {

class CodeZone;
class MethodMap;
struct JITMethod;

// One freed span of the code zone, [start, end).  Collected during eviction
// so every cache holding raw code addresses can be scrubbed against it.
struct EvictedCodeRange {
    uint64_t start;
    uint64_t end;
};

// Allocate `codeSize` bytes (plus `numSendSites` IC sites) in the code
// zone.  On a full zone: pin every method live on the active stack, in the
// J2J save pool, or in a suspended process's context chain; LRU-evict cold
// methods into the free list; retry.  If that is still not enough, flush
// every unpinned method, flush the caches, and retry once more.
//
// Returns nullptr only if allocation is impossible even after a full
// flush, or if eviction could not be proved safe (no stack bounds).  The
// caller owns its own failure counter.
JITMethod* allocateWithEviction(CodeZone& zone, MethodMap& methodMap,
                                Interpreter& interp, size_t codeSize,
                                uint32_t numSendSites);

}  // namespace jit
}  // namespace pharo

#endif  // PHARO_JIT_ENABLED
#endif  // PHARO_JIT_ZONE_EVICTION_HPP
