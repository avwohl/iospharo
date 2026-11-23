# SettingsStonReader: ignore a stored setting whose instance variable is gone

## Local data (not part of the PR text)

- Upstream repo: pharo-project/pharo, base branch Pharo15
- Local branch: pr15/settings-ston-stale-variable
- Work tree: /Users/wohl/pharo-work/wt/pharo-settings-ston-stale-variable
- Commit: c6d642c27f SettingsStonReader: ignore a stored setting whose instance variable is gone
- Related issue: #17376 (closed, same class, a different defect: #default in fromSton:)
- Source in this project: scripts/image-fixes/20-SettingsStonReaderTest.st, docs/upstream/pharo-image-repairs-2026-11-17.md (section SettingsStonReaderTest)
- Change from the project fix: the PR test also uses shouldnt:raise: InstanceVariableNotFound and adds a secureLoad test. The PR also changes the text of the storedSettings assertion.
- Review notes: the class SystemSettingsPersistenceTest does not exist, so the regression list now names SettingsManagerTest. The PR text softens the claim about startup because the team measured no startup trace. The test does not cover a valid entry before the stale one. The Notes say that load drops all entries of the file. The round 2 review asked for four text changes (pronoun, direct-caller note, softer design claim, active voice). The author made all four.

## Pull request text

SettingsStonReader>>load lets InstanceVariableNotFound escape when a settings file names an instance variable that the class no longer has. SettingsStonReader>>secureLoad catches only STONReaderError, so the safe loader does not protect the caller either. The change handles the error in load, and the test that was an expected failure now passes.

### The defect

A user has a saved settings file. A later Pharo version removes or renames an instance variable of a stored setting class. Then the settings manager reads the file and signals InstanceVariableNotFound. The settings are not applied. The error can reach the caller of SettingsManager>>allStoredSettings, which calls secureLoad.

Reproduction in a clean Pharo 15 image:

```smalltalk
| reader |
reader := SettingsStonReader new.
reader stream: MockSettings changedClassStoredSettingVariable readStream.
reader secureLoad
```

Observed: the reader signals InstanceVariableNotFound.
Expected: no error, and an empty collection as the answer.

### The cause

STONReader>>parseNamedInstVarsFor: signals InstanceVariableNotFound (not a STONReaderError) when a stored name is not an instance variable of the class. SettingsStonReader>>load does not handle this error. SettingsStonReader>>secureLoad handles only STONReaderError.

### The change

SettingsStonReader>>load handles InstanceVariableNotFound around the STON read. Then load leaves storedSettings as nil and answers an empty collection. The change does not edit secureLoad, and secureLoad now also answers an empty collection for such a file.

Design choice: one stale entry makes load drop the whole file. A skip of only the stale entry needs a change in STONReader or a reader subclass in System-Settings, which is more than this fix. STONReader>>parseNamedInstVarsFor: signals the error in the middle of the parse of one object. A handler in load fits the existing test, which calls load and demands no error.

### Tests

- SettingsStonReaderTest>>testNonExistingStoredSettingVariable: the change removes the expectedFailure pragma. The test now checks that load does not raise InstanceVariableNotFound. The last assertion checks the reader that did the load (the old assertion looked at a new reader and was always true).
- SettingsStonReaderTest>>testSecureLoadNonExistingStoredSettingVariable: new. secureLoad answers an empty collection.
- Regression classes to run: SettingsStonReaderTest, SettingsManagerTest, SettingsStonWriterTest, StoredSettingsMergerTest.

### How it was tested

Test environment: Pharo 15 build 141 b4d2dfd image, stock Cog v12.0.6-beta bebdbaca8, macOS arm64. We loaded the change into a fresh copy of the base image with the Tonel loader of the test scripts. The load log ended with "DONE 0 errors" and listed the expected ADD and MODIFY lines.

Proof tests. The "before" image has only the test changes of this branch. The "after" image has the full change. Each test ran 3 times in each image.
- SettingsStonReaderTest>>testNonExistingStoredSettingVariable: FAIL 3 of 3 before (the reader raises InstanceVariableNotFound), PASS 3 of 3 after.
- SettingsStonReaderTest>>testSecureLoadNonExistingStoredSettingVariable: ERROR 3 of 3 before (InstanceVariableNotFound), PASS 3 of 3 after.

Regression classes. Each class ran once on the base image and once on the full image. No test that passed on the base image fails on the full image.
- SettingsStonReaderTest: base 6 PASS and 1 ERROR (the old expected-failure test), after 8 PASS.
- SettingsManagerTest: 9 PASS before, 9 PASS after.
- SettingsStonWriterTest: 5 PASS before, 5 PASS after.
- StoredSettingsMergerTest: 2 PASS before, 2 PASS after.

Not tested: Windows and Linux (the change is platform independent).

### Notes

- A settings file with one stale entry loses all its settings, also the valid entries before and after the stale entry. The load does not change the file on disk.
- Direct callers of load no longer see InstanceVariableNotFound. The only caller in the image is secureLoad.
- Not tested on Windows or Linux. The change does not depend on the platform.

Generated with [Claude Code](https://claude.com/claude-code)
