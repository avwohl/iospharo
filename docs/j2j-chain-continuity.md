# J2J Chain Continuity

Multi-session project plan to close the JIT method-to-method (J2J) chain-continuity gap:
lift execution residency from ~5% chains/(chains+falls) toward Cog-like ~90%, so hot
sends stay in JIT native code instead of bouncing to the C++ interpreter dispatch loop.

This doc is self-contained. A fresh session should be able to start work from
section 8 (Resumption Notes) without re-deriving the analysis below.

---

## 1. GOAL + METRIC

### Target

```
metric                              current baseline    target
--------------------------------    ----------------    ----------------
J2J method-send continuity          ~5%                 ~90% (Cog-like)
  = actChains / (actChains+actFalls)
boundary-machinery % of profile     ~40%                <10%
primitive chaining (already good)   ~94%                hold >= 94%
```

J2J method-send continuity is the headline number. "Continuity" means: when a
JIT-compiled method finishes (or sends), the next method also runs in JIT native
code via the chain loop, rather than the chain loop materializing saved frames and
handing control back to the interpreter dispatch loop. Every materialize/bail is a
"fall"; every successful in-JIT continuation is a "chain".

### How measured

Three independent oracles, used together:

1. **actChain / actFall counters** (primary, in-VM, zero external tooling).
   - `jitJ2JActChains_` (Interpreter.hpp:1471) — successful J2J continuations.
   - `jitJ2JActFalls_`  (Interpreter.hpp:1472) — falls to interpreter (sum of all
     three fall sites in section 3).
   - Continuity % = `jitJ2JActChains_ / (jitJ2JActChains_ + jitJ2JActFalls_)`.
   - Sub-counters localize the fall:
     - Site 1 (stencil non-return): `jitStencilFallSendCached_` (Hpp:1489),
       `jitStencilFallSend_` (1490), `jitStencilFallJ2JCall_` (1491),
       `jitStencilFallOther_` (1492).
     - Site 2 (block-frame-push prim): `jitChainPrimFalls_` (1481),
       per-prim `jitPrimFallHisto_[600]` (1482); chain side
       `jitChainPrimChains_` (1480), `jitPrimChainHisto_[600]` (1483).
     - Site 3 (activateMethod frame-push): `jitChainActivateFalls_` (1486),
       chain side `jitChainActivateChains_` (1487).
     - Stencil success vs resume success split: `jitJ2JStencilReturns_` (1475)
       vs `jitJ2JActChains_` — if the stencil returned cleanly (ExitReturn) but
       the chain still fell, the *resume* failed, not the stencil.

2. **PHARO_ACTIVATION_LOG** — per-activation trace (path/method/exit-reason) for
   attributing falls to specific call edges. Use when the counters say "site 1
   ExitSendCached is 60% of falls" and you need to know *which sends*.

3. **macOS `sample` / Instruments** — wall-clock attribution of the ~40% boundary
   machinery. The counters tell you *how often* the chain falls; `sample` tells
   you *how expensive each fall is* (materializeJ2J, frame rebuild, dispatch-loop
   re-entry, IC re-lookup). Confirm a milestone moved real time, not just a counter.

### Current baseline numbers

Measured per ~5000-compile run (from fall-taxonomy map):

```
jitJ2JActChains_  ~=  10k
jitJ2JActFalls_   ~= 195k
continuity        ~= 10k / 205k  ~= 5%

jitChainPrimChains_ / (chains+falls)  ~= 94%   (primitives already chain well)
jitChainActivateFalls_ (site 3)        ~= 0 in AWFY (structural, rare)
```

Re-establish these exact numbers in M0 before any change (the run above predates
several emit knobs; the M0 deliverable is a *fresh* baseline plus per-site
attribution).

---

## 2. WHY IT MATTERS

### The perf chain

```
compiler is ~20x Cog            (each compile is expensive; can't compile our way out)
  -> ~40% of the profile is boundary machinery
       (materializeJ2J, frame rebuild, dispatch-loop re-entry, IC re-lookup)
  -> driven by ~5% method-send J2J continuity
       (95% of method sends fall back to the interpreter)
```

