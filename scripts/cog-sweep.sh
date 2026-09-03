#!/bin/bash
#
# Full-suite Δcog baseline against stock Cog, in batches.
#
#   scripts/cog-sweep.sh <class-list> <outdir> [pharo-dir]
#
# Why this exists: run_sunit_cog.st runs the whole class list in ONE Cog
# invocation, so a single hard hang costs the entire run.  This splits the list
# into chunks and relaunches per chunk, exactly as scripts/sunit-sweep.sh does
# for our VM, so the damage from one bad class is bounded to its chunk.
#
# THE STOCK VM MUST BE THE x86_64 BUILD, RUN UNDER ROSETTA.  The arm64 Cog
# aborts on this host wanting its JIT code zone at a fixed 0x320000000 that
# Darwin 27 will not grant in the arm64 address space; the x86_64 one gets it.
# Install with:
#
#     mkdir -p /tmp/harness-x86 && cd /tmp/harness-x86
#     arch -x86_64 /bin/bash -c 'curl -sL https://get.pharo.org/64/130+vm | bash'
#
# THE PATHS ARE NAMESPACED, so this is safe to run beside our own sweep as far
# as /tmp state goes: SUNIT_PREFIX makes run_sunit_cog.st use <prefix>_* rather
# than the /tmp/sunit_* files scripts/sunit-sweep.sh owns.  CPU contention is a
# separate question -- the suite is full of wall-clock-sensitive tests, so
# prefer an idle machine for either run.
#
# The class list wants the runner's own ordering so batch indices line up with
# our sweeps; docs/results/sweep-arm-2026-09-02/class-index-map.txt is that
# list for Pharo 13.1 build 745, and the header there says how it was made.
set -u

LIST=${1:?usage: cog-sweep.sh <class-list> <outdir> [pharo-dir]}
OUT=${2:?usage: cog-sweep.sh <class-list> <outdir> [pharo-dir]}
PHARO_DIR=${3:-/tmp/harness-x86}
RUNNER=${RUNNER:-$(cd "$(dirname "$0")" && pwd)/pharo-headless-test/run_sunit_cog.st}
STEP=${STEP:-100}
PER_BATCH_TIMEOUT=${PER_BATCH_TIMEOUT:-1800}
PREFIX=${SUNIT_PREFIX:-/tmp/cogsweep}

[ -f "$LIST" ]            || { echo "no class list: $LIST" >&2; exit 1; }
[ -f "$RUNNER" ]          || { echo "no runner: $RUNNER" >&2; exit 1; }
[ -x "$PHARO_DIR/pharo" ] || { echo "no stock VM at $PHARO_DIR/pharo" >&2; exit 1; }

# Strip comments and index columns so the same file the sweeps document can be
# handed straight in.
mkdir -p "$OUT"
grep -v '^#' "$LIST" | awk 'NF { print $NF }' > "$OUT/classes.txt"
TOTAL=$(wc -l < "$OUT/classes.txt" | tr -d ' ')
: > "$OUT/all_results.txt"
: > "$OUT/sweep.log"

{
  echo "cog sweep start $(date '+%F %H:%M:%S')"
  echo "  vm      $PHARO_DIR/pharo (x86_64 under Rosetta)"
  echo "  runner  $RUNNER"
  echo "  classes $TOTAL  step=$STEP  timeout=${PER_BATCH_TIMEOUT}s  prefix=$PREFIX"
} >> "$OUT/sweep.log"

start=1
while [ "$start" -le "$TOTAL" ]; do
    end=$(( start + STEP - 1 ))
    [ "$end" -gt "$TOTAL" ] && end=$TOTAL

    sed -n "${start},${end}p" "$OUT/classes.txt" > "${PREFIX}_test_classes.txt"
    rm -f "${PREFIX}_test_results.txt" "${PREFIX}_run_completed.txt"

    t0=$(date +%s)
    ( cd "$PHARO_DIR" && rm -f startup.st && \
      SUNIT_PREFIX="$PREFIX" timeout "$PER_BATCH_TIMEOUT" \
        arch -x86_64 ./pharo Pharo.image eval "'$RUNNER' asFileReference fileIn" ) \
        > "$OUT/batch_${start}.log" 2>&1 < /dev/null
    rc=$?
    t1=$(date +%s)

    got=0
    if [ -f "${PREFIX}_test_results.txt" ]; then
        tr '\r' '\n' < "${PREFIX}_test_results.txt" >> "$OUT/all_results.txt"
        got=$(tr '\r' '\n' < "${PREFIX}_test_results.txt" | grep -c '^Total:' || true)
    fi

    printf 'batch %5d-%-5d rc=%-3s %5ds  classes=%s\n' \
        "$start" "$end" "$rc" "$((t1-t0))" "$got" >> "$OUT/sweep.log"
    start=$(( end + 1 ))
done

echo "cog sweep done $(date '+%F %H:%M:%S')" >> "$OUT/sweep.log"

awk '/^Total:/ && NF > 2 {
    t += $2
    for (i = 3; i <= NF; i++) { split($i, kv, ":")
        if (kv[1] == "P") p += kv[2]; else if (kv[1] == "F") f += kv[2]
        else if (kv[1] == "E") e += kv[2]; else if (kv[1] == "S") s += kv[2]
        else if (kv[1] == "T") tt += kv[2] }
    c++
} END {
    printf "=== COG TOTALS ===\n  classes %d\n  tests   %d\n  PASS    %d\n  FAIL    %d\n  ERROR   %d\n  SKIP    %d\n  TIMEOUT %d\n",
           c, t, p, f, e, s, tt
    if (t > 0) printf "  rate    %.2f%%\n", 100.0 * p / t
}' "$OUT/all_results.txt" >> "$OUT/sweep.log"

tail -n 12 "$OUT/sweep.log"
