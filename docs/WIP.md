# WIP — 2026-11-13

Current state only. This file was 4882 lines of stacked session entries going
back to 2026-05-27; on 2026-08-23 it was pruned the same way `docs/deferred.md`
was on 2026-08-12 — the history moved to
`docs/history/wip-2026-05-27--2026-08-23.md`, nothing was deleted, and the full
text is also in `git log -p docs/WIP.md`.

47 entries were classified before pruning. Five had anything still live in them;
the other 42 were superseded or finished. The facts that were still true and
recorded in **no other file** were pulled forward into "Carried forward" at the
bottom of this file, each re-verified against the tree on 2026-08-23 with a
file:line. If you are looking for something that is not here, read the archive
before concluding it was never known.

Routing to the durable docs is in the header of `docs/deferred.md`.

## Package run on a real x86_64 host (2026-11-13, Windows 11)

This is the result of the task list in
[x86-package-run.md](x86-package-run.md).  The host is a Windows 11
x86_64 machine (24 cores, 31 GB), and the user chose a native Windows
run.  The build is MSYS2 CLANG64 (`build-win`).  The reference is the
stock Windows VM v12.0.6 with the Pharo 14 image.  The Linux x86_64 build
is still not validated on a real host.

The defects of our VM that this run found are in
[vm-compat-bugs.md](vm-compat-bugs.md) (first section).  All are fixed on
`jit`.  The result files are in `docs/results/win-x86-pkg200-2026-11-13/`.

Build and smoke test: the Windows build needed portable includes, a
portable clock and `intptr_t` for external primitives (`long` has 32 bits
on Windows).  `42 factorial printString` is correct with the JIT, with
`PHARO_NO_JIT=1` and with machine-code frames.

Differential scripts (`scripts/cogdiff/run-cogdiff.sh`, new driver): 20
scripts in 3 modes at the final state: the default (JIT with machine-code
frames), `PHARO_NO_JIT=1`, and `PHARO_NO_X86_MCF=1`.  `PHARO_NO_MCF=1`
has no effect on x86_64.  44 of 60 runs are the same as Cog.
`nlrhome.st` and `callbackstress.st` give the Cog answers in all 3 modes.

The 16 other runs are known differences: the temp of a dead context
(`ctxdiff`), VM attributes (`sysdiff`), `isRunningCogit` with the JIT off,
surface ids, and float results where the stock Windows VM is not accurate
(`primdiff`, `primdiff2`).  New scripts: `ffirange.st`, `utf8path.st`,
`filepos64.st`.  The last expression of `edgediff.st`, a wait on a new
Semaphore, does not end on Windows on the two VMs.  The driver leaves the
expression out with `EDGE_NO_WAIT=1`.

Package run, 200 manifest rows:

- 148 packages load.  52 do not load (arm64 Mac: 151 and 49).
- First sweep (before the fixes), packages with a result on the two VMs:
  Cog 32521 passes, our VM 31931.  124 packages were the same.
- Final sweep (fixed VM and fixed harness, 147 packages, without
  `unicompute-gemstone-pharo-bridge`): Cog 32521 passes, our VM 32749.
  138 packages have a result on the two VMs: Cog 32521, our VM 32522.
  131 of the 138 are the same.  Cog has no result for 9 packages, our VM
  for 2.
- The final sweep used machine-code frames, which are now the default on
  Windows.  This sweep is the validation of that default.
- The final numbers are one sweep plus a second run of 4 packages
  (`summary-final.tsv`).  The last fix (64-bit file positions) came after
  the sweep.  Only `apptivegrid-soil` ran again with that fix.

Load failures by cause (52):

| Count | Cause | Class |
| ---: | --- | --- |
| 10 | Buoy baseline: the `loadLocaleData` post-load step fails on Pharo 14 (ba-st family) | package |
| 9 | Metacello opens the `MCMergeOrLoadWarning` dialog (Zinc, STON, DrTests in the image differ from the repository) and waits for a click until the 600 s limit | harness |
| 4 | georghagn-tsf-*: the repositories are not public, and the clone waits for credentials | package |
| 4 | Baseline class not found in the repository (pharo-advent-of-code, pharowiki, pharo-openapi-generator, pharo-agent) | package or manifest |
| 4 | Code needs methods that Pharo 14 does not have (jslink, pharolink, pharo-archc, spec) | package |
| 3 | `RPackageOrganizer` is not in Pharo 14 (pharo-enhancements, casino, seekerdebugger) | package |
| 3 | The group name of the manifest is not in the baseline (bloc, pharo-huggingface, pharo-test-analysis) | manifest |
| 3 | A name is not found during or after the load (conduit, newtools, pharoaiactions) | package |
| 3 | The baseline requires a name that it does not define (stargate, superluminal, jadeiteforpharo) | package |
| 3 | Famix packages without Moose in the image (esope, famixtypescript, famix-cpp) | package |
| 2 | Repository conflict (iceberg, kendrick) | package |
| 2 | A file name with a vertical bar cannot be checked out on Windows (pam-gsoc-25-project, coypu) | Windows host |
| 2 | The baseline needs OSSubprocess, which does not run on Windows (biosmalltalk, viennatalk) | Windows host |

Only the last 4 are specific to Windows.  The 9 dialog cases can
possibly load with a warning handler in the load expression.  This was
not tried.

Differences in the final sweep, each with its class:

| Package | Cog | Our VM | Class |
| --- | --- | --- | --- |
| pharo-graphics-alexandrie | 144 pass | 140 pass | Cog defect: 4 rotation tests compare the VM `sin` with cairo, and the stock Windows C library has the same error as cairo |
| pharo-graphics-pharo-sdl-experiments | 27 | 25 | Known limit of our SDL2 stubs (pixel read-back, render driver list) |
| apptivegrid-soil | 704 | 706 | Our VM permits the delete of an open file (as POSIX); 2 tests fail on Cog |
| mumez-pharo-agentic-browser | 594 | 596 | Same cause as Soil |
| koendehondt-hera-for-pharo | 581 | 582 | Package: `testDuration` needs two different timestamps; the Cog clock has 1 ms steps |
| pharo-llm-pharo-acp | 98 | 99 | Package: same clock cause |
| pharo-xp-tools-sindarin-dap | 17 | 18 | Package: same clock cause |
| moosetechnology-mooseide | no result | no result | Package: an unhandled error in a forked process ends the run on the two VMs at the same test (1081 tests run; the failure lists are the same) |
| smalltalkweb-myprecious | no result (107 pass in a run alone) | no result | Package: `testReconnectsIfOtherSideIsDisconnected` leaks all task workers after a disconnect.  Cog gets a timeout or stops; our VM stops with a service-name error.  All tests before the last test pass on our VM |
| astares-pharo-os-windows | no result | 151 pass | Package: a test closes the stdout handle of the process in a finalizer, and Cog cannot write its log.  The 5 failures of our VM are the same on Cog when each test runs alone |
| exercism-pharo-smalltalk-test-runner | no result | 0 pass, 28 errors | Package: reads `ClapCommand` and `RPackageOrganizer`, which Pharo 14 does not have |
| pharo-rdbms-pharo-sqlite3, rko281-restoreforpharo | no result | errors only | Host: no `sqlite3.dll`.  The error in the finalizer process stops Cog |
| mumez-redistick, newapplesho-aws-sdk-smalltalk, pharo-nosql-mongotalk | no result | errors | Host: the tests need a Redis, AWS or MongoDB server |

