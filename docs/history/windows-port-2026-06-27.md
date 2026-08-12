# Windows (clang / LLVM-MinGW) port status — archived

> Archived 2026-08-12 from `docs/deferred.md` (lines 1296-2739).
> Frozen as of 2026-07-04. 38 `- [x]` entries against 4 open. There is no
Windows CI job and no Windows source commit since 2026-07-04, so every "FIXED"
verdict below means *fixed as of July*, against a binary that ~250 commits of
VM-core churn have landed on top of untested. The still-open items from this
section were lifted into `docs/vm-compat-bugs.md`; what remains here is record.

---

  volumes is not productive; the durable fix is stock-parity signal
  DELIVERY timing on the JIT path (defer the wake past the currently
  executing statement / to the interrupt checkpoint, like Cog), or
  ephemeron-fire scheduling that cannot land between an explicit
  garbageCollect and the next full send boundary.  Re-check after the
  full-suite run; if WKD fails there too, promote to fix-now.

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
- [x] **ProcessTest>>testResumeAfterBCR — FIXED (2026-07-03); ProcessTest
  46/46.** ROOT CAUSE: returning INTO a dead context (pc nil — e.g. after
  `on: BlockCannotReturn do: #resume` resumed `Context>>cannotReturn:`,
  which nils the failing context's pc) fell into executeFromContext's
  dead-pc branch, which sends cannotReturn: to the DEAD context. For a
  closure context that re-signals BlockCannotReturn; the #resume handler
  is still installed → infinite BCR ping-pong through the image exception
  machinery (the "billions of bytecodes" spin). Stock parity fix (Cog
  internalCannotReturn:): the fd==0 return path now checks the sender's
  pc BEFORE the transition; if dead, it sends cannotReturn: to the
  RETURNING context (a method context → `self error: 'computation has
  been terminated'` → the test's on:Error handler). The returning context
  is deliberately NOT marked dead first, so the Error still finds
  handlers through its sender chain.
- [x] **"WeakArrayTest hangs" — RESOLVED as a misnomer (2026-07-03), and the
  REAL weak-reclaim bug behind it FIXED.** `WeakArrayTest` does not exist in
  Pharo 13 (the "hang" was an OCUndeclaredVariable compile error surfacing as
  silent eval death). Running the ACTUAL 11 Weak* test classes (1007 tests):
  all complete, and the one deterministic failure was
  `WeakSetTest>>testAddIncludesSizeReclaim` (fails only JIT-warm, full-class
  run; passes cold/isolated/JIT-off). ROOT CAUSE (found via the new
  `PHARO_PIN_DIAG=<string>` GC-pin forensics): the J2J save-pool slice
  RESERVATIONS (`j2jPoolCursor_ = base+32` in tryJITActivation and the rj2j
  chain loop) make [base,base+32) visible to forEachRoot's receiver walk
  BEFORE the entries are written — stale receiver oops from earlier released
  chains (here a dead `'123' copy`) got marked live every fullGC and pinned
  weakly-referenced objects. FIX: `clearJ2JSlice()` wipes receiver/closure/
  resume fields at all three reservation sites (matches the convention the
  resume-slice site already used for its uninit-memory case). Verified:
  WeakSetTest 50/50 x3 (was 49/50 every warm run), weak battery green,
  step/exception/process battery green (558 tests), bench identical
  (fib 12ms, ensure 32ms).
- [x] **RESOLVED 2026-07-04: WeakAnnouncerTest at stock parity warm AND
  cold** (4/4 warm x4-in-one-VM runs at 33/34+1EF; see the residual
  sub-item below for the final mechanism — 27e4ca74's weak-cache purge).
  Historical: testWeakObject/testWeakDoubleAnnouncer failed JIT-warm
  (32-33/34, was 31-32; stock 33/34).
  TWO pin mechanisms found with PIN_DIAG (2026-07-03); ONE FIXED:
  - [x] **Mourn-queue starvation FIXED**: activateMethod's deferred
    FinalizationSemaphore check (finalizationCheckAfterGC_) sat AFTER the
    `return` in the tryJITActivation branch — once execution went
    JIT-resident the signal never fired, the finalization process (pri
    79/50, would preempt the pri-40 test) never woke, and mourn-queue
    entries (strong GC roots) pinned their ephemeron keys across every
    subsequent GC. The JIT branch now performs the same one-shot check
    before returning. First warm run now reaches stock parity (33/34).
  - [x] **RESOLVED 2026-07-04 (by 27e4ca74 weak-root GC treatment):
    warm runs now 4/4 at 33/34+1EF = STOCK PARITY** (WeakAnnouncerTest
    x4 in one VM, wa-warm.log).  The residual pin was the ephemeron key
    held via VM cache slots (method cache / IC entries marked STRONG
    during fullGC) — exactly what the weak-root purge removes.
    Historical investigation notes below.
  - [old] **Residual: mourn-queue consumer identity/timing (one flake from
    parity: warm runs 32-33/34, stock 33/34).** The earlier "stale
    context temp" lead DISSOLVED — a post-nil-only probe (skip the
    pre-nil GCs) shows the ONLY post-nil pin is mournQ[0]
    (FinalizationRegistryEntry, key = the subscriber), persisting across
    consecutive GCs. Hardening added en route (all committed, each a
    strict improvement toward Cog's forceInterruptCheck semantics):
    (a) activateMethod's JIT branch now consumes the deferred-signal
    one-shot; (b) all three JIT ExitYield handlers bail to the dispatch
    loop when the one-shot is armed; (c) prim 130/131 set forceYield_
    when arming so the next JIT back edge exits. Result: first warm run
    consistently 33/34 (stock parity), later runs flake 32/34.
    DECISIVE NEGATIVE RESULTS for the residual: `Processor yield` after
    each GC does NOT cure it, and PHARO_GC_EPH_DEBUG shows [SIG-FIN]
    firing with **hasWaiter=0** (the image's Finalization Process — two
    instances, pri 79 and 50 — was NOT waiting on TheFinalizationSemaphore
    at signal time) and far fewer SIG-FINs than arm events. NEXT (fresh
    session): identify exactly which semaphore/delay Pharo 13's
    FinalizationProcess mournLoop blocks on (image-side source:
    FinalizationProcess>>mournLoopWith:, WeakArray class>>
    restartFinalizationProcess) and whether our TheFinalizationSemaphore
    (specialObjectsArray slot) is the same object; if the FP polls on a
    Delay instead, warm runs simply outrun the poll and the fix is to
    signal the CORRECT registered semaphore. Diag probes:
    C:/tmp/weakann-probe*.st, PHARO_PIN_DIAG=weakpinmarker,
    PHARO_GC_EPH_DEBUG=1.
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

- [x] **VM can't compile very large methods (convertStorePop) — FIXED
  (2026-07-02).** ROOT CAUSE: primitiveStringReplace's write-barrier
  probe ran BEFORE the memmove and wrote dst[i]=src[i] for the first
  young slot — on an overlapping self-shift-left (OrderedCollection>>
  removeIndex:) dst[i] IS src[i-1], so the probe clobbered a source slot
  the memmove then read: the first young value in the shifted range got
  duplicated one slot lower and the value there was destroyed — one slot
  corrupted per removal on any TENURED (old) collection whose range held
  a young ref. Hence every hallmark: motion-dependence (the collection
  must be tenured => scavenge timing), heap-phase/form/newspace-size
  sensitivity, JIT-independence, invisibility to write tripwires (a
  legal storePointer), and self-hiding from post-copy fidelity checks
  (the probe corrupted the source before any snapshot). FIX: the barrier
  probe runs AFTER the copy and re-stores a dest slot with its own value
  (data no-op, remembered-set side effect only). VERIFIED: the runner
  fileIn passes 5/5 (JIT on), plus 16/24/32MB newspace and JIT-off;
  collections regression 2062/2062 (OrderedCollection/Array/Sorted/
  String/Dictionary/Interval/Set/StUnifiedProcessor). Our VM now
  compiles the ~380-line runAllTests directly — reference-VM injection
  is no longer required for the runner. (The hunt also fixed, en route:
  Spur min-16 allocation, fullGC scavenge-before-mark, prepareForGC
  pc/class-guard hardening, materializeFrameStack truncation; and left
  a reusable GC forensics toolkit in debug_vars.h.)
  ORIGINAL ENTRY + full investigation log follows for reference:
  surfaced by
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
      the replay visible. Replay-mechanism candidates tested and
      EXONERATED so far: (a) ctxSynced incremental-materialization skip
      (PHARO_MAT_FULL_RESYNC=1 still fails); (b) GC-time context pc
      staleness (prepareForGC now refreshes pc for BOTH saved frames and
      the current frame's materialized context — kept as hardening, no
      cure); (c) materializeFrameStack truncation — REAL BUGS FIXED as
      hardening: temps were capped at 32 (methods encode up to 63),
      expression stacks >100 items were silently skipped, and contexts
      were fix-sized at 6+temps+32 so deeper operand stacks silently
      dropped writes via storePointer's bounds check. Contexts are now
      sized from the frame's ACTUAL expr/operand depth and a
      [CTX-CAPACITY] warning logs any residual shortfall (it never fires
      in the repro, so this wasn't the repro's bug either). REMAINING
      REPLAY SUSPECTS: executeFromContext pc->ip rebuild path (audit the
      +1/-1 vs the 1-based pc-at-next-unexecuted-bytecode convention, and
      WHICH frames get rebuilt vs left as heap contexts on switch-back);
      the to:do:-inlined loop-state (limit/index as stack items) across
      materialize/rebuild; and the possibility that the replay unit is a
      whole BLOCK re-invocation — REFUTED: conversion activation counts
      are IDENTICAL in the aligned FAIL/PASS trace prefixes
      (convertStorePop 19/19, removeIndex: 19/19, remove: 20/20,
      replaceNode: 20/20; prefixes line-identical to the divergence).
      IDENTICAL Smalltalk operations produced DIFFERENT array content
      (+1 element, triplet->closure-run substitution) — so the delta is
      a VM-level effect inside one of those identical operations. growAtLast EXONERATED
      (srcLive=1, young OC, ocArrIsSrc=1 — healthy fresh-young grow; the
      [P105] log now prints srcLive/srcOld/oc/ocOld/ocArrIsSrc via
      ObjectMemory::isLiveYoung). THE SHARPENED CONTRADICTION: the run's
      slots were appended as DISTINCT nodes (zero DUP-APPEND fires — a
      whole-prefix scan per append), no instrumented writer ever
      equalized them, yet they READ equal at first-shift time. Therefore
      the reads target different memory/object than the appends wrote:
      an OBJECT-IDENTITY SWITCH between append-time and shift-time for
      the OC or its array — while the error-time dump shows a CONSISTENT
      seq->OC->array chain. Identity-switch test RESULT (seqids.txt
      hook in activateMethod, logs seq/oc/arr at visitSequence: +
      convertStorePop: entries): the failing sequence's seq->OC->array
      chain is IDENTITY-STABLE from visitSequence: entry to the error
      (arr matches the error dump exactly), and exactly 8 conversion
      assocs precede the failure — the visitor recorded a 9th pair whose
      pop the SAME array no longer contains. So the content mutated
      BETWEEN the do: iteration and the conversions, inside ONE
      visitSequence: activation, whose only intervening code is the
      retToFix conversions (convertReturn: replace+remove -> shifts) and
      the earlier storePop conversions — none of which can transmute a
      [PushLiteral,StoreRemoteTemp,Pop] triplet into extra closure-ref
      copies. NEXT (surgical): extend the seqids hook to DUMP THE FULL
      ARRAY ELEMENT BITS at OCIRSimpleOptimizerVisitor>>visitSequence:
      ENTRY (capped to the last few sequences), then diff that entry
      snapshot against (i) the state at first convertStorePop and (ii)
      the error dump — this pins the mutation to [entry..retToFix] vs
      [retToFix..storePop] vs mid-conversions, at which point the
      handful of candidate operations can be single-stepped. Note
      convertStorePop's arg-1 in seqids is the ASSOC (mislabeled seq) —
      the assoc addresses usefully enumerate the recorded pairs.
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

### Full-suite re-baseline (2026-07-03, after the debugger/unwind + GC-pin fixes)
**460 classes / 7375 tests: 7281 pass, 3 F / 40 E ≈ 98.7% raw — and the
true VM-attributable count is ONE test.** Breakdown of the 43 deviations:
27 Athens/Cairo (known graphics-FFI gap — no cairo DLL on Windows; same
category as every prior run), 15 Cly*Query "failures" that are EXPECTED
FAILURES miscounted by the suite runner (ClyFilterQueryTest 7EF,
ClyAsyncQueryTest 7EF, ClySemiAsyncQueryResultTest 1EF — all pass clean
standalone), and 1 real singleton:
- [x] **DebugPointTest>>testTranscriptDebugPoint — NOT OURS (2026-07-03):
  the stock reference VM fails it too** (isolated suite run on stock:
  1 failure). Image-level defect in headless mode; zero VM-attributable
  deviations remain in the 7375-test suite outside the Cairo category.
The previously-hanging classes (StepThroughTest, FastStepThroughTest,
ProcessTest, the Weak* family) are now runnable and green. The run went
460 classes deep vs the previous 349-class abort. NOTE: the suite VM
WEDGED post-completion (results + completion marker written, then the
exit path idled at ~800 steps/s until killed) — eval-mode shutdown after
the full run, worth a look if it recurs.
RUN #7 (2026-07-03, all fixes incl. audit batch + ring fix): **COMPLETED
ALL 2047/2047 CLASSES — official summary 27967 tests: 27360 pass / 50 F /
377 E / 155 skip / 25 timeout = 97.8%** (BATCH COMPLETE written; ~8500s).
Zero firings of the new loud diagnostics (MAT-CAP / NLR-SAVE-OVERFLOW /
GC-PLAN-OVERFLOW / CTX-CAPACITY / STUCK-GUARD) across the whole catalog —
the audit fixes are quiet at scale.
**exitSuccess "wedge" RESOLVED as a slow exit tail, not a hang**: after
BATCH COMPLETE the VM idles minutes at ~1M steps/s while the runner's
per-test watchdog machinery winds down (STATE-DUMPs name the actors:
DelayMicrosecondTicker>>waitForUserSignalled:orExpired: at P80,
Process>>endProcess at P40, P60 ifFalse: idle loops), then prints
"Test Complete" and exits normally.  The earlier "wedge" kills at ~30 min
were premature terminations of this tail.  REMAINING REAL ITEM:
- [x] RESOLVED 2026-07-04: **Teardown SEGFAULT after "Test Complete"**
  was NOT a static-destructor race — [WIN-CRASH] backtraces (symbolized
  with llvm-addr2line against the crashing binary) exposed a
  use-after-free FAMILY in the TFFI teardown paths, all reachable when a
  test error abandons an in-flight callback invocation (its nested
  interpreter loop then runs the rest of the suite, including tearDowns
  that free FFI resources still on the C stack):
  (a) callbackClosureHandler's fell-out/timeout paths re-read
      cif->rtype after primitiveUnregisterCallback freed the
      CallbackInfo -> crash in the memset sizing.  FIX: capture retSize
      at handler entry.
  (b) primitiveFreeDefinition freed a function cif while the OUTER
      callout using it was still inside ffi_call -> crash in
      tffiConvertReturnValue (cif->rtype == NULL, faultAddr 0xA).  FIX:
      cif graveyard — free defers to a checkpoint drain gated on
      callbackDepth_==0, no xtcb pendings, no worker tasks in flight
      (g_tasksInFlight), entry >5s old.
  (c) TestLibrary retained a callback thunk pointer and invoked it after
      ffi_closure_free -> executing freed trampoline (rip==faultAddr in
      non-code memory).  FIX: unregister marks CallbackInfo->unregistered
      and LEAKS thunk+cif+struct (a C library can hold the thunk
      forever); the handler answers zeroes for unregistered callbacks.
  Plus exit-latency + residual-destructor hardening: pharo_xtcbShutdown
  unparks workers blocked on forwarded callbacks (exit was serializing
  behind their 120s timeouts -> exit code 124), a shutdown guard stops
  re-parking, immortal (leaked) cross-thread statics in xtcb / the
  worker registry / SocketPlugin, and a bounded drain for detached DNS
  lookup threads.  VERIFIED: TFCallbacksTest solo exit-loop 8/8 exit 0
  (was ~2/6 with 139s and 124s); battery + x4 gauntlet green.

RUN #6 (2026-07-03, pre-audit binary): **2043 of 2047 classes —
27956 tests / 27257 pass = 97.5%**
(timeout-killed 4 classes from the end at 9000s; the per-test watchdog
runner is the wall-clock bottleneck, not hangs). 52F/371E dominated by
the known categories (Cairo/graphics, Zn network sandboxing, runner
expected-failure miscounts — FL*/Cly* families pass standalone).
NEW-territory triage (~1100 classes never run before on our VM produced
only a handful of real singles): EDDebuggingAPITest = suite-order
artifact (27/27 standalone); OCClassBuilderTest = stock fails identically
(1 error, not ours); REAL our-VM items, all niche:
- [x] RESOLVED 2026-07-03 (27e4ca74): ObsoleteTest>>testFixObsoleteSharedPools
  (was ours 2/3, stock 3/3) — implemented the weak-root plan below
  verbatim: RootScope{All,StrongOnly} on forEachRoot, markPhase marks
  StrongOnly on the true fullGC/sweep path, purgeDeadCacheRoots() voids
  dead cache slots after the mark fixpoint (+ rebuildMethodMap so no
  stale key false-hits a recycled address), scavenge keeps all roots
  strong.  ObsoleteTest 3/3 x4-in-one-VM; extended Weak*/Finalization/
  TFFI/Process battery green; benchFib 12ms.  PHARO_GC_PURGE_LOG=1
  prints per-GC purge counts.  Historical plan (implemented):
  1. forEachRoot gains a scope flag (All | StrongOnly).  The weak group =
     method-cache entries, IC method/selector slots, JIT count-map +
     failed-map keys, JITMethod header compiledMethodOop.  markPhase
     (ObjectMemory.cpp:3816 area) visits StrongOnly; the pointer-update
     pass (line ~2032 area) keeps All so survivors get moved pointers.
  2. New hook after processMarkStack + ephemeron fixpoint (mark bits
     final, BEFORE plan/compact): purgeDeadCacheRoots() — void unmarked
     method-cache entries, reset IC slots with unmarked method/selector,
     tombstone unmarked count/failed-map keys, invalidate JITMethods
     whose compiledMethodOop died (eviction reclaims later).
  3. Keep the pre-compact SCAVENGE treating all roots strong initially
     (young objects referenced only by caches must tenure, not dangle);
     revisit after the fullGC path is proven.
  Payoff beyond the test: dead classes/methods stop being pinned by VM
  caches entirely (IDE class-redefinition memory leak).
- [x] RESOLVED (verified 2026-07-04): NetNameResolverTest>>testLocalHostName
  passes (1/1; suite 1 pass + 1 skip) — fixed by the 2026-07-03 hostname
  prims (primHostNameSize/Result; localHostName answers the machine name
  'wohl25', isConnected true).  Entry predated that fix.
- [x] RESOLVED (verified 2026-07-04): MicTextPresenterTest 21/21 incl.
  testHugeFontIsHuge (sole implementor — the "Microdown twin" no longer
  exists as a separate test).  Fixed by the Cairo runtime stack
  (de30e70b).
AUDIT BUILD VALIDATION (bd306e47): battery 626/626 green (weak/step/
exception/process/collections), TFFI 75/75, fib 12ms & ensure 31ms
unchanged, and the MethodMap tombstone fix cut zone-full failed-compiles
from 22726 to 620 (the residue = the legit uncompilable negative-cache).
The one [STATE-DUMP] fired during the Zn phase named a P60 idle-wait
loop (#ifFalse: spin at fd=0) — the step-rate-collapse diagnostic works;
the exitSuccess wedge itself wasn't reached (timeout kill first).

RUN #3 (2026-07-03, after the TFFI-worker + diagnostics commits):
**483 classes / 7874 tests / 7586 pass, 6F/40E — deepest run yet**
(killed by the 5400s outer timeout mid-Fuel, not by any hang; the
per-test watchdog runner is the bottleneck). All 46 deviations are the
SAME known set: 27 Cairo, 15 Cly* + 3 FL* runner-miscounted EXPECTED
FAILURES (all pass standalone: FL* 76/79+3EF each), 1 DebugPointTest
(stock fails too). Zero Weak* deviations at suite scale — the GC-pin
fixes hold. True VM-attributable deviations outside Cairo: ZERO.
The [STATE-DUMP] wedge diagnostic is armed but this run never reached
the post-completion phase (timeout kill), so the wedge remains
un-diagnosed — rerun with TIMEOUT=9000 to get a completing run.

### RESOLVED 2026-07-03 (27186475): the InLoop stall was the aging clock
handleForceYield's aging-based preemption measured WALL time since the
tracked process's last aging event; idle interludes never update the
tracker, so a process that slept ~1s was INSTANTLY aged out to a lower-
priority process at its first yield check after waking (zero bytecodes —
the empty-wake signature).  Fixes: slice-clock restart on reschedule,
grace-undo exempts P80 (the Delay ticker), and grace requires a fresh
full threshold after each window (caps grace duty cycle at ~50% — was
CONTIGUOUS, starving P41-79 watchdog machinery).  TFUFFICallbackTest:
13/13 STOCK PARITY (baseline re-measured 8/13); x2 in one VM both 13/13.
- [x] RESOLVED 2026-07-03 (dcacc401 + e9a7e984): the repeat-run wedge had
  TWO stacked mechanisms, both fixed:
  1. dcacc401 (exactly-once hand-out): primitiveReadNextCallback returned
     the TOP stack entry on EVERY read while the image's TFCallbackQueue
     treats the callback semaphore as a queue (one signal == one item; nil
     when empty).  Excess/duplicate signals made the queue process re-read
     the SAME still-executing invocation and fork duplicate executors —
     the allocation-heavy P70 livelock sampled above.  Now each entry is
     handed out exactly once (FIFO), extra reads answer nil, and timed-out
     worker callbacks are reaped lazily via xtcb::g_dead.
  2. e9a7e984 (requeue order): after the exactly-once fix the wedge
     STILL fired — enterInterpreterFromCallback's post-return yank
     re-queued the mid-cleanup executor at the BACK of its run queue,
     landing it BEHIND the same-priority resumer forked by
     interpriorityYield:'s `[p resume] fork. p suspend` (cleanup is far
     longer than the 500-step cooldown).  Spent resume -> eternal suspend
     -> stackProtect mutex held forever -> P70 queue wedged.  Proof:
     gauntlet6.log pc=99(pre-suspend)-vs-pc=101(post-suspend) resume
     signature.  Fix: putToSleepPreempted (front) — involuntary
     displacement never reorders same-priority processes (Cog rule).
     Plus primitiveExitCriticalSection Cog parity (strict >, front).
  Verified: x4 gauntlet 5/5 clean (4x13/13 each, exit 0, ~75s total),
  battery green twice (InCallbacks 2x36/36, Derived 12/12, Weak*,
  Process 46/46, Semaphore 18/18, StepOver, Delay, FFICallback*).
  TFCallbacksTest — **RE-VERIFIED FULLY RESOLVED 2026-07-07 (cont.):** on
  the current VM the suite runs **8 passed + 2 skipped, 0 failures, 0 errors**
  (stable across a single run, twice-in-one-VM order check, and per-test
  enumeration). testSingleCalloutDuringCallback PASSES under BOTH runners
  (TFSameThreadRunner + TFWorker). The 2 skips are testCallbackNotRespectingLIFOOrderFailsReturn,
  whose source hardcodes `true ifTrue: [ ^ self skip ]` — an unconditional
  UPSTREAM self-skip, identical on stock Cog (NOT VM suppression). CORRECTION
  to the "baseline-identical / stock parity" framing: the old 1/8+3F+4E were
  NOT stock-parity — they were 5 GENUINE our-VM defects that were FIXED
  (TestLibrary fixture arg+1 semantics, primitiveReleaseWorker abort-before-join,
  XTCB-DEAD-SKIP dead-entry hand-out, reentrant worker task-queue servicing,
  pendingXtcbAdoption_ drain in nested enterInterpreterFromCallback) — which is
  why the tests pass today. So TFCallbacksTest has NO remaining our-VM defect;
  the sole residual (2 self-skips) is legitimately upstream test design.
  testSingleCalloutDuringCallback: PASSES (no longer 1F), and
  TFUFFIConcurrencyTest-UsingWorker marginal pacing:
- [x] TFUFFIConcurrencyTest>>testConcurrentlyCompiling(UsingWorker) —
  FIXED 2026-07-05 (ARM), commit 3940b62c.  The quiet-machine profiling
  found it was NOT image-side compile cost: warm worker callouts ran
  26-50ms EACH (500x = 13s) while [TFLAT] tracing showed the VM
  transport (enqueue -> worker signal -> pending drain) completing in
  20-40us and the waiter woken immediately.  XFER traces caught the
  wake being UNDONE: the 2ms heartbeat force-yield hands the CPU DOWN
  to the P10 idle loop (wakeLowerPriorityProcess), and the only route
  back UP was checkForPreemption at the every-1024-step periodic — but
  a process sleeping in prim 230 executes ~10-20 bytecodes per 10ms
  quantum, so the ready caller starved ~1s until the Delay scheduler's
  next tick.  FIX: prim 230 runs checkForPreemption at entry + after
  the sleep, gated to the idle band (pri <= 10) — ungated it
  resurrects the P80<->P60 voluntary-yield bounce (whole batch
  TIMEOUTs).  Result: 500 callouts 13035ms -> 95ms; the UsingWorker
  test 10s-FAIL -> 995ms PASS (same-thread speed).  Validation:
  20-class scheduler batch 1445 P / 0 F; TFCallbacksTest 8/8+2S.
  The Windows 7-8.4s core-loop numbers likely had the same starvation
  component — re-measure there before blaming compile cost.

### TFCallbacksTest (TF-plain suite; stock 8/8+2S, ours was 1/8+3F+4E)
LARGELY FIXED 2026-07-04; four independent root causes found and fixed:
1. TestLibrary fixture semantics: singleCallToCallback must be
   `return cb(value + 1)` (arg+1, result passed through) — our clean-room
   `cb(value) + 1` satisfied the UFFI a+1 test by coincidence and broke
   every TF-plain single/reentrant test (3F).  All same-thread variants
   pass now.
2. primitiveReleaseWorker joined a worker whose thread was parked in the
   forwarded-callback wait -> whole VM froze for the 120s timeout
   (old-session test releases mid-callback by design).  Fix:
   xtcb::abortCallbacksForWorkerThread before the join (per-thread
   pendings, timeout-path cleanup).  Old-session(worker) passes solo.
3. The aborted invocation stayed BURIED on callbackContextStack_ and the
   FIFO hand-out gave it to the image next suite (args point into the
   exited worker's stack -> error cascade across suites).  Fix: hand-out
   scan skips g_dead entries ([XTCB-DEAD-SKIP]); primitiveCallbackReturn
   also lazily pops dead TOP entries before reading (returning a live
   invocation under a dead top would complete the wrong vmcc).
4. Reentrant callouts during a forwarded callback deadlocked: the target
   worker's only thread was parked in OUR cv-wait.  Fix: the parked wait
   now SERVICES the worker's own task queue (Worker::runOneTask; nested
   ffi_call frames on the worker's C stack — stock pThreadedFFI's
   reentrancy model).  testReentrantCalloutsDuringCallback(worker) passes
   solo (7-deep chain verified in trace).
- [x] RESOLVED 2026-07-04 (5th root cause; STOCK PARITY 8/8+2skip):
  the in-suite order dependence was a missing adoption drain — the
  nested enterInterpreterFromCallback loop processed pending signals and
  the timer after each 1000-step batch but NEVER pendingXtcbAdoption_
  (only the main interpret() checkpoint did).  The same-thread
  old-session test abandons its invocation BY DESIGN, so its nested loop
  hosts all subsequent execution — and every worker-forwarded callback
  after that sat in the xtcb queue forever (XTCB-ADOPT count 0 for the
  rest of the run; minimal pair old-session(same-thread) ->
  singleCallout(worker) reproduced it deterministically, pair2.log).
  The nested loop now drains adoption too.  VERIFIED: TFCallbacksTest
  8/8+2skip x2-in-one-VM even after the full battery + 4 UFFI gauntlet
  iterations in the same VM; UFFI 13/13 warm x2 (the warm UFFI in-suite
  flake — worker halves erroring on second runs — is gone with it).

### TFFI v2 residual notes (historical; resolution above)
NOTE 2026-07-03 (post e9a7e984): the "woken waiter runs zero bytecodes
then re-suspends" mystery below is explained by the callback-return
requeue mechanism — the yanked mid-cleanup executor (or its
interpriorityYield: dance partner) WAS the "empty wake": a process
resumed into the fork/suspend window executes `suspend` immediately,
which from the trace's viewpoint is a wake that consumes a signal and
runs ~zero bytecodes.  With order-preserving requeue the one-behind
service pattern is gone (x4 suite ~75s total vs minutes).
v2 forwarding landed 2026-07-03 (commit 6280deb8): worker/native-thread
callbacks forward to the VM thread (xtcb queue -> periodic-check adoption
-> image callback semaphore -> primitiveCallbackReturn wakes the parked
thread), plus interruptible idle (prim 230 condvar wait, wakeIdleSleep
from signalExternalSemaphore/adoption) and 25 TestLibrary callback
fixtures (NB: scripts/build-windows.sh does NOT rebuild TestLibrary.dll —
run `ninja TestLibrary` in build-win).  Recovered: InCallbacks 36/36 +
derived 12/12 + Concurrency 2/2 + CallbackTest 12..13/13 (incl. the
raw-native-thread callbackFromAnotherThread test).  ONE residual vs
stock's 13/13:
- [x] RESOLVED (aging-clock fixes 27186475/d91c0df8 + requeue-order
  e9a7e984; verified 2026-07-04): TFUFFICallbackTest is 13/13 in-suite
  across 20+ full-suite executions (every x4 gauntlet run 2026-07-03/04)
  — the order-dependent InLoop(UsingWorker) timeout no longer occurs.
  The one-behind service pattern is explained by the requeue mechanism
  (see the #14 resolution note above).  Historical evidence chain:
  [old] testCallbackInLoop(UsingWorker) trips TestTookTooMuchTime when the
  OTHER UsingWorker tests run first (standalone: 1.05s PASS).
  EVIDENCE CHAIN (2026-07-03, now nailed to one scheduling step):
  * New tool: PHARO_STATE_DUMP_PERIOD_MS=N — C-side wall-clock scheduler
    sampler (image-side probes HEAL the stall by existing: a P79 Delay
    monitor, a P30 busy spinner, even one extra Stdio print all flip it
    to PASS — classic scheduler Heisenbug).
  * Combined PHARO_STATE_DUMP_PERIOD_MS=400 + PHARO_CALLBACK_DEBUG=1 +
    PHARO_DELAY_DEBUG=1 + PHARO_SEM_SIGNAL_TRACE=1 on the repro shows,
    inside every ~1s gap (post-kill cadence; 5-9s pre-kill matching the
    SUnit time-limit Delay window):
      XTCB-ADOPT (signal sent via ring)
      -> NO [SEM-SIGNAL] line = synchronousSignal found NO WAITER on the
         callback semaphore (went to excessSignals) — the P70 queue
         process ('Callback queue', TFCallbackQueue forkCallbackProcess)
         was NOT in semaphore-wait at that instant
      -> XFER idle(P10) -> P70 (same process!) -> back to idle in us,
         WITHOUT a CALLBACK-READNEXT — it woke, did something tiny that
         is not the read (suspect: finishing the PREVIOUS invocation's
         `stackProtect critical:` release, or the on:Exception fork:
         wrapper), and re-entered `semaphore wait`, CONSUMING the excess
         signal at wait-entry??  (If wait-entry consumed the excess it
         should proceed to the read — it does not.)
      -> the VM idles ~1s until the next DELAY-FIRE (a 1s-cadence image
         Delay); the ticker transition reschedules and ONLY THEN the P70
         process does CALLBACK-READNEXT and the round-trip completes in
         2ms.
  * [SEM-WAIT] trace added to primitiveWait (2026-07-03; both sem trace
    caps raised to 3M under PHARO_SEM_SIGNAL_TRACE=1).  Uncapped gap
    dissection (stall4.log pattern):
      ADOPT -> [SEM-SIGNAL] on the callback sem WITH the P70 queue proc
      as waiter (identity confirmed) -> XFER idle->P70 -> XFER P70->idle
      within us, and BETWEEN THOSE the process executes NO traceable
      primitive (no READNEXT, no [SEM-WAIT] re-entry) -> 1s DELAY-FIRE
      -> ticker -> P70 does READNEXT, and its next queue wait shows
      excess=1 (one-behind pipeline signature).
  * TWO live hypotheses for "woken waiter runs zero bytecodes then
    re-suspends":
    (a) the wake transfers to a process whose suspendedContext resume
        path re-suspends WITHOUT re-running the wait prim (check
        executeFromContext for a process suspended inside
        primitiveWait's putToSleep);
    (b) an immediate counter-preemption bounces it back to idle before
        its first bytecode (check checkForPreemption/forceYield right
        after the drain's transferTo).
    Either way the signal is consumed by the empty wake and the
    callback is serviced one Delay-cycle late.
  Also: `timeLimit: 100 seconds` on the case is NOT honored in-suite
  (defect at exactly 10.03s).  PHARO_DET_SCHED wedges the repro entirely
  (det-sched starves cross-thread wakes — conflict by design).  Repro:
    pre := TFUFFICallbackTest buildSuite tests select: [:x |
      (x printString includesSubstring: 'UsingWorker') and: [
      (x printString includesSubstring: 'InLoop(') not]].
    pre do: [:x | x run].
    "now testCallbackInLoop(UsingWorker) -> 10s defect"

### Silent-cap audit residue (2026-07-03; confirmed-but-deferred findings)
The 41-agent audit (commit bd306e47 fixed the actionable set; e631d780
fixed the ring-size regression it introduced) confirmed these additional
findings, deferred with rationale — all medium/low, none reachable from
green-suite workloads today:
CLOSED 2026-07-04 (loud-not-silent batch; every disposition below):
- [x] storePointer OOB: now loud ([STORE-OOB], first 50) — dropped
  stores are visible (zero firings across battery+gauntlet).
  fetchPointer OOB: CLOSED WITH RATIONALE, no tripwire — nil-answer is
  a relied-upon API semantic (classNameOf-style probes read optional
  slots past shorter shapes, e.g. slot 6 on a 6-slot Metaclass; a
  tripwire attempt logged 50 false positives per run).  Comment added
  at the fetch site so nobody re-adds it.
- [x] followForwarded 10-hop cap: loud when the cap is hit with the
  chain still forwarded ([FWD-CHAIN-CAP], first 20).
- [x] findGlobal heuristic caps: CLOSED WITH RATIONALE, no code change —
  the caps are skip-and-continue guards over a heuristic scan for
  VM-internal well-known globals; a miss degrades to nil which every
  caller handles (several probe for optional globals, so a
  nil-result tripwire would spam).
- [x] updatePointersAfterCompact new-space scan terminator: loud
  ([NS-SCAN-TERM], first 10) — objects beyond the stop keep stale
  pointers, so firing = real corruption evidence.
- [x] unblockStuckSnapshotCallers 10000-object cap: loud once per run
  when the cap is reached.
- [x] Corruption-tripwire log caps (8x `< 5` sites: SP-CORRUPT family,
  BLOCKRET-FAIL): now first-5 + every-4096th — mid-run corruption storms
  stay visible without flooding.
- [x] setSenderSafe 200-deep cycle walk: CLOSED WITH RATIONALE, no code
  change — deliberate HELPER_SENDS cycle-breaker that is already loud
  (CYCLE-BREAK log) when it fires.
- [x] bvClosureSaveStack_ 256-entry guard: loud ([BV-SAVE-GUARD], first
  20) — a trip means an inlined block ran with the caller's closure.
- [x] Heuristic object-shape probes (primitiveGetNextEvent /
  InputSemaphore2 / CalloutToFFI literal scans): CLOSED WITH RATIONALE,
  no code change — the probed shapes are VM-injected/fixed structures we
  control; a "proper layout lookup" adds image-version fragility for
  zero observed defects on the green suite.
- [x] TFFI callback-polling nil/0 interceptions: REMOVED (3 sites) — the
  real callback machinery landed (primitiveReadNextCallback + v2
  forwarding), so lookups that reach those scanners now fail loudly
  instead of silently answering "empty queue".

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
- [x] **TFFI worker runtime (threaded FFI) — v1 DONE (2026-07-03,
  commit e823b724); cross-thread CALLBACKS remain (v2).**
  RESULT: the worker halves of every non-callback TFUFFI suite are at
  parity with same-thread — BasicTypeSize 48/48, Structures 22/22,
  BasicTypeMarshalling 34/36 (2 pre-existing BOTH-halves testUnrefPointer
  failures), DerivedTypeMarshalling 32/34 (2 pre-existing both-halves
  testMarshallingOOPIsSameObject errors), FunctionCall 5/5, MethodRegistry
  2/2, DerivedTypeSize 2/2 — ~120 worker tests recovered. FFI battery
  regression-clean (CalloutBuilder 10/10, Parser 45/45, ExternalStructure
  12/12, ExternalArray 7/7, TFStruct 6/6). The return-value conversion is
  now shared (tffiConvertReturnValue) between same-thread and worker paths.
- [x] LANDED 2026-07-03 (6280deb8; hardened by e9a7e984 + 56997740):
  **TFFI v2 cross-thread callback forwarding** — InCallbacks 2x36/36,
  Derived 12/12, TFUFFICallbackTest 13/13 verified repeatedly at suite
  scale.  Design notes below are the implemented plan:
  [old] the *InCallbacks*
  suites (36+12 errors), TFUFFICallbackTest worker half (9 errors), and
  TFUFFIConcurrencyTest>>testConcurrentlyCompiling (1) need callbacks
  invoked FROM the worker thread: the worker must block while the VM
  thread runs the callback and resumes it (stock pThreadedFFI's callback
  forwarding). SCOPED FURTHER 2026-07-03: ALL FIVE image-side callback
  prims already exist and work same-thread (primitiveRegisterCallback/
  Unregister/CallbackReturn/ReadNextCallback/InitilizeCallbacks —
  registrations at Interpreter.cpp ~19884; pending-queue machinery at
  ~4959-5109 incl. pendingCallbackReturn_). v2 therefore reduces to: in
  the libffi closure thunk (FFI.cpp/vmCallback.h), detect "not on the VM
  thread" → enqueue the invocation into the EXISTING pending-callback
  queue + signal its semaphore → block the worker thread on a
  condition variable until primitiveCallbackReturn (VM thread) stores
  the return value and wakes it. Image machinery (TFCallbackQueue's
  callbackProcess) already drains the queue. Sources in
  C:/tmp/tfcb-sources.txt. Original scoping notes follow:
  SCOPED 2026-07-03 (image protocol fully mapped; sources in
  C:/tmp/tfw-sources*.txt):
  - 4 named prims: `primitiveCreateWorker` (receiver=TFWorker; create
    thread+task queue, store ExternalAddress handle into receiver ivar 0
    `handle`); `primitiveWorkerCallout` (recv worker; args: TFExternalFunction,
    plain Array of Smalltalk args — SAME shape as primitiveSameThreadCallout —
    plus external-semaphore INDEX; enqueue heap task, return ExternalAddress
    of the task); `primitiveWorkerExtractReturnValue` (recv worker; arg task
    address → return-value Oop, free task); `primitiveReleaseWorker`
    (stop+join+free).
  - Completion signal: worker thread → `signalExternalSemaphore(index)` —
    the SAME mechanism the socket I/O thread already uses (thread-safe).
    Image side registers the Semaphore via registerExternalObject: and waits.
  - Implementation: refactor primitiveSameThreadCallout's libffi
    marshalling (args + return-Oop conversion) into helpers writing into
    malloc'd task storage (alloca crosses threads today); worker loop:
    pop → ffi_call → done flag → signal. Callbacks from the worker thread
    are OUT OF SCOPE for the first cut (the TFUFFI basic-type suites don't
    need them; TFWorker callback tests may remain red).

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
- [x] **SDL2 / Morphic display — WORKING ON-SCREEN with input
  (status corrected 2026-07-04; the header below was stale).**  All three
  "WHAT'S MISSING" parts landed and were verified interactively with
  screenshots: part 2 = Win32DisplaySurface HWND + GDI StretchDIBits
  (PHARO_GUI_WINDOW; docs/images/windows-gui-onscreen-window.png), part 3
  = wndProc -> gEventQueue -> stub_SDL_PollEvent event injection
  (verified: menu clicks, Playground typing, debugger-on-DNU —
  docs/images/windows-gui-menu-click.png / windows-gui-debugger.png),
  part 1 = interactive run mode (--interactive, non-eval).  REMAINING
  GUI polish tracked separately: IME/text-composition (below) and
  making the on-screen mode the packaged default (milestone 5).
  Historical architecture notes:
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
- [x] **Sampling profiler — Windows backend DONE (2026-07-03, commit
  0f3bf93c).** Sampler thread wakes every PHARO_PROFILE_INTERVAL_US
  (default 1ms), gates on the VM thread's consumed CPU time via
  GetThreadTimes (emulating ITIMER_PROF's don't-sample-while-idle), and
  records via the same tolerated-race single-word read of method_ the
  POSIX SIGPROF handler uses. Runtime-verified: PHARO_PROFILE=1 with
  interpreted 32 benchFib → 381 samples, benchFib top at 19.9%,
  dropped-when-idle working. (Cosmetic: some entries print "?>>?" when
  class/selector resolution fails — same as POSIX.)
- [x] **SIGSEGV crash recovery — CLOSED BY DESIGN (2026-07-04)**
  (`test_load_image.cpp` `pharoWinCrashHandler`): dump-then-crash is the
  intended end state.  A VEH cannot safely unwind arbitrary faults, every
  Windows fault family seen so far was memory corruption where "recovery"
  would mask the disease, and the dump has repeatedly been the root-cause
  tool (the 2026-07-04 teardown-segfault family was solved entirely from
  its symbolized backtraces).  Keep: diagnostic dump yes, recovery no.
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
- [x] **POSIX file ownership — CLOSED BY DESIGN (2026-07-04)** —
  `chown`/`lchown` return ENOSYS (Windows has no POSIX uid/gid model); the
  two calling primitives turn that into a primitive Failure, which the
  image handles.  Honest-failure is the correct terminal state; mapping
  onto Windows ACLs would fake semantics stock doesn't provide either.
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
- [x] **ARM64-Windows J2J trampoline — CLOSED, NOT APPLICABLE to the x86-64
  target (2026-07-04)** (`CMakeLists.txt`) — `TrampolineAsm.S` is
  REMOVE_ITEM'd on Windows and preprocesses to empty on x86-64 anyway (the
  C++ while-loop fallback is used).  Re-open ONLY if an ARM64-Windows port
  is attempted (GAS-vs-MASM assembler-dialect question).

### Memory
- [ ] **Old-space heap commit** — `win_mman.h` `mmap` does
  `MEM_RESERVE|MEM_COMMIT` for the whole (~4 GB) reservation up front, charging
  it all to the Windows commit limit (pagefile) — though demand-zero means no
  physical RAM until touched. macOS/Linux rely on lazy overcommit. A
  fault-driven incremental-commit handler would lower the commit charge (the
  config requests 4 GB; if a machine's commit limit is tight this could fail
  allocation). `madvise(MADV_DONTNEED)` -> `MEM_RESET` (RSS hint) is a faithful
  analogue.
  DESIGN NOTE (2026-07-04 assessment; deliberately NOT implemented now):
  prefer COMMIT-AHEAD IN THE ALLOCATION SLOW PATH (when the bump pointer
  crosses a committedEnd_ watermark, VirtualAlloc(MEM_COMMIT) the next
  64MB chunk in the existing OOM/GC-trigger branch) over a fault-driven
  VEH handler: a commit-on-fault VEH would silently absorb wild-pointer
  writes anywhere in the 4GB reservation, destroying exactly the crash
  diagnostics that solved the 2026-07-04 teardown-segfault family.
  Benefit only materializes on commit-limit-constrained machines; risk is
  in the allocator's most safety-critical invariants — schedule as its
  own milestone with a full-suite gate, not as a drive-by fix.

### RNG
- [x] **arc4random_buf — UPGRADED 2026-07-04** (`win_posix_compat.h`) —
  now `BCryptGenRandom(BCRYPT_USE_SYSTEM_PREFERRED_RNG)` (the documented
  Windows CSPRNG; links bcrypt), removing the dependence on which RNG the
  C++ runtime wires into `std::random_device` (libstdc++-on-MinGW
  historically made it DETERMINISTIC).  random_device remains only as a
  never-observed failure fallback.

### Packaging
- [x] **Authenticode signing + Windows installer — DONE 2026-07-04**
  (owner decision: reuse the `z80cpmw-public` Trusted Signing profile —
  the cert subject is the personal `CN=Aaron Wohl`, not product-named).
  `packaging/windows/build-installer.ps1` stages build-win into a dist
  layout (exe renamed to `iospharo.exe` — binary is name-agnostic,
  verified), signs `iospharo.exe` + `TestLibrary.dll` with the z80cpmw
  signing kit (signtool + Azure.CodeSigning.Dlib, SP creds, RFC-3161
  timestamp via timestamp.acs.microsoft.com), builds
  `packaging/windows/iospharo.nsi` with makensis (per-user install to
  `%LOCALAPPDATA%\Programs\iospharo`, HKCU, no UAC; optional .image
  association; explicit-enumeration uninstaller), signs the setup exe,
  and verifies everything.  Third-party DLLs keep upstream provenance
  (unsigned); the SDL2.dll text marker is never fed to signtool.
  VERIFIED end-to-end: silent install -> `signtool verify /pa` passes on
  both installed binaries -> installed VM boots a stock Pharo 13 image
  and runs benchFib -> silent uninstall leaves no files/registry
  residue.  First artifact: `dist/iospharo-0.1.0-setup.exe` (17.7 MB,
  SHA256 1D96612B79DDBA519D9B95AF0DADC4DB12E394AF603FD39D69720F94943B23B6).
  Kit gotcha recorded in the build script: sign.ps1 mis-binds a second
  positional file into -Verify — sign one file per invocation.

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
- [x] **Step-through debugger tests hang — FIXED (2026-07-02).**
  `FastStepThroughTest` 11/11, `StepThroughTest` 11/11 (both previously hung
  on `testStepThroughLonger`). ROOT CAUSE: `primitiveFindHandlerContext`
  (prim 197) capped its sender-chain walk at 10000 ("safety limit") and
  returned NIL when the chain was deeper. `testStepThroughLonger` recurses
  `evalBlock:afterLoop:` 10000 deep; the `Break` signaled by the debugger's
  HaltingBlock (fast mode) / the spliced `contextOn:do:` handler (slow mode)
  sat BELOW those 10000 frames, so the handler search exhausted the cap,
  the image saw "no handler", the Break surfaced as an unhandled-exception
  dump, and the step-through machinery never regained control — the busy
  spin. Depth-bisect nailed it: N=9950 PASS / N=10000 FAIL (recursion +
  ~50 machinery frames crosses the cap). The earlier "temp mutation not
  honored" suspicion was WRONG — the log showed Break firing, so the
  HaltingBlock replacement worked; the handler LOOKUP was the break.
  FIX: removed the arbitrary cap from prim 197 AND prim 195
  (findNextUnwindContext, same latent bug for `ensure:` blocks under deep
  recursion); the walk now ends only at nil, guarded against genuinely
  cyclic (corrupt) chains via Floyd tortoise-and-hare + one-time stderr
  warning. Regression battery: ExceptionTest 47/47, ContextTest 34/34,
  StepIntoTest 7/7, SUnitTest 35/35.
  DISCOVERED EN ROUTE (pre-existing, stock passes 10/10):
- [x] **StepOverTest>>testStepOverNonErrorExceptionSignalWithHandlerDeeperInTheContextStack
  — FIXED (2026-07-03), 10/10.** TWO stock-parity root causes, found by
  tracing the debugged process's frozen mid-unwind state vs stock:
  1. **Context-NLR unwind now uses the stock `aboutToReturn:through:`
     protocol** (handleContextNLRUnwind dispatches the send; the image's
     `Context>>aboutToReturn:through:`/`return:through:`/`resume:through:`
     runs the unwind blocks, terminates intervening contexts and returns
     the value). The old native pending-NLR (nlrTargetCtx_/nlrEnsureCtx_ +
     executeFromContext ensure-hopping) kept mid-unwind state in C++ where
     the debugger could neither see nor resume it: a stepOver frozen
     mid-unwind (inserted-ensure `here jump`) finished the ensure block and
     returned NORMALLY to the ensure's sender, losing the pending NLR.
     Empirically verified first (proto-probe): image-side
     `findNextUnwindContextUpTo:` + `resume:through:` (terminateTo: sender
     surgery + cross-frame return) work correctly on our VM from live
     frames — the 2026-era failed attempt at this protocol predated the
     prim-195 cap fix + materialization hardening. Opt-out knob for
     bisecting: `PHARO_NATIVE_NLR_UNWIND=1`.
  2. **prim 196 (terminateTo:) now nils BOTH sender AND pc of intermediate
     contexts** (stock parity; `Context>>isDead` tests `pc isNil`). It
     nil'd only senders, so terminated contexts read as alive and the
     debugger's `[context isDead ...] whileFalse:` walk stepped PAST the
     return target, simulating the debugged process to termination
     (suspendedContext=nil). Also removed its two 10000-deep walk caps
     (Floyd/self-breaking guards instead) and stopped nil-ing the
     receiver's own sender mid-walk (stock starts at receiver's sender).
  Regression battery green: StepOver 10/10, StepInto 7/7, StepThrough
