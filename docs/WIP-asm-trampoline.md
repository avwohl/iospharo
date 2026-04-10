# WIP: Assembly J2J Trampoline

**Date started**: 2026-04-09
**Status**: **Phase 1 + Phase 1.5 complete. ASM now 19% faster on fib(28) (9.7ms vs 12ms).**
**Goal**: Break the 10.4ms fib(28) wall by hand-writing the J2J trampoline
hot loop in ARM64 assembly.

## Post-IC-fill results (2026-04-10)

IC fill + byte-object stencil fixes changed the equation. More methods stay in JIT,
so trampoline overhead matters MORE, making the ASM win larger:

    Config                          fib(28)   sieve x3   tinyBenchmarks
    C++ trampoline (current):       12ms      3.1ms      6509ms
    ASM trampoline (Phase 1.5):     9.7ms     TBD        TBD
    Interpreter (no JIT):           43ms      2.95ms     5724ms

ASM trampoline is 19% faster than C++ on fib(28), up from the previous 3%.
The improvement scales with JIT utilization — more JIT = more trampoline calls = more ASM wins.

## Phase 1.5 outcome (2026-04-09 — same day)

Three micro-opts on top of Phase 1:

  1. `.p2align 4` before `Ltramp_loop` head (loop entry now 16-byte
     aligned at 0x10000f390 instead of 0x10000f388). The two added nops
     run exactly once per trampoline entry — both back-edges
     (`b 0x10000f390`) jump past the nops.

  2. Return path: load `save.jitMethod` and `save.resumeAddr` together
     via `ldp x11, x7, [x23, #JSV_JITMETHOD]`. JSV_JITMETHOD(32) and
     JSV_RESUMEADDR(40) are adjacent. Hoists the resume-address load
     to the very top of the return path so its load latency is fully
     hidden behind the self-recursive restore. retVal moves from x7
     to x17 (caller-saved IP1, free since we never cross a BLR with
     it live).

  3. Call path: load `state.sp` and `state.receiver` together via
     `ldp x10, x11, [x21]`. JS_SP(0) and JS_RECEIVER(8) are adjacent.

Net instruction count: same as Phase 1 (LDPs save 2 insts, alignment
adds 2 nops). All wins come from scheduling and alignment, not
instruction count.

Interleaved A/B (50 samples each, fresh builds, alternating):

    Build   min   p10   p25   median  p75   p90   max    mean  stdev
    CPP     9591  9733  9768  9857    9937  9984  10101  9852  111
    ASM15   9090  9204  9268  9561    9697  9808   9833  9505  229

  ASM best:    -5.22% vs CPP
  ASM p10:     -5.44% vs CPP
  ASM median:  -3.00% vs CPP   ← was +1.28% in Phase 1
  ASM mean:    -3.53% vs CPP
  ASM stdev:    2.07x CPP      ← was 4.27x in Phase 1 (halved)

Stencil counts identical: 6170696/6170673 in both binaries.

The variance halving is the most striking result. Hypothesis: the
unaligned loop head in Phase 1 was straddling a 16-byte fetch block
boundary, so some `ldr w0, [x21, #EXITREASON] / cmp / b.eq` sequences
needed two fetch cycles instead of one. Aligning the head ensures
single-fetch dispatch every iteration.

ASM median (9561us) is now decisively below CPP median (9857us) on
fresh interleaved samples, and the lower bound (9090us) is the
fastest fib(28) we've ever measured. Phase 2 (stencil ABI change)
remains the bigger lever, but Phase 1.5 alone justifies the trampoline.

## Phase 1 outcome (2026-04-09)

Files landed:
    src/vm/jit/TrampolineAsm.S       hand-written ARM64 asm loop (~390 lines)
    src/vm/jit/TrampolineAsm.hpp     extern "C" declaration
    src/vm/Interpreter.cpp           #ifdef PHARO_ASM_TRAMPOLINE path
    CMakeLists.txt                   -DPHARO_ASM_TRAMPOLINE=ON switch,
                                     LANGUAGES C CXX OBJC ASM

