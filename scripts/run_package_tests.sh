#!/bin/bash
# Run higher-level package test suites against our VM.
# Usage: ./scripts/run_package_tests.sh [package-name ...]
# Without arguments, runs all packages in order.
# Results: /tmp/pkg_test_results_all.txt (combined summary)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
VM="$PROJECT_DIR/build/test_load_image"
COMBINED="/tmp/pkg_test_results_all.txt"
WORKDIR="/tmp/pkg_tests"

if [ ! -x "$VM" ]; then
  echo "ERROR: VM not found at $VM — run cmake --build build first"
  exit 1
fi

if [ ! -f /tmp/pharo ]; then
  echo "ERROR: Stock Pharo VM not found at /tmp/pharo"
  echo "  cd /tmp && curl -sL https://get.pharo.org/64/130 | bash"
  exit 1
fi

# Package definitions: name|metacello-expression|test-pattern
PACKAGES=(
  "NeoJSON|Metacello new repository: 'github://svenvc/NeoJSON/repository'; baseline: 'NeoJSON'; load.|NeoJSON"
  "Mustache|Metacello new baseline: 'Mustache'; repository: 'github://noha/mustache:v1.4/repository'; load.|Mustache"
  "XMLParser|Metacello new baseline: 'XMLParser'; repository: 'github://pharo-contributions/XML-XMLParser/src'; load.|XML"
  "Grease|Metacello new baseline: 'Grease'; repository: 'github://SeasideSt/Grease:master/repository'; load.|Grease"
  "PolyMath|Metacello new repository: 'github://PolyMathOrg/PolyMath'; baseline: 'PolyMath'; load.|PM"
  "DataFrame|Metacello new baseline: 'DataFrame'; repository: 'github://PolyMathOrg/DataFrame/src'; load.|DataFrame"
  "Fuel|Metacello new repository: 'github://theseion/Fuel:Pharo12/repository'; baseline: 'Fuel'; load.|Fuel"
)

