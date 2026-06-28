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

### SUnit parity (JIT on, vs Linux/macOS 99.1% baseline)

**Windows-sensitive catalog (2026-06-27).** After the file subsystem reached
100%, ran all 245 Windows-sensitive TestCase subclasses (file/OS/process/time/
path/FFI/socket) out of the full 2047 to map the remaining gaps. Buckets, each
detailed in its section below: file subsystem = 100% (long-path + isHidden +
FILE_SHARE_DELETE + logical-drives all fixed); FFI ABI fixed (+11), FFI struct
tests need the TestLibrary.dll fixture; platform-path UTF-16 fixed (+2),
nLink/permissions niche-deferred; sockets/HTTP/TLS (~95 tests) await the
winsock2 milestone and fail-fast (no hangs); 8 GUI/headless presenter+debugger
tests HANG (GUI milestone); MicFileResourceReferenceTest (16) hits a BitBlt
"Fraction" graphics bug. Net: every non-green Windows item is now either fixed
or categorized below with a root cause.

Validated ~3800 Kernel/Collections/Exceptions/Context/Stream/Reflection tests
on Windows with the JIT enabled, 0 failures / 0 errors (run via `eval "(X suite
run) printString"` on a fresh Pharo 13 image). Batches: numbers+collections
3150/3150; exceptions+contexts+streams+reflection 461/461; Semaphore 18/18,
Mutex 7/7, Delay 5/5, ProcessSpecific 8/8. Two test classes hang when run in
isolation (no suite watchdog):
- [ ] **ProcessTest>>testResumeAfterBCR** spins (billions of bytecodes) — resume
  after BlockCannotReturn. Spins with the JIT OFF too, so it is NOT the Win64
  JIT fix; it's a pre-existing Windows interpreter/process-termination edge
  case (the BCR sentinel fix from Build 80, executeFromContext, is
  platform-independent C++, so this needs investigation on Windows). Low
  priority (single niche test).
- [ ] **WeakArrayTest** hangs — KNOWN nondeterministic weak-ref/GC-timing test
  per debug_vars.h:260 ("only nondeterministic weak-ref/GC-timing tests differ"
  under the x86 JIT, same on arm64-JIT). Not a Windows-specific regression.
