# VM Timeout Review

All hardcoded timeout values in the C++ VM code, for review.
The "5x monitor stall" timeout (300s in test_load_image.cpp) is excluded
per request — it's simply 5x the 60-second base.

## Process-Level Timeouts

    Value       Location                            Purpose
    -------     ----------------------------------  -----------------------------------------------
    600,000 ms  Interpreter.cpp:1268, :2083         Wall-clock stuck process detection (prio < 79).
    (10 min)                                        Terminates process if it runs 10 min without
                                                    yielding. Was 90s, raised for test suite.

    ~15 sec     Interpreter.cpp:1739-1744           Heartbeat watchdog: checks every 5000 ticks
    (5s × 3)                                        (5 sec), terminates after 3 consecutive checks
                                                    with no progress.

    2,000,000   Interpreter.cpp:3475                CannotReturn deadline (bytecode steps, ~2 sec).
    steps                                           Prevents cannotReturn error handler from
                                                    monopolizing CPU at P80.

## Heartbeat / Scheduling

    Value       Location                            Purpose
    -------     ----------------------------------  -----------------------------------------------
    1 ms        Interpreter.cpp:1704                Heartbeat thread sleep interval (master clock).

    2 ms        Interpreter.cpp:1734                Force yield / preemption interval (every 2 ticks).

    33 ms       Interpreter.cpp:1714                Display sync at ~30 fps.

    65,536      Interpreter.cpp:1245                Infrequent check interval (stuck process,
    bytecodes                                       watchdog priority update).

    102,400     Interpreter.cpp:1292                Input event processing, CFRunLoop pump (macOS).
    bytecodes

## Callback / FFI

    Value       Location                            Purpose
    -------     ----------------------------------  -----------------------------------------------
    10,000,000  Interpreter.cpp:1885                Callback invocation timeout (bytecode steps,
    steps                                           ~1 sec). Abandons callback if primitiveCallbackReturn
                                                    never fires.

    500 steps   Interpreter.cpp:1937                Cooldown after callback return — lets Smalltalk
                                                    finish cleanup (release mutex, signal semaphore).

    ~100 ms     FFI.cpp:1158                        SDL event retry loop: 10 iterations × 10 ms.
    (10 × 10ms)                                     For modal event loops (menus).

## Network I/O

    Value       Location                            Purpose
    -------     ----------------------------------  -----------------------------------------------
    100 ms      SocketPlugin.cpp:141                Socket select() timeout for non-blocking
                                                    readiness check.

    200 ms      SocketPlugin.cpp:255                Socket I/O thread shutdown wait.

## Primitives

    Value       Location                            Purpose
    -------     ----------------------------------  -----------------------------------------------
    10 ms       Primitives.cpp:12200                Max sleep in primitiveRelinquishProcessor.
                                                    Caps Delay.wait sleep to keep event loop responsive.

## Test Harness (test_load_image.cpp)

    Value       Location                            Purpose
    -------     ----------------------------------  -----------------------------------------------
    5 sec       test_load_image.cpp:767             Click injection delay after launch.

    10 sec      test_load_image.cpp:762, :777       Progress logging interval; test trigger delay.

    300 sec     test_load_image.cpp:786             Monitor stall detection (EXCLUDED — 5x pattern).

## Notes

- The 600s process timeout was raised from 90s on 2026-04-06 because the
  test suite legitimately runs for minutes in a single P40 process.
- The heartbeat watchdog (15s) and the wall-clock timeout (600s) are
  independent layers — the heartbeat fires on timer ticks, the wall-clock
  fires on bytecode check intervals.
- Bytecode-step-based timeouts (cannotReturn, callback) scale with VM speed;
  wall-clock timeouts do not.
