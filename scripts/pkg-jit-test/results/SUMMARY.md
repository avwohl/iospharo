# Package JIT test results — 2026-06-19

Custom JIT VM (`build-rel`, arm64, -O2, JIT default-on) vs stock Cog (Pharo 13.1),
same saved image per package. Loaded via stock Cog Metacello.

## Correctness

```
package    classes  Cog P/F/E      JIT P/F/E/T       JIT-only failures
NeoJSON    11       116/0/0        116/0/0           0   CLEAN PARITY
NeoCSV     ~3       66/0/0         66/0/0            0   CLEAN PARITY
STON       11       317/0/0        316/0/1           1   testDeepStructure (deep-recursion)
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

## Bugs found (JIT-confirmed = fails JIT, passes PHARO_NO_JIT)

- JIT off-by-one subscript: PolyMath ×51 + Fuel testBitmap. **ROOT-CAUSED + FIXED
  2026-06-19.** Root cause: the arm64 send-bearing inline-J2J gate admitted
  `canBailMidMethod` callees; an `ExitArithOverflow` mid-bail (SmallInt arith ->
  Fraction/Float) left the caller's J2J save un-popped -> operand stack +1 ->
  wrong index to at:. Fix: default-exclude mid-bailing callees (AsmjitT1.cpp +
  JITRuntime.hpp), opt-in `PHARO_T1_ADMIT_BAILMID_CALLEES` restores the old admit.
  Cost +5-31%. Found via PHARO_SP_DEPTH_CHECK differential + knob bisection.
  Verified: 11-class repro 48->0; kernel + NeoJSON unchanged.
- JIT deep-recursion operand corruption: STON testDeepStructure (1024-deep).
  STILL OPEN — distinct from the above (with the fix the error changes signature
  `True>>#\\` -> `KeyNotFound: key 0`). Likely same class as the pre-existing
  kernel failures ArrayTest>>testPrintingRecursive / IntegerTest>>testSlowFactorial.
- VM-core (not JIT): SmallFloat64 >> #inject:into: ×18 in PolyMath (interp fails too,
  Cog passes).
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
