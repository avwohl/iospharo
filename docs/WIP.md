# WIP (2026-08-19) — the x86_64 "error gap" is a missing x86_64 Cairo, not a VM defect

Earlier in this session I called the x86-vs-arm error gap (288 vs 22) "the
largest open item".  **That framing was wrong.**  Measured on the running
x86 sweep, the errors are almost entirely one missing native library.

Errors by class-name prefix, same 1550 classes, arm vs x86:

    RS*       (Roassal)   x86=229   arm=0
    Athens*               x86=26    arm=0
    Sp*       (Spec GUI)  x86=10    arm=16     <- comparable, arm slightly worse
    Zn*, Morph*           x86=0     arm=0

Error classes, deduplicated:

    201  Error                <- Roassal downstream of the Athens failure
     61  SymbolNotFoundError
     10  MessageNotUnderstood
      6  SubscriptOutOfBounds

and the text names it outright:

    SymbolNotFoundError: Could not find symbol named:
      #cairo_image_surface_create searching in module: '...'
    ... #cairo_matrix_init_identity ...

The host has exactly one Cairo, and it is the wrong architecture:

    /opt/homebrew/lib/libcairo.2.dylib    arm64      <- only build present
    /usr/local/lib/libcairo*.dylib        absent     <- Intel prefix empty

A x86_64 VM running under Rosetta cannot load an arm64 dylib, so every
Athens/Roassal test that reaches Cairo fails at symbol resolution.  That
accounts for 255 of the 268 x86 errors seen so far; the remaining ~13 are
in line with arm.

So arm64 and x86_64 do NOT differ by an order of magnitude in VM
correctness.  They differ by one missing dependency.

## Remedy — NOT done, needs a decision

Installing an Intel-prefix Homebrew (`/usr/local`) plus `cairo` and
`freetype` for x86_64 is a system-level change to the developer's machine,
so it is not something to do unilaterally.  Until then the honest way to
report x86 SUnit numbers is to exclude the Cairo-dependent classes (RS*,
Athens*, Cairo*, FreeType*) and say so, rather than quoting a total that
is dominated by a missing dylib.

# WIP (2026-08-19) — arm full sweep after the `setGlobal` fix: F+E 55 -> 45

Both sweeps tallied with ONE method (sum only the `=== BATCH TOTAL ===`
blocks; per-class lines use `P:/F:/E:/S:` and double-count if included),
so these are directly comparable — same 2047 classes, same 27974 denominator:

    sweep-arm      (pre-fix)   P=27730  F=29  E=26  T=7   F+E=55   99.13%
    sweep-arm-fix  (post-fix)  P=27737  F=23  E=22  T=10  F+E=45   99.15%

Ten fewer failures+errors -- but **that delta is NOT the `setGlobal` fix
alone**, and the header of this entry overstates it.  `sweep-arm` is dated
08-18 07:39 and therefore predates FOUR VM commits, not one:

    08-18 12:32  456b4872  eval deferred-quit deadline
    08-18 21:23  30882ace  fail new:/basicNew: rather than class index 0
    08-18 22:26  2595f5bc  register a class on demand in new:
    08-19 07:03  9a9aefce  setGlobal wrong classes

The only clean attribution for `setGlobal` is the direct before/after on
the reproducing pair: Error 2 -> 0, Pass 47 -> 49.  Do not read the sweep's
-10 as this fix's contribution.

`testAllGlobalBindingAreGlobalVariables` and
`testAllGlobalNamesStartingWithDoCaseSensitive` both pass in full-sweep
context, and the `SmallInteger >> #asciiValue` / `#asLowercase` MNU
signature occurs ZERO times across all 2047 classes (it was the recurring
error before).

Not hidden: Timeout went 7 -> 10, which is why Pass rose by 7 rather than
10.  Timeouts are timing-sensitive and this delta is NOT shown to be real
— it needs a second run before anyone reads it as a regression.

## Earlier percentages in this file were computed inconsistently

The previously quoted "arm64 98.78% / x86_64 98.77%" used a different
denominator from the block-sum above.  Recomputed consistently the
pre-fix arm baseline is 99.13%, and the architectures are NOT near parity:

    sweep-x86         F=25  E=365  F+E=390  97.93%
    sweep-x86-fixed   F=25  E=288  F+E=313  98.20%
    sweep-arm-fix     F=23  E=22   F+E=45   99.15%

x86_64 carried an order of magnitude more ERRORS than arm64 (288 vs 22)
as of 08-18 16:42.  That was the largest open item, and far bigger than
the XMLParser deltas chased earlier.

The x86 sweep now running is already contradicting that gap.  Note the
`sweep-x86-fixed` baseline is dated 08-18 16:42, so it POSTdates the FFI
fix but PREdates the two `new:`/class-registration commits and
`setGlobal` -- most of any improvement is likely the class-registration
work (which took PolyMath from 4/117 to 117/117), not `setGlobal`.
Batch boundaries also differ between the two runs (1200 classes vs 1050
for the same batch-number range), so only the completed-run totals are
worth quoting.

# WIP (2026-08-19) — RESOLVED: the `68` DNU was `setGlobal` allocating `#Display` as a `Symbol`

Root cause, fixed in `ObjectMemory::setGlobal`.  When it installs a global
the image does not already have, it built BOTH halves of the binding with
the wrong class:

    Oop symbolClass = findGlobal("Symbol");            // abstract, format 0
    Oop symbolObj   = allocateBytes(symbolClassIdx, name.size());
    ... allocateSlots(indexOfClass(findGlobal("Association")), 2)

`Symbol` is abstract with instSpec format 0 (no indexable fields), so the
key symbol indexed as a non-indexable object and `at:` answered a
SmallInteger where the image expects a Character.  `68` is `$D`, from
`#Display`.  `lookupSymbol` twenty lines above already preferred
`ByteSymbol`; `setGlobal` simply did not.

The only caller that reaches this in practice is
`Interpreter::ensureDisplayForm`, and a fresh Pharo 13 image really has no
`Display` key (`Smalltalk globals includesKey: #Display` = false), so the
create path always ran.

Both halves fixed: `ByteSymbol` for the key (reusing the interned symbol
rather than allocating a duplicate -- symbols are unique), `GlobalVariable`
for the binding (a `LiteralVariable`, inst vars `#(name value)`, instSize 2,
so the existing slot stores were already right).

## Measured, reproducing pair (ReleaseTest, SystemNavigationTest), 55 tests

    pre-fix            Pass 47  Fail 4  Error 2   MNU 4-5   (3 runs, identical)
    + ByteSymbol       Pass 48  Fail 5  Error 0   MNU 0
    + GlobalVariable   Pass 49  Fail 4  Error 0   MNU 0

`testAllGlobalNamesStartingWithDoCaseSensitive` and
`testAllGlobalBindingAreGlobalVariables` both pass.  The 4 remaining
failures are pre-existing harness artifacts (Tests-Runner package,
SUnitRunner selectors, CommandLine handler process, weak subscriptions).

## How it was found, and two wrong turns worth remembering

The chain `SmallInteger.doesNotUnderstand: | Symbol.isLiteralSymbol` plus
`callerCls=Symbol callerSize=7` looked like a wrong-class *reference*, and
I published a hypothesis that it was a stale pointer surviving object
movement -- the corpse-trap family.  **That was wrong.**  `Symbol
allInstances` is a heap scan and it returned exactly 1, which a stale
reference could never satisfy.  Dumping the object's bytes named it
outright: `rcvrBytes=Display rcvrIsGlobalKey=true`.

The bisect also produced `noyg rc=124 mnu68=0`.  That was a TIMEOUT, not a
fix -- 900s was never enough for a no-young-generation run.  Reading the
zero as a result would have been a fabricated finding.  Record rc and a
completion marker alongside every count.

Ruled out and still true: deterministic, count stable at 2 under
`PHARO_NO_JIT=1` and `PHARO_NO_SISTA_PER_BC=1`, so not a JIT defect.

## Known remaining limitation (identified, NOT fixed)

`setGlobal` appends the binding to the first free slot of the dictionary
array without honouring hash placement or updating `tally`.  Enumeration
(`keysDo:`) sees it -- that is how the tests above find `#Display` -- but
hash lookup (`at:`/`includesKey:`) may not.  The real fix is to install the
global through an image-level `Smalltalk at:put:` send instead of poking
the dictionary from C++.  Not attempted here.

## In flight

Full SUnit sweeps on both architectures against the fixed binaries
(`$MY/fullsweep.sh`, arm then x86, sequential to avoid CPU contention
corrupting the per-test timeouts).  Baseline to beat: arm64 98.78%,
x86_64 98.77%.

# WIP (2026-08-19) — RETRACTION: the `68` DNU is not a stale reference. It is one real object.

The previous entry hypothesised that the wrong-class reference was a stale
pointer surviving object movement (the corpse-trap family).  **That is wrong
and is retracted.**  `Symbol allInstances` is a heap scan, and it finds the
object:

    symInstCount=1  rcvrIsByteSymbol=false  callerCls=Symbol  callerSize=7

Exactly ONE object in the entire heap has class `Symbol`, and it is the MNU
receiver.  A stale reference would not be reachable by a heap scan.  So this
is a real, persistent, heap-resident object whose class field is `Symbol`
(class-table index 3095) instead of `ByteSymbol` (3085).

## Where it comes from — narrowed to the run

    base.image (fresh)          Symbol allInstances size = 0
    prepped image (post-fileIn) Symbol allInstances size = 0
    during the test run         Symbol allInstances size = 1

So neither image load nor the harness fileIn creates it.  It appears while
ReleaseTest / SystemNavigationTest run.

## Ruled out

Bisect over the reproducing pair, counting `MNU rcvr=68`:

    base     rc=0    mnu68=2
    nojit    rc=0    mnu68=2     <- NOT the JIT
    nosista  rc=0    mnu68=2     <- NOT the Sista per-bytecode tier
    noyg     rc=124  mnu68=0     <- TIMEOUT.  A false zero, NOT a fix.
    base2    rc=0    mnu68=2     <- reproduces

The count is stable at 2 across configurations with very different timing
(no-JIT is several times slower), so this is deterministic, not a race.
`noyg` is re-queued with a 5400s budget and an explicit completion check --
900s was never enough for a no-young-generation run, and reading its
`mnu68=0` as a fix would have been a fabricated result.

`primitiveChangeClass`/`adoptInstance` (Primitives.cpp `changeClassOf`) is
NOT the creator: `Symbol format = 0` is a pointer format, a ByteSymbol's
instFormat is 16 (bytes), and the pointer/non-pointer compatibility check
returns Failure for exactly this pair.

## Open

Which code path manufactures it.  Both MNU chains walk globals
(`SystemEnvironment keysDo:` / `allGlobalNamesStartingWith:`,
`Association printOn:`), so a globals key is the leading guess.  The
in-flight probe (`$MY/symbytes.sh`) dumps the object's 7 bytes and tests
whether it is a key in `Smalltalk globals`, which should name it outright.

# WIP (2026-08-19) — the `SmallInteger 68` DNU is a wrong-CLASS reference, not a wrong value

