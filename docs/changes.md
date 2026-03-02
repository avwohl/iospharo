# What's New in Build 44

Build 44 — 2026-03-01

## UI Improvements

### Strip Button Icons
Replaced text labels (DoIt, Print, Inspect, Debug, Expand, Accept, Cancel)
with SF Symbol icons on both iPhone and iPad. Fixes the "Inspect" label
wrapping on small screens and the confusing "Print" name (now play triangle,
text-append, eyeglasses, ant icons etc.). All buttons now have `.help()`
tooltips visible on Mac Catalyst hover.

### Bottom Safe Area Fix
Canvas now extends to the bottom edge on iPhone and iPad (no gap for the
home indicator). Changed `.ignoresSafeArea(.keyboard)` to `.ignoresSafeArea()`.

### iPhone Strip Cleanup
Removed dividers from the iPhone button strip to save vertical space when
the keyboard is shown.

## Code Quality

### Codebase Simplification (Round 1)
Full review of ~55K lines of C++ and ~3.7K lines of Swift. Removed dead
code, deduplicated logic, and improved efficiency:

  - Removed ~670 lines of dead code (unused diagnostics, commented-out
    debug blocks, unreachable methods, dead Smalltalk scripts)
  - Deduplicated identity hash primitives (fixed 30-bit vs 22-bit mask
    inconsistency between primitiveIdentityHash and SmallInteger variant)
  - Consolidated fixedFieldCountOf, bitwise primitives (BitAnd/Or/Xor),
    and version label formatting into shared helpers
  - Replaced per-pixel display copy with memcpy in syncDisplayToSurface
  - Changed passThroughEvents_ from vector to deque for O(1) front removal
  - Deduplicated build-third-party.sh platform setup (5 functions to 1)
    and meson cross-file generation
  - Added script safety (set -euo pipefail, timeout on app launch)

### Codebase Simplification (Round 2)
Added ObjectMemory utility functions and inlined hot-path operations:

  - Inlined push/pop/stackTop/stackValue/fetchByte into Interpreter.hpp
    with __builtin_expect branch hints for better codegen
  - Added oopToString, nameOfClass, classNameOf, numLiteralsOf, selectorOf
    utilities — replaced ~30 inline string/header extraction patterns
  - Deduplicated keyboard event processing between MTKView and
    ViewController (postKeyDown/postKeyUp + shouldHandleKeyInPresses)
  - Gated recentBytecodes_ recording behind ENABLE_DEBUG_LOGGING flag
    to remove a per-bytecode write from the release hot path

Net result: ~810 lines removed across both rounds.

---

# What's New in Build 42

Build 42 — 2026-03-01

## New Features

### Expanded Modifier Strip (iPad)
The left-side strip now has 16 buttons covering the most common Pharo
operations, so you rarely need to show the soft keyboard:

  - Ctrl, Cmd — modifier toggles (same as before)
  - Tab, Esc, Backspace — direct keys (Backspace is new)
  - DoIt, PrintIt, InspectIt, DebugIt — Pharo evaluation shortcuts
  - Cut, Copy, Paste — clipboard (Cmd+X/C/V)
  - Expand, Accept, Cancel — select enclosing expression (Cmd+2),
    save code (Cmd+S), cancel edit (Cmd+L)
  - Keyboard toggle and Help at the bottom

### Compact iPhone Strip
On iPhone the strip shows a reduced set of 8 buttons that fits the
shorter landscape height:

  - Keyboard toggle at the top (stays accessible when keyboard shows)
  - Ctrl, Cmd, Backspace
  - DoIt, PrintIt, InspectIt

No Help button on iPhone (not enough room).

## Bug Fixes

### iPad Floating Keyboard Blank Space
When the iPad floating keyboard appeared, the Pharo canvas shrank and
left a blank area at the bottom. The canvas now ignores the keyboard
safe area — the keyboard floats over the Metal-rendered content instead
of pushing it up.

---

# What's New in Build 39

Build 39 — 2026-03-01

## New Features

