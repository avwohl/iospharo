# Patched monomorphic send sites — design

Status: COMPLETE 2026-06-10 — empirical inputs + synthesized design.
Implementation batches B0-B6 in section 11 of the design.

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

## Design — synthesized (wf_21df75cc-6cc, 11 agents: 4 readers, 3 designers, 3 adversarial reviews, 1 synthesis)

# Patched Monomorphic Send Sites (PMS) — arm64 T1 JIT, definitive design

Status: synthesis of three independent designs (minimal-risk, cog-faithful, perf-first) and their
adversarial reviews. Skeleton = **perf-first** (site-side patched class key + patched direct branch
into a per-site baked J2J tail), with the minimal-risk design's mirror-invariant/fallback discipline
and the cog-faithful design's link-predicate and quick-prim ideas grafted in. Every reviewer finding
is resolved in-line; the traceability table in §15 maps finding → resolution. Line anchors are as of
commit `124877dd` (re-grep before editing; anchors marked `~` were taken from the analysis pack,
unmarked ones were re-verified 2026-06-10).

Verified anchors this session: `isPhase4SendOp` AsmjitT1.cpp:917-933; send-emit branch :3782;
poly-walk knob opt-in DebugSettings.cpp:246 vs stale opt-out comment AsmjitT1.cpp:3897;
`_pad_76` JITMethod.hpp:250; `IC_BYTES_PER_SITE` JITMethod.hpp:116; chain-loop stale-IC zeroing
Interpreter.cpp:~23582-23594 (raw `state.icDataPtr`, no ownership guard); `isSpliceTarget` set at
compile (JITCompiler.cpp:2512) **and after the fact at splice time** (JITRuntime.cpp:2935, 3481),
race-gating precedent at JITRuntime.cpp:2370-2388.

---

## 0. Decision summary

- **Site-side patching, no callee prologue.** The cog-faithful callee-entry design was rejected
  because prepending a prologue breaks the load-bearing `codeStart() == entry` invariant in ≥6
  places (bit-60 fills, savedResumeEntry, save-addr range checks, `resumeAfterCall`
  self-identification via `g_codeStartLabel`, megaCache jitEntry, JIT_CALL) — a cross-cutting
  migration with no incremental shape. PMS leaves every existing entry-address computation
  untouched.
- **Two link states only: UNLINKED and LINKED-J2J.** The perf-first design's third state
  (LINKED-GEN, key-check-only) is **dropped** per its review BLOCKER-1: it has today's load depth
  (no win) while being the one state where a missed slot-0-writer hook becomes silent
  wrong-inline-getter corruption (the MAX_IC=1 poisoning family). Mismatched or unclassified
  classes always take the in-site generic probe.
- **No closed PICs in v1.** The heap IC (slots 0-5) + megaCache is the polymorphic/megamorphic
  tier, exactly as today. The generic probe IS our PIC. (Cog-faithful's CPIC lifecycle had four
  unresolved gaps — orphaning, oop-0 collisions in methodMap, eviction thrash, forEachRoot
  tier-awareness; none are worth solving before the monomorphic win is proven.)
- **Heap IC stays the single source of truth.** A patch is a pure, derivable cache of IC slot 0.
  Unlink is always-safe and is ONE aligned 32-bit store.

### The four invariants (everything else follows from these)

1. **No oop bits are ever patched into code.** Patched immediates are only: a classIndex /
   immediate-tag lookup key (GC-stable — and this VM never reclaims class-table indices, see §7
   event 1 note), a `JITMethod*` (the code zone never moves), and code-zone branch displacements.
   Callee `compiledMethodOop`, `bcStartCache`, literals are loaded at runtime from the baked
   JITMethod header, which `forEachRoot` already updates in place (Interpreter.hpp:~3357-3375).
   Scavenge is therefore a structural no-op for patches; the bcStartCache / megaCache-tenure /
   icBuffer-write-after-free staleness class is eliminated by construction.
2. **Every patch is re-derivable from heap IC slot 0** via one idempotent function
   `JITRuntime::linkSendSite(JITMethod*, uint32_t siteIdx)`. Every writer or zeroer of slot 0 must
   end by calling it (full enumeration §6; tripwire for the dead writer §6.1).
3. **Patches never change code layout.** Fixed-shape sites, in-place 32-bit stores only. All
   continuations — bcToCode re-entries, V2 `resumeAfterCall`, J2J-save resumeAddrs, pinned native
   LRs — stay valid across every link-state change.
4. **No BL/BLR anywhere between the site head and the end of the patched tail** (emit-time
   assert). Hence no native return address can ever point AT a patched word, which is what makes
   patch-while-frames-are-live safe without Cog's callee-side miss recovery.

Dual-protocol: runtime emit knob `DEBUG_BOOL(PHARO_T1_PATCHED_SENDS)` in `src/vm/debug_vars.h`
(NOT a compile-time `#define` — within-binary A/B is mandatory for all perf claims, per the
layout-knife-edge rule). Knob-off emit must be byte-identical to today. Flip at the end =
default-on + `DEBUG_BOOL(PHARO_T1_NO_PATCHED_SENDS)` opt-out, the `PHARO_T1_NO_INLINE_J2J`
pattern. arm64 only; the x86_64 emit (AsmjitT1.cpp:~2010-2115) is untouched.

---

## 1. Motivation and honest performance model

Today's monomorphic classified hit (AsmjitT1.cpp:3782 onward) costs, per send:

```
loads   ~14, in 5 dependent levels:
        state->jm -> jm->icBuffer -> icData[0]/extras -> calleeJM-derived gate loads
        (canSkipJ2JSave, methodHeader, numICEntries, isStubOnEntry, canBailMidMethod,
        bcStartCache) -> br x9 (indirect)
stores  ~12-14 (V2 save push, JITState field updates, nil-fill)
uops    ~50-60
```

The linked PMS path is ~35-40 µops, ~9 loads **all at dependence level 1** (off `x0` or the baked
`x10` immediate), the same ~13 stores, and two statically-predicted direct branches replacing the
predicted-indirect `br x9`.

