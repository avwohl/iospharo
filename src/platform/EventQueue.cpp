/*
 * EventQueue.cpp - Event queue implementation
 */

#include "EventQueue.hpp"
#include <iostream>

namespace pharo {

EventQueue gEventQueue;

void EventQueue::push(const Event& event) {
    // Debug: Log ALL events to dedicated file
    static FILE* allEventsLog = nullptr;
    static int allEventsCount = 0;
    allEventsCount++;

    if (!allEventsLog) {
        allEventsLog = fopen("/tmp/queue_all_events.log", "w");
    }
    if (allEventsLog && allEventsCount <= 500) {
        fprintf(allEventsLog, "[QUEUE-PUSH] #%d type=%d args=(%d,%d,%d,%d,%d) queue=%p\n",
                allEventsCount, event.type, event.arg1, event.arg2, event.arg3, event.arg4, event.arg5,
                (void*)this);
        fflush(allEventsLog);
    }

    EventCallback callbackToInvoke = nullptr;
    void* context = nullptr;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        events_.push(event);
        // Capture callback outside lock to avoid holding lock during callback
        callbackToInvoke = callback_;
        context = callbackContext_;
    }

    // Signal that an event is available (outside lock)
    // Debug: Log callback invocation
    static FILE* pushLog = fopen("/tmp/event_push.log", "a");
    static int pushCount = 0;
    pushCount++;
    if (pushLog && pushCount <= 50) {
        fprintf(pushLog, "[PUSH] #%d type=%d callback=%p\n",
                pushCount, event.type, (void*)callbackToInvoke);
        fflush(pushLog);
    }

    if (callbackToInvoke) {
        callbackToInvoke(context);
    }
}

bool EventQueue::pop(Event& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (events_.empty()) return false;
    event = events_.front();
    events_.pop();

    // Debug: Log all popped events
    static FILE* popLog = nullptr;
    static int popCount = 0;
    popCount++;

    if (!popLog) {
        popLog = fopen("/tmp/queue_pop_events.log", "w");
    }
    if (popLog && popCount <= 500) {
        fprintf(popLog, "[QUEUE-POP] #%d type=%d args=(%d,%d,%d,%d,%d)\n",
                popCount, event.type, event.arg1, event.arg2, event.arg3, event.arg4, event.arg5);
        fflush(popLog);
    }

    return true;
}

bool EventQueue::isEmpty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return events_.empty();
}

void EventQueue::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!events_.empty()) events_.pop();
}

void EventQueue::setEventCallback(EventCallback callback, void* context) {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = callback;
    callbackContext_ = context;
}

void EventQueue::setInputSemaphoreIndex(int index) {
    std::lock_guard<std::mutex> lock(mutex_);
    inputSemaphoreIndex_ = index;
}

int EventQueue::getInputSemaphoreIndex() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return inputSemaphoreIndex_;
}

} // namespace pharo