### Left-Side Modifier Strip (iOS)
Replaced the floating circular toolbar buttons with a vertical strip on the
left edge of the screen. The strip stays fixed (no dragging) and provides:

  - Ctrl / Cmd — one-shot modifier toggles (same behavior as before)
  - Tab / Esc — direct key sends
  - DoIt / Print / Inspect — synthesize Cmd+D / Cmd+P / Cmd+I without
    needing to show the soft keyboard. Requested by Tim who found that
    the old Cmd button required showing the keyboard to type the letter.
  - Keyboard toggle and ? help button at the bottom

The canvas fills the remaining width to the right of the strip.

## Bug Fixes

### Menu Shortcut Symbols Show as "?" (reported by Tim)
Menu items showed "?D" instead of "Cmd+D" for keyboard shortcuts. The
embedded Source Sans Pro font (2012) lacks the Unicode modifier key
glyphs (U+2318 ⌘, U+2303 ⌃, etc.) that Pharo's `KMOSXShortcutPrinter`
uses. Adobe added these glyphs in Source Sans 2.040 (2018) but Pharo
still ships the old version. This affects all Pharo VMs, not just ours.

Fix: startup.st now patches `KMShortcutPrinter symbolTable` to use
ASCII text labels ("Cmd+", "Ctrl+", "Shift+", "Opt+") instead of the
missing Unicode symbols.

### Keyboard Toggle Out of Sync
The keyboard toggle button could get stuck in the "on" state when the user
dismissed the soft keyboard via the system globe/dismiss key instead of the
strip button. Now observes `keyboardWillHideNotification` to keep the
toggle in sync.

---

# What's New in Build 38

Build 38 — 2026-02-28

## Bug Fixes

### Cmd+Q Now Quits the App on Mac
Pressing Cmd+Q did nothing on Mac Catalyst — the standard Mac quit
shortcut was silently swallowed.

Root cause: To prevent crashes from Pharo's FFI calls to AppKit menu
APIs on the VM thread, `setMainMenu:` was swizzled to a complete no-op.
This also blocked UIKit/SwiftUI from installing the system menu bar
(including the Quit item). Since Cmd+Q dispatches through the system
menu bar on Mac, there was nothing to handle it.

Fix: `setMainMenu:` now checks `pthread_main_np()` — calls from the
main thread (UIKit creating the menu) pass through to the original
implementation, while calls from the VM thread (Pharo FFI) are silently
ignored. A UIKeyCommand fallback on the view controller provides a
second layer of defense.

---

# What's New in Build 37

Build 37 — 2026-02-28

## Code Review and Cleanup

Full comment-vs-code audit across the entire codebase, fixing stale
comments and correcting actual bugs found during the review.

### Comment Fixes (15 files)
  - Interpreter.hpp: rewrote bytecode summary from V3PlusClosures to
    Sista V1 with correct ranges and descriptions
  - Interpreter.cpp: fixed bytecode range labels, header bit layout
    docs, method comments (createFullBlock, activeContext,
    executePrimitive), startup attempt labels
  - Primitives.cpp: removed wrong thisContext reference, fixed 0-based
    vs 1-based indexing comment, clarified handler/unwind marker
    primitive table slot vs method header primitive index
  - ObjectMemory.hpp/cpp: clarified ClassBlockClosure alias, removed
    duplicated comment
  - ObjectHeader.hpp: format 5 "Weak with fixed" → "Ephemeron",
    format 6 clarified as unused
  - test_load_image.cpp: fixed "256 MB" → "4 GB virtual", added bit
    range labels to header dump
  - FFI.cpp: updated file header to reflect actual contents
  - SocketPlugin.cpp: "TCP" → "TCP/UDP"
  - sqMacSSL.c: fixed filename typo, fixed SqueakSSLRead → SqueakSSLWrite
  - PharoBridge.swift: simplified display callback comment
  - PharoCanvasView.swift: fixed stale vm_setTextInputCallback reference
  - NSEventMonitor.h/m: fixed local → global preferred comment (then
    deleted — see below)

