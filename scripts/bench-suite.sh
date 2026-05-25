#!/bin/bash
# bench-suite.sh — JIT bench-suite measurement modelled on
# bench-correctness.sh's working pattern (handler-fork + SessionManager
# registration + cd-to-/tmp injection).  Replaces run_benchmarks.sh
# which times out for both our VM and Cog due to harness drift.
#
# Cog reference uses `--headless --no-quit` flags — without those Cog
# falls through to its CommandLineHandler help screen and never fires
# the registered startup handler.
#
# Usage:
#   scripts/bench-suite.sh                 # both VMs
#   scripts/bench-suite.sh --ours-only
#   scripts/bench-suite.sh --ref-only

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
OUR_VM="$PROJECT_ROOT/build/test_load_image"
HARNESS_DIR="/tmp/harness"
COG_VM="$HARNESS_DIR/pharo"
IMAGE_SRC="$HARNESS_DIR/Pharo.image.bak"
TIMEOUT_PER_RUN="${BENCH_TIMEOUT:-180}"

RUN_REF=true
RUN_OURS=true
case "${1:-}" in
    --ours-only) RUN_REF=false ;;
    --ref-only)  RUN_OURS=false ;;
esac

[ -x "$OUR_VM" ]    || { echo "ERROR: $OUR_VM not built (cmake --build build)" >&2; exit 2; }
[ -f "$IMAGE_SRC" ] || { echo "ERROR: $IMAGE_SRC missing (curl -sL https://get.pharo.org/64/130+vm | bash)" >&2; exit 2; }
[ -x "$COG_VM" ]    || { echo "WARNING: $COG_VM missing — skipping reference run" >&2; RUN_REF=false; }

# Stage image + handler injection.  Run pharo from /tmp (Cog silently
# fails to persist image edits when invoked from project working dir).
IMAGE="/tmp/bench_suite.image"
INJECT_ST="/tmp/bench_suite_inject.st"
RESULT_OURS="/tmp/bench_suite_ours.txt"
RESULT_REF="/tmp/bench_suite_ref.txt"

cp "$IMAGE_SRC" "$IMAGE"
cp "$HARNESS_DIR/Pharo.changes" "${IMAGE%.image}.changes" 2>/dev/null || true
SRCFILE=$(ls "$HARNESS_DIR"/Pharo*.sources 2>/dev/null | head -1)
[ -n "$SRCFILE" ] && [ ! -f "/tmp/$(basename "$SRCFILE")" ] && cp "$SRCFILE" /tmp/

cat > "$INJECT_ST" <<'EOF'
"bench-suite harness — runs the bench-suite benchmarks, writes
 results to /tmp/bench_suite_result.txt, exits.  Mirrors
 PharoBenchCorrectness pattern from bench-correctness.sh.  Handler
 stays minimal (fork + 2s delay + run + exit) to avoid the silent-
 startup-fail mode that bites PharoBenchmarkRunner."
ShiftClassInstaller make: [:b |
  b name: #PharoBenchSuite;
    superclass: Object;
    package: 'BenchSuite']!

PharoBenchSuite class compile: 'startUp: resuming
  resuming ifTrue: [self runAsync]' classified: 'system startup'!

PharoBenchSuite class compile: 'runAsync
  [
    (Delay forSeconds: 2) wait.
    [self runAll]
      on: Error
      do: [:e |
        ''/tmp/bench_suite_result.txt'' asFileReference writeStreamDo: [:f |
          f nextPutAll: ''ERROR: ''; nextPutAll: e messageText; lf]].
    Smalltalk exitSuccess
  ] forkAt: Processor highestPriority - 1' classified: 'running'!

PharoBenchSuite class compile: 'time: aBlock
  ^ aBlock timeToRun asMilliSeconds' classified: 'running'!

PharoBenchSuite class compile: 'time: aBlock
  ^ aBlock timeToRun asMilliSeconds' classified: 'running'!

