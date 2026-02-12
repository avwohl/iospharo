/*
 * PlatformBridge.cpp - C bridge implementation
 */

#include "PlatformBridge.h"
#include "DisplaySurface.hpp"
#include "EventQueue.hpp"
#include "../vm/ObjectMemory.hpp"
#include "../vm/ImageLoader.hpp"
#include "../vm/Interpreter.hpp"
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include <mutex>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace pharo {
    DisplaySurface* gDisplaySurface = nullptr;
}

// Double-buffered display surface with deferred resize
// Resize is queued and applied during buffer swap to prevent tearing
class SimpleDisplaySurface : public pharo::DisplaySurface {
public:
    SimpleDisplaySurface(int w, int h, int d)
        : width_(w), height_(h), depth_(d),
          frontWidth_(w), frontHeight_(h),
          pendingWidth_(0), pendingHeight_(0), pendingDepth_(0),
          pendingResize_(false) {
        backBuffer_.resize(w * h);
        frontBuffer_.resize(w * h);
    }

    int width() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return width_;
    }
    int height() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return height_;
    }
    int depth() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return depth_;
    }

    uint32_t* pixels() override {
        std::lock_guard<std::mutex> lock(mutex_);
        return backBuffer_.data();
    }

    uint32_t* frontPixels() {
        std::lock_guard<std::mutex> lock(mutex_);
        return frontBuffer_.data();
    }

    size_t pitch() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return width_ * 4;
    }

    // Returns front buffer info - this buffer remains valid until next update()
    void getBufferInfo(int& w, int& h, uint32_t*& pixels, size_t& size) {
        std::lock_guard<std::mutex> lock(mutex_);
        w = frontWidth_;
        h = frontHeight_;
        pixels = frontBuffer_.data();
        size = frontBuffer_.size();
    }

    bool isResizing() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pendingResize_;
    }

    void invalidateRect(int x, int y, int w, int h) override {
        DisplayUpdateFunc cb = nullptr;
        void* ctx = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cb = updateCallback_;
            ctx = context_;
        }
        if (cb) {
            cb(x, y, w, h, ctx);
        }
    }

    void update() override {
        int newWidth, newHeight;
        {
            std::lock_guard<std::mutex> lock(mutex_);

            // Apply pending resize to front buffer during swap
            if (pendingResize_) {
                frontBuffer_.resize(pendingWidth_ * pendingHeight_);
                frontWidth_ = pendingWidth_;
                frontHeight_ = pendingHeight_;
                pendingResize_ = false;
            }

            std::swap(backBuffer_, frontBuffer_);
            std::swap(width_, frontWidth_);
            std::swap(height_, frontHeight_);

            newWidth = width_;
            newHeight = height_;
        }
        invalidateRect(0, 0, newWidth, newHeight);
    }

    void setCallback(DisplayUpdateFunc cb, void* ctx) {
        std::lock_guard<std::mutex> lock(mutex_);
        updateCallback_ = cb;
        context_ = ctx;
    }

    // Queue resize - applied during next update() to prevent tearing
    void resize(int w, int h, int d) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (w == width_ && h == height_ && d == depth_ && !pendingResize_) return;

        // Resize back buffer immediately (VM renders here)
        width_ = w;
        height_ = h;
        depth_ = d;
        backBuffer_.resize(w * h);

        // Queue front buffer resize for next swap (Metal reads from front)
        pendingWidth_ = w;
        pendingHeight_ = h;
        pendingDepth_ = d;
        pendingResize_ = true;
    }

private:
    mutable std::mutex mutex_;

    // Back buffer dimensions (current rendering target)
    int width_, height_, depth_;
    std::vector<uint32_t> backBuffer_;

    // Front buffer dimensions (current display)
    int frontWidth_, frontHeight_;
    std::vector<uint32_t> frontBuffer_;

    // Pending resize (applied during swap)
    int pendingWidth_, pendingHeight_, pendingDepth_;
    bool pendingResize_;

    DisplayUpdateFunc updateCallback_ = nullptr;
    void* context_ = nullptr;
};

