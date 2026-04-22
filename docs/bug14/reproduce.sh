#!/bin/bash
# Bug 14 minimal reproducer (2026-04-22).
# Takes ~60s.  Fresh image, no harness, single test method,
# with both tripwires and JIT tracing enabled.  Dumps comparison
# against NO_JIT run.
#
# Output:
#   /tmp/bug14-jit.log    — JIT on, will hang (SmallInteger DNUs fire)
#   /tmp/bug14-nojit.log  — NO_JIT, completes cleanly
# Diff the two to see where JIT diverges.

set -eu
WORKDIR=/tmp/bug14-repro
IOSPHARO=/Users/wohl/src/iospharo

# Fresh Pharo 13.1 image
rm -rf "$WORKDIR"
mkdir -p "$WORKDIR"
(cd "$WORKDIR" && curl -sL https://get.pharo.org/64/130+vm | bash) >/dev/null

# Install SUnitRunner so the test resolves
"$WORKDIR/pharo" "$WORKDIR/Pharo.image" eval --save \
    "'$IOSPHARO/scripts/pharo-headless-test/run_sunit_tests.st' asFileReference fileIn" \
    >/dev/null 2>&1

# Stage class list (not strictly needed for this repro but keeps harness happy)
cp "$IOSPHARO/scripts/pharo-headless-test/test_classes.txt" /tmp/sunit_test_classes.txt

echo "=== JIT run (expected to hang at testNthRootTruncated) ==="
rm -f /tmp/sunit_test_results.txt /tmp/sunit_run_completed.txt
unset PHARO_NO_JIT
PHARO_JIT_DEFER=0 \
PHARO_SLOT_TRIPWIRE=1 \
timeout 60 "$IOSPHARO/build/test_load_image" "$WORKDIR/Pharo.image" eval \
    "(IntegerTest selector: #testNthRootTruncated) runCase. 'done'" \
    > /tmp/bug14-jit.log 2>&1 || true
echo "  exit: $?"
echo "  SLOT-TRIPWIRE count: $(grep -c '^\[SLOT-TRIPWIRE\]' /tmp/bug14-jit.log || echo 0)"
echo "  TERM-Pxx events:    $(grep -c '^\[TERM-P' /tmp/bug14-jit.log || echo 0)"
echo "  atEnd-DNU events:   $(grep -c 'atEnd not understood' /tmp/bug14-jit.log || echo 0)"

echo ""
echo "=== NO_JIT run (expected to complete in <30s) ==="
rm -f /tmp/sunit_test_results.txt /tmp/sunit_run_completed.txt
PHARO_NO_JIT=1 \
timeout 60 "$IOSPHARO/build/test_load_image" "$WORKDIR/Pharo.image" eval \
    "(IntegerTest selector: #testNthRootTruncated) runCase. 'done'" \
    > /tmp/bug14-nojit.log 2>&1 || true
echo "  exit: $?"
echo "  TERM-Pxx events:    $(grep -c '^\[TERM-P' /tmp/bug14-nojit.log || echo 0)"
echo "  atEnd-DNU events:   $(grep -c 'atEnd not understood' /tmp/bug14-nojit.log || echo 0)"

echo ""
echo "See /tmp/bug14-jit.log and /tmp/bug14-nojit.log for full output."
echo "Key greps:"
echo "  grep -A8 'atEnd not understood' /tmp/bug14-jit.log   — the 3 DNU events"
echo "  grep 'lastJitReturn' /tmp/bug14-jit.log              — preceding JIT returns"
echo "  grep 'TERM-P' /tmp/bug14-jit.log                     — scheduler-proc cascade"
