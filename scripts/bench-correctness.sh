#!/bin/bash
# bench-correctness.sh — correctness harness for JIT perf changes.
#
# For each named (bench, N), runs our VM under current default settings,
# asserts the result matches a known-good value, and reports the timing.
# Optional --ab also re-runs each with PHARO_T1_NO_INLINE_J2J=1 and
# cross-checks the two results.  Per docs/jit-may20.md: a perf claim that
# doesn't pass through this script is void.
#
# Usage:
#   scripts/bench-correctness.sh                       # default: fib 20 28 30
#   scripts/bench-correctness.sh fib 28
#   scripts/bench-correctness.sh fib 20 28 30
#   scripts/bench-correctness.sh --ab fib 20           # also run no-J2J mode
#
# Image: built once at /tmp/bench_correctness.image from /tmp/harness/Pharo.image.bak.
# Sources file: /tmp/harness/Pharo*.sources copied to /tmp/ if missing.
#
# Exit code: 0 if all asserts passed, 1 otherwise.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
OUR_VM="$PROJECT_ROOT/build/test_load_image"

HARNESS_DIR="/tmp/harness"
IMAGE_SRC="$HARNESS_DIR/Pharo.image.bak"
IMAGE="/tmp/bench_correctness.image"
INJECT_ST="/tmp/bench_correctness_inject.st"
RESULT="/tmp/bench_correctness_result.txt"
TIMEOUT_PER_RUN="${BENCH_TIMEOUT:-600}"

AB_MODE=false
if [ "${1:-}" = "--ab" ]; then
    AB_MODE=true
    shift
fi

BENCH="${1:-fib}"
shift || true
if [ $# -eq 0 ]; then
    case "$BENCH" in
        fib) set -- 20 28 30 ;;
        *)
            echo "ERROR: unknown bench '$BENCH' — supply explicit args" >&2
            exit 2 ;;
    esac
fi

# Expected results per (bench, N).
expected() {
    case "$1:$2" in
        fib:20) echo 21891 ;;
        fib:28) echo 1028457 ;;
        fib:30) echo 2692537 ;;
        *) echo "" ;;
    esac
}

# --- preflight ---
[ -x "$OUR_VM" ]   || { echo "ERROR: $OUR_VM not built. Run: cmake --build build" >&2; exit 2; }
[ -f "$IMAGE_SRC" ] || { echo "ERROR: $IMAGE_SRC not found. Stage with: curl -sL https://get.pharo.org/64/130+vm | bash" >&2; exit 2; }
[ -f "$HARNESS_DIR/pharo" ] || { echo "ERROR: $HARNESS_DIR/pharo not found. Cannot inject startup handler." >&2; exit 2; }

cp "$IMAGE_SRC" "$IMAGE"
cp "$HARNESS_DIR/Pharo.changes" "${IMAGE%.image}.changes" 2>/dev/null || true
SRCFILE=$(ls "$HARNESS_DIR"/Pharo*.sources 2>/dev/null | head -1)
if [ -n "$SRCFILE" ] && [ ! -f "/tmp/$(basename "$SRCFILE")" ]; then
    cp "$SRCFILE" /tmp/
fi

# Build the embedded items literal: #(#fib 20 #fib 28 ...)
ITEMS="#("
for N in "$@"; do
    ITEMS="$ITEMS#$BENCH $N "
done
ITEMS="${ITEMS% })"

cat > "$INJECT_ST" <<EOF
"Bench-correctness harness.  Session-startup handler runs an embedded
 items list, writes results+timings to /tmp/bench_correctness_result.txt,
 and exits.  Items literal: #(#name N #name N ...) — no file IO."
ShiftClassInstaller make: [:b |
  b name: #PharoBenchCorrectness;
    superclass: Object;
    package: 'BenchCorrectness']!

PharoBenchCorrectness class compile: 'startUp: resuming
  resuming ifTrue: [self runAsync]' classified: 'system startup'!

PharoBenchCorrectness class compile: 'runAsync
  [
    (Delay forSeconds: 2) wait.
    [self runOnce]
      on: Error
      do: [:e |
        ''/tmp/bench_correctness_result.txt'' asFileReference writeStreamDo: [:f |
          f nextPutAll: ''ERROR: ''; nextPutAll: e messageText; lf]].
    Smalltalk exitSuccess
  ] forkAt: Processor highestPriority - 1' classified: 'running'!

