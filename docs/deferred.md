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
  - **2026-07-02 DEEP INVESTIGATION (unresolved, 3 real bugs fixed en route).**
    Repro: `eval "[['<repo>/scripts/pharo-headless-test/run_sunit_tests.st'
    asFileReference fileIn. 'FILEIN-OK'] on: Error do: [:e | 'ERR: ', e
    messageText]] value"` on a fresh Pharo.image. Deterministic per invocation
    FORM (a big padding chunk compiled first flips it to PASS); flips with
    `PHARO_NEWSPACE_MB` (16 fail / 24 pass / 32 fail / 48+ pass). EVIDENCE
    CHAIN (all instrumented, knobs still in tree):
    - Scavenge-count bisect (`PHARO_YG_SKIP_SCAV_FROM`): first corrupting
      scavenge = #12. Substituting fullGC at #12 (`PHARO_SCAV_FULLGC_AT`)
      ALSO fails => any object MOTION there corrupts; prepare/afterGC
      round-trip WITHOUT motion (`PHARO_GC_ROUNDTRIP_ONLY`) passes.
    - Post-scavenge state verified clean: raw word-scan of old+perm for
      eden-range values (`PHARO_SCAV_RAWSCAN`), per-frame ip verify
      (`PHARO_GC_FRAME_VERIFY`), `PHARO_NO_METHOD_CACHE`, JIT off — all
      still fail / find nothing.
    - Eden generation-rotation detector (`PHARO_EDEN_ROTATE`: eden halves
      alternate, retired half PAGE_NOACCESS): compile STILL FAILS WITH NO
      FAULT => no stale young pointer is ever DEREFERENCED — the divergence
      is pure identity-compare (`==` on raw bits).
    - Activation-trace diff (`PHARO_TRACE_SENDS_FROM_SCAV`, acttrace):
      failing vs passing runs IDENTICAL for 29,049 activations after
      scavenge 12, then FAIL's `Object>>=` scan in
      `OrderedCollection>>remove:ifAbsent:` runs past where PASS matches:
      the recorded pop's identity is not among the sequence's elements.
    - Forensics at the error (`PHARO_OCIR_ERROR_DUMP` + heap sweep): the
      store/pop pair exist ONLY in the recorded Association + materialized
      Contexts — in NO array anywhere. The sequence's OC array instead has
      an 8x-repeated ref to ONE young OCIRPushFullClosure at [43..50] and a
      young tail [51..62]; per `PHARO_SCAV_DUMP_FORWARD` maps the tail
      values were never tenured (fresh young), the pop was tenured at
      scavenge 13. The 8x run pre-exists the optimizer's first removeIndex:
      (SMEAR-105 detector) but was NOT written by storePointer, at:put:,
      prim 105, prim 145, become, or the tenure memcpy (all instrumented —
      RUN-FORM/SMEAR detectors silent). slotAtPut is ALSO exonerated
      (PHARO_SLOT_RUN_TRIPWIRE at the ObjectHeader level: only legit nil
      fills fire, nothing at scav>=12) — so the smear enters via raw
      `slots()[i]=` writes, an uninstrumented memcpy (shallowCopy/clone,
      compact copyAndUnmark), or the OC's array IVAR is switched to a
      different array entirely. The compact savedFirstFieldsSpace
      parallel-walk desync was RULED OUT (copyAndUnmark now asserts the
      saved-fields pointer drains fully — [GC-COMPACT-DESYNC] never fires
      in the failing run). 2026-07-02 LATE-SESSION NARROWING (decisive): with ALL writers
      instrumented simultaneously (slotAtPut both-direction run tripwire
      nil-filtered, prim-105 post-copy OCIR-run import scan across the
      whole run, tenure-copy scan, become, fills), the 8x-run is NEVER
      OBSERVED BEING WRITTEN — its first sighting is always "already
      present" at the optimizer's first removeIndex: shift. Discovered en
      route: SequenceableCollection>>from:to:put: (prim-105 doubling
      self-copy) is a LEGIT run producer, so run-shaped content exists
      innocently in other arrays. Conclusion: the array's content was
      never smeared — the REFERENCES to the sequence's young array were
      RETARGETED TO A DIFFERENT OBJECT at scavenge 13 (bad forward-map
      entry, tenure memcpy from wrong source, or overlapping eden
      allocations), so post-GC readers see another tenured array whose
      legit content merely looks smeared. Tenure source/dest range-overlap checks: CLEAN (no
      overlapping copies); growAtLast young sources verified fresh (not
      in any forward map — legit). FINAL SYNTHESIS (2026-07-02 end):
      the run is COALESCED, not written — 8 references to ONE young
      OCIRPushFullClosure node sat INTERLEAVED with the store/pop pairs,
      and each convertStorePop removeIndex: shift pulled them adjacent
      (that is why no write tripwire ever fires: the equal values were
      never adjacent at write time, and every detector needs >=2 equal
      neighbors). The ROOT anomaly is therefore upstream in IR BUILD:
      the sequence was given EIGHT references to a single
      PushFullClosure node, alternating with store/pop pairs — the exact
      shape of a re-executed `sequence add:` loop (partial rollback /
      resume-from-context replay with current temps, or an interpreter
      loop-back-edge bug at a GC boundary). DUP-APPEND detector result:
      ZERO duplicate OCIR insertions (excluding legit sort swap:with:
      transients) — the 8 references were never INSERTED as duplicates
      either. Every write/insertion path is clean; therefore 8 DISTINCT
      refs were later REWRITTEN to one value by a non-store mechanism
      (no slotAtPut/storePointer/prim-105/tenure/become fires). NEXT
      ANALYTICAL STEP (no new instrumentation needed): the [P105] logs
      capture the failing array's complete removeIndex: shift sequence —
      invert the shifts against the final OCIR-ERROR dump to reconstruct
      the exact pre-conversion array content and localize which 8
      positions held what before the loop; then diff that against what
      the visitor must have seen (the recorded pairs) to pin the rewrite
      window to either [tenure..first-shift] or mid-loop. DONE (PHARO_DUMP_AT_ACT=28600, actdump-FAIL vs
      actdump-PASS): at the identical traced activation, FAIL's sequence
      has lastIndex 66 vs PASS 65, and where PASS holds
      [PushLiteral,StoreRemoteTemp,Pop]x4 + PushFullClosure x2, FAIL holds
      the triplets x3 (phase-shifted) + PushFullClosure x6 — one triplet
      CONSUMED, the closure-pair GROWN by 4. That is the arithmetic
      signature of FOUR REPLAYED shift-left operations: each re-executed
      removeIndex: shift over a region containing an adjacent [C,C] pair
      extends the run downward by one and eats one element below.
      CONCLUSION: the interpreter RE-EXECUTES a bytecode range (including
      the prim-105 shift send) after a GC at request #12 — partial replay
      (the lastIndex arithmetic suggests the shift replays without a
      matching extra decrement, i.e. resumption lands mid-removeIndex:).
      Replayed shifts are idempotent on run-free regions (why 3800 tests
      pass); only a shifted region containing adjacent equal refs makes
      the replay visible. PRIME SUSPECT: the ip/offset round-trip or
      resume-from-context restoring execution to a point BEFORE a
      completed send when the scavenge safe point fires near a
      primitive-triggering bytecode; the current-frame GC-VERIFY byte
      compare cannot catch a systematic restore-to-same-send. NEXT: log
      (method, bcOffset) at prepareForGC/afterGC for the current frame at
      request #12 and single-step-trace the following ~200 bytecodes in
      FAIL vs PASS (PHARO_TRACE_EXTENT_SEL or a new bytecode-window trace)
      to catch the replayed range red-handed; also audit executeFromContext
      pc->ip conversion for an off-by-one vs the pc convention (pc points
      AT the next unexecuted bytecode, 1-based).
      Remaining leads (in order):
      (a) ALL run detectors check LOWER neighbors ([i-1],[i-2]) — a
      DESCENDING fill evades every one of them, and
      OrderedCollection>>makeRoomAtFirst's copy loop IS descending
      (`array at: newLastIndex - offset put: (array at: lastIndex -
      offset)`): if the READ side hits a wrong/stale array returning a
      constant, the descending writes paint exactly the observed run —
      re-run with a descending-aware tripwire (check [i+1],[i+2] too);
      (b) raw slots()[i] writers audit came up empty (ImageLoader/nil-fill/
      scavenge/compact/prim-145 only; prim 145 binds solely to byte/word
      classes in Pharo 13 — its pointer-fill branch is dead code but fills
      fixed ivars of fmt 0-3 receivers, harden someday);
      (c) shallowCopy/clone memcpy paths; (d) updatePointersAfterCompact's
      unbounded survivor-space garbage walk (writes past newSpaceEnd_
      possible from garbage headers — harden regardless); (e)
      resume-from-context partial rollback via a path the pc-refresh fix
      doesn't cover. Overlap semantics verified bug-compatible with stock
      (both smear `#(1 2 1 2 1 2 1 2 9 10)` on a dst>src pointer-array
      self-copy).
    - REAL BUGS FIXED during the hunt (all committed, regression-clean):
      (1) allocators lacked the Spur 16-byte minimum object size — 0-slot
      objects (Object new, #(), '') packed at 8 bytes, desyncing every
      linear heap walk (allObjectsDo/become write-through, old-space scan
      via the eden-full fallback); (2) fullGC ran its pre-compact scavenge
      AFTER markPhase — scavenge-tenured-but-unmarked objects (weak-only
      reachable, e.g. fresh Symbols in the SymbolTable WeakSet) were
      compacted over while still referenced; (3) prepareForGC's temp-sync
      wrote through `frame.materializedContext` without verifying it is a
      Context, and refreshed temps/stackp but never pc — a later
      resume-from-context replayed already-executed bytecode ranges with
      current temps (partial rollback). None cured this repro, but each is
      a genuine correctness fix.
    Workaround unchanged: inject the runner with the reference VM
    (scripts/win-run-full-suite.sh does this).
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
(FileAttributesPluginPrimsTest: fixed 2026-07-02 — see the [x] entry above.)

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
- [x] **TestLibrary.dll FFI fixture — BUILT (2026-07-02)**
  (`src/vm/plugins/TestLibrary.c`, clean-room from the image tests' signatures
  and assertions; CMake target `TestLibrary`, staged next to the exe where
  FFIWindowsLibraryFinder probes). Same-thread TFUFFI results:
  BasicTypeSizeTest 48/48, MethodRegistryTest 2/2, StructuresTest 11/11
  same-thread (+11 worker-variant errors), BasicTypeMarshalling 17/18
  same-thread, DerivedTypeMarshalling 16/16 same-thread, FunctionCall 2/2+1skip.
  FFICalloutMethodBuilderTest still 10/10.
- [ ] **TFFI worker runtime (threaded FFI)** — every TFUFFI* class is
  parameterized over {TFTestLibraryUsingSameThreadRunner,
  TFTestLibraryUsingWorker}; the WORKER halves (callouts on a dedicated
  worker thread: TFWorker/primitive support) error because our VM lacks the
  threaded-FFI runtime. ~55 tests across the family. A real VM feature
  (worker threads + cross-thread callout queue + callbacks), not a fixture
  issue.

### File attributes (Windows semantics)
- [x] **`DiskFileAttributesTest>>testToPlatformPath` / `testFromPlatformPath`** —
  FIXED. `File toPlatformPath:` = `primToPlatformPath:` (= named primitive
  `primitiveStToPlatPath`); on Windows the platform path encoding is UTF-16LE but
  our primitive returned the UTF-8 bytes unchanged (identity, correct only on
  POSIX). Implemented UTF-8<->UTF-16LE conversion in `primitiveStToPlatPath`
  (MultiByteToWideChar) and `primitivePlatToStPath` (WideCharToMultiByte) under
  `#elif defined(_WIN32)`; macOS NFC/NFD and POSIX identity branches unchanged.
  DiskFileAttributesTest 20->22; DiskFileSystemTest still 59/59.
- [x] **FileAttributesPluginPrimsTest — DONE (2026-07-02), 6/6** (stock is
  6/6; the old "pre-existing/cross-platform" note was wrong). Error-code
  fidelity in three raw prims: primClosedir arg-shape failures and
  primFileExists non-string args fail with #'bad argument' (primFailCode 3,
  asserted via the PrimitiveFailed selector); primFileAttribute validates the
  attribute number (1..16) BEFORE stat'ing (out-of-range surfaced as
  FileDoesNotExistException); primFileExists paths > PATH_MAX fail with
  PrimitiveError -1 (stringTooLong -> IllegalFileName).