# Parse which packages to run
packages_to_run=()
if [ $# -gt 0 ]; then
  for arg in "$@"; do
    for entry in "${PACKAGES[@]}"; do
      name="${entry%%|*}"
      if [ "$name" = "$arg" ]; then
        packages_to_run+=("$entry")
      fi
    done
  done
else
  packages_to_run=("${PACKAGES[@]}")
fi

if [ ${#packages_to_run[@]} -eq 0 ]; then
  echo "ERROR: No matching packages found"
  echo "Available: NeoJSON Mustache XMLParser Grease PolyMath DataFrame Fuel"
  exit 1
fi

mkdir -p "$WORKDIR"
echo "=== Package Test Suite ===" | tee "$COMBINED"
echo "Date: $(date)" | tee -a "$COMBINED"
echo "" | tee -a "$COMBINED"

for entry in "${packages_to_run[@]}"; do
  IFS='|' read -r pkg load_expr pattern <<< "$entry"

  echo "========================================" | tee -a "$COMBINED"
  echo "PACKAGE: $pkg" | tee -a "$COMBINED"
  echo "========================================" | tee -a "$COMBINED"

  PKG_DIR="$WORKDIR/$pkg"
  rm -rf "$PKG_DIR"
  mkdir -p "$PKG_DIR"

  # 1. Fresh image
  echo "  Downloading fresh Pharo 13 image..."
  if ! (cd "$PKG_DIR" && curl -sL https://get.pharo.org/64/130 | bash) > /dev/null 2>&1; then
    echo "  ERROR: Failed to download image" | tee -a "$COMBINED"
    continue
  fi

  # 2. Load package via Metacello
  echo "  Loading $pkg via Metacello (timeout 5min)..."
  if timeout 300 /tmp/pharo --headless "$PKG_DIR/Pharo.image" eval --save \
    "$load_expr" > "$PKG_DIR/load.log" 2>&1; then
    echo "  Loaded OK."
  else
    RC=$?
    echo "  Load exited with code $RC (checking if image was saved...)"
    if [ ! -f "$PKG_DIR/Pharo.image" ]; then
      echo "  ERROR: No image after load" | tee -a "$COMBINED"
      continue
    fi
  fi

  # 3. Write startup.st that will run the tests on image startup
  #    Uses StartupPreferencesLoader (auto-loads startup.st from working dir)
  cat > "$PKG_DIR/startup.st" << STEOF
"Package test runner for $pkg — auto-loaded by StartupPreferencesLoader"
| testClasses stream detail totalP totalF totalE pattern |
pattern := '$pattern'.
stream := (Smalltalk imageDirectory / 'pkg_test_results.txt') writeStream.
detail := (Smalltalk imageDirectory / 'pkg_test_detail.txt') writeStream.
totalP := 0. totalF := 0. totalE := 0.

testClasses := TestCase allSubclasses select: [:cls |
  ([cls isAbstract] on: Error do: [:e | true]) not and: [
    (cls package name includesSubstring: pattern caseSensitive: false)
      or: [cls name asString includesSubstring: pattern caseSensitive: false]]].

testClasses := testClasses asSortedCollection: [:a :b | a name < b name].

stream nextPutAll: '=== Package Tests: '; nextPutAll: pattern; nextPutAll: ' ==='; cr.
stream nextPutAll: 'Found '; print: testClasses size; nextPutAll: ' test classes'; cr; cr.
stream flush.
Stdio stderr nextPutAll: 'PkgTestRunner: '; nextPutAll: pattern;
  nextPutAll: ' — '; print: testClasses size; nextPutAll: ' test classes'; cr.

"Patch test timeouts for interpreter VM (10x)"
[TestCase compile: 'defaultTimeLimit ^ 100 seconds' classified: 'running'.
 TestCase allSubclasses do: [:cls2 |
   (cls2 includesSelector: #defaultTimeLimit) ifTrue: [
     cls2 removeSelector: #defaultTimeLimit]]]
on: Error do: [:e | ].

testClasses do: [:tc | | suite result startMs elapsedMs |
  stream nextPutAll: '=== '; nextPutAll: tc name; nextPutAll: ' ==='; cr. stream flush.
  Stdio stderr nextPutAll: '  Running: '; nextPutAll: tc name; cr.
  startMs := Time millisecondClockValue.
  [suite := tc buildSuite.
   result := [suite run] on: Error do: [:e |
     stream nextPutAll: '  RUNNER-ERROR: '; nextPutAll: e class name;
       nextPutAll: ' - '; nextPutAll: (e messageText copyFrom: 1 to: (e messageText size min: 200)); cr.
     nil].
   elapsedMs := Time millisecondClockValue - startMs.
   result ifNotNil: [
     stream nextPutAll: '  Pass: '; print: result passedCount;
       nextPutAll: '  Fail: '; print: result failureCount;
       nextPutAll: '  Error: '; print: result errorCount;
       nextPutAll: '  ('; print: elapsedMs; nextPutAll: 'ms)'; cr.
     totalP := totalP + result passedCount.
     totalF := totalF + result failureCount.
     totalE := totalE + result errorCount.
     result failures do: [:f |
       detail nextPutAll: 'FAIL '; nextPutAll: f class name;
         nextPutAll: ' >> '; nextPutAll: f selector; cr.
       stream nextPutAll: '  FAIL: '; nextPutAll: f selector; cr].
     result errors do: [:f |
       detail nextPutAll: 'ERROR '; nextPutAll: f class name;
         nextPutAll: ' >> '; nextPutAll: f selector; cr.
       stream nextPutAll: '  ERROR: '; nextPutAll: f selector; cr]].
   stream flush. detail flush]
  on: Error do: [:e |
    stream nextPutAll: '  CLASS-ERROR: '; nextPutAll: e class name;
      nextPutAll: ' - '; nextPutAll: (e messageText copyFrom: 1 to: (e messageText size min: 200)); cr.
    stream flush]].

stream cr.
stream nextPutAll: '=== TOTAL ==='; cr.
stream nextPutAll: 'Pass: '; print: totalP; cr.
stream nextPutAll: 'Fail: '; print: totalF; cr.
stream nextPutAll: 'Error: '; print: totalE; cr.
stream nextPutAll: 'Total: '; print: totalP + totalF + totalE; cr.
stream nextPutAll: '=== COMPLETE ==='; cr.
stream close. detail close.
Stdio stderr nextPutAll: 'PkgTestRunner: DONE Pass='; print: totalP;
  nextPutAll: ' Fail='; print: totalF; nextPutAll: ' Error='; print: totalE; cr.
[Smalltalk exitSuccess] on: Error do: [:e |].
Smalltalk quitPrimitive.
STEOF

  # 4. Clean old results
  rm -f "$PKG_DIR/pkg_test_results.txt" "$PKG_DIR/pkg_test_detail.txt"

  # 5. Run with our VM
  echo "  Running tests with our VM (timeout 10min)..."
  timeout 600 "$VM" "$PKG_DIR/Pharo.image" > "$PKG_DIR/vm.log" 2>&1 || true

  # 6. Collect results
  if [ -f "$PKG_DIR/pkg_test_results.txt" ]; then
    cat "$PKG_DIR/pkg_test_results.txt" | tee -a "$COMBINED"
  else
    echo "  ERROR: No results file produced" | tee -a "$COMBINED"
    echo "  Last 30 lines of VM output:" | tee -a "$COMBINED"
    tail -30 "$PKG_DIR/vm.log" 2>/dev/null | tee -a "$COMBINED"
  fi
  echo "" | tee -a "$COMBINED"
done

echo "========================================" | tee -a "$COMBINED"
echo "ALL DONE" | tee -a "$COMBINED"
echo ""
echo "Combined results: $COMBINED"
echo "Per-package dirs: $WORKDIR/<package>/"
