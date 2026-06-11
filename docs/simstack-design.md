# T1 TOS register cache — definitive design (arm64)

Status: APPROVED FOR IMPLEMENTATION (synthesis of three candidate designs +
adversarial reviews, 2026-06-10). Every reviewer finding is resolved in §10
or listed as an open question with a probe in §11.

## 1. Decision: write-through TOS cache in x26, with targeted grafts

Skeleton chosen: the **minimal-risk write-through design**. Grafted from the
others: per-term mask-knob bisection and the cmp+b.cond fusion row
(perf-first), the constant-SmI single-check shrink, register-poison debug
knob, the emitted x26-vs-memory VERIFY trap, shared prescan/emit decode, and
gate hygiene (cog-faithful). The two rejected skeletons and why:

- **Cog-faithful deferred simStack** (pool x8-x13, deferred entries,
  bail-materialization stubs): its review found the pool premise false
  (inline ByteString `=` — an isPhase3ArithOp — hard-codes x8-x15 outside
  any flush), and the architecture re-creates every contract hazard
  (GC-invisible register Oops, bcToCode zeroing, per-bail stubs with
  code-zone cost) to win instruction *count*, which this VM's measurement
  history says is free anyway. Its own review priced cfib at 3-8%. Not
  worth the blocker-#4-class risk surface.