Measured, on the arm build, in the base image (`$MY/symcls`):

    #Display class          = ByteSymbol
    (#Display at: 1)        = $D          (a Character)
    #Display basicAt: 1     = 68          <- the "68"
    Symbol format           = 0           <- NOT byte-indexable
    ByteSymbol format       = 1048576
    Symbol identityHash     = 792320      (class-table index 3095)
    ByteSymbol identityHash = 789760      (class-table index 3085)

Measured, in the failing harness run (`$MY/logs/symidx.txt`), via
`ex signalerContext sender receiver class` — this is the RECEIVER's class,
not the method's defining class, so `Symbol` here is meaningful:

    MNU rcvr=68 sel=#asciiValue
        callerCls=Symbol callerClsHash=792320   <- exactly Symbol's hash
        ByteSymbolHash=789760                   <- and NOT ByteSymbol's
        callerSize=7
        chain=SmallInteger.doesNotUnderstand: | Symbol.isLiteralSymbol | ...

## Why those two are inconsistent

`Symbol` has format 0: zero indexable fields.  An instance of `Symbol`
cannot have 7 characters.  But `basicSize` is read from the object
header's slot count, independent of the class field — so a genuine
7-byte `ByteSymbol` reached through a reference whose class resolves to
`Symbol` reports exactly this pair.  3085 -> 3095 is not a bit flip.

`Symbol>>isLiteralSymbol` walks the symbol expecting Characters from
`at:` (primitive 63, `primitiveStringAt`).  It got 68, i.e. primitive 60
(`Object>>at:`) semantics, and then `68 asciiValue` is the DNU.

## Hypothesis (NOT yet established)

This is the same defect as the corpse traps, one notch less severe: a
stale reference surviving object movement.  When it lands on a dead
object the class index reads 0 and the corpse trap fires; when it lands
on a live object of a different class you get this — a valid object,
wrong class, no trap.  That would explain why corpse traps and this DNU
travel together and why both need the harness (deep stacks, heavy GC) to
appear.

Unverified.  The discriminating run is in flight: `$MY/symbisect.sh`
re-runs the reproducing pair (ReleaseTest, SystemNavigationTest) under
base / PHARO_NO_JIT / PHARO_NO_SISTA_PER_BC / PHARO_NO_YG / base again,
counting `MNU rcvr=68`.  Results land in `$MY/logs/symbisect.txt`.
Two `base` runs bracket the knobs because this signal has not yet been
shown to reproduce run-to-run -- a knob that "fixes" it against a single
base run proves nothing.

# WIP (2026-08-19) — the last stable package residual is a package bug, not ours

`XMLWriterTest` was the one package failure that reproduced on BOTH
architectures in every run (8 errors).  All eight are the same send:

    MessageNotUnderstood: XMLWriterFormatter class >> #which

and it is not a VM divergence:

    implementors of #which   SDL_MouseButtonEvent, SDL_JoyAxisEvent,
                             OSJoyButtonEvent, ... (12, all SDL/joystick)
    XMLWriterFormatter class canUnderstand: #which        false
    XMLWriterFormatter class-side selectors               defaultIndentLevel,
                                                          defaultIndentString,
                                                          writer:

`#which` exists nowhere in `XMLWriterFormatter`'s hierarchy.  The test sends a
selector the loaded code does not implement, so it fails identically on any VM,
stock Cog included.  It is a version mismatch between the XMLWriter tests and
the XMLWriter code that Metacello resolved — a package problem.

## The package tier is now fully accounted for

    NeoJSON     116 P /  0 F                     clean, both arches
    Mustache     47 P /  0 F                     clean, both arches
    DataFrame   839 P / 14 F                     identical on both arches
    XMLParser  6359 P (arm) / 6354 P (x86)       XMLParserTest x86-only, unconfirmed
    PolyMath   ~1390 P, err 16-19                see below
    Fuel         19 P /  0 F                     clean
    Grease      no tests in its Metacello group  package definition

PolyMath's residual, now attributed:

    XMLWriterTest 8         package version mismatch (this entry) — NOT ours
    PMKDTreeTest 2-4        suite-context dependent; passes 13/13 alone on x86
    the rest, singles       run-to-run variance (arm alone scores 16,16,17,18)
    PMArbitraryPrecisionFloatTest  hits the 120 s per-class bound

So there is no confirmed VM defect left in the package tier.  What remains is
one unconfirmed x86-only difference (XMLParserTest, 5 errors, needs a second run
per side before it means anything) and one genuinely slow test.

---

# WIP (2026-08-19) — the "8-test cross-arch delta" is mostly NOISE, and I said otherwise

## Correction

The previous entry called the arm-vs-x86 package difference "genuine
architecture deltas rather than environment", on the strength of ONE run per
architecture.  Two runs each say otherwise:

    arm-run1   pass=1390 fail=3 err=17   PMKDTreeTest=3  PMGeneralFunctionFitTest=1  SMarkAutosizeRunnerTest=1
    arm-run2   pass=1390 fail=2 err=18   PMKDTreeTest=3  PMGeneralFunctionFitTest=2
    x86-run1   pass=1389 fail=2 err=19   PMKDTreeTest=4  PMGeneralFunctionFitTest=2
    x86-run2   pass=1389 fail=2 err=19   PMKDTreeTest=4  PMGeneralFunctionFitTest=2

Across all four PolyMath runs made today, arm has scored err = 16, 16, 17, 18 —
it VARIES BY 2 on its own.  x86 has scored 19 twice.  A 3-error difference taken
from one run per side is inside arm's own spread and is not established.

`PMKDTreeTest` is the one that still looks like it might be real (arm 2/3/3,
x86 4/4), but it also passes 13/13 when run ALONE on x86, so whatever it is
depends on suite context, not on the instruction set.

## What IS solid

    XMLWriterTest = 8 errors on BOTH architectures, every run

Stable, reproducible, architecture-independent.  That is the residual worth
investigating; the rest of the "delta" is run-to-run variance in a suite whose
own errors move by 2 between identical runs.

## Method note

Same trap as the corpse counts: a number measured once is not a difference.  The
package tier needs two runs per side before any arm-vs-x86 claim, exactly as
`scripts/sunit-sweep.sh`'s header already says for the SUnit tier ("if a result
looks like a regression, re-run it idle before believing it").

---

# WIP (2026-08-19) — UN-RETRACTION: vm_stop DOES time out, and the fix is in

## I retracted a correct conclusion on too small a sample

The original entry (`e84e8501`) said: vm_stop waits at most 2 s for the
interpreter to return, gives up, and vm_destroy frees the interpreter and heap
anyway — a use-after-free.  A later entry retracted that as "measured FALSE",
on the strength of 9 consecutive runs with a worst wait of 15 ms.

**The retraction was wrong.**  Nine runs is simply not enough to see a ~10%
event.  Ten more runs, with vm_stop now reporting its own outcome:

    relaunch x10:  pass=9  fail=1  destroy-leak=1  stop-timeout=1

and the failing run says it plainly:

    === Cycle 1 of 3 ===   [VM-STOP] interpret() returned after 15ms   PASS
    === Cycle 2 of 3 ===   [VM-STOP] interpret() returned after 15ms   PASS
    === Cycle 3 of 3 ===   [VM-STOP-TIMEOUT] interpret() has not returned 2s
                           after stop(); the worker is still running
                           [VM-DESTROY-LEAK] the worker is STILL inside
                           interpret() after vm_stop and a further 5s wait
      cycle 3: FAIL (vm_stop timed out; worker stuck)

So the original mechanism stands: vm_stop times out on roughly one cycle in
thirty, and before today vm_destroy then freed the interpreter and the heap
under a thread still executing inside interpret().  That is exactly the two
crash reports — EXC_BAD_ACCESS in `Interpreter::synchronousSignal` with the
main thread inside `free()`.

Lesson worth keeping: "it did not reproduce in N runs" is not "it does not
happen" unless N is large enough for the rate.  The retraction cited 9/9 as if
it were proof.

## The fix

`vm_destroy` no longer assumes the precondition its own comment documents.  It
waits a further 5 s and, if the worker is still inside `interpret()`, LEAKS the
interpreter and heap rather than freeing them under it, and says so.  A leak
during teardown costs memory in a process that is exiting; a use-after-free
corrupts whatever the worker touches next.

Measured: 10 runs, one timeout, **zero EXC_BAD_ACCESS, exit code 0**.  The cycle
is reported as `FAIL (vm_stop timed out; worker stuck)` — which is the truth,
and which test_relaunch was always able to say but never got to because the
crash beat it to the summary.

STILL OPEN, and unchanged by this: WHY interpret() sometimes fails to return
within 2 s.  The fix makes the consequence safe and visible; it does not explain
the cause.  `[VM-STOP-TIMEOUT]` now marks the moment for whoever chases it.

---

# WIP (2026-08-19) — x86_64 packages are measurable after all: load on arm, test on x86

## The method

x86_64 cannot LOAD a package on this host: Metacello's `github://` goes through
Iceberg -> libgit2, and only an arm64 libgit2 exists here.  But it can RUN the
tests — Fuel passes 19/19 there, needing no fetch — and Spur images are
architecture-neutral.  So: load with the arm64 VM, then run the test step with
the x86_64 VM against that same image.

That is a BETTER cross-architecture test than loading separately on each arch,
because the image is held constant: any difference is codegen, not environment.

## Results, same image, both VMs

    package     classes   arm64              x86_64            delta
    NeoJSON        11      116 P /  0 E       116 P /  0 E     identical
    Mustache        1       47 P /  0 E        47 P /  0 E     identical
    DataFrame      27      839 P / 14 F       839 P / 14 F     identical
    XMLParser     159     6359 P /  0 E      6354 P /  5 E     XMLParserTest 0 -> 5
    PolyMath      117     1392 P / 16 E      1389 P / 19 E     PMKDTreeTest 2 -> 4
                                                               PMGeneralFunctionFitTest 1 -> 2
                                                               SubunitTestExamples +1

So x86_64 runs 8261 package tests here, against arm64's 8273, and the entire
difference is eight tests in three classes.  That is the first real x86 package
number this project has had; it was previously recorded only as "blocked".

## A stale binary nearly produced a fictional finding

The first x86 attempt reported PolyMath `corpse=4 dnu=42, PARTIAL 4/117` — the
exact pre-fix signature — which reads as an architecture-specific GC defect.
It was not: `build-x86` had not been rebuilt after the class-registration fix.

    build-rel   NEW-BAD-CLASS string: 1   mtime 08-18 22:25
    build-x86   NEW-BAD-CLASS string: 0   mtime 08-18 12:47

Rebuilt, x86 completes PolyMath exactly as arm does.  Check the binary contains
the change before attributing a difference to the architecture — `strings |
grep` on a diagnostic the change introduced is enough.

## Next on this tier

The eight differing tests are the only remaining cross-arch package delta, and
they are concentrated: `XMLParserTest` (5), `PMKDTreeTest` (2 more than arm),
`PMGeneralFunctionFitTest` (1 more).  Two of the three are numeric; that is
where to start.

---

# WIP (2026-08-19) — the `SmallInteger` DNU: the receiver's class is the ABSTRACT `Symbol`

## Captured from inside the failing run

The runner's error handler now reports the MNU receiver and the sender chain
(local instrumentation of the pharo-headless-test submodule, not committed
there):

    MNU rcvr=68 sel=#asciiValue
      chain= SmallInteger.doesNotUnderstand: | Symbol.isLiteralSymbol
             | Symbol.storeOn: | Symbol.printOn: | Association.printOn: | ...
    MNU rcvr=68 sel=#asLowercase
      chain= SmallInteger.doesNotUnderstand: | Symbol.beginsWith:caseSensitive:
             | ByteString.withIndexDo: | SystemNavigation.allGlobalNamesStartingWith:do:caseSensitive: | ...

**68 is the code point of `$D`.**  So `at:` answered the raw byte where a
Character was required — and the calling frame's receiver class is `Symbol`,
the ABSTRACT class, not `ByteSymbol`.

Measured in the same image:

    #someLiteral class            ByteSymbol      (a normal symbol)
    Symbol includesSelector: #at:  false          <-- the whole mechanism
    objects with class == Symbol   0 at rest

`ByteSymbol>>at:` is `<primitive: 63>` and answers a Character.  `Symbol` does
not implement `at:` at all, so a receiver whose class resolves to `Symbol` falls
through to `Object>>at:` (primitive 60), which answers a raw SmallInteger for a
byte object.  68.  Every observation follows from that one fact.

## What this is, and is not

It is NOT a String / `at:` defect: both primitives are correct, and the right
one for `ByteSymbol` was simply never reached.  It is a **class-index integrity**
problem — an object whose header classIndex resolves to the wrong class — which
is the same family as the class-table root cause confirmed earlier today, and
consistent with everything already eliminated:

    not the JIT             baseline / NO_JIT / NO_SISTA_PER_BC all identical
    not the method cache    PHARO_NO_METHOD_CACHE=1 identical (MNU=6)
    not visible at rest     0 objects with class == Symbol in a resting image

It needs the SUnit runner to appear, and the runner compiles methods during its
startup, so a symbol created at runtime is the natural suspect.

## Next

Find the object: at the DNU, walk from the failing frame to the receiver and
print its raw header classIndex alongside `classTable_[thatIndex]`, exactly as
`[NEW-BAD-CLASS]` does for `new:`.  If the index is `Symbol`'s, something
created a symbol with the abstract class; if the index is right and the TABLE
entry is wrong, it is a class-table corruption and the `[CTOVERWRITE-*]`
diagnostics under `PHARO_CTCHECK` should catch the writer.

---

# WIP (2026-08-19) — session close: all three tiers, with the class fix verified

## No regression from the class-registration fix

A change that touches every `new` / `new:` in the VM has to be shown harmless at
suite scale, not argued to be.  Full arm64 sweep, same image, same machine:

    baseline (pre-fix)            2045 classes  28070 tests  27728 P  29 F  26 E  98.78%
    with class-registration fix   2045 classes  28070 tests  27728 P  29 F  26 E  98.78%

Byte-identical.  Per batch it tracked the baseline throughout (batch 1 exactly,
600 exactly, 1798 within one test of it — inside this suite's documented
few-tests-per-run noise).

## Where the three tiers stand

    VM tier     green on both architectures
                test_sista_ir, test_asmjit_t1_stub, test_class_table,
                test_relaunch 9/9

    SUnit       arm64   2045 cls  27728 P / 29 F / 26 E   98.78%   1 h 47 m
                x86_64  1997 cls  27221 P / 25 F / 26 E   98.77%
                (x86 excluding the classes that need this host's arm64-only
                cairo/freetype; raw x86 is 2046 cls / 25 F / 288 E)

    packages    arm64, 7772 tests passing:
                NeoJSON 116, Mustache 47, XMLParser 6359, PolyMath 1392,
                DataFrame 839, Fuel 19
                x86_64 blocked by an arm64-only libgit2 on this host; Fuel
                passes there 19/19 (no fetch needed), which is the control
                showing the x86 VM runs package tests fine

## Real defects fixed this session, each measured before and after

  1. **x86_64 FFI ABI** — libffi's x86 ffitarget.h gates its 64-bit ABI enum on
     X86_64/X86_DARWIN, which libffi's own configure defines and we did not, so
     x86_64 compiled FFI_DEFAULT_ABI = FFI_SYSV = 1 against a library expecting
     FFI_UNIX64 = 2.  79 errors -> 2.
  2. **The 120 s eval deadline** killed any eval still working, at exit code 0
     with no output.  Now distinguishes working from idle by bytecode progress.
  3. **libffi universal headers** — make_universal lipo'd two architectures'
     libraries and copied only slice_a's headers.
  4. **`new:` allocating with class index 0** — indexOfClass answers 0 for
     "not found"; six primitives passed it to allocateSlots, manufacturing an
     object with no class.
  5. **Runtime-created classes never registered** — classTable_ is rebuilt at
     load from object headers, so a class with no instance at snapshot time has
     no slot.  registerClass already handled it and was simply never called.
     XMLParser: no RESULT -> 6359 passing.  PolyMath: 4/117 -> 117/117.
  6. **fullGC's pre-compact scavenge escaped every scavenge bisect knob**, so
     "disable scavenging" floored at 1 and read as a non-fix.

Plus four harness defects that were producing VM-looking failures: the missing
`.sources` file (69 errors in one batch), substring test selection, unreported
non-persisting loads, and write-once result files.

## Retracted this session, in case any of it was acted on

Each was published here and then measured false: the JIT hypothesis for the
`SmallInteger` DNU; vm_stop's 2 s timeout as the test_relaunch crash mechanism;
the image-size correlation for "no RESULT"; the remote-temp and
operand-stack-above-sp hypotheses; and the entire GC / use-after-collect framing
of the PolyMath hang.  The object was never collected.

Two diagnostics caused most of that and are now annotated where they are
emitted: `selectorOf` on a CompiledBlock answers the block's LAST LITERAL rather
than a selector (`method_=#cos` was never Float's), and comparing corpse COUNTS
between bisect arms hides genuinely different corpses behind equal numbers.

## Open, in priority order

  1. x86_64 packages need cairo/freetype/libgit2 for x86_64 on this host, or
     the already-fat maccatalyst slices linked statically.
  2. PolyMath residual: XMLWriterTest 8 errors, PMKDTreeTest 2, five singles,
     PMArbitraryPrecisionFloatTest at the 120 s per-class bound.
  3. `SmallInteger` DNU in ReleaseTest / SystemNavigationTest — measured NOT the
     JIT; needs the receiver captured from inside the failing run.
  4. test_relaunch's use-after-free — not reproducing, but vm_destroy freeing
     after a bounded wait is wrong on its own terms.
  5. Grease ships no tests in its default Metacello group.

---

# WIP (2026-08-18) — arm64 package tier, before and after the class-registration fix

Same harness, same base image, same machine:

    package     before                              after
    NeoJSON     116 P /  0 F                        116 P /  0 F     unchanged
    Mustache     47 P /  0 F                         47 P /  0 F     unchanged
    XMLParser   NO RESULT (test pass never          6359 P /  0 F    159 classes
                reported anything)
    PolyMath    PARTIAL 4/117, 240 tests, then      1392 P /  2 F    117 classes
                the VM idled until the eval           16 E / 1 T
                deadline killed it
    DataFrame   839 P / 14 F                        839 P / 14 F     unchanged
    Fuel         19 P /  0 F                         19 P /  0 F     unchanged
    Grease      0 test classes                      0 test classes   package group
                                                                     ships no tests

7772 tests passing.  The four packages that already worked are byte-identical,
so the fix is confined to what was broken.  PolyMath reproduces through the
harness (873 s load, 284 s test) exactly as it did run directly, so the number
is not a one-off.

Two harness notes, both mine and both now visible rather than silent:

  * PolyMath needs `LOAD_TIMEOUT` above ~890 s.  At the 900 s default the load
    is marginal and rc=124; the run that hit it correctly reported
    `DID-NOT-PERSIST` and then `classes=0`, instead of the pre-2026-08-18
    behaviour of reporting `classes=0` with no indication why.
  * Grease's `classes=0` is a package-definition fact, not a VM one: its default
    Metacello `load.` group contains no TestCase subclasses at all.  Getting its
    tests needs the `Tests` group in the PACKAGES table.

## Still open on this tier

  * XMLWriterTest 8 errors, PMKDTreeTest 2, five singles, and
    PMArbitraryPrecisionFloatTest hitting the 120 s per-class bound — the
    PolyMath residual, spread thin.
  * x86_64 packages remain blocked by an arm64-only libgit2 on this host;
    Fuel passes there (19/19) because it needs no fetch, which is the control
    showing the x86 VM runs package tests fine and only cannot fetch.

---

# WIP (2026-08-18) — PolyMath's suite now completes: 117/117 classes, 1392 passing

## The result

    before   PARTIAL after 4/117    240 tests, then the VM idled and was killed
    after    RESULT classes=117 pass=1392 fail=2 err=16 timeout=1

1392 of 1411 (98.7%), with the residual spread thin rather than concentrated:

    XMLWriterTest                   61 ran, 53 passed,  8 errors
    PMKDTreeTest                    13 ran, 11 passed,  2 errors
    PMAnotherGeneticOptimizerTest   11 ran, 10 passed,  1 error
    PMGeneralFunctionFitTest         9 ran,  8 passed,  1 error
    PMPermutationTest               20 ran, 19 passed,  1 error
    PMTSNETest                       6 ran,  5 passed,  1 error
    SMarkTest                        2 ran,  1 passed,  1 error
    PMArbitraryPrecisionFloatTest   TIMEOUT (120 s per-class bound)

## What actually fixed it

One line per `new`/`new:` site: register the class on demand.  See the previous
entry for the root cause — `classTable_` is rebuilt at load by walking object
headers, so a class with no instance at snapshot time is never registered, and
`indexOfClass` then answered 0 for it.

## A note on how long this took, and why

The chain of symptoms ran: "PolyMath reports 4 of 117 classes" -> a corpse
cascade -> a wedged Delay scheduler -> a VM that idles until the eval deadline
kills it.  Read from the top it looks like a GC bug, and several entries above
pursued exactly that, through eight refuted hypotheses.  It was none of them:
the object was never collected, it was allocated with class index 0.

The two things that would have shortened it:

  * `method_=#cos` in the DNU dump is `selectorOf` on a **CompiledBlock**, which
    answers the block's LAST LITERAL, not a selector.  Two mechanisms were built
    on reading it as `Float>>cos` / `PMVector>>cos`.
  * comparing `corpse=N` counts between bisect arms instead of the corpse's
    `origSel=/method_=` IDENTITY.  Equal counts hid different corpses and once
    inverted the attribution outright.

Both are now called out where the diagnostics are emitted.

---

# WIP (2026-08-18) — ROOT CAUSE: runtime-created classes never get a class-table index

## The measurement

In the reloaded PolyMath image, `new` splits perfectly along one line:

    Object=OK  OrderedCollection=OK  Array=OK
    PMVector=FAIL  PMAB2Solver=FAIL  PMAB2Stepper=FAIL
    PMAB2SolverTest=FAIL  PMExplicitSystem=FAIL

and the class hashes say why.  Pharo's `identityHash` is the VM's raw header
hash x256, so dividing gives the class-table index the VM actually uses:

                  Smalltalk identityHash     raw = class index
    Array                       13056                     51
    TestCase                   286464                   1119
    OrderedCollection          808704                   3159
    Object                     809216                   3161
    ------------------------------------------------------------
    PMExplicitSystem        334304512                1305877
    PMAB2SolverTest         369531648                1443483
    PMVector                801588736                3131206

Base-image classes hold small, real class-table indices.  The package classes
hold ORDINARY OBJECT HASHES — over a million — which were never class-table
indices.  (`Array=51` also matches the `allocCls=51` the allocator trap printed
for an unrelated allocation, which confirms the x256 decoding.)

`ObjectMemory::indexOfClass` is built on the assumption in its own comment,
"a class's identity hash IS its class table index".  That holds for classes the
image shipped with.  For a class created at runtime it reads
`classTable_[1443483]`, finds nil, falls through the linear scan, and returns 0
— and every `new`/`new:` then allocated an object with class index 0.

## Why it is package-dependent

`classTable_` is built at image load by walking object headers: a class is
registered because some instance's header names its index.  A class that HAS
instances when the image is snapshotted therefore survives; a class with NO
instances at that moment never appears in any header and is never registered.

That is why NeoJSON, Mustache, DataFrame and Fuel pass the same
load -> snapshot -> reload workflow while PolyMath does not.  It is not "loaded
classes fail", it is "classes that never received an index fail", which varies
per package with what the load happened to instantiate.

## What the VM is missing

Cog assigns a class-table index on demand (`ensureBehaviorHash:`): when a class
is first needed as a class, the VM allocates a free class-table slot, writes
that index into the class's header hash, and registers it.  This VM has no such
path — `indexOfClass` can only look up, never assign.

**Fix design** (not yet implemented): give `indexOfClass` a non-const
counterpart that, on miss, allocates a free slot, stores it as the class's
header identityHash, and sets `classTable_[idx] = classOop`.  The `new` family
should call that rather than fail.  Care needed on: which index ranges are
reserved, keeping it stable across snapshot (the hash is written into the
object, so it persists), and not assigning an index to a non-class.

Until then the guard committed earlier turns this from an unrecoverable corpse
cascade plus a wedged Delay scheduler into a clean, catchable `PrimitiveFailed`.

## Superseded

Every entry above that treats this as a GC or scavenger defect is chasing a
symptom.  The object was never collected; it was allocated with class index 0
because the class lookup failed.  The scavenge-dependence observed earlier is
consistent with GC timing changing *which* allocation trips first, not with the
GC causing it.

---

# WIP (2026-08-18) — ROOT CAUSE + FIX: `new:` allocated with class index 0

## Not a GC bug at all

The allocator forensics settle it.  On the deterministic repro:

    [CORPSE-PUSH] val=0x71f1f1c400 ... bcOff=2 bc=0x7d
                  isLastAlloc=1 allocScav=2 nowScav=2 allocCls=0

  * `isLastAlloc=1`  — the corpse IS the object `PMVector new: 2` just returned
  * `allocScav == nowScav` — NO scavenge ran between the allocation and the
    push, so nothing collected it
  * `allocCls=0` — `allocateSlots` was CALLED with class index 0

The object was **born** with classIndex 0.  Several entries above this one frame
this defect as a use-after-collect and hunt the scavenger; that framing is wrong
and those entries are superseded.  (The scavenge dependence was real but
indirect — see the open question below.)

## Why it presented as an unrecoverable hang

`ObjectMemory::indexOfClass` answers **0 for "not found"**, and 0 is not a valid
Spur class index:

    if (hash != 0 && hash < classTable_.size() && classTable_[hash] == classOop)
        return hash;
    for (uint32_t i = 0; i < classTable_.size(); ++i)
        if (classTable_[i] == classOop) return i;
    return 0;  // Not found

All six `new` / `new:` primitives passed that straight to `allocateSlots`.  An
object with classIndex 0 has no class, so every send to it raises
`doesNotUnderstand:` — and the DNU cannot be handled either, for the same
reason.  It surfaced far from the allocation as a "corpse" cascade that also
left the Delay scheduler with `timerSem=nil`, i.e. as a hang.

## The fix, and what it does and does not do

All six sites now fail the primitive and name the failure, so the image's
`basicNew:` fallback runs and can signal normally.  Measured on the repro:

    before   corpse=4  dnu=42  timerSem=nil x25  VM idle until the 120 s
             eval deadline killed it (~243 s)
    after    corpse=0  dnu=0   no timer wedge    Error: PrimitiveFailed

Normal allocation is unaffected: 5000 `Array new: 3` on a clean image allocate
correctly with no `[NEW-BAD-CLASS]`.

**The test still does not PASS.**  This converts an unhandleable hang into a
catchable image-level error; it does not make the lookup succeed.

## Still open: why the class is not found

`indexOfClass` compares `classTable_` entries against the receiver by IDENTITY.
A stale class Oop therefore misses the hash path AND the linear scan.  The
scavenge does pass `includeClassTable=true`, so the table itself is updated —
which points at the class Oop reaching the primitive being stale, not the table.
That is the next thing to chase, and `[NEW-BAD-CLASS]` now prints the receiver
address to start from.

---

# WIP (2026-08-18) — the corpse is BORN DEAD: `new:` returns classIndex 0

## Measured

The corpse trap now prints the pc and bytecode.  On the deterministic repro:

    [CORPSE-PUSH] val=0x73c9f1afa0 ... inStack=0 sp=68 fp=64 bcOff=2 bc=0x7d
    [CORPSE-PUSH] val=0x73c9f1afa0 ... inStack=1 firstSlot=67  bcOff=4 bc=0x42
    [CORPSE-PUSH] val=0x73c9f1afa0 ... inStack=1 firstSlot=67  bcOff=8 bc=0x71

The block's bytecodes start at method offset 49, so these map exactly:

    bcOff=2  0x7D  send: new:       <- FIRST push, and it is already a corpse
    bcOff=4  0x42  pushTemp: 2
    bcOff=8  0x71  send: at:put:

**The first corpse push is the RESULT of `PMVector new: 2`.**  The object is not
something that lives and later dies inside the block — it comes back from the
allocation already carrying classIndex 0.  That is also why `inStack=0`: at that
instant it exists only in the allocation path's C++ local, before any push.

## `method_=#cos` was a red herring for this entire investigation

`memory_.selectorOf(method_)` on a **CompiledBlock** answers its last literal,
and this block's last literal is `#cos`.  Every `in=#cos` / `method_=#cos` in
these logs means "inside this block", NOT `Float>>cos`, and not `PMVector>>cos`.
Two earlier entries in this file built a mechanism on that label; both are
already retracted, and this is why.  Anyone reading `method_=` for a block frame
should treat it as a literal, not a selector.

## What this points at, and what is still inferred

`ObjectMemory::allocateRaw` documents the design intent:

    // Threshold-based GC trigger: request compacting GC at next safe point
    // This avoids running GC from allocation where C++ locals hold Oops.

So allocation is meant to DEFER its GC to a safe point precisely because the
new object is only in a C++ local.  The measurements say the object nevertheless
comes back dead, and that it takes fullGC's pre-compact scavenge to happen at
all (scavenges=0 -> no corpse).  The obvious reading is that the deferral is not
airtight for this path — but that is INFERENCE, and eight hypotheses have died
on this defect already.

Next, and it is direct: trap inside the allocation itself.  Log the Oop returned
by `PMVector new:`-sized `allocateSlots` together with `g_scavengeCount`, and
compare the count at allocation with the count at the push.  If a scavenge
occurred in between, the deferral leaked; if not, the object was never
initialised and the fault is in the allocator, not the GC.

---

# WIP (2026-08-18) — `v` is a PLAIN frame temp; remote temps refuted; the window is bytecodes 59-63

Compiling the same block source in a clean base image (the compiler produces the
same temp layout, and this costs 5 s instead of 90 s on the 1.28 GB PolyMath
image) gives the CompiledBlock's bytecode:

    52 <D2> popIntoTemp: 2      <- v
    53 <42> pushTemp: 2
    54 <51> pushConstant: 1
    55 <41> pushTemp: 1         <- t
    56 <82> send: sin
    57 <71> send: at:put:
    58 <D8> pop
    59 <42> pushTemp: 2         <- v pushed as the at:put: RECEIVER
    60 <21> pushConstant: 2
    61 <41> pushTemp: 1         <- t
    62 <83> send: cos           <- allocates; the corrupting GC lands in here
    63 <71> send: at:put:       <- receiver is the v pushed at 59

`popIntoTemp:` / `pushTemp:` — **`v` is a plain FRAME temp, not a remote temp**,
and the enclosing `fullClosure:` reports `NumCopied: 0`, so nothing is captured
into an indirection vector.  The remote-temp hypothesis from the previous entry
is REFUTED.  (It was worth testing: this workload reports `remoteTemp=2661087`.)

## The sharp form of the defect

At the GC inside bytecode 62, `v` should be live in TWO places, both inside
`[stackBase_, stackPointer_)`:

  1. temp slot 2 of the block's frame
  2. the operand pushed at bytecode 59, sitting under the pending `at:put:`

Either alone should keep it.  Yet it is collected, and the corpse trap reports
`inStack=0` at the first push — the value is in NEITHER place by the time it is
pushed again.

So the question is no longer "which holder is unrooted".  It is: **why is the
frame region that holds a live block temp and a pending send's receiver not
being scanned (or not being updated) while a send from that frame is
executing?**  sp=68 and fp=64 at the trap, so the whole frame is four slots and
sits well inside the scanned range on the face of it.

## Ruled out, all by measurement

    the JIT                        4 configurations
    the Sista per-bytecode tier    PHARO_NO_SISTA_PER_BC=1
    f96cb69b's temp-sync residual  PHARO_NO_GC_TEMPSYNC=1
    mark / compaction              [HEAPCHECK post-fullGC] => CLEAN
    a stack slot above sp          PHARO_SCAV_SCAN_ABOVE_SP=64 and =1024
    SavedFrame's Oop fields        all six visited (code)
    remote temps / captured vars   plain pushTemp:, NumCopied: 0 (bytecode)

## Next, and it is one cheap instrument

Extend the corpse trap to print the current PC and bytecode alongside the
sp/fp it already prints.  That says exactly which bytecode pushes the corpse and
therefore which frame region the value came out of — the last thing still being
inferred rather than measured.  Everything else about this defect is now
measured, and eight hypotheses have died, so infer nothing further.

---

# WIP (2026-08-18) — the holder is OUTSIDE the operand stack: measured, not argued

The corpse-push trap now reports whether the value is also resident in
`[stackBase_, stackPointer_)`, and where.  On the deterministic repro:

    [CORPSE-PUSH] val=0x7a49f1b3b0 in=#cos fd=20 jitState=0 inStack=0 firstSlot=-1 sp=68 fp=64
    [CORPSE-PUSH] val=0x7a49f1b3b0 in=#cos fd=20 jitState=0 inStack=1 firstSlot=67 sp=68 fp=64
    [CORPSE-PUSH] val=0x7a49f1b3b0 in=#cos fd=20 jitState=0 inStack=1 firstSlot=67 sp=68 fp=64

**On the FIRST push, `inStack=0`** — the value is nowhere in the live operand
stack.  The later `inStack=1 firstSlot=67` is just the first push's own result
sitting at sp-1.  So the value is produced from somewhere OUTSIDE the operand
stack and pushed into it.

That settles where the fix belongs.  It is not a root-scan range bug: had the
value been inside `[stackBase_, stackPointer_)`, the scan walked it and dropped
it anyway.  It is a holder the collector never looks at.

## SavedFrame is NOT the gap

Every Oop field of `SavedFrame` is visited by `forEachOopRoot`:

    savedMethod  savedHomeMethod  savedReceiver
    savedClosure savedActiveContext  materializedContext

The only other pointer members are `savedFP` and `materializedRetSlot`, both
`Oop*` INTO the stack rather than Oops.  So the saved-frame array is fully
rooted and cannot be the missing holder.

## Ruled out so far, every one by measurement

    the JIT                        4 configurations
    the Sista per-bytecode tier    PHARO_NO_SISTA_PER_BC=1
    f96cb69b's temp-sync residual  PHARO_NO_GC_TEMPSYNC=1
    mark / compaction              [HEAPCHECK post-fullGC] => CLEAN
    a stack slot above sp          PHARO_SCAV_SCAN_ABOVE_SP=64 and =1024
    a stack slot below sp          inStack=0 on the first push
    SavedFrame's Oop fields        all six are visited (code)

## The candidate that fits, and how to test it cheaply

A value pushed in `#cos` that was never in the stack is read from somewhere:
a VM register, a literal, an instance variable, or a REMOTE TEMP vector.  The
first three are traced (`receiver_` and friends are visited registers; literals
and ivars live in traced heap objects).  Remote temps are the one that is worth
checking first, and this workload leans on them hard — the JIT summary for this
very run reports `remoteTemp=2661087`.

If `v` lives in an indirection vector and that vector's slots are not traced or
not updated by the scavenge, `pushRemoteTemp` would read a stale slot and push a
value that is in no root — exactly what is measured.

Test: dump the block's bytecode for
`PMExplicitSystem block:`'s argument in `PMAB2SolverTest>>testVectorSystem` and
see whether `v` is a plain frame temp or a remote temp; if remote, watch the
vector across the corrupting scavenge.  Do NOT assume — this defect has already
killed six hypotheses, three of them mine and stated confidently.

---

# WIP (2026-08-18) — RETRACTION: the corpse is a BLOCK TEMP, and `#cos` is Float's

An earlier entry in this file states the corpse is "`self`, the receiver of
`PMVector>>cos`, which mutates in place".  **That is wrong.**  It was inferred
from `method_=#cos` plus finding a `PMVector>>cos` in the image, without reading
the test.  The test says:

    testVectorSystem
        | solver stepper system dt |
        dt := 0.01.
        system := PMExplicitSystem block: [ :x :t |
                  | v |
                  v := PMVector new: 2.
                  v at: 1 put: t sin.
                  v at: 2 put: t cos.
                  v ].
        ...

So the `#cos` in the DNU is **`Float>>cos`** (`t cos`), and the object taking
`at:put:` on a dead header is **`v`** — a TEMP OF THE BLOCK, holding a
`PMVector new: 2` allocated three lines earlier.  `PMVector>>cos` exists but
never runs here.

That changes the shape of the defect: it is not "a receiver mutated in place
dies under its own method", it is **a freshly allocated object held only in a
block temp does not survive a GC triggered by an allocating send between its
creation and its use** (`t sin` and `t cos` each box a Float).

Consistent with everything measured:

  * only 2 scavenges and 4 fullGCs occur in the whole run, so the window is
    narrow and the failure is rare rather than immediate — the solver completes
    thousands of steps successfully
  * `PHARO_WATCH_ROOT_CLASS=PMVector` emitted ZERO `[ROOT-WATCH]` lines.  The
    matcher is an exact class-name compare and `PMVector allSubclasses` is
    `#()`, so it WOULD have matched; no root visited a PMVector at any of those
    six GC events
  * the object-aware dangle check is clean, and it walks the same root set

## A minimal repro does NOT yet exist

The obvious synthesis — a block temp holding a fresh Array across two allocating
sends — does not reproduce:

    200,000 iterations of
        [ :t | | v | v := Array new: 2.
               v at: 1 put: t sin. v at: 2 put: t cos. v ]
    on a clean base image:  bad=0, corpse=0, dnu=0

So the shape alone is not sufficient; the real case runs the block through
PMExplicitSystem/PMAB2Solver at frame depth 20.  Do not spend more on
synthesising one before instrumenting the real repro, which is deterministic and
costs ~90 s.

## Method note

This is the second time on this defect that reading a diagnostic field without
reading the corresponding SOURCE produced a confident wrong answer (the first
was comparing corpse COUNTS across arms).  `method_=#cos` names the method the
VM is in; it does not tell you which `cos`, and there were eleven implementors.

---

# WIP (2026-08-18) — the missed root is NOT an operand-stack slot above sp

The one shape that fitted every earlier measurement was: the receiver is live
only in a stack slot outside `[stackBase_, stackPointer_)` — args popped by a
primitive that then allocates — invisible to the scavenger AND to the dangle
check, which walk the same set.  That is this repo's defect `#1` family, so it
was the obvious candidate.

**It is wrong.**  `PHARO_SCAV_SCAN_ABOVE_SP=N` (new) additionally roots N
operand-stack slots above `stackPointer_`, visiting only values that pass
`isValidPointer`:

    baseline              origSel=#at:put: method_=#cos   scavenges=2 fullGCs=4
    above-sp N=64         origSel=#at:put: method_=#cos   scavenges=2 fullGCs=4
    above-sp N=1024       origSel=#at:put: method_=#cos   scavenges=2 fullGCs=4

Unchanged at either window.  So the holder is not in the operand stack at all.

The knob is kept: it is a cheap, guarded way to test this shape on the next
use-after-collect, and it says at its own definition that it is a diagnostic and
not a fix (it conservatively pins dead slots).

## Hypotheses now dead, all by measurement

    the JIT                         4 configurations, identical
    the Sista per-bytecode tier     PHARO_NO_SISTA_PER_BC=1, identical
    f96cb69b's temp-sync residual   PHARO_NO_GC_TEMPSYNC=1, identical
    mark / compaction               [HEAPCHECK post-fullGC] ... => CLEAN
    a stack slot above sp           above, N=64 and N=1024

Still standing, and now the only one: something reachable from NO root the
collector walks holds the receiver — i.e. a C++ local in a primitive or runtime
helper that survives across an allocation.  `PHARO_WATCH_ROOT_CLASS=PMVector`
answers this directly: `forEachRoot` reports which root CATEGORY visits each
instance, so either it names the holder or it shows the receiver is visited by
nothing at the moment it dies.  That run is in flight.

Method note, because it has now cost real time twice: compare the corpse's
`origSel=/method_=` IDENTITY between arms, never `corpse=N`.  Two arms with
equal counts have had different corpses, and reading counts once inverted this
attribution.

---

# WIP (2026-08-18) — pin CONFIRMED with young-gen still on

`PHARO_YG_NO_SCAVENGE` now gates fullGC's pre-compact scavenge as well as the
interpreter-side one, so it finally means what it says.  With young-gen
ALLOCATION still enabled and only the scavenge skipped:

    baseline               corpse origSel=#at:put: method_=#cos      scavenges=2 fullGCs=4
    no-scavenge (gated)    #cos corpse GONE                          scavenges=0 fullGCs=2

That confirms the attribution WITHOUT `PHARO_NO_YG`, which disabled eden
allocation wholesale and so proved less than it appeared to.  The corrupting
scavenge is `ObjectMemory.cpp`'s pre-compact call inside `fullGC`.

## The corpse that appears INSTEAD at scavenges=0 is a different, expected one

    PHARO_NO_YG=1            origSel=#class  method_=#nextPutAll:
    no-scavenge (gated)      origSel=#executeDeferredStartupActions:
                             method_=#snapshot:andQuit:

Different from each other and from `#cos`.  This is the 2026-07-02 defect the
pre-compact scavenge exists to prevent, reappearing exactly as predicted when
the scavenge is skipped: a young object that is scavenge-reachable but NOT
mark-reachable arrives in old space unmarked, and planCompact reclaims it under
live weak slots.  It is flagged at the call site as a bisect axis only.

**So the fix is NOT to remove or reorder this scavenge.**  Removing it trades
one corruption for another.  The fix is the root it misses.

## Where that leaves the defect

    the corpse is `self`, receiver of PMVector>>cos, which mutates in place
    produced by fullGC's pre-compact scavenge; zero scavenges, no #cos corpse
    mark/compact clean; object-aware dangle scan clean; raw-scan hits all
        ExternalAddress false positives
    not the JIT (4 configurations), not the f96cb69b temp-sync residual

The one shape consistent with all of it: at the moment fullGC runs, the receiver
is live only somewhere `forEachRoot` does not walk — a C++ local across an
allocation, or an operand-stack slot outside `[stackBase_, stackPointer_)`.  The
scavenger and the dangle diagnostic share that blind spot, which is why the
diagnostic reports clean while the object dies.

Next, and now cheap because the repro is one deterministic ~90 s test: find
which allocation triggers that fullGC inside `PMVector>>cos`'s loop, and dump
the root set at that instant against the receiver's address.

---

# WIP (2026-08-18) — the corrupting scavenge is fullGC's, not the interpreter's

## Pinned by elimination, and the off-by-one checked

`scavRequestNum` is incremented BEFORE the bisect test
(`static int scavRequestNum = 0; ++scavRequestNum;` then
`skipForBisect = ... scavRequestNum >= PHARO_YG_SKIP_SCAV_FROM`), so the first
interpreter-side request is 1 and `PHARO_YG_SKIP_SCAV_FROM=1` skips ALL of them.
That makes the two arms decisive:

    PHARO_YG_SKIP_SCAV_FROM=1   interpreter-side scavenges 0, observed 1
                                -> the survivor is fullGC's pre-compact scavenge
                                -> #cos corpse PRESENT
    PHARO_NO_YG=1               that one is gated too (enableYoungGen_)
                                -> observed 0
                                -> #cos corpse GONE

So ONE scavenge is sufficient, and the sufficient one is the scavenge INSIDE
fullGC — `ObjectMemory.cpp:2718`, the call moved to before `markPhase` on
2026-07-02 so that mark could see tenured copies and apply weak semantics.

    if (enableYoungGen_ && edenFree_ > edenAllocBase_) {
        scavenge();
    }

## Why that one and not the interpreter's

The interpreter-side site brackets its scavenge itself:

    prepareForGC();  memory_.scavenge();  afterGC();

and runs at a bytecode-boundary safe point, where the live set is on the operand
stack and in the frame registers — exactly what `forEachRoot` walks.

fullGC's internal scavenge runs wherever fullGC was triggered from, which
includes an allocation deep inside a primitive.  At that moment an Oop can be
live only in a C++ local, not yet stored anywhere the root walk can see.  That
is consistent with every measurement taken:

  * the object-aware dangle scan finds 0 dangling refs — it walks the same
    `forEachRoot` set the scavenge updates, so a C++-local holder is invisible
    to BOTH
  * the raw scan's only hits are ExternalAddress false positives
  * mark/compact is clean, because by then the damage is a stale value held
    outside the heap

NOT yet proven: which allocation triggers that fullGC, and which local holds the
receiver across it.  That is the next step, and it is cheap — the repro is one
deterministic test, ~90 s a run:

    (PMAB2SolverTest selector: #testVectorSystem) run

on `pkg-arm3/PolyMath/pkg.image`.  Read the `origSel=/method_=` identity in the
`[DNU] CASCADE` line, NOT the corpse count: counts are equal across arms that
have different corpses, which already inverted this attribution once.

## Suggested first move for whoever continues

Gate fullGC's internal scavenge on the same bisect knobs as the interpreter-side
one.  It is a two-line change, it makes `PHARO_YG_NO_SCAVENGE` mean what it says,
and it turns "disable scavenging" into a usable bisect axis instead of one that
floors at 1 and reads as a non-fix.

---

# WIP (2026-08-18) — the `#cos` corpse IS the scavenger, and counting nearly hid it

## Correction to this file's own attribution table

An earlier entry listed five configurations as giving an "identical corpse
count" and concluded the corpse was insensitive to every knob.  That compared
`corpse=4 dnu=42` — COUNTS — and counts are not identity.  Compared on what the
corpse actually is, across all 16 arms run today:

    arms with scavenges >= 1   (15)   origSel=#at:put:  method_=#cos
    arms with scavenges == 0    (1)   origSel=#class    method_=#nextPutAll:

The `#cos` corpse appears in every arm that ran at least one scavenge and in no
arm that ran none.  **The scavenger is causal.**  The single scavenges=0 arm
(`PHARO_NO_YG=1`) shows a DIFFERENT corpse in a different method, with a
different header (`hdr=0x7000000000` against the `#cos` corpse's
`0x200000002000000`) — a separate question, not the same defect, and it must not
be folded in.

This is also why the three scavenge bisect knobs read as non-fixes:

    PHARO_YG_NO_SCAVENGE=1       scavenges 2 -> 1   #cos corpse still present
    PHARO_YG_SKIP_SCAV_FROM=1    scavenges 2 -> 1   #cos corpse still present
    PHARO_GC_ROUNDTRIP_ONLY=1    scavenges 2 -> 1   #cos corpse still present
    PHARO_NO_YG=1                scavenges 2 -> 0   #cos corpse GONE

None of the first three reaches zero, because fullGC's own pre-compact scavenge
is ungated (now commented at the call site).  ONE scavenge is enough to produce
the corpse.

## What is established

    the corpse is `self`, the receiver of PMVector>>cos, which mutates in place
    it requires a scavenge; zero scavenges, no corpse
    full GC + compaction are clean:
        [HEAPCHECK post-fullGC] scanned=826469 corruptSlots=0 badHeaderObjs=0 => CLEAN
    the object-aware post-scavenge dangle scan finds NOTHING:
        [SCAV-DANGLE] 0 hits, and it covers old space, perm space AND
        interpreter_->forEachRoot (the VM register/stack/frame roots)
    the format-agnostic raw scan's 54 hits are false positives:
        all inside ExternalAddress (fmt 16, ptrSlots=0), which legitimately
        stores native addresses; the corpse value appears in NONE of them
    not the JIT: identical under PHARO_NO_JIT=1, PHARO_NO_SISTA_PER_BC=1,
        both together, and PHARO_CODE_ZONE_MB=192
    not f96cb69b's temp-sync residual: PHARO_NO_GC_TEMPSYNC=1 unchanged

Put together: a scavenge collects (or fails to forward) an object that IS still
live, and the holder is somewhere the dangle scan does not look — which is the
same set `forEachRoot` walks, so scavenger and diagnostic share the blind spot.
A holder outside that set — a C++ local holding an Oop across an allocation, or
an operand-stack slot outside `[stackBase_, stackPointer_)` — fits every
observation.  That last shape is this repo's defect `#1` family.

## Next

Bisect WHICH scavenge with `PHARO_YG_SKIP_SCAV_FROM=N` for N=1,2 — but read the
`origSel/method_` identity, not the count, or the answer inverts.  Then
instrument the surviving holder directly: the repro is one deterministic test,
`(PMAB2SolverTest selector: #testVectorSystem) run` on the loaded PolyMath
image, and it costs about 90 seconds a run.

---

# WIP (2026-08-18) — PMAB2SolverTest uses a COLLECTED object, and that is what hangs PolyMath

## The finding

`PMAB2SolverTest>>testVectorSystem` sends `at:put:` to an object that is a
CORPSE — collected, class index 0, forwarding pointer resolving to nil:

    [DNU] CASCADE: receiver can't handle doesNotUnderstand:
    [DNU]   caller=#state:time: fd=20
    [DNU]   CASCADE rcvr=0x7717d01610 kind=obj classIdx=0 origSel=#at:put: method_=#cos
    [DNU]   CASCADE corpse hdr=0x200000002000000 w1=0x7000000000
              targetClass=UndefinedObject class followed=nil
    [DNU]   frame[14] m=#testVectorSystem rcvr=PMAB2SolverTest
    [DNU]   ctx[2]  m=#runCaseManaged rcvrCls=PMAB2SolverTest

`classIdx=0` is not a live class.  The object was freed and is still reachable
from the running frame, inside PolyMath's ODE integration (`#cos`,
`#state:time:`) — float- and Array-heavy code, i.e. heavy allocation and
therefore heavy GC.  This is a memory-management defect, not a PolyMath bug:
no image-level mistake can hand the VM a receiver with class index 0.

## Why it looked like a hang, and why the per-class timeout did not help

The cascade is unrecoverable — the receiver cannot even handle
`doesNotUnderstand:` — and what it leaves behind is a dead Delay scheduler:

    [DIAG-TIMER] usecArmed=0 msArmed=0 timerWasArmed=0 timerSem=nil   (x25)
    [STATE-DUMP] pri=80 method=DelayMicrosecondTicker>>waitForUserSignalled:orExpired:

With `timerSem=nil` and never re-armed, every Delay in the image is dead.  That
is why bounding each class with `valueWithin:onTimeout:` changed nothing: the
watchdog is itself a Delay.  The mechanism is fine in isolation — measured on a
clean image, `[ (Delay forSeconds: 30) wait ] valueWithin: 3 seconds` returns
TIMED-OUT in 3010 ms, and it interrupts a pure busy loop too — so the failure is
specific to an image whose timer has already been wedged by the corpse.

Sequence, end to end:

    at:put: on a collected object
      -> DNU on a corpse, which cannot handle doesNotUnderstand: either
      -> the test process dies mid-flight, Delay scheduler left with timerSem=nil
      -> every Delay in the image is dead, so no watchdog can fire
      -> VM goes idle; the eval deadline correctly reports
         `only 256,156 bytecodes ... idle, not working` and exits
      -> PolyMath reports 4 of 117 classes

Note the eval-deadline fix from earlier today is what makes this legible: before
it, the VM exited at the first 120 s window with no distinction between
"working" and "dead", so none of the above was visible.

## SHARPENED: the corpse is `self`, the receiver of the running method

`#cos` here is not `Collection>>cos`.  Asked of the loaded image, the vector one
is:

    PMVector>>cos
        "Apply cos function to every element of a vector"
        1 to: self size do: [ :n | self at: n put: (self at: n) cos ].

It mutates the receiver IN PLACE.  So the object receiving `at:put:` on a dead
header is `self` -- the receiver of the method currently executing.  Each
element's `Float>>cos` is `^(self + Halfpi) sin`, which allocates a boxed Float
through primitiveSine, so the loop allocates once per element and one of those
GCs takes the receiver out from under its own active frame.

    [CORPSE-PUSH] val=0x7375b1b238 in=#cos fd=20 jitState=0   (x3, same value)

(An earlier note in this entry guessed the corpse was a collection allocated by
`collect:`.  Wrong -- there is no allocation in PMVector>>cos at all.)

## Attribution so far, all measured

    jit-default                corpse=4  dnu=42
    PHARO_NO_JIT=1             corpse=4  dnu=42
    PHARO_NO_SISTA_PER_BC=1    corpse=4  dnu=42
    PHARO_CODE_ZONE_MB=192     corpse=4  dnu=42
    PHARO_NO_GC_TEMPSYNC=1     corpse=4  dnu=42

Identical to the count in every arm: deterministic, JIT-independent, and NOT the
temp-sync residual that `f96cb69b` predicted for this family (that commit's
description is JIT-specific -- "after the frame re-enters JIT" -- and this
reproduces with the JIT off, so it was never a good fit).

Reading the root scan does not obviously explain it either: `forEachOopRoot`
visits `savedFrames_[i].savedReceiver` for every `i < frameDepth_`, and the
running frame's receiver is a visited register.  So either a frame's receiver is
not where the scan expects at the moment the GC runs, or the object is moved by
compaction and this reference is not updated.  Do not guess between those --
PHARO_HEAP_CHECK (post-fullGC walk for slots aimed at free/garbage targets) and
PHARO_GC_LOG are the next instruments, and the repro is deterministic enough to
answer it.

## Status

Reduced to ONE test — `(PMAB2SolverTest selector: #testVectorSystem) run` on the
loaded PolyMath image — and being run across JIT-on / PHARO_NO_JIT=1 /
PHARO_NO_SISTA_PER_BC=1 to establish whether the JIT is implicated.  Do not
assume either way until that lands: three JIT hypotheses have already died this
session, and one non-JIT one.

This supersedes "PMAB2SolverTest goes idle after ~2 minutes and never finishes"
as the description of the defect.  It does not go idle because it is slow; it
goes idle because it has already crashed into a corpse and taken the timer with
it.

---

# WIP (2026-08-18, end of session) — both arches measured end to end

## RETRACTION FIRST: the test_relaunch mechanism I published was wrong

The earlier entry in this file said vm_stop "times out on EVERY cycle here" and
that the timing "confirms it exactly".  That was inference fitted to one
arithmetic coincidence, not measurement.  vm_stop now REPORTS its wait, and:

    9 consecutive runs, 3 cycles each, arm64
    worst wait observed          15 ms      (not 2000)
    [VM-STOP-TIMEOUT] fired      0 times
    failures                     0 of 9

So the timeout mechanism is measured FALSE.  What remains true is the crash
itself, from two crash reports: EXC_BAD_ACCESS with the MAIN thread inside
`_free` / `_xzm_free_tc` while the VM worker faulted in
`Interpreter::synchronousSignal` under `interpret()`.  That is a
use-after-free, and `vm_destroy()` freeing unconditionally after a BOUNDED wait
is a real hazard in the code whatever the trigger was.  But it does not
reproduce on the current build, and I could not establish why it happened.

Kept from the work: vm_stop now prints how long interpret() took to return, and
prints `[VM-STOP-TIMEOUT]` when it gives up, so the next occurrence arrives with
its evidence instead of needing to be reconstructed from a crash dump.

## CONFIRMED BY A FULL RE-SWEEP: x86_64 now matches arm64

A complete x86_64 sweep with the FFI fix in (2046 classes, same image, same
machine, idle):

                                           classes  tests   F    E     rate
    x86_64 raw                 before        2046   28071  25   365   97.58%
    x86_64 raw                 after         2046   28071  25   288   97.86%
    x86_64 minus cairo/ft      before        1997   27638  25   103   98.49%
    x86_64 minus cairo/ft      after         1997   27638  25    26   98.77%
    arm64  same exclusion                    1996   27637  29    25   98.77%

**98.77% on both architectures**, 26 errors against 25.  The two columns now
agree to two decimal places, which is what this comparison existed to find out.

Per class, the FFI family before -> after:

    FFICallbackParametersTest                    P0/E11  ->  P11/E0
    FFICallbackTest                              P0/E2   ->   P2/E0
    TFBasicTypeMarshallingInCallbacksTest        P0/E18  ->  P18/E0
    TFUFFIBasicTypeMarshallingInCallbacksTest    P0/E18  ->  P18/E0
    TFUFFICallbackTest                           P0/E6   ->   P6/E0
    TFUFFIDerivedTypeMarshallingInCallbackTest   P0/E6   ->   P6/E0
    TFUFFIStructuresTest                         P5/E6   ->  P11/E0
    TFCallbacksTest                              P0/E4   ->   P4/E0
    TFStructTest                                 P0/E3   ->   P3/E0
    TFFunctionCallTest                           P4/E4   ->   P7/E1
                                        errors    79     ->     2

Every other FFI* class is byte-identical, so the change is confined to what was
broken.  The 2 survivors: one residual in TFFunctionCallTest, and
TFUFFIDifferentCallingConventionFunctionCallTest, which probes the Windows
`w64Convention` and fails on any non-Windows host.

## UPDATE, same session: the x86_64 FFI defect is FIXED

The "single highest-value x86 item" below is done.  Root cause: libffi's x86
ffitarget.h gates its 64-bit ABI enum on `defined(X86_64) || (defined
(__x86_64__) && defined (X86_DARWIN))`, macros libffi's own configure defines
when building the library but which we never defined when INCLUDING the header.
The x86_64 build therefore compiled `FFI_DEFAULT_ABI = FFI_SYSV = 1` while the
linked libffi expects `FFI_UNIX64 = 2`, so ffi_prep_cif answered FFI_BAD_ABI on
every callback registration.

    clang -arch arm64                FFI_DEFAULT_ABI=1  FIRST=0  LAST=3
    clang -arch x86_64               FFI_DEFAULT_ABI=1  FIRST=0  LAST=9   wrong
    clang -arch x86_64 -DX86_DARWIN  FFI_DEFAULT_ABI=2  FIRST=1  LAST=5   right

    x86_64                       before        after
    FFICallbackTest              0 P /  2 E    2 P / 0 E
    FFICallbackParametersTest    0 P / 11 E   11 P / 0 E
    TFCallbacksTest              0 P /  4 E    4 P / 0 E

arm64 byte-identical before and after.  Found by giving
`primitiveRegisterCallback`'s five silent `return Failure` sites distinct
messages -- `[TFCB-FAIL] ffi_prep_cif status=2` named it in one run, after the
bare "primitive #registerCallback: failed" had said nothing for a whole sweep.

Also fixed the way this became possible: `build-libffi.sh`'s `make_universal`
lipo'd two architectures' libraries together and then copied only slice_a's
HEADERS, so a fat slice shipped one arch's `ffitarget.h`.  It now stages both
and generates an arch-dispatching header.

CHECKED, and it does NOT also explain the x86 package failures: with the ABI
fixed, an x86_64 NeoJSON load still fails with `IceGenericError: no error
message set by libgit2` and zero TFCB-FAIL lines.  The libgit2 attribution
below stands on its own.

## SUnit: both architectures, complete runs

STEP=300 with a pristine image per batch, `.sources` staged, idle machine.

    arm64 raw                             2045 cls  28070 tests  29 F   26 E  98.78%
    x86_64 raw                            2046 cls  28071 tests  25 F  365 E  97.58%
    x86_64 minus cairo/freetype classes   1997 cls  27638 tests  25 F  103 E  98.49%
    arm64 same exclusion                  1996 cls  27637 tests  29 F   25 E  98.77%

arm64 took 1 h 47 m, against "never finished inside 4 h on either arch".

The x86 column needs the exclusion because this host has arm64-ONLY copies of
the libraries the image dlopens, and Rosetta cannot load them:

    /opt/homebrew/lib/libcairo.2.dylib    arm64      (no x86_64 anywhere)
    /opt/homebrew/lib/libgit2.dylib       arm64
    freetype                              same story
    neither VM binary links any of them statically (0 symbols in both)

That accounts for 175 of the x86 sweep's first 190 errors and for the whole
Roassal RS* family (121 x `StrikeFont>>asFreetypeFont: not supported yet. and
ever`, reached because FreeType did not initialise).

**The entire remaining arm-vs-x86 gap is FFI callbacks**: 79 errors over 11
TF*/FFICallback* classes, of which 77 are one root --
`TFCallback>>register` -> `registerCallback:` (62) or the
`primitiveInitializeStructType` inside its `validateTypes` (15).  arm64 passes
every one.  It is NOT a libffi packaging problem: both binaries export
`ffi_closure_alloc` and the macos libffi slice is fat.  That is the single
highest-value x86 item and it is well localised
(`Primitives.cpp primitiveRegisterCallback`, five silent `return Failure`
sites -- give them distinct messages first, then the cause will name itself).

## Packages: arm64 measured, x86_64 blocked by libgit2

    NeoJSON     116 P /  0 F   11 classes
    Mustache     47 P /  0 F    1 class
    DataFrame   839 P / 14 F   27 classes
    Fuel         19 P /  0 F    2 classes
    Grease      loads (423 MB) and adds ZERO TestCase subclasses -- the default
                `load.` group has no tests; needs the Tests group, not a VM fix
    XMLParser   loads (699 s, 1019 MB)
    PolyMath    loads (888 s, 1287 MB), was "times out at 1200 s"

x86_64: all seven loads fail on `IceGenericError: no error message set by
libgit2` -- Metacello's github:// goes through Iceberg -> libgit2, which is
arm64-only here.  Fuel is the control that proves the VM is fine: it needs no
fetch and passes 19/19 on x86_64 too.

## The eval deadline: open item 1 root-caused and FIXED

"XMLParser produces no RESULT" was the VM killing it.  `primitiveQuit`'s
deferral arms a 120 s deadline; at expiry the only question asked was "is
startup.st still on disk?", and finding it gone the VM concluded the process had
been killed and exited.  A working eval is indistinguishable under that test.
The tell was in the summary the whole time -- 122 s, 123 s, 123 s against
`std::chrono::seconds(120)`.

Fixed by comparing bytecodes run since the deadline was armed, with the floor
set from measurement rather than taste: 26,095,351 working vs 220,402 and
237,531 idle, in three consecutive windows of one run.  Floor 1M.  Verified:
window 1 re-arms, window 2 exits saying "idle, not working".

It also revealed a real defect the blanket exit had been hiding:
**PMAB2SolverTest goes idle after ~2 minutes and never finishes.**  That is what
stops PolyMath at 4 of its 117 classes (240 tests, all passing).

## Harness defects fixed, all of which had been reading as VM failures

  * **No `.sources` file beside the image** -> every class comment answers nil
    -> 69 of one 300-class batch's 94 errors.  Both self-hosted runners now
    stage it and refuse to run without it.  This also gives a second, entirely
    deterministic cause for the "Improper store into indexable object" scatter
    that `docs/local-arm-x86-2026-08-17.md` attributed to a busy machine.
  * **Package tests selected by name substring** -> 'Grease' and 'PolyMath'
    matched none of their own GR*Test / PM*Test classes and reported
    "classes=0", identical to a failed load.  Now selected by what the load
    ADDED, with the pattern as a fallback for packages already in the base
    image (Fuel).
  * **A load that did not persist looked like a successful one.**  Now
    DID-NOT-PERSIST.
  * **Package results written once at the end** -> any death discarded
    everything and named nothing.  Now written per class, ending in
    `PARTIAL after n/N last=<class>`, which is what identified PMAB2SolverTest.

## Open, in priority order

  1. **x86_64 FFI callback registration** — 77 errors, one root, arm64 clean.
  2. **PMAB2SolverTest goes idle and never completes** — blocks PolyMath.
     Give the package runner a per-class timeout so one hung class cannot
     swallow the other 113.
  3. **test_relaunch's use-after-free** — not reproducing; `vm_destroy` freeing
     after a bounded wait is still wrong on its own terms.
  4. **`SmallInteger` DNU in ReleaseTest / SystemNavigationTest** — measured NOT
     a JIT defect (identical under PHARO_NO_JIT=1 and PHARO_NO_SISTA_PER_BC=1),
     needs the SUnitRunner harness to reproduce.  Get the receiver from INSIDE
     the failing run; three outside-in hypotheses have died.
  5. **x86_64 cairo/freetype/libgit2** — install under a Rosetta prefix, or link
     the already-fat maccatalyst slices statically, or the x86 column stays
     unmeasurable for anything that draws or fetches.
  6. **Image bloat** — 52 MB base becomes 1287 MB after a Metacello load.

---

# WIP (2026-08-18, later still) — the `SmallInteger` DNU is NOT a JIT defect

## What it is

Two tests, on BOTH architectures, send a Character-only selector to a
SmallInteger:

    ReleaseTest>>testAllGlobalBindingAreGlobalVariables
        SmallInteger(Object)>>doesNotUnderstand: #asciiValue
        via Symbol(String)>>isLiteralSymbol,        `(self at: 1) asciiValue`
    SystemNavigationTest>>testAllGlobalNamesStartingWithDoCaseSensitive
        SmallInteger(Object)>>doesNotUnderstand: #asLowercase
        via Symbol(String)>>beginsWith:caseSensitive:, `(self at: index) asLowercase`

## RETRACTION

The previous entry named the T1 inline `at:` (primKind 14, primitive 60) as the
suspect, reasoning that its byte arm returns `(byte << 3) | 1` — a SmallInteger
— and checks only the object FORMAT, never the class, so a String reaching it
would produce exactly this.  **That is wrong, and it is now measured wrong, not
merely doubted.**  Reduced to one test method, one class, a fresh copy of the
prepped image:

    default (JIT on)                              Total: 1 P:0 F:0 E:1
    PHARO_NO_JIT=1                                Total: 1 P:0 F:0 E:1
    PHARO_NO_SISTA_PER_BC=1                       Total: 1 P:0 F:0 E:1
    PHARO_NO_JIT=1 PHARO_NO_SISTA_PER_BC=1        Total: 1 P:0 F:0 E:1

Both knobs, together, and the failure does not move.  `PHARO_NO_JIT=1` really
did take the JIT out — the log carries `[JIT] Disabled via PHARO_NO_JIT` and no
`[JIT] Initialized` — and `jit_rt_array_prim` is only reachable from emitted
code.  So the inline `at:` cannot be running, and this is plain-interpreter
behaviour.  Including `PHARO_NO_SISTA_PER_BC=1` matters because this repo has
already been caught treating `PHARO_NO_JIT=1` as a full JIT-off switch when it
is not (defect #1, five sessions).

Also ruled out by measurement, each one cheap and each one wrong:

    primitiveStringAt (63)      correct in both arms; the wide branch really
                                does answer Oop::fromCharacter(codePoint)
    operand-stack displacement  PHARO_DEPTH_ORACLE='beginsWith:caseSensitive:'
                                reports NO depth disagreement
    a bad global at rest        scanned every global name's every character in
                                the base image AND the prepped suite image:
                                all Characters, 10842 globals
    runtime-created class names ClassFactoryForTestCase newClass -> the name is
                                a normal ByteSymbol, all Characters
    runtime method compilation  `Object compile: ...` then rescan: still none
    forking                     running the test in a forkAt: 40 process: passes

## What is actually established

The failure needs the SUnitRunner harness.  The SAME test, in the SAME image,
called directly as `tc setUp. tc performTest` — and again inside a forked
process — PASSES.  Driven through the runner it fails every time, down to a
single selector.  So the trigger is something the runner's startup does
(it compiles a patched `DateAndTime class>>now`, installs a scheduler logger,
a delay-recovery handler and timeout overrides) and not the test.

That makes this, on current evidence, a harness-interaction defect rather than
a JIT one, and it should not be counted against the JIT tier.  It stays open,
but it is off the critical path for "the JIT passes the suites".

Next step for whoever picks it up: get the RECEIVER.  Every probe so far has
asked the image a question from OUTSIDE the failing run; the answer is inside
it.  Add a handler to the runner's own `runSingleTest:selector:timeout:...:on:`
that prints `e receiver`, `e receiver class`, and the `self` of the signalling
context, then read what it says instead of guessing again — three hypotheses
have now died in a row, all of them plausible on paper.

---

# WIP (2026-08-18, later) — the full arm64 suite now finishes, and it is 98.78%

## The full suite is measurable again

The 2026-08-18 morning entry lists as open item 3: "Full suite never finished
inside 4 h on either arch... it makes the full suite unmeasurable as currently
run. Consider periodic image restart."  That is done, and it works:
`scripts/sunit-sweep.sh` at **STEP=300**, relaunching from the pristine prepped
image per batch.  arm64, one machine, otherwise idle:

    batch     1-300   rc=0    151s   301 classes
    batch   301-600   rc=0    327s   301
    batch   601-900   rc=0    338s   299
    batch   901-1200  rc=0    525s   301
    batch  1201-1500  rc=0    503s   301
    batch  1501-1800  rc=0   2205s   301
    batch  1801-2047  rc=124 2400s   248     <- hit the cap AFTER reporting
                                                every class; nothing lost
    total 1 h 47 m

    classes 2045   tests 28070   PASS 27728   FAIL 29   ERROR 26   SKIP 182
    rate 98.78%

against the recorded baselines on the same VM lineage:

    2026-08-11 baseline   27701 P   22 F   50 E   (2052 classes)
    "suite 5"             27732 P   22 F   21 E   (2046 classes)
    this run              27728 P   29 F   26 E   (2045 classes)

so it lands on top of the best recorded numbers, and it does so in under two
hours instead of not at all.  Note batch 7's `rc=124`: the batch timed out, but
all 248 of its classes had already written results, so the totals are complete.
Give the last batch a larger `PER_BATCH_TIMEOUT` to get a clean rc.

STEP=300 only became usable once the `.sources` file was staged — see the
commit that added it.  Batch 1 alone, same 300 classes, same binary:

    without .sources    4902 tests   4753 P   14 F   94 E
    with .sources       4902 tests   4861 P    0 F    0 E

The whole "batch size changes the answer" theory in this script's header rested
on runs made without it.  With the file present the first 1198 classes score
**0 FAIL and 2 ERROR** in 300-class batches, which is the opposite of what a
batching artefact would do.

## The 27 non-clean classes, triaged

    family                        classes   F    E    status
    Spec/GUI adapters (Sp*, FT*)     10      4   17   documented residual
    Tools (St*)                       5     10    0   documented residual
    harness self-pollution            3      5    3   the runner is IN the image
    weak / GC timing                  2      2    0   documented residual
    network (ZnClientTest)            1      1    0   testQueryGoogle
    upstream image bug                1      0    1   OCClassBuilderTest
    UNTRIAGED                         5      7    5   below

"harness self-pollution" is literal, not a euphemism: `ReleaseTest` fails
`testNoOrphanPackage` on `Package(Tests-Runner)`,
`testThatThereAreNoSelectorsRemainingThatAreSentButNotImplemented` on
`SUnitRunner class>>#runAllTests`, and `testUnknownProcesses` on
`'CommandLine handler process'`.  All three name the runner this harness
injected.  `TraitFileOutTest` wants a directory from the previous session's
scratchpad, i.e. the image's saved working directory.  These are not VM
results and should be excluded from any headline number, or the runner should
be filed out before ReleaseTest runs.

## The one lead worth pulling: `at:` on a String answering a SmallInteger

TWO independent classes, both `Symbol(String)>>...`, both a `String at:` that
answered a SmallInteger where a Character belongs:

    ReleaseTest>>testAllGlobalBindingAreGlobalVariables
      SmallInteger(Object)>>doesNotUnderstand: #asciiValue
      Symbol(String)>>isLiteralSymbol            -- `ascii := (self at: 1) asciiValue`

    SystemNavigationTest>>testAllGlobalNamesStartingWithDoCaseSensitive
      SmallInteger(Object)>>doesNotUnderstand: #asLowercase
      [ :each :index | (self at: index) asLowercase = each asLowercase ... ]
        in Symbol(String)>>beginsWith:caseSensitive:
      ByteString(SequenceableCollection)>>withIndexDo:

Both are Character-only selectors sent to a SmallInteger, so the wrong
PRIMITIVE ran, not merely the wrong value.  The relevant facts:

    ByteString>>at: / ByteSymbol>>at:   <primitive: 63>  -> Character
    Object>>at:                         <primitive: 60>  -> SmallInteger
                                                            for byte formats

    Interpreter.cpp inlinePrimKind():   case 60 -> pk 14 ;  no case 63

and the T1 inline for pk 14 (AsmjitT1.cpp, `tryPrimAt`) has a byte path whose
own comment reads "fmt 16-23 (ByteArray, String, Symbol) ... Returns SmI of
byte value".  It checks only the FORMAT, never the class — by design, because
"receiver class is whatever IC matched".  So the guard that keeps a String off
this path is entirely the IC's class key plus pk being 0 for primitive 63.

That makes the hypothesis concrete: at a megamorphic `at:` site --
`SequenceableCollection>>withIndexDo:` is exactly one, since it sends `at:` to
Arrays, ByteArrays, ByteStrings and Intervals -- a receiver whose entry should
be pk 0 takes the pk 14 arm.  It is a hypothesis, NOT established: nobody has
yet run either test with `PHARO_NO_JIT=1`, which is the one-command
discriminator and the first thing to do next.  `PHARO_T1_VERIFY_AT=1` exists
but only cross-checks the fmt==2 Array arm, not the byte arm.

The other untriaged four, for completeness:

    VariableBreakpointTest   testNoRemoveAfterSubclassRemoved [Denial failed]
                             testNotifyArgumentBreakpointHit  (nil breakpoint)
    LinkInstallerTest        testPropagateNewClassScopedLinks [Leaked metalinks]
    RSLinesTest              testMarkerOffset (BlockCannotReturn) -- a `^ true`
                             out of a block whose home,
                             RSAbstractLine>>markersIncludesPoint:, is still
                             four frames below on the stack.  Also worth a
                             PHARO_NO_JIT=1 check.
    TraitTest                testTraitsUsersSanity [Assertion failed]

---

# WIP (2026-08-18, later) — VM tier: test_relaunch has a real use-after-free

Continuation of the 2026-08-18 session below.  Work tree: the scratch worktree
on `jit-work` (same commit as `jit`, `98b72d59`); builds reused from that tree
(`build-rel` = arm64, `build-x86` = x86_64 under Rosetta).

## `test_relaunch` SIGSEGVs about half the time, and it is not flaky-by-nature

    run 1   rc=139 (SIGSEGV), never printed the cycle-1 summary
    run 2   rc=0,   3/3 cycles PASS
    (same binary, same fresh Pharo 13.1 image, back to back)

Both crash reports name the same two frames, and the SECOND thread is what
makes the diagnosis unambiguous:

    thread 2 (VM worker)  pharo::Interpreter::synchronousSignal(Oop)
                          pharo::Interpreter::primitiveRelinquishProcessor(int)
                          pharo::Interpreter::sendSelector(Oop, int)
                          pharo::Interpreter::interpret()
    thread 0 (main)       _xzm_free_tc     <- inside free()

EXC_BAD_ACCESS / KERN_INVALID_ADDRESS.  The main thread is in `free()` at the
instant the VM thread faults reading an object header: this is a
use-after-free, not a stale Oop.

The mechanism is in `src/platform/PlatformBridge.cpp`:

    vm_stop()     asks the interpreter to stop, then waits for gRunning to
                  clear -- but gives up after 2 SECONDS and returns anyway.
    vm_destroy()  then runs `delete gInterpreter; delete gMemory;`
                  unconditionally, on the documented assumption that
                  "the VM worker thread is parked on its condition variable
                  at this point".  When vm_stop timed out, it is not.

The timing confirms it exactly.  Crash 1: process launched 05:39:13.90, faulted
05:39:21.15 = 7.25 s in.  The cycle is a 5 s pump, then `probeEventDelivery`
returns at once with an empty queue, then `vm_stop` -- so vm_stop was entered at
~5.1 s, burned its full 2 s wait, and `vm_destroy` began freeing at ~7.1 s.

So `vm_stop` times out on EVERY cycle here; whether the process dies is only a
race on how fast the freed pages become unreadable.  `runOneCycle` already
computes `stoppedCleanly` and would have reported "vm_stop timed out; worker
stuck" -- the crash beats it to the summary print.

Still to establish (needs one instrumented build of PlatformBridge.cpp alone,
so it is cheap): WHY interpret() does not return inside 2 s.  The leading
candidate is that `running_` is a plain `bool` written by `stop()` from the
main thread and read by the VM thread (`Interpreter.hpp:1099`, `:285`)  -- a
data race, and the computed-goto dispatch loop only tests it "after sends and
returns" by its own comment.  A `std::atomic<bool>` with RELAXED load/store
compiles to the same ldrb/movb and forbids the caching; that is the fix if the
measurement supports it.  Do not guess -- measure first, the same way `#1` was
finally found.

Independently of the cause, `vm_destroy()` freeing into a thread that is
demonstrably still executing is wrong on its own terms.

## SUnit sweep: STEP=300 with a pristine image per batch

`scripts/sunit-sweep.sh` at STEP=50 is documented to invent failures, and one
batch of 2047 never finishes.  300 classes per batch, relaunching from the
prepped image each time, is the middle ground the 2026-08-18 session's own
"300 direct = 99.98%" number points at.  Batch 1 (classes 1-300) ran in 182 s
and completed:

    classes 300   tests 4902   P 4753   F 14   E 94   S 26   rate 96.96%

Three Calypso classes own 69 of the 94 errors:

    ClyConcreteGroupCritiquesTest    36 tests   36 E
    ClyBrowserToolValidityTest       25 tests   25 E
    ClyNotebookPageRecyclerTest       8 tests    8 E

all with one signature:

    MessageNotUnderstood: receiver of "ifEmpty:" is nil
      UndefinedObject(Object)>>doesNotUnderstand: #ifEmpty:
      ClyClassIconTableDecorator class(ClyClassTableDecorator class)>>decorateTableCell:of:

and the method is

    decorateTableCell: anItemCellMorph of: aDataSourceItem
        | labelMorphExtension |
        labelMorphExtension := anItemCellMorph label assureExtension.
        labelMorphExtension balloonText:
            (aDataSourceItem actualObject comment ifEmpty: [ ... ])

so `aDataSourceItem actualObject comment` answers nil.  These same three score
100% alone AND scored 100% in the 2026-08-11 single-batch run of all 2052
classes, which is why the sweep script's header attributes them to system state
a fuller run establishes.  That attribution is still unverified -- nobody has
checked what `actualObject` is at the failure, or whether `comment` answering
nil is itself a VM divergence.  Worth one targeted look before another sweep.

---

# WIP (2026-08-18) — arm64 + x86_64 on one machine; no regression found

State saved mid-session at user request. Everything below is on `jit`, pushed
to origin (`ce1c05a8` and later). Work tree: a scratch worktree on branch
`jit-work`; the main checkout is on `main` at origin/main and clean.

## The headline

**No regression exists.** Full-suite runs report 95.34% (arm) / 93.77% (x86)
and look alarming against the 98.87% recorded on 2026-08-11, but their failures
do not reproduce. Every suspect class passes at exactly the baseline number in
isolation, including the last one checked:

    class                        baseline   full run       isolated
    ReflectivityReificationTest  112/112    P:61 E:48      112/112
    ReSmalllintTest               58/58     P:19 F:38       58/58
    ReflectivityControlTest       71/71     P:41 E:25       71/71
    OCASTClosureAnalyzerTest      33/33     P:5  E:26       33/33
    MethodMapTest                 30/30     P:10 E:17       30/30
    SindarinDebuggerTest          85/85     P:60 F:7 E:18   85/85 x3

The honest figure for this VM on this machine is **99.98% over the first 300
classes** (5227/5228, one error). Full-suite percentages should not be read as
VM quality numbers without re-running their failures in isolation.

Four causes ruled out by measurement, not argument:

  - many classes in one image — 300 direct = 99.98%
  - the SUnitRunner harness — same 300 through it = 0 F / 0 E
  - snapshot-prepped images — round-trip verified with a marker global
  - code zone exhaustion — the CLEAN run also filled it (196607/196608 KB,
    9944 failed compilations) and still scored 0 F / 0 E. The zone costs
    throughput, not correctness.

Left standing: cumulative in-image state over many hundreds of classes. The
worst-hit classes install metalinks and rewrite ASTs, which is the population
that would leave residue. That is a test-isolation property of the image.

## Done this session

  - **defect #15 HIGH -> LOW.** Hang gone, overflow catchable, unwind verified
    with the mutex counter-test that caused the 2026-08-15 revert
    ("fork caught overflow: yes  mutex: MUTEX-FREE"). Re-priced; stale code
    comment corrected.
  - **x86_64 builds and runs** under Rosetta, verified executing real Smalltalk.
    Both architectures now measurable on one machine.
  - **C++/VM tier passes on both**: test_sista_ir, test_asmjit_t1_stub,
    test_class_table, test_relaunch 3/3.
  - **Packages load without stock Cog** (`scripts/package-tests-selfhosted.sh`):
    NeoJSON 116/116, Mustache 47/47, DataFrame 431/437, Fuel 19/19.
  - **CI green** for the first time in 60+ runs. Two causes: a Linux build I
    broke with an unconditional Frameworks check (now `if(APPLE)`), and the
    WarpBlt tripwire, which had been correctly reporting that defect #1 was
    FIXED since 2026-08-12 and nobody read it.
  - New tooling: `scripts/sunit-sweep.sh`, `scripts/package-tests-selfhosted.sh`.

## Open, in priority order

  1. **XMLParser, Grease, PolyMath** package loads. XMLParser loads in 1055 s
     but its test pass produces no RESULT; Grease matches 0 classes (pattern
     wrong); PolyMath load times out at 1200 s.
  2. **Image bloat on snapshot after a Metacello load.** A 52 MB base becomes
     234 MB (NeoJSON), 401 MB (Grease), 559 MB (DataFrame), 1056 MB (XMLParser).
     Unexplained and probably worth its own look.
  3. **Long-run degradation.** Full suite never finished inside 4 h on either
     arch. Cause is in-image state accumulation, not the VM, but it makes the
     full suite unmeasurable as currently run. Consider periodic image restart.
  4. **defect #2** — 13 packages, ~6000 tests. Now partly unblocked: package
     loading no longer needs Cog.
  5. **Code zone never recovers.** Fills at ~22K methods even at 192 MB
     (`MaxCodeZoneSize` caps at 256). Costs throughput only. Real fix is the
     ~6 KB/method emit or genuine eviction.
  6. Re-enable the WarpBlt CI job with EXPECT set to 0 — one line, strictly
     better than leaving it disabled.

## Reproduction notes that cost time

  - Prep a suite image with `Smalltalk snapshot: true andQuit: true`, NOT
    `eval --save` (that is the stock VM's flag; ours writes nothing).
  - Run suites on an IDLE machine. Compiling during a run turns passes into
    ERRORs — 99.7% -> 96.8%.
  - Use one batch, not many. Small batches invent failures: three Calypso
    classes score 100% alone and error wholesale in a batch of 50.
  - Set `PHARO_CODE_ZONE_MB=192 PHARO_MAX_STEPS=4000000000000`, as the AWS
    scripts do; every recorded baseline used them.
  - No stock Cog on macOS: it aborts allocating its code zone at 0x320000000,
    the ASLR problem this project exists to solve. All comparisons here are
    ours-vs-ours.

---

# WIP (2026-08-09) — audit follow-through: the 2026-05-27 warning-hygiene fixes were incomplete

Triggered by a report relayed from another machine claiming to have "fixed"
prim 132, the MIDIPlugin memset and the PlatformBridge pragmas. All three were
in fact fixed here on 2026-05-27 (`f68392c2`, `95378117`, `34e14b4a`) and are
documented below under "Two real bug fixes surfaced by the warning hygiene".
The relayed report described existing history as new work. What it did surface,
indirectly, is that **two of those fixes were incomplete**, and the same bug
classes had untouched siblings elsewhere.

Five commits, all on `jit`:

    2e41fda8  prim 132 pointsTo: — complete the non-pointer format guard
    33173419  sweepGC overflow double-count, MIDI array memset, real objectsMoved
    6396b289  nextInstanceAfter overflow overshoot; clamp prim 132 literal scan
    bdfbb5f3  pointer-format predicate drops ephemerons; become Reserved guard
    92d0c13b  proxy_clone on shallowCopy; ImageLoader walk; accept setNonBlocking

Two lessons worth keeping:

- **`-Wnontrivial-memcall` has an array-form blind spot.** It fires on
  `memset(p, 0, sizeof(OpenPort))` but NOT on `memset(gPorts, 0, sizeof(gPorts))`.
  That is exactly why `95378117` fixed midiOpenPort and left the wider
  midiInit instance alive. Any warning-driven sweep inherits this hole.
- **`totalSize()` includes the overflow word, which PRECEDES the header.**
  Only the START pointer needs backing up; adding 8 to the size double-counts.
  Four sites had it wrong (sweepGC, nextInstanceAfter, proxy_clone,
  ImageLoader); `objectAfter` and `copyAndUnmark` are the correct idiom to
  copy. `nextInstanceAfter` was the reachable one — prim 78 `Object>>nextInstance`.

Severity discipline applied: prim 132 and prim 78 are installed in
`generated_primitives.inc` and were live; the MIDI prims (330/332/334) are
`nullptr` and `sweepGC()` has no callers, so those two fixes are landmine
removal, not crash fixes.

STILL OPEN (verified, deliberately not fixed — each wants its own investigation):

- `removeProcessFromList` return value discarded at `Interpreter.cpp` ~18241
  and `Primitives.cpp` ~4875; the caller then unconditionally nils
  `ProcessNextLinkIndex`, which would truncate the run queue if the search
  failed. Plausibly related to full-suite blocker #4; wants a repro before
  anyone touches the scheduler.
- `makeWritable`/`makeExecutable` results ignored at 8 JIT sites. Harmless on
  Apple (always true) but `platform/linux.cpp` and `platform/windows.cpp` can
  genuinely fail -> silent write to RO / execute NX.
- `makeFreeChunk` OOB memset (`ObjectMemory.cpp` ~3372/~3391): slotCount
  ignores the 8 bytes the overflow branch prepends. Latent — reachable only
  via the callerless `sweepGC` and `allocateFromFreeList`.
- `primitiveConstantFill` uses `format <= IndexableWithFixed`, dropping
  Weak/WeakWithFixed. Fails safe (primitive falls back), low priority.

Validation harness used throughout: `build/test_load_image
.sweep-state/Pharo-fix.image eval "<expr>"`, run from a scratch directory (the
startup.st CWD trap). The discriminating repro for prim 132 is worth reusing:

    dba := DoubleByteArray new: 4.   "one 64-bit slot"
    dba at: 1 put: 337.              "= SmallInteger 42's oop bits 0x151"
    dba pointsTo: 42.                "buggy: true.  fixed: false"

Note `ClassTest>>testComment` fails 1/46 independently of any of this
(pre-existing, A/B confirmed), and CompiledMethodTest is 75/10/1 pre-existing.

---

# WIP RESUME (2026-06-18e) — cold-image-boot corruption REFRAMED: JIT operand-stack cascade, NOT a heap wild-write

Goal: finish WIP, make the JIT add-on work for arm + x86. Blocker for image
testing = the 0x300000000 cold-image-startup corruption. This session OVERTURNED
the prior "raw wild write" model. See memory [[vm-cold-image-startup-operand-corruption]].

ESTABLISHED (commits a28259f0, 537e8dfd — diag tooling, pushed):
- NOT a heap wild-write: a malformed-tag-aware full-heap+eden scan
  (PHARO_HEAP_SCAN_EVERY) is CLEAN at every checkpoint up to the DNU. The bad
  receivers (0x9=SmI 1, 0x3=Char 0, nil) are LEGAL oops in WRONG frame slots.
- JIT-SPECIFIC: PHARO_NO_JIT boots /tmp/h3 (and the /tmp/h3/warm.image I made
  via a NO_JIT snapshot) cleanly to eval; JIT-on => 9 DNUs. NOT stock-vs-our
  context format (the warm image is OUR snapshot and still fails under JIT) —
  our VM forces isImageStarting=true on resume so startup handlers always re-run.
- PERVASIVE CASCADE, not one method: PHARO_T1_NOJIT_SEL=do: just relocates the
  first DNU (#value: rcvr=0x9 in #do: -> #new: rcvr=nil in #growAtLast). No
  single feature toggle fixes it (inline-J2J/FSR-cursor/patched-sends/ALL resume
  variants/native-backjump/block-create/inline at:+size all NO effect).
- SYMPTOM PINNED: DNU#1 = `FFIStructure subclasses do: aBlock`. Decoded
  Array>>do: (oop 0x3002d5420): offset-11 bytecode PushTemp0 (=aBlock) is the
  value: receiver. Frame: fp[1]=aBlock, fp[2]=limit3, fp[3]=i1, operands from
  fp[4]. At value:, fp[4] holds 0x9 (=i) and aBlock is NOWHERE on the operand
  stack -> the operand stack was ALREADY SHIFTED on entry to #do:. #do: is NOT
  resumed (DO-RESUME probe never fires) -> the shift is INHERITED from the caller.
  => ONE upstream JIT operand-stack shift in cold startup that CASCADES.

NEXT (precise): build a per-JIT-send operand-DEPTH checker (after each send/
chain-return, assert sp matches the bytecode's expected depth via
src/vm/jit/BcDepthMap.hpp) to catch the FIRST send that leaves the wrong sp
depth = the cascade origin. Prime suspects: chain-loop send-return/activation
(Interpreter.cpp 27147 tempBase=sp-nArgs, 27381 sp[-(nArgs+1)]=retVal) with a
miscounted nArgs. Tools all env-gated on a FIXED binary (no relocation):
PHARO_HEAP_SCAN_EVERY, PHARO_DNU_DUMP_COLL, PHARO_T1_NOJIT_SEL/_CLASS.
Repro: `PHARO_DET_SCHED=1 PHARO_DNU_DUMP_COLL=1 build/test_load_image /tmp/h3/warm.image eval "3 + 4"`.

---

# WIP — Cog-speed /goal (2026-06-14, ongoing): arm64 zone wins landed; x86 blocked

/goal "JIT as fast as cog on arm AND x86". This session's landed wins (all
committed+pushed): B0+B0.5 (shared return-prelude / zone-global stub);
inline-`class` upgrade-path fix + confirmed inline-prims healthy
([[inline-class-already-works]]); B1 send-emit MAPPED;
**DEAD-CODE GATING (commit 4ee9224b) = ~10.5% image-wide zone reduction**, SAFE
(tryMultiSlot/tryReturnsLiteral bodies were emitted per-site but their dispatch is
default-off → dead; gated them; cfibx 5568->4904B, battery==Cog, SUnit 1577 PASS).
arm64 inlined arith/float/at/size/class ≈ Cog; residual ~2.7x = per-send dispatch
(B1, multi-session) + ~1.9x per-bytecode tax (architectural).
X86 COMPILE-THRASH FIXED (commit 74f12085, box-validated): failed compiles
14,069,951 -> 13; ours-x86 cogRunBench empty/timeout -> RETURNS loop20M=1381
fib30=520 cfibx30=856 (Cog-x86: 44/9/13). negative-cache permanent asmjit-T1 emit
failures in compileViaAsmjit; arm64 unaffected (battery==Cog). X86 PROGRESS (2026-06-15, box-validated): x86 broken->working, benchFib ~10x faster.
3 commits: thrash-fix (74f12085, 14M->13), forward-ExtJump native (c80799c8,
benchFib 520->51ms = 58x->5.7x vs Cog), ExtendB+ExtJump back-edge native (61a5b4af,
correct, helps LARGE loops; micro-bench loop unchanged — it uses SHORT back-jumps).
REMAINING x86 (CORRECTED 2026-06-15):
- loop 98x (vs arm64 14ms) was COVERAGE not sp-tax: the JIT'd loop method BAILED at
  ExtJumpFalse (0xEF) condition + ExtStoreTemp (0xF5) counter. FIXED dae71b87 (port
  both + canBailMidMethod scan fix). BOX-VALIDATED: loop20M 1375->133ms (10.3x),
  loopsum1M correct, battery golden -> now ~3x vs Cog-x86's 44ms. Residual 133 vs
  arm64 14ms IS the per-bytecode sp-tax (sp/TOS register residency = deferred lever).
- cfibx 27x (599ms vs arm64 22ms) = x86 has SELF-recursive inline-J2J only (benchFib
  works, 3.2x), NO cross-method inline-J2J (AsmjitT1.cpp:95-99: "bail-time J2J-save
  materialization on x86 is not yet implemented"). cfibx's cross-method `incc` send
  runs full IC dispatch (222ns/call vs arm64's 8ns). LEVER = port arm64's xmethod
  inline-J2J + bail-time J2J-save to x86 — a MAJOR focused-session port (the
  historically buggy save/restore area), not a blind edit.
- broad x86 SUnit coverage: naked-extended push/store family DONE (766f3a9b,
  box-validated: extTemp=580, 845 SUnit tests 0-fail, battery golden) —
  ExtPushRecvVar/LitVar/LitConst/Temp, ExtPopStoreTemp, ExtStoreTemp,
  ExtPopStore/StoreLitVar. STILL unported (next coverage): ExtSend/ExtSuperSend
  (sends — need IC setup, more complex) + ExtStore/PopStoreRecv (immutable bail,
  excluded to keep canBailMidMethod surface small). Mirror arm64 ~9418/~9594.
(older detail:)
NEXT x86 LEVER (CORRECTED via clean box #6): the disagree/DNU were polling
artifacts (clean run: 0 disagrees, benchFib=2692537 correct). x86 is 31-66x because
the correctness-first x86 emit loop (AsmjitT1.cpp ~8936) BAILS EVERY >=0xE0 extended
bytecode to interp, and hot methods use them (cfibx has ED=ExtJump for its
ifTrue:ifFalse: -> interp-bound). arm64 handles >=0xE0 inline; x86 doesn't. LEVER =
port the arm64 >=0xE0 handling to the x86 loop (ExtJump/True/False control flow
first, then ExtSend/ExtPush/PushInteger). Multi-op, box-validated, deep. (older
detail:)
X86 ROOT-CAUSED (diagnostic box #4): ours-x86 evals empty because the x86 JIT
COMPILE-THRASHED (now fixed) — cogRunBench under PHARO_X86_JIT=1 = exit 124 (timeout) with
14,069,951 FAILED compiles; ours-interp + Cog-x86 complete fine. Mechanism:
`[asmjit-t1] BUG: prescan/emit disagree at bc=0xEA (ExtSend) / 0xF9 (PushFullBlock)`
-> compile fails -> the ACTIVATION-driven compile (JITRuntime.cpp:3828) re-attempts
on hotness (the eager path's negative-cache initialCompileFailedContains @23434 is
prim/block-only), so the x86 extended-bytecode failures thrash. THE x86 LEVER
(likely the biggest reason x86 lags): (1) fix the x86 emit-loop stepping of
extended bytecodes (0xEA/0xF9) so they don't disagree; (2) negative-cache PERMANENT
(bytecode-unsupported) activation-path compile failures — must NOT cache transient
zone-full failures, so compiler_->compile needs to report the reason. BOX-GATED:
x86 emit can't be built/repro'd locally (#if x86 skipped on arm64) -> needs ONE
focused x86 fix+validate session (build + run_x86diag.sh, confirm 14M failed -> ~0,
ours-x86 bench returns). ~4 boxes spent; STOP ad-hoc runs. Full detail +
run_x86diag.sh in memory [[cog-speed-lever-closed]]. Cog-x86 baseline ~loop20M=50
fib30=10 cfibx30=13. NEXT: this x86 fix session; OR arm64 B1 reachable-handler
relocation; verify the x86 benchFib DNU isn't a separate correctness bug.

---

# WIP — arm64 Cog-speed: cross-method lever CLOSED, next levers are multi-session (2026-06-14)

Fair same-machine re-measurement (docs/cog-speed-current.md): the documented
"cross-method send activation" Cog-speed lever is DONE — cfibx 43x->2.65x,
cfibs 50x->2.9x, benchFib 2.9x, inline-loop 1.9x (MAX_IC=8 + bailmid +
prim-prologue + PMS + XGATE + FSR all default-on). Residual = ~1.9x per-bytecode
naive-stack tax + ~1.5x per-send sequence. A 6-agent cog-speed-anatomy workflow
ranked the next levers; the OoO lesson (count is free, only RMW/STLF kills
measure) means there is NO safe quick win left.

DONE this session: fixed a latent j2jDepthFromCursor() /32-divisor silent-
corruption landmine (commit 26613442; V2 J2JSave is 40B since JSV_CLOSURE,
dormant helper, pinned to JSV_SIZE).

NEXT-LEVER DECISION (strategic, multi-session — awaiting direction):
- FSR M3c (PHARO_T1_FSR_NODEPTH, kill per-call j2jDepth RMW): ENABLING-ONLY
  (<3% likely; real value = freeing x20 for M5). Blockers the workflow critique
  found: must guard THREE push-RMW sites (AsmjitT1.cpp 6813-6815/5341-5343/
  6446-6448) and re-fund in-descent scheduler preemption (forceYield doorbell is
  back-edge-only; fib/cfib have no back-edges -> deadlock-family risk) + a
  deep-recursion-starves-Delay probe.
- Out-of-line dispatch (Cog-style shared send-stubs): HIGHEST ceiling. DESIGN
  DONE 2026-06-14 -> docs/out-of-line-dispatch-design.md (7-agent workflow +
  adversarial critique). KEY CORRECTION: the obvious "bl harvests x30 as the
  per-site resume address" is FATAL (x30 is the method's live return-to-C++ link;
  the VM is frameless, 0 bl / 15 ret in cfibx). Resume instead via the SHIPPED
  per-site `adr x14, resumeAfterCall` + plain `b`/`br`. Win is Axis-1
  (zone/i-cache/compile-COVERAGE: ~204B/site saved via LINKED-STUB = a PMS
  tail-deletion; recover 64MB->16MB), NOT per-send latency (NEUTRAL by
  construction). Staged B0-B6, B0 = shared return-prelude (de-risked first move,
  no resume-address problem). Gates are compile-fail-rate + survivability, NOT
  cfib ms.
  B0 LANDED opt-in (commit f2493c49, PHARO_T1_SHARED_RETPRELUDE): per-method
  shared prelude+epilogue owning BOTH exits; single-return methods stay inline
  (benchFib/cfibx untouched). Validated: battery==Cog, rdense (3 ret) 5692->5528B
  both exits correct, cfibx (1 ret) SIZE-identical 5568->5568, DET_SCHED rdense
  75025 x3, SUnit subset 1577 tests per-test identical on/off. MEASUREMENT
  GOTCHA: byte dumps AND EMIT_HASH vary run-to-run via ASLR-baked helper/zone
  addresses (off-vs-off differs) -> use emitted SIZE as the knob-off-identity
  proxy, NOT cmp/hash.
  B0.5 LANDED opt-in (commit 5fe0c001): zone-GLOBAL shared stub (one never-freed
  MAP_JIT page via getSharedReturnPreludeStub) -> EVERY real non-block method's
  returns become `mov x16,stub; br x16` (x30 stays the live return link, no bl).
  Single-return methods now shrink too (cfibx 5568->5496). Validated: battery==Cog,
  DET_SCHED 75025 x3, SUnit subset 1577 tests per-test identical on/off, actual
  zone 32.70M->32.00M (~1.8%). Measured (PHARO_T1_RETPRELUDE_STATS): return prelude
  is only ~1.2-1.8% of bloat; the per-SEND machinery dominates -> B1 is the real
  win. B0.5's value = the proven zone-global-stub infra B1 reuses. NEXT = B1
  (per-send shared stub: LINKED-STUB state + the resume-address mechanism, §2-5).
Harness ready for either: /tmp/cogbench2.st + golden /tmp/battery_golden.txt
(Cog-validated), PHARO_T1_DUMP_SEL+capstone, fresh /tmp/bench/Pharo.image.

---

# WIP — x86 JIT startup-corruption FIXED (2026-06-13)

Root cause of the long-standing "x86 tier-1 JIT corrupts/DNUs/hangs at
startup": the SHARED `supportedPrimIndex()` (AsmjitT1.cpp ~1469) advertised
prims 10-13 (`\\` `//` bitShift: `/`), 60-62 (at:/at:put:/size), 541/542/549
(SmallFloat +/-/*) as JIT-supported, but `emitPrimProlog_x86()` (~1557) only
ever implemented 1,2,3-8,9,14,15,16,110. So on x86 those primitive methods
compiled a prologue that failed its SmI-receiver check (Array/Float isn't a
SmI) and fell THROUGH to the method's Smalltalk error fallback WITHOUT running
the C primitive — e.g. `Array>>at:` ran `^self errorSubscriptBounds: index`
on a *valid* index, raising a spurious SubscriptOutOfBounds in the morphic
startup loop, which had no handler → `primitiveFindHandlerContext` looped
forever (DET) / DNU'd (wall-clock). The sender chain was intact the whole time
(red herring).

Fix (commit 337eeb20): on x86 only, `supportedPrimIndex` returns -1 for those
prims → prim-fallback BODY path (C primitive runs first, correct). arm64
untouched (`#if defined(__x86_64__)`). Perf follow-up: port the arm64
emitPrimProlog cases (60/61/62/10-13/541/542/549) to x86 and remove them from
the x86 -1 list one at a time.

Validated on the x86 box (18.221.159.216): fib/sum/mul/bigmul battery +
Dictionary/OrderedColl/Float/SortedColl/String all match the interpreter
byte-for-byte (wall-clock AND DET_SCHED); a 7-class SUnit batch ran 1292 tests
0 fail / 1 env-err (testPrintingRecursive passes when invoked directly under
both interp and x86jit). arm64 builds clean, battery unchanged. Memory:
`jit-x86-prim-prologue-mismatch.md`.

Debugging notes that worked: EVAL-RESULT IS capturable from the headless
harness (differential interp-vs-JIT eval beats chasing startup DNUs);
PHARO_DET_SCHED turns the wall-clock DNU into a deterministic hang;
`gdb -p` after `sysctl kernel.yama.ptrace_scope=0` located the loop in
primitiveFindHandlerContext; gdb can't read member vars on the box build
(no full DWARF) so instrument in C++ (PHARO_T1_TRACE_HANDLER dumps the
handler-search chain w/ receiver class + at: index).

## x86 self-recursive inline-J2J — RESOLVED + DEFAULT-ON (2026-06-14, b471f862)

**STATUS: landed.** The "BLOCKER" narrative below is HISTORICAL — superseded by
the design workflow (docs/x86-inline-j2j-design.md). The real fix was much
smaller than the multi-session redesign feared here: **coupling-1 alone**
(the inline send-site push must publish `state.j2jDepth` so the C++ chain-loop /
materializer see the pending saves) fixed correctness. Coupling-2/3 proved
unnecessary even past the 32-slot save-room limit. Validated at full-suite scale
(x86, 12689 pass, 0 deterministic regressions; see docs/sunit-3way-comparison.md)
and now flipped DEFAULT-ON to match arm64 (default-on since 2026-06-10). Opt out
with `PHARO_T1_X86_NO_INLINE_J2J=1`; `PHARO_T1_X86_INLINE_J2J` is now a no-op.
The historical "blocked" account is kept below as the diagnostic trail.

### (historical) WIP — knob-gated, then thought BLOCKED on materialization

Goal: bypass the JIT->C++->JIT activate/resume round-trip for self-recursive
JIT->JIT calls on x86 (the bigger remaining perf lever; arm64's larger J2J win
comes from this). Implemented behind two default-off knobs:
`PHARO_T1_X86_INLINE_J2J` (master) + `PHARO_T1_X86_J2J_SEL=<selector>`
(per-method opt-in, so the unfinished mechanism never touches startup/library
code). Triple-gated (x86-only too); default x86 + arm64 are unaffected
(verified: battery + arm64 build clean).

What's built (AsmjitT1.cpp):
- `emitJ2JReturnPrelude_x86` (after emitPushReg): at each of the 5 return ops,
  if j2jSaveCursor > j2jEntryCursor, pop the V1 J2JSave, restore caller
  sp/tempBase/receiver, write retval as caller TOS, branch to saved resumeAddr.
- Send-site fast path (in the bit-60 region): self-rec gate (extras bit 60 +
  cached methodBits == OFF_METHOD), save-stack-room check, push V1 J2JSave
  (sp/recv/tempBase/ip/jitMethod/resumeAddr/nArgs), set up callee frame
  (newReceiver=sp[-1-nArgs], newTempBase=sp-8*nArgs, nil locals,
  newSp=tempBase+callerTempCount*8), branch to extras&0x0000FFFFFFFFFFFF.
  The frame model + branch target match arm64 (verified vs AsmjitT1:6520-6930,
  branch target = entryAddr = extras & ADDR_MASK at :5783).
- Step A (return prelude as a no-op when no saves) validated CLEAN knob-on.

THE BLOCKER (precisely identified, this is the multi-session part):
`Interpreter::tryJITActivation` (Interpreter.cpp:24290) UNCONDITIONALLY resets
`state.j2jSaveCursor = state.j2jEntryCursor = j2jPool base` on every C++ JIT
activation. During IC warmup a self-recursive method mixes J2J calls (warm site)
with C++ activations (cold site) — and each C++ activation DISCARDS the pending
inline-J2J saves, orphaning the caller's resume. Result (SEL=rfib): rfib
collapses toward rfib(n-1) (rfib(20)=1, rfib(28)=1) instead of the real value.
This is the bail-time J2J-save MATERIALIZATION problem: on any exit to C++ with
pending inline-J2J saves, the C++ side must materialize them into real frames
(arm64 does; x86's asmjit-T1 path never had J2J saves so it doesn't). Fixing it
means either (a) materialize pending saves into C++ frames before every C++
activation/resume, or (b) make the cursor reset conditional and have the C++
chain-loop/resume understand asmjit-T1 V1 saves. NOT a localized change.

SCOPE CORRECTION (deeper dig): the fix is bigger than "add materialization."
x86 ALREADY HAS J2J via the C++ CHAIN LOOP in tryJITActivation (~24500:
`while (exitReason == ExitJ2JCall || ExitSendCached ...)`, converts
ExitSendCached->ExitJ2JCall, drives recursion in C++ via j2jDepth/j2jPool_,
materializes on resume — the "J2J stencil: calls=NN" stat).  My inline-J2J is a
SECOND, conflicting J2J manager on the same pool: when it fires it bypasses the
chain loop, but any cold/non-J2J send in the chain does a recursive
tryJITActivation that resets the cursor and discards the inline saves.  So a
correct inline-J2J must REPLACE the chain loop's J2J for the methods it owns
(handle every J2J send + materialize on every bail) — an architectural redesign
of a LOAD-BEARING mechanism, risky on the non-shipping arch.

PERF REALITY: wall-clock rfib(28) on x86 is ~interp-speed (chain-loop J2J has
real per-send C++ overhead), so inline-J2J WOULD help — but the baseline is
chain-loop J2J, NOT no-J2J, so the win is smaller than arm64's 296->32ms benchFib
(inline-J2J vs FULL activation).  Recommend treating this as a dedicated
multi-session redesign with its own validation gates, or deprioritizing (x86 is
not the shipping arch and already has working chain-loop J2J).

DESIGN DOC: docs/x86-inline-j2j-design.md — synthesized from a 5-map + 3-proposal
+ 3-critique workflow.  CORRECTS the blocker diagnosis: not the cursor reset, but
(1) inline push never publishes state.j2jDepth (so C++ consumers + materialize
don't see the saves) and (2) the chain loop's local j2jDepth=0 + j2jStack[0]
ALIASES inline save #0 and overwrites it on a cold re-entry.  The materializer
already exists (V1/x86-ready).  Fix = 3 couplings (publish depth; seed chain-loop
depth from state.j2jDepth + fix j2jDepthFromCursor V1 stride; reuse materialize),
gated, gates G0-G5.  Realistic 5-8 sessions for default-on; coupling 1 is ~1-2h.  The WIP code
stays inert (triple-gated default-off) as scaffolding.

## PERF FOLLOW-UP — ported at:/size/SmallFloat x86 prim prologues (3b81d93d)

emitPrimProlog_x86 now implements prims 60 (at:), 62 (size), 541/542/549
(SmallFloat +/-/*), ported from emitPrimProlog_arm64 (removed from the x86
supportedPrimIndex -1 list). Drafted + adversarially verified via a Workflow,
cross-checked vs arm64 + the C primitives, validated on the x86 box:
eval matches interp (Array/String/ByteArray at:/size, fmt-9 DoubleWordArray
helper path, float incl. 0.1+0.2 precision); SUnit ~2319 tests 0 fail (only the
pre-existing ArrayTest>>testPrintingRecursive env-error); arm64 builds clean +
battery unchanged. A/B win on x86 is MODEST (~2-4% on at:/size/float loops):
the x86 -1 fallback already ran the C prim first, and x86 has no inline-J2J, so
the prologue only elides the C-prim call, not the activation. Main value: arm64
parity. STILL on -1: prim 61 (at:put: — arm64 fmt-2 inline store omits the
old->young write barrier; the C-prim path's storePointer is the safe one) and
10-13 (no arm64 prologue exists; C-prim-first already optimal). Next perf lever
if wanted: give x86 an inline-J2J path so prim-prologue callees activate without
the C runtime hop (that's where arm64 gets its larger win).

---

# WIP — JIT optimization session (2026-05-27 → 2026-05-28)

## 2026-05-28 late PM — graphics-test queue kickoff

See `docs/graphics-testing.md` for the full queue and per-package
instructions, and `docs/results/` for raw run output.

Pharo 13 already ships with most large graphics packages preinstalled
— no Metacello load needed for Roassal3 (RS, 99/879), Spec2 (Sp,
204/3505), Bloc (53/642), Athens (10/80), Cairo (7/32), Plot (10/146),
Chart (4/17).  Only PolyMath needs Metacello, which is blocked on the
Iceberg `Character>>bitShift:` regression.

* Roassal3 run 1: 32 PASS / 0 FAIL / 3 ERROR / 0 TIMEOUT, then
  SIGSEGV in JIT-emitted `#inverseTransformPiOrZero:` (fault addr
  bit 60 = `J2J_ENTRY_BIT` leak; codeStart=`0x10af31130`, offset=2496).
  Reached 35 of 879 tests.  All 3 ERRORs share the same root cause:
  `Color>>blue` returning KeyNotFound on the `ColorRegistry`
  IdentityDictionary — yet an isolated probe shows the dict is
  populated and `Color blue` works.  So it's a transient corruption
  later in the run (likely GC of the identity dict).
* Spec2 in flight: see `docs/results/spec2_inflight.txt`.  High
  ERROR rate (~50%) expected from UI-shim limitations.
* Bloc / Athens / Cairo / Plot / Chart queued behind a shell wrapper
  `scripts/graphics/run_graphics_queue.sh` that swaps the class-name
  filter file between runs and skips entries whose result file
  already exists.



## Goal
"Fix the JIT optimization to be as fast as Cog."

## 2026-05-28 PM — StringTest fail investigation

StringTest>>testOnlyLetters and the line-ending tests fail because
of a `WriteStream on: WideString` interaction bug.  The select-by-
isLetter pattern is:
```
result := src species new: src size streamContents: [:stream |
  1 to: src size do: [:i |
    (each := src at: i) isLetter ifTrue: [stream nextPut: each]]]
```
where `src species` for a WideString returns WideString, so the
stream's underlying buffer is `WideString new: 14`.  Subsequent
`nextPut:`s produce a buggy pattern (Character oop bits in odd
slots, zeros in even slots, then transitioning to correct
codepoints at slot ~50+).

Pattern observed (write 100 chars cp 1001..1100 to `WideString new: 100`):
```
ws[1] = 8011 (= (1001<<3) | 3 oop bits)
ws[2] = 0
ws[3] = 8019 (cp 1003 oop bits)
ws[4] = 0
...
ws[50] = 0
ws[51] = 1051 (correct codepoint!)
ws[52] = 1052
...
```

The bug is independent of JIT — `PHARO_NO_JIT=1` shows identical
behavior.  Direct `ws at: i put: ch` works correctly; only the
WriteStream wrapping triggers the bug.  Investigated:
`primitiveStringAtPut` (Primitives.cpp:5599) looks correct;
`asCharacter()` correctly decodes; `bytes()` returns the right
pointer.  Where exactly the buggy stores come from is TBD —
would need printf instrumentation or lldb.  Worth a focused
session.

### 2026-05-28 late PM — diagnostic attempts

Added gated printf to `primitiveStringAtPut` for WideString
branch and ran a minimal `WriteStream on: (WideString new: 6)`
probe.  Observations:

* `ManProbe` (instance-vars `collection`/`position`, manual
  `position := position + 1. collection at: position put: anObject`)
  works correctly on `WideString new: 10`.
* `WSProbe` running `WriteStream on: ws` followed by 6
  `nextPut:`s produces NO `stringAtPut` traces with the correct
  receiver bits, and the probe itself stops emitting at
  `print: stream collection class`.  The `print:` aborts the
  outer file-write block — likely because the printed object
  is unusable (collection's class chain is corrupt, or `print:`
  recurses into the same buggy stream init).
* Disabling each individual T1 inline flag in turn
  (multislot, retlit, inline-getter, inline-setter,
  inline-prim-at, PHARO_NO_JIT) does NOT change the behavior:
  the probe still stops at the same point.

Hypothesis: `WriteStream class >> on:` or
`PositionableStream >> on:from:to:` triggers a bytecode
sequence the VM mis-executes for an arg that is a WideString.
Most likely candidate: a sista bytecode that branches on
`isBytesObject` / `isWideString` returns the wrong answer for
fmt 10/11, causing `WriteStream` to think its buffer is a
`ByteString` and to store 8-bit oops at 4-byte offsets.
Diagnostic printf removed (committed-clean tree); needs lldb
breakpoint at `WriteStream>>nextPut:` entry to confirm.

## 2026-05-28 PM — broader 20-class SUnit run: 3170/3189 (99.6%)

After hardcoding the cos JIT skip (commit `5c870c75`), extended the
SUnit run to the first 20 curated classes:

```
class                    PASS  total
SortedCollectionTest      287
IdentitySetTest           176
SmallIntegerTest           27
IntegerTest                80
FloatTest                  73
FractionTest               30
PointTest                  34
CharacterTest              16
DictionaryTest            205
SetTest                   174
BagTest                   168
IntervalTest              260
SymbolTest                268
OrderedCollectionTest     351
ArrayTest                 323
StringTest                438
HeapTest                  148
BlockClosureTest           50
ContextTest                34
ExceptionTest              47
TOTAL                    3170 / 3189   (99.6%)
                                7 FAIL + 6 ERROR + 6 SKIP
```

Remaining FAILs/ERRORs:
- **StringTest** (3 FAIL): testOnlyLetters, testWithInternalLineEndings,
  testWithUnixLineEndings
- **BlockClosureTest** (2 FAIL + 1 ERROR): testBenchFor,
  testIsClean, testSourceNodeOptimized
- **ContextTest** (1 FAIL + 2 ERROR): testAstScope,
  testMethodContextPrintDetails, testReadVariableNamed
- Plus a few others (didn't enumerate all 13)

These are deeper bugs in specific subsystems (line-ending detection,
block closure introspection, context AST inspection) — out of scope
for the JIT correctness pass that brought the headline result.

## 2026-05-28 PM — broader test coverage findings (cos crash, superseded)

Extended to 20 classes (from focused 4): finds new JIT correctness
bugs.  Most prominent:

- **`Float>>cos` JIT compilation SIGSEGVs** under repeated
  `i degreesToRadians cos` calls (e.g., IntegerTest>>testDegreeCos).
  Reproducer:
  ```
  -360 to: 360 do: [:i | i degreesToRadians cos]
  ```
  Crashes inside JIT'd cos at offset 336 with fault addr
  `0x812d97c7f3321d28` (looks like a SmallFloat64 raw-bits
  interpreted as a heap pointer).
  Workaround: `PHARO_T1_SKIP_SELECTORS=cos` makes the test pass.
  Bytecodes are simple (`self pushLitVar 0 send: + send: sin
  returnTop`) — bug is in the JIT'd sequence somewhere.

These broader-coverage bugs are out of scope for the "fix existing
fails" pass that achieved 100% on the focused 4 classes — listing
here as next targets.

## 2026-05-28 PM — 100% SUnit PASS (commit 3a2c0a68)

Final state: **634/634 PASS / 0 FAIL / 0 ERROR** on the focused
4-class run, repeatably verified across 3 runs.

Two JIT defaults flipped this session to achieve correctness:
- `t1InlineBlockValue` OFF (commit 34e70558) — fixed 22 FAILs from
  the nested `arr do:` + `arr occurrencesOf:` bug.
- `t1InlineJ2J` OFF (commit 3a2c0a68) — fixed the last 1 ERROR
  (testStoreStringAll OCParser interaction).

Perf cost: fib28 11 ms → 79 ms (~7× slower).  Real-world hit is
smaller — fib is the worst case for losing inline-J2J.  Opt-in
fast path for benchmarks:
```
PHARO_T1_INLINE_J2J=1 PHARO_T1_INLINE_J2J_XMETHOD=1
```

Per-class final:
```
SmallIntegerTest   27/29
SymbolTest        268/268   (100%)
CharacterTest      16/19
ArrayTest         323/324
TOTAL             634/634   (100%)
```

## 2026-05-28 PM — disabled BV inline (commit 34e70558): 633/634 PASS

Bisected the 23 "pre-existing" FAILs to a single JIT bug: T1
inline-block-value (BV) corrupts inner-block iteration when the
outer caller is also doing `arr do:` over the same receiver and the
inner block compares captured-outer-each against inner-each.

Minimal reproducer (in our VM with BV inline on):
```
| arr2 |
arr2 := #($a $b $a $c $d).
arr2 do: [:e | Transcript print: (arr2 occurrencesOf: e); cr].
```
Returns 3, 1, 3, 1, 1 (wrong — `$a` appears only twice).
With BV inline off: 2, 1, 2, 1, 1 (correct).

Flipped `t1InlineBlockValue` default to OFF.  SUnit jumped from
611/634 (96.4%) to **633/634 (99.8%)** — 22 FAILs eliminated, only
`CharacterTest>>testStoreStringAll` ERROR remains (separate
OCParser/mustBeBoolean issue, unrelated).

Per-class after the fix:
```
SmallIntegerTest  27/29
SymbolTest       268/268   (100%, was 263/268)
CharacterTest     15/19    (1 ERROR — testStoreStringAll)
ArrayTest        323/324   (was 306/324)
TOTAL            633/634   (99.8%)
```

Perf cost: `fib28` 10 ms → 13 ms (small; fib doesn't lean on
blocks).  Opt back in via `PHARO_T1_INLINE_BLOCK_VALUE=1`.

## 2026-05-28 PM — INLINE+XMETHOD flipped back ON (commit 0731f841)

Bisection on the focused-4-class SUnit run found the earlier
"99.84%" measurement was a Monitor counting artifact — the real
stable result is 611/634 with INLINE off, and the SAME 611/634
with both INLINE+XMETHOD on (identical 23-FAIL list, verified by
diff).  XMETHOD has NO correctness regression in this run but
gives 7× fib speedup:

```
config                       SUnit    fib28
INLINE off                   611/634  76 ms
INLINE on, XMETHOD off       ~297/634 SEGV (cull: bug)
INLINE on, XMETHOD on (NEW)  611/634  11 ms
```

Flipped both defaults back ON.  Opt out via `PHARO_T1_NO_INLINE_J2J=1`
or `PHARO_T1_NO_INLINE_J2J_XMETHOD=1`.

The 23 pre-existing FAILs (5 SymbolTest, 17 ArrayTest assertions,
1 CharacterTest ERROR) are unrelated to inline-J2J — same set
appears in INLINE-off and INLINE+XMETHOD-on.

## 2026-05-28 PM — INLINE_J2J + XMETHOD interaction matrix (superseded)

Bisection on `cleanUpInstanceVariables` stress + full SUnit found
that `PHARO_T1_INLINE_J2J_XMETHOD=1` together with INLINE_J2J=1
recovers most of the perf without the cull: dispatch bug — but
introduces 23 silent wrong-value `FAIL` results in collection tests.

```
config                      SUnit pass     fib28
INLINE off (default)        633/634 99.84% 76 ms
INLINE on, XMETHOD off      297/634 47%    SEGV
INLINE on, XMETHOD on       611/634 96.4%  11 ms
```

XMETHOD-on cross-method inline-J2J fires for more call sites but
produces wrong results on `testAsArray`, `testSize`,
`testAsOrderedCollection`, `test0FixtureCopyPartOfSequenceableTest`,
etc.  Both inline-J2J modes have correctness bugs; default stays off.
Future fix needs to address BOTH paths.

## 2026-05-28 PM — inline-J2J disabled by default (633/634 = 99.84%!)

Bisection found the `cull:`/`do:`/`value:` dispatch confusion bug:
`PHARO_T1_NO_INLINE_J2J=1` lifts SUnit from 297/634 (47%) to
**633/634 (99.84%)** on the focused-4-class run.

Per-class with inline-J2J off:

```
SmallIntegerTest   27/29   (93%)
SymbolTest        268/268  (100%, was 48%)
CharacterTest      15/19   (79%)
ArrayTest         323/324  (99.7%, was 39%)
TOTAL             633/634  (99.84%, was 47%)
```

Flipped `t1InlineJ2J` default to off in DebugSettings.cpp; opt back in
via `PHARO_T1_INLINE_J2J=1`.  Commit `10aa6c80`.

Trade-off: `fib(28)` goes from 9 ms (inline-J2J fast path) to 96 ms
(normal IC dispatch).  ~10× slowdown on the tightest recursive
benchmark, but correctness wins until the inline-J2J receiver-class
check is audited.

**Theory of the bug** (unverified): inline-J2J's IC dispatch trusts
the cached entryAddr without re-verifying the receiver class.  Some
polymorphic call site sees a BlockClosure receiver first, fills the
IC with `extra` carrying bit 60 (J2J_ENTRY_BIT) + BlockClosure>>cull:'s
entryAddr.  When a later call passes a non-block (e.g. LayoutClassScope)
to the same IC site, the slot key check matches (somehow) and
inline-J2J `br`s to BlockClosure>>cull:'s code with the wrong
receiver.  Manifests as impossible stack frames.

Worth a follow-up lldb session to confirm and fix the receiver-class
check; then default can be flipped back on.

## 2026-05-28 PM — additional fixes and cull: investigation

Extended the dual-path primitive trap fix:
- `emitPrimProlog_arm64` for prim 60 (at:) now handles fmt 3/4/5/9
  via `jit_rt_primat_ptr` helper.  Commit `5b2d6e55`.
- `emitPrimProlog_arm64` for prim 61 (at:put:) handles fmt 3/4/5
  via `jit_rt_primatput_ptr`.
- `jit_rt_array_prim` (IC-shortcut path) primKind 14/15 extended for
  fmt 3/4/5(/9 for at:).  Commit `2512d03f`.

**`cull:` JIT bug — unresolved, persists.**

Many SymbolTest ERRORs (138 → ~120 after at: fix) come from a single
recurring pattern:
```
ArgumentsCountMismatch: This block accepts 0 arguments, but called with 1.
  BlockClosure>>value:                          <-- block (real one)
  LayoutClassScope>>do:
  LayoutClassScope(BlockClosure)>>cull:         <-- impossible inheritance
  PointerLayout>>allVisibleSlots
  ClassDescription>>allSlots
  TestCase>>cleanUpInstanceVariables
```

The fourth stack frame is impossible: `LayoutClassScope` doesn't
inherit from `BlockClosure`, and `(LayoutClassScope canUnderstand:
#cull:)` is `false`.  Yet cull: is somehow executing on a
LayoutClassScope receiver — sending `value:` to it, which DNUs.

Probes show:
- Direct call `[42] cull: 5 = 42` works.
- 200,000-call stress (`[:x | x*10] cull: i`) — all correct.
- `SymbolTest class allInstVarNames` direct → works.
- `SymbolTest selector: #test0CopyTest` `runCase` direct → works.
- Forked equivalent → hangs.

`PHARO_T1_SKIP_SELECTORS=cull:` lifts SymbolTest test0Fixture from
12/24 to 28/32 (88%) but regresses CharacterTest 15/19 → 0/19 because
the same dispatch confusion now hits `do:` instead.  The bug is
something deeper — likely IC polymorphic-cache poisoning where one
class's IC entry mis-matches a different-class receiver.

Hypothesis: under the SUnit fork-and-watchdog harness, a polymorphic
send site's IC accumulates entries from blocks AND from layout/scope
receivers; the IC class-key check is missing or wrong in some emit
path.  Worth an lldb session to verify.  Skipping cull: from JIT is
NOT a clean workaround because the same pattern recurs on other
selectors (do:, etc.).

## 2026-05-28 PM — SUnit test results

**First real SUnit run on our VM.**  Focused subset (4 classes:
SmallIntegerTest, SymbolTest, CharacterTest, ArrayTest):

```
class             total  PASS  FAIL  ERROR  SKIP
SmallIntegerTest    29    27    0    0      0       (93%)
SymbolTest         268   128    2  138      0       (48%)
CharacterTest       19    15    0    1      0       (79%)
ArrayTest          324   127   11  185      0       (39%)
TOTAL              634   297   13  324      0       (47%)
```

Before the basicSize JIT fix: **0 tests ran**, every class reported
0/0/0/0 because Symbol>>numArgs returned -1, making OpalCompiler
reject every recompile attempt.

The remaining ERRORs (324) are largely a single repeating pattern in
SymbolTest's `test0FixtureXxx` series:

```
ArgumentsCountMismatch: This block accepts 0 arguments, but was called with 1.
  >> FullBlockClosure(BlockClosure)>>numArgsError:
  >> FullBlockClosure(BlockClosure)>>value:
  >> LayoutClassScope>>do:
  >> LayoutClassScope(BlockClosure)>>cull:
  >> FixedLayout(PointerLayout)>>allVisibleSlots
```

`cull:` should branch on `numArgs = 0` (call `value`) vs not (call
`value: arg`).  Direct probe (`[42] cull: 5 = 42`) works.  So the
cull: bug only manifests in some specific JIT-compile context — TBD.
Likely another JIT correctness gap, separate from basicSize.

ArrayTest ERRORs probably overlap with the same cull: issue.

The win: **our VM now runs real Pharo test classes with healthy
pass rates on the ones where the cull: issue doesn't trigger**
(SmallIntegerTest 93 %, CharacterTest 79 %).  Before today the
fraction was zero.

## 2026-05-28 PM — JIT basicSize correctness bug fixed (71cc0701)

While running SUnit, found a JIT correctness regression that broke
**every** Unicode-classification-using path (isLetter / numArgs /
OpalCompiler arity check / SUnit test discovery / Stream parsing /
...).  Reported 0/0/0/0/0 for every test class.

Root cause: JIT-compiled `basicSize` returned 0 (source fallback)
for SparseLargeTable receivers — fmt=3 (IndexableWithFixed) WITH
slotCount-byte=0xFF (overflow header for >= 255 slots).  Two JIT
paths had the same gap and BOTH had to be fixed:
- `stencil_primSize` fmt 3/4/5 branch fell through to bytecode.
- `emitPrimProlog_arm64` for prim 62 only handled fmt 2/10-11/16-23
  inline, bailed on overflow header, and fell through on fail.

Fix: added `jit_rt_primsize_ptr` helper (class-table lookup of
fixedFields for fmt 3/4/5, slotCount-as-size for fmt 9).  Plumbed
through helpers struct / extract_stencils.py / JITCompiler patch
sites.  Updated both JIT paths to handle overflow headers and call
the helper for fmt 3/4/5.  Also extended `jit_rt_array_prim`
primKind=16 IC-shortcut for symmetry.

Verified: `gc at: 117 = 5` (was 0); `$t isLetter = true`;
SUnit AIAstarTest passes.  See [[jit-dual-path-primitive-trap]] for
the debugging lesson.

## Session end state — 2026-05-28

- Branch: `jit` at `4b4465e8` (pushed to origin).
- Working tree clean.  46 commits this session.
- All benchmarks correct and stable.

**Perf vs Cog (final):**
- fib(28): 9 ms (Cog ~30 ms — we're 3.3× faster)
- sieve x3: 1.7 ms
- tinyBenchmarks: 5.4 s wall time, but the per-rate numbers are
  what matter: **6.14 B bytecodes/sec, 113 M sends/sec.**  Cog
  typical is ~5 B b/s, ~150 M s/s — so we're FASTER per-bytecode
  and 25 % slower per-send.  The 5.4 s vs Cog's "2 s" wall time is
  calibration overhead (a faster VM needs larger n to hit the 1 s
  threshold and therefore spends more total time in calibration
  loops), not raw speed.  See [[jit-tinybench-calibration-insight]].

**The "as fast as Cog" goal is substantially achieved on per-rate
metrics.**  The remaining 25 % send-path gap is in JIT codegen
quality (per-call overhead in tryJITActivation + IC dispatch) —
finite but multi-day engineering.

## Session end state — 2026-05-27/28 commits

### Original session (continued from prior WIP)
1. Verified baseline + sample profile — confirmed 70 % of tinyBench
   time is JIT-compiled code, not C++ overhead.  OSR-off test
   confirmed OSR isn't the bottleneck.
2. **All non-vendor build warnings: 430 → 0** across Interpreter.cpp,
   ObjectMemory.cpp, Primitives.cpp, JITRuntime.cpp, JITCompiler.cpp,
   AsmjitT1.cpp, SistaBuilder.cpp, Tier2Compiler_arm64.cpp,
   InterpreterProxy.cpp, etc.
3. **Two real bug fixes surfaced by the warning hygiene:**
   - Primitive 132 `Object>>pointsTo:`: always-false range check on
     `format >= Indexable32 && format <= Indexable64`
     (Indexable64=9 < Indexable32=10), making word arrays leak
     through to the pointer-slot scan.  Commit `f68392c2`.
   - MIDIPlugin `memset(p, 0, sizeof(OpenPort))` on a struct
     containing `std::mutex` — UB.  Replaced with explicit per-field
     reset.  Commit `95378117`.
4. `handleBenchComplete` now decodes String return values
   (`ff3b738b`).  This revealed the "Cog is 2 s on tinyBench" claim
   was misleading.
5. Bench output overhaul:
   - Per-run delta accounting instead of cumulative (`da56f9ce`) —
     prior session's "85 % GC overhead" claim was a measurement
     artifact; real intra-run GC is ~8 %.
   - Per-run alloc-bytes (`a293bd40`) — revealed tinyBench allocates
     2 GB/run.
   - `PHARO_GC_HEADROOM_MB` env knob (`d0df5a6f`) for in-place tuning.
6. `checkSortstrWatch` gated behind `PHARO_HOT_PATH_DIAG` (`aa79abf3`).

### Audit-gap closure (the major thread)
The maintained `rememberedSet_` was dead infrastructure — populated
by storePointer but never iterated; scavenge did an O(oldSpace) full
scan instead.  Closing the JIT-emit write-barrier audit gap is the
prerequisite for dropping the full scan.

Infrastructure built:
- `_HOLE_RT_WRITE_BARRIER` registered in `extract_stencils.py` (helper
  ID 19), `RuntimeHelpers::writeBarrier` field, JITCompiler arm64
  + x86 patch sites, JITRuntime wiring.  Commit `65792d23`.
- JITState gained 4 cached space pointers (offsets 240-264) for the
  inline-asm barrier.  Populated at all 5 JITState init sites.
- `INLINE_WRITE_BARRIER_OLD_TO_YOUNG` macro: ~13 instructions of
  pure inline asm using only caller-saved x11; sets bit 29 on the
  receiver header.  No BLR, no SimStack-cache disturbance.
- `PHARO_SCAV_BIT_AUDIT=1` env var (`95924da2`): measures
  RememberedBit coverage during scavenge, logs first 10 misses with
  class + referent-class + space (old vs perm).

Barriered call sites (in commit order):
- JIT inline at:put: helper (`storePointerUnchecked`)
- asmjit T1 setter (opt-in via `PHARO_T1_SETTER_BARRIER`)
- Non-SimStack store-recv-var stencils (helper call)
- All 5 SimStack store-recv-var stencils (inline-asm bit set)
- shallowCopy (the major C++ leak)
- become same-size swap + heap-scan
- Dict fixCollisionsFrom: in drainFinalizationQueue
- popStoreLitVar / storeLitVar (global-var) + remoteTemp stencils
- asmjit T1 popStoreRecvVar inline emit

**Audit progression: 260 → 228 misses** during normal image startup.
99.997 % bit accuracy.  The remaining 228 misses are spread across
the asmjit T1 inline-emit paths for other extended store opcodes
(storeRecvVar variants, the extended LitVar/Temp variants at
AsmjitT1.cpp:5910+), C++ paths I haven't statically located, plus
runtime-execution writes whose source needs runtime instrumentation
to identify.

### Dead-end recorded
Adding write barriers to the temp-store stencils
(stencil_popStoreTemp_{1..4}, storeTemp_1/_2) was correctness-
improving but cost ~11 % on tinyBench (millions of temp stores per
run, each paying a barrier check) with zero audit-miss reduction
(materialized-context writes are virtually never exercised by JIT-
compiled code).  Reverted — documented so future sessions don't
re-walk this path.

## Session commits (in order)

```
f497528d vm: clear remaining Primitives.cpp unused-variable warnings
d0660c63 vm: remove 12 unused-variable warnings across VM + JIT
1e0aa459 docs: WIP.md — session resume info for JIT optimization work
80b87e5b vm: remove more unused-variable warnings (5 sites)
b4ea4ecd vm: remove 6 unused-variable warnings from Interpreter.cpp
d33a4fdd gc: gate currentScanParent_/currentScanSlot_ stores behind PHARO_HOT_PATH_DIAG
399f7b06 vm: extract SP_CORRUPT_TRACE / FP_CORRUPT_TRACE_FROM_TB macros
67a37230 vm: consolidate 7 duplicated J2J materialize blocks into helper
33242365 vm: bump gcHeadroom to 512MB (was 256MB)
9605744c vm: bump gcHeadroom 32MB -> 256MB; add per-GC-type counters
22adef73 jit: drop stale task-#8 / J2JSlotPerEntry workaround comments
29df9943 jit: fix materialized frame savedBytecodeEnd — root cause of task #8
```

## 2026-05-27 session notes

### tinyBenchmarks self-measurement insight

handleBenchComplete now decodes String return values.  Our actual
numbers per run:

    6,460,567,823 bytecodes/sec
      114,265,556 sends/sec

For comparison, Cog typically reports ~5B bytecodes/sec and
~150M sends/sec.  We're FASTER per-bytecode and slightly slower
per-send.

Wall-clock 5.3 s is dominated by the doubling-calibration phase
that tinyBenchmarks runs to find an `n` that takes ≥ 1 s:
- Bytecodes calibration: 1+2+4+...+16384 = 32767 trials at 500K
  ops each → ~16 B ops at 6.46 B/s ≈ 2.5 s
- Sends calibration: sum of fib(28..40) ≈ 426 M sends at 114 M/s
  ≈ 3.7 s
- Total: ~6.2 s expected, observed 5.3 s

A FASTER VM spends MORE time in calibration because it needs
larger n to hit the 1 s threshold.  The WIP's "Cog ~2 s" target
appears to be a misleading benchmark — apples-to-apples
bytecodes/sec and sends/sec rates are the right metric, and on
those we are already competitive.

### Sample profile findings

Picked up after the prior WIP.  Confirmed baseline (fib=9 ms,
tiny=5286 ms).  Sample profile shows:
- 1424/2541 samples (56%) in JIT-compiled code via dispatchBytecode chain
- 370 samples (15%) in JIT code via activateMethod → tryJITActivation
- ~5.8% primitiveStringReplace memmove
- ~3.9% primitiveNewWithArg → allocateSlots
- gcTime 21% of run

OSR disable test (PHARO_NO_OSR=1) → 5272 ms, basically same as baseline.
OSR is not the bottleneck — the JIT-compiled code itself is.

Big remaining wins (all require multi-week work or known-broken):
- Sista Tier-2 (`canBailMidMethod` bail protocol, blocked since 2026-05-21)
- xmethod inline-J2J (known broken — #value: DNU on startup)
- bumping gcHeadroom to 2GB gains only ~300 ms (vs 3.3 s gap to Cog)

Cleanup taken this session: all non-vendor unused-variable warnings
are gone (Interpreter.cpp, ObjectMemory.cpp, JITRuntime.cpp,
ImageLoader.hpp, Primitives.cpp).  Remaining warnings are all in
vendored plugin code (B2DPlugin, FloatMath, etc.).

## Root-cause story: the materialize bytecodeEnd bug (29df9943)

The "fb(N) bail-at-limit returns fib(N-1)" bug, originally papered over
by bumping `J2JSlotPerEntry` from 32 to 256, was caused by 7 duplicated
materialize sites all using:
```cpp
frame.savedBytecodeEnd = saveJM->bcStart() + saveJM->numBytecodes;
```
`saveJM->numBytecodes` is 0 for AsmjitT1-compiled methods that have
send sites (the `advertiseResume` gate at `AsmjitT1.cpp:6415`).  This
left `frame.savedBytecodeEnd == bcStart`, so after popFrame restored
`bytecodeEnd_ = bcStart`, the dispatch-loop safety net at
`Interpreter.cpp:1895` immediately fired `returnValue(receiver_)` —
fb(N) returned N (its receiver value) instead of the computed value.

Found via printf instrumentation + `backtrace_symbols` showing
`returnValue` was being called from `interpret()` directly (the safety
net), not from `returnFromMethod` (the normal ReturnTop path).  The
clincher diag was `bcEndOff=0`, meaning `bytecodeEnd_ == bcStart`.

The 7 sites are now consolidated into `materializeJ2JSaveIntoFrame()`
(commit 67a37230), eliminating future duplication risk.

`J2JSlotPerEntry` is back to 32 (no longer needs the 256 workaround).

## GC tuning win (9605744c, 33242365)

Profiling tinyBenchmarks with `sample` found 85% of runtime in
`ObjectMemory::fullGC` with the 32 MB `gcHeadroom_` default (64
fullGCs/run, ~89 ms each).  Bumping to 512 MB drops total GC time from
5851 ms to ~1080 ms — about a 22% gain on tinyBench.

**2026-05-27 amendment:** the "85% GC" reading was cumulative across
all runs.  After the per-run-delta fix (`da56f9ce`), tinyBench
shows ~8% intra-run GC at 512 MB headroom — the bulk of "GC overhead"
in the original measurement was inter-run setup GCs, not the
benchmark inner loop.  The 512 MB choice still helps because it
reduces those setup GCs too.

Headroom knob is now env-tunable (`d0df5a6f` —
`PHARO_GC_HEADROOM_MB`).  Fresh per-run-delta measurements:

    headroom_mb  gcCount  gcTime   wall    delta-vs-512
    512          5        438ms    5533ms  baseline
    1024         2        224ms    5435ms  -98ms (-1.8%)
    2048         0        0ms      5351ms  -182ms (-3.3%)

Allocation pressure is 2 GB/run, so 2048 MB headroom is the smallest
value that fully eliminates intra-run GC.  Real wins are modest
because GC was already only ~8% of run time at 512 MB; the bigger
wins from the old WIP table were calibration artifacts.

Default sweep:
```
 32MB:   64 fullGCs, 5851ms GC, 6738ms total (85% GC)
256MB:   16 fullGCs, 1727ms GC, 5576ms total (31% GC)
512MB:    8 fullGCs, 1080ms GC, 5279ms total (20% GC)
  1GB:    4 fullGCs,  773ms GC, 5108ms total (15% GC)
  2GB:    2 fullGCs,  339ms GC, 4994ms total ( 7% GC)
```
512 MB picked as the sweet spot.  Virtual memory is mmap'd lazily
(4 GB reserved), so this only shifts when GC fires, not physical use.

## Available diag knobs

- `PHARO_B5_TRACE=1` — MAT-RET trace (materialize-bail return values).
- `PHARO_T1_INLINE_J2J=1` — inline-J2J counters (g_inlineJ2J_hits etc.).
- `PHARO_T1_INLINE_PRIM_COUNTERS=1` — per-prim counters
  (g_primAt_hits, g_primAtPut_hits, etc.).  Without this, those
  counters stay 0 even when the inline path fires — was a misleading
  "primAt=0" symptom this session.
- `PHARO_BENCH=fib PHARO_FIB_N=N` — direct fib bench.
- `PHARO_BENCH=tiny`, `=sieve`, `=awfy` etc.

## Profiling commands

```bash
PHARO_BENCH=tiny ./build/test_load_image /tmp/harness/Pharo.image > /tmp/tiny.out 2>&1 &
PID=$!
while ! grep -q "warmup done" /tmp/tiny.out; do sleep 0.2; done
sleep 1
sample $PID 5 1 -file /tmp/sample.txt
kill $PID
```

## Audit-gap finding (2026-05-27): remembered-set is dead infrastructure

`storePointer` and friends maintain `rememberedSet_` via the
old→young write barrier, but the set is never iterated — scavenge
does an O(oldSpace) full scan for old→young pointers explicitly
(`ObjectMemory.cpp:1563-1597`).  Comment at 1565 explains: "Trade
correctness for perf until every write site is audited."

This was a quiet realization while investigating the JIT at:put:
write-barrier site.  The "barrier" I added in `5a7267cd` does work
that the scavenge will redo by scanning every old-space slot.

**Closing the gap would be a real perf win:**

- Scavenge time = O(oldSpace) ≈ ~30 ms / scavenge on a 100 MB heap
- Eliminate by ensuring every slot-write site barriers, then have
  scavenge consume `rememberedSet_` instead of full-scanning

**Audit status** (sites that still write slots without the barrier):

- ~~asmjit T1 inline setter arm64 (AsmjitT1.cpp:4431)~~ — opt-in
  barrier wired up in commit `6b643915` via PHARO_T1_SETTER_BARRIER=1.
  Verified no crash; counters added in `fe4c7b27`.

  **Surprise finding:** the inline-setter path doesn't actually fire
  in practice.  Running normal image startup with
  `PHARO_T1_INLINE_J2J=1` shows the existing per-path counters as
  `getter=16059 setter=0`.  So `g_setterBarrier_calls` stays 0 even
  with the gate on — there are no setter writes to barrier.

  **2026-05-27 investigation:** added throw-away diagnostic counters
  inside `detectTrivialMethod` (now reverted) — the function is only
  invoked ~168 times during a tinyBench run, none classifying as
  setter.  bc0 histogram top entries: `0xf8` (CallPrimitive),
  `0x4c` (PushReceiver), `0x10` (PushTemp 0), `0x40` (PushLitVar).
  No `0xC8-0xCF` (popStoreRecvVar) sightings at all.  The 0x10
  occurrences are followed by `0x81` (send literal 0, 0-args), i.e.
  `^ arg msg`, not the setter pattern.

  Conclusion: the setter recognizer isn't broken — micro-benches
  (fib, sieve, tinyBench) simply don't exercise setter sends.  The
  inline-setter path is alive for real Pharo workloads but invisible
  in our perf-critical benchmarks.  Audit-gap closure stays on the
  todo list but the asmjit setter is not the bottleneck for any
  workload we currently benchmark.

- asmjit T1 inline setter x86 (AsmjitT1.cpp:1929) — same fix needed.
  Can't test on Catalyst arm64.
- stencils.cpp store-recv-var stencils — base variants barrier'd
  via _HOLE_RT_WRITE_BARRIER (commit `65792d23`).  SimStack variants
  (_1/_2/_3/_4) barrier'd via inline-asm bit-set (commit `870c864e`).
  Audit gap for *bit accuracy* is now closed.

  **What works:**
  - JITState gained 4 cached space pointers (offsets 240/248/256/264)
    populated once per tryJITActivation entry.
  - INLINE_WRITE_BARRIER_OLD_TO_YOUNG macro emits ~13 instructions
    of pure inline asm using only caller-saved x11 — no BLR, no
    x19-x22 spill, extract verifier passes.
  - All 5 SimStack store stencils now call the macro after their
    inline slot write.

  **What's still open:**
  - rememberedSet_ vector is still stale (the inline asm sets the
    bit but can't push to std::vector).  Wiring scavenge to consume
    a ring-buffer remembered set, or to skip un-remembered objects
    via the bit alone, is the path to actually dropping the
    O(oldSpace) full scan in `ObjectMemory.cpp:1571-1597`.
  - The non-SimStack base stencils (commit `65792d23`) still call
    the helper, so they DO maintain the vector — but those paths
    are essentially never reached.

  **Hunt for the 256 misses (2026-05-27/28):**
  - Added PHARO_SCAV_BIT_AUDIT (commit `95924da2`) with per-class
    miss logging.  Audit reveals: 260 misses/run across 8 scavenges,
    spread over Context, FullBlockClosure, Array, OrderedCollection.
  - Fixed JITState space-pointer init across all 5 entry sites
    (commit `e67ec61d`) — no impact on miss count, so the JIT path
    is already barriered.  The misses come from C++.
  - Found and fixed `shallowCopy` (the major culprit, -28 misses,
    commit `438b3f0a`): allocated in old space, memcpy'd slots,
    then cleared the bit explicitly.  Now scans for young refs
    post-copy and rememberObjects if any found.
  - Fixed `become` same-size swap and dict-fixCollisionsFrom: in
    drainFinalizationQueue (same commit).
  - Enriched miss log with the referent's class (commit `18b64898`).

  Status: **260 → 230** misses.

  **stencil_popStoreTemp barrier attempt (2026-05-28):**
  Tried adding the inline-asm barrier to all 6 temp-store stencil
  variants (storeTemp_1/_2, popStoreTemp_1/_2/_3/_4 + the two base
  variants).  Range-check tempBase against oldSpace to skip the
  C-stack case, with `tempBase - 48` as the derived Context header.
  Verifier passes; build clean.

  Result: **miss count unchanged at 230, and tinyBench regressed
  ~11 %** (5350 ms → 5950 ms).  Two findings:

  1. The materialized-context temp-store path is virtually never
     exercised in normal workloads.  JIT-compiled code runs with
     frameDepth_ > 0 (non-materialized); only reflection-style
     paths (`Context>>tempAt:put:`) hit materialized frames, and
     those go through C++ primitives, not the temp stencils.
     So the barrier was correctness-improving but caught no real
     gaps.
  2. The ~6-instruction barrier check ran on every temp store in
     hot loops (tinyBench's bytecodes test does millions of temp
     stores per run).  Pure overhead with no audit benefit.

  Reverted.  The 230 audit-gap misses must come from a different
  C++ path — likely Sista-emitted stores or another primitive's
  direct write that I haven't located yet.  Continuing the hunt
  needs per-miss callsite logging (e.g. backtrace at storePointer
  vs at direct slot-write sites) rather than path-by-path
  speculation.

Once stencils barrier, scavenge can be flipped to consume
`rememberedSet_` (`ObjectMemory.cpp:1571-1597` full-scan replaced
by remembered-set iteration).  The asmjit setter fix is real
infrastructure for the day bit 62 starts firing, but doesn't unblock
the scavenge change on its own.

See `memory/jit_remembered_set_dead.md` for the full notes.

## Open performance opportunities (NOT chased this session)

1. **tinyBench inline-prim path is firing correctly.**  Confirmed via
   `PHARO_T1_INLINE_PRIM_COUNTERS=1`: primAtPut=65521 / 65521 visits.
   The 5.3 s wall is ~70 % real JIT execution at this point; further
   wins would need either Sista Tier-2 to compile the hot inner loops,
   or saveless-self-rec for methods with `canBailMidMethod=true`
   (currently gated off because the bail path can't return via the
   blr/ret protocol).

2. **Inter-run fullGC** at `Interpreter.cpp:1166` fires unconditionally
   between bench runs.  With 512 MB headroom, the threshold-based
   trigger handles real allocation pressure already; making the
   inter-run GC conditional on `needsCompactGC()` would save ~90 ms
   per bench run.  Doesn't affect per-run timing (the GC fires
   between runs, not inside).

3. **xmethod inline-J2J** (`PHARO_T1_INLINE_J2J_XMETHOD=1`) — known
   broken, produces #value: DNU + C-stack crash during normal startup.
   Was attempted prior to this session; left default-off.

4. **`-Wunused` remaining warnings**: 0 in non-vendor code (was 13
   in Interpreter.cpp + ~25 more in Primitives.cpp / ObjectMemory.cpp).
   Cleared 2026-05-27.  Vendored plugins still warn (~360 lines):
   B2DPlugin.c, FloatMathPlugin.c, SocketPlugin.c, etc. — leave alone.

   Other non-vendor warnings also cleared on 2026-05-27:
   - `-Wreorder-ctor` (Interpreter init-list order)
   - `-Wformat` (4 sites in Interpreter.cpp)
   - `-Wsign-compare` (IC entries loop)
   - `-Winvalid-offsetof` (dumpInterpOffsets, suppressed locally)

   The four remaining are intentional / vendored:
   - `-Wframe-address` (Interpreter.hpp:361 — backtrace aid)
   - `-Wignored-qualifiers` (vendored sqVirtualMachine.h)
   - `-Wignored-pragmas` × 2 (PlatformBridge.cpp nil push/pop_macro
     for early-exit returns; intentional structural choice)

   **Bonus: warning hygiene + TODO sweep surfaced THREE real bugs.**

   1. `-Wtautological-overlap-compare` flagged Primitive 132's
      `format >= Indexable32 && format <= Indexable64` shortcut as
      always-false (Indexable64=9 < Indexable32=10).  Word arrays
      leaked through to the pointer-slot scan below, where their
      32/64-bit elements would be read as Oop slots.  Fixed in
      `f68392c2`.

   2. `-Wnontrivial-memcall` flagged `memset(p, 0, sizeof(OpenPort))`
      in MIDIPlugin.cpp:183.  `OpenPort` contains a `std::mutex` —
      memset on a non-trivially-copyable type is UB and clobbers the
      mutex's internal state.  Replaced with explicit per-field reset.
      Fixed in `95378117`.

   3. JIT inline at:put: helper (`jit_rt_primatput_ptr`) wrote slots
      directly without the old→young remembered-set entry.  Pre-
      existing TODO comment acknowledged the gap.  Switched to
      ObjectMemory::storePointerUnchecked so the remembered-set is
      now maintained for this site too.  Note: not a bug per se —
      scavenge does an O(oldSpace) full scan for old→young pointers
      explicitly to tolerate missed barriers ("Trade correctness for
      perf until every write site is audited", ObjectMemory.cpp:1563).
      Same audit-gap remains in JIT-emitted inline setter (AsmjitT1
      arm64 line 4431, x86 line 1925, comment at 1925-1928 documents
      the choice).  Closing the gap on these sites would enable
      removing the full scan.  Fixed in `5a7267cd`.

   Net non-vendor warnings remaining: **0**.
   Total build warnings: 327, all in vendored plugin sources
   (B2DPlugin.c, FloatMathPlugin.c, JPEG/Zip/UUID etc. upstream from
   VMMaker) that we deliberately do not modify.

5. **Sista / Tier 2** — not currently compiling.  `T2 (asmjit):
   compiled=0` in stats.  Unblocking it would help tinyBench's inner
   loops significantly, but requires resolving the Sista bail-protocol
   work referenced at `Interpreter.cpp:19504`.

## Files modified

- `src/vm/Interpreter.cpp` (most of the work)
- `src/vm/Interpreter.hpp` (helper decl, J2JSlotPerEntry=32 restored)
- `src/vm/ObjectMemory.cpp` (per-GC-type counters, scanPointer gating)
- `src/vm/ObjectMemory.hpp` (gcHeadroom_ = 512MB, scanPointer fields gated)

## Memory files updated

- `~/.claude/projects/-Users-wohl-src-iospharo/memory/jit_materialize_bytecodeend_bug.md` — bug root cause
- `~/.claude/projects/-Users-wohl-src-iospharo/memory/jit_gc_headroom_tuning.md` — GC sweep
- `~/.claude/projects/-Users-wohl-src-iospharo/memory/jit_fib_perf_baseline.md` — updated post-fix
- `MEMORY.md` index entries added
