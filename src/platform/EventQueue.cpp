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
        callbackToInvoke = callback_;
        context = callbackContext_;
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

size_t EventQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return events_.size();
}

void EventQueue::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!events_.empty()) events_.pop();
}

void EventQueue::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!events_.empty()) events_.pop();
    callback_ = nullptr;
    callbackContext_ = nullptr;
    inputSemaphoreIndex_ = 0;
    sdl2InputSemaphoreIndex_ = 0;
    sdl2EventPollingActive_ = false;
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

void EventQueue::setSDL2InputSemaphoreIndex(int index) {
    std::lock_guard<std::mutex> lock(mutex_);
    sdl2InputSemaphoreIndex_ = index;
}

int EventQueue::getSDL2InputSemaphoreIndex() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sdl2InputSemaphoreIndex_;
}

void EventQueue::setSDL2EventPollingActive(bool active) {
    std::lock_guard<std::mutex> lock(mutex_);
    sdl2EventPollingActive_ = active;
}

bool EventQueue::isSDL2EventPollingActive() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sdl2EventPollingActive_;
}

} // namespace pharo
