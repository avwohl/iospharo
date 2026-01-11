# WIP: Pharo Smalltalk Display Issues

## Current Status
**In Progress** - Core Graphics font rendering implemented, debugging coordinate/color issues

## Recent Changes (2026-01-11)

### Core Graphics Font Rendering [IN PROGRESS]
- Replaced bitmap font5x7 with Core Graphics/Core Text rendering
- Added `#undef nil` after including CoreGraphics/CoreText to avoid Oop::nil() conflict
- Menu bar background now bright red for debugging visibility
- Issues being debugged:
  - Text orientation/positioning (was upside down, now fixing vertical position)
  - Color channel mapping between CG bitmap context and display buffer
  - Using kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Host for native format

### Menu Bar Visibility [FIXED]
- MenubarMorph now renders LAST so it's always on top of other morphs
- Added titleBarOffset (56px Retina, 28px regular) to position below Mac window title bar

### Render Throttling [FIXED]
- Added 16ms minimum between renders (~60fps) to prevent flickering
- Display no longer blinks rapidly

## Issues Fixed (Previous)

### 1. Menu Bar Text Vertical Alignment [FIXED]
- **Problem**: Menu bar text was flush to the bottom with empty space above
- **Fix**: Force MenubarItemMorph to use same Y bounds as MenubarMorph
- **Result**: Text now properly vertically centered in menu bar

### 2. Retro/Bitmapped Font Appearance [IN PROGRESS]
- **Problem**: Font looked very retro/bitmapped (using 5x7 pixel font)
- **Current Fix**: Implemented Core Graphics/Core Text rendering for anti-aliased fonts
- **Status**: Debugging coordinate system and color format issues

### 3. Screen Flash on Menu Click [FIXED]
- **Problem**: Clicking on a menu flashed the screen
- **Fix**: Reduced debug click indicator to small subtle blue dot for 0.3 seconds
- **Result**: Click feedback is now subtle and brief

### 4. Menu Bar Item Spacing [FIXED]
- **Problem**: Menu items overlapped ("BrowseDebugSourceSystem...")
- **Fix**: Custom menu bar rendering with proper text-based spacing instead of Pharo bounds
- **Result**: "Pharo  Browse  Debug  Sources  System  Windows  Help" properly spaced

### 5. "Menu Not Available" Message [FIXED]
- **Problem**: Opening a menu showed placeholder text
- **Fix**: Implemented actual menu item extraction from MenubarMorph hierarchy
- **Result**: Dropdown shows actual Pharo menu items

### 6. Dropdown Menu Item Clicks [FIXED]
- **Problem**: Clicking menu items didn't do anything
- **Fix**: Added dropdown bounds tracking, click detection, and queued action execution:
  - `DropdownState` struct stores bounds, lineHeight, and item morphs
  - Click detection calculates which item was clicked
  - `invokeMenuItemAction()` queues action in `PendingMenuAction` struct
  - Action executed at safe point in `doOneCycleFor:` intercept (after events, before render)
- **Result**: Menu items execute their actions when clicked

## Implementation Notes

### Files Changed
- `src/vm/Interpreter.cpp` - Main rendering, click handling, action inspection, Core Graphics text
- `src/vm/Interpreter.hpp` - Added DropdownState struct and member variables
- `CMakeLists.txt` - Added CoreGraphics and CoreText framework linking

### Key Functions
- `drawText()` - Core Graphics text rendering with CTFont/CTLine
- `renderWorldMorphs()` - Custom menu bar rendering with proper spacing
- `processInputEvents()` - Click detection for menu bar and dropdown
- `invokeMenuItemAction()` - Inspect menu item morph for action (selector/target/block)

### Core Graphics Text Rendering
The drawText function creates a CGBitmapContext, renders text with CTLineDraw,
then alpha-blends the result into the display pixel buffer.

Key considerations:
- CG coordinate system has Y going up; transformed with CGContextScaleCTM
- Bitmap format uses kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Host
- Text baseline positioning affects which part of characters is visible

### Menu Action Structure
Pharo menu items can have actions in various forms:
- `selector` + `target` (traditional Morphic) - send selector to target
- `actionBlock` (Spec/Commander) - evaluate block
- Various slots contain Symbol (selector), objects (target), Block (action)

### Testing
```bash
cmake --build build-app
./build-xcframework.sh
xcodebuild -project iospharo.xcodeproj -scheme iospharo -configuration Debug \
  -destination 'platform=macOS,variant=Mac Catalyst' build
```

Console shows `[MENU-ACTION]` logs when dropdown items are clicked.
Render logs written to `/tmp/iospharo-render.log` (first 10 frames only).
