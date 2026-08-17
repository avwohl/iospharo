#!/bin/bash
#
# Package test suites run WITHOUT a stock Cog VM.
#
#   scripts/package-tests-selfhosted.sh <vm-binary> <base-image> [package ...]
#
# scripts/run_package_tests.sh needs `/tmp/pharo` -- a stock Cog VM -- to do the
# Metacello load, and then `eval --save` to persist it. Neither works
# everywhere:
#
#   * On macOS the stock VM aborts before it starts, "Could not allocate
#     codeZone in the expected place (0x320000000)". That is the ASLR problem
#     this project exists to solve, so the reference VM is unavailable on
#     exactly the platform we most want to test.
#   * `eval --save` is the stock VM's flag. Ours forwards it to the image's
#     command-line handler and nothing is written -- verified by setting a
#     global and finding it gone on the next launch.
#
# So this script does the load with OUR VM and persists with an explicit
# snapshot. If the load itself fails, that is a finding rather than a
# limitation: driving Metacello is a real workload and the reference VM manages
# it.
#
# Run on an idle machine; the suites are timing-sensitive.
set -u

VM=${1:?usage: package-tests-selfhosted.sh <vm-binary> <base-image> [package ...]}
BASE=${2:?usage: package-tests-selfhosted.sh <vm-binary> <base-image> [package ...]}
shift 2

WORK=${WORK:-/tmp/pkg-selfhosted}
LOAD_TIMEOUT=${LOAD_TIMEOUT:-900}
TEST_TIMEOUT=${TEST_TIMEOUT:-900}

# name | metacello expression | test-class name pattern
PACKAGES=(
  "NeoJSON|Metacello new repository: 'github://svenvc/NeoJSON/repository'; baseline: 'NeoJSON'; load.|NeoJSON"
  "Mustache|Metacello new baseline: 'Mustache'; repository: 'github://noha/mustache:v1.4/repository'; load.|Mustache"
  "XMLParser|Metacello new baseline: 'XMLParser'; repository: 'github://pharo-contributions/XML-XMLParser/src'; load.|XML"
  "Grease|Metacello new baseline: 'Grease'; repository: 'github://SeasideSt/Grease:master/repository'; load.|Grease"
  "PolyMath|Metacello new baseline: 'PolyMath'; repository: 'github://PolyMathOrg/PolyMath:master/src'; load.|PolyMath"
  "DataFrame|Metacello new baseline: 'DataFrame'; repository: 'github://PolyMathOrg/DataFrame/src'; load.|DataFrame"
  "Fuel|Metacello new baseline: 'Fuel'; repository: 'github://pharo-project/pharo-fuel:master/src'; load.|Fuel"
)

[ -x "$VM" ]   || { echo "no such VM binary: $VM" >&2; exit 1; }
[ -f "$BASE" ] || { echo "no such image: $BASE" >&2; exit 1; }
BASE_CHANGES="${BASE%.image}.changes"

mkdir -p "$WORK"
SUMMARY="$WORK/summary.txt"
: > "$SUMMARY"

want() {   # no package arguments means all of them
    [ "$#" -eq 0 ] && return 0
    for a in "$@"; do [ "$a" = "$PKG" ] && return 0; done
    return 1
}

for entry in "${PACKAGES[@]}"; do
    PKG=${entry%%|*}
    rest=${entry#*|}
    LOAD_EXPR=${rest%%|*}
    PATTERN=${rest##*|}
    want "$@" || continue

    D="$WORK/$PKG"
    rm -rf "$D"; mkdir -p "$D"
    cp "$BASE" "$D/pkg.image"
    cp "$BASE_CHANGES" "$D/pkg.changes"

    # --- load, and persist with a snapshot rather than --save ---
    t0=$(date +%s)
    timeout "$LOAD_TIMEOUT" "$VM" "$D/pkg.image" eval \
        "$LOAD_EXPR Smalltalk snapshot: true andQuit: true" \
        > "$D/load.log" 2>&1
    load_rc=$?
    t1=$(date +%s)

    # Did the load actually take? A grown image is the cheap check; the real one
    # is whether the test classes exist, which the run below answers.
    size=$(stat -f%z "$D/pkg.image" 2>/dev/null || stat -c%s "$D/pkg.image" 2>/dev/null || echo 0)
    printf '%-12s load rc=%-3s %4ds  image=%sMB\n' \
        "$PKG" "$load_rc" "$((t1-t0))" "$((size/1024/1024))" >> "$SUMMARY"

    # --- run every TestCase subclass whose name matches the pattern ---
    rm -f "$D/result.txt"
    t2=$(date +%s)
    timeout "$TEST_TIMEOUT" "$VM" "$D/pkg.image" eval "
| pat classes s tp tf te |
pat := '$PATTERN'.
s := WriteStream on: String new.
tp := 0. tf := 0. te := 0.
classes := (TestCase allSubclasses
    reject: [ :c | [ c isAbstract ] on: Error do: [ :e | true ] ])
    select: [ :c | c name includesSubstring: pat ].
classes := classes asSortedCollection: [ :a :b | a name <= b name ].
classes do: [ :c |
    | r |
    r := [ c suite run ] on: Error do: [ :e | nil ].
    r ifNil: [ s nextPutAll: c name , ': DIED'; lf ]
      ifNotNil: [
        tp := tp + r expectedPassCount.
        tf := tf + r failureCount.
        te := te + r errorCount.
        s nextPutAll: c name , ': ' , r printString; lf ] ].
s nextPutAll: 'RESULT classes=' , classes size printString ,
    ' pass=' , tp printString , ' fail=' , tf printString ,
    ' err=' , te printString; lf.
'$D/result.txt' asFileReference writeStreamDo: [ :f | f nextPutAll: s contents ]" \
        > "$D/test.log" 2>&1
    test_rc=$?
    t3=$(date +%s)

    line=$(grep -h '^RESULT' "$D/result.txt" 2>/dev/null)
    printf '%-12s test rc=%-3s %4ds  %s\n' \
        "$PKG" "$test_rc" "$((t3-t2))" "${line:-NO RESULT (see $D/test.log)}" >> "$SUMMARY"
done

echo "=== $SUMMARY ==="
cat "$SUMMARY"
