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

## Related commits

- `8924f83` — `PHARO_SENDER_TRIPWIRE` (storePointer-level)
- `e51665f` — `PHARO_SLOT_TRIPWIRE` (slotAtPut-level)
- `docs/jit-uncovered-bugs.md` bug-14 section — full narrative
- memory: `project_bug14_root_cause.md`
