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
#   * On macOS the ARM64 stock VM aborts before it starts, "Could not allocate
#     codeZone in the expected place (0x320000000)". That is the ASLR problem
#     this project exists to solve.
#     CORRECTION 2026-09-02: that is arch-specific, not host-specific. The
#     x86_64 stock VM runs fine under Rosetta and gives this machine a Cog
#     baseline again:
#         mkdir -p /tmp/harness-x86 && cd /tmp/harness-x86
#         arch -x86_64 /bin/bash -c 'curl -sL https://get.pharo.org/64/130+vm | bash'
#         arch -x86_64 ./pharo Pharo.image eval '42 factorial printString'
#     Verified on Cog v10.3.9, `eval` and `eval --save` both. Every stock-VM
#     command needs the `arch -x86_64` prefix or you get the abort above. So a
#     Cog-loaded package baseline IS obtainable now; this script staying
#     self-hosted is still the right default (driving Metacello with our own VM
#     is itself a workload worth exercising), but "no reference VM" is no
#     longer the reason.
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
# Seconds per TestCase subclass.  SIZE THIS FOR THE ARCHITECTURE: an x86_64
# (Rosetta) run needs roughly twice what arm64 does, and the bound failing is
# indistinguishable in the summary from the tests failing.  Measured
# 2026-08-22: PolyMath's PMArbitraryPrecisionFloatTest took 186 s on x86_64
# against a 180 s bound -- six seconds over -- and the resulting TIMEOUT cost
# 57 passes, which is 57 of the 59-test gap between the two architectures'
# PolyMath scores.  Re-run alone with a 900 s bound it answers exactly what
# arm64 answers: 58 ran, 57 passed, 1 error.  Use 360+ for x86_64.
PER_CLASS_TIMEOUT=${PER_CLASS_TIMEOUT:-120}   # seconds, per TestCase subclass
# Seconds for SUnit's OWN per-test limit (TestCase class>>defaultTimeLimit,
# enforced by runCaseManaged).  0 = leave the image default alone, which is
# 10 s.  This is NOT the same bound as PER_CLASS_TIMEOUT above: that one is
# ours and covers a whole class, this one is Pharo's and covers one test.
#
# Measured 2026-08-23, XMLParser's five testAttributeDefaultValue* cases:
#
#     test                               arm64      x86_64    limit
#     testAttributeDefaultValueNmtokens   7004 ms             10000 ms
#     testAttributeDefaultValueEntity     8734 ms   11595 ms  10000 ms
#     testAttributeDefaultValueEntities   8784 ms             10000 ms
#     testAttributeDefaultValueIDRef      8856 ms             10000 ms
#     testAttributeDefaultValueIDRefs     8916 ms             10000 ms
#
# arm64 at ~88% of the allowance, x86_64 at ~116%, so x86_64 reported five
# `TestTookTooMuchTime` errors where arm64 reported one -- the entire arm/x86
# gap in the package tier, and nothing to do with VM computation.
#
# Raise it to compare architectures on equal terms.  But note these tests use
# 88% of the allowance on the FAST arch, and it is NOT established that ~8.8 s
# is reasonable for them; if stock Cog runs them in a fraction of that, the
# real finding is a VM performance gap and raising this only hides it.
PER_TEST_TIMEOUT=${PER_TEST_TIMEOUT:-0}       # seconds; 0 = image default (10)
# REUSE_FROM=<dir>: skip the Metacello load and take each package's already
# loaded pkg.image from a previous run's work directory. Spur images are
# architecture-neutral, so an image loaded by the arm64 VM runs unchanged under
# the x86_64 one. This exists because the load itself cannot work on every
# host: Iceberg resolves github:// through libgit2 over FFI, and a machine with
# only an arm64 libgit2 (no Intel Homebrew under /usr/local) fails every
# X86_64 load with `IceGenericError: no error message set by libgit2`. The
# arm64 VM on the same host loads github:// fine -- the failure is scoped to the
# x86_64 binary, not to the host. Measured
# 2026-08-19, all 7 packages, each in ~5s with the image never growing.
# Loading on arm and testing on x86 is the way to get x86 package numbers there.
REUSE_FROM=${REUSE_FROM:-}

