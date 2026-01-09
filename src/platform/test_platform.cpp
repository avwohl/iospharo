/*
 * test_platform.cpp - Test the full GUI pipeline
 *
 * Tests: VM init → Image load → Run → Display updates → Input events
 */

#include "PlatformBridge.h"
#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <atomic>
#include <cstring>
#include <cstdlib>

// Stats
static std::atomic<int> displayUpdateCount{0};
static std::atomic<int> totalPixelsChanged{0};
static int lastUpdateX = 0, lastUpdateY = 0, lastUpdateW = 0, lastUpdateH = 0;

void displayCallback(int x, int y, int w, int h, void* context) {
    displayUpdateCount++;
    lastUpdateX = x;
    lastUpdateY = y;
    lastUpdateW = w;
    lastUpdateH = h;

    if (displayUpdateCount <= 5) {
        std::cout << "  [Display] Update #" << displayUpdateCount
                  << ": (" << x << "," << y << ") " << w << "x" << h << std::endl;
    }
}

int countNonZeroPixels(uint32_t* pixels, int width, int height) {
    int count = 0;
    int total = width * height;
    for (int i = 0; i < total; i++) {
        if (pixels[i] != 0) count++;
    }
    return count;
}

uint32_t samplePixel(uint32_t* pixels, int width, int x, int y) {
    return pixels[y * width + x];
}

void printPixelSample(uint32_t* pixels, int width, int height) {
    std::cout << "  Pixel samples:" << std::endl;
    // Sample corners and center
    int points[][2] = {{0,0}, {width/2, 0}, {width-1, 0},
                       {0, height/2}, {width/2, height/2}, {width-1, height/2},
                       {0, height-1}, {width/2, height-1}, {width-1, height-1}};
    for (auto& p : points) {
        uint32_t px = samplePixel(pixels, width, p[0], p[1]);
        std::cout << "    (" << p[0] << "," << p[1] << "): 0x"
                  << std::hex << px << std::dec << std::endl;
    }
}

int main(int argc, char* argv[]) {
    std::cout << "========================================" << std::endl;
    std::cout << "Pharo VM GUI Pipeline Test" << std::endl;
    std::cout << "========================================" << std::endl;

    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " <image-path> [run-seconds]" << std::endl;
        std::cout << "  run-seconds: how long to run (default: 3)" << std::endl;
        return 1;
    }

    int runSeconds = (argc >= 3) ? std::atoi(argv[2]) : 3;

    // ===== PHASE 1: Initialize =====
    std::cout << "\n[1] Initializing VM..." << std::endl;

    size_t heapSize = 256 * 1024 * 1024;
    if (!vm_initialize(heapSize)) {
        std::cerr << "FAIL: vm_initialize failed" << std::endl;
        return 1;
    }
    std::cout << "  Heap: " << (heapSize / 1024 / 1024) << " MB" << std::endl;

    // ===== PHASE 2: Set up display =====
    std::cout << "\n[2] Setting up display..." << std::endl;

    int width = 1024, height = 768, depth = 32;
    vm_setDisplaySize(width, height, depth);
    vm_setDisplayUpdateCallback(displayCallback, nullptr);

    uint32_t* pixels = vm_getDisplayPixels();
    if (!pixels) {
        std::cerr << "FAIL: vm_getDisplayPixels returned null" << std::endl;
        return 1;
    }

    std::cout << "  Size: " << vm_getDisplayWidth() << "x" << vm_getDisplayHeight() << std::endl;
    std::cout << "  Pixel buffer: " << (void*)pixels << std::endl;

    // Clear to known pattern
    memset(pixels, 0, width * height * 4);
    int initialNonZero = countNonZeroPixels(pixels, width, height);
    std::cout << "  Initial non-zero pixels: " << initialNonZero << std::endl;

    // ===== PHASE 3: Load image =====
    std::cout << "\n[3] Loading image..." << std::endl;
    std::cout << "  Path: " << argv[1] << std::endl;

    auto loadStart = std::chrono::steady_clock::now();
    if (!vm_loadImage(argv[1])) {
        std::cerr << "FAIL: vm_loadImage failed" << std::endl;
        return 1;
    }
    auto loadEnd = std::chrono::steady_clock::now();
    auto loadMs = std::chrono::duration_cast<std::chrono::milliseconds>(loadEnd - loadStart).count();
    std::cout << "  Load time: " << loadMs << " ms" << std::endl;

    // ===== PHASE 4: Run VM =====
    std::cout << "\n[4] Running VM for " << runSeconds << " seconds..." << std::endl;

    displayUpdateCount = 0;
    auto runStart = std::chrono::steady_clock::now();

    vm_run();

    // Monitor while running
    for (int i = 0; i < runSeconds; i++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        int nonZero = countNonZeroPixels(pixels, width, height);
        double pct = 100.0 * nonZero / (width * height);

        std::cout << "  [" << (i+1) << "s] Updates: " << displayUpdateCount.load()
                  << ", Non-zero pixels: " << nonZero
                  << " (" << std::fixed << std::setprecision(1) << pct << "%)"
                  << std::endl;
    }

    // ===== PHASE 5: Send test events =====
    std::cout << "\n[5] Sending test input events..." << std::endl;

    // Mouse move to center
    vm_postMouseEvent(0, width/2, height/2, 0, 0);
    std::cout << "  Sent mouse move to center" << std::endl;

    // Mouse click
    vm_postMouseEvent(1, width/2, height/2, 4, 0);  // button down
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    vm_postMouseEvent(2, width/2, height/2, 0, 0);  // button up
    std::cout << "  Sent mouse click at center" << std::endl;

    // Wait for response
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    int updatesAfterClick = displayUpdateCount.load();
    std::cout << "  Updates after click: " << updatesAfterClick << std::endl;

    // ===== PHASE 6: Stop and analyze =====
    std::cout << "\n[6] Stopping VM..." << std::endl;
    vm_stop();

    auto runEnd = std::chrono::steady_clock::now();
    auto runMs = std::chrono::duration_cast<std::chrono::milliseconds>(runEnd - runStart).count();

    // ===== RESULTS =====
    std::cout << "\n========================================" << std::endl;
    std::cout << "Results" << std::endl;
    std::cout << "========================================" << std::endl;

    int finalNonZero = countNonZeroPixels(pixels, width, height);
    double finalPct = 100.0 * finalNonZero / (width * height);

    std::cout << "  Run time: " << runMs << " ms" << std::endl;
    std::cout << "  Display updates: " << displayUpdateCount.load() << std::endl;
    std::cout << "  Final non-zero pixels: " << finalNonZero
              << " (" << std::fixed << std::setprecision(1) << finalPct << "%)" << std::endl;

    if (displayUpdateCount > 0) {
        std::cout << "  Last update region: (" << lastUpdateX << "," << lastUpdateY
                  << ") " << lastUpdateW << "x" << lastUpdateH << std::endl;
    }

    printPixelSample(pixels, width, height);

    // Verdict
    std::cout << "\n";
    bool passed = true;

    if (displayUpdateCount == 0) {
        std::cout << "WARNING: No display updates received" << std::endl;
        passed = false;
    }

    if (finalNonZero == 0) {
        std::cout << "WARNING: Display buffer is all zeros (blank)" << std::endl;
        passed = false;
    }

    if (passed) {
        std::cout << "TEST PASSED: Display pipeline is working" << std::endl;
    } else {
        std::cout << "TEST INCOMPLETE: Check display primitives" << std::endl;
    }

    return passed ? 0 : 1;
}
