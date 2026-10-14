# Machine-code frames: one frame model for compiled and interpreted code

Status: the arm64 default since 2026-10-14 (opt-out `PHARO_NO_MCF`).  The
phase list and section 4 record what is done.

## 1. The problem

On arm64, T1 only makes code with loops faster.  Code that is mostly
message sends runs at interpreter speed or slower, and Cog is 6-12 times
faster on such code.

Send-path benchmarks (`mcf-bench.sh`, Release build, milliseconds, best
of 3):

| Benchmark | Our VM, JIT on | Our VM, `PHARO_NO_JIT` | Cog (x86_64, Rosetta) |
|---|---|---|---|
| `30 zzFib` (recursive sends) | 65 | 60 | 7 |
| 3M sends of `zzB:` in a loop | 97 | 98 | 8 |
| 300K `inject:into:` | 128 | 127 | 22 |

Three facts cause this:

1. **Policy.**  T1 compiles a method only when the method has a loop
   (`JITRuntime::noteMethodEntrySlow`, "loops-only T1 policy").  On arm64
   it also refuses every method whose code would emit an inline-J2J
   closure push (`emitClosurePush`), which is every method with a real
   send.  `zzFib` is never compiled.
2. **Every send leaves machine code.**  When T1 code sends a message, the
   code exits to C++ (`ExitSend` / `ExitSendCached`).  C++ does the send,
   the interpreter runs the callee, and when the callee returns,
   `tryJITResumeInCallerSlow` builds a new `JITState` and enters the
   caller's code again at the next bytecode.  With the policy switched
   off (`PHARO_T1_RESUME_EXTERNAL_J2J=1`), printString makes 281 million
   such resumes in 25 s, and the glue (`tryJITResumeInCallerSlow`,
   `JITRuntime::tryResume`, `upgradeICToJ2J`) takes about 28% of the
   time, against 7.8% for the compiled code itself.  printString then
   needs 104.6 billion instructions against 83.7 interpreted.
3. **Two frame models.**  The fast compiled-to-compiled call that exists
   (inline J2J) keeps the caller's state in a `J2JSave` record in
   `j2jPool_`, not in `savedFrames_`.  When the callee must leave machine
   code, C++ must convert ("materialize") the chain of `J2JSave` records
   into `SavedFrame`s.  That conversion is the code that corrupts state
   under resume-internal J2J (jrpc STON parse errors), and the reason for
   the refusal policy in fact 1.

## 2. The design

One rule: **machine code keeps the interpreter's own frames.**  There is
no second frame model and nothing to materialize.

### Invariants

- **INV-1 (one frame model).**  `savedFrames_`, `stack_` and the
  interpreter's frame registers (`method_`, `receiver_`, `framePointer_`,
  `closure_`, `homeMethod_`, `argCount_`, `activeContext_`, ...) are the
  only frame state.  Machine code that calls another method writes the
  same `SavedFrame` that `pushFrame` writes.  At every exit, only `sp`
  and `ip` need to be copied from `JITState` to the interpreter.
- **INV-2 (exits are bytecode boundaries).**  When machine code cannot
  continue, it exits with `ip` at the start of a bytecode and `sp` exact
  for that bytecode.  The interpreter then executes that bytecode in the
  usual way.  Sends to interpreted methods, primitives, `doesNotUnderstand:`,
  non-local return, unwinding and process switches are all done by the
  interpreter code that does them today.
- **INV-3 (return into machine code is a memo).**  A `SavedFrame` can
  hold `mcReturn`: the machine-code address that continues the frame
  after the call returns.  The address is a memo of the lookup "compiled
  code of `savedMethod`, entry for bytecode `savedIP`".  It is valid only
  while `savedIP` equals the IP recorded with it (`mcKeyIP`) and the code
  epoch recorded with it equals `JITRuntime`'s code epoch.  The code
  epoch increases on every code flush, eviction, become of a method, and
  every GC that can move a method.  Any other change to a frame (debugger,
  `executeFromContext`, a context write) changes `savedIP` or the epoch,
  or makes a new frame with `mcReturn` cleared, so a bad memo is never
  used: the frame is interpreted instead.
