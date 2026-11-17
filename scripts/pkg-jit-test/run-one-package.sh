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
# A package can have these files in this directory:
#   patches/L.patch   a git diff of the repository of the package.  The load
#                     then comes from a local clone with the patch (for a
#                     defect that stops the load)
#   locks/L.txt       dependencies that need the patch of another row.  Each
#                     line: the baseline name of the dependency and the label
#                     of that row.  The load takes the dependency from a
#                     local clone with the patch (a Metacello lock)
#   fixes/L.st        entries in the form of scripts/image-fixes/, filed in
#                     after the load (for a defect in the code or in a test)
#   fixtures/L.txt    names of test classes that are fixtures: another test
#                     runs them, and some fail on purpose.  One name for each
#                     line; a line that starts with # has the proof.  A test
#                     class in a package with TestResources in its name is a
#                     fixture with no entry here.  res/L.list_fixtures.txt
#                     has the classes that the list left out.
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
#   label  state  classes  cog  ours  different  cog_exit  ours_exit
# state: ok (the two VMs ran the full class list), COG-INCOMPLETE,
# OURS-INCOMPLETE, BOTH-INCOMPLETE (a VM stopped in the middle), LOAD-FAIL,
# PATCH-FAIL, PREP-FAIL, CLASS-LIST-FAIL, NO-TEST-CLASSES, and the states of
# a run that was stopped from outside and runs again at the next call:
# KILLED, SETUP-FAIL, MEMORY.
# "different" is the number of tests with a status that is not the same on the
# two VMs, or that only one VM ran.  It has a value only for the state ok.
#
# Environment:
#   MANIFEST      default scripts/pkg-jit-test/packages-200.tsv
#   COG           stock VM binary (default /tmp/harness14/pharo-vm/.../Pharo)
#   OURS          our VM binary (default build-rel/test_load_image)
#   BASE_IMAGE    pristine image (default /tmp/harness14/Pharo.image)
#   LOAD_TIMEOUT  seconds for the load (default 900)
#   TEST_TIMEOUT  seconds for one test run of one VM (default 1800)
#   PKG_FIXES_FILE  another fixes file for the row (default
#                 scripts/pkg-jit-test/fixes/<label>.st); for a run of one
#                 row with an earlier state of its fixes
#   PKG_REDO      1 = run a package again that has a row in summary.tsv
#   PKG_REUSE_LOAD 1 = with PKG_REDO: start from the image that the load of an
#                 earlier run left (no download)
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
# Stock Cog gets the old-space limit of our VM.  With no limit, a recursion
# with no end (a MuTalk test) made stock Cog use 13 GB in 12 seconds.
COG_MEM=--maxOldSpaceSize=${PHARO_MAX_OLD_SPACE_MB}M
# When stock Cog stops with a VM error, it prints the stack of each process.
# For a recursion with no end that was some million lines (500 MB in 10
# minutes, and the VM was not done).
COG_FRAMES=--maxFramesToLog=200

mkdir -p "$OUT/img" "$OUT/log" "$OUT/res" "$OUT/run" || exit 2
OUT=$(cd "$OUT" && pwd)
SUMMARY=$OUT/summary.tsv
[ -f "$SUMMARY" ] || printf 'label\tstate\tclasses\tcog\tours\tdifferent\tcog_exit\tours_exit\n' > "$SUMMARY"

line=$(tail -n +2 "$MANIFEST" | sed -n "${ROW}p")
[ -n "$line" ] || { echo "no row $ROW in $MANIFEST"; exit 3; }
label=$(printf '%s' "$line" | cut -f1)
owner=$(printf '%s' "$line" | cut -f2)
repo=$(printf '%s' "$line" | cut -f3)
branch=$(printf '%s' "$line" | cut -f4)
test_prefix=$(printf '%s' "$line" | cut -f12)
load_expr=$(printf '%s' "$line" | cut -f15)
[ -n "$label" ] && [ -n "$load_expr" ] || { echo "row $ROW has no label or no load expression"; exit 3; }

