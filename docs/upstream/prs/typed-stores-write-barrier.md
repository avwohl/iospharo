# Typed store primitives respect the write barrier

Local data (not part of the PR text):

- Upstream repo: pharo-project/pharo-vm, base branch pharo-12
- Local branch: pr15/typed-stores-write-barrier
- Work tree: /Users/wohl/pharo-work/wt/vm-typed-stores-write-barrier
- Commit: 1b6c6a060 Typed store primitives respect the write barrier (fix, JIT part and VMMakerTests tests in one commit)
- Related issue: pharo-project/pharo#10053 (open, and reports the symptom for floatAt:put: and doubleAt:put:)
- Companion image PR in this project: write-barrier-typed-store-tests (WriteBarrierTest, TFFIBackend float stores)
- Source in this project: Cog branch fix/write-barrier-typed-stores, scripts/image-fixes/21-write-barrier-integer-stores.st, docs/upstream/cog-changes.md
- Probe: scripts/upstream-pr/probes/typed-stores-write-barrier.st
- Review notes (round 1): all findings accepted. Added tests for the JIT code and for primitiveFFIIntegerAtPut. Removed the WriteBarrierTest proof line from the .tests file (the two tests carry an expectedFailure mark in Pharo 15 and flip only with the companion image PR). Corrected the "Expected" text. Restored the whitespace of two blank lines. Moved the new tests into the order of the Tonel writer (the Float64 FailsIf test sorts before ForSecondSlotArrays, not after). Reused the category of existing tests. Added VMJitMethodWithImmutabilityTest to the regression classes. Review notes (round 2): accepted. VMJittedByteArrayAccessPrimitiveTest had IMMUTABILITY off (jitOptions of VMSimpleStackBasedCogitAbstractTest), so the three JIT tests could not pass. The class now overrides jitOptions with IMMUTABILITY true (same text as VMPrimitiveCallAbstractTest). The validation stage must check that the existing tests of the class still pass with this option on. The validation stage must also check that the three JIT tests fail without the genPrimitiveStore change. Text of the cause, the reproduction and the JIT claim corrected. The author wrote the new tests without a run of the VMMaker image. The validation stage must run them. Review notes (round 3, JIT validation with Unicorn, aarch64): the three JIT tests without the JIT change did not give FAIL. The host VM crashed with a bus error, because machine code that completes the store returns to an unmapped caller address. The tests now set the caller address to the stop address (callerAddress := target). The run then ends there in both cases and the test fails on the bytes. Commit amended.

## Pull request text

The primitives that store a typed value into the bytes of an object did not test the receiver for immutability. A store into a read-only ByteArray changed the bytes and the primitive did not fail. This change makes each primitive fail with PrimErrNoModification for a read-only receiver, in the interpreter and in the machine code of the JIT.

Related: pharo-project/pharo#10053. A companion pull request to pharo-project/pharo removes the expected-failure marks of the corresponding tests in WriteBarrierTest.

### The defect

The typed store primitives (primitiveStoreInt8IntoBytes to primitiveStoreFloat64IntoBytes, primitiveStoreBoolean8IntoBytes, the Char stores, primitiveStorePointerIntoBytes) and primitiveFFIIntegerAtPut write into the object without a test of the immutable flag. The primitive of at:put: has the test.

Reproduction in a clean Pharo 15 image with the stock VM:

```smalltalk
| g |
g := ByteArray new: 8.
g beReadOnlyObject.
g uint32AtOffset: 0 put: 7.
g printString.
g float64AtOffset: 0 put: 2.5.
g printString
```

Observed: no error. The ByteArray changes after each store. The validation stage records the printed values.

Expected from the VM: each typed store primitive fails with PrimErrNoModification and does not change the bytes. Which error the caller sees (for example ModificationForbidden) depends on the fallback code of the image method. The companion pull request corrects the fallback of the float stores. The validation stage records what the caller sees with the VM change only.

The image fallback code decides which error the caller sees after the primitive fails. For example, ByteArray>>float64AtOffset:put: in Pharo 15 falls back to TFFIBackend, which tests the backend and not the ByteArray (see Notes). The companion pull request corrects that fallback.

The machine code of the JIT for these primitives has no test for a read-only receiver before this change. The interpreter and the machine code agree after this change.

### The cause

The methods primitiveStore*IntoBytes in StackInterpreterPrimitives and primitiveFFIIntegerAtPut do not call isObjImmutable: on the receiver. The method genPrimitiveStore:intoBytesWith:needsToTrap: in CogObjectRepresentationForSpur generates a store for every element size without a test of the flag.

### The change