// Global state
static pharo::ObjectMemory* gMemory = nullptr;
static pharo::Interpreter* gInterpreter = nullptr;
static SimpleDisplaySurface* gDisplay = nullptr;
static std::thread gVMThread;
static std::atomic<bool> gRunning{false};

// Pending display callback (registered before display exists)
static DisplayUpdateFunc gPendingCallback = nullptr;
static void* gPendingCallbackContext = nullptr;

// Event callback to signal the input semaphore
static void eventCallback(void* context) {
    (void)context;  // Unused

    // Debug: Log callback invocations
    static FILE* callbackLog = fopen("/tmp/event_callback.log", "a");
    static int callbackCount = 0;
    callbackCount++;
    if (callbackLog && callbackCount <= 50) {
        fprintf(callbackLog, "[CALLBACK] #%d gInterpreter=%p\n",
                callbackCount, (void*)gInterpreter);
        fflush(callbackLog);
    }

    if (gInterpreter) {
        int semIndex = pharo::gEventQueue.getInputSemaphoreIndex();
        // If Pharo hasn't set a semaphore index, use 1 as fallback (common for input semaphore)
        if (semIndex <= 0) {
            semIndex = 1;
        }
        if (callbackLog && callbackCount <= 50) {
            fprintf(callbackLog, "[CALLBACK] #%d Signaling semaphore %d\n", callbackCount, semIndex);
            fflush(callbackLog);
        }
        gInterpreter->signalExternalSemaphore(semIndex);
    }
}

extern "C" {

bool vm_initialize(size_t heapSize) {
    if (gMemory) {
        return true;
    }

    gMemory = new pharo::ObjectMemory();
    pharo::MemoryConfig config;
    config.oldSpaceSize = heapSize;
    config.newSpaceSize = 32 * 1024 * 1024;
    config.permSpaceSize = 8 * 1024 * 1024;

    return gMemory->initialize(config);
}

bool vm_loadImage(const char* imagePath) {
    if (!gMemory) {
        return false;
    }

    pharo::ImageLoader loader;
    pharo::LoadResult result = loader.load(imagePath, *gMemory);

    if (!result.success) {
        return false;
    }

    gInterpreter = new pharo::Interpreter(*gMemory);
    gMemory->setInterpreter(gInterpreter);  // Critical: GC needs this to update interpreter roots
    gInterpreter->setImageName(imagePath);

    // In Pharo 13, the standard VM is always "headless" by default.
    // OSWorldRenderer >> isApplicableFor: checks:
    //   CommandLineArguments new hasOption: 'interactive'
    // which reads from Smalltalk arguments (attribute indices 3+).
    // So we pass --headless as VM param AND --interactive as image argument.
    gInterpreter->setVMParameters({"--headless"});
    gInterpreter->setImageArguments({"--interactive"});

    if (!gInterpreter->initialize()) {
        return false;
    }

    // Apply display size if already set (vm_setDisplaySize may be called before vm_loadImage)
    if (gDisplay) {
        gInterpreter->setScreenSize(gDisplay->width(), gDisplay->height());
        gInterpreter->setScreenDepth(gDisplay->depth());

        // Ensure Display Form exists and is bound to 'Display' global
        // This is critical for Morphic rendering
        if (gInterpreter->displayForm().isNil()) {
            gInterpreter->ensureDisplayForm(gDisplay->width(), gDisplay->height(), gDisplay->depth());
        }
    }

    // Register event callback to signal input semaphore when events arrive
    pharo::gEventQueue.setEventCallback(eventCallback, nullptr);

    return true;
}

void vm_run(void) {
    if (!gInterpreter || gRunning) return;

    // Start the heartbeat thread (handles timers, like official VM)
    gInterpreter->startHeartbeat();

    gRunning = true;
    gVMThread = std::thread([]() {
        // Post a window resize event to trigger Pharo layout
        // This tells Pharo the display size so it can lay out morphs properly
        if (gDisplay) {
            vm_postWindowEvent(gDisplay->width(), gDisplay->height());

            // Show a test pattern to verify display pipeline works
            // This proves native rendering works even before Pharo renders
            int w = gDisplay->width();
            int h = gDisplay->height();
            uint32_t* pixels = gDisplay->pixels();
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    // Purple gradient to distinguish from Pharo content
                    uint8_t r = static_cast<uint8_t>(100 + (x * 100 / w));
                    uint8_t g = static_cast<uint8_t>(50);
                    uint8_t b = static_cast<uint8_t>(100 + (y * 100 / h));
                    pixels[y * w + x] = (255 << 24) | (r << 16) | (g << 8) | b;
                }
            }
            gDisplay->update();
        }

        // Call interpret() which includes periodic event processing and semaphore handling
        gInterpreter->interpret();
        gRunning = false;
    });
}