# A row is final when the package got a result.  A row of a run that was
# stopped from outside (KILLED, SETUP-FAIL, MEMORY) is not final.
old_state=$(awk -F'\t' -v l="$label" '$1 == l { s = $2 } END { print s }' "$SUMMARY")
if [ -n "$old_state" ]; then
    case "$old_state" in
        KILLED*|SETUP-FAIL*|MEMORY*) redo=1 ;;
        *) redo=${PKG_REDO:-0} ;;
    esac
    if [ "$redo" != 1 ]; then echo "[$ROW] $label: done before ($old_state)"; exit 0; fi
    awk -F'\t' -v l="$label" '$1 != l' "$SUMMARY" > "$SUMMARY.new" && mv "$SUMMARY.new" "$SUMMARY"
fi

ROW_DONE=0
row_out() {     # $1 = state, then the 6 other columns.  One row for one run.
    [ "$ROW_DONE" = 1 ] && return 0
    ROW_DONE=1
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$label" "$1" "${2:--}" "${3:--}" "${4:--}" "${5:--}" "${6:--}" "${7:--}" >> "$SUMMARY"
}
fail_setup() { row_out "SETUP-FAIL $1"; echo "[$ROW] $label: SETUP-FAIL $1"; exit 2; }

# A stop from outside (the time limit of vmrun.sh) stops the VM of the
# current step and leaves a row.  Each VM runs in the background and the
# script waits for it: bash runs a trap at once only during a wait.
VM_PID=""
on_stop() {
    [ -n "$VM_PID" ] && kill "$VM_PID" 2>/dev/null
    row_out KILLED
    echo "[$ROW] $label: KILLED"
    exit 143
}
trap on_stop TERM INT HUP

# Before each VM start: the memory check of vmrun.sh (VMRUN_CHECK is the path
# of vmrun.sh when vmrun.sh started this script).
memory_ok() {
    [ -z "${VMRUN_CHECK:-}" ] && return 0
    "$VMRUN_CHECK" check && return 0
    row_out "MEMORY refused before $1"
    echo "[$ROW] $label: the memory check refused the start of a VM ($1)"
    exit 4
}

