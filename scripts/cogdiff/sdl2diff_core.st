"sdl2diff_core.st -- SDL2 events (watch, filter, queue), threads, locks, timers, hints, RWops, log, error text.
Run on stock Cog (real libSDL2, SDL_VIDEODRIVER=dummy) and on our VM; the answers must match.
The ffiCall methods are compiled on the SDL2 class by this script.  Callbacks are FFICallback blocks: a watch
runs on the VM thread, a thread function and a timer callback arrive from a foreign thread."

| i out defs cb ev evs seen done sem th status wait pushEv poll names mark i32 m s c a spin mem rw buf tmp ra rb rr pts line tb |
out := WriteStream on: String new.
defs := OrderedCollection new.
defs
  add: 'xInit: f ^ self ffiCall: #( int SDL_Init ( uint32 f ) )';
  add: 'xAddWatch: cb ud: ud ^ self ffiCall: #( void SDL_AddEventWatch ( FFICallback cb, void* ud ) )';
  add: 'xDelWatch: cb ud: ud ^ self ffiCall: #( void SDL_DelEventWatch ( FFICallback cb, void* ud ) )';
  add: 'xSetFilter: cb ud: ud ^ self ffiCall: #( void SDL_SetEventFilter ( FFICallback cb, void* ud ) )';
  add: 'xSetFilterRaw: f ud: ud ^ self ffiCall: #( void SDL_SetEventFilter ( void* f, void* ud ) )';
  add: 'xGetFilter: f ud: ud ^ self ffiCall: #( int SDL_GetEventFilter ( void* f, void* ud ) )';
  add: 'xFilterEvents: cb ud: ud ^ self ffiCall: #( void SDL_FilterEvents ( FFICallback cb, void* ud ) )';
  add: 'xPush: ev ^ self ffiCall: #( int SDL_PushEvent ( void* ev ) )';
  add: 'xPoll: ev ^ self ffiCall: #( int SDL_PollEvent ( void* ev ) )';
  add: 'xPeep: ev n: n action: a min: lo max: hi ^ self ffiCall: #( int SDL_PeepEvents ( void* ev, int n, int a, uint32 lo, uint32 hi ) )';
  add: 'xHasEvent: t ^ self ffiCall: #( int SDL_HasEvent ( uint32 t ) )';
  add: 'xHasEvents: lo hi: hi ^ self ffiCall: #( int SDL_HasEvents ( uint32 lo, uint32 hi ) )';
  add: 'xFlushEvent: t ^ self ffiCall: #( void SDL_FlushEvent ( uint32 t ) )';
  add: 'xFlushEvents: lo hi: hi ^ self ffiCall: #( void SDL_FlushEvents ( uint32 lo, uint32 hi ) )';
  add: 'xRegister: n ^ self ffiCall: #( uint32 SDL_RegisterEvents ( int n ) )';
  add: 'xEventState: t state: s ^ self ffiCall: #( uint8 SDL_EventState ( uint32 t, int s ) )';
  add: 'xCreateThread: fn name: n data: d ^ self ffiCall: #( void* SDL_CreateThread ( FFICallback fn, String n, void* d ) )';
  add: 'xWaitThread: t status: s ^ self ffiCall: #( void SDL_WaitThread ( void* t, void* s ) )';
  add: 'xDetachThread: t ^ self ffiCall: #( void SDL_DetachThread ( void* t ) )';
  add: 'xThreadName: t ^ self ffiCall: #( String SDL_GetThreadName ( void* t ) )';
  add: 'xThreadID ^ self ffiCall: #( ulong SDL_ThreadID ( ) )';
  add: 'xGetThreadID: t ^ self ffiCall: #( ulong SDL_GetThreadID ( void* t ) )';
  add: 'xTLSCreate ^ self ffiCall: #( uint32 SDL_TLSCreate ( ) )';
  add: 'xTLSSet: id value: v dtor: d ^ self ffiCall: #( int SDL_TLSSet ( uint32 id, void* v, void* d ) )';
  add: 'xTLSGet: id ^ self ffiCall: #( void* SDL_TLSGet ( uint32 id ) )';
  add: 'xCreateMutex ^ self ffiCall: #( void* SDL_CreateMutex ( ) )';
  add: 'xLock: m ^ self ffiCall: #( int SDL_LockMutex ( void* m ) )';
  add: 'xTryLock: m ^ self ffiCall: #( int SDL_TryLockMutex ( void* m ) )';
  add: 'xUnlock: m ^ self ffiCall: #( int SDL_UnlockMutex ( void* m ) )';
  add: 'xDestroyMutex: m ^ self ffiCall: #( void SDL_DestroyMutex ( void* m ) )';
  add: 'xCreateSem: n ^ self ffiCall: #( void* SDL_CreateSemaphore ( uint32 n ) )';
  add: 'xSemWait: s ^ self ffiCall: #( int SDL_SemWait ( void* s ) )';
  add: 'xSemTryWait: s ^ self ffiCall: #( int SDL_SemTryWait ( void* s ) )';
  add: 'xSemWaitTimeout: s ms: ms ^ self ffiCall: #( int SDL_SemWaitTimeout ( void* s, uint32 ms ) )';
  add: 'xSemPost: s ^ self ffiCall: #( int SDL_SemPost ( void* s ) )';
  add: 'xSemValue: s ^ self ffiCall: #( uint32 SDL_SemValue ( void* s ) )';
  add: 'xCreateCond ^ self ffiCall: #( void* SDL_CreateCond ( ) )';
  add: 'xCondSignal: c ^ self ffiCall: #( int SDL_CondSignal ( void* c ) )';
  add: 'xCondWaitTimeout: c mutex: m ms: ms ^ self ffiCall: #( int SDL_CondWaitTimeout ( void* c, void* m, uint32 ms ) )';
  add: 'xAtomicAdd: a v: v ^ self ffiCall: #( int SDL_AtomicAdd ( void* a, int v ) )';
  add: 'xAtomicCAS: a old: o new: n ^ self ffiCall: #( int SDL_AtomicCAS ( void* a, int o, int n ) )';
  add: 'xAtomicSet: a v: v ^ self ffiCall: #( int SDL_AtomicSet ( void* a, int v ) )';
  add: 'xAtomicGet: a ^ self ffiCall: #( int SDL_AtomicGet ( void* a ) )';
  add: 'xTryLockSpin: a ^ self ffiCall: #( int SDL_AtomicTryLock ( void* a ) )';
  add: 'xUnlockSpin: a ^ self ffiCall: #( void SDL_AtomicUnlock ( void* a ) )';
  add: 'xAddTimer: ms cb: cb param: p ^ self ffiCall: #( int SDL_AddTimer ( uint32 ms, FFICallback cb, void* p ) )';
  add: 'xRemoveTimer: id ^ self ffiCall: #( int SDL_RemoveTimer ( int id ) )';
  add: 'xSetHint: n v: v ^ self ffiCall: #( int SDL_SetHint ( String n, String v ) )';
  add: 'xSetHintP: n v: v p: p ^ self ffiCall: #( int SDL_SetHintWithPriority ( String n, String v, int p ) )';
  add: 'xGetHint: n ^ self ffiCall: #( String SDL_GetHint ( String n ) )';
  add: 'xGetHintBool: n d: d ^ self ffiCall: #( int SDL_GetHintBoolean ( String n, int d ) )';
  add: 'xResetHint: n ^ self ffiCall: #( int SDL_ResetHint ( String n ) )';
  add: 'xAddHintCb: n cb: cb ud: ud ^ self ffiCall: #( void SDL_AddHintCallback ( String n, FFICallback cb, void* ud ) )';
  add: 'xDelHintCb: n cb: cb ud: ud ^ self ffiCall: #( void SDL_DelHintCallback ( String n, FFICallback cb, void* ud ) )';
  add: 'xClearHints ^ self ffiCall: #( void SDL_ClearHints ( ) )';
  add: 'xGetenv: n ^ self ffiCall: #( String SDL_getenv ( String n ) )';
  add: 'xSetenv: n v: v o: o ^ self ffiCall: #( int SDL_setenv ( String n, String v, int o ) )';
  add: 'xSetError: m ^ self ffiCall: #( int SDL_SetError ( String m ) )';
  add: 'xGetError ^ self ffiCall: #( String SDL_GetError ( ) )';
  add: 'xClearError ^ self ffiCall: #( void SDL_ClearError ( ) )';
  add: 'xError: c ^ self ffiCall: #( int SDL_Error ( int c ) )';
  add: 'xRWFromMem: m n: n ^ self ffiCall: #( void* SDL_RWFromMem ( void* m, int n ) )';
  add: 'xRWFromConstMem: m n: n ^ self ffiCall: #( void* SDL_RWFromConstMem ( void* m, int n ) )';
  add: 'xRWFromFile: f mode: m ^ self ffiCall: #( void* SDL_RWFromFile ( String f, String m ) )';
  add: 'xRWsize: r ^ self ffiCall: #( int64 SDL_RWsize ( void* r ) )';
  add: 'xRWseek: r o: o w: w ^ self ffiCall: #( int64 SDL_RWseek ( void* r, int64 o, int w ) )';
  add: 'xRWtell: r ^ self ffiCall: #( int64 SDL_RWtell ( void* r ) )';
  add: 'xRWread: r p: p s: s n: n ^ self ffiCall: #( size_t SDL_RWread ( void* r, void* p, size_t s, size_t n ) )';
  add: 'xRWwrite: r p: p s: s n: n ^ self ffiCall: #( size_t SDL_RWwrite ( void* r, void* p, size_t s, size_t n ) )';
  add: 'xRWclose: r ^ self ffiCall: #( int SDL_RWclose ( void* r ) )';
  add: 'xReadLE32: r ^ self ffiCall: #( uint32 SDL_ReadLE32 ( void* r ) )';
  add: 'xReadBE32: r ^ self ffiCall: #( uint32 SDL_ReadBE32 ( void* r ) )';
  add: 'xReadBE16: r ^ self ffiCall: #( uint16 SDL_ReadBE16 ( void* r ) )';
  add: 'xReadLE64: r ^ self ffiCall: #( uint64 SDL_ReadLE64 ( void* r ) )';
  add: 'xReadU8: r ^ self ffiCall: #( uint8 SDL_ReadU8 ( void* r ) )';
  add: 'xWriteBE32: r v: v ^ self ffiCall: #( size_t SDL_WriteBE32 ( void* r, uint32 v ) )';
  add: 'xWriteLE16: r v: v ^ self ffiCall: #( size_t SDL_WriteLE16 ( void* r, uint16 v ) )';
  add: 'xHasInter: a b: b ^ self ffiCall: #( int SDL_HasIntersection ( void* a, void* b ) )';
  add: 'xInter: a b: b r: r ^ self ffiCall: #( int SDL_IntersectRect ( void* a, void* b, void* r ) )';
  add: 'xUnion: a b: b r: r ^ self ffiCall: #( void SDL_UnionRect ( void* a, void* b, void* r ) )';
  add: 'xEnclose: p n: n clip: c r: r ^ self ffiCall: #( int SDL_EnclosePoints ( void* p, int n, void* c, void* r ) )';
  add: 'xClipLine: r x1: x1 y1: y1 x2: x2 y2: y2 ^ self ffiCall: #( int SDL_IntersectRectAndLine ( void* r, void* x1, void* y1, void* x2, void* y2 ) )';
  add: 'xInterF: a b: b r: r ^ self ffiCall: #( int SDL_IntersectFRect ( void* a, void* b, void* r ) )';
  add: 'xItoa: v buf: b radix: r ^ self ffiCall: #( void* SDL_itoa ( int v, void* b, int r ) )';
  add: 'xStrlcpy: d s: s n: n ^ self ffiCall: #( size_t SDL_strlcpy ( void* d, String s, size_t n ) )';
  add: 'xStrlcat: d s: s n: n ^ self ffiCall: #( size_t SDL_strlcat ( void* d, String s, size_t n ) )';
  add: 'xUtf8len: s ^ self ffiCall: #( size_t SDL_utf8strlen ( String s ) )';
  add: 'xCrc32: c d: d n: n ^ self ffiCall: #( uint32 SDL_crc32 ( uint32 c, void* d, size_t n ) )';
  add: 'xCrc16: c d: d n: n ^ self ffiCall: #( uint16 SDL_crc16 ( uint16 c, void* d, size_t n ) )';
  add: 'xStrtol: s end: e base: b ^ self ffiCall: #( long SDL_strtol ( String s, void* e, int b ) )';
  add: 'xPlatform ^ self ffiCall: #( String SDL_GetPlatform ( ) )';
  add: 'xCpuCountOk ^ self ffiCall: #( int SDL_GetCPUCount ( ) )';
  add: 'xLogSetPrio: c p: p ^ self ffiCall: #( void SDL_LogSetPriority ( int c, int p ) )';
  add: 'xLogGetPrio: c ^ self ffiCall: #( int SDL_LogGetPriority ( int c ) )';
  add: 'xLogSetAll: p ^ self ffiCall: #( void SDL_LogSetAllPriority ( int p ) )';
  add: 'xLogSetOutput: cb ud: ud ^ self ffiCall: #( void SDL_LogSetOutputFunction ( FFICallback cb, void* ud ) )';
  add: 'xLogMessage: c p: p m: m ^ self ffiCall: #( void SDL_LogMessage ( int c, int p, String m ) )';
  add: 'xLog: m ^ self ffiCall: #( void SDL_Log ( String m ) )'.
