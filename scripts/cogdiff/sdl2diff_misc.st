"sdl2diff_misc.st -- pixel formats, surfaces, BMP files, audio conversion, keyboard names, window and display
calls, text conversion and the no-device families, with valid arguments.
Run on stock Cog (real libSDL2, SDL_VIDEODRIVER=dummy) and on our VM; the answers must match.
Calls that differ by design (driver name, built-in tables, host values) are not in this script; see
docs/sdl2-stubs.md."

| out defs mark mem i32 u32 rd surf win px s2 fmt bytes hex fs rw buf tmp cvt stream r |
out := WriteStream on: String new.
defs := OrderedCollection new.
defs
  add: 'xInit: f ^ self ffiCall: #( int SDL_Init ( uint32 f ) )';
  add: 'xClearError ^ self ffiCall: #( void SDL_ClearError ( ) )';
  add: 'xGetError ^ self ffiCall: #( String SDL_GetError ( ) )';
  add: 'xMemcpy: d s: s n: n ^ self ffiCall: #( void* SDL_memcpy ( void* d, void* s, size_t n ) )';
  add: 'xFormatName: f ^ self ffiCall: #( String SDL_GetPixelFormatName ( uint32 f ) )';
  add: 'xEnumToMasks: f bpp: b r: r g: g b: bl a: a ^ self ffiCall: #( int SDL_PixelFormatEnumToMasks ( uint32 f, void* b, void* r, void* g, void* bl, void* a ) )';
  add: 'xMasksToEnum: bpp r: r g: g b: b a: a ^ self ffiCall: #( uint32 SDL_MasksToPixelFormatEnum ( int bpp, uint32 r, uint32 g, uint32 b, uint32 a ) )';
  add: 'xAllocFormat: f ^ self ffiCall: #( void* SDL_AllocFormat ( uint32 f ) )';
  add: 'xFreeFormat: f ^ self ffiCall: #( void SDL_FreeFormat ( void* f ) )';
  add: 'xMapRGB: f r: r g: g b: b ^ self ffiCall: #( uint32 SDL_MapRGB ( void* f, uint8 r, uint8 g, uint8 b ) )';
  add: 'xMapRGBA: f r: r g: g b: b a: a ^ self ffiCall: #( uint32 SDL_MapRGBA ( void* f, uint8 r, uint8 g, uint8 b, uint8 a ) )';
  add: 'xGetRGBA: p f: f r: r g: g b: b a: a ^ self ffiCall: #( void SDL_GetRGBA ( uint32 p, void* f, void* r, void* g, void* b, void* a ) )';
  add: 'xGamma: g ramp: r ^ self ffiCall: #( void SDL_CalculateGammaRamp ( float g, void* r ) )';
  add: 'xAllocPalette: n ^ self ffiCall: #( void* SDL_AllocPalette ( int n ) )';
  add: 'xSetPaletteColors: p c: c first: f n: n ^ self ffiCall: #( int SDL_SetPaletteColors ( void* p, void* c, int f, int n ) )';
  add: 'xCreateSurface: fmt w: w h: h fl: fl dp: dp ^ self ffiCall: #( void* SDL_CreateRGBSurfaceWithFormat ( uint32 fl, int w, int h, int dp, uint32 fmt ) )';
  add: 'xFreeSurface: s ^ self ffiCall: #( void SDL_FreeSurface ( void* s ) )';
  add: 'xFillRect: s r: r c: c ^ self ffiCall: #( int SDL_FillRect ( void* s, void* r, uint32 c ) )';
  add: 'xSetClip: s r: r ^ self ffiCall: #( int SDL_SetClipRect ( void* s, void* r ) )';
  add: 'xGetClip: s r: r ^ self ffiCall: #( void SDL_GetClipRect ( void* s, void* r ) )';
  add: 'xBlit: s sr: sr d: d dr: dr ^ self ffiCall: #( int SDL_UpperBlit ( void* s, void* sr, void* d, void* dr ) )';
  add: 'xBlitScaled: s sr: sr d: d dr: dr ^ self ffiCall: #( int SDL_UpperBlitScaled ( void* s, void* sr, void* d, void* dr ) )';
  add: 'xSetColorKey: s f: f k: k ^ self ffiCall: #( int SDL_SetColorKey ( void* s, int f, uint32 k ) )';
  add: 'xGetColorKey: s k: k ^ self ffiCall: #( int SDL_GetColorKey ( void* s, void* k ) )';
  add: 'xHasColorKey: s ^ self ffiCall: #( int SDL_HasColorKey ( void* s ) )';
  add: 'xSetBlend: s m: m ^ self ffiCall: #( int SDL_SetSurfaceBlendMode ( void* s, uint32 m ) )';
  add: 'xGetBlend: s m: m ^ self ffiCall: #( int SDL_GetSurfaceBlendMode ( void* s, void* m ) )';
  add: 'xSetAlphaMod: s a: a ^ self ffiCall: #( int SDL_SetSurfaceAlphaMod ( void* s, uint8 a ) )';
  add: 'xSetColorMod: s r: r g: g b: b ^ self ffiCall: #( int SDL_SetSurfaceColorMod ( void* s, uint8 r, uint8 g, uint8 b ) )';
  add: 'xConvert: s f: f fl: fl ^ self ffiCall: #( void* SDL_ConvertSurfaceFormat ( void* s, uint32 f, uint32 fl ) )';
  add: 'xDuplicate: s ^ self ffiCall: #( void* SDL_DuplicateSurface ( void* s ) )';
  add: 'xConvertPixels: w h: h sf: sf s: s sp: sp df: df d: d dp: dp ^ self ffiCall: #( int SDL_ConvertPixels ( int w, int h, uint32 sf, void* s, int sp, uint32 df, void* d, int dp ) )';
  add: 'xSaveBMP: s rw: r free: f ^ self ffiCall: #( int SDL_SaveBMP_RW ( void* s, void* r, int f ) )';
  add: 'xLoadBMP: r free: f ^ self ffiCall: #( void* SDL_LoadBMP_RW ( void* r, int f ) )';
  add: 'xRWFromFile: f m: m ^ self ffiCall: #( void* SDL_RWFromFile ( String f, String m ) )';
  add: 'xRWFromConstMem: m n: n ^ self ffiCall: #( void* SDL_RWFromConstMem ( void* m, int n ) )';
  add: 'xNewStream: sf c: sc r: sr df: df c: dc r: dr ^ self ffiCall: #( void* SDL_NewAudioStream ( uint16 sf, uint8 sc, int sr, uint16 df, uint8 dc, int dr ) )';
  add: 'xStreamPut: s b: b n: n ^ self ffiCall: #( int SDL_AudioStreamPut ( void* s, void* b, int n ) )';
  add: 'xStreamGet: s b: b n: n ^ self ffiCall: #( int SDL_AudioStreamGet ( void* s, void* b, int n ) )';
  add: 'xStreamAvail: s ^ self ffiCall: #( int SDL_AudioStreamAvailable ( void* s ) )';
  add: 'xFreeStream: s ^ self ffiCall: #( void SDL_FreeAudioStream ( void* s ) )';
  add: 'xBuildCvt: c sf: sf sc: sc sr: sr df: df dc: dc dr: dr ^ self ffiCall: #( int SDL_BuildAudioCVT ( void* c, uint16 sf, uint8 sc, int sr, uint16 df, uint8 dc, int dr ) )';
  add: 'xConvertAudio: c ^ self ffiCall: #( int SDL_ConvertAudio ( void* c ) )';
  add: 'xMix: d s: s f: f n: n v: v ^ self ffiCall: #( void SDL_MixAudioFormat ( void* d, void* s, uint16 f, uint32 n, int v ) )';
  add: 'xLoadWAV: r free: f spec: sp buf: b len: l ^ self ffiCall: #( void* SDL_LoadWAV_RW ( void* r, int f, void* sp, void* b, void* l ) )';
  add: 'xScancodeName: s ^ self ffiCall: #( String SDL_GetScancodeName ( int s ) )';
  add: 'xScancodeFromName: n ^ self ffiCall: #( int SDL_GetScancodeFromName ( String n ) )';
  add: 'xKeyName: k ^ self ffiCall: #( String SDL_GetKeyName ( int32 k ) )';
  add: 'xKeyFromName: n ^ self ffiCall: #( int32 SDL_GetKeyFromName ( String n ) )';
  add: 'xKeyFromScancode: s ^ self ffiCall: #( int32 SDL_GetKeyFromScancode ( int s ) )';
  add: 'xScancodeFromKey: k ^ self ffiCall: #( int SDL_GetScancodeFromKey ( int32 k ) )';
  add: 'xKeyboardState: n ^ self ffiCall: #( void* SDL_GetKeyboardState ( void* n ) )';
  add: 'xCreateWin: t x: x y: y w: w h: h flags: f ^ self ffiCall: #( void* SDL_CreateWindow ( String t, int x, int y, int w, int h, uint32 f ) )';
  add: 'xWinMin: w w: a h: b ^ self ffiCall: #( void SDL_GetWindowMinimumSize ( void* w, void* a, void* b ) )';
  add: 'xWinMax: w w: a h: b ^ self ffiCall: #( void SDL_GetWindowMaximumSize ( void* w, void* a, void* b ) )';
  add: 'xSetWinMin: w w: a h: b ^ self ffiCall: #( void SDL_SetWindowMinimumSize ( void* w, int a, int b ) )';
  add: 'xSetWinMax: w w: a h: b ^ self ffiCall: #( void SDL_SetWindowMaximumSize ( void* w, int a, int b ) )';
  add: 'xWinSize: w w: a h: b ^ self ffiCall: #( void SDL_GetWindowSize ( void* w, void* a, void* b ) )';
  add: 'xSetWinSize: w w: a h: b ^ self ffiCall: #( void SDL_SetWindowSize ( void* w, int a, int b ) )';
  add: 'xSetWinData: w n: n d: d ^ self ffiCall: #( void* SDL_SetWindowData ( void* w, String n, void* d ) )';
  add: 'xGetWinData: w n: n ^ self ffiCall: #( void* SDL_GetWindowData ( void* w, String n ) )';
  add: 'xSetGrab: w g: g ^ self ffiCall: #( void SDL_SetWindowGrab ( void* w, int g ) )';
  add: 'xGetGrab: w ^ self ffiCall: #( int SDL_GetWindowGrab ( void* w ) )';
  add: 'xSetMouseGrab: w g: g ^ self ffiCall: #( void SDL_SetWindowMouseGrab ( void* w, int g ) )';
  add: 'xGetMouseGrab: w ^ self ffiCall: #( int SDL_GetWindowMouseGrab ( void* w ) )';
  add: 'xGetKeyboardGrab: w ^ self ffiCall: #( int SDL_GetWindowKeyboardGrab ( void* w ) )';
  add: 'xGrabbed ^ self ffiCall: #( void* SDL_GetGrabbedWindow ( ) )';
  add: 'xOpacity: w o: o ^ self ffiCall: #( int SDL_GetWindowOpacity ( void* w, void* o ) )';
  add: 'xSetOpacity: w o: o ^ self ffiCall: #( int SDL_SetWindowOpacity ( void* w, float o ) )';
  add: 'xBrightness: w ^ self ffiCall: #( float SDL_GetWindowBrightness ( void* w ) )';
  add: 'xSetBrightness: w b: b ^ self ffiCall: #( int SDL_SetWindowBrightness ( void* w, float b ) )';
  add: 'xGamma: w r: r g: g b: b ^ self ffiCall: #( int SDL_GetWindowGammaRamp ( void* w, void* r, void* g, void* b ) )';
  add: 'xBorders: w t: t l: l b: b r: r ^ self ffiCall: #( int SDL_GetWindowBordersSize ( void* w, void* t, void* l, void* b, void* r ) )';
  add: 'xModal: w p: p ^ self ffiCall: #( int SDL_SetWindowModalFor ( void* w, void* p ) )';
  add: 'xFlash: w o: o ^ self ffiCall: #( int SDL_FlashWindow ( void* w, int o ) )';
  add: 'xPixelFormat: w ^ self ffiCall: #( uint32 SDL_GetWindowPixelFormat ( void* w ) )';
  add: 'xSizeInPixels: w w: a h: b ^ self ffiCall: #( void SDL_GetWindowSizeInPixels ( void* w, void* a, void* b ) )';
  add: 'xMouseRect: w ^ self ffiCall: #( void* SDL_GetWindowMouseRect ( void* w ) )';
  add: 'xSetMouseRect: w r: r ^ self ffiCall: #( int SDL_SetWindowMouseRect ( void* w, void* r ) )';
  add: 'xDisplayName: i ^ self ffiCall: #( String SDL_GetDisplayName ( int i ) )';
  add: 'xClosestMode: i m: m c: c ^ self ffiCall: #( void* SDL_GetClosestDisplayMode ( int i, void* m, void* c ) )';
  add: 'xScreenSaver ^ self ffiCall: #( int SDL_IsScreenSaverEnabled ( ) )';
  add: 'xEnableSaver ^ self ffiCall: #( void SDL_EnableScreenSaver ( ) )';
  add: 'xDisableSaver ^ self ffiCall: #( void SDL_DisableScreenSaver ( ) )';
  add: 'xGLContext: w ^ self ffiCall: #( void* SDL_GL_CreateContext ( void* w ) )';
  add: 'xGLSwap: w ^ self ffiCall: #( void SDL_GL_SwapWindow ( void* w ) )';
  add: 'xVkExt: w c: c n: n ^ self ffiCall: #( int SDL_Vulkan_GetInstanceExtensions ( void* w, void* c, void* n ) )';
  add: 'xHitTest: w cb: cb d: d ^ self ffiCall: #( int SDL_SetWindowHitTest ( void* w, void* cb, void* d ) )';
  add: 'xShowCursor: t ^ self ffiCall: #( int SDL_ShowCursor ( int t ) )';
  add: 'xWasInit: f ^ self ffiCall: #( uint32 SDL_WasInit ( uint32 f ) )';
  add: 'xIconv: to from: from in: in len: n ^ self ffiCall: #( void* SDL_iconv_string ( String to, String from, void* in, size_t n ) )';
  add: 'xFree: p ^ self ffiCall: #( void SDL_free ( void* p ) )';
  add: 'xPrimarySet: t ^ self ffiCall: #( int SDL_SetPrimarySelectionText ( String t ) )';
  add: 'xPrimaryHas ^ self ffiCall: #( int SDL_HasPrimarySelectionText ( ) )';
  add: 'xGuidString: a b: b out: o n: n ^ self ffiCall: #( void SDL_JoystickGetGUIDString ( uint64 a, uint64 b, void* o, int n ) )';
  add: 'xNumJoy ^ self ffiCall: #( int SDL_NumJoysticks ( ) )';
  add: 'xJoyEvent: s ^ self ffiCall: #( int SDL_JoystickEventState ( int s ) )';
  add: 'xCtrlEvent: s ^ self ffiCall: #( int SDL_GameControllerEventState ( int s ) )';
  add: 'xAxisFromString: s ^ self ffiCall: #( int SDL_GameControllerGetAxisFromString ( String s ) )';
  add: 'xButtonString: b ^ self ffiCall: #( String SDL_GameControllerGetStringForButton ( int b ) )';
  add: 'xAxisString: b ^ self ffiCall: #( String SDL_GameControllerGetStringForAxis ( int b ) )';
  add: 'xNumTouch ^ self ffiCall: #( int SDL_GetNumTouchDevices ( ) )';
  add: 'xTouchDevice: i ^ self ffiCall: #( int64 SDL_GetTouchDevice ( int i ) )';
  add: 'xOpenAudio: n c: c d: d o: o a: a ^ self ffiCall: #( uint32 SDL_OpenAudioDevice ( String n, int c, void* d, void* o, int a ) )';
  add: 'xAudioDeviceName: i c: c ^ self ffiCall: #( String SDL_GetAudioDeviceName ( int i, int c ) )';
  add: 'xHidInit ^ self ffiCall: #( int SDL_hid_init ( ) )';
  add: 'xHidEnum: v p: p ^ self ffiCall: #( void* SDL_hid_enumerate ( uint16 v, uint16 p ) )'.
