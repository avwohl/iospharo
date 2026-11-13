"sdl2diff_render.st -- software renderer: draw calls, scale, viewport, clip, logical size, RenderCopyEx.
Run on stock Cog (real libSDL2, SDL_VIDEODRIVER=dummy) and on our VM; the two answers must match.
Every call goes through ffiCall methods that this script compiles on the SDL2 class (the image has none
for most of them).  Each case draws into a 16 x 16 render target texture and prints its pixels.
Run: see scripts/cogdiff/README-sdl2diff.md  (docs/sdl2-stubs.md)"

| out win r tex defs fmt rect pts32 fpts32 line dump case clearTo |
out := WriteStream on: String new.
defs := OrderedCollection new.
defs
  add: 'xCreateWin: t x: x y: y w: w h: h flags: f ^ self ffiCall: #( void* SDL_CreateWindow ( String t, int x, int y, int w, int h, uint32 f ) )';
  add: 'xCreateRenderer: win index: i flags: f ^ self ffiCall: #( void* SDL_CreateRenderer ( void* win, int i, uint32 f ) )';
  add: 'xCreateTex: r fmt: f access: a w: w h: h ^ self ffiCall: #( void* SDL_CreateTexture ( void* r, uint32 f, int a, int w, int h ) )';
  add: 'xUpdateTex: t rect: rc pixels: p pitch: pitch ^ self ffiCall: #( int SDL_UpdateTexture ( void* t, void* rc, void* p, int pitch ) )';
  add: 'xSetTarget: r tex: t ^ self ffiCall: #( int SDL_SetRenderTarget ( void* r, void* t ) )';
  add: 'xSetColor: r r: rr g: g b: b a: a ^ self ffiCall: #( int SDL_SetRenderDrawColor ( void* r, uint8 rr, uint8 g, uint8 b, uint8 a ) )';
  add: 'xSetBlend: r mode: m ^ self ffiCall: #( int SDL_SetRenderDrawBlendMode ( void* r, uint32 m ) )';
  add: 'xTexBlend: t mode: m ^ self ffiCall: #( int SDL_SetTextureBlendMode ( void* t, uint32 m ) )';
  add: 'xTexMod: t r: r g: g b: b ^ self ffiCall: #( int SDL_SetTextureColorMod ( void* t, uint8 r, uint8 g, uint8 b ) )';
  add: 'xTexAlpha: t a: a ^ self ffiCall: #( int SDL_SetTextureAlphaMod ( void* t, uint8 a ) )';
  add: 'xClear: r ^ self ffiCall: #( int SDL_RenderClear ( void* r ) )';
  add: 'xReadPixels: r rect: rc fmt: f pixels: p pitch: pitch ^ self ffiCall: #( int SDL_RenderReadPixels ( void* r, void* rc, uint32 f, void* p, int pitch ) )';
  add: 'xSetScale: r x: sx y: sy ^ self ffiCall: #( int SDL_RenderSetScale ( void* r, float sx, float sy ) )';
  add: 'xGetScale: r x: sx y: sy ^ self ffiCall: #( void SDL_RenderGetScale ( void* r, void* sx, void* sy ) )';
  add: 'xSetViewport: r rect: b ^ self ffiCall: #( int SDL_RenderSetViewport ( void* r, void* b ) )';
  add: 'xGetViewport: r rect: b ^ self ffiCall: #( void SDL_RenderGetViewport ( void* r, void* b ) )';
  add: 'xSetClip: r rect: b ^ self ffiCall: #( int SDL_RenderSetClipRect ( void* r, void* b ) )';
  add: 'xGetClip: r rect: b ^ self ffiCall: #( void SDL_RenderGetClipRect ( void* r, void* b ) )';
  add: 'xIsClip: r ^ self ffiCall: #( int SDL_RenderIsClipEnabled ( void* r ) )';
  add: 'xSetLogical: r w: w h: h ^ self ffiCall: #( int SDL_RenderSetLogicalSize ( void* r, int w, int h ) )';
  add: 'xGetLogical: r w: w h: h ^ self ffiCall: #( void SDL_RenderGetLogicalSize ( void* r, void* w, void* h ) )';
  add: 'xSetInt: r on: f ^ self ffiCall: #( int SDL_RenderSetIntegerScale ( void* r, int f ) )';
  add: 'xGetInt: r ^ self ffiCall: #( int SDL_RenderGetIntegerScale ( void* r ) )';
  add: 'xW2L: r x: x y: y lx: lx ly: ly ^ self ffiCall: #( void SDL_RenderWindowToLogical ( void* r, int x, int y, void* lx, void* ly ) )';
  add: 'xL2W: r x: x y: y wx: wx wy: wy ^ self ffiCall: #( void SDL_RenderLogicalToWindow ( void* r, float x, float y, void* wx, void* wy ) )';
  add: 'xPoints: r p: p n: n ^ self ffiCall: #( int SDL_RenderDrawPoints ( void* r, void* p, int n ) )';
  add: 'xPointsF: r p: p n: n ^ self ffiCall: #( int SDL_RenderDrawPointsF ( void* r, void* p, int n ) )';
  add: 'xLines: r p: p n: n ^ self ffiCall: #( int SDL_RenderDrawLines ( void* r, void* p, int n ) )';
  add: 'xLinesF: r p: p n: n ^ self ffiCall: #( int SDL_RenderDrawLinesF ( void* r, void* p, int n ) )';
  add: 'xRects: r p: p n: n ^ self ffiCall: #( int SDL_RenderDrawRects ( void* r, void* p, int n ) )';
  add: 'xRectsF: r p: p n: n ^ self ffiCall: #( int SDL_RenderDrawRectsF ( void* r, void* p, int n ) )';
  add: 'xFill: r p: p n: n ^ self ffiCall: #( int SDL_RenderFillRects ( void* r, void* p, int n ) )';
  add: 'xFillF: r p: p n: n ^ self ffiCall: #( int SDL_RenderFillRectsF ( void* r, void* p, int n ) )';
  add: 'xRectF: r p: p ^ self ffiCall: #( int SDL_RenderDrawRectF ( void* r, void* p ) )';
  add: 'xFillRect: r p: p ^ self ffiCall: #( int SDL_RenderFillRect ( void* r, void* p ) )';
  add: 'xDrawRect: r p: p ^ self ffiCall: #( int SDL_RenderDrawRect ( void* r, void* p ) )';
  add: 'xLineF: r x1: x1 y1: y1 x2: x2 y2: y2 ^ self ffiCall: #( int SDL_RenderDrawLineF ( void* r, float x1, float y1, float x2, float y2 ) )';
  add: 'xLine: r x1: x1 y1: y1 x2: x2 y2: y2 ^ self ffiCall: #( int SDL_RenderDrawLine ( void* r, int x1, int y1, int x2, int y2 ) )';
  add: 'xPointF: r x: x y: y ^ self ffiCall: #( int SDL_RenderDrawPointF ( void* r, float x, float y ) )';
  add: 'xCopy: r t: t s: s d: d ^ self ffiCall: #( int SDL_RenderCopy ( void* r, void* t, void* s, void* d ) )';
  add: 'xCopyF: r t: t s: s d: d ^ self ffiCall: #( int SDL_RenderCopyF ( void* r, void* t, void* s, void* d ) )';
  add: 'xCopyEx: r t: t s: s d: d a: a c: c f: f ^ self ffiCall: #( int SDL_RenderCopyEx ( void* r, void* t, void* s, void* d, double a, void* c, int f ) )';
  add: 'xCopyExF: r t: t s: s d: d a: a c: c f: f ^ self ffiCall: #( int SDL_RenderCopyExF ( void* r, void* t, void* s, void* d, double a, void* c, int f ) )';
  add: 'xSetHint: n v: v ^ self ffiCall: #( int SDL_SetHint ( String n, String v ) )';
  add: 'xTexScale: t mode: m ^ self ffiCall: #( int SDL_SetTextureScaleMode ( void* t, int m ) )'.
