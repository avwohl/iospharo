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

namespace pharo {
    DisplaySurface* gDisplaySurface = nullptr;
}

// Double-buffered display surface
// WIP: Resize tearing not fully fixed - needs proper synchronization
class SimpleDisplaySurface : public pharo::DisplaySurface {
public:
    SimpleDisplaySurface(int w, int h, int d) : width_(w), height_(h), depth_(d) {
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

    void getBufferInfo(int& w, int& h, uint32_t*& pixels, size_t& size) {
        std::lock_guard<std::mutex> lock(mutex_);
        w = width_;
        h = height_;
        pixels = frontBuffer_.data();
        size = frontBuffer_.size();
    }

    bool isResizing() const {
        return false;  // Disabled - was causing deadlock
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
        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::swap(backBuffer_, frontBuffer_);
        }
        invalidateRect(0, 0, width_, height_);
    }

    void setCallback(DisplayUpdateFunc cb, void* ctx) {
        std::lock_guard<std::mutex> lock(mutex_);
        updateCallback_ = cb;
        context_ = ctx;
    }

    void resize(int w, int h, int d) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (w == width_ && h == height_ && d == depth_) return;
        width_ = w;
        height_ = h;
        depth_ = d;
        backBuffer_.resize(w * h);
        frontBuffer_.resize(w * h);
    }

private:
    mutable std::mutex mutex_;
    int width_, height_, depth_;
    std::vector<uint32_t> backBuffer_;
    std::vector<uint32_t> frontBuffer_;
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
    if (gInterpreter) {
        int semIndex = pharo::gEventQueue.getInputSemaphoreIndex();
        if (semIndex > 0) {
            gInterpreter->signalExternalSemaphore(semIndex);
        }
    }
}

extern "C" {

bool vm_initialize(size_t heapSize) {
    std::cerr << "[PB] vm_initialize: heapSize=" << heapSize << "\n";
    if (gMemory) {
        std::cerr << "[PB] vm_initialize: already initialized\n";
        return true;
    }

    gMemory = new pharo::ObjectMemory();
    pharo::MemoryConfig config;
    config.oldSpaceSize = heapSize;
    config.newSpaceSize = 32 * 1024 * 1024;
    config.permSpaceSize = 8 * 1024 * 1024;

    bool result = gMemory->initialize(config);
    std::cerr << "[PB] vm_initialize: " << (result ? "success" : "failed") << "\n";
    return result;
}

bool vm_loadImage(const char* imagePath) {
    if (!gMemory) {
        std::cerr << "[PB] vm_loadImage: gMemory is null\n";
        return false;
    }

    std::cerr << "[PB] vm_loadImage: Loading " << imagePath << "\n";
    pharo::ImageLoader loader;
    pharo::LoadResult result = loader.load(imagePath, *gMemory);

    if (!result.success) {
        std::cerr << "[PB] vm_loadImage: Image load failed\n";
        return false;
    }
    std::cerr << "[PB] vm_loadImage: Image loaded successfully\n";

    gInterpreter = new pharo::Interpreter(*gMemory);
    if (!gInterpreter->initialize()) {
        std::cerr << "[PB] vm_loadImage: Interpreter init failed\n";
        return false;
    }
    std::cerr << "[PB] vm_loadImage: Interpreter initialized\n";

    // Apply display size if already set (vm_setDisplaySize may be called before vm_loadImage)
    if (gDisplay) {
        gInterpreter->setScreenSize(gDisplay->width(), gDisplay->height());
        gInterpreter->setScreenDepth(gDisplay->depth());

        // Ensure Display Form exists and is bound to 'Display' global
        // This is critical for Morphic rendering
        if (gInterpreter->displayForm().isNil()) {
            std::cerr << "[PB] vm_loadImage: Creating Display Form...\n";
            gInterpreter->ensureDisplayForm(gDisplay->width(), gDisplay->height(), gDisplay->depth());
        }
    }

    // Register event callback to signal input semaphore when events arrive
    pharo::gEventQueue.setEventCallback(eventCallback, nullptr);

    return true;
}

void vm_run(void) {
    if (!gInterpreter || gRunning) return;

    std::cerr << "[PB] vm_run: starting thread\n";

    // Start the heartbeat thread (handles timers, like official VM)
    gInterpreter->startHeartbeat();

    gRunning = true;
    gVMThread = std::thread([]() {
        std::cerr << "[PB] Thread started, isRunning=" << gInterpreter->isRunning() << "\n";

        // Post a window resize event to trigger Pharo layout
        // This tells Pharo the display size so it can lay out morphs properly
        if (gDisplay) {
            std::cerr << "[PB] Posting initial window resize event: "
                      << gDisplay->width() << "x" << gDisplay->height() << "\n";
            vm_postWindowEvent(gDisplay->width(), gDisplay->height());

            // Show a test pattern to verify display pipeline works
            // This proves native rendering works even before Pharo renders
            int w = gDisplay->width();
            int h = gDisplay->height();
            uint32_t* pixels = gDisplay->pixels();
            std::cerr << "[PB] Drawing startup test pattern " << w << "x" << h << "\n";
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
            std::cerr << "[PB] Test pattern drawn and update() called\n";
        }

        while (gRunning && gInterpreter->isRunning()) {
            gInterpreter->step();
        }
        gRunning = false;
        std::cerr << "[PB] Thread finished\n";
    });
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
            std::cerr << "[PB] Applying pending display callback\n";
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
    event.arg5 = type;  // 0=move, 1=down, 2=up
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
