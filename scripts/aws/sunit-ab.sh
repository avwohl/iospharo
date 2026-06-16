#!/usr/bin/env bash
# sunit-ab.sh — RUNS LOCALLY (orchestrator).  SUnit A/B on the x86 box without
# needing a working stock pharo ON the box.
#
# THE HARNESS FIX (2026-06-16):  test_load_image cannot parse `eval --save`, so
# the SUnit runner must be filed-in by a STOCK pharo during prep.  Prior attempts
# tried to install stock pharo on the EC2 box (get.pharo.org/64/vm…) and it never
# produced a runnable VM -> prep rc=127 -> the box ran an UN-prepped image -> the
# SUnitRunner/SessionManager handler was absent -> 0 results / full-suite timeout.
#
# A Spur image is arch-independent (x86 and arm64 both 64-bit LE), so we instead
# prep the image LOCALLY with a stock pharo we already have, then ship the
# PORTABLE prepped image (+ its matching .sources) to the box.  The box-side
# runner (sunit-ab-box.sh) just runs it with the runtime class filter — no prep,
# no stock pharo on the box.  Verified: x86 ON==OFF==arm64-golden, 12 classes
# 2271 pass, runs in ~12s each on -O2.
#
# Usage:  scripts/aws/sunit-ab.sh [class1 class2 ...]
#   default class set is the 12-class fast representative below.
#   Reads PUBLIC_IP/PEM from scripts/aws/state.env.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
source "$HERE/state.env"
PEM="${PEM:?set PEM in state.env}"
IP="${PUBLIC_IP:?set PUBLIC_IP in state.env}"
SSH="ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 -i $PEM ubuntu@$IP"
SCP="scp -o StrictHostKeyChecking=no -o ConnectTimeout=15 -i $PEM"

# --- local stock pharo + pristine image (macOS Pharo.app or any stock pharo CLI) ---
HARNESS="${HARNESS:-/tmp/harness}"
STOCK_PHARO="${STOCK_PHARO:-$HARNESS/pharo-vm/Pharo.app/Contents/MacOS/Pharo}"
[ -x "$STOCK_PHARO" ] || { echo "!! no stock pharo at $STOCK_PHARO — set STOCK_PHARO"; exit 1; }
PRISTINE="${PRISTINE:-$HARNESS/Pharo.image}"
SOURCES=$(ls "$HARNESS"/*.sources 2>/dev/null | head -1)
[ -f "$PRISTINE" ] || { echo "!! no pristine image at $PRISTINE"; exit 1; }
[ -f "$SOURCES" ]  || { echo "!! no .sources next to $PRISTINE"; exit 1; }

CLASSES=("$@")
[ ${#CLASSES[@]} -gt 0 ] || CLASSES=(SmallIntegerTest FractionTest CharacterTest \
  AssociationTest IntervalTest ArrayTest OrderedCollectionTest DictionaryTest \
  StringTest SetTest BagTest SymbolTest)

PREPPED="$HARNESS/Pharo-prepped.image"
echo "== local prep: fileIn SUnit runner into a portable image =="
cp "$PRISTINE" "$PREPPED"
cp "${PRISTINE%.image}.changes" "${PREPPED%.image}.changes" 2>/dev/null || true
timeout 180 "$STOCK_PHARO" "$PREPPED" eval --save \
  "'$REPO/scripts/pharo-headless-test/run_sunit_tests.st' asFileReference fileIn" \
  >/dev/null 2>&1
echo "   prepped: $(ls -la "$PREPPED" | awk '{print $5}') bytes"

echo "== ship portable prepped image + matching sources to box =="
$SSH 'mkdir -p /home/ubuntu/harness'
$SCP "$PREPPED" "${PREPPED%.image}.changes" "$SOURCES" ubuntu@"$IP":/home/ubuntu/harness/ 2>&1 | tail -1
printf '%s\n' "${CLASSES[@]}" > /tmp/sunit_ab_classes.txt
$SCP /tmp/sunit_ab_classes.txt "$HERE/sunit-ab-box.sh" ubuntu@"$IP":/tmp/ 2>&1 | tail -1

echo "== launch A/B on box (detached) =="
$SSH 'chmod +x /tmp/sunit-ab-box.sh; nohup /tmp/sunit-ab-box.sh > /tmp/sunit-ab-box.out 2>&1 & echo launched pid=$!'

echo "== poll for AB-FINISHED =="
until $SSH 'grep -q AB-FINISHED /tmp/sunit-ab-box.out 2>/dev/null'; do sleep 15; done
$SSH 'cat /tmp/sunit-ab-box.out'
echo "== done.  Tear down with: scripts/aws/teardown.sh --instance-only =="
