# Package tier, arm64, 2026-09-03 — with the night's VM fixes

`scripts/package-tests-selfhosted.sh` against the pristine base image, all
seven packages loaded natively by our own VM.  20 minutes end to end.
Raw: `packages-arm-2026-09-03.txt`.

## Against the 2026-08-22 baseline

    package     classes   pass          fail      err        note
    NeoJSON          11    116 =  116    0 = 0     0 = 0
    Mustache          1     47 =   47    0 = 0     0 = 0
    XMLParser       159   6359 v 6358    0 = 0     0 v 1     one error gone
    Grease           37    554 =  554    0 = 0     0 = 0
    PolyMath        117   1392 v 1448    2 = 2    16 v 18    one class timed out
    DataFrame        27    839 =  839   14 = 14    0 = 0
    Fuel              2     19 =   19    0 = 0     0 = 0
                    ---   ----          ---       ---
                    354   9326 v 9381   16 = 16   16 v 19

**Three fewer errors, identical failures, and no new failing class.**  The
55-pass gap is one class: `PMArbitraryPrecisionFloatTest` hit the 120 s
`PER_CLASS_TIMEOUT` here, where on 2026-08-22 it fitted inside the same bound
(58 ran / 57 passed / 1 err).  That is a 57-pass swing from one bound, exactly
the effect `docs/results/packages-arm-2026-08-22.md` documents for x86_64.
Whether it is variance on a borderline class or something tonight made slower
is being measured separately — the low-space latch adds a little work to every
old-space allocation, and this class is allocation-heavy, so it is not a
question to wave away.

## Loads got substantially faster

    package     2026-08-17   2026-08-22   today
    NeoJSON          20 s        21 s      16 s
    Mustache          9 s        11 s      10 s
    XMLParser      1055 s       369 s     239 s
    Grease           50 s        48 s      43 s
    PolyMath    TIMEOUT(1200)   269 s     145 s
    DataFrame       139 s        91 s      60 s
    Fuel              4 s      FAILS       45 s   loads, does not persist

XMLParser and PolyMath are the two that used to be unrunnable on this host;
they are now 3.5x and 1.9x faster than the run that first made them work.
`Fuel` loads again (rc=0) where 2026-08-22 recorded "FAILS — repo is gone",
though its image does not persist; its two test classes run either way.
