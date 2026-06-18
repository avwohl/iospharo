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

## NEXT STEP — isolate the real menu-path corruptor

Use the clean-scope technique (NO mismatch artifact): enable inline-J2J + SENDS
for the menu-chain caller(s) AND their callees via the comma-list `J2J_SEL`,
then bisect which send-bearing caller's mid-resume swaps the value. Candidate
chain methods: `icon`, `cull:`, `ifNotNil:`, `do:`, `layoutItems`,
`asMenubarMenuMorph`, `ifTrue:`. Add their callees so there is no
caller-J2J/callee-no-J2J mismatch, confirm SENDS-off clean / SENDS-on corrupt at
that scope, then narrow. The defect is the MID-CALLEE resume of a send-bearing
callee — compare the leaf path (works, default-on) vs the send-bearing path at
the fast-rj2j entryDepth/entryCursor pin (Interpreter.cpp ~25024-25035) and the
materialize (`materializeJ2JSaveIntoFrame`, ~23936).

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
