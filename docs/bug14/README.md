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

1. Disassemble JIT-compiled method 0x3003f8158 (BlockClosure>>
   whileFalse:) — the method ID is reproducible across runs of
   the same image.  Compare bytecode layout with what the
   JIT emitted.
2. Instrument the return stencils (`stencil_returnTop`,
   `stencil_returnReceiver`, J2J_INLINE_RETURN macro) to log
   the stack-slot write offset.  The bug is likely in
   J2J_INLINE_RETURN line 506:
   `_sv->sp[-(_nArgs + 1)] = retVal;` — if `_nArgs` is captured
   with the wrong value at J2JSave time, the retVal lands in
   a wrong slot.
3. Or check the `whileFalse:` primitive (272) path — if the JIT
   specializes whileFalse: via a primitive stencil, check that
   stencil's stack arithmetic.

## Related commits

- `8924f83` — `PHARO_SENDER_TRIPWIRE` (storePointer-level)
- `e51665f` — `PHARO_SLOT_TRIPWIRE` (slotAtPut-level)
- `docs/jit-uncovered-bugs.md` bug-14 section — full narrative
- memory: `project_bug14_root_cause.md`
