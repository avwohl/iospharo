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

UPDATE 3 (end of session) — bail-loop hypothesis WEAKENED + a new direction:
- Code-reading the bail path: BV bail (bailBV ~6603) -> `b j2jBail` (7826) -> for a
  `value:` send the receiver is a heap FullBlockClosure (tag 0) so it takes the heap
  path (7833/7850) -> not getter/setter/at -> `b dispatchCached`. dispatchCached
  re-dispatches normally (sets EXIT_SEND_CACHED, chain loop activates the cached
  target) — i.e. it ADVANCES. So the "tight bail re-dispatch loop" is unlikely the
  cause; the bail routes to a normal dispatch. (Not 100% confirmed at runtime — see next.)
- Tried to catch the runaway steady-state (break helper, ignore-count 500000, compare
  caller ip across calls) but it TIMED OUT: under lldb the runaway is slow (~1.18M sends
  in 160s), so 500k helper calls weren't reached. lldb single-stepping is too slow for
  this — prefer light gated instrumentation (e.g. a counter on re-entry of the same ip,
  or dump the Smalltalk context chain) over lldb for the next attempt.
- NEW CLUE: the runaway log shows `[XFER-N]` scheduler process-transfer traces walking
  DOWN priorities (80->70->60->50->40->40, two pri-40 processes at the end). This MAY
  indicate the runaway is process/scheduler-level (processes cycling / not terminating)
  rather than a single hot send loop — which would connect to the known scheduler-
  deadlock / process-leak family ([[vm-scheduler-cog-parity]], [[timer-scheduler-wedge]]).
  UNCONFIRMED: [XFER] may also be normal boot scheduling; needs a BV-on vs BV-off diff of
  the [XFER] sequence to tell.
- RULED OUT this session: closure side-stack, 4764 V1/V2 jitMethod misread, spLiveInX2,
  staticJ2JArgCount, and (by code-reading) the tight bail->j2jBail re-dispatch loop.
- NEXT: (a) diff the [XFER] scheduler trace BV-on vs BV-off to confirm/deny the process-
  cycling angle; (b) if it's a send loop, add a gated "same-ip re-entry" counter at the
  BV bail to catch it cheaply (lldb too slow); (c) the latent 4764 V1/V2 closure-as-
  jitMethod bug is real + still worth fixing (extend V2 save to carry callerJitMethod).

UPDATE 4 — [XFER] diff CONFIRMS a process ping-pong (the runaway is scheduler-level):
- BV-OFF: 21 XFER transfers then DONE (R<7>). The pri-40<->pri-80 exchange (XFER 14-21)
  runs ~4 times and RESOLVES.
- BV-ON: XFER count grows without bound (49+ in a 25s sample) in a PERFECT INFINITE
  cycle: `pri=40 -> pri=80, pri=80 -> pri=40, …` forever. Two processes ping-pong and
  never terminate.
- Addresses: the pri-80 process **0x302c52a18 is CONSTANT across both configs**. BV-off:
  it ping-pongs with pri-40 0x55b54ef10 then resolves. BV-on: it ping-pongs with a
  DIFFERENT pri-40 process 0x303e281f8 forever. So the pri-40 partner (the eval/work
  process) never completes its work with BV on → the pri-80<->pri-40 signal/wait
  handshake (a semaphore / Delay / timer) loops indefinitely.
- INTERPRETATION: the runaway is NOT a tight send loop — it's a non-terminating
  process handshake. Some computation in the pri-40 process doesn't finish (or a
  wait-condition never resolves) under BV on, so the pri-80 partner keeps being
  signalled/rescheduled. Connects to the scheduler/timer family
  ([[vm-scheduler-cog-parity]], [[timer-scheduler-wedge]], [[sunit-headless-renderloop-wedge]]).
- NEXT: (a) dump the pri-40 process 0x303e281f8's Smalltalk context/method chain (what
  is it looping on?) and the pri-80 process 0x302c52a18's wait object (which semaphore/
  Delay?) — break in the scheduler transfer (the [XFER] print site) and walk
  suspendedContext_ of each (the "suspend + walk suspendedContext" technique in
  [[sunit-fullrun-clynotebook-blocker]]); (b) since BV's runtime bail routes to
  dispatchCached (advances) and the codegen side-effects are ruled out, suspect a
  CORRECTNESS divergence in a block-using computation that changes a loop/wait condition
  (e.g. a `whileTrue:`/`Delay`/semaphore predicate that reads a wrong value). The
  [XFER] print site is the cheap entry point (no lldb slowness).

