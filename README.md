# iospharo

A Pharo Smalltalk VM for iOS and macOS, written as a clean C++ interpreter.

## Overview

iospharo runs standard Pharo 13 and Pharo 14 images on iOS devices and Mac
(via Catalyst). It is a from-scratch interpreter implementation — not a port
of the Cog JIT VM — with full support for the Sista V1 bytecode set, FFI with
callbacks, and the standard Pharo test suite.

**Note:** Pharo 12 and earlier use a different class table layout that the VM
does not yet handle.

## Status

**VM core (solid):**
- **99.90% test pass rate** on Mac Catalyst (13,040 / 13,053)
- **99.55% test pass rate** on iOS Simulator
- FFI with callbacks (sigsetjmp/siglongjmp)
- All standard VM plugins built-in (B2D, JPEG, DSA, SSL, etc.)
- Third-party libraries: cairo, freetype, harfbuzz, pixman, libpng, OpenSSL, libssh2, libgit2

**GUI (working):**
- Metal rendering pipeline — Pharo desktop renders correctly
- Menu bar, world menu, and context menus all functional
- Touch-to-mouse event translation (tap, long-press, two-finger, pinch, drag)
- Hardware keyboard support with modifier keys
- Image library with download, import, and catalog management

## Install from the App Store

Available for iPad, iPhone, and Mac:

