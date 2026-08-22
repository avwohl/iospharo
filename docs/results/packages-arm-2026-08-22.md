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
    Fuel              4 s        (not reached)             (see below)

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
