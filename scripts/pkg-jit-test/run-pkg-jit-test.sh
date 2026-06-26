#!/usr/bin/env bash
# Run a loaded package's SUnit suite on BOTH the custom JIT VM and stock Cog,
# then diff the pass/fail sets to surface JIT-correctness regressions.
#
# Prereq: a Pharo image that ALREADY has the package loaded (the stock Cog VM
# does the Metacello network load + save — our VM cannot do HTTPS).  See
# load_*.st in this dir / docs/jit-test-packages.md for the load expressions.
#
# Usage:
#   run-pkg-jit-test.sh <image> <label> <prefix>[,<prefix>...]
# e.g.
#   run-pkg-jit-test.sh /tmp/pkgtest/polymath.image polymath Math-Tests
#   run-pkg-jit-test.sh /tmp/pkgtest/libs.image     ston    STON,Neo-CSV
#
# Env:
#   PHARO=/tmp/h3/pharo                         stock Cog launcher
#   CUSTOM_VM=.../build-rel/test_load_image     custom JIT VM (use the -O2 build)
#   OUT=/tmp/pkgtest                            output dir
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
RUNNER="$HERE/run_pkg_tests.st"
PHARO="${PHARO:-/tmp/h3/pharo}"
CUSTOM_VM="${CUSTOM_VM:-$HERE/../../build-rel/test_load_image}"
OUT="${OUT:-/tmp/pkgtest}"
mkdir -p "$OUT"

IMAGE="${1:?image path}"
LABEL="${2:?label}"
PREFIXES="${3:?comma-separated package-name prefixes}"

# stage the external prefix file (the fairness contract: prefixes never appear
# as literals in the runner, so both VMs select the same classes)
printf '%s\n' "${PREFIXES//,/$'\n'}" > /tmp/pkg_prefixes.txt

# Optional PRELUDE .st fileIn'd before the runner on BOTH VMs (same on each, so
# it never biases the diff) -- used for visual packages to install the headless
# fake-GUI (scripts/pharo-headless-test/setup_fake_gui.st) so Morphic/Spec tests
# can run without a display.
PRELUDE="${PRELUDE:-}"
prelude_expr=""
[ -n "$PRELUDE" ] && prelude_expr="'$PRELUDE' asFileReference fileIn. "
EVAL="${prelude_expr}$(cat "$RUNNER")"

echo "== $LABEL: stock Cog =="
timeout 600 "$PHARO" "$IMAGE" eval "$EVAL" > "$OUT/${LABEL}_cog.log" 2>&1
grep -aE "^CLASSES|^RESULT" "$OUT/${LABEL}_cog.log"

echo "== $LABEL: custom JIT VM =="
PHARO_MAX_STEPS=2000000000000 timeout 900 "$CUSTOM_VM" "$IMAGE" eval "$EVAL" > "$OUT/${LABEL}_jit.log" 2>&1
# the custom VM interleaves [JIT]/[DIAG] telemetry; filter to the runner's lines
grep -aE "^CLASSES|^RESULT" "$OUT/${LABEL}_jit.log" | grep -avE "\[JIT\]"

echo "== $LABEL: parity diff (failures present on ONE VM only) =="
# normalize FAIL/ERR/TIMEOUT lines to "<KIND> Class>>sel" and compare the sets
norm() { grep -aE "^(FAIL|ERR|TIMEOUT) " "$1" | sed -E 's/ \[.*//; s/ \(.*//' | sort -u; }
norm "$OUT/${LABEL}_cog.log" > "$OUT/${LABEL}_cog.fails"
norm "$OUT/${LABEL}_jit.log" > "$OUT/${LABEL}_jit.fails"
echo "--- JIT-only failures (candidate JIT regressions) ---"
comm -13 "$OUT/${LABEL}_cog.fails" "$OUT/${LABEL}_jit.fails"
echo "--- Cog-only failures (baseline issues the JIT does NOT hit) ---"
comm -23 "$OUT/${LABEL}_cog.fails" "$OUT/${LABEL}_jit.fails"
echo "(shared baseline failures are parity, not regressions)"
