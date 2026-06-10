# Claude Code Instructions

This is an iOS Pharo VM (clean C++ reimplementation; oop encoding moved to
low bits for ASLR compatibility).

Key paths:
    src/vm/              clean C++ VM (ImageLoader, Interpreter, ObjectMemory, Primitives)
    src/ios/             reference: cointerp-cpp.c, primitives.json, generated_primitives.inc, oop.hpp
    src/vm/jit/          JIT compiler + stencils; SistaV1.hpp is the bytecode-opcode source of truth
    scripts/             build and transformation scripts
    docs/                documentation
    docs/deferred.md     consolidated list of deferred work
    docs/image_issues.md Pharo 13 image bugs we patch + upstream wishlist

## Hard rules

- Never draw tables using markdown table syntax or `|` (pipe) characters in
  console output — they don't copy/paste cleanly into email. Use space-aligned
  columns inside a code block (fixed-width font is assumed). Other markdown is
  fine: headings, lists, blockquotes, code blocks, links, .md files. The rule
  is narrowly about table-drawing, not a ban on markdown or on the pipe char.
- Never use raw hex like `0x23` for opcodes or other constants. Use named
  constants (`SistaV1::PushReceiver`, `SistaV1::isSendBytecode(op)`).
- Never skip hooks (`--no-verify`), force-push to main, or run destructive git
  operations without asking first.
- Only use emojis if the user explicitly requests them.
- Never call `std::getenv(...)` for new VM debug/control knobs. Every
  env-var lookup goes through `g_debug.<field>` (defined in
  `src/vm/DebugSettings.{hpp,cpp}`), which reads each variable ONCE at
  static-init. Add a field + initializer there, then read
  `g_debug.fieldName` at the call site. The `static const bool x =
  getenv(...)` lazy-init pattern is also banned — use DebugSettings.

## Workflow

- Don't lose hours of work to a crash: commit when there's a meaningful unit
  of progress, and don't sit on uncommitted work for hours. This is a safety
  rule, not a quota — committing more often than the work warrants makes
  things worse (rapid small commits, half-validated experiments, vanity
  metric chasing). For large projects, fewer larger commits are better than
  many small ones.
- Pick the next item and keep going — don't stop to summarize or ask for
  direction.
- Update `docs/*.md` and memory files at least once per hour during long tasks.
  Sessions have crashed after 12 h with nothing written down.
- Debug before asking: run the app and check logs yourself before asking the
  user to test.
- `git status` before and after commits.
- When a background task is running, start the next task immediately. Don't
  idle waiting for the notification.
- Don't ScheduleWakeup unless you're actually waiting for something — a long
  build, a Monitor event, or an explicit "check back in N minutes." For
  `/loop until <goal>` invocations, just keep iterating; an idle 20-30 min
  sleep burns the user's time for no benefit. The /loop skill calls these
  "fallback heartbeats" but they're only useful when a real signal exists.
- In `/loop` dynamic mode: **KEEP WORKING in the current turn**. Don't
  preemptively wrap up with "stopping here, re-invoke /loop." That defeats
  the whole point of /loop — the user wants continuous iteration without
  having to keep re-typing the command. Only stop when (a) the goal is
  genuinely achieved, (b) a real blocker requires user input, or (c) you
  hit a destructive action that needs confirmation. Picking the next
  sub-task from `docs/deferred.md` and continuing is always preferable to
  ending the turn. If you genuinely run out of context, the turn will end
  on its own — don't help it along.
