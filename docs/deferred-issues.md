# Deferred Issues

Last updated: 2026-04-14

Issues that were identified during test-suite runs and deferred rather
than fixed. Each entry has a hypothesis, what's been ruled out, and a
concrete next step.

## 1. Harness SemaphoreTest / valueWithin timing interaction

**Tests affected (10 in total, all pass standalone):**

    ProcessTest>>testResumeAfterBCR
    SemaphoreTest>>testWaitAndWaitTimeoutTogether
    SemaphoreTest>>testWaitTimeDuration
    SemaphoreTest>>testWaitTimeDurationWithCompletionAndTimeoutBlocks
    SemaphoreTest>>testWaitTimeoutMSecs
    SemaphoreTest>>testWaitTimeoutSecondsOnCompletionOnTimeout
    BlockClosureValueWithinTest>>testValueWithinNonLocalReturnDoesNotTimeout
    BlockClosureValueWithinTest>>testValueWithinTimingRepeatMilliseconds
    BlockClosureValueWithinDurationTest>>testValueWithinTimingNestedInner
    BlockClosureValueWithinDurationTest>>testValueWithinTimingRepeat

**Symptom:** timing-sensitive assertions (Delay-based waits, valueWithin
timeouts) fail with "Assertion failed" or "Denial failed" inside the
SUnit harness, but pass under every other invocation path.

**Ruled out:**
- Cross-test pollution: SemaphoreTest fails even when it runs FIRST in
  a batch (commit e5e0fba).
- VM logic: each test passes `tc runCase` and `tc run: TestResult new`
  standalone, including under a deliberate P40-fork wrapper.

**Hypothesis:** the harness's `runSingleTest:selector:timeout:priority:on:`
wraps every test in P40 test fork + P60 relinquish watchdog + 4 nested
exception handlers (TestFailure / Deprecation / TestSkipped / Error) +
ensure:. Some combination of that stack disturbs `Delay>>wait`'s
excess-signal cleanup or the `Semaphore>>wait:` timeout race.

**Next step:** simplify `scripts/pharo-headless-test/run_sunit_tests.st`
runSingleTest: so timing tests aren't wrapped in the double-watchdog —
either by detecting them (class name match) and using `tc run: result`
directly, or by dropping the P60 relinquish watchdog once we trust the
P40 deadline. Owner: harness submodule.

## 2. Reflection-walk perf under batch load

**Tests affected:** any test that walks `allObjects` or `allInstances`:

    ProtoObjectTest>>testFastPointersTo        (logic fix: commit 1730e5a)
    ProtoObjectTest>>testPointersTo
    ProtoObjectTest>>testPointersToCycle
    ByteSymbolTest>>testAs
    ByteSymbolTest>>testNewFrom
    ByteSymbolTest>>testReadFromString
    HashTableSizesTest>>testHashTableSizes
    (plus 10+ others surfaced across seven batches)

**Symptom:** `pointersTo:` / `allInstances` completes in <1s standalone
but exceeds the 80s watchdog under batch load (after 1000+ tests have
populated the heap).

**Ruled out:**
- Correctness: testFastPointersTo passes standalone after the context-
  exclusion fix (commit 1730e5a) and returns the right set of pointers.
- Harness artifacts: this is independent of the wrapper stack — even
  with a P40 fork alone, the walk takes >80s once heap is large.

**Hypothesis:** our `memory_.allObjectsDo` is a linear scan over every
heap word (~2-3M ops by mid-batch). Standard Spur uses a class-table
fast path for `allInstances` that skips chunks with no instances of the
target class. We don't have that fast path.

**Next step:** profile a batch-mid `pointersTo:` call to confirm the
scan is the hot loop. If so, add a class-table-indexed accelerator for
`allInstances:` (primitive 177) and change `pointersTo:` (prim 132) to
short-circuit on small receivers using a per-class-index filter.
Medium-scope change to `src/vm/ObjectMemory.cpp`.

## 3. Weak-reference / finalization timing tests

