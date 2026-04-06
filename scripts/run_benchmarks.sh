#!/bin/bash
# Run VM performance benchmarks on both the reference Pharo VM and our VM.
#
# Usage:
#   scripts/run_benchmarks.sh                  # both VMs
#   scripts/run_benchmarks.sh --ours-only      # our VM only
#   scripts/run_benchmarks.sh --ref-only       # reference VM only
#
# Prerequisites:
#   - Reference VM: `pharo` on PATH, or set PHARO_VM=/path/to/Pharo
#     (download from https://pharo.org/download)
#   - Our VM: ./build/test_load_image (cmake --build build)
#
# Output:
#   /tmp/pharo_benchmarks_ref.txt    Reference VM results
#   /tmp/pharo_benchmarks_ours.txt   Our VM results

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BENCH_ST="$SCRIPT_DIR/run_benchmarks.st"
OUR_VM="$PROJECT_ROOT/build/test_load_image"

# Find reference Pharo VM
if [ -n "${PHARO_VM:-}" ]; then
    REF_VM="$PHARO_VM"
elif command -v pharo &>/dev/null; then
    REF_VM="pharo"
elif [ -x "/tmp/pharo-vm/Pharo.app/Contents/MacOS/Pharo" ]; then
    REF_VM="/tmp/pharo-vm/Pharo.app/Contents/MacOS/Pharo"
else
    REF_VM=""
fi

IMAGE_DIR="/tmp/pharo-bench-$$"
REF_RESULTS="/tmp/pharo_benchmarks_ref.txt"
OUR_RESULTS="/tmp/pharo_benchmarks_ours.txt"

RUN_REF=true
RUN_OURS=true

case "${1:-}" in
    --ours-only) RUN_REF=false ;;
    --ref-only)  RUN_OURS=false ;;
esac

cleanup() { rm -rf "$IMAGE_DIR"; }
trap cleanup EXIT

echo "=== Pharo VM Benchmarks ==="
echo ""

if $RUN_REF && [ -z "$REF_VM" ]; then
    echo "WARNING: Reference Pharo VM not found."
    echo "  Set PHARO_VM=/path/to/Pharo or install pharo on PATH."
    echo "  Skipping reference VM benchmarks."
    RUN_REF=false
fi

# Download fresh image
echo "[1/3] Downloading fresh Pharo 13 image..."
mkdir -p "$IMAGE_DIR"
cd "$IMAGE_DIR"
curl -sL https://get.pharo.org/64/130 | bash > /dev/null 2>&1
echo "  Image: $IMAGE_DIR/Pharo.image"

# Inject benchmark runner
echo "[2/3] Injecting benchmark runner..."
INJECT_VM=""
if [ -n "$REF_VM" ]; then
    INJECT_VM="$REF_VM"
elif [ -f ./pharo ]; then
    INJECT_VM="./pharo"
else
    echo "  ERROR: No VM available to inject benchmarks"
    exit 1
fi
"$INJECT_VM" Pharo.image eval --save "'$BENCH_ST' asFileReference fileIn" > /dev/null 2>&1
echo "  Injected: $BENCH_ST"
echo ""

# The benchmarks auto-run on image startup (registered as session handler).
# Just start the image and wait for /tmp/pharo_benchmarks.txt to appear.

wait_for_results() {
    local timeout_secs="$1"
    local elapsed=0
    while [ $elapsed -lt $timeout_secs ]; do
        sleep 5
        elapsed=$((elapsed + 5))
        if [ -f /tmp/pharo_benchmarks.txt ]; then
            return 0
        fi
    done
    return 1
}

