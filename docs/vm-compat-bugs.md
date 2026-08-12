# VM-compatibility bugs (our VM fails, Cog passes)

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

## OPEN DEFECTS (as of 2026-08-12)

**Update 2026-08-12 (later session): `#1` and `#5b` are FIXED.**  `#1` was the
top open VM bug and was NOT where six sessions had it filed — see its entry.
Ten remain: `#2` (partially), `#3`, `#4`, `#6`-`#12`, `#14`.

Provenance: these were extracted from `docs/deferred.md`, which had grown to
2739 lines of which 86% was changelog and lab notebook. Five of them had been
invisible because they sat *inside* entries whose parent was checked `- [x]`.

### 1. ~~WarpBlt expression-stack displacement~~ — FIXED 2026-08-12 (`bf3f6c58`)

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

SUITE-LEVEL CONFIRMATION (macOS-arm64, COMPLETE — 2046 classes):

                        this run          2026-08-11 baseline (HEAD, same box)
    pass                27729             27701
    fail                   24                22
    error                  20                50
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
    j-brant-smacc                  cog 557 P in 30 s   ours hangs to 1800 s

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
    smalltalkweb-myprecious     cog 107 P    ours exits 0 after CLASSES

`soil` aborting is new information — `rc=134` is `abort()`, so an assertion or
an uncaught C++ exception, distinct from mutalk's segfault. Both reproduce with
`PHARO_NO_JIT=1` (mutalk stops segfaulting but still produces no RESULT).

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
    (131072 Oops against 56000 frames is barely 2 slots per frame).  Adding
    the same bound there and returning `ExitStackOverflow` (`86260b47`) is
    correct but unusable as it stands: mutalk went from 5,439,488 sends to
    **>2.8 billion and still climbing**, i.e. the bail is retried without
    forward progress instead of converting into a Smalltalk stack-overflow
    signal.  Reverted in `959da174`, with the gap and the requirement written
    at the site — closing it means making `ExitStackOverflow` raise the
    image-side signal (or unwind) rather than bounce back into the same J2J
    call.  Until then a deep enough JIT-to-JIT chain can still overrun.

Explains `PHARO_NO_JIT=1` exiting 0 in the original triage.

Note the sizing invariant this exposed: `MaxStackDepth = 131072` carries the
comment "must be large enough for MaxFrameDepth frames" and
`MaxFrameDepth = 65536` — exactly 2 Oops per frame, which only holds for a
zero-temp method with an empty operand stack.  The soft frame limit
(`StackOverflowLimit = 56000`) is what actually keeps it honest, and the JIT
was not honouring the operand-stack side of it at all.

Canaries placed immediately after `stack_` and `savedFrames_` did NOT catch
this (they are kept, and that negative is why): the JIT writes individual
slots at computed offsets rather than sweeping, so it stepped over the canary
word.  That is what pointed at a wild write and then at ASan.

STILL OPEN: the same package produces no `RESULT` line — the eval's own
startup.st IS consumed (the "eval never ran" guard does not fire), so the
script runs and the process dies partway through the tests.  Cog:
pass=336 err=1.  Localize that process death next (a `[TERM-P*]` trace, or
the runner's own error path).

### 4. `fedeloch-ume` — ROOT-CAUSED 2026-08-12: the image is MULTI-SEGMENT — HIGH

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
  * `loadHeapData` REFUSES a multi-segment image with a named error rather
    than relocating half a heap and handing the GC a live mine (`73565806`).
    Single-segment images are unaffected — control verified.

Also settled by measurement, which the previous session flagged as unchecked:
**the `[IMGLOAD-DECLINE]` lines are false positives.**  New per-format
attribution (`[IMGLOAD-DECLINE-BY-FORMAT]`): of 49,831 declined old-base
pointers on this image, all 49,831 are in **format 9** and ZERO in pointer
formats 0-5.  Format 9 is a 64-bit WORD array that `hasPointers` includes for
hiddenRoots' sake, so those are numeric data misread as pointers — lead 2,
confirmed.  `[IMGLOAD-CLAMP]` does not fire on this image either.

STILL OPEN — real multi-segment support.  Two parts, written down so they are
not re-derived: walk the bridges, and relocate **per segment**, because each
segment carries its own saved start address and the single
`newBase - oldBase` delta this loader applies is only correct for the first.
The lead-2 hazard is also still open in its own right: format-9 data that
lands INSIDE the old-base window is silently RELOCATED, i.e. mangled.

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
    mumez-pharo-acp              true          pass=168 err=2             pass=161 err=9  FIXED, 7-test residual
    fedeloch-ume                 true          pass=11                    -               STILL FAILS
    moosetechnology-gitprojecthealth false     pass=6                     -               STILL FAILS
    tomooda-viennatalk           false         pass=1430                  -               STILL FAILS
    evref-bl-mcp                 -             (load failed locally: network, not a VM issue)

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

`src/platform/win_mman.h:68` uses `MEM_RESERVE|MEM_COMMIT` on the full mapping,
so startup fails outright on a machine with under ~4 GB of commit headroom. The
2026-07-04 design note (commit-ahead in the allocation slow path) is sound and
unimplemented — no `committedEnd_` symbol exists.

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

## LEADS (real work, not yet a filed defect)

15. Code-path localization for #1 — see #1; the best 18 lines in the old file,
    previously filed under a *fixed* bug's heading.
16. `become` scan-and-replace still misses live refs (JIT operands under
    materialization). `ObjectMemory.cpp:1625-1634` says so in the shipped
    comment; the forwarder that shipped is a safety net, not the fix.
17. ~~Prim 145 pointer-fill branch writes fixed ivars~~ — FIXED 2026-08-12
    (`3c772997`).  Confirmed against `InterpreterPrimitives>>
    primitiveConstantFill` (cointerp-cpp.c:28585): stock fails unless
    `format >= sixtyFourBitIndexableFormat` and below the first
    CompiledMethod format, i.e. raw-data indexables only.  Our extra
    pointer-object branch filled `slotCount` slots from index 0, which for
    format 3 starts at the NAMED ivars.  Branch removed, Cog's format gate
    added up front.  Verified identical to Cog on `Bitmap>>primFill:`,
    `ByteArray>>atAllPut:`, `Array>>atAllPut:` (which now takes the
    Smalltalk fallback, as on Cog) and `Form>>fillColor:`.
18. ~~`updatePointersAfterCompact` walks survivor space to `newSpaceEnd_`~~ —
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
19. ARM "context storm" prevention was proven on a constructed analogue, never
    on the storm. Catalog #10 is cited twice as proof and has no artifact in
    the repo — find the log or stop citing it.
20. Six Win64 debug-gated helper call sites fixed by reasoning, never executed.
21. Windows 7-8.4 s core-loop numbers never re-measured after `3940b62c`.
22. SoundPlugin waveOut backend implemented, never runtime-verified.
23. Inline NLR path still uses native ensure-hopping instead of
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

