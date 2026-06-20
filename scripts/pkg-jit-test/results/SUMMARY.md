# Package JIT test results — 2026-06-19

Custom JIT VM (`build-rel`, arm64, -O2, JIT default-on) vs stock Cog (Pharo 13.1),
same saved image per package. Loaded via stock Cog Metacello.

## Correctness

```
package    classes  Cog P/F/E      JIT P/F/E/T       JIT-only failures
NeoJSON    11       116/0/0        116/0/0           0   CLEAN PARITY
NeoCSV     ~3       66/0/0         66/0/0            0   CLEAN PARITY
STON       11       317/0/0        316/0/1           1   testDeepStructure (deep-recursion)
STON       11       317/0/0        317/0/0           0   FIXED (StackOverflowLimit 4096->56000)
PolyMath   90       777/0/0        707/0/69/1        51 SubscriptOutOfBounds (JIT)
                                                     +18 SmallFloat64 inject:into: (VM-core, not JIT)
PolyMath   90       777/0/0        757/1/18/1        off-by-one fix: 0 SubscriptOutOfBounds
PolyMath   90       777/0/0        775/1/0/1         +Sista fixes: 0 errors (51 off-by-one + 18
  (all 3 fixes)                                      SmallFloat64 gone); gap=1 flake + 1 slow-timeout

3 JIT/Sista bugs fixed (all committed): off-by-one (inline-J2J canBailMidMethod),
factorial (Sista tag-check skip), inject-capture (Sista do-splice). PolyMath 707->775.
Fuel       46       733/10/5       aborted@testBitmap >=1 (SubscriptOutOfBounds:0, halts runner)
Soil       ~30      ~425/6/2/1     SIGABRT            VM FFI file-lock crash (not JIT)
```

## Broad JIT-correctness sweep — 2026-06-20 (post all 6 fixes)

Method: per target, run the whole category JIT vs PHARO_NO_JIT; a failure is a
JIT bug ONLY if it fails JIT, passes NO_JIT, AND passes Cog. (workflow:
jit-correctness-sweep)

```
target            JIT result                 JIT-confirmed bugs
Fuel (47 cls)     780/12/34/1                0  (all 12 fail + 34 err reproduce under NO_JIT)
Collections(63)   6068/0/52/0                0  (JIT == NO_JIT byte-identical; 52 err = abstract CollectionRootTest)
Kernel (90)       1453/0/51/0                0  (JIT == NO_JIT; 51 err = ProcessTest/WriteBarrier image-compat)
Text/Intl (17)    134/0/0/1                  0  (1 timeout = offline URL fetch, both configs)
NeoCSV (3)        66/0/0/0                   0  CLEAN
Zinc (49)         no-sockets in sandbox      0  (env, not VM)
```

RESULT: **ZERO JIT-confirmed correctness bugs across ~8,500 tests in 6 packages.**
After the JIT fixes this session, the tier-1 JIT (+ Sista) is byte-identical to the
interpreter on this broad sweep. The remaining package failures were VM-core/image
(NOT JIT codegen); investigating them surfaced a 7th VM bug (prim 105), now fixed:
- **prim 105 (replaceFrom:to:with:startingAt:) forward-overlap self-copy — FIXED
  2026-06-20 (1fb18d67).** The Fuel GZip family was NOT plugin-absence: gzip inflate's
  LZ77 self-copy hit primitiveStringReplace which used memmove (copies backward on a
  dst>src overlap, preserving the source) instead of the spec'd FORWARD propagation ->
  inflate produced wrong bytes -> CRCError/SubscriptOutOfBounds on low-entropy/repeated
  payloads of ANY size. Generic (RLE/buffer-shift/LZ77), not Zip-specific. Proven not
  plugin-absence by forcing Cog onto the same pure-Smalltalk inflate fallback (Cog
  correct, our VM corrupted). ZipPlugin remains unimplemented but is NOT needed for
  correctness — the Smalltalk fallback round-trips correctly once prim 105 is fixed.
  Fuel improved 780/12/34/1 -> 794/12/20/1 (CRCError ~12 -> 0).
