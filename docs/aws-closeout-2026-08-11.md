# Closing out the x86 & arm AWS runs — 2026-08-11

The 2026-08-10/11 full-suite runs and 200-package A/B sweeps left exactly two
real findings open (`docs/arm-pkg200-2026-08-11.md`, section "The comparable
128"). Both are now root-caused and fixed. **Neither cause was what the
stop-point notes predicted**, and in one case the "fix" already committed for it
was measured to change nothing at all.

    rko281-porpoise            weak-ref retention   ->  a GC trace-bound divergence
    pharo-rdbms-pharo-sqlite3  FFI module lookup    ->  a missing environment variable

    da9159e9  fix(gc): a Context's dead stack residue was a GC root
    d209543d  fix(gc): cover every context slot the VM writes
    4a46413f  fix(ffi): the image saw no arch library directory
    919904cd  fix(ffi): search the platform lib dirs ourselves too

## 1. The "second weak-reference path" is a GC trace-bound divergence

`PropertyManagerTest>>testPropertyManagerValueWeakness` scored cog 14 P / 0 F
against our 13 P / 1 F, "Got 1 instead of 2". The previous session read that
message as "something is MISSING from `allInstances`" and started from a
young-space blind spot, which it then ruled out. The message means the
opposite.

### Reading the failure without touching the test

The test's three assertions all live in one method, so the message alone does
not say which fired. A runner that catches `TestFailure`, walks the sender
chain back to the test method's own context and prints `ctx sourceNode` plus
`ctx tempNames`/`tempNamed:` says it exactly — and changes nothing about the
run, which matters here because this family of bug is famously sensitive to
added statements:

    FAIL PropertyManagerTest>>testPropertyManagerValueWeakness [Got 1 instead of 2.]
      TEMP  count = 1
      TEMP  object = nil
      TEMP  property = a PropertyManagerTestObject

`object = nil` pins it to the SECOND assertion, and `count = 1` is the real
finding: a `PropertyManagerTestObject` was already alive when this test took
its baseline, and the GC under test is what finally reclaimed it. The test is
not observing its own object at all — it is observing the previous test's.

### What was holding it

Three probe upgrades, in the order they were needed:

    [ROOT-WATCH] PropertyManagerTestObject oop=0x… via operand-stack
                 slot 29/32 frame#9/10 #testPropertyManagerValueWeakness fp+3

`PHARO_WATCH_ROOT_CLASS` already named the root CATEGORY; naming the owning
FRAME and the offset inside it showed at a glance that the only direct root was
the live `property` temp. So the stale instance was reachable transitively, and

    [HEAP-WATCH] 0x…e88 <- parent 0x…be8 cls=Context slot=8/40
                 {stackp=2 liveSlots=6..7 slot=DEAD-RESIDUE
                  method=testPropertyManagerKeyWeakness}

named the holder: slot 8 of the materialized `Context` of the PREVIOUS test,
whose `stackp` is 2 — the slot is dead expression-stack residue. `[HEAP-CHAIN]`
(the full parent chain, printed after the mark fixpoint, with the terminating
root category) closed it:

    [HEAP-CHAIN] 0x… <- Context(0x…)[8]{… slot=DEAD-RESIDUE
                        method=testPropertyManagerKeyWeakness}
                     <- ROOT(activeContext_)

`PHARO_WEAK_SURVIVOR_PATHS` could not have found this: it prints chains only
for referents sitting in a weak slot, and this one is reached through a plain
Context slot.

### The divergence

`ObjectMemory::pointerSlotsOf` traced a Context's WHOLE slot array. Spur does
not — `numPointerSlotsOf:` special-cases MethodContext and returns
`CtxtTempFrameStart + stackp` (`src/ios/cointerp-cpp.c:57328`). Everything
above the stack pointer is unreachable by the language's own rules, so Cog
never marks it; we did, and a returned activation's leftovers kept a
collectable object alive for one more collection.

Our own `cacheGCClassIndices` still carries the comment describing the intended
behaviour — "only trace slots up to the stack pointer ... beyond ... contain
garbage from previous activations". The full scan replaced it to work around
the opposite hazard: a `stackp` too LOW makes the mark AND the post-compaction
pointer update skip LIVE slots, which shows up as `classIdx=0` crashes. That
hazard is real, so the bound could only be restored by closing it at the
writers:

  - `prepareForGC`'s two temp syncs already RAISE stackp to cover what they
    write;
  - `storeContextStackp` (new) nils the tail wherever the VM LOWERS stackp on a
    reused context — all three of materializeFrameStack's context-reuse paths;
  - `raiseContextStackpTo` (new) covers the single-slot writers,
    `setTemporary`'s write-through and `setOuterTemporary`.

`PHARO_CTX_TRACE_ALL_SLOTS=1` restores the old scan for bisecting.

An earlier session tried nil-ing the tail at the sync sites alone and recorded
it as "ruled out, does not fix these tests". That was correct and the reason is
worth keeping: residue can be written AFTER a sync, so clearing at the sync
points is not sufficient. The trace bound is what fixes it; the clearing is now
kept for a different reason — with the bound in place, slots above stackp are
no longer pointer-updated after a compaction, so leaving stale oops there would
be worse than leaving dead ones.

Result: porpoise 14 P / 0 F, byte-identical to stock Cog, on arm64 and on the
x86_64 build under Rosetta, and on the aarch64 Linux box:

    package                     cog                       jit          jit-only
    rko281-porpoise             pass=14  fail=0 err=0     pass=14  0/0     0
    pharo-rdbms-pharo-sqlite3   pass=122 fail=0 err=0     pass=122 0/0     0
                                                    (was pass=11 err=111)

### Regression coverage for a GC change

    1042 tests   context / closure / exception / process / weak / finalization
                 families at HEAD:  995 P / 0 F / 0 unexplained E
                 (the 46 ProcessTest and 1 WeakAnnouncerTest errors are probe
                 artifacts — those need the SUnit harness's own setup, and pass
                 46/46 and SKIP under it)
    5x5 rounds   FinalizationRegistryTest / WeakSetTest / WeakOrderedCollectionTest
                 in isolation: clean every round.  The single
                 `FinalizationRegistryTest>>testFinalizationWithMultipleFinalizersPerObject`
                 TIMEOUT in the full local run does not reproduce — it is inside
                 the harness's known timeout noise (the pre-fix arm run had 14
                 timeouts of its own, in other classes).
    full suite   macOS-arm64 and Linux-aarch64, plus the 200-package A/B sweep.

