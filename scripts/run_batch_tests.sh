#!/bin/bash
# Run test suite in batches, each with a fresh Pharo image.
# GC compaction corruption kills the runner after ~150-200 classes,
# so we split into batches of 150 classes each.

BATCH_SIZE=50
TOTAL_CLASSES=600  # approximate
PROJ_DIR="/Users/wohl/src/iospharo"
VM="$PROJ_DIR/build/test_load_image"
SCRIPT="$PROJ_DIR/scripts/run_sunit_tests.st"
COMBINED="/tmp/sunit_test_combined.txt"

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

    # Inject test runner
    /Users/wohl/Downloads/pharo /tmp/Pharo.image eval --save \
        "'$SCRIPT' asFileReference fileIn" 2>/dev/null || true

    # Write batch range
    echo "$start $end" > /tmp/sunit_batch.txt

    # Clean stale detail/results files BEFORE running
    rm -f /tmp/sunit_test_detail.txt /tmp/sunit_test_results.txt

    # Run batch with 15-minute timeout
    echo "Running batch $batch..."
    timeout 900 $VM /tmp/Pharo.image > /tmp/test_stdout.log 2>/tmp/test_stderr_batch${batch}.log || true

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
