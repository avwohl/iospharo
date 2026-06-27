# Windows Port Plan — iospharo-jit (branch `jit`)

> Produced 2026-06-27 from an 11-agent gap-analysis workflow (7 subsystem
> readers + 3 adversarial JIT-ABI verifiers + synthesis). All file:line
> references were current at HEAD `9bc6a10f`.

## 1. Executive summary

Windows was anticipated in the design but never implemented: `JITConfig.hpp`
already detects `OS::Windows`/x86-64, `Platform.hpp` comments describe the
intended `VirtualProtect`/RWX behavior, and the architecture/source-selection
logic in CMake already falls through to the correct x86-64 source set on
`CMAKE_SYSTEM_PROCESSOR==AMD64`. But there is no `src/platform/windows.cpp`, no
`WIN32`/`MSVC` branch anywhere in `CMakeLists.txt`, and the core VM is written
against a POSIX/glibc baseline (exactly one `#ifdef _WIN32` exists today, at
`Primitives.cpp:13835`). So a Windows configure currently selects no platform
file, links no libraries, and fails at both compile and link.

The toolchain MUST be clang. `Interpreter.cpp` uses computed gotos
(`goto *dispatchTable[bc]`) and label-as-value, plus GNU AT&T inline asm in
`JITState.hpp` — none of which MSVC `cl.exe` accepts. clang (clang-cl or
LLVM-MinGW) compiles every GNU language extension in the tree (computed goto,
`__int128`, the full `__builtin_*` family, GNU inline asm) natively, so the
language features need zero `#ifdef`. The portability work is entirely (1) POSIX
system headers/APIs and (2) the missing platform/build plumbing.

Recommended strategy: ship a small headless milestone first —
`test_load_image.exe`, clang, **JIT disabled** (`PHARO_JIT_ENABLED=0`,
interpreter fallback), no SDL2, `PHARO_WITH_CRYPTO=OFF`, SocketPlugin excluded —
then layer JIT-on-Win64, sockets/crypto, GUI/SDL2, and signing as later
milestones. All three adversarial JIT-ABI lenses independently converge on
disable-JIT-for-milestone-1.

## 2. Recommended milestone sequence

### Milestone 1 — Headless interpreter `test_load_image.exe` (smallest thing that builds and runs)

Goal: a clang-built Windows x64 console exe that loads a standard Pharo image
and runs the interpreter dispatch loop to a known result (e.g. the injected
SUnit runner), JIT off.

Work items (file-level):
- Add a `WIN32` branch to `CMakeLists.txt` platform selection
  (`CMakeLists.txt:203-238`) mirroring the Linux block: append
  `src/platform/windows.cpp`, a headless WorldRenderer stub, and the
  SoundPlugin/MIDIPlugin non-Apple stub TUs.
- Add early guard `if(WIN32 AND MSVC) message(FATAL_ERROR ...)` and keep all
  existing GNU-style `-Wall/-Wextra/-include/-w` flags on the WIN32 path (clang
  driver, not clang-cl reinterpretation).
- Set `PHARO_JIT_ENABLED=0` on the WIN32 x86-64 target (see section 3).
- Configure with `-DPHARO_WITH_CRYPTO=OFF`.
- Create `src/platform/windows.cpp` (the ~9 platform-seam functions + headless
  link stubs; see section 5).
- Create `src/vm/WorldRenderer_windows_stub.cpp` (or rename/share
  `WorldRenderer_linux_stub.cpp` as `WorldRenderer_headless_stub.cpp`) —
  `Interpreter.hpp:999` declares `WorldRenderer worldRenderer_;` unconditionally,
  so an impl MUST link even with JIT off.
