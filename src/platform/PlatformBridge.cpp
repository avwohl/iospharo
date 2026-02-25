/*
 * PlatformBridge.cpp - C bridge implementation
 */

#include "PlatformBridge.h"
#include "DisplaySurface.hpp"
#include "EventQueue.hpp"
#include "../vm/ObjectMemory.hpp"
#include "../vm/ImageLoader.hpp"
#include "../vm/Interpreter.hpp"
#include "../vm/FFI.hpp"
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include <mutex>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <objc/objc.h>
// C API for autorelease pool management from C++ threads.
// Any thread that might create ObjC objects (even indirectly via FFI callbacks)
// must have an autorelease pool, or objects leak into an implicit pool that
// crashes on thread exit when it tries to release already-freed objects.
extern "C" void *objc_autoreleasePoolPush(void);
extern "C" void objc_autoreleasePoolPop(void *pool);
#endif

namespace pharo {
    DisplaySurface* gDisplaySurface = nullptr;
}

#ifdef __APPLE__
// Thread-local autorelease pool for the VM thread.  The relinquish callback
// pops and re-pushes this pool on every yield, preventing autoreleased ObjC
// objects from accumulating for the entire thread lifetime.
static thread_local void *tls_autoreleasePool = nullptr;
#endif

// Double-buffered display surface with deferred resize
// Resize is queued and applied during buffer swap to prevent tearing
class SimpleDisplaySurface : public pharo::DisplaySurface {
public:
    SimpleDisplaySurface(int w, int h, int d)
        : width_(w), height_(h), depth_(d),
          pendingWidth_(0), pendingHeight_(0), pendingDepth_(0),
          pendingResize_(false) {
        backBuffer_.resize(w * h);
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
        return backBuffer_.data();  // Single-buffer: same as pixels()
    }

    size_t pitch() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return width_ * 4;
    }

    // Returns buffer info - single-buffer, same as what VM writes to
    void getBufferInfo(int& w, int& h, uint32_t*& pixels, size_t& size) {
        std::lock_guard<std::mutex> lock(mutex_);
        w = width_;
        h = height_;
        pixels = backBuffer_.data();
        size = backBuffer_.size();
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

            // Apply pending resize
            if (pendingResize_) {
                pendingResize_ = false;
            }

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
    (void)context;
    if (gInterpreter) {
        int semIndex = pharo::gEventQueue.getInputSemaphoreIndex();
        if (semIndex <= 0) {
            semIndex = 1;  // Default input semaphore index
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
    // Set app bundle path so FFI can find bundled libraries in Contents/Frameworks
#ifdef __APPLE__
    CFBundleRef mainBundle = CFBundleGetMainBundle();
    if (mainBundle) {
        CFURLRef bundleURL = CFBundleCopyBundleURL(mainBundle);
        if (bundleURL) {
            char bundlePath[1024];
            if (CFURLGetFileSystemRepresentation(bundleURL, true, (UInt8*)bundlePath, sizeof(bundlePath))) {
                pharo::ffi::setAppBundlePath(bundlePath);
            }
            CFRelease(bundleURL);
        }
    }
#endif

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
    gInterpreter->setOriginalImageHeader(loader.header());

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

    // SDL2/cairo libraries are compiled into the binary as stubs.
    // File primitives (primitiveFileExists, primitiveFileAttribute) report
    // these library names as "existing" so Pharo's FFI finder proceeds to
    // primitiveLoadModule, which returns the built-in handle.
    // No placeholder files needed on disk.

    // Register event callback to signal input semaphore when events arrive
    pharo::gEventQueue.setEventCallback(eventCallback, nullptr);

    return true;
}

void vm_run(void) {
    if (!gInterpreter || gRunning) return;

    // Start the heartbeat thread (handles timers, like official VM)
    gInterpreter->startHeartbeat();

    // Set relinquish callback for background thread (uses usleep, not CFRunLoop)
    gInterpreter->setRelinquishCallback([](int microseconds) {
#ifdef __APPLE__
        // Drain autoreleased ObjC objects that accumulated since the last
        // yield.  FFI callbacks (clipboard, SDL2 stubs, etc.) create
        // autoreleased objects on this thread.  Without periodic draining,
        // they pile up for the entire interpret() run and are freed
        // externally before the pool drains at thread exit → crash in
        // AutoreleasePoolPage::releaseUntil().
        if (tls_autoreleasePool) {
            objc_autoreleasePoolPop(tls_autoreleasePool);
        }
        tls_autoreleasePool = objc_autoreleasePoolPush();
#endif
        int sleepUs = std::max(microseconds, 1000);
        if (sleepUs > 10000) sleepUs = 10000;  // Cap at 10ms
        usleep(sleepUs);
    });

    gRunning = true;
    gVMThread = std::thread([]() {
#ifdef __APPLE__
        tls_autoreleasePool = objc_autoreleasePoolPush();
#endif
        // Post a window resize event to trigger Pharo layout
        if (gDisplay) {
            vm_postWindowEvent(gDisplay->width(), gDisplay->height());
        }

        // Call interpret() which includes periodic event processing and semaphore handling
        gInterpreter->interpret();

        // Interpreter exited (primitiveQuit or stopVM called).
        // Stop heartbeat from this thread before marking as not running,
        // so vm_stop() doesn't need to join a dead heartbeat thread.
        gInterpreter->stopHeartbeat();
        gRunning = false;
#ifdef __APPLE__
        if (tls_autoreleasePool) {
            objc_autoreleasePoolPop(tls_autoreleasePool);
            tls_autoreleasePool = nullptr;
        }
#endif
    });
}

void vm_runOnMainThread(void) {
    if (!gInterpreter || gRunning) return;

    // Start the heartbeat thread (handles timers, like official VM)
    gInterpreter->startHeartbeat();

    gRunning = true;

    // Ensure display surface exists before interpreter starts.
    // The interpreter blocks the main thread, and SwiftUI's MetalView
    // may not have called vm_setDisplaySize yet. Create a default
    // display surface so SDL stubs can render to it immediately.
    // MetalView's drawableSizeWillChange will resize later.
    if (!gDisplay) {
        vm_setDisplaySize(1024, 768, 32);
    }

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
        // Pump the native run loop so Metal can render frames and UIKit
        // can deliver touch/keyboard events. The interpreter runs on the
        // main thread, so we must periodically give the run loop time.
        double seconds = std::max(microseconds, 1000) / 1000000.0;
        if (seconds > 0.016) seconds = 0.016;  // Cap at ~60fps
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, true);
#else
        int sleepUs = std::max(microseconds, 1000);
        if (sleepUs > 10000) sleepUs = 10000;
        usleep(sleepUs);
#endif
    });

    // Pump the run loop before starting the interpreter so SwiftUI has
    // time to lay out the Metal view. The interpreter blocks the main
    // thread and doesn't yield during initial startup (main process never
    // relinquishes). Without this, the MTKView is never created and the
    // display callback never fires.
