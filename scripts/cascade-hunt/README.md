# cascade-hunt — simulation-family corpse instrumentation (2026-07-05)

Full story: docs/deferred.md, ARM section, ProcessTest/simulation entry.

State of the backward chase (round 5):
- The [DNU] CASCADE corpse is the `lookupClass` value in the stepping
  machinery's send:to:with:super: — a stale young/eden pointer whose
  object died in a scavenge and whose memory now sits inside a fresh
  allocation (header reads nil-fill => classIdx 0).
- Same-run watchpoint pipeline WORKS (this dir):
  pharo_cascade_bp (Interpreter.cpp anchor, args corpse/sb/sp in x0-x2)
  -> cascade_watch.py arms write-watchpoints on the corpse's operand
  slots, filters for DEAD-POINTER DEPOSITS (written value's target
  header classIdx==0), prints bt + disassembly + registers, and
  auto-continues.  Run:
      EXPR=$(cat scripts/cascade-hunt/repro2.st)
      lldb --batch -s scripts/cascade-hunt/watch-driver2.lldb -- \
        ./build/test_load_image /tmp/harness/Pharo.image.bak eval "$EXPR"
  (repro2.st = parent/child form that survives cascade-death and
  refires; repro.st = single-shot form.)
- Round-5 finding: the caught JIT writes are temp<->stack COPIES
  (popStoreTemp/pushTemp shape; x4 = state.tempBase (JITState+0x18),
  the dead value already sits in the temp = the method's ARGUMENT).
  So the poison enters EARLIER: chase one hop upstream per run by
  arming the TEMP slot (tempBase+idx*8 from the DEADWRITE register
  snapshot) instead of the operand slots — repeat until the writer is
  a non-copy (the original stale-value producer or the location the
  scavenge failed to retarget).
- Everything else falsified so far (rounds 1-4): all emit knobs,
  become-fixup (fixed), temp-sync (knob), classTable sweep, prim-111
  ring, interp push() trap, write barriers (added anyway), remembered
  set (unused; scavenge full-scans old+perm), eden poison (corpse is a
  stale ADDRESS, not recycled content), OSR, Sista, parked-JITState
  chain (real bug, fixed, insufficient).

Round 6 (05:05): the auto-migrating chase's first [ORIGIN] was FALSE —
the writer is `popStoreTemp 4` in send:to:with:super: (x4 register =
state.tempBase; watched slot == tempBase+0x20 == temp 4 == the
lookupClass LOCAL — image-side identity now CONFIRMED).  The popped
source cell sits ABOVE the destination, which the downward-only
lower-cell scan misses: extend the migration to also consider the cell
at x25 (the pop source) and/or scan the whole live stack.  KEY
INFERENCE: lookupClass is dead THE MOMENT IT IS COMPUTED — prim 111
cannot return a dead object, so either the receiver temp (temp 1) was
already dead at frame entry (chase one frame up), or the value is
re-injected from a STALE CACHED MATERIALIZED CONTEXT: see
materializeFrameStack's ctxSynced short-circuit ("cached context means
this frame has NOT executed since suspension... skipping re-sync") —
if a cached ctx's temps hold a since-died pointer and fd=0 stepping
resumes THROUGH that context, the dead value re-enters execution.
NEXT PROBES: (a) chase with pop-source handling; (b) cheap C++ knob:
PHARO_MAT_FULL_RESYNC=1 already EXISTS (disables the ctxSynced
short-circuit) — run the repro with it FIRST, it is one env var.

Round 7 (05:10): PHARO_MAT_FULL_RESYNC=1 does NOT cure — ctxSynced
re-injection falsified.  Next hop (queued): HEAP-SIDE chase — at
cascade #1, scan old space [oldSpaceStart_, oldSpaceFree_) and live
eden for cells holding the corpse value (the simulated contexts'
stack slots live in the HEAP; Context>>stackValue: feeds rcvr/args
into the stepping machinery), then arm watchpoints on those heap
cells.  Old-space addresses persist across the parent/child repro's
restarts (no fullGC in the window), so the writer of the NEXT dead
deposit into a heap ctx slot gets caught with a backtrace.  The
python needs: read oldSpaceStart_/oldSpaceFree_ via the anchor args
(extend pharo_cascade_bp to pass them in x3/x4) — one-line C++ change.

Round 8 (05:25): PROBE HYGIENE LESSON — PHARO_T1_NO_CHAIN_RESUME_PLAIN
looked like a cure (exit 0, zero cascades) but was VACUOUS: under that
knob the VM ends the run early (~950-line log, no EVAL-RESULT, no
steps=, suite runner never starts).  The knob restores the
known-broken double-pop path; boot dies benignly.  EVERY knob probe
must check `steps=99999` (scenario truly completed), as the quarantine
probes did.  STANDING REAL FINDINGS: (a) quarantine at scavenge <=8 =>
genuine full pass; (b) per-run corpse addresses form a regular
~0x1A0F8 allocation stride = fresh per-iteration objects; (c) at
cascade time NO heap (old-space) cell and NO VM root holds the corpse
=> the value is not a retained pointer — favor the WRONG-STACK-SLOT
hypothesis: the JIT'd simulated send:super:numArgs: picks up a
per-iteration OBJECT (receiver/context) where the class (prim-111
result) belongs, and that object has died by read time.  Next: verify
by logging prim-111's ARG vs the corpse (extend the prim-111 ring
compare to check e.rcvr == corpse — if the corpse IS a recent prim-111
RECEIVER, wrong-slot is proven), then inspect the JIT resume path for
the send shape `objectClass:` (1-arg prim send) inside a JIT'd method
called from the chain loop.

