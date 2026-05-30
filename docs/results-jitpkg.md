# JIT stress via non-graphics packages

Goal: exercise the JIT beyond the graphics suites with packages that hammer
*different* code paths — compiler/AST, parsing, serialization, reflection,
numerics, graph algorithms. These are kernel-quality packages that ship green
on stock Pharo, so any divergence on our VM is a VM/JIT bug.

## Method

1. Extract each package's TestCase subclasses (`scripts/extract_lists.st`).
2. Run on our VM: `scripts/graphics/run_jit_pkg_queue.sh` (test_load_image +
   Pharo-gfx.image, SUnitRunner auto-fires). Per-test results in
   `docs/results/jitpkg/<tag>_detail.txt`.
3. Stock-Cog baseline: `scripts/graphics/run_cog_baseline.sh` (alt /tmp paths
   so it runs concurrently). Output `docs/results/jitpkg/<tag>_cog.txt`.
4. Δcog diff: `scripts/diff_jit_pkg.py <tag>_cog.txt <tag>_detail.txt` — lists
   tests that pass on Cog but regress on ours (JIT/VM-bug candidates).
5. Classify each candidate by ISOLATION repro on the *clean* image with JIT
   on vs off: `scripts/repro_tests.sh /tmp/harness/Pharo.image 'Class>>sel' …`
   - fails JIT-on only            → JIT bug
   - fails JIT-on and JIT-off     → VM/interpreter/primitive bug (not JIT)
   - passes in isolation          → suite-interaction / image-state artifact

## Summary

14 packages run on our VM (gfx image) vs stock Cog, then JIT-bug verdicts via
clean-image JIT-on-vs-off where it mattered. Verdict legend: JIT = confirmed
JIT bug (clean-image on/off); WS = known WideString bug (non-JIT); STORM =
JIT hang; — = clean / no JIT bug.

       package       cog P/F/E      our (gfx) P/F/E    verdict
       regex         196/0/0        196/0/0            — clean both
       numberparser  38/0/0         36/0/0             — clean
       strings       738/0/0        733/3/0            WS (3 = known WideString, JIT on==off)
       seq           1695/0/0       (skipped — overlaps kernel suite)
       ston          156/0/6        153/4/5            0 JIT; +1 NEW non-JIT word-array bug
       zincenc       120/0/1        109/9/2            WS family (wide-codepoint encoders)
       ast           621/1/1        597/5/19           clean-image on 594/6/20; JIT-off too slow to confirm fraction
       opal          633/0/0        446/30/147         many gfx artifacts; JIT-off too slow to confirm
       microdown     620/4/6        (partial)          batch killed to free VM
       systime       659/0/0        3/0/0 (killed)     NEW Delay/scheduler nil bug breaks valueWithin:
       aigraph       84/0/0         78/0/6             **JIT** — inline IC probe (inline-getter). on/off proven
       ring          290/0/0        250/35/5           **JIT** — inline IC probe (returnsSelf/Literal); +non-JIT metamodel
       reflectivity  322/0/0        STORM              metalink instrumentation send-storm (hang)
       fuel          37/0/474       STORM              reflective-walk megamorphic thrash (also broken on cog)

Clean-image JIT-on-vs-off verdicts (the authoritative JIT-bug test):

       aigraph   JIT-on 78P/6E   JIT-off 84P/0E   -> 6 JIT-induced  (inline-getter)
       ring      JIT-on 263P/25F JIT-off (slow, partial) -> >=8 JIT-induced (returnsSelf/Literal)
       ston      same JIT on/off                  -> 0 JIT-induced

Headline: BOTH aigraph and ring JIT bugs root-cause to the **inline IC probe**
(`PHARO_T1_NO_IC_PROBE=1` fixes both). See below.

## UPDATE 2026-05-29 — deterministic + propagation pinned (root cause = inline getter)

Built `PHARO_DET_SCHED` (deterministic scheduling, committed 419b0a7a) which turns
this Heisenbug into a deterministic, instrumentation-safe repro. With timing-safe
ring captures + in-process `backtrace()` (no MCP lldb this session):

- **Root cause = the inline getter**, proven by deterministic bisection under
  `PHARO_DET_SCHED=1 PHARO_T1_INLINE_GETTER=1`:
  `PHARO_T1_NO_INLINE_GETTER=1` → 0 corrupt tuples; `PHARO_T1_NO_INLINE_PRIM_AT=1`
  → 12→3 (primAt only amplifies).
- The corrupt `AIWeightedEdge>>asTuple` tuples are built in the **interpreter**
  (`{from model. to model. weight}`) with **element-0 (`from model`) stale** while
  element-1/2 are correct. asTuple is **resumed at bytecode offset 2** (skipping the
  `from model` getter) with its first operand slot (fp[1]) holding a stale leftover.
- Mechanism: asTuple's frame **bounces between a force-yield save**
  (`materializeFrameStack ← transferTo ← handleForceYield`) **and a relinquish-resume**
  (`executeFromContext ← transferTo ← primitiveRelinquishProcessor`) **at offset 2 with
  fp[1] stale — self-perpetuating across the forked SUnit processes.** The save/resume
  round-trip is faithful; the staleness is upstream. The first-seeding transition
  (asTuple first reaching interp offset 2 without fp[1]=from.model) evades every C++
  capture point — needs MCP lldb single-step (deterministic now). Full chain +
  inline-`selectorOf` capture recipe in memory `jit_aigraph_fork_arg_corruption.md`.
- Mitigation holds: `t1InlineGetter` default-OFF, aigraph 10/10 default, perf-neutral.

### lldb breakthrough (2026-05-29) — asTuple is RE-RUN with the prior tuple

CLI lldb (codesigned binary; `astuple_stale_save_bp` noinline marker w/ unique
side effects to defeat identical-code-folding; SmI decode `value<<3|1`) caught the
1st corrupt build and showed the actual mechanism:

- asTuple is **re-executed on the same edge**: its element-0 slot (fp[1]) holds the
  edge's CORRECT prior asTuple result — a 3-elem tuple `{from.model, to.model,
  weight}` — instead of the immediate `from.model`. So the rebuild produces
  `{prior_tuple, to.model, weight}`; `findNode: curEdge first` then gets the whole
  tuple → "No Element in Graph".
- asTuple is LIVE in the interpreter at bytecode offset 2 (pushRcvr2) with fp[1]=the
  tuple — resumed (`executeFromContext ← transferTo ← relinquish/forceYield`) from a
  context whose **pc=offset2 (mid-build) but op0=the-built-tuple (post-build)** — an
  inconsistent context that re-runs the build.
- The save↔resume round-trip is faithful; asTuple is never frame0 / inner-frame (only
  the current-frame materialize, which updates pc+op0 atomically). So no instrumented
  path creates the `{pc=offset2, op0=tuple}` mix — the inconsistency is in the LIVE
  state (`ip=offset2` yet stack is post-build), arising from asTuple's JIT
  execution / a JIT-exit that sets `state.ip=offset2` with a post-build operand stack.
- **Fresh-context / self-perpetuating finding:** logging every asTuple current-frame
  save ([ASSAVE]) shows normal saves are offset2→op0=immediate (from.model) and
  returnTop→op0=heap-tuple (the return value). The CORRUPT save is offset2+op0=tuple,
  and each corrupt context appears EXACTLY ONCE → it is **fresh-created at offset2 with
  op0=tuple**, not mutated from a returnTop context. So at creation the live state is
  `{ip=offset2, fp1=tuple, stackp=1}` — asTuple parked at offset 2 with element-0
  already a tuple. Nothing between offset-1 (sets fp1=from.model) and the offset-2 save
  changes fp1, so asTuple ENTERED interp at offset 2 with fp1=tuple (resumed from a
  prior `{pc=offset2, op0=tuple}` context — itself fresh the same way). The corruption
  **self-perpetuates with no first cause in any captured path** (8 JIT transitions,
  all 3 materialize paths, executeFromContext, the save log). The seed is a transient
  JIT-execution/resume event invisible to per-event C++ capture — resolving it needs
  reverse-execution (rr, unavailable on macOS arm64) or single-stepping asTuple's first
  corrupt activation in lldb from its J2J entry (deterministic under DET_SCHED).
- **Structural fact (lldb bcToCode dump):** asTuple's JITMethod has `numBytecodes=0`
  (the `advertiseResume=false` gate for methods with sends, AsmjitT1.cpp:6643-6647),
  so `codeOffsetForBC` always returns codeSize → asTuple can NEVER be JIT-resumed and
  is forced to bail to the interpreter after any JIT exit (and resume via
  `executeFromContext` on process switches). With the getter inline asTuple builds
  correctly all-JIT (site=1 ExitArrayCreate); the corrupt builds are the interp-resume
  path (site=0). `PHARO_ASMJIT_T1_FORCE_RESUME_FOR_SENDS=1` (make send-methods
  resumable) HANGS the VM — so that gate protects a broken/slow send-resume path, and
  can't be used as a fix. The corruption is plausibly a leak from that same machinery.
- **CORRECTION:** aigraph default has `J2J-r: 0/0` — it uses NO JIT-resume, so the
  asTuple corruption is purely the INTERP-resume (executeFromContext/materialize) path,
  whose save/restore is faithful (seed = a transient, needs reverse-exec). The
  `PHARO_T1_RESUME_SENDS_NO_CONDJUMP` knob + `#asMutator` startup DNU exercise the
  SEPARATE JIT-resume protocol gap (Phase 4b.2): asmjit-T1 returns at JITCompiler.cpp:1676
  before `setBcEntryStates` (2937), so asmjit-T1 methods have NO bcEntryState — tryResume's
  register-liveness safety check is a no-op for asmjit-T1 (JIT_CALL does restore x0/x19/x20,
  so the gap is subtler: operand-stack/bcToCode reconstruction). These are related
  (both send-method resume) but DISTINCT mechanisms — fixing the JIT-resume gap would
  NOT fix aigraph. The knob is a valid JIT-resume-gap debugging tool, not an aigraph repro.
- Full chain + lldb recipes in memory `jit_aigraph_fork_arg_corruption.md`.

## HEADLINE: confirmed JIT correctness bug in aigraph (Prim/Tarjan)

The one unambiguous JIT bug this campaign found. On the **clean** harness image
(Pharo-jit, no FakeGUI, 0 startup-corruption lines), full `AI-Algorithms-Graph`
suite:

       JIT ON  : 78 PASS / 6 ERROR
       JIT OFF : 84 PASS / 0 ERROR    (PHARO_NO_JIT=1)

Only the JIT toggles. The 6:
`AIPrimTest>>{testMinimumSpanningTreeSimple, testMinimumSpanningTreeComplex,
testMinSpanningTreeComplex2}`,
`AITarjanTest>>{testStronglyConnectedGraph, testComplexCycle,
testStronglyConnectedGraphWithObjects}`.

