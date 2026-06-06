# Porting `test_load_image` to Linux/arm64 (AWS Graviton) — Scoping Report

Date: 2026-06-06. Produced by a 5-dimension parallel scoping sweep (build
system, JIT W^X/codesign, Darwin syscalls, FFI/display, libc) verified against
the tree. Companion to `docs/linux-port-status.md` (the x86_64 Linux port).

## 1. Verdict

**Feasible — and already ~90% done.** A headless `test_load_image` build for
Linux/arm64 is not a "port" in the usual sense; it is the existing macOS-arm64
source set minus Apple frameworks, running through a Linux CMake branch that
**already exists and already produced a working x86_64-Linux ELF**
(`docs/linux-port-status.md`, jit-x86 branch).

**Total effort: S (small).** The hard work — the `pharo::platform` abstraction,
`src/platform/linux.cpp`, the `CMAKE_SYSTEM_NAME STREQUAL "Linux"` branch,
`scripts/build-linux.sh`, the libstdc++ `getenv` poison fix, the ELF-vs-Mach-O
symbol decoration in the asm trampoline — is all committed. arm64 Linux is
strictly **easier than the x86_64 Linux build already done**: it reuses the
*mature* arm64 JIT lowerer (`src/vm/jit/sista/SistaLowering_arm64.cpp`,
`src/vm/jit/Tier2Compiler_arm64.cpp`, `src/platform/jit_jmsize_arm64.cpp`,
hand-written `TrampolineAsm.S`) instead of the x86_64 stubs, because Graviton
shares the arm64 ISA, LP64 word size, little-endianness, and AAPCS64 ABI with
the current macOS-arm64 host. Most expected effort is **environmental** (install
apt packages, run the script), not source changes.

There are **zero hard blockers** in the strict sense (nothing prevents
compile+link+run headless). Section 2 lists the items that *could* surface on a
real first build, ordered by likelihood of needing a touch.

## 2. Items that might need a touch (ordered)

Every Apple-coupled item in all five dimensions is already `#ifdef __APPLE__` /
`if(APPLE)`-guarded with a working Linux fallback. The honest list of "things
that might actually require action on a fresh Graviton box" is short and mostly
environmental:

1. **Install system packages (the only true prerequisite).**
   - `apt install build-essential cmake pkg-config libffi-dev` — `libffi` is the
     one `pkg_check_modules(... REQUIRED ...)` at CMakeLists.txt:348; a missing
     `libffi-dev` fails configure.
   - `libsdl2-dev` is optional (CMake logs "building without GUI support" if
     absent — fine for headless). bootstrap.sh already installs these.

2. **Confirm the kernel permits `PROT_EXEC` on anonymous `mmap` (JIT).**
   `src/platform/linux.cpp:46-52` does one `mmap(PROT_READ|WRITE|EXEC,
   MAP_PRIVATE|MAP_ANONYMOUS)` (RWX once, no `MAP_JIT`, no per-flip MSR toggle).
   Stock Ubuntu 24.04 allows this for an unprivileged process. Only an
   SELinux/PaX/W^X-enforcing hardened kernel would block it — not the case on a
   standard Graviton image. No entitlement, no codesigning needed (Linux JIT is
   *simpler* than macOS — the ~55%-CPU W^X-flip overhead disappears).

   The above two are the only items that gate a successful headless run.
   Everything below is optional polish that does not block build, link, or SUnit
   correctness:

3. *(optional, small)* **FFI callback force-export for FFI-callback SUnit
   tests.** The `-Wl,-u,_sym` block (CMakeLists.txt:447-471) is Mach-O syntax
   inside `if(APPLE)`. Most SUnit runs don't exercise FFI callbacks. If they do,
   add a Linux branch using GNU ld `--undefined=symbol` (no leading underscore)
   or simply `-rdynamic`. The comment at CMakeLists.txt:443-446 already notes it.

4. *(optional, small)* **FFI bare-name library resolution defaults to `.dylib`.**
   `FFI.cpp:324-327` appends `.dylib`/`lib<name>.dylib` candidates for bare
   module names; on Linux these `dlopen` calls fail. Headless FFI tests mostly
   use the statically force-loaded testlib via `dlsym(RTLD_DEFAULT)` + full-path
   `dlopen`, so they pass anyway. Add `.so`/`lib<name>.so` candidates under
   `#ifdef __linux__` to close the by-name gap.

5. *(optional, medium)* **Rich SIGSEGV crash diagnostics.** The register/PC dump
   + Character-immediate fault-skip recovery in `test_load_image.cpp` is gated
   `#if defined(__APPLE__) && defined(__arm64__)` and reads Apple's
   `__ss.__pc/__x[]` mcontext. On Linux the handler degrades to a fault-addr
   `fprintf` + `backtrace()` + `_exit(139)` — it still installs and runs, just
   without the JIT register dump. To restore it for live JIT debugging, add a
   `#elif defined(__linux__) && defined(__aarch64__)` branch reading glibc
   `uc->uc_mcontext.pc/.regs[]/.sp` (`<asm/sigcontext.h>`). The LDR-decode mask
   logic is ISA, not OS, and works verbatim. Needed for crash-debug fidelity,
   not for running SUnit.

