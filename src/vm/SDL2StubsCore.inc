// SDL2StubsCore.inc - stubs of the SDL2 layer that need no window state:
// events, threads, locks, timers, hints, log, assertions, error text, RWops,
// atomics, platform queries, text helpers.
//
// Included by FFI.cpp inside namespace pharo::ffi, after the window and
// renderer stubs.  Each stub is a plain function with the C ABI of the SDL
// function, so that libffi can call it with the signature of the image.  The
// header files SDL2Sys.hpp, SDL2Base.hpp, SDL2RW.hpp and SDL2Events.hpp hold
// the logic; this file adapts it to the argument types of the stubs.

// ----- Memory (SDL_stdinc.h) -----
//
// SDL_malloc, SDL_calloc, SDL_realloc and SDL_free go through one table.  It
// holds the C library functions until SDL_SetMemoryFunctions replaces them.
// A block that SDL hands to the image (a string, a path, clipboard text) comes
// from the same table, so SDL_free frees it with the matching function.

struct SDLMemoryTable {
    void* (*malloc_)(size_t) = ::malloc;
    void* (*calloc_)(size_t, size_t) = ::calloc;
    void* (*realloc_)(void*, size_t) = ::realloc;
    void (*free_)(void*) = ::free;
};

static SDLMemoryTable& sdlMemory() {
    static SDLMemoryTable* table = new SDLMemoryTable;
    return *table;
}

static char* sdlAllocString(const char* s) {
    if (!s) return nullptr;
    const size_t n = strlen(s) + 1;
    char* out = static_cast<char*>(sdlMemory().malloc_(n));
    if (out) memcpy(out, s, n);
    return out;
}

static int stub_SDL_SetMemoryFunctions(void* m, void* c, void* r, void* f) {
    if (!m) return sdl2::sys::invalidParam("malloc_func");
    if (!c) return sdl2::sys::invalidParam("calloc_func");
    if (!r) return sdl2::sys::invalidParam("realloc_func");
    if (!f) return sdl2::sys::invalidParam("free_func");
    SDLMemoryTable& t = sdlMemory();
    t.malloc_ = reinterpret_cast<void* (*)(size_t)>(m);
    t.calloc_ = reinterpret_cast<void* (*)(size_t, size_t)>(c);
    t.realloc_ = reinterpret_cast<void* (*)(void*, size_t)>(r);
    t.free_ = reinterpret_cast<void (*)(void*)>(f);
    return 0;
}

static void stub_SDL_GetMemoryFunctions(void** m, void** c, void** r, void** f) {
    const SDLMemoryTable& t = sdlMemory();
    if (m) *m = reinterpret_cast<void*>(t.malloc_);
    if (c) *c = reinterpret_cast<void*>(t.calloc_);
    if (r) *r = reinterpret_cast<void*>(t.realloc_);
    if (f) *f = reinterpret_cast<void*>(t.free_);
}

static void stub_SDL_GetOriginalMemoryFunctions(void** m, void** c, void** r, void** f) {
    if (m) *m = reinterpret_cast<void*>(static_cast<void* (*)(size_t)>(::malloc));
    if (c) *c = reinterpret_cast<void*>(static_cast<void* (*)(size_t, size_t)>(::calloc));
    if (r) *r = reinterpret_cast<void*>(static_cast<void* (*)(void*, size_t)>(::realloc));
    if (f) *f = reinterpret_cast<void*>(static_cast<void (*)(void*)>(::free));
}

// SDL counts allocations only in a build that asks for it; the answer of every
// other build is -1.
static int stub_SDL_GetNumAllocations() { return -1; }

extern "C" {
void stub_SDL_free(void* mem) { sdlMemory().free_(mem); }
}

// ----- Error text (SDL_error.h) -----

static int stub_SDL_SetError(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int r = sdl2::sys::setErrorV(fmt, ap);
    va_end(ap);
    return r;
}

static void stub_SDL_ClearError() { sdl2::sys::lastError().clear(); }

static int stub_SDL_Error(int code) { return sdl2::rw::errorCode(code); }

