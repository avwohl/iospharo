"sdl2diff_surfaces.st -- SDL2 surfaces of every packed, indexed and 24-bit format: fields, fill, conversion,
palette, blit, BMP save and load; plus the answers of the 2026-11-13 review fixes (SDL_PeepEvents with no buffer,
OpenGL attributes, window flags for graphics APIs, texture creation errors, clipboard).
Run on stock Cog (real libSDL2, SDL_VIDEODRIVER=dummy) and on our VM; the answers must match."

| out defs mark fmts hex px put mk fields dump rwmem t |
out := WriteStream on: String new.
defs := OrderedCollection new.
defs
  add: 'xInit: f ^ self ffiCall: #( int SDL_Init ( uint32 f ) )';
  add: 'xGetError ^ self ffiCall: #( String SDL_GetError ( ) )';
  add: 'xClearError ^ self ffiCall: #( void SDL_ClearError ( ) )';
  add: 'xMemcpy: d s: s n: n ^ self ffiCall: #( void* SDL_memcpy ( void* d, void* s, size_t n ) )';
  add: 'xCreateSurf: f w: w h: h d: d r: r g: g b: b a: a ^ self ffiCall: #( void* SDL_CreateRGBSurface ( uint32 f, int w, int h, int d, uint32 r, uint32 g, uint32 b, uint32 a ) )';
  add: 'xCreateSurfF: f w: w h: h d: d fmt: fmt ^ self ffiCall: #( void* SDL_CreateRGBSurfaceWithFormat ( uint32 f, int w, int h, int d, uint32 fmt ) )';
  add: 'xFill: s r: r c: c ^ self ffiCall: #( int SDL_FillRect ( void* s, void* r, uint32 c ) )';
  add: 'xBlit: s r: sr d: d r: dr ^ self ffiCall: #( int SDL_UpperBlit ( void* s, void* sr, void* d, void* dr ) )';
  add: 'xConvert: s f: f fl: fl ^ self ffiCall: #( void* SDL_ConvertSurfaceFormat ( void* s, uint32 f, uint32 fl ) )';
  add: 'xDup: s ^ self ffiCall: #( void* SDL_DuplicateSurface ( void* s ) )';
  add: 'xFree: s ^ self ffiCall: #( void SDL_FreeSurface ( void* s ) )';
  add: 'xAllocPalette: n ^ self ffiCall: #( void* SDL_AllocPalette ( int n ) )';
  add: 'xSetPaletteColors: p c: c first: f n: n ^ self ffiCall: #( int SDL_SetPaletteColors ( void* p, void* c, int f, int n ) )';
  add: 'xSetSurfPalette: s p: p ^ self ffiCall: #( int SDL_SetSurfacePalette ( void* s, void* p ) )';
  add: 'xRWFromMem: m n: n ^ self ffiCall: #( void* SDL_RWFromMem ( void* m, int n ) )';
  add: 'xRWFromConstMem: m n: n ^ self ffiCall: #( void* SDL_RWFromConstMem ( void* m, int n ) )';
  add: 'xRWseek: r o: o w: w ^ self ffiCall: #( int64 SDL_RWseek ( void* r, int64 o, int w ) )';
  add: 'xRWclose: r ^ self ffiCall: #( int SDL_RWclose ( void* r ) )';
  add: 'xSaveBMP: s rw: r free: f ^ self ffiCall: #( int SDL_SaveBMP_RW ( void* s, void* r, int f ) )';
  add: 'xLoadBMP: r free: f ^ self ffiCall: #( void* SDL_LoadBMP_RW ( void* r, int f ) )';
  add: 'xPeep: ev n: n action: a min: lo max: hi ^ self ffiCall: #( int SDL_PeepEvents ( void* ev, int n, int a, uint32 lo, uint32 hi ) )';
  add: 'xPush: ev ^ self ffiCall: #( int SDL_PushEvent ( void* ev ) )';
  add: 'xFlushEvents: lo hi: hi ^ self ffiCall: #( void SDL_FlushEvents ( uint32 lo, uint32 hi ) )';
  add: 'xGLGet: a v: v ^ self ffiCall: #( int SDL_GL_GetAttribute ( int a, void* v ) )';
  add: 'xGLSet: a v: v ^ self ffiCall: #( int SDL_GL_SetAttribute ( int a, int v ) )';
  add: 'xGLReset ^ self ffiCall: #( void SDL_GL_ResetAttributes ( ) )';
  add: 'xGLExt: n ^ self ffiCall: #( int SDL_GL_ExtensionSupported ( String n ) )';
  add: 'xCreateWin: t x: x y: y w: w h: h flags: f ^ self ffiCall: #( void* SDL_CreateWindow ( String t, int x, int y, int w, int h, uint32 f ) )';
  add: 'xWinFlags: w ^ self ffiCall: #( uint32 SDL_GetWindowFlags ( void* w ) )';
  add: 'xCreateRenderer: win index: i flags: f ^ self ffiCall: #( void* SDL_CreateRenderer ( void* win, int i, uint32 f ) )';
  add: 'xCreateTexture: r f: f a: a w: w h: h ^ self ffiCall: #( void* SDL_CreateTexture ( void* r, uint32 f, int a, int w, int h ) )';
  add: 'xSetClip: t ^ self ffiCall: #( int SDL_SetClipboardText ( String t ) )';
  add: 'xGetClip ^ self ffiCall: #( String SDL_GetClipboardText ( ) )';
  add: 'xHasClip ^ self ffiCall: #( int SDL_HasClipboardText ( ) )'.
