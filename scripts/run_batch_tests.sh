#!/bin/bash
set -euo pipefail
# Run test suite in batches, each with a fresh Pharo image.
# Split into small batches to avoid hitting VM limitations during
# extended test runs. Each batch starts from a fresh image.
#
# Usage: scripts/run_batch_tests.sh [--validate]
#   --validate:  run image validator before and after each batch

VALIDATE=0
VALIDATOR=""

for arg in "$@"; do
    case "$arg" in
        --validate) VALIDATE=1 ;;
    esac
done

BATCH_SIZE=25
TOTAL_CLASSES=600  # approximate
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJ_DIR="$(dirname "$SCRIPT_DIR")"
VM="$PROJ_DIR/build/test_load_image"
SCRIPT="$PROJ_DIR/scripts/pharo-headless-test/run_sunit_tests.st"
COMBINED="/tmp/sunit_test_combined.txt"
VALIDATOR="$PROJ_DIR/../validate_smalltalk_image/build/validate_smalltalk_image"

if [ $VALIDATE -eq 1 ] && [ ! -x "$VALIDATOR" ]; then
    echo "ERROR: Validator not found at $VALIDATOR"
    echo "Build it: cd ../validate_smalltalk_image && cmake -B build && cmake --build build"
    exit 1
fi

# Clear previous results
rm -f "$COMBINED" /tmp/sunit_test_detail_batch*.txt

batch=1
start=1
while [ $start -le $TOTAL_CLASSES ]; do
    end=$((start + BATCH_SIZE - 1))
    echo "=== Batch $batch: classes $start-$end ==="

    # Fresh image each batch
    rm -f /tmp/Pharo.image /tmp/Pharo.changes
    (cd /tmp && curl -sL https://get.pharo.org/64/130 | bash) 2>/dev/null || true

    if [ ! -f /tmp/Pharo.image ]; then
        echo "ERROR: Failed to download Pharo image"
        exit 1
    fi

    # Validate fresh image (pre-test)
    if [ $VALIDATE -eq 1 ]; then
        echo "  [validate] Pre-test validation..."
        if $VALIDATOR /tmp/Pharo.image 2>/dev/null; then
            echo "  [validate] Pre-test: PASS"
        else
            echo "  [validate] Pre-test: ERRORS FOUND"
        fi
    fi

    # Inject test runner (PHARO_REF_VM can override the reference VM path)
    ${PHARO_REF_VM:-pharo} /tmp/Pharo.image eval --save \
        "'$SCRIPT' asFileReference fileIn" 2>/dev/null || true

    # Write batch range
    echo "$start $end" > /tmp/sunit_batch.txt

    # Clean stale detail/results files BEFORE running
    rm -f /tmp/sunit_test_detail.txt /tmp/sunit_test_results.txt

    # Tell test runner to save image if validation is enabled
    if [ $VALIDATE -eq 1 ]; then
        touch /tmp/sunit_save_after.txt
    else
        rm -f /tmp/sunit_save_after.txt
    fi

    # Run batch with 15-minute timeout
    echo "Running batch $batch..."
    timeout 900 $VM /tmp/Pharo.image > /tmp/test_stdout.log 2>/tmp/test_stderr_batch${batch}.log || true

    # Validate saved image (post-test)
    if [ $VALIDATE -eq 1 ] && [ -f /tmp/Pharo.image ]; then
        echo "  [validate] Post-test validation..."
        if $VALIDATOR /tmp/Pharo.image 2>/dev/null; then
            echo "  [validate] Post-test: PASS"
        else
            echo "  [validate] Post-test: ERRORS FOUND"
        fi
    fi
    rm -f /tmp/sunit_save_after.txt

    # Save this batch's detail file (only if new one was created)
    if [ -f /tmp/sunit_test_detail.txt ]; then
        tr '\r' '\n' < /tmp/sunit_test_detail.txt > /tmp/sunit_test_detail_batch${batch}.txt
    else
        echo "WARNING: No detail file for batch $batch (VM may have crashed before test runner started)"
    fi

    # Report batch results
    echo "Batch $batch done:"
    tr '\r' '\n' < /tmp/sunit_test_results.txt 2>/dev/null | grep -E "^(Pass:|Fail:|Error:|Skip:|Timeout:|Classes:|BATCH)" | head -10
    echo ""

    start=$((end + 1))
    batch=$((batch + 1))
done

echo "=== All batches complete ==="
echo ""

# Combine all batch detail files
cat /tmp/sunit_test_detail_batch*.txt 2>/dev/null > "$COMBINED"

echo "Combined results in $COMBINED"
echo ""
echo "=== Summary ==="
for status in PASS FAIL ERROR SKIP TIMEOUT; do
    count=$(grep -c "	$status" "$COMBINED" 2>/dev/null || echo "0")
    echo "  $status: $count"
done
total=$(grep -v "^$" "$COMBINED" 2>/dev/null | wc -l | tr -d ' ')
echo "  TOTAL: $total"

echo ""
echo "=== Non-PASS results ==="
grep -v "	PASS" "$COMBINED" 2>/dev/null | grep -v "^$"
