// no_getenv.hpp — poison macro to prevent bare std::getenv() in VM hot paths.
//
// Every env-var read must go through debug_vars.h / DebugVars.hpp, which
// parses each variable ONCE at static-init.  Bare getenv() in hot paths is
// a perf trap — each call does a linear search through the env array, and a
// previous session lost 20-30% of bench-suite to "if (std::getenv(...))"
// patterns that LOOKED gated but executed per-call.
//
// CLAUDE.md mandates this; this header enforces it at compile time.
//
// To migrate an existing getenv call:
//   1. Add `DEBUG_BOOL(PHARO_TRACE_PRIM207)` (or DEBUG_INT/DEBUG_STR) to
//      src/vm/debug_vars.h.
//   2. Replace `if (std::getenv("PHARO_TRACE_PRIM207"))` with
//      `if (GET_DEBUG_BOOL(PHARO_TRACE_PRIM207))` (include DebugVars.hpp).
//   (The legacy DebugSettings.{hpp,cpp} pair is FROZEN — see the envPresent
//   RATCHET in CMakeLists.txt; do not add fields there.)
//
// DebugSettings.cpp itself MUST NOT include this header (it needs the real
// getenv).  Every other VM .cpp file should include it (directly or
// transitively).

#ifndef PHARO_NO_GETENV_HPP
#define PHARO_NO_GETENV_HPP

// This header is force-included (-include) BEFORE each VM .cpp's own includes.
// A bare `#define getenv X` here breaks the C++ standard library on libstdc++
// (x86_64 Linux): glibc's <stdlib.h> declares the renamed symbol, then
// libstdc++'s <cstdlib> does a literal `using ::getenv;` that no longer
// resolves ("'getenv' has not been declared in '::'").  libc++ (macOS) happens
// to tolerate it, but it is not portable.
//
// Fix: pull in the real declarations FIRST so the standard-library headers
// compile cleanly, then poison.  Re-includes of these headers from the .cpp hit
// their include guards, so only user call sites see the poison macro.
#include <cstdlib>
#include <stdlib.h>

#define getenv NEVER_CALL_GETENV_DIRECTLY_USE_DEBUGSETTINGS

#endif  // PHARO_NO_GETENV_HPP
