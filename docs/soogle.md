# soogle — Smalltalk-package index for JIT test coverage

[soogle](https://github.com/avwohl/soogle) is a ~60k-entry index of public
Smalltalk packages. We mine it for real-world packages to load and run against
the JIT (beyond the 565-class kernel SUnit suite), to find correctness bugs and
measure speed on non-kernel code.

Access: the index DB is reached over `ssh -p 24 <host>` then `mysql soogle`
(credentials via `~/.my.cnf` — never inline them). Our VM can't do HTTPS, so
packages are Metacello-loaded by a stock Cog/Pharo 13.1 VM, saved to an image,
then both the custom JIT VM and stock Cog run the package's `TestCase`s on that
image. Harness: `scripts/pkg-jit-test/` (see `docs/jit-test-packages.md`).

## Added to the tests (6 packages, wired with stock-Cog-vs-JIT A/B results)

    package    classes  Cog P/F/E      JIT P/F/E/T       verdict
    NeoJSON    11       116/0/0        116/0/0           CLEAN PARITY
    NeoCSV     ~3       66/0/0         66/0/0            CLEAN PARITY
    STON       11       317/0/0        316/0/1           1 JIT bug (deep recursion -> True>>\\)
    PolyMath   90       777/0/0        707/0/69/1        51 JIT off-by-one subscript + 18 VM-core
    Fuel       46       733/10/5       aborted early     >=1 JIT bug halts the runner (testBitmap)
    Soil       ~30      ~425/6/2/1     SIGABRT           VM FFI file-lock crash (not JIT)

Of the 6, four run to completion (NeoJSON/NeoCSV/STON clean, PolyMath completes
with bugs); Fuel and Soil hit VM-level aborts.

## Candidates to add (8 curated, not yet wired)

Tier 1 — clean headless load, high JIT value:

    smarr/SMark              11    compute-kernel bench framework  (smark.image already staged in /tmp/pkgtest)
    smarr/are-we-fast-yet     1    canonical VM bench set, self-verifying
    svenvc/zinc              19    encoding/resource subset (network-free)
    pillar-markup/Microdown 160    parser/visitor/exporter, P13+P14 CI

Tier 2 — real value, with a port or P13/GUI caveat:

    j-brant/SmaCC                          dynamic parser codegen (load packages directly to dodge Spec UI)
    magritte-metamodel/magritte            reflective dispatch (P13 load unverified)
    svenvc/P3                              server-free parser/crypto subset only
    KenDickey/Cuis-Smalltalk-Shootout      manual Cuis->Pharo13 port

SMark is closest to ready (image staged; just needs a run + results wired).

## Explicitly skip (not candidates)

    rakki-18/Matrix-Benchmarks    LAPACK FFI + Roassal GUI in the test path
    pharo-project/pharo-benchmarks  Athens/Cairo + Morphic + FFI, ~5y stale
    VMMaker/CogBenchmarks         no GitHub repo / no BaselineOf — unloadable

## On the broader index

The raw soogle index is ~60k entries, but the vast majority aren't
headless-testable (GUI/Morphic/network/FFI deps). The 8 curated candidates above
are the practical headless-loadable, SUnit-bearing subset. To find more, re-query
soogle for packages with a BaselineOf and no Spec/Morphic/Athens/FFI dependency.
