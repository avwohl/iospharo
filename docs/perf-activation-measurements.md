# perf-activation: measurements on real workloads

Moved from `scripts/perf-activation/README.md`, which holds the harness and
the microbenchmark baseline.  This file holds the measurements taken on
real workloads: defect #6's whole-image scan and the printString loop.

## 2026-09-04: a REAL-workload profile and knob triage (not the microbench)

Everything above is `bench_activation.st`.  This section is the same question
asked of the workload that actually costs test results — the whole-image
reflective scan behind defect #6's TIMEOUTs
(`NoUnusedVariablesLeftTest>>testNoUnusedTemporaryVariablesLeft`, run outside
SUnit so it completes rather than being killed).  Native arm64, quiet machine.

Baseline, interleaved reps:

    stock Cog (x86_64 Rosetta)     5.76 s
    ours, JIT on                 216.8 / 217.3 / 218.2 s
    ours, JIT off                177.4 / 160.9 / 161.2 s

**The JIT is 35% slower than our own interpreter here.**  That is consistent
with `docs/results-perfdb.md`'s "1M blocks: JIT 5.5x SLOWER" — a reflective
scan is `allMethodsDo: [...]`, i.e. block-activation-bound.

`sample`, 10 s mid-scan, top of stack (JIT-emitted code carries no symbols so
it is excluded; these are the C++ frames):

    724  Interpreter::tryJITResumeInCaller
    560  Interpreter::activateBlock
    443  Interpreter::tryJITActivation
    436  ObjectMemory::scavenge()::$_3
    386  JITRuntime::noteMethodEntry
    327  Interpreter::interpret
    319  JITRuntime::tryResume
    295  Interpreter::push
    254  Interpreter::upgradeICToJ2J
    196  Interpreter::primitiveFullClosureValue

Knob triage, one rep each, same expression, same machine:

    knob                          time      delta vs default
    (default)                    217.3 s      --
    PHARO_T1_NO_CALLER_RESUME    187.2 s     -14%
    PHARO_NO_BLOCK_VALUE_SPEC    216.9 s       0
    PHARO_NO_EAGER_BLOCK_VALUE   217.5 s       0
    PHARO_T1_NO_INLINE_J2J       220.8 s      +2%
    PHARO_NO_JIT                 161.0 s     -26%

**Only the caller-resume path has a measurable share**, and it is 14% — which
matches `tryJITResumeInCaller` topping the profile, and makes hypothesis 1 (the
bcToCode resume-address lookup on the J2J return path) the one with evidence
behind it.  The other 12% of the gap to the interpreter is spread across
`activateBlock` / `tryJITActivation` / `noteMethodEntry` and does not fall to
any single existing knob.

Two things NOT to re-test:

  * the block-value specialisations — `PHARO_NO_BLOCK_VALUE_SPEC` and
    `PHARO_NO_EAGER_BLOCK_VALUE` are each within noise of the default, so the
    block cost is in the generic activation path, not in those emits;
  * `noteMethodEntry`'s `hasSplice()` — it looks like the per-call
    `unordered_set` lookup CLAUDE.md warns about, but `sistaRuntimeForGCHook_`
    is null unless `PHARO_SISTA_COMPILE`/`sistaDispatch` is set, so the lookup
    never runs in a default build.

`PHARO_T1_NO_CALLER_RESUME` is NOT a fix — it is the x86 sp-leak workaround
knob, it does not close the gap to the interpreter (187 s vs 161 s), and
disabling caller re-entry costs send-bound code where the JIT does win.  It is
useful here only as an attribution instrument.


## 2026-10-11: the gap is general, and two interpreter hot-path fixes

`bench_ast_phases.st` splits defect #6's scan into phases.  Native arm64 for
ours, stock Cog x86_64 under Rosetta, ms:

    phase            Cog    ours JIT   ours interp   JIT/Cog
    enumerate         10        73          53          7x
    sourceCode        70      1849        1327         26x
    scan             114      1631        1340         14x
    parse            328      5080        3922         15x
    parse+semantic   677     12210        9186         18x
    printString 3M   552     10765        6559         20x

