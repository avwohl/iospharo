#!/bin/bash
# Wrapper: starts lldb with MCP protocol server on a TCP port, then
# forwards MCP stdio traffic from Claude Code to that port via netcat.
# Invoked automatically when Claude Code launches — see
# `claude mcp add lldb -- scripts/lldb-mcp-start.sh`.

PORT="${PHARO_LLDB_MCP_PORT:-59999}"
LLDB="${LLDB:-/usr/bin/lldb}"
LOG=/tmp/lldb-mcp.log

# Start lldb with MCP server if nothing is already listening on this
# port.  lldb in -b (batch) mode exits after the command finishes,
# killing the server with it — so we pipe the start command into an
# interactive lldb and keep stdin open so lldb stays alive.
if ! /usr/bin/nc -z localhost "$PORT" 2>/dev/null; then
    (echo "protocol-server start MCP listen://localhost:$PORT"; \
     while :; do sleep 3600; done) | "$LLDB" >"$LOG" 2>&1 &
    # Wait up to ~5 s for the port to bind.
    for i in $(seq 1 20); do
        /usr/bin/nc -z localhost "$PORT" 2>/dev/null && break
        sleep 0.25
    done
fi

# Forward stdio to the MCP server.  exec means Claude Code's MCP
# client speaks directly to lldb's MCP over the TCP socket.
exec /usr/bin/nc localhost "$PORT"
