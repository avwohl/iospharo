# T1 J2J Memory-Op Reduction — Design

**Status:** Design-only; not implemented in session 23.

## Goal

Reduce the ~42 memory ops per J2J send on send-heavy workloads. Send-heavy
AWFY (Bounce/Permute/Queens) runs ~2x slower than interpreter on T1
because save/restore dominates.

## Candidate: drop `literals`, `argCount`, `bcStart` from J2JSave

These three fields are all derivable from `save.jitMethod`:

- `literals = save.jitMethod->compiledMethodOop + 8`
- `argCount = save.jitMethod->argCount` (cached uint8_t at offset 34)
- `bcStart = save.jitMethod->compiledMethodOop + (2 + numLits) * 8`
  where `numLits = save.jitMethod->methodHeader & 0x7FFF`

**Expected savings per J2J round-trip:**

- Save side: −3 stores (literals, argCount, bcStart)
- Restore side: +0 loads (reuse compiledMethod cached load for literals
  and bcStart; argCount load is one byte, effectively free)

Net: ~3 memory ops saved per send. ~7% of the 42-op baseline.

Struct size shrinks from 72 bytes to 56 bytes (one cache line).

## Files touched (5)

1. `src/vm/Interpreter.hpp` — J2JSave struct
2. `src/vm/jit/stencils/stencils.cpp` — J2JSave mirror + `J2J_INLINE_RETURN` macro + save sites (two)
3. `src/vm/jit/TrampolineAsm.S` — offset defines, save/restore paths
4. `src/vm/Interpreter.cpp` — fallback paths that materialize SavedFrames from J2JSave
5. `scripts/extract_stencils.py` + regenerate `generated_stencils.hpp`

## Gotchas

- The self-recursive marker (low bit of `save.jitMethod`) becomes
  OBSOLETE — it only existed to skip literals/argCount/bcStart writes
  for same-method sends. Remove the marker entirely; always derive.
- `Interpreter.cpp` has 5+ sites that read `save.argCount` /
  `save.literals` / `save.bcStart` directly (materialization paths).
  All need to be replaced with jitMethod-based derivation.
- The ASM trampoline loads these fields with scheduling paired LDR/LDP;
  removing them lets the compiler pack the remaining fields more
  tightly.

## Estimated effort

Probably 4-6 hours to implement and verify on AWFY. The J2J path is
subtle — any mistake can silently produce wrong values on send returns.
Test iterations need full AWFY runs to catch regressions.

## Why deferred in session 23

ROI is real but marginal (~7% on send-heavy). The larger lever — T2
chain-loop continuation — has bigger upside (restores T2 send
performance from "same as T1" to potentially 2-3x T1 via intra-method
fast path). Session 23's focus should shift to T2 chain-loop or to
deferred test-suite issues.