static char* stub_SDL_GetErrorMsg(char* buf, int len) {
    if (!buf || len <= 0) return buf;
    const std::string& e = sdl2::sys::lastError();
    strncpy(buf, e.c_str(), static_cast<size_t>(len) - 1);
    buf[len - 1] = '\0';
    return buf;
}

// ----- Events (SDL_events.h) -----

static void stub_SDL_PumpEvents() { pumpHostEvents(); }

static int stub_SDL_PeepEvents(void* events, int num, int action, uint32_t minType, uint32_t maxType) {
    return sdl2::ev::peepEvents(static_cast<sdl2::ev::Event*>(events), num, action, minType, maxType);
}

static int stub_SDL_HasEvents(uint32_t minType, uint32_t maxType) { return sdl2::ev::hasEvents(minType, maxType); }
static int stub_SDL_HasEvent(uint32_t type) { return sdl2::ev::hasEvents(type, type); }
static void stub_SDL_FlushEvents(uint32_t minType, uint32_t maxType) { sdl2::ev::flushEvents(minType, maxType); }
static void stub_SDL_FlushEvent(uint32_t type) { sdl2::ev::flushEvents(type, type); }

static void stub_SDL_SetEventFilter(void* filter, void* userdata) {
    sdl2::ev::setEventFilter(reinterpret_cast<sdl2::ev::EventFilter>(filter), userdata);
}

static int stub_SDL_GetEventFilter(void** filter, void** userdata) {
    sdl2::ev::EventFilter f = nullptr;
    void* u = nullptr;
    const int has = sdl2::ev::getEventFilter(&f, &u);
    if (filter) *filter = reinterpret_cast<void*>(f);
    if (userdata) *userdata = u;
    return has;
}

static void stub_SDL_FilterEvents(void* filter, void* userdata) {
    if (filter) sdl2::ev::filterEvents(reinterpret_cast<sdl2::ev::EventFilter>(filter), userdata);
}

static void stub_SDL_AddEventWatch(void* filter, void* userdata) {
    sdl2::ev::addEventWatch(reinterpret_cast<sdl2::ev::EventFilter>(filter), userdata);
}

static void stub_SDL_DelEventWatch(void* filter, void* userdata) {
    sdl2::ev::delEventWatch(reinterpret_cast<sdl2::ev::EventFilter>(filter), userdata);
}

static uint32_t stub_SDL_RegisterEvents(int count) { return sdl2::ev::registerEvents(count); }

// SDL_WaitEventTimeout: a negative timeout waits for ever, zero polls.  The
// wait runs on the calling (VM) thread in slices of one millisecond and pumps
// the host events each time.  Answers 1 when an event came, 0 on timeout.
static int stub_SDL_WaitEventTimeout(void* event, int timeoutMs) {
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        if (stub_SDL_PollEvent(event)) return 1;
        if (timeoutMs == 0) return 0;
        if (timeoutMs > 0 &&
            std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(timeoutMs)) return 0;
        usleep(1000);
    }
}

// ----- Threads (SDL_thread.h) -----

static void* stub_SDL_CreateThread(void* fn, const char* name, void* data) {
    return sdl2::sys::createThread(reinterpret_cast<sdl2::sys::ThreadFunction>(fn), name, data);
}

static void* stub_SDL_CreateThreadWithStackSize(void* fn, const char* name, size_t stackSize, void* data) {
    return sdl2::sys::createThreadWithStackSize(reinterpret_cast<sdl2::sys::ThreadFunction>(fn), name, stackSize, data);
}

