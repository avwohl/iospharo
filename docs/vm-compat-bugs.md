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

## Full-suite re-measurement after WideString fix (2026-06-01)

Run on the now-healthy harness (after deleting the poisoned /tmp/harness/startup.st
and clearing run-state). Our VM, interp (PHARO_NO_JIT=1), 12s/test cap, in two
parts (skipping the unkillable-hang class BehaviorWithCompilerTest at ~357):

    part1 (classes 1-356):  P=10409 F=87  E=208 S=18   (361 class-headers)
    part2 (classes 358-475): P=6536  F=4   E=66  S=7    (119 class-headers)
    COMBINED: 480 classes, 17310 tests, P=16945 F=91 E=274 S=25 => 97.9% pass

Zero regressions; StringTest 438/438 (WideString fix holds).

KEY FINDING: the 91 batch FAILures cluster in class-definition/metamodel tests
(CD*ClassParserTest family ~12 classes each F:2; Slot*; RG* Ring; OpalCompiler;
OCClassBuilder). But EVERY batch failure drilled so far PASSES IN ISOLATION:
  - CharacterTest 19/19 isolated (batch showed 16/19)
  - CollectionArithmeticTest>>testAverageIfEmpty isolated PASS (#() average
    correctly raises CollectionIsEmpty; batch "hang" was exitSuccess/harness)
  - CDNormalClassParserTest 16/16 isolated (batch showed F:2)
  - DurationTest isolated (batch 69/71)
All four are COGPASS (Cog runs the full class clean) and OURS-PASS-ISOLATED.

=> The remaining batch failures are CUMULATIVE-STATE ARTIFACTS, not individual
per-test VM bugs. State from earlier tests corrupts later ones on our VM (Cog
does not exhibit this to the same degree). This confirms the documented lead:
the real remaining bug is long-run heap/string corruption, NOT a list of
fixable per-test defects. Drilling individual batch-failing tests is futile —
they pass alone. The WideString synthetic-prim fix (d5608fd4) was one concrete
instance of such a string-corruption root cause.

