# arm64 sweep, 2026-09-02 — what is in here

The write-up is one level up: `docs/results/sweep-arm-2026-09-02.md`.

    sweep.log                     the driver's per-batch log (rc, seconds, class count)
    per-class-totals.txt          every class's `Total: N P:.. F:.. E:.. S:..` line
    nonzero-classes.txt           just the classes with a non-zero F, E or T
    failing-selectors.txt         every FAIL/ERROR/TIMEOUT line, with its class
    class-index-map.txt           index -> class for this image; resolves any batch
                                  number in any sweep log.  1911 TonelWriterV3Test,
                                  1912 TraitChangesTest, 1951 UndefinedPackageTest.
    storm-heap-census.txt         the heap census at the batch-1901 abort (defect #23)

Everything below is stock Cog v10.3.9 (x86_64 under Rosetta) on the SAME
pristine image, which is what made most of the residual explicable.  See
CLAUDE.md for how to get that VM; the arm64 one aborts on this host.

    cog-residual-baseline.txt     every class in our residual, run on Cog, with timings
    cog-trait-baseline.txt        the 27 Trait*Test classes (defect #23) and the rest
                                  of the range the storm ate
    cog-defect22-evidence.txt     RSLinesTest on Cog, plus the image-wide count of
                                  methods whose block has a foreign outerCode
    cog-defect24-repro.txt        the reproduction: Morphic installed with its UI
                                  process suspended gives our exact failure set,
                                  plus the two controls that do not
    cog-parameterized-check.txt   ParametrizedTestCase census, the instance dumps,
                                  and Cog's headless GUI state
    cog-tf-callback-baseline.txt  the threaded-FFI block Cog runs in 7.4 s and we
                                  spend 1800 s on (defect #26)

A caveat that applies to every Cog file here: test COUNTS are not comparable to
ours, because `suite run` expands parameterised cases our runner does not.  The
F/E columns and the timings are.
