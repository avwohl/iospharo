#!/usr/bin/env bash
# sunit-fullsuite-ab.sh — RUNS ON THE BOX.  Full-suite (2045-class) A/B of
# today's JIT state: default config vs PHARO_T1_CAN_SKIP_J2J_SAVE=1 (the
# saveless leaf-call path, candidate for default-on).  Writes per-config
# results + a per-test diff to ~/results/ and uploads to S3 when configured.
#
# Bakes in the silent-runner checklist (WIP.md 2026-06-10):
#   - rm /tmp/sunit_run_completed.txt before every run (eval runs recreate it)
#   - rm <imagedir>/startup.st (stale eval script quits the image instantly)
#   - /tmp/sunit_batch.txt is 1-BASED
#   - PHARO_MAX_STEPS large (fast builds burn the 60G default in minutes)
#   - PHARO_CODE_ZONE_MB=192 (the 64MB zone fills mid-suite)
#   - check <imagedir>/PharoDebug.log when a run is silent
set -uo pipefail
REPO=/home/ubuntu/src/iospharo
HARNESS=/home/ubuntu/harness
STOCK=/home/ubuntu/stockvm
OUT=/home/ubuntu/results
VM="$REPO/build-opt/test_load_image"
mkdir -p "$OUT"

if [ ! -x "$STOCK/pharo" ]; then
    echo "== installing stock pharo VM =="
    rm -rf "$STOCK"; mkdir -p "$STOCK"
    (cd "$STOCK" && curl -sL https://get.pharo.org/64/vm130 | bash) >/dev/null 2>&1
fi
[ -f "$HARNESS/Pharo.image" ] || (mkdir -p "$HARNESS" && cd "$HARNESS" && curl -sL https://get.pharo.org/64/130 | bash >/dev/null 2>&1)

if [ ! -x "$VM" ]; then
    echo "== building build-opt =="
    cmake -S "$REPO" -B "$REPO/build-opt" -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build "$REPO/build-opt" --target test_load_image -j"$(nproc)"
fi

cp "$REPO/scripts/pharo-headless-test/test_classes.txt" /tmp/sunit_test_classes.txt
rm -f /tmp/sunit_class_names.txt
NCLASSES=$(wc -l < /tmp/sunit_test_classes.txt | tr -d ' ')
printf '1 %s\n' "$NCLASSES" > /tmp/sunit_batch.txt
echo "full suite: $NCLASSES classes"

echo "== prep: fileIn SUnit runner =="
rm -f "$HARNESS/startup.st"
"$STOCK/pharo" "$HARNESS/Pharo.image" eval --save \
    "'$REPO/scripts/pharo-headless-test/run_sunit_tests.st' asFileReference fileIn" 2>&1 | tail -2

run_config() {
    local name="$1"; shift
    echo "== run: $name =="
    rm -f /tmp/sunit_run_completed.txt "$HARNESS/startup.st" \
          /tmp/sunit_test_results.txt /tmp/sunit_test_detail.txt \
          "$HARNESS/PharoDebug.log"
    env "$@" PHARO_MAX_STEPS=4000000000000 PHARO_CODE_ZONE_MB=192 \
        timeout 14400 "$VM" "$HARNESS/Pharo.image" \
        > "$OUT/run_$name.log" 2>&1
    cp /tmp/sunit_test_results.txt "$OUT/results_$name.txt" 2>/dev/null || {
        echo "!! $name produced no results — check $OUT/run_$name.log and PharoDebug.log"
        cp "$HARNESS/PharoDebug.log" "$OUT/pharodebug_$name.log" 2>/dev/null
    }
    cp /tmp/sunit_test_detail.txt "$OUT/detail_$name.txt" 2>/dev/null
    rm -f /tmp/sunit_run_completed.txt
    tail -8 "$OUT/results_$name.txt" 2>/dev/null
}

run_config default
run_config saveless PHARO_T1_CAN_SKIP_J2J_SAVE=1

echo "== per-test diff (default vs saveless) =="
diff <(sort "$OUT/detail_default.txt" 2>/dev/null | awk -F'\t' '{print $2, $3, $4}') \
     <(sort "$OUT/detail_saveless.txt" 2>/dev/null | awk -F'\t' '{print $2, $3, $4}') \
     > "$OUT/ab_diff.txt" && echo "PER-TEST IDENTICAL" || head -40 "$OUT/ab_diff.txt"

# S3 upload — REQUIRED, not optional: the idle-shutdown unit terminates
# the box ~30 min after the run and ~/results is lost (learned 2026-06-10:
# a completed full-suite A/B evaporated).  The bucket name is stable for
# this account; instance-profile credentials grant access on the box.
BUCKET="${BUCKET:-iospharo-build-670060058357}"
if command -v aws >/dev/null 2>&1; then
    aws s3 cp --recursive "$OUT" "s3://$BUCKET/sunit-arm/ab-$(date +%Y%m%d-%H%M)/" || echo "!! S3 upload failed — copy ~/results off-box before idle shutdown"
fi
echo "== DONE =="