The only FAILs in the local full suite are `ReleaseTest`
(`testNoLiteralIsPinnedInMemory`, `testNoOrphanPackage`) — the documented
run-order-pollution family, which the pre-fix arm run also hit.  The 32 `Cly*` /
`FTTableMorphTest` errors are an artifact of this run's prep, which did not
inject `setup_fake_gui.st`; `x86-fullsuite.sh` does.

## 2. The sqlite3 failure was a missing environment variable

`c1d6eef7` (previous session) fixed `primitiveLoadModule`'s candidate-name list
— which appended only `.dylib`, on every platform — and was committed as "fixed
by construction, NOT verified on Linux". Re-run on a fresh aarch64 box with
that fix in: **pass=11 err=111, unchanged.** The candidate list was never
reached.

The package computes the path itself and gives up before the VM is involved:

    SQLite3Library>>unix64LibraryName
        (#('/usr/lib/x86_64-linux-gnu' '/lib/x86_64-linux-gnu' '/usr/lib64' '/usr/lib'),
         ((OSEnvironment current at: 'LD_LIBRARY_PATH' ifAbsent: ['']) substrings: ':'))
            do: [ :path |
                #('libsqlite3.so.0' 'libsqlite3.so') do: [ :libraryName |
                    | libraryPath |
                    libraryPath := path asFileReference / libraryName.
                    libraryPath exists ifTrue: [ ^ libraryPath fullName ]]].
        self error: 'Module not found.'