static const char* stub_SDL_GetThreadName(void* t) { return sdl2::sys::threadName(static_cast<sdl2::sys::Thread*>(t)); }
static unsigned long stub_SDL_ThreadID() { return sdl2::sys::currentThreadId(); }
static unsigned long stub_SDL_GetThreadID(void* t) { return sdl2::sys::threadId(static_cast<sdl2::sys::Thread*>(t)); }
static int stub_SDL_SetThreadPriority(int p) { return sdl2::sys::setThreadPriority(p); }
static void stub_SDL_WaitThread(void* t, int* status) { sdl2::sys::waitThread(static_cast<sdl2::sys::Thread*>(t), status); }
static void stub_SDL_DetachThread(void* t) { sdl2::sys::detachThread(static_cast<sdl2::sys::Thread*>(t)); }
static unsigned stub_SDL_TLSCreate() { return sdl2::sys::tlsCreate(); }
static void* stub_SDL_TLSGet(unsigned id) { return sdl2::sys::tlsGet(id); }
static int stub_SDL_TLSSet(unsigned id, const void* value, void* destructor) {
    return sdl2::sys::tlsSet(id, value, reinterpret_cast<sdl2::sys::TlsDestructor>(destructor));
}
static void stub_SDL_TLSCleanup() { sdl2::sys::tlsCleanup(); }

// ----- Mutex, semaphore, condition variable (SDL_mutex.h) -----

static void* stub_SDL_CreateMutex() { return sdl2::sys::createMutex(); }
static void stub_SDL_DestroyMutex(void* m) { sdl2::sys::destroyMutex(static_cast<sdl2::sys::Mutex*>(m)); }
static int stub_SDL_LockMutex(void* m) { return sdl2::sys::lockMutex(static_cast<sdl2::sys::Mutex*>(m)); }
static int stub_SDL_TryLockMutex(void* m) { return sdl2::sys::tryLockMutex(static_cast<sdl2::sys::Mutex*>(m)); }
static int stub_SDL_UnlockMutex(void* m) { return sdl2::sys::unlockMutex(static_cast<sdl2::sys::Mutex*>(m)); }
static void* stub_SDL_CreateSemaphore(uint32_t initial) { return sdl2::sys::createSemaphore(initial); }
static void stub_SDL_DestroySemaphore(void* s) { sdl2::sys::destroySemaphore(static_cast<sdl2::sys::Semaphore*>(s)); }
static int stub_SDL_SemWait(void* s) { return sdl2::sys::semWait(static_cast<sdl2::sys::Semaphore*>(s)); }
static int stub_SDL_SemTryWait(void* s) { return sdl2::sys::semTryWait(static_cast<sdl2::sys::Semaphore*>(s)); }
static int stub_SDL_SemWaitTimeout(void* s, uint32_t ms) { return sdl2::sys::semWaitTimeout(static_cast<sdl2::sys::Semaphore*>(s), ms); }
static int stub_SDL_SemPost(void* s) { return sdl2::sys::semPost(static_cast<sdl2::sys::Semaphore*>(s)); }
static uint32_t stub_SDL_SemValue(void* s) { return sdl2::sys::semValue(static_cast<sdl2::sys::Semaphore*>(s)); }
static void* stub_SDL_CreateCond() { return sdl2::sys::createCond(); }
static void stub_SDL_DestroyCond(void* c) { sdl2::sys::destroyCond(static_cast<sdl2::sys::Cond*>(c)); }
static int stub_SDL_CondSignal(void* c) { return sdl2::sys::condSignal(static_cast<sdl2::sys::Cond*>(c)); }
static int stub_SDL_CondBroadcast(void* c) { return sdl2::sys::condBroadcast(static_cast<sdl2::sys::Cond*>(c)); }
static int stub_SDL_CondWait(void* c, void* m) {
    return sdl2::sys::condWait(static_cast<sdl2::sys::Cond*>(c), static_cast<sdl2::sys::Mutex*>(m));
}
static int stub_SDL_CondWaitTimeout(void* c, void* m, uint32_t ms) {
    return sdl2::sys::condWaitTimeout(static_cast<sdl2::sys::Cond*>(c), static_cast<sdl2::sys::Mutex*>(m), ms);
}

// ----- Atomics (SDL_atomic.h) -----

