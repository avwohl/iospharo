# Display Setup Analysis: Official Pharo vs iOS Implementation

## Overview

This document analyzes how display setup works in the official Pharo VM (OpenSmalltalk with OSWindow/SDL2) versus our iOS implementation, identifying key differences and potential issues.

---

## Official Pharo Display Setup

### Key Components

1. **Display Global** - An instance of `Form` (32-bit depth)
2. **WorldMorph** - The root morph containing all visible content
3. **WorldState** - Coordinates rendering, damage tracking, stepping
4. **AbstractWorldRenderer** - Abstract interface for renderers
5. **OSWorldRenderer** - Concrete renderer using OSWindow/SDL2

### Initialization Sequence

```
1. Bootstrap starts
2. WorldMorph.initializeWorldAndActiveWorld called
3. Creates WorldMorph instance
4. Binds to 'World' global in SmalltalkDictionary
5. WorldState created with NullWorldRenderer (initially)
6. AbstractWorldRenderer.detectCorrectOneForWorld: selects appropriate renderer
7. OSWorldRenderer.doActivate creates:
   - Form at screenExtent * canvasScaleFactor, depth 32
   - OSWindow with SDL2 backend
   - Event handler bound to WorldMorph
8. Display Form stored in OSWorldRenderer.display
9. World.display -> WorldState.worldRenderer.display returns the Form
```

### Display Form Creation (OSWorldRenderer)

```smalltalk
doActivate [
    initialExtent := world worldState realWindowExtent
        ifNil: [ self class defaultExtent ].  "976@665"

    display := Form extent: initialExtent * self canvasScaleFactor depth: 32.
    world extent: initialExtent.

    driver := self pickMostSuitableWindowDriver.
    osWindow := OSWindow createWithAttributes: attributes
                         eventHandler: (OSWindowMorphicEventHandler for: world).
]
```

### Rendering Flow

```
WorldMorph >> doOneCycle
  -> displayWorldSafely
    -> WorldState >> displayWorldState: ofWorld:
      -> renderer displayWorldState: worldState ofWorld: self
        -> drawDuring: [ :canvas | ... draw morphs ... ]
        -> updateDamage: allDamage
          -> osWindowRenderer updateAreas: scaledDamage
```

### Key Insight: Display is Created BY the Renderer

The Display Form is NOT a pre-existing global. It is CREATED by OSWorldRenderer when it activates. The `Display` global is set after the Form is created.

---

## Our iOS Implementation

### Key Components

1. **SimpleDisplaySurface** - C++ pixel buffer (std::vector<uint32_t>)
2. **gDisplaySurface** - Global pointer to DisplaySurface
3. **displayForm_** - Interpreter member trying to hold the Display Form
4. **renderWorldMorphs()** - Direct C++ morph rendering
5. **syncDisplayToSurface()** - Copies Form bits to platform surface

### Initialization Sequence

```
1. iOS app calls vm_setDisplaySize(800, 600, 32)
2. Creates SimpleDisplaySurface with pixel buffer
3. vm_loadImage() loads the Pharo image
4. Image has NullWorldRenderer (headless mode)
5. vm_run() starts interpreter thread + heartbeat
6. Heartbeat calls syncDisplayToSurface() at 30fps
7. syncDisplayToSurface() searches for Display global - NOT FOUND
8. Falls back to renderWorldMorphs() - direct C++ rendering
```

### The Problem

**NullWorldRenderer does NOT create a Display Form!**

Looking at NullWorldRenderer:
```smalltalk
NullWorldRenderer >> doActivate [
    "do nothing - headless mode"
]

NullWorldRenderer >> display [
    ^ nil  "or creates temp form only when needed"
]
```

The iOS-ready image uses NullWorldRenderer because:
1. It was saved in headless mode
2. No OSWindow/SDL2 available
3. WorldState defaults to NullWorldRenderer

---

## Key Differences

