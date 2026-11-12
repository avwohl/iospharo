# Startup patch system

The app patches the Pharo image at every start.
The patches fix image defects that appear when the image runs on this VM.
The Swift function `PharoBridge.writeStartupScript(to:)` in
`iospharo/Bridge/PharoBridge.swift` generates the patch files.
This document describes the files, the version dispatch and each patch.

## How Pharo loads the files

Pharo has a class `StartupPreferencesLoader`.
On every image start, this class loads the file `startup.st` from the
current directory.
Before the VM starts, the app changes the current directory to the
directory of the image file.
Then the app writes the generated files next to the image.
The app overwrites the generated files at every start.
Do not edit them.

## The files

The app creates three files:

- `startup.st` is the dispatcher.
- `startup-13.st` holds the common patches and the Pharo 13 patches.
- `startup-14.st` holds the common patches and the Pharo 14 patches.

You can add a fourth file, `startup-user.st`, in the image directory.
The dispatcher loads this file after the generated patches.
The app never writes or deletes `startup-user.st`.

## Version dispatch

The dispatcher does these steps in order:

1. It sets the flag `bitBltSubPixelAvailable` of `FreeTypeSettings` to false.
   This step runs first. A file operation can yield the process, and then
   the render loop can run before the flag is set.
   The sub-pixel renderer then fails on a nil value.
2. It reads `SystemVersion current major`.
3. It loads `startup-14.st` when the major version is 14 or higher.
   Pharo 15 images therefore use the Pharo 14 file.
4. Otherwise it loads `startup-13.st`.
5. If the selected file is missing, it prints a warning on stderr.
6. It loads `startup-user.st` if the file exists.

Each file prints a line on stderr when it starts and when it ends.
The line starts with `[startup-13]` or `[startup-14]`.

## Common patches

The common patches are in both `startup-13.st` and `startup-14.st`.
Each patch checks that its target class exists, with
`Smalltalk hasClassNamed:`, unless the table says otherwise.

FreeTypeSettings sub-pixel flag.
The VM runs the sub-pixel primitive as a normal copy operation.
The image detects the sub-pixel feature with a test that expects a failure.
The patch sets the flag to false, so the test never runs.

SDLOSXPlatform release patches.
The methods `allowTouchpadInertia` and `afterSetWindowTitle:onWindow:` call
`release:` on a string from `ObjCLibrary>>nsStringOf:`.
The autorelease pool owns the string, so the caller does not own it.
The extra release makes the pool release the string a second time,
and the app crashes.
The patch recompiles both methods without the `release:` call.

Upstream Pharo has the same defect but does not crash,
because it never drains the pool.
See `docs/image_issues.md`.
The original methods are the same in Pharo 13 and Pharo 14.

MicGitHubRessourceReference `githubApi`.
The documentation browser uses `IceTokenCredentials`, which holds a
placeholder token.
GitHub answers with status 401, and the rate-limit parser fails.
The patch makes the method answer `MicGitHubAPI new beAnonymous`.

MicDocumentBrowserModel `document`.
The patch catches `Error` and uses `messageText`.
The original method catches only `MicResourceReferenceError`.
The patch shows an error page for more kinds of failure.

MicDocumentBrowserPresenter `childrenOf:`.
The patch catches API errors and answers an empty collection.
The original method has no error handler.

KMShortcutPrinter symbol table.
The embedded Source Sans Pro font is from 2012.
It has no glyphs for the menu symbols such as U+2318.
The patch maps the shortcut names to the plain words `Cmd+`, `Opt+`,
`Ctrl+`, `Shift+` and `Enter`.

Morph `fullDrawOn:`.
The patch catches errors that a morph raises while it draws, except `PrimitiveFailed`.
It writes the error and the stack to stderr and to `draw_errors.txt`.
It then marks the morph with the property `#errorOnDraw`.

The image uses `PrimitiveFailed` for feature detection, so the patch must
not catch it.

Morph `drawErrorOn:`.
The patch draws the error text and the stack in the red error box.