- **INV-4 (flat C stack).**  Machine code does not keep Smalltalk state
  on the C stack across a send.  C++ enters machine code from
  `interpret()` through a trampoline, and machine code returns to that
  trampoline on exit.  A compiled-to-compiled call is a branch, and the
  return address is in the callee's `SavedFrame`, not in a C stack frame.

### Return into machine code

`returnValue` pops the frame (as today) and pushes the result.  Then, if
the frame that is now current has a valid `mcReturn`, the interpreter
enters machine code at that address through the resume trampoline.  This
replaces `tryJITResumeInCallerSlow` for these frames: no `JITState` is
built (the fields that do not change per entry are set once), no method
map lookup is made, and no exit-handling loop is entered.

### Exit at a send

When machine code exits at a send, it records the resume address of the
bytecode after the send in `pendingMcReturn_`.  The interpreter executes
the send bytecode.  If the send activates a method, `pushFrame` copies
`pendingMcReturn_` (with its key IP and epoch) into the new `SavedFrame`.
If a primitive answers without a frame, the interpreter enters machine
code again at once.

### Compiled-to-compiled call (phase 2)

At a send whose inline cache holds a compiled method, machine code
writes the callee's `SavedFrame` (the fields `pushFrame` writes, plus
`mcReturn`), updates the frame registers and the `JITState` view, fills
the callee's temporaries with nil and branches to the callee's entry.
At a method return, machine code pops the `SavedFrame`, restores the
frame registers and, when the caller's memo is valid, pushes the result
and branches to `mcReturn`.  Otherwise it exits, and the interpreter
continues the caller.  A return from a frame that has a materialized
context, a block return, and a non-local return always exit (INV-2).

## 3. Phases

Each phase has a binary done condition.  A phase that does not meet its
condition is reverted, not tuned.

1. **Memo return (no new machine code).**  `SavedFrame.mcReturn`,
   `mcKeyIP`, `mcEpoch`; the code epoch; `pendingMcReturn_`; the resume
   path in `returnValue`.  Done when: with T1 compiling send-bearing
   methods, printString needs fewer instructions than the interpreter
   (83.7 billion), and the battery and one full SUnit sweep give the
   same results as sweep U.
2. **Native compiled-to-compiled call and return** (arm64 T1 send tail
   and return emission).  Done when: `30 zzFib` and the send loop are at
   least 3 times faster than the interpreter, with the same battery and
   sweep results.
3. **Policy.**  Compile send-bearing methods by default; remove the
   loops-only and closure-push refusals for this path.  Done when:
   deeptraverser, lexicon and printString instruction counts are lower
   than today's (1004, 1699, 83.7 billion), with clean battery, sweep
   and package runs.
4. **Delete the old glue** that the new path makes dead (resume loop,
   `J2JSave` materialization) once phase 3 is the default.

Kill rule: if a phase makes the workloads slower and a measurement does
not show why, stop and measure before the next phase.

## 4. Status and measurements

The mode is opt-in: `PHARO_MCF=1` (arm64 only).  The code is in
`src/vm/jit/MachineCodeFrames.cpp` (entry, exits, memo resume) and in
`AsmjitT1.cpp` (`emitMcfReturn_arm64` and the `mcfNative` block at the
inline-cache hit).

**Phase 1 (2026-10-13): done, but it does not meet its condition.**
printString needs 97.7 billion instructions: less than the old send glue
(104.6), more than the interpreter (84.8).  `30 zzFib` takes 95 ms
against 62 interpreted.  The phase-1 condition was wrong: while every send
still leaves machine code, each call pays two machine-code entries on top
of the interpreter's own send, so phase 1 alone cannot beat the
interpreter.  A census of the send exits during printString (diagnostic
knob `PHARO_MCF_CENSUS`) decided the next step.  Of 29 exits for each
printString:

