# Frame-State Residency (FSR) — definitive design, T1 arm64 JIT

Status: SYNTHESIS of three independent designs (lazy-exit-materialization,
cog-faithful-frame-slots, callee-self-derivation) and their adversarial
reviews. Skeleton = callee-derive's core mechanism (x19 invariant +
derive-at-boundary + coherence-not-freshness GC), grafted with lazy-exit's
register plan (x23 cursor — already trampoline-pinned) and frame-slots'
no-save-layout-change framing, doorbell preemption, and money-first batch
order. Every reviewer finding is resolved in-line (traceability table, §12)
or carried as an open question with a concrete probe (§13).

Facts re-verified against HEAD `124877dd` on 2026-06-11 (settling reviewer
disagreements):

- `grep -c 'a\.blr(' AsmjitT1.cpp` = **23 sites** (348, 384, 2518, 2640,
  2765, 2989, 3940, 4626, 4908, 5114, 5320, 5358, 5866, 5894, 5935, 5967,
  5997, 6032, 6487, 6523, 6761, 7007, 7234). The reviews' "10 audited
  BLRs" / "~19" / "~35" counts were all wrong; the WIP.md:400-405 citation
  is dead. The classification table is regenerated mechanically in M0.
- The literals **+8 bug is real and present in BOTH protocol arms**:
  `JITRuntime.cpp:890` (V2) and `:899` (V1) write
  `literals = compiledMethodOop + 8`; every other writer (and the
  trampoline at `TrampolineAsm.S` Lcall path, `add x15, x14, #16`) uses
  +16. Latent only because nothing reads literals on that path before a
  refresh — the strongest in-tree evidence that the literals mirror is dead.
- The trampoline call path **never establishes x19**: `Lcall_enter_callee`
  (TrampolineAsm.S:476-484) stores callee JM to `JS_JITMETHOD` then
  `blr x7` with no `mov x19, x8`. x19 still holds the trampoline-entry
  hoist (S.169-170). Harmless today (xmethod paths read the mirror);
  fatal under FSR. Fix is mandatory in M1.
- `JIT_CALL` (JITState.hpp:288-322) clobbers x19/x20/x21/x22/x25/x26 but
  **NOT x23/x24/x27/x28**, and bakes raw offsets `#208/#56/#0` plus an
  unconditional `ldur x26, [x25, #-8]` TOS live-in. Cursor-in-x23 for
  emitted code therefore REQUIRES macro changes (hoist + live-out +
  clobber), and tempBase-in-x26 is compile-time-only.
- Trampoline prologue (S.128-170) saves x19-x28 and pins x21=state,
  x22=save base, x23=cursor, x24=localFrameDepth, x27=base+MAX_J2J_BYTES,
  x28=nil bits; the V2 return pop (S.190-232) already maintains x23 and
  defensively syncs it to `JS_J2J_SAVE_CURSOR`.

## 0. Verdict and core principle

The JITState per-activation mirror (jitMethod/method/literals/argCount/ip
written per cross-method inline-J2J call; 4 fields rewritten per resume) is
the single largest removable block in the 83-insn linked round trip. Cog
writes none of it: the frame IS the state, and C++/GC consumers derive
method/numArgs/bcpc on demand. FSR adopts that split with our constraints:

- The **J2J save pool (V2, 32B, unchanged) + JITMethod zone headers +
  operand stack remain the canonical state** for suspended activations —
  they already are: `{sp, receiver, tempBase, packedResumeAddr}` derives
  jm (findMethodByPC), ip (bcStart+bcOff), nArgs (Interpreter.cpp:22029-22058).
  **No save-layout change.** FSR is a writer-discipline change only.
- The **active** activation's identity lives in registers:
  x19 = JITMethod* (invariant), x25 = sp (done), x23 = save cursor (new),
  and — late, gated — x20 = receiver, x26 = tempBase.
- Derivable fields (method, literals, ip, argCount, jitMethod mirror)
  become **exit-only scratch**: written by C++ at entry edges and by
  publish points at every JIT→C++ edge; never by per-call emitted code.
- **Receiver and tempBase mirrors are RETAINED per-call in the core
  batches** (the 1dbc538b −25% lesson: a register MIRROR of a
  still-maintained field loses; a register that IS the state, published
  only at exits, is the sp-in-x25 shape that won 42→29 ms). They migrate
  to x20/x26 only in late, severable, measurement-gated batches.

The discipline test for every batch: the register replaces a per-call
WRITE that already exists, so the exit publish is work already being done
— never a new mirror plus new sync.

## 1. Register plan

```
reg   today                                    under FSR
x0    JITState* live-in                        unchanged
x19   state.jitMethod hoist (stale in J2J      INVARIANT: active JITMethod*, always (M1).
      callees — the xmethod hole, 4064)        mov x19,<calleeJM> at activations; adr/sub at
                                               resumes (kept); JIT_CALL/trampoline hoists kept
x20   j2jDepthInc constant (depth RMW)         freed by M3; RECEIVER in M5 (gated)
x21   JITState* (trampoline)                   unchanged
x22   save base (trampoline)                   unchanged
x23   save cursor (trampoline-only)            live cursor, owned by emitted code too (M2)
x24   localFrameDepth (trampoline)             unchanged
x25   sp (contract done, 42->29 ms)            unchanged
x26   TOS mirror (simStack, opt-in)            TEMPBASE in M6 (compile-time, gated, #error vs TOS)
x27   save limit constant (trampoline-only)    stays trampoline-internal. NOT a protocol register:
                                               the limit is the preemption DOORBELL and must stay
                                               a per-call memory load (see §9)
x28   nilOop bits (trampoline-only)            read by emitted nil-fill (M2 freebie, optional)
```

## 2. The protocol — who writes what, when

```
field          C++ entry writes      during JIT execution                 at JIT->C++ edge (publish)
sp             yes + x25 hoist       x25 only (done)                      str x25 (done; macro live-out)
receiver       yes (+x20 hoist M5)   core: per-call str KEPT              core: already current
                                     M5: x20 IS the state                 M5: str x20 (exits + GC-BLR brackets)
tempBase       yes (+x26 hoist M6)   core: per-call str KEPT              core: already current
                                     M6: x26 IS the state                 M6: str x26
jitMethod      yes + x19 hoist       x19 IS the state; call: mov x19;     str x19 (exit stubs + JIT_CALL
                                     resume: adr/sub (existing)           live-out + GC-BLR brackets)
method         yes (compat)          NEVER written, never read            funnel derives jm->compiledMethodOop
literals       yes (compat)          NEVER written; reads via x19->       nothing (funnel recompute for compat;
                                     literalsCache (new JM field)         afterGC 18425 recompute retained)
ip             yes                   NEVER written per-call               exit stubs + GC-BLR brackets derive:
                                                                          ldr bcStartCache(x19); add #bcOff; str
argCount       yes                   NEVER written, never read            funnel derives jm->argCount
j2jSaveCursor  yes + x23 hoist       x23 IS the state                     str x23 (exits + macro live-out +
                                                                          GC-BLR brackets)
j2jSaveLimit   yes                   per-call ldr (DOORBELL — never       n/a (heartbeat/checkpoint smashes it)
                                     hoisted into a register)
j2jDepth       dead                  derived: (cursor-base)/JSV_SIZE      funnel helper j2jDepthFromCursor()
j2jEntryDepth  -> j2jEntryCursor     prelude compares cursor vs it        C++ sets per entry episode
j2jTotalCalls  dead (see §9)         none                                 trampoline localCalls writeback kept
```

