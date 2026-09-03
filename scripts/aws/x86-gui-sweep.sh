#!/usr/bin/env bash
# x86-gui-sweep.sh — RUNS ON THE BOX.  Batched full-suite sweep on native
# x86_64 Linux, on a fake-GUI-prepped image, with per-batch checkpointing to S3
# so a spot reclaim costs one batch instead of the whole run.
#
# Why batched rather than x86-fullsuite.sh's single 4 h run: one class can wedge
# the VM past the in-image watchdog (measured 2026-09-03, batch 651-700 on the
# Mac), and a single-run sweep loses everything after that point.  Batching
# bounds it, and RETRY_DAMAGED re-runs whatever a wedged batch skipped.
#
# Resume: re-run with the same RUN_ID.  It pulls the previous outdir from S3 and
# continues from the first batch the last attempt did not finish.
set -uo pipefail
REPO=${REPO:-/home/ubuntu/src/iospharo}
HARNESS=${HARNESS:-/home/ubuntu/harness}
STOCK=${STOCK:-/home/ubuntu/stockvm}
RUN_ID=${RUN_ID:-x86-gui-$(date -u +%Y%m%d)}
OUT=${OUT:-/home/ubuntu/results/$RUN_ID}
BUCKET=${BUCKET:-iospharo-build-670060058357}
S3=${S3:-s3://$BUCKET/sweeps/$RUN_ID}
VM="$REPO/build/test_load_image"
mkdir -p "$OUT"

log() { echo "[$(date -u '+%H:%M:%S')] $*"; }

# --- stock VM: needed for the prep, our VM cannot file the runner in itself ---
if [ ! -x "$STOCK/pharo" ]; then
    log "installing stock pharo VM"
    rm -rf "$STOCK"; mkdir -p "$STOCK"
    (cd "$STOCK" && curl -sL https://get.pharo.org/64/vm130 | bash) >/dev/null 2>&1
fi
[ -x "$STOCK/pharo" ] || { echo "FATAL: no stock Pharo VM; the prep cannot install SUnitRunner" >&2; exit 1; }
[ -x "$VM" ] || { echo "FATAL: no $VM -- run clone-and-build.sh first" >&2; exit 1; }

# --- resume? pull whatever the last attempt got to ---
START_AT=1
if aws s3 ls "$S3/sweep.log" >/dev/null 2>&1; then
    log "found a previous attempt in S3 -- pulling it back"
    aws s3 sync "$S3" "$OUT" --quiet
    last=$(grep -a '^batch' "$OUT/sweep.log" 2>/dev/null | tail -1 | awk '{split($2,b,"-"); print b[2]}')
    if [ -n "$last" ]; then
        START_AT=$((last + 1))
        log "resuming at index $START_AT (last completed batch ended at $last)"
    fi
fi

# --- prep: fresh harness, fake GUI, then the runner ---
# image+changes+sources must come from the SAME fetch; a stale .sources beside a
# fresh image mimics a VM regression across ~700 AST/decompiler tests.
if [ ! -f "$HARNESS/Pharo.image" ]; then
    log "fetching a fresh Pharo 13 image"
    rm -rf "$HARNESS"; mkdir -p "$HARNESS"
    (cd "$HARNESS" && curl -sL https://get.pharo.org/64/130 | bash >/dev/null 2>&1)
fi
if [ ! -f "$HARNESS/sunit-gui.image" ]; then
    log "prepping: setup_fake_gui.st then run_sunit_tests.st"
    cp "$HARNESS/Pharo.image"   "$HARNESS/sunit-gui.image"
    cp "$HARNESS/Pharo.changes" "$HARNESS/sunit-gui.changes"
    rm -f "$HARNESS/startup.st"
    (cd "$HARNESS" && "$STOCK/pharo" "$HARNESS/sunit-gui.image" eval --save \
        "'$REPO/scripts/pharo-headless-test/setup_fake_gui.st' asFileReference fileIn.
         '$REPO/scripts/pharo-headless-test/run_sunit_tests.st' asFileReference fileIn" ) 2>&1 | tail -2
    log "prepped image: $(stat -c%s "$HARNESS/sunit-gui.image") bytes"
fi
# DO NOT "verify" the prep by resuming the prepped image.  It carries
# SUnitRunner's SessionManager startUp: handler, so ANY resume -- including a
# one-line `eval` -- starts a full test run.  A verification eval here hung the
# script on 2026-09-03 while quietly running the suite in the background.
# The first batch's classes= count is the verification.

# --- checkpoint loop: keep S3 within a batch of the truth ---
( while sleep 60; do aws s3 sync "$OUT" "$S3" --quiet 2>/dev/null; done ) &
SYNC_PID=$!
trap 'kill $SYNC_PID 2>/dev/null; aws s3 sync "$OUT" "$S3" --quiet 2>/dev/null' EXIT

log "sweep starting (START_AT=$START_AT)"
export PHARO_CODE_ZONE_MB=192 PHARO_MAX_STEPS=4000000000000 PHARO_MAX_OLD_SPACE_MB=12288
START_AT=$START_AT STEP=${STEP:-50} PER_BATCH_TIMEOUT=${PER_BATCH_TIMEOUT:-1800} \
  RETRY_DAMAGED=1 "$REPO/scripts/sunit-sweep.sh" "$VM" "$HARNESS/sunit-gui.image" "$OUT"
rc=$?
log "sweep exit=$rc"
aws s3 sync "$OUT" "$S3" --quiet 2>/dev/null
log "results synced to $S3"
