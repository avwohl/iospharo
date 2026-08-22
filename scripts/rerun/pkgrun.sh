#!/bin/bash
# Package tier, chained behind the SUnit sweeps.
#
# Runs only once the sweeps are done: both suites are timing-sensitive and
# the sweep's own header documents that competing load does not slow a
# wall-clock watchdog evenly -- it converts passes into ERRORs.  Running the
# package loads (network + compiler + large image writes) alongside a sweep
# would corrupt both sets of numbers.
MY=/private/tmp/claude-501/-Users-wohl-src-iospharo-jit/9e3b600b-3928-4a21-98c1-940a40fb8b09/scratchpad
R=/Users/wohl/src/iospharo-jit

echo "waiting for sweeps to finish..."
while pgrep -f 'sunit-sweep.sh|fullsweep.sh' > /dev/null 2>&1; do sleep 30; done
echo "sweeps done $(date '+%H:%M:%S'); starting package tier"

export LOAD_TIMEOUT=1800 TEST_TIMEOUT=1200 PER_CLASS_TIMEOUT=180
export PHARO_CODE_ZONE_MB=192 PHARO_MAX_STEPS=4000000000000

# arm64: load with our own VM (no stock Cog on this host) and test.
echo "=== arm64 packages $(date '+%H:%M:%S') ==="
WORK=$MY/pkg-arm $R/scripts/package-tests-selfhosted.sh \
    $R/build-rel/test_load_image $MY/work/base.image > $MY/pkg-arm.log 2>&1
echo "arm rc=$?"
cat $MY/pkg-arm/summary.txt 2>/dev/null

# x86_64: try a NATIVE load first.  Every previous x86 package run had to
# borrow arm-loaded images via REUSE_FROM, because Iceberg resolves github://
# through libgit2 over FFI and this host had only an arm64 libgit2.  An
# x86_64 libgit2 is now staged beside the binary, so whether the native load
# works is itself a result worth recording rather than assuming.
echo "=== x86_64 packages, native load $(date '+%H:%M:%S') ==="
WORK=$MY/pkg-x86 $R/scripts/package-tests-selfhosted.sh \
    $R/build-x86/test_load_image $MY/work/base.image > $MY/pkg-x86.log 2>&1
echo "x86 native rc=$?"
cat $MY/pkg-x86/summary.txt 2>/dev/null

echo "=== done $(date '+%H:%M:%S') ==="