SystemWindow `openInWorld:`.
The patch moves a new window below the menu bar when the window overlaps it.
Pharo 14 does not define `openInWorld:` in `SystemWindow`.
The patch then adds a new method that calls `super`.

Two background processes.
The first process waits 500 ms and moves the existing windows below the
menu bar.

The second process waits 800 ms and clears stale `#errorOnDraw` marks
from morphs.
It also clears the caches of all `FreeTypeFont` objects.

SHA256 `hashMessage:`.
The kernel SHA256 is Smalltalk code and is slow.
The patch compiles `SHA256 class>>hashMessage:` with the primitive
`primitiveSHA256Message` of the module `DSAPrims`.
If the primitive fails, the method calls the Smalltalk code.

Pharo 14 has no `hashMessage:` method in `SHA256` itself.
It inherits the method from `HashFunction`, and the patch adds the new one.
On 2026-11-12 this VM and the stock Cog VM both answered
`ba7816bf...f20015ad` for the hash of `abc` on Pharo 14.
The answer matches the known SHA-256 value.

The patch is inside `[ ... ] on: Error do: [ ]`, so an error does not stop
the startup file.

## Pharo 13 patches

These patches are only in `startup-13.st`.

MicRichTextComposer `bulletForLevel:`.
The patch replaces the bullet character U+2022 with `*`, because the
embedded font has no glyph for it.
The class `MicRichTextComposer` has no method `bulletForLevel:` in Pharo 13
and in Pharo 14.
The composer asks its `textStyler`, which is an instance of
`MicTextStyler` or `MicDynamicTextStyler`.
The patch therefore has no effect today.

It stays in the Pharo 13 file only.
A working patch must change `MicTextStyler>>bulletForLevel:`.
Check first on screen that the glyph is missing.

WarpBlt `mixPix:sourceMap:destMap:`.
The Pharo 13 method drops the alpha channel.
The color swatch in the Color inspector becomes transparent.
The patch adds the alpha sum.
The Pharo 14 method has the same alpha sum, so Pharo 14 does not need the patch.

SmalltalkImage `systemInformationString`.
The patch adds the VM disclaimer and the source link to the About window.
Pharo 14 has no such method.

Stale doc browser windows.
A background process waits 3 seconds.
It then sends `updateTree` to each `MicDocumentBrowserPresenter`.
This refreshes windows that a saved session restored.

## Pharo 14 patches

These patches are only in `startup-14.st`.

StPharoSettings class `openPharoAbout`.
The patch adds the VM disclaimer and the source link to the About dialog.
The patch sends `message:` to `SpInformDialog`.
In Pharo 14, the method `label:` is obsolete and forwards to `message:`.

OSSDL2FormRenderer `outputExtent`.
Pharo 14 starts the Morphic render loop before the SDL renderer exists.
The original method sends a message to a nil renderer.
The patch calls `validate` when the renderer is nil.
If the renderer is still nil, the patch answers `0@0`.

Stale doc browser windows.
This patch is the same as the Pharo 13 patch, but it sends
`updateSourcePresenter`.
Pharo 14 removed `updateTree`.

## How to check a patch

Do not trust a patch only because the file loads.
Compile the patch in a copy of the target image, then read the method.
For example, with a copy of a pristine image in a clean directory:

```
build-rel/test_load_image Pharo.image eval \
  "(SDLOSXPlatform >> #allowTouchpadInertia) sourceCode"
```

Compare the source of the original method with the patch.
A patch is dead when its target method does not exist, or when the
original method already has the fix.
Remove dead patches from the file for that version.

You can extract the generated files from the Swift source and load them
with `fileIn` in a copy of the image.
The check on 2026-11-12 loaded `startup-13.st` into Pharo 13.1 and
`startup-14.st` into Pharo 14.0 without errors.

## Related files

- `iospharo/Bridge/PharoBridge.swift` holds the patch text.
- `docs/image_issues.md` explains the image defects in more detail.
- `resources/startup.st` is an old Pharo 13 patch file.
  No build file or script loads it. The Swift generator replaced it.
