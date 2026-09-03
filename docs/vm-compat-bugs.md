# VM-compatibility bugs (our VM fails, Cog passes)

## ~~arm64 JIT crashes (SIGBUS) on a runtime-compiled hot loop~~ — FIXED `260692fa` (2026-08-19), VERIFIED 2026-08-22

Filed 2026-08-19 and fixed the same day by `260692fa` ("guard the
isSpliceTarget header writes with a W^X window"); the entry was never
retired, so it sat at the top of this file as the newest open defect for
three days.  Re-measured at HEAD on 2026-08-22 with the repro below:

    build-rel  arm64    rc=0   EVAL-RESULT='20000'
    build-x86  x86_64   rc=0   EVAL-RESULT='20000'

The diagnosis in the original entry was right, and `JITRuntime::tryExecute`
now carries the reasoning at the store site: `jm->isSpliceTarget = true` is a
one-byte write into a JITMethod header that is bump-allocated INSIDE the
MAP_JIT code zone, on an entry path that runs in EXECUTABLE mode.  The
"unaligned tagged-immediate-looking" fault address was `jm + 45`, the offset
of that field.

Original report, kept for the shape it names:

Hard crash, reproduced independently 3/3. Six lines, base Pharo 13 image, no
package load required:

    | o r |
    Object compile: 'pbLoopAdd | s | s := 0. 1 to: 20000 do: [:i | s := s + 1 ]. ^s'.
    o := Object new. r := 0.
    1 to: 400 do: [ :k | r := o pbLoopAdd ].
    r printString

    build-rel arm64            rc=139   42 crash lines, no result
    build-rel arm64 NO_JIT=1   rc=0     EVAL-RESULT='20000'
    build-x86 x86_64           rc=0     EVAL-RESULT='20000'

So it is arm64-specific AND JIT-specific: the same binary with `PHARO_NO_JIT=1`
returns the right answer, and the x86_64 build is fine.

Faulting frame:

    [CRASH] PC in C symbol:
      pharo::jit::JITRuntime::tryExecute(Oop, JITState&, JITMethod*) + 1188
      2  pharo::Interpreter::tryJITActivation(Oop, int) + 3056
      4  pharo::Interpreter::activateMethod(Oop, int) + 5184

    [SIGSEGV] Signal 10 caught!  Fault addr=0x11219db6d
    x3=0x11219db40   (fault addr is x3 + 0x2d)

Two details worth having before anyone starts:

  * **Signal 10 is SIGBUS on macOS, not SIGSEGV.** The handler prints the label
    `[SIGSEGV]` for any fatal memory signal, so the label is misleading.
  * The fault address is **not 8-byte aligned** (`...b6d`, low bits 0b101).
    In this VM low-bits 101 is the SmallFloat tag, so this looks like a tagged
    immediate being dereferenced as a pointer rather than a wild address.

Shape that triggers it: compile a method AT RUNTIME with `compile:` whose body
is a counting loop, then call it enough times to go hot. Found incidentally by
a JIT performance investigation, not by the test suites -- no SUnit or package
test covers runtime `compile:` followed by a hot call, which is why three green
tiers did not catch it.


Two parts:

  * **[OPEN DEFECTS](#open-defects-as-of-2026-08-12)** — the current work list.
    Audited against HEAD on 2026-08-12; every entry carries a repro, the commit
    it was last verified at, and an arch.
  * **The 2026-06-01 investigation** that this file started as, below the open
    list. That part is a record — the WideString, scavenge-root and Sista
    `^self` bugs in it are all fixed.

Nothing here is a "deferred feature" or an intentional stub. Those live in
`docs/deferred.md`, which is now only that. Fixed work is recorded in
`docs/changes.md` and `docs/history/`.

## NEW 2026-08-22 — four dead weak subscriptions are never removed from the system announcer

Two tests report it, and they are NOT equally strong evidence — read both
before quoting either.

**`ReleaseTest>>testNoDeadSubscriptions` is the real one.** It runs on Pharo
CI today (its comment says "we skiped this on the ci **in the past**:
pharo-project/pharo#2471"), so upstream expects it to pass:

    testNoDeadSubscriptions
        3 timesRepeat: [ Smalltalk garbageCollect ].
        dead := self class codeChangeAnnouncer subscriptions subscriptions
                    select: [ :sub | sub subscriber isNil ].
        self assert: dead asArray equals: #()

Ours fails it with four entries:

    a WeakAnnouncementSubscription (nil subscribes to MethodAdded)
    a WeakAnnouncementSubscription (nil subscribes to ClassRemoved)
    a WeakAnnouncementSubscription (nil subscribes to MethodModified)
    a WeakAnnouncementSubscription (nil subscribes to MethodRemoved)

`nil subscribes to X` means the weak reference to the subscriber WAS cleared
correctly — the subscription object itself is what never got removed from the
announcer, and removing it is finalization's job.

**`WeakAnnouncerTest>>testNoDeadWeakSubscriptions` reproduces the same four in
13 s in isolation**, which is the useful part:

    printf 'WeakAnnouncerTest\n' > /tmp/sunit_class_names.txt
    build-rel/test_load_image <prepped sunit image>
    -> 34 tests, 32 P, 1 F, 1 S

but it is **skipped on Pharo CI by its own first lines** (`self longTestCase.
self skipOnPharoCITestingEnvironment.` — that reads
`PHARO_CI_TESTING_ENVIRONMENT` out of the process environment, which our
harness does not set). So upstream makes no claim about it, and it cannot be
cited as "Cog passes this". Use it as a fast reproducer, not as evidence.

This corrects the "harness self-pollution" label `ReleaseTest`'s four FAILs
have carried since 2026-08-11: `testNoOrphanPackage`,
`testThatThereAreNoSelectorsRemainingThatAreSentButNotImplemented` and
`testUnknownProcesses` genuinely do name the injected runner, but
`testNoDeadSubscriptions` does not — WeakAnnouncerTest reproduces it with no
other test class in the image.

Not established: whether stock Cog passes `testNoDeadSubscriptions` on this
image. There is no runnable Cog on this host (it aborts allocating its code
zone at 0x320000000), so that comparison needs another machine.

Also worth deciding, separately: our sweeps do not set
`PHARO_CI_TESTING_ENVIRONMENT`, so we run every test Pharo's own CI skips.
That is more coverage, but it means some of our failures are tests upstream
has already given up on, and they should not be counted against the VM
without checking for that guard first.

## NEW 2026-08-22 — x86_64: a SmallInteger reaches `cull:` in a JIT'd `ifNotNil:ifNil:` after a J2J return

Filed first as "x86_64 only, dies with 'only integers should be used as
indices'". Three tests later that description is wrong in almost every
particular, and what is left is worse: **the abort is intermittent and
produces no error at all.**

The same command, verbatim from `scripts/package-tests-selfhosted.sh`, run
back to back on x86_64:

    verb-1  rc=0  613 s  image 1262 MB   loaded
    verb-2  rc=0   54 s  image   52 MB   did nothing, and printed NOTHING

`verb-2` stops after three Metacello lines, at
`MetacelloNotification: Project: BitmapCharacterSet` — the FIRST dependency,
i.e. inside a `github://` sub-project clone. `pre.txt` was written correctly
(2185 classes). There is no exception, no error line, no crash. The VM
simply does not reach `Smalltalk snapshot: true andQuit: true` and exits
zero, which the package script faithfully records as `load rc=0
DID-NOT-PERSIST`.

**An exit code of 0 on a load that did nothing is the real defect here.**
Everything downstream — the harness, the summary, anyone reading it — treats
rc=0 as success.

#### FIXED 2026-08-22: eval mode now exits non-zero when the eval never completed

`main()` in `test_load_image.cpp` ended in an unconditional `return 0`, so
every way an eval can die short of a quit primitive — the command's process
being terminated by a DNU, the idle-deadline guard at
`Interpreter.cpp`'s `cg_exit`, or `startup.st` never being executed at all —
was reported as success. The pre-existing "eval never ran" guard already
printed `ERROR:` for the third of those and then returned 0 anyway.

`Interpreter::evalReachedQuit_` is now set on the two paths that take a real
quit (`primitiveQuit`'s `running_ = false`, and the deferred-quit honouring in
the bytecode checkpoint). At exit, eval mode with the flag still false prints
what happened and returns **71**; the leftover-`startup.st` guard returns
**70**. Non-eval modes are untouched and still return 0.

Verified: `3 + 4`, `eval --save`, and `Smalltalk snapshot: true andQuit: true`
all still exit 0 (no false positives on the shapes the harness actually runs),
and `test_class_table` / `test_relaunch` still pass.

This does not fix the abort itself — it makes the abort impossible to record
as a pass. The intermittent `cull:` DNU below is the separate, still-open bug.

### What was ruled out on the way, each by measurement

  * *"x86_64 cannot load XMLParser."* It can: 709 s, 1265 MB image, 0 errors,
    the same expression without the preamble. And `verb-1` above.
  * *"The write-stream preamble is the trigger."* Run alone it is clean on
    both arches, JIT and no-JIT: 2185 classes written, 0 errors. The
    `StdioStream`/`ZnUTF8Encoder` frames in the original stack were the error
    being REPORTED to stderr.
  * *"'only integers should be used as indices' is the failure."* `verb-2`
    failed the same way with no such error, so that message is one
    manifestation, not the cause.

### CORRECTION (same evening): the arm64 failure was DNS, a different cause

`de6e1138` claimed "it is NOT x86-specific — arm64 does it too, in 3
seconds", on the strength of this pair:

    armverb-1  rc=0  354 s  image 1226 MB   loaded
    armverb-2  rc=0    3 s  image   52 MB   did nothing

That was wrong, and the commit asserting it is already pushed. Reading
`armverb-2`'s log properly — the error is above the JIT-statistics block at
the tail, which is where I had been looking — it says:

    Error: IceGenericError: failed to resolve address for github.com:
           nodename nor servname provided, or not known

That is a DNS resolution failure. Environmental, and it PRINTS. So the three
observed failures have three different causes:

    armverb-2   arm64    3 s   DNS: cannot resolve github.com
    verb-2      x86_64  54 s   no error of any kind          <-- the silent one
    pkg-x86     x86_64  31 s   "only integers should be used as indices"

arm64 still has ZERO confirmed instances of the silent mode. Whether that
mode is x86-specific is therefore OPEN again, not refuted, and the
consequences drawn for the package numbers (that XMLParser's arm-pass /
x86-fail split was a coin flip, and that PolyMath's 59-test gap is the same
variance) do not follow from this evidence and should not be relied on.

### And the "silent" x86_64 run is not silent either — it is a JIT displacement

Reading `verb-2`'s log all the way through rather than its tail, the failure
is a `doesNotUnderstand`, logged under `[DNU-*]` markers rather than
`Error:`, which is why every grep so far had missed it:

    [DNU-RCVR] #cull: rcvr=0x11 cls=SmI ... depth=5
    [DNU-STACK]   [0] Context>>privRefresh
    [DNU-STACK]   [1] TFStringType>>freeValueIfNeeded:
    [DNU-STACK]   [current] #ifNotNil:ifNil: fd=1
    [DNU]   rcvr is SmallInteger 2
    [DNU]   arg[0] = 0x9 (SmallInt)                  <- SmallInteger 1
    [DNU-JIT] lastJitReturn: method=#allocatedStrings retVal=...(obj 0)
    [DNU-JIT]   [0] #privRefresh <<JIT>>
    [DNU-JIT]   [1] #freeValueIfNeeded: <<JIT>>

`ifNotNil:ifNil:` sends `cull:` to its block argument, and the receiver of
that send is **SmallInteger 2**. Both frames are JIT-compiled, and the JIT
had just returned from `#allocatedStrings`. A non-block sitting in a block
slot immediately after a J2J return is operand-stack displacement — the same
shape as defect `#1` in this file (the WarpBlt expression-stack displacement,
`bf3f6c58`, "one phantom operand per phi").

So the three failures classify as:

    armverb-2   arm64    3 s   DNS, environmental, prints IceGenericError
    verb-2      x86_64  54 s   JIT displacement: SmallInteger reaches cull:
    pkg-x86     x86_64  31 s   "only integers should be used as indices"
                               -- plausibly the same displacement landing on
                               an indexing primitive instead, NOT established

None of this is "a package load aborts silently". The VM exits 0 because the
image's error handler runs, reports, and the eval never reaches its snapshot
— which is a real reporting weakness worth fixing on its own, but it is a
consequence, not the defect.

### A structural asymmetry that fits, verified from the source

`emitMethodBytes` (`src/vm/jit/asmjit/AsmjitT1.cpp`) has one arch split:

    10079:  #if defined(__x86_64__)
    10435:      if (op >= SistaV1::ExtendA)   // 0xE0..0xFD
                    loopBail(EXIT_ARITH_OVERFLOW)      <-- catches 0xF9 / 0xFA
    10493:  #elif defined(__aarch64__)
    10869:      if (op == SistaV1::PushFullBlock)      // 0xF9
                    ... ExitBlockCreate                <-- dedicated handler

`PushFullBlock` is 0xF9 and `PushClosure` is 0xFA (`SistaV1.hpp:106-107`), so
on **x86_64 both block-creating bytecodes fall into the generic
extended-bytecode bail**, while arm64 routes them to `ExitBlockCreate`, whose
handler creates the closure and syncs the interpreter globals.

That is this repo's documented `per-arch-backend-drift` class — the one that
has already produced three bugs — and it fits every feature of the evidence:
x86-only, a non-block where a `FullBlockClosure` belongs, and small
sequential integers (1 and 2) of the kind that live in a neighbouring frame's
temps. The intermittency fits too: it needs the callee JIT-compiled with its
IC already upgraded to a J2J entry.

**Fits is not causes.** The structure is confirmed; that it produces THIS DNU
is not. Four knobs decide it, run as rate-vs-rate because the failure is
intermittent (a single passing run proves nothing):

    PHARO_T1_EXTBAIL_SEND=1      most targeted -- changes that exact bail's code
    PHARO_T1_X86_NO_XMETHOD=1    disables x86 cross-method inline-J2J
    PHARO_T1_NO_CALLER_RESUME=1  disables the documented x86 per-send sp leak
    PHARO_T1_XMETHOD_MAX_IC=0    restores leaf-only callees

### The rate is not ~50% — it is 0 in 15, and that changes the method

Measured, 150 s cutoff (all three observed failures happened inside 54 s, so
a run that clears 150 s has cleared the window):

    arm64    would-load 8   DNU 0   DNS 0   other 0   of 8
    x86_64   would-load 8   DNU 0   DNS 0   other 0   of 8

**Zero reproductions in sixteen runs.** The "roughly half the time" in the
earlier revision of this entry came from one failure in two samples and was
small-sample noise, not a rate. That kills the suppressor A/B outright: with
no baseline failures there is nothing for `PHARO_T1_X86_NO_XMETHOD` or
friends to suppress, and any "it passed 8/8 with the knob" would be
measuring nothing.

So the lever has to work the other way. `PHARO_T1_NO_CHAIN_RESUME_PLAIN`
(debug_vars.h:123) opts OUT of the 2026-06-19 "COLD-BOOT DOUBLE-POP FIX",
deliberately restoring

    "cold IC-miss nArgs>0 sends (do:-loop at:/value:) over-popped
     = the nested do:/on:do: cold-startup operand SHIFT"

which is our symptom class stated in the codebase's own words — and our
failure is a cold `cull:` (nArgs=1) inside `ifNotNil:ifNil:` (nArgs=2). If
setting it makes the DNU frequent, the operand-shift class is confirmed live
on this path and there is finally a repro to bisect the suppressors against.
If it does NOT, this whole line of investigation is unsupported and the three
surviving mechanisms below go back to being reading, not evidence.

**Not on x86 — but 4 of 4 on arm64.** `PHARO_T1_NO_CHAIN_RESUME_PLAIN=1`:

    x86_64   DNU=0  ok=6  of 6
    arm64    DNU=4  ok=0  of 4      every run

(An earlier revision of this paragraph said "the amplifier does not reach this
workload either". That was written from the x86 half while the arm half was
still running, and it was wrong.)

Two things follow, and they point in opposite directions:

  * **The 2026-06-19 double-pop fix is load-bearing on arm64.** Opt out of it
    and cold startup corrupts on every run, with exactly this DNU shape —
    `#+` sent to `false`, `#includes:` sent to `IdentitySet class`, inside
    `ExternalObject class>>installSubclasses` / `WorkingSession>>startup:`.
    A wrong receiver in a correctly-shaped frame. That is a useful standing
    validation of a fix that had none.
  * **It is NOT the mechanism for the x86 `cull:` DNU.** x86 does not exhibit
    the bug even with the double-pop deliberately restored, so whatever
    x86 does on this path, it is not this. The arch split is the opposite way
    round from the failure being chased.

So the three surviving mechanisms stay demoted: readings of the source, not
evidence about this DNU. Its honest status is **one unreproduced failure with
a good log and no established cause** — 0 in 16 default-config runs, 0 in 6
amplified ones.

Start by getting a repro, not by acting on the mechanisms. The package tier
hit this twice in one day while 22 deliberate attempts did not, which points
at something the package script does that an isolated eval does not.

### What a 12-agent adversarial pass established about the mechanism

Three exclusions, all verified against source, all high confidence:

  * `#ifNotNil:ifNil:` and `#cull:` are not inlined or special-cased anywhere
    in the JIT. The only nil-check peephole is an arch-neutral interpreter
    fusion in `op_dup` that never materialises a block.
  * `argCount` is a compile-time constant from the opcode on both sides. The
    recorded dump proves it independently: `pushFrame` nil-fills
    `numTemps - argCount`, and `Object>>ifNotNil:ifNil:` has numTemps=2, so
    argCount=1 would have left FP[2]=nil — it holds an object. Only
    argCount=2 fits. **The bad value was already in the arg slot when the
    frame was activated.**
  * The block-value stencil specialisation is dead code in a default build
    (asmjit-T1 short-circuits before `applyICSpecialization`).

Two candidates were REFUTED by the verify pass and are not worth re-deriving:
a claimed x86 resume-override single-point-of-failure (contradicted by
`t1ICProbe` defaulting true at DebugSettings.cpp:117), and a claimed no-op
`bailMatJ2J` (contradicted by the per-`tryJITActivation` JITState/J2J slice
lifetime at Interpreter.cpp:27750, 27877-27888).

Where to look: the load stops inside a libgit2 `github://` clone, which is
also where `Fuel` sits for 17 minutes against a repository that no longer
exists. A VM that exits 0 after a Smalltalk process was terminated mid-eval
is the shape described in memory `eval-deadline-killed-long-work`.

## OPEN DEFECTS (as of 2026-08-12)

**Update 2026-08-12 (later session): `#1` and `#5b` are FIXED.**  `#1` was the
top open VM bug and was NOT where six sessions had it filed — see its entry.
Ten remain: `#2` (partially), `#3`, `#4`, `#6`-`#12`, `#14`.

Provenance: these were extracted from `docs/deferred.md`, which had grown to
2739 lines of which 86% was changelog and lab notebook. Five of them had been
invisible because they sat *inside* entries whose parent was checked `- [x]`.

### 1. ~~WarpBlt expression-stack displacement~~ — FIXED 2026-08-12 (`bf3f6c58`)

**CI corroborates the fix, and was ignored while doing so.** The
`warpblt-regression` job asserts the bug is still present, so that fixing it
fails loudly; it duly failed with `0 errors - docs/vm-compat-bugs.md #1 appears
FIXED` on every push after `bf3f6c58` — five days of red mail nobody read,
because the fixing commit updated this file but not the job's EXPECT_ERRORS.
The job is disabled as of 2026-08-17; re-enabling it with EXPECT set to 0 turns
it into a genuine regression guard and is a one-line change.

Root cause: the **per-bytecode Sista backward-jump tier**, not the
materialize/restore path six sessions had it filed under.

    repro   PHARO_DET_SCHED=1 build/test_load_image <image> eval \
              "$(cat scripts/repro/warpblt_temp_displacement.st)"

            before   ~796 calls / 12 errors, every run
            after    1440 calls /  0 errors — byte-identical to stock Cog,
                     JIT on AND PHARO_NO_JIT=1

MECHANISM.  A region lifted from a backward-jump target is registered at
every interior loop top it built a dispatch arm for.  Entering at an
interior arm needs the target block's phi inputs, so the builder appends a
loader pseudo-block whose `kLoadStackSlot` ops read them off the
interpreter's operand stack.  It read them and never lowered `state.sp`.

Each phi operand was therefore live TWICE: once in the region's registers,
which the next bail writes back starting at `state.sp`, and once in the
copy still sitting below it.  The resumed frame gained one phantom operand
per phi and an expression evaluated against its neighbour.  Depth of the
leak = the entry block's phi count — 1 for an arm one loop in, 2 for two,
which is the +1 / +2 measured at the bail.  Temps stay correct throughout,
which is why the 2026-08-11 localizer (all temps CORRECT, failing sends at
stable pcs) pointed at "the expression stack" and no further.

Fixed in BOTH backends (`SistaLowering_arm64.cpp`, `SistaLowering_x86_64.cpp`
— the whole-file per-arch split had the identical omission, the shape in
memory `per-arch-backend-drift`): after a loader's `kLoadStackSlot` ops and
before its terminator, `state.sp -= phiCount * 8`.

**`PHARO_NO_JIT=1` does not disable this tier** — that is why every earlier
session concluded the bug was interpreter-side.  The knob that isolates it
is `PHARO_NO_SISTA_PER_BC=1`.

#### The instrument that found it — `PHARO_DEPTH_ORACLE=<exact selector>`

A self-calibrating operand-stack-depth oracle: record the depth the FIRST
time each bytecode offset executes, report every later visit that
disagrees.  Loop and straight-line code has a fixed depth per pc, so it
needs no static analysis, and it fires at the FIRST displaced bytecode
rather than at the impossible-receiver DNU downstream.  On divergence it
dumps the live frame, the owning context, a 48-entry ring of
restore/save/return/push/pop/materialize/transfer/Sista-dispatch events and
a 160-entry ring of (offset, depth, bytecode, ctx, fp).

Those two rings are the whole method: they turned "the expression stack is
displaced somewhere" into

    SISTA-IN  entry=340 depth=25
    SISTA-OUT bail=640  depth=28   (expected 26)

in a single run.  Companion lift-time traces `[SISTA-LIFT-BAIL]` /
`[SISTA-LIFT-POP]` (under `PHARO_SISTA_BJ_TRACE`) show a region's bail
offsets and modelled pops, which is what ruled the lifter itself IN as
correct and moved the search to the dispatch prologue.

Ruled out on the way, each by measurement: the `raiseContextStackpTo`
write-through (`PHARO_NO_CTX_STACKP_RAISE=1` — 12 errors), the Context GC
trace bound (`PHARO_CTX_TRACE_ALL_SLOTS=1` — 12 errors), the return-value
placement gap, the ctxSynced skip, preemption frequency.

SUITE-LEVEL CONFIRMATION (macOS-arm64, COMPLETE — 2046 classes), and again
at the end of the session with every later fix in:

                        #1 only    all fixes    2026-08-11 baseline (same box)
    pass                27729      27732        27701
    fail                   24         22           22
    error                  20         21           50
    ClyBrowserToolValidityTest   25 P / 0 F / 0 E    (baseline: 25 ERRORs)
    ClyNotebookPageRecyclerTest   8 P / 0 F / 0 E    (baseline:  8 ERRORs)
    Cly* classes with any F or E:  NONE (140 Cly result lines)

The ERROR column is the one that moved: 50 -> 20, and the 33 errors this
defect owned are gone.  FAIL moved by one, inside the harness's documented
few-tests-per-run noise floor.  Every remaining non-clean class is in an
already-documented residual family — `OCClassBuilderTest` (upstream image
bug, `docs/image_issues.md:184`), `ReleaseTest` run-order pollution, the
Spec/GUI adapters, `StDebugger`/`StSpotter`/`StTranscript`, TKT — plus
`FTTableMorphTest`, which passes on our VM in isolation (see below).

Those 33 are the exact errors section 10 of the 2026-08-11 write-up
attributed to this defect (`MessageNotUnderstood: SmallInteger >>
#pixelAt:`), and they are gone.  The two remaining errors are both
accounted for: `OCClassBuilderTest` is the stock-Cog-identical upstream
image bug (`docs/image_issues.md:184`), and `FTTableMorphTest>>
testCanAlternateRowColors` PASSES on our VM when run through the same
startup.st path the suite uses — it is suite-context GUI state, not a VM
defect (and NOT the `eval`-mode result, which is contaminated: our
generated startup.st suspends every Morphic process, so any test that
needs a redraw fails under `eval` on our VM and passes on Cog for reasons
that have nothing to do with the VM.  Compare the SAME isolation on both
VMs — the lesson already recorded further down this file).

Do NOT read the raw error count as a trend — `UITheme>>formSetsForScale:`
is `at:ifAbsentPut:`, so the count is bimodal (1 or ~100) for a fixed
defect.

### 2. 13 packages our VM cannot run at all, where Cog can — HIGH

Triaged 2026-08-12 on a fresh aarch64 box, each package standalone with a
1800 s budget per arm and exit codes recorded (4 workers, not 8, so an OOM
cannot masquerade as a crash again). Raw:
`docs/results/pkg16-triage-2026-08-12.txt`.

Of the 16 originally flagged, **3 were OOM collateral and now run clean**
(`moosetechnology-fast`, `-fast-java`, `shnarazk-aoc-in-pharo`). The other 13
are real, and they are worth **~6,000 tests that stock Cog executes and we
report nothing for**. They fall into three families:

#### 2a. Startup/resume: 7 packages never reach the runner — NOT JIT-dependent

Our VM never prints the runner's first landmark. `PHARO_NO_JIT=1` behaves
identically, so this is not the JIT. Cog runs every one of them.

    package                            cog                     ours
    evref-bl-mcp                       696 P in 62 s           rc=0 in 4 s, no output
    mumez-pharo-acp                    170 P in  1 s           rc=0 in 4 s, no output
    pillar-markup-pillar                18 P in  1 s           rc=0 in 3 s, no output
    fedeloch-ume                        11 P in 49 s           rc=0 in 0 s, no output
    moosetechnology-famix             1293 P in 21 s           hangs, killed at 1800 s
    tomooda-viennatalk                1430 P in 86 s           hangs, killed at 1800 s
    moosetechnology-gitprojecthealth     6 P in  0 s           hangs, killed at 1800 s

Two sub-modes. The first four exit 0 within seconds having done nothing; the
VM's own guard fires — *"eval never ran — startup.st was still on disk at exit,
so StartupPreferencesLoader did not execute it"*. The last three hang instead,
before printing anything. `gitprojecthealth` is the sharpest: Cog finishes 6
tests in under a second, we hang for 1800.

`fedeloch-ume` shows what this looks like from inside:

    [RESUME] Initial context: method=#? pc=nil
    [RESUME] Not in snapshot code — resuming as-is
    Interpreter initialization failed (may need process setup)

i.e. an image that stock Cog resumes normally, our VM cannot boot. NOTE this
supersedes the earlier "ume SIGSEGVs in every configuration" filing — that was
measured on a differently-loaded image; on this one it fails to start instead.

#### 2b. Three JIT-dependent losses — `PHARO_NO_JIT=1` fixes all three

The runner starts, enumerates classes, then the run is lost. With the JIT off,
each produces a normal RESULT.

    juliendelplanque-jrpc          cog  81 P    ours exits 0 after CLASSES
    punt-labs-anthropic-sdk-pharo  cog 901 P    ours exits 0 after CLASSES
    j-brant-smacc                  cog 557 P in 30 s   FIXED — see below

**`j-brant-smacc` is FIXED 2026-08-12, and it was never JIT-dependent.**  Its
image is MULTI-SEGMENT (97.8 MB, firstSegmentBytes 74.7 MB, 4 segments), so it
was hitting defect #4 — the loader mis-parsed at a bridge and corrupted the
heap.  With segment support (`6edf6b33`):

    RESULT pass=557 fail=0 err=8 timeout=0 classes=9

i.e. **the pass count matches Cog's 557 exactly**, and the "hangs to 1800 s"
was never a hang.  The 8 residual errors are all `SmaCCEndToEndTest` sending
`#subclass:instanceVariableNames:classVariableNames:category:` and
`ByteSymbol>>#asClass` — a Pharo 13 API removal, not a VM defect (Cog baseline
re-run pending to confirm the same 8).

LESSON for the rest of this list: "hangs to 1800 s" and "exits 0 after CLASSES"
were treated as a JIT/scheduler family for months.  At least one of them was an
IMAGE LOADER bug, and the tell was in the image header all along —
`scripts/image-segments.py <image>` answers it in milliseconds.  **Check the
other packages in #2a/#2b for multi-segment before assuming anything else.**

#### The table above is stale — five of the 13 were re-measured on 2026-08-13

Acting on exactly that lesson closed most of #2a, and the results sat in the
repo-root `WIP.md` (now `docs/history/wip-root-2026-08-13.md`) rather than
here, which is why this defect still reads as 13 packages.  Freshly loaded,
all with segment support:

    package                            cog                 ours
    mumez-pharo-acp                    168 P/0 F/2 E       168 P/0 F/2 E   EXACT
    moosetechnology-famix             1293 P/5 F/2 E      1292 P/5 F/2 E/1 T
    fedeloch-ume                      times out at 1200s  loads + runs (6 segments)
    moosetechnology-famixtagging         0 P/115 E           0 P/115 E    broken pkg, parity
    apptivegrid-soil                   464 P/6 F/1 E      runs; bounded by #6

`famix` was never a hang and `famixtagging` fails identically on both VMs, so
it should never have been counted as ours.  `ume` prints
`[IMGLOAD-MULTISEG] 6 segments` and boots where it used to answer "Interpreter
initialization failed".  `soil`'s SIGABRT was `544740ac` — super sends did not
honour `objectAsMethod`.

Still open from the two lists: `evref-bl-mcp`, `pillar-markup-pillar`,
`tomooda-viennatalk`, `moosetechnology-gitprojecthealth` (2a), and `jrpc` /
`anthropic-sdk` (2b) — the latter two no longer "exit 0 after CLASSES" but
fail on sockets instead (`EADDRINUSE` on the ~12th server start;
`primSocketReceiveDataAvailable:` inside a threaded Zn mock).  Nobody has
re-run the six against a current build, and there is no live Cog baseline on
this host to compare them to.

**2026-08-13: acted on that lesson — two more closed, and the family is
smaller than it looked.**  Each package reloaded from scratch with stock Cog,
segment-checked, then run on both VMs:

    package                 segments  cog                    ours (2026-08-13)
    moosetechnology-famix   MULTI (4) 1293 P/5 F/2 E/0 T     1292 P/5 F/2 E/1 T
    mumez-pharo-acp         MULTI      168 P/0 F/2 E/0 T      168 P/0 F/2 E/0 T
    apptivegrid-soil        single     464 P/5 F/1 E/1 T     SIGABRT (rc=134)
    moosetechnology-famixtagging single   0 P/115 E             0 P/115 E (parity)
    punt-labs-anthropic-sdk single     916 P/0 F/0 E         CLASSES 113 then lost
    smalltalkweb-myprecious single     107 P/0 F/2 E          90 P/2 F/16 E -> see #20
    juliendelplanque-jrpc   single      81 P/0 F/0 E         CLASSES 11 then EADDRINUSE

`moosetechnology-famix` — the second-largest entry in #2a at 1293 tests — was
NOT a hang either.  Same root as smacc: multi-segment image, defect #4.  It
now runs to completion with the same fail/error sets as Cog; the single delta
is one JIT-only TIMEOUT (`MooseScriptsTest>>testCreateLightModels`).

`mumez-pharo-acp` is at EXACT parity — see #5, its 7-test residual was one
missing plugin primitive, not a startup problem.

`punt-labs-anthropic-sdk-pharo` is single-segment, so the loader fix does not
touch it — but it is no longer opaque.  It reaches `CLASSES 113` and runs;
what kills the run is inside its `ZnManagingMultiThreadedServer` mock, where
`Socket>>primSocketReceiveDataAvailable:` fails and the image signals
`SocketError: Undefined error: 0` (errno 0), after which a DNU of
`#printMethodAndUriOn:` on a NIL receiver comes out of `Context>>errorReportOn:`
while reporting that error.  A plain `ZnServer` + `ZnClient` round trip is
identical on both VMs (both answer `'pong'`), so the divergence needs the
package's multi-threaded server to reproduce.  See memory
`zn-socket-hunt-lost-wakeups` before chasing it.

`moosetechnology-famixtagging` fails identically on both VMs (115 errors, 0
passes): a broken package, not a VM defect.  It should never have been counted.

So of the "13 packages our VM cannot run at all", the multi-segment loader
alone accounted for smacc + famix (~1850 tests), and the OSProcess pipe gap
for acp.  What is left is `soil`'s SIGABRT, `mutalk` (#3), and the packages
that still need a retest on a freshly loaded image.

`jrpc` reproduces on macOS too (`CLASSES 11` then no RESULT, exit 0, ~7 s) but
via a DIFFERENT proximate cause — an unhandled error out of JRPC's
`ZnMultiThreadedServer` listen loop:

    Socket>>listenOn:backlogSize: <- ZnNetworkingUtils>>serverSocketOn:
      <- ZnSingleThreadedServer>>initializeServerSocket <- listenLoop
      <- [[self listenLoop] repeat] in ...>>start <- BlockClosure>>newProcess

so the macOS run cannot be used to chase the Linux cyclic-chain bug — the
Linux investigation needs a Linux box. What the two DO share is the outcome:
an error raised in a FORKED process ends the whole eval with no RESULT, where
stock Cog completes 81 tests. That shared shape looked like a general defect and is NOT:
tested directly (fork a process that raises, check the main eval survives) and
**stock Cog dies exactly the same way** — both VMs print only the first marker.
So an unhandled forked-process error ending an eval is standard Pharo eval
behaviour, not our bug. Hypothesis disproven; do not re-test it.

**2026-08-13 re-measurement on a freshly loaded jrpc image: the macOS
proximate cause is now pinned, and it is NOT what the paragraph below says.**
Our VM reaches `CLASSES 11` and RUNS the tests; the run dies part-way with

    SocketError: Address already in use
    Socket>>primSocket:bindTo:port: <- Socket>>bindTo:port: <- bindToPort:
      <- listenOn:backlogSize: <- ZnNetworkingUtils>>serverSocketOn:
      <- ZnManagingMultiThreadedServer(ZnSingleThreadedServer)>>initializeServerSocket

i.e. by the ~12th test a previous test's LISTEN socket has not been released.
Cog reaches `RESULT pass=81` on the same image.  This reproduces with the Cog
arm not running at all, so it is not the harness reusing a port.

THREE simpler forms of it are at PARITY — do not re-derive these:

    Socket newTCP listenOn:backlogSize: + closeAndDestroy, 8x    8/8 both VMs
    Socket bindTo:port: + listenWithBacklog: + close, 8x         8/8 both VMs
    ZnServer on: start/stop, 10x same port                     10/10 both VMs
    ZnManagingMultiThreadedServer start + GET + stop, 14x      14/14 both VMs

So it needs JRPC's own server lifecycle to reproduce; a bare Zn multi-threaded
server on a reused port is not enough.

What that leaves for macOS jrpc is sharper: Cog reaches `RESULT pass=81` on the
same image, so under Cog the server socket LISTEN succeeds and under ours it
does not. That is a socket-layer divergence (`Socket>>listenOn:backlogSize:`),
and prior art exists — see the `zn-socket-hunt-lost-wakeups` memory for this
VM's socket failure modes and the diagnostic pattern that cracked them.

**BUT the simple form of that inference is refuted.**  Measured 2026-08-12 on
the SAME base image, both VMs:

    Socket newTCP; listenOn: 42731 backlogSize: 10   ours = Cog: succeeds,
                                                     isValid=true, localPort ok
    ZnServer on: 42732; start                        ours = Cog: isRunning=true

So plain `listenOn:backlogSize:` and a `ZnServer` start are NOT divergent.
Whatever fails in jrpc is narrower than "our listen fails" — it needs the
package's own forked listen loop to reproduce, and the claim above should not
be carried forward as established.

On Linux, `jrpc` shows a VM-level mechanism:

    [VM] primitiveFindHandlerContext: cyclic sender chain at 0x...
    Error: This Delay has already been scheduled.
      DelaySemaphoreScheduler>>schedule: <- DelayWaitTimeout(Delay)>>schedule <- ...

A context's sender chain contains a CYCLE, which the VM detects with Floyd
tortoise-and-hare (`Primitives.cpp` ~11520) and responds to by returning nil —
i.e. telling the image **"no handler exists"**. The runner's `on: Error do:`
therefore never fires, the exception escapes, and the whole package's results
are lost to one corrupted chain. `Primitives.cpp` already carries a comment
calling this "the x86-JIT sender-chain corruption" and a `PHARO_T1_TRACE_HANDLER`
knob that dumps the walked chain — run it on jrpc first; it is a 7-second repro.

#### 2c. Three crashes / silent losses, not JIT-dependent

    apptivegrid-soil            cog 464 P    ours SIGABRT (rc=134) after CLASSES
    pharo-contributions-mutalk  cog 336 P    ours SIGSEGV (rc=139) after CLASSES  (= #3)
    smalltalkweb-myprecious     cog 107 P    ours RUNS (2026-08-13); see #20

`soil` aborting is new information — `rc=134` is `abort()`, so an assertion or
an uncaught C++ exception, distinct from mutalk's segfault. Both reproduce with
`PHARO_NO_JIT=1` (mutalk stops segfaulting but still produces no RESULT).

**2026-08-13: `soil`'s SIGABRT is FIXED (`544740ac`) — it was a SUPER SEND to
a Reflectivity-instrumented method.**  The crash log names it outright:

    [FATAL] pushFrame: method header not SmallInteger!
      cls=11781 fmt=3 slots=2 isCompiledMethod=0
      className=ReflectiveMethod

A method-dictionary lookup may answer a non-CompiledMethod, and the VM
contract is to send it `#run:with:in:`.  Both non-super send paths did that;
all THREE super-send paths activated it directly.  Soil's suite drives
OCASTCache/Reflectivity, so any instrumented method reached through `super`
killed the VM.  Repro with anonymous classes only, in the commit message.

With that fixed, soil RUNS: it reaches `CLASSES 37`, and every failure it
produces is one Cog produces too —

    FAIL SoilCleanCodeTest>>testNoUnusedClasses
    FAIL SoilIndexedDictionaryTest>>testAddRandom
    FAIL SoilIndexedDictionaryTest>>testConcurrentIsEmpty
    FAIL SoilIndexedDictionaryTest>>testRemoveKeyWithTwoTransactions

plus our own TIMEOUTs, all of them in `SoilCleanCodeTest` and all of them
reflective scans, i.e. defect #6.  Given a 3600 s budget the run still does
not reach a RESULT line: `testCodeCoverage`, `testNoUnsentMessages` and
`testNoUnusedClasses` time out at the 80 s watchdog and
`testNoImplementedCalls`/`testNoUnimplementedCalls` was still running when
the budget expired.  So what is left of "soil" is entirely the activation
wall, not a crash — and soil is now the cheapest available repro for #6.

**2026-08-13: `smalltalkweb-myprecious` is no longer "exits 0 after CLASSES"
— it runs, and its failures are defect #20.**  Freshly loaded, single-segment:
ours `pass=90 fail=2 err=16 timeout=1` vs Cog `pass=107 fail=0 err=2`, and 13
of the 16 errors are one class.  That class needs no network at all:

    MpUnconnectedTransportMiddlewareTest suite run   (in-image SharedQueue)
      stock Cog                        32 ran, 32 passed
      ours before the #20 fix          32 ran, 18 passed, 1 F, 13 E
      ours after                       32 ran, 31 passed, 0 F,  1 E

Its transporter's reception service is a `TKTParameterizableService` with
`stepDelay: 0 milliSecond`, i.e. exactly #20's spinner.  The remaining
`MpTcp*`/`MpWeb*` failures are separate and still open.

### 3. `pharo-contributions-mutalk` — the crash is FIXED 2026-08-12 (`57022d3a`); the lost RESULT is not

    before  exit 134 (SIGABRT) after CLASSES 100
    after   exit 0

ROOT CAUSE: `push()` bounds-checks the operand stack
(`stackPointer_ >= stack_.data() + MaxStackDepth`), but once control reaches
compiled code the JIT writes that stack directly through `state.sp` and never
consults it.  A deep enough Smalltalk recursion writes PAST the end of the
`stack_` array and into the members declared after it.

Measured end to end (reproduced locally by loading the package):

    ASan        bad __sanitizer_annotate_contiguous_container in
                std::vector<void*>::__destroy_vector <- ~Interpreter(),
                beg=0xfffffffffffaa879 end=0xfffffffffffaa871 — the SAME
                address this entry recorded from Linux.  They are not
                pointers: they decode as consecutive SmallInteger Oops
                (-43762, -43761), i.e. operand-stack slots.
    the member  `std::vector<void*> stackPushReturnAddr_` at offset 9482648,
                48 bytes past the end of `stack_`.  Its writer is guarded and
                its knob is off, so nothing legitimately touches it.
    watchpoint  fires writing 0x9 (SmallInteger 1) there, frame #0 with NO
                SYMBOL (raw `mov x2, x25` = JIT code), entered from
                tryJITActivation <- activateMethod <- sendSelector.
    culprit     `#recursiveFactorial:` at sp = 130,881 of 131,072 slots.

FIX, at BOTH places compiled code can grow the operand stack:

  * `tryJITActivation` (`57022d3a`) declines when the stack is within
    `StackSafetyZone` (256 Oops) of `MaxStackDepth` and falls back to the
    interpreter, which has the check and raises the proper stack-overflow
    signal.  One compare per JIT activation; a base-image eval declines zero.
  * `pushFrameForJIT` — the J2J path — is a KNOWN REMAINING GAP, not fixed.
    It checks only `frameDepth_ >= StackOverflowLimit`, which cannot help
    (131072 Oops against 56000 frames is barely 2 slots per frame), so a deep
    enough JIT-to-JIT chain can still overrun.

    **CORRECTION.**  `86260b47` added the bound there and `959da174` reverted
    it claiming a 500x blowup on mutalk (5,439,488 sends -> >2.8 billion).
    **That claim was wrong** — the comparison was confounded twice over: a
    full SUnit suite was running concurrently for the second measurement, and
    mutalk's own send count is not stable run to run (the reverted binary
    reproduces the same "blowup" on a quiet machine).  Re-measured properly on
    a stable benchmark, 3 runs each:

        tinyBenchmarks   without bound   468 / 466 / 473  Mbytecodes/sec
                         with bound      432 / 447 / 457  Mbytecodes/sec

    i.e. about 2-3%, not 500x.  It is left OUT for a different and honest
    reason: it costs a few percent in the hottest call path and there is no
    demonstrated case it fixes that the entry guard does not — mutalk is fixed
    by the entry guard alone.  Restore it if a J2J-only overrun is ever
    observed.

Explains `PHARO_NO_JIT=1` exiting 0 in the original triage.

SIZING INVARIANT — FIXED 2026-08-17.  `MaxStackDepth = 131072` carried the
comment "must be large enough for MaxFrameDepth frames" against
`MaxFrameDepth = 65536` — exactly 2 Oops per frame, which only holds for a
zero-temp method with an empty operand stack.  Measured against the limit the
VM actually enforces (`StackOverflowLimit` 56000 + `StackOverflowSignalHeadroom`
8192 = 64192 frames, the ceiling that applies only while a stack-overflow
signal is being raised; 56000 otherwise) it was 2.0 slots per frame, so the
VALUE stack was the
binding constraint for every method with more than a temp or two.  That is the
wrong way round: running out of value stack calls `stopVM()` and kills the whole
VM, running out of frames raises the image's stack-overflow signal and at worst
ends one process, so the frame limit has to trip first.

`MaxStackDepth` is now 2097152 — 32.7 slots per frame at the effective limit,
the same ratio `main` uses.  Cost: `stack_` is value-initialised (Oop has a
user-provided default constructor), so resident goes 1.0 MiB -> 16.0 MiB and
`sizeof(Interpreter)` ~9.5 MB -> ~25 MB, both heap.  Under
`PHARO_TRACE_STACK_ORIGIN` the two parallel vectors sized from `stack_.size()`
go ~2 MiB -> ~32 MiB; that knob is off by default and prints its own MB figure.

This also narrows the J2J gap above without paying for it: at ~32 slots per
frame `frameDepth_ >= effectiveStackOverflowLimit()` in `pushFrameForJIT` is a
sound proxy for operand-stack safety for any frame that is not pathologically
wide, which is why the reference shape needs only the frame check.  The explicit
bound is still the honest fix if a J2J-only overrun is ever observed.

Canaries placed immediately after `stack_` and `savedFrames_` did NOT catch
this (they are kept, and that negative is why): the JIT writes individual
slots at computed offsets rather than sweeping, so it stepped over the canary
word.  That is what pointed at a wild write and then at ASan.

STILL OPEN: the same package produces no `RESULT` line — the eval's own
startup.st IS consumed (the "eval never ran" guard does not fire), so the
script runs and the process dies partway through the tests.  Cog:
pass=336 err=1.  Localize that process death next (a `[TERM-P*]` trace, or
the runner's own error path).

### 4. `fedeloch-ume` — LOADER FIXED 2026-08-12 (`6edf6b33`); package needs a clean retest

    firstSegmentBytes = 75,825,152   of   imageBytes = 149,692,728

**`37eeb743` was right and `38e3c050` retracted it wrongly.**  The retraction
argued the file is exactly `headerSize + imageBytes` with zero bytes past the
declared data, so there is no second segment.  That test does not disprove
segments: Spur writes the heap as one or more segments INSIDE `imageBytes`,
each but the last terminated by a bridge (a double-word free chunk whose slot
count spans the gap to the next segment).  The field that answers it is
`firstSegmentBytes`, and on a freshly loaded Ume image half the heap is in a
second segment.

Full chain, every step measured (`Metacello ... baseline: 'Ume'`, loaded
locally, 142 MB image):

    firstSegmentBytes < imageBytes        segments exist
    forEachObject mis-parses at a bridge  [IMGLOAD-WALK-MISALIGN] at +0x584fcc0;
                                          words 0xff00000000000066 / 0xfffc30 /
                                          0xff000000000078cf — two 0xFF-marked
                                          words 16 bytes apart at a delta-0
                                          object boundary, i.e. a bridge
    the walk stops there                  [IMGLOAD-WALK-TRUNC] 38.14% of the
                                          image never relocated (was a bare
                                          `break`)
    those objects keep saved addresses    [WEAK-BAD-REF] WeakValueAssociation
                                          slot 1 = 0x10000000000
    the GC dereferences one               SIGSEGV in markPhase — lldb:
                                          `ldrb w8, [x24, #6]`, x24 = the old
                                          image base, MarkedBit is bit 55

Two things fixed on the way, both independent of segment support:

  * `processWeaklings` dereferenced weak referents and ephemeron keys without
    the bounds check `markAndTrace` already applies, so ONE bad slot killed
    the VM.  Now goes through `ObjectMemory::isReadableHeapObject` (same bound,
    one name) and nils + reports instead (`3c772997`, `248ceff8`).
  * `loadHeapData` refused a multi-segment image outright (`73565806`) rather
    than relocating half a heap.  SUPERSEDED — segments are now supported, see
    below.

BLAST RADIUS of that refusal, measured rather than asserted — every image on
this box (34 of them, via the new `scripts/image-segments.py`, which reads the
header without booting a VM):

    Ume            142.8 MB   imageBytes 149,692,728  firstSegment 75,825,152   MULTI
    everything else                                                             single

That "everything else" includes a 70.8 MB mutalk package image, the stock
Pharo 13 base image, images our own `saveAs:` produced, and images stock Cog's
`eval --save` produced.  So the refusal fires only on images whose heap grew
past roughly 76 MB in one session — i.e. heavy package loads, which are
exactly the ones that were being silently half-relocated before.  Check any
image with:

    scripts/image-segments.py <image>   # exit 1 if multi-segment

Also settled by measurement, which the previous session flagged as unchecked:
**the `[IMGLOAD-DECLINE]` lines are false positives.**  New per-format
attribution (`[IMGLOAD-DECLINE-BY-FORMAT]`): of 49,831 declined old-base
pointers on this image, all 49,831 are in **format 9** and ZERO in pointer
formats 0-5.  Format 9 is a 64-bit WORD array that `hasPointers` includes for
hiddenRoots' sake, so those are numeric data misread as pointers — lead 2,
confirmed.  `[IMGLOAD-CLAMP]` does not fire on this image either.

SEGMENT SUPPORT IS IMPLEMENTED 2026-08-12 (`6edf6b33`), from the reference VM
(`SIR_readSegmentsFromImageFile`, cointerp-cpp.c:14984).  `readSegments()`
walks the bridge chain, PACKS the segments — dropping each 16-byte bridge, as
the reference does by letting the next segment overwrite it — and records a
per-segment swizzle; `relocatePointer` finds the owning segment and applies
its delta.  A one-segment image takes the identical old path.

    before   mis-parse at a bridge, 38% of the heap unrelocated, SIGSEGV in markPhase
    after    [IMGLOAD-MULTISEG] 6 segments; packed 149692632 from 149692728
             zero WALK-TRUNC / WALK-MISALIGN / WEAK-BAD-REF, no SIGSEGV,
             proper [RESUME] SnapshotOperation chain,
             test_class_table ALL TESTS PASSED on the multi-segment image

ONE ARITHMETIC BUG on the way, caught by `test_class_table` rather than by
re-reading the diff: the SAVED base must advance by the FULL segment size
INCLUDING the bridge, which occupies saved address space even though no object
lives there.  Advancing by the payload put each later segment 16, then 32,
then 48 bytes low — surfacing as exactly "8 invalid pointers in hiddenRoots"
while the 7185-entry class table still matched perfectly.  Keep that test in
the loop for any future change here.

STILL OPEN for the ume PACKAGE itself: the test image was built on a harness
copy that already carried SUnitRunner, so its resume hits a `nil suspend` DNU
from the runner instead of running the package.  A retest from a pristine
Pharo image is what remains; the loader half is done.

### 5. Startup dies on a nil Delay semaphore; the "frozen eval" is a SYMPTOM — CONFIRMED — HIGH

**`#5b` below is FIXED (`e00f0acb`) and its cause was NOT a nil Delay
semaphore** — it was the resumed image's leftover `exitSuccess`.  Read that
entry before trusting the Delay framing in this one.

**FIXED for OSSubprocess images (`9d9f8154`) — but NOT the whole group.**
Measured after the fix: `mumez-pharo-acp` goes from producing nothing to
`pass=161 fail=0 err=9` (Cog: `pass=168 err=2`), while **the plain `saveAs:`
repro on a BASE image still returns the frozen `'FIRST'`**. So my earlier
"eight defects, one root cause" consolidation was WRONG and is withdrawn:

  * packages that load **OSSubprocess** died in its startup handler — fixed by
    implementing `UnixOSProcessPlugin` (see below);
  * the plain `saveAs:` freeze (#5b, base image, no OSSubprocess) is a
    SEPARATE defect and is still open.

MEASURED across the group (2026-08-12, macOS-arm64, after `9d9f8154`):

    package                      OSSVMProcess  cog                        ours
    pillar-markup-pillar         true          pass=18 err=9              pass=18 err=9   FIXED, exact
    mumez-pharo-acp              true          pass=168 err=2             pass=168 err=2  FIXED, EXACT (2026-08-13)
    fedeloch-ume                 true          pass=11                    -               STILL FAILS
    moosetechnology-gitprojecthealth false     pass=6                     -               STILL FAILS
    tomooda-viennatalk           false         pass=1430                  -               STILL FAILS
    evref-bl-mcp                 -             (load failed locally: network, not a VM issue)

**2026-08-13: the 7-test residual is CLOSED (`ca58c987`), and it was not a
startup problem at all.**  All seven were subprocess-spawning tests, and all
seven died at the same place: `OSSUnixSystemAccessor` declares SIX
`UnixOSProcessPlugin` primitives and we had registered TWO, so `primCreatePipe`
answered nil and `OSSPipe>>initializeWith:readBlocking:` sent `#first` to it.
Implementing `primitiveCreatePipe` (answering the Array of two SQFile
ByteArray handles the image expects), plus `primitiveUnixFileNumber`,
`primitiveSQFileSetNonBlocking` and `primitiveSemaIndexFor`, takes acp to
`pass=168 fail=0 err=2` — Cog's numbers exactly, no JIT-only failures.

Never assume a per-package residual is "the long tail"; itemize it.  Seven
tests, one primitive.

So the fix closes TWO of the group outright and leaves three. The
`OSSVMProcess inStartupList` flag predicts the no-OSSubprocess ones correctly
(gitprojecthealth, viennatalk have a different cause), but **`ume` has
OSSubprocess and still fails**, so there is at least a second startup-killing
mechanism beyond the missing plugin. Do not assume one cause for the rest.

#5b (plain `saveAs:` on a BASE image) is characterised and distinct from both:
the clone still answers the frozen `'FIRST'`, and unlike acp there is **no
error or DNU at all** — the startup sequence simply is never invoked. Planting
a `startup.st` by hand confirms it: never runs, left on disk, silently.

**Corrected 2026-08-12 (this supersedes the snapshot/resume framing below).**
The image resumes from its snapshot CORRECTLY. What kills it is downstream:

    [DNU-STACK] #waitTimeoutMilliseconds: in #ifTrue:ifFalse: ip=127 rcv=nil
      fp+0  FullBlockClosure
      fp+1  Delay
      fp+2  nil  <== receiver
      fp+3  SmallInteger

A `Delay`'s semaphore is nil, the send DNUs, that kills the process (P70), and
the startup sequence dies with it — so `StartupPreferencesLoader` never runs
(its script is left on disk, which is the VM's "eval never ran" guard firing)
and the only thing left to report is the previous eval's result. The "frozen
eval" is the visible symptom of a dead startup process, not a snapshot bug.

Proof the startup loader is what dies, not the resume: plant a `startup.st` by
hand and run each image with NO eval argument —

    porp (works)   startup.st RUNS and is consumed
    acp  (frozen)  startup.st NEVER runs, still on disk at exit

and the resume traces of the two images are byte-identical (same 9-frame
snapshot chain, same patches). Stock Cog runs the acp image to
`RESULT pass=170`.

This is the **defect #11 family** (upstream Pharo Delay/watchdog nil-timeout,
`docs/image_issues.md:153-174`), which #11 already notes our harness patches in
`run_sunit_tests.st` and which "interp-only does not wedge, i.e. our scheduler
timing is what surfaces it". Not JIT-dependent — `PHARO_NO_JIT=1` also fails.

SO THE FIX TARGET IS THE DELAY/SCHEDULER PATH, NOT SNAPSHOT/RESUME. That is
much safer ground: a wrong change there fails loudly rather than corrupting
saved images. Find why the semaphore is nil at that moment under our scheduler
when it is not under Cog's — a process terminated mid-`schedule` leaving the
Delay half-initialised is the leading candidate (the same run shows a
`terminateRealActive` trace).

Fixing this should close #5 AND the seven packages in #2a AND likely #11.

---

#### Superseded framing, kept for the evidence trail

### 5b. ~~`Smalltalk saveAs:` freezes the eval result~~ — FIXED 2026-08-12 (`e00f0acb`)

    step 1  eval "Smalltalk saveAs: 'clone'. 'FIRST'"
    step 2  run clone.image with eval "'SECOND-', 3 factorial printString"

    Cog      'SECOND-6'
    ours     'FIRST'                  <- the new command never ran at all
    now      'FIRST' then 'SECOND-6'

The snapshot/resume framing was wrong, and so was "the startup sequence is
simply never invoked".  Measured on the resumed clone, every step runs and
runs correctly:

    SnapshotOperation isImageStarting  true   (the resume patch works)
    hasError                           false
    WorkingSession>>runStartup: true   entered — extent byte-identical
                                       (11237 traced bytecodes) to a fresh boot
    BasicCommandLineHandler class>>startUp: true   adds its deferred action
    executeDeferredStartupActions:     n=2, runs it
    BasicCommandLineHandler>>activate  entered, no error

and then nothing, because `activate` **forks**:

    [ ... self handleArgument: self firstArgument ... ]
        forkAt: Processor userSchedulingPriority named: 'CommandLine handler process'

so the dispatch that reaches `PharoCommandLineHandler>>activate` ->
`runPreferences` -> `StartupPreferencesLoader` -> our `startup.st` is a
fresh P40 process.  The continuation the image resumed INTO is the
PREVIOUS command's script, boosted to P80 by the headless startup boost,
and it ends with `Smalltalk exitSuccess` — the VM quits before the P40
process ever gets the CPU.  Proof that CPU is the only thing missing: put
an 8 s `Delay` in the old continuation and the UNFIXED VM answers
'SECOND-6'.

FIX: our generated `startup.st` deletes itself as its first statement, so a
quit raised while THIS run's script is still on disk cannot be ours.
`primitiveQuit` honours "that command is over" by terminating the process
that asked and lets the scheduler reach the queued handler.  One-shot, so
an image that never consumes the script still exits and the "eval never
ran" guard still reports.

NOT verified: whether this also closes the four `#2a` packages that "exit 0
in seconds having done nothing" (`evref-bl-mcp`, `moosetechnology-famix`,
`tomooda-viennatalk`, `moosetechnology-gitprojecthealth`).  They carry
exactly this signature and are stock-Cog `eval --save` images, but they
need a package load to test.

### 6. Activation wall — reflective scans TIMEOUT at 80 s — MEDIUM

Base suite: `testNoUnusedInstanceVariablesLeft`, `testNoUnusedTemporaryVariablesLeft`,
`testNoShadowedVariablesInMethods`, `testUsingMethods`. Packages: famixreplication
(43->39, 4 new timeouts), p3, polymath, lexicon, deeptraverser — all pass->timeout
deltas against Cog. The `refersToLiteral:`/`scanFor:` primitives (`aad03bc0`)
did not close it; `scripts/perf-activation/README.md` lists four untried
ablations. The SUnit runner carries an image-side memoize workaround for the
same wall.

Still open and unchanged on the rebuilt host — the 2026-09-02 arm64 sweep hit
it three times, and every one of the five TIMEOUTs in that run except the two
network tests belongs to this defect:

    NoUnusedVariablesLeftTest>>testNoUnusedTemporaryVariablesLeft   80 s
    ReleaseTest>>testNoShadowedVariablesInMethods                   80 s
    ReleaseTest>>testNoNullCharacter                                80 s

The first two are named in the list above; `testNoNullCharacter` is a new
member of the family (a whole-image source scan, same shape).

**Now with the other half of the ratio (2026-09-02).**  The same classes on
stock Cog v10.3.9, same image, headless:

    NoUnusedVariablesLeftTest     3 tests   Cog  12.0 s   ours killed at 80 s
    ReleaseTest (whole class)    43 tests   Cog  24.5 s   ours 2 killed at 80 s
    MCSmalltalkhubRepositoryTest  1 test    Cog   1.6 s   ours killed at 80 s

So the wall is at least 6.7x on the cheapest of them and at least 50x on
`MCSmalltalkhubRepositoryTest`, and those are lower bounds twice over: the 80 s
is a kill, not a completion, and this Cog is x86_64 under Rosetta against a
native arm64 build of ours.  Raw in
`docs/results/sweep-arm-2026-09-02/cog-residual-baseline.txt`.

### 7. `rko281-restoreforpharo` ~50x slower than Cog on live SQLite — MEDIUM

    cog   2354 tests  145 s
    ours  unfinished in a 7200 s budget

Invisible until `4a46413f` made SQLite3 resolve — before that all 4712 tests
errored instantly. Three JIT-only failures in `SSWReStoreDependent*DictionaryTest`
appeared in the part that ran. Needs an aarch64 Linux box with libsqlite3.

### 8. Stepping family hangs on native x86_64 Linux — MEDIUM

`StepOverTest`/`StepIntoTest` hit the runner's consecutive-timeout bail-out
(`docs/x86-test-run-2026-08-10.md:194-207`) while ARM runs 28/28 clean. The
per-arch-backend-drift shape that has already produced three bugs.

### 9. Windows RUN #7 residual: 50 F / 377 E / 25 T, never itemized — MEDIUM

97.8% against a 99.1% Linux baseline, and the Cairo bucket that would have
absorbed ~150 of them was fixed the same day. Exists only as a number in a
paragraph. See `docs/history/windows-port-2026-06-27.md`.

### 10. Windows old-space commits the whole ~4 GB reservation up front — MEDIUM

`src/platform/win_mman.h` uses `MEM_RESERVE|MEM_COMMIT` on the full mapping,
so startup fails outright on a machine with under ~4 GB of commit headroom. The
2026-07-04 design note (commit-ahead in the allocation slow path) is sound and
unimplemented — no `committedEnd_` symbol exists.

SPECIFIED 2026-08-12 by reading the chain, so this stops being a note:

  * The caller is `ObjectMemory::initialize` (`src/vm/ObjectMemory.cpp`,
    the old-space `mmap`), and it reserves the whole ceiling ON THE EXPLICIT
    PREMISE, stated in its own comment, that "mmap(MAP_ANONYMOUS) reserves
    address space without committing pages ... a 4 GB reservation on an
    iPhone 8 with 1 GB of physical RAM costs ~0 bytes".  True on Linux/macOS.
  * The Windows shim silently violates that premise.  So the defect is a
    CONTRACT MISMATCH between a documented call site and a platform shim, not
    a tuning problem — which is why it is invisible in review.  A warning
    block now sits at the shim so the next reader cannot miss it.
  * Reserve-only is NOT a drop-in fix: Windows does not auto-commit on touch,
    so the VM would fault on the first write past the committed region.  The
    two viable designs are (a) commit-ahead in the allocation slow path — grow
    a `committedEnd_` watermark in page-sized chunks as the bump pointer
    advances — or (b) a vectored exception handler that commits on fault.
  * **A mitigation already exists and needs no new code**:
    `PHARO_MAX_OLD_SPACE_MB=512` (or whatever the box can commit) lowers the
    reservation, and therefore the Windows commit, at the call site in
    `test_load_image.cpp`.  Worth trying first on any Windows box that cannot
    start.

Deliberately NOT implemented here: this is Windows-only and cannot be
exercised on this machine, and the repo's own lesson from `4a46413f` is that
"fixed by construction, not verified on the platform" was worth exactly
nothing.

### 11. Upstream Pharo Delay/watchdog nil-timeout trio, patched only in our harness — MEDIUM

`docs/image_issues.md:153-174`. The fix lives in `run_sunit_tests.st`, so a
stock unpatched image under our VM can still poison the DelayMicrosecondTicker.
Interpreter-only does not wedge, i.e. our scheduler timing is what surfaces it.

### 12. SHA256 11/12 — one FIPS vector fails on Windows — LOW

Which vector is not recorded. A failing published test vector on a hash
function should not be invisible; it was a single line inside a checked-off
Crypto entry.

MEASURED 2026-08-12, macOS-arm64: `SHA256Test suite run` -> **12 ran, 12
passed, 0 failures, 0 errors**, and `SHA1Test` is present alongside it.  So
the class is SHA256Test, the count is 12, and the failure is Windows-specific
(or stale) — it does not reproduce on arm.  Closing it needs a Windows run;
there is nothing to chase here.

### 13. ~~`StDebuggerActionModelTest>>testEventAfterProceed:`~~ — CLOSED 2026-08-12

Stale. Two corrections: the selector is `testEventAfterProceed` (no colon — the
original entry's name does not exist), and it **PASSES** on arm at HEAD when run
in isolation. Corroborated by both recent full suites (macOS-arm64 and
Linux-aarch64), where it appears in neither FAIL list. Nothing to fix.

### 14. `MicFileResourceReferenceTest` BitBlt "Fraction" bug on Windows — LOW

Passes on ARM. Needs a Windows run to close.

### 15. ~~Recursion deeper than ~56,000 frames HANGS the VM~~ — the hang and the uncatchability are FIXED; only the depth gap remains — LOW (was HIGH; re-measured 2026-08-17)

**Both dangerous halves are closed.** Re-measured on HEAD, same repro as below:

    N        2026-08-12                  2026-08-17
    10000    10000  (2 s)                10000  (2 ms)
    40000    40000  (1 s)                40000  (2 ms)
    56000    56000  (2 s)                56000  (3 ms)
    60000    HANGS - killed at 60 s      CAUGHT Error  (27 ms)
    100000   HANGS - killed at 180 s     CAUGHT Error  (11 ms)
    200000   (not reached)               CAUGHT Error  (28 ms)

The whole six-point sweep now runs in 4 s and exits 0. The error the image
catches is the VM's own: `'stack overflow: recursion deeper than 56000 frames'`,
so `on: Error do:` works where the entry below says it could not.

**And the unwind property the 2026-08-15 revert was protecting is intact.**
That revert rested on a mutex counter-test; re-run on HEAD it passes:

    fork caught overflow: yes   mutex: MUTEX-FREE

Note the counter-test as originally written cannot answer the question, and this
is worth keeping because it is the same trap #21 retracted: with no handler
inside the fork, the unhandled overflow makes the image quit, the eval never
reaches its last expression, and "no EVAL-RESULT" gets read as "leaked". The run
exits 0 after 4 s with a 25 s Delay still in the script -- an early quit, not a
hang. Handling the error inside the fork restores the discriminator.

So `handleStackOverflow` on HEAD signals a catchable `#error:` AND unwinds. The
"RETRIED AND REVERTED 2026-08-15" note below is stale: a third attempt landed
and holds. Its reasoning is kept because the counter-test it introduced is the
right test, and because it records why the naive form failed.

**What is left is the depth gap, and it is a capability difference rather than a
defect:** Cog returns 100000 because its stack is pages that spill to the heap as
contexts; ours raises a clean, catchable, unwinding error at 56000. Nothing is
lost or corrupted at the cap. Do NOT close this by raising the constants -- the
costing below still stands, and today's value-stack change already spent
+15.7 MB of permanently resident memory on an iOS-targeted VM. The real fix is
still heap-spilled contexts, still a project.

Re-priced 2026-08-17 for whoever takes it on. MaxStackDepth is now 2097152 and
StackOverflowLimit 56000, i.e. 37.4 value-stack slots per frame. Matching Cog at
100000 frames needs MaxFrameDepth ~108000 (savedFrames_ 7.5 -> 13 MB) and
MaxStackDepth ~3.2 M to hold the ratio (16.8 -> 25.6 MB): about +14 MB against a
measured 293 MB baseline for `eval "42"`.

Still worth testing, unchanged from below and now unblocked by the hang fix: the
four "hangs to 1800 s" packages in #2a/#2b.

### 15-original. Recursion deeper than ~56,000 frames HANGS the VM; Cog does it instantly (found 2026-08-12, superseded above)

Three lines, same image, apples-to-apples (no Morphic, so the `eval`-preamble
caveat does not apply):

    Object compile: 'deepRec: n
      ^ n = 0 ifTrue: [0] ifFalse: [ (self deepRec: n - 1) + 1 ]'.
    [ (Object new deepRec: N) printString ] on: Error do: [:e | 'CAUGHT ', e class name ]

    N        ours                         Cog
    10000    10000  (2 s)                 10000
    40000    40000  (1 s)                 40000
    56000    56000  (2 s)                 56000
    60000    HANGS — killed at 60 s       60000
    100000   HANGS — killed at 180 s      100000 in 0.10 s

The cliff is exactly `StackOverflowLimit = 56000`.  What happens there:

    [OVERFLOW] fd=56024 pushing #deepRec: (argCount=1)
    [OVERFLOW] driving Process>>terminate to unwind (fd=56024) instead of
               hard-kill — releases held critical: mutexes

and then the VM **idles**: the traces show it transferring between P80 and P10
forever (`[XFER-n] pri=80 -> pri=10`), i.e. the eval's process is gone and
nothing is left to do, but the VM does not exit.  So the observable behaviour
is "runs until the harness kills it", which is exactly what a hung package
looks like from outside — and why four of these were filed as hangs.  Two
distinct problems:

  1. **The limit.**  Cog has no trouble at 100,000 frames — its stack grows.
     Ours caps at 56,000 and treats exceeding it as fatal.
  2. **The handling hangs.**  Even granting the cap, the overflow path is
     supposed to unwind and end the process; instead the VM stops making
     progress.  And because the recovery is `Process>>terminate` rather than
     an Error, the image's own `on: Error do:` CANNOT catch it — the handler
     above never fires, where on Cog there is nothing to catch.

**NOT `#3`** — checked rather than assumed: the mutalk runs contain ZERO
`[OVERFLOW]` lines, so its missing `RESULT` is not this path, despite its
culprit method being `#recursiveFactorial:`.  What mutalk hits is the OPERAND
stack (131072 slots) filling while the frame count stays well under 56000 —
a different limit.  That inference is withdrawn.

STILL WORTH TESTING once this is fixed (untested — needs package loads): the
"hangs to 1800 s" entries in `#2a`/`#2b` — `moosetechnology-famix`,
`tomooda-viennatalk`, `moosetechnology-gitprojecthealth`, and `j-brant-smacc`
(a recursive-descent parser, so the best candidate of the four).

Found while measuring something else: the deep-recursion probe was written to
test whether a JIT stack bound livelocked, and BOTH arms hung — which is what
exposed that the hang is pre-existing and not about the JIT at all.

DON'T just raise the constants — costed 2026-08-12, it is not the cheap fix
it looks like.  Both bounding arrays are INLINE members of the single
Interpreter object, so their size is always resident:

    sizeof(SavedFrame) ~ 120 bytes
    savedFrames_   65,536 frames = 7.5 MB   ->  200,000 = 22.9 MB
    stack_        131,072 Oops   = 1.0 MB   ->  600,000 =  4.6 MB

i.e. reaching Cog-like depths costs roughly +19 MB of permanently resident
memory, on a VM whose primary target is iOS.  Cog does not pay that: its stack
is PAGES that spill to the heap as contexts, so depth is bounded by available
memory rather than by a fixed array.  That — not a bigger constant — is the
real fix, and it is a project.

TWO cheaper intermediates, in order of confidence:

  * ~~**Make the VM exit instead of idling.**~~  DONE 2026-08-12.  The repro
    now exits in ~62 s with `[OVERFLOW] driving Process>>terminate` followed by
    "the command's process is gone", instead of running until the harness
    kills it.  Counter-tests: a legitimate `(Delay forSeconds: 45) wait` still
    returns normally (46.6 s), and an ordinary eval is untouched.

    NOTE the condition that does NOT work, so it is not retried: "no armed
    timer and no pending signals" evaluated in the idle loop NEVER FIRES — a
    live image always has a Delay timer armed, measured.  The deadline is
    armed in `handleStackOverflow` instead, i.e. on the actual event, which is
    also why it cannot touch a run that never overflowed.

  * The original framing of this half, kept for the reasoning:
    After `handleStackOverflow` terminates the process, the image still has an
    idle process and the timer process, so `tryReschedule()` succeeds and the
    existing "No runnable processes" `stopVM` never fires.  A precise
    condition exists: in eval mode, the generated `startup.st` always ends in
    `Smalltalk exitSuccess`, so **script consumed + no quit ever arrives + the
    VM is in `primitiveRelinquishProcessor` with no armed timer and no pending
    external signals** means the command died and nothing can wake anything.
    NOT implemented here deliberately — a wall-clock idle heuristic would kill
    a legitimate `(Delay forSeconds: 300) wait`, so the no-armed-timer part of
    the condition is load-bearing and wants writing carefully.
  * **Make the cap catchable.**  Note the previous session already tried and
    rejected the naive form: the comment in `handleStackOverflow` records that
    signalling `#error:` "doesn't suffice: it goes unhandled in a forked test
    and our unhandled-error path skips unwinds too", which is why it drives
    `Process>>terminate` (that release of held `critical:` mutexes is itself a
    root-cause fix — do not undo it).  Any catchable-error attempt has to keep
    the unwind.

    RETRIED AND REVERTED 2026-08-15, with numbers this time, so the third
    attempt does not have to rediscover them.  The change was: capture the
    failed send's receiver before `popN`, then `push(rcvr); push(msg);
    sendSelector(#error:, 1)` instead of driving `#terminate`, plus clearing
    `inStackOverflowSignal_` once the stack unwinds clear of the cap (needed
    because a *caught* overflow no longer forces the process switch that used
    to clear the flag).

    The catchable half WORKS — this is not why it was reverted:

        [ (Object new deepRec: 100000) printString ] on: Error do: [:e | ... ]
        ->  EVAL-RESULT='CAUGHT Error: stack overflow: recursion deeper
                          than 56000 frames'

    It was reverted because the unwind property is genuinely lost.  The
    counter-test — an overflow with NO handler, inside a held mutex:

        | m r |
        Object compile: 'deepRec: n
            ^ n = 0 ifTrue: [0] ifFalse: [ (self deepRec: n - 1) + 1 ]'.
        m := Mutex new.
        [ m critical: [ Object new deepRec: 60000 ] ] fork.
        (Delay forSeconds: 25) wait.
        r := [ m critical: [ 'MUTEX-FREE' ] ]
                valueWithin: (Duration seconds: 20) onTimeout: [ 'MUTEX-LEAKED' ].
        r

        terminate path (HEAD)   EVAL-RESULT='MUTEX-FREE'
        #error: path            no EVAL-RESULT at all — the eval never
                                finishes.  Not even 'MUTEX-LEAKED': the leak
                                takes the Delay scheduler down with it, so
                                `valueWithin:` cannot fire its own timeout.

    So the ordering is: the unhandled-error path must learn to unwind FIRST,
    and only then can the cap be made catchable.  A related measurement while
    isolating this, which is the smaller bug to attack next and reproduces on
    HEAD with no VM change at all:

        | flag |
        flag := false.
        [[ Object new zorkNotUnderstood ] ensure: [ flag := true ]] fork.
        (Delay forSeconds: 8) wait.
        'ensure-ran=', flag printString

    never prints an EVAL-RESULT either — an unhandled DNU in a forked process
    is already enough to stop the eval from completing, with no stack overflow
    involved.  That is the actual root cause behind "our unhandled-error path
    skips unwinds", and it is worth fixing on its own terms.

    CORRECTION, same day: the paragraph above and the revert it justifies both
    rest on a discriminator that turned out to be wrong — see #21, retracted.
    "No EVAL-RESULT" does not mean the eval stopped; it is also what a correct
    quit looks like, and an unhandled error is SUPPOSED to make this image log
    and exit.  So the `#error:` arm's "no result" in the mutex counter-test is
    not evidence of a leaked mutex.  What it most likely shows is the image
    logging the unhandled stack-overflow Error and quitting — which is what
    stock Pharo does with any unhandled error, and is arguably MORE faithful
    than the terminate path, where the process dies silently and the eval
    carries on.

    RERUN with that discriminator — markers written to a file as they happen,
    plus elapsed time and exit status:

        terminate (HEAD)  48 s, rc=0
                          1-before-fork  2-delay-returned  3-verdict=MUTEX-FREE
        #error:           25 s, rc=0
                          1-before-fork          <- and nothing after

    Neither run timed out.  The `#error:` arm does not leak the mutex and does
    not wedge: the eval simply never reaches the second marker, because the
    image logged the unhandled Error and QUIT at that moment.  **The stated
    reason for the revert is refuted.**

    What the change actually does, stated correctly:

        handled     `on: Error do:` now catches the cap
                    ('CAUGHT Error: stack overflow ...'), where terminate is
                    uncatchable and the handler never runs
        unhandled   image logs and exits — what stock Pharo does with ANY
                    unhandled error, rather than a process silently vanishing
                    while the run carries on

    That is defensible on both arms, and the mutex property is not lost.  The
    open question is no longer correctness but BLAST RADIUS under the test
    harness: an overflow inside SUnit should be caught by the framework and
    recorded as an error (an improvement — today it vanishes), but one raised
    outside any handler would now end the image mid-batch where before it cost
    a single process.  That is exactly what a full-suite run answers, and it is
    the gate the change has to pass before it goes back in.

    GATE RUN — PASSED, and LANDED 2026-08-15.  Built on a side branch, full
    896-class suite:

        Pass 17956   Fail 2   Error 1   Skip 75   899 sections   BATCH COMPLETE

    Identical to the baseline on every counter, so no regression.  The handled
    arm is verified directly: `EVAL-RESULT='CAUGHT Error'` for a 100 000-deep
    recursion wrapped in `on: Error do:`, where HEAD's terminate never ran the
    handler at all.

    READ THE GATE NARROWLY, because it is weaker than it looks: the run logged
    ZERO `[OVERFLOW]` events.  No test in the suite recurses past 56 000 frames,
    so the new path was never taken during it.  The gate therefore establishes
    "changes nothing that was already working" and NOT "the unhandled arm is
    safe under the harness" — the blast-radius question above is still formally
    unanswered, because nothing in the suite triggers it.  Answering it properly
    needs a test that deliberately overflows OUTSIDE any handler while a batch
    is running; that is the follow-up, and it is cheap to write.

    FOLLOW-UP RUN — the blast-radius question is now ANSWERED, and the answer is
    good.  A TestCase with two methods, one recursing 100 000 deep with no
    handler anywhere and one trivial assertion, run under SUnit:

        runs=2   passed=1   errors=1        [OVERFLOW] signalled: 1

    The overflow fires, SUnit catches it as an ordinary Error and records a test
    ERROR, the image SURVIVES, and the following test still runs and passes.
    The eval completes normally.  So the feared failure mode — an unhandled
    overflow ending the image mid-batch — does not occur under the harness,
    because SUnit is itself a handler.  What used to happen instead was the
    process vanishing silently while the batch carried on with no record of it.

    That closes #15's remaining half.  The cap is catchable, it is reported
    where a test causes it, and it regresses nothing.

### 16. ~~Loading a Cog-written image CORRUPTS 64-bit word arrays~~ — FIXED 2026-08-12 (`3c494b65`)

Filed late: the fix commit and three source comments referenced "#16" before
this entry existed.  That is the exact failure mode this file was created to
stop, so it is recorded here rather than quietly renumbered.

    DoubleWordArray written by stock Cog, read back:

    element  written          Cog reads        ours (before)
    1        16r10000000000   16r10000000000   16r300000000   <- MANGLED
    2        16r10008F709A8   16r10008F709A8   16r10008F709A8
    3        16r300000010     16r300000010     16r300000010

`ImageLoader::relocatePointers` used `hasPointers = (format <= 5) ||
(format == 9)`.  Format 9 is a 64-bit WORD ARRAY and is pointer-bearing for
exactly ONE object — `hiddenRootsObj`, which holds the class-table pages.  The
`|| format == 9` was a documented hack for it.  Every OTHER format-9 object is
user DATA, and each element landing in `[oldBase, oldBase + imageBytes)` was
relocated, i.e. silently rewritten.  On a 59 MB image that window is
`16r10000000000..16r10384xxxx` — ordinary values for microsecond timestamps,
packed bitfields, hashes or large ids.

WHY IT HID: `ImageWriter::writeHeapData` had the SAME rule, so our writer
tag-converted the data on the way out and our loader converted it back.  Our
own images round-tripped perfectly and the pair was invisible; it only shows
against a VM that does not share the bug.  Both sides now special-case the
OBJECT instead of the format — the loader locates hiddenRootsObj by position
(`findHiddenRootsHeader`), the writer compares against
`memory.hiddenRootsObj()`.

Verified: Cog-written array exact; our round-trip unchanged; `test_class_table`
passes on the base image and on a freshly saved one; `allClasses` (10632),
`SHA256Test` 12/12 and `garbageCollect` all identical to stock Cog.  The 74,998
`[IMGLOAD-DECLINE]` lines a healthy image used to print are gone, because
format-9 data is no longer offered to `relocatePointer` at all.

Related and STILL OPEN: `freeListsObj` is also format 9.  Its slots are read
only by a gated diagnostic, nothing functional, so leaving them as data is
correct — but a future change that starts reading them must relocate it too.

### 17. ~~`char32AtOffset:` truncated fields >= 2^30~~ — FIXED 2026-08-12 (`b999d9d9`)

Also filed late, same reason as #16.

`Oop::asCharacter` masked with `0x3FFFFFFF` and `Oop::fromCharacter`'s range
assert is compiled out under NDEBUG, so primitive 612 read a 32-bit field back
TRUNCATED — `16r40000000` answered 0 — with no failure anywhere.  That ceiling
was OURS alone: the tagged encoding is `(codepoint << 3) | tag` in a 64-bit
word, 61 payload bits.

    char32AtOffset:   ours (before)   Cog           ours (now)
    16r40000000       0               1073741824    1073741824
    16rFFFFFFFF       0               4294967295    4294967295

TWO WRONG FIXES FIRST, both caught by measurement:

  1. Failing the primitive for `>= 2^30`.  Cog SUCCEEDS there, so that traded
     truncation for a divergence.
  2. Widening primitive 170 (`Character value:`) to match.  The IMAGE PINS that
     one — `Character class>>maxVal` is `(2 ** 30) - 1` and
     `CharacterTest>>testMaxVal` asserts `maxVal + 1` raises PrimitiveFailed.
     Widening it turned CharacterTest 19/19 into 18/19.

Cog is deliberately inconsistent — prim 170 refuses `>= 2^30` while prim 612
hands back a full 32-bit Character — so matching it means doing both.  A note
at prim 170 says not to "fix" it again.  Verified against Cog on
CharacterTest 19/19, StringTest 438/438, SymbolTest 268/268,
WideStringTest 19/19, ByteArrayTest 12/12.

### 18. ~~Auto-triggered GC never fired ephemerons or nilled weak refs~~ — FIXED 2026-08-13 (`4d6bf314`)

Filed late, and for the reason this file exists: it was recorded in
`docs/changes.md` as "**## Improvement:** Auto-compact GC skips ephemeron
firing", i.e. as a deliberate emulation of scavenge semantics.  It was a
measured divergence from stock Cog.

All four auto-GC call sites passed `skipEphemerons=true`, and `markPhase`
gates BOTH `fireAllEphemerons()` and `processWeaklings()` on
`!skipEphemerons` — and both `pendingFinalizationSignals_++` sites are
inside that skipped branch.  So an image that never calls `Smalltalk
garbageCollect` explicitly never nilled a weak reference and never ran a
single finalizer, however much old-space garbage it churned: file
descriptors, sockets and FFI handles leak for the life of the session.

Repro — one weak slot, then allocation pressure only, no explicit GC:

    | wa n | wa := WeakArray new: 1. [wa at: 1 put: Object new] value.
    n := 0. [(wa at: 1) isNil or: [n > 200]] whileFalse: [n := n + 1.
      Array new: 1000000].

    stock Cog      n=2    slot=NIL
    ours (before)  n=201  slot=STILL   (NIL only after an explicit GC)
    ours (after)   n=83   slot=NIL

The recorded rationale — "a real generational GC scavenge wouldn't fire
old-space ephemerons" — misread `needsCompactGC_`: that flag is a MAJOR
old-space compaction, not a minor scavenge, and Cog's major GC does full
weak/ephemeron processing.  The motivating test
(`WeakKeyDictionaryTest>>testClearing`) is unaffected either way — 6 weak
classes 718 P / 0 F / 0 E identical with `PHARO_GC_AUTO_SKIP_EPH=1`.

### 19. Glyph drawing is 12-25x slower than Cog; two FreeTypeCacheTests are watchdog-killed — MEDIUM

`FreeTypeCacheTest>>testGlyphAccessIsThreadSafe` is killed at the 80 s
watchdog on **both** current full suites (macOS-arm64 and Linux-aarch64)
while stock Cog passes it in under 2 s on the same image.  It has only
ever appeared as a bare number in the TIMEOUT column of
`docs/test-results.md`, never as a filed defect.

Isolated on this box, same image, same build:

    workload                                     ours     Cog     ratio
    Form blit, 2000x 200x200 -> 1000x1000          27 ms    24 ms   1.1x
    drawString:, 200x 22 glyphs                   129 ms    11 ms    12x
    full RubScrolledTextMorph render of 25 KB    1586 ms    63 ms    25x
    Semaphore>>criticalReleasingOnError:, 200k    355 ms     7 ms    50x
      of which  ensure:                            54 ms     ~0
                on:do:                             26 ms     ~0
                signal+wait                        14 ms      1 ms

So BitBlt itself is at parity — the gap is entirely in the per-send and
per-block overhead that the glyph path multiplies by thousands of glyphs.
The test renders 25 KB of text into three 1000x1000 canvases ten times;
at 25x that is ~48 s of render plus fork contention, hence the 80 s kill.

Structural cause, from `sample`: `Interpreter::materializeFrameStack()` is
called from `createFullBlockWithLiteral` **whenever `frameDepth_ > 0`**, so
creating any closure converts the entire native frame stack into heap
Contexts and resets `frameDepth_` to 0.  `criticalReleasingOnError:` builds
three closures over shared outer temps per call, so each call materializes
the chain and allocates a temp vector; the sample is dominated by
`scavenge`, `storePointer` and `materializeFrameStack`.  Cog does not pay
this because its frames are *married* to lazily-created contexts.

The fix is frame/context marriage (or at least materializing only the home
frame), which is the same root as the 32x `value:`/`do:` gap in memory
`block-invocation-perf.md`.  Not a one-line change; filed so it stops
hiding in a timeout column.

### 20. ~~A process spinning on a ZERO-ms Delay starves an equal-priority peer~~ — FIXED 2026-08-13 (`preempt starvation guard`)

Seven lines, no packages, no network, no JIT (`PHARO_NO_JIT=1` identical):

    | done count |
    done := false. count := 0.
    [ [done] whileFalse: [ count := count + 1. (Delay forMilliseconds: 0) wait ] ]
        forkAt: Processor userSchedulingPriority.
    (Delay forMilliseconds: 500) wait.
    done := true.
    Transcript show: 'ZS-WOKE count=', count printString; cr.

    stock Cog   ZS-WOKE count=1109651     every run
    ours        NEVER WOKE                3 runs in 6; the other 3 print a
                                          comparable count.  Same-image,
                                          same-binary, load-dependent.

The main process's 500 ms Delay never fires while a same-priority process
loops on a zero-duration Delay.  Matrix on our VM (spinner delay / spinner
priority, main always P40):

    0 ms  P40   NEVER WOKE          <- the bug
    0 ms  P50   NEVER WOKE
    0 ms  P30   woke, 53361 spins
    1 ms  P40   woke, 41 spins      (and 41 spins per 500 ms means our 1 ms
    2 ms  P40   woke, 39 spins       Delay actually costs ~12 ms — separate)

So it is specific to a ZERO-duration Delay at >= the waiter's priority.  A
zero-Delay in a SINGLE process is fine on both VMs (5 in a row, interleaved
with 100-200 ms delays, identical output), so it needs the second process.

**Where it bites:** `TKTService` — Pharo's TaskIt service loop — is
`self stepService. currentDelay := stepDelay asDelay. currentDelay wait`, so
ANY service configured `stepDelay: 0` is exactly this spinner, at P40 by
default.  That is `smalltalkweb-myprecious`'s whole failing set:

    MpUnconnectedTransportMiddlewareTest suite run
      stock Cog   32 ran, 32 passed
      ours        32 ran, 18 passed, 1 failure, 13 errors
                  (MpRemoteMessageResultTimeout, JIT and NO_JIT alike)

and those tests use an in-image `SharedQueue`, no sockets at all — the
transporter's reception service is `TKTParameterizableService ... stepDelay:
0 milliSecond`.  A bare probe of that service on our VM does not even return
from `svc stop`; with `stepDelay: 10 milliSecond` it steps 38 times in 500 ms
and stops cleanly.  Cog steps it 578761 times in 1 s.

Ruled out: `Processor yield` fairness is fine (a P40 loop calling
`Processor yield` lets the P40 waiter wake, 3.2M yields vs Cog's 49.8M).
`PHARO_RR_SCHED=1` does NOT fix it.  Note our scheduler has NO same-priority
rotation by default (deliberate — Cog does not time-slice within a priority)
and headless aging starts at P41, so priority 40 has no route at all once a
peer refuses to suspend.

**ROOT CAUSE: `putToSleepPreempted` front-appends.**  A process preempted by
a higher-priority wake goes to the FRONT of its run queue, so it resumes
first.  A process preempted CONSTANTLY — the zero-Delay spinner is preempted
by the P80 Delay scheduler on every single iteration — therefore keeps
PERMANENT residency at the head, and a peer woken behind it never runs.
`Processor yield` fairness is irrelevant because the spinner never yields:
its `delaySemaphore wait` finds the semaphore already signalled.

The obvious fix — always back-append, which is what `Smalltalk vm parameterAt:
48` bit 2 says BOTH VMs do — is wrong for us, and the June comment was right
about that even though its stated reason was not.  Measured on 12
scheduler/weak classes:

    always front-append (old)   621 P / 1 F / 0 E
    always back-append          618 P / 3 F / 1 E   ProcessTerminateBugTest>>
                                                    testTerminationDuringNestedUnwindS2
                                                    SemaphoreTest>>testSimpleCommunication
                                                    WeakKeyDictionaryTest>>testClearing
    STARVATION GUARD (new)      619 P / 1 F / 1 E   only TKTWorkerTest, which is
                                                    equally flaky with the guard
                                                    OFF (3 runs each: 1E/1T/1T vs
                                                    1T/1E/1E+1T) — pre-existing

We preempt far more often than Cog does (aging, heartbeat force-yield, timer
granularity), so unconditional back-appending round-robins a priority level in
a way Cog never would.  The guard keeps front-append, but if the SAME process
is preempted 50 times in a row while a peer is queued behind it, it yields
once.  That bounds a peer's wait instead of leaving it infinite, and leaves
Cog's no-time-slice guarantee intact for anything not being preempted in a
tight loop.

    repro, 5 runs   guard on: woke every time (~81 000 spins)
                    guard off (PHARO_PREEMPT_NO_YIELD=1): 2 of 3 wedge

**CORRECTION 2026-08-17 — the guard was treating a symptom.**  The premise
above ("the obvious fix, always back-append, is wrong for us") set up a false
choice between always-front and always-back, and missed that Cog does neither
unconditionally: it back-appends at exactly three sites, and only when the
image asks.  Cog reads `preemptionYields` from the image header
(`src/ios/cointerp-cpp.c:14317`, `(headerFlags bitAnd: 16) = 0`) and passes it
to `putToSleep:yieldingIf:` from `resume:preemptedYieldingIf:from:`, whose only
callers are CSSignal, CSResume and CSExitCriticalSection.  Every stock Pharo
image ships flags `0x2`, so bit 16 is clear and Cog runs it with
`preemptionYields` TRUE.  We never parsed the bit at all, so priority 40 had no
rotation route whatsoever — which is what this entry is really describing.
Worse, `vmParameterAt: 48` advertised the policy we did not implement.

That is now fixed: `putToSleepPreemptedYieldingIf` honours the flag at those
three sites and nowhere else.  The four remaining `putToSleepPreempted` callers
(heartbeat scan, JIT step yield, callback-return requeue) have no Cog
counterpart and keep the order-preserving front-append — back-appending the
callback-return requeue puts the yanked process behind its own resumer in
`interpriorityYield:`'s `[p resume] fork` / `p suspend` window and stalls the
FFI callback suite outright.  That is the real reason blanket back-appending
failed, and it is not what the table above measured.

The table above is also stale.  Re-measured 2026-08-17 across 355 scheduler and
weak tests, plus `ProcessTerminateBugTest` three times in each mode: identical
with the flag honoured and with it forced off (12/12 every run).  The three
named regressions do not reproduce.

Whether the 50-preemption starvation guard can now be deleted is untested —
this entry's own repro is preempted via CSSignal, which the fix already
back-appends, so it is probably redundant.

End to end, `MpUnconnectedTransportMiddlewareTest` goes 18/32 -> 31/32
(Cog 32/32).

### 21. ~~An unhandled exception in a FORKED process wedges the whole eval~~ — RETRACTED 2026-08-15, SAME DAY

**There is no wedge.  This entry was wrong, and the error is instructive enough
to keep rather than delete.**

The repro filed here —

    [ Error signal: 'boom' ] fork. (Delay forSeconds: 3) wait. 'ALIVE'

does not hang.  The image logs the unhandled error and exits, which is exactly
what `CommandLineUIManager>>unhandledErrorDefaultAction:` is written to do, and
what stock Pharo does with the same script.  Confirmed three ways:

    elapsed        21 s across 3 runs — the same ~20 s every other eval in this
                   image takes to start.  Not a timeout.
    exit status    0, never 124.  `timeout` never fired in ANY of these runs.
    PharoDebug.log 108 matching lines, including entries for the exact probes
                   called "wedged" here: #zorkNotUnderstood and #asUppercase.

The whole entry rested on one bad discriminator: reading "no EVAL-RESULT
printed" as "hung".  It is also, and much more often, what a correct quit looks
like — the eval never reaches its final expression because the image exited
first, on purpose.  Every "wedge" above is that.  The bisection built on top of
it (fork+error needed, neither alone) is real but means something ordinary:
those are the variants that trigger a quit.

Two things this cost, both now corrected:

  * The **forwarding-pointer** theory built on it is REFUTED, not merely
    unproven.  A probe in `Interpreter::literal()` that logs any literal handed
    back forwarded fired ZERO times across the repro.  The "touching the
    literals cures it" observation was measuring quit-vs-quit, not fix-vs-wedge.
    The candidate read paths noted earlier (`literal()`,
    `pushLiteralVariable()`) do lack `followForwarded`, which may still be worth
    a look on its own merits — but nothing here is evidence for it.

  * The **#15 revert** used this same discriminator and is therefore also
    suspect; see the correction under #15.

Method note, since this is the second time in this repo a "hang" turned out not
to be one: for anything that ends without output, elapsed time and exit status
have to be recorded, because "produced no result" and "did not terminate" are
different findings that look identical in a log.

### 22. ~~`RSLinesTest` — `BlockCannotReturn` on a four-frame non-local return~~ — FIXED and VERIFIED 2026-09-03

    RSLinesTest, JIT on          18 P / 0 F / 0 E    (was 16 P / 2 E on arm64,
    RSLinesTest, PHARO_NO_JIT=1  18 P / 0 F / 0 E     17 P / 1 E on x86_64)

Both modes, so the defect was in the shared home resolution and not in a JIT
specialisation — the `PHARO_NO_JIT` bisect this entry called for is answered by
the fix itself.  Our VM reports the same shape the fix targets:

    RSAbstractLine >> markersIncludesPoint:
       methodClass      = RSAbstractLine
       block outerCode  = RSTMarkeable>>#markersIncludesPoint:
       outerCode == m   = false

Raw in `docs/results/sweep-arm-2026-09-02/defect22-verification.txt`.  The
analysis that got there is kept below, refutations included.

Two errors in the 2026-09-02 arm64 sweep and one in the x86_64 sweep
(`testMarkersIncludesPoint` on both, `testMarkerOffset` on arm only), so the
defect is **arch-independent** — as a home-resolution bug in shared C++ should
be.  It is the only newcomer in that residual that is VM-visible rather than
display-, network- or image-related:

    ERROR: testMarkerOffset          (BlockCannotReturn: )
    ERROR: testMarkersIncludesPoint  (BlockCannotReturn: )

The non-local return in question is short and entirely synchronous — read out
of this image's `.sources`:

    RSLine>>includesPoint:            ^ self hasBorder and: [ ... or: [ self markersIncludesPoint: aPoint ] ]
    RSShape>>markersIncludesPoint:    self markerShapesInPositionDo: [ :m |
                                          (m shape preciseIncludesPoint: aPoint) ifTrue: [ ^ true ] ]
    RSLine>>markerShapesInPositionDo: self markerEnd ifNotNil: [ :marker | marker withEnd: cp do: aBlock ]
    RSMarker>>withEnd:controlPoints:do:  ... self setPositionTo: to vector: to-from do: aBlock
    RSMarker>>setPositionTo:vector:do:   ... aBlock value: shape

`ifNotNil:` with a literal block is inlined by the compiler, so the `^ true`
returns through four live method activations to a home that is still on the
stack.  `BlockCannotReturn` means the VM decided that home was gone.  Not the
#15 depth family (that was a 200-frame cap on the context-NLR home search,
raised to 70000; this is four frames), and not the Build-80
`HasBeenReturnedFrom` resume case, which is about resuming an already-returned
context rather than returning to a live one.

**Intermittent**: clean in the 2026-08-22 sweep, present in the pre-reboot
residual and in this one.  Reach for `PHARO_DET_SCHED=1` before adding
instrumentation.

#### CONFIRMED 2026-09-02 — Cog passes, and the mechanism is measured

Stock Cog v10.3.9 (x86_64 under Rosetta) runs `RSLinesTest` **18 P / 0 F /
0 E**.  So this is ours.  And the mechanism is no longer a guess — asking the
image directly:

    RSAbstractLine >> markersIncludesPoint:
       methodClass       = RSAbstractLine
       inner blocks      = 1
       block outerCode   = RSTMarkeable>>#markersIncludesPoint:     <-- the TRAIT
       outerCode == m    = false

The class's own copy of the trait method carries a `CompiledBlock` whose
`outerCode` still names the **trait's** method.  Our inline NLR resolves the
`^ true`'s home by matching that method oop against
`savedFrames_[si].savedMethod` (`Interpreter.cpp:8470`), and the frame on the
stack holds `RSAbstractLine`'s copy — so the match cannot succeed, the
context-chain search finds nothing either, and the VM sends `cannotReturn:` on
a home that is alive.  Raw in
`docs/results/sweep-arm-2026-09-02/cog-defect22-evidence.txt`.

**Blast radius, measured over the whole image:**

    23963   methods with inner CompiledBlocks
     3278   of those, some block's outerCode is a DIFFERENT method (13.7%)
      677   of those, that block performs a non-local return   <-- can hit this

677 installed methods can take this path, and the list is not exotic: it starts
with `findOriginMethodOf:` / `findOriginClassOf:` on the metaclass of every
traited class, i.e. the traits infrastructure itself.  That the suite still
passes 27461 tests says the defect needs the `^` to actually fire, not merely
to exist.

The fix belongs in the home resolution.  Spur's model is identity-based — a
`FullBlockClosure` knows its `outerContext` — and our code has a dynamic
fallback that uses exactly that, tried only after the static `outerCode`
method-oop match fails, with a comment warning that `closure_` can be stale on
JIT-resident block returns.  So `PHARO_NO_JIT=1` separates "the fallback is
never reached" from "the fallback is reached with a stale closure".

#### The original hypothesis, kept for the record (2026-09-02)

`markersIncludesPoint:` and `markerShapesInPositionDo:` are each defined
**twice** in this image, and the second owner is a trait:

    markersIncludesPoint: aPoint       RSAbstractLine   +  RSTMarkeable
    markerShapesInPositionDo: aBlock   RSAbstractLine   +  RSTMarkeable

`RSTMarkeable`'s own class comment says "I am a trait to create markers in some
especific classes", and the image stores two separate sources for each
selector, so two `CompiledMethod`s exist rather than one shared object.

The inline NLR resolves the `^ true`'s home by matching the **method oop** of
the block's static `outerCode` literal against `savedFrames_[si].savedMethod`
(`Interpreter.cpp:8470`).  So the question that decides this defect is one an
`eval` answers in a line: does `RSAbstractLine>>markersIncludesPoint:`'s inner
`CompiledBlock` have an `outerCode` that is *that* method, or the trait's?  If
it is the trait's, the identity match cannot succeed against the frame actually
on the stack, the context-chain search finds nothing either, and the VM sends
`cannotReturn:` — `BlockCannotReturn` on a home that is demonstrably alive.

    (RSAbstractLine >> #markersIncludesPoint:) literals
        detect: [ :l | l isCompiledBlock ] ifNone: [ nil ]
        -> then compare `that outerCode == (RSAbstractLine >> #markersIncludesPoint:)`

This is a hypothesis with a one-line check, not a finding.  What IS established
is that the two selectors on the failing return each exist twice with a trait
as one owner, and that the VM's home resolution is an oop-identity match.

The code already knows this failure shape.  The comment at
`Interpreter.cpp:8432` describes it for a different cause ("the CompiledBlock
is SHARED between methods ... so outerCode points at the stale original") and
adds a dynamic fallback that retries from the running closure's captured
`outerContext`.  The same comment says why that fallback can miss: "closure_
need not correspond to the frame being returned from (JIT-resident block
returns reach here with a stale closure_)".

So the first bisect is `PHARO_NO_JIT=1`.  Passing there points at the stale
`closure_` on the JIT-resident return, i.e. the dynamic fallback failing to
rescue a trait-copied home; failing there too points at the static
`outerCode`/method-oop identity match itself.  Either way the fix is in the
home resolution, not in a T1 specialization.

Note this puts both of the 2026-09-02 newcomers on trait method copies — the
other being the storm that killed the batch containing all 27 `Trait*Test`
classes.

### 23. The Context storm — Cog runs the block clean, we exhaust the heap — HIGH, arm64-only, non-deterministic

Reproduces in **18 s** at `PHARO_MAX_OLD_SPACE_MB=1024`.  It is NOT the trait
tests — the accumulation is already running in the Tonel block, and the 27
trait classes are clean in isolation on our VM.  The bisect below is
self-contradictory because the storm is timing-dependent, so `PHARO_DET_SCHED=1`
is the next instrument.  Details in order below.

The arm64 sweep's batch 1901-1950 died at index 1912 with

    [VM] FATAL: old space exhausted during scavenge tenure (16 free of 12884901888)
    [HEAP-CENSUS] 33363299 objs  Context / 6656491 objs Error / 6664295 objs FullBlockClosure

taking 39 classes with it — all 27 `Trait*Test` plus `TrueTest`, `Tutorial*`,
`UDPSocket*`, `UUID*` and `Undefined*`.  Census and analysis in
`docs/results/sweep-arm-2026-09-02/storm-heap-census.txt`; the ratios say an
unbounded recursion that signals and catches an `Error` at every level and
never unwinds (1.00 closures per Error, 5.01 Contexts per Error).

**And the x86_64 sweep ran the whole block without storming** (2026-09-03):
batch 1901-1950 completed in 270 s with all 51 classes.  So the storm is
**arm64-only** in this pair of runs — which narrows it a great deal, and makes
`PHARO_DET_SCHED=1` on arm the obvious next instrument.  It also means the
trait block finally has one of our own measurements to compare against Cog:

    TraitTest             53 P / 1 F   testTraitsUsersSanity [Assertion failed]
    TraitFileOutTest       2 P / 2 E   FileDoesNotExistException — the test
                                        writes its fileOut into the CWD
    TraitInTraitClassTest  1 P / 1 T
    the other 24 classes   clean

`TraitTest>>testTraitsUsersSanity` is the selector behind the entry this file
has carried since 2026-06-01.  See `docs/results/sweep-x86-2026-09-03.md`.

**And it is not an arbitrary assertion — it is a whole-image invariant on
exactly the state a trait rebuild touches:**

    testTraitsUsersSanity
        Smalltalk allClassesAndTraits do: [ :each |
            self assert: (each traits allSatisfy: [ :t | t traitUsers includes: each ]) ].
        Smalltalk globals allTraits do: [ :each |
            self assert: (each traitUsers allSatisfy: [ :b | b traits includes: each ]) ]

Every class must be listed by the traits it uses, and every trait's users must
still use it.  The test runs late in the trait block, after two dozen classes
have created and rebuilt traits through `ShiftClassBuilder`.  So the failure
says our trait rebuilds leave `traitUsers` inconsistent — stale state left
behind by a rebuild, which is the SAME family as this defect's trigger
(a trait-class rebuild with live instances leaving a husk).

That is the cheapest handle on this defect so far, and it does not need the
storm: run the trait block and then evaluate the two assertions directly.  On
x86_64 the invariant breaks without any storm; on arm64 the storm arrives
first.  One mechanism, two escalations, is the hypothesis to test.

**Stock Cog runs the same 27 classes clean, in seconds:**

    27 classes   270 tests   0 F   0 E        (Cog v10.3.9, x86_64 under Rosetta)
    TraitTest alone: 54 P / 0 F / 0 E

Raw in `docs/results/sweep-arm-2026-09-02/cog-trait-baseline.txt`.  So this is
ours, not the image and not the harness.  It also finally settles the entry
under "Confirmed our-VM bugs" that has read "`TraitTest` 54P/0/0 on Cog, errors
on ours — not yet drilled" since 2026-06-01: same class, same Cog number, and
now a mechanism-shaped symptom to chase.

What is already ruled out by reading (2026-09-02), so do not re-derive it: the
2026-07 husk-trigger fixes are all still in place —
`collectInstancesOfClass` skips forwarded objects (`ObjectMemory.cpp:3386`),
and `becomeForward`-leaves-forwarder and classOf-follows-forwarders are
default-on with opt-out knobs only (`:1803`, `:1895`, `:558`).  See
`docs/history/arm-context-storm-2026-07.md`, which identifies the storm's
trigger as exactly a trait-class rebuild with live instances.

**A correlation worth carrying into the hunt:** `[DIAG-TIMER] ... timerSem=nil`
— the Delay scheduler's timer semaphore gone — appears in only 7 of the arm64
sweep's 41 batches, once each, EXCEPT in the two pathological ones:

    batch 1901-1950 (this storm)          16 occurrences
    batch 1801-1850 (defect #26's hang)   22 occurrences
    five other batches                     1 each

A dead Delay scheduler means `valueWithin:onTimeout:` never fires, so anything
that retries under a timeout retries forever — which is the shape of an
unbounded recursion that signals and catches an Error at every level.  Whether
that is cause or consequence here is not established; it is a place to look
early, and `PHARO_DET_SCHED=1` makes the ordering reproducible.

#### CORRECTION 2026-09-03: it is not the trait tests, it is the Tonel ones

Two runs settle this, and both contradict the "start at `TraitChangesTest`"
reading below.

*The trait classes are innocent in isolation.*  All 27 of them on our VM
against the pristine base image, with the `traitUsers` invariant re-checked
after each: **0 violations at all 28 checkpoints, `TraitTest` 54 P / 0 F / 0 E,
no storm** — identical to Cog bar `TraitFileOutTest`'s working-directory
artifact (`docs/results/sweep-x86-2026-09-03/ours-trait-invariant.txt`).

*And the storm starts EARLIER than the class the 12 GB heap died on.*  Re-run
batch 1901-1950 whole with the heap squeezed to 1024 MB and it reproduces in
**18 seconds**:

    rc=134, 8 of 51 classes, last reported TonelWriterV1Test (index 1909)
    [HEAP-CENSUS] 2654994 Context / 516760 Error / 524728 FullBlockClosure

Same 5.1 : 1 : 1 ratios as the sweep's 12 GB census.  A smaller heap fills
sooner, so the class boundary it dies on moves earlier — 1909 here against 1912
in the sweep.  **That means index 1912 was never the start; it was where a
12 GB heap ran out.**  The accumulation is already running during the Tonel
block:

    1903 TonelParserTest      1907 TonelScannerTest
    1904 TonelReaderTest      1908 TonelSourceScannerTest
    1905 TonelReaderTraitCompositionTest
    1906 TonelRepositoryTest  1909 TonelWriterV1Test

which also explains why the trait block alone never storms.

#### And the bisect says it is NON-DETERMINISTIC

Seven runs at `PHARO_MAX_OLD_SPACE_MB=1024`, each a couple of seconds unless it
storms:

    1901-1902  Timespan only          rc=0     3s   clean
    1903-1909  Tonel only             rc=0     4s   clean
    1901-1905  prefix to 1905         rc=134  18s   2655142 Contexts  <-- STORM
    1901-1907  prefix to 1907         rc=0     5s   clean
    1901-1909  prefix to 1909         rc=0     4s   clean
    1905-1905  TonelReaderTraitComp   rc=0     2s   clean
    1909-1909  TonelWriterV1 alone    rc=0     2s   clean

`1901-1905` storms while both of its supersets, `1901-1907` and `1901-1909`,
run clean — so this is not a function of the class set.  It is timing, and it
matches the Heisenbug character the 2026-07 dossier records ("2/10 real-catalog
runs").  The storming run reported 3 classes before dying, i.e. it went down
during index 1904 `TonelReaderTest`, and its census is the same 2.65M Contexts.

**Rate, measured on the same range** (1901-1950's prefix 1901-1905, 1024 MB):

    wall clock          1 of 3 runs storms      18 s when it does, 4 s when not
    PHARO_DET_SCHED=1   0 of 3 runs storms      45 s every time

So it is roughly a one-in-three race under the wall clock, and
`PHARO_DET_SCHED=1` **suppresses** it rather than pinning it — the opposite of
what that knob did for the aigraph transient CLAUDE.md cites.  Its 1024-bytecode
yield quantum is far finer than the ~2 ms heartbeat it replaces, so the
interleaving it produces is not the one that races.

`PHARO_DET_SCHED_QUANTUM=N` widens that to N x 1024 bytecodes — coarser, closer
to the wall-clock interleaving, still deterministic — and walking it up is the
next thing to try: a quantum that storms every time turns this into a fixed
repro.

#### `PHARO_DET_SCHED_QUANTUM=16` reproduces it — 2 of 2 where wall clock is 1 of 3

    quantum   runs   storms   time
    (wall)      3      1      18 s storming, 4 s not
    1           3      0      45 s
    4           2      0      4-6 s
    16          2      2      18-19 s      <-- reliable repro

So the storm needs a yield interval near 16 x 1024 bytecodes: finer (quantum 1
or 4) and it never fires, and the wall-clock heartbeat only lands in the window
about a third of the time.  **`PHARO_DET_SCHED=1 PHARO_DET_SCHED_QUANTUM=16
PHARO_MAX_OLD_SPACE_MB=1024` on batch 1901-1905 is the repro to use** — about
19 seconds, and it survives instrumentation, which is the whole point of
DET_SCHED and what plain quantum 1 failed to deliver here.

Not byte-identical between runs (2 vs 3 classes reported, Context counts
2655263 vs 2663681), so some wall-clock input remains — Delays, most likely —
but it storms every time.

**Practical note on the knob**: at quantum 1 it costs ~15x.  The 5-class range
goes 4 s -> 45 s, and a whole 51-class batch does not finish — one attempt
reached 14 of 51 classes in 900 s.  At quantum 4 the cost is back to 4-6 s.  So
DET_SCHED at the default quantum is usable for a handful of classes and not for
a batch; widen it or narrow the range.  Raw in `docs/results/sweep-arm-2026-09-02/defect23-bisect.txt`.

Note the Context count barely moves across storms — 2654994, 2655137, 2655142 —
because it is set by the heap size, not by the bug; do not read it as a
signature.

#### The breaker DOES fire in the storm — once — and that is the problem

I read this wrong twice, so the evidence first.  Both storm runs contain

    [LOW-SPACE] threshold crossed #1 (free=22 MB) — signaling LowSpaceSemaphore

exactly once, and then

    [VM] low-space breaker at abort: threshold=0 bytes (DISARMED ...)

The threshold is zero at the abort **because the breaker disarmed itself on
delivery** — Cog's one-shot contract, where the image re-arms from
`lowSpaceWatcher`'s tail.  It is not "never armed", which is what a bare
`threshold=0` reading suggested and what an earlier version of this entry said.
(The diagnostic now prints the delivery count so the two cannot be confused
again.)

22 MB free is exactly the effective threshold the fix computes —
`max(400000, min(one eden, reservation/16))` with a 1 GB reservation — so the
mechanism worked as designed.  What did not happen is the image stopping the
hog: `[DIAG-TIMER] ... timerSem=nil` right beside it says the Delay scheduler is
already dead, so the P60 `lowSpaceWatcher` cannot run to re-arm, and the storm
spends the last 22 MB.  The queue dump names the culprit:

    [DIAG-QUEUE] P79 proc=... susp=Error>>or:

That print is `<receiver class>>><method selector>` of the process's suspended
context (`Interpreter.cpp:4529-4540`), so what is solid is: **the storming
process is at P79 and its suspended context's receiver is an `Error`.**  Read
the selector with care — `or:` is compiler-inlined when its argument is a
literal block, so a context whose method is `or:` is unusual, and
`selectorOf` on a `CompiledBlock` does not answer what it does on a method.
Do not build on the selector until it is confirmed from a live process dump.

The census puts a tight constraint on any explanation: **one `FullBlockClosure`
per `Error`** (516808 : 524544, a ratio of 1.015) and **5.1 `Context`s per
`Error`**.  Whatever the loop is, each turn of it allocates exactly one closure
and one Error.  One firing at 22 MB is not
enough headroom for a process minting ~150k Contexts a second, and after that
firing there is no second chance.  (In a bare `eval` it does, and the
breaker fires 462 times; the 2026-07 dossier has this exactly backwards.)  The
latch fix is still right and still needed, but it cannot help a run where the
image never arms the threshold.

**Startup ORDER is not the reason** — measured, so nobody re-derives it: in an
image with the runner filed in, `SessionManager default startupList` puts
`ProcessorScheduler` at #5 and `SUnitRunner` at #50 of 56.  The scheduler's
handler, which calls `installLowSpaceWatcher`, runs long before the runner's.

What that leaves is *when the arming actually happens*.
`installLowSpaceWatcher` only FORKS: it creates
`[ self lowSpaceWatcher ] newProcess` at `Processor lowIOPriority` (60) and
resumes it.  The `primSignalAtBytesLeft:` call is inside `lowSpaceWatcher`, so
the threshold is armed only once that **P60 process first gets the CPU** — and
the runner forks its tests at P79/P80.  In a bare `eval` there is little to
compete with and it arms (462 firings); in a batch it can lose the race, and a
storm that starts ~18 s in gets there first.  Consistent with both
observations: the batch-1801 log shows the watcher reaching
`#lowSpaceThreshold` at step 161M, i.e. it does eventually run, while the
1024 MB storm aborts with `threshold=0` well before that.

So the remaining question is a scheduling one, and `PHARO_DET_SCHED=1` makes it
observable.  The other candidate, `lowSpaceWatcher`'s own guard bailing with
"Not enough memory to launch the lowSpaceWatcher", needs
`garbageCollectMost`/`garbageCollect` to answer under 400000; ours return
`freeOldSpaceBytes()`, which is billions here, so it is unlikely.

#### (superseded) Where to start: `TraitChangesTest`.**  Not inferred any more — the index map
is generated from the image and archived at
`docs/results/sweep-arm-2026-09-02/class-index-map.txt`.  It reproduces
`TOTAL=2047` exactly, and

    1911  TonelWriterV3Test      <- last class the dead batch reported
    1912  TraitChangesTest       <- the storm starts here or after
    1951  UndefinedPackageTest   <- first class of the next batch

The two `*Test` classes in that range the runner drops as abstract are
`TraitAbstractTest` and `TraitTestCase`, which is why 41 candidates leave 39
missing.

And `TraitChangesTest` is not an arbitrary starting point — it is the trigger
family.  Every one of its tests drives `ShiftClassBuilder` to create and then
REBUILD a trait:

    t1 := self newTrait: #T1 with: { }.
    builder := ShiftClassBuilder new name: #T1; beTrait; traitComposition: TEmpty; ...
    builder oldClass: t1.
    builder tryToFillOldClass. builder detectBuilderEnhancer.
    builder builderEnhancer validateRedefinition: builder oldClass.
    builder validateSuperclass. builder compareWithOldClass.

which is exactly what `docs/history/arm-context-storm-2026-07.md` identifies as
the storm's trigger: a trait-class rebuild with live instances leaving a stale
husk that a `freeze` handler re-hits.  Twenty-seven classes of that in a row,
each re-creating `#T1`, is a lot of obsolete traits.

The rest of that range on Cog is also on record (same file): `TrueTest`,
`Tutorial*`, `UDPSocket*`, `UUID*`, `Undeclared*` and `Undefined*` are all
clean and take milliseconds, so nothing else in the batch is a candidate.
Note `TraitAbstractTest suite` aggregates the whole block — 273 tests, and it
takes Cog **76.5 s**, so the trait block is genuinely heavy work, not a
trivial suite our VM trips over.

**Chunks of five do NOT reproduce it** (2026-09-03): all eight chunks covering
1912-1950 ran clean, 135 s for the whole 39 classes that the sweep never got
to.  That is a property of the hunt, not
evidence against the defect — `sunit-sweep.sh` relaunches from a pristine image
per chunk, and the storm needs the accumulated state of many trait rebuilds in
ONE image.  The sweep runs all 51 classes in a single VM; the hunt must too.

So bisect by CLASS COUNT within one image, not by fresh chunks: run
1901-1950 whole (it storms), then 1901-1930, then 1901-1920, and so on.  The
`traitUsers` invariant above is the cheaper instrument for the same question —
`scripts/pkg-jit-test/probe_trait_users_sanity.st` runs all 27 classes in one
image and re-checks the invariant after each, so it names the first class that
breaks it without needing the storm at all.  Stock Cog scores 0 violations at
all 28 checkpoints
(`docs/results/sweep-x86-2026-09-03/cog-trait-invariant-baseline.txt`).

Then `storm_repro_husk_freeze.st`, which must answer `NO-HUSK`.  Use
`PHARO_MAX_OLD_SPACE_MB=1024` for the hunt — the storm took 758 s to reach the
FATAL at 12288 and should take about a twelfth of that at 1024, and the census
ratios that identify it do not depend on the absolute counts.

### 24. Fourteen classes fail because the runner must suspend the UI process — ROOT-CAUSED 2026-09-02, MEDIUM

Carried through several sweeps as "the missing display" and therefore as a
residual that could not be helped.  The 2026-09-02 Δcog says otherwise: stock
Cog v10.3.9, **headless**, on the same pristine Pharo 13.1 image, with no
fake-GUI prelude, scores 0 F / 0 E on every one of them.

    class                                    Cog        ours (arm64 sweep)
    SpAthensAdapterTest                      18P/0F/0E   8P/0F/1E
    SpComponentListAdapterTest               16P/0/0     7P/0/1
    SpListCommonPropertiestTest              46P/0/0    18P/0/5
    SpTableCommonPropertiestTest             34P/0/0    14P/0/3
    SpTreeAdapterMultipleSelectionTest       40P/0/0    18P/0/2
    SpTreeTableAdapterMultiColumnMultiSel..  40P/0/0    18P/1/1
    SpTreeTableAdapterMultiColumnTest        40P/0/0    18P/1/1
    SpTreeTableAdapterSingleColumnMultiSel.. 40P/0/0    18P/1/1
    SpTreeTableAdapterSingleColumnTest       40P/0/0    17P/1/1
    FTTableMorphTest                          1P/0/0     0P/0/1
    StDebuggerActionModelTest                54P/0/0    53P/1/0
    StSpotterModelTest                        2P/0/0     0P/2/0
    StTranscriptPresenterTest                 5P/0/0     2P/3/0

The dominant error on our side is `SubscriptOutOfBounds: 1 in #()` reaching for
an adapter's widget list, plus `MessageNotUnderstood: receiver of "x" is nil`.

#### Ruled out: the parameterisation (2026-09-02, and I got this wrong first)

The nine `Sp*` classes descend from `ParametrizedTestCase`, their suites are
exactly twice their selector count, and our runner ran exactly half of what Cog
ran — so the obvious story was that the runner builds
`testClass selector: sel`, whose `parametersToUse` really is nil, leaving the
adapter with no backend.  I filed that, wrote a runner change for it, and it is
**wrong**.  `ParametrizedTestCase>>setUp` carries a workaround for precisely
this case:

    (self parametersToUse isEmpty and: [self class testParameters isNotEmpty])
        ifTrue: [ self class testParameters expandMatrix first
                    do: [ :aParameter | aParameter applyTo: self ] ]

so a bare instance applies the first parameter set anyway.  Measured on Cog
along the runner's own path — a bare `SpListCommonPropertiestTest` driven by
`runCase` passes **23 of 23** selectors.  The change was reverted
(submodule `ad78260`).  Do not re-derive this: an instance dump showing
`parametersToUse = nil` is not evidence, because `setUp` fills it in.

What the parameterisation DOES explain is the count difference, and only that:
our 23 against Cog's 46 is one case per selector versus the whole matrix.
Image-wide that is a real coverage gap — 241 of the 2047 concrete test classes
are parameterised, their selectors total 2074 and their suites 12641, so 10567
cases have never been run by this harness — but it is a coverage gap, not a
cause of these failures.  (`scripts/package-tests-selfhosted.sh` already runs
`c suite run`, so the package tier is unaffected either way.)

#### ROOT CAUSE, reproduced on stock Cog: Morphic installed, UI process suspended

Two wrong hypotheses first, both refuted by test rather than argument, and both
worth not re-deriving:

  * *No display.*  Cog passes these with `Display` nil, `UIManager default` =
    `NonInteractiveUIManager` and `isHeadless` true.
  * *No `WorldMorph`.*  Nilling `World` on Cog gives 2 P / 21 F, every one
    `MessageNotUnderstood: receiver of "displayScaleFactor" is nil` — not our
    signature at all.

The answer is the third thing.  On stock Cog, install `MorphicUIManager` and
then **suspend its UI process**:

    UIManager default = MorphicUIManager
    uiProcess suspended? = true
    pass=18 fail=5
       testChangeListInPresenterUpdatesWidget                    SubscriptOutOfBounds: 1 in #()
       testDoubleClickActivatesRowInDoubleClickActivationMode    SubscriptOutOfBounds: 1 in #()
       testRemoveHeaderTitleInPresenterRemovesColumnHeaderMorph  AssertionFailure: Assertion failed
       testSetColumnTitleInPresenterPutsColumnHeaderMorph        SubscriptOutOfBounds: 1 in #()
       testSingleClickActivatesRowInSingleClickActivationMode    SubscriptOutOfBounds: 1 in #()

That is our arm64 sweep's result for this class exactly — same count, same five
selectors, same messages.  Leave the UI process RUNNING and Cog is back to
23 / 23, so it is the suspension and nothing else: the widget refreshes are
deferred to the UI process, and with it suspended the assertions read an empty
list.

**The same condition reproduces most of the rest of the bucket**, which is why
this entry is no longer about nine classes:

    class                      Cog suspended            ours (arm64 sweep)
    FTTableMorphTest           0P/1F SubscriptOOB       0P/1E, same test+message
    StTranscriptPresenterTest  2P/3F "Got '' ..."       2P/3F, same three
    SpAthensAdapterTest        8P/1F MNU "x" is nil     8P/1E, same test+message
    SpComponentListAdapterTest 7P/1F SubscriptOOB       7P/1E, same test+message
    StDebuggerTest            63P/4F                    58P/3F, all three among them
    StDebuggerInspectorTest   10P/1F                    same — but Cog fails it
                                                        UNSUSPENDED too: parity
    StSpotterTest              2P/1F                    same — parity likewise
    ---- not reproduced: still ours ----
    StDebuggerActionModelTest 55P/0F                    53P/1F testEventAfterProceed
    StSpotterModelTest         2P/0F                    0P/2F

Of those last two, the x86_64 sweep narrows it further:
`StDebuggerActionModelTest` is **clean on x86_64** — the only class in 1800
covered classes that is non-clean on arm and clean on x86 — so its single
`testEventAfterProceed [Denial failed]` is timing, not logic.
`StSpotterModelTest` fails on both arches and passes on Cog suspended or not —
but reading its two tests closes it out rather than leaving it open:

  * `testAnnounceQueryEndedIsSentOnce` opens with
    `self skipOnPharoCITestingEnvironment`, so Pharo's own CI does not run it;
    our sweeps do, because they do not set `PHARO_CI_TESTING_ENVIRONMENT`.
  * `testSpotterModelShouldWaitToPerformActualSearch` forks the search and then
    asserts it has NOT started for 5 x 50 ms and HAS started 300 ms later — a
    250-to-550 ms scheduling window and nothing else.

So both are wall-clock, one of them upstream-skipped.  With that, every
non-pass in the 2026-09-02 arm64 residual is attributed, and none of the GUI
ones is a VM computation error.

Counting the whole 2026-09-02 arm64 residual against that: **9 of the 21 FAILs
and 17 of the 21 ERRORs are downstream of this one suspension.**  Three more
FAILs are parity with Cog (image issues), and three are genuinely ours and
unexplained — `StDebuggerActionModelTest>>testEventAfterProceed` and
`StSpotterModelTest`'s two.

Raw in `docs/results/sweep-arm-2026-09-02/cog-defect24-repro.txt`.

**Restoring `NonInteractiveUIManager` afterwards does not help** — tested:
install Morphic, suspend the UI process, then set `NonInteractiveUIManager` back
as default, and it is still 18 P / 5 E with the same five.  So the operative
state is not which manager is installed but a Morphic world that was **started
and then stopped**; nothing restarts the deferred work.  A pristine image that
never installed Morphic passes 23 / 23.

That points at the prep as the cheapest harness-side lever: our prepped image
is snapshotted with a live Morphic world (which is why the resume restarts a
render loop at all), where a stock headless boot has none.  Prepping with
`NonInteractiveUIManager` and no live world before `snapshot:andQuit:` would
leave nothing for the runner to suspend.  Untested — it needs our VM.

**And our runner suspends it deliberately.**  `run_sunit_tests.st`'s
`startUp:` does it as its very first action, and says why:

> Suspend the saved WorldMorph render loop FIRST, before any other startup
> work.  On our custom VM the headless resume restarts MorphicRenderLoop at
> pri-80; it busy-spins `WorldState>>drawWorld:` (Morphic DNUs), starves the
> pri-80 Delay scheduler, and the scheduler dies (timerSem=nil) → only-idle
> wedge before any test runs.

So the chain is complete, and the defect is not where it was filed:

    our VM's headless resume restarts MorphicRenderLoop
      -> it busy-spins with Morphic DNUs and kills the Delay scheduler   <- THE DEFECT
      -> the runner suspends the UI process to survive that
      -> deferred widget refreshes never run
      -> nine Sp* classes fail, and got labelled "the missing display"

`setup_fake_gui.st` clears the bucket because it starts a UI process again.

**What is proven and what is not.**  Proven: the suspended-UI-process state
reproduces our exact failure set on stock Cog.  Taken from the runner's own
comment, not re-measured tonight: that our headless resume installs Morphic and
that the render loop DNUs.

**Measured 2026-09-03, and on a PRISTINE image we are identical to Cog:**

    ours: {NonInteractiveUIManager. true. 1. WorldMorph. UndefinedObject}
    Cog:  #(NonInteractiveUIManager  true  1  WorldMorph  UndefinedObject)

(`UIManager default class`, `isHeadless`, `WorldMorph allInstances size`,
`World class`, `Display class`.)  So there is no environment difference in a
bare boot at all, and the divergence has to come from the PREPPED image —
which is snapshotted with a live Morphic world, and is why the resume has a
render loop to restart and the runner has one to suspend.  That promotes
"prep with `NonInteractiveUIManager` and no world before `snapshot:andQuit:`"
from a cheaper lever to **the** thing to try.  Then chase the
`WorldState>>drawWorld:` DNUs — **now filed separately as defect #25**, because
a pri-80 busy-spin that kills the Delay scheduler is a defect in its own right.
Fixing it retires nine classes of residual without touching Spec.

Raw: `docs/results/sweep-arm-2026-09-02/cog-residual-baseline.txt` and
`cog-parameterized-check.txt`.

### 25. The resumed `MorphicRenderLoop` busy-spins on Morphic DNUs and kills the Delay scheduler — NEW 2026-09-02, MEDIUM

The root behind #24, promoted out of a runner comment because it is a VM
divergence and has been costing us a residual bucket for months.

`run_sunit_tests.st`'s `startUp:` suspends the saved `WorldMorph` render loop
as its very FIRST action, ahead of everything else, and says why:

> On our custom VM the headless resume restarts `MorphicRenderLoop` at pri-80;
> it busy-spins `WorldState>>drawWorld:` (Morphic DNUs), starves the pri-80
> Delay scheduler, and the scheduler dies (timerSem=nil) → only-idle wedge
> before any test runs.  The same suspension lived inside `runAllTests` but ran
> ~111 lines too late (after the deferred timer bootstrap at step 25M).

Stock Cog on the same image resumes headless with `UIManager default` =
`NonInteractiveUIManager`, `Display` nil, and no such spin.  So either our
resume path installs Morphic where Cog does not, or the render loop it starts
DNUs where Cog's would not — and the DNUs are the part to chase, because a
busy-spin at pri-80 that starves the Delay scheduler is a scheduling defect on
its own, quite apart from Spec.

**Cost, now measured:** with the UI process suspended, every widget refresh
that Spec defers to it never runs.  Reproduced on stock Cog — Morphic installed
and its UI process suspended gives `SpListCommonPropertiestTest` 18 P / 5 E,
our exact failure set (#24, `cog-defect24-repro.txt`).  Nine classes of the
2026-09-02 residual are downstream of this suspension.

**Ruled out by reading (2026-09-02), so do not spend time on it:** the loop is
`[ aBlock value ] whileTrue: [ self doOneCycle. Processor yield ]`, so an
obvious theory is that our `Processor yield` fails to rotate the pri-80 queue
and lets the loop monopolise it.  It does not — `primitiveYield`
(`Primitives.cpp:9552`) appends the active process to the BACK of its priority
list and then wakes the highest-priority ready process, which is Cog's
behaviour, including the no-other-process-at-this-priority early exit.

**Where to start:** get the DNU selectors.  The runner suspends the loop before
they can be observed, so run a headless resume WITHOUT the suspension — file
the runner in and immediately resume the UI process, or run a bare image with
`PHARO_MAX_STEPS` low — and read what `WorldState>>drawWorld:` sends that the
image does not understand.  A Morphic DNU under a display-less resume is
usually a missing plugin primitive answering nil where a Form or an event
buffer is expected.

### 26. ~~The threaded-FFI batch never exits~~ — FIXED and VERIFIED 2026-09-03: 1800 s -> 54 s

    before   rc=124   1800 s   (55 s of work, 1730 s idle)
    after    rc=0        54 s   51 of 51 classes reported

    [primitiveQuit] Deferred: 1 callback(s) outstanding, C frames must unwind first
    [primitiveQuit] Deferred: 1 callback(s) outstanding, C frames must unwind first
    [primitiveQuit] Honouring deferred quit from the callback loop
                    (grace period expired; callback did not unwind)

Clean exit, so the plain return out of the nested loop unwound fine and the
`CallbackComplete` fallback noted below is not needed.  That is ~29 minutes
recovered per sweep per architecture.  Raw in
`docs/results/sweep-arm-2026-09-02/defect26-verification.txt`.

Sweep batch 1801-1850 is the single largest wall-clock item in a full sweep and
has never been filed.  It contains the threaded-FFI block —
`TFCallbacksTest`, `TFUFFICallbackTest`, `TFUFFIConcurrencyTest`, the
`TFUFFI*Marshalling*` families — plus `Step*Test` and `TCPSocket*Test`.

    arm64 2026-09-02   rc=124, 1800 s, classes=51, completed=yes
    x86_64 2026-09-02  same, and idling at ~1% CPU when observed at +20 min
    stock Cog          the whole block: 429 tests, 0 F / 0 E, ~7.4 s total

Exactly one batch per sweep shows the trace — a grep for
`[primitiveQuit] Deferred` across all 41 arm64 batch logs hits only
`batch_1801.log` — so the cost is one occurrence, not a per-batch tax.

**It is a 55-second batch.**  The `[PROGRESS]` lines in the arm64 batch log
date the first `[primitiveQuit] Deferred` to between 50 s and 60 s in, and then
run to 1790 s — so the batch does its work in under a minute and **idles for
1730 s** waiting to be killed.  The fix recovers essentially the whole of it,
per sweep per architecture.

The tests are not the problem, and the results file proves how far it got —
the batch writes its per-class results, its `=== BATCH TOTAL ===` block
(1306 P / 1 F / 0 E / 10 S), its completion marker, and `=== BATCH COMPLETE ===`.
Only then does it fail to exit, and the harness kills it at
`PER_BATCH_TIMEOUT`.  So this is a **shutdown hang**, not a test failure, and
it costs 30 minutes of every sweep on every architecture — an hour a night at
the current cadence.

#### ROOT CAUSE — the deferred quit is never honoured, and the trace was in the log

Two dead ends first: the runner's own comment says *"exitSuccess goes through
SessionManager shutdown which may not reach primitiveQuit"*, and that is not
true of Pharo 13 — `SmalltalkImage>>exitSuccess` is `self exit: 0` and
`exit:` is `<primitive: 113>`, the very same primitive as `quitPrimitive`.  No
SessionManager shutdown is involved.

So primitive 113 runs, and our `primitiveQuit` does this:

    if (callbackDepth_ > 0) {
        fprintf(stderr, "[primitiveQuit] Deferred: %d callback(s) outstanding, "
                        "C frames must unwind first\n", (int)callbackDepth_);
        pendingQuit_ = true;
        return PrimitiveResult::Success;
    }

and `pendingQuit_` is honoured in exactly one place — the `interpret()`
checkpoint — under `pendingQuit_ && callbackDepth_ == 0`.  Both halves fail
here:

  * `TFCallbacksTest`'s old-session test **abandons a same-thread invocation by
    design**, so `callbackDepth_` stays 1 for the rest of the run and the
    condition can never become true;
  * and a VM parked in `enterInterpreterFromCallback`'s nested loop is not
    running the `interpret()` checkpoint at all, so nothing even looks.

The evidence was in the logs the whole time, buried in 4 MB of periodic JIT
stats — `[primitiveQuit] Deferred: 1 callback(s) outstanding, C frames must
unwind first`, five times in the arm64 batch and sixty-one in the x86_64 one.

**Fixed 2026-09-02** by bounding the deferral: `primitiveQuit` records
`g_stepNum` when it first defers, and both the `interpret()` checkpoint and the
nested callback loop honour the quit once `kQuitGraceSteps` (2M bytecodes) have
passed, whether or not the callback unwound.  Stranding a C frame matters while
the VM keeps running; it does not matter when the process is exiting anyway,
and Cog simply exits.  Unbuilt as of this writing.

**What to watch for when verifying.**  Clearing `running_` makes the nested
loop exit and `enterInterpreterFromCallback` RETURN to the callback trampoline
— which is the same path any other `stopVM()` already takes out of that loop,
but the C caller of an ABANDONED callback may no longer be there to return to.
So the batch may end rc=0 (clean) or it may end rc=139.  Either beats rc=124
after 1800 s, and the difference tells us whether the abandoned invocation
still has live C frames: if it crashes, the next thing to try is the exit the
loop's own abandonment path uses — it restores the suspended process and then
`throw pharo::CallbackComplete{}`, with the comment *"C++ exception (not
siglongjmp): unwinds through all C++"* — rather than a plain return.

Worth knowing either way: `callbackDepth_` is decremented ONLY by
`primitiveCallbackReturn` and by the two `[XTCB-DEAD-POP]` paths (worker
timeouts).  A same-thread invocation the image abandons is never popped by
construction, so the depth cannot come back to zero on its own.  That is why
bounding the deferral, rather than waiting for the depth, is the fix.

**And the quit is not the only thing that stalls on it.**
`Interpreter::drainCallbackGraveyard()` opens with `if (callbackDepth_ != 0)
return;` — so once `TFCallbacksTest` has abandoned an invocation, every retired
libffi closure and `ffi_cif` for the REST OF THE RUN is buried and never freed.
Small per entry and not fatal, but unbounded in principle, and it is a second
symptom of the same stuck counter.  A deeper fix that popped the depth on
abandonment would clear both; bounding the quit only clears the one that costs
30 minutes a sweep.

The mechanism is already described in our own source.  `Interpreter.cpp`'s
`enterInterpreterFromCallback` comment says an *"abandoned same-thread
invocation (TFCallbacksTest's old-session test, by design) parks everything
after it inside this loop"*.  A VM parked in that nested loop has left the main
`interpret()` loop, so whatever asks it to quit is not being seen — the loop
polls `hasPendingSignals`, `checkTimerSemaphore`, the xtcb adoption drain and
(since `3c75aca5`) the low-space latch, but nothing that ends the session.

Two things to check, in order: whether `running_` / the quit path is observed
by that loop at all, and whether the abandoned invocation can be reaped at
image-quit time instead of parking forever.  Raw Cog numbers in
`docs/results/sweep-arm-2026-09-02/cog-tf-callback-baseline.txt`.

## LEADS — a SEPARATE number space (real work, not yet a filed defect)

These are `LEAD n`, NOT `#n`.  The two spaces overlap (there is a defect #15
AND a lead 15) because leads were numbered as a continuation of the defect
list back when the list ended at 14.  Existing cross-references say "lead 22",
so the numbers are kept; always write the prefix.

LEAD 15. Code-path localization for #1 — see #1; the best 18 lines in the old file,
    previously filed under a *fixed* bug's heading.
LEAD 16. `become` scan-and-replace still misses live refs (JIT operands under
    materialization). `ObjectMemory.cpp:1625-1634` says so in the shipped
    comment; the forwarder that shipped is a safety net, not the fix.
LEAD 17. ~~Prim 145 pointer-fill branch writes fixed ivars~~ — FIXED 2026-08-12
    (`3c772997`).  Confirmed against `InterpreterPrimitives>>
    primitiveConstantFill` (cointerp-cpp.c:28585): stock fails unless
    `format >= sixtyFourBitIndexableFormat` and below the first
    CompiledMethod format, i.e. raw-data indexables only.  Our extra
    pointer-object branch filled `slotCount` slots from index 0, which for
    format 3 starts at the NAMED ivars.  Branch removed, Cog's format gate
    added up front.  Verified identical to Cog on `Bitmap>>primFill:`,
    `ByteArray>>atAllPut:`, `Array>>atAllPut:` (which now takes the
    Smalltalk fallback, as on Cog) and `Form>>fillColor:`.
LEAD 18. ~~`updatePointersAfterCompact` walks survivor space to `newSpaceEnd_`~~ —
    FIXED 2026-08-12 (`3c772997`).  There are no allocations in
    `[survivorStart_, newSpaceEnd_)` at all — scavenge tenures every
    reachable young object straight to old space and resets eden, and
    nothing writes there (`survivorStart_` is only ever read as the eden
    limit).  The walk parsed never-initialised pages as object headers and
    relied on the implausible-totalSize guard to stop, printing
    "[NS-SCAN-TERM] survivor" — a warning about young objects losing
    old-space updates in a region with no young objects.  Removed; the
    condition for restoring it (a copying survivor's live watermark, never
    `newSpaceEnd_`) is written at the site.  The eden scan already ends at
    `edenFree_`, a real watermark.
LEAD 19. CLOSED 2026-09-02 — the prevention did not work, and there is now an
    artifact.  The 2026-08-22 low-space circuit breaker (`22fcb0e7`) never
    fired on the first real storm we have a log for: arm64 sweep batch
    1901-1950 ran old space from 12 GB free down to 16 bytes with 33.4M
    Contexts / 6.66M Errors and printed no `[LOW-SPACE]` line at all.  The
    artifact this lead asked for is now in the repo:
    `docs/results/sweep-arm-2026-09-02/storm-heap-census.txt`.  Root cause is structural, not a
    tuning miss: the threshold was sampled on the interpreter's per-1024-
    bytecode checkpoint, but old space is consumed almost entirely by scavenge
    tenure, in steps of up to one eden (22 MB on that image).  Pharo arms the
    threshold at `lowSpaceThreshold` = 400000 bytes, so a step lands inside the
    observable window 400000/22003584 ≈ 1.8% of the time; the other 98.2% the
    next tenure overruns `oldSpaceEnd_` and aborts before any checkpoint runs.
    Fixed by latching the crossing at the two sites that advance
    `oldSpaceFree_` and consuming the latch at the checkpoint, with the
    effective threshold raised to max(image threshold, one eden) so the image
    is interrupted while one more worst-case scavenge can still be absorbed.
    Still to verify against a live storm — the reproduction is queued behind
    the x86_64 sweep.
LEAD 20. Six Win64 debug-gated helper call sites fixed by reasoning, never executed.
LEAD 21. Windows 7-8.4 s core-loop numbers never re-measured after `3940b62c`.
LEAD 22. SoundPlugin waveOut backend implemented, never runtime-verified.
LEAD 23. Inline NLR path still uses native ensure-hopping instead of
    `aboutToReturn:through:`; the context path uses the stock protocol.

## Corrections to earlier verdicts in this repo

Recording these because each cost time:

  * **"porpoise = upstream test-design GC race"** — it was our bug, fixed by
    `da9159e9`. Filed as somebody else's problem for a month.
  * **"Zero genuine unfixed VM bugs remain in the base harness"** — the same
    harness produces 93 errors from defect #1.
  * **"Exhaustive triage of the 25 catalog non-passes came up EMPTY"** and
    **"counter-probes CONFIRMED our VM matches stock on weak-clearing"** —
    both falsified repeatedly since. The probes passed because none drove the
    failing path. The bucket taxonomy is still useful; the verdicts are not.
  * **"restoreforpharo: FIXED"** — only true while the package could not reach
    SQLite. Now #7.
  * **"our VM can't do HTTPS"** — false since 2026-06-26 (`f26d45a2`,
    `3af37640`). This false premise is why the catalog-build path still depends
    on a stock Cog that segfaults.
  * **`MEM_RESET` recommended for madvise** — implementing it reintroduces the
    heap corruption `da5c4128` fixed. Deleted rather than carried forward.

---

## The 2026-06-01 investigation (historical record)

Kept for the method — the WideString, scavenge-root and Sista `^self`
bugs it chases are all fixed. Verdicts inside it that were later
overturned are listed under "Corrections" above.

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
    SystemEnvironmentTest   217P/0/0     217P/0/0    FIXED — our number is from
                                                     the 2026-09-02 arm64 sweep
                                                     and is now exact parity;
                                                     "79P/138E" below is 2026-06
    TraitTest               54P/0/0      errors      now defect #23; Cog number
                                                     re-confirmed 2026-09-02

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

Original cumulative-state symptom (CDNormalClassParserTest suite run 5x in one image):
  pre-fix : iter1-3 P16 F0, iter4 P15 F1, iter5 P14 F2 (degrades)
  post-fix: iter1-5 ALL P16 F0  (clean — both PHARO_NO_JIT=1 and full JIT+Sista)

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

BROADER IMPACT (2026-06-03).  The same fix dramatically improves other classes
where `^self` method bodies (prim 256 returnSelf or `^self`) were being
inlined at non-self send-sites.  Targeted 21-class re-run on /tmp/harness/Pharo.image
with the fix (JIT+Sista default):

    class                              pre-fix         post-fix       Cog baseline
    CDNormalClassParserTest            cumulative-F2   16/16 P        16/16 P
    SystemEnvironmentTest              79P/138E        199P/3F/15E    205P/3F/9E
    TraitTest                          (errors)        32P/1F/21E     40P/4F/10E
    RGMethodDefinitionTest             23P/8F/1E      23P/8F/1E      23P/8F/1E
                                       (separate JIT IC-probe bug;     (matches our
                                        unaffected by inliner fix)     post-fix —
                                                                       same issue
                                                                       on Cog too)
    all 17 other CD/RG/OC classes      mixed           clean (P==Total) clean
    --
    targeted batch totals              ~half passing   464 P / 12 F / 37 E / 513

The SystemEnvironmentTest jump (79→199 P) is the biggest single-class delta
and confirms the fix wasn't specific to the CD parser geometry — `^self` is
the most common method-body shape in Pharo (prim 256 returnSelf is the
default for many superclass methods that act as "do nothing" hooks; subclasses
override).  Any method that activates such an inherited `^self` through Sista's
inline path at a non-self send-site was miscompiled the same way.

Residual TraitTest delta vs Cog (32 vs 40 P) and RGMethodDefinitionTest at
23 P are PRE-EXISTING, NOT caused by the inliner fix (RGMethodDefinitionTest's
JIT IC-probe bug is documented in docs/jit-sunit-fullrun.md and fixed by
PHARO_T1_NO_IC_PROBE=1).


## 2026-08-23 — dead weak subscriptions: what is established, and what is not

`ReleaseTest>>testNoDeadSubscriptions` fails on both arches with four
`WeakAnnouncementSubscription`s whose subscriber is nil:

    ClassRemoved    WeakMessageSend(#classRemovedEventOccurs: -> nil)
    MethodRemoved   WeakMessageSend(#removedEventOccurs:      -> nil)
    MethodAdded     WeakMessageSend(#addedEventOccurs:        -> nil)
    MethodModified  WeakMessageSend(#modifiedEventOccurs:     -> nil)

Measured (arm64, `codeChangeAnnouncer subscriptions subscriptions`):

    after 3 x garbageCollect        total=54  dead=4
    after 3 more + 300ms delays     total=54  dead=4

So they survive six full GCs. **The weak clearing itself works** — the
subscriber slot IS nil. What never happens is the REMOVAL of the subscription
from the registry.

Mechanism as it stands in this image (Pharo 13):

  * The old `WeakArray` finalization API is gone: `WeakArray class` has zero
    selectors matching `inal`.
  * Pharo 13 uses `FinalizationProcess` (class-side only, `allInstances` = 0)
    with `#primitiveFetchMourner`, `#mournLoopWith:`,
    `#runningFinalizationProcess`, `#restartFinalizationProcess`.
  * `WeakMessageSend` is **weak but NOT an ephemeron** (`isWeak=true`,
    `isEphemeronClass=false`).
  * `WeakAnnouncementSubscription` implements `#finalize`.
  * `SubscriptionRegistry` has only `#remove:` and `#removeSubscriber:` — there
    is **no lazy self-cleaning path**, so removal must come from finalization
    actually invoking `#finalize`.

Our `drainMournQueueNatively` (`Interpreter.cpp:22894`) claims to re-push
non-WKA mourners — `WeakSubscription` is named in its comment — for image-side
dispatch via primitive 172, while dropping WeakArray mourners and handling WKAs
natively. Whether these four ever reach that queue is NOT yet measured.

**NOT established, and important before anyone calls this a VM defect:** the
test's own comment says upstream skipped it on Pharo's CI —
`https://github.com/pharo-project/pharo/issues/2471`. So it is a
known-troublesome test upstream, and this host has no runnable Cog to compare
against. Do not write this up as "our VM fails to finalize" without either a
stock-VM number or direct evidence that these four are enqueued as mourners and
dropped.

### MEASURED: our finalization chain works end to end, so this is not it

Ran the 13 s reproducer under `PHARO_GC_EPH_DEBUG=1` (34 tests, 32 P, 1 F,
1 S — the documented signature). The existing `[DRAIN-n]` and `[POP-FIN]`
diagnostics answer the question without new instrumentation:

    [DRAIN-1] initial=11 processed=11 wka=2  wkaarr-drop=1 kept=8
    [DRAIN-2] initial=3  processed=3  wka=0  wkaarr-drop=3 kept=0
    [DRAIN-3] initial=14 processed=14 wka=12 wkaarr-drop=2 kept=0
    [DRAIN-4] initial=6  processed=6  wka=0  wkaarr-drop=0 kept=6
    [DRAIN-5] initial=1  processed=1  wka=0  wkaarr-drop=1 kept=0

    [POP-FIN] x17, signal->pop latency=0ms

So mourners ARE enqueued, the finalization semaphore IS signalled, and the
image's `FinalizationProcess` IS fetching them — 17 pops at 0 ms latency. The
process itself is alive (`runningFinalizationProcess` answers `a Process in
FinalizationProcess class>>finalizationProcess`).

The primitive wiring is correct too, which is worth recording because the
header comment is misleading: `Interpreter.hpp:2816` annotates
`primitiveSetGCSemaphore` with `// 172`, but the generated table
(`src/ios/generated_primitives.inc:180`) maps
`primitiveTable_[172] = &Interpreter::primitiveFetchNextMourner`, matching the
reference (`cointerp-cpp.c:2277`, `/* 172 */ primitiveFetchNextMourner`) and
matching what the image calls (`<primitive: 172 error: 'ec'>`). **That stale
`// 172` comment should be fixed or dropped — it invites exactly the wrong
conclusion.**

**Conclusion: the four dead subscriptions are never enqueued as mourners at
all**, which follows from `WeakMessageSend` being weak but NOT an ephemeron —
the GC nils its receiver slot and that is the end of it; nothing asks the
announcer to drop the subscription. Combined with upstream skipping this test
on their own CI (pharo#2471), the evidence does not support calling this a VM
defect, and it should not be counted against the VM without a stock-Cog
comparison this host cannot run.
