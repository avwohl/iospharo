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
