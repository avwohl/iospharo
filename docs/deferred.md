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

### A5. arm64 bench-suite flaky hang at `runCollect` — RESOLVED 2026-05-18 (bench-suite stable)

After the 2026-05-18 inline-J2J shipping + bytecode-coverage
work, 8/8 bench-suite runs complete cleanly without the
`PHARO_NO_SISTA_DO_SPLICE=1` workaround.  `collect 10x100K`
runs in 248-250 ms consistently, `sieve x100` in 7-8 ms (no
splice regression).  Workaround removed from
`scripts/run_benchmarks.sh`; the env var still works if anyone
needs to re-enable it.  No further investigation needed unless
the hang reappears.

(Below is the original investigation — kept for archeology.)

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

**Iter N+26 (2026-05-20) — SELF_REC_BIT in IC extra.**

Self-recursive sends now encoded in IC extra bit 56 at patch time:
`Interpreter::upgradeICToJ2J` and `patchJITICAfterSend` set bit 56
when `callerMethod.rawBits() == cachedMethod.rawBits()` along with
the existing bit 60 J2J entry encoding.  `rewriteIcEntriesAfterRecompile`
preserves bits 48-63 (only rewrites entryAddr bits 47:0) so the bit
stays valid across method recompile.

asmjit-T1's inline-J2J emit replaces the runtime CM-oop comparison
(2 ldr + cmp + branch = 4 instr) with a single `tbz x7, 56`
(default xmethod-off path) or `tbnz x7, 56` (xmethod-on path).  In
the xmethod path the calleeCM load is deferred to the cross-method
branch where it's actually used for state.method/literals updates.

