# Plan: T1 JIT send-path — close the 3.5x-vs-Cog gap (corrected)

Scoped via a 6-agent design workflow + adversarial verify (returned **needs-
revision**; this is the CORRECTED plan). Read with `docs/results-perfdb.md` and
`docs/patched-ic-design.md`. The send path is THE remaining lever for the goal's
send-heavy real workloads (sends are 3.3x denser than arith; our JIT sends are
3.5x slower than Cog; the send cost dominates the 7.8x SUnit-CPU gap).

## Per-send anatomy (workflow, file:line in AsmjitT1.cpp arm64)

Every send pays a **~13–14 instr / ~4 load probe head** (`6043–6077`): receiver
load, SmI-tag check, a **leak guard** (`lsr #48; cbnz` — 2 instr Cog lacks,
`6068`), **class-key recomputed from the header every send** (`ldr w4,[x1]; and
#0x3FFFFF` — Cog caches a class-index reg, `6072`), and an **out-of-line IC
pointer+key load** (Cog embeds the cache inline at the call site, `5955/6212`).
Cog's monomorphic send is ≈`cmp class, b.ne miss, bl target` (~4 instr). So the
head alone is ~9–10 removable instr + ~3 loads.

After the head the send diverges by terminal outcome:
- **inline-J2J** (self-recursive only; `7550–7856` push + `4650–4814` return):
  ~40 instr + ~15 mem ops of software-frame save/restore (V2 = 40-byte/5-word
  save, `J2JSaveLayout.h`), no Cog analogue — Cog uses a hardware call/ret. Mostly
  irreducible on the software-frame design; it's the BV/sender-chain corruption
  surface.
- **dispatchCached** (`9509–9548`): NOT a cached direct call — a **JIT→C++→JIT
  round-trip** (stores state, `EXIT_SEND_CACHED`, `ret x30` back to the C++ chain
  loop, which re-enters the callee). This is the dominant cost for send-heavy
  SUnit/soogle, where inline-J2J rarely fires.

## ⚠️ What the adversarial verify corrected (do NOT skip)

1. **The plan's headline lever — "activate the inert PMS direct-call" — is ALREADY
   SHIPPED.** PMS/patchedShape is default-ON (`5939`, opt-out only) and
   `linkSendSite` (`JITRuntime.cpp:3350–3548`) is fully implemented + runtime-wired
   (called on recompile + every slot-0 IC write, patches key/branch/J2J-tail). There
   is no inert PMS to turn on.
2. **PMS does not touch the slow bucket.** `linkSendSite` only links `extras&kJ2JBit`
   sites and refuses primitive/canBail/stub callees (`3390,3409`), so the linked-PMS
   population == the existing inline-J2J population. The sends that hit dispatchCached
   are the **`extras==0` unclassified / quick-prim sends** (`o class`/prim 111, `#==`,
   `#isNil`). `patched-ic-design.md:192` states verbatim that PMS CANNOT move those —
   they need a **separate B6 classifier / compiler-coverage lever**.
3. **`F` was mis-defined.** `(PMS+dispatchCached)/all` conflates the fast PMS-linked
   direct call with the slow round-trip. The real go/no-go number is the
   **`extras==0` dispatchCached fraction** (the B6-addressable bucket), broken out by
   selector.
4. **Emit-hash-identical validation is impossible** for emit-time counters (they add
   bytes). Use cpu_ms-neutral + report-sunit 0-regression instead.
5. **Horizon is over-optimistic.** PMS is already shipped and the design doc's own
   estimate for it was ~15–30% on microbenches. The remaining lever (B6 quick-prim
   classifier coverage) is **single-digit-to-~20% on send-bound**, less end-to-end on
   SUnit. The J2J software save (~13 stores + nil-fill) and the probe head are the
   floor. **Cog parity needs the native-call/checked-entry rewrite** the plan
   correctly excludes (the multi-session corruption surface behind the BV saga).

## Phase 1 (CORRECTED) — cheap decisive send census (1 session)

Measure the **execution-weighted terminal-outcome split** on benchFib (send-bound)
+ a SUnit send-heavy batch, into SIX mutually-exclusive buckets: (1) inline-J2J,
(2) inline-prim/getter/setter/returnsSelf, (3) **PMS-linked direct call** (already
fast — NOT addressable), (4) PMS-unlinked→dispatchCached, (5) plain dispatchCached
(`extras==0` unclassified/quick-prim — the B6-addressable bucket), (6) IC-miss→chain.
The decision number is **`F_addr = (4+5)/all` = the dispatchCached round-trip
fraction**, with bucket 5 broken out **by selector** (to confirm it's dominated by
quick-prims like `#class`/`#==`/`#isNil` that classifier coverage could convert).
Buckets 1–3 are already maximally specialized — not addressable by any committed
phase.

