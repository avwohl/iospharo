/*
 * test_load_image.cpp - Test loading a Pharo image
 *
 * Usage: ./test_load_image <path-to-image>
 */

#include "ObjectMemory.hpp"
#include "ImageLoader.hpp"
#include "Interpreter.hpp"
#include "../platform/DisplaySurface.hpp"
#include "../platform/EventQueue.hpp"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <vector>
#include <cstring>
#include <unordered_map>

using namespace pharo;

// Global interpreter pointer for event callback
static Interpreter* gTestInterpreter = nullptr;

// Event callback to signal input semaphore when events arrive
static void testEventCallback(void* context) {
    (void)context;
    if (gTestInterpreter) {
        int semIndex = gEventQueue.getInputSemaphoreIndex();
        if (semIndex <= 0) semIndex = 1;  // Default to semaphore 1 if not set
        gTestInterpreter->signalExternalSemaphore(semIndex);
    }
}

// Inject a mouse click event (down then up)
// button: 4=left (red), 2=right (yellow), 1=middle (blue)
static void injectMouseClick(int x, int y, int button, int modifiers = 0) {
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    // Mouse down event
    // Pharo event format: arg1=x, arg2=y, arg3=buttons, arg4=modifiers, arg5=subtype
    // Subtype: 1=mouse down, 2=mouse up, 3=mouse move
    Event downEvent;
    downEvent.type = static_cast<int>(EventType::Mouse);
    downEvent.timeStamp = static_cast<int>(ms & 0x7FFFFFFF);
    downEvent.arg1 = x;  // x position
    downEvent.arg2 = y;  // y position
    downEvent.arg3 = button;  // buttons pressed
    downEvent.arg4 = modifiers;  // modifiers
    downEvent.arg5 = 1;  // CRITICAL: subtype 1 = mouse down
    downEvent.windowIndex = 1;
    gEventQueue.push(downEvent);

    std::cout << "[TEST] Injected mouse DOWN at (" << x << "," << y
              << ") button=" << button << " subtype=" << downEvent.arg5 << std::endl;

    // Mouse up event (a few ms later)
    Event upEvent = downEvent;
    upEvent.timeStamp += 50;  // 50ms later
    upEvent.arg3 = 0;  // no buttons pressed
    upEvent.arg5 = 2;  // CRITICAL: subtype 2 = mouse up
    gEventQueue.push(upEvent);

    std::cout << "[TEST] Injected mouse UP at (" << x << "," << y
              << ") subtype=" << upEvent.arg5 << std::endl;
}

// Inject a mouse move event
static void injectMouseMove(int x, int y, int modifiers = 0) {
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    Event moveEvent;
    moveEvent.type = static_cast<int>(EventType::Mouse);
    moveEvent.timeStamp = static_cast<int>(ms & 0x7FFFFFFF);
    moveEvent.arg1 = x;
    moveEvent.arg2 = y;
    moveEvent.arg3 = 0;  // no buttons
    moveEvent.arg4 = modifiers;
    moveEvent.windowIndex = 1;
    gEventQueue.push(moveEvent);
}

// Test display surface for verifying Morphic rendering
class TestDisplaySurface : public DisplaySurface {
private:
    int width_;
    int height_;
    std::vector<uint32_t> pixels_;
    int updateCount_ = 0;
    uint32_t initialChecksum_ = 0;

public:
    TestDisplaySurface(int w, int h) : width_(w), height_(h), pixels_(w * h, 0xFF808080) {
        // Initialize with gray pattern so we can detect changes
        initialChecksum_ = checksum();
    }

    int width() const override { return width_; }
    int height() const override { return height_; }
    int depth() const override { return 32; }

    uint32_t* pixels() override { return pixels_.data(); }
    size_t pitch() const override { return width_ * sizeof(uint32_t); }

    void invalidateRect(int x, int y, int w, int h) override {
        // Could track dirty regions, but for testing we just note it
    }

    void update() override {
        updateCount_++;
    }

    int getUpdateCount() const { return updateCount_; }

    uint32_t checksum() const {
        uint32_t sum = 0;
        for (size_t i = 0; i < pixels_.size(); i += 100) {
            sum ^= pixels_[i];
            sum = (sum << 7) | (sum >> 25);
        }
        return sum;
    }

    bool hasPixelsChanged() const {
        return checksum() != initialChecksum_;
    }

