# WIP — latest session 2026-08-12b (two HIGH defects root-caused and fixed)

## ===== 2026-08-12b SESSION (read this first) =====

Two of the twelve open defects in `docs/vm-compat-bugs.md` are FIXED, and
both root causes were somewhere nobody had looked.

### `#1` WarpBlt expression-stack displacement — FIXED `bf3f6c58`

The top open VM bug, 93 of the 102 ERRORs in the last full ARM suite, filed
as a materialize/restore defect since 2026-07-07.  **It is the per-bytecode
Sista backward-jump tier.**

    PHARO_DET_SCHED=1 build/test_load_image <image> eval \
      "$(cat scripts/repro/warpblt_temp_displacement.st)"

    before   ~796 calls / 12 errors, every run
    after    1440 calls /  0 errors — byte-identical to stock Cog,
             JIT on AND PHARO_NO_JIT=1

A region lifted from a backward-jump target is registered at every interior
loop top it built a dispatch arm for.  Entering at an interior arm needs the
target block's phi inputs, so a loader pseudo-block reads them off the
interpreter's operand stack with `kLoadStackSlot` — and never lowered
`state.sp`.  So each phi operand was live twice: in the region's registers,
which the next bail writes back at `state.sp`, and in the copy still below
it.  One phantom operand per phi, exactly the +1 / +2 measured.  Temps stay
correct, which is why the localizer said "expression stack" and stopped.

Fixed in BOTH backends — the whole-file per-arch split had the identical
omission for the fourth time (memory `per-arch-backend-drift`).

**`PHARO_NO_JIT=1` does not disable this tier.**  That single fact is why
five sessions concluded "interpreter-side, not the JIT".  The isolating knob
is `PHARO_NO_SISTA_PER_BC=1`.

### `#5b` `saveAs:` freezes the eval result — FIXED `e00f0acb`

Not snapshot/resume, and not "the startup sequence is never invoked".  Every
step of the resumed image's startup runs correctly — `isImageStarting=true`,
the startup list byte-identical to a fresh boot, the deferred action queued
AND executed.  `BasicCommandLineHandler>>activate` then **forks** the actual
dispatch at P40, and the continuation the image resumed into (the previous
command's script, boosted to P80) reaches its own `Smalltalk exitSuccess`
first.  Proof it is only CPU: an 8 s Delay in the old continuation makes the
UNFIXED VM answer correctly.  primitiveQuit now defers a quit raised while
this run's `startup.st` is still on disk (our script deletes itself first,
so such a quit cannot be ours), one-shot.

### The instrument, kept: `PHARO_DEPTH_ORACLE=<exact selector>`

Records the operand depth the FIRST time each bytecode offset runs and
reports any later visit that disagrees — no static analysis needed, because
loop and straight-line code has a fixed depth per pc.  It fires at the first
displaced bytecode instead of the impossible-receiver DNU downstream, and
dumps the live frame, the owning context, a 48-entry event ring
(restore/save/return/push/pop/materialize/transfer/Sista) and a 160-entry
(offset, depth, bytecode, ctx, fp) ring.  Those rings turned "displaced
somewhere" into `SISTA-IN entry=340 depth=25 -> SISTA-OUT bail=640 depth=28`
in one run.  `[SISTA-LIFT-BAIL]`/`[SISTA-LIFT-POP]` under
`PHARO_SISTA_BJ_TRACE` show a region's bail offsets at lift time.

### Ruled out along the way, each measured, each 12 errors

`PHARO_NO_CTX_STACKP_RAISE=1`, `PHARO_CTX_TRACE_ALL_SLOTS=1`, the
return-value placement gap, the ctxSynced skip, preemption frequency.
`PHARO_NO_FRAME0_REUSE=1` is broken on its own (no output) — not a usable
bisect axis.

### Also fixed: LEADS 17 and 18 (`3c772997`), both by reading

`primitiveConstantFill` (145) carried a pointer-object branch that filled
`slotCount` slots from index 0 — for format 3 that starts at the NAMED
instance variables.  Stock Cog fails the primitive for anything that is not
a raw-data indexable, so this was a silent whole-object clobber our VM could
produce and Cog could not.  And `updatePointersAfterCompact` walked
`[survivorStart_, newSpaceEnd_)`, a region with NO allocations in this VM
(scavenge tenures straight to old space), parsing uninitialised pages as
object headers.

### Both backends VERIFIED, not just written

The `#1` fix was applied to `SistaLowering_arm64.cpp` and
`SistaLowering_x86_64.cpp`.  Both were then run: arm64 native and x86_64
through `build-x86` under Rosetta each give **1440 calls / 0 errors** on the
repro, and `test_sista_ir` (which has a multi-entry dispatch round-trip
case) PASSes in both trees.  The per-bc tier is still fully active after the
fix — 78,848 dispatches on the repro — so this corrected its bails rather
than disabling it.

### `#4` fedeloch-ume — ROOT-CAUSED: the image is MULTI-SEGMENT

    firstSegmentBytes = 75,825,152   of   imageBytes = 149,692,728

`37eeb743` said multi-segment and `38e3c050` retracted it.  **The retraction
was wrong**, and it is worth knowing why: it argued the file is exactly
`headerSize + imageBytes` with nothing past the declared data.  Segments live
INSIDE imageBytes — each but the last terminated by a bridge — so that test
cannot see them.  `firstSegmentBytes` is the field that answers it.

Chain, all measured on a locally loaded Ume image:

    firstSegmentBytes < imageBytes     segments exist
    walk mis-parses at a bridge        [IMGLOAD-WALK-MISALIGN] +0x584fcc0,
                                       0xff00000000000066 / 0xfffc30 /
                                       0xff000000000078cf — two 0xFF words
                                       16 bytes apart at a delta-0 boundary
    walk stops (was a bare `break`)    [IMGLOAD-WALK-TRUNC] 38.14% never
                                       relocated
    those objects keep saved pointers  [WEAK-BAD-REF] WeakValueAssociation
                                       slot 1 = 0x10000000000
    the GC dereferences one            SIGSEGV in markPhase (lldb:
                                       `ldrb w8,[x24,#6]`, x24 = old base)

Fixed on the way, both independent of segment support: `processWeaklings`
now bounds-checks weak referents and ephemeron keys through the same
predicate `markAndTrace` uses (one bad slot used to kill the VM), and
`loadHeapData` refuses a multi-segment image with a named error instead of
relocating half a heap.  Our own `saveAs:` images are single-segment and
round-trip unaffected — verified.

Also settled: the `[IMGLOAD-DECLINE]` lines the previous session built the
theory on are FALSE POSITIVES.  Per-format attribution says all 49,831 are
format 9 (64-bit word arrays that `hasPointers` includes for hiddenRoots),
ZERO in pointer formats.  Lead 2 confirmed by measurement.

### Full-suite validation of `#1` (macOS-arm64, COMPLETE — 2046 classes)

                    this run     2026-08-11 baseline (HEAD, same box)
    pass            27729        27701
    fail               24           22
    error              20           50

    Cly* classes with any F or E:  NONE

ERROR 50 -> 20; the 33 `SmallInteger >> #pixelAt:` errors this defect owned
(`ClyBrowserToolValidityTest` 25, `ClyNotebookPageRecyclerTest` 8) are gone.
FAIL moved by one, inside the documented noise floor.  Every remaining
non-clean class is in a known residual family.

### `#3` mutalk — crash FIXED `57022d3a`; the lost RESULT is a separate, still-open failure