- **`/goal` rules** (same shape as /loop — these PERSIST across sessions):
  - The goal NEVER auto-clears. If the doc's queue empties, ADD MORE
    TASKS by scanning the codebase for similar issues. Don't stop just
    because a list ran out.
  - Never write "session totals", "progress summary", or "moving to next
    task" messages between tasks. They're stop-signals.  Just do the
    next thing.
  - When the user interrupts mid-goal: answer in ≤2 sentences, then
    IMMEDIATELY return to the next task. Don't recap, don't restate
    plans, don't say "continuing now". The next thing you output after
    the interrupt should be a tool call, not prose.
  - "Investigation" / "audit" / "characterize" tasks invite summary
    responses → stop. Tasks must have BINARY done conditions: file X
    line Y edited, counter Z non-zero, bench 5/5 PASS.
  - If a task is sized "1-4 hours" or "multi-hour", split it. The
    larger the task, the more likely you stop partway. Max 15 min per
    leaf task.
  - **NEVER add per-call `std::getenv()` in hot paths.** Each call does
    linear env scan. Use `static const bool x = std::getenv(...) != nullptr;`
    or DebugSettings. This is THE biggest accidental-overhead trap —
    one session found 20-30% bench-suite overhead from this anti-pattern.
- **Two separate numbers govern /loop — do NOT conflate them:**
  1. **How long to WORK in a single turn:** very long. Keep doing tasks
     back-to-back until a natural stopping point (real blocker, needed
     confirmation, or context exhaustion). The wake-up interval does NOT
     bound the turn's work duration.
  2. **How long to SLEEP between turns:** short. Default ~60 s for
     fixed-interval `/loop Nm` and ~60–120 s for dynamic-mode fallback
     wake-ups unless there's a concrete reason to sleep longer (waiting
     on a CI run, deploy, etc.). Never auto-pick 1200–1800 s "because
     the loop is idle" — that's misreading the cache-window guidance.
  If `/loop 1m` was set, that means "wake every 1 min if I'm not already
  running"; it does NOT mean "work for at most 1 min per wake-up."

## No workarounds — fix root causes

When something doesn't work: stop, find the root cause, fix the actual bug.
Don't add hacks that hide the problem. Patterns that are ALWAYS wrong:

- Silently swallowing errors (push nil and return, `suspendActiveProcess_`,
  removing from scheduler queues).
- Hardcoded class/selector checks to skip methods that "cause problems".
- Treating non-booleans as false — conditional jumps must send `mustBeBoolean`
  per spec.
- Loop/depth detectors that silently recover — if DNU recurses infinitely,
  `stopVM()`, don't push nil.
- C++ doing Smalltalk's job (direct HandMorph manipulation, C++ event dispatch).

## Common silent perf traps

These don't cause bugs, but silently waste cycles in hot paths.  May 2026
audit found these were costing 20-70% on bench-suite each:

- **Per-call `std::getenv()` in hot functions.**  Each call does a linear
  search through the environment array.  Pattern `if (std::getenv("X"))`
  LOOKS gated but actually calls getenv every invocation.  Found in
  executePrimitive entry (16007), activateMethod (8455), op_value1
  bytecode (2260), activateBlock (9831), prim207 (3461), patchJITICAfterSend,
  and bytecode 0x7A.  Caching with `static const bool x = std::getenv(...)
  != nullptr;` recovered: sum 1M 176→101 (-43%), alloc 13→5 (-62%),
  floatSum 402→119 (-70%).
- **`std::unordered_map` lookup in per-call paths.**  Even O(1) amortized
  is too expensive when the hot path already runs in 50-100 cycles.  e.g.
  a CompiledBlock→home-method cache LOST 15% on fib because the lookup
  overhead exceeded the saved chain walk.

## Verify GUI changes visually

Never claim display, menus, or interaction work without screenshotting and
reading the screenshot. Logs and test pass rates aren't enough — two weeks of
"verified working" claims were wrong because nobody looked at the screen.

`screencapture -x` can't capture Metal content from Mac Catalyst apps. Use
window-specific capture:

```bash
PID=$(pgrep -f "iospharo" | head -1)
swift -e "
import CoreGraphics
let windowList = CGWindowListCopyWindowInfo(.optionAll, kCGNullWindowID) as? [[String: Any]] ?? []
for w in windowList {
    guard let ownerPID = w[kCGWindowOwnerPID as String] as? Int, ownerPID == $PID else { continue }
    let windowID = w[kCGWindowNumber as String] as? Int ?? -1
    print(\"id=\(windowID)\")
}"
screencapture -x -l <WINDOW_ID> /tmp/pharo-screenshot.png
```

