"sdl2diff_threads.st -- SDL threads, thread ids, thread local storage, timers and shared objects.
The thread functions and the timer callback are NATIVE C library functions that the script finds with
SDL_LoadObject and SDL_LoadFunction: abs (a thread function that returns its argument), usleep, and fputc
(a timer callback that writes one byte per firing into a FILE).  No Smalltalk callback is involved, because
stock Cog aborts when a callback arrives on a foreign thread in a headless run ('Code zone writing is not
reentrant'); sdl2diff_callbacks.st runs the Smalltalk callback cases on our VM.
Run on stock Cog (SDL_VIDEODRIVER=dummy) and on our VM; the answers must match."

| out defs mark libc fnAbs fnUsleep fnFputc th status sem tmp fp ids t0 elapsed mem results |
out := WriteStream on: String new.
defs := OrderedCollection new.
defs
  add: 'xInit: f ^ self ffiCall: #( int SDL_Init ( uint32 f ) )';
  add: 'xLoadObject: n ^ self ffiCall: #( void* SDL_LoadObject ( String n ) )';
  add: 'xLoadFunction: h name: n ^ self ffiCall: #( void* SDL_LoadFunction ( void* h, String n ) )';
  add: 'xUnloadObject: h ^ self ffiCall: #( void SDL_UnloadObject ( void* h ) )';
  add: 'xCreateThread: fn name: n data: d ^ self ffiCall: #( void* SDL_CreateThread ( void* fn, String n, void* d ) )';
  add: 'xCreateThread: fn name: n stack: s data: d ^ self ffiCall: #( void* SDL_CreateThreadWithStackSize ( void* fn, String n, size_t s, void* d ) )';
  add: 'xWaitThread: t status: s ^ self ffiCall: #( void SDL_WaitThread ( void* t, void* s ) )';
  add: 'xDetachThread: t ^ self ffiCall: #( void SDL_DetachThread ( void* t ) )';
  add: 'xThreadName: t ^ self ffiCall: #( String SDL_GetThreadName ( void* t ) )';
  add: 'xThreadID ^ self ffiCall: #( ulong SDL_ThreadID ( ) )';
  add: 'xGetThreadID: t ^ self ffiCall: #( ulong SDL_GetThreadID ( void* t ) )';
  add: 'xSetPrio: p ^ self ffiCall: #( int SDL_SetThreadPriority ( int p ) )';
  add: 'xTLSCreate ^ self ffiCall: #( uint32 SDL_TLSCreate ( ) )';
  add: 'xTLSSet: id value: v dtor: d ^ self ffiCall: #( int SDL_TLSSet ( uint32 id, void* v, void* d ) )';
  add: 'xTLSGet: id ^ self ffiCall: #( void* SDL_TLSGet ( uint32 id ) )';
  add: 'xCreateSem: n ^ self ffiCall: #( void* SDL_CreateSemaphore ( uint32 n ) )';
  add: 'xSemWaitTimeout: s ms: ms ^ self ffiCall: #( int SDL_SemWaitTimeout ( void* s, uint32 ms ) )';
  add: 'xSemPost: s ^ self ffiCall: #( int SDL_SemPost ( void* s ) )';
  add: 'xSemValue: s ^ self ffiCall: #( uint32 SDL_SemValue ( void* s ) )';
  add: 'xAddTimer: ms cb: cb param: p ^ self ffiCall: #( int SDL_AddTimer ( uint32 ms, void* cb, void* p ) )';
  add: 'xRemoveTimer: id ^ self ffiCall: #( int SDL_RemoveTimer ( int id ) )';
  add: 'xGetError ^ self ffiCall: #( String SDL_GetError ( ) )';
  add: 'xClearError ^ self ffiCall: #( void SDL_ClearError ( ) )';
  add: 'xFopen: p mode: m ^ self ffiCall: #( void* fopen ( String p, String m ) ) library: ''libSystem.B.dylib''';
  add: 'xFclose: f ^ self ffiCall: #( int fclose ( void* f ) ) library: ''libSystem.B.dylib'''.
defs do: [ :d | SDL2 class compile: d classified: 'x' ].
mark := [ :label | out << '== ' << label; nextPut: Character lf ].
SDL2 xInit: 16r4001.

mark value: 'shared objects'.
libc := SDL2 xLoadObject: 'libSystem.B.dylib'.
out << 'load ' << libc isNull not printString; nextPut: Character lf.
fnAbs := SDL2 xLoadFunction: libc name: 'abs'.
fnUsleep := SDL2 xLoadFunction: libc name: 'usleep'.
fnFputc := SDL2 xLoadFunction: libc name: 'fputc'.
out << 'functions ' << (fnAbs isNull | fnUsleep isNull | fnFputc isNull) not printString; nextPut: Character lf.
SDL2 xClearError.
out << 'missing function ' << (SDL2 xLoadFunction: libc name: 'no_such_function_xyz') isNull printString << ' ' << (SDL2 xGetError beginsWith: 'Failed loading no_such_function_xyz') printString; nextPut: Character lf.
SDL2 xClearError.
out << 'missing library ' << (SDL2 xLoadObject: '/nonexistent/lib.dylib') isNull printString << ' ' << (SDL2 xGetError beginsWith: 'Failed loading /nonexistent/lib.dylib') printString; nextPut: Character lf.

mark value: 'threads'.
status := ByteArray new: 4.
th := SDL2 xCreateThread: fnAbs name: 'worker-abs' data: (ExternalAddress fromAddress: 12).
out << 'created ' << th isNull not printString << ' name ' << (SDL2 xThreadName: th) printString << ' id is not ours ' << ((SDL2 xGetThreadID: th) ~= SDL2 xThreadID) printString << ' null id is ours ' << ((SDL2 xGetThreadID: nil) = SDL2 xThreadID) printString; nextPut: Character lf.
SDL2 xWaitThread: th status: status.
out << 'status ' << (status signedLongAt: 1) printString; nextPut: Character lf.
th := SDL2 xCreateThread: fnAbs name: nil data: (ExternalAddress fromAddress: 3).
out << 'unnamed ' << (SDL2 xThreadName: th) printString; nextPut: Character lf.
SDL2 xWaitThread: th status: nil.
"wait for a thread that sleeps"
t0 := Time millisecondClockValue.
th := SDL2 xCreateThread: fnUsleep name: 'sleeper' data: (ExternalAddress fromAddress: 150000).
SDL2 xWaitThread: th status: status.
elapsed := Time millisecondClockValue - t0.
out << 'join waited ' << (elapsed >= 140) printString << ' status ' << (status signedLongAt: 1) printString; nextPut: Character lf.
"stack size"
th := SDL2 xCreateThread: fnAbs name: 'big-stack' stack: 4194304 data: (ExternalAddress fromAddress: 9).
SDL2 xWaitThread: th status: status.
out << 'stack size thread status ' << (status signedLongAt: 1) printString; nextPut: Character lf.
"many threads"
ids := (1 to: 8) collect: [ :k | SDL2 xCreateThread: fnAbs name: 'w', k printString data: (ExternalAddress fromAddress: k * 5) ].
results := ids collect: [ :each | SDL2 xWaitThread: each status: status. status signedLongAt: 1 ].
out << 'eight threads ' << results asArray printString; nextPut: Character lf.
"detach"
th := SDL2 xCreateThread: fnUsleep name: 'detached' data: (ExternalAddress fromAddress: 30000).
SDL2 xDetachThread: th.
(Delay forMilliseconds: 150) wait.
out << 'detached then idle ok'; nextPut: Character lf.
SDL2 xDetachThread: nil. SDL2 xWaitThread: nil status: nil.
out << 'null thread calls ok'; nextPut: Character lf.
out << 'priority ' << (SDL2 xSetPrio: 1) printString << (SDL2 xSetPrio: 0) printString << (SDL2 xSetPrio: 1) printString; nextPut: Character lf.
"thread local storage ids count up; a value is per thread"
out << 'tls ids increase ' << (SDL2 xTLSCreate < SDL2 xTLSCreate) printString; nextPut: Character lf.
(SDL2 xTLSCreate) in: [ :id |
  out << 'tls unset ' << (SDL2 xTLSGet: id) isNull printString.
  SDL2 xTLSSet: id value: (ExternalAddress fromAddress: 1234) dtor: nil.
  out << ' set ' << (SDL2 xTLSGet: id) asInteger printString; nextPut: Character lf ].
out << 'tls id 0 ' << (SDL2 xTLSSet: 0 value: nil dtor: nil) printString << ' ' << SDL2 xGetError printString; nextPut: Character lf.

mark value: 'timers'.
tmp := '/tmp/sdl2diff_timer_', UUID new asString36.
fp := SDL2 xFopen: tmp mode: 'wb'.
ids := SDL2 xAddTimer: 25 cb: fnFputc param: fp.
out << 'timer id ' << ids printString; nextPut: Character lf.
(Delay forMilliseconds: 400) wait.
out << 'remove ' << (SDL2 xRemoveTimer: ids) printString << ' again ' << (SDL2 xRemoveTimer: ids) printString << ' bogus ' << (SDL2 xRemoveTimer: 9999) printString; nextPut: Character lf.
(Delay forMilliseconds: 80) wait.
SDL2 xFclose: fp.
out << 'fired several times ' << (tmp asFileReference size between: 4 and: 20) printString; nextPut: Character lf.
tmp asFileReference ensureDelete.
out << 'second id ' << ((SDL2 xAddTimer: 1000000 cb: fnFputc param: nil) - ids) printString; nextPut: Character lf.
SDL2 xClearError.
out contents
