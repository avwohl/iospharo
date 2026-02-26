# Work Log

## 2026-02-26 — Memory Leak Test

Opened and closed every window reachable from the Pharo menus, 10 times each,
then measured C++ heap leaks with macOS `leaks` tool.

### Test Setup

- Injected `scripts/leak_test_windows.st` into a fresh Pharo 13 image
- Script registers a startup handler that opens each window type 10× via
  Smalltalk (e.g. `ClyFullBrowserMorph open`, `StPlaygroundPresenter open`),
  closes it (`window close` for Morphs, `presenter window close` for Spec2),
  and logs results
- Ran headless via `test_load_image` (no display — Morphic runs offscreen)
- Also tested Mac Catalyst app with real Metal rendering

### C++ Leak Results

| Stage | Malloc Nodes | Memory | Leaks |
|---|---|---|---|
| Baseline (Pharo loaded) | 1,021,097 | 140 MB | **0** |
| After test (80 open/close cycles) | 1,219,311 | 146 MB | **0** |

Mac Catalyst app: 2 constant leaks (80 bytes) from startup — one `NSMenu`
(64 bytes) and one `NSSet` (16 bytes), both from AppKit menu swizzling. These
don't grow with window operations.

### Windows Tested

| Window | Open API | Close API | Result |
|---|---|---|---|
| System Browser | `ClyFullBrowserMorph open` | `close` | Clean |
| Playground | `StPlaygroundPresenter open` | `window close` | Clean |
| Transcript | `StTranscriptPresenter open` | `window close` | Clean |
| Inspector | `StInspectorPresenter inspect: 42` | `window close` | Clean |
| Settings | `SettingBrowser open` | `close` | Clean |
| Dr Test | `DrTests open` | `window close` | Clean |
| Finder | `StFinderPresenter open` | `window close` | Clean |
| Process Browser | `StProcessBrowser open` | `window close` | Clean |

Skipped: Spotter (kills Delay scheduler in headless mode), Critique Browser
(cascading hang without display).

### Sporadic Errors (not leaks)

A few Morphic timing races across 80 iterations (headless only):
- `receiver of "extent" is nil` — morph GC'd before close finishes
- `receiver of "hasPositiveExtent" is nil` — same race
- `index out of range` on Inspector close — sporadic

These don't occur in the Mac Catalyst app with a real display.

### Conclusion

No C++ memory leaks. The 6 MB heap growth is normal Smalltalk object
allocation. The 2 Mac Catalyst leaks (80 bytes total) are constant startup
artifacts from NSMenu swizzling — they don't grow.
