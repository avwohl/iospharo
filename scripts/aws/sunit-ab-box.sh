#!/usr/bin/env bash
# sunit-ab-box.sh — RUNS ON THE BOX.  Driven by scripts/aws/sunit-ab.sh.
# Expects /home/ubuntu/harness/Pharo-prepped.image (runner ALREADY filed-in
# locally by a stock pharo — see sunit-ab.sh) plus its matching .sources.
# Runs the class set in /tmp/sunit_ab_classes.txt under two configs (ON = the
# cross-method inline-J2J fix default; OFF = PHARO_T1_X86_NO_XMETHOD=1) and
# diffs the per-class totals.  No stock pharo / no prep needed on the box.
set -u
REPO=/home/ubuntu/src/iospharo
VM="$REPO/build/test_load_image"
[ -x "$VM" ] || VM="$REPO/build-opt/test_load_image"
H=/home/ubuntu/harness
IMG=$H/Pharo-prepped.image

echo "AB-START $(date +%T)  VM=$VM"

# stop idle auto-term + light multi-core keep-alive (3 loops; >3 starves sshd)
sudo systemctl stop iospharo-idle.timer 2>/dev/null
for i in 1 2 3; do ( while true; do :; done ) & done
echo "keepalive-armed"

cp /tmp/sunit_ab_classes.txt /tmp/sunit_class_names.txt
echo "classes: $(tr '\n' ' ' < /tmp/sunit_class_names.txt)"

run_one () {
  local tag="$1"; shift
  rm -f /tmp/sunit_run_completed.txt /tmp/sunit_test_results.txt \
        /tmp/sunit_test_detail.txt /tmp/sunit_unknown_classes.txt \
        "$H/startup.st" 2>/dev/null
  echo "=== RUN $tag $(date +%T) env: $* ==="
  env "$@" PHARO_MAX_STEPS=2e12 timeout 600 "$VM" "$IMG" > /tmp/run_$tag.log 2>&1
  echo "RUN-$tag-RC=$? $(date +%T)"
  cp /tmp/sunit_test_results.txt /tmp/results_$tag.txt 2>/dev/null
  grep -E "=== .*Test ===|^Total:|BATCH TOTAL|^Classes:|^Pass:|^Fail:|^Error:|^Skip:" \
    /tmp/results_$tag.txt 2>/dev/null
  echo "--- $tag unknown ---"; cat /tmp/sunit_unknown_classes.txt 2>/dev/null || echo "(none)"
}

run_one on
run_one off PHARO_T1_X86_NO_XMETHOD=1

echo "=== DIFF on vs off ==="
F='=== .*Test ===|^Total:|BATCH TOTAL|^Classes:|^Pass:|^Fail:|^Error:|^Skip:'
grep -E "$F" /tmp/results_on.txt  > /tmp/cmp_on.txt  2>/dev/null
grep -E "$F" /tmp/results_off.txt > /tmp/cmp_off.txt 2>/dev/null
if [ ! -s /tmp/cmp_on.txt ] || [ ! -s /tmp/cmp_off.txt ]; then
  echo "AB-RESULT: INCOMPLETE (on=$(wc -l </tmp/cmp_on.txt 2>/dev/null) off=$(wc -l </tmp/cmp_off.txt 2>/dev/null))"
elif diff -q /tmp/cmp_on.txt /tmp/cmp_off.txt >/dev/null; then
  echo "AB-RESULT: IDENTICAL  (x86 cross-method ON == OFF)"
else
  echo "AB-RESULT: DIFFERS"; diff /tmp/cmp_on.txt /tmp/cmp_off.txt
fi

for p in $(jobs -p); do kill "$p" 2>/dev/null; done
echo "AB-FINISHED $(date +%T)"