6. *(optional, trivial)* **`primitiveExitToDebugger` trap.** `Primitives.cpp:4621`
   gates `__builtin_debugtrap()` on `__APPLE__ && __arm64__`; arm64-Linux falls
   to `std::abort()` (functionally fine). Broaden to `defined(__aarch64__)`.

7. *(optional, trivial)* **`SIGPIPE` on socket writes.** `SocketPlugin.cpp` sets
   no `SO_NOSIGPIPE`/`MSG_NOSIGNAL`; a peer-closed write could `SIGPIPE`-kill the
   process during socket tests on Linux. Install `SIG_IGN` for `SIGPIPE` at
   startup or pass `MSG_NOSIGNAL`. Runtime robustness, not a build issue.

8. *(cosmetic, trivial)* **`TARGET_OS_*` macros leak into Linux `#if`.**
   `FFI.cpp:296,368` reference `TARGET_OS_IPHONE`/`TARGET_OS_IOS`, undefined on
   Linux so they evaluate to 0 — compiles benignly, just pushes dead Homebrew
   search paths. Wrap in `#ifdef __APPLE__` if cleaning up.

## 3. Already portable (verified present)

- **CMake Linux branch** (CMakeLists.txt:178-194, 344-362): adds `linux.cpp` +
  `WorldRenderer_linux_stub.cpp` + Sound/MIDI `#else` stubs; links
  `pthread/dl/m` + libffi/SDL2 via pkg-config; drops OBJC and all Apple
  frameworks.
- **arm64 JIT source selection** (CMakeLists.txt:71-84, 206-209, 233-235):
  `CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64"` picks the mature
  `SistaLowering_arm64.cpp`, `Tier2Compiler_arm64.cpp`, `jit_jmsize_arm64.cpp`
  and enables `PHARO_ASM_TRAMPOLINE=1`.
- **`test_load_image` target** (CMakeLists.txt:440-442): `add_executable` + link
  `PharoVMCore`, gated only `NOT iOS`. Links the in-tree static lib directly —
  never touches the xcframework / `build-xcframework.sh` (Catalyst-GUI-only).
- **Platform JIT W^X** (`Platform.hpp`, `PlatformJIT.hpp`, `linux.cpp`): single
  chokepoint; Linux backend complete (RWX mmap, no-op flips, clear_cache).
- **asmjit tier-2** (`third_party/asmjit/.../virtmem.cpp`): cross-platform;
  `MAP_JIT`/`pthread_jit_write_protect_np` Apple-arm64-gated; `__GNUC__` icache
  branch for Linux. The only local asmjit patch is Catalyst-only / no-op on Linux.
- **Image loading**: `std::ifstream` into the pre-mmap'd heap; native-LE only
  (arm64-LE == arm64-LE).
- **Heap**: `mmap(MAP_ANONYMOUS|MAP_PRIVATE)`, no `MAP_FIXED`/`MAP_JIT`.
- **Timing**: `std::chrono` / `gettimeofday` / `clock_gettime`; `readTSC`
  arch-branches to `mrs cntvct_el0` on `__aarch64__` (ObjectMemory.cpp:2021).
- **Signals/threads**: `sigaction`, `setitimer(ITIMER_PROF)`, `std::thread`
  heartbeat — pure POSIX.
- **`TrampolineAsm.S`**: `SYMBOL()` macro handles ELF (bare) vs Mach-O
  (`_`-prefixed); portable GAS, AAPCS64. Assembles unchanged on Graviton — the
  big arm64-Linux advantage over the x86_64-Linux build.
- **Exec path**: `_NSGetExecutablePath` (Apple) / `readlink(/proc/self/exe)`
  (Linux) already dual-pathed.
- **`st_birthtime`, CF locale prims, Unicode NFC/NFD, FFI ObjC exception guard,
  `getPlatformName`→"linux", `no_getenv.hpp` libstdc++ fix**: all have Linux
  `#else` branches.
- **Crypto** (`sqMacSSL.c`/`DSAPrims.c`/`SqueakSSL.c` + Security.framework):
  disabled via `-DPHARO_WITH_CRYPTO=OFF` in `build-linux.sh` — never compiles.

## 4. `#ifdef` strategy / minimal Platform shim list

**Keep the existing two-axis split — do NOT add a third.**

- **OS axis** (`if(APPLE)` in CMake; `#ifdef __APPLE__` in source): selects
  Apple-framework TUs vs Linux stub TUs. All current Apple coupling lives here;
  the Linux/arm64 build rides the existing `else`/`__linux__` branches.