defs do: [ :d | [ SDL2 class compile: d classified: 'x' ] on: Error do: [ :e | out << 'COMPILEFAIL ' << d; nextPut: Character lf ] ].
mark := [ :s | out << '== ' << s; nextPut: Character lf ].
hex := [ :bytes | | s | s := WriteStream on: String new. bytes do: [ :b | s nextPutAll: (b printStringBase: 16 length: 2 padded: true) ] separatedBy: [ s space ]. s contents ].
px := [ :sf :n | | p | p := sf pointerAt: 33. (1 to: (n min: (sf signedLongAt: 25) * (sf signedLongAt: 21))) collect: [ :i | p unsignedByteAt: i ] ].
put := [ :sf :bytes | | p room | p := sf pointerAt: 33. room := (sf signedLongAt: 25) * (sf signedLongAt: 21). bytes doWithIndex: [ :b :i | i <= room ifTrue: [ p unsignedByteAt: i put: b ] ] ].
mk := [ :f :w :h | SDL2 xCreateSurfF: 0 w: w h: h d: 0 fmt: f ].
fields := [ :sf | | f pal | f := sf pointerAt: 9. pal := f pointerAt: 9.
  { 'fmt' . f unsignedLongAt: 1. 'pal' . pal isNull . 'bpp' . f unsignedByteAt: 17. 'bytes' . f unsignedByteAt: 18.
    'masks' . (f unsignedLongAt: 21) printStringBase: 16 . (f unsignedLongAt: 25) printStringBase: 16 . (f unsignedLongAt: 29) printStringBase: 16 . (f unsignedLongAt: 33) printStringBase: 16.
    'wh' . sf signedLongAt: 17 . sf signedLongAt: 21 . 'pitch' . sf signedLongAt: 25 . 'flags' . sf unsignedLongAt: 1 } ].
dump := [ :label :blk | | r |
  SDL2 xClearError. Stdio stdout nextPutAll: 'TRY ', label; lf; flush.
  r := [ blk value ] on: Error do: [ :e | 'EXC ' , e class name ].
  r isString ifFalse: [ r isNil ifTrue: [ r := 'nil' ] ifFalse: [ (r isKindOf: ExternalAddress) ifTrue: [ r := r isNull ifTrue: [ 'null' ] ifFalse: [ 'nonnull' ] ] ] ].
  out << label << ' -> ' << r printString << ' | ' << SDL2 xGetError printString; nextPut: Character lf ].