    void printStats() const {
        std::cout << "\n=== Display Surface Stats ===" << std::endl;
        std::cout << "Size: " << width_ << "x" << height_ << std::endl;
        std::cout << "Update calls: " << updateCount_ << std::endl;
        std::cout << "Pixels changed: " << (hasPixelsChanged() ? "YES" : "NO") << std::endl;
        std::cout << "Initial checksum: 0x" << std::hex << initialChecksum_ << std::dec << std::endl;
        std::cout << "Current checksum: 0x" << std::hex << checksum() << std::dec << std::endl;

        // Sample some pixels to see what's there
        std::cout << "\nSample pixels:" << std::endl;
        int samples[][2] = {{0,0}, {10,35}, {100,100}, {width_/2, height_/2}, {width_-1, height_-1}};
        for (auto& s : samples) {
            int x = s[0], y = s[1];
            if (x < width_ && y < height_) {
                uint32_t p = pixels_[y * width_ + x];
                std::cout << "  [" << x << "," << y << "]: 0x" << std::hex << p << std::dec
                          << " (A=" << ((p >> 24) & 0xFF)
                          << " R=" << ((p >> 16) & 0xFF)
                          << " G=" << ((p >> 8) & 0xFF)
                          << " B=" << (p & 0xFF) << ")" << std::endl;
            }
        }

        // Count unique colors
        std::unordered_map<uint32_t, int> colorCounts;
        for (uint32_t p : pixels_) {
            colorCounts[p]++;
        }
        std::cout << "\nUnique colors: " << colorCounts.size() << std::endl;
        if (colorCounts.size() <= 10) {
            for (auto& [color, count] : colorCounts) {
                std::cout << "  0x" << std::hex << color << std::dec << ": " << count << " pixels" << std::endl;
            }
        }
    }
};

// Global test surface
static TestDisplaySurface* gTestSurface = nullptr;

void printHeader(const SpurImageHeader& header) {
    std::cout << "=== Image Header ===" << std::endl;
    std::cout << "Format:            " << header.imageFormat;
    if (header.imageFormat == 68021) std::cout << " (Spur 64-bit)";
    else if (header.imageFormat == 68533) std::cout << " (Spur 64-bit Sista)";
    std::cout << std::endl;

    std::cout << "Header size:       " << header.headerSize << " bytes" << std::endl;
    std::cout << "Image size:        " << header.imageBytes << " bytes ("
              << (header.imageBytes / 1024 / 1024) << " MB)" << std::endl;
    std::cout << "Old base address:  0x" << std::hex << header.startOfMemory
              << std::dec << std::endl;
    std::cout << "Special objects:   0x" << std::hex << header.specialObjectsOop
              << std::dec << std::endl;
    std::cout << "Last hash:         " << header.lastHash << std::endl;

    uint32_t width = static_cast<uint32_t>(header.screenSize >> 32);
    uint32_t height = static_cast<uint32_t>(header.screenSize & 0xFFFFFFFF);
    std::cout << "Screen size:       " << width << " x " << height << std::endl;

    std::cout << "Flags:             0x" << std::hex << header.imageHeaderFlags
              << std::dec << std::endl;
    if (header.imageHeaderFlags & 1) std::cout << "  - Full block closures" << std::endl;
    if (header.imageHeaderFlags & 8) std::cout << "  - Sista V1 bytecodes" << std::endl;

    // Segment info
    std::cout << "First segment:     " << header.firstSegmentBytes << " bytes" << std::endl;
    std::cout << "Free old space:    " << header.freeOldSpaceInImage << " bytes" << std::endl;
    std::cout << "Eden bytes:        " << header.edenBytes << std::endl;
}