- **Arch axis** (`CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64"`;
  `#if defined(__aarch64__)` in source — GCC/Clang spell it `__aarch64__`, NOT
  Apple's `__arm64__`): selects the JIT lowerer/trampoline/jmsize files.
  Graviton takes the same arm64 path macOS-arm64 already uses.

**The trap to avoid: `__arm64__` is Apple-only.** Any new arch-conditional code
on Linux must test `__aarch64__`. The few existing `__APPLE__ && __arm64__`
guards (crash handler `test_load_image.cpp:485-545`, debug trap
`Primitives.cpp:4621`) are exactly the ones that silently don't match on
Graviton — that's why the rich diagnostics degrade (items 5-6).

**Minimal shim list (all already exist; none need new files for headless):**

```
  Shim TU / symbol                 Status        What it provides on Linux
  -------------------------------  ------------  --------------------------------------
  src/platform/linux.cpp           DONE          W^X JIT mmap(RWX), no-op flips,
                                                 __builtin___clear_cache, stack bounds
                                                 (pthread_getattr_np), relinquishCPU
                                                 (usleep), gDisplaySurface=nullptr,
                                                 vm_* clipboard/text-input stubs
  WorldRenderer_linux_stub.cpp     DONE          no-op WorldRenderer (linker satisfied)
  SoundPlugin.cpp  (#else stub)    DONE          audio prims return failure
  MIDIPlugin.cpp   (#else stub)    DONE          MIDI prims return failure
  src/platform/jit_jmsize_arm64    DONE (reuse)  arm64 JIT method-size calc
  TrampolineAsm.S  (SYMBOL macro)  DONE (reuse)  ELF symbol decoration
  no_getenv.hpp    (cstdlib first) DONE          libstdc++ `using ::getenv;` compat
```

Optional new shim code (items 3-7) slots into the existing OS/arch branches — no
new shim TU required.

## 5. First-build plan (fresh Ubuntu 24.04 arm64 / Graviton)

```
# 0. Provision the box:
#    CONFIG_FILE=scripts/aws/config-arm.env ./scripts/aws/provision.sh
#    (c7g.16xlarge default; spot ~$0.62/hr. Quota increases to 128 vCPU pending.)

# 1. Toolchain + deps (the ONLY hard prerequisite; bootstrap.sh does this).
sudo apt update
sudo apt install -y build-essential cmake pkg-config libffi-dev git
# sudo apt install -y libsdl2-dev   # OPTIONAL — headless does not need it

# 2. Clone + submodules (asmjit + pharo-headless-test). Work branch: jit-arm-linux.
git clone --recurse-submodules <repo-url> iospharo && cd iospharo

# 3. Sanity-check pkg-config sees libffi (configure FAILs without it).
pkg-config --exists libffi && echo "libffi OK: $(pkg-config --modversion libffi)"

# 4. Build headless (crypto OFF, JIT on; LTO off for a fast first build).
PHARO_DISABLE_LTO=1 ./scripts/build-linux.sh
#    Expect: "J2J trampoline: ARM64 hand-written", "Using libffi from
#    pkg-config", and (no SDL2) "building without GUI support".
#    Output: build/test_load_image

# 5. Smoke-test against a fresh standard Pharo image.
cd /tmp && mkdir -p harness && cd harness && \
  curl -sL https://get.pharo.org/64/130+vm | bash
~/iospharo/build/test_load_image /tmp/harness/Pharo.image
#    (If JIT RWX mmap is refused you'll see an alloc failure here — blocker #2.)

# 6. Run the SUnit suite to validate correctness on Graviton.
cp ~/iospharo/scripts/pharo-headless-test/test_classes.txt /tmp/sunit_test_classes.txt
/tmp/harness/pharo /tmp/harness/Pharo.image eval --save \
  "'$HOME/iospharo/scripts/pharo-headless-test/run_sunit_tests.st' asFileReference fileIn"
rm -f /tmp/sunit_run_completed.txt
~/iospharo/build/test_load_image /tmp/harness/Pharo.image
cat /tmp/sunit_test_results.txt
#    An arm64-Linux result should track the macOS-arm64 baseline closely
#    (same lowerer, ABI, byte order).

# 7. If FFI-callback tests fail to resolve symbols (item #3), add a Linux
#    force-export branch at CMakeLists.txt:447 (--undefined=SYM / -rdynamic).

# 8. (Optional) Re-enable LTO for the production build once green:
./scripts/build-linux.sh
```

## Key paths

build driver `scripts/build-linux.sh`; Linux platform shim
`src/platform/linux.cpp`; CMake Linux/arch branches `CMakeLists.txt` (71-84,
178-194, 206-209, 233-235, 344-362, 440-491); prior-art `docs/linux-port-status.md`;
arm64 JIT `src/vm/jit/sista/SistaLowering_arm64.cpp`,
`src/vm/jit/Tier2Compiler_arm64.cpp`, `src/platform/jit_jmsize_arm64.cpp`,
`src/vm/jit/TrampolineAsm.S`; crash-handler arm64-Linux gap
`src/vm/test_load_image.cpp` (485-545).