base_dir=$(cd "$(dirname "$BASE_IMAGE")" && pwd)
ls "$OUT/img"/*.sources >/dev/null 2>&1 || cp "$base_dir"/*.sources "$OUT/img/" || fail_setup "no .sources file beside $BASE_IMAGE"
img=$OUT/img/$label.image
cp -f "$BASE_IMAGE" "$img" && cp -f "${BASE_IMAGE%.image}.changes" "${img%.image}.changes" || fail_setup "copy of the base image"
rm -f "$OUT/res/$label".* "$OUT/log/$label".*

# A scratch directory as the current directory of a VM: a load and a test can
# write files there.  Our VM puts the startup.st of an eval there too.
scratch() { local d=$OUT/run/$label.$1; rm -rf "$d"; mkdir -p "$d" || fail_setup "mkdir $d"; SCRATCH=$d; }

# A log above 20 MB keeps its first 5 MB and its last 1 MB.
trim_log() {
    local f=$1 n
    [ -f "$f" ] || return 0
    n=$(wc -c < "$f")
    [ "$n" -gt 20000000 ] || return 0
    { head -c 5000000 "$f"; printf '\n[... %s bytes cut by run-one-package.sh ...]\n' "$((n - 6000000))"; tail -c 1000000 "$f"; } > "$f.cut" && mv "$f.cut" "$f"
}

# The totals of a results file of run_sunit.st, in one line.
totals() {
    [ -f "$1" ] || { echo "NO-RESULT"; return; }
    local t
    t=$(awk '/^Total:/ && NF > 2 { t += $2
                for (i = 3; i <= NF; i++) { split($i, kv, ":"); n[kv[1]] += kv[2] } }
             END { printf "tests=%d P=%d F=%d E=%d S=%d T=%d X=%d", t, n["P"], n["F"], n["E"], n["S"], n["T"], n["X"] }' "$1")
    if grep -q '=== BATCH COMPLETE ===' "$1"; then echo "$t"; else echo "INCOMPLETE $t"; fi
}
# The number of results in the class totals that are not a pass.  This count
# has the errors that have no line in the detail file (a class whose
# buildSuite fails).
nonpass_total() {
    [ -f "$1" ] || { echo 0; return; }
    awk '/^Total:/ && NF > 2 { for (i = 3; i <= NF; i++) { split($i, kv, ":"); if (kv[1] != "P") n += kv[2] } } END { print n + 0 }' "$1"
}
# One line for each test of a detail file of run_sunit.st: Class>>test STATUS
status_list() {
    [ -f "$1" ] || return 0
    awk -F'\t' 'NF == 5 && $4 ~ /^(PASS|FAIL|ERROR|SKIP|TIMEOUT|XFAIL)$/ { print $2 ">>" $3 " " $4 }' "$1" | LC_ALL=C sort
}

# A load in a headless image must not wait for an answer.  Metacello gives a
# warning to its default action when the load expression has no onWarning:
# block.  The default action of MCMergeOrLoadWarning (the image has changes
# in a package, and the load has another version of that package) opens a
# dialog and waits: the load of Hyperspace, which loads Zinc from GitHub,
# stopped there until the time limit.  With this block the load takes the
# incoming version (the answer true), as onConflict: useIncoming does, and
# each other warning goes to the log.
#   headless_load <load expression>
headless_load() {
    printf '%s' "$1" | sed 's/; *load\([:.]\)/; onWarning: [ :w | Stdio stdout nextPutAll: #WARNING; nextPut: Character space; nextPutAll: w class name; nextPut: Character space; nextPutAll: w messageText asString utf8Encoded; lf; flush. (w class name = #MCMergeOrLoadWarning) ifTrue: [ w resume: true ] ifFalse: [ w resume ] ]; load\1/g'
}
# A baseline of a repository can name the repository itself for another
# baseline of it.  In the clone, such a reference names the clone, so each
# part of the package comes from the patched code.
#   self_references <owner> <repo> <branch> <directory>
self_references() {
    local f
    grep -rl --include='*.st' "github://$1/$2:$3" "$4" 2>/dev/null | while IFS= read -r f; do
        sed -i '' "s|github://$1/$2:$3|gitlocal://$4|g" "$f" || return 1
    done
}
# A local clone of a repository that has a patch of this directory as a
# commit.  The name of the directory is the name of the Iceberg repository.
#   patched_clone <label of the patch file> <owner> <repo> <branch> <directory>
patched_clone() {
    rm -rf "$5"
    git clone -q --branch "$4" "https://github.com/$2/$3.git" "$5" >> "$OUT/log/$label.patch.log" 2>&1 \
        && git -C "$5" apply "$HERE/patches/$1.patch" >> "$OUT/log/$label.patch.log" 2>&1 \
        && self_references "$2" "$3" "$4" "$5" \
        && git -C "$5" add -A >> "$OUT/log/$label.patch.log" 2>&1 \
        && git -C "$5" -c user.name=package-run -c user.email=package-run@localhost commit -q -m "local patch of the package run" >> "$OUT/log/$label.patch.log" 2>&1
}
if [ -f "$HERE/patches/$label.patch" ] || [ -f "$HERE/locks/$label.txt" ]; then
    rm -rf "$OUT/patched/$label"; mkdir -p "$OUT/patched/$label"
    : > "$OUT/log/$label.patch.log"
fi

# A package with a patch file (patches/<label>.patch, a git diff of its
# repository) is loaded from a local clone that has the patch as a commit.
# The patch repairs a defect of the package that stops the load.
if [ -f "$HERE/patches/$label.patch" ]; then
    # pkg_classes.st looks for the repository by the name of the manifest.
    pdir=$OUT/patched/$label/$repo
    if patched_clone "$label" "$owner" "$repo" "$branch" "$pdir"; then
        load_expr=$(printf '%s' "$load_expr" | sed "s|github://$owner/$repo:$branch|gitlocal://$pdir|g")
        case "$load_expr" in
            *"gitlocal://$pdir"*) ;;
            *) row_out "PATCH-FAIL load-expression"; echo "[$ROW] $label: the load expression does not name github://$owner/$repo:$branch"; exit 0 ;;
        esac
    else
        row_out "PATCH-FAIL"
        echo "[$ROW] $label: the patch does not apply (log/$label.patch.log)"
        exit 0
    fi
fi

# A package with a locks file (locks/<label>.txt) has a dependency that needs
# the patch of another row of the manifest.  Each line has the baseline name
# of the dependency and the label of that row.  The load starts with the load
# of the dependency from a local clone with the patch and a Metacello lock of
# its baseline.  The load of the package then keeps that dependency.
lock_expr=""
if [ -f "$HERE/locks/$label.txt" ]; then
    while read -r lock_baseline lock_label _; do
        case "$lock_baseline" in ''|'#'*) continue ;; esac
        dline=$(tail -n +2 "$MANIFEST" | awk -F'\t' -v l="$lock_label" '$1 == l' | head -1)
        downer=$(printf '%s' "$dline" | cut -f2)
        drepo=$(printf '%s' "$dline" | cut -f3)
        dbranch=$(printf '%s' "$dline" | cut -f4)
        # The directory of the repository that has the code (for example /source).
        dsub=$(printf '%s' "$dline" | cut -f15 | sed -n "s|.*github://$downer/$drepo:$dbranch\([^']*\)'.*|\1|p")
        ddir=$OUT/patched/$label/$drepo
        if [ -n "$dline" ] && [ -f "$HERE/patches/$lock_label.patch" ] \
            && patched_clone "$lock_label" "$downer" "$drepo" "$dbranch" "$ddir"; then
            # Metacello does not load a locked project that is not in the
            # image.  The dependency is loaded first, with the load
            # expression of its own row, and then locked.
            dload=$(headless_load "$(printf '%s' "$dline" | cut -f15 | sed "s|github://$downer/$drepo:$dbranch|gitlocal://$ddir|g")")
            lock_expr="$lock_expr $dload Metacello new baseline: '$lock_baseline'; repository: 'gitlocal://$ddir$dsub'; lock."
        else
            row_out "PATCH-FAIL lock=$lock_label"
            echo "[$ROW] $label: no clone of $lock_label with its patch (log/$label.patch.log)"
            exit 0
        fi
    done < "$HERE/locks/$label.txt"
    # The load takes the locked repository and gives no warning.
    load_expr=$(printf '%s' "$load_expr" | sed 's/; *load\([:.]\)/; onLock: [ :lockEvent | lockEvent honor ]; load\1/g')
fi

load_expr=$(headless_load "$load_expr")

# 1. load.  The image as the load left it stays in img/<label>.loaded.image.
# With PKG_REUSE_LOAD=1 a later run starts from that image and makes no
# download: for work on the fixes file of a package.
loaded=$OUT/img/$label.loaded.image
if [ "${PKG_REUSE_LOAD:-0}" = 1 ] && [ -f "$loaded" ] && [ ! -f "$HERE/patches/$label.patch" -o "$loaded" -nt "$HERE/patches/$label.patch" ]; then
    echo "[$ROW] $label: the load of an earlier run ($(date +%H:%M:%S))"
    cp -f "$loaded" "$img" && cp -f "${loaded%.image}.changes" "${img%.image}.changes" || fail_setup "copy of the loaded image"
    load_rc=0
else
echo "[$ROW] $label: load ($(date +%H:%M:%S))"
scratch load
memory_ok load
# Iceberg tries an SSH clone first and falls back to HTTPS after an
# authentication error.  The load asks for HTTPS at once.
# GitHub asks for a name and a password when a repository is private or does
# not exist.  Iceberg then opens a dialog and waits (row 51, a repository
# that is gone, held the load until the time limit).  With the
# non-interactive provider the load stops with an error at once.
( cd "$SCRATCH" && exec timeout -k 10 "$LOAD_TIMEOUT" "$COG" --headless "$COG_MEM" "$COG_FRAMES" "$img" eval --save \
    "Iceberg remoteTypeSelector: #httpsUrl. IceCredentialsProvider providerType: IceNonInteractiveCredentialsProvider.$lock_expr $load_expr" ) > "$OUT/log/$label.load.log" 2>&1 < /dev/null &
VM_PID=$!; wait "$VM_PID"; load_rc=$?; VM_PID=""
trim_log "$OUT/log/$label.load.log"
echo "[load exit=$load_rc]" >> "$OUT/log/$label.load.log"
rm -rf "$SCRATCH"
load_state=ok
# A load that fails ends with an exit status that is not 0.  The second test
# is for an error that the load printed and went past: a line that starts
# with the name of an exception class.
if [ "$load_rc" -ne 0 ]; then load_state="LOAD-FAIL rc=$load_rc"
elif grep -aqE '^(.\[[0-9;]*m)?[A-Za-z]*(Error|NotUnderstood|NotFound|Exception)[: ]' "$OUT/log/$label.load.log"; then load_state="LOAD-FAIL error-in-log"
fi
if [ "$load_state" != ok ]; then
    row_out "$load_state"
    echo "[$ROW] $label: $load_state (log/$label.load.log)"
    rm -f "$img" "${img%.image}.changes"
    exit 0
fi
cp -f "$img" "$loaded" && cp -f "${img%.image}.changes" "${loaded%.image}.changes"
fi

# 2. fake GUI, image fixes, and the fixes of this package if it has a file
scratch prep
pkgfix=""
fixes_file=${PKG_FIXES_FILE:-$HERE/fixes/$label.st}
if [ -f "$fixes_file" ]; then
    python3 "$REPO/scripts/gen-image-fixes.py" --one "$fixes_file" "$SCRATCH/pkgfix.st" || fail_setup "$fixes_file"
    pkgfix="'$SCRATCH/pkgfix.st' asFileReference fileIn. "
fi
memory_ok prep
( cd "$SCRATCH" && exec timeout -k 10 600 "$COG" --headless "$COG_MEM" "$COG_FRAMES" "$img" eval --save \
    "'$REPO/scripts/pharo-headless-test/setup_fake_gui.st' asFileReference fileIn. '$REPO/scripts/pharo-image-fixes.st' asFileReference fileIn. ${pkgfix}1" ) \
    > "$OUT/log/$label.prep.log" 2>&1 < /dev/null &
VM_PID=$!; wait "$VM_PID"; prep_rc=$?; VM_PID=""
echo "[prep exit=$prep_rc]" >> "$OUT/log/$label.prep.log"
if [ "$prep_rc" -ne 0 ]; then
    row_out "PREP-FAIL rc=$prep_rc"
    echo "[$ROW] $label: PREP-FAIL rc=$prep_rc (log/$label.prep.log)"
    rm -rf "$SCRATCH"
    exit 0
fi

# 3. class list
classes=$OUT/res/$label.classes.txt
memory_ok class-list
fixtures=""
[ -f "$HERE/fixtures/$label.txt" ] && fixtures=$(grep -v '^#' "$HERE/fixtures/$label.txt" | awk 'NF { printf "%s%s", sep, $1; sep = "," }')
( cd "$SCRATCH" && SUNIT_PREFIX="$OUT/res/$label.list" PKG_PREFIXES="$test_prefix" PKG_REPO="$repo" PKG_FIXTURES="$fixtures" exec timeout -k 10 300 "$COG" --headless "$COG_MEM" "$COG_FRAMES" "$img" eval \
    "'$HERE/pkg_classes.st' asFileReference fileIn" ) >> "$OUT/log/$label.prep.log" 2>&1 < /dev/null &
VM_PID=$!; wait "$VM_PID"; VM_PID=""
rm -rf "$SCRATCH"
if [ ! -f "$OUT/res/$label.list_class_names.txt" ]; then
    row_out "CLASS-LIST-FAIL"
    echo "[$ROW] $label: no class list (log/$label.prep.log)"
    exit 0
fi
mv "$OUT/res/$label.list_class_names.txt" "$classes"
nclasses=$(awk 'NF { n++ } END { print n + 0 }' "$classes")
npackages=$(awk 'NF { n++ } END { print n + 0 }' "$OUT/res/$label.list_packages.txt" 2>/dev/null)
if [ "$nclasses" -eq 0 ]; then
    # 0 packages: the repository was not found in the image, or the lookup
    # failed (res/<label>.list_error.txt, res/<label>.list_repos.txt).
    row_out "NO-TEST-CLASSES packages=${npackages:-0}" 0
    echo "[$ROW] $label: no test class in the ${npackages:-0} loaded packages of the repository '$repo' and none with the package prefix '$test_prefix'"
    exit 0
fi

# 4. and 5. the tests, each VM on its own copy of the image
run_vm() {      # $1 = cog|ours
    local kind=$1 pre=$OUT/res/$label.$1 d s rc
    scratch "$kind"; d=$SCRATCH
    cp "$img" "$d/pkg.image" && cp "${img%.image}.changes" "$d/pkg.changes" || fail_setup "copy of the package image"
    for s in "$OUT/img"/*.sources; do ln "$s" "$d/" 2>/dev/null || cp "$s" "$d/" || fail_setup "copy of the sources file"; done
    # The clones of the load (the tests of a package can read files of its
    # repository).  Each VM gets a copy, so a test that writes there on one VM
    # does not change the start state of the other VM.  cp -c makes an APFS
    # clone, which takes no time and no space.
    if [ -d "$OUT/img/pharo-local" ]; then
        cp -cR "$OUT/img/pharo-local" "$d/pharo-local" 2>/dev/null || cp -R "$OUT/img/pharo-local" "$d/pharo-local" || fail_setup "copy of pharo-local"
    fi
    cp "$classes" "${pre}_class_names.txt" || fail_setup "copy of the class list"
    memory_ok "$kind"
    if [ "$kind" = cog ]; then
        ( cd "$d" && SUNIT_PREFIX="$pre" exec timeout -k 10 "$TEST_TIMEOUT" "$COG" --headless "$COG_MEM" "$COG_FRAMES" "$d/pkg.image" eval "$RUNNER_EXPR" ) \
            > "$OUT/log/$label.cog.log" 2>&1 < /dev/null &
    else
        ( cd "$d" && SUNIT_PREFIX="$pre" PHARO_EVAL_NO_SUNIT_MARKER=1 exec timeout -k 10 "$TEST_TIMEOUT" "$OURS" "$d/pkg.image" eval "$RUNNER_EXPR" ) \
            > "$OUT/log/$label.ours.log" 2>&1 < /dev/null &
    fi
    VM_PID=$!; wait "$VM_PID"; rc=$?; VM_PID=""
    trim_log "$OUT/log/$label.$kind.log"
    echo "[exit=$rc]" >> "$OUT/log/$label.$kind.log"
    rm -rf "$d"
    VM_RC=$rc
}
echo "[$ROW] $label: $nclasses test classes, stock Cog ($(date +%H:%M:%S))"
run_vm cog;  cog_rc=$VM_RC
echo "[$ROW] $label: our VM ($(date +%H:%M:%S))"
run_vm ours; ours_rc=$VM_RC

# 6. summary
c=$(totals "$OUT/res/$label.cog_test_results.txt"); o=$(totals "$OUT/res/$label.ours_test_results.txt")
status_list "$OUT/res/$label.cog_test_detail.txt"  > "$OUT/res/$label.cog.status"
status_list "$OUT/res/$label.ours_test_detail.txt" > "$OUT/res/$label.ours.status"
LC_ALL=C comm -3 "$OUT/res/$label.cog.status" "$OUT/res/$label.ours.status" > "$OUT/res/$label.diff"
# The state is "ok" only when the two VMs ran the full list.  A VM that
# stopped in the middle gives no comparison.
state=ok
case "$c" in INCOMPLETE*|NO-RESULT) state="COG-INCOMPLETE" ;; esac
case "$o" in INCOMPLETE*|NO-RESULT) [ "$state" = ok ] && state="OURS-INCOMPLETE" || state="BOTH-INCOMPLETE" ;; esac
clean=no
if [ "$state" = ok ]; then
    d=$(sed 's/^	//' "$OUT/res/$label.diff" | awk '{ print $1 }' | LC_ALL=C sort -u | awk 'END { print NR }')
    nonpass=$(( $(nonpass_total "$OUT/res/$label.cog_test_results.txt") + $(nonpass_total "$OUT/res/$label.ours_test_results.txt") ))
    [ "$d" -eq 0 ] && [ "$nonpass" -eq 0 ] && [ "$cog_rc" -eq 0 ] && [ "$ours_rc" -eq 0 ] && clean=yes
else
    d=-
fi
row_out "$state" "$nclasses" "$c" "$o" "$d" "$cog_rc" "$ours_rc"
echo "[$ROW] $label: $state cog[$c] ours[$o] different=$d ($(date +%H:%M:%S))"

case "$KEEP_IMAGES" in
    all)  ;;
    none) rm -f "$img" "${img%.image}.changes" "$loaded" "${loaded%.image}.changes" ;;
    *)    [ "$clean" = yes ] && rm -f "$img" "${img%.image}.changes" "$loaded" "${loaded%.image}.changes" ;;
esac
exit 0