void vm_runOnMainThread(void) {
    if (!gInterpreter || gRunning) return;

    // Start the heartbeat thread (handles timers, like official VM)
    gInterpreter->startHeartbeat();

    gRunning = true;

    // Post a window resize event to trigger Pharo layout
    if (gDisplay) {
        vm_postWindowEvent(gDisplay->width(), gDisplay->height());
    }

    // Set a relinquish callback that processes the native run loop.
    // When Pharo calls Processor yield or Delay wait, the VM calls
    // relinquishProcessor which invokes this callback. This lets
    // the UIKit run loop process events (display updates, touch events)
    // while the interpreter "sleeps".
    gInterpreter->setRelinquishCallback([](int microseconds) {
#ifdef __APPLE__
        // Process the main run loop for up to `microseconds` or 1ms minimum.
        // This allows Metal rendering, touch events, and other UIKit work to proceed.
        double seconds = std::max(microseconds, 1000) / 1000000.0;
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, true);
#else
        std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
#endif
    });

    fprintf(stderr, "[VM] Running interpreter on main thread\n");

    // Run interpreter on the current (main) thread
    // This blocks until the interpreter exits
    gInterpreter->interpret();
    gRunning = false;
}

void vm_stop(void) {
    gRunning = false;
    if (gVMThread.joinable()) {
        gVMThread.join();
    }
    // Stop the heartbeat thread
    if (gInterpreter) {
        gInterpreter->stopHeartbeat();
    }
}

bool vm_isRunning(void) {
    return gRunning;
}

void vm_setDisplaySize(int width, int height, int depth) {
    if (gDisplay) {
        // Resize existing display (thread-safe: doesn't delete the object)
        gDisplay->resize(width, height, depth);
    } else {
        // Create new display
        gDisplay = new SimpleDisplaySurface(width, height, depth);
        pharo::gDisplaySurface = gDisplay;

        // Apply pending callback if one was registered before display existed
        if (gPendingCallback) {
            gDisplay->setCallback(gPendingCallback, gPendingCallbackContext);
        }
    }

    // Also update the interpreter's screen size
    if (gInterpreter) {
        gInterpreter->setScreenSize(width, height);
        gInterpreter->setScreenDepth(depth);
    }
}

uint32_t* vm_getDisplayPixels(void) {
    // Return front buffer for display (Metal reads this)
    // Rendering writes to back buffer via gDisplay->pixels()
    return gDisplay ? gDisplay->frontPixels() : nullptr;
}

int vm_getDisplayWidth(void) {
    return gDisplay ? gDisplay->width() : 0;
}

int vm_getDisplayHeight(void) {
    return gDisplay ? gDisplay->height() : 0;
}

void vm_getDisplayBufferInfo(DisplayBufferInfo* info) {
    if (!info) return;

    if (gDisplay) {
        // Get all info atomically with a single lock
        int w, h;
        uint32_t* pixels;
        size_t size;
        gDisplay->getBufferInfo(w, h, pixels, size);
        info->pixels = pixels;
        info->width = w;
        info->height = h;
        info->size = size;
    } else {
        info->pixels = nullptr;
        info->width = 0;
        info->height = 0;
        info->size = 0;
    }
}

