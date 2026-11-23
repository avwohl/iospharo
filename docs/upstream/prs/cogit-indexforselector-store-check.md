# Cogit: store the followed literal with the store check

## Local data (not part of the PR text)

- Upstream repo: pharo-project/pharo-vm, base branch pharo-12
- Local branch: pr15/cogit-indexforselector-store-check
- Work tree: /Users/wohl/pharo-work/wt/vm-cogit-indexforselector-store-check
- Commits:
  - ff9603c17 Cogit: store the followed literal with the store check
  - e20109222 VMMakerTests: a followed young literal makes an old method remembered
- Related issues and pull requests (all different defects):
  - pull request 1150, open, a different store without a barrier (primitiveStringReplace)
  - pull request 1151, open, permspace forwarders
  - issue 689, a different crash (unlinking sends after become:)
- Source in this project: branch fix/indexforselector-store-check in
  ~/src/pharo-vm, and the record in docs/upstream/cog-changes.md.
- Review notes (round 1): commit title shortened and wrapped at 72 columns, trailer set to Opus 5.5, local run details removed from the commit message (they stay in the PR text). The STE check results 3.5 and 3.6 were read and are false positives or heading text.
- Triage id: cog-indexforselector.

## Pull request text

Cogit>>indexForSelector:in:at: follows a forwarded literal of a method
and writes the followed object back into the method with a plain
longAt:put:.  This store has no store check.  This pull request uses
storePointer:ofObject:withValue: for the store, and adds a unit test.

### The defect

A become: can make an old literal into a forwarder to a young object.
The old method that holds the literal then refers to a young object.
When a compaction of the machine code zone unlinks a send of that
method, indexForSelector:in:at: replaces the forwarder in the literal
frame with the young object.  The method is not in the remembered set
after this store.

The next scavenge does not update this literal.  The young object
moves or dies, and the literal keeps the old address.  The next full GC
reads this address and can stop with a segmentation fault in
markAndTrace or in fixFollowedField:ofObject:withInitialValue:.

The crash is not deterministic.

The stock VM v12.0.6 (macOS arm64) stopped in the tests of the
package PharoMisc.  The test LoArrayAutomaticEventGenerationTest>>testReset does a become: on the
literal Array of its method.  A later test calls Smalltalk
garbageCollect.  2 of 5 runs stopped with a segmentation fault.

No small script shows the defect, because a checked store
repaired the literal first in each of three scripts.  The image-level
reproduction is to run the tests of the package PharoMisc again and
again on the stock VM v12.0.6: 2 of 5 runs stopped.  This reproduction
was not run on a clean Pharo 15 image.  The unit test below is the
deterministic proof: it shows the missing remembered-set entry.

Expected: the old method is in the remembered set after the store.
Observed: the old method is not in the remembered set.

### The cause

Cogit>>indexForSelector:in:at: (called from
Cogit>>relocateIfCallOrMethodReference:mcpc:delta: during
compactCogCompiledCode) writes the followed literal with
objectMemory longAt:put:.  This write skips the store check of
SpurMemoryManager>>storePointer:ofObject:withValue:.

### The change

The write uses objectMemory storePointer: i ofObject: methodOop
withValue: methodLiteral.  An old method that gets a young value is now
in the remembered set.  A comment names the reason.

### Tests

New test: VMCogitIndexForSelectorTest>>testFollowedYoungLiteralOfOldMethodRemembersTheMethod.
The test builds an old method with a literal that is a forwarder to a
young object, and calls indexForSelector:in:at:.  The test checks that
the literal is the young object and that the method is in the remembered
set.  The test must fail without the change and pass with the change.

Existing tests for regressions: VMSelectorIndexDereferenceRoutineTest,
VMCodeCompactionTest, VMForwardLiteralInMachineMethodTest.

### How it was tested

Versions: the image before is the Pharo VMMaker image of the
unchanged pharo-12 branch.  The image after is the same image with this
branch loaded through the Tonel loader of this project.  The VM that ran
the images is the patched Cog build of this project (pharo-12 at 398023142
plus the fix commits).  The stock VM for the Pharo 15 image is Cog
v12.0.6-beta bebdbaca8 with Pharo 15 build 141 b4d2dfd.  The host is
macOS arm64.

