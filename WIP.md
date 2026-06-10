# WIP — JIT correctness + Cog-speed (resume after reboot)

Goal (active /goal): **fix this jit to work and be as fast as cog.**
Branch: `jit`. All work below is COMMITTED (clean tree except pre-existing
`third_party/asmjit` submodule diff + untracked `build-opt/`, `scripts/pharo-headless-test`).
Build: `cmake --build build-opt`. Test VM: `./build-opt/test_load_image /tmp/harness/Pharo.image eval "<expr>"`.
Stock Cog baseline: `/tmp/harness/pharo /tmp/harness/Pharo.image eval "<expr>"`.

## DONE this session (committed)

1. **inline-J2J #extent crash FIXED** (the 5-month blocker). Root cause: the
   inline-J2J send emit's dispatch-A (AsmjitT1.cpp ~3710-3780) routed to six
   UNVALIDATED "extra" inline specs (IC bits 51-58: multiSlot/returnsLiteral/
   tempReturn/intCmp/intArith/evenOdd) the validated paths never use;
   `tryMultiSlot` writes a wild receiver -> #extent crash. Fix: those 6 are now
   DEFAULT-OFF opt-in knobs in `src/vm/debug_vars.h`
   (`DEBUG_BOOL(PHARO_T1_INLINE_MULTISLOT)` etc.), read via `GET_DEBUG_BOOL` at
   the AsmjitT1.cpp emit sites. (NOTE: earlier I wrongly used `envPresent` in
   DebugSettings.cpp — user corrected; now uses debug_vars.h per the rules.)
   NOT a GC bug, NOT the J2J save/return mechanism (both rigorously ruled out
   earlier — see docs/jit-retrospective.md). Validated: `PHARO_T1_INLINE_J2J=1`
   -> 3+4=7, factorial/gcd match default; `PHARO_T1_INLINE_MULTISLOT=1`
   re-reproduces the crash (knob wired correctly).

2. **Hot methods now JIT-compile** (was: most failed -> interpreted -> slow):
   - Per-method emit buffer was `bcLen*512+512` (too small; send bytecodes emit
     ~900 B each) -> raised to `bcLen*1536+4096` + grow-and-retry on overflow
     (AsmjitT1.cpp ~6850). Buffer overflows 844 -> 0.
   - Code zone 16 MB filled (asmjit-T1 ~6 KB/method) -> raised to 64 MB
     (`JITConfig.hpp DefaultCodeZoneSize`) so methods compiled late (e.g.
     benchFib during an eval) can allocate.

## Cog-speed MAP (validated; docs/jit-retrospective.md "Cog-speed MAP")

benchFib / cfib timing, `Time millisecondsToRun:` warmed, custom(inline-J2J) vs Cog:
- tight inlined loop (20M): custom 25ms vs Cog 50ms -> **custom 2x FASTER**
- self-recursion benchFib(30): custom 30ms vs Cog 5ms -> 6x
- +1 CROSS-METHOD call/node cfib(30): custom 344ms vs Cog 8ms -> **~43x**

**The entire real-code Cog gap is CROSS-METHOD send throughput** (~90 ns/send via
the JIT->C++->JIT trampoline activation). Custom beats Cog on compute; self-rec
sends are 6x (inline-J2J self-rec works); cross-method sends are ~43-49x because
they get **NO bit-60 J2J fill** and fall to the slow activation path.

## EXACT NEXT STEP (where I was when interrupted)

Investigating **why cfib->incc (a cross-method send) never gets a bit-60 J2J fill**
(`PHARO_J2J_LOG_FILL=1` shows fills-to-incc=0 across 1.3M calls), while
cross-method J2J DOES engage for ~4026 startup pairs. Ruled out: classification
(non-classified `^self+1+1` helper also gets 0 fills), the numICEntries==0 gate
(helpers have no sends), and SISTA_BIT (t1InlineSistaCall default-off; NO_SISTA
didn't help — 313ms, still 0 fills).

Was reading the IC-upgrade path `upgradeICToJ2J` at **Interpreter.cpp ~21102-21142**:
it only upgrades an IC entry to bit-60 when `extra == 0` (line 21114) and the
callee `target` is compiled NOW. PRIME HYPOTHESIS: the cfib->incc IC is patched
BEFORE incc compiles (so it gets a non-bit-60 `extra` != 0, e.g. a stale/quickPrim
or plain cached entry), and is then NEVER re-upgraded once incc compiles ->
cross-method send stuck on the trampoline path forever. Confirm + fix:

1. Instrument the bit-60 fill (Interpreter.cpp ~20779) AND upgradeICToJ2J
   (~21102) to log, for caller=cfib / selector=incc, WHICH condition blocks the
   bit-60 set (target not compiled yet? extra!=0 so upgrade skipped? banned?).
   Use a gated `debug_vars.h` knob (DEBUG_BOOL) — NOT envPresent.
2. Likely fix: when a method finishes JIT-compiling, UPGRADE existing IC entries
   that point to it (extra==0 OR re-resolvable) to bit-60; OR on IC-hit, if the
   entry lacks bit-60 but the callee is now compiled + J2J-eligible, upgrade it.
   This turns cross-method sends from ~90ns (trampoline) toward the 6x self-rec
   path -> the big Cog-speed win.
3. CAUTION: cross-method J2J was just made correct via the 6-spec fix; validate
   every change with: cfib A/B (target: 344ms -> toward ~40-60ms), benchFib
   correctness, 3+4=7, and a diverse-eval correctness batch.

Repro for the cross-method bench:
```
| t | Integer compile: 'incc ^self + 1'.
Integer compile: 'cfib ^self < 2 ifTrue: [1] ifFalse: [((self - 1) cfib + (self - 2) cfib) incc]'.
28 cfib. 28 cfib. t := Time millisecondsToRun: [30 cfib]. 'ms=', t printString
```
Run with `PHARO_T1_INLINE_J2J=1 PHARO_J2J_LOG_FILL=1` and grep `-> incc`.

## Remaining Cog-parity levers (docs/jit-retrospective.md)
- (a) IC-upgrade-to-bit-60 timing (above) — likely the dominant cross-method cause.
- (b) reconcile Sista (J2J-s) vs J2J (bit-60) dispatch.
- (c) relax the numICEntries==0 cross-method gate to admit callees-with-sends
  (needs nested-save correctness — careful).
- (d) compact the per-send emit (out-of-line dispatch) to shrink ~6KB/method
  bloat so 16MB zone suffices without the 64MB bump.
- (e) inline-J2J default-on after SUnit/GUI validation (it's the recursion win).

## Rules reminders (bit me this session)
- New env knobs -> `src/vm/debug_vars.h` (DEBUG_BOOL/INT/STR + GET_DEBUG_*), NOT
  `envPresent`/getenv in DebugSettings.cpp. Read via `GET_DEBUG_BOOL(NAME)`.
- EVAL-RESULT prints to **stderr** — capture with `2>&1`, not `2>/dev/null`.
- Use `PHARO_DET_SCHED=1` to make JIT timing bugs deterministic/lldb-able.
