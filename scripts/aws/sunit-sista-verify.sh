#!/usr/bin/env bash
# sunit-sista-verify.sh — RUNS ON THE BOX.  Correctness gate for the x86 Sista
# port: runs an SUnit subset twice on the custom VM — once WITHOUT and once WITH
# PHARO_SISTA_DISPATCH=1 — and diffs the per-test results.  Identical results
# mean Sista (incl. the newly-ported ops) computes the same as the known-good
# tier-1 path.  Differences pinpoint a miscompiled op.
#
#   ./sunit-sista-verify.sh ["ClassA ClassB ..."]   (default: collection/float subset)
set -uo pipefail
REPO=/home/ubuntu/src/iospharo
HARNESS=/home/ubuntu/harness
STOCK=/home/ubuntu/stockvm
SUBSET="${1:-FloatTest ArrayTest OrderedCollectionTest IntervalTest Float64ArrayTest SetTest DictionaryTest}"

# Stock Pharo VM (used only to fileIn the runner into the image for prep).
if [ ! -x "$STOCK/pharo" ]; then
    echo "== installing stock pharo VM =="
    rm -rf "$STOCK"; mkdir -p "$STOCK"
    (cd "$STOCK" && curl -sL https://get.pharo.org/64/vm130 | bash) >/dev/null 2>&1
fi
[ -f "$HARNESS/Pharo.image" ] || (cd "$HARNESS" && curl -sL https://get.pharo.org/64/130 | bash >/dev/null 2>&1)

cp "$REPO/scripts/pharo-headless-test/test_classes.txt" /tmp/sunit_test_classes.txt
: > /tmp/sunit_class_names.txt
for c in $SUBSET; do echo "$c" >> /tmp/sunit_class_names.txt; done
echo "subset: $SUBSET"

echo "== prep: fileIn SUnit runner into image =="
"$STOCK/pharo" "$HARNESS/Pharo.image" eval --save \
    "'$REPO/scripts/pharo-headless-test/run_sunit_tests.st' asFileReference fileIn" 2>&1 | tail -2

run() {  # $1=label  $2=sista(0/1)
    rm -f /tmp/sunit_test_results.txt
    local pfx=""; [ "$2" = 1 ] && pfx="PHARO_SISTA_DISPATCH=1"
    env $pfx timeout 300 "$REPO/build/test_load_image" "$HARNESS/Pharo.image" \
        > "/tmp/sunit_$1.log" 2>&1
    echo "[$1] vm_exit=$?"
    cp -f /tmp/sunit_test_results.txt "/tmp/results_$1.txt" 2>/dev/null || echo "  (no results file)"
    echo "  summary: $(grep -ciE 'pass' /tmp/results_$1.txt 2>/dev/null) pass / $(grep -ciE 'fail|error' /tmp/results_$1.txt 2>/dev/null) fail+err  ($(wc -l </tmp/results_$1.txt 2>/dev/null) lines)"
    grep -E '\[SISTA-x86\] lower' "/tmp/sunit_$1.log" | tail -1
}

echo "== NO-SISTA baseline =="; run nosista 0
echo "== WITH-SISTA =====";    run sista   1

echo "== DIFF (per-test result lines; empty = identical) =="
if diff <(sort /tmp/results_nosista.txt 2>/dev/null) <(sort /tmp/results_sista.txt 2>/dev/null); then
    echo "IDENTICAL — Sista matches the tier-1 baseline. ✅"
else
    echo "^^ DIFFERENCES — a Sista-compiled method diverges; investigate."
fi
