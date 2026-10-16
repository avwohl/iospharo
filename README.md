# iospharo

A Pharo Smalltalk VM for iOS and macOS, written in C++.

## Overview

iospharo runs standard Pharo 13 and Pharo 14 images on iOS devices and on
the Mac (through Catalyst).  It is a new implementation, not a port of the
Cog VM.  It has an interpreter and a JIT compiler for the Sista V1 bytecode
set, FFI with callbacks, and the standard VM plugins.

**Note:** Pharo 12 and earlier use a different class table layout.  The VM
does not support them.

## Status

- The full Pharo 13 SUnit suite (2042 classes, 27996 tests): 27812 pass.
  The tests that do not pass are a live network query, a UI timing race and
  two timing-dependent tests (docs/WIP.md).
- Package tests (93 package images): the same results as the Cog VM, in
  less than half the time.
- Speed: about the same as Cog on large programs.  See
  [docs/benchmarking.md](docs/benchmarking.md).
- GUI: Metal rendering, menus, touch-to-mouse events, hardware keyboard,
  and an image library.

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

## Build

The full build (xcframeworks, third-party libraries, the app) is in
[docs/building.md](docs/building.md).  When steps 1 and 2 there are done,
this builds a command-line VM for the Mac:

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/test_load_image /path/to/Pharo.image
```

## Documentation

- [docs/building.md](docs/building.md): prerequisites and build steps
- [docs/architecture.md](docs/architecture.md): project structure,
  architecture, configuration
- [docs/benchmarking.md](docs/benchmarking.md): benchmarks against Cog
- [docs/WIP.md](docs/WIP.md): the current state of the work
- [docs/deferred.md](docs/deferred.md): known gaps and deferred work
- [docs/vm-compat-bugs.md](docs/vm-compat-bugs.md): VM defects, open and
  fixed
- [docs/image_issues.md](docs/image_issues.md): Pharo image defects
- [docs/credits.md](docs/credits.md): credits and acknowledgements

## Related

- **[pharo-headless-test](https://github.com/avwohl/pharo-headless-test)** —
  Run the full Pharo SUnit test suite headless with a fake GUI (click menus,
  press buttons, take screenshots — no display needed). Extracted from this
  project but works with any Spur VM. Included here as a submodule at
  `scripts/pharo-headless-test/`.

## License

MIT — see [LICENSE](LICENSE) and [THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES).

## Credits

This VM uses the architecture of the Pharo and Squeak projects and many
open-source libraries.  See [docs/credits.md](docs/credits.md).
