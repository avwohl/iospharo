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

### The actual next step (next session)

The audit didn't find the bug by static reading.  To actually
catch it, add a printf to `J2J_INLINE_RETURN_IMPL` in
`src/vm/jit/stencils/stencils.cpp` logging:
  - retVal
  - _sv->sp
  - _sv->sendArgCount
  - _sv->resumeAddr (the tail-call target)

**Critical**: must then regenerate
`src/vm/jit/generated_stencils_arm64.hpp` via
`python3 scripts/extract_stencils.py` before rebuilding, otherwise
the compiled VM runs stale stencils and the printf never fires.
(Verified during this session: fprintf in the non-stencil C++
return path at `Interpreter.cpp:12685` logs zero events — all
J2J returns go through the stencil path, not the C++ trampoline.)

Once the printf fires in the reproducer, correlate the
resumeAddr values against the JIT method's compiled code addresses
(available from `JITMethod` metadata) to confirm it points at
the Pop stencil entry.  If it doesn't — there's the bug.

### The 18-byte whileFalse: method is a red herring

The DNU says `in #whileFalse:` because the DNU reporter walks up
the *context* chain looking for a method name.  But the loop is
compiler-inlined inside `OCScanner>>contents`; there's no actual
`whileFalse:` activation.  The `method_` field being reported as
`BlockClosure>>whileFalse:` is either stale JIT state or the
reporter following `nlrHomeMethod_`.  Don't disassemble the
18-byte whileFalse: — it's dead code.  Focus on the 26-byte
`OCScanner>>contents` body above.

## Related commits

- `8924f83` — `PHARO_SENDER_TRIPWIRE` (storePointer-level)
- `e51665f` — `PHARO_SLOT_TRIPWIRE` (slotAtPut-level)
- `docs/jit-uncovered-bugs.md` bug-14 section — full narrative
- memory: `project_bug14_root_cause.md`