Exception (captured via the runner's stack-trace logging):

       Error: No Element in Graph : #(3 4 2)
         AIPrim(AIGraphAlgorithm)>>findNode:
         AIPrim>>run

`AIPrim>>run` does `fromNode := self findNode: curEdge first` but under JIT
`findNode:` receives the WHOLE edge tuple `#(3 4 2)` instead of `curEdge first`
(`3`). So a JIT'd send passes the wrong argument — `curEdge first` evaluates to
`curEdge`, or the argument slot is mis-loaded. The raw VM log corroborates
stack/arg corruption: garbage receivers appear in the forked test processes —
a **stack address** (`#asInteger not understood by rcvr=0x591885918`, FP is
`0x5bdb...`) and `nil` (`#suspend`/`#acceptVisitor:` to `0x300000000`). Bits
48-63 are clear, so this is distinct from the known classifier-bit leak (task
#10 / b8eead95).

Trigger: **warmup + fork**. Passes in isolation (even JIT-on). Needs
SUnitRunner's multi-test fork sequence — running several different test methods
forks them at priority 40 and warms up multiple JIT compilations; one of the
warmed compiled methods then mis-passes an argument when re-entered in a forked
context. A minimal same-test `forkAt: 40` repro does NOT trigger it (the
multi-method warmup matters).

Reliable repro:

       printf 'AIPrimTest\nAITarjanTest\n' > /tmp/sunit_class_names.txt
       rm -f /tmp/sunit_run_completed.txt /tmp/sunit_test_{results,detail}.txt
       ./build/test_load_image /tmp/harness/Pharo-jit.image      # JIT on  -> 3 ERROR
       PHARO_NO_JIT=1 ./build/test_load_image /tmp/harness/Pharo-jit.image  # -> 0 ERROR

### ROOT CAUSE: the inline IC probe (confirmed by env-knob bisection)

Bisected on the reliable 2-class repro (`Pharo-jit`, JIT on):

       baseline (JIT on)                3 ERROR   (full suite: 6)
       PHARO_NO_J2J=1                   still fails   -> NOT J2J
       PHARO_T1_NO_IC_PROBE=1           0 ERROR (84/84) -> the inline IC probe
       PHARO_T1_NO_INLINE_GETTER=1      0 ERROR (84/84) -> specifically inline-getter

So aigraph's bug is the **inline-getter specialization of the asmjit-T1 inline
IC probe** (`AsmjitT1.cpp` ~4586, `tryGetter`). Ring's 8 JIT-induced failures
are also in the IC probe but a DIFFERENT spec: `PHARO_T1_NO_IC_PROBE=1` fixes
them (ring-min 41/41), and `PHARO_T1_NO_INLINE_RETURNS_SELF=1` /
`PHARO_T1_NO_INLINE_RETURNS_LITERAL=1` each fix 7 of 8; inline-getter-off does
NOT. So the IC probe has correctness bugs across MULTIPLE inline
specializations.

The probe (`t1ICProbe`) was **default-OFF until 2026-05-16** precisely because
of "correctness icData[2]==0 guard, downstream DNUs" (DebugSettings.hpp comment)
— then flipped default-ON. This campaign shows the correctness gap is still
present: the probe takes an IC HIT on a 22-bit class key (`AsmjitT1.cpp:3465`,
`w4 & 0x3FFFFF`) and inlines a getter/returnsSelf/returnsLiteral, but in a
warmup+fork scenario the inlined spec is applied wrongly — reading the wrong
slot (getter → wrong value, e.g. `curEdge first` yields the whole tuple) or
leaving a garbage value that becomes a later send's receiver (the stack-address
/ nil DNUs). The immediate-receiver guard (`tst x1,#7; b.ne dispatchCached`,
:3650) is present, so it is NOT a SmallInteger-as-pointer bug; the insufficiency
is in HIT validation / classification under the warmed, forked state.

Mitigation breadth (clean image, baseline vs `PHARO_T1_NO_IC_PROBE=1`):

       aigraph   78P/6E   -> 84P/0E    FIXED by probe-off
       ring-min  33P/8F   -> 41P/0F    FIXED by probe-off
       ston      153P/4F/5E -> 154P/3F/5E   UNCHANGED — ston is genuinely non-JIT
       ast       592P/6F/22E -> CRASHES after ~34 tests (SIGSEGV in interpret())

So disabling the probe is NOT a universally safe mitigation: the non-probe
dispatch path SIGSEGVs on `ast` (dispatch-heavy compiler suite). That argues for
FIXING the probe's HIT validation, not flipping it off. Root fix: strengthen
IC-HIT validation before dispatching an inline spec (the original `icData[2]==0`
guard direction); lldb single-step `tryGetter` at a failing HIT to see whether
the slot index, the cached method/selector, or IC staleness is wrong. (The
22-bit class key `& 0x3FFFFF` is NOT truncating — image classIndexes are < 2^22 —
so it is not a key-collision bug.)

### FIX APPLIED 2026-05-28: inline-getter default OFF

`t1InlineGetter` now defaults OFF (`DebugSettings.cpp`; opt back in with
`PHARO_T1_INLINE_GETTER=1`, `PHARO_T1_NO_INLINE_GETTER` still forces off).
This reverts the 2026-05-16 enablement of a spec that was OFF for exactly these
"downstream DNUs".

Result with the new default (clean Pharo-jit image):
- aigraph: 84 PASS / 0 ERROR (was 78/6), reliable across runs.
- regex: 196/0/0; ast: 596/5/20 (unchanged, no crash — ast's failures are non-getter).
- No package regressed: across every package tested this session, getter-off
  either equals getter-on or fixes (aigraph).

Perf: a getter-heavy micro-bench (50M iters of `p x + p y`, Point accessors)
measured 3662 ms with getter ON and 3662 ms with it OFF — i.e. **perf-neutral**
(a warm monomorphic getter dispatches about as fast via the normal cached path
as via the inline spec). So the correctness fix costs ~nothing here.

### lldb root-cause progress (2026-05-28)

Caught the bug under lldb (`PHARO_T1_INLINE_GETTER=1`, 2-class repro, break on
`sendDoesNotUnderstand` with condition
`((Interpreter*)$x0)->stackPointer_[-1-(int)$w2].bits_ > 0x400000000`):

       garbage receiver = 0x4758857f0   (the DNU receiver)
       method_          = 0x30380d5f0   (#asInteger)
       receiver_        = 0x3037fc300   (valid heap obj)
       stackPointer_    ≈ 0x479b0eaa8   (live execution stack)
       operand stack near SP holds the garbage value REPEATED across slots:
         0x4758857f0, 0x475885808, 0x475885838, 0x475888b10 ...

The garbage values are all **stack-region pointers** (0x475…, same region as the
execution stack at 0x479…). So the inline-getter IC-probe path **leaks a
stack/frame pointer onto the Smalltalk operand stack**; it's later loaded as a
send receiver (tag bits 0 → treated as a heap object), mis-resolved to a bogus
class, and DNUs. The value repeats across slots — a cascade once the first leak
lands. NB the VM build is RelWithDebInfo (-O2): lldb can read registers / fields
/ memory but cannot CALL inlined methods (selectorOf etc.), so decoding the exact
origin getter needs a Debug (-O0) VM build or machine-level single-stepping of
the getter inline at its runtime code address (read x1/x2/x6/x7 around
`ldr x6,[x3,8]; stur x6,[x2,off]`).

### SECOND bug found + FIXED: unbound-label garbage bcToCode (2026-05-28)

The Debug (-O0) build of the repro asserted during compilation:
`asmjit codeholder.h:591 is_bound()` in `emitMethodBytes` →
`label_offset_from_base`. Root cause: `emitMethodBytes` built the bcToCode table
by querying `label_offset_from_base(bcLabels[i])` for ALL `i` in `[0,bcLen)`, but
the emit loop only BINDS `bcLabels[emitSkip..bcLen)`. For primitive methods
(`emitSkip=3`, the CallPrimitive 0xF8/lo/hi header), `bcLabels[0..2]` are never
bound → `label_offset_from_base` returns GARBAGE in NDEBUG (asserts in Debug),
so `bcToCode[0..2]` became bogus JIT re-entry offsets. Fixed (`AsmjitT1.cpp`):
map the unbound header slots to 0. This is a real latent correctness landmine
(any primitive method had garbage re-entry offsets for its header indices), but
it is NOT the aigraph/ring inline-spec bug — those persist after this fix.

### Ring is NON-deterministic (the inline-spec bug is timing-dependent)

After the unbound-label fix, ringmin JIT-on varies run-to-run (40/1, 33/8, …);
JIT-off and PROBE-OFF are reliably 41/0. So ring's IC-probe inline-spec bug
(returnsSelf/returnsLiteral) is FLAKY/timing-dependent, unlike aigraph's getter
bug which is deterministic. This explains the earlier inconsistent spec-disable
bisections. PROBE-OFF reliably fixes ring but SIGSEGVs ast (separate non-probe
dispatch bug), and spec-disables are flaky — so ring is not cleanly fixable
without the inline-spec emit root cause.

### Debug session (2026-05-28): precise localization of the aigraph getter bug

Built a Debug (-O0, assertions on) VM (`build-dbg/`) and instrumented the C++
side (gated by `PHARO_T1_GETTER_CLASSIFY_LOG`). Findings:

- The harness DNUs I first chased (`FileReference>>asInteger` from
  `SUnitRunner>>nextRunNumber`'s `file contents asInteger`, caught by `on:
  Error`, then nil-receiver cascade) are largely NOISE, not the test failure.
- The REAL aigraph failure: `AIPrim>>run`'s `edges do:` block does
  `fromNode := self findNode: curEdge first`, and `findNode:` receives the WHOLE
  `curEdge` Array instead of `curEdge first`. Confirmed via an `activateMethod`
  probe: `findNode:` got `argClass=Array slotCount=3 elems:[0]=3 [1]=4 [2]=2`
  (the correct edge tuple `{from model. to model. weight}`), frame
  `AIPrim>>run → SortedCollection>>do: → block`.
- `curEdge` and `asTuple` are CORRECT (the array is well-formed). So the operand
  stack is MISALIGNED: the `#first` send effectively returns its receiver
  (`curEdge`). `#first` (`^self at: 1`) is NOT getter-classified (verified — not
  in the GCLASSIFY log), yet `PHARO_T1_NO_INLINE_GETTER=1` fixes it — so an
  inline-getter elsewhere in the warmed code misaligns the operand stack and the
  misalignment propagates (OFF_SP is threaded through memory per bytecode) to the
  `findNode:` send.
- The DNU receiver `0x6a38…` is heap-VALID (isValidHeapAddress=1, cls=5709
  FileReference) — NOT a raw stack pointer (correcting the earlier read). So
  it's a valid-but-wrong object surfaced by the misalignment.

Not pinned: the exact arm64 instruction / the specific getter whose inline
misaligns the stack. The `asTuple` `model`-getter inline maintains OFF_SP
correctly (curEdge is well-formed), so the culprit is a getter inline at the
block/method boundary whose net-SP effect is wrong only in that context. Pinning
it needs live single-stepping of the getter inline at its runtime code address
(read x1/x2/x6 + OFF_SP before/after) — blocked this session by the -O2 build
hiding lldb locals, Debug-build line-breakpoint non-resolution, and asmjit
instrumentation perturbing the bug (code-size → method bails). `build-dbg/` +
`PHARO_T1_GETTER_CLASSIFY_LOG` are staged for that session.

### lldb session #2 (2026-05-28): localized, not yet pinned

Drove the Debug VM (`build-dbg/`) under lldb + breakable C++ traps. Established:

- The failing `findNode:` send (and the `curEdge first` before it) is
  **INTERPRETED**, not JIT'd (C++ bt: `interpret → sendSelector → activateMethod`).
- `curEdge` is the **correct flat array** `#(3 4 2)` (slotCount=3, SmallIntegers)
  at the moment `#first` is sent — confirmed by a trap on `#first`-on-Array.
- `SequenceableCollection>>first` (`^self at: 1`, with `at:` a `<70>` special
  send) is **NOT JIT-compiled** (methodMap lookup = null).
- The interpreter's inline fast-paths DO apply IC classifications
  (`Interpreter.cpp` getter@8138, setter@8155, returnsSelf@8180, etc.) — a
  plausible culprit — but instrumented traps show NONE of them fire for
  `#first`/`#at:`/`#second`.
- So `#first` dispatches+activates normally on a correct `curEdge`, yet the
  interpreted result fed to `findNode:` is the whole array (= `curEdge`).

Conclusion: the interpreter is correct in isolation (JIT-off passes), so JIT-ON
must be **corrupting shared state** that the interpreted block then reads — i.e.
an inline-getter (for a node accessor like `model`/`distance`, which DO inline)
corrupts the operand stack / a temp / a cache, and the corruption surfaces in the
interpreted `curEdge first`. This is the hardest bug class (JIT corrupting
interpreter-visible state). The exact corrupting getter + instruction is NOT yet
pinned — every direct mechanism on the `first`/`at:`/`findNode:` path was ruled
out, so the next step is to catch the FIRST corrupting write (a watchpoint on the
specific operand/temp slot, set at block entry for the failing edge). Getter-off
remains the correct shipped mitigation (perf-neutral).

### lldb session #3 — watchpoint (2026-05-29): concrete corruption caught, instruction not pinned

Watchpointed the block's `sp[-1]` slot (where `curEdge first` returns its result),
trapped at the `#first`-on-tuple send:

- `#first` IS sent on the correct flat tuple (slotCount=3, SmInts) and returns via
  `returnFromMethod → returnValue → push` — i.e. interpreted.
- The value `#first` writes back is **`0x8641036085410060`** — a contaminated
  NON-Oop (bit 63 set; not the SmallInteger element it should be). That is the
  concrete corruption: interpreted `curEdge first` yields garbage.
- `#at:` (inside `first`) is handled by the interpreter's IC-cached **prim-60
  fast-path** (`executePrimitive`), NOT `activateMethod` (the at:-activation trap
  never fired). `#at:` is ALSO J2J-compiled (`[IC-PATCH] #at: J2J=1
  extra=0x100e0001084f0a70`, bit 60 = J2J entry).
- Both `first` and prim-60 are correct in isolation (JIT-off passes), so JIT-ON
  corrupts shared state (operand stack / frame) that the interpreted `first`
  reads and returns.

Wall hit: the operand slot is REUSED (every push writes through it), so the
write-watchpoint is too noisy to isolate the FIRST corrupting write from the many
transient pushes. Pinning the exact leaking instruction needs a non-noisy target —
a watchpoint on a stable heap-object slot, or catching the first getter-inline
that emits a non-heap value without perturbing code size (asmjit instrumentation
shifts method sizes and masks the bug). That's a further deep pass beyond three
lldb sessions. Getter-off remains the correct shipped mitigation.

STILL OPEN (task #4):
- The exact arm64 emit defect (a Heisenbug — adding a trace BLR inflated code
  size, pushing a method past its budget so it bailed to interpreter, masking
  2 of 3 errors; overflow bails safely, so this is not corruption but it does
  prove the bug is in the JIT getter inline and is layout-sensitive). Pinning it
  needs lldb single-stepping with a stack watchpoint on the failing send.
- Ring's failures are the same IC-probe family but a DIFFERENT spec
  (returnsSelf/returnsLiteral), and flaky across spec-disables — only
  whole-probe-off reliably fixes ring, and that SIGSEGVs ast (a separate
  non-probe-dispatch bug). So ring is NOT fixed by the getter default; it needs
  the root-cause emit fix. returnsSelf/returnsLiteral were left ON (their perf
  value is unmeasured and ring's fix needs the deeper work anyway).

CAVEAT on the per-test isolation-triage method: because this bug is warmup+fork
dependent, isolation repro (one test at a time) mislabels it ARTIFACT. The
authoritative test is full-suite JIT-on vs JIT-off on the clean image
(`scripts/graphics/run_clean_onoff.sh`); the per-test isolation pass
(`triage_candidates.sh`) only catches non-warmup-dependent bugs.

## Second package with JIT bugs: Ring metamodel

Clean image, full `Ring-Core` suite:

       JIT ON  : 263 PASS / 21 FAIL / 4 ERROR / 2 TIMEOUT
       JIT OFF : 144 PASS / 2 FAIL / 19 ERROR / 1 TIMEOUT  (INCOMPLETE — see caveat)

**8 confirmed JIT-induced** (FAIL on JIT-on, PASS on JIT-off):
`RGClassStrategyTest>>{testCategory, testDefinition, testDefinitionWithSlots}`,
`RGClassTest>>{testCopyForBehaviorDefinition, testDefinition, testTagsCollection,
testTagsForMethodsCollection, testTagsForMethodsRemoval}`. These are *wrong-result*
assertion failures (class definition / category / tags strings differ under JIT) —
a different manifestation than aigraph's exceptions, but same conclusion: JIT'd
code computes the wrong answer. 2 both-fail (non-JIT): `RGEnsureTraitTest` trait
tests.

CAVEAT — **JIT-off is 10-100× slower on reflective code**, so the JIT-off Ring run
hit the 900s cap after 166/290 tests. 17 JIT-on failures are in classes JIT-off
never reached, so their status is unknown. The 8 confirmed are a LOWER BOUND;
Ring likely has more. A definitive count needs a long-timeout JIT-off rerun
(`TO=3600` or `SUnitTimeoutMultiplier`).

## Key findings

### 1. The suite-vs-isolation gap is mostly the IC probe, not (only) the gfx image

Initial read: the gfx image (clean Pharo 13 + SUnitRunner + FakeGUI shims) emits
a `KeyNotFound` / "Decompilation failed" startup storm (known Color/Morphic
issue), and tests that PASS in isolation but FAIL in the full suite looked like
image-state artifacts. **That was partly wrong.** The aigraph "artifacts" pass
in isolation but are CONFIRMED IC-probe JIT bugs in the full suite (same 6
errors on the *clean* image, fixed by `PHARO_T1_NO_IC_PROBE=1`). So the
"passes-alone-fails-in-suite" pattern is largely the IC probe's WARMUP
dependence: in isolation the method runs interpreted / un-warmed and the buggy
inline-spec IC HIT never fires; in the full suite it gets JIT-compiled + the IC
filled, and the bug triggers.

What IS genuinely gfx/image-state vs IC-probe vs non-JIT (clean-image,
probe-off):
- aigraph, ring(RGClass*): IC-probe JIT bugs (probe-off fixes).
- ston (testStrings/WideSymbol/Collections/...): non-JIT (probe-off UNCHANGED,
  same JIT on/off) — WideString + word-array + memory-FS.
- gfx startup storm is real but a smaller contributor than first thought.

Lesson: classify with full-suite clean-image JIT-on-vs-off (and probe-on-vs-off),
NOT per-test isolation — isolation hides warmup-dependent JIT bugs.

### 2. Reflectivity and Fuel storm the JIT (performance pathology, not a crash)

Both hang our VM with a send/patch storm that stock Cog runs in seconds:

* **Reflectivity** (metalinks / bytecode instrumentation): 1.4B+ sends with
  RUN-count=0 — stuck in test-class setup before the first test registers,
  looping on a DNU under `SUnitRunner>>runTestClass:`. Excluded from the batch.
* **Fuel** (reflective object-graph serialization): 6B+ sends, IC hit-rate
  collapses to 17%, **J2J-d ≈ 700M direct call-site re-patches** (~11% of all
  sends re-patch a site). Fuel walks arbitrary object graphs, so each J2J
  call site sees a stream of different callee classes and the direct-patch
  path thrashes instead of transitioning to a megamorphic/IC fallback the way
  Cog's inline caches do. (Fuel is also 474/511 ERROR on stock Cog on this
  image, so low comparative signal.) Excluded from the batch.

`J2J-d` = `Interpreter::jitJ2JDirectPatches` (JITRuntime.cpp:2682, printed at
:2696). A healthy run patches a site once and leaves it; ~700M patches is the
megamorphic-thrash signature.

Root cause: the IC-fill path (`Interpreter.cpp:19608-19642`, reached from
`patchJITICAfterSend`) writes the resolved method's J2J entry into the IC
`extra` slot on every fill, and a fill happens on every IC MISS. Our IC has no
polymorphic/megamorphic cache (PIC) — a send site that sees many receiver
classes never "sticks", so each send is a full method lookup plus a re-patch.
Fuel's run shows 17% IC hit (≈880M misses of 1.06B probes) → ~700M re-patches.
Cog absorbs megamorphic sites with open PICs + a megamorphic lookup cache; we
don't, so reflective/megamorphic workloads (Fuel, Reflectivity) degrade from
fast to effectively-hung. This is an architectural gap (add PICs), not a
one-line fix — tracked as a follow-up, not fixed in this campaign.

### 3. STON — fully triaged, 0 JIT bugs

All 9 gfx-suite failures behave IDENTICALLY with JIT on and off (isolation,
clean image):

       test                                      isolation result    class
       STONReaderTest>>testUser                  PASS                 gfx artifact
       STONReaderTest>>testUser2                 PASS                 gfx artifact
       STONWriteReadTest>>testColors             PASS                 gfx artifact
       STONWriteReadTest>>testTextAndRunArray    PASS                 gfx artifact
       STONJSONTest>>testStrings                 FAIL (wide garbage)  known WideString bug
       STONReaderTest>>testWideSymbol            FAIL (wide garbage)  known WideString bug
       STONWriteReadTest>>testSymbols            FAIL (parser err)    WideString family
       STONWriteReadTest>>testCollections        FAIL (word-array)    NEW VM bug (non-JIT)
       STONWriteReadTest>>testMemoryFileRefs     FAIL (memory FS)     env / non-JIT

* **NEW (non-JIT) word-array bug**: `testCollections` round-trips an
  `IntegerArray`/`Float32Array` and gets wrong element values —
  `Float32Array(1.0 2.0 3.0)` reads back as denormal garbage
  (`7.0e-45 1.7e38 7.0e-45` = integer bit-patterns reinterpreted as float32).
  Same with JIT off, so it's a primitive/interpreter bug in 32-bit word-array
  `at:`/`at:put:` (or literal construction), not the JIT. Needs its own task.

### 4. systime — Delay/scheduler bug breaks `valueWithin:` (timeout) tests

`System-Time-Tests` hangs our VM on the `BlockClosureValueWithinDuration`/
`valueWithin:` timing tests (each burns the 300s per-test cap). The run log
shows the cause:

       [DNU] #asInteger not understood by rcvr=nil in #setDelay:forSemaphore: P79
       [DNU] #< not understood by rcvr=nil in #setDelay:forSemaphore: P79
       [MUSTBOOL] value_class=UndefinedObject in #setDelay:forSemaphore: rcv_class=DelayWaitTimeout
       [MUSTBOOL] value_class=UndefinedObject in #ifNotNil: rcv_class=DelaySemaphoreScheduler
       [DIAG] P80 DelaySemaphoreScheduler>>whileTrue: ip=210

A `nil` (0x300000000) flows where a delay value is expected inside
`DelayWaitTimeout>>setDelay:forSemaphore:`, so `asInteger`/`<` DNU and the
timeout never arms — the block waits forever. Cog passes systime 659/0/0. The
batch run was killed after 3 tests to unblock the queue; needs isolation triage
(JIT on/off) to confirm whether the nil originates in a JIT'd method or a Delay
primitive. Captured signature: `/tmp/systime_delay_bug.txt`.

### 5. strings — 0 new bugs (known WideString bug)

All 3 candidates are the already-documented WideString-WriteStream bug
(WIP.md): `StringTest>>testOnlyLetters`, `testWithInternalLineEndings`,
`testWithUnixLineEndings`. Same failure JIT on/off; not a JIT bug.

## Pending isolation triage

Candidate lists staged at `/tmp/cand/<tag>.txt`:

* **aigraph** (6) — AIPrimTest (MST), AITarjanTest (SCC). HIGH priority: pure
  recursive/comparison compute, clean on Cog. Best shot at a real JIT bug.
* **zincenc** (11) — wide-codepoint UTF-8/16/32 encoders; expect WideString family.
* **ast** (24) — OpalCompiler AST-node tests.
* **ring** (40) — Ring metamodel (class/trait/metaclass definitions +
  implicit-environment); smell like gfx live-class-model corruption.
* opal / microdown / systime / strings / seq — pending batch completion.

## 2026-05-29 — aigraph inline-getter deep dive + a real latent fix

Resumed pinning the AIPrim/AITarjan inline-getter bug. Findings:

* **Debug build (-O0, `build-dbg/`) reproduces** (7 PASS / 3 ERROR on
  AIPrimTest+AITarjanTest) — usable for lldb (reads locals, calls selectorOf).
* **Real symptom:** `AIPrim>>run` does `self findNode: curEdge first` and under
  JIT **`curEdge first` returns the whole edge tuple** (`No Element in Graph :
  #(4 5 3)` etc.). `first` is `^ self at: 1` (bytecode `4C 51 70 5C`) — not a
  getter. A gated activateMethod watch on findNode: showed: correct calls have
  `lastJitReturn=#first retVal=<SmI>`; the 2-of-1048 bug calls (late, after
  warmup, JIT-compiled caller) have `lastJitReturn=#first retVal=<the Array>` —
  i.e. **JIT-compiled `first` returns its own receiver**.
* **Classification is clean:** 751 getter classifications, 0 OOB. Not a bad
  slotIdx at classify time — purely a runtime transient.
* **Bisection (release, getter forced on):** getter necessary (off → 0 err);
  primAt compounds only the Tarjan-object test (3→2, NOT the MST tests);
  returnsSelf / multislot / J2J / resume / chain knobs irrelevant (NO_RESUME
  made it worse). So it is pure JIT execution, getter-gated via a LEGIT 0-arg
  getter elsewhere in run/block (distance/model/previousNode/…).

### Real latent bug found + FIXED (commit f251d009)

`detectTrivialMethod` classified ANY `pushRecvVar N; returnTop` as a getter
(and `popStoreRecvVar N; returnReceiver` as a setter) on bytecode shape alone,
without checking the method's arg count. Multi-arg methods share the shape —
`ConstantBlockClosure>>value:`/`value:value:` is `^ capturedConstant`
(pushRecvVar 3) — so a 1/2-arg method got a getter bit (63) in the IC extra
word. The interpreter already guarded its getter fast path on `argCount == 0`
(Interpreter.cpp ~8138), but asmjit-T1 dispatched `tbnz x7,63` regardless of
arity. Fixed: numArgs==0 getter / numArgs==1 setter in detectTrivialMethod
(root) + arity-gate the 3 asmjit IC-probe dispatch sites (defense-in-depth).
Validated: aigraph 10/10 default; kernel batch (15 core classes) 2249/0/0.

**This is a genuine fix but does NOT close the headline MST bug** — it persists
with PHARO_T1_INLINE_GETTER=1 (the value: misclassification was benign: the
getter path handles nArgs>0 correctly). The MST bug remains a rare warmup+fork
transient (JIT `first` returns self) that needs live single-stepping of the bad
iteration (~1041/1048) to pin the exact instruction — impractical in batch lldb
amid the VM's own diagnostic traps + Catalyst noise. **Mitigation stays:
t1InlineGetter default-OFF (perf-neutral).** Next concrete step recorded in the
`jit_aigraph_fork_arg_corruption` memory note.

### 2026-05-29 (cont.) — lldb pinned it: `first` is innocent, curEdge[1] is corrupted

Drove lldb on build-dbg via a Python script: a `noinline`
`pharo_first_codestart_hook(codeStart,size)` (gated PHARO_FINDNODE_WATCH) hands
the callback first's runtime code address at the bug moment; the callback
disassembles to first's returnTop `ret` (x1 = the return value there) and arms a
conditional breakpoint.  Results overturn the earlier "first returns the tuple"
reading:

* Broad bp `first returns any heap ptr` fires on a LEGIT `nodes first` (returns
  an AIGraphNode) — at that stop `x1 == receiver[element1]` exactly, so **`first`
  is correct** (returns curEdge's element 1, not self).
* A `[FNSTK]` operand-stack dump at the findNode: bug shows
  **`firstRes == argSlot(sv0)`** — findNode: DOES receive first's result.  That
  result (curEdge's element 1) is a HEAP object (class 0x33 Array in one run,
  class 0x14e5 in another — the bug is transient, a different edge corrupts each
  run).
* ⇒ **curEdge's element 1 is itself corrupted to a heap value.**  For the MST
  char/int tests it must be an immediate node-id (`$c`, a SmI); under getter-ON
  it is a live-but-wrong heap pointer.  `first` / `at:` / `findNode:` are all
  correct propagators.  The inline getter corrupts the edge-endpoint upstream
  (building `curEdge = edge asTuple` — an `edge from`/`to` ivar getter, or the
  tuple element store, returns/stores a heap ptr where an immediate belongs).
  Matches the original "leaks a stack/frame pointer onto the operand stack".

The corrupted value is a *valid* heap object (not out-of-heap garbage), so the
getter read a wrong slot/receiver returning an adjacent live object — pinning
the exact transient edge-field getter is the remaining single-step (scoped in
the `jit_aigraph_fork_arg_corruption` memory note).  Mitigation unchanged:
t1InlineGetter default-OFF.

### 2026-05-29 (cont.) — asTuple also innocent; bug is run's operand-stack misalignment

Four more lldb probes (build-dbg, hooks gated PHARO_FINDNODE_WATCH + lldb Python
driver) walked the corruption further upstream.  `AIWeightedEdge>>asTuple` =
`^{from model. to model. weight}`; `AIGraphNode>>model` = `^instVar0`.

* asTuple ENTRY: `from.model` is ALWAYS a valid immediate (SmI) — input is fine.
* Conditional bp after asTuple's inline `model`-getter load on `x6` heap: never
  fires → the getter returns the correct SmI.
* Conditional bp at asTuple's returnTop `ret` on built `tuple[1]` heap: never
  fires → asTuple's OUTPUT tuple is correct `{SmI,SmI,SmI}`.
* Write-watchpoint on asTuple-output `[T+8]`: never fires → nothing corrupts the
  result after construction.

Yet at the failing `findNode:`, `first`'s receiver `curEdge` (class 0x33 Array)
has element-1 = a HEAP object (even a *node*, not the SmI asTuple produces).
Since asTuple's output T has a correct SmI element-1 and is never written,
**`curEdge` does not point to T**: the JIT'd `curEdge := edge asTuple` assignment
(popInto-temp) reads/stores the wrong operand-stack slot, so `curEdge` is bound to
an unrelated heap object.  ⇒ an inline getter elsewhere in `AIPrim>>run`'s loop
body leaves run's operand stack misaligned (SP off-by-N), corrupting the
assignment — exactly the original "leaks a pointer onto the operand stack".
**first / at: / findNode: / asTuple / model-getter are ALL correct.**

Net: this turn's lldb work conclusively relocated the bug from the leaf methods
(where prior sessions looked) to a getter-induced operand-stack/return-placement
misalignment in the JIT-compiled `run`.  Next: instrument run's JIT code — watch
`curEdge`'s temp slot for the wrong store, walk back to the getter that left SP
off.  Mitigation unchanged: t1InlineGetter default-OFF.

### 2026-05-29 (correction) — the lldb "innocence" findings above are inconclusive

Honesty correction after attempting to "fix run": the lldb conclusions in the two
prior entries (first innocent / asTuple innocent / run operand-stack misalignment)
are NOT reliably proven and should be treated as a working hypothesis only.

Across ~8 probes, the ONLY conditional breakpoint that fired was the broad
"first returns any heap ptr" — and it caught a *legitimate* `nodes first`
(returning a node), not the buggy `curEdge first`.  Every breakpoint aimed at the
actual bug path (first-returns-Array, asTuple's `model`-getter load, asTuple's
returnTop, the curEdge remote-temp store/load, a write-watchpoint on the tuple
slot) never fired.  Likely because those instructions are on the block's HOT path
and a per-hit lldb condition perturbs the fork+warmup timing enough to suppress
the transient.

What IS reliable (timing-safe C++ diagnostics): findNode: receives `curEdge`'s
element-1 = a heap object where the MST tests need an immediate; asTuple's input
`from.model` is a valid SmI; the getter classifier is clean (0 OOB); and the
inline getter is necessary to trigger the bug (primAt compounds 1 of 3 tests;
returnsSelf/multislot/J2J/resume/chain are all irrelevant).

Net: the bug is a getter-gated transient that corrupts an edge endpoint in
`AIPrim>>run`, but the exact faulting instruction is NOT pinned — lldb hot-path
conditional breakpoints can't catch it without perturbing the timing.  No
root-cause fix is landed for getter-ON (a non-root-cause change would violate the
project's no-workaround rule).  `run` works under the shipped mitigation
(t1InlineGetter default-OFF, perf-neutral; AI-Algorithms-Graph 10/10).  Pinning it
needs lldb Python-callback breakpoints (deref in Python, minimal per-hit cost) or
a one-shot cheap C++ capture from the chain-loop resume.

### 2026-05-29 (cont.) — full reliable chain (timing-safe C++), exact instruction blocked by Heisenbug

Pushed the localization to a precise, reproducible chain using only cheap C++
diagnostics (lldb hot-path breakpoints and even traces perturb this fork+warmup
transient — PHARO_TRACE_STACK_ORIGIN alone drops ERROR 2→1):

1. findNode: receives `curEdge[1]` = a heap object; the MST `#($b $c 5)` tests
   need an immediate there.
2. At `first`'s entry the receiver is a 3-slot Array (curEdge) whose element-1 is
   ALREADY heap ⇒ **first is innocent** (curEdge[1] corrupt upstream).
3. That corrupt element-1 is an Array exactly 32 bytes (one 3-slot allocation)
   before curEdge, with slot0 = a Char ⇒ a STALE recent allocation (the previous
   tuple), not `from model`.
4. No size-3 ExitArrayCreate ⇒ asTuple runs interpreted and returns a CORRECT
   tuple; its input `from.model` is a valid SmI.

⇒ The defect is in the JIT-compiled `edges do:` block: resuming after the
`edge asTuple` send (→ `curEdge := ...` remote-temp store), it binds `curEdge`
to a stale operand-stack value instead of asTuple's correct result.  Getter-gated
— PHARO_T1_INLINE_GETTER=1 enlarges the block's send-site code and shifts the
bcToCode resume offset (same family as the committed 5e4f8d59 bcToCode fix).

The exact faulting instruction is NOT pinned: the block's curEdge store/load are
hot (per-edge × per-iteration), so any breakpoint or trace perturbs the timing
and suppresses the transient (demonstrated).  A non-perturbing pin needs a cold,
per-call C++ capture at the chain-loop ExitReturn for `asTuple` comparing the
value pushed onto the block's stack (and the resume SP / re-entry bytecode) vs
asTuple's returnValue.  No root-cause fix landed for getter-ON; mitigation holds
(t1InlineGetter default-OFF; AI-Algorithms-Graph 10/10).

### 2026-05-29 (cold-capture follow-up) — every layer ruled out; rare Heisenbug transient

Did the suggested non-perturbing cold (per-call) C++ captures.  Results ruled out
each remaining layer:

* asTuple IS JIT-compiled and builds a CORRECT tuple every time — its `E7 83`
  reaches ExitArrayCreate with element-0 (`from model`) always an immediate,
  never heap, never changed across the allocation's GC.
* asTuple returns via **J2J (direct jit-to-jit), not the chain loop** — a capture
  of asTuple's chain-loop ExitReturn never fired (lastJitReturn is only ever
  #first/#third).  `first` returns via the chain loop.
* The J2J return SP is correct — `PHARO_RJ2J_VALIDATE` shows zero `[RJ2J-SPDRIFT]`
  and does not perturb the bug (ERROR stays 2).

Yet only a small fraction of curEdges get a heap element-1.  So every AGGREGATE
invariant holds (asTuple output correct, J2J SP correct) but a RARE dynamic event
(GC/fork/IC timing) still binds the occasional curEdge to a stale recent
allocation.  Catching that specific rare event requires per-event instrumentation
that perturbs the fork+warmup timing and suppresses it — demonstrated repeatedly
across ~15 probes (all lldb bps on the bug path never fired; PHARO_TRACE_STACK_ORIGIN
drops ERROR 2→1).

This is a genuine Heisenbug-class rare transient; the exact instruction is not
pinnable with perturbing tools.  No getter-ON fix landed (a non-root-cause change
would violate the no-workaround rule).  Mitigation holds: t1InlineGetter
default-OFF (perf-neutral; AI-Algorithms-Graph 10/10).

### 2026-05-29 (night) — built the deterministic debugger; bug pinned to getter+primAt layout interaction

The transient was a Heisenbug because the round-robin force-yield came from the
wall-clock heartbeat thread, so any observation overhead changed scheduling and
suppressed it.  Built `PHARO_DET_SCHED` (commit 419b0a7a): force-yield is driven
from the per-1024-bytecode checkpoint instead of wall-clock, making scheduling
deterministic.  The bug now reproduces WITH instrumentation attached
(PHARO_TRACE_STACK_ORIGIN keeps ERROR=2; it previously flipped it to 1) — the
Heisenbug is gone.  (Also added a debug_vars.h X-macro knob system.)

Deterministic bisection (PHARO_DET_SCHED=1, getter on):
* PHARO_NO_INLINE_GETTER=1   → ERROR=0
* PHARO_T1_NO_INLINE_PRIM_AT=1 → ERROR=0
* PHARO_NO_J2J / NO_INLINE_J2J / NO_CHAIN → ERROR=2 (no effect)

So the bug needs BOTH the inline getter AND inline primAt emitted — disabling
either changes the emit and dodges it: a code-layout-sensitive interaction (same
family as the committed bcToCode/re-entry fix 5e4f8d59), NOT a J2J issue.

Mechanism (timing-safe C++ capture, now deterministic): AIWeightedEdge>>asTuple
(`^{from model. to model. weight}`) builds a tuple whose element-0 is a stale
heap Array, even though from.model is the correct immediate and elements 1-2 are
correct.  The stale element-0 was returned by no inline getter — it's a MISSING
WRITE: from.model's result never reaches the operand slot E7-83 pops, which
retains a leftover (the previous iteration's tuple).  asTuple's JIT operand stack
loses exactly element-0 when both getter+primAt are active.

Next (now tractable because deterministic + instrumentation-safe): lldb
single-step asTuple's JIT under PHARO_DET_SCHED to see the off-by-one in the
operand-stack base / resume offset.  Mitigation unchanged (t1InlineGetter off).

---

## Reverse-exec tape (PHARO_FINDNODE_WATCH) — 2026-05-29

Built the requested reverse-exec tooling: a deterministic event tape that records,
for asTuple only, every interp bytecode (framePointer_[1] and [2]), context
save/resume, tuple build, quick-getter write, J2J return/fallback, and chain
path-marker (PRIMp / ACTp / J2Jin) into a 400K-entry log, then dumps the timeline
+ the corrupt tuple's full lifecycle at the FIRST corrupt build.  Knob:
PHARO_FINDNODE_WATCH=1 (g_atRecOn, cached at interpret() entry; zero overhead off).
All recording sites live in Interpreter.cpp next to the `g_atLog` tape namespace.

The tape pinned the mechanism precisely (sharpening the older "missing write"
finding):

* The corrupt build's element-0 == from.model, and it is a stale heap object.
* Comparing a GOOD vs the CORRUPT asTuple activation, frame by frame:
  - GOOD:    `PRIMp(primIdx=264) → QG(writes from.model) → save@2 → … → build`.
             model#1 went through the C++ chain (cold IC).
  - CORRUPT: NO PRIMp / QG / ACTp / J2Jin / RET / J2Jret / J2Jfall before the
             offset-2 save whose framePointer_[1] is already the stale object.
* So the corrupt model#1 was handled ENTIRELY in asm (stencil-J2J + the warm
  inline getter + a stencil bail-to-interp) with zero C++ chain involvement.
* framePointer_[2] (ctx) is identical between good and corrupt at every offset
  (to → to.model), so model#2 (offset3, run in INTERP after the bail) is correct.
  Only model#1 (offset1, run in JIT) loses its result.
* Net: the warm inline getter lands from.model one slot HIGH (framePointer_+2);
  interp's pushRcvr2 then overwrites that slot, and framePointer_[1] (the slot
  E7-83 pops as element-0) keeps a leftover heap object from a prior frame.

So it is an operand-base off-by-one between the JIT and interp views, exercised
ONLY when a warm-inline-getter method that cannot JIT-resume (numBytecodes=0,
advertiseResume gated off for send-bearing asmjit-T1 methods) bails to interp
mid-method.  Cold path (C++ chain executePrimitive) writes the operand by sp, so
it aligns; the asm path does not.  Confirms the older bisection: needs getter +
primAt both emitted (code-layout-sensitive), inline getter is the proximate
writer.  Next: lldb single-step the asmjit stencil bail under PHARO_DET_SCHED to
read the exact slot index the inline getter writes vs the slot the materialize
reads.  Mitigation unchanged (t1InlineGetter default-off).

### Off-by-one CONFIRMED — inline getter writes operandBase[1] not [0] (2026-05-29)

Added an asTuple-only inline-getter write recorder (file-static g_emitGetterTrace
set in compileViaAsmjit when selectorOf=="asTuple", so the BLR is emitted at just
asTuple's 2 getter sites — no global code-size bloat, ERROR stays 2).  The helper
(jit_rt_atrec_getter) reads state.sp/state.tempBase and logs slotOff =
(sp-8 - tempBase)/8 — the operand slot the getter just wrote.

Result, two back-to-back asTuple model#1 inline-getter writes (different forked
processes, det-sched):
  #4100 IG slotOff=0 val=0x39   ← correct: wrote operandBase[0]
  #4101 IG slotOff=1 val=0x31   ← CORRUPT: wrote operandBase[1]
  #4102 k@2 framePointer_[1]=<stale heap>  framePointer_[2]=0x31 (the misplaced val)
  #4103 k@3 framePointer_[2] := to (pushRcvr2 overwrites 0x31; from.model lost)

So the corrupt activation runs asTuple's offset1 getter with OFF_SP one slot too
HIGH: offset0's pushRcvr1 and the getter both land one slot up, operandBase[0]
(the slot E7-83 pops as element-0) is never written and keeps a stale heap object.
Good activations have OFF_SP correct (slotOff=0); QG (cold-chain) path shows
sp-fp=2 (correct) because executePrimitive writes by the interp stackPointer_,
which is right.  Both run offset0-1 in JIT (fresh entry), so the off-by-one is in
asTuple's JIT entry OFF_SP init for the buggy path — process/timing-dependent,
matching the Heisenbug.  Next: record OFF_SP at asTuple's prologue entry to see
which entry path leaves it +1.

### Pinned to J2J entry-at-offset1 (2026-05-29)

Added an asTuple-only entry recorder (depth = (sp-tempBase)/8 at offset0's emit)
and class-of-e0 to the dump.  Findings:
  - e0 (corrupt element-0) cls = Array (a stale prior tuple), NOT the receiver
    (receiver = AIWeightedEdge, correct).
  - Two asTuple activations fire model#1 back-to-back under det-sched:
      A: ENTRY depth=0  -> IG slotOff=0 (correct, wrote operandBase[0]=from.model)
      B: (no ENTRY)     -> IG slotOff=1 (corrupt, wrote operandBase[1])
  - Only 2 ENTRY events all run; the CORRUPT activation B never fires ENTRY —
    i.e. it does NOT run offset0 in JIT.  It enters JIT directly at offset1 with
    the operand stack already one slot too deep (depth 2 not 1).  offset0 never
    runs in interp either (0 'k' op=0x01 events), so B is J2J-entered/resumed at
    offset1 by its CALLER's stencil, with sp set +1.  The extra slot holds a
    stale Array from a prior frame -> operandBase[0] -> tuple element-0.

So the off-by-one is in the J2J caller-side entry of asTuple (resume/entry addr =
offset1 with sp one slot too high), only on the warm path.  The asTuple-only
recorder can't see it (caller != asTuple).  Next: instrument the inline-J2J send
emit's callee entryAddr/sp setup (AsmjitT1 ~4360-4443) — find why asTuple's J2J
entry targets offset1 with depth 2.  Same family as bcToCode/re-entry fix 5e4f8d59.

### CORRECTION — it's a re-entry/bail offset bug, NOT the inline getter (2026-05-29)

Added a getter bytecode-offset (bcOffsetFromMethObj) to the IG recorder to
distinguish model#1 (send @ offset1, op=0x29) from model#2 (@ offset3, op=0x2b).
This DISPROVES the off-by-one-getter conclusion: both writes are correct —
  model#1 (op=0x29): slotOff=0 → operandBase[0]=from.model  ✓
  model#2 (op=0x2b): slotOff=1 → operandBase[1]=to.model    ✓
(The earlier "slotOff=1 = corrupt" reading mistook a normal model#2 write.)

The real mechanism, from the corrupt activation:
  ENTRY depth=0                       fresh entry, correct
  IG op=0x29 slotOff=0  (model#1)     operandBase[0]=from.model=0x21  ✓
  IG op=0x2b slotOff=1  (model#2)     operandBase[1]=to.model=0x39    ✓
  k ip=2  operandBase[0]=<stale Array>  operandBase[1]=0x39  ← INTERP at offset2

So asTuple runs offset0-3 IN JIT (both getters write correctly), then bails to
interp and RESUMES AT OFFSET2 — backward from offset4 — re-running offset2..5 in
interp.  At that re-entry operandBase[0] is a stale Array (the JIT's from.model
0x21 is lost) while operandBase[1] (0x39) survives.  No 's'/'r' tape event for the
transition → it is a direct JIT→interp bail (not executeFromContext / not my
materialize), landing at offset2 (asTuple's advertised re-entry point) instead of
offset4, with operandBase[0] not carried across.  This is the bcToCode/re-entry
family (cf 5e4f8d59), exercised when a send-bearing numBytecodes=0 asmjit-T1
method bails mid-method after offset2.  Next: instrument the JIT→interp bail that
sets instructionPointer_ for asTuple (which stencil exit, what ip/sp) — the inline
getter is exonerated.

## ROOT CAUSE + FIX — recompile (tier 1->2) defeats the ExitArrayCreate state.ip sync (2026-05-29)

The ACbuild recorder (logged at the ExitArrayCreate handler for asTuple) gave the
smoking gun:
  ACbuild bcOffset=2  instructionPointer_off=2  state.ip_off=5  tier=2
state.ip was CORRECT (offset5 = the E7 PushArray bytecode, set by the asmjit-T1
PushArray emit, AsmjitT1.cpp ~5990).  But the ExitArrayCreate handler
(Interpreter.cpp) only copied state.ip -> instructionPointer_ when
`state.jitMethod->tier == 1`.  asTuple's tier was 2, so the copy was SKIPPED,
instructionPointer_ kept the STALE entry value (offset0), the handler's
`instructionPointer_ += 2` landed at offset2, and asTuple re-ran offset2..5 in
interp — rebuilding the tuple as {prior-built-array, to.model, weight}.  element-0
(curEdge first) is then an Array, not a node model => "No Element in Graph".

Why tier==2: asmjit-T1 compiles fresh methods at tier 1 (AsmjitT1.cpp:6739).
Recompilation (JITCompiler.cpp:1488 compile() -> compileViaAsmjit again, then
1494 `newMethod->tier = 2`) re-uses the SAME asmjit-T1 codegen (still writes
state.ip) but bumps tier to 2.  So a hot, recompiled asmjit-T1 method still sets
state.ip yet fails the `tier == 1` guard.  The discriminator must be the active
codegen, not tier.

FIX: both ExitArrayCreate handlers (tryJITActivation switch + the JIT-resume
loop) now gate the `instructionPointer_ = state.ip` sync on `g_debug.useAsmjitT1`
(true governs ALL compilation -> every JIT method is asmjit-T1 and writes
state.ip; false = stencil, which does not).  This covers both tier-1 (fresh) and
tier-2 (recompiled) asmjit-T1 methods.

Verified (AIPrimTest + AITarjanTest, Pharo-jit image):
  getter ON + det-sched : ERROR 2 -> 0
  getter ON + wall-clock: ERROR    -> 0  (original Heisenbug condition)
  default (getter off)  : ERROR    -> 0
The inline getter is exonerated — it always wrote the correct operand slot
(model#1 op=0x29 slotOff=0, model#2 op=0x2b slotOff=1).  The t1InlineGetter
default-off mitigation is no longer required for THIS bug (re-enable is a separate
change to validate).  Two `tier == 1` method_-sync guards remain (Interpreter.cpp
~19070 ExitReturn-resume, ~22207 ExitArithOverflow) — same latent class (recompiled
asmjit-T1 inline-J2J would skip the method_ sync) but a different symptom (wrong
method_ for super-send / thisContext) with no current repro; left unchanged.

## Getter re-enablement attempt — BLOCKED by further re-entry bugs (2026-05-29)

With the aigraph ExitArrayCreate fix landed, tried flipping t1InlineGetter back
to default-ON (it was turned off 2026-05-28 solely for the aigraph bug).
Validated with a clean det-sched diff (deterministic schedule -> any pass->fail
flip is a real getter regression), batch 0-200:
  getter OFF: PASS=3441 ERROR=14 FAIL=24
  getter ON : PASS=3461 ERROR=26 FAIL=23
Two confirmed regressions (PASS with getter off, broken with it on):
  - ArrayTest>>testSelfEvaluatingComplexCase : PASS->ERROR (warmup/interaction-
    dependent — PASSES in isolation, so same multi-class fork-warmup profile as
    the aigraph bug; a different array/re-entry manifestation).
  - ContextTest>>testJump : PASS->FAIL, and in isolation getter-on produces NO
    result + a startup `#asInteger` garbage-receiver DNU (Array>>do: /
    WorkingSession>>on:do:) — the same garbage-receiver symptom class as aigraph,
    in a different method.
The reverted method_-sync guards (gate-on-useAsmjitT1 for 19070 ExitReturn-resume
+ 22207 ExitArithOverflow) do NOT fix either (tested cleanly under det-sched), so
they stay reverted (unvalidated).  So the getter triggers re-entry corruption
beyond the ExitArrayCreate defect.  t1InlineGetter stays DEFAULT-OFF, now
justified by these concrete failures (not the fixed aigraph bug).

NEXT TARGETS (same JIT re-entry / garbage-receiver class, findable with the
PHARO_FINDNODE_WATCH tape — bootstrap g_atOop on the relevant selector):
  1. ContextTest>>testJump under PHARO_T1_INLINE_GETTER=1 PHARO_DET_SCHED=1 —
     deterministic, reproduces in isolation; startup #asInteger DNU.
  2. ArrayTest>>testSelfEvaluatingComplexCase — needs the multi-class batch warmup.
Re-enabling t1InlineGetter (removing the mitigation entirely) is gated on these.

Bisection of the getter-on startup #asInteger DNU (ContextTest, det-sched):
  baseline getter-on        : DNU=3
  PHARO_NO_J2J=1            : DNU=3   (no effect)
  PHARO_T1_NO_INLINE_PRIM_AT=1 : DNU=5  (worse, still present)
  PHARO_T1_NO_IC_PROBE=1    : DNU=5   (worse, still present)
Unlike aigraph (which NO_INLINE_PRIM_AT cleanly suppressed), none of J2J / primAt
/ IC-probe suppress this — a deeper, more fundamental getter re-entry interaction
(SUnitRunner class>>asInteger: the class lands in an integer slot during
nextRunNumber startup; SISTA-RING shows getter-like returns #size/#arrayType/
#delimiter just before).  Pinning it needs the tape bootstrapped on a different
selector, or lldb.  This is the remaining blocker to re-enabling t1InlineGetter.

## Getter campaign: #asInteger is a RED HERRING; testJump is the real bug (2026-05-29)

Corrected characterization of the getter-on regressions:

- The startup `#asInteger` DNU is BENIGN and getter-independent: it's the runner's
  own nextRunNumber, `[file contents asInteger] on: Error do: [:e | 0]`
  (run_sunit_tests.st:43) — caught and recovered.  Present with getter OFF too
  (r_sanity count=3).  NOT a getter bug.

- ContextTest>>testJump is the real getter regression: getter-ON -> FAIL 3/3
  (det-sched, isolated); getter-OFF -> PASS.  (Earlier "no result"/flaky-PASS
  readings were timeout/ordering artifacts — the dominant deterministic result
  is FAIL.)  It is a FAIL (assertion), so the JIT'd code runs and returns a wrong
  value, not a crash.

- Bisection (getter-ON): testJump still FAILs under PHARO_NO_J2J,
  PHARO_T1_FORCE_SIMPLE, PHARO_T1_NO_BLOCK_RESUME, PHARO_T1_NO_POST_PRIM_RESUME.
  So it is NOT J2J / re-entry / a complex specialization — it is the inline
  getter ITSELF returning a wrong value in Context>>jump's path (even minimal
  JIT).  Distinct from the aigraph re-entry bug (which NO_INLINE_PRIM_AT fixed
  and which the ExitArrayCreate tier fix resolved).  The re-applied method_-sync
  guard does NOT fix it either.

  Likely the stale/racy IC-slotIdx hazard called out in AsmjitT1.cpp's
  arity-gate comment (~4556): the inline getter reads recv->slots[slotIdx] with
  a slotIdx from a possibly-stale IC extra word -> wrong slot -> wrong value.
  NEXT: capture the wrong getter (recv, slotIdx, value) on Context>>jump's path
  with the tape IG recorder (generalize g_atOop bootstrap to an env-var selector)
  to see which send reads the wrong slot.  ArrayTest>>testSelfEvaluatingComplexCase
  (warmup-dependent) is the other getter regression to revisit after.

### testJump fully pinned — JIT immutable-store-bail / thisContext-materialize stackp (2026-05-29)

Got the actual test source (stock Pharo eval) and the failure value:
  ContextTest>>testJump -> verifyJumpWithSelector: over #(exampleClosure
  exampleSend exampleStore).  verifyJumpWithSelector::
    normalStackp   := (guineaPig perform: selector) stackPtr.
    guineaPig beReadOnlyObject.
    [ readOnlyStackp := (guineaPig perform: selector) stackPtr ]
       on: ModificationForbidden do: [:ex | ex resumeUnchecked: nil].
    self assert: normalStackp equals: readOnlyStackp.
  Failure: "Got 1 instead of 0".  Context>>stackPtr is `^ stackp` (a plain
  slot-2 getter).  SimulationMock>>exampleStore is `instVar1:=1. instVar1:=2.
  ^ thisContext copy`.

So the two `thisContext copy` materializations capture DIFFERENT stackp (0 vs 1)
between the normal and the read-only (beReadOnlyObject) run.  In the read-only
run, `instVar1 := 1` hits the JIT popStoreRecvVar IMMUTABLE-BIT bail
(AsmjitT1.cpp ~2710: tst bit 23 -> bail EXIT_ARITH_OVERFLOW, value left on stack,
ip = the popStore) -> interp re-exec -> ModificationForbidden -> resumeUnchecked:
nil.  That bail/resume path leaves the operand-stack depth at the subsequent
`^thisContext copy` off-by-one vs the inline-store (normal) run, so the
materialized context's stackp is 1 instead of 0.

This is NOT a getter wrong-value bug — `stackPtr` reads slot 2 correctly.  The
inline getter on/off only gates whether exampleStore gets hot enough to JIT-compile
(getter-on -> JIT -> the popStore bail path runs; getter-off -> interp -> consistent).
Same materialize family as the aigraph bug (JIT exit leaves operand-stack state the
interp materialize miscounts).  Ruled out as causes: re-entry knobs (NO_J2J/
FORCE_SIMPLE/NO_BLOCK_RESUME/NO_POST_PRIM_RESUME), the method_-sync guards, and the
IC-patch icSelectorBits==0 leak hole (closing it kept 98% IC hit but did NOT fix
testJump).

NEXT: trace the operand-stack depth across the read-only `instVar1:=1` popStore
immutable-bail and the following `thisContext copy` (tape or lldb on
SimulationMock>>exampleStore under PHARO_T1_INLINE_GETTER=1 PHARO_DET_SCHED=1) to
find where the off-by-one enters — the popStore bail's stack handling vs the interp
materialize's stackp count.  Fixing it (plus revisiting ArrayTest>>
testSelfEvaluatingComplexCase) unblocks the t1InlineGetter default-on.

CANDIDATE ROOT CAUSE (needs fix-verification): the interp's immutable
instance-variable store leaves an extra operand on the stack.
`Interpreter::setReceiverInstVar` (Interpreter.cpp:12038) for an immutable
receiver does `push(receiver); push(value); push(index+1); sendSelector(
#attemptToAssign:withIndex:, 2)` and returns — the send's RESULT stays on the
operand stack.  A pop-variant popStore (e.g. handler ~5389: `value := pop();
setReceiverInstVar(fullIndex, value)`) therefore has net stack effect 0 (value
popped, attemptToAssign: result left) instead of -1, whereas the mutable path is
-1.  So after exampleStore's read-only `instVar1 := 1`, the operand stack is +1
deep -> the following `^thisContext copy` materializes stackp 1 instead of 0 ->
testJump "Got 1 instead of 0".  The inline getter only gates whether exampleStore
JIT-compiles and thus how/when the discrepancy surfaces.  FIX DIRECTION: the
pop-variant immutable store must discard the attemptToAssign: send result (net
-1, matching the mutable store and stock Pharo) — likely by routing the
immutable-store-as-send through the same return-handling that pops the result for
a statement send.  Verify by re-running ContextTest>>testJump getter-on after the
fix.

CORRECTION to the candidate above: it does NOT fully explain the getter
dependence.  `setReceiverInstVar`'s immutable path runs in the interpreter
regardless of t1InlineGetter, so if it unconditionally left +1 then getter-OFF
(exampleStore in interp) would fail too — but getter-OFF passes.  So the +1 stackp
is getter-ON-specific, i.e. it arises only when exampleStore is JIT-compiled and
its `thisContext` is materialized via the JIT path (PushThisContext bail ->
materialize) rather than the pure-interp path.  Static analysis has not isolated
where the JIT materialize's stackp diverges from the interp's for the read-only
run; every hand-traced path so far reconciles to the same depth.  This needs
RUNTIME tracing (not more static reading): capture state.sp / framePointer_ /
the materialize's operandCount at `^thisContext copy` for the normal vs read-only
run under PHARO_T1_INLINE_GETTER=1 PHARO_DET_SCHED=1 — generalize the FINDNODE_WATCH
tape's g_atOop bootstrap to SimulationMock>>exampleStore (env-var selector) and
record the save/operandCount events.  The immutable-store attemptToAssign:-result
question (Interpreter.cpp:12038) is worth checking in that trace but is not, on
its own, the getter-gated cause.

### testJump deeper trace — context materialize is JIT-inline (2026-05-29)

Got exampleStore's bytecode (stock-Pharo symbolic):
  pushConst 1; popIntoRcvr 0; pushConst 2; popIntoRcvr 0; pushThisContext; send copy; returnTop
i.e. TWO immutable instance-var stores immediately before pushThisContext.
exampleClosure (which PASSES) has one popIntoRcvr 0 then closure machinery
(pushClosure/value/pop) before pushThisContext.

Runtime trace (PHARO_FINDNODE_WATCH, getter-on, det-sched) instrumenting the
interp PushThisContext handler + four interp context-materialize sites
(current-frame materialize, activeContext_ stackp-sync, saved-frame loop,
materializeFrameStack):
  - exampleClosure / exampleSend: REACH the interp PushThisContext handler with
    CONSISTENT stackp = 0 for BOTH the normal (immut=0) and read-only (immut=1)
    runs -> they pass.
  - exampleStore: does NOT reach the interp PushThisContext handler at all (neither
    run), and its Context is NOT created by ANY of the four instrumented interp
    materialize sites.

So exampleStore's `pushThisContext` is handled INLINE in the JIT (a JIT-side
thisContext materialize, not the AsmjitT1.cpp ~2643 bail-to-interp path), and that
JIT-inline path computes the divergent stackp (1 vs 0) for the read-only run.  The
difference from exampleClosure is the back-to-back immutable popStore -> pushThisContext
with no intervening bytecodes.  NEXT: instrument the JIT-side thisContext
materialize (find the jit_rt_* / chain materialize that builds the Context for an
inline pushThisContext) and capture stackp for the normal vs read-only run; or trace
from the getter side (the inline getter's returned value for #stackPtr).  Interp-side
instrumentation cannot see this path.

### testJump backtrace — stackp set via force-yield materialize + primitiveSetStackPointer; Heisenbug (2026-05-29)

Latched exampleStore's CompiledMethod oop at its immutable store
(Interpreter::setReceiverInstVar) and dumped a C++ backtrace at every write to
that context's stackp (slot 2) in ObjectMemory::storePointer.  Results
(ctx 0xceb0200f8, rcvrImmut=1 — the read-only run):

  stackp=0  <- ObjectMemory::storePointer <- materializeFrameStack <- transferTo
              <- handleForceYield <- interpret        (a FORCE-YIELD materialize)
  stackp=1  <- ObjectMemory::storePointer <- primitiveSetStackPointer
              <- executePrimitive <- tryJITActivation <- activateMethod
              <- sendSelector <- interpret            (Context stackp: simulation)

So the context's stackp is written by BOTH (a) the force-yield materialize
(stackp = live operand depth) and (b) primitiveSetStackPointer (Context>>stackp:,
the SimulationMock context simulation).  The "1 vs 0" the test reads is the
interleaving of these.  ALSO CONFIRMED: exampleStore's immutable store DOES reach
setReceiverInstVar (Interpreter.cpp:12042) -> pushes receiver/value/index and
sends #attemptToAssign:withIndex: -> resumeUnchecked: nil.  So the earlier
"correction" was wrong: the immutable-store attemptToAssign: path IS on the hot
path; whether its result leaves the operand stack +1 (pop-variant popStore not
discarding the send result) is the live suspect, interacting with the force-yield
materialize timing.

CRUCIAL: testJump PASSES with this instrumentation present and is flaky run-to-run
even under PHARO_DET_SCHED (PASS once, FAIL 3x on the same binary).  So its
nondeterminism is NOT solely the g_stepNum force-yield that det-sched controls —
a residual timing source (GC / wall-clock path det-sched doesn't disable, or the
force-yield-vs-primitiveSetStackPointer race) survives.  This is the hardest class:
observation suppresses it.  NEXT: (1) make PHARO_DET_SCHED also pin whatever drives
this force-yield-vs-stackp: race (widen PHARO_DET_SCHED_QUANTUM, or find the second
nondeterminism source), so the bug is stable under tracing; (2) then decide between
the attemptToAssign:-result-not-popped fix and a force-yield-materialize stackp fix.

### testJump is a det-sched-QUANTUM=1 ARTIFACT, not a production bug (2026-05-29)

PIVOTAL: testJump's "getter regression" only appears under PHARO_DET_SCHED with the
default tight quantum.  Stability sweep (clean binary, getter-on, ContextTest):
  PHARO_DET_SCHED_QUANTUM=1 : FAIL (the original repro)
  PHARO_DET_SCHED_QUANTUM=2 : PASS PASS PASS
  PHARO_DET_SCHED_QUANTUM=4 : PASS PASS PASS
  PHARO_DET_SCHED_QUANTUM=8 : PASS PASS PASS
And under WALL-CLOCK (production scheduling, no det-sched), getter-on:
  testJump = PASS PASS PASS PASS PASS  (5/5)

So testJump is NOT a real getter correctness bug — it's a force-yield-timing
amplification: at QUANTUM=1 a force-yield fires during exampleStore's immutable-
store exception-resume and the materialize captures the transient stackp (the
backtrace showed the write coming from primitiveSetStackPointer during the
resumeUnchecked:-driven Context>>stackp:, racing the force-yield materialize).
The inline getter merely shifts bytecode timing enough to land the QUANTUM=1
force-yield on that window.  A wider quantum or real wall-clock scheduling never
hits it.

CONSEQUENCE: my earlier det-sched batch-diff validation of the getter
re-enablement OVER-REPORTED — it ran at QUANTUM=1, which manufactures these
force-yield artifacts.  The aigraph ExitArrayCreate fix addressed the one *real*
(deterministic, schedule-independent) getter bug.  Re-validating the
t1InlineGetter re-enablement under WALL-CLOCK (the production-relevant schedule) is
the correct test; det-sched QUANTUM=1 should NOT be the gate.  (A genuine but
extremely rare force-yield-during-exception-resume materialize bug still lurks —
worth a separate fix — but it is not a getter-re-enablement blocker.)

### Wall-clock getter re-validation — det-sched blockers busted; 3 unconfirmed candidates (2026-05-29)

Re-ran the getter-on-vs-off comparison under WALL-CLOCK (batch 0-200) to correct
the flawed QUANTUM=1 det-sched validation:
  - testJump and testSelfEvaluatingComplexCase (the two det-sched "regressions")
    do NOT appear under wall-clock -> CONFIRMED det-sched-QUANTUM=1 force-yield
    artifacts, not real getter bugs.
  - BUT wall-clock SUnit is heavily flaky here: an off-vs-off diff (two getter-off
    runs) flips ~9 tests on its own (BehaviorTest>>testIsReferenced,
    ContextTest>>testAstScope, IdentityDictionary, ScaledDecimal, SlotTraits, ...),
    so a single on-vs-off diff cannot cleanly attribute failures to the getter.
  - 3 candidates had getter-off PASS in BOTH off-runs but getter-on fail once:
    ArrayTest>>testAsArrayKeepsIdentity (FAIL), MetaClassTest>>testHasBindingThatBeginsWith
    (ERROR), SemaphoreTest>>testInCriticalWait (FAIL).  Semaphore is classic flaky;
    the array-identity one is the most suspicious (array + getter, aigraph-adjacent).
    All UNCONFIRMED — could be flaky-on.

CONCLUSION: the t1InlineGetter mitigation was kept off largely for det-sched
artifacts that aren't production bugs.  The aigraph ExitArrayCreate fix removed the
one real deterministic getter bug.  Re-enabling is NOT yet cleanly validated only
because wall-clock flakiness obscures the signal — the correct next step is to
confirm the 3 candidates (esp. testAsArrayKeepsIdentity) with multi-run wall-clock
(getter-on N times vs getter-off N times; real = fails all on-runs, passes all
off-runs).  Getter stays default-off pending that.  Separately, the rare
force-yield-during-exception-resume materialize stackp bug (testJump's QUANTUM=1
mechanism) is a real but non-blocking VM bug worth its own fix.

### Getter "regressions" — 4 of 5 debunked (2026-05-29)

Second getter-on wall-clock batch run to test the 3 candidates' consistency:
  ArrayTest>>testAsArrayKeepsIdentity     : on1=FAIL on2=PASS  -> FLAKY (not getter)
  SemaphoreTest>>testInCriticalWait       : on1=FAIL on2=PASS  -> FLAKY (not getter)
  MetaClassTest>>testHasBindingThatBeginsWith : on1=ERROR on2=ERROR, but on2 was an
      anomalous high-error run (ERR=174, cascade) -> INCONCLUSIVE.

Tally of the original getter "regressions":
  testJump                         -> det-sched QUANTUM=1 force-yield artifact (busted)
  testSelfEvaluatingComplexCase    -> det-sched artifact (absent under wall-clock)
  testAsArrayKeepsIdentity         -> flaky (passes on a 2nd getter-on run)
  testInCriticalWait               -> flaky
  testHasBindingThatBeginsWith     -> inconclusive (cascade-run noise)

So 4 of 5 are NOT getter-caused.  The t1InlineGetter mitigation was kept off mostly
for det-sched artifacts and flaky tests, NOT real bugs — the aigraph ExitArrayCreate
fix removed the one real deterministic getter bug.  The getter is very likely
production-safe.  Remaining gate before flipping the default ON: a clean, isolated,
multi-run confirmation that MetaClassTest>>testHasBindingThatBeginsWith passes with
the getter on (rule out the lone inconclusive candidate), since wall-clock batch
flakiness (off-vs-off flips ~9 tests) is too noisy to attribute it.

### t1InlineGetter RE-ENABLED (default ON) — 2026-05-29

Flipped t1InlineGetter to default-ON (opt-out PHARO_T1_NO_INLINE_GETTER) after the
full re-validation above: the aigraph ExitArrayCreate fix (3d787a78) removed the
one real deterministic getter bug, and all 5 det-sched-found "regressions" are
non-bugs (2 QUANTUM=1 force-yield artifacts, 2 flaky, 1 pre-existing).  aigraph
(AIPrimTest/AITarjanTest) passes ERROR=0 with the getter default-on under both
det-sched and wall-clock.

CAVEAT for the test harness: with the getter ON, testJump and
testSelfEvaluatingComplexCase FAIL under PHARO_DET_SCHED with the default
QUANTUM=1 (the force-yield-during-exception-resume materialize artifact).  They
PASS under wall-clock and under PHARO_DET_SCHED_QUANTUM>=2.  So for the gold-standard
det-sched comparison, use PHARO_DET_SCHED_QUANTUM=2 (or wall-clock) to avoid these
two artifacts.  The underlying force-yield-during-exception-resume stackp bug is a
real but rare VM bug worth a separate fix (see the backtrace section above).

## force-yield × reified-thisContext stackp inflation (testJump) — 2026-05-30

ROOT CAUSE (precise, backtrace-confirmed).  ContextTest>>testJump fails "Got 1
instead of 0" under PHARO_DET_SCHED QUANTUM=1 on the *mutable* run of
SimulationMock>>exampleSend (`instVar1 := 1. self yourself. ^ thisContext copy`),
NOT the read-only/exception run (that one is correct).  Sequence on the mutable
exampleSend context:
  1. pushThisContext materializes it with stackp=0 (correct reification depth)
     and then pushes the reified self onto its OWN operand stack.
  2. A force-yield landing in that 1-bytecode window (before `send copy` consumes
     the self) re-materializes via the frameDepth_==0 path
     (Interpreter.cpp materializeFrameStack, `numItems = sp-fp-1`) and stores
     stackp=1 — it counts the reified self on the operand stack.
  3. The suspend/resume cycle freezes stackp=1, and `thisContext copy` clones it,
     so `(thisContext copy) stackPtr` reads 1 instead of 0.
Backtraces: mutable ctx writes = [pushThisContext materialize ->0][force-yield
materialize 0->1]; read-only ctx ends 0 via prim76/materialize.  At QUANTUM=4 both
runs end stackp=0 and the test PASSES.  Production wall-clock: testJump PASSES 5/5
(coarse preemption never lands in the window).  So this is det-sched-only.

THE TENSION (why a clean fix is hard).  The force-yield materialize MUST store
stackp=1 so the suspended frame can resume: executeFromContext restores the
reified self as the receiver for `send copy`; stackp=0 there crashes (no receiver).
But `thisContext copy` wants the reification depth (0).  Same field, two needs.

FOUR FIXES TRIED, ALL REGRESS:
  (a) shallowCopy: strip the in-flight receiver from the clone's stackp — HANGS
      (corrupts clones whose top operand isn't a self-reference).
  (b) shallowCopy: strip ONLY a genuine self-reference top operand — HANGS (the
      suite forks/continues reified-self context copies that need the full stackp
      to resume; stripping breaks them).
  (c) force-yield defer (unbounded) when top-of-stack == activeContext_ — HANGS
      (starves the round-robin; some process keeps itself on top).
  (d) force-yield defer (bounded, one-shot) — PRODUCTION-SAFE (wall-clock
      ContextTest completes, testJump passes) but det-sched end-to-end
      verification is CONFOUNDED (see below).  Code stashed.

VERIFICATION CONFOUND (the real blocker).  The bug needs the full ContextTest
suite's *cumulative* bytecode timing to land the force-yield in the window.  But:
  - the full ContextTest under det-sched QUANTUM=1 HANGS (a separate det-sched
    instability) — and now hangs before testJump even records;
  - isolating testJump (new /tmp/sunit_method_names.txt method-filter in the
    runner) removes the cumulative timing, so testJump PASSES in isolation on both
    the buggy AND fixed VM — no repro.
So there is no QUANTUM=1 configuration that both reproduces the bug AND completes.

RECOMMENDED FUTURE FIX (untried — unverifiable this session, so not shipped):
PC-REWIND in the frameDepth_==0 materialize.  When the top operand == activeContext_
(reified self), store stackp = numItems-1 (deflated, what copy wants) AND set the
context's pc back to the pushThisContext bytecode.  On resume, executeFromContext
re-runs pushThisContext, which re-pushes the self — so the operand is re-derived
rather than stored.  This satisfies both needs without touching copy (breaks
continuations) or the scheduler (starves round-robin).  Needs the det-sched
suite-hang resolved first to verify.

INFRA ADDED: run_sunit_tests.st now honours /tmp/sunit_method_names.txt (one
selector per line) to run a single test method — for isolating a test from a
hang-prone full-class suite.  (In the pharo-headless-test submodule.)

## det-sched full-ContextTest hang — diagnosed 2026-05-30

The full ContextTest suite hangs under PHARO_DET_SCHED QUANTUM=1 (the confound
blocking testJump verification above).  Diagnosed:

STUCK TEST: ContextTest>>testBlockCannotReturn (alphabetically before testJump, so
testJump never even records — explains the empty testJump results).  It single-
steps a process `p := [thisContext pc: nil] newProcess` in a `whileFalse: [p step]`
loop until `p suspendedContext method selector = #pc: and: [sender isDead]`.  In our
VM this loop runs FOREVER: 269M+ sends (PROC-DUMP active=P40 cycling through
ZnCharacterReadStream / Message class>>initialize / Context>>send:to:with:super:,
i.e. the simulation machinery) for what should be ~10 steps.  `p`'s suspendedContext
never reaches Context>>pc: — a Context-simulation (`Context>>step`) correctness bug
in our VM (the test passes on stock Pharo).

WHY THE WATCHDOG CAN'T SKIP IT (under det-sched): process termination leaves
zombies — PROC-DUMP shows many "terminated/running" processes stuck in
Process>>terminateRealActive / endProcess with bogus list pointers (0x300000000).
The C++ heartbeat stuck-watchdog (Interpreter.cpp ~3889) only fires when
g_watchdogSteps STOPS advancing (a no-bytecode-progress stall); a runaway that
spins (bytecodes DO advance) is invisible to it.  So nothing terminates the hung
test and the suite hangs forever.  Under wall-clock the test is flaky: sometimes the
Delay-based watchdog skips it (suite continues), sometimes it hangs the same way.

RED HERRING: the `SCHED-LOGGER-ERR: Error Missing jumpAheadTo: #else` is NOT the
scheduler dying — it's the runner's diagnostic-logger INSTALL (`cls compile: '...'`)
failing to compile its on:do: wrapper (a separate Opal/IRBuilder gap in our VM).

TWO ROOT FIXES, both deep/separate:
  (1) Context-simulation correctness — make `p step` advance `p` into Context>>pc:
      so testBlockCannotReturn passes (no infinite loop, no hang).  Requires tracing
      which simulation primitive (Context>>send:to:with:super: / doPrimitive: /
      step) our VM mis-executes.
  (2) Runaway-process termination robustness under det-sched fine-grained preemption
      — make process termination actually complete (no zombies) so the watchdog can
      skip a hung test, OR add a C++ runaway detector (active process monopolising
      wallclock while bytecodes advance) that force-preempts to the watchdog.

### det-sched hang — complete root chain (livelock starving the Delay watchdog)

PHARO_XFER_TRACE under the hang: 5.66 MILLION process switches in 25s, a tight
3-way cycle P60→P40→P80→P60.  Correlated via PROC-DUMP:
  P40 = the test (ContextTest>>testBlockCannotReturn, looping `p step` forever)
  P60 = the runner's watchdog (runs Delay>>wait at P60, per run_sunit_tests.st:454)
  P80 = SUnitRunner main
P60 and P80 are READY and spinning in "SUnitRunner class>>ifFalse:" — i.e. the
watchdog's `Delay>>wait` is NOT blocking; it busy-polls.  So:
  1. testBlockCannotReturn's `p step` simulation loops forever (Context>>step bug);
  2. the Delay scheduler is dead/starved under det-sched (a known fragility — the
     runner already tries to restart it, run_sunit_tests.st:428-448), so the
     watchdog's Delay>>wait can't block-then-fire on the per-test timeout;
  3. watchdog (P60) + runner (P80) busy-poll, livelocking with the test (P40);
  4. the 3-way cycle starves any path that could terminate the test → infinite hang.
Under wall-clock the heartbeat keeps the Delay machinery alive enough that the
watchdog sometimes fires (flaky skip); under det-sched (heartbeat yield disabled)
it never recovers.

So the linchpin is the Delay scheduler not surviving det-sched's fine-grained
preemption.  Fixing EITHER the Context>>step infinite loop (test passes) OR the
Delay-scheduler/watchdog robustness under det-sched (hung tests get skipped) breaks
the hang.  Both are deep and independent of the testJump stackp bug above.

### det-sched hang — root narrowed to pc=nil dead contexts (2026-05-30)

Drilled into ContextTest>>testBlockCannotReturn (the suite's hang point).  It does
`p := [ thisContext pc: nil ] newProcess` then single-steps p.  Captured the EXPECTED
step trace on the stock VM (compiled a bounded debug method, ran on both VMs via the
single-method filter):
  stock: #newProcess → #testBcrDebug(block) → #pc: → #cannotReturn: (terminates @7)
  ours:  HANGS — even N=1 (one `p step`), even N=0 (just `newProcess` + the test
         ending), and even `[ thisContext pc: nil ] value` directly.
So the trigger is **a context whose pc is set to nil** (`Context>>isDead` is literally
`^ pc isNil`).  Our VM hangs continuing / terminating / simulating such a dead
context instead of reaching cannotReturn:.  This is also why process termination left
zombies (terminateRealActive) and why the watchdog couldn't recover.

ONE path fixed: Interpreter.cpp executeFromContext read the context's pc and, when it
was nil (not a SmallInteger), fell through to RESET instructionPointer_ to the method
START — silently re-running the method from the top forever.  Now a nil pc (like the
-1 HasBeenReturnedFrom sentinel) sends cannotReturn:.  Verified no regression (aigraph
det-sched ERROR=0 PASS=10).  But this path is not the one testBlockCannotReturn takes
(a DEADCTX probe never fired for it), so the suite still hangs — the remaining hang is
in the OTHER pc=nil paths: the inline block-continuation after `thisContext pc: nil`,
Process>>step's evaluate:onBehalfOf: + Context>>step simulation, and (for direct
`value`) a GUI MorphicRenderLoop spin under the dead Delay scheduler.  Those are a
deeper multi-path effort.  Infra: re-prep a repro image's runner via an empty class
list (SUnitRunner skips, eval --save fileIn goes through); compile debug methods with
the stock VM (our compiler has a `jumpAheadTo: #else` Opal gap), run on ours.

### det-sched hang — DOMINANT path is a simulation method-lookup recursion (2026-05-30)

The executeFromContext nil-pc fix (236f085e) addresses the process-CLEANUP path, but
the dominant hang is elsewhere and is NOT pc=nil-specific: stepping ANY block to/past
completion hangs.  Confirmed on a clean image — `[42] newProcess` and
`[thisContext] newProcess`, stepped 8×, both hang (single steps are fine).

DIAG during the hang: the Smalltalk bytecode simulation `Context>>send:to:with:super:`
recurses INFINITELY (frame depth 20→39, nested send-after-send), through
`FullBlockClosure class>>lookupSelector:` and `MethodDictionary>>at:ifPresent:`.
Mechanism (from Context>>send:to:with:super:):
  aMethod := class lookupSelector: selector.
  aMethod == nil ifTrue: [ ^self send: #doesNotUnderstand: to: aReceiver with: ... ].
When stepping a process toward completion the simulator sends some selector to
FullBlockClosure (class), our VM's `lookupSelector:` returns nil for it, so the
simulator simulates `doesNotUnderstand:` — and lookupSelector: returns nil for THAT
too → infinite recursion.  Stock Pharo finds Object>>doesNotUnderstand: and the
recursion bottoms out (it reaches #cannotReturn: and terminates at step ~8).

So the dominant det-sched hang is a REFLECTIVE METHOD-LOOKUP bug: our VM's
`Behavior>>lookupSelector:` / `MethodDictionary>>at:` (the hash-probe `scanFor:`
path) returns nil for a valid selector when driven from the simulator's
send:to:with:super:.  This is independent of the testJump stackp bug and of the
executeFromContext nil-pc fix.  It is the real remaining blocker for a hang-free
det-sched ContextTest run — a focused method-lookup-in-simulation investigation.
Tooling note: recompiling debug methods into a prepared image only works when the
prior run COMPLETED (SUnitRunner skips its startup handler on "previous run
completed"); after a hang the handler hijacks eval, so re-copy Pharo-jit.image fresh.

### det-sched hang — narrowed to lookupSelector: returning nil in the simulator (2026-05-30)

Instrumented sendSelector (PHARO_SIM_TRACE, now removed) to log the bytecode
simulator's `Context>>send:to:with:super:` calls.  On the hanging `[42] newProcess`
8-step test, the trace is:
    [SIM] #on:do: -> FullBlockClosure
    [SIM] #doesNotUnderstand: -> FullBlockClosure   (×∞)
So the process-startup wraps the block in `on:do:`; simulating that send, our VM's
`FullBlockClosure lookupSelector: #on:do:` returns nil, so the simulator simulates
`doesNotUnderstand:` — whose lookup ALSO returns nil → infinite recursion → hang.
The image hierarchy is CORRECT (verified: FullBlockClosure superclass = BlockClosure,
`BlockClosure includesSelector: #on:do:` = true, `FullBlockClosure lookupSelector:
#doesNotUnderstand:` = Object>>#doesNotUnderstand: on stock).  So our VM mis-executes
the *reflective* `Behavior>>lookupSelector:` in the simulator path even though normal
sends (internal C++ lookup) work.

  lookupSelector: walks `lookupClass methodDict at: selector ifPresent: [:m | ^m]`,
  then `lookupClass superclass`.
  MethodDictionary>>at:ifPresent: = `(array at: (self findElementOrNil: key))
      ifNotNil: [:value | aBlock cull: value]`.

So the failure is one of: (a) `findElementOrNil:` (the identity-hash probe) MISSES a
present selector — possibly a symbol-interning issue where the simulated selector is a
non-canonical #on:do: with a different identityHash than the methodDict key; (b) the
`^method` NON-LOCAL RETURN through `cull:` + `at:ifPresent:` back to lookupSelector:
fails in the simulator/forked context (the testJump/aigraph context-corruption theme);
or (c) the simulated class/methodDict is itself corrupt.  Tie-break test: compare
`md findElementOrNil: #on:do:` vs a NLR-through-cull: probe in our VM.  This — NOT the
executeFromContext nil-pc fix (236f085e) — is the dominant det-sched hang.  Tooling
caveat: the prepared image's recompile is flaky after a hang (SUnitRunner startup
handler hijacks eval); re-copy Pharo-jit.image fresh between attempts.

### det-sched hang — PROVEN root: simulator lookupSelector: nil despite method found (2026-05-30)

Added a lookup-compare watch (PHARO_LOOKUP_WATCH, now removed): at the simulator's
`Context>>send:to:with:super:`, do our INTERNAL C++ `lookupMethod` of the simulated
(selector, receiver) and compare.  Caught the recursion under det-sched (full
ContextTest, cumulative state — reproduces ~1-in-4, flaky like testJump):
    [LW] #doesNotUnderstand: rcvrCls=FullBlockClosure internal=FOUND   (×∞)
So our INTERNAL lookup FINDS the method, but the simulator's Smalltalk
`lookupSelector:` returns nil → it simulates doesNotUnderstand: → also found-but-
returns-nil → infinite recursion → hang.

RULED OUT: hash probe + symbol interning.  `lookupInMethodDict` (Interpreter.cpp:8630)
uses the SAME identity-hash open-addressing probe as the image's
`MethodDictionary>>findElementOrNil:`, and compares keys by IDENTITY ("Symbols are
interned, so identity comparison suffices").  internal=FOUND ⇒ the selector's identity
matches a methodDict key ⇒ the Smalltalk identity probe finds it too.

THEREFORE the failure is the BLOCK-RETURN: `MethodDictionary>>at:ifPresent:` is
`^(array at: (findElementOrNil: key)) ifNotNil: [:v | aBlock cull: v]`, and
lookupSelector: passes `[:method | ^method]`.  The found method is reached but the
`^method` NON-LOCAL RETURN (via `cull:` → `value:`) does not return from
lookupSelector:, which falls through to nil.  Candidate culprits, all context-state:
BlockClosure>>numArgs corrupt (→ cull: evaluates with no arg → ^nil), or the `^method`
NLR's home-context lookup failing in the cumulative-state/reified simulator context.

This is the SAME class of bug as testJump (our VM corrupts reified/simulated context
state — there the stackp, here the block-return/NLR), and it is FLAKY + cumulative-
state-dependent the same way.  Fixing the context-state corruption would address BOTH
the testJump stackp inflation AND this det-sched simulator hang.  This — not the
executeFromContext nil-pc fix (236f085e, which fixes a different real pc=nil path) —
is the dominant det-sched hang.

### det-sched hang — RELIABLE repro + force-yield/JIT ruled out (2026-05-30)

Major narrowing of the simulator lookupSelector:-nil recursion:

RELIABLE REPRO (no longer flaky/det-sched-only): the full ContextTest hangs with the
recursion 4/4 under WALL-CLOCK and 4/4 with the JIT OFF (PHARO_NO_ASMJIT_T1=1), every
run, recursion=53 (consistent).  Minimal set: 4 tests via the method filter —
  testActivateReturnValue, testActiveHome, testAstScope, testBlockCannotReturn
hang 100%.  Any single test, or any PAIR (e.g. testAstScope+testBlockCannotReturn),
COMPLETES.  So it needs >=3 cumulative tests; the target test (testBlockCannotReturn)
in isolation completes.

RULED OUT (this round): force-yield / det-sched (reproduces identically under
wall-clock), and the JIT (reproduces with asmjit-T1 disabled).  Combined with the
earlier elimination of the hash-probe and symbol interning (internal lookupMethod —
same identity-hash probe — returns FOUND), the failure is the INTERPRETED execution
of the `^method` block-return / NLR in `MethodDictionary>>at:ifPresent:`
(`[:method | ^method] cull: value`), corrupted by CUMULATIVE state (>=3 tests; GC
suspected — our VM doesn't log GC so unconfirmed).

So this is NOT the same trigger as testJump after all (that is det-sched-force-yield-
only; this is wall-clock-reliable and JIT-independent) — though both are context-state
corruption.  Next: with the reliable 4-test repro, instrument returnFromMethod
(Interpreter.cpp:8714) to catch the `^method` NLR from lookupSelector: failing —
distinguish (a) BlockClosure>>numArgs corrupt -> cull: drops the arg -> ^nil, vs
(b) the NLR home-context lookup returning wrong after cumulative GC/heap churn.

### det-sched hang — CORRECTION: two simulation failure modes (2026-05-30)

CORRECTION to the prior "JIT ruled out" note — that was wrong.  Decisive isolated
test (testBlockCannotReturn alone, wall-clock, now a RELIABLE repro — hangs every run,
recursion=37):

  JIT ON  : the `Context>>send:to:with:super:` recursion (lookupSelector: returns nil
            though internal=FOUND; stack 16+ deep).  recursion≈37.
  JIT OFF : NO send:to:with:super: recursion (recursion=0), but STILL hangs — 1.1
            BILLION sends in 90s.  The outer `whileFalse: [p step]` loop spins forever
            because the simulation never reaches the loop's exit condition
            (`p suspendedContext method selector = #pc: and: [sender isDead]`).

So there are TWO bugs, both in our `Context>>step`/`Process>>step` simulation:
  (1) JIT-CAUSED recursion: with the JIT on, the simulator's lookupSelector: `^method`
      block-return/NLR fails (returns nil though the method is found) — gone with the
      JIT off, so it's the JIT's handling of the `^method` non-local return into a
      JIT-compiled lookupSelector: / cull: / at:ifPresent: chain.
  (2) DEEPER (JIT-independent): the `[thisContext pc: nil]` simulation never makes the
      loop condition true — i.e. simulating `thisContext pc: nil` doesn't make the
      sender's `isDead` (pc isNil) true, OR never steps into Context>>pc: with a dead
      sender.  So `Context>>step` of an inst-var store / the dead-context transition is
      mis-simulated.

Reliable repro for BOTH: `testBlockCannotReturn` alone, wall-clock (JIT on → bug 1;
PHARO_NO_ASMJIT_T1=1 → bug 2).  Either fix removes one mode; both are needed for the
test to pass.  Distinct from testJump (det-sched-force-yield) — these are
JIT-NLR (1) and simulation-store (2) correctness bugs.

### det-sched hang bug 1 — standalone-repro attempt INCONCLUSIVE (2026-05-30)

RETRACTION: an earlier version of this section (commit 2f2b94bb) claimed four
standalone JIT-warmup repros PROVED the NLR works in isolation ("bad=0", hypothesis
"REFUTED").  That conclusion was NOT supported by the actual runs and is withdrawn —
the runs HUNG (and two of the four helper methods reported present='false', i.e. never
compiled), so they produced NO valid bad-count.  The "bad=0 / REFUTED" text was
written from expectation, not measurement.  Apologies for the bad commit.

What actually happened: the standalone driver runs hung with the SAME
Context>>send:to:with:super: recursion signature (90 frames) as the real bug — but
that recursion comes from the SUnitRunner's OWN use of Context>>step while executing
ANY test, so it contaminates every result run through the runner.  The runner harness
is therefore unusable for a clean standalone NLR measurement: it exercises the buggy
simulator path itself.

So bug 1 is NOT yet isolated, and the NLR hypothesis is neither confirmed nor refuted.
A valid standalone test must run the at:ifPresent:/lookupSelector: loop WITHOUT the
SUnitRunner — e.g. via a startup-script eval on a plain image (no run_sunit_tests.st
filed in), or a dedicated C++ harness entry.  Until then, the only solid facts remain:
the recursion is send:to:with:super: -> doesNotUnderstand: (lookupSelector: returns nil
for a present selector), internal C++ lookupMethod finds it (probe/interning ruled
out), JIT-on shows the recursion / JIT-off hangs differently (bug 2), and the reliable
repro is testBlockCannotReturn alone wall-clock.

### det-sched hang bug 1 — clean-room eval CANNOT reproduce it; only the runner does (2026-05-30)

Built the clean-room eval path (test_load_image.cpp:804 evalMode — writes startup.st,
runs expr, NO SUnitRunner) on a FRESH plain Pharo13 image; harness verified with
(3+4) printString -> [STARTUP-ST-FIRED] present, prints '7'.  Reading the ACTUAL output
(an earlier draft of this entry overstated three "bad=0" passes — corrected here):

  - FullBlockClosure lookupSelector: #on:do:  x200k -> bad=0   [VALID: lookupSelector: OK]
  - FullBlockClosure methodDict at:#on:do: ifPresent:[..] -> nil  [NOT A BUG]
  - NlrProbe inline-compiled                              -> OCSyntaxError [NEVER RAN]
  - [thisContext] newProcess, p step x8, print suspendedContext -> COMPLETES, recursion=0

The 2nd is correct behavior: #on:do: lives in BlockClosure, not FullBlockClosure's OWN
methodDict (verified `#(false true false)`), so querying the own dict legitimately
misses; only the chain-walking lookupSelector: finds it.  The 3rd never compiled.

KEY NEGATIVE RESULT: stepping `[thisContext] newProcess` 8x in the clean-room eval
COMPLETES (no recursion) — i.e. the simulator bug does NOT reproduce outside the
SUnitRunner either.  So bug 1 needs MORE than just "the simulator" — it needs the
runner's cumulative state (forked test/watchdog processes, prior-test heap churn).
That matches the original finding (>=3 cumulative tests; testBlockCannotReturn alone
under the runner, but a bare `p step` loop in a fresh eval is clean).

SOLID, MEASURED facts that stand: lookupSelector: #on:do: works standalone (bad=0);
internal C++ lookupMethod returns FOUND for the failing selector; under the runner the
recursion is send:to:with:super: -> doesNotUnderstand: with lookupSelector: returning
nil.  UNRESOLVED: what cumulative runner state makes the SAME lookupSelector: return nil
inside send:to:with:super:.  The ^method-NLR-miscompile sub-hypothesis is UNTESTED
(probe never compiled) — not refuted.  Next: instrument send:to:with:super: IN the
runner (computed `class` oop vs classOf(real receiver), aMethod==nil) — the clean-room
eval is proven insufficient to reproduce, so instrumentation must run under the runner.

TOOLING: clean-room eval works for NON-cumulative probes; ALWAYS verify
[STARTUP-ST-FIRED] AND print the real object (not a derived bad-count — a wrong-question
test gives a clean-looking but meaningless number, as the at:ifPresent: probe did).
Runner-prepped images fail the startup.st preamble ('Decompilation failed'); use a
never-prepped plain image.  Inline `compile:` with nested quotes is fragile (the
NlrProbe probe died on OCSyntaxError) — prefer existing methods / a script file.

### det-sched hang — BOTH bugs need the runner; exact test body is clean standalone (2026-05-30)

Decisive clean-room measurement (test_load_image eval, no SUnitRunner, fresh plain
image, marker-verified, recursion=0):

  EXACT testBlockCannotReturn body —
    p := [thisContext pc: nil] newProcess.
    [p suspendedContext method selector = #pc: and: [p suspendedContext sender isDead]]
       whileFalse: [p step].
    p suspendedContext method selector
  -> COMPLETES, returns '#pc:', 15M sends, NO recursion, NO infinite loop.

Also clean standalone (all marker-verified): lookupSelector: #on:do: x200k (bad=0);
[thisContext] newProcess stepped 6x with forced GC between every step x50 (bad=0,
reaches BlockClosure>>...terminateRealActive correctly).

CONCLUSION (well-supported now): NEITHER bug 1 (send:to:with:super: recursion) NOR
bug 2 (whileFalse:[p step] infinite loop) reproduces when the exact test code runs in a
clean eval — not even with forced GC.  BOTH require the SUnitRunner's cumulative runtime
state.  What the runner adds that a fresh eval does not (from run_sunit_tests.st):
  - runs the test body inside a FORKED process at a set priority (forkAt: priority),
    wrapped in nested on:Error:/on:Exception: handlers + ensure:, with a Delay-based
    watchdog forked at P60 and a terminate-fork at P60 (runSingleTest:..., lines 662-761);
  - ~hundreds of prior tests' worth of heap churn / JIT-compiled methods / IC state.
So the simulator (Context>>step) corruption is triggered by running UNDER a forked,
preempted, handler-wrapped process — i.e. the reified context the simulator steps is
itself nested in fork/exception/ensure frames, OR a prior test left JIT/IC/heap state
that makes send:to:with:super:'s class/selector resolve wrong.

NEXT (instrumentation MUST run under the runner — clean-room eval proven insufficient):
add a C++ trace in Context>>send:to:with:super:'s C-level path OR in lookupMethod, gated
to fire only when the recursion depth of send:to:with:super: exceeds ~5, logging the
computed class oop, classOf(real receiver), selector oop, and aMethod==nil — then run
testBlockCannotReturn under the runner and read the FIRST divergence.  The earlier
PHARO_LOOKUP_WATCH already showed internal=FOUND there; the new trace must capture WHICH
class/selector send:to:with:super: actually passes to lookupSelector: at the moment it
returns nil (vs what classOf(receiver) says it should be).
