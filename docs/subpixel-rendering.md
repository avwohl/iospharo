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


## The right fix

The current solution works but is a band-aid.  The real fix is to
eliminate the timing gap entirely.  Options, from best to most pragmatic:

**Option A: Set from C++ before the image runs.**
In `PlatformBridge.cpp`, after `vm_init()` but before `vm_run()`, find
the `FreeTypeSettings` singleton in the image and set its
`bitBltSubPixelAvailable` instance variable to false directly.  This
requires knowing the class layout (fragile across image versions) but
guarantees the setting is applied before any Smalltalk code executes.

**Option B: Defer the render loop.**
Patch the image's `MorphicRenderLoop` startup to wait until
`StartupPreferencesLoader` has finished.  This could be done in
startup.st by having the render loop check a flag, but startup.st
loads after the render loop starts — a chicken-and-egg problem.

**Option C: Register a higher-priority startup handler.**
Pharo's `SessionManager` runs handlers in priority order.  If we could
register a handler that runs before MorphicRenderLoop's handler and sets
`bitBltSubPixelAvailable := false`, the gap would be eliminated.  But
this requires image-side changes (defeating the "standard image" goal).


## Files involved

    src/vm/Primitives.cpp               primitiveCopyBits — returns Failure for argCount > 1
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
