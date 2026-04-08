/*
 * JITConfig.hpp - JIT compiler configuration and platform detection
 *
 * Copyright (c) 2026 Aaron Wohl. Licensed under the MIT License.
 *
 * Defines compile-time constants for the JIT subsystem:
 * - Whether JIT is enabled (never on iOS)
 * - Code zone sizing
 * - Compilation thresholds
 * - Architecture detection
 */

#ifndef PHARO_JIT_CONFIG_HPP
#define PHARO_JIT_CONFIG_HPP

#include <cstddef>
#include <cstdint>

namespace pharo {
namespace jit {

// ===== JIT ENABLE/DISABLE =====

// JIT is disabled on iOS (kernel forbids W^X pages) and can be
// force-disabled at build time via -DPHARO_JIT_ENABLED=0.
#ifndef PHARO_JIT_ENABLED
  #if defined(__APPLE__) && defined(__arm64__) && defined(__IPHONE_OS_VERSION_MIN_REQUIRED)
    #define PHARO_JIT_ENABLED 0
  #elif defined(TARGET_OS_IOS) && TARGET_OS_IOS && !TARGET_OS_MACCATALYST
    #define PHARO_JIT_ENABLED 0
  #else
    #define PHARO_JIT_ENABLED 1
  #endif
#endif

static constexpr bool Enabled = (PHARO_JIT_ENABLED != 0);

// ===== ARCHITECTURE DETECTION =====

enum class Arch : uint8_t {
    ARM64,
    X86_64,
    Unknown
};

#if defined(__aarch64__) || defined(_M_ARM64)
  static constexpr Arch HostArch = Arch::ARM64;
#elif defined(__x86_64__) || defined(_M_X64)
  static constexpr Arch HostArch = Arch::X86_64;
#else
  static constexpr Arch HostArch = Arch::Unknown;
#endif

// ===== OS DETECTION =====

enum class OS : uint8_t {
    macOS,
    Linux,
    Windows,
    Unknown
};

#if defined(__APPLE__) && defined(__MACH__)
  static constexpr OS HostOS = OS::macOS;
#elif defined(__linux__)
  static constexpr OS HostOS = OS::Linux;
#elif defined(_WIN32)
  static constexpr OS HostOS = OS::Windows;
#else
  static constexpr OS HostOS = OS::Unknown;
#endif

// ===== CODE ZONE SIZING =====

// Default machine code zone size (16 MB, same as Cog)
static constexpr size_t DefaultCodeZoneSize = 16 * 1024 * 1024;

// Minimum code zone (1 MB)
static constexpr size_t MinCodeZoneSize = 1 * 1024 * 1024;

// Maximum code zone (256 MB)
static constexpr size_t MaxCodeZoneSize = 256 * 1024 * 1024;

// ===== COMPILATION THRESHOLDS =====

// Number of calls before a method gets compiled (Cog uses 2)
static constexpr uint32_t CompileThreshold = 2;

// Maximum bytecode count for a single method we'll attempt to compile.
// Methods larger than this stay interpreted.
static constexpr size_t MaxCompilableBytecodes = 4096;

// ===== JIT METHOD SIZING =====

// Maximum machine code size for a single compiled method (64 KB)
static constexpr size_t MaxMethodCodeSize = 64 * 1024;

// Alignment for JIT method entries (cache-line aligned)
static constexpr size_t MethodAlignment = 64;

// ===== INLINE CACHE =====

// Maximum entries in a closed polymorphic inline cache (PIC)
static constexpr size_t MaxPICEntries = 6;

// ===== MEGAMORPHIC METHOD CACHE =====
//
// A simple direct-mapped cache probed by JIT stencils when the 4-entry
// PIC misses. Keyed on (selectorBits, classIndex).

static constexpr size_t MegaCacheSize = 65536;  // Must be power of 2

struct MegaCacheEntry {
    uint64_t selectorBits;
    uint64_t classIndex;     // For objects: class index (22-bit); for immediates: tag|0x80000000
    uint64_t methodBits;     // Oop bits of the resolved CompiledMethod
};

// ===== PAGE SIZE =====

// We use 16 KB as the assumed page size. On ARM64 macOS/iOS this is the
// actual page size. On x86_64 Linux (4 KB pages) this is conservative
// but correct -- mprotect works on any multiple of the real page size.
static constexpr size_t PageSize = 16 * 1024;

} // namespace jit
} // namespace pharo

#endif // PHARO_JIT_CONFIG_HPP
