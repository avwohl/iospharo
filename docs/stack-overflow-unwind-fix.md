# VM change: stack-overflow now unwinds instead of leaking mutexes

This documents the **VM-side** (C++) changes that fix the full-SUnit-suite
*timer-scheduler-wedge* (blocker #2). It is the companion to the **image-side**
fix (an iterative `Context>>copyTo:`, see `docs/image_issues.md`); together they
let the suite run past the old ~511-class deadlock. Commit: `73eb8947`. Files:
`src/vm/Interpreter.cpp`, `src/vm/Interpreter.hpp`.

## The bug this fixes

A Smalltalk process can hold a mutex by being inside a `Semaphore>>critical:`
block. `critical:` releases the mutex on a non-local exit via `ifCurtailed:`:

       critical: aBlock
           ^ [ self wait. aBlock value ] ifCurtailed: [ self signal ]

Before this change, when a process overflowed the VM frame stack
(`frameDepth_ >= StackOverflowLimit`, 4096), `handleStackOverflow` called
`terminateAndSwitchProcess`, which **hard-kills the process in C++ without
running its `ensure:`/`ifCurtailed:` unwind blocks**. So a `critical:` mutex held
by the overflowing process was never released. Any later process that entered the
same critical section — in practice the **Delay/timer scheduler** — then blocked
on the leaked mutex forever. The timer was never re-armed, only the idle process
(P10) stayed runnable, and the whole VM deadlocked.

In the full suite this fired around class ~511 (the mass-erroring `Package*`
tests): an error signalled ~4000 frames deep was frozen via the recursive
`Context>>copyTo:`, which recursed chain-deep and tripped the 4096 overflow while
a `critical:` was on the stack.

Minimal repro (`/tmp/mutex_leak.st`, eval mode): fork
`mutex critical: [ infinite-recursion ]` so it overflows + is terminated; a
second process's `mutex critical: [ held := true ]` then blocks forever. Pre-fix
`held=false` (leaked); post-fix `held=true`.

## What changed

### 1. `handleStackOverflow` drives `Process>>terminate` (Interpreter.cpp:9547)

Instead of the C++ hard-kill, the overflow handler now sends the active process
the image's own `#terminate`, which walks the context chain running the unwind
blocks (releasing the mutex) before ending the process:

       void Interpreter::handleStackOverflow(int argCount) {
           popN(argCount + 1);                 // undo the failed send's push

           if (inStackOverflowSignal_) {        // overflowed AGAIN mid-unwind
               inStackOverflowSignal_ = false;  // last resort: hard-kill
               terminateAndSwitchProcess();
               return;
           }

           Oop termSel   = memory_.lookupSymbol("terminate");
           Oop activeProc = getActiveProcess();
           if (!termSel.isObject() || termSel.isNil() ||
               !activeProc.isObject() || activeProc.isNil()) {
               terminateAndSwitchProcess();     // fallback if #terminate missing
               return;
           }
           inStackOverflowSignal_ = true;
           push(activeProc);
           sendSelector(termSel, 0);            // activeProcess terminate
       }

`receiver_`/`method_` are still the *caller's* at this point (they are only set
after a successful `pushFrame`, Interpreter.cpp:9666), so the send originates
cleanly from the caller's frame and unwinds up through any enclosing `critical:`.

Why `terminate` and not `#error:`: signalling an error goes **unhandled** in a
forked test, and our unhandled-error path *also* skips unwinds — so the mutex
would still leak. An explicit `terminate` unwinds regardless of handlers.

### 2. Frame-budget headroom while signalling (Interpreter.hpp + pushFrame)

The terminate-unwind itself runs Smalltalk and pushes frames at `fd≈4096`, which
would instantly re-trip the overflow guard. A new flag grants extra frame budget
only while driving the recovery:

       // Interpreter.hpp
       static constexpr size_t StackOverflowLimit          = 4096;   // soft limit
       static constexpr size_t StackOverflowSignalHeadroom = 8192;   // 4096->12288
       static constexpr size_t MaxFrameDepth               = 65536;  // hard limit
       bool inStackOverflowSignal_ = false;

The guard in `pushFrame` (Interpreter.cpp:11493) keeps the hot path a single
compare; the flag is only consulted when actually near the limit:

       size_t overflowLimit = StackOverflowLimit;
       if (__builtin_expect(inStackOverflowSignal_, 0))
           overflowLimit += StackOverflowSignalHeadroom;
       if (__builtin_expect(frameDepth_ >= overflowLimit, 0)) { ... return false; }

