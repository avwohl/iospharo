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

namespace pharo {
    DisplaySurface* gDisplaySurface = nullptr;
}

// Simple display surface backed by a pixel buffer
class SimpleDisplaySurface : public pharo::DisplaySurface {
public:
    SimpleDisplaySurface(int w, int h, int d) : width_(w), height_(h), depth_(d) {
        pixels_.resize(w * h);
    }

    int width() const override { return width_; }
    int height() const override { return height_; }
    int depth() const override { return depth_; }
    uint32_t* pixels() override { return pixels_.data(); }
    size_t pitch() const override { return width_ * 4; }

    void invalidateRect(int x, int y, int w, int h) override {
        static int invalidateCount = 0;
        invalidateCount++;
        if (invalidateCount <= 5) {
            std::cerr << "[DISPLAY] invalidateRect #" << invalidateCount
                      << " (" << x << "," << y << "," << w << "," << h << ")"
                      << " callback=" << (updateCallback_ ? "set" : "null") << "\n";
        }
        if (updateCallback_) {
            updateCallback_(x, y, w, h, context_);
        }
    }

    void update() override {
        invalidateRect(0, 0, width_, height_);
    }

    void setCallback(DisplayUpdateFunc cb, void* ctx) {
        updateCallback_ = cb;
        context_ = ctx;
    }

private:
    int width_, height_, depth_;
    std::vector<uint32_t> pixels_;
    DisplayUpdateFunc updateCallback_ = nullptr;
    void* context_ = nullptr;
};

// Global state
static pharo::ObjectMemory* gMemory = nullptr;
static pharo::Interpreter* gInterpreter = nullptr;
static SimpleDisplaySurface* gDisplay = nullptr;
static std::thread gVMThread;
static std::atomic<bool> gRunning{false};

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
    }

    // Register event callback to signal input semaphore when events arrive
    pharo::gEventQueue.setEventCallback(eventCallback, nullptr);

    return true;
}

void vm_run(void) {
    if (!gInterpreter || gRunning) return;

    std::cerr << "[PB] vm_run: starting thread\n";

    gRunning = true;
    gVMThread = std::thread([]() {
        std::cerr << "[PB] Thread started, isRunning=" << gInterpreter->isRunning() << "\n";

        // Post a window resize event to trigger Pharo layout
        // This tells Pharo the display size so it can lay out morphs properly
        if (gDisplay) {
            std::cerr << "[PB] Posting initial window resize event: "
                      << gDisplay->width() << "x" << gDisplay->height() << "\n";
            vm_postWindowEvent(gDisplay->width(), gDisplay->height());
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
}

bool vm_isRunning(void) {
    return gRunning;
}

void vm_setDisplaySize(int width, int height, int depth) {
    if (gDisplay) delete gDisplay;
    gDisplay = new SimpleDisplaySurface(width, height, depth);
    pharo::gDisplaySurface = gDisplay;

    // Also update the interpreter's screen size
    if (gInterpreter) {
        gInterpreter->setScreenSize(width, height);
        gInterpreter->setScreenDepth(depth);
    }
}

uint32_t* vm_getDisplayPixels(void) {
    return gDisplay ? gDisplay->pixels() : nullptr;
}

int vm_getDisplayWidth(void) {
    return gDisplay ? gDisplay->width() : 0;
}

int vm_getDisplayHeight(void) {
    return gDisplay ? gDisplay->height() : 0;
}

void vm_setDisplayUpdateCallback(DisplayUpdateFunc callback, void* context) {
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