static int stub_SDL_AtomicAdd(void* a, int v) { return sdl2::sys::atomicAdd(static_cast<sdl2::sys::AtomicInt*>(a), v); }
static int stub_SDL_AtomicCAS(void* a, int o, int n) { return sdl2::sys::atomicCAS(static_cast<sdl2::sys::AtomicInt*>(a), o, n); }
static int stub_SDL_AtomicSet(void* a, int v) { return sdl2::sys::atomicSet(static_cast<sdl2::sys::AtomicInt*>(a), v); }
static int stub_SDL_AtomicGet(void* a) { return sdl2::sys::atomicGet(static_cast<sdl2::sys::AtomicInt*>(a)); }
static int stub_SDL_AtomicCASPtr(void** a, void* o, void* n) { return sdl2::sys::atomicCASPtr(a, o, n); }
static void* stub_SDL_AtomicSetPtr(void** a, void* v) { return sdl2::sys::atomicSetPtr(a, v); }
static void* stub_SDL_AtomicGetPtr(void** a) { return sdl2::sys::atomicGetPtr(a); }
static int stub_SDL_AtomicTryLock(int* l) { return sdl2::sys::atomicTryLock(l); }
static void stub_SDL_AtomicLock(int* l) { sdl2::sys::atomicLock(l); }
static void stub_SDL_AtomicUnlock(int* l) { sdl2::sys::atomicUnlock(l); }
static void stub_SDL_MemoryBarrierReleaseFunction() { sdl2::sys::memoryBarrierRelease(); }
static void stub_SDL_MemoryBarrierAcquireFunction() { sdl2::sys::memoryBarrierAcquire(); }

// ----- Timers (SDL_timer.h) -----

static int stub_SDL_AddTimer(uint32_t interval, void* callback, void* param) {
    return sdl2::sys::addTimer(interval, reinterpret_cast<sdl2::sys::TimerCallback>(callback), param);
}
static int stub_SDL_RemoveTimer(int id) { return sdl2::sys::removeTimer(id); }

// ----- Hints (SDL_hints.h) -----

static int stub_SDL_SetHintWithPriority(const char* n, const char* v, int p) { return sdl2::base::setHintWithPriority(n, v, p); }
static int stub_SDL_GetHintBoolean(const char* n, int d) { return sdl2::base::getHintBoolean(n, d); }
static int stub_SDL_ResetHint(const char* n) { return sdl2::base::resetHint(n); }
static void stub_SDL_ResetHints() { sdl2::base::resetHints(); }
static void stub_SDL_ClearHints() { sdl2::base::clearHints(); }
static void stub_SDL_AddHintCallback(const char* n, void* cb, void* ud) {
    sdl2::base::addHintCallback(n, reinterpret_cast<sdl2::base::HintCallback>(cb), ud);
}
static void stub_SDL_DelHintCallback(const char* n, void* cb, void* ud) {
    sdl2::base::delHintCallback(n, reinterpret_cast<sdl2::base::HintCallback>(cb), ud);
}

// ----- Log (SDL_log.h) -----

static void stub_SDL_LogSetAllPriority(int p) { sdl2::base::logSetAllPriority(p); }
static void stub_SDL_LogSetPriority(int c, int p) { sdl2::base::logSetPriority(c, p); }
static int stub_SDL_LogGetPriority(int c) { return sdl2::base::logGetPriority(c); }
static void stub_SDL_LogResetPriorities() { sdl2::base::logResetPriorities(); }
static void stub_SDL_LogGetOutputFunction(void** cb, void** ud) {
    sdl2::base::LogOutputFunction f = nullptr;
    sdl2::base::logGetOutputFunction(&f, ud);
    if (cb) *cb = reinterpret_cast<void*>(f);
}
static void stub_SDL_LogSetOutputFunction(void* cb, void* ud) {
    sdl2::base::logSetOutputFunction(reinterpret_cast<sdl2::base::LogOutputFunction>(cb), ud);
}
static void stub_SDL_LogMessageV(int c, int p, const char* fmt, va_list ap) { sdl2::base::logMessageV(c, p, fmt, ap); }
#define PHARO_LOG_STUB(name, level) \
    static void stub_SDL_##name(int category, const char* fmt, ...) { \
        va_list ap; va_start(ap, fmt); sdl2::base::logMessageV(category, level, fmt, ap); va_end(ap); }
