# Sista IC-promotion plan — multi-session work

## Problem

Sista's `tryInlineConstReturn` (SistaBuilder.cpp:5462) can inline getters
into a hot loop using IC hints from `extractInlineHintsForMethod`
(Interpreter.cpp:18745).  But hints require `jitRuntime_.methodMap().
lookup(method)` → non-null.  The method must be **JIT-compiled** for
hints to exist.

For benches like `1M getter+yourself`:
```smalltalk
[| pt | pt := 1 @ 2. 1 to: 1000000 do: [:i | pt x]] value
```
The outer block is called ONCE (by `time:`).  Even at threshold=1,
the block isn't JIT-compiled before its single execution starts.
The 1M `pt x` sends run in interp → no IC fills → no hints when
Sista's per-bytecode lifter sees the loop body → `kSendUnspeculated`
→ `sendNoSplice` Sista bail → 33ms wall.

## Fix shape

Maintain an **interp-side IC table** parallel to T1's IC zone, populated
by interp's send dispatch.  Hints get extracted from this table when
the method has no JITMethod.

## Implementation steps (each ≈ 1 focused session)

### Session A — Data structure + populate

1. Add to `Interpreter`:
   ```cpp
   struct InterpHintEntry {
       uint16_t bcOff;     // caller's bytecode offset for the send
       uint32_t classKey;  // receiver class index (low 22 bits) | 0x80000000 if SmI
       uint64_t targetMethod;  // resolved method oop
       uint32_t hits;      // count for monomorphism check
   };
   std::unordered_map<uint64_t, std::vector<InterpHintEntry>> interpHints_;
   ```
   Keyed by caller method oop's rawBits.

2. Hook into `cacheMethod` or a strategic point in `sendSelector`:
   - Compute `callerBcOff = instructionPointer_ - bcBase`.
   - Compute `classKey` from receiver.
   - Find or insert entry in `interpHints_[method_.rawBits()]`.
   - Increment hits on match; replace on different class (monomorphic
     latch — multi-class sites bail to no-hint).

3. GC awareness: clear `interpHints_` in `recoverAfterGC` (similar to
   existing IC clears).  V1 can be lazy — accept staleness as
   degraded but not wrong (worst case: Sista bails on bad hint).

### Session B — Plumb into hint extraction

1. In `extractInlineHintsForMethod`:
   - If `jm == nullptr || jm->numICEntries == 0`: fall back to
     `interpHints_[method.rawBits()]`.
   - For each `InterpHintEntry` with hits >= K (start with K=5),
     emit a `sista::InlineHint{bcOff, classKey, targetMethod}`.

2. Test: with `PHARO_PROFILE=1`, verify hints emitted for the bench's
   outer block's `x` send.

### Session C — Validate end-to-end

1. Run bench-suite: expect `1M getter+yourself` to drop from 33ms.
2. Stability runs (5×): confirm bimodal doesn't reappear.
3. Sista bail stats: `g_sistaBail_sendNoSplice` should drop for blocks
   that previously had no IC hints.

## Risks

- Memory: per-method per-bcOff entries.  For 1500 compiled methods
  with ~10 send sites each = 15K entries × ~24 bytes = ~360 KB.
  Acceptable.
- Send hot path overhead: hash lookup per send is ~5-10ns.  For
  bench-suite ~5M sends → ~25ms overhead.  Probably net positive
  for benches that benefit from inlining but worth measuring.
- GC: stale entries point at moved methods.  Sista should validate
  the hint's `targetMethod` before using.

## Files to touch

- `src/vm/Interpreter.hpp` — add field
- `src/vm/Interpreter.cpp` — add struct, record fn, plumb into
  cacheMethod or sendSelector site, update extractInlineHintsForMethod
- `src/vm/jit/sista/SistaBuilder.cpp` — possibly tighten hint
  validation
- `docs/sista-ic-promotion-plan.md` — this file; update with outcomes

## Status — 2026-05-25

Survey + design done.  Implementation deferred (architectural).
