/*
 * iOSBridge.cpp - Bridge between iOS API and PlatformBridge C++ API
 *
 * Provides compatibility layer for the iOS app which expects the old API.
 */

#include "PlatformBridge.h"
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <thread>
#include <chrono>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

// The iOS app expects these types
typedef struct VMParameterVector_ {
    uint32_t count;
    const char** parameters;
} VMParameterVector;

typedef struct VMParameters_ {
    char* imageFileName;
    bool isDefaultImage;
    bool defaultImageFound;
    bool isInteractiveSession;
    bool isWorker;
    int maxStackFramesToPrint;
    long long maxOldSpaceSize;
    long long maxCodeSize;
    long long edenSize;
    long long minPermSpaceSize;
    long long maxSlotsForNewSpaceAlloc;
    int processArgc;
    const char** processArgv;
    const char** environmentVector;
    bool avoidSearchingSegmentsWithPinnedObjects;
    VMParameterVector vmParameters;
    VMParameterVector imageParameters;
} VMParameters;

// Callback storage
static void (*gIOSDisplayCallback)(int, int, int, int) = nullptr;

extern "C" {

// ===== VM Lifecycle =====

void vm_parameters_init(VMParameters* parameters) {
    if (parameters) {
        memset(parameters, 0, sizeof(VMParameters));
        parameters->maxOldSpaceSize = 256 * 1024 * 1024;  // 256 MB default
        parameters->edenSize = 10 * 1024 * 1024;          // 10 MB default
        parameters->isInteractiveSession = true;
    }
}

void vm_parameters_destroy(VMParameters* parameters) {
    if (parameters) {
        if (parameters->imageFileName) {
            free(parameters->imageFileName);
            parameters->imageFileName = nullptr;
        }
    }
}

int vm_init(VMParameters* parameters) {
    if (!parameters) return 0;

    // Initialize VM memory
    size_t heapSize = parameters->maxOldSpaceSize > 0
                      ? (size_t)parameters->maxOldSpaceSize
                      : 256 * 1024 * 1024;

    if (!vm_initialize(heapSize)) {
        return 0;
    }

    // Set up a default display surface BEFORE loading image, but only if not already set
    // This ensures the VM has somewhere to render during startup
    // The actual size will be updated when the Metal view appears
    if (vm_getDisplayWidth() == 0) {
        vm_setDisplaySize(1024, 768, 32);
    }

    // Load the image if specified
    if (parameters->imageFileName) {
        if (!vm_loadImage(parameters->imageFileName)) {
            return 0;
        }
    }

    return 1;  // Success
}

void vm_run_interpreter(void) {
    vm_run();
    // Block while running
    while (vm_isRunning()) {
        // Sleep to avoid busy-wait
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // CRITICAL: Must call vm_stop to join the thread, otherwise std::terminate is called
    vm_stop();
}

// ===== iOS Display Bridge =====

void ios_registerDisplayUpdateCallback(void (*callback)(int, int, int, int)) {
    gIOSDisplayCallback = callback;

    // Create an adapter that calls the iOS callback
    vm_setDisplayUpdateCallback([](int x, int y, int w, int h, void* ctx) {
        if (gIOSDisplayCallback) {
            gIOSDisplayCallback(x, y, w, h);
        }
    }, nullptr);
}

void* ios_getDisplayBits(void) {
    return vm_getDisplayPixels();
}

int ios_getDisplayWidth(void) {
    return vm_getDisplayWidth();
}

int ios_getDisplayHeight(void) {
    return vm_getDisplayHeight();
}

int ios_getDisplayDepth(void) {
    return 32;  // Always 32-bit RGBA
}

void ios_setDisplaySize(int width, int height) {
    vm_setDisplaySize(width, height, 32);
}

// iOS version of DisplayBufferInfo
typedef struct {
    uint32_t* pixels;
    int width;
    int height;
    size_t size;
} IOSDisplayBufferInfo;

void ios_getDisplayBufferInfo(IOSDisplayBufferInfo* info) {
    if (!info) return;
    // Use the atomic vm_ function
    DisplayBufferInfo vmInfo;
    vm_getDisplayBufferInfo(&vmInfo);
    info->pixels = vmInfo.pixels;
    info->width = vmInfo.width;
    info->height = vmInfo.height;
    info->size = vmInfo.size;
}

bool ios_copyDisplayBuffer(uint32_t* dest, size_t destSize, int* outWidth, int* outHeight) {
    return vm_copyDisplayBuffer(dest, destSize, outWidth, outHeight);
}

// ===== Legacy Touch/Key Events (redirect to new API) =====

void ios_queueTouchEvent(int type, int x, int y, int buttons, int modifiers) {
    vm_postMouseEvent(type, x, y, buttons, modifiers);
}

void ios_queueKeyEvent(int charCode, int pressCode, int modifiers) {
    vm_postKeyEvent(pressCode, charCode, 0, modifiers);
}

// ===== iOS Utility Functions =====

#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR

// These need to be implemented in Objective-C for actual iOS device queries
// For now, provide stub implementations that work for testing

int iosIsIPad(void) {
#if TARGET_OS_IPHONE
    // Check UI idiom at runtime
    // This is a simplified check - actual implementation should use UIDevice
    return 0;  // Assume iPhone by default
#else
    return 0;
#endif
}

static char gDeviceModel[64] = "iPhone Simulator";
const char* iosGetDeviceModel(void) {
    return gDeviceModel;
}

static char gSystemVersion[32] = "15.0";
const char* iosGetSystemVersion(void) {
    return gSystemVersion;
}

int iosOpenURL(const char* urlString) {
    // Not implemented - would need UIApplication
    return 0;
}

void iosShowAlert(const char* title, const char* message) {
    // Not implemented - would need UIAlertController
}

void iosHapticFeedback(int style) {
    // Not implemented - would need UIFeedbackGenerator
}

#else
// Non-iOS stubs

int iosIsIPad(void) {
    return 0;
}

const char* iosGetDeviceModel(void) {
    return "macOS";
}

const char* iosGetSystemVersion(void) {
    return "10.0";
}

int iosOpenURL(const char* urlString) {
    return 0;
}

void iosShowAlert(const char* title, const char* message) {
}

void iosHapticFeedback(int style) {
}

#endif

} // extern "C"