Differences of the first sweep that were defects of our VM (fixed):
`apptivegrid-soil` (394 tests), `pharo-graphics-pharosdl3` (91 tests),
`smalltalkweb-myprecious`, `moosetechnology-mooseide`,
`pharo-graphics-alexandrie` (5 tests), and 3 tests that went over the 50 s
limit (`pharo-contributions-deeptraverser`, `omarabedelkader-pharo-lexicon`,
`moosetechnology-famixreplication`).

Differences of the first sweep that were harness defects: `evref-bl-mcp`,
`pharo-llm-chatpharo` and `omarabedelkader-heuristiccompletion-history`
(the copy of our VM did not have `libgit2` beside it), 3 FAIL lines of
`koendehondt-hera-for-pharo` that Cog wrote as 32-bit characters, and
single Cog failures that a second run did not show (`tinchodias-sauco`,
`polymathorg-polymath`).  One stock VM start stopped with "Could not
allocate newSpace in the expected place" (`pharo-rdbms-glorp` load).  The
second run loaded the package, and the results are the same.

Incident: in the first sweep the stock VM used 17 GB of memory in 35
seconds in `GbsSessionTest>>testLoginSuccess`
(`unicompute-gemstone-pharo-bridge`).  I stopped the process by its PID.  The final sweep does not include this package.

Open after this run:

- The Linux x86_64 build has no run on a real host.
- Our VM is slower than Cog on Windows: 2 to 3 times with machine-code
  frames on the three slow tests above.
- `cannotReturnStormGuard` stops `nlrhome.st` unless
  `PHARO_CANNOT_RETURN_BURST` is raised.  The driver raises the value.
- The fixes of this run have no arm64 or macOS build yet, and no SUnit
  sweep.  The non-Windows branches of the file path change were not
  compiled on this host.
- The remote `jit` got its own fixes for the SDL3 library, the BitBlt
  forwarder, the `cannotInterpret:` root and the wide FAIL lines while
  this run was in progress.  The rebase kept the remote versions of the
  SDL3 and BitBlt changes.

## Right now (2026-11-13) -- Pharo 14 package run, six VM fixes, four sweeps done

The four full SUnit sweeps ran at `3a697c16` on `jit`.  `main` is
fast-forwarded to `jit` after each set of sweeps.

| image | arch | pass, recovery pass included | not passed | sweep at `479dc56e` |
|---|---|---|---|---|
| Pharo 14 | arm64 | 28197 | 2 | 28198 / 1 |
| Pharo 13 | arm64 | 27930 | 2 | 27930 / 2 |
| Pharo 14 | x86_64 | 28199 | 2 | 28200 / 1 |
| Pharo 13 | x86_64 | 27928 | 6 | 27929 / 5 |

The three changes from the sweeps at `479dc56e` are not VM regressions.
Each change was measured with interleaved runs of the old binary
(`d8a36711`), the new binary and stock Cog:

- `FreeTypeCacheTest>>testGlyphAccessIsThreadSafe` (Pharo 14 arm64): a race
  in the image on the lazy FreeType face.  Old binary 11 failures in 50
  runs, new binary 11 in 50, Cog 9 errors in 50.
- `StObjectTranscriptPresenterTest>>testOpenedLogIsSelectedLog` (Pharo 14
  x86_64): the test process and the Morphic UI process, both at priority
  40, run `TabSelectorMorph>>updateTabs` on one morph.  The old binary, the
  new binary and Cog all show the error, at about 3 in 1000 runs on x86_64.
- `TestTookTooMuchTime` on a changing set of tests (Pharo 13 x86_64): a
  runner defect.  The runner did not disarm the watchdog of the SUnit
  environment after a test that set `timeLimit:`, and the watchdog then
  signalled a later test.  Fixed in `pharo-headless-test` `7e0c95b` and the commit after
  ([deferred.md](deferred.md) has the measurement).  The next sweep must
  use an image prepared again with the fixed runner.

`testQueryGoogle` fails in each sweep, on Cog also (live network query).

The four sweeps ran again at `a21b24c4` (the three fixes from the second
package run, and the fixed runner).  No `TestTookTooMuchTime` error is left.

| image | arch | pass, recovery pass included | not passed |
|---|---|---|---|
| Pharo 14 | arm64 | 28195 | 4 |
| Pharo 13 | arm64 | 27931 | 1 |
| Pharo 14 | x86_64 | 28200 | 1 |
| Pharo 13 | x86_64 | 27931 | 3 |

The tests that did not pass: `testQueryGoogle` (4 sweeps); two tests that
could not resolve `api.github.com` in that run; and the timing-dependent
tests `testNewProcessTaskRunnerRunsInNewProcessEveryTime`,
`StPlaygroundPageTest>>testContents` and
`testOpenSpotterRefreshesPreviewOnce`, each one known from earlier sweeps.

- Pharo 14 package run (`scripts/pkg-jit-test/`, 200 manifest rows, stock
  Cog v12.0.6 loads each package): 151 packages load, 49 do not load on
  Pharo 14.  First run at `d8a36711`, arm64: Cog 30436 passes, our VM 30414.
