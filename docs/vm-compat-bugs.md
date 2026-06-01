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

## CONFIRMED VM bug: StringTest>>testOnlyLetters (2026-06-01)

Deterministic, both VMs, single-method isolation (run_one_test on the clean
image /tmp/harness/Pharo-clean.image):

    COG : PASS StringTest>>testOnlyLetters
    OURS: FAIL StringTest>>testOnlyLetters    (PHARO_NO_JIT=1, interpreter)

This is a REAL VM bug (passes Cog, fails ours, same image, one method, isolated).
Not JIT-specific (reproduced interp-only). `String>>onlyLetters` is
`^ self select: [:c | c isLetter]`, so the defect is in our `Character>>isLetter`
(Unicode classification) or `String>>select:`.

Cog reference: `'abc98def' onlyLetters = 'abcdef'`; `$8 isLetter=false`,
`$a isLetter=true`. The test asserts digits/spaces are dropped:
  'abc98def' onlyLetters = 'abcdef'
  'abc 98 12 def' onlyLetters = 'abcdef'
  '012  345' onlyLetters = ''
If our VM keeps a digit or drops a letter, one isLetter class is wrong. The
Unicode path (GeneralCategory SparseLargeTable) was fixed once for the
SUnit-blocking bug (docs/image_issues style); this is a residual classification
error for some character(s) in the test's input.

Sibling COGPASS-OURSFAIL leads (same StringTest, not yet drilled):
  testWithUnixLineEndings, testWithInternalLineEndings — both include WideString
  cases (WideString with: 403 asCharacter ...), the known WideString-family weak
  spot; likely a separate WideString bug, not the same isLetter one.

NEXT: probe `'abc98def' onlyLetters` on our VM to see the exact wrong char
(prep was flaky this session — the stock-pharo --save intermittently hangs/errs,
unrelated to the VM bug). Then fix isLetter for that codepoint and verify with
  COG=1 NOJIT=1 scripts/run_one_test.sh 'StringTest>>testOnlyLetters'
expecting OURS: PASS.

## Triage status (kernel candidates, scripts/triage_one_tests.sh)

First 8 kernel candidates classified:
  COGPASS-OURSFAIL (real VM bugs): StringTest testOnlyLetters,
    testWithInternalLineEndings, testWithUnixLineEndings
  BOTHFAIL (image/env, out of scope): BlockClosureTest testIsClean,
    testSourceNodeOptimized; ContextTest testMethodContextPrintDetails,
    testReadVariableNamed, testTempNamed
So even among kernel candidates, most "failures" are image/env (fail on Cog too);
the StringTest/isLetter + WideString ones are the genuine VM bugs to fix.

## DEEP DIVE: testOnlyLetters fails only via compiled-method execution (2026-06-01)