Core invariants:

- **I1 (identity)**: x19 == JITMethod* of the currently executing method at
  every instruction of emitted code. Establishment dominates every bail
  edge of the new activation and no bail edge of the old one (CR-F5 rule:
  on the generic path, the `mov x19` sits AFTER the pool-full check at
  5373 and the gate bails at 4770-4843/4969-4970, at the activation
  commit point).
- **I2 (coherence, not freshness)**: at every point C++ can run, JITState
  holds a coherent `{jitMethod, ip}` pair written together from the same
  x19. Freshness is NOT required between publishes; coherence is. This is
  what makes GC anchoring sound (§7) and resolves LR-F1 vs CR-F3.
- **I3 (publish set)**: every JIT→C++ edge — exit stub, GC-capable BLR,
  trampoline-internal C call — publishes the full live-register set that
  C++ or GC may consume: {ip, x19, x23} core; +x20 in M5; +x26 in M6.
- **I4 (entry set)**: every C++→JIT edge hoists the same set from
  JITState. A missed entry is a wild-register bug; the enumeration is
  §6.2 and is grep-regenerated, not hand-maintained (CR-F8).

## 3. Per-call instruction ledger (linked PMS round trip, 83 insns)

A = send head 14 (untouched — explicitly out of scope, see §11),
B = call tail 39, D = return prelude 17, E = resume continuation 13.
Anchors are HEAD AsmjitT1.cpp lines; "batch" = where the line dies.

```
site  line       insn                                fate    replacement / note
B     4204-06    movz/movk JM-immediate feeders      M4      callee JM arrives in x10; x19 = mov
B     4209       ldp cursor,limit                    M2      ldr limit only (doorbell); cursor = x23
B     4221       ldr OFF_RECEIVER (save input)       M5      x20 (until M5: retained)
B     4223       ldr OFF_TEMPBASE (save input)       M6      x26 (until M6: retained)
B     4228       ldr x13,[x10,#cmOop]                M4      fed only the dead stores
B     4229       str x10,OFF_JITMETHOD               M1      mov x19,x10 (count wash; enables all)
B     4230       str OFF_METHOD                      M4      funnel derives at exit
B     4231       add x13,#16                         M4      —
B     4232       str OFF_LITERALS                    M4      reads go x19->literalsCache
B     4233       mov w13,#nArgs                      M4      —
B     4234       str OFF_ARGCOUNT                    M4      funnel derives jm->argCount
B     4235       str cursor                          M2      stp [x23],#32 post-index owns the bump
B     4236-38    ldr/add/str depth RMW               M3      deleted; depth derived from cursor
B     4239       str OFF_RECEIVER (callee)           M5      mov x20,x1 (wash)
B     4241       str OFF_TEMPBASE (callee)           M6      sub x26,x2,#nArgs*8 (wash)
B     4242       ldr bcStartCache                    M4      GC in-bounds invariant moves to publishes
B     4243       str OFF_IP                          M4      —
B     4252       mov nilBits                         M2opt   x28 (already trampoline-pinned)
D     3231-32    ldr depth + ldr entryDepth          M3      single ldr j2jEntryCursor (stable field)
D     3238       ldr cursor                          M2      x23
D     3240       str cursor                          M2      pre-indexed ldp pop owns the decrement
D     3242       str depth                           M3      —
D     3248       str OFF_RECEIVER                    M5      pop into x20
D     3263       str OFF_TEMPBASE                    M6      pop into x26
D     3273       str wzr,OFF_EXIT                    KEEP    1 free store; OQ3 (audit before deleting)
E     7183-84    adr/sub x19                         KEEP    PC-relative self-identification
E     7185       str OFF_JITMETHOD                   M4      x19 is current; macro live-out publishes
E     7186-91    ldr cm / str method / add /         M4      funnel derives at next exit
                 str literals / mov / str argCount
```

Generic-path extras: `ldr [x0,#OFF_JITMETHOD]` in emitMaterializeX5
(4064-4071) collapses to x19 unconditionally (M1 — kills one level of the
state→jm→icBuffer→entry→cmp probe chain); duplicate calleeCM ldr (5505);
the redundant self-rec E2 mirror block (5500-5511) dies entirely (M4); the
saveless stash (4988-5238) shrinks from the 96-byte 8-field form to
{x30, x19, j2jEntryCursor pin, receiver/tempBase as applicable} (M4, see
§6.4); the trampoline J2J-convert mirror stores (S.250-258, 448-454) die
in M4.

Net counts (A=14 constant): M1 ±0 linked / −1 generic; M2 −3; M3 −5;
M4 −17; M5 −3; M6 −3. Cumulative: **83 → ~52** linked. Count is NOT the
claim — see §11 for which deltas are predicted to measure.

## 4. Coverage matrix — every consumer of every removed store → new source

Format per field: consumer (anchor) → new source. GC, exit, and NLR
consumers called out explicitly.

**OFF_METHOD per-call/resume stores (4230, 7187, E2, trampoline)**

```
exit/bail stubs building ip (ldr OFF_METHOD pattern,    ldr [x19,#offsetof(JITMethod,bcStartCache)];
 e.g. 3091-3093, 3249-3255)                              add #bcOff (cost-identical, no STLF)
C++ post-exit: super-send 23626, pendingICOwnerMethod_   funnel syncDerivedFromJM(): state.method =
 23666/23763, foreign-site guard 23712, upgradeICToJ2J   Oop::fromRawBits(jm->compiledMethodOop) —
 22715/23909, method_/homeMethod_ chain-loop syncs       the chain loop already does this at
 (23003/23222/23422)                                     23003/23222/23422; now single-sited
GC prepareForGC/afterGC ip<->offset round trip           re-anchored on state.jitMethod (coherent
 (18241-18248, 18421-18426)                              with ip per I2/I3); in-bounds assert added
GC forEachRoot visit of state.method (Hpp:3495)          KEPT (stale-but-LIVE: zone walk anchors
                                                         every zone CM via jm->compiledMethodOop,
                                                         Hpp:3364-3372; zone eviction while frames
                                                         live is already banned) — zero-risk visit
Sista bail readers (11046+, 18959+) via T2/JITRuntime    funnel runs at the MACRO boundary (§6.2),
 paths that never reach the two named funnels (CR-F2)    so every macro-entered episode is covered
```

