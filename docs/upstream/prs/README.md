# Pull requests for Pharo 15 and Cog, for review by the owner (2026-11-23)

This directory has 11 pull requests for 10 defects of the Pharo 15 image and
of the stock Cog VM.  Each defect is in the code of the image or of the VM,
not in a test.  The tests of this project found the defects.  Nothing here
was sent to a pharo-project repository: no push, no issue, no pull request.

For each pull request:

- `<name>.md`: the local data (branch, commits, issues, review notes), then
  the paste-ready text of the pull request under "Pull request text".
- `<name>.patch`: `git format-patch` of the branch, the copy that stays if a
  work tree is lost.
- `<name>.tests`: the tests and probes of the before and after runs.
- The branch `pr15/<name>`: in `~/src/pharo` (base `origin/Pharo15`) or in
  `~/src/pharo-vm` (base `origin/pharo-12`).  The work trees are in
  `~/pharo-work/wt/pharo-<name>` and `~/pharo-work/wt/vm-<name>`.

## The pull requests

| # | Pull request | Repository | Defect | Kind | Proof |
|---|---|---|---|---|---|
| 1 | [cogit-indexforselector-store-check](cogit-indexforselector-store-check.md) | pharo-vm | `Cogit>>indexForSelector:in:at:` stores a followed literal into the method with no store check; a later full GC can crash | crash | VMMakerTests test: FAIL before, PASS after (aarch64 case) |
| 2 | [spur-low-space-before-limit](spur-low-space-before-limit.md) | pharo-vm | At the old-space limit the VM stops with a segmentation fault and gives no low-space signal (pharo-vm#918) | crash | probe: stock VM segfault, PR VM `OutOfMemory`; VMMakerTests test FAIL before, PASS after |
| 3 | [typed-stores-write-barrier](typed-stores-write-barrier.md) | pharo-vm | The typed store primitives (and their machine code) write into a read-only object (pharo#10053) | wrong result, immutability | probe; VMMakerTests interpreter and JIT tests FAIL before, PASS after |
| 3b | [write-barrier-typed-store-tests](write-barrier-typed-store-tests.md) | pharo | Companion of 3: `TFFIBackend` float stores test the wrong object; `WriteBarrierTest` loses its expected-failure marks and gets 4 tests | wrong result | 6 tests FAIL on stock VM, PASS on the VM of 3 |
| 4 | [socket-listen-bind-result](socket-listen-bind-result.md) | pharo-vm | The listen primitive ignores a failed `bind` or `listen`: a UDP socket on a busy port gives no error | silent failure | probe: no error before, `SocketError` after |
| 5 | [socket-local-address-any-interface](socket-local-address-any-interface.md) | pharo-vm | `primLocalAddress` looks only at `eth0` and `wlan0`: 0.0.0.0 on macOS and on Linux with new interface names | wrong result | probe: `#[0 0 0 0]` before, the interface address after |
| 6 | [atomic-shared-queue-wakeups](atomic-shared-queue-wakeups.md) | pharo | `AtomicSharedQueue` with two or more readers loses a wake-up; a terminated reader can leave the queue locked | hang | 3 tests FAIL 3 of 3 before, PASS 3 of 3 after |
| 7 | [duration-asdelay-rounds-up](duration-asdelay-rounds-up.md) | pharo | `Duration>>asDelay` rounds down, so `Duration>>wait` can end early | wrong result | 1 test FAIL 3 of 3 before, PASS 3 of 3 after |
| 8 | [znurl-equals-query-order](znurl-equals-query-order.md) | pharo | Two `ZnUrl` with the same query fields in another order are not equal since Pharo 14 (a side effect of the fix of #11065) | wrong result, regression | 2 tests FAIL 3 of 3 before, PASS 3 of 3 after |
| 9 | [mc-load-method-stamp](mc-load-method-stamp.md) | pharo | Monticello installs a method from an image definition with the stamp nil or empty | wrong result | 3 tests FAIL 3 of 3 before, PASS 3 of 3 after |
| 10 | [settings-ston-stale-variable](settings-ston-stale-variable.md) | pharo | A settings file that names a removed instance variable makes the settings load signal `InstanceVariableNotFound` | error | 2 tests FAIL or ERROR 3 of 3 before, PASS 3 of 3 after |

## How the pull requests were tested

- Image: Pharo 15 build 141 (`b4d2dfd`), 2026-11-20.  Stock VM: Cog
  v12.0.6-beta (`bebdbaca8`).  Machine: macOS arm64.  Not tested on Linux
  or Windows.
- Image pull requests: `scripts/upstream-pr/stage-pr.sh` and
  `scripts/upstream-pr/load-pr.st` load only the definitions that the branch
  changes into a stock Pharo 15 image.  "Before" is an image with only the
  test part of the branch, "after" is an image with the whole branch.  Each
  proof test ran 3 times in 3 VM starts.  The regression classes ran on the
  base image and on the image with the change.
- VM pull requests: `scripts/upstream-pr/vm-pr-build.sh` builds each branch
  alone and puts the VM core and the socket plugin into a copy of the stock
  VM app.  The probe of each branch ran on the stock VM and on the VM of the
  branch.  The VMMakerTests tests ran in a VMMaker image of unchanged
  `pharo-12` with the test part only (before) and with the whole branch
  (after).
- All 11 pull requests together: the merge of the 5 VM branches
  (`pr15/all-five`, built alone) and a Pharo 15 image with the 6 image
  pull requests and the image fix entries of this project.  The whole
  Pharo 15 suite ran on that VM (2026-11-23): 40369 tests, 40278 pass,
  2 fail, 88 skip, 1 expected failure.  The skips are the same as on
  stock Cog.  The 2 failures:
  - `CodeSimulationTest>>testErrorCodeNotFound` needs the Cog change of
    `primitiveDoNamedPrimitiveWithArgs`, which is not in this set.
  - `ZnClientTest>>testQueryGoogle` asks Google for a search page and
    fails on the stock VM too on that day.
- The same suite on stock Cog with the image fix entries (2026-11-20) had
  the 4 `WriteBarrierTest` failures that pull requests 3 and 3b repair.
  No test that passes on stock Cog fails with the pull requests.

## Review state

Each pull request had a review by an agent that did not write it, and a
second round where the first round found a defect.  The findings that a
reviewer and the author did not agree on are in "Review notes" in the local
data of each file.

Before the send:

- The owner must sign the Pharo license agreement once
  (`CONTRIBUTING.md` of `pharo-project/pharo`).
- Pull request 3b goes in after a VM release with pull request 3.  Before
  that release, its four new tests and the two former expected failures
  fail on the stock VM.
- Pull request 2 says "Fixes #918".  The match is probable (the same log
  lines), not proven.
- Zinc also has its own repository (`svenvc/zinc`).  Pull request 8 can need a
  copy there.

## Defects found and not in this set

On 2026-11-23 the triage found 32 defects in the code of the image or of
the VM.  These defects are still in `Pharo15` and in `pharo-12`.  The 22
defects that are not in this set, by repository:

- pharo-vm:
  - the same-thread FFI runner and a callback from another thread (crash)
  - NewFilePlugin: checks of the buffer and of the handles (crash, memory corruption)
  - SocketPlugin: integer option values, and `SO_LINGER`
  - `primitiveDoNamedPrimitiveWithArgs`: the error code in the simulation
- pharo:
  - TaskIt: the worker pool loses a task
  - TaskIt: the order of `andThen:`
  - Reflectivity: the uninstall of an object-specific link (#20156)
  - Reflectivity: a link on a method that is on the stack (#20072)
  - `OCProgramNode>>bestNodeFor:` (#17231)
  - Fuel: global names with wide characters (Fuel#192)
  - Ring: `RGMC` class variable names
  - Ring: the chunk importer and a class trait
  - `OCSimpleFormatter` loses comments
  - `SHA1>>hashStream:` on a character stream, with no plugin
  - `ProtoObject>>pointersToExcept:among:`
  - `SDLOSXPlatform` releases an autoreleased string
  - `TaskbarMorph>>updateTasks` with native windows
  - Calypso `spawnQueryBrowserOn:` with the NewTools message browser
- pharo-spec/Spec: `SpCodePresenter`, browse of an undeclared name
- pharo-contributions/Geometry: intersections of ellipses and arcs

The triage data of the 43 candidates (classification, evidence in the
upstream source, upstream duplicates, and the verdict of a second agent
that tried to refute each one) is in
[../../results/upstream-triage-2026-11-23.json](../../results/upstream-triage-2026-11-23.json).

This work also found a defect that has no change yet.  The stock VM
v12.0.6-beta loads the VMMaker dependencies with libgit2.  6 of 11 such
loads stopped with a heap-corruption abort
(`nanov2_guard_corruption_detected`) in the TLS handshake of a clone or a
fetch.  See `scripts/upstream-pr/vm-pr-build.sh`.
