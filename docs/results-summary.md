# Graphics SUnit summary

All seven packages run on `/tmp/harness/Pharo-gfx.image` (clean Pharo 13 +
SUnitRunner + FakeGUI shims).  Per-package raw output under
`docs/results/<tag>{,_detail}.txt`.

       package      classes  pass  fail  err  timeout  pass-of-run  notes
       Roassal3     99       32    0     3    0        91% of 35    crash @ test 35 (JIT #inverseTransformPiOrZero:)
       Spec2        204      363   19    318  2        51% of 702   wall-capped at 716 of 3505
       Athens       8        52    0     4    0        93% of 56    all 4 ERRs in SVG-export (Cairo plugin absent)
       Cairo        2        4     0     0    0        100%         clean
       Color/Form   3        31    0     0    0        100%         clean
       Morph        5        19    1     26   0        42% of 46    most ERRs from FakeGUI (testOpenInWorld etc.)

       totals       321      501   20    351  2        58% of 874   (Roassal3 covers only 35 of its 879; partial)

## Headline findings

* **Roassal3 crash** — JIT-emitted `#inverseTransformPiOrZero:` faults
  with `J2J_ENTRY_BIT` (bit 60) leaking into a receiver Oop.  Tracks as
  task #10.  This is the single biggest blocker — fixing it unlocks
  844 more Roassal3 tests.
* **`Color>>blue` KeyNotFound** (3 Roassal3 errors) — transient
  IdentityDictionary corruption late in the run; not reproducible in
  isolation.  Tracks as task #11.
* **Athens 93%** — only failures are CairoSVGSurfaceTest, which
  needs the `libcairo` external plugin that our VM doesn't load.
  Not a VM bug.
* **Cairo, Color, Form: 100%** — primitive-level graphics
  (BitBlt-free paths) work correctly.
* **Morph 42%** — most errors are `testOpenInWorld`, `testHaloIsDisable`,
  etc., which need a real WorldMorph display.  FakeGUI shim limitation.

## Out of scope here

* **PolyMath** (task #7) — not preinstalled; Metacello load fails on
  Iceberg's `Character>>bitShift:` regression during SHA1 of the
  commit blob.
* **Bloc** — confirmed NOT preinstalled in Pharo 13; the 183 substring
  matches were all `*Block*` compiler tests.  Deleted from queue.
* **Plot, Chart** — RS-prefix subsets of Roassal3, already exercised by
  the Roassal3 run.  Deleted from queue.

## Next focus

Picking based on bang-for-buck:

1. **Task #10 — J2J entry-bit leak in `#inverseTransformPiOrZero:`** —
   unblocks 844 Roassal3 tests.  Reproducer is one method, asmjit dump
   is one env-var away.
2. **Task #11 — `Color>>blue` KeyNotFound** — likely a GC/identity
   regression triggered under Roassal3's allocation pressure.  Catch
   it with an lldb watchpoint on `ColorRegistry`'s Oop.
3. **Task #8 — PNG render harness** — once #10/#11 are fixed, a fully
   green Roassal3 run is the right time to add visual diff against a
   Cog-rendered baseline.