UPDATE 5 — ROOT NAMED: infinite exception-handler search on a corrupted Context chain.
Walked both processes' suspendedContext at the XFER site (temp dump, reverted). The two
ping-pong partners are:
- pri-80 0x302c52a18 (CONSTANT): `waitForUserSignalled:orExpired: ... ensure:
  runBackendLoopAtTimingPriority` — the Morphic/SDL2 backend event loop, waking on a
  Delay (NORMAL; it's just the high-pri timing loop).
- pri-40 (the runaway process): alternates between
  `raiseUnhandledError -> defaultAction -> signalForException: -> signal` and
  `findContextSuchThat: / isHandlerContext / findNextHandlerContext / nextHandlerContext
  / findNextHandlerContext`. It is STUCK IN AN INFINITE EXCEPTION-HANDLER SEARCH.
=> The runaway is the EXCEPTION MACHINERY looping: an UnhandledError's handler search
   (`findNextHandlerContext` walking the Context SENDER chain, slot 0) never terminates —
   a corrupted / cyclic sender chain. Classic "broken Context sender chain so handler
   search can't reach/terminate" — the SAME failure family as the HELPER_SENDS bug
   ([[sunit-full-suite-run]]) and the NLR sender-chain bugs
   ([[nlr-nested-valuewithexit-bug]]). BV on corrupts the materialized sender chain
   (materializeFrameStack + the BV/J2J-save -> Context materialization; suspect a V1/V2
   save-field misread like the 4764 closure-as-jitMethod bug feeding a wrong sender).
NEXT (the actual fix path): (a) find the exception being raised under BV (why
raiseUnhandledError at all — likely a BV-induced wrong value/DNU); (b) dump the Context
SENDER chain (slot 0) of the pri-40 process and find the CYCLE / broken link — that's the
materialization bug to fix; (c) cross-check materializeFrameStack's handling of BV J2J
saves vs the V2 save layout (Interpreter.hpp:681). The exception-search loop, not a send
loop, is THE thing to fix.

UPDATE 6 — NOT a cycle / not prim-197; it's an infinite exception RE-HANDLE loop.
- Dumped the pri-40 suspendedContext SENDER chain (slot 0) with address cycle detection:
  250 frames, NO repeated address — so NOT a tight cycle. The 250 frames are
  185x `nextHandlerContext` + 184x `findNextHandlerContext` alternating = deep/unbounded
  MUTUAL RECURSION of the handler search.
- Used prim 197's built-in diag (PHARO_T1_TRACE_HANDLER=1, Primitives.cpp:10852): prim 197
  WORKS — it repeatedly logs `FOUND handler #on:do:` / `FOUND handler #evaluateSignal:`
  (exhausted=0). So the search primitive is NOT failing and the chain DOES reach handlers.
- Selector frequencies in the runaway stack: besides find/nextHandlerContext, there are
  19x handleSignal:, 11x signal, 5x defaultAction, 4x evaluateSignal:/handleError:/cull:/
  signal:/ensure:/logDuring:. => the loop is at the HANDLING level: an exception is found
  AND handled (handleSignal: -> evaluateSignal: -> handleError: -> cull: the handler block)
  but then RE-SIGNALS -> re-search -> re-handle -> forever.
- CONCLUSION: the corruption is a BLOCK executed in the exception path (the handler block
  via cull:/value, or a block inside the signal/handle machinery) returning wrong / not
  consuming the signal under BV, so the exception is re-raised infinitely. NOT prim 197,
  NOT a sender-chain cycle, NOT the codegen side-effects already ruled out.
- NEXT (different strategy needed — one-level-deeper lldb dives are hitting diminishing
  returns): (a) get the Pharo source of the handler path (handleSignal:/evaluateSignal:/
  return:/Signal>>return:) and find which block's wrong result re-raises; (b) compare the
  exception path BV-on vs BV-off at the Smalltalk level (e.g. trap the SECOND signal of the
  same exception — BV-off signals once, BV-on signals repeatedly); (c) the BV block-value
  inline corrupts a value/cull: result specifically in the handler-return path — likely a
  non-local-return or a wrong retval from a BV-inlined block (ties to the J2J-save/return
  value handling). The exception is real (some startup error); BV makes its HANDLING loop.

UPDATE 7 — the re-signaled exception is FileWriteError (BV-on only); divergence pinned.
Trapped the handled exception via prim 197's FOUND path (PHARO_T1_TRACE_HANDLER, temp
[HFOUND] dump of handler selector + the exception class found by walking startContext to
the #signal* context). Diff:
- BV-OFF (completes -> R<7>): 14 handled exceptions then DONE — FileDoesNotExistException
  (boot file probes), Error, PrimitiveFailed, STONReaderError, FileDoesNotExistException.
  All boot-expected, each handled once, boot proceeds.
- BV-ON (runaway): identical up to PrimitiveFailed (HFOUND 7), then DIVERGES: PrimitiveFailed
  (8) -> FileWriteError (9, evaluateSignal:) -> FileWriteError re-handled repeatedly
  (12,13,17,21,24…) + UnhandledError (16,22), forever. **FileWriteError appears ONLY with
  BV on.**
- So: BV-on, the PrimitiveFailed handler diverges and produces a FileWriteError whose
  handler (evaluateSignal:) never consumes it -> re-signal -> re-handle -> infinite,
  escalating to UnhandledError. The loop is the boot's error-LOGGING / file-write path
  (logDuring: seen in the stack) re-raising under BV. FileWriteError = a file write failing
  (the error logger writes a file; under BV the write/handler corrupts and loops).
- PERSISTENT CONTRADICTION to resolve for the fix: the BV helper ALWAYS bails (no 2180
  hit, even over 112M sends) and the bail routes to dispatchCached (advances) — yet BV-on
  diverges. CANDIDATE MECHANISM: the bit-59 BV CLASSIFICATION corrupts the IC even when the
  helper bails (e.g. dispatchCached re-dispatches a value: send to a wrong cached target
  because bit-59 is set), so the handler/log block returns wrong WITHOUT a BV "hit".
- NEXT (decisive, fresh session): (a) disable the bit-59 classification (so value: sends
  are never BV-classified -> helper never called) while keeping t1InlineBlockValue's other
  codegen — if the runaway stops, the bit-59/IC-classification is the cause (not a hit, not
  a codegen side-effect); find where bit 59 is SET and why it corrupts the bailed
  dispatchCached. (b) Otherwise trace the FileWriteError's first signal BV-on vs BV-off to
  see what write fails + which block returns wrong.

UPDATE 8 — ROOT CAUSE CONFIRMED: the bit-59 BLOCK_VALUE_BIT classification.
Decisive A/B (existing runtime knob PHARO_NO_BLOCK_BIT, gates the bit-59 set at
Interpreter.cpp:23338):
  BV-on + PHARO_NO_BLOCK_BIT=1  -> R<7>   (runaway GONE)
  BV-on alone                   -> runaway (no result)
  PHARO_NO_BLOCK_BIT=1 alone    -> R<7>   (sanity)
So the runaway is caused by the bit-59 CLASSIFICATION of value: sends — NOT a BV inline
"hit" (the helper always bails), NOT a codegen side-effect (all ruled out). With bit-59
omitted, BV-on is safe.
- The IC fill for a BV site (Interpreter.cpp:23814-23816) sets icData[e*3+1]=value: method
  (cached target is CORRECT) and icData[e*3+2] = (1ULL<<59) "BLOCK_VALUE_BIT only" (the
  extra is OVERWRITTEN to bit-59-only, losing other classification bits). The cached target
  being correct means the corruption is in the bit-59 DISPATCH PATH (tbnz 59 -> tryBlockValue
  -> helper bails -> j2jBail -> dispatchCached), not the target — it corrupts the handler-
  block value: send's state/resume even though the helper bailed.
- PHARO_NO_BLOCK_BIT is a SAFE-BUT-USELESS workaround: with it, BV-on never runs the inline
  (bit-59 never set -> tbnz never fires -> helper never called), so no runaway AND no
  benefit. It is NOT a fix; it confirms the bug is in the bit-59 classification/dispatch.
- THE FIX (next focused step): correct the bit-59 value: dispatch so a BV-classified send
  that bails dispatches the value: send cleanly (the cached target is already value:
  method, so the corruption is in the path's state handling / the resume after
  dispatchCached, or the overwrite at 23816 losing needed extra bits). Trace ONE bit-59
  handler-block value: send through bail -> j2jBail -> dispatchCached -> value: method ->
  block -> return, diffing interp state vs the PHARO_NO_BLOCK_BIT (normal) path, to find
  the divergent field. The exception (FileWriteError) re-handle loop is downstream of this.

UPDATE 9 — HIT-path bisect + a fix attempt (necessary but NOT sufficient).
- New diagnostic knob PHARO_BV_FORCE_BAIL (jit_rt_inline_block_value_prep returns nullptr
  immediately): keeps the bit-59 classification + the tryBlockValue detour, but the BV
  inline HIT never executes. BV-on + PHARO_BV_FORCE_BAIL=1 -> R<7> (no runaway). So the
  corruption is in the HIT path (helper success -> br into block -> J2J save/state setup
  -> block runs -> return), NOT the classification/bail (corrects the "always bails"
  confusion from UPDATE 5/6 — hits DO occur and corrupt). PHARO_NO_BLOCK_BIT and
  PHARO_BV_FORCE_BAIL are both safe-disables (BV-on stops looping) but neither enables the
  inline.
- IDENTIFIED + ATTEMPTED one real HIT-path bug: the BV resume = endOfSend (AsmjitT1.cpp
  ~6583) SKIPS the caller-JM PC-relative restore at resumeAfterCall/9549. After a BV
  inline the block ran with x19 = the BLOCK's JM; resuming at endOfSend leaves the caller
  under the block's JM. FIX TRIED: route BV resume to a restore-only label (bound after
  resumeAfterCall's pop+retval, before the restore) so the prelude's pop+retval isn't
  doubled. Default-safe (BV opt-in). RESULT: did NOT stop the runaway -> NECESSARY but NOT
  SUFFICIENT; reverted (left a NOTE at 6583). The HIT path has ADDITIONAL corruption
  beyond the return restore — in the helper's setup (receiver/temps/literals/ip at
  2144-2180, the J2J save 2090-2123, the closure side-stack 2133-2142) and/or the block's
  execution/return interaction.
