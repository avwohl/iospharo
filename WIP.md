# WIP — JIT Cog-speed: inline-J2J DEFAULT-ON (2026-06-09/10)

Goal (active /goal): **fix this jit to work and be as fast as cog.**

## CHECKPOINT 2026-06-12nnn — scanFor: ping-pong data complete; r2-miss exits despite healthy sites

[IC-SITE] dump on the live scanFor: jm (steady state): sites HEALTHY
— at: pk14+b60(+57), == pk10, key bit63-getter, hash poly-b60.  The
stale-extras hypothesis is DEAD.  Exit histogram: r2(EXIT_SEND, IC
MISS)=8191 : r7=1 — every jit entry of scanFor: exits ONCE as an IC
MISS, then the activation finishes in the INTERP (the 13M census
sends; ipOff 87/96 = probe loop 1; lrk=2).
OPEN QUESTIONS, precisely: (1) WHICH site misses (r2's ip needed —
extend the histogram to bucket by exit ipOff); (2) why does the
post-miss resume leave the interp running the WHOLE loop (resume-ok
counters say resumes succeed globally — maybe the EXIT_SEND handler
path for THESE sends doesn't attempt resume: check the chain
EXIT_SEND (miss) handler vs SENDCACHED — the MISS path may
activateMethod + interp WITHOUT the kind-1 precompute!);
(3) resume-kind 2 = which site (Interpreter.cpp:24235)?
HYPOTHESIS RANKED FIRST: the IC-MISS handler (EXIT_SEND) lacks the
precomputed-resume machinery that SENDCACHED has — after a miss the
caller continues in INTERP BY DESIGN, and scanFor: misses ONCE PER
ACTIVATION at a polymorphic/rotating entry (the hash site is 5-way
poly = entry rotation/thrash!) -> every activation = miss -> interp.
FIX SHAPE: per-activation miss should still resume the caller in
JIT after the C++ lookup (mirror the kind-1 rung onto the miss
path), or widen the hash site's entries (IC_ENTRIES_PER_SITE=6,
5 used — borderline).  Queue re-arm soak PASSED 2468/0/0.

## CHECKPOINT 2026-06-12mmm — queue-drop re-arm shipped; the scanFor: ping-pong = THE dict question

SHIPPED: compile-queue full -> silent drop -> permanent starvation
FIXED (queueInitialCompile returns drop status; countMap re-arms).

THE REMAINING DICT MECHANISM (precisely bounded now):
- Dictionary>>scanFor: (oop printable via [SF-SEND-CTX]) IS compiled
  (nBC=94, resumable), resumes succeed (RESUME-REFUSE ok=14M+,
  refusals startup-frozen), the gates are open... yet ~13M at:/key
  sends per 800K-op dict run execute in the INTERP from ipOff 87/96
  (probe loop 1), and the steady profile is the per-return
  returnValue -> tryJITResumeInCaller -> activateMethod chain.
- HYPOTHESIS: resume-succeed -> immediate re-bail ping-pong: each
  interp RETURN into scanFor: resumes it in JIT; the jitted code
  hits the NEXT send/op and bails again (one C++ round trip per
  scanFor:-interior send, ~16/op).  WHAT bails?  ipOff 87 = the
  at: send; its IC extras may lack pk14/bit60 (filled before at:
  compiled?) and never self-heal — OR the to:by:do: loop's
  non-local exit shape.  NEXT PROBE: extend [IC-SITE] dump to
  scanFor:'s jm (PHARO_JIT_TRACE_OOP=<scanFor: oop from SF-SEND-CTX>)
  — read sites at steady state; if extras stale -> the fix is IC
  self-healing (upgrade non-empty non-bit60 entries when the target
  compiles later: rewriteIcEntriesAfterRecompile only rewrites
  EXISTING bit-60s; INITIAL compiles never retro-fill sites that
  cached extra=0!!! <- likely THE mechanism: scanFor: sites filled
  while at:/callees uncompiled stay extra=0 forever; the per-send
  ExitSendCached -> upgradeICToJ2J should heal... 'only upgrade when
  extra==0' includes extra==0 ✓ heals... unless these sites carry
  TRIVIAL bits).  Soak of re-arm in flight.

## CHECKPOINT 2026-06-12lll — cond-jump resume DEFAULT-ON: dict 218-232 vs Cog 26 (8.5x)

The day's third default flip, same pattern: a gate whose
justification evidence was contamination-era, requalified clean
after the PMS fix.  Cond-jump send methods (scanFor:!) now advertise
mid-method resume (opt-out PHARO_T1_NO_RESUME_CONDJUMP; marked
198/199 still excluded).  Soaked 2468/0/0, sieve 1028.

DAY TOTAL (one same-image day): dict 437 -> 218-232 (Cog 26-27);
fib30 ~18 (Cog 6).  Gap: 16.8x -> 8.5x dict, 3x fib.
Shipped defaults today: sieve-fix+gate-off, prim-prologue admit,
at:-family leaf, MAX_IC=8, b46 admit, cond-jump resume, PMS/noJ2J
auto-disable, ec-hygiene.

NEXT: (a) fresh dict census (what's STILL 18M interp at:/key? the
resume flip should collapse scanFor: residency — VERIFY); (b) the
LEAF_ALL flicker (arith leaves; jjj leads); (c) per-call sequence
shrink (cfib-anatomy levers); (d) fib30 gap (2.7-3x) = the per-call
J2J sequence + send-site costs — census-driven.

## CHECKPOINT 2026-06-12kkk — J2J GATES OPENED: dict ~251-268 (Cog 25, ~10.3x); bail rate 92%->2%

THE POISONED-EVIDENCE DIVIDEND: with the PMS/NO_INLINE_J2J fix in,
requalified both remaining gate classes — their corruption evidence
was contamination-era.  MAX_IC default 1->8 (numic 1.77M->7K);
canBailMidMethod callees admitted (b46 1.83M->0; opt-out
PHARO_T1_NO_BAILMID_CALLEES).  Census: 3.4M enters, ~71K total
bails (~2%, was 92%).  dict 251-268 vs 292-303 (~14%, 3/3).
fib30 16-17.  Suite 2468/0/0.  Sieve 1028.

CURRENT GAP: dict ~255 min vs Cog 25 => ~10x.  fib30 16 vs 6 =>
2.7x.  REMAINING DICT COST (new census needed): with gates open,
sends now inline-J2J — the per-send cost is the inline-J2J call
sequence itself (the ~43-insn census from the cfib anatomy) + interp
residency of whatever still escapes.  NEXT LEVERS:
(a) re-run the CPP-SEND-CENSUS + sample profile on dict (what
    remains in C++ now?);
(b) LEAF_ALL flicker — STILL open (4/8; J2J-on + arith leaves);
    suspect list in jjj; with gates open its value rose (arith
    leaves would now inline);
(c) the per-call sequence shrink (patched-IC B5 saveless tail,
    nil-fill specialization — the cfib-anatomy levers).

## CHECKPOINT 2026-06-12jjj — LEAF_ALL: two more fix attempts falsified; full evidence inventory

Flicker facts (all measured this window, post-PMS-fix toolbox):
- 4/8 with inline-J2J ON; 0/8 with leafs off; 0/3 with J2J off.
- NOT cured by: NO_PATCHED(2/6) NO_RESUME(3/6) NO_XGATE(3/6)
  NO_J2J_PRIM_PROLOGUE(4/6).
- Frame identity SOUND at all 3 ArithOverflow handlers (diverged
  probe: only j2jD=1 hits, 6M lines pre-DNU, weak causality).
- FAILED FIX #1: identity sync at handlers (regressed suite).
- FAILED FIX #2: materializeJ2J at chain handler (8/8 WORSE —
  double-handles; return-false context has downstream save handling).
- Bisect (workflow): minimal skip set = the 14 arith/cmp/==/species
  selectors; comparisons central (skipping all-but-comparisons =
  13/16 WORSE, non-monotone).
NEXT IDEAS (fresh window): (a) trace the SAVE LIFECYCLE for a
flicker run — tag each save push/pop/materialize with a counter and
find the save that gets popped/materialized TWICE or never (the
non-monotone knob behavior smells like double-pop); (b) suspect the
COMPARISON leaf bail's interaction with the cond-jump CONSUMER: a
leafed < is called, prologue fails (non-SmI), interp computes the
body, returns a Boolean — but the JIT CALLER site expected the
result for an immediately-following conditional jump emitted as a
FUSED compare-branch?? (check if cmp-prims at IC sites fuse with
jumps — the bcArithBail counters); (c) species (prim 111) always-
fail leaf is in the minimal set — its every call bails; check what
its callers' sites assume.
Production state: default ladders 3/3, PMS fix soaked 2468/0/0.
dict ~253-283ms vs Cog 25 (~10x); fib30 16-18 vs 6 (~3x).

## CHECKPOINT 2026-06-12iii — PMS/NO_INLINE_J2J defect FIXED; workflow repro invalidated; flicker isolated to J2J-on

MAJOR: PHARO_T1_NO_INLINE_J2J=1 ALONE was deterministically broken
(5-7 DNUs) since >=2026-06-11 — PMS patched sites assume the
inline-J2J tail shape; patching tail-less sites corrupts words.
patchedShape now requires inline-J2J.  SOAKED 2468/0/0.
CONSEQUENCES: every NO_INLINE_J2J bisect arm since 06-11 was
poisoned, INCLUDING the closure-DNU workflow's 'deterministic repro'
(LEAF_ALL+NO_INLINE_J2J 8/8) — that was 100% the PMS bug (3/3 clean
post-fix), which is why the handler-completion synthesis failed.
AO-DIVERGED probes (in-tree, log-only): frame identity SOUND at all
three ExitArithOverflow handlers under the repro.

REMAINING: the genuine LEAF_ALL flicker = 4/8 with inline-J2J ON
only (arith leaves + inline-J2J interaction).  Saveless non-RETURN
recovery (retro-save) reviewed: looks correct.  NEXT: knob matrix on
the flicker config (NO_PATCHED / NO_RESUME / saveless force-off /
XGATE fold off), 8 runs per arm; then per-arith-prim leaf bisect
(PHARO_T1_SKIP_SELECTORS on +,-,<..., 8 runs/arm).  Production
unaffected (at:-family leaf, 2468/0/0 throughout).

## CHECKPOINT 2026-06-12hhh — handler completion REGRESSED + reverted; suite clean; rollback probe next

Final state of the closure-DNU arc this window:
- Synthesis's handler fix: REFUTED on the repro AND regressed the
  suite (groupsOfAtATimeCollect 2467/0/1 -> 2466/0/2; the identity
  sync clobbers framePointer_ on hot real-body arith bails).
  REVERTED with an in-code tombstone note.  Suite re-verified
  2468/0/0 twice (one IntervalTest TIMEOUT flake, non-reproducing).
- ec-hygiene KEPT (leaf refused for store-at-body-start prims).
- LESSON: 'complete the thin handler to match siblings' is NOT
  automatically safe — the siblings run in contexts where state
  identity is authoritative; ExitArithOverflow fires mid-REAL-method
  where interp globals are already correct and state mirrors may be
  coarser.  Sibling-parity arguments need per-exit context analysis.
- OPEN (the one blocker for arith leafing / MAX_IC / numic 1.77M):
  the LEAF_ALL closure-as-receiver bug.  NEXT PROBE per the
  synthesis refutation arm: counters + logs on the materialize
  ROLLBACK paths (Interpreter.cpp materializeJ2J lambda rollback
  ~24336-41: silently drops ALL materialized frames on a mid-pool
  materializeJ2JSaveIntoFrame failure; also the rj2j rollback
  ~20770-80).  Ask WHY materializeJ2JSaveIntoFrame fails, make
  failure loud, fix the drop.  Deterministic repro:
  PHARO_T1_LEAF_ALL_PRIMS=1 PHARO_T1_NO_INLINE_J2J=1 PHARO_DET_SCHED=1
  eval 3+4 -> 4/4 fail, 5 dnus.

## CHECKPOINT 2026-06-12ggg — closure-bug synthesis REFUTED; handler hygiene kept; rollback paths next

The 4-agent workflow's synthesis (thin ExitArithOverflow handlers =
the mechanism) was tested per its own falsification plan and
REFUTED: completing both handlers (materializeJ2J + audit-S2
identity sync, sibling parity) did NOT fix the LEAF_ALL repro —
deterministic variant (LEAF_ALL+NO_INLINE_J2J+DET_SCHED) still 4/4
fail (5 dnus each); flicker config now 2-3 dnus/run.  Default config
6/6 CLEAN — the handler completion is kept as hygiene (it matches
the audited sibling shape; soak pending /tmp/soak_handlers.log).

PER THE SYNTHESIS'S REFUTATION ARM, the search moves to the
materialize-ROLLBACK paths: Interpreter.cpp ~24336-24341 (the
materializeJ2J lambda rolls back ALL materialized frames and
SILENTLY RETURNS on a mid-pool materializeJ2JSaveIntoFrame failure
— frames dropped, the no-workarounds anti-pattern) and ~20770-20780
(rj2j rollback).  NEXT PROBE: counter+log on both rollbacks under
the deterministic repro — if they fire before the DNU, the fix is
making materialize failure LOUD + handling it correctly (why does
materializeJ2JSaveIntoFrame fail? its failure conditions are the
real question).  Also still open from the synthesis: the bisect
agent's claim that the hole fires 5/5 WITHOUT LEAF_ALL under
NO_INLINE_J2J — i.e. a LIVE pre-existing defect on the
NO_INLINE_J2J diagnostic config (not production).

Error-clause hygiene SHIPPED: leaf refused when bc[3] is a store
(latent <primitive:error:> hazard, 3 cold fixtures).

## CHECKPOINT 2026-06-12fff — at:-family prologue-leaf SHIPPED; dict ~253-283ms

Leaf (Cog-style, prim 60/61/62 only): prologue + interp-resume bail
(EXIT_ARITH_OVERFLOW + ip=body), numIC=0/noCondJump/notStub => full
J2J admit.  Ladder 6/6, sieve 1028.  Census: b46 169K->36K.
SCOPING LESSON: leafing arithmetic prims (1-16) flickered the
closure-as-receiver DNU — their fail paths (overflow->LargeInt) are
hot and the admitted-callee bail path has a latent bug.  That bug
(also seen as the numIC/canBailMid exemption corruption) is now THE
blocker for: arith leafing, MAX_IC raise, numic 1.77M bails
(scanFor:/at:ifAbsent: as callees).  Root-cause it next (workflow
recon + the emit-count bisect pattern; repro = leaf-all-prims build,
1-in-3 ladders, #do: receiver=FullBlockClosure).

NOTE: /tmp purge ate Pharo.image — rebuilt from Pharo-base2 (cp).
Baselines SHIFTED (dict noleaf ~263-292 on the new image vs ~425
before).  COG RE-BASELINE NEEDED on this same image for honest gap
numbers.  Soak in flight (/tmp/soak_leaf.log).

## CHECKPOINT 2026-06-12eee — sieve fix + prim-prologue J2J admit SHIPPED; dict 437->~370

ALL SOAKED 2468/0/0 (three soaks: gate-off default; +contracts;
+admit).  Stack landed this session:
1. Sieve-bug root fix (dead prologue helper blocks + jitPrimAtFull
   coverage-miss full-prim route).  Gate default OFF.  Sieve=1028 x3.
2. Prologue J2J contracts: x1 retval at all 20 success returns;
   success routes through emitJ2JReturnPrelude_arm64 (extracted free
   fn) via a per-method shim — pops the save-push J2J save (plain
   ret leaked it -> pool wedge).
3. xmethod PRIM gate admits hasPrimPrologue callees (predicate +
   cascade + bit-57 fill all agree).  numIC/canBailMid still apply
   to ALL (exempting them = closure-as-receiver corruption,
   deterministic, sub-bisected).  Opt-out PHARO_T1_NO_J2J_PRIM_PROLOGUE.
MEASURED: dict interleaved min-of-5 x3: 355/374/384 admit vs
425/442/447 no-admit (~16%); bail_prim 1.45M->40K.  fib30 16-17ms
(was 17.5; mild).  dict now ~370 vs Cog 26 = ~14x.
REMAINING BAILS: numic 2.3M + b46 170K — at:/at:put:'s cond-jump
fallback BODIES keep them refused.  NEXT LEVERS (in order):
(a) make at:/at:put: bodies not look scary: their numIC counts body
    send sites — a 'prologue-leaf' variant could compile prologue-
    only + EXIT_SEND body stub => numIC=0, no cond jumps => admit
    => the probe-loop at:s become inline J2J calls;
(b) root-cause the closure-as-receiver corruption (the b46/numIC
    exemption bug) — it gates MAX_IC raising generally;
(c) extras-reclassify pass (prim-75 fill on warm sites);
(d) popFrameForJIT retslot -> matguard removal.

## CHECKPOINT 2026-06-12ddd — SIEVE BUG ROOT-CAUSED + FIXED; gate OFF; at:/at:put: real-compile

The 13-month-feeling mystery fell to a 4-agent workflow + probes:
NOT a cond-jump miscompile.  emitPrimProlog_arm64 prim-60/61 format
misses branched to fail(=BODY), the fmt-3/4/5/9 helper blocks were
DEAD CODE, and uncovered formats entered fallback bodies that CANNOT
retry (Object>>at: body unconditionally raises for variable classes).
Fix: range-miss -> helper blocks (prim-62 pattern) + coverage-miss ->
jitPrimAtFull (real primitiveAt/AtPut staged above state.sp,
GC-visible) + emitted pre-tests dropped.  Gate default OFF
(PHARO_T1_SIEVE_GATE=1 restores).  Acceptance: sieve x3=1028 (was 1);
ladders clean (quiet + DET + gate-on/off).

LESSON (workflow synthesis caveat proved right): ac25a547's "bug
class still real" conflated TWO mechanisms — the May wrong-result
(stale-IC/recompile era, likely fixed since) and the June burn (this
dead code).  Also: a python edit script that asserts mid-way writes
NOTHING — verify each edit landed (the missing primat_ptr route cost
3 probe cycles).

IN FLIGHT: suite soak gate-off default (/tmp/soak_sieve.log).
AFTER SOAK: dict bench + xmethod gate-bail census (expect bail_prim
~halved: at:/at:put:/basicAt: now J2J-able with REAL prologues);
then re-run [IC-SITE] dump on scanFor: (its at: sites should now
carry pk14 AND hit the inline path); fib30 re-measure.

## CHECKPOINT 2026-06-12ccc — sieve gate is REAL; root-cause queue reordered

Sieve-gate removal: eval never completes (resume traffic x25,
startup burn).  The cond-jump emit interaction from 2026-05-19 is a
LIVE bug, not stale scar tissue.  Its root-cause hunt = the gateway
to un-stubbing at:/at:put: (half of bail_prim).  Repro recipe: build
with PHARO_T1_NO_SIEVE_GATE=1, eval 3+4 burns — bisect WHICH method
of the p60/61/62+condjump family corrupts via PHARO_JIT_SKIP_SELECTORS
or the compile-seq bisect knobs.  ALSO: ladder log grew to 280MB of
RES traces (trap mode) after today's fills — resume traffic up; keep
an eye on whether prim-75 fill caused legit extra exit/resume churn.
QUEUE: (1) sieve-bug root cause (unlocks at:/at:put: bodies);
(2) popFrameForJIT retslot protocol (removes matguard);
(3) extras reclassify pass (activates prim-75 fill on warm sites);
(4) bail-protocol -> MAX_IC.

## CHECKPOINT 2026-06-12bbb — prim-75 fill landed; flake characterized; bail_prim needs refill

- prim-75 identityHash fill: SHIPPED via upgradeICToJ2J (the kind-20
  emit existed complete; only the fill was missing).  bail_prim
  UNCHANGED on dict because existing site entries don't reclassify
  (only-fill-when-extra==0).  To see the win: IC clear/eviction or
  fresh sites.  Consider a one-shot extras RECLASSIFY pass at
  recompile/GC for pk-only prims (cheap, bounded).
- STARTUP-DNU FLAKE: rate varies per binary AND per load on the SAME
  binary+config (0/5 then 5/5 then 0).  NOT matguard (A/B noise —
  narrow stays default).  DET_SCHED doesn't pin GC/startup-Delay
  timing.  FORENSIC RECIPE: loop `PHARO_SP_DEPTH_TRAP=1
  PHARO_DET_SCHED=1 eval 3+4` until dnus>0, save the full log; the
  DNU-STACK + SEL-CORRUPT forensics auto-print.  Suspect pool:
  recent windows' default-path changes (ignOC widening GC window?
  matguard narrow + popFrameForJIT retslot hole — REAL regardless;
  scan-fix newly-real methods changing layout).
- popFrameForJIT does NOT honor materializedRetSlot (confirmed read)
  — thread the retslot write through its callers; removes the
  matguard entirely.
- Soak in flight (/tmp/soak_idh.log).
FLAKE RESOLVED: 0/40 on a QUIET box — every dnus=1 ladder ran CONCURRENT with a background soak.  Load-contamination, not a VM defect; the never-bench-during-suite rule extends to LADDERS.  NEXT: (1retired)
(2) retslot protocol in popFrameForJIT; (3) extras reclassify pass;
(4) sieve-gate root fix (un-stub p60/61); (5) bail-protocol for
MAX_IC.

## CHECKPOINT 2026-06-12aaa — gate-bail census: 92% bail rate; both halves named

xmethod gate counters (PHARO_T1_INLINE_J2J=1, dict): enter=3.6M,
bail_prim=1.45M, bail_numic=1.8M (= 92% bail -> dispatchCached exit
-> C++ round trip; THE per-send cost).
- bail_prim half: callees WITH prims (hash p75, at: p60-sieve-stub,
  key p264).  J2J correctly refuses (would skip the prim).  FIX =
  send-site inline kinds: identityHash (~4-insn header extract,
  bits 32-53) + getter p264 engagement + un-stub p60 (the sieve
  gate!).  This is the yy item-1 plan, now confirmed by counters.
- bail_numic half: MAX_IC=1 leaf-only cap.  PHARO_T1_XMETHOD_MAX_IC=8
  wins 3/3 interleaved pairs but only ~3-6%, AND ladder dnus=1 →
  re-exposes the mid-method-bail corruption the cap guards.  Raising
  the cap requires fixing that bug class first (canBailMidMethod
  callees corrupting the caller frame on bail — the inline-activate
  fallback protocol).
SOBER TRAJECTORY NOTE: dict layers each yield single-digit %.  The
~15x lives in per-send C++ round trips vs Cog's all-machine-code
PIC+prim sends.  The compounding path: (1) identityHash inline kind,
(2) sieve-gate root fix to un-stub p60/61 at:/at:put: methods,
(3) mid-method-bail fix -> raise MAX_IC.  Each removes a bail class;
together they make the probe loop exit-free.

## CHECKPOINT 2026-06-12zz — hash-site exit = runtime J2J gate decline on a HEALTHY IC entry

Full IC dump: scanFor: #hash site is POLYMORPHIC (5 entries; SmI
entry has bit60 + bit57 xgate-ok = healthy).  The per-activation
exit is an IC HIT that DECLINES to inline-J2J at a runtime gate and
exits ExitSendCached.  NEXT (one probe): count the j2jBailSelf2
reasons at runtime — gate candidates in priority order:
(1) state.j2jSaveCursor NULL at tryJITActivation entry (the JIT_CALL
    state init may not provision a pool slice for the top-level
    activation; cursor=null -> every save-push/saveless bails ->
    dispatchCached exit).  CHECK tryJITActivation's state setup vs
    the chain loop's (which sets &j2jPool_[...] slices).
(2) PMS patched-key mismatch: site patched for the startup class
    (0xc0d); SmI receivers miss the patched compare and the
    unpatched fallback shape may exit rather than probe.
(3) warm-J2J gate / depth limit.
Add counters at the bail branches (inlineJ2JCounters exists —
PHARO_T1_INLINE_J2J_COUNTERS=1 emits per-gate bail counters
ALREADY: g_xgate_bail_* family!).  Run dict with that knob FIRST —
zero new code.  prim-75 inline (yy item 1) parked until this gate
question resolves (the healthy-entry decline may explain everything
without new inline kinds).

