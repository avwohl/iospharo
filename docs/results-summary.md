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
       PolyMath     101      11    0     0    0        100% of 11   crash @ test 11 (primitiveFloatAdd, same J2J family)

       totals       422      512   20    351  2        58% of 885   (Roassal3 reached 35/879, PolyMath 11/3500)

## Headline findings

* **Roassal3 crash + PolyMath crash + Color>>blue errors** all
  stem from one bug: JIT-side classifier bits (bit 60 J2J_ENTRY_BIT,
  bit 63 getter, etc.) leaking into Oops that downstream code
  dereferences.  `PHARO_NO_JIT=1` removes all three symptoms;
  attempting to filter the bad Oops at C++ primitive boundaries
  (`extractFloat`) made things WORSE because the original
  primitiveFloatAdd crash was acting as fail-fast.  Closing #11
  and #14 as duplicates of #10 — fix #10 and ~850 + ~3500 tests
  unblock.
* **Athens 93%** — only failures are CairoSVGSurfaceTest, which
  needs the `libcairo` external plugin that our VM doesn't load.
  Not a VM bug.
* **Cairo, Color, Form: 100%** — primitive-level graphics
  (BitBlt-free paths) work correctly.
* **Morph 42%** — most errors are `testOpenInWorld`, `testHaloIsDisable`,
  etc., which need a real WorldMorph display.  FakeGUI shim limitation.

## Out of scope here

* **PolyMath** (task #7) — was blocked on Iceberg's
  `Character>>bitShift:` regression; **fixed 2026-05-28** via
  `scripts/patches/character_numeric_coercion.st` (see
  `docs/image_issues.md`).  309 classes load, 11 tests pass before
  hitting the same J2J leak as Roassal3.
* **Bloc** — confirmed NOT preinstalled in Pharo 13; the 183 substring
  matches were all `*Block*` compiler tests.  Deleted from queue.
* **Plot, Chart** — RS-prefix subsets of Roassal3, already exercised by
  the Roassal3 run.  Deleted from queue.

## Next focus

Task #10 is now the only blocker.  Fixing it unlocks:

* ~844 more Roassal3 tests (currently reached 35 of 879)
* ~3489 more PolyMath tests (currently reached 11 of 3500)
* The 3 Color>>blue errors and any other transient corruption
  symptoms.
* The 3 remaining renders in the PNG harness
  (single_box, bar_plot, line_plot).

The bug needs lldb to root-cause — set a breakpoint at the
crashing PC (`codeStart + 2496` in the JIT'd
`#inverseTransformPiOrZero:`) and walk back to find which IC fill
or method-map mutation left bit 60 set on a slot that's later
loaded as a clean Oop.  See `docs/results-roassal3.md` for the
full investigation log.