defs do: [ :d | SDL2 class compile: d classified: 'x' ].

pts32 := [ :nums | | b | b := ByteArray new: nums size * 4. nums withIndexDo: [ :n :i | b signedLongAt: i - 1 * 4 + 1 put: n ]. b ].
fpts32 := [ :nums | | b | b := ByteArray new: nums size * 4. nums withIndexDo: [ :n :i | b float32AtOffset: i - 1 * 4 put: n ]. b ].
dump := [ :label :rr | | px s |
  px := ByteArray new: 16 * 16 * 4.
  SDL2 xReadPixels: rr rect: nil fmt: 372645892 pixels: px pitch: 64.
  s := WriteStream on: String new.
  1 to: 16 do: [ :y | 1 to: 16 do: [ :x | | v | v := px unsignedLongAt: (y - 1 * 16 + x - 1) * 4 + 1. s nextPutAll: (v printStringBase: 16 ) ; space ]. s nextPut: Character lf ].
  out nextPutAll: '== '; nextPutAll: label; nextPut: Character lf; nextPutAll: s contents ].
clearTo := [ :rr :c | SDL2 xSetBlend: rr mode: 0. SDL2 xSetColor: rr r: (c at: 1) g: (c at: 2) b: (c at: 3) a: (c at: 4). SDL2 xClear: rr. SDL2 xSetViewport: rr rect: nil. SDL2 xSetClip: rr rect: nil. SDL2 xSetScale: rr x: 1.0 y: 1.0 ].