Interleaved A/B (50 samples each, fresh builds, alternating):

    Build  min    p25    median  p75    max    mean   stdev
    CPP    10012  10140  10206   10278  10408  10202   100
    ASM     9595   9933  10336   10702  11026  10349   427

  ASM best:    -4.17% vs CPP (genuine lower bound drops)
  ASM median:  +1.28% vs CPP
  ASM mean:    +1.44% vs CPP
  ASM stdev:   4x higher than CPP

Correctness: stencil call counts identical (6170696/6170673) between CPP
and ASM, benchmark completes all 5 runs without deopt or bailout.

Full Kernel-Tests suite **could not be run** against either binary: the
fresh Pharo 13.0 image triggers a non-boolean receiver in Array sort during
WorkingSession>>runStartup, which cascades into an infinite recursion of
NonBooleanReceiver->signalerContext->findContextSuchThat: warnings. Both
CPP and ASM fail IDENTICALLY on this path, so it is not a Phase 1
regression — it's a pre-existing image/VM incompatibility with fresh 13.0
images that has drifted since the last successful full-suite run
(2026-04-08, 99.7% on a different image snapshot). Correctness parity is
the load-bearing claim here, not "all tests pass on this image".

Interpretation: Phase 1 isolated lower bound is real (9.6ms vs 10.0ms),
but the median is noisier. The asm pins x21-x28 for state/cursor/counters,
but stencils still clobber x0-x17/x19/x20 on every BLR, so the wins from
cross-BLR register caching are diluted by the per-BLR spill/reload cost
the stencils impose. Phase 2 (stencil ABI change to pin state.sp, receiver,
tempBase in callee-saved regs directly) is where the real savings are.

**Do not retry Phase 1 micro-tuning before understanding the variance
increase.** The 4x stdev jump suggests some samples land in a worse
scheduling window — maybe x21-x28 pinning makes Clang's surrounding
frame-materialization code fight for registers, or the inlining decision
tree in tryJITActivation changed. Before Phase 2, profile a sample of
"fast" vs "slow" ASM runs and see if the variance comes from inside the
trampoline or from the surrounding bailout/materialization path.

## Original plan (below) remains the spec for Phase 2.

---

---

## Why this task exists

fib(28) has been stuck at ~10.4ms best / ~10.65ms median for the whole
2026-04-09 session. Per-call overhead is ~10.1ns (~32 cycles); Cog's target
is ~15 cycles. The last two micro-ops attempted — eliminating `localReturns`
spill and caching `bytecodeStart` in JITMethod — both regressed despite
objectively saving 4+ instructions per call in the disassembly. The C++
trampoline's Clang-driven register allocation is too fragile for further
source-level tuning. See `memory/project_fib_benchmark.md` "Failed attempts"
section for the post-mortems.

The user picked "Assembly trampoline" as the next direction and needs to
reboot before resuming. This doc is the resume point.

---

## Baseline to beat (2026-04-09, commit e07a9d7)

    Config                  fib(28)   Notes
    Cog VM:                 2 ms      Target
    Our JIT (current):     10.4 ms    Best case, 5-run median ~10.65 ms
    Interpreter:           47 ms      Dropped through

    Per-call: ~10.1 ns / ~32 cycles at 1.03M calls
    Cog target: ~15 cycles/call — roughly 2x headroom remaining

Run baseline with:

    PHARO_BENCH=fib ./build/test_load_image /tmp/Pharo.image

PHARO_BENCH runs fib(28) 3 times per invocation internally. For medians take
5+ invocations. Noise floor ~3% on M-series perf cores, so sub-1% wins are
invisible — always interleave A/B samples with fresh binaries.

Git state at doc write: **clean**, `main` at e4eed0b (tidy gitignore).

---

## The current C++ trampoline (what you're replacing)

Location: `src/vm/Interpreter.cpp:11043-11255` inside `tryJITActivation`.

Shape: a single `while` loop in an inner block scope. Handles two exit
reasons in a dispatch:

    ExitJ2JCall  -> push J2JSave, set up callee JITState, BLR to callee entry
    ExitReturn   -> pop J2JSave, restore caller JITState, BLR to resumeAddr