Because the compiler is 20x slower than Cog, we cannot close the gap by compiling
more or compiling faster. The lever is *residency*: once code is compiled, keep
execution inside it. Every fall pays the boundary tax twice — once to materialize
saved frames on the way out, once to re-establish JIT state on the way back in.
At 95% fall rate that tax dominates (~40% of profile).

This is the same shape FSR attacked at the *intra-call* level (per-call mirror
writes). J2J chain continuity attacks the *inter-call* level (does the next call
even stay in JIT). They compose: FSR makes each hop cheaper; continuity makes more
hops happen in-JIT.

### Already shipped (do NOT redo)

- **GC young-generation scavenger** — allocation during chain no longer forces a
  full GC; the chain survives more allocating callees.
- **Sista churn fix** — the recompile/IC-churn that was evicting hot J2J entries
  is fixed; J2J entries now persist long enough to be reused.
- **FSR M0–M4** (commits `8971777b..15c4ea55`) — per-call state mirror writes
  removed; each J2J hop is already cheaper. This plan builds on FSR being landed.

### Ruled out (do NOT re-investigate)

- **FSR as the continuity lever** — FSR is a *per-hop cost* win, measured a wash on
  continuity %. It does not change *how often* the chain falls. Orthogonal.
- **Block-body inlining** — not the lever for method-send continuity. Block
  handling is section-3 site 2; the headline 5% is dominated by site 1
  (stencil send exits), which is a method-send problem.
- **Blocks as a distinct bottleneck** — block-resume already chains via
  tryExecute for prims 207/209 (Interpreter.cpp:28161). Blocks are not a separate
  large bucket; they fold into site 2 which is medium, not dominant.
- **BV-specialization path** (`jit_rt_inline_block_value_prep`, JITRuntime.cpp:2025)
  — measured fires=0. Dead. See section 7; it is cruft to remove, not a lever.

---

## 3. FALL TAXONOMY

Three fall sites in the chain loop (`tryJITActivation`, Interpreter.cpp ~27300–28340).
Each increments `jitJ2JActFalls_`. Ranked by leverage (frequency x how much is
missed-opt vs correctness-required).

```
site  what                         counter(s)                  freq     class
----  --------------------------   -------------------------   ------   --------------------
1     stencil non-return exit      jitStencilFall{SendCached,   HIGH     mixed (see split)
      (state.exitReason !=            Send,J2JCall,Other}_
       ExitReturn after stencil)
2     block-frame-push primitive   jitChainPrimFalls_,         MEDIUM   mostly missed-opt;
      (prim succeeded + pushed       jitPrimFallHisto_[]                  hasNLR = correctness
       a frame)
3     activateMethod frame-push    jitChainActivateFalls_       ~0       structural (non-JIT
      (non-JIT / unsafe-prim                                              target); unavoidable
       target)
```

### Site 1 — stencil non-return (Interpreter.cpp:28043–28049). DOMINANT.

After `JIT_CALL(stencil)` (~27495), if `state.exitReason != ExitReturn` (~27595)
the callee stencil exited mid-flight. Split by exit reason:

```
exit reason       sub-counter                 class                 why it bails
---------------   -------------------------   -------------------   ------------------------------
ExitSendCached    jitStencilFallSendCached_   MISSED-OPT (target)   IC matched + resolved, but
                                                                    nesting the send safely needs
                                                                    robust nested J2J save/restore;
                                                                    bails to dodge x86 nested-send
                                                                    corruption (PHARO_T1_X86_
                                                                    XMETHOD_SENDS open issue)
ExitSend          jitStencilFallSend_         MISSED-OPT            uncached/mismatched target;
                                                                    needs fresh send resolution
ExitJ2JCall       jitStencilFallJ2JCall_      CORRECTNESS          stencil signals trampoline must
                                                                    orchestrate the J2J frame, not
                                                                    inline further
ExitOther         jitStencilFallOther_        CORRECTNESS          BlockCreate / ArrayCreate /
                                                                    MustBool / Deopt / PrimFail /
                                                                    ArithOverflow — stencil cannot
                                                                    synthesize the handler
```

The headline 5% is dominated here. **ExitSendCached + ExitSend are the prize**:
both are missed-opt, both are method sends, both are high frequency. ExitJ2JCall
and ExitOther are correctness-required and out of scope for continuity lift.

### Site 2 — block-frame-push primitive (Interpreter.cpp:28171–28174). MEDIUM.

