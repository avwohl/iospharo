# Plan: Pharo Smalltalk in WebAssembly (Browser)

Smalltalk running in a browser -- zero install, just open a URL and you have
the full Pharo IDE with windows, menus, a debugger, and a compiler. This
document examines whether that's feasible and what it would take.


## The Good News: It's Been Done Before

Several Smalltalk-in-browser implementations exist:

SqueakJS (Vanessa Freudenberg)
  Pure JavaScript VM that runs Squeak and Pharo images in the browser.
  Loads any Squeak/Pharo image from 1996 to current. Uses HTML5 Canvas for
  display. Has an optional JS JIT (translates bytecodes to JS functions).
  Usable for interactive development, not for heavy computation.
  https://squeak.js.org/

Caffeine (Craig Latta)
  Built on SqueakJS. Adds livecoding web pages, WebWorkers, VR support.
  Supports Squeak, Pharo, and Cuis images in the browser.
  https://caffeine.js.org/

Catalyst (Craig Latta, 2023-present)
  A from-scratch Smalltalk VM built as a native WebAssembly GC module.
  Translates Smalltalk compiled methods directly to WASM text format.
  Since both Smalltalk and WASM are stack-oriented, the translation is
  relatively straightforward. Uses WASM GC struct types for VM state.
  Active research as of 2025.

PharoJS
  Transpiles Pharo source code to JavaScript (not a VM -- generated JS
  runs natively). WebSocket bridge for development. Only a subset of Pharo
  works. https://pharojs.org/

Amber Smalltalk
  Self-hosted Smalltalk that compiles to JavaScript. Not image-based.
  Includes a web IDE. https://amber-lang.net/

The existence of SqueakJS running Pharo images proves the concept. Our C++
interpreter compiled to WASM via Emscripten would likely perform BETTER than
SqueakJS because compiled C++ in WASM runs faster than hand-written JS for
interpreter loops.


## Browser Memory Limits

This is the first hard constraint. WebAssembly uses 32-bit addressing:

    Browser         Desktop limit   Mobile limit    Notes
    --------------- --------------- --------------- -------------------------
    Chrome/V8       4 GB            ~300 MB         Full 4GB since May 2020
    Firefox         4 GB            ~300 MB         Was 2GB, now 4GB
    Safari          4 GB            ~300 MB         Kills tabs aggressively
    Edge            4 GB            ~300 MB         Uses V8 (same as Chrome)

A fresh Pharo 13 image is ~60 MB on disk. At runtime with growth:

    Scenario                        Memory needed
    ------------------------------- ---------------
    Fresh image, light browsing     100-150 MB
    Development session             200-400 MB
    Heavy (many packages loaded)    500 MB - 1 GB
    Our VM's current mmap request   4 GB virtual

Desktop browsers: Fine. 4 GB is plenty for any Pharo workload.

Mobile browsers: Tight. iOS Safari kills tabs above ~300 MB with no warning.
A fresh image works; heavy development does not.

### Memory64 Proposal
WASM 3.0 (September 2025) includes Memory64. Chrome 133+ and Firefox 134+
ship it. BUT: it comes with a severe performance penalty (10% to 100%
slowdown) because the browser can no longer eliminate bounds checks via
virtual memory tricks. Not worth it for an interpreter VM. Stick with 4 GB.

### Dynamic Growth
WASM supports memory.grow in 64 KiB page increments. Strategy: start with
~128 MB, grow on demand. Emscripten handles this with -sALLOW_MEMORY_GROWTH=1.
Better than reserving 4 GB upfront, especially on mobile.


## The JIT Problem (Not a Problem for Us)

WebAssembly does NOT allow runtime code generation within a running module.
This is the same constraint we already handle on iOS. Our iospharo VM is
already interpreter-only -- no JIT, no mmap(PROT_EXEC). This makes our VM
uniquely well-suited for WASM. The official Cog VM CANNOT run in WASM because
it generates native machine code at runtime.

### Module-level JIT Workaround
Andy Wingo documented a technique: generate a new WASM module at runtime,
instantiate it, and add its functions to the indirect function table. This is
how Craig Latta's Catalyst VM works -- translating Smalltalk methods to WASM
functions on the fly. Compilation latency is high (browser's WASM compiler
must process each new module), but it's functional.

### Performance Expectations

    Execution mode                          Relative speed
    --------------------------------------- -----------------
    Cog VM native (JIT)                     15-30x
    Our C++ VM native (interpreter)         3-5x
    Our C++ VM in WASM (interpreter)        1.5-3x
    SqueakJS (pure JS interpreter)          1x (baseline)

C/C++ compiled to WASM runs ~1.5x slower than native. On top of our existing
interpreter penalty (3-10x vs Cog JIT), expect Pharo-in-WASM to be roughly
5-25x slower than Pharo on Cog natively. This is comparable to SqueakJS
performance, which is usable for interactive development.


