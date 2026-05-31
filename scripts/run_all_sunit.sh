#!/bin/bash
# Offline full-coverage SUnit driver for the custom VM.
#
#   scripts/run_all_sunit.sh [image]
#
# Drives the project's canonical in-image runner
# (scripts/pharo-headless-test/run_sunit_tests.st) so that EVERY concrete
# TestCase subclass (~2051 classes / ~14600 tests) gets a chance to run, even
# though a single class can hard-hang the VM and the monolithic run stalls
# partway (the in-image per-test watchdog does not catch every hang).
#
# IMPORTANT: the runner's batch-RANGE mechanism (/tmp/sunit_batch.txt) does NOT
# work on our VM — the integer-parse path hits a ByteString DNU, so batchClasses
# silently defaults to all 2051 and every window re-runs from class 1. The
# class-NAMES filter (/tmp/sunit_class_names.txt, read via #lines) DOES work.
# So this driver feeds each window an explicit slice of class NAMES taken from
# a pre-generated ordered list (/tmp/sunit_all_class_names.txt). Regenerate that
# list with scripts/gen_sunit_class_list.sh if the image's test set changes.
#
# Per window: fresh VM process for isolation; STALL-based watchdog kills it only
# when /tmp/sunit_test_results.txt stops growing for STALL_SECONDS (a clean
# window exits early via the run_completed marker). After each window we count
# completed classes (Total: lines); a window that completed fewer than it was
# given stalled on the next one — that class is the hanger: recorded in
# /tmp/sunit_hangers.txt and skipped. So only genuine hangers are dropped.
#
# Outputs:
#   /tmp/sunit_all_results.txt   concatenation of every window's results
#   /tmp/sunit_all_detail.txt    concatenation of every window's detail
#   /tmp/sunit_hangers.txt       classes that hung the VM (one per line)
#   /tmp/sunit_all_summary.txt   final aggregate P/F/E/S + hanger count
#
# Env: WINDOW (default 50), STALL_SECONDS (150), POLL (5),
#      HARD_CAP per-window ceiling seconds (1200),
#      LIST (default /tmp/sunit_all_class_names.txt),
#      PHARO (stock pharo CLI, default /tmp/harness/pharo),
#      SKIP_PREP=1 to reuse an already-prepped image,
#      SKIP_LIST=1 to reuse an existing class-name list.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMG="${1:-/tmp/harness/Pharo-jit.image}"
VM="$ROOT/build/test_load_image"
PHARO="${PHARO:-/tmp/harness/pharo}"
RUNNER="$ROOT/scripts/pharo-headless-test/run_sunit_tests.st"
LIST="${LIST:-/tmp/sunit_all_class_names.txt}"
WINDOW="${WINDOW:-50}"
STALL_SECONDS="${STALL_SECONDS:-150}"
POLL="${POLL:-5}"
HARD_CAP="${HARD_CAP:-1200}"

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

if [ "${SKIP_LIST:-0}" != "1" ] || [ ! -s "$LIST" ]; then
  echo "[list] generating ordered class-name list -> $LIST"
  "$ROOT/scripts/gen_sunit_class_list.sh" "$IMG" > "$LOGDIR/genlist.log" 2>&1
fi
# bash 3.2 (macOS) has no mapfile — read into array portably.
CLASSES=()
while IFS= read -r line || [ -n "$line" ]; do
  [ -n "$line" ] && CLASSES+=("$line")
done < "$LIST"
N=${#CLASSES[@]}
echo "[run] $N classes, window=$WINDOW"

: > "$ALL_RES"; : > "$ALL_DET"; : > "$HANGERS"
idx=0
while [ "$idx" -lt "$N" ]; do
  # write next WINDOW names to the names filter
  : > /tmp/sunit_class_names.txt
  end=$((idx + WINDOW)); [ "$end" -gt "$N" ] && end="$N"
  given=$((end - idx))
  for ((j=idx; j<end; j++)); do printf '%s\n' "${CLASSES[$j]}" >> /tmp/sunit_class_names.txt; done
  first="${CLASSES[$idx]}"
  rm -f /tmp/sunit_test_results.txt /tmp/sunit_test_detail.txt /tmp/sunit_run_completed.txt

  # FRESH_IMAGE=1: run each window on a pristine copy of the prepped image so
  # cumulative image-state degradation cannot accumulate across windows (that
  # degradation, not any per-class bug, is what makes late classes "hang" — all
  # 20 hangers from the single-image run pass cleanly in isolation; see
  # docs/sunit-hangers-classified.txt).
  RUNIMG="$IMG"
  if [ "${FRESH_IMAGE:-0}" = "1" ]; then
    RUNIMG="/tmp/sunit_win.image"
    cp "$IMG" "$RUNIMG"
    [ -f "${IMG%.image}.changes" ] && cp "${IMG%.image}.changes" "/tmp/sunit_win.changes" 2>/dev/null
  fi

  "$VM" "$RUNIMG" > "$LOGDIR/win_${idx}.log" 2>&1 &
  pid=$!
  t0=$(date +%s); last_change=$t0; psz=-1; reason=running
  while kill -0 "$pid" 2>/dev/null; do
    sleep "$POLL"; now=$(date +%s)
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

  if [ "$c" -ge "$given" ]; then
    echo "win@$idx [$first +$given] reason=$reason completed=$c -> advance"
    idx="$end"
  else
    hanger="${CLASSES[$((idx + c))]}"
    echo "win@$idx [$first +$given] reason=$reason completed=$c HANGER=$hanger -> skip"
    printf '%s\n' "$hanger" >> "$HANGERS"
    idx=$((idx + c + 1))
  fi
done
rm -f /tmp/sunit_class_names.txt

awk '/^Total:/{for(i=1;i<=NF;i++){if($i~/^P:/){split($i,a,":");p+=a[2]}
  if($i~/^F:/){split($i,a,":");f+=a[2]} if($i~/^E:/){split($i,a,":");e+=a[2]}
  if($i~/^S:/){split($i,a,":");s+=a[2]}}}
  END{printf "classes_run=%d  P=%d F=%d E=%d S=%d  total=%d\n",NR,p,f,e,s,p+f+e+s}' \
  "$ALL_RES" > "$SUMMARY"
echo "hangers=$(wc -l < "$HANGERS" | tr -d ' ')" >> "$SUMMARY"
echo "=== summary ==="; cat "$SUMMARY"
echo "=== hangers ==="; cat "$HANGERS"
