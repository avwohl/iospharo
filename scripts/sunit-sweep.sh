#!/bin/bash
#
# Offline full-coverage SUnit sweep, one architecture per invocation.
#
#   scripts/sunit-sweep.sh <vm-binary> <prepped-image> [outdir]
#
# Why this exists alongside scripts/run_all_tests.sh: that driver curls a fresh
# image from the network for every batch and hardcodes /tmp/Pharo.image, so it
# cannot run offline, cannot run against a locally prepped image, and cannot be
# pointed at a second VM binary. This one takes both as arguments, which is what
# makes an arm-vs-x86 comparison on one machine possible.
#
# Why batches at all: a monolithic run stalls. One class can hard-hang the VM in
# a way the in-image per-test watchdog cannot interrupt, and nothing after it
# runs. Relaunching per batch from a pristine image bounds the damage to the
# batch that contains the offender.
#
# ---------------------------------------------------------------------------
# PREPARING THE IMAGE — read this, it is the part that wastes time.
#
# The image must have run_sunit_tests.st filed in AND SAVED, because the runner
# installs a SessionManager startUp: handler and the sweep relies on it firing
# on resume. Prepare it with:
#
#     <vm> fresh.image eval \
#       "'scripts/pharo-headless-test/run_sunit_tests.st' asFileReference fileIn.
#        Smalltalk snapshot: true andQuit: true"
#
# NOT with `eval --save`. Other docs in this repo describe prepping via
# `pharo image eval --save ...`; that is the STOCK Cog VM's flag. Under
# test_load_image the flag is forwarded to the image's command-line handler and
# nothing is saved -- verified by setting a global and finding it gone on the
# next launch. Use the explicit snapshot above.
#
# A correctly prepped Pharo 13 image grows from ~52 MB to ~74 MB and answers
# non-nil for `Smalltalk at: #SUnitRunner`.
# ---------------------------------------------------------------------------
set -u

VM=${1:?usage: sunit-sweep.sh <vm-binary> <prepped-image> [outdir]}
IMAGE=${2:?usage: sunit-sweep.sh <vm-binary> <prepped-image> [outdir]}
OUT=${3:-./sunit-sweep-out}

TOTAL=${TOTAL:-2047}          # concrete TestCase subclasses in a clean Pharo 13
STEP=${STEP:-50}
PER_BATCH_TIMEOUT=${PER_BATCH_TIMEOUT:-600}

[ -x "$VM" ]      || { echo "no such VM binary: $VM" >&2; exit 1; }
[ -f "$IMAGE" ]   || { echo "no such image: $IMAGE" >&2; exit 1; }
CHANGES="${IMAGE%.image}.changes"
[ -f "$CHANGES" ] || { echo "no changes file beside the image: $CHANGES" >&2; exit 1; }

mkdir -p "$OUT"
: > "$OUT/all_results.txt"
: > "$OUT/sweep.log"

# A leftover class/method filter silently OVERRIDES the batch range -- the
# runner documents that the names file takes priority. Symptom is every batch
# reporting the same handful of classes.
rm -f /tmp/sunit_class_names.txt /tmp/sunit_method_names.txt

{
  echo "sweep start $(date '+%F %H:%M:%S')"
  echo "  vm      $VM  ($(lipo -archs "$VM" 2>/dev/null || uname -m))"
  echo "  image   $IMAGE"
  echo "  total=$TOTAL step=$STEP timeout=${PER_BATCH_TIMEOUT}s"
} >> "$OUT/sweep.log"

start=1
while [ "$start" -le "$TOTAL" ]; do
    end=$(( start + STEP - 1 ))
    [ "$end" -gt "$TOTAL" ] && end=$TOTAL

    # Pristine image per batch: an earlier batch may have left processes
    # running, corrupted globals, or died mid-test.
    cp "$IMAGE"   "$OUT/run.image"
    cp "$CHANGES" "$OUT/run.changes"
    printf '%s %s' "$start" "$end" > /tmp/sunit_batch.txt
    rm -f /tmp/sunit_test_results.txt /tmp/sunit_run_completed.txt

    t0=$(date +%s)
    timeout "$PER_BATCH_TIMEOUT" "$VM" "$OUT/run.image" > "$OUT/batch_${start}.log" 2>&1
    rc=$?
    t1=$(date +%s)

    got=0
    if [ -f /tmp/sunit_test_results.txt ]; then
        tr '\r' '\n' < /tmp/sunit_test_results.txt >> "$OUT/all_results.txt"
        got=$(tr '\r' '\n' < /tmp/sunit_test_results.txt | grep -c '^Total:' || true)
    fi
    marker=no; [ -f /tmp/sunit_run_completed.txt ] && marker=yes

    printf 'batch %5d-%-5d rc=%-3s %4ds  classes=%-4s completed=%s\n' \
        "$start" "$end" "$rc" "$((t1-t0))" "$got" "$marker" >> "$OUT/sweep.log"

    start=$(( end + 1 ))
done

echo "sweep done $(date '+%F %H:%M:%S')" >> "$OUT/sweep.log"

# The runner emits TWO shapes of Total: line -- a per-class
# "Total: N P:x F:y E:z S:w" and a bare batch-level "Total: N". Counting both
# roughly doubles the test count and halves the apparent pass rate, so require
# the P/F/E/S fields.
awk '/^Total:/ && NF > 2 {
    t += $2
    for (i = 3; i <= NF; i++) { split($i, kv, ":")
        if (kv[1] == "P") p += kv[2]; else if (kv[1] == "F") f += kv[2]
        else if (kv[1] == "E") e += kv[2]; else if (kv[1] == "S") s += kv[2] }
    c++
} END {
    printf "=== TOTALS ===\n  classes %d\n  tests   %d\n  PASS    %d\n  FAIL    %d\n  ERROR   %d\n  SKIP    %d\n",
           c, t, p, f, e, s
    if (t > 0) printf "  rate    %.2f%%\n", 100.0 * p / t
}' "$OUT/all_results.txt" >> "$OUT/sweep.log"

tail -n 9 "$OUT/sweep.log"
