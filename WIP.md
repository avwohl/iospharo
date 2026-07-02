# WIP — Windows JIT parity (branch `jit`)

Last updated: 2026-07-01. Build is green.

## DONE & verified (milestones 1–4)
- **File subsystem 100%** — long-path `\\?\`; `DiskFileSystemTest` 59/59.
- **FFI working** — `getSystemAttribute 1003` = CPU arch; `FFICalloutMethodBuilderTest` 10/10.
- **Platform-path UTF-16LE** — `DiskFileAttributesTest` +2.
- **TCP sockets** — winsock2 `SocketPlugin` port + `WSAStartup` wiring; ~80+ tests recovered; `TCPSocketTest` 9/9.
- **Crypto/TLS — HTTPS works** — `ZnClient get: 'https://example.com'` → 200.
- **GUI RENDER + on-screen window WORK** — the Pharo Morphic desktop renders into a real Win32 window. Evidence: `docs/images/windows-gui-*.png`.
- **GUI INPUT WORKS (2026-07-01)** — real mouse + keyboard drive the image:
  menubar menus open (Pharo/Browse dropdowns), World menu on right-click,
  Playground opens and accepts typed text, code evaluation runs, a DNU opens
  the full debugger (stack/inspector/Create-method), Profiler tool opens from
  a context menu. Evidence: `docs/images/windows-gui-menu-click.png`,
  `docs/images/windows-gui-debugger.png`. Verified via posted WM_* messages to
  the live HWND + PrintWindow screenshots (SetForegroundWindow is blocked from
  a background automation shell — that's a test-harness limitation, not a VM one).

## GUI — how to reproduce a windowed interactive session
1. Put any file named `SDL2.dll` in the IMAGE dir (`C:/temp/pharo-win-test/`). The
   `FFIWindowsLibraryFinder` only checks the image dir; our FFI routes SDL symbols
   to the built-in stubs so the file just needs to exist. Flips
   `SDL2 isAvailable`/`OSSDL2Driver isSuitable` true.
2. `PHARO_GUI_WINDOW=1 build-win/test_load_image.exe C:/temp/pharo-win-test/Pharo.image`
   (NO eval args → interactive mode → the image auto-picks `OSWorldRenderer`).
   Add `PHARO_WIN_EVENT_TRACE=1` to trace the input pipeline (wndProc push →
   stub_SDL_PollEvent delivery).

### The two bugs that made "clicks do nothing" (both fixed 2026-07-01)
1. **Stale eval-mode `startup.st`** (the big one): every eval run writes a
   `startup.st` next to the image whose preamble SUSPENDS all Morphic processes.
   Interactive runs auto-load it (StartupPreferencesLoader) → desktop renders but
   the UI event loop is dead → all input silently ignored. Fix: non-eval runs now
   detect the `[STARTUP-ST-FIRED]` marker and delete the stale file
   (test_load_image.cpp).
2. **Stuck mouse button in stub_SDL_PollEvent**: `arg3` on UP events is the
   buttons-STILL-pressed state (0 after releasing the only button), but the code
   derived *which button changed* from `arg3` directly → every UP reported
   SDL_BUTTON_LEFT → a right-click's release never reached the image → Morphic's
   hand kept the right button pressed forever, turning later clicks into drags.
   Fix: track prev state (`sMouseButtons`) and use the delta for both DOWN and UP.

Also fixed while wiring real input (Win32DisplaySurface.hpp):
- keyboard encoding was wrong (subtype in arg4 with 1/2/3 numbering) — now matches
  the proven `vm_postKeyEvent` layout: arg1=SDL keysym, arg2=0 down/1 up/2 stroke,
  arg3=mods, arg4=SDL(USB-HID) scancode, with a VK→SDL mapping table.
- events now carry real timestamps (steady_clock ms, same base as injectMouseClick).
- WM_MOUSEWHEEL → pharo MouseWheel events (screen→client converted).
- button state on every mouse push comes from wParam's MK_* mask (remaining-state
  convention, matches Pharo's event format).

### GUI testing recipe (Windows, no foreground needed)
- Post messages straight to the HWND: `PostMessage(h, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x,y))`
  etc. — exercises the real wndProc path. Client area is exactly 1024×768.
- Screenshot an occluded window: `PrintWindow(h, dc, PW_RENDERFULLCONTENT=2)`.
- For synthetic WM_KEYUP include the transition bits (`lParam=0xC0000001`),
  otherwise TranslateMessage generates a spurious extra WM_CHAR (doubled text).
- Scripts from this session: `/c/tmp/click5.ps1` (param click+shot), `/c/tmp/final6.ps1`.

## Everything-not-100% catalog
`docs/deferred.md` is the comprehensive list (file/FFI/sockets/crypto all green;
GUI items A=window DONE, B=auto-activate DONE, C=event injection DONE; plus
sound/MIDI/clipboard/profiler/backtrace/symlink/nLink/UDP stubs, the socket
read-path EOF needing POSIX verification, FileAttributesPluginPrimsTest
error-fidelity). Memory: `windows-port-milestones.md`.

## Build / run
- Build (MSYS2 CLANG64): `scripts/build-windows.sh test_load_image`.
- Run from a NATIVE shell (not the MSYS login shell — it strips USERPROFILE).
- Image: `C:/temp/pharo-win-test/Pharo.image`; reference VM:
  `C:/temp/pharo-win-test/refvm/pharo-vm/PharoConsole.exe`.
- SDL2.dll currently staged at `C:/temp/pharo-win-test/SDL2.dll` (a copied DLL).