# name | metacello expression | test-class name pattern
PACKAGES=(
  "NeoJSON|Metacello new repository: 'github://svenvc/NeoJSON/repository'; baseline: 'NeoJSON'; load.|NeoJSON"
  "Mustache|Metacello new baseline: 'Mustache'; repository: 'github://noha/mustache:v1.4/repository'; load.|Mustache"
  "XMLParser|Metacello new baseline: 'XMLParser'; repository: 'github://pharo-contributions/XML-XMLParser/src'; load.|XML"
  # Grease keeps its tests OUT of the 'default' group: under the baseline's
  # #'pharo13.x' block, default -> Slime -> Grease-Pharo110-Slime-Core, which
  # closes over three CODE packages and no test package. Plain `load.` therefore
  # loaded cleanly (clone 89606edf, rc=0, 27s) and added ZERO TestCase
  # subclasses, reported as "classes=0 pass=0" -- indistinguishable from a failed
  # load. Ask for the Tests group explicitly. The fallback pattern is 'GR', not
  # 'Grease': every Grease test class is GR-prefixed (GRPharoConverterTest etc)
  # and the string 'Grease' matches none of them.
  "Grease|Metacello new baseline: 'Grease'; repository: 'github://SeasideSt/Grease:master/repository'; load: #('default' 'Tests').|GR"
  "PolyMath|Metacello new baseline: 'PolyMath'; repository: 'github://PolyMathOrg/PolyMath:master/src'; load.|PolyMath"
  "DataFrame|Metacello new baseline: 'DataFrame'; repository: 'github://PolyMathOrg/DataFrame/src'; load.|DataFrame"
  # 2026-08-22: github.com/pharo-project/pharo-fuel is GONE -- 404 on the web
  # page, 401 "Repository not found" on .git/info/refs.  Iceberg tries SSH
  # first (auth error, normal -- every package here logs it), falls back to
  # HTTPS, and then sits in libgit2's http_stream_read against a repository
  # that does not exist: 1042 s with 20 KB in pharo-local, where a working
  # clone (NeoJSON) has 1.5 MB inside 21 s.  It is NOT a VM defect and it is
  # not a regression either: the 2026-08-17 run's "Fuel load 4 s" produced the
  # same 2 classes / 19 passes this run does, and those come from the Fuel
  # that is already IN the base image -- this entry has never actually loaded
  # anything from that URL.
  #
  # theseion/Fuel is the surviving repository and it does clone -- but its
  # BASELINE is broken too, so Fuel is currently unloadable from ANY upstream.
  # Measured 2026-08-22:
  #
  #   pharo-project/pharo-fuel  404 on the page, 401 on .git/info/refs;
  #                             libgit2 sits in http_stream_read 17+ min
  #   theseion/Fuel             clones, loads Fuel-Core, then in 37 s:
  #                             "KeyNotFound: key 'Fuel-Core-Tests' not found"
  #                             -- BaselineOfFuel names Fuel-Core-Tests but the
  #                             repository ships Fuel-Tests-Core.  There is no
  #                             'core' group either ("Name not found: core").
  #
  # Kept pointed here anyway because failing in 37 s with a clear error beats
  # hanging the whole tier for 17 minutes.  Either way this entry's 2 classes /
  # 19 passes come from the Fuel ALREADY IN the base image, not from a load --
  # which is also true of the 2026-08-17 run that recorded "load 4 s".
  # Do not spend time on it again without first checking upstream is fixed.
  "Fuel|Metacello new baseline: 'Fuel'; repository: 'github://theseion/Fuel:master/repository'; load.|Fuel"
)

[ -x "$VM" ]   || { echo "no such VM binary: $VM" >&2; exit 1; }
[ -f "$BASE" ] || { echo "no such image: $BASE" >&2; exit 1; }
BASE_CHANGES="${BASE%.image}.changes"