### Bug Fixes Found by Review
  - isSend bytecode ranges in stepDetailed() were wrong: included
    jumps (0xB0-0xBF), stores (0xC0-0xDF), callPrimitive (0xF8),
    and pushClosure (0xF9) as sends; missed 1-arg (0x90-0x9F) and
    2-arg (0xA0-0xAF) sends. Now matches Sista V1 spec exactly.
  - primitiveIndexOf() used full byte for high bits of primitive
    index instead of masking with & 0x1F. The callPrimitive format
    is `248 iiiiiiii mssjjjjj` — only the low 5 bits (jjjjj) are
    the primitive index high bits.
  - numLiterals extraction in FullBlockClosure activation used
    & 0xFFFF instead of & 0x7FFF, including bit 15 (requiresCounters
    flag) in the literal count.

### Dead Code Removed
  - NSEventMonitor.h/m (253 lines) — never called from anywhere,
    superseded by UIKit gesture handlers
  - 9 unused bridging header declarations (deprecated display getters,
    unused VM entry points, unused SDL2 flags)
  - ImageManager.checkForExistingImage() — dead wrapper, load() is
    called directly

### Debug Code Removed
  - Removed unused globals: g_lastDispatchSelector, g_lastDispatchRcvrClass,
    g_lastDispatchMethod, g_lastDispatchPrimIndex, g_lastSelName, g_stepCount
  - Removed ~60 commented-out DEBUG_LOG lines
  - Gated [DNS], [NET], [SOCK], [SSL], [ImageWriter], [DISPATCH] logging
    behind #ifdef DEBUG
  - Gated Swift fputs/NSLog behind #if DEBUG
  - Deleted DeviceLog.hpp and 12 stale scripts

---

# What's New in Build 35

Build 35 — 2026-02-28

## New Features

### Auto-Launch with Countdown Splash
Right-click any image in the library and choose "Set as Auto-Launch" to
mark it as the default. On next app launch, a 3-second countdown splash
appears with the image name and a "Show Library" button to cancel. This
replaces the old automatic behavior (which launched whenever there was
exactly one image with no escape hatch).

Auto-launch images show an orange star icon in the image list. The
preference persists across sessions via AppStorage. Deleting an auto-launch
image automatically clears the preference.

### CLI `--image` Flag
Launch the app with a specific image from the command line:

    open /path/to/iospharo.app --args --image /tmp/Pharo.image

This bypasses both the library and the splash screen for immediate launch.
Useful for automated testing and scripting.

## Bug Fixes

### SSL Data Loss After EOF (Doc Browser Blank Right Pane)
The Build 34 `eofDetected` fix had a secondary bug: after setting
`eofDetected = true`, the I/O thread stopped monitoring the socket entirely
and never signaled `readSema` again. Pharo's SSL layer had buffered
decrypted data but was never woken up to drain it via `recv()`.

Fix: The I/O thread now continues monitoring SOCK_CONNECTED sockets in
`readfds` even after EOF. The MSG_PEEK probe only runs once (gated on
`!eofDetected`), but `readSema` is signaled every 100ms until Pharo's
`recv()` returns 0 and the recv primitive sets SOCK_OTHER_END_CLOSED.
Write monitoring is skipped after EOF (no point signaling write-ready
after FIN).

---

# What's New in Build 34

Build 34 — 2026-02-28

## Bug Fixes

### SSL Read Stall on Connection Close (Doc Browser Blank Right Pane)
Clicking items in the Help Documentation Browser showed a blank right pane.
The tree populated correctly (fixed in Build 33), but document content never
loaded because HTTPS downloads from raw.githubusercontent.com stalled after
receiving partial data.

Root cause: Race condition between the I/O monitor thread and Pharo's SSL
read loop. When the HTTP server sent `Connection: close`, the TCP FIN
arrived while SSL-buffered data remained unread. The I/O thread's MSG_PEEK
detected FIN (recv returned 0) and immediately set SOCK_OTHER_END_CLOSED.
Pharo's `readInto:startingAt:count:` loop checks `self isConnected` on
each iteration — with the state already set to "closed", the loop exited
before draining the SSL layer's internal buffer, losing the tail of the
HTTP response.