PHARO_LOG_STUB(LogVerbose, sdl2::base::kLogVerbose)
PHARO_LOG_STUB(LogDebug, sdl2::base::kLogDebug)
PHARO_LOG_STUB(LogInfo, sdl2::base::kLogInfo)
PHARO_LOG_STUB(LogWarn, sdl2::base::kLogWarn)
PHARO_LOG_STUB(LogError, sdl2::base::kLogErr)
PHARO_LOG_STUB(LogCritical, sdl2::base::kLogCritical)
#undef PHARO_LOG_STUB
static void stub_SDL_LogMessage(int c, int p, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); sdl2::base::logMessageV(c, p, fmt, ap); va_end(ap);
}
static void stub_SDL_Log(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); sdl2::base::logMessageV(sdl2::base::kLogApplication, sdl2::base::kLogInfo, fmt, ap); va_end(ap);
}

// ----- Assertions (SDL_assert.h) -----

static int stub_SDL_ReportAssertion(void* data, const char* fn, const char* file, int line) {
    return sdl2::base::reportAssertion(static_cast<sdl2::base::AssertData*>(data), fn, file, line);
}
static void stub_SDL_SetAssertionHandler(void* h, void* ud) {
    sdl2::base::setAssertionHandler(reinterpret_cast<sdl2::base::AssertionHandler>(h), ud);
}
static void* stub_SDL_GetAssertionHandler(void** ud) { return reinterpret_cast<void*>(sdl2::base::getAssertionHandler(ud)); }
static void* stub_SDL_GetDefaultAssertionHandler() { return reinterpret_cast<void*>(sdl2::base::getDefaultAssertionHandler()); }
static const void* stub_SDL_GetAssertionReport() { return sdl2::base::getAssertionReport(); }
static void stub_SDL_ResetAssertionReport() { sdl2::base::resetAssertionReport(); }

// ----- Environment, strings and memory (SDL_stdinc.h) -----

static char* stub_SDL_getenv(const char* n) { return const_cast<char*>(sdl2::base::envLookup(n)); }
static int stub_SDL_setenv(const char* n, const char* v, int o) { return sdl2::base::envSet(n, v, o); }

static char* stub_SDL_itoa(int v, char* s, int r) { return sdl2::base::itoa_(v, s, r); }
static char* stub_SDL_uitoa(unsigned v, char* s, int r) { return sdl2::base::uitoa_(v, s, r); }
static char* stub_SDL_ltoa(long v, char* s, int r) { return sdl2::base::ltoa_(v, s, r); }
static char* stub_SDL_ultoa(unsigned long v, char* s, int r) { return sdl2::base::ultoa_(v, s, r); }
static char* stub_SDL_lltoa(long long v, char* s, int r) { return sdl2::base::lltoa_(v, s, r); }
static char* stub_SDL_ulltoa(unsigned long long v, char* s, int r) { return sdl2::base::ulltoa_(v, s, r); }
static char* stub_SDL_strrev(char* s) { return sdl2::base::strRev(s); }
static char* stub_SDL_strupr(char* s) { return sdl2::base::strUpr(s); }
static char* stub_SDL_strlwr(char* s) { return sdl2::base::strLwr(s); }
static char* stub_SDL_strtokr(char* s1, const char* s2, char** save) { return sdl2::base::strTokR(s1, s2, save); }
static size_t stub_SDL_strlcat(char* d, const char* s, size_t n) { return sdl2::base::strLCat(d, s, n); }
static size_t stub_SDL_strlcpy(char* d, const char* s, size_t n) { return sdl2::base::strLCpy(d, s, n); }
static char* stub_SDL_strcasestr(const char* h, const char* n) { return const_cast<char*>(sdl2::base::strCaseStr(h, n)); }
static size_t stub_SDL_utf8strlen(const char* s) { return sdl2::base::utf8Len(s); }
static size_t stub_SDL_utf8strnlen(const char* s, size_t n) { return sdl2::base::utf8NLen(s, n); }
static size_t stub_SDL_utf8strlcpy(char* d, const char* s, size_t n) { return sdl2::base::utf8LCpy(d, s, n); }
static uint16_t stub_SDL_crc16(uint16_t crc, const void* data, size_t len) { return sdl2::base::crc16(crc, data, len); }
static uint32_t stub_SDL_crc32(uint32_t crc, const void* data, size_t len) { return sdl2::base::crc32(crc, data, len); }
static int stub_SDL_abs(int x) { return x < 0 ? -x : x; }
static int stub_SDL_isalpha(int c) { return isalpha(c); }
static int stub_SDL_isalnum(int c) { return isalnum(c); }
static int stub_SDL_isblank(int c) { return c == ' ' || c == '\t'; }
static int stub_SDL_iscntrl(int c) { return iscntrl(c); }
static int stub_SDL_isdigit(int c) { return isdigit(c); }
static int stub_SDL_isxdigit(int c) { return isxdigit(c); }
static int stub_SDL_ispunct(int c) { return ispunct(c); }
static int stub_SDL_isspace(int c) { return isspace(c); }
static int stub_SDL_isupper(int c) { return isupper(c); }
static int stub_SDL_islower(int c) { return islower(c); }
static int stub_SDL_isprint(int c) { return isprint(c); }
static int stub_SDL_isgraph(int c) { return isgraph(c); }
static int stub_SDL_toupper(int c) { return toupper(c); }
static int stub_SDL_tolower(int c) { return tolower(c); }