# The .sources file has to travel with the image. Pharo reads class comments
# out of it at RUNTIME, so without it `Object comment` answers nil and every
# test that touches a comment fails for a reason that has nothing to do with
# the package under test -- measured 2026-08-18, see scripts/sunit-sweep.sh's
# header for the four classes and 69 errors it cost there.
BASE_DIR=$(cd "$(dirname "$BASE")" && pwd)
BASE_SOURCES=$(ls "$BASE_DIR"/*.sources 2>/dev/null | head -1)
[ -n "$BASE_SOURCES" ] || BASE_SOURCES=$(find "$BASE_DIR" -maxdepth 3 -name '*.sources' 2>/dev/null | head -1)
[ -n "$BASE_SOURCES" ] || { echo "no .sources file for $BASE -- class comments would all read nil" >&2; exit 1; }

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
    if [ -n "$REUSE_FROM" ]; then
        if [ ! -f "$REUSE_FROM/$PKG/pkg.image" ]; then
            printf '%-12s REUSE  no image in %s -- skipped\n' "$PKG" "$REUSE_FROM/$PKG" >> "$SUMMARY"
            continue
        fi
        cp "$REUSE_FROM/$PKG/pkg.image"   "$D/pkg.image"
        cp "$REUSE_FROM/$PKG/pkg.changes" "$D/pkg.changes" 2>/dev/null
        cp "$REUSE_FROM/$PKG/pre.txt"     "$D/pre.txt"     2>/dev/null
        cp "$BASE_SOURCES" "$D/"
        sz=$(stat -f%z "$D/pkg.image" 2>/dev/null || stat -c%s "$D/pkg.image" 2>/dev/null || echo 0)
        printf '%-12s reuse  from %s  image=%sMB\n' "$PKG" "$(basename "$REUSE_FROM")" "$((sz/1024/1024))" >> "$SUMMARY"
    else
    cp "$BASE" "$D/pkg.image"
    cp "$BASE_CHANGES" "$D/pkg.changes"
    cp "$BASE_SOURCES" "$D/"
    fi

    # --- load, and persist with a snapshot rather than --save ---
    if [ -z "$REUSE_FROM" ]; then
    t0=$(date +%s)
    # Write the pre-load TestCase subclasses first: the test pass below selects
    # the classes THIS LOAD ADDED, which is exact, rather than the classes whose
    # name contains $PATTERN, which is a guess. Measured 2026-08-18: 'PolyMath'
    # and 'Grease' each matched ZERO test classes after a load that had plainly
    # worked, because their tests are named PM*Test and GR*Test. Both reported
    # as "classes=0 pass=0", indistinguishable from a failed load.
    timeout "$LOAD_TIMEOUT" "$VM" "$D/pkg.image" eval \
        "'$D/pre.txt' asFileReference writeStreamDo: [ :f |
             TestCase allSubclasses do: [ :c | f nextPutAll: c name; lf ] ].
         $LOAD_EXPR Smalltalk snapshot: true andQuit: true" \
        > "$D/load.log" 2>&1
    load_rc=$?
    t1=$(date +%s)

    # Did the load actually take? A grown image is the cheap check; the real one
    # is whether the test classes exist, which the run below answers.
    size=$(stat -f%z "$D/pkg.image" 2>/dev/null || stat -c%s "$D/pkg.image" 2>/dev/null || echo 0)
    base_size=$(stat -f%z "$BASE" 2>/dev/null || stat -c%s "$BASE" 2>/dev/null || echo 0)
    grew=grew
    [ "$size" -le "$base_size" ] && grew=DID-NOT-PERSIST
    # Distinguish "the clone never happened" from "the VM failed the load".
    # Both produce an unchanged image, and DID-NOT-PERSIST alone has been read
    # as a VM defect more than once.  The clone failure looks like:
    #     Error: IceGenericError: no error message set by libgit2
    # and the load then exits in 4-5 s having done nothing.
    #
    # The commonest cause is NOT the network.  FFI resolves bare library names
    # through getLibSearchPaths(), which searches THE EXECUTABLE'S OWN DIRECTORY
    # FIRST (src/vm/FFI.cpp).  Only build-x86/ holds an x86_64 libgit2.dylib;
    # /opt/homebrew/lib/libgit2.dylib is arm64-only.  So an x86_64 VM COPIED
    # anywhere else falls back to the arm64 dylib, dlopen refuses it, and every
    # github:// load fails instantly -- while the same binary run from
    # build-x86/ loads fine, and any arm64 VM works from anywhere because the
    # homebrew dylib matches it.  Measured 2026-08-22, after a copy of the x86
    # VM in a scratch dir failed 5/5 on XMLParser and NeoJSON alike with a plain
    # `git clone` of the same URLs still returning 0.
    #
    # Run x86_64 VMs from build-x86/, or stage libgit2*.dylib beside them.
    if grep -qE "IceGenericError|libgit2|failed to resolve address" "$D/load.log" 2>/dev/null; then
        grew="CLONE-FAILED(network, not the VM)"
    fi
    printf '%-12s load rc=%-3s %4ds  image=%sMB %s\n' \
        "$PKG" "$load_rc" "$((t1-t0))" "$((size/1024/1024))" "$grew" >> "$SUMMARY"
    fi

    # --- run every TestCase subclass whose name matches the pattern ---
    rm -f "$D/result.txt"
    # Select the TestCase subclasses this load ADDED (pre.txt vs now). The name
    # pattern is only a fallback for when pre.txt is missing -- i.e. the load
    # step never ran -- OR the package already ships in the base image and so
    # adds no classes at all. Fuel is the case that proves the fallback is
    # needed: it is already present, the diff is empty, and only the pattern
    # finds its 19 tests.
    #
    # Failing SELECTORS are listed under each class line. TestResult printString
    # reports COUNTS ONLY, and that alone left two real questions unanswerable
    # after the fact on 2026-08-19: which 5th XMLParserTest case errored on x86,
    # and which of PMGeneralFunctionFitTest's two slow tests produced its single
    # error. Counts cannot be reconciled against a re-run; names can.
    #
    # The result file is rewritten after EVERY class, ending in a PARTIAL line
    # that names the last class to finish. Writing it only at the end -- as this
    # did until 2026-08-18 -- means anything that kills the eval between the last
    # class and the final write discards every result and names nothing. That is
    # precisely what "XMLParser ... produces no RESULT" was: 110M activations of
    # tests that plainly ran, reported as silence. PolyMath does the same.
    #
    # Each class also gets its own timeout, because one class that never returns
    # otherwise swallows every class after it: PMAB2SolverTest goes idle after
    # ~2 minutes and never finishes, which is why PolyMath reported 4 of its 117
    # classes and nothing about the other 113.
    #
    # NOTE: the eval below is one double-quoted shell string, so it must contain
    # no bare double quote -- a Smalltalk "comment" inside it ends the string and
    # the rest is executed as shell words.
    t2=$(date +%s)
    timeout "$TEST_TIMEOUT" "$VM" "$D/pkg.image" eval "
| pat pre added classes s tp tf te tt |
pat := '$PATTERN'.
$PER_TEST_TIMEOUT > 0 ifTrue: [ TestCase defaultTimeLimit: $PER_TEST_TIMEOUT seconds ].
s := WriteStream on: String new.
tp := 0. tf := 0. te := 0. tt := 0.
classes := (TestCase allSubclasses
    reject: [ :c | [ c isAbstract ] on: Error do: [ :e | true ] ]).
pre := ('$D/pre.txt' asFileReference exists)
    ifTrue: [ '$D/pre.txt' asFileReference contents lines asSet ]
    ifFalse: [ Set new ].
added := pre isEmpty
    ifTrue: [ #() ]
    ifFalse: [ classes reject: [ :c | pre includes: c name ] ].
classes := added isEmpty
    ifTrue: [ classes select: [ :c | c name includesSubstring: pat ] ]
    ifFalse: [ added ].
classes := classes asSortedCollection: [ :a :b | a name <= b name ].
classes withIndexDo: [ :c :i |
    | r timedOut |
    timedOut := false.
    r := [ [ c suite run ]
             valueWithin: (Duration seconds: $PER_CLASS_TIMEOUT)
             onTimeout: [ timedOut := true. nil ] ]
           on: Error do: [ :e | nil ].
    timedOut
      ifTrue: [ tt := tt + 1.
                s nextPutAll: c name , ': TIMEOUT after $PER_CLASS_TIMEOUT s'; lf ]
      ifFalse: [
        r ifNil: [ s nextPutAll: c name , ': DIED'; lf ]
          ifNotNil: [
            tp := tp + r expectedPassCount.
            tf := tf + r failureCount.
            te := te + r errorCount.
            s nextPutAll: c name , ': ' , r printString; lf.
            ([ r errors do: [ :ec | s nextPutAll: '    E: ' , ec selector; lf ].
               r failures do: [ :fc | s nextPutAll: '    F: ' , fc selector; lf ] ]
                 on: Error do: [ :e | s nextPutAll: '    (selector list unavailable)'; lf ]) ] ].
    '$D/result.txt' asFileReference writeStreamDo: [ :f |
        f nextPutAll: s contents;
          nextPutAll: 'PARTIAL after ' , i printString , '/' , classes size printString ,
                      ' last=' , c name; lf ] ].
s nextPutAll: 'RESULT classes=' , classes size printString ,
    ' pass=' , tp printString , ' fail=' , tf printString ,
    ' err=' , te printString , ' timeout=' , tt printString; lf.
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
