# Block-value inline (`PHARO_T1_INLINE_BLOCK_VALUE`) — debug map

Status 2026-06-20: **opt-in because BROKEN**. Enabling it corrupts the image
during **startup** — even a no-block eval fails. This file maps the mechanism and
records what's been ruled out, so the next (dedicated, lldb) session starts informed.

## Symptom

- `PHARO_T1_INLINE_BLOCK_VALUE=1` + `eval "3 + 4"` (NO blocks) → **no result**
  (BV OFF → `R<7>`). So BV fires during the JIT-compiled startup and corrupts
  interpreter state before any user code runs — it is NOT specific to the eval.
- A block loop (`b := [:x|x*2+1]. 1 to: N do: [:i| s := s + (b value: i)]`) →
  no result at any N (100..3M); a 200k run shows BV `tries=194543 hits=14081
  bails=180462`, then **342M runaway sends, no result, no clean error/DNU/TERM**
  (state corruption, not an exception).

## Mechanism (the moving parts)

1. **Emit** — `AsmjitT1.cpp:6550` (`g_debug.t1InlineBlockValue`): on IC extra
   bit 59 (BLOCK_VALUE_BIT), call the prep helper, then `br x9` into the block's
   JIT entry. On NULL return, bail to `j2jBail`.
2. **Prep helper** — `JITRuntime.cpp:1997` `jit_rt_inline_block_value_prep`:
   validates the closure, gates (leaf-only by default: bails non-leaf / NLR /
   prim / stub / canBailMidMethod), pushes a J2J save (2090-2123), saves+sets the
   closure on a side-stack (2133-2142), sets callee state (receiver/tempBase/
   literals/jitMethod/method/ip, 2144-2158), copies captures + nil-fills temps
   (2160-2180), returns the block's JIT entry.
3. **Return** — three paths pop the J2J save:
   - C++ fast path `JITRuntime.cpp:918` — restores the BV closure (927-935). ✓
   - `J2J_INLINE_RETURN_IMPL` (stencils.cpp:562) → helper `JITRuntime.cpp:1981`
     — restores the BV closure (1988-1992). ✓
   - **asm prelude `emitJ2JReturnPrelude_arm64` `AsmjitT1.cpp:4647`** — the HOT
     path for JIT'd block returns. Restores sp/receiver/tempBase + (for BV/xmethod)
     jitMethod/method/literals/argCount (4753-4775), but **does NOT restore the BV
     closure** (no `bvIsBvSaveAtJ2jDepth_` check / `setCurrentClosure`).

## Ruled out