SDL2 xInit: 16r4021.
fmts := #( 286261504 287310080 303039488 304088064 318769153 336660481 353570562 353701890 357896194 355602434 355667970 356782082 386930691 390076419 370546692 372645892 372711428 376840196 ).

mark value: 'surface fields'.
fmts do: [ :f | dump value: 'fields ', f printString value: [ | sf | sf := mk value: f value: 5 value: 3. sf isNull ifTrue: [ 'NULL' ] ifFalse: [ fields value: sf ] ] ].

mark value: 'default palette'.
#( 286261504 303039488 318769153 ) do: [ :f | dump value: 'palette ', f printString value: [ | sf pal n c | sf := mk value: f value: 2 value: 2. pal := (sf pointerAt: 9) pointerAt: 9.
  n := pal signedLongAt: 1. c := pal pointerAt: 9.
  { n . (c unsignedLongAt: 1) printStringBase: 16 . (c unsignedLongAt: 5) printStringBase: 16 } ] ].

mark value: 'fill'.
fmts do: [ :f | dump value: 'fill ', f printString value: [ | sf r | sf := mk value: f value: 3 value: 2. r := SDL2 xFill: sf r: nil c: 16r00A5C3E1. { r . hex value: (px value: sf value: 8) } ] ].

mark value: 'convert from ARGB8888'.
fmts do: [ :f | dump value: 'convert ', f printString value: [ | src c | src := mk value: 372645892 value: 4 value: 1.
  put value: src value: #( 16r00 16r00 16rFF 16rFF  16r00 16rFF 16r00 16rFF  16rFF 16r00 16r00 16r80  16r80 16r80 16r80 16r00 ).
  c := SDL2 xConvert: src f: f fl: 0. c isNull ifTrue: [ 'NULL' ] ifFalse: [ hex value: (px value: c value: 16) ] ] ].

mark value: 'convert to ARGB8888'.
fmts do: [ :f | dump value: 'back ', f printString value: [ | src c | src := mk value: f value: 3 value: 1.
  put value: src value: #( 16r12 16r34 16r56 16r78 16r9A 16rBC 16rDE 16rF0 16r11 16r22 16r33 16r44 ).
  c := SDL2 xConvert: src f: 372645892 fl: 0. c isNull ifTrue: [ 'NULL' ] ifFalse: [ hex value: (px value: c value: 12) ] ] ].

mark value: 'palette and indexed convert'.
dump value: 'indexed to ARGB' value: [ | sf pal cols c |
  sf := mk value: 318769153 value: 4 value: 1. pal := SDL2 xAllocPalette: 4.
  cols := ByteArray withAll: #( 255 0 0 255  0 255 0 255  0 0 255 255  10 20 30 128 ).
  SDL2 xSetPaletteColors: pal c: cols first: 0 n: 4. SDL2 xSetSurfPalette: sf p: pal.
  put value: sf value: #( 0 1 2 3 ).
  c := SDL2 xConvert: sf f: 372645892 fl: 0. hex value: (px value: c value: 16) ].
dump value: 'duplicate indexed' value: [ | sf pal cols c |
  sf := mk value: 318769153 value: 4 value: 1. pal := SDL2 xAllocPalette: 4.
  cols := ByteArray withAll: #( 255 0 0 255  0 255 0 255  0 0 255 255  10 20 30 128 ).
  SDL2 xSetPaletteColors: pal c: cols first: 0 n: 4. SDL2 xSetSurfPalette: sf p: pal.
  put value: sf value: #( 3 2 1 0 ).
  c := SDL2 xDup: sf. { hex value: (px value: c value: 4) . (((c pointerAt: 9) pointerAt: 9) pointerAt: 9) unsignedLongAt: 13 } ].