| Aspect | Official Pharo | Our iOS Implementation |
|--------|---------------|------------------------|
| Display Creation | OSWorldRenderer creates Form | We expect to find existing Form |
| Display Global | Set after renderer activates | Looking for it (doesn't exist) |
| Renderer | OSWorldRenderer with SDL2 | NullWorldRenderer (headless) |
| Pixel Buffer | Form.bits (Bitmap object) | C++ std::vector<uint32_t> |
| Update Trigger | Morphic calls primitive 231 | Heartbeat polls every 33ms |
| Canvas | FormCanvas on Display Form | Direct pixel manipulation |

---

## Why Display Global Doesn't Exist

1. Image saved with `Smalltalk saveAs: 'Pharo-iOS-Ready'` in headless mode
2. WorldState has `worldRenderer := NullWorldRenderer`
3. NullWorldRenderer never creates a Display Form
4. No `Display` binding in SmalltalkDictionary
5. Our code searches for 'Display' global -> returns nil

---

## Solutions

### Option A: Create Display Form from C++ (Complex)

1. Allocate Smalltalk Form object via memory_.allocate()
2. Set class index to Form class
3. Create Bitmap for bits
4. Store dimensions
5. Bind to 'Display' global
6. Inject into WorldState.worldRenderer.display

**Problems**:
- Complex object creation from C++
- Need to find Form class index
- Need to update Smalltalk globals

### Option B: Switch to OSWorldRenderer (Requires OSWindow)

1. Build OSWindow with iOS Metal backend
2. Let Pharo use normal rendering path
3. OSWorldRenderer creates Display

**Problems**:
- OSWindow has SDL2 dependency
- Need to create iOS Metal driver

### Option C: Create iOS-specific WorldRenderer (Best)

1. Create `iOSWorldRenderer` Smalltalk class
2. Subclass of AbstractWorldRenderer
3. Creates Display Form on activation
4. Renders to our pixel buffer via primitives
5. Register with higher priority than NullWorldRenderer

**Implementation**:
```smalltalk
iOSWorldRenderer >> doActivate [
    display := Form extent: self actualScreenSize depth: 32.
    Smalltalk at: #Display put: display.
]

iOSWorldRenderer >> updateDamage: rects [
    "Call primitive to copy display bits to native surface"
    self primitiveCopyDisplayToSurface
]
```

### Option D: Intercept and Create Display (Current Partial Solution)

What we're currently doing:
1. Intercept displayWorldStateOf:during: on NullWorldRenderer
2. Try to execute draw block
3. Fall back to renderWorldMorphs() for direct pixel drawing

**Problems**:
- renderWorldMorphs() doesn't use Morphic drawing
- Colors and layout are approximate
- No text rendering
- Missing most visual features

---

## Recommended Fix

### Short Term: Create Display Form via Primitive

Add primitive that:
1. Creates a Form object at specified size
2. Binds it to 'Display' global
3. Returns it for Pharo to use

```cpp
PrimitiveResult Interpreter::primitiveCreateDisplay(int argCount) {
    // Get dimensions from stack
    int width = stackValue(1).asSmallInteger();
    int height = stackValue(0).asSmallInteger();

    // Create Form object
    Oop form = createFormObject(width, height, 32);

    // Bind to Display global
    bindGlobal("Display", form);

    // Store locally
    displayForm_ = form;

    // Pop args, push form
    popN(argCount);
    push(form);
    return PrimitiveResult::Success;
}
```

### Medium Term: iOS WorldRenderer

Create Smalltalk class that:
1. Subclasses AbstractWorldRenderer
2. Has higher priority than NullWorldRenderer
3. Creates Display Form properly
4. Uses primitives to sync to native surface

### Long Term: OSWindow iOS Backend

Port OSWindow to use iOS Metal/UIKit:
1. Create OSWindowIOSDriver
2. Use MTKView for rendering
3. Map touch events to mouse events
4. Full Morphic rendering support

---

## Current State Summary

Our iOS implementation:
- Has working pixel buffer (SimpleDisplaySurface)
- Has heartbeat calling syncDisplayToSurface() at 30fps
- Has fallback renderWorldMorphs() for direct rendering
- Missing: Display Form creation
- Missing: Proper Morphic canvas drawing
- Result: Only test pattern visible, no real Morphic UI

The fix needs to happen at the Smalltalk level - we need to create and bind a Display Form that Morphic can draw to, then copy its bits to our native surface.

---

## Files Referenced

### Our Implementation
- `/Users/wohl/src/iospharo/src/platform/PlatformBridge.cpp` - Display surface creation
- `/Users/wohl/src/iospharo/src/platform/DisplaySurface.hpp` - Surface interface
- `/Users/wohl/src/iospharo/src/vm/Interpreter.cpp` - syncDisplayToSurface(), renderWorldMorphs()
- `/Users/wohl/src/iospharo/src/vm/Primitives.cpp` - primitiveForceDisplayUpdate()

### Official Pharo
- `/Users/wohl/src/pharo/src/Graphics-Display Objects/Form.class.st` - Display Form class
- `/Users/wohl/src/pharo/src/Morphic-Core/WorldMorph.class.st` - World initialization
- `/Users/wohl/src/pharo/src/Morphic-Core/WorldState.class.st` - Renderer management
- `/Users/wohl/src/pharo/src/Morphic-Core/AbstractWorldRenderer.class.st` - Renderer protocol
- `/Users/wohl/src/pharo/src/OSWindow-Core/OSWorldRenderer.class.st` - Display creation
