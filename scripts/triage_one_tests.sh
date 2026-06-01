#!/bin/bash
# Batch cross-VM triage: for each `Class>>selector` in a list, run it in ISOLATION
# on stock Cog and on our VM, and classify:
#   COGPASS-OURSFAIL  -> genuine VM bug (the thing to fix)
#   BOTHFAIL          -> image/environment (out of scope)
#   BOTHPASS          -> full-suite artifact (not a per-method bug)
#   COGFAIL-OURSPASS  -> rare; our VM more lenient
#
# Efficiency: each image is prepped ONCE (the slow --save that installs
# OneTestRunner + unregisters SUnitRunner); then we relaunch the prepped image
# per test, rewriting /tmp/sunit_one.txt between launches. One VM boot per test,
# no re-save.
#
#   scripts/triage_one_tests.sh LISTFILE
# Env: BASE, VM, PHARO, NOJIT (default 1 — compare Cog vs our interpreter),
#      TMO per-launch seconds (default 90), OUT (default /tmp/triage_out.txt)
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LIST="${1:?usage: triage_one_tests.sh LISTFILE}"
BASE="${BASE:-/tmp/harness/Pharo-jit.image}"
VM="${VM:-$ROOT/build/test_load_image}"
PHARO="${PHARO:-/tmp/harness/pharo}"
RUNNER="$ROOT/scripts/pharo-headless-test/run_one_test.st"
NOJIT="${NOJIT:-1}"
TMO="${TMO:-90}"
OUT="${OUT:-/tmp/triage_out.txt}"

COG_IMG=/tmp/triage_cog.image
OUR_IMG=/tmp/triage_our.image

echo "[prep] cog image"
cp "$BASE" "$COG_IMG"; cp "${BASE%.image}.changes" "${COG_IMG%.image}.changes" 2>/dev/null
printf 'x' > /tmp/sunit_one.txt; touch /tmp/sunit_one_save_prep.txt
timeout 180 "$PHARO" --headless "$COG_IMG" eval --save "'$RUNNER' asFileReference fileIn" >/dev/null 2>&1
echo "[prep] our image"
cp "$BASE" "$OUR_IMG"; cp "${BASE%.image}.changes" "${OUR_IMG%.image}.changes" 2>/dev/null
timeout 180 "$PHARO" --headless "$OUR_IMG" eval --save "'$RUNNER' asFileReference fileIn" >/dev/null 2>&1
rm -f /tmp/sunit_one_save_prep.txt

verdict() { LC_ALL=C tr -cd '[:print:]\n' < /tmp/sunit_one_result.txt 2>/dev/null | head -1 | awk '{print $1}'; }

: > "$OUT"
n=0
while IFS= read -r spec; do
  [ -z "$spec" ] && continue
  n=$((n+1))
  printf '%s' "$spec" > /tmp/sunit_one.txt
  # Cog
  rm -f /tmp/sunit_one_result.txt /tmp/sunit_one_done.txt
  timeout "$TMO" "$PHARO" --headless "$COG_IMG" >/dev/null 2>&1
  cog="$(verdict)"; [ -z "$cog" ] && cog="TIMEOUT"
  # Ours
  rm -f /tmp/sunit_one_result.txt /tmp/sunit_one_done.txt /tmp/sunit_run_completed.txt
  env $([ "$NOJIT" = 1 ] && echo PHARO_NO_JIT=1) timeout "$TMO" "$VM" "$OUR_IMG" >/dev/null 2>&1
  our="$(verdict)"; [ -z "$our" ] && our="TIMEOUT"
  # classify
  cls="OTHER"
  [ "$cog" = PASS ] && [ "$our" != PASS ] && cls="COGPASS-OURSFAIL"
  [ "$cog" != PASS ] && [ "$our" != PASS ] && cls="BOTHFAIL"
  [ "$cog" = PASS ] && [ "$our" = PASS ] && cls="BOTHPASS"
  [ "$cog" != PASS ] && [ "$our" = PASS ] && cls="COGFAIL-OURSPASS"
  printf '%-18s cog=%-7s ours=%-7s  %s\n' "$cls" "$cog" "$our" "$spec" | tee -a "$OUT"
done < "$LIST"
echo "=== summary ==="
awk '{print $1}' "$OUT" | sort | uniq -c | sort -rn