- VM defects found from the package differences, all fixed on `jit`
  ([vm-compat-bugs.md](vm-compat-bugs.md)):
  - Block `^` in a method copied from a trait: the home was found by method,
    not by context identity (`evref-bl-mcp`, 23 tests).
  - The machine-code `ensure:` shortcut did not send
    `aboutToReturn:through:` when a tool replaced that method (MethodProxies).
  - Block `^` with a returned home went into another live activation of the
    same method.  Cog sends `cannotReturn:`.
  - Block `^` after a process switch went into the wrong activation of a
    recursive method (scenario tG5 of `scripts/cogdiff/nlrhome.st`).
  - Callback return: the cooldown from `08c94847` left a window in which the
    executor process suspended itself with the `stackProtect` lock held
    (`pharo-cig` hung under load).  The return now follows Cog, and `step()`
    switches processes only at interrupt points.
- New differential scripts: `scripts/cogdiff/nlrhome.st` (70 scenarios) and
  `scripts/cogdiff/callbackstress.st`.  Both give the Cog answers on arm64
  and x86_64 (x86_64 under Rosetta).
- Harness fixes in `scripts/pkg-jit-test/` (see the README there): the
  fake-GUI prelude goes through `PKG_PRELUDE`, the log extraction accepts
  CR line endings and glued lines, `RUN` lines, `Halt` is an ERR, the
  summary has the exit statuses.  The 21 GUI packages gave no result in the
  first run because of the prelude defect.
- Package differences that are not VM defects: `pharo-cig` aborts because
  the package declares the 16-byte `CXString` struct as one pointer;
  `smalltalkweb-myprecious` has a busy loop and a service-name race, and Cog
  under Rosetta does not complete the reconnect while the loop runs.
- Second package run (fixed harness, VM at `8b93526c`, arm64): 150 packages
  load, 142 give a result on the two VMs.  Cog 37411 passes, our VM 37309.
  The differences, each one classified and reviewed:
  - `moosetechnology-mooseide` hung on our VM.  VM defect, fixed
    (`d31e40af`): `selectors_.cannotInterpret` was not a GC root, so the
    `cannotInterpret:` lookup failed after a compacting GC.
  - `pharo-graphics-pharosdl3` (Cog 108 passes, our VM 7).  Three causes.
    Our arm64 VM has no `libSDL3` where the image looks for the library
    (not fixed, a packaging decision).  The VM sent each SDL module to the
    SDL2 stubs (fixed, `c9c79751`).  BitBlt read a forwarded `Form` bits
    object (fixed, `c9c79751` and the commit after).  With an arm64
    `libSDL3` beside the image, the fixed VM gives the Cog result, 108/1/1.
  - `pharo-graphics-pharo-sdl-experiments` (Cog 27 passes, our VM 21).
    Open: the SDL2 stub layer has no `SDL_GetHint`, `SDL_GetTicks64`,
    `SDL_QueryTexture`, `SDL_RenderReadPixels`, `SDL_GetRenderDriverInfo`.
  - `koendehondt-hera-for-pharo`: harness defect, fixed (`652e163c`).  Cog
    writes a wide-string FAIL line with NUL bytes.
  - `exercism-pharo-smalltalk-test-runner`, `opensmock-molecule`,
    `apptivegrid-soil`, `pharo-graphics-spec-toplo`: package defects or
    races that Cog shows also.  `newapplesho-google-cloud-smalltalk`: Cog
    crashes in `libcrypto`.
  - `pharo-cig` and `smalltalkweb-myprecious`: no change, see below.
- Not done: a package run on a real x86_64 host
  ([x86-package-run.md](x86-package-run.md)).
- Open: a block whose home is live on the stack of a different process
  still uses the method fallback (scenario B7; the tested answer matches
  Cog).  `cannotReturnStormGuard` stops a process after 64 handled
  `BlockCannotReturn` signals; Cog has no such limit.

## Earlier (2026-11-12) -- Pharo 14 support, main merged into jit

- `origin/main` is merged into `jit` (`49d55559`).  Pharo 14.0.0 is the
  default image (`scripts/pharo-version.sh`, `PHARO_VERSION`, default 140).
- Pharo 14 changes in the VM: the `#stackOverflow` send (Process slot
  `shouldOverflow`, special object 24), `primitiveVMVersion`, the VM
  identity `v12.0.6+iospharo` ([vm-version-identity.md](vm-version-identity.md)),
  and the failure codes of Cog v12 for primitives 71, 597 and 599.
- Defects fixed after the first Pharo 14 sweeps
  ([vm-compat-bugs.md](vm-compat-bugs.md), sections "FIXED 2026-11-12"):
  the callback return let other processes run (SpAthensAdapterTest),
  `primitiveRegisterSurface` wrote through a stale pointer, and the
  synthetic `add:` and `nextPut:` primitives are removed.
- The SUnit runner fix for StTarjanAlgorithmTest is in the
  `pharo-headless-test` submodule (`590c7a4`).
- Full sweeps at `479dc56e` (the pass counts include the recovery pass):

      image     arch    classes  tests   pass    skip  not passed
      Pharo 14  arm64   2154     28412   28198   219   1
      Pharo 13  arm64   2047     28077   27930   182   2
      Pharo 14  x86_64  2154     28412   28200   217   1
      Pharo 13  x86_64  2047     28077   27929   180   5

- Tests that did not pass:
  - `ZnClientTest>>testQueryGoogle` in all four sweeps: a live network
    query.  Cog fails this test too.
  - `FreeTypeCacheTest>>testGlyphAccessIsThreadSafe` on Pharo 13, both
    architectures: a timing-dependent test.
  - Pharo 13 x86_64 only: three `TestTookTooMuchTime` errors
    (`LinkInstallerTest>>testOneMetaLinkManyObjects`,
    `TraitTestCase>>testCompositionCopy`,
    `ReleaseTest>>testMethodsOfTheClassShouldNotBeRepeatedInItsSuperclasses`)
    and `StSpotterTest>>testOpenSpotterRefreshesPreviewOnce`, which Cog
    fails too.
- Open leads without a reproduction are in [deferred.md](deferred.md)
  ("Open leads from the 2026-11-12 sweeps").
- Not verified on this host: the Swift app build (no Xcode), the GUI on
  Pharo 14, the 93-package run on Pharo 14, Windows and Linux.

## Earlier (2026-10-16, late) -- x86 at about 2x Cog

- x86 since the last entry: registers are the truth (state copies at
  exits and C calls), an x86 simStack (deferred pushes, register
  operands for + - comparisons == ~~ and conditional jumps), the failed
  primitive body call, the perform: cache, no key-0 test.  Lexicon 451
  billion instructions; the 2e7 loop 20 ms (Cog 32 ms); Famix
  testMediumSizeSources 14.6 s (it passes now, limit 15 s).  Per call
  (zzFib, translated instructions): about 165 against Cog's 75.
