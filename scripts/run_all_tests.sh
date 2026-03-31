#!/bin/bash
# Run the full Pharo test suite in batches, auto-recovering from VM hangs.
# Each batch gets a timeout; if the VM dies, we restart from the next batch.
#
# Usage: scripts/run_all_tests.sh [--validate] [start_batch] [batch_size]
#   --validate:   run image validator before and after each batch
#   start_batch:  starting class index (default: 1)
#   batch_size:   classes per batch (default: 100)

set -e

VM="./build/test_load_image"
SCRIPT="scripts/pharo-headless-test/run_sunit_tests.st"
RESULTS="/tmp/sunit_test_results_combined.txt"
DETAILS="/tmp/sunit_test_detail_combined.txt"
BATCH_TIMEOUT=600  # 10 minutes per batch of 100 classes

VALIDATE=0
VALIDATOR="../validate_smalltalk_image/build/validate_smalltalk_image"

# Parse args
POSITIONAL=()
for arg in "$@"; do
    case "$arg" in
        --validate) VALIDATE=1 ;;
        *) POSITIONAL+=("$arg") ;;
    esac
done

START=${POSITIONAL[0]:-1}
BATCH_SIZE=${POSITIONAL[1]:-100}

if [ $VALIDATE -eq 1 ] && [ ! -x "$VALIDATOR" ]; then
    echo "ERROR: Validator not found at $VALIDATOR"
    echo "Build it: cd ../validate_smalltalk_image && cmake -B build && cmake --build build"
    exit 1
fi

# Clear combined output files
> "$RESULTS"
> "$DETAILS"

echo "=== Full Test Suite Run ===" | tee -a "$RESULTS"
echo "Start: $START, Batch size: $BATCH_SIZE, Timeout: ${BATCH_TIMEOUT}s per batch" | tee -a "$RESULTS"
[ $VALIDATE -eq 1 ] && echo "Image validation: ENABLED" | tee -a "$RESULTS"
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

    # Validate fresh image (pre-test)
    if [ $VALIDATE -eq 1 ]; then
        echo "  [validate] Pre-test validation..." | tee -a "$RESULTS"
        if $VALIDATOR /tmp/Pharo.image 2>/dev/null | tee -a "$RESULTS"; then
            echo "  [validate] Pre-test: PASS" | tee -a "$RESULTS"
        else
            echo "  [validate] Pre-test: ERRORS FOUND" | tee -a "$RESULTS"
        fi
    fi

    # Inject test runner
    touch /tmp/sunit_run_completed.txt
    /tmp/pharo --headless /tmp/Pharo.image eval --save \
        "'$(pwd)/$SCRIPT' asFileReference fileIn" > /dev/null 2>&1

    # Set batch range
    echo "$CURRENT $END" > /tmp/sunit_batch.txt
    rm -f /tmp/sunit_run_completed.txt

    # Tell test runner to save image if validation is enabled
    if [ $VALIDATE -eq 1 ]; then
        touch /tmp/sunit_save_after.txt
    else
        rm -f /tmp/sunit_save_after.txt
    fi

    # Run with timeout (capture exit code without letting set -e abort)
    EXIT_CODE=0
    timeout $BATCH_TIMEOUT $VM /tmp/Pharo.image > /dev/null 2>&1 || EXIT_CODE=$?

    if [ $EXIT_CODE -eq 124 ]; then
        echo "  [KILLED by timeout after ${BATCH_TIMEOUT}s]" | tee -a "$RESULTS"
    elif [ $EXIT_CODE -ne 0 ]; then
        echo "  [VM exited with code $EXIT_CODE]" | tee -a "$RESULTS"
    fi

    # Validate saved image (post-test)
    if [ $VALIDATE -eq 1 ] && [ -f /tmp/Pharo.image ]; then
        echo "  [validate] Post-test validation..." | tee -a "$RESULTS"
        if $VALIDATOR /tmp/Pharo.image 2>/dev/null | tee -a "$RESULTS"; then
            echo "  [validate] Post-test: PASS" | tee -a "$RESULTS"
        else
            echo "  [validate] Post-test: ERRORS FOUND" | tee -a "$RESULTS"
        fi
    fi
    rm -f /tmp/sunit_save_after.txt

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
