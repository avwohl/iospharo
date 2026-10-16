# Machine-code frames on x86-64

Status: the default on x86-64 macOS and Linux since 2026-10-18 (opt out
with `PHARO_NO_X86_MCF=1`).  Windows: opt-in (`PHARO_X86_MCF=1`), not
built or tested there yet.  Design: docs/mc-frames-design.md (arm64).

## Why

Measured 2026-10-17 on lexicon, x86-64 builds under Rosetta:

    build-x86, JIT on           117.5 s
    build-x86, JIT off           98.8 s
    build-x86, compile all      175.9 s
    Cog 10.3.9 (x86-64)          11.9 s

On arm64 the same workload takes 69.0 s interpreted and 10.4 s with the
JIT.  With machine-code frames off (`PHARO_NO_MCF`), arm64 compiles 7
methods and takes 66.8 s.  So machine-code frames give the whole JIT win
on arm64, and x86 does not have them: `mcfEnabled()` is false on x86
(`src/vm/DebugVars.hpp`), and the loops-only compile policy
(`JITRuntime.cpp`, `noteMethodEntrySlow`) applies.  Compiling every hot
method without machine-code frames is slower, because each send leaves
machine code and comes back through `tryJITResumeInCaller`.

Rosetta costs about 1.6x (the interpreter on the two arches), so the gap
is in the x86 backend, not in Rosetta.

## What is portable

`src/vm/jit/MachineCodeFrames.cpp` is C++.  Its only arm64 call is
`pharo_jit_osr_resume` (enter machine code at an address).  The
arm64-specific parts are the emitted code in `AsmjitT1.cpp`:

- the resume trampoline,
- the MCF stubs (`emitMcf*`, `mcfBuildStubs`),
- the send and return emission in `emitOne_arm64` under `mcfEnabled()`.

## Phases

Each phase has a binary done condition.  The knob is `PHARO_X86_MCF`
(opt-in) until phase 3.

1. MachineCodeFrames.cpp builds on x86; exits at sends record the memo;
   returns resume through the memo.  Done when the VM fix battery passes
   with `PHARO_X86_MCF=1` on build-x86.
2. Native compiled-to-compiled call and return in x86 emission.  Done
   when `30 zzFib` is at least 2x faster than the x86 interpreter.
3. Policy: compile send-bearing methods on x86 by default.  Done when
   lexicon on build-x86 is faster than with the x86 JIT off, and an x86
   sweep is clean.
4. The arm64 speed work (primitive stubs, block stubs, lazy view) one
   piece at a time, each measured on lexicon.

## Status (2026-10-17)

Phases 1 and 2 are done, and most of the arm64 phase 2b/2c/3b work is
ported.  The mode is still opt-in (`PHARO_X86_MCF=1`).

- Phase 1 (`3d23b3a4`): MachineCodeFrames.cpp runs on x86; `mcfMode()`
  gates the shared C++ paths.
- Phase 2 (`7a207146`): native call at an IC hit (`emitMcfCall_x86`) and
  native return (`emitMcfReturn_x86`).  Every x86 record is a full one
  (`recFull` 1), the shape a C++ `pushFrame` writes.
- `85461839`: six-entry IC probe, inline `==` and `~~`, primitive targets
  through `pharo_mcf_prim`, constant-return methods at the call site.
- `dd9dcc7e`: block frames, native calls from blocks, `value`/`value:`
  block calls (`emitMcfBlockCall_x86`), unprefixed `ExtSend` sites; an
  exit at an arithmetic special send runs the primitive in `mcfRun`
  (both arches).

Measurements, x86 under Rosetta:

    benchmark            interpreter  old x86 JIT  x86 MCF  Cog
    30 zzFib (ms)            121          122         28      5
    3M sends (ms)            151          200         33      6
    lexicon (s)              98.8        117.5       ~66    11.9

Micro set (ms; old x86 JIT, x86 MCF, Cog): doBlock 2550, 657, 129;
fib 37, 8, 2; isKindOf 2308, 318, 77; dictAt 3653, 2177, 308.

Sweep X1 (x86, the mode on, `85461839`): 2042 classes.  All 274 extra
non-passes were Athens/Roassal tests that could not load Cairo; the cause
was the FFI library search, fixed in `dd9dcc7e`.

Next, from the exit census on lexicon:

- megamorphic sites (all six entries full): probe the mega cache;
- `=` with Character operands inline;
- extended receiver stores (`ExtStoreRecv`, bytecode 243) emitted;
- block creation (`PushFullBlock`) without an exit;
- then the arm64 trims (lazy view, pinned registers, shared stubs).

## Status (2026-10-18)

- `cff75da2`: mega cache probe for full sites, extended receiver stores,
  `=` with a Character receiver (while `Character>>#=` is primitive 110),
  native calls at frame depth 0.
- `8a799eef`: super sends exit as sends (a method with a super send was a
  stub before), the interpreter's block-shape shortcuts are off under the
  mode (the blocks they answered never compiled), block local returns are
  native, `PushFullBlock` creates the closure without an exit.

x86 lexicon: 1138 billion instructions (from 1468).  Micro set (ms, x86
MCF): doBlock 564, dictAt 1599, strEq 680, ocAddRemove 592, collect 178,
detect 141, isKindOf 280 (Cog: 129, 308, 125, 107, 45, 41, 77).

- `d234756d`: super sends and refused arithmetic sends ask
  `pharo_mcf_extsend` for the target (`emitMcfHelperSend_x86`);
  `value:value:` and longer call blocks natively; `PushArray` without an
  exit; inline widowing at the native return; `ensure:` and `on:do:` are
  native-call targets.
- `de4b0e07`: a shared stub answers the common primitives on the caller's
  stack (`emitX86PrimStub`); `^` in a block returns from the home through
  `pharo_mcf_nlr`; Context-field methods compile.

x86 lexicon: 742 billion instructions, 43.6 s (117.5 s at the start; Cog
11.9 s).  Sweep X3 (`d234756d`): 27812 of 27996 pass.

A primitive can serve several arities: 170 is both `Character
class>>value:` and `Integer>>asCharacter`.  The stub checks the send's
argument count for every primitive (the arm64 stubs are built per arity).

Two more arch gates in shared code turned up after `de4b0e07`: methods
with primitive 198 or 199 (`ensure:`, `on:do:`) advertised no bytecode
map unless arm64 MCF was on, so every return into them left machine code;
and the IC patch set `kXGateOkBit` (bit 57, which arm64 MCF reads as the
multi-slot getter) whenever arm64 MCF was off.
