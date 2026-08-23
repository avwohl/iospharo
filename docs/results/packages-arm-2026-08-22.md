# Package tier, arm64, 2026-08-22 — before and after the eviction fixes

Same host, same script (`scripts/package-tests-selfhosted.sh`), same base
image, three runs a few minutes apart. The only difference between the two
columns is the three eviction/finalization fixes (`1d7a91af`, `79cc0fa0`,
`dd85a061`).

The 2026-08-17 column is from `docs/local-suite-run-2026-08-17.md` — the last
run made BEFORE code-zone eviction became reachable at all, so it is the
"JIT froze when the zone filled" baseline.

## Load times

    package     2026-08-17   today, before fixes   today, after fixes
    NeoJSON          20 s           29 s                  21 s
    Mustache          9 s           11 s                  11 s
    XMLParser      1055 s      TIMEOUT (1800 s)          369 s
    Grease           50 s          365 s                   48 s
    PolyMath   TIMEOUT (1200 s)   (not reached)          269 s
    DataFrame       139 s        (not reached)             91 s
    Fuel              4 s        (not reached)          FAILS — repo is gone

Two packages that had NEVER produced a result on this host now do:
`XMLParser` (which timed out at 1800 s in the morning run and produced "no
RESULT" on 2026-08-17) and `PolyMath` (which timed out at 1200 s on
2026-08-17).

## Test results, after

    package     classes   pass   fail   err
    NeoJSON          11     116      0     0
    Mustache          1      47      0     0
    XMLParser       159    6358      0     1
    Grease           37     554      0     0
    PolyMath        117    1448      2    18
    DataFrame        27     839     14     0
    Fuel              2      19      0     0
                    ---    ----    ---   ---
                    354    9381     16    19

The 2026-08-19 reference in `docs/test-results.md` for the same seven
packages is 9383 pass / 16 fail / 17 err, measured on a different host where
all seven loaded. Today's run is within two tests of it on every column —
and every load ran natively here, which they could not this morning.

## Fuel does not load, and never did

`github.com/pharo-project/pharo-fuel` is **gone**: 404 on the web page, 401
"Repository not found" on `.git/info/refs`. Iceberg tries SSH first (auth
error — every package here logs that, it is normal), falls back to HTTPS, and
then sits inside libgit2's `http_stream_read` against a repository that does
not exist. Killed at 1042 s with 20 KB in `pharo-local`; a working clone
(NeoJSON) has 1.5 MB inside 21 s, and nothing was logged for the last 15
minutes of it.

The 19 passes are from the Fuel already IN the base image, so this entry has
never actually loaded anything from that URL — including in the 2026-08-17
run, which recorded "load 4 s" and the same 2 classes / 19 passes.

`scripts/package-tests-selfhosted.sh` now points at `theseion/Fuel`, whose
existence, `master` default branch and `srcDirectory: 'repository'` are
verified over the GitHub API. **The load itself is untested.**

Worth a separate look: the VM sat in a blocking libgit2 read for 17 minutes
against a 401 instead of surfacing an error. Whether stock Cog fails faster
there is unknown.

`Grease`'s 37 classes / 554 passes are new since the script's test-class
pattern was corrected from `Grease` (matches nothing) to `GR`.

The residual matches the classification already recorded in
`docs/test-results.md`: DataFrame's 14 are the `Float
DefaultComparisonPrecision` tightening (1e-4 -> 1.49e-8), PolyMath's errors
are concentrated in `XMLWriterTest` (8), `PMGeneralFunctionFit` /
`PMKDTree` (2 each) and an intentional `SMarkTest` demo fixture, and
`XMLParser`'s single error is in `XMLParserTest`. None of them is new today.

## What the fixes were

Full detail in `docs/WIP.md`; in one line each:

  * `1d7a91af` — the eviction pin scan walked the whole heap on every round
    (826,734 object visits per round to find 32 Process objects). Cached per
    GC, eden re-walked each round.
  * `79cc0fa0` — the IC scrub tested every J2J cache entry of every method
    against every evicted range, linearly. Sorted + coalesced + binary
    searched.
  * `dd85a061` — once the mourn queue held nothing but keepers, every method
    activation popped and re-pushed the whole queue forever. Guarded on the
    queue size not having changed.

A `sample` of the stuck 30-minute XMLParser load put 94% of the process in
the first of those, and a later one put 38% in the third.

# x86_64 package tier, same day, native Metacello load

The first time x86_64 has loaded these packages itself. Every previous x86
package number came from arm-loaded images borrowed via `REUSE_FROM`,
because Iceberg resolves `github://` through libgit2 over FFI and this host
had only an arm64 libgit2. `scripts/fetch-x86-libs.sh` staged an x86_64 one,
and the native load works.

    package     classes   pass   fail   err        arm64 pass
    NeoJSON          11    116      0     0             116
    Mustache          1     47      0     0              47
    XMLParser         0      0      0     0            6358   <-- see below
    Grease           37    554      0     0             554
    PolyMath        117   1389      2    19            1448   (+1 timeout)
    DataFrame        27    839     14     0             839
    Fuel              2     19      0     0              19
                    ---   ----    ---   ---
                    195   2964     16    19            9381

Five of seven match arm64 exactly, including DataFrame's 14 known
`Float DefaultComparisonPrecision` failures. The gap is two packages:

  * **XMLParser loads on arm64 and not on x86_64** — `rc=0` in 31 s with the
    image never persisting, against 369 s and 159 classes on arm. Filed in
    `docs/vm-compat-bugs.md`; the single error in its log is "only integers
    should be used as indices", raised while the image writes a debug stack
    to stderr, so whether that is the original failure or a second one on the
    reporting path is not yet established.
  * **PolyMath's 59-test gap is a timeout bound, not a divergence.** Diffing
    the two runs class by class, 117 entries each, exactly two differ:

        PMArbitraryPrecisionFloatTest   arm 58 ran/57 passed/1 err
                                        x86 TIMEOUT after 180 s      -57 passes
        PMKDTreeTest                    arm 11 passed/2 err
                                        x86  9 passed/4 err          -2 passes

    57 + 2 = 59, the whole gap. Re-run alone on x86_64 with a 900 s bound,
    `PMArbitraryPrecisionFloatTest` takes **186 s** — six seconds over the
    bound — and returns `58 ran, 57 passed, 1 error`, byte-identical to
    arm64. So the only real arm/x86 difference in PolyMath is
    `PMKDTreeTest`'s 2 tests, and x86_64's package total is effectively
    1446 against arm's 1448.

    `PER_CLASS_TIMEOUT` is sized for arm64; x86_64 under Rosetta needs ~2x.
    The script now says so at the knob.

Load times, arm64 -> x86_64: NeoJSON 21->29 s, Mustache 11->15 s, Grease
48->89 s, PolyMath 269->577 s, DataFrame 91->176 s. That is the usual ~2x
Rosetta factor and nothing anomalous.