- [x] **Full suite on Windows — RUNS** (recipe below). 2047 non-abstract
  TestCase subclasses.
  - RESULT (run #1, JIT on, ~45 min outer cap): got through the first **111
    classes** (A*..Ci*) before a Morphic GUI test (`CircleMorphTest`) hung past
    the per-test watchdog and the outer timeout killed the run. Of those 111:
    **1477 PASS, 0 FAIL, 42 ERROR, 3 SKIP = 97.2%**. The 42 errors are
    EXPECTED-category: 27 are Athens/**Cairo** graphics (FFI to the Cairo lib —
    GUI/graphics gap) and 15 are `BinaryFileStreamTest` (binary file I/O — the
    one NON-gui item to investigate; possible Windows binary-mode issue). 0
    real test FAILURES.
  - RESULT (run #3, JIT on, after the sqInt LLP64 fix, curated 1400 NON-GUI):
    NO CRASH (exit 0; the CoCompletionEngine SIGSEGV is gone). Got through 152
    classes = **1961 PASS, 0 FAIL, 15 ERROR, 1 SKIP = 99.2% pass — at/above the
    Linux 99.1% baseline, ZERO failures.** Errors: 15 `BinaryFileStreamTest` + 1
    `CodeSimulationTest`. It stopped at 152 (not all 1400) on the ~50-min outer
    timeout — the per-test-watchdog runner is SLOW (~20s/class: fork a watchdog
    process + wait, per test). To run all 1400 either raise the timeout (hours)
    or speed up the watchdog. The pass RATE is the parity signal; it's solid.
  - [x] `BinaryFileStreamTest` — FIXED (29 errors -> 15/15 pass). Root cause:
    `primitiveFileOpen` used plain `fopen`, which on Windows omits
    FILE_SHARE_DELETE, so a file held open by a stream could not be deleted
    (`CannotDeleteFileException: ... Check the file is not open`); POSIX allows
    delete-while-open. Fixed: `pharoSharedFopen` opens via CreateFileA with
    FILE_SHARE_READ|WRITE|DELETE (+ _open_osfhandle/_fdopen) on Windows; plain
    fopen elsewhere. (The earlier "passes individually" read was wrong —
    TestCase>>run swallows the error into the TestResult; `debug` revealed it.)
  - [x] **`isHidden` / file-attributes slot** — FIXED. `WindowsStore>>isHidden:`
    reads `(statAttributes at: 13) anyMask: 16r2` (FILE_ATTRIBUTE_HIDDEN), but
    the VM's stat array had only 12 slots -> SubscriptOutOfBounds:13. Added slot
    13 = the Windows file-attributes DWORD (GetFileAttributesA, fallback
    FILE_ATTRIBUTE_NORMAL on INVALID) to both `primitiveFileAttributes` and
    `primitiveReaddir` (Windows-only growth; POSIX arrays unchanged). Verified
    testIsHiddenWithRealFilesystem passes; file/stream classes 1112/1113.
  - [x] `StFileFilterTest`/`StNavigationSystemTest` hidden tests — FIXED. The
    real cause wasn't hidden-file creation: `File class>>primLogicalDrives`
    (`primitiveLogicalDrives`, FileAttributesPlugin) was unimplemented, so
    `WindowsStore>>directoryAt:nodesDo:` (lists drive letters as browser roots)
    -> PrimitiveFailed -> signalError:for: in setUp. Implemented it to return the
    Win32 GetLogicalDrives() bitmask. All hidden tests now 8 pass / 1 skip / 0 err.
  - [x] `DiskFileSystemTest>>testLongFilename` — FIXED. Windows `MAX_PATH` (260)
    long-path support. The test builds a ~284-char path (130-char dir inside a
    130-char dir, then `hello.txt`); `LongPathsEnabled` is 0 on this machine so a
    manifest opt-in is insufficient. Implemented the `\\?\` extended-length
    prefix via a `winLongPath(path)` helper (Primitives.cpp): for an absolute
    drive path >= 248 chars it canonicalizes slashes and prepends `\\?\`
    (`\\?\UNC\` for UNC). Crucially it does NOT route absolute long paths through
    `GetFullPathNameA` — the ANSI form is itself MAX_PATH-limited and fails on a
    long input. Applied at every file syscall site (no-op on POSIX / short
    paths): `_mkdir`, `CreateFileA` (pharoSharedFopen), `remove`, `rmdir`,
    `rename`, `stat`/`lstat` (exists/attributes/lookup/readdir),
    `GetFileAttributesA` (winFileAttributes). Directory ENUMERATION needed more:
    `opendir`/`readdir` go through the ANSI `FindFirstFileA`, which cannot open a
    `\\?\` path, so long dirs listed as empty (cleanup then failed with
    `DirectoryIsNotEmpty`). Added a wide-API enumerator `winListDir`
    (FindFirstFileW/FindNextFileW) and routed both enumeration primitives through
    it on Windows: `primitiveDirectoryLookup`, and `primitiveOpendir`/`Readdir`/
    `Closedir`/`Rewinddir` now hand out a pre-enumerated `WinDirIter*` instead of
    a libc `DIR*`. Verified: testLongFilename PASS via canonical `TestCase>>run`;
    `DiskFileSystemTest` 59/59; FileSystemTest 126/126, FileReferenceTest 112/112,
    FileLocatorTest 38/38, PathTest 76/76 — no regression. (The 6
    `FileAttributesPluginPrimsTest` error-fidelity failures are PRE-EXISTING —
    confirmed identical on a stash-rebuilt baseline; they want `IllegalFileName`/
    `#'bad argument'` exception selectors our VM doesn't yet raise, unrelated to
    long paths. Tracked separately below.)
  - RESULT (run #2, JIT on, before the sqInt fix): ~130 classes, 1702 PASS / 0
    FAIL / 15 ERROR = 99.1%, then crashed exit 139 at `CoCompletionEngineTest` —
    NOW FIXED
    (root cause was the sqInt LLP64 truncation, below). Re-running the suite
    with the fix should get far past this point.
    RESOLVED — root cause = **`sqInt` LLP64 truncation**. Added a Windows
    crash-dump Vectored Exception Handler (test_load_image.cpp); the fault
    address was a sign-extended 32-bit value and addr2line put it in
    `proxy_isBytes(long)` / `ObjectHeader::isBytesObject`. `sqMemoryAccess.h`
    typedef'd `sqInt`/`usqInt`/`sqLong`/`sqIntptr_t` to `long`, which is 64-bit
    on LP64 (Linux/macOS/ARM64) but only 32-bit on Windows (LLP64). sqInt holds
    oops/pointers throughout the plugin/InterpreterProxy interface, so every oop
    above 4 GB (heap at ~2.7 TiB) truncated to 32 bits + sign-extended into a
    garbage pointer -> corruption. Fixed: typedef to intptr_t/int64_t
    (byte-identical on LP64; 64-bit on Windows). Verified CoCompletionEngineTest
    65/65 (was a hard SIGSEGV), 996/996 sanity, no regression. This systemic fix
    likely clears other latent Windows oop-truncation failures too.
    (The old note kept for history: it was NOT the disabled SIGSEGV recovery and
    NOT the MADV_DONTNEED/MEM_RESET shim — though that shim was a real latent bug
    also fixed. SIGSEGV recovery wouldn't have cleanly fixed a corruption; it
    would only have let the run skip the crashing test.)
  WORKING RECIPE:
  1. `mkdir C:\tmp` (the runner writes there; "/tmp" resolves to C:\tmp).
  2. Download the reference Pharo Windows VM: `curl -sL https://get.pharo.org/64/vm130 | bash` (gives `pharo-vm/PharoConsole.exe`).
  3. Copy the image, then inject the runner with the REFERENCE VM (its compiler
     handles the huge runAllTests; ours can't — see convertStorePop below) —
     run from a NATIVE shell (USERPROFILE set), NOT the MSYS2 login shell:
     `./PharoConsole.exe Pharo-sunit.image eval --save "'<repo>/scripts/pharo-headless-test/run_sunit_tests.st' asFileReference fileIn"`
  4. Run the prepped image with OUR VM, invoking the now-PRE-COMPILED runAllTests
     directly (bypasses the startUp-hook handoff, which didn't drive it):
     `build-win/test_load_image.exe Pharo-sunit.image eval "(Smalltalk at: #SUnitRunner) runAllTests"`
     -> writes C:\tmp\sunit_test_results.txt / sunit_test_detail.txt.
  Gotchas hit along the way: the exe needs its 5 runtime DLLs present (AV may
  quarantine the .exe — rebuild if missing); run everything from a native shell.

- [ ] **VM can't compile very large methods** (convertStorePop) — surfaced by
  the above: OUR VM's OpalCompiler errors compiling the runner's ~380-line
  `runAllTests` (`OCIRSimpleOptimizerVisitor>>convertStorePop:` ->
  `OCIRSequence>>remove:` element-not-found -> `Error: 'Error!'`). Fails with the
  JIT OFF too (interpreter-level), and is almost certainly cross-platform (the
  Linux/Mac flow injects via the reference VM, so our VM never compiles it
  there). Worth fixing so our VM can compile large methods directly. Earlier
  ~3800-test validation was unaffected (those methods were already compiled in
  the image).
  - PARTIAL RESULT (synchronous batches via `scripts/win-sunit-batches.sh`):
    the 3 batches that finished within the 200s/batch timeout = 2709/2882
    passed; representative batch-1 (first 100 classes) = 1433/1495 (96%), 1
    failure, 61 errors. Most "TIMEOUT" batches were NOT real hangs — 100-class
    batches simply need >200s (image load + JIT + thousands of tests), so the
    fixed-timeout synchronous approach undercounts badly. Batch-21's 101 errors
    are mostly `Zn*` (Zinc networking) — EXPECTED, sockets are stubbed.
  - `/tmp/...` paths DO resolve on Windows (image WindowsStore maps "/tmp" to
    `C:\tmp` on the current drive — create `C:\tmp`).
  - **BLOCKER — the pharo-headless-test watchdog runner errors on Windows.**
    `run_sunit_tests.st` `SUnitRunner runAllTests` runs synchronously (per-test
    forked watchdog + timeout) and calls `Smalltalk exitSuccess` itself at the
    end, but on Windows it throws an opaque `Error: Error: Error!` (nested error
    in its own error handler) after ~1.9B bytecodes — so no results file is
    written. ROOT CAUSE FOUND (bisected to one chunk + captured the real stack):
    the runner's `fileIn` fails while **compiling the huge `runAllTests`
    method** (chunk 13, ~380 lines). The real error is
    `OCIRSimpleOptimizerVisitor>>convertStorePop:forInstructionSequence:` ->
    `OCIRSequence>>remove:` -> `OrderedCollection>>remove:ifAbsent:` (element not
    found) -> `Error: 'Error!'`. i.e. OUR VM's OpalCompiler IR optimizer mis-
    handles this large method. Fails with the JIT OFF too, so it is an
    INTERPRETER-level compiler bug, almost certainly NOT Windows-specific —
    the Linux/macOS workflow never hits it because it injects the runner with
    the **reference Pharo VM** (`pharo image eval --save "... fileIn"`), so our
    VM only ever RUNS the pre-compiled methods, never compiles them.
    IMPLICATION FOR PARITY: this is NOT a Windows-JIT parity gap — the JIT is at
    parity (validated). It's (a) a pre-existing cross-platform limitation of our
    VM's compiler on very large methods, and (b) a Windows tooling gap: to run
    the full suite we likewise need reference-VM injection. The reference Pharo
    Windows VM IS downloaded (`pharo-win-test/refvm/pharo-vm/PharoConsole.exe`),
    but `eval --save` injection on Windows hit file-locking friction (lingering
    PharoConsole holds the image/.changes). Next: get one clean reference-VM
    `eval --save "<runner> fileIn"` (kill stray PharoConsole first; use the LF
    runner copy), then run the prepped image with OUR VM (delete the
    `/tmp/sunit_run_completed.txt` marker so the startUp hook fires) -> the
    per-test-watchdog full-suite number. Separately worth fixing the
    convertStorePop compiler bug so our VM can compile large methods directly. Even the runner's `runDiagnostic:` (a minimal fork +
    on:ZeroDivide:do: + `sem waitTimeoutSeconds:` sanity check) does NOT complete
    on Windows — so the likely root cause is the forked-process + semaphore-
    timeout mechanism the watchdog is built on, the SAME area as the ProcessTest
    hang. Fixing fork/process-timeout semantics on Windows would unblock BOTH the
    runner (-> full-suite number) and ProcessTest>>testResumeAfterBCR. Also
    `Smalltalk saveAs:` (to prep a runner-installed image) errors on our VM.
    Until this is fixed, the full per-test-watchdog number vs the 99.1% Linux
    baseline isn't obtainable. The 61 batch-1 errors and 5 batch-21 failures
    also want characterization (Windows-specific vs missing-feature like
    sockets/GUI).

### Primitive error-signal fidelity (cross-platform, pre-existing)
- [ ] **`FileAttributesPluginPrimsTest`** — 6 tests (run=6 pass=0 fail=5 err=1),
  confirmed IDENTICAL on a pre-long-path baseline (stash + rebuild), so NOT a
  regression. They assert exact VM-raised exceptions our clean VM doesn't yet
  produce: `primExists: nil` / `primClosedir: nil|aString|wrongLenByteArray`
  expect `PrimitiveFailed` with `exception selector == #'bad argument'`;
  `fileAttribute:number:` out of range expects `PrimitiveFailed`; and
  `exists:` on a path of `primPathMax * 2` expects `IllegalFileName`. Our
  primitives return a generic failure instead of the named-primitive
  `#'bad argument'` error selector / `IllegalFileName`. Fix is an error-signal
  fidelity pass in the named-primitive failure path, not Windows-specific.

### FFI (libffi / TFFI)
- [x] **FFI ABI resolution** — FIXED. `FFICalloutMethodBuilderTest` (10/10, was
  0/10) and `FFIFunctionParserTest` (45/45, was 44/45) failed with `Error: The
  requested ABI is not available for this architecture: #(#Win32
  #StackInterpreter #cdecl)`. Pharo's TFFI builds its libffi ABI-lookup tuple as
  `#(platformName  getSystemAttribute:1003  callingConvention)`; attribute 1003
  is the CPU architecture and we returned the literal `"StackInterpreter"` instead
  of `"x86_64"` (the reference Cog VM returns `"x86_64"`). Fixed `getSystemAttribute`
  case 1003 to report the real arch (x86_64/aarch64/armv7l/i686). Cross-platform
  correctness fix (also right for our ARM/Linux builds). No regression
  (SystemVersionTest 17/17, SmalltalkImageTest 9/9, OSEnvironmentTest 9/9).
- [ ] **`TFUFFIStructuresTest` (22) / `TFUFFIMethodRegistryTest` (2)** — these do
  real callouts into `TestLibrary.dll` (the Pharo FFI unit-test C library):
  `SymbolNotFoundError: Could not find symbol named: #newPoint searching in
  module: 'TestLibrary.dll'`. The library is a test fixture not present on this
  machine; setUp opens it and fails, so all tests in the class error. This is a
  MISSING TEST FIXTURE, not a VM bug (the reference VM fails identically without
  it) — the VM's FFI callout path itself is proven working by
  FFICalloutMethodBuilderTest. To run them, build/stage the iceberg
  `pharo-ffi`/`libtcommon` TestLibrary.dll next to the exe.

### File attributes (Windows semantics)
- [x] **`DiskFileAttributesTest>>testToPlatformPath` / `testFromPlatformPath`** —
  FIXED. `File toPlatformPath:` = `primToPlatformPath:` (= named primitive
  `primitiveStToPlatPath`); on Windows the platform path encoding is UTF-16LE but
  our primitive returned the UTF-8 bytes unchanged (identity, correct only on
  POSIX). Implemented UTF-8<->UTF-16LE conversion in `primitiveStToPlatPath`
  (MultiByteToWideChar) and `primitivePlatToStPath` (WideCharToMultiByte) under
  `#elif defined(_WIN32)`; macOS NFC/NFD and POSIX identity branches unchanged.
  DiskFileAttributesTest 20->22; DiskFileSystemTest still 59/59.
- [ ] **`testNLink` (DiskFileAttributesTest + FileReferenceAttributeTest) /
  `testPermissions`** — 3 niche tests that assert Windows must RAISE on
  unsupported attributes: `file numberOfHardLinks` -> `FileAttributeNotSupported`,
  `fileReference permissions: p` -> `Error`.  `FileSystemDirectoryEntry>>
  numberOfHardLinks` already always raises, but the `File`-level path returns our
  stat `st_nlink`/mode instead of signalling unsupported.  Matching the reference
  Windows VM means leaving the nlink / permission stat slots absent/nil so the
  image raises FileAttributeNotSupported — a subtle stat-array-contract change for
  3 niche tests.  Deferred.

### GUI / headless display — tests that HANG (not just fail)
- [ ] A focused scan of 245 Windows-sensitive classes (2026-06-27) found 8 that
  HANG under a 30s isolation timeout (a hang is worse than a failure — it wedges a
  naive suite run; the official runner's per-test watchdog masks it).  All are the
  GUI/headless gap (Spec presenters / debugger tree-builders block on a World /
  display that does not exist headless — same family as the known CircleMorphTest
  hang): `StOpenFilePresenterTest`, `StOpenDirectoryPresenterTest`,
  `StNavigationSystemTest`, `StUnifiedProcessorTest`,
  `StDebuggerStackCommandTreeBuilderTest`, `StDebuggerToolbarCommandTreeBuilderTest`.
  Plus `TFUFFIFunctionCallTest` (FFI callout into the missing TestLibrary.dll) and
  `SystemDependenciesTest` (likely just slow scanning all 2047 classes, not a true
  deadlock).  Resolved by the SDL2/GUI milestone (4) + the TestLibrary.dll fixture.

### Networking / TLS
- [x] **SocketPlugin (winsock2 port)** — DONE (2026-06-27). The real 1338-line
  BSD-sockets `SocketPlugin.cpp` now compiles, links, and RUNS on Windows over
  winsock2 (replacing `SocketPlugin_win_stub.cpp`); TCP networking works end to
  end through the async I/O-thread + Pharo semaphore model. How: `winsock_compat.h`
  shim (SOCKET/INVALID_SOCKET, SHUT_*/MSG_DONTWAIT, SOCK_LAST_ERROR/EWOULDBLOCK/
  EINPROGRESS, sockClose/sockSetNonBlocking, get/setsockopt char*-optval macros —
  all aliasing native names on POSIX so the file stays semantically identical
  there); `int fd`->`SOCKET fd` (compared to INVALID_SOCKET since SOCKET is
  unsigned); the self-pipe replaced by a loopback UDP socketpair (winsock select()
  watches only sockets), behind gWakeReadFd/drainWake/initWakePair/closeWakePair
  with POSIX keeping the real pipe via #ifdef. **Crucial extra fix:**
  `pharo::platform::platformInit()` was NEVER called, so `WSAStartup` never ran and
  every socket()/getaddrinfo() silently failed — wired it into test_load_image.cpp
  main() (this also fixed DNS: NetNameResolver now resolves). Results vs the
  all-failing stub: Socket newTCP OK; localhost resolves; TCPSocketTest 9/9,
  ZdcSimpleSocketStreamTest 15/15, ZdcReferenceSocketStreamTest 14/15,
  SocketAddressTest 5/5, TCPSocketEchoTest 1/1, SocketStreamTest 17/19,
  ZdcSocketStreamTest 9/15, ZdcOptimizedSocketStreamTest 10/15 (~80+ tests
  recovered, 0 -> majority passing). No hangs.
