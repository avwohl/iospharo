# iOS Pharo VM — Work In Progress

## Current Status (2026-02-16, commit d82c201)

### Test Results

**Last full baseline (commit 2265fb6):** 10779/11732 pass (91.9%), 83 fail, 835 error, 21 skip, 14 timeout

Since that baseline, **41 commits** have been made with significant fixes.
A new full batch test has NOT completed yet — partial results below.

**Partial batch (8 classes, current build):**
- 695/727 pass (95.6%), 9 fail, 19 error, 4 skip

| Test Class | P | F | E | S | Total |
|---|---|---|---|---|---|
| SortedCollectionTest | 278 | 0 | 9 | 0 | 287 |
| IdentitySetTest | 173 | 1 | 2 | 0 | 176 |
| SmallIntegerTest | 27 | 0 | 0 | 0 | 29* |
| IntegerTest | 70 | 7 | 3 | 3 | 83 |
| FloatTest | 70 | 1 | 1 | 1 | 75 |
| FractionTest | 28 | 0 | 2 | 0 | 32 |
| PointTest | 34 | 0 | 0 | 0 | 36* |
| CharacterTest | 15 | 0 | 2 | 0 | 19 |

*Some tests in SmallIntegerTest/PointTest not counted (2 skip each).

### Fixes Since Last Baseline (41 commits)

**Process switching / scheduling:**
- `70fa3b9` Fix process switching temp corruption: sync C++→context (not reverse)
- `8150a5c` Fix primitiveYield: remove checkTimerSemaphore() call (was corrupting processes)
- `d82c201` Simplify test timeout: yield-loop time check
- `65bc5e1` Fix cannotReturn for blocks returning from dead methods
- `a7059dd` Fix BlockReturn bytecodes (5D/5E) for inlined blocks

**Primitives / arithmetic:**
- `712a05e` Fix comparison primitives for large integers + identity hash for immediates
- `17b84b0` Fix identityHashOf for SmallFloat64
- `dc2bc85` Fix long-form conditional jumps (0xEE/0xEF) mustBeBoolean enforcement
- `42ed810` Fix primitiveFindFirstInString
- `f783b8f` Fix bit-reverse and byte-swap for LargeInteger results
- `0c29aac` Implement ByteArray data access primitives 600-629
- `dddde65` Fix primitiveAt/primitiveAtPut: reject non-indexable objects

**Overflow / safety:**
- `51d47fd` Fix remaining fromSmallInteger overflows
- `2d738b5` Fix FFI callout address/time fromSmallInteger overflow
- `949e2b4` Fix executeFromContext: guard SmallInteger corruption in unrelocated fixup

**GC / finalization:**
- `70f6b3d` Fix signalFinalizationIfNeeded: standard semaphore signaling
- `1bf6287` Run weak collection tests at priority 40

### Key Findings This Session

1. **WeakKeyDictionary tests pass 100% synchronously**: 207/207 and 209/209 when run without forking
2. **Tests fail only when forked**: With `forkAt: 40` + yield loop, 34+ errors appeared
3. **After recent fixes, behavior changed**: testAdd now HANGS instead of erroring with "Instances of SmallInteger are not indexable" — the identity hash fix may have changed the failure mode from crash to infinite loop
4. **Minimal fork test**: 9/10 tests pass (including WeakKeyDictionary basics: at, sort, associations, copy). Only `testAdd` hangs
5. **No SmallInteger-at: diagnostic fired**: The `primitiveAt` on SmallInteger diagnostic added to this build never triggered — the original error cause may be resolved, but a new hang appeared

### Known Remaining Issues

**High Priority — Tests:**
- **WeakKeyDict testAdd hangs when forked**: `DictionaryTest>>testAdd` calls `nonEmptyDict` then `add: associationWithKeyNotInToAdd`. Something in this flow loops infinitely in a forked process. Hypothesis: hash table scan infinite loop after identity hash fix changed hash values
- **InstanceVariableNotFound errors** (~50): Trait-defined test methods reference instance variables that don't exist on concrete test classes. Example: `sortedCollection not found in LinkedListTest`. This is a class/trait restructuring issue — likely need to implement missing trait slot composition
- **NonBooleanReceiver errors** (~20): `SystemEnvironment >> #>` comparison sends, `storeOn:` issues — likely the `#>` DNU for non-comparable objects, need `Magnitude >> #>` or similar
- **IntegerTest failures**: testFactorial (returns 94 instead of 94!), testLargeShift, testModulo — factorial bug is in optimized partition algorithm (conditional jump issue?)

**Medium Priority — GUI:**
- Desktop renders, top menu draws, world menu draws
- Dragging startup window makes it disappear (window management issue)
- Menus don't execute actions (likely event handling / morphic issue)

### What To Do Next

1. **Run a full batch test** with the current build to get updated numbers. This is the single most important thing — we need a real baseline with all 41 fixes.
   ```bash
   scripts/run_batch_tests.sh
   ```

2. **Investigate testAdd hang**: Add bytecode tracing to the forked process to find the infinite loop location. Key suspect: hash table probing in `HashedCollection >> #findElementOrNil:` or similar. The identity hash fix (`712a05e`) changed SmallFloat hash values — verify that WeakKeyDictionary's hash function doesn't produce degenerate probe sequences.

3. **Investigate InstanceVariableNotFound**: This affects ~50 tests across many classes. Root cause is trait composition not properly adding instance variable slots to concrete test classes. May need to fix slot composition or trait application in the VM's class creation primitives.

4. **Fix factorial**: The `Integer >> #factorial` optimized algorithm returns `self` (94) instead of computing the factorial. Long-form conditional jump (0xEE/0xEF) was fixed in `dc2bc85` but factorial might use a different bytecode pattern. Need bytecode tracing of `94 factorial` execution.

5. **GUI menus**: After test suite is stable, focus on making menu actions work.

### Architecture Notes

- C++ inline stack: `stackBase_` to `stackPointer_`, `framePointer_` for current frame
- Process switch: `materializeFrameStack()` saves C++ state → context object, `executeFromContext()` restores
- GC traces C++ stack via `forEachRoot()` which iterates `stackBase_..stackPointer_`
- Saved frames (inline calls) stored in `savedFrames_[]` array, materialized to context objects on switch
- Context layout: slot 0=sender, 1=pc, 2=stackp, 3=method, 4=closure, 5=receiver, 6+=temps+stack