**Tests affected (6 known):**

    WeakArrayTest family (3)
    FLWeakObjectsTest family (some)
    EphemeronDictionaryTest family (depending on image)
    FinalizationProcess-related tests in KernelTests

**Symptom:** assertions that check "the finalizer ran" fail because the
image's FinalizationProcess (priority 50) either hasn't woken yet or
has been starved by test activity. Whether the finalizer actually fires
depends on scheduler timing, not just GC.

**Ruled out:**
- Ephemeron scanning: task #7 fix verified; weak slots are cleared and
  ephemerons queued correctly.
- GC incrementalism: fullGC() produces the same finalization set as
  manual walk.

**Hypothesis:** the tests race with FinalizationProcess wakeup. Our
signal-to-process path from GC → FinalizationProcess may deliver on a
later tick than Cog, so tests that wait `N ms` and check "fired?" see
no fire yet.

**Not yet ruled out:** whether `Semaphore>>signalFinalization` from
C++ GC code correctly schedules the priority-50 process, or whether
there's a lost wakeup when GC happens under an already-active P50.

**Next step:** instrument the signalFinalization path with a counter,
compare against Cog on the same workload. If we under-signal, find the
lost-wakeup; if counts match, the issue is wakeup latency and we need
to either scale the tests' sleep budget or boost the FinalizationProcess
priority briefly during GC. Scope: a day of diagnostics work in
`src/vm/ObjectMemory.cpp` and the P50 scheduler path.

## 4. JIT eval-mode boot hang

**Symptom:** with `PHARO_NO_JIT=0` in eval mode, any expression
(including `Smalltalk snapshot: false andQuit: true`) hangs. Image
compiles ~190 methods during StartupPreferencesLoader's chain, then
enters idle loop and never runs the eval expression. Process sits at
99% CPU in idleProcess, stacksampled to `primitiveRelinquishProcessor
→ usleep`.

**Ruled out:**
- JIT initialization: `PHARO_JIT_THRESHOLD=999999` (init but no compile)
  exits cleanly. Threshold 100 or default (2) hangs.
- Specific expression: `42 printString` and `Smalltalk snapshot:
  false andQuit: true` both hang identically. Not expression-specific.

**Bisected with `JIT_MAX_COMPILE=N`:**
- N=0 (no compiles): exits cleanly (code 0).
- N=1 (compile one method): crashes with SIGSEGV (code 139).
- N=5: errors out (code 1).
- N≥10: hangs indefinitely.

So the first compiled method already triggers a crash. The crash
stack: `primitiveFlushCacheBySelector` → `flushJITCaches` → SIGSEGV.
The sigsegv handler reports "PC not in any active JIT method
(evicted?)". Instruction bytes at crash PC decode as NEON `stp q0,
q0, [x11, #...]` — JIT-generated code that's either freed or
corrupt.

**Hypothesis:** `primitive 120` (flushCacheBySelector) fires during
method installation. The first JIT compilation happens, then image
installs a CompiledMethod which triggers prim 120 → `flushJITCaches()
→ codeZone_.firstMethod()` walks the just-compiled method whose
invariants may not yet be fully established (IC count vs code size).
Alternatively, the compiling code itself was clobbered between
compile finish and first invocation.

**Why this matters:** blocks using JIT for benchmarking via the eval
path. Full-image boot (non-eval) runs longer, but session-handler
forks at P79 haven't produced output either. Without JIT reliably
usable, we can't measure improvements.

**Next step:** add a compile-blacklist mechanism
(`PHARO_JIT_SKIP_SELECTORS=...`) so we can bisect which compilation
is the problem. Scope: small addition to `src/vm/jit/JITCompiler.cpp`
plus logging in `Interpreter::initializeJIT`.

## Why these are deferred, not fixed

All three would take substantial focused work (half-day to multi-day)
to resolve. During the 2026-04-14 test-widening session I chose breadth
(new batches to characterize unknown failure modes) over depth on these
three, which are already well-characterized. The tradeoff: we now know
that in 10+ batches totaling 9000+ tests, there are exactly zero
uncharacterized logic bugs left. These three are the only unresolved
items, and their scope is known.