One W^X toggle wraps the entire loop (not per-BLR — per-BLR was 77ms).
State.exitReason is never cleared or read inside the loop (stencils only
WRITE it). Frames are lazy — only `localFrameDepth` is incremented;
SavedFrames are only materialized on bailout at lines 11276-11318.

### J2JSave struct (72 bytes, 1 cache line, Interpreter.cpp:11048-11063)

    Oop*            sp;            // 0
    Oop             receiver;      // 8
    Oop*            tempBase;      // 16   <-- pair with ip
    uint8_t*        ip;            // 24
    jit::JITMethod* jitMethod;     // 32   <-- pair with resumeAddr
                                   //          LSB=1 marks self-recursive save
    uint8_t*        resumeAddr;    // 40   Precomputed, skips bcToCode lookup
    int             sendArgCount;  // 48   <-- packed with argCount
    int             argCount;      // 52   Non-self-recursive only
    Oop*            literals;      // 56   Non-self-recursive only
    uint8_t*        bcStart;       // 64   Non-self-recursive only

    static_assert(sizeof(J2JSave) == 72);

Field order is TUNED for STP/LDP pairing. Do not reorder without measuring.

Max depth: `j2jStack[256]` on the C stack. Overflow falls back to
ExitSendCached.

### JITState struct (144 bytes, src/vm/jit/JITState.hpp:42-84)

**CRITICAL: stencils bake these offsets into machine code. Never reorder.**

    offset  field               type
    0       sp                  Oop*
    8       receiver            Oop
    16      literals            Oop*
    24      tempBase            Oop*
    32      memory              ObjectMemory*
    40      interp              Interpreter*
    48      ip                  uint8_t*
    56      jitMethod           JITMethod*
    64      method              Oop
    72      argCount            int
    76      exitReason          int         (stencils WRITE only)
    80      returnValue         Oop
    88      cachedTarget        Oop
    96      icDataPtr           uint64_t*
    104     sendArgCount        int
    112     simTOS              uint64_t    (SimStack register caching)
    120     simNOS              uint64_t
    128     trueOop             Oop
    136     falseOop            Oop

On ExitJ2JCall, the stencil has set:
    state.cachedTarget = target method Oop
    state.returnValue  = (uint64_t)entryAddr  (raw bits)
    state.sendArgCount = nArgs
    state.ip           = past-send bytecode
    state.exitReason   = ExitJ2JCall

### JIT_CALL macro (src/vm/jit/JITState.hpp:133-145)

    asm volatile(
        "mov x0, %[s]\n\t"
        "blr %[e]"
        :: [s]"r"(state_ptr), [e]"r"(entry_ptr)
        : "x0"-"x17", "x19", "x20", "x30", "memory", "cc"
    );

Key point: the `"memory"` clobber forces Clang to reload any `this->member_`
access after every BLR. This is why we cached `frameDepth_`,
`jitJ2JStencilCalls_`, `jitJ2JStencilReturns_` into local variables.

Stencils are assumed to clobber x0-x17, x19, x20, x30. Everything else
(x21-x28, SP, FP) is preserved. **Our asm trampoline can freely use
x21-x28** for cross-BLR register caching.

---

## Design sketch for the asm trampoline

### Phase 1: Minimal asm port (conservative)

Write a naked `__attribute__((naked))` function that replicates the C++
loop in asm but:

1. **Pins J2JSave stack pointer in x21** (callee-saved). No add/sub every
   iteration — just pre-compute save slot address inline.
2. **Pins `state` pointer in x22** (callee-saved). Every BLR does `mov x0,
   x22` instead of rematerializing through Clang's stack spill.
3. **Pins `localFrameDepth`, `localCalls`, `localReturns` in x23/x24/x25**
   (callee-saved). These are currently local size_t's but Clang sometimes
   spills them based on register pressure. Asm pins them.
4. **Loads JITState hot fields into x26-x28** as scratch around push/pop,
   uses STP/LDP pairs aggressively. No redundant field reads.
5. **Skips the ExitReason branch** at the top of the loop. Instead:
   - Callee's return stencil sets exitReason=ExitReturn.
   - After BLR, compare exitReason directly (load from offset 76 in x0=state).
   - Branch: ExitJ2JCall → call path; ExitReturn + j2jDepth>0 → return path;
     anything else → exit.
