# BUG-2 — x86 send-bearing inline-J2J (`PHARO_T1_X86_XMETHOD_SENDS`)

**Goal:** make `PHARO_T1_X86_XMETHOD_SENDS=1` (send-bearing cross-method
inline-J2J) correct on x86, so send-bearing callees inline-J2J instead of
round-tripping through C++ — unlocking the cfibs ~12x speedup. Default-OFF/gated;
the shipped leaf-only x86 JIT is unaffected.

## CRITICAL CORRECTION (2026-06-18d) — the `asString` repro was a SCOPING ARTIFACT

The `asString` corruption that this directory was built around (and that 2-3
prior sessions chased as "BUG-2") is **NOT a real bug**. It is an artifact of the
`PHARO_T1_X86_J2J_SEL=asString` debug knob:

  - `J2J_SEL=asString` enables inline-J2J emission for `Symbol>>asString` but
    compiles its callees (`species`, `size`, `new:`, `replaceFrom:to:with:start…`)
    WITHOUT inline-J2J. The mismatch (caller-inline-J2J / callee-NOT-inline-J2J)
    is what corrupts — `Symbol>>asString`'s `self species` resume returns
    `ByteString` (the class, = `Symbol>>species`'s result) instead of continuing
    to `new:`, so `^tmp1` is the class → `#capitalized`/`#asForm` DNU.
  - PROOF (deterministic, det-sched): scoping inline-J2J to the caller AND ALL
    its callees makes it clean, with SENDS on OR off:

        SCOPE="asString,species,size,new:,replaceFrom:to:with:startingAt:"
        J2J_SEL=$SCOPE                      SENDS off -> EVAL-RESULT=7   (clean)
        J2J_SEL=$SCOPE  XMETHOD_SENDS=1     SENDS on  -> EVAL-RESULT=7   (clean)
        J2J_SEL=asString J2J_CLASS=Symbol   SENDS off -> #capitalized DNU (artifact)

    (The comma-separated `J2J_SEL` list and the `PHARO_T1_X86_J2J_CLASS` bisect
    knob were added this session for exactly this isolation.)
  - Full default config (everything inline-J2J, SENDS off) is CLEAN — the
    mismatch never occurs in production, only under the partial-scope debug knob.

DO NOT re-chase `asString`. The prior "orphaned save / route-through-rj2j /
codeOffsetForResume" hypotheses were all chasing this artifact and are DEAD. The
`PHARO_T1_X86_XMETHOD_PROBE` leak detector confirms the J2J save BALANCES
(`j2jDepth == j2jEntryDepth`) on the asString path — there is no orphan/leak.

## THE REAL BUG — full config + `SENDS=1` corrupts the menu path

Faithful, deterministic repro (no scoping):

    env PHARO_T1_X86_XMETHOD_SENDS=1 PHARO_X86_JIT=1 PHARO_DET_SCHED=1 \
        PHARO_MAX_STEPS=2000000000000 \
        build-x86/test_load_image /tmp/harness/Pharo.image eval "3 + 4"

→ `#asForm` DNU on a `PragmaMenuAndShortcutRegistrationItem`, call stack:

    ToggleMenuItemMorph>>icon -> MenubarMenuMorph>>ifNotNil: -> Array>>do:
      -> MenubarMenuMorph>>layoutItems -> PluggableMenuSpec>>asMenubarMenuMorph
      -> ByteSymbol>>cull: -> PluggableMenuSpec>>ifNotNil:
      -> MenubarMorph>>ifTrue: -> OrderedCollection>>do: ... #asForm

This is the genuine send-bearing inline-J2J defect: a NON-LEAF inline-J2J'd
callee does its own send (exits to C++ mid-callee), and the mid-callee resume
swaps a value/receiver. `NO_XMETHOD` (self-rec only) is CLEAN, so it is in the
CROSS-METHOD admit; `NO_XMETHOD_ALLARGS` does not clear it.

## REAL BUG ROOT-CAUSED (2026-06-18e) — nArgs>0 send-bearing callee whose own send exits to C++

The full-config `SENDS=1` `#asForm` corruption is now fully root-caused (mismatch-
free hash-bisect over all callers via `PHARO_T1_X86_SENDS_HMOD/_HVAL`, then the
`SENDS_SEL`/`SENDS_CLASS` knobs):

- **Corrupting caller: `PragmaMenuAndShortcutRegistrationItem>>icon:`**
  `icon: arg1   ^ self iconFormSet: (arg1 ifNotNil: [ FormSet form: arg1 ])`
- **Send-bearing callee: `FormSet class>>form: arg1  ^ self forms: { arg1 }`** (nArgs=1).
- DECISIVE BISECTS: `SENDS_SEL=icon:` corrupts; `SENDS_SEL=form:`/`forms:` clean;
  `icon:` + `NO_XMETHOD_ALLARGS=1` (nArgs>0 NOT admitted) clean. So the trigger is
  precisely **nArgs>0 AND send-bearing** cross-method inline-J2J.

MECHANISM (evidence chain below): `icon:` inline-J2Js `form:` (nArgs=1) and pushes
ONE J2J save (state.j2jDepth=1). `form:` is send-bearing — it sends `forms:`. Under
`SENDS_SEL=icon:`, `form:` uses the LEAF admit, and `forms:` is itself send-bearing
(`^self extent: arg1 first extent depth: arg1 first depth forms: arg1`) so it is NOT
leaf-admittable → `form:`'s `forms:` send EXITS to C++ (ExitSendCached) **while
icon:'s J2J save is still pending (depth=1)**. The C++ mid-callee resume of `form:`
(a send-bearing callee inline-J2J'd into icon:, with icon:'s receiver `self_icon`
sitting on icon:'s operand stack BELOW the form: send) is where the swap happens:
`self_icon` leaks into form:'s arg/result, so eventually `#asForm` is sent to
`self_icon` (a PragmaMenuAndShortcutRegistrationItem) → DNU.

EVIDENCE (all committed knobs/probes):
- `[J2J-DBG]` (PHARO_T1_X86_J2J_DBG): depth NEVER exceeds 1 → `form:` does not push
  its own in-JIT save; its `forms:` send round-trips C++ with icon:'s save pending.
- `[MAT-STK]` (PHARO_J2J_MAT_LOG, added this session): icon:'s MATERIALIZED stack is
  CORRECT — `[self_icon, FormSet class, arg1=Form]`, retSlot = FormSet slot. So the
  swap is NOT in icon:'s materialize; the save BALANCES (PHARO_T1_X86_XMETHOD_PROBE
  shows no leak). The in-JIT prelude + resumeAfterCall arg-pop math is also correct
  for the simple return. => the defect is specifically the C++ mid-resume of the
  send-bearing callee `form:` whose own send exited to C++ with the outer save live.

CANDIDATE FIX DIRECTIONS (from a 4-lens + verify workflow; two refuted by runtime
evidence, two survive — confirm empirically with `scripts/bug2-asString-repro/run.sh`
`real` + `verify_bug2_fix.sh`):
- REFUTED: nil-fill newSp `+8*nArgs` (AsmjitT1 ~3246) — `tempCount` already INCLUDES
  args (Spur numTemps, JITCompiler ~2547), so the existing `tempBase+tempCount*8` is
  correct; adding nArgs double-counts.
- REFUTED: materializedRetSlot off-by-one (Interpreter ~24154) — the MAT-STK dump
  shows retSlot correctly = form:'s receiver (FormSet) slot.
- SURVIVING: the sp restoration when a send-bearing callee returns to its caller
  AFTER its own inner send round-tripped C++ — the precomputed-resume / chain-loop
  that resumes `form:` mid-method (Interpreter ~26840-27390, esp. the cursor/depth
  reset ~27034/27277 documented as the orphan site) vs the V2 return prelude
  (AsmjitT1 ~1760-1800) + resumeAfterCall (~3526). Mirror arm64 (~6733-7060), which
  keeps sp/cursor in callee-saved regs and is correct for this case.

## THE PERF PREMISE IS INVALID — SENDS does not deliver the cfibs win

Measured this session (clean-scope, NO mismatch artifact), cfibs28
[`incs ^(self+1) max: 0`; `cfibs ^…((self-1)cfibs+(self-2)cfibs) incs`]:

    SENDS off -> 171 ms     SENDS on -> 184 ms     (FLAT, within noise)
    cfibx (incc `^self+1`, a LEAF cross-method callee) -> 13 ms

So the 13x cfibs gap is INTRINSIC: `incs` is send-bearing (calls `max:`, which
bails mid-method), and the admit gate correctly EXCLUDES mid-bailing callees
(`x86HasMidBail`) — `incs` is NOT inlined into `cfibs` even with SENDS=1. Fixing
the real menu-path corruption would NOT close the cfibs gap. RECOMMENDATION: do
not pursue SENDS — it is correctly default-OFF; the shipped leaf + leaf-cross-
method x86 JIT (default-on) already captures the inlinable recursion win.

## If you do isolate the real menu-path bug anyway

It is a genuine (separate) correctness defect, but with no perf payoff. Use the
clean-scope technique: comma-list `J2J_SEL` over the menu-chain caller(s)
(`icon`/`cull:`/`ifNotNil:`/`do:`/`layoutItems`/`asMenubarMenuMorph`) AND their
callees (no mismatch), confirm SENDS-off clean / SENDS-on corrupt at that scope,
narrow. The defect is the MID-CALLEE resume of a send-bearing callee — compare
the leaf path (works) vs send-bearing at the fast-rj2j entryDepth/entryCursor pin
(Interpreter.cpp ~25024-25035) and `materializeJ2JSaveIntoFrame` (~23936).

## Tooling added this session (all x86-only, gated; arm64 untouched)

- `PHARO_T1_X86_J2J_SEL` now accepts a COMMA-SEPARATED exact list (scope a caller
  + all callees → no mismatch artifact). Single trailing `*` still = prefix.
- `PHARO_T1_X86_J2J_CLASS=<ClassName>` — additionally restrict emission to one
  defining class (bisect which impl of an overloaded selector corrupts).
- `[J2J-EMIT]` trace (gated `PHARO_J2J_NEST_TRACE`) names each method getting
  inline-J2J emission (class + sel + nLits).

## Key code locations

- x86 send emit / cross-method admit / resumeAfterCall: `AsmjitT1.cpp` ~2951,
  ~3155, ~3526; comma-list/class scope ~10874-10905.
- fast-rj2j chain loop + entryDepth/cursor pin: `Interpreter.cpp` ~24829-25035.
- materialize: `Interpreter.cpp` ~23936 (`materializeJ2JSaveIntoFrame`),
  j2jBase-materialize site ~25196.
- Memory: `jit-x86-sendbearing-two-bugs`, `jit-x86-xmethod-receiver-corruption`.
