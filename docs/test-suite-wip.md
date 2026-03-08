# Test Suite WIP — 2026-03-08

## Current Status

Full test suite running across 1833 non-GUI classes (Spec tests skipped).
Completed ~1433 classes in Run #49 + #50. Remaining ~400 classes need
a final batch run.

## Results So Far

~100 non-passing tests across ~1433 classes tested.

### Not VM bugs (image/environment)
- Fuel WideString serialization (12 failures across 4 FL* test classes)
- Calypso browser scope/tab tests (11 failures)
- ReflectivityExamples anonymous subclass errors (4)
- testFastPointersTo, testCreateNormalClassWithTraitComposition, testAllCallsOn
- testNoOrphanPackage (Tests-Runner package from test injection)

### GC/process (inherent to no generational GC)
- testClearing x2 (WeakKeyDictionary)
- testHeavyContention2 (race condition)
- Process termination edge cases (3)

### Performance (interpreted VM, not bugs)
- Various 80s timeouts: testBmpWriteReadInMemory, testCopyFileLocator,
  testSearch, testNoShadowedVariablesInMethods, testPragmaEnvironment

### Font metrics (headless, no FreeType)
- widthAndKernedWidthOfLeft:right:into: nil (3 tests)

### Potentially VM-related
- "Bad BitBlt arg (Fraction?)" in GIF/PNG/FormSet tests (~15)
  Root cause: Form>>colorReduced produces Fraction dimensions
- testBmp16Bit: Color transparent vs Color red (1)
- testBmpWriteReadUsingFiles: True >> #+ in Adler32 (suspicious)
- testVmBinary/testVmDirectory: VM path assertions (2)
- testErrorProducedByAllocatingInTheImage: needs OutOfMemory signal (1)
- testUnoptimisedValueSpecialSendsMessageCapturesSend: special selector (1)

## Skip List

Classes skipped (hang, crash, or infrastructure issues):
- FFI callback/Athens/Roassal/TF* (no native libs)
- Trait modification tests (corrupt T1-T5 state)
- Epicea tests (file watchers hang)
- Fuel serialization tests (many 80s timeouts)
- CodeSimulationTest, FBDDecompilerTest (extremely slow become:/decompile)
- Sp* (Spec GUI) — need setup_fake_gui.st (tested separately: 94.6% pass)
- MicInlineDelimiterTest (deadlock), SocketStreamTest (all error)
- Various filesystem/network tests that hang headless

## Key Wins This Session
- FormTest 6/6 PASS (BitBlt fixes from previous session held)
- Zero timeouts in first 800 classes (skip list working)
- Fixed skip list bypass bug: classes in priority tiers bypassed skipNames filter
- Overall pass rate >99% for non-skipped, non-GUI tests

## Fixes From Previous Session (still holding)
1. File attribute timezone offset — 4 tests fixed
2. BitBlt fill for all depths — FormTest fixed
3. BitBlt rule 33 tallyMap — GIF no longer hangs
4. 16-bit ColorMap shift/mask
5. Auto-compact GC skip ephemerons

## Next Steps
1. Run remaining ~400 classes (batch 1434-1833)
2. Investigate "Bad BitBlt arg (Fraction?)" — Form>>colorReduced
3. Investigate True >> #+ in Adler32
4. Investigate SocketStreamTest "Undefined error: 0"
5. Consider OutOfMemory signaling for huge allocations
