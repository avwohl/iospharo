# Deferred Items (consolidated)

Single source of truth for currently-open work.  Anything shipped
is logged in `docs/changes.md`; this file lists only what's *not*
done.

Detailed session-by-session narratives for closed investigations
live in `git log` and `memory/*.md` — not here.

---

## Recently-closed (one-line breadcrumbs only, per the preamble)

Full narratives in `git log` and `memory/*.md`.  Listed by old
section number for git-archaeology continuity.  Note: the
"old A3" and "old A4" below are *not* the same as A3/A4 in the
current "## A. Open VM issues" section — they share the
letter+number by accident across two numbering generations.

- **JIT default flip** 2026-05-15 (`b5a7c837` + `d2566953`).
  Asmjit-T1 replaced the legacy stencil JIT as the default path.
  Phase4b.38 routes through `DebugSettings::useAsmjitT1` (default
  true; `PHARO_NO_ASMJIT_T1=1` reverts).  Phase4b.39 added the
  immutable-bit check to popStoreRecv so JIT-compiled setters
  honor `beReadOnlyObject` (ObjectTest 26/28 → 28/28).
- **A0a** FFI `C11 class >> #current` DNU cascade — APPEARS FIXED
  2026-04-22.  Cause likely one of `17a0ff7 / 61eef4f / cafe6a2`.
- **A0b** Other stock-Cog-passing error buckets — APPEARS FIXED
  2026-04-22 alongside A0a.
- **A00** `StringTest>>testSelect` — interpreter timeout, perf
  not correctness.
- **(old) A3** JIT sequential-test regression — NOT REPRODUCING
  post commit `0bd8c501` (the +1 sp leak fix).
- **(old) A4** IntegerTest hang under JIT — root-caused 2026-05-07
  to `IntegerTest>>testNthRootTruncated` only; workaround landed.
- **A0 (weak-ref GC: testClearing)** — RESOLVED 2026-04-20
  (generational GC + finalization signal semantics).

## A. Open VM issues

Surfaced 2026-04-19 by removing the harness skip list and the JIT
auto-disable in test/eval mode.  The previous "clean" test suite
runs were hiding these behind workarounds.

**JIT-off state (2026-04-19 after scheduler + OOM + profiler + timeout fixes):**
Interpreter-mode test suite is effectively clean.  Each of the four
failures surfaced by the no-skip probe has been fixed or characterized:

- `AllocationTest>>testOneGWordAllocation` → **PASS**.  Fixed by
  PrimErrNoMemory_ primFailCode in basicNew:/basicNew (commit
  `8d69724`).  Image raises `OutOfMemory`, test catches it.
- `AndreasSystemProfilerTest>>testSimple` → **PASS**.  Fixed by
  implementing `primitiveProfile{Semaphore,Start,Sample,Primitive}`
  (commit `6d21105`).
- `BehaviorTest>>testAllReferencesTo` → **PASS** (9.6 s).  Fixed by
  the 10× timeout multiplier when PHARO_NO_JIT is set (submodule
  `150bbd4`).  Was 8 s > 8 s previously, now 8 s > 80 s.
- `ArrayTest>>testPrintingRecursive` → still **TIMEOUT** at 80 s.
  Pathological case: stuck in `InstructionStream>>scanFor:` inside
  the recursive printOn: chain.  Needs interpreter perf work or JIT
  correctness work — not a tractable single-session fix.

### A0. Chunk-format `methodsFor:` incompatible — RESOLVED (harness converted)
Pharo 13 / 14 removed `#methodsFor:` from `ClassDescription`, so
chunk-file headers like `<bang>SUnitRunner class methodsFor: 'cat'<bang>`
never worked cleanly — the harness shipped that way but the image's
CodeImporter evaluated every chunk as a plain DoIt, so multi-line
method bodies with temps / nested blocks silently failed to install.

Fixed by converting `run_sunit_tests.st` and `setup_fake_gui.st`
from chunk format to explicit `Class compile: 'source' classified:
'cat'` calls (submodule commits `17bd98a`, `f50f2d6`).  With the
converted harness:

- JIT off, no skips: first ~35 test classes exercised end-to-end
  with a mix of silent passes, genuine 8-s timeouts on known-slow
  tests (e.g., `ArrayTest>>testPrintingRecursive`), and a few real
  PrimitiveFailed errors (e.g., 1-GB allocation).
- JIT on, no skips: immediate DNU cascade during boot — the bugs
  below.  Harness never reaches the test runner.

Preserved the `methodsFor:` shim at the top of the harness for
belt-and-suspenders.

### A1. JIT eval-mode hang at PHARO_JIT_DEFER=0 — **FIXED 2026-05-08** (`2ee2495b`)
`eval "42 printString"` runs cleanly at every DEFER setting,
including `PHARO_NO_DEFER_CLAMP=1 PHARO_JIT_DEFER=0` (5/5 stability
in commit msg, re-verified 2026-05-08 after this build with the
asmjit-patches CMake hook in place).

**Mechanism (Resolver class-var-set signal):** the C++ side sets
`g_resolverClassVarSet` and `g_resolverSetAtStep` when interp /
JIT see `popStoreLitVar` write a non-nil value into the class-var
binding whose key is the Symbol `Resolver`.  That moment is line 1
of `FileLocator class>>startUp:`:

    Resolver := InteractiveResolver new.

`noteMethodEntry` then holds defer until step >= resolverSet +
`kResolverBufferSteps` (~3s, env-tunable via `PHARO_RESOLVER_BUFFER=
<seconds>`).  The remaining startup work — `addResolver:` calls,
RFB display preferences — fits inside the buffer, so JIT engages
post-init even with `PHARO_JIT_DEFER=0`.

**Sweep at PHARO_NO_DEFER_CLAMP=1 (2026-05-08, post-2ee2495b):**

  DEFER=0:   5/5 — Resolver+3s gates the lift cleanly
  DEFER=1:   5/5
  DEFER=2:   5/5
  default:   5/5 (auto 4s deferSteps + Resolver+3s; Resolver wins)

5 small evals (`42 printString`, inject:into: 1k, OC>>size, etc.)
× both default and DEFER=0+NO_CLAMP all 3/3.

**SUnit A/B (2026-05-08):** 14 collection/number test classes
(Array+String+Dict+Set+DateAndTime+OC+Symbol+SmallInteger+
Fraction+Time+LargeInteger+ByteArray+Interval+SortedCollection),
fresh harness image, JIT engages mid-test (defer lifts at step
~120M):

  default DEFER:           2485 tests, 2485 P, 0 F, 0 E
  DEFER=0+NO_CLAMP:        2485 tests, 2485 P, 0 F, 0 E

100% parity.  Resolver-buffer signal at the new 4s default is
reliable across both eval and SUnit-harness modes.

**Resolver buffer length:** 4s is the production default (was
3s; bumped 2026-05-08).  Empirical sweep:

  Cold-eval init (Resolver fires at step ~35M):
    1s buffer  0/5  hangs in DelayMicrosecondTicker
    3s buffer  5/5  passes
    4s buffer  5/5  passes

  SUnit-harness resume (Resolver fires at step ~181K because
  the saved image is mid-session):
    3s buffer  0/14 — SymbolTest `SubscriptOutOfBounds 1 in
                       WeakArray` cascading to runSingleTest:
                       termination
    4s buffer  14/14 (2485/2485 tests pass)

The two modes need different absolute lift-step values because
Resolver-fire timing depends on whether init is cold or
resuming a saved image.  4s buffer covers both.  See
`memory/project_resolver_buffer_3s_floor_2026_05_08.md`.
PHARO_RESOLVER_BUFFER=<seconds> for diagnostics.

