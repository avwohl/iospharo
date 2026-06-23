# Plan: T1 JIT send-path — close the 3.5x-vs-Cog gap (corrected)

Scoped via a 6-agent design workflow + adversarial verify (returned **needs-
revision**; this is the CORRECTED plan). Read with `docs/results-perfdb.md` and
`docs/patched-ic-design.md`. The send path is THE remaining lever for the goal's
send-heavy real workloads (sends are 3.3x denser than arith; our JIT sends are
3.5x slower than Cog; the send cost dominates the 7.8x SUnit-CPU gap).

## Per-send anatomy (workflow, file:line in AsmjitT1.cpp arm64)

Every send pays a **~13–14 instr / ~4 load probe head** (`6043–6077`): receiver
load, SmI-tag check, a **leak guard** (`lsr #48; cbnz` — 2 instr Cog lacks,
`6068`), **class-key recomputed from the header every send** (`ldr w4,[x1]; and
#0x3FFFFF` — Cog caches a class-index reg, `6072`), and an **out-of-line IC
pointer+key load** (Cog embeds the cache inline at the call site, `5955/6212`).
Cog's monomorphic send is ≈`cmp class, b.ne miss, bl target` (~4 instr). So the
head alone is ~9–10 removable instr + ~3 loads.

After the head the send diverges by terminal outcome:
- **inline-J2J** (self-recursive only; `7550–7856` push + `4650–4814` return):
  ~40 instr + ~15 mem ops of software-frame save/restore (V2 = 40-byte/5-word
  save, `J2JSaveLayout.h`), no Cog analogue — Cog uses a hardware call/ret. Mostly
  irreducible on the software-frame design; it's the BV/sender-chain corruption
  surface.
- **dispatchCached** (`9509–9548`): NOT a cached direct call — a **JIT→C++→JIT
  round-trip** (stores state, `EXIT_SEND_CACHED`, `ret x30` back to the C++ chain
  loop, which re-enters the callee). This is the dominant cost for send-heavy
  SUnit/soogle, where inline-J2J rarely fires.

## ⚠️ What the adversarial verify corrected (do NOT skip)

1. **The plan's headline lever — "activate the inert PMS direct-call" — is ALREADY
   SHIPPED.** PMS/patchedShape is default-ON (`5939`, opt-out only) and
   `linkSendSite` (`JITRuntime.cpp:3350–3548`) is fully implemented + runtime-wired
   (called on recompile + every slot-0 IC write, patches key/branch/J2J-tail). There
   is no inert PMS to turn on.
2. **PMS does not touch the slow bucket.** `linkSendSite` only links `extras&kJ2JBit`
   sites and refuses primitive/canBail/stub callees (`3390,3409`), so the linked-PMS
   population == the existing inline-J2J population. The sends that hit dispatchCached
   are the **`extras==0` unclassified / quick-prim sends** (`o class`/prim 111, `#==`,
   `#isNil`). `patched-ic-design.md:192` states verbatim that PMS CANNOT move those —
   they need a **separate B6 classifier / compiler-coverage lever**.
3. **`F` was mis-defined.** `(PMS+dispatchCached)/all` conflates the fast PMS-linked
   direct call with the slow round-trip. The real go/no-go number is the
   **`extras==0` dispatchCached fraction** (the B6-addressable bucket), broken out by
   selector.
4. **Emit-hash-identical validation is impossible** for emit-time counters (they add
   bytes). Use cpu_ms-neutral + report-sunit 0-regression instead.
5. **Horizon is over-optimistic.** PMS is already shipped and the design doc's own
   estimate for it was ~15–30% on microbenches. The remaining lever (B6 quick-prim
   classifier coverage) is **single-digit-to-~20% on send-bound**, less end-to-end on
   SUnit. The J2J software save (~13 stores + nil-fill) and the probe head are the
   floor. **Cog parity needs the native-call/checked-entry rewrite** the plan
   correctly excludes (the multi-session corruption surface behind the BV saga).

## ✅ Phase 1 EXECUTED (2026-06-23) — verdict: LARGE surface, but the safe lever is small