defs do: [ :d | SDL2 class compile: d classified: 'x' ].
mark := [ :label | out << '== ' << label; nextPut: Character lf ].
i32 := [ :nums | | b | b := ByteArray new: nums size * 4. nums withIndexDo: [ :n :i | b signedLongAt: i - 1 * 4 + 1 put: n ]. b ].
u32 := [ :ba :k | ba unsignedLongAt: k * 4 + 1 ].
hex := [ :ba | | s | s := WriteStream on: String new. ba do: [ :b | s nextPutAll: (b printStringBase: 16 length: 2 padded: true) ]. s contents ].
rd := [ :ptr :n | | b | b := ByteArray new: n. SDL2 xMemcpy: b s: ptr n: n. b ].
SDL2 xInit: 16rF230.
SDL2 xClearError.
r := [ :label :v | out << label << ' = ' << v printString << ' | ' << SDL2 xGetError printString; nextPut: Character lf. SDL2 xClearError ].

"---- pixel formats ----"
mark value: 'pixel formats'.
#( 372645892 370546692 376840196 353701890 353570562 336660481 386930691 390076419 318769153 357764866 373694468 0 1 ) do: [ :f |
  | bpp rm gm bm am ok |
  bpp := ByteArray new: 4. rm := ByteArray new: 4. gm := ByteArray new: 4. bm := ByteArray new: 4. am := ByteArray new: 4.
  ok := SDL2 xEnumToMasks: f bpp: bpp r: rm g: gm b: bm a: am.
  out << f printString << ' ' << (SDL2 xFormatName: f) << ' masks ok=' << ok printString << ' bpp=' << (bpp signedLongAt: 1) printString << ' ' << (u32 value: rm value: 0) printStringHex << ' ' << (u32 value: gm value: 0) printStringHex << ' ' << (u32 value: bm value: 0) printStringHex << ' ' << (u32 value: am value: 0) printStringHex << ' | ' << SDL2 xGetError printString; nextPut: Character lf.
  SDL2 xClearError ].