- Each C primitive tests isObjImmutable: after it checks the data pointer, and fails with PrimErrNoModification. The check goes before the first store.
- primitiveFFIIntegerAtPut tests only when the receiver is not an ExternalAddress. A store through an ExternalAddress goes to memory outside the object.
- genPrimitiveStore:intoBytesWith:needsToTrap: generates a jump on an immutable receiver (genJumpImmutable:). The jump goes to the same label as the jump for an immediate receiver. The primitive then fails, and the C primitive or the fallback code of the method handles the case. The new code is inside the same cppIf: IMMUTABILITY switch as the other immutability code.

### Tests

New tests in VMMakerTests, class VMPrimitiveTest:

- testPrimitiveStoreFloat64IntoBytesFailsIfObjectIsImmutable
- testPrimitiveStoreInt32IntoBytesFailsIfObjectIsImmutable
- testPrimitiveStoreUInt8IntoBytesFailsIfObjectIsImmutable
- testPrimitiveStoreFloat64IntoBytesStoresIfObjectIsMutable

New tests in VMMakerTests, class VMByteArrayAccessPrimitive (primitiveFFIIntegerAtPut):

- testFFIIntegerAtPutFailsIfReceiverIsImmutable (4 byte store)
- testFFIIntegerAtPut64bitFailsIfReceiverIsImmutable (8 byte store)
- testFFIIntegerAtPutThroughExternalAddressStoresEvenIfAddressObjectIsImmutable

The last test checks the ExternalAddress exception: the ExternalAddress object is read-only and points to the data of a ByteArray, and the store still goes through.

New tests in VMMakerTests, class VMJittedByteArrayAccessPrimitiveTest (machine code):

- testPrimitiveStoreInt32DoesNotStoreIfReceiverIsImmutable
- testPrimitiveStoreUInt64DoesNotStoreIfReceiverIsImmutable (needsToTrap: true)
- testPrimitiveStoreFloat64DoesNotStoreIfReceiverIsImmutable

Each test compiles the primitive and runs the machine code with a read-only receiver. The caller address is the stop address, so a run ends there whether the machine code falls through (primitive failure) or completes the store and returns. The test checks that the bytes did not change. Machine code without the jump changes the bytes, and the test fails with a clean assertion.

Each test that has the name part FailsIf checks that the primitive fails with PrimErrNoModification and that the bytes stay zero. The test with the name part StoresIf checks that a mutable ByteArray still takes the store. We removed a test of a mutable store for primitiveStoreInt32IntoBytes and for primitiveFFIIntegerAtPut. The simulator cannot run the store (no longAt:put: in the simulator, and PrimitiveFailed in TFFIBackend), so the test fails for a reason that is not the change.

In the image, WriteBarrierTest>>testMutateByteArrayUsingDoubleAtPut and testMutateByteArrayUsingFloatAtPut test the same defect from the image side. In Pharo 15 both carry an expectedFailure mark. The companion pull request handles them, so they are not proof tests for this pull request.

The validation stage runs the existing tests of the classes VMPrimitiveTest, VMByteArrayAccessPrimitive, VMJittedByteArrayAccessPrimitiveTest and VMJitMethodWithImmutabilityTest with this change.

### How it was tested

Note: the probe results below with the "patched" VM come from an earlier build. That build had an earlier form of this commit, before the review. The results of the final form of the branch are in "Build of this branch alone, and of the set" at the end of this section.

Builds and machine: Pharo 15 build 141 (b4d2dfd) image. Stock VM: Cog v12.0.6-beta (bebdbaca8). Patched VM: pharo-12 at 398023142 plus the fix commits of this project, with this commit unchanged. macOS arm64.

