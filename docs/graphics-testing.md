# Graphics package testing

A queue of large Smalltalk graphics packages used to stress the VM
beyond the kernel SUnit classes.  Each package is loaded into a fresh
Pharo image via stock-Cog Metacello, snapshotted, then re-run on our
VM with the existing `run_sunit_tests.st` harness — so the same fork /
timeout / classifier infrastructure used for kernel tests applies.

## Why these packages

The 4-class / 20-class curated SUnit runs exercise collections,
strings, contexts, and blocks but barely touch the graphics stack
(BitBlt, Form, Athens canvas, font rendering, OSWindow).  Real Pharo
apps spend most of their cycles there, so getting these suites green
is a much better proxy for "VM is production-ready" than 99.6% on
StringTest.

## Queue (ordered by ratio of bug-isolation to setup cost)

1. **Roassal3** — visualization engine.  ~3-4 K tests.  Loads Athens
   + Bloc + layout algorithms transitively, so it's the biggest single
   surface.  Headless-friendly: tests render `RSCanvas` to `Form`,
   we can PNG-dump for visual diff.

       Metacello new
         baseline: 'Roassal3';
         repository: 'github://pharo-graphics/Roassal:v1.x/src';
         load.

2. **Athens** — vector-graphics layer (canvas, paint, path, transform).
   ~300 tests, very focused.  Best for isolating canvas/transform/path
   bugs from the viz layer above.  Loaded transitively by Roassal3, so
   if Roassal3 surfaces Athens failures, run this in isolation.

       Metacello new
         baseline: 'Athens';
         repository: 'github://pharo-graphics/Athens/src';
         load.

3. **Bloc** — newer graphics framework (replacement for Morphic).
   ~1-2 K tests.  Lower-level than Roassal3 — surfaces VM bugs in
   `Form bitsPerPixel:`, BitBlt blits, glyph layout that the
   higher-level viz suite may mask.

       Metacello new
         baseline: 'Bloc';
         repository: 'github://pharo-graphics/Bloc:dev/src';
         load.

4. **PolyMath** — numerical computing + Roassal3 plotting.  ~1 K tests.
   Stresses FP arithmetic alongside rendering — useful angle on the
   `Float>>cos` JIT skip (commit `5c870c75`) and any other FP
   regressions.

       Metacello new
         baseline: 'PolyMath';
         repository: 'github://PolyMathOrg/PolyMath:dev/src';
         load.

## Workflow per package

Same pattern as kernel SUnit:

    # 1. Fresh image
    cd /tmp/harness && cp Pharo.image Pharo-roassal.image \
                       && cp Pharo.changes Pharo-roassal.changes

    # 2. Install via stock Cog (Metacello needs network + stable VM)
    timeout 600 /tmp/harness/pharo /tmp/harness/Pharo-roassal.image eval --save \
      "Metacello new baseline: 'Roassal3'; \
        repository: 'github://pharo-graphics/Roassal:v1.x/src'; load"

    # 3. Install runner + GUI shims
    /tmp/harness/pharo /tmp/harness/Pharo-roassal.image eval --save \
      "'$PWD/scripts/pharo-headless-test/setup_fake_gui.st' asFileReference fileIn.
       '$PWD/scripts/pharo-headless-test/run_sunit_tests.st' asFileReference fileIn"

    # 4. List the package's test classes for the runner's filter
    /tmp/harness/pharo /tmp/harness/Pharo-roassal.image eval --save \
      "FileLocator imageDirectory / 'sunit_class_names.txt' writeStreamDo: [:s |
         ((RPackage organizer packages select: [:p | p name beginsWith: 'Roassal']) \
            flatCollect: [:p | p definedClasses select: [:c | c inheritsFrom: TestCase]]) \
              asSortedCollection do: [:c | s nextPutAll: c name; lf]]"
    cp /tmp/harness/sunit_class_names.txt /tmp/

    # 5. Run on our VM
    rm -f /tmp/sunit_test_results.txt /tmp/sunit_test_detail.txt
    ./build/test_load_image /tmp/harness/Pharo-roassal.image

    # 6. Baseline on stock Cog (separate image — graphics packages tend
    #    to have version drift between VMs)
    timeout 1800 /tmp/harness/pharo /tmp/harness/Pharo-roassal-cog.image eval \
      "'$PWD/scripts/pharo-headless-test/run_sunit_cog.st' asFileReference fileIn"

    # 7. Diff
    scripts/classify-sunit.py cog.txt ours.txt > docs/results-roassal3.md

## Headless graphics gotchas

* `OSWindow` / `OSSDL2Driver` — Roassal3 tests open windows by default.
  Our VM has SDL2 stubs (commit see CLAUDE.md "GUI verified working
  2026-02-24"), but stock Cog still wants a real display.  Run stock
  with `-headless` flag where supported, or patch
  `Smalltalk session>>isInteractiveGraphic` to `^ false` during
  Metacello load.
* Tests that depend on `World` or `currentHand` — `setup_fake_gui.st`
  already installs the shims needed for Spec tests; reuse it here.
* PNG capture: `RSCanvas asForm` → `PNGReadWriter putForm: f onStream: s`
  is the canonical path.  Save under
  `/tmp/sunit_graphics/<TestClass>_<selector>.png`.
* Athens-Cairo backend is C plugin — our VM doesn't load Cairo, so
  Roassal3 will fall back to the Athens-Morphic backend.  That's fine
  for testing, but pixel-diff against stock Cog (which uses Cairo) will
  show benign anti-aliasing differences — compare by structural
  similarity (SSIM) not exact bytes.

## Status

Queue tracked in TaskList — tasks #4 (Roassal3), #5 (Athens), #6 (Bloc),
#7 (PolyMath), #8 (render harness).  Roassal3 is the first
in-progress.

Results land in `docs/results-<package>.md` (one per package, same
shape as `docs/test-results.md` for kernel SUnit).