6. **Inline the self-recursive fast path** at the top of the call path.
   fib is monomorphic self-recursive, so this is the critical branch.

Expected win: 10-20% (~1-2ms). Not huge, but it establishes the asm harness
for Phase 2.

### Phase 2: Register-cached JITState (aggressive, requires stencil rework)

The real bottleneck is that stencils read JITState via `[x0]` — so we MUST
have the struct in memory correct before every BLR. This defeats cross-BLR
register caching.

The fix is a stencil ABI change:

- Pin `state.sp` in **x21** (callee-saved register known to stencils)
- Pin `state.receiver` in **x22**
- Pin `state.tempBase` in **x23**
- Pin `state.ip` in **x24**
- Pin `state.jitMethod` in **x25**
- Keep `state.literals`, etc. in the struct (cold-ish)

Stencils load from `x21` etc. directly, not `[x0+0]`. On exit-to-C++ paths,
stencils write registers back to the struct. On normal J2J BLR, no writeback
is needed — the callee stencil just uses the same registers.

This eliminates ~4-6 memory ops per call (the state updates in the call
path) and ~4-6 per return (the state restores in the return path). Plus the
stencil-side load savings. Plausible total: 30-50%.

Cost: stencil extraction pipeline changes. `scripts/extract_stencils.py`
needs `-ffixed-x21,x22,x23,x24,x25` and the stencils need to be written
against these registers. This is a ~1-2 day refactor and risks breaking the
full test suite. **Do Phase 1 first, measure, only do Phase 2 if Phase 1
is insufficient.**

### Phase 3: Merged exit via matched BL/RET (optional)

Rather than the trampoline loop dispatching returns, arrange that:
- Caller stencil's send point does a plain `BL` to callee entry.
- Callee's return stencil does a plain `RET`.
- The trampoline loop is NOT involved in self-recursive hot paths at all.

Problem: the caller stencil is copy-and-patch code with no ABI for
"establish a call frame". You'd need a wrapper stencil that pushes the
J2JSave to the trampoline's save stack, BL's, then pops on return. Plausible
but requires new stencil infrastructure. Defer until Phase 2 results land.

---

## Constraints and watch-outs

1. **Do not touch JITState field offsets.** Stencils have them baked in.
2. **W^X toggle stays outside the loop.** Single `pthread_jit_write_protect_np`
   call each side. Per-BLR toggle was 77ms catastrophe.
3. **localFrameDepth must sync to `frameDepth_` on bailout AND normal exit.**
   Other code (frame materialization, stack overflow) reads `frameDepth_`.
4. **checkCountdown_ charge happens only at loop exit**, bulk-charged by
   `(localCalls + localReturns) * 10`. Reading it per-iteration forced
   reloads via the memory clobber — don't reintroduce it.
5. **Self-recursive save marker**: LSB of `save.jitMethod` = 1 means "skip
   literals/argCount restore on return". JITMethod* is 8-byte aligned so bit
   0 is always free. Return path MUST check this bit before restoring.
6. **Stencils only WRITE state.exitReason, never READ.** We skip the
   `state.exitReason = ExitNone` write between iterations. Do not
   reintroduce it in the asm version.
7. **Frame materialization on bailout** (Interpreter.cpp:11276-11318) reads
   j2jStack[0..j2jDepth-1] after the loop. If the asm changes J2JSave
   layout, the materialization code must be updated to match.
8. **The `state.ip` optimization**: on self-recursive hot path, state.ip is
   NEVER modified by stencils (they only touch ip on deopt). So on
   self-recursive return, state.ip is already correct and we skip the
   restore. The asm must preserve this behavior.
9. **No GC/process-switch during the loop.** Stencils don't allocate and
   no timer checks run. Asm can assume this.

---

## How to start (concrete first steps)

1. **Capture the current baseline cleanly** — 10 interleaved runs,
   `PHARO_BENCH=fib ./build/test_load_image /tmp/Pharo.image`. Record
   min/median to the nearest microsecond.