**What to expect (per both adversarial reviews — do not oversell):** the `b.ne`/`br` on today's
path are predicted, so the deleted load chain mostly delays branch *verification*, not fetch.
Realistic win: **~15-30% on send-bound microbenches** (dependent-send chains like cfib benefit
most), plus a second-order win on large-footprint reflective workloads (direct `b` removes
per-site indirect-BTB capacity pressure across thousands of SUnit sites). What PMS does **not**
touch: the store half (V2 save push, JITState updates, nil-fill) and — critically — the
**unclassified-send C++ round trip** (`extras==0 → dispatchCached → EXIT_SEND_CACHED`, ~1000
cycles), which is where the measured `o class` 3.4M/s-vs-Cog-60M/s gap lives (`class` is prim 111:
no inline-prim mapping, no trivial classification, eager compile negative-cached → extras stays 0
→ never linkable, never fast). Closing the Cog gap is the **composition**: PMS (this doc) +
classifier/compiler coverage for quick prims (§11 B6, a mostly-orthogonal lever) + saveless/
self-rec tail variants + MAX_IC widening (§13). Initially the linked population ≈ today's
inline-J2J population (the `PHARO_T1_XMETHOD_MAX_IC`=1 gate, debug_vars.h:~92); link-time gating
is precisely what makes widening that gate nearly free later, because the per-call gate loads
become once-per-link C++ checks.

Benchmarks for claims: cfib/benchFib (classified-send-dense — the lane PMS accelerates),
`/tmp/bench2.st`/`/tmp/bench3.st` send-activation micro (expected to move only at B6), and the
W^X-regression sentinels sieve×100 (~44 ms; the 2026-05-03 per-fill-flip regression took it to
87 ms) and sort 100K.

---

## 2. Site layout

Emitted in the `isPhase4SendOp` branch (AsmjitT1.cpp:3782) when `GET_DEBUG_BOOL(PHARO_T1_PATCHED_SENDS)`.
`isPhase4SendOp` (AsmjitT1.cpp:917-933) covers only plain sends — the two non-inlined binary
special selectors, special-selector sends 16-31, and the 0/1/2-arg literal-send ranges. Super and
directed-super sends are not Phase-4 sites, so their exclusion from linking is automatic
(cog-review Q3, resolved).

### 2.1 Hot head — fixed shape, offsets from site start S (= `bcToCode[sendBc]`)

```
S+0   mov   x2, x25                       ; sp copy (x25 = resident sp)
S+4   ldur  x1, [x2, #-8*(nArgs+1)]       ; receiver                    LOAD A
S+8   and   x4, x1, #7
S+12  cbnz  x4, S+36                      ; immediate receiver
S+16  lsr   x4, x1, #48                   ; bits-48-63 leak guard (knob-conditional, as today ~3864)
S+20  cbnz  x4, LmissNoX5
S+24  ldr   w4, [x1]                      ; header low word              LOAD B (dep on A)
S+28  and   w4, w4, #<classIndexMask>     ; named ObjectHeader constant, never a literal
S+32  b     S+40
S+36  orr   w4, w4, #0x80000000           ; immediate-key marker (valid AArch64 logical imm,
                                          ;  same encoding as today's ~3872 — verified, no
                                          ;  asmjit silent-drop hazard)
S+40  movz  w6, #<keyLo16>                ; PATCH W0
S+44  movk  w6, #<keyHi16>, lsl #16       ; PATCH W1   <- the single-store unlink word
S+48  cmp   w4, w6                        ; 32-bit compare; both key forms fit 32 bits
S+52  b.ne  Lprobe                        ; class mismatch -> in-site generic probe
S+56  b     <target>                      ; PATCH W2   unlinked: b Lprobe   linked: b T
```

`kImpossibleKeyHi16 = 0x0040`: an unlinked/unlinked-again key reads `0x0040____`, which exceeds
the 22-bit classIndex range and lacks bit 31 (the immediate marker), so it matches no receiver
ever — therefore **unlink = one aligned store of W1**, architecturally atomic to instruction
fetch. After unlink, W2 may still say `b T`; it is unreachable because the compare cannot succeed.
*Forbidden forever:* any change that makes W2 reachable without a key match.

The `ldr x11,[x0,#OFF_JITMETHOD]` / `ldr x5,[x11,#icBuffer]` / `add` prefix (today ~3782-3791)
moves into `Lprobe`; the hot path never touches `x5`/`x11`.

### 2.2 Linked-J2J tail — per-site skeleton at recorded offset T

Emitted only when the V2 packing gate passes (1-byte send, `resumeBcOff ≤ 0xFFF`, `nArgs ≤ 15` —
the exact existing gate at ~4965-4970, reused verbatim); otherwise `tailOffset = 0` and the site
can never link (it keeps the full legacy behavior via Lprobe). Shape mirrors today's J2J body
(~4925-5219) with three changes: calleeJM is **patched immediates**; the five runtime gate loads
are **evaluated once at link time in C++ and disappear**; the terminal `br x9` becomes a
**patched direct `b`**.