PharoBenchSuite class compile: 'runAll
  ''/tmp/bench_suite_result.txt'' asFileReference writeStreamDo: [:f |
    | t |
    [f nextPutAll: ''tinyBenchmarks: ''; nextPutAll: 0 tinyBenchmarks; lf]
      on: Error do: [:e | f nextPutAll: ''tinyBenchmarks: ERROR: ''; nextPutAll: e messageText; lf].
    [t := self time: [28 benchFib]. f nextPutAll: ''fib(28) = ''; print: t; nextPutAll: '' ms''; lf]
      on: Error do: [:e | f nextPutAll: ''fib(28) = ERROR: ''; nextPutAll: e messageText; lf].
    [t := self time: [(1 to: 100) inject: 0 into: [:s :i | s + i]]. f nextPutAll: ''sieve x100 = ''; print: t; nextPutAll: '' ms''; lf]
      on: Error do: [:e | f nextPutAll: ''sieve x100 = ERROR: ''; nextPutAll: e messageText; lf].
    [t := self time: [| a | a := (1 to: 100000) collect: [:i | 100000 - i]. a sort]. f nextPutAll: ''sort 100K = ''; print: t; nextPutAll: '' ms''; lf]
      on: Error do: [:e | f nextPutAll: ''sort 100K = ERROR: ''; nextPutAll: e messageText; lf].
    [t := self time: [| d | d := Dictionary new. 1 to: 50000 do: [:i | d at: i put: i]. 1 to: 50000 do: [:i | d at: i]]. f nextPutAll: ''dict 50K = ''; print: t; nextPutAll: '' ms''; lf]
      on: Error do: [:e | f nextPutAll: ''dict 50K = ERROR: ''; nextPutAll: e messageText; lf].
    [t := self time: [(1 to: 1000000) inject: 0 into: [:s :i | s + i]]. f nextPutAll: ''sum 1M = ''; print: t; nextPutAll: '' ms''; lf]
      on: Error do: [:e | f nextPutAll: ''sum 1M = ERROR: ''; nextPutAll: e messageText; lf].
    [t := self time: [5000 factorial]. f nextPutAll: ''5000 factorial = ''; print: t; nextPutAll: '' ms''; lf]
      on: Error do: [:e | f nextPutAll: ''5000 factorial = ERROR: ''; nextPutAll: e messageText; lf].
    [t := self time: [| x | x := 0. 1 to: 1000000 do: [:i | [x := x + 1] value]]. f nextPutAll: ''1M blocks = ''; print: t; nextPutAll: '' ms''; lf]
      on: Error do: [:e | f nextPutAll: ''1M blocks = ERROR: ''; nextPutAll: e messageText; lf].
    [t := self time: [| pt | pt := 1 @ 2. 1 to: 1000000 do: [:i | pt x]]. f nextPutAll: ''1M getter+yourself = ''; print: t; nextPutAll: '' ms''; lf]
      on: Error do: [:e | f nextPutAll: ''1M getter+yourself = ERROR: ''; nextPutAll: e messageText; lf].
    [t := self time: [| arr | arr := Array new: 100000. 1 to: 100000 do: [:i | arr at: i put: Object new]]. f nextPutAll: ''100K allocations = ''; print: t; nextPutAll: '' ms''; lf]
      on: Error do: [:e | f nextPutAll: ''100K allocations = ERROR: ''; nextPutAll: e messageText; lf].
    [t := self time: [(1 to: 1000000) inject: 0.0 into: [:s :i | s + i asFloat]]. f nextPutAll: ''floatSum 1M = ''; print: t; nextPutAll: '' ms''; lf]
      on: Error do: [:e | f nextPutAll: ''floatSum 1M = ERROR: ''; nextPutAll: e messageText; lf].
    [t := self time: [| arr | arr := Array new: 100000 withAll: ''hello''. arr do: [:s | s hash]]. f nextPutAll: ''stringHash 100K = ''; print: t; nextPutAll: '' ms''; lf]
      on: Error do: [:e | f nextPutAll: ''stringHash 100K = ERROR: ''; nextPutAll: e messageText; lf].
    [t := self time: [| a | a := 1 to: 100000. 1 to: 10 do: [:i | a collect: [:x | x * 2]]]. f nextPutAll: ''collect 10x100K = ''; print: t; nextPutAll: '' ms''; lf]
      on: Error do: [:e | f nextPutAll: ''collect 10x100K = ERROR: ''; nextPutAll: e messageText; lf].
    [t := self time: [| a | a := 1 to: 100000. 1 to: 10 do: [:i | a select: [:x | x even]]]. f nextPutAll: ''select 10x100K = ''; print: t; nextPutAll: '' ms''; lf]
      on: Error do: [:e | f nextPutAll: ''select 10x100K = ERROR: ''; nextPutAll: e messageText; lf].
    f nextPutAll: ''DONE''; lf]' classified: 'running'!