## What Needs to Change in Our VM

Our VM has several properties that make it a strong WASM candidate:

  1. Interpreter-only: matches WASM's no-JIT constraint perfectly
  2. Clean C++17: Emscripten handles modern C++ well
  3. Abstracted display: DisplaySurface is a clean interface
  4. Abstracted events: EventQueue is a clean interface
  5. Standard Spur format: loads unmodified Pharo images

### File-by-file assessment:

ObjectMemory.cpp -- Small change
  Replace mmap/munmap with malloc/free. Emscripten does not reliably support
  MAP_ANONYMOUS for large allocations. Lose the lazy-commit optimization;
  start smaller and grow via emscripten_resize_heap().

Interpreter.cpp -- Medium change
  Remove CFRunLoopRunInMode (Apple run loop pumping).
  Replace the interpret() infinite loop with chunked execution that returns
  control to the browser via emscripten_set_main_loop(). Pattern:

      void interpreterTick() {
          auto deadline = now() + 14ms;  // ~60fps budget
          while (now() < deadline) {
              interpreter.executeBytecodes(1000);
          }
          if (displayDirty) updateCanvas();
      }
      emscripten_set_main_loop(interpreterTick, 0, 1);

  Replace heartbeat std::thread with setTimeout/setInterval callback.
  Handle setjmp/longjmp with -sSUPPORT_LONGJMP=wasm flag.

FFI.cpp -- Significant change
  Replace all dlsym/dlopen with a static function registry. Our existing
  SDL2 stub system is a model for this -- register known functions by name.
  Emscripten provides native SDL2 support (-sUSE_SDL=2) which maps SDL2
  calls to Canvas/WebGL automatically. The Pharo image's OSSDL2Driver would
  work through Emscripten's real SDL2 instead of our stubs.

  libffi is the hardest single problem. A community port exists (trampoline-
  based, type signatures encoded as integers dispatched through switch
  statements). Alternative: since our FFI primarily serves SDL2, use
  Emscripten's native SDL2 and avoid libffi for the display path.

WorldRenderer.cpp -- Significant change
  All text rendering uses CoreText/CoreGraphics (Apple-only). Replace with:
    - Canvas 2D text rendering via EM_JS calls, or
    - FreeType + HarfBuzz compiled to WASM (Emscripten has ports)
  Non-text pixel manipulation is platform-independent and works unchanged.

Primitives.cpp -- Small-medium change
  File I/O: works unchanged via Emscripten's MEMFS (in-memory filesystem).
  Persistence: IDBFS for saving images to IndexedDB.
  Sockets: do not work. Replace with WebSocket shim or stub out.
  Sound: replace AudioToolbox with Web Audio API or stub out.
  MIDI: replace CoreMIDI with Web MIDI API or stub out.
  Clock/time: Emscripten provides gettimeofday/clock_gettime.

Plugins (C code) -- Mostly unchanged
  B2DPlugin.c, JPEGReaderPlugin.c, DSAPrims.c: pure C, compile unchanged.
  SqueakSSL/sqMacSSL: needs WebCrypto API replacement or stub.
  Cairo/FreeType/Pixman/HarfBuzz/libpng: Emscripten has ports for all of
  these (-sUSE_FREETYPE=1, -sUSE_LIBPNG=1).

New platform code needed:
  WasmBridge.cpp -- platform integration (replaces iOSBridge)
  CanvasDisplaySurface -- implements DisplaySurface via Canvas putImageData()
  DOM event bridge -- converts mousedown/keydown/etc to Pharo Events


## Threading

Our VM uses several OS threads that need replacement:

    Thread              Purpose                     WASM replacement
    ------------------- --------------------------- ----------------------
    heartbeatThread_    Timer semaphore signaling    setTimeout/setInterval
    gIOThread           Socket I/O monitoring        Browser async APIs
    DNS resolver        Hostname resolution          fetch() / WebSocket
    Audio callback      Sound output                 Web Audio API callback

Pharo's green threads (Processes) work perfectly in single-threaded WASM --
the process scheduler is entirely in-image, managed by the interpreter.

WASM threads (SharedArrayBuffer + Web Workers) exist in Chrome, Firefox, and
Safari, but require cross-origin isolation headers. We probably don't need
them -- the interpreter is inherently single-threaded.


## Persistence

    Loading the image:
      Fetch .image via HTTP, load into MEMFS.
      Or: --preload-file packages the image into the WASM build.
      A 60MB image with gzip: ~20-30MB transfer.

    Saving the image:
      Option 1: IDBFS -- mount a directory backed by IndexedDB. Survives
      page reloads. Requires FS.syncfs() call after save.
      Option 2: Download -- offer the saved image as a browser download
      via Blob + URL.createObjectURL().

    Startup time estimate:
      Image download (cached): 0s / first load on fast connection: 2-5s
      WASM module compilation: 1-2s (streamed)
      Image parsing + load: 3-5s in WASM
      Total first load: 5-15s
      Subsequent loads (cached): 3-7s