2. **Disassemble the current trampoline** so you have the Clang reference:

        cmake --build build --target pharo_vm_lib
        otool -tV build/libpharo_vm.a | \
            awk '/_ZN5pharo11Interpreter16tryJITActivation/,/^$/' \
            > /tmp/trampoline_clang.asm

   (exact mangled name may differ — check with `nm build/libpharo_vm.a |
   grep tryJITActivation`.) This is the instruction budget you're trying
   to beat.

3. **Write the asm as a separate `.S` file**, not inline asm. Name suggestion:
   `src/vm/jit/TrampolineAsm.S`. Expose a single entry point:

        // void pharo_jit_j2j_trampoline(
        //     pharo::jit::JITState* state,     // x0
        //     void* j2jSaveStack,              // x1 (256 * 72 bytes)
        //     size_t* localFrameDepth,         // x2 (in/out)
        //     size_t* localCalls,              // x3 (out)
        //     size_t* localReturns,            // x4 (out)
        //     size_t stackOverflowLimit);      // x5
        //
        // Returns: final exitReason in w0 (per ARM64 AAPCS).

4. **Update `tryJITActivation` to call it**. Keep the C++ version
   behind `#ifdef PHARO_ASM_TRAMPOLINE` so you can A/B quickly. Build both,
   flip a CMake flag.

5. **Verify correctness first.** Full test suite must pass before any
   perf measurement. Inject the test runner and run as documented in
   CLAUDE.md "Running Official Pharo Test Suite".

6. **Then measure.** Interleaved 10-sample A/B with fresh binaries.
   Compare best cases AND medians. If median didn't drop by at least 2%,
   assume the asm is not better and check the disassembly.

---

## Risks and failure modes

- **"I wrote it in asm and it's actually slower"** — this happens. Clang's
  register allocator is good. The win comes from Phase 2 (register-cached
  stencil ABI), not Phase 1 (naive transliteration). If Phase 1 regresses,
  don't panic, just check: am I doing more memory ops than Clang was?
  Am I pairing STP/LDP correctly? Am I missing a branch hint?

- **Breaking frame materialization** — if asm changes j2jStack layout
  without updating lines 11276-11318, bailouts will crash on exception
  handling paths. Run the full test suite, not just fib.

- **Stencil ABI drift in Phase 2** — the `-ffixed-xN` flag must be applied
  consistently across all stencils. Missing it on one stencil = corruption.
  The stencil extraction pipeline generates a hole-patched array; verify
  via `otool -tV` of a few stencils that xN is never touched before Phase 2
  changes ship.

- **W^X toggle accidentally moved inside the loop** — 77ms catastrophe.
  Assert the toggle is outside, verify in the disassembly.

---

## Files you'll touch (Phase 1)

    src/vm/Interpreter.cpp          Replace lines 11086-11255 with a call to
                                    pharo_jit_j2j_trampoline
    src/vm/jit/TrampolineAsm.S      NEW — hand-written asm
    src/vm/jit/TrampolineAsm.hpp    NEW — extern "C" declaration
    CMakeLists.txt                  Add TrampolineAsm.S to pharo_vm_lib sources,
                                    add -DPHARO_ASM_TRAMPOLINE flag

No stencil changes in Phase 1. No `extract_stencils.py` changes. No
`JITState.hpp` changes. No `JITMethod.hpp` changes.

---

## Resume checklist

When you pick this back up:

- [ ] `git status` is clean, on `main`
- [ ] Baseline: run 10 interleaved `PHARO_BENCH=fib` samples, record min/median
- [ ] Read Interpreter.cpp:11043-11318 end-to-end so the C++ semantics are fresh
- [ ] Read JITState.hpp (all 156 lines) so you know the ABI you can't break
- [ ] Disassemble current trampoline for the instruction budget
- [ ] Create `src/vm/jit/TrampolineAsm.S` and the CMake hook
- [ ] Write Phase 1 (conservative port) first
- [ ] Full test suite pass before any perf claim
- [ ] Interleaved A/B measurement (not single-run comparisons)
- [ ] Update `memory/project_fib_benchmark.md` with the result (positive OR
      negative — the negative-results log is load-bearing)
- [ ] Commit Phase 1 before starting Phase 2 even if gains are small