## CHECKPOINT 2026-06-12yy — scanFor: IC map decoded; 2 concrete fixes queued

[IC-SITE] dump (new, under PHARO_JIT_TRACE_OOP): scanFor: site0
#size pk16+J2J ok; site1 #hash = J2J bit-60 NO KIND -> inline-J2J
gate bails on callee prim (75 identityHash, no prologue) ->
dispatchCached EXIT per activation; sites 2-7 (at:/==/key) ALL ZERO
— never filled, because post-hash the activation runs interp and
interp sends don't fill T1 ICs.  One bad site poisons the method's
whole IC warmup.

CONCRETE FIXES (next window, in order):
(1) prim-75 identityHash INLINE: add an inline prim kind for
    basicIdentityHash (hash = header bits 32-53 extract per the
    relocated-header memory — ~4 insns) at fill (inlinePrimKind) +
    emit (the tryPrim* family).  Kills the per-activation hash exit;
    scanFor: then runs to the at: site jitted and FILLS sites 2-7.
    NOTE fill-path: prim-75 callee has hasPrimPrologue=false — route
    like quick prims (kind classification, NOT bit-60).
(2) Verify at:-site pk14 then engages (the dump will show extras
    with kind 14 once site 2 fills from the JIT path).
(3) Re-measure dict; if the loop still exits, repeat the site-dump
    method on the next exit source.
Suspect this chain (bad-prim site -> IC warmup poisoning) is GENERIC
— same dump recipe applies to any hot method.

## CHECKPOINT 2026-06-12xx — resumability lever DEAD; at:-inline engagement is the dict endgame

MEASUREMENT VERDICTS THIS WINDOW:
- matguard 1-deep: shipped + soaked 2468/0/0 (cascade fixed).
- Cond-jump resume flip: REJECTED — force-rung collapses scanFor:
  interp sends 19x (17M->920K, the resume DOES work now) yet dict is
  consistently SLOWER (449-500 vs 413-435, interleaved min-of-5,
  3/3 pairs).  Per-send exit/resume round trips > interp dispatch.
  Keep nBC=0 default for cond-jump send methods.