static void* stub_SDL_SIMDAlloc(size_t n) { return sdl2::base::simdAlloc(n); }
static void* stub_SDL_SIMDRealloc(void* m, size_t n) { return sdl2::base::simdRealloc(m, n); }
static void stub_SDL_SIMDFree(void* m) { sdl2::base::simdFree(m); }
static size_t stub_SDL_SIMDGetAlignment() { return sdl2::base::simdAlignment(); }

// ----- Platform, CPU, power, version, shared objects, paths -----

static const char* stub_SDL_GetPlatform() { return sdl2::base::platformName(); }
static int stub_SDL_GetCPUCount() { return sdl2::base::cpuCount(); }
static int stub_SDL_GetCPUCacheLineSize() { return sdl2::base::cpuCacheLineSize(); }
static int stub_SDL_GetSystemRAM() { return sdl2::base::systemRAM(); }
static int stub_SDL_HasRDTSC() { return sdl2::base::hasRDTSC(); }
static int stub_SDL_HasMMX() { return sdl2::base::hasMMX(); }
static int stub_SDL_HasSSE() { return sdl2::base::hasSSE(); }
static int stub_SDL_HasSSE2() { return sdl2::base::hasSSE2(); }
static int stub_SDL_HasSSE3() { return sdl2::base::hasSSE3(); }
static int stub_SDL_HasSSE41() { return sdl2::base::hasSSE41(); }
static int stub_SDL_HasSSE42() { return sdl2::base::hasSSE42(); }
static int stub_SDL_HasAVX() { return sdl2::base::hasAVX(); }
static int stub_SDL_HasAVX2() { return sdl2::base::hasAVX2(); }
static int stub_SDL_HasAVX512F() { return sdl2::base::hasAVX512F(); }
static int stub_SDL_HasNEON() { return sdl2::base::hasNEON(); }
static int stub_SDL_Has3DNow() { return 0; }
static int stub_SDL_HasAltiVec() { return 0; }
static int stub_SDL_HasARMSIMD() { return 0; }
static int stub_SDL_HasLSX() { return 0; }
static int stub_SDL_HasLASX() { return 0; }

// The host gives no battery state to the VM: the answer is "unknown" with no
// seconds and no percentage, the answer of SDL on a platform without power
// information.
static int stub_SDL_GetPowerInfo(int* seconds, int* percent) {
    constexpr int kPowerStateUnknown = 0;
    if (seconds) *seconds = -1;
    if (percent) *percent = -1;
    return kPowerStateUnknown;
}

static const char* stub_SDL_GetRevision() { return "SDL-release-2.26.5-0-gac13ca9ab"; }
static int stub_SDL_GetRevisionNumber() { return 0; }
static void stub_SDL_SetMainReady() {}