`push()` bounds-checks the operand stack; the JIT writes it through
`state.sp` and does not.  Deep recursion (`#recursiveFactorial:` at 130,881
of 131,072 slots) therefore wrote past the end of `stack_` into
`std::vector<void*> stackPushReturnAddr_`, 48 bytes later, and ~Interpreter
aborted freeing it.  Found with ASan (which named the member and showed the
"pointers" were consecutive SmallInteger Oops) plus a hardware watchpoint
(whose frame #0 had no symbol — JIT code — under tryJITActivation).
tryJITActivation now declines within StackSafetyZone of the limit and falls
back to the interpreter.  exit 134 -> exit 0.

The J2J path (`pushFrameForJIT`) has the same hole and is NOT fixed.  I first
claimed adding the bound there caused a 500x blowup (5.4M sends -> 2.8B);
**that claim was wrong and is withdrawn** — the comparison had a full SUnit
suite running concurrently for one arm, and mutalk's send count is not stable
run to run (the reverted binary reproduces the same "blowup" on a quiet
machine).  Re-measured on tinyBenchmarks, 3 runs each: 468/466/473 vs
432/447/457 Mbytecodes/sec — about 2-3%.  Left out for that honest reason plus
"no demonstrated case the entry guard does not already cover", not for the
fictional one.

### NEW defect `#15`, found by accident: recursion past ~56,000 frames HANGS

    deepRec: 56000    ours 56000 in 2 s      Cog 56000
    deepRec: 60000    ours HANGS             Cog 60000
    deepRec: 100000   ours HANGS             Cog 100000 in 0.10 s

Three lines, same image, no Morphic (so the eval-preamble caveat does not
apply).  The cliff is exactly `StackOverflowLimit = 56000`:

    [OVERFLOW] fd=56024 pushing #deepRec:
    [OVERFLOW] driving Process>>terminate to unwind (fd=56024)

and then no further progress.  Two problems: the cap itself (Cog's stack
grows — 100,000 frames is nothing to it), and the overflow handling failing to
make progress.  Since the recovery is `Process>>terminate` rather than an
Error, image-side `on: Error do:` cannot catch it either.

I claimed this explains `#3`'s missing RESULT.  **Checked, and it does not** —
zero `[OVERFLOW]` lines in the mutalk runs; that one fills the OPERAND stack
(131072 slots) while frames stay under 56000, a different limit.  Withdrawn.
Still untested and still plausible: the "hangs to 1800 s" packages in
`#2a`/`#2b` — famix, viennatalk, gitprojecthealth, and `j-brant-smacc`
(a recursive-descent parser, the best candidate).

It surfaced because a probe written to test whether a JIT stack bound
livelocked hung in BOTH arms — the arm that was supposed to be the control.

### Validation status of the day's LATER changes — NOT established

The full suite that validated `#1` (27729 P / 24 F / 20 E, complete) predates
`#5b`, prim 145, the survivor-scan removal, the weak-ref guard, the JIT
headroom check and the canaries.  The re-run with all of them WEDGED at
`TraitTestCase` (1962 of 2052 classes, 27010 P / 22 F / 21 E — tracking the
first run almost exactly up to that point).

What the wedge is, measured rather than assumed:

  * lldb on the live process: thread 1 parked in
    `primitiveRelinquishProcessor` — the idle path, i.e. the image has NO
    runnable process.  1.9% CPU, not a spin.
  * NONE of the day's new diagnostics fired: zero `JIT-NO-HEADROOM`, zero
    `OVERFLOW`, zero `WEAK-BAD-REF`, zero `ARRAY-OVERRUN`, zero
    `[EVAL] deferring`.
  * The Trait classes run CLEAN in isolation on the same binary —
    TraitTestCase 19/19, TraitTest 54/54, and three siblings, 93 tests,
    0 F / 0 E.

So it is suite-context, and it matches the documented wedge family (memory
`stdebugger-ticker-death-wedge`, `idle-band-scheduler-starvation`) rather than
anything this session added.  **But one run cannot separate "known flaky
wedge" from "new regression"**, so the later changes are NOT validated at
full-suite scale.  A re-run is in flight; treat that as the outstanding item.

### Open

`#4` is root-caused with the remaining work specified (bridge walk +
per-segment relocation delta).  Nine others remain: `#2` (partially), `#3`,
`#6`-`#12`, `#14`.
A full 2055-class macOS-arm64 suite is the outstanding validation for `#1`;
the pre-fix baselines to compare against are in the 2026-08-11 section below
(27701 P / 22 F / 12 T / 50 E, of which ~25 Cly errors were this defect).

## ===== 2026-08-12 (earlier session) =====

## ===== 2026-08-12 SESSION (read this first) =====

Worked `docs/vm-compat-bugs.md`, which the previous session split out of the
2,739-line `deferred.md`.  **Two real VM fixes landed; twelve defects remain
open.**  Read the retractions section — four conclusions were committed and then
withdrawn today, and knowing which is worth more than the fixes.

### Fixed

**`9d9f8154` — `UnixOSProcessPlugin` signal forwarding (the session's real fix).**
OSSubprocess registers `OSSVMProcess>>initializeChildWatcher` as a SESSION
STARTUP handler, which does `self sigChldSemaphore waitTimeoutMilliseconds: 1000`.
That resolves through `primSigChldNumber`
(`<primitive: 'primitiveSigChldNumber' module: 'UnixOSProcessPlugin'>`), which we
did not implement, so it failed to nil, `forwardSignal:` answered nil, and the
image sent `#waitTimeoutMilliseconds:` TO NIL.  The DNU killed the startup
process, `StartupPreferencesLoader` never ran, and the image reported the
PREVIOUS eval's result — which is what looked like a "frozen eval".
Implemented `primitiveSigChldNumber` + `primitiveForwardSignalToSemaphore`,
purely additively, delivering through the existing MPSC signal ring
(async-signal-safe, `SA_RESTART`).

    pillar-markup-pillar   nothing -> pass=18 fail=0 err=9   BYTE-IDENTICAL to Cog
    mumez-pharo-acp        nothing -> pass=161 fail=0 err=9  (Cog 168/2; 7 residual)

No regressions: porpoise 14 P / 0 F, context/closure/exception/process/weak/
finalization family 1042 tests / 995 P / 0 F.

**`#13` closed as stale.** The entry named a selector that does not exist
(`testEventAfterProceed:`); the real one passes on arm at HEAD and appears in
neither recent full suite's FAIL list.

### Retracted today — read this before trusting anything above it

1. **"snapshot/resume is the fix site"** — it is not.  A working image and a
   frozen one resume IDENTICALLY (same 9-frame chain, same patches).
   Instrumenting before editing is the only reason the fix landed in an
   additive plugin instead of in snapshot/resume.
2. **"eight defects, one root cause"** — wrong.  `#5b` (plain `saveAs:` on a
   base image) still freezes WITH the plugin fix in.  It is a separate defect.
3. **"ume belongs to #2a"** — wrong.  It SIGSEGVs in `Interpreter::initialize`
   before the resume path; restored as its own `#4`.
4. **"`#4` is a MULTI-SEGMENT image"** — wrong, and committed as ROOT CAUSE
   before being withdrawn one message later.  The file is exactly
   `headerSize + imageBytes`, ZERO bytes past the declared data.  There is no
   second segment.  The `[IMGLOAD-DECLINE]` lines are probably false positives
   from format-9 word arrays relocated as pointers, and **I never verified that
   the failing `activeProcess` value is among the declined ones** — that
   unchecked step is what the whole theory rested on.

### State of the group that #5/#2a used to be

    pillar             OSSVMProcess=true   FIXED (identical to Cog)
    mumez-pharo-acp    OSSVMProcess=true   FIXED (7-test residual)
    fedeloch-ume       OSSVMProcess=true   OPEN — #4, SIGSEGV in initialize
    gitprojecthealth   OSSVMProcess=false  OPEN — third mechanism, undiagnosed
    tomooda-viennatalk OSSVMProcess=false  OPEN — third mechanism, undiagnosed
    moosetechnology-famix OSSVMProcess=false OPEN — third mechanism, undiagnosed
    evref-bl-mcp       load fails locally (network, not a VM issue)

### Diagnostics added (all gated, all kept)

    PHARO_TRACE_DNU_STACK=<sel>   dump the LIVE frame's operand stack at a DNU —
                                  the image cannot show this; it is what proved
                                  the materialized context disagrees with the frame
    [IMGLOAD-CLAMP]               ImageLoader's silent slotCount truncation now reports
                                  (ruled the clamp OUT for #4 — it never fires)
    [IMGLOAD-DECLINE]             relocatePointer silently declining an old-base
                                  pointer now reports
    [STARTUP] scheduler chain broken at '<link>'
                                  names the bad link instead of segfaulting
    [MAT-SAVE*/MAT-RESTORE]       paired context save/restore depths + ctx identity

### `#1` WarpBlt — narrowed, not solved

Deterministic 4-min repro (`scripts/repro/warpblt_temp_displacement.st` under
`PHARO_DET_SCHED=1`, 12 errors; stock Cog 1440/0) and now wired into CI.
Eliminated by measurement: concurrency IS required (single-process 240/0 on both
VMs), NOT preemption-frequency (12 errors at quantum 1/2/4/8), NOT the ctxSynced
skip, NOT the JIT, NOT any of the three July candidates, NOT the return-value
placement gap (built the fix, 12 errors before and after, reverted).
Strongest lead: 555 contexts each restored many times against <=1 save, 123 never
saved at all.

### Honest status

TWELVE defects remain open: `#1`, `#2` (partially), `#3`, `#4`, `#5b`, `#6`-`#12`,
`#14`.  Every one carries a repro, a ruled-out list and a named next step.

`#4`'s next step is ONE grep: does `0x10008f709a8` appear in the
`[IMGLOAD-DECLINE]` output?  Then check whether format 9 is producing false
positives.  Five minutes for someone who can verify their own work.

I stopped because four conclusions were retracted today, two of them in
consecutive messages, and the remaining defects live in image loading and frame
materialization where a wrong change corrupts heaps silently rather than
failing a test.  At that error rate more output is net-negative.

## ===== 2026-08-11 CLOSE-OUT SESSION =====

## ===== 2026-08-11 CLOSE-OUT SESSION (read this first) =====

Goal: finish handling today's x86 and arm AWS test runs.  Both of the previous
session's open findings (section H below) are now ROOT-CAUSED AND FIXED, and
both root causes were different from what H predicted.

    2nd weak-ref path (porpoise)     FIXED  da9159e9 + d209543d
    sqlite3 FFI Module-not-found     FIXED  4a46413f + 919904cd
                                     (c1d6eef7 was NOT it — measured, see 2)

Verified END-TO-END on a fresh aarch64 Linux box, both packages now byte-equal
to the stock-Cog arm:

    package                     cog                       jit          jit-only
    rko281-porpoise             pass=14  fail=0 err=0     pass=14  0/0     0
    pharo-rdbms-pharo-sqlite3   pass=122 fail=0 err=0     pass=122 0/0     0
                                                    (was pass=11 err=111)

Full write-up: `docs/aws-closeout-2026-08-11.md`.
Box: c7g.4xlarge on-demand, us-east-2 (2nd box `i-0b676d684313167b7`; the first
was reaped mid-run, see 5).

**Re-sweep: ZERO JIT-only failures across 200 packages** (was two real findings
pre-fix).  `docs/results/arm-closeout-2026-08-11-summary.tsv`.  Eleven rows
carry a non-zero flag; reading the counters, every one is a test that passes on
Cog and TIMES OUT on ours, a row with byte-identical counters
(`mumez-redistick`), or a row where neither arm produced a result.

**Full suites, 2055 classes each — including a same-machine A/B against the
commit before everything in this session:**

    macOS-arm64  HEAD                27701 P / 22 F / 12 T /  50 E
    macOS-arm64  361bf92b (pre-fix)  27700 P / 22 F / 12 T /  51 E
    Linux-aarch64 HEAD               27647 P / 24 F / 13 T / 102 E
    (morning arm, pre-session)       27752 P / 19 F / 14 T /   1 E

The macOS pair is the regression result that matters: one pass and one error
apart, identical FAIL and TIMEOUT counts, so the six commits are regression-free
at full-suite scale.  Both members of that pair carry the same 25
`ClyBrowserToolValidityTest` errors, which is the pre-existing WarpBlt defect in
section 8 — and the reason the Linux ERROR column reads 102 against the
morning's 1 is the bimodal cache described there, not a code change.

Every FAIL on both is in a documented residual family — `ReleaseTest`
run-order pollution, `StDebugger`/`StSpotter`, TKT, ZnClient — the same shape
as the pre-fix run's 19.  The ERROR column is the one thing that moved, and it
is the GUI/Form family in section 8, which is measured NOT to be this
session's work.

AWS: both boxes terminated, security group / IAM / deploy key removed, lease
released.  Nothing left running.

**Cost of the GC change: none measurable.** Bounded vs `PHARO_CTX_TRACE_ALL_SLOTS=1`,
3 runs each, seconds:

    gc-under-deep-stack   5.35 5.67 5.79   vs   5.93 5.92 6.49
    materialize-churn     3.66 3.85 3.92   vs   3.94 4.00 4.30
    alloc-collect (ctrl)  3.15 3.25 3.32   vs   3.37 3.41 3.59

The control moves by as much as the two GC benchmarks, so: no regression, a
small win at best.  Expected — the bounded trace scans strictly fewer slots.

### 6. A false alarm worth the paragraph

Three packages in the re-sweep SIGSEGV'd, two of which (`moosetechnology-fast`,
`-fast-java`) had scored identical-to-Cog the day before.  That is the exact
shape of a GC regression from the trace bound, and it is what the old
whole-array scan existed to prevent.  It was not one: `sudo dmesg` shows a
single `test_load_image invoked oom-killer` event where a stock-Cog process had
reached **30 GB total-vm on a 32 GB box**, and whichever VMs were mid-GC at that
instant faulted in `processWeaklings`.  Re-run standalone all three are clean
and unchanged, under default / `PHARO_CTX_TRACE_ALL_SLOTS=1` / `PHARO_NO_JIT=1`
alike.  The runner now records each arm's exit status (`137` = SIGKILL, `124` =
budget, `139` = SIGSEGV) so nothing has to be reconstructed from a log again.

### 7. Three new findings, from a bucket nobody had opened

The arm document left "the 15 cog-ran/jit-did-not packages are unexamined".
`scripts/pkg-jit-test/classify-missing-jit.py` (new) examines them, and one is a
genuine defect: **`pharo-contributions-mutalk` SIGSEGVs in `~Interpreter()`**
(`__libc_free`, fault addr `0xfffffffffffaa871`), identically in BOTH sweeps, so
pre-existing and not OOM collateral.  Reproduces standalone and is already
bisected one step: `PHARO_CTX_TRACE_ALL_SLOTS=1` crashes identically (so not the
GC change) and `PHARO_NO_JIT=1` does not crash (so the JIT is required).

`fedeloch-ume` SIGSEGVs too, at the same address under default,
`PHARO_CTX_TRACE_ALL_SLOTS=1` AND `PHARO_NO_JIT=1`, before the runner prints
`PREFIXES` — so neither the JIT nor the GC, and it should be the cheapest of
the three to localize.  It reaches the VM at all only because the package now
loads.

`rko281-restoreforpharo` is a PERF finding: cog runs 2354 tests in 145 s, ours
did not finish in 7200 s.  Before the FFI fix all 4712 of its tests errored
instantly, so this cost was invisible.  Three JIT-only failures in one family
appeared in the part that did run.

All four are in `docs/deferred.md` with repros.

### 8. The GUI errors: a real VM bug, now with a 4-minute deterministic repro

This took four wrong readings before it came out right; the wrong ones are in
`docs/deferred.md` so nobody repeats them.  Final state:

`pixelAt:` is a **BitBlt** message, not a Form/Bitmap one, so "a Form's bits is
an Array" was never the right reading.  Every failing receiver is a sibling
temp of ONE activation, `WarpBlt>>warpBitsSmoothing:sourceMap:`.  Our
`primitiveWarpBits` bails for non-32-bit depths, so the image drops into a
Smalltalk fallback stock Cog never executes — and `UITheme>>formSetsForScale:`
is `at:ifAbsentPut:`, which caches nothing on failure, so one early failure
retries forever and one early success warms the cache forever.  **That is why
the count is 1 or 102 for a fixed defect**, and why the morning-vs-evening
comparison I built a conclusion on was noise.  That conclusion is withdrawn.

    stock Cog                   1440 calls   0 errors
    ours 361bf92b, DET_SCHED    ~797 calls  12 errors
    ours HEAD,     DET_SCHED    ~795 calls  12 errors

`PHARO_DET_SCHED=1` is what makes it deterministic — without it the probe
passes 1440/1440 on a quiet machine and fails only under load, which is why the
early attempts contradicted each other.  Identical on both builds: pre-existing,
not this session's, not JIT-dependent.

The localizer settles the mechanism: every temp is CORRECT (`picker=BitBlt`,
`poker=BitBlt`, `pix=Array`) while the failing sends are `BitBlt >> #+` / `#//`
at stable pcs — so the **expression stack** is displaced and the temps are not.
That is the materialize/restore path.  Probes: `scripts/repro/warpblt_*.st`.

### 9. Two real defects in this session's own commits, found by review

Neither was caught by any test; an adversarial re-read of the diffs found them.

  - `d209543d`'s message claims it routed the third context-reuse path
    (`frame[0] == activeContext_`) through `storeContextStackp`.  It did not —
    the hunk landed on the `thisContext` re-sync and that site kept a raw
    `storePointer` that lowered stackp with a dirty tail.
  - `89942e89` narrowed the tail clear on the premise that everything above the
    previous stackp was already nil.  False: `primitiveSetStackPointer` nils
    only when GROWING, `primitiveContextAtPut` writes any slot bounded only by
    `slotCount`, and the site above.  Reverted to clearing the whole tail.

Both matter *because* `pointerSlotsOf` now stops at stackp — residue above it is
neither marked nor relocated.  Fixed in `de666ee7`.

### 10. One regression that is NOT ours, proven rather than argued

`ClyBrowserToolValidityTest` (25) and `ClyNotebookPageRecyclerTest` (8) passed
in the morning's arm full run and ERROR in both evening full runs, one
signature: `MessageNotUnderstood: SmallInteger >> #pixelAt:`.  That is display
state, but "it looks like display state" is not evidence, so it was measured: a
full macOS-arm64 suite with `PHARO_CTX_TRACE_ALL_SLOTS=1` — the GC change
reverted — reproduces it exactly (26773 P / 21 F / 55 E, same 28 Cly errors).
`da9159e9`/`d209543d` are ruled out.  The LD_LIBRARY_PATH work is
`#if defined(__linux__)` and cannot explain a macOS repro either.  The
remaining in-window commit active on macOS is `c1d6eef7`'s shared
`ffi::moduleCandidates`.  Open in `docs/deferred.md` with the bisect written
down so the next session starts from the answer, not the question.

OPEN, CREATED BY THE FIX: `rko281-restoreforpharo` now genuinely RUNS its 2354
tests instead of erroring out of all 4712 in seconds, and no longer fits the
harness's 900 s per-package budget (cog finishes in ~2 min; ours had not
finished at 15).  Needs a single long-timeout re-run to get a real A/B number —
it is a speed gap on freshly-reachable FFI code, not a correctness result.

