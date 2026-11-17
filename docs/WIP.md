# WIP — 2026-11-17

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

## Right now (2026-11-17) -- every test that does not pass gets a fix

The owner changed the focus on 2026-11-17.  The rule is in `CLAUDE.md`
("Every failing test gets a fix").  A test that stock Cog fails too is not
accepted any more.  The fix goes into the test, the image, our VM or Cog.
Changes for Cog and for the Pharo image upstream go to the owner for review
before any pull request.

### Measured state, Pharo 14 build 791, arm64

One sweep of each VM on the same image (fake GUI and image fixes filed in):

| VM | Tests | Pass | Fail | Skip | Expected failure |
|---|---|---|---|---|---|
| Our VM, JIT (`build-rel`) | 28413 | 28193 | 1 | 170 | 49, not run by the old runner |
| Stock Cog v12.0.6, native arm64 | 28413 | 28193 | 1 | 170 | 49 fail or error |

The two VMs give the same result for each of the 220 tests that do not pass,
with one exception: the two `WriteBarrierTest` tests that upstream marked as
expected failures.  Our VM fails the primitive, and the image then signals
`PrimitiveFailed`.  Cog writes into the read-only `ByteArray`.

The 220 tests in groups:

| Group | Tests | State |
|---|---|---|
| Fixture tests that only another test runs (`MockTest`, `RTFFake*`) | 35 | to prove as not applicable |
| Windows-only and 32-bit-only tests | 24 | to prove as not applicable |
| Tests that skip in a session that is not interactive | 28 | our VM cannot give an interactive session to a test run; 6 of the 28 use `Smalltalk isHeadless`, which always answers true in Pharo 14 |
| Tests that upstream set to skip, or marked as expected failures | 132 | each one needs a repair |
| `ZnClientTest>>testQueryGoogle` | 1 | the test looks for `pharo.org` in the Google result page |

### Harness defects found on 2026-11-17

- The runner did not run a test that has the expected-failure mark.  It
  counted the test as a skip.  Fixed (`pharo-headless-test` `300ce55`): such a
  test runs, a failure is an `XFAIL` line, a pass is a `FAIL` line.
- The Cog runner used a bare `runCase`.  `ProcessTest`, `SUnitTest` and others
  gave 56 errors and failures that were not defects.  Fixed in the same
  commit: `runCaseManaged`, the same session state as our runner, no run of
  abstract classes.
