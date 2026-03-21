# Proposed Changes for Upstream Pharo

Everything in this document is a bug or improvement in the stock Pharo 13
image that affects all VMs and all platforms. Nothing here is iOS-specific.
Each item includes reproduction steps on the official Pharo VM so the Pharo
team can verify independently.

Tested against:

    Pharo-13.1.0+SNAPSHOT.build.729.sha.f201357
    pharoImage-arm64.zip from https://get.pharo.org/64/130
    Official Pharo VM 10.3.1 (macOS arm64)

---

## Bug Fixes

### 1. MicGitHubRessourceReference >> githubApi sends authenticated request with nil token

Package: Microdown-RichTextComposer
Class:   MicGitHubRessourceReference
Method:  githubApi

`MicGitHubAPI new` defaults to authenticated mode and picks up
credentials from `IceTokenCredentials`. In a fresh image the token is
nil, so GitHub gets `Authorization: Bearer nil` and returns 401. The
error path calls `extractRateInfo:` which does
`response headers at: 'X-Ratelimit-Remaining'` without a default —
crash on `KeyNotFound`.

Repro (fresh image, any VM):

    1. Help > Microdown Document Browser
    2. Expand "github://pharo-project/pharo/doc"
    3. Debugger opens on KeyNotFound

Proposed fix — change `githubApi` to:

    githubApi
        ^ MicGitHubAPI new beAnonymous

Public repos don't need auth. Alternatively, fix `extractRateInfo:` to
use `at:ifAbsent:`.

### 2. MicDocumentBrowserModel >> document sends #message instead of #messageText

Package: Microdown-RichTextComposer
Class:   MicDocumentBrowserModel
Method:  document

The error handler sends `error message` but `Error` doesn't implement
`#message` as a public API — `#messageText` is correct. Also, the
handler only catches `MicResourceReferenceError`, so network errors
like `ConnectionTimedOut` propagate uncaught.

Repro:

    1. Open Documentation Browser
    2. Click a tree item while network is unreachable
    3. Debugger opens on MessageNotUnderstood

Proposed fix:

    document
        resourceReference ifNil: [ ^ nil ].
        document ifNotNil: [ ^ document ].
        [ document := resourceReference loadMicrodown ]
            on: Error
            do: [ :error |
                document := Microdown parse: '# Error: ' , error messageText ].
        ^ document

### 3. MicDocumentBrowserPresenter >> childrenOf: missing error handler on loadChildren

Package: Microdown-RichTextComposer
Class:   MicDocumentBrowserPresenter
Method:  childrenOf:

The `ifEmpty:` branch has `on: Error do:` but the `ifNotEmpty:` branch
and the `loadChildren` call itself do not. DNS failure, socket timeout,
or JSON parse error propagates uncaught.

Repro:

    1. Open Documentation Browser
    2. Disconnect from network
    3. Expand a GitHub tree node
    4. Debugger opens on ConnectionTimedOut