defs do: [ :d | SDL2 class compile: d classified: 'x' ].
i32 := [ :nums | | b | b := ByteArray new: nums size * 4. nums withIndexDo: [ :n :i | b signedLongAt: i - 1 * 4 + 1 put: n ]. b ].
mark := [ :label | out << '== ' << label; nextPut: Character lf ].
wait := [ :cond :ms | | t | t := 0. [ cond value or: [ t >= ms ] ] whileFalse: [ (Delay forMilliseconds: 5) wait. t := t + 5 ]. cond value ].
SDL2 xInit: 16r4001.

"---- events: push, watch, filter, queue ----"
mark value: 'events'.
seen := OrderedCollection new.
cb := FFICallback signature: #( int ( void* ud, void* ev ) ) block: [ :u :e | seen add: (e unsignedLongAt: 1) -> (e unsignedLongAt: 9). 0 ].
SDL2 xAddWatch: cb ud: nil.
ev := ByteArray new: 56. ev unsignedLongAt: 1 put: 32768. ev unsignedLongAt: 9 put: 77.
out << 'push ' << (SDL2 xPush: ev) printString << ' seen ' << seen printString; nextPut: Character lf.
out << 'has ' << (SDL2 xHasEvent: 32768) printString << ' ' << (SDL2 xHasEvents: 0 hi: 65535) printString << ' ' << (SDL2 xHasEvent: 32769) printString; nextPut: Character lf.
evs := ByteArray new: 56 * 3. 1 to: 3 do: [ :k | evs unsignedLongAt: (k - 1) * 56 + 1 put: 32768 + k. evs unsignedLongAt: (k - 1) * 56 + 9 put: k * 10 ].
out << 'peep add ' << (SDL2 xPeep: evs n: 3 action: 0 min: 0 max: 0) printString << ' seen ' << seen size printString; nextPut: Character lf.
out << 'peep peek ' << (SDL2 xPeep: nil n: 0 action: 1 min: 32768 max: 32770) printString << ' ' << (SDL2 xPeep: (ByteArray new: 56 * 2) n: 2 action: 1 min: 0 max: 65535) printString; nextPut: Character lf.
out << 'peep get with no buffer ' << (SDL2 xPeep: nil n: 0 action: 2 min: 32768 max: 32770) printString << ' still ' << (SDL2 xPeep: nil n: 0 action: 1 min: 32768 max: 32770) printString << ' get with buffer ' << (SDL2 xPeep: (ByteArray new: 56 * 2) n: 2 action: 2 min: 32768 max: 32770) printString << ' left ' << (SDL2 xPeep: nil n: 0 action: 1 min: 32768 max: 32770) printString; nextPut: Character lf.
SDL2 xPeep: evs n: 3 action: 0 min: 0 max: 0.
SDL2 xFlushEvent: 32770.
out << 'after flush ' << (SDL2 xHasEvent: 32770) printString << ' ' << (SDL2 xHasEvent: 32769) printString; nextPut: Character lf.
ev := ByteArray new: 56.
[ (SDL2 xPoll: ev) = 1 ] whileTrue: [ out << 'polled ' << (ev unsignedLongAt: 1) printString << '/' << (ev unsignedLongAt: 9) printString << ' ' ].
out nextPut: Character lf.
out << 'poll empty ' << (SDL2 xPoll: ev) printString << ' null-poll ' << (SDL2 xPoll: nil) printString; nextPut: Character lf.
seen := OrderedCollection new.
SDL2 xDelWatch: cb ud: nil.
ev := ByteArray new: 56. ev unsignedLongAt: 1 put: 32768.
SDL2 xPush: ev. SDL2 xFlushEvents: 0 hi: 65535.
out << 'after del watch seen ' << seen printString; nextPut: Character lf.
"watch added twice with two userdata: removal is by the pair"
seen := OrderedCollection new.
SDL2 xAddWatch: cb ud: (ExternalAddress allocate: 8).
SDL2 xAddWatch: cb ud: nil.
SDL2 xDelWatch: cb ud: nil.
ev := ByteArray new: 56. ev unsignedLongAt: 1 put: 32800. SDL2 xPush: ev.
out << 'pair removal calls ' << seen size printString; nextPut: Character lf.
SDL2 xFlushEvents: 0 hi: 65535.
"filter"
seen := OrderedCollection new.
SDL2 xSetFilter: (FFICallback signature: #( int ( void* ud, void* ev ) ) block: [ :u :e | | t | t := e unsignedLongAt: 1. seen add: t. t = 32801 ifTrue: [ 0 ] ifFalse: [ 1 ] ]) ud: nil.
out << 'getfilter ' << ((ByteArray new: 8) in: [ :f | SDL2 xGetFilter: f ud: (ByteArray new: 8) ]) printString; nextPut: Character lf.
1 to: 3 do: [ :k | ev := ByteArray new: 56. ev unsignedLongAt: 1 put: 32800 + k. out << 'push' << k printString << '=' << (SDL2 xPush: ev) printString << ' ' ].
out nextPut: Character lf.
out << 'filter saw ' << seen printString << ' queued ' << (SDL2 xHasEvents: 32800 hi: 32810) printString << ' has32801 ' << (SDL2 xHasEvent: 32801) printString << ' has32802 ' << (SDL2 xHasEvent: 32802) printString; nextPut: Character lf.
SDL2 xFilterEvents: (FFICallback signature: #( int ( void* ud, void* ev ) ) block: [ :u :e | (e unsignedLongAt: 1) = 32802 ifTrue: [ 0 ] ifFalse: [ 1 ] ]) ud: nil.
out << 'after FilterEvents has32802 ' << (SDL2 xHasEvent: 32802) printString << ' has32803 ' << (SDL2 xHasEvent: 32803) printString; nextPut: Character lf.
SDL2 xSetFilterRaw: nil ud: nil.
out << 'set filter flushes: has32803 ' << (SDL2 xHasEvent: 32803) printString; nextPut: Character lf.
"register events and event state"
out << 'register ' << (SDL2 xRegister: 0) printString << ' ' << (SDL2 xRegister: 3) printString << ' ' << (SDL2 xRegister: 1) printString << ' ' << (SDL2 xRegister: 70000) printString << ' ' << (SDL2 xRegister: -1) printString; nextPut: Character lf.
out << 'eventstate ' << (SDL2 xEventState: 32805 state: -1) printString << ' ' << (SDL2 xEventState: 32805 state: 0) printString << ' ' << (SDL2 xEventState: 32805 state: -1) printString << ' ' << (SDL2 xEventState: 32805 state: 1) printString << ' ' << (SDL2 xEventState: 32805 state: -1) printString; nextPut: Character lf.
out << 'eventstate text ' << (SDL2 xEventState: 16r303 state: -1) printString; nextPut: Character lf.
ev := ByteArray new: 56. ev unsignedLongAt: 1 put: 32806. SDL2 xPush: ev. SDL2 xEventState: 32806 state: 0.
out << 'disable flushes ' << (SDL2 xHasEvent: 32806) printString; nextPut: Character lf.
SDL2 xEventState: 32806 state: 1.

"---- mutex, semaphore, condition, atomics ----"
mark value: 'sync'.
m := SDL2 xCreateMutex.
out << 'mutex ' << (SDL2 xLock: m) printString << (SDL2 xLock: m) printString << (SDL2 xTryLock: m) printString << (SDL2 xUnlock: m) printString << (SDL2 xUnlock: m) printString << (SDL2 xUnlock: m) printString << ' err ' << SDL2 xGetError printString; nextPut: Character lf.
SDL2 xClearError.
out << 'unlock not owned ' << (SDL2 xUnlock: m) printString << ' ' << SDL2 xGetError printString; nextPut: Character lf.
s := SDL2 xCreateSem: 2.
out << 'sem ' << (SDL2 xSemValue: s) printString << (SDL2 xSemTryWait: s) printString << (SDL2 xSemTryWait: s) printString << (SDL2 xSemTryWait: s) printString << ' ' << (SDL2 xSemWaitTimeout: s ms: 20) printString << ' ' << (SDL2 xSemPost: s) printString << (SDL2 xSemWait: s) printString << ' ' << (SDL2 xSemValue: s) printString; nextPut: Character lf.
c := SDL2 xCreateCond.
SDL2 xLock: m.
out << 'cond timeout ' << (SDL2 xCondWaitTimeout: c mutex: m ms: 20) printString << ' signal ' << (SDL2 xCondSignal: c) printString; nextPut: Character lf.
SDL2 xUnlock: m.
a := ByteArray new: 4. a signedLongAt: 1 put: 5.
out << 'atomic ' << (SDL2 xAtomicAdd: a v: 3) printString << ' ' << (SDL2 xAtomicGet: a) printString << ' ' << (SDL2 xAtomicSet: a v: 10) printString << ' ' << (SDL2 xAtomicCAS: a old: 10 new: 11) printString << ' ' << (SDL2 xAtomicCAS: a old: 10 new: 12) printString << ' ' << (SDL2 xAtomicGet: a) printString; nextPut: Character lf.
spin := ByteArray new: 4.
out << 'spin ' << (SDL2 xTryLockSpin: spin) printString << ' ' << (SDL2 xTryLockSpin: spin) printString; nextPut: Character lf.
SDL2 xUnlockSpin: spin.
out << 'spin after unlock ' << (SDL2 xTryLockSpin: spin) printString; nextPut: Character lf.

"---- hints ----"
mark value: 'hints'.
out << 'unset ' << (SDL2 xGetHint: 'PHARO_TEST_UNSET') printString << ' bool default ' << (SDL2 xGetHintBool: 'PHARO_TEST_UNSET' d: 1) printString << (SDL2 xGetHintBool: 'PHARO_TEST_UNSET' d: 0) printString; nextPut: Character lf.
out << 'set ' << (SDL2 xSetHint: 'PHARO_TEST_A' v: 'one') printString << ' get ' << (SDL2 xGetHint: 'PHARO_TEST_A') printString; nextPut: Character lf.
out << 'prio low blocked ' << (SDL2 xSetHintP: 'PHARO_TEST_A' v: 'two' p: 0) printString << ' get ' << (SDL2 xGetHint: 'PHARO_TEST_A') printString; nextPut: Character lf.
out << 'prio override ' << (SDL2 xSetHintP: 'PHARO_TEST_A' v: 'three' p: 2) printString << ' get ' << (SDL2 xGetHint: 'PHARO_TEST_A') printString; nextPut: Character lf.
out << 'normal after override ' << (SDL2 xSetHint: 'PHARO_TEST_A' v: 'four') printString << ' get ' << (SDL2 xGetHint: 'PHARO_TEST_A') printString; nextPut: Character lf.
out << 'reset ' << (SDL2 xResetHint: 'PHARO_TEST_A') printString << ' get ' << (SDL2 xGetHint: 'PHARO_TEST_A') printString << ' reset unknown ' << (SDL2 xResetHint: 'PHARO_TEST_NEVER') printString; nextPut: Character lf.
out << 'bool ' << (SDL2 xSetHint: 'PHARO_TEST_B' v: '0') printString << (SDL2 xGetHintBool: 'PHARO_TEST_B' d: 1) printString << (SDL2 xSetHint: 'PHARO_TEST_B' v: 'false') printString << (SDL2 xGetHintBool: 'PHARO_TEST_B' d: 1) printString << (SDL2 xSetHint: 'PHARO_TEST_B' v: 'yes') printString << (SDL2 xGetHintBool: 'PHARO_TEST_B' d: 0) printString << (SDL2 xSetHint: 'PHARO_TEST_B' v: '') printString << (SDL2 xGetHintBool: 'PHARO_TEST_B' d: 1) printString; nextPut: Character lf.
"environment override: PHARO_TEST_ENV is set by the runner"
out << 'env ' << (SDL2 xGetenv: 'PHARO_TEST_ENV') printString << ' hint ' << (SDL2 xGetHint: 'PHARO_TEST_ENV') printString << ' set blocked ' << (SDL2 xSetHint: 'PHARO_TEST_ENV' v: 'fromhint') printString << ' get ' << (SDL2 xGetHint: 'PHARO_TEST_ENV') printString << ' override ' << (SDL2 xSetHintP: 'PHARO_TEST_ENV' v: 'forced' p: 2) printString << ' get ' << (SDL2 xGetHint: 'PHARO_TEST_ENV') printString; nextPut: Character lf.
out << 'setenv ' << (SDL2 xSetenv: 'PHARO_TEST_NEW' v: 'v1' o: 1) printString << (SDL2 xGetenv: 'PHARO_TEST_NEW') printString << (SDL2 xSetenv: 'PHARO_TEST_NEW' v: 'v2' o: 0) printString << (SDL2 xGetenv: 'PHARO_TEST_NEW') printString << (SDL2 xSetenv: 'PHARO_TEST_NEW' v: 'v3' o: 1) printString << (SDL2 xGetenv: 'PHARO_TEST_NEW') printString << ' invalid ' << (SDL2 xSetenv: 'A=B' v: 'x' o: 1) printString << (SDL2 xSetenv: '' v: 'x' o: 1) printString; nextPut: Character lf.
out << 'hint sees setenv ' << (SDL2 xGetHint: 'PHARO_TEST_NEW') printString; nextPut: Character lf.
seen := OrderedCollection new.
cb := FFICallback signature: #( void ( void* ud, String name, String old, String new ) ) block: [ :u :n :o :nw | seen add: { n. o. nw } ].
SDL2 xAddHintCb: 'PHARO_TEST_CB' cb: cb ud: nil.
SDL2 xSetHint: 'PHARO_TEST_CB' v: 'x1'. SDL2 xSetHint: 'PHARO_TEST_CB' v: 'x1'. SDL2 xSetHint: 'PHARO_TEST_CB' v: 'x2'.
SDL2 xResetHint: 'PHARO_TEST_CB'.
SDL2 xDelHintCb: 'PHARO_TEST_CB' cb: cb ud: nil.
SDL2 xSetHint: 'PHARO_TEST_CB' v: 'x3'.
out << 'callbacks ' << seen printString; nextPut: Character lf.

"---- error text ----"
mark value: 'error'.
SDL2 xClearError.
out << 'cleared ' << SDL2 xGetError printString; nextPut: Character lf.
out << 'seterror ' << (SDL2 xSetError: 'boom') printString << ' ' << SDL2 xGetError printString; nextPut: Character lf.
#( 0 1 2 3 4 5 99 ) do: [ :code | out << 'error(' << code printString << ')=' << (SDL2 xError: code) printString << ' ' << SDL2 xGetError printString; nextPut: Character lf ].

"---- RWops ----"
mark value: 'rwops'.
mem := ByteArray withAll: #( 1 2 3 4 5 6 7 8 9 10 ).
rw := SDL2 xRWFromMem: mem n: 10.
out << 'size ' << (SDL2 xRWsize: rw) printString << ' tell ' << (SDL2 xRWtell: rw) printString << ' read ' << (SDL2 xRWread: rw p: (buf := ByteArray new: 8) s: 2 n: 3) printString << ' ' << buf asArray printString << ' tell ' << (SDL2 xRWtell: rw) printString; nextPut: Character lf.
out << 'seek ' << (SDL2 xRWseek: rw o: -2 w: 2) printString << ' ' << (SDL2 xRWseek: rw o: 100 w: 0) printString << ' ' << (SDL2 xRWseek: rw o: -100 w: 1) printString << ' bad whence ' << (SDL2 xRWseek: rw o: 0 w: 9) printString << ' ' << SDL2 xGetError printString; nextPut: Character lf.
SDL2 xRWseek: rw o: 0 w: 0.
out << 'le32 ' << (SDL2 xReadLE32: rw) printString << ' be32 ' << (SDL2 xReadBE32: rw) printString << ' be16 ' << (SDL2 xReadBE16: rw) printString << ' u8 ' << (SDL2 xReadU8: rw) printString << ' le64 past end ' << (SDL2 xReadLE64: rw) printString; nextPut: Character lf.
SDL2 xRWseek: rw o: 0 w: 0.
out << 'write ' << (SDL2 xWriteBE32: rw v: 16r01020304) printString << (SDL2 xWriteLE16: rw v: 16rABCD) printString << ' mem ' << mem asArray printString; nextPut: Character lf.
SDL2 xRWseek: rw o: 8 w: 0.
out << 'write short ' << (SDL2 xWriteBE32: rw v: 16rFFFFFFFF) printString << ' mem ' << mem asArray printString; nextPut: Character lf.
out << 'close ' << (SDL2 xRWclose: rw) printString; nextPut: Character lf.
rw := SDL2 xRWFromConstMem: mem n: 10.
out << 'const write ' << (SDL2 xRWwrite: rw p: (ByteArray new: 4) s: 1 n: 4) printString << ' ' << SDL2 xGetError printString; nextPut: Character lf.
SDL2 xRWclose: rw.
out << 'from mem null ' << ((SDL2 xRWFromMem: nil n: 4) isNull printString) << ' ' << SDL2 xGetError printString << ' size0 ' << ((SDL2 xRWFromMem: mem n: 0) isNull printString) << ' ' << SDL2 xGetError printString; nextPut: Character lf.
out << 'file missing ' << ((SDL2 xRWFromFile: '/nonexistent/x' mode: 'rb') isNull printString) << ' ' << SDL2 xGetError printString << ' nomode ' << ((SDL2 xRWFromFile: 'x' mode: '') isNull printString) << ' ' << SDL2 xGetError printString; nextPut: Character lf.
tmp := '/tmp/sdl2diff_rw_', UUID new asString36.
rw := SDL2 xRWFromFile: tmp mode: 'wb'.
out << 'file write ' << (SDL2 xRWwrite: rw p: (ByteArray withAll: #( 9 8 7 6 5 )) s: 1 n: 5) printString << ' tell ' << (SDL2 xRWtell: rw) printString << ' close ' << (SDL2 xRWclose: rw) printString; nextPut: Character lf.
rw := SDL2 xRWFromFile: tmp mode: 'rb'.
out << 'file size ' << (SDL2 xRWsize: rw) printString << ' read ' << (SDL2 xRWread: rw p: (buf := ByteArray new: 8) s: 1 n: 8) printString << ' ' << buf asArray printString << ' tell ' << (SDL2 xRWtell: rw) printString; nextPut: Character lf.
SDL2 xRWclose: rw.
tmp asFileReference ensureDelete.

"---- rectangles ----"
mark value: 'rects'.
ra := i32 value: #(0 0 10 10). rb := i32 value: #(5 5 10 10). rr := ByteArray new: 16.
out << 'inter ' << (SDL2 xInter: ra b: rb r: rr) printString << ' ' << ((1 to: 4) collect: [ :k | rr signedLongAt: k - 1 * 4 + 1 ]) printString << ' has ' << (SDL2 xHasInter: ra b: rb) printString << ' disjoint ' << (SDL2 xHasInter: ra b: (i32 value: #(20 20 5 5))) printString << ' empty ' << (SDL2 xInter: ra b: (i32 value: #(1 1 0 5)) r: rr) printString; nextPut: Character lf.
SDL2 xUnion: ra b: rb r: rr.
out << 'union ' << ((1 to: 4) collect: [ :k | rr signedLongAt: k - 1 * 4 + 1 ]) printString; nextPut: Character lf.
pts := i32 value: #(3 4 -2 9 7 1 20 20).
out << 'enclose ' << (SDL2 xEnclose: pts n: 4 clip: nil r: rr) printString << ' ' << ((1 to: 4) collect: [ :k | rr signedLongAt: k - 1 * 4 + 1 ]) printString << ' clipped ' << (SDL2 xEnclose: pts n: 4 clip: (i32 value: #(0 0 10 10)) r: rr) printString << ' ' << ((1 to: 4) collect: [ :k | rr signedLongAt: k - 1 * 4 + 1 ]) printString << ' none ' << (SDL2 xEnclose: pts n: 4 clip: (i32 value: #(50 50 5 5)) r: rr) printString; nextPut: Character lf.
#( #(-5 3 20 12) #(3 -4 10 30) #(0 0 5 5) #(-9 -9 -1 -1) #(10 2 -6 2) ) do: [ :l | | x1 y1 x2 y2 |
  x1 := i32 value: { l at: 1 }. y1 := i32 value: { l at: 2 }. x2 := i32 value: { l at: 3 }. y2 := i32 value: { l at: 4 }.
  out << 'clipline ' << l printString << ' -> ' << (SDL2 xClipLine: (i32 value: #(0 0 16 16)) x1: x1 y1: y1 x2: x2 y2: y2) printString << ' ' << { x1 signedLongAt: 1. y1 signedLongAt: 1. x2 signedLongAt: 1. y2 signedLongAt: 1 } printString; nextPut: Character lf ].
(ByteArray new: 16) in: [ :fr | out << 'finter ' << (SDL2 xInterF: ((ByteArray new: 16) in: [ :b | b float32AtOffset: 0 put: 0.0; float32AtOffset: 4 put: 0.0; float32AtOffset: 8 put: 4.5; float32AtOffset: 12 put: 4.5. b ]) b: ((ByteArray new: 16) in: [ :b | b float32AtOffset: 0 put: 2.5; float32AtOffset: 4 put: 1.0; float32AtOffset: 8 put: 4.0; float32AtOffset: 12 put: 1.0. b ]) r: fr) printString << ' ' << ((0 to: 3) collect: [ :k | fr float32AtOffset: k * 4 ]) printString; nextPut: Character lf ].

"---- text helpers ----"
mark value: 'text'.
tb := ByteArray new: 40.
#( #(255 10) #(255 16) #(-255 10) #(-255 16) #(0 2) #(1295 36) #(100 8) ) do: [ :p | SDL2 xItoa: p first buf: tb radix: p last. out << 'itoa ' << p printString << ' ' << (tb copyUpTo: 0) asString printString; nextPut: Character lf ].
tb := ByteArray new: 8. tb at: 1 put: 65.
out << 'strlcpy ' << (SDL2 xStrlcpy: tb s: 'hello world' n: 6) printString << ' ' << (tb copyUpTo: 0) asString printString << ' strlcat ' << (SDL2 xStrlcat: tb s: 'xyzzy' n: 8) printString << ' ' << (tb copyUpTo: 0) asString printString; nextPut: Character lf.
out << 'utf8len ' << (SDL2 xUtf8len: 'abc') printString << ' ' << (SDL2 xUtf8len: (String with: (Character value: 16rE4) with: (Character value: 16rF6))) printString; nextPut: Character lf.
tb := ByteArray withAll: 'hello' asByteArray.
out << 'crc32 ' << (SDL2 xCrc32: 0 d: tb n: 5) printString << ' ' << (SDL2 xCrc32: 16rFFFFFFFF d: tb n: 5) printString << ' crc16 ' << (SDL2 xCrc16: 0 d: tb n: 5) printString << ' ' << (SDL2 xCrc16: 16rFFFF d: tb n: 5) printString; nextPut: Character lf.
out << 'strtol ' << (SDL2 xStrtol: '  -42abc' end: nil base: 10) printString << ' ' << (SDL2 xStrtol: 'ff' end: nil base: 16) printString; nextPut: Character lf.
out << 'platform ' << SDL2 xPlatform printString << ' cpu>0 ' << (SDL2 xCpuCountOk > 0) printString; nextPut: Character lf.

"---- log ----"
mark value: 'log'.
seen := OrderedCollection new.
SDL2 xLogSetOutput: (FFICallback signature: #( void ( void* ud, int cat, int prio, String msg ) ) block: [ :u :c :p :m | seen add: { c. p. m } ]) ud: nil.
SDL2 xLog: 'plain message'.
SDL2 xLogMessage: 0 p: 5 m: 'error at app'.
SDL2 xLogMessage: 1 p: 3 m: 'info in error category'.
SDL2 xLogMessage: 8 p: 1 m: 'verbose test category'.
SDL2 xLogMessage: 30 p: 4 m: 'warn custom'.
SDL2 xLogMessage: 30 p: 6 m: 'critical custom'.
SDL2 xLogMessage: 0 p: 3 m: 'with newline
'.
out << 'prios ' << ((#(0 1 2 3 8 19 30) collect: [ :cat | SDL2 xLogGetPrio: cat ]) printString); nextPut: Character lf.
SDL2 xLogSetPrio: 30 p: 2.
SDL2 xLogMessage: 30 p: 1 m: 'dropped verbose'. SDL2 xLogMessage: 30 p: 2 m: 'debug kept'.
SDL2 xLogSetAll: 5.
SDL2 xLogMessage: 0 p: 3 m: 'dropped info'. SDL2 xLogMessage: 0 p: 5 m: 'error kept'.
out << 'log ' << seen printString; nextPut: Character lf.
out contents
