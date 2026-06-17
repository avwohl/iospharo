# Nested in-JIT sends for J2J-admitted callees (x86) — design

Planning doc for the architectural piece that the cross-method campaign
(boxes #18–24, see `docs/x86-cog-gap.md`) converged on. This is NOT a gate
relaxation or a save-field fix — those were tried and exonerated. It is the
mechanism that lets a J2J-admitted callee make its OWN sends without leaving JIT.

Status: DESIGN ONLY. No code beyond the diagnostic knobs already committed
(`PHARO_T1_X86_XMETHOD_SENDS`, `PHARO_T1_X86_J2J_DBG`, the `[MAT-SP]` trace).

## 1. Problem statement

x86 tier-1 cross-method inline-J2J admits only LEAF callees
(`canSkipJ2JSave` = `!canBailMidMethod && numICEntries==0`, AsmjitT1.cpp ~2929).
A leaf callee runs entirely in-JIT and returns via the V2 return prelude. That is
why `cfibx` (calling the leaf `incc=^self+1`) is a 6.4× JIT win.

The moment a callee has an inner send (`cmid=^(self+10) cleaf`, or any real
library method like `findElementOrNil:` which sends `hash`), admitting it does
NOT help: when the admitted callee reaches its inner send, that send exits to C++
(`ExitSendCached`), and because a J2J save for the caller is pending, the C++ side
must `materializeJ2JSaveIntoFrame` to reconstruct the caller frame before the
interpreter can continue. So admitting a send-bearing callee RELOCATES the bail
from the call site to the callee's inner send — and amplifies it: under
`PHARO_T1_X86_XMETHOD_SENDS=1` startup emits MILLIONS of (correct) materializes
and hangs even on `3 + 4`.

CONCLUSION (campaign): the bug was never in the save data (sp/bcOff/materialize
are all correct — exonerated boxes #23–24). The missing capability is **nested
in-JIT sends**: a J2J-admitted callee's inner sends must themselves stay in JIT
(inline-J2J the inner callee, or otherwise keep the operand + frame state
resident across the inner send) instead of taking the C++ `ExitSendCached →
materialize → resume` round trip per send.

## 2. The ONE question to answer first (before any code)

arm64 admits send-bearing callees (`PHARO_T1_XMETHOD_MAX_IC=8`) and works. There
are two mutually-exclusive explanations, and the fix differs completely between
them:

  (A) arm64 keeps the inner sends IN-JIT (true nested J2J) — its per-startup
      materialize count stays flat when send-bearing callees are admitted.
      => the fix is "port arm64's nesting to x86".

  (B) arm64 ALSO bails+materializes per inner send, but its bail/materialize/
      resume cycle is correct and merely slower (no hang) — the x86 hang is then
      a control-flow defect in x86's resume-after-materialize, not a missing
      nesting capability.
      => the fix is "fix x86's resume control flow", a smaller, different change.

FIRST EXPERIMENT — DONE (arm64, local, 2026-06-16). Counted `materialize: count`
on `3+4` startup with PHARO_T1_XMETHOD_MAX_IC=8 (admits send-bearing) vs =0
(leaf-only):

    arm64 MAX_IC=8 : materialize count = 35458  -> 3+4=7 (COMPLETES)
    arm64 MAX_IC=0 : materialize count =   956  -> 3+4=7

RESULT = **case (B)**. arm64 ALSO explodes the materialize count 37× when
admitting send-bearing callees (so it does NOT nest in-JIT — both arches
bail+materialize per inner send on cold ICs). But arm64 COMPLETES; x86 HANGS on
the identical workload. Therefore the x86 failure is a CONTROL-FLOW defect in
x86's materialize/resume cycle, NOT a missing nesting capability. **Pursue path
(B), §5 — not the architectural path (A), §4.**

Corollary: the materialize explosion is mostly STARTUP cold-IC churn (warm
steady-state inner sends inline-J2J and don't exit to C++); arm64 survives it,
x86 hangs in it. And since `materializeJ2JSaveIntoFrame` is SHARED C++ and both
arches are V2, the divergence is in the x86 EMIT's exit-state (what the JIT
leaves in OFF_SP/IP/DEPTH/CURSOR when a send-bearing callee's inner send exits
ExitSendCached), NOT in the shared materialize.

## 3. Current architecture (what a send compiles to, and the resident state)

Send-site emit (`emitOne_x86`, AsmjitT1.cpp ~2807–3360), per send:
- IC probe on the receiver class key.
- On hit + J2J-admit (bit 60 set, gates pass): inline-J2J — push a V2 J2JSave
  (40B: sp, receiver, tempBase, packed resumeAddr, closure), set callee state,
  `jmp` callee entry. Callee returns via `emitJ2JReturnPrelude_x86` (~1655): pop
  the save, restore caller sp/receiver/tempBase, `jmp` the packed resumeAddr →
  the caller's `resumeAfterCall` continuation (arg-pop + retval write, + xmethod
  caller-context re-derivation). NO C++.
- Otherwise: `ExitSendCached` → return to the C++ chain loop
  (`tryJITResumeInCaller` / the rj2j loop, Interpreter.cpp ~20404–21260).

The C++ chain loop, on `ExitSendCached` with a pending save (`j2jDepth>0`):
- can itself J2J the callee (`JIT_CALL` into compiled callee) — the rj2j loop, OR
- `materializeJ2JSaveIntoFrame` the pending save(s) into real `SavedFrame`s and
  let the interpreter run the callee — the slow path. THIS is what explodes.

Resident state that must survive a nested send (the "frame-state residency"
protocol, FSR M0–M4; arm64-tuned): `OFF_SP`, `OFF_RECEIVER`, `OFF_TEMPBASE`,
`OFF_METHOD`/`OFF_LITERALS` (derived from the JITMethod), `OFF_J2J_SAVE_CURSOR`/
`OFF_J2J_DEPTH`/`OFF_J2J_ENTRY_CURSOR`, and the operand stack itself. A leaf
callee needs none of this preserved across an inner send (it has none). A
send-bearing callee needs ALL of it consistent at the inner send so the inner
send can itself inline-J2J (push its own save on top) and return.

Key knobs already in the area: `PHARO_T1_NO_RESUME_SENDS` (default-on: condjump-
free send-bearing methods "advertise resume" so their post-send continuation
re-enters JIT) and `PHARO_T1_NO_CALLER_RESUME` (x86 diagnostic). The resume-sends
machinery is the existing partial answer; the gap is making it work for a
cross-method-J2J-ENTERED callee, not just a top-level activation.

## 4. Design — path (A): true nested in-JIT sends (if arm64 nests)

Goal: when a J2J-admitted callee hits an inner send whose callee is also
admittable, inline-J2J the inner callee directly (push its save on top of the
outer save), with NO C++ round trip. The save stack already supports depth
(`j2jDepth`, cursor); self-recursive deep chains (cfibx→cfibx→…) prove the
stack mechanism works for nesting of the SAME method. The work is making
CROSS-method nesting (caller≠callee at each level) resident.

Sub-steps (each behind `PHARO_T1_X86_XMETHOD_SENDS`, x86-only emit, arm64
byte-identical; validate `arm64 battery == /tmp/arm64_battery_golden_step1.txt`
after every shared-C++ commit):
1. At the inner send site of an admitted callee, ensure the admit's resident
   state (OFF_SP/RECEIVER/TEMPBASE/METHOD/LITERALS/cursor/depth) is exactly what
   the inner send's admit reads — i.e. the outer admit's callee-state writes must
   leave the SAME invariant a top-level method's prologue establishes. Audit the
   cross-method callee-state setup (AsmjitT1.cpp ~3007–3054) against what a
   normal method entry leaves resident.
2. Confirm the inner send's IC probe + inline-J2J path fires from within a
   J2J-entered frame (it must read the resident state, not a stale C++ mirror).
   The `[MAT-SP]` / a new `[NEST]` trace should show the inner send taking the
   inline-J2J push (depth grows) rather than ExitSendCached.
3. The return cascade: inner callee returns → outer callee's resumeAfterCall →
   outer callee returns → caller's resumeAfterCall. The V2 prelude already pops
   one save per return; verify the depth bookkeeping balances at each level (the
   `PHARO_T1_X86_J2J_DBG` PUSH/POP balance trace is the witness — leaf shows
   balanced, nested must show depth 0→1→2→1→0).
4. Bail safety: an inner send that CAN'T inline-J2J (cold/poly IC, non-compiled
   callee) still has to bail to C++ correctly — that path is exonerated
   (materialize is correct), it's just slow. Acceptable as the cold-path
   fallback as long as the WARM path nests (the materialize count must not
   explode in steady state).

