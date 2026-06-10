# Patched monomorphic send sites — design

Status: empirical-inputs section final (2026-06-10); design section
pending the multi-agent design synthesis (workflow wf_21df75cc-6cc).

Goal: collapse the per-send cost of the monomorphic case toward Cog's
patched-immediate class check + direct branch.  This is the primary
remaining structural lever for the ~3.5x microbench gap vs Cog
(cfib 29 vs 8 ms, sfib 41 vs 12, benchFib x10 ~87 vs 25).

## Empirical inputs (all measured 2026-06-10, build-opt -O2, M-series)

### 1. Where cfib's time does NOT go (false leads, each killed by measurement)

- NOT C++ round trips: bare-startup baseline subtraction shows the
  cfib loop adds ~zero C++ IC-miss lookups (the 1.73M lookups incl.
  689K noICData are all image startup).
- NOT trampoline-asm hops: jitJ2JStencilCalls_ counts the INLINE
  dispatch-A J2J calls themselves (x20 dual depth+totalCalls bump in
  the inline sequence); the loop's sends never leave emitted code.
- NOT branch-prediction (RAS defeat): scripts/rasbench shows br-call
  + side-stack-save + indirect-br-return runs at 0.98-1.13x of
  bl/ret for the exact fib-shaped pattern.  The M-series indirect
  predictor handles it.  Do NOT redesign J2J control flow for RAS.
- NOT the icBuffer probe loads/RMWs in isolation: simulating the
  3-dependent-load probe + cursor/depth RMWs in the rasbench
  skeleton adds only ~0.3 ns/call.

### 2. Where it DOES go (cfib disassembly, PHARO_T1_DUMP_SEL + capstone)

Loop shape: 7.1M sends/run, 100% IC-hit, split 4.75M dispatch-A
inline-J2J (two cfib self-recursive sites) + 2.38M xmethod
inline-J2J (incc).  Scale anchor: the bare control-flow+saves
skeleton of this workload is 4 ms; Cog runs the full benchmark in
8 ms; we take ~29 ms.

The xmethod send sequence is ~90 instructions with 10-15 dependent
loads spread over 4+ cache lines:

    icDataPtr 2-load chain (state -> JITMethod -> icBuffer)
    receiver tag tests + header load + classIndex extract
    IC key load + compare              (probe proper)
    extras load + cbz + primKind extract + 2x tbnz dispatch
    calleeJM derive (sub from entryAddr)
    EIGHT JITMethod-header gate loads: isXmethod byte, numIC ldrh
      cmp, canBailMidMethod, hasNLR-ish, prim bytes, saveless byte —
      re-validating per SEND what is CONSTANT after callee compile
    ~25-insn machine-stack stash/unstash of caller state around blr
    retro-save push + entryDepth save/restore on return
    V2 return path: ~20 insns (depth check, save unpack, pop, br)

The per-bytecode (non-send) tax is a separate lever (naive stack
machine, no TOS register caching; see WIP.md 2026-06-11d "simStack")
— out of scope here, sequenced after patched ICs.

### 3. Cog reference behavior (what "patched" means)

At ~/src/pharo-vm: a monomorphic send site after first link is

    MoveR  ReceiverResultReg, ClassReg   ; (or inline class fetch)
    CmpCq  <expectedClassTag>, ClassReg  ; class IMMEDIATE in code
    JumpNonZero <linkStub>               ; miss -> relink
    Call   <calleeEntryAfterTypeCheck>   ; DIRECT call, patched

Zero loads beyond the receiver header.  All callee-suitability
decisions were made ONCE, at link/patch time.  Polymorphic sites
upgrade to a closed PIC (jump table), megamorphic to a hash probe.

### 4. Constraints this VM adds (vs Cog)

- ASLR/low-bit oop encoding: class IMMEDIATES in code are
  classIndex values (22 bits, stable across GC), not pointers —
  patched compares must use classIndex, same as the IC key today.
- W^X (MAP_JIT): patching code requires the per-thread write flip;
  IC fills today go to the heap-side icBuffer precisely to avoid
  this.  A patched-site design must batch/amortize W^X flips
  (e.g. patch queue drained at safepoints) or accept the flip cost
  on first-link only (it IS once per site per target).
- GC: moving compiledMethodOop/selector literals is already handled;
  patched DIRECT branch targets point into the code zone, which
  does not move (methods are freed, never moved) — but eviction of
  the CALLEE must unlink all sites pointing at it (Cog: scan sites /
  generation epoch; we already have rebuildMethodMap + eviction
  pinning infrastructure).
- findMethodByPC is now O(log n) (pcIndex_) — anything the design
  puts on the send path must stay O(1)/O(log n) in zone size
  (memory: jit-findmethodbypc-linear-walk-trap).

### 5. Intermediate lever the design should rank (no code patching needed)

Fold the constant-after-compile callee gate bits (isXmethod-OK:
numIC<=cap && !canBailMid && !hasNLR && prim-OK && saveless-OK) into
ONE bit of the IC extras word at IC-fill/upgrade time
(upgradeICToJ2J / patchJITICAfterSend already write extras).  The
8-load gate cascade then collapses to one tbnz on the
already-loaded extras register.  Smaller win than full patching
(probe loads remain) but: heap-side write (no W^X), no new
invalidation surface beyond existing IC-clear paths, and the same
precomputed predicate later feeds the real patcher.

## Design (synthesis pending)

(to be filled from the design workflow output)
