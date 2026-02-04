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
#include <csignal>
#include <execinfo.h>
#include <unistd.h>
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

        // Dump framebuffer to PPM file
        const char* ppmPath = "/tmp/pharo-display.ppm";
        FILE* f = fopen(ppmPath, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", width_, height_);
            for (int i = 0; i < width_ * height_; i++) {
                uint32_t p = pixels_[i];
                uint8_t rgb[3] = {
                    (uint8_t)((p >> 16) & 0xFF),
                    (uint8_t)((p >> 8) & 0xFF),
                    (uint8_t)(p & 0xFF)
                };
                fwrite(rgb, 1, 3, f);
            }
            fclose(f);
            std::cout << "\nFramebuffer saved to " << ppmPath << std::endl;
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

static void sigsegvAction(int sig, siginfo_t* info, void* ctx) {
    fprintf(stderr, "\n[SIGSEGV] Signal %d caught! Fault addr=%p\n", sig, info->si_addr);
    void* callstack[128];
    int frames = backtrace(callstack, 128);
    backtrace_symbols_fd(callstack, frames, 2);
    _exit(139);
}

int main(int argc, char* argv[]) {
    struct sigaction sa;
    sa.sa_sigaction = sigsegvAction;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    std::cout << "Pharo Clean VM - Image Loader Test" << std::endl;
    std::cout << "===================================" << std::endl;

    // Run tagging tests first
    testOopTagging();

    if (argc < 2) {
        std::cout << "\nUsage: " << argv[0] << " [--test] <path-to-image>" << std::endl;
        std::cout << "\nNo image specified, skipping image load test." << std::endl;
        return 0;
    }

    // Parse --test flag
    bool testMode = false;
    const char* imagePath = nullptr;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--test") == 0) {
            testMode = true;
        } else {
            imagePath = argv[i];
        }
    }
    if (!imagePath) {
        std::cerr << "ERROR: No image path specified" << std::endl;
        return 1;
    }
    if (testMode) {
        std::cout << "\n*** TEST MODE: Will run SUnit tests and write results to /tmp/sunit_test_results.txt ***" << std::endl;
        // Remove stale results file
        std::remove("/tmp/sunit_test_results.txt");
    }
    std::cout << "\nLoading image: " << imagePath << std::endl;

    // Initialize memory (256 MB should be enough for most images)
    ObjectMemory memory;
    MemoryConfig config;
    config.oldSpaceSize = 8ULL * 1024 * 1024 * 1024;  // 8 GB (GC is disabled)
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
    interpreter.setImageName(imagePath);
    interpreter.setVMPath(argv[0]);
    interpreter.setTestMode(testMode);

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
            // Debug: show raw slot values
            std::cout << "  Raw slots: bits=0x" << std::hex << bits.rawBits()
                      << " width=0x" << width.rawBits()
                      << " height=0x" << height.rawBits()
                      << " depth=0x" << depth.rawBits() << std::dec << std::endl;
            std::cout << "  Slot types: bits=" << (bits.isObject() ? "obj" : bits.isSmallInteger() ? "smi" : "other")
                      << " width=" << (width.isObject() ? "obj" : width.isSmallInteger() ? "smi" : "other")
                      << " height=" << (height.isObject() ? "obj" : height.isSmallInteger() ? "smi" : "other")
                      << " depth=" << (depth.isObject() ? "obj" : depth.isSmallInteger() ? "smi" : "other") << std::endl;

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

        // Delay heartbeat start - start it after 1.6M steps to avoid
        // timer preemption corrupting startup process state
        std::cout << "Heartbeat will start after startup completes..." << std::endl;
        bool heartbeatStarted = false;

        // Run bytecode steps for testing
        std::cout << "\n=== Execution Test ===" << std::endl;
        auto execStart = std::chrono::steady_clock::now();
        int totalSteps = testMode ? 200000000 : 30000000;  // 200M for tests, 30M for quick test
        std::cout << "Running up to " << totalSteps << " bytecode steps..." << std::endl;
        if (testMode) {
            std::cout << "Test mode: will stop when /tmp/sunit_test_results.txt contains ALL TESTS COMPLETE" << std::endl;
        }
        int activeSteps = 0;
        int idleSteps = 0;
        bool clickInjected = false;
        int clickResponseSteps = 0;

        for (int i = 0; i < totalSteps; i++) {
            // Start heartbeat after startup completes
            if (!heartbeatStarted && i == 2000000) {
                std::cout << "Starting heartbeat thread (post-startup)..." << std::endl;
                interpreter.startHeartbeat();
                heartbeatStarted = true;
            }
            bool result = interpreter.step();


            // Progress report every 10M steps
            if (i > 0 && i % 10000000 == 0) {
                std::cout << "[PROGRESS] Step " << i << ": active=" << activeSteps
                          << " idle=" << idleSteps << " result=" << result << std::endl;
            }

            // Report every 10M steps
            if (i > 0 && i % 10000000 == 0) {
                static auto start = std::chrono::steady_clock::now();
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
                fprintf(stderr, "[STEP %d] active=%d total_time=%lldms\n", i, activeSteps, elapsed);
            }

            static int totalIdleSteps = 0;
            static bool wasActive = false;
            if (result) {
                activeSteps++;
                idleSteps = 0;  // Reset consecutive idle count
                wasActive = true;

                // In test mode, check for test completion during active steps too
                if (testMode && activeSteps % 1000000 == 0) {
                    FILE* rf = fopen("/tmp/sunit_test_results.txt", "r");
                    if (rf) {
                        fseek(rf, 0, SEEK_END);
                        long fsize = ftell(rf);
                        if (fsize > 18) {  // "ALL TESTS COMPLETE" is 18 chars
                            fseek(rf, fsize - 30, SEEK_SET);
                            char buf[64] = {0};
                            fread(buf, 1, 30, rf);
                            fclose(rf);
                            if (strstr(buf, "ALL TESTS COMPLETE")) {
                                std::cout << "\n[TEST] SUnit test results complete!" << std::endl;
                                FILE* results = fopen("/tmp/sunit_test_results.txt", "r");
                                if (results) {
                                    std::cout << "\n=== SUnit Test Results ===" << std::endl;
                                    char line[256];
                                    while (fgets(line, sizeof(line), results)) {
                                        std::cout << line;
                                    }
                                    fclose(results);
                                    std::cout << "=========================" << std::endl;
                                }
                                goto done;
                            }
                        } else {
                            fclose(rf);
                        }
                    }
                }

                // Process dumps at key intervals to track MorphicRenderLoop
                if (activeSteps % 20000000 == 0) {
                    std::cerr << "\n=== PROCESS DUMP AT " << (activeSteps/1000000) << "M STEPS ===" << std::endl;
                    interpreter.dumpProcessQueues();
                }

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
                // Detect transition from active to idle (limit dumps to avoid flooding)
                if (wasActive) {
                    static int idleTransitions = 0;
                    idleTransitions++;
                    if (idleTransitions <= 5 || idleTransitions % 100 == 0) {
                        std::cerr << "[WENT-IDLE #" << idleTransitions << " step=" << i << " active=" << activeSteps << "]\n";
                        interpreter.dumpProcessQueues();
                    }
                }
                wasActive = false;
                idleSteps++;
                totalIdleSteps++;
                // Report when we first go idle
                if (totalIdleSteps == 1) {
                    std::cout << "[IDLE] First idle at step " << i << " after " << activeSteps << " active steps" << std::endl;
                    interpreter.dumpProcessQueues();
                }
                // Check for test results file periodically
                if (totalIdleSteps % 1000000 == 0) {
                    // Check for SUnit test results (test mode)
                    if (testMode) {
                        FILE* rf = fopen("/tmp/sunit_test_results.txt", "r");
                        if (rf) {
                            // Read last 30 bytes to check for completion marker
                            fseek(rf, 0, SEEK_END);
                            long fsize = ftell(rf);
                            if (fsize > 0) {
                                long readFrom = (fsize > 30) ? fsize - 30 : 0;
                                fseek(rf, readFrom, SEEK_SET);
                                char buf[64] = {0};
                                fread(buf, 1, 30, rf);
                                fclose(rf);
                                if (strstr(buf, "ALL TESTS COMPLETE")) {
                                    std::cout << "\n[TEST] SUnit test results complete!" << std::endl;
                                    // Print the full results
                                    FILE* results = fopen("/tmp/sunit_test_results.txt", "r");
                                    if (results) {
                                        std::cout << "\n=== SUnit Test Results ===" << std::endl;
                                        char line[256];
                                        while (fgets(line, sizeof(line), results)) {
                                            std::cout << line;
                                        }
                                        fclose(results);
                                        std::cout << "=========================" << std::endl;
                                    }
                                    break;
                                }
                            } else {
                                fclose(rf);
                            }
                        }
                    }
                    // Check for other diagnostic results
                    FILE* rf = fopen("/tmp/diag_trace.txt", "r");
                    if (!rf) rf = fopen("/tmp/inline_results.txt", "r");
                    if (rf) {
                        char buf[64] = {0};
                        fseek(rf, -20, SEEK_END);
                        fread(buf, 1, 20, rf);
                        fclose(rf);
                        if (strstr(buf, "DONE")) {
                            std::cout << "[TEST] Results file complete! Breaking." << std::endl;
                            break;
                        }
                    }
                }
                // If we have 500M+ total idle steps and step count > 500M, we're truly idle
                if (totalIdleSteps > 500000000 && i > 500000000) {
                    std::cout << "[IDLE] Detected relinquish-based idle at step " << i
                              << " (" << totalIdleSteps << " total idle steps, " << activeSteps << " active)" << std::endl;
                    // Dump process scheduler queues
                    interpreter.dumpProcessQueues();
                    break;
                }
            }
        }
        done:
        std::cout << "\n=== Execution Summary ===" << std::endl;
        std::cout << "Active bytecode steps: " << activeSteps << std::endl;
        std::cout << "Steps after click: " << clickResponseSteps << std::endl;

        // Check if Display form bits were modified
        {
            Oop display = interpreter.displayForm();
            if (display.isObject() && display.rawBits() > 0x10000) {
                Oop bits = memory.fetchPointer(0, display);
                if (bits.isObject() && bits.rawBits() > 0x10000) {
                    ObjectHeader* bitsHdr = bits.asObjectPtr();
                    uint32_t* pixels = (uint32_t*)bitsHdr->bytes();
                    size_t pixelCount = bitsHdr->byteSize() / 4;
                    // Sample some pixels to check if display was updated
                    std::cout << "\n=== Display Check ===" << std::endl;
                    std::cout << "Pixel count: " << pixelCount << std::endl;
                    // Check first row, middle, and bottom
                    if (pixelCount > 1024*384) {
                        std::cout << "pixel[0,0]   = 0x" << std::hex << pixels[0] << std::dec << std::endl;
                        std::cout << "pixel[512,0] = 0x" << std::hex << pixels[512] << std::dec << std::endl;
                        std::cout << "pixel[0,384] = 0x" << std::hex << pixels[1024*384] << std::dec << std::endl;
                        std::cout << "pixel[512,384]= 0x" << std::hex << pixels[1024*384+512] << std::dec << std::endl;
                    }
                    // Count non-gradient pixels (gradient was R=x%256, G=y%256, B=128)
                    int modified = 0;
                    for (size_t i = 0; i < pixelCount && i < 100; i++) {
                        int x = i % 1024;
                        int y = i / 1024;
                        uint32_t expected = 0xFF000000 | ((x & 0xFF) << 16) | ((y & 0xFF) << 8) | 128;
                        if (pixels[i] != expected) modified++;
                    }
                    std::cout << "First 100 pixels: " << modified << " modified from gradient" << std::endl;
                }
            }
        }

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
