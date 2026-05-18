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
    # Our interpreter is slower — give it 10 minutes.
    # PHARO_JIT_DEFER=15: defer JIT compilation 15s.  At default 4s
    # the bench-suite hits an intermittent ~50% hang during
    # tinyBenchmarks (deferred.md A1: scheduling interaction between
    # JIT compile and bench process forked at high priority).  15s
    # makes the bench reliably complete (5/5) without measurably
    # changing the JIT speed of post-tinyBenchmarks benches.
    #
    # PHARO_NO_SISTA_DO_SPLICE=1 (arm64 only, 2026-05-17): bench-suite
    # otherwise hangs ~80% at runCollect from a scheduler race in the
    # SessionManager-startUp:-forked-at-high-priority context.  Same
    # class of bug as runSum's relinquishSlept fix (31f1c640) — see
    # deferred.md A5.  Sieve regresses 8 ms → ~120 ms with the flag
    # set (sieve uses the do-accum splice); everything else parity.
    # Override with PHARO_KEEP_SISTA_DO_SPLICE=1 to test the splice
    # path (will hang ~80% of the time on arm64).
    EXTRA_ENV=""
    if [ "$(uname -m)" = "arm64" ] && [ -z "${PHARO_KEEP_SISTA_DO_SPLICE:-}" ]; then
        EXTRA_ENV="PHARO_NO_SISTA_DO_SPLICE=1"
    fi
    timeout 600 env PHARO_JIT_DEFER="${PHARO_JIT_DEFER:-15}" $EXTRA_ENV \
        "$OUR_VM" Pharo-ours.image > /dev/null 2>&1 || true
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

# Side-by-side comparison.  The .st bench output is:
#   '<bench> = <wall> ms (cpu=<cpu> ms)'
# We use CPU time for the ratio (noise-free) and show wall too.  When
# CPU is absent (Cog reference VM, which doesn't have our primitive),
# we fall back to wall time.
if $RUN_REF && $RUN_OURS && [ -f "$REF_RESULTS" ] && [ -f "$OUR_RESULTS" ]; then
    echo "=== Comparison ==="
    echo ""
    echo "  Benchmark            Reference(cpu/wall)  Ours(cpu/wall)       Ratio(cpu)"
    echo "  ---------            -------------------  --------------       ----------"

    # Helper: extract "X ms (cpu=Y ms)" — returns "Y/X" (cpu/wall).
    # If no cpu marker, returns "X/X" (treat wall as cpu fallback).
    parse_bench() {
        local file="$1" bench="$2"
        # Use grep -F (fixed string) so parens / `+` in the bench label
        # don't get interpreted as regex metachars (`fib(28)` etc).
        local line
        line=$(grep -F "${bench} = " "$file" 2>/dev/null | head -1)
        [ -z "$line" ] && { echo "?/?"; return; }
        local wall cpu
        wall=$(echo "$line" | sed -nE 's/.* = ([0-9]+) ms.*/\1/p')
        cpu=$(echo "$line" | sed -nE 's/.*\(cpu=([0-9]+) ms\).*/\1/p')
        [ -z "$cpu" ] && cpu="$wall"
        echo "$cpu/$wall"
    }

    for bench in "fib(28)" "sieve x100" "sort 100K" "dict 50K put+get" \
                 "sum 1M" "5000 factorial" "1M blocks" "1M getter+yourself" \
                 "100K allocations" "floatSum 1M" "stringHash 100K" \
                 "collect 10x100K" "select 10x100K"; do
        ref_pair=$(parse_bench "$REF_RESULTS" "$bench")
        our_pair=$(parse_bench "$OUR_RESULTS" "$bench")
        ref_cpu="${ref_pair%/*}"; ref_wall="${ref_pair#*/}"
        our_cpu="${our_pair%/*}"; our_wall="${our_pair#*/}"
        if [ "$ref_cpu" != "?" ] && [ "$our_cpu" != "?" ] && [ "$ref_cpu" -gt 0 ] 2>/dev/null; then
            ratio=$(echo "scale=1; $our_cpu / $ref_cpu" | bc 2>/dev/null || echo "?")
            printf "  %-20s %5s/%-5s ms        %5s/%-5s ms        %sx\n" \
                "$bench" "$ref_cpu" "$ref_wall" "$our_cpu" "$our_wall" "$ratio"
        elif [ "$ref_cpu" != "?" ] || [ "$our_cpu" != "?" ]; then
            printf "  %-20s %5s/%-5s ms        %5s/%-5s ms\n" \
                "$bench" "$ref_cpu" "$ref_wall" "$our_cpu" "$our_wall"
        fi
    done

    # tinyBenchmarks — wall + cpu (when present).
    ref_line=$(grep "bytecodes/sec" "$REF_RESULTS" 2>/dev/null | head -1)
    our_line=$(grep "bytecodes/sec" "$OUR_RESULTS" 2>/dev/null | head -1)
    parse_tiny_wall() {
        local line="$1" what="$2"
        echo "$line" | sed -nE "s/^([0-9]+) bytecodes\/sec; ([0-9]+) sends\/sec.*/\1 \2/p" \
            | awk -v w="$what" '{ if (w=="bps") print $1; else print $2 }'
    }
    parse_tiny_cpu() {
        local line="$1" what="$2"
        echo "$line" | sed -nE "s/.*\(cpu=([0-9]+) bytecodes\/sec, ([0-9]+) sends\/sec\).*/\1 \2/p" \
            | awk -v w="$what" '{ if (w=="bps") print $1; else print $2 }'
    }
    ref_bps=$(parse_tiny_wall "$ref_line" bps); ref_bps=${ref_bps:-?}
    our_bps=$(parse_tiny_wall "$our_line" bps); our_bps=${our_bps:-?}
    ref_sps=$(parse_tiny_wall "$ref_line" sps); ref_sps=${ref_sps:-?}
    our_sps=$(parse_tiny_wall "$our_line" sps); our_sps=${our_sps:-?}
    ref_bps_c=$(parse_tiny_cpu "$ref_line" bps); ref_bps_c=${ref_bps_c:-?}
    our_bps_c=$(parse_tiny_cpu "$our_line" bps); our_bps_c=${our_bps_c:-?}
    ref_sps_c=$(parse_tiny_cpu "$ref_line" sps); ref_sps_c=${ref_sps_c:-?}
    our_sps_c=$(parse_tiny_cpu "$our_line" sps); our_sps_c=${our_sps_c:-?}
    echo ""
    echo "  tinyBenchmarks (wall / cpu):"
    echo "    Reference: $ref_bps / $ref_bps_c bytecodes/sec, $ref_sps / $ref_sps_c sends/sec"
    echo "    Ours:      $our_bps / $our_bps_c bytecodes/sec, $our_sps / $our_sps_c sends/sec"
    echo ""
fi

echo "Done."