- CONCLUSION: block-value's HIT path is a genuine MULTI-bug mechanism. It is correctly
  opt-in/off; both PHARO_NO_BLOCK_BIT and PHARO_BV_FORCE_BAIL safely neutralize it. A real
  fix needs systematically validating the helper's setup against activateBlock
  (Interpreter.cpp ~10568) field-by-field for a single BV hit, plus the caller-JM restore
  above — a focused multi-step effort, not a one-line fix.

UPDATE 10 — field-by-field validation of the helper setup vs activateBlock / value-prim.
The value-prim (primitiveFullClosureValue, Primitives.cpp:3751) general path delegates to
activateBlock (Interpreter.cpp:12236), which sets up via pushFrame. Compared each field of
jit_rt_inline_block_value_prep (JITRuntime.cpp:2090-2181):
- RECEIVER: CORRECT. Resolved the slot-3 ambiguity empirically — FullBlockClosure
  allInstVarNames = (outerContext, compiledBlock, numArgs, receiver), instSize 4. So slot 3
  IS the receiver (fixed instVar); copied values are indexed slots 4+. The helper's
  blockRecv = closureObj.slotAt(3) and copies = slots 4+ MATCH the layout. (The value-prim
  fast-path comments saying "copies at slot 3+" / "closure.slot(3+M)" look off-by-one vs
  this layout, but that fast path is narrow and BV-off works, so it's not the lever.)
- ip / method / literals / jitMethod / argCount: MATCH activateBlock.
- NLR home: N/A — the helper bails on blockJM->hasNLR (blocks with ^).
- CAPTURE-COPY (2160-2180): TESTED nil-init-all (no copy) via a temp knob -> did NOT fix
  the runaway -> the copy is NOT the (sole) bug; copied values appear to be frame-resident
  (the block reads them from the frame), so the copy is needed.
- CALLER-JM restore (resume=endOfSend skips the restore at 9540): NECESSARY but NOT
  SUFFICIENT (UPDATE 9).
- CONCLUSION: the HIT-path corruption is NONE of receiver/ip/method/literals/argCount/
  capture-copy/caller-JM-restore individually. It is subtle — remaining suspects: the V2
  J2J save push (2090-2123: the packed resumeAddr/bcOff + closure fields), the closure
  side-stack interaction (2133-2142, bvClosureSaveStack push without a matching pop on the
  BV fast-prelude return), or the block-execution/return interaction. Likely a COMBINATION
  (caller-JM-restore + one more) since single fixes don't clear it. A real fix needs
  single-stepping ONE BV hit's block from entry to return under lldb with PHARO_DET_SCHED,
  diffing the live interp state at each bytecode vs a non-inlined activateBlock run of the
  same block — the field-level static comparison is now exhausted.
  Two safe-disables confirmed: PHARO_NO_BLOCK_BIT, PHARO_BV_FORCE_BAIL.

UPDATE 11 — DECISIVE: the BV HIT-path RETURN is missing TWO restores (why single fixes fail).
The asm fast-prelude return (emitJ2JReturnPrelude_arm64, AsmjitT1.cpp:4647-4806) — the path
a BV-inlined block takes when it returns — is missing BOTH restores that the C++ return
paths perform:
  (1) CALLER-JM restore. The prelude's restore branch reads save[32] as jitMethod (V1
      layout) but under V2 save[32]=closure; and the BV resume=endOfSend skips the
      PC-relative caller-JM restore at resumeAfterCall/9540. So after a BV return the caller
      runs under the BLOCK's JM. (UPDATE 9.)
  (2) CLOSURE side-stack POP. The helper pushes the block's closure on bvClosureSaveStack
      (JITRuntime.cpp:2137) + sets s->closure (2140). The pop/restore exists ONLY in the
      C++ return paths (JITRuntime.cpp:932 fast-path, :1992 J2J_INLINE_RETURN_IMPL) — the
      ASM prelude (4647) does NOT pop it. So a BV return via the asm prelude LEAKS the
      block's closure (bvClosureSaveDepth_ grows, s->closure stays the block's). (CONFIRMED
      this update by grepping the prelude: zero bvClosure/setCurrentClosure/block_value_post
      refs.)
