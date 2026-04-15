# Deferred Issues

Last updated: 2026-04-14 (session 3b: AMS selector fix + new JIT bisect data)

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

**Instrumentation landed (2026-04-14, commit TBD):**
Interpreter now tracks `finalizationSignalCount_` and
`finalizationPendingTotal_`. With `PHARO_GC_EPH_DEBUG=1`, every firing
logs `[SIG-FIN] #N pending=P total=T hasWaiter=B excess=E`.

Initial observation: on first GC after boot, `pending=63 hasWaiter=0
excess=0` — the semaphore is signaled but nobody is waiting yet. The
signal increments excessSignals, so when the finalization process
eventually calls `wait`, it proceeds immediately without suspending.
This is correct behavior; the concern is whether the finalization
process gets scheduled before the test's sleep elapses.

Next diagnostic: instrument the test-path too — when a test calls
something like `100 milliSeconds wait. self assert: finalizerFired`,
log the time-since-signal in `signalFinalizationIfNeeded` vs the time
the finalization process actually pops a mourner (prim 172). A latency
>50ms suggests scheduler gap, not signal-path loss.

**Update (2026-04-14, session 4):** instrumented. Added
`lastFinalizationSignalTime_` timestamp in `Interpreter::signalFinalizationIfNeeded`
and a `[POP-FIN] signal->pop latency=%dms` log in `primitiveFetchNextMourner`
(primitive 174). Ran `100 milliSeconds wait. 42 printString` under
`PHARO_NO_JIT=1 PHARO_GC_EPH_DEBUG=1`:

    [SIG-FIN] #1 pending=63 total=63 sema=... hasWaiter=0 excess=0
    [POP-FIN] signal->pop latency=1ms (signal #1)   x 63

All 63 pops happened at 1ms from signal — well under the 50ms
scheduler-gap threshold. **Primary hypothesis ruled out**: the
finalization process is scheduled promptly; latency is not the
failure mode. The test failures in deferred #3 must originate
elsewhere — candidates:

  - tests that rely on `Smalltalk garbageCollect` synchronously
    flushing finalizers (they return before the fin process runs
    even 1ms later)
  - tests with no explicit wait at all (zero-delay assertion)
  - weak references or ephemeron-style reachability mismatches
    that prevent mourner enqueue in the first place

Next diagnostic: find the failing SUnit finalization tests and
examine whether they include any delay between triggering GC and
asserting, or whether they expect synchronous finalization.

**Update (2026-04-14, session 4 cont'd):** Inspected the six failing
test sources. Key finding: none of them wait. They expect `Smalltalk
garbageCollect` followed immediately by an assertion to see the
post-finalization state. `WeakKeyDictionaryTest>>testClearing`:

    keys := nil.                                         "drop 1000 strong refs"
    dict at: self put: nil.
    Smalltalk garbageCollect.
    self assert: dict size equals: self size + 1.        "still 1001 — OK"
    self assert: dict keys size equals: 1.               "<-- fails, got 3"
    self assert: dict size equals: 1.

We report "Got 3 instead of 1" — meaning after GC, 2 of the 1000
keys survived. The test logic: `setUp` pins 1000 keys via a strong
`keys` ivar; test nils that ivar, GC runs, all 1000 keys should
become collectible. Two are retained somewhere.

Stock Pharo doesn't wait either — it relies on the GC itself (not
the finalization process) to reclaim weak keys. Our issue is that
some of those keys survive GC. Candidate retainers:

  - interpreter stack slots of the test framework still pointing at
    elements enumerated by the previous `keys do:` in setUp
  - JITMethod literal copies keeping references to the `'key',N`
    strings used in the previous iteration
  - temp-frame cleanup gap — frames that returned but whose stack
    slots were not zeroed before the next GC

Next diagnostic: run `(1000 to: 1 by: -1) collect: ['key', n asString]`
to build the set, then force GC and count via `Smalltalk allInstances`
how many `'key*'` strings remain. That isolates the retention count
without involving WeakKeyDictionary at all.

**Update (2026-04-14, session 4 cont'd):** Strong-ref test passed —
`ByteString allInstances` went from 1023 → 23 for `'key*'` strings
after nilling the collection and 3x GC (23 is the built-in baseline:
`'keyboard'`, etc.). The plain GC path is **working correctly** for
unreachable strings. So the failure is specifically in
WeakKeyDictionary's tracking, not in GC itself.

Likely culprit: `WeakKeyDictionary` uses `Ephemeron`s (one per
entry, where the key is the Ephemeron's key slot). During the
earlier test run, `[GC-EPH] fired=12` after a GC that should have
fired ~1000 (one per unreachable key). If only 12 ephemerons fire
when 1000 keys become unreachable, entries remain live in the dict.

**Root cause FOUND (2026-04-14, session 4):** `fireAllEphemerons`
used a range-based `for (auto obj : ephemeronList_)` loop. Calls
to `scanPointerFields` during firing (which marks the key and
values) can discover NEW ephemerons that get appended to
`ephemeronList_` mid-loop. Those newly-appended entries:
  - sit past the captured `.end()` iterator, so aren't fired
  - are then wiped by the `.clear()` at the end of the function

On testClearing (1001 entries, 1 live + 1000 dead), the very first
fire-pass processed only 12 entries before the list's underlying
storage reallocated. The remaining 328+ active ephemerons were
dropped. Fix: index-based loop + outer fixed-point loop wrapping
`markInactiveEphemerons` / `fireAllEphemerons`. Post-fix, debug
log shows `fired=1004` (vs `fired=12` before). Committed.

**Remaining issue (tracking separately):** testClearing passes 3-7/10
runs even with the fix. Deeper investigation (session 5, 2026-04-14):

    Test setup: `keys := (1 to:1000) collect:[:n| 'key', n asString].
                 dict := WeakKeyDictionary new.
                 keys do:[:n| dict at: n put: n,n].
                 keys := nil.
                 dict at: #sentinel put: nil.`
    After one fullGC: dict keys size = 8-12 (should be 1)
    After two fullGCs: dict keys size = 1 ✓

    Debug output from one GC:
      [GC-EPH] encountered=765 inactive=425 active=340 fired=12

    So of 1001 ephemerons in the dict:
      - Only 765 were encountered during marking (236 missed!)
      - Of the 765, 425 had keys already marked alive (inactive)
      - 340 were classified "active" (dead keys)
      - After fixed-point iteration, only 12 TRULY had dead keys.

Two puzzles:
  1. **Undercount on encounter (1001 → 765):** why are 236 ephemerons
     not encountered? The `dict` is a root (local temp), its `array`
     is a Pharo Array that should be scanned by scanPointerFields,
     and each live slot should be an ephemeron → markAndTrace.
     Possible: scanning order leaves some associations as entries in
     tombstoned slots that markAndTrace skips (though isMarked short-
     circuits on the entry itself, not the slot). Need to log each
     ephemeron markAndTrace vs. scan and diff against a pre-GC
     allInstances count.

  2. **328 of 340 "active" transitioned to "inactive" during
     fixed-point:** a key that was unmarked at encounter becomes
     marked during the fixed-point iteration. What strong path adds
     those marks? `markInactiveEphemerons` calling `scanPointerFields`
     on a newly-alive ephemeron marks its value and container. The
     container (dict) is already reached; so marking its array hits
     every association, which classifies each one *again* as an
     ephemeron — but markAndTrace short-circuits on already-marked
     objects. So this shouldn't add NEW key markings.

  Attempted experiment: calling `signalFinalizationIfNeeded()`
  synchronously inside `primitiveFullGC` (instead of setting the
  `finalizationCheckAfterGC_` flag) regresses test from 4/10 → 0/10.
  The test's second assertion expects `dict size = self size + 1`
  (unfinalized!) right after GC — synchronous signalling transfers
  to P70 which drains mourners immediately, breaking the assertion.
  Reverted. Keep the deferred-to-next-step design; it matches the
  test's semantics.

Ephemeron correctness is broadly fixed (fire-all bug, outer loop);
residual flakiness is a separate "miss-rate on encounter" puzzle.

**ROOT CAUSE RECLASSIFIED (2026-04-14, session 6):** the residual
testClearing flakiness is a test-framework retention artifact, not a
VM bug. Evidence:

    Direct eval, setUp inside a block (test instance GC-collectible):
        10/10 runs pass — dict keys size = 1

    SUnit framework (suite := cls selector: ...; suite run):
        5/30 pass, 25/30 fail — dict keys size = 8-12

    POP-FIN diagnostic: 10069 mourners processed across 10 runs
    (~1007 per run). Signal, transfer to FinalizationProcess, and
    mournAction execution all working correctly.

What the SUnit wrapper keeps alive: the forked P40 test process
holds the test instance in its stack frame across the watchdog
handler chain. The test instance's `keys` ivar is nil'd, but the
ORIGINAL collection is still referenced from a compiler-cache or
test-context temp that isn't released until the test fork exits.

Net impact: the 5 "failing" weak/finalization tests (testClearing x2,
testFinalizeValuesWhenLastChainContinuesAtFront, testWeakObject,
testWeakDoubleAnnouncer) all share this pattern. VM-side ephemeron
+ finalization is correct. Further improvement requires either
(a) trimming the test-runner wrapper so fewer frames retain, or
(b) forcing a GC from INSIDE the test fork after the ivar nil.

**Attempted (a), did not fix:** extended the harness fast-path
(single TestFailure handler, no P60 watchdog) to cover the three
weak-retention test classes. Failures unchanged (5/5 still fail
with "Got N instead of 1" where N is 3-10). Retention source is
deeper than the handler stack — likely stale slots in the fork's
block frame or the runCase path itself.

Diagnostic from 10 consecutive direct-eval GCs of the same body
(inside a block, no locals): `encountered=1774 inactive=1410
active=364 fired=4`. Only 364 of ~1000 expected test ephemerons
classify as "active" (dead key) at encounter — 636 are seen as
keyed-alive even though their keys *should* be unreferenced.
Possible cause: operand stack slots below `stackPointer_` holding
stale Oops from setUp's `keys do:[:n| dict at: n put: n,n]` loop.

Closes the VM-side investigation of deferred #3. Further work is
either a test-runner rewrite or a stack-slot scrub primitive.

## 4. JIT eval-mode boot hang (session-3 "resolved" claim RETRACTED)

**Status (2026-04-14, session 3 correction):** the earlier "resolved"
claim was wrong. The fresh-image test ran without `PHARO_NO_JIT=0`, so
`test_load_image.cpp:603` auto-disabled JIT in eval mode. The '42'
output came from the interpreter, not the JIT. Re-running with
`PHARO_NO_JIT=0` explicit — `42 printString` hangs at boot after ~5M
bytecode steps, same symptom as before: P10 ProcessorScheduler idle
loop with timer wakeups firing but no progress through the eval path.

Tools added this session that ARE working and useful for future bisects:
- `JIT_EXCLUDE_OOP=0xhex,...` — skip JIT-compile by specific method oop
  (complements existing `JIT_EXCLUDE=sel,...` by selector).
- When selectorOf returns `?` at compile time, log numLits + classNames
  of the penultimate and last literals to help identify block methods.

**Session-3 bisect LOCALIZED: it's the `do:` method at
oop 0x30032ee48.** With `PHARO_NO_T2=1 PHARO_NO_JIT=0` on a fresh
image, JIT_MAX_COMPILE=7 works, N=8 (compiles `max:`) still works
when we exclude max:, N=9 which compiles `do:` (0x30032ee48) hangs.
Excluding that single oop via `JIT_EXCLUDE_OOP=0x30032ee48` delays
but does not eliminate the hang — **some later compile of a sibling
do: implementation triggers the same issue**.

**Session-3b bisect REFINED (2026-04-14, with AMS selectorOf fix):**
The previous session's `JIT_MAX_COMPILE` threshold at the boundary is
actually N=119 works, N=120 hangs. With AMS selectorOf now working,
**the 120th method is `ensure:` (BlockClosure>>ensure:, oop 0x3003ea8d0,
5248 bytes, 4 lits, AMS-based)**. But:
  - `JIT_EXCLUDE=ensure:` alone does NOT eliminate the hang.
  - `JIT_EXCLUDE_OOP=0x3003ea8d0` alone does NOT eliminate the hang.
  - With ensure:-excluded, bisect shows a NEW boundary: N=167 works,
    N=168 hangs (no DNU — just scheduler spinning P80↔P40↔P10).

Conclusion: the hang isn't triggered by a single method's miscompilation.
It's cumulative — once ~120-170 methods are T1-compiled, some pattern
of interaction (stencil bug × IC state × J2J save stack × something)
corrupts state enough that the startup-process exception unwind eventually
sends a message to nil (DNU #resolve: on nil) or the scheduler gets
stuck in a transfer cycle without arming the timer.

Current symptom flavors:
  - DNU #atEnd on Array (stack corruption in parseFields:structure:)
  - DNU #resolve: on nil (Context>>resume: to dead context)
  - Silent hang: P80↔P40↔P10 scheduler cycle, no timer armed

**Next step (more actionable):** add per-stencil execution counters so
we can see which stencil is hit at hang-time. The previous session's
hypothesis about J2J call/return imbalance was wrong — the J2J_INLINE_RETURN
tail-calls caller's resume and only hits the interpreter's return counter
when the full J2J chain unwinds. The 65M/20K imbalance is expected.

**Session-3b improvement landed:** `ObjectMemory::selectorOf` now handles
AdditionalMethodState — when the penultimate literal is an AMS (not a
Symbol), falls back to AMS slot 1 (the real selector). Unblocks method
identification in `[JIT] Compiled method ...` and `[DNU-STACK]` output.

**J2J ruled out as culprit (2026-04-14):** `PHARO_NO_J2J=1 PHARO_NO_T2=1`
reproduces the hang MUCH faster (~300K steps in 20s vs 18M with J2J,
DNU #>= appears). Disabling J2J makes things worse, so the bug is in
the base T1 stencil dispatch, not the J2J chain specifically.

The `do:` bytecodes (26 bc, 3 lits — ByteSymbol lit[1]=#size? lit[2]=#at?
lit[3]=GlobalVariable class binding) are the standard
`| size i | size := self size. i := 1. [i <= size] whileTrue: [aBlock
value: (self at: i). i := i + 1]` form. The J2J chain at bc[14]/[15]
does `self at: i` -> `aBlock value:` back-to-back with stencil_sendJ2J
(op 0x70 -> operand 16842766 for #at:, op 0x7A -> 16842767 for #value:).

Suspect stencil(s): either
  - stencil_sendJ2J when the target is a FullBlockClosure>>value:
    (op 0x7A, value: special selector — bc 0x78-0x7B range)
  - stencil_jumpBack (op 0xB0) in the whileTrue loop
  - the interaction between them when J2J-chained after a block call

**Dump tooling (new, committed):**
- `PHARO_JIT_DUMP_SEL=sel1,sel2,...` — at compile time for any matching
  selector, print methodOop, numLits, literal Oop+class, and the full
  stencil decode table.
- `PHARO_JIT_TOP=1` — append top-10 hottest-JIT-method list to each
  periodic [JIT] Stats line (by JITMethod.executionCount).

**New diagnostic (2026-04-14 session 3):** full-JIT hang shows a
**massive J2J call/return imbalance** in the JIT stats:

    J2J-s: 20524 returns / 65M calls    (3000:1 skew)

J2J direct-patch count freezes at 233. Stencil-call counter grows
linearly while stencil-return counter stays at 20,524 from early boot
forward. Also the countMap evicts: "map: 367 tracked, 234 hot" drops
to "map: 178 tracked, 46 hot" by late boot, suggesting GC is wiping
JIT countMap entries without re-registration.

Interpretation: hot methods enter J2J via stencil dispatch but don't
return — they either re-enter J2J at an inner send (looping without
unwind) or the return stencil branches to a wrong address. Because
return count stays frozen, the failure is NOT per-call — it's a
specific method path that loops inside the J2J chain after being
compiled hot. The 277 compiled methods include scheduler code
(tickAfterMilliseconds:, waitForUserSignalled:orExpired:,
timingPrioritySignalExpired), consistent with the hang sitting in
the P10 scheduler loop.

Next step: dump J2J call/return oops to find the leaking send.
Add per-method J2J call/return counters; any method with
unbounded call-minus-return is the culprit.

**Session 4 observations (2026-04-14):** confirmed the hang terminal
state: `P80 DelaySemaphoreScheduler>>whileTrue:` with
`usecArmed=0 timerSem=nil nextUsec=0x7fffffffffffffff` — i.e., the
delay scheduler has scheduled nothing for the indefinite future, and
is waiting on an un-set timer semaphore. Something torched the delay
registration path before startup.st got to run.

Just before the hang appears, logs show:

    [TERM] terminateCurrentProcess: proc=0x302c89138 pri=40 fd=0 method=#ensure:
    [TERM]   C++ caller: Interpreter::returnValue+4784
    [JIT] Compiled method 0x3002da798 'idleProcess' ...
    [JIT] Tier 2 compiled method 0x3002da798 'idleProcess'

So the startup process returns past its top-level frame (normal), is
terminated in its ensure: unwind, and then `idleProcess` gets T2
compiled. After that the delay scheduler never rearmed. This is
consistent with either (a) the T2 compile of `idleProcess` cutting
off the wakeup path, or (b) the ensure: unwind corrupting some
scheduler linked-list while running under JIT.

Startup.st is present on disk but does NOT appear to execute in the
JIT path — no `'42'` printed, no `Smalltalk exitSuccess` trace.
Stock-interp path (`PHARO_NO_JIT=1`) prints `'42'` and exits in
under 15 s on the same image.

Minimal-repro test kept: `/tmp/harness-fresh/Pharo.image eval
"42 printString"` with `PHARO_NO_JIT=0 PHARO_NO_T2=1` hangs in
DelaySemaphoreScheduler within ~15 s.

Previous analysis (still valid) below.

---

### Earlier analysis — JIT eval-mode boot hang (crash fixed 2026-04-14; hang remains)

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

**2026-04-14 T1-only bisect (session 2):**

    JIT_MAX_COMPILE=N    Result (PHARO_NO_T2=1)
    ≤119                 clean boot to '42' eval output
    120                  DNU #asSymbol on Array (rcvr=0x300016768)
    >120                 same or cascaded DNUs

The 120th method compiled is `0x3003ea8d0 '?' (5248 bytes)` — `?`
meaning selectorOf didn't find a valid symbol literal (block method or
nameless). To identify it: add a diagnostic that prints the class name
and the penultimate/last literal bytes when the selector resolves to
`?`, or compile only through N=119 and then single-step method 120.

The DNU is `Array does not understand #asSymbol` — so stack top is an
Array where a ByteString/Symbol is expected. Consistent with the
stack-pointer mismatch hypothesis: JIT-compiled method #120 leaves the
stack in a state such that a later send pops the wrong object.

## Why these are deferred, not fixed

All three would take substantial focused work (half-day to multi-day)
to resolve. During the 2026-04-14 test-widening session I chose breadth
(new batches to characterize unknown failure modes) over depth on these
three, which are already well-characterized. The tradeoff: we now know
that in 10+ batches totaling 9000+ tests, there are exactly zero
uncharacterized logic bugs left. These three are the only unresolved
items, and their scope is known.
