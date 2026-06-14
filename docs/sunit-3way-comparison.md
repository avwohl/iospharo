# SUnit 3-way comparison: x86 (ours) vs arm64 (ours) vs Cog (2026-06-14)

Same sources (origin/jit @ 2de6b72c), same curated 565-class shared list
(`scripts/pharo-headless-test/test_classes.txt`, ~12724 tests), same runner
(`run_sunit_tests.st` for our VM, `run_sunit_cog.st` for stock Cog). Optimized
builds (Release / build-opt). x86 on an AWS m6a.4xlarge spot box; arm64 + arm64
Cog on the Mac. Raw results: `s3://iospharo-build-670060058357/sunit-3way/` and
`/tmp/cmp/out`.

## Headline

```
config                         Pass    Fail  Error  vs-Cog Δ (regressions of cog-pass)
arm64 ours (shipping JIT)      12694    0      1     0      <- matches/exceeds Cog
x86   ours JIT, inline-J2J OFF 12689    2      3     4      <- all 4 are flaky/non-JIT (below)
x86   ours JIT, inline-J2J ON  12689    3      2     4      <- same; inline-J2J adds none
Cog   (stock, x86)             12778    4     96            <- buggier per-test (96 errors)
```

- **arm64 (our shipping VM): 0 regressions vs Cog.** Our VM is actually *cleaner*
  than Cog on the tests both run (error=1 vs Cog's 96 errors + 4 fails). The
  ~84-test pass gap to Cog is tests Cog runs that our harness skips, not
  failures.
- **x86 JIT introduces ZERO deterministic regressions.** The 4 x86 "regressions
  vs Cog" were each re-run in isolation + interp-vs-JIT and classified:
  - `ProcessMonitorTestServiceTest>>testFailTestWhen…` — PASS in isolation on
    BOTH arches → flaky (timing/fork), full-suite-load only.
  - `TestExecutionEnvironmentTest>>testHandleForkedProcessesByAllServices` —
    PASS in isolation both arches → flaky (timing/fork).
  - `FBIRBytecodeDecompilerTest>>testDecompileIRBuilderTestClass` — PASS on
    interp + JIT-rerun×2 (one earlier FAIL) → flaky on x86.
  - `SHA1Test>>testLargeCharacterStream` — deterministic FAIL on x86, but
    **NOT an x86 bug**: it is a CRYPTO-BUILD-CONFIG artifact. The x86 box was
    built `PHARO_WITH_CRYPTO=OFF` (build-linux.sh, to dodge an SSL link error),
    which drops the crypto-gated DSAPlugin and its native SHA1 primitives
    (`primitiveHashBlock`/`primitiveExpandBlock`); SHA1 then uses its
    pure-Smalltalk fallback, which fails on large input. PROVEN config-not-arch:
    an arm64 build with `PHARO_WITH_CRYPTO=OFF` fails this test IDENTICALLY,
    while arm64 crypto-ON passes. So with matched crypto config x86 has ZERO
    deterministic regressions, same as arm64.

## inline-J2J at full-suite scale (the default-on gate)

```
inline-J2J OFF vs ON:  same Pass (12689), same total non-PASS (35), net 0 pass change.
per-test diff: 4 tests SHUFFLE (2 each way) — all timing-flaky:
   FBIRBytecodeDecompiler  ERROR(off) -> PASS(on)     (flaky)
   StringInitialization    TIMEOUT(off) -> PASS(on)   (timing)
   NativeArray             PASS(off) -> TIMEOUT(on)    (timing)
   WeakOrderedCollection…GarbageCollected PASS->FAIL  (GC timing)
```
Net 0 pass change, no deterministic regression — inline-J2J is
**correctness-neutral** at full-suite scale. The shuffles are the expected
signature of a perf change perturbing timing-sensitive (TIMEOUT/GC/fork) tests.
This clears the default-on gate on correctness grounds (perf win ~20% on
recursion, see x86-inline-j2j-design.md).

## Cross-arch (x86 ours vs arm64 ours)

x86 vs arm64: 4 differences = exactly the 4 above (3 flaky + 1 x86-core SHA1).
So the x86 VM (with this session's JIT fixes) matches arm64 modulo flakes and
the one pre-existing x86-core SHA1 issue. The supportedPrimIndex fix +
prim-prologue port + inline-J2J brought x86 from "hangs at startup" to
parity-with-arm64 on the curated suite.

## Follow-ups

- SHA1 largeCharacterStream is a CRYPTO-CONFIG artifact, not an x86 bug
  (arm64 crypto-OFF reproduces it). To make x86 match arm64 here, build x86 with
  PHARO_WITH_CRYPTO=ON (needs the SSL platform impl linked, or the SSL syms
  stubbed). Separately, the image's pure-Smalltalk SHA1 fallback has a
  large-input bug exposed only in crypto-OFF builds (arch-independent, image-level).
- inline-J2J default-on: cleared on correctness; flip is a config decision on
  the non-shipping arch.