A primitive succeeded (PrimitiveResult::Success) but pushed a frame
(`frameDepth_ != primCallerDepth`, ~28089). Frame-pushing prims (block-activation
81/82/201–209, perform 83/84) need interpreter dispatch to drive the activated
frame. The loop *already* attempts inline block resume for prims 207/209 via
tryExecute (~28161), gated by:

```
guard                          file:line                   class
----------------------------   -------------------------   -----------
!g_debug.t1NoBlockResume       Interpreter.cpp ~28109      knob (A/B)
blockJM found, not stub-only   Interpreter.cpp ~28109      correctness
!blockJM->hasNLR               Interpreter.cpp 28130-28131 CORRECTNESS (2026-05-30)
prim 207/209 only              Interpreter.cpp ~28109      scope
```

Success path increments `jitJ2JActChains_` (~28164); failure/guard-reject
increments `jitJ2JActFalls_` + `jitChainPrimFalls_` (~28171). Missed-opt except
the hasNLR guard, which is correctness-required.

### Site 3 — activateMethod frame-push (Interpreter.cpp:28269–28270). NEGLIGIBLE.

Send target is non-JIT or has an unsafe primitive; `activateMethod` (~28248)
pushes an interpreter frame. Structural — a non-compiled target MUST run in the
interpreter. ~0 in AWFY (comment ~28298). No correctness guard, no lift available
beyond "compile the target" (out of scope; the compiler is the bottleneck).

### Continuity arithmetic

```
continuity = chains / (chains + falls)
           = chains / (chains + site1 + site2 + site3)
```

Lifting continuity = converting site-1-missed-opt (ExitSendCached, ExitSend) and
site-2-missed-opt (non-NLR block resume) falls into chains, WITHOUT regressing the
correctness-required falls (ExitJ2JCall, ExitOther, hasNLR, stub-only).

---

## 4. INVARIANTS TO PRESERVE

The "do not break" list. Every milestone gate must keep ALL of these green. Each
is a real bug that a guard now prevents; "chain more aggressively" is exactly the
change that risks re-opening them. Hazard region: Interpreter.cpp 20960–22230
(rj2j chain loop) and 25700–28400 (main dispatch + J2J activation).

```
#   invariant                         guard file:line                        protects against
--  -------------------------------   ------------------------------------   ----------------------------
1   hasNLR block must not fast-       Interpreter.cpp:28130-28131            ContextTest>>testBlock-
    resume (NLR returns nil ->          JITRuntime.cpp:2155                  CannotReturn hang (DNU
    stale outer IC -> DNU loop)                                             infinite recursion)
2   clear cachedTarget/icDataPtr/     Interpreter.cpp:21096-21106            wrong method at wrong offset
    sendArgCount BEFORE every           Interpreter.cpp:28155-28157          (minExtent width-coordinate
    resume re-entry                                                          corruption)
3   no BV-inline with hasRemoteTemp   JITRuntime.cpp:2161-2162               lost captured-var writes
                                                                            (startup runaway)
4   no BV-inline with                 JITRuntime.cpp:2164-2173               wrong frame-slot reads
    numCopied > PHARO_BV_MAX_CAP                                            (multi-capture corruption)
5   J2J-call conversion must verify   Interpreter.cpp:21476-21524            frame re-execution on return
    packable resume (bcOff<=0xFFF,      Interpreter.cpp:24391                (RESUME-MISMATCH, whole
    nArgs<=15, getBcEntryState==0)                                          caller body re-run)
6   materialize must validate         Interpreter.cpp:24379-24391            wrong-offset bytecode resume
    saveIp in saveJM bytecode range     Interpreter.cpp:24400-24404          (silent eval loss / DNU)
    + resume-addr in JIT code
7   cull: sends skip mega-IC patch    Interpreter.cpp:14351-14365,           1MB-aligned stale receiver
    + trap suspect receivers            23102-23109, 26205-26233            -> classIndex 0 -> nil DNU
8   stub methods must not enter       JITRuntime.cpp:2145                    block return-prelude mismatch
    block-value-inline                  Interpreter.cpp:27209-27216
9   every materialize-failure site    Interpreter.cpp:21126-21157,           partial frame-stack
    rolls back frameDepth_ + bails      21827-21846, 24289-24302            corruption (silent)
10  SP/FP always consistent with      Interpreter.cpp:590-621,               stack overflow/underflow
    frame depth; sync+trace after       21176-21178, 21852-21854,          (heap/code-zone corruption)
    every handler                       22186/22285/22340 ...
11  GC-during-chain: set              Interpreter.cpp:21023-21029,           stale receiver/ip after
    currentJITState_ before loop;       21280-21288, 19595-19610,          scavenge (invisible saves)
    reserve j2jPoolCursor_ before       19800-19806
    allocating ops
12  exit handlers sync ALL frame-     Interpreter.cpp:21900-21919,           stale method_/receiver_/
    identity globals before             22089, 22187-22195, 21163-21184    argCount_ -> wrong method run
    downstream calls
13  context pc=nil sends              Interpreter.cpp:18832-18843            nil-PC infinite loop
    cannotReturn:, not method reset                                         (suite hang under det-sched)
```

