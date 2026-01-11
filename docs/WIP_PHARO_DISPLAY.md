# WIP: Pharo Smalltalk Display Issues

## Status
**All issues addressed** - Fixes implemented in `src/vm/Interpreter.cpp`

## Issues Fixed

### 1. Menu Bar Text Vertical Alignment [FIXED]
- **Problem**: Menu bar text was flush to the bottom with empty space above
- **Fix**: Force MenubarItemMorph to use same Y bounds as MenubarMorph
- **Result**: Text now properly vertically centered in menu bar

### 2. Retro/Bitmapped Font Appearance [FIXED]
- **Problem**: Font looked very retro/bitmapped
- **Fix**: Added edge smoothing/anti-aliasing with 80% alpha blend fringe pixels
- **Result**: Smoother font edges without needing a new font

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
- `src/vm/Interpreter.cpp` - Main rendering, click handling, action inspection
- `src/vm/Interpreter.hpp` - Added DropdownState struct and member variables

### Key Functions
- `renderWorldMorphs()` - Custom menu bar rendering with proper spacing
- `processInputEvents()` - Click detection for menu bar and dropdown
- `invokeMenuItemAction()` - Inspect menu item morph for action (selector/target/block)

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
