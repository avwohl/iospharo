# LLDB MCP — AI-controllable debugger

Apple's `/usr/bin/lldb` ships with a built-in MCP (Model Context
Protocol) server.  With it enabled, Claude Code can drive lldb via
the `lldb_command` tool — set breakpoints, inspect memory, step
through code, read registers, etc. — all from inside a session.

## Setup (one-time)

1. The MCP is registered per-project via the wrapper at
   `scripts/lldb-mcp-start.sh`.  If you're on a fresh checkout,
   add it to your Claude Code config:

   ```bash
   claude mcp add lldb -- $(pwd)/scripts/lldb-mcp-start.sh
   ```

2. Restart Claude Code.  On start-up it launches the wrapper,
   which spawns `lldb` in the background with
   `protocol-server start MCP listen://localhost:59999`, then
   forwards the session's stdio over netcat.  `claude mcp list`
   should show `lldb: ... ✓ Connected`.

3. Verify with a simple roundtrip inside Claude:

   ```
   > use the lldb MCP to show `version`
   ```

   Claude will call `lldb_command` with
   `{"debugger_id": 0, "arguments": "version"}` and paste lldb's
   banner back.

## How to use

The exposed tool is intentionally thin — it just runs lldb
commands.  Example debugging pattern for the iOS Pharo VM:

1. Create a target and set breakpoints:

   ```
   target create /Users/wohl/src/iospharo/build/test_load_image
   breakpoint set --name _ZN5pharo11Interpreter18sendMustBeBooleanENS_3OopE --auto-continue true
   breakpoint command add --script-type default 1
   bt 3
   DONE
   ```

2. Run the scenario:

   ```
   settings set target.env-vars PHARO_NO_JIT=0 PHARO_JIT_SIMSTACK=1
   run /tmp/revert_check.image eval "'/tmp/test_int_detail2.st' asFileReference fileIn"
   ```

3. Inspect when the breakpoint fires.  `frame variable`, `reg read
   x19 x20`, `memory read`, etc. are all available through the same
   `lldb_command` call.

## Files

- `scripts/lldb-mcp-start.sh` — wrapper that auto-starts lldb and
  pipes stdio to the MCP socket.  Reads `PHARO_LLDB_MCP_PORT`
  (default 59999) and `LLDB` (default `/usr/bin/lldb`) from the
  environment.
- `/tmp/lldb-mcp.log` — lldb output captured for debugging the
  wrapper itself.

## Why not Claude Code's built-in tool set?

Claude Code doesn't currently ship with a debugger tool. The MCP
route uses Apple's built-in lldb MCP server (requires lldb 18 or
newer — present on macOS 15+ / Xcode 17+; our system has
lldb-2100.0.16.12).  No third-party installs needed.

## Future work

- Script common debugging scenarios (watchpoints on the IC slot
  for A3, register watches for the SimStack spill path) as Bash
  helpers so the human + AI can launch them in one command.
- Add a breakpoint helper that prints the selector + bytecode
  offset of the current frame when relevant.
