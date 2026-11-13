/*
 * SDL2Base.hpp - environment, hints, log, assertions, platform queries,
 * shared objects, GUIDs, text conversion and the small C helpers of SDL2.
 *
 * Written from the documented SDL 2.x behaviour (SDL_hints.h, SDL_log.h,
 * SDL_assert.h, SDL_cpuinfo.h, SDL_platform.h, SDL_loadso.h,
 * SDL_filesystem.h, SDL_guid.h, SDL_stdinc.h) and checked against a real
 * libSDL2.  No window or renderer state is needed here.
 *
 * Environment: SDL reads the process environment on every SDL_getenv and
 * SDL_GetHint call.  This layer copies the environment ONCE, at the first
 * lookup, into a table that SDL_getenv, SDL_setenv, SDL_unsetenv and the hint
 * functions share.  SDL_setenv also writes through to the process
 * environment, so a child process sees the change.  A change made by other
 * code with the C library (setenv) after the first lookup is not seen.
 *
 * Copyright (c) 2025-2026 Aaron Wohl. Licensed under the MIT License.
 */
#pragma once

#include "SDL2Sys.hpp"
#include "SDL2Utf.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cwchar>
#include <dlfcn.h>
#include <map>
#include <sys/stat.h>
#include <sys/types.h>
#include <unordered_map>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <crt_externs.h>
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
#else
extern char** environ;
#endif
#if !defined(_WIN32)
#include <pwd.h>
#endif

