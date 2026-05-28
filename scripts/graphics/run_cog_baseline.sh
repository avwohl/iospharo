#!/bin/bash
# Stock-Cog baseline for JIT-package campaign.  Uses ALTERNATE /tmp paths
# (cog_input_classes.txt / cog_output_results.txt) so it can run concurrently
# with the our-VM batch which owns /tmp/sunit_test_*.txt.
# Per tag: stage /tmp/pkg_<tag>.txt -> run stock Cog -> save <tag>_cog.txt.
set -u
PHARO="${PHARO:-/tmp/harness/pharo}"
IMAGE="${IMAGE:-/tmp/harness/Pharo.image}"
RUNNER="${RUNNER:-/tmp/run_sunit_cog_alt.st}"
OUTDIR="${OUTDIR:-/Users/wohl/src/iospharo/docs/results/jitpkg}"
TIMEOUT_S="${TIMEOUT_S:-1800}"
PROGRESS="$OUTDIR/_cog_progress.txt"
mkdir -p "$OUTDIR"

run_one() {
    local tag="$1" classlist="/tmp/pkg_${1}.txt"
    local result="$OUTDIR/${tag}_cog.txt"
    if [ -f "$result" ] && [ "${FORCE_RERUN:-0}" != "1" ]; then
        echo "[skip-cog] $tag"; return 0
    fi
    [ -f "$classlist" ] || { echo "[err] $tag: $classlist missing"; return 1; }
    cp "$classlist" /tmp/cog_input_classes.txt
    rm -f /tmp/cog_output_results.txt /tmp/cog_output_completed.txt
    local t0; t0=$(date +%s)
    timeout "$TIMEOUT_S" "$PHARO" --headless "$IMAGE" eval \
        "'$RUNNER' asFileReference fileIn" > "$OUTDIR/${tag}_cog_run.log" 2>&1
    local rc=$?; local t1; t1=$(date +%s)
    if [ -f /tmp/cog_output_results.txt ]; then
        cp /tmp/cog_output_results.txt "$result"
        local p f e
        p=$(grep -c '^Total:' "$result" >/dev/null; awk '/^Pass:/{print $2}' "$result")
        f=$(awk '/^Fail:/{print $2}' "$result"); e=$(awk '/^Error:/{print $2}' "$result")
        printf "%-14s cog: Pass=%s Fail=%s Error=%s rc=%s %ss\n" "$tag" "$p" "$f" "$e" "$rc" "$((t1-t0))" | tee -a "$PROGRESS"
    else
        printf "%-14s cog: NO-RESULT rc=%s %ss\n" "$tag" "$rc" "$((t1-t0))" | tee -a "$PROGRESS"
    fi
}

for tag in "$@"; do run_one "$tag"; done
echo "[done] cog baselines: $*" | tee -a "$PROGRESS"
