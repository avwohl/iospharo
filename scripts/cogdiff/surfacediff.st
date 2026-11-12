"surfacediff.st -- SurfacePlugin primRegisterSurface:dispatch:surfaceId: with a getSurfaceFormat callback that runs a scavenge.
Run: test_load_image P.image eval with the text of surfacediff.st; for Cog, arch -x86_64 Pharo C.image eval ...
primRegisterSurface calls the Smalltalk callback, so image code runs inside the primitive.
The surfaceId: holder is a young ByteArray and the scavenge moves it.
Cog answers ids that count up from 1.  Before the fix of 2026-11-12 our VM wrote the id through a stale
pointer, so every holder stayed 0 and a live young object got the bytes instead."

| surf disp fn ids |
surf := AthensCairoSurface extent: 10@10.
AthensCairoSurface checkSession.
disp := SQSurfaceDispatch externalNew.
fn := FFICallback
  signature: #(int (void *handle, int* width, int* height, int* depth, int* isMSB))
  block: [ :handle :width :height :depth :isMSB |
     Smalltalk garbageCollectMost. Smalltalk garbageCollectMost.
     width signedLongAt: 1 put: 10. height signedLongAt: 1 put: 10.
     depth signedLongAt: 1 put: 32. isMSB signedLongAt: 1 put: 0. 1 ].
disp getSurfaceFormatFn: fn.
ids := OrderedCollection new.
5 timesRepeat: [ | id ok |
  id := ByteArray new: 4.
  ok := AthensCairoSurface primRegisterSurface: surf getHandle dispatch: disp getHandle surfaceId: id.
  ids add: ok -> (id signedLongAt: 1) ].
ids printString
