# Out-of-line (shared-stub) send dispatch — arm64 T1 JIT design

Status: SYNTHESIS-v2 2026-06-14 — final design pass, post adversarial review.
Generalizes the PMS doc's §11 B6 sketch ("shared per-(kind,nArgs) frameless
stubs") into the full lever: **stop EMITTING the full dispatch sequence inline at
every send site; share the cold body once.**

> **v2 correction (load-bearing).** The v1 draft's central mechanism — site ends
> in `bl SharedSendStub` to harvest x30 as the per-site resume address — was
> WRONG and is ABANDONED.  On this VM **x30 is the JIT method's live
> return-to-C++ link**, not a spare LR: `JIT_CALL` enters every method via
> `blr %[e]` (`JITState.hpp:341-379`) and the method exits via `a.ret()` =
> `ret x30` (AsmjitT1.cpp:1648/1901/…).  The whole J2J chain is FRAMELESS and
> shares ONE x30 across `b`/`br` transfers (ground truth
> `/tmp/disasm_jit_cfibx_1.txt`: 0 `bl`, 3 `br`, 15 `ret` in 1392 insns).  A
> `bl` would clobber that link → the terminal `ret x30` returns into the stub,
> not C++ → runaway.  The resume address is instead carried the way the shipped
> code already carries it: per-site `adr x14, resumeAfterCall` (1 insn, off the
> critical chain, AsmjitT1.cpp:5309/6656), passed into the shared stub in a
> register; the stub transfers control with `b`/`br`, NEVER `bl`/`blr`.  The
> headline win is re-grounded on **PMS tail-deletion** (a LINKED-STUB state that
> elides the per-site J2J save-push skeleton), which is shipped PMS infra and
> needs no x30 trick.

Goal: stop emitting the full dispatch sequence INLINE at every send site.  Today
a linked recursive send is ~102 *emitted* instructions; cfibx (19 bytecodes)
compiles to 5568 B / 1392 arm64 insns (~293 B/bytecode), and that bloat is the
sole reason `DefaultCodeZoneSize` was bumped 16 MB → 64 MB
(`JITConfig.hpp:75-84`).  Cog instead makes each send site a short transfer into
a shared trampoline/PIC; the dispatch logic lives once, out of line.

Line anchors are as of commit `124877dd` (re-grep before editing).  Verified
this session:
- `JIT_CALL` entry `blr %[e]` / live x30 contract: `JITState.hpp:341-379`.
- method exit `a.ret()` (== `ret x30`): AsmjitT1.cpp:1648, 1901, 1925, 1951, …
- per-site resume-address producer `adr x14, resumeAfterCall`:
  AsmjitT1.cpp:5309 (linked tail), :6656 (V2 packed save-push); also
  `adr x5, resumeAfterCall` :6371/:6404.
- `resumeAfterCall` continuation, INCLUDING the PC-relative caller-context
  re-establish block: `a.bind(resumeAfterCall)` :8488; `adr x19,
  g_codeStartLabel; sub; str OFF_JITMETHOD; method; literals; argCount`
  :8503-8515.
- `emitJ2JReturnPrelude_arm64` :3887; return-prelude pop+`br x8` :3956-3966;
  per-return-op inline call site + its two exits :4337-4415.
- `g_codeStartLabel` :113 / bound :8902-8903; `emitLoadSp/emitStoreSp`
  :3059/3067; zone size `JITConfig.hpp:84`.
- Disasm ground truth: `/tmp/disasm_jit_cfibx_1.txt` (1392 insns; 0 `bl`,
  3 `br`, 15 `ret`; the V2 save-push reads `adr x14,#0x9c8` /
  `movk x14,#0xb,lsl#48`) and `/tmp/disasm_jit_tl_1.txt`.

---

## 0. Decision summary + core invariants

**Decision.** Adopt the **Cog site-as-short-transfer** model, applied ONLY to
the cold/unlinked/cross-method-cold lane, layered ON TOP OF PMS — it does NOT
replace PMS's per-site direct-branch J2J tail.  Concretely:

- **Resume crux (§4): keep the proven per-site `adr x14, resumeAfterCall`.** The
  resume address is produced at the SITE by one `adr` (off the critical chain,
  bit-identical to today, AsmjitT1.cpp:5309/6656), passed into the shared stub in
  a register, and the stub transfers control with `b`/`br`.  **No `bl`/`blr` is
  ever introduced into a send-site fast path or any patched word** (OL-I7,
  emit-time asserted) — because x30 is the method's live return-to-C++ link
  (`blr` entry → `ret x30` exit) and the frameless J2J chain shares it.
- **The split (§2/§3):** SITE keeps the IC-probe head (receiver load +
  class-key compare — the one dependent chain the OoO lesson says measures), the
  `adr x14` resume producer, the short transfer, and a post-return result-splice.
  The dispatch fan-out + xmethod gate cascade + the J2J save-push BODY move into
  a shared frameless stub.
- **PMS is SUBSUMED, not fought (§5):** a PMS-linked monomorphic *self-rec* site
  keeps its baked direct-branch J2J tail (the recursion win — HARD CONSTRAINT 6).
  Everything else — unlinked, polymorphic, cross-method-cold — takes the
  shared-stub path.  The new patcher state is **LINKED-STUB** (W2 = `b
  SharedSendStub`, not `bl`), which **deletes the per-site save-push skeleton**.
  That deletion — a PMS tail-deletion, shipped infra — is the actual zone win.
- **The win is Axis-1 (zone/i-cache/compile-coverage), NOT Axis-2 (per-send
  latency).**  On the hot linked path the design is ~0 by construction (the
  self-rec tail stays inline + direct).  The payoff is restoring compile
  *coverage*: per-method emit shrinks (re-derived honestly in §1) so 16 MB holds
  far more methods, attacking the retrospective's #1 root cause (95%
  compile-fail; jit-retrospective.md:881).

**Core invariants** (each is a binary gate in §6/§7):

- **OL-I1 (resume = per-site `adr`, register-passed).**  Every shared-stub site
  produces its continuation with `adr x14, resumeAfterCall` at the SITE HEAD
  (1 insn, off the critical chain), and the stub records `packedResume = x14 |
  (nArgs<<12|bcOff)<<48` (the same packed word built today; the WRITER moves from
  inline to stub, the VALUE and consumers are UNCHANGED).  x30 is never read or
  written by the site or the stub for resume purposes.
- **OL-I2 (head stays at site).**  The receiver LOAD A → header LOAD B → key
  compare dependent chain (`disasm 0x1ec-0x224`; AsmjitT1.cpp:5220-5273) is
  per-site DATA (the `-8*(nArgs+1)` slot is this send's nArgs; the W0/W1 key is
  this site's PMS class).  It never moves to the stub.
- **OL-I3 (VM-resident register contract preserved across the `b`-transfer).**
  These are NOT AAPCS callee-saved across a call — there is no call boundary; the
  transfer is a frameless `b`.  They are **VM-resident registers** set by
  `JIT_CALL` and maintained across the frameless chain (JITState.hpp:341-379):
  `x19` = active `JITMethod*` (FSR I1), `x23` = save cursor (FSR M2), `x20` =
  j2jDepthInc constant, `x25` = sp, `x26` = TOS mirror (simStack).  The site head
  does not reassign x19/x20/x23/x25; the stub publishes {ip, x19, x23} ONCE at
  its head before any allocating helper (FSR I3) so GC reached from inside the
  stub sees a coherent pair (FSR I2: coherence not freshness).  **x30 is
  OFF-LIMITS to the stub** (OL-I7): it holds the method's return-to-C++ link.
- **OL-I4 (stub is frameless + shared-once).**  The stub never builds a frame,
  reads its inputs from a fixed register contract, is emitted ONCE in the runtime
  region (like `jit_rt_*` helpers), and is NEVER re-patched.  It transfers to the
  callee via `br calleeEntry`; it never executes `ret`.  Only PMS's W0/W1/W2 are
  patched per link.
- **OL-I5 (the transfer is a `b`, the continuation is a `bcToCode` re-entry).**
  The site's transfer into the stub is `b SharedSendStub` (the patched W2 word,
  see §5/§6 for the patch-while-live discipline).  The resume continuation
  (`resumeAfterCall`) is a normal `bcToCode`-class address inside the method,
  outside any patched word, so it is always a valid {ip} mapping for GC/NLR.
