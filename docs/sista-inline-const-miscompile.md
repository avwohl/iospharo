# Blocker #4 root cause: speculative-inline miscompile in the optimized tiers

Status: ROOT-CAUSED + deterministically reproducible, NOT yet fixed.
Date: 2026-06-05.

## Summary

The full-SUnit "blocker #4" (the SystemEnvironment / Package error-heavy region
~class 500 wedging the runner, previously described as a broad nondeterministic
value-corruption heisenbug) is a **speculative-inline miscompile in the optimized
execution tiers**. The pure interpreter is correct; both optimized tiers miscompile
under heavy class-create/recompile/remove load:

- **Sista tier:** `tryInlineConstReturn` (the inline-const-return speculative
  inliner, `src/vm/jit/sista/SistaBuilder.cpp`). `PHARO_SISTA_NO_INLINE_CONST=1`
  fixes it DETERMINISTICALLY.
- **T1 JIT tier:** the inline-cache probe. `PHARO_T1_NO_IC_PROBE=1` fixes the
  JIT-only variant.
- With BOTH tiers on (the real suite config) there is an additional interaction;
  neither single knob fully fixes the JIT+Sista combination.

The earlier memory note "pure interpreter (`PHARO_NO_JIT=1`) reproduces" was a
**false lead**: `PHARO_NO_JIT=1` does NOT disable Sista — `sistaDispatch` defaults
ON on arm64 (`DebugSettings.cpp`, `kDefaultSistaOn=true`); only `PHARO_NO_SISTA=1`
turns it off. So every "pure interpreter" repro actually had Sista active.

## Deterministic repro

`/tmp/perrun.st` runs `(PackageOnModelTest selector: #testAddTag) runCase` six
times in one eval and reports pass/fail + error class per run:

    cd /tmp/harness
    PHARO_NO_JIT=1 ./build/test_load_image /tmp/harness/Pharo-sunit-fixed.image \
        eval "$(cat /tmp/perrun.st)"

Config matrix (deterministic; `Pharo-sunit-fixed.image`):

    NO_JIT + Sista-on (default w/ NO_JIT)          P,P,F,F,F,F  (run3 NonBooleanReceiver, run4+ KeyNotFound)
    NO_JIT + NO_SISTA                              P,P,P,P,P,P  (pure interpreter — CLEAN)
    NO_JIT + Sista + PHARO_SISTA_NO_INLINE_CONST   P,P,P,P,P,P  (FIXES the Sista variant; 3/3 trials)
    JIT-on + Sista-on (real suite config)          P,F,F,F,F,F
    JIT-on + NO_SISTA                              alternating P/F (T1 variant)
    JIT-on + NO_SISTA + PHARO_T1_NO_IC_PROBE       P,P,P,P,P,P  (FIXES the T1 variant)
    JIT-on + Sista + NO_IC_PROBE + NO_INLINE_CONST P,P,F        (interaction remains)

`testAddTag` creates 3 packages + 4 classes, compiles 8 methods, then tearDown
`removeFromSystem`s them all — heavy class-create/method-recompile/class-remove,
which is exactly the suite's ~class-500 error-heavy region at small scale.

## What it is NOT (ruled out empirically)

- NOT the class table: `dumpClassTableConsistency()` (PHARO_CTCHECK) reports
  `orphanInstances=0`, no `registerClass` overwrites, every entry's
  `identityHash == index`, all entries valid — classOf/dispatch is sound.
- NOT the heap structure: walks cleanly to the free pointer every run, 0 bad
  headers.
- NOT the interpreter method cache: a full flush (`Object flushCache`, prim 89)
  between runs does NOT fix it.
- NOT symbol-table corruption: `#SystemOrganization identityHash` is constant and
  `'SystemOrganization' asSymbol == #SystemOrganization` holds throughout.
- NOT a GC/scavenge bug: NO GC fires during the runs (young-gen off; the one
  startup GC moves 0 objects).
- NOT stale inline hints at extraction: re-resolving each hint's selector in its
  classKey's class (PHARO_SISTA_VALIDATE_HINTS) drops only ~4 hints and does NOT
  fix the failure.
- NOT a loose class guard: the `kGuardClass` lowering compares the full 22-bit
  class index (`hdr & 0x3FFFFF` vs `expectedIdx`), so when an inline fires the
  receiver genuinely IS the guarded class.

## What it IS

A compile-time miscompile in `tryInlineConstReturn` that **scales with compile
count**: forcing extra recompiles (resetting the Sista compiled-fn cache on every
`flushJITCaches`) made it FAIL EARLIER (run 1), not later. So the bad code is baked
at compile time, and more compiles = more corruption — pointing to **shared
builder-state corruption in the recursive callee-lift** (`Builder::build(calleeOop,
...)` inside `tryInlineConstReturn`), not stale runtime data.

Symptom decomposition (after 2 testAddTag runs): a fresh `SystemEnvironment new
organization` raises KeyNotFound even though, on the same dict + key,
`includesKey:`=true, `findElementOrNil:` returns the correct index, and
`array at: thatIndex` IS the stored association. A manual reconstruction of
`organization` PASSES — only the REAL method's Sista-compiled form is wrong — so
it is the compilation of the actual Dictionary/SystemEnvironment methods that
miscompiles, not the data.