No phase stands out, so it is not the parser.  `1 to: N do: [:i | i
printString]` alone shows the whole problem and is the repro to use.

Per 1M printString, before -> after the two fixes below:

    interpreter (PHARO_NO_JIT)   1980 -> 1500 ms
    JIT (default)                3170 -> ~3000 ms

**Fix 1: `Interpreter::push` was the hottest interpreter frame** (863 of
~6000 samples).  Every push ran a three-region heap-bounds test plus two
large diagnostic dumps, all inline, so the function could not be inlined.
Now a one-test fast path with the old body moved byte-for-byte into a cold
`pushSlow()`.  -21% on the interpreter.

**Fix 2: `patchJITICAfterSend` was an out-of-line call per send** that
returned at once almost always (no pending patch).  Inline gate now.  -4%.

**What did NOT move the JIT number**, each 2 reps on the printString loop:

    knob                                             ms      vs ~3050
    PHARO_T1_NO_CALLER_RESUME                        2900-2934  -5%
    PHARO_NO_RESUME                                  2952-2979  -3%
    PHARO_T1_ADMIT_BAILMID_CALLEES                   3026-3028  -1%
    ADMIT_BAILMID + PHARO_T1_XMETHOD_MAX_IC=200      3977-4006  +31%

`PHARO_T1_SEND_CENSUS` on this loop: 69% of sends take the dispatchCached
round trip (top: `#byteAt:put:` 5.6 per printString, `#digitValue:` 4.9),
and the census now also prints which early return `upgradeICToJ2J` took.
Almost all of them leave at "entry matched, extras already set": the IC
already holds a J2J entry (bit 60) and the emitted send still exits
ExitSendCached.  Opening the bail-mid and numIC gates does not change the
69%, so those gates are not what refuses them.  This is the
`docs/j2j-chain-continuity.md` project (site 1); start there.

## 2026-10-11 (later): two more, now with line-level attribution

`scripts/perf-activation/line-profile.py` turns a `sample` report into self
time per source line.  Its docstring has the build recipe: with ThinLTO the
line tables live in temporary objects the linker deletes, so a plain `-g`
build symbolizes to function+offset only; `-Wl,-object_path_lto,<dir>` plus
`dsymutil` fixes that.

On the JIT printString loop it put `tryJITActivation` at 29.7% self, its two
hottest lines being:

- **`jit::SistaSave sistaSavePool[256]`** -- value-initialized because Oop's
  constexpr default constructor zeroes `receiver`: 256 stores at a 56-byte
  stride, ~14 KB of cache lines, on EVERY JIT activation, with Sista off by
  default.  Now raw uninitialized storage (nothing reads past the cursor).
  -17% on the loop (3050 -> 2540 ms); `tryJITActivation` 29.7% -> 17.3%.
- `clearJ2JSlice` (4.7%) -- kept: it wipes stale receivers the GC would
  otherwise pin, and bounding the GC walk instead regressed TFFI (2026-07-03).

**`upgradeICToJ2J` fast exit**: nearly every call finds the site's entry for
the receiver class already upgraded, but reached that return only after a
methodMap lookup, the eager-compile test, isJ2JBanned and prim
classification.  An inline scan at the top returns first.  -4%.

Phase bench after all four fixes, ms (before -> after):

    phase            JIT              interp
    parse+semantic   12210 -> 10540   9186 -> 8073
    testBody         12632 -> 11139   9733 -> 8420
    printString 3M   10765 ->  8643   6559 -> 4960

SUnit batches 1-200 and 1001-1100: same P/F/E as the full sweep, 8-20% less
wall time.

## 2026-10-11 (evening): why 69% of sends round-trip -- resumed methods have J2J off

A read-only audit workflow over AsmjitT1.cpp (four readers, a verifier per
claim) found the one exit condition that does not depend on the callee.
`tryJITResumeInCaller` resumes a method in emitted code with
`state.j2jSaveCursor = state.j2jSaveLimit = nullptr` unless
`PHARO_T1_RESUME_INTERNAL_J2J` is set.  The send emitter's save-pool check
(`cmp cursor, limit; b.hs j2jBailFull`, AsmjitT1.cpp ~7725) is taken when both
are NULL, so EVERY J2J-marked send in a resumed method exits ExitSendCached.
It is self-sustaining: the send round-trips, the callee returns, the caller is
resumed again with a NULL slice, and its next send exits too.  That is the 69%
-- and why opening the bail-mid and numIC gates changed nothing (the check sits
after them).