- **OL-I6 (register-only stub I/O).**  The stub's per-call inputs ride registers
  (x1/x4/x2/x10/x14/x19/x20/x23/x25), never a shared JITState scratch slot.  One
  shared stub means every site `b`s to one address — fine for i-fetch (direct
  branch, BTB-warm) but a spill to a shared slot would create a new STLF hot spot
  (the OoO failure mode).  If BTB/STLF shows in A/B, split into per-(kind,nArgs)
  stubs (§3.4).
- **OL-I7 (NO `bl`/`blr` in any send fast path or patched word — x30 is
  sacred).**  An emit-time assert (mirroring PMS invariant 4, §6.4) FAILS the
  build if any `bl`/`blr` is emitted between a send-site head and the end of its
  patched tail, or into any word PMS may patch.  This is the structural guard
  that the v1 x30 hazard can never be reintroduced silently.

---

## 1. Motivation + honest win model

### 1.1 The bloat is real; the latency win is not; the lever is PMS tail-deletion

Ground-truth anatomy of ONE linked recursive send (cog-speed-current.md +
AsmjitT1.cpp), ~102 *emitted* insns:

```
    IC-probe head            17   (receiver load, tag/leak guard, class-key cmp)
    dispatch / poly-walk     13   (slot-0/1/2 key walk, primKind fan-out)
    xmethod gate cascade     16   (8-load JM-header gate: bit16 prim, numIC, ...)
    J2J save-push            34   (cursor check, stp save record, callee-state setup)
    branch                    1   (br calleeEntry)
    return-prelude           21   (poppability check, ldp pop, restore, br x8)
```

cfibx: 19 bytecodes → 5568 B / 1392 insns → **~293 B/bytecode**, send-dominated
(disasm shows 156 `str`-to-`[x0]` state writes, 20 `stp` save-pushes, 15
ret/return-preludes for those 19 bytecodes).  The non-send loop `tl` is only
~39 B/bytecode — essentially ALL the bloat is the inline send sequence.

**What ALREADY moved off the steady-state path under default-on PMS (so it is NOT
the residual lever).**  The ~102 figure conflates code already out-of-line
per-site with code that executes inline per send.  On a LINKED recursive send
(the steady state = the disasm hot path), only head(14) + the linked-J2J
save-push EXECUTE; the dispatch fan-out(13) and gate cascade(16) ALREADY live in
the cold per-site `Lprobe` block (AsmjitT1.cpp:5376-6024), reached ONLY on IC
miss.  PMS already moved ~29 insns off the steady-state EXECUTION path.

**So where is the residual EMITTED bloat — the thing this lever actually
deletes?**  Two distinct contributors, and the win must be attributed honestly to
each:

1. **The per-site `Lprobe` SKELETON is still EMITTED inline** at every site even
   though it executes only on miss (AsmjitT1.cpp:5376-6024).  That is pure
   zone / i-cache bloat: bytes occupied, rarely fetched.
2. **The per-site J2J save-push body** (28 reachable insns) is emitted at every
   non-self-rec linked tail.

Deleting (1) and (2) per-site is the **LINKED-STUB patcher state** (§5.1) — a PMS
tail-deletion: the linked site keeps its patched key + a `b SharedSendStub`, and
the save-push/probe body lives once in the stub.  **This needs no x30 trick.**
The v1 draft mis-attributed the win to the `bl`/x30 resume mechanism; the win is
the tail/skeleton deletion, achievable on the proven `adr`+`b` resume path.

### 1.2 Axis-1: zone bloat / compile coverage / i-cache (the real win)

This is the axis where the lever is unambiguous.  The win is re-derived from what
LINKED-STUB actually elides (§1.1), NOT from the abandoned x30 trick:

- **What LINKED-STUB deletes per site:** the per-site `Lprobe` skeleton
  (dispatch fan-out + gate cascade, ~29 insns of emitted-but-cold code) and the
  per-site save-push body (~28 insns), replaced by: the unchanged head(14), one
  `adr x14` (resume), one `movk x14` (packed metadata) folded into the resume
  word, a `b SharedSendStub` (W2), and the post-return splice(5).  Per
  LINKED-STUB site: **~57 emitted insns deleted, ~6 added → ~51 insns / ~204 B
  saved per converted site.**
- **What it does NOT delete:** self-rec linked sites keep their direct tail
  (HARD CONSTRAINT 6); the SHARED RETURN-PRELUDE (§4.2, B0) deletes the
  per-return-op 21-insn prelude separately.

The achievable shrink is therefore **gated on the LINKED-STUB site fraction**
(open Q4 — a link-state histogram bounds it).  Honest framing:

- **If most non-self-rec sites convert:** cfibx-shaped methods are heavily
  self-rec, so cfibx itself shrinks LESS than a reflective method.  Expect cfibx
  5568 B → ~3000-4000 B (return-prelude sharing + the few non-self-rec sites).
  Reflective/SUnit methods (many distinct, mostly non-self-rec sites) shrink
  far more: ~2.0-3.0× is plausible there.
- **Headline target (must be measured, not asserted):** report the shrink as a
  **link-state-weighted** number from the histogram, not a single cfibx figure.
  The B1 gate (§7) requires the cfibx emitted-byte drop AND a reflective-method
  emitted-byte drop, both from DUMP_SEL stats, before any perf claim.

Second-order effects (the actual payoff — the OoO lesson says raw insn count is
otherwise free):

1. **Code zone fits 16 MB again.** Per-method emit shrinks (link-state-weighted,
   §1.2), so 16 MB holds materially more methods than today.  This directly
   attacks jit-retrospective.md:881 — "the dominant reason the VM is slow is HOT
   METHODS FAIL TO JIT-COMPILE" (3474 compiled / 70069 FAILED).  Late-compiled
   hot methods that can't allocate run interpreted → the historical gap.  Smaller
   methods = fewer zone-full failures = more hot methods actually JITed.  This is
   a **correctness-of-coverage win**, the highest-value effect.  (Magnitude is a
   B3 deliverable, not a §1 promise.)
