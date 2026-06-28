"win-gui-render-check.st — prove the Windows morphic GUI render path works headlessly.

Prereq: a file named SDL2.dll must exist in the IMAGE directory (any DLL works —
our FFI routes SDL symbols to the built-in stubs; FFIWindowsLibraryFinder just
needs the file to exist).  Run with both knobs set so the surface is created in
eval mode AND the rendered frames are dumped:

    PHARO_FORCE_DISPLAY=1 PHARO_DUMP_DISPLAY=1 \
      build-win/test_load_image.exe <image> eval \
      \"'scripts/win-gui-render-check.st' asFileReference fileIn\"

Then convert /tmp/vm-display-{20,60,150}.ppm to PNG and view — it shows the live
Pharo desktop (menu bar + Welcome window).  See docs/deferred.md GUI section."

| r |
"1. Initialise the SDL2 driver (sets up its mutex + SDL2 init via the FFI stubs)."
OSSDL2Driver new.
"2. Create an OSWorldRenderer for the World and install it (replaces NullWorldRenderer)."
r := OSWorldRenderer forWorld: World.
World worldState instVarNamed: #worldRenderer put: r.
"3. doActivate creates the OSWindow (-> SDL_CreateWindow stub sizes gDisplaySurface),
    picks the SDL2 driver, and draws the World once."
r doActivate.
"4. Force repeated full repaints so RenderPresent fires enough times to hit the
    PHARO_DUMP_DISPLAY thresholds (20/60/150)."
60 timesRepeat: [ World fullRepaintNeeded. World displayWorld ].
