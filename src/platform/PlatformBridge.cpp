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
#include "../vm/InterpreterProxy.h"
#include "../vm/plugins/SocketPlugin.h"
#include "../vm/plugins/SoundPlugin.h"
#include "../vm/plugins/MIDIPlugin.h"
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include <mutex>

#ifdef __APPLE__
#include <TargetConditionals.h>
#include <CoreFoundation/CoreFoundation.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include "../vm/ObjCExceptionGuard.h"

// On Mac Catalyst, the Pharo image calls AppKit APIs via FFI during SDL
// platform init (finishLaunching, NSMenu creation, etc). These crash on
// Catalyst because UIKit manages the app lifecycle and menus. Swizzle
// the problematic methods to no-ops — Pharo's AppKit menus are meaningless
// on Catalyst since UIKit handles menus via UIMenu.
// Saved original IMP for setMainMenu: — called through for UIKit (main thread) calls.
static IMP origSetMainMenu = nullptr;

static void swizzleCatalystAppKit() {
#if TARGET_OS_MACCATALYST
    // [NSApplication finishLaunching] — throws assertion failures
    // [NSApplication setMainMenu:] — throws "API misuse: setting the main
    //   menu on a non-main thread" because the VM thread calls SDL init
    //   which creates AppKit menus via FFI. We block off-main-thread calls
    //   but allow UIKit's main-thread calls so the system Quit menu works.
    Class nsApp = objc_getClass("NSApplication");
    if (nsApp) {
        class_replaceMethod(nsApp, sel_registerName("finishLaunching"),
            (IMP)+[](id, SEL){}, "v@:");

        // Save original setMainMenu: and replace with thread-checking version
        Method m = class_getInstanceMethod(nsApp, sel_registerName("setMainMenu:"));
        if (m) origSetMainMenu = method_getImplementation(m);
        class_replaceMethod(nsApp, sel_registerName("setMainMenu:"),
            (IMP)+[](id self_, SEL sel_, id menu) {
                // Allow UIKit's main-thread calls (creates system menu with Quit).
                // Block Pharo FFI calls from the VM thread.
                if (pthread_main_np() && origSetMainMenu) {
                    ((void(*)(id, SEL, id))origSetMainMenu)(self_, sel_, menu);
                }
            }, "v@:@");

        class_replaceMethod(nsApp, sel_registerName("setAppleMenu:"),
            (IMP)+[](id, SEL, id){}, "v@:@");
        class_replaceMethod(nsApp, sel_registerName("setServicesMenu:"),
            (IMP)+[](id, SEL, id){}, "v@:@");
        class_replaceMethod(nsApp, sel_registerName("setWindowsMenu:"),
            (IMP)+[](id, SEL, id){}, "v@:@");
        class_replaceMethod(nsApp, sel_registerName("setHelpMenu:"),
            (IMP)+[](id, SEL, id){}, "v@:@");
    }

    // Pharo's SDL platform init creates AppKit menus via FFI.
    // On Catalyst these crash — swizzle ALL NSMenu/NSMenuItem mutation
    // methods to no-ops so none of them touch invalid ObjC objects.
    // Note: lambdas with variadic args can't convert to IMP, so we use
    // concrete signatures. Extra args are ignored on ARM64 calling convention.
    auto voidNoop1 = (IMP)+[](id, SEL, id){};
    auto voidNoop2 = (IMP)+[](id, SEL, id, long){};
    auto nilRet4 = (IMP)+[](id, SEL, id, SEL, id, long) -> id { return nullptr; };
    auto nilRet3 = (IMP)+[](id, SEL, id, SEL, id) -> id { return nullptr; };
    auto voidNoopBool = (IMP)+[](id, SEL, bool){};
    auto voidNoopSel = (IMP)+[](id, SEL, SEL){};
    auto voidNoopLong = (IMP)+[](id, SEL, long){};
    auto voidNoop2Obj = (IMP)+[](id, SEL, id, id){};
    auto voidNoopUL = (IMP)+[](id, SEL, unsigned long){};

    Class nsMenu = objc_getClass("NSMenu");
    if (nsMenu) {
        class_replaceMethod(nsMenu, sel_registerName("insertItemWithTitle:action:keyEquivalent:atIndex:"), nilRet4, "@@:@:@l");
        class_replaceMethod(nsMenu, sel_registerName("addItemWithTitle:action:keyEquivalent:"), nilRet3, "@@:@:@");
        class_replaceMethod(nsMenu, sel_registerName("insertItem:atIndex:"), voidNoop2, "v@:@l");
        class_replaceMethod(nsMenu, sel_registerName("addItem:"), voidNoop1, "v@:@");
        class_replaceMethod(nsMenu, sel_registerName("removeItem:"), voidNoop1, "v@:@");
        class_replaceMethod(nsMenu, sel_registerName("removeItemAtIndex:"), voidNoopLong, "v@:l");
        class_replaceMethod(nsMenu, sel_registerName("setTitle:"), voidNoop1, "v@:@");
        class_replaceMethod(nsMenu, sel_registerName("setSubmenu:forItem:"), voidNoop2Obj, "v@:@@");
        class_replaceMethod(nsMenu, sel_registerName("setAutoenablesItems:"), voidNoopBool, "v@:B");
    }

    Class nsMenuItem = objc_getClass("NSMenuItem");
    if (nsMenuItem) {
        class_replaceMethod(nsMenuItem, sel_registerName("setSubmenu:"), voidNoop1, "v@:@");
        class_replaceMethod(nsMenuItem, sel_registerName("setEnabled:"), voidNoopBool, "v@:B");
        class_replaceMethod(nsMenuItem, sel_registerName("setTitle:"), voidNoop1, "v@:@");
        class_replaceMethod(nsMenuItem, sel_registerName("setAction:"), voidNoopSel, "v@::");
        class_replaceMethod(nsMenuItem, sel_registerName("setTarget:"), voidNoop1, "v@:@");
        class_replaceMethod(nsMenuItem, sel_registerName("setKeyEquivalent:"), voidNoop1, "v@:@");
        class_replaceMethod(nsMenuItem, sel_registerName("setKeyEquivalentModifierMask:"), voidNoopUL, "v@:Q");
        class_replaceMethod(nsMenuItem, sel_registerName("setTag:"), voidNoopLong, "v@:l");
    }
#endif
}
#endif

