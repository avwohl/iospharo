# Roassal3 SUnit results

Run on `/tmp/harness/Pharo-gfx.image` (clean Pharo 13 + SUnitRunner +
FakeGUI shims).  Class filter: 99 RS-prefixed `TestCase` subclasses
from `/tmp/roassal_test_classes.txt`.

## Run 1 — 2026-05-28 14:48 → 14:50

Raw files: `docs/results/roassal3_run1.txt`,
`docs/results/roassal3_run1_detail.txt`.

       counter   value
       PASS      32
       FAIL      0
       ERROR     3
       TIMEOUT   0
       crashed   yes (SIGSEGV inside JIT-compiled #inverseTransformPiOrZero:)
       coverage  35 of ~879 tests (4%)

The VM crashed in JIT-emitted code partway through
`RSAdjacencyMatrixBuilderTest`, so the run never reached the rest of
the 99 classes.

### The three errors

All three errors are the same root cause — `Color class >> blue`
returning `KeyNotFound: key #blue not found in IdentityDictionary`:

       RSAdjacencyMatrixBuilderTest>>testRendering01
       RSAdjacencyMatrixBuilderTest>>testSorting01
       RSAdjacencyMatrixBuilderTest>>testSorting02

Stack:

       Color class>>blue
       RSAdjacencyMatrixBuilder>>connectingColor
       … >>renderIn: …

These are NOT a Roassal3 bug — `Color blue` is a stock-Pharo API.
The fact that other RSAdjacencyMatrixBuilderTest selectors PASS
(testBasic01-03, testConnections01, testCycle01-04,
testIncomingConnectionsOf, testOutgoingConnectionsOf) means
`Color blue` works in some call sites but not others.  Likely
candidates:

* `Color colorNames` `IdentityDictionary` was lazily initialized by
  one of the passing tests and the init pushed the wrong key tag
  (`'blue'` ByteString vs `#blue` Symbol).
* JIT'd `Color class >> blue` accessor differs from interpreted.

### The SIGSEGV

       JIT method: sel=#inverseTransformPiOrZero: oop=0x301326f98
                   codeStart=0x10af31130 codeSize=8464 numBC=0 numIC=5
                   offsetInCode=2496
       Fault addr=0x86fe800000000008
       PC at: ldr w4, [x1]   (x1 = fault addr)

The fault address has bit 60 set (`0x8000_0000_0000_0000`'s neighbour
`0x0800_…` — actually 0x86 has bit 60 set).  Bit 60 is
`J2J_ENTRY_BIT`.  Reading the fault address as if it were an Oop —
without first masking off the entry-bit — is a classic J2J leak.
Either:

* The JIT prologue for `#inverseTransformPiOrZero:` failed to mask
  bit 60 off a receiver/arg coming back from a J2J return.
* A saved-sender slot was corrupted to a method-with-J2J-bit value
  and read as an Oop later.

Documented in `docs/deferred.md` (or should be — TBD).

## Next runs

* **Skip the test classes that crashed**: filter the 99 down to a
  list excluding `RSAdjacencyMatrixBuilderTest`, re-run, see how
  much further we get before the next crash.
* **Bisect with `PHARO_NO_JIT=1`**: confirm the SIGSEGV goes away
  when JIT is off (will be ~50× slower; only worth running the
  first few classes to confirm).
* **Capture the JIT'd `#inverseTransformPiOrZero:`** via the
  asmjit disassembler dump (`PHARO_JIT_DUMP=…`) to spot the missing
  mask.