- 40% go to a compiled method without a primitive,
- 23% go to a compiled method with a primitive,
- 24% go to a primitive method that is never compiled,
- 10% are inline-cache misses,
- 3% go to a method that is not compiled.

So direct calls (phase 2) can remove most exits.

**Phase 2a (2026-10-13): done.**  Direct call to a compiled method
without a primitive, through the call cache `g_mcfCallCache`, and the
direct return.  The direct call refuses at frame depth 0, because there
`pushFrame` must store the caller's pc into `activeContext_`; a first
version without this check failed at startup.

| Benchmark | Interpreter | `PHARO_MCF=1` |
|---|---|---|
| `30 zzFib` | 62 ms | 30 ms |
| 3M sends of `zzB:` | 97 ms | 33 ms |
| 300K `inject:into:` | 132 ms | 131 ms |
| printString (instructions) | 84.8 billion | 81.9 billion |
| deeptraverser (instructions) | 1004 billion | 1173 billion |

The VM fix battery passes with `PHARO_MCF=1`.

**Phase 2b (2026-10-13): primitives and receiver-independent
specializations.**  A census by selector showed where the remaining exits
went.  Three changes:

1. Every send site in this mode takes the inline-cache-hit path without
   inline J2J.  That path lacked the `^ literal`, `^ argument` and
   `^ self` specializations, so a quick primitive on an immediate receiver
   (`SmallInteger>>asInteger`) left machine code every time.  The path
   now dispatches them before its heap-receiver check.
2. A compiled method whose primitive T1 implements as a prologue (the
   arithmetic, comparison and bit primitives, `at:`, `at:put:`, `size`,
   `==`, SmallFloat arithmetic) can be called directly: the prologue is the
   primitive, and its failure edge runs the method body.  A method that
   stores a primitive error code is refused.
3. A send to a primitive that only touches the stack (a whitelist in
   `mcfPrimCallable`) runs the primitive through the helper
   `pharo_mcf_prim` and continues in machine code.  On failure the send
   exits and the interpreter runs it.  If the primitive pushed a frame or
   switched process, machine code exits with `ExitDeopt` and the
   interpreter's state is used as it is.

| Benchmark | Interpreter | `PHARO_MCF=1` |
|---|---|---|
| `30 zzFib` | 62 ms | 31 ms |
| 3M sends of `zzB:` | 97 ms | 15 ms |
| 300K `inject:into:` | 132 ms | 142 ms |
| printString (instructions) | 84.8 billion | 70.7 billion |
| deeptraverser (instructions) | 1004 billion | 941 billion |

The battery passes.

**Phase 2c (2026-10-13): `==` inline, six-entry probe, and three defects
fixed.**

- The deeptraverser census showed that most "inline-cache misses" were not
  polymorphic sites: they were the `==` bytecode (0x76), which T1 emitted
  as a send whose cache is never filled, so every `==` left machine code.
  T1 now compares identity inline for `==` and `~~` (0x78), as Cog's
  `bytecodePrimIdenticalSistaV1` does; a forwarded operand exits at the
  bytecode.  The interpreter's slow dispatcher now does the same (its fast
  dispatcher already did).
- The IC probe walks all six entries, and a full site probes the mega
  cache before it exits.
- Defect 1: `mcfActivate` entered a primitive method at `codeStart`, after
  `activateMethod` had already run and failed the primitive and stored its
  error code.  It now enters at the interpreter's ip.
- Defect 2: a pending IC patch that `mcfRun` left for the interpreter
  could land on a different send when the interpreter did the exited
  bytecode without a send (`==`, block `value:`, SmallInteger arithmetic).
  The patch is now keyed to the ip after the exited send
  (`pendingICOwnerIP_`).
