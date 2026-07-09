#!/usr/bin/env bash
# Clean background re-run of PolyMath on our JIT VM, with a hard per-process
# timeout and progress logging so we can see exactly where (if anywhere) it stops.
set -u
cd /c/temp/src/iospharo-jit
cp /c/tmp/pm_classes.txt /c/tmp/sunit_class_names.txt
rm -f /c/tmp/sunit_run_completed.txt /c/tmp/sunit_test_results.txt /c/tmp/sunit_test_detail.txt
echo "PMOURS2-START $(date)"
# Hard 900s cap; PHARO_SUNIT-style per-test timeout is inside the runner.
timeout --kill-after=20 900 build-win/test_load_image.exe \
  "C:/tmp/pkgtest/polymath-sunit.image" > /c/tmp/pm_ours2.log 2>&1
echo "PMOURS2-EXIT=$? $(date)"
echo "classes=$(grep -c '^=== ' /c/tmp/sunit_test_results.txt 2>/dev/null) tests=$(wc -l < /c/tmp/sunit_test_detail.txt 2>/dev/null)"
echo "last=$(tail -1 /c/tmp/sunit_test_detail.txt 2>/dev/null | cut -f2)"
echo "completed_marker=$([ -f /c/tmp/sunit_run_completed.txt ] && echo yes || echo no)"
