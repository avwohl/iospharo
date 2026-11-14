# SDL3 library

The image package pharo-graphics-pharosdl3 (PharoSDL3) calls a real SDL3
shared library through FFI.  This document tells where the VM gets the
library, where the library goes, and what is supported on each platform.

## What is staged

- Version: SDL 3.4.10.  The stock Cog VM bundle has the same version
  (library current version 401.10.0), so one package gives the same
  result on both VMs.
- Source: the release tarball from github.com, with a SHA-256 hash pinned in
  `scripts/build-sdl3.sh`.  The hash was checked against the same file on
  libsdl.org.
- Licence: zlib.  The text is in `THIRD_PARTY_LICENSES` (item 15).
- This library is not the SDL2 static slice.  The SDL2 slice is linked into
  the VM.  It gives headers and a symbol table only.  `src/vm/FFI.cpp` serves
  the SDL2 entry points with stubs that draw through Metal.  SDL3 is a loose
  shared library that the image loads with dlopen.

## Build and staging (macOS)

Run once:

```
scripts/build-sdl3.sh
```

The script downloads the tarball, checks the hash, and builds
`libSDL3.0.dylib` for arm64 and for x86_64.  The files go to
`third-party-build/sdl3-install/macos-<arch>/lib/`.  The build takes about
one minute for both arches.  Run `cmake` again after the script, then
build: CMake looks for the library at configure time.

`CMakeLists.txt` copies the library for the build arch to
`<build dir>/Plugins/libSDL3.0.dylib` and makes a `libSDL3.dylib` link beside
the copy.  Both `build-rel` (arm64) and `build-x86` (x86_64) get the correct
arch.  If the library was not built, CMake prints one status line and the
build continues.

## Why Plugins

`FFIMacLibraryFinder` in the image looks for `libSDL3.dylib` and then
`libSDL3.0.dylib` in these places, in this order: the image directory,
`<Smalltalk vm directory>/Plugins`, `<vm directory>/../Resources/lib`,
`/opt/homebrew/lib`, `/usr/lib`, `/usr/local/lib`.  The VM answers
`Smalltalk vm directory` with the directory of the executable.  `Plugins` is
the stock Cog layout, so the same package works on Cog and on this VM
without a library beside the image.

The image finder does not list the executable directory itself.  The VM FFI
search (`getLibSearchPaths` in `src/vm/FFI.cpp`) does search the executable
directory, but a library there does not help the image finder.

## App bundle (Mac Catalyst)

Use `scripts/build-sdl3.sh --stage DIR` to make one universal
(arm64 and x86_64) library with the link in DIR.  For a macOS or Mac Catalyst
bundle, put the files in `Contents/MacOS/Plugins` (if the VM directory of the
app is `Contents/MacOS`) or in `Contents/Resources/lib`.  Both places are on
the image finder list.  This layout was not tested in an app bundle.

## iOS

SDL3 is not supported on iOS devices and the iOS simulator.  The app draws
through Metal, and the SDL2 stubs in `FFI.cpp` give the image a fake window.
The PharoSDL3 package needs a real SDL3 window.  An iOS app can load only
libraries that are inside its own bundle, and the VM has no SDL3 there.  The
package tests error with "Cannot locate" on iOS.  That error is the correct
result.  No iOS run was made.

## Headless runs

The tests open real windows (SDL3WindowTest creates, hides, shows and
destroys windows).  On a logged-in Mac the windows appear for a short time and
the run ends normally.  The runs in the results below completed with exit
code 0 and no hang.  Without a window server (for example a plain ssh session
of a user who is not logged in) SDL_Init for video fails and the window tests
error.  This case was not tested.

## Results

Package row pharo-graphics-pharosdl3, class prefix SDL3-Tests, 12 classes,
110 tests.  No library beside the image.

- arm64 `build-rel`, two runs: pass 110, fail 0, error 0.
- x86_64 `build-x86` under Rosetta, two runs: pass 110, fail 0, error 0.
- Stock Cog x86_64, two runs: pass 110, fail 0, error 0.
- arm64 VM with no `Plugins` directory: pass 7, fail 1, error 133 (every
  error is "Cannot locate any of #('libSDL3.dylib' 'libSDL3.0.dylib')").

Two tests, test11Fullscreen and test19Occluded, failed on both VMs in an
earlier run that used another SDL3 build.  They pass in all runs above, on
both VMs.  The cause of the earlier failures was not investigated.
