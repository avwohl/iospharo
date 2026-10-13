# Machine-code frames: one frame model for compiled and interpreted code

Status: design, 2026-10-13.  Work in progress; the phase list at the end
records what is done.

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

The VM fix battery passes with `PHARO_MCF=1`.  Next: primitive methods
(direct primitive calls from machine code), blocks, and the reason for
the deeptraverser loss.
