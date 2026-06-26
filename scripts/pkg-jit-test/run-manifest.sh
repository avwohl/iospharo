#!/usr/bin/env bash
# run-manifest.sh — drive the 200-package JIT correctness sweep from a manifest.
#
# For each package: copy a clean Pharo 13.1 image, have the STOCK Cog VM
# Metacello-load+save it (our VM can't do HTTPS), then run the package's SUnit on
# BOTH the custom JIT VM and stock Cog and record load status + the JIT-only
# failure count (candidate JIT regressions).  Resumable: rows already in
# summary.tsv are skipped.  Visual ('gui') rows get the headless fake-GUI prelude.
#
# Best run on the AWS build box (16 vCPU); the keep-alive lease keeps the box up
# while this Claude drives it.  This is a LONG job — run it backgrounded + polled.
#
#   PHARO=/tmp/h3/pharo BASE_IMAGE=/tmp/h3/Pharo.image \
#   CUSTOM_VM=$PWD/build-rel/test_load_image \
#     scripts/pkg-jit-test/run-manifest.sh [manifest.tsv] [start_row] [count]
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
MANIFEST="${1:-$HERE/packages-200.tsv}"
START="${2:-1}"; COUNT="${3:-100000}"
PHARO="${PHARO:-/tmp/h3/pharo}"
BASE_IMAGE="${BASE_IMAGE:-/tmp/h3/Pharo.image}"
CUSTOM_VM="${CUSTOM_VM:-$HERE/../../build-rel/test_load_image}"
FAKE_GUI="${FAKE_GUI:-$HERE/../pharo-headless-test/setup_fake_gui.st}"
OUT="${OUT:-/tmp/pkg200}"; mkdir -p "$OUT"
SUMMARY="$OUT/summary.tsv"
[ -f "$SUMMARY" ] || printf 'label\tload\tcog_RESULT\tjit_RESULT\tjit_only_fails\n' > "$SUMMARY"

# Pharo finds its .sources next to the image; copy the base image's .sources into
# OUT once (a REAL file, not a symlink — the source map can be flaky over one) so
# every per-package image copy resolves it.  Same for the shared base .changes.
base_dir="$(cd "$(dirname "$BASE_IMAGE")" && pwd)"
src_file="$(ls "$base_dir"/*.sources 2>/dev/null | head -1)"
if [ -n "$src_file" ] && [ ! -f "$OUT/$(basename "$src_file")" ]; then
    cp -f "$src_file" "$OUT/"
fi
base_changes="${BASE_IMAGE%.image}.changes"
row=0
# columns: label owner repo branch baseline test_path category risk p13 n_tests stars test_prefix jit_value risk_reason load_expr
tail -n +2 "$MANIFEST" | while IFS=$'\t' read -r label owner repo branch baseline test_path category risk p13 n_tests stars test_prefix jit_value risk_reason load_expr; do
    [ -n "$label" ] || continue
    row=$((row+1))
    [ "$row" -lt "$START" ] && continue
    [ "$row" -ge $((START+COUNT)) ] && break
    if grep -q "^$label	" "$SUMMARY" 2>/dev/null; then echo "[$row] skip $label (done)"; continue; fi

    img="$OUT/$label.image"
    cp -f "$BASE_IMAGE" "$img"
    [ -f "$base_changes" ] && cp -f "$base_changes" "${img%.image}.changes"
    echo "[$row] $label : load on stock Cog ($test_path, risk=$risk) ..."
    if timeout 600 "$PHARO" "$img" eval --save "$load_expr" > "$OUT/$label.load.log" 2>&1 \
         && ! grep -qiE 'does not understand|could not resolve|MetacelloError|fetchRequiredError|error during|nonexistent|fileNotFound' "$OUT/$label.load.log"; then
        load=ok
    else
        load=FAIL
    fi
    if [ "$load" != ok ]; then
        printf '%s\tFAIL\t-\t-\t-\n' "$label" >> "$SUMMARY"
        echo "   LOAD FAILED (see $OUT/$label.load.log)"; rm -f "$img" "${img%.image}.changes"; continue
    fi

    prelude=""; [ "$test_path" = gui ] && prelude="$FAKE_GUI"
    PHARO="$PHARO" CUSTOM_VM="$CUSTOM_VM" OUT="$OUT" PRELUDE="$prelude" \
        bash "$HERE/run-pkg-jit-test.sh" "$img" "$label" "$test_prefix" > "$OUT/$label.test.log" 2>&1 || true
    cog=$(grep -aE '^RESULT' "$OUT/${label}_cog.log" 2>/dev/null | tail -1)
    jit=$(grep -aE '^RESULT' "$OUT/${label}_jit.log" 2>/dev/null | grep -avE '\[JIT\]' | tail -1)
    jitonly=$(comm -13 "$OUT/${label}_cog.fails" "$OUT/${label}_jit.fails" 2>/dev/null | wc -l | tr -d ' ')
    printf '%s\tok\t%s\t%s\t%s\n' "$label" "${cog:--}" "${jit:--}" "${jitonly:-0}" >> "$SUMMARY"
    echo "   cog[$cog] jit[$jit] jit-only-fails=$jitonly"
    rm -f "$img" "${img%.image}.changes"
done

echo; echo "=== summary ($SUMMARY) ==="
awk -F'\t' 'NR>1{n++; if($2=="ok")ok++; if($5+0>0)reg++} END{
  printf "packages: %d | loaded: %d | load-failed: %d | with JIT-only failures: %d\n", n, ok, n-ok, reg}' "$SUMMARY"
echo "rows with JIT-only failures (candidate regressions):"
awk -F'\t' 'NR>1 && $5+0>0{print "  "$1": "$5" jit-only ("$4")"}' "$SUMMARY"
