# x86 Cog gap — re-measured 2026-06-16 (cross-method default-on era)

Fair, same-machine, same-image (Pharo 13, `get.pharo.org/64/130+vm`), OPTIMIZED
build (`build-opt`, RelWithDebInfo) vs stock Cog x86 (`pharo-vm-Linux-x86_64-stable`,
v10.3.9). m6a.4xlarge (AMD EPYC 7R13). Both VMs pinned to core 0, min-of-9, thorough
warmup. Bench: `/tmp/cogbench3.st` (inline loops dodge the method-wrapped Time
artifact). arm64 column = local Apple-silicon, `build/test_load_image` (-O2) vs
macOS stock Cog.

## THE headline: the x86 tier-1 JIT is OFF BY DEFAULT

Runtime banner on the box:

    [JIT] x86_64 tier-1 JIT off by default (bit-rotted vs arm64;
          set PHARO_X86_JIT=1 to enable) — running interpreted

So EVERY default-config x86 run is the **interpreter**. JIT Stats confirm
`compiled: 0 methods` unless `PHARO_X86_JIT=1`. The memory's "cross-method
inline-J2J DEFAULT-ON" / "cfibx 8.7x" refer to the cross-method FEATURE being
default-on *within* the JIT — not to the JIT itself, which is gated off. Earlier
SUnit A/Bs run without the gate compared two interpreter configs (so "ON==OFF"
only validated the interpreter, not the cross-method JIT path).

## Numbers (ms; lower is better)

    bench       x86 interp  x86 JIT-on  x86 Cog   JIT/Cog   JITspeedup   arm64 JIT/Cog
    loopD100M      6736         672        226      3.0x      10.0x         0.55x
    loopS100M      7940        8206        254      32x        0.97x        0.44x
    fib33          1430         242         42      5.8x       5.9x         2.96x
    cfibx32        1220         187         32      5.8x       6.5x         2.70x
    cfibs32        1584        2208         41      54x        0.72x        3.18x

  loopD = `[1 to: 1e8 do: [:i | i + i]]`         (discard; no closure write)
  loopS = `[s:=0. 1 to: 1e8 do: [:i | s:=s+i]]`  (closure-temp write each iter)
  fib33 = self-recursive benchFib
  cfibx = recursive fib calling `incc ^self+1`   (cross-method, nArgs 0, SmI recv)
  cfibs = recursive fib calling `incs ^(self+1) max: 0` (cross-method + `max:` send)

## What the x86 JIT does and does NOT accelerate

WORKS (JIT clearly faster than interp, lands 3-5.8x Cog):
- loopD: 10x over interp → 3.0x Cog. Plain inlined `to:do:`.
- fib33 (self-rec inline-J2J): 5.9x → 5.8x Cog.
- cfibx32 (cross-method inline-J2J, the 2026-06-16 literalsCache fix): 6.5x →
  5.8x Cog. cfibx=187ms is exactly the memory's prior "185ms" — the fix is real
  *when the JIT is enabled*.

REGRESSES (JIT same-or-slower than interp; 32-54x Cog):
- cfibs32: JIT 0.72x = SLOWER than interpreter (1584→2208). The callee does a
  `max:` send (not a bare arith leaf like cfibx's incc).
- loopS: JIT 0.97x = no benefit (7940→8206). The loop body writes a closure temp
  each iteration.

## Diagnosis

The regressions are the documented x86 caller-resume bit-rot (`debug_vars.h`):
`PHARO_T1_NO_CALLER_RESUME` — "the x86 tier-1 caller-resume re-entry has a
pre-existing ~1-word-per-send operand-stack leak (frame-state-residency protocol
bit-rotted vs arm64 V2)". When a method's post-send continuation must re-enter
JIT with live operand-stack state (cfibs's `max:` result, loopS's accumulator),
the x86 path either strands in the interpreter or thrashes — so those cases get
no speedup or regress. The arm64 V2 frame-state-residency path (FSR M2 cursor
residency, resume-sends, PMS) is what makes arm64 0.44-3.18x across the board;
x86 never got the working V2 port. THIS is why the JIT is gated off by default.

## The lever toward x86 Cog parity

Port arm64's V2 frame-state-residency caller-resume to x86 (the
`PHARO_J2J_SAVE_V2` path is already on for x86 self-rec; the gap is the post-send
operand-stack resume protocol). Once cfibs/loopS stop regressing and match the
cfibx/fib quality (~3-5x Cog), the `PHARO_X86_JIT` gate can flip default-on.
Target: arm64-parity (sub-3x Cog) on all five benches.

Harness: `scripts/aws/sunit-ab.sh` style; bench `/tmp/cogbench3.st`; stock Cog via
`get.pharo.org/64/130+vm` (NOT `/64/vm`, which returns an HTML error page — the
historical install blocker). Run our VM with `PHARO_X86_JIT=1` or every number is
the interpreter.
