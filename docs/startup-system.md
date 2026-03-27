# Startup Patch System

The app applies Smalltalk patches to the running image on every launch via
Pharo's built-in `StartupPreferencesLoader`.  This document explains why
the patches exist, how version detection works, and how to add your own.

## Why patches are needed

Stock Pharo images are built for the official Cog/Spur VM on Linux, macOS,
and Windows.  Our VM differs in several ways:

  - SDL2 is stubbed (real rendering goes through Metal)
  - Some text-rendering primitives behave differently
  - AppKit APIs (NSMenu, etc.) crash on Mac Catalyst
  - The embedded Source Sans Pro font is missing several Unicode glyphs
  - IceTokenCredentials ships with a placeholder token that breaks GitHub API calls

Rather than maintaining a patched Pharo image, the app writes Smalltalk
patch files next to the `.image` file before launching the VM.  Pharo's
`StartupPreferencesLoader` picks up `startup.st` automatically.  This
means any standard Pharo 13 or 14 image works out of the box.


## File layout

The app generates three files in the image directory:

    <image-dir>/
      Pharo.image
      Pharo.changes
      startup.st          Dispatcher (detects version, loads the right file)
      startup-13.st       All patches for Pharo 13
      startup-14.st       All patches for Pharo 14
      startup-user.st     (optional) Your custom patches — never overwritten

`startup.st`, `startup-13.st`, and `startup-14.st` are regenerated on
every launch.  Do not edit them — your changes will be lost.

`startup-user.st` is loaded last and is never overwritten by the app.
Create this file to add your own patches.


## How version detection works

There are two version-detection mechanisms, at different layers:

**Smalltalk side (startup.st dispatcher):**

    version := SystemVersion current major.

This returns the integer major version (13, 14, ...).  The dispatcher uses
`version >= 14` to choose between `startup-13.st` and `startup-14.st`.
This runs inside the image, so it's authoritative.

**Swift side (image library UI):**

    guessPharoVersion(from: filename)

Uses the regex `[Pp]haro\s*(\d{1,2})` on the image filename (e.g.
`Pharo13.1-64bit-xxx.image` -> `"13"`).  This is a cosmetic hint used in
the image library table — it does not affect which patches are loaded.
If the filename doesn't match the pattern, the version column shows "--".


## Where images come from

Images are downloaded from the official Pharo file server.  The app has
two built-in templates hardcoded in `ImageManager.swift`:

    Pharo 13 (stable)   https://files.pharo.org/get-files/130/pharoImage-arm64.zip
    Pharo 14 (dev)       https://files.pharo.org/get-files/140/pharoImage-arm64.zip

Users can also import `.image` files from the Files app or via drag-and-drop.
Imported images get the same startup patch treatment.


## What each patch does

### Common patches (both P13 and P14)

  Patch                                       Why
  ------------------------------------------  -------------------------------------------
  FreeTypeSettings bitBltSubPixelAvailable     Disables sub-pixel text detection test that
    := false                                   triggers PrimitiveFailed on some code paths

  MicGitHubRessourceReference >> githubApi     Forces anonymous GitHub API (IceTokenCredentials
    → beAnonymous                              has a placeholder token that causes 401 errors)

  MicDocumentBrowserModel >> document          Uses messageText instead of message in error
                                               handler (prevents MessageNotUnderstood)

  MicDocumentBrowserPresenter >> childrenOf:   Wraps API calls in error handlers to prevent
                                               doc browser crashes on network errors

  KMShortcutPrinter symbolTable               Replaces Unicode symbols (U+2318 etc.) with
                                               ASCII text — embedded font lacks those glyphs

  MicRichTextComposer >> bulletForLevel:       Replaces Unicode bullet (U+2022) with ASCII
                                               * and - for the same font reason

  WarpBlt >> mixPix:sourceMap:destMap:         Preserves alpha channel in the Smalltalk
                                               fallback (fixes transparent Color swatches)

  Morph >> fullDrawOn:                         Error handler that logs to stderr and file,
                                               excludes PrimitiveFailed from being caught

  Morph >> drawErrorOn:                        Shows error text in the red error rectangle
                                               instead of just a blank red box

  SystemWindow >> openInWorld:                 Repositions windows that open under the
                                               Pharo menu bar

  Reposition existing windows                  (forked) Moves any windows overlapping the
                                               menu bar after 500ms

  Clear errorOnDraw marks                      (forked) After 800ms, clears transient
                                               startup errors and resets font caches


### Pharo 13-only patches

  Patch                                       Why
  ------------------------------------------  -------------------------------------------
  SmalltalkImage >> systemInformationString    About window shows "Pharo Smalltalk" VM
                                               disclaimer and GitHub link

  MicDocumentBrowserPresenter refresh          Refreshes stale doc browser windows from
    via updateTree                             previously saved sessions


### Pharo 14-only patches

  Patch                                       Why
  ------------------------------------------  -------------------------------------------
  StPharoSettings class >> openPharoAbout      About dialog shows VM disclaimer (P14 moved
                                               the about window to a different class)

  OSSDL2FormRenderer >> outputExtent           Guards against nil renderer during early
                                               startup (P14 starts MorphicRenderLoop before
                                               SDL renderer is created)

  MicDocumentBrowserPresenter refresh          Same stale-window fix as P13, but uses
    via updateSourcePresenter                  updateSourcePresenter (updateTree was removed)


## Adding custom patches

Create `startup-user.st` in the same directory as the `.image` file:

    "startup-user.st — My custom patches"
    Stdio stderr nextPutAll: '[user] Custom patches loaded'; lf; flush.

    "Example: increase the default font size"
    StandardFonts defaultFont: (LogicalFont familyName: 'Source Sans Pro' pointSize: 14).

This file is loaded after the version-specific patches, so you can override
anything they set.  It is never overwritten by the app.

To find the image directory:
  - **Mac Catalyst**: Right-click an image in the library → "Show in Finder"
  - **iOS**: Use the Files app → On My iPad → Pharo Smalltalk → Images
  - **CLI**: Pass `--image /path/to/Pharo.image` — patches are written next to it


## Technical details

The startup patch system is implemented in `PharoBridge.swift`:

    PharoBridge.writeStartupScript(to: imageDir)

This is called from `launchVM(imagePath:)` before `vm_init()`.  The working
directory is set to the image directory so that `FileSystem workingDirectory`
in Smalltalk resolves to the same path.

`test_load_image` (the CLI test binary) does the same `chdir` but does not
generate the startup files — they must already exist next to the image.

For the list of image bugs these patches work around, see
`docs/image_issues.md`.  For upstream fix proposals, see
`docs/upstream-proposals.md`.