void printSpecialObjects(ObjectMemory& memory) {
    std::cout << "\n=== Special Objects ===" << std::endl;

    // Note: In Smalltalk, nil/true/false are actual singleton objects,
    // not special pointer values. So nil.isNil() checks for null pointer (0),
    // but the nil singleton has a real address.

    Oop specialArray = memory.specialObjectsArray();
    std::cout << "Special objects array: 0x" << std::hex << specialArray.rawBits()
              << std::dec << std::endl;

    if (!specialArray.isObject()) {
        std::cout << "ERROR: Special objects array is not valid" << std::endl;
        return;
    }

    ObjectHeader* arrayHeader = specialArray.asObjectPtr();
    uint64_t rawHeader = arrayHeader->rawHeader();
    std::cout << "  Raw header: 0x" << std::hex << rawHeader << std::dec << std::endl;
    std::cout << "  Slot count byte: " << (rawHeader & 0xFF) << std::endl;
    std::cout << "  Hash: " << ((rawHeader >> 8) & 0x3FFFFF) << std::endl;
    std::cout << "  Format: " << ((rawHeader >> 30) & 0x1F) << std::endl;
    std::cout << "  Class index: " << ((rawHeader >> 35) & 0x3FFFFF) << std::endl;
    std::cout << "  Flags: " << ((rawHeader >> 57) & 0x7F) << std::endl;

    size_t slotCount = arrayHeader->slotCount();
    std::cout << "  Computed slot count: " << slotCount << std::endl;

    // Sanity check
    if (slotCount > 1000) {
        std::cout << "ERROR: Slot count too large, header may be corrupt" << std::endl;
        return;
    }

    // Print first few special objects
    std::cout << "\nFirst 20 special objects:" << std::endl;
    for (size_t i = 0; i < 20 && i < slotCount; i++) {
        Oop obj = arrayHeader->slotAt(i);
        std::cout << "  [" << i << "]: 0x" << std::hex << obj.rawBits() << std::dec;
        if (obj.isSmallInteger()) {
            std::cout << " (SmallInteger: " << obj.asSmallInteger() << ")";
        } else if (obj.rawBits() == 0) {
            std::cout << " (nil/zero)";
        } else if (obj.isObject()) {
            // Check if address looks valid
            uint64_t addr = obj.rawBits() & ~7ULL;
            if (memory.isValidHeapAddress(reinterpret_cast<void*>(addr))) {
                std::cout << " (object, class: " << obj.asObjectPtr()->classIndex() << ")";
            } else {
                std::cout << " (INVALID ADDRESS)";
            }
        }
        std::cout << std::endl;
    }
}

void printMemoryStats(ObjectMemory& memory) {
    std::cout << "\n=== Memory Layout ===" << std::endl;
    std::cout << "Old space:  0x" << std::hex
              << reinterpret_cast<uint64_t>(memory.oldSpaceStart())
              << " - 0x"
              << reinterpret_cast<uint64_t>(memory.oldSpaceEnd())
              << std::dec << std::endl;

    auto stats = memory.statistics();
    std::cout << "Allocated:  " << (stats.bytesAllocated / 1024 / 1024) << " MB" << std::endl;
    std::cout << "Free:       " << (stats.bytesFree / 1024 / 1024) << " MB" << std::endl;

    // Debug: show first few bytes at start of old space
    std::cout << "\nFirst 128 bytes of old space (raw):" << std::endl;
    uint64_t* ptr = reinterpret_cast<uint64_t*>(memory.oldSpaceStart());
    for (int i = 0; i < 16; i++) {
        std::cout << "  [" << (i * 8) << "]: 0x" << std::hex << ptr[i] << std::dec << std::endl;
    }

    // Try to interpret first object
    ObjectHeader* firstObj = reinterpret_cast<ObjectHeader*>(memory.oldSpaceStart());
    uint64_t header = firstObj->rawHeader();
    std::cout << "\nFirst object header analysis:" << std::endl;
    std::cout << "  Raw: 0x" << std::hex << header << std::dec << std::endl;
    if (header != 0) {
        std::cout << "  Slot count: " << (header & 0xFF) << std::endl;
        std::cout << "  Format: " << ((header >> 30) & 0x1F) << std::endl;
        std::cout << "  Class index: " << ((header >> 35) & 0x3FFFFF) << std::endl;
    }
}

void countObjects(ObjectMemory& memory) {
    std::cout << "\n=== Object Statistics ===" << std::endl;

    size_t totalObjects = 0;
    size_t pointerObjects = 0;
    size_t byteObjects = 0;
    size_t methodObjects = 0;
    size_t totalBytes = 0;

    memory.allObjectsDo([&](Oop obj) {
        if (!obj.isObject()) return;

        ObjectHeader* header = obj.asObjectPtr();
        totalObjects++;
        size_t objSize = header->totalSize();

        // Debug: catch objects with suspicious sizes
        if (objSize > 100 * 1024 * 1024) {  // > 100 MB
            std::cout << "WARNING: Huge object at 0x" << std::hex
                      << reinterpret_cast<uint64_t>(header) << std::dec
                      << " size=" << objSize
                      << " slots=" << header->slotCount()
                      << " format=" << static_cast<int>(header->format())
                      << " raw=0x" << std::hex << header->rawHeader() << std::dec
                      << std::endl;
        }

        totalBytes += objSize;

        if (header->isBytesObject()) {
            byteObjects++;
        } else if (header->isCompiledMethod()) {
            methodObjects++;
        } else if (header->isPointersObject()) {
            pointerObjects++;
        }
    });

    std::cout << "Total objects:    " << totalObjects << std::endl;
    std::cout << "Pointer objects:  " << pointerObjects << std::endl;
    std::cout << "Byte objects:     " << byteObjects << std::endl;
    std::cout << "Compiled methods: " << methodObjects << std::endl;
    std::cout << "Total size:       " << (totalBytes / 1024 / 1024) << " MB" << std::endl;
}