## Networking

Raw sockets do not exist in browsers. The SocketPlugin would not work.

    Need                    Browser replacement
    ----------------------- ----------------------------
    TCP sockets             WebSocket (wss://)
    HTTP requests           fetch() API
    DNS resolution          Implicit in fetch/WebSocket
    UDP                     Not available (WebRTC?)

Pharo's networking would need image-side changes to use WebSocket-based
transports. Alternatively, run a proxy server that bridges WebSockets to
TCP for legacy Pharo networking code.


## Build System

    # Emscripten build
    emcmake cmake -B build-wasm \
      -DCMAKE_BUILD_TYPE=Release \
      -DPHARO_WITH_CRYPTO=OFF
    emmake make -C build-wasm

    # Link flags
    -sUSE_SDL=2
    -sALLOW_MEMORY_GROWTH=1
    -sMAXIMUM_MEMORY=4294967296
    -sSUPPORT_LONGJMP=wasm
    -sEXPORTED_FUNCTIONS=['_main']
    --preload-file Pharo.image@/Pharo.image

    # Output
    pharo.html    -- host page with canvas
    pharo.js      -- Emscripten glue
    pharo.wasm    -- compiled VM (~2-5 MB)
    pharo.data    -- preloaded image (gzipped, ~20-30 MB)


## Estimated Effort

    Task                                            Time
    ----------------------------------------------- ----------
    ObjectMemory mmap -> malloc                     1-2 days
    Interpreter loop for emscripten_set_main_loop   2-3 days
    CanvasDisplaySurface + event bridge             3-5 days
    FFI static registry (replacing dlsym)           2-3 days
    WorldRenderer text (replacing CoreText)         3-5 days
    CMake + Emscripten build system                 1-2 days
    WasmBridge platform layer                       2-3 days
    Testing and debugging                           5-10 days
    ----------------------------------------------- ----------
    TOTAL for working prototype                     ~3-5 weeks

    Polish (persistence, networking, sound, mobile): +2-4 months


## Would It Be Usable or Just a Demo?

Based on SqueakJS precedent (which already runs Pharo images):

    Use case                    Verdict
    --------------------------- ------------------------------------------
    Interactive development     Yes -- browsing, editing, small examples
    Education / tutorials       Excellent -- zero install, open a URL
    Live coding demos           Excellent -- share a link, everyone sees it
    "Try Pharo" experience      The killer app for this
    Production applications     No -- too slow, memory constrained on mobile
    Heavy computation           No -- 5-25x slower than native Cog VM

The value proposition is ZERO-INSTALL ACCESS, not performance. Open a URL,
get the full Pharo IDE. No download, no VM installation, no image management.
This is how you get people to try Smalltalk.


## Architecture Diagram

    Browser tab
    +--------------------------------------------------+
    | index.html                                        |
    |  +--------------------------------------------+  |
    |  | <canvas id="pharo"> (display surface)      |  |
    |  +--------------------------------------------+  |
    |                                                   |
    |  pharo.js (Emscripten glue)                       |
    |    |                                              |
    |    +-- Module.canvas -> Canvas element             |
    |    +-- FS.mount(IDBFS, {}, '/images')             |
    |    +-- fetch('Pharo.image') -> MEMFS              |
    |    |                                              |
    |    +-- emscripten_set_main_loop(tick, 0, 1)       |
    |         |                                         |
    |         +-- run bytecodes for ~14ms               |
    |         +-- check EventQueue (DOM events)         |
    |         +-- if dirty: putImageData() to canvas    |
    |                                                   |
    |  pharo.wasm (~3 MB)                               |
    |    ObjectMemory (malloc-based heap)                |
    |    Interpreter (bytecode loop, chunked)            |
    |    Primitives (file via MEMFS, no sockets)         |
    |    FFI (static registry, Emscripten SDL2)          |
    |                                                   |
    |  pharo.data (~25 MB gzipped)                      |
    |    Pharo.image + Pharo.changes                    |
    +--------------------------------------------------+
    |  IndexedDB (persistent image storage)             |
    +--------------------------------------------------+


## Comparison with Other Approaches

    Approach                    Performance   Install   Image compat   Effort
    --------------------------- ------------- --------- -------------- --------
    Our C++ VM -> Emscripten    Good          Zero      Full Pharo     3-5 wks
    SqueakJS (exists today)     Fair          Zero      Most of Pharo  0 (done)
    Catalyst (Latta, WASM GC)   Unknown       Zero      Research       Years
    PharoJS (transpiler)        Native JS     Zero      Subset only    N/A
    Official Cog VM -> WASM     Impossible    N/A       N/A            N/A
                                (needs JIT)
