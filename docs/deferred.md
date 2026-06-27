# Deferred / not-100% work

Consolidated list of things that are NOT at full parity with the other
platforms (macOS / Linux), including deferred features, workarounds, honest
platform stubs, and known gaps. Updated as the Windows port progresses.

## Windows (clang / LLVM-MinGW) — port status 2026-06-27

Working: headless interpreter builds and correctly evaluates Smalltalk on a
standard Pharo 13 image (see docs/windows-port-plan.md). The items below are the
gaps vs macOS/Linux.

### JIT
- [x] **JIT enabled on Windows** — DONE. The x86-64 Tier-1 (`AsmjitT1.cpp`) now
  emits Win64-correct helper calls (arg-register aliases kCArg0..kCArg4 +
  `emitCallCHelper_x86` shadow-space bracket); SysV emit is unchanged. JIT is on
  by default for the WIN32 build. Verified: compiles 3827 methods, runs 879M
  bytecodes, evals correct ((3+4)*6, 100 factorial, inject:, fractions).
  Tier-2/Sista needed no change (asmjit high-level Compiler derives the ABI).
  - CAVEAT: 6 debug/verify-gated helper sites (default-OFF flags
    PHARO_T1_J2J_DBG, the verify-getter/spec knobs) were fixed by reasoning
    (Win64 5-arg stack layout) but NOT runtime-tested — they don't fire in
    normal operation. Runtime-test before relying on those knobs on Windows.

### Networking / TLS
- [ ] **SocketPlugin** — replaced by `SocketPlugin_win_stub.cpp` on Windows:
  every socket primitive fails (primitiveFail) and `hasSocketAccess` reports
  false. No TCP/UDP networking. Needs a winsock2 port (milestone 3). NOTE: the
  core DNS path (getaddrinfo in Primitives.cpp) IS wired (ws2_32 + WSAStartup).
- [ ] **Crypto / SqueakSSL** — `PHARO_WITH_CRYPTO=OFF` on Windows; no OpenSSL
  link, so HTTPS/TLS is unavailable. Needs vcpkg/MSYS2 OpenSSL (milestone 3).

### GUI
- [ ] **SDL2 / Morphic display** — headless only. No window/rendering surface;
  `WorldRenderer_linux_stub.cpp` (no-op) is reused. The image runs the Morphic
  World loop but draws nothing. Needs an SDL2 + Windows rendering surface
  (milestone 4).

### Diagnostics / platform features (honest stubs)
- [ ] **Sampling profiler** (`Profiler.cpp`) — `enable()` is a no-op on Windows
  (POSIX SIGPROF/setitimer). `PHARO_PROFILE=1` prints "not supported on Windows".
- [ ] **SIGSEGV crash recovery** (`test_load_image.cpp`) — the POSIX
  `sigaction` SIGSEGV/BUS/ILL handler + `g_sigsegvRecovery` longjmp recovery is
  compiled out on Windows; a fault crashes with the default Windows behavior
  instead of the diagnostic dump / Character-deref recovery. Could add a
  Vectored Exception Handler later.
- [ ] **execinfo backtrace** (`win_compat.h`) — `backtrace()`/
  `backtrace_symbols()` are no-op stubs; crash/DNU dumps print no native frames.
  Could use RtlCaptureStackBackTrace + DbgHelp.
- [ ] **Symlinks** — `win_posix_compat.h` maps `lstat`->`stat`, `S_ISLNK`->0,
  `readlink`->EINVAL; the directory-attributes primitive (`fstatat`) does a
  full-path `stat` (follows links). So symlinks are treated as regular files —
  no symlink detection/target resolution. Acceptable for milestone 1.
- [ ] **POSIX file ownership** — `chown`/`lchown` return ENOSYS (Windows has no
  POSIX uid/gid model); the two calling primitives turn that into a primitive
  Failure, which the image handles.

### Memory
- [ ] **Old-space heap commit** — `win_mman.h` `mmap` does
  `MEM_RESERVE|MEM_COMMIT` for the whole (~4 GB) reservation up front, charging
  it all to the Windows commit limit (pagefile) — though demand-zero means no
  physical RAM until touched. macOS/Linux rely on lazy overcommit. A
  fault-driven incremental-commit handler would lower the commit charge (the
  config requests 4 GB; if a machine's commit limit is tight this could fail
  allocation). `madvise(MADV_DONTNEED)` -> `MEM_RESET` (RSS hint) is a faithful
  analogue.

### RNG
- [ ] **arc4random_buf** (`win_posix_compat.h`) — implemented via
  `std::random_device` (OS CSPRNG on LLVM-MinGW). Adequate for UUIDs; revisit if
  a hardened CSPRNG is required.

### Packaging
- [ ] **Authenticode signing** — not wired yet (milestone 5). z80cpmw's Azure
  Trusted Signing kit can sign `test_load_image.exe` (jsign cross-platform or
  signtool+dlib). Open: reuse `z80cpmw-public` cert vs a dedicated
  `iospharo-public` profile.

### Run-environment caveat (not a bug)
- The exe MUST run from a NATIVE Windows shell. Launching via the MSYS2 *login*
  shell (`bash -lc`) strips USERPROFILE/APPDATA and sets TEMP=/tmp, which breaks
  Pharo's WindowsResolver (home/preferences -> "Can't find the requested
  origin"). CMake copies the runtime DLLs next to the exe so it is
  self-contained.
