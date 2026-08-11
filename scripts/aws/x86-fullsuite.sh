#!/usr/bin/env bash
# x86-fullsuite.sh — RUNS ON THE BOX.  Single-config full-suite run of the
# jit branch @ HEAD on native x86 Linux.  Distilled from sunit-fullsuite-ab.sh
# (same silent-runner checklist), minus the A/B second config.
set -uo pipefail
REPO=/home/ubuntu/src/iospharo
HARNESS=/home/ubuntu/harness
STOCK=/home/ubuntu/stockvm
OUT=/home/ubuntu/results
VM="$REPO/build/test_load_image"
mkdir -p "$OUT"

# Stock Pharo VM.  Needed because our VM cannot file the SUnit runner into the
# image itself -- the prep step below is what installs SUnitRunner.
#
# get.pharo.org/64/vm130 serves x86_64 ONLY.  On aarch64 it returns something
# that is not a zip, unzip fails, and (because the whole thing was
# >/dev/null 2>&1) the script carried on with no VM: the prep silently did
# nothing, the image saved WITHOUT SUnitRunner, and the run reported
# "run exit=0" while producing zero results.  Fetch the right VM per arch, and
# fail loudly if we end up without one.
if [ ! -x "$STOCK/pharo" ]; then
    echo "== installing stock pharo VM ($(uname -m)) =="
    rm -rf "$STOCK"; mkdir -p "$STOCK"
    case "$(uname -m)" in
        x86_64)
            (cd "$STOCK" && curl -sL https://get.pharo.org/64/vm130 | bash) >/dev/null 2>&1
            ;;
        aarch64|arm64)
            # Pharo publishes no aarch64 build under get.pharo.org/64/vm130;
            # the newest Linux-aarch64 headless VM is the 10.2.x line.
            _base=https://files.pharo.org/vm/pharo-spur64-headless/Linux-aarch64
            _zip=$(curl -sL "$_base/" 2>/dev/null \
                   | grep -oE 'PharoVM-10\.2\.[0-9]+-[a-z0-9]+-Linux-aarch64-bin\.zip' \
                   | sort -V | tail -1)
            if [ -n "$_zip" ]; then
                (cd "$STOCK" && curl -sLO "$_base/$_zip" && unzip -q "$_zip" && rm -f "$_zip")
            fi
            ;;
    esac
fi
if [ ! -x "$STOCK/pharo" ]; then
    echo "FATAL: no stock Pharo VM available for $(uname -m)." >&2
    echo "  The prep step cannot install SUnitRunner without one, and the run" >&2
    echo "  would report success while producing no results." >&2
    exit 1
fi
echo "== stock VM: $("$STOCK/pharo" --version 2>&1 | head -1) =="
# ALWAYS a fully-fresh harness: image+changes+sources must come from the SAME
# fetch.  Re-downloading image/changes next to a stale .sources produced a
# source-misaligned image (argumentNames #(), garbage sourceCode) that failed
# ~700 AST/decompiler/FFI-parser tests and mimicked a VM regression
# (box run #4, 2026-07-06).
rm -rf "$HARNESS"; mkdir -p "$HARNESS"
(cd "$HARNESS" && curl -sL https://get.pharo.org/64/130 | bash >/dev/null 2>&1)

cp "$REPO/scripts/pharo-headless-test/test_classes.txt" /tmp/sunit_test_classes.txt
# full suite: no batch file, no class-names filter -> runner runs everything
rm -f /tmp/sunit_class_names.txt /tmp/sunit_batch.txt

echo "== prep: fileIn fake GUI + SUnit runner =="
rm -f "$HARNESS/startup.st"
"$STOCK/pharo" "$HARNESS/Pharo.image" eval --save \
    "'$REPO/scripts/pharo-headless-test/setup_fake_gui.st' asFileReference fileIn. '$REPO/scripts/pharo-headless-test/run_sunit_tests.st' asFileReference fileIn" 2>&1 | tail -2

echo "== run: full suite =="
rm -f /tmp/sunit_run_completed.txt "$HARNESS/startup.st" \
      /tmp/sunit_test_results.txt /tmp/sunit_test_detail.txt \
      "$HARNESS/PharoDebug.log"
env PHARO_MAX_STEPS=4000000000000 PHARO_CODE_ZONE_MB=192 \
    timeout 14400 "$VM" "$HARNESS/Pharo.image" \
    > "$OUT/run_full.log" 2>&1
rc=$?
echo "run exit=$rc"
cp /tmp/sunit_test_results.txt "$OUT/results_full.txt" 2>/dev/null || {
    echo "!! no results — check $OUT/run_full.log and PharoDebug.log"
    cp "$HARNESS/PharoDebug.log" "$OUT/pharodebug_full.log" 2>/dev/null
}
cp /tmp/sunit_test_detail.txt "$OUT/detail_full.txt" 2>/dev/null
tail -20 "$OUT/results_full.txt" 2>/dev/null
