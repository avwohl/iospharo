# iospharo

iOS client for the Pharo Smalltalk VM.

## Overview

iospharo is an iOS/macOS application that runs the Pharo Smalltalk virtual machine, providing an interactive development environment on Apple mobile devices.

## Features

- **SwiftUI Interface**: Modern, native iOS UI
- **Metal Rendering**: GPU-accelerated display of the Pharo world
- **Touch Support**: Full gesture mapping for Pharo mouse events
  - Tap = Left click
  - Long press = Right click
  - Two-finger tap = Middle click
  - Drag = Mouse move with button down
- **Image Management**: Download and manage Pharo images
- **Mac Catalyst**: Runs on macOS via Catalyst

## Requirements

- iOS 13.0+ / macOS 10.15+ (Catalyst)
- Xcode 14.0+
- CMake 3.20+
- pharo-vm source (sibling directory)

## Project Structure

```
iospharo/
├── CMakeLists.txt          # Build configuration
├── cmake/
│   └── iOS.cmake           # iOS platform config
├── src/
│   └── ios/
│       ├── iosDisplay.m    # Display backend
│       ├── iosEvents.m     # Event handling
│       ├── iosParameters.m # Bundle config
│       └── iosUtils.mm     # Utilities
├── iospharo/               # Swift app
│   ├── App/
│   │   ├── iosparoApp.swift
│   │   └── ContentView.swift
│   ├── Bridge/
│   │   ├── PharoBridge.swift
│   │   └── iospharo-Bridging-Header.h
│   ├── Metal/
│   │   ├── MetalRenderer.swift
│   │   └── Shaders.metal
│   ├── Views/
│   │   └── PharoCanvasView.swift
│   └── Image/
│       └── ImageManager.swift
└── resources/
    └── ios/
        └── Info.plist.in
```

## Building

### Prerequisites

1. Clone pharo-vm as a sibling directory:
   ```bash
   cd /path/to/pharo
   git clone https://github.com/pharo-project/pharo-vm.git
   ```

2. Generate VM sources (if not already done):
   ```bash
   cd pharo-vm
   cmake -B build -DGENERATE_SOURCES=ON
   cmake --build build
   ```

### Build VM Library

```bash
cd iospharo
mkdir build && cd build
cmake .. -G Xcode \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0
cmake --build . --config Release
```

### Build iOS App

1. Open `iospharo/iospharo.xcodeproj` in Xcode
2. Add the built `libPharoVMCore.a` to the project
3. Configure signing
4. Build and run

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
│   iosDisplay.m              │  VM display interface
│   iosEvents.m               │  Event queue
├─────────────────────────────┤
│   libPharoVMCore.a          │  Pharo VM (static library)
└─────────────────────────────┘
```

## Touch Gestures

| Gesture | Pharo Event |
|---------|-------------|
| Single tap | Red button click |
| Double tap | Double click |
| Long press | Blue button (right click) |
| Two-finger tap | Yellow button (middle click) |
| Drag | Mouse move with red button |
| Pinch | Zoom (Cmd+scroll) |

## Configuration

Edit `Info.plist` to configure:

- `PharoImageFile`: Default image filename
- `PharoMaxOldSpaceSize`: Max heap size (bytes)
- `PharoEdenSize`: Young generation size
- `PharoCodeSize`: JIT code space size
- `PharoHeadless`: Run without display

## Known Limitations

- JIT compilation may have restrictions on iOS due to code signing
- File access is limited to the app's sandbox
- Background execution is limited by iOS

## License

GPL v3 (matching Pharo VM license)

## Credits

Built on the [Pharo VM](https://github.com/pharo-project/pharo-vm) project.
