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
   which spawns a fresh `lldb` in the background with
   `protocol-server start MCP listen://localhost:59999`, then
   forwards the session's stdio over netcat.  `claude mcp list`
   should show `lldb: ... ✓ Connected`.

3. Verify with a simple roundtrip inside Claude:

   ```
   > use the lldb MCP to show `version`
   ```

   Claude will call `lldb_command` with
   `{"debugger_id": 1, "arguments": "version"}` and paste lldb's
   banner back.  The interactive lldb the wrapper spawns is
   **debugger_id 1**, not 0 — id 0 returns `"no debugger with id
   0"`.

## Session lifecycle

Each Claude Code MCP reconnect gets a **fresh** lldb instance.
The wrapper spawns lldb on invocation and kills it when the stdio
pipe closes (nc EOF).  Consequences:

- Debugger state (targets, breakpoints, watchpoints) is **not**
  preserved across Claude Code reconnects.  Treat each session as
  starting from scratch and script any setup you need.
- No socket leaks.  Apple's shipped lldb
  (`lldb-2100.0.16.12` / Xcode 17E202) does not deregister
  disconnected MCP clients from its kqueue, so a shared persistent
  lldb would spin at 99% CPU per prior disconnected peer.
  Fresh-per-session sidesteps that bug.

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

- `scripts/lldb-mcp-start.sh` — wrapper that spawns a fresh lldb,
  forwards stdio to its MCP socket, and kills the lldb on EOF so
  each reconnect starts clean.  Reads `PHARO_LLDB_MCP_PORT`
  (default 59999) and `LLDB` (default `/usr/bin/lldb`) from the
  environment.
- `/tmp/lldb-mcp.log` — lldb output captured for debugging the
  wrapper itself, including start/exit timestamps per invocation.

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
