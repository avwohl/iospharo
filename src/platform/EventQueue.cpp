/*
 * EventQueue.cpp - Event queue implementation
 */

#include "EventQueue.hpp"
#include <iostream>

namespace pharo {

EventQueue gEventQueue;

void EventQueue::push(const Event& event) {
    // Log mouse events for debugging
    // Mouse event: type=1, arg1=x, arg2=y, arg3=buttons, arg4=modifiers, arg5=subtype(0=move,1=down,2=up)
    if (event.type == static_cast<int>(EventType::Mouse)) {
        const char* typeStr = "?";
        switch (event.arg5) {  // arg5 is the mouse event subtype
            case 0: typeStr = "move"; break;
            case 1: typeStr = "down"; break;
            case 2: typeStr = "up"; break;
        }
        std::cerr << "[EVENT] Mouse " << typeStr
                  << " at (" << event.arg1 << "," << event.arg2 << ")"
                  << " buttons=" << event.arg3
                  << " mods=" << event.arg4 << "\n";
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
