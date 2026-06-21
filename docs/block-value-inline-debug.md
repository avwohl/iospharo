# Block-value inline (`PHARO_T1_INLINE_BLOCK_VALUE`) — debug map

Status 2026-06-21: **FIXED (scoping) — BV-on now correct; still opt-in for perf.** The
startup runaway was fixed earlier (cap>=2 + remote-temp bails); the last class of failures
(the "12 sort" / cross-method `value:value:` corruption) is FIXED by UPDATE 31's scoping fix:
when BV inline is ON, cross-method inline-J2J is disabled (it was the value:value:-block-clean-
inline corruptor).  BV stays OPT-IN because BV-on now trades cross-method inline-J2J for
correctness; the default config (BV off) is byte-identical and unaffected.  See UPDATE 31.

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

UPDATE 13 — third return-path fix (nArgs/offset-48); BV runaway is in EXECUTION, not return.
Found + fixed a real V1/V2 bug in the dynamic arg-pop: the asm return prelude's dynamic
path (taken whenever staticJ2JArgCount<0, which block-value FORCES via 11623) loads nArgs
with `ldr w9, [x4, 48]` (the V1 sendArgCount slot, also written by the x86 J2J push at
AsmjitT1.cpp:3349). The 40-byte V2 save has no offset-48 field and the C++ BV helper never
wrote it -> BV returns popped a GARBAGE nArgs. FIX: the BV helper now writes save[48]=nArgs
(JITRuntime.cpp, matching the x86 push; leaf-only BV blocks => the overflow slot survives
to the return). Default-safe (helper only runs BV-on; default 3+4 R<7>, kernel clean).
RESULT: BV runaway STILL persists. So all THREE return-path bugs are now fixed (closure-pop
UPDATE 12, caller-JM restore UPDATE 9/12, nArgs UPDATE 13) — each real + necessary (they
match the C++/x86 canonical paths) but NONE is the primary cause. Combined with FORCE_BAIL
(no-hit => clean), this localizes the PRIMARY corruption to the block's EXECUTION (entered
via `br x9` at codeStart with the helper's setup) — NOT the setup fields (validated equal,
UPDATE 10) and NOT the return (now correct). The block runs wrong despite a correct frame.
NEXT: a CONTROLLED-hit lldb single-step is now required — gate BV hits to one test selector
(so startup bails, no runaway) then break at the helper return (2183) and step the block's
prologue + first bytecodes, diffing receiver/sp/temps vs a non-inlined activateBlock run.
The return-path is fully fixed; the execution defect is the last piece. block-value remains
opt-in/off; all fixes are default-safe + committed.

UPDATE 14 — controlled-hit harness attempt (gate BV to one caller selector). Added a temp
PHARO_BV_ONLY_SEL gate (helper bails unless callerJM->selector contains the string) to fire
ONE controlled hit with startup bailing (no runaway). Compiled bvxprobe (`^[:x|x*2+1] value:
20` -> 41) + bvxcap (capturing `[:x|acc:=acc+x]` summed 1..10 -> 55), ran each 200k hot.
RESULT: PROBE<41> CAP<55> CORRECT, exit 0 (no runaway) — BUT block-value hits=0 (bail
breakdown all-zero internally => all bails were the gate). So the controlled blocks' value:
sends never reached the helper as matched-selector hits (either bvxprobe/bvxcap didn't
JIT-compile their value: as a BV emit in this harness, or the oopToString selector match
needs adjustment). NET: confirmed the NON-inlined path is correct (41/55), but did NOT
exercise a HIT, so no new execution-bug data. Reverted the gate (temp). The controlled-hit
harness needs more work (verify the test method's value: is BV-classified + the gate match)
before it can isolate one hit. Execution bug remains localized (not setup UPDATE 10, not
return UPDATE 9/12/13) but unpinned. block-value stays opt-in; default config passes all
newly-added suites (the JIT works with the tests).

UPDATE 15 — BREAKTHROUGH (2026-06-20): startup runaway FIXED; BV-on total-runaway -> 99.4%.
Built a controlled-hit harness (PHARO_BV_ONLY_ARG gates hits to one value:-arg/home-receiver
so startup bails -> no runaway; PHARO_BV_TRACE_HITS logs hit shapes). Proved the simple
cases CORRECT under BV inline (identity, capture-read, multi-arg, nArgs=0, home+foreign
frame, deep j2jDepth — all return right values). Then found TWO real corruptors by shape:
  1. REMOTE-TEMP WRITE (FIXED). A block that STORES a captured remote temp (0xFB/0xFC/0xFD,
     e.g. `c do: [:x | s := x]`) loses the write when BV-inlined in a foreign frame
     (`bvcolldo` -> 0 instead of the last element; BV-off correct). FIX: JITMethod->
     hasRemoteTemp (scanned at compile, AsmjitT1.cpp ~12091) + bail in the helper.
  2. MULTI-CAPTURE cap>=2 (BAILED). Blocks with >=2 copied values corrupt under BV inline
     (bisected: PHARO_BV_MAX_CAP<=1 stops the runaway). The C++ capture-copy was verified
     IDENTICAL to activateBlock (firstCopiedSlot=4, order, count all match), so the defect
     is in the JIT block's emitted multi-copied-value ACCESS, not the copy. FIX (interim):
     PHARO_BV_MAX_CAP default 1 (bail cap>=2). The proper fix (emit-level) would re-enable
     cap>=2 inline.
  3. ALSO: the asm BV return does the closure-pop + caller-JM restore + nArgs/offset-48
     (UPDATEs 12-13), all necessary.
RESULT: BV-on 3+4 -> R<7> (the months-old startup runaway is GONE). Kernel A/B: BV-OFF
1937/0/0, BV-ON 1925/1937 (12 sort-related fails: testSort/testSorted/testAsSortedArray/
testSortUsingSortBlock/testIndexOf...Using). Default config UNAFFECTED (BV off -> 1937/0/0).
REMAINING (the last 12): deterministic (DET_SCHED same), NOT a closure/j2jDepth leak
(clodep stays 1), and every sort/=/isSorted op is CORRECT in isolated evals — so it's a
test-CONTEXT corruption (SUnit-forked test method calling sort+assert) the isolated evals
don't reproduce. Needs running ONE failing test (e.g. ArrayTest>>testSort) under the
trace/lldb to see which in-context hit corrupts. block-value remains opt-in.

UPDATE 16 — last-12 PRECISELY characterized (2026-06-21): BV + inline-J2J + SEND-COMPUTED
args. The 12 sort fails reproduce OUTSIDE SUnit as a hot JIT'd method (the fork was a red
herring): `stsort ^c sort asArray = #(...)` -> BV-OFF true, BV-ON returns the UNSORTED
order. Narrowed: `c sort` from a hot JIT'd method does not sort (the comparison corrupts);
`=` is fine. Then minimized to `cmpat ^[:a :b | a <= b] value: (arr at: 1) value: (arr at:
2)` -> BV-ON errors "Array did not understand #<=" (the block's inline-prim <= read the
ARRAY as an operand). KEY isolation:
  - The block ARGS arrive CORRECT: `[:a:b|a]`->5 and `[:a:b|b]`->999999 both right. Only the
    block's INLINE-PRIM (a<=b, operand-stack op) reads a bad operand.
  - CONSTANT args work (`[:a:b|a<=b] value: 5 value: 999999` -> true). Only SEND-computed
    args (`arr at: i`) corrupt -> the preceding cached send leaves state the BV block reads.
  - **PHARO_T1_NO_INLINE_J2J=1 FIXES IT** (sort + cmpat both correct). So the bug is the
    inline-J2J <-> BV interaction (the at: send + the value: BV inline at j2jDepth>=1), NOT
    BV alone. Constant-arg BV at depth>=1 works (recw/D2/D3), so it's specifically the
    cached-send-computed arg at depth>=1 feeding the BV block's operand-stack op.
