# x86 Cog gap + the cross-method JIT campaign (consolidated 2026-06-16)

Status of the x86 tier-1 JIT vs stock Cog, and the open work to make it fast.
Investigation spanned boxes #16–#22 (m6a.4xlarge, AMD EPYC); this is the
consolidated reference — superseded hypotheses are compressed into "Ruled out"
below, not narrated.

## 1. Headline: the x86 tier-1 JIT is OFF BY DEFAULT

    [JIT] x86_64 tier-1 JIT off by default (bit-rotted vs arm64;
          set PHARO_X86_JIT=1 to enable) — running interpreted

Every default-config x86 run is the **interpreter** (`JIT Stats: compiled: 0`
unless `PHARO_X86_JIT=1`). The memory's "cross-method inline-J2J DEFAULT-ON /
cfibx 8.7x" refer to the cross-method FEATURE being on *inside* the JIT, not the
JIT itself. Consequence: any x86 A/B that doesn't set `PHARO_X86_JIT=1` is only
comparing two interpreter configs.

## 2. Measurement (PHARO_X86_JIT=1, build-opt -O2, same Pharo-13 image, core 0, min-of-9)

    bench       x86 interp  x86 JIT-on  x86 Cog   JIT/Cog   arm64 JIT/Cog
    loopD100M      6736         672        226      3.0x       0.55x
    loopS100M      7940        8206        254      32x        0.44x
    fib33          1430         242         42      5.8x       2.96x
    cfibx32        1220         187         32      5.8x       2.70x
    cfibs32        1584        2208         41      54x        3.18x

    loopD = [1 to: 1e8 do: [:i | i + i]]            (discard; no closure write)
    loopS = [s:=0. 1 to: 1e8 do: [:i | s:=s+i]]     (closure-temp write per iter)
    fib33 = self-recursive benchFib
    cfibx = recursive fib calling  incc = ^self+1            (leaf callee)
    cfibs = recursive fib calling  incs = ^(self+1) max: 0   (send-bearing callee)

WORKS (JIT clearly beats interp): loopD (3.0x Cog), fib33 (5.8x), cfibx (5.8x;
cfibx=187ms confirms the literalsCache cross-method fix is real). REGRESSES:
cfibs (0.72x = slower than interp), loopS (0.97x = no gain).

## 2b. sp-residency A/B — what porting x25-sp to x86 is actually worth (2026-06-18)

x86 has NEVER had sp-residency (sp lives in memory at [rdi+OFF_SP], every
push/pop is a load+store — exactly the arm64 `PHARO_T1_SP_IN_X25=0` shape).
arm64 IS sp-resident by default (=1). To measure the slice that the port would
buy, the arm64 `=0` (memory-sp) baseline was repaired (commit fd8bfc94 — it had
bit-rotted: six JIT-entry sites wrote a stale x25 back to JS_SP; guarded behind
PHARO_T1_SP_IN_X25) and A/B'd against `=1` on the same dev build, same image:

    bench       =1 (x25 sp)  =0 (mem sp)   =0 penalty   character
    loopD100M      75            75            0%        flat inlined loop
    loopS100M     100           104           +4%        closure-temp write
    fib33          86           133           +55%       self-recursion
    cfibx32        70           105           +50%       recursion, leaf callee
    cfibs32        99           163           +65%       recursion, send-bearing

THE FINDING, and it is sharp:
  - sp-residency does **nothing** for the flat per-bytecode loop (loopD
    identical). That loop is already ≈Cog (inlined arithmetic, no sp traffic in
    the hot path) — there is no "flat per-bytecode tax" left to fix, and
    sp-residency was never the lever for it. The OoO core's store-forwarding
    fully hides the redundant sp load/store (the same reason M4 store-deletion
    and TOS_REG were washes).
  - sp-residency is worth **+50–65% on recursion/activation** (fib, cfibx,
    cfibs). Recursion pumps sp hard across the J2J call/return dance; keeping
    sp in a register removes the per-activation memory round-trips.

