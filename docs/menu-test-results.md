# Menu Item Test Results (2026-02-23)

Programmatic test via `scripts/test_menu_items.st` — discovers all `<worldMenu>` pragmas
via PragmaMenuBuilder, executes each action in the Mac Catalyst app with 10s timeout.

**Summary: 47 pass, 2 fail, 6 error, 23 skip (of 78)**

## PASS (47)

| Menu | Item | Detail |
|------|------|--------|
| Pharo | Settings | 3→4 morphs |
| Browse | System Browser | 3→4 morphs |
| Browse | Git Repositories Browser | 3→4 morphs |
| Browse | Critique Browser | 3→4 morphs |
| Browse | Playground | 3→4 morphs |
| Browse | Transcript | 3→4 morphs |
| Browse | Dr Test | 3→4 morphs |
| Browse | Finder | 7→8 morphs |
| Debug | Debug Point Browser | 7→8 morphs |
| Debug | Remove all Debug Points | 7→7 morphs |
| Debug | Enable all Debug Points | 7→7 morphs |
| Debug | Disable all Debug Points | 7→7 morphs |
| Debug | Enable all break/inspect once | 7→7 morphs |
| Sources | Git Repositories Browser | 7→8 morphs |
| Sources | PackageDependencies | 7→8 morphs |
| Sources | Undo last refactoring | 7→8 morphs |
| Sources | Scopes Editor | 7→8 morphs |
| System | System Reporter | 7→8 morphs |
| System | Process Browser | 7→8 morphs |
| System | NewToolsMenu > NewFileBrowser | 7→8 morphs |
| System | NewToolsMenu > New Settings Browser | 7→8 morphs |
| System | NewToolsMenu > ObjectTranscript | 7→8 morphs |
| System | SystemStartup > SystemStartupLoader | 8→8 morphs |
| System | SystemStartup > SystemStartupCreator | 8→8 morphs |
| System | SystemStartup > SystemStartupFolder | 8→9 morphs |
| System | SystemStartup > SystemStartupFolder | 8→9 morphs |
| System | SystemFolders > SystemImageFolder | 8→8 morphs |
| System | SystemFolders > SystemVMFolder | 8→8 morphs |
| System | Space left | 8→9 morphs |
| Tools | Rewrite Rule Editor | 8→9 morphs |
| Tools | Expression Finder | 8→9 morphs |
| Tools | Match Tool | 8→9 morphs |
| Tools | Shortcuts Browser | 8→9 morphs |
| Tools | Icons Pack Manager | 8→9 morphs |
| Tools | Roassal > RoassalHelp > RoassalChat | 8→8 morphs |
| Tools | Roassal > RoassalHelp > RoassalDocumentation | 8→8 morphs |
| Tools | Roassal > RoassalSelf | 9→10 morphs |
| Windows | Reopen closed windows | 9→10 morphs |
| Windows | Expand all windows | 9→9 morphs |
| Windows | Fit all windows | 10→10 morphs |
| Windows | Close all debuggers | 10→6 morphs |
| Windows | Send top window to back (\) | 6→6 morphs |
| Help | Welcome to Pharo | 7→8 morphs |
| Help | Pharo Zen | 7→8 morphs |
| Help | Spec2 demo | 7→8 morphs |
| Help | Spec2 examples | 7→8 morphs |
| Help | About... | 7→8 morphs |

## FAIL (2) — Timeouts

| Menu | Item | Detail | Cause |
|------|------|--------|-------|
| Tools | Roassal > RoassalLoad > RoassalLoadFullVersion | timeout | Tries to download from network |
| Windows | Delete unchanged windows | timeout | Iterating many open morphs slowly |

## ERROR (6)

| Menu | Item | Error | Cause |
|------|------|-------|-------|
| Browse | Spotter | `a Heap() is empty` | Spotter internal issue |
| Sources | Code Changes | `Error creating new surface` | Cairo library not available |
| Tools | LoadToplo | `no error message set by libgit2` | libgit2 library not available |
| Tools | Roassal > RoassalLoad > RoassalExporters | `no error message set by libgit2` | libgit2 library not available |
| Windows | Collapse all windows | `receiver of "fitContents" is nil` | Morphic upstream issue |
| Help | Documentation Browser | `primitive #primStartLookupOfName: in NetNameResolver class failed` | Network DNS primitive not implemented |

## SKIP (23) — Dangerous items intentionally skipped

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
