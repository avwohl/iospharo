# JIT TODO

Consolidated from `jit-punchlist.md`, `jit-t2-chainloop-plan.md`, and
`jit-j2j-reduction-plan.md` (all three deleted after merging). Reflects
the state after 2026-04-17 session.

---

## Status at a glance

**Shipped** (stability + correctness):
- IC layout GC scanner fix (`fd03572`)
- JITState as GC root (`a311688`)
- J2J literals pointer +8 → +16 fix (`2b1629f`)
- SimStack default off (`b9ab22e`)
- T1 J2J reduction / A2 — 72→56 byte save struct (`415d899`)
- **MIR removed, asmjit vendored** (`d10b00a`, `0cdd738`, `7a5061b`)
- **asmjit T2 MVP**: compiles leaf methods — getters, setters, constant
  returns (`3cf135c`, `1ed9590`)

**Default config:** T1 JIT on, T2 off, SimStack off, asmjit as the T2 backend.

**asmjit T2 coverage (~5-15% of candidates depending on workload):**

    Return-only    0x58/59/5A/5B            ^ self/true/false/nil
    Push-return    0x4C-0x4F + 0x5C         ^ self/true/false/nil (2-byte)
    Push-return    0x50/0x51 + 0x5C         ^ 0 / ^ 1
    Push-return    0x20-0x3F + 0x5C         ^ literal[N]
    Push-return    0x10-0x1F + 0x5C         ^ globalVar                    (assoc.value)
    Getter         0x00-0x0F + 0x5C         ^ instVar[N]
    Setter         0x40 + 0xC8-0xCF + 0x58  ivar := arg; ^ self
    Init-const     {0x4D-0x51} + 0xC8-0xCF + 0x58  ivar := const; ^ self

Gated off: any pattern that contains a send.  Tried the "push + exit
with ExitSend" approach — correctness is fine, but the round-trip to
the interpreter for each send is ~1.8× slower than T1 staying inline.
Any real send support needs an inline IC check (6-way probe + direct
J2J call on hit) — see task #31.

Perf impact today: negligible.  MVP proves the asmjit pipeline;
coverage doesn't overlap the hot bytecodes.

**Measured perf (prior MIR T2):** JIT was **net-negative** on arith-heavy
loops (2.5× slower than interpreter at 3M iterations; MIR T2 was 12×
slower).  The asmjit replacement starts from the same baseline; the
payoff requires adding send / arith support.

---

## Open bugs

### B1. `bench_loop.st` intermittent hang
Non-deterministic: ~3/5 runs hang at `n=16` on `SmallInteger>>benchmark`
in a doubling loop. Timing-sensitive — probably depends on which methods
reach the JIT compile threshold during which iteration. The SimStack-off
commit helped on some paths but didn't eliminate the race.

**Next step:** `lldb` breakpoint on `sendMustBeBoolean` during a failing
iteration. Inspect the IC entry that fed nil where a boolean was
expected. See `memory/project_tinybench_jit_hang.md`.

### B2. benchFib 2nd-iteration 12× slowdown
`28 benchFib` = 57ms (first call). `29 benchFib` right after = 1065ms
(12× slower than the 92ms it takes in isolation). Something about the
first call JIT-compiling `benchFib` makes subsequent recursive calls
drastically slower. Not a hang, just a regression.

**Next step:** compare the 1st-call JIT stats (IC state, J2J-d count)
with the 2nd-call state. Likely an IC entry cached during the first
call that's wrong for the recursive path.

### B3. `tinyBenchmarks` scheduler-idle hang
After `bench_loop` finishes but before the send-throughput measurement,
the scheduler goes idle and never wakes. Probably a consequence of B2
blowing past the timing budget. Fixing B1 + B2 likely fixes this.

### B4. Latent MIR codegen SIGSEGV on specific methods
MIR's `generate_func_code` crashes compiling `#position:` and similar.
Caught by the signal guard (`9ffa5f7`) so it no longer takes the VM
down, but those methods fall through to T1 forever. If the method is
hot, that's a lost perf opportunity.

**Next step:** narrow down the bytecode pattern that MIR can't handle
(probably something rare — only seen once per run). Either fix MIR or
teach T2 compiler to bail pre-emptively on that pattern.

### B5. IC hit rate on Permute / send-heavy benchmarks
50.6% IC hit rate with only 51 IC patches across the benchmark is
suspicious. Either IC is cold (patches never applied at warm sites) or
the mega-cache is being skipped. See
`memory/project_ic_hit_rate_investigation.md` and
`memory/project_ic_selbits_mystery.md`.

**Next step:** add a mega-cache hit counter next to the existing IC
hit counter. Split misses by cold vs polymorphic vs `icData[18] == 0`
(already landed — noSelBits, cold, poly counters).

---

## Performance work (ordered by ROI)

