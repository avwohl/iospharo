# Plan: Pharo Smalltalk on Desktop Linux

The official Pharo VM already runs on Linux. So why port OUR VM? Because our
VM is a clean, readable C++ implementation (~25K lines vs the generated C
spaghetti of the Cog VM), and a Linux build would be the easiest path to a
portable, cross-platform Pharo VM. Linux is also the natural stepping stone
to Android, WebAssembly, and bare metal (see companion docs).


## What Works Unchanged on Linux

Our VM is fundamentally POSIX. Almost everything "just works":

    Component               File(s)                         Linux status
    ----------------------- ------------------------------- ----------------
    Heap (mmap/munmap)      ObjectMemory.cpp                Works as-is
    Image load/save         ImageLoader.cpp, ImageWriter.cpp Works as-is
    Interpreter             Interpreter.cpp                 99% (remove CFRunLoop)
    Primitives (file I/O)   Primitives.cpp                  Works as-is
    Primitives (sockets)    SocketPlugin.cpp                Works as-is
    Primitives (clock)      InterpreterProxy.cpp            Works as-is
    Primitives (dirs)       Primitives.cpp                  Works as-is
    FFI (dlopen/dlsym)      FFI.cpp                         Works as-is
    Threading               std::thread/mutex/atomic        Works as-is
    B2D plugin              B2DPlugin.c                     Works as-is
    JPEG plugin             sqJPEGReadWriter2Plugin.c       Works as-is
    DSA plugin              DSAPrims.c                      Works as-is
    Locale primitives       Primitives.cpp                  Has Linux fallback
    File birthtime          Primitives.cpp                  Uses st_ctime

That's roughly 95% of our C++ code. The only Apple-specific parts are
rendering, sound, MIDI, text rendering, and the app shell.


## What Needs Replacing

### 1. Display Rendering (Metal -> SDL2 or X11/Wayland)

Currently: Metal via MetalRenderer.swift, exposed through DisplaySurface.

The natural replacement is REAL SDL2. Here's why this is elegant:

  - The Pharo image already uses OSSDL2Driver as its display driver
  - OSSDL2Driver calls SDL2 functions via FFI
  - Our iOS VM intercepts these FFI calls with C++ stubs (FFI.cpp)
  - On Linux, we can link REAL SDL2 and let the FFI calls go through

This means: the Pharo image drives the display through OSSDL2Driver -> SDL2
FFI calls -> real SDL2 library -> X11/Wayland. No stubs, no Metal, no
custom rendering code. The image handles display composition (BitBlt) and
SDL2 handles window management and pixel buffer presentation.

    iOS path:       OSSDL2Driver -> FFI -> our C++ stubs -> Metal
    Linux path:     OSSDL2Driver -> FFI -> real SDL2 -> X11/Wayland

SDL2 handles both X11 and Wayland transparently. It also handles:
  - Window creation and management
  - Keyboard and mouse events (SDL_PollEvent)
  - Clipboard
  - Full-screen toggle
  - Multi-monitor support

What we need to do:
  - Link real SDL2 (apt install libsdl2-dev)
  - Remove our SDL2 stub registration in FFI.cpp (or let dlsym find real SDL2
    first, which it already does -- our stubs are fallbacks)
  - Ensure our FFI correctly passes SDL2 calls through to the real library
  - The SDL2 stubs in FFI.cpp already check for real SDL2 before falling back:

      void* sdlInit = dlsym(RTLD_DEFAULT, "SDL_Init");
      if (sdlInit) {
          // Real SDL2 found -- don't register stubs
      }

So this may already work with zero code changes if SDL2 is installed.

### 2. Text Rendering (CoreText -> FreeType + HarfBuzz)

Currently: WorldRenderer.cpp uses CoreGraphics/CoreText for text rendering
(font loading, glyph layout, bitmap rendering). This is ~100 lines of
Apple-only code inside #ifdef __APPLE__.

