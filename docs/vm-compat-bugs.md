# VM-compatibility bugs (our VM fails, Cog passes) — 2026-06-01

These are NOT JIT bugs and NOT image/environment gaps. Verified against stock
Cog (Pharo 10.3.9) on the SAME image our VM uses. Earlier docs wrongly called
the suite's ~5000 errors a "VM-compat ceiling, not fixable" — that was based on
comparing JIT vs our own interpreter, which cannot separate our-VM bugs from
image issues. The Cog comparison shows many are real, fixable VM defects.

## Method: distinguish our-VM bug from image issue

    cp Pharo-jit.image X.image                     # same image
    /tmp/harness/pharo --headless X.image eval \
      "(SomeTest selector: #someTest) run errorCount printString"   # Cog
    # vs our VM via the SUnit runner (PHARO_NO_JIT=1 to exclude JIT)
    # Cog passes + ours errors  => OUR VM BUG (fixable)
    # both fail                 => image/env (out of scope)

## Confirmed our-VM bugs (Cog passes, ours errors)

    class                   Cog          our VM      notes
    SystemEnvironmentTest   217P/0/0     79P/138E    boolean mis-eval (below)
    TraitTest               54P/0/0      errors      not yet drilled

## Out of scope (Cog also fails)

    ZnClientTest    network (no sockets in sandbox)
    StDebuggerTest  debugger/UI (Cog 58E too)

## Root-cause progress: SystemEnvironmentTest NonBooleanReceiver

The 138 errors are all ONE failure, raised as a side effect when our SUnit
harness fires package-change announcements. The exact failing test
`SystemEnvironmentTest>>testCollectThenSelectOnEmpty` passes on Cog directly
(1 run / 0 err); errors on ours.

Trigger: `IceSystemEventListener class>>handlePackagesChange:`, innermost block:

    57 pushTemp:0 inVectorAt:2    ; tmp2 (starts false)
    60 pushTemp:0                 ; arg3
    61 send: isNotNil
    62 jumpFalse: 68              ; and:
    63 pushTemp:1                 ; arg2  (an IceLibgitRepository)
    64 pushTemp:0; 65 send: name
    66 send: notifyPackageModified:
    67 jumpTo: 69
    68 pushConstant: false
    69 send: |                    ; tmp2 | <result>
    70 storeIntoTemp:0 inVectorAt:2
    ...later... tmp2 ifTrue:[...]  ; <-- NonBooleanReceiver here

MUSTBOOL diagnostic: the value reaching `ifTrue:` is the IceLibgitRepository
(arg2 / `value_class=IceLibgitRepository`). Since `tmp2` starts false and
`false | X = X`, the `|` send returned `arg2` — i.e. `notifyPackageModified:`
(which delegates `^ self workingCopy notifyPackageModified: arg1`, IceRepository)
returned `self` instead of a boolean ON OUR VM. Cog returns the boolean.

=> Somewhere in the notifyPackageModified: delegation chain, a method `^ expr`
(returnTop) yields the receiver instead of expr on our VM — a likely NLR /
return-value / method-return bug in the interpreter (NOT JIT: reproduces with
PHARO_NO_JIT=1). This matches the repo's known "fb(N) returns receiver" /
materialize-bytecodeEnd family (see memory jit_materialize_bytecodeend_bug,
but that one was JIT; this is interp).

NEXT: get a deterministic single-method repro (the SUnit method-filter
/tmp/sunit_method_names.txt is ignored by the runner; need another isolation
path) then trace the returnTop that yields self. Then the fix is in the
interpreter's method-return path, verifiable against Cog as oracle.

## UPDATE (2026-06-01): deterministic harness overturns the SystemEnvironmentTest verdict

Built `run_one_test.st` (shared headless repo) + `scripts/run_one_test.sh` to run
ONE method in true isolation on both VMs. Result for the test I had called a VM bug:

    COG   : PASS SystemEnvironmentTest>>testCollectThenSelectOnEmpty
    OURS* : PASS SystemEnvironmentTest>>testCollectThenSelectOnEmpty   (* = interp)

So the test **passes on our VM in isolation** — same as Cog. The 138
`NonBooleanReceiver` errors are therefore NOT a per-method VM bug: they are a
**full-suite harness-interaction artifact**. Inside run_sunit_tests.st the test
classes trigger Iceberg package-change announcements (the harness installs GUI /
Morphic / package machinery), and `IceSystemEventListener>>handlePackagesChange:`
mis-evaluates only under that accumulated shared state. Run alone, no listener
fires, and the boolean is correct.

This corrects the previous section: SystemEnvironmentTest is NOT a fixable VM
primitive bug. It is a harness/shared-state effect — the same class of issue as
the suite's cumulative-state errors. The cross-VM comparison earlier (217/0/0 on
Cog vs 138E ours) compared `cls suite run` on Cog (no harness side effects) vs our
FULL harness — not apples to apples. The apples-to-apples single-method run is
PASS/PASS.

LESSON: to call something a VM bug, run the SAME isolation on both VMs.
`scripts/run_one_test.sh` is that tool. Re-triage the other "VM bug" candidates
(TraitTest etc.) with it before assuming a primitive gap. The genuinely
VM-specific, reproducible bug found this campaign remains the JIT IC-probe /
inline-primAt(size) one (RGMethodDefinitionTest) — but note even that should be
re-checked with run_one_test once a single failing RG method can be isolated.
