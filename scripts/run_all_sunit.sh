#!/bin/bash
# Offline full-coverage SUnit driver for the custom VM.
#
#   scripts/run_all_sunit.sh [image]
#
# Drives the project's canonical in-image runner
# (scripts/pharo-headless-test/run_sunit_tests.st) over its batch-range
# mechanism (/tmp/sunit_batch.txt) so that EVERY concrete TestCase subclass
# (~2051 classes / ~14600 tests) gets a chance to run, even though a single
# class can hard-hang the VM (the in-image per-test watchdog does not catch
# every hang; the monolithic run stalls partway).
#
# Unlike scripts/run_all_tests.sh (which curls a fresh image from the network
# each batch), this works fully offline against a local prepped image.
#
# Strategy: run a window [start, start+WINDOW-1]; a fresh VM process per
# window gives per-window isolation. A STALL-based watchdog kills a window
# only when /tmp/sunit_test_results.txt stops growing for STALL_SECONDS.
# After each window we count completed classes (Total: lines); if the window
# stalled, the class right after the last completed one is the hanger — it is
# recorded in /tmp/sunit_hangers.txt and skipped, and the run continues. So
# only genuine hangers are dropped; nothing else is lost.
#
# Outputs:
#   /tmp/sunit_all_results.txt   concatenation of every window's results
#   /tmp/sunit_all_detail.txt    concatenation of every window's detail
#   /tmp/sunit_hangers.txt       classes that hung the VM (one per line)
#   /tmp/sunit_all_summary.txt   final aggregate P/F/E/S + hanger list
#
# Env: WINDOW (default 50), STALL_SECONDS (150), POLL (5),
#      HARD_CAP per-window ceiling seconds (1200), MAX_CLASSES safety (2200),
#      PHARO (stock pharo CLI, default /tmp/harness/pharo),
#      SKIP_PREP=1 to reuse an already-prepped image.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMG="${1:-/tmp/harness/Pharo-jit.image}"
VM="$ROOT/build/test_load_image"
PHARO="${PHARO:-/tmp/harness/pharo}"
RUNNER="$ROOT/scripts/pharo-headless-test/run_sunit_tests.st"
WINDOW="${WINDOW:-50}"
STALL_SECONDS="${STALL_SECONDS:-150}"
POLL="${POLL:-5}"
HARD_CAP="${HARD_CAP:-1200}"
MAX_CLASSES="${MAX_CLASSES:-2200}"

LOGDIR=/tmp/sunit_logs; mkdir -p "$LOGDIR"
ALL_RES=/tmp/sunit_all_results.txt
ALL_DET=/tmp/sunit_all_detail.txt
HANGERS=/tmp/sunit_hangers.txt
SUMMARY=/tmp/sunit_all_summary.txt
norm() { tr '\r' '\n' < "$1" 2>/dev/null; }

if [ "${SKIP_PREP:-0}" != "1" ]; then
  echo "[prep] filing canonical runner into $IMG (--save)"
  cp "$ROOT/scripts/pharo-headless-test/test_classes.txt" /tmp/sunit_test_classes.txt 2>/dev/null
  timeout 180 "$PHARO" "$IMG" eval --save \
    "'$RUNNER' asFileReference fileIn" > "$LOGDIR/prep.log" 2>&1
  echo "[prep] exit=$?"
fi

: > "$ALL_RES"; : > "$ALL_DET"; : > "$HANGERS"
start=1; total=0; window_no=0

while [ "$start" -le "$MAX_CLASSES" ]; do
  window_no=$((window_no + 1))
  end=$((start + WINDOW - 1))
  echo "$start $end" > /tmp/sunit_batch.txt
  rm -f /tmp/sunit_test_results.txt /tmp/sunit_test_detail.txt /tmp/sunit_run_completed.txt

  "$VM" "$IMG" > "$LOGDIR/win_${start}.log" 2>&1 &
  pid=$!
  t0=$(date +%s); last_change=$t0; psz=-1; reason=running
  while kill -0 "$pid" 2>/dev/null; do
    sleep "$POLL"
    now=$(date +%s)
    [ -f /tmp/sunit_run_completed.txt ] && { reason=completed; kill -9 "$pid" 2>/dev/null; break; }
    sz=$(wc -c < /tmp/sunit_test_results.txt 2>/dev/null | tr -d ' '); sz=${sz:-0}
    if [ "$sz" != "$psz" ]; then psz=$sz; last_change=$now; fi
    if [ $((now - last_change)) -ge "$STALL_SECONDS" ]; then reason=stalled; kill -9 "$pid" 2>/dev/null; break; fi
    if [ $((now - t0)) -ge "$HARD_CAP" ]; then reason=hardcap; kill -9 "$pid" 2>/dev/null; break; fi
  done
  wait "$pid" 2>/dev/null

  norm /tmp/sunit_test_results.txt >> "$ALL_RES"
  norm /tmp/sunit_test_detail.txt  >> "$ALL_DET"
  c=$(norm /tmp/sunit_test_results.txt | grep -c '^Total:'); c=${c:-0}
  [ "$total" = "0" ] && total=$(norm /tmp/sunit_test_results.txt | sed -n 's/.*of \([0-9]*\) ).*/\1/p' | head -1)
  [ -z "$total" ] && total=0
  hanger=$(norm /tmp/sunit_test_results.txt | grep '^=== ' | grep -v 'SUnit Test Run' | tail -1 | sed 's/^=== //; s/ ===.*//')

  if [ -f /tmp/sunit_run_completed.txt ] || [ "$reason" = "completed" ] || [ "$c" -ge "$WINDOW" ]; then
    echo "win=$window_no [$start-$end] reason=$reason completed=$c -> advance"
    start=$((end + 1))
  else
    # stalled/crashed after $c completed: the (c+1)-th class in the window hung
    echo "win=$window_no [$start-$end] reason=$reason completed=$c HANGER=${hanger:-?} (idx $((start + c)))"
    [ -n "$hanger" ] && echo "$hanger" >> "$HANGERS"
    start=$((start + c + 1))
  fi
  if [ "$total" -gt 0 ] && [ "$start" -gt "$total" ]; then echo "reached total=$total"; break; fi
done

# aggregate
awk '/^Total:/{for(i=1;i<=NF;i++){if($i~/^P:/){split($i,a,":");p+=a[2]}
  if($i~/^F:/){split($i,a,":");f+=a[2]} if($i~/^E:/){split($i,a,":");e+=a[2]}
  if($i~/^S:/){split($i,a,":");s+=a[2]}}}
  END{printf "classes_run=%d  P=%d F=%d E=%d S=%d  total=%d\n",NR,p,f,e,s,p+f+e+s}' \
  "$ALL_RES" > "$SUMMARY"
echo "hangers=$(wc -l < "$HANGERS" | tr -d ' ')" >> "$SUMMARY"
echo "=== summary ==="; cat "$SUMMARY"
echo "=== hangers ==="; cat "$HANGERS"