Full completion is blocked by a small set of unkillable-hang CLASSES (blocked
processes the Smalltalk watchdog can't preempt): BehaviorWithCompilerTest>>
testContinuationExample2, StopwatchTest, ScheduleTest (latter hangs on Cog too).

NEXT (real lead): hunt the cumulative-state corruption directly — run a long
sequence and bisect which earlier class/test poisons a known-isolated-pass test
(e.g. run [poison-candidate, CDNormalClassParserTest] pairs and see which pairing
flips CD to F:2). That isolates the corrupting operation the way the WideString
bug was isolated.

## NARROWED: the cumulative-state corruption (CDNormalClassParserTest) — 2026-06-01

Drilled the cumulative-state artifact to a deterministic, Cog-divergent VM bug.

DETERMINISTIC REPRO (stable; minimized probes perturb it — use this one):
  Run `CDNormalClassParserTest suite run` repeatedly in ONE image instance.
    iter 1: P16 F0   (pass)
    iter 2..N: P14 F2  — fails testSlotNodesHaveParentReference +
                         testClassNameNodeHaveParentReference
  Cog: stays P16 F0 every iteration (4x verified). => REAL our-VM bug.
  Reproduces under PHARO_NO_JIT=1 (interp) AND PHARO_DET_SCHED=1.

WHAT IT IS NOT (ruled out):
  - NOT data corruption. Inline checks of the exact assertions
    (`slotNode parent == classDefinition`, `classDefinition children includes:
    slotNode`) done in a fresh probe method ALWAYS pass, even on the poisoned
    image. The AST/parse result is correct.
  - NOT the parser. Double-parse identity probe: both parses produce fresh
    nodes with correct parent pointers.
  - NOT a single test. The 2 failing tests, run individually via
    `(X selector: #sel) run` repeatedly, pass every time.
  - NOT bare execution. `t setUp. t performTest` (no runner wrapper) PASSES
    on the poisoned state.

WHAT TRIGGERS IT: running the `testBestNodeFor*` cluster (each does
  `classDefinition bestNodeFor: aSelection`) accumulates state; after ~3-5 such
  runs, the next `(X selector: #testSlot) run` records a SPURIOUS failure.
  The flip point varies run-to-run (timing-sensitive Heisenbug; minimizing
  perturbs it — hence use the suite-2x repro, not the minimized sequence).

NARROWED LOCUS: the failure is introduced by the SUnit runner's execution path
  — `TestCase>>run` -> `TestResult>>runCase:` (nested `on: failure do:` /
  `on: error do:` + `ensure:` around setUp/performTest/tearDown) — NOT by the
  test assertions. Bare `performTest` passes; the runCase exception-handling
  wrapper records a failure that isn't real. So the bug is in the VM's
  exception / NLR / ensure: / handler-context machinery degrading after
  accumulated operations (cf. memories jit_forceyield_reified_thiscontext,
  jit_sim_lookupselector_nlr_recursion — same family).

NEXT (focused lldb session, per CLAUDE.md JIT/sentinel workflow):
  1. Repro: install PB probe that runs `CDNormalClassParserTest suite run` twice,
     under PHARO_NO_JIT=1 PHARO_DET_SCHED=1. 2nd run = deterministic F2.
  2. The spurious failure comes through TestResult>>runCase:'s
     `on: TestResult failure do:` / `on: error do:`. Breakpoint the
     exception-signal / handler-lookup path (Interpreter exception machinery)
     during the 2nd suite run; compare handler-context state vs the 1st run.
  3. Suspect: a stale handler/marker context, a corrupted ensure: block, or a
     GC-moved handler context after accumulated allocations. Check whether
     forcing GC (or huge GC headroom) shifts the flip — GC-moved context is the
     leading hypothesis (data correct, only the runner's unwind miscomputes).
  This is THE "long-run heap corruption" lead; it is exception/context-machinery
  specific, not string/heap-data corruption (that was the WideString bug, fixed).

## ROOT CAUSE: young-gen SCAVENGE mishandles a root (2026-06-01)

The cumulative-state corruption (CDNormalClassParserTest suite degrading on
re-run) is a SCAVENGE bug. One-flag confirmation on the deterministic repro
(CDNormalClassParserTest suite run 5x in one image, PHARO_NO_JIT=1):

    baseline                 : iter1-3 P16F0, iter4 P15F1, iter5 P14F2  (degrades)
    PHARO_YG_NO_SCAVENGE=1   : iter1-5 ALL P16F0                        (BUG GONE)
    PHARO_GC_HEADROOM_MB=2048: iter1 P15F1 already                      (faster; full-GC
                               headroom is irrelevant — confirms it's young-gen
                               scavenge, not full GC)

=> Young-generation scavenge moves (or collects) a young object while a live
reference to it is not updated/scanned — a MISSED SCAVENGE ROOT. The stale/dead
pointer surfaces in the SUnit runner's exception-handling path (TestResult>>
runCase: on:do:/ensure: around setUp/performTest/tearDown), recording a spurious
failure even though the test data and assertions are correct (inline checks pass,
bare performTest passes; only the runCase wrapper miscomputes).

This is almost certainly the general "long-run heap corruption" lead and explains
the broad cumulative-state-artifact class across the full suite (tests pass
isolated, fail in-batch) — accumulated allocations eventually trigger a scavenge
at a point where the missed root matters. Note memory jit_remembered_set_dead:
scavenge does an O(oldSpace) full scan instead of using the (dead) remembered
set, so the missed root is likely a NON-oldSpace, non-stack root the scavenger
forgets (e.g. a handler/marker context, an ensure: block, a VM-held temp, or a
special-objects/root-table entry).

NEXT: audit the scavenge root set (ObjectMemory scavenge / collectYoungSpace).
Enumerate every root source it scans (active stack/contexts, old-space scan,
special objects, JIT/IC tables, VM-held registers like newMethod_/method_/
the exception handler chain) and find the one category of live young object it
fails to forward. The runCase exception machinery points at handler/ensure
contexts as the likely missed root. Repro for the fix loop (deterministic):
    PB probe: CDNormalClassParserTest suite run 5x; expect 5x P16F0 once fixed.
    PHARO_NO_JIT=1 (no PHARO_YG_NO_SCAVENGE) ./build/test_load_image IMG

## CORRECTION + refinement: it's an UNROOTED C++ LOCAL, not a missed heap root (2026-06-01)

Added a post-scavenge diagnostic (PHARO_SCAV_DANGLE_CHECK, ObjectMemory.cpp
scavenge()): before eden reset, scan all of old+perm space AND the format-9
roots pointerSlotsOf() skips (hiddenRoots, freeLists, class-table pages) AND the
forEachRoot set, for any Oop still pointing into eden. Result on the repro:
**ZERO dangling pointers**, while the corruption is present (suite iter2+ = F2).

So the earlier "missed scavenge root" hypothesis is WRONG — the HEAP is fully
pointer-consistent after every scavenge. Re-reading PHARO_YG_NO_SCAVENGE: it does
NOT disable scavenge, it only skips the PER-SAFE-POINT trigger ("Pre-compact
scavenge still runs inside fullGC"). So the fix-by-flag works because it changes
WHEN scavenge runs, not whether.

REFINED ROOT CAUSE: scavenge firing at a per-safe-point moment while a
primitive/bytecode handler holds an UNROOTED C++ local Oop / ObjectHeader*
(not enumerated by forEachRoot) across the allocation that triggers it. The move
tenures the object and updates all HEAP references, but the C++ stack local keeps
the stale young address and is used after eden reset -> corruption. This:
  - is invisible to a heap+forEachRoot dangle scan (stale ptr is a C++ local),
  - accumulates / is timing-sensitive (only bites when the trigger aligns with
    the in-flight unrooted local; eden fullness after the bestNodeFor cluster
    shifts the trigger into the runCase/parse path),
  - is clean under fullGC-time scavenge (VM at a safe boundary, no unrooted local).

NEXT (lldb, per CLAUDE.md "lldb is available"): repro under
  PHARO_NO_JIT=1 ./build/test_load_image /tmp/gc.image   (gc.image = PB probe
  running CDNormalClassParserTest suite 5x; iter2+ = F2 deterministically).
  Break in ObjectMemory::scavenge(); when it fires during the 2nd+ suite run,
  walk the C++ call stack to find the in-flight primitive/bytecode handler, and
  inspect its locals for an Oop/ObjectHeader* pointing into [edenStart_,
  edenFree_). That local is the unrooted reference to fix (root it via
  gcTempOop_/forEachRoot, or reload it after the allocation). The
  PHARO_SCAV_DANGLE_CHECK diagnostic stays as a reusable tool (proved the heap
  side is clean).

## Ruled out (negative results, narrowing to runner control-flow) — 2026-06-01

Black-box probing on the deterministic repro has now eliminated every
object-level corruption hypothesis. Scavenge at a safe point does NOT corrupt
the test objects:
  - identityHash STABLE across scavenge: probe allocated 200 Arrays, recorded
    hashes, churned 50x5000 young allocs + GC, re-checked: 0/200 changed,
    IdentitySet still finds all. (So not a hash-instability / Set-bucket bug.)
  - NO identity split: held a CDNormalClassParserTest's fresh classDefinition +
    slotNode across 8 rounds of forced scavenge (240K young allocs/round);
    `slotNode parent == classDefinition` stayed TRUE every round, same
    identityHash. (So scavenge does not duplicate/diverge the AST.)
  - afterGC IP restoration CLEAN: no GC-VERIFY-FAIL fires during the repro.
  - heap pointer-consistent post-scavenge (PHARO_SCAV_DANGLE_CHECK = 0 dangles).
  - scavenge fires at a clean BYTECODE BOUNDARY (Interpreter.cpp:2794, top of
    step() loop), not mid-primitive — needsScavenge_ is a deferred flag set in
    allocate() (ObjectMemory.cpp:2372) and consumed at the safe point.

=> The spurious failure is recorded by TestResult>>runCase: (a real TestFailure
is caught by its `on: failure do:`), yet bare `t setUp; t performTest` never
raises and the asserted objects are provably intact across scavenge. So the
corruption is in the RUNNER's exception/control-flow path when a scavenge fires
DURING `run` (the extra allocation in run/TestResult triggers the safe-point
scavenge that the bare performTest path doesn't). The remaining suspect is the
exception machinery (handler context / ensure: / signal-return) interacting with
a safe-point scavenge — NOT any heap object corruption.

NEXT (lldb, the only remaining tool): break ObjectMemory::scavenge(); filter to
the scavenge that fires while a CDNormalClassParserTest method or
TestResult>>runCase: is on the C++ frame stack (inspect method_ selector); single
-step the subsequent assert/exception dispatch and compare control flow vs a
non-scavenge run. Repro: PHARO_NO_JIT=1 ./build/test_load_image /tmp/gc.image
(PB probe: CDNormalClassParserTest suite run 5x; iter2+ = F2 deterministic).

## RESOLVED — Sista 2-value `^self` inliner loaded the wrong receiver (2026-06-02)

The cumulative-state corruption is NOT scavenge, NOT an unrooted C++ local, NOT
exception/control-flow machinery. It is a Sista IR-builder bug in the inline-const
-return path (`tryInlineConstReturn`, src/vm/jit/sista/SistaBuilder.cpp).

REPRO (1-shot, no suite needed). `^parent classDefinitionNode` on a CDSlotNode
whose `parent` is a CDClassDefinitionNode returns `self` (the CDSlotNode) instead
of `parent classDefinitionNode` (the CDClassDefinitionNode) on the 2nd+ invocation.
Round 1 is correct because Sista has no IC hint yet; rounds 2+ are wrong because
Sista compiles CDNode>>classDefinitionNode and mis-inlines the inner `parent
classDefinitionNode` send.

THE BUG. In tryInlineConstReturn, a 2-value callee `[kLoadReceiver, kReturn(v0)]`
(e.g. CDBehaviorDefinitionNode>>classDefinitionNode = `<primitive: 256>` returnSelf)
was inlined by setting `inlineOp = Op::kLoadReceiver` and falling through to the
common emit at the end of the function.  The common emit creates a new `kLoadReceiver`
IR value with no operands — and SistaLowering implements that as "load the current
compiled method's receiver".  When the callee is reached at an arbitrary send-site
(here `parent classDefinitionNode` inside CDNode>>classDefinitionNode), the "current
compiled method's receiver" is the OUTER method's self (= sn), NOT the value pushed
before the inlined send (= parent).  So the inlined body returns sn instead of cd.

The other 2-value cases (kLoadTrueOop / kLoadFalseOop / kConstantOop) are
load-constants and are inlined correctly regardless of receiver context.  Only
kLoadReceiver is context-sensitive and was being handled context-blindly.

FIX (commit `<this commit>`).  Move the kLoadReceiver case out of the common
emit and into a kLoadTemp-style direct stack passthrough: emit kGuardClass on the
inlined send's receiver, then push `recvId` (the simulated stack slot for the
inlined send's receiver) directly.  `^self` of a sub-send now correctly returns
the sub-send's receiver, not the outer method's self.  Same shape as the kLoadTemp
branch already there for `^arg`.

VERIFIED.  /tmp/cn.image PB probe: 8/8 correct under both PHARO_NO_JIT=1 (interp +
Sista) and the full JIT+Sista path.  PHARO_NO_SISTA=1 / PHARO_SISTA_NO_INLINE_CONST=1
also produce 8/8 (they sidestep the buggy inliner).

WHY THE EARLIER HYPOTHESES WERE WRONG.
  - "Scavenge missed root":  the FAILURE-pattern of a scavenge missed-root mimicked
    exactly what we saw (degrades on re-run; PHARO_YG_NO_SCAVENGE=1 hides it).
    But there was no missed heap root — the Sista compile is TRIGGERED by
    accumulated activations crossing its compile threshold, which happens to
    coincide with scavenge timing.  Once Sista's miscompile is installed, every
    subsequent invocation through it returns the wrong value, regardless of GC.
    PHARO_YG_NO_SCAVENGE=1 "fixed" it only because deferring the scavenge also
    deferred eviction of the JIT method-map / sista cache that the compile
    relies on, hiding the buggy compile in many runs.
  - "Unrooted C++ local":  same story — the dangle scan was clean because there
    was no dangling pointer.  The corruption was in JIT-emitted code, not the heap.
  - "Runner exception/control-flow":  the spurious test failure surfaces in
    runCase: only because runCase: invokes the (mis-inlined) classDefinitionNode
    assertion through its `on:do:` wrapper after enough activations to trigger
    the Sista compile.  Bare `performTest` passes because it hits the assertion
    BEFORE the Sista threshold (no compile yet).

LESSON.  Heisenbugs that appear to be GC/scheduling bugs can be JIT-compiler bugs
that share the timing surface area (compile thresholds, IC-fill epoch, ramp-up
allocations).  Bisect with PHARO_NO_SISTA=1, PHARO_SISTA_NO_INLINE_CONST=1,
PHARO_NO_JIT=1, PHARO_YG_NO_SCAVENGE=1 — the difference between which flag(s)
fix it pinpoints the layer (Sista IR / JIT / GC / scheduler).