**OFF_LITERALS stores (4232, 7189, E2, trampoline)**

```
pushLiteral 3005 / pushLitVar 3014 / inline-spec         ldr xT,[x19,#offsetof(JITMethod,
 literal compares 7755/7762/7803                          literalsCache)]; ldr lit,[xT,#i*8].
                                                          NEW JM field literalsCache = cmOop+16,
                                                          refreshed by the same zone walk that
                                                          refreshes bcStartCache (Hpp:3369). Same
                                                          2-load depth; first load from a stable
                                                          zone-header address (no STLF) instead of
                                                          a per-call-forwarded mirror
C++ post-exit readers                                     NONE exist (verified by all three designs;
                                                          the +8 bug at JITRuntime.cpp:890/899 is
                                                          the existence proof). Funnel recompute +
                                                          afterGC 18425 retained for migration compat
```

**OFF_IP per-activation store (4242-4243, 5574-5579)**

```
its ONLY purpose: GC in-bounds invariant for a stale     invariant holds BY CONSTRUCTION: GC runs
 mirror at unbracketed GC points                          only in C++; every JIT->C++ edge (exit
                                                          stubs AND GC-capable BLR brackets, §6.3)
                                                          publishes a coherent {x19, ip} pair.
                                                          prepareForGC asserts in-bounds (loud trap
                                                          replaces silent corruption forever)
exit-stub ip writes                                       KEPT, re-sourced from x19 (above)
C++ ip readers: instructionPointer_ syncs, *state.ip     unchanged — they read the stub/bracket-
 re-decode 22742/23620, Sista bails                       published value, same semantics as today
```

**OFF_ARGCOUNT stores (4234, 7191, E2)**

```
in-execution readers                                      NONE (verified)
chain-loop argCount_ sync (23066/23233/23433)             funnel: state.argCount = jm->argCount
                                                          (precedent: restores at 22917,
                                                          JITRuntime.cpp:885/894). Block JMs carry
                                                          their own argCount. FR-7 guard: a VERIFY
                                                          exit assert (entry-edge-written argCount ==
                                                          jm->argCount) must run a full soak BEFORE
                                                          the stores are deleted — block/DNU/perform
                                                          entry edges write it independently
```

**OFF_JITMETHOD per-call store (4229, 7185, E2)**

```
emitMaterializeX5 memory reload (4064-4071, xmethod-on)  x19 unconditionally (M1)
saveless stash copy (restores OFF_JITMETHOD at 5215)     stash keeps it AND restores x19 (FR-6)
C++ post-exit: chain loop, materialize fallback           published by: exit stubs (str x19, 1 free
 (22035-22042), tier checks, upgradeICToJ2J               store), the JIT_CALL/JIT_RESUME_CALL
                                                          live-out (str x19 next to the existing
                                                          str x25 — covers all macro episodes,
                                                          CR-F2), and GC-BLR brackets (§6.3)
GC: in-flight CM reachability                             zone walk visits every jm->compiledMethodOop
                                                          (verified Hpp:3364-3372) — independent of
                                                          any state field; x19 points into the zone,
                                                          which does not move during GC
```

**Save-cursor memory residency (ldp 4209 / str 4235 / 3238 / 3240)**

```
emitted push/pop                                          x23: stp ...,[x23],#32 / ldp ...,[x23,#-32]!
limit check                                               per-call ldr OFF_J2J_SAVE_LIMIT; cmp x23
                                                          (doorbell preserved, §9 — NEVER hoisted)
GC pool receiver scan bound (Hpp:3498-3500;               brackets publish x23 BEFORE any GC-capable
 prepareForGC takes max(j2jPoolCursor_, state cursor)     BLR (CR-F1: the A6 stale-receiver corruption
 at 18261-18272 for in-JIT GCs)                           otherwise reappears); slice RESERVATION
                                                          (j2jPoolCursor_ = base + maxDepth,
                                                          19577/22435) is representation-independent
                                                          and unweakened (LR-F10)
C++ cursor movers: chain-loop push/pop 22695-22970,       macro live-in re-hoists x23 on every
 19770-19859, materialize drain/reset (23175, 24175,      re-entry; trampoline already maintains
 24415 wedge-comment sites), jit_t2_send 804/860,         x23 natively (S.152, 204, 423). BLR
 block-value prep 1923-1948                               helpers that move the cursor are bracket
                                                          sites (criterion, §6.3): reload x23 after
trampoline                                                native x23 owner — no change
```

**Depth RMW (4236-4238, 3231/3232/3242) + j2jEntryDepth**

```
return-prelude unwind decision                            ldr j2jEntryCursor (NEW field, stable);
                                                          cmp x23 — no forwarding hazard
spDepthCheck / C++ depth consumers (BcDepthMap            funnel derives j2jDepth = entryDepth +
 305-410, chain loop)                                     (cursor - entryCursor)/JSV_SIZE
                                                          (saveless calls never bumped depth, so
                                                          equivalence holds)
every j2jEntryDepth writer must be mirrored for           THE M3 CHECKLIST (LR-F4, grep-verified):
 j2jEntryCursor                                           zeroers Interpreter.cpp 10905, 14579,
                                                          18993, 19438, 22445, 23177, 24177, 24415;
                                                          pinners 19893-19896, 22872-22878,
                                                          JITRuntime.cpp:2358-2363, AsmjitT1.cpp
                                                          5015-5028 (saveless stash pin — see §6.4);
                                                          C++ movers jit_t2_send 806/861, block-
                                                          value prep ~1948. Miss a zeroer = prelude
                                                          wedge; miss a pinner = over-pop corruption.
                                                          VERIFY derives depth both ways and traps
                                                          on divergence at every exit
j2jTotalCalls charging input (22995-22999)                §9 decision + OQ1
```

**Receiver mirror (M5 only: 4239 str, 4221 ldr, 3248 str, prelude/trampoline pops)**

