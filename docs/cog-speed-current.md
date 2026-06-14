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

## Next lever (under investigation, see characterization workflow)

The cross-method lever is closed; the remaining ~1.9x bytecode tax + ~1.5x send
tax are the constant per-op overhead of the naive stack machine vs Cog's
register-resident frames. Candidate levers: (1) finish frame-state-residency
(docs/frame-state-residency.md, M1/M2 landed); (2) revisit the TOS register
cache for arith-heavy code; (3) shrink the per-send emit (out-of-line the
IC-probe/dispatch into shared trampolines, Cog-style — also fixes the ~6KB/method
zone pressure). Decision pending the disasm characterization.