# Run reference VM
if $RUN_REF; then
    echo "[3a] Running reference VM (Cog JIT)..."
    cp Pharo.image Pharo-ref.image
    cp Pharo.changes Pharo-ref.changes
    rm -f /tmp/pharo_benchmarks.txt
    timeout 120 "$REF_VM" Pharo-ref.image > /dev/null 2>&1 || true
    if [ -f /tmp/pharo_benchmarks.txt ]; then
        cp /tmp/pharo_benchmarks.txt "$REF_RESULTS"
        echo "  Results: $REF_RESULTS"
        echo ""
        cat "$REF_RESULTS"
    else
        echo "  ERROR: No results produced (benchmarks may need more time)"
    fi
    echo ""
fi

# Run our VM
if $RUN_OURS; then
    if [ ! -x "$OUR_VM" ]; then
        echo "ERROR: Our VM not found at $OUR_VM"
        echo "Build with: cmake -B build && cmake --build build"
        exit 1
    fi
    echo "[3b] Running our VM..."
    cp Pharo.image Pharo-ours.image
    cp Pharo.changes Pharo-ours.changes
    rm -f /tmp/pharo_benchmarks.txt
    # Our interpreter is slower — give it 10 minutes
    timeout 600 "$OUR_VM" Pharo-ours.image > /dev/null 2>&1 || true
    if [ -f /tmp/pharo_benchmarks.txt ]; then
        cp /tmp/pharo_benchmarks.txt "$OUR_RESULTS"
        echo "  Results: $OUR_RESULTS"
        echo ""
        cat "$OUR_RESULTS"
    else
        echo "  ERROR: No results produced (benchmarks may need more time)"
    fi
    echo ""
fi

# Side-by-side comparison
if $RUN_REF && $RUN_OURS && [ -f "$REF_RESULTS" ] && [ -f "$OUR_RESULTS" ]; then
    echo "=== Comparison ==="
    echo ""
    echo "  Benchmark          Reference     Ours          Ratio"
    echo "  ---------          ---------     ----          -----"

    # Extract timings and compare
    for bench in "fib(28)" "sieve x100" "sort 100K" "dict 50K" "sum 1M" "5000 factorial" "1M blocks" "1M getter" "100K alloc"; do
        ref_ms=$(grep -o "$bench = [0-9]*" "$REF_RESULTS" 2>/dev/null | grep -o '[0-9]*$' || echo "?")
        our_ms=$(grep -o "$bench = [0-9]*" "$OUR_RESULTS" 2>/dev/null | grep -o '[0-9]*$' || echo "?")
        if [ "$ref_ms" != "?" ] && [ "$our_ms" != "?" ] && [ "$ref_ms" -gt 0 ] 2>/dev/null; then
            ratio=$(echo "scale=1; $our_ms / $ref_ms" | bc 2>/dev/null || echo "?")
            printf "  %-20s %6s ms     %6s ms      %sx\n" "$bench" "$ref_ms" "$our_ms" "$ratio"
        elif [ "$ref_ms" != "?" ] || [ "$our_ms" != "?" ]; then
            printf "  %-20s %6s ms     %6s ms\n" "$bench" "$ref_ms" "$our_ms"
        fi
    done

    # tinyBenchmarks
    ref_bps=$(grep "bytecodes/sec" "$REF_RESULTS" 2>/dev/null | grep -o '^[0-9]*' || echo "?")
    our_bps=$(grep "bytecodes/sec" "$OUR_RESULTS" 2>/dev/null | grep -o '^[0-9]*' || echo "?")
    ref_sps=$(grep "sends/sec" "$REF_RESULTS" 2>/dev/null | grep -o '[0-9]* sends' | grep -o '^[0-9]*' || echo "?")
    our_sps=$(grep "sends/sec" "$OUR_RESULTS" 2>/dev/null | grep -o '[0-9]* sends' | grep -o '^[0-9]*' || echo "?")
    echo ""
    echo "  tinyBenchmarks:"
    echo "    Reference: $ref_bps bytecodes/sec, $ref_sps sends/sec"
    echo "    Ours:      $our_bps bytecodes/sec, $our_sps sends/sec"
    echo ""
fi

echo "Done."
