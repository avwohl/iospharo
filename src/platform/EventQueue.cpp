/*
 * EventQueue.cpp - Event queue implementation
 */

#include "EventQueue.hpp"
#include <iostream>

namespace pharo {

EventQueue gEventQueue;

void EventQueue::push(const Event& event) {
    EventCallback callbackToInvoke = nullptr;
    void* context = nullptr;
    size_t queueSize = 0;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        events_.push(event);
        queueSize = events_.size();

        // Capture callback outside lock to avoid holding lock during callback
        callbackToInvoke = callback_;
        context = callbackContext_;
    }

    // Log mouse events being pushed
    if (event.type == 1) {  // Mouse event
        std::cerr << "[QUEUE] PUSH mouse type=" << event.arg5 << " at " << event.arg1 << "," << event.arg2
                  << " queueSize=" << queueSize << "\n";
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

    // Log mouse events being popped
    if (event.type == 1) {  // Mouse event
        std::cerr << "[QUEUE] POP mouse type=" << event.arg5 << " at " << event.arg1 << "," << event.arg2
                  << " remaining=" << events_.size() << "\n";
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
    if (inputSemaphoreIndex_ != index) {
        std::cerr << "[QUEUE] Setting input semaphore index: " << inputSemaphoreIndex_ << " -> " << index << "\n";
    }
    inputSemaphoreIndex_ = index;
}

int EventQueue::getInputSemaphoreIndex() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return inputSemaphoreIndex_;
}

} // namespace pharo