2. **I-cache locality.** Moving the cold per-site probe/save skeleton out of line
   packs the hot path into far fewer lines and makes the shared stub a hot,
   resident, shared working-set entry across ALL sites (Cog's model).  On
   reflective/SUnit workloads with thousands of distinct sites this also relieves
   indirect-BTB capacity pressure.
3. **Fewer late-compile failures** also reduces recompile thrash and eviction
   churn.

### 1.3 Axis-2: per-send dependent chain (NEUTRAL by construction)

The OoO lesson, six-times-confirmed (FSR §11 / simstack §9 /
cog-speed-current.md:62-65): instruction COUNT is free — independent stores
drain via the store buffer, predicted branches don't stall fetch.  Only (a)
dependent-chain shortening, (b) shared-address serialization (RMW / STLF round
trips), and (c) zone-bloat / i-cache effects MEASURE.  Applying this to a shared
stub reached by `b` (NOT `bl`):

1. **The dispatch dependent chain is RELOCATED, not removed.** The stub does the
   SAME `ldur`-receiver → `and`-tag → `ldr`-header → classIndex-extract → `cmp` →
   branch chain.  Moving it behind a `b` changes its address, not its length.
   Net dependent-chain delta from relocation alone: ~0.
2. **The `b`/`br` transfers add NO `ret` to predict.** Because resume is via the
   per-site `adr x14` value popped by the callee's return-prelude (`br x8`), and
   the stub→callee transfer is `br calleeEntry`, the control flow is a chain of
   direct/indirect branches — the SAME predictor surface as today's J2J chain
   (rasbench 0.98-1.13× on the fib-shaped pattern, PMS §1).  There is no
   `bl`/`ret` pair added, so no RAS perturbation and no x30 hazard.
3. **PMS already won the recursion case the right way.** PMS gives each linked
   self-rec site a patched class-key + DIRECT-BRANCH J2J tail — saveless, no
   indirect `br`, no stub hop.  Routing self-rec through the stub would add a stub
   hop to the hottest loop; we DON'T — self-rec stays direct (§5.1).

**Conclusion on Axis-2: keep the hot LINKED self-rec tail inline and
direct-branched; out-of-line ONLY the cold/non-self-rec body.**  Axis-2 is ~0 on
the hot path BY CONSTRUCTION, and the entire win is Axis-1.

### 1.4 Honest projection

- Microbench (cfib/benchFib/cfibx) at -O2, within-binary, hot linked path:
  predict **~0 to -3%** (flat, or a small WIN only if i-cache pressure on the
  bloated inline version is currently real).  Do NOT promise a cfib speedup.
- Real-code / SUnit / reflective: this is where it WINS, via the Axis-1
  second-order effects.  Projection: "more methods JITed → fewer interpreted hot
  paths → closing part of the 18× non-inlined-send gap"
  (vm-speed-lever-dispatch memory) + reduced indirect-BTB pressure.  A COVERAGE
  win, gated on "compile-fail rate drops and full-suite survivability rises," NOT
  "cfib drops N ms."

---

## 2. Site layout — the short-transfer shape

The site replaces today's inline cold dispatch + save-push with: IC-probe head
(stays) → `adr x14` resume producer + packed-metadata fold (stays) → ONE `b`
into the stub (the patched W2) → post-return splice (stays).

### 2.1 What stays at the site (per-site DATA)

**(i) IC-probe head, 14 insns** (disasm 0x1ec-0x224; AsmjitT1.cpp:5220-5273):

```
    mov  x2, x25                      ; sp copy
    ldur x1, [x2, #-8*(nArgs+1)]      ; LOAD A receiver (per-site nArgs slot)
    and  x4, x1, #7                   ; tag test
    cbnz x4, Lmiss                    ;   (immediate -> miss)
    lsr  x4, x1, #48                  ; leak guard
    cbnz x4, Lmiss
    ldr  w4, [x1]                     ; LOAD B header (dep on A) -- the chain
    and  w4, w4, #classIndexMask
    movz w6, #<keyLo>                 ; PMS W0  (this site's patched class)
    movk w6, #<keyHi>, lsl #16        ; PMS W1
    cmp  w4, w6
    b.ne Lmiss                        ; -> the stub / generic miss path
```

The LOAD A → LOAD B → cmp is the send critical dependent chain (OL-I2): the one
thing the OoO lesson says actually measures; it cannot be shared.

**(ii) Per-site resume producer + transfer, 3 insns** (the load-bearing
site/stub boundary):

```
    adr  x14, resumeAfterCall              ; resume ADDRESS (1 insn, off-critical-chain, == today)
    movk x14, #(nArgs<<12 | resumeBcOff), lsl #48   ; fold packed metadata into spare top bits
    b    SharedSendStub_<kind>             ; PMS W2 word: a plain branch, NEVER bl
```

This is exactly the value the shipped V2 save-push builds today (`adr x14,
resumeAfterCall; movk x14,(nArgs<<12|bcOff),lsl#48`, AsmjitT1.cpp:6656/6693-6694)
— we just produce it at the site and pass it in x14 instead of letting the inline
save-push build it.  The `adr` is OFF the caller's hot dependent chain (it feeds
the stub's save record, not the receiver compare), so it lands in the
off-critical-chain regime.  **There is no `bl` and no x30 use** — the transfer is
a branch; the resume address rides x14.

**(iii) Post-return result-splice** (`resumeAfterCall` continuation;
AsmjitT1.cpp:8488-8516).  Two cases, and the budget must reflect BOTH:

- **Self-context lane (callee resumes back into the same method's frame
  state):** ~5 insns — load sp, drop args, write retval into this site's recv
  slot, store sp:

```
  resumeAfterCall:                   ; <- the adr target; the popped resume addr lands here
    <emitLoadSp x2>
    sub  x2, x2, #8*nArgs
    stur x1, [x2, #-8]               ; write callee retval (x1) into THIS site's recv slot
    <emitStoreSp x2>
```

- **Cross-method / block-value lane (the cold lane this design routes to the
  stub):** the continuation must ALSO re-establish caller context PC-relatively,
  which is per-site and NOT shareable (AsmjitT1.cpp:8497-8515):

```
  resumeAfterCall:                   ; cross-method variant
    adr  x19, g_codeStartLabel        ; recover caller code base
    sub  x19, x19, #<this method's start offset>
    str  x19, [x0, #OFF_JITMETHOD]    ; re-publish caller JITMethod*
    ; restore method / literals / argCount for THIS caller (per-site immediates)
    ...
    <emitLoadSp x2> ; sub args ; stur retval ; emitStoreSp   (as above)
```

**Site continuation budget is ~12-14 insns for the cross-method lane, ~5 for the
self-context lane** — NOT a flat 5.  Because the shared stub is for the
unlinked/poly/cross-method-cold population, most stub-targeting sites use the
cross-method variant; budget accordingly (§2.2).  GC coherence (CONSTRAINT 3) is
satisfied because the continuation is a valid {ip} mapping for this jitMethod
(OL-I5), independent of how control reached it.

### 2.2 Site insn budget

```
    head                          14
    adr x14 + movk + b             3
    post-return splice (self)       5     ; self-context lane
    post-return splice (xmethod)  12-14   ; cross-method lane (the targeted lane)
    --------------------------------------
    SITE total (self lane)        ~22
    SITE total (xmethod lane)     ~29-31  (vs ~102 emitted today incl. inline probe+save skeleton)
```

The win is in the EMITTED total dropping from ~102 (head + inline probe skeleton
+ inline save-push + prelude) to ~22-31, because the probe skeleton and save-push
move to the shared stub.  See §1.2 for the per-site byte accounting.

---

## 3. Shared stub layout — per-(kind,nArgs) frameless stub

### 3.1 Register contract (the head's existing Lprobe contract, 5263-5264,
5320-5323)

On entry to `SharedSendStub_<kind>` (reached by `b` from the site W2):

```
    x1  = receiver           (LOAD A, already validated by the site head)
    x4  = receiver classIndex (already extracted)
    x2  = sp                 (mov x2,x25 from the head)
    x10 = calleeJM           (derived at the site or by the stub from the IC)
    x14 = resumeAddr | (nArgs<<12 | resumeBcOff)<<48   (the per-site adr+movk, OL-I1)
    x19 = caller JITMethod*   (VM-resident, FSR I1, set by JIT_CALL, live across the b)
    x23 = save cursor         (VM-resident, FSR M2)
    x20 = j2jDepthInc const   (VM-resident, JITState.hpp:351)
    x25 = sp                  (VM-resident)
    x26 = TOS mirror          (VM-resident, simStack)
    x30 = caller's return-to-C++ link  (VM-resident, NOT TOUCHED -- OL-I7)
```

These are VM-resident registers, NOT AAPCS callee-saved across a call — there is
no call boundary; the site reaches the stub with a frameless `b`.  The stub runs
on the same register file the method body uses, does cursor-check + save-push,
then `br calleeEntry`.  It never reads or writes x30 (OL-I7).

### 3.2 Stub body (paid once, ~78 insns)