- Add the SoundPlugin/MIDIPlugin TUs to the WIN32 source list (their `#else`
  stubs at e.g. `SoundPlugin.cpp:230` compile fine; they just must be added or
  `Primitives.cpp` won't link).
- Create `src/platform/win_dl.h` dl-compat shim (`dlopen`->`LoadLibraryA`,
  `dlsym`->`GetProcAddress`, `dlclose`->`FreeLibrary`, `RTLD_DEFAULT` emulation)
  and include it where `<dlfcn.h>` is now used.
- Source shims (POSIX include guards + Win32 substitutes) in: `ObjectMemory.cpp`
  (heap mmap->VirtualAlloc), `FFI.cpp` (dl + `usleep` at `FFI.cpp:1173` +
  `<malloc.h>` for `alloca`), `Interpreter.cpp` (`<execinfo.h>/<cxxabi.h>/
  <pthread.h>/<csignal>` + `sigsetjmp`->`setjmp`), `Primitives.cpp` (POSIX
  headers, directory/time/sysconf), `InterpreterProxy.cpp`, `Profiler.cpp` (stub
  on `_WIN32`), `test_load_image.cpp` (POSIX headers + SEH for SIGSEGV).
- Exclude `SocketPlugin.cpp` from `VM_SOURCES` on WIN32 (stub its primitives) to
  defer the large winsock port.
- Gate `TrampolineAsm.S` out of `VM_SOURCES` on non-ARM64 (it preprocesses to
  empty anyway) to drop the MASM-vs-GAS ASM-dialect risk.
- `git submodule update --init --recursive` for `third_party/asmjit` (still
  built/linked even with JIT off; it compiles cleanly under Windows/clang).
- libffi for Win64: vcpkg/MSYS2/vendored; add WIN32 include+link branch;
  `static_assert(FFI_DEFAULT_ABI==FFI_WIN64)` on `_WIN64`.
- Link `ws2_32` (needed by the core DNS path in `Primitives.cpp:28555` even
  without SocketPlugin); call `WSAStartup` in `platformInit`.

Done-condition: `cmake --build build` produces `build/test_load_image.exe`;
running it against a fresh Pharo image (with the injected SUnit runner) writes
`sunit_test_results.txt` with a pass count comparable to the Linux
interpreter-only baseline. No JIT.

### Milestone 2 — JIT enabled on Win64

Goal: `PHARO_JIT_ENABLED=1` on Windows, Tier-1 + Tier-2 + Sista running.

Work items:
- Rework the two always-live AsmjitT1 helper-call sites for Win64 (see sections
  3/4): `jit_rt_primsize_ptr` (`AsmjitT1.cpp:2199`) and `jit_rt_primat_ptr`
  (`AsmjitT1.cpp:2343`) — switch arg regs SysV->Win64 (RCX/RDX/R8/R9), add
  `sub rsp,32`/`add rsp,32` shadow space while preserving 16B alignment. Same
  treatment for the six debug/verify-gated sites before enabling those knobs.
- Verify `asmjit Environment::host()` emits Win64 prologues for Tier-2
  (`Tier2Compiler_x86_64.cpp:498`) and Sista (`SistaLowering_x86_64.cpp:234`) —
  expected automatic, no source change.
- Regenerate `generated_stencils_x86_64.hpp` with `-target
  x86_64-pc-windows-msvc` and add a COFFParser to `scripts/extract_stencils.py`
  (only needed if the legacy `PHARO_NO_ASMJIT_T1` copy-and-patch path is ever
  wanted; AsmjitT1 is the default).
- Confirm `JIT_CALL` inline-asm branch (`JITState.hpp:449`, guarded
  `__x86_64__ || _M_X64`) stays selected (it already is).

Done-condition: JIT-on test suite matches Linux JIT pass rates; no crashes in
`at:`/`new:` hot paths.

### Milestone 3 — Sockets + Crypto parity

Goal: full SocketPlugin and OpenSSL on Windows.

Work items: port `SocketPlugin.cpp:12-39` to winsock2 (SOCKET, `closesocket`,
`ioctlsocket(FIONBIO)`, `WSAGetLastError`); extend OpenSSL find/link into the
WIN32 branch (vcpkg) and turn `PHARO_WITH_CRYPTO=ON`.

Done-condition: socket + SSL SUnit tests pass.

### Milestone 4 — GUI / SDL2

Goal: windowed Pharo (Morphic) on Windows. Out of scope until headless is solid;
SDL2 stubs + a Windows rendering surface replacing the headless WorldRenderer
stub.

### Milestone 5 — Signing the deliverable

Goal: Authenticode-signed `test_load_image.exe`. Add a
`WIN32 AND PHARO_SIGN_WINDOWS` POST_BUILD step mirroring the APPLE codesign block
(`CMakeLists.txt:636-649`) calling `scripts/sign-windows.ps1`, which resolves the
existing kit (`$env:Z80CPMW_SIGNING_KIT` else `C:/temp/in/z80cpmw-signing-kit`)
and shells to its `sign.ps1`. Gate the option OFF by default. Do not vendor the
kit or its secret into the repo. No installer needed.

Done-condition: `signtool verify /pa build/test_load_image.exe` succeeds;
publisher shows `CN=Aaron Wohl`.

## 3. The JIT-on-Windows decision

Consensus verdict from all three adversarial lenses:
**jit-disable-for-milestone-1**.

Rationale (verified across `AsmjitT1.cpp`, `Tier2Compiler_x86_64.cpp`,
`SistaLowering_x86_64.cpp`, `generated_stencils_x86_64.hpp`, `JITState.hpp`):
- The default, always-on Tier-1 (`AsmjitT1.cpp`) uses the low-level
  `asmjit::x86::Assembler` with `code.init(Environment::host())` and NO
  `FuncFrame`/`FuncDetail` (`AsmjitT1.cpp:9768`), so it applies zero ABI
  abstraction: state pinned in RDI, Smalltalk SP in RBX, helper args marshalled
  in System V order with only push-based 16B realignment and no Win64 32-byte
  shadow space.
- Red zone is NOT a problem (confirmed by all lenses): a scan for negative-offset
  RSP writes returns zero matches; all arg staging is push + positive
  `ptr(rsp,+N)` reads. Open question on red-zone reliance is closed.
- Shadow space IS the real defect: with no `sub rsp,32`, a Win64 helper's
  home-slot stores land on `[rsp..rsp+32)`, overwriting the just-pushed saved
  state, so `pop rdi` reloads a corrupted state pointer. Plus wrong arg registers
  (helpers read RCX/RDX/R8/R9, code writes RDI/RSI/RDX).
- BUT the always-live ABI surface in a default build is only TWO call sites:
  `AsmjitT1.cpp:2199` (`jit_rt_primsize_ptr`) and `AsmjitT1.cpp:2343`
  (`jit_rt_primat_ptr`). The other six are behind default-OFF debug/verify knobs.
  So the eventual fix is small/medium and localized, NOT a rewrite — but those
  two sites are hot (`at:`/`new:`), so it is a genuine blocker for an enabled JIT.
- Tier-2/Sista use the high-level `asmjit Compiler` and are already ABI-portable,
  but they sit on Tier-1, so they are dead until Tier-1 is fixed.

How to disable for milestone 1: set `PHARO_JIT_ENABLED=0` in the WIN32 CMake
branch (same proven config iOS device builds use, `CMakeLists.txt:254-259`). This
is clean: every JIT TU wraps its entire body in `#if PHARO_JIT_ENABLED`
(`AsmjitT1.cpp:32`, `JITRuntime.cpp:39`, `JITCompiler.cpp:26`,
`Tier2Compiler_*`, `SistaLowering_x86_64.cpp:40`, `CodeZone.cpp:18`,
`SendSitePatcher.cpp:9`, `BcDepthMap.cpp:18`), and `JITState.hpp` (incl. the
SysV inline-asm `JIT_CALL` macros) is wrapped at `JITState.hpp:28`. All
`JIT_CALL` call sites in `Interpreter.cpp` fall inside `PHARO_JIT_ENABLED`
regions. The W^X platform-seam functions (`allocCodeMemory`, `makeWritable`,
etc.) are referenced ONLY from JIT-gated code, so JIT=0 drops the entire ABI/W^X
surface from the link.

What that leaves working: the full bytecode interpreter (computed-goto dispatch —
a clang dependency, NOT an ABI dependency; clang/clang-cl/MinGW all support
label-as-value), image load/save, primitives, FFI, the core DNS path. The
interpreter alone is ABI-neutral.

Important: `PHARO_JIT_ENABLED=0` is necessary, NOT a preprocessor accident. The
AsmjitT1 x86 emit is guarded `defined(__x86_64__) || defined(_M_X64)`
(`AsmjitT1.cpp:232,1096,...`), and clang-cl defines `_M_X64`, so the
SysV-hardcoded emit WOULD compile and be active without the flag. Under
LLVM-MinGW, `__x86_64__` is defined too, so `generated_stencils_x86_64.hpp:16`
would compile the SysV blobs in if the legacy path were used. Do not rely on
guard luck.

## 4. Blocker / major inventory (by subsystem, trivial-effort first within each)

### Build system (CMakeLists.txt)
```
trivial  asmjit submodule not checked out -> add_subdirectory fails
         CMakeLists.txt:198 / .gitmodules:4-7
         Fix: git submodule update --init --recursive (Apple-only patch is inert on Win)
small    POSIX-only link libs (pthread/dl/m); Windows needs ws2_32, none of those
         CMakeLists.txt:510-519
         Fix: WIN32 branch links ws2_32 only; threads/mutex from clang runtime
small    PHARO_WITH_CRYPTO ON pulls sqGenericSSL.c but OpenSSL link is Linux-only
         CMakeLists.txt:30,144-160,512-519
         Fix: milestone-1 configure -DPHARO_WITH_CRYPTO=OFF
medium   No WIN32 branch selects a platform impl (windows.cpp absent)
         CMakeLists.txt:203-238
         Fix: elseif(WIN32) appending windows.cpp + headless renderer + Sound/MIDI stubs
medium   Flag plumbing is GNU-style; must force clang driver, forbid MSVC/clang-cl
         CMakeLists.txt:316-334,385-404
         Fix: keep -include/-w/-Wall as-is; FATAL_ERROR if(WIN32 AND MSVC)
medium   No WIN32 libffi handling; FFI.cpp always compiled, needs ffi.h + lib
         CMakeLists.txt:309-314,448-501 / FFI.cpp
         Fix: WIN32 branch -> vcpkg/MSYS2 libffi; ensure FFI_WIN64
medium   test_load_image.cpp POSIX-only headers (execinfo/libgen/unistd/dlfcn/sigjmp)
         test_load_image.cpp:23-60
         Fix: _WIN32 shims (_chdir, PathRemoveFileSpec, CaptureStackBackTrace, SEH)
```

### Platform abstraction layer
```
trivial  windows.cpp must provide headless link stubs (gDisplaySurface + 4 vm_* hooks)
         mirror linux.cpp:26-39
small    Create src/platform/windows.cpp with ~9 seam functions (RWX, mirrors linux.cpp)
         Platform.hpp:40-168 ; fix stale Platform.hpp:55 VirtualProtect comment
small    Add WIN32 branch to platform-file selection (none exists)
         CMakeLists.txt:203-238
```
Confirmed no-ops: inline `flipJitToWritable/Executable` `#else` already covers
Windows (`Platform.hpp:88-116`); `jitTrampolineJMSize` comes from
`jit_jmsize_other.cpp:25-29`, must NOT be redefined in windows.cpp.

### Core VM POSIX surface (src/vm)
```
small    Unconditional POSIX system headers break compile (pthread/execinfo/cxxabi/
         unistd/sys/* ...) Interpreter.cpp:38-50, Primitives.cpp:41-61, FFI.cpp:25
         Fix: #ifndef _WIN32 guards + Windows counterparts in a posix-compat header
small    libffi ABI: code uses FFI_DEFAULT_ABI (FFI.cpp:1787) -> must link Win64 libffi
         Fix: build libffi for x86_64-windows; static_assert FFI_DEFAULT_ABI==FFI_WIN64
small    sysconf/setenv/usleep/statvfs (Primitives.cpp:24004-24125, FFI.cpp:1173)
         Fix: GetSystemInfo/GlobalMemoryStatusEx/_putenv_s/Sleep/GetDiskFreeSpaceExW
medium   sigsetjmp/siglongjmp/sigjmp_buf (Interpreter.cpp:36,66,2532,18617; .hpp:3334)
         Fix: map to setjmp/longjmp; SIGSEGV recovery via AddVectoredExceptionHandler
medium   Time: localtime_r/tm_gmtoff/tm_zone/tzset/clock_gettime/gettimeofday
         Primitives.cpp:16298-16441,22485 ; InterpreterProxy.cpp:995,1104
         Fix: localtime_s (swapped args), GetDynamicTimeZoneInformation, GetProcessTimes
medium   Sockets/DNS: getaddrinfo without WSAStartup (Primitives.cpp:28555-28579)
         Fix: <winsock2.h>/<ws2tcpip.h>, link ws2_32, WSAStartup in platformInit
medium   SIGPROF/setitimer/sigaction profiler (Profiler.cpp:33-133)
         Fix: stub the file on _WIN32 (diagnostic-only, PHARO_PROFILE-gated)
large    Heap mmap/munmap/madvise with Linux lazy-commit (ObjectMemory.cpp:24-3827)
         Fix: VirtualAlloc(MEM_RESERVE)+incremental MEM_COMMIT; VirtualFree(MEM_DECOMMIT)
         for madvise; route through a platform seam
large    dlopen/dlsym/dlclose/RTLD_DEFAULT (FFI.cpp:265-390; Primitives.cpp:26218-26409;
         Interpreter.cpp:49) Fix: win_dl.h shim; .so/.dylib suffix logic -> .dll
large    Directory/file POSIX ops (opendir/readdir/mkdir-mode/ftruncate/fileno/lstat/
         access/stat macros) Primitives.cpp:12895-28172
         Fix: prefer std::filesystem; else _mkdir/_chsize_s/_fileno/_access/_stat64;
         S_ISSOCK/FIFO/LNK -> false; PATH_MAX -> MAX_PATH
minor    execinfo backtrace() diagnostics (Interpreter.cpp:38,14640,16609)
         Fix: RtlCaptureStackBackTrace+DbgHelp, or stub (debug-only)
```

### JIT (deferred to milestone 2 via JIT=0; listed for completeness)
```
small    Add WIN32 branch setting PHARO_JIT_ENABLED=0 (milestone-1 gate)
         JITConfig.hpp / Stencil.hpp:34
medium   Tier-1 helper calls lack Win64 shadow space (AsmjitT1.cpp:2199,2343 always-on)
large    Tier-1 hardcodes SysV arg registers (same two sites + 6 debug-gated)
medium   Regenerate generated_stencils_x86_64.hpp for x86_64-pc-windows-msvc
medium   extract_stencils.py needs a COFFParser (only if legacy stencil path used)
```

### SDL_EXPORT (major, milestone 4)
```
small    SDL_EXPORT uses ELF weak/visibility attrs (FFI.cpp:1384,1458,1463)
         Fix: __declspec(dllexport) or empty on Windows; accept no weak-override
```

## 5. New files and CMakeLists.txt changes (sketched)

New files:
- `src/platform/windows.cpp` — the ~9 seam functions plus headless stubs.
- `src/platform/win_dl.h` — dl-compat shim.
- `src/vm/WorldRenderer_windows_stub.cpp` (or rename
  `WorldRenderer_linux_stub.cpp` -> `WorldRenderer_headless_stub.cpp` and share).
- `src/vm/win_posix_compat.h` — POSIX header/macro shims (`localtime_r`,
  `S_IS*`, `PATH_MAX`, `usleep`, etc.).
- `scripts/sign-windows.ps1` — thin wrapper over the z80cpmw kit (milestone 5).

`src/platform/windows.cpp` sketch:
```cpp
#include <windows.h>
#include "Platform.hpp"
#include "DisplaySurface.hpp"
namespace pharo { DisplaySurface* gDisplaySurface = nullptr; }
extern "C" {
  const char* vm_getClipboardText() { return ""; }
  void vm_setClipboardText(const char*) {}
  void vm_startTextInput() {}
  void vm_stopTextInput() {}
}
namespace pharo::platform {
  void* allocCodeMemory(size_t n){ return VirtualAlloc(nullptr,n,
      MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE); }       // nullptr on failure
  void  freeCodeMemory(void* p,size_t){ if(p) VirtualFree(p,0,MEM_RELEASE); }
  int   currentWXShadowMode(){ return 1; }                     // RWX, like Linux
  bool  makeWritable(void*,size_t){ return true; }             // no-op
  bool  makeExecutable(void*,size_t){ return true; }           // no-op
  void  flushICache(void* p,size_t n){ FlushInstructionCache(GetCurrentProcess(),p,n); }
  bool  getStackBounds(uint8_t** top, uint8_t** bot){
      ULONG_PTR lo,hi; GetCurrentThreadStackLimits(&lo,&hi);
      *bot=(uint8_t*)lo; *top=(uint8_t*)hi; return true; }
  void  relinquishCPU(unsigned us){ if(us){ us<1000?SwitchToThread():(void)Sleep(us/1000); } }
  void  platformInit(){ WSADATA w; WSAStartup(MAKEWORD(2,2),&w); }
}
// Do NOT define flipJitToWritable/Executable (inline in Platform.hpp #else)
// Do NOT define jitTrampolineJMSize (comes from jit_jmsize_other.cpp)
```
Note `GetCurrentThreadStackLimits` needs `_WIN32_WINNT >= 0x0602`.

CMakeLists.txt platform branch sketch (after the Linux `elseif`):
```cmake
elseif(WIN32)
  if(MSVC)
    message(FATAL_ERROR "MSVC unsupported (computed gotos + GNU asm); use clang")
  endif()
  list(APPEND VM_SOURCES
    src/platform/windows.cpp
    src/vm/WorldRenderer_headless_stub.cpp   # shared with Linux
    src/vm/plugins/SoundPlugin.cpp
    src/vm/plugins/MIDIPlugin.cpp)
  # SocketPlugin.cpp deliberately excluded for milestone 1
  list(REMOVE_ITEM VM_SOURCES src/vm/plugins/SocketPlugin.cpp)
  target_link_libraries(PharoVMCore PRIVATE ws2_32 ffi)
  target_compile_definitions(PharoVMCore PRIVATE PHARO_JIT_ENABLED=0 _WIN32_WINNT=0x0602)
endif()
```
Also: gate `TrampolineAsm.S` into `VM_SOURCES` only `if(PHARO_TARGET_ARM64)` (it
is empty on x86-64); the ARM64 detection regex (`CMakeLists.txt:90-98`) already
hits the x86-64 else correctly for `"AMD64"`, so no milestone-1 impact.

Signing POST_BUILD (milestone 5), after the APPLE codesign block, still inside
the `NOT iOS` guard:
```cmake
if(WIN32 AND PHARO_SIGN_WINDOWS)
  add_custom_command(TARGET test_load_image POST_BUILD
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
      -File ${CMAKE_SOURCE_DIR}/scripts/sign-windows.ps1 $<TARGET_FILE:test_load_image>
    VERBATIM COMMENT "Authenticode-signing test_load_image.exe")
endif()
```

## 6. Open questions for the user

- Toolchain: clang-cl (MSVC ABI, MSVC STL, defines `_M_X64` not `__x86_64__`;
  native PE; almost no POSIX headers — must shim unistd/dirent/fcntl/pthread/
  sockets) vs LLVM-MinGW (defines `__x86_64__`; MinGW-w64 headers resolve
  unistd/dirent/fcntl/pthread/sockets for free; libstdc++/libc++).
  Recommendation: LLVM-MinGW for milestone 1 to minimize the header-shim
  surface; the JIT ABI rework in milestone 2 is identical either way (both
  target Win64).
- Milestone-1 scope: OK to exclude `SocketPlugin.cpp` (the single largest source
  effort) and set `PHARO_WITH_CRYPTO=OFF`, so milestone 1 needs only windows.cpp
  + dl/test shims + libffi + heap bridge.
- Heap commit policy on Windows: MEM_COMMIT the full ~4 GB old-space up-front
  (matches current always-readable assumption, charges 4 GB to commit/pagefile)
  vs incremental commit-on-grow + decommit-on-shrink (lower commit charge,
  touches ObjectMemory's bump-allocator grow path).
- SIGSEGV unrelocated-pointer recovery (`g_sigsegvRecovery`): reimplement via a
  Vectored Exception Handler on Windows, or drop it (image relocation should
  leave no stale pointers)? Profiler: stub acceptable for the port.
- libffi sourcing: vcpkg `unofficial-libffi`, MSYS2 pkg-config, or vendored
  prebuilt — which, so `FFI_DEFAULT_ABI==FFI_WIN64` is guaranteed.
- Signing (milestone 5): reuse the existing `z80cpmw-public` certificate profile
  (publisher identity `CN=Aaron Wohl` is identical, works immediately, but
  cross-brands the products and ties SmartScreen reputation/revocation together)
  vs provision a dedicated `iospharo-public` profile under the same
  `ms-code-sign-account`. Also confirm whether the Individual->Public Trust
  identity validation has been marked Completed in Azure.

---

## Milestone 1 — STATUS (2026-06-27): builds + runs, blocked on Delay timer

The Windows port went from "does not build at all" to a clean clang/LLVM-MinGW
build that loads and runs a standard Pharo 13 image. Toolchain: MSYS2 CLANG64
(install via `winget install MSYS2.MSYS2` then
`pacman -S mingw-w64-clang-x86_64-{toolchain,cmake,ninja,libffi,dlfcn,unzip}`).
Build: `scripts/build-windows.sh` (configures into `build-win/`, JIT off).

DONE:
- Clean build of `build-win/test_load_image.exe` (0 errors/0 warnings, JIT off).
- New `src/platform/windows.cpp` (W^X seam over VirtualAlloc RWX) +
  `win_compat.h` (sigjmp/backtrace) + `win_mman.h` (mmap over VirtualAlloc,
  MEM_RESET=MADV_DONTNEED) + `win_posix_compat.h` (getrusage/statvfs/uname/
  sysconf/setenv/localtime_r/arc4random_buf + cross-platform tm_gmtoff/tm_zone).
- WIN32 CMake branch; `SocketPlugin_win_stub.cpp` (honest failing stubs, sockets
  deferred to milestone 3); links libffi + ws2_32 + dl (dlfcn-win32).
- Loads a standard fresh Pharo 13 image (740k objects), resumes the snapshot,
  executes 38M+ bytecode steps of startup.
- FIXED: 4 "unrelocated pointer" heuristics hardcoded the [1TiB,2TiB) old-image
  window; Windows VirtualAlloc ASLR maps the new heap there ~50% of runs,
  false-positiving valid pointers. Now gated by `!memory_.isValidPointer()` —
  robust across ASLR (5/5 runs).
- FIXED: platform reported as "Mac OS" -> Pharo used Unix paths on Windows and
  could not find sibling files (.sources/startup.st). Now reports "Win32"
  (getSystemAttribute 1001 + primitiveGetPlatformName) -> WindowsResolver
  active, .sources found, startup runs WITHOUT the "UndefinedObject not
  indexable" error.

NEXT BLOCKER (the milestone-1 done-condition gate): the Delay/microsecond-timer
subsystem never arms on Windows. Symptom: a pri-80 process sits forever in
`DelayMicrosecondTicker>>waitForUserSignalled:orExpired:`; the
`[DIAG-TIMER]` line shows `timerSem=nil usecArmed=0 timerWasArmed=0
nextUsec=0x7fffffffffffffff`. The interpreter spins in the ProcessorScheduler
idle loop (steps keep advancing) because the delay never fires, so any startup
step using `Delay` hangs and full startup never completes (so the eval-mode
`startup.st` never fires and no EVAL-RESULT is produced).
  - The microsecond CLOCK is portable (`std::chrono::system_clock`,
    Primitives.cpp ~16259) — not the cause.
  - Investigate: `Interpreter::checkTimerSemaphore()` (Interpreter.cpp ~4309),
    how `timerSemaphore_` / `nextWakeupUsec_` get set from
    `primitiveSignalAtUTCMicroseconds` (prim 242, Primitives.cpp 16224) and the
    `SpecialObjectIndex::TheTimerSemaphore` special object (Interpreter.cpp
    ~1203), and the heartbeat thread's role in signaling. Determine why the
    image never arms the timer on Windows (prim 242 failing? ticker not calling
    it? VM not signaling TheTimerSemaphore?).
  - Repro: `MSYSTEM=CLANG64 /c/msys64/usr/bin/bash.exe -lc 'timeout 60
    /c/temp/src/iospharo-jit/build-win/test_load_image.exe
    "C:/temp/pharo-win-test/Pharo.image" eval "3+4"'` then look for
    `[DIAG-TIMER]` / `EVAL-RESULT`.

After the timer fix, the eval should print `EVAL-RESULT=...`; then wire the
SUnit runner (needs `scripts/pharo-headless-test` submodule init + the reference
Pharo Windows VM to inject `run_sunit_tests.st`) for the pass-count comparison.