[Download on the App Store](https://apps.apple.com/us/app/pharosmalltalk/id6759073615)

**Requirements:**
- iPad (5th gen / 2017 or newer) or iPhone (6s / 2015 or newer) or Mac (Apple Silicon or Intel)
- iOS / iPadOS 15.0 or later, macOS 14.0 or later
- ~150 MB free storage (app + image + sources)

Pharo images are downloaded in-app (no separate download needed).

## Beta Testing (TestFlight)

There may be a newer pre-release version available via TestFlight:

1. Install **TestFlight** from the App Store (free, ~30 MB)
2. Open this invite link on your iPad or iPhone: [Join the Beta](https://testflight.apple.com/join/kGmPQFr9)
3. Tap "Accept" then "Install" — the app appears on your home screen

TestFlight builds expire after 90 days but auto-update when new builds
are published.

## Prerequisites

Install these before building:

```bash
# Xcode command-line tools (includes clang, make, etc.)
xcode-select --install

# CMake (build system)
brew install cmake

# For third-party library builds (cairo, freetype, etc.)
brew install meson ninja pkg-config autoconf automake libtool
```

You also need:
- **Xcode 15+** (for the iOS/Mac Catalyst app)
- **A Pharo 13 or 14 image** — download from https://pharo.org/download

## Building

### Step 1: Build libffi and SDL2 xcframeworks

```bash
scripts/build-libffi.sh
scripts/build-sdl2.sh
```

This downloads, cross-compiles, and packages libffi (FFI/callbacks) and SDL2
(display driver) as xcframeworks for iOS device, simulator, Mac Catalyst, and
macOS. Takes about 10 minutes. The xcframeworks are gitignored due to size.

### Step 2: Build third-party libraries

Cairo, freetype, harfbuzz, pixman, libpng, OpenSSL, libssh2, and libgit2
are cross-compiled as static xcframeworks:

```bash
scripts/build-third-party.sh
```

This downloads source tarballs and builds for iOS device, Simulator, and Mac Catalyst.
Takes about 15 minutes on first run. Use `--no-crypto` to skip OpenSSL and libssh2
(libgit2 is always built for local repository support).

### Step 3: Build the app

```bash
open iospharo.xcodeproj
```

Select your target (iOS device, Simulator, or My Mac - Catalyst) and build.

Xcode has a "Check XCFramework Freshness" build phase that automatically runs
`scripts/build-xcframework.sh` whenever VM source files (`src/vm/`, `src/platform/`)
are newer than the xcframework. The first Xcode build will take several minutes
while it compiles the VM; subsequent builds are fast unless you change VM sources.

To manually rebuild the VM xcframework (e.g. after a git pull):

```bash
scripts/build-xcframework.sh
```

This produces `Frameworks/PharoVMCore.xcframework` with slices for iOS device
(arm64), iOS Simulator (arm64 + x86_64), and Mac Catalyst (arm64 + x86_64).

**Code signing (optional):** To deploy to a physical device or the App Store,
copy `Local.xcconfig.example` to `Local.xcconfig` and fill in your Apple
Developer Team ID. `Local.xcconfig` is gitignored.

### Quick development build (Mac only)

For faster iteration on VM internals. This builds a headless command-line binary
(not the iOS/Catalyst app). Requires Steps 1 and 2 above — the cmake build
links against the xcframeworks in `Frameworks/`.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Run headless with a Pharo image
./build/test_load_image /path/to/Pharo.image
```

## Project Structure

```
iospharo/
├── src/vm/           C++ VM: Interpreter, ObjectMemory, Primitives, FFI
├── src/include/      VM headers (vmCallback.h, etc.)
├── src/platform/     Platform abstraction (EventQueue, display)
├── src/ios/          Generated interpreter reference (cointerp-cpp.c)
├── iospharo/         SwiftUI app (Metal renderer, bridge, views)
├── scripts/          Build scripts (VM xcframework, third-party libraries)
├── docs/             Technical reference (bytecode spec, architecture)
├── Frameworks/       Built xcframeworks (gitignored)
└── CMakeLists.txt    CMake build for the VM library
```

## Architecture

```
┌─────────────────────────────┐
│   SwiftUI App               │
│   (ContentView, Settings)   │
├─────────────────────────────┤
│   PharoBridge.swift         │  VM lifecycle, event bridge
├─────────────────────────────┤
│   MetalRenderer.swift       │  GPU display rendering
├─────────────────────────────┤
│   C++ Interpreter           │  Sista V1 bytecodes, GC,
│   (libPharoVMCore.a)        │  FFI, primitives, plugins
└─────────────────────────────┘
```

The image's OSSDL2Driver calls SDL2 functions via FFI. Our SDL2 stubs bridge
these to the Metal rendering pipeline. Touch gestures are mapped to Pharo
mouse events (tap=left-click, long-press=right-click, two-finger tap=right-click).

### Startup patches

The app applies Smalltalk patches on every launch to fix image bugs and adapt
to VM differences (stubbed SDL2, missing font glyphs, etc.). Patches are
version-specific — the app detects the Pharo version and loads `startup-13.st`
or `startup-14.st` accordingly. Users can add custom patches by creating
`startup-user.st` next to the image file (it is never overwritten).
See [docs/startup-system.md](docs/startup-system.md) for the full details.

## Configuration

VM parameters are set in `PharoBridge.swift` when calling `vm_init()`:

  maxOldSpaceSize   2 GB    Max heap (virtual, lazy commit)
  edenSize          10 MB   Young generation size
  maxCodeSize       0       JIT code space (unused)

## Benchmarking

### Quick benchmark (built-in)

The simplest performance check — already in every Pharo image:

```bash
# On the reference Cog VM:
pharo /tmp/Pharo.image eval "1 tinyBenchmarks"
# => "2718114840 bytecodes/sec; 247952910 sends/sec"

# On our VM (results written to Transcript):
./build/test_load_image /tmp/Pharo.image
```

### Benchmark suite

A self-contained benchmark runner that measures 10 workloads and writes
results to `/tmp/pharo_benchmarks.txt`:

```bash
# Inject benchmarks into a fresh image, run on both VMs, compare:
scripts/run_benchmarks.sh

# Or run on just our VM:
scripts/run_benchmarks.sh --ours-only
```

Benchmarks included:

    Benchmark       What it measures
    tinyBenchmarks  Built-in sieve + fibonacci (bytecodes/sec, sends/sec)
    fibonacci(34)   Recursive fib — pure message send overhead
    sieve(100)      Sieve of Eratosthenes — array access + loop cost
    sort(100K)      Sort shuffled array — comparison + collection ops
    dict(100K)      Dictionary put + get — hashing + lookup
    sum(1M)         Sum integers via do: — block/closure + arithmetic
    factorial(10K)  10000! — large integer arithmetic
    block(1M)       1M block evaluations — closure overhead
    instVar(1M)     1M getter/setter — accessor cost
    create(100K)    100K object allocations — GC pressure

Results go to `/tmp/pharo_benchmarks.txt` (one file per VM). The shell
script prints a side-by-side comparison table when run on both VMs.

### External benchmark suites

For more rigorous benchmarking, these community suites work with any
Pharo VM:

- **[Are We Fast Yet](https://github.com/smarr/are-we-fast-yet)** —
  14 cross-language benchmarks (Richards, DeltaBlue, Mandelbrot, NBody,
  etc.) with Pharo implementations. The standard for cross-VM comparison.
  See Marr, Daloze, and Mossenboeck, "Cross-Language Compiler
  Benchmarking — Are We Fast Yet?", DLS 2016.
- **[SMark](https://github.com/smarr/SMark)** (MIT, Stefan Marr) —
  test-like framework for repeatable benchmarks with warmup, iteration
  control, and statistical reporting.
- **[Computer Language Benchmarks Game](https://benchmarksgame-team.pages.debian.net/benchmarksgame/measurements/pharo.html)** —
  Pharo entries for binary-trees, mandelbrot, n-body, spectral-norm, etc.

## Related

- **[pharo-headless-test](https://github.com/avwohl/pharo-headless-test)** —
  Run the full Pharo SUnit test suite headless with a fake GUI (click menus,
  press buttons, take screenshots — no display needed). Extracted from this
  project but works with any Spur VM. Included here as a submodule at
  `scripts/pharo-headless-test/`.

## License

MIT — see [LICENSE](LICENSE) and [THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES).

## Credits and Acknowledgements

This project would not exist without the decades of work by the Smalltalk
community. The VM's interpreter, object memory, and primitives are a clean
C++ reimplementation of the architecture defined by the Pharo and Squeak
projects.

**Pharo and predecessors:**
- [Pharo](https://pharo.org) — the Smalltalk environment this VM executes
  (MIT; Copyright 2008-2019 The Pharo Project, Inria)
- [OpenSmalltalk-VM](https://github.com/OpenSmalltalk/opensmalltalk-vm) —
  the reference VM from which VMMaker-generated plugin code and headers
  are taken (MIT; Copyright 2013 3D Immersive Collaboration Consulting, LLC)
- [Squeak](http://squeak.org) — the original open source Smalltalk from which
  Pharo descends (MIT; Copyright 1996-2008 Viewpoints Research Institute,
  Apple Inc.)

**VMMaker-generated plugins included in this repo:**
- BalloonEnginePlugin (B2DPlugin.c) — vector graphics
- DSAPlugin (DSAPrims.c) — digital signature algorithm
- JPEGReaderPlugin, JPEGReadWriter2Plugin — JPEG codec wrappers
- SqueakSSLPlugin (SqueakSSL.c, sqMacSSL.c by Andreas Raab and Tobias Pape)

**Bundled libraries:**
- [Independent JPEG Group](http://www.ijg.org) libjpeg 6b by Thomas G. Lane
  (IJG license) — bundled in src/vm/plugins/jpeg/

**Benchmark references (not bundled, used for performance comparison):**
- [Are We Fast Yet](https://github.com/smarr/are-we-fast-yet) — cross-language
  VM benchmarks by Stefan Marr, Benoit Daloze, and Hanspeter Mossenboeck
  (mixed licenses; DLS 2016, DOI 10.1145/2989225.2989232)
- [SMark](https://github.com/smarr/SMark) — benchmark framework for Smalltalk
  (MIT; Copyright 2020 Stefan Marr and others)

**Statically linked libraries (cross-compiled as xcframeworks):**
- [libffi](https://github.com/libffi/libffi) 3.5.2 — Foreign Function Interface (MIT)
- [SDL2](https://www.libsdl.org) 2.26.5 — Simple DirectMedia Layer (zlib)
- [cairo](https://cairographics.org) 1.18.2 — 2D graphics (LGPL-2.1 / MPL-1.1)
- [FreeType](https://freetype.org) 2.13.3 — font rendering (FTL)
- [pixman](https://cairographics.org) 0.43.4 — pixel manipulation (MIT)
- [HarfBuzz](https://github.com/harfbuzz/harfbuzz) 10.1.0 — text shaping (MIT)
- [libpng](http://libpng.org) 1.6.43 — PNG support (libpng license)
- [OpenSSL](https://www.openssl.org) 3.4.0 — cryptography (Apache-2.0)
- [libssh2](https://www.libssh2.org) 1.11.1 — SSH client (BSD-3-Clause)
- [libgit2](https://libgit2.org) 1.8.4 — Git library (GPL-2.0 with linking exception)

This software is based in part on the work of the Independent JPEG Group.
