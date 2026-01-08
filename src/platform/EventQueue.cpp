/*
 * EventQueue.cpp - Event queue implementation
 */

#include "EventQueue.hpp"

namespace pharo {

EventQueue gEventQueue;

void EventQueue::push(const Event& event) {
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