Added a defensive x25 (sp) reload before the block `br x9` (AsmjitT1.cpp ~6635) — correct
for cap>0 operand-stack blocks, default-safe, but does NOT fix this bug (x25 was already the
block base for cap=0). The real defect is a register/send-resume state the inline-J2J'd at:
leaves that the BV block's inline-prim reads (a stale operand). NEXT: lldb single-step
cmpat's `<=` (BV-on, inline-J2J on) — watch which register/slot the inline-prim reads and
where the Array comes from (likely the at: receiver left in a simStack-tracked register the
mid-protocol block entry inherits). Minimal repro: /tmp/cmpat.st. block-value still opt-in;
default config UNAFFECTED.

UPDATE 17 — lldb single-step of cmpat NAILS the mechanism (2026-06-21). Broke at
Interpreter::sendDoesNotUnderstand (entry, --skip-prologue false; -O2 makes `this` unusable
post-prologue, so read members via (pharo::Interpreter*)$x0). At the cmpat `<=` DNU dumped
the frame (sp=0x..ab88, fp=0x..ab50):
    fp[1]=arr(0x..66448)  fp[2]=0x..66460  fp[3]=0x29(=5)  fp[4]=0x300000020
    fp[5]=arr(0x..66448)  fp[6]=0x..66460  <- sp[-2],sp[-1] = the <= operands a,b
The `<=` operands a,b = arr (the Array) + the next heap object — NOT the at: RESULTS
5 (0x29) and 999999 (0x7a11f9), which ARE present in the frame ~2 slots away. So the
BV-inlined block read its args from a ~2-slot-STALE operand sp: it got the lingering at:
RECEIVER (arr) instead of the at: RESULT. ROOT: when the enclosing method (cmpat) is
inline-J2J'd, its preceding cached `at:` send leaves x25 (the SP_IN_X25 residency register,
the fresher source — state.sp is only a mirror synced at exits, so it's even staler) off by
the send's net pop; the BV emit's emitSyncSpToState then publishes the stale x25 -> state.sp,
and jit_rt_inline_block_value_prep reads the wrong receiver/args. PHARO_T1_NO_INLINE_J2J=1
avoids it (the cached send keeps x25 in sync when cmpat isn't inline-J2J'd). FIX (next): make
the cached-send resume restore x25 correctly in the inline-J2J context, OR have the BV helper
derive the receiver/args fp-relative (stable) rather than from the residency sp. Repro
/tmp/cmpat_1l.st; lldb recipe above. block-value still opt-in; default config UNAFFECTED.

UPDATE 19 — x25-sync fix attempt: WRONG premise; corrected understanding (2026-06-21).
Tried to implement the "x25-sync fix" from UPDATE 17/18. Findings that INVALIDATE the
earlier inline-J2J framing:
 - The defensive x25 reload before the block `br x9` (committed df022704) does NOT fix the
   bug. For cap=0 blocks spOut==s->sp, so the reload is a no-op; x25 was already the block's
   operand base. The corruption is NOT a stale block-entry x25.
 - A bail on s->j2jDepth>0 does NOT fix the 12 sort tests (kernel still 1925/12). So the
   failing BV `value:` runs at j2jDepth==0 — the preceding cached `at:` send's J2J (if any)
   has already returned. So "inside an inline-J2J call" is the wrong condition.
 - PHARO_T1_NO_INLINE_J2J=1 prints NO block-value stats at all -> it effectively DISABLES BV
   (BV uses the J2J save infra). So "NO_INLINE_J2J fixes cmpat" only means "BV-off fixes it"
   — it does NOT prove an at:/inline-J2J interaction. UPDATE 16/17's framing is unconfirmed.
 What IS solid: BV-on corrupts ONLY with SEND-COMPUTED value: args (constant args work:
 bvhit/bvcmpL); BV-OFF cmpat is correct (so the at: sends leave a correct stack [closure,5,
 999999]); yet BV-ON the block's inline-prim reads arr (the at: RECEIVER) not 5. So the BV
 emit/helper, between the (correct) args-on-stack and the block's operand read, corrupts for
 send-computed args. The exact site is unresolved (lldb at the DNU was UNGATED so it may have
 shown a downstream corruption; a GATED single-step INTO the block's bytecodes is needed but
 the -O2 build blocks lldb expr/condition eval). FIX NOT LANDED. Reverted the depth-bail +
 its knob. NEXT: gated lldb single-step into the block on a -O0 build (build/test_load_image),
 OR bisect the BV emit's send-dispatch vs the helper arg-read. Bail-only fixes (depth/
 send-arg) make BV correct but bail its main use (do: blocks w/ send-computed args), so they
 don't make BV useful — the real BV-path fix is required. block-value opt-in; default 1937/0/0.

