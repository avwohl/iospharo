# WIP — scavenge "returns-receiver" corruption (resume after reboot)

Date: 2026-06-01. Branch: jit.

## TL;DR of where we are
Debugging the cumulative-state SUnit corruption. Narrowed it ALL the way to:
a method `^ expr` (specifically `CDNode>>classDefinitionNode` = `^ parent
classDefinitionNode`) **returns its receiver instead of the expression** when a
young-gen **scavenge** fires during its execution. This is the documented
"returns-receiver-after-GC" interp family.

Deterministic minimal repro (no suite needed):
- Image: `/tmp/cn.image` (rebuild via the prep below if gone).
- Run: `PHARO_NO_JIT=1 timeout 50 ./build/test_load_image /tmp/cn.image`
- Read `/tmp/cn_out.txt`: rounds 1-5 `cdNode==cd:true`, rounds 6-8
  `cdNode==cd:false` + `cdNode==sn:true` (i.e. `sn classDefinitionNode` returns
  `sn` instead of `cd`).
- `PHARO_YG_NO_SCAVENGE=1` makes it 5/5 correct (defers scavenge to fullGC time).

## Confirmed (committed in docs/vm-compat-bugs.md, commits 709089bb..648d2be3)
- Root cause is young-gen **scavenge** (PHARO_YG_NO_SCAVENGE eliminates it;
  PHARO_GC_HEADROOM_MB irrelevant → not full GC).
- NOT object/data corruption: AST identity stable across scavenge, identityHash
  stable (0/200), no identity split, `sn parent == cd` ALWAYS true, heap
  pointer-consistent post-scavenge (PHARO_SCAV_DANGLE_CHECK = 0 dangles),
  afterGC IP restore clean (no GC-VERIFY-FAIL).