```
  SharedSendStub_<kind>:
    ; --- (0) FSR publish for GC coherence, ONCE (OL-I3, replaces per-site copies)
    ldr  x16, [x19, #bcStartCache]   ; ip = bcStart + bcOff
    ubfx x17, x14, #48, #12          ; bcOff from packed word (top bits of x14)
    add  x16, x16, x17
    str  x16, [x0, #ip]              ; publish ip
    str  x19, [x0, #jitMethod]       ; publish x19  (FSR I3 core set)
    str  x23, [x0, #j2jSaveCursor]   ; publish x23  (bounds the save-pool scan, A6)

    ; --- (1) dispatch fan-out + xmethod gate cascade (29 insns; AsmjitT1.cpp
    ;         5389-5421 poly-walk + 5868-6024 gate cascade; disasm 0x2e4-0x3bc)
    ;   slot-0/1/2 key walk; primKind classifier tbnz fan; cross-method gate
    ;   cascade (methodHeader bit16 prim gate, numICEntries, isStubOnEntry,
    ;   canBailMidMethod, cap).  This is the per-site Lprobe SKELETON moved here:
    ;   sharing removes the per-site EMITTED COPY (zone/i-cache), not the
    ;   execution.
    ...
    cbz  <suitable>, Lgeneric_bail    ; -> C++ dispatchCached / ExitSendCached

    ; --- (2) J2J save-push body (28 insns; AsmjitT1.cpp 5296-5369 linked /
    ;         6420-6505 generic; disasm 0x228-0x2dc).  packedResume is ALREADY
    ;         in x14 (built at the site by adr+movk) -- the stub just stores it.
    ldp  x6, x14tmp, [x0, #cursor]    ; cursor/limit load
    cmp  x6, x14tmp
    b.hs Lfull_bail                   ; pool full -> C++ full-bail
    mov  x15, x25
    ldr  x4r, [x0, #8]
    stp  x15, x4r, [x6], #0x28        ; push save record (sp, recv, ...)
    ldr  x15, [x0, #0x18]
    stp  x15, x14, [x6, #-0x18]       ; tempBase + packedResume (x14 from the site)
    ; callee-state level-1 loads off x10 (calleeJM): CM, jitMethod/method/
    ; literals, argCount, cursor, depth via x20, receiver, tempBase, ip from
    ; bcStartCache; dynamic nil-fill loop
    ...

    ; --- (3) transfer control to callee (NEVER ret; NEVER bl)
    mov  x19, x10                     ; x19 = callee JITMethod*  (FSR I1 at activation)
    br   calleeEntry                  ; tail-branch; callee return-prelude brs to the popped resumeAddr
```

### 3.3 How control flows (no `bl`, no `ret` for the transfer)

The stub NEVER returns to its caller via `ret`, and the site NEVER used `bl`.
Control flows:

```
    site head
      -> (b SharedSendStub, the patched W2)         ; plain branch, x30 untouched
    stub: publish FSR, dispatch, push save (packedResume=x14 from site adr)
      -> (br calleeEntry)                            ; tail branch into callee
    callee runs ...
    callee return-prelude: ldr resumeAddr from save record; br to it
      -> site resumeAfterCall (== the adr x14 target); pop args, write retval
```

