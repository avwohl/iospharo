# Branch consolidation: `jit-arm-ci` and `jit-arm-linux` folded into `jit`

**Date:** 2026-08-13
**Result:** the repository now has exactly two branches, `main` and `jit`.
`jit-arm-ci` and `jit-arm-linux` were deleted from `origin` after the audit
below established that nothing of value was lost.

This file exists so the deleted branches leave a readable trace. Nothing was
merged wholesale, because neither branch contained an independent line of
development — both were snapshots of the same work taken by CI builders on
other architectures.


## What the three branches actually were

There was never a three-way fork. `jit` is the trunk; the other two were
points on that same trunk that build machines happened to push from.

    branch            tip         date         relationship to jit
    ------            ---         ----         -------------------
    jit               2478cf98    2026-08-13   trunk
    jit-arm-ci        e194a011    2026-06-13   strict ancestor of jit
    jit-arm-linux     10d28cb8    2026-06-10   parent is an ancestor of jit;
                                               1 unique tip commit

Verification commands and their results:

    git merge-base --is-ancestor origin/jit-arm-ci origin/jit    -> true
    git rev-list --count origin/jit..origin/jit-arm-ci           -> 0
    git cherry origin/jit origin/jit-arm-ci                      -> no '+' lines

    git merge-base --is-ancestor origin/jit-arm-linux^ origin/jit -> true
    git rev-list --count origin/jit..origin/jit-arm-linux         -> 1

### `jit-arm-ci` — nothing to preserve

A strict ancestor. Every one of its commits is already reachable from `jit`.
Deleting it removed no content whatsoever. It appears to have been the ref
the arm CI job pushed to, left behind when the job stopped running.

### `jit-arm-linux` — one bot commit, almost entirely build artifacts

The single unique commit:

    commit 10d28cb83b0b5e228afad66b5d5e56d4f466f2f3
    author iospharo-x64-builder <builder@iospharo.local>
    date   Wed Jun 10 10:02:38 2026 +0000
    subj   wip(x64): autosave on spot-interruption @ 20260610T100237Z

This is an AWS spot-instance shutdown handler dumping its working tree before
the VM was reclaimed. It is not authored work. Of its 320 files:

    319 files    build-opt/ — CMakeCache.txt, CMakeFiles/, *.o.d dependency
                 files, compiler-probe binaries (a.out, *.bin). Build output
                 that should never have been committed and must not be merged.
      1 file     src/vm/Interpreter.cpp — the only real content, 6 one-line
                 changes, all of them printf format-specifier casts.

Four of those six were already present on `jit`, arrived at independently
(`Interpreter.cpp` lines 18381, 18392, 18459, 18470 as of 2478cf98). The
salvage below covers the remainder.


## The salvaged change, and why it only ever mattered on Linux

Every one of the six hunks does the same thing: cast an `int64_t` to
`long long` at a `%lld` conversion in a debug `fprintf`.

`Oop::asSmallInteger()` returns `int64_t` (`src/vm/oop.hpp:128`). Whether that
matches `%lld` depends entirely on the platform's `stdint.h`:

    macOS arm64 / x86_64     int64_t is `long long`   -> %lld matches, silent
    Linux x86_64 (LP64)      int64_t is `long`        -> %lld mismatches,
                                                        -Wformat warns

Both are 64 bits wide, so the runtime behaviour is identical everywhere and no
output was ever wrong. It is purely a diagnostic-cleanliness issue — and one
that is *invisible on Apple platforms*. That is precisely why the fix
originated on the x86-64 Linux builder and why the remaining sites survived so
long on a branch developed mostly on macOS.

Confirmed empirically with clang: `fprintf(stderr, "%lld", x)` where `x` is
`long` produces

    warning: format specifies type 'long long' but the argument has type
    'long' [-Wformat]

and produces nothing when `x` is `long long` or is cast.

### What was applied to `jit`