Then read the screenshot with Read and verify each specific claim.

GUI verified working 2026-02-24: SDL2 stubs in `FFI.cpp` bridge the image's
`OSSDL2Driver` to the Metal pipeline.

## GUI testing — mandatory timeouts

Every GUI interaction MUST have a hard timeout that kills the process.
`osascript` and Catalyst interactions have hung sessions for 28+ minutes.

- Always `timeout 60 <command>` (or appropriate).
- Background launches: also set up `sleep N && kill` as a kill timer.
- `osascript` UI clicks: `timeout 10 osascript -e '...'`.
- Screenshot loops: max 10 iterations with `sleep` between.
- Never `open -W` — it waits forever. Use `open` + sleep + screenshot.
- Kill the app between tests. Don't leave it running.

Pattern:

```bash
timeout 90 open /path/to/app &
APP_PID=$!
sleep 15
timeout 5 screencapture -x -l <WIN_ID> /tmp/pharo-screenshot.png
timeout 10 osascript -e 'tell application "System Events" to ...'
kill $APP_PID 2>/dev/null
killall iospharo 2>/dev/null
```

## Image compatibility

The VM must work with standard Pharo images (no iOS-specific prep). Always
test with freshly downloaded images, not saved ones.

- `primitive 97` (snapshot) saves in standard Spur format. Both
  `Smalltalk snapshot:andQuit:` and raw `<primitive: 97>` work, round-trip
  verified.
- Startup-script patches live in `PharoBridge.writeStartupScript()` and are
  auto-loaded by `StartupPreferencesLoader`.

## Build + run

    cmake --build build                                      # normal rebuild
    xcodebuild -project iospharo.xcodeproj -scheme iospharo \
      -configuration Debug -destination 'platform=macOS,variant=Mac Catalyst' build
    ./build/test_load_image <image-path>                     # quick VM test
    timeout 60 open path/to/iospharo.app --args --image /tmp/Pharo.image
                                                             # auto-launch bypass library

Xcode's "Check XCFramework Freshness" build phase auto-runs
`build-xcframework.sh` if VM sources changed.

## lldb is available — don't dismiss bugs as "needs lldb"

This machine has full lldb tooling for live JIT debugging.  Multiple
prior sessions have used it heavily.  Do NOT punt on a bug by saying
"needs lldb, multi-day, out of scope" — attach lldb and investigate.

What's wired up:
- `scripts/lldb-mcp-start.sh` — Claude Code launches this automatically
  via MCP, so lldb is reachable as MCP tools (`mcp__lldb__*`) when
  the lldb MCP server is registered for the session.
- `test_load_image` is codesigned with `get-task-allow` + JIT
  entitlements via the CMake POST_BUILD step (commit `5b715fc2`),
  so `lldb -p <pid>` and `lldb ./build/test_load_image` both attach
  cleanly without the macOS task-port hang.
- The MCP wrapper restarts lldb fresh per session to dodge a known
  Apple lldb-2100 socket-leak bug (commit `00c93286`).

Standard workflow for "JIT compiles wrong method, sender chain
corrupts, P80 terminates" / sentinel-pattern bugs:
1. Reproduce under lldb attach.  Set a breakpoint at the TERM
   trace site (e.g. `Interpreter::terminateCurrentProcess` line
   that prints `[TERM-P%lld] PROCESS TERMINATING via #%s`).
2. When it fires, inspect `activeContext_`, `savedFrames_`, and
   the JITMethod-map entry for `method_`.
3. Walk the bytecode of the offending method, identify the
   stencil whose epilog leaves the saved-sender slot corrupt.

CLI invocation if MCP isn't loaded for some reason:

    lldb -O 'b Interpreter::terminateCurrentProcess' \
         ./build/test_load_image -- /tmp/harness/Pharo.image

Memory: `project_jit_session_2026_05_06.md` documents prior
lldb-driven JIT debug sessions.

## Debugging timing-sensitive JIT Heisenbugs (`PHARO_DET_SCHED`)

