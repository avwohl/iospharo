# WIP — Windows JIT parity (branch `jit`)

Last updated: 2026-06-28. ~54 commits ahead of `origin/jit`. Build is green.
(Replaces the older 2026-06-23 perf-tracking resume point.)

## DONE & verified (milestones 1–3 + most of 4)
- **File subsystem 100%** — long-path `\\?\`; `DiskFileSystemTest` 59/59.
- **FFI working** — `getSystemAttribute 1003` = CPU arch; `FFICalloutMethodBuilderTest` 10/10.
- **Platform-path UTF-16LE** — `DiskFileAttributesTest` +2.
- **TCP sockets** — winsock2 `SocketPlugin` port + `WSAStartup` wiring; ~80+ tests recovered; `TCPSocketTest` 9/9.
- **Crypto/TLS — HTTPS works** — `ZnClient get: 'https://example.com'` → 200.
- **GUI RENDER + on-screen window WORK** — the Pharo Morphic desktop renders into a real Win32 window. Evidence: `docs/images/windows-gui-*.png`.

## GUI — current exact state (the live work)
Reproduce a windowed GUI:
1. Put any file named `SDL2.dll` in the IMAGE dir (`C:/temp/pharo-win-test/`). The
   `FFIWindowsLibraryFinder` only checks the image dir; our FFI routes SDL symbols
   to the built-in stubs so the file just needs to exist. Flips
   `SDL2 isAvailable`/`OSSDL2Driver isSuitable` true.
2. `PHARO_GUI_WINDOW=1 build-win/test_load_image.exe C:/temp/pharo-win-test/Pharo.image`
   (NO eval args → interactive mode → harness passes `--interactive` → the image
   auto-picks `OSWorldRenderer` and draws the World). Window comes up showing the
   live desktop. **No manual activation eval needed.**
   - Headless render PROOF without a window: add `PHARO_FORCE_DISPLAY=1
     PHARO_DUMP_DISPLAY=1` and `eval` `scripts/win-gui-render-check.st` → dumps
     `/tmp/vm-display-{20,60,150}.ppm` (changed=1).

### What WORKS
- Render: World → OSWorldRenderer → FFI SDL2 stubs → gDisplaySurface (Pharo
  BitBlts directly into gDisplaySurface via the LockTexture stub; ~122
  RenderPresents/run).
- On-screen window: `src/platform/Win32DisplaySurface.hpp` (GDI StretchDIBits,
  top-down 32bpp BI_RGB DIB — XRGB == BGRA on little-endian).
- **Responsive window** (UNCOMMITTED change just made): the HWND now lives on a
  DEDICATED THREAD with a `GetMessage` loop. Was pumping in `update()` only → a
  static World froze → "Not Responding". Now verified `IsHungAppWindow=False`.
  `update()`/`invalidateRect` just `InvalidateRect` → WM_PAINT on the window thread.

### NOT working yet — THE remaining problem (user: "right click doesnt do anything")
- The wndProc translates WM_MOUSE*/WM_KEY*/WM_CHAR into `pharo::Event` and pushes
  to `pharo::gEventQueue`, using the SAME encoding as the proven `injectMouseClick`
  in `test_load_image.cpp:124` (`type=Mouse(1)`, arg1=x, arg2=y, arg3=buttons
  Red=4/Yellow=2/Blue=1, arg4=mods, arg5=subtype 1=down/2=up/3=move).
- BUT real clicks on the window do nothing visible. The harness's SYNTHETIC
  `injectMouseClick(50,300,2)` at 5s IS proven to drive the World menu — so the
  queue→image path works. Likely causes (investigate in this order):
  1. **Input semaphore / SDL2-polling flag not set** — `stub_SDL_PollEvent`
     (FFI.cpp ~931) only returns events when `gEventQueue.isSDL2EventPollingActive()`
     is true; otherwise events drain to `passThroughEvents_`. injectMouseClick may
     run after the flag is set; a window-thread push before/independent of that may
     be dropped. Check `EventQueue::push` (src/platform/EventQueue.cpp) — does it
     fire the callback / signal `inputSemaphoreIndex_` / `sdl2InputSemaphoreIndex_`?
     Replicate whatever injectMouseClick relies on (it just calls push, so push must
     do the signalling — confirm).
  2. **Coordinate mapping** — window client may not be exactly 1024×768
     (AdjustWindowRect). Compare `GetClientRect` to 1024×768; scale/offset the
     click x/y to surface coords if different.
  3. **eventLoop must be the consumer** — confirm `OSSDL2Driver>>eventLoop` is
     running in THIS windowed interactive run (it ran in the earlier no-window
     interactive run: `[DIAG] OSSDL2Driver>>eventLoop`).
  4. **Move-before-click** — Morphic often needs a mouse-MOVE to position the hand
     before a click lands on the right morph; the wndProc does send WM_MOUSEMOVE,
     confirm they flow.

### Next steps (in order)
1. Temp-trace `Win32DisplaySurface::pushMouse` AND `stub_SDL_PollEvent` (FFI.cpp
   ~931) — confirm real clicks reach the queue AND get polled (vs the synthetic
   path). This will immediately show which of #1–#4 it is.
2. Read `src/platform/EventQueue.cpp` `push()` — make the window-thread push do
   exactly what injectMouseClick's push does (semaphore/callback).
3. Fix client→surface coords if needed.
4. Verify: left-click "Pharo" menu (client ~45,11) → dropdown appears. PowerShell
   click+screenshot scripts are in `/c/tmp/menu.ps1`, `rclick2.ps1` (use
   Get-Process MainWindowHandle + mouse_event; FindWindow didn't match).

## Uncommitted change to COMMIT
- `src/platform/Win32DisplaySurface.hpp` — window-thread + WM_PAINT blit + event
  injection (builds clean; window responsive; real-input wiring not yet confirmed).

## Everything-not-100% catalog
`docs/deferred.md` is the comprehensive list (file/FFI/sockets/crypto all green;
GUI items A=window DONE, B=auto-activate DONE, C=event injection IN PROGRESS; plus
sound/MIDI/clipboard/profiler/backtrace/symlink/nLink/UDP stubs, the socket
read-path EOF needing POSIX verification, FileAttributesPluginPrimsTest
error-fidelity). Memory: `windows-port-milestones.md`.

## Build / run
- Build (MSYS2 CLANG64): `scripts/build-windows.sh test_load_image`.
- Run from a NATIVE shell (not the MSYS login shell — it strips USERPROFILE).
- Image: `C:/temp/pharo-win-test/Pharo.image`; reference VM:
  `C:/temp/pharo-win-test/refvm/pharo-vm/PharoConsole.exe`.
- SDL2.dll currently staged at `C:/temp/pharo-win-test/SDL2.dll` (a copied DLL).
