#!/bin/bash
# Full-coverage crash-resilient SUnit driver for the custom VM.
#
#   scripts/run_all_sunit.sh [image] [--resume]
#
# Runs EVERY concrete TestCase subclass under the JIT VM. Relaunches the VM
# after each crash/hang; the crasher is blacklisted (named in sunit_current.txt
# but absent from sunit_done.txt) so progress always advances. Finishes when
# /tmp/sunit_ALL_DONE.txt appears or MAXITERS launches are spent.
#
# Watchdog is STALL-BASED, not a fixed timeout: a launch is killed only when
# /tmp/sunit_done.txt stops growing for STALL_SECONDS. A slow-but-progressing
# run is never penalised; only a genuinely stuck class (hard hang or soft
# infinite loop) trips the watchdog and gets blacklisted.
#
# Env: MAXITERS (default 120), STALL_SECONDS (default 150), POLL (default 5),
#      HARD_CAP per-launch ceiling seconds (default 3600),
#      PHARO (stock pharo CLI for prep, default /tmp/harness/pharo),
#      SKIP_PREP=1 to reuse an already-prepped image.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMG="${1:-/tmp/harness/Pharo-jit.image}"
VM="$ROOT/build/test_load_image"
PHARO="${PHARO:-/tmp/harness/pharo}"
MAXITERS="${MAXITERS:-120}"
STALL_SECONDS="${STALL_SECONDS:-150}"
POLL="${POLL:-5}"
HARD_CAP="${HARD_CAP:-3600}"
RESUME=0
[ "${2:-}" = "--resume" ] && RESUME=1

LOGDIR=/tmp/sunit_logs
mkdir -p "$LOGDIR"

count() { wc -l < "$1" 2>/dev/null | tr -d ' '; }

if [ "${SKIP_PREP:-0}" != "1" ]; then
  echo "[prep] filing in runner into $IMG (--save)"
  timeout 180 "$PHARO" "$IMG" eval --save \
    "'$ROOT/scripts/run_all_sunit.st' asFileReference fileIn" \
    > "$LOGDIR/prep.log" 2>&1
  echo "[prep] exit=$?"
fi

if [ "$RESUME" != "1" ]; then
  echo "[reset] clearing run state"
  rm -f /tmp/sunit_done.txt /tmp/sunit_blacklist.txt /tmp/sunit_current.txt \
        /tmp/sunit_test_detail.txt /tmp/sunit_test_results.txt \
        /tmp/sunit_ALL_DONE.txt /tmp/sunit_startup_error.txt
fi

i=0
while [ $i -lt $MAXITERS ]; do
  i=$((i + 1))
  "$VM" "$IMG" > "$LOGDIR/launch_$i.log" 2>&1 &
  pid=$!

  start=$(date +%s)
  last_done=$(count /tmp/sunit_done.txt); [ -z "$last_done" ] && last_done=0
  last_change=$start
  while kill -0 "$pid" 2>/dev/null; do
    sleep "$POLL"
    now=$(date +%s)
    cur=$(count /tmp/sunit_done.txt); [ -z "$cur" ] && cur=0
    if [ "$cur" != "$last_done" ]; then
      last_done=$cur; last_change=$now
    fi
    if [ $((now - last_change)) -ge "$STALL_SECONDS" ]; then
      echo "  [watchdog] stalled ${STALL_SECONDS}s at done=$cur, killing launch $i"
      kill -9 "$pid" 2>/dev/null
      break
    fi
    if [ $((now - start)) -ge "$HARD_CAP" ]; then
      echo "  [watchdog] hard cap ${HARD_CAP}s, killing launch $i"
      kill -9 "$pid" 2>/dev/null
      break
    fi
  done
  wait "$pid" 2>/dev/null
  rc=$?

  done=$(count /tmp/sunit_done.txt); bl=$(count /tmp/sunit_blacklist.txt)
  cur=$(cat /tmp/sunit_current.txt 2>/dev/null | tr -cd '[:print:]')
  echo "iter=$i rc=$rc done=${done:-0} blacklist=${bl:-0} current=${cur:-none}"
  if [ -f /tmp/sunit_ALL_DONE.txt ]; then
    echo "ALL_DONE after $i launches"
    break
  fi
done

echo "=== summary ==="
cat /tmp/sunit_test_results.txt 2>/dev/null || echo "(no results file)"
echo "blacklist ($(count /tmp/sunit_blacklist.txt) classes):"
cat /tmp/sunit_blacklist.txt 2>/dev/null || echo "(none)"
