# World Menu Drawing Issue - Investigation Notes

## Status: BLOCKED - 3 days of investigation

## Problem Summary
- World menu (right-click popup) does not appear
- Top menu bar menus work (native macOS menus)
- Touch/click events ARE reaching the VM correctly
- Display IS rendering (Pharo desktop shows correctly)

## Root Cause Identified
Method lookup failures when processing events:
- Basic methods like `#owner`, `#layoutChanged`, `#defaultColor` not found
- `MorphicRenderLoop` (a CLASS) receives instance messages like `#initialize:`, `#valueChanged:`
- This suggests incorrect object references in the notification/subscription system

## Key Evidence

### Touch Events Work
```
[TOUCH] Detected right-click (touchesEnded without touchesBegan) at (368, 354)
[BRIDGE] sendTouchDown at (368.42578125, 354.3046875) buttons=1
```

### DNU Cascade on Event Processing
```
[DNU] Selector '#initialize:' not found on MorphicRenderLoop (args=1)
[DNU] Selector '#valueChanged:' not found on MorphicRenderLoop (args=1)
[DNU] Selector '#owner' not found on SystemWindow (args=0)
[DNU] Selector '#layoutChanged' not found on Morph (args=0)
```

### Method Lookup Traces
```
[LOOKUP] #owner depth=3 class=Morph methodDict=valid
[LOOKUP] #owner depth=4 class=Object methodDict=valid
[LOOKUP] #owner NOT FOUND after 6 classes
```

## What Works
- Image loading
- Class table loading (23,552 classes)
- Initial desktop rendering
- Display updates
- Native macOS menu bar

## What Doesn't Work
- World menu (right-click popup)
- Apparently any user interaction that triggers Morphic event processing

## Theories
1. Startup sequence differs from official VM - something not initialized
2. MorphicRenderLoop incorrectly registered as observer
3. Method dictionary lookup has edge cases that fail
4. Event processing code path differs from what Pharo expects

## Startup Sequence Comparison

### OpenSmalltalk VM Startup (iOS)
1. `UIApplicationMain()` → `sqSqueakMainApplication`
2. `setupFloat()` - Float word order configuration
3. `setupErrorRecovery()` - Signal handlers for crashes
4. `fetchPreferences()` - Load user settings
5. `setVMPathFromApplicationDirectory()`
6. `findImageViaBundleOrPreferences()`
7. `readImageIntoMemory()` - Image loading
8. On main thread:
   - `makeMainWindow()` - Create display window
   - `setupMenus()` - Initialize menus
   - `setupBrowserLogic()` - Browser integration
   - `setupSoundLogic()` - Audio setup
9. `setupTimers()` - Timer subsystem
10. `setupAIO()` - Async I/O subsystem
11. `ioInitThreads()` - Threading subsystem
12. `interpret()` - **Resumes from saved process context**

### Our Startup (iospharo)
1. `vm_initialize()` - Creates ObjectMemory
2. `vm_loadImage()` - Loads image, creates Interpreter
3. `vm_run()`:
   - `startHeartbeat()` - Timer thread
   - `vm_postWindowEvent()` - Send size to VM
   - `interpret()` → `bootstrapStartup()`:
     - Attempt 1: `SmalltalkImage>>recordStartupStamp`
     - Attempt 2: `SmalltalkImage>>restartMethods`
     - Attempt 3+: `World>>doOneCycle` repeatedly

### Critical Differences

#### 1. Process Resume vs Manual Method Invocation
OpenSmalltalk resumes from the **saved suspended context** in the image.
We try to **manually invoke startup methods** which:
- Bypasses the normal startup sequence
- May leave the object graph in an inconsistent state
- Doesn't properly initialize Morphic infrastructure

#### 2. Missing Subsystems
We don't call:
- `setupFloat()` - Could affect float handling
- `setupErrorRecovery()` - Crash handling
- `setupTimers()` - May affect timer-dependent code
- `setupAIO()` - Async I/O
- `ioInitThreads()` - Threading primitives

#### 3. Event Handling
OpenSmalltalk uses `interpreterProxy->signalSemaphoreWithIndex()` to signal the input semaphore when events arrive. We do similar signaling but the Morphic event processing fails due to DNU errors.

## Root Cause Hypothesis

**The MorphicRenderLoop corruption is likely caused by our manual startup sequence.**

When we call `World>>doOneCycle` manually instead of resuming from the saved context:
1. The Announcer/Subscriber infrastructure may not be properly initialized
2. Objects that should be instances are instead class references
3. Method lookups fail because the receiver isn't what the code expects

The DNU errors show:
- `MorphicRenderLoop` (a CLASS) receives `#initialize:`, `#valueChanged:`, `#addTopicSpec:`
- These are messages meant for an INSTANCE of some announcement subscriber

This suggests that during our manual startup, the subscriber list got corrupted:
- Instead of storing `anInstanceOfSubscriber`, it stores `MorphicRenderLoop` (the class)
- When events fire, the class receives instance messages and fails

## Update: January 17, 2026

### New Findings

After adding debugging, we discovered:

1. **Process resume IS working correctly**
   - `bootstrapStartup()` is NOT being called during normal operation
   - The VM resumes from the saved context as expected
   - No `[STARTUP]` logs appear during normal execution

2. **Menu interactions work in test_platform**
   - Left-click menu interactions work correctly
   - The Quit menu item triggers `primitiveQuit`
   - No DNU errors during menu clicks

3. **The DNU errors may be image-specific or intermittent**
   - With fresh Pharo 12 and 13 images, no DNU errors observed
   - The original DNU logs may have been from a corrupted image state

### What Still Needs Testing
- Right-click (world menu) specifically
- The Mac Catalyst app with interactive use
- Different image states (freshly saved vs long-running)

### Updated Hypothesis
The original DNU errors may have been caused by:
1. A corrupted image state that got saved
2. Image-specific configuration (subscriber lists with wrong objects)
3. An edge case in our event handling that's not triggered by test_platform

### Next Steps
1. Test right-click (button=1) to trigger world menu
2. Interactive test of Mac Catalyst app with fresh image
3. Check if the DNU errors are reproducible with specific images
4. If DNU errors reappear, trace the specific object that receives the wrong message