Confirmed apples-to-apples (identical probe, same image, our VM vs Cog):
    (StringTest selector: #testOnlyLetters) run
      Cog : failCount=0  (PASS)
      OURS: failCount=1  (FAIL)   -- interp (PHARO_NO_JIT=1) AND JIT both fail

But EVERY constituent operation is correct on our VM when run directly:
  - all 5 `onlyLetters` results correct: abc98def->abcdef, '012 345'->'', etc.
  - all 5 equality asserts true: ('abc98def' onlyLetters = 'abcdef') = true
  - printString of every result clean: 'abcdef', '' (no corruption)
  - assert:equals: called manually on a fresh TestCase: all PASS (lit-lit, ol-lit, empty)
  - setUp: ok

The failure appears ONLY when the **compiled testOnlyLetters bytecode** runs via
performTest/runCase. The TestFailure message is `Got '<corrupt>'` where <corrupt>
is raw oop words (tag-3 Character oops + a SmallFloat-tagged word 0x8bdd), i.e.
`assert:equals:` saw a CORRUPT actual value — even though the identical
`onlyLetters` call in a fresh frame returns the correct String (verified: right
after the failure, `'abc98def' onlyLetters` still prints `abcdef`).

bytecode (numLits=11): for each of 5 asserts:
    self; pushConstant: <input>; send: onlyLetters; pushConstant: <expected>;
    send: assert:equals:; pop

=> This is an INTERPRETER method-execution bug (reproduces with JIT off): in this
specific compiled-method frame, the value returned by `onlyLetters` (or left on
the stack between `send: onlyLetters` and `send: assert:equals:`) is corrupted —
a stale/wrong oop on the operand stack. The same send in an isolated frame is
fine. Classic "returns wrong object on the stack" / frame-stack-slot corruption,
not a String/isLetter/select: primitive defect (all of those are correct).

This is a REAL, isolated, deterministic VM bug (COG PASS / OURS FAIL, same image,
single method, both JIT and interp). It is NOT the same class as the
SystemEnvironmentTest harness artifact: here the failure reproduces via a bare
`(X selector: #m) run` with no full-suite harness.

NEXT: trace the operand stack across `send: onlyLetters -> send: assert:equals:`
in the testOnlyLetters frame (PHARO_SLOT_TRIPWIRE / SP-corrupt traces, or lldb on
the assert:equals: entry comparing arg vs the value onlyLetters returned). The
corrupt word 0x8bdd recurring is a fingerprint to grep for. Repro:
  (deterministic, ~5s) install a probe class or use scripts/run_one_test.sh
  COG=1 NOJIT=1 scripts/run_one_test.sh 'StringTest>>testOnlyLetters'  (OURS:FAIL)

## ROOT CAUSE: WriteStream on WideString is broken (2026-06-01)

testOnlyLetters fails because it asserts on a WideString case
(literal[6] = '012 àôüÖ ẞ 345', codepoints 233,224,244,252,214,7838).
Drilled with deterministic probes to a ONE-LINE repro:

    (WideString with: 233 asCharacter with: 224 asCharacter) select: [:c | c isLetter]
      Cog  -> WideString (233 224)
      OURS -> WideString (1867 0)        <- 1867 = (233<<3)|3 = the Character OOP!

So select: stores the raw 64-bit Character OOP across two 32-bit slots instead of
the codepoint. Narrowed the mechanism — it is NOT isLetter, at:put:, copyFrom:,
or comma, all of which are correct on WideString:

    isLetter(233/224/...) = true          (correct)
    WideString at:put: (ws at:i)          -> 233,224   (correct)
    ws copyFrom: 1 to: 2                   -> 233,224   (correct)
    ws , otherWide                         -> correct
    WideString>>select: / WriteStream:     -> CORRUPT

The culprit is **WriteStream on a WideString** (select: builds via a WriteStream):

    | s | s := WriteStream on: (WideString new: 0).
    (WideString with: 233 asCharacter with: 224 asCharacter) do: [:c | s nextPut: c].
    s contents
      Cog  -> WideString (233 224)
      OURS -> ByteString  (233 0)         <- WRONG class AND truncated/corrupt

=> On our VM, a WriteStream built on a WideString collapses to a ByteString and
mis-stores wide characters. select: (Collection>>select: uses a species
WriteStream) inherits this, so any String operation that filters/streams a
WideString (select:, collect:, reject:, onlyLetters, withUnixLineEndings, etc.)
corrupts. This is the underlying defect behind the StringTest COGPASS-OURSFAIL
cluster (testOnlyLetters, testWith{Unix,Internal}LineEndings) and likely many
other WideString-touching failures across the suite.

LIKELY FIX SITE: WriteStream's grow / pastEndPut: path on a non-byte (32-bit)
backing collection — the VM primitive it uses (new:/species/at:put:-past-end or
the grow that allocates a ByteString instead of preserving WideString format).
The `species`/`new:` returned WideString correctly in isolation, so the bug is in
WriteStream's grow allocating/replacing with the wrong format, or pastEndPut:
storing into a byte buffer. Reproduce + verify the fix with the one-liner above
(expect OURS -> WideString 233 224), then re-run:
    COG=1 NOJIT=1 scripts/run_one_test.sh 'StringTest>>testOnlyLetters'  (expect OURS: PASS)

## FIXED (2026-06-01): synthetic WriteStream>>nextPut: corrupted WideString

The above "LIKELY FIX SITE" guesses (grow/species/pastEndPut:) were all WRONG —
the real culprit was a missing format guard in the **synthetic-primitive**
`primitiveWSNextPut` (Primitives.cpp), our inlined C fast path for
`WriteStream>>nextPut:` (installed at cacheMethod time, dispatched BEFORE the
declared prim 64, so it never showed up tracing prim 64).

Chain that found it: `at:put:` on the WideString never reached prim 64
(primitiveStringAtPut) — verified by entry-trace, prim 64 fired 8873× but ONLY
for ByteString, never fmt 10/11. The store went through the synthetic
`WriteStream>>nextPut:` path (sendSelector's `primIdx==0 && cached->primitive`
synthetic-prim dispatch, Interpreter.cpp:8395).

`primitiveWSNextPut` does `memory_.storePointer(pos, coll, arg)` — a raw-Oop
store, valid ONLY for Array-backed (pointer) WriteStreams (the `Array
streamContents:` bench fast path it was written for). It bailed for
`isBytesObject()` (ByteString) but NOT for WideString/WordArray (fmt 10-11, which
are neither bytes nor pointers). So it stored the raw Character Oop 1867
(=(233<<3)|3) into the 32-bit word instead of the codepoint 233.

FIX: replace the `isBytesObject()` bail with `!isPointersObject()` — only genuine
pointer Arrays take the fast path; WideString/WordArray/ByteString all fall
through to their real `at:put:` method (prim 64 etc., which extract the codepoint).

Also hardened prim 60/61 (`primitiveAtPut`) format-10/11 branch to accept a
Character value (store its codepoint), matching Cog — so a direct
`wideString basicAt: i put: aChar` no longer raises "Improper store".

Verified (clean image, both VMs, single-method isolation):
    one-liner: (WideString with: 233 asCharacter with: 224 asCharacter)
               select: [:c | c isLetter]   OURS -> WideString 233 224  (was 1867 0)
    COG=1 NOJIT=1 scripts/run_one_test.sh 'StringTest>>testOnlyLetters'
      COG : PASS   OURS: PASS   (was OURS: FAIL)

CLUSTER REPAIRED — full StringTest class on our VM (interp): **438 P / 0 F / 0 E**
(was failing testOnlyLetters, testWithUnixLineEndings, testWithInternalLineEndings).
testOnlyLetters also PASS under JIT ON and JIT OFF (single-method isolation:
passed=1 failed=0 errors=0 both ways). One guard fix cleared the whole
WideString-streaming cluster. Committed d5608fd4.

GENERAL LESSON: synthetic primitives (installed at cacheMethod time, dispatched
in sendSelector BEFORE the declared `<primitive: N>`) silently shadow the real
primitive — tracing prim N will NOT show the call. When a store/format bug
"can't reach" the primitive you expect, check Interpreter.cpp:8395 synthetic-prim
dispatch and the primitiveWS*/primitiveOC* fast paths. Any raw-Oop fast path must
guard `isPointersObject()`, not merely `!isBytesObject()` — WideString/WordArray
(fmt 10-11) are neither bytes nor pointers and slip through a `!isBytesObject()`
check.