The honest picture: our stencils average ~460 bytes per bytecode.
`stencil_sendJ2J` alone is 523 ARM64 instructions (~2KB). Every send
copies this. A 29-bytecode benchmark method is 13,304 bytes of JIT
code. That busts M1's 128-byte i-cache lines on every dispatch. The
architecture is wrong for bytecode-heavy code; the interpreter's 20-50
bytes per handler stays cache-hot.

### P1. Sista-style method inlining (biggest win)
Inline getter/setter/simple-arith methods at T2 compile time. Eliminates
per-send boundary cost entirely for inlined hot methods. This is THE
change that closes the Cog gap.

**Scope:** multi-session (2-3 sessions). Needs deopt support so T2 can
speculate on receiver class and bail back to T1 on misses.

### P2. Shrink `stencil_sendJ2J`
523 instructions → target 150-200. Extract the IC probe loop and
megamorphic fallback into a shared helper function. Tail-call the
helper instead of inlining 6-entry unrolled probes. Saves ~2KB per
send site, improves i-cache hit rate.

**Scope:** 1-2 sessions. Needs careful benchmarking — if the helper's
call overhead exceeds the icache win, it's a wash.

### P3. Method-level opt-in instead of global threshold
Currently any method crossing the executionCount threshold gets
JIT-compiled. For arith-dominated methods (`sum:do:`, `Integer>>to:do:`),
JIT is slower than interpreter. A simple heuristic: only compile
methods with ≥N sends. Arith-loop methods stay interpreted (faster),
send-heavy methods benefit from IC caching.

**Scope:** half session. Low risk, modest upside.

### P3b. Cache state.sp in a dedicated register across the hot loop
(discovered from native-dump inspection 2026-04-17)

`reg_sp_` gets spilled to `state.sp` whenever any path in the method
stores it (e.g., every emitSendExit or flushVStack). In benchmarks
with slow-path arith exits, MIR has to spill/reload sp on every
iteration of the main loop, even if those slow paths never execute.

**Attempted 2026-04-17, FAILED:**  moved the `state.sp = reg_sp_`
write out of the jumpTrue/jumpFalse/jump backward-branch yield check
so only the yield-taken branch writes state.sp. Fast-path just
branches to the loop head with reg_sp_ live in register. Result: T2
hangs on `sum 3M`. Likely root cause: MIR's post-send reload of
reg_sp_ from `state.sp` (in emitSendCall continuation) creates a
data dependency with the state.sp memory; removing the back-edge
write leaves state.sp stale when the fast-path feeds into a reload.
Reverted.

Remaining possibilities:
- Remove the explicit `state.sp = reg_sp_` writes from emitSendCall
  too, trust that MIR's register allocator keeps reg_sp_ across
  the C call (requires verifying MIR's ABI treats the MIR-reg as
  callee-saved for our function).
- Use a separate MIR register for sp that's NEVER stored to
  state.sp except at true exits. Require reg_sp_ to be the "live"
  sp, with explicit `state.sp = reg_sp_` only at RET points.
- Investigate MIR's hard-register attribute.

Perf target: ~10-20% win on loop-dominated benchmarks. Requires
deeper MIR internals knowledge than I had this session.  Next
attempt should start by reading MIR's register allocator docs /
source to confirm the reg-vs-memory model for MIR_T_I64 locals.

### Also tried + reverted this session (2026-04-17):
- `PHARO_T2_OPT=2/3`: level 2 gives 1250ms vs level 1 1162ms on
  sum 3M — higher opt levels DON'T help (level 3 hangs, level 2
  slightly worse).
- Dirty-temp writeback: hangs on sum 3M. Interaction with send
  bail / reload paths.
- `upgradeICToJ2J` layering: regression on bench_loop.
- Inline fast-reject at tryJITActivation call site: no measurable
  change.

### P4. Fix `upgradeICToJ2J` to layer J2J on existing extras
IC entries with inline-primKind bits (52:48) never get the J2J direct-
call bit (60) added later. My attempt at this caused regression
(intermittent hang at n=16). The code change itself is 5 lines but
interacts with stencil IC hit paths in ways I couldn't trace without
lldb.

**Scope:** half session once B1 is debuggable.

### P5. Re-enable SimStack TOS/NOS caching
~5-10% win on arith-heavy code once B1 is resolved. SimStack is
disabled by default because of a timing-sensitive correctness bug in
arith-jump chains after hot loops.

**Scope:** depends on B1.

### P6. Reduce `tryJITActivation` fast-reject overhead
Current stats: ~10% hit rate on `tryJITActivation` calls. 90% of
activations reach the function and bail at the "method not compiled"
check. Cheap to fix: inline the check at call sites, saving function
call overhead on the common miss path.

**Scope:** 1-2 hours. Small but measurable win.