The resume address came from the site's own `adr x14` (OL-I1), stored into the
save record by the stub, and popped + `br`'d by the callee's return-prelude.  No
`bl` is issued anywhere on this path, so x30 (the method's return-to-C++ link)
remains intact for the eventual `ret x30` that returns to `JIT_CALL`.  The
predictor surface is direct/indirect branches only (rasbench 0.98-1.13×, PMS §1).

### 3.4 Per-(kind,nArgs) specialization

The stub must know nArgs to pop args + size the save.  Two options:

- **Parameterize nArgs in x14 (preferred for the i-cache win):** ONE stub per
  kind, nArgs read from the packed word's top bits.  Smallest stub-table
  footprint.
- **Bake N stubs (B6 "per-(kind,nArgs) frameless stubs"):** kinds {J2J,
  saveless, self-rec, prim-prologue} × nArgs 0..4 ≈ 15-25 stubs, if profiling
  shows the per-call nArgs read lands on a dependent chain that measures.

Gate (§7): measure stub-table total bytes; parameterized-nArgs is the default
unless A/B shows the read costs.

---

## 4. The resume-address mechanism (PMS §13 Q7 solution, corrected)

### 4.1 Why per-site `adr x14` (PMS §13 Q7 option (b)), and why NOT `bl`/x30

HARD CONSTRAINT 1 / PMS §13 Q7: a shared stub does not know the per-site resume
point that the save-push records and the return-prelude pops.  The v1 draft chose
"`bl` to harvest x30" — that is FATAL on this VM and is abandoned.  The viable
mechanism is the proven one, re-costed honestly:

- **(b) per-site `adr x14, resumeAfterCall`, register-passed — CHOSEN.** The
  resume address is produced at the site by ONE `adr` (PC-relative,
  AsmjitT1.cpp:5309/6656), folded with the 16-bit `nArgs|bcOff` metadata via one
  `movk`, and passed into the stub in x14.  Real cost: **1 `adr` + 1 `movk` per
  site**, both off the critical dependent chain (they feed the save record, not
  the receiver compare).  This is bit-identical to today's V2 save-push word; it
  is NOT a 2-4 insn 48-bit `movz/movk` address build (the v1 mis-cost of option
  (b)) — `adr` is a single PC-relative insn.  The `adr` target is a normal
  in-method address (not a separately-baked absolute), so there is no new W^X
  patch surface beyond PMS's existing W2.
- **(a) `bl` to harvest x30 — REJECTED (FATAL).** x30 is the method's live
  return-to-C++ link: `JIT_CALL` enters via `blr %[e]` (JITState.hpp:341-379) and
  the method exits via `a.ret()` = `ret x30` (AsmjitT1.cpp:1648/1901/…).  The
  whole J2J chain is FRAMELESS and shares ONE x30 across `b`/`br` transfers
  (`/tmp/disasm_jit_cfibx_1.txt`: 0 `bl`, 3 `br`, 15 `ret`).  A `bl` overwrites
  x30; the terminal `ret x30` then returns into the stub, not C++ → runaway.
  Worse, it would destroy x30 for a value `adr` already provides for free.  An
  emit-time assert (OL-I7, §6.4) prevents any `bl`/`blr` from re-entering a send
  fast path or patched word.
- **(c) Side-table keyed by a return address — REJECTED.** This is the documented
  findMethodByPC linear-walk hot-path trap (memory
  jit-findmethodbypc-linear-walk-trap; PMS §4 / §13).  Even the O(log n)
  `pcIndex_` binary search is too expensive per-send (shared-address
  serialization, the exact failure mode the win model forbids).

### 4.2 J2J save-push + return-prelude interaction

- **Save-push.** The V2 packed push today is `adr x14, resumeAfterCall; movk
  x14, (nArgs<<12|resumeBcOff), lsl #48; stp x15,x14,[x6,#-24]`
  (AsmjitT1.cpp:6656/6693-6694; disasm 144-152).  Under this design the SITE
  builds the `x14` packed word (the `adr` + `movk` moved to the site, §2.1(ii))
  and the STUB keeps the SAME `stp` (§3.2 step 2).  The JSV layout (V2, 32 B / the
  size-40 `tempBase@-24, packedResume@-16, closure@-8` layout at :6693) is
  UNTOUCHED — this is a writer-location change (inline → split site/stub),
  FSR-neutral.
- **Return-prelude.** Pops it unchanged: `ldr x8,[x4,#JSV_RESUMEADDR]; and
  x8,x8,#0x0000FFFFFFFFFFFF; br x8` (AsmjitT1.cpp:3956-3966), landing at the
  site's own `resumeAfterCall` (:8488), which pops args and writes retval.  The
  prelude does NOT move into the SEND stub — it `br`s to the per-site
  continuation, which only exists at the site (the `adr x14` target).  But it CAN
  become its OWN shared stub: it is small, has NO resume-address problem (it
  already `br`s to a popped x8), and is the LOWEST-RISK first move (§7 B0).  See
  §4.4 for the non-trivial details B0 must handle.
- **Inline-J2J tail interaction (HARD CONSTRAINT 6).** A PMS-linked monomorphic
  *self-rec* site keeps its existing baked direct-branch J2J tail and NEVER uses
  the send stub — its resume is already site-local and its tail is saveless/direct
  (the measured recursion win).  The shared stub only covers the unlinked /
  polymorphic / cross-method-cold lane.  So:

```
    linked-monomorphic-self-rec   = unchanged per-site direct-branch tail (no stub)
    everything else (unlinked/poly/cross-method-cold) = b SharedSendStub
```

  Because BOTH disciplines transfer with `b`/`br` (the shipped linked tail uses
  `b calleeEntry`, AsmjitT1.cpp:5369; the stub uses `b SharedSendStub` then
  `br calleeEntry`), they share ONE x30 with no conflict — there is no `bl` in
  either, so a stub-lane site earlier in a body cannot corrupt x30 for a later
  self-rec site's eventual `ret x30`.  This is precisely why v1's mechanism (a)
  was incompatible and why (b) composes cleanly.

### 4.3 NLR / terminate interaction (CONSTRAINT 4)

`materializeJ2JSaveIntoFrame` resolves the caller JITMethod via
`findMethodByPC(resumeAddr)` exactly as today — the resume mechanism change moves
WHERE `packedResume` is built (site `adr` → x14 → stub `stp`), not its VALUE or
its consumers.  Critically, **the resume address is the site's `adr x14` value,
NOT derived from a clobbered x30** — so the v1 coupling-1 hazard ("a path
reaching the stub without x30 records a wrong resume") cannot occur: x14 is
always the correct per-site `resumeAfterCall`, set at the site head before the
transfer, whether the site is hot, cold, or re-entered after an IC miss.  NLR
walkers consume `savedFrames_` only, and all pending saves are materialized
before the interpreter takes over (FSR §4 NLR row); each save's `packedResume`
points at the per-site `resumeAfterCall`, so misattribution is impossible.  The
stub is frameless and tail-`br`s to the callee, so NLR never unwinds THROUGH the
stub.

### 4.4 What B0 (shared return-prelude) must actually handle

The return-prelude is NOT a trivial single-exit block.
`emitJ2JReturnPrelude_arm64` (AsmjitT1.cpp:3887-3967) is inlined into each return
op (4341-4415) with **two exits** and **per-site variants**:

- **Exit 1 (poppable):** `ldp` pop the save record, restore, `br x8` to the popped
  resume address.
- **Exit 2 (fall-through, non-poppable):** `normalReturn` → `str RETVAL`,
  `EXIT_RETURN`, `syncSp`, `ret x30` (the C++-return path — and note this is
  EXACTLY where x30 must be intact, OL-I7).
- **Per-site variants:** `staticJ2JArgCount` V1 path (3973), simStack x26
  ordering (4397), `g_fsrCursor`/`g_fsrNodepth` publishes (3898-3934).

A shared `SharedReturnPrelude` must therefore (a) handle BOTH exits in the shared
block, and (b) parameterize the per-site variants (V1 argCount, simStack x26,
g_fsr*) via registers or a small kind-fan, OR restrict B0 to the variant subset
that is byte-identical across sites and leave the rest inline.  This is
salvageable with care, NOT a trivial "already `br`s to x8" move.  B0's gate (§7)
requires the byte-identical knob-off hash AND coverage of both exits.

---

## 5. Composition with PMS and FSR

### 5.1 PMS — SUBSUMES, does not fight (CONSTRAINT 2)

PMS is default-on and provides per-site direct-branch J2J tails.  The new design
adds ONE patcher state and a routing rule:

```
    PMS site states today:   UNLINKED -> LINKED (W0/W1 key, W2 = b <baked tail>)
    NEW state:               LINKED-STUB (W0/W1 key, W2 = b SharedSendStub,
                                          NO per-site save-push/probe skeleton emitted)
```

- **Where the zone saving comes from (re-grounded, §1.2):** the LINKED-STUB state
  emits NO per-site save-push body and NO per-site `Lprobe` skeleton; both live in
  the shared stub.  This is the non-self-rec population (most reflective sites).
  The win is a **PMS tail-deletion** — shipped infra — NOT the x30 trick.
- **Self-rec stays direct:** linked-monomorphic-self-rec = unchanged direct tail
  (recursion win, HARD CONSTRAINT 6).  The patcher chooses LINKED (direct tail)
  for self-rec, LINKED-STUB (`b` stub) otherwise, from the same callee-suitability
  predicate PMS already computes.  W2 is a `b` in BOTH states — never a `bl`
  (OL-I7), so PMS invariant 4 (no BL/BLR between site head and end of patched
  tail) is preserved verbatim.
- **Link/unlink** derive from IC slot 0 exactly as PMS §6/§7; LINKED-STUB unlink
  rewrites W2 back to `b Lmiss`/the generic probe — no baked tail to scrub (the
  skeleton lives in the shared stub, which is never patched).

### 5.2 PMS §6/§7 nine-event invalidation matrix, re-derived for LINKED-STUB

The shared stub adds a new patched-state target (`W2 = b SharedSendStub`).  The
stub itself is emit-once-never-repatched (OL-I4), so it is NOT a patch target —
but W2's VALUE (the branch displacement to the stub) and the LINKED-STUB state
must be covered for every PMS event.  Re-derivation (events from PMS §7):

```
  event                          LINKED-STUB action
  -----------------------------  ----------------------------------------------
  1 first link (UNLINKED->...)   if self-rec: W0/W1=key, W2=b baked tail (as today)
                                 else:        W0/W1=key, W2=b SharedSendStub_<kind,nArgs>
  2 IC miss / class change       unlink: W2 := b Lmiss (generic probe); re-link per (1)
  3 callee recompile / tier-up   stub is shared+stable -> W2 (b stub) UNCHANGED;
                                 calleeJM is read from the IC at run time, so a new
                                 callee body is picked up with NO re-patch
  4 callee eviction              calleeJM in IC goes stale -> handled exactly as PMS:
                                 IC invalidation forces unlink (event 2); W2 -> generic
                                 probe; the stub address itself is never freed (runtime
                                 region), so no dangling-stub branch is possible
  5 caller eviction              whole method freed; its W2 words vanish with it; the
                                 shared stub persists (no back-reference into the method)
  6 GC scavenge (move)           stub holds NO baked object pointers; calleeJM/receiver
                                 ride the IC + registers + save pool, all FSR-tracked;
                                 W2 displacement is code-relative (stub in runtime region,
                                 not moved) -> no fixup
  7 GC fullGC / compaction       same as (6); code zone not compacted by object GC
  8 splice (re-point W2)         the ONLY W2 writes are: link (->b stub or b tail),
                                 unlink (->b probe).  Single-word atomic store under PMS
                                 ScopedWriteAccess + ranged icache invalidate, identical
                                 to PMS today; no new flip kind
  9 patch-while-live re-entry    W2 is the LAST word of the site head fast path; the
                                 continuation (resumeAfterCall) is outside any patched
                                 word (OL-I5) -> patch-while-another-thread-executes is
                                 the SAME safety case PMS already proved for the tail
```

Key results: (i) callee recompile/tier-up needs **no re-patch** of stub-targeting
sites (the stub reads calleeJM from the IC), strictly fewer round trips than a
per-site baked tail that bakes a callee entry; (ii) the baked stub address is
never invalidated because the stub lives in the never-freed runtime region;
(iii) all W2 writes remain single-word atomic stores under PMS's existing
ScopedWriteAccess discipline — no new W^X flip kind, so the 2026-05-03 sieve 2×
per-flip regression cannot recur.

### 5.3 FSR — x19 / x23 / cursor (CONSTRAINT 3)

- **x19** (FSR I1, active `JITMethod*`): VM-resident, live across the `b`-transfer
  (set by JIT_CALL, never reassigned by the head).  The stub publishes x19 →
  `state.jitMethod` at its head (§3.2 step 0), then `mov x19, x10` at the
  activation commit (FSR I1 at activations).
- **x23** (FSR M2, save cursor): VM-resident; the stub publishes it before any
  allocating helper (closes FSR A6 stale-receiver reappearance — the save-pool
  receiver scan FSR Hpp:3498-3500 is bounded by the published x23).
- **cursor RMW:** exists inline today and is UNCHANGED by relocation (FSR M2/M3
  attack it orthogonally and COMPOSE: the stub reads the x23-resident cursor).
  Net dependent-chain delta from the cursor: 0.
- **The {ip, x19, x23} publish is written ONCE in the stub** instead of replicated
  at every inline send site — this IS the zone-bloat lever the i-cache argument
  rests on (FSR I3 publish set, emitted shared).

---

## 6. GC + W^X + NLR correctness

### 6.1 GC at the stub bracket (CONSTRAINT 3, FSR §7 coherence)

- **jitMethod:** x19 == caller `JITMethod*` on stub entry (FSR I1; VM-resident,
  unchanged across the `b`).  The stub publishes it (§3.2 step 0) so any GC
  reached from inside the stub's own C-call brackets sees a coherent jitMethod.
- **ip:** the resume address rides x14 (the site's `adr`); the stub derives the
  caller's bcOff from x14's packed top bits + `bcStartCache(x19)` and publishes ip
  (§3.2 step 0).  This yields exactly the {x19, ip} coherent pair FSR I2 requires
  at GC-capable brackets — and it does NOT depend on x30, which is irrelevant to
  resume here.
- **The per-site `b` is NOT a GC point** — it just transfers control.  GC happens
  only inside the stub's own C-helper brackets, which publish per FSR I3.  The
  resume continuation (`resumeAfterCall`) is a valid `bcToCode`-class address
  outside any patched word (OL-I5).
- The save-pool receiver scan is bounded by the published x23 cursor, which the
  stub publishes before any allocating helper (FSR A6).
- **This is CHEAPER than today's inline path:** the FSR publish set is written
  ONCE in the stub instead of replicated at every inline send site.

### 6.2 W^X / MAP_JIT (CONSTRAINT 5)

The stub is emitted ONCE in the runtime region (like `jit_rt_*` helpers) and is
NEVER re-patched (OL-I4).  Only PMS's W0/W1/W2 are patched per link, reusing PMS's
amortized link-time patching (depth-aware `ScopedWriteAccess`, ranged
`sys_icache_invalidate`, link-once-per-site-per-GC — PMS §4.1).  W2 is a
single-word `b`-displacement store in BOTH LINKED and LINKED-STUB states (§5.2
event 8) — no new flip kind, no per-call flip, so it cannot re-introduce the
2026-05-03 per-fill-flip sieve 2× regression.  Net: FEWER W^X round trips than
today's per-site-tail emit, because the shared stub's body is written once at
startup, not re-emitted per method, AND stub-targeting sites need no re-patch on
callee recompile/tier-up (§5.2 event 3).

### 6.3 NLR (CONSTRAINT 4)

Unchanged — the stub is frameless and tail-`br`s to the callee, so NLR walks the
same byte-identical save-record chain and never unwinds THROUGH the stub.
`materializeJ2JSaveIntoFrame(resumeAddr)` resolves the correct caller JITMethod
(§4.3), and the recorded `resumeAddr` is the site's own `adr x14` value (never a
clobbered x30), so the v1 silent-misattribution class
(jit-xmethod-gate-offbyone, inline-J2J #extent) is structurally excluded.
Deep-recursion preemption (cog-speed-current.md scheduler-preempt blocker) still
fires because the save records are byte-identical.

### 6.4 Emit-time guard (OL-I7) — the structural defense against the x30 hazard

Add an emit-time assert mirroring PMS invariant 4: during method emit, track the
span from each send-site head to the end of its patched tail (and the bytes of any
PMS-patchable word).  If the asmjit instruction stream emits `bl` or `blr` inside
that span, FAIL the build (assert in the T1 emitter, like the existing PMS
no-call-in-patched-tail assert).  This makes it impossible to reintroduce v1's
`bl`/x30 mechanism silently: any future "harvest x30" attempt trips the assert at
compile time, not at runtime via a runaway.  The assert also covers the shared
stub body (the stub may call C++ helpers via `blr` for its generic-bail / full-bail
exits — those `blr`s are OUTSIDE any send-site fast path and any patched word, so
they are explicitly allowed; the assert scope is the per-site fast path + patched
words, exactly PMS invariant 4's scope).

---

## 7. Staged landing plan (B0..B6, each knob-gated, BINARY gate)

Every batch: (i) byte-identical knob-off emit (hash harness); (ii) within-binary
A/B only for any perf claim; (iii) `PHARO_DET_SCHED=1` AIPrimTest/AITarjanTest
ERROR=0; (iv) emitted-bytes stat under PHARO_T1_DUMP_SEL.  Knobs go in
`debug_vars.h` (project rule), opt-in until the default-ON flip.

- **B0 — SHARED RETURN-PRELUDE — LANDED opt-in (commit f2493c49, 2026-06-14).**
  Per-method scope (local label, zero cross-method-addressing risk): when an
  arm64 method has >=2 prelude-using return ops, route them to ONE shared
  prelude+epilogue block at method end; the shared block owns BOTH exits (the
  uniform `str retval;EXIT_RETURN;syncSp;ret x30` epilogue is byte-identical
  across return ops, so no per-site normalReturn address is needed). Single-
  return methods stay inline (so benchFib/cfibx are untouched). Validated:
  battery==golden==Cog; rdense (3 ret) 5692->5528 B + correct both exits;
  cfibx (1 ret) SIZE-identical 5568->5568; DET_SCHED rdense 75025 x3; SUnit
  subset (1577 tests, Array/OC/Dict/String/Interval) per-test IDENTICAL on/off.
  NOTE (measurement): raw byte dumps AND EMIT_HASH vary run-to-run via
  ASLR-baked helper/zone addresses (off-vs-off differs) — emitted SIZE is the
  ASLR-immune knob-off-identity proxy; capstone-classified baseline diff is
  deferred to the default-ON flip.
  B0.5 — ZONE-GLOBAL shared stub — LANDED opt-in (commit 5fe0c001, 2026-06-14).
  Promotes B0 to ONE never-freed MAP_JIT stub page (getSharedReturnPreludeStub:
  asmjit -> flatten -> copy under ScopedWriteAccess; stable absolute address,
  separate from the flush/evict'd method zone -> no invalidation). EVERY real
  non-block method's returns collapse to `mov x16,stub; br x16` (x30 stays the
  live return link). Single-return methods now shrink too (cfibx 5568->5496).
  Position-independent prelude (no adr/literal/abs-reloc) copies directly; gated
  off under per-method VERIFY knobs (g_codeStartLabel) -> inline. Validated:
  battery==Cog, cfibx/rdense correct, DET_SCHED 75025 x3, SUnit subset 1577 tests
  per-test identical on/off, actual zone 32.70M->32.00M (~1.8%). Measured reach
  (PHARO_T1_RETPRELUDE_STATS): the return prelude is a SMALL fraction of bloat
  (~1.2-1.8%) — the per-SEND machinery dominates (B1). B0.5's real value is the
  proven zone-global-stub infra that B1's per-send stub reuses. (Original B0
  design notes below.)
- **B0 (original design notes) — de-risked first move, but NOT trivial — see
  §4.4.**  Build `SharedReturnPrelude` as a frameless shared stub handling BOTH
  prelude exits (poppable `br x8` AND fall-through `normalReturn`/`ret x30`) and
  the per-site variants (V1 staticJ2JArgCount, simStack x26 ordering, g_fsr*),
  either parameterized via registers or restricted to the byte-identical-variant
  subset.  Replace the per-return-op inline `emitJ2JReturnPreludeIfEnabled()`
  (AsmjitT1.cpp:4337-4415) with a per-method `b SharedReturnPrelude` for the
  covered variants.  NO resume-address problem; validates the
  frameless-shared-stub + within-binary-A/B machinery before the harder send side.
  *Knob:* `PHARO_T1_SHARED_RETPRELUDE`.
  *Gate (BINARY):* (1) knob-off byte-identical (hash); (2) BOTH exits exercised
  (a poppable-return test AND a non-poppable normalReturn test) produce identical
  observable behavior knob-on vs off; (3) cfibx emitted bytes drop on
  return-dense methods (DUMP_SEL stat, target ≥10% method shrink there);
  (4) within-binary A/B cfib/benchFib/cfibx 5×5 medians within noise of knob-off
  (NOT slower); (5) DET_SCHED AIPrimTest/AITarjanTest ERROR=0; (6) 60-class SUnit
  subset per-test identical knob-on vs off; CharacterTest>>testStoreStringAll
  passes.

- **B1 — SHARED SEND STUB skeleton + LINKED-STUB state (the core).**  Emit
  `SharedSendStub_<kind>` (§3); add the LINKED-STUB patcher state (§5.1) and the
  §5.2 invalidation handling; route unlinked/poly/cross-method-cold sites to
  `b` stub, self-rec to the existing direct tail.  Resume via per-site `adr x14`
  (mechanism (b), §4); add the OL-I7 emit-time assert (§6.4).
  *Knob:* `PHARO_T1_OOL_DISPATCH`.
  *Gate (BINARY):* (1) knob-off byte-identical; (2) emitted-byte shrink measured
  BOTH on cfibx AND on a reflective method (DUMP_SEL), reported as a
  link-state-weighted number — **the primary deliverable, measured before any
  perf claim**; (3) within-binary A/B hot linked-send path within noise (the
  LOSE-check — if it regresses, the stub is wrongly on the hot path, pull the hot
  tail back inline); (4) DET_SCHED ERROR=0; (5) 60-class SUnit subset per-test
  identical; (6) PHARO_T1_PATCH_VERIFY clean (PMS composition); (7) the OL-I7
  assert is active and the build is clean (no `bl`/`blr` in any send fast path).

- **B2 — GC + NLR + W^X soak.**  scavenge + fullGC stress with PATCH_VERIFY;
  eviction stress + post-eviction verify walk (no LINKED-STUB W2 targets a freed
  range; the stub itself is in the never-freed runtime region per §5.2 event 4/5);
  callee recompile/tier-up stress (verify NO re-patch needed, §5.2 event 3);
  NLR/exception soak; sieve×100 / sort 100K sentinels unmoved; deep-recursion
  preemption still fires.
  *Gate (BINARY):* zero misattribution in the NLR soak; PATCH_VERIFY clean
  throughout; sentinels unmoved; no faults under the W^X relink stress
  (force-relink every linked site each `drainRecompileQueue`); the nine-event
  matrix (§5.2) exercised with PATCH_VERIFY clean per event.

- **B3 — ZONE-FIT validation (the headline Axis-1 gate).**  Set
  `DefaultCodeZoneSize` back to 16 MB under the knob; run startup + a full
  60-class SUnit batch; dump JIT stats.
  *Gate (BINARY):* (1) ZERO zone-full allocation failures (dumpJITStats); (2)
  compiled-method count ≥ the 64 MB-zone baseline; (3) FAILED/(compiled+FAILED)
  does NOT rise (ideally falls); hot methods (benchFib-during-eval) still JIT.
  This directly tests the retrospective's #1 root cause.

- **B4 — breadth + full-suite survivability.**  Full-suite batched run; per-test
  diff vs knob-off; `-O0` AND `RelWithDebInfo` both (the dev-build-is-O0 lesson);
  any divergence reproduced under DET_SCHED before touching code.
  *Gate (BINARY):* full-suite batched run ≥ the 836-class watermark; per-test
  diff vs knob-off empty; classify-sunit.py Δcog shows zero deterministic
  regressions.

- **B5 — default-ON flip.**  `PHARO_T1_NO_OOL_DISPATCH` opt-out; keep
  `DefaultCodeZoneSize = 16 MB` permanently if B3 held; the generic path stays
  (nothing removed, no cliff).
  *Gate (BINARY):* full-suite ≥ baseline at 16 MB; image snapshot round-trip;
  fresh-image startup clean.

- **B6 — per-(kind,nArgs) stub split + i-cache refinement (telemetry-driven).**
  Only if B1's A/B showed BTB/STLF pressure (OL-I6) or the parameterized-nArgs
  read measured: split into per-(kind,nArgs) stubs; measure stub-table total
  bytes; re-aim at the bench2/bench3 send-rate step.
  *Gate (BINARY):* L1i miss rate per send does NOT rise (proxy on a send-dense
  reflective workload); indirect-BTB mispredict rate on send branches does NOT
  rise; stub-table total bytes < (saved per-site bytes).

---

## 8. Honest win model recap + stop-rules

**Recap.** Axis-2 (per-send latency) is ~0 on the hot path BY CONSTRUCTION (PMS
keeps the direct-branch self-rec tail; the stub absorbs only the cold/non-self-rec
body, reached by `b`, with no `bl`/`ret` perturbation).  Axis-1 (method-size
shrink via PMS tail-deletion of the per-site save-push + probe skeleton, zone
bump 64 MB → 16 MB recoverable, compile-fail rate down) is the entire payoff.
The win is the **LINKED-STUB skeleton/tail deletion** — shipped PMS infra — NOT
any x30 trick.  The biggest single win is restoring compile COVERAGE — the
retrospective's #1 root cause of slowness — bigger than any microbench delta.
This lever MEASURES ~ZERO on microbenches and that is EXPECTED, not failure.

**Stop-rules (kill or park the lever if):**

1. **B1 hot-path A/B regresses cfib/benchFib > 3%** AND pulling the hot tail
   inline does not recover it → the stub is structurally on the recursion path;
   STOP (PMS already banked recursion the right way).
2. **B1 emitted-byte shrink is small on reflective methods too** (the
   link-state-weighted shrink is < ~1.5×) → the LINKED-STUB population is too
   small to matter (open Q4); the Axis-1 thesis is weak for this workload mix.
   PARK and re-measure what actually fills the zone.
3. **B3 does not reduce compile-fail rate** at 16 MB (FAILED/(compiled+FAILED)
   flat or rising, or hot methods still fail to JIT) → the Axis-1 thesis is wrong
   for this workload mix; PARK.
4. **B6 shows the stub-table working set itself bloats** (per-(kind,nArgs)
   proliferation > saved per-site bytes, or L1i misses rise) → revert to the
   single parameterized-nArgs stub; if even that loses i-cache, the lever's only
   remaining value is zone-fit, gate on B3 alone.
5. **Any W^X per-call flip creeps in** (re-introducing the 2026-05-03 sieve 2×
   regression) → STOP; the stub MUST be emit-once-never-repatch (OL-I4).
6. **The OL-I7 assert is disabled or any `bl`/`blr` enters a send fast path** →
   STOP; x30 corruption is a correctness loss (runaway/crash), not a perf wash.
7. **The metric keys on cfib ms** → WRONG GATE.  Judge on compile-fail rate and
   full-suite survivability (the OoO lesson: count is free).

---

## 9. Open questions — each with a concrete probe

1. **Does the parameterized-nArgs read land on a dependent chain that
   measures?**  The single-stub design reads nArgs from x14's packed top bits to
   pop args + size the save.  *Probe:* within-binary A/B, single-stub vs B6
   baked-N stubs, on cfibx + a 0/1/2/3-arg send mix; if single-stub is within
   noise, ship it (smaller i-cache).  If it measures, the read is on the
   save-push chain → bake N.

2. **Is the dispatch ALREADY i-cache-resident in the inline version?** (PMS §1
   explicit caveat.)  If so, both the inline fan-out AND the stub hit L1, so the
   stub adds the transfer for no i-cache benefit on THAT workload (net small
   loss).  Only workloads large enough to thrash i-cache (thousands of distinct
   sites) see the locality win.  *Probe:* L1i miss rate per send, cfibx (tight,
   resident) vs a send-dense SUnit-shaped workload, knob-on vs off; the win
   should appear ONLY on the latter.

3. **Cross-method-resume splice fraction.**  §2.1(iii) shows the cross-method
   lane continuation is ~12-14 insns (PC-relative caller-context re-establish),
   the self-context lane ~5.  The site-budget and zone win both depend on which
   lane dominates the stub-targeting population.  *Probe:* count
   resumeAfterCall-variant emit (self-context vs cross-method) on a full-suite
   run; if cross-method dominates, the per-site continuation is bigger than the
   v1 budget assumed → re-rank the zone win vs B0 (return-prelude) alone.

4. **LINKED-STUB lane volume (the headline-win bound).**  How many sites are
   LINKED-STUB (non-self-rec) vs LINKED (self-rec direct tail) vs UNLINKED?  The
   zone win scales with the LINKED-STUB population (§1.2).  *Probe:* a link-state
   histogram counter on a full-suite run; report the link-state-weighted
   emitted-byte shrink (this IS the B1 primary deliverable, §7).  If self-rec
   dominates, the zone win is smaller than projected (re-rank vs B3 alone).

5. **Closed-PIC tier (polymorphic) — needed?**  Cog upgrades a 2nd-class hit to a
   small standalone cmp-chain stub (firstCPICCaseOffset); PMS dropped CPICs from
   v1.  *Probe:* poly-density counter (sites with ≥2 hit classes) on a reflective
   run; build the CPIC-shaped out-of-line jump-table stub only if poly density
   warrants (it composes: the site's `b` re-points at the PIC stub instead of the
   mono stub, same shape — still a `b`, never a `bl`).

6. **Does threading the resume value through a register cost anything vs the
   shipped inline save-push?**  Today the inline save-push builds the packed word
   right before the `stp`; under this design the site builds it (`adr`+`movk`) and
   the stub stores it, so x14 is live across the `b` into the stub.  *Probe:*
   confirm x14 is not clobbered by the head between the `adr` and the stub's `stp`
   (it is not — the head uses x1/x2/x4/x6), and A/B the single extra register
   liveness; expect ~0 (off the receiver-compare critical chain).

7. **Can the SEND stub and RETURN-prelude stub share one runtime-region entry**
   to cut total stub bytes, given both are reached by `b` (not the
   `blr x7` trampoline)?  *Probe:* prototype a shared prologue block both stubs
   fall into; measure total stub bytes + mispredict rate; only if it shrinks the
   working set without an indirect-predictor penalty.  (Note: do NOT route either
   stub through a `blr x7` trampoline — that would put a `blr` on the path and is
   exactly the x30 hazard the OL-I7 assert forbids.)

---

## Critique resolutions

Mapping each required change from the adversarial review to how this v2 doc
addresses it:

- **RC1 — ABANDON mechanism (a) (`bl` to harvest x30); transfer with `b`/`br`,
  never `bl`.**  Done: §0 decision, the v2-correction banner, §4.1(a) REJECTED
  with the JITState.hpp:341-379 `blr` entry / `ret x30` exit ground truth and the
  `/tmp/disasm_jit_cfibx_1.txt` 0-`bl` count.  Every transfer in §2/§3 is a `b` or
  `br`.  OL-I7 (§0, §6.4) is the structural guard.

- **RC2 — Re-base resume on per-site `adr x14, resumeAfterCall` passed in a
  register; re-cost (b) honestly (1 `adr`, not 2-4 movz/movk).**  Done: §2.1(ii),
  §4.1(b) CHOSEN with the honest cost (1 `adr` + 1 `movk`, both off-critical), and
  OL-I1.  The stub stores the site-built x14; control transfers via `b`/`br`.

- **RC3 — Re-ground the win model on LINKED-STUB-deletes-per-site-skeleton (PMS
  tail deletion), not the x30 trick; quantify with a link-state histogram.**
  Done: §1.1 explicitly attributes the residual EMITTED bloat to the per-site
  `Lprobe` skeleton + save-push body, §1.2 re-derives the shrink as
  link-state-weighted, §5.1 frames it as a PMS tail-deletion, and the B1 gate
  (§7) + open Q4 require the histogram-weighted number as the primary deliverable.

- **RC4 — Fix §2.1(iii): the cross-method/block-value resume splice must include
  the PC-relative caller-context re-establish block (~12-14 insns, not 5).**
  Done: §2.1(iii) now shows BOTH lanes (self-context ~5, cross-method ~12-14 with
  the `adr x19, g_codeStartLabel; sub; str OFF_JITMETHOD; …` block at
  AsmjitT1.cpp:8497-8515), §2.2 budgets both, and open Q3 probes the fraction.

- **RC5 — Correct the AAPCS framing; document the real VM-resident register
  contract and state x30 is off-limits.**  Done: OL-I3 (§0) and §3.1 now label
  x19/x20/x23/x25/x26 as VM-resident registers set by JIT_CALL and maintained
  across the frameless `b` chain (NOT AAPCS callee-saved across a call boundary,
  because there is no call boundary), and explicitly mark x30 OFF-LIMITS (OL-I7).

- **RC6 — Downgrade B0 to a real subtask: handle both prelude exits and all
  per-site variants, with a byte-identical knob-off gate.**  Done: §4.4
  enumerates the two exits (poppable `br x8`; fall-through `normalReturn`/`ret
  x30`) and the per-site variants (V1 argCount, simStack x26, g_fsr*); the B0 gate
  (§7) requires byte-identical knob-off AND both exits exercised.

- **RC7 — Add an emit-time assert that NO `bl`/`blr` enters any send-site fast
  path or patched word.**  Done: OL-I7 (§0) and §6.4, mirroring PMS invariant 4,
  scoped to the per-site fast path + patched words (the stub's own generic/full
  bail `blr`s to C++ are outside that scope and allowed).

- **RC8 — Re-derive the PMS §6/§7 nine-event invalidation matrix for the new
  shared-stub-targeting state (eviction, recompile/tier-up, splice).**  Done:
  §5.2 gives the full nine-event matrix for LINKED-STUB, including the key
  results that callee recompile/tier-up needs no re-patch (calleeJM read from the
  IC), the baked stub address is never freed (runtime region), and all W2 writes
  stay single-word atomic under PMS's existing flip discipline.

- **PMS-conflict resolutions.**  (i) LINKED-STUB W2 is a `b`, never a `bl`, so PMS
  invariant 4 holds verbatim (§5.1, OL-I7).  (ii) Both self-rec direct tail and
  stub lane transfer with `b`/`br` and share one x30 with no conflict — the v1
  incompatibility was unique to `bl` (§4.2).  (iii) "Fewer W^X round trips" is now
  derived, not asserted (§5.2 event 3 + §6.2).  (iv) The §4.3/§6.3
  misattribution worry is structurally excluded because `resumeAddr` is the site's
  own `adr x14` value, never a clobbered-then-reused x30.

- **Win-model honesty.**  §1 now states the latency win is not real, the lever is
  PMS tail-deletion, and the headline shrink is a measured link-state-weighted
  number — directly reflecting the critique's winModelCheck ("broken as written;
  underlying thesis plausible with a different resume mechanism").