namespace pharo {
    DisplaySurface* gDisplaySurface = nullptr;
}


// Single-buffered display surface — the VM writes directly and Metal reads at 30fps.
// Mutex protects dimensions/buffer pointer during resize.
class SimpleDisplaySurface : public pharo::DisplaySurface {
public:
    SimpleDisplaySurface(int w, int h, int d)
        : width_(w), height_(h), depth_(d) {
        // Pre-reserve capacity for the largest display we'll ever see.
        // This guarantees resize() never reallocates the backing storage,
        // which would invalidate the pointer the VM thread got from pixels().
        static constexpr size_t kMaxPixels = 4096 * 3072;  // ~48MB virtual
        backBuffer_.reserve(kMaxPixels);
        backBuffer_.resize(w * h, 0);
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
        return false;  // Resize is immediate (no deferred/double-buffer)
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

    // Resize the display buffer. Clears to black to prevent garbled display
    // from stale pixel data written at the old pitch being read at the new pitch.
    // Uses resize() + fill() instead of assign() to avoid reallocation — the
    // VM thread may hold a pointer from pixels() and writing to it concurrently.
    void resize(int w, int h, int d) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (w == width_ && h == height_ && d == depth_) return;

        size_t needed = static_cast<size_t>(w) * h;
        width_ = w;
        height_ = h;
        depth_ = d;
        backBuffer_.resize(needed);  // Grows within reserved capacity — no realloc
        std::fill(backBuffer_.begin(), backBuffer_.end(), 0);  // Clear to black
    }

private:
    mutable std::mutex mutex_;

    int width_, height_, depth_;
    std::vector<uint32_t> backBuffer_;

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

#ifdef __APPLE__
    objc_install_exception_handler();
    swizzleCatalystAppKit();
#endif

