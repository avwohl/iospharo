# Pharo SUnit Suite — VM Compatibility Status

## 2026-06-04 full-suite run: custom VM vs stock Cog

**Setup:** full discovery run (every non-abstract `TestCase` subclass — the
2026-04 no-skip policy), Pharo 13 image. Stock Cog (`~/stockpharo/pharo`,
`run_sunit_cog.st`) on the x64 box vs the clean C++ VM (`test_load_image`,
`run_sunit_tests.st`). Custom-VM arm64 = local build; x86 = AWS box.

**Stock Cog — COMPLETES the full suite (baseline):**

```
  classes  2045
  pass     27815      fail 38   error 83   skip 128
  total    28064      pass rate 99.1%
```

**Custom VM — CANNOT complete the full suite.** Where each config stalls:

```
  arm64, Sista ON   ~80 / 2045 classes, then hangs in CollectionArithmeticTest
  arm64, Sista OFF  stalls at IntegerTest (class 7)
  x86,   Sista ON   DNU at startup, 0 tests: SUnitRunner>>nextRunNumber does
                    [file contents asInteger] but asInteger is sent to the
                    SUnitRunner class (self) — an inlined-block wrong-receiver
                    miscompile (x86 analog of the arm64 fix 4b446bf4)
```

**The CollectionArithmeticTest hang is NOT a JIT/Sista bug — corrected
2026-06-04.** Originally mislabelled a "Sista miscompile of testRunningAverage".
Investigation (eval-mode repro + runCase) disproved that:
- Every test in the class — testRunningAverage, …WithSubsetSize1…, testStdev,
  testRunningMax/Min — PASSES when run directly via `(Class selector: sel)
  runCase`, under Sista, in a warm VM. The test *logic* is correct.
- The full-suite run hangs in the pure INTERPRETER too (`PHARO_NO_JIT=1`), at a
  *different* test (testStdev-ish) — so it is timing-dependent and not JIT.
- No WhileTrueAccum fold fires on this path (CAND counter = 0).
- At the hang only the idle process (P10) is runnable → a **scheduler
  DEADLOCK**, not a runaway loop.
The hang is in the SUnit **runner's** per-test fork + Delay-based watchdog
machinery (`run_sunit_tests.st`) deadlocking against our VM's process
scheduler / Delay timer — not in the tests or the JIT. The earlier 4.7e9-step
"spin" was the watchdog/heartbeat cycling, not the test computing. Real fix
target: the VM scheduler / Delay-timer interaction under the runner's fork +
watchdog load (a base-VM scheduler issue), or a runner that avoids the
deadlock-prone fork/watchdog for tests that don't actually hang.

**Δcog on the 80 classes the custom VM (Sista) DID complete** (4405 tests):

```
  pass 4306   fail 12   error 80   skip 6   timeout 1     (97.8% of the 4405)
  93 tests pass on Cog but not here: 80 error, 12 fail, 1 timeout
```

Regression clusters (delta file `*.delta-vs-cog.txt`): collection
`testReject` / `testCopyWithout*` (Bag/Dictionary/Heap/DoubleLinkedList/…
errors — likely one shared `reject:`/`copyWithout:` root cause),
`BecomeTest>>testBecome*IdentityHash` (become+identityHash, fails),
class-introspection (`BehaviorTest`/`ClassHierarchyTest` testAllReferencesTo /
testSubclasses errors), `IntegerTest>>testReciprocalModulo` (timeout).

**Bottom line:** Cog runs the whole suite at 99.1%; the custom VM's full-suite
coverage is gated by non-preemptible runaway tests (Sista miscompiles), not by
the asmjit RA crash fixed earlier (bb158a7f — bench-validated, orthogonal). The
next lever for custom-VM SUnit coverage is (a) a preemptible-loop interrupt
check so the watchdog can kill runaway tests, and (b) fixing the Sista
miscompiles that turn finite loops infinite (testRunningAverage) and the x86
inlined-block wrong-receiver bug (nextRunNumber).

## 2026-05-28 first end-to-end run

**Setup:** `scripts/pharo-headless-test/` harness, fresh Pharo 13
image (`/tmp/harness/Pharo.image`) with `setup_fake_gui.st` +
`run_sunit_tests.st` injected via Cog `eval --save` into
`/tmp/harness/Pharo-sunit.image`.

**Result:** Run completes structurally but reports 0 pass / 0 fail /
0 error / 0 skip / 0 timeout for every test class.  2046 test
classes discovered (dynamic fallback); zero of them ran any tests.

The harness is exposing three latent VM bugs that block meaningful
SUnit execution:

### 1. `FileReference >> #lines` DNU

```
outer err: MessageNotUnderstood
msg: Message not understood: FileReference >> #lines
```