Probe (float64AtOffset:put:, uint32AtOffset:put:, int16AtOffset:put:, uint8AtOffset:put: on a read-only 8-byte ByteArray, in the Pharo 15 image):
- Stock VM: each of the four stores gives no error and changes the bytes (for example uint32 gives #(3 0 0 0 0 0 0 0)).
- Patched VM: each of the four stores raises PrimitiveFailed and the bytes stay #(0 0 0 0 0 0 0 0). The caller sees PrimitiveFailed, not ModificationForbidden, because the fallback code of the image methods is not changed by this PR.

VMMakerTests, interpreter part. Before: the clean VMMaker image of unchanged pharo-12 with only the test files of this branch loaded. After: the same image with the full change.

Each test runs in 4 variants of word size and image format. Both images loaded with "DONE 0 errors".
- VMPrimitiveTest, the 3 immutable-receiver tests (Float64, Int32, UInt8): before, 4 FAIL and 8 ERROR in 12 of 12 runs (the Float64 test fails its assertion, and the Int32 and UInt8 tests reach the store in the simulator and raise an error). After, PASS in 12 of 12 runs.
- VMByteArrayAccessPrimitive, the 2 FFIIntegerAtPut immutable-receiver tests: before, ERROR in 8 of 8 runs (PrimitiveFailed inside TFFIBackend in the simulator). After, PASS in 8 of 8 runs.

VMMakerTests, machine code part (VMJittedByteArrayAccessPrimitiveTest). The JIT tests run in the Unicorn simulator. These results are for the aarch64 case only.

The Unicorn library of this host (Python package unicorn 2.1.4, found with LD_LIBRARY_PATH) fails or crashes the VM for the X64 and riscv64 cases. The 32-bit cases are not in this class. The X64 code did not run. It was read (see Notes).

We filed two harness files, not part of the pull request, into the VMMaker image after the change. The first limits the ISA matrix to aarch64 (wordSize64Parameters). The second makes Unicorn>>lastInstructionCount answer 0. Unicorn 2.1.4 answers UC_ERR_ARG to the instruction-count query. Each test that uses runUntilReturn then errors, in the clean image too.

- The 3 new JIT tests, test files only (machine code without the jump): FAIL in 3 of 3 runs for each of the 3 tests (Int32: "Got 7 instead of 145", UInt64: "Got 7 instead of 145", Float64: "Got 0 instead of 145"). The store changed the first byte of the read-only receiver.
- The 3 new JIT tests, full change: PASS in 3 of 3 runs for each of the 3 tests.
- VMJittedByteArrayAccessPrimitiveTest, whole class, per test: the clean image runs 52 tests, all PASS. The image with the full change runs 55 tests, all PASS, with the IMMUTABILITY option on. The only difference per test is the 3 new tests.
- VMJitMethodWithImmutabilityTest, whole class: 1 test, PASS in the clean image and PASS in the image with the full change.

Regressions of the interpreter classes (whole classes, clean VMMaker image against the image with the full change):
- VMPrimitiveTest: 1314 pass, 0 fail, 0 error, 142 skip before. 1330 pass, 0 fail, 0 error, 142 skip after. The 4 new tests, in 4 variants each, account for the 16 extra passes.
- VMByteArrayAccessPrimitive: 24 pass, 0 fail, 0 error before. 36 pass, 0 fail, 0 error after (3 new tests, 4 variants each).
- No test that existed before the change changed its result.

Logs: /Users/wohl/pharo-work/validate/typed-stores-write-barrier/ (jit-testonly-*.txt, jit-full-*.txt, a64b-reg-*.txt).

Build of this branch alone, and of the set:

We also built this branch alone (2026-11-23, `scripts/upstream-pr/vm-pr-build.sh`, pharo-12 at 398023142 plus this branch).  The probe on that VM: each of the four stores raises PrimitiveFailed and the bytes stay #(0 0 0 0 0 0 0 0).  The stock VM changes the bytes with no error.  With the image of the companion pull request (write-barrier-typed-store-tests), WriteBarrierTest gives 35 PASS on the VM of this branch and 6 FAIL on the stock VM.

The five VM branches of this set together (branch `pr15/all-five`,
built alone) ran the whole Pharo 15 suite with the six image pull
requests of the set: 40369 tests, 40278 pass, 2 fail, 88 skip, 1
expected failure.  The skips are the same as on the stock VM.  The two
failures do not come from this change:
`CodeSimulationTest>>testErrorCodeNotFound` needs another Cog change, and
`ZnClientTest>>testQueryGoogle` depends on the answer of Google and fails
on the stock VM too.

### Notes

- The JIT change is the part with the most risk. Three new tests run the machine code in simulation with IMMUTABILITY on: one 32-bit store, one 64-bit store (needsToTrap) and one float store. The other store sizes use the same generator code. The jump target is the same label as the jump of the immediate case. The ARM64 path ran in the simulator. The x86_64 path did not run on the test machine. The generator code is the same. The jump uses TempReg (RAX on X64) as scratch register, as the other genJumpImmutable:scratchReg: calls do. Review the x86_64 code path on a machine with a working Unicorn.
- The image methods TFFIBackend>>on:float64At:put: and on:float32At:put: test self in place of anObject. The companion pull request to pharo-project/pharo corrects them. Without that correction the two float tests of WriteBarrierTest can still fail.
- The change was not tested on Windows or on Linux.
- A 64-bit integer store from compiled code (needsToTrap: true): the machine code leaves the store to the C primitive, which now has the test. Float64 stores complete in machine code and have the new jump.
- The results of the two float tests in WriteBarrierTest do not change until the companion pull request lands. A comparison of that class before and after this change is not useful.

Generated with [Claude Code](https://claude.com/claude-code)
