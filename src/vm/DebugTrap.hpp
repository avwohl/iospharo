/*
 * DebugTrap.hpp - portable software breakpoint for env-gated diagnostics.
 *
 * __builtin_debugtrap is clang/MSVC only; GCC (the Linux x86_64/arm64
 * build compiler) has no direct equivalent, so emit a software-breakpoint
 * instruction per arch, falling back to __builtin_trap.  All call sites
 * are env-var-gated diagnostics, never on the hot path.
 */
#ifndef PHARO_DEBUG_TRAP_HPP
#define PHARO_DEBUG_TRAP_HPP

#if defined(__has_builtin) && __has_builtin(__builtin_debugtrap)
#  define PHARO_DEBUGTRAP() __builtin_debugtrap()
#elif defined(__i386__) || defined(__x86_64__)
#  define PHARO_DEBUGTRAP() __asm__ __volatile__("int3")
#elif defined(__aarch64__)
#  define PHARO_DEBUGTRAP() __asm__ __volatile__("brk #0")
#else
#  define PHARO_DEBUGTRAP() __builtin_trap()
#endif

#endif  // PHARO_DEBUG_TRAP_HPP