The harness tries to read `/tmp/sunit_test_classes.txt` via
`listFile contents lines` to use the curated class order (which
keeps `ClassQueryTest>>testAllCallsOn` counts comparable to Cog).
Our image doesn't have `#lines` on FileReference.

Effect: falls back to dynamic discovery via `TestCase allSubclasses`
— harness still runs, but with a different class iteration order.

### 2. `Character>>isLetter` broken

```
DIAG: Unicode isLetter:$t=false isDigit:$5=true
DIAG: cs isLetter: $t=false
DIAG: $t between:$a and:$z=true
DIAG: isLetter: source=  DIAG error: #Error Attempting to assign selector with wrong number of arguments.
```

`$t isLetter` returns `false`.  `$t between: $a and: $z` returns
`true` (so basic character-range math works).  Unicode classification
tables are either uninitialized or our VM has a primitive bug.

The harness diagnostic also tries to read the source code of
`isLetter` to print it; that throws `Error Attempting to assign
selector with wrong number of arguments.` — secondary bug
suggesting decompiler/reflection has a selector-arity issue.

### 3. `methodDict` doesn't see test methods (the showstopper)

For every test class:

```
DIAG-EMPTY: allTestSelectors=0 for AIAstarTest
DIAG-EMPTY: allSelectors size=58
DIAG-EMPTY: test* in allSelectors=8
DIAG-EMPTY: first5=#(#testSimpleWeightedGraphBacktracking ...)
DIAG-EMPTY: methodDict size=9
DIAG-EMPTY: test* in methodDict=0
```

- `allSelectors` (walks the class hierarchy) reports 58 selectors,
  of which 8 start with `test`.  Correct — those are the actual
  inherited + own selectors.
- `methodDict` reports size 9 — the class's own methods, including
  the 8 tests + a couple of helpers.
- But iterating methodDict and filtering for `test*` selectors
  finds **zero**.

That's the contradiction.  `allSelectors` finds them via the
class hierarchy walk; `methodDict` iteration doesn't.  Either:

- `methodDict` iteration is broken (the underlying primitive
  returns wrong selectors), OR
- Selector identity is broken (the `test*` Symbols in methodDict
  hash differently than the ones the test discovery code creates), OR
- `methodDict` keys aren't actually Symbols in our image

Without working test discovery, `SUnitRunner>>allTestSelectors`
returns empty, so each class runs 0 tests.

## What this means for the VM

The micro-benchmarks (fib, sieve, tinyBench) don't exercise:
- Reflection / `methodDict` iteration
- Symbol-table operations across realistic class hierarchies
- Character classification / Unicode tables
- File I/O selectors past the simplest paths

So we've been "as fast as Cog" on workloads that systematically
miss the parts of the VM that don't actually work.

## Root cause traced via probes (2026-05-28)

A series of probe images (`/tmp/probe_initN.st`) narrowed the bug
chain.  All three SUnit-blocking symptoms reduce to ONE root cause:
**`SparseLargeTable>>size` (or `at:`) is broken in our VM.**

The dependency chain:

1. **`SparseLargeTable>>size`** — VM either crashes or returns a
   wrong value.  Probe stops emitting output after `gc class =
   SparseLargeTable` and before the next `gc size = ...` line,
   suggesting `size` throws an error that the on:do: handler
   can't catch (or it hangs and gets killed at the SDL2 display
   timeout).

2. **`Unicode class>>isLetter:`** uses `(GeneralCategory at:
   charCode + 1)`.  GeneralCategory is a `SparseLargeTable` class
   var.  With the table broken, this returns wrong values (or
   the early-exit `index > GeneralCategory size ifTrue: [^false]`
   path fires unconditionally and returns false for everything).

3. **`Character>>isLetter`** → `self characterSet isLetter: self`
   → `Unicode isLetter: self` — always returns false on our VM.

4. **`Symbol>>numArgs`** checks `firstChar isLetter` first.  With
   `isLetter` broken, falls through to the "not-a-valid-selector"
   branch and returns `-1` for everything.

5. **`TestCase class>>allTestSelectors`** filters by
   `numArgs isZero`.  Returns `false` for every selector (`-1` ≠
   `0`), so the result is empty.

6. **Every test class runs 0 tests.**

Probe data confirming the chain:
- `$t characterSet = Unicode` ✓ (both VMs)
- `Unicode global = Unicode` ✓
- `GeneralCategory class = SparseLargeTable` ✓
- `gc size = 917632` ✓ (both VMs — `size` works fine)
- `gc at: 117` returns **5** (Cog) vs **0** (ours) ❌ — the actual bug
- `gc at: 98`  returns **5** (Cog) vs **0** (ours) ❌
- `(gc instVarAt: 2) = 917632` ✓ — slot reads work
- `base offset (instVarAt: 1) = SmallInteger` ✓ on both — layout matches
- `$t isLetter = false` (ours) vs `true` (Cog) — downstream
- `#testFoo numArgs = -1` (ours) vs `0` (Cog) — downstream

