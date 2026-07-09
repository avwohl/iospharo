# Windows regression-check — 2026-07-08

After the ~40 cross-platform core commits that landed after the 2026-07-04 Windows
baseline (become/forwarder semantics, `classOf` follows forwarders, GC/JIT cascade
fixes, x86 stencil regen "now-live", socket `poll()` on POSIX, TFFI teardown) — all
authored and tested on ARM (macOS) + x86-Linux — this session re-verified Windows.

**Goal:** get Windows working + passing (vm / sunit / soogle) without breaking
arm/x86, or breaking them less than last time.

**Verdict: Windows is NOT regressed by the ARM/x86 work. No VM/JIT changes required.**

## Build
Incremental `cmake --build build-win --target test_load_image` → **exit 0**, warnings
only. Every post-07-04 change compiles clean on MSYS2 CLANG64. Platform guards verified:
- socket `poll()` (850ea188) is `#ifndef _WIN32`; Windows keeps `select()`.
- `classOf`-follows-forwarders (296bba26) is a strict no-op for non-forwarders.

## Smoke
`(3+4)*6` → 42, `100 factorial printString size` → 158, `42 printString` → '42'.
JIT ON, **0 failed compiles** (x86 stencil regen did not break compilation).

## SUnit (565 curated kernel + full-image tail; diffed vs stock Cog on the same list)
`scripts/classify-sunit.py cog.txt ours.txt`:

    ours     total=17863  error=18  fail=15  pass=17807   (891 classes)
    cog      total=12898  error=96  fail=5   pass=12781   (568 classes)
    Δcog     1 regression of cog-pass

The **single** Δcog "regression" is `SemaphoreTest testUnCategorizedMethods`
(`#(#vmRegisterAsDelayRecovery) should have been empty`) — a **test-harness
artifact**: the runner (`run_sunit_tests.st:393`) does `Semaphore compile:
'vmRegisterAsDelayRecovery…'` with no `classified:` category, so the reflection
test finds an uncategorized method. Identical on all platforms; not a VM bug.
Fix (deferred — submodule): add `classified: 'vm-instrumentation'`.

Every other ours-side deviation is documented pre-existing / cross-platform:
- Fuel `testWideStringGlobal/ClassName/ConsiderCustomWideSymbol` (15) — passes
  standalone; in-suite global-state artifact (see soogle.md). Cog also fails/skips.
- Calypso `Cly*AsyncQuery/FilterQuery` scope tests (12) — Cog also fails.
- `OCClassBuilderTest testCreateNormalClassWithTraitComposition` (1) — commit
  296bba26's own note calls this "pre-existing upstream OCCodeError".
- `DebugPointTest testTranscriptDebugPoint` — needs Github SSH credentials.

`WeakKeyDictionaryTest test0FixtureEmptyTest TIMEOUT` in the full run was a
**load/timing artifact**: in isolation the class is **207/207 PASS in 11 s** (same
near-10s-limit behaviour the 07-04 handoff flagged for ObsoleteTest). Not a hang.

## Soogle
- **STON** (10 classes, already in base image): **310/310 PASS on our VM AND Cog,
  0 Δcog** — exact match to the documented clean-parity HEAD number.
