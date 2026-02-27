# What's New in Build 30

Build 30 — 2026-02-27

## Bug Fixes

### Crash on Quit (SIGABRT) (reported by users)
Quitting Pharo crashed the app with SIGABRT instead of exiting cleanly.

Root cause: the SocketPlugin I/O monitor thread was a static std::thread
that was still joinable when exit() ran. During atexit cleanup, its
destructor called std::terminate() which aborted the process.

Fix: the I/O thread is now detached after creation so its destructor is
a no-op during process exit.

### Network Connections on IPv6-Only Networks (NAT64)
Expanded the Build 28 DNS fix to fully support IPv6-only networks with
NAT64/DNS64 (common on iPad cellular and some WiFi networks).

Two problems: (1) Pharo 13's NetNameResolver requires exactly 4-byte
addresses — returning 16-byte IPv6 addresses crashes with SizeMismatch.
(2) Even with a valid IPv4 address from DNS, connecting via IPv4 fails
on IPv6-only networks because there is no IPv4 route.

Fix: DNS always returns 4-byte IPv4 addresses (extracting the embedded
IPv4 from synthesized IPv6 when no IPv4 results exist). The socket
connect primitive now uses getaddrinfo() with AI_NUMERICHOST to
re-resolve the address before connecting — on NAT64 networks this
synthesizes the proper IPv6 address from the IPv4 literal, and the
socket is automatically re-created as AF_INET6. This is Apple's
recommended approach for NAT64 compatibility.

---

# What's New in Build 28

Build 28 — 2026-02-27

## Bug Fixes

### Hardware Keyboard Input on iPad (reported by users)
Typing with a connected hardware keyboard (Bluetooth or Smart Connector)
did nothing when the on-screen keyboard was hidden. The soft keyboard
worked fine.

Root cause: on iOS, regular key events were routed exclusively through
UIKeyInput (which only fires when the view is first responder / soft
keyboard showing). With the soft keyboard hidden, no view was first
responder, so hardware keyboard events went nowhere.

Fix: the view controller now becomes first responder on iOS when the soft
keyboard is hidden, capturing hardware keyboard events via pressesBegan.
When the soft keyboard is shown, the Metal view takes over via UIKeyInput.
When dismissed, focus returns to the view controller automatically.

### Network Connections Timeout on iPad (~40 seconds)
The Help Browser and other network operations locked up for ~40 seconds
then failed with "connection aborted" on real iPads.

Root cause: DNS resolution forced IPv4 only (AF_INET). On IPv6-preferred
networks (common on iPad, especially cellular), the IPv4 DNS query times
out because there may be no IPv4 DNS server available.

Fix: DNS now uses AF_UNSPEC, allowing the OS to use whatever address
family is available (with automatic DNS64/NAT64 synthesis on iOS). IPv4
results are preferred when available. Socket connect handles both 4-byte
IPv4 and 16-byte IPv6 addresses, re-creating the socket as AF_INET6 when
needed.

---

# What's New in Build 27

Build 27 — 2026-02-27

## Bug Fixes

### Keyboard Text Input Fixed (reported by users)
Typing in Pharo editors (Playground, System Browser, Transcript, etc.) did
nothing — keystrokes were intercepted as shortcuts but never inserted as text.

Root cause: the SDL2 event stub only generated SDL_KEYDOWN/KEYUP events.
Pharo's OSSDL2Driver requires SDL_TEXTINPUT events for text insertion. Real
SDL2 generates both: KEYDOWN, TEXTINPUT, KEYUP for each typed character.

Fix: keystroke events with printable characters and no command modifiers now
generate SDL_TEXTINPUT with proper UTF-8 encoding. Modifier chords (Cmd+C,
Ctrl+X, etc.) continue to generate only KEYDOWN/KEYUP so shortcuts still work.

### Pharo Menu Bar No Longer Overlaps macOS Traffic Lights
On Mac Catalyst, the Pharo in-image menu bar (Pharo, Browse, Debug, ...)
overlapped the red/yellow/green window controls. The Metal view now insets
from the top on Mac to leave room for the title bar area.

---

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
