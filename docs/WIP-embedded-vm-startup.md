# WIP: Embedded VM Startup Fix

## Status: Fixed (2026-01-14)

## Problem
When loading a Pharo image in embedded mode, the snapshot operation called `primitiveQuit`. Previously, we just ignored the quit and returned Success, but this left the VM in a broken state with corrupted stack (wrong objects like SessionManager instead of intended targets), causing cascading DNU errors.

## Solution
Modified `primitiveQuit` in `src/vm/Primitives.cpp` to properly handle embedded mode:
1. Pop the broken stack state
2. Call `tryReschedule()` to find another runnable process
3. MorphicRenderLoop at priority 40 is found and runs
4. UI event loop starts correctly

## Key Changes
- `src/vm/Primitives.cpp`: primitiveQuit now reschedules instead of continuing broken execution
- `src/vm/Interpreter.cpp`: Cleaned up debug logging

## Testing
- App starts without DNU errors
- MorphicRenderLoop runs correctly
- Menu bar renders with 8 items
- Heartbeat and sync continue normally

## Remaining Work
- Test menu item clicks to verify original lockup issue is fully resolved
- Implement drag-to-select menu behavior (pending)