```
T+0    movz  x10, #<jmLo16>                          ; PATCH W3   calleeJM — 48-bit zone
T+4    movk  x10, #<jmMid16>, lsl #16                ; PATCH W4   address, never moves
T+8    movk  x10, #<jmHi16>,  lsl #32                ; PATCH W5
T+12   ldp   x6, x14, [x0, #OFF_J2J_SAVE_CURSOR]     ; cursor/limit (x14 is pure scratch here —
T+16   cmp   x6, x14                                 ;  the resume word is built below at T+24,
T+20   b.hs  Lprobe                                  ;  unlike the cog-faithful inbound-x14 ABI)
T+24   adr   x14, resumeAfterCall                    ; static (label inside this site)
T+28   movk  x14, #<nArgs<<12 | resumeBcOff>, lsl #48 ; static V2 packing (Interpreter.hpp:~656-676)
T+32   mov   x15, x25
T+36   ldr   x4, [x0, #OFF_RECEIVER]                 ; caller receiver — same source as today ~4972
T+40   stp   x15, x4, [x6], #32                      ; V2 packed save push (record layout
T+44   ldr   x15, [x0, #OFF_TEMPBASE]                ;  bit-identical to today's ~4971-4975)
T+48   stp   x15, x14, [x6, #-16]
T+52   ldr   x13, [x10, #offsetof(JITMethod, compiledMethodOop)]   ; level-1: x10 is an immediate
T+56   str   x10, [x0, #OFF_JITMETHOD]
T+60   str   x13, [x0, #OFF_METHOD]
T+64   add   x13, x13, #16
T+68   str   x13, [x0, #OFF_LITERALS]
T+72   mov   w13, #nArgs
T+76   str   w13, [x0, #OFF_ARGCOUNT]
T+80   str   x6, [x0, #OFF_J2J_SAVE_CURSOR]
T+84   ldr   x13, [x0, #OFF_J2J_DEPTH]
T+88   add   x13, x13, x20                           ; x20 = depth+totalCalls inc, trampoline-preloaded
T+92   str   x13, [x0, #OFF_J2J_DEPTH]
T+96   str   x1, [x0, #OFF_RECEIVER]                 ; x1 = new receiver, live from the head
T+100  sub   x13, x2, #nArgs*8                       ; x2 = caller sp, live from the head
T+104  str   x13, [x0, #OFF_TEMPBASE]
T+108  ldr   x14, [x10, #offsetof(JITMethod, bcStartCache)]        ; GC-refreshed header field
T+112  str   x14, [x0, #OFF_IP]
T+116  ldrb  w15, [x10, #offsetof(JITMethod, tempCount)]
T+120… nil-fill loop (dynamic, as today's xmethod path ~5198-5212)
Tend-4 mov   x25, x15                                ; callee sp resident
Tend   b     <calleeEntry>                           ; PATCH W6   imm26 direct branch
```

All JITMethod field accesses via `offsetof(JITMethod, ...)` ONLY (the JM-byte-offset off-by-one
lesson). Link-time gating reads struct fields directly in C++, eliminating that bug class.

`b.hs Lprobe` (save-stack full): at T+20 the Lprobe register contract holds — `x0,x1,x2,x4,x25`
intact (only `x6,x10,x14` clobbered so far). Lprobe re-probes slot 0, the key matches (the site is
linked, so slot-0 key == patched key), extras carries bit 60 → the generic `tryInlineJ2J` path
runs its own cursor check and performs the proper full-bail to C++. Verified chain; document at
the emit site.

### 2.3 Cold blocks (per site)

```
Lprobe:     ldr x11,[x0,#OFF_JITMETHOD]; ldr x5,[x11,#offsetof(JITMethod,icBuffer)]
            (add x5, #siteIdx*IC_BYTES_PER_SITE — elided for siteIdx 0)
            <today's probe + extras classifier chain, byte-for-byte: slot-0 key cmp,
             opt-in poly-walk slots 1-2, cbz x7 -> dispatchCached, per-nArgs prim
             prechecks, dispatch-A trivial specs, generic tryInlineJ2J, all knobs>
LmissNoX5:  materialize x5 (ldr/ldr/add); b Lmiss      ; leak-guard edge, satisfies Lmiss's x5 contract
Lmiss / dispatchCached / resumeAfterCall / endOfSend:  unchanged (~6685-6747)
```

The debug probe knobs (`t1ProbeAlwaysMiss`, `PHARO_T1_HIT_FORCE_DISPATCH`) live in Lprobe;
`linkSendSite` **refuses to link while either is set** so the knobs keep meaning what they mean.

Register contracts, documented as a comment block at the emit site (the shared-label hazard):

```
Lprobe       requires x0=state x1=receiver x2=spCopy x4=lookupKey x25=sp; defines x5,x6,x7,x11
Lmiss, dispatchCached   additionally require x5=icDataPtr (hence LmissNoX5)
classifier   x4,x6 dead on entry; x5 live through the whole dispatch chain (tryPrim* stash protocol)
```

Unlinked-state cost: 4 inert ALU instructions + one extra `b` versus today (the never-matching
movz/movk/cmp/b.ne). Measured in B1, budget < 2-3% within-binary.

---

## 3. Link states

```
state        W0/W1 key           W2              W3-W5        W6
UNLINKED     kImpossibleKey      b Lprobe (or    skeleton/    stale, unreachable
                                 stale b T)      stale
LINKED-J2J   IC slot-0 key       b T             calleeJM     b calleeEntry
```

That is the whole state machine. Megamorphic, polymorphic, unclassified, trivial-spec, immediate-
heavy, blocked-by-gate — all of those are simply UNLINKED, i.e. exactly today's behavior.

---

## 4. The patcher — `src/vm/jit/SendSitePatcher.{hpp,cpp}` (new)

Hand-rolled encoders for exactly four instruction forms, **unit-tested at build time against
asmjit/llvm-mc output** (asmjit is never used at patch time — sidesteps its silent-drop hazard):

```
movz w6,  #imm16            0x52800006 | imm16<<5
movk w6,  #imm16, lsl 16    0x72A00006 | imm16<<5
movz/movk x10 (3 forms)     0xD280000A / 0xF2A0000A / 0xF2C0000A | imm16<<5
b    imm26                  0x14000000 | (((target-pc)>>2) & 0x3FFFFFF)
```

`JITRuntime::linkSendSite(JITMethod* jm, uint32_t siteIdx)`:

