#!/bin/bash
#
# run-one-package.sh -- load ONE package of the manifest on stock Cog, then run
# its tests on stock Cog and on our VM with the sweep runner (run_sunit.st).
#
#   run-one-package.sh <outdir> <row>          row 1 is the first package
#
# Start this script through scripts/vmrun.sh (subcommand pkgs): the script
# starts VMs one after the other and has no lock and no memory guard of its
# own.
#
# Steps for the package of the row:
#   1. Copy the pristine base image.  Stock Cog loads the package with the
#      load expression of the manifest and saves the image (a download from
#      GitHub; LOAD_TIMEOUT seconds).
#   2. Stock Cog files in setup_fake_gui.st, pharo-image-fixes.st and the
#      fixes of the package (scripts/pkg-jit-test/fixes/<label>.st, entries in
#      the form of scripts/image-fixes/), and saves.
#   3. Stock Cog writes the list of the test classes of the package
#      (pkg_classes.st: the classes in the packages of the repository, and
#      the classes that the test_prefix column of the manifest names).
#   4. Stock Cog runs the classes with run_sunit.st, on a copy of the image
#      in a directory of its own.
#   5. Our VM does the same, on another copy of the same image.
#   6. One line goes to <outdir>/summary.tsv.
#
# Files of a package with the label L:
#   <outdir>/img/L.image, L.changes   the image with the package.  Deleted when
#                                     all tests pass on the two VMs (KEEP_IMAGES)
#   <outdir>/img/pharo-local/         the clones of all packages (Iceberg)
#   <outdir>/log/L.load.log, L.prep.log, L.cog.log, L.ours.log
#   <outdir>/res/L.classes.txt        the test classes
#   <outdir>/res/L.cog_test_results.txt, L.cog_test_detail.txt   (and L.ours_*)
#   <outdir>/res/L.cog.status, L.ours.status   one line for each test
#   <outdir>/res/L.diff               the tests that differ between the VMs
#
# Columns of summary.tsv:
#   label  load  classes  cog  ours  different  cog_exit  ours_exit
# "different" is the number of tests with a status that is not the same on the
# two VMs, or that only one VM ran.
#
# Environment:
#   MANIFEST      default scripts/pkg-jit-test/packages-200.tsv
#   COG           stock VM binary (default /tmp/harness14/pharo-vm/.../Pharo)
#   OURS          our VM binary (default build-rel/test_load_image)
#   BASE_IMAGE    pristine image (default /tmp/harness14/Pharo.image)
#   LOAD_TIMEOUT  seconds for the load (default 900)
#   TEST_TIMEOUT  seconds for one test run of one VM (default 1800)
#   KEEP_IMAGES   nonpass (default): keep the image of a package when a test
#                 does not pass or the VMs differ; all; none
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
OUT=${1:?usage: run-one-package.sh <outdir> <row>}
ROW=${2:?row}
MANIFEST=${MANIFEST:-$HERE/packages-200.tsv}
COG=${COG:-/tmp/harness14/pharo-vm/Pharo.app/Contents/MacOS/Pharo}
OURS=${OURS:-$REPO/build-rel/test_load_image}
BASE_IMAGE=${BASE_IMAGE:-/tmp/harness14/Pharo.image}
LOAD_TIMEOUT=${LOAD_TIMEOUT:-900}
TEST_TIMEOUT=${TEST_TIMEOUT:-1800}
KEEP_IMAGES=${KEEP_IMAGES:-nonpass}
RUNNER_EXPR="'$REPO/scripts/pharo-headless-test/run_sunit.st' asFileReference fileIn"
export PHARO_MAX_STEPS=${PHARO_MAX_STEPS:-4000000000000}
export PHARO_CODE_ZONE_MB=${PHARO_CODE_ZONE_MB:-192}
export PHARO_MAX_OLD_SPACE_MB=${PHARO_MAX_OLD_SPACE_MB:-6144}

mkdir -p "$OUT/img" "$OUT/log" "$OUT/res" "$OUT/run" || exit 2
OUT=$(cd "$OUT" && pwd)
SUMMARY=$OUT/summary.tsv
[ -f "$SUMMARY" ] || printf 'label\tload\tclasses\tcog\tours\tdifferent\tcog_exit\tours_exit\n' > "$SUMMARY"

line=$(tail -n +2 "$MANIFEST" | sed -n "${ROW}p")
[ -n "$line" ] || { echo "no row $ROW in $MANIFEST"; exit 3; }
label=$(printf '%s' "$line" | cut -f1)
repo=$(printf '%s' "$line" | cut -f3)
test_prefix=$(printf '%s' "$line" | cut -f12)
load_expr=$(printf '%s' "$line" | cut -f15)
[ -n "$label" ] && [ -n "$load_expr" ] || { echo "row $ROW has no label or no load expression"; exit 3; }
if grep -q "^$label	" "$SUMMARY"; then echo "[$ROW] $label: done before"; exit 0; fi

