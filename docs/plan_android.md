# Plan: Pharo Smalltalk on Android

Our iospharo VM runs on iOS via Mac Catalyst. This document examines porting
it to Android -- the other major mobile platform. The good news: Android is
Linux under the hood, and unlike iOS, Android ALLOWS JIT compilation.


## Why Android is Easier Than iOS

    Constraint              iOS                 Android
    ----------------------- ------------------- -----------------------
    JIT (runtime codegen)   FORBIDDEN           Allowed (mprotect works)
    mmap large virtual      Works               Works (it's Linux)
    dlopen/dlsym            Restricted           Works normally
    File system access      Sandboxed            Sandboxed but more open
    Native code             Xcode + frameworks   NDK + CMake (we use CMake)
    Build system            Xcode project        Gradle + CMake (easy)
    Distribution            App Store only       Play Store, sideload, F-Droid
    Memory per app          ~1-2 GB              ~256 MB - 1 GB (varies)

Android is Linux. Our POSIX code (mmap, fopen, sockets, pthreads) works
unchanged. The main work is replacing Apple-specific code (Metal, UIKit,
CoreText, AudioToolbox) with Android equivalents.


## Architecture: iOS vs Android

Our iOS architecture:

    SwiftUI app (PharoBridge.swift, MetalRenderer.swift)
      |
      +-- PlatformBridge C API (vm_getDisplayPixels, vm_postMouseEvent, ...)
      |
      +-- PharoVMCore static library (C++ VM: Interpreter, Primitives, etc.)
      |
      +-- Metal rendering (CAMetalLayer, MTKView)
      +-- UIKit events (UITouch, UIKey, UIGestureRecognizer)
      +-- AudioToolbox (AudioQueue for sound)
      +-- CoreText/CoreGraphics (text rendering)

Proposed Android architecture:

    Kotlin/Java app (PharoActivity.kt, PharoSurfaceView.kt)
      |
      +-- JNI bridge (native methods calling C++ VM)
      |
      +-- PharoVMCore shared library (.so, same C++ VM code)
      |
      +-- ANativeWindow rendering (direct pixel buffer access)
         OR OpenGL ES / Vulkan (GPU-accelerated)
      +-- MotionEvent / KeyEvent (touch, keyboard, mouse)
      +-- OpenSL ES or AAudio (sound)
      +-- FreeType + HarfBuzz (text rendering -- already in our deps)


## Display Rendering

### Option A: ANativeWindow (simplest, recommended for prototype)

Android's ANativeWindow gives direct pixel buffer access -- exactly what
Pharo's BitBlt needs. This is the closest equivalent to our Metal approach
of writing directly to a pixel buffer.

    // Lock the surface to get a raw pixel buffer
    ANativeWindow_Buffer buffer;
    ANativeWindow_lock(window, &buffer, NULL);

    // buffer.bits   = raw RGBA pixel pointer (like vm_getDisplayPixels())
    // buffer.width  = pixel width
    // buffer.height = pixel height
    // buffer.stride = pixels per row (may be > width for alignment)

    // Copy Pharo's display buffer to the surface
    memcpy(buffer.bits, pharoDisplayPixels, width * height * 4);

    ANativeWindow_unlockAndPost(window);

This maps cleanly to our existing DisplaySurface abstraction. Implement
AndroidDisplaySurface with ANativeWindow_lock/unlockAndPost.

Pixel format: WINDOW_FORMAT_RGBA_8888 matches our 32-bit ARGB buffer
(may need byte swizzle: Pharo uses BGRA, Android uses RGBA).

### Option B: OpenGL ES texture (better performance)

Upload Pharo's pixel buffer as a texture, draw a full-screen quad. Better
because the compositor doesn't need to touch our pixels:

    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                    GL_RGBA, GL_UNSIGNED_BYTE, pharoDisplayPixels);

This is what SqueakJS does with HTML Canvas, but with OpenGL ES instead.

### Option C: Vulkan (overkill)

Vulkan is available on Android 7+ but the complexity is not justified for
a 2D pixel-buffer blit. Save this for if we ever do GPU-accelerated
rendering of Morphic.


## Input Handling

### Touch Events

Android MotionEvent maps to Pharo mouse events:

    Android event           Pharo event type    Notes
    ----------------------- ------------------- --------------------------
    ACTION_DOWN             mouse down (red)    First finger = left button
    ACTION_MOVE             mouse move          Track primary pointer
    ACTION_UP               mouse up            Release
    ACTION_POINTER_DOWN     (2nd finger)        Map to right-click or Cmd
    Long press              mouse down (yellow) Context menu / right-click

