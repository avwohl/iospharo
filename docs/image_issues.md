# Pharo Image Issues

Bugs and limitations in the stock Pharo 13 image that affect iospharo.
We work around the bugs via `startup.st` (auto-loaded by
`StartupPreferencesLoader`). Ideally these would be fixed upstream so
the patches can be removed.

Tested against:

    Pharo-13.1.0+SNAPSHOT.build.729.sha.f201357
    pharoImage-arm64.zip from https://get.pharo.org/64/130
    Downloaded 2026-02-28

---

## Bug 1: MicGitHubRessourceReference >> githubApi creates authenticated client by default

**Class**: `MicGitHubRessourceReference` (package Microdown-RichTextComposer)
**Method**: `githubApi`

Stock source:

    githubApi
        ^ MicGitHubAPI new

`MicGitHubAPI new` defaults to authenticated mode. It picks up
credentials from `IceTokenCredentials`, which in a fresh image
contains `nil` for the token. The resulting HTTP request includes
an `Authorization: Bearer nil` header (or similar malformed value).
GitHub returns `401 Unauthorized`.

The 401 response omits the `X-Ratelimit-Remaining` header. The
error-handling path in `MicGitHubAPI` calls `extractRateInfo:`,
which does `response headers at: 'X-Ratelimit-Remaining'` without
a default, causing a `KeyNotFound` exception.

**Steps to reproduce** (stock Pharo 13, any VM):

    1. Download a fresh image from https://get.pharo.org/64/130
    2. Open Help > Microdown Document Browser
    3. Click the triangle to expand "github://pharo-project/pharo/doc"
    4. Tree stays empty; a debugger opens on KeyNotFound