LOW-RISK implementation (sidesteps the verify's emit-injection concerns): the
dispatchCached + IC-miss buckets are HANDLED IN C++ (the chain loop), so count them
there with a selector histogram — **no codegen change** (unlike the regstack census,
which was emit-time but zero-codegen, this is even safer: pure C++ counters at the
exit handler). The inline-J2J bucket reuses the always-on `J2J stencil calls`
counter (547M observed). PMS-linked (bucket 3, in emitted code) is the only one
needing care — infer it from the linked-site set or accept a bound. Validate:
report-sunit 0-regression vs the 12,898-test Cog baseline + cpu_ms-neutral.

- **GO** if `F_addr ≥ 0.40` AND bucket 5 is dominated by addressable quick-prims
  (classifier coverage can convert them) → pursue the B6 classifier lever.
- **NO-GO** if `F_addr < 0.40` (most real sends already inline-J2J/inline-prim → the
  3.5x is the irreducible head + J2J-save/activation floor; redirect to
  `tryJITActivation`, `Interpreter.cpp:24488`) OR if bucket 5 is dominated by sends
  classifier coverage can't help.

## Phases 2+ (only the corrected levers)

- **Phase 2 — trim the universal probe head (1–2 sess, MEDIUM risk).** Default
  `PHARO_T1_LEAK_GUARD_OFF` ON (leaks root-caused, `6068`) and avoid re-deriving the
  class key when the receiver class is known. Recovers ~2–5 instr on EVERY send
  (helps fib too). Done: knob flipped + benchFib cpu_ms REPEAT=5 drops + report-sunit
  0-worse. Risk: the leak guard caught stale classifier-bit receivers (Roassal3
  SIGSEGV history) — full SUnit + Roassal under DET_SCHED before defaulting.
- **Phase 3 (CORRECTED) — B6 classifier / compiler coverage for quick-prims (3–5
  sess, HIGH risk).** NOT PMS (already done). Make the `extras==0` dispatchCached
  quick-prims (`#class`/`#==`/`#isNil`/prim 111) get classified+inlined (an extras
  bit + inline emit) so they stop taking the C++ round-trip. Contingent on Phase-1
  showing bucket 5 is large + quick-prim-dominated. Done: bucket-5 fraction drops +
  send-bound cpu_ms improves + report-sunit 0-worse.
- inline-J2J admission widening: **deprioritized** — PMS already covers the
  J2J-eligible monomorphic population, so widening admission is low-upside / EXTREME
  risk (the BV×inline-J2J corruption just fixed at HEAD `bf17def0`). Do not pursue
  without first root-causing the value:value: corruptor.

## Horizon + kill criteria (honest)

- **Horizon:** Cog parity is NOT reachable on the software-frame design (needs the
  native-call/checked-entry rewrite, deliberately excluded). PMS — the big monomorphic
  lever — is already shipped. Realistic REMAINING win: Phase 2 head-trim ~single-digit
  on send-bound; Phase 3 B6 coverage single-digit-to-~20% on send-bound, **less
  end-to-end on SUnit**. Net: a meaningful-but-modest dent in the 7.8x SUnit gap, not
  closure. Same shape as the operand-stack direction (E): the addressable fraction is
  modest; the structural floor (software frame + IC machinery) dominates.
- **Kill criteria:** Phase-1 `F_addr < 0.40` or bucket-5 not quick-prim-dominated →
  STOP (gap is the activation/frame floor). Phase 2 can't pass a 0-regression suite
  gate twice → keep the guard. Any phase producing sender-chain corruption that
  DET_SCHED can't localize in one session → revert the knob (binary stays green),
  stop. General rule: any phase that can't hold the 12,898-baseline 0-new-regressions
  under DET_SCHED is killed, not worked around.

## Meta (both scoped directions together)

Operand-stack (E) Phase-1 = NO-GO (~3%); send-path remaining levers = modest
(single-digit-to-~20%). **The goal's send-heavy workloads have no dramatic-win lever
on the current design** — the 21x/7.8x gap is structural (software-frame calls + the
per-bytecode/per-send IC machinery of a from-scratch JIT vs mature Cogit). Closing it
needs a native-frame send rewrite (the corruption surface the project avoids) or a
matured Sista that fires for tests. The cheap executed Phase-1 measurements are what
let us say this with data instead of speculation.