#( #(32 16rFF0000 16rFF00 16rFF 16rFF000000) #(32 0 0 0 0) #(16 16rF800 16r7E0 16r1F 0) #(16 0 0 0 0) #(15 16r7C00 16r3E0 16r1F 0) #(8 0 0 0 0) #(24 16rFF 16rFF00 16rFF0000 0) #(32 16rFF 16rFF00 16rFF0000 16rFF000000) #(32 16rFF000000 16rFF0000 16rFF00 16rFF) #(16 1 2 3 4) ) do: [ :m |
  out << 'masks->enum ' << m printString << ' = ' << (SDL2 xMasksToEnum: m first r: (m at: 2) g: (m at: 3) b: (m at: 4) a: (m at: 5)) printString; nextPut: Character lf ].
#( 372645892 353701890 318769153 376840196 ) do: [ :f | | pf row |
  pf := SDL2 xAllocFormat: f.
  row := rd value: pf value: 56.
  out << 'alloc ' << (SDL2 xFormatName: f) << ' bits=' << (row at: 17) printString << ' bytes=' << (row at: 18) printString << ' masks ' << (u32 value: row value: 5) printStringHex << ' ' << (u32 value: row value: 6) printStringHex << ' ' << (u32 value: row value: 7) printStringHex << ' ' << (u32 value: row value: 8) printStringHex << ' loss/shift ' << ((37 to: 44) collect: [ :k | row at: k ]) printString << ' palette ' << ((row unsignedLongLongAt: 9) = 0) printString; nextPut: Character lf.
  #( #(255 128 64) #(0 0 0) #(255 255 255) #(10 200 30) ) do: [ :c | | v rr gg bb aa |
    v := SDL2 xMapRGB: pf r: c first g: (c at: 2) b: (c at: 3).
    rr := ByteArray new: 1. gg := ByteArray new: 1. bb := ByteArray new: 1. aa := ByteArray new: 1.
    SDL2 xGetRGBA: v f: pf r: rr g: gg b: bb a: aa.
    out << '  map ' << c printString << ' -> ' << v printStringHex << ' back ' << { rr first. gg first. bb first. aa first } printString; nextPut: Character lf ].
  out << '  maprgba ' << (SDL2 xMapRGBA: pf r: 100 g: 150 b: 200 a: 128) printStringHex; nextPut: Character lf.
  SDL2 xFreeFormat: pf ].