**Suggested fix**: Either

  (a) Change `githubApi` to `^ MicGitHubAPI new beAnonymous` (public
      repos don't need auth), or

  (b) Have `MicGitHubAPI >> extractRateInfo:` use
      `response headers at: 'X-Ratelimit-Remaining' ifAbsent: [nil]`
      so 401 responses don't crash, or

  (c) Have `MicGitHubAPI` detect 401 and retry anonymously.

**Our workaround** (startup.st):

    MicGitHubRessourceReference compile: 'githubApi
        ^ MicGitHubAPI new beAnonymous'.

---

## Bug 2: MicDocumentBrowserModel >> document sends #message instead of #messageText

**Class**: `MicDocumentBrowserModel` (package Microdown-RichTextComposer)
**Method**: `document`

Stock source:

    document
        resourceReference ifNil: [ ^ nil ].
        document ifNotNil: [ ^ document ].
        [ document := resourceReference loadMicrodown.]
            on: MicResourceReferenceError
            do: [ :error |
                document := Microdown parse: '# Error: ' , error message].
        ^ document

The `do:` block sends `error message`. But `MicResourceReferenceError`
inherits from `Error`, which does not implement `#message` as a public
API. The correct accessor is `#messageText`. This causes a
`MessageNotUnderstood: MicResourceReferenceError >> #message` when
any document fails to load (network timeout, 404, etc.).

**Steps to reproduce**:

    1. Open the Documentation Browser
    2. Click any tree item while the network is unreachable
       (or point to a nonexistent path)
    3. Debugger opens on MessageNotUnderstood

**Suggested fix**: Change `error message` to `error messageText`.

**Our workaround** (startup.st):

    MicDocumentBrowserModel compile: 'document
        resourceReference ifNil: [ ^ nil ].
        document ifNotNil: [ ^ document ].
        [ document := resourceReference loadMicrodown ]
            on: Error
            do: [ :error |
                document := Microdown parse:
                    ''# Error
    '', error messageText ].
        ^ document'.

(We also widen the handler from `MicResourceReferenceError` to `Error`
because network errors like `ConnectionTimedOut` are not subclasses of
`MicResourceReferenceError`.)

---

## Bug 3: MicDocumentBrowserPresenter >> childrenOf: missing outer error handling

**Class**: `MicDocumentBrowserPresenter` (package Microdown-RichTextComposer)
**Method**: `childrenOf:`

Stock source:

    childrenOf: aNode
        "I am a utility method to find children in a node"
        (aNode isKindOf: MicElement)
            ifTrue: [ ^ aNode subsections children].
        aNode loadChildren
            ifNotEmpty: [ :children |
                ^ children sort: [:a :b |
                    (self displayStringOf: a) < (self displayStringOf: b)] ]
            ifEmpty: [
                [ ^ self childrenOf:
                    (MicSectionBlock fromRoot: aNode loadMicrodown) ]
                on: Error
                do: [ ^ #() ]]

The `ifEmpty:` branch has an `on: Error do:` handler, but the
`ifNotEmpty:` branch (and the initial `aNode loadChildren` call itself)
does not. If `loadChildren` raises an exception before returning a
collection — e.g., DNS failure, socket timeout, JSON parse error — it
propagates uncaught and opens a debugger.

**Steps to reproduce**:

    1. Open the Documentation Browser
    2. Disconnect from the network (or block api.github.com)
    3. Click the triangle to expand a tree node
    4. Debugger opens on ConnectionTimedOut (or similar)

**Suggested fix**: Wrap the entire method body in `on: Error do:`:

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

---

## Bug 4: Menu shortcut modifier symbols render as "?" (missing font glyphs)

**Class**: `KMShortcutPrinter` / `KMOSXShortcutPrinter` (package Keymapping-Core)
**Method**: `KMShortcutPrinter class >> createSymbolTable`

The shortcut printer uses Unicode symbols for modifier keys:

    Modifier     Codepoint   Symbol   Unicode Name
    Cmd/Meta     U+2318      ⌘       PLACE OF INTEREST SIGN
    Alt/Option   U+2325      ⌥       OPTION KEY
    Ctrl         U+2303      ⌃       UP ARROWHEAD
    Shift        U+21E7      ⇧       UPWARDS WHITE ARROW
    Enter        U+23CE      ⏎       RETURN SYMBOL

The embedded `SourceSansProRegular` font (copyright 2010, 2012, from the
old SourceForge repo) does not contain any of these glyphs. FreeType
renders the `.notdef` glyph (a small box) which at small sizes looks
like "?". None of the other embedded fonts (Lucida Grande, Source Code
Pro) have these glyphs either.

Adobe added the macOS keyboard modifier glyphs to Source Sans Pro in
**version 2.040** (October 2018). The current version is 3.052 (April
2023). Pharo still ships the 2012 version.

**This affects ALL Pharo VMs on ALL platforms**, not just iospharo. The
stock Pharo 13 image on macOS with the official Pharo VM has the same
missing glyphs — they render as tiny `.notdef` rectangles.

**Steps to reproduce** (stock Pharo 13, any VM):

    1. Open any tool with keyboard shortcuts (System Browser, Playground)
    2. Right-click to open a context menu
    3. Menu items with shortcuts show "?D", "?S" etc. instead of "⌘D", "⌘S"

**Suggested fix**: Update `SourceSansProRegular` in the `EmbeddedFreeType`
package to Source Sans 3 (version 3.052). The font is SIL Open Font
Licensed and a drop-in replacement. This would fix shortcut display for
all Pharo users on all platforms.

**Our workaround** (startup.st):

    KMShortcutPrinter symbolTable
        at: #Cmd put: 'Cmd+';
        at: #Meta put: 'Cmd+';
        at: #Alt put: 'Opt+';
        at: #Ctrl put: 'Ctrl+';
        at: #Shift put: 'Shift+';
        at: #Enter put: 'Enter'.

This replaces the Unicode symbols with ASCII text labels so shortcuts
display as "Cmd+D", "Cmd+S", etc.

---

## Feature Request: Portrait layout / small-screen support

On an iPhone in portrait orientation (or any narrow window < ~500pt),
the Pharo menu bar does not adapt:

    Pharo | Browse | Debug | Sources | System | Library | Windows | Help

All 8 items render in a single row. On a 393pt-wide iPhone 16 screen,
the last 3-4 items are clipped. There is no wrapping, truncation, or
overflow menu. Standard tool windows (System Browser, Inspector,
Workspace) are designed for ~800pt minimum width and are unusable in
portrait.

This is the single biggest obstacle to using Pharo on a phone.

**Concrete suggestions** (from least to most effort):

  1. **Menu bar overflow**: If world width < 600pt, collapse the menu
     bar into a single button that opens a vertical list of all menus.
     This is a Morphic-only change — no VM or platform work needed.

  2. **Narrow window layouts for tools**: The System Browser currently
     uses a 4-pane horizontal layout (packages | classes | protocols |
     methods) with source below. In a narrow window, stack these as a
     drill-down navigation: tap a package to see its classes full-width,
     tap a class to see methods full-width, etc. Similar to how Xcode's
     navigator works on a narrow iPad split.

  3. **Size-class-aware layout engine**: Expose a `compactWidth` flag
     (true when world width < 600pt) to the Morphic layout system.
     Morphic layouts could query this and choose between horizontal and
     vertical arrangements. The VM already provides window dimensions
     via `SDL_GetWindowSize`; this just needs to be surfaced as a
     Morphic preference.

  4. **Full adaptive layout**: Adopt a constraint-based or responsive
     layout system (like iOS Auto Layout or CSS Flexbox) where panes
     specify minimum widths and the system automatically reflows.

Even just suggestion 1 (menu overflow button) would make portrait
orientation functional.

---

## Feature Request: Keyboard shortcut discoverability

Pharo relies on keyboard shortcuts that are invisible on touch devices:

    Cmd+D (Do It)    Cmd+P (Print It)    Cmd+I (Inspect It)
    Cmd+S (Accept)   Cmd+L (Cancel)      Cmd+B (Browse It)

Without a physical keyboard, there is no way to discover or invoke
these. iospharo adds a floating toolbar with Ctrl/Cmd modifier buttons,
but this only helps if you already know the shortcuts exist.

**Suggestion**: Add a context-sensitive action bar or long-press
radial menu that surfaces the 5-6 most common actions for the
current selection (Do It, Print It, Inspect It, Accept, Cancel,
Browse) as labeled tap targets. This could be a standard Morphic
widget that appears above any text editor morph.

---

## Feature Request: Touch-friendly scroll and selection

Morphic's event model assumes mouse input. On a touchscreen:

  - **Scrolling**: Drag gestures conflict with Morphic's morph-drag
    and text-selection handlers. Two-finger scroll works (via our VM's
    gesture recognizer), but single-finger scroll is natural on touch.

  - **Text selection**: Requires precise drag with no visible handles.
    iOS/Android text selection uses drag handles at both ends of the
    selection and a magnifier loupe. Morphic provides none of these.

  - **Zoom**: No pinch-to-zoom on code panes. On a phone-sized screen,
    code is too small to read without zooming.

**Suggestion**: A `TouchInputMode` preference that, when enabled:

  - Delegates scroll to the platform's native scroll physics
  - Shows selection handles on text selections
  - Supports pinch-to-zoom on text panes (scaling the font size
    or using a viewport transform)

---

## Bug 5: WarpBlt Smalltalk fallback drops alpha channel

**Class**: `WarpBlt` (package Graphics-Primitives)
**Method**: `mixPix:sourceMap:destMap:`

When the `primitiveWarpBits` C primitive is unavailable (as in our VM
prior to this fix), `WarpBlt >> warpBitsSmoothing:sourceMap:` falls back
to a Smalltalk pixel-by-pixel implementation. The `mixPix:` method
averages N×N source pixels for anti-aliasing but only averages the R, G,
B channels — it ignores the alpha channel (bits 24-31). The result has
alpha=0 (fully transparent).

This manifests when inspecting a Color (`Color red inspect`): the Color
tab shows a gray box instead of a colored rectangle. The inspector path
is:

    Morph new color: Color red; asFormOfSize: 50@50

`asFormOfSize:` calls `transformBy:clippingTo:during:smoothing: 2` which
uses WarpBlt with smoothing=2. The morph is drawn correctly onto a
sub-canvas (BitBlt fill, works fine), but the WarpBlt copy from
sub-canvas to the target form drops all alpha, producing `0x00FF0000`
instead of `0xFFFF0000`.

**Steps to reproduce** (any VM without primitiveWarpBits):

    1. `Color red inspect`
    2. Click the "Color" tab in the inspector
    3. The swatch rectangle is gray/transparent instead of red

**Root cause in code** (`mixPix:sourceMap:destMap:`):

    "Only R, G, B are summed — alpha is ignored"
    r := r + ((rgb bitShift: -16) bitAnd: 255).
    g := g + ((rgb bitShift: -8) bitAnd: 255).
    b := b + ((rgb bitShift: 0) bitAnd: 255).
    "...reassembled without alpha:"
    rgb := ((r // nPix bitShift: d) bitShift: bitsPerColor * 2)
         + ((g // nPix bitShift: d) bitShift: bitsPerColor)
         + ...

**Suggested fix**: Add alpha channel averaging:

    a := a + ((rgb bitShift: -24) bitAnd: 255).
    "...include in reassembly:"
    rgb := ((a // nPix bitShift: d) bitShift: bitsPerColor * 3) + ...

**Our fixes** (two-pronged):

  1. Implemented `primitiveWarpBits` in the C++ VM for 32-bit depth,
     so the buggy Smalltalk fallback is never reached.

  2. Patched `mixPix:sourceMap:destMap:` via startup.st to preserve
     alpha, as a safety net for any other code path that might hit
     the Smalltalk fallback.

---

## Bug 6: Doc browser bullet chars render as "?" (same font issue as Bug 4)

**Class**: `MicRichTextComposer` (package Microdown-RichTextComposer)
**Method**: `bulletForLevel:`

Stock source:

    bulletForLevel: level
        ^ ('•-' at: (level - 1 \\ 2) + 1) asText

The bullet character U+2022 (BULLET) is not in the embedded Source Sans
Pro v2.020 font (same root cause as Bug 4). FreeType renders `.notdef`
which shows as "?" at body text sizes.

**Steps to reproduce** (stock Pharo 13, any VM):

    1. Open Help > Microdown Document Browser
    2. Expand any document with unordered lists
    3. Bullet points show "?" instead of a bullet symbol

**Suggested fix**: Update the embedded Source Sans Pro font to v3.052
(same as Bug 4 — one fix resolves both).

**Our workaround** (startup.st):

    MicRichTextComposer compile: 'bulletForLevel: level
        ^ (''*-'' at: (level - 1 \\ 2) + 1) asText'.

Replaces the Unicode bullet with ASCII `*` for level 1, `-` for level 2.

---

## Fixed (Build 62): PMArbitraryPrecisionFloat transcendental math

Previously 12 of 58 tests failed. Root cause was `primitiveBitShift`
overflow: `4 bitShift: 126` returned 0 instead of 2^128 because the
`__int128` fast path overflowed. Fixed by checking magnitude bit-width
before using `__int128`. Now 57 pass, 0 fail, 1 error (testPrintAndEvaluate
times out on both our VM and the reference VM — not a correctness bug).

---

## Bug 7: DebugPointTest >> testTranscriptDebugPoint fails on all VMs

**Class**: `DebugPointTest` (package NewTools-Debugger-Breakpoints-Tools-Tests)
**Method**: `testTranscriptDebugPoint`

Stock source:

    testTranscriptDebugPoint
        self skipOnPharoCITestingEnvironment.
        dp := DebugPointManager
              installNew: DebugPoint
              on: node
              withBehaviors: { TranscriptBehavior }.
        dp text: 'string'.
        dp hitWithContext: context.
        self assert: Transcript contents equals: 'string'

Two bugs:

  1. **Missing Transcript clear**: The test asserts `Transcript contents`
     equals exactly `'string'`, but never clears Transcript first. By the
     time the test runs in a suite, Transcript has accumulated output from
     session startup, image loading, etc. The assertion sees
     `'Using authentification for Github API...<more stuff>string'`
     instead of `'string'`.

  2. **Fails headless**: In CLI/headless mode, `Transcript` is a
     `NonInteractiveTranscript` which does not implement `#contents`.
     The test gets `MessageNotUnderstood: NonInteractiveTranscript >> #contents`
     (ERROR, not FAIL). The `skipOnPharoCITestingEnvironment` guard only
     skips on CI, not all headless runs.

**Reproduction** (stock Pharo 13, official VM):

    Headless:
    $ pharo Pharo.image test DebugPointTest
    → testTranscriptDebugPoint: ERROR (NonInteractiveTranscript >> #contents)

    Interactive (with other tests already run):
    → testTranscriptDebugPoint: FAIL (Transcript has prior content)

**Result on our VM**: FAIL (we always run with interactive Transcript)
**Result on standard VM headless**: ERROR (NonInteractiveTranscript)

**Suggested fix** (either or both):

  (a) Add `Transcript clear` at the start of the test method:

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

  (b) Use `assert: includes:` instead of exact equality:

      self assert: (Transcript contents endsWith: 'string')

  (c) Guard against headless mode by checking
      `Transcript respondsTo: #contents` or skipping when Transcript
      is a `NonInteractiveTranscript`.

**No workaround needed** — this is a test bug, not a behavior bug.
It does not affect runtime behavior.

---

## Note: Cairo/Athens not available

Cairo (`libcairo`) is not included in the iospharo VM. All `cairo_*` FFI
calls resolve to a stub that returns 0. This means:

  - Athens (the Cairo-based canvas) silently fails — any rendering that
    goes through Athens produces no output.
  - The standard Pharo display pipeline uses OSSDL2Driver → BitBlt →
    FormCanvas, which does NOT use Cairo, so normal rendering is fine.
  - Some packages (e.g., Roassal visualizations) use Athens and will
    not render correctly.

The stub now logs the first 5 `cairo_*` function registrations to
stderr for diagnostic visibility.
