# VMMaker / Slang Issues

Last updated: 2026-08-09

Defects in the VM build toolchain and in the VMMaker-generated plugin sources
we compile. Nothing here is a bug in our hand-written C++, and nothing here is
a bug in the Pharo image.

    Image bugs .................. docs/image_issues.md, docs/upstream-proposals.md
    Our own VM defects .......... docs/known-issues.md
    Generator / generated code .. this file

These affect every Pharo VM build that compiles VMMaker output with warnings
enabled, not just iOS. Observed with Apple clang 21 (clang-2100.1.1.101),
arm64 macOS, using the `-Wall -Wextra` that CMakeLists.txt sets on PharoVMCore.

## Summary

322 warnings, all of them in three generated files. Our hand-written code
builds clean at zero.

    src/vm/plugins/B2DPlugin.c ......... 315   (312 unused-function,
                                                  2 unused-but-set-variable,
                                                  1 unused-variable)
    src/vm/plugins/JPEGReaderPlugin.c ..   4   (4 unused-variable)
    src/vm/plugins/DSAPrims.c ..........   3   (2 sign-compare,
                                                  1 unused-variable)

Four of the five findings below are emission artifacts — the generator produces
dead C. The fifth is a genuine logic defect in generated crypto arithmetic and
matters regardless of warnings.

## Where the generator lives

Slang is the Smalltalk-to-C translator the Pharo VM is built with. The
interpreter, the GC and the plugins are written in a restricted subset of
Smalltalk; Slang compiles that subset to the C in `src/vm/plugins/*.c`. The
`/* BalloonEngineBase>>#aaColorMaskGet */` comments above each generated
function are Slang stamping the originating Smalltalk method onto its output.

    Repo ......... https://github.com/pharo-project/pharo-vm
    Package ...... smalltalksrc/Slang            (55 classes)
    Relevant ..... CCodeGenerator.class.st
                   CCodeGeneratorInlineAlwaysStrategy.class.st
                   CCodeGeneratorInlineAsSpecifiedStrategy.class.st
                   CCodeGeneratorInlineNeverStrategy.class.st
                   TInlineNode.class.st
    Local clone .. ~/esrc/pharo-vm  (at 336b3a770 when this was written)
    Tests ........ smalltalksrc/Slang-Tests

That last line is the useful one: Slang has its own test package, so a fix can
be written and tested without building VMMaker or regenerating any plugin.

Slang already has a dead-code-cleaning pass — its history includes
`13db23f4d` "refactoring of the dead code cleaning process", `33f5256cd`
"ensure there is a begin and end comment to every method inlined", and
`eabd581c4` "add tests for switch, found a bug related to inlining". So issues
1-3 are not a missing feature; they are that pass not collecting everything the
inliner leaves behind.

## 1. Out-of-line copies of fully-inlined methods (312 warnings)

File:   src/vm/plugins/B2DPlugin.c  (15,210 lines, from BalloonEnginePlugin)
Symbol: -Wunused-function

Slang inlined every sender of 312 methods — the file contains 1,010
`/* begin NAME */` inline markers — but still emitted each method's standalone
out-of-line definition. Nothing references those definitions, so clang reports
every one of them.

Worked example, `aaColorMaskGet`:

    line  267   static sqInt aaColorMaskGet(void);          <- forward declaration
    line  865   /* BalloonEngineBase>>#aaColorMaskGet */
    line  867   aaColorMaskGet(void)                        <- dead definition
                { return workBuffer[GWAAColorMask]; }

    line 3199   /* begin aaColorMaskGet */                  <- the inlined copy
                cMask = workBuffer[GWAAColorMask];          <- body pasted inline
    line 3427   /* begin aaColorMaskGet */                  <- and again
    line 3582   /* begin aaColorMaskGet */                  <- and again

Verified: **none of the 312 is a `primitiveXxx` entry point.** They are all
per-field accessors and helpers (`aaColorMaskGet`, `aaColorMaskPut`,
`aetStartGet`, `aetUsedPut`, `adjustAALevel`, ...). The plugin's ~60 primitive
exports are all still referenced, so no primitive is unregistered by this and
the plugin is fully functional. This is noise, not breakage.

Proposed fix: after inlining, Slang's dead-code pass should collect any method
whose sender count dropped to zero and skip emitting both its definition and
its forward declaration.

## 2. Unused parameter locals materialised by the inliner (2 warnings)

File:   src/vm/plugins/B2DPlugin.c, lines 13506 and 13684
Symbol: -Wunused-but-set-variable, variable `yValue` both times

When Slang inlines a method that takes parameters, it hoists each parameter
into a local in the caller and assigns the argument to it. If the inlined body
never actually reads that parameter, the local is written and never read.

