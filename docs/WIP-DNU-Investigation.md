# DNU Investigation - RESOLVED

## Status: FIXED ✓

The major DNU issues have been resolved. The VM now progresses through startup without critical errors.

## Issues Fixed This Session

### 1. NLR Home Frame Detection (Previous Session)
Fixed non-local returns in blocks to find the correct home frame by:
- Using CompiledBlock's last literal to identify the enclosing CompiledMethod
- Searching for the frame executing that specific method
- Fixed off-by-one error in frame indexing

### 2. Memory Exhaustion Causing Block Allocation Failure (This Session)

**Root Cause**: The scavenge function doesn't reset eden (to avoid pointer corruption from missing forwarding pointers). This causes eden to fill up completely, and new allocations fail.

**Symptom**: `value:` sent to nil (UndefinedObject), which appeared as:
```
[DNU] Selector '#value:' not found on OrderedCollection [instance]
```
Note: The receiver shown was **wrong** due to stale `receiver_` variable. Actual receiver was nil.

**Investigation Path**:
1. Added trace showing `value:` never sent to OrderedCollection via sendSelector
2. Discovered DNU logging used stale `receiver_` instead of stack receiver
3. Fixed DNU logging - revealed actual receiver was nil (UndefinedObject)
4. Traced `do:` calls - found block argument was nil
5. Traced block creation - found `allocateSlots` returning nil
6. Traced memory allocation - found eden exhausted (1 byte remaining)
7. Found scavenge doesn't reset eden (by design, to avoid corruption)

**Fix**: Bypass eden and allocate directly in old space as temporary solution until proper GC with pointer forwarding is implemented.

### 3. DNU Logging Fix
Changed `sendDoesNotUnderstand` to use `stackValue(argCount)` for receiver instead of stale `receiver_` instance variable.

## Current State

The VM now runs through startup without the `value:` DNU errors. Remaining DNU:
- `#asPointerType` on Dictionary - FFI-related, not critical

## Key Files Modified
- `src/vm/Interpreter.cpp` - NLR home frame detection, DNU logging fix
- `src/vm/ObjectMemory.cpp` - Direct old space allocation

## Commits
- Fix NLR home frame detection for non-local returns in blocks
- Fix memory exhaustion causing block allocation failure

## Technical Notes

### Why Eden Allocation Was Failing
The scavenge implementation copies objects from eden to old space but:
1. Does NOT update pointers (no forwarding)
2. Therefore cannot reset eden (would cause corruption)
3. Eden fills up and stays full
4. All subsequent allocations fail

### Temporary Fix
Allocate new objects directly in old space, bypassing eden entirely.

### Proper Fix (TODO)
Implement proper scavenge with forwarding pointers:
1. Copy objects to to-space
2. Store forwarding pointer in from-space object
3. Scan entire heap, update pointers using forwarding table
4. Flip spaces

## Next Steps
1. The `#asPointerType` DNU is FFI-related - investigate if needed
2. Implement proper garbage collection with forwarding pointers
3. Clean up debug traces (many can be removed now)