- The failing assert is #2 of 3 in `CDBehaviorParserTest>>
  testSlotNodesHaveParentReference`: `slotNode classDefinitionNode identicalTo:
  classDefinition`. `classDefinitionNode` walk: `CDNode>>classDefinitionNode
  ^parent classDefinitionNode`; base `CDBehaviorDefinitionNode>>
  classDefinitionNode ^self` (prim 256 quick-return-self).
- `lookupMethod(classDefinitionNode, CDSlotNode)` → CDNode (CORRECT).
  `lookupMethod(...,CDClassDefinitionNode)` → CDBehaviorDefinitionNode (CORRECT).
- returnsSelf inline fast path fires only for cd (correct), never for sn.
  `PHARO_NO_GETTER_BIT=1` does NOT fix it → fast path exonerated.
- So `sn classDefinitionNode` does a FULL activation of CDNode>>
  classDefinitionNode and returns `sn` (its receiver) instead of the recursion
  result `cd`. => `^expr` returns receiver after a scavenge fires during the
  (1-deep nested) send.

## EXACT next step (a trace is half-applied — see below)
Goal: catch the wrong return value at the source.
File: src/vm/Interpreter.cpp, `returnFromMethod()` (~line 6327).
A trace was JUST ADDED (uncommitted, unbuilt) right after `Oop value = pop();`:
it logs, when `selectorOf(method_)=="classDefinitionNode"`, the methodCls,
receiver cls, returnVal cls, whether returnVal==receiver, and the new stack top.
ACTION ON RESUME:
1. `cmake --build build` (the trace edit is in the tree, not yet built).
2. `rm -f /tmp/cn_out.txt /tmp/sunit_run_completed.txt`
   `PHARO_NO_JIT=1 PHARO_SCAV_DANGLE_CHECK=1 timeout 50 ./build/test_load_image /tmp/cn.image > /tmp/rm.log 2>&1`
3. `grep RETMETH /tmp/rm.log` — interpret:
   - If `returnVal-cls=CDSlotNode sameAsRcvr=1` for the CDNode method →
     confirms returnFromMethod popped the receiver (sn). Then the bug is that the
     operand-stack top at ReturnTop is the receiver, i.e. the inner send's
     result was never left on the stack OR the stack was rewound to the receiver
     slot by the scavenge safe-point handling. Inspect the ReturnTop path
     (Interpreter.cpp:4895) + the send/return stack bookkeeping around a
     scavenge (safe point at Interpreter.cpp:2794, prepareForGC/afterGC).
   - If `returnVal-cls=CDClassDefinitionNode` (correct) here but the test still
     sees sn → the corruption is AFTER returnFromMethod (in the caller's stack
     placement) — trace the caller frame's result slot.

## Leading hypothesis for the fix
A scavenge safe point fires between the inner send completing (leaving `cd` on
the operand stack) and the outer `ReturnTop`. Something in prepareForGC/afterGC
or the send-result/stack bookkeeping rewinds/loses the just-pushed inner result
so ReturnTop pops the receiver instead. Compare operand-stack state (stackBase_,
stackPointer_, framePointer_) across the scavenge during this nested send.
Relevant memories: jit_materialize_bytecodeend_bug ("fb(N) returns receiver",
fixed 29df9943 — but that was JIT/materialize; this is interp),
jit_forceyield_reified_thiscontext.

## Repro image prep (if /tmp/cn.image is gone after reboot)
Cog tooling: `/tmp/harness/pharo` works (0.15s); the session-long flakiness was a
stale `/tmp/harness/startup.st` (DELETED — do not recreate). Fresh VM+image
backup at `/tmp/cogfresh/`.

```
cp /tmp/cogfresh/Pharo.image /tmp/cn.image; cp /tmp/cogfresh/Pharo.changes /tmp/cn.changes
# write /tmp/cn.st = the PB probe (see git stash / below), then:
touch /tmp/sunit_one_save_prep.txt
timeout 90 /tmp/cogfresh/pharo --headless /tmp/cn.image eval --save "'/tmp/cn.st' asFileReference fileIn"
rm -f /tmp/sunit_one_save_prep.txt
```
PB probe (`/tmp/cn.st`): class PB with classVar Buf; `mark:` appends to
/tmp/cn_out.txt via writeStreamDo; `churn` = `1 to: 40 do:[:k|(1 to:4000)do:[:i|Array new:4]]`;
`probe` = make `CDNormalClassParserTest new`, `setUp`, cd:=instVarNamed:'classDefinition',
sn:=cd slotNodes first, then `1 to: 8 do:[:r| self churn. cdn:=sn classDefinitionNode.
mark 'r',r,' cdNode==cd:',(cdn==cd),' cdNode==sn:',(cdn==sn)]`; startUp: resuming runs
probe then exitSuccess; register via `SessionManager default register:
(ClassSessionHandler forClassNamed: #PB) inCategory: SessionManager default userCategory`.
(Full text is in /tmp/cn.st if it survived; chunk file, double single-quotes inside compile: strings.)

## Uncommitted local changes right now
- src/vm/Interpreter.cpp: the RETMETH trace in returnFromMethod (NOT built, NOT committed).
- Everything else is committed. `git status` to confirm; `git stash` if you want a clean tree.

## Already-committed wins this session (do NOT redo)
- WideString synthetic-prim fix (d5608fd4): StringTest 438/438, verified JIT on+off + cross-VM.
- Cog tooling root-cause (startup.st deleted) + harness gotchas memory.
- Full-suite re-measure: 480 classes, 97.9%, batch failures = cumulative-state artifacts.
- PHARO_SCAV_DANGLE_CHECK diagnostic (gated, reusable) in ObjectMemory.cpp + debug_vars.h.

## Other reusable knobs
- PHARO_YG_NO_SCAVENGE=1 — workaround that hides the bug (do NOT ship as a fix).
- PHARO_SCAV_DANGLE_CHECK=1 — post-scavenge heap dangling-pointer scan (clean here).
- PHARO_DET_SCHED=1 — deterministic scheduling (repro holds under it).
