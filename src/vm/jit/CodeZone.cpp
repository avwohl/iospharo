/*
 * CodeZone.cpp - Machine code zone implementation
 *
 * Copyright (c) 2026 Aaron Wohl. Licensed under the MIT License.
 *
 * Currently header-only (CodeZone is fully inline for performance).
 * This file exists as a compilation unit to:
 * 1. Verify the headers compile cleanly
 * 2. House future non-inline methods (compaction helpers, diagnostics)
 * 3. Provide a link target for the build system
 */

#include "CodeZone.hpp"
#include "JITMethod.hpp"
#include "PlatformJIT.hpp"
#include "JITConfig.hpp"

#if PHARO_JIT_ENABLED

namespace pharo {
namespace jit {

// Static assertions for layout guarantees
static_assert(sizeof(ICEntry::CacheSlot) == 16, "CacheSlot should be 16 bytes");
static_assert(sizeof(ICEntry) <= 112, "ICEntry should be compact");
static_assert(MethodAlignment >= alignof(JITMethod),
              "MethodAlignment must satisfy JITMethod alignment requirements");

// Out-of-line definition of an inline-default destructor so this
// translation unit emits at least one symbol.  Without it the .o
// is empty and `ranlib` complains during static-archive creation:
//   ranlib: warning: 'libPharoVMCore.a(CodeZone.cpp.o)' has no symbols
// Pure cosmetic — keeping CodeZone otherwise header-inline.
CodeZone::~CodeZone() { destroy(); }

} // namespace jit
} // namespace pharo

#endif // PHARO_JIT_ENABLED