Proposed fix — wrap entire body:

    childrenOf: aNode
        [
            (aNode isKindOf: MicElement)
                ifTrue: [ ^ aNode subsections children ].
            aNode loadChildren
                ifNotEmpty: [ :children |
                    ^ children sort: [:a :b |
                        (self displayStringOf: a)
                            < (self displayStringOf: b)] ]
                ifEmpty: [
                    ^ self childrenOf:
                        (MicSectionBlock fromRoot: aNode loadMicrodown) ]
        ] on: Error do: [ ^ #() ]

### 4. Embedded Source Sans Pro font missing common Unicode glyphs

Package: EmbeddedFreeType
Font:    SourceSansProRegular (v2.020, copyright 2010/2012)

The embedded font is missing several commonly-used glyphs:

    Character   Codepoint   Used by
    ⌘           U+2318      KMShortcutPrinter (Cmd modifier)
    ⌥           U+2325      KMShortcutPrinter (Alt/Option)
    ⌃           U+2303      KMShortcutPrinter (Ctrl)
    ⇧           U+21E7      KMShortcutPrinter (Shift)
    ⏎           U+23CE      KMShortcutPrinter (Enter)
    •           U+2022      MicRichTextComposer bulletForLevel:

FreeType renders .notdef (small box / "?") for all of these.

Repro (any VM, macOS/Linux/Windows):

    1. Right-click in System Browser
    2. Shortcuts show "?D", "?S" instead of "⌘D", "⌘S"

    1. Help > Microdown Document Browser
    2. Expand a document with bullet lists
    3. Bullets show "?" instead of "•"

Proposed fix: Update to Source Sans 3 (v3.052, April 2023). Same license
(SIL OFL), drop-in replacement, includes all the above glyphs. Adobe
added the keyboard modifier symbols in v2.040 (October 2018).

### 5. WarpBlt >> mixPix:sourceMap:destMap: drops alpha channel

Package: Graphics-Primitives
Class:   WarpBlt
Method:  mixPix:sourceMap:destMap:

When `primitiveWarpBits` is unavailable or fails, the Smalltalk fallback
averages R, G, B for anti-aliasing smoothing but ignores the alpha
channel (bits 24-31). The result has alpha=0 (fully transparent).

Repro (any VM where primitiveWarpBits fails, or patch it to fail):

    1. Color red inspect
    2. Click the "Color" tab
    3. Swatch is gray/transparent instead of red

Root cause — only RGB are summed:

    r := r + ((rgb bitShift: -16) bitAnd: 255).
    g := g + ((rgb bitShift: -8) bitAnd: 255).
    b := b + ((rgb bitShift: 0) bitAnd: 255).

Proposed fix — add alpha to the loop:

    a := a + ((rgb bitShift: -24) bitAnd: 255).

And include `a // nPix` in the reassembly.

### 6. DebugPointTest >> testTranscriptDebugPoint fails on all VMs

Package: NewTools-Debugger-Breakpoints-Tools-Tests
Class:   DebugPointTest
Method:  testTranscriptDebugPoint

Two bugs:

  (a) Missing `Transcript clear` — asserts exact `Transcript contents`
      without clearing first. Fails whenever Transcript has any prior
      content from session startup or other tests.

  (b) Fails headless — `NonInteractiveTranscript` doesn't implement
      `#contents`. The `skipOnPharoCITestingEnvironment` guard only
      skips on CI (env var), not all headless runs.

Repro on official VM:

    $ pharo Pharo.image test DebugPointTest
    testTranscriptDebugPoint: ERROR (NonInteractiveTranscript >> #contents)

    Or interactively after other tests have run:
    testTranscriptDebugPoint: FAIL ("Using authentifi..." ≠ "string")

Proposed fix:

    testTranscriptDebugPoint
        self skipOnPharoCITestingEnvironment.
        Transcript clear.
        dp := DebugPointManager
              installNew: DebugPoint
              on: node
              withBehaviors: { TranscriptBehavior }.
        dp text: 'string'.
        dp hitWithContext: context.
        self assert: Transcript contents equals: 'string'

---

## Feature Requests

### 7. Portrait / small-screen layout support

The Pharo menu bar renders 8 items in a fixed single row:

    Pharo | Browse | Debug | Sources | System | Library | Windows | Help

On any window narrower than ~600pt (iPhone portrait, narrow iPad split),
the last 3-4 items are clipped with no overflow menu.

Smallest useful fix: if world width < 600pt, collapse the menu bar into
a single hamburger button that opens a vertical menu list. This is a
Morphic-only change — no VM or platform work required.

### 8. Touch-friendly text interaction

Morphic's event model assumes mouse input:

  - No selection handles on text selections (iOS/Android have
    drag handles + magnifier loupe)
  - No pinch-to-zoom on code panes
  - Single-finger drag conflicts with text selection vs morph dragging

Suggestion: a `TouchInputMode` preference that enables native-feeling
selection handles, delegates scroll to platform physics, and supports
pinch-to-zoom on text panes.

### 9. Keyboard shortcut discoverability on touch devices

Essential shortcuts (Cmd+D Do It, Cmd+P Print It, Cmd+I Inspect It,
Cmd+S Accept) are invisible without a keyboard. A context-sensitive
action bar or long-press radial menu above text editors would make
these discoverable on touch-only devices.

### 10. Headless SUnit test runner and fake GUI for CI

Published as a standalone repo: https://github.com/avwohl/pharo-headless-test

Two pure-Smalltalk scripts that run the full SUnit suite headless on
any VM — standard Pharo, Cog JIT, interpreter, Mac Catalyst. Zero
VM-specific dependencies.

**setup_fake_gui.st** — Creates a virtual Morphic display (1024x768
Form, WorldMorph, MorphicUIManager, UI process) and installs the
`FakeGUI` helper class for programmatic interaction: click menus and
buttons by name, find morphs by label or type, open Spec presenters,
and take screenshots as PNG files. Fixes ~350 "receiver of activate
is nil" errors. Enables 94.6% pass rate (1054/1113) on 64 Spec GUI
test classes.

**run_sunit_tests.st** — Batch SUnit runner with per-test watchdog
timeouts, auto-discovery of all TestCase subclasses, skip list for
known hangers, Delay scheduler health checks, and detailed result logs.

See the repo README for full usage instructions.