Our iOS code already handles touch-to-mouse mapping in PharoBridge.swift.
The Android equivalent would be similar logic in Kotlin/JNI.

### Keyboard

    Android                 Pharo
    ----------------------- -----------------------------------
    Soft keyboard (IME)     KeyEvent via InputConnection
    Hardware keyboard       KeyEvent directly (Chromebooks, BT)
    Volume keys             Can intercept or let system handle

Android's soft keyboard (IME) is more complex than iOS's. We need an
InputConnection implementation to handle text composition, autocorrect,
and character-by-character input.

### Mouse (Chromebooks, Samsung DeX, USB mice)

Android has full mouse support. MotionEvent with SOURCE_MOUSE provides:
  - Left/right/middle click buttons
  - Mouse wheel scrolling (ACTION_SCROLL)
  - Hover events (ACTION_HOVER_MOVE)

This maps perfectly to Pharo's 3-button mouse model. On Chromebooks and
Samsung DeX, Pharo would work with a real mouse cursor.


## Memory Management

Android is Linux, so mmap works identically to our existing code:

    mmap(nullptr, size, PROT_READ | PROT_WRITE,
         MAP_ANONYMOUS | MAP_PRIVATE, -1, 0)

However, Android's Low Memory Killer (LMK) is aggressive:

    Device class        Typical app limit    Notes
    ------------------- -------------------- -------------------------
    Low-end (1-2 GB)    128-256 MB           LMK kills aggressively
    Mid-range (4 GB)    256-512 MB           Usually fine for Pharo
    High-end (8-16 GB)  512 MB - 1 GB        Plenty for development
    Chromebook           1-2 GB+              Desktop-class memory

A fresh Pharo image needs ~100-150 MB at runtime. Development sessions
may reach 200-400 MB. Our 4 GB mmap virtual reservation is fine (Android/
Linux lazy-commits pages), but actual physical usage must stay reasonable.

Request large heap in AndroidManifest.xml:

    android:largeHeap="true"

This roughly doubles the available memory. Not guaranteed but helps.


## JIT Compilation -- The Big Win

Unlike iOS, Android allows mprotect(PROT_EXEC). This means we could:

1. Add JIT to our interpreter VM (huge effort, multi-year project)
2. Use the official Cog VM instead of our interpreter (recommended)
3. Keep interpreting (works, but 3-15x slower)

SELinux on Android does enforce some restrictions:
  - Writable+executable memory (W^X) is increasingly restricted
  - Android 10+ enforces W^X on app code
  - But: dual mapping works (mmap the same physical pages twice,
    one RW for writing, one RX for executing)
  - The Cog VM already implements dual-mapped code zones

### Performance Impact

    VM mode                     Android performance
    --------------------------- ----------------------
    Our interpreter             1x (same as iOS today)
    Cog VM with JIT             3-15x faster
    Cog VM with Sista           10-45x faster

For Android, using the official Cog VM (which already has ARM64 JIT
support) would give a dramatic performance improvement over our
interpreter-only iOS VM. This alone makes an Android port compelling.


## File System and Storage

    Location                    Access              Use for
    --------------------------- ------------------- -----------------------
    context.filesDir            App-private         Pharo images, changes
    context.cacheDir            App-private, temp   Downloaded images
    getExternalFilesDir()       App-private, SD     Large images, backups
    SAF (Storage Access)        User-granted        Import/export images

Scoped storage (Android 11+) restricts direct file system access. But
app-private directories work fine without permissions. Strategy:

  - Store .image and .changes in context.filesDir (survives updates)
  - Bundle a default image in the APK assets
  - Copy bundled image to filesDir on first launch
  - Use SAF (document picker) for importing/exporting images


## What Needs to Change (File by File)