- arm64: primitive 18 (@) is callable; the block stub tests the
  block-home memo inline.
- Sweeps X10, X11, X12 (x86): 27810-27811 of 27996, the only extra a
  LinkInstallerTest or TraitTestCase timeout under load.

### State at reboot (HEAD `96356a7c`, 23 commits not pushed since `5bae4280`)

- The working tree is clean.  Do NOT push yet: the commits after X12
  (`27694737` arm64 block-home memo in the block stub, `d663bb02` x86 no
  key-0 test) have no full sweep.  The arm64 sweep L82 was running and the
  reboot kills it.
- To do, in order: rebuild build-rel and build-x86; the arm64 sweep (the
  last clean arm64 sweep is L81 on `8a799eef`, before every arm64-visible
  change of this session: prim 18 callable, the block-stub memo, the
  mcfDeriveView gate); the x86 sweep X13; the x86 package run (93 package
  images, compare with the arm64 results); then push origin jit.
- The reboot empties /private/tmp, so the session scratchpad is gone:
  sweep images (work/sunit8.image), package images, the Cog harness
  (/tmp/harness-x86), the battery baseline, probes.sh, cbench.sh,
  fibcnt.sh, microcnt.sh.  Rebuild them as CLAUDE.md "Running Pharo test
  suites" says (prep a fresh image with our VM; the stock x86 Cog under
  Rosetta).  The sweep script is scripts/sunit-sweep.sh; the battery is
  scripts/run_vm_fix_battery.st (its x86 and arm64 outputs must be
  identical to each other).
- Next perf levers: x86 per call is about 165 translated instructions
  against Cog's 75 (record, depth and countdown checks, call-cache lookup;
  docs/x86-mcf-port.md, last section).  arm64 outliers against Cog:
  exception (materializeFrameStackImpl, context allocation), perform,
  charTests (machine-code quality in SparseLargeTable>>noCheckAt:).

## Earlier (2026-10-16) -- x86 at 2.3x Cog

- The x86 call path has the arm64 trims: compact records, no closure or
  active context in records, r12/r13/r14 pinned (tempBase, receiver,
  JIT method) with the lazy method view, short pushes, cold slow paths,
  native perform:.  x86 lexicon 485 billion instructions, 27 s against
  Cog's 11.9 s.  Detail: docs/x86-mcf-port.md, last section.
- Sweeps X8 and X9 (x86): 27810-27811 of 27996 pass; the only extra is a
  LinkInstallerTest timeout under load.  X10 runs on `70b9f8cc`.
- Not pushed since `5bae4280`: push after a clean X10 and an arm64 sweep.
- The dates in the entries below (10-17, 10-18) are wrong; the machine
  clock said 10-15 and 10-16.

## Earlier -- x86 at 3x Cog, from 10x

