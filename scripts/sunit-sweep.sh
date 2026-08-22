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
# THE .sources FILE MUST SIT BESIDE THE IMAGE. It is not optional decoration:
# Pharo reads class comments out of it at RUNTIME, so without it
#
#     Object comment   ->   nil        (with it: a 2925-character String)
#
# and every test that touches a class comment fails in a way that looks like a
# VM bug and is not. Measured on 2026-08-18, batch 1-300 on arm64:
#
#     ClyConcreteGroupCritiquesTest   36 tests   36 ERROR
#     ClyBrowserToolValidityTest      25 tests   25 ERROR
#     ClyNotebookPageRecyclerTest      8 tests    8 ERROR
#     BCBeautifulCommentsSettingsTest  5 tests    4 ERROR
#
# 69 of that batch's 94 errors, all reducible to one nil. Two different
# signatures come out of it and neither names the cause:
#
#     MessageNotUnderstood: receiver of "ifEmpty:" is nil
#       ...ClyClassTableDecorator class>>decorateTableCell:of:      (comment ifEmpty:)
#     Error: Improper store into indexable object
#       ...WriteStream>><< ... TestCase class>>buildMicroDownUsing:withComment:
#
# The second one is why this matters beyond the four classes above: an earlier
# note in this repo attributed exactly that "Improper store" scatter to running
# the sweep on a busy machine. Some of it was the missing .sources file.
# This script now refuses to run without it rather than quietly costing ~5
# points of pass rate.
# ---------------------------------------------------------------------------
# SET THE BASELINE ENVIRONMENT, or the numbers are not comparable to the
# recorded runs and the VM looks worse than it is:
#
#     PHARO_CODE_ZONE_MB=192  PHARO_MAX_STEPS=4000000000000
#
# scripts/aws/sunit-fullsuite-ab.sh and scripts/aws/x86-fullsuite.sh both set
# these, so every recorded full-suite result was measured with them. At the
# 64 MB default the zone fills mid-suite -- observed 8005 methods compiled
# against 16810 compilations FAILED with the zone at 65535/65536 KB -- and
# every late-hot method after that runs interpreted. debug_vars.h says the same
# thing at the knob and names SHA256Test as a casualty.
#
# BATCH SIZE CHANGES THE ANSWER, and small batches make the VM look worse than
# it is. Measured on this suite:
#
#   ClyBrowserToolValidityTest     alone 25/25 PASS   in a batch of 50: 25 ERRORs
#   ClyConcreteGroupCritiquesTest  alone 36/36 PASS   in a batch of 50: 36 ERRORs
#   ClyNotebookPageRecyclerTest    alone  8/8  PASS   in a batch of 50:  8 ERRORs
#
# and the 2026-08-11 reference run, which used ONE batch of all 2052 classes,
# had all three at 100%. So the failures are not pollution from an earlier class
# -- more isolation made them worse, not better. Those classes depend on system
# state that a fuller run establishes and a 50-class window starting at index
# 151 never does; the errors are "a Set() is empty" and "receiver of ifEmpty:
# is nil" out of Calypso's environment machinery.
#
# Use STEP large enough to match how the result will be read. To compare against
# the recorded baselines, use one batch (STEP=2047) and a long timeout. Small
# batches are for bisecting a hang, not for measuring a pass rate.
#
# RUN THIS ON AN OTHERWISE IDLE MACHINE. The suite is full of timing-sensitive
# tests and the runner's watchdogs are wall-clock based, so competing load does
# not slow the run down evenly -- it converts passes into ERRORs. Measured:
# batch 1-50 scored 772/774 PASS with 0 FAIL and 0 ERROR on three consecutive
# idle runs, and the same batch during a concurrent `cmake --build -j4` produced
# a scatter of MessageNotUnderstood and "Improper store" errors in classes that
# pass 100% in isolation. If a result looks like a regression, re-run it idle
# before believing it.
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

# See the .sources note in the header. Take it from beside the image, and fall
# back to anywhere under the image's directory tree, which is where a
# `curl get.pharo.org | bash` download leaves it.
IMAGE_DIR=$(cd "$(dirname "$IMAGE")" && pwd)
SOURCES=$(ls "$IMAGE_DIR"/*.sources 2>/dev/null | head -1)
[ -n "$SOURCES" ] || SOURCES=$(find "$IMAGE_DIR" -maxdepth 3 -name '*.sources' 2>/dev/null | head -1)
[ -n "$SOURCES" ] || { echo "no .sources file for $IMAGE -- class comments would all read nil; see the header" >&2; exit 1; }

mkdir -p "$OUT"
: > "$OUT/all_results.txt"
: > "$OUT/sweep.log"

# A leftover class/method filter silently OVERRIDES the batch range -- the
# runner documents that the names file takes priority. Symptom is every batch
# reporting the same handful of classes.
#
# This clears it ONCE. Every SUnit runner invocation reads and writes the same
# fixed /tmp paths -- class-names, batch, results, detail, completed -- with no
# per-run namespacing, so starting ANY targeted class run while this sweep is
# in flight writes a filter that every SUBSEQUENT batch picks up at startup,
# and appends into the same results file. Done accidentally on 2026-08-22
# while chasing a crash mid-sweep. While a sweep runs, investigate with things
# that do not touch these paths (log reads, `sample`, eval-mode runs) or wait.
rm -f /tmp/sunit_class_names.txt /tmp/sunit_method_names.txt

{
  echo "sweep start $(date '+%F %H:%M:%S')"
  echo "  vm      $VM  ($(lipo -archs "$VM" 2>/dev/null || uname -m))"
  echo "  image   $IMAGE"
  echo "  sources $SOURCES"
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
    [ -f "$OUT/$(basename "$SOURCES")" ] || cp "$SOURCES" "$OUT/"
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