### Code that works UNCHANGED on Android

    File                        Why it works
    --------------------------- -------------------------------------------
    ObjectMemory.cpp            mmap/munmap are Linux-native
    ImageLoader.cpp             std::ifstream is cross-platform
    ImageWriter.cpp             std::ofstream is cross-platform
    Interpreter.cpp             Pure C++ (remove CFRunLoop, keep rest)
    Primitives.cpp              fopen/fread/stat/sockets all POSIX
    FFI.cpp                     dlopen/dlsym are Linux-native
    InterpreterProxy.cpp        gettimeofday is POSIX
    plugins/SocketPlugin.cpp    POSIX sockets work unchanged
    plugins/B2DPlugin.c         Pure C
    plugins/DSAPrims.c          Pure C
    plugins/jpeg/*              Pure C

### Code that needs Android replacements

    iOS file                    Android replacement           Effort
    --------------------------- ----------------------------- --------
    MetalRenderer.swift         ANativeWindow or OpenGL ES    Medium
    PharoBridge.swift           PharoBridge.kt (Kotlin + JNI) Medium
    iOSBridge.cpp               AndroidBridge.cpp (JNI calls) Medium
    WorldRenderer.cpp           FreeType + HarfBuzz (we have) Medium
    SoundPlugin.cpp             OpenSL ES or AAudio           Small
    MIDIPlugin.cpp              Android MIDI API or stub      Small
    ObjCExceptionGuard.m        Remove (no Obj-C on Android)  Trivial

### New code needed

    File                        Purpose                       Lines est.
    --------------------------- ----------------------------- ----------
    PharoActivity.kt            Main activity, lifecycle      ~200
    PharoSurfaceView.kt         SurfaceView + touch handling  ~300
    AndroidBridge.cpp           JNI bridge to C++ VM          ~400
    AndroidDisplaySurface.cpp   DisplaySurface via ANativeWin ~200
    android/CMakeLists.txt      NDK build integration         ~100
    build.gradle                Gradle + CMake wiring         ~100
    AndroidManifest.xml         Permissions, largeHeap        ~30

Total new code: ~1,300 lines (Kotlin + C++ + build config)


## Build System

Our project already uses CMake. Android NDK uses CMake natively:

    # In android/CMakeLists.txt
    cmake_minimum_required(VERSION 3.18)
    project(pharo-android)

    # Add our existing VM sources
    add_library(pharo-vm SHARED
        ${VM_SOURCES}           # Same .cpp files as iOS
        AndroidBridge.cpp       # New: JNI bridge
        AndroidDisplaySurface.cpp  # New: ANativeWindow impl
    )

    target_link_libraries(pharo-vm
        android        # ANativeWindow, ALooper
        log            # __android_log_print
        OpenSLES       # Sound (or aaudio)
        ffi            # libffi (build from source or use prebuilt)
    )

Gradle integration:

    // app/build.gradle.kts
    android {
        defaultConfig {
            minSdk = 26        // Android 8.0+
            ndk { abiFilters += listOf("arm64-v8a", "x86_64") }
        }
        externalNativeBuild {
            cmake { path = file("src/main/cpp/CMakeLists.txt") }
        }
    }

APK structure:

    app.apk
      lib/arm64-v8a/libpharo-vm.so    (~5-10 MB)
      lib/x86_64/libpharo-vm.so       (~5-10 MB, emulator)
      assets/Pharo.image               (~60 MB, compressed in APK)
      assets/Pharo.changes
      classes.dex                      (Kotlin app code)


## Existing Squeak/Pharo on Android

### Squeak Android VM (Tim Felgentreff, ~2012)

An early port of the Squeak VM to Android. Used JNI + SurfaceView.
99.4% C code. Not maintained. https://github.com/timfel/squeak-android-vm

### ScratchJr

ScratchJr for Android uses a custom Squeak VM. Open source. Demonstrates
that a Smalltalk VM can ship on the Play Store.

### Pharo Launcher

There was briefly a Pharo Launcher for Android concept, but it used a
headless VM communicating via sockets, not a native graphical VM.

### Verdict

No maintained graphical Pharo VM exists for Android today. This would be
the first modern one.


## Comparison: iOS Port Effort vs Android Port Effort

    iOS port (what we built)            Android port (proposed)
    ----------------------------------- -----------------------------------
    Xcode project, Swift, Obj-C         Gradle, Kotlin, JNI
    Metal rendering                     ANativeWindow / OpenGL ES
    UIKit events                        MotionEvent / KeyEvent
    CoreText text rendering             FreeType + HarfBuzz (already have)
    AudioToolbox sound                  OpenSL ES / AAudio
    CoreMIDI                            Android MIDI API
    No JIT (Apple forbids)              JIT possible (big performance win)
    App Store only                      Play Store + sideload + F-Droid
    Mac required for development        Any OS for development

The Android port is LESS work than the iOS port was, because:
  1. Android is Linux -- all POSIX code works unchanged
  2. We already have the platform abstraction (DisplaySurface, EventQueue)
  3. No Obj-C/Swift bridging complexity -- JNI is simpler
  4. No Metal -- ANativeWindow is simpler than Metal
  5. Text rendering: FreeType/HarfBuzz work on Android (CoreText doesn't)
  6. CMake is already our build system


## Estimated Effort

    Task                                            Time
    ----------------------------------------------- ----------
    Gradle + CMake project setup                    1-2 days
    JNI bridge (AndroidBridge.cpp)                  2-3 days
    ANativeWindow display surface                   2-3 days
    Touch/keyboard event handling                   2-3 days
    Image bundling + first-launch copy              1 day
    Sound (OpenSL ES)                               2-3 days
    Text rendering (switch to FreeType/HarfBuzz)    3-5 days
    Testing on devices + emulator                   5-7 days
    ----------------------------------------------- ----------
    TOTAL for working prototype                     ~3-4 weeks

    Polish (image library UI, auto-launch,
      Chromebook mouse, Samsung DeX, Play Store):   +2-3 months


## What We Get That iOS Doesn't Have

1. JIT compilation -- 3-15x performance improvement (use Cog VM or add JIT)
2. Sideloading -- distribute without app store review
3. F-Droid -- open source app distribution
4. Mouse support -- Chromebooks and Samsung DeX get real 3-button mouse
5. File system -- more open access for image management
6. Larger memory -- high-end Android devices have 8-16 GB RAM


## Shared Code Strategy

Most of the C++ VM code is platform-independent. Here's what we share
vs what diverges:

    Layer                   Shared          iOS-specific        Android-specific
    ----------------------- --------------- ------------------- ------------------
    ObjectMemory            100%
    ImageLoader/Writer      100%
    Interpreter             ~98%            CFRunLoop           (nothing)
    Primitives              ~99%            (nothing)           (nothing)
    FFI                     ~95%            ObjC stubs          (nothing)
    SocketPlugin            100%
    B2D/JPEG/DSA plugins    100%
    SoundPlugin             0%              AudioToolbox        OpenSL ES
    MIDIPlugin              0%              CoreMIDI            Android MIDI
    WorldRenderer           ~50%            CoreText            FreeType
    Display surface         Interface only  Metal               ANativeWindow
    Event handling          Interface only  UIKit               MotionEvent
    Platform bridge         Interface only  Swift/PharoBridge   Kotlin/JNI


## Distribution

    Channel             Requirements                    Notes
    ------------------- ------------------------------- ----------------------
    Google Play Store   AAB format, signing, review     Broadest reach
    APK sideload        Enable "unknown sources"        Developer testing
    F-Droid             Open source, reproducible build Open source community
    Amazon App Store    APK, separate listing           Kindle Fire tablets
    Samsung Galaxy      APK/AAB, separate listing       Samsung devices


## Architecture Diagram

    +--------------------------------------------------+
    | PharoActivity.kt (Kotlin)                         |
    |   Lifecycle management, permissions, file access  |
    +--------------------------------------------------+
    | PharoSurfaceView.kt (Kotlin)                      |
    |   SurfaceHolder callback, touch/key dispatch      |
    +--------------------------------------------------+
              |  JNI calls (native methods)
              v
    +--------------------------------------------------+
    | AndroidBridge.cpp (C++, JNI)                      |
    |   JNI_OnLoad, native method implementations       |
    |   Converts Java events -> EventQueue              |
    |   Manages ANativeWindow lifecycle                 |
    +--------------------------------------------------+
    | PharoVMCore (C++ static/shared library)           |
    |   ObjectMemory   - mmap (Linux native)            |
    |   Interpreter    - bytecode execution             |
    |   Primitives     - file I/O, sockets, clock       |
    |   FFI            - dlopen/dlsym (Linux native)    |
    |   Plugins        - B2D, JPEG, DSA, Sound, ...     |
    +--------------------------------------------------+
    | AndroidDisplaySurface (C++)                       |
    |   ANativeWindow_lock -> memcpy pixels -> unlock   |
    +--------------------------------------------------+
              |
              v
    +--------------------------------------------------+
    | Android OS (Linux kernel)                         |
    |   SurfaceFlinger compositor                       |
    |   Binder IPC                                      |
    |   Linux mmap/mprotect/sockets/pthreads            |
    +--------------------------------------------------+


## Recommendations

Start with the interpreter VM (our existing code):
  Get a working prototype in 3-4 weeks. All POSIX code works unchanged.
  Only need: JNI bridge, ANativeWindow display, touch events, sound.

Then evaluate JIT options:
  Once the prototype works, benchmark against the official Cog VM on Linux.
  If performance matters (it will for development use), either:
  a) Port the official Cog VM's platform layer to Android (medium effort)
  b) Use our interpreter for the initial release, add JIT later

Target Chromebooks as a first-class platform:
  Chromebooks run Android apps with keyboard + mouse + large screen.
  Pharo on a Chromebook with a real mouse would be a compelling development
  environment. Samsung DeX (phone as desktop) is similar.

Ship on F-Droid first:
  Open source distribution, no review process, builds community.
  Google Play Store later for broader reach.
