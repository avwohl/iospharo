# T2 Inline IC Implementation Plan

**Status:** open — task #31.  Prerequisite for T2 to beat T1 on
send-heavy benchmarks.

## Problem

The asmjit-based T2 MVP (as of 2026-04-17) handles only leaf methods —
no sends.  Attempts to compile send-containing methods by emitting
"push args + set state + ExitSend + return" delivered a 1.8× regression
on the array-fill benchmark.  The round-trip to the interpreter is
strictly slower than T1 staying in native code with inline IC check +
direct J2J call on hit.

For T2 to be a perf win on any send-containing method, it must do the
IC check *inline* — match the receiver class against cached entries and
take a fast path without leaving native code on the common case.

## Reference implementation

`src/vm/jit/stencils/stencils.cpp:stencil_sendJ2J` is the T1 reference.
Key points:

- IC data layout: 6 entries × 3 uint64_t — `(lookupKey, methodBits, extra)`
  — followed by `icData[18]` = selectorBits for megacache fallback.
- `lookupKey` is `classIndex` for objects (tag == 0) or
  `tag | 0x80000000` for immediates.
- On hit, entries with `extra != 0` can inline getter/setter/returnsSelf
  (bits 63/62/61 of extra).
- If `extra == 0`, set `state.cachedTarget = methodBits`, exit
  `ExitSendCached` — the trampoline in Interpreter.cpp converts to
  `ExitJ2JCall` when the target is also JIT-compiled, avoiding a C
  bounce.

## Proposed asmjit emission

For each send in a T2 method, emit:

    ; 1. Load receiver from state.sp[-(argCount+1)*8]
    ldr sp, [state, OFF_SP]
    ldr recv, [sp, #-(argCount+1)*8]

    ; 2. Compute lookupKey
    ;    tag = recv & 7
    ;    if tag == 0 && recv != 0:  lookupKey = (class.header >> CLASS_SHIFT) & CLASS_MASK
    ;    else:                      lookupKey = tag | 0x80000000
    and tag, recv, #7
    cbz tag, heap_path
    orr lookupKey, tag, #0x80000000
    b check

    heap_path:
    cbz recv, null_bail              ; nil receiver — safe fallback
    ldr header, [recv]               ; ObjectHeader first word
    lsr lookupKey, header, #CLASS_SHIFT
    and lookupKey, lookupKey, #CLASS_MASK

    check:
    ; 3. Six-way IC probe
    ;    icData pointer baked as immediate (64-bit movz/movk sequence)
    movz icData, #IC_LOW, lsl #0
    movk icData, #IC_MID, lsl #16
    movk icData, #IC_HI,  lsl #32
    movk icData, #IC_TOP, lsl #48

    ldr  key0, [icData, #0]
    cmp  lookupKey, key0
    b.eq hit0
    ldr  key1, [icData, #24]
    cmp  lookupKey, key1
    b.eq hit1
    ; ... keys 2..5 ...
    b    miss

    hit0:
    ldr  method, [icData, #8]
    str  method, [state, OFF_CACHEDTARGET]
    str  icData, [state, OFF_ICDATAPTR]
    mov  w_args, #argCount
    str  w_args, [state, OFF_SENDARGCOUNT]
    mov  w_exit, #ExitSendCached
    str  w_exit, [state, OFF_EXIT]
    ret

    ; ... hit1..hit5 similar with different method offsets ...

    miss:
    null_bail:
    str  icData, [state, OFF_ICDATAPTR]
    mov  w_args, #argCount
    str  w_args, [state, OFF_SENDARGCOUNT]
    mov  w_exit, #ExitSend
    str  w_exit, [state, OFF_EXIT]
    ret

## IC data allocation

- Size: 6 × 24 + 8 = 152 bytes per send site.
- Allocate one per send in the T2-compiled method.
- Store alongside the Tier2Compiler, keyed by the compiled function
  pointer: `std::unordered_map<void*, std::vector<uint8_t[152]>>`.
- At compile time, the icData pointer is baked into the code as an
  immediate (4-instruction movz/movk sequence for 64-bit address).

### Lifetime + GC

- The IC data buffer is C heap, outlives GC moves.
- BUT `methodBits` stored in IC data points to CompiledMethod Oops
  that CAN move.  Existing machinery: `JITRuntime::recoverAfterGC`
  flushes T1 ICs; need to extend it to also flush T2 ICs.
- Walk the Tier2Compiler's IC buffer list on recoverAfterGC, zero
  out all entries.  This mirrors the T1 IC scan.

### Free on method replacement

- When a method is recompiled (oldVersion replacement), free its T2
  IC buffers.

## Scope for the first patch

1. **One send per T2 method**: only compile methods with exactly one
   send.  Multiple sends adds complexity (per-send IC data, jump
   labels).  Already most leaf+send methods match this.
2. **6-entry IC**: match T1 exactly for correctness + compatibility
   with existing pendingICPatch_ flow.
3. **No inline getter/setter on J2J**: `extra == 0` path only, setting
   cachedTarget + ExitSendCached.  Adding the inline getter/setter
   path (extra bit 63/62/61) is a follow-up once the baseline is in.
4. **Object + immediate receivers**: handle both paths (tag==0 →
   classIndex, else tag | 0x80000000).

## Success criterion

Benchmark: array-fill loop (10 000 × (1 to: 1 000)) should be no
slower than T2=0 baseline.  Stretch: ~5-10% faster because T2 code
is tighter than T1's stencil chain.

## Estimate

Likely 2-3 sessions:

- Session A: implement the inline IC probe for one send + single-arg
  send methods; verify correctness end-to-end; measure perf.
- Session B: extend to multi-send methods (requires per-send labels +
  state advancement); add inline getter/setter via `extra` bits.
- Session C: GC hook, IC flushing, stress-test.