```
in-execution: pushReceiver 3030, recvVar 2991/2995,       x20 directly (head of the send critical
 returnReceiver 3390, prim prologues 2387/2406,           chain: recv -> tag check -> class key ->
 storeRecvVar 3153, inline-spec 7747/7813,                dispatch — this is the chain-kill)
 save-push input 4221/5414
C++ post-exit (receiver_ syncs 23062/23229/23429/         exit stubs + GC-BLR brackets publish
 23925, syncGlobals 20712)                                str x20; C++ reads unchanged
GC roots                                                  state.receiver fresh at every GC point
                                                          (publish set); pool save.receiver pushed
                                                          from x20, pool walk unchanged; after any
                                                          ALLOCATING BLR emitted code RELOADS x20
                                                          from state.receiver (object may have moved)
C++ re-entry writers (23370/23964/24058, JITRuntime       keep writing the field; all entry edges
 block-value prep 1922/1968, jitSistaSelfRecCall)         hoist x20 (§6.2)
block activations                                         receiver exists ONLY in x20 + state.receiver
                                                          ([tempBase-8] holds the closure) — the
                                                          audit must never lean on an operand-stack
                                                          copy as fallback root (LR-F2 note)
```

**tempBase mirror (M6 only)** — x26; pointers into the C++ stack, GC-stable,
no post-BLR reload. Trampoline pops (S.217-218, 437-440) must restore x26,
the JIT_CALL `ldur x26,[x25,#-8]` TOS line conflicts (compile-time #error
vs TOS, extending AsmjitT1.cpp:73-78), and the knob is compile-time because
the macro is baked asm (CR-F6). Gated on a send-heavy/SUnit-shaped bench,
not cfib alone — the per-exit `str x26` tax on exit-heavy workloads is the
1dbc538b shape (CR-F6); the difference is the per-call str/ldr and the
per-temp-access load are REMOVED, not duplicated.

