#!/bin/bash
# record-bench.sh — run the bench-suite on Cog and/or our JIT VM and record
# every result (per-benchmark in-image CPU ms + process CPU/wall time) into the
# vmperf MySQL DB via perfdb.py.
#
# Usage:
#   scripts/perfdb/record-bench.sh                  # both VMs
#   scripts/perfdb/record-bench.sh --ours-only
#   scripts/perfdb/record-bench.sh --ref-only
#   scripts/perfdb/record-bench.sh --jit-knobs "PHARO_T1_INLINE_BLOCK_VALUE=1 PHARO_BV_MAX_CAP=1"
#
# Cog timing rarely changes on a fixed machine, so --skip-cog-if-present skips
# the Cog run when a Cog bench result already exists for this machine+source.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
PERFDB="$SCRIPT_DIR/perfdb.py"
INJECT_ST="$SCRIPT_DIR/bench_inject.st"

OUR_VM="${OUR_VM:-$PROJECT_ROOT/build-rel/test_load_image}"
[ -x "$OUR_VM" ] || OUR_VM="$PROJECT_ROOT/build/test_load_image"
HARNESS_DIR="${HARNESS_DIR:-/tmp/harness}"
COG_VM="$HARNESS_DIR/pharo"
IMAGE_SRC="$HARNESS_DIR/Pharo.image.bak"
[ -f "$IMAGE_SRC" ] || IMAGE_SRC="$HARNESS_DIR/Pharo.image"
TIMEOUT_PER_RUN="${BENCH_TIMEOUT:-200}"
IMAGE_SIG="${IMAGE_SIG:-pharo13.1-45e803d}"
JIT_KNOBS="${JIT_KNOBS:-PHARO_JIT_DEFER=15}"

RUN_REF=true; RUN_OURS=true; SKIP_COG_IF_PRESENT=false
while [ $# -gt 0 ]; do
  case "$1" in
    --ours-only) RUN_REF=false ;;
    --ref-only)  RUN_OURS=false ;;
    --jit-knobs) JIT_KNOBS="$2"; shift ;;
    --skip-cog-if-present) SKIP_COG_IF_PRESENT=true ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
  shift
done

[ -x "$OUR_VM" ]    || { echo "ERROR: our VM not built: $OUR_VM" >&2; exit 2; }
[ -f "$IMAGE_SRC" ] || { echo "ERROR: image missing: $IMAGE_SRC (curl get.pharo.org/64/130+vm)" >&2; exit 2; }
[ -x "$COG_VM" ]    || { echo "WARN: Cog VM missing — ref run skipped" >&2; RUN_REF=false; }

# --- identities (idempotent) ---
MACHINE=$(python3 "$PERFDB" register-machine)
SHA=$(git -C "$PROJECT_ROOT" rev-parse HEAD)
GITDESC=$(git -C "$PROJECT_ROOT" describe --always --dirty 2>/dev/null || echo "$SHA")
JIT_VM=$(python3 "$PERFDB" register-vm --kind jit --git-sha "$SHA" \
           --build-config RelWithDebInfo --arch "$(uname -m)" --binary "$OUR_VM" --vm-version "iospharo-jit@${SHA:0:9}")
COG_VER=$("$COG_VM" --version 2>/dev/null | grep -oE 'v[0-9.]+\+[0-9.]+[a-f0-9]+' | head -1)
COG_VM_ID=$(python3 "$PERFDB" register-vm --kind cog --build-config Release \
           --arch "$(uname -m)" --vm-version "Pharo-${COG_VER:-unknown}" --git-sha "${COG_VER##*+}")
BENCH_SRC=$(python3 "$PERFDB" register-source --name bench-suite --kind bench \
           --pharo-version 13.1 --image-sig "$IMAGE_SIG")
echo "[ids] machine=$MACHINE jit_vm=$JIT_VM cog_vm=$COG_VM_ID bench_src=$BENCH_SRC"

# --- stage + inject the bench harness once (via Cog --save) ---
IMAGE="/tmp/bench_suite.image"
cp "$IMAGE_SRC" "$IMAGE"
cp "$HARNESS_DIR/Pharo.changes" "${IMAGE%.image}.changes" 2>/dev/null || true
SRCFILE=$(ls "$HARNESS_DIR"/Pharo*.sources 2>/dev/null | head -1)
[ -n "$SRCFILE" ] && [ ! -f "/tmp/$(basename "$SRCFILE")" ] && cp "$SRCFILE" /tmp/
echo "[setup] injecting bench handler via Cog..."
(cd /tmp && timeout 60 "$COG_VM" --headless "$IMAGE" st --save --quit "$INJECT_ST") >/dev/null 2>&1

run_one() {  # label vm_id knobs sh_command
  local label="$1" vm_id="$2" knobs="$3" cmd="$4"
  rm -f /tmp/bench_suite_result.txt
  echo "[$label] running..."
  local run_id
  run_id=$(python3 "$PERFDB" run-and-record --kind bench --vm "$vm_id" \
    --machine "$MACHINE" --source "$BENCH_SRC" --knobs "$knobs" \
    --result /tmp/bench_suite_result.txt --timeout "$TIMEOUT_PER_RUN" \
    --git-describe "$GITDESC" --log /tmp/bench_suite_result.txt \
    --shell "$cmd")
  echo "[$label] recorded run id=$run_id"
}

if $RUN_REF; then
  if $SKIP_COG_IF_PRESENT && \
     [ -n "$(python3 "$PERFDB" query "SELECT id FROM run WHERE kind='bench' AND vm_build_id=$COG_VM_ID AND machine_id=$MACHINE AND source_id=$BENCH_SRC LIMIT 1;" | tail -n +2)" ]; then
    echo "[ref] skipped — Cog bench result already present for this machine"
  else
    cp "$IMAGE" /tmp/bench_suite-ref.image
    cp "${IMAGE%.image}.changes" /tmp/bench_suite-ref.changes 2>/dev/null || true
    run_one ref "$COG_VM_ID" "" \
      "cd /tmp && exec '$COG_VM' --headless /tmp/bench_suite-ref.image --no-quit"
  fi
fi

if $RUN_OURS; then
  cp "$IMAGE" /tmp/bench_suite-ours.image
  cp "${IMAGE%.image}.changes" /tmp/bench_suite-ours.changes 2>/dev/null || true
  run_one ours "$JIT_VM" "$JIT_KNOBS" \
    "cd /tmp && exec env $JIT_KNOBS '$OUR_VM' /tmp/bench_suite-ours.image"
fi

echo "=== JIT vs Cog (latest, in-image CPU ms) ==="
python3 "$PERFDB" report-bench --source bench-suite