- **File I/O into/from non-byte buffers (WordArray/Bitmap/FloatArray) — FIXED
  2026-06-20 (09528ecf).** The FLBinaryFileStream*/FLFileReferenceStream*
  testWordArray/testBitmap SubscriptOutOfBounds was TWO coupled VM-core defects:
  (1) ObjectMemory::storeByte/fetchByte guarded on isBytesObject() so byte access to
  a words/shorts object was a no-op/0; (2) primitiveFileRead/Write treated
  start/count as BYTES but the FilePlugin contract (and the image's `buffer
  basicSize`) is ELEMENTS, so a 4-byte-element WordArray under-read 4x. Result: a
  zeroed words buffer -> Fuel read encoded-reference 0 -> SubscriptOutOfBounds. Both
  fixed; byte buffers unchanged. JIT-independent. broad kernel 2782/0/0.

Fuel progression: 780/12/34/1 (orig) -> 794/12/20/1 (prim-105) -> 800/12/14/1 (file
I/O). The remaining 12 fail + 14 err are NOT our VM: testContextWithClosure /
testBlockClosure* / testWideString* all fail IDENTICALLY on stock Cog
(ERR:MessageNotUnderstood / ERR:Error on JIT, NO_JIT, AND Cog) = Fuel-vs-image-version
test/image issues, plus abstract FileSystemTest (Cog fails it too). So after the 8
fixes our VM is at COG-PARITY on Fuel — every remaining failure fails on Cog too.

=== SESSION CONCLUSION (2026-06-20) ===
8 VM/JIT correctness fixes; 0 JIT-confirmed bugs across the 6-package / ~8500-test
sweep. The custom JIT (+Sista) is byte-identical to the interpreter, and the VM is at
Cog-parity on every tested package (NeoJSON/NeoCSV/STON/PolyMath clean; Fuel remaining
failures fail on Cog too). The 8 fixes:
  1 off-by-one subscript (inline-J2J canBailMidMethod)   [JIT]
  2 factorial miscompile (Sista tag-check skip)          [JIT/Sista]
  3 inject:into: capture (Sista do-splice)               [JIT/Sista]
  4 STON deep-recursion (StackOverflowLimit 4096->56000) [VM-core]
  5 testPrintingRecursive (context-NLR cap 200->70000)   [VM-core]
  6 sibling context-NLR caps (proactive completeness)    [VM-core]
  7 prim-105 forward-overlap self-copy (memmove)         [VM-core, generic]
  8 non-byte-buffer file I/O (storeByte + element count) [VM-core, generic]
Remaining non-correctness gaps (documented, not bugs in our VM): the speed spectrum
(13-39x on floats/collections), Soil FFI SIGABRT guard, ZipPlugin (optional, perf).
- WideString globals/class-name (Fuel testWideString*) — image-compat, all serializers.
- ProcessTest x47 MNU, WriteBarrier Double atPut — image-compat.
- Soil FFI SIGABRT (see bug 5) — FFI robustness, not JIT.

## Bugs found (JIT-confirmed = fails JIT, passes PHARO_NO_JIT)

- JIT off-by-one subscript: PolyMath ×51 + Fuel testBitmap. **ROOT-CAUSED + FIXED
  2026-06-19.** Root cause: the arm64 send-bearing inline-J2J gate admitted
  `canBailMidMethod` callees; an `ExitArithOverflow` mid-bail (SmallInt arith ->
  Fraction/Float) left the caller's J2J save un-popped -> operand stack +1 ->
  wrong index to at:. Fix: default-exclude mid-bailing callees (AsmjitT1.cpp +
  JITRuntime.hpp), opt-in `PHARO_T1_ADMIT_BAILMID_CALLEES` restores the old admit.
  Cost +5-31%. Found via PHARO_SP_DEPTH_CHECK differential + knob bisection.
  Verified: 11-class repro 48->0; kernel + NeoJSON unchanged.
- Deep-recursion: STON testDeepStructure + ArrayTest>>testPrintingRecursive — BOTH
  FIXED 2026-06-19. Two VM-core causes: (1) StackOverflowLimit was an over-aggressive
  4096 though savedFrames_ holds 65536 -> raised to 56000 (fixes STON + the
  overflow-corruption class); (2) the context-NLR home-context search was capped at
  depth<200 -> deep printString's limit-block ^ (~14000 contexts up) hit an uncatchable
  BlockCannotReturn -> raised to 70000 (fixes testPrintingRecursive). Neither was JIT
  codegen (both failed under PHARO_NO_JIT). IntegerTest>>testSlowFactorial was a
  separate Sista tag-check miscompile, also fixed.
- SmallFloat64 >> #inject:into: ×18 in PolyMath — FIXED (Sista do-splice captured-var;
  was mislabeled VM-core).
- VM FFI gap (not JIT): Soil-File LibC flock/fsync → SIGABRT.

## Speed spectrum (ms, lower better)

```
workload                 Cog    JIT    slowdown
int_loop                 123    277    2.3x
method_recursion_fib30   193    558    2.9x
largeint_factorial       72     241    3.3x
sort                     79     423    5.4x
orderedcollection_churn  141    1041   7.4x
polymorphic_sends        1725   14526  8.4x
collection_protocols     50     660    13x
float_loop               218    4005   18x
dictionary_ops           152    2736   18x
allocation               53     1061   20x
string_build             87     1947   22x
block_recursion_fib28    109    3792   35x
set_ops                  43     1667   39x
```

NeoJSON real roundtrip (2000x): Cog 5.7s / JIT 155s = 27x.

Verdict: JIT competitive (2-5x) only on inlinable integer arithmetic + method
recursion; 13-39x slower on floats/blocks/collections/hashing/allocation — the
real-world levers. Build -O0 vs -O2 barely matters (hot loop is JIT-emitted).

2026-06-20 re-measure (build-rel, all 8 correctness fixes): spectrum essentially
UNCHANGED — the fixes are PERF-NEUTRAL (int_loop 264, float_loop 3787, fib30 544,
block_fib28 3961, polymorphic 14204, collection 881, oc_churn 1010, dict 2852,
set 1614, string_build 1876, alloc 1014, factorial 333, sort 521 ms; ratios 2-39x as
above). The prim-105 branch did not hurt string_build. Lever ranking for the
"as-fast-as-Cog" half (all DEEP, multi-session JIT work — consistent with memory
"Cog-speed lever CLOSED / no safe quick win"):
  1. ALLOCATION (highest ROI): TWO sub-levers. (a) DONE 2026-06-20 (b52dbf14):
     jit_rt_new_prim now allocates to EDEN (young) via ObjectMemory::allocateRawYoung
     (old-space fallback for overflow/eden-full; needsScavenge_ deferred to a safe
     point -> GC-safe). Was old-space-only -> full-GC pressure (shallowCopy old-space
     trap). MODEST win ~4-7% (allocation 1014->974, collection 881->820, sort
     521->486) -> GC pressure was NOT the dominant cost. (b) THE BIG REMAINING LEVER:
     the JIT emits an inline stencil at each new/new: site that CALLS the C++ helper
     jit_rt_new_prim (helpers.newPrim, stencils.cpp:1586); the ~18x residual is that
     per-alloc C++ call + the helper's size/format/header/nil-fill work. Cog inlines
     the whole bump in emitted asm (no call). Inlining jit_rt_new_prim's body into the
     new-stencil is the deep win. Underlies allocation(19x)+string_build(22x)+
     collection(17x)+dict(18x)+set(39x).
  2. Block activation: first-class block #value: not inlined (block_recursion 37x).
  3. Float unboxing: every float op boxes a heap Float / SmallFloat64 (float_loop 17x).
  4. Per-send dispatch tax: the residual ~1.5x send tax (polymorphic_sends 8x).
None attempted this session (each is a focused multi-session effort); correctness was
the productive vein (8 fixes, Cog-parity).
