#!/bin/bash
# Classify one (or more) SUnit class as JIT-hang / VM-hang / not-a-hang.
# For each class: run it alone JIT-on then PHARO_NO_JIT=1, each with a hard
# timeout, recording exit code (124=timed out=hang) and the Total: line.
#
#   scripts/classify_hanger.sh Class1 [Class2 ...]
# Env: IMG (default /tmp/harness/Pharo-dbg.image), TMO (default 75),
#      OUT (default /tmp/classify.txt)
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VM="$ROOT/build/test_load_image"
IMG="${IMG:-/tmp/harness/Pharo-dbg.image}"
TMO="${TMO:-75}"
OUT="${OUT:-/tmp/classify.txt}"
: > "$OUT"
res() { tr '\r' '\n' < /tmp/sunit_test_results.txt 2>/dev/null | grep '^Total:' | head -1 | tr -cd '[:alnum:]:_ '; }
for cls in "$@"; do
  printf '%s\n' "$cls" > /tmp/sunit_class_names.txt
  rm -f /tmp/sunit_test_results.txt /tmp/sunit_run_completed.txt
  timeout "$TMO" "$VM" "$IMG" > "/tmp/cl_${cls}_jit.log" 2>&1; je=$?
  jr="$(res)"
  rm -f /tmp/sunit_test_results.txt /tmp/sunit_run_completed.txt
  PHARO_NO_JIT=1 timeout "$TMO" "$VM" "$IMG" > "/tmp/cl_${cls}_nojit.log" 2>&1; ne=$?
  nr="$(res)"
  verdict=VM-hang
  [ "$je" = "124" ] && [ "$ne" != "124" ] && verdict=JIT-hang
  [ "$je" != "124" ] && [ "$ne" != "124" ] && verdict=NOT-a-hang
  printf '%-44s JIT exit=%-3s [%s] | NOJIT exit=%-3s [%s] => %s\n' \
    "$cls" "$je" "$jr" "$ne" "$nr" "$verdict" >> "$OUT"
done
rm -f /tmp/sunit_class_names.txt
cat "$OUT"
