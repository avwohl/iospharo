#!/bin/bash
# record-sunit.sh — run the SUnit suite on Cog and/or our JIT VM and record
# per-test results + process CPU/wall into the vmperf DB.
#
# Cog correctness for a fixed image is deterministic, so once a full Cog run is
# recorded for a source it never needs re-running: pass --skip-cog-if-present.
#
# Usage:
#   scripts/perfdb/record-sunit.sh --classes "ArrayTest SetTest OrderedCollectionTest"
#   scripts/perfdb/record-sunit.sh --classes-file scripts/pharo-headless-test/test_classes.txt
#   scripts/perfdb/record-sunit.sh --classes-file ... --skip-cog-if-present
#   scripts/perfdb/record-sunit.sh --classes "ArrayTest" --ours-only --jit-knobs "PHARO_JIT_DEFER=15"
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
PERFDB="$SCRIPT_DIR/perfdb.py"
RUNNERS="$PROJECT_ROOT/scripts/pharo-headless-test"

OUR_VM="${OUR_VM:-$PROJECT_ROOT/build-rel/test_load_image}"
[ -x "$OUR_VM" ] || OUR_VM="$PROJECT_ROOT/build/test_load_image"
# OUR_ARCH labels the JIT vm_build in the perfdb.  Defaults to the host arch, but
# the x86_64 binary (build-x86, run via Rosetta on Apple Silicon) must be recorded
# as x86_64, not the host arm64 — set OUR_ARCH=x86_64 OUR_VM=build-x86/test_load_image.
OUR_ARCH="${OUR_ARCH:-$(uname -m)}"
HARNESS_DIR="${HARNESS_DIR:-/tmp/harness}"
COG_VM="$HARNESS_DIR/pharo"
IMAGE_SRC="$HARNESS_DIR/Pharo.image.bak"
[ -f "$IMAGE_SRC" ] || IMAGE_SRC="$HARNESS_DIR/Pharo.image"
TIMEOUT_PER_RUN="${SUNIT_TIMEOUT:-1200}"
IMAGE_SIG="${IMAGE_SIG:-pharo13.1-45e803d}"
JIT_KNOBS="${JIT_KNOBS:-PHARO_JIT_DEFER=15}"

RUN_REF=true; RUN_OURS=true; SKIP_COG_IF_PRESENT=false
CLASSES=""; CLASSES_FILE=""
# Source identity — defaults to the kernel suite; override for soogle packages
# (which, once Metacello-loaded into an image, are just SUnit TestCases).
SOURCE_NAME="sunit-kernel"; SOURCE_KIND="sunit"; SOURCE_URL=""
while [ $# -gt 0 ]; do
  case "$1" in
    --ours-only) RUN_REF=false ;;
    --ref-only)  RUN_OURS=false ;;
    --classes)   CLASSES="$2"; shift ;;
    --classes-file) CLASSES_FILE="$2"; shift ;;
    --jit-knobs) JIT_KNOBS="$2"; shift ;;
    --skip-cog-if-present) SKIP_COG_IF_PRESENT=true ;;
    --image)       IMAGE_SRC="$2"; shift ;;       # package-loaded image
    --source-name) SOURCE_NAME="$2"; shift ;;     # e.g. soogle:PolyMath
    --source-kind) SOURCE_KIND="$2"; shift ;;     # sunit | package
    --source-url)  SOURCE_URL="$2"; shift ;;      # git URL the package came from
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
  shift
done

[ -x "$OUR_VM" ]    || { echo "ERROR: our VM not built: $OUR_VM" >&2; exit 2; }
[ -f "$IMAGE_SRC" ] || { echo "ERROR: image missing: $IMAGE_SRC" >&2; exit 2; }
[ -x "$COG_VM" ]    || { echo "WARN: Cog VM missing — ref skipped" >&2; RUN_REF=false; }

# --- shared class list. Cog reads /tmp/sunit_test_classes.txt; our runner
# discovers ALL TestCase subclasses unless /tmp/sunit_class_names.txt restricts
# it. Stage BOTH with the same set so both VMs run the SAME classes. ---
if [ -n "$CLASSES_FILE" ]; then
  cp "$CLASSES_FILE" /tmp/sunit_test_classes.txt
elif [ -n "$CLASSES" ]; then
  printf '%s\n' $CLASSES > /tmp/sunit_test_classes.txt
else
  echo "ERROR: pass --classes or --classes-file" >&2; exit 2
fi
cp /tmp/sunit_test_classes.txt /tmp/sunit_class_names.txt   # our-runner subset filter
NCLASSES=$(wc -l < /tmp/sunit_test_classes.txt | tr -d ' ')
rm -f /tmp/sunit_batch.txt

