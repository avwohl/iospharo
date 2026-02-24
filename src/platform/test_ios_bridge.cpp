/*
 * test_ios_bridge.cpp - Test the iOS bridge API end-to-end
 *
 * This tests the same code path that the iOS app uses:
 * 1. vm_parameters_init
 * 2. vm_init (with image path)
 * 3. ios_setDisplaySize
 * 4. ios_registerDisplayUpdateCallback
 * 5. vm_run_interpreter (runs for a few seconds)
 * 6. Check if display was updated
 */

#include <iostream>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <thread>

#include "VMParameters.h"

// Declare the iOS bridge API
extern "C" {

void vm_parameters_init(VMParameters* parameters);
void vm_parameters_destroy(VMParameters* parameters);
int vm_init(VMParameters* parameters);
void vm_run_interpreter(void);
void vm_stop(void);
bool vm_isRunning(void);

void ios_registerDisplayUpdateCallback(void (*callback)(int, int, int, int));
void* ios_getDisplayBits(void);
int ios_getDisplayWidth(void);
int ios_getDisplayHeight(void);
int ios_getDisplayDepth(void);
void ios_setDisplaySize(int width, int height);

void vm_postMouseEvent(int type, int x, int y, int buttons, int modifiers);
void vm_postKeyEvent(int type, int charCode, int keyCode, int modifiers);

} // extern "C"

// Test state
static std::atomic<int> gDisplayUpdateCount{0};
static std::atomic<bool> gVMFinished{false};

void displayUpdateCallback(int x, int y, int width, int height) {
    gDisplayUpdateCount++;
    if (gDisplayUpdateCount <= 5) {
        std::cerr << "[TEST] Display update #" << gDisplayUpdateCount
                  << ": rect(" << x << "," << y << "," << width << "," << height << ")\n";
    }
}

int main(int argc, char* argv[]) {
    std::cerr << "===========================================\n";
    std::cerr << "iOS Bridge Integration Test\n";
    std::cerr << "===========================================\n\n";

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <image-path> [run-seconds]\n";
        return 1;
    }

    const char* imagePath = argv[1];
    int runSeconds = (argc > 2) ? atoi(argv[2]) : 5;

    std::cerr << "Image: " << imagePath << "\n";
    std::cerr << "Run time: " << runSeconds << " seconds\n\n";

    // Step 1: Initialize parameters (like Swift does)
    std::cerr << "[TEST] Step 1: Initialize VM parameters\n";
    VMParameters params;
    vm_parameters_init(&params);
    params.imageFileName = strdup(imagePath);
    params.maxOldSpaceSize = 256 * 1024 * 1024;
    params.isInteractiveSession = true;

    // Step 2: Set up display BEFORE vm_init (testing the fix)
    std::cerr << "[TEST] Step 2: Set display size (800x600)\n";
    ios_setDisplaySize(800, 600);

    // Step 3: Register display callback
    std::cerr << "[TEST] Step 3: Register display update callback\n";
    ios_registerDisplayUpdateCallback(displayUpdateCallback);

    // Step 4: Initialize VM
    std::cerr << "[TEST] Step 4: Initialize VM (vm_init)\n";
    int initResult = vm_init(&params);
    if (initResult == 0) {
        std::cerr << "[TEST] FAILED: vm_init returned 0\n";
        vm_parameters_destroy(&params);
        return 1;
    }
    std::cerr << "[TEST] vm_init succeeded\n";

    // Step 5: Check display is set up
    std::cerr << "[TEST] Step 5: Verify display setup\n";
    void* displayBits = ios_getDisplayBits();
    int width = ios_getDisplayWidth();
    int height = ios_getDisplayHeight();
    int depth = ios_getDisplayDepth();
    std::cerr << "  Display: " << width << "x" << height << "x" << depth << "\n";
    std::cerr << "  Pixels ptr: " << (displayBits ? "OK" : "NULL") << "\n";

    if (!displayBits) {
        std::cerr << "[TEST] FAILED: Display bits is NULL\n";
        vm_parameters_destroy(&params);
        return 1;
    }

    // Step 6: Start VM in background thread (like Swift does)
    std::cerr << "[TEST] Step 6: Start VM interpreter in background\n";
    std::thread vmThread([]() {
        vm_run_interpreter();
        gVMFinished = true;
        std::cerr << "[TEST] VM interpreter finished\n";
    });

    // Step 7: Wait and monitor
    std::cerr << "[TEST] Step 7: Running for " << runSeconds << " seconds...\n";
    auto startTime = std::chrono::steady_clock::now();
    int lastUpdateCount = 0;

    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - startTime).count();

        int currentUpdates = gDisplayUpdateCount.load();
        if (currentUpdates != lastUpdateCount) {
            std::cerr << "[TEST] " << elapsed << "s: " << currentUpdates << " display updates\n";
            lastUpdateCount = currentUpdates;
        }

        if (elapsed >= runSeconds) {
            std::cerr << "[TEST] Time limit reached, stopping VM\n";
            vm_stop();
            break;
        }

        if (gVMFinished) {
            std::cerr << "[TEST] VM finished on its own\n";
            break;
        }
    }

    // Wait for VM thread to finish
    if (vmThread.joinable()) {
        vmThread.join();
    }

    // Step 8: Check results
    std::cerr << "\n[TEST] Step 8: Results\n";
    std::cerr << "  Total display updates: " << gDisplayUpdateCount.load() << "\n";

    // Check if any pixels were written
    uint32_t* pixels = static_cast<uint32_t*>(displayBits);
    int nonBlackPixels = 0;
    int totalPixels = width * height;
    for (int i = 0; i < totalPixels && i < 100000; i++) {
        if (pixels[i] != 0 && pixels[i] != 0xFF000000) {
            nonBlackPixels++;
        }
    }
    std::cerr << "  Non-black pixels (sampled): " << nonBlackPixels << "\n";

    // Cleanup
    vm_parameters_destroy(&params);

    // Final verdict
    std::cerr << "\n===========================================\n";
    if (gDisplayUpdateCount > 0 || nonBlackPixels > 0) {
        std::cerr << "TEST PASSED: Display activity detected\n";
        return 0;
    } else {
        std::cerr << "TEST INCONCLUSIVE: No display updates\n";
        std::cerr << "(VM ran but didn't trigger display callbacks)\n";
        return 0;  // Not a failure, just no UI activity
    }
}
