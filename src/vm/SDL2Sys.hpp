/*
 * SDL2Sys.hpp - threads, locks, atomics, thread-local storage and timers of
 * the SDL2 stub layer.
 *
 * Written from the documented SDL 2.x behaviour (SDL_thread.h, SDL_mutex.h,
 * SDL_atomic.h, SDL_timer.h) and checked against a real libSDL2.  Every
 * object is a real native object: a thread is a pthread, a mutex is a
 * recursive pthread mutex, a timer fires on its own thread.
 *
 * A function pointer that the image passes in (thread function, timer
 * callback) can be a Smalltalk callback thunk.  The thunk is then entered
 * from a thread that is not the VM thread.  The VM already handles that: the
 * thunk handler queues the call for the VM thread and parks the calling
 * thread until the image has answered (the cross-thread callback queue of
 * Primitives.cpp, "TFFI v2").  This file therefore needs no callback code of
 * its own: a native thread calls the pointer like any C function.
 *
 * Copyright (c) 2025-2026 Aaron Wohl. Licensed under the MIT License.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <sys/time.h>
#include <unistd.h>

namespace pharo {
namespace sdl2 {
namespace sys {

// ---------------------------------------------------------------------------
// Error state (SDL_error.h).  The error text belongs to the calling thread,
// as in SDL.
// ---------------------------------------------------------------------------

inline std::string& lastError() {
    static thread_local std::string error;
    return error;
}

inline int setError(const char* message) {
    lastError() = message ? message : "";
    return -1;
}

inline int setErrorV(const char* fmt, va_list ap) {
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt ? fmt : "", ap);
    lastError() = buf;
    return -1;
}

inline int invalidParam(const char* name) {
    return setError((std::string("Parameter '") + name + "' is invalid").c_str());
}

constexpr int kMutexTimedOut = 1;                 // SDL_MUTEX_TIMEDOUT
constexpr uint32_t kMutexMaxWait = 0xFFFFFFFFu;   // SDL_MUTEX_MAXWAIT is (Uint32)-1

// ---------------------------------------------------------------------------
// Mutex, semaphore and condition variable (SDL_mutex.h).
// ---------------------------------------------------------------------------

struct Mutex { pthread_mutex_t id; };

struct Semaphore {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    uint32_t count;
};

struct Cond { pthread_cond_t id; };

inline Mutex* createMutex() {
    auto* m = static_cast<Mutex*>(calloc(1, sizeof(Mutex)));
    if (!m) { setError("Out of memory"); return nullptr; }
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    const int rc = pthread_mutex_init(&m->id, &attr);
    pthread_mutexattr_destroy(&attr);
    if (rc != 0) { setError("pthread_mutex_init() failed"); free(m); return nullptr; }
    return m;
}

inline void destroyMutex(Mutex* m) {
    if (!m) return;
    pthread_mutex_destroy(&m->id);
    free(m);
}

inline int lockMutex(Mutex* m) {
    if (!m) return invalidParam("mutex");
    if (pthread_mutex_lock(&m->id) != 0) return setError("pthread_mutex_lock() failed");
    return 0;
}

inline int tryLockMutex(Mutex* m) {
    if (!m) return invalidParam("mutex");
    const int rc = pthread_mutex_trylock(&m->id);
    if (rc == 0) return 0;
    if (rc == EBUSY) return kMutexTimedOut;
    return setError("pthread_mutex_trylock() failed");
}

inline int unlockMutex(Mutex* m) {
    if (!m) return invalidParam("mutex");
    if (pthread_mutex_unlock(&m->id) != 0) return setError("pthread_mutex_unlock() failed");
    return 0;
}

inline Semaphore* createSemaphore(uint32_t initial) {
    auto* s = static_cast<Semaphore*>(calloc(1, sizeof(Semaphore)));
    if (!s) { setError("Out of memory"); return nullptr; }
    pthread_mutex_init(&s->lock, nullptr);
    pthread_cond_init(&s->changed, nullptr);
    s->count = initial;
    return s;
}

inline void destroySemaphore(Semaphore* s) {
    if (!s) return;
    pthread_cond_destroy(&s->changed);
    pthread_mutex_destroy(&s->lock);
    free(s);
}

inline void absoluteDeadline(struct timespec* ts, uint32_t ms) {
    struct timeval now;
    gettimeofday(&now, nullptr);
    int64_t ns = static_cast<int64_t>(now.tv_usec) * 1000 + static_cast<int64_t>(ms % 1000) * 1000000;
    ts->tv_sec = now.tv_sec + ms / 1000 + ns / 1000000000;
    ts->tv_nsec = static_cast<long>(ns % 1000000000);
}

inline int semWaitTimeout(Semaphore* s, uint32_t ms) {
    if (!s) return invalidParam("sem");
    struct timespec deadline;
    if (ms != kMutexMaxWait && ms != 0) absoluteDeadline(&deadline, ms);
    pthread_mutex_lock(&s->lock);
    int result = 0;
    while (s->count == 0) {
        if (ms == 0) { result = kMutexTimedOut; break; }
        if (ms == kMutexMaxWait) {
            pthread_cond_wait(&s->changed, &s->lock);
        } else if (pthread_cond_timedwait(&s->changed, &s->lock, &deadline) == ETIMEDOUT) {
            result = kMutexTimedOut;
            break;
        }
    }
    if (result == 0) --s->count;
    pthread_mutex_unlock(&s->lock);
    return result;
}

inline int semWait(Semaphore* s) {
    if (!s) return invalidParam("sem");
    return semWaitTimeout(s, kMutexMaxWait);
}

inline int semTryWait(Semaphore* s) {
    if (!s) return invalidParam("sem");
    return semWaitTimeout(s, 0);
}

inline int semPost(Semaphore* s) {
    if (!s) return invalidParam("sem");
    pthread_mutex_lock(&s->lock);
    ++s->count;
    pthread_cond_signal(&s->changed);
    pthread_mutex_unlock(&s->lock);
    return 0;
}

inline uint32_t semValue(Semaphore* s) {
    if (!s) return 0;   // SDL answers 0 for a null semaphore and sets no error
    pthread_mutex_lock(&s->lock);
    const uint32_t v = s->count;
    pthread_mutex_unlock(&s->lock);
    return v;
}

inline Cond* createCond() {
    auto* c = static_cast<Cond*>(calloc(1, sizeof(Cond)));
    if (!c) { setError("Out of memory"); return nullptr; }
    if (pthread_cond_init(&c->id, nullptr) != 0) { setError("pthread_cond_init() failed"); free(c); return nullptr; }
    return c;
}

inline void destroyCond(Cond* c) {
    if (!c) return;
    pthread_cond_destroy(&c->id);
    free(c);
}

inline int condSignal(Cond* c) {
    if (!c) return invalidParam("cond");
    if (pthread_cond_signal(&c->id) != 0) return setError("pthread_cond_signal() failed");
    return 0;
}

inline int condBroadcast(Cond* c) {
    if (!c) return invalidParam("cond");
    if (pthread_cond_broadcast(&c->id) != 0) return setError("pthread_cond_broadcast() failed");
    return 0;
}

inline int condWait(Cond* c, Mutex* m) {
    if (!c) return invalidParam("cond");
    if (!m) return invalidParam("mutex");
    if (pthread_cond_wait(&c->id, &m->id) != 0) return setError("pthread_cond_wait() failed");
    return 0;
}

inline int condWaitTimeout(Cond* c, Mutex* m, uint32_t ms) {
    if (!c) return invalidParam("cond");
    if (!m) return invalidParam("mutex");
    if (ms == kMutexMaxWait) return condWait(c, m);
    struct timespec deadline;
    absoluteDeadline(&deadline, ms);
    const int rc = pthread_cond_timedwait(&c->id, &m->id, &deadline);
    if (rc == 0) return 0;
    if (rc == ETIMEDOUT) return kMutexTimedOut;
    return setError("pthread_cond_timedwait() failed");
}

// ---------------------------------------------------------------------------
// Atomics (SDL_atomic.h).  SDL_atomic_t is a struct that holds one int.
// ---------------------------------------------------------------------------

struct AtomicInt { volatile int value; };

inline int atomicAdd(AtomicInt* a, int v) { return __atomic_fetch_add(&a->value, v, __ATOMIC_SEQ_CST); }
inline int atomicCAS(AtomicInt* a, int oldValue, int newValue) {
    return __atomic_compare_exchange_n(&a->value, &oldValue, newValue, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 1 : 0;
}
inline int atomicSet(AtomicInt* a, int v) { return __atomic_exchange_n(&a->value, v, __ATOMIC_SEQ_CST); }
inline int atomicGet(AtomicInt* a) { return __atomic_load_n(&a->value, __ATOMIC_SEQ_CST); }
inline int atomicCASPtr(void** a, void* oldValue, void* newValue) {
    return __atomic_compare_exchange_n(a, &oldValue, newValue, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 1 : 0;
}
inline void* atomicSetPtr(void** a, void* v) { return __atomic_exchange_n(a, v, __ATOMIC_SEQ_CST); }
inline void* atomicGetPtr(void** a) { return __atomic_load_n(a, __ATOMIC_SEQ_CST); }

inline int atomicTryLock(int* lock) { return __atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE) == 0 ? 1 : 0; }
inline void atomicLock(int* lock) {
    while (!atomicTryLock(lock)) sched_yield();
}
inline void atomicUnlock(int* lock) { __atomic_store_n(lock, 0, __ATOMIC_RELEASE); }
inline void memoryBarrierRelease() { __atomic_thread_fence(__ATOMIC_RELEASE); }
inline void memoryBarrierAcquire() { __atomic_thread_fence(__ATOMIC_ACQUIRE); }

// ---------------------------------------------------------------------------
// Thread-local storage (SDL_TLSCreate, SDL_TLSGet, SDL_TLSSet).  Id 1 belongs
// to the error state in SDL, so the first id handed out is 2.
// ---------------------------------------------------------------------------

typedef void (*TlsDestructor)(void*);
struct TlsSlot { void* value = nullptr; TlsDestructor destructor = nullptr; };

inline std::atomic<unsigned>& tlsNextId() {
    static std::atomic<unsigned> next{ 2 };
    return next;
}

inline std::vector<TlsSlot>& tlsSlots() {
    static thread_local std::vector<TlsSlot> slots;
    return slots;
}

inline unsigned tlsCreate() { return tlsNextId().fetch_add(1); }

inline void* tlsGet(unsigned id) {
    auto& slots = tlsSlots();
    if (id == 0 || id >= slots.size()) return nullptr;
    return slots[id].value;
}

inline int tlsSet(unsigned id, const void* value, TlsDestructor destructor) {
    if (id == 0) return invalidParam("id");
    auto& slots = tlsSlots();
    if (id >= slots.size()) slots.resize(id + 1);
    slots[id].value = const_cast<void*>(value);
    slots[id].destructor = destructor;
    return 0;
}

// SDL_TLSCleanup: run the destructor of every value of the calling thread.
inline void tlsCleanup() {
    auto& slots = tlsSlots();
    for (size_t i = 0; i < slots.size(); i++) {
        TlsSlot slot = slots[i];
        slots[i] = TlsSlot();
        if (slot.destructor && slot.value) slot.destructor(slot.value);
    }
    slots.clear();
}

// ---------------------------------------------------------------------------
// Threads (SDL_thread.h).
// ---------------------------------------------------------------------------

typedef int (*ThreadFunction)(void*);

enum ThreadPriority : int { kPriorityLow = 0, kPriorityNormal = 1, kPriorityHigh = 2, kPriorityTimeCritical = 3 };

struct Thread {
    pthread_t handle{};   // an integer on glibc, a pointer on Darwin
    unsigned long id = 0;
    std::string name;
    bool hasName = false;
    ThreadFunction function = nullptr;
    void* data = nullptr;
    int status = 0;
    std::atomic<int> state{ 0 };       // see ThreadState
    Semaphore* ready = nullptr;        // posted once the thread has copied its arguments
};

enum ThreadState : int { kThreadAlive = 0, kThreadDetached = 1, kThreadZombie = 2 };

inline unsigned long currentThreadId() {
    return reinterpret_cast<unsigned long>(pthread_self());
}

inline void* threadMain(void* arg) {
    Thread* self = static_cast<Thread*>(arg);
    ThreadFunction function = self->function;
    void* data = self->data;
    semPost(self->ready);   // the creator may now return
    // The thread function can be a Smalltalk callback.  The VM handler for a
    // foreign thread queues the call for the VM thread and parks this thread.
    const int status = function(data);
    tlsCleanup();
    self->status = status;
    int expected = kThreadAlive;
    if (!self->state.compare_exchange_strong(expected, kThreadZombie)) {
        // Detached: nobody will join, so the thread frees its own record.
        destroySemaphore(self->ready);
        delete self;
    }
    return nullptr;
}

inline Thread* createThreadWithStackSize(ThreadFunction function, const char* name, size_t stackSize, void* data) {
    if (!function) { invalidParam("fn"); return nullptr; }
    auto* t = new Thread();
    t->function = function;
    t->data = data;
    if (name) t->name = name;
    t->hasName = name != nullptr;
    t->ready = createSemaphore(0);
    if (!t->ready) { delete t; return nullptr; }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stackSize > 0) pthread_attr_setstacksize(&attr, stackSize);
    const int rc = pthread_create(&t->handle, &attr, threadMain, t);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        setError("Not enough resources to create thread");
        destroySemaphore(t->ready);
        delete t;
        return nullptr;
    }
    t->id = reinterpret_cast<unsigned long>(t->handle);
    semWait(t->ready);
    return t;
}

inline Thread* createThread(ThreadFunction function, const char* name, void* data) {
    return createThreadWithStackSize(function, name, 0, data);
}

inline const char* threadName(Thread* t) {
    return (t && t->hasName) ? t->name.c_str() : nullptr;
}

inline unsigned long threadId(Thread* t) {
    return t ? t->id : currentThreadId();
}

inline void waitThread(Thread* t, int* status) {
    if (!t) return;
    pthread_join(t->handle, nullptr);
    if (status) *status = t->status;
    destroySemaphore(t->ready);
    delete t;
}

inline void detachThread(Thread* t) {
    if (!t) return;
    // Copy the handle first: once the CAS succeeds the thread owns the record
    // and can free it at any time, so the record must not be read afterwards.
    const pthread_t handle = t->handle;
    int expected = kThreadAlive;
    if (t->state.compare_exchange_strong(expected, kThreadDetached)) {
        pthread_detach(handle);   // the thread frees the record when its function returns
    } else {
        // The function already returned: nobody joined, so free the record now.
        pthread_join(t->handle, nullptr);
        destroySemaphore(t->ready);
        delete t;
    }
}

// SDL_SetThreadPriority sets the priority of the calling thread inside the
// SCHED_OTHER range of the system.
inline int setThreadPriority(int priority) {
    int policy = 0;
    struct sched_param param;
    if (pthread_getschedparam(pthread_self(), &policy, &param) != 0) return setError("pthread_getschedparam() failed");
    const int lo = sched_get_priority_min(policy);
    const int hi = sched_get_priority_max(policy);
    int value = param.sched_priority;
    switch (priority) {
    case kPriorityLow: value = lo; break;
    case kPriorityNormal: value = lo + (hi - lo) / 2; break;
    case kPriorityHigh:
    case kPriorityTimeCritical: value = hi; break;
    default: break;
    }
    param.sched_priority = value;
    if (pthread_setschedparam(pthread_self(), policy, &param) != 0) return setError("pthread_setschedparam() failed");
    return 0;
}

// ---------------------------------------------------------------------------
// Timers (SDL_timer.h).  One timer thread serves every timer, as in SDL.
// The state is immortal: the timer thread can still run at process exit.
// ---------------------------------------------------------------------------

typedef uint32_t (*TimerCallback)(uint32_t interval, void* param);

struct TimerEntry {
    int id;
    TimerCallback callback;
    void* param;
    uint32_t interval;
    std::chrono::steady_clock::time_point due;
    bool canceled = false;
};

struct TimerState {
    std::mutex mx;
    std::condition_variable cv;
    std::map<int, TimerEntry*> timers;     // every timer that RemoveTimer can still find
    int nextId = 1;
    bool threadStarted = false;
};

inline TimerState& timerState() {
    static TimerState* state = new TimerState;   // never freed
    return *state;
}

inline void timerThread() {
    TimerState& ts = timerState();
    std::unique_lock<std::mutex> lk(ts.mx);
    for (;;) {
        TimerEntry* next = nullptr;
        for (auto& kv : ts.timers)
            if (!kv.second->canceled && (!next || kv.second->due < next->due)) next = kv.second;
        if (!next) { ts.cv.wait(lk); continue; }
        // Copy the due time: wait_until keeps a reference to its argument while the lock is
        // released, and RemoveTimer can delete the entry during the wait.
        const auto due = next->due;
        if (std::chrono::steady_clock::now() < due) { ts.cv.wait_until(lk, due); continue; }
        TimerCallback callback = next->callback;
        void* param = next->param;
        const uint32_t interval = next->interval;
        const int id = next->id;
        lk.unlock();
        // A Smalltalk callback runs on the VM thread; this thread waits for the answer.
        const uint32_t again = callback(interval, param);
        lk.lock();
        auto it = ts.timers.find(id);
        if (it == ts.timers.end()) continue;   // removed while the callback ran
        TimerEntry* e = it->second;
        if (e->canceled) continue;
        if (again > 0) {
            e->interval = again;
            e->due = std::chrono::steady_clock::now() + std::chrono::milliseconds(again);
        } else {
            e->canceled = true;   // answered 0: the timer is finished; RemoveTimer then answers false
        }
    }
}

inline int addTimer(uint32_t interval, TimerCallback callback, void* param) {
    TimerState& ts = timerState();
    if (!callback) { invalidParam("callback"); return 0; }
    std::lock_guard<std::mutex> lk(ts.mx);
    if (!ts.threadStarted) {
        ts.threadStarted = true;
        std::thread(timerThread).detach();
    }
    auto* e = new TimerEntry();
    e->id = ts.nextId++;
    e->callback = callback;
    e->param = param;
    e->interval = interval;
    e->due = std::chrono::steady_clock::now() + std::chrono::milliseconds(interval);
    ts.timers[e->id] = e;
    ts.cv.notify_all();
    return e->id;
}

inline int removeTimer(int id) {
    TimerState& ts = timerState();
    std::lock_guard<std::mutex> lk(ts.mx);
    auto it = ts.timers.find(id);
    if (it == ts.timers.end()) return 0;
    const bool wasActive = !it->second->canceled;
    // The timer thread copies what it needs while it holds the lock and finds the
    // record again by id, so the record can go now.
    delete it->second;
    ts.timers.erase(it);
    return wasActive ? 1 : 0;
}

} // namespace sys
} // namespace sdl2
} // namespace pharo