Cross-cutting open correctness issues feeding these guards:

- **PHARO_T1_X86_XMETHOD_SENDS** (x86 nested J2J): ExitSendCached inside a stencil
  cannot currently chain on x86 because the J2J cursor reset (~27475) orphans the
  outer caller's pending save. Documented in `docs/x86-nested-j2j-design.md`. This
  is *the* thing standing between us and lifting site-1 ExitSendCached. Arm64 is
  the safer first target.
- **Bug 11b layer 4** (Interpreter.cpp:28974): savedMethod can be nil in release
  builds (FreeBlock reuse); stencil fallback null-guards frame materialization.

---

## 5. MILESTONE BREAKDOWN

Mirrors the FSR M0–M4 shape: **establish invariant -> verify-oracle -> enable ->
measure**. Each milestone lifts ONE missed-opt fall reason, safest/highest-frequency
first, behind a binary kill-switch knob (all knobs in `src/vm/debug_vars.h` per the
frozen-DebugSettings rule). No milestone may regress any section-4 invariant.

Knob naming convention (follows FSR `PHARO_T1_FSR*` / opt-out `PHARO_T1_NO_*`):
- `PHARO_T1_CHAIN_*` master + per-milestone sub-knob.
- `PHARO_T1_CHAIN_*_VERIFY` dual-oracle sub-knob (verify before enable).
- After flip: invert to `PHARO_T1_NO_CHAIN_*`.

### M0 — Establish baseline + verify-oracle (NO behavior change)

- **Scope:** Re-measure the four counters fresh on current HEAD; build the
  per-site attribution table. Add `PHARO_CHAIN_VERIFY` dual-oracle: at each fall
  site, cross-check the in-VM counter against a PHARO_ACTIVATION_LOG-derived tally
  and trap `[CHAIN-VERIFY]` on divergence. NO emit/control-flow change.
- **Invariant risked:** none (measurement only).
- **Verify-oracle + kill-switch:** `PHARO_CHAIN_VERIFY` (counters vs activation
  log agree, zero traps). `PHARO_T1_NO_BLOCK_RESUME` already exists as the site-2
  A/B baseline lever — record continuity with it on and off.
- **Metric delta to confirm:** none expected; deliverable is the *table*:
  per-site fall counts, site-1 exit-reason split, `jitPrimFallHisto_` top-10
  offenders, `jitJ2JStencilReturns_` vs `jitJ2JActChains_` (stencil-success vs
  resume-success split). This table tells M1+ where the bytes are.
- **SUnit-ladder gate:** kernel SUnit + `classify-sunit.py` Δcog == baseline
  (must be byte-identical; measurement-only).

### M1 — Site-1 ExitSend resolution stays in JIT (arm64 first)

- **Scope:** When a stencil exits `ExitSend` (uncached/mismatched target,
  `jitStencilFallSend_`, Interpreter.cpp:28046), perform the fresh send lookup
  *inside the chain loop* and re-enter the resolved callee in JIT via
  `upgradeICToJ2J` (Interpreter.cpp:23638) + chain continue, instead of
  materializing and falling to the interpreter. ExitSend (not ExitSendCached) is
  the safest site-1 lever: no IC-already-populated nesting hazard, the lookup is
  fresh, and the existing `upgradeICToJ2J` eligibility checks (23784–23964) already
  gate unsafe primitives.
