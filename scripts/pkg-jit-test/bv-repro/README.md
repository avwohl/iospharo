# Block-value (BV) inline + inline-J2J interaction bug

The real repro for the BV-on correctness failure (the "12 sort failures").
NOTE: the simple direct case (`bv_direct_OK.st`) does NOT reproduce — it works.
The bug needs the CROSS-METHOD case (`bv_cross_FAILS.st`): a method (`cmpat2:`)
whose home is inline-J2J'd into its caller (`cmpatTop`), containing a BV
`value:value:` with send-computed args.

Run (custom VM, neojson.image or any std image):

    VM=build-rel/test_load_image
    # FAILS with BV-on (DNU: Array did not understand #<=) after ~75k iters:
    PHARO_T1_INLINE_BLOCK_VALUE=1 PHARO_BV_MAX_CAP=1 PHARO_MAX_STEPS=2e12 \
      $VM <img> eval "$(cat bv_cross_FAILS.st)"
    # WORKS (direct, no cross-method inline-J2J):
    PHARO_T1_INLINE_BLOCK_VALUE=1 PHARO_BV_MAX_CAP=1 PHARO_MAX_STEPS=2e12 \
      $VM <img> eval "$(cat bv_direct_OK.st)"

Confirmed minimal conditions (each tested):
  - BV-off (default), inline-J2J on        -> WORKS  (CROSSFALSES[0])
  - BV-on, inline-J2J off (NO_INLINE_J2J)  -> WORKS  (CROSSFALSES[0])
  - BV-on, inline-J2J on (real config)     -> FAILS  (DNU #<= rcvr=Array)
So the bug is the INTERACTION; neither feature alone fails.

The corrupted BV calls are at j2jDepth==0 (inline-J2J does NOT bump j2jDepth),
so a `j2jDepth>0` bail is the WRONG fix (it bails unrelated system blocks into a
bail path that is itself broken at depth>0, and misses the actual cross calls).
See docs/block-value-inline-debug.md UPDATE 23.
