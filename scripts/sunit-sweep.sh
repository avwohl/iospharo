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

# The VM loads its FFI dylibs from beside its own binary.  Running a COPY of
# the binary out of its build directory therefore silently loses them:
# LibTTYTest went 5 P to 5 E on 2026-09-03 with "SymbolNotFoundError: Could not
# find symbol named: #tty_spawn searching in module: 'libtty.dylib'", which
# reads like an FFI regression and is not.  Copying the VM is a legitimate
# thing to want -- it stops a rebuild changing the binary mid-sweep -- so warn
# rather than refuse, and name the fix.
VM_DIR=$(cd "$(dirname "$VM")" && pwd)
for lib in libtty.dylib libTestLibrary.dylib; do
    if [ ! -f "$VM_DIR/$lib" ]; then
        echo "WARNING: $lib is not beside the VM ($VM_DIR)." >&2
        echo "         Tests that FFI into it will fail with SymbolNotFoundError." >&2
        echo "         Copy the build directory's *.dylib next to the VM, or run" >&2
        echo "         the VM in place from its build directory." >&2
    fi
done

mkdir -p "$OUT"
# Truncate only on a fresh sweep.  A START_AT resume appends to what the
# interrupted run already wrote.
if [ -z "${START_AT:-}" ]; then
    : > "$OUT/all_results.txt"
fi
if [ -z "${START_AT:-}" ]; then
    : > "$OUT/sweep.log"
    : > "$OUT/damaged.txt"
fi

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
#
# "Do not start a runner" is too narrow, and following it literally still cost a
# batch on 2026-09-02. The PREPPED image carries SUnitRunner's SessionManager
# startUp: handler, so ANY VM binary that resumes it starts a test run of its
# own: test_relaunch, run on the prepped image for three 8 s cycles, wrote
# /tmp/sunit_test_results.txt underneath a live x86_64 sweep and batch 1001-1050
# reported classes=1 instead of 51 -- 50 classes lost, and a foreign partial
# result merged into all_results.txt. The tell is a classes= count far below the
# batch size on an rc=0 completed=yes line. Point C++ tier tests at the pristine
# base.image instead, or wait for the sweep.
rm -f /tmp/sunit_class_names.txt /tmp/sunit_method_names.txt

{
  echo "sweep start $(date '+%F %H:%M:%S')"
  echo "  vm      $VM  ($(lipo -archs "$VM" 2>/dev/null || uname -m))"
  echo "  image   $IMAGE"
  echo "  sources $SOURCES"
  echo "  total=$TOTAL step=$STEP timeout=${PER_BATCH_TIMEOUT}s"
} >> "$OUT/sweep.log"

# Run one batch and leave its exit status in $?.  Once the runner writes
# /tmp/sunit_run_completed.txt every class in the batch has been recorded, so a
# VM still alive SHUTDOWN_GRACE seconds later is wedged in shutdown and has
# nothing left to contribute.  The 2026-09-02 x86_64 sweep burned its whole
# 1800 s budget exactly that way -- batch 1801-1850 came back rc=124 with
# classes=51 completed=yes, i.e. 1800 s spent for a batch that had finished its
# work in a fraction of it.  Kill it at the marker rather than waiting the
# timeout out.  timeout(1) is still the outer bound for a batch that hangs
# BEFORE finishing, which is the case the marker cannot see.
SHUTDOWN_GRACE=${SHUTDOWN_GRACE:-30}
run_batch() {                       # $1 = log path
    local log=$1 grace=0 pid
    timeout "$PER_BATCH_TIMEOUT" "$VM" "$OUT/run.image" > "$log" 2>&1 < /dev/null &
    pid=$!
    while kill -0 "$pid" 2>/dev/null; do
        if [ -f /tmp/sunit_run_completed.txt ]; then
            grace=$(( grace + 1 ))
            if [ "$grace" -ge "$SHUTDOWN_GRACE" ]; then
                # kill BOTH: killing timeout(1) orphans the VM it wraps.  The
                # image path is unique to this sweep's outdir, so the pkill
                # pattern cannot reach another sweep or a package run.
                kill -9 "$pid" 2>/dev/null
                pkill -9 -f "$OUT/run.image" 2>/dev/null
                wait "$pid" 2>/dev/null
                return 137
            fi
        fi
        sleep 1
    done
    wait "$pid"
}