Replacement: FreeType + HarfBuzz. We already link these libraries (they're
in our XCFramework dependencies for Cairo support):

    Library     Version     Purpose                 Already in project?
    ----------- ----------- ----------------------- -------------------
    FreeType    2.13.3      Font loading, rasterize Yes (xcframework)
    HarfBuzz    10.1.0      Text shaping/layout     Yes (xcframework)
    Pixman      0.43.4      Pixel manipulation       Yes (xcframework)
    Cairo       1.18.2      2D rendering            Yes (xcframework)
    libpng      1.6.43      PNG support             Yes (xcframework)

On Linux, install these as system packages instead of bundled xcframeworks:

    apt install libfreetype-dev libharfbuzz-dev libcairo2-dev libpixman-1-dev

The WorldRenderer text path becomes:

    FT_Library ftLib;
    FT_Init_FreeType(&ftLib);
    FT_Face face;
    FT_New_Face(ftLib, "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 0, &face);
    FT_Set_Pixel_Sizes(face, 0, fontSize);
    // ... render glyphs to bitmap with FT_Render_Glyph

Or use Cairo (which wraps FreeType/HarfBuzz) for a higher-level API:

    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t* cr = cairo_create(surface);
    cairo_select_font_face(cr, "DejaVu Sans", ...);
    cairo_set_font_size(cr, fontSize);
    cairo_show_text(cr, text);

### 3. Sound (AudioToolbox -> ALSA/PulseAudio/PipeWire)

Currently: SoundPlugin.cpp uses Apple AudioQueue Services (~160 lines,
entirely inside #ifdef __APPLE__).

Our sound architecture (ring buffer, callback-driven) maps directly to
Linux audio APIs:

    iOS (current)                   Linux replacement
    ------------------------------- ----------------------------
    AudioQueueNewOutput()           snd_pcm_open() (ALSA)
    AudioQueueEnqueueBuffer()       snd_pcm_writei()
    AudioQueue callback             ALSA period callback or thread
    AudioQueueStart/Stop            snd_pcm_prepare/drain

Options ranked by simplicity:

    API         Complexity   Coverage                    Recommendation
    ----------- ------------ --------------------------- ---------------
    SDL2_mixer  Very low     Uses whatever backend works  Best for prototype
    PulseAudio  Low          Most desktop Linux distros   Good default
    PipeWire    Low          Modern distros (Fedora, etc) Future-proof
    ALSA        Medium       Universal, low-level         Maximum compat
    OpenAL      Medium       Cross-platform               Overkill

Recommendation: Use SDL2_audio (SDL_OpenAudioDevice). Since we're already
linking SDL2 for display, using SDL2 for audio is zero additional
dependencies. The ring buffer architecture stays the same:

    SDL_AudioSpec spec;
    spec.freq = 44100;
    spec.format = AUDIO_S16SYS;
    spec.channels = 2;
    spec.samples = 1024;
    spec.callback = audioCallback;  // reads from our ring buffer
    SDL_OpenAudioDevice(NULL, 0, &spec, &obtained, 0);

### 4. MIDI (CoreMIDI -> ALSA MIDI)

Currently: MIDIPlugin.cpp uses CoreMIDI (~150 lines, #ifdef __APPLE__).

Linux replacement: ALSA sequencer API (libasound):

    snd_seq_t* seq;
    snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, 0);
    snd_seq_set_client_name(seq, "Pharo");

Or stub it out. MIDI is rarely used in Pharo development.

### 5. Crypto/SSL (Security.framework -> OpenSSL)

Currently: sqMacSSL.c uses Apple Security.framework for SSL/TLS.

We already link OpenSSL (3.4.0) in our xcframeworks. On Linux, use the
system OpenSSL or our bundled one. There's already a SqueakSSL.c that
uses OpenSSL -- we just need to compile that instead of sqMacSSL.c.

### 6. App Shell (SwiftUI -> Command Line or GTK)

Currently: 13 Swift files (~3,573 lines) provide the app UI:
  - PharoBridge.swift (VM lifecycle, clipboard, text input)
  - MetalRenderer.swift (Metal rendering)
  - ImageManager.swift (image library management)
  - PharoCanvasView.swift (touch/keyboard events)
  - ImageLibraryView.swift (file browser)
  - iosparoApp.swift (app entry point)

For Linux, we don't need most of this. The simplest approach:

**Option A: Headless + SDL2 window (recommended for prototype)**

    int main(int argc, char* argv[]) {
        // Parse --image flag
        // Initialize ObjectMemory
        // Load image via ImageLoader
        // SDL2 creates window, handles events
        // Run interpreter loop
    }

This is essentially what test_load_image.cpp already does, minus the
SDL2 window management. We already have a working command-line launcher.

**Option B: GTK4 app with image library**

A GTK4 app wrapper that provides:
  - Image library (pick which .image to open)
  - SDL2 embedded in a GTK widget (GtkDrawingArea or embedded window)
  - Menu bar, preferences

This would mirror the iOS app experience but is optional. The SDL2-only
path gets us running immediately.

### 7. Platform Bridge (remove Apple, add Linux)

Currently: PlatformBridge.cpp has AppKit swizzling code (Objective-C runtime
calls to replace NSApplication/NSMenu methods). This is Mac Catalyst-specific
and not needed on Linux.

    What to remove:
      - All objc_getClass / class_replaceMethod / method_setImplementation code
      - #include <objc/runtime.h> and <objc/message.h>
      - The ObjCExceptionGuard.m file entirely

    What to keep:
      - vm_getDisplayPixels() -- works as-is (returns pixel buffer pointer)
      - vm_setDisplaySize() -- works as-is
      - vm_postMouseEvent() / vm_postKeyEvent() -- works as-is
      - EventQueue -- works as-is

### 8. Interpreter.cpp (remove CFRunLoop)

One small change. Lines 721-732:

    #if __APPLE__
        // Pump the native run loop periodically so Metal can render and
        // UIKit can deliver events.
        if (++runLoopCounter >= runLoopInterval) {
            runLoopCounter = 0;
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true);
        }
    #endif

On Linux, this block is already skipped (it's inside #if __APPLE__).
No change needed -- the #ifdef handles it.


## Build System

Our CMakeLists.txt currently links Apple frameworks. For Linux:

    if(APPLE)
        # ... existing Apple framework linking
    elseif(UNIX AND NOT APPLE)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SDL2 REQUIRED sdl2)
        pkg_check_modules(FREETYPE REQUIRED freetype2)
        pkg_check_modules(HARFBUZZ REQUIRED harfbuzz)
        pkg_check_modules(CAIRO REQUIRED cairo)
        pkg_check_modules(OPENSSL REQUIRED openssl)

        target_link_libraries(PharoVMCore
            ${SDL2_LIBRARIES}
            ${FREETYPE_LIBRARIES}
            ${HARFBUZZ_LIBRARIES}
            ${CAIRO_LIBRARIES}
            ${OPENSSL_LIBRARIES}
            pthread dl m
        )
        target_include_directories(PharoVMCore PRIVATE
            ${SDL2_INCLUDE_DIRS}
            ${FREETYPE_INCLUDE_DIRS}
        )
    endif()

Build:

    # Install dependencies (Debian/Ubuntu)
    sudo apt install build-essential cmake pkg-config \
        libsdl2-dev libfreetype-dev libharfbuzz-dev \
        libcairo2-dev libffi-dev libssl-dev

    # Build
    cmake -B build-linux -DCMAKE_BUILD_TYPE=Release
    cmake --build build-linux

    # Run
    ./build-linux/pharo-vm /path/to/Pharo.image


## The SDL2 Strategy: Why It's Almost Free

The key insight is that our VM's FFI layer already bridges SDL2 calls.
The Pharo image's OSSDL2Driver makes these calls:

    SDL_Init, SDL_CreateWindow, SDL_CreateRenderer
    SDL_CreateTexture, SDL_UpdateTexture, SDL_RenderPresent
    SDL_PollEvent, SDL_GetWindowSize, SDL_SetWindowTitle
    SDL_free, SDL_GetClipboardText, SDL_SetClipboardText

On iOS, our FFI.cpp intercepts these and routes them to Metal. On Linux,
these calls would go to the REAL SDL2 library. The display driver in the
Pharo image doesn't change at all.

Our FFI.cpp already has this logic (line 200):

    void* sdlInit = dlsym(RTLD_DEFAULT, "SDL_Init");
    if (sdlInit) {
        fprintf(stderr, "[FFI] Real SDL2 found — not registering stubs\n");
        return;
    }
    // ... register stubs only if real SDL2 is not found

So if we link real SDL2, the stubs are automatically bypassed. This is
the zero-code-change path.

What might need attention:
  - SDL2 event loop integration with our interpreter loop
  - Our heartbeat thread currently drives display updates via timer
    semaphore; SDL2 events need to be pumped from the main thread
  - test_load_image.cpp may need SDL_Init() and event loop integration


## Headless Server Mode

Linux is the natural platform for headless Pharo (no display). This is
already partially working:

    ./build/test_load_image /path/to/Pharo.image

Our test harness loads and runs the image without any display. For a
proper headless mode:

  - Skip SDL2 initialization
  - Return a null/dummy DisplaySurface
  - Disable sound and MIDI plugins
  - Keep sockets, file I/O, and interpreter running
  - Useful for: Pharo web servers, CI/CD, batch processing, image
    preparation scripts


## JIT Compilation on Linux

Like Android, Linux allows mprotect(PROT_EXEC). We could:

1. Use our interpreter (works today, 3-15x slower than Cog)
2. Use the official Cog VM (already runs on Linux -- but then why port?)
3. Add JIT to our VM (multi-year project)

The practical answer: for Linux desktop use, the official Cog VM is already
excellent. Our VM's value on Linux is as:
  - A readable reference implementation
  - A testbed for our iOS oop encoding experiments
  - A stepping stone to ports where Cog doesn't run (WASM, bare metal)
  - A base for adding JIT later with a clean architecture


## Raspberry Pi / ARM Linux

Our C++ code is architecture-independent (no x86 assembly, no inline asm).
It should compile on ARM64 Linux (Raspberry Pi 4/5, Pine64, etc.) with:

    # On the Pi itself, or cross-compile with aarch64 toolchain
    cmake -B build-arm64 -DCMAKE_BUILD_TYPE=Release
    cmake --build build-arm64

The Raspberry Pi is interesting because:
  - It has a real GPU (VideoCore / Vulkan) for rendering
  - It runs full desktop Linux (Raspberry Pi OS)
  - It's cheap enough for education (Smalltalk in schools)
  - ARM64 is the same architecture as iOS/Android (test platform)


## Estimated Effort

    Task                                            Time
    ----------------------------------------------- ----------
    CMake Linux build (remove Apple, add SDL2/etc)  1-2 days
    Verify real SDL2 passthrough works              1-2 days
    Text rendering (FreeType/Cairo path)            2-3 days
    Sound (SDL2_audio)                              1-2 days
    Headless mode (null display)                    1 day
    test_load_image with SDL2 event loop            1-2 days
    Testing on Ubuntu + Fedora                      2-3 days
    ----------------------------------------------- ----------
    TOTAL for working prototype                     ~1.5-2 weeks

    Polish (packaging, Wayland, HiDPI, Raspberry Pi): +2-4 weeks

This is the shortest port of any platform because:
  1. All POSIX code works unchanged
  2. SDL2 passthrough may work with zero code changes
  3. FreeType/HarfBuzz/Cairo are already in our dependency list
  4. No new platform bridge needed (SDL2 handles events and display)


## Distribution

    Format          Tool                    Notes
    --------------- ----------------------- ----------------------------
    AppImage        linuxdeploy             Single file, runs anywhere
    Flatpak         flatpak-builder         Sandboxed, auto-updates
    Snap            snapcraft               Ubuntu-focused
    .deb            dpkg-deb                Debian/Ubuntu native
    .rpm            rpmbuild                Fedora/RHEL native
    Tarball         tar czf                 Universal, manual install
    Nix             nix-build               Reproducible builds

Recommendation: Start with a tarball (./pharo-vm Pharo.image), then
package as AppImage for broad compatibility.


## File-by-File Change Summary

    File                        Change needed           Effort
    --------------------------- ----------------------- --------
    ObjectMemory.cpp            None                    -
    ImageLoader.cpp             None                    -
    ImageWriter.cpp             None                    -
    Interpreter.cpp             None (#ifdef handles)   -
    Primitives.cpp              None (Linux fallbacks)  -
    FFI.cpp                     None (real SDL2 found)  -
    InterpreterProxy.cpp        None                    -
    SocketPlugin.cpp            None                    -
    B2DPlugin.c                 None                    -
    DSAPrims.c                  None                    -
    JPEG plugin                 None                    -
    WorldRenderer.cpp           Add FreeType/Cairo path Small
    SoundPlugin.cpp             Add SDL2_audio path     Small
    MIDIPlugin.cpp              Add ALSA path or stub   Small
    sqMacSSL.c                  Use SqueakSSL.c instead Trivial
    PlatformBridge.cpp          Remove ObjC swizzling   Trivial
    ObjCExceptionGuard.m        Exclude from build      Trivial
    iOSBridge.cpp               Exclude from build      Trivial
    CMakeLists.txt              Add Linux target        Medium
    test_load_image.cpp         Add SDL2 event loop     Small
    New: linux_main.cpp         App entry point         Small


## Architecture Diagram

    +--------------------------------------------------+
    | Pharo Image                                       |
    |   OSSDL2Driver -> SDL2 FFI calls                  |
    |   BitBlt, Morphic, ProcessScheduler               |
    +--------------------------------------------------+
              |  FFI calls (SDL_CreateWindow, etc.)
              v
    +--------------------------------------------------+
    | PharoVMCore (C++ shared library)                  |
    |   ObjectMemory  - mmap (Linux native)             |
    |   Interpreter   - bytecode loop                   |
    |   Primitives    - file, sockets, clock            |
    |   FFI           - dlsym finds real SDL2           |
    |   Plugins       - B2D, JPEG, DSA, Sound, etc.    |
    +--------------------------------------------------+
              |  dlsym -> real SDL2
              v
    +--------------------------------------------------+
    | SDL2 (system library)                             |
    |   Window management                               |
    |   Keyboard/mouse events                           |
    |   Audio output (SDL_audio)                        |
    |   Clipboard                                       |
    +--------------------------------------------------+
              |
              v
    +--------------------------------------------------+
    | Linux                                             |
    |   X11 or Wayland (display)                        |
    |   ALSA/PulseAudio/PipeWire (audio)                |
    |   evdev (input)                                   |
    +--------------------------------------------------+


## Comparison with Other Platforms

    Platform    POSIX code   Display path         JIT    New code   Time
    ----------- ------------ -------------------- ------ ---------- --------
    Linux       100% works   Real SDL2 (free!)    Yes    ~500 LOC   1.5-2 wk
    Android     100% works   ANativeWindow/GLES   Yes    ~1,300 LOC 3-4 wk
    WebAssembly ~80% works   Canvas/Emscripten    No     ~3,000 LOC 3-5 wk
    Bare metal  ~60% works   Custom framebuffer   Yes    ~5,000 LOC 2-3 mo

Linux is the lowest-effort port. Do this first, then use it as the
portable base for Android and WebAssembly.
