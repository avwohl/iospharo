#pragma once
#include <cstdint>

namespace pharo {

// VM-side tables the scavenger visits as roots (the method cache; the JIT's
// method headers, inline caches and maps) hold young objects only when one
// was stored since the last scavenge: every scavenge tenures all survivors.
// Their writers call noteJitOop (the method cache sets its flag in
// cacheMethod); the scavenger walks a table only when its
// flag is set, then clears it.  The flags start set (an image may start
// with anything).  g_newSpaceLo/Hi: set once when new space is allocated.
inline uint64_t g_newSpaceLo = 0, g_newSpaceHi = ~0ull;
inline bool g_mcacheMayHoldYoung = true;
inline bool g_jitMayHoldYoung = true;
inline bool isNewSpaceBits(uint64_t bits) { return bits - g_newSpaceLo < g_newSpaceHi - g_newSpaceLo; }
inline void noteJitOop(uint64_t bits) { if (isNewSpaceBits(bits)) g_jitMayHoldYoung = true; }
// Whether the last scavenge found the JIT tables possibly holding young
// objects (a young method compiled, say): only then can it have moved a
// compiled method, so only then must the method map be rebuilt.
inline bool g_jitWalkedAtLastScavenge = true;

}  // namespace pharo