- x86 machine-code frames are the default on macOS and Linux
  (`632afcd8`).  Since then: the x86 primitive stub (most common
  primitives on the caller's stack), native ExtendA sends, compare-branch
  fusion.  x86 lexicon 563 billion instructions (1468 at the start), about
  35 s against Cog's 11.9 s.  Sweeps X5-X7 pass 27808-27811 of 27996; the
  extras are compile-heavy tests timing out in a batch (they pass alone).
- arm64 micro set on an idle machine: 15 of 24 at or below Cog; perform
  1.50x, exception 1.43x, charTests 1.36x, detect 1.33x.
- Pushed through `5bae4280`.  Running: the 93 package images on x86
  (pc5).

## Earlier (2026-10-18, 01:00) -- machine-code frames on x86

- Machine-code frames are ported to x86-64 (the default on macOS and Linux, opt-out `PHARO_NO_X86_MCF=1`;
  docs/x86-mcf-port.md).  x86 lexicon instructions (Rosetta): 1468 to
  1138 billion; zzFib 4x and the send loop 4.5x faster than the x86
  interpreter.  Sweep X2 (x86, the mode on): 27808 of 27996, the arm64
  count.
- Found on the way, for both arches: an exit at an arithmetic special
  send now runs a primitive target in mcfRun and records a return memo
  otherwise; the interpreter's FFI symbol loader searches the
  executable's directory (270 Athens/Roassal errors on x86); the x86
  TestLibrary has `w64Convention`; a leftover primitiveStringAt trace is
  gone.
- Sweep L80 (arm64, `dd9dcc7e`) clean; pushed through `94a38340`.  Sweep
  L81 runs on `8a799eef`.

## Earlier (2026-10-17, 19:00) -- x86 is 10x slower than Cog

- Sweep L79 clean (2042 classes, 27808 of 27996 pass); pushed through
  `10cf2afd`.
- FFI callouts, struct returns, struct pointers and callbacks agree with
  Cog (`scripts/cogdiff/ffidiff.st`, `ffidiff2.st`).
- x86-64 build against Cog x86-64, both under Rosetta: lexicon 117.5 s
  against 11.9 s.  With the x86 JIT off: 98.8 s, so the x86 JIT makes
  real code slower.  (arm64: 69.0 s interpreted, 10.4 s with the JIT.)
  The x86 JIT compiles about 400 methods in lexicon; arm64 compiles 5448.
  Micro set on x86: 4x to 30x Cog.  Rosetta costs about 1.6x
  (interpreter on both arches), so most of the gap is in the x86 backend.
  Background: docs/x86-cog-gap.md (the x86 backend has no machine-code
  frames).

## Earlier (2026-10-17, 15:00) -- Cog differential checks, two flakes fixed

- Pushed through `09c030db` (sweep L73 clean).  Sweep L74 runs on
  `d27dd451`.
- Speed: perform: of a primitive runs in machine code, also at frame
  depth 0 (`3ba27cc7`, 106 to 66 ms per 10M, Cog 45).  compareWith: runs
  in the primitive stub (`c8c7fef1`).  The non-local return path is
  shorter (`797efdc0`, detect: -11% instructions).
- Cog fidelity: scripted diffs against Cog (`scripts/cogdiff/`, see
  docs/vm-compat-bugs.md 2026-10-17) found and fixed compareWith:'s
  answer, SmallFloat64 identity hash, exponent of 0.0, become error
  codes, perform: of a non-symbol, dead contexts after a non-local
  return, and stale pc/stackp reads of a live context.  Primitives
  (58345 calls), processes, exceptions, objects, files and clock queries
  now agree with Cog.
- Flakes: ProcessSpecificTest>>testLocalVariable was a VM race in
  primitive 230 (`d27dd451`, 0 of 12 fail, 6 of 12 before).  The
  in-memory event ring `PHARO_EVENT_RING` caught it.  Next:
  FreeTypeCacheTest, with the same tool.
- Harness: test_load_image eval doubles `!` (chunk format) and prints the
  result as UTF-8 (`e9f2fd0d`).
- Not done: `thisContext` in Exception>>signal materializes every frame
  (13% of a signal); Cog marries only the top frame.
- Later the same day: LocalePlugin and FilePlugin primitives registered
  under the image's names; reverse DNS lookup and the SocketPlugin
  address-info API (IPv6 connect, bind, accept) added in Cog's layout;
  exit without fixed sleeps (an eval of 1: 1.33 s to 0.34 s, Cog 1.0);
  inline allocation fills two slots per step; full GC skips pointers below
  the first mover (42 to 37 ms, Cog 23 under Rosetta).
- FreeTypeCacheTest testGlyphAccessIsThreadSafe: 5 of 6 alone fail on L73,
  3 of 6 on L74 and L75, 0 of 12 on L76.  The test races its own
  renderers by design; faster code (L76 compiles faster) leaves fewer
  preemption windows.  Treat it as timing, not fixed.
- TraitFileOutTest fails in every full sweep since L73 and passes in any
  shorter run: an IdentitySet-order dependency in the image
  (docs/image_issues.md).
- Full GC 42 to 32 ms (Cog 23 under Rosetta): the compaction update skips
  pointers below the first mover, the mark loop filters inline and marks
  plain objects without a call, and the clear walk is folded into the
  bitmap walk (30 GCs: 14.5 to 11.3 billion instructions).  The skip
  exposed purgeDeadCacheRoots leaving the code zone writable (SIGBUS after
  ~100 GCs), fixed in `815e03a9`.  Sweeps L75-L77 found nothing new; L78
  runs on `fd13c3b7`.
- Micro set vs Cog now (machine loaded): 15 of 24 at or under 1.1x.  Left
  over 1.2x: perform: of a primitive 2.1x, detect: 1.5x, exceptions 1.4x,
  isLetter 1.3x.  The last three come down to context creation and
  materialization (a married context is nil-filled and gets its temps
  copied; `thisContext` materializes every frame).

## Earlier (2026-10-17, 08:00) -- sweeps L67-L69 clean, exception returns at Cog speed

- Pushed through `aa9d98c8`.  Sweeps L67, L68 and L69 are clean.  L69:
  27812 of 27996 pass.  The two non-passes are SpAthensAdapterTest (UI
  race) and ZnClientTest testQueryGoogle (live network, Cog fails it too).
  Sweep L70 runs on `797efdc0`.
- `963006ad`: an FFI callback ends with a return status, not a C++
  exception (90.7 to 83.1 us per callback).
- `4d445bc9`: the resume shortcut.  `Context>>resume:` with no unwind
  frame before its receiver resumes the receiver directly, with no
  thisContext materialization (docs/native-nlr-plan.md).  Exception
  signal + return: 100 ms per 100000, Cog 191-242 cold and 45 warm.
- `797efdc0`: a non-local return over heap contexts now kills each
  context it unwinds (sender and pc nil), as Cog does.  Before, a context
  captured in a `do:` block stayed alive after `^`.  The same commit
  trims the machine-code non-local return (detect: -11% instructions).
- FreeTypeCacheTest testGlyphAccessIsThreadSafeWhenAskingWidth timed out
  once in L67 and passed in L68 and L69.  The test says it "breaks 95% of
  times".  Alone, it fails about half the runs on several builds.
- Micro set vs Cog (warm, machine loaded by a sweep): writeStream 13 vs
  11 ms (an earlier 38 was a one-off).  Left: perform: of a primitive 3x,
  detect: 1.7x (a closure creation marries a context, and the non-local
  return goes through C++), symbol compare 1.6x.

## Earlier (2026-10-17, 01:00) -- sweeps L63-L66 clean; perform cache complete

- Pushed through `411004ba`.  Sweeps L63, L64, L65, L66 clean (L65:
  27812 of 27996 pass; the non-passes are SpAthensAdapterTest's UI race,
  ZnClientTest's live Google query, and known timing flakes).  Sweep L67
  runs on `f4ecac2f`.
- perform: cache: native calls, quick methods, primitives
  (`02087199`, `a642af4f`, `3578d86a`); `f4ecac2f` clears it in
  flushJITCaches (the selective flushes after a method change).
  docs/mc-frames-design.md, "Speed work, 2026-10-16".
- The ReStore package rerun passes SSWReStoreProxyNonForwardingTest 31/31
  (the pc3 22/31 was a one-off; its Preempted tests depend on process
  timing).  The x86_64 tree builds and passes this session's semantic
  checks under Rosetta.
- Measured, recorded in docs/deferred.md: FFI callbacks 85 us vs Cog 9
  (step() loop, process switches, C++ unwind); full GC 1.5x Cog (marking).

## Earlier (2026-10-16, 21:00) -- perform cache; per-class run 714 s vs Cog 1546 s

- Pushed through `046bfe7c` (sweep L63 clean: 27996 tests, 27811 pass).
  Sweep L64 runs on `e4c7e238`.
- Since L63: `02087199` machine-code perform: cache (perform: #size 32
  -> 15 ms, Cog 14), `a642af4f` quick methods answer from it (collect:
  #accessor 2x faster), `e4c7e238` primitive 197 callable natively
  (exception signal + return -6.6% instructions), `41f1738a` MethodMap
  hash folds the high half (lexicon -0.5%).
- Per-class revalidation of the 93 package images (pc3, build
  `046bfe7c`): ours 714 s, Cog 1546 s.  Pass-count differences: pharo-cig
  (package bug), StSpotterModelTest and StTranscriptPresenterTest (ours
  better), SSWReStoreProxyNonForwardingTest 22/31 in package order (31/31
  alone and after all 69 classes before it in one eval; a rerun of the
  package is in progress).
- Micro set vs Cog (machine loaded): faster on loops, streams, String =,
  copies; perform: of a primitive 4.6x, exceptions 2.1x, the rest within
  1.5x.

## Earlier (2026-10-16, 17:00) -- sweep L62: 3 non-passes, none a VM defect

- Pushed through `49ce55e5`.  Sweep L62 (image sunit8, runner
  `1bbfc99`): 27996 tests, 27811 pass.  Left: SpAthensAdapterTest (UI
  scheduling race, docs/deferred.md), ZdcSecureSocketStreamTest testPlain
  (network flake), ZnClientTest testQueryGoogle (Cog fails it too).
- Runner: StDebuggerTest runs through `runCaseManaged` (its
  testIsInSelectedContextPackage reads the standard SUnit frames).
- Not yet swept: `284e9ba4` lazy become path compression, unforward every
  4096 pairs (SSWReStoreSpeedTest 2.3 -> 0.74 s, Cog 2.3); `046bfe7c`
  lazy become accepts overflow-header objects (microdown HTML tests 1.7x
  faster).
- Running: per-class revalidation of all 93 package images with the
  newest build (scratchpad pc3/, failing selectors recorded by per2.st).
- Measured, not fixed: an exception signal + return is ~25500
  instructions, 20000 in C++ (three full frame materializations for
  thisContext, a lazy-context sync for `sender`, four exits).  Cog marries
  one frame per `sender` step; doing that here means incremental lazy
  contexts.  perform: of a compiled method is ~330 instructions (Cog ~40);
  a machine-code perform cache would cut most of it.  tinyBenchmarks:
  bytecodes at parity with Cog, sends 68% of Cog.

## Earlier (2026-10-16, 12:00) -- lexicon faster than Cog; package tests 2x faster than Cog

- Pushed through `af483604`; sweeps L59, L60, L61 clean (L61: 27996
  tests, 27810 pass, 4 known failures).
- `9e929309`: a non-local return through one `ensure:` frame skips the
  image unwind protocol (docs/native-nlr-plan.md, "Ensure shortcut").
  Lexicon does 1.7M of them (Symbol class>>intern: via critical:):
  222.5 -> 187.5 billion instructions, 13.4 -> 11.4 s.  Cog: 11.6-12 s.
- `8c0b1283`: immediate literals are code constants (benchFib -9%
  instructions).
- `8348ea18`, `89006f0f`: a scavenge visits only the class-table entries
  written since the last one.  The full walk was ~80% of a small
  scavenge; Soil pins 46k buffers (one scavenge each with 4a4fd340) and
  went 4 s -> 236 s with 15 failures in package order.  Found by the
  per-class timing run (scratchpad pc2/).
- `374ec1b7`: pin of a young object by forwarding (Spur), not a scavenge
  in the primitive.  The scavenge hung ReStore's SQLite tests since
  4a4fd340.  Named primitives and the FFI callouts follow forwarders.
- Per-class package timing (93 package images, Cog side from the 10-15
  run): ours 740.7 s, Cog 1545.5 s on the same classes.  Remaining
  pass-count differences: pharo-cig (a package binding bug, see
  docs/vm-compat-bugs.md), PropertyManagerTest (finalization timing after
  a test that fails on Cog too), StSpotterTest (not yet looked at).
- Memory notes: async-writer-breaks-check-then-act,
  fix-that-adds-gcs-needs-a-gc-heavy-timing.

## Earlier (2026-10-16, 06:00) -- read-only countdown, perform fixes; sweep L58

- Pushed through `42440da8` (sweep L56 clean; its one new entry,
  StPlaygroundPageTest, passes 3 of 3 alone).  Since L55: MCF record trim
  stages 1-4 (docs/mcf-record-trim-plan.md; stage 1, the paired depth and
  context, is opt-in `PHARO_MCF_PAIRED_DEPTH`: the ldp/stp pair caused
  store-forwarding stalls, benchFib 38 -> 53 ms), body plan step 5
  (binary operations pop memory operands into registers), a pending
  method result goes to x1 for ReturnTop.
- `e04ed3b2`: native calls only read the checkpoint countdown; the
  heartbeat, external signals and a full eden set it to 0.  fibA 33 ->
  25 ms, benchFib 38 -> 35 ms.  Sweep L57 clean.  It opened a race:
  `f48b7172` gives perform: a call stub without the second countdown
  test (a perform loop failed after ~1.5 s with "selector argument must
  be a Symbol").
- `be44a84b`: perform: of a quick-primitive method answers in place
  (52 -> 27 ms for 3M; Cog 10).  perform: #+ with: is still 53 ms (Cog
  10): lookup, call-entry hash and primitive call, no single hot spot.
- Tried, no gain, reverted: predicted returns (bl/ret), frameDepth_ in a
  register (docs/deferred.md).  Memory note
  per-call-memory-chains-cost-wall-not-instructions.
- NLR through ensure: is 2.3x Cog (exA micro 260 vs 115 ms): two full
  frame materializations per return, the second for thisContext in
  resume:through:.  Plain ensure: and on:do: are 2-3x faster than Cog.
- Running: sweep L58 on `be44a84b`; a fresh per-class timing run of our
  VM over the 93 package images (scratchpad pc2/, Cog side reused from
  pc/) to find the current outliers.

## Earlier (2026-10-16, 01:00) -- pin fix, image test fixes, call-record design panel

- Pushed through `9d3489bc`.  Sweeps: L49 (segment on by default) clean;
  L50 (leaf primitives) clean apart from FFICallbackTest, which found a
  real VM bug: `primitivePin` left a pinned young object in eden, so a
  scavenge inside a qsort callback moved the buffer under C (`4a4fd340`:
  pin now scavenges first; repro 16 of 20 wrong before, 0 after; memory
  pinned-young-objects-must-tenure).  L51 clean apart from load flakes.
- `a56aab58`: leaf primitives called from machine code skip the register
  sync and executePrimitive; primitive 158 (String/Symbol compare) runs
  in place.  Symbol `<` 30 -> 14 ms (Cog 9).
- `0c70c9d4`: binary bytecodes load operands with one ldp and push the
  result through x25 (body plan step 3 part 2).
- `9d3489bc`: `scripts/pharo13-image-fixes.st` fixes six tests that fail
  identically on stock Cog (renamed trait, stale allowlist, a shadowed
  instance variable, a racy debugger provider, two swapped-index layout
  tests).  docs/image_issues.md lists them; CLAUDE.md prep files it in.
  Sweep L52 runs on the new image `sunit7` (scratchpad prep7.sh).
- Speed against Cog (Cog under Rosetta): tinyBenchmarks bytecodes 4.7G
  vs 5.0G per second, sends 280M vs 405M.  Lexicon 226 billion
  instructions.  Microbenchmarks: we are faster on loops, String =,
  copies; slower on exceptions (2.3x), perform: (now 4x), isLetter
  (1.6x, shared primitive stubs), Dictionary at: (1.25x).
- The send gap is the native call record: ~58 instructions per call pair
  against Cog's ~25.  A design panel (workflow mcf-record-trim-2) is
  mapping the C++ readers of each record field for a staged trim.
- Tried and reverted (no gain): Context-primitive sync skipped for
  non-running contexts; eden-full early checkpoint; inline basicNew: in
  the 1-argument primitive stub (the IC path already allocates inline).

## Older entries

Entries from 2026-08-23 to 2026-10-15, and the "three tiers", "Landed
today" and "Open" sections of 2026-09, are in
`docs/history/wip-2026-08-23--2026-10-16.md` (moved 2026-10-17,
unchanged).

## Carried forward from the archive

Still true, and recorded in no other file. Each was re-verified against the tree
on 2026-08-23; line numbers are from that day. Grouped, terse, and deliberately
not expanded — the full narrative is in
`docs/history/wip-2026-05-27--2026-08-23.md`.

### JIT / code zone

 1. **evictLRU pass 2 is address order, not recency.** Pass 1 needs ~650K
    method entries untouched (64K per epoch, `epoch_-10` threshold,
    `CodeZone.hpp:313`), so pass 2 usually runs and walks `firstMethod_`
    (`:331-344`), evicting the earliest-compiled kernel methods that stay hot.
    `763f7f7b` fixed the epoch never advancing; the fallback is unfixed.
    Planned: progressively younger thresholds (`epoch_-5, -2, -1`, then all).
 2. **The evictLRU second-pass change was measured and DROPPED** — 1 MB zone
    8 s/277 methods vs 7 s/286; 192 MB zone (XMLParser load) 344 s/5769 vs
    340 s/5542. Re-open only with a workload that fills the zone *and* advances
    the epoch.
 3. **248,066 methods compiled during one XMLParser load** against ~176k
    CompiledMethods+CompiledBlocks in the whole image (recompile churn).
    `evictTarget` is 1/64th of the zone (`JITCompiler.cpp:1780-1782`) and may
    want raising now that a round is cheaper.
 4. **Bit-60 J2J precedence makes five inline-return specializations dead on
    arm64** — the bit-60 `tbnz` is emitted before the extras dispatch
    (`AsmjitT1.cpp:6488`, dispatch at ~6498/6506/6512/6525/6540). Derived
    upside ~3.9% on a 356 ms workload. **The fix is the reorder**;
    `PHARO_NO_J2J=1` alone makes things slower.
 5. **arm64 emits 2.7-2.9x more bytes per compiled method than x86_64** — 7.89
    vs 2.76 KB/method; 8.91 vs 3.31 across full sweeps. This is why arm64 fills
    the code zone first.
 6. **35% of arm64's code footprint is inline-J2J** — 45,176 KB drops to
    29,260 KB with `PHARO_T1_NO_INLINE_J2J=1` (`debug_vars.h:92`).
 7. **IC hit rates are not comparable across arches**: arm walks IC slots 0-2
    inline (`AsmjitT1.cpp:6331-6363`), x86 probes slot 0 and exits
    (`:3185-3189`), so the two builds count different event populations.
 8. **`[JIT] Stats:` lines are periodic checkpoints, not totals** — one every
    65,536 method entries (`JITRuntime.cpp:~3896`). Two mid-run samples were
    once published twice as evidence of a structural arm-vs-x86 gap; real
    end-of-run totals are 0.56% apart.
 9. **`PHARO_T1_INLINE_PRIM_COUNTERS=1` gotcha** — without it `g_primAt_hits`
    and friends stay 0 even when the inline path is firing, giving a misleading
    "primAt=0".
10. **The asmjit T1 inline-SETTER path never fires in micro-benchmarks**
    (getter=16059, setter=0; `detectTrivialMethod` runs ~168 times per
    tinyBench and never classifies a setter). The recognizer is not broken —
    fib/sieve/tinyBench contain no setter sends.
11. **DEAD END, do not re-walk**: adding the inline-asm write barrier to the six
    temp-store stencils (`storeTemp_1/_2`, `popStoreTemp_1/_2/_3/_4`,
    `stencils.cpp:~4496-4540`) left the miss count unchanged at 230 and
    regressed tinyBench ~11% (5350 -> 5950 ms). JIT code runs `frameDepth_ > 0`,
    so that path is virtually never exercised. Reverted.

### GC / memory

12. **The remembered set is dead infrastructure.** `rememberedSet_` is populated
    (`ObjectMemory.cpp:3712`) but never iterated — only `.clear()`ed (`:2321`);
    scavenge still does an O(oldSpace) full scan ("Trade correctness for perf
    until every write site is audited", `:2221-2226`). Closing it would drop
    ~30 ms/scavenge on a 100 MB heap. The audit reached 260 -> 228/230 misses at
    99.997% bit accuracy; residual misses are C++ paths, not the JIT path.
13. **`PHARO_WATCH_ROOT_CLASS` (`debug_vars.h:201`) is an exact class-name
    compare** — it does not follow subclasses, so check `allSubclasses` before
    reading a zero-hit result as meaningful.
14. **Raw-heap-scan gotcha**: `PHARO_SCAV_RAWSCAN`
    (`ObjectMemory.cpp:2434-2513`) scans format-agnostically and hits
    `ExternalAddress` objects (fmt 16, ptrSlots=0) that legitimately store
    native addresses — all 54 hits in one run were such false positives.
15. **`[SCAV-DANGLE]` shares the scavenger's blind spot** — both walk the same
    `forEachRoot` set (`ObjectMemory.cpp:2188` and `:2421-2425`), so a holder
    outside that set makes the diagnostic report clean while the object dies.
    A clean dangle scan is not proof the roots are complete.
16. **`ObjectMemory::setGlobal`'s tail** (`ObjectMemory.cpp:~1063-1073`) walks
    the dictionary array for the first nil/zero slot and stores there, with no
    hash placement and no tally update. Unresolved, recorded nowhere else.
    (The `68` DNU that led here *is* fixed; see `ObjectMemory.cpp:1029-1035`.)

### Scheduler / VM control

17. **`Interpreter::running_` is a plain `bool`** (`Interpreter.hpp:1168`),
    written by `stop()` (`:285`) and read by the dispatch loop — a real data
    race, and the leading candidate for the `vm_stop` timeout. Left standing
    **deliberately**: `std::atomic` puts a real load in the hot dispatch loop,
    so it needs a bench-suite before/after, not a blind edit.
18. **`removeProcessFromList`'s return value is discarded**
    (`Interpreter.cpp:18685`, `Primitives.cpp:5017`) and the caller then
    unconditionally nils `ProcessNextLinkIndex`, truncating the run queue if the
    search failed. Wants a repro before touching the scheduler.
19. **`vm_stop` timed out on roughly one relaunch cycle in thirty** before the
    later fixes (10 runs: 9 pass, 1 fail, one stop-timeout, one destroy-leak).
    Post-fix runs show zero. Budget that rate for any repro attempt.
    Instrumentation is live at `PlatformBridge.cpp:618` and `:669`.

### Measurement traps

20. **We do NOT set `PHARO_CI_TESTING_ENVIRONMENT`**, so our sweeps run the 17
    tests Pharo's own CI skips via `skipOnPharoCITestingEnvironment` —
    including `AllocationTest` testOneGB/testOneGWordAllocation,
    `RandomTest>>testUnixRandomGeneratorSeed`, `StDebuggerTest`, `StSpotter*`,
    `StPlaygroundPageTest>>testContents`, `TKTNewProcessTaskRunnerTest`, and
    `WeakAnnouncerTest>>testNoDeadWeakSubscriptions`. Check the guard before
    calling any of them a VM defect.
21. **Classes that time out emit no `Total:` line** and drop out of the
    per-class tally (`run_sunit_tests.st:582-598`) — which is why arm counted
    2043 and x86 2037 against 2047 declared. A class-count difference between
    runs is not evidence of a crash.
22. **Package-tier noise floor is ±2**: four PolyMath runs on one day scored arm
    err = 16, 16, 17, 18. Any arm-vs-x86 package delta needs two runs per side.
    `sunit-sweep.sh:95-102` states the equivalent rule for SUnit;
    `package-tests-selfhosted.sh` does **not**.
23. **A killed run reads as a passing one.** A bisect reported
    `noyg rc=124 mnu68=0` and the zero was a TIMEOUT — 900 s is not enough for a
    no-young-generation run (`PHARO_NO_YG`, `DebugSettings.cpp:379`). Record rc
    and an explicit completion marker alongside every count.
24. **"It did not reproduce in N runs" is not "it does not happen"** unless N
    suits the rate — 9 consecutive clean runs were once cited as proof against a
    ~10% event, and a correct conclusion was retracted on that basis.
25. **Verify the binary contains the change before blaming the architecture.**
    `build-x86` had not been rebuilt after the class-registration fix, so x86
    reported the pre-fix signature and read as an arch-specific GC defect. Both
    trees must be rebuilt (`build-rel` arm64, `build-x86` x86_64);
    `strings <binary> | grep <marker>` settles it.
26. **`bash -n` passing does not mean a driver script works** — a Smalltalk
    `"comment"` inside a double-quoted eval string terminates the string.
    Smoke-test every generated eval.

### Host & tooling

27. **`scripts/sunit-sweep.sh` still defaults `PER_BATCH_TIMEOUT=600`**
    (`:111`), which is too tight — batch 1501-1550 legitimately takes 613 s and
    is not a hang. `scripts/rerun/fullsweep.sh:9` sets 900.
28. **`scripts/rerun/fullsweep.sh:4` and `scripts/rerun/pkgrun.sh:9` have a
    session scratchpad path baked in** — fix that line before rerunning either.
29. **After a reboot clears `/private/tmp`, run `git worktree prune`** — git
    otherwise still believes the branch is checked out in the deleted worktree
    and refuses `git checkout jit`. There is a live scratchpad worktree today.
30. **x86-box constraints**: gdb there cannot read member vars (no full DWARF),
    so instrument in C++ instead; `gdb -p` needs
    `sysctl kernel.yama.ptrace_scope=0`; `PHARO_T1_TRACE_HANDLER`
    (`debug_vars.h:288`) dumps the handler-search chain.
31. **Cog-x86 baseline** measured on the x86 box (18.221.159.216), recorded
    nowhere else: loop20M ~44-50 ms, fib30 ~9-10 ms, cfibx30 ~13 ms.
32. **arm64 cost baselines** (ad-hoc evals, no committed script): INTADD3M
    4 ns/iter; BLOCKVAL3M 72 ns per block `value:`; ALLOCARR2M 93 ns per
    `Array new: 4`; COMPILE300 1.54 ms per `Compiler evaluate: '1+1'`. Block
    activation and small-object allocation sit 18-23x above the fast integer
    path.
33. ~~**Whether this VM is slower than Cog on these workloads is unanswerable
    on this host**~~ — SUPERSEDED 2026-09-02. The abort is arch-specific: the
    **x86_64** Cog runs under Rosetta (`arch -x86_64`, see CLAUDE.md), so the
    comparison IS available, and it is fair against `build-x86` since both are
    x86_64 under Rosetta. Separately, Rosetta overhead vs x86_64 backend code
    quality has still never been split.
34. **`-Wnontrivial-memcall` has an array-form blind spot** — it fires on
    `memset(p, 0, sizeof(OpenPort))` but not on
    `memset(gPorts, 0, sizeof(gPorts))`. Any warning-driven sweep inherits the
    hole. (Re-reproduced on Apple clang 21.0.0.)
35. **`LinkInstallerTest>>testPropagateNewClassScopedLinksOnMethodNode` is
    GC-pressure-dependent, not the JIT** — 40/40 alone with JIT on or off; fails
    with any predecessor class; `PHARO_NEWSPACE_MB=1` makes it fail alone but
    only sometimes (39/40 and 40/40 on identical re-runs). Needs repetition
    counts before believing any knob bisect.
36. **Do not re-chase the `cos(2)` 1-ULP claim.** The image defines
    `Float>>cos` as `^ (self + Halfpi) sin`, and computing *that* formula with
    this host's libm reproduces the VM's bits exactly. No discrepancy exists.

### Corrected while pruning

37. **The WideString-WriteStream bug is FIXED** (`d5608fd4`, 2026-06-01), but
    `docs/results-jitpkg.md:519-521` still cites "the already-documented
    WideString-WriteStream bug (WIP.md)" as if open. Fix that citation rather
    than re-investigating.
38. **`indexOfClass` returning 0 for not-found** (`ObjectMemory.cpp:628,649`) is
    still true, but the "any new caller is unguarded" half is now stale — 5 of
    the 10 enumerated callers were guarded on 2026-08-23 (`b0ea7721`).