SessionManager default register:
  (ClassSessionHandler forClassNamed: #PharoBenchSuite)!
EOF

echo "[setup] injecting bench-suite handler via Cog..."
# --headless is required: without it Cog drops into the world morph
# display path during `eval` and the save aborts before the file-in
# completes.  Symptom: image saves but PharoBenchSuite class is nil.
(cd /tmp && timeout 60 "$COG_VM" --headless "$IMAGE" st --save --quit \
    "$INJECT_ST") > /dev/null 2>&1

if $RUN_REF; then
    echo "[ref] running Cog reference..."
    cp "$IMAGE" "${IMAGE%.image}-ref.image"
    cp "${IMAGE%.image}.changes" "${IMAGE%.image}-ref.changes" 2>/dev/null || true
    rm -f /tmp/bench_suite_result.txt
    (cd /tmp && timeout "$TIMEOUT_PER_RUN" "$COG_VM" --headless "${IMAGE%.image}-ref.image" --no-quit) > /dev/null 2>&1 || true
    if [ -f /tmp/bench_suite_result.txt ]; then
        cp /tmp/bench_suite_result.txt "$RESULT_REF"
        echo "=== Cog reference ==="
        cat "$RESULT_REF"
        echo ""
    else
        echo "  ERROR: no Cog result produced"
    fi
fi

if $RUN_OURS; then
    echo "[ours] running our VM..."
    cp "$IMAGE" "${IMAGE%.image}-ours.image"
    cp "${IMAGE%.image}.changes" "${IMAGE%.image}-ours.changes" 2>/dev/null || true
    rm -f /tmp/bench_suite_result.txt
    PHARO_JIT_DEFER="${PHARO_JIT_DEFER:-15}" \
        timeout "$TIMEOUT_PER_RUN" "$OUR_VM" "${IMAGE%.image}-ours.image" > /dev/null 2>&1 || true
    if [ -f /tmp/bench_suite_result.txt ]; then
        cp /tmp/bench_suite_result.txt "$RESULT_OURS"
        echo "=== our VM ==="
        cat "$RESULT_OURS"
        echo ""
    else
        echo "  ERROR: no our-VM result produced"
    fi
fi

# Side-by-side compare
if $RUN_REF && $RUN_OURS && [ -f "$RESULT_REF" ] && [ -f "$RESULT_OURS" ]; then
    echo "=== Comparison (ms; ratio = ours/cog) ==="
    while read -r line; do
        bench=$(echo "$line" | sed -E 's/ = [0-9]+ ms.*//; s/^ *//')
        cog_ms=$(echo "$line" | grep -oE '[0-9]+ ms' | head -1 | awk '{print $1}')
        our_line=$(grep -F "$bench = " "$RESULT_OURS" 2>/dev/null | head -1)
        [ -z "$our_line" ] && continue
        our_ms=$(echo "$our_line" | grep -oE '[0-9]+ ms' | head -1 | awk '{print $1}')
        [ -z "$cog_ms" ] || [ -z "$our_ms" ] || [ "$cog_ms" -eq 0 ] 2>/dev/null && continue
        ratio=$(awk -v a="$our_ms" -v b="$cog_ms" 'BEGIN { printf "%.1f", a/b }')
        printf "  %-30s cog=%5s ms  ours=%5s ms  %sx\n" "$bench" "$cog_ms" "$our_ms" "$ratio"
    done < "$RESULT_REF"
fi