# --- identities ---
MACHINE=$(python3 "$PERFDB" register-machine)
SHA=$(git -C "$PROJECT_ROOT" rev-parse HEAD)
GITDESC=$(git -C "$PROJECT_ROOT" describe --always --dirty 2>/dev/null || echo "$SHA")
JIT_VM=$(python3 "$PERFDB" register-vm --kind jit --git-sha "$SHA" \
           --build-config RelWithDebInfo --arch "$OUR_ARCH" --binary "$OUR_VM" --vm-version "iospharo-jit@${SHA:0:9}-${OUR_ARCH}")
COG_VER=$("$COG_VM" --version 2>/dev/null | grep -oE 'v[0-9.]+\+[0-9.]+[a-f0-9]+' | head -1)
COG_VM_ID=$(python3 "$PERFDB" register-vm --kind cog --build-config Release \
           --arch "$(uname -m)" --vm-version "Pharo-${COG_VER:-unknown}" --git-sha "${COG_VER##*+}")
SUNIT_SRC=$(python3 "$PERFDB" register-source --name "$SOURCE_NAME" --kind "$SOURCE_KIND" \
           --pharo-version 13.1 --image-sig "$IMAGE_SIG" ${SOURCE_URL:+--url "$SOURCE_URL"})
echo "[ids] machine=$MACHINE jit_vm=$JIT_VM cog_vm=$COG_VM_ID sunit_src=$SUNIT_SRC classes=$NCLASSES"

record_run() {  # label vm_id knobs status
  local label="$1" vm_id="$2" knobs="$3"
  local run_id
  run_id=$(python3 "$PERFDB" run-and-record --kind sunit --vm "$vm_id" \
    --machine "$MACHINE" --source "$SUNIT_SRC" --knobs "$knobs" \
    --result /tmp/sunit_test_results.txt --detail /tmp/sunit_test_detail.txt \
    --timeout "$TIMEOUT_PER_RUN" \
    --git-describe "$GITDESC" --log /tmp/sunit_test_results.txt \
    --shell "$4")
  echo "[$label] recorded run id=$run_id"
}

# --- Cog ---
if $RUN_REF; then
  if $SKIP_COG_IF_PRESENT && \
     [ -n "$(python3 "$PERFDB" query "SELECT id FROM run WHERE kind='sunit' AND vm_build_id=$COG_VM_ID AND source_id=$SUNIT_SRC AND status='ok' LIMIT 1;" | tail -n +2)" ]; then
    echo "[ref] skipped — Cog SUnit result already present for this source (deterministic)"
  else
    cp "$IMAGE_SRC" /tmp/sunit_cog.image
    cp "$HARNESS_DIR/Pharo.changes" /tmp/sunit_cog.changes 2>/dev/null || true
    rm -f /tmp/sunit_test_results.txt /tmp/sunit_test_detail.txt /tmp/sunit_run_completed.txt
    echo "[ref] running Cog SUnit ($NCLASSES classes)..."
    record_run ref "$COG_VM_ID" "" \
      "cd /tmp && exec '$COG_VM' --headless /tmp/sunit_cog.image eval \"'$RUNNERS/run_sunit_cog.st' asFileReference fileIn\""
  fi
fi

# --- ours: prep image with runner+handler (via Cog --save), then run our VM ---
if $RUN_OURS; then
  cp "$IMAGE_SRC" /tmp/sunit_ours.image
  cp "$HARNESS_DIR/Pharo.changes" /tmp/sunit_ours.changes 2>/dev/null || true
  echo "[ours] prepping image (install SUnitRunner + handler)..."
  (cd /tmp && timeout 120 "$COG_VM" --headless /tmp/sunit_ours.image eval --save \
     "'$RUNNERS/run_sunit_tests.st' asFileReference fileIn") >/dev/null 2>&1
  rm -f /tmp/sunit_test_results.txt /tmp/sunit_test_detail.txt /tmp/sunit_run_completed.txt
  echo "[ours] running our VM SUnit ($NCLASSES classes), knobs: $JIT_KNOBS ..."
  record_run ours "$JIT_VM" "$JIT_KNOBS" \
    "cd /tmp && exec env $JIT_KNOBS PHARO_MAX_STEPS=2e12 '$OUR_VM' /tmp/sunit_ours.image"
fi

echo "=== SUnit pass/fail by VM (this source) ==="
python3 "$PERFDB" query "
SELECT v.kind, r.knobs, r.pass_count, r.fail_count, r.error_count, r.skip_count, r.status,
       ROUND(r.proc_cpu_seconds,1) cpu_s, ROUND(r.proc_real_seconds,1) real_s
FROM run r JOIN vm_build v ON v.id=r.vm_build_id
WHERE r.kind='sunit' AND r.source_id=$SUNIT_SRC ORDER BY r.id DESC LIMIT 6;"
