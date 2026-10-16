# Machine-code frames on x86-64

Status: started 2026-10-17.  Design: docs/mc-frames-design.md (arm64).

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