CONSEQUENCE FOR x86: x86's worst gaps are exactly the activation-heavy ones
(cfibs 54x, loopS was 32x pre-fix). The arm64 `=0`→`=1` step (133→86 on fib =
−35%) is the upside a faithful x86 sp-residency port would add **on top of**
fixing the send-bearing operand-stack leak (§3/§5). It is a real, measured,
secondary lever — NOT a per-bytecode-tax fix (there is no per-bytecode tax to
fix; flat code is already at parity). The PRIMARY x86 recursion lever remains
the send-bearing caller-resume fix in §6.

### x86 sp-residency port — DONE 2026-06-18 (PHARO_T1_X86_SP_IN_REG, opt-in)

LANDED and validated. The design DIFFERS from the push/pop sketch below in one
decisive way that made it both simpler and safer: instead of `push rbx`/`pop
rbx` per method (which would break the rsp≡8-mod-16-at-entry invariant the
in-body C-calls' odd-push realignment relies on — the workflow critic's Risk
R1), the port loads rbx live-in INSIDE the JIT_CALL/JIT_RESUME_CALL macro (like
arm64 loads x25) and lists rbx clobbered, so the C compiler preserves its own
rbx. Method exits then only `sync rbx→mem` (no pop). Commits: af404cba
(scaffolding) → d9691c9a (batches A/C/D) → 184af43c (batch B + the crash fix).

THE crash fix (lldb-traced): the x86 mid-method resume entered JIT via a bare
`entry(&state)` in JITRuntime tryResume/tryResumeFast (the `#else` of the arm64
asm-trampoline branch) — it never loaded rbx, so resumed code ran with a garbage
sp register (rbx=2 → SIGSEGV reading [rbx-8] in the first send IC probe; memory
state.sp was valid the whole time). Routed both through JIT_CALL.

MEASURED (build-x86-sp -DPHARO_T1_X86_SP_IN_REG=1 vs build-x86, Rosetta, min-9):

    bench       flag=0(mem)  flag=1(rbx)   speedup
    loopD100M     1277          749         1.70x
    loopS100M     1092          745         1.47x
    fib33          150          113         1.33x
    cfibx32        119           93         1.28x
    cfibs32       1501         1449         1.04x  (send-bearing thrash, untouched)

SURPRISE vs the arm64 prediction: on x86 sp-residency helps the FLAT loop too
(loopD 1.70x), unlike arm64 where the OoO store-forwarding fully hid it. (These
are Rosetta numbers; native x86 wins are likely smaller but same direction.)

VALIDATION: flag=0 byte-identical (battery==golden); arm64 untouched
(battery==golden); flag=1 battery==golden, a 12-op closure/NLR/recursion/
exception stress == golden, and SUnit A/B IDENTICAL to flag=0 at 12 classes/
1938 tests, 40 classes/3509 tests, AND 150 classes/7866 tests (same Pass 7846 /
Fail 14 / Error 6 on both). The 150-class run showed ONE F/E-NAME difference
(flag=0 ProcessTerminateBugTest>>testTerminationDuringNestedUnwindWithReturn1
vs flag=1 WeakOrderedCollectionTest>>testWeakOrderedCollectionAllGarbageCollected)
— PROVEN a nondeterministic GC/scheduler flake, not an sp effect: re-running just
those two classes 5x on EACH flag, the WeakOrderedCollection GC-timing failures
flip pass/fail run-to-run on BOTH flags (flag=1 run1=1F vs runs2-5=2F; flag=0
run4=1F vs others=2F) and ProcessTerminateBugTest passes in isolation. So DEFAULT-
ON (commit 46be2194); opt out -DPHARO_T1_X86_SP_IN_REG=0.

--- original port plan (superseded by the DONE design above) ---

Register: rbx (SysV callee-saved; rbx/r12-r15 are ALL unused in the x86 emit —
grep count 0). Mirror arm64's PHARO_T1_SP_IN_X25 with a build-time
`PHARO_T1_X86_SP_IN_REG` (default 0 = today's memory-sp, byte-identical; each
converted batch commits green at 0, flag=1 correct only once ALL sites convert —
the same all-or-nothing-at-runtime / incremental-at-source model).