Round 9 (05:30): PRIM111-RCVR-MATCH negative (32-deep ring) — corpse
was neither a recent prim-111 RESULT nor RECEIVER.  Queued probes:
(1) deepen g_prim111Ring to 256 and re-check both matches;
(2) at cascade, dump the LEVEL-1 simulated send's superFlag + the
    simulated method's last literal chain (is the corpse the
    methodClass association VALUE read via literalAt:? — the super
    branch bypasses prim 111 entirely);
(3) disassemble the JIT'd Context>>send:super:numArgs: around its
    objectClass:-send site (use methodMap lookup at cascade to get the
    JITMethod code address) and audit the operand-slot usage of the
    prim-call resume — the wrong-slot hypothesis remains the
    front-runner given the fresh-cohort stride + no-holder findings.

Round 10 (05:40): TEMP DUMP at cascade: t0=Symbol(selector),
t1=SmallInteger 44 (the SIMULATED receiver!), t2=args Array (SAME
allocation cohort as the corpse — corpse sits 0x578 BELOW it, i.e.
allocated just before: the per-iteration InstructionStream/context),
t3=false (superFlag — NOT the super branch), t4=corpse(lookupClass).
=> prim 111 ran on SmInt 44 and must return the PERM SmallInteger
class; temp4 instead holds last-iteration's dead per-iteration object.
CONVERGED HYPOTHESIS: the JIT'd send:to:with:super:'s POST-PRIMITIVE
RESUME reads the prim result from the WRONG STACK CELL (one off),
picking up the previous iteration's leftover object.  RING-T1 probe
added (what prim 111 returned for temp1).  Next: confirm via RING-T1,
then audit the executePrimitive-from-JIT result-placement/resume for
1-arg prim sends (prim 111 argCount=1) in chain-loop-called JIT code.

Round 11 (2026-07-05, FIXED — commit e40cd65b): RING-T1 was VACUOUS
(the ring records the prim RECEIVER; in the 1-arg MIRROR form
`Context>>objectClass: anObject` the receiver is the CONTEXT, and 44
is the ARGUMENT — the ring could never match 0x161).  The decisive
probe was an IC-SITE DUMP added to the cascade forensics: walk the
caller's JITMethod icBuffer (icSiteAt / selBitsArray) and print every
entry's key/method/extras.  Output:

    IC-DUMP caller=#send:to:with:super: sites=20
    IC site=6 #objectClass: e=0 key=36 m=#objectClass: x=0x18000000000000

extras = pk-24 (kPrimKindClass << 48) on a 1-ARG send site.  Three
conspiring defects:

1. CLASSIFIER (Interpreter.cpp upgradeICToJ2J + patchJITICAfterSend):
   `primIdx == 75 || primIdx == 111 -> pk` had NO ARITY CHECK.
   Prim 111's pk-24 inline is the 0-arg #class semantics; the 1-arg
   mirror form got the same classification.
2. DISPATCH (AsmjitT1 phase-1a): W3 IntArithReturn used a single-bit
   `tbnz bit52` at nArgs==1 sites — unsound, pk 16-31 all set bit 52
   (same B6-F1 unsoundness fixed earlier for W6/bit-51).  pk-24 =
   0b11000 was stolen as "W3 kind 0" = tagged ADD.
3. EMIT (W3/W2 bodies): the both-SmI check OR-combined the tags —
   (rcvr|arg)&7==1 ACCEPTS a (heap, SmI) pair.  So the stolen dispatch
   executed `contextOop + 0x161 - 1` = contextOop + 352: a young
   pointer just past the per-iteration simulated Context.  That
   explains every prior finding: fresh-cohort stride (it IS the
   per-iteration context + offset), no heap/root holders (it's a
   fabricated address, never a real reference), corpse 0x578 below the
   args array (allocation order), dead by read time (next scavenge).

FIXES (all three layers): inlinePrimKind(prim, methodNumArgs) returns
0 on arity mismatch at all 4 classify sites; W3 dispatch decodes the
full 5-bit extras field and range-checks 16..18; W3+W2 emits use
strict per-operand SmI tag checks.

VERIFIED (probe-hygiene rule satisfied): repro.st exit=0, 0 cascades,
steps=99999; repro2.st 4/4 done; stepping family (StepOverTest,
StepIntoTest, StepThroughTest, ContextTest, BlockClosureTest,
ProcessTest) 156 P / 0 F / 0 E (was ~50 cascade failures);
JIT-warm bitXor/arith/class identities hold.

Historical footnote: the round-1-4 "falsifications" of the getter /
setter / class-inline knobs were correct — the guilty inline was W3
IntArithReturn (PHARO_T1_NO_INLINE_INT_ARITH_RETURN), which was never
in the bisect set.  The scavenge<=8 quarantine "cure" worked by
keeping the per-iteration context alive (fewer scavenges -> the
fabricated pointer still pointed at live-ish memory longer), not by
fixing anything.
