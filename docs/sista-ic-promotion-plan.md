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

Sessions A+B+C+D+E shipped.  Session E plumbed three off-by-one
bugs preventing the bench's `pt x` send from being inlined:

1. **cacheMethod recorded post-advance IP as the send bcOff.**
   The bytecode dispatcher post-increments IP via
   `bytecode = *instructionPointer_++`, so by the time cacheMethod
   fires for a 1-byte send (special send 0x70-0x7F or send-literal
   0x80-0xDF), `instructionPointer_ - bcBase` is at the byte AFTER
   the send.  T1's IC table records the bcOff of the SEND byte
   itself (from JITCompiler.cpp:2596 `siteOffsets.push_back(bc.bcOffset)`).
   TICR / IC matchers expect bcOff to point AT the send byte.  Fix:
   record `instructionPointer_ - 1 - bcBase` in cacheMethod.

2. **Per-bc Sista lift used local bcOffsets; hints use absolute.**
   `Builder::buildFromOffset(startBcOffset > 0)` shifts the lifter's
   bytecode pointer so `ip` starts at 0 within the lifted region.
   The lifter then queries `h.bcOffset == bcOffset` (local) but
   hints carry method-absolute bcOffsets.  Fix: in buildFromOffset,
   build a shifted copy of the hints vector and use that for the
   lift; drop hints that fall before startBcOffset.

3. **Sista's tryInlineConstReturn (TICR) was wired only into the
   Send0/Send1/Send2 literal-send path.**  SpecialSend ops
   (0x70-0x7F) include `#x`, `#y`, `#value`, `#class`, etc.  These
   went straight to kSendUnspeculated, terminating the lift at the
   first SpecialSend.  Fix: add the same TICR call (then continue
   the lift on success) into the SpecialSend handler at
   SistaBuilder.cpp:~4562.

**Visible-bench outcome**: 1M getter+yourself unchanged at 32-33ms.
Counter dump:
```
[SISTA-INLINE] sends-lifted=4459 hints-provided=2027 hints-consumed=529
              callees-attempted=485 callees-lifted=476 callee-values=1854
              inlines-emitted=43
```
The bench's `pt x` IS inlined now (TICR-EMIT fires once for Point
class 0x36), AND the bench block's Sista compile is dispatched.
But the dispatched fn appears to run the loop once then blacklist
(kBcDispatchBlacklistThreshold=1) on the natural BlockReturnTop
bail (bailDistance=16 < 20 → marked as "close to entry").  Subsequent
1M-iter benches go entirely through interp.

**Blacklist fix landed** (Session E): added BlockReturnTop/Nil
detection in the dispatch-bail accounting so natural end-of-block
bails don't blacklist.  Bench-suite stable; bench-suite mix
unchanged.

**Open question after Session E**: even with TICR firing and the
dispatch surviving, the 1M getter+yourself bench remains at 33ms
(vs Cog 1ms — 33× gap).  The per-bc Sista compile dispatches once,
runs ~999K iters in compiled code, bails naturally at BlockReturnTop
— but compiled-code throughput appears to be at parity with interp
(~30ns/iter).  Expected throughput for the loop body
(kPrimTagCheckInt + kPrimLeInt + kBranch + kGuardClass +
kLoadInstVar + kPrimAddInt + kStoreTemp + kBranch back) is
~25-35 cycles = 8-12ns at 3GHz, predicting ~8-12ms total.  Gap
suggests the lowered code carries per-iter overhead (per-iter
kGuardClass that could be hoisted out of the loop, redundant tag
checks on back-edge loads, or branch-mispredict cost on the
deopt branches).  Disassembly + tightening landed in a follow-up
session focused on Sista lowering, not the IC-promotion path.
