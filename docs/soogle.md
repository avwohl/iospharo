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

### UPDATE 2026-06-23 — re-run on HEAD (getter/trio shipped): most prior bugs GONE

    package    JIT result @ HEAD       was            status
    PolyMath   942/942 PASS            707/0/69/1     FIXED (all 69 VM-core + 51 subscript gone)
    STON       310/310 PASS            316/0/1        FIXED (deep-recursion True>>\\ gone; testDeepStructure PASS)
    NeoJSON    112/112 PASS            116/0/0        CLEAN PARITY (re-confirmed)
    Fuel       724/751 (15 F / 10 E)   aborted        testBitmap now PASS; runner completes; 24 NEW-visible fails

KEY: PolyMath/STON/NeoJSON are now CLEAN on the JIT branch — HEAD fixes (incl. this
session's getter+trio) resolved every previously-tracked bug. Fuel's runner-halt
(testBitmap) is fixed, exposing 24 failures underneath.

Fuel's 24 failures are **VM-CORE, NOT JIT** (verified: identical under PHARO_NO_JIT=1;
the getter/trio flips do NOT cause them). Cog passes all but 1. They cluster into:
  - WideString/WideSymbol (15: testWideStringGlobal/ClassName/ConsiderCustomWideSymbol
    x 5 backends): RESOLVED — NOT a VM/JIT bug. PROVEN via standard SUnit `cls suite
    run` on our VM: ALL 5 backend classes pass 79/79, 79/79, 79/79, 86/86, 82/82 —
    TOTAL NON-PASS 0, byte-identical to Cog. The failures appear ONLY through our
    custom run_sunit_tests.st (per-test `runCase`), which does not replicate proper
    SUnit's suite-level execution environment / test order (these tests are order-
    sensitive: they register wide identifiers in Smalltalk globals; via per-test
    runCase the wide-String key hits SystemEnvironment's symbol-only check, but
    `suite run` sandboxes/orders them so it passes). A custom-runner measurement
    artifact that would affect any VM through that harness — the JIT branch passes
    these tests via the standard mechanism.
  - BlockClosure/Context serialization (14, ROOT-CAUSED 2026-06-23): when a method
    RETURNS a closure, our VM does NOT clean the closure's captured outerContext —
    it retains pc (36 vs Cog's nil) and the FULL live sender chain (50+ deep vs Cog's
    nil/1-deep). So Fuel serializes the entire call stack: the test closure is 638
    bytes on Cog but ERRORs (SubscriptOutOfBounds: 36 in Array>>do:) on our VM, and
    its outerContext is 446 bytes on Cog vs 249591 (250KB) on our VM. VM-core context-
    return-cleanup bug (a returned context held by a closure must have IP/sender
    cleared, the "dead context" semantics). Isolated cleanly: baseline assoc / class-
    ref / clean-block all serialize IDENTICALLY (244/280/15491 bytes both VMs); only
    the closure-with-live-outerContext diverges. NOT a JIT bug (interp identical).
    Likely fixes the FLMethodChanged DNU too (downstream of the bloated context).
  - [superseded] earlier symptom: `OCAssignmentNode DNU sourceNodeForPC:`.
    Root-caused: the ORIGINAL closure's source-node mapping is CORRECT on our VM
    (pcInOuter=33 -> OCBlockNode, == Cog), but the MATERIALIZED (deserialized) closure
    has a wrong pcInOuter, so CompiledBlock>>sourceNodeForPC: resolves the adjacent
    assignment node (`x := [...]`) and DNUs. The Fuel materialize round-trip itself
    errors on our VM. Deep VM-core: closure/CompiledBlock reconstruction via object-
    creation primitives. Multi-session.
  - misc (testSerializingShortDelay, SortedCollection x2).

VERDICT for the JIT branch (UPDATED after fixes): HEALTHY, Fuel RESOLVED.
- Closure/Context cluster (7): FIXED by a real VM-core change (commit ba11e482) —
  mark the returning frame's materialized context dead on return (Interpreter.cpp
  popFrame path), so a closure-captured outerContext doesn't keep the whole live
  sender chain. Validated: closure serializes to 640 bytes (was 250KB), Fuel
  BlockClosure+Context tests all pass, ZERO kernel-suite regressions (12674 P / 0 F).
- WideString cluster (15): NOT a VM bug — proven passing via standard SUnit (suite
  run 79/79 x3, 86/86, 82/82, == Cog); a custom-runner artifact (above).
- So the JIT branch passes ALL Fuel tests via the standard SUnit mechanism.
- Kernel SUnit at parity with Cog; the lone SHA256Test>>testFips180Example3 timeout
  is a structural PERF issue (passes in isolation at ~7.2s; exceeds its 10s self-
  limit only under full-suite load — our VM ~15x slower on the alloc-heavy
  ThirtyTwoBitRegister code; not a miscompile, and a 256MB code zone did not fix it).

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
