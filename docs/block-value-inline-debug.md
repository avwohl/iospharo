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
