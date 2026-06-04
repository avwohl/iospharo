#!/usr/bin/env bash
# box-safe-run.sh — run test_load_image on the build box inside a memory- and
# CPU-capped systemd transient service, fully DETACHED from the ssh session.
#
# Why: a buggy Sista loop fusion can infinite-loop while allocating, driving the
# 64 GiB box into swap until sshd itself stalls (ssh exit 255, box "lost"). A
# transient service with MemoryMax + CPUQuota + RuntimeMaxSec is cgroup-killed
# when it misbehaves — the SERVICE dies, never the box. Because it's detached,
# an ssh drop mid-run no longer orphans a spinning VM either.
#
# Run this ON THE BOX. It blocks only briefly to launch, then polls.
#
#   box-safe-run.sh <unit-name> <runtime-sec> <env-assignments...> -- <argv...>
# e.g.
#   box-safe-run.sh sv 120 PHARO_SISTA_DISPATCH=1 -- \
#       ./build/test_load_image ~/harness/Pharo.image
set -u

UNIT="${1:?unit name}"; shift
MAXSEC="${1:?runtime sec}"; shift
ENVARGS=()
while [ $# -gt 0 ] && [ "$1" != "--" ]; do ENVARGS+=( -E "$1" ); shift; done
[ "${1:-}" = "--" ] && shift
LOG="/tmp/${UNIT}.log"

sudo systemctl reset-failed "${UNIT}.service" 2>/dev/null || true
sudo systemctl stop "${UNIT}.service" 2>/dev/null || true
: > "$LOG"; chmod 666 "$LOG" 2>/dev/null || true

# 8 GiB cap (56 GiB headroom for OS/sshd), 4 cores (12 free), hard 2-min kill.
sudo systemd-run --unit="$UNIT" --collect \
  -p MemoryMax=8G -p MemorySwapMax=0 -p CPUQuota=400% \
  -p "RuntimeMaxSec=${MAXSEC}" \
  --uid=ubuntu --gid=ubuntu \
  --working-directory=/home/ubuntu/src/iospharo \
  "${ENVARGS[@]}" \
  bash -c 'exec "$@" > '"$LOG"' 2>&1' _ "$@"

echo "launched ${UNIT}.service (MemMax=8G swap=0 cpu=400% maxsec=${MAXSEC}); log=$LOG"
# Poll status in short, un-starvable bursts.
for i in $(seq 1 $(( (MAXSEC/5) + 6 ))); do
  sleep 5
   st=$(systemctl show "${UNIT}.service" -p SubState,Result,MainPID --value 2>/dev/null | tr '\n' ' ')
  active=$(systemctl is-active "${UNIT}.service" 2>/dev/null)
  echo "[$((i*5))s] is-active=$active  ($st)"
  [ "$active" = "active" ] || break
done
echo "=== exit/result ==="
systemctl show "${UNIT}.service" -p Result,ExecMainStatus,ExecMainCode --value 2>/dev/null
echo "=== tail log ==="
tail -40 "$LOG"