- Defect 3 (the one behind the sweep regressions): at an exit, `mcfRun`
  resynced the interpreter's frame registers only when the method or the
  frame pointer had changed.  A native return followed by a native call
  can leave a new activation of the same method at the same frame
  pointer with another receiver (`new` sent to two classes).  `receiver_`
  stayed stale, the next `pushFrame` saved it, and later code ran against
  the wrong object: `WeakSet>>growTo:` filled a symbol table's new array
  with another set's `flag`, and startup failed with a DNU of `asSymbol`.
  The resync is now unconditional.  Found with the diagnostic oracles
  under `PHARO_MCF_VERIFY` (memo target, call target, `==` result, pushed
  frame, receiver = fp[0]) and the bisect knobs `PHARO_MCF_CALL_MOD/REM`
  and `PHARO_MCF_WATCH_OOP`.

| Benchmark | Interpreter | `PHARO_MCF=1` |
|---|---|---|
| `30 zzFib` | 62 ms | 31 ms |
| 3M sends of `zzB:` | 97 ms | 13 ms |
| 300K `inject:into:` | 132 ms | 131 ms |
| printString (instructions) | 84.8 billion | 62.5 billion |
| deeptraverser (instructions) | 1004 billion | 783 billion |

The battery passes, and the classes that failed in the phase-2a sweep
(MetacelloVersionNumberTestCase, ZnChunkedStreamTest, the three Spec file
dialog presenter tests, OCCompilerSyntaxErrorNotifyingTest) pass.

Sweep M3 (`PHARO_MCF=1` at `21e4ae7a`): 27877 P / 11 F / 2 E, the baseline
level (sweep U: 27879 / 9 / 2).  Its two non-passes that are not in the
baseline lists both pass alone 3/3: TraitTest>>testSubclasses (a timeout
in the batch that has a known intermittent watchdog timeout) and
WriteStreamTest>>testMethodsOfTheClassShouldNotBeRepeatedInItsSuperclasses
("receiver of #x is nil"; never seen before, a stress run of its lint
rule over 600 classes is clean -- watch it).  lexicon: 1699 -> 1097
billion instructions; myprecious: 44.3 -> 42.9 (network-bound).

**Phase 3b (2026-10-14): native block calls.**  At `value` / `value:`
sites, a FullBlockClosure whose CompiledBlock is in the call cache
(compiled, old, no non-local return, matching arity) is called natively:
T1 pushes the SavedFrame and sets up the block frame as `activateBlock`
does.  Block code may make native calls (the record saves the caller's
closure; the native return restores it).  Entering or resuming block
frames from the interpreter stays opt-in (`PHARO_MCF_BLOCKS`): it was
slower.  A block microbenchmark: 9.57 billion instructions, against 9.94
without native block calls and 13.28 interpreted; deeptraverser does not
change.

**Default (2026-10-14).**  `mcfEnabled()` is true on arm64 unless
`PHARO_NO_MCF` is set.

**After the flip (2026-10-14).**

- Shared stubs: the native method and block call sequences are two stubs
  built once (`emitMcfCallStub`, `emitMcfBlockStub`); code for the same
  printString run is 10% smaller (34.8 -> 31.4 MB).
- `value:value:` and longer block sends call the block natively too.
- T1 no longer emits primitive 60/61/62 methods as prologue-only leaves in
  this mode.  A leaf has no body, so ByteString>>byteAt:put: with a
  Character (every SmallInteger printString) left machine code on every
  call.  printString 62.1 -> 38.65 billion instructions.
- The pending-IC owner check runs at `sendSelector` entry: a block-value
  primitive moves ip first, so the old check dropped every value:value:
  site's IC patch.
- The pending memo is dropped at every GC (keyed by a raw ip).

**Validation of the default.**

| Run | Binary | Result |
|---|---|---|
| Sweep U (old default, baseline) | `1c689fff` | 27879 P / 9 F / 2 E |
| Sweep M3 (`PHARO_MCF=1`) | `21e4ae7a` | 27877 / 11 / 2 |
| Sweep M4 (MCF default) | `35dab876` | 27878 / 11 / 1 |
| Sweep M5 | `ad043e26` | 27877 / 11 / 2 |
| Sweep M6 | `a1a8b3c6` | 27876 / 11 / 3 |

