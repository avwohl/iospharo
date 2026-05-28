# Graphics SUnit summary

All seven packages run on `/tmp/harness/Pharo-gfx.image` (clean Pharo 13 +
SUnitRunner + FakeGUI shims).  Per-package raw output under
`docs/results/<tag>{,_detail}.txt`.

       package      classes  pass  fail  err  timeout  pass-of-run  notes
       Roassal3     99       51    0     121  0        run 2 reached 172 (was 35 + crash) after b8eead95
       Spec2        204      363   19    318  2        51% of 702   wall-capped at 716 of 3505
       Athens       8        52    0     4    0        93% of 56    all 4 ERRs in SVG-export (Cairo plugin absent)
       Cairo        2        4     0     0    0        100%         clean
       Color/Form   3        31    0     0    0        100%         clean
       Morph        5        19    1     26   0        42% of 46    most ERRs from FakeGUI (testOpenInWorld etc.)
       PolyMath     101      11    0     0    0        100% of 11   crash @ test 11 (same J2J family — fix b8eead95 should apply)

       totals       422      531   20    469  2        72% of 1022  (Roassal3 reached 172/879, PolyMath 11/3500)

## Headline findings

* **Roassal3 crash + PolyMath crash + Color>>blue errors** all
  stem from one bug: JIT-side classifier bits (bit 60 J2J_ENTRY_BIT,
  bit 63 getter, etc.) leaking into Oops that downstream code
  dereferences.  Captured under lldb (commit `b8eead95`) — same fault
  addr `0x86fe800000000008` in arm64 `ldr w4, [x1]` at the IC probe.
  **Two-site defensive guard landed**: JIT emit_send checks bits
  48-63 of receiver and routes to MISS, and `sendDoesNotUnderstand`
  catches non-canonical receivers and pushes nil instead of
  crashing.  Roassal3 now reaches 172/879 tests (was 35).  Root
  cause for the upstream leak still TBD — these guards keep the run
  alive but don't stop the bit-60 escape itself.
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
