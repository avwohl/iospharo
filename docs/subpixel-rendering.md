# Sub-Pixel Text Rendering

## The problem

Pharo's FreeType text renderer has a sub-pixel anti-aliasing mode
(`FreeTypeSubPixelAntiAliasedGlyphRenderer`) that calls
`copyBitsColor:alpha:gammaTable:ungammaTable:` — a four-argument variant
of `primitiveCopyBits` (primitive 96).  Our VM does not implement this
extension.  The primitive returns `PrimitiveFailed`, which the image
handles gracefully by falling back to standard (non-sub-pixel) rendering.

The image controls whether to use the sub-pixel path via:

    FreeTypeSettings current bitBltSubPixelAvailable

When false, the sub-pixel renderer is never instantiated, and the
four-argument primitive call never happens.  Our startup patches set
this to false.


## The timing gap

The startup patches are loaded by Pharo's `StartupPreferencesLoader`,
which runs during `PharoCommandLineHandler>>activate`.  But the
MorphicRenderLoop can start BEFORE this point in the session startup
sequence:

    Image resumes from snapshot
        SessionManager>>startUp: runs handlers
            ... various subsystems initialize ...
            MorphicRenderLoop starts          <-- render loop running
            ...
        PharoCommandLineHandler>>activate
            StartupPreferencesLoader loads startup.st
                bitBltSubPixelAvailable := false   <-- too late

During this gap (~100ms on fast hardware, longer on slower devices),
the render loop uses the default `bitBltSubPixelAvailable = true` and
tries sub-pixel rendering.  The primitive fails.  Pharo's standard
`fullDrawOn:` catches the error and marks the affected morphs with
`#errorOnDraw` (red error boxes).

Pharo 14 is worse than Pharo 13 here because P14 starts the render
loop earlier in its startup sequence.


## Current solution

Three layers work together:

1. **Dispatcher early-set** (`startup.st`):
   The dispatcher sets `bitBltSubPixelAvailable := false` before doing
   any `fileIn` calls.  This minimizes the window — only bytecodes
   between image resume and StartupPreferencesLoader loading remain
   exposed.

2. **Primitive returns Failure** (`Primitives.cpp`):
   `primitiveCopyBits` returns `PrimitiveFailed` for `argCount > 1`.
   This is the correct spec-compliant behavior — we do not implement
   the sub-pixel extension.  The image catches PrimitiveFailed and
   falls back to non-sub-pixel rendering.

3. **Cleanup fork** (`startup-13.st` / `startup-14.st`):
   A forked process waits 800ms, then:
   - Clears all FreeType font caches (forces re-creation of glyph
     renderers, which now pick up `bitBltSubPixelAvailable = false`)
   - Removes `#errorOnDraw` and `#drawError` properties from all morphs
   - Forces a world redraw cycle

   After cleanup, sub-pixel rendering is fully disabled and errors
   do not recur.


## Why not strip args and succeed?

Build 106 tried a different approach: make `primitiveCopyBits` strip the
extra sub-pixel arguments and perform a regular copyBits with rule 41
(rgbComponentAlpha).  The primitive succeeded, but the Smalltalk code
above it (`FreeTypeSubPixelAntiAliasedGlyphRenderer>>filter:...`) still
expected sub-pixel data structures and hit `nil doesNotUnderstand:`
errors (`#rounded` on P13, `#>` on P14).  This was worse than returning
PrimitiveFailed because the image couldn't fall back gracefully.


## Option A: C++ pre-set (implemented, build 108)

The timing gap is now eliminated by setting `bitBltSubPixelAvailable`
directly in the heap from C++, before any Smalltalk code runs.

`disableSubPixelRendering()` in `PlatformBridge.cpp` (and identically in
`test_load_image.cpp`) does this after `vm_init()` but before `vm_run()`:

1. Find the `FreeTypeSettings` class via `findGlobal("FreeTypeSettings")`.
2. Scan the class object's slots for a `FreeTypeSettings` instance (the
   `current` singleton, stored as a class instance variable).
3. Navigate the class's `FixedLayout` (slot 3) → `LayoutClassScope` (slot 1)
   to find the `bitBltSubPixelAvailable` Slot object by name.
4. Compute the instance variable index (scope position - 1, since
   LayoutClassScope slot 0 is the parentScope).
5. Store `false` at that index in the singleton.

This works for both Pharo 13 (where the value is `nil` in fresh images)
and Pharo 14 (where the value is `true`, baked in from when the image
was saved on a desktop VM that supports sub-pixel rendering).

If the singleton doesn't exist (hypothetical truly-fresh image where
`FreeTypeSettings current` was never called), the function logs a message
and returns — startup.st handles that case.


## Fallback layers

Even with Option A, the other layers remain as defense-in-depth:

1. **C++ pre-set** (`PlatformBridge.cpp` / `test_load_image.cpp`):
   Sets `bitBltSubPixelAvailable := false` before any Smalltalk runs.

2. **Dispatcher early-set** (`startup.st`):
   Sets `bitBltSubPixelAvailable := false` via `instVarNamed:put:` before
   doing any `fileIn` calls.  Catches edge cases the C++ code might miss.

3. **Primitive returns Failure** (`Primitives.cpp`):
   `primitiveCopyBits` returns `PrimitiveFailed` for `argCount > 1`.
   Correct spec-compliant behavior — the image falls back gracefully.

4. **Cleanup fork** (`startup-13.st` / `startup-14.st`):
   A forked process waits 800ms, clears FreeType caches and error marks,
   forces a world redraw.  Handles any residual artifacts.


## Pharo class layout details

In Pharo 13+, class slot 3 is a `FixedLayout` object (not an Array of
Symbols as in older Squeak).  The layout chain is:

    Class slot 3 → FixedLayout
    FixedLayout slot 1 → LayoutClassScope
    LayoutClassScope slot 0 → parentScope
    LayoutClassScope slots 1..N → Slot objects (one per instance variable)
    Slot slot 0 → name (ByteSymbol)

Instance variable index = scope position - 1 (because slot 0 is parentScope).

The `current` singleton is a class instance variable, NOT a class variable
(not in classPool).  It's stored as a named slot in the class object itself,
after the standard Class layout.  Finding it by scanning for a FreeTypeSettings
instance is more robust than computing the exact slot offset.


## Files involved

    src/platform/PlatformBridge.cpp      disableSubPixelRendering() — C++ pre-set
    src/vm/test_load_image.cpp           same logic for headless testing
    src/vm/Primitives.cpp                primitiveCopyBits — returns Failure for argCount > 1
    iospharo/Bridge/PharoBridge.swift    writeStartupScript — generates startup.st dispatcher
                                         and startup-{13,14}.st with cleanup fork
    startup.st (generated)               Sets bitBltSubPixelAvailable := false early
    startup-13.st / startup-14.st        Cleanup fork clears transient error marks


## History

    Build 106  Made primitiveCopyBits strip extra args and succeed.
               Caused nil#rounded (P13) and nil#> (P14) crashes because
               FreeTypeSubPixelAntiAliasedGlyphRenderer expects data
               structures we don't provide.

    Build 107  Reverted to PrimitiveFailed.  Added early-set in dispatcher
               and cleanup fork to handle the timing gap.

    Build 108  Implemented Option A: C++ pre-set of bitBltSubPixelAvailable
               before any Smalltalk code runs.  Eliminates timing gap entirely.
               Verified on fresh P13 (was nil → false) and P14 (was true → false).
