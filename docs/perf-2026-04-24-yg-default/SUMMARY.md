# Perf comparison: Cog vs default (YG on) vs PHARO_NO_YG=1

Median of 3 runs each, fresh prepped Pharo 13 image, M-series arm64,
2026-04-24 evening (after class-table fix + YG default-on).

## Headline table

    Workload                Cog       Default    PHARO_NO_YG=1   Default vs Cog
    tinyBench bytecodes/sec  74.0 M    18.0 M    17.4 M           4.1× behind
    tinyBench sends/sec       3.66 G  133.2 M   139.7 M          27.5× behind
    fib(28)                   2 ms     74 ms     68 ms           37.0× behind
    sieve x100                9 ms    137 ms    128 ms           15.2× behind
    sort 100K                16 ms    270 ms    285 ms           16.9× behind
    dict 50K put+get         22 ms    178 ms    390 ms            8.1× behind  ←  closed from 18×
    sum 1M                    5 ms     81 ms     73 ms           16.2× behind
    5000 factorial           34 ms     26 ms    211 ms            BEATS Cog by 24%
    1M block invocations      0 ms     23 ms     21 ms            (Cog too small)
    1M getter+yourself        4 ms     96 ms     85 ms           24.0× behind
    100K Array allocations    0 ms      5 ms      4 ms            (Cog too small)

## Key findings

### We now beat Cog on `5000 factorial`

  Default JIT under YG-on: **26 ms**
  Stock Cog:               **34 ms**

The benchmark allocates O(n²) intermediate LargeIntegers.  YG
scavenges these cheaply; mark-sweep had to walk the whole heap.
Cog has a more sophisticated GC but our scavenge happens to win
here.

### Big closure on `dict 50K put+get`

  Pre-YG: 390 ms  (18× behind Cog)
  Default (YG on): **178 ms**  (8× behind Cog)
  Cog: 22 ms

Dictionary entries are short-lived young allocations.  Same
mechanism as factorial.

### Small losses (5-13%) on pure-arith workloads

  Workload           NO_YG    Default (YG on)  Δ
  fib(28)            68 ms    74 ms            -9 %
  sieve x100        128 ms   137 ms            -7 %
  sum 1M             73 ms    81 ms           -11 %
  1M getter         85 ms     96 ms           -13 %
  1M blocks          21 ms    23 ms           -10 %

These workloads allocate little; YG's write barrier is overhead
without offsetting savings.  Acceptable trade for the
allocation-heavy wins, which dominate real Pharo IDE workloads.

### Cog gap by workload

    Cog vs default (YG on):
      Best:    factorial         **WE WIN by 24 %**
      2nd:    dict 50K           8.1× (was 18× pre-YG)
      Mid:    sieve              15.2×
              sum 1M             16.2×
              sort 100K          16.9×
      Worst:  getter             24.0×
              tinyBench sends    27.5×
              fib(28)            37.0×

The Cog gap is now bimodal: where YG saves allocation cost we're
competitive (sometimes ahead).  Where allocation cost was already
small, we're 15-37× behind — the architectural T1-stencil-vs-
register-allocation gap that Phase 4 inlining would close.

## Compared to morning numbers

Morning bench used "JIT" (= YG off) as the "Our" column.  Today's
"default" column corresponds to the new shipping default (YG on).
The morning bench's 17.4 M bps / fib 68 ms / dict 380 ms numbers
are now reproduced as the **PHARO_NO_YG=1** column above —
identical to morning, as expected.

Raw runs in this directory: perf-cog-yg-run{1,2,3}.txt,
perf-default-yg-run{1,2,3}.txt, perf-noyg-run{1,2,3}.txt.
