# WIP — 2026-08-23

Current state only. This file was 4882 lines of stacked session entries going
back to 2026-05-27; on 2026-08-23 it was pruned the same way `docs/deferred.md`
was on 2026-08-12 — the history moved to
`docs/history/wip-2026-05-27--2026-08-23.md`, nothing was deleted, and the full
text is also in `git log -p docs/WIP.md`.

47 entries were classified before pruning. Five had anything still live in them;
the other 42 were superseded or finished. The facts that were still true and
recorded in **no other file** were pulled forward into "Carried forward" at the
bottom of this file, each re-verified against the tree on 2026-08-23 with a
file:line. If you are looking for something that is not here, read the archive
before concluding it was never known.

Routing to the durable docs is in the header of `docs/deferred.md`.

## Right now (2026-10-15, evening) -- Cog interrupt points, two-way become; pushed `59d6af46`, sweep L30 on `57646eb6`

Pushed `443b5b44` (L28: 27802 / 8 / 3) and `59d6af46` (L29: 27803 / 6 /
3; block frames enter and resume in machine code by default).

- 57646eb6: process switches only at Cog's interrupt points (method or
  block activation, back jump), not at sends a primitive answers.  The
  Morphic alarm Heap held nil / lost an alarm in L27 and L29
  (WorldState>>removeAlarm:for:): a switch landed inside
  Heap>>removeAt:.  Companions so machine code does not stall on a
  pending checkpoint: MC entry declines at countdown <= 1, MC back edges
  exit on a spent countdown, a yield suppressed at an ensure: entry
  retries at the next interrupt point.
- 9f0d91f9: two-way become (prim 128) of objects of different sizes:
  copies + forwarders + one scavenge for young pairs, else one scan
  (young-only or parallel) through becomeForwardAll's two-way mode; no
  cache flush unless a method, class, symbol or MethodDictionary moved.
  ReStore writes 1000 objects in 2.4 s (was 13.5 s; Cog 63 ms).  Still
  O(heap) once the pair is old (ReStore swaps the same objects again):
  Cog forwards lazily.  ReStore's speed tests assert on these times.

## Earlier (2026-10-15) -- exits per call; pushed `253d78f3`, sweep L26 on `35ad5971`

Pushed: origin/jit at `253d78f3` (sweep L25 27801 / 9 / 3; its new
entries are known flakes: LinkInstallerTest, a fake-GUI keyboard-focus
error).

Two more exit-per-call shapes removed:
- 35ad5971: a primitive that fails at an inline-cache hit runs the method
  body natively (was one exit per failure).  The primitive stub answers
  exact SmallInteger `/` and reports a sure failure for an inexact one,
  and answers `class` of immediates.  Fraction loop 18.7 -> 14.3 billion.
- d9b09985: ExtendA + ExtSend (a send to literal 32-255) is a helper
  send; it took the prefix bail, so the rest of a big method (parsers)
  ran interpreted.

Then (lexicon 321.1 -> 303.7 billion instructions, sweep L26 on
`35ad5971`: 27801 / 10 / 1):
- 7cb8ddcf: contexts made for frames are sized as Cog sizes them (6 +
  16, or 6 + 56 with the large-frame bit) instead of 6 + temps + 32 at
  least; the stack-tail clear uses raw stores.
- e3be2488: ExtSend sites cache primitive targets; pharo_mcf_prim
  answers primitive 105 (bytes/Array) without the state sync.  `,` 52 ->
  34 ms per 300K (Cog 24).
- e3d47401: inline at:/at:put:/size read the overflow slot count (a 64
  KB stream buffer has 8K slots and took the primitive stub).
- d53cfdbd: an IC hit on a primitive entry branches to it first.

Profile of lexicon (interpreter thread, `sample` + PHARO_JIT_PERF_MAP on
build-prof, a -g release tree): machine-code methods 55%, C++ 26%,
stubs 19% (call stub 10.6%).  IPC is 4.3, so the cost is instruction
count, not stalls.  A send + return is ~150 instructions (Cog ~40):
the SavedFrame record (11 stores), the state mirrors, the IC extras
decode and the call-cache probe.  Non-local return through ensure: is
~15,500 instructions (Cog ~420 ns under Rosetta).

FreeTypeCacheTest>>testGlyphAccessIsThreadSafeWhenAskingWidth (L26):
a flake on both binaries (HEAD 6/6 pass alone, the L25 binary timed out
once); the test's own comment says it "breaks 95% of times".

Pushed `93cf45e5` (sweep L27: 27804 / 7 / 2, the best so far; its one
new entry, SpCodePresenterTest>>testDoBrowseHierarchy, is a fake-GUI
alarm race that passes 4/4 alone).  The runner no longer shadows
variables (af649a8 in the submodule); ReleaseTest still lists
FastStepThroughController, which is base-image code.

Since then (sweep L28 on `443b5b44`): thisContext and terminateTo: run
from machine code (055e7e8a, 443b5b44), so resume:through: no longer
falls into the interpreter; young contexts are synced with raw stores;
the context tail clear and allocateSlots' nil fill vectorize
(f952c81e).  lexicon ~300.0 billion instructions.

Measuring under load: instruction counts move with machine load (a
slower wall clock means more heartbeat checkpoints), so A/B only with
interleaved runs while a sweep is going.

## Earlier (2026-10-15, early) -- stalls and stubs; pushed `eebd43a3`, sweep L25 on `253d78f3`

Pushed: origin/jit at `eebd43a3` (sweeps L23 27799/10/3, L24 27800/10/2;
every new entry passes alone; SpAthensAdapterTest>>testResize... is a
pre-existing 1-in-3 flake on every binary back to L19).

Three "stable slow states" found and fixed -- each made machine code
leave at every iteration of a loop, with no test failing:
- a089f65d / aee72a62: the interpreter's checkpoint, deferred to an
  interrupt point (c6e7182a), did not count special-selector sends
  (`s next`) or arithmetic sends on non-SmallIntegers as sends; the MCF
  call stub then refused every call on the spent countdown (4.2x, 2.9x).
- eebd43a3: an ensure: activation's switch suppression went stale and
  held back every heartbeat yield; back edges polling forceYield_ exited
  each iteration (an ensure: loop 2.9x).
Signature in the MCF exit census: about one exit per iteration for a
callee that is compiled and in the call cache, or a back-edge exit
(reason 6 at an ExtendB) per iteration.

Stubs: valueNoContextSwitch (how ensure: runs its block) is a native
block call (5dc0ab1f); uncontended Semaphore signal/wait in the primitive
stub (da644162); closures and their lazy outer contexts are created in
machine code (253d78f3, 51M a lexicon run).  Call path: stub addresses
in the state, savedArgCount by one strb (~122 instructions a send+return).

lexicon instructions (billions): 351 -> 346 (call path) -> 345 (stalls)
-> 337 (valueNoContextSwitch) -> 335 (semaphore) -> 321 (closures).
Non-local returns through ensure: are still 49 of the 321.

## Right now (2026-10-14, night 7) -- unwinding and stub coverage; pushed `6aaa0050`

Pushed: origin/jit at `6aaa0050` (sweep L22: 27800 / 11 / 3; its four new
entries pass alone and in their batch, run under lldb load).

Sweep L21 (`61a816e4`) caught a real bug: the ExtSend site caches hold
method oops that a full GC's compaction moves; a stale entry called
FormSet>>asForm for extent:depth:forms:.  The per-become cache flush had
hidden it until 5ae06110 removed that flush.  Fixed in 6aaa0050
(recoverJITAfterGC empties them); found by knob bisection under
PHARO_DET_SCHED on batch 1751-1800.

lexicon instructions (billions): 364 -> 360 (stub: quick primitives,
class, behaviorHash, basicNew, CompiledMethod at:; hot-path counters
gated) -> 352 (a non-local return through ensure: restores the frames
below its home instead of resuming each from its context;
fetchPointer inline) -> 351 (no cycle walk for fresh contexts).

Wall time, quiet machine: lexicon 19.4 s, Cog 11.3 s.  Suite (ms, ours /
Cog): fib30 15/7, loop30M 115/133, collect3M 96/135, inject3M 47/31,
printString 51/47, dict 136/122, ocAdd 79/101, string 69/49, float 190/199,
sort 228/155, compile 19/88, setStr 163/135.

