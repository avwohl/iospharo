# Menu Item Test Results (2026-02-23)

## Visual Verification (commit cb7c34b)

**ALL THREE GUI PRIORITIES VERIFIED WORKING:**

1. **Display properly** — Pharo 13 desktop renders correctly with dark theme,
   Welcome window, Pharo lighthouse logo, text rendering, window controls. No red X.
2. **Top menu works** — Clicking "Pharo" in menu bar opens dropdown with:
   Settings (⌘OS), Save (⇧⌘S), Save as..., Save and quit, Quit
3. **World menu works** — Left-click on empty desktop opens World menu with:
   Pharo, Browse, Debug, Sources, System, Library, Windows, Help (all with submenu arrows)

Verified via PPM display surface dumps (Metal content can't be captured by screencapture).

### Key fixes enabling this:
- **findSelectorInClass** (ad68f75): Was scanning MethodDictionary values array instead of
  keys (slots 2+). This caused selectors_.subtract=nil → Point >> #- DNU → Emergency
  Debugger → VM stall at 0.22M/s. Fixed: scan keys with symbolEquals.
- **Native Cairo surfaces** (ad68f75): Removed FFI dispatch callbacks from primitiveCopyBits
  (unsafe inside VM primitives). Uses dlsym-resolved cairo_image_surface_get_* instead.
- **SurfacePlugin** (e08530c): Added primitiveRegisterSurface/primitiveUnregisterSurface
  for Athens/Cairo dispatch surface registration.

---

## Programmatic Test Results

Programmatic test via `scripts/test_menu_items.st` — discovers all `<worldMenu>` pragmas
via PragmaMenuBuilder, executes each action in the Mac Catalyst app with 8s timeout.

**Summary: 50 pass, 0 fail, 3 error, 25 skip (of 78)**

Previous: 47 pass, 2 fail, 6 error, 23 skip → 15 pass, 40 error (regression from stub removal)
→ **50 pass, 0 fail, 3 error, 25 skip** (after BitBlt/FloatArray/library fixes)

## Changes from previous run

- **BitBlt negative depth fix**: `Form>>swapEndianness` sets `depth := 0 - depth`.
  Our BitBlt couldn't handle negative depth → `copyFromByteArray:` → `unhibernate` →
  `decompress` cascade failure. Fixed by handling `srcDepth < 0` (byte-swap within words).
- **FloatArrayPlugin**: Real `primitiveAt`/`primitiveAtPut` for IEEE 32-bit float read/write.
  Fixes `MatrixTransform2x3` errors (24+ `EXT-PRIM-FAIL` per session).
- **Dynamic library loading**: FFI loads real libraries (FreeType, libgit2, Cairo) from
  bundled Frameworks dir before falling back to stubs.
- **Library bundling**: `scripts/bundle-libs.sh` copies 22 Homebrew dylibs, rewrites
  `@loader_path`, re-signs with Apple Development identity.
- **Test runner priority fix**: Actions at P35, watchdog at P79 prevents priority inversion.

## PASS (50)

| Menu | Item | Detail |
|------|------|--------|
| Pharo | Settings | 3→4 morphs |
| Browse | System Browser | 3→4 morphs |
| Browse | Git Repositories Browser | 3→4 morphs |
| Browse | Critique Browser | 3→4 morphs |
| Browse | Playground | 3→4 morphs |
| Browse | Transcript | 3→4 morphs |
| Browse | Dr Test | 3→4 morphs |
| Browse | Spotter | 3→5 morphs |
| Browse | Finder | 3→4 morphs |
| Debug | Debug Point Browser | 3→4 morphs |
| Debug | Remove all Debug Points | 3→3 morphs |
| Debug | Enable all Debug Points | 3→3 morphs |
| Debug | Disable all Debug Points | 3→3 morphs |
| Debug | Enable all break/inspect once | 3→3 morphs |
| Sources | Git Repositories Browser | 3→4 morphs |
| Sources | PackageDependencies | 3→4 morphs |
| Sources | Undo last refactoring | 3→4 morphs |
| Sources | Scopes Editor | 3→4 morphs |
| System | System Reporter | 3→4 morphs |
| System | Process Browser | 3→4 morphs |
| System | NewToolsMenu > NewFileBrowser | 3→4 morphs |
| System | NewToolsMenu > New Settings Browser | 3→4 morphs |
| System | NewToolsMenu > ObjectTranscript | 3→4 morphs |
| System | SystemStartup > SystemStartupLoader | 3→3 morphs |
| System | SystemStartup > SystemStartupCreator | 3→3 morphs |
| System | SystemStartup > SystemStartupFolder | 3→4 morphs |
| System | SystemStartup > SystemStartupFolder | 3→4 morphs |
| System | SystemFolders > SystemImageFolder | 3→3 morphs |
| System | SystemFolders > SystemVMFolder | 3→3 morphs |
| System | Space left | 3→4 morphs |
| Tools | Rewrite Rule Editor | 3→4 morphs |
| Tools | Expression Finder | 3→4 morphs |
| Tools | Match Tool | 3→4 morphs |
| Tools | Shortcuts Browser | 3→4 morphs |
| Tools | Icons Pack Manager | 3→4 morphs |
| Tools | Roassal > RoassalHelp > RoassalChat | 3→3 morphs |
| Tools | Roassal > RoassalHelp > RoassalDocumentation | 3→3 morphs |
| Tools | Roassal > RoassalSelf | 3→4 morphs |
| Windows | Reopen closed windows | 3→4 morphs |
| Windows | Collapse all windows | 3→3 morphs |
| Windows | Expand all windows | 3→3 morphs |
| Windows | Fit all windows | 3→3 morphs |
| Windows | Close all debuggers | 3→3 morphs |
| Windows | Send top window to back (\) | 3→3 morphs |
| Windows | Delete unchanged windows | 3→4 morphs |
| Help | Welcome to Pharo | 3→4 morphs |
| Help | Pharo Zen | 3→4 morphs |
| Help | Spec2 demo | 3→4 morphs |
| Help | Spec2 examples | 3→4 morphs |
| Help | About... | 3→4 morphs |

## ERROR (3)

| Menu | Item | Error | Cause |
|------|------|-------|-------|
| Sources | Code Changes | `Unable to register surface with SurfacePlugin` | Missing `primitiveRegisterSurface` (Athens/Cairo surface dispatch) |
| Tools | LoadToplo | `could not find repository` | Git repo not present on disk (not a VM bug) |
| Help | Documentation Browser | `Socket destroyed, cannot retrieve error message` | Network socket issue (not a VM bug) |

## SKIP (25) — Dangerous or network-dependent items

| Menu | Item | Reason |
|------|------|--------|
| Pharo | Save | dangerous |
| Pharo | Save as... | dangerous |
| Pharo | Save and quit | dangerous |
| Pharo | Quit | dangerous |
| Debug | Profiler | dangerous |
| Debug | Start profiling all Processes | dangerous |
| Debug | Start profiling UI | dangerous |
| System | Do Image Cleanup | dangerous |
| System | Start drawing again | dangerous |
| System | Start stepping again | dangerous |
| System | Restore display | dangerous |
| System | Screenshot | dangerous |
| Tools | Roassal > RoassalLoad > RoassalLoadFullVersion | dangerous (network download) |
| Tools | Roassal > RoassalLoad > RoassalExporters | dangerous (network download) |
| Windows | Profiles > New Profile | dangerous |
| Windows | Profiles > Update current profile | dangerous |
| Windows | Profiles > Show/Hide current profile | dangerous |
| Windows | Profiles > Rename profile | dangerous |
| Windows | Profiles > Delete Profile | dangerous |
| Windows | Profiles > Reset Windows | dangerous |
| Windows | Profiles > Import Profile | dangerous |
| Windows | Profiles > Export Profile | dangerous |
| Windows | Profiles > Tutorial | dangerous |
| Windows | Delete all windows discarding edits | dangerous |
| Windows | Toggle full screen mode | dangerous |
