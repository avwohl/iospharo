# JIT stress via non-graphics packages

Goal: exercise the JIT beyond the graphics suites with packages that hammer
*different* code paths — compiler/AST, parsing, serialization, reflection,
numerics, graph algorithms. These are kernel-quality packages that ship green
on stock Pharo, so any divergence on our VM is a VM/JIT bug.

## Method

1. Extract each package's TestCase subclasses (`scripts/extract_lists.st`).
2. Run on our VM: `scripts/graphics/run_jit_pkg_queue.sh` (test_load_image +
   Pharo-gfx.image, SUnitRunner auto-fires). Per-test results in
   `docs/results/jitpkg/<tag>_detail.txt`.
3. Stock-Cog baseline: `scripts/graphics/run_cog_baseline.sh` (alt /tmp paths
   so it runs concurrently). Output `docs/results/jitpkg/<tag>_cog.txt`.
4. Δcog diff: `scripts/diff_jit_pkg.py <tag>_cog.txt <tag>_detail.txt` — lists
   tests that pass on Cog but regress on ours (JIT/VM-bug candidates).
5. Classify each candidate by ISOLATION repro on the *clean* image with JIT
   on vs off: `scripts/repro_tests.sh /tmp/harness/Pharo.image 'Class>>sel' …`
   - fails JIT-on only            → JIT bug
   - fails JIT-on and JIT-off     → VM/interpreter/primitive bug (not JIT)
   - passes in isolation          → suite-interaction / image-state artifact

## Summary

14 packages run on our VM (gfx image) vs stock Cog, then JIT-bug verdicts via
clean-image JIT-on-vs-off where it mattered. Verdict legend: JIT = confirmed
JIT bug (clean-image on/off); WS = known WideString bug (non-JIT); STORM =
JIT hang; — = clean / no JIT bug.

       package       cog P/F/E      our (gfx) P/F/E    verdict
       regex         196/0/0        196/0/0            — clean both
       numberparser  38/0/0         36/0/0             — clean
       strings       738/0/0        733/3/0            WS (3 = known WideString, JIT on==off)
       seq           1695/0/0       (skipped — overlaps kernel suite)
       ston          156/0/6        153/4/5            0 JIT; +1 NEW non-JIT word-array bug
       zincenc       120/0/1        109/9/2            WS family (wide-codepoint encoders)
       ast           621/1/1        597/5/19           clean-image on 594/6/20; JIT-off too slow to confirm fraction
       opal          633/0/0        446/30/147         many gfx artifacts; JIT-off too slow to confirm
       microdown     620/4/6        (partial)          batch killed to free VM
       systime       659/0/0        3/0/0 (killed)     NEW Delay/scheduler nil bug breaks valueWithin:
       aigraph       84/0/0         78/0/6             **JIT** — inline IC probe (inline-getter). on/off proven
       ring          290/0/0        250/35/5           **JIT** — inline IC probe (returnsSelf/Literal); +non-JIT metamodel
       reflectivity  322/0/0        STORM              metalink instrumentation send-storm (hang)
       fuel          37/0/474       STORM              reflective-walk megamorphic thrash (also broken on cog)

Clean-image JIT-on-vs-off verdicts (the authoritative JIT-bug test):

       aigraph   JIT-on 78P/6E   JIT-off 84P/0E   -> 6 JIT-induced  (inline-getter)
       ring      JIT-on 263P/25F JIT-off (slow, partial) -> >=8 JIT-induced (returnsSelf/Literal)
       ston      same JIT on/off                  -> 0 JIT-induced

Headline: BOTH aigraph and ring JIT bugs root-cause to the **inline IC probe**
(`PHARO_T1_NO_IC_PROBE=1` fixes both). See below.

## HEADLINE: confirmed JIT correctness bug in aigraph (Prim/Tarjan)

The one unambiguous JIT bug this campaign found. On the **clean** harness image
(Pharo-jit, no FakeGUI, 0 startup-corruption lines), full `AI-Algorithms-Graph`
suite:

       JIT ON  : 78 PASS / 6 ERROR
       JIT OFF : 84 PASS / 0 ERROR    (PHARO_NO_JIT=1)