dump value: 'RGB to INDEX8' value: [ | src c | src := mk value: 372645892 value: 2 value: 1.
  put value: src value: #( 0 0 255 255  0 255 0 255 ).
  c := SDL2 xConvert: src f: 318769153 fl: 0. c isNull ifTrue: [ 'NULL' ] ifFalse: [ hex value: (px value: c value: 2) ] ].

mark value: 'blit across formats'.
#( 353701890 386930691 318769153 355602434 ) do: [ :f | dump value: 'blit from ', f printString value: [ | a b |
  a := mk value: f value: 2 value: 1. b := mk value: 372645892 value: 2 value: 1.
  put value: a value: #( 16r12 16r34 16r56 16r78 16r9A 16rBC ).
  { SDL2 xBlit: a r: nil d: b r: nil . hex value: (px value: b value: 8) } ] ].
#( 353701890 386930691 355602434 ) do: [ :f | dump value: 'blit to ', f printString value: [ | a b |
  b := mk value: f value: 2 value: 1. a := mk value: 372645892 value: 2 value: 1.
  put value: a value: #( 16r12 16r34 16r56 16rFF 16r9A 16rBC 16rDE 16rFF ).
  { SDL2 xBlit: a r: nil d: b r: nil . hex value: (px value: b value: 6) } ] ].

mark value: 'bmp round trip'.
#( 318769153 386930691 372645892 370546692 353701890 ) do: [ :f | dump value: 'bmp ', f printString value: [ | sf buf rw n back |
  sf := mk value: f value: 3 value: 2.
  f = 318769153 ifTrue: [ | pal | pal := SDL2 xAllocPalette: 4. SDL2 xSetPaletteColors: pal c: (ByteArray withAll: #( 255 0 0 255  0 255 0 255  0 0 255 255  10 20 30 255 )) first: 0 n: 4. SDL2 xSetSurfPalette: sf p: pal ].
  put value: sf value: ((1 to: (sf signedLongAt: 25) * (sf signedLongAt: 21)) collect: [ :i | i * 9 \\ 4 ]).
  buf := ByteArray new: 4096. rw := SDL2 xRWFromMem: buf n: 4096.
  n := SDL2 xSaveBMP: sf rw: rw free: 0. SDL2 xRWseek: rw o: 0 w: 1. n := SDL2 xRWseek: rw o: 0 w: 1. SDL2 xRWclose: rw.
  rw := SDL2 xRWFromConstMem: buf n: n. back := SDL2 xLoadBMP: rw free: 1.
  { n . hex value: (buf first: (n min: 70)) . back isNull ifTrue: [ 'NULL' ] ifFalse: [ { fields value: back . hex value: (px value: back value: (back signedLongAt: 25) * (back signedLongAt: 21)) } ] } ] ].

mark value: 'review fixes'.
dump value: 'surface width -1' value: [ SDL2 xCreateSurf: 0 w: -1 h: 2 d: 32 r: 16rFF0000 g: 16rFF00 b: 16rFF a: 0 ].
dump value: 'surface height -1' value: [ SDL2 xCreateSurf: 0 w: 2 h: -1 d: 32 r: 16rFF0000 g: 16rFF00 b: 16rFF a: 0 ].
dump value: 'surfaceF width -1' value: [ SDL2 xCreateSurfF: 0 w: -1 h: 2 d: 32 fmt: 372645892 ].
dump value: 'surface unknown masks' value: [ SDL2 xCreateSurf: 0 w: 2 h: 2 d: 32 r: 1 g: 2 b: 4 a: 0 ].
dump value: 'peep NULL does not remove' value: [ | ev b |
  SDL2 xFlushEvents: 0 hi: 65535. ev := ByteArray new: 56. ev unsignedLongAt: 1 put: 16r8000.
  1 to: 5 do: [ :i | SDL2 xPush: ev ].
  b := ByteArray new: 56 * 10.
  { SDL2 xPeep: nil n: 0 action: 2 min: 0 max: 65535 . SDL2 xPeep: b n: 10 action: 2 min: 16r8000 max: 16r8000 . SDL2 xPeep: b n: 10 action: 2 min: 16r8000 max: 16r8000 } ].
