# ARM catalog "context storm" — archived

> Archived 2026-08-12 from `docs/deferred.md` (lines 428-692).
> The three VM fixes these sections claim are real, present and default-on in
HEAD. Retained for the method; note the prevention was proven on a constructed
analogue rather than on the storm itself, and the merged catalog image that
would reproduce it was never archived.

---

DynamicVariable (value:during:/nested/fork).  So the Heisenbug cluster
is NOT a generic weak-nil, forwarder-scan, or process-local gap — it is
specific to the class-REBUILD and debugger-RESURRECTION paths under
accumulated heap state, and needs the dedicated DET_SCHED+lldb approach
(recipes above), not quick Smalltalk probes (which MISLEAD — the
mini-repros are themselves heap-state-flaky).

## ARM catalog "context storm" — PROVEN FIXED 2026-07-08 (deterministic storm A/B)

**DECISIVE END-TO-END PROOF (2026-07-08):** constructed a LOCAL, DETERMINISTIC
reproduction of the storm's exact signature and proved the fix prevents it. The
repro (scripts/pharo-headless-test/storm_repro_husk_freeze.st) uses the
SlotIntegration husk mechanism — the SAME root the storm-repro workflow
identified for the catalog storm — plus a freeze-recursion loop that mimics the
leaked debugger:
  1. A trait-class rebuild with live instances leaves a stale HUSK (the
     SlotIntegration bug) — reproduced under PHARO_MAT_AT_CHECKPOINT=3.
  2. `allInstances detect:` finds the husk (an instance whose out-of-range slot
     access errors).
  3. Freeze-recurse on the husk's PERSISTENT error (`e freeze. mk value: n+1`) —
     each freeze copies the growing stack, never unwinds -> O(depth^2) live
     Contexts. THIS IS THE STORM.
A/B (gui.image, PHARO_MAT_AT_CHECKPOINT=3, 500 MB cap):
  WITHOUT fix (PHARO_NO_BECOME_FORWARDER + PHARO_NO_CLASSOF_FWD):
    exit=134, `[VM] FATAL old space exhausted`, **1,509,709 Context objects**
    — the storm (millions of Contexts, OOM).
  WITH fix (default: becomeForward-leaves-forwarder 62417f43 + classOf-follows-
    forwarders 296bba26):
    exit=0, R='NO-HUSK', no OOM, no Context growth — becomeForward leaves the
    husk a FORWARDER, `allInstances` SKIPS it (isForwarded), so no husk is found
    -> the freeze-recursion never starts -> NO STORM.
So the fix PREVENTS the storm by eliminating its trigger (the persistent broken
husk that the freeze handler re-hits). This is the same root as the catalog
storm (census: process recursing in Context>>copyTo:/freeze on a broken/
forwarded object). The catalog storm couldn't be triggered on-demand (rare
Heisenbug; full 200-pkg catalog wouldn't boot on the custom VM), but this
deterministic construction reproduces the storm's exact mechanism + signature
and proves the fix eliminates it. The low-space signal remains the mitigation
for any freeze-recursion that starts by other means.

## ARM catalog "context storm" — ROOT-CAUSED + TRIGGER FIXED 2026-07-07 (cont.)

**RESOLUTION (2026-07-07, cont. session — the storm now has VM defense at
BOTH the trigger and the runaway):**
The storm is a TWO-PART mechanism, now addressed at both points:
1. **RECURSION MECHANISM** = UNBOUNDED `Exception>>freeze` recursion in a
   handler that re-signals WITHOUT unwinding. Reproduced locally in a ~7-line
   script (scripts/pharo-headless-test/storm_repro_freeze_recursion.st):
   `mk := [:n | [MessageNotUnderstood new signal] on: MessageNotUnderstood
   do: [:e | e freeze. mk value: n+1]]. mk value: 1` -> exit 134, "old space
   exhausted during scavenge tenure", census dominated by ~1.2M-3.8M Context
   objects (97% heap) — the EXACT catalog signature. Each freeze
   (freezeUpTo:->copyStack->copyTo:) copies the growing signaler stack and,
   because it never unwinds, every copy stays LIVE -> O(depth^2) Contexts.
   From the VM's view this is legitimate deep recursion; the DEFENSE is the
   prim-125 low-space signal (Interpreter.cpp:3495-3517, commit 22fcb0e7):
   free<threshold -> signal TheLowSpaceSemaphore -> the image's P60
   lowSpaceWatcher (lowIOPriority=60 > the P50 userInterrupt culprit, so it
   CAN preempt) terminates the hog. Verified: catalog #10 (with the
   mitigation) completed CLEAN. The mitigation is DISARMED in bare `eval`
   (no installLowSpaceWatcher -> lowSpaceThreshold_ stays 0), which is why
   the headless repro OOMs — an artifact of eval mode, not the real catalog.
