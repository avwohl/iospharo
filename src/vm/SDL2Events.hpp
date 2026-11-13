/*
 * SDL2Events.hpp - the SDL event queue, event filter and event watches.
 *
 * Follows SDL_events.h: SDL_PushEvent runs the filter, then every watch,
 * then adds the event to the queue.  SDL_PeepEvents, SDL_HasEvents,
 * SDL_FlushEvents, SDL_SetEventFilter, SDL_FilterEvents, SDL_RegisterEvents
 * and SDL_EventState act on the same queue.
 *
 * Threads.  The host UI thread produces the real events (mouse, keyboard,
 * window).  It does not call SDL.  It puts them in the platform event queue
 * (pharo::gEventQueue), and the VM thread converts them to SDL events inside
 * SDL_PollEvent.  A watch or filter is therefore called on the VM thread, at
 * the moment an event enters the SDL queue.  Real SDL calls a watch on the
 * thread that pushes the event; here that is always the VM thread, for two
 * reasons.  First, a watch is usually a Smalltalk callback, and the VM
 * enters Smalltalk callbacks from the VM thread without a queue hop.  A call
 * from the UI thread would block the UI thread until the image ran, and the
 * image can wait for the UI thread.  Second, no event is lost or reordered:
 * the VM thread sees every event in the order of the platform queue.
 * SDL_PushEvent from a native thread (a thread made by SDL_CreateThread that
 * does not run Smalltalk) runs the filter and the watches on that thread, as
 * in SDL; a Smalltalk callback then crosses to the VM thread by the callback
 * queue of the VM.
 *
 * No lock is held while a callback runs, except a recursive lock that only
 * guards the filter and watch tables for the thread that dispatches.  The
 * callbacks get a pointer to a private copy of the event, never to memory of
 * the Smalltalk heap, so a garbage collection inside a callback cannot move
 * the event under the callback.
 *
 * Copyright (c) 2025-2026 Aaron Wohl. Licensed under the MIT License.
 */
#pragma once

#include "SDL2Sys.hpp"

#include <algorithm>
#include <deque>
#include <set>

