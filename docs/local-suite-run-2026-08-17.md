# arm64 and x86_64 on one machine — 2026-08-17

Apple M4, macOS 27, both VMs built locally (`build-rel` arm64, `build-x86`
x86_64 under Rosetta). No stock Cog on this host: it aborts allocating its code
zone at 0x320000000, which is the ASLR problem this project exists to solve. So
everything below is ours-vs-ours, never ours-vs-Cog.

## C++ / VM-level tier — all pass, both architectures

    binary                  arm64   x86_64
    test_sista_ir           PASS    PASS
    test_asmjit_t1_stub     PASS    PASS
    test_class_table        PASS    PASS
    test_relaunch           3/3     3/3

The 2026-08-11 comparison had x86 `test_sista_ir` failing at the time of its
run, fixed later in `ce2dcdd2`. This confirms it on x86 and confirms arm was
not disturbed.

## Packages, loaded WITHOUT a stock Cog VM

`scripts/package-tests-selfhosted.sh` does the Metacello load with our own VM
and persists with an explicit snapshot. Driving Metacello is a real workload --
network, compiler, file system, large image writes -- and it works:

    package      load          tests
    NeoJSON      20 s          11 classes, 116 pass, 0 fail, 0 err
    Mustache      9 s           1 class,    47 pass, 0 fail, 0 err
    DataFrame   139 s          10 classes, 431 pass, 6 fail, 0 err
    Fuel          4 s           2 classes,  19 pass, 0 fail, 0 err
    XMLParser  1055 s          loaded, but the test pass produced no RESULT
    Grease       50 s          loaded, 0 classes matched the name pattern
    PolyMath   TIMEOUT 1200 s  load never completed

The last three are open. Note the image sizes the loads produce: a 52 MB base
becomes 234 MB (NeoJSON), 401 MB (Grease), 559 MB (DataFrame), 1056 MB
(XMLParser). That growth is worth a look on its own.

## SUnit — the VM is healthy; long-run numbers are not trustworthy

Measured several ways on the same build and machine, and they disagree in a way
that matters for anyone reading a full-suite percentage:

    300 classes, direct (no harness)      5227/5228   0 fail  1 error   99.98%
    300 classes, through SUnitRunner      4861/4902   0 fail  0 error   99.16%
    full run, 4 h cap, arm  (1544 cls)   20261/21251  177 F   573 E     95.34%
    full run, 4 h cap, x86  (1461 cls)   18395/19618  165 F   826 E     93.77%

Both full runs hit the 4-hour cap without finishing. More importantly, **the
failures they report do not reproduce.** Every class that looked like a
regression against the 2026-08-11 baseline passes at exactly the baseline
numbers when run on its own:

    class                        2026-08-11   full run     isolated
    ReflectivityReificationTest  112 P:112    P:61 E:48    112/112
    ReSmalllintTest               58 P:58     P:19 F:38     58/58
    ReflectivityControlTest       71 P:71     P:41 E:25     71/71
    OCASTClosureAnalyzerTest      33 P:33     P:5  E:26     33/33
    MethodMapTest                 30 P:30     P:10 E:17     30/30
    SindarinDebuggerTest          85 P:85     P:60 F:7 E:18 83/85

So there is **no regression** — five of six are byte-identical to the baseline.
`SindarinDebuggerTest` at 83/85 is the one real delta and is worth a look.

What is NOT the cause, each ruled out by measurement rather than argument:

  - *Many classes in one image.* 300 in a single fresh image scores 99.98%.
  - *The SUnitRunner harness.* The same 300 through the runner: 0 F, 0 E.
  - *Snapshot-prepped images.* A snapshot round-trip preserves state and eval
    runs normally on the clone (verified with a marker global).
  - *Code zone exhaustion.* The clean 300-class run ALSO filled the zone
    (196607/196608 KB, 9944 failed compilations) and still scored 0 F / 0 E.
    The zone costs throughput, not correctness.

What is left is cumulative in-image state across many hundreds of classes --
and note the classes worst affected are the metaprogramming ones (Reflectivity
installs metalinks, OCAST rewrites ASTs, Sindarin drives the debugger), which
is exactly the population you would expect to leave residue. That is a
test-isolation property of the image, not a VM fault.

**Practical consequence: do not read a full-suite percentage as a VM quality
number without re-running the failures in isolation.** On this machine the
honest statement is 99.98% over the first 300 classes, with the long-run figure
unexplained by anything in the VM.

## Two methodology traps, both measured

  - Running the suite while compiling turns passes into ERRORs. Batch 1-50:
    772/774 with 0 F / 0 E on three idle runs; a scatter of
    MessageNotUnderstood and "Improper store" errors during a concurrent
    `cmake --build -j4`.
  - Small batches invent failures. Three Calypso classes score 100% alone and
    error wholesale in a batch of 50 (25/25, 36/36, 8/8 -> all errors), because
    they depend on state a fuller run establishes.
