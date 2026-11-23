# Typed stores into a read-only ByteArray signal ModificationForbidden

## Local data (not part of the PR text)

- Upstream repo: pharo-project/pharo, base branch Pharo15
- Local branch: pr15/write-barrier-typed-store-tests
- Work tree: /Users/wohl/pharo-work/wt/pharo-write-barrier-typed-store-tests
- Commit: f1a146233e "Typed stores into a read-only ByteArray signal ModificationForbidden"
- Related issue: pharo-project/pharo #10053 "ByteArray >> floatAt:put: and doubleAt:put: does not honor the ReadOnlyFlag" (open). Merged PR #10057 only put the expectedFailure mark back.
- Companion VM PR: typed-stores-write-barrier (pharo-project/pharo-vm, base pharo-12). "Fixes #10053" is true only with both PRs.
- Source in this project: scripts/image-fixes/20-WriteBarrierTest.st and scripts/image-fixes/21-write-barrier-integer-stores.st, docs/upstream/pharo-image-repairs-2026-11-17.md, docs/upstream/cog-changes.md
- Validation (measured): all six tests fail on the stock VM with or without the image change (the stock primitive succeeds on a read-only receiver). They pass only with the image change plus the VM PR. This image PR alone adds six tests that fail on the stock VM. The owner decides: merge both PRs together, or mark the tests until the VM change reaches the stock VM.

## Pull request text

Typed stores into a read-only ByteArray (for example doubleAt:put:, floatAt:put:, unsignedLongAt:put:) must signal ModificationForbidden, as at:put: does. This change repairs the image code of the two float stores (TFFIBackend) and of the integer fallback (ByteArray>>integerAt:put:size:signed:). The change also enables the tests that check these stores. The tests in WriteBarrierTest need a VM that also has the change typed-stores-write-barrier (companion PR in pharo-vm).

Related: #10053. The issue closes only when this PR and the VM PR are both merged.

### The defect

A read-only ByteArray can change through typed stores. Run in a clean Pharo 15 image:

```smalltalk
| b |
b := ByteArray new: 8.
b beReadOnlyObject.
b unsignedLongAt: 1 put: 16r01020304.
b
```

Expected: ModificationForbidden, and the ByteArray stays unchanged.
Observed on the stock VM (Cog v12.0.6-beta), with or without this change: no error, and the bytes change to #[4 3 2 1 0 0 0 0].

Result per selector, measured on a read-only ByteArray in the stock VM:

- No error, and the bytes change: doubleAt:put: (Float and Integer value), floatAt:put:, unsignedLongAt:put:, unsignedLongLongAt:put:, unsignedShortAt:put:, signedShortAt:put:, signedLongAt:put:, uint32AtOffset:put:, int16AtOffset:put:, float64AtOffset:put:, float32AtOffset:put:.
- ModificationForbidden, and the bytes stay: at:put:, byteAt:put:, and each store with the bigEndian: argument (for example doubleAt:put:bigEndian: and unsignedLongAt:put:bigEndian:, both true and false).

Reason: on the stock VM the primitive of each store without bigEndian: succeeds on a read-only receiver. The image code runs only when the primitive fails. With the companion VM change, the primitive fails, and the image code then runs. With the VM change and without this change, the stores signal PrimitiveFailed, not ModificationForbidden.

The two float stores of the threaded FFI backend are wrong in a second way. They tested the backend, not the object that receives the store.

### The cause

- TFFIBackend>>on:float32At:put: and TFFIBackend>>on:float64At:put: send isReadOnlyObject to self (the backend), not to anObject. The test is never true for a read-only ByteArray.
- ByteArray>>integerAt:put:size:signed: (FFI-Kernel) is the fallback of the primitives of uint8AtOffset:put: to uint64AtOffset:put: and of the signed versions. The method has no read-only check.
- WriteBarrierTest>>testMutateByteArrayUsingDoubleAtPut and testMutateByteArrayUsingFloatAtPut have an expectedFailure mark, and their last assertion reads the first byte of the ByteArray (first), not the stored Float. The assertion cannot pass after the retry.

### The change

