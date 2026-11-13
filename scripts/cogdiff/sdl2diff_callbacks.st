"sdl2diff_callbacks.st -- Smalltalk callbacks that arrive on a thread that is not the VM thread:
a thread made by SDL_CreateThread, the SDL timer thread, and an event pushed from such a thread.
The VM queues each call for the VM thread and parks the calling thread until the image has answered
(the cross-thread callback queue of the VM).  Stock Cog aborts on these cases in a headless run
('Code zone writing is not reentrant'), so this script runs on our VM only; the same calls with native
function pointers are compared with Cog in sdl2diff_threads.st.
Expected output: scripts/cogdiff/sdl2diff_callbacks.expected"

| out defs cb ev seen done sem th status wait names mark i |
out := WriteStream on: String new.
defs := OrderedCollection new.
defs
  add: 'xInit: f ^ self ffiCall: #( int SDL_Init ( uint32 f ) )';
  add: 'xAddWatch: cb ud: ud ^ self ffiCall: #( void SDL_AddEventWatch ( FFICallback cb, void* ud ) )';
  add: 'xPush: ev ^ self ffiCall: #( int SDL_PushEvent ( void* ev ) )';
  add: 'xPoll: ev ^ self ffiCall: #( int SDL_PollEvent ( void* ev ) )';
  add: 'xFlushEvents: lo hi: hi ^ self ffiCall: #( void SDL_FlushEvents ( uint32 lo, uint32 hi ) )';
  add: 'xCreateThread: fn name: n data: d ^ self ffiCall: #( void* SDL_CreateThread ( FFICallback fn, String n, void* d ) )';
  add: 'xWaitThread: t status: s ^ self ffiCall: #( void SDL_WaitThread ( void* t, void* s ) )';
  add: 'xDetachThread: t ^ self ffiCall: #( void SDL_DetachThread ( void* t ) )';
  add: 'xThreadName: t ^ self ffiCall: #( String SDL_GetThreadName ( void* t ) )';
  add: 'xThreadID ^ self ffiCall: #( ulong SDL_ThreadID ( ) )';
  add: 'xGetThreadID: t ^ self ffiCall: #( ulong SDL_GetThreadID ( void* t ) )';
  add: 'xCreateSem: n ^ self ffiCall: #( void* SDL_CreateSemaphore ( uint32 n ) )';
  add: 'xSemPost: s ^ self ffiCall: #( int SDL_SemPost ( void* s ) )';
  add: 'xSemValue: s ^ self ffiCall: #( uint32 SDL_SemValue ( void* s ) )';
  add: 'xAddTimer: ms cb: cb param: p ^ self ffiCall: #( int SDL_AddTimer ( uint32 ms, FFICallback cb, void* p ) )';
  add: 'xRemoveTimer: id ^ self ffiCall: #( int SDL_RemoveTimer ( int id ) )'.
defs do: [ :d | SDL2 class compile: d classified: 'x' ].
mark := [ :label | out << '== ' << label; nextPut: Character lf ].
wait := [ :cond :ms | | t | t := 0. [ cond value or: [ t >= ms ] ] whileFalse: [ (Delay forMilliseconds: 5) wait. t := t + 5 ]. cond value ].
SDL2 xInit: 16r4001.

"---- threads: a native thread that runs a Smalltalk callback ----"
mark value: 'threads'.
done := false. status := ByteArray new: 4. names := OrderedCollection new.
sem := SDL2 xCreateSem: 0.
th := SDL2 xCreateThread: (FFICallback signature: #( int ( void* d ) ) block: [ :d | names add: 'in-thread'. done := true. SDL2 xSemPost: sem. 42 ]) name: 'worker-1' data: nil.
out << 'thread created ' << th isNull not printString << ' name ' << (SDL2 xThreadName: th) printString; nextPut: Character lf.
out << 'thread done ' << (wait value: [ done ] value: 4000) printString << ' ' << names printString; nextPut: Character lf.
out << 'sem value ' << (SDL2 xSemValue: sem) printString; nextPut: Character lf.
out << 'id differs ' << ((SDL2 xGetThreadID: th) ~= SDL2 xThreadID) printString << ' null-id is own ' << ((SDL2 xGetThreadID: nil) = SDL2 xThreadID) printString; nextPut: Character lf.
SDL2 xWaitThread: th status: status.
out << 'status ' << (status signedLongAt: 1) printString; nextPut: Character lf.
done := false.
th := SDL2 xCreateThread: (FFICallback signature: #( int ( void* d ) ) block: [ :d | done := true. 7 ]) name: nil data: nil.
out << 'unnamed ' << (SDL2 xThreadName: th) printString; nextPut: Character lf.
wait value: [ done ] value: 4000.
SDL2 xDetachThread: th.
out << 'detached ran ' << done printString; nextPut: Character lf.
mark value: 'timers'.
done := 0.
i := SDL2 xAddTimer: 30 cb: (FFICallback signature: #( uint32 ( uint32 interval, void* p ) ) block: [ :iv :p | done := done + 1. iv ]) param: nil.
out << 'timer id ' << i printString; nextPut: Character lf.
wait value: [ done >= 3 ] value: 4000.
out << 'fired at least 3 ' << (done >= 3) printString; nextPut: Character lf.
out << 'remove ' << (SDL2 xRemoveTimer: i) printString << ' again ' << (SDL2 xRemoveTimer: i) printString << ' bogus ' << (SDL2 xRemoveTimer: 9999) printString; nextPut: Character lf.
done := 0.
i := SDL2 xAddTimer: 20 cb: (FFICallback signature: #( uint32 ( uint32 interval, void* p ) ) block: [ :iv :p | done := done + 1. 0 ]) param: nil.
wait value: [ done >= 1 ] value: 4000.
(Delay forMilliseconds: 120) wait.
out << 'one shot fired ' << done printString << ' remove after finish ' << (SDL2 xRemoveTimer: i) printString; nextPut: Character lf.


mark value: 'event pushed from a thread'.
seen := OrderedCollection new. done := false.
cb := FFICallback signature: #( int ( void* ud, void* ev ) ) block: [ :u :e | seen add: (e unsignedLongAt: 1). 0 ].
SDL2 xAddWatch: cb ud: nil.
th := SDL2 xCreateThread: (FFICallback signature: #( int ( void* d ) ) block: [ :d | | e | e := ByteArray new: 56. e unsignedLongAt: 1 put: 32900. out << 'push from thread ' << (SDL2 xPush: e) printString; nextPut: Character lf. done := true. 0 ]) name: 'pusher' data: nil.
wait value: [ done ] value: 4000.
SDL2 xWaitThread: th status: nil.
out << 'watch ran ' << seen printString; nextPut: Character lf.
ev := ByteArray new: 56.
out << 'polled ' << (SDL2 xPoll: ev) printString << ' ' << (ev unsignedLongAt: 1) printString; nextPut: Character lf.
out contents