PharoBenchCorrectness class compile: 'runBench: name n: n
  | v s |
  s := Time millisecondClockValue.
  name = ''fib'' ifTrue: [v := n benchFib].
  v ifNil: [^ Array with: nil with: 0].
  ^ Array with: v with: (Time millisecondClockValue - s)' classified: 'running'!

PharoBenchCorrectness class compile: 'runOnce
  | items |
  items := $ITEMS.
  ''/tmp/bench_correctness_result.txt'' asFileReference writeStreamDo: [:f |
    1 to: items size by: 2 do: [:i | | name n r |
      name := (items at: i) asString.
      n := items at: i + 1.
      r := self runBench: name n: n.
      f nextPutAll: name; space; print: n; nextPutAll: '' => ''.
      f print: (r at: 1); nextPutAll: '' in ''; print: (r at: 2); nextPutAll: '' ms''; lf]]' classified: 'running'!

SessionManager default register:
  (ClassSessionHandler forClassNamed: #PharoBenchCorrectness)!
EOF

echo "[setup] injecting handler via Cog..."
# Run pharo from /tmp — Cog silently fails to persist image edits when
# invoked from $HARNESS_DIR (CWD must be a regular dir for the save).
(cd /tmp && timeout 60 "$HARNESS_DIR/pharo" "$IMAGE" eval --save \
    "'$INJECT_ST' asFileReference fileIn") > /dev/null 2>&1

run_mode() {
    local label="$1" env_kv="$2"
    rm -f "$RESULT"
    if [ -n "$env_kv" ]; then
        timeout "$TIMEOUT_PER_RUN" env $env_kv "$OUR_VM" "$IMAGE" > /dev/null 2>&1 || true
    else
        timeout "$TIMEOUT_PER_RUN" "$OUR_VM" "$IMAGE" > /dev/null 2>&1 || true
    fi
    if [ ! -f "$RESULT" ]; then
        echo "[$label] NO RESULT (timeout=${TIMEOUT_PER_RUN}s)"
        return 1
    fi
    cp "$RESULT" "$RESULT.$label"
    return 0
}

echo
echo "=== bench-correctness: $BENCH $* ==="
fail=0
run_mode "default" "" || fail=1
if $AB_MODE; then
    run_mode "no-J2J" "PHARO_T1_NO_INLINE_J2J=1" || fail=1
fi

echo
echo "=== results ==="
status=0
for N in "$@"; do
    exp=$(expected "$BENCH" "$N")
    line_def=$(grep -E "^$BENCH $N => " "$RESULT.default" 2>/dev/null || true)
    val_def=$(echo "$line_def" | sed -nE 's/.* => ([0-9-]+) in .*/\1/p')
    t_def=$(echo "$line_def"   | sed -nE 's/.* in ([0-9]+) ms.*/\1/p')

    tag="PASS"
    if [ -z "$val_def" ]; then
        tag="FAIL(missing)"; status=1
    elif [ -n "$exp" ] && [ "$val_def" != "$exp" ]; then
        tag="FAIL(expected=$exp got=$val_def)"; status=1
    fi

    if $AB_MODE; then
        line_nj=$(grep -E "^$BENCH $N => " "$RESULT.no-J2J" 2>/dev/null || true)
        val_nj=$(echo "$line_nj" | sed -nE 's/.* => ([0-9-]+) in .*/\1/p')
        t_nj=$(echo "$line_nj"   | sed -nE 's/.* in ([0-9]+) ms.*/\1/p')
        if [ "$tag" = "PASS" ] && [ -n "$val_nj" ] && [ "$val_def" != "$val_nj" ]; then
            tag="FAIL(A/B disagree default=$val_def no-J2J=$val_nj)"; status=1
        fi
        printf "  %s(%s) val=%s  default=%sms  no-J2J=%sms  %s\n" \
            "$BENCH" "$N" "${val_def:-?}" "${t_def:-?}" "${t_nj:-?}" "$tag"
    else
        printf "  %s(%s) val=%s  default=%sms  %s\n" \
            "$BENCH" "$N" "${val_def:-?}" "${t_def:-?}" "$tag"
    fi
done

exit $status