- [ ] **Socket read-path EOF reporting** — ROOT CAUSE DIAGNOSED (not a race; the
  ECONNRESET fix above already took ZdcReference to 15/15).  Remaining failures
  (`ZdcSocketStream`/`ZdcOptimized` `testPlainClientRead*`/`testPlainClientSkip*`/
  `testReverseEchoUpToEnd`) are DETERMINISTIC in isolation (testPlainClientRead
  fails every run) and PASS on the reference Pharo Windows VM — so it's a real VM
  behaviour difference, not flakiness.  SOCKDBG tracing shows the client reads all
  data correctly (received=6), hits EOF (recv 0 -> OtherEndClosed), but then a
  buffered stream's read loop raises an UNCAUGHT ConnectionClosed.  Cause:
  `primitiveSocketReceiveDataAvailable` uses a bare `select()`, which reports a
  peer-closed socket as readable (recv would return 0) — so `dataAvailable` is
  `true` at EOF.  The buffered Zdc/ZdcOptimized streams poll dataAvailable, keep
  trying to read past EOF, and raise ConnectionClosed.  THE FIX (verified
  partially): make dataAvailable MSG_PEEK after select and return false when the
  peek is 0 (EOF) — that makes testPlainClientRead PASS.  BUT it then breaks the
  server-side `upToEnd` (`[isConnected] whileTrue: [receiveData]` in
  testPlainClientWrite): with dataAvailable false at EOF the image never recv()s
  the 0 that flips the state, so isConnected stays true and it ConnectionTimedOut/
  crashes.  The COMPLETE fix needs BOTH: dataAvailable=false at EOF AND the I/O
  thread setting `SOCK_OTHER_END_CLOSED` when it detects EOF (MSG_PEEK==0 at
  SocketPlugin.cpp ~287), so connectionStatus reflects the close without the image
  having to read 0.  That I/O-thread change touches the deliberate "don't change
  state on EOF" SSL workaround (comment ~288) and MUST be verified on macOS/Linux
  SSL (ZdcSecureSocketStream) — this machine is Windows-only and can't.  Deferred
  to a POSIX-capable session.  Core path solid: TCPSocketTest 9/9, ZdcSimple 15/15,
  ZdcReference 15/15.  Not a blocker — TCP works.