(ByteArray new: 512) in: [ :ramp |
  #( 1.0 2.2 0.5 0.0 ) do: [ :g | SDL2 xGamma: g ramp: ramp. out << 'gamma ' << g printString << ' ' << ((0 to: 255 by: 51) collect: [ :k | ramp unsignedShortAt: k * 2 + 1 ]) printString; nextPut: Character lf ] ].
(SDL2 xAllocPalette: 0) isNull printString in: [ :s | out << 'palette 0 ' << s << ' ' << SDL2 xGetError printString; nextPut: Character lf ]. SDL2 xClearError.

"---- surfaces ----"
mark value: 'surfaces'.
surf := SDL2 xCreateSurface: 372645892 w: 8 h: 8 fl: 0 dp: 32.
s2 := SDL2 xCreateSurface: 372645892 w: 8 h: 8 fl: 0 dp: 32.
px := [ :sf | | head p | head := rd value: sf value: 96. p := ExternalAddress fromAddress: (head unsignedLongLongAt: 33). rd value: p value: 8 * 8 * 4 ].
out << 'surface fields ' << ((rd value: surf value: 96) in: [ :h | { h unsignedLongAt: 1. h signedLongAt: 17. h signedLongAt: 21. h signedLongAt: 25 } ]) printString; nextPut: Character lf.
SDL2 xFillRect: surf r: nil c: 16rFF102030.
SDL2 xFillRect: surf r: (i32 value: #(2 2 4 4)) c: 16rFFAA0000.
SDL2 xFillRect: surf r: (i32 value: #(-3 -3 6 6)) c: 16rFF00BB00.
out << 'fill ' << (hex value: (px value: surf)) hash printString << ' ' << ((u32 value: (px value: surf) value: 0) printStringHex) << ' ' << ((u32 value: (px value: surf) value: 3 * 8 + 3) printStringHex) << ' ' << ((u32 value: (px value: surf) value: 7 * 8 + 7) printStringHex); nextPut: Character lf.
(ByteArray new: 16) in: [ :c | SDL2 xSetClip: surf r: (i32 value: #(1 1 3 3)). SDL2 xGetClip: surf r: c. out << 'clip ' << ((1 to: 4) collect: [ :k | c signedLongAt: k - 1 * 4 + 1 ]) printString << ' setclip outside ' << (SDL2 xSetClip: surf r: (i32 value: #(20 20 3 3))) printString; nextPut: Character lf. SDL2 xSetClip: surf r: nil ].
SDL2 xFillRect: s2 r: nil c: 16r80FF0000.
#( 0 1 2 4 8 ) do: [ :mode |
  SDL2 xFillRect: surf r: nil c: 16rFF102030.
  SDL2 xSetBlend: s2 m: mode.
  (ByteArray new: 4) in: [ :m | SDL2 xGetBlend: s2 m: m. out << 'blend ' << mode printString << ' get ' << (m unsignedLongAt: 1) printString ].
  out << ' blit ' << (SDL2 xBlit: s2 sr: nil d: surf dr: (i32 value: #(1 1 0 0))) printString << ' ' << ((u32 value: (px value: surf) value: 1 * 8 + 1) printStringHex) << ' ' << ((u32 value: (px value: surf) value: 0) printStringHex); nextPut: Character lf ].
SDL2 xSetBlend: s2 m: 1.
SDL2 xSetAlphaMod: s2 a: 100. SDL2 xSetColorMod: s2 r: 200 g: 255 b: 50.
SDL2 xFillRect: surf r: nil c: 16rFF102030.
SDL2 xBlit: s2 sr: nil d: surf dr: (i32 value: #(0 0 0 0)).
out << 'blit modulated ' << ((u32 value: (px value: surf) value: 0) printStringHex); nextPut: Character lf.
SDL2 xSetAlphaMod: s2 a: 255. SDL2 xSetColorMod: s2 r: 255 g: 255 b: 255. SDL2 xSetBlend: s2 m: 0.
SDL2 xFillRect: s2 r: nil c: 16rFF00FF00.
SDL2 xFillRect: s2 r: (i32 value: #(0 0 2 2)) c: 16rFFFF00FF.
out << 'colorkey none ' << (SDL2 xHasColorKey: s2) printString << ' ' << (SDL2 xGetColorKey: s2 k: (ByteArray new: 4)) printString << ' ' << SDL2 xGetError printString; nextPut: Character lf. SDL2 xClearError.
SDL2 xSetColorKey: s2 f: 1 k: 16rFFFF00FF.
out << 'colorkey set ' << (SDL2 xHasColorKey: s2) printString << ' ' << ((ByteArray new: 4) in: [ :k | SDL2 xGetColorKey: s2 k: k. k unsignedLongAt: 1 ]) printStringHex; nextPut: Character lf.
SDL2 xFillRect: surf r: nil c: 16rFF000000.
SDL2 xBlit: s2 sr: nil d: surf dr: (i32 value: #(0 0 0 0)).
out << 'keyed blit ' << ((u32 value: (px value: surf) value: 0) printStringHex) << ' ' << ((u32 value: (px value: surf) value: 2 * 8 + 2) printStringHex); nextPut: Character lf.
(ByteArray new: 16) in: [ :dr | dr replaceFrom: 1 to: 16 with: (i32 value: #(6 6 8 8)) startingAt: 1. SDL2 xFillRect: surf r: nil c: 16rFF000000. out << 'blit clipped ' << (SDL2 xBlit: s2 sr: nil d: surf dr: dr) printString << ' dst rect ' << ((1 to: 4) collect: [ :k | dr signedLongAt: k - 1 * 4 + 1 ]) printString; nextPut: Character lf ].
SDL2 xSetColorKey: s2 f: 0 k: 0.
(ByteArray new: 16) in: [ :dr | dr replaceFrom: 1 to: 16 with: (i32 value: #(0 0 3 3)) startingAt: 1. SDL2 xFillRect: surf r: nil c: 16rFF000000. SDL2 xBlitScaled: s2 sr: (i32 value: #(0 0 2 2)) d: surf dr: dr. out << 'scaled ' << ((px value: surf) in: [ :p | (0 to: 3) collect: [ :k | u32 value: p value: k ] ]) printString << ' ' << ((u32 value: (px value: surf) value: 2 * 8 + 2) printStringHex); nextPut: Character lf ].
out << 'blit null ' << (SDL2 xBlit: nil sr: nil d: surf dr: nil) printString << ' ' << SDL2 xGetError printString; nextPut: Character lf. SDL2 xClearError.
(SDL2 xConvert: surf f: 376840196 fl: 0) in: [ :c | out << 'convert ABGR ' << ((u32 value: (px value: c) value: 3 * 8 + 3) printStringHex) << ' ' << ((u32 value: (px value: c) value: 0) printStringHex); nextPut: Character lf. SDL2 xFreeSurface: c ].
(SDL2 xDuplicate: surf) in: [ :c | out << 'duplicate ' << ((px value: c) = (px value: surf)) printString; nextPut: Character lf. SDL2 xFreeSurface: c ].
"ConvertPixels between formats"
(ByteArray new: 16) in: [ :src | | dst |
  src unsignedLongAt: 1 put: 16rFF8040C0; unsignedLongAt: 5 put: 16r80102030; unsignedLongAt: 9 put: 16r00FFFFFF; unsignedLongAt: 13 put: 16rFF000000.
  #( 370546692 376840196 353701890 373694468 ) do: [ :df | | bytesPer |
    bytesPer := df = 353701890 ifTrue: [ 2 ] ifFalse: [ 4 ].
    dst := ByteArray new: 4 * bytesPer.
    out << 'convertpixels to ' << (SDL2 xFormatName: df) << ' ' << (SDL2 xConvertPixels: 4 h: 1 sf: 372645892 s: src sp: 16 df: df d: dst dp: 4 * bytesPer) printString << ' ' << (hex value: dst); nextPut: Character lf ] ].
SDL2 xFreeSurface: surf. SDL2 xFreeSurface: s2.

"---- BMP files ----"
mark value: 'bmp'.
tmp := '/tmp/sdl2diff_', UUID new asString36, '.bmp'.
surf := SDL2 xCreateSurface: 372645892 w: 3 h: 2 fl: 0 dp: 32.
(px := ByteArray new: 24) in: [ :p | 0 to: 5 do: [ :k | p unsignedLongAt: k * 4 + 1 put: 16r80000000 + (k * 16r010203) ] ].
(rd value: surf value: 96) in: [ :head | SDL2 xMemcpy: (ExternalAddress fromAddress: (head unsignedLongLongAt: 33)) s: px n: 24 ].
out << 'save argb ' << (SDL2 xSaveBMP: surf rw: (SDL2 xRWFromFile: tmp m: 'wb') free: 1) printString; nextPut: Character lf.
bytes := tmp asFileReference binaryReadStreamDo: [ :f | f upToEnd ].
out << 'argb file size ' << bytes size printString << ' ' << (hex value: bytes); nextPut: Character lf.
(SDL2 xLoadBMP: (SDL2 xRWFromFile: tmp m: 'rb') free: 1) in: [ :l |
  out << 'load argb ' << l isNull printString << ' ' << ((rd value: l value: 96) in: [ :h | { h signedLongAt: 17. h signedLongAt: 21 } ]) printString << ' ' << ((u32 value: (px := (rd value: l value: 96) in: [ :h | rd value: (ExternalAddress fromAddress: (h unsignedLongLongAt: 33)) value: 24 ]) value: 0) printStringHex) << ' ' << (u32 value: px value: 5) printStringHex; nextPut: Character lf ].
tmp asFileReference ensureDelete.
SDL2 xFreeSurface: surf.
surf := SDL2 xCreateSurface: 370546692 w: 3 h: 2 fl: 0 dp: 32.
(px := ByteArray new: 24) in: [ :p | 0 to: 5 do: [ :k | p unsignedLongAt: k * 4 + 1 put: 16rFF000000 + (k * 16r010203) ] ].
(rd value: surf value: 96) in: [ :head | SDL2 xMemcpy: (ExternalAddress fromAddress: (head unsignedLongLongAt: 33)) s: px n: 24 ].
SDL2 xSaveBMP: surf rw: (SDL2 xRWFromFile: tmp m: 'wb') free: 1.
bytes := tmp asFileReference binaryReadStreamDo: [ :f | f upToEnd ].
out << 'xrgb file size ' << bytes size printString << ' ' << (hex value: bytes); nextPut: Character lf.
tmp asFileReference ensureDelete.
SDL2 xFreeSurface: surf.
out << 'load not bmp ' << (SDL2 xLoadBMP: (SDL2 xRWFromConstMem: (ByteArray new: 64) n: 64) free: 1) isNull printString << ' ' << SDL2 xGetError printString; nextPut: Character lf. SDL2 xClearError.

"---- audio conversion ----"
mark value: 'audio'.
"formats: U8 8, S8 16r8008, U16LSB 16, S16LSB 16r8010, S16MSB 16r9010, S32LSB 16r8020, F32LSB 16r8120"
#( #(16r8010 16r8120) #(16r8010 8) #(8 16r8010) #(16r8010 16r9010) #(16r8120 16r8010) #(16r8010 16r8020) ) do: [ :pair |
  | st inb outb n |
  st := SDL2 xNewStream: pair first c: 2 r: 22050 df: pair last c: 2 r: 22050.
  inb := ByteArray new: 16. #( 0 100 -100 32767 -32768 1000 -1000 5 ) withIndexDo: [ :v :k | inb signedShortAt: k - 1 * 2 + 1 put: v ].
  SDL2 xStreamPut: st b: inb n: 16.
  outb := ByteArray new: 64.
  n := SDL2 xStreamGet: st b: outb n: 64.
  out << 'stream ' << pair printString << ' avail-after-get ' << (SDL2 xStreamAvail: st) printString << ' bytes ' << n printString << ' ' << (hex value: (outb copyFrom: 1 to: n)); nextPut: Character lf.
  SDL2 xFreeStream: st ].
out << 'stream partial ' << ((SDL2 xNewStream: 16r8010 c: 2 r: 22050 df: 16r8010 c: 2 r: 22050) in: [ :st | | v | v := SDL2 xStreamPut: st b: (ByteArray new: 6) n: 6. SDL2 xFreeStream: st. v ]) printString << ' ' << SDL2 xGetError printString; nextPut: Character lf. SDL2 xClearError.
out << 'stream bad channels ' << (SDL2 xNewStream: 16r8010 c: 3 r: 22050 df: 16r8010 c: 2 r: 22050) isNull printString << ' ' << SDL2 xGetError printString; nextPut: Character lf. SDL2 xClearError.
(ByteArray new: 160) in: [ :cvt |
  out << 'build same ' << (SDL2 xBuildCvt: cvt sf: 16r8010 sc: 2 sr: 44100 df: 16r8010 dc: 2 dr: 44100) printString << ' needed ' << (cvt signedLongAt: 1) printString; nextPut: Character lf.
  out << 'build format ' << (SDL2 xBuildCvt: cvt sf: 16r8010 sc: 2 sr: 44100 df: 16r8120 dc: 2 dr: 44100) printString << ' needed ' << (cvt signedLongAt: 1) printString << ' len_mult ' << (cvt signedLongAt: 33) printString << ' ratio ' << (cvt doubleAt: 37) printString; nextPut: Character lf.
  out << 'build rate ' << (SDL2 xBuildCvt: cvt sf: 16r8010 sc: 2 sr: 22050 df: 16r8010 dc: 2 dr: 44100) printString << ' needed ' << (cvt signedLongAt: 1) printString << ' len_mult ' << (cvt signedLongAt: 33) printString << ' ratio ' << (cvt doubleAt: 37) printString << ' incr ' << (cvt doubleAt: 9) printString; nextPut: Character lf.
  out << 'build bad ' << (SDL2 xBuildCvt: cvt sf: 16r8010 sc: 3 sr: 22050 df: 16r8010 dc: 2 dr: 44100) printString << ' ' << SDL2 xGetError printString; nextPut: Character lf. SDL2 xClearError ].
"mix"
#( 16r8010 8 16r8120 16r8020 ) do: [ :f | | d sv |
  d := ByteArray new: 8. sv := ByteArray new: 8.
  f = 16r8010 ifTrue: [ d signedShortAt: 1 put: 1000; signedShortAt: 3 put: 30000; signedShortAt: 5 put: -30000; signedShortAt: 7 put: 7. sv signedShortAt: 1 put: 2000; signedShortAt: 3 put: 10000; signedShortAt: 5 put: -10000; signedShortAt: 7 put: -7 ].
  f = 8 ifTrue: [ d replaceFrom: 1 to: 8 with: #(128 200 250 10 128 128 255 0) asByteArray. sv replaceFrom: 1 to: 8 with: #(150 200 250 10 100 128 255 0) asByteArray ].
  f = 16r8120 ifTrue: [ d float32AtOffset: 0 put: 0.5; float32AtOffset: 4 put: -0.25. sv float32AtOffset: 0 put: 0.25; float32AtOffset: 4 put: -1.5 ].
  f = 16r8020 ifTrue: [ d signedLongAt: 1 put: 100000000; signedLongAt: 5 put: -2000000000. sv signedLongAt: 1 put: 300000000; signedLongAt: 5 put: -500000000 ].
  #( 128 64 0 ) do: [ :vol | | dd | dd := d copy. SDL2 xMix: dd s: sv f: f n: 8 v: vol. out << 'mix ' << f printString << ' vol ' << vol printString << ' ' << (hex value: dd); nextPut: Character lf ] ].
out << 'mix unknown format ' << (SDL2 xMix: (ByteArray new: 4) s: (ByteArray new: 4) f: 1 n: 4 v: 100) isNil printString << ' ' << SDL2 xGetError printString; nextPut: Character lf. SDL2 xClearError.
"WAV loading from memory"
(ByteArray new: 52) in: [ :wav |
  wav replaceFrom: 1 to: 4 with: 'RIFF' asByteArray. wav unsignedLongAt: 5 put: 44.
  wav replaceFrom: 9 to: 12 with: 'WAVE' asByteArray. wav replaceFrom: 13 to: 16 with: 'fmt ' asByteArray. wav unsignedLongAt: 17 put: 16.
  wav unsignedShortAt: 21 put: 1; unsignedShortAt: 23 put: 2; unsignedLongAt: 25 put: 22050; unsignedLongAt: 29 put: 88200; unsignedShortAt: 33 put: 4; unsignedShortAt: 35 put: 16.
  wav replaceFrom: 37 to: 40 with: 'data' asByteArray. wav unsignedLongAt: 41 put: 8.
  wav replaceFrom: 45 to: 52 with: #( 1 2 3 4 5 6 7 8 ) asByteArray.
  (ByteArray new: 32) in: [ :spec | (ByteArray new: 8) in: [ :pb | (ByteArray new: 4) in: [ :lb | | ret |
    ret := SDL2 xLoadWAV: (SDL2 xRWFromConstMem: wav n: 52) free: 1 spec: spec buf: pb len: lb.
    out << 'wav ' << ret isNull printString << ' freq ' << (spec signedLongAt: 1) printString << ' format ' << (spec unsignedShortAt: 5) printString << ' channels ' << (spec at: 7) printString << ' samples ' << (spec unsignedShortAt: 9) printString << ' size ' << (spec unsignedLongAt: 13) printString << ' len ' << (lb unsignedLongAt: 1) printString << ' data ' << (hex value: (rd value: (ExternalAddress fromAddress: (pb unsignedLongLongAt: 1)) value: 8)); nextPut: Character lf ] ] ] ].
out << 'wav bad ' << ((SDL2 xLoadWAV: (SDL2 xRWFromConstMem: (ByteArray new: 64) n: 64) free: 1 spec: (ByteArray new: 32) buf: (ByteArray new: 8) len: (ByteArray new: 4)) isNull) printString << ' ' << SDL2 xGetError printString; nextPut: Character lf. SDL2 xClearError.

"---- keyboard ----"
mark value: 'keyboard'.
#( 4 30 40 41 44 58 79 224 225 227 511 512 -1 0 3 ) do: [ :sc | out << 'scancode ' << sc printString << ' name ' << (SDL2 xScancodeName: sc) printString << ' key ' << (SDL2 xKeyFromScancode: sc) printString << ' back ' << (SDL2 xScancodeFromKey: (SDL2 xKeyFromScancode: sc)) printString << ' | ' << SDL2 xGetError printString; nextPut: Character lf. SDL2 xClearError ].
#( 'A' 'a' 'Return' 'space' 'Escape' 'Left Shift' 'F1' 'nonsense' '' 'Keypad 5' ) do: [ :n | out << 'fromname ' << n printString << ' sc ' << (SDL2 xScancodeFromName: n) printString << ' key ' << (SDL2 xKeyFromName: n) printString << ' | ' << SDL2 xGetError printString; nextPut: Character lf. SDL2 xClearError ].
#( 97 65 13 27 8 9 32 127 48 16r4000003A 16r400000E1 16rE4 16r20AC 16r1F600 ) do: [ :k | out << 'keyname ' << k printString << ' ' << (SDL2 xKeyName: k) printString; nextPut: Character lf ].
out << 'keyboard state numkeys ' << ((ByteArray new: 4) in: [ :n | SDL2 xKeyboardState: n. n signedLongAt: 1 ]) printString; nextPut: Character lf.

"---- window and display ----"
mark value: 'window'.
win := SDL2 xCreateWin: 'w' x: 10 y: 10 w: 100 h: 80 flags: 0.
(ByteArray new: 8) in: [ :a | | b | b := ByteArray new: 8.
  SDL2 xWinMin: win w: a h: b. out << 'min default ' << (a signedLongAt: 1) printString << 'x' << (b signedLongAt: 1) printString.
  SDL2 xSetWinMin: win w: 120 h: 90. SDL2 xWinMin: win w: a h: b. out << ' set 120x90 -> ' << (a signedLongAt: 1) printString << 'x' << (b signedLongAt: 1) printString.
  SDL2 xWinSize: win w: a h: b. out << ' size grew to ' << (a signedLongAt: 1) printString << 'x' << (b signedLongAt: 1) printString; nextPut: Character lf.
  SDL2 xSetWinMax: win w: 200 h: 150. SDL2 xWinMax: win w: a h: b. out << 'max ' << (a signedLongAt: 1) printString << 'x' << (b signedLongAt: 1) printString.
  SDL2 xSetWinMax: win w: 50 h: 50. out << ' max below min: ' << SDL2 xGetError printString. SDL2 xClearError.
  SDL2 xSetWinMin: win w: 0 h: 10. out << ' min zero: ' << SDL2 xGetError printString; nextPut: Character lf. SDL2 xClearError.
  SDL2 xSetWinSize: win w: 500 h: 500. SDL2 xWinSize: win w: a h: b. out << 'size clamped to max ' << (a signedLongAt: 1) printString << 'x' << (b signedLongAt: 1) printString; nextPut: Character lf.
  SDL2 xSizeInPixels: win w: a h: b. out << 'pixels ' << (a signedLongAt: 1) printString << 'x' << (b signedLongAt: 1) printString; nextPut: Character lf ].
out << 'data ' << (SDL2 xSetWinData: win n: 'k' d: (ExternalAddress fromAddress: 77)) isNull printString << ' ' << (SDL2 xGetWinData: win n: 'k') asInteger printString << ' replaced ' << (SDL2 xSetWinData: win n: 'k' d: (ExternalAddress fromAddress: 88)) asInteger printString << ' name empty ' << (SDL2 xGetWinData: win n: '') isNull printString << ' ' << SDL2 xGetError printString; nextPut: Character lf. SDL2 xClearError.
out << 'grab ' << (SDL2 xGetGrab: win) printString; nextPut: Character lf.
SDL2 xSetGrab: win g: 1.
out << 'grab on ' << (SDL2 xGetGrab: win) printString << ' mouse ' << (SDL2 xGetMouseGrab: win) printString << ' keyboard ' << (SDL2 xGetKeyboardGrab: win) printString << ' grabbed is win ' << (SDL2 xGrabbed = win) printString; nextPut: Character lf.
SDL2 xSetGrab: win g: 0.
out << 'grab off ' << (SDL2 xGetGrab: win) printString << ' grabbed ' << (SDL2 xGrabbed isNull) printString; nextPut: Character lf.
out << 'opacity get ' << ((ByteArray new: 4) in: [ :o | SDL2 xOpacity: win o: o. o float32AtOffset: 0 ]) printString << ' set ' << (SDL2 xSetOpacity: win o: 0.5) printString << ' ' << SDL2 xGetError printString; nextPut: Character lf. SDL2 xClearError.
out << 'brightness ' << (SDL2 xBrightness: win) printString << ' set ' << (SDL2 xSetBrightness: win b: 0.5) printString << ' ' << SDL2 xGetError printString; nextPut: Character lf. SDL2 xClearError.
(ByteArray new: 1536) in: [ :ramp | out << 'gamma ramp ' << (SDL2 xGamma: win r: ramp g: ramp b: ramp) printString << ' ' << ((ramp unsignedShortAt: 1) printString) << ' ' << (ramp unsignedShortAt: 3) printString << ' ' << (ramp unsignedShortAt: 511) printString; nextPut: Character lf ].
out << 'borders ' << (SDL2 xBorders: win t: (ByteArray new: 4) l: (ByteArray new: 4) b: (ByteArray new: 4) r: (ByteArray new: 4)) printString << ' ' << SDL2 xGetError printString << ' modal ' << (SDL2 xModal: win p: win) printString << ' ' << SDL2 xGetError printString << ' flash ' << (SDL2 xFlash: win o: 0) printString << ' ' << SDL2 xGetError printString; nextPut: Character lf. SDL2 xClearError.
out << 'pixel format ' << (SDL2 xPixelFormat: win) isZero not printString; nextPut: Character lf.
out << 'mouse rect ' << (SDL2 xMouseRect: win) isNull printString << ' set ' << (SDL2 xSetMouseRect: win r: (i32 value: #(1 2 3 4))) printString << ' get ' << ((rd value: (SDL2 xMouseRect: win) value: 16) in: [ :b | (1 to: 4) collect: [ :k | b signedLongAt: k - 1 * 4 + 1 ] ]) printString << ' clear ' << (SDL2 xSetMouseRect: win r: nil) printString << (SDL2 xMouseRect: win) isNull printString; nextPut: Character lf.
out << 'display name ' << (SDL2 xDisplayName: 0) isNil not printString << ' bad ' << (SDL2 xDisplayName: 5) printString << ' ' << SDL2 xGetError printString; nextPut: Character lf. SDL2 xClearError.
out << 'closest mode ' << (SDL2 xClosestMode: 0 m: nil c: (ByteArray new: 24)) isNull printString << ' ' << SDL2 xGetError printString; nextPut: Character lf. SDL2 xClearError.
out << 'screensaver ' << SDL2 xScreenSaver printString.
SDL2 xDisableSaver. out << ' disabled ' << SDL2 xScreenSaver printString.
SDL2 xEnableSaver. out << ' enabled ' << SDL2 xScreenSaver printString; nextPut: Character lf.
out << 'GL context ' << (SDL2 xGLContext: win) isNull printString << ' ' << SDL2 xGetError printString; nextPut: Character lf. SDL2 xClearError.
SDL2 xGLSwap: win. out << 'GL swap ' << SDL2 xGetError printString; nextPut: Character lf. SDL2 xClearError.
out << 'Vulkan ext ' << (SDL2 xVkExt: win c: (ByteArray new: 4) n: nil) printString << ' ' << SDL2 xGetError printString; nextPut: Character lf. SDL2 xClearError.
out << 'hit test ' << (SDL2 xHitTest: win cb: nil d: nil) printString << ' ' << SDL2 xGetError printString << ' bad window ' << (SDL2 xHitTest: nil cb: nil d: nil) printString << ' ' << SDL2 xGetError printString; nextPut: Character lf. SDL2 xClearError.
out << 'cursor ' << (SDL2 xShowCursor: -1) printString << (SDL2 xShowCursor: 0) printString << (SDL2 xShowCursor: -1) printString << (SDL2 xShowCursor: 1) printString << (SDL2 xShowCursor: -1) printString; nextPut: Character lf.
out << 'primary selection ' << SDL2 xPrimaryHas printString << ' set ' << (SDL2 xPrimarySet: 'text one') printString << ' has ' << SDL2 xPrimaryHas printString; nextPut: Character lf.
out << 'wasinit ' << ((SDL2 xWasInit: 16r20) bitAnd: 16r20) printString << ' video+events ' << ((SDL2 xWasInit: 16r4020) bitAnd: 16r4020) printString; nextPut: Character lf.

"---- text conversion ----"
mark value: 'iconv'.
#( #('UTF-16LE' 'UTF-8' 'A') #('UTF-8' 'UTF-8' 'plain') #('ASCII' 'UTF-8' 'abc') #('UCS-4BE' 'UTF-8' 'xy') #('UTF-8' 'UTF-16BE' nil) #('UTF-8' 'KLINGON' 'x') ) do: [ :c | | in p n w |
  in := c last ifNil: [ #( 0 65 0 66 ) asByteArray ] ifNotNil: [ :t | t asByteArray ].
  p := SDL2 xIconv: c first from: (c at: 2) in: in len: in size.
  w := ((c first beginsWith: 'UTF-16') or: [ c first beginsWith: 'UCS-2' ]) ifTrue: [ 2 ] ifFalse: [ ((c first beginsWith: 'UCS-4') or: [ c first beginsWith: 'UTF-32' ]) ifTrue: [ 4 ] ifFalse: [ 1 ] ].
  out << 'iconv ' << c allButLast printString << ' -> ' << (p isNull ifTrue: [ 'NULL' ] ifFalse: [ n := 0. [ ((rd value: (ExternalAddress fromAddress: p asInteger + n) value: w) allSatisfy: [ :b | b = 0 ]) not ] whileTrue: [ n := n + w ]. hex value: (rd value: p value: n + w) ]); nextPut: Character lf.
  p isNull ifFalse: [ SDL2 xFree: p ] ].

"---- families without a device ----"
mark value: 'no device'.
out << 'joysticks ' << SDL2 xNumJoy printString << ' event state ' << (SDL2 xJoyEvent: -1) printString << (SDL2 xJoyEvent: 0) printString << (SDL2 xJoyEvent: -1) printString << (SDL2 xJoyEvent: 1) printString << (SDL2 xJoyEvent: 7) printString; nextPut: Character lf.
out << 'controller event ' << (SDL2 xCtrlEvent: -1) printString; nextPut: Character lf.
out << 'axis strings ' << (SDL2 xAxisFromString: 'leftx') printString << (SDL2 xAxisFromString: '+RIGHTY') printString << (SDL2 xAxisFromString: 'bogus') printString << (SDL2 xAxisFromString: '') printString << ' ' << (SDL2 xAxisString: 4) printString << (SDL2 xAxisString: 9) printString << ' button ' << (SDL2 xButtonString: 0) printString << (SDL2 xButtonString: 20) printString << (SDL2 xButtonString: 21) printString; nextPut: Character lf.
out << 'touch ' << SDL2 xNumTouch printString << ' device ' << (SDL2 xTouchDevice: 3) printString << ' ' << SDL2 xGetError printString; nextPut: Character lf. SDL2 xClearError.
out << 'audio devices name ' << (SDL2 xAudioDeviceName: 0 c: 0) isNil printString << ' ' << SDL2 xGetError printString; nextPut: Character lf. SDL2 xClearError.
out << 'hid ' << SDL2 xHidInit printString << ' enumerate ' << (SDL2 xHidEnum: 0 p: 0) isNull printString; nextPut: Character lf.
out contents
