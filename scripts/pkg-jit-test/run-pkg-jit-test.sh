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
#   PRESERVE_S3=s3://bucket/prefix              upload each package's logs and
#                                               .fails as soon as they exist, so
#                                               a box that dies mid-sweep still
#                                               leaves everything it finished
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

# Both arms now run from a per-package working directory (see below), so every
# path handed to them must be absolute or it would resolve against that
# directory instead of the caller's.
abspath() { case "$1" in /*) printf '%s' "$1";; *) printf '%s' "$PWD/$1";; esac; }
IMAGE="$(abspath "$IMAGE")"
OUT="$(abspath "$OUT")"
PHARO="$(abspath "$PHARO")"
CUSTOM_VM="$(abspath "$CUSTOM_VM")"

# Pass the prefixes to the runner via the env (per-process, so parallel workers
# don't race) AND the legacy /tmp file.  Either way they stay OUT of the runner's
# literal pool (the fairness contract), so both VMs select the same classes.
export PKG_PREFIXES="$PREFIXES"
printf '%s\n' "${PREFIXES//,/$'\n'}" > /tmp/pkg_prefixes.txt

# Optional PRELUDE .st fileIn'd before the runner on BOTH VMs (same on each, so
# it never biases the diff) -- used for visual packages to install the headless
# fake-GUI (scripts/pharo-headless-test/setup_fake_gui.st) so Morphic/Spec tests
# can run without a display.
PRELUDE="${PRELUDE:-}"
prelude_expr=""
[ -n "$PRELUDE" ] && PRELUDE="$(abspath "$PRELUDE")"
[ -n "$PRELUDE" ] && prelude_expr="'$PRELUDE' asFileReference fileIn. "
EVAL="${prelude_expr}$(cat "$RUNNER")"

# Per-package working directory, used by BOTH arms.
#
# Pharo's StartupPreferencesLoader reads `startup.st` from the CURRENT
# DIRECTORY, and our VM's eval mode writes one there.  Running N workers from a
# shared directory therefore has them delete and overwrite each other's script:
# in the 2026-08-11 arm sweep, 152 of 223 custom-VM runs evaluated nothing at
# all and the summary recorded `jit_RESULT = -`.  A directory per package fixes
# that, and giving the SAME directory to both arms keeps the A/B fair — CWD is
# image-visible (CWD-relative file lookups), so the two VMs must not see
# different ones.
WD="$OUT/wd-$LABEL"
rm -rf "$WD"; mkdir -p "$WD"

echo "== $LABEL: stock Cog =="
(cd "$WD" && timeout 600 "$PHARO" "$IMAGE" eval "$EVAL") > "$OUT/${LABEL}_cog.log" 2>&1
grep -aE "^CLASSES|^RESULT" "$OUT/${LABEL}_cog.log"

echo "== $LABEL: custom JIT VM =="
(cd "$WD" && PHARO_MAX_STEPS=2000000000000 timeout 900 "$CUSTOM_VM" "$IMAGE" eval "$EVAL") > "$OUT/${LABEL}_jit.log" 2>&1
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

# Ship this package's results off-box AS SOON AS THEY EXIST.
#
# A sweep is hours of work sitting on an instance store.  On 2026-08-11 a
# CloudWatch idle alarm terminated the box at 158/200 and every result went
# with it — `preserve.sh` syncs notes and logs, not these.  Uploading per
# package means a box that dies mid-sweep still leaves everything it finished.
# Best-effort: a failed upload must never fail the package.
if [ -n "${PRESERVE_S3:-}" ]; then
    for f in "$OUT/${LABEL}_cog.log" "$OUT/${LABEL}_jit.log" \
             "$OUT/${LABEL}_cog.fails" "$OUT/${LABEL}_jit.fails"; do
        [ -f "$f" ] || continue
        aws s3 cp --only-show-errors "$f" \
            "${PRESERVE_S3%/}/$(basename "$f")" 2>/dev/null \
            || echo "  (preserve: upload failed for $(basename "$f"))"
    done
fi

# Clean the per-package working directory; the loaded image and its
# pharo-local cache are large and 200 of them fill the disk.
rm -rf "$WD"