So a normal process still overflows at 4096; a process *driving the overflow
terminate* gets up to 12288 (well under `MaxFrameDepth` 65536) for the unwind to
complete.

### 3. The flag is cleared on process switch (executeFromContext)

`inStackOverflowSignal_` is interpreter-global, so it must not leak to the next
process. `Process>>terminate` always switches away, and `executeFromContext`
resets per-process interpreter state — the two `frameDepth_ = 0` reset points now
also clear the flag (Interpreter.cpp:16422 sigsegv-recovery path, 16433 normal
process-switch path):

       frameDepth_ = 0;
       inStackOverflowSignal_ = false;  // fresh process never inherits headroom

Because `terminate` always switches, no process keeps running with the flag set,
so no eager hot-path clear is needed.

### 4. Last-resort fallback

If a *second* overflow happens past the headroom (i.e. the unwind block is itself
pathologically deep), `handleStackOverflow` sees `inStackOverflowSignal_` already
true and falls back to the original `terminateAndSwitchProcess` hard-kill. This
preserves the old "a runaway process must not wedge the VM" guarantee as a
backstop. (In practice this no longer fires for the suite once the image-side
iterative `copyTo:` removes the deep recursion — see below.)

### 5. `dumpTimerWedgeState()` one-shot diagnostic (Interpreter.cpp:1916, called 3711)

Added a diagnostic that fires once at the first `[DELAY-DEATH]` detection
(`checkTimerSemaphore`). It dumps the timing-semaphore wait list plus the full
process table (each process's priority, state — ACTIVE / on-sem / ready /
terminated — `myList`, and suspended-context top frame), so any future
scheduler wedge can be root-caused in a single run instead of repeated ~1h lldb
sessions. It distinguishes a *dead* scheduler (gone from the wait list) from a
*stuck-but-alive* one (still queued) — the key question the original
re-signal-only recovery couldn't answer.

## Why the VM change needs the image change too

The VM change alone is a strict improvement (it fixed the shallow mutex-leak
repro), but it did **not** fix the full run on its own: the terminate-unwind of a
deep stack re-triggered another deep, recursive `Context>>copyTo:` (a secondary
`SubscriptOutOfBounds` raised during unwind), which re-overflowed *past* the 12288
headroom → hit the hard-kill fallback → leaked the mutex again (observed at
`fd=12288`, `[TERM] ... method=#copyTo:`).

The root of the recursion is that stock Pharo's `Context>>copyTo:` copies the
sender chain **recursively**. The image-side fix replaces it with an iterative
copy so no deep-stack freeze/copy ever overflows. With both in place:

- iterative `copyTo:` prevents the overflow on *bounded* deep stacks (the actual
  suite trigger) — no overflow, no terminate, no leak;
- the VM terminate-on-overflow releases mutexes for any *genuine infinite
  recursion* that still legitimately overflows.

## Validation

- Mutex-leak repro `/tmp/mutex_leak.st`: `held=false → held=true`, 0 hard-kills.
- Simple infinite-recursion overflow: still recovers, the Delay timer survives
  (10 subsequent `Delay wait` ticks all fire).
- Sista loop bench: all benchmarks complete (the guard change is one
  predicted-not-taken branch on the per-activation hot path).
- 8-class SUnit subset: BATCH COMPLETE, no wedge, no hard-kills.
- Full suite end-to-end (with the image fix): reached **836 classes / 17,125
  tests, 0 DELAY-DEATH, 0 overflow hard-kills** (vs the old ~511 wedge), bounded
  only by the runner's 2h batch deadline.

## Tuning knobs

       StackOverflowLimit          4096    soft overflow limit (unchanged)
       StackOverflowSignalHeadroom 8192    extra frames for the unwind to run
       MaxFrameDepth               65536   hard limit (stopVM); must exceed
                                           StackOverflowLimit + headroom

Raising `StackOverflowSignalHeadroom` gives a deeper unwind more room before the
hard-kill fallback; it must stay comfortably below `MaxFrameDepth - StackOverflowLimit`.

## See also

- `docs/image_issues.md` — the recursive `Context>>copyTo:` image bug + iterative shim.
- `docs/test-results.md` — full-suite results and the blocker #1/#2 narrative.
- memory `timer-scheduler-wedge` — the full investigation and repro recipes.