## 5. Design — path (B): fix x86 resume control flow (if arm64 bails-but-works)

CONFIRMED this is the path (§2 = case B). The save DATA is correct (exonerated),
so the defect is that x86's resume-after-materialize does NOT make forward
progress — it re-materializes the same saves. Evidence (local arm64 vs box x86,
both PHARO_J2J_MAT_LOG, `3+4`):

    save              arm64 (COMPLETES)   x86 (HANGS)
    hash bcOff=2            7427             134076   (~18x, still unfinished)
    hash bcOff=5           12435             116402

x86 materializes a single save ~18x more than arm64 materializes it across the
WHOLE completed startup — i.e. x86 is stuck re-materializing without advancing.

PRIME SUSPECT: the documented x86 caller-resume operand-stack leak
(`PHARO_T1_NO_CALLER_RESUME` in debug_vars.h: "the x86 tier-1 caller-resume
re-entry has a ~1-word-per-send operand-stack leak"). When a materialized
send-bearing callee resumes via x86's BUGGY `tryJITResumeInCaller`, the per-send
leak accumulates -> sp drifts -> non-progress / re-bail loop -> hang. arm64's
caller-resume has no such leak, so it advances and completes.

DECISIVE TEST (binary, one box): run `SENDS=1` WITH `PHARO_T1_NO_CALLER_RESUME=1`
(forces interp-after-send, bypassing the buggy x86 caller-resume). If startup
COMPLETES (`3+4`=7), the caller-resume leak IS the SENDS hang cause -> the fix is
to repair the x86 caller-resume sp protocol (or ship SENDS only with caller-resume
off, accepting the interp-after-send perf cost). If it STILL hangs, the leak is
not the (sole) cause and the resume re-entry must be traced directly
(ip-before/ip-after per resume, vs arm64).

This is a smaller, localized C++/emit fix than path (A) — pursue it.

UPDATE (box #25, 2026-06-16): the caller-resume-leak suspect is REFUTED —
`SENDS=1 + PHARO_T1_NO_CALLER_RESUME=1` STILL HANGS (`3+4` never completes). So
the non-progress is NOT in the JIT caller-resume re-entry; it persists even when
post-send execution is forced into the interpreter. The loop is in the
bail→materialize→chain-loop cycle itself.

REFINED ROOT MODEL (definitive, from AsmjitT1.cpp:11321-11356 + the §2 result):
- The x86 emit BAILS mid-method on inner sends (ExitSendCached), ExtSend, and
  non-tail arith. `x86HasMidBail` (a term of `canSkipJ2JSave`) detects this — but
  it is only COMPUTED for `numIC==0` methods (line 11328). For send-bearing
  callees (numIC>0) no such analysis is done, and EVERY send-bearing callee bails
  mid-method at its own inner sends.
- A mid-method bail of a CROSS-METHOD-INLINED callee leaves the caller's J2J save
  un-popped (the callee never reaches its return prelude). The comment at
  11338-11340 documents exactly this leak→corruption hazard; `canSkipJ2JSave`/
  leaf-only exists to admit ONLY callees that NEVER bail mid-method.
- Therefore the leaf-only gate is FUNDAMENTALLY correct for the current x86 emit:
  a send-bearing callee cannot be safely inlined because its inner sends bail.
- arm64 admits send-bearing callees and survives the identical bail+materialize
  churn (§2: 35458 materializes, completes); x86 does the same churn but its
  resume-after-mid-method-bail of an inlined callee does NOT advance (18x more
  materializes of the same save, then hang).

So the two viable fixes remain, now sharply defined:
  (A) nested in-JIT sends — make the inner sends NOT bail (inline-J2J / chain-loop
      them in-JIT) so the inlined callee always reaches its return prelude.
  (B) make x86's resume-after-mid-method-bail of an inlined callee ADVANCE like
      arm64's — a control-flow fix in the chain-loop / materialize-resume.
Either is a deep effort. The DECISIVE next instrument (needs lldb or a per-
iteration chain-loop trace, NOT another env-combo box): catch the exact step
where x86 re-materializes the same (caller,bcOff) without advancing, and diff the
same point on arm64 (which advances). Refuted so far: double-pop, bail-leak,
tail-send/resume-bcOff, materialize sp/bcOff, caller-resume leak, isStubOnEntry
(not the core — send-bearing callees bail at their SENDS, not just stubs).

## 6. Validation

- Primary metric: per-startup `jitMaterializeCount_` with SENDS-on must be within
  a small factor of SENDS-off (no explosion). This is the direct "does it nest"
  signal — far more reliable than timing.
- `PHARO_T1_X86_J2J_DBG` PUSH/POP balance: nested sends show depth returning to 0.
- Correctness: the §4-of-x86-cog-gap repro (`[N timesRepeat: [5 cmid]]` → 115),
  battery == golden, and a real load (startup completes; a small SUnit batch).
- Then the full gate-flip criteria in `docs/x86-cog-gap.md` §7 (still gated on
  the independent prescan/emit-disagree + Corrupt-stackPointer blockers).

## 7. arm64 safety (hard constraint)

All emit changes live in `emitOne_x86` / x86-only `#if` blocks → cannot alter an
arm64 byte. The only arm64-touching surface is shared C++ (Interpreter.cpp chain
loop / materialize). Gate every shared-C++ change behind `PHARO_T1_X86_XMETHOD_
SENDS` (an x86-only knob, off on arm64) and re-verify `arm64 battery ==
/tmp/arm64_battery_golden_step1.txt` byte-for-byte after EVERY commit to a file
arm64 compiles.

## 8. Scope, risk, and the honest recommendation

- This is the most bug-prone area of the branch (the cross-method saga). Path (A)
  is a multi-session architectural effort; path (B) is smaller but only applies
  if the §2 experiment says so. Run §2 FIRST — it is one cheap box and it decides
  everything.
- Even a perfect fix here does NOT flip `PHARO_X86_JIT` default-on by itself: the
  ~10 prescan/emit-disagree bytecodes and the Corrupt-stackPointer miscompile
  (commit 7af58fdfa) are independent blockers (docs/x86-cog-gap.md §7).
- ROI check: the x86 JIT is an opt-in capability (like tier-2/Sista on x86); the
  interpreter is correct and arm64 (the shipping config) is at Cog parity. The
  leaf-only cross-method win (Increment 1, cfibx 6.4×) already exists for when
  the JIT is enabled. Decide whether full x86 send-bearing cross-method parity is
  worth a multi-session campaign before starting path (A).

## 9. Pointers (campaign artifacts)

- `docs/x86-cog-gap.md` — measurement, ruled-out leads, the full diagnosis.
- Repro: `cleaf ^self+100` / `cmid ^(self+10) cleaf` / `[N timesRepeat: [5 cmid]]`
  under `PHARO_X86_JIT=1 PHARO_T1_X86_XMETHOD_SENDS=1` (but note: startup itself
  hangs under SENDS=1, so the §2 experiment uses the materialize COUNT on a
  short/leaf-only-vs-admit comparison, not this hanging repro).
- Knobs: `PHARO_T1_X86_XMETHOD_SENDS` (admit send-bearing — currently hangs),
  `PHARO_T1_X86_J2J_DBG` (PUSH/POP save-stack balance), `PHARO_J2J_MAT_LOG` +
  `[MAT-SP]` (materialize sp/bcOff + tail-send fingerprint), `PHARO_J2J_MAT_SEL`.
- Key code: send emit + cross-method admit AsmjitT1.cpp ~2807–3104; V2 return
  prelude ~1655; resumeAfterCall ~3327; materialize Interpreter.cpp ~23894;
  chain loop / tryJITResumeInCaller ~20404–21260; bailMatJ2J ~25379.
- Golden: `/tmp/arm64_battery_golden_step1.txt`.
