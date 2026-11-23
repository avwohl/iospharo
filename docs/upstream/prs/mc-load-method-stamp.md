# Monticello: keep the method stamp when loading image methods

Local data (not part of the PR text)

- Upstream repo: pharo-project/pharo, base branch Pharo15
- Local branch: pr15/mc-load-method-stamp
- Work tree: /Users/wohl/pharo-work/wt/pharo-mc-load-method-stamp
- Commits: ee4085baca "Monticello: keep the method stamp when loading image methods"
- Related issues: #18445 (closed, a different stamp symptom in the method version browser)
- Source in this project: scripts/image-fixes/38-mc-load-keeps-method-stamp.st
- Review notes (round 1):
  - Eager read is not a general fix: accepted. The PR text now says that a load nested in an outer deferFlushDuring: is not covered by the read. The change also makes MCMethodDefinition>>timeStamp not keep an empty answer, and a new test covers this.
  - Claims about the intent of the guard in FilesSourcesManager>>timeStampAt:for: and about the speed gain: removed. The records do not support them (the guard came with the move of FilesSourcesManager, commit 21891a3f4a). The PR text names a change in FilesSourcesManager (flush the changes file before the read) as an alternative that was not tried.
  - Test with loader load: changed to loader basicLoad, so the dependency check (Warning) cannot interfere. The validation stage must still run the test before and after the change.
  - Review notes (round 2): the finding about the two comment strings and the uncached empty answer in MCMethodDefinition>>timeStamp is not changed. The cost is small, and the second comment explains why the empty answer is not kept.
  - MCStWriterTest moved from proof to regress in the tests file (the two tests fail only after MCPackageTest in one image).
  - The commit title is shorter. The read loop now lives in the named method MCPackageLoader>>readMethodTimeStamps.
  - Cost of the eager read: not measured.
  - The reproduction no longer needs the test packages.
- The proof entries are the three tests in MCMethodDefinitionTest.

## Pull request text

Monticello gave the method installer a nil stamp, or an empty stamp, when a method definition came from a method of the image. This pull request makes the installed method keep the real stamp. It adds three tests that check this directly.

### The defect

A method definition made with CompiledMethod>>asMCMethodDefinition has a source pointer. The definition computes the time stamp lazily, in MCMethodDefinition>>timeStamp. Two causes give a wrong stamp:

- Direct load: loadWithLoader: passes the instance variable, which is nil. The installer then writes the stamp 'nil' (from "nil asString").
- Load with MCPackageLoader: the load runs while MCPackageLoader defers the flush of the changes file. FilesSourcesManager>>timeStampAt:for: then answers the empty string, and the definition keeps this empty string.

Reproduction in a clean Pharo 15 image (no test packages needed):

```smalltalk
| definition loader |
definition := (Point >> #x) asMCMethodDefinition.
loader := MCPackageLoader new.
definition loadWithLoader: loader.
(Point >> #x) timeStamp
```

This snippet shows only the direct cause (the nil instance variable). The second test in the tests list covers the cause through deferFlushDuring:.

Observed: not measured yet (the validation stage fills in the value). Expected before the change: nil. Expected after the change: the stamp that the method had before the load.

Effect seen on Pharo 15 build 141: MCStWriterTest>>testMethodDefinition and testClassMethodDefinition fail when they run after MCPackageTest in the same image. MCPackageTest unloads the mock package, and its tearDown installs the snapshot again. Then the MC writer writes a wrong stamp. The PR does not claim that the present change fixes these two tests. The validation stage measures the result.

### The cause

- MCMethodDefinition>>loadWithLoader: sends "changeStamp: timeStamp" to the method installer. That is the instance variable. For a definition from the image, the variable is nil until a sender calls the accessor #timeStamp.
- MCPackageLoader>>basicLoad runs the load in SourcesManager current deferFlushDuring:. During that time FilesSourcesManager>>timeStampAt:for: answers the empty string ("flushChanges ifFalse: [ ^ '' ]").
- MCMethodDefinition>>timeStamp keeps that empty answer in the instance variable.

### The change

- MCMethodDefinition>>loadWithLoader: sends "changeStamp: self timeStamp".
- MCMethodDefinition>>timeStamp does not keep an empty answer, so a later call can answer the real stamp.
- MCPackageLoader>>basicLoad sends the new method #readMethodTimeStamps before basicLoad defers the flush. Each method definition then holds the real stamp when the load needs the stamp.

Limit: the read in basicLoad does not help a load that runs inside an outer deferFlushDuring: section. deferFlushDuring: does nothing when the flush is already deferred, so the read also runs with the flush deferred. In this case the definition answers an empty stamp, as before, but it does not keep the empty stamp.

Alternative that was not tried: change FilesSourcesManager>>timeStampAt:for: so that the stamp is correct during a deferred flush (for example, flush the changes file before the read).

A change in FilesSourcesManager>>timeStampAt:for: would cover the nested case. The reviewers can choose this change in FilesSourcesManager instead. The intent of the present guard is not known to the author.

### Tests

New tests in MCMethodDefinitionTest:

- testLoadKeepsTimeStampOfMethodFromImage: install a method with a known stamp, make a definition with asMCMethodDefinition, call loadWithLoader:, check the stamp of the method. It must fail before the change (stamp 'nil').
- testLoadWithPackageLoaderKeepsTimeStampOfMethodFromImage: the same, with MCPackageLoader>>basicLoad (not #load, so the dependency check cannot interfere). It must fail before the change.
- testLoadTimeStampIsNotCachedWhileFlushIsDeferred: ask a definition for the stamp inside deferFlushDuring:, then outside. The second answer must be the real stamp. It must fail before the change.

The tests do not depend on the order of the tests. MCStWriterTest is run for regressions only.

### How it was tested

Test setup: Pharo 15 build 141 (b4d2dfd) with the stock Cog VM v12.0.6-beta (bebdbaca8), macOS arm64. The author loaded the change into a clean Pharo 15 image from the Tonel files of the branch. The loader reported "DONE 0 errors". A second image had only the new tests, without the fix, to show the defect.

Proof tests, each run 3 times in a new VM:

- MCMethodDefinitionTest>>testLoadKeepsTimeStampOfMethodFromImage: FAIL 3 of 3 before ('nil' instead of '1/2/2020 10:20'), PASS 3 of 3 after.
- MCMethodDefinitionTest>>testLoadWithPackageLoaderKeepsTimeStampOfMethodFromImage: FAIL 3 of 3 before ('nil' instead of '1/2/2020 10:20'), PASS 3 of 3 after.
- MCMethodDefinitionTest>>testLoadTimeStampIsNotCachedWhileFlushIsDeferred: FAIL 3 of 3 before (empty string instead of '1/2/2020 10:20'), PASS 3 of 3 after.

Regression runs, whole classes, base image and image with the change. Each test gives the same result in both images, except the three new tests:

- MCMethodDefinitionTest: 8 PASS before, 11 PASS after (the three new tests).
- MCStWriterTest: 9 PASS before and after.
- MCPackageTest: 2 PASS before and after.
- MCPackageLoaderTest: 1 PASS before and after.
- MCSnapshotTest: 5 PASS before and after.
- MCWorkingCopyTest: 9 PASS before and after.

### Notes

- The read in basicLoad costs one read of the sources or changes file for each method definition that has a source pointer and no stamp. A definition with a stamp (all definitions that are read from a package file) does no read. The cost was not measured.
- A load nested in an outer deferred section is not covered (see The change).
- Windows and Linux were not tested.
- The change affects only the stamp of the installed method. It does not change the runtime behaviour.

Generated with [Claude Code](https://claude.com/claude-code)
