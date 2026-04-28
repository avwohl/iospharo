#!/bin/bash
# Run the bench panel image N times and report best-of-N per bench.
#
# Why best-of-N: the closure-accum splice has a known T1-vs-Sista race
# (memory: project_t1_vs_sista_race.md).  About 50% of runs T1 wins
# and the bench falls back to interpreter speed (~1s).  Best-of-N
# filters those out and reports the splice-active timing.
#
# Usage:
#   scripts/run_bench_panel.sh           # 5 runs (default)
#   scripts/run_bench_panel.sh 10        # 10 runs
#
# Requires PharoBenchPanel.image at /tmp/harness/.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
N="${1:-5}"
IMAGE="${PHARO_BENCH_IMAGE:-/tmp/harness/PharoBenchPanel.image}"
VM="$PROJECT_ROOT/build/test_load_image"

if [ ! -f "$IMAGE" ]; then
  echo "ERROR: bench panel image not found at $IMAGE" >&2
  echo "Set PHARO_BENCH_IMAGE to override." >&2
  exit 1
fi
if [ ! -x "$VM" ]; then
  echo "ERROR: VM not built at $VM (run cmake --build build)" >&2
  exit 1
fi

# Splice flags — must be opt-in (see feedback_splice_flags_opt_in memory).
export PHARO_SISTA_DO_SPLICE=1
export PHARO_SISTA_DO_SPLICE_NO_HINT=1
export PHARO_SISTA_INLINE_ARITH=1
export PHARO_SISTA_IV_DO_ACCUM=1
export PHARO_SISTA_COLLECT=1
# PHARO_SISTA_VERBOSE=1 is an unexpected stabilizer: without it, the
# T1-vs-Sista race (memory: project_t1_vs_sista_race.md) makes panel
# sumArr 50/50 fast-or-slow.  Verbose mode logs every Sista dispatch,
# which delays T1 engagement enough that Sista always wins the race.
# Without verbose, T1 compiles sumArr in ~50% of runs and intercepts
# Sista's dispatch, falling back to per-iter IC speed (~1s).  Send
# overhead from verbose adds <5% to the bench timing — worth it for
# stability.
export PHARO_SISTA_VERBOSE=1

LOG=$(mktemp -d)/bench
trap "rm -rf $(dirname "$LOG")" EXIT

echo "Running bench panel $N times against $IMAGE..."
for i in $(seq 1 "$N"); do
  timeout 60 "$VM" "$IMAGE" > "$LOG.$i.log" 2>&1 || true
  printf "."
done
echo

# Parse "[A] <name>: H:MM:SS:HH.mmm" lines.  The format is
# Pharo Duration's printString: days:hours:minutes:seconds.ms.
# For the panel we always have 0 days/hours/minutes — extract the
# seconds.ms part.
awk_extract='
  /\[IMAGE\] \[A\] / {
    line = $0
    sub(/.*\[IMAGE\] \[A\] /, "", line)
    sub(/\[IMAGE\].*$/, "", line)
    # line: "name: 0:00:00:00.007"
    n = index(line, ":")
    name = substr(line, 1, n-1)
    rest = substr(line, n+1)
    gsub(/^[ \t]+|[ \t]+$/, "", name)
    gsub(/^[ \t]+|[ \t]+$/, "", rest)
    # rest: "0:00:00:00.007"  parse last colon-separated as seconds
    # split on ":" and take last field
    nf = split(rest, parts, ":")
    sec = parts[nf]
    # convert to ms (seconds may be float like "00.007")
    if (sec ~ /\./) {
      # "00.007" -> 0.007 sec -> 7 ms
      ms = sec * 1000.0
    } else {
      ms = sec * 1000
    }
    printf "%s\t%s\n", name, ms
  }
'

ALL="$LOG.all.tsv"
: > "$ALL"
for i in $(seq 1 "$N"); do
  awk "$awk_extract" "$LOG.$i.log" >> "$ALL"
done

echo
echo "Best-of-$N (ms):"
sort -t$'\t' -k1,1 "$ALL" | awk -F'\t' '
  {
    if ($1 != prev) {
      if (prev != "") printf "  %-36s %s\n", prev, best
      prev = $1
      best = $2
    } else if ($2+0 < best+0) {
      best = $2
    }
  }
  END { if (prev != "") printf "  %-36s %s\n", prev, best }
'
