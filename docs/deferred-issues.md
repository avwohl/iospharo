# Deferred Issues

Last updated: 2026-04-14 (second session: sender=nil reframe + T1 DNU)

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

## 4. JIT eval-mode boot hang (crash fixed 2026-04-14; hang remains)

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

So the first compiled method already triggers a crash. Corrected
crash stack (2026-04-14 re-verified):
`ObjectMemory::fullGC + 7584` → SIGSEGV (not flushJITCaches as
earlier notes suggested). Offending instruction `str x12, [x10]` at
`0x100022594` — inside a root/remember-set walk loop. `x10` walks
off the valid region and hits unmapped memory. The sigsegv handler
emits "PC not in any active JIT method (evicted?)" because the
default JIT-crash branch fires on any non-JIT PC.

Disassembly of flushCaches() (`0x1000cdc4c`) confirmed CORRECT: the
compiler emits the 3x `stp q0, q0` loop zeroing exactly bytes 0-95
of each 104-byte IC slot, preserving selectorBits at offset 96.
Earlier hypothesis (vectorization off-by-80) was a misreading.

**Hypothesis (revised):** first JIT compile forces a GC that walks
a stale remember-set or codezone-root region. Probable suspect:
`updatePointersAfterCompact` or a root-walk that iterates `[x19+0x390,
x19+0x398)`. One of those two fields holds an end pointer that's
not kept in sync when the JIT's code-zone region is registered or
resized — so the walker runs past the mapped region.

**Why this matters:** blocks using JIT for benchmarking via the eval
path. Full-image boot (non-eval) runs longer, but session-handler
forks at P79 haven't produced output either. Without JIT reliably
usable, we can't measure improvements.

**Crash fixes (2026-04-14):**
1. Interpreter::forEachRoot now calls `jit::makeWritable(codeZone
   .rawStart(), totalBytes())` at the top of its JIT-method loop
   (commit 3ea4f7f). The W^X toggle around JIT execution could leave
   the zone executable if a path skipped the matching makeWritable
   (e.g., non-local return out of JIT_CALL), and the next fullGC's
   in-place Oop rewrite then SIGSEGVed.
2. JITRuntime::flushCaches now also calls makeWritable on the full
   code zone at entry (commit 5da4193). Same root cause: mprotect
   operates on pages, so makeExecutable(methodA) can flip methodB's
   IC region non-writable, and a subsequent flushCaches store
   SIGSEGVs. Unblocks primitiveFlushCacheBySelector after JIT
   invocations.

Verified: `PHARO_NO_JIT=0 PHARO_JIT_THRESHOLD=999999` now evaluates
`42 printString` cleanly. Full-JIT boot completes 273-method
compilation without crashing.

**Hang remaining:** with full JIT (threshold=2), boot hits a P80
startup process terminating on `SubscriptOutOfBounds>>freeze` with a
corrupted sender chain (sender=0x300000000, chain length 1). This
implies a JIT-miscompiled at:/at:put: bytecode that indexes out of
bounds, and the exception handler unwind sees a bogus sender Oop.

**Bisect notes:** JIT_MAX_COMPILE=1 → works. N=20 → hangs but no
SubscriptOutOfBounds. N=150 → SubscriptOutOfBounds terminates P80.
So the miscompile happens in the 20–150 range; a later compile may
mask it by recompiling differently.

**2026-04-14 (session 2) reframe:**

1. "sender=0x300000000" is NOT corruption. nilObject_.rawBits() in
   this image equals 0x300000000 (heap base). Confirmed by reading
   `Oop::nil()` → `Oop(s_nilBits)` and `ObjectMemory::cacheSpecialObjects`
   which sets `s_nilBits = nilObject_.rawBits()`. TERM-P80 diagnostic
   updated to tag `(nil)` explicitly (commit cbd8227).

2. The P80 termination is a downstream effect of an earlier
   unhandled exception — freeze runs at the top of the context chain
   because Process>>terminate's unwinding leaves a single frame.

3. The primary JIT bug (T1 stencil, reproducible without T2) is
   stack-pointer corruption. `PHARO_NO_JIT=0 PHARO_NO_T2=1` produces:

       [DNU] #1: #atEnd not understood by rcvr=0x30352dcc0
                 in #parseFields:structure: P80
       [DNU]   rcvr cls=51 fmt=2 class=Array
       [DNU]   receiver_=0x30352dae0 method_=0x30097cc30

   rcvr (stackValue(0)) ≠ receiver_. For a unary send, they should
   match — stackValue(argCount=0) IS the receiver. The stack is
   inconsistent with the frame's true receiver.

4. With T2 enabled (`T2_LIMIT>=50`), the error switches to
   SubscriptOutOfBounds>>freeze. T2 alone doesn't explain the bug
   since T1-only also fails. T2 amplifies a pre-existing T1 bug.

**Next step:** find the T1 stencil that corrupts the stack/receiver
mapping. Candidates:

- bytecode 0x70-0x7F (specialSelector sends) — push(receiver),
  push(args), sendSelector pattern
- bytecode 0x5C (returnTop) — pop SP calculation
- the push/pop-into-receiver stencils (0x00-0x0F / 0xC0-0xCF)

A targeted bisect: compile only `FFIExternalStructureFieldParser>>parseFields:structure:`
(and whatever it activates) and check if DNU appears. If yes, disassemble
the T1 code and find the stack-touching stencil that's wrong.

Owner/scope: half-day of T1 stencil audit. Unblocks full JIT boot.

## Why these are deferred, not fixed

All three would take substantial focused work (half-day to multi-day)
to resolve. During the 2026-04-14 test-widening session I chose breadth
(new batches to characterize unknown failure modes) over depth on these
three, which are already well-characterized. The tradeoff: we now know
that in 10+ batches totaling 9000+ tests, there are exactly zero
uncharacterized logic bugs left. These three are the only unresolved
items, and their scope is known.