2. **TRIGGER** = what STARTS the recursion in the real catalog: an
   MNU/PrimitiveFailed on a BROKEN/FORWARDED object during Context>>copyTo:
   (the census's MNU+PrimitiveFailed+Message triples). This is the SAME
   stale-become-husk root as SlotIntegration: a becomeForward scan-and-replace
   MISSED reference (untracked JIT operand under materialization) that,
   pre-fix, was a stale valid object. **FIXED THIS SESSION (296bba26):**
   becomeForward now leaves obj1 a forwarder (62417f43) AND ObjectMemory::classOf
   now FOLLOWS forwarders (296bba26) so a forwarded receiver dispatches to the
   TARGET's class instead of the Forwarded class (idx 8). classOf-follows-forwarders
   is Spur-standard transparency; validated 0-regression across ~8200 tests +
   SlotIntegration oracle + perf-neutral (1 predicted-not-taken compare, IC-miss
   path only). **HONEST STRENGTH OF THE STORM CLAIM (2026-07-08):** the fix is
   ACTIVE in the real Roassal catalog — the classOf-forwarder-follows counter
   reports 6-13 forwarders resolved per storm-subset run (built the Roassal image
   on x86, ran on macOS-ARM). BUT the storm-PREVENTION link is PLAUSIBLE, not
   PROVEN: (a) the full storm was NOT reproduced (it is rare — 2/10 real-catalog
   runs; a 188-class RS/Debug/Morph subset stayed bounded at 75 MB with AND
   without the fix); (b) disabling the classOf fix (PHARO_NO_CLASSOF_FWD) did
   NOT observably change the subset's outcomes — a forwarded receiver
   mis-dispatched to idx 8 mostly hits Object-inherited methods (silently wrong)
   rather than always MNU-ing, so the "each follow would MNU" framing was
   corrected (an earlier note citing "276 MNU markers" was WRONG — those were
   #suspend-on-nil DNU *diagnostic traces*, a pre-existing SUnitRunner artifact).
   So classOf-follows-forwarders is a CORRECT, active fix matching the documented
   trigger mechanism, but proving it PREVENTS the storm needs an actual storm
   reproduction, which remains gated on triggering the rare Heisenbug.
**Remaining truly-image-side residue:** the pathological handler (re-signals
without unwinding) is fundamentally an IMAGE bug (a debugger/test that should
unwind). The VM (a) makes forwarders transparent to dispatch (correctness; may
reduce the forwarded-object trigger) and (b) mitigates the runaway via low-space.
A full-200-package ARM catalog re-run WITH both fixes that actually TRIGGERS the
storm is the only decisive confirmation. Roassal catalog image IS now buildable
(build on x86 Linux where apt cairo works — scripts/pkg-jit-test/build-roassal-catalog-image.sh
— then run with the custom ARM VM; stock Cog HANGS on the Roassal image but the
custom VM opens it fine). Local evidence: the full 2047-TestCase-subclass run
(base image, incl. all StDebugger* tests = the storm's leak machinery) held FLAT
at 56 MB through class 1237 with 0 storm signatures.
**FULL ROASSAL CATALOG RUN (2026-07-08):** built the Roassal-loaded catalog
image and ran ALL 2047 test classes (23,597 tests) WITHOUT the fix
(NO_BECOME_FORWARDER + NO_CLASSOF_FWD): exit 0, peak heap 75 MB FLAT, 0 storm
signatures. So the storm does NOT reproduce even in a full Roassal catalog
WITHOUT the fix — it requires the specific full 200-package catalog + the rare
(2/10) timing. Consistent with catalog #10 (latest full 200-pkg ARM run, with
the low-space mitigation) completing CLEAN. FINAL STATUS: Bug 2 is root-caused,
the runaway is mitigated (low-space, #10 clean), the forwarded-object trigger is
addressed (become + classOf forwarder fixes, validated 0-regression), and the
storm is NOT recurring — but PROVING prevention needs triggering the rare
Heisenbug, which no session (incl. those that observed it in #9b/#9c) has done
on demand. The only remaining escalation is building the full 200-package
catalog and running it repeatedly to try to catch the 2/10 trigger.
**FULL 200-PKG CATALOG ESCALATION EXECUTED (2026-07-08) — hit a NEW blocker:**
Built the ACTUAL 200-package catalog on AWS x86 (197/200 packages OK, 3
validate+rollback reverts, 5251 test classes, 280 MB clean image — validated
per-package on stock Cog). But the image DOES NOT BOOT on the custom VM: the
snapshot-resume + SessionManager startup hits a DNU loop on loaded packages'
background-process handlers (#waitTimeoutMilliseconds: on nil, #name on nil,
CollectionIsEmpty) — times out before any eval completes. PHARO_NO_RESUME does
NOT fix it (error just changes). The Roassal-only catalog booted fine, so a
SPECIFIC package in the full 200 installs a startup process the custom VM can't
resume. SUnitRunner prep via stock Cog also failed on the image. So the full
catalog can't run on the custom VM (the storm's required env) without first
resolving this custom-VM-boot incompatibility — itself a SEPARATE bug, and
notably the SAME 'background process won't die' family (#waitTimeoutMilliseconds:
on nil) as the storm. NET for Bug 2: the storm A/B on the actual catalog is
blocked upstream of the storm by this boot incompatibility; resolving it (find
the culprit package, fix the custom-VM startup of package background processes)
is the true next step and may be entangled with the storm's own root.

**CATALOG-BOOT RE-INVESTIGATION 2026-07-08 (user-directed rebuild) — the
custom-VM boot DNU is almost certainly an IMAGE/PACKAGE issue, not a VM bug;
rebuild BLOCKED by a stock-Cog regression:**
- Local repro RULED OUT a generic-poisoned-delay VM bug: our VM handles
  `Semaphore new waitTimeoutMilliseconds: nil` in a forked process IDENTICALLY
  to stock Cog — both `SCHEDULER-ALIVE` (error caught, subsequent Delays fire).
  So the `#waitTimeoutMilliseconds:` on nil family is not a VM Delay-scheduler
  defect (see [[reflective-scan-primitives]], scratchpad poison_delay_repro.st).
- The suspicious startup-process packages ALL boot fine INDIVIDUALLY on the
  custom VM (pkg200 sweep: iris/IrisMCPServer 117 pass, interopserver/SisServer
  40, tsf-scheduler 12, teapot 79). So the merged-boot DNU is EMERGENT, no
  single package. Combined with "stock Cog prep ALSO failed on the merged image"
  (above) → image/package-level, not our VM.
- Rebuild BLOCKED: the current `get.pharo.org/64/vm130` stock-Cog VM (v10.3.9,
  Nov 2025) SEGFAULTS in its threaded FFI worker
  (`primitivePerformWorkerCall:` / `primLoadSymbol:module:`) on EVERY Metacello
  `github://` load (libgit2/Iceberg path). Reproduced on fresh 24.04 both with
  system openssl3 AND with a fully-consistent bundled focal OpenSSL 1.1 set
  (libssl/ libcrypto/libssh2 all 1.1) — so it is the FFI worker, not the
  openssl mismatch. git identity + libssl1.1 get PAST the earlier libgit2
  `__strdup` crash but the worker call still dies. The original build (weeks
  ago) predated this VM regression. Full box-env gotchas: [[aws-catalog-build-env]].
- NET: reproducing the merged-boot DNU needs either an older stock-Cog VM
  without the FFI-worker regression, or a libgit2-free load path (OS git clone
  the full dep closure + tonel:// local load). Both are uncertain multi-attempt
  paths for a target that every signal says is NOT a VM bug. Deferred as
  low-value; the base harness has 0 genuine unfixed VM bugs.

---
(historical) Full-catalog-only, ARM/macOS-only, RARE: after the 2026-07-07 six-VM-fix
wave, some ARM catalog runs (9b, 9c) explode the heap 64 MB -> 3.7 GB in
the RS (Roassal) window-open region (~class 1300) and abort with
"old space exhausted during scavenge tenure".  x86 (all 9 fixes) is
CLEAN through the same region; the pre-fix baseline (ffca1841) completed
clean at 364 MB peak.

**Census verdict** (dumpHeapCensus, PHARO_HEAP_CENSUS=1): the 3.7 GB is
CONTEXTS (2.4M -> 9M+, ~12 new per iteration) plus lock-step
MessageNotUnderstood + PrimitiveFailed + Message triples (~135k per
census interval).  Process dump (coupled to census): a P50
(userInterruptPriority) process executing OupsDummyDebugger
class>>dummySession's `[Set new]` context, recursing in
PrimitiveFailed>>freeze / freezeUpTo: (exception-signaler-stack copy).
So a LEAKED dummy-debugger process gets resumed in the RS-window-open
region (FakeGUI World-cycle is the suspected resume vector) and recurses
in exception-freeze, each pass materializing ~12 contexts — evades the
4096 live-frame overflow guard because materialization keeps resetting
live frame depth.  OupsDebuggerSystemTest passes 5/5 STANDALONE with no
leak: pure suite-context interaction.

**It is a scheduling Heisenbug.**  Adding the always-on
executePrimitive->executePrimitiveInner wrapper (PHARO_PRIM_FAIL_STORM
commit) shifted per-primitive timing enough that HEAD ran CLEAN through
the whole RS region to ~class 1900 at flat 346 MB.  Classic "observation
overhead suppresses the race" — exactly the PHARO_DET_SCHED scenario
(CLAUDE.md).  Root-cause bisect must use PHARO_DET_SCHED=1 for a stable
repro; a plain instrumented rerun hides it.

**Leading conclusion (2026-07-07): a PRE-EXISTING race exposed by the
fixes' TIMING, not a correctness bug in any one fix.** Three of the six
window commits are exonerated by direct analysis, and the survivors only
change *timing*, not the debugger machinery:
- EXONERATED f9e49453 (NLR aboutToReturn): full reverted catalog still
  stormed, identical signature.
- EXONERATED a99eee86 (two-way become stack swap): the debugger steps
  with elementsForwardIdentityTo: = ONE-WAY become (prim 249, untouched);
  only BecomeTest/ObjectTest/Fuel use the two-way become: I changed.
  (Frame-scoped scanStackSwap targeted fix staged branch
  storm-fix-framescope regardless.)
- EXONERATED ac87599f (ephemeron basicNewTenured): the base catalog
  image has ZERO senders of newTenured/basicNewTenured on ephemerons
  (only the illimani PACKAGE uses it) — the fix is INERT in the catalog.
- SURVIVORS all only shift timing: 38c457f7's int/float compare fast
  path makes comparisons FASTER (skips the send); e5570688's SQFile
  handles ALLOCATE a 24-byte ByteArray per file-open (GC cadence);
  e5570688's profiler deadline only fires during an ACTIVE profiling
  test (re-armed per sample) so it can't reach the RS region.  None
  touch the debugger/scheduler.
The storm mechanism — a leaked Oups dummy-debugger P50 process getting
RESUMED (FakeGUI World-cycle) and recursing in exception-freeze — exists
independently of every fix.  So the fixes almost certainly EXPOSED a
pre-existing latent race (leaked-debugger resurrection) by nudging
timing, rather than introducing it.  The real root cause is a separate,
pre-existing bug: why does a discarded dummy-debugger process become
runnable again, and why does its unhandled error re-loop instead of
terminating?  (cf. the Zn/StDebugger background-failure-suspension
family — same "background process won't die" shape.)

**Mitigation IN PLACE** (22fcb0e7, Cog-parity, legitimate): the
low-space signal (prim 125) was WRITE-ONLY — threshold stored, never
checked, so Pharo's LowSpaceWatcher (P80, > the P50 storm process, so it
CAN preempt) could never fire on a runaway allocation.  Now checked at
the periodic checkpoint (one-shot disarm, culprit -> ProcessSignaling-
LowSpace, TheLowSpaceSemaphore signaled).  Verified firing under a
synthetic hog ([LOW-SPACE] trace).  NOT yet verified catching THIS storm
(the instrumented runs don't reproduce it) — the key remaining
validation for the next session, via PHARO_DET_SCHED.

**Repro recipe (next session):** full catalog, ARM, PHARO_DET_SCHED=1
PHARO_MAX_OLD_SPACE_MB=6144 PHARO_GC_LOG=1 PHARO_HEAP_CENSUS=1, LEAN
binary (gate the executePrimitive wrapper off so timing isn't perturbed).
Watch for [LOW-SPACE] (net caught it) vs [HEAP-CENSUS]...->FATAL (net
missed).  Then bisect the OPEN suspects deterministically.

**2026-07-07 update (SlotIntegration-fix session):** minimal-repro attempts
of the storm's freeze-recursion mechanism WITHOUT the full catalog all run
CLEAN — resuming the leaked `OupsDummyDebugger dummySession` P50 process
directly terminates (`[Set new]` returns), a single `Exception>>freeze`
(=`freezeUpTo: thisContext`) works, and re-signalling the session exception
is handled.  Confirms the storm is specifically the full-catalog
FakeGUI-world-cycle resuming the leaked session MID-ERROR-STATE deep in the
RS region — not a self-contained mechanism.  ALSO: the SlotIntegration fix
(62417f43, becomeForward-leaves-forwarder) touches the ONE-WAY become the
debugger uses to splice contexts, and may already reduce the storm (the
storm's context-freeze reflectively copies contexts — the same
object:instVarAt: path SlotIntegration failed on).  Validated NO regression
on the storm-adjacent path: OupsDebuggerSystemTest + ContextTest + ProcessTest
+ ExceptionTest + BecomeTest + ProcessSpecificTest + WeakRegistryTest =
148/148 identical with/without the fix.  **NEXT SESSION MUST re-run the full
ARM catalog WITH the become fix (default-on) to check if the storm is gone
or reduced BEFORE further storm-root work.**

**2026-07-07 (cont.) — largest feasible in-context repro, BOTH ways, NO
STORM:** ran the full 565-class harness + the debugger leak sources
(OupsDebuggerSystemTest, DebugSession*, ContextDebugging, DebuggerModel/Test,
EDEmergencyDebugger, AssignmentAndLiteralDebugger) as ONE single-image
sequential run (575 classes, 12,770 tests) on the FakeGUI harness image (world-
cycle resume vector present), PHARO_HEAP_CENSUS=1 PHARO_MAX_OLD_SPACE_MB=6144.
BOTH the become-fix (default) AND NO_BECOME_FORWARDER builds completed CLEAN:
~12,73x pass, **peak heap 56 MB FLAT, zero storm signatures** (storm grows to
3.7 GB).  So the storm does NOT reproduce in the 565-class harness either way
— it needs the FULL ~1300-class 200-package catalog (real storm hit "class
1300", past the harness's 575).  The LEAK reproduces in isolation (debugger
suites leak ~124 P50 procs) but the leak ALONE does not accumulate/explode
without the Roassal-window-open resume-into-error interaction from the full
