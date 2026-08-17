# Every test skip that was investigated turned out to be a bug

**Date:** 2026-08-17

The headless test runner used to carry a class-level skip list — names of test
classes that "hang" or "timeout" and are therefore not run. Upstream deleted it
in 2026-04 and said why, in the runner's own source:

    Skip list: empty — we run every non-abstract TestCase subclass.
    The 2026-04 policy removed all class-level skips so the test suite
    would actually exercise what real Pharo does; skipped classes were
    hiding root-cause VM bugs behind harness exclusions.

This branch pins that runner and runs everything. This file exists because the
evidence for that policy is worth keeping, and because it was about to be lost:
it lived only in commit messages on a submodule branch, which has now been
deleted.

The short version: **every entry anyone actually investigated was a VM defect,
not a bad test.** Not most of them. Every one. Several had been on the list for
months under a description that was simply wrong — "filesystem timeout" for a
class that runs in 0.36 seconds, "hangs" for a class that was only slow because
it was measured on an unoptimised build.

## The entries, and what was actually wrong

`FastStepThroughTest` — recorded as a hang. The VM silently terminated any
process that recursed past 4096 frames, and `testStepThroughLonger` recurses
10000 deep, so the process running it vanished and the process waiting on it
waited forever. With the limit raised: about a second, 10 of 11 passing.

The filesystem families — recorded separately over months as "timeout on file
watcher operations", "timeout writing to disk", "hangs in tearDown", "blocking
filesystem write", "filesystem persistence tests timeout on every test". Five
descriptions, one bug: `primitiveReaddir` returned the `.` and `..` entries, the
image turned them into real `FileReference`s, and every directory appeared to
contain itself and its own parent. Any tree walk climbed toward the filesystem
root, so `deleteAll` never terminated. Measured after the fix:

    Fuel (whole family)               never finished  ->  0.4s to 6.6s
    Epicea (whole family)             never finished  ->  6ms to 11.2s
    GlobalIdentifier*                 81s / 8 errors  ->  41ms, 9 of 9
    GlobalIdentifierFuelPersistence   100.6s          ->  28ms, 10 of 10
    MCDirectoryRepositoryTest         skipped         ->  15/15, 436ms
    OmSessionStoreTest                skipped         ->  22/22, 320ms
    EpLogTest                         skipped         ->  13/13, 295ms
    TonelWriterV2Test                 skipped         ->  19/19, 336ms
    DrTestsTestRunnerTest             skipped         ->  6/6, 243ms
    EDDebuggingAPITest                skipped         ->  27/27, 3.8s
    DiskFileSystemTest                skipped         ->  59 ran, 54 passed

`RSLinePlotTest` — recorded as a Roassal problem. A non-local return went to the
wrong activation whenever the same method appeared more than once on the stack,
so the `break` in `NSNiceLinearTicksGenerator` — four nested `valueWithExit`
blocks — never fired and the tick search spun forever. 31 ran, 28 passed, 31s.

`FileAttributesPluginPrimsTest` — recorded as a filesystem hang. It crashed the
VM instead: a directory handle is a plain `ByteArray` holding a `DIR*`, so the
`String` the test deliberately passes to `primClosedir:` looked like a valid
handle and was dereferenced. The VM now only passes pointers it issued to libc
directory calls, and sets the bad-argument error code the tests check for.

`ClyBrowserToolValidityTest` and `StDebuggerTest` — recorded as hangs. They were
being measured against a `CMAKE_BUILD_TYPE=Debug` build, whose flags in this
project are `-g` with no `-O` at all, 5 to 19 times slower than `-O3`. SUnit
reports a test that outruns its limit as an ERROR, `TestTookTooMuchTime`, which
in a batch summary is indistinguishable from a crash. On `-O3`:
ClyBrowserToolValidityTest is 25 of 25 in 42s where Debug gave 25 errors in
270s; StDebuggerTest is 60 of 61 in 59s where it had not finished in 280s.

`FFICallbackTest` / `FFICallbackParametersTest` — the callback bug was real and
is fixed: the VM woke the `TFCallbackQueue` handler process and then immediately
displaced it with the process blocked in the callout, so the callback never
answered. 2 of 2 and 11 of 11, each under 0.2s.

`SystemNavigationTest` (11 ran, 10 passed, 14s), `DiskFileAttributesTest` (24
ran, 22 passed, **0.36s**), `FileReferenceTest` (114 ran, 108 passed, 31s) — all
three were listed as hanging or timing out. None does. DiskFileAttributesTest at
a third of a second is the clearest case: it was recorded as a filesystem
timeout.

## Two live hazards for a no-skip policy

Running everything is right, but two classes will stop a batch run, and both
were found only by deliberately sampling classes that were *not* on the list —
which is the only way an unlisted hang is ever found. A batch killed by its
timeout loses every class it had not yet reached, so one unknown hang costs far
more than the class itself: two of them cost 340 of 1,675 classes in one run.

**`TFUFFICallbackTest` never finishes**, with or without `libTestLibrary.dylib`
present, and a control build confirms this predates the 2026-08-16 callback fix.
It was never on any skip list, so it was silently eating batch runs.

**`TFCallbacksTest` needs `libTestLibrary.dylib`**, which the Pharo VM
distribution ships and this project does not. Without it the image raises
`SymbolNotFoundError` inside a callback, the callback never answers, and the VM
spins in its nested interpreter — indistinguishable from a hang from outside,
and that is exactly how it was recorded for months. `scripts/build-ffi-test-library.sh`
builds it; the class then runs in about 0.4s, 8 ran, 4 passed, 2 skipped, 4
errors. All five tests pass individually on the same-thread runner; the four
errors are the worker-thread variants failing on the unimplemented
`primitiveCreateWorker`.

## Still genuinely unresolved

Kept honest rather than quietly dropped. These finish or fail for reasons that
are understood but not fixed:

- `ObsoleteTest` passes 3/3 in 37s, but `testTraitObsolete` modifies traits and
  that leaks into later classes in the same batch. The "hang" half of its old
  note was wrong; the contamination half stands.
- `NoUnusedVariablesLeftTest` is genuinely slow rather than broken — 3m50s on
  `-O3`, 2 of 3 with one error.
- `GlobalIdentifierWithDefaultConfigurationTest` does finish, in 81 seconds.
- `SocketStreamTest` and the other network tests can block on a machine with a
  live network.

## The lesson worth keeping

A skip list is a workaround, and it fails in the specific way workarounds fail:
it removes the signal that would have led to the bug. Four separate "known bad
test" entries here were four VM defects — a frame limit, a readdir bug, a
non-local-return bug and a quadratic `become` — and each sat hidden for months
because the harness had been taught not to look.

The general-purpose replacement is a timeout, which upstream now uses. The
difference is not that a timeout is nicer: it is that a timeout still *reports*
what timed out, so the failure stays visible and stays somebody's problem. A
skip makes a broken thing and a working thing look identical in the summary.