-1 to: 28 do: [ :a | dump value: 'GL get ', a printString value: [ | v | v := ByteArray new: 4. v unsignedLongAt: 1 put: 77. { SDL2 xGLGet: a v: v . v signedLongAt: 1 } ] ].
dump value: 'GL set 999' value: [ SDL2 xGLSet: 999 v: 1 ].
dump value: 'GL set then get' value: [ | v | v := ByteArray new: 4. SDL2 xGLSet: 17 v: 3. SDL2 xGLGet: 17 v: v. { v signedLongAt: 1 . SDL2 xGLReset . SDL2 xGLGet: 17 v: v . v signedLongAt: 1 } ].
dump value: 'GL ext' value: [ { SDL2 xGLExt: 'GL_ARB_x' . SDL2 xGetError } ].
dump value: 'GL ext empty' value: [ SDL2 xGLExt: '' ].
dump value: 'window flags opengl' value: [ | w | w := SDL2 xCreateWin: 't' x: 0 y: 0 w: 20 h: 20 flags: 2. w isNull ifTrue: [ 'NULL' ] ifFalse: [ (SDL2 xWinFlags: w) printStringBase: 16 ] ].
dump value: 'window flags vulkan' value: [ | w | w := SDL2 xCreateWin: 't' x: 0 y: 0 w: 20 h: 20 flags: 16r10000000. w isNull ifTrue: [ 'NULL' ] ifFalse: [ 'window' ] ].
dump value: 'window flags metal' value: [ | w | w := SDL2 xCreateWin: 't' x: 0 y: 0 w: 20 h: 20 flags: 16r20000000. w isNull ifTrue: [ 'NULL' ] ifFalse: [ 'window' ] ].
dump value: 'window flags conflict' value: [ | w | w := SDL2 xCreateWin: 't' x: 0 y: 0 w: 20 h: 20 flags: 16r20000002. w isNull ifTrue: [ 'NULL' ] ifFalse: [ 'window' ] ].
dump value: 'texture access 9' value: [ | w r tx | w := SDL2 xCreateWin: 't' x: 0 y: 0 w: 20 h: 20 flags: 0. r := SDL2 xCreateRenderer: w index: -1 flags: 1.
  tx := SDL2 xCreateTexture: r f: 372645892 a: 9 w: 4 h: 4. tx isNull ifTrue: [ 'NULL' ] ifFalse: [ 'texture' ] ].
dump value: 'texture format 0' value: [ | w r tx | w := SDL2 xCreateWin: 't' x: 0 y: 0 w: 20 h: 20 flags: 0. r := SDL2 xCreateRenderer: w index: -1 flags: 1.
  tx := SDL2 xCreateTexture: r f: 0 a: 0 w: 4 h: 4. tx isNull ifTrue: [ 'NULL' ] ifFalse: [ 'texture' ] ].
dump value: 'texture format 7' value: [ | w r tx | w := SDL2 xCreateWin: 't' x: 0 y: 0 w: 20 h: 20 flags: 0. r := SDL2 xCreateRenderer: w index: -1 flags: 1.
  tx := SDL2 xCreateTexture: r f: 7 a: 0 w: 4 h: 4. tx isNull ifTrue: [ 'NULL' ] ifFalse: [ 'texture' ] ].
dump value: 'texture indexed' value: [ | w r tx | w := SDL2 xCreateWin: 't' x: 0 y: 0 w: 20 h: 20 flags: 0. r := SDL2 xCreateRenderer: w index: -1 flags: 1.
  tx := SDL2 xCreateTexture: r f: 318769153 a: 0 w: 4 h: 4. tx isNull ifTrue: [ 'NULL' ] ifFalse: [ 'texture' ] ].
dump value: 'clipboard round trip' value: [ { SDL2 xSetClip: 'hello'. SDL2 xHasClip . SDL2 xGetClip } ].
out contents