base_dir=$(cd "$(dirname "$BASE_IMAGE")" && pwd)
ls "$OUT/img"/*.sources >/dev/null 2>&1 || cp "$base_dir"/*.sources "$OUT/img/" \
    || { echo "no .sources file beside $BASE_IMAGE"; exit 2; }
img=$OUT/img/$label.image
cp -f "$BASE_IMAGE" "$img" && cp -f "${BASE_IMAGE%.image}.changes" "${img%.image}.changes" || exit 2

row_out() {     # $1 = load state, then the 6 other columns
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$label" "$1" "${2:--}" "${3:--}" "${4:--}" "${5:--}" "${6:--}" "${7:--}" >> "$SUMMARY"
}
# A stop from outside (the time limit of vmrun.sh) still leaves a row.
trap 'row_out KILLED; echo "[$ROW] $label: KILLED"; exit 143' TERM INT HUP

# A scratch directory as the current directory of a VM: a load and a test can
# write files there.  Our VM puts the startup.st of an eval there too.
scratch() { local d=$OUT/run/$label.$1; rm -rf "$d"; mkdir -p "$d" || exit 2; SCRATCH=$d; }

# The totals of a results file of run_sunit.st, in one line.
totals() {
    [ -f "$1" ] || { echo "NO-RESULT"; return; }
    local t
    t=$(awk '/^Total:/ && NF > 2 { t += $2
                for (i = 3; i <= NF; i++) { split($i, kv, ":"); n[kv[1]] += kv[2] } }
             END { printf "tests=%d P=%d F=%d E=%d S=%d T=%d X=%d", t, n["P"], n["F"], n["E"], n["S"], n["T"], n["X"] }' "$1")
    if grep -q '=== BATCH COMPLETE ===' "$1"; then echo "$t"; else echo "INCOMPLETE $t"; fi
}
# One line for each test of a detail file of run_sunit.st: Class>>test STATUS
status_list() {
    [ -f "$1" ] || return 0
    awk -F'\t' 'NF >= 4 { print $2 ">>" $3 " " $4 }' "$1" | LC_ALL=C sort
}

# 1. load
echo "[$ROW] $label: load"
scratch load
# Iceberg tries an SSH clone first and falls back to HTTPS after an
# authentication error.  The load asks for HTTPS at once.
( cd "$SCRATCH" && exec timeout -k 10 "$LOAD_TIMEOUT" "$COG" --headless "$img" eval --save \
    "Iceberg remoteTypeSelector: #httpsUrl. $load_expr" ) > "$OUT/log/$label.load.log" 2>&1 < /dev/null
load_rc=$?
echo "[load exit=$load_rc]" >> "$OUT/log/$label.load.log"
rm -rf "$SCRATCH"
load_state=ok
if [ "$load_rc" -ne 0 ]; then load_state="LOAD-FAIL rc=$load_rc"
elif grep -qiE 'does not understand|could not resolve|MetacelloError|fetchRequiredError|error during|nonexistent|fileNotFound' "$OUT/log/$label.load.log"; then load_state="LOAD-FAIL error-in-log"
elif cmp -s "$BASE_IMAGE" "$img"; then load_state="LOAD-FAIL image-not-saved"
fi
if [ "$load_state" != ok ]; then
    row_out "$load_state"
    echo "[$ROW] $label: $load_state (log/$label.load.log)"
    rm -f "$img" "${img%.image}.changes"
    exit 0
fi

# 2. fake GUI, image fixes, and the fixes of this package if it has a file
scratch prep
pkgfix=""
if [ -f "$HERE/fixes/$label.st" ]; then
    python3 "$REPO/scripts/gen-image-fixes.py" --one "$HERE/fixes/$label.st" "$SCRATCH/pkgfix.st" || exit 2
    pkgfix="'$SCRATCH/pkgfix.st' asFileReference fileIn. "
fi
( cd "$SCRATCH" && exec timeout -k 10 600 "$COG" --headless "$img" eval --save \
    "'$REPO/scripts/pharo-headless-test/setup_fake_gui.st' asFileReference fileIn. '$REPO/scripts/pharo-image-fixes.st' asFileReference fileIn. ${pkgfix}1" ) \
    > "$OUT/log/$label.prep.log" 2>&1 < /dev/null
prep_rc=$?
echo "[prep exit=$prep_rc]" >> "$OUT/log/$label.prep.log"
if [ "$prep_rc" -ne 0 ]; then
    row_out "PREP-FAIL rc=$prep_rc"
    echo "[$ROW] $label: PREP-FAIL rc=$prep_rc (log/$label.prep.log)"
    exit 0
fi

# 3. class list
classes=$OUT/res/$label.classes.txt
rm -f "$OUT/res/$label".*
( cd "$SCRATCH" && SUNIT_PREFIX="$OUT/res/$label.list" PKG_PREFIXES="$test_prefix" PKG_REPO="$repo" exec timeout -k 10 300 "$COG" --headless "$img" eval \
    "'$HERE/pkg_classes.st' asFileReference fileIn" ) >> "$OUT/log/$label.prep.log" 2>&1 < /dev/null
rm -rf "$SCRATCH"
if [ ! -f "$OUT/res/$label.list_class_names.txt" ]; then
    row_out "CLASS-LIST-FAIL"
    echo "[$ROW] $label: no class list (log/$label.prep.log)"
    exit 0
fi
mv "$OUT/res/$label.list_class_names.txt" "$classes"
nclasses=$(awk 'NF { n++ } END { print n + 0 }' "$classes")
if [ "$nclasses" -eq 0 ]; then
    row_out "NO-TEST-CLASSES" 0
    echo "[$ROW] $label: no test class in the repository '$repo' and none with the package prefix '$test_prefix'"
    exit 0
fi

# 4. and 5. the tests, each VM on its own copy of the image
run_vm() {      # $1 = cog|ours
    local kind=$1 pre=$OUT/res/$label.$1 d s rc
    scratch "$kind"; d=$SCRATCH
    cp "$img" "$d/pkg.image" && cp "${img%.image}.changes" "$d/pkg.changes" || exit 2
    for s in "$OUT/img"/*.sources; do ln "$s" "$d/" 2>/dev/null || cp "$s" "$d/" || exit 2; done
    # The clones of the package (test resources): the same directory for the two VMs.
    [ -d "$OUT/img/pharo-local" ] && ln -s "$OUT/img/pharo-local" "$d/pharo-local"
    cp "$classes" "${pre}_class_names.txt" || exit 2
    if [ "$kind" = cog ]; then
        ( cd "$d" && SUNIT_PREFIX="$pre" exec timeout -k 10 "$TEST_TIMEOUT" "$COG" --headless "$d/pkg.image" eval "$RUNNER_EXPR" ) \
            > "$OUT/log/$label.cog.log" 2>&1 < /dev/null
    else
        ( cd "$d" && SUNIT_PREFIX="$pre" PHARO_EVAL_NO_SUNIT_MARKER=1 exec timeout -k 10 "$TEST_TIMEOUT" "$OURS" "$d/pkg.image" eval "$RUNNER_EXPR" ) \
            > "$OUT/log/$label.ours.log" 2>&1 < /dev/null
    fi
    rc=$?
    echo "[exit=$rc]" >> "$OUT/log/$label.$kind.log"
    rm -rf "$d"
    VM_RC=$rc
}
echo "[$ROW] $label: $nclasses test classes, stock Cog"
run_vm cog;  cog_rc=$VM_RC
echo "[$ROW] $label: our VM"
run_vm ours; ours_rc=$VM_RC

# 6. summary
c=$(totals "$OUT/res/$label.cog_test_results.txt"); o=$(totals "$OUT/res/$label.ours_test_results.txt")
status_list "$OUT/res/$label.cog_test_detail.txt"  > "$OUT/res/$label.cog.status"
status_list "$OUT/res/$label.ours_test_detail.txt" > "$OUT/res/$label.ours.status"
LC_ALL=C comm -3 "$OUT/res/$label.cog.status" "$OUT/res/$label.ours.status" > "$OUT/res/$label.diff"
d=$(sed 's/^	//' "$OUT/res/$label.diff" | awk '{ print $1 }' | LC_ALL=C sort -u | awk 'END { print NR }')
nonpass=$(cat "$OUT/res/$label.cog.status" "$OUT/res/$label.ours.status" | awk '$NF != "PASS"' | awk 'END { print NR }')
row_out ok "$nclasses" "$c" "$o" "$d" "$cog_rc" "$ours_rc"
echo "[$ROW] $label: cog[$c] ours[$o] different=$d"

clean=no
case "$c$o" in *INCOMPLETE*|*NO-RESULT*) ;; *) [ "$d" -eq 0 ] && [ "$nonpass" -eq 0 ] && clean=yes ;; esac
case "$KEEP_IMAGES" in
    all)  ;;
    none) rm -f "$img" "${img%.image}.changes" ;;
    *)    [ "$clean" = yes ] && rm -f "$img" "${img%.image}.changes" ;;
esac
exit 0
