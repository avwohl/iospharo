# SDL2 stub layer

The VM has no SDL2 library at run time.  `src/vm/FFI.cpp` answers every
SDL2 call that the Pharo image makes (OSWindow-SDL2, the SDL2 class, and the
pharo-sdl-experiments package).  `src/vm/SDL2Raster.hpp` holds the pixel
code that needs no window state.  This page describes what the layer models
and which rules it follows.

## Rules

- A call follows the SDL 2.0.x rule for the same call: the same return
  value, the same output structure, the same error text style.
- A symbol without a stub does not resolve.  `ffi::lookupFunction` returns
  null, the same result as a real library that lacks the symbol, and the
  image raises its own error.  The older fallback answered 0 for every
  unknown `SDL_` symbol.  That fallback hid missing stubs: a test passed or
  failed on a value that no code had computed.
- `SDL_GetError` answers the message of the last failed call, and an empty
  string before the first failure.
- The layer supports the 32-bit packed pixel formats only: ARGB8888,
  XRGB8888 (named RGB888 in SDL), ABGR8888, XBGR8888, RGBA8888, RGBX8888,
  BGRA8888 and BGRX8888.  A call with another format fails with an error.

## What the layer models

- Windows: the layer keeps size, position, title and flags.  `SDL_WINDOWPOS_CENTERED`
  and `SDL_WINDOWPOS_UNDEFINED` resolve at create time and at `SDL_SetWindowPosition`.
  Minimize needs a visible window and maximize needs a resizable window, as in SDL.
  `SDL_SetWindowHitTest` fails with -1 and the SDL message "That operation is not
  supported", the answer of a platform without window drag support.
- Renderers: each renderer draws into a software target, which is the texture
  named by `SDL_SetRenderTarget` or else the window.  `SDL_RenderReadPixels`
  reads that same target.  Each renderer keeps its draw colour, draw blend mode,
  clip rectangle and vsync flag.
- Textures: each texture keeps its format, access, blend mode, colour and
  alpha modulation, and scale mode.  `SDL_LockTexture` with a rectangle answers a pointer to the
  first pixel of the rectangle.  A lock of a texture that is not streaming fails.
- Blend modes: the five named modes and every custom mode compose from the six
  fields.  The arithmetic is integer and truncates, as in the SDL software
  renderer.  `SDL_ComposeCustomBlendMode` answers the name of a mode when the
  fields equal that mode.
- Draw calls: `SDL_RenderClear`, `SDL_RenderDrawPoint`, `SDL_RenderDrawLine`,
  `SDL_RenderDrawRect`, `SDL_RenderFillRect`, `SDL_RenderFillRectF`,
  `SDL_RenderCopy`, `SDL_RenderCopyF`, `SDL_RenderGeometry` and
  `SDL_RenderGeometryRaw`.  A scaled copy samples the nearest pixel, or
  interpolates bilinear when the texture scale mode is linear (`SDL_RENDER_SCALE_QUALITY`
  hint at create time, or `SDL_SetTextureScaleMode`).
- Surfaces: `SDL_Surface` and `SDL_PixelFormat` are real structures in C memory
  with the SDL 2.0.x layout, because the image reads their fields.  A surface
  made with `SDL_CreateRGBSurface*` owns a zeroed buffer.  `SDL_CreateRGBSurfaceFrom`
  wraps the buffer of the caller.  `SDL_ConvertSurfaceFormat`, `SDL_LockSurface`
  and `SDL_FreeSurface` work on these surfaces.
- Window surface: for the display window, `SDL_GetWindowSurface` answers a
  surface that wraps the display surface, and `SDL_UpdateWindowSurface` updates
  the display.  Another window gets a private surface.
- Hints: `SDL_SetHint` stores the value, `SDL_GetHint` answers the value or null.
  The environment override that SDL reads is not modelled.
- Display: one display with one mode, the size of the display surface at 60 Hz,
  and a logical resolution of 96 dots per inch.
- Devices: no joystick, game controller or audio device exists.  The counts are
  zero and each open call fails with a message.

## The display renderer

The first renderer of the app, while a display surface exists, keeps the path
that the app depends on.  The image locks the streaming texture, and the lock
answers the display surface bytes.  `SDL_RenderCopy` of that texture records the
texture only.  `SDL_RenderPresent` shows the display surface.

`SDL_RenderClear` on this renderer does nothing.  The copy at present replaces
every pixel.  A clear on the shared surface would show a black frame to the
Metal thread.  Every other draw call of this renderer draws into the display
surface.  A renderer without a display surface (a hidden window, a headless
run) draws into a buffer of the window size.

## Not implemented

These symbols stay unresolved, and the image raises `SymbolNotFoundError`:

- `SDL_AddEventWatch` and `SDL_DelEventWatch`: the filter is a Smalltalk callback.
  Only the SceneResizingDemo example calls these.
- `SDL_CreateThread`: no caller in the Pharo 13 or Pharo 14 images.

`SDL_RenderCopyEx` (rotation and flip) and `SDL_RenderSetLogicalSize` are also
absent.

## Check by comparison with Cog

The image can run one expression list on Cog (real SDL2, software renderer)
and on this VM.  Two such lists covered windows, fills, blends, lines, clip,
render targets, scaled copies, geometry, `SDL_RenderReadPixels`, surfaces and
hints.  The two VMs gave the same results.  One difference remains: Cog finds
the audio device of the Mac, and this VM has no audio device.

## What to check on a device

The app GUI cannot be built on a machine without Xcode.  Check these in the
running app:

- The Pharo world draws, and a drag of a window leaves no black frame.
- A resize of the app view resizes the world (display renderer output size).
- Keyboard and menu events arrive as before.  The change does not touch `SDL_PollEvent`.
- A second OSWindow (the debugger, a Spec window) draws into its own buffer
  and does not change the display.
