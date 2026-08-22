#!/bin/bash
# Sequential full sweeps: one architecture at a time, so each runs on an
# otherwise idle machine (competing load turns passes into ERRORs).
MY=/private/tmp/claude-501/-Users-wohl-src-iospharo-jit/9e3b600b-3928-4a21-98c1-940a40fb8b09/scratchpad
R=/Users/wohl/src/iospharo-jit
cd $MY/work || exit 1
export PHARO_CODE_ZONE_MB=192 PHARO_MAX_STEPS=4000000000000
for arch in rel x86; do
  rm -rf $MY/sweep-$arch
  STEP=50 PER_BATCH_TIMEOUT=900 $R/scripts/sunit-sweep.sh \
      $R/build-$arch/test_load_image $MY/work/sunit.image $MY/sweep-$arch \
      > $MY/sweep-$arch.log 2>&1
  echo "=== $arch rc=$? $(date '+%H:%M:%S') ==="
  sed -n '/=== TOTALS ===/,$p' $MY/sweep-$arch.log
done
