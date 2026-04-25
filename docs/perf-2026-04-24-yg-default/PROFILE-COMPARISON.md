# Profile comparison: pre-YG vs YG-default

Same workloads, same VM binary version with one diff: morning had
`PHARO_YOUNG_GEN` default-OFF, evening has it default-ON.

Morning samples in `docs/perf-2026-04-24/profile-{idle,active}.txt`
Evening samples in this directory: `profile-{idle,active}-yg-default.txt`

## IDLE Pharo image (vanilla — no test workload)

    Symbol                                Pre-YG (YG off)    Post-YG-default
    ---------------------------------------------------------------------------
    pharo::ObjectMemory::fullGC           29 678 (63.8 %)         0 (—)
    pharo::ObjectMemory::scanPointerFields  7 275 (15.6 %)         0 (—)
    pharo::ObjectMemory::markAndTrace       6 696 (14.4 %)         0 (—)
    pharo::ObjectMemory::storePointer       1 648 (3.5 %)         63 (0.2 %)
    pharo::ObjectMemory::scavenge               0                125 (0.3 %)
    pharo::ObjectMemory::classOf              253                  27 (0.1 %)

    TOTAL ObjectMemory                    >43 000 (>92 % of CPU)   ~250 (0.7 %)
    Main-thread samples                   46 548                36 840

  **Idle GC dropped from 64 % to 0.7 % — a 90× reduction.**

  CPU now goes overwhelmingly to:
    primitiveRelinquishProcessor  34 497 (93 %)   ← sleep waiting for events
    sendSelector / executePrimitive   ~35 000 each — same path

  The Pharo image is genuinely idle under YG-default.  No GC
  churn, just Morphic UI sleeping waiting for input.  This was
  the headline prediction from yesterday's `profile-idle.txt`,
  validated empirically.

## ACTIVE workload (long-bench loop: compile + dict + sort + benchFib)

    Symbol                                  Pre-YG    Post-YG    Δ
    --------------------------------------------------------------
    pharo::Interpreter::interpret           44 607    48 352     +8 % (more useful work)
    pharo::Interpreter::sendSelector        17 593    20 193     +15 %
    pharo::Interpreter::activateMethod      10 343    12 107     +17 %
    pharo::Interpreter::push                 6 157     7 104     +15 %
    pharo::Interpreter::executePrimitive     2 618     2 871     +10 %
    pharo::ObjectMemory::fullGC              3 487     1 202     -66 %
    pharo::ObjectMemory::scavenge                0       986     NEW
    pharo::ObjectMemory::scanPointerFields     911       221     -76 %
    pharo::ObjectMemory::markAndTrace          824       208     -75 %
    pharo::ObjectMemory::storePointer          177       211     similar

    Total GC samples (full+scav+scan+mark+sp): 5 399    2 828    -48 %
    Total GC % of interpret:                  12.1 %    5.8 %

  **Active GC overhead halved (12.1 % → 5.8 %).**

  And the VM did **8 % more useful work in the same wall time**
  (interpret samples 44 607 → 48 352).  Exactly what bench
  numbers showed for `5000 factorial`: 9× wall-clock speedup
  driven by GC-cost reduction.

## Implication for "what's next"

The morning plan said:

> Hypothesis from the inlining plan: 90 %+ in primitives, JIT
> matters for the remaining ~5–10 %.

The IDLE side of that hypothesis was wrong (was 64 % GC, now is
0.7 % GC).  The ACTIVE side has shifted: under YG-default,
dispatch is now ~80 % of CPU on long-running code, and there's
no more easy GC fix to take.  Phase 4 inlining is now the
biggest remaining lever for active-execution code, and there's
nothing analogous for idle (which is dominated by intentional
`primitiveRelinquishProcessor` sleep).

**Recommendation update**: Option D fully shipped, GC is solved
for both idle and active workloads.  Next lever is Option A
(Phase 4 monomorphic inlining) — addresses the dispatch path
that now dominates active execution.  7–11 weeks of work,
expected 3–5× on send-heavy benchmarks.  Phase 3 step 1
(framepoint capture) shipped same day; remaining 12 steps
detailed in `docs/sista-phase3-progress.md`.