- TFFIBackend float stores: test anObject, and signal ModificationForbidden on anObject with the retry selector #floatAt:put: or #doubleAt:put:.
- ByteArray>>integerAt:put:size:signed:: when the receiver is read-only, signal ModificationForbidden with the index-based accessor of the same size and sign as retry selector (for example #unsignedLongAt:put:).
- ByteArray>>integerAt:put:size:signed: keeps the old failure (primitiveFailed) for a read-only receiver and a size other than 1, 2, 4 or 8.
- WriteBarrierTest: remove the two expectedFailure marks, read the stored value with doubleAt: and floatAt: in the last assertion, and add four tests: testMutateByteArrayUsingUnsignedLongAtPut, testMutateByteArrayUsingUnsignedLongLongAtPut, testMutateByteArrayUsingSignedShortAtPut and testMutateByteArrayUsingDoubleAtPutInteger (an Integer value).

### Tests

- Proof: WriteBarrierTest>>testMutateByteArrayUsingDoubleAtPut, testMutateByteArrayUsingFloatAtPut, testMutateByteArrayUsingUnsignedLongAtPut, testMutateByteArrayUsingSignedShortAtPut, testMutateByteArrayUsingUnsignedLongLongAtPut, testMutateByteArrayUsingDoubleAtPutInteger.
- The proof tests can need the companion VM change. On a stock VM, a primitive can succeed on a read-only receiver. Then a test fails because of the VM, not because of the image code. Measured, 3 runs of each test (see How it was tested): all four proof tests, and the two extra tests, need BOTH this change and the companion VM change. On the stock VM, with this change, each test fails (3 of 3), because the first assertion sees that the store did not signal ModificationForbidden. On the patched VM, without this change, the tests signal PrimitiveFailed (3 of 3). Only the two together pass.
- Regression classes: WriteBarrierTest, ByteArrayTest, TFBasicTypeMarshallingTest, FFITypesTest.

### How it was tested

Environment: Pharo 15 build 141 (b4d2dfd) image, macOS arm64. Stock VM: Cog v12.0.6-beta (bebdbaca8). Patched VM: pharo-12 at 398023142 plus the commits of the companion VM change.

The Tonel loader of this project loaded the change into a copy of the stock image. The load log ends with "DONE 0 errors", and each changed method passed its check. Each test ran in a new process, 3 times.

Proof tests, results of 3 runs each. The test-only image has the test changes and not the code change. The full image has both:

- Stock VM, test-only image: each of the six tests FAIL 3 of 3 (Assertion failed).
- Stock VM, full image: each of the six tests FAIL 3 of 3 (Assertion failed). The image change alone does not repair the defect, because the stock primitive succeeds.
- Patched VM, test-only image: each of the six tests ERROR 3 of 3 (PrimitiveFailed from TFFIBackend or from integerOfObject:at:put:size:signed:).
- Patched VM, full image: each of the six tests PASS 3 of 3. The six tests have the names testMutateByteArrayUsingDoubleAtPut, testMutateByteArrayUsingFloatAtPut, testMutateByteArrayUsingUnsignedLongAtPut, testMutateByteArrayUsingSignedShortAtPut, testMutateByteArrayUsingUnsignedLongLongAtPut and testMutateByteArrayUsingDoubleAtPutInteger.
- Base image (no change): on the stock VM the two old tests (double, float) fail 3 of 3 (Assertion failed). On the patched VM they ERROR 3 of 3 (PrimitiveFailed). The other four tests do not exist in the base image.

Regression classes, stock VM, base image against full image (same result on both, test by test):

- ByteArrayTest: 13 PASS, 0 FAIL, 0 ERROR on both.
- TFBasicTypeMarshallingTest: 44 PASS on both.
- FFITypesTest: 15 PASS on both.
- WriteBarrierTest on the stock VM: base 29 PASS and 2 FAIL (the two old float tests). Full 29 PASS and 6 FAIL (the six proof tests, because the stock VM lacks the companion change). No test that passed on the base image fails on the full image.
- WriteBarrierTest on the patched VM: base 29 PASS and 2 ERROR. Full 35 PASS, 0 FAIL, 0 ERROR.

Logs: /Users/wohl/pharo-work/validate/write-barrier-typed-store-tests/ (not part of the PR).

### Notes

- The six tests need a VM with the companion change. On the stock VM, all six fail after this change, so this PR alone adds tests that fail on the stock VM. Merge the two PRs together, or mark the tests until the VM change reaches the stock VM.
- The tests use the stores without the bigEndian: argument. The stores with bigEndian: already signal ModificationForbidden on the stock VM, and the tests do not cover them.
- For a read-only receiver, integerAt:put:size:signed: now signals ModificationForbidden before the check of the value and of the index. A read-only receiver with a bad value or an out-of-range index signals ModificationForbidden, where the old code signaled the range failure.
- Not tested on Windows or Linux.
- The change adds a check only for a read-only receiver. The speed of a store into a normal ByteArray does not change, because the primitive succeeds before the fallback runs.

Generated with [Claude Code](https://claude.com/claude-code)
