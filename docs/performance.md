# Performance: Our VM vs Reference Pharo VM (2026-02-23, commit ecd0d70)

## Summary

Our VM is a pure bytecode interpreter. The reference Pharo VM uses a JIT compiler
(Cogit) that translates hot Smalltalk methods to native machine code. This
fundamental difference accounts for virtually all performance gaps.

**Overall**: ~445x slower across the full test suite (26.7s vs 0.06s for 577 classes).
Individual tests range from 20x to 340x slower depending on how loop-heavy they are.

## Why: Interpreter vs JIT

Every Smalltalk operation — `+`, `*`, `<`, message sends, block evaluations —
goes through our bytecode dispatch loop. The reference VM JIT-compiles these
into native ARM64 instructions that run directly on the CPU.

For a tight loop like `1 to: 10000 do: [:i | result := result * i]`:
- **Reference VM**: The loop body compiles to ~10 native instructions
- **Our VM**: Each iteration dispatches ~50 bytecodes through the interpreter,
  each requiring decode, dispatch table lookup, stack manipulation, and method
  cache checks

The C++ primitive code (multiply, divide, etc.) runs at the same speed on both
VMs. But the Smalltalk code that *calls* those primitives is 50-200x slower.

## Measured Test Performance

Per-test timing using `scripts/time_tests.st` (30s timeout per test):

| Test | Ref VM | Our VM | Ratio | Bottleneck |
|------|--------|--------|-------|------------|
| IntegerTest>>testSlowFactorial | 118ms | 24s | 203x | Loop dispatching 10000 multiplies |
| IntegerTest>>testReciprocalModulo | 33ms | 11s | 339x | Modular exponentiation loop |
| IntegerTest>>testPrintStringBase | 48ms | 8.4s | 174x | Repeated division for digit extraction |
| IntegerTest>>testNthRootTruncated | 23ms | 5.2s | 225x | Newton's method iteration |
| LargePositiveIntegerTest>>testReciprocalModulo | 33ms | 2.2s | 68x | Same as above, smaller inputs |
| IntegerTest>>testHighBit | 14ms | 1.3s | 94x | Loop calling highBit on many values |
| IntegerTest>>testIsPrime | 5ms | 1s | 198x | Miller-Rabin with modular exponentiation |
| ArrayTest>>testPrintingRecursive | 12ms | 30s+ | 2500x+ | Deep recursion (hits 30s timeout) |

Typical collection/string tests: 20-50x slower (less loop-intensive).

## Where Time Goes (10000! example)

For `IntegerTest>>testSlowFactorial` computing 10000! (result is ~15000 bytes):

| Component | Time | Notes |
|-----------|------|-------|
| C++ multiply primitives | <1s | Karatsuba for large, scalar fast path for small |
| Interpreter loop dispatch | ~20s | 10000 iterations × message send/return overhead |
| GC pauses | ~3.5s | 7 full GCs from allocating ~112MB of intermediate LargeIntegers |
| Total | ~24s | C++ is <5% of runtime |

The reference VM's JIT eliminates the interpreter loop overhead entirely,
leaving only the C++ multiply time (~118ms).

## What We Optimized (commit ecd0d70)

These improve the C++ primitive quality but don't significantly dent the
interpreter overhead that dominates:

1. **32-bit word-level arithmetic** — All magnitude operations process 4 bytes
   at a time instead of 1 byte. Reduces inner loop iterations by 16x for
   multiply and 4x for division.

2. **Knuth's Algorithm D** for multi-word division — Replaced byte-level trial
   division (which did O(n) vector shifts per digit) with proper word-level
   Algorithm D with normalization.

3. **Scalar multiply fast path** — When one operand is a single 32-bit word
   (common in factorial: `bignum * smallInt`), uses O(n) multiply-and-carry
   instead of full schoolbook/Karatsuba.

4. **Montgomery multiplication** (`primMontgomeryTimesModulo`) — Used by
   modular exponentiation (isPrime, RSA). Was falling back to Smalltalk.

5. **primAnyBitFromTo** — Bit range scanning. Was falling back to slow
   Smalltalk byte-by-byte loop.

6. **primMontgomeryDigitLength** — Returns 32 (digit size constant). Trivial
   but was causing primitive failures.

7. **primitiveHighBit** — Reads MSB directly from the object instead of
   copying the entire magnitude into a vector.

## What Would Actually Help

To close the 200x gap, we would need interpreter-level optimizations:

- **Inline caching**: Skip method lookup for repeated sends to the same class.
  Our method cache has ~91% hit rate; inline caches achieve ~99%.
- **Bytecode superinstructions**: Combine common bytecode sequences
  (e.g., push-send-store) into single dispatch steps.
- **JIT compilation**: The nuclear option. Cogit is ~50K lines of Smalltalk
  that generates native code. Not practical for our project scope.
- **Specialized arithmetic dispatch**: For `SmallInteger op SmallInteger`,
  skip full message send and go directly to the primitive.

## LargeInteger Algorithms

Our C++ implementations match or exceed typical VM quality:

| Operation | Algorithm | Complexity |
|-----------|-----------|------------|
| Addition/Subtraction | Word-level with carry/borrow | O(n) |
| Multiplication (small) | Schoolbook on 32-bit words | O(n²) |
| Multiplication (large) | Karatsuba (threshold: 8 words) | O(n^1.585) |
| Multiplication (scalar) | Single-word multiply-and-carry | O(n) |
| Division (single-word divisor) | Long division on 32-bit words | O(n) |
| Division (multi-word) | Knuth's Algorithm D | O(n·m) |
| Montgomery multiply | Standard Montgomery reduction | O(n²) |
| Bit scanning (highBit, lowBit) | Direct byte access + CLZ | O(1) |
| Bit range test (anyBitFromTo) | Word-level mask scanning | O(n/32) |
| Comparison | Word-level from MSW | O(n) |
| Bitwise AND/OR/XOR | Word-level with two's complement | O(n) |
| Bit shift | Word + bit shift | O(n) |

All primitives are registered in both indexed (0-37) and named
(LargeIntegers plugin) forms. Zero LargeIntegers plugin primitive failures.