namespace pharo {
namespace sdl2 {
namespace ev {

constexpr size_t kMaxQueuedEvents = 65535;   // SDL_MAX_QUEUED_EVENTS
constexpr uint32_t kFirstEvent = 0;
constexpr uint32_t kLastEvent = 0xFFFF;
constexpr uint32_t kUserEvent = 0x8000;
constexpr uint32_t kTextInputEvent = 0x303;
constexpr uint32_t kTextEditingEvent = 0x302;
constexpr uint32_t kSysWmEvent = 0x201;

enum EventAction : int { kAdd = 0, kPeek = 1, kGet = 2 };
enum EventStateValue : int { kQuery = -1, kDisable = 0, kEnable = 1 };

struct Event {                // SDL_Event is a 56 byte union; type and timestamp lead
    uint32_t type;
    uint32_t timestamp;
    uint8_t rest[48];
};
static_assert(sizeof(Event) == 56, "SDL_Event");

typedef int (*EventFilter)(void* userdata, Event* event);

struct Watch { EventFilter filter; void* userdata; unsigned id; };

struct State {
    std::recursive_mutex mx;
    std::deque<Event> queue;
    EventFilter filter = nullptr;
    void* filterData = nullptr;
    std::vector<Watch> watches;
    unsigned nextWatchId = 1;
    uint32_t nextUserEvent = kUserEvent;
    std::set<uint32_t> disabled;       // types turned off with SDL_EventState
    std::set<uint32_t> explicitlyDisabled;   // the subset that SDL_EventState(DISABLE) set, not the SDL start-up defaults
    State() {
        // SDL starts with these three types switched off (SDL_StartTextInput turns the first two on).
        disabled.insert(kTextInputEvent);
        disabled.insert(kTextEditingEvent);
        disabled.insert(kSysWmEvent);
    }
};

inline State& state() {
    static State* s = new State;   // never freed: a timer or thread can push during exit
    return *s;
}

inline void copyOut(Event* dst, const Event& src) { memcpy(dst, &src, sizeof(Event)); }

// ----- the queue -----

inline int queueAdd(const Event& e) {
    State& s = state();
    std::lock_guard<std::recursive_mutex> lk(s.mx);
    if (s.queue.size() >= kMaxQueuedEvents) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Event queue is full (%d events)", static_cast<int>(s.queue.size()));
        sys::setError(buf);
        return 0;
    }
    s.queue.push_back(e);
    return 1;
}

inline bool popFront(Event& out) {
    State& s = state();
    std::lock_guard<std::recursive_mutex> lk(s.mx);
    if (s.queue.empty()) return false;
    out = s.queue.front();
    s.queue.pop_front();
    return true;
}

inline bool queueNotEmpty() {
    State& s = state();
    std::lock_guard<std::recursive_mutex> lk(s.mx);
    return !s.queue.empty();
}

// SDL_PeepEvents.  Add answers the number added.  Peek and get answer the
// number of events in [minType, maxType], at most `numevents` when a buffer is
// given; get also removes them.  With no buffer, peek and get count them all.
inline int peepEvents(Event* events, int numevents, int action, uint32_t minType, uint32_t maxType) {
    State& s = state();
    if (action == kAdd) {
        if (numevents <= 0) return 0;
        if (!events) return sys::invalidParam("events");
        int used = 0;
        for (int i = 0; i < numevents; i++) used += queueAdd(events[i]);
        return used;
    }
    if (action != kPeek && action != kGet) return 0;
    std::lock_guard<std::recursive_mutex> lk(s.mx);
    int used = 0;
    for (auto it = s.queue.begin(); it != s.queue.end() && (!events || used < numevents);) {
        if (it->type >= minType && it->type <= maxType) {
            if (events) copyOut(&events[used], *it);
            ++used;
            if (action == kGet) { it = s.queue.erase(it); continue; }
        }
        ++it;
    }
    return used;
}

inline int hasEvents(uint32_t minType, uint32_t maxType) {
    return peepEvents(nullptr, 0, kPeek, minType, maxType) > 0 ? 1 : 0;
}

inline void flushEvents(uint32_t minType, uint32_t maxType) {
    State& s = state();
    std::lock_guard<std::recursive_mutex> lk(s.mx);
    for (auto it = s.queue.begin(); it != s.queue.end();) {
        if (it->type >= minType && it->type <= maxType) it = s.queue.erase(it);
        else ++it;
    }
}

// ----- filter and watches -----

inline void setEventFilter(EventFilter filter, void* userdata) {
    State& s = state();
    std::lock_guard<std::recursive_mutex> lk(s.mx);
    s.filter = filter;
    s.filterData = userdata;
    flushEvents(kFirstEvent, kLastEvent);   // SDL discards the pending events when the filter changes
}

inline int getEventFilter(EventFilter* filter, void** userdata) {
    State& s = state();
    std::lock_guard<std::recursive_mutex> lk(s.mx);
    if (filter) *filter = s.filter;
    if (userdata) *userdata = s.filterData;
    return s.filter ? 1 : 0;
}

// SDL_FilterEvents: the filter looks at each queued event; an answer of 0 removes it.
inline void filterEvents(EventFilter filter, void* userdata) {
    State& s = state();
    std::vector<Event> snapshot;
    {
        std::lock_guard<std::recursive_mutex> lk(s.mx);
        snapshot.assign(s.queue.begin(), s.queue.end());
        s.queue.clear();
    }
    // The filter can be a Smalltalk callback, so it runs without the lock and on a copy.
    std::vector<Event> kept;
    for (Event& e : snapshot)
        if (filter(userdata, &e)) kept.push_back(e);
    std::lock_guard<std::recursive_mutex> lk(s.mx);
    // Events pushed while the filter ran stay behind the kept ones.
    s.queue.insert(s.queue.begin(), kept.begin(), kept.end());
}

inline void addEventWatch(EventFilter filter, void* userdata) {
    State& s = state();
    std::lock_guard<std::recursive_mutex> lk(s.mx);
    s.watches.push_back(Watch{ filter, userdata, s.nextWatchId++ });
}

// Removal is by the pair (function, userdata); the first match goes.
inline void delEventWatch(EventFilter filter, void* userdata) {
    State& s = state();
    std::lock_guard<std::recursive_mutex> lk(s.mx);
    for (auto it = s.watches.begin(); it != s.watches.end(); ++it) {
        if (it->filter == filter && it->userdata == userdata) { s.watches.erase(it); return; }
    }
}

// Run the filter, then the watches, on `e`.  False when the filter drops the event.
// A watch that is removed while the dispatch runs is not called any more.
inline bool filterAndWatch(Event& e) {
    State& s = state();
    EventFilter filter;
    void* filterData;
    std::vector<Watch> watches;
    {
        std::lock_guard<std::recursive_mutex> lk(s.mx);
        filter = s.filter;
        filterData = s.filterData;
        watches = s.watches;
    }
    if (filter && !filter(filterData, &e)) return false;
    for (const Watch& w : watches) {
        {
            std::lock_guard<std::recursive_mutex> lk(s.mx);
            bool still = false;
            for (const Watch& cur : s.watches) if (cur.id == w.id) { still = true; break; }
            if (!still) continue;
        }
        w.filter(w.userdata, &e);
    }
    return true;
}

// An event that enters the queue: filter, watches, then add.  Answers 1 when
// added, 0 when the filter dropped it, -1 when the queue refused it.
inline int enqueue(Event& e) {
    if (!filterAndWatch(e)) return 0;
    return queueAdd(e) > 0 ? 1 : -1;
}

// SDL_PushEvent.  The timestamp of the caller's event is set first.
inline int pushEvent(Event* caller, uint32_t timestamp) {
    caller->timestamp = timestamp;
    Event local;
    copyOut(&local, *caller);
    return enqueue(local);
}

// SDL_RegisterEvents: the first of `count` fresh user event types, or
// (Uint32)-1 when the count is below one or the range of event types is used up.
inline uint32_t registerEvents(int count) {
    State& s = state();
    std::lock_guard<std::recursive_mutex> lk(s.mx);
    if (count > 0 && s.nextUserEvent + static_cast<uint32_t>(count) <= kLastEvent) {
        const uint32_t first = s.nextUserEvent;
        s.nextUserEvent += static_cast<uint32_t>(count);
        return first;
    }
    return 0xFFFFFFFFu;
}

// SDL_EventState.  Answers the previous state; DISABLE also drops queued events of the type.
inline int eventState(uint32_t type, int newState) {
    State& s = state();
    std::lock_guard<std::recursive_mutex> lk(s.mx);
    const int current = s.disabled.count(type) ? kDisable : kEnable;
    if (newState != current && newState != kQuery) {
        if (newState == kDisable) {
            s.disabled.insert(type);
            s.explicitlyDisabled.insert(type);
            flushEvents(type, type);
        } else if (newState == kEnable) {
            s.disabled.erase(type);
            s.explicitlyDisabled.erase(type);
        }
    }
    return current;
}

// SDL_StartTextInput and SDL_StopTextInput switch the two text event types.
// Only the reported state changes: host text events still arrive after
// SDL_StopTextInput, because the host keyboard sends them without waiting for
// a matching SDL_StartTextInput (see hostEventDisabled).
inline void setTextEventsEnabled(bool enabled) {
    State& s = state();
    std::lock_guard<std::recursive_mutex> lk(s.mx);
    for (uint32_t type : { kTextInputEvent, kTextEditingEvent }) {
        if (enabled) s.disabled.erase(type); else s.disabled.insert(type);
    }
}

// True when a host event of this type must be dropped.  Only a type that the
// image switched off by SDL_EventState(DISABLE) is dropped: the SDL start-up
// defaults (text input, text editing and window manager events off) are not
// enforced for host events, because the host sends text events without a
// matching SDL_StartTextInput.
inline bool hostEventDisabled(uint32_t type) {
    State& s = state();
    std::lock_guard<std::recursive_mutex> lk(s.mx);
    return s.explicitlyDisabled.count(type) != 0;
}

} // namespace ev
} // namespace sdl2
} // namespace pharo