Bisecting shapes: disabling getter (`kLoadInstVar`) inlines at the common-emit
delays the failure run3→run4; disabling const inlines does nothing; only disabling
ALL of `tryInlineConstReturn` (`NO_INLINE_CONST`) fully fixes it — consistent with
a shared-mechanism (recursive-lift) bug rather than one shape.

## Why pure interpreter is correct but the tiers are not

The image never sends `flushCache` (prim 89) for `compile:`/`removeFromSystem`
(measured: 0 calls across 6 runs); it uses the per-method/selector prims 116/119
(23 calls/run, both of which DO clear all JIT ICs). The interpreter's `methodCache_`
keys on the class OOP and never returns wrong methods here; the optimized tiers
bake speculative inlines into compiled code that the IC flush does NOT recompile.

## Localization (2026-06-05, deeper dive — ruled out more)

- NOT the recursive callee-lift state: `Builder::build` constructs a FRESH lifter
  per call operating on the passed `out`, so the outer build's `out_`/`stack_`/
  `currentBlock_` are isolated; the only shared globals (g_currentBuildMemory,
  g_currentBuildHints, g_calleeLiftDepth, g_buildStartBcOffset) are all saved/
  restored.
- NOT the probe-lift side effect: `PHARO_SISTA_ICR_PROBE_ONLY=1` (do the recursive
  probe-lift but NEVER emit an inline) → 12/12 PASS. So the bug is in the EMITTED
  IR, not the probe-lift.
- NOT an out-of-bounds getter: added a soundness check (the inlined ivar index
  must be < the guarded class's instance field count); it never fires here, and
  the failure persists. Getter inlines are layout-consistent (right class,
  in-bounds ivar). The check is kept as a defensive hardening.
- Shape bisection is TIMING-CONFOUNDED: disabling a SUBSET of emissions
  (`ICR_NO_GETTER` / `ICR_NO_COMMON`) only DELAYS the failure (run3→run4) — it
  shifts the per-method compile schedule rather than removing the cause. Only
  disabling ALL emissions (`NO_INLINE_CONST` or `ICR_PROBE_ONLY`) fully fixes it.
  So the failure scales with the NUMBER of inline emissions, consistent with a
  data-dependent bug in the SHARED emission mechanism (the `kGuardClass` emission
  + `stack_` pop/push + deopt framepoint that every emitting shape performs),
  manifesting probabilistically across the compiled-with-inline methods.

## DECISIVE (2026-06-05): it's the guard-HIT inlined VALUE, not the deopt

`PHARO_SISTA_GUARD_ALWAYS_DEOPT=1` (lowering makes every `kGuardClass` compare
against an impossible class index, so EVERY speculative inline misses → deopts to
the real unspeculated send) → 6/6 PASS, deterministically. This is a RELIABLE
oracle (it keeps the IR structure / compile schedule identical, so it is NOT
timing-confounded, unlike the emission-disabling knobs). Conclusions:
- The deopt reconstruction (stack re-push + ip resume) is CORRECT. The earlier
  "deopt stack capture" hypothesis is REFUTED.
- The bug is the INLINED VALUE used on a guard HIT (when the receiver IS the
  guarded class). For some shape, the value emitted does not equal what the real
  send returns for that class.
- Plain getters (kLoadInstVar) are proven correct, so the culprit is a NON-GETTER
  value: kConstantOop (const), kLoadTrue/FalseOop (bool), kLoadReceiver (`^self`),
  kLoadTemp (`^arg`), kLoadLiteral, or a chained shape's final inner value.

## Next step (the real fix)

Audit the VALUE emitted by the non-getter shapes for the case where it differs
from the real send's result for the guarded class. Suspects, by likelihood:
  - kLoadReceiver `^self`/yourself direct emit (SistaBuilder.cpp ~6230-6258): it
    pushes `recvId` (= stack_[size-nArgs-1]); verify recvId is the inlined send's
    receiver for every call shape, not the outer method's self.
  - kLoadTemp `^arg` (~6260-6290) and the literal/temp arg-forwarders
    (~7090-7161): verify the substituted arg/literal index.
  - kConstantOop const-return: verify the literal Oop is read from the correct
    callee literal slot (a wrong literal index would return a wrong constant).
  - chained inner const values (6628 / 7000s).
RELIABLE bisection tool: GUARD_ALWAYS_DEOPT can be made SELECTIVE (tag the guard
with the inlineOp in free bits 22-31 of guardLit and force-deopt only chosen
shapes) — same structure, no timing confound. Then confirm the fix with
scripts/repro/blocker4-perrun.st (PHARO_NO_JIT=1, expect all-PASS).

Interim correctness option: default `tryInlineConstReturn` off
(`sistaNoInlineConst`) and the T1 IC probe off until the emission bug is fixed —
at a perf cost on accessor-heavy code.

## Diagnostics added this session (all opt-in, zero-cost when off)

- `PHARO_CTCHECK` — `registerClass` overwrite log + `ObjectMemory::dumpClassTableConsistency()`
  on `anyClass flushCache`.
- `PHARO_NO_METHOD_CACHE` — force `probeCache` to always miss.
- `PHARO_SISTA_ICR_LOG` — log every `tryInlineConstReturn` emit (callee selector,
  shape, inlineOp, guarded class name).
- `PHARO_SISTA_VALIDATE_HINTS` — opt-in re-resolution filter for stale hints
  (partial; does not fix the miscompile).