Implemented `PHARO_T1_SEND_CENSUS` (pure C++, no codegen change, counts the
per-execution dispatchCached round-trip at `Interpreter.cpp case ExitSendCached`
with a per-selector histogram). Correctness-neutral (SUnit 1988 pass / 0 fail / 0
error). Measured:

    workload   emitted-J2J     dispatchCached   F_addr
    bench      547,061,403     6,390,885        0.012   (fib recursion = all J2J)
    SUnit      3,308,199       2,237,309        0.403   (real send-heavy code)

**`F_addr=0.40` HITS the GO threshold** — 40% of real-code sends take the
JIT→C++→JIT dispatchCached round-trip (vs operand-stack E's 3%). The round-trip
overhead is a genuinely large, real-code cost. BUT two follow-ups correct the lever:

- **The dispatch bucket is NOT quick-prims** (the plan's B6 premise, refuted). Top
  selectors: `#at: #do: #nextPut: #value: #at:put: #buffer #nextPutAll: #value:value:
  #wordSize #cull: #instVarNamed:put: #size #basicAt:` — mostly REAL polymorphic
  method/block sends.
- **The inline-prim/getter candidates mostly already fire.** With
  `PHARO_T1_INLINE_PRIM_COUNTERS=1` on SUnit: at=607K, atPut=166K, size=48K,
  class=110K all fire — so the dispatchCached `#at:`/`#at:put:` are the NON-Array
  collection methods (`OrderedCollection>>at:`, `Dictionary>>at:`), real sends, not
  missed inline-prims. **getter=0, setter=0** — the only clear missed-inline slice
  (getters like `#buffer`).

**VERDICT (nuanced):** the addressable surface is large (40% round-trips) but the
chunk needing only SAFE incremental work is small. The big chunk is real method/
block sends whose round-trip cost is reducible mainly by the **native-call/checked-
entry rewrite the plan deliberately excludes** (the BV/sender-chain corruption
surface). The SAFE levers — Phase 2 head-trim (helps ALL sends incl. the 40%
round-trips) and inline-getter coverage (the getter=0 slice) — capture a meaningful
but bounded fraction. So: more promising than E, but Cog parity still needs the
excluded rewrite. Recommended next: **Phase 2 (head-trim)** — it's low-risk, helps
every send (the universal probe head), and is independent of the round-trip
question; measure its cpu_ms on benchFib + the send-bound bench.

## Phase 1 (CORRECTED) — cheap decisive send census (1 session)

Measure the **execution-weighted terminal-outcome split** on benchFib (send-bound)
+ a SUnit send-heavy batch, into SIX mutually-exclusive buckets: (1) inline-J2J,
(2) inline-prim/getter/setter/returnsSelf, (3) **PMS-linked direct call** (already
fast — NOT addressable), (4) PMS-unlinked→dispatchCached, (5) plain dispatchCached
(`extras==0` unclassified/quick-prim — the B6-addressable bucket), (6) IC-miss→chain.
The decision number is **`F_addr = (4+5)/all` = the dispatchCached round-trip
fraction**, with bucket 5 broken out **by selector** (to confirm it's dominated by
quick-prims like `#class`/`#==`/`#isNil` that classifier coverage could convert).
Buckets 1–3 are already maximally specialized — not addressable by any committed
phase.

LOW-RISK implementation (sidesteps the verify's emit-injection concerns): the
dispatchCached + IC-miss buckets are HANDLED IN C++ (the chain loop), so count them
there with a selector histogram — **no codegen change** (unlike the regstack census,
which was emit-time but zero-codegen, this is even safer: pure C++ counters at the
exit handler). The inline-J2J bucket reuses the always-on `J2J stencil calls`
counter (547M observed). PMS-linked (bucket 3, in emitted code) is the only one
needing care — infer it from the linked-site set or accept a bound. Validate:
report-sunit 0-regression vs the 12,898-test Cog baseline + cpu_ms-neutral.

- **GO** if `F_addr ≥ 0.40` AND bucket 5 is dominated by addressable quick-prims
  (classifier coverage can convert them) → pursue the B6 classifier lever.
- **NO-GO** if `F_addr < 0.40` (most real sends already inline-J2J/inline-prim → the
  3.5x is the irreducible head + J2J-save/activation floor; redirect to
  `tryJITActivation`, `Interpreter.cpp:24488`) OR if bucket 5 is dominated by sends
  classifier coverage can't help.

## Phases 2+ (only the corrected levers)

- **Phase 2 — trim the universal probe head. ✅ EXECUTED (2026-06-23) — PERF-NEUTRAL,
  NOT pursued.** Measured the leak-guard removal with zero code change (it's already
  gated by `PHARO_T1_LEAK_GUARD_OFF`): REPEAT=5 min cpu_ms, guard ON vs OFF —
  fib(28) 6→6 (tie, the most send-bound case), sum 60→64, sort 165→178, dict 120→125
  (within noise / slightly worse), 1M blocks 206→184 (the one improvement, but block
  activation not send dispatch). The 2 saved instructions (`lsr #48; cbnz miss`) are
  predicted-not-taken branches that cost ~0 cycles out-of-order — instruction-count
  reduction ≠ cpu_ms when the removed instrs aren't on the critical path (same lesson
  as TOS-in-register). The class-key recompute is similarly cheap ALU + a header load
  that is fundamental (Cog also loads the receiver class). So head-trim does NOT
  deliver a measurable win, and flipping the leak-guard default would add the Roassal3
  SIGSEGV-history risk for no benefit. NOT flipped. The real send cost is the MEMORY
  traffic + the dispatchCached C++ round-trip, not the cheap probe-head ALU.
- **Phase 3 (CORRECTED) — B6 classifier / compiler coverage for quick-prims (3–5
  sess, HIGH risk).** NOT PMS (already done). Make the `extras==0` dispatchCached
  quick-prims (`#class`/`#==`/`#isNil`/prim 111) get classified+inlined (an extras
  bit + inline emit) so they stop taking the C++ round-trip. Contingent on Phase-1
  showing bucket 5 is large + quick-prim-dominated. Done: bucket-5 fraction drops +
  send-bound cpu_ms improves + report-sunit 0-worse.
- inline-J2J admission widening: **deprioritized** — PMS already covers the
  J2J-eligible monomorphic population, so widening admission is low-upside / EXTREME
  risk (the BV×inline-J2J corruption just fixed at HEAD `bf17def0`). Do not pursue
  without first root-causing the value:value: corruptor.

## Horizon + kill criteria (honest)

- **Horizon:** Cog parity is NOT reachable on the software-frame design (needs the
  native-call/checked-entry rewrite, deliberately excluded). PMS — the big monomorphic
  lever — is already shipped. Realistic REMAINING win: Phase 2 head-trim ~single-digit
  on send-bound; Phase 3 B6 coverage single-digit-to-~20% on send-bound, **less
  end-to-end on SUnit**. Net: a meaningful-but-modest dent in the 7.8x SUnit gap, not
  closure. Same shape as the operand-stack direction (E): the addressable fraction is
  modest; the structural floor (software frame + IC machinery) dominates.
- **Kill criteria:** Phase-1 `F_addr < 0.40` or bucket-5 not quick-prim-dominated →
  STOP (gap is the activation/frame floor). Phase 2 can't pass a 0-regression suite
  gate twice → keep the guard. Any phase producing sender-chain corruption that
  DET_SCHED can't localize in one session → revert the knob (binary stays green),
  stop. General rule: any phase that can't hold the 12,898-baseline 0-new-regressions
  under DET_SCHED is killed, not worked around.

## Meta (both scoped directions together)

Operand-stack (E) Phase-1 = NO-GO (~3%); send-path remaining levers = modest
(single-digit-to-~20%). **The goal's send-heavy workloads have no dramatic-win lever
on the current design** — the 21x/7.8x gap is structural (software-frame calls + the
per-bytecode/per-send IC machinery of a from-scratch JIT vs mature Cogit). Closing it
needs a native-frame send rewrite (the corruption surface the project avoids) or a
matured Sista that fires for tests. The cheap executed Phase-1 measurements are what
let us say this with data instead of speculation.

## 🎯 getter=0 thread — a REAL 10x win behind a fixable correctness bug

Following the send-census `getter=0`: inline-getter IS default-on (`t1InlineGetter`),
but getter sends still hit dispatchCached because the dispatch-A-side getter entry
(`PHARO_T1_GETTER_IN_J2J`) is default-OFF for correctness (debug_vars.h:81 — corrupts
under MAX_IC=1, bail paths reach it with inconsistent register/state).

Enabling `PHARO_T1_GETTER_IN_J2J=1` and measuring (REPEAT=5 cpu_ms):

    1M getter+yourself (pt x):  default 40ms -> 4ms  = 10x  (Cog ~1ms: 43x->4x gap)
    dict 50K / sum 1M:          neutral

220K getters inline on SUnit (correctness-clean on the 10-class set: 1988 P/0 F/0 E),
dispatchCached drops ~5%. (#buffer stays in dispatch — it's a lazy-init method, not a
plain ^ivar getter, so not getter-classifiable.)

This is the FIRST genuine large lever found: a 10x getter win. It's gated off behind a
SPECIFIC documented bug, not a structural floor — so unlike the other directions, the
work here is to FIX that bug (the MAX_IC=1 dispatch-A-side register/state corruption)
so the win ships. Next: broad correctness validation (full SUnit + DET_SCHED, report-
sunit diff vs the 12,898-test Cog baseline) to see whether the bug still manifests at
HEAD or was mitigated by later fixes.

## Disabled-for-correctness re-validation sweep (2026-06-23) — full results

Survey workflow + empirical test of every arm64 perf opt disabled for a
correctness bug. Each candidate: enable via knob, confirm it FIRES (counter),
run its named repro + full SUnit (report-sunit diff vs the Cog baseline).

    candidate                       fires?   correctness@HEAD      perf            action
    getter (GETTER_IN_J2J)          220K     CLEAN (bug fixed)     10x getters     FLIPPED default-on
    returnsLiteral (bit 58)         yes      CLEAN                 ~3% SUnit CPU    FLIPPED (trio)
    tempReturn (bit 54)             yes      CLEAN                 (trio)           FLIPPED (trio)
    intArith (bit 52)               yes      CLEAN (overflow ok)   (trio)           FLIPPED (trio)
    multiSlot (bit 57)              9154x    CLEAN (#extent fixed) NEUTRAL on SUnit DEFERRED (no demo win)
    intCmp (bit 53)                 0        n/a (never fires)     -               SKIP (no coverage)
    evenOdd (bit 51)                0        n/a (never fires)     -               SKIP (no coverage)
    ADMIT_BAILMID_CALLEES           yes      STILL-BROKEN          +26% select!    SKIP (needs the fix)
    BLOCK_VALUE                     -        fixed but trade-off   block+ send-    SKIP (disables inline-J2J)
    SHARED_RETPRELUDE               -        clean (byte-id off)   zone-only        DEFERRED (unmeasurable)

KEY FINDINGS:
- Two clean wins shipped (getter 10x, the safe trio ~3% on real SUnit CPU). The
  shared 2026-06-09 dispatch-A disable cause is fixed at HEAD; the getter proved
  the path, and returnsLiteral/tempReturn/intArith carry no spec-specific defect.
- multiSlot's #extent wild-write bug is ALSO fixed at HEAD (fires 9154x, full
  SUnit 0 fail) — but perf-neutral on SUnit/bench (its benefit is geometry/GUI
  code), so deferred until a demonstrable win.
- intCmp/evenOdd never fire (integer comparisons are primitives, not `^self cmp
  arg` user methods) — no coverage, unvalidatable, skip.
- **ADMIT_BAILMID is the highest-value remaining opportunity: +26% on `select`
  (273->201ms), but CONFIRMED still-broken (SP_DEPTH_CHECK = 1 violation = the
  un-popped J2J save on V2 ExitArithOverflow). The fix (materialize/pop the save
  on V2 ExitArithOverflow — AO_MAT_J2J is currently V1-only) is a SCOPED CODEGEN
  FIX that would unlock the +26% AND the 51 PolyMath failures. Implement-first.**

## ADMIT_BAILMID fix — root-caused with a deterministic repro (2026-06-23)

Took on the ADMIT_BAILMID fix (the +26% `select` win). Built a DETERMINISTIC
repro and root-caused the bug far past the stale 2026-06-19 docs.

DETERMINISTIC REPRO (100% across reps):
- Image: /tmp/pkgtest/polymath.image (PolyMath loaded via Cog; 103 test classes).
- `printf 'PMAdditionalTest\n' > /tmp/sunit_class_names.txt` (+ test_classes.txt)
- ADMIT_BAILMID OFF -> 3 PASS;  PHARO_T1_ADMIT_BAILMID_CALLEES=1 -> 3 ERROR
  (testMatrixInversionSmall/testMatrixSquared/testTensorProduct), each
  "SubscriptOutOfBounds: 6 in a PMVector" — the off-by-one (index +1).
- ORACLE: PHARO_SP_DEPTH_CHECK=1 reports 15 JIT-side mismatches, ALL delta=+1.

ROOT CAUSE (corrects the stale "un-popped save at ExitArithOverflow" diagnosis):
- [AO-DIVERGED] (j2jDepth>0 at the AO handlers) NEVER fires -> j2jDepth==0 at
  every ExitArithOverflow. The save is NOT pending at the bail.
- The +1 originates in an INLINE-J2J'd canBailMidMethod BLOCK (isBlock=1,
  tempCount=3, op=0x5e BlockReturnTop, exit=6 ExitArithOverflow): when the block
  arith-overflows, its callee `state.sp` is left +1. The chain-loop handler
  (Interpreter.cpp:28413) faithfully copies `stackPointer_ = state.sp`, so the
  +1 propagates UP the call stack (observed at exit=2/7 sends in closed/close as
  the corrupted stack unwinds) until it becomes a wrong `at:` index.
- STRICTLY requires inline-J2J: ADMIT_BAILMID + PHARO_T1_NO_INLINE_J2J=1 -> 3 PASS.
  So the +1 is in the inline-J2J block-ENTRY sp setup (AsmjitT1.cpp:7225-7240, the
  extras-init loop + emitStoreSp(x15) using callerTempCount) or the non-RETURN
  retro-save recovery (AsmjitT1.cpp:7257), NOT the C++ AO handler (where naive
  materializeJ2J is documented to double-handle + break battery_golden).

FIX LOCATION (focused follow-up): NARROWED. The inline-J2J block ENTRY sp is NOT the
bug — the crossSaveless entry (AsmjitT1.cpp:7196-7211) loads the callee's OWN tempCount
at runtime and sets sp = tempBase + tempCount*8 correctly (only the self-rec path at
7215 uses compile-time callerTempCount, valid since callee==caller). So the +1 is in
EITHER (a) the arith-overflow STENCIL exit-sp for a frame entered via inline-J2J (it
restores entry-SP for C++ re-execution; verify it matches the block's actual frame),
OR (b) the non-RETURN retro-save recovery (AsmjitT1.cpp:7257) that rebuilds the elided
save on the ExitArithOverflow bail. Needs lldb-level tracing of state.sp at the arith
stencil's overflow exit vs the depth-map expected, on the deterministic repro (break
at g_mismatches++, BcDepthMap.cpp:569, exit=6 isBlock=1). Validate any fix:
PMAdditionalTest 3->0, SP_DEPTH 15->~0, battery_golden clean, full SUnit report-sunit
0 new regressions, PolyMath full (the 51), then flip ADMIT_BAILMID default-on for +26%
select. NOT shipped: delicate emit surgery on the BV-saga arith-overflow/J2J-save path;
rushing it risks new corruption.
