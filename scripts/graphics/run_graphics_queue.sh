#!/bin/bash
# Run the graphics SUnit queue sequentially.
# Each entry: class-list file (absolute path) and a short tag for output naming.
# Reuses /tmp/harness/Pharo-gfx.image (must be pre-built with SUnitRunner +
# FakeGUI installed).
#
# Resume support: skips entries whose result file already exists, unless
# FORCE_RERUN=1 is set in the environment.

set -u
IMAGE="${IMAGE:-/tmp/harness/Pharo-gfx.image}"
VM="${VM:-/Users/wohl/src/iospharo/build/test_load_image}"
OUTDIR="${OUTDIR:-/Users/wohl/src/iospharo/docs/results}"
TIMEOUT_S="${TIMEOUT_S:-3600}"

mkdir -p "$OUTDIR"

run_one() {
    local tag="$1"
    local classlist="$2"
    local result="$OUTDIR/${tag}.txt"
    local detail="$OUTDIR/${tag}_detail.txt"
    if [ -f "$result" ] && [ "${FORCE_RERUN:-0}" != "1" ]; then
        echo "[skip] $tag (already have $result)"
        return 0
    fi
    if [ ! -f "$classlist" ]; then
        echo "[err]  $tag: classlist $classlist missing"
        return 1
    fi
    local nclass; nclass=$(wc -l < "$classlist")
    echo "[run]  $tag ($nclass classes) -> $result"
    cp "$classlist" /tmp/sunit_class_names.txt
    rm -f /tmp/sunit_run_completed.txt \
          /tmp/sunit_test_results.txt /tmp/sunit_test_detail.txt
    timeout "$TIMEOUT_S" "$VM" "$IMAGE" > "$OUTDIR/${tag}_run.log" 2>&1
    local rc=$?
    if [ -f /tmp/sunit_test_results.txt ]; then
        cp /tmp/sunit_test_results.txt "$result"
    fi
    if [ -f /tmp/sunit_test_detail.txt ]; then
        cp /tmp/sunit_test_detail.txt "$detail"
        local p=$(grep -c PASS$ "$detail")
        local f=$(grep -c FAIL$ "$detail")
        local e=$(grep -c ERROR$ "$detail")
        echo "       PASS=$p FAIL=$f ERROR=$e (rc=$rc)"
    else
        echo "       (no detail file — VM may have crashed early; rc=$rc)"
    fi
}

# Queue: tag, classlist
run_one bloc    /tmp/bloc_test_classes.txt
run_one athens  /tmp/athens_test_classes.txt
run_one cairo   /tmp/cairo_test_classes.txt
run_one plot    /tmp/plot_test_classes.txt
run_one chart   /tmp/chart_test_classes.txt

echo "[done] graphics queue complete"