#ifdef __APPLE__
    for (int i = 0; i < 30; i++) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.016, false);
    }
#endif

    // Run interpreter on the current (main) thread
    // This blocks until the interpreter exits
    gInterpreter->interpret();
    gRunning = false;
}

void vm_stop(void) {
    // Signal the interpreter to exit its main loop
    if (gInterpreter) {
        gInterpreter->stop();
    }

    // Wait for the interpreter thread with a timeout.
    // applicationWillTerminate gives ~5 seconds; don't block forever.
    if (gVMThread.joinable()) {
        auto start = std::chrono::steady_clock::now();
        while (gRunning) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed > std::chrono::seconds(2)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!gRunning) {
            gVMThread.join();
        } else {
            gVMThread.detach();  // Abandon thread if interpreter won't stop
        }
    }

    // Stop the heartbeat thread (may already be stopped by VM thread)
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

    // Notify the SDL stub layer so it pushes SIZE_CHANGED + EXPOSED events.
    // This ensures Pharo re-layouts when the Metal view size changes.
    ffi_notifyDisplayResize(width, height);
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
    // Pharo event format: 1=mouseDown, 2=mouseUp, 3=mouseMove
    // Swift sends: 0=move, 1=down, 2=up - convert move from 0 to 3
    event.arg5 = (type == 0) ? 3 : type;
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

// Clipboard
static ClipboardGetFunc gClipboardGet = nullptr;
static ClipboardSetFunc gClipboardSet = nullptr;

void vm_setClipboardCallbacks(ClipboardGetFunc getFunc, ClipboardSetFunc setFunc) {
    gClipboardGet = getFunc;
    gClipboardSet = setFunc;
}

const char* vm_getClipboardText(void) {
    if (gClipboardGet) return gClipboardGet();
    return "";
}

void vm_setClipboardText(const char* text) {
    if (gClipboardSet) gClipboardSet(text);
}

// Text input (keyboard)
static TextInputFunc gTextInputFunc = nullptr;

void vm_setTextInputCallback(TextInputFunc func) {
    gTextInputFunc = func;
}

void vm_startTextInput(void) {
    if (gTextInputFunc) gTextInputFunc(true);
}

void vm_stopTextInput(void) {
    if (gTextInputFunc) gTextInputFunc(false);
}

} // extern "C"
