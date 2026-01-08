/*
 * test_platform.cpp - Test the platform bridge
 */

#include "PlatformBridge.h"
#include <iostream>
#include <thread>
#include <chrono>

void displayCallback(int x, int y, int w, int h, void* context) {
    std::cout << "Display update: " << x << "," << y << " " << w << "x" << h << std::endl;
}

int main(int argc, char* argv[]) {
    std::cout << "Platform Bridge Test" << std::endl;
    std::cout << "====================" << std::endl;

    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " <image-path>" << std::endl;
        return 1;
    }

    // Initialize VM
    std::cout << "Initializing VM with 256MB heap..." << std::endl;
    if (!vm_initialize(256 * 1024 * 1024)) {
        std::cerr << "Failed to initialize VM" << std::endl;
        return 1;
    }
    std::cout << "VM initialized" << std::endl;

    // Set up display
    std::cout << "Setting up 1024x768 display..." << std::endl;
    vm_setDisplaySize(1024, 768, 32);
    vm_setDisplayUpdateCallback(displayCallback, nullptr);

    uint32_t* pixels = vm_getDisplayPixels();
    std::cout << "Pixel buffer at: " << (void*)pixels << std::endl;
    std::cout << "Display: " << vm_getDisplayWidth() << "x" << vm_getDisplayHeight() << std::endl;

    // Load image
    std::cout << "Loading image: " << argv[1] << std::endl;
    if (!vm_loadImage(argv[1])) {
        std::cerr << "Failed to load image" << std::endl;
        return 1;
    }
    std::cout << "Image loaded" << std::endl;

    // Run VM for a short time
    std::cout << "Starting VM..." << std::endl;
    vm_run();

    // Wait a bit
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Stop
    std::cout << "Stopping VM..." << std::endl;
    vm_stop();

    std::cout << "Test complete" << std::endl;
    return 0;
}