The comment at the resume site still says internal mode is "default ON" with
`PHARO_T1_RESUME_EXTERNAL_J2J` as the opt-out; that knob is read nowhere.
Internal mode was turned back to opt-in on 2026-06-12 (`e81e6f0e`): it then
produced garbage-selector DNUs because the loop's handlers decoded the send
bytecode at `state.ip`, a shape only external mode guaranteed.

Measured today, printString loop (both arms with the send census on):

    default (NULL slice)               3411, 3396 ms
    PHARO_T1_RESUME_INTERNAL_J2J=1     1968, 1980 ms     -42%

SUnit batches 1-200 (3185 P / 0 F / 0 E) and 1001-1100 (597 P / 0 F / 0 E)
with the knob on: identical to the default, and 1-200 ran in 105 s against
~165 s.  A full sweep with it on is the gate for flipping the default.

**Full sweep with `PHARO_T1_RESUME_INTERNAL_J2J=1`** (arm64, same settings and
image as the 2026-10-11 sweeps):

    knob     wall      classes  PASS    FAIL  ERROR
    off      1 h 53 m  2047     27872   13    1     (sweep-arm-2026-10-11)
    on       59 m      2047     27877   12    1

48% less wall time, no garbage-selector DNUs (interpreter `[DNU]` lines 2094
against 2125).  New non-clean: `ProcessSpecificTest>>testLocalVariable` 1 F
(two processes timed with 30 ms Delays) and `TraitPureBehaviorTest` 1 T --
both still to be A/B'd.  **And one SIGSEGV**: batch 1-50 crashed (rc=139) in
`AndreasSystemProfilerTest`, inside `Interpreter::interpret` at the
PushReceiverVariable handler with `receiver_` holding a SmallFloat -- a
receiver/frame desync, the shape the 2026-06-12 note warned about.  It did not
recur in 8 repeats of batch 1-50 nor in 6 runs of the class alone (about 1 in
10 batch runs).  `PHARO_ACTIVATION_CHECK` now also checks, on every way out of
the resume loop, that `receiver_` matches the frame's receiver slot (block
frames: the closure's receiver); it reads 0 mismatches on clean runs.

**The default stays OFF** until that crash is explained.

### The internal-J2J crash, localized (2026-10-11, late)

Crash rate with the knob, batch 1-50, interleaved: **on 1 of 40, off 0 of 40**
(2 of ~52 knob-on runs overall, always the same place).  Both crashes:
`receiver_` = SmallFloat `0x7f3ae147ae147ae5`, `method_` =
`Float>>absPrintExactlyOn:base:`, ip offset 826 -- **68 bytes past that
method's last bytecode**.  So the frame ran with another method's ip, and the
"PushReceiverVariable" it crashed in was a byte of that other method.

`PHARO_RESUME_EXIT_CHECK=1` (new) checks, O(1), that `instructionPointer_` lies
inside `method_`'s bytecodes at every place a (method_, ip) pair is installed:
the resume-loop and activation exits, `executeFromContext`, `popFrame`,
`sendSelector`, `returnValue`; plus the J2J-save ip in
`materializeJ2JSaveIntoFrame`.  Every SavedFrame now records its push site.
(The O(method size) instruction-boundary walk, `PHARO_IP_BOUNDARY_WALK`,
slowed things enough to hide the crash in 120 runs -- keep it off.)  Zero
reports on clean runs, knob on or off.

Hunt results (knob on), first incoherent pair each time, never a SAVE-IP report:

    run  site                     method_ with a foreign ip     last resume exit
     5   popFrame (pushFrame)     #isOctetString                ExitReturn #indexOf:ifAbsent:
    44   popFrame                 #to:do:                       ExitReturn #to:
     5   popFrame (pushFrame L15731) #from:                     ExitReturn #isCreateFullBlock:code:at:
     8   returnValue              #indexOf:startingAt:ifAbsent: ExitReturn #indexOf:ifAbsent:

Every one follows an `ExitReturn` in the resume loop, and in the last one the
frame left running is the CALLEE of the method whose ExitReturn was handled.
So with internal J2J, a J2J callee's native return and the loop's ExitReturn
handling disagree about which frame is current: the interpreter is left on
the callee's `method_` with the caller's ip.  That is the 2026-06-12 note's
"handlers assume external-mode shapes", now pinned to one handler: the
`case jit::ExitReturn` of `tryJITResumeInCaller` when the resume slice had
pending J2J saves.  Fix there, then re-run the 40-rep A/B.

**Further narrowing (same evening).**  Five more hunts, each crashing within
5-33 runs with the identical signature (`absPrintExactlyOn:base:`, ip off 826,
last resume exit ExitReturn `#indexOf:ifAbsent:`, `fd=0`).  None of the checks
fired before any of them:

    check                                            result before the crash
    (method_, ip) O(1) at resume/activation exits,
      executeFromContext, popFrame, popFrameForJIT,
      sendSelector, returnValue, jitSistaCallSend     silent
    the exit's own (state.method, state.ip)           silent
    J2J save ip at materialization                    silent
    every saved frame and the current pair at GC     silent
    full instruction-boundary walk at the four
      install sites, and at activation exits for
      absPrintExactlyOn:base: specifically            silent

So no checked path INSTALLS an incoherent pair for this method.  The ip goes
bad while `interpret()` is already executing it, by a write to
`instructionPointer_` (or a jump) that leaves `method_` alone.  The
partial-identity audit workflow (three readers + verifiers over
tryJITResumeInCaller) confirmed no candidate for the crash; it did confirm two
latent defects there: the materialize-failure bail
(`[RESUME-MAT-FAIL]`) writes only ip/sp and drops the rolled-back frame's
materialized Context, and `closure_` is never synced by either identity sync.
Next instrument: log every write to `instructionPointer_` that is not paired
with a `method_` write, while `method_` is `absPrintExactlyOn:base:`.

**Two more ruled out, and the recurring shape.**  `extA_`/`extB_` are 0 at the
crash (no stale extension byte widening a jump), and an O(1) check on every
`activateMethod` return stays silent.  What DOES recur is the earlier-caught
pair, now seen twice (runs 8 and 23 of two hunts):

    [IP-INCOHERENT returnValue] OUTSIDE: ip off 306 vs #indexOf:startingAt:ifAbsent:
        last resume exit: ExitReturn, state.method = #indexOf:ifAbsent:

`method_` is the J2J CALLEE (`indexOf:startingAt:ifAbsent:`) while the ip
belongs elsewhere, apparently to the resumed CALLER `indexOf:ifAbsent:`, whose
ExitReturn the resume loop just handled.  So under internal J2J the exit's
"which method returned" (`state.method` / `state.jitMethod`) and "where to
continue" (`state.ip`) can describe different frames -- callee vs resumed
caller -- and the loop's ExitReturn handling (or the materialize sync just
before it, which sets `method_` from `state.jitMethod` and the ip from
`state.ip`) installs that mixed pair.  `state.ip` is not published by a return
exit, so after a J2J callee's ExitReturn it can still hold the caller's resume
ip.  That is the place to fix; the crash in `absPrintExactlyOn:base:` is the
same mix landing in a Float method, where it executes a neighbouring object's
bytes as code.  Knob stays OFF.

