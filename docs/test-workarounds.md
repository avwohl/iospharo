# Test Runner Workarounds to Review

These are workarounds in `scripts/pharo-headless-test/run_sunit_tests.st` and
related scripts. Each has been investigated to determine if it's a legitimate
adaptation or a workaround for a fixable bug.

## 1. UndefinedObject >> findNextHandlerContext  (line 12)

Adds `findNextHandlerContext` to UndefinedObject so exception handler chain
traversal terminates at nil instead of hitting DNU.

Status: SAFETY NET — probably no longer needed.
- Primitive 197 (primitiveFindHandlerContext) is registered and works correctly.
  It walks the context chain in C++ and returns nil at the end, never sending
  messages to nil.
- Fresh image startup produces zero DNU messages without this method.
- The test runner may trigger edge cases where prim 197 fails and the Smalltalk
  fallback runs, eventually reaching nil. Keep the method as defensive code
  until we can verify it's truly unnecessary under test runner stress.
- The method is harmless (just `^ nil`) and matches what a correct Smalltalk
  image would have if Cog's C shortcuts weren't assumed.

## 2. relinquishProcessorForMicroseconds: instead of Processor yield  (line 733)

Uses `ProcessorScheduler relinquishProcessorForMicroseconds: 50000` instead of
`Processor yield` to avoid CPU-spinning when no other processes at same priority.

Status: FIXED (partially) + LEGITIMATE adaptation.
- Added early-exit check to primitiveYield: if the priority queue is empty,
  return immediately without modifying scheduler state. Matches Cog VM behavior.
- However, yield is a Smalltalk-level process switch, NOT a CPU idle.
  Even with the fix, a tight yield loop will busy-wait. Using
  relinquishProcessorForMicroseconds: for actual CPU idle is correct behavior,
  not a workaround.

## 3. 100+ skipped test classes  (lines 449-562)

Over 100 test classes are skipped. Categories:

Legitimate (missing features):
- TFFI callback tests — no callback support yet
- Cairo/Athens FFI tests — native libs not available
- Network socket tests — blocking primitives
- Epicea file watcher tests — no inotify/kqueue support

Needs investigation:
- ProcessTest (kills Delay scheduler) — may indicate scheduler robustness issue
- Filesystem persistence tests (timeout) — may be real bugs
- Tests that modify traits/classes (corrupt state) — may be GC/become: issue

Status: REVIEW PERIODICALLY as VM matures.

## 4. Sort infinite loop detection  (line 759)

Adds comparison counter to test selector sorting, errors after 200K comparisons.

Status: DEFENSIVE CODE — keep as is.
- No evidence of this ever triggering in practice.
- Symbol/String comparison primitives implement correct transitive ordering.
- The Smalltalk fallback (String>><=) is also transitive.
- This is good defensive programming for a test runner, not a workaround.

## 5. Morph>>activate nil submorphs patch  (setup_fake_gui.st:50)

Patches `Morph>>activate` and `passivate` to skip nil submorphs.

Status: LEGITIMATE — image-side headless initialization issue.
- NOT a GC bug. GC never nils live references (verified by code review).
- In headless mode, morphs can be created before the layout engine fully
  initializes submorphs arrays. Nil entries are pre-allocated padding.
- Standard Pharo doesn't need this because interactive mode initializes
  layout before any morph activation.
- The patch correctly adapts to the headless reality.

## 6. Delay scheduler health checks  (lines 732-749, 854-883)

Before/after each test class, checks if Delay scheduler is responsive.

Status: DEFENSIVE — keep for diagnostics.
- ProcessTest (skipped) is known to kill the Delay scheduler.
- Other tests may also corrupt scheduling state.
- The health check doesn't hide bugs — it detects and reports them.

## 7. TestExecutionEnvironment manual reset on timeout  (lines 991-997)

When watchdog kills a test, manually resets TestExecutionEnvironment state.

Status: LEGITIMATE — test framework cleanup.
- Standard Pharo's test framework expects cooperative test completion.
- When a test is forcibly terminated by watchdog, internal state needs cleanup.
- This is standard test runner behavior, not a VM workaround.

## 8. Session exit fallback  (lines 715-718)

Calls `Smalltalk quitPrimitive` directly because exitSuccess may not reach it.

Status: NEEDS INVESTIGATION
- May indicate a session shutdown bug in our VM.
- exitSuccess goes through SessionManager which runs shutdown handlers.
- If a shutdown handler hangs, primitiveQuit is never reached.

## 9. Symbol table corruption detection  (lines 619-632)

Checks if Symbol class itself has been added to symbol tables.

Status: PROBABLY OBSOLETE after NLR fix.
- This was added during the period when intern: returned Symbol class instead
  of the interned symbol (the NLR-through-ensure: bug).
- With the NLR fix, intern: should always return the correct symbol.
- Keep for safety until verified over many test runs.

## 10. Nested watchdog for blocking primitives  (lines 964-974)

Uses `doneSem waitTimeoutSeconds:` as safety net for blocking C++ primitives.

Status: LEGITIMATE — robust test runner design.
- Some primitives (file I/O, network) can block indefinitely.
- A nested timeout prevents the entire test suite from hanging.