- **Invariant risked:** #2 (stale IC fields — must clear before re-entry, 21096),
  #5 (packable resume — refuse conversion if unpackable, 21476), #12 (frame-identity
  sync before re-entry).
- **Verify-oracle + kill-switch:** `PHARO_T1_CHAIN_RESEND` + `_VERIFY`. Verify mode:
  do the in-loop resolution AND the old materialize/fall in parallel, assert the
  resolved method/receiver/ip match, trap on divergence. Run verify-clean before
  enabling.
- **Metric delta to confirm:** `jitStencilFallSend_` drops; `jitJ2JActChains_`
  rises by the same count; continuity % up; `sample` shows less time in
  materializeJ2J + dispatch re-entry. No rise in any other fall sub-counter.
- **SUnit-ladder gate:** kernel SUnit Δcog == 0 new regressions; bench 5/5 within
  noise; PHARO_DET_SCHED A/B identical.

### M2 — Site-2 non-NLR block resume widening

- **Scope:** Extend the block-resume fast path (Interpreter.cpp:28090–28167) to
  more frame-pushing block prims and reduce tryExecute-failure falls. Stay strictly
  inside the existing guard envelope: `!hasNLR` (#1), `!isStubOnEntry` (#8),
  `!hasRemoteTemp` (#3), `numCopied <= PHARO_BV_MAX_CAP` (#4). Target the top
  `jitPrimFallHisto_` block-prim offenders identified in M0.
- **Invariant risked:** #1 (hasNLR — the 2026-05-30 fix; must NOT relax),
  #8 (stub-only), #3/#4 (BV capture limits), #2 (clear IC at 28155).
- **Verify-oracle + kill-switch:** `PHARO_T1_CHAIN_BLOCKRESUME` + `_VERIFY`;
  `PHARO_T1_NO_BLOCK_RESUME` is the existing master off-switch for the whole path.
- **Metric delta to confirm:** `jitChainPrimFalls_` drops for the targeted prims
  (`jitPrimFallHisto_` shrinks at those indices), `jitChainPrimChains_` rises,
  continuity % up. hasNLR/stub-only fall counts UNCHANGED (proof the guards held).
- **SUnit-ladder gate:** include block-heavy + NLR test classes explicitly
  (ContextTest, BlockClosureTest); Δcog == 0; PHARO_DET_SCHED A/B identical
  (block resume is timing-sensitive — det-sched mandatory).

### M3 — Site-1 ExitSendCached nesting (arm64 only, GATED, riskiest)

- **Scope:** The big prize and the most dangerous. Chain `ExitSendCached`
  (`jitStencilFallSendCached_`, the largest single site-1 bucket) by making the
  nested J2J save/restore robust enough to keep an IC-resolved send in JIT.
  **arm64 only** — x86 nested-send corruption (PHARO_T1_X86_XMETHOD_SENDS,
  `docs/x86-nested-j2j-design.md`) is unresolved; gate this entirely off on x86.
  Depends on the save-pool / cursor reservation discipline (#11) and packable-resume
  predicate (#5) being airtight.
- **Invariant risked:** ALL of #2, #5, #6, #9, #10, #11, #12 simultaneously — this
  is the deepest nesting change. Treat each as a separate dual-write assertion.
- **Verify-oracle + kill-switch:** `PHARO_T1_CHAIN_SENDCACHED` + `_VERIFY`,
  arm64-compile-gated. Verify mode dual-writes the materialized-fall path alongside
  the nested-chain path and asserts the nested save round-trips (push then pop
  restores exact caller sp/receiver/tempBase/ip). Long soak before enable.
- **Metric delta to confirm:** `jitStencilFallSendCached_` drops sharply
  (this is where the bulk of the 95% lives); continuity % makes its largest jump
  toward target; `sample` boundary machinery % drops below 20%.
- **SUnit-ladder gate:** FULL kernel SUnit Δcog == 0; GC-stress (allocation-heavy
  forced scavenges, exercises #11); PHARO_DET_SCHED A/B + 20K-test soak; bench
  19/19. Decision point: if M1+M2+M3 cumulative continuity < target, re-scope.

### M4 — Cleanup + default-on flip

- **Scope:** After M1–M3 soak clean, default the knobs on, invert to
  `PHARO_T1_NO_CHAIN_*` opt-outs, retire `_VERIFY` dual-write arms, delete the
  dead BV path (section 7). No new behavior.
- **Invariant risked:** none new (removing scaffolding only).
- **Verify-oracle + kill-switch:** the `PHARO_T1_NO_CHAIN_*` inverts remain as
  emergency off-switches.
- **Metric delta to confirm:** continuity % and boundary-machinery % match the
  M3-enabled numbers with verify off (byte-identical control flow at knob-off).
- **SUnit-ladder gate:** full-suite soak; Δcog == 0; bench 19/19.

---

## 6. VALIDATION METHODOLOGY

Same toolchain FSR used (the playbook is proven; reuse it verbatim).

### Verify oracle (per-milestone)

Dual-path: keep the old materialize/fall AND the new in-JIT chain running in
parallel under `*_VERIFY`, assert they agree on resolved method / receiver / ip /
sp-depth, trap `[CHAIN-VERIFY]` on any divergence. Soak verify-clean before
enabling the new path. This is the M0-establishes / M1+-deletes discipline.

### Kernel SUnit ladder

```
# stage the shared external test-class list (fairness: same senders both VMs)
cp scripts/pharo-headless-test/test_classes.txt /tmp/sunit_test_classes.txt

# custom VM (installs SUnitRunner + SessionManager startup handler, then runs)
/tmp/harness/pharo /tmp/harness/Pharo.image eval --save \
  "'$PWD/scripts/pharo-headless-test/run_sunit_tests.st' asFileReference fileIn"
./build/test_load_image /tmp/harness/Pharo.image

# stock-Cog baseline (separate image)
/tmp/harness/pharo /tmp/harness/Pharo.image eval \
  "'$PWD/scripts/pharo-headless-test/run_sunit_cog.st' asFileReference fileIn"

# delta vs Cog: regressions = tests passing on Cog but not on our JIT under test
scripts/classify-sunit.py cog.txt ours.txt
```

`classify-sunit.py` `delta_vs_cog()` (line ~70) is the gate: every milestone must
add ZERO new Δcog regressions. Per-milestone, narrow with `/tmp/sunit_class_names.txt`
to the hazard classes that milestone risks (M2: ContextTest, BlockClosureTest;
M3: full suite).

### PHARO_DET_SCHED for Heisenbugs

The chain loop is timing-sensitive (block resume, nested sends fire under specific
fork interleavings). Any counter/trace probe shifts wall-clock heartbeat timing and
can hide the bug. Use `PHARO_DET_SCHED=1` (drives force-yield from the per-1024-bytecode
checkpoint, `g_stepNum`, Interpreter.cpp:2637, disabling the heartbeat) so the bug
reproduces at a FIXED point and *keeps* reproducing under lldb/trace. Every M2+ gate
includes a DET_SCHED A/B; `PHARO_DET_SCHED_QUANTUM=N` widens the yield interval.
Reach for this FIRST on any "works under debugger, fails otherwise" chain bug.

### Bench gate

```
sista_loop_bench.st      self-contained fusion bench, seconds, /tmp/sista_bench.txt
benchFib / cfib          5x5 interleaved quiet A/B @ -O2, medians (build-opt only)
full bench-suite         19/19 matrix, dual-knob states
```

Perf numbers only from `build-opt` (-O2). Clean rebuild (`rm -rf build build-opt`)
at any milestone touching JITState offsets. Validate both -O0 and -O2.

### Gate discipline (every milestone)

```
- byte-identical codegen at knob-off (binary verification)
- sp-depth checker clean (PHARO_SP_DEPTH_CHECK, no [SP-DEPTH] logs)
- per-test SUnit A/B identical across knob states
- continuity-% delta in the predicted direction (counters)
- sample boundary-machinery % delta confirms real time moved
```

---

## 7. KNOWN CRUFT TO REMOVE

**Dead BV-specialization path** — `jit_rt_inline_block_value_prep`
(`src/vm/jit/JITRuntime.cpp:2025`).

- Measured **fires=0** in all runs (fall-taxonomy + j2j-infra maps). Returns
  nullptr on every bail condition (PHARO_BV_FORCE_BAIL, debug gates, receiver not
  a closure); never reaches upstream. Marked in-code as a ~0.0006% path like
  `jit_rt_j2j_call`.
- The block-value inline JIT entry stub that would call it does not fire for
  block-value dispatch in measured runs.
- Session H moved the BV closure saves to `bvClosureSaveStack_` (Interpreter.hpp:791)
  for GC walkability, but the prep logic itself is unexercised.
- **Action (M4):** delete `jit_rt_inline_block_value_prep` and its entry stub,
  the `PHARO_BV_FORCE_BAIL` knob, and any `PHARO_BV_MAX_CAP`-only-for-prep code.
  KEEP the BV capture-limit guards (#3, #4) that protect the *live* block-resume
  path (Interpreter.cpp:28090) — those are not part of the dead prep path. Confirm
  with a `[BV-PREP-FIRED]` counter trap (must stay 0 across full suite) before
  deleting.
- Note: "BV-spec path dead" is also listed in section 2 as ruled-out; this is the
  same code, removed here rather than investigated.

---

## 8. RESUMPTION NOTES

A fresh session starts here. Everything needed is above; this is the fast path.

### Key file:line anchors

```
chain loop (main)            src/vm/Interpreter.cpp ~27300-28340  (tryJITActivation)
  fall site 1 (stencil)        Interpreter.cpp:28043-28049
  fall site 2 (block prim)     Interpreter.cpp:28171-28174 ; block-resume 28090-28167
  fall site 3 (activate)       Interpreter.cpp:28269-28270
rj2j chain loop + hazards    src/vm/Interpreter.cpp 20960-22230
upgradeICToJ2J (re-enter)    src/vm/Interpreter.cpp:23638
clear-IC-before-resume       src/vm/Interpreter.cpp:21096-21106   (invariant #2)
packable-resume predicate    src/vm/Interpreter.cpp:21476-21524   (invariant #5)
hasNLR block guard           src/vm/Interpreter.cpp:28130-28131   (invariant #1)
counters                     src/vm/Interpreter.hpp:1471-1492
stencil sendJ2J / direct     src/vm/jit/stencils/stencils.cpp:1469 / label 1843
J2J_INLINE_RETURN macro      src/vm/jit/stencils/stencils.cpp:562
dead BV prep (delete)        src/vm/jit/JITRuntime.cpp:2025       (section 7)
x86 nested-send blocker      docs/x86-nested-j2j-design.md         (gates M3 off on x86)
knobs                        src/vm/debug_vars.h                   (add PHARO_T1_CHAIN_*)
```

### The metric command

```
# build, run the suite, then read the counters out of the run.
# continuity = jitJ2JActChains_ / (jitJ2JActChains_ + jitJ2JActFalls_)
# site split: jitStencilFall{SendCached,Send,J2JCall,Other}_  (Hpp:1489-1492)
# block-prim offenders: jitPrimFallHisto_[]                    (Hpp:1482)

cmake --build build
printf 'all\n' > /tmp/sunit_class_names.txt   # or a narrow hazard class
./build/test_load_image /tmp/harness/Pharo.image
# (counters are dumped at run end; M0 adds PHARO_ACTIVATION_LOG cross-check)
```

### First concrete M0 step

1. Add `DEBUG_BOOL(PHARO_CHAIN_VERIFY)` to `src/vm/debug_vars.h` (one line, per the
   X-macro convention; NOT in DebugSettings.cpp, which is frozen).
2. At each of the three fall sites (Interpreter.cpp:28043, 28171, 28269), under
   `GET_DEBUG_BOOL(PHARO_CHAIN_VERIFY)`, log the fall with its exit reason / prim
   index / target-method into a per-site tally, and at run end assert the tally
   matches the existing counter (trap `[CHAIN-VERIFY]` on divergence).
3. Run the kernel SUnit suite once with the verify on, once with it off; confirm
   Δcog identical (measurement-only must not perturb behavior).
4. Emit the M0 attribution table: per-site fall counts, site-1 exit-reason split,
   `jitPrimFallHisto_` top-10, `jitJ2JStencilReturns_` vs `jitJ2JActChains_`.
   This table is the input to M1 (it tells you whether ExitSend or ExitSendCached
   is the bigger arm64-safe prize, and which block prims M2 should target).

Do NOT start M1 emit changes until the M0 table exists and the verify oracle soaks
clean. Establish -> verify -> enable -> measure, in that order, every milestone.
