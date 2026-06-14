# Cog-speed: current state (2026-06-14) — cross-method lever CLOSED

Fair, same-machine, same-image (fresh Pharo 13), optimized arm64 build
(RelWithDebInfo, `build/test_load_image`) vs stock Cog
(`/tmp/harness/pharo-vm/.../Pharo --headless`). Min-of-9, thorough warmup.
Bench source: `/tmp/cogbench2.st` (+ inline-loop form to dodge a
`Time millisecondsToRun:` resolution artifact on the method-wrapped loop).

```
benchmark                  ours   Cog   ratio   isolates
inline loop 100M (arith)    237   125   1.9x    per-bytecode naive-stack tax
benchFib 33 (self-rec)       73    25   2.9x    + per-send sequence
cfibx 32 (xmethod leaf)      61    23   2.65x   cross-method (WAS ~43x)
cfibs 32 (xmethod+send)      85    29   2.9x    cross-method (WAS ~50x)
```

## Headline: the documented "cross-method send activation" lever is DONE

The 2026-06-09 retrospective (docs/jit-retrospective.md) characterized the whole
real-code Cog gap as cross-method send throughput (cfib 344ms = 43x, an
`incs ^(self+1) max:0` helper = 503ms = 50x). Those are now **22-30ms = 2.65-2.9x** —
in line with self-recursion. The levers that closed it, all now DEFAULT-ON:
- `PHARO_T1_XMETHOD_MAX_IC=8` (admit cross-method callees with up to 8 IC sends)
- bailmid callees admitted (`PHARO_T1_NO_BAILMID_CALLEES` opt-out)
- prim-prologue callees admitted (`PHARO_T1_NO_J2J_PRIM_PROLOGUE` opt-out)
- PMS patched sends (`PHARO_T1_NO_PATCHED_SENDS` opt-out)
- XGATE fold (`PHARO_T1_NO_XGATE_FOLD` opt-out)
- FSR M2 cursor residency (`PHARO_T1_NO_FSR_CURSOR` opt-out)

The retrospective's "43x / 503ms / lever (c) not yet correct" narrative is STALE.

## The residual gap decomposes into two constant per-op taxes

- **Per-bytecode stack tax ≈ 1.9x** (pure arith loop, no sends). Naive stack
  machine: every bytecode round-trips TOS through memory. `PHARO_T1_TOS_REG`
  (x26 write-through TOS cache) targets this but is parked as an -O2 A/B wash.
- **Per-send sequence ≈ +1.5x on top** (→2.9x on fib/cfib). Each send emits
  IC-probe + class-key + dispatch + 56-byte J2JSave push + branch + return-prelude.

Emitted-code bloat (ground truth, `PHARO_T1_DUMP_SEL` + capstone):
- cfibx: 19 bytecodes → **5568 B / 1392 arm64 insns** (~293 B/bytecode, send-dominated)
- tl loop: 29 bytecodes → **1124 B / 281 insns** (~39 B/bytecode, push/store/arith)
Disasm: `/tmp/disasm_jit_cfibx_1.txt`, `/tmp/disasm_jit_tl_1.txt`.

## Knob sweep (default = all levers on)

```
config              loop  fib  cfibx cfibs   (noisy single-shot, min-of-5)
default (all on)     ~36   16   22    31
+TOS_REG             ~56   15   21    28     (helps arith loop; ~wash on sends)
NO_PATCHED_SENDS      90   17   24    33     (PMS worth ~1-2ms)
NO_XGATE_FOLD         92   17   24    33     (XGATE worth ~1-2ms)
NO_FSR_CURSOR        var   18   25    35     (FSR worth ~1-3ms on sends)
```

## Next-lever analysis (cog-speed-anatomy workflow, 2026-06-14)

A 6-agent workflow (4 parallel disasm/source/doc characterizers → synthesis →
adversarial critique) mapped the residual and ranked the levers. Ground-truth
per-send anatomy (one linked recursive cfibx send ≈ 102 emitted insns): IC-probe
head 17, dispatch/poly-walk 13, xmethod gate 16, J2J save-push 34, the branch 1,
return-prelude 21. Per the six-times-confirmed OoO lesson (FSR §11, simstack §9),
**instruction COUNT is free** (independent stores drain via the store buffer);
only dependent-chain shortening and shared-address serialization (RMWs, STLF
round-trips) measure. That single fact decides the ranking:

```
lever                              verdict
poly-walk fold (cut IC walk)       REJECT — misread of static/unlinked disasm; linked PMS
                                   sites already cmp/b.ne/b-tail past the walk (linked-now=747)
M4 (delete save-push mirror stores) DEAD — already built + [M4-PARITY]-clean + MEASURED A WASH
                                   (store-buffer-absorbed category)
TOS_REG on the arith loop          REJECT — measured REGRESSING tightLoop 36->56; park rule fired
FSR M3 NODEPTH (kill depth RMW)    candidate, but ENABLING-ONLY (<3% likely) + scheduler-preempt risk
FSR M5 (receiver in x20)           higher upside BUT depends on M3 freeing x20; #extent-history risk
out-of-line dispatch (Cog-style)   HIGHEST ceiling (attacks +1.5x send tax AND 6KB/method bloat),
                                   but large/exploratory — needs a design pass (no doc beyond
                                   patched-ic-design §11 B6 sketch)
```

The only proven-to-MEASURE shape left is the dependent-chain/RMW kill: M2 cursor
(LANDED, took cfib 29→24-25), M3 depth, M5 receiver. The critique's honest call:
**there is no safe quick win left** — the cross-method lever already banked them.

### What the workflow's adversarial critique found (real defects in the M3c plan)

1. **FIXED THIS SESSION (commit 26613442):** `j2jDepthFromCursor()` divided by a
   stale literal `32`; V2 J2JSave is 40 bytes — a 1.25x mis-derivation landmine,
   dormant (no callers) but would bite the moment M3c wired the helper in.
2. M3c must guard **three** push-side depth-RMW emit sites (AsmjitT1.cpp
   6813-6815, 5341-5343, AND 6446-6448 — the synthesis missed the retro-save one).
3. **Scheduler-preemption blocker:** the forceYield doorbell is emitted only on
   native loop back-edges (9097-9104); benchFib/cfib are pure recursion with NO
   back-edges, so a deep self-rec J2J descent is preempted SOLELY by the per-call
   j2jTotalCalls charge. Dropping that charge without a per-call doorbell can run
   the chain un-preempted → the timer-scheduler-wedge / blocker-cascade deadlock
   family. M3c is not safe until in-descent preemption is re-funded + probed.

### Recommendation

M3c is enabling-only (its real value is freeing x20 for M5, the actual
receiver-residency send-critical-chain kill) and carries a real scheduler-deadlock
risk that needs a deep-recursion-starvation probe to clear — modest payoff, careful
work. The higher-ceiling play (out-of-line dispatch, Cog-shaped) needs a design
pass first. Both are multi-session; neither is a quick win. The divisor-bug
prerequisite is now fixed. Direction is a strategic call (see WIP.md).