THE DICT ENDGAME (one question): why does scanFor:'s `array at:
index` site NOT engage the primKind-14 inline-at dispatch?  Stats:
atPut inline=863K fires vs at:=49K (with ~800K at: ops/run).  The
site exits ExitSendCached per iteration (the r7:388K histogram via
PHARO_JIT_TRACE_OOP=0x3003037f0 + [JIT-TRACE-EXITS]).  Suspects:
(a) dispatch ORDER at sites whose extras carry BOTH bit-60 J2J and
    pk 14: J2J gate tbnz bit16 (callee has prim) bails -> falls to
    dispatchCached/EXIT instead of trying the pk-14 inline (the
    basicNew-style 'prim kind wins over bit 60' reordering comments
    exist for at:/atPut:/size — verify it actually covers this
    shape);
(b) the at:-site extras never get pk 14 (fill-path condition);
(c) the inline-at emit bails on format/bounds for these receivers.
Probe: dump scanFor:'s at:-site IC extras word at steady state
(rederiveSiteForICData-style read), or add a bail counter to the
inline-at emit.  Fix = make the at: probe loop run with ZERO exits;
THEN the dict gap should finally step toward Cog 26ms.

## CHECKPOINT 2026-06-12ww — matguard cascade fixed; scanFor: bail edge = last open dict item

SHIPPED (soak in flight /tmp/soak_matguard.log): canJITActivate's
matRetSlot guard narrowed 4-deep -> 1-deep.  [SF-ACT] proved the
cascade: 1428 of 1.05M scanFor: activations passed canJITActivate;
post-fix interp activations collapse.  Opt-back
PHARO_JIT_MATGUARD_DEEP=1.

LAST OPEN DICT ITEM: scanFor: STILL does ~17M interp at:/key sends
(per-activation ~17 = the probe loop) even with matguard-narrow +
FORCE_RESUME_FOR_SENDS.  Its jit-entered activations bail at the
FIRST send (#hash) and complete interpreted; resume telemetry shows
ZERO refusals for it (frozen at startup levels) — so the resume is
never even ATTEMPTED on that bail path.  NEXT PROBE (5 lines):
per-method counter in the tryJITActivation/tryExecute exit loop —
which exitReason does scanFor: produce, and does its handler attempt
tryResume at all?  Suspects: (a) the exit lands in a handler that
goes interp without resume (EXIT_SEND miss-path?), (b) #hash callee
(SmallInteger>>hash / prim-75 family) routes through a no-resume
edge, (c) t1NoPostPrimResume-style skip.  When fixed, expect the
17M sends to collapse and dict to step toward Cog (currently
~360-470ms noisy vs 26).

Also pending: force-rung suite soak -> cond-jump gate default flip;
fib30 unchanged ~17.5 vs 5.

## CHECKPOINT 2026-06-11vv — RAW-SCAN BUG FIXED (scanFor: un-stubbed); census attribution next

MAJOR FIX (commit "allBytecodesSupported prefix scan walked RAW
BYTES"): the ExtA/ExtB acceptance pass byte-stepped, so operand bytes
that LOOK like prefixes (0xE0/0xE1) stubbed entire methods.
Dictionary>>scanFor: (long backward jump E1 FF ED E0) was the
canonical victim — explains the whole scanFor:-interp mystery chain
of checkpoint uu (it was NEVER Sista ownership; hasSplice=0; the
SF-NOTE probe disproved that narrative).  Post-fix scanFor: compiles
REAL and (under force-rung) resumes fine.

OPEN: the at:/key sender census STILL shows ~17M sendSelector sends
attributed to scanFor: while dict stays ~360ms.  Attribution caveat:
census uses method_ which may be STALE for chain-mediated sends.
NEXT: (1) tag sendSelector call SITES (interp 9159/9430 vs
chain-side 21060/21396) in the census; (2) if chain-side dominates,
the cost is per-send C++ chain round trips in jitted scanFor: —
check why its at:(0x70 special) IC sites miss/exit (IC fill?
primChain routing? '[JIT] Stats' IC-miss breakdown noICData);
(3) suite-qualify force-rung (resumes now healthy post-ensure:-fix
+ post-scan-fix) and consider the cond-jump gate default flip.
Soak of the scan fix in flight (/tmp/soak_scanfix.log).
Ladder-verified dnus=0.  Earlier-window items still open: married
contexts design; xmethod gate flip measurement.

## CHECKPOINT 2026-06-11uu — ignOC widening SHIPPED (+20% dict); scanFor: orphan = open

SHIPPED + SOAKED (2468/0/0): ignoreOuterContext widening — dict 424
-> 329ms min.  The remaining dict cost is decoded to ONE bug:
HashedCollection>>scanFor: executes INTERPRETED (18.4M at:/key
interp sends per bench; [CPP-SEND-CENSUS] sender breakdown) because
it has NO JITMethod (SF-PROBE jm=0) while Sista hasSplice()-owns it
(splice compiled, NEVER ENGAGES: [SISTA] dispatch=0).  NO_SISTA ->
sends collapse to 41K.  The 256-skip orphan escape added at the
hasSplice gates (JITRuntime.cpp ~2727 + ~2833) DID NOT take —
census unchanged.  NEXT (in order):
(1) instrument WHICH path refuses: does noteMethodEntry fire for
    scanFor: at all? is method bits in initialCompileFailed?
    check the OTHER hasSplice gates (~3320, ~3874);
(2) also check Sista's lookupBcEntry OSR hook — why dispatch=0 for
    a method Sista owns (bcOffset key mismatch?);
(3) once scanFor: is jitted, re-census; then quiet-box dict measure
    (expect a major step toward Cog 26ms).
Box-noise warning: dict swings 330-450 across windows on identical
configs — only trust min-of-3 interleaved or functional counters.

## CHECKPOINT 2026-06-11tt — inline block-create parked; the lever is married contexts

In-JIT block creation (jit_rt_block_create, the basicNew blr pattern)
BUILT AND PARKED OPT-IN: structurally unsound under JIT-to-JIT calls
— the CALLER's activation lives only in machine state (chain
inline-activate / inline J2J / saveless) and only the EXIT handlers
reify it before C++ creates the closure; an in-JIT create builds
outerContext from a frame model missing the caller.  j2jDepth==0
guard insufficient (chain + saveless callers leave no depth trace);
falsified even with NO_INLINE_J2J=1.  Diagnosis tools that found it:
emit-count bisect (PHARO_T1_INLINE_BLOCK_CREATE_MAX + /tmp/ibisect.sh,
culprit site #3 = Dictionary>>at:) + PHARO_BLOCK_CREATE_TRACE diff
(creates identical -> emit-interaction bug, not C++).

=> THE DICT GAP'S REAL REQUIREMENT, twice confirmed now: closures
need an outerContext WITHOUT reifying the frame chain = MARRIED
CONTEXTS (Cog's design).  Sketch: closure.outerContext = a lightweight
context whose sender is a SENTINEL resolved on access (Context>>sender
prim / VM walks); divorce on frame pop.  Entry points to audit:
Context>>sender/home/unwind search, NLR home finding
(validateNLRHomeFrame), exception delivery, thisContext, debugger.
Write docs/married-contexts.md FIRST (multi-session).

Cheaper interim candidates for dict: (i) Sista-side: at:ifAbsent:
specialization that REUSES a per-call-site closure or compiles the
common hit path without creating the block (Cog's Cogit doesn't
create the block either on the hit path — the BYTECODE COMPILER in
the image already evaluates ifAbsent: lazily... verify what stock
Pharo bytecode does for at:ifAbsent: w/ literal block: the block IS
created each call there too — Cog pays closure alloc but inline +
married ctx).  (ii) clean-block image-side optimization
(CompilationContext optionCleanBlockClosure) — clean blocks skip
outerContext entirely (ignOC=1 -> our create skips materialize!).
Enabling that image-side for Pharo's collection protocol might kill
most materializes WITHOUT VM work.  CHECK FIRST NEXT SESSION — it
may be a pure image/startup-script change.

SHARPER FIRST EXPERIMENT (next session, ~30 lines): VM-side
ignoreOuterContext widening.  Pharo sets ignOC only for CLEAN blocks
(no self/outer-temps/^), so at:'s [self errorKeyNotFound: key] gets
ignOC=0 and pays materializeFrameStack — but the closure's
outerContext is semantically UNREAD unless the block has ^ (NLR) or
thisContext.  In createFullBlockWithLiteral: scan the CompiledBlock's
bytecodes once (cache verdict in a side map keyed by block oop) for
ReturnTop-from-block/thisContext; if absent -> skip materialize, use
the activeContext_ else-branch.  If the suite holds, EVERY dict-path
block-create stops materializing — without touching the JIT.  Risks
to watch: debugger tests, Context>>home/outerContext reflection
tests, exception-retry (#retry re-runs protected block via ctx).

Suite soak of current default PASSED 2468/0/0 (/tmp/soak_ibc.log).
dict 437ms vs Cog 26 (16.8x); fib30 ~17.5 vs 5 (3.5x).

## CHECKPOINT 2026-06-11ss — dict gap FULLY decoded: block-creation exits are the cost

Counter false alarm resolved: inline-prim counters need
PHARO_T1_INLINE_PRIM_COUNTERS=1.  With it: atPut=863K size=378K
bitOp=366K fire fine; at=49K (vs 11.9M C++ #at:) and getter=0 (vs
8.8M C++ #key).  The 11.9M C++ #at: are DICTIONARY-receiver at: (no
prim — can't inline as primKind 14; must call the compiled method).

THE CHAIN (per dict op): jitted caller -> Dictionary>>at: ->
PushFullBlock (the ifAbsent: closure) -> ExitBlockCreate exits to C++
-> createFullBlockWithLiteral + materializeFrameStack (incremental
now, still 1-3 allocs) -> tryResume -> re-enter.  EVERY at: pays a
full JIT exit/resume round trip + closure alloc + context
materialization, and the #key/#hash sends execute interpreted inside
that window.  Cog: closure alloc INLINE in machine code + married
(lazy) outerContext — never exits.

NEXT BIG LEVER (multi-session design, docs/ worthy): inline
FullBlockClosure creation in T1 — pointer-bump alloc helper (no full
exit), receiver+copied from stack, and the HARD part: outerContext.
Options: (a) materialize-on-demand sentinel in the closure +
divorce-on-access (married-context lite; touches thisContext, NLR,
exception search); (b) keep eager materialization but make it O(1)
via the incremental cache (now in: ctxSynced) + top-frame-only
materialize with lazy sender linking.  Either removes the per-at:
exit.  Suite 2468/0/0 green on everything through the ensure: fix.
dict 437ms vs Cog 26 (16.8x); fib30 ~17.5 vs 5 (3.5x).

## CHECKPOINT 2026-06-11rr — ensure: was the force-rung blocker; dict lever = dead inline-prims

FORCE-RUNG FIXED: compile-seq bisect (script /tmp/rbisect.sh,
RESUME_MIN/MAX_COMPILE knobs) -> ONE culprit, #ensure: (prim 198,
unwind-MARKED).  Marked methods (198/199) now never advertise resume
(AsmjitT1, ~9171 gate region).  Force-rung ladder CLEAN.  Suite soak
of the new default IN FLIGHT (/tmp/soak_ensure.log).
NOTE: cond-jump send-resume default flip is now UNBLOCKED (remove
t1HasCondJump from the gate) but NOT YET justified by measurement —
force-rung dict showed NO win (450 vs 437ms).

REFUSAL-RATE THEORY KILLED (5th false-lead class): resumability does
not move dict.  THE REAL LEAD — C++-send census (new probe in
sendSelector, PHARO_JIT_FAIL_REASONS=1, [CPP-SEND-CENSUS]): per 1M
window #at:=477K #key=453K dwarf everything; and the stats footer
shows inline-prim: at=0 atPut=0 size=0 class=0 — the send-site
primKind 14/15/16 specializations AND the bit-63 quick-getter (#key,
prim 264!) NEVER ENGAGE in the dict loop.  In bench-suite-era runs
these fired constantly.  NEXT SESSION:
(1) find why at:-site IC extras lack primKind 14 here (fill order?
    late-spec recompile dead? tier=2 recompiles w/ numBytecodes=0
    suggest the recompile path is producing stub JMs!);
(2) check JITCompiler recompile()/compile() — observed tier=2 JMs
    with nBC=0 contradict JITCompiler.cpp:2539 setting numBytecodes;
(3) #key inline-getter: UNSUPP-PRIM log showed prim=264 #key REFUSED
    compile — quick prims should at least fallback-body-compile now,
    but the SEND-SITE getter bit is the real fix (caller-side).
dict still 437ms vs Cog 26 (16.8x).  fib30 unchanged ~17.5 vs 5.

## CHECKPOINT 2026-06-11qq — refusal chain fully decoded; force-rung is the dict lever

tryResume refusal telemetry (rrDump in JITRuntime.cpp, knob
PHARO_JIT_FAIL_REASONS): 51% of dict-loop resumes refused, ALL at the
codeOff gate, on JMs with numBytecodes=0.  TWO causes peeled:
(1) unsupported-prim methods were refused WHOLESALE -> FIXED: the
    prim-fallback-body change (AsmjitT1, opt-out
    PHARO_T1_NO_PRIM_FALLBACK_BODY) compiles the bytecode body past
    the CallPrimitive header; activateMethod runs the prim in C++
    first, all direct-call paths gate on hasPrimPrologue.  Ladder
    clean; dict ~3% only because cause (2) dominates.
(2) THE BIG ONE: cond-jump send-methods (scanFor:, do:, ifNotNil:,
    findElementOrNil: — every loop) have advertiseResume=false
    (AsmjitT1 ~9171, the Phase-4b.2 mustBeBoolean-risk gate).  The
    force-rung PHARO_ASMJIT_T1_FORCE_RESUME_FOR_SENDS=1 lifts it.
    Bench + ladder in flight; if ladder+suite qualify, flip the gate
    default (the six send-resume root causes are all fixed — this
    soak was the documented open item).

## CHECKPOINT 2026-06-11pp — dict profile decoded; NEXT LEVER = resume-fail rate

Profile (sample of the dict loop, steady state): time is C++ interp —
returnFromMethod -> tryJITResumeInCaller -> activateMethod chain +
storePointer sweep.  Two fixes landed: incremental materialization
(SavedFrame.ctxSynced; skip re-sync of unchanged suspended frames;
opt-out PHARO_MAT_FULL_RESYNC; dict ~5%) — soak IN FLIGHT
(/tmp/soak_matskip.log, expect 2468/0/0).

THE BIG LEAD (steady-state [JIT] Stats windows, dict run): J2J-r
resume succeeds only ~22% (1650/7400 per 65K-send window).  A failed
tryResume leaves the caller INTERPRETED until return -> per-return
tryJITResumeInCaller -> the C++ storm.  Suspected refusal:
getBcEntryState(jm, postSendOff) != 0 (register-reading _N entries)
— over-conservative for offsets the resumeOverrides table points at
LANDERS (they take retval in x1; JIT_RESUME_CALL passes it).  TODO:
(1) instrument tryResume fail reasons (counter per refusal site);
(2) if confirmed, accept lander-override entries at the tryResume +
kind-1 precompute gates (use JIT_RESUME_CALL with the retval);
(3) re-measure dict (was 378-450ms vs Cog 26).

## CHECKPOINT 2026-06-11oo — fresh gap numbers; dict is the headline

Quiet re-measure (min-of-3 in-process, current default build-opt):
  dict 200K at:put:+at:  OURS 378ms  vs COG 26ms  = 14.5x  <- THE GAP
  fib30                  OURS ~17.5ms vs COG ~5ms = ~3.5x
NEXT WINDOW: profile the dict loop (PHARO_JIT_FAIL_REASONS, sample,
IC-hit counters) — per vm-speed-lever-dispatch the lever is
non-inlined activation throughput (at: -> at:ifAbsent: -> scanFor:
send chain + prims 60/61/110/111 inline coverage).  Then nil-fill
link-time spec + M3c doorbell.

## CHECKPOINT 2026-06-11nn — XGATE fold shipped default-ON; soak in flight

Lever (1) from mm implemented (commit "jit: XGATE fold"): extras bit
57 = precomputed xmethod gate verdict (xmethodGateOk() in
JITRuntime.hpp MUST mirror the emitted cascade), set at the 3 bit-60
setters + recomputed in rewriteIcEntriesAfterRecompile; emit = one
tbnz x7,#57 past the 4-load cascade (cascade kept as fallback;
opt-out PHARO_T1_NO_XGATE_FOLD; counters/cap/log modes unfolded).
Ladder clean.  cfib interleaved min-of-5: fold wins 2/3 pairs (~3%,
NOISY box — re-measure quiet before quoting).  DONE: 1-15 batch suite soak
PASSED 2468/0/0 (fold default-ON qualified).  If soak regresses:
suspect a bit-57 misuse collision or a gate-predicate mismatch —
PHARO_T1_NO_XGATE_FOLD=1 A/B isolates instantly.

NEXT после soak: (a) quiet-box cfib + fib30 + dict re-measure vs Cog
baseline (the Stop-hook numbers fib30 19/5, dict 375/18 are stale);
(b) nil-fill link-time specialization; (c) M3c §9 doorbell.

## CHECKPOINT 2026-06-11mm — M4 FULL MODE CLEAN; verdict = perf-neutral, park opt-in

ROOT CAUSE of the full-LAZY startup DNU: EIGHT exit-ip idioms in the
extended-op emit loop spelled the pattern `OFF_METHOD + bcOffsetBase +
globalIdx` (vs the converted family's `bcOffsetFromMethObj`) — grep for
ONE spelling missed the other.  PushFullBlock's was the killer: inside
an inline-J2J callee the method mirror is stale, so ExitBlockCreate
handed C++ an ip in the WRONG method -> wrong closure literal ->
garbage-Array-as-selector DNU at startup (Array>>do:).  All 8 now
derive from x19->bcStartCache.  Hunt artifacts that were real but not
sufficient (all committed): materializeX5 mirror reload, block-value
br x19 reload, lander gate widened to blockValue, GC prepare/forEach
refresh + ip clamp, tier-2 bit-60 refusal/unlink, exit-side x19 brk
#0xF14 oracle (verify modes).

DIAGNOSTIC LESSONS:
- "dnus=10" was a print cap — every config read identical; eval7 was
  the real binary signal.  Knob-bisect said all pairs fail/all-off
  passes because the bug needed (any cross-method continuation) AND
  (an extended-op bail inside a J2J callee).
- [SEL-CORRUPT] + RESUME-MISMATCH ip-arithmetic (ip = wrongMethod +
  smallOffset) pointed at the ip WRITER; the exit-side x19 oracle
  (clean) then EXONERATED x19 flow and narrowed it to the ip idioms.

MEASUREMENT (quiet, x3-4 each): fib30 18.5 -> 17.5ms (~5%, saveless-
dominated); cfib28 ~110 both with Sista off (WASH — 8th OoO-absorption
instance: independent fire-and-forget stores are free on M-series;
only dependent-chain cuts pay).  VERDICT: M4 parks OPT-IN per the
simStack/M2 park rule.  Census instruction counts are NOT cycle
counts — stop pricing levers by instruction deltas alone.

Suite soak (1-15 batch, full LAZY, Pharo-jit3) running in background.
NEXT LEVERS (dependent-chain, per cfib-gap-anatomy): (1) the 8-load
JM gate cascade fold at linked send sites (PMS gate-bit fold), (2)
nil-fill link-time specialization (cmp+branch header is on the
critical path), (3) M3c via §9 doorbell (frees x20).

## CHECKPOINT 2026-06-11ll — M4 built + verify-clean; full mode = one consumer left

M4 implemented per the census (~21%/call): exit publish via
emitSyncSpToState (62 sites) + Ltramp_exit; mirror stores + ip stores
gated out at the 3 J2J tails; 15 exit-ip idioms re-derive from
x19->bcStartCache; literal pushes read literalsCache; the saveless
stash carries live x19; fsrLazyRefresh at 14 C++ choke points + 3
helper heads + the trampoline caller.  x19 movs now UNCONDITIONAL.

VERIFY MODE ([M4-PARITY] oracle, stores+publish both on): CLEAN.
FULL MODE: still 10 startup DNUs — garbage SELECTOR (a non-symbol
literal) in WorkingSession>>on:do:/Array>>do: => an interp selector
fetch reads method_/literals between a JIT exit and the refresh.
NEXT PROBES (3-min cycle each): (1) the 26122-helper switch's
return-false path (audit finding: syncs only ip/sp; the interp then
dispatches with whatever method_ — under LAZY state.method itself is
stale until refreshed — add fsrLazyRefresh INSIDE that helper before
return false); (2) grep Interpreter for `state.method` reads in exit
handlers that run BEFORE the choke refresh line (ordering within the
handler bodies); (3) the [M4-PARITY] oracle moved INTO the failing
window: run VERIFY+full-delta — enable LAZY_VERIFY and ALSO delete
stores (a hybrid knob) so the oracle pinpoints the first stale read.

Perf measurement deferred until full mode is clean.  Default
unchanged + validated; everything gated.

## CHECKPOINT 2026-06-11kk — THE COUNT: per-call instruction census (decides M4 first)

Capstone census of benchFib's LINKED J2J call sequence (the PMS
patched tail, /tmp/jit_benchFib_1.bin 0x228-0x2d0, ~43 insns/call):
  calleeJM movz x3 (patched)              3
  cursor headroom (M2 shape)              4   (mov x23 + ldr limit + cmp + b.hs)
  resume adr+movk                         2
  save push (stp pairs + loads)           5   (M2b: -1 write-through str)
  MIRROR STORES                           9   <- M4: ldr CM + str jitMethod +
                                               str method + add/str literals +
                                               mov/str argCount + ldr bcStart
                                               + str ip
  cursor write-through + x23              2   (M2b: -1)
  depth RMW (x20 batched)                 3   (M3c, blocked on §9)
  receiver + tempBase stores              3   (M5 territory)
  nil-fill loop header (0 locals!)        ~5  <- link-time specializable:
                                               patched sites KNOW the callee;
                                               emit could skip/unroll the fill
  sp update (x25)                         1
=> **M4 is the single biggest cut (~21%/call), then the nil-fill
specialization (~12% for 0-local callees like fib), then M3c+M2b
(~14% combined, blocked/small)**.  Plus the return-side prelude+
restore sequence (not yet censused — do the return side next).

M4 prerequisites all in place: x19 invariant (M1 ✓ verified), the
syncDerivedFromJM funnel + literalsCache (M0 ✓), exit stubs publish
points enumerated.  M4 = delete the 9 mirror stores from the linked
tail; exit stubs re-derive method/literals/argCount/ip from x19 (via
funnel on the C++ side; bcStartCache for ip).  VERIFY: FR-7 assert
(mirror==derived at exits) soaked before deletion, like M3's oracle.

## CHECKPOINT 2026-06-11jj — M3 stages a+b GATED-IN, parity-proven; (c) blocked on §9

Stage (a) (both representations + parity oracle) and stage (b) (the
prelude's 1-load cursor compare) are committed and SOAK-PROVEN
(2468/2468 x2 with zero [M3-PARITY] divergences).  The entryCursor
protocol is correct end-to-end including the saveless stash carry
(new 64-bit slot; base stash 48->64, cross uses free [80]).

**Stage (c) blocker, precisely**: the per-push depth RMW is the x20
BATCHED increment 0x100000001 — its high half counts j2jTotalCalls,
which chargeJITBytecodes converts into checkCountdown_ charging
(GC/timer/scheduler periodic checks).  Dropping the RMW without the
§9 doorbell-preemption redesign starves the scheduler.  The doorbell
design (per frame-state-residency.md §9): back edges + send entries
poll a single forceYield byte (already emitted for native back
edges!), and charging moves to exit-time wall-clock or per-exit
estimates — THEN the RMW drops and x20 frees (-> M5 receiver
residency).

**Where the remaining fib gap likely lives (next measurement step
before more FSR)**: capstone-profile ONE hot linked round trip
(PHARO_T1_DUMP_SEL=benchFib + the disasm recipe) and COUNT the
remaining per-call instructions by category (gate cascade / mirror
stores (M4) / save push (M2b) / depth RMW (M3c)).  Decide
M4-vs-M2b-vs-send-fold from the counts, not the design order.

Current: fib30 ~19ms vs Cog 5; dict ~375 vs 18; suite 4134/4140;
all FSR knobs verified-correct (x19 invariant, x23 cursor, entry-
cursor protocol).

## CHECKPOINT 2026-06-11ii — M3 phase 1 IN (C++ baseline maintenance); phase 2 design pinned

Phase 1 committed: j2jEntryCursor maintained at all 11 C++
depth-baseline writer sites (8 zeroers + 3 pin/restore pairs).
Ladder clean; field maintained, not yet consumed.

**Phase 2 (emit) — the remaining design decisions, resolved:**
- The saveless stash carries the caller's entryDepth in the 32-bit pad
  slot [sp,40]; the 64-bit entryCursor needs a new slot: grow the
  stash by 16 (alignment) in NODEPTH mode — base 48->64, cross
  96->112, entryCursor at [stashSize-16]; keep [40] entryDepth DURING
  the verify-parity soak, drop it at RMW removal.  (Considered: 32-bit
  pool OFFSETS in [40] — rejected: the emitted code lacks a cheap pool
  base; the pool is 32KB so offsets fit, but base materialization
  costs more than the stash growth.)
- Emit pin sites to convert (entryDepth -> entryCursor): 5171
  (saveless stash save+pin), 5320/5382 (retro unwind restores), 5399,
  3298 (the return-prelude READ -> becomes ldr x4,[x0,#312];
  cmp x23,x4).
- Push-site RMW removal: the three M2-gated sites' depth triple.
- Choke-point depth refresh (state.j2jDepth = j2jDepthFromCursor())
  at: chain-loop post-JIT_CALL, resume-loop post-tryResume,
  tryJITActivation initial, the two osr entries — AFTER the verify
  soak proves parity.
- VERIFY order: (a) both representations maintained + a C++ assert at
  choke points (knob PHARO_T1_FSR_NODEPTH_VERIFY), soak; (b) flip the
  prelude to the cursor compare, soak; (c) remove the RMW + install
  the choke-point refresh, soak; (d) measure.

## CHECKPOINT 2026-06-11hh — M3 implementation design (worked out, ready to execute)

**M3 (depth elimination) — the exact plan, avoiding the consumer sweep:**
1. KEY TRICK: do NOT rewrite the dozens of C++ `state.j2jDepth` readers.
   Instead the emitted code stops writing depth, and C++ REFRESHES
   `state.j2jDepth = state.j2jDepthFromCursor()` once at each
   exit choke point (post-JIT_CALL in the chain loop, post-tryResume in
   the resume loop, tryJITActivation's initial call, the osr entries —
   ~5 sites) BEFORE the exit switch dispatches.  All existing readers
   then keep working unchanged.
2. Emitted push sites (the 3 gated in M2): under PHARO_T1_FSR_NODEPTH
   skip the depth RMW triple (ldr OFF_J2J_DEPTH / add x20 / str).
3. Return prelude: replace `ldr w3,[DEPTH]; ldr w4,[ENTRY_DEPTH]; cmp;
   b_le` with `ldr x4,[x0,#312] (j2jEntryCursor); cmp x23,x4; b_ls` —
   x23 is live (M2 default-on).
4. j2jEntryCursor writers: every C++ site that sets j2jEntryDepth
   (grep state.j2jEntryDepth =) also sets j2jEntryCursor =
   j2jSaveCursor-at-that-moment.  The EMITTED callee entry currently
   pins entryDepth via the x20 trick — replace with `str x23 ->
   [x0,#312]`; the SAVELESS STASH slot [sp,40] that carries
   entryDepth (see the retro path's `ldr w4,[sp,40]`) must carry the
   caller's entryCursor instead (64-bit — check the stash slot width!).
5. The trampoline's depth writes become harmless redundancy (C++
   refresh overwrites) — leave them for v1.
6. VERIFY knob: at the choke points, assert
   j2jDepthFromCursor() == the old-style depth while both maintained
   (run one soak with the emitted RMW still on + the cursor compare
   verified, THEN remove the RMW).
7. Gate: ladder + 2468-soak + timer/Delay classes + /tmp/mutex_leak.st.

**M4 preview (the likely bigger cut, per the cfib anatomy):** the
per-call activation-commit stores (method/literals/argCount/ip — 4-5
stores + address computations per cross-method J2J call) delete; exit
stubs re-derive from x19 via syncDerivedFromJM/literalsCache (all
landed in M0/M1).  M4 starts only after M3's verify soaks clean.

**Estimate check (price-the-design):** M3 removes ~5 insns of ~83 per
linked round trip; M4 ~15+.  If fib30 doesn't approach ~12-14ms after
M3+M4, the residual is the send-site gate cascade (patched-IC fold)
and dict's C++-exit sends — pivot levers documented in
jit-cfib-gap-anatomy.

## CHECKPOINT 2026-06-11gg — M2 v1 CORRECT + default-ON (wash); M2b/M3 are the cut

M2 v1 (write-through x23) is correct (2468-test soak under the
dual-cursor brk-trap, zero divergences) and DEFAULT-ON.  The hole was
pharo_jit_osr_resume never loading x23 (caught by the audit trap +
lldb: x23 garbage at a tryResume entry).  Pack-word patcher offset and
emit shape now derive from one fsrCursorMode() helper.

HONEST PERF: fib30 A/B x4 = 19ms BOTH states (wash — write-through
keeps the store; OoO absorbs the load-latency win; 7th instance of
the measurement lesson).  Per the design, the instructions LEAVE at:
- M2b: drop the write-through -> publish x23 only at exit stubs +
  BLR brackets (the M0 blr classification table gates this), and
- M3: depth elimination — replace the OFF_J2J_DEPTH RMW pair with the
  j2jEntryCursor compare (field + helper already in from M0).
PRICE-THE-DESIGN gate after M3: if M2b+M3 < 3% on fib/cfib, the
remaining macro gap is the send SEQUENCE (patched-IC fold) and
C++-exit sends (dict's cost), not frame state.

Current honest numbers: fib30 ~19ms (Cog 5), dict ~375 (Cog 18).

## CHECKPOINT 2026-06-11ff — FSR M2 v1 scaffold in (gated, one coherence hole)

M2 v1 = WRITE-THROUGH x23 cursor residency (design deviation from the
doc's full no-write-through M2: keeps memory authoritative at every
mutation so C++/BLR observers need no brackets; the win is the
dependent cursor LOAD leaving the J2J push/pop critical path; full
no-write-through becomes M2b if v1 measures short).  Implemented:
macro hoists + clobbers (unconditional, inert), 5 gated emit sites.
KNOB-ON IS BROKEN (eval lost) — one coherence hole; default inert.

NEXT for M2: the dual-cursor audit — gated emit at every exit stub
comparing x23 vs [x0,#144] with brk #0xF23 on divergence (mirror the
x19-verify pattern at dispatchCached + emitSyncSpToState callers);
run the ladder -> the first trap names the divergent path.  Suspects:
a trampoline path updating memory-cursor without x23; an emitted path
running before the macro hoist (OSR entry? pharo_jit_osr_resume's
register reloads — check whether IT reloads x23); C++ mid-chain
cursor rewrites paired with non-macro JIT entries.

Then: measure (cfib/fib30/dict A/B) -> M3 (depth elimination via
j2jEntryCursor, the second money batch) -> price-the-design gate.

## CHECKPOINT 2026-06-11ee — SEVENTH root cause FIXED; suite fully clean

**The -2^62-ns clock lesion = the Phase-3 arm64 inline multiply (0x68)
missing the 61-bit range check** — the FOURTH mul emit site (the three
audited ones all had it).  smulh caught only 64-bit overflow; a 62-bit
product (microsecondClock*1000 in DateAndTime>>now) retagged mod 2^61
= value - 2^62 = year 1880, on every WARM call of the compiled method.
Found via: WD-CLOCK forensics -> exact 2^62-ns date arithmetic ->
warm-call repro (#(2026 1880 1880...)) -> piece bisection (all clean
solo) -> CAPSTONE DISASSEMBLY of the dumped method showed the missing
check.  Fixed + validated: repro all-2026; ladder clean; 60-class
suite 4134/4140 ZERO fail/error/timeout; NANO-CORRUPT tripwire
(permanent runner canary) zero hits; fib30=22ms.

**AUDIT RULE REINFORCED**: there were FOUR inline-mul emit sites, not
three — when auditing an emit pattern, grep the OPCODE dispatch
(op == 0x68) AND the prim indices AND the IC-spec blocks; or better,
disassemble the SHIPPED code (PHARO_T1_DUMP_SEL + capstone), which is
what actually caught it.

**Correctness scoreboard: SEVEN root causes, ALL FIXED; no known
remaining correctness defects.**  Next: FSR M2 (cursor residency,
MONEY 1) -> M3 (depth elimination, MONEY 2) for the macro gap
(fib30 22 vs 5; dict ~390 vs 18; sendmix ~96 vs 2), then the force
rung, full suite, Cog re-baseline.

## CHECKPOINT 2026-06-11dd — 1880-clock = EXACTLY -2^62 NANOSECONDS

**The corrupted clock value is correct_ns MINUS EXACTLY 2^62** —
proven: bad(1880-04-21T11:16:24-04:00) + 2^62 ns =
2026-06-11T11:10:02-04:00, INSIDE the run window (log mtime 11:10:58).
So a NANOSECOND-domain quantity (ns-since-1901 ~3.96e18: exceeds the
61-bit SmI range -> LargeInt territory, bit 61 set, bit 62 clear)
lost/never-gained bit 62, going negative.

Checked and CLEAN: all three arm64 inline-mul sites have both
overflow checks (smulh + 61-bit retag); no 32-bit stores to oop
slots; 3M-iteration single-flow hammer of `µsClock * 1000` shows no
wrap (the corruption needs the suite's process-switch context,
~1-2 per 2500 tests).

SUSPECT SPACE (next session): (a) the image-side DateAndTime now
nanosecond path (read DateAndTime class>>now + asNanoSeconds source:
WHERE does a single 62-bit-significant integer arise — LargeInt
digit ops? a cached offset?); (b) the inline tagged ADD/SUB checks
only the 64-bit V flag (adds+b_vs) NOT the 61-bit SmI range — sums
with values in 2^58..2^60 produce silent non-canonical 'SmIs'
(NOTE: arithmetic says a tagged add CAN'T reach bit 62 from
legitimate µs/ns-piece values; but a chain of such non-canonical
results could — audit the add/sub inline emits' range check);
(c) LargeInt primitive paths under JIT'd callers.  Repro: the 1-15
narrow batch with WD-CLOCK forensics (~1-2 hits/run, results stream
shows now=1880 lines).

## CHECKPOINT 2026-06-11cc — FSR M1 GATE PASSED; the 1880-clock signature

**FSR M1 complete + gate passed**: x19 = active-JITMethod invariant
maintained at all 5 emitted commit/restore sites + the trampoline
Lcall fix; verify oracle (brk #0xF19 at dispatchCached on
x19-vs-mirror divergence) ran the 1-15 suite (2466 tests) + full
ladder with ZERO traps.  Knobs PHARO_T1_FSR_X19 / _VERIFY.  M2
(cursor residency, MONEY 1) can now consume x19.

**The residual sporadic corruption now has an exact signature**: the
runner's WD-CLOCK forensics caught both spurious timeouts:
`DateAndTime now` = year 1880 (now AND the derived deadline both
1880, internally consistent; the later re-read returns 2026 ->
instant-false compare -> spurious TIMEOUT verdict).  ~1-2 per 2500
tests.  Shape candidates: (a) SmallFloat raw (tag 5) misread as SmI
(huge negative — e.g. 0x854...005 = 80.0); (b) partial-slot
overwrite (ruled out for the T1 emit: no 32-bit stores to oop slots).
NEXT PROBE: compute the EXACT µs value for 1880-04-21T11:16:24 from
the printed dates and match against candidate raw words; or add a
clock-plausibility trap in primitiveUTCMicrosecondsClock's consumers
(value < epoch-2020 => dump JIT state + frames).  The corrupted value
enters between prim 240 (C++-correct) and DateAndTime's arithmetic —
all JIT-visible.

## CHECKPOINT 2026-06-11bb — SIXTH root cause; send-resume rung FLIPPED DEFAULT-ON

**Root cause #6 = the chain-loop ExitYield missing frame-identity
sync** (committed a7cb9410): a heartbeat forceYield inside an
inline-J2J callee resumed the OUTER method at the callee's ip —
preemption-timed SILENT value corruption (no DNU/MUSTBOOL; surfaced
as spurious watchdog timeouts + wrong Integer/Fraction values).
Proof: every run on the pre-sync binary had 1-7 spurious timeouts;
SIX consecutive runs post-sync have ZERO.  The SP-DEPTH oracle's 8
startup hits are config-independent noise (identical in default).

**THE FLIP (committed): condjump-free send methods are RESUMABLE BY
DEFAULT** (resumeSendsNoCondjump default-true; opt-out
PHARO_T1_NO_RESUME_SENDS).  Qualified by an IDENTICAL 60-class A/B
(4134==4134, zero fail both sides) + ladder + benches.

**Trap fixed en route: JM_SIZE drift** — FSR-M0's literalsCache grew
sizeof(JITMethod) 120->128; TrampolineAsm.S JM_SIZE stayed 120 -> V2
identities 8 bytes off -> 10x benchFib (251ms).  Bisected across 3
commits; fixed + LOCKED with an exact static_assert (build fails on
future drift).

**Current quiet numbers:** fib30 24-25ms (Cog 5), dict 388 (Cog 18),
sendmix 96 (Cog 2).  The macro gap = FSR M1-M3 (per-activation mirror
+ send sequence), unchanged by the rung as predicted.

**NEXT:** (1) FSR M1 (x19 invariant) -> M2 (cursor residency, MONEY)
-> M3 (depth elimination, MONEY) per docs/frame-state-residency.md;
M0 remains: FSR_VERIFY oracle wiring at choke points, grep tables;
(2) next resume rung when ready (RESUME for condjump methods = the
full force config — ladder-clean already, suite-soak needed);
(3) full-suite + Cog re-baseline after M3.

## CHECKPOINT 2026-06-11aa — narrow-rung cluster = SILENT VALUE CORRUPTION under process-switch resume

**The 'timeouts' are SPURIOUS VERDICTS, not stalls.**  A stall-catcher
(sample-on-no-progress, /tmp/stall_catcher.sh) ran the 1-15 repro 3x:
timeouts occurred but the detail file NEVER paused >=15s — no stall
exists.  The runner watchdog computes `DateAndTime now < deadline`; an
instant-false comparison means a CORRUPTED CLOCK/DateAndTime value
(silent wrong value — no DNU, no MUSTBOOL).  Delay accuracy in
isolation is CLEAN both configs (100ms +-14ms x30).

**Bisect:** narrow + NO_INLINE_J2J still produces timeouts PLUS new
visible corruption (IntegerTest>>testNegativeIntegerPrinting FAIL,
FractionTest>>testAsSmallerPowerOfTwo FAIL) => the lesion is NOT
inline-J2J; it is the C++ RESUME machinery under PROCESS-SWITCH
PREEMPTION (heartbeat forceYield mid-JIT-method -> suspend ->
materialize/context -> later resume).  The eval ladder never exercises
this (single flow), which is why all 4 ladder configs are clean while
the suite shows ~1-7 corrupt values per 2500-test batch.

**NEXT (the decisive probe):** instrument the process-switch
suspend/resume of JIT activations: when the heartbeat preempts a
resumable JIT method, log/verify the suspended frame's (sp, ip,
method) at suspend vs at resume (a [PSWITCH] pair trace); or
PHARO_DET_SCHED with a SMALL quantum reproduces preemption
deterministically (DET_SCHED's known narrow-rung slowness is itself
likely THIS BUG repeating per-quantum!).  Try:
PHARO_DET_SCHED=1 PHARO_DET_SCHED_QUANTUM=1 + narrow rung + eval of a
multi-process workload (forked delays); expect deterministic value
corruption.  The DET_SCHED mass-timeout slowness and the spurious
suite timeouts are PROBABLY THE SAME LESION (per-preemption value
damage), making DET_SCHED the deterministic repro after all.

**FSR M0 partial committed:** literalsCache + j2jEntryCursor +
maintenance (forEachRoot refresh, compile finalize, invalidation).
Remaining: syncDerivedFromJM funnel, FSR_VERIFY oracle, grep tables.

## CHECKPOINT 2026-06-11z — audit fixes in; narrow-rung A/B: one exception-cluster from flippable

**Audit (42-agent workflow, 17 confirmed / 46 clean) — 9 fixed+committed:**
S2 sync at 5 more handler sites (chain Send/SendCached/J2JCall,
resume-loop SendCached/J2JCall, ExitYield), 3 unguarded
executeFromContext(sender) calls gated, per-BC fd=0 ExitReturn now
follows the heap sender chain (was: TERMINATED the process), chain
depth-0 matRetSlot honor, cachedTarget cleared at all 9 re-entries,
site5 materialize-failure accounting, Sista bail-blacklist byte
off-by-one (bcs[bailBcOff], not -1).  Ladder clean on all 3 configs.
Remaining (documented, low priority): stencil-tier-only ip gates
(21097/26125-class), dead-context cannotReturn contract (20688), 25732.

**60-class SUnit A/B (quiet, no DET_SCHED — see below):**
  A3 default:      4132/4140 (2 fail: known process tests)
  B3 narrow rung:  ~4090 — B3-only failures CLUSTER:
    ExceptionTest UnhandledError family (7 TIMEOUTs),
    testCannotReturn x2, DelayTest (2F/1E/2T), SemaphoreTest (6T),
    ProcessSpecificTest (3T), nested-unwind termination (1T)
  => the narrow rung's remaining defect = resume x EXCEPTION-UNWIND/
  DELAY interplay (context materialization under resumable frames).
  ~29 tests, clustered -> likely 1-3 root causes.

**DET_SCHED x narrow rung = mass timeouts (debug-only config):**
SortedCollectionTest: narrow+DET 66P/8T vs narrow-noDET 287P/0T.
Plus a DET-only post-suite teardown wedge ([WEDGE] timer-runner
death).  DET_SCHED is the diagnostic scheduler; characterize later.

**Contamination discipline (hit 3x today):** never rebuild/bench
while a suite runs; A/B pairs must use the SAME binary; results files
can be STALE (rm before runs; Pharo-jit.image had vanished entirely —
earlier '205 PASS' canaries were stale files).

**Cluster repro refinement (2026-06-11 late):** ExceptionTest SOLO
under the narrow rung = 47/47 PASS; batch 1-20 reproduces the cluster;
batch 1-10 CLEAN; batch 1-15's timeouts hit FractionTest/PointTest
(classes 6-7, INSIDE the clean prefix) => POSITION-FLAKY sporadic
extreme slowness, not a fixed population threshold.  Combined with the
Delay/Semaphore fails and the DET teardown timer-runner [WEDGE]:
hypothesis = TIMER/DELAY STARVATION under the narrow rung (resumable
methods x heartbeat/forceYield interplay; suspect forceYield_ not
cleared on the JIT yield path, or P80 timer-runner starved by resume
churn).  Decisive probe: catch a stalled test live and `sample` the
VM / suspend+walk suspendedContext (the documented no-lldb stack
capture), or instrument the ExitYield round-trip rate.
chain-ExitYield S2 sync added+committed (same class as resume-loop).

**NEXT:** (1) repro one cluster test solo (ExceptionTest>>
testUnhandledErrorWhenNoHandlers, narrow rung, no DET) + forensics;
(2) fix the cluster -> flip RESUME_SENDS_NO_CONDJUMP default-ON;
(3) FSR M0 (the Cog-parity macro lever; bench truth in checkpoint y);
(4) full-suite + Cog re-baseline.

## CHECKPOINT 2026-06-11y — FIVE root causes fixed; ladder fully clean; bench truth

**FIX 5 (committed): saveless-J2J null-cursor underflow.**  With the
resume loop's j2jSaveCursor=limit=NULL, the headroom check's unsigned
sub underflowed and the saveless path engaged with no pool; its retro
pool-full check exited ExitRetroFull(13) which has NO C++ handler
(fell into default arms, elided frame lost).  cbz-guard -> normalJ2J.
GLOBAL FORCE-RESUME now: 0 DNU, 0 MUSTBOOL, eval correct.

**Correctness status:** all four ladder configs deterministic-clean
(default / narrow rung / windows / global force).  CharacterTest
16/16 on default; under global force testStoreStringAll TIMEOUTs
(perf collapse of force-everything — stress config, not the lever).

**Bench truth (quiet, vs stock Cog same machine, same exprs):**
  fib30:   Cog 5ms   ours 18ms   (3.6x)
  sendmix: Cog 2ms   ours 79ms   (40x)  OC addLast/detect
  dict:    Cog 18ms  ours 342ms  (19x)  Dictionary at:put:/at:
  class-send-3M: 154ms default -> 12ms under resume configs (12.8x —
  the IC-side lever fires) but macro benches UNCHANGED by send-resume:
  the macro gap is per-send/per-activation overhead (the FSR design,
  docs/frame-state-residency.md), NOT the resume gate.

**Measured dead end (reverted in-tree with note):** compiling quick
prims 256-519 body-only = ~2x macro REGRESSION (C++ quick path beats
a J2J call to a 2-bytecode method; compile flood).  [UNSUPP-PRIM]
diagnostic kept: prim-fail census on dict bench = mostly quicks +
117/105/83/70/71/207.

**Contamination warnings hit twice today:** (1) benches run while the
SUnit A/B was live are garbage (dict 342->871 'regression' was load);
(2) the A/B's run B exec'd a binary that was REBUILT mid-run —
discard, re-run.  RULE: no rebuilds, no benches while a suite runs.

**NEXT:** (1) finish + diff the 60-class A/B (rerun B clean);
(2) flip RESUME_SENDS_NO_CONDJUMP default-ON if A/B identical;
(3) THE Cog-parity lever per bench truth = FSR implementation
(M0-M6) + send-sequence shortening — start M0;
(4) exit-handler audit workflow results (running).

## CHECKPOINT 2026-06-11x — FOURTH class FIXED; ladder status: narrow rung CLEAN, full force = ONE residual

**FIX 4 (committed): chain-loop create handlers.**  ExitBlockCreate/
ExitArrayCreate in the chain loop never refreshed state.sp after
pushing the created closure/array (j2jDepth>0 branch) — the object sat
ABOVE the JIT's resumed sp, the next send read operands one slot low
(addAllLast:'s do: ran with arg0==receiver).  Plus missing/conditional
globals sync (BlockCreate conditional, ArrayCreate none).  Diagnosed
via [SELFARG-ACT] (all hits caller=#addAllLast: ipOff=5 kind=1).

**Ladder status (deterministic, DET_SCHED, build-opt, eval 3+4):**
- default: CLEAN (eval ✓, 0 DNU/MUSTBOOL)
- narrow rung RESUME_SENDS_NO_CONDJUMP=1: CLEAN (was 10 DNUs+lost eval)
- windows 2000-5000: 1 residual DNU (#method on nil, OpalCompiler>>
  evaluate DoIt path), eval ✓ (was 10 DNUs+lost eval)
- GLOBAL FORCE (PHARO_ASMJIT_T1_FORCE_RESUME_FOR_SENDS=1, the
  historical 117-MUSTBOOL wedge config): ONE MUSTBOOL left:
  DateParser>>parseNextPattern ipOff=6, fp[1](temp char)=THE RECEIVER
  — `char := self readNextChar` returned the receiver.  site0 IC =
  bit-60 J2J to readNextChar (fill correct).  Same recv-as-result
  family: a J2J-called callee under force-resume whose return is
  mis-delivered.  Forensics in the res38 pattern (rerun the force
  config with SP_DEPTH_TRAP).  eval lost in this config.
- benches: benchFib/sqrt unchanged on narrow rung (loops have
  condjumps -> excluded there; the 14x needs the full force).

**NEXT**: (1) DateParser residual: trace readNextChar's J2J return
under force-resume ([RES-IN/OUT] on readNextChar/parseNextPattern,
check the J2J return's retval slot arithmetic vs resumed-caller sp);
(2) windows residual (#method on nil); (3) when force is clean:
60-class SUnit A/B + 16K soak + bench ladder -> flip
RESUME_SENDS_NO_CONDJUMP default-ON first (it is CLEAN now), then
full send-resume; (4) re-baseline vs Cog (cfib/benchFib/sfib).
NOTE: /tmp/harness/Pharo-jit.image was MISSING — earlier 'canary 205
PASS' results were STALE files; re-prepped (bg task).  Re-run AIPrim
canary before trusting it.

## CHECKPOINT 2026-06-11w — fourth class: activation arg-slot = receiver (off-by-one)

Narrow-rung repro (PHARO_T1_RESUME_SENDS_NO_CONDJUMP=1 + DET_SCHED +
SP_DEPTH_TRAP, build-opt, eval 3+4) fails EARLY (first DNU at output
line ~21K, vs ~5.7M for the windows config) with a SHARPER signature:

  [DNU] #value: on Array in Array>>do: fd=12 ipOff=16
  fp[0]=Array(receiver) fp[1]=Array(SAME OOP — should be the closure!)
  caller stack: fp[-1]=OrderedCollection (not a closure either)

i.e. Array>>do: was ACTIVATED with its argument slot holding the
RECEIVER oop — the closure never made it into the frame.  An
activation argument-copy off-by-one (copy source = stack[recv] instead
of stack[recv+1], or the send pushed [recv, recv]) in a resume-related
activation path.  Chimera caller stack (SourceFile>>on:do: under
FinalizationRegistry>>add:) — frame identities unreliable, as with the
prior classes.  The windows-config variant of this class shows as
'#+ on OCVariableNode' (loop-index temp = args array) — likely the
SAME copy off-by-one seen through the vec capture.

Next probes: (1) trap in activateMethod/activateBlock when argCount>=1
and arg0 oop == receiver oop (cheap, knob-gated) -> print the caller
frame + savedIP to identify the pushing path; (2) audit the J2J/chain
activation copies (`state.sp[-(nArgs+1)]` family) for one path using
nArgs where nArgs+1 belongs; (3) [VEC-POP] trap is in (both create
handlers) — successorSequences pops are legit captures, ignore.

## CHECKPOINT 2026-06-11v — THIRD root cause fixed (silent encoding drop); FOURTH class isolated

**FIX 3 (committed): un-encodable IC-offset add silently dropped.**
emitMaterializeX5's `add x5,x5,#(siteIdx*152)` is un-encodable for
siteIdx>=27 (imm12 max 4095); asmjit SILENTLY SKIPPED it -> every send
site >=27 probed SITE 0's IC: same-class key matched, dispatchCached
served site 0's METHOD for the wrong selector (minExtent bc-144
minHeight ran #hResizing -> #shrinkWrap as a Point coordinate ->
Morphic DNU cascade).  Diagnosis: [IC-WRONGSEL] consumer check (all
hits site=0) -> table verified -> [X5ADD] code scan found ZERO adds.
Fixed: split lsl#12+low add; T1EncodingErrorHandler now FAILS any
compile with an encoding error (systemic guard — the 3rd shipped bug
of this silent-drop class); consumer-side wrong-selector bail kept.
Wrong-selector dispatches: 34 -> 0.

**FOURTH class (OPEN, the current blocker for the resume ladder)**:
10x '#+ on OCVariableNode' DNU during in-image compilation
(OCASTTranslator>>emitMessageNode: subtree).  Shape: a loop-index
temp in the OrderedCollection do:/fillFrom:with: machinery holds the
ARGS ARRAY instead of the index SmI (`i := i + 1` -> `array + 1` ->
adaptToCollection -> per-element #+ on OCVariableNode).  The temp is
block-written (indirection vector, PushTempAtInVec 0xFB) -> suspect
the tempVector slot or the copied-values capture corrupted by a
resume.  Reproduces ALSO under the NARROW rung
(PHARO_T1_RESUME_SENDS_NO_CONDJUMP=1 + DET_SCHED, no windows): 10
DNUs, eval result lost — so this lesion (not the fixed three) is what
the old 'AI-Algorithms-Graph asTuple operand corruption' note in the
resume gate comment was about.  NEXT: RETPLACE/RES-IN trace on
fillFrom:with:/do: frames around the first DNU (line ~5.7M in
/tmp/res33.txt pattern); check the 0xFB/vec-create handling in resumed
methods (ExitArrayCreate sync, tempVector slot writes); the asTuple
atRec tape (FINDNODE_WATCH) is purpose-built for exactly this shape.

## CHECKPOINT 2026-06-11u — #3 CRACKED OPEN: TWO root causes fixed; one DNU class left

The "resume RE-ENTRY protocol" hypothesis from checkpoint t was WRONG.
The lldb session was NOT needed — compiled-in forensics found both.

**FIX 1 (commit 849e9431): phantom IC sites.**  Five raw byte scans in
AsmjitT1.cpp counted send sites without skipping multi-byte operands;
PushArray <E7 81>'s operand byte (0x81 = Send0 range) minted a phantom
site, shifting every later site's selectorBits by one.  The shifted
miss path then looked up the WRONG SELECTOR (parseAssignment's
isIdentifier site got #value:, a self-returning setter -> receiver at
TOS -> MUSTBOOL -> only-idle wedge exit 124).  Fix: forEachRealOpcode
walker (SistaV1::bytecodeLength).  Validated: deterministic repro went
exit-124+117-MUSTBOOLs -> exit-0+ZERO.

**FIX 2 (latest): dangled NLR homeFrameDepth markers.**  The NLR home
marker is an ABSOLUTE savedFrames_ index; exception-materialization
rebuilds the stack under a live block (observed: the closure's caller
record at 19 at activation, 17 at its ^v), so the marker dangles.
The dangled check then silently degrades a block's ^-NLR to a NORMAL
send-return: the home method's pop;returnSelf tail runs and returns
the RECEIVER (FreeTypeCache atFont:...: ^v swallowed, cache propagated
up widthOf: into width arithmetic -> 8x '#+ on FreeTypeCache' DNU/run,
eval result lost).  Fix: validateNLRHomeFrame at BOTH consumers —
derive the expected home CM (block literal chain / nlrHomeMethod_),
verify the marker's frame, re-search innermost on mismatch, SIZE_MAX
fallback.  Plus popFrame scrubs the popped slot's marker.
Validated: FreeType DNU class 8 -> 0; 40 [NLR-REPAIR] hits.

**REMAINING (windows repro, in progress)**: ~10 DNUs of a second
class: 7x '#+ on InstanceVariableSlot' (OCBlockNode>>argumentNames /
collect: path), 2x adaptToNumber:andSend: on a Character (Morphic
doLayoutIn:), 1x #negative on ByteSymbol.  Same broad shape (wrong
object where a number belonged).  Suspect: another NLR/marker case
the validator can't check (expected underivable), or a second resume
lesion.  Repro: PHARO_DET_SCHED=1 + the four RESUME window knobs
(2000-3500 + 3500-5000) on build-opt, eval 3+4; forensics knob
PHARO_SP_DEPTH_TRAP=1; traces: [NLR-REPAIR]/[BLKACT]/[BLKRET]/[HFD-W]/
[RES-IN]/[RES-OUT]/[CTX-RET]/[DNU-FORENSICS].

**NEXT**: (1) chase the InstanceVariableSlot DNU with the same chain
(DNU-FORENSICS fp window -> bytecode map -> RES-IN/BLKRET around it);
(2) when windows-repro is CLEAN: ladder = RESUME_SENDS_NO_CONDJUMP ->
full force-resume -> default-ON send-resume (the proven 14x lever);
(3) re-baseline vs Cog.

## CHECKPOINT 2026-06-11t — six latent classes fixed; #3's producer still standing; the lldb session is unavoidable

- **Fixed during the #3 hunt (all real, all committed, default-safe)**:
  1. resume-path ExitBlockCreate/ArrayCreate global syncs
  2. literals +8 -> +16 (both protocol arms)
  3. C++ rj2j null-resume refusal (conversion -> regular send)
  4. ASM trampoline Lresume_null refusal (the high-frequency producer;
     ip rewound via x16 stash; predicate ALIGNED with the C++ side —
     plain bcToCode, not override-aware, else the refusals ping-pong)
  5. (+ fix #1 bcToCode poisoning, #2 headroom — the earlier cascade)
  6. PMS/B6/backjump all unaffected; default gates green throughout.
- **The #3 corruption STILL reproduces** (deterministic config,
  394 MUSTBOOL/DNU lines): the producer is none of the above.
  Remaining hypothesis space: the resume RE-ENTRY protocol itself
  (a path that re-enters with sp/operands subtly off in a way the
  depth checker only catches downstream), or an interaction in the
  ExitReturn caller-restore under mixed resumable/non-resumable
  chains.
- **THE UNAVOIDABLE NEXT STEP**: full-symbol lldb.  Procedure:
  (1) clean-rebuild build/ (the debug map went stale:
  'debug map object file does not exist'), (2) run the deterministic
  config with PHARO_SP_DEPTH_TRAP=1 + a NEW trap at the FIRST
  MUSTBOOL with a non-Boolean (the earliest signal:
  OCParser>>parseAssignment ipOff=32), (3) at the trap walk:
  framePointer_[..], the SavedFrame above, WHO resumed this
  activation (add a per-activation 'resumedBy' debug field if
  needed), and the operand window vs the depth map.
- All tooling (traps, traces, selector lists, window knobs) is
  committed and documented across checkpoints m-t.

## CHECKPOINT 2026-06-11s — #3 SHARPEST HYPOTHESIS: a resume lands past-send WITHOUT the send completing

- **Chronology correction**: the EARLIEST corruption is NOT fillFrom —
  [MUSTBOOL] #13-17 in OCParser>>parseAssignment (ipOff=32, fd=2)
  fire BEFORE the fillFrom sp-depth mismatch, with an
  OCIdentifierToken/OCSpecialCharacterToken as the jump condition.
- **The signature decodes**: the condition value = the SEND'S RECEIVER
  (the token that isIdentifier-style send was sent TO), at CONSISTENT
  stack depth (sp-depth checker exempts MUSTBOOL but saw no general
  mismatch there).  result==receiver at right depth = **the send was
  SKIPPED: a resume re-entered at the post-send (plain) label without
  C++ having completed the send** — for a 0-arg send the depth stays
  consistent and the receiver masquerades as the result.  fillFrom's
  delta=-1 then = the 1-ARG variant of the same skip (recv+arg in,
  one value out: skipping leaves depth one HIGH at the post-send
  label... observed one LOW at later exits — reconcile signs during
  the session).
- **WHERE such a resume can originate**: an exit AT a send (ip not yet
  advanced / completion not done) whose handler computes a PAST-send
  bcOffset for re-entry.  Audit list (with fix-#1's plain-label
  landing now making such mistakes EXECUTABLE rather than wedging):
  the inline tryResume at ~23395 (which bcOffset does it use, and is
  the send completed on every path into it?), the savedResumeEntry
  precompute at ~24346 (pastSendOff — only valid after ExitReturn of
  the callee), the yield re-entry at ~19717, JITRuntime tryResume
  ~3972.  The discriminating question per site: is the send GUARANTEED
  completed (retval on the memory stack) before the JIT_CALL to a
  past-send label?
- **Tools ready**: PHARO_T1_RESUME_TOS_LOG (the [JIT-MUSTBOOL-RES]
  trace) + PHARO_SP_DEPTH_TRAP + the deterministic config; the
  parseAssignment MUSTBOOL is the least-downstream signal — trace IT.
- lldb note: dev-binary debug map stale after many rebuilds
  ("debug map object file does not exist") — clean-rebuild build/
  (or dsymutil) before the interactive session.

## CHECKPOINT 2026-06-11r — #3: the off-by-one CAUGHT LIVE; lldb walk is the last step

- **The evidence chain is complete**:
  1. DNU receiver forensics: a FreeTypeCache INSTANCE receives #+
     (canonical selector) — a one-slot operand shift with named
     identities (the width-accumulator slot holds the cache object).
  2. PHARO_SP_DEPTH_CHECK (dev build) catches the shift LIVE and
     EARLIER: **Array(ArrayedCollection)>>fillFrom:with: exits at
     bcOff=10 (PushFullBlock) with delta=-1 — sp one word low — on
     EVERY resumed activation** under the deterministic window config
     (DET_SCHED + saveless-off + [2000,3500)+[3500,5000)).  The shift
     is created by the resume of the send just BEFORE bcOff 10.
  3. Isolation: single-method and 6-selector sets do NOT fire — the
     producing resume needs the broader population (callee chain
     resumable too).
- **THE LAST STEP (lldb, dev build, fully deterministic)**: break at
  the SP-DEPTH fprintf for fillFrom:with:, walk back to the resume
  entry that re-entered this activation (savedResumeEntry JIT_CALL vs
  V2 pop JIT_RESUME_CALL vs plain tryResume) and compare state.sp
  against the save/frame's recorded sp.  delta=-1 arithmetic =
  a post-send-sp vs pre-send-sp disagreement between ONE save
  producer and the continuation protocol (the V2 continuation
  subtracts nArgs and writes the retval at [sp-nArgs*8-8]; a producer
  that records a post-send sp makes that one short for 1-arg sends).
  Audit candidates: every J2JSave.sp writer (the rj2j C++ push ~19800
  region, materializeJ2JSaveIntoFrame's inverse, the xmethod emit
  push, the retro-save) for pre-vs-post-send sp semantics.
- Also landed: the literals +8->+16 fix (both arms; real latent bug,
  not this producer).
- Cascade: #1 FIXED, #2 CURED, #3 = one lldb session from the fix.

## CHECKPOINT 2026-06-11q — #3 narrowed to a first-failure site; isolation negative; forensics next

- **Divergence finder built + run** (PHARO_BLOCK_CREATE_TRACE, 60K
  creates, structural diff): the wedge and clean runs are IDENTICAL
  through create #27035; at #27036 the wedge enters handleError:log:.
  **First failure: '#+' DNU in FreeTypeFont>>widthOfString:from:to:**
  (the width-accumulating send-in-loop) under Dictionary>>
  at:ifAbsentPut: from widthAndKernedWidthOfLeft:right:into:.
- **Isolation NEGATIVE**: PHARO_T1_RESUME_ONLY_SEL now takes a COMMA
  LIST (committed); single/pair/quad of the visible stack's methods
  all PASS.  Window bisection hit its resolution limit (unions wedge,
  halves pass at every level) — the trigger is a specific dynamic
  resume SEQUENCE that only occurs at population scale, deterministic
  under DET_SCHED.
- **NEXT (forensics, in order)**:
  1. Dump the DNU RECEIVER (class + raw bits) at doesNotUnderstand
     when selector==#+ under the wedge config — shifted-slot garbage
     vs a valid-but-wrong object distinguishes operand-shift from
     slot-misalignment.
  2. Trace resume EVENTS into widthOfString:from:to:'s frame lineage:
     log every JIT_RESUME_CALL/savedResumeEntry re-entry whose target
     method is one of the four stack methods (selector match on
     state.jitMethod) with {bcOff, sp, depth, x1} — the failing
     iteration's entry will show the protocol step that shifted.
  3. lldb on the DET_SCHED repro: break at sendDoesNotUnderstand
     (selector #+), walk the operand stack + savedFrames_ + the pool.
- Tooling landed this stretch: comma-list RESUME_ONLY_SEL, the
  60K-create divergence finder, resume window-pair knobs.

## CHECKPOINT 2026-06-11p — #3: resume-path create-handlers hardened (negative result); trace next

- Resume-loop ExitBlockCreate/ExitArrayCreate now sync receiver_/
  framePointer_/method_/homeMethod_ from state unconditionally
  (commit "resume-path ExitBlockCreate/ExitArrayCreate sync") — a real
  staleness hole, but **the deterministic #3 repro STILL wedges**:
  the corrupt-closure signature has another source.
- **NEXT (trace, not inference)**: knob-gated dump in
  createFullBlockWithLiteral — per create: {stackPointer_, framePointer_,
  the numCopied copied values, the outerContext oop chosen, method_
  selector}; run the deterministic repro AND a clean resume-off run of
  the same DET_SCHED schedule; diff streams; the first divergence
  names the corrupt field + its producer.  Suspects list, ranked:
  (a) copied VALUES wrong (operand stack shifted at create — would
  point back at a resume-entry sp bug, e.g. the V2 pop's stur/sp
  adjust interacting with creates BETWEEN sends), (b) outerContext
  machinery (materialized-context identity), (c) something earlier
  corrupting the stack that creates merely capture.
- Cascade: #1 FIXED, #2 CURED, #3 = one trace away from its producer.

## CHECKPOINT 2026-06-11o — #2 CURED (headroom reservation); #3 refined to corrupt-closure creation

- **#2 is DONE** (commit "cascade #2 CURED"): the saveless fast path
  reserves 64 saves of pool headroom and falls back to the save-push
  path near capacity — the retro pool-full class is unreachable by
  construction (and sidesteps the chained-handoff clobber problem the
  single-slot ExitRetroFull design had; that machinery stays as
  insurance).  bisect-ALL: exit 133 (brk) -> 124 (#3's wedge only).
  Default config verified unchanged.
- **#3 SIGNATURE REFINED** (deterministic repro unchanged): the run
  is littered with "<Error printing blockClosure ... Error printing
  the compiledBlock in FreeTypeFont>>widthAndKernedWidth... /
  Dictionary>>at:ifAbsentPut: ..." — FullBlockClosures CREATED while
  a resumed frame is live have corrupt innards (outerContext /
  compiledBlock), which also explains the cannotReturn: (a ^ through
  a corrupt outerContext).  This is the ExitBlockCreate-stale-state
  FAMILY (the 2026-06-09 xmethod sortBlock bug's sibling — see the
  materializeJ2J lambda's comment about ExitBlockCreate sp-resync
  skips at ~23267): resume's state re-basing (j2jDepth/cursor/sp)
  leaves something stale that the closure-creation path captures.
  NEXT: trace ExitBlockCreate under the deterministic repro — dump
  state.{sp,tempBase,j2jDepth,cursor} + the created closure's
  outerContext at each block-create while a resumed frame is live;
  compare against the non-resume run.  Check BOTH resume entry
  classes (savedResumeEntry path resets depth/cursor at ~24614;
  the V2 save-pop JIT_RESUME_CALL path at ~19999/22970 must NOT
  reset mid-chain — verify each maintains what block-create reads).
- Cascade scoreboard: #1 FIXED, #2 CURED, #3 = corrupt-closure
  creation under resume (deterministic, family-known).  After #3:
  stage resume -> the 14x iterative-workload win.

## CHECKPOINT 2026-06-11n — cascade #2 emit-half landed; #3 = NLR-vs-resume (the real architecture item)

- **#2 emit side SHIPPED** (commit "cascade #2 emit side"): the
  retro-save pool-full brk is now a graceful ExitRetroFull handoff
  (retro* JITState fields @272+).  REMAINING = the C++ handler at the
  dispatch switches (precise plan in the commit message; ordering:
  drain-then-retro-innermost).  Unreachable in default config —
  safe as landed.
- **#3 (the NLR cannotReturn under broad resume)** is the remaining
  blocker and the real architecture item: resumed activations break
  `^` home-finding (Context identity across materialize->resume; the
  married/widowed-context problem).  Fix design sketch: when resume
  re-enters a frame that was materialized (has a SavedFrame/Context),
  either (a) keep the SavedFrame entry alive + marked resumed so the
  home-search (Interpreter.cpp ~11445 family) can match it to the
  live JIT frame, or (b) Cog-style: re-marry — record the Context oop
  in the resumed frame's identity so NLR targeting the Context
  resolves to the live activation.  Start by instrumenting WHICH
  home-search fails (the cannotReturn signal path) under the
  deterministic config: DET_SCHED + saveless-off + windows
  [2000,3500)+[3500,5000).
- After #2-C++ + #3: stage resume (RESUME_SENDS_NO_CONDJUMP -> full)
  with the ladder; the measured prize = 14x on send-loops, i.e. the
  iterative-workload suite family.

## CHECKPOINT 2026-06-11m — send-resume fix #1 LANDED; cascade components #2/#3 mapped

- **Fix #1 SHIPPED + validated** (commit "SEND-RESUME FIX #1"): plain
  bcToCode + per-JM override side table (resumeOvOffset, JM 112->120,
  appended at END — the stencils.cpp JITMethod_mirror models the first
  96 bytes by FIXED offsets, never insert mid-struct).  One retval-
  carrying consumer switched (the V2 save packer ~19800); all others
  plain by construction.  The DET_SCHED max: repro PASSES; windows
  [0,1500) pass; default config untouched (all default gates green).
- **Component #2**: bisect-ALL (and FORCE) crash exit=133 = the
  SAVELESS retro-save pool-overflow brk (AsmjitT1.cpp ~5139,
  0xDEAE) — broad resume deepens save chains past the pool.  Fix
  direction: graceful pool-full path (restore the stash, hand the
  frame to C++ via state fields + a new exit reason; C++ materializes
  the pool + this frame, resets cursor, resumes) — OR first try:
  saveless-off interaction quantified (next).
- **Component #3**: with saveless OFF + force resume -> only-idle
  WEDGE again (exit=124).  Bisect it with PHARO_T1_RESUME_MIN/
  MAX_COMPILE on a PHARO_T1_NO_CAN_SKIP_J2J_SAVE=1 build-opt run;
  then DET_SCHED + the DNU-stack tracing as for #1.  NOTE the seq2
  counter counts compile ATTEMPTS (incl. failures) — startup exceeds
  5000; use MAX=-1 for 'everything'.
- **#3 CHARACTERIZED (bisect data)**: [0,2000) ok, [2000,8000) WEDGE,
  [2000,5000) WEDGE, but BOTH halves [2000,3500)/[3500,5000) ok and
  [2000,2750)/[2750,3500) ok -> POPULATION-DEPENDENT, not a single
  method: the wedge needs enough simultaneously-resumable methods
  (pair interaction or resource exhaustion — prime suspects: the J2J
  save pool filling with materialize-only/live saves, SavedFrames/
  frameDepth limits, or the j2jBailFull handling under resume).
  Diagnose with: pool-occupancy counter at wedge time + the WEDGE
  process-table lldb recipe (memory sunit-fullsuite-blocker-cascade)
  + DET_SCHED on a wedging window for determinism.
- **#3 ROOT-CAUSED (deterministic: DET_SCHED + saveless-off +
  windows [2000,3500)+[3500,5000))**: the wedge output contains
  character-INTERLEAVED error prints (two processes printing at once)
  that deinterleave to **Context>>cannotReturn: +
  Exception>>handleSignal: + NonInteractiveUIManager>>handleError:**,
  and [DIAG-TIMER] shows timerSem=nil/armed=0 at the wedge.  Chain:
  a NON-LOCAL RETURN from (or through) a RESUMED activation fails
  home-context identification -> cannotReturn: -> error cascade kills
  the active process -> Delay scheduler never re-armed -> only-idle.
  This is the known NLR home-finding weakness (memory
  nlr-nested-valuewithexit-bug: home matched by method-oop,
  innermost) interacting with resume's frame identity: resumed
  callers' frames are re-entered/re-materialized and blocks' `^`
  can't find home.  FIX AREA: the NLR home-finding vs
  SavedFrame/materialize bookkeeping across resume
  (Interpreter.cpp ~11445 home-matching + materializeJ2JSaveIntoFrame
  + the resume entry's frame identity).  NOT pool exhaustion (8x pool
  no effect); population-dependence = more resumable methods => more
  blocks whose home is a resumed frame.
- The prize (re-verified post-fix-#1 pending): single-method resume
  sendloop 345 -> 25 ms (14x).  After #2/#3: stage
  RESUME_SENDS_NO_CONDJUMP -> full -> default with the ladder
  (asTuple canary = AIPrim DET_SCHED; CharacterTest;
  200-class A/B; the 16K soak).

## CHECKPOINT 2026-06-11l — SEND-RESUME ROOT CAUSE + the 14x sendloop proof; FSR design landed

- **THE SUITE LEVER, proven**: enabling mid-method resume for ONE
  send-bearing method (PHARO_T1_RESUME_ONLY_SEL=sendloop) took the
  send-in-loop bench from 345-381 ms to **25 ms (14x)** — the
  advertiseResume gate (numSendSites > 0 -> no resume) is why every
  send-bearing activation that exits ONCE finishes interpreted, and
  why iterative send-loops (the FFI/text run-killer family) crawl.
- **THE WEDGE ROOT CAUSE (deterministic repro: PHARO_DET_SCHED=1
  PHARO_T1_RESUME_ONLY_SEL=max: on build-opt, 45s, only-idle wedge
  with #isInteger DNUs + WorldMorph>>ifTrue: = one-slot-shifted
  operand stack)**: the V2 emit APPLIES resumeOverrides INTO the
  bcToCode table — bcToCode[postSendOff] points at the
  resumeAfterCall continuation, which assumes the retval-in-x1
  JIT_RESUME_CALL protocol (pops nArgs, sturs x1).  But bcToCode
  serves MANY consumers; interp-side/tryResume entries (operand
  stack already complete in memory, NO retval in x1) land on the
  same continuation -> double-pop + garbage store.  Send-free
  methods have no overrides — exactly why only they were safe.
- **THE FIX (designed, not yet implemented)**:
  1. bcToCode reverts to PLAIN next-bytecode offsets (stop applying
     the overrides into the table in emitMethodBytes ~7521 'for
     (auto& ov : resumeOverrides)').
  2. Store the override pairs (bcOff -> continuationCodeOff) in a
     small per-JM side table (size it like the patch map; fill next
     to it).
  3. Add JITMethod::codeOffsetForResume(bcOff): consult the pairs,
     fall back to plain.  Switch ONLY the retval-carrying resume
     consumers to it.  NOTE: the V2 save-pool pops (JIT_RESUME_CALL
     at Interpreter.cpp ~19999/22970) use save.resumeAddr DIRECTLY
     (no bcToCode) — already correct.  The consumers to CLASSIFY
     (retval-carrying vs plain-entry) are the codeOffsetForBC sites:
     Interpreter.cpp 19717, 19800, 22781, 23395, 24190, 24346 (the
     'pastSendOff' one!), JITRuntime.cpp 3972 (tryResume!), 4187.
     For each: does C++ push the retval to the MEMORY stack before
     entering (plain target, stack complete) or hand it via
     JIT_RESUME_CALL x1 (continuation target)?  Get this per-site
     classification RIGHT — a mistake re-wedges; the DET_SCHED repro
     validates each in 45s.
  4. Then advertiseResume for send-bearing methods (knob-staged:
     RESUME_SENDS first, then default) + the full ladder (the
     historical asTuple corruption canary = AIPrim DET_SCHED;
     CharacterTest>>testStoreStringAll; 200-class A/B).
- **FSR design landed** (docs/frame-state-residency.md, wf_f0056620):
  x19-invariant + derive-at-boundary; batches M0-M6; found two real
  latent bugs en route (literals +8-vs-+16 at JITRuntime.cpp:890/899;
  trampoline Lcall never establishes x19).  IMPLEMENT AFTER the
  send-resume fix (resume is strictly higher leverage: 14x on loops
  vs ~25% of the linked round trip).
- Bisect tooling committed: PHARO_T1_RESUME_MIN/MAX_COMPILE.

## CHECKPOINT 2026-06-11k — NATIVE LOOP BACK EDGES + the OSR ping-pong discovery

- **Loops now JIT** (commit "NATIVE loop back edges"): the ExtendA/B
  prefix handler bailed every prefixed bytecode -> every to:do:/
  whileTrue back edge dropped the rest of the activation to interp
  FOREVER (the floor-bench contradiction exposed it: 29.6ns/iter
  "empty loop" = the interpreter).  Now: ExtendB+ExtJump emits
  natively; back edges poll forceYield_ (baked addr) and bail on
  heartbeat preemption; DET_SCHED keeps the old bail; opt-out
  PHARO_T1_NO_NATIVE_BACKJUMP.  All gates green.
- Measured: sendless loops ~flat (naive JIT body ~= interp for simple
  bytecodes at -O2); send-in-loop ~6%.  BUT:
- **NEXT INVESTIGATION (the suite-scale lever): the send-in-loop
  pathology** — 'sendloop: s := s incc inside 1 to: 3M do:' runs at
  117ns/send-iteration BOTH knob states, with OSR=155K (baseline 14K),
  activations NOT growing with the 3M sends, inline-J2J hits BELOW
  bare-startup baseline.  The loop ping-pongs interp<->OSR ~21
  iterations per cycle and its send sites never IC-classify.  This is
  very plausibly THE mechanism behind the FFI/text run-killer family
  (platformLongAt: loops at 8490 sends/sec).  Diagnose: WHY does the
  OSR-entered activation exit per ~21 iterations (what exit reason?),
  and why don't the loop's IC sites classify (pendingICPatch_ owner =
  the OSR variant? upgradeICToJ2J never sees ExitSendCached because
  the site misses every time after IC flush? trace with
  PHARO_JIT_FAIL_REASONS + SLOW-EXIT-SEND + a per-exit-reason count
  on a sendloop run).
- Status vs Cog (quiet, default config): cfib ~25, benchFib ~69-80,
  sfib ~31 (Cog 8/25/12).  The microbench gap is send-activation
  machinery (frame-state design workflow wf_f0056620 running); the
  SUITE gap now has a named candidate (the OSR ping-pong above).

## CHECKPOINT 2026-06-11j — simStack VERDICT: parked opt-in (design's own rule); next lever = frame-state residency

- **Interleaved 5x5 quiet A/B (the design's measurement protocol):**
  cfib OFF {25,26,29,27,26} vs ON {24,26,26,25,27} — median 26 BOTH,
  min 25 vs 24.  The simStack effect at -O2 quiet is ~0-4% (the
  earlier ~9% was load-amplified memory traffic).  floor + benchFib:
  wash.  **6th instance of the OoO lesson** — the round trips ARE
  killed (structurally verifiable) but store-to-load forwarding +
  OoO absorb them on this core at -O2.
- **DECISION per the design's own park rule (<3% ensemble): simStack
  stays OPT-IN (PHARO_T1_TOS_REG), correct and fully soak-validated
  (20K+ tests, zero VERIFY traps), available for: x86 (in-order-ish
  cores may benefit), loaded conditions, and as the foundation for
  future fusion work.  NO B5 flip.**
- **THE REMAINING GAP, final decomposition (design §9 + measurements):**
  cfib ~26ms vs Cog 8 vs skeleton 4.  The ~20ms over skeleton is
  ~230/310 per-activation insns of SEND-ACTIVATION MACHINERY:
  maintaining the JITState mirror (receiver/method/literals/argCount/
  ip/tempBase stores per call + save-pool RMWs) that Cog doesn't do —
  Cog's machine frames ARE its state; ours shadows everything into
  memory per activation.  THE next lever = **frame-state residency**
  (keep activation state implicit in frame/registers, materialize on
  exit) — an architectural design problem; workflow next.

## CHECKPOINT 2026-06-11i — simStack B2 COMPLETE (B2a/b/c); soak running

- **B2 is fully landed behind PHARO_T1_TOS_REG**: B2a (arith consumes
  TOS from x26), B2b (arith produces x26 — all 7 end-paths re-arm,
  validity once at end), B2c (0-arg send-head receiver feed +
  resumeAfterCall mov x26,x1 + ALL 27 endOfSend spec arrivals through
  the per-site tosLrearm stub = the RM-F1 fix; validity claimed at the
  send case end; both arrival classes re-arm AFTER the callee returns
  so in-send GC cannot stale x26).
- Every step gated under PHARO_T1_TOS_VERIFY (the per-consumer
  ldur/cmp/brk net): cfib + DictionaryTest 205/205 + DET_SCHED 10/10,
  zero traps at every batch.
- **Perf so far (within-binary, build-opt): cfib ON 29-30 vs OFF
  32-33 (~9%)** — inside the design's honest -5..-15% band.
- 200-class+ soak with TOS_VERIFY + PATCH_VERIFY running in background.
- **B4 fusion SHIPPED** (commit "simStack B4"): cmp+b.cond direct for
  comparison+naked-forward-cond-jump pairs; the jump keeps its unfused
  emit at its bcLabel; pops published before the branch.  Eval gates
  green under VERIFY (cfib's '< 2 ifTrue:' is the fused pair).
  QUEUED behind the running soak: Dictionary + DET_SCHED single-class
  gates (the soak owns /tmp/sunit_test_detail.txt — do NOT run
  single-class until BATCH COMPLETE), quiet perf re-baseline, then
  the B5 default-flip decision.
- **B3.1 + B3.2 SHIPPED**: constSmI single-check shrink (arith dual
  5-op check -> 3-op single-side when TOS is a known SmI constant;
  fires on cfib's '< 2'/'- 1'; mixed-type float bail verified) and
  popStoreRecvVar value-from-x26 (immutable bail before sp decrement
  preserved).  Both VERIFY-gated.  B3.3 post-blr re-arm SKIPPED
  (expected ~0 per design's park rule); B3.4 join-merge PARKED.
- **SOAK MILESTONE (2026-06-11 early am)**: the B2 soak (TOS_REG +
  TOS_VERIFY + PATCH_VERIFY, -O0 dev) ran PAST every previous
  endpoint — 20,336+ tests vs the prior ~16.7K max — with ZERO VERIFY
  traps and ZERO mirror drift.  Baselined-span comparison (first
  16,752 vs the B6 run): 29 vs 26 fails, identical composition (Cly*
  14, CodeSimulation 2, environmental singles) -> no simStack
  regression.  The 20K+ region's new fails are the DOCUMENTED gap
  families (PNG 37, Fuel ~30, Mic resources, FileAttributes/LibTTY
  env-dependent) — no session baseline exists there; they are the
  suite-gap work, not simStack regressions.
- **NEXT after soak completes**: quiet perf re-baseline (cfib/
  benchFib/sfib/floor ON vs OFF x5) -> B5 default-flip decision
  (flip = PHARO_T1_NO_TOS_REG opt-out + 200-class per-test A/B).
- (orig) REMAINING simStack: B3 (constSmI single-check shrink,
  popStoreRecvVar, post-blr rearm, join-label merge), B4 (cmp+b.cond
  fusion — the suite's #1 pair), B5 default flip after soak.
- NOTE for the flip decision: with TOS default-on the suite/bench
  numbers re-baseline; expect cfib ~29-30 quiet -> recompute the Cog
  gap then.

## CHECKPOINT 2026-06-11h — simStack B0+B1 SHIPPED; next = B2 (the payoff batch)

- **B0 + B1 are LIVE behind PHARO_T1_TOS_REG** (commits "simStack B0",
  "B1 pass A", "B1 pass B").  Producers: all simple pushes,
  PushInteger/PushCharacter (constSmI tagged), Dup.  Consumers: cond
  jumps, returnTop, popStoreTemp.  All verified: cfib correct under
  PHARO_T1_TOS_VERIFY (zero brk traps), Dictionary 205/205 under
  VERIFY, DET_SCHED 10/10, knob-off structurally identical.
  Perf at B1: cfib 30-31 vs 31-33 (small, expected).
- **NEXT = B2 (design §8): the payoff** — REQUIRES the Lrearm fix in
  the same batch:
  1. inline arith family (isPhase3ArithOp at ~3380): TOS from x26
     (kills the ldur of sp[-8]), result -> x26 + stur, valid=true;
     bails BEFORE result store + sp adjust (memory still exact)
  2. 0-arg send head: mov x1, x26 replaces the receiver ldur (the
     PMS patched head + probe head feed; W-offsets are label-derived
     so the patch map is unaffected)
  3. resumeAfterCall continuation: + mov x26, x1 (retval), valid
     downstream; THEN retarget every spec-path `b endOfSend` (~27)
     through a per-site `Lrearm: ldur x26,[x25,#-8]; b endOfSend`
     stub — design §5 RM-F1: a single valid flag across all arrivals
     leaves x26 = stale pre-send junk on inline-spec hits ->
     wrong-receiver IC keys.  resumeAfterCall falls through load-free.
  4. inline-getter result: + mov x26, x6 when the result lands at TOS
  GATE: B1 gates + PHARO_SP_DEPTH_CHECK clean + full bench 19/19 +
  60-class + cfib/benchFib/sfib A/B x5 + ONE FULL RUN under
  PHARO_T1_TOS_VERIFY (the deterministic net for exactly the Lrearm
  bug class) + DET_SCHED canaries.
- After B2: B3 (constSmI shrink, popStoreRecvVar, post-blr rearm,
  join-label merge), B4 (cmp+b.cond fusion), B5 default flip.

## CHECKPOINT 2026-06-11g — simStack design landed; B0 in progress

- **docs/simstack-design.md is the working plan** (write-through TOS
  cache in x26; all 30 review findings resolved; batches B0-B5 §8).
- B0 progress: the 4 knobs are IN debug_vars.h (TOS_REG/TOS_MASK/
  TOS_POISON/TOS_VERIFY).  REMAINING B0 (design §8 B0 box):
  1. file-scope T1TosCache + g_tos/g_tosIn + snapshot-then-clear at
     BOTH dispatch heads (the arm64 emit loop ~7189 after
     a.bind(bcLabels[globalIdx]) AND before each inline multi-byte
     handler's emit; jump-target bitmap forces g_tos={} at BIND time)
  2. jump-target bitmap prescan in emitMethodBytes — mirror
     BcDepthMap.cpp's decode EXACTLY (ExtJump offset = byte +
     (extB<<8) UNSIGNED byte; short jump helpers in SistaV1) + emit-
     time assert in emitOne jump emits: target not marked -> return
     false (compile hard-fail -> interp fallback)
  3. JIT_CALL + JIT_RESUME_CALL (JITState.hpp): add post-blr
     `ldur x26,[x25,#-8]`-equivalent (ldr x26 from x25-8) + "x26" in
     clobbers
  4. TrampolineAsm.S :226/:292/:477 + pharo_jit_osr_resume: x26
     re-establish after each x25 hoist + osr x26 save/restore at
     frame #48/#56
  5. #error guard: PHARO_T1_TB_IN_X26 must be 0 when TOS cache code
     is present
  GATE: PHARO_T1_DUMP_SEL=cfib dumps byte-identical knob-off AND
  knob-on (B0 emits nothing); bench 19/19; 60-class identical.
- B6 suite gate running in background (BATCH-COMPLETE waiter).

## CHECKPOINT 2026-06-11f — B6 #class inline landed; gap ~2.6-2.9x; simStack next

- **QUIET BENCH STATE (2026-06-10 21:50, everything default-on):**
      cfib(30)        22-24 ms   (Cog  8;  2.9x)   [was 43x at arc start]
      benchFib(28)x10 69 ms      (Cog 25;  2.8x)
      sfib(30)        31 ms      (Cog 12;  2.6x)
- **B4 EXTENDED GATE**: default config ran 10765 tests before being
  stopped (watermark passed): 4 fails, all known/pre-existing, zero
  mirror drift.
- **B6 phase 1a SHIPPED** (commit "B6 phase 1a"): prim-111 #class =
  kPrimKindClass(24) extras with NO bit 60, classifier in
  upgradeICToJ2J (no compile attempt — the dispatch-A snippet
  replaces the callee), emit compare in the heap fall-through, F1
  even/odd full-field-decode fix (a pk-24 immediate would have
  returned a BOOLEAN from #class under INLINE_EVEN_ODD), F4 stencil
  arity routing.  Compiled-method class bench -17% (195 vs 234).
  **MEASUREMENT LESSON: eval-doIt loops run INTERPRETED — they never
  exercise dispatch-A.  Bench #class (and anything send-path) inside
  an Integer compile:'d method.**  The historical "o class 3.4M/s"
  figure likely measured the interp path; the interp-send #class
  fast path + stencil-tier pk-24 handler + 1b immediates are
  follow-ups (design + review in /tmp/b6_design.md /tmp/b6_review.md,
  workflow wf_b266f2c3).
- **THE FLOOR MEASUREMENT (B6 epilogue, decisive for lever ranking):**
  an EMPTY `1 to: 3000000 do: [:i | ]` loop in a T1-compiled method
  costs 83 ms = **28 ns/iteration of pure naive-emit loop machinery**
  (push/compare/jump/increment memory round-trips).  A #class send
  adds only ~21 ns ON vs ~23 ns OFF on top.  The loop FLOOR alone,
  at suite scale, is bigger than the whole remaining Cog gap —
  simStack is now unambiguously the #1 lever.  (B6 inline stays:
  correct, cheap, helps the C++-exit-heavy shapes.)
- simStack synthesis workflow resumed (wf_8ad1bdf7, designs+reviews
  cached; synthesis had hit the session limit) -> docs/simstack-design.md.
- Also landed today: §14 #5 eager-compile owner re-validation,
  PHARO_T1_PATCH_MAX_IC (Q3 probe), compact() tripwire, §14 #4.
- **Q3 probe results (PATCH_MAX_IC=4)**: cfib 22 / benchFib 68 (flat
  vs default — cfib callees already <=1 IC), Dictionary 205/205 with
  VERIFY silent, DET_SCHED 10/10.  Safe; suite-scale benefit unproven
  -> left non-default.  Flip candidate after a 60-class A/B if the
  suite-scale linked population matters.

## CHECKPOINT 2026-06-11e — PMS design COMPLETE + B0 LANDED; resume at B1

- **docs/patched-ic-design.md is the working plan** (synthesized by an
  11-agent workflow; every adversarial-review finding resolved, §15
  traceability).  Batches B0-B6 in §11, each with binary gates.
- **B0 SHIPPED** (commit "PMS B0"): knobs (PHARO_T1_PATCHED_SENDS
  family in debug_vars.h), SendSitePatch map (+patchMapOffset in the
  old _pad_76), SendSitePatcher encoders (asmjit-cross-validated at
  startup), entryAddrFor, link/unlink skeletons + counters,
  ScopedPatchWriteAccess (restores ENTERED W^X mode via a platform
  thread-local shadow — depth counter rejected, the VM has deliberate
  unmatched defensive force-X calls), jit_rt_fill_ic tripwire.
  Gate passed: selfcheck, eval, cfib 30ms, DictionaryTest 205/205.
- **B1a SHIPPED** (commit "PMS B1a"): the §2.1 head shape + Lprobe/
  LmissNoX5 restructure + patch-map plumbing (labels -> post-flatten
  offsets -> in-zone SendSitePatch map), PHARO_T1_EMIT_HASH harness.
  tailOffset=0 everywhere — heads exist, nothing can link.  Gates
  passed: knob-off emit STRUCTURALLY identical (the byte-identity gate
  must classify diffs: baked global-counter ADDRESSES differ per
  binary — all 60 differing words were mov/movk immediates, the
  correct check), knob-on cfib(28)=1028457, DictionaryTest 205/205,
  unlinked overhead <=3% within-binary, DET_SCHED AIPrim 10/10.
- **B1b SHIPPED (B1 COMPLETE)** (commit 26a8ec83): §2.2 tail skeleton
  per site (W3-W5 calleeJM immediates, cursor ldp + b.hs Lprobe,
  packed V2 resume, save push, callee state off immediate x10,
  dynamic nil-fill, W6 placeholder) + tail offsets in the patch map +
  the invariant-4 no-BL-precedes-patch-word scan per compile.  All
  gates green (knob-off structurally identical, knob-on correct +
  tail disasm-verified, Dictionary 205/205, DET_SCHED 10/10, zero
  within-binary overhead, +198 B/site knob-on).
- **B2 SHIPPED AND GREEN (2026-06-10)**: linking LIVE.  All §6
  triggers + §7 events wired (commit "PMS B2"); linkSendSite is
  pure-derive; batch walks one-window via the W^X shadow.  GATES:
  knob-on 200-class suite **1/8459** (only the ProcessTerminate
  flake; BETTER than knob-off baseline) with PATCH_VERIFY silent;
  5x forced-GC storm, Integer-redefinition storm (immediate effect),
  1MB-zone eviction storm — all correct, verify clean; sieve 97 (off
  102), sort 449; cfib within-binary A/B: LINKING 30-33 vs NOLINK
  34-36 (~-10%, below the 15-30% hope — the tail still pays the full
  V2 save push; saveless tail variant is the B5 upside).  Startup:
  ~1.4K links, ~290 live.  Crash lesson recorded: the patch map is
  IN MAP_JIT — rec.flags writes must stay inside the write window.
- **B3 SOAK PASSED + B4 FLIPPED (2026-06-10 evening)**: soak = 16372
  tests knob-on, VERIFY silent, 24 fails = 5 known-environmental + a
  pre-existing extended-region cluster (ClyFilter/ClyAsyncQuery 14,
  CodeSimulation 2, OCClosure, OCClassBuilder) reproduced IDENTICALLY
  knob-off (single-class A/B) -> zero knob-induced regressions.
  **PMS is DEFAULT-ON** (opt-out PHARO_T1_NO_PATCHED_SENDS=1).
  Default-config gates: Dictionary 205/205, cfib value correct,
  ~1.1K startup links.  Default-config 200-class suite gate running.
  Also landed: §14 #5 fix (upgradeICToJ2J owner re-validation after
  the eager compile — confirmed WAF window), §14 #4 comment,
  compact() tripwire.
- **NEXT**: B5 saveless/self-rec tail variants + MAX_IC widening via
  link-time gates (perf upside; two design workflows in flight:
  B6 prim-111 #class classifier wf_b266f2c3, simStack wf_8ad1bdf7 —
  implement from their outputs) -> quiet-machine bench re-baseline.
- (was) **B2 plan — kept for reference:**
  Implementation order inside the batch:
  1. linkSendSite body: §5 predicate (incl. extras bits 60 set /
     59,58,55 clear; callee gates via methodMap; reach check
     refuse-on-fail), §4-step-3 pre-patch opcode asserts (decoders in
     SendSitePatcher.hpp), ScopedPatchWriteAccess window, W1-LAST
     store order, ranged sys_icache_invalidate (head words + tail
     words only), counters + numPatchedSites_.
  2. unlinkSendSite: W1 := impossible store + ranged flush.
  3. §6 triggers in Interpreter.cpp: patchJITICAfterSend (both
     paths, wrote-slot-0 flag), upgradeICToJ2J (both fills +
     POST-compile() owner re-validation), chain-loop zeroing hook
     (ownership guard + floor-to-site — fixes §14 bug #1).
  4. §7 events: 1 recoverAfterGC unlinkEverything (+ stale W^X
     comments update), 3 recompile re-patch via entryAddrFor +
     UNLINK on tier change + eager warm-IC link pass at recompile
     finalize, 4 eviction scrub off patch maps + fix §14 #2 (extras
     + megaCache.jitEntry on the full-flush path), 5 flushCaches
     unlinkEverything early-out, 8 splice-set unlink hooks
     (JITRuntime.cpp 2935/3481), 9 = trigger 3 above.
  5. PHARO_T1_PATCH_VERIFY walk (§10: linked-over-zeroed = FAIL).
  Gate (§11 B2): B1 suite + forced-fullGC bench loop + redefinition
  storm + 1MB-zone eviction storm + PATCH_VERIFY clean + counters
  nonzero + within-binary cfib A/B (link vs NOLINK, expect 15-30%
  send-dense) + sieve x100 / sort 100K sentinels + lldb disassemble
  of a freshly linked site.
- B1a result-check trap: an early "r=1" failure was an OUTPUT-CAPTURE
  artifact (truncated stream), not a real miscompile — verify with
  `(28 cfib) printString` and the full EVAL-RESULT line before
  debugging emit.
- Then B2 = linking + the COMPLETE §7 invalidation matrix in ONE batch
  (9 events; §6 trigger enumeration; §14 pre-existing bugs #1/#2 fixed
  in the same commits they touch).
- Current bench state (quiet, post emit-fixes): cfib 24-25 ms,
  benchFib x10 76, sfib 35 (Cog 8/25/12) = ~3.0x.  Suite 3/8459
  (all documented environmental).

## CHECKPOINT 2026-06-11d — cfib DISASSEMBLED: the gap named instruction-by-instruction

- Tooling: `PHARO_T1_DUMP_SEL=cfib` dumps emitted bytes to
  /tmp/jit_cfib_1.bin; disassemble with python3+capstone (recipe in
  the session transcript; /tmp/cfib_disasm.txt).  cfib: 18 bytecodes
  -> ~1550 instructions / 6.3KB (the known bloat, quantified).
- **Per-bytecode tax (the NEW second lever)**: every push is 5 insns
  (`ldr; mov x2,x25; str; add; mov x25,x2`) and adjacent bytecodes
  round-trip operands through memory (push then immediate ldur back
  for the compare) — a naive stack machine with no TOS register
  caching.  Store-to-load forwarding (~4-5cy) sits on the critical
  path of every bytecode; this is the naive-Cogit vs
  **StackToRegisterMappingCogit** gap (reference impl in
  ~/src/pharo-vm).  Fixed the shared helper (emitPushReg = 1
  post-index store now); the 35 inline copies + real TOS caching =
  the simStack design, AFTER patched-ICs.
- **Per-send tax (validates the patched-IC lever)**: the xmethod
  inline-J2J send is ~90 insns: icDataPtr 2-load chain, receiver tag
  tests + header classIndex extract, key cmp, extras decode chain,
  then EIGHT JITMethod-header gate loads (numIC/canBail/hasNLR/
  isXmethod/prim...) re-validating per SEND what Cog settles ONCE at
  patch time, then ~25 insns of machine-stack state stash around the
  blr, then the retro-save push + entryDepth dance on return.  The
  V2 return path is ~20 insns.  Patched monomorphic sites collapse
  the probe+gates to cmp-imm + direct branch.
- **Found+fixed a production hot-path telemetry counter**: the Eδ.2b
  canSkipJ2JSave ic-hits counter (7 insns + shared-global RMW) was
  emitted on EVERY inline-J2J hit because its gate said `xmethod ||
  counters` and xmethod went default-on.  Now counters-only.  When a
  knob flips default-ON, AUDIT what its emit-gates drag in.
- Microbench scale anchor (scripts/rasbench): bare cfib-shaped
  control flow + side-stack saves = 4ms; + IC probe & RMW sim = +0.3
  ns/call.  Cog full bench = 8ms.  Ours = 29ms.  The probe/RMW alone
  doesn't explain the gap — the LENGTH of the real sequences above
  (with their 10-15 dependent loads per send) does.
- **QUIET RE-BASELINE (suite done, harness free): today's emit fixes
  moved the gap 3.5x -> ~3.0x**: cfib 24-25 ms (was 29; Cog 8),
  benchFib x10 76 (was 87; Cog 25), sfib 35 (was 41; Cog 12).
  Suite gate on the changes: 3/8459, all documented environmental.
  The win is mostly the Eδ.2b counter removal — a shared-global RMW
  (serializing store-load on ONE address from every send site) is
  REAL cost even on the OoO core; pure instruction-count cuts are
  not.  Refines the "micro-structure is free" lesson: shared-memory
  serialization and dependent loads matter, register/mov chatter
  doesn't.

## CHECKPOINT 2026-06-11c — V2 suite-clean after findMethodByPC fix

- **V2's ONE real suite regression found+fixed**: the quiet suite gate
  came back 2 fails (vs 6 in the bench-contaminated run): WeakAnnouncer
  (known flake) + BehaviorTest>>testAllReferencesTo TIMEOUT — the
  latter deterministic (repro'd single-class), NEW vs both V1
  baselines.  Root cause: V2 resolves every save's caller via
  codeZone.findMethodByPC(resumeAddr) (both C++ chain-loop pops + the
  eviction pinner), and findMethodByPC was a LINEAR walk over every
  zone method ("for crash diagnostics") — reflective workloads
  (allReferencesTo: = system-wide scan, heavy interp/JIT crossing)
  paid O(methods) per pop.  Same trap as the old "defensive
  findMethodByPC scan was 93% of tryResume CPU" note.
- **Fix (committed)**: binary search over a lazily-rebuilt
  address-sorted snapshot (pcIndex_) of the already address-ordered
  method list; dirty flag in linkMethod/freeMethod/initialize/destroy
  (evictLRU only mutates via freeMethod; compact() needs empty list).
- Validated: BehaviorTest 43/43 PASS (was TIMEOUT); cfib 28-29ms,
  benchFib x10 80ms — equal-or-better.  Full suite gate re-running.
- LESSON for the patched-IC work: anything on the per-send or per-pop
  path must be O(1)/O(log n) in zone size; grep callers of any zone
  walk before putting it on a hot path.

## CHECKPOINT 2026-06-11b — J2JSave V2 FLIPPED; lever re-ranked to PATCHED ICs

- **PHARO_J2J_SAVE_V2 = 1 SHIPPED**: the packed 32-byte save protocol
  is live end-to-end (emit push/prelude/continuation/retro-stub,
  trampoline call/return/null-resume, all C++ producers/consumers,
  eviction pinner).  Validated: sp-depth 1.83M+76K checks 0
  mismatches, DictionaryTest 0 fails, suite gate in flight.
- **MEASURED: V2 is perf-NEUTRAL on the -O2 build** (cfib 29-30,
  sfib 40, benchFib x10 87-90 — within noise of V1; the dev -O0 build
  shows the real -25% instruction win, which the M-series OoO core
  fully hides at -O2).  Third independent instruction-count-is-free
  result (leak guard, x26, V2).  KEEPING V2: structurally better
  (fold-bug class impossible, GC-stable saves, half the pool traffic).
- **THE REMAINING 3.5x, FINAL DIAGNOSIS**: (a) microbench gap =
  IC-probe LOAD LATENCY (icBuffer ldr -> entry ldr -> cmp dependent
  chain ~12+ cycles vs Cog's patched-immediate class check) — the fix
  is **Cog-style PATCHED MONOMORPHIC SITES** (bake expected-class +
  target into the instruction stream, patch on first fill; needs a
  post-zone-copy patch pass — the same infrastructure the baked-IC
  idea needed); (b) suite/real-workload gap = the 22.6% inline-J2J
  catch rate (77% of sends round-trip through C++) — raise via
  admission (MAX_IC sweep is FLAT on microbenches but untested at
  suite scale) and via patched sites (which inline the dispatch too).
  MAX_IC sweep on cfib: flat (31-34ms) — its callee already inlines.
- **SEND-FLOW ACCOUNTING on cfib (build-opt, V2) — FINAL, after a
  bare-startup baseline (`eval "1+1"`) and counter-source reads:**
  - Startup-vs-loop subtraction: ALL C++ IC-miss traffic (1.73M
    lookups, noICData=689K) and ALL gate bails (extra_no_bit60=722K,
    bail_self=282K, xmethod prim/numic bails) are IMAGE STARTUP.
    The loop adds ~zero of any of them.
  - **The cfib loop runs 100% inline in machine code**: 7.14M loop
    sends, all IC-hit — 4.75M dispatch-A inline-J2J (the two cfib
    self-recursive sites) + 2.38M xmethod inline-J2J (incc).  The
    "5M J2J stencil calls" are NOT trampoline hops: the inline
    dispatch-A call sequence itself bumps j2jTotalCalls (x20 dual
    depth+totalCalls increment, AsmjitT1.cpp ~5092), and
    jitJ2JStencilCalls_ aggregates that counter at exit.  Two false
    leads killed: no C++ hop, no trampoline hop.
  - **So the 3.5x is pure per-send cost of the inline machinery**:
    (1) IC probe = 3 dependent loads (icBuffer ptr -> entry -> cmp)
    before any decision; (2) the extras/tbnz dispatch chain;
    (3) the J2J call sequence: V2 save pack (movk+2xstp), cursor
    ldr/str, depth ldr/add/str (~15-20 insns with 2 read-modify-write
    latency chains); (4) returns are computed `br` (no RAS pairing).
  - **(4) RAS-defeat hypothesis REFUTED by standalone microbench**
    (/tmp/rasbench): fib(30)-shaped recursion, bl/ret vs br-call +
    side-stack-save + indirect-br-return (exact V2 return shape):
    ratio 0.98-1.13x — the M-series indirect predictor handles the
    alternating return targets near-perfectly.  br control flow is
    FINE; do NOT redesign J2J around bl/ret.  (4th independent
    "micro-structure is free, dependent loads aren't" result.)
  - Same microbench, absolute scale: the bare cfib control-flow
    skeleton (2.7M calls + side-stack saves) = ~4 ms.  Cog full
    benchmark = 8 ms, ours = 29 ms.  The ~25 ms over skeleton is the
    per-send IC-probe dependent loads + dispatch chain + prelude work
    — i.e., (1)+(2) dominate, exactly what patched-IC sites remove.
    Design workflow in flight: docs/patched-ic-design.md incoming.

## CHECKPOINT 2026-06-11 — sp-residency LIVE; gap = 3.5x

- **PHARO_T1_SP_IN_X25 = 1 SHIPPED**: state.sp is register-resident in
  x25 across all JIT execution (full contract: live-in at every entry,
  maintained by every emit site, synced at 47 rets + 10 audited BLRs,
  live-out at every blr return).  Validated: sp-depth 1.82M checks 0
  mismatches, DictionaryTest 0 fails, **200-class suite 2/8459 (both
  known environmental — correctness floor holds)**.
- **Head-to-head (build-opt vs stock Cog), the flip's effect:**
      cfib(30)         42 -> 29 ms   (Cog  8;  5.3x -> 3.6x)
      sfib(30)         54 -> 41 ms   (Cog 12;  4.5x -> 3.4x)
      benchFib(28)x10 128 -> 87 ms   (Cog 25;  5.1x -> 3.5x)
- Remaining levers toward parity (UPDATED after the x26 experiment):
  (a) tempBase residency: **TRIED AND MEASURED NET-NEGATIVE** (~25%
  regression: 13 read sites vs an exit-sync tax on every send bail;
  infrastructure kept at PHARO_T1_TB_IN_X26=0, full writeup in the
  commit).  Residency pays only for per-bytecode-WRITTEN fields.
  (b) per-method emit bloat (~6KB/method; SHA timeout = zone
  exhaustion; smaller code = better i-cache, suite-wide);
  (c) IC-probe shortening — ANALYZED (emit at ~3830): the slot-0
  monomorphic path is ~12 instr: jm(hoisted, free) -> ldr icBuffer ->
  add siteOffset -> tag-test -> classifier-leak guard (lsr+cbnz, 2
  instr, removable once task#10 bit leaks are confirmed dead) ->
  ldr header -> and classIndex -> ldr entry key -> cmp+b.  Cog is ~4
  because the expected class + target are PATCHED INTO the code.
  Cheap cuts: (i) bake icBuffer+siteOffset as movz+movk immediates —
  BLOCKED as a quick win: icBuffer is allocated by CodeZone AFTER the
  emit (would need a patch pass);
  (ii) drop the leak guard — TRIED, measured NEUTRAL on Apple Silicon
  (dual-issues under load latency; knob PHARO_T1_LEAK_GUARD_OFF kept,
  guard stays default-on).
  CONCLUSION: probe micro-cuts are exhausted on this core; the send-
  path gap is the save push/prelude (slot-reservation design) and
  the dispatch chain — plus suite-wide wins from emit-size (i-cache).
  **J2JSAVE V2 DESIGN (worked out 2026-06-11 — SUPERSEDES
  slot-reservation as the next send-path lever; simpler AND faster,
  no GC-sentinel or C++-interleave hazards because the save is always
  complete):** move statically-known work from the save/prelude to
  the per-site RESUME CONTINUATION (which knows nArgs, callerJM, and
  its own method context at EMIT time):
  - save shrinks 56 -> 32 bytes {sp, receiver, tempBase, resumeAddr}
    = 2 stp push (from ~12 instr to ~6);
  - DROP sendArgCount: the resume code does the arg pop with a
    STATIC immediate (sub x25, #nArgs*8) — also retires the
    staticJ2JArgCount fold class of bugs permanently;
  - DROP jitMethod: the resume code re-establishes state.jitMethod/
    method/literals/argCount from EMIT-TIME immediates (movz/movk
    of callerJM; 3-4 instr per site, i-cache for prelude latency);
  - DROP ip: materialize derives it from resumeAddr ->
    findMethodByPC(zone) -> jm + a 16-bit bcOffset packed in the
    resumeAddr slot's low bits? NO — simpler: materialize uses
    resumeAddr->jm (zone binary search, rare path) and the existing
    bcToCode table INVERSE (codeToBc) or store bcOffset in the
    spare 16 bits of the tempBase slot (stack addrs fit 48 bits);
  - retval moves to x1 REGISTER end-to-end (prelude already has it;
    the resume writes it with the static-offset stur — Cog-style);
  - prelude becomes: pop 2 ldps + cursor/depth-- + br resumeAddr
    (~7 instr from ~14).
  Estimated: ~10-12 instr off every J2J call+return pair (~40% of
  the remaining per-send cost).  Same execution pattern: V2 behind a
  build switch, both protocols compiled, batches green, flip last.
  Consumers to convert: push emits (generic + xmethod + retro-stub),
  prelude, TrampolineAsm push/pop paths, C++ pops (2 chain loops),
  materializeJ2JSaveIntoFrame, prepareForGC/afterGC pool walk,
  forEachRoot save.receiver visit (offset changes), J2JSave struct +
  JSV_* asm constants + the sp-depth save checker.
  BATCHES 0, 1a, 1b, 1c DONE (layout header; JIT_RESUME_CALL at both
  C++ pops; both pops dual-protocol with findMethodByPC(resumeAddr)
  caller resolution).  BATCH 2 DESIGN (resolved): pack the V2
  resumeAddr slot as addr(48 bits) | bcOff(12)<<48 | nArgs(4)<<60 —
  push sites know bcOff+nArgs statically; the prelude masks the
  address with one AND before br; materialize unpacks for ip
  (bcStart+bcOff) and matRetSlot (nArgs); methods with bcLen > 4095
  fall back to V1-style handling (compile-time gate).  GC SIMPLIFIES
  under V2: the packed bcOff is an offset (GC-stable) — the
  prepareForGC/afterGC pool ip-stash machinery (ipOffset field) is
  V1-only; forEachRoot's save.receiver visit is offset-compatible.
  Batch 2 edits: dual J2JSave struct (V2 = 4 fields/32B), materialize
  unpack path, gate the GC pool walks + .ipOffset uses (2 sites),
  then batch 3 = the emit (push/prelude/resume continuations).
  2a-2d DONE (GC walks; dual struct; all 17 C++ stragglers).  3a DONE
  (emit layout constants via JSV_*).  3b DONE (V2 packed push emit:
  movk-pack + 2 stps = 5 instr, compiles at V2=1 only).
  **V2 KEYSTONE for the prelude/resume batch (3c) — LABEL SPLIT:**
  endOfSend is ALSO the merge point for inline-spec fallthroughs
  (getter/setter/prims), which pop their own args and don't carry x1
  — the V2 resume continuation (static arg-pop + stur x1) MUST live
  at a NEW label `resumeAfterCall` placed JUST BEFORE endOfSend and
  falling into it.  Then: (i) the push's `adr x14, endOfSend` becomes
  `adr x14, resumeAfterCall`; (ii) bcToCode[postSendOff] must map to
  resumeAfterCall (NOT the next bytecode's label) so the C++
  JIT_RESUME_CALL/trampoline landers — which now all provide x1 —
  take the pop+write path too — **CORRECTION (flip-saving, found
  implementing 3c): do NOT re-bind bcLabels[postSendOff] at
  resumeAfterCall — forward JUMPS targeting the post-send bytecode
  use that label and would land on the continuation and double-pop.
  Collect vector<pair<bcOff, Label>> resumeOverrides during the emit
  and apply when filling bcToCode after assembly: jumps keep the
  plain label, only resume machinery gets the continuation offset**;
  (iii) the V2 prelude tail is:
  emitStoreSp(x5)=caller sp -> and x8,#addrMask -> str wzr,OFF_EXIT ->
  br x8 (no sendArgCount load, no jitMethod restores); (iv) xmethod
  sites additionally re-establish jitMethod/method/literals/argCount
  at resumeAfterCall from emit-time immediates (skip for self-rec).  **STRAGGLER ENUMERATION
  DONE (V2=1 local build): exactly 17 error lines, ALL in
  Interpreter.cpp** — 19323/19325 (early J2J region), 19753-19782
  (the rj2j C++ push site: needs the V2 packed write), 19937-19950
  (null-resume save.ip readers: V2 derives ip = jm->bcStart()+bcOff()),
  21882-21935 (materializeJ2JSaveIntoFrame: V2 unpack — jm via
  findMethodByPC(addr()), ip via bcStart+bcOff(), matRetSlot via
  nArgs()).  JITRuntime.cpp and AsmjitT1.cpp compile clean at V2=1
  (the emit writes raw offsets, converted in batch 3).  Fix these 17,
  then batch 3 = the emit sides.  **COMPLETION STRATEGY for 2b-6: make the
  struct dual FIRST, then build with the switch flipped LOCALLY — the
  compiler enumerates every remaining V1-field reference as an error;
  write each site's V2 side (push packing: resumeAddr | bcOff<<48 |
  nArgs<<60; materialize unpack; MAT_LOG; jit_t2_send pushes in
  JITRuntime.cpp; the 22xxx C++ push; null-resume save.ip readers),
  flip back to 0, commit the fully-gated code green, repeat until a
  V2=1 build compiles — then the emit batch, trampoline JSV_* paths
  (already layout-driven via the shared header), checker, and flip.**  CONTRACT DETAIL for batch 1+ (discovered sizing the
  C++ pops): under V2 the RESUME SITE does the arg-pop and expects
  the RETVAL IN x1 — so the C++ chain-loop pops that JIT_CALL a
  resumeAddr need a JIT_RESUME_CALL macro variant that passes
  x1 = retval live-in (the plain JIT_CALL only pre-loads
  x19/x20/x25).  The trampoline's Ltramp_return already has retval
  in x17/x1-adjacent flow — re-check its hand-off.  Batch order:
  (1) struct V2 variant + C++ pops + JIT_RESUME_CALL, (2) materialize
  + GC walks (resumeAddr->jm zone lookup), (3) emit push/prelude/
  resume-site continuations, (4) trampoline, (5) checker, (6) flip.  BUT instruction-count says the BIGGER
  per-send cost is the J2J save push (~12 instr) + return prelude
  (~14): the slot-reservation saveless design (reserve the pool slot
  with a cursor bump at call, fill retroactively on bail — fixes the
  ordering problem that blocks with-send callees) is the main
  remaining send-path lever.
  (d) the remaining per-send sequence: with sp resident the next
  costs are the save push/pop (slot-reservation design for
  with-sends saveless) and the dispatch chain;
  (e) x86 Phase 2 (mirror the x25 sweep with r13/rbx).

## CHECKPOINT 2026-06-10 EOD — correctness floor reached; gap = 5x

- **Suite: 2-3/8459 (~99.97%), ZERO stable fails** (only known
  environmental flickers: WeakAnnouncer/WeakIdentityKey GC-flakes,
  FIFOQueue load-flake, SHA256 64MB-zone timeout).  All-time best;
  the historical 12-15 stable-fail floor is GONE (swallowed-block-NLR
  fix).
- **PHARO_T1_XMETHOD_MAX_IC default = 1** (lever c flipped): callees
  WITH sends now inline-J2J by default.  60-rep det catch loop clean.
- **Head-to-head, build-opt vs stock Cog (same image, same benches):**
      cfib(30)  leaf-callee     42 ms vs  8 ms   5.3x
      sfib(30)  send-callee     54 ms vs 12 ms   4.5x
      benchFib(28) x10         128 ms vs 25 ms   5.1x
  Was 43x on cross-method sends at session start; with-send callees
  were ~60x.  The remaining ~5x is UNIFORM per-call overhead, not a
  pathology: next levers = register-resident state.sp (trampoline
  re-homing audit DONE, x25/x26 -> frame slots, x27 recompute, then
  JIT_CALL clobber update), per-method emit bloat (~6KB/method ->
  zone exhaustion: SHA timeout), then the full-2045-class suite
  survivability run on build-opt.
- **Saveless-extension ceiling experiment (PHARO_T1_SAVELESS_FORCE_SEL,
  knob kept):** forcing the saveless path for fib-shaped callees
  (sends + cond-jumps) CRASHES immediately — the recovery stub's
  retro-append is out of order once the callee pushes its own saves,
  exactly as the ordering analysis predicted.  Extending saveless to
  with-send callees REQUIRES the slot-reservation design (bump cursor
  at call, fill on bail).  BUT: the leaf A/B (423 vs 447 ms) already
  showed save TRAFFIC is only ~5% — the 5x is the per-bytecode
  state-memory round-trips (every push/pop = ldr+str of OFF_SP plus
  the operand store).  REGISTER-RESIDENT state.sp is confirmed as THE
  structural lever.
  **Phase 0 DONE (62b4461f):** x25/x26 freed from the trampoline
  pinned set (counters -> frame slots [sp,#120]/[sp,#128], frame
  144B); validated (bench unchanged, counters flow, 5/5 det reps).
  **Phase 1 (NEXT SESSION, fresh context required):** x25 := state.sp
  mirror.  Contract changes: (a) trampoline loads x25 = [x21+JS_SP]
  before EVERY blr into JIT code and treats it live-out (stores back
  on each return-to-loop); (b) JIT_CALL macro pre-loads x25 like
  x19/x20 and adds it to the clobber list; (c) T1 emit: every
  `ldr xN, [x0, OFF_SP]` -> use x25; every `str xN, [x0, OFF_SP]` ->
  `mov x25, xN` AND keep the memory store ONLY at exit boundaries
  (every bail/ret site must `str x25, [x0, OFF_SP]` first — sweep all
  `EXIT_*` emit sites, ~50); (d) C++ helpers called via BLR from JIT
  code that READ/WRITE state.sp (jit_rt_*) need sync around the BLR.
  VALIDATE each sub-step with: eval smoke, benchFib, PHARO_SP_DEPTH_
  CHECK healthy run (0 mismatches), DictionaryTest single-class,
  200-class suite.  Phase 2: mirror on x86 (rsi/r13 candidate).
  **SWEEP STATUS (batches 1-2 COMMITTED GREEN at switch=0):** all
  arm64 OFF_SP accesses in the T1 emit now flow through
  emitLoadSp/emitStoreSp (84 sed-batch + emitPushReg + 2 ldp pairs);
  emitSyncSpToState (no-op at 0) sits before all 47 arm64 rets.
  Validated per batch: eval, benchFib, sp-depth 1.82M checks clean.
  **REMAINING BEFORE FLIPPING PHARO_T1_SP_IN_X25=1:**
  (1) BLR-site audit — DONE (inventory below); the mechanical
      insertion of emitSyncSpToState (before) / emitReloadSpFromState
      (after x0-restore) remains.  All 24 emit BLR sites, classified:
      MUST sync-before + RELOAD-after (sp can CHANGE):
        5467 jit_rt_t1_sista_dispatch (runs a whole splice),
        5920/5954 jit_rt_basic_new_with_arg/basic_new (alloc ->
        scavenge -> prepareForGC reads AND now rebuilds sp),
        2383/2503/2626 jit_rt_primsize/primat/primatput_ptr (pop/push
        through state.sp).
      Sync-before only (READ sp, never change):
        4107 inline_block_value_prep, 5372 check_setter_bounds,
        5433 setter_write_barrier, 6620 sync_globals.
      Diagnostics (knob-gated, off by default — convert for hygiene
      or assert knob+flip incompatible):
        277 shadow_verify, 313/5404 store_ring, 2849 atrec_entry,
        5304 atrec_getter, 5332 verify_getter, 3612 trace_mod,
        4387 xmethod_log, 4783 bail_gate_log, 4821 log_selfrec_push,
        6191 verify_inline_at, 6437 trace_idh, 4593 (unidentified —
        read before flip).
      emitReloadSpFromState helper is COMMITTED (no-op at 0).
  (2) **TIER-INTERACTION CONSTRAINT (discovered in the sweep): x25
      residency is an asmjit-T1-only contract.**  Stencil-compiled
      methods + Sista splices/lowered code keep sp in memory.  br
      transitions between T1 code and foreign JIT code MUST sync:
      bit-60 direct calls into a STENCIL-compiled JITMethod (both
      claim tier 1 — check whether the stencil compiler still
      produces methods at all on this branch), and Sista kSendInline
      BRs.  Either sync at those brs or verify they cannot occur.
  (3) Enable the three marked writebacks (TrampolineAsm.S x2 +
      osr_resume) and the JIT_CALL post-blr store.
  (4) Validation ladder per WIP; then measure fib/cfib vs Cog 8/25ms.
  **Phase-1 design notes (worked out 2026-06-10 EOD — saves the next
  session the re-derivation):**
  - AAPCS is ALREADY handled: the trampoline saves/restores x25 at
    [sp,#48] (kept in Phase 0), and JIT_CALL declaring x25 in its
    clobber list makes the C++ compiler save the interpreter's x25
    around the asm.  No JIT-side stash slot needed.
  - Trampoline sync points: load `x25 = [x21+JS_SP]` immediately
    BEFORE each blr into JIT code; store `x25 -> [x21+JS_SP]`
    immediately AFTER each blr returns to the loop (the loop's own
    paths read JS_SP from memory and must see the callee's final sp).
    Grep TrampolineAsm.S for `blr` — a handful of sites.
  - Inline-J2J br chains stay coherent for free ONCE THE EMIT IS
    FULLY MIGRATED: caller and callee share x25 across the br/return
    like a real machine register — that IS the win (no OFF_SP
    traffic at call boundaries at all).
  - **MIGRATION TRAP: the emit canNOT migrate bytecode-by-bytecode.**
    An unmigrated emit writes OFF_SP only -> x25 goes stale for the
    next migrated emit.  The sweep must convert ALL OFF_SP reads/
    writes in emitOne_arm64 + the send-site emit + preludes in ONE
    change, with `str x25, [x0, OFF_SP]` added at every exit site
    (every `EXIT_*` store + ret — grep OFF_EXIT for the full list)
    and around every BLR to a C++ helper that touches state.sp
    (jit_rt_* with sp side effects: block_value_prep, sista helpers).
    Do it on a branch with the dev build; the sp-depth instrument
    catches any missed site within one healthy startup (765K checks).
Branch: `jit`. Build: `cmake --build build-opt` (optimized; the plain `build/` is -O0).
Test VM: `./build-opt/test_load_image /tmp/harness/Pharo.image eval "<expr>"`.
Stock Cog baseline: `cd /tmp/harness && ./pharo Pharo.image eval "<expr>"`.

## NEWEST (2026-06-10 PM): sp-depth instrument -> staticJ2JArgCount fold bug FIXED

- **PHARO_SP_DEPTH_CHECK** (BcDepthMap.{hpp,cpp}, knob in debug_vars.h):
  per-bcOffset static operand-depth map (worklist walk, side table per
  JITMethod) verified against `state.sp` at every checkable JIT exit
  (Send/SendCached/MustBool/ArithOverflow/Block/ArrayCreate/Yield; 8 call
  sites incl. both chain loops).  754K checks / 0 false positives healthy.
  KEY FACT it documented: **sp convention is one-past-TOS everywhere**
  (stackTop() = sp[-1]; the JITState.hpp "points to TOS" comment was stale).
- **Found in its FIRST failing-config run**: [SP-DEPTH] delta == nArgs at
  handle:offset: -> the J2J return prelude's staticJ2JArgCount fold
  (AsmjitT1 ~7318) is UNSOUND cross-method: the CALLEE's prelude pops the
  CALLER's save but folds the sp-adjust from its OWN send sites
  (initializeHandle:offset:'s only send is 0-arg `self initialize` ->
  sp-adjust folded to 0 while popping a 2-arg save -> sp high by nArgs,
  retval in an arg slot).  This was the lever-(c) MAX_IC=1 corruptor the
  selector bisect named.  FIX: force dynamic load-from-save when xmethod
  or inline-block-value is enabled (every push site writes
  save.sendArgCount correctly, so dynamic is always sound).
- **Validation**: MAX_IC=1 repro 0/4 -> ~85-90% pass; benchFib unchanged
  (10x fib28 123ms); 200-class suite vs abdet_default: **12 stable
  FAIL->PASS** (the entire *DictionaryTest>>testIncludes family!) and 0
  stable regressions (3 flickers = FIFOQueue heavyContention2,
  ProcessTerminate nestedUnwindS1, WeakIdentityKey clearing/includes —
  all re-run flaky).  Default config was previously safe only by the
  leaf-only accident (MAX_IC=0 callees have no sends -> fold already -1).
- **RESIDUAL (~8-20% by layout): MAX_IC=1 still loses the eval** with a
  nextPutAll: DNU in OpalCompiler>>evaluate (catch_FAIL.log).  Caught a
  failing run WITH the detector on: **mismatches=0 in 767K checks** ->
  NOT an sp-depth desync at checkable exits.  Value corruption (wrong
  receiver), or desync confined to unmappable methods (~10%) /
  uncheckable exits (ExitReturn/J2JCall).  Per-run variance under
  DET_SCHED = ASLR slide is part of the knife-edge (lldb's no-ASLR
  hides it).
- **RESIDUAL DIAGNOSED + FIX BUILT (catch2 anatomy -> stale
  bcStartCache):** the caught DNU shows `#,` sent to a StdioStream — the
  receiver one slot below (the 'EVAL-RESULT=' string) was REPLACED by a
  wrong VALUE at CONSISTENT depth, with adjacent duplicated slots and
  scavengesSoFar=3.  Wrong-values-at-right-depth = a materialized frame
  resumed at a WRONG-BUT-IN-RANGE bytecode offset: JITMethod::
  bcStartCache (used by the xmethod inline-J2J emit for state.ip,
  AsmjitT1 4392/4873) is set once at compile from compiledMethodOop and
  NEVER refreshed — a scavenge moving the CompiledMethod (fresh eden
  DoIt / OpalCompiler infra) leaves it at the old address; sp-depth
  validates ip-vs-sp CONSISTENCY so it stays clean at a wrong ip.
  FIX: refresh bcStartCache in forEachRoot's JIT-zone walk (zone already
  writable there; idempotent across mark/update phases; covers scavenge
  + fullGC).  This was also "latent suspect #2" from the 3-agent map —
  yesterday's lldb only ruled it out at ONE fire.  Validation: catch3
  40-rep loop on the fixed binary (in flight), then 200-class suites
  default + MAX_IC=1.
- **Lever (c) PRIZE CONFIRMED on the fold-fixed binary**: incs-shape
  bench (callee with a send: `sfib ... incs`, `incs ^self incc`)
  default 370-378ms vs MAX_IC=1 **51-52ms (7.3x)** — raising MAX_IC
  is the next big cross-method win once the residual is confirmed dead.
- **Interp-side sp-depth extension** (same commit family): the residual
  DNU fires at a `,` that dispatches via **ExtSend 0xEA** (the eval DoIt
  has >15 literals) — op_send0/1/2 hooks alone NEVER see it.  Hooks now:
  threaded op_send0/1/2 + the ExtSend switch case.  Known noise floor:
  6 FFI/stdio sites report stable +1 (special activation frame variant,
  deduped).  Internal resends (adaptTo*, value-retry) and primitive
  methods are skipped (argCount-vs-site filter + prim-bit filter).
  Catch-loop protocol: /tmp/catch_fail5.sh — rep until no EVAL-RESULT,
  keep the log, grep SP-DEPTH-INTERP minus the noise selectors.
- **ROOT CAUSE FOUND (catch loop iterations 4-9): SCAVENGE NEVER CALLED
  prepareForGC/afterGC.**  Only fullGC wraps object motion in the
  ip->offset round-trip.  A scavenge moves young CompiledMethods —
  including the one the interpreter is CURRENTLY EXECUTING (fresh eval
  DoIt / freshly-compiled methods): forEachRoot updates method_ oops,
  but instructionPointer_/savedIP/state.ip keep pointing at the OLD
  eden copy.  Execution continues on the stale copy — CORRECT until
  eden refills and overwrites those bytes, then the interp executes
  the new objects' raw bytes (string-as-method, `,`-to-StdioStream,
  silent early return — every observed mode).  Explains the ~10%
  rate (race: DoIt completion vs eden refill), the scavengesSoFar=3
  signature, the ASLR knife-edge, and the detectors' silence (stale
  ip is out-of-range vs the MOVED method -> interp check skipped it
  silently).  FIX: scavenge() now wraps in prepareForGC/afterGC(false)
  (skips the full-GC-only methodCache/IC flush tail; gcPrepared_
  guards the fullGC-internal scavenge from double-prepare).
  En-route fixes that were ALSO real bugs: stale bcStartCache (commit
  earlier), megaCacheAdd tenure guard (young oops in a non-root,
  stencil-probed cache = same hazard class, distinct path), walker
  ExtJump semantics (operand is UNSIGNED + extB sign — T1's emit reads
  int8: LATENT T1 MISCOMPILE for naked forward long-jumps >= 128,
  AsmjitT1.cpp ~6965/1221/846 — STILL UNFIXED, next task).
  Validation in flight: catch10 60-rep loop.  Scavenge-wrap perf cost
  TBD (prepareForGC is O(frameDepth) per scavenge; if measurable, only
  wrap when any executing/saved method is YOUNG — usually none are).
- **catch10/11 (post scavenge-wrap): STILL FAILS (~5%).**  [DNU-METHODREG]
  probe (knob-gated, in sendDoesNotUnderstand) corrected an earlier
  misread: method_ at DNU time IS valid heap — an EDEN CompiledBlock
  (the DoIt's on:Error block; selectorOf prints `,` for blocks —
  quirk), ip correctly inside it, bytecode window sane (`...9b 96 >d8
  53 87 d8 88 5e`).  The failure shape from the window: `Stdio stderr`
  (send0) RETURNED THE WRONG OBJECT (the 'EVAL-RESULT=' string) ->
  cascade dup'd it -> nextPutAll: DNU on ByteString.  Earlier variants
  (`,` to StdioStream) = the same class with the wrong object appearing
  one send earlier.  VALUES wrong at CORRECT depth at every check.
  **DICTIONARY BUG KILLED — SWALLOWED NLR IN T1-COMPILED BLOCKS — AND
  THE SUITE IS AT AN ALL-TIME BEST: 3/8459 (all three known
  environmental: FIFOQueue contention load-flake, SHA256 64MB-zone
  timeout, WeakAnnouncer GC-flake).  ZERO stable correctness fails.**
  Root cause: the T1 emit treated 0x58-0x5C inside CompiledBlocks as
  plain EXIT_RETURNs — `^x` in a hot block became a block-local
  return.  Dictionary>>includes:'s do:-block fell through to ^false
  once compiled.  Repro chain that cracked it: suite-pair diff ->
  single-class (fails with NO knobs; batch passed = context) ->
  NO_JIT passes -> STUB_ONLY passes -> skip-all-blocks passes ->
  block-pair bisect -> eval 2-test repro (test0CopyTest primes,
  testIncludes fails) -> stepwise assert replication (b=false with
  anySatisfy true!) -> includes: source = `do: [... ^true]` -> the
  0x5C emit.  Fix: block compiles bail 0x58-0x5C to interp like
  0x5D/0x5E (both arms); jm->isBlock also fixed (was hardcoded
  false).  The earlier "sortBlock pair" attribution was an artifact
  of attempt-index landscapes — the mechanism was always the
  includes:-block itself going hot.
  incs prize re-confirmed on this binary: 491 -> 57ms at MAX_IC=1.
  IN FLIGHT: MAX_IC=1 200-class suite — if clean, the lever-(c)
  default flip is UNGATED.
  **DICTIONARY REPRO CHAIN (supersedes the MAX_IC attribution!):**
  single-class DictionaryTest>>testIncludes FAILS with NO KNOBS AT ALL
  (the suite-pair MAX_IC attribution was run-context coincidence: batch
  200-class PASSES, single-class FAILS, same binary same env).
  PHARO_NO_JIT -> PASS.  Bisect panel: NO_INLINE_J2J / NO_INLINE_GETTER
  / NO_J2J / NO_TIER2 / NO_SISTA all FAIL; **PHARO_ASMJIT_T1_STUB_ONLY
  -> PASS = the bug is in T1's REAL codegen**, independent of the J2J/
  IC superstructure.  Compile-cap bisect: PASS<=3242 FAIL>=3246, but
  the window selectors (slot-reflection family: write:to:,
  instVarNamed:put:, slotNamed:ifFound:ifNone:,
  instanceVariablesToKeep) don't cure individually with uncapped JIT —
  cap-boundary not identity-stable (known trap, rediscovered).
  Halving over 1759 method selectors SANITY-FAILED (skip-all doesn't
  cure) while STUB_ONLY cures -> the corruptor is a **CompiledBlock**
  (blocks compile selector-less, unskippable by selector list).
  **BLOCK PAIR PINNED (attempt-indexed bisects via BLOCKS_SKIP_FROM/TO
  + BLOCKS_FIRST_N; note: rejected blocks RETRY, so single-attempt
  skips defer rather than prevent — use suffix/range bisects):**
  - attempt #387 oop=0x3003101a0 bcLen=43: a 2-temp COMPARISON-CHAIN
    block (and:/or: cond-jump diamonds):
    `40 80 c2 41 80 b0 4e c1 4d b6 40 81 c2 41 81 b0 ...`
  - attempt #193 oop=0x3004320a8 bcLen=4: the canonical comparator
    `[:a :b | a <lit0>: b]` = `40 41 90 5e` (sortBlock family!).
  INTERACTION bug: ONLY-387 passes, FIRST_N=387 fails, partner
  boundary sharp at #193.  PHARO_NO_BLOCK_BIT does NOT cure.
  **HOME METHODS RESOLVED (T1-BLOCK trace now prints home=):**
  - 0x3004320a8 (#193) = the comparator block in **OCParser>>
    addCommentsTo:** (`[:a :b | a <lit0>: b]`);
  - 0x3003101a0 (bisect-#387; #386 on the new binary — attempt
    indices shift per binary, OOPS are old-space-stable, grep by oop)
    = the 43-byte type-tolerant comparison chain in
    **Dictionary>>keysSortedSafely** (the classic sortBlock).
  BOTH ARE SORTBLOCKS -> this is the ORIGINAL sortStructs/mergeSort
  corruption family (mergeFirst `by` temp, SettingTree sortBlock DNU)
  with a 30s deterministic repro and both players named.  MECHANISM
  HYPOTHESIS to check first: a block-value dispatch path that caches
  per-CLASS (FullBlockClosure classIndex is the SAME for every block!)
  and replays block A's compiled code for closure B when two compiled
  sortBlocks flow through one value:value: send site (mergeSort's).
  Audit: the value:/value:value: IC route (BLOCK_VALUE bit 59
  consumption in the chain loop + stencils + jit_rt_inline_block_value
  _prep) — verify every path re-derives the CompiledBlock FROM THE
  RECEIVER CLOSURE at runtime rather than from cached bits.
  (PHARO_NO_BLOCK_BIT alone does NOT cure — the consuming path may be
  elsewhere, e.g. OSR/tryJITActivation on CompiledBlock or the
  primitiveFullClosureValue JIT fast path.)
  RULED OUT by inspection/experiment: value-family J2J + compile skips
  (both FAIL), BV-prep helper (re-derives per-closure, default-off),
  MethodMap collisions (lookup validates value->compiledMethodOop),
  jump-to-end-sentinel (bcLabels.size()==bcLen, OOR jumps bail).
  PAIR REQUIREMENT BIDIRECTIONAL: ONLY-193 passes, ONLY-387 passes,
  both FAIL.  TEST SEMANTICS: testIncludes asserts
  `newDict includes: o2` with o1=2@3, o2=2@3 (equal non-identical
  Points; o1 stored, o2 probed) — Point equality evaluated inside
  Dictionary includes:'s iteration block.  NEXT SESSION: instrument
  the assert (run `2@3 = 2@3`-via-includes: minimal eval with both
  blocks warmed), or per-block execute-counters to see WHICH of the
  two named blocks runs during testIncludes (neither obviously
  belongs to includes:! — if NEITHER runs during the test, the
  corruption happened EARLIER, during fixture/compile time, and
  persisted in a global), then lldb the first wrong comparison.  Repro: printf 'DictionaryTest\n' >
  /tmp/sunit_class_names.txt; run build/test_load_image Pharo.image;
  grep testIncludes /tmp/sunit_test_detail.txt (30 s, deterministic,
  no env knobs needed; JIT-off/STUB_ONLY/skip-all-blocks all PASS).
  **MAX_IC FLIP GATE (suite pair on the final binary): MAX_IC=1
  RE-BREAKS the *DictionaryTest>>testIncludes family** (the exact 12
  tests the fold fix cured in default config; default run PASSES them,
  MAX_IC=1 run FAILS them, 26 diff lines total = just this family +
  flakies).  Eval-level (catch25 80-rep) is CLEAN — this is a
  DETERMINISTIC suite-level repro: stage `DictionaryTest` in
  /tmp/sunit_class_names.txt, run with PHARO_T1_XMETHOD_MAX_IC=1 +
  detectors (SP_DEPTH_CHECK, VERIFY_GETTER) — same bisect protocol,
  far easier than the eval Heisenbug.  Suspect shape: another
  cross-method assumption (same class as the fold bug) in a path that
  with-send-callee admission newly exercises (includes: -> hashed
  collection scan loops).  The MAX_IC default flip is BLOCKED until
  this family is clean; the 7.3x incs prize stays opt-in meanwhile.
  **RESIDUAL KILLED (catch22-25, controlled):** the corruptor is the
  DISPATCH-A-SIDE entry into the shared tryGetter label (AsmjitT1
  ~3880).  Bisect: disabling only that tbnz -> 30/30 clean; same-length
  dummy env var -> fail rep 1 (real, not layout).  x2/x1 contract
  reload REDUCED (fail rep 31) but didn't eliminate -> the dispatch-A
  bail paths leave MORE than registers inconsistent (suspect: partial
  state commits, e.g. state.ip = callee bcStart at the J2J callee-setup
  ~4392 before a late bail).  FIX SHIPPED: that entry is DEFAULT OFF
  (opt-in PHARO_T1_GETTER_IN_J2J); getter sends on that path take
  dispatchCached (they were already on a bail path).  VALIDATION:
  80-rep catch25 on the full failing config -> ZERO failures.
  PROPER ROOT-CAUSE (later): audit dispatch-A bail paths for partial
  state commits; verify-getter v1/v2/v3 (all clean) prove the IC
  entries were never poisoned — it was always execution-state, which is
  why every entry-validity instrument stayed silent.
  catch19 (verify-on-fire v1, bounds check only): CAUGHT rep 2 with
  ZERO [VERIFY-GETTER] flags — the poisoned slotIdx is IN-BOUNDS
  (wrong-but-small index, e.g. ivar 2 instead of 0 -> wrong object,
  no bounds trip).  v2 NEEDED: full re-derivation in the helper —
  expected slotIdx from the IC entry's METHOD slot (entry base reg:
  see the probe emit at AsmjitT1 ~5011-5088 where x7=extra is loaded;
  pass the entry pointer as arg4) -> primitiveIndexOf(methodBits)-264
  vs extra&0xFFFF; ALSO compare entry key vs recv classIndex.  Any
  mismatch = the poisoned entry with full provenance.
  catch16/17/18 (ExtJump fix, pendingICPatch GC-clear, icBuffer
  ownership guard — all real bugs, all committed): residual STILL
  reproduces (~rep 6-16).  GETTER CLASSIFICATION MACHINERY now fully
  mapped (upgradeICToJ2J ~21436): bit63|slotIdx derives from
  cachedMethod's QUICK-PRIM index (264+N), keyed by receiver =
  stackPointer_[-(sendArgCount+1)].  TWO REMAINING CANDIDATES:
  (a) stale cachedMethod oop at classification time (wrong method's
  header read -> wrong slotIdx, valid-looking entry);
  (b) stale state.sendArgCount at the upgrade call (Interpreter.cpp
  20035) -> WRONG RECEIVER fetched -> wrong class lookupKey -> getter
  classification poisoned onto an unrelated class.
  NEXT INSTRUMENT (decisive): verify-on-fire — knob-gated BLR in the
  T1 inline-getter emit calling a C++ helper that re-derives (recv
  class -> selector via selBitsArray -> lookup -> quick-prim classify)
  and compares slotIdx + re-read value; mismatch prints the IC entry +
  both classifications.  IMPLEMENTATION SHORTCUT: the arm64 getter emit
  (AsmjitT1 ~5135-5172) ALREADY has the exact BLR scaffolding — the
  FINDNODE_WATCH g_emitGetterTrace block (sp-48 save of
  x0/x1/x2/x6/x7/x30, BLR jit_rt_atrec_getter(state, recv, val,
  bcOffsetFromMethObj), restore).  Clone it under a new knob
  (PHARO_VERIFY_GETTER), pass x7 (the extra word, already saved at
  sp+32) so the helper has slotIdx; helper mirrors
  jit_rt_atrec_getter's plumbing to find the site selector from
  state.jitMethod's selBitsArray.
  catch15 (methodMap-rebuild-per-scavenge fix in): CAUGHT at rep 25 —
  the patchJITICAfterSend guard hole was REAL (fix committed, sound)
  but the getter poisoning has ANOTHER path.  NEXT hygiene candidates:
  (a) clear pendingICPatch_ in prepareForGC (any pending patch carries
  pre-GC assumptions); (b) selector equality is NOT site identity —
  same-selector foreign sites still pass the guard (poison a getter
  classification across RECEIVER CLASSES with different ivar indices);
  (c) **T1 ExtJump MISCOMPILE CONFIRMED WIDER**: the pre-scan ACCEPTS
  ExtendB+ExtJump bundles (t1EnableJumps), but the emit at ~6965 reads
  ONLY the operand byte as int8, ignoring extB.  Quadrants: extB=-1 &
  operand>=128 (backward 1-128 bytes — common loop bodies) is correct
  BY COINCIDENCE; backward >128 bytes, naked forward >=128, and all
  extB=+1 forward jumps MISCOMPILE (branch to a wrong in-range label =
  wrong-position execution, locally depth-consistent — fits the
  residual!).  OPEN: check whether the prefix BUNDLE handler (~6719)
  intercepts ExtendB+ExtJump before the 6965 site (if so the naked
  >=128 case is the only live miscompile).  Fix all decode sites
  (computeLiveLength ~846, first-pass ~1221, emit ~6965) to interp
  semantics: offset = byte + (extB << 8), byte UNSIGNED.
  catch13/14 VERDICT (KNIFE-EDGE-CONTROLLED): **PHARO_T1_NO_INLINE_GETTER
  -> 30/30 PASS; same-length DUMMY var (PHARO_T1_NO_INLINE_GETTEX) ->
  FAILED at rep 8 — the bit-63 INLINE GETTER emit is the corruptor
  route, NOT layout luck.**  Next: root-cause the getter staleness —
  audit every IC extra-word writer (jit_rt_fill_ic rewrites extras
  fresh ✓; check patchJITICAfterSend / upgradeICToJ2J /
  applyICSpecialization for partial writes), and the getter emit's
  ivar-index/receiver assumptions under xmethod (MAX_IC=1).  Suspect
  family: recoverAfterGC's selective IC clear PRESERVES upper-16 extra
  flags (bit 63 + classification) while zeroing key/method — sound only
  if every refill path fully rewrites the extra word.
  catch12 VERDICT: **PHARO_NO_JIT -> 30/30 PASS (same env) — the
  residual is JIT-SIDE.**  A JIT-executed send (the `Stdio stderr`
  class-side accessor, xmethod-admitted at MAX_IC=1) returns the WRONG
  OBJECT with correct sp.  PRIME SUSPECTS (wrong value, right depth):
  the IC-hit INLINE specializations that produce a result WITHOUT
  calling — TRIVIAL_BITS (63/62/61/57), RETURNS_LITERAL (58), and the
  bit-63 inline getter (stats show getter=325K fires in failing runs;
  retLit=0).  These read CACHED values/slot addresses from IC extras —
  a stale cached value = exactly this symptom.  NEXT (catch13): bisect
  with the spec knobs on the failing config — PHARO_T1_NO_INLINE_GETTER
  and the trivial/retLit opt-outs (see debug_vars.h / DebugSettings) —
  one knob per 30-rep catch loop; the knob that cures it names the
  emit.  Then root-cause that emit's staleness (likely: IC extras not
  GC-visited for the cached-value bits, the SAME hazard class as the
  megaCache — young value cached in a non-root location).
  Catch logs: /tmp/catch11_FAIL.log.
  NOTE: all catch reps run in /tmp/harness on Pharo-base.image; evals
  share /tmp/harness/startup.st — NEVER run a second eval there while
  a catch loop is live (use /tmp/harness2).

## Headline: inline-J2J is DEFAULT-ON (lever e, commit after 0a48a0e1).
DEFAULT config now: cfib(30) 344ms -> 44ms (Cog 8), benchFib(30) 296ms -> 32ms
(Cog 5).  60-class SUnit A/B default-vs-J2J per-test identical (4130/4140;
two known-flaky tests flip one each way; the historical flip-blocker
CharacterTest>>testStoreStringAll PASSES).  Opt-out: PHARO_T1_NO_INLINE_J2J=1.
Confirmation COMPLETE: 1-60 re-run 4131/3 (one flaky better than the
pre-flip A/B); 61-200 (4319 tests) J2J-on 4297 vs J2J-off 4299 — only
per-test diffs are LIFOQueueTest>>testHeavyContention and
WeakAnnouncerTest>>testWeakDoubleAnnouncer, both PASS under J2J in
isolation on a quiet machine (load-flaky).  No reproducible regression
across 200 classes / 8459 tests.  The 61-200 SHA256Test TIMEOUT (both
configs) is the 64MB zone filling (~14.6K methods) -> late-hot methods
interpreted; PHARO_CODE_ZONE_MB=192 works around it until lever (d)
shrinks the ~6KB/method emit.  Next perf checkpoints vs Cog: re-run
the full classify-sunit Δcog comparison on build-opt, and watch
inline-J2J chain lengths vs preemption fairness (testHeavyContention
under load is the canary).

## DONE this session (committed)

1. **dd0fa6f2 — cross-method inline-J2J gate off-by-one.** The xmethod gate
   chain in AsmjitT1.cpp read JITMethod bytes 47/46 as isStubOnEntry/
   canBailMidMethod; real layout 46=hasNLR 47=canBailMidMethod 48=isStubOnEntry.
   Since t1NlrTailOnly (default ON) sets hasNLR for EVERY method containing a
   return opcode 0x58-0x5C, the byte-46 read rejected ~all real cross-method
   callees (574K/930K in cfib) — this WAS the entire cross-method send gap.
   The real isStubOnEntry was never checked (stubs slipped through = the old
   "xmethod corrupts state" reputation). All JM reads now use offsetof().
   Also: per-gate bail counters (`xmethod gates:` line in dumpJITStats, only
   emitted when PHARO_T1_INLINE_J2J is set), and the 30000 default fire cap
   (bisection leftover) lifted to unlimited.

2. **24175466 — stale state.j2jDepth corruption (xmethod at scale).** With
   876K+ fires/run, ~50% of startups corrupted (SettingTree mergeSort
   sortBlock -> #value:value: DNU). Root cause: materializeJ2J (chain loop,
   Interpreter.cpp ~22556) consumed pending saves into SavedFrames but never
   reset state.j2jDepth; the ExitBlockCreate/ExitArrayCreate handlers check
   `state.j2jDepth > 0` BEFORE enableJ2J resets it, so the
   `state.sp = stackPointer_` resync was skipped on consumed saves -> fresh
   closure above stale sp -> overwritten -> shifted operand passed as the
   sortBlock. Fix: materializeJ2J resets depth+cursor. 10/10 clean startups.
   Also hardened: prepareForGC/afterGC now round-trip live J2J pool saves'
   raw `ip` through J2JSave.ipOffset (was a real GC hole: forEachRoot visits
   only save.receiver); PHARO_J2J_MAT_LOG diagnostic knob.

3. **(uncommitted at write time) rj2j slice reservation** — the opt-in
   PHARO_RESUME_J2J chain loop's save slice was never reserved in
   j2jPoolCursor_, so GC receiver walk / ip round-trip / eviction pinning
   all missed those saves. Latent (rj2j default-off); fixed anyway.

## Key corrections to the previous WIP (premise was WRONG)

- "cfib->incc gets 0 bit-60 fills" was an ARTIFACT: PHARO_J2J_LOG_FILL caps
  at 4000 lines and startup burns them. The IC fill always worked
  (PHARO_DUMP_RECOMPILE_IC=cfib shows bit-60 + entryAddr on site 2).
- The slow path was the SEND-SITE DISPATCH: with the gates broken, every
  cross-method bit-60 send bailed to the trampoline activation
  (2.5M C++ activations/run @ ~340ns).
- incc DOES JIT-compile (T1-COMPILE right after cfib). `^self+1` is NOT
  classified by detectTrivialMethod (intArith needs a method ARG, 0x40).

## Current state / next levers toward Cog parity (~5x left on cfib)

- Validation in flight: SUnit batch 0-150 default-vs-INLINE_J2J
  (results /tmp/sunit_results_{default,j2j}.txt). MUST be equal-or-better
  before flipping PHARO_T1_INLINE_J2J default-on (lever (e), the big one —
  all of today's wins are behind that opt-in env).
- Residual xmethod bails (cfib run): bail_prim=105K (prim-bearing callees),
  bail_numic=237K (callees with sends — lever (c), needs nested-save
  correctness), bail_b47/stub+canBail ~17K. Each bail = slow trampoline.
  ALSO: any callee with conditional jumps has canBailMidMethod=1 -> b46
  bail (e.g. Number>>max: = 1.35M bails in the incs bench) — relaxing
  that is part of the same nested-correctness project.
- **Lever (c) status (PHARO_T1_XMETHOD_MAX_IC=N knob, commit 1238b606):**
  N>=1 (admit callees with sends) still corrupts after the j2jDepth fix
  AND after j2jEntryDepth hardening at 4 JIT_CALL sites.  DETERMINISTIC
  REPRO: `PHARO_T1_INLINE_J2J=1 PHARO_T1_XMETHOD_MAX_IC=1 PHARO_DET_SCHED=1`
  -> 4/4 runs lose the eval silently (clean exit, no DNU, no EVAL-RESULT;
  [STARTUP-ST-FIRED] never appears -> the corruption drops the startup
  action from SessionManager's startup list, no error raised);
  **BISECT CAVEAT (2026-06-10, supersedes the 'culprit pinned' claim):**
  the cap-bisect boundary is NOT call-identity-stable — capping changes
  inline-vs-trampoline charge counts downstream, so the boundary fire
  shifts with the binary (26705 = handle:offset:->initializeHandle:offset:
  on one build; 26683 = reset->resetTo: — a LEAF pair! — on the next).
  A leaf boundary proves the bisect finds a scheduling-sensitivity
  point, not the corruptor.  The corruptor is some 1-IC-callee fire
  (leaf-only is clean), identity unknown.  The watchpoint recipe still
  gives a deterministic stop + live registers at any chosen fire count,
  and `expr *(unsigned long long*)&g_xmethod_max = -1` at the stop
  un-caps live so the boundary call can be single-stepped.  RULED OUT at the fire (lldb raw-memory checks): tempCount setup (the
  xmethod path reads callee JM[35] dynamically; offsets 34/35 verified
  vs offsetof) and stale bcStartCache (JM[104] == compiledMethodOop+40,
  consistent -> CM had not moved by fire #26705).  NEXT: single-step
  the callee's FIRST mid-method C++ exit (its send sites are cold ->
  ExitSendCached) and its return path — the corruption mechanism lives
  in that exit/materialize/resume sequence, since leaf callees (which
  never exit mid-method) are immune.  Callee initializeHandle:offset:
  stores into receiver ivars (write-barrier path) — check the
  popStoreRecvVar interaction with a J2J-entered frame too.
  LOGGER FIXED (928df628): the XMETHOD_LOG wrapper now saves x0-x13+x30
  (was clobbering x1-x6 = receiver+args -> every logged call corrupted,
  the origin of the "xmethod corrupts state" lore).
  **SELECTOR-LEVEL BISECT DONE (2026-06-10, identity-stable):**
  PHARO_J2J_SKIP_SELECTORS halving over the 429 with-sends+filled
  callees converged on ONE selector: **initializeHandle:offset:**
  (ExternalData family: `self initialize. handle := aHandle.
  startOffset := aNumber - 1` — FIRST bytecode is a send -> immediate
  mid-method C++ exit on every cold call).  Skipping ONLY its fills
  cures the full failing config (ok + DNU=0).  Same selector the
  original fire-bisect named — that identification was genuine.
  **ROUTE NARROWED:** with the fixed XMETHOD_LOG, the failing run shows
  only 8 dispatch-A inline fires, NONE to this callee -> the corruptor
  route is the C++ J2J driver / stencil consumption of its bit-60 IC
  entry (ExitJ2JCall driver, Interpreter.cpp ~19302) — which has NO
  numICEntries gate and runs in the DEFAULT config too.  MAX_IC=1
  likely only shifts scheduling to expose it; the default config may
  carry the same latent bug.  NEXT: lift the 200-line PHARO_J2J_MAT_LOG
  cap (Interpreter.cpp materializeJ2JSaveIntoFrame) and/or conditional-
  break the ExitJ2JCall driver on calleeCM==initializeHandle's oop in
  the DET run; inspect the mid-callee-exit (its first `self initialize`
  send) -> materialize -> resume -> return sequence.
  GOTCHA: PHARO_J2J_ONLY_SELECTORS kills ALL IC fills for other
  selectors (0% IC hit rate) — runs under it DNU with ANY selector and
  are NOT a valid minimal repro.
  **EPISTEMOLOGY (2026-06-10, READ BEFORE TRUSTING ANY A/B ON THIS BUG):**
  the DNU-visibility is a LAYOUT KNIFE-EDGE: adding ANY env var (even
  AAAA=1, or byte-length-matched pads) makes the default+DET 7-DNU
  baseline go silent.  DebugSettings copies env strings at static init,
  so env content shifts allocation layout deterministically; the
  corrupt write lands in slack on most layouts.  CONSEQUENTLY:
  - POISONED (layout mirage): the heap-write-callee gate "cure"
    (PHARO_J2J_NO_HEAPWRITE_CALLEES — byte-matched pad cures equally),
    the skip-selector bisect convergence on initializeHandle:offset:
    (env length shrank monotonically with the candidate halving), the
    SCAV_DANGLE_CHECK "cure".
  - STILL VALID: the 7-DNU baseline itself (stable across reruns,
    exact env); cap=26682-pass vs 26683-fail (identical env length —
    ONE extra cross-method inline fire flips the severe outcome);
    NO_J2J_BRANCH keeping 7 DNUs DESPITE its env-length change (the
    corruption fires broadly; behavior knobs re-land it visibly).
  - The corruption is REAL and present in the shipping default config;
    wall-clock scheduling + layout luck hide it (200-class suites
    clean).
  - **KNIFE-EDGE EXTENDS TO BINARY DELTAS (2026-06-10 experiment
    series):** even the C helper's body size moves the visibility, so
    cross-binary DNU comparisons are NOISE.  Tested at the
    DET+NO_J2J_BRANCH 2-DNU deterministic repro: store-site nop sleds,
    register save/restore, empty calls, delay loops — DNUs persist
    within each binary; the one 0-DNU binary (full ring) is not
    attributable.  ONLY within-binary deterministic comparisons are
    valid evidence on this bug.
  - PHARO_T1_STORE_RING landed (knob-gated): store-provenance ring +
    DNU-time scan — useful once a config shows DNUs WITH the ring
    enabled (search configs; the scan prints planted-value provenance
    with selectors).
  - **SHADOW-SLOT INSTRUMENT BUILT (PHARO_SHADOW_SLOTS, committed
    2026-06-10):** ShadowSlots.{hpp,cpp} — 1M-entry (object,slot)->
    (value,writer) table; writers tracked: 3 JIT store emits,
    storePointer, storePointerUnchecked (covers setReceiverInstVar);
    readers verified: interpreter pushRecvVar (both paths) + JIT
    pushRecvVar/ExtPushRecvVar emits.  GC: forEachRoot visits entry
    oops, afterGC rehashes, becomeForward clears.  VALIDATED: ~28M
    stores / 3.45M checks / 0 mismatches / 0 false positives per
    startup across default+DET+MAX_IC=1+NOJB configs — writer coverage
    complete for exercised paths.  CAVEAT: those configs also showed
    no DNUs in the instrument's layout, so absence-of-mismatch is not
    yet absence-of-corruption.
  - **SUITE-SCALE VERDICT (200 classes): IVAR SLOTS EXONERATED.**
    974M tracked stores / 199M verified reads / exactly 4 mismatches —
    all the two-way-become family (prim 128 now clears the table; the
    only untracked writer found).  No ivar ever changed via an
    untracked path at scale.  (Suite 38 fails/6 timeouts vs 13/1
    uninstrumented = the ~4x instrumentation slowdown on
    timing-sensitive tests, not corruption.)
  - **REFRAME: the corruption class is STACK-FRAME SP-DESYNC, not heap
    stores.**  All victims in the original evidence were TEMPS
    (mergeFirst's `by` arg, OpalCompiler do: receiver), and both
    mechanisms actually root-caused (stale j2jDepth sp-resync skip;
    saveless return hijack) were sp desyncs that shift operand/temp
    slots.  NEXT INSTRUMENT: per-bcOffset expected-stack-depth table
    (computable at T1 compile from the bytecode stack effects) +
    verification of (state.sp - state.tempBase) at every JIT
    exit/resume boundary — catches the desync AT the boundary that
    produces it, with the method+bcOffset in hand.  The existing
    spAtLastJ2JCall/dispatchTraceLeak machinery in the chain loop is
    prior art for the check sites.
  - hasRecvFieldWrite now computed by T1 (commit 0bc7d7f9) — flag is
    real even though the gate experiment was inconclusive.
  Recipe (reusable): DET_SCHED makes the failure a deterministic
  function of the fire cap -> binary-search the cap (~20 runs), then
  watchpoint the cap-bail counter in the LAST PASSING run; x10=calleeJM
  x19/x11=callerJM x12=callerCM at the stop; resolve oops via
  PHARO_T1_TRACE_COMPILE log lines (lldb expression evaluator is
  useless under LTO).
  without DET_SCHED: intermittent startup DNUs (#do: in OpalCompiler
  evaluate / #key in WorkingSession startup), runs recover.  Control:
  DET_SCHED+leaf-only is clean.  Next session: lldb the deterministic
  repro; the send-bearing-callee bench is incs '^(self+1) max: 0' in a
  cfib loop (503ms today vs 41ms leaf — the lever-(c) prize).
- The xmethod fire path still bumps g_xmethod_count + cap-check per fire
  (2 ld + add + st + cmp); could be emitted only when a cap is set.
- J2J-s (stencil_sendJ2J out-of-line) handles 2.4M calls/run — dispatch-A
  inline-J2J only covers sites in recompiled (tier-2) callers? Verify
  coverage; tier-1-only callers may never get the inline emit.
- Saveless path (PHARO_T1_CAN_SKIP_J2J_SAVE), state after e6bb3809:
  - RETURN-HIJACK FIXED: the callee prelude tail-jumps past the
    post-blr restore whenever j2jDepth > j2jEntryDepth (callers
    mid-J2J-chain) -> sp-stash leaked 96B/call -> guard-page crash.
    Fix: pin entryDepth = depth across the blr (stash pad slot [40]).
  - Controlled single-site repro recipe: PHARO_T1_SAVELESS_MIN_COMPILE=N
    emits saveless only in compiles seq>=N (find the target caller's
    seq with PHARO_T1_TRACE_COMPILE; cfib lands ~#4699 in the bench
    eval).  cfib->incc via saveless: CORRECT (val=2692537).
  - PERF: as implemented it LOSES to the save-push (510 vs 412 ms 10x
    cfib) — the 96B stash + entryDepth pin cost more than they save.
    Diet plan: x25-x28 are callee-saved AND absent from JIT_CALL's
    clobber list (JIT code preserves them; trampoline relies on
    x23/x24 likewise) -> carry caller receiver/sp/tempBase/ip in
    x25-x28 across the blr, shrink the stash to {x30, entryDepth} +
    cross fields, and skip the OFF_RECEIVER/TEMPBASE/IP restores
    (write-back from regs).  Target: beat the ~23-op save-push.
  - **COMPLETE (a978a7f6)**: the at-scale bug was ExitArithOverflow
    bails — ANY SmI arith in a leaf can bail; canSkipJ2JSave only
    excluded cond-jump bailers (confirmed exit=6 via brk trap; the
    controlled cfib->incc site just never overflowed).  The non-return
    RECOVERY STUB retro-builds the elided pool save (post-send ip from
    callerCM + compile-time bcOffset; resumeAddr=endOfSend) and RETs
    with the callee's exit state -> C++ materializes as if save-push.
    Full scope, no bisect gates: 3/3 clean startups, 0 DNUs; 10x cfib
    420ms vs 440 save-push; benchFib + battery correct.
    REGISTER DIET VERDICT (3-agent audit, 2026-06-10): NOT VIABLE as a
    local change.  T1 emits use none of x22/x25-x27 (free there), but
    TrampolineAsm.S PINS ALL of x21-x28 across its BLRs into JIT code
    (x22=save base, x25=localCalls, x26=localReturns — LOAD-BEARING:
    checkCountdown_ -= (calls+returns)*10 — x27=overflow limit,
    x28=nilOop).  A diet register set inside a trampoline-entered
    caller corrupts the loop state it RETs back into; saving/restoring
    around the blr re-adds the eliminated memory ops (net zero).  A
    true win needs trampoline re-homing (counters -> frame slots,
    x27 -> recompute from x22+imm) freeing x25-x27 zone-wide + adding
    them to JIT_CALL clobbers — ~+4 mem ops per trampoline round-trip
    (NOT cold: all non-leaf J2J) for -6 per leaf call.  Marginal and
    risky; SUBSUMED by the register-resident state.sp project (the
    structural fix for the ~55-instr-vs-Cog-12 per-call gap).  Audit
    artifact: workflows task w5s5eg5y3 output.
    Default-on gate: full-suite A/B on the Graviton box (in flight at
    write time — default config 12674/12724 = 99.6%%; saveless run +
    per-test diff pending).  Bisect gates (MIN_COMPILE / MAX_ARGS /
    NO_EXTRAS) remain for future shape isolation.
- docs/jit-retrospective.md "Cog-speed MAP" numbers now stale for
  cross-method; tight-loop 2x-faster-than-Cog and self-rec 6x still hold.
- **First-compile fail thrash: FIXED (c79b97ab + 9dbe6a49).**  All
  failures were unsuppPrim (PHARO_JIT_FAIL_REASONS=1).  TWO paths:
  (a) initialCompileFailed_ negative cache was cleared every
  recoverAfterGC -> per-GC retries (now a GC-visited, rehashed 16K-key
  array like countMap_); (b) THE dominant one: upgradeICToJ2J's eager
  compile of prim-bearing callees runs per ExitSendCached send with no
  cache -> per-SEND asmjit pipeline re-runs.  Combined: 83.5K -> 694
  failed attempts per bare startup (120x); suite batches were burning
  ~700 failed compiles/sec.

## Workflow artifacts (multi-agent investigation maps, this session)

- /private/tmp/claude-501/-Users-wohl-src-iospharo/a6ed987d-559a-4f18-a43d-7bf2905adc05/tasks/wqrxey8av.output
  (send-path map: bit consumers, compile triggers, ExitSendCached flow, gates)
- .../tasks/wn7p9ib48.output (save-stack lifecycle: GC, recompile/eviction,
  protocol diff — contains the remaining suspect list w/ likelihoods+tests:
  matRetSlot-ignoring ExitReturn pops at ~22693 & jit_loop_exit, stale
  bcStartCache after GC, recoverAfterGC ghost-version methodMap rebuild)

## Rules reminders
- **SUnit runner invocations: rm TWO stale files first** or the run
  silently does nothing (ate three 90-min A/B attempts 2026-06-09/10):
  1. /tmp/sunit_run_completed.txt — runner's done-marker, ALSO touched
     by every test_load_image EVAL run (the A3 fix); blocks auto-start
     ("skipping auto-restart", one easy-to-miss line).
  2. <imagedir>/startup.st (e.g. /tmp/harness/startup.st) — eval mode
     writes it and non-eval runs EXECUTE the stale one -> evaluates the
     old expr and quits the image seconds after startup (0 SUnitRunner
     lines, "Test Complete" almost immediately).
  3. /tmp/sunit_batch.txt is 1-BASED (`1 60`, not `0 60`): index 0 makes
     the runner's copyFrom:to: raise, SessionManager swallows it, and the
     run is silent — the error appears ONLY in <imagedir>/PharoDebug.log
     (always check that file when a runner run is silent).
  4. The optimized build burns the 60G step budget in ~4-5 min; set
     PHARO_MAX_STEPS=2000000000000 for suite runs (knob added 0a48a0e1).
- EVAL-RESULT prints to stderr — capture with 2>&1.
- grep -o "ms=..." matches the arg echo first; grep EVAL-RESULT first.
- New env knobs -> src/vm/debug_vars.h (DEBUG_BOOL + GET_DEBUG_BOOL).
- JITMethod field reads in emit code: offsetof() ONLY, never raw ints.