# START_AT resumes a sweep that was cut short -- a spot reclaim, a kill, a
# reboot.  Everything before it is left alone, so point it at the first index
# the previous run did not finish and append to the same outdir.  With
# START_AT set the results files are NOT truncated (see above), because the
# whole point is to keep what the earlier attempt already produced.
start=${START_AT:-1}
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
    run_batch "$OUT/batch_${start}.log"
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

    # Record damaged batches for the recovery pass below.  The runner walks the
    # batch in index order, so `got` reported classes means indices
    # [start, start+got-1] are done and [start+got, end] never ran.  Verified
    # against the 2026-09-02 arm64 sweep: batch 1901-1950 reported got=11 and
    # its last class was TonelWriterV3Test, which is index 1911 = start+got-1.
    # (The runner reports end-start+2 classes for a healthy batch -- it runs one
    # past `end` -- so this compares against end-start+1 and under-detects a
    # batch that lost exactly one class rather than false-positiving on every
    # healthy one.)
    if [ "$got" -lt "$(( end - start + 1 ))" ]; then
        printf '%s %s\n' "$(( start + got ))" "$end" >> "$OUT/damaged.txt"
    fi

    start=$(( end + 1 ))
done

# ---------------------------------------------------------------------------
# RECOVERY PASS.  One class can take a whole batch with it -- a runaway
# allocation storm aborts the VM (rc=134) and every class after it in that
# batch is simply never run.  Twice now that has cost ~40 classes and hidden
# whether a residual class "went clean" or never ran at all (2026-08-22 batch
# 601-650, 2026-09-02 batch 1901-1950, the latter taking all 27 trait tests).
#
# Re-run only the indices that never reported, in small chunks, so the same
# storm costs ONE chunk instead of a whole batch.  Results go to a separate
# file: the batch-size caveat in the header is real, and merging a
# RETRY_STEP-sized run into the main totals would silently change the
# denominator's meaning.  Set RETRY_DAMAGED=0 to skip.
RETRY_DAMAGED=${RETRY_DAMAGED:-1}
RETRY_STEP=${RETRY_STEP:-5}
if [ "$RETRY_DAMAGED" = "1" ] && [ -s "$OUT/damaged.txt" ]; then
    : > "$OUT/retry_results.txt"
    echo "recovery pass $(date '+%F %H:%M:%S') (step=$RETRY_STEP)" >> "$OUT/sweep.log"
    while read -r rstart rend; do
        s2=$rstart
        while [ "$s2" -le "$rend" ]; do
            e2=$(( s2 + RETRY_STEP - 1 ))
            [ "$e2" -gt "$rend" ] && e2=$rend

            cp "$IMAGE"   "$OUT/run.image"
            cp "$CHANGES" "$OUT/run.changes"
            printf '%s %s' "$s2" "$e2" > /tmp/sunit_batch.txt
            rm -f /tmp/sunit_test_results.txt /tmp/sunit_run_completed.txt

            t0=$(date +%s)
            # run_batch redirects the VM's stdin from /dev/null, which this site
            # needs regardless: the VM must not eat the `read` loop's stdin.
            run_batch "$OUT/retry_${s2}.log"
            rc=$?
            t1=$(date +%s)

            got=0
            if [ -f /tmp/sunit_test_results.txt ]; then
                tr '\r' '\n' < /tmp/sunit_test_results.txt >> "$OUT/retry_results.txt"
                got=$(tr '\r' '\n' < /tmp/sunit_test_results.txt | grep -c '^Total:' || true)
            fi
            printf 'retry %5d-%-5d rc=%-3s %4ds  classes=%-4s\n' \
                "$s2" "$e2" "$rc" "$((t1-t0))" "$got" >> "$OUT/sweep.log"

            s2=$(( e2 + 1 ))
        done
    done < "$OUT/damaged.txt"
fi

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

if [ -s "$OUT/retry_results.txt" ]; then
    awk '/^Total:/ && NF > 2 {
        t += $2
        for (i = 3; i <= NF; i++) { split($i, kv, ":")
            if (kv[1] == "P") p += kv[2]; else if (kv[1] == "F") f += kv[2]
            else if (kv[1] == "E") e += kv[2]; else if (kv[1] == "S") s += kv[2] }
        c++
    } END {
        printf "=== RECOVERED (classes the main pass never ran) ===\n  classes %d\n  tests   %d\n  PASS    %d\n  FAIL    %d\n  ERROR   %d\n  SKIP    %d\n",
               c, t, p, f, e, s
    }' "$OUT/retry_results.txt" >> "$OUT/sweep.log"
fi

tail -n 18 "$OUT/sweep.log"