**Measured:** essentially flat — fib(28) 12.8 → 12.8 ms, fib(30)
34.7 → 34.6 ms.  Saved instructions are not on the critical path
(L1-hot JM[0] loads + cmp are absorbed by M1's OoO window).
Catch rate unchanged at 100%.  Smaller emit is still a long-term
i-cache win for hotter workloads with many sends per method.

**Iter N+25 (2026-05-20) — self-recursive callee setup shrink.**

Two further inline-J2J emit shrinks specialized for the SELF-RECURSIVE
path (the only path when `t1InlineJ2JXmethod` is OFF — the default):

1. **Skip redundant receiver load.**  The IC HIT setup already loaded
   the new receiver into `x1` via `ldur x1, [x2, rcvrOffsetBytes]`,
   and `x1` is preserved through the inline-J2J emit until the
   callee tail-call.  The old emit re-read it from the stack with
   `sub x13, x12, (nArgs+1)*8; ldr x14, [x13]` — 2 wasted instrs.
   Replaced with `str x1, [x0, OFF_RECEIVER]` directly.  Saves 2
   instr per inline-J2J site.

2. **Skip dynamic tempCount load + init loop when statically
   known.**  For the self-recursive path, `callee.tempCount ==
   caller.tempCount` and `nArgs == caller.argCount`, both compile-
   time constants for the method being JIT'd.  Plumbed
   `callerArgCount` / `callerTempCount` through `emitMethodBytes`
   to `emitOne_arm64`.  When `nArgs == callerTempCount` (no extra
   temps to init — e.g., `benchFib` with 1 arg / 1 temp), the
   entire init loop (`ldrb`, 3 arith, `mov nilBits`, `cmp`,
   `b_hs`, `str sp`) collapses to a single `str x12, [x0, OFF_SP]`.
   When `extras = callerTempCount - nArgs > 0` (up to 8), the loop
   is unrolled to N nil-stores + static sp computation.  Saves
   ~8-10 instr per inline-J2J site for typical recursive methods.
   The xmethod path (when enabled) still uses the dynamic loop
   because callee tempCount can differ.

**Measured (PHARO_BENCH=fib, M1):**

    bench         pre-N+25    post-N+25   cog       gap-to-cog
    fib(28)       13.3 ms     12.8 ms     3 ms      4.0×  (-3.8%)
    fib(30)       35.7 ms     34.7 ms     ~8 ms     4.3×  (-2.6%)

Catch rate unchanged at 100% (7.4M of 7.4M sends inline-J2J on
fib(28)).  Sieve and other benches unchanged.

**Iter N+24 (2026-05-20) — bcStart cache + inline-J2J stp-fold
(`9240027f` + `4da43536`).**

Two further emit shrinks at the inline-J2J self-recursive site:

1. **bcStart cache (`9240027f`).**  Added `bcStartCache` field to
   JITMethod, pre-computed in `compileViaAsmjit` from `compiledMethodOop
   + (2 + numLits) * 8`.  The inline-J2J emit was previously recomputing
   bcStart per send via a 7-instruction chain (load methodObj, load
   methodHeader, mask numLits, add 1, lsl 3, add 8, add).  Replaced with
   a single `ldr` from the cache.  Saves 6 instructions per inline-J2J
   site.  JITMethod grew 96→104 bytes; `TrampolineAsm.S` JM_SIZE bumped
   to match (runtime sentinel asserts agreement).

2. **tempBase + ip stp-fold (`4da43536`).**  Folded the separate ldr-str
   pairs for save.tempBase (offset 16) and save.ip (offset 24) into a
   single stp.  Saves 1 instruction per inline-J2J site.

**Measured (PHARO_BENCH=fib):**

    bench         pre-N+24    post-N+24   cog       gap-to-cog
    fib(28)       13 ms       13 ms       3 ms      4.3×
    fib(30)       36-39 ms    35 ms       ~8 ms     4.4×

fib(28) is within bench noise — M1's wide pipeline + memory-level
parallelism mask single-instruction savings on short methods.  fib(30)
shows a measurable 5-10% reduction; the deeper recursion makes the
cumulative saving visible.

**Iter N+23 (2026-05-19 late evening) — sieve correctness + inline-J2J
emit shrink (`de84c68e` + `8d983dff`).**

Two changes this iter:

1. **Sieve correctness — `de84c68e`.**  `Integer>>benchmark` (prime
   sieve) was returning 1 instead of 1028 under the default JIT
   (pre-existing — repros on 14a7fa73 too).  Bisection via
   `PHARO_ASMJIT_T1_JUMPS_SKIP_N` narrowed the trigger to the 3rd
   conditional-jump compile, which in sieve's compile order is
   Array>>at:put: (primIndex 61).  The cond-jump emit in a
   prim-60/61/62 method's bytecode body interacts badly with the
   prim prologue / caller IC.  Workaround: stub-compile any prim
   60/61/62 method that has conditional jumps in its body.  The
   send-site catch (primKind 14/15/16) still fires for the common
   case, so net perf loss is <5% per the iter N+19 measurements.
   Root cause of the cond-jump emit bug remains open; revert the
   gate when fixed.

2. **inline-J2J emit shrink — `8d983dff`.**  Folded the two
   ldr-add-str pairs for `j2jDepth` and `j2jTotalCalls` (6 instrs)
   into one 64-bit ldr-add-str + 2-instr immediate materialization
   (5 instrs).  The two fields are adjacent int32 at offsets
   160/164; depth caps well below 2^31 so the low-32 increment
   never carries into the high 32 bits.

**Measured (PHARO_BENCH=fib/sieve, this branch's HEAD):**

    bench         ours       cog        gap
    fib(28)       13 ms      3 ms       4.3×
    sieve x3      2 ms       ~1 ms      ~2× (was BROKEN — returned 1)
    factorial     <1 ms      <1 ms      noise

The fib gap remains architectural: each Sista bytecode emits ~148
bytes of native code (~37 ARM instrs); Cog gets to ~50 bytes per
bytecode via register-allocated whole-method codegen.  Closing the
gap needs Sista Phase 4 method inlining or equivalent.

**Iter N+22 (2026-05-19 evening) — prim 60/61/62 prologue with fmt 10-11 (`1d8ee8a7`).**

Restored the prim 60/61/62 prologue gate after adding fmt 10-11 (32-bit
WordArray / IntegerArray / WideString) handling alongside the existing
fmt 2 (Array) and fmt 16-23 (byte indexable) paths.  Per Spur:

  - fmt 10-11: numElements = slotCount*2 - (fmt - 10)
  - 32-bit slot at recv + 4 + idx*4 (slot[0] at byte offset 8)
  - prim 61 val gate: SmI in [0, 0xFFFFFFFF]

This recovers the 16M C primitive call savings (prim 60: 8.0M → ~65K,
prim 61: 7.0M → ~16K) that the N+21 revert removed, while keeping the
Improper Store correctness fix.

Remaining gaps (next fix target if/when they show up in hot workloads):

  - fmt 9 (Indexable64) — FloatArray, LargePositiveInteger, DoubleArray
    (size = slotCount; at:/at:put: requires 64-bit element handling)
  - fmt 12-15 (16-bit indexable) — DoubleByteArray, Utf32String
    (numElements = slotCount*4 - (fmt - 12))
  - fmt 3-5 (variable + fixed) — Context, etc.
    (needs fixedFieldCountOf adjustment via runtime helper)

Verified: eval "42 printString" + WordArray/ByteArray/String/Array
at:put: no longer raise Improper Store under default JIT.

**Iter N+21 (2026-05-19 evening) — revert prim 60/61/62 prologue (`9bd381a6`).**

The prologue added in `8b6d0495` only handles fmt 2 (Array) and fmt 16-23
(byte indexable).  For fmt 9 (Indexable64), fmt 10-15 (32-bit + 16-bit
indexable: WordArray, IntegerArray, DoubleByteArray), and fmt 24-31, the
prologue falls through to the Smalltalk fallback at the bytecode body.
For `Object>>basicAtPut:`, that fallback is `^ self errorImproperStore:
aValue` — raising spuriously for WordArray-like receivers where the C
primitive would have stored cleanly.

Reproducer: `./build/test_load_image /tmp/harness/Pharo.image eval "42
printString"` — startup hit `ExternalStructure class>>recompileStructures`
which stores into a WordArray via Object>>basicAtPut:, fires the spurious
errorImproperStore, cascades into the bench script's saved-image resume
producing `ERROR: Improper store into indexable object` in the bench file
before any benchmark ran.

Fix: revert just the `supportedPrimIndex` enablement for 60/61/62.  The
icDataPtr stash + `dispatchCachedRestoreX5` stub from `8b6d0495` are
independent correctness and stay in place.  The send-site catch via
primKind 14/15/16 still fires — per iter N+19's own numbers, the
prologue contributed <5% on top of the send-site path.

Proper fix when revisiting: handle fmt 9, 10-15, 24-31 in the prologue
following `Primitives.cpp:2240-2290`, OR change the prologue `fail` path
to call the actual C primitive instead of falling through to the
bytecode body (the second approach generalizes to other primitives).

**Iter N+20 (2026-05-19 PM) — SmallFloat send-site inline + perf assessment.**

Wired up send-site dispatch for SmallFloat +/-/* (primKind 21/22/23) in `b5aa17f4`.  The emit is correct (no crash, no regression) but the bench-suite's floatSum 1M loop is already SISTA-spliced — never reaches the regular send dispatch — so the new path's hit counter stays at 0 during the bench.  The dispatch path stands ready for non-SISTA workloads.

**Cumulative session perf (iter N+19/N+20):**

After 9 commits this session (3 diagnostic + 6 perf), the C++ primitive call rate dropped massively:

    primitive         before     after    catch
    60 (at:)          8.0M       65K      99.2%
    61 (at:put:)      7.0M       15K      99.8%
    62 (size)         1.4M       4K       99.7%
    541 (Float+)      ~1.0M      <137     >99.9%

Bench-suite (ours vs Cog reference):

    bench         ours    cog    Δsession   gap
    fib(28)       11      3      same       3.7×
    sort 100K     325     17     same       19×
    dict 50K      293     13     same       23×
    sum 1M        98      3      same       33×
    floatSum 1M   116     8      same       14.5×
    stringHash    95      2      same       48×
    collect 100K  249     41     -2%        6×
    select 100K   306     8      -21%       38×

Most numbers stable.  Select 10x100K dropped 388→306ms (21%) — the only clearly measurable improvement.  The other benches didn't move because the C++ primitive dispatch was already cheap (~50ns/call); removing 16M of those saves ~800ms total but spread across the suite.

Remaining 19-48× gaps to Cog are architectural (Sista tier-2 inlining, closure direct call, method inlining) and not closeable via more send-site inline-prim work.

**Iter N+19 (2026-05-19 PM) — send-site prim 60/61/62 inline shipped (`8b6d0495`).**

Root-caused why g_primAt_hits/atPut_hits/size_hits were always 0:
- supportedPrimIndex didn't recognize prims 60/61/62 → target->hasPrimPrologue=false → IC patch never set primKind=14/15/16 in extras → send-site tryPrimAt/AtPut/Size emit was unreachable.
- Even after enabling, the send-site emit had a latent bug: it clobbered x5 (which holds icDataPtr) with slotCount during header decode, then bailed to dispatchCached which expected x5 = icDataPtr → ldr x6,[x5,8] faulted.

Fix: introduced `dispatchCachedRestoreX5` label that reloads x5 from OFF_ICDATAPTR (pre-stashed at the top of each tryPrim* block) before falling through to dispatchCached.

Bench-suite primitive call counts (top primitives):

    prim       before     after    inline catch
    60 (at:)   8.0M       65K      99.2%
    61 (at:put:) 7.0M     16K      99.8%
    62 (size)  1.4M       4K       99.7%

Bench numbers are within noise — sort 326ms, dict 290ms, select went 322→303ms, collect went 253→244ms.  The C++ prim dispatch was already cheap (~50ns/call), so killing 16M C++ calls saves ~800ms total but spread across many benches.  The bigger gains require closing the JIT→C++→JIT round-trip on every send, not just dodging the C++ primitive impl.

**Chain-break protocol re-verification (2026-05-19):** with all gates ON (`PHARO_T1_INLINE_J2J_XMETHOD=1`, `MAX=-1`, `RECEIVER_SYNC=1`, `POST_SEND_IP=1`, `SPLIT_POOL=1`), the bench-suite completes cleanly 5/5 runs at 122-127K xmethod fires.  Previous documentation indicated 32-50K thresholds for various corruption modes — those are gone for the bench-suite workload.  Sunit broader workloads still vulnerable (SEGV at unlimited cap), so production default stays at MAX=30000.

Diagnostic infra added:
- `PHARO_PRIM_PROFILE=1` — dump top-30 primitives by C++ call count at exit
- `PHARO_T1_XMETHOD_LOG=1` — dump xmethod ring buffer at terminateCurrentProcess
- `PHARO_T1_INLINE_BLOCK_VALUE_NONLEAF=1` — opt-in for non-leaf block-value inline (re-test the iter N+16 leaf-only gate)
- `g_xmethod_count` exposed in JIT stats dump

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

**Data point 2026-05-17 (loop iter 1):** `30 benchFib` run with
`PHARO_T1_INLINE_J2J=1 PHARO_DUMP_JIT_STATS=1` reports
`inline-J2J: hits=0 bail_zero=0 bail_full=0 bail_self=468886
(catch rate 0.0%)`.  Confirms 468K sends arrive at tryInlineJ2J
with bit 60 set (eligible) in a single fib(30).  Currently 100%
bail.  At ~500cy savings/hit, full capture = ~78ms on M1 — i.e.
the inline path alone closes ~30% of the fib(30) Cog gap.

The catch rate is artificial: bail path is unconditional
(`a.b(j2jBail)` after counter increment).  Real capture potential
once the chain-break protocol is solved is the full 468K.

**Chain-break protocol options (consolidated 2026-05-17 iter 1):**

The fundamental problem: when an inline-J2J'd callee's inner send
misses J2J (no bit 60 on the inner IC), it falls through to
dispatchCached.  The C++ chain loop then activates a target T via
activateMethod (pushing an interp frame).  When T eventually
returns via its prelude, j2jDepth > 0 → it pops OUR save → tail-
calls OUR resumeAddr.  But T wasn't entered via J2J; the interp
frame is orphaned.

Three plausible fixes:

1. **j2jEntryFlag in JITState + priorFlag in J2JSave.**  Each
   inline-J2J emit saves prior flag into save struct, sets flag=1
   before br.  Return prelude only pops when flag==1.  Restores
   prior flag on pop.  Chain-loop activateMethod always clears
   flag=0 before re-entering JIT.  Requires: JITState field add,
   J2JSave field add, activateMethod path update, all return
   prelude sites updated to check flag (not depth).  ~5 files.

2. **Pure-J2J callee gate.**  Only inline-J2J when callee's
   bytecode contains no sends that could miss J2J.  Implement as
   lazy `isPureJ2J` bool on JITMethod, computed by scanning bytecode
   for any send whose IC's extras lack J2J_ENTRY_BIT.  Cheaper than
   #1 but catches less.  For benchFib it works (all inner sends are
   to prims or recursive benchFib).

3. **blr-based with state restore.**  Don't use J2J save mechanism
   at all.  Emit: save caller state in callee-saved regs
   (x19-x28); set up callee state; blr entry; check exitReason; if
   EXIT_RETURN, restore caller state + push retval; otherwise bail.
   Doesn't handle inner chain-bounce cleanly (callee's exit via
   dispatchCached needs activateMethod handling).  Per option (b)
   investigation, this is the path that hit "callee doesn't run
   to completion" wall.

Iter 1 conclusion: option #2 (pure-J2J gate) is the cheapest
viable shipping path for fib-like workloads.  Next iteration
will scan benchFib's bytecode to confirm all sends qualify, then
emit the unconditional inline for the qualifying-self-recursive
subset.

**Iter 2 finding 2026-05-17:** Wrote self-recursive inline-J2J
emit (calleeJM via x7 mask, compare callerMethod oop vs
calleeJM->compiledMethodOop, push J2J save, br entry).  Code is
correct per asmjit log (`and x9, x7, 281474976710655; sub x10,
x9, 96; ldr x11, [x0, 56]; cmp x10, x11; b.ne L19`).

Runtime check: with knob on, `30 benchFib NO_SISTA`:
`inline-J2J: hits=0 bail_zero=0 bail_full=0 bail_self=9869`.

Switching the check to `state.jitMethod->compiledMethodOop` vs
`calleeJM->compiledMethodOop` (stable across recompile) didn't
help — still 0 hits.  Debug dump shows the last-seen comparison
had `last_caller=0x30046d180 last_callee=0x30037c950` — TWO
different method oops.  No self-recursive matches observed.

**Surprising finding:** benchFib's recursive send is NOT hitting
tryInlineJ2J at all.  Only ~9.8K bit-60 entries observed in fib(30)
run (without Sista); a 30-deep benchFib has ~514K recursive sends.

**Iter 3 instrumentation 2026-05-17:** Added two counters to
asmjit-T1 IC HIT path (gated on PHARO_T1_INLINE_J2J=1):
- `g_inlineJ2J_dbg_ic_hits` — every IC HIT entry
- `g_inlineJ2J_dbg_extra_no_bit60` — HIT with extra but no bit 60

For `30 benchFib NO_SISTA`:
- ic_hits = 48,164 total
- extra_no_bit60 = 31,929 (66% — extras set but bit 60 missing)
- tryInlineJ2J entries (bit 60 set) = 7,142
- Self-recursive matches = 0

Also added upgradeICToJ2J calls to BOTH fast chain loops
(Interpreter.cpp:16650 + 18713) on the ExitSendCached path —
before pharo_jit_convert_send.  Bit 60 should now get set on any
IC that takes the fast J2J-conversion path.

**Iter 3 conclusion 2026-05-17:** Logged upgradeICToJ2J's callee
selectors.  ALL early upgrades are for `#new:` (from
`initialize:`) or `#at:put:` (from `atNewIndex:put:`) — same IC
sites being repeatedly invoked.  Filtered for "fib" in the
selector — NO entries.  benchFib's IC is NEVER reaching
upgradeICToJ2J.  Filter for "fib" in patchJITICAfterSend selector
— also NO entries.

This means benchFib's IC is never being updated to add bit 60.
asmjit-T1's IC probe stays cold on benchFib's recursive site,
so every recursive call falls through to the chain loop's
J2JCall handling without ever upgrading the IC.

Next iteration:
1. Verify benchFib is being JIT-compiled at all (or stays interp).
2. If JIT'd, trace what exitReason fib's recursive call produces.
3. Either patch the J2JCall path to also upgrade IC, OR fix
   asmjit-T1's emit to upgrade IC on miss.

**Iter 5 finding 2026-05-17:** Added compile + activation + chain
loop traces (all gated on PHARO_T1_INLINE_J2J=1):

- `[JIT-COMPILE-FIB]` confirms benchFib IS compiled (oop=0x3008a33f8,
  jm=0x108469840, tier=1, state=Compiled).
- `[FIB-ACTIVATE]` fires multiple times — benchFib's JIT method is
  activated through tryJITActivation millions of times (matches 4M
  total activations from JIT stats).
- `[FAST-CHAIN1-SC]` and `[FAST-CHAIN2-SC]` (added to both fast
  chain loops' ExitSendCached path) NEVER fire — neither fast chain
  loop sees ExitSendCached for benchFib's calls.
- `[SLOW-EXIT-SEND]` fires for findElementOrNil:/scanFor:/name/= but
  ALL with `ic=0x0` (no IC data).  NONE for benchFib.
- `[IC-PATCH]` for first 30 patches all show `pending=0x0` —
  interpreter-level paths that don't set pendingICPatch_.  No
  benchFib in the first 30.

So benchFib's recursive call:
- Doesn't exit JIT with ExitSendCached (no fast chain loop fires)
- Doesn't exit JIT with ExitSend (slow path's IC pointer is null)
- Doesn't trigger upgradeICToJ2J or patchJITICAfterSend for fib
- Yet tryJITActivation activates benchFib 4M times

Mystery: how does benchFib re-enter tryJITActivation if not via
chain-loop's send-exit handlers?  Possible: an inline JIT-to-interp
path (stencil/legacy) that doesn't go through ExitSendCached;
or an activateMethod path from a non-instrumented entry.

Next iteration: instrument tryJITActivation entry path to see what
WHO is calling it for benchFib (caller selector + call site).

**Iter 6 ROOT CAUSE 2026-05-17:** Found via post-compile IC dump.
benchFib's JIT method compiles as **STUB** (isStubOnEntry=1,
numICEntries=0, icBuffer=NULL).  Every call hits the stub's
bail-on-entry, returns ExitSend with NO icDataPtr, interpreter
handles via activateMethod, repeat.

Bytecode of benchFib: `4c 20 62 c2 51 ed 0b 4c 51 61 81 4c 20 61
81 60 51 60 5c ...`

Opcode `0xed` at offset 5 is the SistaV1 long-conditional-jump
(jumpIfFalse with 2-byte offset).  asmjit-T1's
`allBytecodesSupported` only handles SHORT jumps (0xB0-0xC7) —
long jumps (0xE8-0xEF range) fall through to `return false`.

The opt-in `PHARO_ASMJIT_T1_ENABLE_JUMPS=1` (default OFF, despite
comment claiming "default-on since 2026-05-16") enables short
conditional jumps but NOT long jumps.  benchFib needs both.

**The inline-J2J emit + chain-loop IC upgrade code is correct
but unreachable** until asmjit-T1 supports more bytecodes.
benchFib (and likely most methods with non-trivial control flow)
compile as stubs.  The 0% catch rate isn't an inline-J2J bug;
it's a JIT-coverage gap.

**Path forward to ship inline-J2J on arm64:**

Option A: Add 0xE8-0xEF long-jump support to asmjit-T1's
`allBytecodesSupported` + `emitOne_arm64`.  Unblocks benchFib +
many other methods.  Then inline-J2J's self-recursive path can
finally fire.  Estimated: ~200 LoC for the jump emit + label
fixup.

Option B: Detect "method compiled as stub" path and skip inline-
J2J optimization entirely (no benefit anyway).  Easy but doesn't
ship the win.

Option C: Make `PHARO_ASMJIT_T1_ENABLE_JUMPS=1` default-on AND
extend it to handle 0xED long jumps.

The deferred A6 "ship inline-J2J on arm64" goal therefore
**depends on Option A** as a prerequisite.  The inline-J2J emit
in tree (opt-in PHARO_T1_INLINE_J2J=1) is ready and will fire
correctly once methods like benchFib actually compile.

**A6 SHIPPED 2026-05-17 iter 7 (commit `57cc0d91` + default-on flip):**
Long-jump support + buffer-cap raise + jumps default-on +
inline-J2J default-on.

Bench results on `[N benchFib] timeToRun`:

  config                                fib(28)  fib(30)
  baseline (NO_INLINE_J2J + NO_JUMPS)    ~684     ~684 ms
  jumps-on + INLINE-J2J (new default)     13       31 ms

12× faster on fib than baseline.  fib(28) Cog gap closed from
115× to 6.5×.  No regression on factorial 5000 (24ms) or sieve
x100 (7ms; Sista-spliced).

The catch rate of inline-J2J on fib(30): 64.1% (2.69M of 4.2M
sends hit the self-recursive inline path).  bail_self (cross-
method bit-60 sends) accounts for the rest — those'd need the
non-self-recursive emit (still TODO).

Opt-out: PHARO_T1_NO_INLINE_J2J=1 (also PHARO_ASMJIT_T1_NO_JUMPS=1).
Opt-in PHARO_T1_INLINE_J2J=1 still enables per-bail debug counters
+ traces for further investigation.

**Iter 8+ (2026-05-18): more bytecodes shipped to broaden compile
coverage.**

- `c63c8f72` PushInteger 0xE8 (literal SmI push, NN < -1 or > 2)
- `e6aaa96f` PushCharacter 0xE9 (literal Character push)
- `c251d485` Extended push/store family:
  - 0xE2 ExtPushRecvVar, 0xE3 ExtPushLitVar, 0xE4 ExtPushLitConst,
    0xE5 ExtPushTemp (index 0-255 via operand byte)
  - 0xF2 ExtPopStoreTemp, 0xF5 ExtStoreTemp
- `33c8acb7` PushFullBlock 0xF9 (bail to ExitBlockCreate; 3-byte)
- `dfd4b454` PushArray 0xE7 (bail to ExitArrayCreate; chain
  loop ExitArrayCreate handlers sync state.ip → instructionPointer_
  when tier=1)
- `2031d98a` PushThisContext 0x52 (bail to interp via
  EXIT_ARITH_OVERFLOW; partial JIT — interp runs the rest of
  the method)
- `13c5ce48` 4 binary special selectors (0x69 /, 0x6A \\, 0x6B @,
  0x6D //) routed through Phase 4 IC
- `f3bd48ab` Remote temps 0xFB/0xFC/0xFD as bail-to-interp
- `bc25a1d0` ExtA/ExtB prefix bytes — explicit first-pass
  rejection (until prefix-state plumbing lands).  Removes the
  per-byte fallthrough noise; one trace per rejected method.
- `99f3ee62` Inline \\ (0x6A) and // (0x6D) — moved from Phase
  4 IC to inline arith with floor-div sign-adjustment.
- `bae7797d` Inline ExtPopStoreLitVar 0xF1 + ExtStoreLitVar 0xF4
  (Association slot store, no write barrier — YG scavenge scans
  all of old space).
- `0xEC InlinedPrimitive` shipped earlier as a 2-byte no-op.

Remaining unsupported (eval-startup trace, 2026-05-18):
- 0xE1 ExtendB / 0xE0 ExtendA prefix bytes (15 hits — methods
  bailed by the first-pass guard).  Need full prefix-state
  plumbing across pre-scan + emit.
- 0xEA/0xEB ExtSend/ExtSuperSend (10 + 5 hits).  Naive
  acceptance breaks snapshot startup with #do: DNU on
  UndefinedObject class.  Behind opt-in flag
  PHARO_T1_ACCEPT_EXTSEND=1 pending investigation.

**Non-self-recursive inline-J2J still TODO** — iter attempted but
state.literals update corrupted callee execution (PRIM-AT-BADIDX
+ `#do: was sent to nil` after a few inline-J2J levels deep);
needs lldb to trace what state field is being mishandled.  The
36% bail_self rate remains unaddressed.

**Cross-method investigation 2026-05-18 (commits 848be634,
b9730279, 06837bdb, d2475452):**

Found partial root cause via bisection.  PHARO_T1_INLINE_J2J_XMETHOD=1
opt-in gate enables cross-method emit; bisection helper
PHARO_T1_INLINE_J2J_XMETHOD_MAX=N counter-limits fires.

Even MAX=1 (single cross-method fire) corrupts state during
image init.  Logged that fire's state values via C helper
(jit_rt_xmethod_log):
- callee CM=0x30033ae08 JM=... numLits=3 argCount=8 tempCount=16
- caller CM=0x30033a940 JM=... (same shape!)
- callee bc[0..7]: 40 f9 00 00 7b d8 40 5c
  = PushTemp 0, PushFullBlock <block>, special-send sel-11,
    Pop, PushTemp 0, ReturnTop
- state.literals correctly set to calleeCM+16 by xmethod emit

Partial fix shipped (commit `b9730279`): chain loop's
ExitBlockCreate / ExitArrayCreate handlers now sync C++ globals
(receiver_, framePointer_, method_) from state when J2J chain
active (state.j2jDepth > 0), AND skip the state-overwrite-from-
globals in the resume re-setup.  Correct in principle but doesn't
fully fix corruption.

Tried gates that didn't help:
- callee.numICEntries == 0 (no inner sends): still corrupts
- callee.argCount == nArgs (correct arity): still corrupts

Remaining bug: when callee bails to chain loop via PushFullBlock
(ExitBlockCreate), something in the resume protocol still
mishandles callee's state.  Needs lldb single-step through
`createFullBlockWithLiteral` + `tryResume` to find which field
becomes wrong and where.

**Iter N (2026-05-18) lldb diagnostic:** SEL-CORRUPT trace reveals
**IP/method_ inconsistency** at the DNU point:

    [SEL-CORRUPT #1] selector fmt=28 cls=3117 raw=0x300350ce8
        bc=0x5C litIdx=-1 method=0x30033ae08 (#addAllLast:) fd=16
    [SEL-CORRUPT]   IP=0x30033a96b method bytes=0x30033ae10

method_ = callee's CM (#addAllLast:) at 0x30033ae08.
method.bytes() = 0x30033ae10.
But IP = 0x30033a96b which is at CALLER's bytecode position 3
(caller CM = 0x30033a940, bytecode start = 0x30033a968, +3 = 0x30033a96b).

So instructionPointer_ is in CALLER's bytecode area but method_ is
CALLEE's CM.  These should be consistent.

Lead for next iter: find where IP/method_ get out of sync.  Likely:
either chain-loop prefix updates method_ from state.jitMethod
without checking IP consistency, OR my xmethod return prelude
correctly restores state but C++ method_ gets stale-updated
afterwards.

**Iter N+1 (2026-05-18) suspect: `state.ip` set to method-START
in ExitBlockCreate resume path:**

Interpreter.cpp:19871 (ExitBlockCreate handler):
```
state.ip = methObj->bytes() + (1 + numLits) * 8;
```

This sets state.ip to method START, not the resume offset
(post-PushFullBlock = bcOffset).  tryResume runs JIT at
codeOffset corresponding to bcOffset, but state.ip says method
start.  Inconsistency.

JIT code mostly doesn't read state.ip mid-execution (only on
bail-out), so this might be benign in the JIT phase.  But if
the callee subsequently bails to chain loop, state.ip = method
start, and chain loop's prefix sets `instructionPointer_ = state.ip
= method start`.  Then bytecode dispatch at "method start" with
method_ = callee's CM reads bytecode at the wrong offset.

Worth testing: change ExitBlockCreate to set state.ip from
bcOffset (or skip the assignment) to keep IP consistent with
tryResume's actual entry point.  ExitArrayCreate has the same
pattern (line 19926 area).

Tested 2026-05-18: `state.ip = methObj->bytes() + (1+numLits)*8
+ bcOffset` — doesn't fix corruption.  Default still works.
So state.ip is not (alone) the bug.

**Iter N+2 (2026-05-18) found j2jPool_ COLLISION:**

Interpreter.cpp:16574 + 18712 + 18517: `rj2jSaves` and `j2jStack`
both point to `&j2jPool_[base]` where `base = j2jPoolCursor_`.
The JIT's `state.j2jSaveCursor` is initialized to the same base
(Interpreter.cpp:18452).

So JIT inline-J2J pushes saves to `j2jPool_[base + 0]`, advancing
`state.j2jSaveCursor`.  Chain loop pushes `rj2jSaves[rj2jDepth=0]`
to `j2jPool_[base + 0]` — **SAME MEMORY ADDRESS**.  Collision.

When asmjit-T1 cross-method inline pushes save_A, then callee
chain-breaks (e.g., PushFullBlock → ExitBlockCreate), and then
callee's inner send fires that chain loop handles via J2JCall
path — the chain loop writes its save to the same slot as save_A.
When callee returns via prelude, prelude pops the OVERWRITTEN
save (= chain loop's data, not save_A).  Wrong state restored.

Tested fix: `int j2jDepth = state.j2jDepth;` (chain loop starts
its index after JIT's pushed entries).  BROKE DEFAULT.  Reason
TBD — likely state.j2jDepth has non-zero values from prior JIT
calls that aren't quite right for this entry, OR materialize
needs to be adjusted to only iterate chain-loop's range
(state.j2jDepth..rj2jDepth-1), not all 0..rj2jDepth-1.

The proper fix is one of:
1. Separate pool slices for rj2jSaves and state.j2jSaveCursor
   (memory layout change in Interpreter::tryJITActivation init)
2. Make chain loop track state.j2jDepth as offset for index
   (more pervasive change to push/pop/materialize)
3. Disable cross-method inline-J2J when chain-loop activity is
   expected (heuristic)

Option 1 is cleanest.  Requires j2jPool_ layout change:
asmjit-T1 cross-method gets first M slots; chain loop gets
slots M+.

**Diagnostic flags:**
- PHARO_T1_INLINE_J2J_XMETHOD=1: enable cross-method (broken)
- PHARO_T1_INLINE_J2J_XMETHOD_MAX=N: bisect-limit fires
- PHARO_T1_INLINE_J2J_XMETHOD_BRK=1: brk at xmethod for lldb
- PHARO_T1_INLINE_J2J_XMETHOD_LOG=1: printf state at xmethod

**Iter N+3 (2026-05-18, commit `44d93188`) split-pool stability fix.**

Pre-fix: with `PHARO_T1_J2J_SPLIT_POOL=1` (option 1 from N+2 above),
the materialize site at Interpreter.cpp:19068-19094 read
`j2jStack[i]` directly without the split-pool ternary.  In
split-pool mode, JIT pushes inline-J2J saves to `stateSaves`
(`&j2jPool_[j2jStateBase]`), not `j2jStack` (`&j2jPool_[j2jPoolBase]`).
So the loop read zeroed memory at j2jStack[i], fired the null-saveJM
warning, and aborted materialization.  Net effect: split-pool alone
broke the image startup ("logDuring:/logError:inContext:" trace).

Post-fix: introduced `matSrc` ternary at the materialize site (mirrors
the 4 other sites already updated at lines 19211, 19417, 20114, 20573).
With split-pool ON, the full bench suite completes (fib28=13ms,
fact5k=29ms, sieve=10ms, sort=461ms, dict=389ms, sum=127ms,
floatSum=169ms, stringHash=125ms, collect=1ms-spliced, select=478ms).
Numbers within ±5% of default — no win on its own (purpose is to
enable cross-method inline-J2J without the j2jPool collision).

**Cross-method crash bisection (2026-05-18 post-fix):**

With `PHARO_T1_J2J_SPLIT_POOL=1 PHARO_T1_INLINE_J2J_XMETHOD=1` on
`runSum` (sum 1M):

  MAX     crashes (3/3)   median ms
  0       0/3              115
  100     0/3              137
  1000    0/3              123
  5000    0/3              ~120
  10000   3/3              CRASH
  100000  3/3              CRASH
  inf     5/5              CRASH

Crash signature: PC in JIT code zone at consistent offset 0xec4 in
some method, with LR 0x...bc8 nearby.  symbolicates to
`Interpreter::interpret + 956`.  Instruction is `ldr x9, [x8]`
preceded by `ldr x8, [x28, #0xa0]` — offset 160 of x28 (= JITState
j2jDepth field if x28 = state ptr).  Suggests stale-save pop in
return prelude, but precise root cause needs lldb step-through.

Tried `chain-loop isolation` (save+clear+restore state.j2jDepth
around the chain-loop's `JIT_CALL(entryAddr, &state)` sites at
Interpreter.cpp:16870 + 18955).  Hypothesis: outer's inline-J2J
push leaves state.j2jDepth=1; chain-loop activates inner method T
which doesn't push but its return prelude pops outer's save with
wrong resumeAddr.  Build clean.  No regression on default config.
Did NOT fix the cross-method crash — fires at same MAX threshold.
Reverted.

Conclusion: cross-method has additional state corruption beyond
the j2jPool collision (which split-pool fixed).  Next investigation
needs interactive lldb step-through of one xmethod fire's full
lifecycle.  Until then: cross-method opt-in remains broken;
self-recursive inline-J2J (default ON, 64% catch rate on fib(30))
is the only inline-J2J in production.

**Iter N+4 (2026-05-18, commits `4de3eeb8` + `c069c192`) inline-prim-bitops.**

Adds inline SmI bitAnd:/bitOr:/bitXor: dispatch at the IC HIT site
in asmjit-T1's send emit.  primKind bits 52:48 distinguish prim
type; when SmI receiver + matching primKind, emit the bitwise op
inline (tagged-op semantics) + retag for bitXor:, skipping the
chain-loop round-trip.

Also added `case 16: return 19;` to `inlinePrimKind()` so the IC
patch sets primKind for bitXor: (prim 16) IC entries.

Fires from both paths:
- After tryInlineJ2J self-recursive bail (bit 60 set, cross-method)
- Direct fall-through (bit 60 unset, callee not JIT-compiled)

Bench impact: ~stringHash 100K runs 95-97ms (was 97-99ms baseline).
Within noise — the stringHash inner-loop bottleneck is `k hash`
(String>>hash primitive) + closure dispatch + Array>>do: iteration,
not the bitXor: send.

Default-on; PHARO_T1_NO_INLINE_PRIM_BITOPS=1 opt-out.  Infrastructure
ready for primKind expansion to:
- primKind 14 (at:) — would benefit sort/dict/sum (Array indexing)
- primKind 7 (=)    — SmI equality via named-send (common in collections)
- primKind 10 (==)  — identity via named-send

The above need more emit (bounds check for at:, true/false oop loads
for =) — left for next iteration.

**Bench gap vs Cog — progression 2026-05-18 → 2026-05-19 (default config):**

  Bench              Cog   2026-05-18  Today  Δ      Ratio
  fib(28)              3     13         11   -15%   3.7×
  sieve x100          10      9          8   -11%   0.8× (wins)
  sort 100K           17    399        326   -18%   19×
  dict 50K            13    354        290   -18%   22×
  sum 1M               3    121         94   -22%   31×
  factorial 5K         2     26         24    -8%   12×
  1M blocks            3      2          0     -    (splice wins)
  1M getter+yourself   2    124          0    -    (splice wins)
  100K alloc           3      7          5   -29%   1.7×
  floatSum 1M          8    151        111   -26%   14×
  stringHash 100K      2    113         96   -15%   48×
  collect 10x100K     41    301          1     -    (splice wins)
  select 10x100K       8    490        383   -22%   48×

Changes shipped on 2026-05-18 → 2026-05-19:
- `44d93188` split-pool materialize fix (enables future cross-method)
- `4de3eeb8` + `c069c192` inline SmI bitwise prims (bitAnd/bitOr/bitXor)
- `eef15c62` inline at:/at:put: for fmt-2 Array
- `ea3656ea` inline size for fmt-2 Array
- `54c03c92` tryPrimEq attempted + reverted (primKind 10 dispatch overhead
  added ~4 instr/send to every send and didn't pay off in benchmarks)

**Iter N+5 (2026-05-19, commit `605a5f7b`) j2jEntryDepth infra.**

Implements option (a) per-entry depth snapshot.  Adds
`j2jEntryDepth` field to JITState (offset 200), initialized in
`tryJITActivation` and updated at the two chain-loop J2JCall
sites (Interpreter.cpp:16870 + 18955).  asmjit-T1's return
prelude now checks `current j2jDepth > entry depth` instead of
just `> 0`, so a chain-loop-activated method can't accidentally
pop an OUTER method's save.

No regression on default (entryDepth stays 0 in non-xmethod
configs, prelude pop behavior unchanged for that case).

**Cross-method still crashes** at MAX>5000 fires.  Crash PC shifted
by 44 bytes (0xec4 → 0xef0) due to code-growth from the new
field check, but the crash is at the same RELATIVE location in
`Interpreter::interpret` — `ldr x8, [x28+0xa0]; ldr x9, [x8]`.
Offset 0xa0 = 160 corresponds to JITState.j2jDepth (8-byte load
gets j2jDepth + j2jTotalCalls).  x28 might be holding a JITState
pointer somewhere in interp's main loop; the dereferenced value
looks like garbage.

Next iteration investigation: lldb single-step from the
chain-loop's J2JCall through the crash, observe register
evolution.  Without lldb, can't isolate the second corruption
source.  Also try: cross-method with state-validate asserts at
every iteration boundary to find the corruption-introducing
fire.

**Iter N+6 (2026-05-19, lldb investigation, commit `5b728347`):**

Reproduced cross-method crash under lldb.  Key findings:

- Crash PC: `Interpreter::interpret + 956`, instruction
  `ldr x9, [x8]` after `ldr x8, [x28, #0xa0]`.
- x28 = address of `interp->stackPointer_` (compiler base-of-hot-fields
  trick).  So `x28 + 0xa0` = address of `interp->receiver_`
  (offset confirmed via `PHARO_DUMP_INTERP_OFFSETS=1`).
- Fault address `0x656c646e61487265` decodes to ASCII "erHandle"
  (low byte first) — likely Symbol content like #...erHandle...
- So `interp->receiver_` was overwritten with a Symbol's CONTENT
  bytes (not its header oop).
- Dereferenced as object header → SIGSEGV.

Added prim-only gate (commit `5b728347`): cross-method bails when
callee has a declared primitive (bit 16 of methodHeader).  This
excludes `FullBlockClosure>>value:` (prim 207) where asmjit-T1
has no prim prologue, so cross-method would skip the prim and
run `^ self primitiveFailed` fallback.  Verified XLOG output:
only no-prim Smalltalk callees pass the gate now.

**Crash persists with prim-only gate.**  Even pure-Smalltalk
callees (#max:, #copyFrom:to:, #header, etc.) trigger the
corruption after ~10K xmethod fires.  So the bug isn't
value:-specific — it's a chain-break protocol issue with
Smalltalk callees whose inner sends bail.

Analysis of the flow:
- M_A pushes save_A (caller state).
- Branches to M_B's JIT entry.
- M_B runs, hits inner send that bails to chain loop.
- Chain loop syncs `interp->receiver_ = state.receiver = M_B.receiver`.
- Chain loop processes bail, activates inner T.
- T runs, returns.
- Chain loop resumes M_B via JIT_CALL of save.resumeAddr.
- BUT: does it sync `interp->receiver_` back?  Looking at J2JReturn
  handler at Interpreter.cpp:18957+, it sets state.receiver from
  save.receiver but does NOT update `interp->receiver_`.

When materialize fires (j2jDepth > 0 → synthesize savedFrames_),
receiver_ DOES get set to state.receiver per line 19097-19099.
So that path is covered.

Suspect: a code path where chain loop returns to JIT (resuming
caller) without going through materialize, leaving `interp->
receiver_` set to the inner method's receiver.  When that JIT
eventually returns to interp via tryJITActivation completing,
interp dispatches next bytecode reading the stale receiver_.

This needs interactive lldb step-through to confirm — set hardware
watchpoint on `&interp->receiver_`, run xmethod-enabled bench,
inspect each write to identify the bad write site.

**Iter N+8 (2026-05-19, commit `80dc9dd4`) second corruption fixed.**

Found via XLOG bisection (errorNotIndexable count vs MAX): the
error count goes from 0 at MAX=100 to 4 at MAX=300 to 19 at MAX=5000,
indicating cumulative state corruption from xmethod fires.

Root cause: inline-J2J save.ip was set to state.ip = current send
position (pre-send).  The chain-loop's J2JCall handler at
Interpreter.cpp:18829-18833 advances state.ip past the send BEFORE
saving, so its save.ip = post-send.  Materialize sites read save.ip
into frame.savedIP; when interp pops that frame, instructionPointer_
= savedIP.  With pre-send savedIP, interp re-executes the send.

Fix: PHARO_T1_J2J_POST_SEND_IP=1 stores `state.method +
bcOffsetFromMethObj + 1` (post-send IP for single-byte Phase 4
sends) into save.ip instead of raw state.ip.

Bisection proof:
  xmethod + receiver-sync alone:     ~550-1031 errorNotIndexable/run
  xmethod + receiver-sync + post-ip: 0 errors across 5 runs

So combining both fixes eliminates the in-image errorNotIndexable
corruption.  Cross-method now WORKS for limited fires:
  MAX=0:    sum 1M = 110ms (xmethod effectively off)
  MAX=1000: sum 1M = 114ms (some xmethod fires, slight check overhead)
  MAX=5000: hangs (process timeout)
  MAX=10000+: CRASH (x0 = 0x6, garbage in state pointer)

So a third issue surfaces around MAX=5000+ fires.  Suspect:
something accumulates in J2J save pool or chain-loop state across
many xmethod fires, eventually causing infinite loop or branching
to garbage.

Performance note: xmethod with limited MAX is slightly SLOWER than
default (110ms vs 93ms baseline) because xmethod's check overhead
applies to every send while the cross-method gain only fires for
some.  Need to ship cross-method default-on with high MAX to
actually see the win — needs the third issue fixed first.

Default config (no xmethod) unaffected by either fix.

**Iter N+9 (2026-05-19, commit `cce90145`) leaf+nonstub safety gates.**

Bisection on `42 printString` eval-mode workload found the corruption
threshold collapses to MAX=~20 fires (not the MAX=5000+ originally
documented — that was for `sum 1M` which exercises a narrower set of
methods).  Adding two safety gates raised the threshold from MAX=20
to MAX=~30,000 — a ~1500x improvement:

1. **Leaf-only gate**: reject callees with `numICEntries > 0`.  When
   the callee has inner sends, those sends can take chain-break exits
   (ExitSendCached, ExitArrayCreate, …) that the inline-J2J return
   prelude can't restore correctly.  Pure leaf callees always exit
   via the return bytecodes through the prelude → balanced push/pop.
2. **Non-stub gate**: reject callees with `isStubOnEntry == true`.
   A stub method's JIT body is just `mov [s+OFF_EXIT], ExitSend; ret`,
   which BYPASSES the return prelude entirely.  The xmethod push
   happens but the pop never does → state.j2jDepth permanently +1.

Bench-suite (full PharoOursBench, MAX=20000, xmethod+SP+RS+PI ON):
no observable crashes, modest perf changes in noise (±10%).  Default
config (xmethod OFF) unchanged.

**Residual corruption past MAX=30K not yet root-caused.**  Both
prior cases (`xmethod into leaf-with-sends` and `xmethod into stub`)
were structural — the fix is identical (refuse to xmethod) rather
than restoring state correctly.  The remaining slow drift past
MAX=30K is likely a different class of bug — possibly accumulated
delta in state.j2jSaveCursor or save-slot reuse across recompile
boundaries.  Needs lldb watchpoint instrumentation to find.

**Iter N+10 (2026-05-19) block-value inline infrastructure shipped
(`b351afbd`, `14576331`, `2c013c15`).**

Mirrors the xmethod path but targets BLOCK_VALUE_BIT (bit 59) IC
entries — invocations of FullBlockClosure>>value:/value:/...  The
asmjit-T1 emit checks bit 59 BEFORE bit 60 (when t1InlineBlockValue
is on); when bit 59 set, blr's into `jit_rt_inline_block_value_prep`
(JITRuntime.cpp).  The helper validates the closure layout, looks up
the block's JM via methodMap, applies leaf+nonstub+canBailMid+noprim
gates, pushes a J2J save with resumeAddr=afterSend, sets up callee
state from closure slots (receiver from slot 3, captures from
slots 4..N copied to temp area), and returns the block JIT entry
address.  asmjit emit then `br x0` to enter the block.

`Interpreter::upgradeICToJ2J` was also patched (when eager-compiling
prim 207/209 targets that have no prim prologue, we now write a
minimal IC entry with BLOCK_VALUE_BIT alone instead of bailing without
touching the IC).  This is what makes the asmjit-T1 emit's bit-59
check fire at all.

**Bench impact: net zero.**  Two failure modes:

1. **Lookup-miss dominant**: 20K tries / 0 hits when run with the
   helper's "bail on lookup miss" gate.  User blocks aren't JIT-
   compiled at the time of the value: invocation, so methodMap.lookup
   returns null.  Tried synchronous compile from helper → SIGSEGV
   (compiler not safe mid-send).  Tried queueing via
   `JITRuntime::queueInitialCompile` (drain at safe points) → blocks
   eventually compile and helper starts firing.  But:

2. **State corruption when helper fires**: 826 hits at 79.3% catch
   rate → bench fails with "withAllSuperclassesDo: is nil".  Same
   chain-break protocol issue as xmethod inline-J2J.  Inner sends in
   the block bail to chain loop; chain loop activates inner method;
   inner method's return path can't restore the right state.

Both paths are blocked on the same chain-break protocol fix as
xmethod cross-method.  Infrastructure is in tree behind opt-in
PHARO_T1_INLINE_BLOCK_VALUE=1 (default OFF) — ready to enable when
the protocol is corrected.

**Files / new flags:**
- `JITRuntime.cpp`: `jit_rt_inline_block_value_prep` helper (validate
  + lookup + J2J save + callee setup + capture copy).
- `JITRuntime.cpp`: `g_jitRuntimeForBlockValue` singleton (set in
  ctor) for future eager-compile use.
- `AsmjitT1.cpp`: bit-59 check before bit-60, emit blr to helper, br
  to returned entry.  Counters g_blockValue_tries/hits/bails +
  per-gate bail breakdown printed by dumpJITStats.
- `Interpreter.cpp`: write minimal IC entry with BLOCK_VALUE_BIT in
  upgradeICToJ2J when target has no prim prologue (prim 207/209).
- `DebugSettings.{hpp,cpp}`: `t1InlineBlockValue` flag (PHARO_T1_
  INLINE_BLOCK_VALUE=1 opt-in).

**Next iteration: chain-break protocol fix.**  Until this is solved
(needs lldb step-through), both xmethod and block-value inline are
opt-in / dormant.  Once fixed, both unlock simultaneously and
should each contribute 15-50% gains on send-heavy benchmarks.

**Iter N+7 (2026-05-19, commit `2c1370f2`) partial fix shipped.**

Added `PHARO_T1_J2J_RECEIVER_SYNC=1` opt-in that syncs Interpreter::
receiver_ and method_ at the chain-loop J2JReturn handler.  With
this opt-in:

- xmethod no longer SIGSEGVs (chain-loop bail/resume no longer
  leaves stale callee-receiver in interp->receiver_).
- BUT the crash signature shifts to caught
  `SmallInteger>>errorNotIndexable` — a second state-corruption
  source surfaces where SmI is incorrectly used as an indexable
  receiver.  The bench's Error-handler catches it; bench
  doesn't produce output but VM doesn't die.

Suspect second corruption: state.argCount or state.activeContext
isn't being restored properly somewhere in the chain-break flow,
causing a downstream send to use stale receiver.  Or my
cross-method emit's receiver computation (`sp[-(nArgs+1)*8]`) is
sometimes off — maybe sp gets bumped between IC HIT entry and the
xmethod state setup.

Next iteration: set hardware watchpoint on `interp->receiver_` (or
`state.receiver`) and find the bad write site that introduces SmI.
Or scan all chain-loop sync sites for missing `_ = state.*`
restores on J2J return paths.

The stencil version at `stencils.cpp:1642-1731` inlines
FullBlockClosure>>value/value: by extracting compiledBlock from
slot 1, looking up its JM via methodMap, then doing a J2J save +
direct entry call.  Skips the chain-loop activation of value: +
prim 207 dispatch.  For sort/select/collect-style benches with
100K+ block invocations, full inline would save ~40ms per bench.

asmjit-T1 implementation challenges:
- methodMap lookup needs a C helper (~6-probe linear scan)
- Tempbase setup + captured-value copy requires a per-call loop
  (closure slot 4..N → callee temp slots)
- Chain-break recovery: if block bails mid-execution to chain loop
  (inner send the block can't inline), the helper-driven path can't
  cleanly resume — would need to drive a mini chain loop in the
  helper or emit full inline with proper bail-protocol.

Helper-only approach (call C helper that does everything) is the
simpler implementation but loses much of the win to the C-call cost
and bail-recovery complexity.  Full inline emit (mirroring the
stencil) is the right shipping path but ~80 lines of arm64 emit.

5-15% gains across most benches.  Larger improvements (e.g.
floatSum -26%, getter+yourself -100%) include Sista splice
trigger variance.

The 14–48× remaining gaps are all in iter-heavy methods with
small-callee sends in the inner loop.  Closing them needs:
- Cross-method inline-J2J (blocked by chain-break protocol —
  needs interactive lldb investigation)
- Block.value: inline (asmjit-T1 doesn't yet handle BLOCK_VALUE_BIT)
- Method inlining (Sista Phase 4, queued)

**Iter 11 (2026-05-18): added InlinedPrimitive 0xEC + ext-recv
store family (`2f5823ff`, `888168c4`).**  Removed last common
unsupported simple bytecodes.

**PushArray 0xE7 attempted but reverted** (twice in this session).
First attempt emit set state.ip + cachedTarget then bailed with
EXIT_ARRAY_CREATE.  Result: state-corruption-style DNUs during
image init (`#do: was sent to nil`, `#extent` DNU).  After
revert, fib still 12ms.  The chain loop's resume protocol after
ExitArrayCreate is subtle (case handler doesn't reset
instructionPointer_ from state.ip — relies on it being set
earlier).  Needs lldb to trace the resume after PushArray bail.

**Remaining unsupported (rare or complex):**
- 0xE0/0xE1 ExtA/ExtB prefixes (modifies next bytecode operand)
- 0xEA/0xEB ExtSend/ExtSuperSend (needs IC slot allocation +
  ExtA/B support; bail-to-chain attempt corrupted state)
- 0xE7 PushArray (resume protocol mismatch — see above)
- 0x6A `\\`, 0x6B `@`, 0x6D `//` (need send-style bail with IC site)
- 0xF1/0xF4 ExtPopStoreLitVar / ExtStoreLitVar (need association
  write barrier)
- 0xFA-0xFD 3-byte bytecodes (PushClosure / RemoteTemp variants)
- 0x52 PushThisContext (needs context materialization)

**Scaffold landed 2026-05-17 (`f81d61a0`, `078105ce`):**
arm64 inline-J2J path wired up with per-bail counters, opt-in via
`PHARO_T1_INLINE_J2J=1`.  Currently always bails (counts as
`bail_self`); baseline perf unchanged with knob off.

**Option (a) progress (2026-05-17, `21b6dfaa`):** return prelude
landed in tree.  All arm64 return emit sites (returnTop, returnReceiver,
returnTrue, returnFalse, returnNil) now check `j2jDepth` first and
pop+tail-call-to-resumeAddr when > 0; fall through to normal
exit-to-C++ ret when 0.  No-op when no inline-J2J emit pushes
saves (default).

Tried the matching send-side push.  Worked for pure-recursive
fib (59K inline hits at 100% catch rate; chain stays in JIT) but
corrupts when ANY callee inner-send misses J2J and falls through to
dispatchCached.  The dispatchCached path exits to the C++ chain
loop, which calls activateMethod (pushing an interp frame) and
re-enters via tryJITActivation.  The newly-entered method's
returnTop sees the still-present save and pops/tail-calls back
to A's resumeAddr — bypassing the interp frame.  Cross-chain
confusion produces SettingTree>>pragmasDo: error traces.

**The chain-break case has no obvious local fix:**
- Range-checking the popped save's resumeAddr against the save's
  own jitMethod code zone DOESN'T help — the check passes because
  the save's resumeAddr IS within its (correct) caller's code.
  The wrong-pop happens because j2jDepth doesn't distinguish how
  the current frame was entered.
- Per-frame "entry-depth" tracking would work but needs a new
  JITState field + each method prologue records its entry j2jDepth
  + returnTop pops only when current j2jDepth > entry-depth.
  Requires asmjit-T1 to gain a per-method prologue and the
  invariant maintenance.
- Have dispatchCached pop the pending save before exiting.  But
  then A's resumeAddr is lost; chain loop re-entry would have to
  recreate the continuation, which requires re-entering A's
  compiled code at post-send IP via tryJITActivation.  This is
  essentially "make dispatchCached imply a chain-loop activation."

Net: option (a) is half-shipped (return prelude is in tree, benign).
The send-side push is blocked on a chain-break-protocol decision
that's bigger than a single emit-site change.

**Option (c) (C helper owning chain-loop processing) — ATTEMPTED
AND CRASHES, 2026-05-17.**

Wrote `jit_rt_inline_j2j_call(state, entry, calleeJM, methodBits, nArgs)`
in JITRuntime.cpp.  Helper saves caller state, sets up callee
state in JITState, calls `entry(state)` directly via C call, returns
the retval on EXIT_RETURN or 0 on any other exit (signaling bail).
Caller's emit calls the helper via blr.

First helper call goes through correctly (printf trace confirms
args sane: state=valid, entry=valid JIT code, methodBits=valid heap
oop, nArgs=1).  But entry's compiled code crashes a few levels deep
in the recursion (fib of 5 is enough to trigger).  PC at crash is
in the JIT code zone but BEFORE the called entry's codeStart —
execution branched to garbage.  Register state at crash: x0 =
0x680008 (looks like a method-header word, not a state ptr); state
ptr survived in x6 and x10.

Hypothesis: helper recurses via `entry(state)` → JIT code → inline-
J2J emit calls helper again → another entry call → fib(5) ~10
helper frames deep.  Something corrupts state in the recursion —
possibly the nested entry call's state setup uses values the
recursive context left in registers.

Reverted; tree back to clean (return prelude only, opt-in send path
always bails).  BOTH option (a) AND option (c) need lldb attach
through the recursive chain — beyond what this session can deliver
via static reasoning + printf.

**Option (c) retry with lldb (2026-05-17 session 2):**

Re-applied option (c) with two additions:
1. Helper validates calleeJM (state==Compiled, codeSize sane,
   entryAddr in code range) — bails on stale IC entries.
2. lldb attached to fib(5) run with breakpoint on helper.

**Surprising finding:** crash happens BEFORE the helper is even
called (breakpoint never fires).  At crash:
  - PC = 0x107f4e4a4 in some JIT method's code at offset 452
  - x0 = 0x0000000000680008 (junk — being dereferenced, sigsegv)
  - x2 = 0x107f4e600 (calleeJM was being set up)
  - x6 = 0x30033ae08 (methodBits)
  - x7 = 0x1000000107f4e660 (extras, bit 60 set + entryAddr)
  - x10/x23 = 0x16fdfc7a8 (state ptr survived in those regs)
  - bt shows only tryJITActivation → JIT (1 frame, no helper)

The inline-J2J emit was MID-SETUP (x2/x6/x7 all populated) when
the crash hit.  The crashing instruction is `ldr x2, [x0]` —
a typical first-instruction-of-some-bytecode pattern (Pop, ReturnTop,
etc.).  So execution jumped to that location with bad x0 BEFORE
the helper was called.

Theories I couldn't validate from static + single-shot lldb:
- asmjit's `mov(x9, Imm64)` mis-encoded → blr to garbage (but
  we'd see a frame in bt then)
- The b → endOfSend after helper return jumps to wrong address
  (but bail path never reached helper — should fall through to
  dispatchCached normally)
- The inline-J2J emit corrupts adjacent emit somehow
- Asmjit's sub/add sp manipulation around the blr corrupts SP
  beyond the helper's prologue

All paths require **interactive lldb stepping** through the emit
instruction-by-instruction starting from when we know x0 is good
(at IC HIT) to when x0 becomes 0x680008.  Not deliverable in a
batch session.

**Permanent bookmark:** the option (c) implementation in
`f81d61a0` history shows the helper signature, args, and emit
structure.  Next session can cherry-pick that diff onto a fresh
branch and run lldb with proper step-instruction loop.

Reverted, tree clean.

**lldb brk confirmation 2026-05-17 session 2+:**

Re-applied option (c) with `a.brk(asmjit::Imm(0xBEEF))` as first
instruction of `tryInlineJ2J`.  lldb attach catches the brk:
  - PC = brk site (in JIT method)
  - x0 = 0x16fdfc7a8 (valid state ptr)
  - x7 = 0x100000010800 95a0 (extras with bit 60 set)

So control DOES reach tryInlineJ2J with valid state.  Continuation
past the brk hits another brk (another inline-J2J site, also with
valid state).  After many continues, eventually crashes in
unrelated dyld code (process exited).

So the corruption must happen AFTER the emit's tryInlineJ2J body
runs.  Either:
1. The emit's `mov x9, helperAddr` mis-encodes (asmjit bug for
   certain 64-bit values), so `blr x9` jumps to garbage
2. The helper executes but corrupts state in a way that downstream
   bytecodes hit
3. The emit's `ldr x0, [sp, 0]` after blr doesn't restore the
   right x0 (asmjit sp-relative addressing quirk?)
4. Buffer overflow: cap = bcLen*128+128 may not fit inline-J2J
   per-send overhead.  Many methods fail compile with warnings
   `[asmjit-t1] code.code_size=X out of [1, M]` — but those bail
   to interp, no corruption.

Genuine next steps (need interactive lldb session):
- Set conditional breakpoint at `mov x9` site, single-step through
  the mov-imm64 sequence (movz + 3 movk), verify x9 is helper addr
- Set breakpoint at the `blr x9` itself; verify we enter
  `jit_rt_inline_j2j_call` at the C function's prologue
- If blr doesn't reach helper, disassemble the emitted bytes to
  see what mov-imm64 actually encoded

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

**Iter N+19 (2026-05-19) — ring-buffer trace + investigation status.**

`asmjit-T1` xmethod-log ring buffer (`fed752d8`): the buffer now
captures the LAST 64 fires rather than the first 64.  Per-fire fprintf
gated to fires 1-8 only.  Useful for post-corruption diagnosis.

Captured fires near MAX=31000 (works) and MAX=31900 (fails):
- Both show `state.receiver=0xcedb18710` or `0x7387aea90` — high-address
  values that look stack-like but are actually valid eden heap oops
  (Pharo allocates new objects in high address ranges, e.g. 0xc000...).
- The "stack-address receiver" hypothesis is wrong.

So the actual corruption signature in state at fire ~31853 is more
subtle.  Investigation paused — needs interactive lldb step-through
to identify the specific state field that becomes wrong, rather than
post-hoc trace inspection.

**Iter N+18 (2026-05-19) — materialize both slices fix (`88487bb3`).**

`Interpreter::tryJITActivation` materialize block had a bug when
split-pool is on AND BOTH state.j2jDepth and the local chain-loop
j2jDepth are positive: pre-fix code only materialized frames from
ONE slice (stateSaves if state.j2jDepth > 0, else j2jStack), losing
saves in the other slice and producing nil-receiver / wrong-method
corruption downstream.

Fix iterates state.j2jDepth frames from stateSaves first (older
xmethod-pushed), then `local - state.j2jDepth` frames from j2jStack.
Total frames = sum.  Bench-suite at MAX=-1 now completes (the
previous "FloatPrintPolicy >> #nil:" cascade is gone).

`42 printString` eval reliability at MAX=-1:
  Pre-fix:  fails ~100% past the threshold
  Post-fix: 1/10 PASS (the 9 fails first-DNU at xmethod_count ~32K)

So a third corruption source remains past ~32K fires.  The
materialize fix unlocked the bench-suite (which fires ~50K
xmethods total) but not the eval-startup workload (which fires
more rapidly and hits the next threshold).

**Iter N+15-17 (2026-05-19) — additional fixes + cumulative status.**

- `9e3e44ac` — block-value helper was nil-initializing extra temp
  slots with raw 0 instead of the actual nil oop (which has
  rawBits == 0x300000000).  Pharo image code that compares against
  the special nil oop saw raw-0 slots as some OTHER invalid value.
  Real bug, fixed; doesn't address the iter N+16 leaf-gate
  resurfacing but is correctness work on its own merits.
- `d7334d90` (originally `3e3c3b62`, reverted then reinstated) —
  `Interpreter::forEachRoot` now scans the J2J pool's
  `save.receiver` Oops.  Previously these were GC-invisible: pending
  xmethod-pushed saves accumulated stale receiver refs across GC,
  causing nil-receiver corruption when popped later.

Threshold movement on `42 printString` eval bisection:
  Pre-session:                    MAX=33000 (sharp)
  Post-send-IP fix (N+14):        MAX=100000 (sharp → non-deterministic past)
  Plus GC scan + nil-oop (N+17):  MAX=50000 (consistent) → fails at 100K

**Iter N+14 (2026-05-19) — FIRST CHAIN-BREAK BUG ROOT-CAUSED + FIXED (`8c319249`).**

CLI-based lldb investigation pinpointed the first concrete corruption
bug: in the xmethod inline-J2J emit, when PHARO_T1_J2J_POST_SEND_IP=1
is set, save.ip was computed as `state.method + bcOffsetFromMethObj +
1`.  But state.method had already been overwritten with the callee's
compiled-method oop at the cross-method state-update block ~50 lines
earlier.  Result: save.ip = calleeCM + caller_bcOffset + 1, a bogus
address into the callee's memory.

When interp later materialized this save into a SavedFrame and
resumed, `instructionPointer_` landed past the (typically short)
callee's bytecode end — for `findString:` (42-byte method), IP was
1621 bytes past start.  Interp dispatched garbage as Send opcodes,
reading nil from out-of-bounds literal slots → nil selectors → DNU
cascade.

**Diagnostic that found it:** added a temporary printf at the
existing DNU site that dumps `state.method`, `state.literals`,
`instructionPointer_` offset, and `xmethod_count` when selector is
nil.  Output `method=findString: ip off 1621 (of 42)` showed the IP
was wildly out of range — confirming corrupted state.ip.

**Fix:** use x12 (caller's CM oop, loaded at line 2722 and not
overwritten before the save push) directly, instead of reading the
already-mutated `OFF_METHOD`.

**Threshold movement:** `42 printString` eval-mode bisection
  Pre-fix:  fails at MAX=33000 (the long-standing sharp threshold)
  Post-fix: fails at MAX=100000 (different corruption mode)

**Residual: nil-receiver DNUs past MAX=100K.**

A different corruption mode surfaces past MAX=100K: instead of nil
selectors (fixed above), the receiver is nil when the bytecode
expects a heap object.  Trace shows stack-address-shaped values
(e.g. 0x4fa87b8f8) leaking into frame slots — looks like a
`tempBase` or `sp` value being written where an Oop should be.

Possible causes (untested):
- Save struct field mis-stored: save.tempBase being read as
  save.receiver on materialize (offsets are correct on push but
  drift may happen via some other path)
- A J2JSave ring-buffer wrap-around once state.j2jDepth exceeds
  J2JSlotPerEntry=32 within a single tryJITActivation
- xmethod fires DURING a chain-loop activation overwriting the
  chain-loop's rj2jSaves slice (despite splitPool — perhaps an
  off-by-one in the slice boundaries)

Next iteration: diff state at fire N=31000 (just under threshold)
vs fire N=33000 (just past) — what's special about the next
specific call?

**Iter N+13 (2026-05-19) — workload-dependent corruption.**

Discovery: the 32K threshold is `42 printString` eval-specific.  The
full bench-suite at MAX=100000 completes cleanly, but at unlimited
(`MAX=-1`) deterministically fails during floatSum with `Message not
understood: FloatPrintPolicy >> #nil:` — a receiver-type corruption.

This is NOT a global fire-count threshold; it's a workload-specific
pattern.  Different workloads hit different corruption points
depending on which methods get xmethod'd and in what sequence.

Also discovered fib(28) regresses 12ms → 135ms in bench-suite context
when xmethod is ON at the safe default (MAX=30000) — but fib(30) in
ISOLATION runs cleanly at 30ms.  So xmethod-ON disturbs bench-suite
state in a way that hurts fib's measured timing.  Could be process-
switch interaction with the xmethod check overhead, or cumulative
near-corruption state from earlier benches (tinyBenchmarks fires
many xmethods before fib runs).

**Production recommendation: keep xmethod OFF by default** (which
is the case).  The PHARO_T1_INLINE_J2J_XMETHOD=1 opt-in is safe in
the sense of "no observed crashes on tested workloads up to MAX=
30000" but is not a net perf win without the chain-break protocol
fix.

**Iter N+12 (2026-05-19) — sharp threshold at ~32K fires.**

Re-bisection on `42 printString` eval under all four xmethod flags
ON (XMETHOD + SPLIT_POOL + RECEIVER_SYNC + POST_SEND_IP):

  MAX=32000  PASS (4/4)
  MAX=32500  FAIL (4/4)
  MAX=33000  FAIL (4/4)

The threshold is deterministic and sharp.  32768 = 2^15 suggests a
16-bit signed counter overflow somewhere in the xmethod pipeline,
but the obvious candidates (state.j2jDepth = int32, JITMethodStats
fields, MaxJ2JPoolSize = 1024) don't fit.  The default safe cap is
set to 30000 in DebugSettings (`19f2b320`) — well below the 32K
deterministic threshold.

**Iter N+11 (2026-05-19) — lldb investigation plan for chain-break protocol.**

Both xmethod default-on (deferred A6 above) AND block-value inline
default-on (iter N+10) are blocked on the same root cause: when a
non-leaf callee bails mid-execution to the chain loop (e.g., on an
ExitSendCached for an unsupported send), and the chain loop then
activates an inner method, the inline-J2J save protocol corrupts
state.  The fix unlocks both paths simultaneously and is the largest
remaining lever for closing the Cog gap on send-heavy benchmarks
(sort, dict, sum, stringHash, select — each 15-50× slower than Cog).

Specific bug-hunt steps for a future lldb session:

1. **Reproduce minimal:** under `PHARO_T1_INLINE_J2J_XMETHOD=1
   PHARO_T1_INLINE_J2J_XMETHOD_MAX=300`, run `eval "42 printString"` —
   this consistently fails around MAX=30 fires with corruption.
   Should give a focused repro with O(30) xmethod fires.

2. **Set watchpoint** on `interp->receiver_` (offset 0xa0 from
   `interp->stackPointer_` per `PHARO_DUMP_INTERP_OFFSETS=1`).
   At the moment the watchpoint fires with a non-Oop value (e.g.
   Symbol bytes), `bt` shows the bad write site.

3. **Likely candidates** (from prior static reading):
   - Chain loop's `J2JCall` handler at Interpreter.cpp:18971 sets
     `state.receiver = calleeRecv` but doesn't sync `interp->
     receiver_`.  If a bail from the callee returns to interp
     before materialize fires, `interp->receiver_` is stale.
   - `state.j2jSaveCursor` may be off across the J2J Call/Return
     boundary when xmethod fires during the JIT_CALL.
   - `state.j2jEntryDepth` restoration at line 19009 happens
     AFTER `JIT_CALL` returns — if a longjmp-style exit fires
     mid-call, the restoration is skipped.

4. **Hypothesis to verify:** the chain loop's activate-inner-method
   path needs to TEMPORARILY MATERIALIZE the in-flight xmethod saves
   to `savedFrames_` before pushing its own save, then re-push them
   on return.  Or alternatively, the inline-J2J emit should never
   fire when there are pending chain-loop saves (state.j2jEntryDepth
   tracks this).

5. **Validation:** once a fix candidate lands, run with
   `PHARO_T1_INLINE_J2J_XMETHOD=1` (no MAX) on full bench-suite +
   `42 printString` eval + `1 to: 100000 do: [:i | i printString]`.
   All three should complete cleanly.  Then flip to default-on and
   re-run the standard A/B bench.  Expected gains: sort -25%, dict
   -20%, stringHash -15% (block-value path).  xmethod default-on
   would also help fib(28) recursion gap modestly.

Infrastructure already in tree (no-ops until protocol is fixed):
- `g_debug.t1InlineJ2JXmethod` + safety gates (leaf/stub/canBail)
- `g_debug.t1InlineBlockValue` + helper + IC fill + bit-59-first
- `g_debug.t1J2JReceiverSync` + `t1J2JPostSendIp` + `t1J2JSplitPool`
- Per-bail counters dumped by `dumpJITStats`
- `g_jitRuntimeForBlockValue` singleton for future eager-compile
- `Interpreter::queueInitialCompile` drain at safe points (used by
  block-value helper for async block compile)

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