**NLR / exception / terminate** — UNCHANGED, verified by all three reviews:
walkers consume `savedFrames_` only; all pending saves are materialized
(23049/23201/23416/24206) before the interpreter takes over;
`materializeJ2JSaveIntoFrame` (22021-22144) reads save fields +
findMethodByPC(resumeAddr), and its one live input — the state.jitMethod
fallback for null-resumeAddr/self-rec saves — is published by the exit that
necessarily preceded materialization (and by brackets under BLRs). The
nested-valueWithExit innermost-match bug and the null-resumeAddr deep-chain
misattribution are pre-existing and FSR-neutral (OQ5 documents why, so the
M4 soak doesn't get blamed for them).

## 5. Exit protocol

Universal exit/bail epilogue (replaces the `ldr OFF_METHOD; add; str OFF_IP`
pattern at ~60 stub sites):

```
ldr  xT, [x19, #offsetof(JITMethod, bcStartCache)]
add  xT, xT, #bcOff
str  xT,  [x0, #OFF_IP]
str  x19, [x0, #OFF_JITMETHOD]        ; M4 (free, off-chain)
str  x23, [x0, #OFF_J2J_SAVE_CURSOR]  ; M2
str  x20, [x0, #OFF_RECEIVER]         ; M5
str  x26, [x0, #OFF_TEMPBASE]         ; M6
<exitReason + sp per existing x25 contract>
```

+3-4 stores per stub vs today, all off-chain, executed only when the bail
is taken — stubs are off the J2J hot path; that is the entire bet. Code-size
mitigation if zone pressure shows (the 63ead0d9 hot-method-compile-failure
class): shared per-method exit-publish thunk (`movz wT,#bcOff; b thunk` is
net SMALLER than today's 3-insn inline form). Exit-specific operands
(returnValue, sendArgCount, selector, IC site index, prim error code) are
unchanged. The per-exit-reason consumer walk (ExitReturn / Send / SendCached
/ J2JCall / BlockCreate / ArrayCreate / ArithOverflow / MustBool / Yield /
PrimFail / StackOvfl / Sista bails) was performed independently by all three
designs against this template: no handler reads a removed field given the
publish set + funnel derivations.

## 6. Entry/edge enumeration (grep-regenerated in M0 — never hand-trusted, CR-F8)

### 6.1 The funnel

`syncDerivedFromJM(state)`: method = jm->compiledMethodOop; literals =
cmOop+16 (compat); argCount = jm->argCount; j2jDepth = entryDepth +
(cursor−entryCursor)/JSV_SIZE. It runs at the MACRO boundary — a thin
wrapper around JIT_CALL/JIT_RESUME_CALL — not at hand-listed call sites.
This resolves CR-F2's 7x undercount structurally: macro sites
(Interpreter.cpp 19721, 19895, 22876, 23405, 24194, 24421, 24622;
JIT_RESUME_CALL 19999, 22970; JITRuntime.cpp 845, 2361, 3936) are covered
by construction; the raw T2 entries (JITRuntime.cpp:843, 3930) bypass the
macro and are audited + converted in M0.

### 6.2 C++→JIT entry edges (hoist set: x19, x23 [, x20 M5] [, x26 M6])

```
JIT_CALL macro            JITState.hpp:288-322   add hoists + live-outs + clobbers for x23
                                                 (and x28 if the nil-fill freebie lands);
                                                 replace raw #208/#56/#0 with offsetof-
                                                 checked constants (static_assert lockstep)
JIT_RESUME_CALL           JITState.hpp (below)   same
trampoline loop entry     TrampolineAsm.S:163-170  x19 hoist exists; x23 native
trampoline V2 return pop  TrampolineAsm.S:213-232  resume continuation self-identifies x19
                                                 (adr/sub); pop feeds x20/x26 under M5/M6
Lcall_enter_callee        TrampolineAsm.S:476-484  ADD mov x19, x8 (BOTH self-rec and cross
                                                 arms) — the verified FR-1 blocker fix
Ltramp_convert route      inherits Lcall path     covered by the same fix
pharo_jit_osr_resume      TrampolineAsm.S:~561    add hoists
null-cursor mode          tryJITResumeInCaller    state cursor/limit = nullptr (19434-19436):
                          Interpreter.cpp:19434   hoist x23=0 UNCONDITIONALLY — cmp 0,0; b.hs
                                                 preserves the always-bail semantics (LR-F5);
                                                 skipping the hoist leaves a stale cursor
```

### 6.3 GC-capable BLR brackets

Criterion (union of LR-F2 + FR-2): a helper qualifies if it **can GC, OR
moves the save cursor, OR runs nested sends/materialization, OR reads or
writes any lazified field** (e.g. jit_rt_sync_globals at 7234,
pharo_jit_convert_send, and the trampoline's Ltramp_call JS_IP decode at
S.329-336 — those need an exact published ip, not merely in-bounds).
Worklist = the 23-site grep above, classified per-helper in a table checked
into this doc's companion (M0 deliverable). Bracket:

```
PRE:   str ip (ldr bcStartCache(x19); add #bcOff; str)   ; coherence (I2) — this is what
       str x19 ; str x23 [; str x20]                     ; makes jm-anchored GC sound and
POST:  ldr x23 from state.j2jSaveCursor                  ; resolves LR-F1 + CR-F3 together
       [ldr x20 from state.receiver]   ; allocating helpers only — object may have moved
```

Saveless-gated blr callees are exempt by the canSkipJ2JSave proof
(no GC, no yield, no exit — 4955-4957). VERIFY net: record gcEpoch + cursor
at every BLR entry; trap at return if either changed at an unbracketed site
(FR-2). Cost: +4-6 insns at allocation-class sites only — off the J2J path.

### 6.4 Saveless stash (4988-5238)

Shrinks from the 96-byte 8-field stash to: **{x30, caller x19,
j2jEntryCursor pin [, caller x20] [, caller x26]}**.

- The x19 slot is mandatory: under M4 a literal-pushing caller resumes
  after the blr and reads via x19; the stash already restores
  OFF_JITMETHOD at 5215 — restore the register too (FR-6).
- The **j2jEntryCursor pin is mandatory**: AsmjitT1.cpp:5015-5028 pins
  j2jEntryDepth across the blr precisely so a leaf callee's prelude does
  not pop the CALLER's pending self-rec save and leak the sp-stash to the
  guard page (the Eδ.2d launch crash). Under M3 the same failure recurs
  via the cursor compare. Save old j2jEntryCursor; `str x23 →
  j2jEntryCursor` before the blr; restore after (LR-F3).

## 7. GC story

- **GC runs only in C++**, reached via an exit stub, a bracketed BLR, or
  the trampoline's C calls — all publish per I3. Therefore at every GC
  point: `{jitMethod, ip}` is coherent (I2); receiver is fresh
  (state.receiver per-call-written in core; x20-published in M5);
  tempBase values are C++-stack pointers (GC-stable).
- **Anchor = state.jitMethod**: prepareForGC derives the bytes base from
  `jm->compiledMethodOop` (coherent with the published ip); afterGC
  rebuilds ip from the same. A loud **in-bounds assert** in prepareForGC
  (under VERIFY, then permanent) converts any missed publish site into a
  deterministic trap — this replaces the deleted per-activation
  `str OFF_IP = bcStartCache`, whose only job was keeping a stale mirror
  in-bounds.
- **state.method stays a visited root** (Hpp:3495): stale-but-LIVE is
  safe — the zone walk visits and updates every JITMethod's
  compiledMethodOop and refreshes bcStartCache (verified Hpp:3364-3372),
  so every zone CM survives and remaps; CMs die only with zone eviction,
  and evicting a JITMethod with live frames (resumeAddrs, x19) is already
  banned. Add the zone-sweep-entry assert. Keeping the visit is zero-risk
  and removes the LR-F1 wild-root hazard without depending on freshness.
- **Pool receiver scan** (Hpp:3498-3500) is bounded by the published
  cursor; prepareForGC's max(j2jPoolCursor_, state cursor) logic
  (18261-18272) plus the BLR-bracket x23 publish closes the CR-F1 A6
  reappearance. Slice reservation (19577/22435) is representation-
  independent.
- **literals** are stale between GCs by design (zero readers); reads go
  through literalsCache, refreshed by the zone walk — moved CMs handled
  with zero register fixup, since x19 points into the non-moving zone.

## 8. Why this synthesis (skeleton choice + grafts)

- **callee-derive core**: cleanest mechanism (x19 + derive-at-boundary +
  receiver/tempBase retained), survived review with "no finding kills FSR
  itself". Its funnel-undercount and bracket gaps are fixed by the macro
  live-out/funnel (its own reviewer's suggestion) and the full publish set.
- **lazy-exit graft**: the x23/x27/x28 observation (cursor residency rides
  registers the trampoline ALREADY pins and maintains, S.139-170/204/423)
  beats frame-slots' x20-cursor, which its review showed needs 6 hoist
  sites plus a baked-asm offset dodge — and x20 has a better use (receiver,
  once M3 frees it). lazy-exit also contributes the entry-glue and
  exit-site enumerations and the j2jEntryCursor prelude design.
- **frame-slots grafts**: no-save-layout-change framing (JSV stays V2/32B —
  the migration is writer-discipline only, far smaller than the V2 flip);
  the doorbell preemption analysis (§9); the within-binary A/B knob
  structure; and its review's two structural rules adopted wholesale:
  money-first batch order, and verify-by-dual-write (a method-entry
  `cmp x19, state.jitMethod` canary is self-contradictory under FSR, FR-4).
- **Rejected**: receiver-from-frame-slot derivation (breaks for blocks —
  closure at tempBase−8 — and costs a 2-load chain; Cog itself keeps a
  FoxMFReceiver slot); sentinel-save depth elimination (changes pool/
  materialize-walk semantics; entryCursor compare is strictly simpler);
  limit-in-register (kills the preemption doorbell, §9); save shrink to
  24B and adr-derived x19-free exits (real, but follow-ons — out of scope).

## 9. Preemption and charging (the depth-RMW replacement)

The deleted per-call depth/totalCalls RMW served two masters:

1. **Preemption** of call-heavy recursion (fib has no backward jumps).
   Mechanism preserved EXACTLY: today's emitted per-call check already
   loads the limit from JITState (the ldp at 4209); under M2 it remains a
   per-call `ldr OFF_J2J_SAVE_LIMIT; cmp x23` — a load from a
   rarely-written address (no STLF, off the dependent chain, feeds only a
   branch). The heartbeat (or DET_SCHED checkpoint) smashes
   `j2jSaveLimit = saveBase` → next call bails (the existing
   `j2jSaveLimit = nullptr` "inline-J2J off" idiom, Interpreter.cpp:10900,
   with a new trigger) → scheduler check → C++ restores the limit.
   **The limit is never hoisted into a register** (FR-3/FR-5 rule); x27
   stays trampoline-internal, and Ltramp_call additionally reloads the
   limit from JS_ once per hop (1 cold ldr) so trampoline chains become
   smashable too — today they are not (verified: S.155-156 constant),
   so this strictly improves preemption coverage.
2. **Charging** (j2jTotalCalls*10 at 22995-22999). Cursor-delta cannot
   reconstruct burst counts (2^n calls at depth n). Decision: keep the
   trampoline's localCalls counter ([sp,#120], written back at exit —
   zero new cost) for trampoline chains; PMS-linked emitted chains charge
   jm->numBytecodes at exits (19537, unchanged) but lose the per-call
   term. Fairness is guaranteed by the DOORBELL (bounded preemption
   latency = one heartbeat period + one J2J call), not by charge
   precision; charging accuracy affects only accounting and DET_SCHED
   step boundaries. **OQ1 carries the probe.**
3. **DET_SCHED compatibility** (FR-9, adopted): no legacy-RMW carve-out —
   the deterministic config must run PRODUCTION code. Under
   PHARO_DET_SCHED the smash fires at charged-step boundaries
   (deterministic by construction since charges are deterministic).
   Existing repro baselines re-record once (yield points move).

## 10. Migration plan — dual protocol, batches, binary gates (the V2-flip playbook)

Playbook invariants (every batch):

- **Knobs in `src/vm/debug_vars.h` only** (DebugSettings is FROZEN +
  ratcheted). Master `DEBUG_BOOL(PHARO_T1_FSR)`; per-batch sub-knobs
  effective only under the master (the 6-knob inline-J2J pattern);
  `DEBUG_BOOL(PHARO_T1_FSR_VERIFY)` for the oracle. After the flip:
  invert to `PHARO_T1_NO_FSR` (the NO_INLINE_J2J precedent).
  **Arch-gated arm64-only** (FR-8): x86 T1 emit (AsmjitT1.cpp:1693-2262)
  keeps the legacy contract; shared C++ consumers (funnel, depth helper)
  tolerate both protocols via the knob until the jit-x86 branch ports.
- **Protocol/layout header discipline**: an `FSR_PROTOCOL` version
  constant beside J2JSaveLayout.h's V2 flag; ALL new JITState/JITMethod
  accesses via `offsetof` + static_asserts (the JM hasNLR off-by-one cost
  a 43x gap once); fix the raw `#208/#56/#0` in JIT_CALL and the raw `35`
  (JM.tempCount) at AsmjitT1.cpp:5640 in passing; new JITState fields
  (`j2jEntryCursor`) appended with JS_ constants + static_asserts.
- **Clean-rebuild discipline**: `rm -rf build build-opt` at every batch
  touching JITState offsets or layout-adjacent asserts; validate BOTH
  -O0 and -O2; ALL perf numbers from build-opt only (the dev-build-is-O0
  lesson — every historical custom-vs-Cog number was measured on a
  crippled build).
- **Instrument-first** (the retrospective's validate-first rule): the M0
  oracle lands and soaks before ANY removal. Verify mode = **dual-write**:
  the JIT keeps emitting legacy mirror stores alongside FSR; at every
  exit (and in prepareForGC) the oracle cross-checks derived vs mirrored
  {method, literals, argCount, j2jDepth, jitMethod} and traps on mismatch
  (FR-4). **Gate configs are pinned dual-write-OFF** — measuring with
  dual-write on temporarily recreates the 1dbc538b shape and under-reads
  (LR-F7).
- **Standard gates per batch** (binary, no judgment calls): byte-identical
  codegen at knob-off; sp-depth checker 1.8M+ checks clean; cfib +
  benchFib interleaved 5x5 quiet A/B at -O2, medians; 60-class SUnit A/B
  per-test identical; PHARO_DET_SCHED AIPrimTest repro still
  deterministic; the SILENT-SUNIT-RUNNER checklist (rm
  /tmp/sunit_run_completed.txt, rm stale startup.st, 1-based batch file,
  PHARO_MAX_STEPS=2e12, check PharoDebug.log) before any SUnit A/B.

Batches (money-first order per FR-10/CR-F7 — M2+M3 price the design
BEFORE the lazy-deletion infrastructure is built):

```
M0  prep + oracle (NO emitted-code change)
    - fix literals +8 at JITRuntime.cpp:890/899 (verified live in both arms)
    - JITMethod.literalsCache + zone-walk refresh; j2jEntryCursor field;
      j2jDepthFromCursor(); syncDerivedFromJM() funnel at the macro boundary;
      macro live-out str x19; convert/audit the raw T2 entries (843/3930)
    - grep-generated tables checked in: 23 blr sites classified; entry
      edges; depth-writer sites (the §4 checklist); chain-loop literals
      writers (>=8 sites: 22833, 23371, 23957, 24051, 24389, 24941, 24981,
      25060)
    - PHARO_T1_FSR_VERIFY dual-write oracle + prepareForGC in-bounds assert
    - static_asserts for every raw offset incl. JIT_CALL #208/#56/#0, 5640
    GATE: oracle soaks 60-class SUnit + bench with ZERO traps (the +8 fix
    is why it can)
M1  x19 invariant            PHARO_T1_FSR_X19
    - mov x19,calleeJM at PMS/generic/saveless emit sites, placed at the
      activation commit (dominates no old-activation bail edge — CR-F5)
    - TrampolineAsm Lcall_enter_callee: mov x19, x8 BOTH arms (FR-1 fix)
    - saveless stash carries + restores caller x19 (FR-6)
    - emitMaterializeX5 / 5437 / 5577 xmethod splits collapse to x19
    - VERIFY: exits trap if x19 != mirror OFF_JITMETHOD
M2  cursor residency (x23)   PHARO_T1_FSR_CURSOR          [MONEY 1]
    - emitted push/pop via x23 post/pre-index; limit stays per-call ldr
      (doorbell); optional x28 nil-fill freebie
    - JIT_CALL/JIT_RESUME_CALL: hoist + live-out + CLOBBER x23 (+x28)
      (CR-F1 second hole); unconditional null-cursor hoist (LR-F5)
    - exit stubs + BLR brackets publish x23; Ltramp_call limit reload
    - dual-cursor audit (j2jPoolCursor_ index vs byte cursor at every C++
      observation point)
    GATE: standard + GC-stress (allocation-heavy forced scavenges)
M3  depth elimination        PHARO_T1_FSR_NODEPTH         [MONEY 2]
    - prelude: ldr j2jEntryCursor; cmp x23 — kills the RMW
    - the FULL writer-site checklist from §4 (8 zeroers + 4 pinners + 3
      C++ movers); saveless stash j2jEntryCursor pin slot (LR-F3)
    - doorbell preemption + charging decision (§9); DET_SCHED checkpoint
      smash; frees x20 (telemetry re-emit knob must NOT use x20 — LR-F8;
      scratch reg or #error vs M5)
    - VERIFY derives depth both ways, traps on divergence at every exit
    GATE: standard + timer/Delay SUnit classes + /tmp/mutex_leak.st +
    wedge detector (OQ1 probe)
    ==> PRICE THE DESIGN HERE: if M2+M3 cumulative < 3% on cfib at -O2,
        re-evaluate M4-M6 as enabling-only work (no perf claims)
M4  lazy mirror deletion     PHARO_T1_FSR_LAZY
    - exit stubs re-source ip from x19->bcStartCache + publish x19
    - delete per-call/resume method/literals/ip/argCount/jitMethod stores
      (PMS, generic, saveless E2, resume continuation, trampoline-convert)
    - pushLiteral/pushLitVar via literalsCache; chain-loop argCount_ from
      jm->argCount — ONLY after the FR-7 VERIFY assert soaked clean
    - GC re-anchor on jitMethod; saveless stash shrink (keep §6.4 slots)
    - LONG soak before flip: after M4, dual-write is the only x19-ordering
      cross-check left (CR-F5)
    GATE: standard + GC-stress + full-suite soak. Expect ~0-2 ms; neutral
    here is NOT a stop signal (enabling batch — FR-10/CR-F7)
M5  receiver residency (x20) PHARO_T1_FSR_RECV   [gated, riskiest]
    - entry hoists everywhere (§6.2); mov x20,x1 at call sites; prelude/
      trampoline pops feed x20; save pushes from x20; OFF_RECEIVER reads
      re-emit to x20; exits + allocating-BLR brackets publish/RELOAD
    - dual-write sub-knob through soak (oracle compares x20 vs mirror);
      gates measured dual-write-OFF
    GATE: standard + PHARO_DET_SCHED A/B + 20K-test soak + full bench
    19/19; wild-receiver class — the #extent history applies
M6  tempBase residency (x26) compile-time PHARO_T1_TB_IN_X26 [gated]
    - #error vs TOS cache (incl. the .S ldur x26 lines — LR-F9);
      trampoline pops restore x26; gate on send-heavy bench >= 0 at -O2
      (the 1dbc538b precedent makes this strictly optional — the design
      is whole without it)
M7  cleanups (anytime after M4)
    - gate trampoline b5 telemetry (bl + localReturns, S.195-201);
      drop the JS_SP str/ldr round trip (S.221/226 class); shared
      exit-publish thunk if zone stats show pressure
M8  flip
    - default-on; invert to PHARO_T1_NO_FSR; retire per-batch knobs;
      delete legacy emit arms + dual-write after a full-suite soak;
      decommission like the V1 save layout
```

## 11. Honest win model — cfib 26 ms, Cog 8 ms, skeleton 4 ms

Per the six-times-confirmed OoO lesson: instruction COUNT is free
(stores drain via the store buffer; feeders are off-path); only
dependent-chain shortening and shared-address serialization (RMWs,
store-to-load forwarding round trips) measure. Claim-by-claim:

```
batch  mechanism                                  shape                         predicted cfib delta
M1     generic-path IC chain level                dependent-load kill, but      ~0 on cfib (linked path
       (state->jm->icBuffer->entry->cmp)          fires on UNLINKED sends only  branches at 4184); real
                                                  (FR-10a, CR-F7)               on SUnit-shaped code
M2     cursor RMW kill (str 4235 -> next ldp      per-call shared-address       -2 to -4 ms (Edelta.2b
       4209; prelude 3238/3240)                   serialization — the PROVEN    precedent: one RMW kill
                                                  Edelta.2b/x25 shape           = 29 -> 24-25 ms)
M3     depth RMW kill (4236-38 ldr/add/str +      same shape, one link longer   -2 to -4 ms
       prelude 3231/3232/3242)                    (ldr->add->str)
M4     17 store/feeder deletions                  store-buffer-absorbed; pure   0 to -2 ms (issue
                                                  issue bandwidth ~2-2.5 cy     bandwidth on 2.7M calls)
       bail-stub OFF_METHOD load re-source        off hot path                  0 (code size only)
M5     receiver STLF kill (str 4239 -> ldr 4221   forwarding round trips at     -2 to -3 ms
       next call; 2x pushReceiver at the HEAD     the send critical chain
       of the send chain)                         head, 4-5 cy each
M6     per-temp-access forwarded load halved      per-bytecode dependent load   -1 to -3 ms IF gates pass
```

Caps and stop-rules (FR-10b adopted): the critical path is the LONGEST of
the parallel per-call chains, not their sum — until M5/M6 land, the kept
receiver/tempBase STLF chains have the same period as the killed
cursor/depth chains and may cap M2+M3 below the precedent scaling. The
prelude win is partial (ldr entryCursor still feeds a branch; only the
STLF is avoided). Entry glue grows +2-4 ldrs per C++→JIT entry — gate on
chain-loop-bound workloads (bench-suite), not only cfib; predict <1-2%.

**Cumulative honest projection: 26 → ~18-21 ms after M2+M3+M4;
~16-19 ms with M5; stretch ~14-16 with M6.** Commit to nothing until the
interleaved 5x5 quiet -O2 protocol confirms per batch — count-based
predictions have over-promised six times.

**Cog 8 ms is explicitly NOT reached by this lever.** The residual gap is
category-1 work FSR does not touch: the 14-insn serial send head
(ldur→and→cbnz→lsr→cbnz→ldr→and→cmp→b vs Cog's 3-4), the save-push
stp traffic itself, dynamic nil-fill, and the 28 ns/iter naive-emit loop
floor. The skeleton's 4 ms is the no-state-maintenance bound: FSR closes
the state-maintenance share of (26−4), not the dispatch share. FSR's
terminal value even at perf-neutral is that it UNBLOCKS those levers:
saveless-tail-by-default needs the small stash (M4); the 24B save (drop
JSV_SP — caller sp = callee tempBase, derivable in the prelude) needs lazy
state; PMS-link-time static nil-fill needs the freed tail budget.
Necessary, plausibly not sufficient, for 8 ms.

## 12. Traceability — every reviewer finding → resolution

LR = lazy-exit review, FR = frame-slots review, CR = callee-derive review.

```
finding  severity  resolution
LR-F1    BLOCKER   §6.3 brackets publish ip+x19 together (coherent pair); §7 anchors
                   prepareForGC/afterGC on jitMethod; state.method visit KEPT (stale-but-live is
                   safe via zone walk); in-bounds assert added. Resolves jointly with CR-F3.
LR-F2    BLOCKER   §6.3 criterion = {GC | cursor motion | nested sends | lazified-field access};
                   POST-reload = {x20<-receiver, x23<-cursor}; worklist = fresh 23-site grep (M0);
                   VERIFY gcEpoch+cursor trap; block-receiver root note carried into the M5 audit.
LR-F3    HIGH      §6.4: saveless stash keeps a j2jEntryCursor pin slot (the Edelta.2d recurrence).
LR-F4    HIGH      §4 depth block: full zeroer/pinner/mover checklist IS the M3 checklist;
                   VERIFY derives depth both ways and traps at every exit.
LR-F5    MED       §6.2: unconditional null-cursor hoist; JIT_RESUME_CALL/T2-direct/trampoline
                   entries enumerated; raw T2 entries audited in M0.
LR-F6    MED       M0 fixes the +8 (re-verified at JITRuntime.cpp:890 AND 899 — both arms);
                   oracle lands after the fix, so no false positive.
LR-F7    MED       §11 adopts the calibration: bail-load = 0, D/E stores = 0, prediction restated
                   18-21; gates pinned dual-write-OFF; entry-glue tax gated on chain-loop workloads.
LR-F8    LOW       M3: telemetry re-emit uses scratch/#error vs M5; JIT_CALL raw offsets get
                   static_asserts in M0.
LR-F9    LOW       M6: trampoline ldur x26 lines under the compile-time #error.
LR-F10   LOW       Adopted: keep str wzr OFF_EXIT (OQ3); keep str x19 over findMethodByPC at exits.
FR-1     BLOCKER   M1: mov x19, x8 at Lcall_enter_callee, both arms (hole re-verified at S.476-484).
                   Cursor analog moot — x23 is trampoline-native (skeleton choice §8).
FR-2     HIGH      Same as LR-F2; predicate extended to mirror-readers (sync_globals, convert_send,
                   Ltramp_call JS_IP decode needs EXACT ip); cursor-sync assert inside GC helpers.
FR-3     HIGH      §9: limit stays a per-call memory load (doorbell preserved — never hoisted);
                   Ltramp_call reloads the limit (improves on today); charging = trampoline
                   localCalls + numBytecodes; fairness via doorbell latency bound; OQ1 probe.
FR-4     HIGH      M0: verify mode = dual-write + compare at exits (no method-entry canary).
FR-5     MED       Moot for the cursor (x23 chosen over x20). x20's M5 reassignment edits the
                   macro with static_asserts; limit-as-doorbell rule restated in §9.
FR-6     MED       §6.4: stash carries + restores caller x19.
FR-7     MED       §4 argCount: VERIFY exit assert soaks before M4 deletes the stores.
FR-8     MED       §10: knobs arch-gated arm64-only; C++ consumers protocol-tolerant until x86 ports.
FR-9     MED       §9: no DET_SCHED carve-out — checkpoint-driven smash, production code path.
FR-10    LOW       §10 batch order (M2/M3 price before M4-M6); §11 caps (kept receiver/tempBase
                   chains; M1 = 0 on cfib).
FR-11    LOW       M0 fixes raw 35; M6 adds a TOS-on config to the A/B matrix; OQ5 documents the
                   null-resumeAddr sharpness.
CR-F1    BLOCKER   §6.3 brackets publish x23 (A6 closure); §6.2 JIT_CALL adds x23 hoist/live-out/
                   CLOBBER (clobber-list hole re-verified); C++ cursor movers covered by macro
                   live-in re-hoists.
CR-F2    HIGH      §6.1: publish + funnel at the MACRO boundary (str x19 beside the x25 live-out);
                   14 macro sites listed; raw T2 entries audited/converted in M0.
CR-F3    HIGH      §6.3/§7: brackets publish ip WITH x19 (full coherent set, +3 insns at
                   allocation-class sites only) — jm-anchoring is then sound; invariant codified
                   as I2 "coherence, not freshness".
CR-F4    HIGH      = LR-F6 / FR — M0.
CR-F5    MED-HIGH  §2 I1 dominance rule written into the emit; M4 keeps dual-write through a long
                   soak as the post-deletion cross-check.
CR-F6    MED       M6: compile-time knob; trampoline pop restores; send-heavy gate.
CR-F7    MED       §10 order: M2 (R-cursor) and M3 land BEFORE the deletion batches and do not
                   depend on them; §11: core deletion booked at 0-2 ms, "neutral != stop" at M4.
CR-F8    MED       M0 grep-generates every site table (blr classification, entries, depth writers,
                   literals writers) and diffs against this doc.
CR-F9    LOW       OQ6 (T2-as-caller assert under VERIFY); FR-8 arch-gate; OQ5; raw 35 in M0.
```

## 13. Open questions — each with a concrete probe and a binary gate

```
OQ1  Charging fairness after the depth-RMW kill. Probe: run the timer/Delay
     SUnit classes, /tmp/mutex_leak.st, and the full-suite wedge detector
     under numBytecodes-only charging + doorbell. Gate: zero new
     timer-class failures, [WEDGE] never fires, deterministic 2s repro
     unchanged.
OQ2  Doorbell preemption latency. Probe: instrument heartbeat-smash to
     first-bail latency under cfib recursion (counter in the chain loop).
     Gate: p99 < 2 heartbeat periods.
OQ3  str wzr OFF_EXIT (3273) deletion. Keep it (1 free store) unless a
     stale-read audit of exitReason consumers proves no window. Probe:
     VERIFY poisons exitReason at entry and traps on read-before-write.
OQ4  x28 nil-fill hoist value (cat-3). Probe: M2 sub-A/B with/without.
     Gate: keep only if >= 0 and the JIT_CALL clobber addition costs
     nothing measurable on chain-loop workloads.
OQ5  Null-resumeAddr fallback misattribution (deep chain, bcOff > 0xFFF
     path): pre-existing V2 sharpness, FSR-neutral — documented here so an
     M4-soak failure with this signature is triaged against HEAD first.
     Probe: assert under VERIFY that a null-resumeAddr save's derived jm
     equals state.jitMethod at materialize time; count hits over a full
     suite (expected 0).
OQ6  T2-as-caller invariant ("a T1 prelude never pops a T2 save" — safe
     today because every T2 send/return routes through C++ helpers,
     JITRuntime.cpp:860+). Probe: VERIFY tags saves with an emit-tier bit
     in the packed word's spare bits and traps in the T1 prelude on a T2
     tag. Gate: 0 hits full-suite.
```

## Key file anchors

`src/vm/jit/asmjit/AsmjitT1.cpp` (send head 4131-4198, PMS tail 4199-4263,
saveless 4988-5238 esp. 5015-5028/5215, generic 5367-5660 esp. 5500-5511/
5640, prelude 3211-3276, resume 7154-7196, emitMaterializeX5 4039-4073,
blr sites per the 23-line grep), `src/vm/jit/TrampolineAsm.S` (prologue/
pins 125-170, V2 pop 184-232, telemetry 195-201, convert 250-258,
Lcall path 440-484, osr ~561), `src/vm/jit/JITState.hpp` (fields 43-95,
JIT_CALL 288-322), `src/vm/jit/JITRuntime.cpp` (jit_t2_send 777-900 esp.
804/860/885-899, block-value prep 1923-1948, jit_rt_j2j_call 2353-2363,
raw entries 843/3930-3936), `src/vm/jit/J2JSaveLayout.h`,
`src/vm/Interpreter.cpp` (GC 18236-18468 esp. 18261-18272, resume-in-caller
19271-19577, materialize 22021-22144, chain loop 22146-24250, charging
19537/22995-22999), `src/vm/Interpreter.hpp` (zone walk 3341-3372,
forEachRoot 3484-3530), `src/vm/jit/BcDepthMap.cpp` (305-410),
`src/vm/debug_vars.h`.