win := SDL2 xCreateWin: 'x' x: 10 y: 10 w: 64 h: 48 flags: 8.
r := SDL2 xCreateRenderer: win index: -1 flags: 1.
tex := SDL2 xCreateTex: r fmt: 372645892 access: 2 w: 16 h: 16.
out << 'renderer ' << (r isNil | r isNull) printString; nextPut: Character lf.
SDL2 xSetTarget: r tex: tex.

"-- points, lines, rects at scale 1 --"
clearTo value: r value: #(0 0 0 255).
SDL2 xSetColor: r r: 255 g: 255 b: 255 a: 255.
SDL2 xPoints: r p: (pts32 value: #(1 1 14 14 0 15 20 20 -1 3)) n: 5.
dump value: 'points' value: r.

clearTo value: r value: #(0 0 0 255).
SDL2 xSetColor: r r: 255 g: 0 b: 0 a: 255.
SDL2 xLines: r p: (pts32 value: #(0 0 15 7 3 12 3 2 12 12)) n: 5.
dump value: 'lines' value: r.

clearTo value: r value: #(0 0 0 255).
SDL2 xSetColor: r r: 0 g: 255 b: 0 a: 255.
SDL2 xLine: r x1: 2 y1: 2 x2: 13 y2: 4.
SDL2 xLine: r x1: 13 y1: 6 x2: 2 y2: 6.
SDL2 xLine: r x1: 2 y1: 14 x2: 6 y2: 8.
SDL2 xLine: r x1: 8 y1: 8 x2: 8 y2: 14.
SDL2 xLine: r x1: -5 y1: 3 x2: 20 y2: 12.
dump value: 'line-singles' value: r.

clearTo value: r value: #(0 0 0 255).
SDL2 xSetColor: r r: 0 g: 0 b: 255 a: 255.
SDL2 xDrawRect: r p: (pts32 value: #(2 2 8 6)).
SDL2 xDrawRect: r p: nil.
SDL2 xFillRect: r p: (pts32 value: #(5 5 3 3)).
SDL2 xFillRect: r p: (pts32 value: #(12 12 0 0)).
dump value: 'rects' value: r.

clearTo value: r value: #(0 0 0 255).
SDL2 xSetColor: r r: 200 g: 100 b: 50 a: 255.
SDL2 xPointsF: r p: (fpts32 value: #(1.5 1.5 3.7 3.2 5.0 5.0)) n: 3.
SDL2 xLinesF: r p: (fpts32 value: #(1.2 8.7 10.6 8.2 12.4 14.9)) n: 3.
SDL2 xFillF: r p: (fpts32 value: #(10.5 1.5 3.4 2.6)) n: 1.
SDL2 xRectF: r p: (fpts32 value: #(0.5 10.5 4.4 4.4)).
dump value: 'float-calls' value: r.

"-- blended lines and rects --"
clearTo value: r value: #(20 40 60 255).
SDL2 xSetBlend: r mode: 1.
SDL2 xSetColor: r r: 255 g: 255 b: 0 a: 128.
SDL2 xLines: r p: (pts32 value: #(1 1 14 1 14 14 1 14 1 1)) n: 5.
SDL2 xLines: r p: (pts32 value: #(1 3 14 3 14 12)) n: 3.
SDL2 xFill: r p: (pts32 value: #(4 4 8 8 6 6 8 8)) n: 2.
dump value: 'blend-lines' value: r.

"-- scale --"
clearTo value: r value: #(0 0 0 255).
SDL2 xSetScale: r x: 2.0 y: 2.0.
SDL2 xSetColor: r r: 255 g: 0 b: 255 a: 255.
SDL2 xPoints: r p: (pts32 value: #(1 1 3 2)) n: 2.
SDL2 xLines: r p: (pts32 value: #(0 5 6 5 6 7)) n: 3.
SDL2 xLines: r p: (pts32 value: #(1 4 3 7)) n: 2.
SDL2 xFill: r p: (pts32 value: #(4 0 2 2)) n: 1.
SDL2 xDrawRect: r p: (pts32 value: #(5 5 2 2)).
dump value: 'scale2' value: r.
clearTo value: r value: #(0 0 0 255).
SDL2 xSetScale: r x: 1.5 y: 0.5.
SDL2 xSetColor: r r: 255 g: 128 b: 0 a: 255.
SDL2 xFill: r p: (pts32 value: #(2 2 6 20)) n: 1.
SDL2 xPoints: r p: (pts32 value: #(3 3)) n: 1.
dump value: 'scale-frac' value: r.
(fpts32 value: #(0.0 0.0)) in: [ :a | SDL2 xGetScale: r x: a y: (ByteArray new: 4) . out << 'getScale ' << (a float32AtOffset: 0) printString; nextPut: Character lf ].

"-- viewport and clip --"
clearTo value: r value: #(0 0 0 255).
SDL2 xSetViewport: r rect: (pts32 value: #(4 3 8 8)).
SDL2 xSetColor: r r: 255 g: 255 b: 255 a: 255.
SDL2 xFill: r p: nil n: 0.
SDL2 xFillRect: r p: nil.
SDL2 xSetColor: r r: 255 g: 0 b: 0 a: 255.
SDL2 xDrawRect: r p: nil.
SDL2 xSetClip: r rect: (pts32 value: #(2 2 4 4)).
SDL2 xSetColor: r r: 0 g: 255 b: 0 a: 255.
SDL2 xFillRect: r p: (pts32 value: #(0 0 20 20)).
dump value: 'viewport-clip' value: r.
(ByteArray new: 16) in: [ :vp | SDL2 xGetViewport: r rect: vp. out << 'viewport ' << (1 to: 4) asArray printString; nextPut: Character lf.
  out << ((1 to: 4) collect: [ :i | vp signedLongAt: i - 1 * 4 + 1 ]) printString; nextPut: Character lf ].
(ByteArray new: 16) in: [ :vp | SDL2 xGetClip: r rect: vp. out << 'clip ' << ((1 to: 4) collect: [ :i | vp signedLongAt: i - 1 * 4 + 1 ]) printString << ' ' << (SDL2 xIsClip: r) printString; nextPut: Character lf ].
SDL2 xSetClip: r rect: nil. SDL2 xSetViewport: r rect: nil.

"-- logical size --"
SDL2 xSetTarget: r tex: nil.
SDL2 xSetLogical: r w: 32 h: 24.
(ByteArray new: 16) in: [ :vp | SDL2 xGetViewport: r rect: vp. out << 'logical-viewport ' << ((1 to: 4) collect: [ :i | vp signedLongAt: i - 1 * 4 + 1 ]) printString; nextPut: Character lf ].
(ByteArray new: 8) in: [ :s | SDL2 xGetScale: r x: s y: nil. out << 'logical-scale ' << (s float32AtOffset: 0) printString; nextPut: Character lf ].
(ByteArray new: 8) in: [ :a | | b | b := ByteArray new: 4. SDL2 xGetLogical: r w: a h: b. out << 'getlogical ' << (a signedLongAt: 1) printString << ' ' << (b signedLongAt: 1) printString; nextPut: Character lf ].
(ByteArray new: 4) in: [ :lx | | ly | ly := ByteArray new: 4. SDL2 xW2L: r x: 40 y: 30 lx: lx ly: ly. out << 'w2l ' << (lx float32AtOffset: 0) printString << ' ' << (ly float32AtOffset: 0) printString; nextPut: Character lf ].
(ByteArray new: 4) in: [ :wx | | wy | wy := ByteArray new: 4. SDL2 xL2W: r x: 10.0 y: 5.0 wx: wx wy: wy. out << 'l2w ' << (wx signedLongAt: 1) printString << ' ' << (wy signedLongAt: 1) printString; nextPut: Character lf ].
SDL2 xSetInt: r on: 1.
out << 'int ' << (SDL2 xGetInt: r) printString; nextPut: Character lf.
(ByteArray new: 16) in: [ :vp | SDL2 xGetViewport: r rect: vp. out << 'int-viewport ' << ((1 to: 4) collect: [ :i | vp signedLongAt: i - 1 * 4 + 1 ]) printString; nextPut: Character lf ].
SDL2 xSetInt: r on: 0.
SDL2 xSetLogical: r w: 0 h: 0.
(ByteArray new: 16) in: [ :vp | SDL2 xGetViewport: r rect: vp. out << 'reset-viewport ' << ((1 to: 4) collect: [ :i | vp signedLongAt: i - 1 * 4 + 1 ]) printString; nextPut: Character lf ].

"-- RenderCopyEx: a 6x4 test texture, copied rotated and flipped --"
SDL2 xSetTarget: r tex: tex.
fmt := SDL2 xCreateTex: r fmt: 372645892 access: 0 w: 6 h: 4.
(ByteArray new: 6 * 4 * 4) in: [ :px |
  0 to: 3 do: [ :y | 0 to: 5 do: [ :x | px unsignedLongAt: (y * 6 + x) * 4 + 1 put: (16rFF000000 + (x * 40 << 16) + (y * 60 << 8) + (x + y * 20)) ] ].
  SDL2 xUpdateTex: fmt rect: nil pixels: px pitch: 24 ].
#( 0 90 180 270 45 30 -30 135 ) do: [ :ang |
  clearTo value: r value: #(10 10 10 255).
  SDL2 xCopyEx: r t: fmt s: nil d: (pts32 value: #(5 6 6 4)) a: ang asFloat c: nil f: 0.
  dump value: 'copyex-', ang printString value: r ].
#( 1 2 3 ) do: [ :fl |
  clearTo value: r value: #(10 10 10 255).
  SDL2 xCopyEx: r t: fmt s: nil d: (pts32 value: #(5 6 6 4)) a: 0.0 c: nil f: fl.
  dump value: 'copyex-flip', fl printString value: r.
  clearTo value: r value: #(10 10 10 255).
  SDL2 xCopyEx: r t: fmt s: nil d: (pts32 value: #(5 6 6 4)) a: 90.0 c: nil f: fl.
  dump value: 'copyex-flip90-', fl printString value: r ].
clearTo value: r value: #(10 10 10 255).
SDL2 xCopyEx: r t: fmt s: (pts32 value: #(1 1 4 2)) d: (pts32 value: #(2 3 8 6)) a: 30.0 c: (pts32 value: #(1 1)) f: 0.
dump value: 'copyex-src-center' value: r.
clearTo value: r value: #(10 10 10 255).
SDL2 xTexBlend: fmt mode: 1. SDL2 xTexAlpha: fmt a: 128. SDL2 xTexMod: fmt r: 200 g: 255 b: 100.
SDL2 xCopyEx: r t: fmt s: nil d: (pts32 value: #(3 3 9 6)) a: 20.0 c: nil f: 0.
dump value: 'copyex-blend-mod' value: r.
SDL2 xTexBlend: fmt mode: 0. SDL2 xTexAlpha: fmt a: 255. SDL2 xTexMod: fmt r: 255 g: 255 b: 255.
clearTo value: r value: #(10 10 10 255).
SDL2 xSetScale: r x: 2.0 y: 1.0.
SDL2 xCopyEx: r t: fmt s: nil d: (pts32 value: #(1 3 4 3)) a: 25.0 c: nil f: 0.
dump value: 'copyex-scale' value: r.
clearTo value: r value: #(10 10 10 255).
SDL2 xCopyExF: r t: fmt s: nil d: (fpts32 value: #(2.5 3.5 6.0 4.0)) a: 60.0 c: (fpts32 value: #(1.0 1.0)) f: 2.
dump value: 'copyexf' value: r.
SDL2 xTexScale: fmt mode: 1.
clearTo value: r value: #(10 10 10 255).
SDL2 xCopyEx: r t: fmt s: nil d: (pts32 value: #(2 2 9 7)) a: 33.0 c: nil f: 0.
dump value: 'copyex-linear' value: r.
clearTo value: r value: #(10 10 10 255).
SDL2 xCopy: r t: fmt s: nil d: (pts32 value: #(100 100 5 5)).
SDL2 xCopy: r t: fmt s: nil d: (pts32 value: #(-3 -2 12 8)).
dump value: 'copy-outside-and-partial' value: r.
out contents
