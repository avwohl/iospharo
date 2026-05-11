#!/usr/bin/env bash
# Given a failing test_id from corpus.txt, find the minimal JIT_EXCLUDE
# selector list that makes the expression succeed under JIT.  Uses linear
# probing over a candidate selector pool, then bisects.
#
# Usage:
#   bash scripts/jit-diff/bisect.sh <test_id>
#
# Strategy:
#   1. Extract the test's expected pattern from corpus.txt.
#   2. Confirm it fails with no exclusions.
#   3. Build a candidate selector pool from the JIT compile log for that
#      test (every #X that got compiled).
#   4. Try ALL selectors excluded — does it work?  If not, no point.
#   5. Halve the exclusion list, find the smaller half that still works,
#      recurse until a minimal blocking set is found.
#
# Output: a one-line summary of the minimal blocking selector set,
# appended to scripts/jit-diff/results/bisect.log.

set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TEST_ID="${1:?usage: bisect.sh <test_id>}"
CORPUS="$REPO/scripts/jit-diff/corpus.txt"
IMAGE="${PHARO_DIFF_IMAGE:-/tmp/Pharo.image}"
TIMEOUT="${PHARO_DIFF_TIMEOUT:-15}"
VM="$REPO/build/test_load_image"
OUTDIR="$REPO/scripts/jit-diff/results/bisect_$TEST_ID"
mkdir -p "$OUTDIR"

# Pull the expression and expected pattern.
line=$(grep -P "^$TEST_ID\t" "$CORPUS" | head -1)
[ -z "$line" ] && { echo "test $TEST_ID not found in $CORPUS" >&2; exit 1; }
EXPR=$(echo "$line" | cut -d$'\t' -f2)
EXPECT=$(echo "$line" | cut -d$'\t' -f3-)

# Step 1: collect every JIT-compiled selector from a baseline run.
echo "[bisect] gathering compile log for $TEST_ID..." >&2
timeout "$TIMEOUT" "$VM" "$IMAGE" eval "$EXPR" > "$OUTDIR/baseline.out" 2> "$OUTDIR/baseline.err" || true
mapfile -t SELS < <(grep -oE '#[A-Za-z][A-Za-z0-9_:]*' "$OUTDIR/baseline.err" \
    "$OUTDIR/baseline.out" 2>/dev/null \
    | sort -u | sed 's/^#//')

echo "[bisect] ${#SELS[@]} candidate selectors compiled" >&2

# Helper: run with given JIT_EXCLUDE list, return 0 if expression
# produces a result matching $EXPECT (or if $EXPECT empty, matches
# baseline interp output).
INTERP_OUT="$OUTDIR/interp.out"
PHARO_NO_JIT=1 timeout "$TIMEOUT" "$VM" "$IMAGE" eval "$EXPR" > "$INTERP_OUT" 2>/dev/null
INTERP_RESULT=$(awk '
    /^=== Execution Summary ===/ { in_eval = 0 }
    in_eval && NF > 0 { print }
    /^Image args:/ { in_eval = 1 }
' "$INTERP_OUT" | tail -1)
echo "[bisect] interp ground truth: \"$INTERP_RESULT\"" >&2

works_with() {
    local exclude="$1"
    JIT_EXCLUDE="$exclude" timeout "$TIMEOUT" "$VM" "$IMAGE" eval "$EXPR" > /tmp/.bisect.$$.out 2>/dev/null
    local rc=$?
    local got
    got=$(awk '
        /^=== Execution Summary ===/ { in_eval = 0 }
        in_eval && NF > 0 { print }
        /^Image args:/ { in_eval = 1 }
    ' /tmp/.bisect.$$.out | tail -1)
    rm -f /tmp/.bisect.$$.out
    [ $rc -ne 0 ] && return 1
    if [ -n "$EXPECT" ]; then
        echo "$got" | grep -Eq "$EXPECT"
    else
        [ "$got" = "$INTERP_RESULT" ]
    fi
}

# Step 2: confirm failure with empty exclusion.
if works_with ""; then
    echo "[bisect] test $TEST_ID PASSES without any exclusion — no bisect needed" >&2
    exit 0
fi

# Step 3: try ALL selectors excluded.
ALL=$(IFS=,; echo "${SELS[*]}")
if ! works_with "$ALL"; then
    echo "[bisect] $TEST_ID still fails with EVERY compiled selector excluded — " \
         "the bug is in interpreter↔JIT transition, not a specific method" >&2
    echo "$TEST_ID: untestable (fails even with all selectors excluded)" \
        >> "$OUTDIR/../bisect.log"
    exit 2
fi

echo "[bisect] all-excluded works — bisecting..." >&2

# Step 4: bisect.  Maintain set S of "selectors known to be required for failure".
# At each step, pick a candidate from outside S, try excluding it alone with S.
# If that's enough to pass, the candidate alone was protective (skip).
# Iterate: linear probing through the pool, asking for each "is this required to
# trigger the bug?".  Final S = minimal set whose JIT-compilation triggers failure.
declare -a REQUIRED=()
declare -a POOL=("${SELS[@]}")

# To find minimal blocking set, work from the OTHER side: minimize the exclusion.
# Start with all excluded; remove selectors one at a time; if removing keeps the
# test passing, the selector wasn't load-bearing for the bug.
KEEP=("${SELS[@]}")
PROGRESS=1
while [ $PROGRESS -eq 1 ]; do
    PROGRESS=0
    for i in "${!KEEP[@]}"; do
        [ -z "${KEEP[$i]+x}" ] && continue
        TRY=()
        for j in "${!KEEP[@]}"; do
            [ "$i" = "$j" ] && continue
            [ -z "${KEEP[$j]+x}" ] && continue
            TRY+=("${KEEP[$j]}")
        done
        TRY_STR=$(IFS=,; echo "${TRY[*]}")
        if works_with "$TRY_STR"; then
            # KEEP[$i] wasn't needed in the exclusion list; drop it.
            unset 'KEEP[$i]'
            PROGRESS=1
        fi
    done
done

MIN=$(IFS=,; echo "${KEEP[*]}")
echo "[bisect] minimal blocking exclusion for $TEST_ID: $MIN" >&2
echo "$TEST_ID: minimal exclusion = $MIN" >> "$OUTDIR/../bisect.log"