Only the JIT toggles. The 6:
`AIPrimTest>>{testMinimumSpanningTreeSimple, testMinimumSpanningTreeComplex,
testMinSpanningTreeComplex2}`,
`AITarjanTest>>{testStronglyConnectedGraph, testComplexCycle,
testStronglyConnectedGraphWithObjects}`.

Exception (captured via the runner's stack-trace logging):

       Error: No Element in Graph : #(3 4 2)
         AIPrim(AIGraphAlgorithm)>>findNode:
         AIPrim>>run

`AIPrim>>run` does `fromNode := self findNode: curEdge first` but under JIT
`findNode:` receives the WHOLE edge tuple `#(3 4 2)` instead of `curEdge first`
(`3`). So a JIT'd send passes the wrong argument — `curEdge first` evaluates to
`curEdge`, or the argument slot is mis-loaded. The raw VM log corroborates
stack/arg corruption: garbage receivers appear in the forked test processes —
a **stack address** (`#asInteger not understood by rcvr=0x591885918`, FP is
`0x5bdb...`) and `nil` (`#suspend`/`#acceptVisitor:` to `0x300000000`). Bits
48-63 are clear, so this is distinct from the known classifier-bit leak (task
#10 / b8eead95).

Trigger: **warmup + fork**. Passes in isolation (even JIT-on). Needs
SUnitRunner's multi-test fork sequence — running several different test methods
forks them at priority 40 and warms up multiple JIT compilations; one of the
warmed compiled methods then mis-passes an argument when re-entered in a forked
context. A minimal same-test `forkAt: 40` repro does NOT trigger it (the
multi-method warmup matters).

Reliable repro:

       printf 'AIPrimTest\nAITarjanTest\n' > /tmp/sunit_class_names.txt
       rm -f /tmp/sunit_run_completed.txt /tmp/sunit_test_{results,detail}.txt
       ./build/test_load_image /tmp/harness/Pharo-jit.image      # JIT on  -> 3 ERROR
       PHARO_NO_JIT=1 ./build/test_load_image /tmp/harness/Pharo-jit.image  # -> 0 ERROR

### ROOT CAUSE: the inline IC probe (confirmed by env-knob bisection)

Bisected on the reliable 2-class repro (`Pharo-jit`, JIT on):

       baseline (JIT on)                3 ERROR   (full suite: 6)
       PHARO_NO_J2J=1                   still fails   -> NOT J2J
       PHARO_T1_NO_IC_PROBE=1           0 ERROR (84/84) -> the inline IC probe
       PHARO_T1_NO_INLINE_GETTER=1      0 ERROR (84/84) -> specifically inline-getter

So aigraph's bug is the **inline-getter specialization of the asmjit-T1 inline
IC probe** (`AsmjitT1.cpp` ~4586, `tryGetter`). Ring's 8 JIT-induced failures
are also in the IC probe but a DIFFERENT spec: `PHARO_T1_NO_IC_PROBE=1` fixes
them (ring-min 41/41), and `PHARO_T1_NO_INLINE_RETURNS_SELF=1` /
`PHARO_T1_NO_INLINE_RETURNS_LITERAL=1` each fix 7 of 8; inline-getter-off does
NOT. So the IC probe has correctness bugs across MULTIPLE inline
specializations.

The probe (`t1ICProbe`) was **default-OFF until 2026-05-16** precisely because
of "correctness icData[2]==0 guard, downstream DNUs" (DebugSettings.hpp comment)
— then flipped default-ON. This campaign shows the correctness gap is still
present: the probe takes an IC HIT on a 22-bit class key (`AsmjitT1.cpp:3465`,
`w4 & 0x3FFFFF`) and inlines a getter/returnsSelf/returnsLiteral, but in a
warmup+fork scenario the inlined spec is applied wrongly — reading the wrong
slot (getter → wrong value, e.g. `curEdge first` yields the whole tuple) or
leaving a garbage value that becomes a later send's receiver (the stack-address
/ nil DNUs). The immediate-receiver guard (`tst x1,#7; b.ne dispatchCached`,
:3650) is present, so it is NOT a SmallInteger-as-pointer bug; the insufficiency
is in HIT validation / classification under the warmed, forked state.

Mitigation breadth (clean image, baseline vs `PHARO_T1_NO_IC_PROBE=1`):

       aigraph   78P/6E   -> 84P/0E    FIXED by probe-off
       ring-min  33P/8F   -> 41P/0F    FIXED by probe-off
       ston      153P/4F/5E -> 154P/3F/5E   UNCHANGED — ston is genuinely non-JIT
       ast       592P/6F/22E -> CRASHES after ~34 tests (SIGSEGV in interpret())

So disabling the probe is NOT a universally safe mitigation: the non-probe
dispatch path SIGSEGVs on `ast` (dispatch-heavy compiler suite). That argues for
FIXING the probe's HIT validation, not flipping it off. Root fix: strengthen
IC-HIT validation before dispatching an inline spec (the original `icData[2]==0`
guard direction); lldb single-step `tryGetter` at a failing HIT to see whether
the slot index, the cached method/selector, or IC staleness is wrong. (The
22-bit class key `& 0x3FFFFF` is NOT truncating — image classIndexes are < 2^22 —
so it is not a key-collision bug.)

### FIX APPLIED 2026-05-28: inline-getter default OFF

`t1InlineGetter` now defaults OFF (`DebugSettings.cpp`; opt back in with
`PHARO_T1_INLINE_GETTER=1`, `PHARO_T1_NO_INLINE_GETTER` still forces off).
This reverts the 2026-05-16 enablement of a spec that was OFF for exactly these
"downstream DNUs".

Result with the new default (clean Pharo-jit image):
- aigraph: 84 PASS / 0 ERROR (was 78/6), reliable across runs.
- regex: 196/0/0; ast: 596/5/20 (unchanged, no crash — ast's failures are non-getter).
- No package regressed: across every package tested this session, getter-off
  either equals getter-on or fixes (aigraph).

Perf: a getter-heavy micro-bench (50M iters of `p x + p y`, Point accessors)
measured 3662 ms with getter ON and 3662 ms with it OFF — i.e. **perf-neutral**
(a warm monomorphic getter dispatches about as fast via the normal cached path
as via the inline spec). So the correctness fix costs ~nothing here.

STILL OPEN (task #4):
- The exact arm64 emit defect (a Heisenbug — adding a trace BLR inflated code
  size, pushing a method past its budget so it bailed to interpreter, masking
  2 of 3 errors; overflow bails safely, so this is not corruption but it does
  prove the bug is in the JIT getter inline and is layout-sensitive). Pinning it
  needs lldb single-stepping with a stack watchpoint on the failing send.
- Ring's failures are the same IC-probe family but a DIFFERENT spec
  (returnsSelf/returnsLiteral), and flaky across spec-disables — only
  whole-probe-off reliably fixes ring, and that SIGSEGVs ast (a separate
  non-probe-dispatch bug). So ring is NOT fixed by the getter default; it needs
  the root-cause emit fix. returnsSelf/returnsLiteral were left ON (their perf
  value is unmeasured and ring's fix needs the deeper work anyway).

CAVEAT on the per-test isolation-triage method: because this bug is warmup+fork
dependent, isolation repro (one test at a time) mislabels it ARTIFACT. The
authoritative test is full-suite JIT-on vs JIT-off on the clean image
(`scripts/graphics/run_clean_onoff.sh`); the per-test isolation pass
(`triage_candidates.sh`) only catches non-warmup-dependent bugs.

## Second package with JIT bugs: Ring metamodel

Clean image, full `Ring-Core` suite:

       JIT ON  : 263 PASS / 21 FAIL / 4 ERROR / 2 TIMEOUT
       JIT OFF : 144 PASS / 2 FAIL / 19 ERROR / 1 TIMEOUT  (INCOMPLETE — see caveat)

**8 confirmed JIT-induced** (FAIL on JIT-on, PASS on JIT-off):
`RGClassStrategyTest>>{testCategory, testDefinition, testDefinitionWithSlots}`,
`RGClassTest>>{testCopyForBehaviorDefinition, testDefinition, testTagsCollection,
testTagsForMethodsCollection, testTagsForMethodsRemoval}`. These are *wrong-result*
assertion failures (class definition / category / tags strings differ under JIT) —
a different manifestation than aigraph's exceptions, but same conclusion: JIT'd
code computes the wrong answer. 2 both-fail (non-JIT): `RGEnsureTraitTest` trait
tests.

CAVEAT — **JIT-off is 10-100× slower on reflective code**, so the JIT-off Ring run
hit the 900s cap after 166/290 tests. 17 JIT-on failures are in classes JIT-off
never reached, so their status is unknown. The 8 confirmed are a LOWER BOUND;
Ring likely has more. A definitive count needs a long-timeout JIT-off rerun
(`TO=3600` or `SUnitTimeoutMultiplier`).

## Key findings

### 1. The suite-vs-isolation gap is mostly the IC probe, not (only) the gfx image

Initial read: the gfx image (clean Pharo 13 + SUnitRunner + FakeGUI shims) emits
a `KeyNotFound` / "Decompilation failed" startup storm (known Color/Morphic
issue), and tests that PASS in isolation but FAIL in the full suite looked like
image-state artifacts. **That was partly wrong.** The aigraph "artifacts" pass
in isolation but are CONFIRMED IC-probe JIT bugs in the full suite (same 6
errors on the *clean* image, fixed by `PHARO_T1_NO_IC_PROBE=1`). So the
"passes-alone-fails-in-suite" pattern is largely the IC probe's WARMUP
dependence: in isolation the method runs interpreted / un-warmed and the buggy
inline-spec IC HIT never fires; in the full suite it gets JIT-compiled + the IC
filled, and the bug triggers.

What IS genuinely gfx/image-state vs IC-probe vs non-JIT (clean-image,
probe-off):
- aigraph, ring(RGClass*): IC-probe JIT bugs (probe-off fixes).
- ston (testStrings/WideSymbol/Collections/...): non-JIT (probe-off UNCHANGED,
  same JIT on/off) — WideString + word-array + memory-FS.
- gfx startup storm is real but a smaller contributor than first thought.

Lesson: classify with full-suite clean-image JIT-on-vs-off (and probe-on-vs-off),
NOT per-test isolation — isolation hides warmup-dependent JIT bugs.

### 2. Reflectivity and Fuel storm the JIT (performance pathology, not a crash)

Both hang our VM with a send/patch storm that stock Cog runs in seconds:

* **Reflectivity** (metalinks / bytecode instrumentation): 1.4B+ sends with
  RUN-count=0 — stuck in test-class setup before the first test registers,
  looping on a DNU under `SUnitRunner>>runTestClass:`. Excluded from the batch.
* **Fuel** (reflective object-graph serialization): 6B+ sends, IC hit-rate
  collapses to 17%, **J2J-d ≈ 700M direct call-site re-patches** (~11% of all
  sends re-patch a site). Fuel walks arbitrary object graphs, so each J2J
  call site sees a stream of different callee classes and the direct-patch
  path thrashes instead of transitioning to a megamorphic/IC fallback the way
  Cog's inline caches do. (Fuel is also 474/511 ERROR on stock Cog on this
  image, so low comparative signal.) Excluded from the batch.

`J2J-d` = `Interpreter::jitJ2JDirectPatches` (JITRuntime.cpp:2682, printed at
:2696). A healthy run patches a site once and leaves it; ~700M patches is the
megamorphic-thrash signature.

Root cause: the IC-fill path (`Interpreter.cpp:19608-19642`, reached from
`patchJITICAfterSend`) writes the resolved method's J2J entry into the IC
`extra` slot on every fill, and a fill happens on every IC MISS. Our IC has no
polymorphic/megamorphic cache (PIC) — a send site that sees many receiver
classes never "sticks", so each send is a full method lookup plus a re-patch.
Fuel's run shows 17% IC hit (≈880M misses of 1.06B probes) → ~700M re-patches.
Cog absorbs megamorphic sites with open PICs + a megamorphic lookup cache; we
don't, so reflective/megamorphic workloads (Fuel, Reflectivity) degrade from
fast to effectively-hung. This is an architectural gap (add PICs), not a
one-line fix — tracked as a follow-up, not fixed in this campaign.

### 3. STON — fully triaged, 0 JIT bugs

All 9 gfx-suite failures behave IDENTICALLY with JIT on and off (isolation,
clean image):

       test                                      isolation result    class
       STONReaderTest>>testUser                  PASS                 gfx artifact
       STONReaderTest>>testUser2                 PASS                 gfx artifact
       STONWriteReadTest>>testColors             PASS                 gfx artifact
       STONWriteReadTest>>testTextAndRunArray    PASS                 gfx artifact
       STONJSONTest>>testStrings                 FAIL (wide garbage)  known WideString bug
       STONReaderTest>>testWideSymbol            FAIL (wide garbage)  known WideString bug
       STONWriteReadTest>>testSymbols            FAIL (parser err)    WideString family
       STONWriteReadTest>>testCollections        FAIL (word-array)    NEW VM bug (non-JIT)
       STONWriteReadTest>>testMemoryFileRefs     FAIL (memory FS)     env / non-JIT

* **NEW (non-JIT) word-array bug**: `testCollections` round-trips an
  `IntegerArray`/`Float32Array` and gets wrong element values —
  `Float32Array(1.0 2.0 3.0)` reads back as denormal garbage
  (`7.0e-45 1.7e38 7.0e-45` = integer bit-patterns reinterpreted as float32).
  Same with JIT off, so it's a primitive/interpreter bug in 32-bit word-array
  `at:`/`at:put:` (or literal construction), not the JIT. Needs its own task.

### 4. systime — Delay/scheduler bug breaks `valueWithin:` (timeout) tests

`System-Time-Tests` hangs our VM on the `BlockClosureValueWithinDuration`/
`valueWithin:` timing tests (each burns the 300s per-test cap). The run log
shows the cause:

       [DNU] #asInteger not understood by rcvr=nil in #setDelay:forSemaphore: P79
       [DNU] #< not understood by rcvr=nil in #setDelay:forSemaphore: P79
       [MUSTBOOL] value_class=UndefinedObject in #setDelay:forSemaphore: rcv_class=DelayWaitTimeout
       [MUSTBOOL] value_class=UndefinedObject in #ifNotNil: rcv_class=DelaySemaphoreScheduler
       [DIAG] P80 DelaySemaphoreScheduler>>whileTrue: ip=210

A `nil` (0x300000000) flows where a delay value is expected inside
`DelayWaitTimeout>>setDelay:forSemaphore:`, so `asInteger`/`<` DNU and the
timeout never arms — the block waits forever. Cog passes systime 659/0/0. The
batch run was killed after 3 tests to unblock the queue; needs isolation triage
(JIT on/off) to confirm whether the nil originates in a JIT'd method or a Delay
primitive. Captured signature: `/tmp/systime_delay_bug.txt`.

### 5. strings — 0 new bugs (known WideString bug)

All 3 candidates are the already-documented WideString-WriteStream bug
(WIP.md): `StringTest>>testOnlyLetters`, `testWithInternalLineEndings`,
`testWithUnixLineEndings`. Same failure JIT on/off; not a JIT bug.

## Pending isolation triage

Candidate lists staged at `/tmp/cand/<tag>.txt`:

* **aigraph** (6) — AIPrimTest (MST), AITarjanTest (SCC). HIGH priority: pure
  recursive/comparison compute, clean on Cog. Best shot at a real JIT bug.
* **zincenc** (11) — wide-codepoint UTF-8/16/32 encoders; expect WideString family.
* **ast** (24) — OpalCompiler AST-node tests.
* **ring** (40) — Ring metamodel (class/trait/metaclass definitions +
  implicit-environment); smell like gfx live-class-model corruption.
* opal / microdown / systime / strings / seq — pending batch completion.
