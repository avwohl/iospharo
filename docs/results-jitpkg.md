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

## Pending isolation triage

Candidate lists staged at `/tmp/cand/<tag>.txt`:

* **aigraph** (6) — AIPrimTest (MST), AITarjanTest (SCC). HIGH priority: pure
  recursive/comparison compute, clean on Cog. Best shot at a real JIT bug.
* **zincenc** (11) — wide-codepoint UTF-8/16/32 encoders; expect WideString family.
* **ast** (24) — OpalCompiler AST-node tests.
* **ring** (40) — Ring metamodel (class/trait/metaclass definitions +
  implicit-environment); smell like gfx live-class-model corruption.
* opal / microdown / systime / strings / seq — pending batch completion.