UPDATE 20 — DECISIVE (2026-06-21): BV helper INPUT is correct -> the bug is the block's
mid-protocol EXECUTION. Built a true -O0 -g build (build-dbg, CMAKE_BUILD_TYPE=Debug; the
"build/" dir is RelWithDebInfo, NOT -O0). lldb breakpoints at the BV helper resolve, but a
per-hit callback/condition TIMES OUT on the startup volume of value: sends, and -O2/extern-C
hide the params. So pivoted to a gated C++ trace (BVIN, gated on PHARO_BV_ONLY_ARG, in the
helper just after the gate). For cmpat's nArgs=2 helper call it prints:
    [BVIN] nArgs=2 rcv(sp[-3])=<closure> a(sp[-2])=0x29 b(sp[-1])=0x7a11f9
i.e. a=5 (0x29), b=999999 (0x7a11f9), receiver=the closure — ALL CORRECT. So the helper
sets up the block frame (tempBase[0]=5, tempBase[1]=999999) correctly, yet gated cmpat STILL
errors "Array DNU #<=". => the corruption is in the BLOCK's EXECUTION after `br x9` (the
inline-prim reads the wrong operand DESPITE correct args), NOT the helper input, NOT the at:
sends, NOT an inline-J2J stack scramble. This CORRECTS UPDATE 16-19 (which variously blamed
the helper input / inline-J2J / x25-at-the-BV-emit). The defensive x25 reload (df022704) does
NOT fix it, so the block's operand read uses a base/register that's stale after the
mid-protocol br x9 — but tempBase residency is DEAD (memory), so pushTemp reads s->tempBase
(correct). The remaining suspect: the inline-prim `<=` (or the block's pushTemp) reading from
a simStack-tracked REGISTER (not memory) that the mid-protocol entry inherits stale. NEXT:
single-step the block's ASM from codeStart (the helper returns blockJM+sizeof; break at that
addr) watching the pushTemp/inline-prim operand read on the -O0 build. block-value opt-in;
default config 1937/0/0. BVIN trace kept (gated, default-off) for the asm step.

