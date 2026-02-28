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