## True root cause: `SparseLargeTable>>at:` returns defaultValue (0)

The `at:` method delegates to `noCheckAt:`:

```smalltalk
noCheckAt: index
    | chunkIndex t |
    chunkIndex := index - base // chunkSize + 1.
    (chunkIndex > self basicSize or: [chunkIndex < 1]) ifTrue: [^ defaultValue].
    t := self basicAt: chunkIndex.
    t ifNil: [^ defaultValue].
    ^ t at: (index - base + 1 - (chunkIndex - 1 * chunkSize))
```

SparseLargeTable is **indexable with fixed slots** (instSpec format
3 — `IndexableWithFixed`).  The four named inst vars (base, size,
chunkSize, defaultValue) occupy fixed slots; the chunk pointers live
in the object's indexable slot area.

Our VM returns 0 (defaultValue), meaning one of:
- `self basicSize` returns wrong value (likely too small or 0)
- `self basicAt: chunkIndex` returns nil for valid indices

Both depend on our `Primitives.cpp` handling of `IndexableWithFixed`
format correctly — specifically computing the indexable slot count
as `slotCount - fixedFields`.  Other primitives we already audited
(primitiveStringReplace at Primitives.cpp:16319) get this right.
The `basicAt:`/`basicSize` family may have a bug specifically for
this format.

## Next step

Add a 1-line probe (in `Interpreter::primitiveSize` or
`primitiveBasicAt`) that logs receiver classIndex + slotCount +
fixedFields + result when the receiver is a SparseLargeTable.
Compare against Cog's expected output (basicSize ≈ 917632 /
chunkSize ≈ 64 → ~14 K chunks).

## Root cause and fix (2026-05-28)

**Bug:** JIT-compiled `basicSize` returned the source-level fallback
`^ 0` for any IndexableWithFixed/Weak (fmt 3/4/5) receiver AND for any
receiver whose header used the overflow slot-count encoding
(slotCount-byte = 0xFF, real count in the word before the header).

Both gaps were present in **two** JIT paths:

1. `stencil_primSize` (stencils.cpp:3786-3795) — fmt 3/4/5 branch
   fell through to bytecode body.
2. `emitPrimProlog_arm64` for prim 62 (AsmjitT1.cpp:2058-2103) — only
   handled fmt 2 / 10-11 / 16-23 directly, bailed on slotCount==255
   overflow header, and treated `fail` as "fall through to bytecode
   body" (= `^ 0`).

For SparseLargeTable, BOTH conditions were true: fmt=3 AND overflow
header (897 indexable chunks + 4 fixed = 901 slots > 254).
basicSize JIT-returned 0.  Downstream:
- `noCheckAt:` checked `chunkIndex > self basicSize` → `1 > 0` true →
  returned defaultValue (0) for every Unicode lookup.
- `Unicode>>isLetter:` indexed GeneralCategory → got 0 → returned false.
- `Character>>isLetter` → false for every char.
- `Symbol>>numArgs` checked firstChar.isLetter → false → returned -1.
- `OpalCompiler` arity check (numArgs vs bytecoded args) → mismatch →
  raised "Attempting to assign selector with wrong number of
  arguments." for every `compile:classified:` call.
- SUnit test discovery filtered by `numArgs isZero` → empty for every
  class → 0 tests run.

**Fix** (commit-pending):
- New runtime helper `jit_rt_primsize_ptr` (JITRuntime.cpp).  Mirrors
  the existing `jit_rt_primat_ptr` class-lookup for fmt 3/4/5; for
  fmt 9 returns slotCount directly.
- Plumbed through helpers struct (JITCompiler.hpp), extract_stencils.py
  (hole ID 20), JITCompiler.cpp (4 patch sites).
- stencil_primSize fmt 3/4/5 branch now calls the helper instead of
  falling through to `^ 0`.
- asmjit T1 prim 62 prologue extended: reads overflow word for
  slotCount==255 receivers, handles fmt 9 inline (size=slotCount),
  calls the new helper for fmt 3/4/5.

Also extended `jit_rt_array_prim`'s primKind=16 branch to handle fmt
3/4/5 (the IC-shortcut path); not strictly required for the chunk
chain but closes the same gap if reached via the IC dispatch.

**Verified:**
`gc at: 117 = 5` (was 0); `$t isLetter = true` (was false);
`#testFoo numArgs = 0` (was -1).  Chain healed end-to-end.

