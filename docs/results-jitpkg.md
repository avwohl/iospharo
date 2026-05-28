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

## Summary (our VM, Pharo-gfx.image suite run)

       package       our PASS  F   E    cog PASS  F   E    Δcog candidates  notes
       regex         196       0   0    196       0   0    0                clean both
       numberparser  36        0   0    38        0   0    0                clean
       aigraph       78        0   6    84        0   0    6                Prim MST + Tarjan SCC — HIGH signal
       zincenc       109       9   2    120       0   1    11               wide-codepoint encoders (WideString family)
       ston          153       4   5    156       0   6    9                triaged: 0 JIT bugs (see below)
       ast           597       5   19   621       1   1    24               OpalCompiler AST nodes — triage pending
       ring          250       35  5    290       0   0    40               Ring metamodel (class/trait defs) — triage pending
       systime       ...                659       0   0    ...              running
       strings       ...                738       0   0    ...              running
       opal          ...                633       0   0    ...              running
       microdown     ...                620       4   6    ...              running
       seq           ...                1695      0   0    ...              running

(Counts are gfx-image suite runs and OVER-count real bugs — see finding #1.)

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

Next: lldb per CLAUDE.md — break at `sendDoesNotUnderstand` when the receiver is
a stack address (non-heap, FP-range), walk back to the JIT frame and the
stencil whose epilog/arg-load leaves the wrong slot. Likely a J2J or
process-resume frame-restore that mis-sets an argument/receiver slot.

CAVEAT on the isolation-triage method: because this bug is warmup+fork
dependent, isolation repro (one test at a time) mislabels it ARTIFACT. The
authoritative test is full-suite JIT-on vs JIT-off on the clean image
(`scripts/graphics/run_clean_onoff.sh`); the per-test isolation pass
(`triage_candidates.sh`) only catches non-warmup-dependent bugs.

## Key findings

### 1. The Pharo-gfx.image inflates failure counts (suite/image artifact)

The gfx image (clean Pharo 13 + SUnitRunner + FakeGUI shims) emits a
`KeyNotFound` / "Decompilation failed" / WorkingSession error storm at startup
(the known Color/ColorRegistry + Morphic issue). That corrupted startup state
leaks into later tests. STON's `testUser`, `testUser2`, `testColors`,
`testTextAndRunArray` all FAIL in the gfx suite but PASS in isolation on the
clean image — so they are image-state artifacts, not bugs in the tested code.

Consequence: the suite numbers above are a *net* for catching candidates, not a
bug count. Real classification is done by isolation repro on the clean image
(`Pharo-jit.image` = clean Pharo + SUnitRunner only, no FakeGUI — built for
cross-checks).

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
