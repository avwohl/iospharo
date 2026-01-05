# WIP Notes - iOS Pharo VM Display Fix

## Date: 2026-01-05

## Current Status
VM initializes and starts interpreter without crashing (BitBlt fix works), but gets killed by watchdog (signal 27) during initialization.

## Completed Fixes

### 1. BitBltPlugin Registration (FIXED)
- **Problem**: `GrafPort>copyBits` crashed with "Recursive not understood error" because `primitiveCopyBits` wasn't found
- **Solution**: Created `src/include/pharovm/sqNamedPrims.h` that registers essential plugins:
  - BitBltPlugin_exports
  - B2DPlugin_exports
  - MiscPrimitivePlugin_exports
  - LargeIntegers_exports
  - FileAttributesPlugin_exports
  - LocalePlugin_exports
  - DSAPrims_exports
- Updated CMakeLists.txt to put local includes first

### 2. VM Init Timing (PARTIAL FIX)
- **Problem**: SwiftUI showed Metal canvas (60fps) before VM was initialized
- **Solution**: Moved `isRunning = true` to AFTER `vm_init()` succeeds in PharoBridge.swift

### 3. Debug Logging Reduced
- Swizzle logging now every 200,000 objects instead of 10,000
- Added null receiver debug in commonSendOrdinary

## Remaining Issues

### 1. Watchdog Timeout (Signal 27)
- App gets killed during VM initialization (~5-10 sec for 727,901 objects)
- May need to:
  - Further reduce debug output
  - Add periodic yield to main thread during init
  - Or run init completely async with progress indicator

### 2. Null Receiver Crash
- After init, crash at `longAt(rcvr)` where rcvr=0
- Added debug logging to catch selector name when this happens
- Need to identify which primitive/code is pushing null to stack

### 3. NullWorldRenderer Still Used
- Display primitives (ioScreenSize, etc.) are implemented but not called
- Pharo selects NullWorldRenderer during startup
- Need to investigate world renderer selection in Smalltalk image

## Build Process
```bash
# 1. Rebuild PharoVMCore for Mac Catalyst
cd build-test
xcodebuild -project iospharo.xcodeproj -scheme PharoVMCore -configuration Debug \
  -destination 'platform=macOS,variant=Mac Catalyst,arch=arm64' \
  SUPPORTS_MACCATALYST=YES ARCHS=arm64 ONLY_ACTIVE_ARCH=NO

# 2. Copy to xcframework
cp build-test/Debug-maccatalyst/libPharoVMCore.a \
   PharoVMCore.xcframework/ios-arm64_x86_64-maccatalyst/libPharoVMCore.a

# 3. Rebuild Swift app
xcodebuild -project iospharo.xcodeproj -scheme iospharo -configuration Debug \
  -destination "platform=macOS,variant=Mac Catalyst" \
  CODE_SIGN_IDENTITY="-" CODE_SIGNING_REQUIRED=NO
```

## Key Files Modified
- `src/include/pharovm/sqNamedPrims.h` - NEW: plugin exports registration
- `src/ios/cointerp.c` - debug logging, null receiver check
- `CMakeLists.txt` - include path order
- `iospharo/Bridge/PharoBridge.swift` - init timing fix

## Next Steps
1. Test with reduced logging - may avoid watchdog
2. If null receiver crash still occurs, check selector name in debug output
3. Investigate NullWorldRenderer selection in Pharo image
