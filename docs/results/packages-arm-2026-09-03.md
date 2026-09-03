# Package tier, arm64, 2026-09-03 — with the night's VM fixes

`scripts/package-tests-selfhosted.sh` against the pristine base image, all
seven packages loaded natively by our own VM.  20 minutes end to end.
Raw: `packages-arm-2026-09-03.txt`.

## Against the 2026-08-22 baseline

PolyMath is shown at the bound the baseline used (180 s); the first run of the
night used the script's old 120 s default and lost a class to it, which is
explained below.

    package     classes   pass          fail      err        note
    NeoJSON          11    116 =  116    0 = 0     0 = 0
    Mustache          1     47 =   47    0 = 0     0 = 0
    XMLParser       159   6359 v 6358    0 = 0     0 v 1     one error gone
    Grease           37    554 =  554    0 = 0     0 = 0
    PolyMath        117   1450 v 1448    2 = 2    16 v 18    +2 pass, -2 err
    DataFrame        27    839 =  839   14 = 14    0 = 0
    Fuel              2     19 =   19    0 = 0     0 = 0
                    ---   ----          ---       ---
                    354   9384 v 9381   16 = 16   16 v 19

**+3 passes, three fewer errors, identical failures, no timeouts.**

### The 120 s run, and why it was not a regression

The night's first pass used the script's old 120 s `PER_CLASS_TIMEOUT` and
scored 9326 P / 16 F / 16 E / 1 T — 55 passes short.  That was one class and
one knob, measured rather than assumed, because the low-space latch added
tonight touches every old-space allocation and this class is allocation-heavy:

    PMArbitraryPrecisionFloatTest alone, arm64, run 1   158 s  58 ran / 57 P / 1 err
                                              run 2   139 s  58 ran / 58 P / 0 err
    PER_CLASS_TIMEOUT this run (the old default)        120 s   -> TIMEOUT
    PER_CLASS_TIMEOUT the 2026-08-22 runs used          180 s   -> fits

(and the lone error is flaky too — present in one run, absent in the other)

That the baseline used 180 s is legible in its own write-up, which records
x86_64 "TIMEOUT after 180 s" for the same class.  158 s fits inside 180 and not
inside 120, and the class's result at 158 s is byte-identical to the
baseline's.  So the VM is doing the same work in the same time and the gap is
mine for running the default bound.

It does say the default was too small for this class on arm64 as well, not just
under Rosetta — 139-158 s against a 120 s bound, against a knob whose comment
called it "sized for arm64".  **Raised to 200 s**, which covers the measured
cost with margin and still bounds the damage from a genuinely hung class.

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
