#!/bin/bash
# Run a queue of non-graphics JIT-stress packages on OUR VM (test_load_image)
# against the gfx image (has SUnitRunner + FakeGUI).  These are kernel-quality
# pure-compute packages (compiler, AST, parsing, serialization, reflection,
# numerics) that ship green on stock Pharo, so any non-PASS here is a VM/JIT
# bug.  Stock-Cog baselines are run separately, only where this pass shows
# failures.
#
# Per package: stage list -> run our VM -> save <tag>{,_detail}.txt + run log,
# append one-line summary to $PROGRESS.  Resume: skips entries whose detail
# file already exists unless FORCE_RERUN=1.
set -u
IMAGE="${IMAGE:-/tmp/harness/Pharo-gfx.image}"
VM="${VM:-/Users/wohl/src/iospharo/build/test_load_image}"
OUTDIR="${OUTDIR:-/Users/wohl/src/iospharo/docs/results/jitpkg}"
TIMEOUT_S="${TIMEOUT_S:-2400}"
PROGRESS="$OUTDIR/_progress.txt"

mkdir -p "$OUTDIR"

run_one() {
    local tag="$1" classlist="/tmp/pkg_${1}.txt"
    local result="$OUTDIR/${tag}.txt" detail="$OUTDIR/${tag}_detail.txt"
    if [ -f "$detail" ] && [ "${FORCE_RERUN:-0}" != "1" ]; then
        echo "[skip] $tag (have $detail)"; return 0
    fi
    if [ ! -f "$classlist" ]; then
        echo "[err]  $tag: $classlist missing" | tee -a "$PROGRESS"; return 1
    fi
    local nclass; nclass=$(wc -l < "$classlist" | tr -d ' ')
    echo "[run]  $tag ($nclass classes)"
    cp "$classlist" /tmp/sunit_class_names.txt
    rm -f /tmp/sunit_run_completed.txt /tmp/sunit_test_results.txt /tmp/sunit_test_detail.txt
    local t0; t0=$(date +%s)
    timeout "$TIMEOUT_S" "$VM" "$IMAGE" > "$OUTDIR/${tag}_run.log" 2>&1
    local rc=$?; local t1; t1=$(date +%s); local dt=$((t1-t0))
    [ -f /tmp/sunit_test_results.txt ] && cp /tmp/sunit_test_results.txt "$result"
    if [ -f /tmp/sunit_test_detail.txt ]; then
        cp /tmp/sunit_test_detail.txt "$detail"
        local p f e last
        p=$(grep -c 'PASS$' "$detail"); f=$(grep -c 'FAIL$' "$detail"); e=$(grep -c 'ERROR$' "$detail")
        last=$(grep 'RUN:' "$OUTDIR/${tag}_run.log" | tail -1 | sed 's/.*RUN: //')
        printf "%-14s PASS=%-5s FAIL=%-4s ERROR=%-4s rc=%-3s %ss  last=%s\n" \
            "$tag" "$p" "$f" "$e" "$rc" "$dt" "$last" | tee -a "$PROGRESS"
    else
        printf "%-14s NO-DETAIL (crashed early?) rc=%s %ss\n" "$tag" "$rc" "$dt" | tee -a "$PROGRESS"
    fi
}

echo "=== JIT package queue start ===" | tee -a "$PROGRESS"
# Ordered: small high-signal first, big ones later.
# NOTE: reflectivity AND fuel excluded — both storm the JIT on our VM
# (reflectivity: metalink/bytecode-instrumentation, 1.4B+ sends, RUN-count=0;
# fuel: reflective object-graph serialization, 6B+ sends, 17% IC hit, ~1B J2J).
# Both hang past the per-test timeout. Fuel is also 474/511 ERROR on stock Cog
# (broken on this image), so low comparative signal. Investigated separately.
for tag in regex ston ast ring zincenc aigraph numberparser \
           systime strings opal microdown seq; do
    run_one "$tag"
done
echo "=== JIT package queue done ===" | tee -a "$PROGRESS"