Some JIT correctness bugs only fire under a specific interleaving of the
forked SUnit test processes. The round-robin preemption is normally driven by
the **wall-clock heartbeat thread** (`Interpreter.cpp:3814`, sets `forceYield_`
every ~2 ms). That makes such bugs *Heisenbugs*: any observation overhead — an
lldb breakpoint, even a cheap `fprintf` trace — shifts the timing and the bug
vanishes. A full session once burned ~18 probes that all "fixed" the bug merely
by being attached.

The cure is **deterministic scheduling**. `PHARO_DET_SCHED=1` drives the
force-yield from the per-1024-bytecode checkpoint (`g_stepNum`,
`Interpreter.cpp:2637`) instead of the wall clock, and disables the heartbeat
yield. Scheduling is then identical every run, so the bug reproduces at a FIXED
point — **and keeps reproducing with instrumentation attached.** Verified: it
turned the aigraph inline-getter transient (`AIPrimTest` / `AITarjanTest`,
`Error: No Element in Graph`) from unpinnable into a deterministic, lldb-able
layout bug. `PHARO_DET_SCHED_QUANTUM=N` widens the yield interval to N×1024
bytecodes (default 1) if you need a coarser round-robin.

Repro pattern for a timing-sensitive JIT bug:

    printf 'AIPrimTest\n' > /tmp/sunit_class_names.txt
    PHARO_T1_INLINE_GETTER=1 PHARO_DET_SCHED=1 \
      ./build/test_load_image /tmp/harness/Pharo-jit.image
    # ERROR=2 every run; now safe to attach lldb / add traces without
    # suppressing it. Bisect emit knobs (NO_INLINE_GETTER, T1_NO_INLINE_PRIM_AT,
    # NO_J2J, ...) deterministically to isolate the interacting specializations.

When you hit "works under the debugger, fails otherwise" on a JIT bug, reach
for `PHARO_DET_SCHED` FIRST — don't re-derive that the heartbeat is the culprit.
Full evidence chain: memory `jit_aigraph_fork_arg_corruption.md`,
`docs/results-jitpkg.md`.

## Debug/control knobs: add them in `debug_vars.h` (one place)

