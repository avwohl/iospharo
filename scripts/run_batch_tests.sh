#!/bin/bash
# Run test suite in batches, each with a fresh Pharo image.
# GC compaction corruption kills the runner after ~150-200 classes,
# so we split into batches of 150 classes each.

set -e

BATCH_SIZE=150
TOTAL_CLASSES=580  # approximate
VM="./build/test_load_image"
SCRIPT="scripts/run_sunit_tests.st"
DETAIL="/tmp/sunit_test_detail.txt"

# Clear accumulated detail file
rm -f "$DETAIL"

batch=1
start=1
while [ $start -le $TOTAL_CLASSES ]; do
    end=$((start + BATCH_SIZE - 1))
    echo "=== Batch $batch: classes $start-$end ==="

    # Fresh image each batch
    cd /tmp && rm -f Pharo.image Pharo.changes
    curl -sL https://get.pharo.org/64/130 | bash 2>/dev/null
    cd - >/dev/null

    # Inject test runner
    /Users/wohl/Downloads/pharo /tmp/Pharo.image eval --save \
        "'$SCRIPT' asFileReference fileIn" 2>/dev/null

    # Write batch range
    echo "$start $end" > /tmp/sunit_batch.txt

    # Run batch with 15-minute timeout
    timeout 900 $VM /tmp/Pharo.image > /tmp/test_stdout.log 2>/tmp/test_stderr_batch${batch}.log || true

    # Report batch results
    echo "Batch $batch results:"
    tr '\r' '\n' < /tmp/sunit_test_results.txt | grep -E "^(===|Pass:|Fail:|Error:|Skip:|Timeout:|BATCH)" | head -10
    echo ""

    start=$((end + 1))
    batch=$((batch + 1))
done

echo "=== All batches complete ==="
echo "Detail file: $DETAIL"
echo ""
echo "Summary:"
for status in PASS FAIL ERROR SKIP TIMEOUT; do
    count=$(tr '\r' '\n' < "$DETAIL" | grep -c "$status" || true)
    echo "  $status: $count"
done
total=$(tr '\r' '\n' < "$DETAIL" | grep -v "^RUN" | grep -v "^$" | wc -l | tr -d ' ')
echo "  TOTAL: $total"
