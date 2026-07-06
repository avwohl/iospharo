#!/usr/bin/env bash
# x86-fullsuite.sh — RUNS ON THE BOX.  Single-config full-suite run of the
# jit branch @ HEAD on native x86 Linux.  Distilled from sunit-fullsuite-ab.sh
# (same silent-runner checklist), minus the A/B second config.
set -uo pipefail
REPO=/home/ubuntu/src/iospharo
HARNESS=/home/ubuntu/harness
STOCK=/home/ubuntu/stockvm
OUT=/home/ubuntu/results
VM="$REPO/build/test_load_image"
mkdir -p "$OUT"

if [ ! -x "$STOCK/pharo" ]; then
    echo "== installing stock pharo VM =="
    rm -rf "$STOCK"; mkdir -p "$STOCK"
    (cd "$STOCK" && curl -sL https://get.pharo.org/64/vm130 | bash) >/dev/null 2>&1
fi
# ALWAYS a fully-fresh harness: image+changes+sources must come from the SAME
# fetch.  Re-downloading image/changes next to a stale .sources produced a
# source-misaligned image (argumentNames #(), garbage sourceCode) that failed
# ~700 AST/decompiler/FFI-parser tests and mimicked a VM regression
# (box run #4, 2026-07-06).
rm -rf "$HARNESS"; mkdir -p "$HARNESS"
(cd "$HARNESS" && curl -sL https://get.pharo.org/64/130 | bash >/dev/null 2>&1)

cp "$REPO/scripts/pharo-headless-test/test_classes.txt" /tmp/sunit_test_classes.txt
# full suite: no batch file, no class-names filter -> runner runs everything
rm -f /tmp/sunit_class_names.txt /tmp/sunit_batch.txt

echo "== prep: fileIn fake GUI + SUnit runner =="
rm -f "$HARNESS/startup.st"
"$STOCK/pharo" "$HARNESS/Pharo.image" eval --save \
    "'$REPO/scripts/pharo-headless-test/setup_fake_gui.st' asFileReference fileIn. '$REPO/scripts/pharo-headless-test/run_sunit_tests.st' asFileReference fileIn" 2>&1 | tail -2

echo "== run: full suite =="
rm -f /tmp/sunit_run_completed.txt "$HARNESS/startup.st" \
      /tmp/sunit_test_results.txt /tmp/sunit_test_detail.txt \
      "$HARNESS/PharoDebug.log"
env PHARO_MAX_STEPS=4000000000000 PHARO_CODE_ZONE_MB=192 \
    timeout 14400 "$VM" "$HARNESS/Pharo.image" \
    > "$OUT/run_full.log" 2>&1
rc=$?
echo "run exit=$rc"
cp /tmp/sunit_test_results.txt "$OUT/results_full.txt" 2>/dev/null || {
    echo "!! no results — check $OUT/run_full.log and PharoDebug.log"
    cp "$HARNESS/PharoDebug.log" "$OUT/pharodebug_full.log" 2>/dev/null
}
cp /tmp/sunit_test_detail.txt "$OUT/detail_full.txt" 2>/dev/null
tail -20 "$OUT/results_full.txt" 2>/dev/null