At 13506, `stepToNextLine` inlines `stepToNextLineIn:at:`:

    static sqInt
    stepToNextLine(void)
    {
        sqInt err;
        sqInt line;
        sqInt x;
        sqInt yValue;                              <- hoisted parameter

        /* begin stepToNextLineIn:at: */
        line   = aetBuffer[workBuffer[GWAETStart]];
        yValue = workBuffer[GWCurrentY];           <- assigned, never read
        x      = (objBuffer[line + GEXValue]) + (objBuffer[line + GLXIncrement]);

Same shape at 13684. Harmless, but it is the inliner leaving a temporary
behind. Proposed fix: drop hoisted parameter locals that the inlined body does
not reference.

## 3. File-scope statics for unreferenced instance variables (3 warnings)

File:   src/vm/plugins/JPEGReaderPlugin.c, lines 113, 116, 183
Symbol: -Wunused-variable — `cbSampleStream`, `crSampleStream`, `ySampleStream`

Slang emits a file-scope `static sqInt` for every instance variable of the
plugin class, whether or not the generated C ever references it. These three
are the Cb / Cr / Y colour-component sample streams; the surrounding decoder
obtains that state another way, so the statics sit unreferenced beside their
used siblings (`cbBlocks`, `cbComponent`, and so on).

Proposed fix: emit only the instance variables the generated code references.

## 4. `__buildInfo` is emitted but then discarded (3 warnings)

Files:  B2DPlugin.c:22, JPEGReaderPlugin.c:21, DSAPrims.c:21
Symbol: -Wunused-variable

    static char __buildInfo[] = "BalloonEnginePlugin VMMaker.oscog-eem.2480 "
                                "uuid: bb3ffda7-... " __DATE__ ;

This exists so build provenance can be recovered from a shipped binary with
`strings`. But it is `static` and unreferenced, so the compiler discards it —
meaning it does not currently do the job it was added for. The warning is
pointing at a real (if minor) failure, not just noise.

Proposed fix upstream: emit it with `__attribute__((used))`, or have
`getModuleName` return it, so the string actually survives into the binary.

Note for us: adding `__attribute__((used))` locally would be a legitimate fix
rather than a suppression, because it changes code emission so the variable
works as intended. We have not done it, because these files are generated.

## 5. Signed/unsigned comparison in DSA big-integer division (2 warnings)

File:   src/vm/plugins/DSAPrims.c, lines 218 and 220
Symbol: -Wsign-compare

**This one is a genuine logic defect, not an emission artifact, and is worth an
upstream issue on its own merits.**

    if ((d2 * q) > ((((usqInt) (firstTwoDigits - (q * d1)) << 8)) + thirdDigit)) {
         ~~~~~~  ^  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The left operand `d2 * q` is `sqInt` (signed long). The right operand is
`usqInt` (unsigned long) because of the explicit cast. C converts the signed
side to unsigned for the comparison, so if `d2 * q` is ever negative it becomes
a very large positive value and the comparison inverts.

This is inside DSA's big-integer division step, so a wrong branch here is a
correctness problem in signature arithmetic, not a cosmetic one. Someone should
establish whether `d2 * q` can go negative for any reachable input. If it can,
the fix is a range check. If it provably cannot, the fix is to make both sides
the same signedness at the source — in the Smalltalk, so it survives
regeneration.

Casting one side in the generated C would only hide the question.

## What we have deliberately not done

Two tempting non-fixes, both rejected:

  - **Suppressing them.** Adding `-w` or `-Wno-*` for these files was tried and
    reverted. A warning is information; silencing it destroys the signal. Every
    time we actually read these diagnostics in this codebase they turned out to
    be worth reading — a never-true format guard in primitive 132, a `memset`
    over a live `std::mutex`, a GC counter that was never reported.

  - **Deleting the dead generated code by hand.** This was also tried and
    reverted. The edit would be silently undone the next time anyone regenerates
    the plugin, and would leave the tree diverged from the generator in the
    meantime, which is worse than the warnings.

Note that CMakeLists.txt does still carry pre-existing suppressions predating
this analysis: `-Wno-unused-parameter` on PharoVMCore, and `-w` on
`src/vm/plugins/jpeg/*`, `src/vm/plugins/testlib/*` and `sqMacSSL.c`. Those are
separate decisions and have not been revisited here.

## Routes to actually resolving this

  1. **Fix Slang, then regenerate.** The real root cause for issues 1-3, and
     the fix benefits every Pharo VM build. Write it against
     `smalltalksrc/Slang-Tests` first, which needs no VMMaker build. Two
     obstacles for regenerating *our* files afterwards: VMMaker is not built in
     the local clone (`build-gen/build/vmmaker/` is absent), and
     `BalloonEnginePlugin.class.st` does not appear to be in that checkout —
     only `BalloonArray.class.st` — so the Balloon plugin source has to be
     located first, possibly in an OpenSmalltalk repo rather than the Pharo fork.

  2. **A checked-in post-generation cleanup script.** A script under `scripts/`
     that strips the redundant out-of-line definitions after VMMaker emits them,
     invoked as part of the regeneration workflow. This makes the cleanup
     reproducible instead of a one-off hand edit, so it is not lost on the next
     regeneration. Cheaper than route 1 and local to us, but it does not help
     anyone else and it is one more thing to keep working.

  3. **Leave them visible and tracked.** What we are doing today: warnings stay
     unsuppressed, root cause recorded here. Costs nothing, hides nothing, and
     the noise makes new warnings in these files harder to notice.

Issue 5 is independent of all three and should be reported upstream regardless.
