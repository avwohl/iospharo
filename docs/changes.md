# What's New in Build 25

Build 25 — 2026-02-27

## New Features

### Real Audio Output (SoundPlugin)
Pharo can now play sound. Previously, sound primitives silently discarded
all audio data. Build 25 adds a proper SoundPlugin using Apple's Audio Queue
Services with a lock-free ring buffer (VM thread writes, audio callback reads).
Primitives 300-310 and 327 produce real audio output. Recording primitives
(311-316) remain stubbed pending microphone permission work.

### MIDI Support (MIDIPlugin)
Full CoreMIDI integration for both iOS and macOS. Enumerates all connected
MIDI destinations and sources, opens input/output ports, sends Note On/Off,
CC, Program Change, Pitch Bend, and SysEx messages. Input uses a per-port
ring buffer filled by the CoreMIDI callback. Primitives 330-349 all wired
to real hardware.

### UDP Sockets
The SocketPlugin now supports UDP in addition to TCP. Two new primitives:
primitiveSocketSendUDPDataBufCount (sendto with destination address/port)
and primitiveSocketReceiveUDPDataBufCount (recvfrom, fills caller arrays
with source address and port).

### Real System Locale
Locale primitives (390-399) previously returned hardcoded US English values.
They now query the actual device locale via CFLocale: language, country,
currency symbol, decimal/thousands separators, date/time formats. Timezone
name and DST status use localtime_r. All 10 primitives registered as named
primitives under LocalePlugin.

### SecurityPlugin
Four named primitives registered under SecurityPlugin that the Pharo image
calls during startup via SecurityManager:
  - primitiveCanWriteImage — returns true (image saving always allowed)
  - primitiveDisableImageWrite — no-op
  - primitiveGetSecureUserDirectory — returns the image's directory
  - primitiveGetUntrustedUserDirectory — returns $TMPDIR

### System Clipboard
Clipboard primitives (141, 361-363) now use the real system clipboard via
PlatformBridge instead of an internal string field. Copy/paste between Pharo
and other apps works on both iOS and Mac.

## Bug Fixes

### Async DNS Resolution (fixes slow startup and connection failures)
DNS lookups (primitiveResolverStartNameLookup) were blocking the entire VM
thread via a synchronous getaddrinfo() call. This caused two problems:
  - 30-second startup delay: the UI couldn't render while DNS blocked
  - ConnectionClosed errors: already-connected sockets sat idle while DNS
    blocked other Pharo processes, causing servers to timeout

DNS now runs on a background thread and signals the resolver semaphore when
done. primitiveResolverStatus returns the real state (Busy/Ready/Error).

### SocketPlugin Connect with ByteArray Addresses
Fixed primitiveSocketConnectToPort to accept 4-byte ByteArray addresses in
network byte order, in addition to SmallInteger addresses. Pharo 13's
NetNameResolver returns ByteArrays from DNS lookups.

---

# What's New in Build 24

Build 24 — 2026-02-27

## Bug Fixes

### iPhone Forced to Landscape Mode
Portrait mode on iPhone (~390px wide) made Pharo essentially unusable:

- The image library buttons wrapped into unreadable vertical text
- Pharo's desktop windows and dialogs are designed for 1024px+ and
  didn't fit at all
- The Save As dialog was completely broken — no visible text input field,
  keyboard covered half the window
- Keyboard input reportedly didn't register

The app now locks iPhone to landscape orientation, giving ~844px of width on
modern iPhones. This is enough for Pharo's UI to work without any image-side
patching. iPad keeps all orientations (portrait and landscape). Mac is
unaffected.

### iPhone Safe Area Insets
The Pharo display now respects iOS safe area insets on iPhone and iPad.
Previously, content rendered edge-to-edge, causing the menu bar text and
taskbar to be clipped by the device's rounded corners and hidden behind
the Dynamic Island camera cutout. The Metal view is now constrained to
the safe area, with a black background filling the margins to blend with
the device bezel. Mac Catalyst is unaffected (no rounded corners or
notch on Mac windows).

## Changes

### Softened Pharo.org Disclaimer
Changed the image library disclaimer from "not affiliated with or
endorsed by Pharo.org" to "not endorsed by Pharo.org". The project is
engaged with the Pharo community — the disclaimer just clarifies that
bug reports should come to us, not Pharo.org.

---

# What's New in Build 22

Build 22 — 2026-02-26

## Bug Fixes

### Two-Finger Scroll Now Works on iPad
Previously, scrolling with two fingers on the iPad screen did nothing — you had
to use the tiny Pharo scrollbar widgets. Two-finger scroll now sends proper
scroll wheel events to Pharo, so lists, code browsers, and text editors all
scroll naturally.

### Stage Manager Window Resize Fixed
Resizing the app window via iPad Stage Manager (or dragging the window edge on
Mac) could produce a garbled, scrambled display. Fixed a race condition where
the display buffer could be reallocated while the VM was still drawing into it.

### App Properly Exits When You Quit Pharo
On iPhone, quitting Pharo (via the World menu) would drop you back to the image
library in a broken state instead of closing the app. The app now exits cleanly
when Pharo quits, matching the Cmd+Q behavior on Mac.

### Gesture Quick Start (iOS)
First time you launch a Pharo image, a help overlay shows the key gestures:
  - Tap = left-click, Long press = right-click (context menu)
  - Two-finger scroll, Two-finger tap = right-click
  - Keyboard and Ctrl toolbar buttons
  - Ctrl+D (Do It), Ctrl+P (Print It), Ctrl+E (Inspect It)

Tap "Got it" to dismiss. Tap the "?" button in the floating toolbar to see
it again anytime.

---

# What's New in Build 21

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