New env-var knobs go in `src/vm/debug_vars.h` — a single X-macro list that is
the source of truth. The token IS the literal env-var name, used identically at
declaration, as the env var, and at the fetch site, so there is nothing to keep
in sync (the older `DebugSettings.hpp` declaration + `DebugSettings.cpp`
initializer split drifted out of sync; that's what this replaces). To add one:

    // in src/vm/debug_vars.h
    DEBUG_BOOL(PHARO_MY_FLAG)              // present  => getenv(name) != nullptr
    DEBUG_INT(PHARO_MY_COUNT, 1)           // atoi if set+non-empty, else default
    DEBUG_STR(PHARO_MY_PATH)               // value if set+non-empty, else nullptr

Then read it anywhere (after `#include "DebugVars.hpp"`):

    if (GET_DEBUG_BOOL(PHARO_MY_FLAG)) ...
    int n = GET_DEBUG_INT(PHARO_MY_COUNT);

This still obeys the **no-per-call-`getenv`** rule: all knobs are parsed ONCE at
static init (`DebugVars.cpp`, `initDebugVars()`), and `GET_DEBUG_*` is just an
array index. `DebugVars.cpp` is exempted from the CMake getenv-ban alongside
`DebugSettings.cpp`.

**`DebugSettings.cpp` is FROZEN (2026-06-10): never add `envPresent`/`envInt`/
`envStr` lines there** — that was the slow workaround for the getenv ban, and
the CMake configure step now counts `envPresent(` call sites and FAILS the
build if the count grows (the "envPresent RATCHET" in CMakeLists.txt).  ALL
new knobs go in `debug_vars.h`, including default-ON opt-outs: declare
`DEBUG_BOOL(PHARO_NO_FOO)` and write `!GET_DEBUG_BOOL(PHARO_NO_FOO)` at the
use site (see `PHARO_T1_NO_INLINE_J2J` for the pattern).  Combination knobs
(A||B): compute at the use site from the individual `GET_DEBUG_*` reads, or as
a function-local `static const bool` derived from them.  Converting legacy
`DebugSettings` lines to `debug_vars.h` and lowering the frozen count is
welcome.

## References

- Sista V1 bytecode spec: `docs/SistaV1-Bytecode-Spec.md` (local copy — online
  resources often document the older V3PlusClosures set; the 0xE0-0xFF ranges
  differ completely). Bytecode opcode names live in `src/vm/jit/SistaV1.hpp`
  (use `SistaV1::PushReceiver` etc., plus range helpers like
  `SistaV1::isSendBytecode(op)`).
- Primitive table truth source: `~/src/pharo-vm/smalltalksrc/VMMaker/StackInterpreter.class.st`
  in `initializePrimitiveTable`. VMMaker generates `cointerp.c`, so
  `src/ios/cointerp-cpp.c:2094-2756` is a usable reference. The clean C++ VM
  primitive table in `src/vm/Interpreter.cpp` MUST match. Primitives 256-519
  are external plugin indices, not VM primitives.
- iOS device geometry (safe areas, corner radii, Dynamic Island, squircle
  math): use the `device-geometry` skill at
  `.claude/skills/device-geometry.md`.

## Running Pharo test suites

Each VM has its own runner script (the elaborate fork/watchdog/Delay
machinery in `run_sunit_tests.st` is designed around our VM's failure
modes and hangs every test on stock Cog with TIMEOUT(prim-stuck)).
Fairness comes from the shared external class list — neither runner's
source has test-class symbols as literals, so
`ClassQueryTest>>testAllCallsOn` counts the same senders on both VMs.

    cd /tmp && mkdir -p harness && cd harness && \
      curl -sL https://get.pharo.org/64/130+vm | bash

    # stage the external test-class list for both runners
    cp scripts/pharo-headless-test/test_classes.txt /tmp/sunit_test_classes.txt

    # --- custom VM ---
    # prep: install SUnitRunner + SessionManager startup handler
    /tmp/harness/pharo /tmp/harness/Pharo.image eval --save \
      "'$PWD/scripts/pharo-headless-test/run_sunit_tests.st' asFileReference fileIn"
    # run: no eval args — SessionManager fires SUnitRunner>>startUp: on resume
    ./build/test_load_image /tmp/harness/Pharo.image

    # --- stock-Cog baseline (separate image, run_sunit_cog.st executes inline) ---
    /tmp/harness/pharo /tmp/harness/Pharo.image eval \
      "'$PWD/scripts/pharo-headless-test/run_sunit_cog.st' asFileReference fileIn"

    cat /tmp/sunit_test_results.txt

Both write the same `/tmp/sunit_test_results.txt` + `/tmp/sunit_test_detail.txt`
format, so `scripts/classify-sunit.py cog.txt ours.txt` produces the Δcog diff.

Filters for the custom-VM runner (optional):
- `/tmp/sunit_class_names.txt` — one class name per line to run just those.
- `/tmp/sunit_batch.txt` — `<start> <end>` indices into the full list.

For Spec presenter tests, inject `setup_fake_gui.st` into the prep step
BEFORE the runner — it installs `MorphicUIManager`, `Display`, `WorldMorph`,
and a `FakeGUI` helper (see `scripts/pharo-headless-test/setup_fake_gui.st`).
Without it, ~350 Spec tests fail with "receiver of activate is nil";
with it, 94.6% pass rate on 64 GUI classes.

    scripts/pharo-headless-test/   submodule: https://github.com/avwohl/pharo-headless-test
    docs/test-results.md           compatibility analysis

## Agent delegation

Large files (Interpreter.cpp: 8K lines, Primitives.cpp: 14K lines) pollute
context fast. Delegate to agents for:

- Primitive table audits ("compare slots N-M against cointerp-cpp.c")
- Cross-file verification ("find all places primitive X is referenced")
- Grep/search across 20K+ lines
- Reference extraction from large source files

Keep in main context: small focused edits, individual primitive reads,
specific runtime debugging.
