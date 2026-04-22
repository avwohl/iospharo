# Bug 14 artifacts (2026-04-22)

Reference logs from the bug-14 diagnosis session.  The minimal
reproducer is 30 seconds; both logs come from fresh Pharo 13.1
images with the unified harness fileIn'd, running

    (IntegerTest selector: #testNthRootTruncated) runCase

with and without JIT.

## Files

- `jit-slot-tripwire-2026-04-22.log` — JIT on, `PHARO_JIT_DEFER=0
  PHARO_SLOT_TRIPWIRE=1`, timeout 30s.  Hangs; shows 3 DNU events
  with SmallInteger receivers.  Key grep targets:
    - `\[TERM-P`        — scheduler-process terminations
    - `#atEnd not`      — the three DNU signatures
    - `lastJitReturn`   — what the JIT returned just before the DNU
    - `\[SLOT-TRIPWIRE\]` — every slot-0 nil-write on a context-shaped
                            object (318 events over 30s, all legitimate
                            NLR kill-walks)
- `nojit-compare-2026-04-22.log` — same invocation with
  `PHARO_NO_JIT=1`.  Completes in <30s with `'done'`, zero DNUs,
  zero TERM events.  Reference baseline for what "correct" looks like.

## What the logs prove

1. NLR-overwalk hypothesis is falsified (every NLR walk reaches
   its legitimate homeCtx; capping the walk length had zero effect).
2. Sender-bypass hypothesis is falsified (the terminated p80
   context's sender was never written by any `storePointer` or
   `slotAtPut` call — it was nil from creation).
3. The JIT vs. NO_JIT diff pins the difference at three DNU events:
   `#atEnd` sent to SmallInteger 6 / 0 / 37.  In each case the
   lastJitReturn logged a proper object return value, but the
   caller's receiver stack slot now holds an adjacent SmallInteger
   (looks like a loop-counter value).

## DNU #3 stack-slot snapshot

The clearest single snapshot.  DNU fires at fd=14 in
`BlockClosure>>whileFalse:` after JIT returns from `on:`:

    method_=0x3003f8158  (BlockClosure>>whileFalse: JIT-compiled)
    FP[-2] = 0x30344ced8
    FP[-1] = 0x30344d470  <- retVal from #on:
    FP[0]  = 0x30344ced8
    FP[1]  = 0x30344d470  <- retVal from #on: AGAIN (duplicate?)
    FP[2]  = 0x129        <- SmallInteger 37, used as #atEnd receiver

The on: return value shows up in two slots (FP[-1] and FP[1]).
SmallInteger 37 is probably a UTF-8 byte (call stack has
`utf8Decoded → decodeBytes: → decodeWith:` at frames 9-11).  Some
operation during the whileFalse: loop body writes a byte into
the stream-receiver slot.

## Next debugging step

**IMPORTANT gotcha discovered 2026-04-22**: `src/vm/jit/stencils/stencils.cpp`
is NOT linked into the VM.  It's compiled separately by
`scripts/extract_stencils.py` and the raw machine code is copied
into `generated_stencils.hpp` as byte arrays.  Adding a printf to
stencils.cpp and rebuilding the VM does nothing — you also need to
rerun `python3 scripts/extract_stencils.py` after editing.  The
stencil build doesn't happen automatically; check the memory note
"Regenerate stencils after C++ changes".

Verified during this session: instrumenting the *C++-side* J2J
return path at `Interpreter.cpp:12685`
(`state.sp[-(save.sendArgCount + 1)] = retVal;`) produced **zero
events** under the reproducer.  That means all 3 problem returns
go through the **pure stencil path**, not the trampoline bail.
Instrumentation has to land inside the stencil code (requires
regen) or at the boundary where the stencil exits back to C++.

### Three targets, now ordered by tractability

1. **Instrument the return stencils themselves, THEN regen.**
   Edit `J2J_INLINE_RETURN_IMPL` in `src/vm/jit/stencils/stencils.cpp`
   to log `_sv->sp`, `_nArgs`, and the old/new value at the write
   slot. Run `python3 scripts/extract_stencils.py`.  Rebuild.  Run
   `docs/bug14/reproduce.sh`.  Grep for the 3 atEnd DNU events
   and the J2J-RET events immediately preceding each — find the
   return whose `retVal` matches the next DNU's `rcvr` or whose
   write-offset is wrong.
2. **Disassemble JIT-compiled method `BlockClosure>>whileFalse:`**
   (oop varies per run but selector is stable, numArgs=1,
   numTemps=1, 18 bytecodes starting at PC 41).  Bytecodes from
   this session's dump:
        41: 4C   42: 79   43: C1   44: 4F   45: B5   46: 40
        47: 79   48: D8   49: 4C   50: 40   51: 90   52: D8
        53: 5B   54: 00   55: 00   56: 3C   57: 1C   58: 6C
   Literals: {#whileFalse:, #ifFalse:, #whileFalse:,
   #BlockClosure->BlockClosure}.  Source:
        whileFalse: aBlock
          self value ifFalse: [ aBlock value. self whileFalse: aBlock ].
          ^ nil
   The DNU consistently fires in this method after a return from
   `#on:`, so the corrupted bytecode is somewhere in this 18-byte
   sequence.
3. **Prim 272 (`whileFalse`) specialization** — check `stencils.cpp`
   for any primitive-272 handling; if present, its stack arithmetic
   is the likely culprit.

### Stability across runs

Re-ran reproducer 2026-04-22 04:40: 3 DNUs identical in shape,
receivers this time 6, 6, 36 (vs. first run 6, 0, 37 — same class
of loop-counter values, different specific numbers).  Pattern is
deterministic but the exact clobbering value varies based on
which control-flow path gets hit first.  Method IDs (like
`method=#on:(0x300469fd0)`) are stable within a run but change
across fresh image downloads because hash varies.

### 2026-04-22 late — B5 trace nails the root-cause method pattern

Enabled `PHARO_B5_TRACE=1` (existing infra at
`JITRuntime.cpp:97 jit_rt_j2j_trace`) and extended the RET event
logger with `selectorOf(callerCM)` lookup.  Reproducer produced:

    [B5] #107  RET depth=0 retVal=0x31 (SmI  6) callerCM=0x30046cca0 savedArgs=1 sel=#contents
    [B5] #350  RET depth=0 retVal=0x31 (SmI  6) callerCM=0x30046cca0 savedArgs=1 sel=#contents
    [B5] #1800 RET depth=0 retVal=0x129 (SmI 37) callerCM=0x30046cca0 savedArgs=1 sel=#contents

**retVal values match DNU receivers exactly** (SmI 6, SmI 37).
All 3 problematic returns are for method `#contents` with
`savedArgs=1`.  The prime suspect for which class:

    OCScanner>>contents
        | contentsStream |
        contentsStream := (Array new: 50) writeStream.
        [ self atEnd ]
          whileFalse: [ contentsStream nextPut: self next ].
        ^ contentsStream contents

This method has the exact signature:
- `[ self atEnd ] whileFalse: [ ... ]` loop — matches "in #whileFalse:"
- 1-arg send: `contentsStream nextPut: self next` — matches savedArgs=1
- `self next` returns a byte (SmallInteger) — matches retVal tag=1

### Hypothesis / fix target

The loop-body bytecode sequence is:
    push contentsStream
    push self
    send #next       → SmallInt byte on stack
    send #nextPut:   → returns the SmallInt (its arg)
    pop              → discard loop-body result
    jump back to loop header

After the `nextPut:` J2J return, retVal = SmallInt byte lands at
`sp[-(nArgs+1)] = sp[-2]`.  If the subsequent `pop` doesn't fire
correctly (miscompiled or elided by the JIT's jump-back
optimization), the SmallInt stays on the stack.  Next iteration
pushes `self` for the `atEnd` send, but the stack slot computed
as receiver is the leftover SmallInt.

### Concrete next step

Disassemble `OCScanner>>contents` and compare:
1. Bytecode layout, particularly the `pop`/`jump` sequence after
   the nextPut: send
2. JIT-emitted code for the same offsets — look for missing
   pop-stencil emit, or a jump-back-target arithmetic bug that
   lands the IP 1 byte past the pop

### OCScanner>>contents bytecodes (captured 2026-04-22)

size=90, initialPC=65, numArgs=0, numTemps=1.  All bytecodes
decoded:

    65: 0x10 pushLitVar 0        (Array class)
    66: 0x21 pushLitConst 1      (50)
    67: 0x7D specialSend new:    (Array new: 50)
    68: 0x82 send lit[3]=writeStream   (→ WriteStream)
    69: 0xD0 popIntoTemp 0       (contentsStream := ...)

    ;; Loop header @70 — the compiler inlined the whileFalse:
    70: 0x4C pushReceiver        (self)
    71: 0x75 specialSend atEnd
    72: 0xEE ExtJumpTrue +9      (if atEnd, exit loop to @83)

    ;; Loop body @74
    74: 0x40 pushTemp 0          (contentsStream)
    75: 0x4C pushReceiver        (self)
    76: 0x73 specialSend next    (returns SmallInt byte)
    77: 0x74 specialSend nextPut: (1 arg — THE J2J CALL, returns arg)
    78: 0xD8 Pop                 (discard result)
    79: 0xE1 ExtendB 0xFF        (-1 extended signed byte for jump)
    80: 0xFF operand
    81: 0xED ExtJump 0xF3        (computed: (-1 << 8) | 0xF3 = -13)
    82: 0xF3 operand             (target = 83 + (-13) = 70 ← loop head)

    ;; After loop @83
    83: 0x40 pushTemp 0          (contentsStream)
    84: 0x83 send lit[4]=contents (→ the Array-as-content)
    85: 0x5C returnTop

Literals: `{#Array→Array, 50, #writeStream, #contents, #whileFalse:,
#contents, #OCScanner→OCScanner}`.  (The `#whileFalse:` literal
survived as an artifact of the compiler's inlining — it's never
dispatched but the literal entry sticks.)

### Where the bug is hiding in here

Everything hinges on the four bytecodes @77–@82:

    77: 0x74  specialSend nextPut:  (1 arg — J2J call)
    78: 0xD8  Pop                   (discard nextPut:'s result)
    79: 0xE1  ExtendB 0xFF
    80: 0xFF
    81: 0xED  ExtJump 0xF3
    82: 0xF3

After the J2J return from @77 writes retVal at `sp[-(1+1)] = sp[-2]`
and sets `sp -= 1`, the caller's stack top is the SmallInt byte.
The Pop @78 must decrement sp by 1 to discard it, then the
ExtendB+ExtJump must jump back to @70.  If any of:

  (a) the JIT peephole-fuses nextPut:+Pop into a "call and
      discard" opcode that gets the math wrong (the retVal
      landing slot isn't adjusted for the fused Pop)
  (b) the Pop stencil doesn't fire after a J2J resume
      (resumeAddr points past it, skipping the sp decrement)
  (c) the ExtJump target computation lands 1 byte off (at @71
      instead of @70, skipping the pushReceiver so the atEnd
      send reads the stale SmallInt as receiver)

… the bug reproduces exactly as observed: SmallInteger shows up
as `atEnd`'s receiver, DNU fires, cascade terminates scheduler
processes.

### Where to look

- `JITCompiler.cpp` emit paths for `SpecialSend nextPut:`, `Pop`,
  `ExtendB`, and `ExtJump` — specifically the SimStack / peephole
  layer that merges adjacent opcodes.
- Search for whether `nextPut:` has an inline fast-path stencil
  (`stencil_primAtPut` or `stencil_sendInlineSetter`) whose retVal
  accounting doesn't match the J2J return path.
- Check `_HOLE_BRANCH_TARGET` arithmetic when the branch origin
  includes an `ExtendB` prefix.  The jump target is computed
  against the IP after the last byte of the jump opcode;
  accidentally using IP after `ExtendB`'s operand could shift by 1.

### 2026-04-22 late — JIT emit audit findings

Audited the emit paths in JITCompiler.cpp line-by-line against
this specific bytecode sequence.  What looks *correct* so far:

- **ExtJump target** (line 398-404): `branchTarget = i + 2 +
  offset` where i = ExtJump's 0-indexed position.  For our
  method, i=80, offset = 0xF3 + (extB=-1 << 8) = -13, target =
  69 (0-indexed) = 70 (1-indexed, the loop header).  ✓
- **ExtendB sign-extension** (line 296-307): when the operand's
  high bit is set, ORs in `0xFFFFFF00` to sign-extend.  Our
  0xFF correctly becomes -1.  ✓
- **Special sends** (`nextPut:`, `next`, `atEnd` at 0x70-0x7F)
  all map to `stencil_send` — the same code path as literal
  sends.  No peephole specialization.  So candidate (a)
  (nextPut:/Pop fusion) is unlikely because there's no fusion.
- **Pop emit**: at entry state 0 (after send barrier), emits
  `stencil_pop_E`, which is literally `s->sp--; _HOLE_CONTINUE(s);`
  — a single sp decrement.  No way this is wrong in isolation.
- **J2J_INLINE_RETURN tail-call to resumeAddr** (line 509-512):
  `if (_resume != 0) ((void(*)(JITState*))_resume)(s); return;`
  — tail-calls next stencil.  resumeAddr was saved from
  `RESUME_ADDR` hole at save time, which resolves to the next
  stencil's entry per JITCompiler line 789-799.  That's Pop_E.
- **Send barrier state math**: send is a barrier → flush to
  state=0 before send.  State stays 0 after send (retVal lands
  in memory).  Pop at state=0 → stencil_pop_E ✓.
- **Branch target entry flush** (line 984): `if
  (isBranchTarget[i] && state != 0) insertFlush`.  Loop header
  is a branch target, will be entered with state=0.  ✓

So candidates (a), (b), and (c) all *look* wired correctly in
isolation.  The bug still reproduces deterministically, so
something in the interaction must be off — possibly:

1. State mismatch at branch-target *entry* if the backward
   branch source flushed differently than the emit assumes.
2. `resumeAddr` being wired to the wrong stencil when the
   bytecode after a send is itself part of an extend-prefix
   sequence.  For OCScanner>>contents, the bytecode immediately
   after `nextPut:` (@77) is `Pop` (@78), but two bytes later
   is `ExtendB` (@79).  If the JIT somehow targets the stencil
   for the *2nd* bytecode-after-send instead of the 1st, we'd
   skip Pop and resume at ExtendB — which is a nop stencil.
   The ExtJump back would then fire, SmallInt stays on stack.
3. `decodeBytecodes` emitting a `stencil_nop` for ExtendB that
   runs *before* Pop, reordering the sequence.  At line 303-305,
   ExtendB's `bc.stencilIdx` is `stencil_nop` — meaning each
   `DecodedBC` for ExtendB gets emitted as a nop.  The real
   work of ExtendB is in the extB accumulator at *decode* time,
   consumed by the following bytecode's emit.  Sequence in
   `decoded[]` after decode-pass of bytecodes 77..82:
        #N+0: send nextPut:  (stencil_send)
        #N+1: pop            (stencil_pop)
        #N+2: extendB        (stencil_nop, bcLength=2)
        #N+3: extJump        (stencil_jumpBack, branchTarget=69)
   That looks right — Pop lands one entry *before* the nop for
   ExtendB.

### 2026-04-22 final — B5 event=3 TAIL trace landed

Done this session: added a second `_HOLE_RT_J2J_TRACE` call with
event=3 inside `J2J_INLINE_RETURN` that logs `_sv->resumeAddr`
and `_sv->sp`.  Updated `jit_rt_j2j_trace` in JITRuntime.cpp to
dispatch event=3 as a new `[B5] #N TAIL resumeAddr=... savedSp=...`
line.  Changed `stencil_returnTop_E` (the state=0-entry return
stencil used in the hot path) from `J2J_INLINE_RETURN_NO_TRACE`
to the traced variant — state=0 means x19-x22 are dead so the
B5-trace register spill is safe there.  Regenerated stencils
via `python3 scripts/extract_stencils.py` and rebuilt.

With the trace live, the immediate LIVE events preceding DNU #1
(the first atEnd-DNU in the reproducer) are:

    [B5] #1197 RET  sp=0x...288 depth=0 retVal=0x321 callerCM=0x30046cf68 savedArgs=1 sel=#reset
    [B5] #1198 TAIL resumeAddr=0x107f24354 savedSp=0x...288

**Correction to prior analysis**: earlier I said the caller
was `#contents`.  That was reading the ring-dump format (which
uses `callerCM=%llx` without `sel=`).  The live event=2 trace
with selector lookup shows `sel=#reset` — not `#contents`.
Method oop is 0x30046cf68, method's class has a `#reset`
selector, returns SmI 100 from a 1-arg send inside its body.

Plausible candidate: something like `PositionableStream>>reset`
or a fresh-scanner reset that includes an `^ counter + 1` or
similar 1-arg arithmetic.  The retVal 0x321 = SmI 100 is
consistent with an integer arithmetic result.

### Concrete next step (updated)

1. Identify which class's `#reset` method has JIT oop 0x30046cf68
   (note: oop varies per image download — in a fresh reproducer
   run, use the `sel=#reset callerCM=<oop>` from the *first*
   DNU-preceding RET).  Likely reading: `PositionableStream>>reset`,
   `OCScanner>>reset`, or some scanner-family reset.
2. Dump its bytecodes + literals like we did for
   `OCScanner>>contents` earlier.
3. Find the 1-arg send inside `#reset` whose callee returns
   SmI 100 in the reproducer.
4. Correlate `#1198 TAIL resumeAddr=0x107f24354` against the JIT
   method's compiled code range (`JITMethod::codeStart` .. `codeEnd`)
   from `jitRuntime_.methodMap()`.  That tells you exactly
   which bytecode offset inside `#reset` the tail-call jumps to
   — compare with where the post-send Pop should be.
5. If `resumeAddr` points *past* the Pop, that's the bug —
   one-off in the `RESUME_ADDR` hole computation.

### Current diagnostic toolkit

All landed this session, all env-gated:
  - `PHARO_SENDER_TRIPWIRE=1` — storePointer-level sender nil writes
  - `PHARO_SLOT_TRIPWIRE=1`  — slotAtPut-level, catches fast-path writes
  - `PHARO_B5_TRACE=1`       — SAVE / RET (with sel) / TAIL events for J2J
  - `PHARO_B5_MAX=N`         — raise trace event cap (default 800)
  - `PHARO_B5_FOCUS=0xHHH,...` — filter trace to specific caller oops
  - `PHARO_JIT_DEFER=0`      — eager JIT (reproducer needs this)
  - One-shot reproducer: `docs/bug14/reproduce.sh`

### The one thing missing (add this next session first)

A C++-side "classOfMethod" helper.  Given a method oop, walks
all globals looking for a class whose methodDict holds it, and
returns `ClassName>>selectorName` as a string.  Called from the
DNU reporter and/or the B5 RET trace to identify the caller
method by more than just its stored selector.

Rough shape in Interpreter.cpp:

    std::string Interpreter::classOfMethodOop(Oop methodOop) {
        // Walk smalltalkAssoc's values (global dictionary) looking
        // for Behavior instances whose methodDict has methodOop.
        // Cache in a std::unordered_map<uint64_t,std::string>.
    }

With this, the B5 RET event becomes

    [B5] #1197 RET retVal=0x321 savedArgs=1 sel=#reset
                                  ^ cls=<ClassName>

and you know *immediately* which class's `#reset` is being
J2J-returned-to.  Without this, you scan 103 candidates manually.
30 lines of C++ that would have saved the last hour of this
session.  Write it first next time.

### Bug 14 session commit trail

    8924f83  PHARO_SENDER_TRIPWIRE (storePointer level)
    f1f6798  NLR-overwalk hypothesis falsified (reverted, zero-effect fix)
    e51665f  PHARO_SLOT_TRIPWIRE (slotAtPut level, fast-path bypass falsified)
    6293c21  Reference logs + DNU #3 stack snapshot
    bef4d67  One-shot reproducer (docs/bug14/reproduce.sh)
    86c9bc0  stencils.cpp regen-required-after-edit gotcha documented
    25dc068  B5 RET augmented with selectorOf(callerCM)
    6c47c15  Bytecodes of OCScanner>>contents decoded (turned out to be
             mergesort's hot path, not the bug site)
    770779d  Static audit: ExtJump/Pop/ExtendB emit paths look correct in
             isolation; the bug is in their interaction
    0d62a1a  B5 TAIL event (event=3) logging resumeAddr + savedSp;
             corrected `sel=#contents` → `sel=#reset`

Net: from "jit hangs mysteriously, zero reproducer" to "3-step
checklist with full diagnostic instrumentation, 30s repro, pinned
to a one-arg send in some #reset method returning SmI 100 at
startup."  Handoff-ready.

### The 18-byte whileFalse: method is a red herring

The DNU says `in #whileFalse:` because the DNU reporter walks up
the *context* chain looking for a method name.  But the loop is
compiler-inlined inside `OCScanner>>contents`; there's no actual
`whileFalse:` activation.  The `method_` field being reported as
`BlockClosure>>whileFalse:` is either stale JIT state or the
reporter following `nlrHomeMethod_`.  Don't disassemble the
18-byte whileFalse: — it's dead code.  Focus on the 26-byte
`OCScanner>>contents` body above.

### 2026-04-22 handoff follow-up — method identified, resumeAddr CORRECT

New diagnostics landed this turn:

- `Interpreter::classNameOfMethod(Oop)` — thin wrapper over
  existing `methodClassOf` + `ObjectMemory::nameOfClass`.  No
  cache (B5 rate-limited; would require GC invalidation hook).
- B5 SAVE and RET events now include `cls=<ClassName> sel=#<sel>`
  directly in the log — eliminates the per-oop manual bisection.
- `PHARO_JIT_DUMP_SEL=<sel>` now also prints `codeStart`,
  `codeSize`, and the full `bcToCodeTable` so `TAIL resumeAddr`
  can be correlated exactly against bytecode entry points.

**Identity of the mystery `#reset` method: `WriteStream>>reset`**
(class binding via new classNameOfMethod, not OCScanner or
PositionableStream as prior guesses).  Source:

    reset
        "Refer to the comment in PositionableStream|reset."
        readLimit := readLimit max: position.
        position := 0

Bytecodes (initialPC=33..endPC=39, 7 bytes):

    33: 0x02  pushRcvrVar 2       (readLimit)
    34: 0x01  pushRcvrVar 1       (position)
    35: 0x90  Send1 lit[0]=#max:       ← the J2J send
    36: 0xCA  PopStoreRecvVar 2   (readLimit := ...)
    37: 0x50  pushZero
    38: 0xC9  PopStoreRecvVar 1   (position := 0)
    39: 0x58  returnReceiver      (^ self)

**Candidate (b) from the earlier analysis is FALSIFIED.**  With
bc[3]→code+2300, the B5 trace shows the tail-call after max: returns:

    [B5] #1197 RET  retVal=0x321 sel=#reset            (max: returning SmI 100)
    [B5] #1198 TAIL resumeAddr=0x10a810354 savedSp=0x717a8e288

and `bc[3] -> code+2300 = 0x10a810354` — an **exact match**.  The
J2J return resumes at byte 36 (PopStoreRecvVar 2), which is the
correct bytecode to consume the stack top.  So the resume math is
right, the Pop stencil is selected (not skipped), and the return
retVal lands at the right place.

### What's now missing — reset's *own* return path

Between `[B5] #1198 TAIL` and the `[DNU-STACK] atEnd` report,
there is **zero** B5 trace activity.  Reset's `bc[6]=returnReceiver`
uses `stencil_returnReceiver` (verified from JIT-DUMP), which
invokes `J2J_INLINE_RETURN` and *should* emit event=2 + event=3.
It doesn't fire.

Most plausible explanation: `j2jDepth` is already 0 when reset's
`returnReceiver` runs.  Every SAVE and RET in the trace shows
`depth=0`, suggesting each JIT activation starts its own J2J
stack (depth resets to 0 at entry).  Under that model:

    - Interpreter → JIT enters reset at depth=0
    - reset saves for max:       → depth 0 → 1
    - max: returns               → depth 1 → 0 (RET #1197 shows depth=0)
    - reset's returnReceiver     → depth=0 → J2J_INLINE_RETURN's
      `if (j2jDepth > 0)` guard FAILS → falls through to bail:
          s->exitReason = EXIT_RETURN;
      → **no event=2/event=3 trace**, returns control to C++

So reset's J2J path ends cleanly; control lands in the C++
interpreter, which is supposed to complete the return to reset's
caller (PositionableStream instance-side `on:`).  The bug is
somewhere in **that transition** — between JIT exit and the
interpreter's continuation of `on:`'s next bytecode.

### Caller chain preceding the DNU

    #1192 SAVE nArgs=0 cls=PositionableStream sel=#on: (instance)
    #1193 SAVE nArgs=1 cls=String class sel=#new:
    #1194 SAVE nArgs=1 cls=PositionableStream class sel=#on:
    #1195 SAVE nArgs=0 cls=PositionableStream sel=#on: (instance)
    #1196 SAVE nArgs=1 cls=WriteStream sel=#reset       ← reset→max:
    #1197 RET  retVal=0x321 cls=WriteStream sel=#reset  ← max:→reset
    #1198 TAIL resumeAddr=0x10a810354                   ← reset bc[3]

After reset returns cleanly (via C++ bail), control rejoins
`PositionableStream>>on:` mid-body.  That's where the stack
corruption must be happening — the bug is downstream of reset.

### Concrete next step (updated again)

1. Prove or disprove the depth=0 hypothesis: instrument
   `J2J_INLINE_RETURN` to log depth-pre-decrement.  Regenerate
   stencils.  If reset's returnReceiver bails (depth=0), we confirm
   the transition point.
2. At the moment reset's `stencil_returnReceiver` exits with
   `EXIT_RETURN`, log `sp`, `returnValue`, and the interpreter's
   next action.  Specifically: does the interpreter resume `on:`
   at the right bytecode offset, with the right receiver+sp?
3. Dump `PositionableStream>>on:` (instance-side) bytecodes.  The
   stack slot that eventually becomes the atEnd receiver lives
   inside that method's body.
4. If the bug is in the JIT→interpreter transition (the "exit
   path" when J2J depth hits 0), compare against the cleaner
   `PHARO_NO_JIT=1` run to see where the paths diverge.

### Handoff-ready diagnostics (all env-gated, all landed)

  - `PHARO_B5_TRACE=1` + `PHARO_B5_MAX=N` — J2J events with cls+sel
  - `PHARO_JIT_DUMP_SEL=reset[,other]` — compiled-method decode
    + **codeStart / bcToCodeTable** (new) per BC
  - `PHARO_SLOT_TRIPWIRE=1` — slot-0 nil writes on context shapes
  - `PHARO_SENDER_TRIPWIRE=1` — storePointer-level sender nils
  - `PHARO_JIT_DEFER=0` — eager JIT (required for reproducer)

Reference logs saved this turn:
- `/tmp/bug14-save.log` — JIT on with B5+DUMP_SEL=reset, full data
- `/tmp/bug14-dump.log` — earlier variant, pre-SAVE-enrichment

### 2026-04-22 (round 2) — reset's exit is CLEAN, bug is further downstream

Added C++-side instrumentation (`PHARO_B5_TRACE=1` also gates these
now):

- `[B5-EXIT]` at tryJITActivation's ExitReturn switch (line 14237)
- `[B5-EXIT-tryResume]` at tryResume's ExitReturn switch (line 12791)
- `[B5-EXIT-materialized]` at materialized-J2J ExitReturn (line 14088)
- `[B5-RESUME]` after popFrame+push in main switch — shows the
  interpreter state handed back to the caller.

Also captured `PositionableStream>>on:` bytecodes (initialPC=33..43):

    33 pushTemp 0       ← aCollection
    34 PopStoreRecvVar 0 (collection := aCollection)
    35 pushTemp 0
    36 specialSend #size
    37 PopStoreRecvVar 2 (readLimit := size)
    38 pushZero
    39 PopStoreRecvVar 1 (position := 0)
    40 pushReceiver
    41 Send0 lit[0]=#reset        ← THE CALL TO RESET
    42 Pop                        (discard reset's retVal)
    43 returnReceiver             (^ self)

### What the new events show

**Reset's normal exit** (pre-DNU, many times per run):

    [B5-EXIT]   #1 sp=0x1c0 ip=0x…f90 bcOff=0 j2jDepth=0 retVal=<obj>
                chainCallDepth=0 localFrameDepth=21 method=0x30046cf68
                cls=WriteStream sel=#reset
    [B5-RESUME] #1 stackPointer_=0x1c0 framePointer_=0x1a8 sp-fp=3
                method_=0x30046b3f8 rcvrCls=WriteStream sel=#on:
                ip=0x30046b429 bcOff=9 (next bc=0xd8=Pop)

**Reset's buggy exit** (exactly before DNU #1):

    [B5-EXIT]   #620 sp=0x278 ip=0x…f90 bcOff=0 j2jDepth=0 retVal=<obj>
                chainCallDepth=0 localFrameDepth=33 method=0x30046cf68
                cls=WriteStream sel=#reset
    [B5-RESUME] #563 stackPointer_=0x278 framePointer_=0x260 sp-fp=3
                method_=0x30046b3f8 rcvrCls=WriteStream sel=#on:
                ip=0x30046b429 bcOff=9 (next bc=0xd8=Pop)

**Both look structurally identical** — same sp-fp delta (3 slots),
same resume IP (bcOff=9 = byte 42 = Pop), same receiver class, same
method.  Only the absolute addresses and frameDepth differ (buggy is
12 frames deeper — consistent with the deep utf8Decoded chain).

### What's missing from the logs

Instance `PositionableStream>>on:` (method oop 0x30046b3f8) appears
*only* in B5-RESUME events — it has **zero** JIT-exit events in
any of the three switches I instrumented.  Plausibilities:

  a) on:'s execution continues via the chain loop inline (no switch
     reached), and on:'s return becomes embedded in the inner
     trampoline's J2J-return path at line 13932 of Interpreter.cpp
     (not yet instrumented).
  b) on: gets handed back to the interpreter's main bytecode loop
     after the send (so its Pop + returnReceiver run via interpreter,
     not JIT), and returnReceiver's interpreter handler is where the
     corruption slips in.
  c) on: never reaches `returnReceiver` — some intermediate operation
     throws/aborts first.

Class-side `PositionableStream class>>on:` (0x300469fd0) DOES exit
via `[B5-EXIT-tryResume]`, confirming the outer tryResume path is
active. Instance on: doesn't — it's in some other execution path.

### Falsified this round

- Reset's J2J return math is wrong — falsified. sp=0x278 matches
  on:'s save.sp exactly, confirming reset's stencil properly
  restored caller sp.
- popFrame restores on: to a bad state — falsified. Post-popFrame
  state is structurally identical between working and buggy cases.
- Pop stencil gets skipped by the resumeAddr — falsified. on:
  resumes at bcOff=9 (the Pop bytecode) in both cases.

### Concrete next step (updated again)

Instrument line 13932 — the tryJITActivation C++ trampoline's J2J
return branch.  That's the likely path where instance on:'s exit
becomes invisible to the other switches.  Log the method being
returned from (instance on:?) and what sp/ip it's resuming the
caller at.  If on: IS returning via this path, compare the working
vs buggy post-resume state there.  If on: is NOT returning via this
path, instrument `Interpreter::returnFromMethod` (or whichever
interpreter-side handler runs `returnReceiver` at byte 43) and look
for divergence.

### 2026-04-22 (round 3) — [B5-TRAMP-RET] dead end, ASM trampoline is opaque

Added a `[B5-TRAMP-RET]` log inside the C++ trampoline's
`j2jDepth > 0` return branch (Interpreter.cpp:13948).

When rerun with PHARO_ASM_TRAMPOLINE=ON (default): **zero
[B5-TRAMP-RET] events** — the ASM version handles the whole trampoline
in hand-written assembly, so the C++ log is unreachable.
Instrumenting the ASM trampoline requires editing
`src/vm/jit/TrampolineAsm.S` (not yet attempted).

When rerun with PHARO_ASM_TRAMPOLINE=OFF (C++ fallback): 8 events
fire but none for `PositionableStream>>on:` (instance) — reset IS
seen returning via this path (for its inner `max:` call), plus a
handful of other methods, but instance on: is conspicuously absent.

Crucially, with the C++ trampoline: **0 atEnd-DNUs fire** — but the
VM still hangs (idle process loop, ~110k steps in 60s).  Different
failure mode.  Either a second bug lives behind the ASM trampoline,
or both trampolines hit the same bug but manifest differently.

### What the B5 SAVE events actually show

Instance on: (oop 0x30046b3f8) DOES emit SAVE events as a caller —
at various depths (0, 1, … seen depth=1 at event #1419 of this
run).  So instance on: definitely runs JIT and dispatches sends
via the stencil J2J direct-call path.

At the exact bug site:

    #1195 SAVE depth=0 nArgs=0 cls=PositionableStream sel=#on:   ← on:'s save for reset
    #1196 SAVE depth=0 nArgs=1 cls=WriteStream sel=#reset        ← reset's save for max:
    #1197 RET   retVal=SmI 100                                    ← max: returning to reset
    #1198 TAIL  resumeAddr=reset bc[3] = PopStoreRecvVar 2

#1196's depth=0 (pre-inc) means reset enters at state.j2jDepth=0 —
NOT inherited from instance on:'s save.  That means either:
  (a) reset was NOT reached via stencil J2J direct-call from on:
      (some other code path reset state.j2jDepth between the save
       at #1195 and reset's entry), or
  (b) the SAVE at #1195 was NOT followed by the corresponding
      depth++ — i.e., the stencil bailed between the trace and the
      increment.  Stencil code doesn't appear to have such a bail,
      but worth double-checking.

### Post-popFrame [B5-RESUME] log

I added a log inside the tryJITActivation main-switch
`chainCallDepth==0` branch (after popFrame + push) that shows the
interpreter state handed back to the caller.  For both normal and
buggy reset exits:

    stackPointer_=<x>, framePointer_=<y>, sp-fp=3
    method_=instance-on:, ip=bcOff=9 (byte 42 = Pop)

Identical structure between cases.  So reset → on: transition is
clean.

### New most-likely bug location

Instance on: runs the JIT's stencil code, but its `self reset` send
results in *tryJITActivation(reset)* (not stencil J2J direct-call,
judging by the ExitReturn switch firing).  That means on:'s send
bailed out to interpreter or to a helper that then used
activateMethod + tryJITActivation.

After tryJITActivation(reset) returns true, interpreter dispatch
continues at on:'s byte 42 (Pop) as **interpreter bytecode**,
then byte 43 (returnReceiver) as interpreter.  That calls
`Interpreter::returnValue(receiver_)`, which pops on:'s frame and
then invokes `tryJITResumeInCaller()` at line 4809 to re-enter
JIT in the caller.

That re-enter path is the most likely bug site now.  Concrete
probe for next session:

  1. Log every entry to `tryJITResumeInCaller` when called from
     instance on:'s returnReceiver.  Capture sp, fp, method, ip,
     state to be restored into JIT.
  2. Compare working-case vs buggy-case values.  Look for a slot
     mismatch or stale ip.
  3. If tryJITResumeInCaller looks clean, walk forward — log the
     next few bytecodes executed by WriteStream>>on: (the caller)
     until a SmallInt appears where an object should.

### 2026-04-22 (round 4) — every chain link works; bug is in the body of `decodeBytes:to:`

Added `[B5-RESUME-CALLER]` at tryJITResumeInCaller entry.  New
view of the full chain preceding DNU #1:

    [B5-EXIT]   reset → on: sp=0x278  retVal=WriteStream fd=33
    [B5-RESUME] at instance on: bcOff=9 (byte 42 = Pop)
    [B5-RESUME-CALLER] WS>>on: bcOff=4 nextBC=Pop
                       top=0x30386d230(WriteStream)
    [B5-EXIT-tryResume] WS>>on:  retVal=WriteStream fd=31
    [B5-EXIT-tryResume] PositionableStream class>>on:
                       retVal=WriteStream fd=30
    [DNU #1 atEnd on SmI 6]

So: **every link in the class on: → WS>>on: → PS>>on: → reset →
max: chain works correctly.** retVal is WriteStream all the way up.
tryJITResumeInCaller lands at the right bytecode in every frame.

### Where the bug actually lives

Looking at the DNU call stack:

    [current] #whileFalse: fd=30
    [29] String class>>new:streamContents:
    [28] String class>>streamContents:
    [27] ZnUTF8Encoder>>decodeBytes:
    [26] ByteArray>>decodeWith:
    [25] ByteArray>>utf8Decoded

The `#whileFalse:` loop is inside ZnUTF8Encoder>>decodeBytes:to:
(the helper), roughly:

    decodeBytes: bytes to: stream
        | reader |
        reader := bytes readStream.
        [reader atEnd] whileFalse: [
            stream nextPut: (self nextCodePointFromStream: reader) ]

The failing send is `reader atEnd` where reader = SmI 6.

**The WriteStream we so carefully built is innocent** — that's
`stream` in this method.  The corrupted variable is `reader`, which
holds the result of `bytes readStream`.  `bytes readStream`
internally creates a ReadStream via `ReadStream on: bytes`, going
through the SAME class `on:` → instance `on:` → reset chain we've
been tracing.  So the `reader` assignment might be the one getting
corrupted — not the one we've been observing.

`lastJitReturn` at DNU:
    method=#on:(class on:) retVal=0x30386d230 (WriteStream) fd=29 (resume)

So the most recent JIT return was class on: returning a WriteStream
— but that WriteStream is presumably `stream`, not `reader`.  The
`reader` assignment happened earlier and may have produced a bad
value.

### Concrete next step (round 5)

1. The bug likely fires on the `reader := bytes readStream`
   initialization — a similar PositionableStream on: chain but for
   a ReadStream.  Grep the B5 events for an earlier on:-chain that
   produces a non-object retVal at fd ~26-30.
2. OR add a DNU-site probe: dump temps from the current frame
   (reader, stream, aBlock, etc.) when the atEnd-DNU fires, along
   with the BYTECODE offset in the current method.  That tells us
   WHICH temp slot holds the SmI 6 and whether it was always corrupted
   or corrupted mid-loop.
3. OR run with `PHARO_JIT_DEFER=5` or similar to delay JIT
   compilation past the problem path — if the bug disappears, it
   confirms JIT-exit corruption; if it remains, it's deeper.

Reference log: `/tmp/bug14-resumecaller.log`

### 2026-04-22 (round 5) — BUG LOCATED: class on: returns SmI 6 from JIT

Ran the reproducer with the full [B5-EXIT]/[B5-RESUME-CALLER] suite.
The critical transition right before DNU #1:

    [B5] #1192 SAVE sp=0x240 nArgs=0 callerCM=instance on:       ← instance on:'s save for reset
    [B5-EXIT] #619 ExitReturn sp=0x228 retVal=0x31 fd=29
              method=class on: (0x300469fd0)
              cls=PositionableStream class sel=#on:
    [B5-RESUME] #562 method_=ByteArray>>readStream bcOff=3
              (next bc=0x5c=returnTop)
    [B5-RESUME] returning into decodeBytes: top=0x31 (SmallInt)

**`PositionableStream class>>on:` exits with retVal=SmI 6** — that
is the bug.  Then:
  1. readStream's returnTop returns SmI 6 (from class on:).
  2. decodeBytes: stores SmI 6 into `reader := bytes readStream`.
  3. Existing B5-BUG instrumentation at Interpreter.cpp:8892 dumps
     the block's copied values: `temp[1] = 0x31 SmI`.
  4. `[reader atEnd]` whileFalse: sends atEnd to SmI 6 → DNU.

### Class on: bytecodes and resume points

    methodOop=0x300469fd0 numLits=4 source=^ self basicNew on: aCollection
    41: 0x4C  pushReceiver (class)
    42: 0x80  Send0 lit[0]=#basicNew  → new ReadStream instance on TOS
    43: 0x40  pushTemp 0 (aCollection)
    44: 0x91  Send1 lit[1]=#on:       → initialized instance on TOS
    45: 0x5C  returnTop               → returns TOS

    JIT code layout (codeStart=0x…9d8, codeSize=5136):
    bc[0] pushReceiver     → code+0
    bc[1] sendJ2J #basicNew → code+16
    bc[2] pushTemp          → code+2156
    bc[3] sendJ2J #on:      → code+2188
    bc[4] returnTop         → code+4328

state.ip at [B5-EXIT] is bcOff=0 because the C++/ASM trampoline
resets state.ip = savedJM->bcStart() when popping — so that field
isn't diagnostic at exit time.  Instead look at retVal.

### The open question

Between the stencil SAVE for reset (#1192) and class on:'s ExitReturn
(#619), there are **zero J2J trace events** — no TAIL, no RET for
reset, no other save/return.  That means reset's return path (the
stencil's J2J_INLINE_RETURN) either didn't fire the trace, or the
ASM trampoline is doing the whole pop+resume loop opaquely.

Three hypotheses for how class on:'s retVal ended up as SmI 6:

  (a) Instance on:'s `returnReceiver` stencil returns `s->receiver`,
      but `s->receiver` got overwritten to SmI 6 somewhere
      (perhaps during reset's max: J2J restoration).
  (b) Class on:'s returnTop stencil reads from the wrong stack slot
      — its expected TOS was the initialized instance (written by
      instance on:'s J2J restoration at `_sv->sp[-2] = retVal`),
      but a miscomputed offset reads SmI 6 from an adjacent slot.
  (c) An NLR or exception unwound through the chain and picked up
      a transient SmI 6 (e.g., the value of `readLimit` set during
      instance on:) as the return value.

### Concrete next step (round 6)

1. Add a log INSIDE `J2J_INLINE_RETURN`'s bail path (stencils.cpp)
   that prints `s->receiver`, `retVal`, `_sv->receiver`, `_sv->sp`,
   `_sv->sendArgCount` right before setting exitReason=EXIT_RETURN.
   Since this requires regenerating stencils
   (`python3 scripts/extract_stencils.py`), start here.
2. If hypothesis (a) is right, the bail will show s->receiver=SmI 6.
   If (b), the restore is fine but returnTop picks a bad slot —
   add a log at the returnTop stencil.  If (c), we'll see an NLR
   event.
3. ALSO: run the same repro with `JIT_EXCLUDE_OOP=0x300469fd0`
   (exclude class on: from JIT) — if bug disappears, it IS in
   class on:'s JIT code.  Then try excluding instance on:
   (0x30046b3f8) and reset (0x30046cf68) individually.

### 2026-04-22 bisection result

    JIT_EXCLUDE_OOP=<class on:>    → 0 DNUs (test progresses)  ✓
    JIT_EXCLUDE_OOP=<instance on:> → 0 DNUs (test progresses)  ✓
    JIT_EXCLUDE_OOP=<reset>        → 3 DNUs (bug persists)      ✗

The bug requires BOTH class on: AND instance on: to be JIT-compiled.
Excluding either from JIT prevents the corruption.  Reset is NOT
involved — it's collateral noise in the trace.

**So the bug is in the J2J transition from class on: → instance on:**
(stencil J2J direct-call path, or the paired J2J_INLINE_RETURN on
the way back).  Most likely site: the send stencil at class on:'s
bc[3] (Send1 #on:) or instance on:'s returnReceiver
J2J_INLINE_RETURN.

Neither reset's max: J2J nor ByteArray>>readStream is necessary —
this is a direct class-on: ↔ instance-on: pair.

### Concrete next step (round 6, updated)

Since only these two methods + their J2J dance matters, a minimal
stress test is feasible.  In a throwaway Smalltalk eval, call
`PositionableStream class>>on:` many times with freshly-JIT'd
code and watch for the first time retVal isn't an instance.  If
that repros, isolate to a ~50-line reproducer that doesn't need
the whole sunit harness.

Then:
1. Instrument J2J_INLINE_RETURN's bail path (regen stencils) to
   log retVal + s->receiver + _sv fields — with only these two
   methods in play, events will be far fewer and easier to diff
   against a working call.
2. Compare instance on:'s returnReceiver in the bug case vs a
   working case.  If retVal is SmI 6 in the bug, trace upward to
   find where s->receiver was corrupted.

### 2026-04-22 (round 6) — `state.receiver` correct at SAVE; corrupted later

Extended `[B5-EXIT]` to log `state.receiver` and `[B5] SAVE` to log
the caller's `state.receiver` at save time.  The bug window view:

    [JIT] Compiled method #108 0x30046b3f8 'on:'     ← instance on:
                                                      just compiled
    [B5] #1192 SAVE sp=0x240 depth=0 nArgs=0
                rcvr=0x30386c3b8(ReadStream)     ← RECEIVER IS VALID
                callerCM=instance on:
                cls=PositionableStream sel=#on:
    [B5-EXIT] #619 ExitReturn sp=0x228 j2jDepth=0
                retVal=0x31 (SmI 6)              ← BUG — SmI 6
                receiver=0x300024ee8(ReadStream class) ← class on:'s
                                                       (correct)
                method=class on: (0x300469fd0)

So at instance on:'s SAVE, `state.receiver` = a valid ReadStream
instance.  By the time class on:'s activation exits, `retVal` is
SmI 6.  The chain of reasoning:

  - class on:'s returnTop reads TOS → returned value.
  - Trampoline wrote `retVal` (= state.returnValue) to class on:'s
    sp[-2] slot when popping.
  - state.returnValue was set by **instance on:'s returnReceiver
    bail path**: `s->returnValue = s->receiver`.
  - Therefore instance on:'s `s->receiver` was SmI 6 at its
    returnReceiver bytecode, even though it was a valid ReadStream
    at the save slightly earlier.

### What instance on: does between the save and returnReceiver

    36: specialSend #size   (1-arg: aCollection size)
    37: PopStoreRecvVar 2   (readLimit := size)
    38: pushZero
    39: PopStoreRecvVar 1   (position := 0)
    40: pushReceiver
    41: Send0 #reset
    42: Pop
    43: returnReceiver      ← s->returnValue = s->receiver here

The SAVE at #1192 (nArgs=0) could be for byte 36's size send OR
byte 41's reset send (both 0-arg).  No companion trace event tells
us which.  Either way, `state.receiver` should remain pointing at
the ReadStream instance throughout — `PopStoreRcvVar`, `pushReceiver`,
`Pop`, and return stencils don't rewrite `state.receiver`.

Only the stencil J2J direct-call sets/restores `state.receiver`
(save at call-site → restore on return via _sv->receiver).  If that
restoration picks up a bad value, `state.receiver` becomes SmI 6.

### Why the ASM trampoline hides this

PHARO_ASM_TRAMPOLINE=ON uses `pharo_jit_j2j_trampoline` — a
hand-written ARM64 trampoline that handles ExitSendCached →
ExitJ2JCall conversion, the J2J call, AND the J2J-return in
assembly.  My new `[B5-TRAMP-CALL]` log (C++ trampoline's J2J-call
setup) fires ZERO times — the ASM path bypasses it entirely.

The most likely suspect: the ASM trampoline's J2J-call setup
sequence stores/restores state.receiver in a way that, under
specific conditions (fresh compile of instance on: while class on:
is active), leaves it corrupted.

### Concrete next step (round 7)

1. **Instrument the ASM trampoline**: add a single stub call from
   `src/vm/jit/TrampolineAsm.S` into a C helper that logs
   state.receiver + state.sp[-(nArgs+1)] at each J2J-call entry
   AND each J2J-return restore.  This is the only path not yet
   visible.
2. Alternatively, disable **only** the ASM trampoline for instance
   on: / class on: and let the C++ trampoline run those (via
   per-method gate, if one exists).  Compare behavior.
3. Or: instrument stencils.cpp's `J2J_INLINE_RETURN_IMPL` bail path
   to dump `s->receiver`, `retVal`, `_sv->receiver`, `_sv->sp`,
   `_sv->sendArgCount` — regenerate via
   `python3 scripts/extract_stencils.py` and rebuild.  Regen is
   documented and straightforward; it just wasn't done yet.

Reference log: `/tmp/bug14-saverecv.log` (SAVE now shows rcvr).

### 2026-04-22 (round 7) — stencil-regen blocker discovered

Attempted to add a diagnostic event=4 trace to `stencil_returnReceiver`
(logging when `s->receiver` is SmallInt).  `python3
scripts/extract_stencils.py` failed BOTH with and without my edit:

    [3/4] Verifying SimStack register safety...
    ERROR: SimStack stencils have compiler-generated x19-x22 save/restore!
      stencil_returnTop_E @ offset 0: STP/LDP with x22/x21
      stencil_returnTop_E @ offset 4: STP/LDP with x20/x19
      (etc.)

**This is pre-existing in the current tree** — the 0d62a1a commit
switched `stencil_returnTop_E` to the traced `J2J_INLINE_RETURN`
variant, which now exceeds the compiler's caller-saved register
budget.  The generated stencils files haven't been regenerated
since then (timestamped 05:22, stencils.cpp 05:23).  They still
work because the code was compiled BEFORE hitting the added
verification threshold on some earlier compiler pass.

Consequence: the current tree **cannot regenerate stencils**
until `stencil_returnTop_E` is simplified (e.g., move TRACE outside
the hot path, or revert to NO_TRACE with an alternate diagnostic
mechanism).  That's a cleanup task separate from the bug itself.

### C++-trampoline retest (round 7)

Rebuilt with `-DPHARO_ASM_TRAMPOLINE=OFF` and reran reproducer.
Result: 0 atEnd DNUs, but VM hangs in idle after only ~88k
bytecode steps (ASM tramp: ~100k before DNU but then many more
during recovery, eventually hitting the atEnd site multiple
times).

So **both trampolines break the repro, differently**:
  - ASM: produces atEnd DNU cascade (what we've been chasing).
  - C++: hangs much earlier without DNU.

Either (a) there are two distinct bugs, or (b) one bug manifests
differently because C++ tramp path bails earlier (say, at an
ExitSendCached that ASM converts).  Both block bug-14 progress.

Restored `PHARO_ASM_TRAMPOLINE=ON` (the default) for subsequent
debugging — that's the failure mode we understand and have
instrumented.

### Concrete next step (round 8)

1. **Fix `stencil_returnTop_E`** so `extract_stencils.py` regen
   succeeds.  The simplest fix: make its `J2J_INLINE_RETURN` call
   through a noinline `__attribute__((naked))` helper so the
   compiler doesn't spill x19-x22 in the main function body.  Or
   manually split it into two functions: one for the if-branch
   and one for the fall-through.  Once regen works, all stencil
   changes become tractable.
2. **Then** add the bug-14 diagnostic to `J2J_INLINE_RETURN`'s bail
   path (log s->receiver, _sv->receiver, retVal, depth).
3. **Alternative**: directly modify the ASM trampoline
   (`src/vm/jit/TrampolineAsm.S`) to add a single `bl` instruction
   calling a C helper at the J2J-return restore point
   (after `str x9, [x21, #JS_RECEIVER]` at line 178).  Need to
   save/restore x0-x18 before/after; careful ABI work.

### Diagnostics this round (env-gated, all landed)

  - `PHARO_B5_TRACE=1` now also triggers:
    - `[B5-EXIT]` at main tryJITActivation ExitReturn (14237)
    - `[B5-EXIT-tryResume]` at tryResume ExitReturn (12791)
    - `[B5-EXIT-materialized]` at materialized J2J ExitReturn (14088)
    - `[B5-RESUME]` after popFrame + push retVal
  - `PHARO_JIT_DUMP_SEL=<sel>` — prints codeStart + bcToCodeTable

Reference log: `/tmp/bug14-exit4.log` — reproducer with all three
[B5-EXIT] variants.

## Related commits

- `8924f83` — `PHARO_SENDER_TRIPWIRE` (storePointer-level)
- `e51665f` — `PHARO_SLOT_TRIPWIRE` (slotAtPut-level)
- `docs/jit-uncovered-bugs.md` bug-14 section — full narrative
- memory: `project_bug14_root_cause.md`