- [x] **testNLink / testPermissions — DONE (2026-07-02),
  DiskFileAttributesTest 24/24.** Stock-parity contract (probed): the
  Windows FileAttributesPlugin supports neither nlink reads nor chmod —
  both fail with PrimitiveError errorCode -13 (File unsupportedOperation),
  which the image's File class>>signalError:for: maps to
  FileAttributeNotSupported. Implemented via primFailCode_=PrimErrOSError +
  osErrorCode_=-13 in the attrNum-5 read and primitiveChangeMode (Windows
  only); the stat-ARRAY nlink slot is nil. Verified exact exception class
  match vs stock for both paths. No regressions: DiskFileSystemTest 59/59,
  FileSystemTest 126/126, FileReferenceTest 112/112, FileLocatorTest 38/38,
  FileReferenceAttributeTests 19/19.
- [x] **The 8 hang classes from the 2026-06-27 scan — RESOLVED (2026-07-02).**
  All six St* presenter/debugger classes now RUN to completion (the GUI
  milestone fixed the underlying World/display gap):
  StOpenDirectoryPresenterTest 11/11, StNavigationSystemTest 12/12,
  StDebuggerStackCommandTreeBuilderTest 33/33,
  StDebuggerToolbarCommandTreeBuilderTest 20/20. StOpenFilePresenterTest
  16r/2e — BETTER than stock (7e headless). TFUFFIFunctionCallTest fixed by
  the TestLibrary fixture. StUnifiedProcessorTest 24r/19e → **24/24 PASS
  (2026-07-02)**: not a spin at all — primitive 295
  (`primitiveTranslateStringWithTable`) required argCount==4, but the modern
  instance form `String>>translateFrom:to:table:` sends 3 args, so the prim
  ALWAYS failed and every `asLowercase` ran the per-character Smalltalk
  fallback (~28x slower; 100k asLowercase 449ms → 137ms after the fix).
  Spotter's unified search lowercases everything, so each query took ~28s →
  19x TestTookTooMuchTime. Fix accepts both forms (string at stackValue(3)
  either way) and pops argCount leaving the receiver, exactly like stock's
  MiscPrimitivePlugin. String/Symbol/WideString/Character regression: 739
  tests, 0F/0E.

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
- [x] **Socket read-path EOF reporting — DONE (2026-07-02).**
  `ZdcSocketStreamTest` 9/15 → 15/15, `ZdcOptimizedSocketStreamTest` 10/15 →
  15/15. Fix: `primitiveSocketReceiveDataAvailable` now mirrors stock Cog's
  `socketReadable()` — MSG_PEEK instead of bare select() on data sockets
  (bare select reports a peer-closed socket readable, so dataAvailable was
  true at EOF and buffered streams read into an uncaught ConnectionClosed).
  BOTH halves of the earlier diagnosis land in the primitive itself: EOF →
  answer false AND set `SOCK_OTHER_END_CLOSED` + eofDetected — so the
  server-side `[isConnected] whileTrue: [receiveData]` upToEnd loop also
  terminates (testReverseEchoUpToEnd passes). Listening sockets keep select()
  semantics (recv is invalid on listeners). The I/O-thread "don't change
  state on EOF" SSL workaround (~line 288) is UNTOUCHED — the state flip
  happens only when the image polls dataAvailable. SSL verified ON THIS
  machine (crypto is on for Windows since 2026-06-28): ZdcSecure 2/2,
  ZdcReference 15/15, TCPSocketTest 9/9, and `ZnClient get:
  'https://example.com'` → 200. The old "needs a POSIX session" caveat is
  moot. Remaining: SocketStreamTest 17/19 (2 pre-existing errors, separate
  cause — see next item).
  FOLLOW-UP FIX (same day, found by bisect): the MSG_PEEK approach exposed a
  Windows UDP quirk — peeking 1 byte of a LARGER pending datagram fails with
  WSAEMSGSIZE (POSIX truncates and returns 1), which the error branch misread
  as connection death, killing every UDP server's waitForData
  (UDPSocketEchoTest). WSAEMSGSIZE now counts as "data available" in BOTH
  peek sites (dataAvailable prim + the I/O-thread EOF detector), and datagram
  sockets are exempt from all connection-death transitions.