- [ ] **UDP echo/broadcast** (`UDPSocketEchoTest>>testEcho`,
  `UDPSocketTest>>testUDPBroadcastError`) — passes on the reference Windows VM,
  fails on ours with `SocketError: The operation completed successfully` (i.e.
  WSAGetLastError()==0: the image reads `socketError` after a UDP op that returned
  no data, and our sockError is 0).  send/recvfrom primitives look correct; likely
  a loopback-datagram delivery/binding difference or the classic Windows
  SIO_UDP_CONNRESET behaviour (a UDP recvfrom after an ICMP port-unreachable
  returns WSAECONNRESET).  Needs tracing; UDP-specific, separate from TCP.
- [x] **Crypto / SqueakSSL / HTTPS** — DONE (2026-06-28). `PHARO_WITH_CRYPTO=ON`
  on Windows now (was OFF); MSYS2 CLANG64 ships OpenSSL 3.6.3 via pkg-config. The
  three crypto sources (SqueakSSL.c, DSAPrims.c, sqGenericSSL.c — the real OpenSSL
  TLS backend) are POSIX-clean and compiled + linked unchanged; added the OpenSSL
  pkg-config include/link to the WIN32 CMake branch mirroring Linux. With the
  winsock2 SocketPlugin already working, TLS runs over our TCP sockets:
  **`ZnClient get: 'https://example.com'` returns HTTP 200 with the page body** —
  a full TLS handshake end to end.  Also: native SSL plugin now detected
  (ZdcSecureSocketStreamTest 2/2, was 1/2 — testIsNativeSSLPluginPresent passes);
  SHA1 9/9, MD5 9/9, SHA256 11/12 (one FIPS vector, minor).

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