- **Closure-leak hypothesis: REFUTED.** The asm prelude (4647) lacking the closure
  restore looked like the bug (BV blocks returning via asm would leak the block's
  closure as `currentClosure_`). But disabling the closure save entirely
  (`JITRuntime.cpp:2135` → `if (false && ...)`) did NOT fix even the leaf repro,
  and startup still corrupts. So the closure side-stack is not the (sole) bug.
  (The missing asm-prelude closure restore is still a latent gap worth closing,
  but it isn't what breaks startup.)
- **256-cap desync:** not the cause — with balanced save/restore `bvClosureSaveDepth_`
  stays ~nesting depth; and the closure path is refuted anyway.

## Where the bug is (next session)

The corruption is in the **core BV inline** — the J2J save (2090-2123) and/or the
callee state setup (2144-2180) and/or its interaction with the asm return prelude
(4647) — and it fires during startup. Direction:
- `PHARO_DET_SCHED=1` for deterministic startup scheduling (see CLAUDE.md), then
  lldb-break on the FIRST BV hit during boot (`g_blockValue_hits` increment, or
  the `br x9` site) and single-step the block entry + return, comparing
  `state.{sp,receiver,tempBase,ip,literals,jitMethod}` and the operand stack
  against the non-inlined `activateBlock` path (Interpreter.cpp:~10568).
- Suspect areas to check first: (a) `tempBase = fp + 1` vs the block's actual
  arg/temp layout when the block has 0 captures and `totalTemps == nArgs` (the
  copy loop 2172 is a no-op then — verify the arg is where the block reads it);
  (b) the packed V2 resume (`save->resumeAddr` 2104-2110, bcOff vs the BV
  pre-send ip convention) feeding a wrong resume on return; (c) `s->ip` for the
  block (2157: `compiledBlockBits + 8 + (1+numLits)*8`) vs the block's real
  bytecode start.

37x lever if fixed (block_recursion); a plain `[b] value: i` loop is 20x
(366ms vs Cog 18ms). Leaf-only by default; `PHARO_T1_INLINE_BLOCK_VALUE_NONLEAF`
relaxes the gate (also broken).

## lldb session 2026-06-20 — findings (BV still broken, but narrowed a lot)

TOOLING LESSON: lldb runs from its cwd, so a RELATIVE image arg fails SILENTLY as
"Cannot open image file" → exit status 1 (looks like a VM crash but isn't). Always
`cd` to the image dir or pass an ABSOLUTE image path. (Burned several runs on this.)

KEY FINDING — the failure is NOT the BV inline path:
- With BV ON, a breakpoint on `jit_rt_inline_block_value_prep` / JITRuntime.cpp:2180
  (the hit) NEVER fires during the failing `Stdio … (3+4) printString` eval, yet the
  eval still fails — it RUNS AWAY (112M+ sends for a 3+4 eval, no result), no SIGSEGV,
  no `terminateCurrentProcess`. So the bug is a **side effect of the
  `t1InlineBlockValue` flag changing global codegen**, exercised even when no block-
  value send fires. My earlier "core BV inline" framing was wrong.

REAL BUG FOUND (latent, not the runaway): the asm return prelude restore branch
`emitJ2JReturnPrelude_arm64` (AsmjitT1.cpp ~4764) does `ldr x7, [x4, 32]` expecting
`jitMethod` (V1 J2JSave layout), but under PHARO_J2J_SAVE_V2 **offset 32 is `closure`**
(V1: jitMethod@32; V2: closure@32, resumeAddr-packed@24; struct in Interpreter.hpp:681).
Since nil's rawBits != 0, the `cbz x7` guard never skips → it would restore
state.jitMethod=closure (garbage) for xmethod/BV. The branch is `if (xmethod || bv)`
(both opt-in) so default config is unaffected. Making it V1-only did NOT fix the BV
runaway, so this is a SEPARATE latent bug (flagged with a NOTE at the site; not fixed
because the proper V2 fix needs a caller-JM source the 40-byte V2 save lacks — likely
extend the save to 48B with a callerJitMethod field, or rely on the 9558 PC-relative
restore which BV's resume=endOfSend currently SKIPS).

RUNAWAY SIDE-EFFECT SUSPECTS (codegen flips from the flag, active without BV firing):
- AsmjitT1.cpp:7707 `spLiveInX2 = !t1InlineBlockValue && !t1InlineJ2JXmethodLog` — BV ON
  forces the sp-reload-into-x12 path. **RULED OUT 2026-06-20**: forcing spLiveInX2 true
  with BV on (dropping the BV term) did NOT stop the runaway.
- AsmjitT1.cpp:11623 `staticJ2JArgCount = -1` when BV ON — forces the dynamic arg-count
  path in the prelude for ALL methods (but that path is the default for varying-arg
  methods, so less suspect).

UPDATE 2 (later same session) — two corrections + one more suspect ruled out:
- "BV helper NEVER fires" was WRONG (it was based on the HIT bp at 2180). With the
  correct image path, breaking on the helper ENTRY (`jit_rt_inline_block_value_prep`)
  DOES fire: the helper is CALLED from JIT'd code (bt: JIT frame -> tryJITActivation ->
  activateMethod -> sendSelector -> interpret) and BAILS before 2180 (bit-59 IS set on
  startup `value:` sends; they bail, e.g. block-not-compiled). So the BV runtime bail
  path (`b j2jBail` at AsmjitT1.cpp ~6605) IS exercised — it's not a pure no-BV-runtime
  codegen side effect.
- staticJ2JArgCount=-1 (11623): RULED OUT — dropping the BV term (keep the static value
  with BV on) did NOT stop the runaway.
- REFINED PRIME SUSPECT: the BV bail -> j2jBail path for a `value:` send. j2jBail is the
  inline-J2J bail; a BV-classified `value:` that bails may be re-dispatched there without
  advancing (re-classify BV -> re-bail -> loop) -> the 112M-send runaway. NEXT: lldb a
  breakpoint just after a BV bail (the bailBV/j2jBail site) and watch whether the SAME
  send re-enters; if so, fix the BV bail to fall to the normal value: dispatch
  (dispatchCached) instead of j2jBail, or ensure j2jBail advances the ip past the send.
  Ruled out to date: closure side-stack, 4764 V1/V2 jitMethod misread, spLiveInX2 (7707),
  staticJ2JArgCount (11623).

NEXT SESSION: bisect the side effects — build BV-ON but neutralize each in turn
(force spLiveInX2 true at 7707; force staticJ2JArgCount through at 11623; V1-only the
4764 branch) and find which neutralization stops the 112M-send runaway. Repro
(MUST cd to the image dir): `cd /tmp/pkgtest && PHARO_T1_INLINE_BLOCK_VALUE=1
.../test_load_image neojson.image eval "Stdio stdout nextPutAll: 'R<', (3+4) printString,
'>'; lf; flush."` → want R<7>, currently blank. lldb attaches cleanly from /tmp/pkgtest
with an absolute VM path.