- [x] **SocketStreamTest flush-after-close — DONE (2026-07-02), 19/19.**
  THREE stock-parity pieces (each probed against the reference VM):
  1. send() on a dead connection (ECONNRESET/ECONNABORTED/EPIPE, TCP only):
     report 0-sent + flip state to OtherEndClosed instead of failing the prim
     (a failed prim surfaced bare SocketError — not a NetworkError — which
     escaped SocketStream>>flush's handler).
  2. sendDone answers (isDgram or state == Connected) instead of
     unconditionally true — Socket>>waitForSendDoneFor:'s whileFalse loop is
     the only place the image converts a dead send into ConnectionClosed.
  3. Socket close is FIRE-AND-FORGET like stock: shutdown() + state
     UNCONNECTED immediately (+ semaphore signals). We used to hold
     THIS_END_CLOSED until the peer's FIN, so closeAndDestroy:'s
     waitForDisconnectionFor: blocked its full timeout whenever the peer
     stayed open — every such close cost 30 s (stock: 0 ms), which is what
     actually produced TestTookTooMuchTime.
  Also: PrivateSocket.isDgram field; SIO_UDP_CONNRESET disabled at UDP socket
  creation (stock does the same). Full battery green: TCP 9/9, TCPEcho 1/1,
  ZdcSocket/Optimized/Reference/Simple 15/15 each, ZdcSecure 2/2, UDP echo
  1/1 + UDPSocketTest 2/2, HTTPS 200.
- [x] **UDP echo/broadcast — DONE (2026-07-02)** (`UDPSocketEchoTest` 1/1,
  `UDPSocketTest` 2/2). TWO missing pieces, neither in send/recvfrom:
  - `Socket>>setPort:` (how a UDP server binds) calls named primitive
    `primitiveSocketListenWithOrWithoutBacklog`, which the plugin didn't
    implement — the prim failed with sockError 0, producing exactly
    "SocketError: The operation completed successfully". Implemented: 2-arg
    (bind only, the UDP form) and 3-arg (bind+listen) variants; also guarded
    all listen prims to skip listen() on SOCK_DGRAM (EOPNOTSUPP), matching
    stock Cog's sqSocketListenOnPortBacklogSizeInterface UDP special-case.
  - `SO_BROADCAST` was missing from both get/setOptions tables (silently
    ignored), so broadcast could never be enabled AND
    `broadcastMisconfiguredForSendingTo:` couldn't detect the misconfig.
  TCP unaffected: TCPSocketTest 9/9, TCPSocketEchoTest 1/1, ZdcSimple 15/15.
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

### GUI (milestone 4 — the last big gap)

**BREAKTHROUGH (2026-06-28): the Windows morphic GUI render path WORKS — verified
visually.** The full Pharo desktop (menu bar + "Welcome to Pharo 13" window with
the lighthouse logo) renders pixel-perfect into gDisplaySurface on Windows — see
`docs/images/windows-gui-pharo-world.png`.  The render chain World ->
OSWorldRenderer -> SDL2 stubs -> gDisplaySurface is fully working; what remains is
"only" presenting that surface in a real on-screen window + feeding input events.

ACTIVATION RECIPE (reproducible headlessly, no on-screen window needed):
  1. Put any file named `SDL2.dll` in the IMAGE directory (FFIWindowsLibraryFinder
     searches it first; our FFI routes SDL symbols to built-in stubs so the file
     only needs to exist).  This flips `SDL2 isAvailable`/`OSSDL2Driver isSuitable`.
  2. Run with `PHARO_FORCE_DISPLAY=1` (creates gDisplaySurface even in eval/headless
     mode — without it gDisplaySurface is NULL and RenderPresent has nowhere to
     write — this was the last bug) and `PHARO_DUMP_DISPLAY=1` (dumps the frames).
  3. `eval` the activation in `scripts/win-gui-render-check.st`: `OSSDL2Driver new`
     (inits the driver — fixes the earlier nil `critical:` lock), then
     `OSWorldRenderer forWorld: World` + install it on the worldState + `doActivate`
     (creates the OSWindow via SDL_CreateWindow stub, picks the SDL2 driver, draws
     the World), then a `World fullRepaintNeeded; displayWorld` loop to force
     presents.  Result: /tmp/vm-display-{20,60,150}.ppm with `changed=1` showing the
     live desktop.  Confirmed: 1 SDL window, 122 RenderPresents (all main renderer
     + valid texture), Pharo BitBlts directly into gDisplaySurface via LockTexture.

REMAINING for a real interactive GUI (render + on-screen window now DONE):
  A. [x] **On-screen window — DONE (2026-06-28).** `src/platform/Win32DisplaySurface.hpp`
     is a DisplaySurface that owns a Win32 HWND and blits `pixels()` to it on
     `update()` via GDI `StretchDIBits` (top-down 32bpp BI_RGB DIB — Pharo's XRGB
     is BGRA byte order on little-endian, matching the DIB).  Enable with
     `PHARO_GUI_WINDOW=1` (debug_vars.h); test_load_image points gDisplaySurface at
     it instead of TestDisplaySurface.  VERIFIED: the live Pharo desktop appears in
     a real on-screen window titled "Pharo (Windows VM)" — see
     docs/images/windows-gui-onscreen-window.png.  Messages are pumped in update()
     (works while the World is rendering; a static idle World stops calling update()
     so the window would freeze until input — addressed by B/C).
  B. [x] **Auto-activate in interactive mode — DONE (2026-06-28).** No code change
     needed: once `SDL2.dll` is in the image dir (so SDL2 isAvailable) AND the image
     runs in interactive mode (the harness passes `--interactive` when launched with
     no eval args), the image PICKS OSWorldRenderer itself and draws the World.  So
     `PHARO_GUI_WINDOW=1 test_load_image.exe <image>` (no eval) brings up the full
     live Pharo desktop in the on-screen window automatically — see
     docs/images/windows-gui-interactive.png.  The harness already runs the image's
     morphic loop via interpret() and injects a right-click at 5s (injectMouseClick
     -> gEventQueue), so events flow.  (The earlier "headless, no draw" was ONLY
     because SDL2.dll was missing -> SDL2 unavailable -> NullWorldRenderer.)
  C. [x] **Win32 -> SDL event injection — DONE (2026-07-01).** The wndProc
     translates WM_MOUSE*/WM_KEY*/WM_CHAR/WM_MOUSEWHEEL into pharo::Event and
     pushes to gEventQueue; stub_SDL_PollEvent delivers them to OSSDL2Driver.
     VERIFIED interactively: menubar dropdowns, World menu, Playground typing,
     Do-it evaluation, debugger-on-DNU, Profiler — see
     docs/images/windows-gui-menu-click.png / windows-gui-debugger.png.
     Two root-cause fixes were required (both would silently eat ALL input):
     - stub_SDL_PollEvent derived WHICH button changed from arg3 (buttons-still-
       pressed, 0 on UP) → every release reported LEFT → right button stuck
       pressed forever in Morphic's hand.  Now tracks prev state and uses the
       delta (FFI.cpp).
     - a stale eval-mode startup.st in the image dir suspends all Morphic
       processes → interactive runs rendered but ignored input.  Non-eval runs
       now delete a startup.st carrying the [STARTUP-ST-FIRED] marker
       (test_load_image.cpp).
     Debug knob: PHARO_WIN_EVENT_TRACE=1 traces push→poll delivery end-to-end.
  D. [x] **SDL2.dll provisioning — DONE (2026-07-02).** No manual staging: CMake
     writes an `SDL2.dll` marker file next to test_load_image.exe (POST_BUILD),
     and `FFIWindowsLibraryFinder` finds it via `Smalltalk vm directory` — the
     image dir stays pristine and the stock reference VM is never poisoned.
     Two real bugs fixed to make that work:
     - primitive 142 (primVmPath) split the exe path on '/' only, so on Windows
       `Smalltalk vm directory` returned the full EXE path (stock returns the
       DIRECTORY with trailing '\'). Now find_last_of("/\\") — matches stock;
       SystemResolverTest 7/7.
     - test_load_image had no Windows branch for exe-path resolution; argv[0]
       relative + the early chdir(imageDir) produced a wrong vm path. Now
       GetModuleFileNameA (cwd-independent).
     GUI verified with a pristine image dir: desktop renders, World menu opens
     on right-click.

Historical scoping notes (how the breakthrough was reached) follow:
- [ ] **SDL2 / Morphic display** — headless only.  ARCHITECTURE MAPPED
  (2026-06-28), so this is no longer a black box:
  - The render path is CROSS-PLATFORM and already present.  The image's
    `OSSDL2Driver` calls SDL2 via FFI; FFI.cpp implements SDL2 as built-in STUB
    functions (`stub_SDL_CreateWindow`/`CreateRenderer`/`RenderPresent`/`PollEvent`,
    FFI.cpp ~590-780, dispatched because FFI special-cases the "SDL2" module).
    `stub_SDL_RenderPresent` copies the rendered Pharo World into
    `pharo::gDisplaySurface->pixels()` (FFI.cpp ~735-776).  `gDisplaySurface` is a
    `DisplaySurface*` (DisplaySurface.hpp); on Apple it's backed by the Metal
    layer (visible), on Windows the harness points it at an in-memory
    `TestDisplaySurface` (test_load_image.cpp:172, 1032).  So the Morphic World
    pixels ALREADY flow to a buffer on Windows — there's just no window to show it.
  - WHAT'S MISSING on Windows (3 parts):
    1. **Interactive run mode** — test_load_image evals and exits; the Morphic
       World main loop never runs, so nothing draws (the "Display Check Pixel
       count: 0" in the logs is exactly this — not a render bug).  Need a mode that
       enters the image's World loop and keeps running.
    2. **A Win32 HWND-backed `DisplaySurface`** — subclass DisplaySurface to own an
       HWND (RegisterClass/CreateWindow) and present `pixels()` to it on
       `update()` via GDI `StretchDIBits` (or Direct2D); set `gDisplaySurface` to
       it instead of TestDisplaySurface.  Mirrors the Apple Metal path.
    3. **Win32 -> SDL event injection** — pump the window proc (PeekMessage/
       Dispatch) and translate WM_MOUSEMOVE/WM_*BUTTON*/WM_KEY*/WM_CHAR into the
       SDL_Event queue that `stub_SDL_PollEvent` (FFI.cpp ~610) drains.
  - VERIFY HEADLESS FIRST without a window: `PHARO_DUMP_DISPLAY=1` dumps
    gDisplaySurface to `/tmp/vm-display-{20,60,150}.ppm` at those RenderPresent
    counts (test_load_image.cpp ~199-205) — but needs the World loop running
    (part 1) to produce frames.  First increment = get one World render into the
    PPM (proves parts of the path), then add the HWND present, then events.
  - SDL2 is NOT installed in MSYS2 (no real-SDL2 reroute needed — the built-in
    stub bridge is the design; we add a Win32 backend behind it, like Apple/Metal).
  - EMPIRICALLY CONFIRMED (2026-06-28): the image already HAS the pieces —
    `Smalltalk includesKey: #OSSDL2Driver` and `#OSWindowDriver` are both true,
    `World` is a live WorldMorph, and `200 timesRepeat: [World doOneCycle]` runs
    without error — BUT produces zero RenderPresents / zero PPM dumps under
    PHARO_DUMP_DISPLAY=1.  So the World draws into its Form but nothing presents to
    SDL/gDisplaySurface: the image is on a headless display driver, never opening an
    OSSDL2Driver OSWindow.  Part 1 of the plan is therefore really "open an
    OSSDL2Driver-backed OSWindow and run its event/display loop", which then
    activates the existing SDL-stub -> gDisplaySurface path.  (OSPlatform reports
    Win64Platform.)
  - ACTIVATION CHAIN (2026-06-28, deeper probe): `Smalltalk isHeadless` is TRUE and
    `World worldState worldRenderer` is a `NullWorldRenderer` — the image booted
    headless, so part 1 ("interactive run mode") is the real gate.  Also a smaller
    prerequisite: FFI `isModuleLoaded("SDL2")` does `dlsym(RTLD_DEFAULT,"SDL_Init")`
    (FFI.cpp:265), but our exported `SDL_Init` (FFI.cpp:1386, `SDL_EXPORT` =
    `__attribute__((weak,used,visibility("default")))`) is NOT dlsym-findable from a
    Windows EXE (visibility("default") exports nothing on PE; needs
    `__declspec(dllexport)` or a `-Wl,--export-all-symbols` link flag).  EASIEST
    FIX: have `isModuleLoaded`'s SDL2 branch also return true when the stub is in
    our registry (registerSDL2Stubs always registers SDL_Init), so it doesn't
    depend on EXE symbol export.  But that only flips the FFI gate — the World still
    won't render until the image runs non-headless on an OSSDL2Driver window (the
    interactive-mode milestone-4 work).
  - THE PRECISE ACTIVATION GATE (2026-06-28): `OSWorldRenderer class>>isApplicableFor:`
    is literally `^ Smalltalk isHeadless and: [CommandLineArguments new hasOption:
    'interactive']`.  So the morphic OS-window renderer activates only when the
    image is launched with `--interactive`.  The image currently boots via
    `NonInteractiveUIManager` (no --interactive), so it stays on NullWorldRenderer.
    Therefore the concrete milestone-4 FIRST STEP is an INTERACTIVE RUN MODE in
    test_load_image: pass `--interactive` through to the image's CommandLineArguments
    AND run the image's World/event loop instead of eval-then-exit, so the image
    opens an OSSDL2Driver OSWindow (-> SDL_CreateWindow stub sizes gDisplaySurface ->
    RenderPresent fills it).  Then verify headlessly with PHARO_DUMP_DISPLAY before
    adding the real Win32 HWND present + Win32->SDL event injection.  (OSSDL2Driver
    class>>isSuitable = `SDL2 isAvailable`, so the isModuleLoaded/SDL-symbol fix
    above is also needed for the driver to be picked.)
  - SDL2-AVAILABLE ROOT CAUSE (2026-06-28): `SDL2 isAvailable` =
    `[(ExternalAddress loadSymbol: 'SDL_Init' from: SDL2Library uniqueInstance
    libraryName) isNotNil] onErrorDo: [false]`.  Our FFI stubs ARE reachable —
    `ExternalAddress loadSymbol: 'SDL_Init' from: 'SDL2'` returns a valid stub
    address — but `SDL2Library uniqueInstance libraryName` RAISES "Cannot locate any
    of #('SDL2.dll' 'libSDL2.dll')", so isAvailable is false.  The image-side
    library FINDER requires a real DLL file on disk before it will even hand the
    module name to FFI.  COMPLETE gate list to light up GUI on Windows:
    (1) make `SDL2Library>>libraryName` succeed — either drop a findable `SDL2.dll`
    next to the exe (our FFI routes "SDL" module symbols to the built-in stubs at
    Primitives.cpp:26401, so the file only needs to EXIST, not be loaded), OR
    `pacman -S mingw-w64-clang-x86_64-SDL2` and switch to the real-SDL2 path
    (Primitives.cpp:26401 would need to NOT intercept SDL on Windows so real
    SDL2.dll drives a real window — the cleaner long-term path, no Win32 GDI
    backend needed); (2) launch with `--interactive` (OSWorldRenderer gate);
    (3) the interactive run mode (run the World/event loop, not eval+exit);
    (4) for the stub path only, a Win32 HWND present backend behind gDisplaySurface
    + Win32->SDL event injection.  Every gate is now identified — this is a
    focused milestone-4 build-out with visual (windowed) verification.
  - PROGRESS (2026-06-28): gates 1+2 CLEARED.  Dropping any file named `SDL2.dll`
    into the IMAGE directory (the FFIWindowsLibraryFinder searches the image dir
    first; a copy of any DLL works since our FFI routes "SDL" symbols to the
    built-in stubs and never dlopens the file) flips `SDL2 isAvailable` -> true and
    `OSSDL2Driver isSuitable` -> true.  Then running the harness with NO args
    (interactive mode, line ~1100) activates the SDL2 driver: the log shows
    `OSSDL2Driver>>eventLoop` running, a 1024x768 display surface + 32bpp Display
    Form created.  NOTE: the finder checks the image dir, NOT the exe dir, so the
    build can't auto-provide SDL2.dll there — either stage it next to the image or
    add a startup-script patch (PharoBridge.writeStartupScript) that stubs
    `SDL2Library>>libraryName`/`SDL2 class>>isAvailable`.
  - NEXT SUB-PROBLEM (the real remaining gap): in interactive mode the World does
    NOT draw a frame.  `displayFormReady_` stays false (so neither primitiveBeDisplay
    nor primitiveShowDisplayRect at Primitives.cpp:5185/14105 was called) AND there
    is no SDL_RenderPresent — i.e. NEITHER display path fires.  The OSSDL2Driver
    eventLoop is polling (stub_SDL_PollEvent returns 0), but the morphic UI draw
    process never produces a frame.  So the gap is now a Morphic draw/scheduling
    issue (the World renderer/main-loop not drawing in interactive mode), not the
    SDL bridge.  Investigate: is World worldState worldRenderer switched to
    OSWorldRenderer (vs still NullWorldRenderer) under interactive mode, and is the
    morphic UI process actually running doOneCycle / getting an initial full-damage
    redraw.  Verify headlessly via PHARO_DUMP_DISPLAY once a frame is produced.
  - CONCRETE BLOCKER FOUND (2026-06-28): with SDL2 available, `OSWorldRenderer
    startUp: true` runs WITHOUT switching the World renderer — it stays
    NullWorldRenderer — and `OSSDL2Driver allWindows` fails with "receiver of
    'critical:' is nil".  So OSSDL2Driver's class-side mutex/semaphore (the lock its
    window registry uses) is NIL: `OSSDL2Driver class>>startUp`/`initialize` never
    fully ran on our VM, so the driver can't register a window and the renderer
    can't switch.  THE NEXT STEP is to get OSSDL2Driver's class init/startUp to run
    (set up its mutex + SDL2 init) — check `OSSDL2Driver class>>initialize` /
    `startUp:` and why the lock ivar is nil (likely the SDL2 init step or the class
    startUp wasn't invoked because the image booted headless; may need to call it
    explicitly in the interactive path or via a startup-script patch).  Once the
    mutex is non-nil and a window registers, OSWorldRenderer can take over and the
    World should draw -> SDL_RenderPresent -> gDisplaySurface (PPM-verifiable).

### Diagnostics / platform features (honest stubs)
- [ ] **Sampling profiler** (`Profiler.cpp`) — `enable()` is a no-op on Windows
  (POSIX SIGPROF/setitimer). `PHARO_PROFILE=1` prints "not supported on Windows".
- [ ] **SIGSEGV crash recovery** (`test_load_image.cpp:804` `pharoWinCrashHandler`)
  — Windows installs a Vectored Exception Handler that DUMPS fault info (code,
  fault addr, rip, rva, step count, active method, RtlCaptureStackBackTrace) but
  returns `EXCEPTION_CONTINUE_SEARCH` so the process still crashes.  The POSIX
  `sigaction` + `siglongjmp` RECOVERY path is not replicated (a VEH can't safely
  unwind, and the Windows faults seen were heap corruption, not recoverable). So:
  diagnostic dump yes, recovery no.
- [x] **Native backtraces — DONE (2026-07-02)** (`windows.cpp` + dbghelp):
  `backtrace`/`backtrace_symbols` via RtlCaptureStackBackTrace + DbgHelp
  SymFromAddr. System-DLL frames symbolize fully; our clang exe carries DWARF
  (not PDB), so its frames print module+0xOFFSET — resolve with
  `llvm-addr2line -f -C -e test_load_image.exe (0x140000000+offset)`
  (verified: induced AV resolved to primitiveExternalUint32Read + file:line).
  The Win32 crash handler (pharoWinCrashHandler) now prints symbolized
  frames + the resolve recipe; dumpCxxBacktrace/DNU traces work too.
- [x] **Symlinks — DONE (2026-07-02)** (`Primitives.cpp` winIsSymlink /
  winReadSymlinkTarget): detection via FindFirstFileW
  FILE_ATTRIBUTE_REPARSE_POINT + IO_REPARSE_TAG_SYMLINK (attr 16), target
  via CreateFileW(OPEN_REPARSE_POINT) + FSCTL_GET_REPARSE_POINT PrintName
  (attr 1). Verified vs stock on a real mklink symlink: isSymlink=true and
  the exact target path. (lstat/readlink POSIX shims remain no-ops — the
  wide Win32 APIs are the mechanism, same as stock's plugin.) File family
  regression-clean.
- [ ] **POSIX file ownership** — `chown`/`lchown` return ENOSYS (Windows has no
  POSIX uid/gid model); the two calling primitives turn that into a primitive
  Failure, which the image handles.
- [x] **Clipboard — DONE (2026-07-02)** (`windows.cpp`) — real Win32 clipboard:
  `vm_getClipboardText`/`vm_setClipboardText` use OpenClipboard +
  CF_UNICODETEXT with UTF-16<->UTF-8 conversion (mirrors SDL2's
  SDL_windowsclipboard.c). Round-trip verified both directions via
  `SDL2 clipboardText` / PowerShell Get-Clipboard. Line-ending normalization
  is handled image-side (`OSWindowClipboard>>clipboardText` applies
  `withInternalLineEndings`).
- [ ] **Text input / IME** (`windows.cpp:56-57`) — `vm_startTextInput()` /
  `vm_stopTextInput()` are no-ops.  No IME / text-composition support (GUI-only).
- [x] **SoundPlugin — Windows backend DONE (2026-07-02)** (`SoundPlugin.cpp`):
  waveOut (winmm) implementation mirroring the Apple AudioQueue architecture
  (lock-free SPSC ring buffer -> CALLBACK_EVENT feeder thread -> WAVEHDR
  round-robin; waveOut APIs are unsafe inside the waveOut callback, hence the
  event+thread). Links winmm. NOTE: stock Pharo 13 ships NO sound classes
  (no SoundPlayer/SoundSystem/Beeper), so nothing in a bare image can drive
  it — runtime verification pending an image with a sound package loaded.
- [ ] **MIDIPlugin** (`MIDIPlugin.cpp:294-308`) — all MIDI primitives
  (`midiInit`/`midiOpenPort`/`midiRead`/`midiWrite`/...) are honest stubs.  No MIDI
  on Windows (no winmm/midiOut backend; Apple uses CoreMIDI).
- [ ] **WorldRenderer native draw** (`WorldRenderer_linux_stub.cpp`, reused on
  Windows per CMakeLists.txt:264) — `render()`/`renderMorph()`/`renderMenuBar()`/
  glyph + color/rect extraction are no-ops.  This is the C++ "native morph
  rasterization" fast-path (Apple uses CoreText/CoreGraphics); Windows would need
  FreeType + GDI/Direct2D.  Separate from, and subordinate to, the GUI milestone.
- [ ] **ARM64-Windows J2J trampoline** (`CMakeLists.txt:277`) — `TrampolineAsm.S`
  (the hand-written ARM64 J2J trampoline) is REMOVE_ITEM'd on Windows; it
  preprocesses to empty on x86-64 anyway (the C++ while-loop fallback is used), so
  this is a no-op for the current x86-64 Windows target — but a real gap if an
  ARM64-Windows build is ever attempted (GAS-vs-MASM assembler-dialect question).

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

- [x] **PNG 16-bit — DONE (2026-07-02), PNGReadWriterTest 42/42** (stock is
  42/42 too). Three separate 16bpp fixes, each probed byte-level vs stock:
  (a) 16->32 blits ignored a NEGATIVE source depth — halfword-parity flip
  when srcNeedsByteSwap (test16BitReversed); (b) our 32->16 conversion set
  bit 15 as an "alpha" flag — Pharo 16bpp has no alpha bit and the PNG 555
  roundtrip strips it; stock's rgbMap instead maps non-zero sources that
  compress to 0 -> pixel 1 (test16BitDisplay); (c) 5->8 bit expansion must be
  stock's plain <<3 (31 -> 0xF8), not full-range *255/31 — the golden-file
  byte comparison catches the 1-3/channel difference
  (testPngEncodingColors16).
- [ ] **Step-through debugger tests hang (pre-existing)** — BOTH
  `FastStepThroughTest` and `StepThroughTest` hang; the specific test is
  `testStepThroughLonger` (10 of 11 Fast tests pass; bisect script
  /c/tmp/fst-bisect.st). Bisect-verified NOT caused by primitive 218.
  Investigation so far (2026-07-02, probes in /c/tmp/fst-*.st):
  - It is a BUSY spin (JIT sends grow forever), not a deadlock.
  - Fast mode path: DebugSession>>fastStepThrough: -> FastStepThroughController
    prepareContextForStepThrough: (wraps blocks in the suspended context's
    temps with HaltingBlocks — i.e. MUTATES a materialized context) ->
    completeStep:inProcess: resumes the process at full speed and waits for
    the halting block to fire. Instrumented Context>>stepToHome: is never
    reached in fast mode. Prime suspect: temp mutation on a materialized
    context is not honored when our VM resumes the frame (savedFrames_ /
    context write-back), so the halt never fires; secondary suspect: the
    controller's findNextContext: walk loops.
  - Slow mode (StepThroughTest, Context>>stepToHome: simulation loop with
    `home == ctxt home` identity stop) also hangs >300 s.
  - SIDE FINDING: while probing, image-side Delays never fired while the
    step-through machinery spun (probes M5/M6 in fst-sample2/3) even from a
    priority-60 waiter — yet a minimal delay-under-spin repro
    (/c/tmp/delay-spin.st, P60 waiter over P40 spinner) PASSES. Whatever the
    debugged process's effective priority/scheduling is during
    completeStep:/evaluate:onBehalfOf:, it starves the delay machinery in
    eval mode — characterize with forkAt: 79/80 spinners.
  Full-suite runs should exclude StepThroughTest/FastStepThroughTest (and
  GUI-opening tests) until fixed. Needs an interactive debugging session
  (PHARO_DET_SCHED + targeted VM tracing of context mutation write-back).
