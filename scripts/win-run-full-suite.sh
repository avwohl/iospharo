#!/usr/bin/env bash
# Run the full official SUnit suite on Windows with OUR JIT VM.
#
# Why the dance: our VM's OpalCompiler can't compile the runner's huge
# runAllTests method (convertStorePop bug, see docs/deferred.md), so we inject
# the runner with the REFERENCE Pharo Windows VM (whose compiler handles it),
# then run the now-pre-compiled runAllTests with our VM. Run from a NATIVE Git
# Bash shell (USERPROFILE must be set; NOT the MSYS2 login shell).
#
# Results: C:\tmp\sunit_test_results.txt (+ sunit_test_detail.txt)
set -u
REPO=/c/temp/src/iospharo-jit
IMG_SRC="${1:-C:/temp/pharo-win-test/Pharo.image}"
WORK=$(dirname "$(cygpath -u "$IMG_SRC" 2>/dev/null || echo "$IMG_SRC")")
PREPPED="$WORK/Pharo-sunit.image"
REFVM="$WORK/refvm/pharo-vm/PharoConsole.exe"
RUNNER="$REPO/scripts/pharo-headless-test/run_sunit_tests.st"
OURVM="$REPO/build-win/test_load_image.exe"

mkdir -p /c/tmp
[ -f "$OURVM" ] || { echo "Build our VM first (scripts/build-windows.sh)"; exit 1; }

if [ ! -x "$REFVM" ]; then
  echo "Reference Pharo Windows VM not found at $REFVM"
  echo "Get it: cd '$WORK' && mkdir -p refvm && cd refvm && curl -sL https://get.pharo.org/64/vm130 | bash"
  exit 1
fi

echo "== Inject runner with reference VM (compiles runAllTests) =="
cp -f "${IMG_SRC%.image}.image" "$PREPPED"
cp -f "${IMG_SRC%.image}.changes" "${PREPPED%.image}.changes" 2>/dev/null || true
"$REFVM" "$(cygpath -w "$PREPPED" 2>/dev/null || echo "$PREPPED")" eval --save \
  "'$(cygpath -m "$RUNNER" 2>/dev/null || echo "$RUNNER")' asFileReference fileIn. (Smalltalk includesKey: #SUnitRunner) printString" \
  2>&1 | grep -iE "true|false|error" | head -2

echo "== Run the suite with OUR JIT VM (direct runAllTests) =="
rm -f /c/tmp/sunit_run_completed.txt /c/tmp/sunit_test_results.txt /c/tmp/sunit_test_detail.txt
timeout "${TIMEOUT:-3000}" "$OURVM" \
  "$(cygpath -m "$PREPPED" 2>/dev/null || echo "$PREPPED")" \
  eval "(Smalltalk at: #SUnitRunner) runAllTests" > /c/tmp/fullsuite.log 2>&1
echo "exit=$?"

echo "== Results =="
cat /c/tmp/sunit_test_results.txt 2>/dev/null || echo "(no results — see /c/tmp/fullsuite.log)"