namespace pharo {
namespace sdl2 {
namespace base {

using sys::setError;
using sys::invalidParam;

// ---------------------------------------------------------------------------
// Environment snapshot.
// ---------------------------------------------------------------------------

struct EnvTable {
    std::mutex mx;
    std::unordered_map<std::string, std::string> vars;
    bool loaded = false;
};

inline EnvTable& envTable() {
    static EnvTable* table = new EnvTable;   // never freed: lookups can run during exit
    return *table;
}

inline void envLoadLocked(EnvTable& t) {
    if (t.loaded) return;
    t.loaded = true;
#if defined(__APPLE__)
    char** env = *_NSGetEnviron();
#else
    char** env = environ;
#endif
    for (; env && *env; ++env) {
        const char* eq = strchr(*env, '=');
        if (!eq || eq == *env) continue;
        t.vars[std::string(*env, eq - *env)] = eq + 1;
    }
}

// SDL_getenv: the pointer stays valid until that variable changes.
inline const char* envLookup(const char* name) {
    if (!name) return nullptr;
    EnvTable& t = envTable();
    std::lock_guard<std::mutex> lk(t.mx);
    envLoadLocked(t);
    auto it = t.vars.find(name);
    return it == t.vars.end() ? nullptr : it->second.c_str();
}

inline int envSet(const char* name, const char* value, int overwrite) {
    if (!name || !*name || strchr(name, '=') || !value) return -1;
    EnvTable& t = envTable();
    std::lock_guard<std::mutex> lk(t.mx);
    envLoadLocked(t);
    if (!overwrite && t.vars.count(name)) return 0;
    if (setenv(name, value, 1) != 0) return -1;
    t.vars[name] = value;
    return 0;
}

inline int envUnset(const char* name) {
    if (!name || !*name || strchr(name, '=')) return -1;
    EnvTable& t = envTable();
    std::lock_guard<std::mutex> lk(t.mx);
    envLoadLocked(t);
    t.vars.erase(name);
    return unsetenv(name);
}

// ---------------------------------------------------------------------------
// Hints (SDL_hints.h).
// ---------------------------------------------------------------------------

enum HintPriority : int { kHintDefault = 0, kHintNormal = 1, kHintOverride = 2 };
typedef void (*HintCallback)(void* userdata, const char* name, const char* oldValue, const char* newValue);

struct HintWatch { HintCallback callback; void* userdata; };

struct Hint {
    bool hasValue = false;
    std::string value;
    int priority = kHintDefault;
    std::vector<HintWatch> watches;   // newest first, the order SDL calls them
};

struct HintTable {
    std::recursive_mutex mx;
    std::map<std::string, Hint> hints;
};

inline HintTable& hintTable() {
    static HintTable* table = new HintTable;
    return *table;
}

// Copy the strings, then call the watches without the lock, so that a watch
// (a Smalltalk callback, say) can call back into the hint functions.
inline void hintNotify(const std::vector<HintWatch>& watches, const std::string& name,
                       const char* oldValue, const char* newValue) {
    const bool hasOld = oldValue != nullptr, hasNew = newValue != nullptr;
    const std::string oldCopy = hasOld ? oldValue : "", newCopy = hasNew ? newValue : "";
    for (const HintWatch& w : watches)
        w.callback(w.userdata, name.c_str(), hasOld ? oldCopy.c_str() : nullptr, hasNew ? newCopy.c_str() : nullptr);
}

inline int setHintWithPriority(const char* name, const char* value, int priority) {
    if (!name) return 0;
    if (envLookup(name) && priority < kHintOverride) return 0;
    HintTable& t = hintTable();
    std::vector<HintWatch> watches;
    std::string oldValue;
    bool hadOld = false, changed = false;
    {
        std::lock_guard<std::recursive_mutex> lk(t.mx);
        auto it = t.hints.find(name);
        if (it == t.hints.end()) {
            Hint h;
            h.hasValue = value != nullptr;
            if (value) h.value = value;
            h.priority = priority;
            t.hints[name] = h;
            return 1;
        }
        Hint& h = it->second;
        if (priority < h.priority) return 0;
        const bool same = (!value && !h.hasValue) || (value && h.hasValue && h.value == value);
        if (!same) {
            watches = h.watches;
            hadOld = h.hasValue;
            oldValue = h.value;
            changed = true;
        }
        h.priority = priority;
        if (changed) {
            // SDL calls the watches before it stores the new value.
            // The watch list is a copy, so a watch can change the table.
        }
    }
    if (changed) {
        hintNotify(watches, name, hadOld ? oldValue.c_str() : nullptr, value);
        std::lock_guard<std::recursive_mutex> lk(t.mx);
        Hint& h = t.hints[name];
        h.hasValue = value != nullptr;
        h.value = value ? value : "";
    }
    return 1;
}

inline int setHint(const char* name, const char* value) {
    return setHintWithPriority(name, value, kHintNormal);
}

inline const char* getHint(const char* name) {
    if (!name) return nullptr;
    const char* env = envLookup(name);
    HintTable& t = hintTable();
    std::lock_guard<std::recursive_mutex> lk(t.mx);
    auto it = t.hints.find(name);
    if (it != t.hints.end() && (!env || it->second.priority == kHintOverride))
        return it->second.hasValue ? it->second.value.c_str() : nullptr;
    return env;
}

inline int stringBoolean(const char* value, int defaultValue) {
    if (!value || !*value) return defaultValue;
    if (*value == '0' || strcasecmp(value, "false") == 0) return 0;
    return 1;
}

inline int getHintBoolean(const char* name, int defaultValue) {
    return stringBoolean(getHint(name), defaultValue);
}

inline void resetOneHint(const std::string& name, Hint& h, const char* env,
                         std::vector<std::pair<std::vector<HintWatch>, std::pair<std::string, std::pair<bool, std::string>>>>& calls) {
    const bool differs = (!env && h.hasValue) || (env && !h.hasValue) || (env && h.hasValue && h.value != env);
    if (differs)
        calls.push_back({ h.watches, { name, { h.hasValue, h.value } } });
    h.hasValue = false;
    h.value.clear();
    h.priority = kHintDefault;
}

inline int resetHint(const char* name) {
    if (!name) return 0;
    const char* envRaw = envLookup(name);
    const bool hasEnv = envRaw != nullptr;
    const std::string env = hasEnv ? envRaw : "";
    HintTable& t = hintTable();
    std::vector<std::pair<std::vector<HintWatch>, std::pair<std::string, std::pair<bool, std::string>>>> calls;
    {
        std::lock_guard<std::recursive_mutex> lk(t.mx);
        auto it = t.hints.find(name);
        if (it == t.hints.end()) return 0;
        resetOneHint(it->first, it->second, hasEnv ? env.c_str() : nullptr, calls);
    }
    for (auto& c : calls)
        hintNotify(c.first, c.second.first, c.second.second.first ? c.second.second.second.c_str() : nullptr,
                   hasEnv ? env.c_str() : nullptr);
    return 1;
}

inline void resetHints() {
    HintTable& t = hintTable();
    std::vector<std::string> names;
    {
        std::lock_guard<std::recursive_mutex> lk(t.mx);
        for (auto& kv : t.hints) names.push_back(kv.first);
    }
    for (const std::string& n : names) resetHint(n.c_str());
}

inline void clearHints() {
    HintTable& t = hintTable();
    std::lock_guard<std::recursive_mutex> lk(t.mx);
    t.hints.clear();
}

inline void delHintCallback(const char* name, HintCallback callback, void* userdata) {
    if (!name) return;
    HintTable& t = hintTable();
    std::lock_guard<std::recursive_mutex> lk(t.mx);
    auto it = t.hints.find(name);
    if (it == t.hints.end()) return;
    auto& w = it->second.watches;
    for (auto e = w.begin(); e != w.end(); ++e) {
        if (e->callback == callback && e->userdata == userdata) { w.erase(e); break; }
    }
}

inline void addHintCallback(const char* name, HintCallback callback, void* userdata) {
    if (!name || !*name) { invalidParam("name"); return; }
    if (!callback) { invalidParam("callback"); return; }
    delHintCallback(name, callback, userdata);
    {
        HintTable& t = hintTable();
        std::lock_guard<std::recursive_mutex> lk(t.mx);
        Hint& h = t.hints[name];   // a new entry has no value and the default priority
        h.watches.insert(h.watches.begin(), HintWatch{ callback, userdata });
    }
    // The new watch hears the current value once.
    const char* current = getHint(name);
    const bool has = current != nullptr;
    const std::string copy = has ? current : "";
    callback(userdata, name, has ? copy.c_str() : nullptr, has ? copy.c_str() : nullptr);
}

// ---------------------------------------------------------------------------
// Log (SDL_log.h).
// ---------------------------------------------------------------------------

enum LogCategory : int {
    kLogApplication = 0, kLogError = 1, kLogAssert = 2, kLogSystem = 3, kLogAudio = 4,
    kLogVideo = 5, kLogRender = 6, kLogInput = 7, kLogTest = 8, kLogCustom = 19
};
enum LogPriority : int {
    kLogVerbose = 1, kLogDebug = 2, kLogInfo = 3, kLogWarn = 4, kLogErr = 5, kLogCritical = 6, kLogNumPriorities = 7
};
typedef void (*LogOutputFunction)(void* userdata, int category, int priority, const char* message);

struct LogState {
    std::mutex mx;
    std::map<int, int> priorities;     // categories set by SDL_LogSetPriority
    int defaultPriority = kLogErr;     // the default of SDL 2.32; SDL 2.26 used critical
    int assertPriority = kLogWarn;
    int applicationPriority = kLogInfo;
    int testPriority = kLogVerbose;
    LogOutputFunction output = nullptr;   // null: the default output
    void* userdata = nullptr;
};

inline LogState& logState() {
    static LogState* state = new LogState;
    return *state;
}

inline void logDefaultOutput(void*, int category, int priority, const char* message) {
    static const char* const prefixes[kLogNumPriorities] = { nullptr, "VERBOSE", "DEBUG", "INFO", "WARN", "ERROR", "CRITICAL" };
    (void)category;
    fprintf(stderr, "%s: %s\n", (priority > 0 && priority < kLogNumPriorities) ? prefixes[priority] : "?", message);
}

inline int logGetPriority(int category) {
    LogState& s = logState();
    std::lock_guard<std::mutex> lk(s.mx);
    auto it = s.priorities.find(category);
    if (it != s.priorities.end()) return it->second;
    switch (category) {
    case kLogApplication: return s.applicationPriority;
    case kLogAssert: return s.assertPriority;
    case kLogTest: return s.testPriority;
    default: return s.defaultPriority;
    }
}

inline void logSetPriority(int category, int priority) {
    LogState& s = logState();
    std::lock_guard<std::mutex> lk(s.mx);
    s.priorities[category] = priority;
}

inline void logSetAllPriority(int priority) {
    LogState& s = logState();
    std::lock_guard<std::mutex> lk(s.mx);
    s.priorities.clear();
    s.defaultPriority = s.assertPriority = s.applicationPriority = s.testPriority = priority;
}

inline void logResetPriorities() {
    LogState& s = logState();
    std::lock_guard<std::mutex> lk(s.mx);
    s.priorities.clear();
    s.defaultPriority = kLogErr;
    s.assertPriority = kLogWarn;
    s.applicationPriority = kLogInfo;
    s.testPriority = kLogVerbose;
}

inline void logMessageV(int category, int priority, const char* fmt, va_list ap) {
    if (priority <= 0 || priority >= kLogNumPriorities) return;
    if (priority < logGetPriority(category)) return;
    char buf[4096];
    vsnprintf(buf, sizeof(buf), fmt ? fmt : "", ap);
    size_t len = strlen(buf);
    // One trailing line end is cut, as SDL does.
    if (len > 0 && buf[len - 1] == '\n') {
        buf[--len] = '\0';
        if (len > 0 && buf[len - 1] == '\r') buf[--len] = '\0';
    }
    LogOutputFunction out;
    void* userdata;
    {
        LogState& s = logState();
        std::lock_guard<std::mutex> lk(s.mx);
        out = s.output ? s.output : logDefaultOutput;
        userdata = s.userdata;
    }
    // The output function can be a Smalltalk callback; no lock is held here.
    out(userdata, category, priority, buf);
}

inline void logMessage(int category, int priority, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    logMessageV(category, priority, fmt, ap);
    va_end(ap);
}

inline void log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    logMessageV(kLogApplication, kLogInfo, fmt, ap);
    va_end(ap);
}

#define PHARO_SDL_LOG_LEVEL(fn, level) \
    inline void fn(int category, const char* fmt, ...) { \
        va_list ap; va_start(ap, fmt); logMessageV(category, level, fmt, ap); va_end(ap); }
PHARO_SDL_LOG_LEVEL(logVerbose, kLogVerbose)
PHARO_SDL_LOG_LEVEL(logDebug, kLogDebug)
PHARO_SDL_LOG_LEVEL(logInfo, kLogInfo)
PHARO_SDL_LOG_LEVEL(logWarn, kLogWarn)
PHARO_SDL_LOG_LEVEL(logError, kLogErr)
PHARO_SDL_LOG_LEVEL(logCritical, kLogCritical)
#undef PHARO_SDL_LOG_LEVEL

inline void logGetOutputFunction(LogOutputFunction* callback, void** userdata) {
    LogState& s = logState();
    std::lock_guard<std::mutex> lk(s.mx);
    if (callback) *callback = s.output ? s.output : logDefaultOutput;
    if (userdata) *userdata = s.userdata;
}

inline void logSetOutputFunction(LogOutputFunction callback, void* userdata) {
    LogState& s = logState();
    std::lock_guard<std::mutex> lk(s.mx);
    s.output = callback;
    s.userdata = userdata;
}

// ---------------------------------------------------------------------------
// Assertions (SDL_assert.h).
// ---------------------------------------------------------------------------

enum AssertState : int { kAssertRetry = 0, kAssertBreak = 1, kAssertAbort = 2, kAssertIgnore = 3, kAssertAlwaysIgnore = 4 };

struct AssertData {                     // SDL_AssertData
    int always_ignore;
    unsigned int trigger_count;
    const char* condition;
    const char* filename;
    int linenum;
    const char* function;
    const AssertData* next;
};

typedef int (*AssertionHandler)(const AssertData* data, void* userdata);

struct AssertState_ {
    std::mutex mx;
    const AssertData* triggered = nullptr;
    AssertionHandler handler = nullptr;   // null: the default handler
    void* userdata = nullptr;
    std::string report;
};

inline AssertState_& assertState() {
    static AssertState_* state = new AssertState_;
    return *state;
}

// The default handler cannot ask a person at a prompt.  It logs the failure
// and answers SDL_ASSERTION_IGNORE: the VM must not abort on a failed
// assertion of a library that the image loaded.
inline int defaultAssertionHandler(const AssertData* data, void*) {
    logMessage(kLogAssert, kLogWarn, "Assertion failure at %s (%s:%d), triggered %u time%s:\n  '%s'",
               data->function, data->filename, data->linenum, data->trigger_count,
               data->trigger_count == 1 ? "" : "s", data->condition);
    return kAssertIgnore;
}

inline int reportAssertion(AssertData* data, const char* function, const char* file, int line) {
    AssertState_& s = assertState();
    AssertionHandler handler;
    void* userdata;
    {
        std::lock_guard<std::mutex> lk(s.mx);
        if (!data->trigger_count) {
            data->function = function;
            data->filename = file;
            data->linenum = line;
            data->next = s.triggered;
            s.triggered = data;
        }
        ++data->trigger_count;
        if (data->always_ignore) return kAssertIgnore;
        handler = s.handler ? s.handler : defaultAssertionHandler;
        userdata = s.userdata;
    }
    const int state = handler(data, userdata);
    if (state == kAssertAlwaysIgnore) {
        data->always_ignore = 1;
        return kAssertIgnore;
    }
    return state;
}

inline void setAssertionHandler(AssertionHandler handler, void* userdata) {
    AssertState_& s = assertState();
    std::lock_guard<std::mutex> lk(s.mx);
    s.handler = handler;
    s.userdata = userdata;
}

inline AssertionHandler getDefaultAssertionHandler() { return defaultAssertionHandler; }

inline AssertionHandler getAssertionHandler(void** userdata) {
    AssertState_& s = assertState();
    std::lock_guard<std::mutex> lk(s.mx);
    if (userdata) *userdata = s.userdata;
    return s.handler ? s.handler : defaultAssertionHandler;
}

inline const AssertData* getAssertionReport() {
    AssertState_& s = assertState();
    std::lock_guard<std::mutex> lk(s.mx);
    return s.triggered;
}

inline void resetAssertionReport() {
    AssertState_& s = assertState();
    std::lock_guard<std::mutex> lk(s.mx);
    for (const AssertData* d = s.triggered; d;) {
        AssertData* m = const_cast<AssertData*>(d);
        d = d->next;
        m->always_ignore = 0;
        m->trigger_count = 0;
        m->next = nullptr;
    }
    s.triggered = nullptr;
}

// ---------------------------------------------------------------------------
// Platform, CPU, power, locale, version.
// ---------------------------------------------------------------------------

inline const char* platformName() {
#if defined(__APPLE__) && TARGET_OS_IPHONE && !TARGET_OS_MACCATALYST
    return "iOS";
#elif defined(__APPLE__)
    return "Mac OS X";
#elif defined(_WIN32)
    return "Windows";
#elif defined(__linux__)
    return "Linux";
#else
    return "Unknown (see SDL_platform.h)";
#endif
}

#if defined(__APPLE__)
inline int64_t sysctlInt(const char* name, int64_t fallback) {
    int64_t value = 0;
    size_t len = sizeof(value);
    if (sysctlbyname(name, &value, &len, nullptr, 0) != 0) return fallback;
    if (len == sizeof(int32_t)) return *reinterpret_cast<int32_t*>(&value);
    return value;
}
#endif

inline int cpuCount() {
#if defined(__APPLE__)
    const int64_t n = sysctlInt("hw.logicalcpu", 1);
    return n < 1 ? 1 : static_cast<int>(n);
#else
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n < 1 ? 1 : static_cast<int>(n);
#endif
}

inline int cpuCacheLineSize() {
#if defined(__APPLE__)
    const int64_t n = sysctlInt("hw.cachelinesize", 128);
    return n < 1 ? 128 : static_cast<int>(n);
#else
    return 128;   // the value SDL answers when it cannot ask the system
#endif
}

inline int systemRAM() {
#if defined(__APPLE__)
    return static_cast<int>(sysctlInt("hw.memsize", 0) / (1024 * 1024));
#else
    const long pages = sysconf(_SC_PHYS_PAGES), size = sysconf(_SC_PAGESIZE);
    return pages > 0 && size > 0 ? static_cast<int>((static_cast<int64_t>(pages) * size) / (1024 * 1024)) : 0;
#endif
}

#if defined(__x86_64__) || defined(__i386__)
#define PHARO_CPU_FEATURE(feature) (__builtin_cpu_supports(feature) ? 1 : 0)
#else
#define PHARO_CPU_FEATURE(feature) 0
#endif
inline int hasMMX() { return PHARO_CPU_FEATURE("mmx"); }
inline int hasSSE() { return PHARO_CPU_FEATURE("sse"); }
inline int hasSSE2() { return PHARO_CPU_FEATURE("sse2"); }
inline int hasSSE3() { return PHARO_CPU_FEATURE("sse3"); }
inline int hasSSE41() { return PHARO_CPU_FEATURE("sse4.1"); }
inline int hasSSE42() { return PHARO_CPU_FEATURE("sse4.2"); }
inline int hasAVX() { return PHARO_CPU_FEATURE("avx"); }
inline int hasAVX2() { return PHARO_CPU_FEATURE("avx2"); }
inline int hasAVX512F() { return PHARO_CPU_FEATURE("avx512f"); }
inline int hasRDTSC() {
#if defined(__x86_64__) || defined(__i386__)
    return 1;   // every x86 processor that runs this code has the time stamp counter
#else
    return 0;
#endif
}
inline int hasNEON() {
#if defined(__aarch64__) || defined(__ARM_NEON)
    return 1;
#else
    return 0;
#endif
}
inline int hasNoFeature() { return 0; }   // 3DNow, AltiVec, ARM SIMD (ARMv6), LSX, LASX

// SDL_SIMDGetAlignment: the strictest alignment that a SIMD load needs here.
inline size_t simdAlignment() {
    if (hasAVX512F()) return 64;
    if (hasAVX()) return 32;
    return 16;
}

// SDL_SIMDAlloc: the block starts at the alignment; the word before it holds
// the pointer that malloc answered.
inline void* simdAlloc(size_t len) {
    const size_t alignment = simdAlignment();
    if (len + alignment + sizeof(void*) < len) { setError("Out of memory"); return nullptr; }
    void* raw = malloc(len + alignment + sizeof(void*));
    if (!raw) { setError("Out of memory"); return nullptr; }
    uintptr_t aligned = (reinterpret_cast<uintptr_t>(raw) + sizeof(void*) + alignment - 1) & ~(alignment - 1);
    reinterpret_cast<void**>(aligned)[-1] = raw;
    return reinterpret_cast<void*>(aligned);
}

inline void simdFree(void* p) {
    if (p) free(reinterpret_cast<void**>(p)[-1]);
}

inline void* simdRealloc(void* mem, size_t len) {
    if (!mem) return simdAlloc(len);
    const size_t alignment = simdAlignment();
    void* raw = reinterpret_cast<void**>(mem)[-1];
    const size_t oldOffset = static_cast<size_t>(reinterpret_cast<uintptr_t>(mem) - reinterpret_cast<uintptr_t>(raw));
    void* fresh = realloc(raw, len + alignment + sizeof(void*));
    if (!fresh) { setError("Out of memory"); return nullptr; }
    uintptr_t aligned = (reinterpret_cast<uintptr_t>(fresh) + sizeof(void*) + alignment - 1) & ~(alignment - 1);
    const size_t newOffset = static_cast<size_t>(aligned - reinterpret_cast<uintptr_t>(fresh));
    if (newOffset != oldOffset) memmove(reinterpret_cast<void*>(aligned), static_cast<char*>(fresh) + oldOffset, len);
    reinterpret_cast<void**>(aligned)[-1] = fresh;
    return reinterpret_cast<void*>(aligned);
}

// ---------------------------------------------------------------------------
// Shared objects (SDL_loadso.h).
// ---------------------------------------------------------------------------

inline void* loadObject(const char* name) {
    if (!name) { invalidParam("sofile"); return nullptr; }
    void* handle = dlopen(name, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char* why = dlerror();
        setError((std::string("Failed loading ") + name + ": " + (why ? why : "")).c_str());
    }
    return handle;
}

inline void* loadFunction(void* handle, const char* name) {
    void* fn = dlsym(handle, name ? name : "");
    if (!fn) {
        const char* why = dlerror();
        setError((std::string("Failed loading ") + (name ? name : "") + ": " + (why ? why : "")).c_str());
    }
    return fn;
}

inline void unloadObject(void* handle) {
    if (handle) dlclose(handle);
}

// ---------------------------------------------------------------------------
// File system paths (SDL_filesystem.h).  Both answer a malloc block that the
// caller frees with SDL_free, and a path that ends in a slash.
// ---------------------------------------------------------------------------

inline std::string homeDirectory() {
    if (const char* home = envLookup("HOME")) return home;
#if !defined(_WIN32)
    struct passwd pw, *result = nullptr;
    char buf[4096];
    if (getpwuid_r(getuid(), &pw, buf, sizeof(buf), &result) == 0 && result && result->pw_dir) return result->pw_dir;
#endif
    return "";
}

inline char* dupString(const std::string& s) {
    char* out = static_cast<char*>(malloc(s.size() + 1));
    if (out) memcpy(out, s.c_str(), s.size() + 1);
    return out;
}

inline char* getBasePath() {
    std::string exe;
#if defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(&buf[0], &size) == 0) exe = buf.c_str();
#else
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) exe.assign(buf, static_cast<size_t>(n));
#endif
    if (exe.empty()) { setError("Couldn't locate our .app bundle"); return nullptr; }
    const size_t slash = exe.find_last_of('/');
    std::string dir = slash == std::string::npos ? "" : exe.substr(0, slash + 1);
#if defined(__APPLE__)
    // Inside an application bundle SDL answers the resource directory.
    const std::string marker = ".app/Contents/MacOS/";
    const size_t at = dir.find(marker);
    if (at != std::string::npos && at + marker.size() == dir.size())
        dir = dir.substr(0, at) + ".app/Contents/Resources/";
#endif
    return dupString(dir);
}

inline char* getPrefPath(const char* org, const char* app) {
    if (!app) { invalidParam("app"); return nullptr; }
    if (!org) org = "";
    const std::string home = homeDirectory();
    if (home.empty()) { setError("Couldn't find the home directory"); return nullptr; }
#if defined(__APPLE__)
    std::string path = home + "/Library/Application Support/";
#else
    std::string path = home + "/.local/share/";
#endif
    if (*org) path += std::string(org) + "/";
    path += std::string(app) + "/";
    for (size_t i = 1; i < path.size(); i++) {
        if (path[i] == '/') {
            const std::string part = path.substr(0, i);
            mkdir(part.c_str(), 0700);
        }
    }
    if (mkdir(path.substr(0, path.size() - 1).c_str(), 0700) != 0 && errno != EEXIST) {
        setError((std::string("Couldn't create directory '") + path + "': " + strerror(errno)).c_str());
        return nullptr;
    }
    return dupString(path);
}

// ---------------------------------------------------------------------------
// GUID text (SDL_guid.h).
// ---------------------------------------------------------------------------

struct Guid { uint8_t data[16]; };

inline void guidToString(Guid guid, char* out, int size) {
    static const char hex[] = "0123456789abcdef";
    if (!out || size <= 0) return;
    for (int i = 0; i < 16 && i < (size - 1) / 2; i++) {
        *out++ = hex[guid.data[i] >> 4];
        *out++ = hex[guid.data[i] & 15];
    }
    *out = '\0';
}

inline Guid guidFromString(const char* text) {
    auto nibble = [](unsigned char c) -> uint8_t {
        if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
        if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
        if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
        return 0;   // SDL answers 0 for a character that is not a hex digit
    };
    Guid g;
    memset(&g, 0, sizeof(g));
    size_t len = strlen(text ? text : "") & ~static_cast<size_t>(1);
    for (size_t i = 0, o = 0; i < len && o < 16; i += 2, o++)
        g.data[o] = static_cast<uint8_t>((nibble(static_cast<unsigned char>(text[i])) << 4) | nibble(static_cast<unsigned char>(text[i + 1])));
    return g;
}

// ---------------------------------------------------------------------------
// Text helpers of SDL_stdinc.h that the C library does not have.
// ---------------------------------------------------------------------------

// SDL_itoa family: upper case digits in `radix` (2..36); a negative value of
// any radix gets a minus sign.  A radix outside the range answers an empty
// string (SDL itself loops without end there).
inline char* unsignedToText(unsigned long long value, char* out, int radix) {
    static const char kDigits[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    if (radix < 2 || radix > 36) { out[0] = '\0'; return out; }
    char* p = out;
    do {
        *p++ = kDigits[value % static_cast<unsigned>(radix)];
        value /= static_cast<unsigned>(radix);
    } while (value);
    *p = '\0';
    for (char *a = out, *b = p - 1; a < b; ++a, --b) std::swap(*a, *b);
    return out;
}

inline char* signedToText(long long value, char* out, int radix) {
    char* p = out;
    unsigned long long magnitude = static_cast<unsigned long long>(value);
    if (value < 0) { *p++ = '-'; magnitude = 0ull - magnitude; }
    unsignedToText(magnitude, p, radix);
    return out;
}

inline char* itoa_(int v, char* s, int r) { return signedToText(v, s, r); }
inline char* uitoa_(unsigned v, char* s, int r) { return unsignedToText(v, s, r); }
inline char* ltoa_(long v, char* s, int r) { return signedToText(v, s, r); }
inline char* ultoa_(unsigned long v, char* s, int r) { return unsignedToText(v, s, r); }
inline char* lltoa_(long long v, char* s, int r) { return signedToText(v, s, r); }
inline char* ulltoa_(unsigned long long v, char* s, int r) { return unsignedToText(v, s, r); }

inline char* strRev(char* s) {
    if (!s) return s;
    for (char *a = s, *b = s + strlen(s); a < --b; ++a) std::swap(*a, *b);
    return s;
}
inline char* strUpr(char* s) { for (char* p = s; p && *p; ++p) *p = static_cast<char>(toupper(static_cast<unsigned char>(*p))); return s; }
inline char* strLwr(char* s) { for (char* p = s; p && *p; ++p) *p = static_cast<char>(tolower(static_cast<unsigned char>(*p))); return s; }

inline char* strTokR(char* s1, const char* s2, char** saveptr) {
    if (!saveptr || !s2) return nullptr;
    char* p = s1 ? s1 : *saveptr;
    if (!p) return nullptr;
    p += strspn(p, s2);
    if (!*p) { *saveptr = p; return nullptr; }
    char* end = p + strcspn(p, s2);
    if (*end) { *end = '\0'; *saveptr = end + 1; } else { *saveptr = end; }
    return p;
}

inline size_t strLCat(char* dst, const char* src, size_t maxlen) {
    const size_t dstlen = strnlen(dst, maxlen), srclen = strlen(src);
    if (dstlen == maxlen) return maxlen + srclen;   // no terminator in dst: nothing is copied
    const size_t room = maxlen - dstlen - 1;
    const size_t n = srclen < room ? srclen : room;
    memcpy(dst + dstlen, src, n);
    dst[dstlen + n] = '\0';
    return dstlen + srclen;
}

inline size_t strLCpy(char* dst, const char* src, size_t maxlen) {
    const size_t srclen = strlen(src);
    if (maxlen > 0) {
        const size_t n = srclen < maxlen - 1 ? srclen : maxlen - 1;
        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return srclen;
}

inline const char* strCaseStr(const char* hay, const char* needle) {
    const size_t n = strlen(needle);
    if (n == 0) return hay;
    for (; *hay; ++hay)
        if (strncasecmp(hay, needle, n) == 0) return hay;
    return nullptr;
}

// UTF-8: lead byte checks and the bounded copy that never cuts a character.
inline size_t utf8Len(const char* s) {
    size_t n = 0;
    for (; s && *s; ++s) if (!utf::isContinuation(static_cast<unsigned char>(*s))) ++n;
    return n;
}
inline size_t utf8NLen(const char* s, size_t bytes) {
    size_t n = 0;
    for (size_t i = 0; s && i < bytes && s[i]; ++i) if (!utf::isContinuation(static_cast<unsigned char>(s[i]))) ++n;
    return n;
}
inline size_t utf8LCpy(char* dst, const char* src, size_t dstBytes) {
    const size_t srclen = strlen(src);
    if (dstBytes > 0) {
        size_t n = srclen < dstBytes - 1 ? srclen : dstBytes - 1;
        // Back up to a character boundary when the cut falls inside a sequence.
        while (n > 0 && n < srclen && utf::isContinuation(static_cast<unsigned char>(src[n]))) --n;
        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return srclen;
}

// SDL_crc16 and SDL_crc32 are the CRC variants that SDL fixed for its joystick
// GUIDs ("DO NOT CHANGE" in SDL): a table value per byte, mixed with the
// running value shifted right by 8.  They are not the textbook CRCs.
constexpr uint16_t kCrc16Polynomial = 0xA001;           // reflected CRC-16
constexpr uint32_t kCrc32Polynomial = 0xEDB88320u;      // reflected CRC-32
constexpr uint32_t kCrc32FinalMask = 0xFF000000u;       // the xor that SDL adds per byte

inline uint16_t crc16(uint16_t crc, const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; i++) {
        uint16_t r16 = 0;
        uint8_t r = static_cast<uint8_t>(crc) ^ p[i];
        for (int b = 0; b < 8; b++) {
            r16 = static_cast<uint16_t>((((r16 ^ r) & 1) ? kCrc16Polynomial : 0) ^ (r16 >> 1));
            r >>= 1;
        }
        crc = static_cast<uint16_t>(r16 ^ (crc >> 8));
    }
    return crc;
}

inline uint32_t crc32(uint32_t crc, const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; i++) {
        uint32_t r = static_cast<uint8_t>(crc) ^ p[i];
        for (int b = 0; b < 8; b++) r = ((r & 1) ? 0 : kCrc32Polynomial) ^ (r >> 1);
        crc = (r ^ kCrc32FinalMask) ^ (crc >> 8);
    }
    return crc;
}

} // namespace base
} // namespace sdl2
} // namespace pharo
