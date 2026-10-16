# Building iospharo

This text was in README.md until 2026-10-17.

## Prerequisites

You need **full Xcode**, not just the Command Line Tools. The iOS, Simulator
and Mac Catalyst SDKs ship only inside `Xcode.app`; a Command-Line-Tools-only
install has no iOS SDK, and the build then fails deep inside a CMake
try-compile with `library 'System' not found`, which does not name the real
cause.

```bash
# Install Xcode 15+ from the App Store, then point the toolchain at it:
sudo xcode-select -s /Applications/Xcode.app/Contents/Developer

# Verify — this must print a path, not an error:
xcrun --sdk iphoneos --show-sdk-path

# CMake (build system)
brew install cmake

# For third-party library builds (cairo, freetype, etc.)
brew install meson ninja pkg-config autoconf automake libtool
```

If you cannot change the machine-wide selection, set `DEVELOPER_DIR` for the
build instead — the build scripts also detect this case and correct it
themselves:

```bash
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
```

You also need:
- **A Pharo 13 or 14 image** — download from https://pharo.org/download

### Clone with submodules

`third_party/asmjit` (the Tier 2 JIT backend) is a git submodule and is
**required** — a plain `git clone` leaves it empty and CMake will stop.

```bash
git clone --recurse-submodules https://github.com/avwohl/iospharo.git

# already cloned without it?
git submodule update --init --recursive
```

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
