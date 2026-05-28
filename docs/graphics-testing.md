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

1. **Roassal3** — visualization engine.  Pharo 13 ships it
   preinstalled: 99 test classes, 879 tests (RS-prefixed).  No
   Metacello required.  Headless-friendly: tests render `RSCanvas` to
   `Form`, we can PNG-dump for visual diff.

       "Already in Pharo.image — list:"
       (Smalltalk globals values select: [:c |
         c isClass and: [(c inheritsFrom: TestCase) and: [c name beginsWith: 'RS']]])

2. **Spec2** — Pharo's UI presenter framework.  Preinstalled: 204
   test classes, 3505 tests (Sp-prefixed).  Largest preinstalled UI
   surface.  Reuses `setup_fake_gui.st` shims for `openWithSpec`.

3. **Athens** — vector-graphics layer (canvas, paint, path, transform).
   ~300 tests, very focused.  Best for isolating canvas/transform/path
   bugs from the viz layer above.  Loaded transitively by Roassal3, so
   if Roassal3 surfaces Athens failures, run this in isolation.

       Metacello new
         baseline: 'Athens';
         repository: 'github://pharo-graphics/Athens/src';
         load.

4. **Bloc** — newer graphics framework (replacement for Morphic).
   ~1-2 K tests.  Lower-level than Roassal3 — surfaces VM bugs in
   `Form bitsPerPixel:`, BitBlt blits, glyph layout that the
   higher-level viz suite may mask.

       Metacello new
         baseline: 'Bloc';
         repository: 'github://pharo-graphics/Bloc:dev/src';
         load.

5. **PolyMath** — numerical computing + Roassal3 plotting.  ~1 K tests.
   Stresses FP arithmetic alongside rendering — useful angle on the
   `Float>>cos` JIT skip (commit `5c870c75`) and any other FP
   regressions.

       Metacello new
         baseline: 'PolyMath';
         repository: 'github://PolyMathOrg/PolyMath:dev/src';
         load.

## Workflow per package

For **preinstalled** packages (Roassal3, Spec2), reuse the existing
`Pharo-ws.image` (already has `SUnitRunner` + `setup_fake_gui.st`
applied) and just swap the class-name filter file.  Building a
separate Pharo-<pkg>.image from a clean Pharo.image with WorldMorph
init enabled triggers a BitBlt SIGSEGV in `bitBltField + 304` during
graphics-startup before SUnitRunner can fire (see "Known issues"
below).

    # Preinstalled-package workflow:
    cp /tmp/<pkg>_test_classes.txt /tmp/sunit_class_names.txt
    rm -f /tmp/sunit_test_results.txt /tmp/sunit_test_detail.txt
    ./build/test_load_image /tmp/harness/Pharo-ws.image

For **Metacello-loaded** packages (Athens, Bloc, PolyMath):

    # 1. Fresh image
    cd /tmp/harness && cp Pharo.image Pharo-<pkg>.image \
                       && cp Pharo.changes Pharo-<pkg>.changes

    # 2. Install via stock Cog (Metacello needs network + stable VM)
    timeout 600 /tmp/harness/pharo /tmp/harness/Pharo-<pkg>.image eval --save \
      "Metacello new baseline: 'Athens'; \
        repository: 'github://pharo-graphics/Athens/src'; load"

    # 3. Install runner + GUI shims
    /tmp/harness/pharo /tmp/harness/Pharo-<pkg>.image eval --save \
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

## Known issues

* **`bitBltField + 304` SIGSEGV on fresh-image WorldMorph startup** —
  a Roassal-only image with `WorldMorph class>>startUp:` re-enabled
  crashes in our `bitBltField` C helper with `x19 = 0x8867...`
  (clearly a garbage pointer).  Workaround: run on the
  `Pharo-ws.image` snapshot that already has the kernel runner
  installed but never opened a world.  Root cause TBD — likely a
  `BitBlt class>>copyBits` overload our VM mis-dispatches when the
  destination form is the live display surface.
* **Metacello `Character>>bitShift:` regression** — installing any
  package from a GitHub repo throws `Instance of Character did not
  understand #bitShift:` inside `SHA1>>processBuffer:`.  Iceberg
  computes the SHA1 of a commit blob and treats each char as
  a `bitShift:` recipient.  Probably an Iceberg/SHA1 expectation
  that strings stream as `SmallInteger` codepoints, not `Character`.
  Blocks Athens/Bloc/PolyMath install on the standard Pharo 13 image.

## Status

Queue tracked in TaskList:

       task  package    classes  tests   preinstalled  status
       #4    Roassal3   99       879     yes           done — 32 PASS / 3 ERROR / 0 FAIL, then SIGSEGV at test 35
       #9    Spec2      204      3505    yes           done — 363 PASS / 19 FAIL / 318 ERROR / 2 TIMEOUT at 716/3505 (wall-cap)
       #6    Bloc       53       642     yes           in progress (queue)
       #5    Athens     10       80      yes           queued
       #6b   Cairo      7        32      yes           queued (bundled with Bloc)
       #12   Plot       10       146     yes           queued (bundled)
       #12b  Chart      4        17      yes           queued (bundled)
       #7    PolyMath   —        —       no            blocked on Iceberg SHA1 bug
       #8    Render     —        —       —             pending (cross-cutting)

All preinstalled-package class lists are pre-extracted to
`/tmp/<pkg>_test_classes.txt`.

Results land in `docs/results-<package>.md` (one per package, same
shape as `docs/test-results.md` for kernel SUnit).
