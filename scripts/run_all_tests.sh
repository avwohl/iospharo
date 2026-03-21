#!/bin/bash
# Run the full Pharo test suite in batches, auto-recovering from VM hangs.
# Each batch gets a timeout; if the VM dies, we restart from the next batch.
#
# Usage: scripts/run_all_tests.sh [start_batch] [batch_size]
#   start_batch: starting class index (default: 1)
#   batch_size: classes per batch (default: 100)

set -e

VM="./build/test_load_image"
SCRIPT="scripts/pharo-headless-test/run_sunit_tests.st"
RESULTS="/tmp/sunit_test_results_combined.txt"
DETAILS="/tmp/sunit_test_detail_combined.txt"
BATCH_TIMEOUT=600  # 10 minutes per batch of 100 classes

START=${1:-1}
BATCH_SIZE=${2:-100}

# Clear combined output files
> "$RESULTS"
> "$DETAILS"

echo "=== Full Test Suite Run ===" | tee -a "$RESULTS"
echo "Start: $START, Batch size: $BATCH_SIZE, Timeout: ${BATCH_TIMEOUT}s per batch" | tee -a "$RESULTS"
echo "" | tee -a "$RESULTS"

CURRENT=$START
TOTAL_CLASSES=0

while true; do
    END=$((CURRENT + BATCH_SIZE - 1))
    echo "--- Batch $CURRENT-$END ---" | tee -a "$RESULTS"

    # Fresh image
    cd /tmp
    curl -sL https://get.pharo.org/64/130 | bash > /dev/null 2>&1
    cd - > /dev/null

    # Inject test runner
    touch /tmp/sunit_run_completed.txt
    /tmp/pharo --headless /tmp/Pharo.image eval --save \
        "'$(pwd)/$SCRIPT' asFileReference fileIn" > /dev/null 2>&1

    # Set batch range
    echo "$CURRENT $END" > /tmp/sunit_batch.txt
    rm -f /tmp/sunit_run_completed.txt

    # Run with timeout
    timeout $BATCH_TIMEOUT $VM /tmp/Pharo.image > /dev/null 2>&1
    EXIT_CODE=$?

    if [ $EXIT_CODE -eq 124 ]; then
        echo "  [KILLED by timeout after ${BATCH_TIMEOUT}s]" | tee -a "$RESULTS"
    elif [ $EXIT_CODE -ne 0 ]; then
        echo "  [VM exited with code $EXIT_CODE]" | tee -a "$RESULTS"
    fi

    # Append results
    if [ -f /tmp/sunit_test_results.txt ]; then
        cat /tmp/sunit_test_results.txt >> "$RESULTS"
        CLASSES_DONE=$(grep -c "^=== " /tmp/sunit_test_results.txt 2>/dev/null || echo 0)
        TOTAL_CLASSES=$((TOTAL_CLASSES + CLASSES_DONE))
        echo "  Completed $CLASSES_DONE classes in this batch (total: $TOTAL_CLASSES)" | tee -a "$RESULTS"
    fi
    if [ -f /tmp/sunit_test_detail.txt ]; then
        cat /tmp/sunit_test_detail.txt >> "$DETAILS"
    fi

    # Check if batch completed (has BATCH COMPLETE marker)
    if grep -q "BATCH COMPLETE" /tmp/sunit_test_results.txt 2>/dev/null; then
        echo "  Batch completed normally" | tee -a "$RESULTS"
        # Check if we've reached the end
        if grep -q "of [0-9]*" /tmp/sunit_test_results.txt; then
            TOTAL=$(grep "^=== SUnit Test Run" /tmp/sunit_test_results.txt | sed 's/.*of \([0-9]*\).*/\1/')
            if [ $END -ge $TOTAL ] 2>/dev/null; then
                echo "=== ALL BATCHES COMPLETE ===" | tee -a "$RESULTS"
                break
            fi
        fi
    fi

    CURRENT=$((CURRENT + BATCH_SIZE))

    # Safety limit
    if [ $CURRENT -gt 2500 ]; then
        echo "=== REACHED CLASS LIMIT ===" | tee -a "$RESULTS"
        break
    fi
done

echo ""
echo "=== FINAL SUMMARY ==="
echo "Total classes tested: $TOTAL_CLASSES"
echo "Results: $RESULTS"
echo "Details: $DETAILS"

# Count totals from detail file
if [ -f "$DETAILS" ]; then
    PASS=$(grep -c "	PASS$" "$DETAILS" 2>/dev/null || echo 0)
    FAIL=$(grep -c "	FAIL$" "$DETAILS" 2>/dev/null || echo 0)
    ERROR=$(grep -c "	ERROR$" "$DETAILS" 2>/dev/null || echo 0)
    SKIP=$(grep -c "	SKIP$" "$DETAILS" 2>/dev/null || echo 0)
    TIMEOUT=$(grep -c "	TIMEOUT$" "$DETAILS" 2>/dev/null || echo 0)
    TOTAL=$((PASS + FAIL + ERROR + SKIP + TIMEOUT))
    echo "P:$PASS F:$FAIL E:$ERROR S:$SKIP T:$TIMEOUT = $TOTAL tests"
    if [ $TOTAL -gt 0 ]; then
        PASS_RATE=$(echo "scale=2; $PASS * 100 / $TOTAL" | bc)
        echo "Pass rate: ${PASS_RATE}%"
    fi
fi