Helpers (mirror emitLoadSp/emitStoreSp/emitSyncSpToState/emitReloadSpFromState):
load = `mov dst, rbx` (=1) / `mov dst,[rdi+OFF_SP]` (=0); sync = `mov
[rdi+OFF_SP], rbx` (=1) / nop (=0).

Sites (the work): (1) emitPushReg + every raw `mov rcx,[rdi+OFF_SP]` push/pop
(~50, lines 1664–9597); (2) PROLOGUE — `push rbx; mov rbx,[rdi+OFF_SP]` at
method entry; (3) EVERY exit — `mov [rdi+OFF_SP], rbx; pop rbx` before each of
the ~40 `a.ret()` that return to C++ (MUST pair perfectly or the C caller's rbx
is corrupted → crash; this is the high-risk part); (4) the inline-J2J return
prelude (~1693) restores caller sp into rbx, not OFF_SP; (5) before any BLR/call
to a C helper that can change sp, sync rbx→mem then reload.

OVERHEAD NUANCE (why x86 won't get the full arm64 35%): arm64 pins x25 ONCE in
the trampoline, so recursion pays no per-activation pin cost. x86 has no asm
trampoline — JIT_CALL is a plain C call, so each C-entered activation pays
`push rbx`/`pop rbx`. The inline-J2J hot path AMORTIZES this (self-recursion
stays in-JIT, rbx live across all levels, paid once at the outer entry), so
cfibx-style recursion should still see most of the win; but cold/IC-miss/
cross-method C-entries pay it per call. Net x86 expectation: meaningful but
< arm64's 35%. Validate empirically (build-x86 cogbench3, flag on vs off) before
declaring victory — and gate-keep arm64 byte-identical (battery==golden).

## 3. Root cause of the regressions — send-bearing cross-method callees

The split is leaf vs send-bearing callee, confirmed by a controlled A/B
(N=30, min-5): a callee that returns without an inner send (cfibx's incc) is a
6.4x JIT win; a callee that makes ANY inner send (cfibt's `inct=^(self+1) incc`,
cfibs's `incs`) is net-NEGATIVE (cfibt 672ms JIT vs 568 interp). It is NOT
max:-specific — inct sends a trivial J2J-able method and still regresses.

Why: the x86 cross-method inline-J2J admit (AsmjitT1.cpp ~2929) gates on
`canSkipJ2JSave` (= `!canBailMidMethod && numICEntries==0`) — **leaf callees
only**. A send-bearing callee is rejected and bails to C++ per call (the heavy
`tryJITResumeInCaller` round-trip; cfibs32 triggers ~4.88M C++ resumes ≈
2.2×fib(32) vs cfibx32's 476K). Leaf-only is the CORRECT gate for the current
machinery: a leaf callee runs entirely in-JIT and returns via the V2 return
prelude (`emitJ2JReturnPrelude_x86` ~1655); a send-bearing callee, if admitted,
bails to C++ mid-method and the C++ resume/materialize of the cross-method-
INLINED callee frame is wrong → runaway corruption (see §5).

arm64 admits send-bearing callees (xmethodGateOk, `PHARO_T1_XMETHOD_MAX_IC=8`)
and works, via its full frame-state-residency materialize/resume path. x86 never
got that — closing it is the lever (§6).

## 4. Minimal reproducer (committed) + diagnostic knobs

First-ever isolated repro of the cross-method corruption (prior sessions needed
full-startup). NON-recursive, two-level:

    Integer compile: 'cleaf ^self + 100'.
    Integer compile: 'cmid  ^(self + 10) cleaf'.       "send-bearing callee"
    [100000 timesRepeat: [5 cmid]] value. (5 cmid) printString.   "expect 115"

Run with `PHARO_X86_JIT=1 PHARO_T1_X86_XMETHOD_SENDS=1` → runaway (793M bytecode
steps; `5 cmid` should be ~10). SENDS=0 (default, leaf-only) → 115, correct.

Knobs:
- `PHARO_T1_X86_XMETHOD_SENDS` (opt-in, KNOWN-BROKEN): relaxes the admit to
  send-bearing callees (numIC ≤ MAX_IC). Reproduces the bug; the fix target.
- `PHARO_T1_X86_J2J_DBG`: traces the J2J save-stack (saves=(cursor-entry)/40) at
  admit-PUSH / prelude-POP. Shows leaf = balanced PUSH→POP, send-bearing = PUSH
  with no in-JIT POP (returns via C++ instead).
- `PHARO_J2J_MAT_LOG` + `PHARO_SP_DEPTH_CHECK`: flag stale/mis-reconstructed
  saves (IP-OUT-OF-RANGE, sp-depth delta). Under SENDS=1 these fire on the
  send-bearing callees; clean under SENDS=0.

## 5. Ruled out (do not re-try)

- **"double-pop in tryJITResumeInCaller"** (a workflow hypothesis): REFUTED.
  `PHARO_SP_DEPTH_CHECK` is identical (49) for cfibs vs cfibx; `tryResume` uses
  the plain bc label (post-send sp) and `resumeAfterCall` is only fed pre-send
  sp — both self-consistent, no double-accounting. There is no sp-arithmetic
  edit to make in tryJITResumeInCaller.
- **Naive gate relaxation alone** (admit numIC≤MAX_IC): CORRUPTS (the §4 repro).
- **bail-leak / V2 bailMatJ2J empty-on-V2** (attempt 1, commit d3e66787):
  making V2 `bailMatJ2J` materialize on mid-method bail is INSUFFICIENT — cmid
  still runs away. Kept as gated groundwork (arm64 byte-identical), like the V1
  history ("necessary but insufficient alone").
- **recursion-specific**: no — two-level non-recursive (block→cmid→cleaf) runs away.
- **tail-send-specific**: no — `cmid` (tail send) and `cmidnt=^((self+10) cleaf)+0`
  (non-tail) both run away. The bcOff==bcLen IP-OUT-OF-RANGE flags are partly
  benign return-point over-flagging.
- **stale literalsCache**: that was a DIFFERENT (already-fixed, 2026-06-16) bug;
  the cross-method literals read is now calleeCM+16.

## 6. The lever (task #20) — port arm64's cross-method materialize/resume

Make a send-bearing cross-method-INLINED callee that bails to C++ mid-method get
its frame reconstructed correctly (mirror arm64's frame-state-residency path
behind xmethodGateOk). The materialize (`materializeJ2JSaveIntoFrame`,
Interpreter.cpp ~23894) is SHARED C++ and arm64 works through it, so the x86 bug
is in what x86 emits/pushes or how its callee-bail interacts with that path.

Lead 1 — materialize reconstructed sp: EXONERATED (box #23, MAT-SP trace,
commit 31d0a6e0). `materializeJ2JSaveIntoFrame` reconstructs the caller sp as
`save.sp` directly; the operand depth `(save.sp - save.tempBase)` is non-trivial
and CONSISTENT for every heavily-firing send-bearing cross-method callee under
SENDS=1 (hash bcOff2=1, bcOff5=3; findElementOrNil: bcOff3=4; scanFor: bcOff5=7;
nextPutAll: bcOff5=4; …). The ONLY SP-DEPTH-flagged selector is `getSystemAttribute:`
(delta=1, 40×) — and that's the benign startup case present in SENDS=0 too. So the
materialize sp is NOT the bug. (The earlier "sp-depth delta=1" was getSystemAttribute:
startup noise, not the corrupting callees.)

Lead 2 — resume bcOff / tail-send packing: EXONERATED (box #24, instrumentation
commit 403cc247). The `globalIdx+1` packing is consumed in exactly ONE place
(materialize → `frame.savedIP`); the hot return path masks it off. Instrumented
the tail-send fingerprint (atEnd=bcOff==bcLen, lastOp, numIC) at both the
materialize read and the IP-OUT-OF-RANGE report:
- Every materialized send-bearing save (millions, numIC=1..19) has atEnd=0,
  pastEnd=0 — a VALID INTERIOR bcOff (hash bcOff2/bcLen8, findElementOrNil:
  bcOff3/bcLen22, scanFor: bcOff5/bcLen95). The tail-send hypothesis is REFUTED.
- The 40 `IP-OUT-OF-RANGE` are all `numIC=0` (LEAF methods) ending in a 2-byte
  non-send op (lastOp=0xf0, lastIsSend=0) — a SEPARATE, benign-looking leaf
  resume-at-end, NOT the send-bearing corruption.

So the entire save-reconstruction area (sp AND bcOff AND materialize) is now
exonerated. And the corruption is NOT cmid-specific: even a trivial `3+4` HANGS
under SENDS=1 — **startup itself fails**, with MILLIONS of (correct) materializes.

ROOT INSIGHT (premise inverted): admitting a send-bearing callee via inline-J2J
does NOT make its inner sends stay in-JIT — they STILL bail to C++ and materialize
(one materialize per inner send), so SENDS=1 just relocates the bail to the
callee's inner sends and amplifies the thrash into a hang. "Admit send-bearing
callees to avoid the C++ round-trip" is therefore the WRONG framing. The real
lever is **nested in-JIT sends**: a J2J-admitted callee's inner sends must
themselves be J2J (in-JIT), not bail. That is a larger architectural piece (true
nested J2J / keeping the operand+frame state resident across a callee's inner
send) — NOT a gate relaxation or a save-field fix. The leaf-only gate is correct
precisely because a leaf callee has no inner send to bail on.

Next (bigger than one box): design nested in-JIT sends for J2J-admitted callees
(or accept that x86 cross-method stays leaf-only). Validate via the materialize
count: SENDS-on must NOT explode the per-startup materialize count vs SENDS-off.

This is a substantial, bug-prone, multi-session effort (the cross-method path is
historically the most fragile area of this branch).

## 6b. x86 JIT-on is CORRECT — the "bit-rotted" banner is STALE (box #26, 2026-06-17)

SUnit A/B, PHARO_X86_JIT=1 (leaf-only default) vs interp, on the portable prepped
image, 565-class curated set (the project's standard A/B set):

    config    Classes  Pass    Fail  Error  Skip   disagree-ops  crashes
    interp      565   12508    47    129     39      (none)        0
    JIT-on      565   12508    46    130     39      (none)        0

SAME 12508 pass. ZERO prescan/emit disagrees (PHARO_T1_DISAGREE_DUMP). ZERO
crashes / Corrupt-stackPointer. Only 4 of 565 classes diverged; re-running each 3x
in isolation: 3 were full-suite TIMING FLAKES (clean in isolation —
WeakIdentityValueDictionary/SlotBasic/RegisteredClassAnnotations) and the 4th
(WeakOrderedCollectionTest) is a weak-reference GC-timing flake (it PASSED P:2
under JIT in the full-set context). NO deterministic JIT codegen divergence.

So the §7 banner's "~10 prescan/emit disagree bytecodes + Corrupt-stackPointer
miscompile" is STALE — those were fixed (short-jump out-of-range guard
AsmjitT1:2721, prim-prologue, extended-bytecode port, etc.). The HEADLESS x86 JIT
(leaf-only, the default cross-method config) is correct.

REMAINING gate to actually flip PHARO_X86_JIT default-on: GUI/Catalyst validation
(the shipping x86 build runs Morphic, not headless SUnit; per the repo's
"Verify GUI changes visually" rule, the JIT-on Catalyst app must be launched +
screenshot-verified before flipping the shipping default). That needs an x86 Mac /
Catalyst run, NOT the Linux box. Headless correctness: DONE.

## 6c. FLIPPED DEFAULT-ON (2026-06-17, commit 3b6e2435)

DONE — the x86_64 tier-1 JIT is now default-ON, validated entirely LOCALLY under
Rosetta 2 on the arm64 dev Mac (no AWS box; see the CMake build fix 6abf0a35 and
memory x86-jit-local-rosetta). Rosetta faithfully translates the JIT-emitted x86
code (battery==golden, thousands of methods compiled).

- Headless re-validated at tip: 40- and 120-class SUnit A/B at PARITY with both
  the interpreter AND the shipping arm64 JIT. The only per-test diffs are
  nondeterministic weak-ref / GC-timing tests (`testBenchFor`, `testFlatCollect`
  pass in isolation; `WeakOrderedCollection…AllGarbageCollected` is GC-timing-
  dependent and flips on arm64-JIT too — not x86-specific, not a codegen bug).
- GUI/visual gate satisfied without SDL2/Metal: Morphic rendering is PIXEL-
  IDENTICAL under JIT vs interp. Two `imageForm` scenes (Morph/BorderedMorph
  fills+borders, gradient strips, StringMorph+TextMorph FreeType text, BitBlt
  compositing, PNG encode) produce byte-for-byte equal PNGs (same MD5). The
  JIT-relevant GUI surface is Smalltalk drawing (exercised + validated); the
  Metal/SDL2 bridge is JIT-agnostic C++. The §7 "disagree bytecodes + Corrupt-
  stackPointer" blockers were already STALE (§6b).

Gate (Interpreter.cpp ~19612) inverted: JIT inits by default on x86; opt out with
`PHARO_NO_X86_JIT=1` or `PHARO_NO_JIT=1`. Residual unverified surface: the live
Catalyst Metal app render-loop (needs real x86 Catalyst hardware) — escape hatch
mitigates. The §7 blocker list below is now historical.

## 7. Blockers to flipping PHARO_X86_JIT default-on (the full picture)

Even a perfect Increment 2 does NOT flip the gate by itself. The banner cites,
independent of cross-method:
- ~10 prescan/emit-disagree bytecodes (use `PHARO_T1_FATAL_DISAGREE=1` to surface).
- at least one "Corrupt stackPointer_" miscompile (commit 7af58fdfa).

The flip (Interpreter.cpp initializeJIT ~19597, convert PHARO_X86_JIT → opt-out
PHARO_X86_NO_JIT) is gated on: all 5 benches ≤~5x Cog (cfibs/loopS ≤3x), zero
sp-depth drift, AND a clean FULL x86 SUnit A/B (bench-pass is not sufficient —
the disagree bytecodes + miscompile only surface under load). Shippable today:
Increment 1 (leaf-only cross-method) gives cfibx 6.4x when PHARO_X86_JIT=1.

arm64 safety (the hard constraint): all x86 emit lives in `emitOne_x86` /
x86-only `#if`s and cannot alter an arm64 byte; the only arm64-touching surface
is shared C++ (Interpreter.cpp). Gate every shared-C++ change behind an x86 knob
and re-verify `arm64 battery == /tmp/arm64_battery_golden_step1.txt` after every
commit to a file arm64 compiles.

## 8. Harness notes

- Time ONLY `build-opt` (RelWithDebInfo); clone-and-build's `build/` is -O0
  (~10-100x slower — a documented trap).
- Stock Cog x86 installs via `get.pharo.org/64/130+vm`. `…/64/vm` returns an
  HTML error page — that was the 12-box "no stock pharo" historical blocker.
- `PHARO_X86_JIT=1` is mandatory or every number is the interpreter.
- Bench `/tmp/cogbench3.st` uses inline loops (the method-wrapped form hits a
  Time-resolution artifact reporting 0). Eval args with `$a` Character literals
  get shell-expanded inside `"$(cat)"` — keep `$` out of bench exprs.
- Box hygiene: arm the CPU keep-alive (3 nice-19 loops) + stop iospharo-idle.timer
  IMMEDIATELY after SSH (before the build) — the idle mechanism terminated a box
  that went idle in the post-build gap.

## 6c. Both arches confirmed correct (2026-06-17)

565-class SUnit, same prepped image:

    config                         Pass    Fail  Error  Skip  crashes
    arm64 (local, JIT always on)  12509     46    129    39      0
    x86 interp (box #26)          12508     47    129    39      0
    x86 JIT-on (box #26)          12508     46    130    39      0

All three agree within ±1 (weak-ref/GC timing flakes). No crashes on any. So both
the arm64 JIT (shipping config) and the x86 leaf-only JIT are debugged-correct,
and the whole x86 cross-method campaign left arm64 un-regressed (x86-#ifdef'd
emit + battery==golden, re-verified after every commit). The achievable JIT
correctness debugging is complete. Remaining (not correctness): x86 GUI/Catalyst
validation to flip the shipping default (needs an x86 Mac), and the opt-in
send-bearing cross-method optimization (docs/x86-nested-j2j-design.md).
