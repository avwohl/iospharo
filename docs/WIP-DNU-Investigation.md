# WIP: DNU Investigation - #new Sent to Wrong Receiver

## Current Status
The VM gets past initial startup but encounters `#new` being sent to an OrderedCollection **instance** instead of a **class** object, causing DNU errors.

## Root Cause Analysis

### Symptom
```
[SEND-NEW-FAIL #1] #new not found
  rcvr=0x... classIdx=3157 slots=3 (OrderedCollection)
  Current method: <unknown> >> #startUp:
  IP offset: 57 bytecode: 0x7c
  Bytecodes around IP: 4c 80 7c f1 1 58
```

### Bytecode Decoding
The failing code is `self determineActivePlatform new`:
- `0x4c` = Push Receiver (self)
- `0x80` = Send Literal Selector #0 (`determineActivePlatform`)
- `0x7c` = Send Special Message `#new`

### The Bug
`self determineActivePlatform` should return a **class** (like `OSiOSPlatform`), but it's returning an OrderedCollection **instance** (3 slots: firstIndex, lastIndex, array).

### Call Stack at Failure
```
[0] ClassSessionHandler >> #startup:
[1] WorkingSession >> #startup:
[2] WorkingSession >> #<unknown>
[3] FullBlockClosure >> #<unknown>
[4] WorkingSession >> #on:do:
[5] Array >> #do:
```

## Fixes Made This Session

### 1. Special Object Indices (ObjectMemory.hpp)
Fixed incorrect indices that were causing Message creation to fail:
- `ClassMessage = 15` (was 14)
- `SelectorCannotReturn = 21` (was 36)
- `SelectorAboutToReturn = 48` (was 40)

### 2. primitiveTerminateTo (Primitives.cpp)
Fixed to not corrupt entire context chain:
- Now checks if target is reachable within 100 contexts BEFORE nilling
- If target unreachable, succeeds as no-op instead of nilling 10,000 contexts
- Added detailed tracing

### 3. sendDoesNotUnderstand (Interpreter.cpp)
Fixed to use stack receiver instead of stale `receiver_` variable:
- The `receiver_` instance variable was stale from previous operations
- Stack receiver at `stackValue(argCount)` is correct

## Next Steps to Investigate

1. **Trace `determineActivePlatform` execution**
   - Add tracing when this selector is sent
   - Trace the return value to see why it's an OrderedCollection

2. **Check `OSPlatform class >> determineActivePlatform`**
   - In Pharo: `^ self allSubclasses detect: [:each | each isActivePlatform] ifNone: [self]`
   - If `allSubclasses` itself is returned, that would be a collection
   - Check if `detect:ifNone:` is working correctly

3. **Possible causes**
   - Method lookup failure returning wrong default
   - `allSubclasses` being returned instead of detected class
   - Stack corruption from context termination

## Key Files Modified
- `src/vm/ObjectMemory.hpp` - Special object indices
- `src/vm/Primitives.cpp` - primitiveTerminateTo fix
- `src/vm/Interpreter.cpp` - DNU handling, extensive tracing

## Commits This Session
- Fix special object indices (ClassMessage, SelectorCannotReturn, SelectorAboutToReturn)
- Debug #new DNU: trace call stack and fix primitiveTerminateTo
- Fix primitiveTerminateTo to not corrupt entire context chain