## Original narrowing — JIT inline Array>>at: bug (superseded)

Further probes (probe_initN.st, iterations 10-13) collapsed the
chain to a single root cause:

**`Array>>at:` returns 0 on our VM under JIT.  PHARO_NO_JIT=1
fixes everything.**

Reproduction (probe13 on `/tmp/harness/Pharo-probe13.image`):

```
Operation                                     JIT off       JIT on             Cog
gc basicAt: 1 (returns 1024-slot Array)       Array         Array              Array
chunk size                                    1024          1024               1024
chunk at: 1                                   1 OK          hang/crash FAIL    1
chunk at: 98                                  5 OK          no output  FAIL    5
chunk instVarAt: 98                           5 OK          no output  FAIL    5
```

The probe stops emitting output after `chunk size = 1024` with JIT
on.  The `chunk at: 1` call must be triggering a silent crash or
hang — not catchable by `on: Error do:` (which suggests SIGSEGV
or infinite loop, not a Smalltalk exception).

**Likely culprits:**

1. `stencil_primAt` (stencils.cpp:3614) — the JIT inline Array#at:
   stencil.  Line 3624 reads `slotCount = (header >> 56) & 0xFF`
   and handles overflow at 3625-3630 for slotCount==255.  Our
   1024-slot Array DOES have overflow header, so it should hit
   the overflow path.  Possible bugs:
   - Overflow word read at `rcvr.bits - 8` if `rcvr.bits` doesn't
     point at the header for overflow objects in our scheme
   - Slot pointer `rcvr.bits + 8` if overflow alignment is wrong
2. asmjit T1 inline `at:` emit (around AsmjitT1.cpp:4356) — may
   bypass the stencil entirely for plain-Array receivers.

**Why this matters:**
This bug breaks every Pharo program that uses Arrays with >254
slots — which is most non-trivial programs.  Including:
- `SparseLargeTable` (which uses 1024-slot chunks → blocks all
  Unicode classification → blocks all SUnit)
- Any indexable collection past the basic case
- Lots more

This is THE next bug to fix.  The audit-gap work we've been
chasing is dwarfed in impact by this one JIT correctness issue.

**Diagnostic approach:**

```bash
# Confirm the bug
cp /tmp/harness/Pharo-probe13.image /tmp/test.image
PHARO_NO_JIT=1 ./build/test_load_image /tmp/test.image      # works
                ./build/test_load_image /tmp/test.image      # fails

# lldb attach + breakpoint on stencil_primAt or the asmjit emit
# Compare the slotCount it computes against the actual 1024.

# Alternative: add a printf in stencil_primAt that fires for
# slotCount >= 255 (overflow path).  Run probe13 and inspect.
```

## Next-step priorities

1. **Fix `SparseLargeTable>>size`** — single root cause for the
   entire SUnit blockage.  Probable culprits:
   - `SparseLargeTable` uses an arrayed slot layout that needs a
     specific primitive (perhaps `primitive 70` or `primitiveSize`
     for non-indexable formats).
   - One of our prims returns the wrong slot count for
     `IndexableWithFixed` or similar format.
   - Investigate via lldb: attach, breakpoint on
     `Interpreter::primitiveSize` (or whichever is dispatched for
     `SparseLargeTable>>size`), compare result to expected ~17 K
     (Unicode general-category table covers BMP range).

2. **The "selector with wrong number of arguments" error.**
   Separate but related — appears in scheduler patch, timeout
   patch, and source decompilation paths.  Likely a method-header
   arity-encoding bug.

3. **`FileReference>>#lines`** — minor; harness has fallback.

## How to reproduce

```bash
# Stage curated class list
cp scripts/pharo-headless-test/test_classes.txt /tmp/sunit_test_classes.txt

# Clone image, inject runner via Cog
cp /tmp/harness/Pharo.image /tmp/harness/Pharo-sunit.image
cd /tmp && /tmp/harness/pharo --headless /tmp/harness/Pharo-sunit.image eval --save \
  "'$PWD/scripts/pharo-headless-test/setup_fake_gui.st' asFileReference fileIn.
   '$PWD/scripts/pharo-headless-test/run_sunit_tests.st' asFileReference fileIn"

# Clean stale completion marker (otherwise startup hook short-circuits)
rm -f /tmp/sunit_run_completed.txt /tmp/sunit_test_*.txt /tmp/sunit_batch*.txt

# Run our VM
./build/test_load_image /tmp/harness/Pharo-sunit.image > /tmp/sunit-run.out 2>&1

# Results
cat /tmp/sunit_test_results.txt      # summary
cat /tmp/sunit_test_detail.txt       # per-test detail
cat /tmp/sunit_batch_outer_err.txt   # outer errors
```
