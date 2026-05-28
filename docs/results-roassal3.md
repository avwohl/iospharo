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

## Investigation log — 2026-05-28 PM

### Crash is deterministic

Reran the full 99-class list a second time (commit `41eeed6a` baseline).
Identical crash: same method (`#inverseTransformPiOrZero:`), same
codeSize (8464), same offset (2496), same fault addr
(`0x86fe800000000008`), same call site reached at test 35.  So we
have a reliable reproducer.

### Single-class runs do NOT crash

Running just `RSAdjacencyMatrixBuilderTest` alone completes with
13 PASS / 0 FAIL / 3 ERROR (the 3 `Color>>blue` errors) and no SIGSEGV.
Running `RSBarPlotTest` alone also completes (16 ERRORs from
Color/transform issues but no crash).  Even a 9-class subset
(`RSAdjacencyMatrix` through `RSBoxPlot`) completes without crashing.

So the crash needs the full 99-class context — JIT cache pressure
seems to be a precondition.  Plausible mechanisms:

* Method-map / IC eviction patches a J2J entry into an IC that
  later gets read without the corresponding J2J epilog.
* GC moves a JIT-marked method while an IC still holds the old
  bit-tagged address.

### `#inverseTransformPiOrZero:` is not the culprit on its own

The method body is six lines, four float sends.  An isolated probe
that calls it 100 K times with both identical and varying Points
returns correct results and never crashes — so the JIT'd code for
this method is fine in isolation.

The bug only shows up when this method is dispatched THROUGH an IC
whose extras word still carries J2J-classifier bits from a prior
caller.  The crashing site loads `[x19]` where x19 = the IC slot's
cached value with bits 60+57+56+63 still set.

### Bisection so far

Tried `PHARO_T1_NO_INLINE_PRIM_BASIC_NEW=1` — different crash, earlier
test (test 17 instead of 35), same bit-60+ pattern in fault addr
(`0x8340000000000000`).  So the bug is not isolated to one inline
path; the basicNew inline was masking a different latent J2J leak.

### `PHARO_NO_JIT=1` removes BOTH the crash AND the errors

At 35 tests in, the JIT run produces 32 PASS / 3 ERROR (`Color>>blue`
KeyNotFound on testRendering01, testSorting01, testSorting02) and
then crashes.  The same 35 tests under `PHARO_NO_JIT=1` produce
35 PASS / 0 ERROR — the same `Color>>blue` calls succeed.

So both task #10 (SIGSEGV) and task #11 (Color KeyNotFound) are
JIT bugs.  Task #11 is no longer a separate "identity dictionary
corruption" hypothesis — it's the same J2J-bit leak as task #10
manifesting differently (the dispatched Color>>blue method ends
up reading from a tagged extras word instead of the clean
`ColorRegistry` slot).

### Next steps requiring lldb

1. Set a breakpoint at the crashing PC (`codeStart + 2496` in
   `#inverseTransformPiOrZero:`) and inspect what populated x19.
2. Walk back from there to find the IC fill site that left bit 60
   on the cached value.
3. Either mask bits 56-63 in dispatchCached's `[x5, 8]` load, OR
   fix the IC fill so bit 60 only goes into the `extras` word at
   `[x5, 16]`, never into `icData[1]` at `[x5, 8]`.

## Run 2 — 2026-05-28 16:38 → 16:44 (defensive guards landed)

Commit `b8eead95`.  Defensive guards added at two sites:

* `AsmjitT1.cpp` emit_send (arm64): after the low-3-bit tag check,
  inspect bits 48-63 of the receiver Oop.  Non-canonical → MISS
  (route to chain loop).  Two extra arm64 instructions per send.
* `Interpreter.cpp` `sendDoesNotUnderstand`: when receiver has top
  bits set, log `[DNU-LEAK]` and push nil instead of dereferencing.

Result on the same 99-class Roassal3 list:

       counter   run 1   run 2
       PASS      32      51    +19
       FAIL      0       0
       ERROR     3       121   +118
       TIMEOUT   0       0
       reached   35      172   4.9× more
       crashed   YES     no    !!
       DNU-LEAK  n/a     3     all `0x86fe800000000008`

The crash is **gone**.  ERROR count exploded because cascading nil
returns from the 3 leaked sends fail many downstream assertions in
plot/chart tests.  The run eventually stalled at RSClusterChartTest
(suite-specific issue, possibly an infinite loop on the nil result;
the per-test watchdog should catch it but we killed early).

The leak itself is still upstream — these guards prevent the crash
but don't stop the bit-60 escape.  Next step: find the IC fill /
return-value path that emits the polluted Oop.

### Bisection attempt — bit 63, not bit 60

A closer look at the fault address `0x86fe800000000008` shows
**bit 63 set, bit 60 CLEAR** — so this is the inline-getter / quick-prim
classifier bit (bit 63), not J2J_ENTRY_BIT (bit 60).  The Oop is
shaped like an IC `extras` word: `(1ULL << 63) | classifier_bits |
low_address_bits` — typical of `quickPrimExtra` at
`Interpreter.cpp:19858` or trivial-method getter classification at
:19438.

Tried two angles:

1. **`PHARO_T1_NO_INLINE_GETTER=1`** — no `[DNU-LEAK]` events fire,
   but the run also doesn't get past test 35 (different reason: many
   methods that were inlined now take the chain-loop path and the
   suite gets slow enough that no test completes inside a reasonable
   wall budget).  Suggestive that the inline-getter path participates
   in the leak, but not conclusive.

2. **Canonicalize the inline-getter result** — `a.and_` the loaded
   slot value with `0x0000FFFFFFFFFFFF` after checking the tag is 0
   (object).  Build clean, test runs, but `[DNU-LEAK]` still fires —
   so whatever is producing the corrupt receiver is NOT the inline-
   getter result store.  Reverted the mask.

The leak is somewhere else.  Plausible remaining suspects:

* Inline-setter, inline-returnsSelf, or inline-prim-at result paths
  that write extras-shaped values back to the operand stack.
* `dispatchCached` storing the IC's `icData[1]` (cached method
  Oop) into `OFF_CACHED_TARGET` without masking — if the IC fill
  ever wrote a getter-extras-shaped value to the method slot, this
  would propagate.
* A return-value path (returnTop / returnReceiver / etc.) that
  reads from a polluted stack slot.

Needs another lldb pass with a hardware watchpoint on the stack
slot that ends up corrupt, to catch the actual store that pollutes
it.