=> BOTH are needed; that's why caller-JM-restore alone (UPDATE 9) and capture-copy changes
   (UPDATE 10) each failed to clear the runaway. The C++ fast path (JITRuntime.cpp:918) does
   BOTH correctly; the asm fast-prelude does NEITHER.
THE FIX (two coordinated parts on the BV return):
  (a) route the BV resume to a label that does the PC-relative caller-JM restore (the
      bvRestoreOnly approach from UPDATE 9), AND
  (b) make the asm BV return pop the closure side-stack — emit a call to
      jit_rt_inline_block_value_post (JITRuntime.cpp:1981, the existing closure-pop helper)
      on the BV resume, OR route BV returns through the C++ fast path (918) which already
      does both. (a) without (b) leaves the closure leak; (b) without (a) leaves the JM
      wrong — must do both, then re-validate (BV-on 3+4 -> R<7>, block loops correct, full
      kernel/packages A/B).
This is the exact, localized defect. block-value remains correctly opt-in; safe-disables
PHARO_NO_BLOCK_BIT and PHARO_BV_FORCE_BAIL.

UPDATE 12 — implemented the two-part return fix (bvResume); necessary, STILL not sufficient.
Added jit_rt_pop_bv_closure_state(JITState*) wrapper + a BV-only resume label `bvResume`
(AsmjitT1.cpp): the BV resume now routes to bvResume which (1) pops the closure side-stack
via the wrapper and (2) restores the caller's JM PC-relatively — the two restores the asm
fast-prelude skips (the C++ return paths do both). DEFAULT config UNAFFECTED + passes
(3+4 -> R<7>, kernel 9-class 1937/0/0; bvResume is dead code when BV off, skipped by a
b endOfSend). BUT BV-on STILL runs away (104M sends, hits=7973). So the return restores are
necessary (the C++ paths do them) but the PRIMARY HIT corruption is UPSTREAM — in the
helper's frame SETUP (the V2 J2J save push 2090-2123, esp. the packed resumeAddr's pre-send
bcOff which on a GC/bail mid-block would re-execute the value: send) or the block's
mid-protocol EXECUTION (entered via br x9 with no prologue, 6623). The setup FIELDS
(receiver/ip/method/literals/argCount) were validated equal to activateBlock (UPDATE 10),
so the remaining suspect is the J2J-save packed-resume/bcOff semantics or the no-prologue
block entry — needs single-stepping the block's first bytecodes under lldb+DET_SCHED vs a
non-inlined run. block-value stays opt-in; the kept bvResume code is correct + default-safe
(a partial fix), the runaway is a separate upstream defect.

NEXT SESSION: bisect the side effects — build BV-ON but neutralize each in turn
(force spLiveInX2 true at 7707; force staticJ2JArgCount through at 11623; V1-only the
4764 branch) and find which neutralization stops the 112M-send runaway. Repro
(MUST cd to the image dir): `cd /tmp/pkgtest && PHARO_T1_INLINE_BLOCK_VALUE=1
.../test_load_image neojson.image eval "Stdio stdout nextPutAll: 'R<', (3+4) printString,
'>'; lf; flush."` → want R<7>, currently blank. lldb attaches cleanly from /tmp/pkgtest
with an absolute VM path.