static void* stub_SDL_LoadObject(const char* n) { return sdl2::base::loadObject(n); }
static void* stub_SDL_LoadFunction(void* h, const char* n) { return sdl2::base::loadFunction(h, n); }
static void stub_SDL_UnloadObject(void* h) { sdl2::base::unloadObject(h); }
static char* stub_SDL_GetBasePath() { return sdl2::base::getBasePath(); }
static char* stub_SDL_GetPrefPath(const char* o, const char* a) { return sdl2::base::getPrefPath(o, a); }

// A structure that SDL passes by value: 16 bytes in two registers.
static void stub_SDL_GUIDToString(sdl2::base::Guid g, char* s, int n) { sdl2::base::guidToString(g, s, n); }
static sdl2::base::Guid stub_SDL_GUIDFromString(const char* s) { return sdl2::base::guidFromString(s); }

// ----- RWops (SDL_rwops.h) -----

static void* stub_SDL_AllocRW() { return sdl2::rw::allocRW(); }
static void stub_SDL_FreeRW(void* r) { sdl2::rw::freeRW(static_cast<sdl2::rw::RWops*>(r)); }
static void* stub_SDL_RWFromFile(const char* f, const char* m) { return sdl2::rw::fromFile(f, m); }
static void* stub_SDL_RWFromFP(FILE* fp, int autoclose) { return sdl2::rw::fromFP(fp, autoclose); }
static void* stub_SDL_RWFromMem(void* m, int n) { return sdl2::rw::fromMem(m, n); }
static void* stub_SDL_RWFromConstMem(const void* m, int n) { return sdl2::rw::fromConstMem(m, n); }
static int64_t stub_SDL_RWsize(void* c) { return sdl2::rw::rwSize(static_cast<sdl2::rw::RWops*>(c)); }
static int64_t stub_SDL_RWseek(void* c, int64_t o, int w) { return sdl2::rw::rwSeek(static_cast<sdl2::rw::RWops*>(c), o, w); }
static int64_t stub_SDL_RWtell(void* c) { return sdl2::rw::rwTell(static_cast<sdl2::rw::RWops*>(c)); }
static size_t stub_SDL_RWread(void* c, void* p, size_t s, size_t n) { return sdl2::rw::rwRead(static_cast<sdl2::rw::RWops*>(c), p, s, n); }
static size_t stub_SDL_RWwrite(void* c, const void* p, size_t s, size_t n) { return sdl2::rw::rwWrite(static_cast<sdl2::rw::RWops*>(c), p, s, n); }
static int stub_SDL_RWclose(void* c) { return sdl2::rw::rwClose(static_cast<sdl2::rw::RWops*>(c)); }
static void* stub_SDL_LoadFile_RW(void* s, size_t* n, int f) { return sdl2::rw::loadFileRW(static_cast<sdl2::rw::RWops*>(s), n, f); }
static void* stub_SDL_LoadFile(const char* f, size_t* n) { return sdl2::rw::loadFile(f, n); }
static uint8_t stub_SDL_ReadU8(void* s) { return sdl2::rw::readU8(static_cast<sdl2::rw::RWops*>(s)); }
static uint16_t stub_SDL_ReadLE16(void* s) { return sdl2::rw::readLE16(static_cast<sdl2::rw::RWops*>(s)); }
static uint16_t stub_SDL_ReadBE16(void* s) { return sdl2::rw::readBE16(static_cast<sdl2::rw::RWops*>(s)); }
static uint32_t stub_SDL_ReadLE32(void* s) { return sdl2::rw::readLE32(static_cast<sdl2::rw::RWops*>(s)); }
static uint32_t stub_SDL_ReadBE32(void* s) { return sdl2::rw::readBE32(static_cast<sdl2::rw::RWops*>(s)); }
static uint64_t stub_SDL_ReadLE64(void* s) { return sdl2::rw::readLE64(static_cast<sdl2::rw::RWops*>(s)); }
static uint64_t stub_SDL_ReadBE64(void* s) { return sdl2::rw::readBE64(static_cast<sdl2::rw::RWops*>(s)); }
static size_t stub_SDL_WriteU8(void* d, uint8_t v) { return sdl2::rw::writeU8(static_cast<sdl2::rw::RWops*>(d), v); }
static size_t stub_SDL_WriteLE16(void* d, uint16_t v) { return sdl2::rw::writeLE16(static_cast<sdl2::rw::RWops*>(d), v); }
static size_t stub_SDL_WriteBE16(void* d, uint16_t v) { return sdl2::rw::writeBE16(static_cast<sdl2::rw::RWops*>(d), v); }
static size_t stub_SDL_WriteLE32(void* d, uint32_t v) { return sdl2::rw::writeLE32(static_cast<sdl2::rw::RWops*>(d), v); }
static size_t stub_SDL_WriteBE32(void* d, uint32_t v) { return sdl2::rw::writeBE32(static_cast<sdl2::rw::RWops*>(d), v); }
static size_t stub_SDL_WriteLE64(void* d, uint64_t v) { return sdl2::rw::writeLE64(static_cast<sdl2::rw::RWops*>(d), v); }
static size_t stub_SDL_WriteBE64(void* d, uint64_t v) { return sdl2::rw::writeBE64(static_cast<sdl2::rw::RWops*>(d), v); }