- All runner state files had fixed `/tmp/sunit_*` names.  Fixed: the
  `SUNIT_PREFIX` environment variable (`CLAUDE.md`, "Running Pharo test
  suites").
- The runner for our VM gives each test 50 s in place of the 10 s of SUnit,
  and removes each class limit.  The runner also replaces about 12 image
  methods (speed shortcuts and old VM workarounds).  Not fixed yet: an agent
  works on the runner (worktree branch `wt-runner`).
- The runner for our VM does not work on stock Cog (`TIMEOUT(prim-stuck)` on
  each test).  Same agent.
- `scripts/vmrun.sh` looked for VM processes in the full command line of
  each process.  A shell whose command text had the path of a VM binary was
  "a VM that this call did not start", and the script refused each VM start
  of each other call while that shell was alive (6 minutes on 2026-11-17 at
  13:40).  The package loop stopped at row 86, and four other runs did
  nothing.  Fixed: the script looks at the program of a process, not at its
  arguments.  The runs that were refused ran again.
- The fixes file of a package is one chunk.  A line of temporaries in the
  middle of the file is a syntax error of the whole chunk, and the image
  reports the error only at the prep of the row.
  `scripts/gen-image-fixes.py` now stops for such a file, for a string that
  does not end and for brackets that do not balance.

### Host facts found on 2026-11-17

- The checkout is `~/src/iospharo` again.  The CMake caches of all build trees
  named `~/src/iospharo-jit`.  `build-rel` has a new cache and a full build.
  The other trees need `rm -rf <tree>/CMakeCache.txt <tree>/CMakeFiles` and a
  new `cmake -S . -B <tree> ...` before the next build.
- The stock arm64 Cog VMs start on this Mac (builds of 2026-10-23).
- `~/src/pharo-vm` (branch `pharo-12`) and `~/src/pharo` (branch `Pharo14`,
  the commit of image build 791) are new clones for reference and patches.
- A reboot on 2026-11-17 emptied `/tmp`.  The harness directories are
  `/tmp/harness` (Pharo 13), `/tmp/harness14`, and the two `-x86` ones.
- The notes folder of `~/src/iospharo` now has the memory notes of the old
  path `~/src/iospharo-jit` too.

### Rules of the owner for the work on this Mac (2026-11-17)

The owner stopped the first run of this day.  43 groups of agents, two more
agents, sweeps and three Cog builds ran at the same time, and the Mac ran out
of memory.  The rules are in `CLAUDE.md` ("One test VM at a time"):

- Start each test VM through `scripts/vmrun.sh`.  The script permits one VM
  at a time, checks memory, and makes sure that the VM is gone at the end.
- Do not run agents with VM runs, sweeps and builds at the same time.
- Keep shell commands simple, so that the owner gets few approval prompts.

### Results after the repairs (2026-11-17)

The repair run gave fix entries for 132 tests (`scripts/image-fixes/`, 152
entries that apply to a pristine Pharo 14 image on the two VMs).  The record
for the review by the owner is
`docs/upstream/pharo-image-repairs-2026-11-17.md`.

One sweep of our VM (`build-rel`, the old runner, all entries of that time):

| Tests | Pass | Fail | Error | Timeout | Skip | Expected failure |
|---|---|---|---|---|---|---|
| 28416 | 28331 | 0 | 0 | 0 | 84 | 1 |

Before the repairs the same sweep gave 28193 passes.  The 85 tests that do
not pass now:

| Group | Tests | State |
|---|---|---|
| Tests that need an interactive session | 28 | all 28 pass on our VM in an interactive session with the entries of `22-interactive-session-tests.st` (measured by the agent, 3 runs); the sweep has no interactive pass yet |
| Fixture tests that another test runs | 35 | by design (34 skips and 1 expected failure) |
| Windows-only and 32-bit-only tests | 21 | not applicable on macOS arm64 |
| `TFUFFICallbackTest>>testCallbacksInOtherThread` | 1 | passes on our VM; stock Cog stops with a segmentation fault, so the skip stays until Cog has a fix |

VM defects found by the repairs, and fixed in our VM:

- `primitiveResolverLocalAddress` answered 127.0.0.1 on every machine.
- A named primitive that does not exist gave no error code (Cog:
  `#'not found'`).
- `primitiveFFIIntegerAtPut` wrote into a read-only `ByteArray`.
- `PHARO_INTERACTIVE=1` had no effect on an eval run.

Cog defects found, with a tested change for each one in `docs/upstream/`:
the local address lookup, the write barrier of the typed stores (C
primitives and the JIT versions), the error code of primitive 218, and the
pinned code-generation VM that does not start on macOS 27.  Open in Cog: the
segmentation fault for a callback from another thread with the same-thread
runner, and a segmentation fault in `MorphicWindowManagerTest` in an
interactive session (one of three runs).

Harness defect found: the two runners ran a `ParametrizedTestCase` class
with the first parameter set only.  251 classes have 2173 test selectors, and
the real suite has 11825 tests for them.  The new runner
`scripts/pharo-headless-test/run_sunit.st` takes the tests from `buildSuite`
(see [sunit-runner.md](sunit-runner.md)).  The new runner also changes
nothing in the image and uses the SUnit time limits.

### Sweeps with the new runner, and what they showed (2026-11-17)

The new runner has 39455 tests for Pharo 14 (2160 classes).

| Sweep | VM | Pass | Fail | Skip | Expected failure | Note |
|---|---|---|---|---|---|---|
| `sweep14-new1` | ours | 39338 | 2 | 114 | 1 | |
| `sweepcogp14-new1` | patched Cog | 39339 | 1 | 114 | 1 | the two VMs differ in 1 test |
| `sweep14-new2` | ours | 39340 | 0 | 114 | 1 | 1 batch stopped, see below |
| `sweep14-new3` | ours, `f9cac33f` | 39339 | 1 | 114 | 1 | no batch stopped |
| `sweep14-new4` | ours, audit fixes, entries 25 and 26 | 39340 | 0 | 114 | 1 | no batch stopped |
| `sweep14-new5` | ours, `39105f0c`, entries 25 to 27 | 39343 | 0 | 114 | 1 | 39458 tests: 3 new queue tests |
| `sweepcogp14-new3` | patched Cog, entries 25 to 30 | 39370 | 0 | 87 | 1 | entries 28 to 30 turn 27 skips into passes |
| `sweep14-new6` | ours, entries 25 to 30 | - | - | - | - | 2 batches stopped with a segmentation fault, see below |
| `sweep14-new7` | ours, perform-cache repair, entries 25 to 30 | 39370 | 0 | 87 | 1 | no batch stopped; interactive pass 135 of 135 |
| `sweep14-new9` | ours, socket options as in the stock VM | 39369 | 2 | 87 | 1 | 39459 tests; the 2 failures are the rename tests that entry 31 (first form) broke |
| `sweepcogp14-new4` | patched Cog, no gate | 38491 | 2 | 87 | 1 | 38582 tests: 1 batch stopped with "index out of range" in the UI process (the harness race); 1 error |
| `sweepcogp14-new5` | patched Cog, gate (first form), entry 31 | 39370 | 0 | 87 | 1 | 1 error: the rename test, repaired in the last form of entry 31 |
| `sweep14-new10` | ours, gate (first form), file plugin | 39337 | 0 | 87 | 1 | 34 errors: the rename test, and 33 of `LinkInstallerTest` (entry 33) |
| `sweepcogp14-new6` | patched Cog, gate, entries 31 to 33 | 39373 | 0 | 87 | 1 | 39461 tests; no batch stopped |
| `sweep14-new11` | ours, gate, entries 31 to 33, socket options, file plugin, hash generator | 39373 | 0 | 87 | 1 | 39461 tests; no batch stopped; interactive pass 135 of 135; the same 88 tests do not pass as on the patched Cog |
| `sweepcogp14-new7` | patched Cog with the low-space change | 39373 | 0 | 87 | 1 | 39461 tests; no batch stopped; the same 88 tests do not pass |
| `sweepcogp14-new8` | patched Cog with the file plugin change | 39373 | 0 | 87 | 1 | 39461 tests; no batch stopped; the same 88 tests do not pass |

Defects that these sweeps showed, each with its repair:

- Our VM switched process at the entry of `ensure:`.  That was the stopped
  batch of `sweep14-new2`, and `ZdcSocketStreamTest` stopped the image in 4
  of 25 runs.
  Fixed in `f9cac33f`.  A code audit then found one more path to the same
  state and two smaller differences from Cog.  The three are fixed, and
  `sweep14-new4` has them.  Text: [vm-compat-bugs.md](vm-compat-bugs.md).
- TaskIt stops a worker that holds a task (the failure of `sweep14-new3`,
  `TKTFutureTest`, and the old open lead of `TKTCommonQueueWorkerPoolTest`).
  The defect is in the image, and stock Cog has it too.  The repair is
  `scripts/image-fixes/25-taskit-worker-pool-race.st`.  Loops of the test
  (`scripts/cogdiff/taskit_pool_stress.st`, `taskit_pool_diag.st`): 83
  failures in 600 runs before on our VM (stock Cog: 2 in 600), 0 in 3000
  after.
- Four TaskIt tests of `firstCompleteOf:` depend on the order of two
  callbacks in a worker pool (`26-taskit-first-complete-tests.st`).
- `AtomicSharedQueue` loses a wake-up with two readers, and a reader that
  is terminated leaves the queue locked (`27-atomic-shared-queue-wakeups.st`,
  with 3 new tests that fail on the two VMs before the repair).  A review of
  the TaskIt repair found the three defects.
- A stale method comment and a race in a callback test (entries 24 and
  `20-TFFI-disabled-tests.st`).
- 27 skips of the code-snippet tests and of one FFI callback test
  (`28-simple-formatter-comments.st`, `29-code-importer-snippets.st`,
  `30-ffi-callback-from-other-thread.st`).  The callback test needs the Cog
  change for a callback from another thread, so stock Cog still skips it.
- The `perform:` cache of the arm64 JIT called a method through a call-cache
  entry with no compiled code (the two stopped batches of `sweep14-new6`).
  The defect was old, and the new image entries made it show.  Fixed in
  `AsmjitT1.cpp`.  Text: [vm-compat-bugs.md](vm-compat-bugs.md).
- The fake GUI let two processes run World cycles and test code at the same
  time (the stopped batch of `sweepcogp14-new4`, and `StMethodBrowserTest` in
  7 of 80 class runs on stock Cog).  `setup_fake_gui.st` now has a gate:
  `MorphicRenderLoop>>doOneCycle` starts no cycle of another process while
  the test process is ready to run.  With the gate, 30 of 30 class runs on
  stock Cog and 10 of 10 on our VM are clean.
- The uninstall of an object-specific MetaLink depends on the order of a set
  (the 33 errors of `sweep14-new10`).  The defect is in the image, and stock
  Cog has it too (`scripts/cogdiff/metalink_uninstall_order.st`).  The
  repair is `scripts/image-fixes/33-metalink-uninstall-order.st`.
- Our VM made identity hashes with a 32-bit state and did not read the start
  value from the image header.  The sequence of hashes was not the sequence
  of Cog, so the order of hashed collections differed between the VMs.
  Fixed in `ObjectMemory`.  Text: [vm-compat-bugs.md](vm-compat-bugs.md).

### The 200-package run (started 2026-11-17)

The owner allowed the download of the packages from GitHub on 2026-11-17.

    scripts/vmrun.sh pkgs /tmp/pharo-work/pkgs1 <first-row> [last-row]

For each row of `scripts/pkg-jit-test/packages-200.tsv`, stock Cog loads the
package and saves the image.  Stock Cog and our VM then run the test classes
of the package with `run_sunit.st`, each VM on its own copy of that image
(`scripts/pkg-jit-test/run-one-package.sh`).  The results are in
`<outdir>/summary.tsv`, and the tests that differ between the VMs are in
`<outdir>/res/<label>.diff`.  A package can have a fixes file
(`scripts/pkg-jit-test/fixes/<label>.st`).

State on 2026-11-17 at 15:20, rows 1 to 100 (the record of each repair is
[upstream/package-repairs.md](upstream/package-repairs.md)).  Rows 101 to
200 run now (`/tmp/pharo-work/fix33/pkgloop2.sh`).

- 81 rows are clean: each test passes on stock Cog and on our VM.
- 3 rows name repositories that are not public any more (51, 62, 66; row
  135 is the fourth).  These rows cannot run.
- 16 rows are not clean yet.  Each has a repair in work:

| Row | Package | State | Work |
|---|---|---|---|
| 57 | EscapeAnalysisPharo | 69 of 82 pass | second round by an agent with probes: the analyzer of the package gives other answers than 8 tests expect |
| 70 | BioSmalltalk | 1193 of 1205 pass | second round: a stand-in for a Glamorous Toolkit class broke 12 tests |
| 71 | Pillar | 37 of 66 pass | second round: the patch did not bring the package `Microdown-Toc` into the image; 4 PDF tests need LaTeX |
| 72 | Microdown | 1604 of 1605 pass | second round, 1 error |
| 73 | Famix | 1474 of 1477 pass | second round: `FamixStClass` has no LCOM metrics; a cohesion value |
| 74 | ReStore | our VM 2358 of 2358, stock Cog 2357 | second round: one test fails on Cog in some runs |
| 75 | anthropic-sdk-pharo | 931 of 932 pass | the 1 skip writes into the login keychain: the owner must decide |
| 81 | Stargate | 519 of 520 pass | second round: a test compares a URL text, and the order of the query fields differs |
| 87 | Superluminal | 150 of 203 pass | an agent writes a stand-in for the HTTP test service on port 80 |
| 82 | MCP | stock Cog 1166 of 1167, our VM 1165 | 1 test fails on our VM only (the limit of 56000 frames; the VM repair is in work on the branch `deep-recursion`).  The 1 skip cannot apply on Pharo 14 |
| 83 | Kendrick | 219 of 240 pass | second round: 16 errors (API changes of Roassal and DataFrame, the DSL), 1 failure, 4 expected failures |
| 84 | Soil | 1119 of 1139 pass, 2 failures, 18 skips | second round: two lint failures and 4 skips of lint and coverage tests; 14 skips cannot apply (8 Windows lock tests, 6 cases with no meaning) |
| 89 | Teapot | 78 of 79 pass | 1 skip: `testHttpsGet` needs an HTTPS server in the image.  The SSL plugin of the two VMs takes a server certificate only from a keychain on macOS.  A plugin change is needed in our VM and in Cog; it must not touch a keychain of the user.  Not started |
| 90 | pharo-test-analysis | 67 of 71 pass | the row loads now (a patch of the baseline); second round: 4 errors (`RTDHelperVisitor` does not understand `#messages`) |
| 91 | Turbo | 265 of 270 pass | the 5 expected failures are plain failures now: the new pictures of the first round are not what the code draws; second round with probes |
| 100 | Sagan | stock Cog 48 of 133, our VM 45 | the tests use SQLite now (the machine has no PostgreSQL); second round: "database is locked" in the pooled tests, and 3 tests that differ between the VMs |

The package run found these defects outside the packages:

| Defect | Where | Found by | State |
|---|---|---|---|
| The socket option primitives knew 6 names and gave a false success for each other name; a new socket had no `SO_REUSEADDR` | our VM | PharoMisc (multicast tests) | repaired in `SocketPlugin.cpp`; `sweep14-new11` is clean |
| No `NewFilePlugin` | our VM | pharo-newfile (8 tests skip on our VM only) | ported (POSIX side), reviewed two times; the row passes 42 of 42 on the two VMs; `sweep14-new11` is clean |
| `RBNamespace>>renameClass:to:around:` loses method changes | Pharo image | Chanel | image fix 31 |
| `Duration>>asDelay` rounds down, so a wait can end early | Pharo image | PharoMisc (Tasks) | image fix 32 |
| The uninstall of an object-specific MetaLink skips a node when two nodes collide in a set, and a later uninstall removes a method two times | Pharo image | a sweep of our VM (`LinkInstallerTest`, 2 of 13 batches) | image fix 33; the two new tests fail before and pass after on the two VMs |
| The identity hash generator used 32-bit arithmetic and did not read its start value from the image header | our VM | the work on the row above (the order of hashed collections differed from Cog) | repaired in `ObjectMemory`; `sweep14-new11` is clean |
| The stock `NewFilePlugin` reads past a buffer, writes into an Array and into a read-only object, and stops the VM for a copy of a closed handle | stock Cog | `scripts/cogdiff/newfile_plugin_checks.st` and `newfile_plugin_ours_checks.st` | local branch `fix/newfile-plugin-buffer-and-handle-checks`, built and tested on the patched Cog |
| `Socket>>setPort:` on a port in use gives no error | stock Cog | PharoMisc | local branch `fix/socket-listen-bind-result`, tested on the patched Cog |
| `SO_RCVBUF` 65536 becomes 8 MB; `SO_LINGER` cannot be set | stock Cog | `scripts/cogdiff/socket_options.st` | local branch `fix/socket-option-values`, tested on the patched Cog |
| Segmentation fault in the garbage collector in `TkThreadTest>>testFinalization` after the multicast tests bind their sockets | stock Cog | PharoMisc | open, cause not known |
| A load in a headless image waits at a dialog (merge or load; credentials) | our package driver | Hyperspace, row 51 | the driver answers the warning and uses the non-interactive credentials provider |
| The VMs of the package run had the full environment of the shell, with the API keys and tokens of the owner | our package driver and `scripts/vmrun.sh` | row 75 (see below) | repaired on 2026-11-17 at 13:30: the driver starts with a short list of variables, and `vmrun.sh` removes each variable with a name like a credential |

The first two rows are the only tests of rows 1 to 50 with a result on our
VM that is not the result on Cog.

**A key of the owner left the machine (2026-11-17, about 12:45 to 12:50).**
The tests of row 75 (`punt-labs/anthropic-sdk-pharo`) read the variable
`ANTHROPIC_API_KEY` from the environment of the shell that started the run.
46 tests on each VM sent a request with the value as the key to
`api.anthropic.com`, the address in the package.  The service answered
"invalid x-api-key" (401) each time.  The result files and the logs do not
have the value.  The environment had three more names of that kind
(`GH_TOKEN`, `GOOGLE_MAPS_API_KEY`, `CLAUDE_CODE_MESSAGING_TOKEN`).  A
search of the sources of each package clone for names of that kind found
only `ANTHROPIC_API_KEY`, in that one package.  The owner must decide if the
key gets a new value.  The VMs now start without these variables.

Found by the MuTalk row and open: our VM signals an error at a recursion
depth of 56000 frames, and Cog has no such limit
([vm-compat-bugs.md](vm-compat-bugs.md), the first entry).  The owner must
decide if the limit goes.

Found by the same row in stock Cog: at the old-space limit
(`--maxOldSpaceSize`) the VM stops with "Could not allocate new object in the
old space" and does not signal low space.  Cause, from the Cog sources:
`SpurMemoryManager>>sufficientSpaceAfterGC:` reports low space only when the
free old space is below the threshold of the image (400000 bytes).  One
scavenge can move more than that to the old space, and
`SpurGenerationScavenger>>copyToOldSpace:bytes:format:` then stops the VM.
The change for Cog is written, built and tested (see the list below).

Cog changes in work (each one is a local branch of `~/src/pharo-vm`; nothing
is pushed):

- A callback from another thread with the same-thread FFI runner: written,
  built and tested (`docs/upstream/cog-same-thread-runner-callback-from-other-thread.patch`,
  branch `fix/same-thread-runner-callback-from-other-thread`).  The patched
  Cog passes `TFUFFICallbackTest>>testCallbacksInOtherThread`.
- A failed `bind` of the listen primitives gives an error: written, built
  and tested with `scripts/cogdiff/socket_bind_in_use.st` (branch
  `fix/socket-listen-bind-result`).  The sweep of the patched Cog with the
  change is in work.
- Socket option values (an integer of 5 or more digits, `SO_LINGER`):
  written, built and tested: the patched Cog and our VM give the same answer
  for the 72 calls of `scripts/cogdiff/socket_options.st` (branch
  `fix/socket-option-values`).
- The low-space signal at the old-space limit: written, built and tested
  (branch `fix/low-space-signal-before-old-space-limit`,
  `docs/upstream/cog-low-space-before-old-space-limit.patch`).  With the
  change, `scripts/cogdiff/lowspace_recursion.st` gives `OutOfMemory` on the
  patched Cog, and stock Cog stops.  The sweep of the patched Cog with the
  change (`sweepcogp14-new7`) has the same result as the sweep before it.
- The stock `NewFilePlugin` (buffer checks, handles, memory maps): written,
  built and tested on the branch
  `fix/newfile-plugin-buffer-and-handle-checks`
  (`docs/upstream/cog-newfile-plugin-buffer-and-handle-checks.patch`).  The
  patched Cog and our VM give the same 36 lines for
  `scripts/cogdiff/newfile_plugin_checks.st`, and the 42 tests of the
  package pass.  The sweep of the patched Cog with the change
  (`sweepcogp14-new8`) has the same result as the sweep before it.
- The segmentation fault in the garbage collector in the PharoMisc row:
  stock Cog stops in 2 of 6 runs of the row with the fixes file of commit
  `5053c46f` (three sockets on one port).  The run command is
  `/tmp/pharo-work/g8/run.sh`, and the crash reports are
  `/tmp/pharo-work/g8/crash-cog-2.log` and `crash-cog-4.log`.  The mark
  phase of a full GC follows a field of an old object that names an address
  in new space.  The cause is not known.  An agent works on it with the leak
  checker of the VM.
- The segmentation fault of `MorphicWindowManagerTest` in an interactive
  session (1 of 3 runs): not started.

Not done, in order:

1. Run rows 51 to 200 of the package manifest (in work), then repair each
   row that does not pass (the package, our VM, or Cog).
2. `StMethodBrowserTest` failed in about 1 class run of 10 on stock Cog.  The
   cause is in our harness: the fake GUI lets the Morphic UI process run a
   World cycle in the middle of the work of the test process (a nested
   `TabSelectorMorph>>updateTabs`, recorded 2026-11-17).  The repair is
   written and in test: `MorphicRenderLoop>>doOneCycle` starts no cycle of
   another process while the test process is ready to run
   (`/tmp/pharo-work/patchwork/setup_fake_gui.gate.st`; the two runners
   register the test process).  With the first form of the repair, 90 of 90
   class runs on stock Cog were clean, and a full sweep of the patched Cog
   had no batch that stopped.  The repair is not in
   `scripts/pharo-headless-test/setup_fake_gui.st` yet.  Known limit: the
   rule acts when a cycle starts, so a test process that wakes in the middle
   of a cycle of another process can still run inside that cycle.
3. Do the same work for Pharo 13 (first sweep with the old runner: 27894
   passes of 28077, 2 failures, 131 skips, 50 expected failures).
4. Repair the flaky tests of the earlier sweeps.  The flaky tests passed in
   the sweeps of this day.
6. Fix the two open Cog defects.

Not found: the "Apple prompt on every run to allow network" that the owner
sees.  Checks that gave no prompt:

- The firewall is off and has no entry for the VM.
- Terminal has the local-network permission.
- No prompt came during a sweep of each VM, a LAN connection from each VM, or
  a start of the stock VM as an app.

The system log has policy checks for `org.pharo.vm` (the stock VM app) on
2026-11-16.  Ask the owner for the text of the prompt.

## Right now (2026-11-14) -- Linux CI green, Apple build repaired and run

### Stack-depth fix and interpreter-only sweeps (committed 2026-11-15)

The change that this block describes is one commit on `main`, made on
2026-11-15.  The commit has these files: `src/vm/Interpreter.cpp`,
`src/vm/Interpreter.hpp`, `src/vm/jit/JITMethod.hpp`,
`src/vm/jit/MachineCodeFrames.cpp`, `src/vm/jit/asmjit/AsmjitT1.cpp`, three
files in `docs/`, `scripts/test_stack_depth_limit.st`,
`scripts/repro/x86-poller-wedge.st` and `scripts/README.md`.

The stack-depth defect (full text in
[vm-compat-bugs.md](vm-compat-bugs.md), entry "2026-11-14 -- stack-overflow
limit ignored the heap Context chain"):

- The depth limit counted only the inline frames since the last process
  switch.  The scheduler preempts some recursions often.  Such a recursion
  never reached the limit, and the heap Context chain grew until the old
  space was full.
- `ProcessTest>>testStackOverflow` failed in both builds for this reason.
  The JIT build passed only because the recursion ended inside one time slice.
- The limit now counts the whole depth of the process (`frameLimit_`).  A
  table of 8192 entries records the depth of each process at a switch, so a
  resume does not walk the sender chain.  The key of an entry is the
  Process, and the VM holds the Process as a weak root.

First interpreter-only sweeps (JIT off, the iOS configuration), before the
fix, compared test by test with the arm64 JIT baseline of 2026-11-13 on the
same image files:

| Image | Tests | JIT | Interpreter | Wall time |
|---|---|---|---|---|
| Pharo 13 | 28077 | 27945 pass, 1 fail | 27943 pass, 1 fail, 2 timeout | about 19 min |
| Pharo 14 | 28412 | 28241 pass, 1 fail | 28238 pass, 1 fail, 2 error, 1 timeout | about 19 min |

The one fail on every build is `ZnClientTest>>testQueryGoogle` (live network).

Two tests are slow only on the interpreter.  The runner has an 80 s
watchdog:

- `NoUnusedVariablesLeftTest>>testNoUnusedTemporaryVariablesLeft`:
  interpreter 73 s, JIT 13 s.
- `ReleaseTest>>testNoShadowedVariablesInMethods`: interpreter 270 s, JIT
  46 s.

The ratio is 5.6 to 5.9.  Both tests answer correctly when no limit applies.
With `SUNIT_TIMEOUT_MULT=6`, both tests passed on Pharo 13 and Pharo 14 (two
runs of the two tests alone, 411 s and 425 s wall).  The whole-suite sweep
with this value was not run.

How to run an interpreter-only sweep.  The image is a prepped image (see
`CLAUDE.md`, "Running Pharo test suites").  `TOTAL` is 2159 for Pharo 14 and
2052 for Pharo 13:

```
rm -f /tmp/sunit_timeout_scale.txt /tmp/sunit_class_names.txt \
      /tmp/sunit_method_names.txt /tmp/sunit_run_completed.txt /tmp/sunit_batch.txt
cp scripts/pharo-headless-test/test_classes.txt /tmp/sunit_test_classes.txt
PHARO_CODE_ZONE_MB=192 PHARO_MAX_STEPS=4000000000000 \
PHARO_MAX_OLD_SPACE_MB=12288 SUNIT_TIMEOUT_MULT=1 \
TOTAL=2159 STEP=50 PER_BATCH_TIMEOUT=7200 RETRY_DAMAGED=1 RETRY_STEP=5 \
  caffeinate -i scripts/sunit-sweep.sh build-nojit/test_load_image <image> <outdir>
```

These are the settings of all sweeps of 2026-11-13 to 2026-11-15.  With
`SUNIT_TIMEOUT_MULT=6` the two slow tests do not time out.  Run one VM at a
time, and start no build during a sweep.

Final validation on 2026-11-15, before the commit (the final code, four
trees built):

- `build-rel`, Pharo 14 and Pharo 13: both sweeps are identical to the JIT
  baseline of 2026-11-13 (28241 and 27945 pass, 1 fail each).
- `build-nojit`, Pharo 14, four sweeps: 28238 to 28239 pass, 1 or 2 fail, 1
  or 2 error, 1 timeout.  The two slow tests are the expected differences.
  Two flaky tests give the other differences, one time each:
  - `StSpotterTest>>testOpenSpotterRefreshesPreviewOnce` failed in one
    sweep.  The test also failed in one of four pre-fix sweeps.
  - `TKTCommonQueueWorkerPoolTest>>testWorkerPoolDoesNotExceedPoolSizeWhenSchedulingTasksInParallel`
    gave an error in one sweep.  The test passed in four of four pre-fix
    sweeps.  In runs of the class alone, the test failed in 1 of 9 runs on
    the pre-fix binary and in 1 of 9 runs on the fixed binary.  The change
    is not the cause.
- `build-nojit`, Pharo 13, one sweep: 27943 pass, 1 fail, 2 timeout (the two
  slow tests).
- `ProcessTest>>testStackOverflow` passed in all five Pharo 14 sweeps (Pharo
  13 does not have the test).
  `SpHorizontalBoxLayoutTest>>testSpSearchInputFieldPresenterResizeCorrectlyWhenWindowWidthDecrease`
  passed in all four interpreter-only Pharo 14 sweeps (the first version of
  the fix failed it in 3 of 6 sweeps).
- `build-x86`, Pharo 14: batches 1-100 (5862 tests) and 1151-1200 (395 tests)
  have no difference from the baseline.  `build-nojit-x86`: batch 1151-1200
  has no difference.
- Linux in containers (`ubuntu:26.04`, GCC 15.2.0): JIT on and JIT off, on
  arm64 and amd64: 4 builds, no error.  Apple: `scripts/build-xcframework.sh`
  exit 0 (5 slices).  `xcodebuild` iOS Simulator and Mac Catalyst: BUILD
  SUCCEEDED.
- Regression test: `scripts/test_stack_depth_limit.st` (class
  `StackDepthLimitTest`, 11 tests).  All tests pass on the four trees.  8
  tests fail on the two binaries before the fix.  The 3 other tests fail on a
  build with the limit set to 28000.  No sweep image has the class.
- Speed (median of 3 interleaved runs against the pre-fix binary): up to 1000
  processes, at most +1.4% instructions and about 3% wall at most (the wall
  clock has a 10 ms step, and both signs occur).  `many-3000-2000`
  on `build-rel` +4.5% wall.  Details are in the vm-compat-bugs entry.

Not checked:

- A Pharo 13 x86 run, and a full x86 sweep (only the Pharo 14 batches above
  ran on the x86 trees).
- `xcodebuild` for an iOS device destination (the device slice was built by
  the xcframework script only).
- The cause of the flaky
  `TKTCommonQueueWorkerPoolTest>>testWorkerPoolDoesNotExceedPoolSizeWhenSchedulingTasksInParallel`
  (a `TKTTimeoutException` in about 1 of 9 runs on the interpreter-only
  build, with and without the change).
- The cause of the x86_64 wedge in 2% to 5% of runs of a multi-process
  script with a poller.  The defect is open and older than this change (the
  rate is the same on the binaries before the change).  The
  reproducer is `scripts/repro/x86-poller-wedge.st`, and the lead is in
  [deferred.md](deferred.md).
- The GC cost of the weak root alone, and the collection of a dead Process
  after only a scavenge.
- No sweep in CI and no run on an iOS device.


The `jit` branch does not exist after this day.  `main` has all commits of
`jit`, and `main` is the only branch.  Commit to `main` and push `main`.  Do
not push a branch with the name `jit`: the push makes the branch again.

Since 2026-11-15 GitHub protects `main`: GitHub refuses a force push to
`main` and refuses the deletion of `main`.  The protection also applies to
the owner of the repository.  A normal push works as before.  No status
check and no review is necessary.  To rewrite the history of `main`, first
set the protection to off (repository settings, "Branches"), and set the
protection to on again after the push.  Read the state with
`gh api repos/avwohl/iospharo/branches/main/protection`.

The checkout on this Mac keeps the directory name `~/src/iospharo-jit`.  The
older entries in this file that name `jit` tell where a commit was at that
time.  All of those commits are in `main`.

This day found two sets of build defects and fixed them.  The sweeps of the
JIT-off build and the stack-depth fix are in the first block of this entry.

### Linux CI (`df0ac300`)

The CI job "Fresh clone builds" failed on each push from `d5323560` to
`da8e3ab9`.  `9e7582bc` added `pthread_t handle = nullptr` and a use of
`towlower` with no `<cwctype>` to `src/vm/SDL2Sys.hpp`.  The Apple toolchain
accepts both, glibc and GCC do not.  `df0ac300` is the fix.  CI is green on
`main` and `jit` at `df0ac300`.

### Apple build

This Mac (`m4wohl`) has Xcode 27.2 since 2026-10-19.  Text in this file and
in the memory notes said that the host has no Xcode.  That text was out of
date, and no session did the check.  The last Apple app build before this
day was at `0f0828a2` (2026-08-17), 432 `src` commits before.

The first build on this day found three defects.  `349e8953` holds the
three fixes.  The new CI step then found a fourth defect (below the table).

| # | Defect | Cause | Fix |
|---|---|---|---|
| 1 | The JIT-off configuration (`PHARO_JIT_ENABLED=0`: iOS device and iOS Simulator slices) did not compile: about 300 errors in 12 files | Interpreter declarations inside JIT guards in `Interpreter.hpp`, uses of `jit::` names outside guards, and code after the file guard of `AsmjitT1.cpp`.  The machine-code-frame work added them, and no build used the configuration | Guards and guard splits in `Interpreter.hpp`, `Interpreter.cpp`, `Primitives.cpp`, `AsmjitT1.cpp`.  No declaration moved |
| 2 | The x86_64 half of the iOS Simulator app did not link: `sista::Lowering::lower` not defined | `SistaLowering_arm64.cpp` has a JIT-off fallback, `SistaLowering_x86_64.cpp` had none | The same fallback in the x86_64 file |
| 3 | The iOS device app did not link: `_pthread_jit_write_protect_np` not defined | The iOS 27.2 device SDK does not export the symbol.  `apple_shared.cpp` named the symbol in all builds, and the app links with `-all_load` | Only a JIT build names the symbol.  The file stops the compile if `PHARO_JIT_ENABLED` is not defined |

The fourth defect: `349e8953` added a CI step that compiles the JIT-off
configuration on Linux.  The step failed on its first run.  The CI runner
has Ubuntu 26 with GCC 15, and `Primitives.cpp` uses `std::remove` and 54
other standard algorithms with no `#include <algorithm>`.  A JIT build gets
the header through the JIT headers.  A JIT-off build with GCC 15 does not,
and `std::remove` then means the C function `remove(const char*)`.

The fix is the include.  The first container check of the CI step used
Ubuntu 24.04 with GCC 13 and did not show the defect.  Use the image of the
runner (`ubuntu:26.04` now) for a container check.

Checks at the final state of the sources:

- JIT-on trees `build-rel` (arm64) and `build-x86` (x86_64): the `size` line
  and the sorted symbol list of `test_load_image` are the same as before the
  fixes.  The fixes do not change a JIT build.
- JIT-off trees `build-nojit` (arm64) and `build-nojit-x86` (x86_64): build,
  `42 factorial`, and the WarpBlt probe (1440 calls, 0 errors) are correct.
  The arm64 JIT-off binary gave the same answers as the JIT binary on about
  60 expressions and 15 SUnit classes (1885 tests).
- Linux in containers.  Ubuntu 24.04 (GCC 13): JIT-off builds on arm64 and
  x86_64, the JIT-on build on arm64.  Ubuntu 26.04 (GCC 15): JIT-off and
  JIT-on builds on arm64, the JIT-off build on x86_64.  No errors.
- `scripts/build-libffi.sh`, `scripts/build-sdl2.sh`,
  `scripts/build-third-party.sh`, `scripts/build-xcframework.sh`: all end
  with exit code 0.  `PharoVMCore.xcframework` has the three slices.
- `xcodebuild`, Debug: iOS device (arm64, no code signature), iOS Simulator
  (arm64 and x86_64), Mac Catalyst.  All three: BUILD SUCCEEDED.
- Mac Catalyst app on a pristine Pharo 13 image, JIT on: `World imageForm`
  is 1024x736 and shows the menu bar, the Welcome window with its text and
  logo, and the taskbar.
- iPhone 18 Pro simulator (iOS 27.2), interpreter only: `World imageForm` is
  834x624 with the same content.  The simulator screenshot shows the Pharo
  display behind the gesture help sheet of the app.

Not checked: a real iOS device, Pharo 14 in the app, a signed archive,
GitHub issue #7.  (A SUnit sweep of a JIT-off build ran later on this day;
see the first block.)  These are in
[deferred.md](deferred.md) ("Platform validation status").

Facts about the host and the tools:

- The app build needs the Metal Toolchain component of Xcode.  The component
  was not installed.  `xcodebuild -downloadComponent MetalToolchain`
  installed the component (839 MB) on this day.
- The app has a sandbox and cannot read an image in `/tmp`.  The steps for a
  command-line start are in [building.md](building.md).
- `Frameworks/` now has the xcframeworks from the build scripts, with iOS,
  Simulator and Catalyst slices.  Before this day the directory had only
  the macOS slices from `scripts/build-macos-slices.sh`.
- CI has a new step that compiles the JIT-off configuration on Linux.
- `OCSpecialSelectorTest` through `eval` is not stable: one of its four
  tests gave an error in some runs and not in others, on our JIT binary and
  on Cog (a different test on Cog).  The sweeps do not show this.  No cause
  is known.

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

The four sweeps ran again at `d5323560` (the Windows x86_64 merge and the
full SDL2 function layer).

| image | arch | pass, recovery pass included | not passed |
|---|---|---|---|
| Pharo 14 | arm64 | 28197 | 2 |
| Pharo 13 | arm64 | 27930 | 2 |
| Pharo 14 | x86_64 | 28200 | 1 |
| Pharo 13 | x86_64 | 27932 | 2 |

The tests that did not pass: `testQueryGoogle` (4 sweeps),
`testOpenSpotterRefreshesPreviewOnce` (1 sweep) and the TaskIt test
`testWorkerPoolDoesNotExceedPoolSizeWhenSchedulingTasksInParallel` (the two
arm64 sweeps).  The TaskIt test is a known race (`docs/image_issues.md`).
An interleaved comparison of the batch that contains the TaskIt classes, 20
runs for each build, gave the same result for the build at `9e22a112`:
one of the two timing-dependent TaskIt tests failed in 5 of 20 runs there
and in 4 of 20 runs at `d5323560`.

The four sweeps ran again at `bcb49d42` (the `SDL_CreateWindow` fix for the
OpenGL flag).  In each sweep the only test that did not pass is
`testQueryGoogle`.

| image | arch | pass, recovery pass included | not passed |
|---|---|---|---|
| Pharo 14 | arm64 | 28198 | 1 |
| Pharo 13 | arm64 | 27931 | 1 |
| Pharo 14 | x86_64 | 28200 | 1 |
| Pharo 13 | x86_64 | 27933 | 1 |

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
- Fourth package run (VM at `32568c4b`, arm64): 151 packages load, 144 give
  a result on the two VMs.  Cog 37634 passes, our VM 37637.  Our VM gives a
  result for 147 packages and Cog for 146.
  - The third run (VM at `04959000`) showed one regression.  The SDL2
    review fixes made `SDL_CreateWindow` answer NULL for the OpenGL flag.
    The Bloc window host always sets that flag, so `pharo-graphics-spec-toplo`
    and `ducasse-myg` stopped with "Invalid window handle" and no RESULT
    line.  The stub accepts the flag again (`1ad5a041`), as the cocoa driver
    of libSDL2 does.  The two packages give 40/0/0 and the Cog result again.
  - The rows where our VM and Cog differ in the fourth run:
    `smalltalkweb-myprecious` and `pharo-cig-pharo-cig` (no RESULT on our
    VM, package defects, see below); `moosetechnology-mooseide` (no RESULT
    on the two VMs: a forked process gets `#result was sent to nil`);
    `exercism-pharo-smalltalk-test-runner`, `newapplesho-google-cloud-smalltalk`
    and `pharo-graphics-spec-toplo` (no RESULT on Cog only);
    `apptivegrid-soil` (one race); and four rows where Cog has one more
    timeout or failure than our VM.
  - Two failures that showed only in the third run are tests with random
    input: `EffectDistributionTest>>test002...` fails about 12 times in 100
    and `TbBoundsTest>>testAtRandom` about 10 times in 100, at the same
    rate on our VM and on Cog.  A seeded `Random` sequence is identical on
    the two VMs.
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
  - `pharo-graphics-pharo-sdl-experiments` (Cog 27 passes, our VM 21).  The
    SDL2 stub layer lacked `SDL_GetHint`, `SDL_GetTicks64`, `SDL_QueryTexture`,
    `SDL_RenderReadPixels`, `SDL_GetRenderDriverInfo`, the window position,
    and 80 other SDL2 calls that the images make (86 new stubs in all).  Fixed on branch
    `r6-sdl2-stubs`: see [sdl2-stubs.md](sdl2-stubs.md).  The result is 27/0/0
    on arm64 and x86_64.  The generic no-op for unknown `SDL_` symbols is gone.
    Branch `r8-sdl2-full` completes the layer: every public SDL 2.26.5 function
    has a stub or a documented reason (event watches and filters, threads,
    locks, timers, renderer scale, viewport, logical size and `SDL_RenderCopyEx`,
    surfaces, pixel formats, audio conversion, no-device families).  The
    differential scripts in `scripts/cogdiff/sdl2diff_*.st` give the same
    answers as stock Cog; see [sdl2-stubs.md](sdl2-stubs.md).
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
- Not verified in that session: the Swift app build, the GUI on
  Pharo 14, the 93-package run on Pharo 14, Windows and Linux.  (The text
  here gave "no Xcode" as the reason.  That reason was wrong: this Mac has
  Xcode since 2026-10-19.  The app build was done on 2026-11-14, see the
  top of this file.)

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