**Return-method split ruled out too.**  A check that the resume loop's
ExitReturn is handled as the same method `state.jitMethod` names stayed silent
in the next crashing run (run 14).  Every cheap install-site hypothesis is now
measured and negative.  Next step is an lldb session on a crashing run: a
hardware watchpoint on `instructionPointer_` armed when `method_` becomes
`Float>>absPrintExactlyOn:base:` (`PHARO_DET_SCHED` will not help -- the
profiler test's sampling is driven by a real timer).

## Two per-call costs outside the JIT (2026-10-12)

Found by profiling smalltalkweb-myprecious, whose last 3 errors are a 100 ms
reply timeout that our VM misses (Fuel serialize is 17x slower than Cog).

1. `executeFromContext` called `sigsetjmp(g_sigsegvRecovery, 1)`.  On Darwin
   the mask-saving form makes two syscalls (`sigprocmask`, `sigaltstack`) per
   call, and `executeFromContext` runs on every context resume and process
   switch.  Now `sigsetjmp(..., 0)`; the recovery path unblocks SIGSEGV and
   SIGBUS itself, the one mask that matters there.
2. `primitiveExternalCall` re-resolved the named primitive on every call:
   it copied every short literal into a `std::string` and tried every
   module:name pair in the maps.  It now caches the resolved target per
   method (old-space methods only, keyed by oop + full-GC count, cleared by
   `flushMethodCache`).

Interleaved A/B, arm64 release, 3 rounds each, same result every round:

    zero-ms Delay wait loop (200000 iters)     4.7 us -> 3.5 us per iter  (-25%)
    'the quick brown fox' findString: 'fox'    780 ns -> 537 ns per call  (-31%)
    myprecious Fuel serialize                  10.6 ms -> 10.7 ms          (none)

The serialize loop did not move: a second profile of the same loop is
dominated by the activation wall (`returnValue -> tryJITResumeInCaller`,
`activateMethod -> tryJITActivation`), and the first profile's syscall
samples came from the three zero-delay reception services switching process
around it.  So myprecious's `testArgPassByReference` stays a speed failure
until the resumed-method J2J lever (above) is safe.

## The internal-J2J crash: a process switch inside an extended instruction (2026-10-12)

**Root cause found; it is an interpreter bug, not a JIT bug.**
`PHARO_T1_RESUME_INTERNAL_J2J` only made it more likely to show.

Sista V1 extends a bytecode with a prefix: `E1 FF ED A8` is ExtendB(-1)
followed by ExtJump, i.e. `jumpTo: 570` backward.  The prefix and its consumer
are one instruction.  If the process switches between them, the saved Context
pc points at `ED`, `executeFromContext` zeroes `extB_`, and on resume the jump
runs with extB 0 -- 168 bytes FORWARD, off the end of the method.  That is the
whole crash: `Float>>absPrintExactlyOn:base:` with ip 63 bytes past its end.

`interpret()`'s `periodic_checks` ran two checks that can switch process BEFORE
its `if (inExtension_)` early-out: `checkProfileSampleTick()` (signals the
profiler's semaphore; the profiler runs at a higher priority and takes the CPU
at once) and `checkLowSpaceSignal()`.  `AndreasSystemProfilerTest` profiles
code full of `E1 FF ED xx` loop back edges, so a sample tick that landed on a
prefix switched mid-instruction.  Both checks now wait while `inExtension_` is
set; the early-out sets `checkCountdown_ = 1`, so they run one bytecode later.

    batch 1-50, PHARO_T1_RESUME_INTERNAL_J2J=1     before: 1 crash in 22 runs
                                                   after:  0 crashes in 150 runs
                                                   (chance of 0/150 at 1-in-22: ~0.1%)

How it was found, for the next hunt of this shape:
- `PHARO_IP_RING` (debug_vars.h): ring of the last 256 interpreted bytecodes
  and a bounds check on every fetch.  It showed the profiler process at fd=0
  switching straight into `absPrintExactlyOn:base:` at offset 655.
- `PHARO_PC_BOUNDARY_SEL=<selector>`: reports a pc of that method that is not
  an instruction start, where it is produced (J2J save decode,
  `materializeFrameStack`, `pushFrame`, `executeFromContext`).  It has to treat
  prefix + consumer as one instruction.  The first version walked ExtendB as a
  separate 2-byte instruction and stayed silent through four hunts.
- Every earlier check (ip coherence at each install site, before and after GC)
  was correct and silent, because the ip WAS coherent: it only ever pointed
  at a real byte of the right method, just not an instruction start.

An audit workflow then searched the whole VM for the same shape.  It found one
more live path, now fixed: the FFI callback-return cooldown runs exactly 500
`step()`s and then forces a switch, so a 500th step on a prefix saved the same
bad pc.  The cooldown now finishes the extended instruction first.  The three
remaining paths are knob-only or unreachable for standard methods; see
`docs/deferred.md`.

## Correction: the "internal J2J" speed lever was T1 refusing compiles (2026-10-12)

Every "internal J2J" speed figure in this file -- printString -42%, the 59-min
and 56-min sweeps -- measured something else.  With
`PHARO_T1_RESUME_INTERNAL_J2J` on, `emitClosurePush` emits
`ldr x, [x, offsetof(Interpreter, closure_)]`.  That offset is ~25.7 MB
(`stack_` is a 16 MB member declared before it; `Interpreter.hpp` says so
next to `MaxStackDepth`), far outside AArch64 LDR's 32760.  asmjit rejects it,
`T1EncodingErrorHandler` fails the compile, and the method stays out of T1.
So the knob kept every method with an inline-J2J send push out of T1.

The giveaway was the stats line, which nobody read: with the knob on, a
batch shows `IC: 0/N (0% hit)` and `J2J-r: 0/0`; with it off, `IC: 67% hit`,
`J2J-r: 300k`.

Made the push encodable (ADD-immediates to reach the offset) to measure the
real thing: T1 compiles everything again (IC 76% hit, J2J 400k) and
juliendelplanque-jrpc breaks (STON parse errors, DNU
`#variable:value:position:` on `OCParseErrorNode`).  So real resume-internal
J2J is unsound, as the knob's old "still corrupts" note said.

What the evidence supports: on this suite, keeping those send-bearing methods
OUT of T1 is twice as fast as compiling them, with the same results.  That is
the same finding as memory `perfdb-jit-slower-than-interp` (T1 loses to our
interpreter except on sends).  `5e2780e0` made the mode the arm64 default for
the wrong stated reason.  The behaviour is kept, now as an explicit refusal
in `emitClosurePush` (`a.report_error(...)`, "policy: inline-J2J closure push
refused") so an asmjit that learns to materialise large displacements cannot
silently switch on the corrupting path.

The extension-prefix crash fix in the same commit is real and independent.
The crash showed up in this mode because more code ran in the interpreter.

Open questions this raises: why T1-compiled send-bearing methods are slower
than the interpreter (the activation wall, again), and whether a deliberate
policy -- for example "T1 compiles only methods with no inline-J2J send" --
should replace this indirect one.

## The send path, measured by instruction count (2026-10-13)

Wall-clock numbers on this machine swing by 10-20% with background load, so
this round used `instructions retired` from `/usr/bin/time -l` (run the
binary directly, not under `timeout`, or the count is the wrapper's).  The
workload is `1 to: 3000000 do: [:i | i printString]`, minus the same run
with eval `1` (startup).  Billions of instructions:

    change                                        JIT     interpreter
    start of session (11e94851)                   210.8   173.4
    knobs compiled out (PHARO_DIAG_KNOBS=OFF)     167.8   143.0
    + mega-cache refill gated on an epoch         ~166    ~142
    + noteMethodEntry inline fast path            158.0   142.2
    + classOf inline                              156.7   141.4
    + lazy J2J-slice clear                        153.6   141.7
    + plain method return skips the home walk     144.3   132.0
    + specialObject inline, immediates in classOf 144.0   131.3
    + Sista skip memo                             142.5   130.1
    + T1 compiles only code with a loop           128.0   130.1
    + method-map presence filter                  127.4     -
    + direct dispatch: returns, special sends     122.4     -
    + trace globals out of DISPATCH_NEXT          108.7     -
    + DIAG_FLAG on the remaining trace globals    107.3     -
    + extension / extended-jump direct dispatch   104.5     -

What each one is:

- **Knobs.**  Every `GET_DEBUG_BOOL` was a load and a branch, and the trace
  code behind the knobs bloated the hot functions.  Release builds now compile
  plain `DEBUG_BOOL` knobs to `false` (CMake `PHARO_DIAG_KNOBS`, see
  CLAUDE.md); `DEBUG_BOOL_RT` knobs stay live.
- **Mega cache.**  Every cache-hit send did a method-map probe and wrote the
  JIT's 2 MB mega cache.  Now only when `jit::g_jitStateEpoch` changed since
  that method-cache entry last did it (bumped on method-map insert / remove /
  clear, invalidate, and mega-cache evictions and resets).
- **noteMethodEntry.**  The fast path was defeated twice: entries past probe
  slot 0, and `sistaRuntimeForGCHook_` being non-null in the default build.
  It now probes 4 slots and asks `g_sistaAnySplice`.
- **J2J slice.**  Each JIT entry cleared 32 save slots (20 cache lines).  Now
  only slots that can be dirty: `j2jPoolDirtyHW_` marks the lowest slot that
  may hold a stale oop, and a release whose first slot is still clear lowers
  it.  A slice abandoned without release keeps the mark up.
- **Plain return.**  `returnFromMethod` walked the method's last-literal chain
  twice to decide "is this a block" for every `^`.  A CompiledMethod
  activation with no paused-NLR marker now goes straight to `returnValue`.
- **Sista memo.**  `g_debug.sistaDispatch` is ON by default, so every
  activation probed two `unordered_map`s (`wasCachedAsFail`,
  `sistaGateCache_`).  A 4096-entry direct-mapped memo, trusted while
  `g_sistaEpoch` matches, answers "skip" with one compare.

Wall clock, `bench_ast_phases.st`, same machine, ms:

    phase            before   after
    sourceCode         1322    1002
    scan               1377    1009
    parse              3789    2963
    parse+semantic     9359    7029
    testBody           9840    7619
    printString3M      7045    4743

A send + activate + return of `zzBench: x ^ x + 1` (from a method at frame
depth >= 1, interpreter only) went from ~1570 to ~1085 instructions.  The
lldb single-step histogram that found the dispatch cost is
`stepcount.py`/`steplines.py` (attach, break in activateMethod, step N
instructions, count per function or per source line); one `op_pushSelf` +
`DISPATCH_NEXT` was 31 instructions, 7 of them trace-flag tests.

Before this round: ~47K instructions per printString, 387 bytecodes and ~50
sends, so about 110 instructions per bytecode.  The interpreter keeps its state in members
(`stackPointer_`, `instructionPointer_`, ...), so every push and pop is a
memory access, and the send path is still ~600 instructions.

### Later the same day: frames, blocks, and the trusted scavenge

Further steps, each checked with instruction counts:

- **Cold paths out of the hot frames.**  `sendSelector`'s cache-miss path,
  `returnValue`'s return-into-Context branch and widow step, the Sista hook of
  `activateMethod`, and `pushFrame`'s overflow report now live in their own
  out-of-line functions (`sendSelectorMiss`, `returnValueAtDepth0`,
  `widowReturningContext`, `trySistaActivation`, `reportFrameOverflow`), and
  `tryJITResumeInCaller` has an inline gate.  Sharing one frame made every
  ordinary send and return save the registers the rare paths needed; the
  lldb trace showed prologue + epilogue at ~25 instructions per call.
- **`literal()` and `primitiveSuccess` inline**, the class of SmallInteger /
  Character read from the class table (verified at image load), `primitiveAt`
  fast path for Arrays and byte objects.
- **Sista dispatch off by default** (DebugSettings.cpp, with the numbers).
- **Blocks:** direct dispatch for BlockReturnNil / BlockReturnTop; the
  home-method walk and home-frame search in `activateBlock` run only for a
  block that can return non-locally (`compiledBlockMayReturnNonLocally`,
  memoized per block for one `g_objectMotionEpoch`).  A one-argument block
  call went from ~1060 to ~800 instructions.
- **Trusted remembered set** (`docs/deferred.md`): the scavenge no longer
  scans all of old space.  cormas analysis 23.84 -> 22.30 billion
  instructions; with it the cormas package reaches parity with Cog (332 P on
  both) and the deeptraverser test runs in ~47 s against its 50 s limit
  (Cog 6 s; 66 s this morning).

Deeptraverser instructions: 2311 billion this morning, 1200 now -- but wall
time fell only ~28%: what is left of that workload is partly memory-bound.