The non-passes of M3-M5 that are not in the baseline lists all pass alone:
a TraitTest timeout in each (that batch has an intermittent timeout, a
different test each time), and one novel error each in M3
(WriteStreamTest, "receiver of #x is nil") and M5 (SpButtonPresenterTest,
SubscriptOutOfBounds on a text), and two in M6 (RSViolinPlotTest,
"receiver of #nextCell: is nil"; TKTParameterizedServiceTest, a TaskIt
timeout).  All pass alone 3/3.  Three novel non-timing errors in four
sweeps is a pattern worth watching: a review of the GC, process-switch,
memo, block and primitive paths found no rare-corruption mechanism these
tests could hit, but none of them has a reproduction yet.  (An earlier
"new vs baseline" list compared files sorted in two different orders;
the lists here use one byte order.)  Battery and
test_class_table pass; test_relaunch failed 1-2 of 3 cycles under load in
both modes.

Package batch (95 packages, `vm-M4`, against Cog): identical except the
known differences -- lexicon's 50 s CodeAnalysisRunnerTest (speed),
soil's and famixreplication's slow timeouts, myprecious's round-trip
timeout (1 now, 3 before), pharo-cig (we find a libclang Cog cannot),
methodproxies (Cog's interpreter/JIT split for special #value) -- and two
newtools tests (StSpotterModelTest timing, StTranscriptPresenterTest
testClear), which pass alone in both modes.

| Workload (instructions, billion) | Before (2026-10-13) | Now |
|---|---|---|
| printString (3M) | 83.7 | 38.4 |
| deeptraverser test | 1004 | 767 |
| lexicon test | 1699 | 1081 |
| myprecious round trips | 44.3 | ~44 (network-bound) |

Open items:

- lexicon is now limited by block creation materializing the frame stack
  (docs/deferred.md: married, read-through contexts).
- The default mode with `PHARO_NO_MCF` pays the larger SavedFrame
  (printString 83.7 -> 84.7).

## 5. Debugging the mode

The switches below are plain `DEBUG_BOOL` / `DEBUG_INT` knobs: they work
only in a build with `-DPHARO_DIAG_KNOBS=ON` (build-dbg, build-dbg2).

| Knob | Effect |
|---|---|
| `PHARO_NO_MCF` | Turn the mode off (works in every build). |
| `PHARO_MCF_VERIFY` | Oracles: memo target, call target, `==` result, pushed frame against `pushFrame`, receiver = fp[0] in a method frame. |
| `PHARO_MCF_CENSUS` | Classify every send exit; print the top selectors, receiver classes and primitives. |
| `PHARO_MCF_NO_CALL`, `PHARO_MCF_NO_RET`, `PHARO_MCF_NO_EQ`, `PHARO_MCF_NO_BLOCK_CALL`, `PHARO_MCF_NO_SPECS`, `PHARO_MCF_NO_POLY` | Turn one part off, to bisect. |
| `PHARO_MCF_CALL_MOD` / `_REM` | Only methods whose selector hash matches are called natively (bisect a method set). |
| `PHARO_MCF_COMPILE_MOD` / `_REM` | Only matching methods run in machine code. |
| `PHARO_MCF_WATCH_OOP` | Log every primitive store of this oop, with the frame chain. |
| `PHARO_MCF_BLOCKS` | Also enter / resume block frames from the interpreter (off: slower). |
| `PHARO_MATFS_CENSUS` | Count materializeFrameStack calls and frames walked. |

How the stale-receiver bug was found (2026-10-13), as a pattern: the five
value oracles were silent; the receiver invariant caught it in one run.
Check the basic frame invariants first.  A bisect by selector hash is
timing-sensitive when the filter itself does string work (the Metacello
failure vanished under it); use a deterministic repro when one exists.