- **Peephole fusion with inline slow copies**: sound trick, but two
  blockers (unbound interior labels → the exact 2026-05-28 garbage-bcToCode
  failure class; ExtendB fusion → unpreemptible JIT loops) and ~1.5x local
  code growth. Its genuinely novel rows are grafted here instead: the
  cmp+b.cond fusion (suite #1 pair, 5.3M/100M bc) lands as B4 on top of the
  cache, and the spec==/ifNil elision is an open question (§11.3) because it
  requires siteIdx accounting (fusion review F-4).

### The core property that buys all the safety

The cache is a **write-through TOS mirror**: when the emitter marks it
valid, x26 holds a copy of the value at `[x25, #-8]` and **memory is ALWAYS
exact** — every push still stores, every pop still adjusts x25 exactly as
today. Consumers read x26 instead of loading; producers write x26 in
addition to storing. Consequences, each load-bearing:

- x26 is never the only copy of an Oop → **zero new GC exposure**. No
  flush code is ever emitted; "flush" is emit-time bookkeeping costing zero
  instructions.
- Every existing contract holds unmodified: GC root scan
  (`[stackBase_, stackPointer_)`), spDepthCheck / spDepthCheckJ2JSave,
  materializeJ2JSaveIntoFrame's `save.sp - (nArgs+1)` retSlot,
  EXIT_ARITH_OVERFLOW re-execute, EXIT_MUST_BOOL value-left-on-stack, PMS
  head receiver-from-memory, J2J callee tempBase derivation. BcDepthMap.cpp
  is **unchanged by design invariant**.
- Every bcLabel remains a valid memory-canonical re-entry → no bcToCode
  zeroing, no entryFlushed[] machinery, no OSR resume-rate regression. A
  single `ldur x26, [x25, #-8]` in each C++→JIT entry stub arms the cache
  at ANY entry point, because write-through means memory IS the truth.

Deferred-push / write-back is rejected for all batches: the saved stores are
not on the critical path (they drain through the store buffer), so write-back
re-creates every hazard for a payoff the measurements say is ~zero.

## 2. Why this attacks latency, not count (the mechanism, anchored)

Measured anchors: cfib **24-25 ms** today; the hand-written send/return
skeleton runs **4 ms**, so ~20 ms of gap is the cross-method send-activation
machinery (~230 of ~310 executed insns/activation) which this design
deliberately does NOT touch — that is the other lever. This lever's
addressable share is the ~80 insns of bytecode tax and, more precisely, the
**~13 store→load round trips through `[x25, #-8]` per activation, ~10 of
them within ≤5 insns** of their producer.

Why round trips and not instruction count:

- Each producer→consumer handoff through stack memory costs a store→load
  forwarding round trip (~4-5 cy on M-series) that sits ON the dependency
  chain: the loaded value feeds the next tag check, branch, or send
  receiver. Replacing the load with a register read removes those cycles
  from the chain. The still-emitted store becomes dead weight that retires
  off-path via the store buffer — effectively free on OoO.
- Precedents from this VM, both directions:
  - Count is free: V2 save-protocol -25% insns measured **0 ms**;
    tempBase-in-x26 (-25% insns) measured **net negative**; MAX_IC sweep
    flat.
  - Chains are real: sp-in-x25 (address-dependency kill) **42→29 ms**;
    counter-RMW removal (RMW chain kill) **29→24-25 ms**.

So the win model (§9) is denominated in killed round trips on dependency
chains, not in instruction deltas, and every term carries its own
within-binary A/B via a mask bit.

## 3. Representation

Emit-time only — no runtime data structure, no JITMethod field, no
bcEntryState change:

```cpp
// file-scope in AsmjitT1.cpp, reset at compile start
struct T1TosCache {
    bool    valid    = false;  // x26 == value at [x25,#-8]; memory exact regardless
    bool    constSmI = false;  // B3: x26 is a known tagged SmallInteger ...
    int64_t taggedBits = 0;    // ... with these bits (enables single-check shrink)
};
```

**Snapshot semantics (mandatory — resolves RM-F3):** at the top of every
bytecode emit:

```cpp
const T1TosCache in = tos;   // what the PREVIOUS bytecode left
tos = {};                    // default: invalidated
// converted families read `in`; producers set `tos` ONLY via paired
// helpers that emit the x26 write in the same call (resolves risk 2)
```

Jump-target invalidation applies at **label-bind time**, before the op's
emit reads the snapshot. Unconverted ops are therefore safe automatically;
omissions fail toward lost optimization, never corruption. The single
dangerous direction is valid-marked-but-x26-stale; memory can never be
stale.

Knobs (all in `src/vm/debug_vars.h`, parsed once at static init):

```
DEBUG_BOOL(PHARO_T1_TOS_REG)        opt-in during B0-B4; flip to opt-out
                                    PHARO_T1_NO_TOS_REG at B5 (inline-J2J precedent)
DEBUG_INT(PHARO_T1_TOS_MASK, -1)    bit per family (graft: FUSE_MASK pattern) —
                                    per-term within-binary A/B and miscompile bisect
DEBUG_BOOL(PHARO_T1_TOS_POISON)     graft (RC-F10): emit movz x26,#0xDEAD at every
                                    invalidation point; stale-valid consumers crash
                                    deterministically instead of heap-dependently
DEBUG_BOOL(PHARO_T1_TOS_VERIFY)     graft (cog-faithful's VERIFY knob, transposed):
                                    before EVERY cache-consuming emit, insert
                                    ldur xS,[x25,#-8]; cmp xS,x26; b.eq ok; brk.
                                    This is the ONLY deterministic net for the one
                                    corrupting direction (valid-marked-but-stale,
                                    e.g. a missed endOfSend-class merge): poison
                                    can't catch it because stale x26 holds plausible
                                    junk, not poison. Write-through makes the check
                                    always expressible — memory is the truth.
```

Knob-off emit is **byte-identical by construction** (all divergences guarded
by one per-compile `const bool useTos`, including register choice — knob-off
pushes keep loading into x1).

## 4. Register budget and ABI changes

```
x26   the cache register. Verified the one free callee-saved reg:
      - emitted code: only writers today are the dead PHARO_T1_TB_IN_X26=0
        helpers (AsmjitT1.cpp:72, :2257-2291). Add #error if that macro is
        ever set while TOS cache is compiled in — mutually exclusive.
      - TrampolineAsm.S: x25/x26 already saved at [sp,#48]; x26 marked FREE.
      - JIT_CALL / JIT_RESUME_CALL (JITState.hpp): x26 NOT in clobber lists
        today (verified by grep) -> MUST be added; emitted code now clobbers it.
      - pharo_jit_osr_resume (TrampolineAsm.S:548+): saves x19/x20/x25 but
        not x26 -> add str/ldr pair (frame slots #48/#56 free).
All other registers untouched: x21-x24/x27/x28 stay trampoline-pinned,
x1-x15 per-bytecode scratch semantics unchanged, PMS Lprobe contract
registers (x0,x1,x2,x4,x25) untouched.
```

**Re-establish loads** — add `ldur x26, [x25, #-8]` immediately after each
existing sp hoist in every C++→JIT entry path:

- `JIT_CALL` and `JIT_RESUME_CALL` macros (plus `"x26"` in both clobber lists)
- TrampolineAsm.S hoist sites: **:226, :292, :477** (dispatch-loop resume,
  sp-residency reload, nested-call path — the entry hoist at :150-169 loads
  x19/x20 only; corrected site list per minimal-review F4 note)
- `pharo_jit_osr_resume` (after its x25 reload)

This closes the OSR/chain-resume hole completely: entry at ANY bcLabel finds
x26 correct because memory is exact. The load is always in-bounds
(`sp >= tempBase = fp+1 > stackBase_`); at worst it reads bits the compiler
will never trust (compile-start state is invalid) — documented so nobody
"fixes" the garbage load. Cost: ~1 cy per C++→JIT transition, not per
bytecode; dead load when knob-off.

## 5. Validity policy at every contract point

**Invalidate (emit-time bookkeeping, 0 instructions emitted):**

```
point                          rule
compile start                  tos = {}
every bytecode dispatch head   snapshot-then-clear (§3); converted ops read the snapshot
every jump-target bcLabel      bitmap from the prescan; applied at label-BIND time
after every helper blr         structural: the ONE helper-call emit wrapper auto-
                               invalidates; CMake grep ratchet (envPresent-RATCHET
                               pattern) counts raw a.blr( outside the wrapper — count
                               must not grow (resolves RM-F7; 23 raw a.blr( sites in
                               AsmjitT1.cpp by grep today — the ratchet pins the
                               exact number, not this doc)
after every send               callee clobbers x26; re-established at endOfSend (below)
after Pop                      new TOS = old NOS, unknown (no NOS entry)
both arms of cond jumps        bool consumed; arms start cold
```

**Jump-target bitmap (resolves RM-F4 + RC-F3):** built in the SAME decode
routine the emit loop uses (no parallel reimplementation), covering short
jumps and the long ExtendB-prefixed forms. At every emitted jump, assert the
target was marked; any mismatch **hard-fails the compile** (return false →
interp fallback, negative-cached) — never a BUG-print-and-continue. Targets
not at decoded op-start boundaries also refuse compilation.

**Re-establish:**

```
site                           code                          note
producer ops                   x26 written + str (paired)    §6 table
resumeAfterCall continuation   mov x26, x1; falls through    arms the post-send consumer for
                               to endOfSend                  BOTH br-x8 and JIT_RESUME_CALL
                                                             arrivals (x1 = retval ABI)
endOfSend spec arrivals        Lrearm stub (below)           THE merge fix — resolves RM-F1
C++->JIT entry stubs           ldur x26, [x25, #-8]          §4; covers OSR/chain/tryResume
post-helper-blr (B3, A/B'd)    ldur x26, [x25, #-8]          replaces the KILLED "x26 is
                                                             callee-saved = free survival"
                                                             item — bits survive a blr but
                                                             a moving GC inside the helper
                                                             relocates the memory slot and
                                                             never visits x26 (RM-F2);
                                                             re-arming from memory is always
                                                             correct under write-through
```

**The endOfSend merge fix (RM-F1, BLOCKER in the original draft):**
`endOfSend` in the arm64 send block has ~27 predecessors — resumeAfterCall
plus every inline spec path (setter, returnsSelf/Literal, intCmp/intArith,
tryPrim*, multiSlot, ...). The spec paths write their result to memory only;
a single linear `valid` bool set by resumeAfterCall would carry valid=true
across ALL arrivals, leaving x26 = stale pre-send junk on spec hits → wrong
value silently consumed, or a send-head `mov x1, x26` computing an IC key
from the wrong object's class (the wild-receiver bug class). Fix, per site:

```
Lrearm:  ldur x26, [x25, #-8]     ; legal: write-through memory is exact here
         b    endOfSend
```

Every spec-path `b endOfSend` is retargeted to `b Lrearm` (+2 insns per
SITE, not per path); resumeAfterCall keeps `mov x26, x1` and falls through,
so the hot J2J-return path stays load-free. This fix is REQUIRED in the same
batch as any post-send consumer (B2) — the gates plausibly miss the bug
because the stale value must be both consumed and divergent from memory.

**Exits/bails: nothing to do.** `emitSyncSpToState` already precedes every
ret; memory is depth-complete, so spDepthCheck, GC scan, materialize,
MUST_BOOL, and re-execute bails see exactly today's state. No re-materialize
stubs exist anywhere in this design. Converted arith keeps today's bail
ordering: **type/overflow bails branch before the result store and before
any x25 adjustment** (carries the RC-F5 rule even though our pops were never
deferred).

**Arrival matrix for a bcLabel where the emitter believes valid=true:**

```
arrival path                        why x26 is correct
fall-through                        previous bytecode's paired producer wrote it
explicit jump                       impossible: targets force invalid at bind
chain-loop JIT_CALL / OSR           entry-stub ldur x26,[x25,#-8]
JIT_RESUME_CALL -> resumeAfterCall  mov x26, x1
J2J return br x8 -> resumeAfterCall same continuation
inline-spec hit -> endOfSend        Lrearm ldur
```

## 6. Per-family emit specs (knob-on, cache-valid; else today's shape)

Mask bit per row (PHARO_T1_TOS_MASK) for bisection and per-term A/B.

```
family             today (executed)                      new                                       delta / note
pushes: recvVar,   ldr x1,...; str x1,[x25],#8           load target = x26;                        0 insns; producer
 temp, const, lit,                                       str x26,[x25],#8; valid=true              now feeds the reg
 PushInteger,      (PushInteger/PushCharacter: the       mov-imm into x26 + str;                   RM-F5a fix: these
 PushCharacter      4-op multi-byte round trip)          constSmI=true for PushInteger (B3)        ARE B1 producers
Pop                sub x25,#8                            unchanged; valid=false                    0
Dup                ldur x1,[x25,-8]; str x1,[x25],#8     valid: str x26,[x25],#8 (stays valid);    -1; write-through
                                                         invalid: today's shape                    means Dup is safe
                                                                                                   in any position
arith (#+ #- #<    mov x2,x25; ldur x1,[x2,-16];         ldur x4,[x25,-16] (NOS only);             -2..-3; kills the
 family)           ldur x4,[x2,-8]; 5-insn dual SmI      SmI check vs x26; op; result -> x26;      TOS dependent load;
                   check; op; str/sub; mov               stur x26,[x25,-16]; sub x25,#8;           bails BEFORE result
                                                         valid=true                                store + sp adjust
arith, const side  (same)                                B3: single 2-3 insn SmI check on the      RC graft; imm forms
 (constSmI set)                                          unknown side; imm12-range-checked         range-checked (RF-6,
                                                         immediate op, else emitMovImm             asmjit silent-drop)
cond jumps         ldur x1,[x25,-8]; ldp true/false;     cmp against x26 (skip the ldur);          -1; kills the bool
                   cmps; sub+branch each arm             arms unchanged; valid=false both arms     round trip; MUST_BOOL
                                                                                                   bail unchanged (value
                                                                                                   in memory)
cmp + condJump     bool materialize; str; ldur; ldp;     B4 graft: on the inline-SmI fast path,    suite #1 pair
 fused (B4)        cmps; branch                          cmp + b.cond direct — no boolean          (5.3M/100M); rules:
                                                         materialization. x25 published (pops      naked FORWARD cond
                                                         applied) BEFORE the b.cond (RF-7).        jumps only (RF-2);
                                                         The jump bytecode KEEPS its full          jump must not itself
                                                         unfused emit at its bcLabel for the       be a jump target;
                                                         #< send-resume arrival (RF-1/RF-3)        labels all stay bound
uncond jumps       b label                               unchanged (nothing to flush)              0
send head, n=0     ldur x1,[x2,-8]                       mov x1, x26                               same length, kills the
                                                                                                   load heading the probe
                                                                                                   chain; W-offsets are
                                                                                                   label-derived (RF-5)
send head, n>0     ldur x1,[x2,-8*(n+1)]                 unchanged (receiver is not TOS)           args stay memory-
                                                                                                   resident: PMS contract
resumeAfterCall    sp -= nArgs; stur x1,[x2,-8]; ...     + mov x26, x1; valid=true; falls          +1 emitted/site; plus
                                                         through to endOfSend; spec paths          +2/site for Lrearm
                                                         retargeted to Lrearm (§5)
inline getter      stur x6,[x2,rcvrOff]                  + mov x26, x6 when result lands at TOS    +1, valid=true
popStoreTemp       sub; ldr x1,[x25]; ldr tb; str        sub x25,#8; ldr tb; str x26,[tb,n*8];     -1; valid=false
popStoreRecvVar    immutable chk; pop+ldr; str;          B3: value reg = x26 (skip the ldr);       -1; immutable bail
 (B3)              barrier                               bail still BEFORE the sp decrement        leaves memory intact
returnTop          4-insn pop into x1; prelude           sub x25,#8; mov x1,x26;                   -2; the mov happens
                                                         then the J2J prelude unchanged            before the prelude
                                                                                                   restores caller state
                                                                                                   (RC-F7 ordering note;
                                                                                                   trivially satisfied
                                                                                                   since x26 holds the
                                                                                                   value, not a deferred
                                                                                                   descriptor)
```

Everything else (Ext pushes/stores, remote temps, PushFullBlock, blocks,
ExtendA/B, sista bit-55 paths, tryPrim* interiors) — unconverted,
default-invalidated by the snapshot rule, byte-identical even knob-on.
**No NOS entry** (no free callee-saved reg; scratch conflicts with send-path
x4-x15) — open question §11.2.

## 7. cfib round-trip ledger (corrected per RM-F5)

Per recursive activation, ~310 executed insns, 13 round trips, ~10 tight:

```
bc  op             round trip INTO it                  fate (with B1+B2 incl. Lrearm)
3   inline #<      TOS literal-2 (PushInteger/lit)     KILLED (producer in B1 — RM-F5a fix)
3                  NOS self                            survives (no NOS entry)
4   jumpFalse      bool (3 insns prior)                KILLED (B1); B4 fuses the cmp+branch
9   inline #-      TOS 1                               KILLED
9                  NOS self                            survives
10  send cfib      receiver = bc9 result (probe head)  KILLED (0-arg mov)
13  inline #-      TOS                                 KILLED
13                 NOS self                            survives
14  send cfib      receiver (probe head)               KILLED
15  inline #+      TOS = site-2 resume store           KILLED — VALID ONLY WITH Lrearm (RM-F5b)
15                 NOS = site-1 result (cold, L1)      survives (cheap anyway)
16  send incc      receiver = #+ result (probe head)   KILLED
17  returnTop      TOS = incc resume store             survives: bc17 is a jump target ->
                                                       cache cold at its label; B3 merge
                                                       or arm-duplication recovers it
```

8 of 13 round trips killed (~62%), 8 of the ~10 tight forwarding stalls —
honestly contingent on PushInteger conversion (B1) and the Lrearm fix (B2),
both now in-spec.

## 8. Batches and binary gates (smallest first; every batch within-binary A/B-able)

Gate battery **G**: eval smoke (3+4, factorial); within-binary A/B (same
binary, env knob, >=5 reps, min+median) on cfib/benchFib/sfib/floatSum;
PHARO_SP_DEPTH_CHECK clean run (RC-F5: in EVERY gate, not optional);
PHARO_DET_SCHED determinism; 60-class SUnit per-test identical vs knob-off
**excluding documented scheduling-racy classes (Epicea — RC-F8), or compared
against each side's own NO_JIT baseline**; dump disasm inspected at the
converted sites.

```
B0  plumbing, zero emit change
    - debug_vars.h knobs (TOS_REG, TOS_MASK, TOS_POISON, TOS_VERIFY); T1TosCache +
      snapshot-then-clear at both dispatch heads (emitOne_arm64 +
      emitMethodBytes inline ops); jump-target bitmap from the SHARED decode
      routine + emit-time hard-fail assert
    - JIT_CALL/JIT_RESUME_CALL: + ldur x26,[x25,#-8], + "x26" clobber
    - TrampolineAsm.S :226/:292/:477 + pharo_jit_osr_resume (incl. its own
      x26 save/restore at frame #48/#56)
    - helper-call emit wrapper (auto-invalidate) + CMake a.blr( grep ratchet
    - #error guard vs PHARO_T1_TB_IN_X26
    GATE (binary): PHARO_T1_DUMP_SEL=cfib dump byte-identical (cmp) knob-off
    AND knob-on; bench 19/19 unchanged; 60-class SUnit identical.

B1  producers + cheap consumers (knob-gated, mask bits 0-5)
    - all simple pushes INCLUDING PushInteger/PushCharacter; Dup; Pop;
      cond jumps (bool from x26); returnTop; popStoreTemp
    GATE (binary): knob-off dump byte-identical; knob-on disasm shows the
    ldur elisions; DET_SCHED 60-class A/B per-test identical (racy classes
    excluded); within-binary bench A/B x5 recorded per mask bit.

B2  the payoff (mask bits 6-9) — REQUIRES the Lrearm fix in the same batch
    - inline arith family: TOS from x26, result to x26
    - 0-arg send head mov x1,x26
    - resumeAfterCall mov x26,x1 + retarget ALL ~27 endOfSend spec arrivals
      through per-site Lrearm stubs (RM-F1)
    - inline-getter result mov
    GATE (binary): B1 gates + spDepthCheck clean + full bench 19/19 +
    one full bench + 60-class run under PHARO_T1_TOS_VERIFY (zero traps —
    the deterministic net for exactly the RM-F1 bug class) + canaries
    (CharacterTest>>testStoreStringAll, AIPrimTest under DET_SCHED) +
    cfib/benchFib/sfib within-binary A/B x5 with per-mask-bit attribution.
    Park evaluation point (§9).

B3  conditional extensions, each mask-gated and individually A/B'd
    - constSmI tag + single-check shrink + range-checked imm forms
    - popStoreRecvVar (immutable bail before sp decrement — invariant)
    - post-blr re-arm ldur (the re-scoped RM-F2 item; expect ~0, keep if free)
    - predecessor-AND merge at join labels (recovers bc17's returnTop trip)
    GATE (binary): G per item; any item measuring <1% with added complexity
    is masked off by default, not reverted.

B4  graft: cmp + b.cond fusion (mask bit)
    - inline-SmI fast path of compare emits cmp+b.cond when the next
      bytecode is a naked FORWARD conditional jump that is not itself a
      jump target; x25 published before the b.cond; the jump bytecode's
      full unfused emit remains bound at its bcLabel (send-resume arrival
      path for non-SmI operands)
    GATE (binary): G + per-test identity on a comparison-heavy class set +
    assert-all-labels-bound build (debug asmjit finalize).

B5  default flip -> PHARO_T1_NO_TOS_REG opt-out (separate commit)
    GATE (binary): 200-class per-test-identical A/B + full-suite
    survivability >= current max + emit-size delta measured (<+5% net).
```

## 9. Expected-win model (honest, term-ranked, anchored)

What this lever is NOT: the Cog-gap closer. cfib's 20 ms over the 4 ms
skeleton is ~230/310 insns of send-activation machinery, untouched here.
Judge the batches against the addressable round trips, not against Cog.

Terms, ranked by mechanism confidence (RM-F6 inversion applied — the
original draft's ranking is corrected here):

```
term                                mechanism                            confidence
resume->arith->send chain (bc15)    loop-carried retval chain: callee     HIGH — this is cfib's
                                    ret -> x1 -> mov -> arith -> next     actual cross-activation
                                    send; store+reload removed from       critical path
                                    the carried dependency
cond-jump bool round trip           str -> ldur within <=3 insns,         HIGH — feeds branch
(+ B4 fusion)                       feeds branch resolve; suite #1        resolve; 5.3M/100M bc
                                    pair when fused                       suite-wide
inline-getter / address feeds       x26 value becomes a load ADDRESS      HIGH where present —
                                    downstream (add x3,x1,x6,lsl 3)       the sp-in-x25 -31%
                                                                          precedent was exactly
                                                                          an address-dep kill
arith TOS handoffs                  4-5 cy forwarding off the chain;      MEDIUM — some are
                                    some chains hidden behind OoO         already hidden
0-arg probe-head receiver mov       on a predicted IC hit, x1 feeds       LOW — verification-only
                                    only tag/key/compare/branch — a       chain, plausibly ~0
                                    verification chain OoO retires        (V2 precedent); the
                                    off-path; callee reads receiver       callee reads memory,
                                    from stack memory                     not x1
```

Honest prediction: **cfib 24-25 ms -> 21-24 ms (-5..-15%)**; straight-line
arith/store microbenches (sum-1M class) 10-25%; suite-wide small-but-real via
the cmp+jump pair at B4. Instruction count drops only ~3% — the claim is
~8 killed handoffs x 4-5 cy ≈ 30-40 cy off a 200-300 cy activation, NOT
count.

**Park rule (RC-F12 applied):** park the whole lever only if ALL of cfib,
benchFib, AND the sum-class microbench move <3% within-binary after B2+B4.
A flat probe-head term alone (expected!) does not park anything — mask
attribution exists precisely so vanity terms die individually while real
terms ship. If parked: keep B0/B1 if neutral (they simplify the ~35 inline
emitLoadSp sites), mask the rest, return to the send-activation lever.

## 10. Traceability: every reviewer finding -> resolution

RM-* = minimal review, RC-* = cog-faithful review, RF-* = perf-first review.

```
finding  severity  resolution in this design
RM-F1    BLOCKER   Lrearm stub per send site; all ~27 endOfSend spec arrivals
                   retargeted; written into B2 spec (§5). resumeAfterCall mov
                   falls through, hot path load-free.
RM-F2    HIGH      "callee-saved = free survival" KILLED. B3 item is a post-blr
                   ldur re-arm from memory — always correct under write-through,
                   immune to moving GC (§5).
RM-F3    MED       Snapshot-then-clear semantics mandated in §3; label-bind-time
                   invalidation ordering specified; in B0.
RM-F4    MED       Jump-target mismatch HARD-FAILS the compile (negative-cached
                   interp fallback), never BUG-print-and-continue; shared decode
                   routine (also resolves RC-F3); in B0.
RM-F5    MED       PushInteger/PushCharacter added to B1 producers; cfib ledger
                   (§7) corrected with explicit contingencies; 8/13 claim now
                   consistent with batch scope.
RM-F6    MED       Expectation ranking inverted in §9: resume-chain + getter
                   address-deps HIGH, probe-head mov LOW; park rule keys on the
                   ensemble, not on any single term.
RM-F7    LOW/MED   Helper-call wrapper + CMake grep ratchet on raw a.blr( in B0.
RC-F1    BLOCKER   Moot — no register pool; x26 is untouched by the ByteString =
                   inline op (it uses x8-x15). LESSON CARRIED: any future
                   multi-register extension requires per-family clobber masks +
                   an x8-x15 audit; recorded in §11.2.
RC-F2    HIGH      Moot — no bcToCode zeroing, no entryFlushed[]; every bcLabel
                   stays a memory-canonical entry; entry-stub ldur arms x26 (§4).
RC-F3    HIGH      Adopted: one shared prescan/emit decode + emit-time assert +
                   op-start-boundary refusal (merged with RM-F4, §5).
RC-F4    MED       Moot — no fixup records, no deferred depth; targets simply
                   invalidate; x25 is always exact.
RC-F5    MED       Adopted as written rule: bails branch before result store and
                   before any x25 adjustment (§5, §6 arith row);
                   PHARO_SP_DEPTH_CHECK in every gate battery (§8).
RC-F6    LOW       Moot — no unspilled entries below a retval; x25 exact at
                   returns. The "never add ExitReturn to spDepthCheck without
                   spilling-below-retval" caveat does not arise here.
RC-F7    MED       Ordering note carried (§6 returnTop row): mov x1,x26 precedes
                   the J2J prelude; trivially safe since x26 holds the value,
                   not a re-readable descriptor.
RC-F8    MED       Adopted: racy classes (Epicea) excluded from per-test identity
                   gates or compared against own NO_JIT baseline (§8 battery).
RC-F9    MED-HIGH  Moot — write-through needs zero bail-materialization stubs;
                   total code growth is +2 insns/send-site (Lrearm) + 1 mov,
                   measured at B5 (<+5% net gate).
RC-F10   hardening Adopted twice over: PHARO_T1_TOS_POISON (§3) poisons x26 at
                   every invalidation point (consume-after-invalidate crashes);
                   PHARO_T1_TOS_VERIFY (§3, cog VERIFY-knob transposed) traps
                   valid-but-stale at the consuming insn — required clean in the
                   B2 gate, the deterministic net for the RM-F1 bug class.
RC-F11   LOW       Carried into B3 constSmI item: use the emitMovImm path; never
                   assume 2-insn constant materialization.
RC-F12   perf      Adopted: §9 zero-list (sp-bookkeeping merges, mov chatter,
                   dead-store deletion) excluded from the win model; park rule
                   keys on the ensemble incl. sum-class.
RF-1     BLOCKER   Moot for the core (no bytecode is ever elided; every bcLabel
                   bound + valid). Carried as a B4 design constraint: the fused
                   jump bytecode keeps its full unfused emit at its bcLabel (§6).
RF-2     BLOCKER   Carried as a hard rule: fusion applies to naked FORWARD
                   conditional jumps only; this design introduces zero
                   JIT-resident backward branches (T1's preemption model —
                   backward jumps exit to interp — is untouched).
RF-3     HIGH      Moot — resumeAfterCall keeps its structure (+1 mov); no
                   relocation of resume labels; spec arrivals go through Lrearm,
                   which performs no sp/arg operations.
RF-4     HIGH      Carried into §11.3 (spec== fusion open question): if pursued,
                   siteIdx MUST still increment for elided sends, dead
                   SendSitePatch{} record retained, prescan-send-count ==
                   final-siteIdx assert added.
RF-5     MED       Verified moot for the 0-arg head feed: mov replaces ldur
                   in-place, same length; patch-word offsets are label-derived
                   (keyMovzOffset/tailOffset), no site duplication anywhere.
RF-6     MED       Adopted (§6): imm12/logical-imm range checks before choosing
                   immediate forms; nilOop/heap pointers NEVER as immediates;
                   emitMovImm fallback; assert asmjit error codes on new emits
                   (asmjit silent-drop history).
RF-7     MED       Adopted (§6 B4 row): x25 published before the fused b.cond so
                   both successor edges see canonical sp.
RF-8     MED       Folded into §9: census-derived rows ranked, F4c-class
                   dead-store deletions expected ~0, honest range 5-15%.
RF-9     MED       Largely moot (no inline slow copies); residual growth from
                   Lrearm + retained unfused jump emits measured at B5.
RF-10    LOW       Moot — write-through means Dup's memory read is never stale;
                   the cache-valid Dup uses x26 anyway. No leader-only
                   convention needed.
RF-11    LOW       Moot — returnTop emits the prelude once, as today.
```

## 11. Open questions, each with a probe

1. **Does the 0-arg probe-head receiver mov measure anything?** Probe: it
   has its own mask bit; within-binary A/B on cfib + a send-heavy
   microbench at B2. Expected ~0 (RM-F6); keep if free, since it costs
   nothing and feeds the getter-spec address case when the spec is inlined.
2. **NOS second entry / any multi-register extension.** No free
   callee-saved register; scratch regs conflict with the send path, and
   RC-F1 proved the x8-x15 pool premise false (ByteString = clobbers).
   Probe: emit-time static census of NOS round trips that survive B2
   (count `ldur [x25,-16]` consumers whose producer is <=5 insns prior);
   revisit only if that count is large AND B2 measured real. Prerequisite:
   per-family clobber masks + full x8-x15 audit.
3. **spec== / ifNil fusion** (2.4M + 1.7M + 1.45M per 100M bc suite-wide;
   the only graft row that deletes serializing IC-probe machinery rather
   than movs). Blocked on RF-4 siteIdx accounting. Probe: prototype behind
   a mask bit with the siteIdx fix + prescan/emit send-count assert; A/B on
   an ifNil-heavy microbench; promote to a batch only if >3%.
4. **Predecessor-AND merge at join labels** (recovers bc17's returnTop
   round trip). Probe: emit-time counter of jump-target labels whose every
   predecessor would arrive valid; implement only if the static count says
   the bc17 shape is common (cheap arm-duplication for cond-jump arms is
   the fallback).
5. **Post-blr re-arm ldur** — win or wash? Probe: mask bit + A/B on a
   helper-heavy workload (at:put:-dense). The load is always correct;
   question is purely whether a consumer exists often enough.

## 12. Risks

1. **x26 clobber-contract miss** — some C++ path assumes x26 preserved
   across JIT_CALL (true until now). Mitigation: clobber-list additions
   make the compiler cope; audit = grep every JS_SP hoist + every blr into
   JIT code; pharo_jit_osr_resume is the known site needing a new save.
2. **valid-without-x26-written** (the only corrupting direction).
   Mitigation: paired producer helpers (§3), snapshot-then-clear default,
   POISON knob makes consume-after-invalidate crash deterministically,
   VERIFY knob traps valid-but-stale at the consuming instruction.
3. **endOfSend-class merge missed at some OTHER label.** Mitigation: the
   only emitter-internal multi-predecessor labels that can carry validity
   are endOfSend (fixed) and bcLabels (forced invalid); B4's fusion adds
   none (unfused jump emit retained). Any new internal join introduced
   later must default-invalidate — note added at the T1TosCache definition.
   VERIFY-knob gate runs catch any escape at the exact consuming insn.
4. **Jump-target bitmap divergence.** Mitigation: shared decode + hard-fail
   (§5); failure mode is a refused compile, never corruption.
5. **Perf measures zero** (OoO hides it — V2/leak-guard/MAX_IC precedents).
   Mitigation: per-term mask A/B at every batch, ensemble park rule (§9),
   strongest-prior terms land in the same batch as the measurement gate.
6. **Heisenbug surface**: any new SUnit transient → PHARO_DET_SCHED=1
   first, then knob/mask bisect — the cache is a pure overlay, so a single
   run isolates it.
7. **Code growth**: +2 insns/send site (Lrearm) + 1 mov + retained unfused
   jump emits at B4 — bounded, measured at B5 (<+5% net gate); no inline
   slow copies, no bail stubs.

## 13. Key files

```
src/vm/jit/asmjit/AsmjitT1.cpp   emitOne_arm64 + emitMethodBytes (dispatch heads),
                                 resumeAfterCall/endOfSend (~27 predecessors, Lrearm),
                                 TB-in-x26 helpers :2257-2291 (#error guard), prescan
src/vm/jit/JITState.hpp          JIT_CALL / JIT_RESUME_CALL: + ldur, + "x26" clobber
src/vm/jit/TrampolineAsm.S       hoist sites :226 :292 :477; pharo_jit_osr_resume
                                 :548+ (x26 save/restore + ldur)
src/vm/debug_vars.h              PHARO_T1_TOS_REG / _MASK / _POISON / _VERIFY
src/vm/jit/BcDepthMap.cpp        UNCHANGED — design invariant
CMakeLists.txt                   a.blr( grep ratchet (envPresent-RATCHET pattern)
```
