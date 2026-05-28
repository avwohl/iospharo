# Pharo SUnit Suite — VM Compatibility Status

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

## Next-step priorities

1. **Debug `methodDict` iteration**.  This is the highest-impact
   single fix — it unblocks the entire SUnit suite.  Drop into
   lldb, set a breakpoint on the test-class-iteration path, and
   compare `methodDict do: [:m | ... ]` between Cog and our VM
   for a known class like `AIAstarTest`.

2. **Character isLetter**.  Find `Character>>isLetter` source,
   trace what primitive it calls, verify the primitive exists
   and returns correct values in our `Primitives.cpp`.

3. **The "selector with wrong number of arguments" error**.
   This appears multiple times (scheduler, timeout patch, source
   decompile).  It's likely a single bug surfacing in many
   places.  Probably in our `compiledMethod>>argumentCount` or
   selector-classification code.

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