Fix: Added `eofDetected` flag to the socket struct. The I/O thread now
sets this flag instead of changing sockState when MSG_PEEK returns 0. The
recv() primitive is the only place that sets SOCK_OTHER_END_CLOSED, and
only when recv() itself returns 0 (meaning the kernel buffer is truly
empty). The I/O thread also stops monitoring sockets with eofDetected set,
preventing spin-signaling of the read semaphore.

Verified: 2.6 MB GitHub API response over HTTPS completes without stall
(previously stalled after ~900 bytes on the third connection).

### Doc Browser Error Handler Crash
Clicking a tree item that failed to load crashed with DNU on
`MicResourceReferenceError >> #message`. The error handler in
`MicDocumentBrowserModel >> document` used `error message` but the
actual Pharo API is `error messageText`.

Fix: startup.st now patches `document` to use `messageText` and wraps
the error in a Microdown `# Error` heading for graceful display.

### Doc Browser Tree Expansion Crash
Expanding tree nodes called `childrenOf:` which had no error handling.
Network failures or rate limits caused unhandled exceptions.

Fix: startup.st overrides `childrenOf:` with comprehensive error
handling — wraps all network calls in `on: Error do:` blocks, returning
empty arrays on failure instead of crashing.

## Logging Cleanup

Removed verbose SSL encrypt/decrypt diagnostic logging from sqMacSSL.c
(added in Build 33 for debugging). Kept only error messages (SSLRead
FAILED, SSLWrite FAILED). SSL handshake logging is still present since
it's infrequent and useful for connection diagnostics.

---

# What's New in Build 33

Build 33 — 2026-02-27

## Bug Fixes

### Help Browser Document Tree Empty
The Documentation Browser (Help > Documentation) opened but showed no
entries in the doc tree — just "I am a directory and has no contents"
when clicking the root node. Three issues were involved:

Root cause: Fresh Pharo 13 images ship with `IceTokenCredentials`
containing the placeholder token `'YOUR TOKEN'`. `MicGitHubAPI` uses
these credentials by default (not anonymous), causing GitHub to return
401 with no rate-limit headers. `MicGitHubAPI >> extractRateInfo:`
then crashes with `KeyNotFound: 'X-Ratelimit-Remaining'`. This is a
Pharo image bug affecting all VMs, not specific to ours.

Fix: The VM now auto-creates a `startup.st` alongside the Pharo image.
Pharo's `StartupPreferencesLoader` loads this script on every startup,
patching `MicGitHubRessourceReference >> githubApi` to use anonymous
API access (`MicGitHubAPI new beAnonymous`). No auth is needed for
public GitHub repos.

Supporting changes:
  - VM sets working directory to the image's parent directory so
    `StartupPreferencesLoader` finds `startup.st` (both in
    `test_load_image` and `PharoBridge`)
  - Added SSL diagnostic logging (fprintf) to `sqMacSSL.c` for
    handshake, encrypt, and decrypt operations

## Diagnostics

### SSL Diagnostic Logging
Added `fprintf(stderr, ...)` diagnostics throughout SqueakSSL
(`sqConnectSSL`, `sqEncryptSSL`, `sqDecryptSSL`). The existing
`logTrace()` macro was compiled out as `((void)0)` in `debug.h`, making
SSL completely invisible in logs. The new fprintf logging shows
handshake progress, data flow, and error codes, which was critical for
verifying that SSL works correctly and the doc browser bug was
image-side, not VM-side.

---

# What's New in Build 32

Build 32 — 2026-02-27

## Bug Fixes

### SqueakSSL Data Loss on Partial Reads
HTTPS connections (used by the Help Browser, Iceberg, and ZnClient) could
silently corrupt TLS records, causing connections to fail or return garbled
data.

Root cause: Apple Secure Transport's read callback requests N bytes of
encrypted data. When the socket buffer had fewer than N bytes available,
SqueakSSLRead() correctly copied what it had and returned errSSLWouldBlock,
but then set dataLen to 0 — discarding the bytes it had just delivered to
SSL. On the next callback, SSL expected the continuation of the same TLS
record but got new data from the socket, corrupting the record boundary.