1. Read `SendSitePatch` record (§9); return if `keyMovzOffset == 0` or `tailOffset == 0`.
2. Read heap IC slot 0. Empty → `unlinkSendSite`. Else evaluate the link predicate (§5); fail →
   unlink (or leave unlinked). Pass → derive callee entry via the single shared
   `entryAddrFor(JITMethod*)` helper (also used by `rewriteIcEntriesAfterRecompile` — one
   computation, can't drift).
3. **Debug pre-assert:** decode every word about to be overwritten and verify its opcode class
   (movz/movk/b). Hard-fail on mismatch — catches patch-map drift immediately instead of as a
   layout knife-edge.
4. Under one depth-aware `ScopedWriteAccess` (§4.1): if currently linked to a different target,
   store W1 := impossible **first**; then W3-W6, W0; then **W1 last** (the commit). Single-store
   ordering contract: *the key-hi word is always the last word that can make the fast path
   reachable*, so any future window-splitting refactor fails safe (site stays unlinked rather
   than briefly linking a stale tail). Destructor: `sys_icache_invalidate` over the two touched
   ranges (head words, tail words — NOT the whole zone) + restore X mode.
5. `flags |= linked`, `numPatchedSites_++`, `g_patchLinkCount++`, saturate IC slot-19 hitCount
   (linked sites stop bumping it; saturation keeps warmth-reading heuristics honest).

`unlinkSendSite(jm, siteIdx)` = one W1 store + one-line icache flush + flags/counter updates.
Batch variants (`unlinkAllSites(jm)`, `unlinkSitesTargeting(ranges)`, `unlinkEverything()`) take
one write window and **coalesced ranged flushes** — never a whole-zone invalidate (the 64 MB
cache-line walk objection), and all early-out on `numPatchedSites_ == 0` so non-JIT-heavy runs
keep today's flip-free property on flush paths.

W^X economics (the 2026-05-03 objection answered): the per-**fill** flip that cost sieve 2×
ran millions of times; links run once per site per classification-change (and once per site per
full GC — measured, see §13 Q1). IC *fills* stay heap-side and flip-free.

### 4.1 W^X nesting (cog-review H2 — applies to this design too)

`unlinkSitesTargeting` is called from inside `JITCompiler::compile`'s eviction path — i.e.
potentially between `CodeZone::allocate`'s `makeWritable` and `finalize`'s `makeExecutable`.
`pthread_jit_write_protect_np` is a thread-global mode, not a counter: a naive nested
`ScopedWriteAccess` destructor flips the thread back to X and the outer compile's next emit store
SIGBUSes. **Fix shipped with B2:** `ScopedWriteAccess` gains a thread-local depth counter; only
the outermost construction/destruction flips the mode (icache flushes still happen eagerly —
`sys_icache_invalidate` is mode-independent). Audit every existing `ScopedWriteAccess` user when
adding the counter.

### 4.2 Why patching-while-executing is safe

All patching runs on the single Smalltalk thread inside the C++ runtime; no JIT code executes
concurrently (heartbeat thread sets a flag only; signal handlers verified this session:
`sigprofHandler` reads `activeMethod()` + atomics only, the test-harness SIGSEGV/SIGBUS/SIGILL
handlers dump and `_exit` — no longjmp, so no thread-left-in-W-mode or half-patched-resume
hazard; keep this audited if handlers ever grow). Every re-entry into JIT code lands at a
`bcToCode` offset, a `resumeAfterCall` label, or a pinned helper-call LR — all outside the patched
words by invariant 4 (emit-time assert: no patch word is the successor of a call instruction).
Multi-word tearing is therefore unobservable; the single-store W1 unlink is additionally
fetch-atomic as insurance. `assert(isVMThread())` in `linkSendSite`.

---

## 5. Link predicate (`canLinkSite`) — all must hold, else stay/become UNLINKED

```
site:    keyMovzOffset != 0 && tailOffset != 0
         debug probe knobs unset; PHARO_T1_PATCHED_SENDS_NOLINK unset
         IC slots 1-5 empty (monomorphic evidence; later poly fill does NOT unlink —
         slot-0 class keeps the fast path, others take Lprobe)
slot 0:  key != 0, extras has J2J_ENTRY_BIT (60)
         extras bit 59 (BLOCK_VALUE) clear        — structural, not gate-dependent: a linked
                                                    #value site would direct-call ONE
                                                    CompiledBlock for every closure of the class
         extras bit 55 (SISTA) clear              — Sista fn-ptr lifecycle is not in this matrix
         extras bit 58 (returnsLiteral) clear     — keep the cheaper retlit shortcut on dispatch-A
callee:  methodMap_[methodBits] exists, state == Compiled, tier == T1
         !isSpliceTarget                          — see §7 event 8
         !isStubOnEntry, !canBailMidMethod, methodHeader prim-bail bit clear
         numICEntries <= GET_DEBUG_INT(PHARO_T1_XMETHOD_MAX_IC)
         selectorOop == selBitsArray[siteIdx]     — Cog's selector cross-check, cheap sanity
         numArgs == site arity
reach:   |calleeEntry - W6 pc| fits imm26 (±128 MB; zone max 256 MB per JITConfig — guard
         mandatory, refuse-to-link on failure, NEVER assert-and-continue)
```

Emit-time reach/size failures (adr to resumeAfterCall, tail size on huge tempCount) likewise
degrade to `tailOffset = 0` legacy shape — fallback, never assert.

---

## 6. Link triggers — every slot-0 writer/zeroer, enumerated

```
writer / zeroer                                   action                     anchor
patchJITICAfterSend, first-empty-slot write       linkSendSite if slot 0     Interpreter.cpp ~21381
patchJITICAfterSend, dup-key path                 linkSendSite (re-warm:     ~21048
                                                  covers IC-survived/
                                                  patch-unlinked)
upgradeICToJ2J, extras-upgrade path               linkSendSite               ~21713
upgradeICToJ2J, empty-slot classification fill    linkSendSite               ~21857
chain-loop stale-hit zeroing                      unlinkSendSite WITH the    ~23582-23594
                                                  full ownership guard +
                                                  floor-to-site (see below)
Sista invalidateIfHintless (slot-0 modifier)      re-derive (link/unlink)    ~21409
rewriteIcEntriesAfterRecompile                    re-patch W3-W6 / unlink    JITRuntime.cpp ~2853
recompile finalize (copied warm icBuffer)         eager link pass            JITCompiler.cpp ~2578
```

Both Interpreter hooks must pass an explicit *wrote-slot-0* flag (the upgrade path can also write
slots 1-5; only slot-0 writes trigger). `siteIdx = (slotPtr − jm->icBuffer) / IC_BYTES_PER_SITE`,
already validated by the 2026-06-10 ownership guard in `patchJITICAfterSend` (~20769-20804).

**Eager-compile eviction hazard inside `upgradeICToJ2J`:** its prim-target eager compile
(~21510-21545) can trigger eviction, which can free/recompile the *owner* JM between function
entry and the IC write. Re-run the ownership validation (owner still Compiled in methodMap,
`icDataPtr` within its CURRENT icBuffer) after any `compiler_->compile()` call, before both the
heap IC write and `linkSendSite`. (Whether the current heap-IC write is already exposed to this
is pre-existing-issue #5, §14 — verify regardless of PMS.)

**Chain-loop zeroing hook (the hard one):** today it writes through raw `state.icDataPtr` with no
guard, and under opt-in poly-walk the emitted probe *advances x5 by 24/48 on a slot-1/2 hit*, so
the pointer can be mid-site. The unlink hook must: resolve the owner JM (`findMethodByPC` is not
applicable — use the methodMap/ownership-guard pattern), validate the pointer against the owner's
CURRENT icBuffer, **floor it to the site base** (`(ptr − icBuffer) − ((ptr − icBuffer) %
IC_BYTES_PER_SITE)`), bounds-check siteIdx, and refuse on any mismatch. This also fixes
pre-existing bug #1 (§14).

### 6.1 Tripwire for the dead writer

`jit_rt_fill_ic` (JITRuntime.cpp:~952-1008) is dead but explicitly kept as scaffolding to
reactivate "if ICs move out of MAP_JIT" — which happened. It writes slot 0 from JIT-helper context
with no link hook. Add a loud comment + `assert(numPatchedSites_ == 0)` at its head (or delete
it): reactivation under PMS without a hook is silent corruption.

---

## 7. Invalidation matrix — complete, all nine events

```
#  event                       action                                          where
1  Full GC (recoverAfterGC)    unlinkEverything(): one depth-aware write       JITRuntime.cpp
                               window over the zone, W1 stores alongside the   ~4063-4096
                               existing per-entry IC clear, coalesced ranged
                               icache flushes, flags cleared,
                               numPatchedSites_ = 0.  KEEP THIS PERMANENTLY
                               for v1: the "keys and targets are GC-stable so
                               keep links" optimization rests on classIndex
                               stability, which holds only because this VM's
                               nextClassIndex_ is monotonic (never reclaimed).
                               Add the tripwire comment in registerClass
                               ("class-table reclamation requires
                               unlinkSendsLinkedForInvalidClasses — see PMS
                               design") before anyone adds compaction.
                               UPDATE the now-false "no W^X flip needed"
                               comments at ~3999 and ~4169-4171.
2  Scavenge                    NOTHING (invariant 1).  PHARO_T1_PATCH_VERIFY   —
                               debug walk after every GC in stress builds, §10.
3  Recompile                   rewriteIcEntriesAfterRecompile: for each        JITRuntime.cpp
                               LINKED site whose slot-0 methodBits matches     ~2873-2885;
                               (equivalently decoded W6 in the old range):     JITCompiler.cpp
                               re-evaluate canLinkSite against the NEW JM —    ~2578 (eager pass)
                               tier changed to T2 / gate fails -> UNLINK
                               (never relink across tiers: T2 entry ABI
                               differs — this is a hard rule, not an open
                               question); else re-patch W3-W6 via
                               entryAddrFor().  PLUS the eager link pass at
                               recompile finalize over the memcpy'd warm IC
                               buffer (recompile copies old ICs -> warm bit-60
                               slot-0 entries never miss -> the §6 hooks never
                               fire -> without this pass, recompiled (= the
                               hottest) methods would NEVER link).  Old JM
                               stays Compiled in the zone for in-flight
                               returns, unchanged.
4  Eviction                    Extend the existing post-evict range scrub:     JITCompiler.cpp
                               for SURVIVING methods, walk patchMaps, decode   ~2429-2455 (scrub),
                               W6 of linked sites, unlink any target in an     ~2461-2483 (full-
                               evicted range (single store each; this also     flush fallback)
                               kills the tail's loads from the freed callee's
                               in-zone JM header — one store covers both).
                               Drive off the patch map, not the IC walk, so
                               IC/patch drift can't leak.  Full-flush
                               fallback: unlinkAllSites over every surviving
                               (pinned) method, AND fix the two PRE-EXISTING
                               holes in the same commit (§14 #2): surviving
                               methods' J2J extras and megaCache.jitEntry are
                               not scrubbed on that path today.
                               SHIPS IN THE SAME BATCH AS LINKING — eviction
                               is driven by zone pressure, not link age; a
                               second-old link can point into freed code.
5  prim 89/116/119/214,        flushCaches: unlinkEverything() before the IC   JITRuntime.cpp
   all become variants,        zero (numPatchedSites_==0 early-out preserves   ~3928-3953;
   changeClassOf               the flip-free property when no links exist).    Primitives.cpp
                               Covers redefinition AND any class-table         ~11157
                               surgery.  Sledgehammer-correct, matching
                               today's philosophy; selector-/method-keyed
                               unlink (selBitsArray gives per-site selectors)
                               is a flagged later refinement.  Measured gate:
                               method-install-heavy run (Epicea/SUnit), since
                               this adds a W^X round trip per image-side
                               method install when links exist.
6  Compile failure mid-emit    Nothing: linking requires state==Compiled        —
                               post-finalize; invalidated JMs were never
                               linkable; recompile failure restores the old
                               still-valid JM (kRecompileFailed).
7  freeMethod                  Freed method's own heads/tails die with its     CodeZone.hpp
                               code; inbound links are event 4's job; its      ~227-260
                               freed icBuffer is covered by the ownership
                               guards.
8  Sista splice                isSpliceTarget is set AFTER compile, at splice  JITRuntime.cpp
                               time (verified: 2935, 3481) — so a callee can   2935, 3481;
                               become a splice target after callers linked.    gate precedent
                               TWO hooks: the canLinkSite gate (refuse), AND   2370-2388
                               unlinkSitesTargeting(jm) at both splice-set
                               sites.  This is invalidation event 8, not an
                               open question — it re-creates the documented
                               "T1-vs-Sista race" otherwise.
9  Stale-hit chain-loop        Guarded unlink per §6 (ownership + floor-to-    Interpreter.cpp
   zeroing                     site).  Fires mid-run, not at GC boundaries —   ~23582-23594
                               the easy one to miss.
```

Historical-staleness audit: recompile (3), eviction (4), redefinition/become (5), splice (8) —
handled; moved CM oops — impossible (loaded via the GC-maintained JM header); young-oop recycling
— impossible (invariant 1). Coverage closed.

**Defense-in-depth note (accepted, mitigated):** today every cached send eventually re-crosses
`ExitSendCached`, where the chain loop validates `cachedTarget` and self-heals. A linked site
never exits for its linked class, so any staleness source NOT in this matrix becomes silent wrong
execution instead of a healed miss. Same is true of today's inline-J2J hits — PMS widens the
population. Mitigation: `PHARO_T1_PATCH_VERIFY` (§10) + the matrix-completeness audit rule: **any
new writer of IC slot 0, or any new code-address cache, must be added to §6/§7 in the same
commit.**

---

## 8. Interactions

- **Inline-J2J / classifier:** the linked tail replaces the generic `tryInlineJ2J` only for the
  slot-0 class of gate-passing sites. Everything else — dispatch-A trivial specs (getter/setter/
  returnsSelf/retlit/multi-slot/W1-W6), per-nArgs prim prechecks, BLOCK_VALUE, megaCache — runs
  unchanged inside Lprobe. The six inline-spec knobs keep their meanings.
- **V2 resume:** untouched. Identical save-record layout (the pool walker, GC visitor at
  Interpreter.hpp:~3484-3527, and `materializeJ2J` see no difference), same `resumeAfterCall`
  (~6722-6745), same PC-relative xmethod self-re-identification, same `resumeOverrides`/bcToCode
  rewrites (invariant 3 preserves them). Callee return code needs zero changes.
- **Self-recursion (benchFib):** the tail stores jm/method/literals with identical values —
  correct, ~6 redundant stores. A saveless (`canSkipJ2JSave`) and self-rec tail variant are
  link-time refinements deferred to B5; A/B them against the materializeJ2J j2jDepth-reset lesson.
- **applyICSpecialization / recompile gate:** both read heap IC slot 0, which linking never
  clears — they work unchanged. `applyICSpecialization` may additionally treat a `flags.linked`
  site as confirmed-mono.
- **Telemetry starvation inventory** (the complete list of IC readers): site hitCount (slot 19 —
  saturated at link), `dumpICHistogram` (add a linked-hit/links/unlinks counter set so hot sites
  don't look cold), recompile any-data gate (reads slot 0 — unaffected), Sista
  `invalidateIfHintless` (hooked, §6), megaCache (unaffected — misses still flow through C++ for
  non-linked classes). Probe in §13 Q6.
- **`compact()`:** currently gated to empty zone; add `assert(numPatchedSites_ == 0)` so the gate
  stays honest if anyone relaxes it.
- **x86_64 / stencils / T2:** untouched. T2 tier-up handled by event 3's unlink-on-tier-change.

---

## 9. Bookkeeping

```cpp
// JITMethod.hpp — per-site patch map, in-zone after selBitsArray.
// Same lifetime/properties: written during compile's W window, read-only after,
// NOT GC-visited (contains no oops).  Offsets come from asmjit labels ONLY —
// never fixed-offset arithmetic (debug emit knobs shift layout).
struct SendSitePatch {            // 16 bytes/site
    uint32_t keyMovzOffset;       // code offset of W0; W1=+4, cmp=+8, b.ne=+12, W2=+16.
                                  // 0 = site not patchable (emit fallback shape)
    uint32_t tailOffset;          // T (W3=+0, W4=+4, W5=+8).  0 = no tail emitted
    uint32_t tailBranchOffset;    // W6 (varies with nil-fill shape)
    uint32_t flags;               // bit0 linked (truth readable without disassembly);
                                  // rest telemetry
};
// uint32_t patchMapOffset reuses _pad_76 (JITMethod.hpp:250) — zero layout growth.
```

Sized where selBitsArray is sized (JITCompiler.cpp:~2286-2294 — the count list MUST match the
IC-setup loop, the A1 P0 lesson); records filled in the emit loop next to the
`g_resumeOverridesPtr` pushes (~6743-6745). Global `JITRuntime::numPatchedSites_` for early-outs.
All other per-site state is derived (heap IC + decoding patched words) — no second source of
truth to drift.

```
debug_vars.h:  DEBUG_BOOL(PHARO_T1_PATCHED_SENDS)          emit shape (dev opt-in)
               DEBUG_BOOL(PHARO_T1_NO_PATCHED_SENDS)       opt-out after the default flip
               DEBUG_BOOL(PHARO_T1_PATCHED_SENDS_NOLINK)   shape on, linking off (A/B lane)
               DEBUG_BOOL(PHARO_T1_PATCH_VERIFY)           post-GC mirror check
               DEBUG_BOOL(PHARO_T1_PATCH_STATS)            counters
               DEBUG_INT(PHARO_T1_PATCH_MIN/MAX, -1)       site-index bisect knobs
counters:      links, relinks, unlinks (by cause), linked hits (sampled), refusals (by gate)
```

---

## 10. PHARO_T1_PATCH_VERIFY — the mirror check, specified precisely

Walk every method's patch map after each GC (and on demand): for each `flags.linked` site —

```
decoded W0/W1 key  == icData[0]            (linked-over-zeroed slot is a FAILURE —
AND icData[1] (methodBits) != 0             it means a zeroer skipped its unlink hook;
AND extras bit 60 set, bits 59/58/55 clear  "or slot is empty" would codify exactly
AND decoded W6 == entryAddrFor(methodMap_[methodBits])   that bug)
```

Any failure: dump site, hard-fail in stress builds. This is the tripwire for matrix drift
(§7 defense-in-depth note).

---

## 11. Landing plan — dual-protocol batches, each gated

- **B0 — scaffolding, no behavior change.** Knobs; `patchMapOffset`; `SendSitePatcher` + encoder
  unit checks vs asmjit/llvm-mc; `entryAddrFor` helper; depth-aware `ScopedWriteAccess` (audited
  against all existing users); `linkSendSite` skeleton with no callers; `jit_rt_fill_ic`
  tripwire; counters; emit-bytes-hash diagnostic. *Gate:* knob-off emitted-bytes hash identical
  over a method corpus; bench 19/19.
- **B1 — shape only, never linked.** Knob-on emits §2 (impossible key, Lprobe/LmissNoX5
  restructure, tail skeleton, patch map). No patching exists anywhere. *Gate:* smoke (`3+4=7`,
  factorial, gcd); within-binary unlinked overhead < 2-3% (shape-on vs shape-off, one binary);
  60-class SUnit A/B per-test identical; `PHARO_DET_SCHED=1` AIPrim/AITarjan clean; per-site
  emitted-bytes stat recorded (zone-growth baseline).
- **B2 — linking live + the COMPLETE §7 matrix, one batch.** All §6 triggers (including the
  guarded chain-loop unlink and the eager-compile revalidation), all nine §7 events (full GC,
  flushCaches, eviction range scrub + full-flush fix, recompile re-patch + eager warm-IC link
  pass, splice gate + splice-time unlink, stale-hit). **Invalidation is not deferrable past first
  link — eviction is zone-pressure-driven, recompile targets are the hottest methods; neither
  waits for links to "age".** *Gate:* B1 suite; stress set — forced-fullGC loop during bench,
  `Integer compile:` redefinition storm on a hot linked callee, become stress,
  1 MB-zone eviction storm, Sista-splice stress; PATCH_VERIFY clean throughout; link/unlink/relink
  counters all nonzero; full-GC-stress **timing** (relink-storm check, §13 Q1) and sieve×100 /
  sort 100K sentinels unmoved; within-binary cfib/benchFib A/B (5 runs, link vs NOLINK) — honest
  expectation ~15-30% on send-dense, parity elsewhere; lldb rung: break in `linkSendSite`,
  `disassemble` a freshly linked site, verify encodings (lldb is wired up — use it).
- **B3 — breadth + soak.** dup-key relink warm path, `applyICSpecialization` interplay, method-
  install-heavy flushCaches timing gate. *Gate:* full-suite batched run ≥ the 836-class watermark,
  per-test diff vs knob-off empty; `-O0` AND `RelWithDebInfo` both (the dev-build-is-O0 lesson);
  any divergence reproduced under `PHARO_DET_SCHED=1` before touching code.
- **B4 — default-ON flip.** `PHARO_T1_NO_PATCHED_SENDS` opt-out; the generic path is permanent —
  nothing is ever removed (no all-or-nothing cliff).
- **B5 — telemetry-justified refinements.** Saveless/self-rec tail variants; MAX_IC widening via
  link-time gates (§13 Q3); batch-link queue if Q1's gate failed; selector-keyed unlink for
  prim 116/119; Lprobe-dedupe if zone pressure showed (Q2).
- **B6 — the `o class` lever (separable, mostly orthogonal).** PMS cannot move unclassified
  sends (extras==0 → C++ round trip); that lane needs **classifier/compiler coverage**: add
  inline-prim/extras classifications for quick prims starting with prim 111 `#class` (dispatch-A
  snippet: classTable lookup off the header classIndex — no patching required, benefits unlinked
  sites too), then optionally patched direct dispatch to shared per-kind frameless stubs.
  *Gate:* `/tmp/bench2.st`//tmp/bench3.st send-rate step (this is the batch with the 60M/s
  ambition, not B2 — re-aimed per review).

---

## 12. Validation ladder (binary conditions, hazard-mapped)

1. Byte-identical knob-off emit, every batch (hash harness).
2. Within-binary A/B only, for every perf claim (shape-off / NOLINK / linking are all runtime
   states of one binary).
3. Determinism: `PHARO_DET_SCHED=1` AIPrimTest/AITarjanTest ERROR=0 each batch.
4. GC: scavenge + fullGC stress with `PHARO_T1_PATCH_VERIFY=1`.
5. W^X/icache: stress knob force-relinking every linked site each `drainRecompileQueue`, bench
   suite on top — a missing flush/flip faults deterministically; plus the sieve/sort sentinels.
6. Staleness: eviction stress + post-eviction verify walk asserting no linked W6 targets a freed
   range; redefinition storm with immediate-effect check.
7. Pre-flip: full SUnit vs Cog via `scripts/classify-sunit.py`; image snapshot round-trip;
   fresh-image startup.

---

## 13. Open questions — each with a concrete probe

1. **Relink-storm cost after full GC.** v1 links immediately (one W^X round trip per link, once
   per site per GC — orders below the per-fill regime that motivated heap ICs). *Probe:* per-GC
   link counter + sieve×100/sort 100K during the forced-fullGC stress in B2's gate. *Fallback
   (pre-designed):* `PHARO_T1_PATCH_BATCH_LINK` — pending-link queue drained in
   `drainRecompileQueue` under one window, safe because `linkSendSite` is idempotent and
   re-derives from slot 0 + methodMap at drain time.
2. **Zone growth** (~35-40-instr tail on top of the retained generic J2J body ≈ +50%/site).
   *Probe:* B1 emitted-bytes stat + B2 eviction-frequency counters at default and 1 MB zones.
   *Lever:* drop the generic `tryInlineJ2J` body from Lprobe when a tail exists (poly classes of
   that site then take dispatchCached — slower but correct); only if pressure shows.
3. **MAX_IC widening for linked sends.** Link-time gating moves the per-call gate loads into C++
   — can `PHARO_T1_XMETHOD_MAX_IC` rise past 1 for the linked population specifically? *Probe:*
   `=4` under DET_SCHED, 60-class A/B, NOLINK vs linking, plus the materialize-bail stress.
4. **Keep links across full GC** — resolved **NO** for v1 (classIndex stability is an accident of
   the monotonic class table, not a guarantee; registerClass tripwire comment required before any
   revisit). Revisit only with Q1 churn data in hand.
5. **Immediate-receiver links** (key hi16 = the immediate marker): SmallInteger-heavy selectors
   mostly resolve in Lprobe's inline-prim prechecks anyway. *Probe:* refusal-by-gate counter
   split by key kind; link them only if the counter says they're hot.
6. **Starvation in profile-guided paths.** §8 inventories the readers; *probe:* dumpICHistogram
   delta run with/without linking + recompile-quality spot checks (late-spec bit application
   rate).
7. **B6 stub shape** (shared per-(kind,nArgs) frameless stubs need a per-site resume address and
   payload without re-adding load chains). *Probe:* prototype prim-111 classification first (no
   patching), measure; only design the stub tier if the classifier alone doesn't close bench2/3.
8. **Selector-keyed unlink for prim 116/119** (today: sledgehammer). *Probe:* flushCaches
   frequency × numPatchedSites in an Epicea-heavy run (B3 gate); build only if it shows.

---

## 14. Pre-existing bugs surfaced by this work — file/fix regardless of PMS

1. **Chain-loop stale-IC zeroing writes through unguarded raw `state.icDataPtr`**
   (Interpreter.cpp:~23582-23594) — no ownership guard (write-after-free into a reused heap
   buffer); AND under `PHARO_T1_IC_POLY_WALK` the probe advances x5 by 24/48 on a slot-1/2 hit,
   so the 18-word zeroing from an advanced pointer clobbers the site's selectorBits/hitCount and
   the next site's first key word. Fix = the §6 guard + floor-to-site (shipped with B2's hook).
2. **Eviction full-flush fallback** (JITCompiler.cpp:~2461-2483) does not scrub surviving pinned
   methods' J2J extras, and **neither eviction path scrubs `megaCache_.jitEntry`** — stale code
   addresses to freed zone space survive until the next fullGC/flushCaches. Fixed in B2's event-4
   commit.
3. **T2 ICs are not flushed on the prim-116 path** (only on GC) — a T2 class→method binding
   survives image-side redefinition. Out of PMS scope; recorded.
4. **Stale comment** AsmjitT1.cpp:3897 claims poly-walk is opt-out (`PHARO_T1_NO_IC_POLY_WALK`);
   the knob is opt-in (`PHARO_T1_IC_POLY_WALK`, DebugSettings.cpp:246). Fix the comment.
5. **Verify:** `upgradeICToJ2J`'s eager compile can trigger eviction of the owner JM before its
   own heap-IC write (no re-validation after `compiler_->compile()` today). Confirm the owner is
   pinned by the eviction pin pass, or add the §6 re-validation to the existing code path too.

---

## 15. Review-finding traceability

```
minimal-risk review   C1 eviction-unlink batching        -> §7 ev.4 + §11 B2 (one batch, rationale)
                      C2 recompiled methods never link   -> §7 ev.3 eager warm-IC link pass
                      C3 o-class bench mistargeted       -> §1 honest model + §11 B6
                      H1 stale-hit unguarded unlink      -> §6 chain-loop hook + §14 #1
                      H2 latency overstated              -> §1 (15-30%, store half untouched)
                      H3 reach asserts                   -> §5 reach rules: fallback, never assert
                      M1 compile-time #define            -> §0 runtime emit knob
                      M2 fill_ic / bit-55 / slot-0 flag  -> §6.1, §5 (bit 55), §6 (wrote-slot-0)
                      M3 no re-validation on linked hits -> §7 note + §10 VERIFY
                      M4 whole-zone flush / W^X comments -> §4 ranged flushes, §7 ev.1/ev.5
                      L1-L4                              -> §8 counters, §9 labels-only, §2.3, §11 B1
cog-faithful review   H1 codeStart()==entry invariant    -> §0 (prologue design rejected for this)
                      H2 W^X nesting                     -> §4.1 depth-aware ScopedWriteAccess
                      H3 MAX_IC population cap           -> §1 honest framing + §13 Q3
                      M1 BLOCK_VALUE bit 59              -> §5 structural refusal
                      M2 miss-handler staleness          -> no new miss handler; §6 revalidation
                      M3 PIC lifecycle                   -> §0 PICs dropped from v1
                      M4 tier-up relink crash            -> §7 ev.3 hard rule (unlink on tier change)
                      M5 signal handlers                 -> §4.2 audited + invariant
                      M6 starvation inventory            -> §8 + §13 Q6
                      L4 compact()                       -> §8 assert
                      L5 class-index reclamation         -> §7 ev.1 + §13 Q4 tripwire
perf-first review     BLOCKER-1 LINKED-GEN               -> §0/§3 dropped
                      BLOCKER-2 Sista splice             -> §7 event 8 (verified anchors)
                      HIGH-3 zeroing trigger             -> §6 guard + floor-to-site
                      HIGH-4 classIndex stability        -> §7 ev.1 permanent unlink-all + tripwire
                      MEDIUM-5 relink storms             -> §13 Q1 measured gate + queue fallback
                      MEDIUM-6 zone growth               -> §13 Q2 + mandatory-complete ev.4 walk
                      MEDIUM-7 VERIFY codifies a bug     -> §10 (linked-over-zeroed = FAIL)
                      LOW-8 store order                  -> §4 step 4 W1-last contract
                      LOW-9 jit_rt_fill_ic               -> §6.1 tripwire
                      LOW-10 entryAddrFor                -> §4 step 2 shared helper
```

Key files: `src/vm/jit/asmjit/AsmjitT1.cpp` (site/tail/cold-block emit, label→patch-map fill,
no-BL assert), `src/vm/jit/SendSitePatcher.{hpp,cpp}` (new), `src/vm/jit/JITMethod.hpp`
(`SendSitePatch`, `patchMapOffset` in `_pad_76`), `src/vm/jit/JITRuntime.cpp` (`linkSendSite`/
unlink walks, `entryAddrFor`, hooks in `recoverAfterGC`/`flushCaches`/
`rewriteIcEntriesAfterRecompile`/splice-set sites, `jit_rt_fill_ic` tripwire),
`src/vm/jit/JITCompiler.cpp` (patch-map sizing, eviction scrub + full-flush fixes, eager
warm-IC link pass), `src/vm/jit/PlatformJIT.hpp` (depth-aware `ScopedWriteAccess`),
`src/vm/Interpreter.cpp` (trigger hooks in `patchJITICAfterSend`/`upgradeICToJ2J`, guarded
chain-loop unlink), `src/vm/debug_vars.h` (knobs).