### 1. `da9159e9` — a Context's dead stack residue was a GC root

H guessed "Got 1 instead of 2 means something is MISSING from allInstances".
Wrong: the failing assertion is the SECOND one, and the test's own `count` temp
reads 1 there — an instance from an EARLIER test was still alive when this test
took its baseline, and the GC under test finally reclaimed it.  Full chain,
from the probes:

    PropertyManagerTestObject
      <- Context(...)[8]{stackp=2 liveSlots=6..7 slot=DEAD-RESIDUE
                         method=testPropertyManagerKeyWeakness}
      <- ROOT(activeContext_)

`ObjectMemory::pointerSlotsOf` traced a Context's WHOLE slot array; Spur's
`numPointerSlotsOf:` stops at `CtxtTempFrameStart + stackp`.  So expression-
stack residue left by a RETURNED activation is a root here and invisible on
Cog.  Now bounded at stackp (`PHARO_CTX_TRACE_ALL_SLOTS=1` reverts), with the
two conditions that motivated the full scan handled: `prepareForGC`'s temp
syncs already RAISE stackp to cover what they write, and the new
`storeContextStackp` nils the tail whenever materializeFrameStack LOWERS
stackp on a reused context.

Local regression so far: 822 P / 0 F / 0 E over the weak/GC/context/process
class family; full-suite runs in flight on both macOS-arm64 and Linux-arm64.

### 2. `4a46413f` — the image saw no arch library directory

`c1d6eef7` (previous session) fixed `primitiveLoadModule`'s candidate list
"by construction" and could not be verified on Linux.  Verified now on a fresh
aarch64 box: **unchanged, still err=111.**  The candidate list was never
reached, because the package computes the path ITSELF and gives up before
calling the VM:

    SQLite3Library>>unix64LibraryName
        (#('/usr/lib/x86_64-linux-gnu' '/lib/x86_64-linux-gnu' '/usr/lib64' '/usr/lib'),
         ((OSEnvironment current at: 'LD_LIBRARY_PATH' ifAbsent: ['']) substrings: ':'))
            do: [ :path | ... libraryPath exists ifTrue: [ ^ libraryPath fullName ]].
        self error: 'Module not found.'

The hardcoded half is x86_64-only (so is `FFIUnix64LibraryFinder>>knownPaths`),
so on aarch64 it is LD_LIBRARY_PATH or nothing — and a Pharo distribution's
`pharo` is a shell WRAPPER that sets it from `ldd`'s idea of the VM's libc
directory.  Cog's image saw
`/home/ubuntu/h3/lib:/lib/aarch64-linux-gnu:/lib:/usr/lib/aarch64-linux-gnu:/usr/lib:`;
ours, a bare ELF binary, saw nothing.  `ffi::ensurePlatformLibraryPath()` now
reproduces that variable at startup via `dladdr` on a libc symbol (Linux only;
`PHARO_NO_PLATFORM_LIB_PATH=1` opts out).  After it, ours resolves
`/lib/aarch64-linux-gnu/libsqlite3.so.0` and `sqlite3_libversion` returns
`'3.45.1'` — a real callout through the native library.

LESSON: "fixed by construction, not verified on the platform" was worth exactly
nothing here — the fix was correct code for a bug that was not the bug.

### 3. Probe improvements kept (they are what found #1)

  - `[ROOT-WATCH]` names the OWNING FRAME of an operand-stack hit
    ("slot 29/32 frame#9/10 #testPropertyManagerValueWeakness fp+3"), and the
    singleton VM registers report the FIELD name — `ROOT(activeContext_)`, not
    a shared "vm-registers".
  - `[HEAP-CHAIN]` prints the full retention chain for every watched-class
    instance after the mark fixpoint.  `PHARO_WATCH_HEAP_CLASS` alone stopped
    at the immediate parent; `PHARO_WEAK_SURVIVOR_PATHS` only covered referents
    sitting in a weak slot, and this one did not.
  - Context hops in both are annotated live vs DEAD-RESIDUE with the
    activation's selector.  That annotation made the cause self-evident.

### 4. Follow-on commits the trace bound forced

`d209543d` — with the trace stopping at stackp, a stackp that is too LOW is a
correctness hazard, not just a wasted scan (the value is invisible to the mark
and unpatched by compaction).  That is the exact hazard the whole-array scan
existed to paper over, so it is closed at the writers: `raiseContextStackpTo`
covers the single-slot writers (`setTemporary` write-through,
`setOuterTemporary`), and the third context-reuse path in materializeFrameStack
now goes through `storeContextStackp` like the other two.
`919904cd` — `getLibSearchPaths` searches the platform lib dirs directly, since
glibc snapshots LD_LIBRARY_PATH at process start and our own dlopen would not
see the setenv.
`31aa67b3`, `89942e89` — keep both additions off the hot paths: the
operand-stack root walk stays a bare visit when the probe is off, and
storeContextStackp clears only the range a store VACATES rather than walking to
the context's capacity at every process switch.

### 5. Box gotcha — a control session must beat the lease itself

The first box was terminated 45 min in, MID-RUN
(`Client.UserInitiatedShutdown` = the keep-alive reaper), losing a package
re-verify and a full suite.  `provision.sh` REGISTERS the lease, and only an
actively-working Claude heartbeats it — on the box via the DMI instance id, or
from here via `AWS_LEASE_IID` in the environment the hook sees.  A
`provision.sh` run does not put that in the parent Claude's environment, so
driving a box FROM this Mac needs an explicit periodic `lease.sh beat <iid>`.
The provision output says so; it is easy to skim past.  Nothing else went
wrong with that box.

## ======= 2026-08-11 FOLLOW-UP SESSION =======

Full write-up: `docs/aws-followup-2026-08-11.md`.  Worked the open list in
section D of the previous session (below).  Everything measured locally —
arm64 native plus x86_64 through the `build-x86` tree under Rosetta.

    x86 multi-entry dispatch divergence     FIXED   c5332248 (BOTH arches)
    CWD-relative file resolution            FIXED   d1cd608e
    weak-reference tests                    FIXED   88ce3fee (50 clean runs)
    2nd weak-ref path (porpoise)            OPEN — repro'd locally, see H
    sqlite3 FFI Module-not-found            FIXED   c1d6eef7 (Linux unverified)
    arm package sweep                       DONE — docs/arm-pkg200-2026-08-11.md
    build-hunt history rewrite              DECLINED by user (leave it)
    xcode-select                            NOT DONE (needs a password)

**D. `88ce3fee` — executeFromContext disowned the context it restored.**
The weak-reference failures were never a weak-reference bug.
`executeFromContext` rebuilds a suspended activation into a fresh C++ frame at
a DIFFERENT stack address (measured: savedFP+1 0x44780ab60 -> 0x44780a960 for
the same activation) and sets `activeContext_ = context`, but ALSO cleared
`currentFrameMaterializedCtx_` — so the frame no longer knew it owned that
context and nothing synced back.  A closure holding it as outerContext, the
sender chain, and the GC all kept seeing pre-restore temps.  Fixed by claiming
ownership.  0 P / 2 F in 10/10 runs -> 2 P / 0 F in 14 of 16; ~14,000 tests of
regression coverage with zero new failures; costs ~9% on the `1M blocks` bench
(owned contexts re-sync rather than take the cheap path) — measured in
isolation, 4 runs each, no overlap.  NO RESIDUAL: the 2 failures in the first
10-run batch did not reproduce on a quiet machine.  Final build = 50
consecutive clean runs (20 plain, 16 probed, 8 under six spinning CPU hogs,
6 post-fix) against a 16/16 stock-Cog baseline.  Those 2 came from a window
when this Mac was concurrently driving AWS builds — two failures in ten is not
a rate, which is the same small-sample error that produced the original
"flaky weak refs" misclassification.

Four reusable GC-provenance probes came out of this and are the reason it was
findable: PHARO_WATCH_ROOT_CLASS (which ROOT category holds a class),
PHARO_WATCH_HEAP_CLASS (the MARK-phase parent), PHARO_WEAK_SURVIVOR_PATHS (the
full parent chain for every weak referent that survived) and
PHARO_TRACE_FRAME_TEMPS (frame slots at materialize + push, telling a stale
context from a stale frame).

**E. `41863894` — the sweep caught a regression from D's sibling fix.**
With the VM no longer chdir'ing, eval mode writes `startup.st` into the CWD
(where Pharo actually reads it).  `run-sweep-parallel.sh` runs 24 workers from
ONE directory, so they clobbered each other's script: 152 of 223 custom-VM runs
in the first arm sweep evaluated nothing and the summary said `jit_RESULT = -`.
Both arms now run from `$OUT/wd-$LABEL`, and the VM prints a loud error if its
startup.st is still on disk at exit (the script self-deletes, so a survivor
means the loader never ran it).  Sweep relaunched on the fixed code.

**H. IN PROGRESS — the sweep's two findings (stopped here 2026-08-11).**

1. sqlite3 FFI — **FIXED `c1d6eef7`**.  `primitiveLoadModule` built candidate
   file names with an unconditional ".dylib" and NO ".so" branch, so on Linux
   module `sqlite3` was tried as sqlite3 / libsqlite3 / sqlite3.dylib /
   libsqlite3.dylib and never libsqlite3.so.0.  FFI.cpp's
   `tryLoadFromSearchPaths` had the correct #ifdef split — the two had drifted
   (same shape as the per-arch Sista backends).  Now ONE
   `ffi::moduleCandidates()` shared by both, which also tries versioned sonames
   (.so.0/.1/.2/.3) because distros ship libsqlite3.so.0 and put the bare .so
   symlink in the -dev package.  macOS verified unchanged (21 FFI/TFFI/Athens/
   LibTTY classes, 181 P / 0 F / 0 E).  **NOT verified on Linux** — needs
   pharo-rdbms-pharo-sqlite3 re-run on a box.