Fix: after a partial read, dataLen is decremented by the number of bytes
consumed and any remaining bytes are shifted to the front of the buffer
with memmove(). SSL now sees a consistent byte stream across callbacks.

### Write Semaphore Spam (millions of log lines)
The stderr log grew to 2.6 million lines in under a minute, almost entirely
`[SEMA] signalSemaphoreWithIndex(10)` messages.

Root cause: TCP sockets are almost always writable (the kernel send buffer
is rarely full). The I/O monitor thread polls every 100ms with select(),
and every poll signaled the write semaphore for every connected socket.
Each signal also triggered the `[SEMA]` log line in the interpreter proxy.

Fix: added a `writeSignaled` flag to each socket. The write semaphore is
signaled once when the socket becomes writable, then suppressed until a
send() returns EAGAIN (buffer full), which re-arms the flag. This matches
the edge-triggered semantics that Pharo's SocketStream expects.

### Debug Logging Cleanup
Removed verbose diagnostic logging added during socket/SSL debugging:
  - Removed per-call `[SEMA] signalSemaphoreWithIndex(N)` from the
    interpreter proxy (the single biggest source of log spam)
  - Removed `[SOCK]` debug prints from socket creation, connect, and
    status paths (kept error-case prints and one-time init messages)
  - Removed `[DISPATCH]` logging for every Socket/SqueakSSL primitive call

---

# What's New in Build 31

Build 31 — 2026-02-27

## Bug Fixes

### NAT64 Connect Fix
Removed AI_NUMERICHOST flag from the socket connect path. Apple docs
explicitly state that AI_NUMERICHOST prevents IPv6 address synthesis on
NAT64 networks — which is the exact mechanism needed for connectivity on
IPv6-only cellular/WiFi networks with NAT64.

### Socket Creation Diagnostics
Added comprehensive logging to socket creation, connect, and plugin init
paths (`[SOCK]` prefix on stderr / os_log). This helps diagnose the
"Socket destroyed" error reported on real iPad hardware where socket
creation fails but works on Mac Catalyst.

### Enter/Backspace Doubling (reported by Tim)
Enter key inserted two newlines and Backspace deleted two characters.

Root cause: insertText() sent three events per keystroke: down, stroke,
up. For Enter (charCode 13), the down event generated SDL_KEYDOWN(Return)
and the stroke generated SDL_TEXTINPUT("\r"). Pharo processed both as
text insertions. Real SDL2 does NOT generate SDL_TEXTINPUT for Enter,
Backspace, or Tab — only KEYDOWN/KEYUP.

For Backspace (charCode 8), the stroke bypassed the TEXTINPUT path
(not printable) and fell through to the SDL_KEYDOWN condition, generating
a duplicate keydown.

Fix: removed Enter (13) and Tab (9) from the isPrintable set in the SDL
event converter. Non-printable stroke events are now silently skipped.
deleteBackward() no longer sends the redundant stroke event.

## Changes

### Ctrl Button Is Now One-Shot (suggested by Tim)
The virtual Ctrl button on the iOS toolbar now auto-clears after a
touch/click, acting as a one-shot modifier (like Shift on a phone
keyboard). Previously it stayed active until tapped again.

### Virtual Cmd Button (suggested by Tim)
Added a Cmd button to the iOS floating toolbar alongside Ctrl. Tap Cmd
then type a key to send Cmd+key — e.g. Cmd then D for "Do It". Like
Ctrl, it's one-shot: auto-clears after a keystroke or touch.

---

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
IPv4 from synthesized IPv6 when no native IPv4 results exist). The socket
connect primitive uses getaddrinfo() to re-resolve the address before
connecting — on NAT64 networks this synthesizes the proper IPv6 address
from the IPv4 literal, and the socket is automatically re-created as
AF_INET6. This is Apple's recommended approach for NAT64 compatibility.

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