The bot's two unapplied hunks, plus three further sites on `jit` that have the
identical defect and that the bot never saw (they were written after the
autosave was taken). Four sites in total:

    Interpreter.cpp:18410    [TERM-P%lld]   ctx [0]sdr=... dump
    Interpreter.cpp:18434    [TERM-P%lld]   ctx[%d]: Class>>sel dump
    Interpreter.cpp:18450    [TERM-P%lld] SENDER CHAIN CYCLE DETECTED
    Interpreter.cpp:18476    [TERM] terminateCurrentProcess: ... pri=%lld

each rewritten from

    pri.asSmallInteger()                       -> (long long)pri.asSmallInteger()
    pri.isSmallInteger() ? ... : -1            -> ... ? (long long)... : -1LL

### What was deliberately *not* applied

The bot's sixth hunk touched `tryJITResumeInCaller` (now `Interpreter.cpp:24164`):

    long pVal = pri.isSmallInteger() ? pri.asSmallInteger() : -1;
    fprintf(stderr, "[CHAIN-BAIL] ... active_proc_pri=%ld\n", ..., pVal);

The bot changed the initialiser to `(long long)... : -1LL` while leaving `pVal`
declared `long` and printed with `%ld`. That is a no-op at best — the value is
immediately converted back to `long` — and it makes the code read as though
there were a mismatch when there is none. `%ld` with a `long` is already
correct on every target. Left alone on purpose.

Also left alone: `Interpreter.cpp:18373`, `pri.asSmallInteger() >= 60`, an
integer comparison with no format string involved.


## Recovering the deleted branches

The commits are unreachable from any branch but are not necessarily gone:

  - GitHub retains deleted branch tips server-side for a period and can
    restore them via the API or the UI.
  - The two tips are recorded here: `e194a011` (jit-arm-ci) and `10d28cb8`
    (jit-arm-linux). Either can be fetched directly by SHA while it survives
    server-side garbage collection.

Given the audit above, the only content worth recovering was the four-line
change already carried forward, so this should never be necessary.


## Build verification of the salvaged change

Both platforms were built from `jit` at `fa04dcd4` and the resulting VM was run
against a freshly downloaded Pharo 13 image.

    macOS arm64 (local)
      configure       headless SDL2 + system libffi; no third-party build needed
      build           168/168 targets, exit 0
      Interpreter.cpp compiled with zero warnings of any kind
      -Wformat        0
      binary          build/test_load_image, signed with JIT entitlements
      run             loaded the image, ran to completion, exit 0

    x86_64 Linux (AWS m6a.4xlarge, Ubuntu 24.04, 16 cpus)
      __INT64_TYPE__  long int   <- the condition that makes %lld warn
      build           clone-and-build.sh completed
      binary          ELF 64-bit LSB pie executable, x86-64, not stripped
      run             loaded the image, exit 0; JIT active --
                      3,198,708 activations (100% hits), 3173 OSR, J2J chains

The Linux box independently confirms the platform claim in the section above:
`__INT64_TYPE__` really is `long int` there, versus `long long` on Apple.

The 323 warnings in the macOS build are all pre-existing and elsewhere -- 312
`-Wunused-function` (mostly generated plugins) plus a few unused-variable and
sign-compare. None are in `Interpreter.cpp`.

### What was not captured

The Linux compiler's own warning output for `Interpreter.cpp` was not
retrieved. The box was reaped by the `aws_watch` lease reaper before
`clone-and-build.log` was copied off, because `provision.sh` warned that
`~/.ssh/aws-lease` was missing -- so the box could not self-heartbeat -- and
that warning was not acted on. The build itself had already completed
successfully; only the log was lost. Its smoke result survived in S3 at
`s3://iospharo-build-670060058357/x86_64-builder/smoke/result-20260813T185910Z.txt`,
which is the source of the Linux numbers above.

So the Linux side is verified to *build and run*, not verified to be
warning-free by direct observation. The narrower claim was checked in isolation
with clang: at a `%lld` conversion, a `long` argument produces

    warning: format specifies type 'long long' but the argument has type 'long'

and the same argument cast to `long long` produces nothing.

To capture the warning log on a future run, either generate `~/.ssh/aws-lease`
and register it on awohl.com per `scripts/aws/README.md`, or export
`AWS_LEASE_IID` / `AWS_LEASE_PROJECT` / `AWS_LEASE_REGION` when driving the box
from a local Claude.