    gMemory = new pharo::ObjectMemory();
    pharo::MemoryConfig config;
    config.oldSpaceSize = heapSize;
    config.newSpaceSize = 32 * 1024 * 1024;
    config.permSpaceSize = 8 * 1024 * 1024;

    return gMemory->initialize(config);
}

// Set FreeTypeSettings current bitBltSubPixelAvailable := false directly in
// the image, before any Smalltalk code runs.  This prevents the ~100ms timing
// gap where the render loop tries sub-pixel text rendering before startup.st
// can disable it.  See docs/subpixel-rendering.md.
static void disableSubPixelRendering() {
    // ObjC headers #define nil — undefine it so we can call gMemory->nil()
    #pragma push_macro("nil")
    #undef nil
    using namespace pharo;
    if (!gMemory) return;

    // 1. Find FreeTypeSettings class
    Oop ftClass = gMemory->findGlobal("FreeTypeSettings");
    if (ftClass.isNil() || !ftClass.isObject()) return;

    // 2. Find the singleton (class instance variable 'current').
    //    It's stored in the class object itself, at a slot beyond the standard
    //    Class layout.  Scan for a FreeTypeSettings instance in the class slots.
    Oop singleton = gMemory->nil();
    {
        ObjectHeader* ftHdr = ftClass.asObjectPtr();
        for (size_t i = 0; i < ftHdr->slotCount(); i++) {
            Oop slot = gMemory->fetchPointer(i, ftClass);
            if (slot.isObject() && !slot.isNil() &&
                gMemory->nameOfClass(gMemory->classOf(slot)) == "FreeTypeSettings") {
                singleton = slot;
                break;
            }
        }
    }
    if (singleton.isNil() || !singleton.isObject()) {
        fprintf(stderr, "[VM] FreeTypeSettings singleton not yet created — startup.st will handle it\n");
        #pragma pop_macro("nil")
        return;
    }

    // 3. Find 'bitBltSubPixelAvailable' via the FixedLayout.
    //    In Pharo 13+, class slot 3 is a FixedLayout (not Array of Symbols).
    //    FixedLayout[1] → LayoutClassScope
    //    LayoutClassScope: slot 0 = parentScope, slots 1..N = Slot objects
    //    Each Slot: slot 0 = name (ByteSymbol)
    //    Instance variable index = scope position - 1
    Oop layout = gMemory->fetchPointer(3, ftClass);
    if (!layout.isNil() && layout.isObject()) {
        Oop scope = gMemory->fetchPointer(1, layout);
        if (!scope.isNil() && scope.isObject()) {
            ObjectHeader* scopeHdr = scope.asObjectPtr();
            for (size_t si = 1; si < scopeHdr->slotCount(); si++) {
                Oop slotObj = gMemory->fetchPointer(si, scope);
                if (!slotObj.isObject() || slotObj.isNil()) continue;
                if (gMemory->symbolEquals(gMemory->fetchPointer(0, slotObj), "bitBltSubPixelAvailable")) {
                    size_t instVarIdx = si - 1;
                    gMemory->storePointer(instVarIdx, singleton, gMemory->falseObject());
                    fprintf(stderr, "[VM] Set FreeTypeSettings>>bitBltSubPixelAvailable to false (slot %zu)\n", instVarIdx);
                    #pragma pop_macro("nil")
                    return;
                }
            }
        }
    }
    #pragma pop_macro("nil")
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

    // Display mode: --headless tells the image we don't have a native display
    // (so it uses OSSDL2Driver via FFI stubs instead of native windows).
    // --interactive tells the image to start MorphicUIManager and the render loop.
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

    // Disable sub-pixel text rendering BEFORE the interpreter runs.
    // See docs/subpixel-rendering.md — the render loop starts before
    // startup.st can set this, causing ~100ms of failed sub-pixel calls.
    // Setting it here (after image load, before vm_run) eliminates the gap.
    disableSubPixelRendering();

    // Defer FreeType and FFI type startup to first use.
    // These SessionManager handlers are very slow on our interpreter,
    // especially on fresh images where ExternalObject recompiles hundreds
    // of FFI type definitions.  Startup.st handles FreeType configuration;
    // ExternalObject types are compiled lazily on first FFI call.
    {
        #pragma push_macro("nil")
        #undef nil
        using namespace pharo;
        const char* classesToDefer[] = {
            "FreeTypeSettings", "FreeTypeCache", "ExternalObject", nullptr
        };
        for (const char** p = classesToDefer; *p; p++) {
            Oop cls = gMemory->findGlobal(*p);
            if (cls.isObject() && !cls.isNil()) {
                if (gMemory->patchClassMethodToReturnSelf(cls, "startUp:")) {
                    fprintf(stderr, "[VM] Deferred %s class >> startUp: (will init on first use)\n", *p);
                }
            }
        }
        #pragma pop_macro("nil")
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
        int sleepUs = std::max(microseconds, 1000);
        if (sleepUs > 10000) sleepUs = 10000;  // Cap at 10ms
        usleep(sleepUs);
    });

    gRunning = true;
    gVMThread = std::thread([]() {
        // No autorelease pool on this thread. ObjC objects created by FFI
        // calls (NSString, NSArray, etc.) are leaked without a pool, which
        // keeps them alive for Pharo to reference via ExternalAddress — the
        // same lifetime as the standard Pharo VM where the pool wraps the
        // entire interpreter loop and is never drained. Without a pool,
        // thread exit doesn't try to release potentially-freed objects.

        // Don't post window events here. The SDL poll countdown delivers
        // SIZE_CHANGED + EXPOSED after SessionManager has had time to
        // initialize UITheme (~1.5s). Posting early causes redundant redraws.

        // Call interpret() which includes periodic event processing and semaphore handling
        gInterpreter->interpret();

        // Interpreter exited (primitiveQuit or stopVM called).
        gInterpreter->stopHeartbeat();
        gRunning = false;

        // Do NOT return — returning triggers pthread TSD cleanup which
        // crashes releasing ObjC objects in autorelease pool pages created
        // internally by FFI calls. Block forever; process exit kills us.
        while (true) {
            std::this_thread::sleep_for(std::chrono::hours(24));
        }
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

    // Don't post window events here. The SDL poll countdown delivers
    // SIZE_CHANGED + EXPOSED after SessionManager has had time to
    // initialize UITheme (~1.5s). Posting early causes redundant redraws.

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

    // Wait for the interpreter to finish, then detach the thread.
    // We never join — the VM thread blocks forever after interpret() returns
    // to avoid pthread TSD cleanup crashing on ObjC autorelease pool pages.
    if (gVMThread.joinable()) {
        auto start = std::chrono::steady_clock::now();
        while (gRunning) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed > std::chrono::seconds(2)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        gVMThread.detach();
    }

    // Stop the heartbeat thread (may already be stopped by VM thread)
    if (gInterpreter) {
        gInterpreter->stopHeartbeat();
    }
}

void vm_destroy(void) {
    // Delete all VM objects and reset state so vm_initialize()/vm_loadImage()/vm_run()
    // can be called again for a fresh launch. The old VM thread (if any) is detached
    // and sleeping — it uses negligible resources and dies with the process.

    delete gInterpreter;
    gInterpreter = nullptr;

    delete gMemory;
    gMemory = nullptr;

    delete gDisplay;
    gDisplay = nullptr;
    pharo::gDisplaySurface = nullptr;

    // Shut down plugins that have background threads or OS resources.
    // Must happen before shutdownFFI/resetInterpreterProxy since plugins
    // reference the VM proxy.
    socketPluginShutdown();
    soundStop();
    midiShutdown();

    // Reset subsystems
    pharo::ffi::shutdownFFI();
    pharo::gEventQueue.reset();
    resetInterpreterProxy();

    // Reset display form readiness flag (defined in Interpreter.cpp)
    extern std::atomic<bool> g_displayFormReady;
    g_displayFormReady.store(false, std::memory_order_relaxed);

    // Critical: reset gRunning so vm_run() can start a new interpreter.
    // Without this, the second vm_run() call sees gRunning == true and
    // returns immediately, leaving the VM dead on relaunch.
    gRunning = false;

    // Note: gPendingCallback, gClipboardGet/Set, gTextInputFunc are Swift-side
    // callbacks that persist across VM instances — do NOT reset them.
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