Measured, not done: each non-local return through ensure: (1.7M a
lexicon run) still costs ~27K instructions: materializing ~15 frames
(only ~5 are needed by the image's protocol) and running the protocol in
context land.  The old native unwind path (PHARO_NATIVE_NLR_UNWIND) is
14% faster on lexicon but skips ifCurtailed: blocks and loses an
exception raised in an unwind block, so it stays off.  A send + return
in machine code is ~147 instructions (Cog ~25).

## Right now (2026-10-14, night 6) -- runtime costs outside machine code; sweep L20 on `4f906387`

Pushed: origin/jit at `1ad9a30d` (sweep L19: 27801 / 10 / 1; its new
entries pass alone on that binary and on HEAD).

lexicon instructions (billions): 411 -> 403 (returns into contexts resume
machine code; materialization writes fresh contexts directly) -> 400
(ExtSend site caches; WideString at: in the stub) -> 396 (file reads keep
a pending position: no fseek, no stdio refill) -> 394 (scavenge skips the
method cache and JIT tables unless a young object was stored in them;
roots 770 -> 31 us) -> 364 (young becomeForward: forwards through a
scavenge instead of walking 36 MB of eden; primitive 72 flushes caches
only for methods, classes and symbols).

Measured, not done: one send + return costs ~147 instructions in machine
code (send site 38, call stub 61, return 37); Cog's is 1-2 ns.  Trimming
fields gives ~1% each; the frame record design is the limit.  The
interrupt countdown (4 instructions a call) could become Cog's
stack-limit trick, but every forced-checkpoint writer must then lower the
depth limit.

New diagnostic knobs: PHARO_SCAV_TABLE_VERIFY, PHARO_BECOME_YOUNG_VERIFY
(scavenge path), PHARO_SCAV_STATS root sections, PHARO_BECOME_CENSUS
region times.  Both verifiers were proven by sabotage (a no-op hook).

## Right now (2026-10-14, night 5) -- primitive stub holes; sweep L19 on `1ad9a30d`

Pushed: origin/jit at `c6e7182a` (sweep L18: 27802 / 8 / 2; its three new
entries pass 3 of 3 alone, run while I benchmarked).

Wall time against stock Cog (Rosetta), quiet machine: lexicon 21.9 s vs
11.3 s (was 28.3 s this morning).  Suite (ms, ours / Cog): fib30 14/7,
loop30M 119/133, collect3M 93/135, inject3M 45/31, printString 50/47,
dict 136/122, ocAdd 75/101, string 80/49, float 222/199, sort 232/155,
compile 20/88, setStr 169/135.

lexicon instructions (billions): 451 -> 452 (interrupt points) -> 432
(String>>at: inline: asmjit had dropped `ccmp w10, #63` from the
primitive stub, so 63 always took the C call) -> 416 (at:/size on
formats 3-5: MethodDictionary basicAt:, WeakArray) -> 411 (basicNew
inline under MCF; objectAt:).

Scheduling fixes: the prompt yield at bytecode exits is gone (it made
myprecious RPC tests time out 1 run in 3), and the interpreter's
periodic check switches process only at interrupt points (sends, back
jumps, frame entry), as Cog.  SocketPlugin's io thread no longer spins
on signaled sockets.  Open: ProcessSpecificTest>>testLocalVariable, a
Delay that never wakes on the fake-GUI image (vm-compat-bugs.md).

The diagnostic primitive census (PHARO_MCF_PRIM_CENSUS) now names each
C-called primitive's target, receiver kind and caller path; it found
all of the above.  PHARO_JIT_PERF_MAP also dumps the primitive stub
pages (`.primstubN.bin`) for a disassembler.

## Right now (2026-10-14, night 4) -- call-site shapes; sweep L17 finishing

Since `08deb8cf` (not yet swept):

- `3360d7b9` one `cbnz` for an ordinary callee's entry kind.
- `ff09200b` hashMultiply (primitive 159) runs from machine code:
  Dictionary benchmark 18.2 -> 15.3 billion instructions.
- `683a0ca2` the call stub and native return no longer store the state ip.
- `605cf4ee` the send-site leak guard is off under MCF.
- `a1ac1368` multi-slot getter (`^ a - b + 1`, OrderedCollection>>size)
  answered at the call site; its old x5 clobber (the #extent bug) fixed.
- `a06a810a` tiny shapes: `^ f ifNil: [...]`, `^ fa OP fb`,
  `^ (fa OP fb) and: [...]` answered by a shared stub.

lexicon 471 -> 451 billion instructions.  Call + return of a small method
about 189 instructions.

## Right now (2026-10-14, night 3) -- benchmark suite against Cog; sweep L17

Pushed: origin/jit at `477275fb` (sweep L16: 27801 / 8 / 2; the only new
entry, MCSmalltalkhubRepositoryTest testVersionFromFileNamed, read a
truncated download and passes alone).  Sweep L17 runs on `08deb8cf`.

Benchmark suite ($S/suite.st, ms, ours vs stock Cog under Rosetta; both
measured while a sweep ran):

    fib30            15 / 7      send cost (about 195 instructions per call)
    loop30M          82-120 / 148
    collect3M        99 / 171    was 210: block shapes (08deb8cf)
    inject3M         48 / 51     was 158: block shapes
    printString300K  52 / 49
    dict300K         155 / 123
    ocAdd3M          53 / 97
    string100K       90 / 49     was 151: failed primitive runs the body (79c9fd55)
    float10M         277 / 198   was 1642: mixed SmallInteger/Float inline (785fd2b5)
    sort1M           235 / 158
    compile200       22 / 90
    setStr300K       175 / 138

Fixes behind those numbers: the interpreter's block-shape shortcuts in
primitive 207 kept such blocks from ever compiling, so machine code exited
at every call; a failed primitive in a helper send exited and the
interpreter retried it; `i * 1.5` went through six image sends.  Also
constant-return call entries (`^ $!`) and a single `cbnz` for an
ordinary callee's entry kind (3360d7b9).  lexicon 477 -> 471 billion.

## Right now (2026-10-14, night 2) -- speed round; sweep L15 on `92e646f5`

Pushed: origin/jit at `bdc7a854` (sweep L14: 27806 P / 5 F / 1 E, the
best so far, nothing outside the L9-L13 lists).  Sweep L15 runs on
`92e646f5`.  Not yet swept: `0ee9b7c0`, `25baba71`, `52be7182`,
`e812eb9d`, `1751216e`.

lexicon instructions (billions):

    sweep L14 commit                  575
    IC hit skips the shape chain      564   (92e646f5)
    interpreter fields at x0 offsets  556   (0ee9b7c0)
    scavenge forwards in place        551   (52be7182)
    scavenge: used class table only   486   (e812eb9d)
    young lazy context, no barriers   477   (1751216e)

The class-table fix: forEachMemoryRoot visited all 4M class-table slots
at every scavenge (6.5 ms of 7 ms; lexicon does 390 scavenges).
PHARO_SCAV_STATS (diagnostic build) prints the phase times.

Also: young methods and blocks are called natively (`a336d1c1`; a
method compiled at run time stayed young, and every call to it left
machine code: 1042 -> 285 ms on a recursion micro-benchmark, Cog 230).
x86_64 tree compiles again (`25baba71`).

Call + return of a small method: 225 -> 201 instructions.  Where the rest
goes (lldb single-step, PHARO_NO_HEARTBEAT, $S/jitstep.py): caller
about 40, call stub 58 (25 of them write the 16-field frame record),
return 44.  lexicon makes 1.18 billion native calls, so each instruction
per call is 0.25%.  Become of young objects (WideString promotion) walks
all of eden, 2.6 ms each, about 2%.

## Right now (2026-10-14, late night) -- sweep L14 on `bdc7a854`

Pushed: origin/jit at `683c8b4d` (sweep L13: 27801 / 8 / 1).  Sweep L14
runs on `bdc7a854` with a newly prepped image (`setup_fake_gui.st` +
runner `ceb7b46`).  Push when L14 shows nothing outside the L9-L13
lists.

Since L13:

- `1febeb00`..`4c3fb6d9`: young closure fields stored directly, per-site
  `=` cache, inline primitives on 255+ slot objects.
- `17dfd75d`: `ensure:` and `on:do:` run in machine code.  They had
  `numBytecodes = 0` (a guard for the old resume protocol), so the
  machine-code frame could not start them.  An IC-miss exit also fills
  the site now (special-selector sends never filled it).
- Runner (submodule `2f33033`, `ceb7b46`): `Smalltalk stderr` raised a
  DNU in the exit `ensure:` of `runAllTests`, so each batch idled until
  the harness timeout (ReleaseTest: done at 100 s, killed at 1800 s).
  Now `Stdio stderr`.  `testUnknownProcesses` passes: the runner stops
  its two helper loops for that test and terminates (not only suspends)
  each watchdog.
- myprecious: 109/109 on our VM (Cog: 108, one failure).  soil, famix
  replication, methodproxies: same as Cog.

lexicon: 575 billion instructions, 28.3 s wall against Cog 11.3 s
(Rosetta).  Where the time goes (sample, 1590 samples): machine code 51%,
scavenge 8%, non-local return through `ensure:` about 10% (1.7M times,
all `^` inside `Symbol class>>rawIntern:`'s critical section; each one
makes all 14 frames contexts, and the frames below the home then return
by the slow context path), become 4%, MCF call stub 6%.  Exits are only
about 2% now (23M exits).  Tried and dropped: skipping the interpreter's
block-shape shortcuts in primitive 207 (neutral), new space 32 MB (632)
and 256 MB (596) against the default 64 MB (574).

Next: non-local return through `ensure:` without discarding the frames
below the home; then the call-record cost.

## Right now (2026-10-14, night) -- speed: registers, leaner calls, simStack; sweep L13 running

Pushed: origin/jit at `04a4a843` (sweep L11: 27803 / 7 / 1, the best so
far, nothing outside the earlier lists).  Not pushed yet: `972f27f0`
(packed call-cache entry, ldp return), `3a688767` + `bf…` simStack,
`67190b60`, `51484071` (lazy view).  Sweep L12 (`3a688767`): 4 non-passes
outside earlier lists, all pass alone on HEAD (timing tests run while I
benchmarked).  Sweep L13 on `51484071` is running.

Speed steps, lexicon instructions in billions (cycles in brackets):

    start of day                      714   [168]
    register receiver/tempBase (x21/x22)    [159]
    leaner record push, no code epoch 674   [145]
    inline primitive stubs            651   [140]
    simStack (deferred pushes)        640   [137]
    simStack constants, ==, stores    625
    lazy state view                   614

printString (3M) 29.7 -> 18.4 billion.  Against stock Cog under Rosetta
(indicative, machine loaded): arithmetic loop 24 ms vs 41 ms (faster);
send loop 205 vs 55; printString 114 vs 56; collect: 46 vs 15.  Sends
remain the gap: about 150 instructions per native call and return.  Next
levers: lite frame records, cheaper interrupt checks, a direct callee in
the inline cache (skip the call-cache probe), and simStack consumers for
remote temps.

Lesson (memory per-site-code-growth-thrashes-zone): code emitted at every
send site must be small; the first inline-primitive version overflowed
the code zone and made lexicon 2.5x slower.

## 2026-10-14, evening -- correctness fixes from the package differences

Commits after the push of `eb0bdbdb` (not pushed yet; sweep L10 on
`141818ec` must be clean first):

- `719cf9b2` snapshot saves pending mourners (ReleaseTest,
  WeakAnnouncerTest).
- `5cbed8c4` T1 inline `=` compared any two byte objects: large integers
  of opposite sign and `'ab' = #[97 98]` answered true.  Now only
  String>>='s case (ByteString receiver; ByteSymbol as Symbol>>=).
- `43b528b6` arithmetic special sends that miss the SmallInteger path are
  real sends from machine code (helper send), not a generic exit.
- `bf213943` PushArray builds the Array in machine code.
- `c80adedc` OSR enters block frames: a loop in a block that runs once
  (a DoIt, a test) was interpreted, 4x slower.  `PHARO_JIT_PERF_MAP`
  attributes `sample` addresses in JIT code to methods.
- `91e21241` primitive 188/100 left method_ = the target method, so an
  activating primitive recorded the caller's frame with the wrong method
  (DNU #numArgsError: on a CompiledMethod after a process switch).
  Compiled special #value honours an overridden FullBlockClosure>>value,
  as Cog's JIT does (methodproxies passes).  Two inline-cache poisoning
  paths closed.  Compare + forward conditional jump fusion is the default.
- `141818ec` two-way become keeps identity hashes with the references
  (Spur copyHash false); ReStore's 20 *WithExclusion tests pass.

Regression tests for each: `scripts/test_jit_semantics.st`
(JITSemanticsTest, 8 tests, in the VM fix battery).

Numbers: lexicon 714 -> 683 billion instructions; printString 28.9.
lexicon makes 1.18 billion native calls; per-call overhead (call stub,
frame record, return) is about 170 instructions, a third of all.  A
send-free loop is 3x slower than Cog (temps go through memory); a send
about 8x.  Those are the next speed levers.

Sweep L9 (`bf213943` binary): 27800 / 9 / 1; the four non-passes outside
L7's list pass alone (load from a concurrent package batch).

Open package differences: myprecious (2 errors, speed), soil (2
timeouts, speed), famixreplication (benchmark budget), restoreforpharo
(our run exceeds 900 s; rerun after the become fix).

## 2026-10-15 (misdated; was 2026-10-14 morning) -- lexicon passes; lazy outer contexts and MCF census done, pushed

After machine-code frames, the user approved the next step: lazy
contexts.  Phase 1 is in: lazy outer contexts (docs/lazy-contexts.md).
Block creation no longer materializes the frame stack; image code that
reads a Context syncs first; on arm64 Context methods compile with a
sync before their field reads and stores.  Rounds of the MCF exit
census then found call-path defects (docs/mc-frames-design.md, "Exit
census", items 1-9).  primitiveFileRead uses pread for large reads.

Numbers (instructions, billion): lexicon 1081 -> 774; printString 38.4
-> 29.9; deeptraverser 767 -> 677.  lexicon wall time on a quiet
machine 56.3 s -> 44.4 s (at `33217474`): the lexicon package matches
Cog (51 pass, 0 timeouts).

Validation: sweep L7 (`949cfc84`) 27880 / 11 / 1, the only non-pass
outside the baseline a TraitTestCase timeout; pushed through
`cffdbd03`.  Focused package batch on that binary: same as Cog except
myprecious (3 remote-message timeouts: a round trip is 6.5 ms, Cog
under Rosetta 1.5 ms), soil and famixreplication (speed budgets) and
newtools StDebuggerActionModelTest>>testEventAfterProceed, which is
intermittent in both modes (fails 1 of 2 class runs with
PHARO_NO_LAZY_OUTER, 0 of 2 without).

Next candidates: myprecious speed (the gap is broad: 180M instructions
per round trip); lazy contexts phase 2 (thisContext without a collapse;
in lexicon every collapse is Context>>resume:through:, non-local returns
through ensure:); a checkpoint that does not exit when it has nothing
to do (about 4% of myprecious).

## Right now (2026-10-14, 09:00) -- machine-code frames: done, default, pushed

The user asked for the redesign of how compiled code calls and returns into
interpreted methods.  It is the arm64 default (opt-out `PHARO_NO_MCF`) and
pushed (`7968e116`).  Design, defects found, validation and numbers:
`docs/mc-frames-design.md`.

Validation: sweeps M3-M6 at the baseline level (27876-27878 P against sweep
U's 27879); every non-pass outside the baseline lists passes alone, but
three were novel non-timing errors (watch item, no repro: a rerun of the
RSViolinPlotTest batch 3x on each binary was clean).  Battery,
test_class_table and test_relaunch (3/3 twice on a quiet machine) pass.
95 packages against Cog: identical except known differences (see the
design doc) and thalesgroup-pharoows (4-5 timeouts against remote map
servers, the same with `PHARO_NO_MCF`: not this work).

Instructions (billion): printString 83.7 -> 38.4, deeptraverser 1004 ->
767, lexicon 1699 -> 1081.  lexicon's CodeAnalysisRunnerTest takes 56.7 s
on a quiet machine (was 75-84 s; limit 50 s; Cog 11 s): the remaining cost
is block creation materializing contexts (docs/deferred.md, lazy contexts).

## Right now (2026-10-13, 17:30) -- array loops into T1

Under the arm64 resume-internal policy, at: / at:put: / size
send sites are emitted without inline J2J (their inline primitive paths
stay; the rest goes through dispatchCached), so a loop whose only sends are
those plus inlined arithmetic is no longer refused.  A byte-array sum loop
(`zzSum:`, 21M iterations) 600 -> 129 ms, correct result.  Real workloads
unchanged (printString 83.7, deeptraverser 1004, lexicon 1699, myprecious
44.3 billion), as the census predicted -- this is for numeric and array
code.  Battery passes; sweep U 27879 P / 9 F / 2 E (its new TKTWorkerPoolTest
error passes alone 5/5); ten numeric/array packages (polymath,
vector-matrix, famix, linear-models, graph-algorithms, array2d,
edit-distances, linear-algebra, neojson, neocsv) identical to Cog.

## Right now (2026-10-13, 16:30) -- T1 compiled method TRAILERS: loops refused; fixed

A loop of SmallInteger arithmetic in a method (`zzArith:`, 30M iterations)
ran at interpreter speed with the JIT on (546 ms; off 517; Cog 44).  The new
`[T1-REFUSED]` trace line (PHARO_T1_TRACE_COMPILE, diag build) and the
opcode now printed in the refusal log showed why: T1 refused it at "bc 29
op 0xad" -- a byte of the method TRAILER (source pointer), not a bytecode.
`computeLiveLength`, which trims the trailer, read every extended jump's
operand as an unsigned forward offset and ignored the ExtendB sign prefix,
so a loop's back edge `E1 FF ED xx` looked like a jump past the end and
nothing was trimmed.  Trailer bytes were then emitted as bytecodes; one that
decodes as a send reached the arm64 inline-J2J closure-push refusal and the
whole method stayed interpreted.  Fixed (ExtendB decoded as the interpreter
does; a back edge cannot keep later bytes alive): `zzArith:` 546 -> 133 ms
(x86 204 ms), correct result.  Instructions, before -> after: printString
83.7 -> 83.7, deeptraverser 1039 -> 1004, lexicon 1781 -> 1703, myprecious
44.2 -> 44.3.  Battery passes; sweep T 27880 P / 10 F / 1 E (its new
ProcessSpecificTest>>testLocalVariable passes alone 5/5 and in its batch
2/2); heavy packages polymath, vector-matrix, famix, linear-models,
graph-algorithms identical to Cog (deeptraverser, lexicon, cormas: their
timing tests under load, as before).

Context (unchanged by this fix): compiling every send-bearing method
(`PHARO_T1_RESUME_EXTERNAL_J2J=1`) still loses on real code -- printString
83.7 -> 104.6, deeptraverser 1039 -> 1387 -- so the closure-push refusal for
real sends stays.  Static census: 2,573 of the image's 138,862 methods have
loops.

## Right now (2026-10-13, 15:00) -- end-of-day numbers; `0aa5f085` pushed

Instructions retired, this morning -> now (build-rel, JIT on):

    3M printString                      210.8 ->   83.7 billion
    deeptraverser testCollectionStart  2311   -> 1039
    lexicon CodeAnalysisRunnerTest     2736   -> 1781   (sweep-E binary first)
    myprecious 200 round trips           49.4 ->   44.2

Sweeps all day: 27875-27881 P, 9-13 F, 1-3 E, every non-pass attributed.
Packages: 83 August-parity packages plus the ten August JIT-only ones
re-checked; every remaining difference is speed (lexicon 1 TIMEOUT,
myprecious 3 ERR, deeptraverser under load, restoreforpharo #7,
famixreplication's benchmark test), network/services, environment
(pharo-cig libclang), or the attributed special-#value split.  newtools now
934 P against Cog's 935 (931 this morning): StSpotterModelTest passes since
the parallel become; the one JIT-only failure left is the attributed
StDebuggerActionModelTest>>testEventAfterProceed timing.  The common
cause of the speed residue is activation cost (separate Smalltalk stack,
SavedFrame per send), which is a JIT/interpreter redesign, not a tuning job.

## Right now (2026-10-13, 13:30) -- `f9220e43`/`e9ba6650` pushed; frame-0 resync skip (sweep P)

Uncommitted: materializeFrameStack no longer re-copies frame 0 (the
suspended context-mode base) into activeContext_ on every call -- it is
suspended, so once synced it stays exact; `ctxSynced` now stays true after a
sync instead of being forced false.  Lexicon 1832 -> 1781 billion
instructions.  ContextTest, BlockClosureTest, ProcessTest, SemaphoreTest,
ExceptionTest and the battery pass; sweep P: 27879 P / 10 F / 1 E, nothing
new.

Package batch 3 (26 more August-parity packages): identical to Cog except
myprecious (speed) and soil (same 687 P; Cog and we time out different
SoilCleanCodeTest tests).  With batches 1-2 and the heavy set that is 83
packages checked today; every difference is speed (lexicon, myprecious,
deeptraverser under load) or environment (google-cloud before OpenSSL 1.1,
pharo-cig's libclang) or the attributed special-#value split
(methodproxies).  Batch 4 (the ten with August JIT-only failures) and a
newtools re-run are running.

FFI callbacks: 175 us each against Cog's 10.7 (qsort with a Smalltalk
comparator).  The callback loop now scavenges when eden is full (it never did,
so callbacks ran on full GCs): 95 us.  Sweep S 27881 P / 9 F / 1 E, nothing
new; the callback suites (FFICallback*, TF*Callback*) match the old binary.
An FFI callout is 6.3 us against Cog's 1.5 -- all of it UFFI's Smalltalk
marshalling, i.e. interpreter speed.

Batch 4 (the ten packages with August JIT-only failures): pharo-sqlite3
(111 then) and porpoise now identical to Cog; p3 / redistick / tsf-nexio /
soccertheory / escapeanalysis identical (service-less or runner-level
errors on both); pharoows 2 network TIMEOUTs; famixreplication 1 benchmark
TIMEOUT; restoreforpharo still over the 900 s budget (#7, speed).

## Right now (2026-10-13, 12:00) -- #30 closed: re-home rule off, sweep O clean

`cf1a7e5b` (#30 live-chain check, #32 FFI, LEAD 24, prim 132) pushed after
sweep N.  Next: the #30 re-home rule itself is now OFF by default (opt-in
`PHARO_REHOME_CLOSURES`): with the live-chain check it re-homed nothing in
batch 1151-1200, and with it fully off that batch is clean and 1901-1950
matches the old binary.  Sweep O on that tree: 27879 P / 9 F / 2 E, nothing
new; cormas, polymath, famix identical to Cog (deeptraverser's 50 s test
timed out under the sweep's load, as before).

After vm-O (not in sweep O): the fd==0 `PushThisContext` pc rewind in
materializeFrameStack now requires `!inExtension_` and decodes the method to
confirm the byte before ip starts an instruction (deferred.md, prefix
paths).  (A note here that context mode made a loop 5x slower was a
measurement error -- see deferred.md's correction; resume-as-frame was built,
measured as no gain, and reverted.)

## Right now (2026-10-13, 11:00) -- #30 wrong home fixed in the repro; #32 FFI crash fixed

`8ed7f35a` pushed (sweep M 27880 P / 9 F / 1 E).  The next commit (sweep N:
27879 P / 10 F / 2 E, its new LinkInstallerTest error a missing-fixture
KeyNotFound that passes alone 5/5 on both binaries; SUnit batches 1151-1200
and 1901-1950 as on the old binary):
- **#30**: the closure re-home rule in materializeFrameStack now skips a
  closure whose old outerContext is still on the live sender chain (a
  recursive activation, not a stale duplicate).  The `m:with:` repro answers
  `#(1 1 1 1 1)` like Cog in interpreter, default JIT and compile-all (was
  `#(0 0 0 0 0)` in all three).  The rule's documented load-bearing case
  (SUnit batch 1151-1200 broke with the rule off) passes: 2/2 clean.
- **#32** (vm-compat-bugs.md): a versioned FFI module name that cannot be
  opened no longer borrows same-named symbols from the process-wide
  namespace; google-cloud's SIGSEGV in LibreSSL is gone, and with Cog's
  arm64 `libcrypto.1.1.dylib` beside the binary its auth tests pass.
  `build-rel/` now has that file staged (untracked build dir).
- `sendSelector`'s megamorphic-cache refill moved out of line.

Package regression batches against Cog on this Mac (packages at parity in
the August Linux catalog): batch 1 (26, `8ed7f35a` binary) all identical
except google-cloud (#32; identical once OpenSSL 1.1 is beside the VM).
Batch 2 (27, this tree): identical except
- methodproxies `MpMethodProxyTest>>testCanWrapValueWithException`: the
  attributed Cog interpreter-vs-JIT special-`#value` split (memory note
  cog-interpreter-and-jit-differ; OCSpecialSelectorTest pins the other side);
- pharo-cig: Cog looks for libclang in `/Applications/Xcode.app` (absent on
  this CLT-only host) and fails cleanly; our basename retry finds the CLT
  libclang, the tests run deeper, an image DNU (`CXFile class
  #newValueHolder`) raises inside a TFFI callback, which runs in its own
  process, so it is unhandled and headless Pharo quits -- environment, not
  a VM defect.
- `PHARO_NO_ASMJIT_T1` now means "no JIT" (LEAD 24: the legacy stencil T1
  looped forever); prim 132 on a Context stops at its stack pointer (#31).

## Right now (2026-10-13, 09:30) -- famix, polymath, soil at parity; parallel become

`329924b6` pushed.  Package A/B since then (Cog vs ours, same image):
famix 1316 P both, polymath 776 P both, soil 687 P both (Cog fails one
SoilCleanCodeTest and times out another; we time out both), newtools 931 vs
935 P (the four differences are the attributed timing tests in
`docs/vm-compat-bugs.md`).  Still speed-bound: lexicon 1 TIMEOUT, myprecious 3.

Committed in one unit; sweeps J (first item) 27878 P / 12 F / 1 E, K (through
parallel become) 27880 P / 9 F / 1 E, L (through the count gate) 27881 P /
9 F / 1 E, nothing new in any.  Sweep M re-runs the exact commit:
- `pushFrame`'s fatal bad-header report and sp-corrupt trace moved out of
  line; one shape check (`hasAtLeastSlots(3)`) then raw slot reads in
  `activateBlock` / `primitiveFullClosureValue`; raw method-header read in
  `pushFrame`.  Send 471 -> 403 instructions, block call 708 -> 648.
- New space 64 MB (was 32), eden 100% of it (the 20% survivor reserve was
  never used): lexicon 2069 -> 1942 billion instructions.
- Old-key `becomeForward:` scans old space with a worker pool (opt-out
  `PHARO_BECOME_SERIAL_SCAN`): restart points every ~1 MB
  (`ensureScanCuts`, rebuilt after fullGC), workers collect hits, the main
  thread applies them through the barrier.  2000 ByteString -> WideString
  upgrades 15.3 -> 3.3 s; `StSpotter new` 86 -> 38 ms cold (Cog 28).
  Correct with the forwarder safety net disabled (diagnostic build).

- JIT entry counting only for code a count can compile: the method-cache
  entry caches `JITRuntime::entryCountMatters` (`jitCounted`; under the
  loops-only T1 policy, code with a loop), blocks the same through the
  per-block facts memo.  The count probe ran on every send: printString
  90.4 -> 85.0 billion, deeptraverser 1132 -> 1056, lexicon 1942 -> 1833,
  myprecious 48.2 -> 44.9.
- The Sista backward-jump hook is gated inline on the Sista runtime existing
  (it is created on the first Sista activation); `returnFromMethod` is a
  small function whose plain-method case calls `returnValue` directly, the
  NLR and diagnostic paths moved to `returnFromMethodSlow`.  printString
  85.0 -> 83.8 billion.
- Tried and reverted: inlining the OSR countdown at the nine backward-jump
  sites (+0.5%), a lean `sendSelector` front for plain sends (+2.7%: the
  primitive and shortcut sends then pay two calls).

`[DEPTH0]` measurement: context mode is 0.1-2.5% of checkpoints on the slow
packages, so married contexts are not their lever (deferred.md corrected).

## Right now (2026-10-13, 07:00) -- sweeps E/G/H clean; deeptraverser at parity; lexicon -23%

Sweep E (exact `064e1de6` tree, nothing alongside): 27875 P / 12 F / 3 E,
39 min, no killed batch.  The three non-passes not seen before are all
intermittent: ZnHTTPSTest (external network), MCSmalltalkhubRepositoryTest
(passes alone), StSpotterModelTest>>testSpotterModelShouldWaitToPerformActualSearch
(a 250-550 ms Delay window; fails on the older vm-D binary too when run after
StSpotterTest, passes alone 4/4).

Uncommitted, measured by instructions retired:
- `primitiveFullClosureValue` remembers per CompiledBlock (per
  `g_objectMotionEpoch`) that no shape shortcut fired and skips the matchers:
  one block call ~820 -> 722 instructions; NoUnusedVariablesLeftTest -3.4%.
- One `trivialShape` flag on the method-cache entry gates the five
  trivial-method shortcuts; `probeCache` inline.
- File primitives: `primitiveFileRead` freads straight into the buffer (was a
  temporary vector + one `storeByte` per byte, 18% of lexicon's
  CodeAnalysisRunnerTest); `primitiveFileWrite` gathers with one copy.
- `storePointer` common path inline (out-of-bounds and diagnostics go to
  `storePointerSlow`).
- Young-only `becomeForward:` (opt-out `PHARO_BECOME_FULL_SCAN`, check
  `PHARO_BECOME_YOUNG_VERIFY`): eden + remembered set + recent old space +
  perm instead of the whole heap.  Lexicon's becomes are old ByteStrings
  (`ByteString>>replaceFrom:to:with:startingAt:` -> WideString) so it does
  not help there; old keys need lazy forwarders (Cog's read barrier).
- `materializeFrameStack`: synced frames take a short path (sender link
  only); fresh contexts skip the tail-clearing scan; `setSenderSafe` returns
  early when the link is unchanged.
- Scavenge forwarding table: flat open-addressing table reused across
  scavenges instead of `std::unordered_map`.

Lexicon CodeAnalysisRunnerTest>>testLatexTableForPackagesTest: 2736 -> 2097
billion instructions, 124.6 -> ~100 s (Cog 11 s).  Its profile is now the
interpreter itself (send/return/activate ~55%), scavenge ~9%, become of old
keys ~4%, frame materialization ~6%.

Sweep F was spoiled: I ran targeted class runs beside it and they share
`/tmp/sunit_*` with the sweep (memory note sunit-runner-shares-tmp-state).  The
scratch wrappers now refuse to run while a sweep is in flight.

Also in this commit: `+ - < > <= >= = ~=` each get their own dispatch label
(one indirect jump instead of two); the inline `noteMethodEntry` gate probes
the count map's full 8 slots (it stopped at 4, so a method whose entry sat at
probe 5-8 took the ~150-instruction slow path on every call -- 12% of a
block-calling loop); a `[DEPTH0]` exit line counts bytecode checkpoints taken
in context mode.

Validation: sweep G (through young become) 27876 P / 13 F / 1 E; sweep H
(through the arithmetic labels, with `PHARO_BECOME_YOUNG_VERIFY`) 27875 P /
12 F / 2 E, zero young-become misses.  Its two new non-passes
(SpDropListPresenterTest>>testExample, ZnClientTest>>testTimeout) pass alone
3/3 on both binaries.  VM battery passes on the final tree.

Package A/B on the sweep-H binary: deeptraverser now at PARITY (53/53, its
50 s test passes).  lexicon: 1 TIMEOUT left (~100 s vs Cog 11 s).
myprecious: 3 errors left, `MpRemoteMessageResultTimeout` -- a round trip is
~11 ms of CPU (Cog 2.4 ms) and nested proxy equality needs several inside one
short reception timeout; the round trip's time is Fuel's
`isBehaviorAnonymous:` scanning all of `Smalltalk globals` with a block per
element.  The JIT is net neutral on these workloads (printString 91.3 vs
93.2 billion with/without; deeptraverser 1147 vs 1138; myprecious 49.1 vs
49.4) and a T1-compiled loop calling a block is ~50% slower per call than
the interpreter (1002 vs 698 instructions).

## Right now (2026-10-13, 04:40) -- trusted remembered set ON; cormas at parity

**`c0f09c26` pushed (sweep C: 27876 P / 12 F / 1 E, 45 min).  The next
commit adds:** the trusted remembered set by default (opt-out
`PHARO_SCAV_FULL_SCAN`; every T1 store barriered; Sista dispatch off by
default because its stores are not), cold paths split out of the hot frames,
inline `literal()` / `primitiveSuccess`, a `primitiveAt` fast path, and for
blocks direct block-return dispatch plus skipping the NLR home search when a
block has no `^`.  Sweep D (trusted scan + `PHARO_SCAV_TRUST_VERIFY` on
every scavenge, binary from before the block changes): 2047 classes, zero
missed holders, no new deterministic failure (RSLinesTest>>testMarkerOffset
passes 3/3 alone).  Two of its batches were SIGKILLed mid-run with no VM
output while I ran package A/B jobs alongside -- probably memory pressure;
the retry pass recovered them.  Sweep E (exact tree, nothing alongside)
gates the push.

Package results on the new binaries: cormas-cormas at parity with Cog
(332 P both; the 1 s `testRunIsAsynchronous` now passes, analysis ~935 ms).
deeptraverser `testCollectionStart` ~47 s quiet against a 50 s limit (Cog
6 s) -- still TIMEOUT under the A/B runner with a sweep alongside.
myprecious round trip ~10.6 ms (Cog 2.4 ms): needs a structural change.

Measured instruction counts, morning -> now: 3M printString 210.8 -> 93
billion; deeptraverser 2311 -> 1200; one block call ~2370 -> ~800.

## Right now (2026-10-13, 02:40) -- interpreter send path, ~40% fewer instructions

**Speed work on the interpreter, every change measured by instruction count
(`/usr/bin/time -l`, deterministic; wall clock swings 10-20% here).**  3M
printString: 210.8 -> 104.3 billion instructions with the JIT on, 173.4 ->
~93 with it off.  `bench_ast_phases.st` phases 36-50% faster than this
morning's binary; cormas analysis 1.6 s -> ~1.1 s; myprecious round trip
23 -> ~11 ms; deeptraverser `testCollectionStart` 66 s (limit 50 s, Cog 6 s).
Sweep B (first half of the changes): 2047 classes, 27875 P / 12 F / 3 E in
45 min, no batch needed a retry; the three new names (RSLinesTest #22 flake,
SpDemoTest fake-display order, ZnStaticFileServerDelegateTest second-boundary
timestamp) pass alone on both binaries.  Sweep C (everything) is next.

What changed, biggest first (details: `docs/perf-activation-measurements.md`,
"The send path, measured by instruction count"):

- Release builds compile diagnostic knobs out (`PHARO_DIAG_KNOBS`, CLAUDE.md);
  `DIAG_FLAG()` does the same for runtime trace globals in hot paths, and the
  four per-bytecode trace-flag loads in `DISPATCH_NEXT` are gone (-23% per send).
- T1 compiles only code with a loop (`PHARO_T1_COMPILE_ALL` restores all):
  entering emitted code for a small leaf cost more than it saved.
- Direct dispatch for returns, special-selector sends, Extend A/B, extended
  jumps and integer/character pushes (they went through `dispatchBytecode`).
- Per-send JIT bookkeeping: mega-cache refill gated on `jit::g_jitStateEpoch`,
  inline `noteMethodEntry`, Sista skip memo, method-map presence filter, lazy
  J2J-slice clearing, `[JIT] Stats` every 16M entries instead of 64K.
- Plain method returns skip the home-method walk; `classOf`, `specialObject`,
  `isOld`/`isYoung` inline; no thread_local on the back-jump path.
- Remembered-set groundwork (`docs/deferred.md`, the old-space-scan item):
  exact remembered bits, the recent-region mark reset by full GC, barriers on
  setSpecialObject / become / class-table pages.  The audit now shows zero
  interpreter-side gaps; the JIT's store emits are what still block trusting
  the set.  Perm space is empty (it always was; scans no longer walk 8 MB).

New finding, not fixed: a process switch materializes the whole stack, so a
long-running method that is ever preempted finishes in "context mode" (frame
depth 0: temps mirrored into the Context, pc stored on every send).  Cog's
married contexts avoid this; it is the next structural lever after the JIT.

## Right now (2026-10-12, 23:50) -- stopped for a reboot

**Tree clean, everything pushed (`8b71c753`).  No sweep or package run is in
flight; nothing in the scratchpad needs saving.**  Since the 19:40 entry:

- `553a942a` cheap exits on the activation path (`tryJITResumeInCaller`
  returns early when the caller has no executable JIT method;
  `SistaRuntime::hasSplice` skips the lookup when nothing is spliced) and
  `PHARO_TRACE_BECOME` reports `youngKeys`;
- `8b71c753` `PHARO_SCAV_BIT_AUDIT` splits write-barrier gaps by holder age
  (`gapRecent` = holder allocated in old space since the last scavenge, mark
  `oldFreeAtLastScavenge_`) and logs only the gaps in older holders.

Latest full sweep: 27879 P / 11 F / 1 E, every non-pass attributed.  All
remaining work is speed.  Three candidate projects, best measured first:

1. **Trust the remembered set** (`docs/deferred.md`, "Every scavenge scans
   ALL of old space").  Next step: find the 60 older-holder gaps (43 are a
   Context sender or a FullBlockClosure outerContext pointing at a young
   Context, 5 a CompiledMethod literal = young TFExternalFunction), then scan
   only `[oldFreeAtLastScavenge_, oldSpaceFree_)` plus the remembered set,
   behind an opt-out knob.  Done when `PHARO_SCAV_BIT_AUDIT` shows zero gaps
   over a full sweep.
2. **Lazy become** (Cog follows forwarders; ours scans the heap, about 7 ms
   per call; StSpotter does 12 ByteString -> WideString becomes per open).
   Needs item 1 for a young-only variant.  Analysis: `docs/vm-compat-bugs.md`
   #2, batch 5 newtools paragraph.
3. **Activation wall** (#6: myprecious round trips 10x Cog, polymath, cormas,
   famix).

**A reboot clears `/private/tmp`.**  The session scratchpad, its sweep image
and package images, `/tmp/harness` and `/tmp/harness-x86` are all gone
afterwards.  Rebuild them before the next run:

- `/tmp/harness` and `/tmp/harness-x86`: see "Running Pharo test suites" in
  `CLAUDE.md` (the x86 one needs the `arch -x86_64` prefix).
- Sweep image: prep a copy of `/tmp/harness/Pharo.image` with our VM
  (setup_fake_gui.st, then run_sunit_tests.st, then
  `Smalltalk snapshot: true andQuit: true`), as in `CLAUDE.md`.

Full sweep, about 55 min.  Copy the binary and its dylibs into `$S/vm-X/`
first, and run no other eval of our VM while a sweep runs:

```
PHARO_PRIM_SP_AUDIT=1 STEP=50 PER_BATCH_TIMEOUT=1800 RETRY_DAMAGED=1 \
PHARO_CODE_ZONE_MB=192 PHARO_MAX_STEPS=4000000000000 \
PHARO_MAX_OLD_SPACE_MB=12288 \
  scripts/sunit-sweep.sh $S/vm-X/test_load_image $S/work/sunit.image $S/sweep-X
awk '/^ *=== [A-Za-z0-9]+ ===$/{c=$2} /^  (FAIL|ERROR|TIMEOUT): /{print c, $1, $2}' \
  $S/sweep-X/all_results.txt | sort -u
```

Package A/B against Cog (the scratchpad `pkg-run.sh`, lost on reboot).
Per label in `scripts/pkg-jit-test/packages-200.tsv` (column 12 = test
prefixes, column 15 = load expression), the script loads the package with
stock Cog into a copy of the pristine x86 image, then runs both VMs.
`$S/bin/pharo-x86` is a two-line wrapper,
`exec arch -x86_64 /tmp/harness-x86/pharo "$@"`.  The core lines are:

```
cp /tmp/harness-x86/Pharo.image $img; cp /tmp/harness-x86/Pharo.changes ${img%.image}.changes
(cd /tmp/harness-x86 && rm -f startup.st && \
  timeout 1500 arch -x86_64 ./pharo $img eval --save "$expr")
PHARO=$S/bin/pharo-x86 CUSTOM_VM=$R/build-rel/test_load_image OUT=$S/pkgout7 \
  COG_TIMEOUT=900 JIT_TIMEOUT=900 timeout 2400 \
  bash scripts/pkg-jit-test/run-pkg-jit-test.sh $img $label "$prefixes"
```

Parity means one distinct `RESULT pass= fail= err= timeout=` line and no
ERR/FAIL/TIMEOUT between the `JIT-only` and `Cog-only` markers.  For slow
packages, use 2400 s per VM and a 5400 s outer timeout.

## Right now (2026-10-12, 19:40)

**Catalog re-run on arm64 finished: all 141 packages re-checked; every
remaining difference from Cog is speed or an environment limit.**  Batches
1-5 are in `docs/vm-compat-bugs.md` #2.  Fixed on the way today (all swept):

- `d9f8888f` pipe `atEnd`/size (OSSubprocess 9/9) and the stack contract of
  the FilePlugin / SocketPlugin named primitives;
- `5753f317` the numbered-primitive audit (#31): closure fast path read the
  receiver slot, nested primitives got the outer argCount_, perform arity,
  minVal / -1, Float nan truncated, Context size/at:, FFI stores, ...;
- `56c1cd64` idle sleep now ends at the timer deadline (`1 milliSecond wait`
  12.6 ms -> 1.5 ms); eval mode no longer suspends Morphic (a harness defect
  that failed Spec tests under the package runner only); become no longer
  walks all 4M class-table entries (StSpotter new 165 -> 84 ms).

Open, all speed: activation wall (#6: myprecious round trips 10x Cog, polymath,
cormas, famix, ...), and become = full heap scan (Cog's is lazy; #2 batch 5
newtools paragraph has the analysis).  SUnit: 27878 P / 11 F / 2 E, every
non-pass attributed (image bugs, Cog-shared, network, speed).

## Right now (2026-10-12, 08:00)

**Packages re-checked against Cog, all freshly loaded: seven more at parity.**
anthropic-sdk (895 P / 3 F / 36 E), gitprojecthealth, pillar, ume, viennatalk
(1430 P), mutalk (336 P / 1 E, was SIGSEGV) and soil (after a become fix:
class reshaped twice lost its instances -- `noteClassHashCopy`,
`scripts/repro/class-reshape-twice.st`).  Full sweep with that fix:
27879 P / 11 F / 1 E.  Details: `docs/vm-compat-bugs.md` #2.

## Right now (2026-10-12, 04:40)

**The arm64 default now keeps send-bearing (inline-J2J) methods OUT of T1:
full sweep 56 min instead of 1 h 53 min, 27876 P / 12 F / 1 E (baseline
27872 / 13 / 1).**  It is labelled "resume-internal J2J" but that is not what
runs -- the closure push it needs never encodes, so those compiles are
refused (now on purpose); real resume-internal J2J corrupts.  See
`docs/perf-activation-measurements.md`, "Correction".
The 1-in-22 crash that kept it off was an interpreter bug: `periodic_checks`
ran the profiler tick and the low-space signal before its `inExtension_`
early-out, so a profiler sample could switch process between `E1 FF` and the
jump it extends.  Fixed, plus the same shape in the FFI callback cooldown
(found by an audit workflow).  Write-up: `docs/perf-activation-measurements.md`
("The internal-J2J crash"); sweep: `docs/results/sweep-arm-2026-10-12-internal-j2j.md`.
Opt-out `PHARO_T1_RESUME_EXTERNAL_J2J`; x86_64 stays opt-in
(`PHARO_T1_RESUME_INTERNAL_J2J`) until a Rosetta run validates it there.

## Right now (2026-10-11, 23:50)

**Packages: jrpc fixed, stargate at parity, myprecious down to 3 speed errors.**

    juliendelplanque-jrpc     Cog 81 P     ours 81 P / 0 F / 0 E (was: no RESULT)
    ba-st-stargate            Cog 473 P    ours 473 P (parity)
    smalltalkweb-myprecious   Cog 109 P    ours 106 P / 3 E (was 90 P / 16 E)

jrpc had two defects, one behind the other (details: `docs/vm-compat-bugs.md`,
jrpc paragraph under #2): a listening socket's close did not free the port
(`07dfc643`), and a JIT chain-loop fast path ran a block from its first
bytecode after a process switch, replaying `self schedule` in
`DelayWaitTimeout>>wait` (repro `scripts/repro/block-replay-after-process-switch.st`,
39322 replays in 40000 signals before the fix, 0 after).

myprecious's 3 errors are all `testArgPassByReference`: a nested proxy round
trip with a 100 ms reply timeout.  Ours needs ~110 ms, Cog ~12 ms.  The cost
is Fuel serialization, 17x slower than Cog, and it is the activation wall --
two side fixes (`sigsetjmp` savemask, named-primitive cache: -25% / -31% on
their microbenchmarks) did not move it.  See
`docs/perf-activation-measurements.md`, "Two per-call costs outside the JIT".
Full sweep with both: 27869 P / 14 F / 3 E (baseline 27872 / 13 / 1).  The
new names are intermittent and pre-existing: RSLinesTest>>testMarkersIncludesPoint
(#22's BlockCannotReturn, 2 in 10 runs on the build from before tonight),
StDebuggerTest>>testDynamicVariableEvaluation (Cog-shared), and two classes
that pass in isolation on every build.

## Right now (2026-10-11, 23:00)

**Speed: resumed methods run with J2J OFF, and the knob that turns it on is
-48% sweep wall time -- but crashes 1 in 40 batch runs.**  A read-only audit
workflow found why 69% of sends round-trip through C++: `tryJITResumeInCaller`
gives a resumed method a NULL J2J save slice unless
`PHARO_T1_RESUME_INTERNAL_J2J` is set.  With it set, a full sweep takes 59 m
instead of 1 h 53 m with the same results, but `AndreasSystemProfilerTest`
crashes (SIGSEGV, `method_` = `Float>>absPrintExactlyOn:base:` with an ip past
its end) about 1 run in 40; never without the knob.  Default stays OFF.  The
hunt is in `docs/perf-activation-measurements.md`: ~15 O(1) coherence checks
(`PHARO_RESUME_EXIT_CHECK`) all stay silent before the crash; next step is an
lldb hardware watchpoint on `instructionPointer_`.

**Default-on today and validated on both arches:** `push()` fast path, raw
Sista save pool, `upgradeICToJ2J` fast exit (-25% on send-bound code), the
primitive 196 nil-target fix, and the #28 widow fix.

## Right now (2026-10-11, 17:30)

**#23 is a CROSS-PROCESS kill during test-process termination.**  A forked
SUnit test process, while being terminated, runs `resume:through:` on the
runner P79's loop-body block context, executes it, and widows it; P79 later
returns into the dead context and the storm guard kills the runner.  Caught
with a suspended-chain registry (`PHARO_ACTIVATION_CHECK`, now committed).
Ruled out as the route: every VM widow site, primitive 196's kill loop, the
handler search (prim 197), and `handleContextNLRUnwind`.  Full trail in
`docs/vm-compat-bugs.md` #23.

**New defect #30, OPEN:** closure re-homing by method match gives a block the
wrong home under recursion (`m:with:` answers 0, Cog 1).  Turning the rule off
regressed the SUnit runner (a block read outer temps from a stale duplicate
Context), so it is back ON with `PHARO_NO_REHOME_CLOSURES` as the opt-out.
The real fix is finding where the duplicate Context is made.

**Primitive 196** now kills nothing for a nil target, as Cog does (e7664731).

## Right now (2026-10-11, 13:00)

**Defect #28 is FIXED, and TKTWorkerTest passes: there is no class left on
the "ours" list.**  Two bugs, both in `src/vm/Interpreter.cpp`:

1. `tryJITResumeInCaller`'s `ExitReturn` never widowed the returning frame's
   Context when `popFrame()` succeeded -- 15 of the 19 leaking returns.  The
   2026-09-04 attempt hung because it widowed BEFORE `popFrame()`, and on the
   fd==0 branch that Context is `activeContext_`, whose sender the next line
   reads.  Now widowed after the return value is delivered.
2. The chain loop's inline-activation fallback pushed the caller's SavedFrame
   with `materializedContext = nil` and left `currentFrameMaterializedCtx_`
   set, so the callee inherited its caller's Context.  With fix 1 alone that
   would have widowed live callers (#23's dead-sender shape).  Found by a
   method-match guard that declined ~17000 widows a run; 0 after the fix,
   and 0 in all 41 batches of the full sweep.

TKTWorkerTest A/B 4 reps: 7 P every time (9-10 s) against 4-5 P / 1-2 E / 1 T
(~90 s).  Full arm64 sweep: 2047 classes, 27872 P / 13 F / 1 E / 4 T, 99.27%,
every non-clean class shared with Cog or slow -- see
`docs/results/sweep-arm-2026-10-11.md`.

**New lead:** the macOS fake-GUI wedge batch (651-700) carries #23's
dead-sender + storm-guard lines.  The wedge and #23 may be one bug.

**Next:** the residual is speed (defect #6 TIMEOUTs; package TIMEOUT-only
divergences).  `scripts/perf-activation/bench_ast_phases.st` splits the
whole-image AST scan into phases to find where the 28x interpreter / 37x JIT
gap to Cog sits.

## Right now (2026-09-04, 04:15)

**The residual is one class, and the rest of the gap is speed.**  A
matched-runner check (our VM under `run_sunit_cog.st`, the runner stock Cog
uses) collapsed four "ours" classes to one: `LinkInstallerTest`,
`SpJobListPresenterTest`, `SpHorizontalBoxLayoutTest` and `TraitFileOutTest`
all pass on our VM under that runner, `StSpotterTest` fails identically on
both VMs, and only `TKTWorkerTest` is genuinely ours.  Every Δcog number in
this repo carries that caveat now — it is only a VM claim when the runners
match, and ours forks each test into its own process while Cog's runs inline.

**`TKTWorkerTest` (defect #28) is root-caused.**  Measured in the image at the
last tearDown after a full GC, same binary, only `PHARO_NO_JIT` changed:

    arm        privateStart activations   contexts alive   widowed
    JIT on              19                      7             6
    JIT off             19                      0             0

Block creation calls `materializeFrameStack()`, which reifies EVERY saved
frame rather than only the one owning the closure; the process-body block's
`outerContext` then drags a real Context chain back to
`TKTWorker>>privateStart`, pinning its receiver.  Six of the seven survivors
are already widowed, so widowing is not the lever — a widowed Context still
holds its receiver.  The fix needs the lazy half of Cog's married/widowed
protocol (a married context's sender is a frame pointer, not a caller
Context).  Not attempted.

**The TIMEOUT residual is the JIT being slower than our interpreter.**  Defect
#6 measured to completion for the first time, outside SUnit so the scan
finishes instead of being killed:

    stock Cog (x86_64 Rosetta)     5.76 s
    ours, JIT on                 216.8 / 217.3 / 218.2 s
    ours, JIT off                177.4 / 160.9 / 161.2 s

37x Cog, and **35% slower than our own interpreter**.  That is the known "1M
blocks: JIT 5.5x SLOWER" regression showing up as test failures — a
whole-image scan is `allMethodsDo: [...]`.  A knob triage puts the only
measurable share on the caller-resume path (`PHARO_T1_NO_CALLER_RESUME`
-14%, matching `tryJITResumeInCaller` topping the sample); both block-value
specialisation knobs are within noise.  Full table in
`docs/perf-activation-measurements.md`.

**A validation re-sweep of real x86 is running** with today's two functional
changes (`RUN_FROM_IMAGE_DIR` default, the `popFrameForJIT` widow), at index
~1750 of 2047 with 8 F / 3 E so far.

**Two claims of mine were withdrawn after measurement**, both recorded where
they were made: `primitiveQuit` IS reached in the x86 "wedge" (shutdown is
660 ms; the ~22 s is inside the image, and it is 1 s on arm64, 7 s under
Rosetta, 22 s on native Linux), and the `popFrameForJIT` widow store is
**inert** — the `[WIDOW]` tally reports it firing 0 times per run, so the
retention improvement first attributed to it was noise.

## Right now (2026-09-04, 01:30)

**The real-x86 sweep FINISHED.**  All 2047 classes on native x86_64 Linux:

    arch                classes   tests    PASS   FAIL  ERROR  SKIP   T    rate
    x86_64 (real HW)      2047    28074   27858    13     5     179   19  99.23%
    arm64 (same week)     2047    28060   27857    11     4     182    6  99.28%

Two boxes, one `RUN_ID`: the first was reaped at index 500 and the second
resumed from the S3 checkpoint at 501.  Full report and the per-class
artifacts are in `docs/results/sweep-x86-real-2026-09-04.md`.

The arch diff is 20 x86-only non-clean classes (12 of them TIMEOUT-only), 4
arm-only and 10 shared.  Nothing in it looks like codegen: of the 7 x86-only
FAIL/ERRORs, four carry their own explanation (a wall-clock percentage, a
terminal command that differs between Linux and macOS, a missing certificate
file, two Spec layout/selection tests under the fake GUI).
`LinkInstallerTest`'s "Leaked metalinks" is the one worth a second look.
A stock-Cog baseline for all 30 non-clean classes is running ON THE SAME BOX,
which is the airtight way to attribute them.

Two things this settles that Rosetta could not.  **Defect #26's fix holds on
real hardware**: batch 1801-1850, the threaded-FFI block that used to burn the
full 1800 s timeout on both arches, ran `rc=0` in 137 s.  **The fake-GUI
`KeyboardKeyTest` wedge is macOS-specific**: batch 651-700, which costs 20 of
51 classes on this Mac, ran `rc=0` in 126 s here.

**`popFrameForJIT` now widows the returning frame's context**, as `popFrame`
already did.  It is a real retention fix and it does NOT close defect #28 --
Context:start and Context:privateStart heap parents holding dropped TKTWorkers
go 71 -> 47 each (0 with the JIT off) and the test does not move.  Kept
because it makes the JIT match a documented invariant of the interpreter path;
validated neutral on batch 1-200, both arms, two reps each.

**`sunit-sweep.sh` now runs from the image directory by default.**  That is
the faithful configuration -- Pharo's own `CodeExporter` writes a file-out to
`FileLocator imageDirectory` while `TraitFileOutTest` reads it back by
relative name -- and it is worth 2 ERRORs a sweep.  Diffed first: batch 1-200,
both arms, two reps each, 3184 P / 0 F / 0 E every time.

## Right now (2026-09-03, 22:20)

**The real-x86 sweep is running again and past halfway.**  New box
`i-069e3881670b0cac0` (m6a.4xlarge, us-east-2), `RUN_ID=x86-gui-20260903`, so
it RESUMED from the S3 checkpoint the reaped box left at index 501 rather than
restarting.  A local heartbeat loop beats the lease every 5 min.  Batches are
landing at 60-870 s each; index ~1050 of 2047 at the time of writing.

**What the first 500 real-x86 classes said: 8166 tests, 8116 P, ZERO F, ZERO
E.**  49 SKIP.  So there is no x86-vs-arm codegen divergence anywhere in that
range on real hardware, which is the strongest form of the claim the Rosetta
sweep could only approximate.

**The x86 "shutdown wedge" is not a shutdown hang** (now defect #29).  The
`rc=137` batches are the sweep's own early kill, and every one of them wrote
all 50 class totals and `*** BATCH COMPLETED`, so the cost is wall-clock, not
coverage.  What the VM is doing in that 30-second window is *running*: ~8e9
bytecodes, the active process inside `OpalCompiler>>generateIR`, the JIT still
compiling against a full code zone.  `primitiveQuit` is never reached at all --
its unconditional "Deferred" line is absent and the process does not exit.  It
does not reproduce under Rosetta (marker, then clean exit 8 s later), which
separates the two variables that were confounded: real x86 Linux vs Rosetta,
and fake-GUI prep vs runner-only.  Also settled: batch 651-700, the one the
fake-GUI prep wedges on macOS, runs rc=0 in 126 s on real x86 Linux, so that
wedge is macOS-specific.

**Two of the three "ours" classes are not ours** (defect #28).  All three were
run against stock Cog x86_64 under Rosetta the same evening:

    class                ours (arm64)                    stock Cog x86_64
    StSpotterTest        1 F  Got 2 instead of 1          1 F  Got 2 instead of 1
    TraitFileOutTest     2 E  FileDoesNotExistException   4 P
    TKTWorkerTest        1-2 E  TKTTimeoutException       7 P

`StSpotterTest` is SHARED with Cog, same selector, same numbers.
`TraitFileOutTest` is a working-directory mismatch inside the image --
`CodeExporter` writes to `FileLocator imageDirectory` and the test reads a
bare relative name -- and our VM passes it 4/4 when run from the image
directory.  `RUN_FROM_IMAGE_DIR=1` is the faithful configuration, not a
workaround; a batch 1-100 A/B is running to confirm nothing else moves.

**`TKTWorkerTest` is ours and it is the JIT.**  Interleaved A/B, one class,
same binary, only `PHARO_NO_JIT` changed:

    rep   JIT on            JIT off
     1    5 P / 2 E         7 P / 0 E
     2    4 P / 2 E / 1 T   7 P / 0 E
     3    4 P / 2 E / 1 T   7 P / 0 E

The failures are the finalization tests: the worker is dropped, five full GCs
run, and the worker's process is still alive.  Ruled out by measurement --
finalization in general (a plain doit collects and stops on the first GC), any
VM root (`PHARO_WATCH_ROOT_CLASS=TKTWorker` reports zero visits all run while
the same knob on `Array` reports), a general collection failure (40
unreferenced workers leave 2, identically both ways), the deferred-drain
scheduling (`PHARO_INLINE_FINALIZE=1` changes nothing), and the
frame->context temp sync (`PHARO_NO_GC_TEMPSYNC=1` changes nothing).
`PHARO_NO_J2J=1` still fails.  `[GC-EPH]` says `fired=0` on almost every GC
with `active=375`, so the workers' ephemerons are not firing -- the key is
still being marked, and the marker is not any root category `forEachRoot`
walks.  That is the open question.

**`PHARO_NO_ASMJIT_T1=1` segfaults** (LEAD 24), deterministically, on
`(1 to: 200000) inject: 0 into: [:a :b | a + b]`.  Two `tryExecute` frames at
the same offset.  It produced no results file at all during the bisect above,
which reads exactly like a hang; do not mistake it for one.

**`preserve.sh` no longer pushes to a build branch.**  The autosave goes to
`autosave/<instance-id>` (IMDSv2, hostname fallback).  That closes the
deferred hazard where a box holding a stale tree could fast-forward a revert
onto `jit` with no force flag -- the shape `git diff --name-status e4c47863
ede0fd65` already shows.

## Right now (2026-09-03, 13:45)

**The real-x86 sweep is running on AWS** -- `i-021aa136e1c7cc9b8`, m5.4xlarge
spot, tonight's tree built on the box (`CANNOT-RETURN-STORM` verified in the
binary), fake-GUI-prepped image, batched with per-batch S3 checkpointing.  It
RESUMED at index 51 from the S3 state the first (reaped) box left behind, so
`START_AT` earns its keep on its first real use.

The lease is fixed: `awohl.com`'s `authorized_keys` had no `aws-lease` entry,
so the first box was reaped at 18 minutes.  Appended (backup at
`~/.ssh/authorized_keys.bak-claude-*`), `lease.sh list` answers, and a watcher
beats it every 5 min while the sweep runs.

**First real-hardware finding, and it corrects me.**  Batch 51-100 came back
`rc=137` on native x86 Linux -- the shutdown wedge, where the batch finishes
all 51 classes and the VM then refuses to exit.  I had been treating that as a
Rosetta artifact.  It is not: it happens on real hardware, and the arm64 sweep
had zero of them in 41 batches.  The Rosetta run continues locally as a control
so the two can be diffed.


**The shutdown wedge is an x86 phenomenon, and the early kill is earning its
keep.**  In the running x86 sweep, 4 of the first 13 batches wrote their
completion marker and then sat wedged with the VM refusing to exit; `run_batch`
killed each after the 30 s grace instead of burning the 1800 s timeout, saving
roughly 1700 s apiece.  The arm64 fake-GUI sweep had ZERO such batches in 41.
That asymmetry matches defect #26's own numbers -- the deferred-quit trace
appeared 61 times on x86 against 5 on arm64 -- so the guard fixed the unbounded
case and this is the bounded remainder.

**x86_64 sweep on the fake-GUI image is running locally** (Rosetta,
`build-x86/test_load_image` in place so it keeps its ~200 staged dylibs).  That
is the both-arches comparison on the same footing as the arm64 fake-GUI run.

**The AWS x86 run is blocked, and not by spot.**  A spot box did launch
(m5.4xlarge, after m6i and m6a had no capacity in any AZ) and was terminated 18
minutes in with `StateTransitionReason: User initiated` -- the `aws_watch`
reaper, not a reclaim, because `provision.sh` could not register the keep-alive
lease: awohl.com's `authorized_keys` has lost its `aws-lease` entry.  An
unregistered box is reaped whatever its lifecycle, so no multi-hour run can
survive until that key is back.  Details and the spot-capacity findings are in
`scripts/aws/config.env`.

Built for that run and worth keeping regardless: `START_AT` resume in
`sunit-sweep.sh`, and `scripts/aws/x86-gui-sweep.sh` (batched, per-batch S3
checkpointing, resumes on the same RUN_ID) -- so when the lease is fixed, a
reclaim costs one batch.


**Best sweep to date, and the residual is now mostly not ours.**

    run                        classes   tests    PASS   FAIL  ERROR  T    rate
    fake-GUI prep                2047    28060   27857    11     4    6   99.28%
    guard only                   2043    28043   27812    18    28    3   99.18%
    2026-09-02 (storm aborted)   2005    27682   27453    21    21    5   99.17%

All 2047 classes ran.  ERROR 28 -> 4, FAIL 18 -> 11.  Fourteen classes are
non-clean (counting TIMEOUTs), and against the Cog baseline taken the same
morning: **four at exact parity with Cog, one BETTER than Cog**
(`StDebuggerTest`, 2 F to Cog's 3), three partly shared, three slow-but-correct,
and **three ours** -- `TraitFileOutTest` (a working-directory artifact whose own
error text names the cause), `TKTWorkerTest` (flaky worker timing) and
`StSpotterTest` (one real failure).

The whole display family is gone: all 178 `Sp*` classes clean, and the ten
previously non-clean `Sp*`/`FTTableMorph` classes clean.  16 E and 7 F removed
by the prep alone.

`DUP-FRAME` is **0** on the same workload that produced 126 before -- the
corrected detector agreeing that all 126 were the region-rebuild false
positive.

Costs and caveats measured, not assumed: the fake-GUI prep wedges batch 651-700
on `KeyboardKeyTest>>testEqual` (recovery pass gets all 31 classes back, so
wall-clock not coverage), and this run's four `MISDISPATCH` lines are the other
detector's `CleanBlockClosure` false positives, fixed after the sweep started.

## Right now (2026-09-03, 07:30)

**All three tiers are green on both arches, and the sweeps agree.**

    tier         arm64                                   x86_64
    VM C++       4/4 + test_asmjit_t1_stub               same
    SUnit        2043 cls  27812 P / 18 F / 28 E / 3 T   2042 cls  27810 P / 20 F / 24 E / 8 T
    packages     354 cls   9383 P / 16 F / 17 E / 0 T    354 cls   9376 P / 16 F / 24 E / 0 T

Both sweeps ran 41 batches at rc=0 with no abort.  **No codegen divergence
between the arches**: fifteen classes differ out of 2043 and every one is a
Rosetta timeout, a wall-clock assertion, a `w64Convention` symbol the test
dylib does not export, a flake, or my own copied-VM dylib mistake -- which
x86_64 confirms by passing `LibTTYTest` 5/5 after running its binary in place.
The package tier now agrees exactly on FAIL (14 DataFrame + 2 PolyMath).

Defect #23 is bounded and its guard is load-bearing: it fired once in the arm64
sweep, in `batch_1901`, the batch that used to abort and cost 40 classes.  Both
candidate root fixes were measured and neither closes the hole -- the numbers
are recorded at the knobs so nobody re-runs them.

**The `Total:`-splice fix is verified.**  A freshly prepped image
(`work/sunit2.image`, submodule `db42c37`) runs `RSViolinPlotTest` and
`TKTCommonQueueWorkerPoolTest` -- the two classes every sweep on the old image
loses -- and both emit a clean `Total:` on their own line, including the one
that TIMED OUT, which is the case that used to splice.  Use `sunit2.image` for
the next sweep: it is worth 2 classes and 20 tests.

**Two of the three open items closed themselves under measurement.**  The four
x86-only `XMLParser` errors are `TestTookTooMuchTime` -- SUnit's own per-test
time limit, recorded as an ERROR -- so the package tier's whole x86-vs-arm
ERROR gap is Rosetta speed, not codegen.  And the three arm64 TIMEOUTs are two
slow whole-image scans (one passes 3 of 3 given time; the other is capped by
`SUnitMaxPerTestSeconds`, not by the multiplier) plus one test whose remote
resource is dead (`ZipArchiveError: can't find EOCD position` from
Smalltalkhub).  None is a correctness defect.

That leaves ONE open correctness question in the whole residual -- defect #27,
the ByteSymbol mis-dispatch, at one test per sweep -- plus a performance
question about how slow those image scans are against Cog.

**The fake-GUI prep costs a wedged batch, and that is now its own lead.**  The
confirming sweep lost batch 651-700 entirely (rc=124, 20 of 51 classes) to
`KeyboardKeyTest>>testEqual`.  The in-image watchdog could not interrupt it,
which means no Smalltalk process was running at all -- the VM itself was
blocked, presumably on an event queue the fake GUI creates and nothing pumps.
A probe reproduces it alone on both images and takes a `sample` of the C++
stack, which is what names the blocking call.  The recovery pass gets the
classes back, so the price is wall-clock; `CLAUDE.md`'s recipe now says so.

**The untried lever works, and it is worth most of the remaining residual.**
Prepping with `setup_fake_gui.st` before the runner, measured on the 15
affected classes with everything else identical:

    prep                          classes   PASS   FAIL  ERROR  SKIP
    runner only                     15       209    13     16     7
    setup_fake_gui.st + runner      15       232     6      0     7

Every ERROR goes away and the FAILs more than halve.  `CLAUDE.md` has recorded
this for months and no sweep image this project measured was prepped with it.
A full sweep on a fake-GUI-prepped image is running -- the same image also
carries the `Total:`-splice fix, and the VM carries the new defect-#27
detector.  Expected residual if it holds at scale: roughly 11 F / 7 E over
2045 classes, against today's 18 F / 28 E over 2043.

Three open items with work queued: **defect #27** (a ByteSymbol receiver running
`BlockClosure>>value:` -- once per sweep, so being chased with
`PHARO_T1_VALIDATE_IC` instrumentation rather than a repro), and the five
x86-only `XMLParser` attribute-default errors.


**The arm64 sweep with the storm guard is done and it is the result the goal
wanted: 2043 classes, 28043 tests, 27812 P / 18 F / 28 E / 3 T, 99.18%, 41
batches all rc=0, no abort anywhere.**  The 2026-09-02 run was missing 40
classes to the Context storm; this one has them.  The guard fired exactly once,
in `batch_1901` -- the batch that used to abort -- so the storm costs one
terminated process now.  The trait block is clean on arm64 for the first time,
`TraitTest` 54/54.  `RSLinesTest` 18/18 confirms defect #22's fix at scale.

Two residual items in that run are the harness, not the VM: `LibTTYTest`'s 5
errors are a copied VM missing `libtty.dylib` (fixed for later runs), and
`RSViolinPlotTest` + `TKTCommonQueueWorkerPoolTest` are still lost to the
`Total:` splice because **this image was prepped before that submodule fix**.
Re-prepping recovers both -- that is now the highest-value cheap task.


**Defect #23 is bounded, and the bound is the whole fix that survives.**  With
fix 1 reverted, the build carrying fix 2 and the `cannotReturn:` storm guard
takes the storm batch from 7 aborts of 10 to **0 of 10**, with the guard firing
in 7 of those runs -- every storm becomes one terminated process and the batch
completes -- and it scores 0 non-clean classes on batch 1-100, twice, matching
the unfixed VM.  That is the configuration now running a full arm64 sweep.

What is still open is the hole itself: a closure created while inline-J2J saves
are pending captures an `outerContext` missing every J2J-hidden caller, 481
times in a 20-second run.  A candidate fix is written and inert
(`PHARO_T1_J2J_EXCLUDE_BLOCK_CREATE`, both arches): refuse inline-J2J for a
callee whose body contains `PushFullBlock`/`PushClosure`, which is what the BV
inline prep already does one layer down.  Its validation is queued.

**The duplicate activation has a mechanism.**  The detector fired on its first
sweep -- 27 hits, every one a J2J save for `#methodNode` landing on a frame
pointer another frame already holds -- and the code says why: the `ExitJ2JCall`
handler materializes the whole pending slice, syncs the interpreter, and
`continue`s back into the chain loop **without clearing `state.j2jDepth` or
re-basing the save cursor**.  The `materializeJ2J` lambda does both, and its
comment names leaving the marker set as the cause of the 2026-06-09
xmethod-at-scale corruption.  That site never got the same treatment, so the
next materialize walks the same saves again.

Second candidate fix, also inert: `PHARO_J2J_SITE5_CLEAR_DEPTH`.  Off by
default because the depth is read in two senses in that loop -- "how many live
saves" and "were we in a J2J chain" -- and another site uses the second sense
AFTER materializing.

Queued, in order, running unattended:

1. full arm64 sweep, fix 2 + guard (started 03:21)
2. one binary, four configurations -- defaults, block-create gate, site5
   depth-clear, both -- each judged on batch 1-100 (must stay at 0 non-clean,
   which is exactly where fix 1 died) AND on the storm batch, where the tell is
   **the guard firing**, not the abort: aborts are already 0, and the guard
   fires in 7 of 10 runs, so a candidate that closes the hole should stop it
   firing.  DUP-FRAME hits are counted per configuration too.
3. full x86_64 sweep, shipping defaults
4. package tier on both arches with the guard build, reusing the loaded package
   images.  Baselines to beat: arm64 9326 P / 16 F / 16 E / 1 T, x86_64
   9376 / 16 / 24 / 0.

**Two confirmations from the sweep in flight**, against the 2026-09-02
baseline via the new `scripts/sweep-diff.sh`:

    RSLinesTest       16 P / 2 E  ->  18 P / 0 E   defect #22's fix holds at
                                                   full-sweep scale
    LinkInstallerTest 39 P / 1 F  ->  40 P / 0 F
    ReleaseTest       39 P / 2 T  ->  40 P / 1 T
    RSRoassalTest      6 P        ->   5 P / 1 E   defect #27 (new, filed)
    LibTTYTest         5 P        ->   5 E         harness, the copied VM

**Queued after the current chain:** the timeout residue.  `NoUnusedVariables
LeftTest>>testNoUnusedTemporaryVariablesLeft`, `MCSmalltalkhubRepositoryTest>>
testVersionFromFileNamed` and two `ReleaseTest` cases exceed the 80 s per-test
bound on arm64 and pass on Cog.  A TIMEOUT counts as a failure for the goal,
and these are the only ones left that are plainly about speed rather than
semantics -- worth measuring against Cog before assuming they need a JIT fix.

**C++ tier, both arches:** `test_sista_ir` PASS, `test_class_table` rc=0,
`test_sista_survey` rc=0, `test_asmjit_t1_stub` PASS (1000 reentrant calls all
writing EXIT_SEND).  `test_platform` and `test_ios_bridge` are display smoke
tools, not pass/fail tier members -- on the pristine image in 5 s
`test_platform` reports "No display updates received / TEST INCOMPLETE", which
is what a run with no Morphic world looks like, and `test_ios_bridge` runs long
enough to need its own timeout.  Verifying those two means the screenshot route
in CLAUDE.md, not a return code.

**A copied VM loses its dylibs.**  The arm64 sweep runs a COPY of the binary
(so a rebuild cannot change it mid-run), and the copy left `libtty.dylib` and
`libTestLibrary.dylib` behind: `LibTTYTest` went 5 P to 5 E with
`SymbolNotFoundError: Could not find symbol named: #tty_spawn searching in
module: 'libtty.dylib'`.  Same shape as the x86-VM-loses-libgit2 note in
memory.  Both dylibs are now staged beside the copies, and the x86_64 sweep
runs `./build-x86/test_load_image` in place rather than a copy -- that tree
stages about 200 dylibs.

**Standing rule from tonight:** run `scripts/sunit-sweep.sh` on batch `1 100`
before believing any VM change -- about four minutes, and the unfixed binary
scores 0 non-clean, so any non-zero is the change.  A five-class repro batch
only ever says whether the bug still fires; three interleaved A/Bs on one
called a fix clean that broke 11 classes.


**Fix 1 is reverted; it broke 11 classes that the storm's repro batch never
runs.**  The full sweep found it within four batches: batch 1-100 goes from 0
non-clean on the unfixed VM to 11 with fix 1, with the shifted-operand-stack
signature (`NonBooleanReceiver: proceed for truth`, `MessageNotUnderstood` on
`SmallInteger >> #byteSize`).  Materializing pending J2J saves at
`ExitBlockCreate` is simply not valid there -- bailing to the interpreter
instead of resuming the JIT scores the same 11.

Which means **every "the fix works" number from tonight's storm A/Bs is
suspect**: all of them had fix 1 in the binary, and a change that breaks 11
classes can suppress a storm by breaking the code that reaches it.  What is
being measured now, in order: batch 1-100 must be clean on the reverted build,
then the storm A/B against base, then the full sweep.  Nothing is claimed for
the storm until that runs.

Still in: fix 2 (the materialized-context handover -- the probe clears it of
any part in the regression) and the `cannotReturn:` storm guard.  Still open,
and now the central question: how to give a closure built inside a J2J chain a
complete `outerContext` WITHOUT consuming saves the JIT still owns.  It happens
481 times in a 20-second run.

**Standing rule from tonight:** run `scripts/sunit-sweep.sh` on batch `1 100`
before believing any VM change -- about four minutes, and the unfixed binary
scores 0 non-clean, so any non-zero is the change.  A five-class repro batch
only ever says whether the bug still fires.


**Defect #23 is bounded and mostly fixed; the loop still starts sometimes.**
Two fixes and a bound are in, and across three interleaved A/Bs under held load
the unfixed binary storms 24 times in 32 reps against the fixed build's 3 (all
three on the fix-1-only build).  What is NOT fixed: the underlying dead-sender
return loop still starts -- roughly a third of runs trip the
`cannotReturn:` storm guard at its default burst of 64 -- and the guard
terminates that one process while the batch completes normally.  So the cost
went from a 12 GB abort that takes 39 classes with it to one terminated
process.

Sweeps on both arches are queued behind a marginal A/B (fix 1 alone against
fix 1 + fix 2, 16 reps each, counting guard fires as well as aborts, because
fix 2's marginal value is not yet established).


**Defect #23 (the arm64 Context storm) is root-caused down to its loop and its
emit, and one candidate fix is in and NOT yet vindicated.**  In order:

- The storm is the JIT's: `PHARO_NO_JIT=1` storms 0 of 8 where the shipped
  build storms 6 of 8, same script, same conditions.
- The loop is `Context>>cannotReturn:` -> `error:` -> `signal:` -> `signal`,
  and `signal` scans a sender chain with no `on:do:` in it.  Captured by the
  new `Interpreter::dumpSendChain`, which runs at the old-space-exhaustion
  FATAL and prints both the native frames and the materialized context chain
  with primitive 199/198 frames marked.
- `PHARO_T1_NO_INLINE_J2J=1` storms 0 of 6 against BASE's 4 of 6 in the same
  script, and its clean runs take the same 3-4 s, so it is not buying the
  result with slowness.
- A real unsoundness was found on the default path: the chain loop's
  `ExitBlockCreate` handler called `createFullBlockWithLiteral` with J2J saves
  still pending, and `enableJ2J()` below it then DROPPED them.  Fixed
  (`7724f3fe`) by materializing first.
- **The storm guard's path is exercised and works.**  It cannot fire in a
  healthy run, so its burst limit is a knob (`PHARO_CANNOT_RETURN_BURST`,
  default 64); at `burst=2` the guard fires, terminates the storming process,
  and the batch completes.
- **The dead sender was NOT killed by either unwind.**  From that same
  validation run: `#runSingleTest:selector:timeout:priority:on: returning into
  a dead sender (#ifTrue:ifFalse:, pc=nil); unwind kills so far: nlr=0
  aboutToReturn=0`.  Both unwind counters are ZERO and the marker is `pc=nil`,
  not the `-1` sentinel an unwind writes -- so the sender died the ordinary
  way, by RETURNING.  The caller returned before its callee did, and the callee
  then returned into the corpse.  That is a duplicate or stale activation,
  which is what a J2J save popped twice (or an activation materialized twice)
  would look like.  Next instrument: whether the returning frame carries a
  `materializedRetSlot`, which puts it on the J2J materialize path directly.
- **The A/B says that fix is most of the storm.**  Interleaved, 4 CPU hogs
  holding the load steady, matched binaries: **base 9 storms of 12, fixed 3 of
  12**, and a second A/B against the fix-plus-guard build gave **base 7 of 10,
  fixed 0 of 10** -- 16 of 22 against 3 of 22 combined.  (An earlier note here
  called the fix ineffective off the first 5 reps -- base 3, fixed 2 -- which
  was reading a coin-flip's worth of data.)  So the pending-saves block-create
  is the majority path and something else accounts for the residual.

Next instrument, already built and queued behind the A/B: six per-site
counters on the `cannotReturn:` sends, printed in the fatal dump.  Only two of
the six sites arm the per-process count/deadline guard, which is why a loop
through the other four never self-limits; the tally says which one the storm
runs through.

Two measurement rules learned tonight, both the hard way:

- **The storm rate tracks machine load.**  BASE was 6 of 8 with the package
  tier running, 4 of 6 later, and 1 of 6 idle.  A blocked A-then-B bisect is
  therefore unreadable; interleave the arms and hold the load.
- **Two SUnit runner jobs share `/tmp/sunit_*.txt`.**  A second repro loop
  started next to a running one hijacks it.  Chain them, do not overlap them.

## Right now (2026-09-02)

Fresh clone on the Mac (only the Command Line Tools installed, no Xcode).
Infrastructure rebuilt from nothing: `scripts/build-macos-slices.sh` makes the
two `macos-arm64_x86_64` slices the CMake trees need (the full xcframework
scripts cannot run without iOS SDKs); `build-rel` (arm64) and `build-x86`
(x86_64) configured and built with the JIT on; x86_64 cairo/libgit2 bottles
staged beside `build-x86/test_load_image`; `libTestLibrary.dylib` built for
both arches; a fresh Pharo 13.1 image (`4f7563d`) prepped with the runner via
explicit snapshot (54 -> 79 MB, `SUnitRunner` present).

The `scripts/pharo-headless-test` submodule pointer (`749ce14`) was never
pushed; a fresh clone cannot fetch it.  Re-created as `ed885c8` from the
write-up below (same change: the selector skip list is empty).  Still local
until the submodule is pushed.

**First defect found by the C++ tier: a heap-use-after-free in the shared
lifter** (`SistaBuilder.cpp` Pass 5) -- `test_sista_ir` rc=139 on x86_64,
passing by luck on arm64.  Fixed and ASan-verified on both arches; see
`docs/changes.md` 2026-09-02.

**The stock Cog baseline is back** (2026-09-02).  The `codeZone` abort that
made this host "no Δcog possible" is arch-specific: the **x86_64** Cog runs
fine under Rosetta.  Installed at `/tmp/harness-x86`, v10.3.9, `eval` and
`eval --save` both verified; every command needs the `arch -x86_64` prefix.
Three things fell out of it the same evening, none of which touched our VM or
the running sweep:

  * **The trait-test storm is ours** — Cog runs all 27 `Trait*Test` classes in
    seconds, 270 tests, 0 F / 0 E, where our batch exhausted 12 GB.  Filed as
    defect #23.
  * **Defect #22 (`RSLinesTest` `BlockCannotReturn`) is root-caused** — Cog
    passes 18/18, and the image says `RSAbstractLine`'s copy of the trait
    method carries a block whose `outerCode` is still
    `RSTMarkeable>>#markersIncludesPoint:`.  677 installed methods carry that
    shape with a non-local return in the block.
  * **Fourteen of the sixteen "no display" classes are root-caused, and the
    defect is the render loop** (#24, root #25).  That is 9 of the sweep's 21
    FAILs and 17 of its 21 ERRORs; three more FAILs are parity with Cog.  That
    leaves three genuinely ours, and the x86 sweep narrows even those:
    `StDebuggerActionModelTest` is the ONLY class in 1800 covered classes that
    is non-clean on arm and clean on x86, so its one failure is timing —
    leaving `StSpotterModelTest`'s two — and reading those closes them out too:
    one carries `skipOnPharoCITestingEnvironment` (Pharo's CI does not run it,
    ours does), and the other asserts a forked search has not started for
    5 x 50 ms and has started 300 ms later, i.e. a 250-550 ms scheduling
    window.  **Every non-pass in the arm64 residual is now attributed, and none
    of the GUI ones is a VM computation error.**  On stock Cog, installing `MorphicUIManager` and suspending
    its UI process reproduces our result for `SpListCommonPropertiestTest`
    exactly — 18 P / 5 E, same five selectors, same messages; leave the process
    running and Cog is 23 / 23.  Our runner suspends it as its first startup
    action because on our VM the headless resume restarts `MorphicRenderLoop`,
    which busy-spins with Morphic DNUs until the Delay scheduler dies.  Fix the
    DNUs and nine classes of residual go with them.  Two earlier explanations
    (the missing display; a `ParametrizedTestCase` parameterisation bug) were
    filed and refuted the same evening and are recorded under #24.
  * **A separate, real coverage gap: 10,567 test cases** — 241 of the 2047
    concrete test classes are parameterised, their selectors total 2074 and
    their suites 12641, so the reported test count understates the suite by
    ~38%.  This is a denominator fact, not a failure cause.

  * **The threaded-FFI batch never exits** (#26).  Batch 1801-1850 writes all
    its results and `=== BATCH COMPLETE ===`, then hangs until the harness
    kills it at 1800 s — on both arches.  Stock Cog runs the whole block
    (TF/TFUFFI + Step* + TCPSocket*, 429 tests) in **7.4 s**, 0 F / 0 E.  Only
    two statements run after `BATCH COMPLETE`, `[Smalltalk exitSuccess] on:
    Error do:` and `Smalltalk quitPrimitive`; stderr landmarks were added
    around both so the next sweep says which one hangs.  This is 30 minutes of
    every sweep on every arch.

Full Δcog for the residual in
`docs/results/sweep-arm-2026-09-02/cog-residual-baseline.txt`.

Also cleaned up: the runner sent `#suspend` to **nil once per test**
(`backupWatchdog`, assigned nil right after the watchdog fork and never set
again, with its own temp declared "unused").  Each one costs a 17-line `[DNU]`
frame dump plus `[DNU-RCVR]` and `[SISTA-RING]` traces — together the single
largest source of stderr in a sweep, and the same trace a past session misread
as forwarded-object MNUs.  Guarded with `ifNotNil:`, along with the three other
suspend/terminate sites.

**The x86_64 backend is not producing wrong answers.**  Comparing the two
sweeps class by class while the x86 one was still running (both at **99.17%**),
every class non-clean on x86_64 and clean on arm64 was a timeout or a
wall-clock assertion, not a codegen difference:

    IntegerTest                     1 TIMEOUT (80 s)
    STONWritePrettyPrinterReadTest  1 TIMEOUT
    SpTreePresenterExpandTest       1 TIMEOUT
    RSSequentialAnimationTest       Error: Time up
    SpJobListPresenterTest          Got 68 instead of 100

Rosetta runs that build at roughly half arm64's speed against a fixed 80 s
per-test bound — the same 2x rule `scripts/package-tests-selfhosted.sh` already
documents for the package tier.  The runner now reads `SUNIT_TIMEOUT_MULT`
(submodule `db42c37`), so the next x86 sweep should use `SUNIT_TIMEOUT_MULT=2`.

**Verified overnight**, from the chained post-sweep scripts:

  * `test_relaunch` **3/3 PASS on `base.image`** — the earlier 2/3 was the
    prepped image, as diagnosed; not a regression and not machine load.
  * The **low-space breaker fires**: 462 `[LOW-SPACE]` lines and **zero** FATAL
    aborts on the storm repro at `PHARO_MAX_OLD_SPACE_MB=512`, with the P60
    watcher preempting the P40 hog.  In a bare `eval`, which the 2026-07
    dossier said was impossible.
  * The **x86_64 sweep finished at 99.15%** on 2046 classes — see above.
  * The **storm does not reproduce in chunks**: all eight covering 1912-1950
    ran clean in 135 s total.  It needs one image; the bisect must be by class
    count, not by fresh chunks.
  * Batch 1001-1050, which I damaged mid-sweep, recovered clean in ten chunks.

### Defects filed tonight

    #22  BlockCannotReturn on a trait-copied block's non-local return
         FIXED + VERIFIED — RSLinesTest 18/18 with the JIT and without, from
         16 P / 2 E (arm) and 17 P / 1 E (x86).  The class's copy of a trait
         method shares the trait's CompiledBlock, whose outerCode names only
         the trait's method, so the home match by method oop could not succeed.
    #23  The Context storm — ours, and NOT the trait tests.  Reproduces in
         **18 s** at PHARO_MAX_OLD_SPACE_MB=1024, dying at index 1909
         TonelWriterV1Test; 1912 was only where a 12 GB heap ran out.  The 27
         trait classes are clean in isolation on our VM (0 traitUsers
         violations, TraitTest 54/54, no storm).  Bisect forward from 1901
         through the Tonel block, 18 s a run.  The bisect came back
         NON-DETERMINISTIC — 1901-1905 stormed while both its supersets ran
         clean.  **Best repro so far**: `PHARO_DET_SCHED=1
         PHARO_DET_SCHED_QUANTUM=64 PHARO_MAX_OLD_SPACE_MB=1024` on batch
         1901-1905 — 17 s, stormed 2 of 2 with the SAME Context count both
         times.  Wall clock is 1 of 3, quantum 1 and 4 never fire, quantum 16
         is 3 of 5.  Confirm 64 with six reps on an idle machine before relying
         on it: DET_SCHED removes the heartbeat but not `Delay`, so load still
         leaks in, and quantum 16 is 5 of 8 overall rather than the 2 of 2 it
         first looked like.  **Every storming run dies inside
         `TonelReaderTest`** (index 1904), occasionally one class later — that
         is the target.
    #24  Fourteen classes fail because the runner must suspend the UI process.
         REPRODUCED on Cog; two earlier explanations refuted and recorded.
         Our PRISTINE headless environment is identical to Cog's, so the
         difference is the prepped image's live Morphic world.
    #25  The resumed MorphicRenderLoop busy-spins on Morphic DNUs and kills
         the Delay scheduler — the root behind #24
    #26  The threaded-FFI batch never exits.  FIXED + VERIFIED —
         1800 s -> 54 s, rc=0, 51 of 51 classes.  ~29 minutes per sweep per
         arch.
    LEAD 19  The low-space breaker could not fire.  FIXED + VERIFIED —
         462 firings and zero FATAL aborts in a bare eval, and it fires under
         the runner too, once, at exactly the computed threshold.  The
         threshold=0 in an exhaustion FATAL is the one-shot disarm on
         delivery, not "never armed" — I read that wrong twice, and the FATAL
         now prints the delivery count.  What is left belongs to #23: the
         image cannot re-arm because the Delay scheduler is already dead.

The C++ tier is green on both arches with all of tonight's commits:
`test_sista_ir`, `test_class_table`, `test_sista_survey` and `test_relaunch`
3/3 on arm64; `test_sista_ir` and `test_class_table` on x86_64.

**The arm64 SUnit sweep is done** (`STEP=50 PER_BATCH_TIMEOUT=1800`,
`PHARO_CODE_ZONE_MB=192 PHARO_MAX_STEPS=4000000000000
PHARO_MAX_OLD_SPACE_MB=12288`): 2007 classes, 27692 tests, 27461 P / 21 F /
21 E / 7 T / 182 S, 99.17%.  Full write-up and the per-class residual in
`docs/results/sweep-arm-2026-09-02.md`, artifacts in
`docs/results/sweep-arm-2026-09-02/`.  Two things came out of it that are not
pass-rate numbers:

  * **A write splice in the runner dropped two classes from every
    aggregation** -- the P60 watchdog published `testDone` before printing its
    verdict, so the P80 main wrote the class `Total:` line into the middle of
    it and then suspended the watchdog mid-line.  Fixed in the submodule
    (compose off-stream, publish `testDone` after the flush).  NOT yet
    verified by a run, because a targeted run would collide with the x86_64
    sweep's `/tmp` state.
  * **Batch 1901-1950 died of a Context storm** (`rc=134`, 33M Contexts / 6.6M
    Errors / 12.3 GB) and took 39 classes with it.  Same signature as the
    2026-08-22 batch 601-650 crash and the open "Monticello runaway", but this
    time the missing range is identified without re-running anything: it is
    the **trait tests**.  The batch died at index 1912, batch 1951 starts at
    `UndefinedPackageTest`, and the classes between those two names in this
    image are 27 `Trait*Test` plus `TrueTest`, `Tutorial*`, `UDPSocket*`,
    `UUID*` and `Undefined*` — 41 candidates against the 39-40 that never ran.
    `TraitTest` ("Cog 54 P / 0 E, ours errors, not yet drilled", carried since
    2026-06-01) is one of them, and today's `OCClassBuilderTest` newcomer also
    fails on a trait composition.

The **x86_64 sweep is in flight** with the same settings, output under the
session scratchpad `sweep-x86/`.  **Its batch 1001-1050 is damaged and must be
re-run**: `test_relaunch` was run on the PREPPED image while the sweep was
going, and a prepped image auto-starts `SUnitRunner` on resume, so it wrote
`/tmp/sunit_test_results.txt` underneath the batch, which then reported
`classes=1` instead of 51.  The recovery is automated in the post-sweep script.
The rule this broke is wider than the one written down: it is not "do not run
`test_load_image`", it is "do not start ANY VM binary on the prepped image".

### Queued behind the x86_64 sweep — do these in order when it finishes

Everything here is blocked for one of two reasons: every SUnit runner
invocation reads and writes the same fixed `/tmp/sunit_*` paths (a targeted run
hijacks the sweep's next batch; `test_load_image ... eval` touches
`/tmp/sunit_run_completed.txt`, which makes `SUnitRunner>>startUp:` skip a batch
entirely; and ANY binary started on the PREPPED image does the same, which is
how batch 1001-1050 was lost tonight), or the sweep is running the binary that
would be replaced.

Steps 0-4 are automated in the session scratchpad, chained on each other
(`post-sweep-running.sh` -> `-2-` -> `-3-` -> `-4-` -> `-5-`, each waiting on
the previous one's "done" line):
`post-sweep-running.sh` (waits on the sweep's pid; recovers the damaged x86
batch, `test_relaunch` on `base.image`, the low-space control, the storm hunt
in chunks of 5, rebuild x86), then `post-sweep-2-running.sh` (rebuild both with
tonight's commits, C++ tier on both arches, the #24/#25 environment probe,
`RSLinesTest` with and without the JIT, the defect-#22 block probe), then
`post-sweep-3-running.sh` (re-run batch 1801-1850 and check it no longer idles
1730 s).  The rest are by hand.

 0. **Neither build matches the tree.**  Everything from `77f12ac6` on is
    syntax-checked only — the `!inExtension_` guard, the bounded trace, the
    headroom cap, the FATAL diagnostic, and the three dead-code removals.
    Rebuild BOTH arches and re-run the C++ tier before trusting any number
    from either.  `build-x86` is further behind: it has none of the day's
    changes, starting at `2c2c4616`.
    Two edits to `scripts/sunit-sweep.sh`, both deliberately deferred because
    editing a running bash script shifts the byte offsets it resumes reading
    from:
      * the "no VM binary on the prepped image" note in its header;
      * **kill a batch once it has finished.**  The VM writes
        `/tmp/sunit_run_completed.txt` when the batch is done; the driver
        currently waits for the process to exit and pays
        `PER_BATCH_TIMEOUT` in full when it does not.  Polling for that marker
        and killing caps the damage from ANY shutdown hang, not just defect
        #26's — and would have saved 30 minutes on each of tonight's two
        sweeps even before that fix existed.
 1. **Name the class that storms.**  Re-run arm batch 1901-1950:
    `printf '1901 1950' > /tmp/sunit_batch.txt` and launch the prepped image.
    The x86 sweep will also pass through that range and may name it for free.
    Index 1911 is `TonelWriterV3Test`; 1951 is `UndefinedPackageTest`; the
    classes in between are the trait tests (see the sweep report).  Start with
    the `Trait*` block, not with a blind batch re-run.

    **Run `scripts/pharo-headless-test/storm_repro_husk_freeze.st` first** — it
    is a one-command answer to the most likely hypothesis.
    `docs/history/arm-context-storm-2026-07.md` identifies the storm's trigger
    as a **trait-class rebuild with live instances leaving a stale husk** that a
    `freeze` handler re-hits, eliminated by `62417f43`
    (becomeForward-leaves-forwarder) and `296bba26`
    (classOf-follows-forwarders).  Both are still default-on.  The repro must
    answer `NO-HUSK`; anything else says the trigger fix has regressed and
    explains the trait batch dying.  If it does answer `NO-HUSK`, there is a
    second trigger and the per-class hunt is the way in.

    **Hunt with a small heap.**  The storm took 758 s to reach the FATAL at
    `PHARO_MAX_OLD_SPACE_MB=12288`; at 1024 it should abort in roughly a
    twelfth of that, which turns a bisection over 39 classes from an evening
    into minutes.  The census shape is the same either way — it is the ratios
    (1.00 closures and 5.01 Contexts per Error) that identify it, not the
    absolute counts.

    Two halves of that fix were re-checked by reading, 2026-09-02, so do not
    re-derive them: `collectInstancesOfClass` still skips forwarded objects
    (`ObjectMemory.cpp:3386`, `!obj->isForwarded()`), and both
    `becomeForward`-leaves-forwarder and classOf-follows-forwarders are still
    default-on with opt-out knobs only (`ObjectMemory.cpp:1803`, `:1895`,
    `:558`).  So "the 2026-07 trigger fix silently reverted" is already
    partly refuted; a second trigger is the likelier reading.
 2. ~~**Verify the low-space breaker actually fires**~~ — DONE 2026-09-03:
    462 `[LOW-SPACE]` lines and zero FATAL aborts on
    `storm_repro_freeze_recursion.st` at `PHARO_MAX_OLD_SPACE_MB=512`, with the
    P60 watcher preempting the P40 hog.  And it worked in a bare `eval`, which
    the 2026-07 dossier said was impossible.  Original note kept below.

 2b. (superseded) **Verify the low-space breaker actually fires — NOT with a bare `eval`.**
    The "before" evidence is on record (12 GB consumed, zero `[LOW-SPACE]`
    lines).  The obvious "after" test is
    `PHARO_MAX_OLD_SPACE_MB=512 ./build-rel/test_load_image <image> eval
    "<storm_repro_freeze_recursion.st>"`, and it is **expected to print
    nothing**: `docs/history/arm-context-storm-2026-07.md` records that the
    mitigation is disarmed in bare eval mode, because the image never runs
    `installLowSpaceWatcher` there, so prim 125 is never armed and neither the
    old sampled check nor the new latch can arm either.  The post-sweep script
    runs it anyway as a control; a silent result there is NOT evidence against
    the fix.

    The real test is the runner path, which does install the watcher (the
    2026-09-02 batch log shows the VM inside `#lowSpaceWatcher` at step 161M).
    So verification and investigation are the same run: re-run the trait block
    on the rebuilt binary.  If the storm recurs, `[LOW-SPACE]` lines must
    appear — plural, because the image re-arms after each one and its action
    (`signalException: OutOfMemory`) is an `Error` the hog's handler can
    swallow.
 3. **Verify the runner watchdog fix.**  Re-prep an image with the current submodule
    (`49e35b8`, which also carries SUNIT_TIMEOUT_MULT, the shutdown landmarks and
    the nil-suspend cleanup) — both changes compile (filed into a pristine image under stock
    Cog) but neither has been run.

      a. The watchdog splice: run a class with a known timeout
         (`MCSmalltalkhubRepositoryTest`); its `Total:` line must be at line
         start.
      b. (The parameterised-instance change that was here is reverted — its
         premise was refuted, see defect #24.  Nothing to verify.)
 4. **Defect #22 is root-caused; what is left is the fix.**  Cog runs
    `RSLinesTest` 18/18, and the image says
    `RSAbstractLine>>markersIncludesPoint:`'s inner block has
    `outerCode = RSTMarkeable>>#markersIncludesPoint:` — the trait's method,
    not the class's copy — so our method-oop home match cannot succeed.  677
    installed methods carry that shape with a non-local return in the block.
    Run `PHARO_NO_JIT=1` on the test first: it separates "the dynamic
    `outerContext` fallback is never reached" from "it is reached with a stale
    `closure_`".  (Superseded detail below.)  It was clean in
    the 2026-08-22 sweep and dirty in this one, so treat it as timing-sensitive
    and reach for `PHARO_DET_SCHED=1` first (CLAUDE.md).  Both errors are
    `BlockCannotReturn` on a 4-frame non-local return
    (`markersIncludesPoint:` -> `markerShapesInPositionDo:` ->
    `withEnd:controlPoints:do:` -> `setPositionTo:vector:do:` -> `aBlock value:`,
    all synchronous), which is VM-visible, not display.  **There is a
    mechanism-level hypothesis in defect #22 already** — both selectors are
    defined twice, in `RSAbstractLine` and in the trait `RSTMarkeable`, and the
    inline NLR matches the home by method oop against the block's static
    `outerCode`.  Bisect `PHARO_NO_JIT=1` first: passing there implicates the
    stale `closure_` on JIT-resident block returns that `Interpreter.cpp:8432`
    already warns about.
 5. **Re-run `test_relaunch` on `base.image`.**  It failed 2 of 3 cycles
    tonight (`[VM-STOP-TIMEOUT] interpret() has not returned 2s after stop()`,
    the worker still executing -- `[JIT] Stats:` sends kept climbing, so not a
    wedge) and that was **my doing, not a regression**: I pointed it at the
    PREPPED image.  A prepped image auto-starts `SUnitRunner` on resume, whose
    P80 processes hold the worker well past a 2 s deadline.  The 3/3 run
    earlier the same day used `base.image`.  Re-run there for the record; the
    C++ tier is not being claimed green for `test_relaunch` until it is.
 6. x86_64 sweep write-up plus the arm-vs-x86 residual diff.  Re-run it with
    `SUNIT_TIMEOUT_MULT=2` if the timeout-class failures are to be excluded.  **Recover its
    damaged batches first**: 1001-1050 (my own interference; automated in the
    post-sweep script) and, if it storms there too, 1901-1950.  The recovery
    pass added to `sunit-sweep.sh` will NOT have run for this sweep — the
    script was edited while that instance of it was executing, so bash may
    resume after the batch loop at a stale byte offset.  Re-run those ranges
    by hand, in chunks of 5, the way the post-sweep script does for arm.  A ready
    comparison script is in the session scratchpad (`compare-sweeps.sh`);
    the arm side is staged at `sweep-arm/all_results.txt`.
 6b. **Consume `needsScavenge_` in `step()`** — considered and deliberately NOT
    done tonight.  `step()` already handles `needsCompactGC()` at its declared
    "GC safe point: between bytecodes, no C++ locals hold Oops", so the shape
    is established:

        if (memory_.needsScavenge()) {
            memory_.clearScavengeFlag();
            prepareForGC(); memory_.scavenge(); afterGC();
        }

    (the `interpret()` checkpoint's version, minus its bisect knobs).  That
    would end the whole-batch no-scavenge mode described in `docs/deferred.md`.
    Left undone because the benefit is small — the batch it affects is clean
    apart from one unrelated failure, and defect #26's fix makes it exit
    quickly anyway — while a wrong GC call corrupts the heap in every later
    run, and tonight there is no way to verify it properly.  Do it when a GC
    A/B can be run.
 7. ~~Package tier on both arches.~~  arm64 DONE 2026-09-03: **9384 P / 16 F /
    16 E / 0 T** at the baseline's 180 s bound — +3 passes and 3 fewer errors
    than 2026-08-22, with loads 1.5-3.5x faster.  (The first pass scored 55
    lower because it used the script's old 120 s `PER_CLASS_TIMEOUT`, now
    raised to 200 s.)  x86_64 is chained as `pkg-x86-running.sh` with
    `REUSE_FROM` the arm64 loads and `PER_CLASS_TIMEOUT=400`, since the loads
    cannot run under Rosetta and the bound has to be ~2x arm's.
 7b. **Chase defect #25's DNUs** — the highest-value single item to come out of
    tonight.  Our headless resume restarts `MorphicRenderLoop` at pri-80, it
    busy-spins `WorldState>>drawWorld:` on Morphic DNUs, and the Delay
    scheduler dies; the runner's first startup action is to suspend it, and
    that suspension is what makes nine classes fail (#24).  The runner
    suspends the loop before the DNUs can be observed, so resume WITHOUT the
    suspension and read what `drawWorld:` sends that the image does not
    understand.  A cheaper harness-side lever is worth trying first: our
    prepped image is snapshotted with a live Morphic world, where a stock
    headless boot has none — prep with `NonInteractiveUIManager` and no world
    before `snapshot:andQuit:` and there is nothing for the runner to suspend.
    (Merely switching the manager back AFTER suspending does not work; tested
    on Cog, still 18 P / 5 E.)
 8. **Run the full Δcog sweep.**  `scripts/cog-sweep.sh` (new) drives stock Cog
    in batches with `SUNIT_PREFIX` namespacing, so its `/tmp` state cannot
    collide with ours:

        scripts/cog-sweep.sh docs/results/sweep-arm-2026-09-02/class-index-map.txt \
            <scratch>/cog-sweep

    That produces the first full-suite Δcog baseline this host has ever had.
    Do it on an idle machine — namespacing solves the `/tmp` collision, not the
    wall-clock sensitivity.  Then `scripts/classify-sunit.py cog.txt ours.txt`.
 8. **Use the Cog baseline that is now available.**  DONE 2026-09-02: the
    stock VM's `codeZone` abort was arch-specific, not host-specific, and the
    **x86_64** Cog under Rosetta runs here.  Installed at `/tmp/harness-x86`,
    Cog v10.3.9, `eval` and `eval --save` both verified.  Every stock-VM
    command needs the `arch -x86_64` prefix.  See CLAUDE.md.

    What this unblocks, in rough order of value:

      * **`TraitTest`** — "Cog 54 P / 0 F / 0 E, ours errors, not yet drilled",
        carried since 2026-06-01, and now one of the classes in the batch the
        storm killed.  A Cog run of the whole trait block says at once whether
        the storm is ours.
      * **Defect #19, glyph drawing 12-25x** — needs a Cog timing number.  Use
        `build-x86` for the comparison, not `build-rel`: both it and the Cog
        are x86_64 under Rosetta, so the ratio means something.
      * **The XMLParser ~8.8 s question** in the open list, which is written up
        as "needs a stock-Cog number this host cannot produce".
      * **Defect #2's six remaining packages**, which have no current Cog
        numbers at all.
      * The whole `scripts/classify-sunit.py cog.txt ours.txt` Δcog path in
        `docs/vm-compat-bugs.md`, and `run_sunit_cog.st`.

## Where the three tiers stand

    tier       arm64                            x86_64
    VM C++     test_relaunch 3/3 on base.image;  4/4 as of 10f5f330;
               rest as of 10f5f330, the day's    the day's commits unbuilt
               later commits unbuilt
    packages   9384 P / 16 F / 16 E / 0 T      9382 P / 16 F / 18 E   (2026-08-23)
               (2026-09-03, at the baseline's
                180 s bound: +3 pass, -3 err,
                loads 1.5-3.5x faster)
    SUnit      27453 P / 21 F / 21 E / 5 T      27833 P / 22 F / 26 E / 10 T
               2005 classes, 99.17%             2046 classes, 99.15%
               (39 classes lost to the storm)   (ran the block arm lost)

Both sweeps are 2026-09-02/03 and use the same prepped image.  The x86_64 run
is the more complete of the two: batch 1901-1950 storms on arm64 and runs clean
on x86_64 in 270 s.  Every arch-only difference is a Rosetta timeout, a
wall-clock assertion, a missing Windows-only symbol, a CWD artifact, or a class
the other arch never reached — **no codegen divergence in 28071 tests**.  The
denominator on both is ~38% short of the real suite (see #24).

The 2026-08-23 SUnit line this replaces read `800 P / 13 F / 3 E / 1 T` on
arm and `798 P / 15 F / 4 E / 1 T` on x86 -- that was the 30-class residual
re-measured against a fake-GUI image, not a full sweep, and the two are not
comparable.

Every non-pass is attributed by name in
`docs/results/sweep-arm-2026-09-02.md` (and, for the 2026-08-22 runs,
`docs/results/sweep-{arm,x86}-final-2026-08-22.md`). One newcomer is
untriaged: `RSLinesTest` errors twice with `BlockCannotReturn`, which is a
VM-visible condition rather than a display one. Everything else is display,
network, image drift, or the two known GC/finalization items.

VM C++ tier re-verified green today on both arches after the JIT changes:
`test_class_table`, `test_sista_ir`, `test_sista_survey`, `test_relaunch` (3/3
cycles) on arm64; `test_class_table`, `test_sista_ir` on x86_64.

## Landed today

**Two JIT recompile defects, both found by reading the eviction/recompile/IC
paths rather than by reproduction, and both only reachable since eviction was
wired up on 2026-08-22.**

1. *A superseded method could reclaim its MethodMap entry after GC*
   (`21047517`). `recompile()` deliberately does not free the old JITMethod —
   its code can be live on the stack and other methods' ICs may hold a J2J
   entry into it — but it left the old version `state == Compiled` under the
   same `compiledMethodOop`. `rebuildMethodMap()` runs after every GC and
   re-inserts every Compiled method under that key; `MethodMap::insert` updates
   in place and the zone list is ordered by **address**, so the winner was
   whichever version sat higher in memory. Bump allocation always puts the
   replacement higher, which is why it never showed; once eviction recycles the
   free list a replacement can land *below* what it replaced and the stale
   tier-1 version reclaims the entry, permanently undoing the recompile.
   Fixed with `kSuperseded` (bit 2 of `JITMethodStats::flags` — a heap
   side-table, because `JITMethod` is size-locked to `TrampolineAsm.S JM_SIZE`
   and cannot grow), skipped in `rebuildMethodMap`. Measured, varying only zone
   size:

        zone 64MB   599 superseded    0 outrank their replacement (no eviction)
        zone 12MB    36 superseded    8 outrank
        zone  6MB    13 superseded    1 outrank
        x86  12MB   267 superseded   19 outrank
        real suite  899 superseded  207 outrank      <- 23%

2. *Recompiling never redirected existing callers* (`7c764f09`). A monomorphic
   J2J site branches straight to the entry address in `icData[2]` and never
   consults the MethodMap, so callers kept running the superseded body forever
   — including the site whose count triggered the recompile, which cannot
   re-queue it either because that counter fires on `== 500` exactly. Probed:
   597 recompiles stranded **1242** IC entries across 369 methods; in the live
   suite the scrub fires ~1360 times for ~2820 entries. `recompile()` now
   scrubs them, mirroring the eviction scrub including the PMS slot-0
   re-derive. It **clears** rather than retargets, because the extra word
   carries specialization bits derived from the old JITMethod and per-version
   properties (`canSkipJ2JSave`) need not match. Opt out:
   `PHARO_NO_RECOMPILE_IC_RETARGET`.

   This is a consistency fix, **not a speedup** — A/B flat (inject:into: loop
   4.38 vs 4.38 s; `28 benchFib` 3.37 vs 3.39 s), and it does not increase
   dropped IC patches (`PHARO_IC_PATCH_DEBUG=1` reports zero "STALE pending
   slot dropped"). Its cost is a **full zone scan per recompile**, now
   instrumented under `PHARO_TRACE_RECOMPILE_FLOW` (`67fe25dc`) rather than
   assumed — the number is still to be read off a suite run.

**The SUnit harness was silently broken** (`12ad7265`). The documented prep ran
through the stock Cog VM, which cannot start on this machine at all — it wants
its code zone at a fixed address Darwin 27 will not grant, aborts, and
segfaults (rc=255) on *any* image including a pristine one. So the prep did
nothing, the image saved without `SUnitRunner`, and every suite launch wrote no
results file — indistinguishable from a scheduler wedge, which is how it
presented. Compounding it, `eval --save` is a stock-VM flag ours does not
implement, so even under our VM the fileIn ran and was never persisted. Prep now
uses our VM onto a copy, ending in an explicit snapshot, verified by size
(~54 MB -> ~73 MB). Also means **no live Δcog baseline is obtainable here.**

**The runner's last silent skips are gone** (submodule `749ce14`). The selector
skip list is now empty. Its final two entries were the inherited `ClassTestCase`
AST-lint rules, held back on a cost argument the comment itself flagged for
re-measurement: only **50** classes inherit `ClassTestCase`, not the ~640
assumed, and both selectors across all 50 is **100 tests, 100 PASS, 6473 ms**.
They were omitted *silently* — a skipped selector was never counted in S — so
they appeared in no report.

**Seven never-investigated SUnit residual failures diagnosed** (`2b90dc6e`) —
two pass in isolation; two "Got 4 instead of 2" are Spec *layout* tests counting
layout children, driven by the class-side `maximizeAssertionSpec:` global set
*after* the layout is captured; one inspects its own enclosing call stack and so
reports on whatever wraps it; two are a Spotter timing assertion and a
UI-dependency list. None is a VM computation error.

## Open

  * **`cull:` DNU — 0 reproductions in 24 runs.** `b887d81f` matches the symptom
    and measurably moved `canSkipJ2JSave` (22.4% -> 1.3%), but with no repro
    that is a hypothesis, not a result. Do not write it up as fixed.
  * **The Monticello runaway that ends the sweep.** `MCDictionaryRepositoryTest`
    reproduces it alone (rc=134). The tests are innocent: all five pass when
    driven directly, and `MCRepository fromUrl: 'file:///tmp'` returns the right
    object. Under the runner, P40 sits in `MCRepositoryTest class>>pragmasDo:`
    while P79 spins in `Error>>signal` / `signalerContext` /
    `findContextSuchThat:` / `freeze`, minting errors until the heap is gone.
    `on:do:` catches correctly in both the main process and a `forkAt: 40`, so
    handler visibility is refuted; the fork+watchdog machinery is the remaining
    suspect.
  * **`WeakAnnouncerTest>>testNoDeadWeakSubscriptions`.** Our GC nils the weak
    slots correctly (they print `nil subscribes to MethodAdded`); what does not
    happen is the dead subscriptions being swept from the registry, i.e.
    finalization timing. `PHARO_WEAK_SURVIVOR_PATHS` prints why a referent
    lived. The old caveat here — "this test is one of the 17 Pharo's own CI
    skips, so it is not evidence Cog passes anything" — **is now settled by
    measurement (2026-09-02): stock Cog runs the class 33 P / 0 F / 0 E with
    the same one skip, so Cog does pass it and this is ours.**
  * **Old-space fragmentation (12x).** `makeFreeChunk` was necessary, not
    sufficient. Four 32-byte pins strand 146 MB; they are pinned while already
    old, so the fix needs either a safe alternative to `becomeForward` at pin
    time or GC-safepoint relocation when `callbackDepth_ == 0`. Feeding
    relocation from the arena at pin time crashes (rc=133) — `becomeForward`
    rewrites references mid-primitive. The tenure path is safe because scavenge
    already owns that contract.
  * **`PMArbitraryPrecisionFloatTest>>testPrintAndEvaluate`** passes standalone
    and with its whole class, fails only in the 117-class run, and not on the
    bound (300 s and 900 s both unchanged); 19 preceding classes do not
    reproduce it.
  * Whether ~8.8 s is reasonable for those XMLParser tests at all — needs a
    stock-Cog number, and as of 2026-09-02 this host CAN produce one (x86_64
    Cog under Rosetta, see CLAUDE.md); it needs XMLParser loaded into a Cog
    image first.

`PHARO_OLDSPACE_FREELIST` is **no longer** on this list. It works as of
2026-08-23 after a sixth bug: `collectInstancesOfClass` matched `classIndex 0`,
so `allInstances` of a class with no class-table entry answered an Array of free
chunks — which is what killed class-shape migration. Read the knob's own comment
in `src/vm/debug_vars.h:42`; ten hypotheses are closed by test in there.

## Carried forward from the archive

Still true, and recorded in no other file. Each was re-verified against the tree
on 2026-08-23; line numbers are from that day. Grouped, terse, and deliberately
not expanded — the full narrative is in
`docs/history/wip-2026-05-27--2026-08-23.md`.

### JIT / code zone

 1. **evictLRU pass 2 is address order, not recency.** Pass 1 needs ~650K
    method entries untouched (64K per epoch, `epoch_-10` threshold,
    `CodeZone.hpp:313`), so pass 2 usually runs and walks `firstMethod_`
    (`:331-344`), evicting the earliest-compiled kernel methods that stay hot.
    `763f7f7b` fixed the epoch never advancing; the fallback is unfixed.
    Planned: progressively younger thresholds (`epoch_-5, -2, -1`, then all).
 2. **The evictLRU second-pass change was measured and DROPPED** — 1 MB zone
    8 s/277 methods vs 7 s/286; 192 MB zone (XMLParser load) 344 s/5769 vs
    340 s/5542. Re-open only with a workload that fills the zone *and* advances
    the epoch.
 3. **248,066 methods compiled during one XMLParser load** against ~176k
    CompiledMethods+CompiledBlocks in the whole image (recompile churn).
    `evictTarget` is 1/64th of the zone (`JITCompiler.cpp:1780-1782`) and may
    want raising now that a round is cheaper.
 4. **Bit-60 J2J precedence makes five inline-return specializations dead on
    arm64** — the bit-60 `tbnz` is emitted before the extras dispatch
    (`AsmjitT1.cpp:6488`, dispatch at ~6498/6506/6512/6525/6540). Derived
    upside ~3.9% on a 356 ms workload. **The fix is the reorder**;
    `PHARO_NO_J2J=1` alone makes things slower.
 5. **arm64 emits 2.7-2.9x more bytes per compiled method than x86_64** — 7.89
    vs 2.76 KB/method; 8.91 vs 3.31 across full sweeps. This is why arm64 fills
    the code zone first.
 6. **35% of arm64's code footprint is inline-J2J** — 45,176 KB drops to
    29,260 KB with `PHARO_T1_NO_INLINE_J2J=1` (`debug_vars.h:92`).
 7. **IC hit rates are not comparable across arches**: arm walks IC slots 0-2
    inline (`AsmjitT1.cpp:6331-6363`), x86 probes slot 0 and exits
    (`:3185-3189`), so the two builds count different event populations.
 8. **`[JIT] Stats:` lines are periodic checkpoints, not totals** — one every
    65,536 method entries (`JITRuntime.cpp:~3896`). Two mid-run samples were
    once published twice as evidence of a structural arm-vs-x86 gap; real
    end-of-run totals are 0.56% apart.
 9. **`PHARO_T1_INLINE_PRIM_COUNTERS=1` gotcha** — without it `g_primAt_hits`
    and friends stay 0 even when the inline path is firing, giving a misleading
    "primAt=0".
10. **The asmjit T1 inline-SETTER path never fires in micro-benchmarks**
    (getter=16059, setter=0; `detectTrivialMethod` runs ~168 times per
    tinyBench and never classifies a setter). The recognizer is not broken —
    fib/sieve/tinyBench contain no setter sends.
11. **DEAD END, do not re-walk**: adding the inline-asm write barrier to the six
    temp-store stencils (`storeTemp_1/_2`, `popStoreTemp_1/_2/_3/_4`,
    `stencils.cpp:~4496-4540`) left the miss count unchanged at 230 and
    regressed tinyBench ~11% (5350 -> 5950 ms). JIT code runs `frameDepth_ > 0`,
    so that path is virtually never exercised. Reverted.

### GC / memory

12. **The remembered set is dead infrastructure.** `rememberedSet_` is populated
    (`ObjectMemory.cpp:3712`) but never iterated — only `.clear()`ed (`:2321`);
    scavenge still does an O(oldSpace) full scan ("Trade correctness for perf
    until every write site is audited", `:2221-2226`). Closing it would drop
    ~30 ms/scavenge on a 100 MB heap. The audit reached 260 -> 228/230 misses at
    99.997% bit accuracy; residual misses are C++ paths, not the JIT path.
13. **`PHARO_WATCH_ROOT_CLASS` (`debug_vars.h:201`) is an exact class-name
    compare** — it does not follow subclasses, so check `allSubclasses` before
    reading a zero-hit result as meaningful.
14. **Raw-heap-scan gotcha**: `PHARO_SCAV_RAWSCAN`
    (`ObjectMemory.cpp:2434-2513`) scans format-agnostically and hits
    `ExternalAddress` objects (fmt 16, ptrSlots=0) that legitimately store
    native addresses — all 54 hits in one run were such false positives.
15. **`[SCAV-DANGLE]` shares the scavenger's blind spot** — both walk the same
    `forEachRoot` set (`ObjectMemory.cpp:2188` and `:2421-2425`), so a holder
    outside that set makes the diagnostic report clean while the object dies.
    A clean dangle scan is not proof the roots are complete.
16. **`ObjectMemory::setGlobal`'s tail** (`ObjectMemory.cpp:~1063-1073`) walks
    the dictionary array for the first nil/zero slot and stores there, with no
    hash placement and no tally update. Unresolved, recorded nowhere else.
    (The `68` DNU that led here *is* fixed; see `ObjectMemory.cpp:1029-1035`.)

### Scheduler / VM control

17. **`Interpreter::running_` is a plain `bool`** (`Interpreter.hpp:1168`),
    written by `stop()` (`:285`) and read by the dispatch loop — a real data
    race, and the leading candidate for the `vm_stop` timeout. Left standing
    **deliberately**: `std::atomic` puts a real load in the hot dispatch loop,
    so it needs a bench-suite before/after, not a blind edit.
18. **`removeProcessFromList`'s return value is discarded**
    (`Interpreter.cpp:18685`, `Primitives.cpp:5017`) and the caller then
    unconditionally nils `ProcessNextLinkIndex`, truncating the run queue if the
    search failed. Wants a repro before touching the scheduler.
19. **`vm_stop` timed out on roughly one relaunch cycle in thirty** before the
    later fixes (10 runs: 9 pass, 1 fail, one stop-timeout, one destroy-leak).
    Post-fix runs show zero. Budget that rate for any repro attempt.
    Instrumentation is live at `PlatformBridge.cpp:618` and `:669`.

### Measurement traps

20. **We do NOT set `PHARO_CI_TESTING_ENVIRONMENT`**, so our sweeps run the 17
    tests Pharo's own CI skips via `skipOnPharoCITestingEnvironment` —
    including `AllocationTest` testOneGB/testOneGWordAllocation,
    `RandomTest>>testUnixRandomGeneratorSeed`, `StDebuggerTest`, `StSpotter*`,
    `StPlaygroundPageTest>>testContents`, `TKTNewProcessTaskRunnerTest`, and
    `WeakAnnouncerTest>>testNoDeadWeakSubscriptions`. Check the guard before
    calling any of them a VM defect.
21. **Classes that time out emit no `Total:` line** and drop out of the
    per-class tally (`run_sunit_tests.st:582-598`) — which is why arm counted
    2043 and x86 2037 against 2047 declared. A class-count difference between
    runs is not evidence of a crash.
22. **Package-tier noise floor is ±2**: four PolyMath runs on one day scored arm
    err = 16, 16, 17, 18. Any arm-vs-x86 package delta needs two runs per side.
    `sunit-sweep.sh:95-102` states the equivalent rule for SUnit;
    `package-tests-selfhosted.sh` does **not**.
23. **A killed run reads as a passing one.** A bisect reported
    `noyg rc=124 mnu68=0` and the zero was a TIMEOUT — 900 s is not enough for a
    no-young-generation run (`PHARO_NO_YG`, `DebugSettings.cpp:379`). Record rc
    and an explicit completion marker alongside every count.
24. **"It did not reproduce in N runs" is not "it does not happen"** unless N
    suits the rate — 9 consecutive clean runs were once cited as proof against a
    ~10% event, and a correct conclusion was retracted on that basis.
25. **Verify the binary contains the change before blaming the architecture.**
    `build-x86` had not been rebuilt after the class-registration fix, so x86
    reported the pre-fix signature and read as an arch-specific GC defect. Both
    trees must be rebuilt (`build-rel` arm64, `build-x86` x86_64);
    `strings <binary> | grep <marker>` settles it.
26. **`bash -n` passing does not mean a driver script works** — a Smalltalk
    `"comment"` inside a double-quoted eval string terminates the string.
    Smoke-test every generated eval.

### Host & tooling

27. **`scripts/sunit-sweep.sh` still defaults `PER_BATCH_TIMEOUT=600`**
    (`:111`), which is too tight — batch 1501-1550 legitimately takes 613 s and
    is not a hang. `scripts/rerun/fullsweep.sh:9` sets 900.
28. **`scripts/rerun/fullsweep.sh:4` and `scripts/rerun/pkgrun.sh:9` have a
    session scratchpad path baked in** — fix that line before rerunning either.
29. **After a reboot clears `/private/tmp`, run `git worktree prune`** — git
    otherwise still believes the branch is checked out in the deleted worktree
    and refuses `git checkout jit`. There is a live scratchpad worktree today.
30. **x86-box constraints**: gdb there cannot read member vars (no full DWARF),
    so instrument in C++ instead; `gdb -p` needs
    `sysctl kernel.yama.ptrace_scope=0`; `PHARO_T1_TRACE_HANDLER`
    (`debug_vars.h:288`) dumps the handler-search chain.
31. **Cog-x86 baseline** measured on the x86 box (18.221.159.216), recorded
    nowhere else: loop20M ~44-50 ms, fib30 ~9-10 ms, cfibx30 ~13 ms.
32. **arm64 cost baselines** (ad-hoc evals, no committed script): INTADD3M
    4 ns/iter; BLOCKVAL3M 72 ns per block `value:`; ALLOCARR2M 93 ns per
    `Array new: 4`; COMPILE300 1.54 ms per `Compiler evaluate: '1+1'`. Block
    activation and small-object allocation sit 18-23x above the fast integer
    path.
33. ~~**Whether this VM is slower than Cog on these workloads is unanswerable
    on this host**~~ — SUPERSEDED 2026-09-02. The abort is arch-specific: the
    **x86_64** Cog runs under Rosetta (`arch -x86_64`, see CLAUDE.md), so the
    comparison IS available, and it is fair against `build-x86` since both are
    x86_64 under Rosetta. Separately, Rosetta overhead vs x86_64 backend code
    quality has still never been split.
34. **`-Wnontrivial-memcall` has an array-form blind spot** — it fires on
    `memset(p, 0, sizeof(OpenPort))` but not on
    `memset(gPorts, 0, sizeof(gPorts))`. Any warning-driven sweep inherits the
    hole. (Re-reproduced on Apple clang 21.0.0.)
35. **`LinkInstallerTest>>testPropagateNewClassScopedLinksOnMethodNode` is
    GC-pressure-dependent, not the JIT** — 40/40 alone with JIT on or off; fails
    with any predecessor class; `PHARO_NEWSPACE_MB=1` makes it fail alone but
    only sometimes (39/40 and 40/40 on identical re-runs). Needs repetition
    counts before believing any knob bisect.
36. **Do not re-chase the `cos(2)` 1-ULP claim.** The image defines
    `Float>>cos` as `^ (self + Halfpi) sin`, and computing *that* formula with
    this host's libm reproduces the VM's bits exactly. No discrepancy exists.

### Corrected while pruning

37. **The WideString-WriteStream bug is FIXED** (`d5608fd4`, 2026-06-01), but
    `docs/results-jitpkg.md:519-521` still cites "the already-documented
    WideString-WriteStream bug (WIP.md)" as if open. Fix that citation rather
    than re-investigating.
38. **`indexOfClass` returning 0 for not-found** (`ObjectMemory.cpp:628,649`) is
    still true, but the "any new caller is unguarded" half is now stale — 5 of
    the 10 enumerated callers were guarded on 2026-08-23 (`b0ea7721`).