2. Second weak-reference path — **REPRODUCED LOCALLY, NOT ROOT-CAUSED.**
   Package loaded at /tmp/porp/Pharo.image (Metacello baseline Porpoise,
   github://rko281/Porpoise:master, load default+tests).  Repro:

       cd /tmp/porp && PKG_PREFIXES=Porpoise <VM> Pharo.image eval \
         "$(cat scripts/pkg-jit-test/run_pkg_tests.st)"
       cog:  RESULT pass=14 fail=0
       ours: FAIL PropertyManagerTest>>testPropertyManagerValueWeakness
             [Got 1 instead of 2.]   RESULT pass=13 fail=1

   Passes 3/3 in ISOLATION on both VMs — it needs the package's 3-class suite
   context, exactly like WeakOrderedCollectionTest did.
   Direction is OPPOSITE to the bug fixed in `88ce3fee`: "Got 1 instead of 2"
   means something is MISSING from `PropertyManagerTestObject allInstances`,
   not surviving too long.  The failing assertion is almost certainly the FIRST
   one (`count + 1` right after `PropertyManagerTestObject new`, with no GC in
   between, so the instance is still in eden).
   RULED OUT: `allInstances` missing eden objects — measured on both VMs,
   `OrderedCollection new` then `allInstances size` gives delta=1 on ours AND on
   cog.  So it is not a blanket young-space blind spot.
   NEXT: instrument which of the three assertions fires (recompile the test with
   a marker per assert), then compare `allInstances` behaviour under the suite's
   GC pattern.  The four provenance probes are available if it turns out to be
   retention after all.

**G. ARM SWEEP COMPLETE (third attempt) — `docs/arm-pkg200-2026-08-11.md`.**
128 packages ran on both arms.  Two real findings, one spurious, rest
timeout-only — same shape as x86.
  1. `pharo-rdbms-pharo-sqlite3`: cog pass=122/err=0 -> jit pass=11/err=111,
     ALL "Error: Module not found." — an FFI library-resolution gap (same family
     as the Athens/cairo and LibTTY fixes), NOT a JIT miscompile.  New.
  2. `rko281-porpoise`: `PropertyManagerTest>>testPropertyManagerValueWeakness`
     still JIT-only.  `88ce3fee` fixed WeakOrderedCollectionTest but NOT this —
     so there IS a second weak-reference retention path, and unlike the
     unreproducible "residual" this one is a stable target for the provenance
     probes.
  3. **soccertheory CLOSED.**  x86 had 12 JIT-only XMLFileException errors,
     attributed by reasoning to our chdir.  Confirmed: on arm after `d1cd608e`
     the same 12 are SHARED (cog err=13 vs jit err=12+1 timeout) — both VMs now
     resolve `soccerML.dtd` identically.  0 JIT-only.
  4. `mumez-redistick` counters byte-identical on both arms — the flag column
     over-reports, as on x86.
Results: `docs/results/arm-pkg200-2026-08-11-summary.tsv` +
`s3://iospharo-build-670060058357/arm-pkg200/` (688 files).  Box torn down.

**F. Attempt 2 was LOST at 158/200 — SPOT RECLAMATION.**
`StateReason.Code = Server.SpotInstanceTermination`, "no Spot capacity
available that matches your request".  A spot box taken back by AWS; nothing in
this repo caused it, and `config-arm.env` already documented the same thing
happening to an earlier c7g.16xlarge.  Mitigation: `FORCE_ONDEMAND=1
INSTANCE_TYPE=c7g.4xlarge` (also the ~$0.60/h shape) plus `1b9b7a56`, which
uploads results per package so a reclaimed box leaves what it finished.

CORRECTION: commit `4ca528ad` claims a CloudWatch idle alarm did this.  It did
not — no such alarm existed for that box (the newest in the account is from
2026-06-25, and provision.sh only arms one under ARM_CPU_ALARM=1).
`delete-alarms` succeeds silently on a non-existent alarm and I read that as
confirmation.  The IDLE_CORES scaling in that commit is still reasonable
hardening, and its lease.sh CONFIG_FILE fix is a real bug, but the stated cause
is wrong.  Old text follows for the record:
`idle-alarm.sh` arms a
CloudWatch alarm that TERMINATES the box on <4% average CPU for an hour.
CPUUtilization is a percentage of ALL vCPUs, and the arm default is a 64-vCPU
c7g.16xlarge, so a network-bound 24-worker sweep (~2-8 busy cores) averaged
under 4% and the box was killed an hour in.  Results were on the instance
store: gone.  NEITHER safety net can stop this — the on-box idle-shutdown is
process-aware and knew the sweep was running (`f034b896`), and the keep-alive
lease was live, but a lease only influences the local `reap.sh` while an EC2
alarm action terminates directly ("Service initiated" in StateTransitionReason
is the tell).  Fixed by expressing the threshold in CORES (`IDLE_CORES`,
default 0.64) scaled by the instance's real vCPU count.  Also fixed:
`lease.sh` ignored `CONFIG_FILE`, so the arm box's lease row was tagged
`iospharo-x64`.
RE-RUNNING: pass `PRESERVE_S3=s3://iospharo-build-670060058357/arm-pkg200`
(added to `run-pkg-jit-test.sh`) so each package's logs + .fails go off-box the
moment they exist — `preserve.sh` syncs notes/logs, NOT results.  Note the box
is a 64-vCPU on-demand c7g.16xlarge (~$2.3/h), not the ~$0.60/h 4xlarge quoted
in the earlier section; `INSTANCE_TYPE=c7g.4xlarge` is the cheap option.  The
per-package working directory is now removed after each package, so 200 loaded
images no longer accumulate on disk.

Box gotchas hit while doing this, both now fixed in-tree (`07091eeb`) or worth
remembering: a stale `origin/jit-arm-linux` autosave branch made clone-and-build
silently build 2026-06-10 code and report success; and a `git checkout -B` that
aborts on a submodule leaves a PARTIAL working tree (files silently deleted —
`git restore .` fixes it, and the build fails loudly only later).

**A. `c5332248` — multi-entry dispatch was broken on both arches.**
x86_64 had no dispatch prologue at all, yet SistaRuntime registered the
compiled fn at every dispatchable bcOffset regardless of arch — so an
interior-loop-top trigger jumped into a fn compiled for a different
bcOffset and resumed at the region start with the wrong operand stack
(new unit test, prologue removed: exit 139 / 1 / 139 over three runs —
wrong value or SIGSEGV).  arm64's prologue has been DEAD since
`bb158a7f` (2026-06-04): its unreachable-block prune is seeded only from
block 0, and the loader pseudo-blocks are reached only by the dispatch
chain, so they were pruned and the chain branched at unbound labels.
Restoring it is **~5x on tinyBenchmarks bytecodes/sec** (265-295M ->
1.34-1.51 G, five runs each; sends/sec unchanged).  The two apparent
full-suite deltas (1M blocks +24%, dict 50K -15%) do NOT survive
isolation — suite-order coupling, not the change.  Root-cause fix for the
drift: `lower()` now REPORTS the entry bcOffsets it emitted arms for and
the runtime keys the cache off that, instead of assuming.  `cc.finalize()`
status was discarded in both backends; now checked.

**B. `d1cd608e` — the VM no longer chdir()s to the image directory.**
Same launch dir, `FileSystem workingDirectory` + `'probe.txt' exists`:
Cog `/private/tmp/cwdtest true`, ours WAS `/private/tmp/harness false`,
ours NOW matches Cog.  The chdir was never needed for its stated purpose
(StartupPreferencesLoader reads `FileSystem workingDirectory` —
`lookInImageFolder` is misnamed).  Also fixed: a RELATIVE image path
could not be opened at all.  The startup.st CWD trap is closed at the
same time — the generated script self-deletes as its first statement
(survives a `timeout` kill), the stale sweep runs in both modes, and eval
refuses to clobber a foreign startup.st.  Opt-out
`PHARO_CHDIR_IMAGE_DIR=1`.  512 P / 0 F across the 26-class
file/path/resolver family, from two different CWDs.

**C. The weak-reference tests are NOT flaky — they are a deterministic,
JIT-only divergence.**  Previous sessions recorded "confirmed flaky" from
3F/1P over four x86 runs.  On arm64: ours 2 FAIL in 10/10 runs, stock Cog
2 PASS in 3/3.  `PHARO_NO_JIT=1` -> 2 PASS, so the JIT is required.
RULED OUT: Sista (both knobs), inline eden alloc, gen-clone, the J2J
save-pool GC roots (skipped entirely — still fails), nil-ing context
slots above stackp.  Reproduces ONLY under our runner: a direct 40x
loop, a fork at userBackgroundPriority, and SUnit's own `TestCase>>run`
all pass.  Adding two diagnostic statements to the test body makes it
PASS — dead-slot residue signature.  Live hypothesis: JIT frame slots
already popped but still inside `forEachRoot`'s
`stackBase_..stackPointer_` scan.  Next step + repro recipe in the doc.


## ==== 2026-08-10 -> 08-11 SESSION (infra hardening + the AWS runs) ====

Section D below is the list the follow-up session above worked from.

THEME: every bug this session was a **silent success** — something was broken
and reported OK. A reverted submodule pin, an unhashed tarball, a manifest-less
xcframework, an SDK misconfiguration, a suite that wrote no results, a box that
killed itself mid-run. Each fix converts silence into a loud failure. All work
committed+pushed to origin/jit; no AWS resources left running.

TERMINAL STATE: one real VM defect found and fixed (x86 Sista deopt was
GC-unsafe). Both architectures otherwise pass at ~99.7-99.9%. CI now exists.

### A. The VM bug (the substantive find)

1. **x86_64 Sista deopt ip was GC-unsafe — FIXED `ce2dcdd2`.** The lowerer is
   split whole-file per arch with no `#if`, so the two drifted: x86 baked
   `bytecodeBase + bcOffset` as an immediate at all 16 deopt sites and never
   read `state.method`; arm64 has recomputed it at fn entry since `7f2c55ce`.
   That commit gave x86 only `(void)startBcOffset; // not yet supported`.
   Both production callers pass a non-null bytecodeBase (`SistaRuntime.cpp:113`,
   `:316`), so after GC compaction moved a method a deopt resumed the
   interpreter at a stale address. Ported the arm64 hoist; all 16 sites now go
   through ONE `emitSetIp()` helper so the backends cannot drift the same way.
   Validated on a native x86 box: `test_sista_ir` FAIL→PASS, other binaries
   unchanged, 192-class SUnit subset at or below the pre-fix non-pass count.
   NOT fixed by it: the WeakOrderedCollectionTest failures — that class is
   flaky (3 FAIL / 1 PASS in four isolated runs WITH the fix).

### B. Test runs (docs/x86-test-run-2026-08-10.md, docs/arm-vs-x86-2026-08-11.md)

2. **x86_64 full run.** SUnit 26327/26412 = 99.68% over 1889 of ~2052 classes,
   cut deliberately at 3h04m when the Spec/GUI tail collapsed to ~13
   classes/hour. 200-package A/B sweep: the runner's "12 JIT-only failures"
   headline does NOT survive inspection — 2 real, 5 timeout-only, 5 spurious
   (identical counters, two ran zero tests). The larger "real" one is not a JIT
   bug either: soccertheory's 12 errors are all `XMLFileException: soccerML.dtd
   does not exist`, i.e. our VM chdir()s to the image dir so CWD-relative lookups
   resolve differently than under Cog. Real compatibility bug, wrong layer.

3. **aarch64 full run + comparison.** arm ran the suite to COMPLETION (2052
   classes, 27965 tests, 99.88%) in 2h02m. Per-test differential over the 26571
   tests both runs executed: 4 worse on arm, 63 better. The 63 are almost all
   `x86=TIMEOUT → arm=PASS` in the same GUI families that stalled x86 — Graviton3
   speed against a fixed timeout, not correctness. The 4 are GUI/timing, inside
   the measured flake floor. Conclusion: **equivalent in correctness at the SUnit
   and C++ level.**
   GAP: the arm package sweep was LOST (see 8) — package behaviour is compared
   for x86 only.

4. **Harness noise floor established.** This harness flips a few tests per run in
   either direction. Never conclude anything about an individual test from one
   run — including the "12 genuine failures" list in the x86 doc, which almost
   certainly contains flaky entries.

### C. Build/infra defects fixed (all were silent successes)

5. **asmjit pin reverted for ~2 months — `86151021`.** `27d378d2` (an x64 spot
   autosave) clobbered the pin from the fork branch back to upstream master,
   losing the Catalyst fix. Invisible because the working tree was patched by
   CMake at configure time, so local builds worked. `0c45aadd` stops the autosave
   staging submodule pointers (list now derived from .gitmodules, drift logged)
   and makes `clone-and-build.sh` demand the exact pinned SHA — "populated" is
   not "at the pin".

6. **Nothing verified downloads or artifacts.** `c5a116e2` adds SHA-256 to all
   ten third-party tarballs (libffi/SDL2 piped `curl | tar`, which cannot be
   verified at all — now download→verify→extract). `2fc5aab0` + follow-up make
   all four xcframework-producing scripts validate what they emitted;
   `SDL2.xcframework` had sat for two months as an orphaned directory with NO
   Info.plist — unusable by Xcode — because `Frameworks/` is gitignored and the
   script exited 0. `0f52f38a` adds an xcode-select preflight: the README told
   people to install Command Line Tools, the exact config with no iOS SDK.

7. **`c9b45476` — repo review + CI.** There was NO CI. Added a fresh-clone job
   (checks out with submodules, asserts pins reachable, builds on Linux) and a
   hygiene job (no tracked build output, scripts parse, checksums present, every
   artifact script validates). Also: `.gitignore` listed build dirs by exact name
   — 25 of them — and `build-hunt/` wasn't among them, so 21 MB of object files
   got committed; now pattern-based and untracked. `37e6093e`: SPM dependencies
   were never pinned (a bare `*.xcworkspace` ignore swallowed `Package.resolved`),
   plus two .st handlers that swallowed errors worth seeing.

8. **AWS scripts, three silent-success bugs.**
   - `d1b0c1aa` — fullsuite hardcoded an x86-only stock-VM URL; on aarch64 the
     prep installed no SUnit runner and the run reported `run exit=0` with an
     EMPTY results file. Did this three times before being noticed.
   - `f034b896` — `idle-shutdown.sh` didn't count `pharo` as an active process,
     so a box TERMINATED ITSELF 74 min into a network-bound package sweep
     (CloudTrail: the instance's own role, its own IP). All sweep results lost;
     `preserve.sh` syncs notes/logs, not results.
   - Gotcha (memory only): `pgrep -f`/`pkill -f` self-match over ssh — a sweep
     that never started read as "RUNNING", and a pkill killed its own session.

### D. Open items

- **arm package sweep** — ~1 h, ~$0.60 on a fresh box; would now survive given 8.
- **CWD-relative file resolution** — make lookups behave as under Cog (see 2).
- **Re-triage SUnit failures with repeat runs**, now that the flake floor is
  known; start with the weak-ref tests, which are confirmed flaky not broken.
- **Secondary x86 divergence, observed but unverified**: the multi-entry dispatch
  prologue exists only on arm64, yet `SistaRuntime.cpp:118-124` registers the
  compiled fn at every dispatchable bcOffset regardless of arch.
- **`build-hunt`'s 21 MB is still in git history** (.git is 289 MB); removing it
  needs a rewrite + force-push that invalidates every clone. Not done
  unilaterally.
- `sudo xcode-select -s /Applications/Xcode.app/Contents/Developer` still not
  run on this Mac (needs a password); build scripts self-correct via
  DEVELOPER_DIR, but anything else needing an iOS SDK will still fail.

## ============ 2026-07-08 SESSION SUMMARY ============

TERMINAL STATE: the base harness has ZERO genuine unfixed VM bugs. The three
target bugs are fixed+proven (SlotIntegration, ARM storm, TF-callback — see the
07-07 summary below). This session did the reflective perf gap + a full-harness
re-sweep + a user-directed catalog-boot re-investigation. All work
committed+pushed to origin/jit.

1. REFLECTIVE-SLOWNESS perf gap — PARTIAL FIX (validated), committed+pushed:
   - VM primitives aad03bc0: CompiledCode>>refersToLiteral: ->
     primitiveRefersToLiteral, scanFor: -> primitiveScanForByte. Both keep the
     EXACT Smalltalk fallback; fire only via the pragma the runner installs
     (inert otherwise). Activated in submodule pharo-headless-test e5fdf9d
     (run_sunit_tests.st), parent pointer 0b448cfb.
   - ~1.5-2x on allSendersOf:/allReferencesTo: scans, scales with method count.
     Validated: 0 mismatches over 1,319,322 refersToLiteral: comparisons +
     identical allSendersOf: across 21 selectors.
   - Class-level scan primitive REJECTED: hasSelector:specialSelectorIndex:'s
     #ffiNonCompiledMethod per-method property semantics = whole-image
     reflective-correctness landmine. That collapse is the activation-wall
     project's job. Residual floor = the activation wall.

2. FULL 565-CLASS HARNESS RE-SWEEP (primitives active), batches 1-150/151-340/
   341-565, ~12,700 pass:
        Fail:     0
        Error:    1   OCClassBuilderTest>>testCreateNormalClassWithTraitComposition
                      (stock-Cog-identical upstream — image_issues.md:184)
        Timeout:  1   reflective-slowness (activation wall)
        genuine unfixed VM bugs:  0

3. CATALOG-BOOT DNU re-investigation (user asked to rebuild the catalog to hunt
   more bugs) — CONCLUSION: almost certainly an IMAGE/PACKAGE issue, not a VM
   bug; rebuild BLOCKED by a stock-Cog regression. Evidence (docs/deferred.md
   ~line 213, memory aws-catalog-build-env + reflective-scan-primitives):
   - Local repro RULED OUT a generic-poisoned-delay VM bug: our VM = stock Cog
     on `waitTimeoutMilliseconds: nil` (both SCHEDULER-ALIVE).
   - The startup-process packages (iris/interopserver/tsf-scheduler/teapot) all
     boot fine INDIVIDUALLY on the custom VM -> the merged-boot DNU is emergent;
     stock Cog also failed on the merged image -> image/package-level.
   - Rebuild blocked: current get.pharo.org/64/vm130 stock-Cog VM segfaults in
     its threaded FFI worker (primitivePerformWorkerCall:) on EVERY Metacello
     github:// load, reproduced even with a fully-consistent bundled focal
     OpenSSL 1.1 set -> it's the FFI worker, not openssl. The original build
     predated this VM regression.
   - AWS infra fixed along the way: provision.sh default 24.04 (VM needs glibc
     2.36 arc4random_buf + cmake 3.24) with robust gp3/gp2 AMI lookup; the
     catalog build script bundles focal OpenSSL 1.1 for libgit2 (commits
     112027a1/d434981f/3356371f). All boxes torn down; no AWS cost running.

Note (unchanged, deliberate): third_party/asmjit has a local working-tree patch
(asmjit/core/virtmem.cpp — moves #include <libkern/OSCacheControl.h> out of the
TARGET_OS_OSX guard so sys_icache_invalidate() resolves on Mac Catalyst). It is
macOS-Catalyst-only (Linux/x86 builds don't need it, which is why the AWS boxes
build clean) and intentionally kept uncommitted in the vendored submodule. Do
NOT `git submodule update` third_party/asmjit without re-applying it.

## ============ 2026-07-07 SESSION SUMMARY ============

NINE VM fixes this session, all committed+pushed, all package-verified on
freshly-loaded package images + regression batteries (details: docs/
changes.md 2026-07-07):
  1. e5570688  SQFile ByteArray file handles (soil 9/9) + wall-clock
     profiler deadline (sauco 6/6)
  2. 38c457f7  POLLHUP in connect-writable map (redistick parity) +
     mixed int/float compare fast path (hera kernel 48x->3.6x)
  3. ac87599f  ephemeron basicNewTenured + vmParameter 34 alloc clock
     (illimani 15/15)
  4. f9e49453  live-frame NLR sends aboutToReturn:through: (methodproxies
     39/39)
  5. 4c4e13e6  edge-suppress socket readSema storm (myprecious
     testArgPassByCopy)
  6. a99eee86  two-way become swaps C++ stack refs (ReStore 15/15)
  7. 19dd56b0  primitiveProfileSemaphore clears pinned-process GC roots
     (Cog parity)
  8. 22fcb0e7  low-space signal implemented — prim 125 was write-only
     (Cog parity; the runaway-allocation circuit breaker)
  9. diagnostics: dumpHeapCensus + PHARO_HEAP_CENSUS, PHARO_PRIM_FAIL_
     STORM (gated), PHARO_MAX_OLD_SPACE_MB knob
Plus a798cfac (3 upstream pristine-image bugs -> image_issues.md wishlist).

CANDIDATE-QUEUE HUNT COMPLETE (14-agent workflow): every package
classified; 7 families FIXED (fixes 1-6 above), rest = perf-gap
(activation-wall project) / upstream test-design / Linux-only flavor.

VALIDATION (FINAL):
- ARM full catalog COMPLETED (lean binary + low-space net, 2 GB ceiling):
  **27,760 P / 16 F / 2 E / 7 T = 25 non-pass = 99.91%** — ran to
  completion (ZnUtilsTest, last class), NO STORM this run.  All 25
  non-passes are KNOWN accepted-residual families (reflective-slowness
  timeouts, StDebugger/StSpotter, ReleaseTest run-order pollution, the
  3 upstream image bugs, ZnClient network, TKT flaky, SlotIntegration
  trait doubling [pre-existing dossier], GC flakes) — ZERO new
  regressions from the 9 fixes.  vs catalog #8: 22 non-pass; delta = 3
  timing-variable flakes.  The storm being absent confirms it is RARE;
  the lean binary completes the full catalog.
- x86 (all 9 fixes): NO storm, clean through ~99% of classes (P~26,315,
  F=10, E=14 from parsed totals); ended in the pre-existing ThreadedFFI-
  callback exit-latency region (TFBasicType..., not a storm).  Box torn
  down.
- ARM: catalog #8 baseline was 22 non-pass / 99.92%.  The 2026-07-07
  wave introduced ONE regression — the ARM-only "context storm"
  Heisenbug (full dossier: docs/deferred.md "ARM catalog context
  storm").  Open; mitigated by the low-space signal (fix 8); repro needs
  PHARO_DET_SCHED.  NLR + become EXONERATED; profiler-deadline/ephemeron/
  compare/readSema still open.  Bisect binaries staged on branches
  storm-test-noprofdeadline, storm-test-nobecome, storm-fix-framescope.

OPEN FOLLOW-UPS (next session):
- ARM context storm root-cause bisect via PHARO_DET_SCHED (recipe in
  deferred.md); verify low-space net catches it.
- SlotIntegrationTest trait ivar-add (pre-existing, 18-probe dossier in
  deferred.md).
- TF-callback exit-latency tail hang (pre-existing, both platforms).
- Activation-wall perf project (owns the perf-gap package families).

GOTCHA logged: never run two local test_load_image catalogs at once —
they share /tmp/sunit_test_results.txt + the image and clobber each
other (looks like early death at ~120-240 lines).  One run at a time;
kill strays with pkill -9 -f "test_load_image /tmp/harness".

## ====================================================================



## 2026-07-07 — hunt RESULTS: five VM fixes landed (see docs/changes.md 2026-07-07)

Fix commits (all verified on the hunt images + regression batteries;
built/tested in build-hunt/ to keep the fleet's build/ binary stable):
e5570688 (SQFile handles + profiler deadline), 38c457f7 (POLLHUP +
int/float compare fast path), ac87599f (ephemeron tenure + param 34),
f9e49453 (NLR aboutToReturn: protocol), 4c4e13e6 (readSema storm).
SIXTH fix landed after the fleet finished: a99eee86 two-way become
C++ stack swap (restoreforpharo 15/15, package-free repro in memory).
All six pushed through a99eee86 + docs 069db8eb.

CATALOG #9a/#9b POST-MORTEM (2026-07-07 morning): both died/wedged at
~26k result lines (9a: old-space exhausted in RB*; 9b at 8 GB: wedged in
RSCircleVennDiagramTest with live heap 64 MB -> 3.7 GB, reclaimed=0 —
GC death spiral, lldb showed the VM living inside scavenge()).  ROOT
CAUSE (fixed 19dd56b0): primitiveProfileSemaphore must clear
profileSample_/profilePrimitive_ on every call (Cog parity).  They are
GC roots; the deadline fix made the sampler actually fire, so the last
sampled process stayed pinned forever after MessageTally/ASP
stopProfiling — pinning its death-time stack retinue and everything
reachable from it.  Differential vs #8 that cracked it:
WeakOrderedCollectionTest all-garbage-collected FAIL (passes standalone).
Bisect side-find: SlotIntegrationTest testAddAndAddInstVarNamedWithTrait2
fails standalone at PRE-fix ffca1841 too (trait ivar-add lost; '2 ran'
doubling) — pre-existing, masked by catalog order; queued in deferred.

CATALOG 9c POST-MORTEM: the explosion REPRODUCED with the profiler pin
cleared — same GC-sample signature (64 MB at fullGC #352 -> ~3.69 GB at
#384, reclaimed=0, 12 s GCs), specimen again living inside scavenge().
The exploding class varies per run (9b: RSCircleVennDiagramTest
testBasicVennDiagramOpen; 9c: RSExamplesTest testClassSideExamples —
both PASS standalone and PASSED in catalog #8) — an RS window-open/
examples-region suite-context accumulation.  The pin fix stands on its
own (Cog parity + WeakOrderedCollection differential) but was not the
whole story.  NEW INSTRUMENT (1363d92d): dumpHeapCensus — top-25
class-name histogram by bytes, on the FATAL path and per-fullGC >1 GB
under PHARO_HEAP_CENSUS=1.  Suspect list for the census to arbitrate:
Athens/SDL surface pixel ByteArrays rooted by the (growable) manual-
surface registry vs World-accumulated windows re-rendered by the
FakeGUI 25 ms cycle loop vs materialized-context/other VM-root pinning.

STORM BISECT (2026-07-07 ~14:30):
- become (a99eee86) EXONERATED by cheap sender analysis (no run needed):
  the debugger stepping (FastStepThroughController>>revertBlocks) uses
  elementsForwardIdentityTo: = ONE-WAY become (prim 249, untouched);
  only BecomeTest/ObjectTest/Fuel send the TWO-WAY become: I changed.
  Frame-scoped targeted fix pre-staged anyway (branch storm-fix-
  framescope, 404674cd) in case a later test needs it.
- NEW PRIME SUSPECT: e5570688 profiler-DEADLINE hunk (not SQFile).  It
  is the only fix that changes SCHEDULING TIMING — synchronousSignal at
  every periodic checkpoint + primitiveRelinquishProcessor when a
  deadline is armed.  If an earlier profiler test (StProfiler/
  MessageTally) armed profileStart: and errored before profileSemaphore:
  nil, the deadline keeps firing synchronousSignal on a possibly-stale
  semaphore in the RS region = the process-resurrection vector.  The
  OLD bytecode-count profiler was dormant in-suite (never fired), which
  is why the storm is new.  Awaiting the all-diagnostics run's failing-
  primitive name to confirm before the targeted bisect.

STORM BISECT (2026-07-07 ~14:00):
- NLR revert (f9e49453) EXONERATED: storm still fires, identical
  signature (P80 PrimitiveFailed>>freeze recursion at RSExamplesTest).
- Storm process = a LEAKED Oups dummy-debugger P80 (OupsDummyDebugger
  class>>dummySession's `[Set new]` context process) that gets resumed
  in the RS-window-open region (FakeGUI World cycle) and recurses in
  exception-freeze.  OupsDebuggerSystemTest passes 5/5 STANDALONE, no
  leak — pure suite-context interaction.
- TOP SUSPECT: a99eee86 (two-way become C++ stack swap) — the debugger
  splices contexts via become during stepping/simulation; my change
  swapped stack refs both-ways where it was one-way.  become-reverted
  binary BUILT + staged in build-hunt/ (branch storm-test-nobecome,
  2a8bfa9f), ready to launch the moment the all-diagnostics run
  (build/, HEAD, census+procdump+PRIM_FAIL_STORM) explodes and names
  the failing primitive.  Runs are timing-sensitive → serialized, not
  parallel.
- x86 box (all 9 fixes): NO storm, deep into Calypso Sy* region — Linux
  validation of the whole wave essentially done.

STORM DOSSIER UPDATE (2026-07-07 ~13:00):
- CENSUS VERDICT: the explosion is CONTEXTS (2.4M -> 9M objects, ~12 per
  iteration) + lock-step MNU/PrimitiveFailed/Message triples (~135k per
  census interval) — a P80 process in an Oups dummy-debugger exception
  recursion, endlessly freeze/freezeUpTo:-copying its ever-deepening
  MATERIALIZED stack (evades the 4096 live-frame overflow guard).
- ffca1841 BASELINE CONTROL: full catalog COMPLETED, no storm, peak
  364 MB — and a fresh best-ever 27,766 P / 13 F / 2 E / 4 T = 19
  non-pass (99.93%).  The storm is a REGRESSION from the six VM-fix
  commits (e5570688..a99eee86 window).
- x86 (box, all NINE fixes + census): NO STORM — cruised past the RS
  region, zero census firings.  ARM/macOS-timing-specific race.
- PRIME SUSPECT: f9e49453 (NLR aboutToReturn protocol — the storm IS
  materialized-context churn in exception unwinds).  Local catalog on
  HEAD-with-NLR-reverted (branch storm-bisect-noNLR, 091d8b9e) IN
  FLIGHT to confirm; box completing the x86 9-fix validation.
- Ninth fix landed: low-space signal implemented (prim 125 was
  write-only; Cog semantics; [LOW-SPACE] verified firing).  Census now
  also dumps process queues.

VALIDATION IN FLIGHT (2026-07-07 ~09:30, third round, census armed):
- Local ARM catalog #9 running with all six fixes (launch_catalog.sh,
  log /tmp/catalog9_run.log, results /tmp/sunit_test_results.txt).
  Compare to catalog #8: 27,763 P / 22 non-pass = 99.92%.
- x86 box i-0ee8772f007d035af (3.150.133.214, us-east-2) running the
  full suite at 19dd56b0 (all seven fixes; earlier five-fix run was
  corrupted by my own p3-staging mistake — eval --save on the BAKED
  harness image fired SUnitRunner on the stock VM and reset
  /tmp/sunit_test_results.txt: NEVER run any VM against a runner-baked
  image while a suite is in flight).  Beat the lease with
  ./scripts/aws/lease.sh beat i-0ee8772f007d035af while working; box
  reaps without beats.  Teardown when done.
- After catalog: p3 Linux-resolver check on the box; then the
  activation-wall perf campaign (scripts/perf-activation/ README has
  the 4 ordered load-chain ablations; needs QUIET machine).
Hunt workdirs /tmp/pkghunt-* retained (loaded package images).

## 2026-07-07 — jitpkg candidate-queue hunt (launch notes)

- Workflow `pkg-candidate-hunt` (run wf_214bdc82-158): 14 agents, one per
  candidate package from the pkg200 sweep, each builds a FRESH package
  image locally (pristine 130 base in scratchpad/pkgbase, stock-VM
  Metacello load with retry — stock Cog segfaults ~1-in-2 during git/FFI
  fetch on this Mac, retries succeed), parity-runs the jit-only failing
  tests on both VMs, shrinks divergences to minimal repros.  jit-only
  sets computed from docs/results/catalog-2026-07-06/pkg200-fails/
  (comm -13 cog jit).  NOTE: p3's real jit-only set is
  P3ConnectionPoolTest testOne/testError/testWarmUp TIMEOUTs (socket
  error-path shaped), NOT the P3ClientTest conversion errors (parity —
  both VMs lack a postgres server).
- docs/image_issues.md upstream wishlist written + committed (a798cfac):
  OCClassBuilderTest trait-composition OCCodeError, SystemDependencies
  UI-deps Reflectivity drift, MorphicNativeWindow lacks hasProperty: —
  all three re-verified failing on stock Cog with the PRISTINE image.
- DiskFileSystemTest testLongFilename "one-off": NOT in any archived
  detail (catalog 7, 8, x86 run3 all 59/59 PASS) — never reproduced;
  dropped as non-actionable.

**CATALOG #8 (macOS/ARM, everything): 27,763 P / 15 F / 3 E / 4 T / 182 S
= 22 non-pass = 99.92%.**  Full-day arc: 199 → 113 → 84 → 59 → 33 → 22.
Archived: docs/results/catalog-2026-07-06/arm_catalog8_detail.txt.

NIGHT-WAVE FIXES (all workflow-verified, committed through 43a8b43d):
- VM: nil-methodDict #cannotInterpret: proxy protocol (60d4ee1c) + become
  classTable redirect (07a27e63) — GHost/Mocketry real-object stubs went
  from silently-no-op to fully working (Bitbucket suite 0/30 → 30/30;
  gitlab+bitbucket = 90 jitpkg errors resolved).  Minimal repro preserved
  in memory proxy-protocol-vm-contracts.md, including the mustBeBoolean
  near-miss (VM must NOT rewind; the image does).
- VM: GC class-pinning — forEachMemoryRoot deep-traced classTable pages
  during mark (ReleaseTest testObsoleteClasses); vmParameter 79
  (imageVersion); bare launches non-interactive (PHARO_INTERACTIVE
  opt-in) killing the MorphicWindowManager/clipboard pollution family.
- Runner/prep hygiene batch (submodule 90d60d7): orphan packages
  (protocol reclassifications + BaselineOfSUnitHarness), sched-logger via
  addSelectorSilently + perform: + writeStreamDo: (appendStreamDo: was a
  latent DNU — no implementors in Pharo 13!), pharo.version write,
  SUnitTest fresh-env, MorphicUIManager reinstall.
- 12-agent residual analysis (workflow wi6e5e0me): every one of the 33
  catalog-#7 non-passes root-caused with adversarial verification; the
  22 remaining in #8 are all in the accepted-residual classes:
  upstream-image bugs (OCClassBuilder, SystemDependencies UIdeps),
  stock-identical run-order pollution (ReleaseTest literal-pin/
  PackageOrganizer/testObsoleteClasses-in-suite), env both-fail
  (StDebugger layout/context family), perf casualties (StSpotter x2,
  slow scans x3), GC catalog-context flakes (FinalizationRegistry,
  roving Trait/TraitFileOut timeout), upstream-flaky TKT, network
  ZnClient, and singles (EDEmergency, RBRefactoringChange,
  StTranscript testClear — new roving singles, same flake families).
- jitpkg 200-package sweep on box #2 (i-01e7bfe27763e6ff8): aigraph
  176/176 parity (May bug conclusively dead), bitbucket/gitlab fixed;
  sweep at ~165/200, flagged rows to re-run on the current binary when
  it completes (illimani 15, famixreplication 4, p3 3, singles).

Previous wrap follows.

# WIP — /goal "fix all non-Windows bugs" session 2026-07-06 (day) — WRAPPED ~16:30

**FINAL: catalog #7 (macOS/ARM, everything incl. the surface-registry
fix): 27,760 P / 21 F / 8 E / 4 T / 174 S = 33 non-pass = 99.88%.**
July-4 was ~743 non-pass (97.35%); catalog #2: 199; today 113 → 84 →
59 → 33.  Single VM, no batch splitting, no carpets.  The RS*/Roassal
family (24) vanished with the surface-registry fix, confirming it at
suite scale.  The 33: ReleaseTest 9 + SUnitTest/SystemDeps/ProperPkgs
(harness-hygiene ~12), StDebugger family 8 (GUI residue — next target),
RB 4 (suite-context flake), ZnClient GeoIP 1 (network), TKT 1 (upstream-
flaky), OCClassBuilder 1 (stock fails too), slow-scan T x2, singles x4.
Detail archived: docs/results/catalog-2026-07-06/ (ARM #7 + x86 run3).
x86 box run #3: 99.54% on the pre-World-cycle harness + every family fix
verified individually on-box (TF 48/48, LibTTY 5/5, DiskFileAttributes
24/24, Athens 17/17, FFIParser 45/45).  Box TERMINATED (teardown
--instance-only, lease released).

POST-#6 FIX (Mac-verified, needs next catalog for numbers): **growable
SurfacePlugin registry** (23ae8a07) — the fixed 64-slot table exhausted
mid-suite ('Unable to register surface with SurfacePlugin') and was THE
mechanism behind the RS*/Roassal suite-context error family (~24 of the
59; pass isolated, fail after ~2000 classes).  Expected next catalog:
~35 non-pass ≈ 99.87%.

BOX RUN #4 ANOMALY (root-caused, not a VM bug): re-downloading
image+changes next to a STALE .sources produced a source-misaligned image
(argumentNames #(), garbage sourceCode) failing ~700 AST/decompiler/
FFI-parser tests — looked exactly like a VM regression.  The A/B chain
(old-binary-same-image still broken; fresh-everything clean) pinned it.
Hard rule now baked into scripts/aws/x86-fullsuite.sh (committed
d122d497): the harness dir is nuked and re-fetched WHOLE each run.

PARTING FINDING (repro for next session): looping the FULL
RBBrowserEnvironmentTest suite in ONE warm VM (scratchpad
rb_class_loop.st) HANGS after several iterations — the VM's own
600s stuck-process guard fired ([VM-TIMEOUT] P40 stuck, log
rb_class_loop.log:60130).  The RB suite-context flake is a warm-heap
HANG in the environment-scan machinery, self-contained repro, no full
catalog needed.  Next: rerun with PHARO_PROC_DUMP=1 and read the DIAG
stacks at the stall.

REMAINING (all classified, see deferred.md 2026-07-06 section):
suite-context singles (RB 4, StDebugger 3-4, St* singles), reflective-
slowness timeouts (3, activation-wall perf project), ReleaseTest/
SystemDependencies/SUnitTest harness-hygiene, ZnClient GeoIP (network),
TKT (upstream-flaky test, deterministic under our scheduling — analysis
in session notes), jitpkg external-package re-verify (next box session).

STATE ~14:00: **CATALOG #4 (ARM/macOS) COMPLETE: 27,704 P / 39 F / 66 E /
8 T / 144 S = 99.60%** (99.70% after subtracting the 29 FL/Cly
expectedFailures bogeys whose runner fix landed post-prep).  NO wedge
carpets, single VM, no batch splitting — vs catalog #2's 98.78%/199 and
the four-batch workaround.  Box (x86) run #2: 27,525 P / 41 F / 240 E /
13 T = 98.95%; its extra ~170 TF*/TFUFFI errors + LibTTY + DiskFileAttr +
Athens/RS were ALL x86-environment bugs, all fixed since:
- exe-dir bare-name library search now works on Linux (/proc/self/exe) +
  .so candidates + fixture libs are test_load_image deps (cfe00911) —
  TFBasicTypeSize 48/48 on box (was 0/48).
- libtty _GNU_SOURCE: glibc's undeclared ptsname() truncated the pointer
  (EFAULT in child; parent read() on never-opened master blocked the VM)
  — LibTTY 5/5 on box (cb53b45f).
- DiskFileAttributesTest 24/24 on box (platform-name fix 7df34d01).
- Athens 17/17 + Cairo/FreeType on box (cairo stub gate + resolution
  order + basename fallback).
FINAL VERIFICATION RUNS IN FLIGHT: local catalog #5 (final runner
b67db07) + box suite #3 (fresh image, all fixes).  Remaining known
non-env singles: FTTableMorph alternate-row-colors (SubscriptOutOfBounds,
World-cycle family), RBBrowserEnvironment flake, Sp* tree/table adapters
(~21, headless World-cycle project), reflective-slowness TIMEOUTs x8
(activation-wall perf project), ReleaseTest hygiene (prep pollution).

Machine rebooted (stock-VM eval hang CLEARED; /tmp wiped — harness rebuilt
from fresh Pharo 13 download).  Two full-suite runs in flight:
- LOCAL ARM catalog #3: fresh image + fake-GUI + hardened runner baked,
  2052 classes, PHARO_MAX_STEPS=4e12 + CODE_ZONE_MB=192
  (results/detail in /tmp, log /tmp/catalog3_run.log).
- AWS x86 spot box i-01a4ea1f7bea4bc74 (3.17.153.16, m6a.4xlarge,
  ~/x86-fullsuite.sh, results in ~/results/): full suite at HEAD.
  Manual lease beats needed (./scripts/aws/lease.sh beat <iid>) —
  AWS_LEASE_IID isn't in this Claude's env so the hook no-ops.

FIXED so far this session (committed + pushed):
- 259ad3fc interp: missing <condition_variable> include broke Linux
  (libstdc++) builds — found by the box build.
- ef9cfa59 prims: GCC rejects pharo::-qualified definitions inside
  namespace pharo (g_prim111Ring).
- 4e6bb7d0 plugins: ship libtty (tty_spawn clean-room) — LibTTYTest
  0/5 -> 5/5 (stock ships Plugins/libtty.dylib; we never had it).
- 2527c2c (runner submodule, parent 8c1ae366): honor expectedFailures-
  METHOD declarations (not just the pragma) — kills 30 bogus non-passes
  (FL* x15, ClyAsync/ClyFilter x14, ClySemiAsync x1).  NOT in the baked
  image of the in-flight catalog #3 — subtract those families manually.
- TLS/HTTPS VERIFIED WORKING on macOS (sqMacSSL; ZnClient https 200) —
  catalog #2's "ZnHTTPS (no TLS)" label was stale.

WEDGE **RESOLVED** (~12:20): runner race, NOT a VM bug — the watchdog set
testDone/printed the verdict BEFORE killing the test; the P80 runner main
woke on testDone and its watchdog-cleanup path suspended the P60 watchdog
between the flush and the suspend/terminate — the runaway test was never
killed and starved its whole priority band (front-of-queue after
preemptions).  FIX: kill first, announce after (runner submodule b1a783e,
parent c3376e7c).  Verified on the wedge class-list: 50 P / 5 F / 1 T
(only testNoShadowedVariablesInMethods genuinely >80s, killed cleanly).
The 5 F are harness-hygiene (fake-GUI prep classes trip ReleaseTest
package/selector checks; testPharoVersionFileExists fails on stock too).
Memory: sunit-runner-kill-race.md.  CATALOG #4 RUNNING locally (fixed
runner + fake GUI, fresh prep); box full-suite #2 RUNNING (fixed runner,
fresh image; cairo now resolves via basename fallback aade2dde —
AthensCairoMatrix 17/17 on x86, was 0/17).

Historical hunt notes follow.

WEDGE HUNT STATE (~12:00): catalog #3 KILLED at 22,463 verdicts (unrecoverable
carpet; artifacts in scratchpad catalog3_*_partial.*).  Repro IN HAND:
class-list [ReleaseTest, NonInteractiveTranscriptTest, OSEnvironmentTest] +
PHARO_CODE_ZONE_MB=32 on the prepped harness image wedges identically
(first ReleaseTest scan test times out at 80s, its process NEVER dies,
everything at P40 starves behind it).  EVIDENCE CHAIN:
- lldb chain-walk of wedged victim: test frames (testNoShadowedVariables ←
  performTest ← ensure: x3 ← runCase ← on:do: x8) sitting ON TOP of
  terminateRealActive ← jump — i.e. Pharo-13 termination machinery
  (doTerminationFromAnotherProcess → parallel stack → Context>>unwindTo: →
  runUntilReturnFrom: → jump) RESUMED THE WHOLE TEST instead of just the
  unwind blocks.
- unwindTo: resumes-to-outerMost only when some ensure frame's
  unwindComplete (tempAt: 2) is non-nil.  Bare-eval probes (also with
  8MB zone): materialized ensure frames all have complete=nil and correct
  tempAt:1 (cleanup block) — static state is CLEAN; corruption/decision
  happens during the runner-context kill (suspect: env-watchdog
  TestTookTooMuchTime signalException at ~60s starts a legit unwind, our
  80s watchdog kills MID-UNWIND → resume-to-outerMost path → but then the
  'unwind completion' re-runs the test = the divergence to find).
- PROBE IMAGE BAKED: /tmp/harness/Pharo-probe.image has traced
  Context>>unwindTo: + Process>>doTerminationFromAnotherProcess ([UWT]/
  [DTAP] stderr lines; bake script scratchpad/trace_unwind.st).  Wedge
  repro on it running -> /tmp/wedge_probe.log; read the [UWT] decision
  (outerMost? which ctx? complete flags?) to pin the VM bug.
ALSO both platforms affected: the x86 box hit the same carpet natively.

Earlier session state follows.

OPEN — live-caught wedge window (catalog #3, ~09:35-09:44): 
NoUnusedVariablesLeftTest>>testNoUnusedTemporaryVariablesLeft (image-wide
scan; stock 8.5s, OURS >120s = the reflective perf gap) timed out at 80s,
then 7 subsequent trivial tests (NonInteractiveTranscript x4,
OSEnvironment x3) EACH timed out at 80s while the VM spun 100% CPU in
JIT/scavenge work; recovered on its own at ~09:44:45.  FALSIFIED so far:
slow terminate of the deep scan stack (instant at 5s and 80s depth in
bare forks).  Suspects: SUnit env machinery (runCase wrapping/
ProcessMonitor), timer-subsystem death window not caught by
[DELAY-DEATH] (only 1 firing all run, early + recovered).  NEXT: after
catalog frees /tmp, rerun wedge zone (NoUnusedVariablesLeft +
NonInteractiveTranscript + OSEnvironment) via /tmp/sunit_class_names.txt
with PHARO_DELAY_DEBUG=1 and watch the window live.

Prev shutdown state follows.

---

# WIP — SHUTDOWN STATE 2026-07-06 ~02:00 (all work committed + pushed, HEAD a53318c5)

Session arc COMPLETE — three debugging hunts finished, all validated:

1. **prim-100 simulation cascade** (e40cd65b) — pk-24 arity aliasing into
   the W3 IntArithReturn inline; stepping family 156 P / 0 F.
2. **WKD testClearing** (8cfee28c) + **TFFI idle-band starvation**
   (3940b62c) + **Zn socket hunt** (7478887d + ce4419af): MPSC-unsafe
   external-semaphore ring (lost wakeups), param-49 >65535 failure
   killing server processes via ProcessMonitorTestService suspension,
   socketError-on-stale; ring adversarially hardened (f7ed6703:
   ABA-proof counters, longjmp-safe tail, VM-thread overflow).
   ZnServerTest 22-28/31-every-run -> 31/31 x 10 consecutive.
3. **StDebugger delay-ticker wedge** (runner submodule 0316143, parent
   7eb962bd + a53318c5) — SUnit watchDogLoop desync passes nil into
   waitTimeoutMilliseconds:, nil-duration Delay kills the ticker
   (nil*1000 MNU), ProcessMonitorTestService suspends it, whole catalog
   sections stall.  Fixed via hardened watchDogLoop in the runner prep;
   validated: wedge zone 823 P / 0 timeouts / 0 ticker deaths (was
   5-101 deaths every run).  Full chain in deferred.md +
   docs/image_issues.md + memory stdebugger-ticker-death-wedge.md.

Also this session: stencil extraction had been SILENTLY BROKEN since
Jun-2 (fprintf in a stencil body) — fixed, regenerated, CMake-wired as a
hard build dependency (5accaddc + 4d97b675); IC_HIT arity gates; 215
debug knobs migrated to debug_vars.h (ratchet 250 -> 31).

**HARNESS STATE (for next session):**
- /tmp/harness/Pharo.image = Jul-4 prepped runner image + the hardened
  watchDogLoop guard baked in (Jul-6 01:47).  Pre-guard backup:
  /tmp/harness/Pharo-jul4-preguard.image.  Pristine eval image:
  /tmp/harness/Pharo.image.bak.
- The STOCK Cog VM (/tmp/harness/pharo) currently HANGS on any eval on
  this machine (worked Jul 4; environmental, undiagnosed) — preps were
  done with OUR VM instead (guard-only fileIn + prim-97 snapshot; the
  FULL runner re-fileIn on our VM aborts nondeterministically — bake
  single chunks; verify preps BEHAVIORALLY, never via sourceCode
  without the .changes file).
- Catalog detail files in /tmp: catalog2_part1_detail.txt +
  batch_{A,B,C,D}_detail.txt (98.78% composite, runs 19-22).

**QUEUED NEXT:** activation-wall perf project (harness + quiet-machine
baseline in scripts/perf-activation/, ablation order in its README);
remaining deferred items are Windows-side.  Optional: rerun the full
catalog with the hardened runner (expect the 3 batch-A wedge timeouts
back + steadier StDebugger family, marginal % change).

---

# WIP — catalog after the Zn socket hunt: 98.78% (2026-07-05 night)

**Full catalog #2 (all 2026-07-05 fixes): 27,859 verdicts — 27,519 PASS
= 98.78%, 199 non-pass (35 F / 72 E / 92 T) + 141 skips.**
Progression: July-4 ~743 non-pass (97.35%) -> morning fix wave 295
(98.44%) -> socket/ring fixes **199 (98.78%)** — 33% further cut, 73%
total from July 4.  Zn family: 460 P / 1 F (the network GeoIP test);
Zdc 80/80; SocketStream 22/22; TKT 107/1.  Remaining non-pass families
are exactly the known environmental set: Release/Cly/Renraku/Ring2/Rub
cold-context timeouts (runner caps at 5/class), LibTTY, Roassal/Cairo,
Spec2/StDebugger GUI env, FL WideString, OSEnvironment/
NonInteractiveTranscript, ZnHTTPS (no TLS).
Method note: the tail (Sp*..Z*) ran as FOUR fresh-VM batches to contain
the StDebuggerActionModelTest Delay-ticker-death wedge (see deferred.md
— pre-existing, bisected against the pre-afternoon binary, poisons
everything downstream when the death-loop recovery doesn't stick; batch
A absorbed it with only 3 timeouts).  Detail files:
/tmp/catalog2_part1_detail.txt + /tmp/batch_{A,B,C,D}_detail.txt
(runs 19-22).

---

# WIP — parked-bug fix wave COMPLETE (2026-07-05)

Both deferred deterministic bugs from the 07-04 verification are FIXED, plus
a scheduler starvation bug found while closing the CONC pacing item:

- e40cd65b  jit: prim-100 simulation cascade — pk-24 arity aliasing into
  the W3 IntArithReturn inline (3 conspiring defects: arity-blind
  inlinePrimKind classification of the prim-111 MIRROR form
  Context>>objectClass:, a single-bit tbnz(52) dispatch stolen by
  pk 16-31, and an OR-combined SmI tag check that accepted (heap,SmI)
  pairs — lookupClass = contextOop+arg-1, dead young pointer, DNU
  cascade).  Stepping family recovered: 156 P / 0 F (was ~50 fails:
  StepOver/Into/Through, simulate/tally trio, testBlockCannotReturn,
  testTerminateInTerminate).  Hunt ledger: scripts/cascade-hunt/README.md
  rounds 1-11 (the decisive probe was the IC-site dump now living in the
  DNU cascade forensics).
- 8cfee28c  interp/jit: WKD testClearing warm deviation — the JIT
  activation-exit one-shot woke the P50 mourner mid-statement when the
  GC prim ran INSIDE that activation; now fires only when armed at
  activation ENTRY (interp parity).  testClearing 6/6 warm;
  WeakAnnouncer 3x clean; weak batch 1003 P / 0 F.
- 3940b62c  sched: idle-band relinquish is a preemption point — the
  heartbeat force-yield hands the CPU DOWN to P10 idle but the route
  back UP only ran at 1024-step periodics (~10-20 bytecodes per 10ms
  sleep quantum = ~1s starvation for ready waiters).  TFFI worker
  callouts 13035ms -> 95ms per 500; TFUFFIConcurrencyTest(UsingWorker)
  10s-FAIL -> 995ms PASS.  Gated to pri<=10 (ungated resurrects the
  P80<->P60 voluntary-yield bounce and TIMEOUTs whole batches).
- 709d8a43  debug-vars: 215 legacy DebugSettings bools -> debug_vars.h;
  envPresent ratchet 250 -> 31.
- **Full-catalog 2026-07-05 COMPLETE (two-part run, all fixes in the
  binary): 27,716 verdicts, 27,283 PASS = 98.44%**, 295 non-pass
  (40 F / 125 E / 130 T) + 138 skips — vs July-4's ~743 non-passes,
  a ~60% reduction.  Every remaining family is environmental/known:
  Spec2 + StDebugger (GUI env, no fake-GUI in catalog prep), Zn/Zdc
  (network + the flaky local-server family below), Rub*/Renraku/
  Ring2/Release cold-context timeouts (runner caps at 5/class),
  LibTTY, FL WideString, Cly async, Roassal/Cairo, OSEnvironment/
  NonInteractiveTranscript, VariableBreakpointTest (8).  All
  previously-failing stepping/simulation families PASS in catalog
  context.  Detail preserved: /tmp/catalog_20260705_partial_detail.txt
  + /tmp/catalog_20260705_tail_detail.txt.
- **Zn socket-timing hunt COMPLETE 2026-07-05 (commits 7478887d +
  ce4419af): ZnServerTest 22-28/31-every-run -> 31/31 x 10 consecutive.**
  THREE root causes, none of them "timing" in the suspected sense:
  1. `signalExternalSemaphore`'s ring was MULTI-PRODUCER-UNSAFE
     (load/store/store head — single-producer only, but socket I/O
     thread + per-lookup DNS threads + TFFI workers all produce):
     concurrent producers overwrote each other's slot = silently lost
     semaphore wakeups at any occupancy; full ring also dropped
     silently.  Fixed: MPSC CAS-reserved slots, value-as-publish-flag,
     loud bounded-retry.  The Delay machinery was PROVEN INNOCENT
     (stalled DelayWaitTimeout sat correctly in the heap, ticker armed
     1s idles throughout).
  2. `vmParameterAt: 49 put:` (maxExternalSemaphores) FAILED above
     65535 and wrongly cloned the image-owned ExternalObjectsArray —
     the image's table-doubling past 64k raised 'Not enough space for
     external objects' inside a background Zn SERVER process, which
     SUnit's ProcessMonitorTestService then SUSPENDED (dead server ->
     client stall -> TestTookTooMuchTime shadowing the real error).
     Now pure bookkeeping (stock semantics; our ring is index-agnostic).
  3. sp_primitiveSocketError failed on stale handles from error-
     REPORTING paths ('Cannot access socket error code' replacing the
     real error).  Now never fails.  Plus: accept() now wakes the IO
     thread after resetting the listener (throughput was capped ~10/s
     by the 100ms select tick) and the accept-FAIL path resets the
     promoted listener (was left stuck CONNECTED = permanently deaf
     server, latent).
  Hunt method that cracked it: Error>>sunitAnnounce:toResult: override
  captured the ORIGINAL in-suite exceptions (all TestTookTooMuchTime),
  signalerContext stack walks located the stalls, PHARO_DNS_TRACE +
  PHARO_DELAY_DEBUG exonerated resolver+ticker, the unfiltered
  process-table dump at kill exposed ProcessMonitorTestService
  suspensions, and overriding handleUnhandledException: named the
  hidden errors.  Verification: Zn/Zdc family all green (ZnClient
  49/50 = network GeoIP only; SocketStreamTest 24/24 vs July-4's
  22/24); 22-class scheduler batch 1482 P / 0 F / 0 E.

---

# WIP — ARM re-verification of the Windows merge: fix wave COMPLETE (2026-07-04)

The Windows-session changes (dcacc401..155d9bc4) came back to ARM and broke
startup + several suites.  All blocking regressions root-caused and fixed on
`jit` (this session, ARM/macOS):

- fb1e0d85  interp: block-NLR dynamic home is a gated FALLBACK (fae4edc1
  override let a stale closure_ redirect NLRs on JIT block returns —
  10 varying wrong-receiver DNUs killed StPharo startup before
  SUnitRunner ever registered; ownership gate = closure_'s compiledBlock
  slot must BE method_).
- d6e88006  interp: image-protocol NLR unwind targets the C++-resolved
  home (ec631963's aboutToReturn: re-derived home via `self home`; an
  inline-J2J block callee has no frame, so startCtx was the CALLER and
  delivery landed one frame short — every ensure-crossing FreeTypeCache
  glyph hit returned the CACHE; now sends homeCtx return:value through:).
- 3d83430c  sista: flush interpHints_ on every moving GC (raw oop bits,
  no lifecycle — dangling targetMethod SEGV'd LinearLifter during
  WeakKeyDictionaryTest, even under PHARO_NO_JIT).
- fa02c604 + FFI exe-dir search + fixtures: libTestLibrary.dylib now
  built/staged next to test_load_image on macOS AND the FFI bare-name
  search covers the exe dir; TestLibrary.c gained sum_*enum +
  dereferencing unref_pointer; new primitiveGetObjectFromAddress
  (PointerUtils inverse).  TFFI batch: was 17E+1F+1T scattered -> 
  134 P / 0 F / 0 E / 7 skip.
- 829ddfbe  docs: the stale startup.st CWD trap (a Jun-20 leftover
  hijacked every stock-VM prep from the repo root — looked exactly like
  a scheduler wedge; cost half a day).

ARM verification COMPLETE (2026-07-04 evening).  Additional fixes en route:
- 94d462e5  NLR protocol shape (aboutToReturn: on homeCtx — stepping
  machinery pattern-match preserved) + dead-home liveness gate
  (BlockCannotReturn semantics).
- SIGPIPE ignored globally (SocketStreamTest write-after-peer-close
  killed the VM with exit 141; Windows has no SIGPIPE).
- vmParameterAt: 9 reports the real scavenge count (statisticsReport
  divides by it unguarded; ZnServer /status 500'd ZeroDivide — 28/31
  ZnServerTest errors + the Zn/Zdc timeout cluster, ~75 tests recovered).
- runner submodule: categorized vmRegisterAsDelayRecovery (SemaphoreTest
  lint), plus primitiveGetObjectFromAddress + TestLibrary enum-sum/
  unref_pointer fixtures.

FINAL ARM NUMBERS:
- Full catalog (2031 classes / 28023 tests, 3-part run): 27280 P = 97.35%
  BEFORE the Zn/SIGPIPE fixes; the re-measured Zn/Zdc cluster alone
  recovers ~75 (ZnServer 29/31, ZnClient 49/50, ZnEasy 10/10,
  ZnStaticFile 6/6, Zdc 4x15/15, SocketStream 22/24) -> ~97.6%+.
  Remaining non-passes are (a) context/env behaviors reproducing
  BYTE-IDENTICALLY on the Jun-25 pre-Windows baseline binary (Rub*/
  Renraku/Ring2/stepping cold-context timeouts), (b) environment gaps
  (no HTTPS => Zn remote-URL tests; Spec/GUI env; LibTTY; Roassal/
  Cairo partials; FL WideString runner artifact x5), (c) two parked
  deterministic items with full evidence in deferred.md (WKD
  testClearing warm JIT timing; ProcessTest testTerminateInTerminate —
  note baseline scores 37P/5T vs our 45P/1T there).
- soogle: STON 310/310 parity; Fuel failure-set BYTE-IDENTICAL to Cog
  (719/10/19 both); PolyMath 776 vs 777 (1 slow-test timeout); NeoJSON
  parity except one network-dependent test.  Bench suite healthy
  (fib(28)=7ms), GC purges verified live on ARM incl. the jitMethods
  W^X-flip path, ZERO tripwire firings all session.


---

# WIP — x86/Windows JIT fix list: COMPLETE (2026-07-04)

The 2026-07-03 fix list (#14 repeat-run wedge, #11 ObsoleteTest one-cycle
pin) is DONE, the full-suite goal gate passed above baseline, and the
teardown-segfault family found en route is fixed too.  All committed and
pushed on branch `jit`:

- e9a7e984  sched: callback-return requeue must preserve same-priority
  order — THE #14 wedge fix (with dcacc401's exactly-once hand-out
  underneath).  x4 gauntlet 5/5 clean runs, ~75s vs wedging forever.
  Full mechanism: docs/deferred.md (#14 entry) + memory
  `scheduler-order-invariant.md`.
- 27e4ca74  gc: weak-root treatment for VM caches (#11) — RootScope
  StrongOnly mark + purgeDeadCacheRoots; ObsoleteTest 3/3 x4,
  testFixObsoleteSharedPools at stock parity; dead classes no longer
  pinned by VM caches at all.
- 56997740  tffi: teardown segfault family — never free in-flight FFI
  resources (entry-captured retSize; cif graveyard w/ gated drain;
  unregistered-callback thunks leaked immortal; xtcb shutdown unparking;
  immortal cross-thread statics; DNS drain).  TFCallbacksTest exit-loop
  8/8 exit 0 (was 2/6).

FULL-SUITE GOAL GATE (run #25, on 27e4ca74): 2047/2047 classes,
27967 tests — 27674 pass (99.0%) / 52 F / 77 E / 155 skip / 9 timeout,
exit 0, ~5700s.  Baseline run #9: 27441 (98.1%); run #7: 377 E /
25 timeouts.  Net: +233 passes, errors -300, timeouts -16, ~45 min
faster.

## Deferred-items sweep COMPLETE (2026-07-04, second goal)

Every fix-shaped deferred item is now closed (commits bceefb37, af653a46,
ef61b868).  Highlights:
- TFCallbacksTest: 1/8+3F+4E -> **8/8+2skip STOCK PARITY**, five root
  causes (TestLibrary fixture arg+1 semantics; release-while-parked join
  freeze; buried-dead invocation hand-out; reentrant callouts needing
  the parked worker to service its own queue; missing xtcb adoption
  drain in nested callback loops — the last one also cured the warm
  UFFI in-suite flake).  Full story in deferred.md's TFCallbacksTest
  section.
- Verified-stale entries closed with evidence: WeakAnnouncer warm parity
  (fixed by 27e4ca74), NetNameResolver localhost (hostname prims),
  MicText HugeFont 21/21 (Cairo stack), InLoop(UsingWorker) 13/13,
  TFFI v2 (landed), SDL2/Morphic GUI parent entry (on-screen + input
  verified with screenshots 07-01/02).
- Silent-cap residue batch: loud-not-silent tripwires (STORE-OOB,
  FWD-CHAIN-CAP, NS-SCAN-TERM, BV-SAVE-GUARD, rate-limited SP-CORRUPT
  family), 3 stale callback-polling interceptions removed, fetchPointer
  nil-answer documented as API semantic (tripwire attempt false-posed).
- arc4random_buf -> BCryptGenRandom (links bcrypt); UUIDs verified
  distinct across runs.
- Closed by design: SIGSEGV recovery (dump-then-crash is the tool that
  solved the teardown family), chown ENOSYS, ARM64-Windows trampoline.

Remaining open (features/blocked, NOT fixes — see deferred.md):
CONC UsingWorker pacing (needs quiet-machine profiling; data captured),
IME, MIDI backend (unverifiable: no image-side MIDI classes),
WorldRenderer native fast path, old-space commit-ahead (design note
written; own-milestone risk), Authenticode signing (DONE 2026-07-05: user set up Azure Trusted Signing; latest Windows build ships signed) (previously: needs user cert
decision).

ENVIRONMENT CAVEAT for this session's numbers: the machine was degraded
3-4x from ~03:30 (WmiPrvSE at 12 CPU-hours from tasklist polling loops +
ESET scanning; benchFib 12ms -> 40-55ms) — ObsoleteTest's in-suite 0/3
during this window is the time-limit artifact (test body passes via
direct performTest); see memory wmi-polling-hazard.

## Environment quick-reference

- Build: `/c/temp/src/iospharo-jit/scripts/build-windows.sh` (MSYS2
  CLANG64).  Kill test_load_image.exe before rebuild (link EPERM).
- Probe image: /c/tmp/probe-img/Pharo.image.  Eval mode REQUIRES the
  `eval` keyword: `test_load_image.exe <image> eval "<expr>"` (bare
  launch boots the GUI idle and deletes the staged startup.st).
- Suite env: /c/temp/pharo-win-test/Pharo-sunit.image; run
  `... eval "(Smalltalk at: #SUnitRunner) runAllTests"`; results in
  /c/tmp/sunit_test_results.txt; clear /c/tmp/sunit_class_names.txt /
  sunit_batch.txt / sunit_run_completed.txt first.
- Crash triage: [WIN-CRASH] backtraces in the log; symbolize with
  `llvm-addr2line -f -C -e test_load_image.exe 0x14XXXXXXX`
  (0x140000000 + printed exe offset), against the SAME binary.
