#!/usr/bin/env bash
# JIT-vs-interp differential runner.
#
# For each test in corpus.txt, runs the expression both under
# PHARO_NO_JIT=1 (interp, ground truth) and under JIT-default, then
# compares.  Outputs a markdown report and a CSV log.
#
# Usage:
#   bash scripts/jit-diff/run.sh [corpus-file] [results-dir]
#
# Defaults:
#   corpus-file = scripts/jit-diff/corpus.txt
#   results-dir = scripts/jit-diff/results
#
# Per-test timeout: PHARO_DIFF_TIMEOUT (default 30s).
# Image: PHARO_DIFF_IMAGE (default /tmp/Pharo.image).
# Test_load_image: $REPO/build/test_load_image (must already be built).

set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CORPUS="${1:-$REPO/scripts/jit-diff/corpus.txt}"
OUTDIR="${2:-$REPO/scripts/jit-diff/results}"
IMAGE="${PHARO_DIFF_IMAGE:-/tmp/Pharo.image}"
TIMEOUT="${PHARO_DIFF_TIMEOUT:-30}"
VM="$REPO/build/test_load_image"

if [ ! -x "$VM" ]; then
    echo "ERROR: $VM not built — run 'PHARO_DISABLE_LTO=1 bash scripts/build-linux.sh'" >&2
    exit 1
fi
if [ ! -f "$IMAGE" ]; then
    echo "ERROR: image $IMAGE not found" >&2
    exit 1
fi
if [ ! -f "$CORPUS" ]; then
    echo "ERROR: corpus $CORPUS not found" >&2
    exit 1
fi

mkdir -p "$OUTDIR"
CSV="$OUTDIR/results.csv"
MD="$OUTDIR/report.md"

# Trim "Image args:" prefix and standard test_load_image preamble; we
# only want the actual eval output.  The image's eval startup script
# does `Stdio stdout nextPutAll: result printString; lf; flush.` so the
# answer should be one line right before the test_load_image teardown.
extract_result() {
    local file="$1"
    # The eval prints `result printString; lf` between two markers we
    # control: the line `Image args: eval <expr>` and `=== Execution
    # Summary ===`.  Take everything strictly between those.  If
    # nothing's there (eval crashed or hung silently), the result is
    # empty — which is what we want to compare cleanly.
    awk '
        /^=== Execution Summary ===/ { in_eval = 0 }
        in_eval && NF > 0 { print }
        /^Image args:/ { in_eval = 1 }
    ' "$file" | tail -1
}

# Run a single expression once, capture output.  Args: <jit_env> <expr> <out_stdout> <out_stderr>
run_one() {
    local env_prefix="$1"
    local expr="$2"
    local stdout_file="$3"
    local stderr_file="$4"
    eval "$env_prefix timeout $TIMEOUT $VM $IMAGE eval \"$expr\"" \
        > "$stdout_file" 2> "$stderr_file"
    return $?
}

echo "test_id,status,interp_result,jit_result,interp_exit,jit_exit" > "$CSV"
{
    echo "# JIT-vs-interp differential report"
    echo
    echo "Generated: $(date -u +"%Y-%m-%dT%H:%M:%SZ")"
    echo "Image: $IMAGE"
    echo "Timeout: ${TIMEOUT}s/test"
    echo
    echo "## Results"
    echo
    printf "%-12s %-10s  %s\n" "test_id" "status" "details"
    printf "%-12s %-10s  %s\n" "------------" "----------" "-------"
} > "$MD"

PASS=0; FAIL=0; TIMEOUT_J=0; TIMEOUT_I=0; EXPECT_FAIL=0; TOTAL=0

while IFS=$'\t' read -r id expr expected || [ -n "$id" ]; do
    [ -z "$id" ] && continue
    [[ "$id" =~ ^# ]] && continue
    TOTAL=$((TOTAL+1))

    iw="$OUTDIR/.tmp_${id}_interp.out"
    iwe="$OUTDIR/.tmp_${id}_interp.err"
    jw="$OUTDIR/.tmp_${id}_jit.out"
    jwe="$OUTDIR/.tmp_${id}_jit.err"

    run_one "PHARO_NO_JIT=1" "$expr" "$iw" "$iwe"
    iexit=$?
    run_one "" "$expr" "$jw" "$jwe"
    jexit=$?

    iresult=$(extract_result "$iw")
    jresult=$(extract_result "$jw")

    status=""
    detail=""

    if [ $iexit -eq 124 ]; then
        # interp timed out — corpus issue, not a JIT bug
        status="INTERP_TIMEOUT"
        detail="interp timed out after ${TIMEOUT}s — bad test"
        TIMEOUT_I=$((TIMEOUT_I+1))
    elif [ $jexit -eq 124 ]; then
        status="JIT_TIMEOUT"
        detail="JIT hung; interp result=\"$iresult\""
        TIMEOUT_J=$((TIMEOUT_J+1))
    elif [ -n "$expected" ]; then
        # Both finished; check both against expected pattern.
        if echo "$iresult" | grep -Eq "$expected"; then
            iok=1
        else
            iok=0
        fi
        if echo "$jresult" | grep -Eq "$expected"; then
            jok=1
        else
            jok=0
        fi
        if [ $iok -eq 1 ] && [ $jok -eq 1 ]; then
            status="PASS"
            detail="$iresult"
            PASS=$((PASS+1))
        elif [ $iok -eq 0 ]; then
            status="INTERP_BAD"
            detail="interp output \"$iresult\" doesn't match /$expected/ — bad test or interp bug"
            EXPECT_FAIL=$((EXPECT_FAIL+1))
        else
            status="JIT_DIFF"
            detail="interp=\"$iresult\" jit=\"$jresult\""
            FAIL=$((FAIL+1))
        fi
    else
        # No expected pattern: differential check only.
        if [ "$iresult" = "$jresult" ]; then
            status="PASS"
            detail="$iresult"
            PASS=$((PASS+1))
        else
            status="JIT_DIFF"
            detail="interp=\"$iresult\" jit=\"$jresult\""
            FAIL=$((FAIL+1))
        fi
    fi

    printf "%-12s %-10s  %s\n" "$id" "$status" "$detail" | tee -a "$MD"
    printf '%s,%s,"%s","%s",%d,%d\n' "$id" "$status" "${iresult//\"/\"\"}" \
        "${jresult//\"/\"\"}" "$iexit" "$jexit" >> "$CSV"

    # Keep stderr files for failures, delete stdout/stderr for passes.
    if [ "$status" = "PASS" ]; then
        rm -f "$iw" "$iwe" "$jw" "$jwe"
    else
        # Rename to permanent names so they're inspectable.
        mv "$iw"  "$OUTDIR/${id}_interp.out"
        mv "$iwe" "$OUTDIR/${id}_interp.err"
        mv "$jw"  "$OUTDIR/${id}_jit.out"
        mv "$jwe" "$OUTDIR/${id}_jit.err"
    fi
done < "$CORPUS"

{
    echo
    echo "## Summary"
    echo
    echo "Total tests: $TOTAL"
    echo "PASS:           $PASS"
    echo "JIT_DIFF:       $FAIL"
    echo "JIT_TIMEOUT:    $TIMEOUT_J"
    echo "INTERP_TIMEOUT: $TIMEOUT_I"
    echo "INTERP_BAD:     $EXPECT_FAIL  (corpus issue or interp bug)"
} | tee -a "$MD"
