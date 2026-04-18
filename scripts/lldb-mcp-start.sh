#!/bin/bash
# Wrapper: spawns a FRESH lldb + MCP server for this Claude Code
# session, forwards stdio to it over netcat, and kills lldb when the
# stdio pipe closes.
#
# Why fresh-per-session: Apple's lldb MCP server
# (ProtocolServerMCP::ReadCallback in lldb-2100.0.16.12 / Xcode 17E202)
# does not deregister peers from its kqueue on disconnect.  Each
# CLOSE_WAIT/CLOSED socket triggers a tight `recv → consumeError`
# spin, driving lldb to 99% CPU per prior client.  Killing lldb on
# nc EOF keeps each reconnect clean.
#
# Invoked automatically when Claude Code launches — see
# `claude mcp add lldb -- scripts/lldb-mcp-start.sh`.

PORT="${PHARO_LLDB_MCP_PORT:-59999}"
LLDB="${LLDB:-/usr/bin/lldb}"
LOG=/tmp/lldb-mcp.log

# If a stale lldb (or anything else) is still bound to $PORT —
# e.g., a previous wrapper that was killed -9 before its trap ran —
# clear it so we can bind a fresh listener.
STALE=$(/usr/sbin/lsof -tiTCP:"$PORT" -sTCP:LISTEN 2>/dev/null)
if [ -n "$STALE" ]; then
    kill $STALE 2>/dev/null
    sleep 0.5
    STALE=$(/usr/sbin/lsof -tiTCP:"$PORT" -sTCP:LISTEN 2>/dev/null)
    [ -n "$STALE" ] && kill -9 $STALE 2>/dev/null
    sleep 0.3
fi

echo "--- wrapper started $(date '+%F %T') pid=$$" >>"$LOG"

# Start a fresh lldb in the background with the MCP server
# listening on $PORT.  The subshell prints the start command, then
# execs into `tail -f /dev/null` to keep lldb's stdin open (lldb
# exits on EOF; macOS /bin/sleep doesn't accept `infinity`, and
# `tail -f /dev/null` is a single process with no children).
# Killing that stdin-keeper (via pkill -P $$ below) causes lldb to
# see EOF and exit cleanly.
( printf 'protocol-server start MCP listen://localhost:%s\n' "$PORT"
  exec tail -f /dev/null
) | "$LLDB" >>"$LOG" 2>&1 &
LLDB_PID=$!

# When this wrapper exits (nc returns on EOF, or we get a signal),
# kill every direct child: the stdin-keeper, lldb, and nc.  That
# lets the next Claude Code reconnect start with a fresh lldb.
cleanup() {
    echo "--- wrapper exiting $(date '+%F %T') pid=$$" >>"$LOG"
    pkill -P $$ 2>/dev/null
    kill "$LLDB_PID" 2>/dev/null
}
trap cleanup EXIT INT TERM

# Wait up to ~10s for the listener to bind before forwarding stdio.
for i in $(seq 1 40); do
    /usr/bin/nc -z localhost "$PORT" 2>/dev/null && break
    sleep 0.25
done

# Forward Claude Code's stdio to the MCP socket.  When Claude Code
# closes its pipe, nc exits → the trap fires → lldb is killed.
/usr/bin/nc localhost "$PORT"