void testOopTagging() {
    std::cout << "\n=== Oop Tagging Tests ===" << std::endl;

    // Test SmallInteger
    Oop si = Oop::fromSmallInteger(42);
    std::cout << "SmallInteger 42:  0x" << std::hex << si.rawBits() << std::dec;
    std::cout << " isSmallInteger=" << si.isSmallInteger();
    std::cout << " value=" << si.asSmallInteger() << std::endl;

    // Test negative SmallInteger
    Oop neg = Oop::fromSmallInteger(-100);
    std::cout << "SmallInteger -100: 0x" << std::hex << neg.rawBits() << std::dec;
    std::cout << " value=" << neg.asSmallInteger() << std::endl;

    // Test Character
    Oop ch = Oop::fromCharacter('A');
    std::cout << "Character 'A':    0x" << std::hex << ch.rawBits() << std::dec;
    std::cout << " isCharacter=" << ch.isCharacter();
    std::cout << " codepoint=" << ch.asCharacter() << std::endl;

    // Test nil
    Oop nil = Oop::nil();
    std::cout << "nil:              0x" << std::hex << nil.rawBits() << std::dec;
    std::cout << " isNil=" << nil.isNil() << std::endl;

    // Test SmallFloat encoding/decoding
    std::cout << "\nSmallFloat tests:" << std::endl;
    Oop sf;
    double testValues[] = {1.0, 168.0, 80.0, -42.5, 0.0, 1024.0, 0.5};
    for (double val : testValues) {
        if (Oop::tryFromSmallFloat(val, sf)) {
            double decoded = sf.asSmallFloat();
            std::cout << "  " << val << " -> 0x" << std::hex << sf.rawBits() << std::dec
                      << " -> " << decoded;
            if (decoded == val) std::cout << " OK";
            else std::cout << " FAIL";
            std::cout << std::endl;
        } else {
            std::cout << "  " << val << " cannot be encoded as SmallFloat" << std::endl;
        }
    }

    // Test decoding known Pharo values (tag 5)
    std::cout << "\nDecoding known SmallFloat values from Pharo:" << std::endl;
    uint64_t known[] = {0x8540000000000005, 0x8650000000000005, 0x8000000000000005};
    for (uint64_t bits : known) {
        Oop oop = Oop::nil();
        // Manually create Oop from raw bits for testing
        Oop* oopPtr = &oop;
        std::memcpy(oopPtr, &bits, sizeof(bits));
        if (oop.isSmallFloat()) {
            std::cout << "  0x" << std::hex << bits << std::dec
                      << " -> " << oop.asSmallFloat() << std::endl;
        } else {
            std::cout << "  0x" << std::hex << bits << std::dec
                      << " is not SmallFloat (tag=" << (bits & 7) << ")" << std::endl;
        }
    }

    // Verify tagging uses low bits (iOS ASLR compatible)
    std::cout << "\nTag verification (should use low 3 bits only):" << std::endl;
    std::cout << "  SmallInteger tag: " << (si.rawBits() & 7) << " (expected 1)" << std::endl;
    std::cout << "  Character tag:    " << (ch.rawBits() & 7) << " (expected 3)" << std::endl;
}