The change is in the Slang code of the simulator, so no
Pharo 15 image test and no probe applies.  The change was not run as a
generated C VM.

Load check: the loader reported "DONE 0 errors" for the full change and
for the test-only copy (the test class without the Cogit change).

Host limit: the Unicorn library of this host fails for the X64 case with a
KeyNotFound error.  The library is libunicorn.2.dylib from a Python
package, found with LD_LIBRARY_PATH.  The riscv64 case stops the VM with a
bus error.

These two failures happen with the unchanged pharo-12 image too (class
VMForwardLiteralInMachineMethodTest on the clean image).  For this reason
all results below are for the aarch64 case only.  A harness file (not part
of the pull request) made the aarch64 case the only case of
VMSimpleStackBasedCogitAbstractTest.  The IA32 and ARMv5 cases were not
run.

Proof test, run 3 times in each image:
- VMCogitIndexForSelectorTest>>testFollowedYoungLiteralOfOldMethodRemembersTheMethod,
  aarch64 case, test class without the Cogit change: FAIL 3 of 3
  (Assertion failed).
- The same test with the full change: PASS 3 of 3.

Regression classes, aarch64 case, unchanged image and image with the
change:
- VMSelectorIndexDereferenceRoutineTest: 3 PASS, 1 SKIP before, and 3 PASS,
  1 SKIP after.
- VMForwardLiteralInMachineMethodTest: 1 PASS before and 1 PASS after.
- VMCodeCompactionTest: the class as a whole stops the VM with a bus
  error in libunicorn (aarch64 translation) in both images, so the class
  has no count.  Run one test at a time, 9 of 11 tests give PASS before
  and PASS after: testCompactingAnUnusedMethodCompactsRemainingMethodToTheBeginning,
  testCompactingShouldRelocateInSendsLiterals,
  testCompactingShouldRelocateLiterals,
  testCompactingShouldRelocateMethodReference,
  testCompactingShouldRelocateMonomorphicCallsite,
  testCompactingShouldRelocatePICCallsite,
  testCompactingShouldRelocatePICInterpreterAbortCallsite,
  testCompactingShouldRelocatePICMNUAbortCallsite and
  testRelocatingAMethodDoesNotAffectTheFrameCreationPushes.  The 2 tests
  testCompactDuringInterpreterPrimitiveThatMovesCurrentMethod...
  (two selectors) stop the VM with a bus error before and after, so they
  have no result.
- VMCogitIndexForSelectorTest in the image with the change: 1 PASS.

Logs: /Users/wohl/pharo-work/validate/cogit-indexforselector-store-check/

Build of this branch alone, and of the set:

We also built this branch alone (2026-11-23, `scripts/upstream-pr/vm-pr-build.sh`, pharo-12 at 398023142 plus this branch).  The generated VM starts the Pharo 15 image and runs a short evaluation.  No image-level probe shows this defect on request.

The five VM branches of this set together (branch `pr15/all-five`,
built alone) ran the whole Pharo 15 suite with the six image pull
requests of the set: 40369 tests, 40278 pass, 2 fail, 88 skip, 1
expected failure.  The skips are the same as on the stock VM.  The two
failures do not come from this change:
`CodeSimulationTest>>testErrorCodeNotFound` needs another Cog change, and
`ZnClientTest>>testQueryGoogle` depends on the answer of Google and fails
on the stock VM too.

### Notes

- The change is in Slang, so the build generates the C code again.
- The only caller of the changed code is
  Cogit>>relocateIfCallOrMethodReference:mcpc:delta:, from
  compactCogCompiledCode.  The only caller found of that method is
  CoInterpreter>>commenceCogCompiledCodeCompaction.  The callers of
  commenceCogCompiledCodeCompaction were not traced, so it is not
  proved that no caller runs in the middle of a GC phase.  Reviewers
  should check this.
- Not tested: Linux, Windows, x86_64.

Generated with [Claude Code](https://claude.com/claude-code)