// ----- Rectangles (SDL_rect.h) -----

namespace rectapi {
using IRect = sdl2::rect::RectT<int>;
using FRect = sdl2::rect::RectT<float>;
using IPoint = sdl2::rect::PointT<int>;
using FPoint = sdl2::rect::PointT<float>;
}
static int stub_SDL_HasIntersection(const void* a, const void* b) {
    return sdl2::rect::hasIntersection(static_cast<const rectapi::IRect*>(a), static_cast<const rectapi::IRect*>(b));
}
static int stub_SDL_HasIntersectionF(const void* a, const void* b) {
    return sdl2::rect::hasIntersection(static_cast<const rectapi::FRect*>(a), static_cast<const rectapi::FRect*>(b));
}
static int stub_SDL_IntersectRect(const void* a, const void* b, void* r) {
    return sdl2::rect::intersectRect(static_cast<const rectapi::IRect*>(a), static_cast<const rectapi::IRect*>(b), static_cast<rectapi::IRect*>(r));
}
static int stub_SDL_IntersectFRect(const void* a, const void* b, void* r) {
    return sdl2::rect::intersectRect(static_cast<const rectapi::FRect*>(a), static_cast<const rectapi::FRect*>(b), static_cast<rectapi::FRect*>(r));
}
static void stub_SDL_UnionRect(const void* a, const void* b, void* r) {
    sdl2::rect::unionRect(static_cast<const rectapi::IRect*>(a), static_cast<const rectapi::IRect*>(b), static_cast<rectapi::IRect*>(r));
}
static void stub_SDL_UnionFRect(const void* a, const void* b, void* r) {
    sdl2::rect::unionRect(static_cast<const rectapi::FRect*>(a), static_cast<const rectapi::FRect*>(b), static_cast<rectapi::FRect*>(r));
}
static int stub_SDL_EnclosePoints(const void* p, int n, const void* clip, void* r) {
    return sdl2::rect::enclosePoints(static_cast<const rectapi::IPoint*>(p), n, static_cast<const rectapi::IRect*>(clip), static_cast<rectapi::IRect*>(r));
}
static int stub_SDL_EncloseFPoints(const void* p, int n, const void* clip, void* r) {
    return sdl2::rect::enclosePoints(static_cast<const rectapi::FPoint*>(p), n, static_cast<const rectapi::FRect*>(clip), static_cast<rectapi::FRect*>(r));
}
static int stub_SDL_IntersectRectAndLine(const void* r, int* x1, int* y1, int* x2, int* y2) {
    return sdl2::rect::intersectRectAndLine(static_cast<const rectapi::IRect*>(r), x1, y1, x2, y2);
}
static int stub_SDL_IntersectFRectAndLine(const void* r, float* x1, float* y1, float* x2, float* y2) {
    return sdl2::rect::intersectRectAndLine(static_cast<const rectapi::FRect*>(r), x1, y1, x2, y2);
}
