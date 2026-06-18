# BUG-2 next-session handoff — x86 send-bearing inline-J2J `asString` corruption

**Goal:** make `PHARO_T1_X86_XMETHOD_SENDS=1` (send-bearing cross-method
inline-J2J) correct on x86, so send-bearing callees (e.g. `incs ^(self+1) max: 0`)
inline-J2J instead of round-tripping through C++. That unlocks the cfibs ~12x
speedup (leaf-only `cfibs28=174ms` vs the cfibx-shape ~14ms). The knob is
**default-OFF**, so the shipped leaf-only x86 JIT is unaffected — any fix here is
fully gated and safe to iterate on.

## TL;DR of the prior session (2026-06-18)

- The nested send-bearing **mechanism is SOUND** — `./run.sh clean` (xfib→xinc→xmax:,
  all inline-J2J'd, returning via the V2 prelude) computes `xfib10=143` correctly.
  No architectural rewrite needed. (This refutes the old "port arm64 saveless/blr"
  framing in `memory/jit-x86-sendbearing-two-bugs.md`.)
- The corruption is isolated to **one selector: `#asString`** (and `s*` has a
  second, unbisected one). `./run.sh corrupt` → a send-bearing `asString` resume
  **swaps a string-producing value to a CLASS** (`#capitalized` / `#asForm` DNU
  on `ByteString class`). Same signature as the LEAF cross-method receiver swap
  that was a GC-stale-pointer bug (fixed via literals=CM+16-fresh; see
  `memory/jit-x86-xmethod-receiver-corruption`).
- A real **latent retval bug was found+fixed** this session (commit `1094ed6f`):
  x86 `JIT_RESUME_CALL` dropped the retval, so the C++ resume path fed
  `resumeAfterCall` a garbage rax. NECESSARY but **NOT** the root — corruption
  persists with it in.

## Repro

```
cmake --build build-x86 --target test_load_image    # x86 JIT build (default flag=1)
cp /tmp/harness/Pharo-prepped.image /tmp/harness/Pharo.image   # if /tmp was reaped
scripts/bug2-asString-repro/run.sh corrupt    # -> #capitalized DNU on ByteString class
scripts/bug2-asString-repro/run.sh clean      # -> EVAL-RESULT='xfib10=143' (control)
```
Deterministic under `PHARO_DET_SCHED=1` (so lldb/traces don't move it — the
Heisenbug rule, CLAUDE.md). The `PHARO_T1_X86_J2J_SEL=<sel>` / `<pfx>*` knob
scopes inline-J2J EMISSION to matching compiled-method selectors, isolating a
chain from startup interference (AsmjitT1.cpp ~10874).

## DO NOT RE-CHASE (ruled out by measurement, prior session)

1. Architectural port (saveless/blr) — mechanism sound, all clean shapes pass.
2. The retval-pass — fixed (1094ed6f), still corrupts.
3. Literals staleness — V2 admit (AsmjitT1 ~3222) + resumeAfterCall (~3539) use
   FRESH calleeCM+16; the `literalsCache` read at AsmjitT1 ~1818 is V1 dead code.
4. `compiledMethodOop` staleness — same GC-updated field the WORKING leaf path uses.
5. Polymorphism — the IC probe guards the class (`cmp rdx,[rsi]; jne miss`, ~2973).
6. sp-residency — IDENTICAL corruption on build-x86 (=1) and build-x86-0 (=0).
7. ExtSend emit — `PHARO_T1_X86_EMIT_EXTSEND=1` doesn't change it.
8. Every clean SHAPE: `^self <0/1-arg-send>`, `^Class sel: self` (class-literal
   inner receiver), `^Class tailSend: self`, inner-send-bails-mid-method,
   4-deep nesting — ALL correct in isolation (`x*` scope).
9. The cursor-orphan at the precomputed-resume reset (Interpreter.cpp ~27257-59)
   alone — xfib→xinc→xbig (depth-1 inner-send bail) works.

## LEADING HYPOTHESIS — GC interaction

Tight-loop repros never GC; startup (the `asString` path) does. The LEAF variant
was exactly a GC-stale pointer. Suspect: a saved field (receiver, cursor, or a
resume address) that the x86 EMIT side caches goes stale across a GC between an
`asString` inline-J2J admit and its resume, OR the x86 in-JIT prelude restores
the wrong save (cursor vs entryCursor) only in `asString`'s real nesting shape.

## EXACT NEXT STEP (lldb)

`run.sh lldb` launches the corrupt repro under lldb (Rosetta x86). The `[ASRET]`
probe added at Interpreter.cpp ~20276 (ExitReturn, gated `PHARO_J2J_NEST_TRACE`)
does NOT fire — **so `asString` returns via a NON-`tryJITActivation` exit; find
that exit first** (likely the rj2j/chain ExitReturn ~21370 or the slow chain
loop). Move/duplicate the `[ASRET]` probe to the real exit, confirm it logs
`asString recv=ByteString class -> valCls=ByteString class` (returning the class).
Then:
1. Break where `asString`'s send-bearing inner send resumes (the in-JIT
   `resumeAfterCall`, AsmjitT1.cpp ~3524 — JIT code, no symbol: set a hardware
   bp on the resumeAfterCall address from a `PHARO_T1_DUMP_SEL=asString` dump,
   or break at the C++ precomputed-resume Interpreter.cpp ~27230 which `[RES]`
   shows active, conditional on `savedMethod` being one of the 5 asString oops:
   `0x2051abf30 0x205280390 0x205192550 0x2051fd458 0x2051d6d38`).
2. Dump `state.receiver`, `save.receiver`, `state.literals`, and the popped-save
   cursor pre/post the resume. Find where `ByteString class` enters.
3. Check whether a GC ran between admit and resume (compare a heap epoch / object
   address). If a saved pointer moved, that's the fix site (refresh it post-GC,
   like the leaf literals=CM+16-fresh fix).

## SUCCESS CRITERION

`run.sh corrupt` prints `EVAL-RESULT` (no DNU); then the full
`PHARO_T1_X86_XMETHOD_SENDS=1` startup completes clean; then cfibs SENDS=1 runs
fast (`/tmp/cogbench3.st`). Keep arm64 untouched (build battery==golden) and the
default x86 (build-x86, knob-off) battery==golden + SUnit A/B identical.

## Key code locations

- x86 send emit / cross-method admit / resumeAfterCall: `AsmjitT1.cpp` ~2951,
  ~3155, ~3524; V2 return prelude ~1751.
- C++ resume paths: `Interpreter.cpp` rj2j ~21359 (pins entryDepth/Cursor),
  ~21478 (JIT_RESUME_CALL), precomputed-resume ~27230 (cursor reset ~27257).
- `JIT_RESUME_CALL` / `JIT_CALL` macros: `JITState.hpp` ~440.
- Memory: `jit-x86-sendbearing-two-bugs`, `jit-x86-xmethod-receiver-corruption`.