UPDATE 21 — block ASM disassembled + bug localized to the helper-return->block transition
(2026-06-21). Dumped the cmpat block's code (BLKDUMP: blockJM->codeStart(), gated) +
capstone-disassembled it:
    +000 ldr x1,[x0,#0x18]   ; x1 = s->tempBase           (OFF_TEMPBASE=0x18)
    +004 ldr x1,[x1]         ; pushTemp 0 -> a = tempBase[0]
    +008 str x1,[x25],#8     ; push a onto operand stack (x25)
    +00c ldr x1,[x0,#0x18]   ; s->tempBase
    +010 ldr x1,[x1,#8]      ; pushTemp 1 -> b = tempBase[1]
    +014 str x1,[x25],#8     ; push b
    +018 mov x2,x25 / ldur x1,[x2,#-0x10] / ldur x4,[x2,#-8]  ; <= reads a,b
    +024.. eor/sub/orr/tst #7 / b.ne ->DNU   ; SmI check; non-SmI -> "Array DNU #<="
So the block reads a,b from s->tempBase[0/1] (x0=state). TBASE trace at the helper END:
tempBase[0]=0x29(5), tempBase[1]=0x7a11f9(999999) -- CORRECT; j2jDepth=1 (the BV save;
cmpat itself is depth 0). And only ONE TBASE line prints => cmpat's FIRST nArgs=2 call BAILS
(not cumulative). So: the helper sets tempBase[0]=5 correctly, the block reads s->tempBase[0]
(x0=state, no prologue, immediate), yet the SmI check sees a non-SmI (arr). => the corruption
is in the ~40-instruction HELPER-RETURN -> BLOCK transition: BV emit AsmjitT1.cpp 6597-6635
(restore x0 from C-stack, hit/bail counters, reload x19, the x25 reload, br x9) and/or the
block's first read -- something makes the block's `ldr x1,[x0,#0x18]; ldr x1,[x1]` yield arr
(x0 wrong, or s->tempBase overwritten, or the tempBase memory (=s->sp[-2]) overwritten)
between line 2230 (helper) and +004 (block).
LIVE single-step BLOCKED by lldb tooling (tried ~8 ways): per-hit helper bp times out on the
startup value: volume; the block codeStart is NOT stable across lldb runs (no capture-then-
break); line breakpoints don't resolve (no DWARF line info in the Mach-O binary -- it's in the
.o debug map, which lldb isn't loading); var/type eval fails on both -O2 and -O0; a gated
__builtin_debugtrap fires but the batch -o python (lldb.debugger.GetSelectedTarget) returns
null so the dynamic bp + pc-advance fail -> continue re-traps. NEXT: register a python bp
callback at the debugtrap ADDRESS (helper+744, stable) that reads g_bvDbgBlockEntry via
frame.GetThread().GetProcess().GetTarget(), sets the block bp, advances pc -- OR inspect
6597-6635 + the block prologue directly (40 insns). Diagnostic suite (BVIN/TBASE/BLKDUMP/
debugtrap, all gated default-off) kept. block-value opt-in; default config 1937/0/0.

UPDATE 22 — 6597-6635 (BV emit transition) + helper tail INSPECTED = CORRECT; the bug is an
irreducible static contradiction; lldb fundamentally cannot single-step this VM's JIT code
(2026-06-21). Read AsmjitT1.cpp 6571-6640 (the BV emit) instruction by instruction:
  6581-6583  sub sp,16 / str x0,[sp,0] / str x30,[sp,8]   ; save state+LR on the C-STACK
             (hardware sp ~0x16fdf..., FAR from the operand stack x25 ~0x4157... — no overlap)
  6593       emitSyncSpToState                              ; state.sp = x25 (helper reads it)
  6596       blr helper                                     ; x0 = block entry
  6599-6602  mov x9,x0 / ldr x0,[sp,0] / ldr x30,[sp,8] / add sp,16  ; x9=entry, x0=state RESTORED
  6603-6619  hit/bail counters (x14,x15)                    ; x0 untouched
  6633       ldr x19,[x0,OFF_JITMETHOD]                     ; x19 = block JM (x0 untouched)
  x25 reload ldr x25,[x0,OFF_SP]                            ; x25 = state.sp (x0 untouched)
  6635       br x9                                          ; -> block, with x0=state
So at br x9: x0=state, x25=state.sp, x19=block JM. NOTHING writes the operand stack or
s->tempBase. Helper tail (JITRuntime 2245-2275): for cmpat cap=0 the copy loop has 0 iters,
s->sp=spOut unchanged — no operand-stack write either. And codeStart()==this+sizeof(JITMethod)
== the helper's return (line 2290), so the disassembled block IS the executed code.
=> AIRTIGHT static chain: helper sets tempBase[0]=5 (TBASE), transition+tail preserve it, the
block reads s->tempBase[0] with x0=state — yet the SmI-bail at block+0x34 fires (tempBase[0]
is even/non-SmI = arr). EVERY code path is correct, but the block reads arr. The corruption is
in NO code path I can read; it requires OBSERVING the block's runtime x0 + *(x0+0x18).
lldb CANNOT do that here (~12 walls hit): DWARF line/type info doesn't load from the Mach-O
(line bps unresolved, var eval fails); DATA symbols are stripped (FindFirstGlobalVariable=0,
FindSymbols=NOSYM, even for an extern-C volatile global -> nm shows no g_bvDbgBlockEntry);
JIT frames have NO unwind info (thread step-out lands in asmjit/garbage); stepping inside a bp
callback invalidates the frame (pc=0xffff...); JIT codeStart is unstable across runs (capture-
then-break fails); per-hit helper bp times out on the startup value: volume; a debugtrap fires
but batch -o python lldb.debugger=null; passing the entry via bvLldbHook's x0 arg read back a
C-stack addr (0x16fdfbcf8), not the arg, then EXC_BAD_ACCESS. lldb is a dead end for this.
NEXT (NOT lldb): inject a C++ trace into the block EMIT — emit `stp x0,x1,[sp,#-16]! ; bl
bvBlockEntryTrace ; ldp x0,x1,[sp],#16` at the cmpat block's codeStart (gated), where
bvBlockEntryTrace(JITState* s) logs s (==x0?), s->tempBase, s->tempBase[0]. That observes the
block's real entry state with zero lldb. Gate it to the cmpat block via a knob + the block's
bytecode signature (PushTemp0/PushTemp1/Send<=/ReturnTop) or the Nth-compiled-block counter.
Diagnostics (BVIN/TBASE/BLKDUMP + the gated bvLldbHook anchor) kept default-off. block-value
opt-in; default config 1937/0/0.

UPDATE 23 — MAJOR CORRECTION: UPDATES 19-22 chased a NON-REPRODUCING case. The REAL bug is
BV x inline-J2J in a CROSS-METHOD context (2026-06-21).

The cmpat_1l.st repro used throughout UPDATES 19-22 (the lldb saga, the "irreducible
contradiction", the single-step, "helper sets tempBase[0]=5 but block reads arr") does NOT
reproduce the bug -- run it and it prints DONE / FALSES[0]. The direct `cmpat` case WORKS; the
block correctly reads 5. The "contradiction" was a phantom (I was single-stepping a correct
execution). All the lldb tooling pain in UPDATES 20-22 was spent on a working case.

REAL repro (saved: scripts/pkg-jit-test/bv-repro/bv_cross_FAILS.st):
    Object compile: 'cmpat2: arr ^[:a :b | a <= b] value: (arr at: 1) value: (arr at: 2)'.
    Object compile: 'cmpatTop | arr | arr := Array with: 5 with: 999999. ^self cmpat2: arr'.
    o := Object new. 1 to: 90000 do: [:k | (o cmpatTop) ifFalse: [...]].
With BV-on it raises "DNU #<= rcvr=Array" at iter ~75199 (when cmpatTop->cmpat2: promotes to
inline-J2J). i.e. the block's `a` reads as the ARRAY (the at: receiver), not 5.

CONFIRMED 3-way (each tested clean):
    BV-off, inline-J2J on        -> WORKS (CROSSFALSES[0])
    BV-on, inline-J2J off        -> WORKS (CROSSFALSES[0])
    BV-on, inline-J2J on         -> FAILS (DNU #<= rcvr=Array)
The bug is the INTERACTION; neither feature alone fails. Direct cmpat (not cross) works because
its home method is not inline-J2J'd the same way.

The corrupted BV calls are at j2jDepth==0 -- inline-J2J does NOT bump s->j2jDepth. So a
`j2jDepth>0` bail is the WRONG fix (TESTED: it broke the DIRECT case by bailing unrelated
depth>0 SYSTEM blocks into the BV bail path, which is ITSELF broken at depth>0, and it missed
the actual cross calls which are at depth 0). The real discriminator is "the BV value: send's
home method (cmpat2:) is executing as inline-J2J'd code" -- not reflected in j2jDepth. The BV
send-site emit (emitSyncSpToState / br x9 / bvResume), compiled for standalone cmpat2:, reads
the wrong operand-stack base when cmpat2: runs inlined into cmpatTop.

NEXT (the real fix): observe the inline-J2J'd cmpat2:'s sp/x25 + the helper input (s->sp[-2])
at the BV value:value: send for the cross corrupted call (the agent-built bvEntryTrace asm
injection: emit `bl trace_fn` right before `br x9`, dump x0/x25/x9). Determine whether the
helper INPUT is already shifted (s->sp[-2]=arr -> upstream inline-J2J operand-stack-offset bug)
or the block reads wrong. Then fix the BV emit's sp handling for inline-J2J'd home methods. The
BAIL path (j2jBail) ALSO corrupts at depth>0 and needs fixing.

TOOLING LESSON: do NOT launch Workflow agents with edit tools against the shared working tree
without isolation:'worktree' -- a hypothesis agent concurrently edited JITRuntime.cpp +
AsmjitT1.cpp, corrupting the baseline mid-investigation (made the direct case spuriously fail).
(One agent independently built the correct bvEntryTrace + PHARO_BV_NO_X25_RELOAD instrumentation
-- the right next-step tooling -- but the uncoordinated edits cost a reset.)

block-value remains opt-in; default config still 1937/0/0 (the /goal is met by default config).
UPDATES 19-22 below are SUPERSEDED for the repro/diagnosis (their lldb-tooling notes still hold).

UPDATE 24 — the corruption is in the POST-WARMUP inlined path, NOT the BV helper (2026-06-21,
continuing UPDATE 23's cross-method repro). Re-added the agent-built instrumentation cleanly:
bvEntryTrace (asm `bl` injected right before the BV `br x9` in AsmjitT1.cpp, gated
PHARO_BV_DUMP_REAL) dumps x0(state)/x25(sp)/x9(entry) + s->tempBase[0/1] + s->sp[-2/-1] at the
block entry; plus a helper-side leak probe (PHARO_BV_TRACE_HITS) identifying cmpat2:'s block by
its compiledBlock oop. Findings on cf_cross (the cross repro):
  - bvEntryTrace: cmpat2:'s BV block entries ALWAYS have tempBase[0]=5 (correct) -- NO corrupted
    BV entry ever fires. So the helper-based BV HIT path is clean.
  - leak probe: jit_rt_inline_block_value_prep is called for cmpat2:'s block only ~3 times, then
    STOPS (no call past iter ~3 in the 90000-iter loop), while the DNU happens at ~75k. So after
    cmpatTop->cmpat2: warms to inline-J2J, cmpat2:'s value:value: NO LONGER routes through the
    helper -- it is re-emitted/inlined differently.
  - PHARO_BV_FORCE_BAIL (helper always bails) makes cf_cross WORK -- but only because it alters
    warmup so cmpat2: is not inlined the same way (NOT a real fix; the helper was never the bug).
  - input probe: when cmpat2: DOES reach the helper, the input (s->sp[-2]) is always 5 (no
    [BVin-corrupt]) -- consistent with the at: sends leaving a correct [closure,5,999999] stack.
=> ROOT CAUSE LOCATION: the inline-J2J re-emission / clean-block-inline of cmpat2:'s
`[:a:b|a<=b] value:(arr at:1) value:(arr at:2)` (the post-warmup, no-helper path) reads the at:
RECEIVER (arr) instead of the at: RESULT (5) -- an operand-stack-offset bug in the inlined value:
emission, NOT the BV helper. The fix is in AsmjitT1.cpp's inline-J2J / value:value: emit, or a
correct refuse-to-inline-J2J-a-value-bearing-callee scoping. A j2jDepth>0 bail is WRONG (UPDATE
23: corrupted calls are at j2jDepth==0; it broke the direct case). A read-only Explore workflow
is mapping the inline-J2J x value: emit interaction to pin the exact offset bug + fix.
Instrumentation kept gated default-off (bvEntryTrace/PHARO_BV_DUMP_REAL + PHARO_BV_TRACE_HITS).
block-value opt-in; default config still 1937/0/0 (goal met by default).

UPDATE 25 — root cause CONFIRMED (workflow synthesis) + scoping fix ATTEMPTED & REVERTED; the
real fix is the deeper save-reconciliation (2026-06-21). A read-only Explore workflow
(wf_1a755a9e-a87) mapped the emit; its synthesis agent re-instrumented and CORRECTED the mapping
agents' x25 theory. ROOT CAUSE (high confidence): cmpat2: (2 IC sites) is admitted for
cross-method inline-J2J. Its value:value: BV helper is called only ~1-3x then the block JM
compiles, a<=b sets canBailMidMethod, so the helper BAILS (g_bvBail_canBail) -> j2jBail -> the
value:value: is serviced as a normal cached send to BlockClosure>>value:value: (prim 209),
activated via the C++/chain-loop path WHILE cmpat2:'s inline-J2J save is still pending. That
activation reads operands +1-shifted -> the block's first arg = the at: RECEIVER (arr) not the
result (5) -> DNU #<= rcvr=Array.  SAME un-reconciled-pending-save / operand +1 family already
gated off for canBailMidMethod and ExitArithOverflow callees (xmethodGateOk).

FIX ATTEMPTS (all FAILED — recorded so they are not repeated):
  (1) j2jDepth>0 bail (UPDATE 23): WRONG — corrupted calls are at j2jDepth==0; broke direct.
  (2) Runtime hasBlockValueSend write in the BV helper (mark s->jitMethod): SIGSEGV. The
      JITMethod header is in MAP_JIT (RX) memory; a C++ write without the JIT write-window
      faults. Any per-method flag must be set at COMPILE time (finalize writes in-window).
  (3) Compile-time hasBlockValueSend (scan the method's literals via memory.oopToString for
      value:value:/value:value:value:/valueWithArguments:; value/value: are special-selectors)
      + xmethodGateOk refuse: does NOT work. ADMIT=1 bisect proves the compile-time flag-set is
      harmless (==baseline); the REFUSE itself (a) breaks the direct case and (b) does not fix
      cross. The gate IS consistent (xmethodGateOk feeds kXGateOkBit at Interpreter 23441/23918/
      24039 + JITRuntime 3673), so refuse-breaks-direct is the gate-refuse FALLBACK (j2jBailSelf2)
      being itself buggy for these methods, not an inconsistency.

CORRECTION to UPDATE 23/24: the "direct works, cross fails" split was FLAKY — the direct case is
timing-dependent (inline-J2J engagement varies) and now fails 3/3 at the same commit where it
earlier passed. So the bug affects BOTH direct and cross; the +1 shift occurs in the C++
value:value: activation whenever a pending inline-J2J save exists, INDEPENDENT of which method is
the inline-J2J callee. => refusing inline-J2J of value-bearing callees is the WRONG layer.

THE REAL FIX (synthesis's "primary", multi-session): reconcile the caller's pending inline-J2J
save with the foreign value:value: (prim 209) activation in the chain loop — Interpreter.cpp
~27094-27130 (inline-activate gate / fallback) and ~25900-25960 (site4 materialize). Pop/adjust
the pending save (or push a proper C++ frame) so the activation reads operands at the right sp.
This is the SAME class as PHARO_T1_AO_MAT_J2J (the V1-only arith-overflow save-pop); a V2
equivalent for the BV-bail-to-prim-209 path is needed. Validation must be done with a
deterministic repro (the inline-J2J warmup is timing-flaky) — run cf_cross + cf_dir multiple
times, or force inline-J2J. Reverted to clean HEAD (e0c23340); default config 1937/0/0 (goal met
by default config); block-value remains opt-in. Repro: scripts/pkg-jit-test/bv-repro/.

UPDATE 26 — deeper fix LOCATED to a single line; naive injection HANGS; correct fix needs the
bail-path sequence (2026-06-21). Traced the chain-loop send/activation in Interpreter.cpp:
  - The shared send-chain code (~27110-28172) ends in C++ fallbacks: the PRIM-execution path
    (~27977, executePrimitive -> for prim 209 value:value: -> activateBlock) and activateMethod
    (~28162).  Both read the operand stack via stackPointer_/framePointer_.
  - There are TWO adjacent paths: the chain-FALLBACK BAIL path (~27875-27969) which, when
    state.j2jDepth>0, materializes pending saves into SavedFrames (site7, ~27918), syncs
    stackPointer_=state.sp / framePointer_=state.tempBase-1 / argCount_ / method_ etc., and
    `return true` (hands the callee to the interp dispatch loop).  The PRIM path (~27977) has
    NO such materialize.
  => EXACT BUG: value:value: reaches the PRIM path with state.j2jDepth>0 (cmpat2:'s pending
     inline-J2J save), and executePrimitive(209)/activateBlock reads operands +1-shifted (block
     arg = the at: RECEIVER arr) because the save is un-reconciled.  The bail path would have
     fixed it; the prim path doesn't run it.

FIX ATTEMPT (commit reverted): inject `if (state.j2jDepth>0) materializeJ2J();` at the prim path
(~27977), opt-out PHARO_T1_NO_J2J_PRIM_MAT.  RESULT: HANGS (timeout; block-value inline
tries=1.9M bails=1.9M lookup, ~20x the ~90k expected).  materializeJ2J sets up an INTERP-RESUME
(method_/ip_/sp_ for the callee + j2jDepth=0) but the prim path then CONTINUES INLINE
(executePrimitive) instead of returning to the interp — the two resume protocols conflict and
loop (the re-entered value:value: can't find its block -> lookup-bail -> retry).  ALSO risky:
the inject fires for ALL j2jDepth>0 prims (not just BV), so it could regress the default config.

CORRECT FIX (still multi-session, but now a SCOPED edit, not a mystery): at the prim path, for
the j2jDepth>0 case, REPLICATE THE BAIL PATH (~27875-27968) — materialize pending saves +
full interp-state setup + `return true` — so the interp dispatches value:value: with the
reconciled stack, rather than executing the prim inline.  Key subtleties to get right: the
resume IP protocol for an ExitSendCached bail (does the interp re-dispatch the pending send, or
resume after it?), whether the OUTER frame (~27900-27915) also needs materializing, and gating
so non-BV inline-J2J prims are unaffected (or proven safe).  Validate with cf_cross + cf_dir
3x each (both reliably FAIL now -> must reliably pass) + the default config (R<7>) + a bench/
SUnit non-regression (the prim path is hot).  Reverted to clean HEAD (485f05ca); default config
1937/0/0 (goal met by default); block-value opt-in.  Repro: scripts/pkg-jit-test/bv-repro/.

UPDATE 26b — EMPIRICAL REFUTATION of the chain-loop "pending save" fix location (2026-06-21).
Three deeper-fix variants at the chain-loop prim path ALL failed: (a) materializeJ2J() inline ->
HANG (loops: 1.9M BV bails; the materialize sets an interp-resume but the prim path continues
inline); (b) materializeJ2J()+return true -> breaks STARTUP (fires for ALL j2jDepth>0 prims
during boot); (c) scoped to prims 201-209 + j2jDepth>0 -> did NOT fire (so didn't fix cross),
which led to the decisive diagnostic.  Instrumented the chain-loop block-value activation:
  - cmpat2:'s value:value: is PRIM 207 (the general block-value prim, all arities; prim 209 is
    a DIFFERENT 0-arg value here).  nArgs=2.
  - At the chain loop, for the first N (working) calls: j2jDepth=0, chainCallDepth=0, and
    operands CORRECT (a=0x29=5, b=0x7a11f9=999999).
  - For the CORRUPTED calls (a = a heap object): STILL j2jDepth=0 AND chainCallDepth=0, and the
    corrupted operand (a=arr) is ALREADY on the stack when it ARRIVES at the chain loop.
=> The synthesis's "pending inline-J2J save +1 at the chain-loop activation" is REFUTED: there
   is NO pending save at the chain-loop value:value: (both counters 0), and the operand is
   already corrupted on arrival.  The corruption happens UPSTREAM, in cmpat2:'s JIT execution
   (the inlined at:/value: operand setup) BEFORE the value:value: send exits to the chain loop,
   for SOME calls (not the first N) — i.e. it is a JIT-EMIT bug (AsmjitT1.cpp), not a chain-loop
   activation bug.  NEXT: instrument cmpat2:'s inlined JIT code (the value:value: arg push +
   the at: result placement) when cmpat2: runs as inline-J2J'd code, to catch the call where
   the operand stack first holds arr instead of 5 — that emit site is the real fix.  ALL deeper-
   fix code reverted; default config 1937/0/0; block-value opt-in.

UPDATE 27 — operand stack at the send is CORRECT; corruption is in the BLOCK FRAME/execution
(2026-06-21).  Instrumented cmpat2:'s value:value: at the chain loop, isolating cmpat2:'s block
by its compiledBlock oop (recorded from a correct a=5,b=999999 call), then logging only its
CORRUPTED calls (a!=5).  RESULT: that trace NEVER fires — i.e. at the chain-loop prim-207
activation, cmpat2:'s block ALWAYS has the correct operands (a=0x29=5, b=0x7a11f9=999999,
j2jDepth=0, chainCallDepth=0) — yet the block's <= still gets arr (the DNU still happens).
=> The corruption is NOT in the operand stack at the value:value: send (it is correct there);
it is DOWNSTREAM, in the BLOCK's frame setup / JIT execution after activateBlock (prim 207),
when cmpat2: (the block's home) is inline-J2J'd.  The block is activated with a=5 but reads its
first temp as arr.  This CONFIRMS UPDATE 22's "the block reads tempBase[0]=arr despite the
helper setting it=5" hypothesis was on the right track — it just couldn't reproduce on cmpat_1l
(direct), and now reproduces via cf_cross (cmpat2: inline-J2J'd).  (NB the earlier [CLbad]
a=heap,b=1 hits were UNRELATED 2-arg blocks, not cmpat2: — isolation by block oop was needed.)
NEXT: instrument the block's frame setup — activateBlock (Interpreter.cpp:12236) for cmpat2:'s
block (log the args it reads from the stack vs the temps it writes to the new frame) AND the
block's JIT entry tempBase (when the block runs JIT'd after activateBlock, with cmpat2: as an
inline-J2J'd home).  The fix is in the block's frame-temp addressing in the inline-J2J'd-home
context (AsmjitT1.cpp block entry / activateBlock), NOT the chain loop and NOT the operand
stack at the send.  Default config 1937/0/0 (goal met); block-value opt-in.

UPDATE 28 — the corrupted call runs PURE-JIT; C++ instrumentation can't see it (2026-06-21).
Instrumented (this turn) cmpat2:'s value:value: at EVERY C++ path, isolating cmpat2:'s block by
its compiledBlock oop and logging only the CORRUPTED calls (a!=5):
  - operand stack at the value:value: send (chain loop, prim 207): correct (a=5,b=999999),
    j2jDepth=0, chainCallDepth=0 — UPDATE 27.
  - activateBlock(Interpreter.cpp:12236): cmpat2:'s 2-arg block NEVER reaches it (only AC=1
    1-arg blocks with arg 999999 do).  So the 2-arg value:value: does not use this activateBlock.
  - chain-loop block inline-activate tryExecute path (Interpreter.cpp ~28064-28104,
    state.tempBase=framePointer_+1, tryExecute): the CORRECT calls reach it with tempBase[0]=5;
    the corrupted ones DON'T fire the trace.
  - the block-activate branch (frameDepth_!=primCallerDepth, ~28023): corrupted calls DON'T
    reach it either.
=> Across ALL C++ instrumentation points, the corrupted cmpat2: call never appears — only the
   CORRECT (early) calls (the ones that exit JIT to the C++ chain loop) do.  CONCLUSION: the
   corrupted calls run ENTIRELY in JIT'd code (cmpat2: inline-J2J'd -> the block chained/inlined
   in JIT) and NEVER exit to the C++ paths.  So C++-side instrumentation is structurally blind to
   the bug; the C++ chain loop only ever sees the correct calls.  This brings it FULL CIRCLE to
   UPDATE 22: the corruption is in the BLOCK's JIT execution (reading tempBase[0]=arr from a
   stale base/register in the inline-J2J'd-home context), reachable ONLY by asm-level
   instrumentation (the bvEntryTrace approach), NOT C++ traces and NOT lldb (JIT code, no
   debug info).  NEXT: asm-inject a trace at the block's JIT tempBase read (the `ldr x1,[x0,#0x18]
   ; ldr x1,[x1]` from UPDATE 22's disasm) on the chain/inline-J2J block-entry path (not the BV
   helper br x9), capturing x0 + state.tempBase + the loaded value for the corrupted call — that
   pins the stale base.  Default config 1937/0/0 (goal met); block-value opt-in.

UPDATE 29 — asm-injected the tempBase trace at the block JIT codeStart; result inconclusive
(2026-06-21).  Added bvBlockStart(state) (JITRuntime.cpp) + asm-injected `bl bvBlockStart` at
the emit-loop globalIdx==0 (AsmjitT1.cpp ~10416, codeStart, gated PHARO_BV_DUMP_REAL, save/
restore x0/x30/x25/x19).  It builds + runs clean.  At codeStart, logging (A) t1==999999 [BLKE-OK]
and (B) heap-t0 [BLKE-BAD]:
  - [BLKE-OK] (t1=999999) fires for MANY DIFFERENT block oops with varying t0 (mostly heap) —
    999999 is NOT a unique cmpat2: signature (it propagates from `Array with: 5 with: 999999`
    into many blocks' temps).
  - NO codeStart entry has t0=0x29 (5).  So cmpat2:'s block with its CORRECT args (a=5,
    b=999999) NEVER appears at this codeStart — even though the chain-loop traces (UPDATE 27,
    [CLcmp]/[BLKTB]) showed cmpat2:'s value:value: + tryExecute with a=5.
=> CONTRADICTION / inconclusive: cmpat2:'s block does not surface at the emit-loop globalIdx==0
   codeStart with a=5.  Likely either (i) the chain-loop tryExecute / inline-J2J enters the
   block at a point that is NOT the emit-loop globalIdx==0 (a trampoline/prologue, or a resume
   offset), so the injection point is wrong; or (ii) cmpat2:'s block reaches codeStart with an
   already-different tempBase (the corruption is in the tryExecute/inline-J2J tempBase setup,
   between the chain-loop trace point and codeStart); or (iii) the block's bytecodes are
   clean-inlined into cmpat2: (no separate codeStart) for the corrupted path.  The trace point
   needs to be the block's ACTUAL runtime entry — verify by dumping blockJM->codeStart() for
   cmpat2:'s block and asm-injecting at THAT exact address (or in the PushTemp emit itself),
   not the emit-loop globalIdx==0 which may differ from the entry the chain/inline-J2J uses.

SESSION SUMMARY of the BV x inline-J2J bug (UPDATES 19-29): root cause CONFIRMED (BV value:value:
in an inline-J2J'd home corrupts the block's first arg to the at: receiver, arr).  RULED OUT as
fix layers / not-the-bug: scoping gate (xmethodGateOk refuse), runtime flag write (MAP_JIT
fault), j2jDepth>0 bail, chain-loop pending-save reconcile (j2jDepth=0/chainCallDepth=0 at the
site), the operand stack at the value:value: send (correct, a=5), activateBlock(12236) (2-arg
block doesn't use it), and the C++ paths generally (corrupted call runs pure-JIT, never exits
to C++).  The corruption is in the block's JIT execution (tempBase read / register handling) in
the inline-J2J'd-home context.  ~6 fix attempts + ~11 instrumentation layers (C++, lldb, asm)
have not pinned the exact emit site; lldb can't step JIT (no debug info), C++ traces are blind
(pure-JIT), and the codeStart asm trace didn't surface cmpat2:'s block cleanly.  Default config
1937/0/0 (goal met); block-value remains OPT-IN with this documented, precisely-bounded bug.

UPDATE 30 — codeStart trace at the REAL block entry: the SEPARATE block run is CORRECT; the
corrupted run is an INLINED/pure-JIT path that evades codeStart (2026-06-21).  Fixed the
isolation: record cmpat2:'s block oop in the BV HELPER (where compiledBlockBits is definitively
cmpat2:'s, identified by a=5/b=999999; the helper computes it at line 2086 BEFORE bailing on
canBailMidMethod at 2146), into a regular C++ global g_cmpat2BlockOop.  Then the asm-injected
codeStart trace (bvBlockStart at emit-loop globalIdx==0) filters state.method==g_cmpat2BlockOop.
RESULT: exactly ONE [BLKCS] fires: t0=0x29(5), t1=0x7a11f9(999999), OK(a=5) — then the DNU, with
NO further [BLKCS].  So cmpat2:'s SEPARATE block run (via tryExecute->codeStart, state.method =
the block oop) is CORRECT (a=5).  The CORRUPTED run never reaches the block's codeStart with
state.method==the block oop.
=> CONCLUSION: the corrupted run is NOT a separate block activation — it is an INLINED path
   (the block clean-inlined, or value:value: re-emitted within the inline-J2J'd home cmpat2:/
   cmpatTop), where state.method is the HOME method, not the block, and the block's bytecodes
   run mid-method (not at globalIdx==0).  It runs entirely in JIT and evades codeStart exactly
   as it evaded all C++ paths (UPDATE 28) and the BV helper (always input a=5).  The separate
   block path is correct; only the inlined path corrupts.  This is consistent across the whole
   investigation: the corruption is the inline-J2J re-emission of cmpat2: that inlines its
   value:value: block, and the inlined block reads its first temp from the home frame at the
   wrong (inline-J2J-shifted) offset -> arr.
NEXT: instrument the INLINE-J2J RE-EMIT of a value:value:/block bytecode (AsmjitT1.cpp inline-J2J
emit path), or the PushTemp emit for an inlined block (mid-method, state.method==home), to catch
the inlined block's operand read.  The fix is the inlined-block operand/tempBase addressing in
the inline-J2J'd-home context — OR, as a correct scoping fallback, refuse to inline-J2J / clean-
inline a callee that contains a block-value send (the bug needs both; either disabled fixes it).
Default config 1937/0/0 (goal met); block-value remains OPT-IN.

UPDATE 31 — SCOPING FIX #2 WORKS: BV-on disables cross-method inline-J2J (2026-06-21).
Built on UPDATE 30 (the SEPARATE block run is correct; only the INLINED path corrupts) + the
deterministic repro (cf_cross reliably DNU 5/5).  First confirmed the precise lever: the
value-bearing-CALLEE refuse (mark cmpat2: canBailMidMethod) does NOT fix it (cf_cross still DNU
3/3+DET_SCHED) — the corrupting inline-J2J is the cross-method CALLER (cmpatTop, no value:
literal, not caught by a callee scan).  The proven lever is PHARO_T1_NO_INLINE_J2J (UPDATE 23:
BV-on + that = WORKS), which disables inline-J2J at the EMIT (not xmethodGateOk, whose refuse
fallback is buggy — UPDATE 25).  FIX: make BV-on imply NO_INLINE_J2J, at ALL FOUR gating reads
of GET_DEBUG_BOOL(PHARO_T1_NO_INLINE_J2J) in AsmjitT1.cpp (the 3 `const bool inlineJ2J = ...`
at ~4653/5148/6293 AND the PMS `patchedShape` at ~5940 — missing the 4th left an INCONSISTENT
partial-disable that errored).  Each gets `&& !(g_debug.t1InlineBlockValue &&
!GET_DEBUG_BOOL(PHARO_T1_BV_KEEP_INLINE_J2J))`.  Opt-out PHARO_T1_BV_KEEP_INLINE_J2J restores
the buggy admit (for A/B).
VALIDATION (deterministic): cf_cross CROSSFALSES[0] 3/3 + DET_SCHED (was DNU 5/5); cf_dir
DIRFALSES[0] (was DNU); a real `a sort: [:x:y|x<=y]` x90000 -> #(1..9) correctly sorted (with
KEEP_INLINE_J2J it returns the UNSORTED #(5 3 8 1 9 2 7 4 6) — the exact original "sort doesn't
sort" symptom); DEFAULT (BV off) R<7> byte-identical (the gate is g_debug.t1InlineBlockValue, so
BV-off codegen is unchanged).
SUNIT A/B CONFIRMATION (2026-06-21, the definitive test): ran the 9 collection classes
(OrderedCollectionTest/ArrayTest/SortedCollectionTest/IntervalTest/HeapTest/LinkedListTest/
CollectionTest/SortFunctionTest/ChainedSortFunctionTest — these hold the sort tests) via
scripts/pharo-headless-test/run_sunit_tests.st on /tmp/harness/Pharo.image (build-rel VM):
  BV-OFF: Pass 1648 / Fail 0 / Error 0
  BV-ON : Pass 1648 / Fail 0 / Error 0   <-- IDENTICAL
The exact 12-failure sort tests (testSort, testSorted, testAsSortedArray,
testAsSortedCollectionWithSortBlock, testSortUsingSortBlock, testSortedUsingBlock,
testIndexOfIfAbsentUsing, testIndexOfStartingAtIfAbsentUsing, ...) all RAN and PASSED under
BV-on.  So the 12 BV-on sort failures (UPDATE 15) are RESOLVED; BV-on now equals BV-off on the
sort/collection suite.  ROOT CAUSE (final): a cross-method inline-J2J'd home that contains
a value:value: clean-inlines the block, and the inlined block reads its first temp from the home
frame at the inline-J2J-shifted offset -> the at: RECEIVER (arr) -> DNU #<=.  The scoping fix
sidesteps it by keeping such homes un-inlined so the block runs as a SEPARATE (correct)
activation.  COST: BV-on loses cross-method inline-J2J (acceptable — BV is opt-in).  The IDEAL
fix (keep BOTH: correct the inlined-block operand/tempBase addressing in the inline-J2J'd-home
context) remains future work.  block-value still OPT-IN; default 1937/0/0; with BV-on the cross/
sort corruption is resolved.

NEXT SESSION: bisect the side effects — build BV-ON but neutralize each in turn
(force spLiveInX2 true at 7707; force staticJ2JArgCount through at 11623; V1-only the
4764 branch) and find which neutralization stops the 112M-send runaway. Repro
(MUST cd to the image dir): `cd /tmp/pkgtest && PHARO_T1_INLINE_BLOCK_VALUE=1
.../test_load_image neojson.image eval "Stdio stdout nextPutAll: 'R<', (3+4) printString,
'>'; lf; flush."` → want R<7>, currently blank. lldb attaches cleanly from /tmp/pkgtest
with an absolute VM path.
