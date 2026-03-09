/*
 * EventQueue.hpp - Event handling for Pharo VM
 */

#ifndef PHARO_EVENT_QUEUE_HPP
#define PHARO_EVENT_QUEUE_HPP

#include <cstdint>
#include <queue>
#include <mutex>

namespace pharo {

/// Event types matching Pharo's event encoding
enum class EventType : int {
    None = 0,
    Mouse = 1,
    Keyboard = 2,
    WindowMetrics = 6,
    MouseWheel = 7,
};

/// Mouse buttons
enum class MouseButton : int {
    None = 0,
    Red = 4,      // Left/primary
    Yellow = 2,   // Right/secondary
    Blue = 1,     // Middle
};

/// Keyboard modifiers
enum ModifierFlags {
    ShiftKey = 1,
    CtrlKey = 2,
    AltKey = 4,
    CmdKey = 8,
};

/// Event structure (matches Pharo's 8-word event buffer)
struct Event {
    int type = 0;
    int timeStamp = 0;
    int arg1 = 0;
    int arg2 = 0;
    int arg3 = 0;
    int arg4 = 0;
    int arg5 = 0;
    int windowIndex = 1;
};

/// Callback type for event notification
using EventCallback = void(*)(void* context);

/// Thread-safe event queue
class EventQueue {
public:
    void push(const Event& event);
    bool pop(Event& event);
    bool isEmpty() const;
    size_t size() const;
    void clear();
    void reset();  // Clear events + reset callbacks/indices for VM relaunch

    /// Set callback to be invoked when events are pushed
    void setEventCallback(EventCallback callback, void* context);

    /// Set input semaphore index (for VM signaling)
    void setInputSemaphoreIndex(int index);
    int getInputSemaphoreIndex() const;

    /// Set SDL2 input semaphore index (for SDL2-specific event signaling)
    void setSDL2InputSemaphoreIndex(int index);
    int getSDL2InputSemaphoreIndex() const;

    /// SDL2 event polling flag - when true, SDL_PollEvent handles events
    /// When false, processInputEvents drains to passThroughEvents_
    void setSDL2EventPollingActive(bool active);
    bool isSDL2EventPollingActive() const;

private:
    mutable std::mutex mutex_;
    std::queue<Event> events_;
    EventCallback callback_ = nullptr;
    void* callbackContext_ = nullptr;
    int inputSemaphoreIndex_ = 0;
    int sdl2InputSemaphoreIndex_ = 0;
    bool sdl2EventPollingActive_ = false;
};

/// Global event queue
extern EventQueue gEventQueue;

} // namespace pharo

#endif