bool vm_copyDisplayBuffer(uint32_t* dest, size_t destSize, int* outWidth, int* outHeight) {
    // Not used anymore but kept for API compatibility
    if (outWidth) *outWidth = 0;
    if (outHeight) *outHeight = 0;
    return false;
}

bool vm_isDisplayResizing(void) {
    return gDisplay ? gDisplay->isResizing() : false;
}

void vm_setDisplayUpdateCallback(DisplayUpdateFunc callback, void* context) {
    // Always store the callback (in case display doesn't exist yet)
    gPendingCallback = callback;
    gPendingCallbackContext = context;

    // Apply immediately if display exists
    if (gDisplay) {
        gDisplay->setCallback(callback, context);
    }
}

void vm_postMouseEvent(int type, int x, int y, int buttons, int modifiers) {
    // Debug: Log all mouse events to file
    static FILE* mouseLog = nullptr;
    static int mouseEventCount = 0;
    mouseEventCount++;

    if (!mouseLog) {
        mouseLog = fopen("/tmp/vm_mouse_events.log", "w");
    }
    // Always log button presses (down/up), limit moves to first 200
    if (mouseLog && (mouseEventCount <= 200 || type != 0)) {
        const char* typeStr = (type == 0) ? "move" : (type == 1) ? "down" : "up";
        fprintf(mouseLog, "[VM_MOUSE] #%d %s at (%d,%d) buttons=%d mods=%d\n",
                mouseEventCount, typeStr, x, y, buttons, modifiers);
        fflush(mouseLog);
    }

    pharo::Event event;
    event.type = static_cast<int>(pharo::EventType::Mouse);
    event.timeStamp = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );
    event.arg1 = x;
    event.arg2 = y;
    event.arg3 = buttons;
    event.arg4 = modifiers;
    // Pharo event format: 1=mouseDown, 2=mouseUp, 3=mouseMove
    // Swift sends: 0=move, 1=down, 2=up - convert move from 0 to 3
    event.arg5 = (type == 0) ? 3 : type;

    // Debug: verify event type before push
    if (mouseLog && mouseEventCount <= 200) {
        fprintf(mouseLog, "[VM_MOUSE] #%d PUSHING event.type=%d to queue at %p\n",
                mouseEventCount, event.type, (void*)&pharo::gEventQueue);
        fflush(mouseLog);
    }
    pharo::gEventQueue.push(event);
}

void vm_postKeyEvent(int type, int charCode, int keyCode, int modifiers) {
    pharo::Event event;
    event.type = static_cast<int>(pharo::EventType::Keyboard);
    event.timeStamp = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );
    event.arg1 = charCode;
    event.arg2 = type;  // 0=down, 1=up, 2=stroke
    event.arg3 = modifiers;
    event.arg4 = keyCode;
    pharo::gEventQueue.push(event);
}

void vm_postScrollEvent(int x, int y, int deltaX, int deltaY, int modifiers) {
    pharo::Event event;
    event.type = static_cast<int>(pharo::EventType::MouseWheel);
    event.timeStamp = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );
    // Mouse wheel event format:
    // arg1 = x position
    // arg2 = y position
    // arg3 = deltaX (horizontal scroll)
    // arg4 = deltaY (vertical scroll)
    // arg5 = modifiers
    event.arg1 = x;
    event.arg2 = y;
    event.arg3 = deltaX;
    event.arg4 = deltaY;
    event.arg5 = modifiers;
    pharo::gEventQueue.push(event);
}

void vm_postWindowEvent(int width, int height) {
    pharo::Event event;
    event.type = static_cast<int>(pharo::EventType::WindowMetrics);
    event.arg1 = width;
    event.arg2 = height;
    pharo::gEventQueue.push(event);
}

} // extern "C"
