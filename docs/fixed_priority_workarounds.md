# Fixed-priority scheduling: workarounds and design notes

**Context:** Several hangs (A1 eval-DEFER=0 hang; earlier CollectionRootTest
"hang" before it was reclassified; various tight-loop cases) trace back
to processes that never get the CPU even though they're runnable.  This
prompts the question: is strict priority the root problem?

## Does Smalltalk require strict priority?

The language spec doesn't — but the image does in practice:

- **DelaySemaphoreScheduler** runs at P80 because it must react to timer
  interrupts quickly.  If it ran at Morphic's P40, animations would
  jitter whenever Morphic is busy.
- **Morphic event handlers** run at P60 so a click preempts a P40
  background job.  Users expect immediate response.
- **Critical sections** use `Semaphore>>critical:` with tight `whileTrue:`
  loops between sends, relying on "same-priority processes only yield
  at sends."  Some image code depends on "nothing at my priority will
  preempt me mid-operation."
- **~30 tests** in `ProcessTest` / `DelayTest` / `SemaphoreTest` verify
  strict-priority behavior explicitly — changing it flips those red.

So strict priority is *convention* not a hard *requirement*, but the
convention is widely depended on.

## Are our bugs actually caused by strict priority?

Mostly **no** — they're wake-up issues masquerading as priority issues:

- **A1 hang**: startup process (P80, oop 0x3033e60f0) gets suspended and
  never wakes.  `PHARO_XFER_TRACE=1` across 6146 process switches shows
  startup never appears in any `[XFER]` event after its initial P79→P80
  boost.  Morphic (P40) has the floor by default because the runnable
  queue has nothing else.  This is a **deadlock**, not starvation.

- **CollectionRootTest "hang"**: turned out not to be a priority issue at
  all — `FFIArchitecture forCurrentArchitecture` is uncached, called
  millions of times from FreeType glyph FFI, interpreter at ~1.6M
  steps/sec.  Just slow, not stuck.

- **Pattern**: when we see "hang," the CPU is running productively
  somewhere (DIAG-QUEUE shows only P10 idle suspended); the victim
  process is blocked on a semaphore that never signals.

Proportional scheduling would *mask* A1 by giving startup some CPU
eventually, but wouldn't fix why it's suspended.  Root cause needs
direct investigation.

## Three options, ranked by risk

### Option A — anti-starvation with strict priority (low risk)

Track per-process "consecutive runtime without yielding to any other
runnable process."  If process X at priority P runs continuously for
>N ms (say 50ms) *and* there is any runnable process at any priority,
force a yield.

Rough equivalent of Linux's old O(1) scheduler with active vs expired
arrays.  Preserves the common-case strict-priority semantic but
recovers from pathological starvation.

Estimated size: 30–80 lines in `step()` and `transferTo()`.

### Option B — proportional within priority level, strict across (medium risk)

Priority levels are strict (P80 > P40 always), but within a level we
already do round-robin via the 2ms `forceYield`.  Extend `forceYield`
to also wake suspended-but-ready processes at the current priority
after N ms.

Most strict-priority tests are across priority levels, so they'd still
pass.  Same-priority tests would need review.

Estimated size: ~150 lines including test fallout.

### Option C — full CPU-allocation scheduling (high risk)

Map priority → weight (e.g. `weight = 2^((P−10)/10)`), distribute CPU
proportionally.  Breaks semantics assumed by image code and tests.
Long project; not recommended unless a compelling reason appears.

## Recommendation

1. **Fix the deadlock first.**  Add a "dump all suspended processes +
   their wait-reason" diagnostic at hang time.  Locate why the startup
   process is suspended without anyone to wake it.  If it's waiting on
   a semaphore whose signaler is trapped in Morphic, that's a concrete
   targetable bug.

2. **Then Option A as a safety net.**  Cheap to implement, preserves
   semantics, unblocks the specific hang, and gives us a margin for
   future starvation bugs.

3. **Skip Option C** unless a compelling reason appears.  Current bugs
   don't need that hammer.

## Status

2026-04-23: Root cause identified and first fix landed (commit `ac350c3`).

**Finding**: `PHARO_PROC_DUMP=1` diagnostic (added same commit) enumerated
every Process in the heap at the hang point.  One stood out:

    [PROC-DUMP] P40 proc=0x3033c39c8 on-sem list=0x3033cb560(Semaphore)
                top=SessionManager>>snapshot:andQuit:

Pharo's `SessionManager >> snapshot:andQuit:` forks a P79 worker to do
the save/quit and the caller does `wait wait` on a freshly-created
Semaphore.  The worker should eventually signal `wait`, but in our
headless-eval + eager-JIT path the worker's tail never reaches the
signal — startup handlers take over first, leaving the caller
suspended forever.  Classic fork/wait deadlock.

**Fix**: `Interpreter::unblockStuckSnapshotCallers()` walks all Process
instances and signals the Semaphore of any whose top frame is
`snapshot:andQuit:`.  Called at resume time and every 10s DIAG cycle.
Idempotent via a per-Oop signaled-set.

**Measured impact**: `PHARO_JIT_DEFER=0 eval "42 printString"` was a
hang before; now it proceeds from ~229 compiled methods to ~520 and
exposes a *different* pre-existing crash (JIT code-zone eviction while
compiling exception infra).  That second crash is the next layer of
the onion, out of scope here.

Default `DEFER=4s` path still works cleanly — 0 regressions on FFI
test suite (35 tests across FFICalloutTest, FFIExternalStructureTest,
AthensCairoMatrixTest).

**Open questions**: Is there a more principled fix than signaling
from C++?  Options:

- Patch `snapshot:andQuit:` in the image so the wait has a timeout —
  cleaner but requires a startup-script image mutation.
- Demote the forked worker from P79 to P10 after snapshot completes
  — matches the existing startup-process P40→P10 demotion pattern,
  would let the wait-loop's natural flow unblock.
- Stay with the current C++ signal — it's a scheduler-level escape
  hatch, cheap, and only affects a very narrow context (the exact
  method name `snapshot:andQuit:`).

Option A is next up if the C++ approach turns out to mask other bugs.