### P7. Enable A1 (T2 chain-loop)
Already shipped, gated behind `PHARO_T2_A1=1`. Can't enable until T2
itself gets faster than T1 (P1 or P2 above). Today T2 is 5× slower
than T1, so A1 gains nothing.

---

## Architectural notes

### T2 inner-loop inspection findings (2026-04-17)

Dumped the native code for `DoIt` running `1 to: 3M do: [:i | sum := sum + i]`
via `PHARO_T2_NATIVE_DUMP=DoIt`. Loop body is 214 ARM64 instructions
(for 3 sends + arith). 3M iterations × 214 = 642M instructions; at
IPC 1.5 that's ~140ms minimum just for instruction throughput. We
measure 1170ms. Gap = ~1030ms of stalls / memory / miss costs.

Key codegen weaknesses visible in the native dump:

1. **MIR uses indexed addressing instead of immediate offsets.**
   State-struct field access compiles to `add x1, x19, #off; ldr xN,
   [x1, xzr, lsl #3]` — 2 instructions where 1 (`ldr xN, [x19,
   #off]`) would suffice. On M1 these don't always fuse to a single
   μop so we pay ~2 cycles per field read.

2. **`state.sp` is reloaded from memory every iteration.**  At the
   loop head we see `ldr x0, [x19, xzr, lsl #3]` (load sp from
   state), and at the end `str x0, [x19, ...]` (write sp back).
   Ideal code keeps sp in a callee-saved register for the entire
   loop. MIR's register allocator is forced to spill it because
   slow-path exits write `state.sp = reg_sp_`; the spill then
   pollutes the fast path.

3. **SmallInt constants are materialized via 2-3 mov/movk per use.**
   E.g. `mov x1, #0x1658; movk #0x6bac lsl16; movk #0x4 lsl32` for
   each Oop literal. MIR has no literal pool for tagged immediates
   on ARM64, so heavy-literal methods emit many movk chains.

4. **Helper function address reloaded per call site.**  Every call
   to `jit_t2_send` re-materializes its 48-bit address with three
   movk instructions, then `blr x1`. A shared helper pointer slot
   + `ldr x1, [base, #off]` would be fewer bytes and possibly
   predictable.

5. **Per-pop `sp -= 8` + writeback.** Explicit `sub x0, x0, #8;
   str x0, [x19, ...]` at every stack pop. For a fast-path arith
   comparison that doesn't touch the Smalltalk stack (values are
   in vstack registers), this is pure overhead.

### Why stencils are this large

Each stencil has prologue/epilogue + per-op logic + tail-call to next
stencil. The prologue/epilogue is ~30-40 bytes each side = 60-80 bytes
of pure overhead per bytecode. The tail-call is an indirect branch
through a patched pointer — unpredictable, saturates branch predictor.

Cog's approach: generate a single function per method with register
allocation across bytecodes. No per-op prologue/epilogue, direct
local branches, register values stay in registers. Each bytecode
translates to ~5-10 native instructions, not ~50-100.

### Why T2 didn't help

T2 (MIR-based) should have solved this — MIR generates whole-method
code with register allocation. But our T2 currently:
- Still goes through `jit_t2_send` (C function call) for every inline
  send that isn't the arith fast path
- Pays a big prologue to set up `JITState`-accessible environment
- Reloads `sp/receiver/literals/tempBase/trueOop/falseOop` after
  every send call

Net: T2's theoretical register-allocation win is swamped by the
per-send C-boundary cost. Fixing this = P1 (method inlining).

---

## Recommendation (what I'd do next)

**If the goal is "make the JIT not net-negative":**

1. **P3 (method-level opt-in).** Half a session. Measurably reduces
   the regression on arith workloads without touching architecture.
   Once in place, the worst-case regression goes from 2.5× to ~1.1×.

2. **B1 / B2 via `lldb`.** The hangs and the benchFib slowdown are
   probably the same root-cause — a stale IC entry in the recursive
   path. Fixing those unblocks P5 (SimStack) and P4 (J2J upgrade),
   which together could flip the JIT from net-negative to net ~1.2×.

**If the goal is "close the gap with Cog":**

3. **P1 (Sista-style inlining).** Multi-session but this is the only
   change with the potential to actually beat Cog-ish performance.
   Without it, we're stuck doing cache-unfriendly stencil dispatch.

**If the goal is the project's actual mission (iOS):**

4. **Pivot.** Mac Catalyst works. iOS device testing needs physical
   hardware + Apple Developer signing — blocked on things outside
   the VM. The JIT perf work has eaten 20+ sessions and the return is
   now actively negative. The VM + interpreter is correct and fast
   enough; ship it on iOS and treat JIT as a future optimization.

My vote: **P3 first** (cheap correctness-safe win), then **pivot to iOS**
if the goal is shipping a product rather than matching Cog.