The hardcoded half is x86_64-only, and so is
`FFIUnix64LibraryFinder>>knownPaths`. On aarch64 the library lives only in
`/usr/lib/aarch64-linux-gnu`, so everything depends on `LD_LIBRARY_PATH` — and
**a Pharo distribution's `pharo` is a shell wrapper, not the VM**. It runs
`ldd` on the real binary, takes the directory holding its libc, and execs with
`$PLUGINS:$PLATFORMLIBDIR:/lib:/usr$PLATFORMLIBDIR:/usr/lib:$LD_LIBRARY_PATH`.
Measured on the box, same image, same directory:

    cog   LD_LIBRARY_PATH = /home/ubuntu/h3/lib:/lib/aarch64-linux-gnu:/lib:
                            /usr/lib/aarch64-linux-gnu:/usr/lib:
          libraryName     = '/lib/aarch64-linux-gnu/libsqlite3.so.0'
    ours  LD_LIBRARY_PATH = <unset>
          libraryName     = <err: Module not found.>

So the two arms of the A/B were never comparing VMs on this axis — they were
comparing environments. `ffi::ensurePlatformLibraryPath()` reproduces the
wrapper's variable at startup, using `dladdr` on a libc symbol as the
in-process equivalent of the wrapper's `ldd` (Linux only;
`PHARO_NO_PLATFORM_LIB_PATH=1` opts out). It also publishes the directories to
`getLibSearchPaths`, because glibc snapshots `LD_LIBRARY_PATH` at process start
and a later `setenv` would steer the image's finders but not our own dlopen.

After it: `libraryName` -> `/lib/aarch64-linux-gnu/libsqlite3.so.0` and
`sqlite3_libversion` -> `'3.45.1'`, i.e. a real callout through the native
library.

`c1d6eef7` stays — its candidate list is still right for bare module names, and
distributions do ship versioned sonames. It just was not this bug.

## 3. Where the x86 run stands

Every finding the 2026-08-10 x86_64 run raised is now closed, without needing a
second x86 box:

    x86 Sista deopt GC-unsafe        ce2dcdd2   (native x86 box, earlier)
    soccertheory XMLFileException    d1cd608e   confirmed shared, not JIT-only
    weak references (both signals)   88ce3fee + da9159e9
    multi-entry dispatch divergence  c5332248   (both arches)

The two fixes here are arch-independent C++ in the collector and in FFI startup,
and the x86 side of each was exercised locally: the `build-x86` tree under
Rosetta is the standing x86 gate (see memory `per-arch-backend-drift`), and
porpoise runs 14 P / 0 F there. `sqlite3` was never an x86 finding — the x86
sweep had that package in the load-failed bucket, and on x86_64 Linux the
image's hardcoded finder paths already name the right arch directory, which is
exactly why the bug only surfaced on aarch64.

A fresh x86 box would re-measure, not discover. Worth doing when the next batch
of VM changes lands, not to close these.

## 4. What this says about the harness

Two lessons, both cheap to act on next time:

  - **"Fixed by construction, unverified on the platform" is worth nothing.**
    It cost a box re-provision to discover the package was still 100% broken,
    and the commit message asserting the fix is now the misleading part of the
    history.
  - **The stock-Cog arm is a wrapper script.** Any A/B against it compares two
    processes with different environments unless something makes them equal.
    Library lookup was the visible case; CWD was the previous one (`d1cd608e`).
    When an arm diverges on anything environmental, diff what the two
    *processes* see before blaming the VM.

## 5. Box notes

The first close-out box (`i-0e84d6df890cce46c`) was terminated 45 minutes in,
mid-package-run: `Client.UserInitiatedShutdown`, i.e. the keep-alive reaper.
`provision.sh` registers the lease but only an actively-working Claude
heartbeats it — from the Mac that requires `AWS_LEASE_IID` in the environment
the hook sees, which a `provision.sh` invocation does NOT arrange for a control
session. Driving a box from this Mac needs an explicit periodic
`lease.sh beat <iid>` (or the exported env) or it will be reaped under you at
the 30-minute mark. The provision output does say so; it is easy to skim past.