**Headless 3s clamp floor** (`kHeadlessFloor` in
`JITRuntime::noteMethodEntry`) is now redundant when Resolver
fires (which it always does in headless+non-bench mode, since
it's part of normal Pharo init).  Kept as belt-and-suspenders for
the "Resolver never fires" fallback path.  Removal is safe to
attempt once we've confirmed every headless path triggers
Resolver.

**Original +1 sp leak fix (commit `0bd8c501`):** C++ ExitArrayCreate
handlers did `instructionPointer_ = state.ip` before `+= 2`, but
`stencil_pushArray` doesn't update `state.ip` — IP ended up at
offset 2 instead of past PushArray, looping through pollEvent:'s
body forever and leaking +1 sp slot per cycle.

**14-session NLR-fallthru bug (commit `33e2ae18` + `ac6df64d`):**
JIT-compiled `Symbol class>>intern:` returned the receiver (Symbol
class) via bc 99 returnSelf fall-through instead of bc 89 early-
returnTop carrying the freshly-interned Symbol.  Mitigated by
JIT-side compile-time rewrite of trailing `pop+returnSelf` as
`returnTop` for FullClosure methods, plus an interp-side peephole
at Pop dispatch.

### A1d. FFICallbackTest qsort tests slow under pure interpreter (2026-04-23)

`FFICallbackTest>>testCqsort` and `>>testCqsortWithByteArray` do NOT
complete within 30s under `PHARO_NO_JIT=1 PHARO_NO_SISTA=1`.  Not a
hang — progress continues at ~2.6M steps/sec (normal interp speed).
Simply needs more steps than the test harness allows.

**Root cause (2026-04-23 investigation)**: Default
`TFCallbackForkRunStrategy` creates a NEW Process per callback
invocation via `forkAt: Processor highIOPriority - 1`.  Each
Process creation walks through:
  `valueUnpreemptively` → `priority:` → `interpriorityYield:` →
  `fork` → `newProcess` → `forContext:priority:` → `priority:`
…and that entire chain runs ~30M interpreter bytecodes per callback
invocation (measured via `PHARO_CALLBACK_DEBUG=1`).  For qsort of
19 elements doing ~60 comparisons, that's ~1.8 BILLION bytecodes =
minutes of pure-interp execution.

Tried patching `TFCallback>>runStrategy` to use
`TFCallbackSameProcessRunStrategy uniqueInstance` — the strategy
patch takes effect (verified: `cb backendCallback runStrategy class`
= `TFCallbackSameProcessRunStrategy`), but that class's
`executeCallback:on:` only uses the cheap persistent-process path
when the worker is in `waitForever`; after the first invocation the
worker is in `executeCallback:`, so subsequent calls fall back to
`super executeCallback:` = fork again.  Net: no measurable speedup.

**Mitigation**: Under default JIT (`PHARO_JIT_DEFER=4s`), the fork
chain is JIT-compiled and runs ~30× faster, so the test completes
in reasonable time.  Under `PHARO_NO_JIT=1` the test needs a longer
timeout (120s+) or should be skipped.

**Callback infrastructure itself works correctly** — confirmed via
direct `FFICallback signature:block:` invocation of a single
callback, and via `CALLBACK-HANDLER enter` / `CALLBACK-RETURN`
round-trips in the test.  The issue is purely perf from the image's
chosen dispatch strategy.

All other FFI tests pass on NO_JIT: FFICalloutTest 6/6,
FFIExternalStructureTest 12/12, FFICalloutAPITest 18/18,
AthensCairoMatrixTest 17/17, etc.

Low priority.  Production callback paths (asmjit WebKit, Morphic
event dispatch) don't hit the qsort-style many-call-in-tight-loop
pattern.  Upstream candidate: `TFCallbackSameProcessRunStrategy`
should reuse the persistent process across successive calls instead
of falling back to fork after the first one.

### A1b. FFI `invokeFunction:withArguments:` receiver corruption — RESOLVED 2026-05-08

Was: `FFICalloutAPITest>>testByteArrayToExternalAddress` hung at both
default DEFER and DEFER=0.  Bisection had pinpointed compile #13 =
`WriteStream>>nextPut:` as the trigger, and the doc said this was A1
observed through a different path.

Re-tested 2026-05-08 with the Resolver-buffer (4s) defer-lift signal
in place: full `FFICalloutAPITest` passes 18/18 at BOTH default
DEFER and `PHARO_NO_DEFER_CLAMP=1 PHARO_JIT_DEFER=0`.  The original
"compile #13 hangs" reproducer no longer fires — A1's fix
(2ee2495b Resolver signal + 33e2ae18/ac6df64d/498003df NLR-fallthru
peephole + the buf=4s production default) covers this case too.

### A1c. `forkAt:` sentinel — RESOLVED 2026-04-19 (was scheduler starvation)
The watchdog's `testProcess suspend` DNU with `rcvr=0x300000000` was
a symptom of scheduler starvation, not a memory bug.  The test
process never actually ran; the watchdog fired its timeout handler;
the `testProcess` local temp was still `nil` (rawBits=0) because
the fork's local-temp array index read uninitialized header space
that *happened* to look like `0x300000000`.

Root cause was `primitiveRelinquishProcessor` yielding to highest
priority instead of same-or-lower, plus sleeping 10 ms unconditionally
before considering transfer.  Fix in commit `a2b99f7` — a 2 ms
`AIAstarTest` test now runs in 2 ms via `runSingleTest` (was 23.6 s).

### A3. SUnitRunner-prepped image leaves stuck handler processes — RESOLVED 2026-05-09 (commit 56ab93ce)

**Fix**: in eval mode, test_load_image touches
`/tmp/sunit_run_completed.txt` BEFORE launching the image.
SUnitRunner>>startUp: sees the marker and returns early
without forking watchdog/test processes.  SessionManager
completes the startup chain; StartupPreferencesLoader runs
our startup.st; eval completes.

  prepped 42 printString:    0/3 → 3/3
  prepped 36 benchFib:       0/3 → 3/3
  SUnit 5-class harness:     1197/1197 unchanged

The fix is coupled to our runner-script's marker convention,
but the runner script and test_load_image are sister components
of the harness — they share other conventions (e.g. startup.st
generation).  Acceptable coupling.

**Original analysis** (preserved):

**Updated 2026-05-08 PM** — narrowed further from the morning finding:
the issue is NOT a JIT correctness bug.  After `pharo Pharo.image eval
--save "fileIn run_sunit_tests.st"`, the saved image has SUnitRunner
registered as a SessionManager startUp handler.  On every resume —
including non-test eval invocations — that handler fires and forks
processes (typically 2: P80 + P60) that suspend in
`SUnitRunner class>>ifFalse:` and never wake up.

These suspended processes leave their context's sender slot pointing
to image-allocated-at-load-time `nil` oop (which decodes as e.g.
`0x300000000` in the address space layout).  Our existing
`terminateCurrentProcess` C++ diag treats `ensure:` and high-pri
process terminations as "exceptional" and prints `[TERM-P*]
PROCESS TERMINATING` — but for these forked watchdog processes,
termination via ensure: at the top of their stack is normal end-of-
process behavior.  The 0x300000000 sender is just `nil` in this
run's address-space (s_nilBits is set per-image-load).

The actual problem with these processes is that they're ALIVE and
SUSPENDED, holding higher priority than the eval process — so any
eval after a prepped-image resume can't complete:

  Fresh image (no `--save fileIn`):
    eval "[...] ensure: [...]. 42 printString" × 5: 5/5 clean
    eval "1 + 1" × 5: 5/5 clean
    eval "FFICalloutAPITest new testByteArrayToExternalAddress" × 5:
                                                            5/5 clean

  SUnitRunner-prepped image (after `eval --save "fileIn ..."`):
    eval "1 + 1" / "42 printString" / "[...] ensure: ...":
      always TERM-P40 in BlockClosure>>ensure: (ctx [4]clo=nil-oop;
      this is normal end-of-process for a forked watchdog block
      whose ensure: fires when its work completes)
      AND eval expression NEVER prints its result — eval
      times out at 30s with `[DIAG-QUEUE] P80 proc=... susp=
      SUnitRunner class>>ifFalse:` and `P60 ... susp=
      SUnitRunner class>>ifFalse:` showing two stuck processes.

So the bug is in our runner script (`scripts/pharo-headless-test/
run_sunit_tests.st`) leaving processes alive in
`SUnitRunner class>>ifFalse:` after the session-startup handler
returns.  Those high-priority suspended processes hold the
scheduler past whatever the eval needs to complete.

**Implications for "no defer":**
- Fresh-image PHARO_NO_DEFER_CLAMP=1 PHARO_JIT_DEFER=0 is fully
  reliable.  The "no defer" goal is achieved.
- Past validation runs (14-class A/B 2485/2485) were on prepped
  images.  Tests passed because they ran inside the SUnit harness
  loop, which iterates regardless of the stuck watchdog processes.
  But individual evals after prep can't complete.

**Workaround**: validate via direct eval on FRESH image (clean
download).  Use SUnit harness only for SUnit batch validation.

**Real fix**: audit `run_sunit_tests.st`'s SessionManager startUp:
handler for forked watchdogs that don't terminate when the test
batch is complete.  Likely candidates: the per-test fork+watchdog
pattern leaves a watchdog `[doneSem wait] forkAt: 81` (or similar)
running after the test process completes.  Add a `terminate`
on the watchdog after `doneSem signal`.

The C++ TERM-P diag (line 11633) currently fires for `ensure:` as
"exceptional", which is misleading — block-process completion
through ensure: at top-of-stack IS normal.  Could narrow the
exceptionalTerm filter (ensure: only when context chain has cycles
or non-nil-but-bad sender), but that's cosmetic.

### A4. J2J-frame-push sites discarded activeContext_ — RESOLVED 2026-05-09 (commit 16bc4ff7)

**Root cause**: 8 sites in Interpreter.cpp that push savedFrames_
entries for J2J-activated methods set
`frame.savedActiveContext = nil` unconditionally, throwing away
the real activeContext_ at the time of activation.  When popFrame
later restored from these frames, activeContext_ became nil; the
nil propagated through subsequent pushes and pops; eventually
`materializeFrameStack` ran with activeContext_ = nil; frame[0]'s
new context got sender = nil; the next return bytecode hit the
top-of-chain check → terminate.

**Fix**: each of the 8 J2J-push sites now saves activeContext_
instead of nil.  popFrame restores correctly.  Sender chain
through deep J2J recursion stays intact.

**Validation at J2JSlotPerEntry=32** (the original value, restored
because the bug is now fixed at root):

  benchFib(28..40):     each 3/3 (was: ≥36 failed ~80%)
  1 tinyBenchmarks:     3/3 (was: 0/3)
  42 printString:       3/3 unchanged
  inject:into: 1M:      3/3 unchanged
  FFICalloutAPITest:    3/3 unchanged
  SUnit 5-class:        1197/1197 unchanged

**Earlier ~mitigations~ now redundant** (kept for safety):

- J2JSlotPerEntry 32→64 (commit 836c3392): worked by avoiding the
  J2J-fallback materialization path that triggered the bug.
  Reverted to 32 in the root-cause fix.
- materializeFrameStack walks all savedFrames_ for sender anchor
  (commit 20108c77): when activeContext_ was nil at materialize
  time, this walked forward to find any non-nil savedActiveContext.
  No longer load-bearing (savedActiveContext is no longer nil),
  but kept as defensive belt-and-suspenders.

**Pre-fix investigation findings** (preserved for archaeology):

The bug is in our JIT compilation of `Integer>>benchFib` via the
J2J chain materialization path.

**Reproducer + trigger condition:**

  `eval "N benchFib"` with default JIT, fresh image:
    N=30..35:  5/5 always succeed
    N=36:      1/5 succeed (4/5 fail)
    N=37+:     fail rate increases

  Adding `benchFib` to JITRuntime's `alwaysExcluded` list:
    N=36:      10/10 — confirmed JIT-compiled benchFib is the bug

  Running with PHARO_NO_JIT=1: works always, all N.

**Failure signature:**

  [TERM] terminateCurrentProcess: proc=... pri=40 fd=0 method=#benchFib
  [TERM]   fd=0 #benchFib (current)
  [TERM]   C++ caller: Interpreter::returnValue(Oop)+7184

  The eval process terminates while EXECUTING benchFib.  The
  C++ caller offset corresponds to the top-of-call-chain
  termination path in `Interpreter::returnValue` (line ~5200),
  fired when `activeContext_` has nil sender at return time.

  At the recursion-depth boundary (36), one of the recursive
  benchFib activations gets a corrupt sender slot (=nil) when
  it should chain back to the outer call.  When that
  activation returns, the runtime sees nil sender → top of
  chain → terminate process.

**JIT stats (success vs fail are nearly identical):**

  compiled: 1 method
  J2J stencil: calls=40.95M, returns=0, patches=0-2
  activations: ~48-49 hits/100%
  resume (J2J-r): 3-9 chains/100%
  materialize: count=0
  chain: actChain=0 actFall=0 | primChain=0 primFall=0

  J2J calls at 40.95M ≈ benchFib(36) result (48.3M) × ~0.85.
  Counter overflow possibility (32-bit at 32M, 24-bit at 16M)
  unconfirmed.  The j2jDepthLimit adaptation (2→8 over clean
  runs) and materializeJ2J may interact poorly at the
  recursion-depth-36 boundary.

**Bisection that did NOT find the cause** (all neutral or
slightly worse at small samples):
  - PHARO_NO_OSR_RECOMPILE=1
  - PHARO_NO_J2J_INLINE_BUMP=1
  - PHARO_NO_J2J_CALLEE_BUMP=1
  - PHARO_NO_LATE_SPEC_RECOMPILE=1
  - PHARO_NO_QUEUE_COMPILE=1
  - PHARO_NO_SISTA=1

**Real fix path:** lldb single-step a benchFib(36) failing
run inside the JIT-compiled stencil on the recursive call's
return path; identify which activation has the corrupted
saved-sender field, find the offset/expression in the JIT
output that miscomputed it.  Multi-day work; the binary is
codesigned for attach.

**Mitigation in tree (not a fix):** Morphic-process
suspension in startup.st (commit f497f115) eliminated the
non-bug-related `36 benchFib` failures (0/10 → 7/10).  The
remaining ~3/10 are this real JIT bug.  PHARO_NO_JIT=1 is
the workaround for benchmark-style workloads.

**tinyBenchmarks status:** still 0/3 even with
benchFib-excluded.  A different bug — probably in
`Integer>>benchmark` (called by tinyBenchmarks first) or in
the `[t < 1000] whileTrue: [n := n * 2]` loop pattern.  Not
investigated.

### A2. B5 cold-IC DNU cascade at PHARO_JIT_DEFER=0 — RESOLVED 2026-05-08

Was: at `PHARO_JIT_DEFER=0`, `decodeBytes:` `bc[2] popStoreTemp 1`
stored a SmallInt into `byteStream` (PositionableStream J2J return-
stack corruption on `class>>on:` → `>>on:` chain), producing DNU
on `#atEnd`.  The 2026-04-22 finding had narrowed it to "Morphic
preempting startup mid-`fields readStream`", concluding A2 ⊆ A1.

Re-tested 2026-05-08 — confirmed.  `eval "42 printString"` at
`PHARO_NO_DEFER_CLAMP=1 PHARO_JIT_DEFER=0` passes 5/5 (PositionableStream
init is now well before defer-lift, since Resolver+4s buffer holds
JIT off until step ~155M, far past the early-startup window where
the cascade triggered).

Diagnostic tooling (`_HOLE_RT_J2J_TRACE`, 512-slot ring buffer,
auto-trigger on SmallInt return) is still in tree from
`f70ad55/cf6ffaf/b94c0a8` if the cascade ever re-emerges.

### A5. arm64 bench-suite flaky hang at `runCollect` — 2026-05-17

Bench-suite returning to arm64 after months on Linux x86 surfaces an
intermittent hang at `runCollect` (the 12th benchmark in the
PharoBenchmarkRunner sequence).

**Repro:** `run_benchmarks.sh` with `PHARO_JIT_DEFER=15`.  Hits at
~80%; the file ends with `--- collect(100K) ---` and no result line,
main thread parked in `primitiveRelinquishProcessor → usleep` per
lldb attach.

**Not a JIT codegen bug.** Verified by running the same bench with
`PHARO_JIT_DEFER=60` (effectively no JIT) — still hangs ~80% of the
time, same lldb signature.  The bug is in the Smalltalk image
scheduler interacting with the high-priority fork
(`Processor highestPriority - 1`) that the bench-suite's
SessionManager startUp: handler creates.

**Env-var workarounds explored, all flaky:**

- `PHARO_NO_SISTA_DO_SPLICE=1`           bench completes (collect ~250ms),
                                         but sieve regresses from 8 ms
                                         to ~120 ms (the splice fires on
                                         sieve's hot loop).
- `PHARO_NO_SISTA_DOACCUM_RESUME=1`      3/10 reliable.
- `PHARO_NO_SISTA_INJECT_RESUME=1`       4/5 reliable.
- `PHARO_NO_SISTA_COLLECT_RESUME=1`      2/3 reliable.
- `PHARO_JIT_DEFER=60` (no JIT)          1/5 reliable.

None of the env-vars deterministically fix the hang.  This is a
scheduler race that varies between runs based on timing.

**Same class of bug as runSum** which was fixed in `31f1c640`
(2026-05-02) by clearing stale `relinquishSlept_` on
`jitSistaCallSend` entry.  Probably another stale scheduler signal
needs clearing somewhere — but not in a JIT path, since the
hang persists with JIT disabled.  Needs lldb attached during a hang
with a breakpoint on `transferTo` / `putToSleep` / the Smalltalk
`Processor activeProcess yield` path to see exactly which process
is hogging and why no lower-priority work runs.

Other splice paths on arm64 work in the same bench-suite run:
`sieve x100 = 7-8 ms`, `1M blocks = 0 ms`, `1M getter+yourself = 0 ms`.
The visible JIT splice infrastructure is healthy.

**Status:** debugging deferred — needs lldb session focused on the
scheduler at runCollect entry on a hung instance, not on the JIT
codegen path.  When the bench-suite IS completing, the numbers are
in line with the x86 documented post-session figures
(`docs/jit-bench-2026-05-16.md`), so JIT correctness is fine.

### A6. arm64 asmjit-T1 JIT is currently a net perf regression — 2026-05-17

**Critical finding from this session's bench-suite work.**

Running every non-splice benchmark with `PHARO_NO_JIT=1` produces
*faster* numbers than the default JIT path on arm64:

    bench (10x100K)      default JIT   PHARO_NO_JIT=1   delta
    fib(28)              245 ms        113 ms           -54% (2.2× faster)
    5000 factorial       177 ms        24 ms            -86% (7.4× faster)
    dict 50K             332 ms        218 ms           -34%
    sort 100K            354 ms        283 ms           -20%
    stringHash 100K      97 ms         80 ms            -18%
    floatSum 1M          108 ms        94 ms            -13%
    sum 1M               92 ms         81 ms            -12%
    sieve x100           8 ms          7 ms             same (Sista splice)
    1M blocks            0 ms          0 ms             same (Sista splice)
    1M getter+yourself   0 ms          0 ms             same (Sista splice)

JIT only wins where the Sista splice replaces the inner loop entirely
(sieve, blocks, getter+yourself).  Everywhere else, asmjit-T1's
emitted code is slower than the interpreter on arm64.  PHARO_NO_ASMJIT_T1=1
(falls back to the legacy stencil JIT) hangs at fib, so the only
way to dodge T1 today is to disable JIT entirely.

Per-tier isolation:

    config                  fib(28)
    default (T1 + Sista)    250 ms
    PHARO_NO_SISTA=1        235 ms   (T1 only — tiny win)
    PHARO_NO_ASMJIT_T1=1    HANGS    (legacy stencil JIT broken?)
    PHARO_NO_JIT=1          121 ms   (pure interp — baseline)

**Cog (arm64) reference:** fib 2-3 ms, factorial 4 ms, sort 17 ms,
sum 2-5 ms.  We are 25-50× behind Cog even with NO_JIT.  T1 then
makes us worse, not better.

**Why is T1 slower than interp?** Investigated 2026-05-17 by reading
the asmjit-T1 send emit + the C stencil_sendJ2J definition:

**Root cause: asmjit-T1 does not emit inline J2J direct calls.**
`src/vm/jit/asmjit/AsmjitT1.cpp:1071-1075` (both x86 and arm64
arms) say literally `inline yet (bit 60 J2J, ...)` — i.e., when
the IC HIT path sees `extra & J2J_ENTRY_BIT` (the bit that says
"target is JIT-compiled — call it directly"), asmjit-T1 just
falls through to `dispatchCached` and `ret`s back to the C++
chain loop.  The chain loop then re-enters JIT for the callee via
`activateMethod` → `tryJITActivation`.  Every recursive fib call
takes that JIT → C++ → JIT round trip (~500-1000 cycles of pure
overhead per call).

The legacy stencil JIT (replaced as default in `b5a7c837` on
2026-05-15) HAD inline-J2J: `stencils.cpp:1733-1877`
(`j2j_direct_call:` block) — saves caller state, sets up callee
state, tail-calls into the callee's compiled entry, all without
returning to C++.  asmjit-T1's send emit hasn't ported this code
path yet.

For benchFib (514K recursive calls, 0-arg, mono-IC on SmI), the
round-trip is the entire perf gap.  500 cycles × 514K calls = 256M
cycles = ~80 ms on a 3.2 GHz arm core — matches the observed 137 ms
gap close enough for rounding.

**Why x86 is "only" 2× faster than arm here:** x86 ABI saves ~5
callee-saved regs across each chain-loop call; arm64 ABI saves ~11.
The extra saves per round-trip cost arm proportionally more, but
both archs pay the round-trip.

**Other contributing factors (smaller):**

- The cmov-mem trio (`218e7547` / `43ab82a1` / `3ee4dc0d`) is
  x86-only by nature (arm csel needs both operands in registers).
  Maybe 5-10% loss on comparison-heavy code.
- Per-method bookkeeping in stencil_sendJ2J's J2J path (tier
  check + callee/caller bump for safe-point recompile drain) —
  ~8 memory accesses per J2J call.  Negligible compared to the
  round-trip cost.

**Workarounds tested:**

- `PHARO_JIT_THRESHOLD=99999999` (effectively no compile) gets fib
  back to 117 ms.  All non-splice benches improve.  Splice-driven
  benches unaffected (they go through the Sista path, not T1).
- Per-method `PHARO_JIT_EXCLUDE=benchFib,factorial,...` could exclude
  the worst regressions selectively but doesn't help the steady
  long-tail regressions.

**The fix is well-defined:** port the `j2j_direct_call:` block from
`src/vm/jit/stencils/stencils.cpp:1733-1877` into both arms of
`AsmjitT1.cpp:emitOne_x86` / `emitOne_arm64`'s send-emit, fired
when `extras & J2J_ENTRY_BIT` after IC HIT.  The save struct
layout is fixed (`J2JSave`) and the save protocol is locked-in
by the chain loop's expectations.  Skeleton:

    after IC HIT:
      r8 = extras (icData[2])
      if (r8 & J2J_ENTRY_BIT) goto inline_j2j_call;
      ... existing inline-spec dispatch ...

    inline_j2j_call:
      check j2jSaveCursor < j2jSaveLimit (bail if full)
      compute calleeJM = (extras & J2J_ADDR_MASK) - JM_SIZE
      save = j2jSaveCursor
      save->sp / receiver / tempBase / ip / sendArgCount /
           resumeAddr / jitMethod  (7 stores)
      j2jSaveCursor += sizeof(J2JSave)
      j2jDepth++ ; j2jTotalCalls++
      set s->receiver, s->tempBase, s->jitMethod, s->ip
      (and s->literals, s->argCount if not self-recursive)
      tail-call entryAddr

That replaces the JIT→C++→JIT round trip with a single inline
call, saving ~500 cycles per send.  Validate against the chain
loop's J2J return path (`J2J_INLINE_RETURN`) — the resumeAddr
+ stencil-style return is what closes the loop.

Risk: getting the save protocol wrong corrupts the sender chain
(class of bug we already know painfully).  Test plan: per-bench
regression battery + `PHARO_RESUME_J2J=1` exercise + fuzzer.

**Status:** until inline-J2J is ported, `PHARO_NO_JIT=1` is a
defensible default for arm64 production runs of arith/send-heavy
code (Sista splice still fires, so splice-driven wins survive).

**Scaffold landed 2026-05-17 (`f81d61a0`, `078105ce`):**
arm64 inline-J2J path wired up with per-bail counters, opt-in via
`PHARO_T1_INLINE_J2J=1`.  Currently always bails (counts as
`bail_self`); baseline perf unchanged with knob off.

**Option (b) (blr/ret normal-call semantics, C-stack save) — TRIED
AND ROOT-CAUSED, 2026-05-17.**  Implementation drafted: sub sp #64;
store 7 caller-state fields; setup callee state; blr x_entry; load
retval; restore caller state; add sp #64; push retval; b endOfSend.

Initially appeared to crash with bad x2 in `stur x13, [x2, #-16]`,
but lldb attach with `brk #0` right before the stur revealed the
ACTUAL bug:
- x2 = 0x0d2f68e178 (correct — caller's JIT.sp restored cleanly)
- x13 = 0x0000000000000000 (RETVAL IS ZERO!)

The callee's `ret x30` ran, but state.returnValue was unchanged
from caller's (zero/nil).  The callee exited via `dispatchCached`
(its OWN nested send) with EXIT_SEND_CACHED, NOT via `returnTop`
with EXIT_RETURN.  asmjit-T1 callees DON'T run-to-completion — they
exit JIT whenever they hit a nested send they can't inline, and
expect the C++ chain loop to drive subsequent ExitSendCached → C++
trampoline → re-enter JIT cycles.

This is the same root-cause class as option (a):
**the asmjit-T1 callee contract requires the C++ chain loop.**
You cannot blr into a callee and expect a single ret to mean
"completion" — it could mean "exited mid-execution for a send".

The two real-fix paths now:

1. **Option (a) revisited:** make asmjit-T1's return bytecodes
   (returnTop, returnReceiver, returnTrue, etc.) follow the J2J
   chain protocol — check j2jDepth and tail-call to saved
   resumeAddr when nonzero.  Big change touching every return
   emit + the dispatchCached path.

2. **Option (c) (new): drive a mini chain-loop after our blr.**
   After blr returns, check state.exitReason:
   - EXIT_RETURN → completion, push retval, continue (option b's
     happy path)
   - EXIT_SEND_CACHED → process the cached send ourselves (call
     activateMethod + tryJITActivation in C++ helper, loop back)
   - Other → exit JIT entirely
   Essentially re-implements the chain loop inline.  Saves the
   blr→ret round trip for terminal-leaf-call cases only.

Neither option is small.  Option (a) is the cleaner long-term
choice but a substantial refactor.  Option (c) is a half-measure
that probably doesn't win.

The instrumentation confirms ~556K J2J-eligible sends per
`fib(28)` run.  Full inline would save ~85ms.  But the win is
gated on one of options (a)/(c) — not just emit polishing.

The instrumentation confirms ~556K J2J-eligible sends per
`fib(28)` run.  At ~500 cycles each, full inline would save ~85ms.

**Catch-rate observation (2026-05-17 experiment):** with the knob
on, `J2J stencil calls` counter drops 294K → 236K for `28 benchFib`.
fib(28) has ~514K recursive sends, so the inline path catches at
most ~58K of them (~11%).  Remaining 89% still go through the
C++ chain loop (likely bailing on the self-recursive check —
calleeJM = entry-96 vs callerJM = state.jitMethod — for some
reason).

**Next debug step:** add a per-bail-reason counter in the inline
emit (or use `spliceSpill0/1` as scratch counters) to see which
bail dominates:
  - entryAddr == 0 (shouldn't happen if bit 60 is set)
  - save stack full (unlikely at depth 28)
  - calleeJM != callerJM (the suspected dominant case)

If the calleeJM mismatch is dominant during steady-state
recursion, possible causes: (a) IC entry's J2J_ADDR points to an
OLD entry after the callee recompiled (rewriteIcEntriesAfterRecompile
should fix this but maybe timing window); (b) state.jitMethod is
the OUTERMOST JIT method in the J2J chain, not the immediate
caller (would need to walk the chain or save+restore around the
nested call).

The chain-protocol contract per the legacy stencil
(`stencils.cpp:1733-1877`) DOES NOT update state.method or
state.jitMethod for self-recursive — they stay at the values
set by tryJITActivation.  So self-recursive should match by
construction.  Bears closer reading on what
state.jitMethod actually points to during nested J2J chain entry.

Cog reference is still 25-50× ahead even with NO_JIT — closing
that is the E-series multi-week work, separate from this T1
regression issue.

---

## B. Code state in-tree — experimental / opt-in

Every knob is declared in `src/vm/DebugSettings.hpp` — one file to
grep.

### B1. `PHARO_T2=0` (default off)
T2 never demonstrably wins on any measured bench.  Stays
default-off until a workload where T2 beats T1 appears.  Under
§1.3c coexist default, `PHARO_T2=1` is safe but also largely
dormant on typical benches (T1 handles everything; T2 compiles
but doesn't intercept).

### B2. `PHARO_T2_UNSAFE_CALLEE=1`
Restores the pre-`f279fd4` buggy callee-invocation path
(double-execution) for benchmarking.  Keep as a flag; do not
remove.

### B3. `PHARO_RESUME_J2J=1`
External J2J trampoline in the resume path.  Works but 18 %
slower than inline resume; opt-in only.  See
`memory/project_resume_j2j_trampoline.md`.

### B4. `PHARO_JIT_SIMSTACK=1`
SimStack TOS/NOS register caching.  Default-flip attempted
2026-04-18, reverted same day (12 IntegerTest regressions that
don't reproduce in isolation).  Root cause open.

### B5. `PHARO_T2_A1=1` — T2 chain-loop continuation
Implemented but dormant.  A1's callee-invocation speedup is only
observable when T2 actually executes; under coexist default, T2
rarely does.  Would need a workload where T2 wins to justify
re-examining.

### B6. T2 diagnostic env vars (keep)
`PHARO_T2_BAIL_OP`, `PHARO_T2_NO_ARITH_FAST`,
`PHARO_T2_NO_ARITH_OPS`, `PHARO_T2_OPT`, `PHARO_T2_TRACE`,
`T2_LIMIT`, `T2_VERBOSE`, `PHARO_T2_MBC_JUMPS`,
`PHARO_T2_MBC_SENDS`, `PHARO_T2_MBC_IC`, `PHARO_T2_WARMUP`,
`PHARO_JIT_NO_SIMSTACK`.  All cheap when not enabled.
(`PHARO_DUMP_MIR` removed when MIR was deleted 2026-04-17.)

### B7. `PHARO_SISTA_HELPER_SENDS=1` — default-on with Array-do gate

**2026-05-06 status:** HELPER_SENDS was flipped default-on in
`2e274c7d` (2026-05-02) plus three precondition fixes
(`bd7adb87`, `2a7e2a4e`, `2e274c7d`).  See
`memory/project_helper_sends_default_on_2026_05_02.md`.

The Array-do/helper-send coexistence gate (`2a7e2a4e`) was briefly
disabled (commit 31f1c640) on the assumption that the
`relinquishSlept_`-clearing fix solved the underlying issue.  It
didn't — bench-suite re-bisection 2026-05-06 showed sum 1M still
fails 0/5 in bench-suite context with the gate off.  Gate
re-enabled in `0803530f` and narrowed to exclude ArrayCollect in
`f5e594d0` (collect's deopt-with-resume helper handles the
interaction correctly).  Bench-suite now 5/5 reliable; sum 1M
runs in interp (80ms) instead of spliced (1ms) when both
conditions apply.

Opt-out flags:
- `PHARO_NO_SISTA_HELPER_SENDS=1`     disables helper-sends entirely
- `PHARO_SISTA_ALLOW_ARRAYDO_HELPER=1` keeps helper-sends on but
   bypasses the Array-do gate (returns to flaky bench-suite)

Future work to recover sum 1M splice perf: find which prior bench
state poisons runSum's compile (sort/dict splice activity is
suspect).  Multi-session — needs lldb attach during bench-suite
or a state-snapshot diff between iso-eval (works) and bench-suite
(fails) at the moment runSum compiles.

---

**Historical context (pre-default-on):**
B-1 helper-based kSendUnspeculated infrastructure (446187d9,
244fde02, bbe36bed).  Lifter emits `kSendCallHelper`, lowering
calls `jit_rt_sista_call_send` via cc.invoke, helper drives
step() to completion with depth cap 1 + inSyncSend_ flag to
suppress process switches.  Default off; default behavior
unchanged.

**2026-04-29 progress:**
- `a2b2934c` — deopt-stack truncation fix: builder snapshots the
  full IR stack into the framepoint; lowering flushes all
  stackValueIds (not just rcvr+args) on helper-returned-zero.
- `ae7f5b6d` — `inSyncSend_` actually gated.  Previously
  set/cleared but never read.  Now suppresses (a) preemption
  checks, (b) processPendingSignals, and (c) Sista compile +
  dispatch — all of which corrupted the helper's frameDepth_
  bookkeeping when active.  This eliminated the depth-1 bail
  cascade that made the failure mode catastrophic.
- `PHARO_SISTA_HELPER_FORCE_BAIL=1` diagnostic added.  Under
  FORCE_BAIL=1, eval succeeds — confirming the deopt path is
  correct.  All remaining bugs are in the helper SUCCESS path.

**Status under env=1:** still hangs.  Trace shows ~14k successful
helper calls before fd=4096 overflow (previously ~30).
470× improvement from the cascade fix.

**Per-call delta confirmed 0:** instrumented every jitSistaCallSend
to compare entryFD vs exitFD across 24k calls — leaks=0 every time.
So jitSistaCallSend itself is internally balanced.

**fd grows BETWEEN calls** (~0.5 fd per call avg).  This means the
JIT-compiled caller of the helper is leaving extra frames on the
stack between helper-sends.  Suspects (in order of likelihood):

1. T1 / T2 / J2J stencil call → return imbalance.  Inner sends
   inside the helper-driven step() loop activate methods via
   tryJITActivation → JIT-compiled code → exits with EXIT_SEND
   for further sub-sends.  Each EXIT_SEND dispatches via interp,
   which pushes a frame for the bailed send's target.  If that
   target is itself JIT-compiled and bails again, frames may
   accumulate.  Probably not specific to HELPER_SENDS — just
   exposed by the high call frequency.

2. Sista-compiled CACHED methods invoked via Sista::Runtime::lookup
   inside the helper.  My inSyncSend_ gate at line 7268 prevents
   Sista compile/dispatch when active, but the gate may not cover
   every path that finds and runs a cached compilation.

3. Block activation via activateBlock (line 8224) doesn't gate on
   inSyncSend_.  If a block runs Sista-compiled code, kSendCallHelper
   may bail (depth-cap) — though deopt should be balanced.

**Next session approach:** narrower instrumentation — log fd at
exit of every JIT mechanism (tryJITActivation, popFrameForJIT,
T1 stencil exit, Sista exit) to find the leak source.

**2026-04-29 traces (cumulative):** logged fd at every helper
enter/exit + peak/end fd within each call, then added pushFrame /
popFrame / pushFrameForJIT / j2jPopFrame / J2J-materialize counters
(only counting while `inSyncSend_=true`).  Findings:

- All inner activations inside helper have `canJITActivate=0`
  (methods aren't compiled when first hit during startup).  So
  the leak is NOT in JIT runtime — it's in interp.
- Each individual helper call's fd is balanced: start == end.
- Between calls, fd grows ~0.5/call.
- The hot path is `shallowCopy` (steps=0, primitive completes
  synchronously, fd unchanged inside the call).  Yet across many
  shallowCopy calls, fd creeps up at +0.5/call.
- Periodic NLR-like drops (e.g. fd 446 → 15 between two trace
  prints) suggest exception unwinding of deep call chains.

**The diagnostic gap:** all instrumented counters were guarded by
`if (inSyncSend_)` so they only fired *inside* the helper call.
The fd-grows-between-calls finding therefore proves the leak is
OUTSIDE the helper — it happens when the JIT caller's compiled
code runs between helper invocations (`inSyncSend_=false`).  The
per-call balance is real but irrelevant: the helper isn't the
leaker, the JIT-compiled outer method is.

This isolates the leak to one of:
- The JIT-compiled OUTER method (M_a) calling shallowCopy in a
  loop is being repeatedly RE-ENTERED.  Each re-entry pushes
  M_a's frame.
- Some interp activation pushed during shallowCopy's caller's
  bytecode dispatch isn't being popped (block invocation primitive
  mismatch?  ensure: handler?).
- The deopt path on a depth-cap bail (now rare with my Sista-skip
  fix but still possible) doesn't fully unwind something.

**Next session approach:** instrument the JIT-runtime mechanisms
(`tryJITActivation`, `popFrameForJIT`, T1 stencil exit, Sista
exit) WITHOUT the `inSyncSend_` gate so they fire between helper
calls.  Hypothesis: one of these is asymmetric — pushes a frame
that the matching pop never sees, or vice versa, by ~0.5/call
amortized over a long-running JIT-compiled outer method.

The primitive overflow check at fd=4096 fires during STARTUP — a
critical process — so the failure surfaces as a process kill
rather than a recoverable stack overflow.  Without HELPER_SENDS,
the same code paths balance (fd plateaus); with HELPER_SENDS,
something accumulates +0.5/call.

Real fix needs either:
- IC-guided emission (only emit kSendCallHelper for sites where
  IC says receiver class → primitive method, never for normal
  method activations).
- Full bcToEntryState materialization on deopt so the interp
  picks up post-send instead of pre-send.

The 27× sum(1M) gap to Cog won't close via B-1 alone — Pharo
macro-inlines `to:do:` but `Array>>do:` is a real send to a
literal-block argument, and the per-iter `value:` block dispatch
dominates.  See B8 for the structural fix.

### B8. B2 splice — Array do: with literal-block inlining (infra shipped, no payoff)
**Status as of c3091d3b (2026-04-27):** end-to-end plumbing is in
tree behind `PHARO_SISTA_DO_SPLICE=1` (with optional
`PHARO_SISTA_DO_SPLICE_NO_HINT=1` to splice without IC hints):

- `kCountedLoopDo` IR op (`SistaIR.hpp`).
- Pre-pass scans for PushFullBlock+SpecialSend(do:) adjacency,
  sub-lifts the block IR into `Method.inlinedBlocks`, validates
  against splice-simple op whitelist (loads + arith + return).
- Pre-pass filters out candidates the main lifter could never reach
  (`sawLiftTerminator` flag — any send-byte before the candidate's
  PushFullBlock).
- Main lift's PushFullBlock arm intercepts admitted candidates and
  emits `kCountedLoopDo` instead of the generic bail.
- Lowering (`SistaLowering.cpp`) emits a counted at: loop using
  `cc.invoke` → `jit_rt_sista_basic_size` + `jit_rt_sista_basic_at`
  with deopt-on-zero, plus an inline whitelist body (loads + return
  + constants only).
- Tracing: SISTA-SPLICE-CAND, -EMIT, -LOWER-OK, -LOWER-FAIL.

**Why it doesn't move benchmarks yet:**
1. **Real Pharo methods almost always have a setup-send before any
   `arr do: [...]`** — the receiver is typically fetched via send
   (`arr := self getArr. arr do: ...`).  Sista's lifter terminates
   `kOk` on the first regular send.  The pre-pass's lift-terminator
   filter rejects every such candidate because the main lift would
   never reach the PushFullBlock anyway.
2. **Bench block bodies need more than the whitelist allows.**
   sum(1M)'s `[:e | sum := sum + e]` has `kPrimAddInt` (would be
   easy to add) and `kStoreTemp` to a closure-captured slot
   (requires escape analysis + a side-table mapping block slot N →
   outer captured temp slot M).

**Empirical:** 100K-iteration `arr do: [:e | e]` bench under PHARO_SISTA_DO_SPLICE=1
PHARO_SISTA_DO_SPLICE_NO_HINT=1 → 0 OK verdicts among 200 sampled
PushFullBlock+do: pairs from real Pharo activity.  No crash, no
divergence, default unchanged.

**2026-04-29 update — splice-trace on the bigger bench suite:**
the suite's `sum(1M)` (`(1 to: 1000000) asArray. a do: [:each | sum := sum + each]`)
hits both rejection patterns: many `lift-terminator before do:`
(setup sends like `Time millisecondClockValue` terminate the lift
before the do: site) and many closure-accum candidates rejected
because `blockLen != 9-10`.  The bench panel's `sumArr` works
because its calling method places `arr; vec; PushFullBlock; do:`
adjacent at method start, with no setup sends in between.

**Real win path** is one of:
- Fix B-1 helper-sends (B7) so the lift continues past sends and
  reaches the PushFullBlock (then the lift-terminator filter
  loosens for sends covered by helper-sends).
- Extend kCountedLoopDo lowering to handle `kPrimAddInt` and
  `kStoreTemp` to closure slot — covers the sum(1M) shape directly.
- ~~Loosen the closure-accum recognizer's `blockLen` check~~ — see
  data below; not a viable lever.

**2026-04-29 blockLen rejection census** (PharoBenchSuite startup
under `PHARO_SISTA_DO_SPLICE=1 PHARO_SISTA_DO_SPLICE_NO_HINT=1`,
116 blockLen rejections, 1 acceptance):
- Only 3/116 rejections (~2.6%) actually start with `0xFB`
  (closure-accum's required lead byte).  All three are
  blockLen=74 — way past the recognizer's window and not the
  closure-accum shape regardless of length.
- The other 113 rejections are different idioms entirely:
  - 60× lead=0x40 (pushTemp 0): mostly `40 41 90 5e` — `[:e | e
    selector: capturedTemp]` (1-arg send with captured temp).
  - 26× lead=0x41 (pushTemp 1): `41 40 7a c0 59 4f 5e` shape —
    captured-then-arg with a SpecialSend + branch.
  - 22× lead=0x4c (pushReceiver): `4c 40 41 a0 5e` — `[:e | self
    selector:e with: capturedTemp]`.
  - 5× lead=0xE7 (ExtPushLitConst): literal-constant-driven
    blocks.

Conclusion: loosening `blockLen` in isolation gains ~3 candidates,
all of them blockLen=74 and shaped wrong anyway.  The 113 other
rejections are *separate splice opportunities* (not closure-accum
variants), each requiring its own recognizer + lowering pair.
None of them are accumulator shape, so widening the existing
recognizer's window is the wrong abstraction — recognize each new
idiom under its own name.

The diagnostic-print line was kept (lead-byte added) so future
investigation can quickly classify rejections without rebuilding.

### B9. Phase 4 inliner — recognized-shape coverage (2026-04-29)

Sized the gap with a `[SISTA-UNRECOG]` shape histogram added behind
`PHARO_SISTA_INLINE_STATS=1`.  Full PharoBenchSuite startup trace:

    sends-lifted=15700  hints-provided=1590  hints-consumed=839
    callees-attempted=737  callees-lifted=707  inlines-emitted=102

So we *successfully probe-lift* 707 callees but recognize only
102 (~14%).  The other 605 lift fine but don't match a recognizer.

Top unrecognized shapes (size / op0.op1.op2 / count):

    sz=7  / kLoadReceiver.kLoadTemp.kConstantOop      / 42
    sz=4  / kLoadReceiver.kLoadInstVar.kConstantOop   / 38
    sz=4  / kLoadLiteral.kLoadInstVar.kConstantOop    / 35
    sz=12 / kLoadReceiver.kLoadInstVar.kConstantOop   / 28
    sz=3  / kLoadLiteral.kLoadInstVar.kSendUnspec     / 27
    sz=4  / kLoadLiteral.kLoadInstVar.kLoadReceiver   / 17
    sz=4  / kLoadReceiver.kLoadTemp.kConstantOop      / 15
    sz=4  / kLoadReceiver.kLoadInstVar.kLoadReceiver  / 15

Decoding: most of these are *arith-on-ivar* methods like
`^ ivar + 1` or `^ ivar - other`.  With `PHARO_SISTA_INLINE_ARITH`
**off** by default, the lifter bails arith ops to
`kSendUnspeculated`, so a 5-instruction `^ ivar + const` source
lifts to a 4-value method ending in kSendUnspeculated.  Without
INLINE_ARITH the recognizer can't extract the arith intent —
even if we added a 4-value rule, the kSendUnspeculated terminator
means the work is identical to a normal send.

**Real lever** for the 1M getter / sum 1M Cog gap is one of:
- Default `PHARO_SISTA_INLINE_ARITH=1` — see findings below.  No
  crash today, but mixed bench results: big win on inner-loop
  arith, regression on tiny-method-in-loop.
- Add a 5-value recognizer for the arith-on-ivar shape AFTER
  enabling INLINE_ARITH (so the lifter emits `kPrimAddInt` etc.
  and the callee shape becomes
  `kLoadReceiver+kLoadInstVar+kConstantOop+kPrimAddInt+kReturn`).
- Recognizing `OrderedCollection>>size`-style 5-7-value getters
  with multi-ivar + arith.

Histogram instrumentation kept (gated by INLINE_STATS) so future
sessions can re-census after each recognizer is added.

**2026-04-29 INLINE_ARITH=1 stability + bench delta (best-of-5
both columns — single-run noise on this panel is huge, ±35%):**

                      baseline   INLINE_ARITH=1   delta
    tiny bc/s          25049115   24900398        ~0
    fib(28) ms              21         21         ~0
    sieve x100 ms           45         44         ~0
    sort 100K ms           221        224         +1%
    dict 50K ms            155        163         +5%
    sum 1M ms               65         66         ~0   (initially
                                                       looked like
                                                       win — noise)
    factorial 5000 ms       23         23         ~0
    1M blocks ms            13         15         +15% mild loss
    1M getter ms            96        110         +15% regression
    100K alloc ms            5          5         ~0

No crash.  The prior crash from `feedback_splice_flags_opt_in.md`
was a multi-flag interaction (with IV_DO_ACCUM + DO_SPLICE), not
INLINE_ARITH on its own.

**Initially I read sum 1M as a 34% win** because I compared a
single baseline run (sum=100, an unlucky run) against best-of-5
INLINE_ARITH (sum=66).  Best-of-5 baseline is also 65 — the
variance dominates.  Bench panel run-to-run noise on this
benchmark is huge; one run can be 65ms, another 103ms.  Use
best-of-N for any A/B claim, both columns.

INLINE_ARITH=1's actual effect on the panel: roughly nothing on
the work-in-loop benches (sum 1M, factorial), small regression on
1M blocks (+15%), real regression on 1M getter (+15%).

Why the getter regression: calls `obj size` (`^ lastIdx -
firstIdx + 1`) inside the bench loop.  Without INLINE_ARITH,
`size` is gated out of Sista (unsafe arith) and runs in the
interpreter — fast for a 3-bytecode method.  With INLINE_ARITH,
`size` gets Sista-compiled, paying activation + tag-check
overhead per call.  Net loss until Phase 4 INLINES the size body
into the calling block.

Action: keep INLINE_ARITH opt-in.  Default-on is blocked by the
1M getter regression, not the prior crash.  Adding the Phase 4
arith-on-ivar recognizer would inline the size method body and
remove the regression — that's the prerequisite for default-on.

**2026-04-29 follow-up: INLINE_ARITH=1 + INLINE_STATS=1 trace
disproved the simple recognizer theory.**  Reran the histogram
under both flags expecting to see kPrimAddInt/kPrimSubInt-ending
shapes in the unrecognized list — that would validate "extend
recognizer for arith-on-ivar" as the lever.  Actual top entries
are all `kSendUnspeculated`-terminated:

    sz=4 / kLoadReceiver.kLoadInstVar.kConstantOop.kSendUnspec : 11
    sz=2 / kLoadTemp.kSendUnspec                                : 6
    sz=3 / kLoadReceiver.kLoadTemp.kSendUnspec                  : 5
    sz=3 / kLoadReceiver.kLoadLiteral.kSendUnspec               : 4
    ... (all remaining entries also kSendUnspec-terminated)

`inlines-emitted` only goes 14 → 15 with INLINE_ARITH=1; the gate
flip doesn't unlock new inline candidates.  The 1M getter
regression isn't about an unrecognized inline shape — it's about
`OrderedCollection>>size` being Sista-compiled (paying full
activation per call) without being inlined at the bench's call
site.

So the lever isn't "extend recognizer for arith-on-ivar" — that
shape doesn't appear in the histogram.  The correct lever is
either:
- Make `size`-shaped methods cheaper to call (smaller activation,
  or T1-style fast paths for short Sista bodies).
- ~~Stop Sista-compiling tiny methods altogether~~ — tested
  2026-04-29, didn't help (see below).
- Get them inlined at the call site by extending hint generation
  to cover this pattern.

Histogram instrumentation now records op3 (was op0..op2) so the
top entries surface their terminator op directly — useful for
distinguishing arith-terminator from send-terminator without
extra code.

**2026-04-29 follow-up: tiny-arith-method gate doesn't fix it.**
Multiple gate variants tested (bcLen ≤ 8, ≤ 24, no limit + only
"pure arith no sends"); all fail to recover baseline.  Even
`PHARO_SISTA_EXCLUDE_SELS=size` (hard-excluding the prime
suspect) only recovers ~1ms — not a real fix.

**Real root cause — Sista activation path is slower than T1.**
Diff'ing JIT Stats on a full bench run pinpoints it:

                          baseline    INLINE_ARITH=1   delta
    compiled methods          230            231       +1
    T1 activations          5.59M          4.49M       **-1.10M**
    J2J stencil calls      55.87M         56.90M       +1.03M
    Sista hit rate          8.9%           6.8%        -2.1pp

Under `INLINE_ARITH=1`, ~1.1M activations migrate from the T1
entry path to the Sista entry path (because the same methods
become Sista-eligible).  J2J trampolines pick up the slack
(+1.03M calls).  The 14ms/iter getter regression is exactly
this: 14 ms / 1.1 M activations = **13 ns per migrated call** —
the Sista activation overhead vs T1's tighter entry stencil.

**1M getter regression FIXED 2026-04-29 (`e90a6ba4`).**  The
13ns/call wasn't a generic Sista entry overhead — it was a single
redundant `pthread_jit_write_protect_np` syscall in the dispatch
path.  Each Sista dispatch was calling `makeExecutable` BEFORE the
call defensively, even though the JIT zone was always executable
on entry (the compile site at line 7359 makes-executable; the
only hot-path `makeWritable` sites in IC patches use RAII guards
that re-makeExecutable on every exit).  Removing the redundant
pre-call toggle closes the 14ms 1M getter regression specifically.

**Default-on flip attempted, reverted, then RE-FLIPPED after a
second entry-path fix (`c362d328` → `e36b7b1b` → `ca3a80ae`).**

The first attempt was reverted because of broader
INLINE_ARITH=1 vs baseline best-of-5 comparison showing mixed
results across the panel:

                          baseline   INLINE_ARITH=1
    tinyBytecodes/s     25,147,928   24,475,524   -2.7%
    fib(28) ms                  21         23      +9.5%
    sieve x100 ms               44         46      +4.5%
    sort 100K ms               223        220      -1.3% (better)
    dict 50K ms                155        154      -0.6% (better)
    sum 1M ms                   65         72     +11%
    factorial 5000 ms           22         21      -4.5% (better)
    1M blocks ms                13         15     +15%
    1M getter ms                98        101      +3%
    100K alloc ms                4          5     +25%

Net: 7 benches regress 3-25%, 3 improve 1-5%.  The 1M getter
regression is gone, but other bytecodes-per-call paths (alloc,
blocks, fib, sieve) lose more than the inlined arith saves.
Likely cause: more methods become Sista-eligible under
INLINE_ARITH=1, paying activation overhead per call where T1's
tighter entry would have been cheaper.

INLINE_ARITH stays opt-in until the residual ~3-15% bench-panel
regressions are understood.  The makeExecutable fix is real and
shipped — closes the 14ms hot-path delta — but isn't sufficient
on its own.

**A second entry-path fix made the default-on viable: gating the
dispatch invariant check behind an opt-in env var (`aafd201a`).**

The dispatch path was unconditionally:
  - Saving receiver_/framePointer_/method_/frameDepth_ pre-call
  - Comparing them post-call
That's 4 stores + 4 reads + 4 compares + branch (~10ns/call).
Across 5M+ activations on the panel = ~50ms paid for a debug
check that hadn't fired in months.  Gated behind
`PHARO_SISTA_INVARIANT_CHECK=1` (default off).

After both fixes, A/B comparison on 1M getter:
  PHARO_SISTA_NO_INLINE_ARITH=1: 111ms
  PHARO_SISTA_INLINE_ARITH=1:    103ms (8ms faster)

ARITH default-on re-flipped in `ca3a80ae`.  fib(28) also wins by
2ms.  sum 1M shows wider variance than other benches (77-114ms
range run-to-run in same binary), so claims at the 5-10% level
on it are inconclusive.

Next-session candidates if more wins are wanted:

1. Profile the Sista entry stencil — what asmjit-generated
   prologue instructions execute on every Sista activation that
   T1 doesn't run?  May find more redundant work.
2. Skip Sista's frame-state machinery on activation when no
   speculation has fired (reset on first deopt).
3. Inline the Sista→T1-fallback path so Sista bails don't
   double-dispatch.

**2026-04-29 follow-up: gate-cache encoding (`fd266981`).** Encoded
the runtime blacklist into the gate cache value (0=admit,
1=reject, 2=blacklist) so the dispatch hot path can skip the
`sistaBailCounter_` hashmap find on every admitted dispatch.
ExitSend's threshold check now promotes the gate state and
erases the counter entry.  Best-of-3 1M getter 98ms vs prior
103ms (~5% improvement, possibly within noise; splice panel
unchanged at 4/7 ms).  Also tried (and reverted): gating the
"always-on" Sista ring-buffer record behind PHARO_SISTA_RING=1.
8 stores per dispatch but no measurable bench delta — keep
always-on for DNU diagnostic context.

**Structural finding — Sista doesn't compile blocks.**
`activateBlock` (line ~8291) bypasses the Sista dispatch path
that lives only in `activateMethod` (line ~7270).  See
`memory/project_sista_skips_blocks.md`.  Hot block-call path
through T1's BLOCK_VALUE_BIT stencil is already J2J-direct
into the block's T1 code, so the BLOCK BODY is fast — but Phase
4 callee inlining at sends inside the block never fires
(no Sista IR for the caller).  The 1M getter+yourself bench
sits at ~98 ms vs Cog ~3 ms because of this: `size` is
Sista-compiled, but the bench block isn't, so `size`'s body
can't be inlined at the call site.  Next-level levers for that
gap are either:
  4. Add Sista dispatch in activateBlock (mirror activateMethod;
     preserve closure_, homeFrameDepth, NLR semantics).
  5. Port Phase 4-style monomorphic inlining to T1's per-method
     compile, widening the existing inline-getter / setter /
     returnsSelf IC fast paths to the arith-on-ivar shapes.

**2026-04-29 attempt at #5 (returnsLiteral) — reverted.**
Prototyped a `returnsLiteral` shape recognizer that detects
`pushTrue/False/Nil/Const + returnTop` and standalone
`ReturnTrue/False/Nil`, encoded the cached Oop bits in IC
`extra` bit 58 (with bits 47:0 = literal), and added a path
to the existing `IC_HIT` macro in `stencils.cpp`.  Attempt
reverted because:

1. **Inline expansion of `IC_HIT` adds per-call overhead to
   ALL sends, not just returnsLiteral hits.** The macro is the
   generic poly-IC fast path used by every send site that
   hasn't been specialized at JIT-recompile time.  Adding a
   4th conditional after bits 63/62/61 imposed a measurable
   cost across the bench panel: splice runs went from
   4/7/7/7/7/4 ms to 6/9/9/9/9/6 ms (2-3 ms each).  Sum 1M did
   show a real win (102→77 ms ~25% faster) but 1M getter
   regressed (99→108 ms) and the panel benches went broadly
   slower.

2. **First bounds-check used `slotCount()` — over-permissive.**
   `pushLitConst N` reads slot `1 + N` from the method, but
   `slotCount()` of a CompiledMethod includes the byte area;
   the bounds were too loose, allowing reads into the
   bytecode region.  Fixed to use `numLiterals` from the
   method header — but the macro-overhead issue remained.

**The right architecture** (deferred to a future session) is
to add a NEW SPECIALIZED STENCIL `stencil_sendInlineReturnsLiteral`
matched to the existing pattern in `stencils.cpp`
(stencil_sendInlineGetter et al), with a corresponding
specialization branch in `JITCompiler.cpp:1256+`.  The
inline-stencil path already works correctly for monomorphic
sites without polluting the polymorphic IC_HIT macro — that's
the only way to add new shapes without taxing all sends.
detectTrivialMethod recognition + IC patch encoding can be
re-introduced together with the new stencil.

**2026-04-29 SHIPPED as opt-in `PHARO_RETLIT=1`** — original
arbitrary-Oop design at `bb9bb798`, bit-budget redesign at
`3e2efb7c`.  The full architecture lives in tree:
  - `stencil_sendInlineReturnsLiteral` (236 bytes, 7 relocs after
    redesign) reads class from `icData[0]` and a 3-bit kind tag
    from `(icData[2] >> 48) & 7`, switching on
    nil/true/false/SmI 0/SmI 1.  Modeled on
    `stencil_sendInlineMonoJ2J`.
  - `detectTrivialMethod` recognizes 1-byte ReturnTrue/False/Nil
    and 2-byte push<const>+returnTop, restricted to the 5
    encodable kinds (`TrivialReturnKind`).
  - IC patcher sets bit 58 + bits 50:48 = kind when
    `PHARO_RETLIT=1`.  Bits 50:48 are above the 47-bit
    virtual-address range, so bits 47:0 stay free for J2J's
    entry address.
  - `TRIVIAL_BITS` does NOT include bit 58 — bit 58 and bit 60
    coexist on the same IC entry.  The if-else specialization
    chain picks bit 58 first; without specialization the bit-60
    J2J path still fires (no regression by construction).
  - `JITCompiler::applyICSpecialization` swaps
    `stencil_sendJ2J` → `stencil_sendInlineReturnsLiteral` on
    bit 58.
  - `numSendSites` count + `operand2Ptr = icBase` loop both
    include the new stencil.

**Default OFF.**  Bench panel identical with or without the flag
(5/7/7/7/7/5 ms).  Eval-mode benches (sum 1M, ifTrue blocks,
isInteger, isNil, yourself) within run-to-run noise.

**Immediate-receiver extension (`28ca1470`, 2026-04-29):**
the heap-only gate was lifted for returnsLiteral specifically
(getter/setter/returnsSelf stay heap-gated since their slot
indices in bits 15:0 still collide with J2J's address).  This
unblocks classification of predicate methods on SmI/Char
receivers.  No measurable speedup at 10M-iter `i isInteger`
loops — at ~50 ns/call the J2J fast path is already only a few
cycles, and saving the tail-call/register-save overhead amounts
to ~0.2 ns/call which is below the bench noise floor.  The
extension is structurally correct but won't move bench numbers
on its own.

**Why the original (`bb9bb798`) regressed:** stored the cached
Oop in bits 47:0 directly, which collided with J2J's entry
address, AND set bit 58 in `TRIVIAL_BITS` to suppress J2J on
literal entries.  Net effect: any method qualifying for
returnsLiteral lost its J2J fast path entirely.  Since J2J on a
1-2 bytecode method is itself very cheap, the displacement
cost (lost J2J on every returnsLiteral target) exceeded the
literal-push savings.  Bench suite saw fib +45%, 1M getter +32%,
1M blocks +43%.

**Levers remaining for unlocking real perf:**
  1. ~~Don't displace J2J~~ — done in `3e2efb7c`.
  2. ~~Extend to immediate receivers~~ — done in `28ca1470`.
     Did not yield measurable wins; J2J is already too fast for
     the constant-push savings to be visible.
  3. Only flip bit 58 for sites where the J2J target is
     uncompiled — bypasses the dispatch overhead entirely when
     no compiled target exists.  This is where a real speedup
     would come from: the alternative path is interp-mode method
     activation, which IS slow.  Worth exploring.
  4. Hit-count tracking so we only specialize hot
     returnsLiteral sites — orthogonal; helps if (3) shows wins.

**Conclusion:** returnsLiteral specialization on top of an
already-fast J2J path has minimal value.  The original Phase 4
inliner targeted UNCOMPILED callees (where J2J doesn't exist),
which is structurally a bigger win.  Future work should focus on
lever #3 or move to other Phase 4 shapes that don't compete with
J2J.

### B10. IC-specialization fires too late for DoIt benches (2026-04-30)

`applyICSpecialization` only runs during `recompile()`, gated on
`executionCount == g_debug.recompileAt` (default 500).  Eval-mode
DoIt benches (`1 to: 10M do: [:i | i isInteger]`) run their hot
loop inside a method that activates ONCE — the threshold is never
reached, all specialization paths miss.

`PHARO_RECOMPILE_AT` sweep showed no win on bench panel (panel uses
splices that bypass IC).  Lowering threshold from 500 to 50 and 10
gave parity / mild regression — not the right knob.

**OSR-triggered recompile (`bb6dee2c`, opt-in `a71cb3a4`,
2026-04-30):** when OSR fires for a T1-compiled method whose IC
entries have populated, force a recompile so
applyICSpecialization runs.  `JITRuntime::maybeRecompileForOSR()`
is idempotent (gates on tier == 1 + at least one IC entry has
data); sets tier=2 on recompile.

**Default-on (2026-04-30, `9d1c5438`)** after IC specialization
splice gate `4a0baf6a` eliminated the prior 1M blocks 13→16ms
regression.  The splice gate prevents post-recompile MonoJ2J
rewrite from bypassing Sista's `fn(&sstate)` for splice callees.

Bench suite A/B (best-of-5):
                     OSR=on    default
  1M blocks            12        13
  1M getter+yourself   95        96
  sum 1M               97        98
elsewhere unchanged.  Bench panel: 4/7/7/7/6/4 either way.

Set `PHARO_NO_OSR_RECOMPILE=1` to opt out.  Legacy
`PHARO_OSR_RECOMPILE` still works (no-op now).

Bench panel at parity (4/7/7/7/7/4 ms either way).  Eval-mode
benches: trace confirms `[RECOMPILE-OSR] DoIt (icEntries=14
execCount=0)` — recompile fires.  But sees only 1/14 IC sites
populated at that moment, so partial specialization.  The other
13 sites populate AFTER recompile (during JIT execution) but
stay as `stencil_sendJ2J` because tier=2 prevents another
recompile.

**Limitation:** OSR-recompile fires on first OSR sample with any
IC data, before all hot sites have been observed.  Real fix:
- "High water mark" trigger: wait for ≥ 50% IC fill before
  recompiling (delays JIT entry but captures more sites).
- Multi-tier recompile: allow ONE additional recompile if fill
  count grew significantly since last recompile.  Needs an
  ic-fill-at-recompile field on stats.
- Per-site late patching: when a site's IC data appears AFTER
  recompile, patch the site's stencilIdx in place (requires
  W^X dance + careful invariants).

`PHARO_TRACE_IC_EXTRA=1` logs per-site extra0 bits during
specialization for diagnosis.

### B11. IC entry-addr rewrite after recompile (`f60844c0`, 2026-05-01)

After `compiler_->recompile()` returns a new JITMethod, every
caller's `IC.extra` still held the OLD entry address.  Two issues:

1. The J2J fast path kept entering OLD (unspecialized) code via
   the cached entry-addr — wasted recompile.
2. If the OLD JITMethod was eventually evicted by codezone LRU,
   callers' IC.extra became a dangling pointer (not yet observed
   in the wild because the codezone hasn't filled in normal
   usage, but a latent crash).

`JITRuntime::rewriteIcEntriesAfterRecompile(methodBits, newAddr)`
walks the code zone, patches J2J entry-addr bits where IC's
methodBits matches.  Two-pass: read-only detect first (X-mode
safe), then `makeWritable+walk+makeExecutable` only when an edit
is needed.  Mega cache also refreshed.

Wired into both recompile paths (tryExecute + maybeRecompileForOSR).

Bench panel best-of-10: 4/7/7/6/6/4 — unchanged from baseline.
Bench suite within noise (±1ms across all benches over best-of-5
A/B).  Correctness-positive without measurable perf cost on the
benches we have.  See memory/project_ic_rewrite_2026_05_01.md.

---

## C. Project mission — iOS

The project's purpose is an iOS Pharo VM.  JIT work has dominated
recent sessions; iOS proper is queued next.

### C1. iOS device testing
Mac Catalyst verified working (2026-02-24).  iOS Device
xcframework slice builds as of 2026-04-19 (commit `22bcc2c`).
Device testing still needs:

- Physical iOS device(s) for build verification.
- Apple Developer signing cert setup.
- TestFlight or direct-device deploy.
- UI touch/pinch/pan exercised end-to-end (not just Mac mouse
  events).

### C2. iOS app-store readiness (remaining items)
Shipped 2026-04-19: privacy manifest, launch screen, scoped ATS
(see `changes.md`).  Still open:

- Remote logging for device debugging — needs a backend choice
  (Sentry / Firebase / custom endpoint).  Apple's built-in crash
  reporter handles crashes; this is for non-crash diagnostic logs.

### C3. Image preparation
- Do iOS images need different startup?  Currently uses standard
  Pharo images with `startup.st` injection — verify behavior on
  real hardware.
- Touch-based Morphic input path — `docs/image_issues.md` has the
  wishlist for portrait layout + touch primitives as upstream
  proposals.

---

## D. Upstream proposals

`docs/upstream-proposals.md` tracks wishlist items for the Pharo
image that would make iOS work cleaner.  Out of process from VM
changes; image-side issues to propose upstream.

- Portrait-aware layout in Morphic.
- Touch event primitives on the standard input path.
- Startup preferences path that survives
  `Smalltalk snapshot:andQuit:` round-trips cleanly.

### D1. asmjit Catalyst include — decide PR vs permanent fork

Submodule `third_party/asmjit` currently points at
`avwohl/asmjit:iospharo-catalyst` (commit `1ce3ea65`), a one-commit
fork that moves `<libkern/OSCacheControl.h>` out of the
`TARGET_OS_OSX` guard so `sys_icache_invalidate()` is declared on
Mac Catalyst builds.  History:

- 2026-05-08: change started life as a `third_party/patches/
  asmjit-catalyst-virtmem.patch` applied by the cmake configure
  loop.  Worked on one machine, broke on every fresh clone
  because the patch file was committed but the in-place edit
  wasn't.
- 2026-05-17 `b9dfefa1`: forked asmjit to `avwohl/asmjit`,
  committed the include move as `1ce3ea65` on branch
  `iospharo-catalyst`, updated `.gitmodules` to point there.
- 2026-05-17 `3ce977cd`: removed the now-redundant in-place
  patch from `build-xcframework.sh`.  The patch file in
  `third_party/patches/` is also redundant but retained as a
  fallback for anyone reverting the submodule URL.

**Decide one of:**

1. **Submit upstream PR.**  The change is one line; rationale is
   that `sys_icache_invalidate` is available on every Apple
   target, not just OSX.  If accepted, bump the submodule back
   to `asmjit/asmjit` master and delete the patch file + fork.
   This is the right long-term outcome.

2. **Keep the fork permanently.**  No external dependency, no
   waiting on upstream.  Cost: have to periodically rebase the
   fork onto upstream when bumping asmjit, and document that we
   carry a one-commit fork.  Delete the patch file (dead code).

3. **Drop the fork, restore in-tree patch.**  Submodule goes back
   to upstream `asmjit/asmjit`; cmake re-applies the patch on
   configure.  This was the pre-2026-05-17 setup.  Less clean
   than the fork (mutation of submodule worktree, fresh clones
   needed `cmake -B build` to apply) but no fork to maintain.

**Upstream readiness check (2026-05-17):**

- asmjit/asmjit is active (last push 2026-03-26, recent PRs #510,
  #508 merged Feb-Mar 2026).
- `gh search issues --repo asmjit/asmjit "catalyst OR sys_icache
  OR OSCacheControl"` → no existing issues or PRs match.  This
  fix has not been reported upstream.
- Upstream `asmjit/core/virtmem.cpp` at master still has the
  Catalyst-broken `#if TARGET_OS_OSX` guard around the include.
- avwohl/asmjit fork already has the fix committed on
  `iospharo-catalyst` branch (commit `1ce3ea65`, sits one commit
  past upstream HEAD `0bd5787`).

**Submitted as upstream PR — 2026-05-17:**
**https://github.com/asmjit/asmjit/pull/521** (OPEN, waiting on
maintainer review).

Check status:
    gh pr view asmjit/asmjit#521 --json state,mergeable,reviewDecision

If merged: bump submodule back to `asmjit/asmjit` master, drop
`.gitmodules` branch override, delete `third_party/patches/
asmjit-catalyst-virtmem.patch`, delete the `iospharo-catalyst`
branch on avwohl/asmjit.

If rejected: fall back to keeping the fork (option 2) or restoring
the in-tree patch (option 3).

Note: the patch file `third_party/patches/asmjit-catalyst-
virtmem.patch` is currently a no-op (cmake's
`git apply --reverse --check` succeeds because the fork has the
patch baked in, so apply is skipped — see CMakeLists.txt:127-151).
It's only used when the submodule URL points at vanilla upstream.

---

## E. Remaining JIT work

0. **JIT_DEFER blocks short-bench compile (2026-05-05).**  Sort 50K
   runs in ~2.5M interp steps; JIT_DEFER floor is 120M (~4 sec).
   So mergeFirst (49K calls during sort) never compiles in time —
   sort gets ~0% JIT speedup vs interp.

   **Architectural piece SHIPPED 2026-05-05** in `5d189328` — see
   `memory/project_jit_defer_queue_2026_05_05.md`.  Initial JIT
   compile now goes through a 256-slot safe-point queue
   (`PHARO_QUEUE_COMPILE=1`), drained between bytecodes alongside
   the existing recompile queue.  Compile no longer interleaves
   with mid-bytecode interp state — the sender-chain corruption
   that broke 7 prior attempts is gone.  Eval-mode `PHARO_DEFER_LIFT=1`
   (requires queue-compile) lifts defer on `#evaluate:` /
   `#evaluateDoIt:` entry; eval-mode benches now complete (vs
   100% hang previously).

   Bench-suite impact under PHARO_QUEUE_COMPILE=1 alone (no
   defer-lift): sieve 7 ms (vs 142 ms baseline, 20×), factorial
   23 ms (vs 200 ms, 9×); other benches at parity.  Both opt-in
   until wider validation.

   Remaining: defer-lift exposes intermittent "Improper store
   into indexable object" — separate JIT codegen bug in at:put:
   path triggered by post-lift compile order.  Default-on for
   defer-lift blocked on resolving that.  Sort 50K (the original
   target) doesn't benefit yet — that bench uses SessionManager
   startUp:, not eval-DoIt; needs detection extension to first
   user-installed startUp: handler entry.

1. **Architectural T1/T2 interaction (§1.3).**  T2 intercepting
   methods still breaks T1's inline-IC warmup in the non-coexist
   (REPLACE=1) path — neither shared-IC, warmup delay, nor
   self-only narrowing has fully solved this.  Coexist default
   (§1.3c) sidesteps it by not replacing T1 at all.  Full fix —
   if ever needed — is a design rethink (shared IC table across
   tiers?  patch-T1-when-T2-compiles?).
2. **Multi-bc §1.2e block activation** (`PushFullBlock` /
   `PushClosure`).  Uses the existing `ExitBlockCreate` chain-loop
   path.  Enables non-inlined blocks to be T2-compiled; marginal
   benefit since Pharo inlines `to:do:` / `whileTrue:` at compile
   time.

3. **Megamorphic-dispatch crash + perf regression** (2026-04-23).

   **Crash side: RESOLVED 2026-04-28** in commit `cbbf7254`.  Three
   independent IC-corruption bugs:

     (a) `bit-63 / bit-60 OR-merge collision` in
         `patchJITICAfterSend` — trivial-getter classification
         (`extra = (1ULL<<63) | slotIdx`) was unconditionally OR'd
         with the J2J merge `(1ULL<<60) | jitAddr`, leaving both
         bits set and low-16 = `slotIdx | (jitAddr & 0xFFFF)`
         (typically 43416).  Inline-getter fast path then read
         way past the receiver and returned garbage that became
         the next send's receiver → DNU.
         Fix: `TRIVIAL_BITS` guard on the J2J merge + skip
         bit-63/62/61 classification when receiver tag != 0.

     (b) Cross-site IC poisoning when slot 18 is 0 post-GC.  The
         `if (icSelectorBits != 0)` selector cross-check was
         bypassed once `recoverAfterGC` zeroed slot 18, so a
         stale `pendingICPatch_` from a different site got
         written into the empty slot.
         Fix: when slot 18 is 0, recover the site's expected
         selector from the side-channel `selBitsArray` (set at
         compile time, never zeroed) and compare against the
         send's selector.

     (c) Same gap in `upgradeICToJ2J`'s empty-slot fill path.
         Mirrored fix.

   Bench panel (post-fix) is 1.5–13× faster than harness Cog on
   all 6 tests.  See `project_next_handler_context_crash.md` for
   diagnostic flags retained as kill-switches and the JIT
   disassembly recipe (`_HOLE_RT_J2J_TRACE` event 200) used to
   root-cause it.

   **Perf side: PARTIALLY ADDRESSED.**  The `selBitsArray`
   side-channel (option (b) below) is now in use as part of
   cbbf7254's fix.  This eliminates the noSelBits slow path
   the original perf regression was driven by.  Re-measurement
   2026-04-28: `Object new pointersTo` completes under default
   `PHARO_JIT_DEFER=4s` (eval prints `0` correctly).  Under
   `PHARO_JIT_DEFER=0` (JIT-from-start) the heap walk still
   hangs at ~7.96M steps with 300 methods compiled — JIT-during-
   heap-walk scheduling issue, separate from the IC corruption.
   Default-defer path is the production path; the JIT-from-start
   path is a benchmarking-only mode.

   Pre-fix history kept for reference:
   Same-image A/B:
     PHARO_NO_JIT=1           15/15 pass  (ProtoObjectTest)
     PHARO_JIT_DEFER=9999     15/15 pass  (JIT enabled, no compiles)
     default JIT              12/17 done at timeout
   Profile showed `#pointsTo:` called 297K times; IC hit rate 78%
   (vs 97% on non-heap-walk workloads).  256K of 1.5M sends took
   the slow noSelBits path because `recoverAfterGC` memsets each
   IC site (zeroing slot 18 / selectorBits).  Attempted fix
   88dd186 (zero only slots 0-17, keep slot 18) got IC hit 97.5%
   / noSelBits 0 but introduced a flaky SIGSEGV — reverted
   bfa20e7.

   Crash-signature capture (PHARO_JIT_KEEP_ICS=1, c48c1d3):
     Always SAME method oop=0x3003b5660 (codeSize=7920, numIC=3)
     Always offset 2444, instruction `ldr x10, [x24]`
     x24 = raw bytes from a Symbol/String (e.g. " in this").
   The JIT IC probe code was reading slot 18 (selector Oop) as
   if it were one of the 6 entry slots, then dereferencing the
   Symbol's char data — that's the crash that the side-channel
   fix obviates.  With memset-all, slot 18 stayed 0 post-GC and
   the probe's cbz skipped the crashing ldr; with KEEP_ICS=1,
   slot 18 retained its Oop and fell through to ldr.

   Memory: `project_next_handler_context_crash.md`,
   `project_jit_timeouts_are_slowness.md`.

Once §1.3 and §1.2e are sorted, JIT reaches diminishing returns
and C is where project value lands.