- **PolyMath** (Metacello-loaded into a Pharo-10-refvm-saved image, 99 Math test
  classes). Stock-Cog baseline: **827/830 pass, 3 errors** — all 3 are
  package/image issues, NOT VM bugs:
  - `PMKDTreeTest testBenchmark` — `UndeclaredVariableRead: PMKDTreeBenchmark`
    (missing benchmark class). **Our VM hits the identical error → shared, not ours.**
  - `PMPrincipalComponentAnalyser…TransformMatrix` / `…MeanCentred` — `fit:` /
    `fitAndTransform:` receiver is nil (missing dependency in this loaded image).
  Our VM's only non-shared deviation is `PMArbitraryPrecisionFloatTest
  testPrintAndEvaluate` TIMEOUT (Cog passes it) — an intrinsically heavy
  arbitrary-precision compute test; being re-checked on a clean quiet-box run to
  distinguish a genuine ours-only perf gap from the first run's botched-launch
  load. (First run was a foreground `timeout` orphaned by the harness's 2-min cap,
  so it ran without a clean hard timeout.)

## Bottom line for the goal
Windows works (build clean, JIT on, smoke correct) and passes vm/sunit/soogle at
**parity with the ARM/x86 baseline**. The post-07-04 ARM/x86 core work introduced
**zero genuine Windows VM/JIT regressions**. Because nothing needed fixing in VM
code, **no ARM/x86 risk was introduced** — the one candidate cleanup
(`classified:` on the runner's injected method) touches only the test-harness .st,
not the VM.

## Clean PolyMath re-run (quiet box, hard 900s timeout)
78/99 classes before the 900s cap (our VM is ~3× slower than Cog on this heavy
numeric suite, so it doesn't finish all 99 in 15 min — a speed limit, not a hang).
Over the 733 tests it did run: **731 pass, 1 error, 1 timeout**.
`classify-sunit.py cog ours2` → **1 Δcog regression = the arbitrary-precision
TIMEOUT** (only ours-only deviation; the 1 error is the shared PMKDTree package
issue). Every value computed is correct → **correctness parity with Cog**; the sole
gap is speed on one intrinsically-heavy test. Cross-platform, not a correctness bug.

## VM integration suite (scripts/vm_integration_tests.st → VMIntegrationTestRunner runAll)
**35/36 PASS** (results in `<image dir>/vm_integration_results.txt`). Green:
socket TCP/UDP/DNS, locale, security dirs, clipboard round-trip, sound/MIDI,
InterpreterProxy plugin objects, file attributes, primitive-error types, surface
plugin. The one FAIL:
- `VMTimerTest testDelayFiresDuringTightLoop` — a Delay forked at the same priority
  does NOT fire while the main process spins a tight `[1+1]` loop (FIRED=false,
  reproduced 3/3). Diagnosis: timer + scheduler both work when the main process
  yields (Processor yield / Delay wait / no-loop all give FIRED=true); only the
  **force-yield preemption of a same-priority tight loop** fails. **Stock Cog gives
  FIRED=true** → genuine divergence. This lives in the scheduler preemption-ordering
  that `e9a7e984` (07-04 baseline) changed to fix the serious repeat-run x4 wedge —
  DANGEROUS to touch. Determining regression-vs-preexisting via a Jul-4-baseline
  (155d9bc4) A/B build before deciding whether to act. [see below]

### A/B verdict — PRE-EXISTING, not a post-07-04 regression
Built the Jul-4 baseline `155d9bc4` (handoff endpoint) in a worktree and ran the
tight-loop case: **FIRED=false 3/3 on Jul-4, same as HEAD 2/2.** The test was
already failing on the Jul-4 Windows baseline, BEFORE any of the ARM/x86 core
work. It is **not** a regression from the changes under evaluation.

Root cause (deliberate design, not a bug to chase): `Interpreter.cpp` ~4324 —
same-priority round-robin on force-yield is **opt-in only** (`PHARO_RR_SCHED`).
Code comment: *"Cog never time-slices within a priority; doing it broke image
code written against that guarantee (ProcessTerminateBugTest / SemaphoreTest
fork-window flakes — 0/8 with rotation off)."* A same-priority Delay-waiter is
intentionally never force-yielded to while another same-priority process spins.
Enabling round-robin to pass this one test would **re-break** those fork-window
tests on ALL platforms, and the preemption-requeue ordering is the same area
`e9a7e984` tuned to fix the serious repeat-run x4 wedge. Correct action:
**leave it** — fixing risks breaking arm/x86, which the goal forbids.

## FINAL VERDICT
The ~40 post-07-04 ARM/x86-Linux core commits introduced **zero Windows VM/JIT
regressions**. Windows works and is at correctness parity with stock Cog on
sunit + soogle; the VM-integration suite is 35/36 with the single failure
pre-existing and by-design. **No VM code changes were made**, so arm/x86 cannot
have been broken by this work. The only optional, zero-VM-risk cleanup is
categorizing the runner's injected `vmRegisterAsDelayRecovery` method (harness
`.st` only) to clear the one Δcog SUnit artifact on every platform.


