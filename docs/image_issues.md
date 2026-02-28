# Pharo Image Issues

Bugs and limitations in the stock Pharo 13 image that we work around
in iospharo's startup.st. Ideally these would be fixed upstream so the
startup.st patches can be removed.

Also includes feature requests that would improve Pharo on mobile/tablet.

---

## Bugs (patched by startup.st)

### 1. IceTokenCredentials sends bad auth to GitHub API

Fresh Pharo 13 images ship with `IceTokenCredentials` containing a
placeholder string `'YOUR TOKEN'`. When `MicGitHubAPI` fetches doc
tree data, it sends this as an Authorization header. GitHub returns
401 without `X-Ratelimit-Remaining`, and `extractRateInfo:` crashes
with `KeyNotFound`.

**Impact**: Documentation Browser tree is empty on first open.

**Our workaround**: Override `MicGitHubRessourceReference >> githubApi`
to return `MicGitHubAPI new beAnonymous` (no auth for public repos).

**Upstream fix**: Either ship with blank credentials (no header sent)
or have `MicGitHubAPI` fall back to anonymous when auth fails with 401.

### 2. MicDocumentBrowserModel >> document uses wrong error API

The error handler calls `error message` but the correct Pharo API is
`error messageText`. Causes a secondary `MessageNotUnderstood` crash
when any document fails to load.

**Our workaround**: Recompile `document` with `error messageText` and
wrap the error in a Microdown `# Error` heading for display.

**Upstream fix**: One-line change: `error message` -> `error messageText`.

### 3. MicDocumentBrowserPresenter >> childrenOf: has no error handling

Expanding tree nodes calls `childrenOf:` which makes network requests
with zero error handling. DNS failures, timeouts, rate limits, or
malformed responses all cause unhandled exceptions that crash the
browser.

**Our workaround**: Recompile with `on: Error do: [ ^ #() ]` wrappers
around all network calls.

**Upstream fix**: Add error handling to `childrenOf:`. Return empty
children and optionally display a retry indicator on the node.

---

## Feature Requests

### 4. Portrait layout / small-screen support

On an iPhone in portrait orientation (or any narrow window), the Pharo
menu bar (`Pharo | Browse | Debug | Sources | System | Library |
Windows | Help`) does not wrap, condense, or adapt. Menu items are
clipped or overlap, and standard Pharo windows (browsers, inspectors,
workspaces) don't fit in the narrow width.

This is the single biggest obstacle to using Pharo on a phone.

**What would help**:

  - A layout system aware of available screen size, similar to how iOS
    and macOS apps use size classes (compact vs regular) to switch
    between layouts.

  - In compact/portrait mode:
      - Collapse the menu bar into a hamburger menu or a single-row
        icon strip.
      - Stack tool panes vertically instead of side-by-side (e.g.,
        browser: class list above method list above source, not three
        columns).
      - Use full-width sheets or navigation-style push for sub-panels
        instead of floating windows.

  - Respect `UITraitCollection` horizontal size class when running
    under Mac Catalyst / UIKit. Pharo already gets the window bounds
    from `SDL_GetWindowSize`; it could derive a compact/regular flag
    and expose it to the Morphic layout engine.

  - Even a minimal first step would help: if the world width is below
    some threshold (e.g., 500px), switch the menu bar to a pop-up menu
    triggered by a single button.

### 5. Keyboard shortcut discoverability on touch devices

Pharo relies heavily on keyboard shortcuts (Cmd+D, Cmd+P, Cmd+E,
etc.) that are invisible on a touch device. There is no on-screen
hint of what shortcuts exist or how to invoke them without a physical
keyboard.

**What would help**: A discoverable shortcut palette or long-press
radial menu that exposes the most common actions (Do It, Print It,
Inspect It, Accept, Cancel) as tap targets.

### 6. Touch-friendly scroll and selection

Pharo's Morphic event handling assumes a mouse with precise pixel
positioning and a scroll wheel. On a touch screen:

  - Text selection requires long-press + drag, but there are no visible
    selection handles.
  - Scrolling in lists and text panes conflicts with Morphic's own
    drag handling.
  - There is no pinch-to-zoom for code panes (useful on small screens).

**What would help**: A touch input mode that uses platform-native
gestures (UIKit scroll views, selection handles, pinch-to-zoom) when
running on a touch device.
