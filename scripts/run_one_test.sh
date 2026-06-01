#!/bin/bash
# Run ONE TestCase method deterministically on our VM, and optionally compare to
# stock Cog, to separate a VM bug (fails here / passes on Cog) from an image or
# environment issue (fails on both).  Uses run_one_test.st (shared headless repo).
#
#   scripts/run_one_test.sh 'Class>>selector'
#
# Both VMs use the same model: prep a fresh image copy with --save (installs the
# OneTestRunner startUp: handler AND unregisters any full-suite SUnitRunner so it
# can't run the whole suite first), then launch the VM with no eval args so the
# handler fires on resume and runs exactly one method.
#
# Env: BASE   prepped base image           (default /tmp/harness/Pharo-jit.image)
#      VM     our VM binary                (default ./build/test_load_image)
#      PHARO  stock Cog launcher           (default /tmp/harness/pharo)
#      NOJIT=1 run our VM interpreter-only
#      COG=1   also run on stock Cog for comparison
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SPEC="${1:?usage: run_one_test.sh 'Class>>selector'}"
BASE="${BASE:-/tmp/harness/Pharo-jit.image}"
VM="${VM:-$ROOT/build/test_load_image}"
PHARO="${PHARO:-/tmp/harness/pharo}"
RUNNER="$ROOT/scripts/pharo-headless-test/run_one_test.st"
printf '%s' "$SPEC" > /tmp/sunit_one.txt

# prep a fresh image copy: install handler, unregister SUnitRunner, no inline run.
prep() {   # $1 = dest image path
  cp "$BASE" "$1"; cp "${BASE%.image}.changes" "${1%.image}.changes" 2>/dev/null
  touch /tmp/sunit_one_save_prep.txt
  timeout 150 "$PHARO" --headless "$1" eval --save \
    "'$RUNNER' asFileReference fileIn" >/dev/null 2>&1
  rm -f /tmp/sunit_one_save_prep.txt
}
result() { LC_ALL=C tr -cd '[:print:]\n' < /tmp/sunit_one_result.txt 2>/dev/null | head -1; }

echo "spec: $SPEC"

if [ "${COG:-0}" = "1" ]; then
  prep /tmp/one_cog.image
  rm -f /tmp/sunit_one_result.txt /tmp/sunit_one_done.txt
  timeout 120 "$PHARO" --headless /tmp/one_cog.image >/dev/null 2>&1
  printf 'COG   : %s\n' "$(result)"
fi

prep /tmp/one_ours.image
rm -f /tmp/sunit_one_result.txt /tmp/sunit_one_done.txt /tmp/sunit_run_completed.txt
ENVV=""; [ "${NOJIT:-0}" = "1" ] && ENVV="PHARO_NO_JIT=1"
env $ENVV timeout 120 "$VM" /tmp/one_ours.image >/dev/null 2>&1
printf 'OURS%s: %s\n' "$([ "${NOJIT:-0}" = "1" ] && echo '*' || echo ' ')" "$(result)"
