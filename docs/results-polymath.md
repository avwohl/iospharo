# PolyMath SUnit results

PolyMath is NOT preinstalled in Pharo 13 — required a Metacello load
from `github://PolyMathOrg/PolyMath/src`.  The Iceberg SHA1 path
needed the `Character>>bitShift:` / `adaptToNumber:andSend:` family
of patches (see `docs/image_issues.md` / `scripts/patches/`) before
the load could succeed.  Once patched, 309 PM classes load and 101
test classes are exposed.

## Run 1 — 2026-05-28 16:18 → 16:21

Raw files: `docs/results/polymath_run1.txt`,
`docs/results/polymath_run1_detail.txt`.

       counter   value
       PASS      11
       FAIL      0
       ERROR     0
       TIMEOUT   0
       crashed   yes (SIGSEGV in primitiveFloatAdd+432)
       coverage  11 of ~3500 tests, 3 of 101 classes

The crash hit on the transition into the 4th test class (after
PMAB2SolverTest, PMAB2StepperTest, PMAB3SolverTest were clean).

## The SIGSEGV

       Fault addr=0x7d9124f4bc610c80
       PC in C symbol: pharo::Interpreter::primitiveFloatAdd + 432
       Stack:
         primitiveFloatAdd + 432
         executePrimitive + 572
         sendSelector + 12296
         interpret + 8072

`0x7d9124f4bc610c80` is non-canonical (top bits look like a random
value, not 0x0000... or 0xffff...).  Whatever was being dereferenced
inside `primitiveFloatAdd` had a corrupt pointer in it — either the
receiver Oop is bogus, or it's a boxed Float whose header points off
into nonsense.

This crash is **NOT** the same J2J entry-bit leak as Roassal3 — it's
deep in C++ primitive code, not JIT-emitted code.  Different bug.

## Next runs

* `PHARO_NO_JIT=1` baseline on PolyMath to see how far interpreted
  gets (Roassal3 NO_JIT also crashed-or-hung at test 35 — different
  trigger).
* Strip down to a single failing class — likely `PMAB3StepperTest`
  alphabetically (the one after PMAB3SolverTest) — and find the
  Float send that crashes.
* Tracks as task #14.

## Investigation log

### Tried: extractFloat canonical-bit guard — made things worse

Added a check in `extractFloat` (`Primitives.cpp:5817`) to fail the
primitive when `(oop.rawBits() >> 48) != 0` (catching the
`J2J_ENTRY_BIT` / extras-bits leak).  The float-add SIGSEGV
disappeared as expected — but the run regressed from 11 PASS to
3 PASS, with the failure shifting to `sendDoesNotUnderstand+3136`.

The original primitiveFloatAdd crash was acting as a "fail-fast"
that stopped a corrupt Oop from propagating further; making the
primitive Failure-out let that Oop flow into Float>>+ Smalltalk
fallback, which DNU'd, and the DNU dispatch crashed on the same
corrupt receiver.  Reverted.

### Run is non-deterministic

Three separate runs on the same image produced:

       run        last-good test        crash site
       1          test 11               primitiveFloatAdd+432
       2 (guard)  test 3                sendDoesNotUnderstand+3136
       3 (no guard, 4 classes only) test 3                sendDoesNotUnderstand+3136

Different timing / JIT-warm state changes which call site sees the
corrupt Oop first.  The underlying bug — the same J2J entry-bit
family as task #10 — is the same in all runs.

### Conclusion

#14 is not an independent bug — it's the same J2J entry-bit leak
manifesting in C++ primitive deref instead of JIT-emitted code.
Fix #10 and #14 should go away.  Closing #14 as a duplicate.
