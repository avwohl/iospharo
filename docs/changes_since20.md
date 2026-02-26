# Changes Since Build 20

Build 21 — 2026-02-26

## Bug Fixes

### iOS Soft Keyboard Input (reported by Tim, Pharo users group)
- Fixed: regular character input did nothing on iPad soft keyboard. Root cause
  was iOS autocorrect/prediction buffering characters instead of delivering them
  immediately. Disabled autocorrection, autocapitalization, spell checking, smart
  quotes, and smart dashes on the Pharo canvas view.
- Fixed: Backspace and Enter were doubled (entered twice per keystroke). On iOS,
  both pressesBegan and UIKeyInput fired for the same key. pressesBegan now skips
  keys that UIKeyInput handles (regular chars, enter, backspace without Cmd/Ctrl).

### Long-Press Right-Click Reliability (reported by Tim, Pharo users group)
- Fixed: long-press to right-click failed most of the time. touchesBegan sent a
  RED (left) button-down immediately on finger contact; 0.5s later the long-press
  handler sent YELLOW (right) button-down on top of it. Pharo had conflicting
  button states and ignored the right-click. Fix: explicitly send RED button-up
  before YELLOW button-down, and set cancelsTouchesInView so UIKit cleanly cancels
  the original touch.

### Timer Check Starvation
- Fixed: periodic check (every 1024 bytecodes) could be permanently starved by
  tight loops with even bytecode counts. Added deferred check flag that fires on
  the next non-extension step if skipped. Fixes BlockClosureValueWithinDurationTest
  hang.

## New Features

### Image Library Redesign
- Redesigned image library to match the Pharo Launcher table layout
- Full-width table with sortable column headers: Name, Version, Size, Last Modified
- Click column headers to sort ascending/descending with sort indicator arrows
- Search/filter bar at top to filter images by name
- Detail panel at bottom shows image info when selected (file name, location,
  version, total size, created/launched dates)
- Context menu on image rows: Launch, Rename, Duplicate, Share, Show in Files, Delete
- Rename alert with text field
- Duplicate creates a full copy of the image directory
- Share sheet for AirDrop / Save to Files
- Show in Files button opens the image directory in Finder (Mac) or Files app (iPad)

### Virtual Ctrl Key (suggested by Tim, Pharo users group)
- New floating "Ctrl" button on the iOS canvas toolbar
- Toggle on/off — stays active until tapped again
- When active: canvas taps include Ctrl modifier (Ctrl+click = right-click)
- When active: soft keyboard input includes Ctrl modifier (Ctrl+E = Do It,
  Ctrl+D = Debug It, Ctrl+P = Print It, etc.)

### Keyboard Toggle Button
- New floating keyboard button on the iOS canvas toolbar
- Tap to show/hide the soft keyboard on demand
- State syncs with VM text input callbacks

### Project Info Bar
- Persistent info bar at the top of the image library window
- Shows: project name, experimental release disclaimer, Pharo.org link,
  GitHub link, Report a Bug link
- Always visible regardless of selection state
- Removed redundant disclaimer from settings screen

## Known Issues (Build 21)

- Two-finger scroll does not work on iPad — must use scrollbar widgets
  (reported by Tim, Pharo users group)
- Stage Manager window resize causes garbled display
- Onboarding/help overlay needed — users don't discover gestures

---

Build 22 — 2026-02-26

## Bug Fixes

### Two-Finger Scroll on iPad (reported by Tim, Pharo users group)
- Fixed: two-finger scroll did nothing on iPad. The pan gesture recognizer had
  cancelsTouchesInView=false, so individual finger touches also sent mouse-down
  events to Pharo, conflicting with the scroll events. Set to true and added
  require(toFail:) coordination with the two-finger tap gesture.

### Stage Manager Window Resize
- Fixed: resizing via Stage Manager or Mac window drag caused garbled display.
  Root cause was a use-after-free race: backBuffer_.assign() could reallocate
  the display buffer while the VM thread held a stale pointer from pixels().
  Fix: pre-reserve buffer capacity (4096x3072) so resize() never reallocates.

### App Exit on Quit
- Fixed: on iPhone, quitting Pharo returned to the image library in a broken
  state (VM global state not resettable). App now calls exit(0) after VM
  cleanup, matching the Cmd+Q behavior on Mac Catalyst.

## Known Issues

- Onboarding/help overlay needed — users don't discover gestures