int main(int argc, char* argv[]) {
    std::cout << "Pharo Clean VM - Image Loader Test" << std::endl;
    std::cout << "===================================" << std::endl;

    // Run tagging tests first
    testOopTagging();

    if (argc < 2) {
        std::cout << "\nUsage: " << argv[0] << " <path-to-image>" << std::endl;
        std::cout << "\nNo image specified, skipping image load test." << std::endl;
        return 0;
    }

    const char* imagePath = argv[1];
    std::cout << "\nLoading image: " << imagePath << std::endl;

    // Initialize memory (256 MB should be enough for most images)
    ObjectMemory memory;
    MemoryConfig config;
    config.oldSpaceSize = 256 * 1024 * 1024;  // 256 MB
    config.newSpaceSize = 32 * 1024 * 1024;   // 32 MB
    config.permSpaceSize = 8 * 1024 * 1024;   // 8 MB

    if (!memory.initialize(config)) {
        std::cerr << "ERROR: Failed to initialize memory" << std::endl;
        return 1;
    }
    std::cout << "Memory initialized successfully" << std::endl;

    // Load the image
    ImageLoader loader;
    LoadResult result = loader.load(imagePath, memory);

    if (!result.success) {
        std::cerr << "ERROR: Failed to load image: " << result.error << std::endl;
        return 1;
    }

    std::cout << "Image loaded successfully!" << std::endl;

    // Print information
    printHeader(loader.header());
    printSpecialObjects(memory);
    printMemoryStats(memory);
    countObjects(memory);

    // Check for iOS-specific classes
    std::cout << "\n=== Checking iOS Classes ===" << std::endl;
    const char* iOSClasses[] = {"OSWorldRenderer", "OSiOSDriver", "OSiOSBackendWindow",
                                 "NullWorldRenderer", "OSWindowDriver", "OSWindow",
                                 "WorldMorph", "WorldState", "Display"};
    for (const char* className : iOSClasses) {
        Oop cls = memory.findGlobal(className);
        std::cout << "  " << className << ": " << (cls.isNil() ? "NOT FOUND" : "found") << std::endl;
    }

    // Create test display surface for Morphic rendering
    std::cout << "\n=== Display Surface Setup ===" << std::endl;
    gTestSurface = new TestDisplaySurface(1024, 768);
    gDisplaySurface = gTestSurface;  // Set global for VM to use
    std::cout << "Created " << gTestSurface->width() << "x" << gTestSurface->height()
              << " test display surface" << std::endl;

    // Try to initialize interpreter
    std::cout << "\n=== Interpreter Initialization ===" << std::endl;
    Interpreter interpreter(memory);

    // Set up event callback BEFORE initialization
    gTestInterpreter = &interpreter;
    gEventQueue.setEventCallback(testEventCallback, nullptr);
    std::cout << "Event callback registered" << std::endl;

    if (interpreter.initialize()) {
        std::cout << "Interpreter initialized successfully!" << std::endl;
        std::cout << "Active method: 0x" << std::hex
                  << interpreter.activeMethod().rawBits() << std::dec << std::endl;

        // Create Display Form if it doesn't exist (image may be headless)
        std::cout << "\n=== Creating Display ===" << std::endl;
        interpreter.ensureDisplayForm(1024, 768, 32);

        // Verify Display was created
        Oop display = memory.findGlobal("Display");
        std::cout << "Display after ensureDisplayForm: "
                  << (display.isNil() ? "NOT FOUND" : "created!") << std::endl;

        // Direct BitBlt test - try to fill Display with a color
        if (!display.isNil() && display.isObject()) {
            std::cout << "\n=== Direct BitBlt Test ===" << std::endl;
            // Get Display's bits (Form layout: bits, width, height, depth)
            Oop bits = memory.fetchPointer(0, display);
            Oop width = memory.fetchPointer(1, display);
            Oop height = memory.fetchPointer(2, display);
            Oop depth = memory.fetchPointer(3, display);
            std::cout << "Display bits: " << (bits.isObject() ? "object" : bits.isSmallInteger() ? "int" : "other")
                      << " width: " << (width.isSmallInteger() ? width.asSmallInteger() : -1)
                      << " height: " << (height.isSmallInteger() ? height.asSmallInteger() : -1)
                      << " depth: " << (depth.isSmallInteger() ? depth.asSmallInteger() : -1) << std::endl;

            // If bits is a Bitmap object, fill the entire display with a gradient
            if (bits.isObject()) {
                ObjectHeader* bitsHdr = bits.asObjectPtr();
                std::cout << "Bitmap format: " << static_cast<int>(bitsHdr->format())
                          << " byteSize: " << bitsHdr->byteSize() << std::endl;
                // Format 9 is 64-bit indexable, 10 is 32-bit indexable
                if (bitsHdr->format() == pharo::ObjectFormat::Indexable32 ||
                    bitsHdr->format() == pharo::ObjectFormat::Indexable64) {
                    size_t byteCount = bitsHdr->byteSize();
                    size_t pixels = byteCount / 4;  // 4 bytes per pixel for 32-bit depth
                    std::cout << "Filling display with gradient (" << pixels << " pixels)..." << std::endl;
                    uint32_t* pixelData = reinterpret_cast<uint32_t*>(bitsHdr->bytes());
                    // Create a gradient to prove full-screen rendering works
                    for (size_t i = 0; i < pixels; i++) {
                        int x = i % 1024;
                        int y = i / 1024;
                        // Simple gradient: red increases with x, green with y
                        uint8_t r = static_cast<uint8_t>((x * 255) / 1024);
                        uint8_t g = static_cast<uint8_t>((y * 255) / 768);
                        uint8_t b = 128;
                        pixelData[i] = 0xFF000000 | (r << 16) | (g << 8) | b;
                    }
                    std::cout << "Wrote gradient to display bitmap" << std::endl;
                }
            }
        }

        // Start the heartbeat thread for display sync
        std::cout << "Starting heartbeat thread..." << std::endl;
        interpreter.startHeartbeat();

        // Run bytecode steps for testing
        std::cout << "\n=== Execution Test ===" << std::endl;
        auto execStart = std::chrono::steady_clock::now();
        int totalSteps = 10000000;  // Run 10M steps to allow startup to complete
        std::cout << "Running up to " << totalSteps << " bytecode steps..." << std::endl;
        int activeSteps = 0;
        int idleSteps = 0;
        bool clickInjected = false;
        int clickResponseSteps = 0;

        for (int i = 0; i < totalSteps; i++) {
            bool result = interpreter.step();


            // Progress report every 100k steps
            if (i > 0 && i % 100000 == 0) {
                std::cout << "[PROGRESS] Step " << i << ": active=" << activeSteps
                          << " idle=" << idleSteps << " result=" << result << std::endl;
            }

            // Report every 10000 steps (or 1000 after step 10000 for debugging)
            if (i > 0 && (i % 10000 == 0 || (i > 10000 && i % 1000 == 0))) {
                static auto start = std::chrono::steady_clock::now();
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
                fprintf(stderr, "[STEP %d] active=%d total_time=%lldms\n", i, activeSteps, elapsed);
            }

            if (result) {
                activeSteps++;
                idleSteps = 0;  // Reset consecutive idle count

                // After 5k active steps, inject a right-click to trigger world menu
                // (Injecting earlier so the render loop is still active)
                if (!clickInjected && activeSteps == 5000) {
                    std::cout << "\n=== Injecting Right-Click (World Menu) ===" << std::endl;
                    // Clear the DNU trace file first
                    FILE* dnu = fopen("/tmp/dnu_trace.log", "w");
                    if (dnu) {
                        fprintf(dnu, "=== DNU Trace for Right-Click Test ===\n");
                        fclose(dnu);
                    }
                    // Right-click (yellow button = 2) on World BACKGROUND
                    // The Welcome window is at bounds [138,57,838,607], so click outside it
                    // Click at (50, 300) which is to the LEFT of the window
                    injectMouseClick(50, 300, 2);
                    clickInjected = true;
                    clickResponseSteps = 0;
                }

                // Count steps after click injection
                if (clickInjected) {
                    clickResponseSteps++;
                    // After processing click for a while, print status
                    if (clickResponseSteps == 100000) {
                        std::cout << "\n=== Click Response Complete (100k steps) ===" << std::endl;
                    }
                    // After 200k steps of click processing, inject a left click
                    if (clickResponseSteps == 200000) {
                        std::cout << "\n=== Injecting Left-Click ===" << std::endl;
                        injectMouseClick(512, 384, 4);  // Left click (red button = 4)
                    }
                }
            } else {
                idleSteps++;
                // Report when we start getting idle
                if (idleSteps == 1) {
                    std::cout << "[IDLE] Started at step " << i << " after " << activeSteps << " active steps" << std::endl;
                }
                // If we get too many consecutive idle steps, stop
                if (idleSteps > 1000) {
                    std::cout << "Interpreter stopped (1000 consecutive idle steps) at step " << i << std::endl;
                    break;
                }
            }
        }
        std::cout << "\n=== Execution Summary ===" << std::endl;
        std::cout << "Active bytecode steps: " << activeSteps << std::endl;
        std::cout << "Steps after click: " << clickResponseSteps << std::endl;

        // Stop the heartbeat thread
        std::cout << "Stopping heartbeat thread..." << std::endl;
        interpreter.stopHeartbeat();
    } else {
        std::cout << "Interpreter initialization failed (may need process setup)" << std::endl;
    }

    // Print display surface stats
    if (gTestSurface) {
        gTestSurface->printStats();
        delete gTestSurface;
        gTestSurface = nullptr;
        gDisplaySurface = nullptr;
    }

    std::cout << "\n=== Test Complete ===" << std::endl;
    return 0;
}
