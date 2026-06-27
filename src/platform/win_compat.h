/*
 * win_compat.h - lightweight POSIX shims for the Windows (clang/LLVM-MinGW)
 * build.  Safe to include from widely-included headers: it pulls in only
 * <csetjmp> and defines nothing on non-Windows.  Heavier shims that need
 * <windows.h> (e.g. mmap) live in their own headers (win_mman.h) included
 * only by the one TU that needs them.
 *
 * See docs/windows-port-plan.md.
 */

#ifndef PHARO_WIN_COMPAT_H
#define PHARO_WIN_COMPAT_H

#ifdef _WIN32

// ---- sigsetjmp / siglongjmp ------------------------------------------------
// Windows has no signal-mask jump.  The VM uses sigsetjmp/siglongjmp purely
// for non-local control flow (callback re-entry, SIGSEGV recovery); the saved
// signal mask is irrelevant here, so plain setjmp/longjmp are exact
// replacements.
#include <csetjmp>
typedef jmp_buf sigjmp_buf;
#define sigsetjmp(env, savesigs) setjmp(env)
#define siglongjmp(env, val)     longjmp((env), (val))

// ---- execinfo backtrace ----------------------------------------------------
// glibc's backtrace() is diagnostic-only (crash dumps, DNU traces).  No
// portable equivalent without DbgHelp; stub to empty so the call sites
// compile and simply print no frames on Windows.
static inline int backtrace(void** /*buf*/, int /*size*/) { return 0; }
static inline char** backtrace_symbols(void* const* /*buf*/, int /*size*/) { return nullptr; }
static inline void backtrace_symbols_fd(void* const* /*buf*/, int /*size*/, int /*fd*/) {}

#endif  // _WIN32
#endif  // PHARO_WIN_COMPAT_H
