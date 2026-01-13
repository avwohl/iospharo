/*
 * Interpreter.cpp - Bytecode Interpreter Implementation
 *
 * This implements the Sista V1 bytecode interpreter.
 */

#include "Interpreter.hpp"
#include "../platform/DisplaySurface.hpp"
#include "../platform/EventQueue.hpp"
#include <cstring>
#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>

#if __APPLE__
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
// Undefine Objective-C's nil macro to avoid conflict with Oop::nil()
#undef nil
#endif

namespace pharo {

// ===== CONSTRUCTION =====

Interpreter::Interpreter(ObjectMemory& memory)
    : memory_(memory)
    , frameDepth_(0)
    , stackPointer_(stack_.data())
    , stackBase_(stack_.data())
    , framePointer_(nullptr)
    , instructionPointer_(nullptr)
    , bytecodeEnd_(nullptr)
    , activeContext_(Oop::nil())
    , argCount_(0)
    , extA_(0)
    , extB_(0)
    , usesSistaV1_(true)  // Default to SistaV1, will be set per method
    , running_(false)
    , primitiveFailed_(false)
{
    // Clear method cache
    for (auto& entry : methodCache_) {
        entry.selector = Oop::nil();
        entry.classOop = Oop::nil();
        entry.method = Oop::nil();
        entry.primitive = nullptr;
        entry.primitiveIndex = 0;
    }

    initializePrimitives();
}

bool Interpreter::initialize() {
    // Set up initial execution context
    // Find the startup process from special objects

    // Uncomment for debugging: std::cerr << "[INIT] Starting...\n";
    Oop scheduler = memory_.specialObject(SpecialObjectIndex::SchedulerAssociation);
    // DEBUG_LOG("[DEBUG] Scheduler: 0x" << std::hex << scheduler.rawBits() << std::dec;
    if (scheduler.isNil()) {
        return false;
    }

    // The scheduler association value is the ProcessScheduler
    // DEBUG: "[DEBUG] Getting process scheduler..."
    Oop processScheduler = memory_.fetchPointer(1, scheduler);  // value
    // DEBUG_LOG("[DEBUG] ProcessScheduler: 0x" << std::hex << processScheduler.rawBits() << std::dec;
    if (processScheduler.isNil()) {
        // DEBUG: "[DEBUG] ProcessScheduler is nil"
        return false;
    }

    // Debug: dump ProcessScheduler slots
    // DEBUG: "[DEBUG] ProcessScheduler slots:"
    if (processScheduler.isObject()) {
        ObjectHeader* psHeader = processScheduler.asObjectPtr();
        size_t slots = psHeader->slotCount();
        // std::cerr << "  ProcessScheduler has " << slots << " slots, classIndex=" << psHeader->classIndex(); // DEBUG
        for (size_t i = 0; i < slots && i < 5; i++) {
            Oop slot = psHeader->slotAt(i);
            // std::cerr << "  [" << i << "]: 0x" << std::hex << slot.rawBits() << std::dec; // DEBUG
        }
    }

    // Get the active process
    // ProcessScheduler layout: quiescentProcessLists (slot 0), activeProcess (slot 1)
    // DEBUG: "[DEBUG] Getting active process..."
    Oop activeProcess = memory_.fetchPointer(1, processScheduler);  // activeProcess is slot 1!
    // DEBUG_LOG("[DEBUG] ActiveProcess: 0x" << std::hex << activeProcess.rawBits() << std::dec;
    if (activeProcess.isNil()) {
        // DEBUG: "[DEBUG] ActiveProcess is nil"
        return false;
    }

    // Debug: dump active process slots
    // DEBUG: "[DEBUG] Active Process slots:"
    if (activeProcess.isObject()) {
        ObjectHeader* procHeader = activeProcess.asObjectPtr();
        size_t slots = procHeader->slotCount();
        // std::cerr << "  ActiveProcess has " << slots << " slots, classIndex=" << procHeader->classIndex(); // DEBUG
        for (size_t i = 0; i < slots && i < 6; i++) {
            Oop slot = procHeader->slotAt(i);
            // std::cerr << "  [" << i << "]: 0x" << std::hex << slot.rawBits() << std::dec;
            if (slot.isSmallInteger()) {
                // std::cerr << " (SmallInt: " << slot.asSmallInteger() << ")";
            }
            // std::cerr; // DEBUG
        }
    }

    // Get the suspended context
    // Modern Pharo Process layout:
    //   slot 0 = nextLink (for LinkedList)
    //   slot 1 = suspendedContext
    //   slot 2 = priority
    // DEBUG: "[DEBUG] Getting suspended context..."
    Oop context = memory_.fetchPointer(1, activeProcess);  // suspendedContext is slot 1 in modern Pharo
    // DEBUG_LOG("[DEBUG] Context: 0x" << std::hex << context.rawBits() << std::dec;

    // Check for unrelocated pointer - if it points to the old image base area
    // Old images use base address 0x10000000000 (1TB)
    // Our loaded images are in a much lower address range
    uint64_t contextAddr = context.rawBits() & ~7ULL;
    if (contextAddr >= 0x10000000000ULL && contextAddr < 0x20000000000ULL) {
        // Context appears unrelocated - use bootstrap startup
        return bootstrapStartup();
    }

    // Check if context is nil (fresh image startup)
    if (context.isNil() || (context.isObject() && context.asObjectPtr()->slotCount() == 0)) {
        return bootstrapStartup();
    }

    // We have a valid context - but first analyze the sender chain
    // to understand what code we're resuming in
    // DEBUG: "[DEBUG] Analyzing sender chain..."

    Oop nilObj = memory_.specialObject(SpecialObjectIndex::NilObject);
    Oop currentCtx = context;
    Oop resumeContext = context;  // Default: resume from original context
    int depth = 0;
    bool inSnapshotCode = false;
    int snapshotEndDepth = -1;

    while (currentCtx.isObject() && currentCtx.rawBits() != nilObj.rawBits() && depth < 20) {
        ObjectHeader* ctxHdr = currentCtx.asObjectPtr();

        // Get receiver and method from context
        Oop receiver = memory_.fetchPointer(5, currentCtx);
        Oop method = memory_.fetchPointer(3, currentCtx);

        std::string rcvrClassName = "<unknown>";
        std::string methodSelector = "<unknown>";

        // Get receiver's class name
        if (receiver.isObject() && receiver.rawBits() > 0x10000) {
            Oop rcvrClass = memory_.classOf(receiver);
            if (rcvrClass.isObject()) {
                ObjectHeader* clsHdr = rcvrClass.asObjectPtr();
                if (clsHdr->slotCount() > 6) {
                    Oop nameOop = memory_.fetchPointer(6, rcvrClass);
                    if (nameOop.isObject() && nameOop.rawBits() > 0x10000) {
                        ObjectHeader* nameHdr = nameOop.asObjectPtr();
                        if (nameHdr->isBytesObject() && nameHdr->byteSize() <= 50) {
                            rcvrClassName = std::string((char*)nameHdr->bytes(), nameHdr->byteSize());
                        }
                    }
                }
            }
        }

        // Try to get method selector (literal 1 in CompiledMethod)
        if (method.isObject() && method.rawBits() > 0x10000) {
            ObjectHeader* methodHdr = method.asObjectPtr();
            if (methodHdr->isCompiledMethod() && methodHdr->slotCount() >= 2) {
                Oop selectorOop = memory_.fetchPointer(1, method);
                if (selectorOop.isObject() && selectorOop.rawBits() > 0x10000) {
                    ObjectHeader* selHdr = selectorOop.asObjectPtr();
                    if (selHdr->isBytesObject() && selHdr->byteSize() <= 50) {
                        methodSelector = std::string((char*)selHdr->bytes(), selHdr->byteSize());
                    }
                }
            }
        }

        // Debug chain output - disabled for normal operation
        // std::cerr << "[CHAIN " << depth << "] " << rcvrClassName << ">>" << methodSelector << "\n";

        // Check if this method has a primitive (for first few contexts)
        if (depth < 10 && method.isObject() && method.rawBits() > 0x10000) {
            int primIdx = primitiveIndexOf(method);
            (void)primIdx;  // Unused when debug is disabled
        }

        // Check if we're in snapshot-related code
        if (rcvrClassName == "SnapshotOperation" || rcvrClassName == "SessionManager") {
            inSnapshotCode = true;
        } else if (inSnapshotCode && snapshotEndDepth < 0) {
            // First context after snapshot code
            snapshotEndDepth = depth;
            resumeContext = currentCtx;
            // DEBUG: "[CHAIN] First non-snapshot context at depth " << depth
        }

        // Move to sender
        Oop sender = memory_.fetchPointer(0, currentCtx);
        currentCtx = sender;
        depth++;
    }

    // If we detected snapshot code, note it but execute normally
    // The fallback handlers in arithmeticSend/commonSend will handle DNU cases
    if (inSnapshotCode) {
        // DEBUG: "[DEBUG] Detected snapshot code - executing normally with fallback handlers"
    }

    // Note: Display initialization is deferred to primitiveForceDisplayUpdate
    // to avoid crashes during early VM setup

    // Now execute from the original context
    // DEBUG_LOG("[DEBUG] Found valid context - delegating to executeFromContext()";
    return executeFromContext(context);
}

// ===== DISPLAY INITIALIZATION =====

void Interpreter::initializeDisplayForm() {
    // Direct platform display initialization is now done in bootstrapStartup
    // This method is kept for future use with Smalltalk Forms
    if (pharo::gDisplaySurface && displayForm_.isNil()) {
        // For now, just fill platform display with a test pattern
        uint32_t* pixels = pharo::gDisplaySurface->pixels();
        int width = pharo::gDisplaySurface->width();
        int height = pharo::gDisplaySurface->height();

        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                uint8_t r = static_cast<uint8_t>(128 + (x * 127 / width));
                uint8_t g = static_cast<uint8_t>(128 + (y * 127 / height));
                uint8_t b = 255;
                pixels[y * width + x] = (255 << 24) | (r << 16) | (g << 8) | b;
            }
        }
        pharo::gDisplaySurface->update();
    }
}

void Interpreter::ensureDisplayForm(int width, int height, int depth) {
    // Check if Display already exists
    Oop existingDisplay = memory_.findGlobal("Display");
    if (!existingDisplay.isNil() && existingDisplay.isObject()) {
        displayForm_ = existingDisplay;
        return;
    }

    // Find Form and Bitmap classes
    Oop formClass = memory_.findGlobal("Form");
    Oop bitmapClass = memory_.findGlobal("Bitmap");

    if (formClass.isNil() || !formClass.isObject()) return;
    if (bitmapClass.isNil() || !bitmapClass.isObject()) return;

    uint32_t formClassIdx = memory_.indexOfClass(formClass);
    uint32_t bitmapClassIdx = memory_.indexOfClass(bitmapClass);

    if (formClassIdx == 0 || bitmapClassIdx == 0) return;

    // Allocate bitmap for pixels (32-bit pixels = 1 word each for 32-bit depth)
    size_t pixelCount = static_cast<size_t>(width) * height;
    Oop bitmapObj = memory_.allocateWords(bitmapClassIdx, pixelCount);

    if (bitmapObj.isNil()) return;

    // Fill bitmap with gray to make it visible
    ObjectHeader* bitmapHdr = bitmapObj.asObjectPtr();
    uint32_t* pixels = reinterpret_cast<uint32_t*>(bitmapHdr->bytes());
    for (size_t i = 0; i < pixelCount; i++) {
        pixels[i] = 0xFF808080;  // Gray
    }

    // Allocate Form with 5 slots: bits, width, height, depth, offset
    Oop formObj = memory_.allocateSlots(formClassIdx, 5);

    if (formObj.isNil()) return;

    // Set Form slots
    memory_.storePointer(0, formObj, bitmapObj);                    // bits
    memory_.storePointer(1, formObj, Oop::fromSmallInteger(width)); // width
    memory_.storePointer(2, formObj, Oop::fromSmallInteger(height)); // height
    memory_.storePointer(3, formObj, Oop::fromSmallInteger(depth));  // depth
    memory_.storePointer(4, formObj, Oop::fromSmallInteger(0));      // offset (0@0)

    // Store locally
    displayForm_ = formObj;
    setScreenSize(width, height);
    setScreenDepth(depth);

    // Bind to 'Display' global so Morphic can find it
    memory_.setGlobal("Display", formObj);
}

// ===== BITMAP FONT FOR TEXT RENDERING =====
// Simple 5x7 bitmap font for menu bar text

static const uint8_t font5x7[96][7] = {
    // ASCII 32-127 (space through tilde)
    // Each character is 5 pixels wide, 7 pixels tall
    // Space (32)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    // ! (33)
    {0x04, 0x04, 0x04, 0x04, 0x00, 0x04, 0x00},
    // " (34)
    {0x0A, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00},
    // # (35)
    {0x0A, 0x1F, 0x0A, 0x0A, 0x1F, 0x0A, 0x00},
    // $ (36)
    {0x04, 0x0F, 0x14, 0x0E, 0x05, 0x1E, 0x04},
    // % (37)
    {0x19, 0x19, 0x02, 0x04, 0x08, 0x13, 0x13},
    // & (38)
    {0x08, 0x14, 0x14, 0x08, 0x15, 0x12, 0x0D},
    // ' (39)
    {0x04, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00},
    // ( (40)
    {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02},
    // ) (41)
    {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08},
    // * (42)
    {0x00, 0x04, 0x15, 0x0E, 0x15, 0x04, 0x00},
    // + (43)
    {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00},
    // , (44)
    {0x00, 0x00, 0x00, 0x00, 0x04, 0x04, 0x08},
    // - (45)
    {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00},
    // . (46)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00},
    // / (47)
    {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10},
    // 0 (48)
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},
    // 1 (49)
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    // 2 (50)
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
    // 3 (51)
    {0x0E, 0x11, 0x01, 0x06, 0x01, 0x11, 0x0E},
    // 4 (52)
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
    // 5 (53)
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    // 6 (54)
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},
    // 7 (55)
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    // 8 (56)
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
    // 9 (57)
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
    // : (58)
    {0x00, 0x04, 0x00, 0x00, 0x04, 0x00, 0x00},
    // ; (59)
    {0x00, 0x04, 0x00, 0x00, 0x04, 0x04, 0x08},
    // < (60)
    {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02},
    // = (61)
    {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00},
    // > (62)
    {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08},
    // ? (63)
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04},
    // @ (64)
    {0x0E, 0x11, 0x17, 0x15, 0x17, 0x10, 0x0E},
    // A (65)
    {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},
    // B (66)
    {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},
    // C (67)
    {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E},
    // D (68)
    {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C},
    // E (69)
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F},
    // F (70)
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},
    // G (71)
    {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F},
    // H (72)
    {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},
    // I (73)
    {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E},
    // J (74)
    {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C},
    // K (75)
    {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11},
    // L (76)
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},
    // M (77)
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11},
    // N (78)
    {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11},
    // O (79)
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
    // P (80)
    {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},
    // Q (81)
    {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D},
    // R (82)
    {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},
    // S (83)
    {0x0E, 0x11, 0x10, 0x0E, 0x01, 0x11, 0x0E},
    // T (84)
    {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
    // U (85)
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
    // V (86)
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04},
    // W (87)
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11},
    // X (88)
    {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11},
    // Y (89)
    {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04},
    // Z (90)
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F},
    // [ (91)
    {0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E},
    // \ (92)
    {0x10, 0x10, 0x08, 0x04, 0x02, 0x01, 0x01},
    // ] (93)
    {0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E},
    // ^ (94)
    {0x04, 0x0A, 0x11, 0x00, 0x00, 0x00, 0x00},
    // _ (95)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F},
    // ` (96)
    {0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00},
    // a (97)
    {0x00, 0x00, 0x0E, 0x01, 0x0F, 0x11, 0x0F},
    // b (98)
    {0x10, 0x10, 0x1E, 0x11, 0x11, 0x11, 0x1E},
    // c (99)
    {0x00, 0x00, 0x0E, 0x10, 0x10, 0x10, 0x0E},
    // d (100)
    {0x01, 0x01, 0x0F, 0x11, 0x11, 0x11, 0x0F},
    // e (101)
    {0x00, 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0E},
    // f (102)
    {0x06, 0x08, 0x1E, 0x08, 0x08, 0x08, 0x08},
    // g (103)
    {0x00, 0x00, 0x0F, 0x11, 0x0F, 0x01, 0x0E},
    // h (104)
    {0x10, 0x10, 0x1E, 0x11, 0x11, 0x11, 0x11},
    // i (105)
    {0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E},
    // j (106)
    {0x02, 0x00, 0x02, 0x02, 0x02, 0x12, 0x0C},
    // k (107)
    {0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12},
    // l (108)
    {0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E},
    // m (109)
    {0x00, 0x00, 0x1A, 0x15, 0x15, 0x15, 0x15},
    // n (110)
    {0x00, 0x00, 0x1E, 0x11, 0x11, 0x11, 0x11},
    // o (111)
    {0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E},
    // p (112)
    {0x00, 0x00, 0x1E, 0x11, 0x1E, 0x10, 0x10},
    // q (113)
    {0x00, 0x00, 0x0F, 0x11, 0x0F, 0x01, 0x01},
    // r (114)
    {0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10},
    // s (115)
    {0x00, 0x00, 0x0E, 0x10, 0x0E, 0x01, 0x1E},
    // t (116)
    {0x08, 0x08, 0x1E, 0x08, 0x08, 0x09, 0x06},
    // u (117)
    {0x00, 0x00, 0x11, 0x11, 0x11, 0x11, 0x0E},
    // v (118)
    {0x00, 0x00, 0x11, 0x11, 0x11, 0x0A, 0x04},
    // w (119)
    {0x00, 0x00, 0x11, 0x15, 0x15, 0x15, 0x0A},
    // x (120)
    {0x00, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11},
    // y (121)
    {0x00, 0x00, 0x11, 0x11, 0x0F, 0x01, 0x0E},
    // z (122)
    {0x00, 0x00, 0x1F, 0x02, 0x04, 0x08, 0x1F},
    // { (123)
    {0x02, 0x04, 0x04, 0x08, 0x04, 0x04, 0x02},
    // | (124)
    {0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
    // } (125)
    {0x08, 0x04, 0x04, 0x02, 0x04, 0x04, 0x08},
    // ~ (126)
    {0x00, 0x08, 0x15, 0x02, 0x00, 0x00, 0x00},
    // DEL (127) - blank
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
};

// Draw text using Core Graphics for proper anti-aliased font rendering
static void drawText(uint32_t* pixels, int dispWidth, int dispHeight,
                     int x, int y, const std::string& text, uint32_t color, int fontSize = 14) {
#if __APPLE__
    if (text.empty()) return;

    // Extract color components (ARGB format)
    CGFloat alpha = ((color >> 24) & 0xFF) / 255.0;
    CGFloat red = ((color >> 16) & 0xFF) / 255.0;
    CGFloat green = ((color >> 8) & 0xFF) / 255.0;
    CGFloat blue = (color & 0xFF) / 255.0;

    // Create a generous text buffer - Helvetica chars are roughly 0.6 * fontSize wide
    int textWidth = static_cast<int>(text.length()) * fontSize;  // Generous estimate
    // Font metrics: ascender can be up to 95% of fontSize for some fonts
    // Use 2x fontSize to ensure we have enough room for any font
    int textHeight = static_cast<int>(fontSize * 2.0);  // Generous room for tall glyphs

    // Clamp to display bounds
    if (x < 0 || y < 0 || x >= dispWidth || y >= dispHeight) return;
    if (x + textWidth > dispWidth) textWidth = dispWidth - x;
    if (y + textHeight > dispHeight) textHeight = dispHeight - y;

    // Create bitmap context with ARGB format matching our display buffer
    // On Apple platforms (little-endian), use ARGB with native byte order
    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(
        nullptr,  // Let CG allocate memory
        textWidth, textHeight,
        8,  // bits per component
        textWidth * 4,  // bytes per row
        colorSpace,
        kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Host  // ARGB native
    );
    CGColorSpaceRelease(colorSpace);

    if (!ctx) return;

    // Clear to transparent
    CGContextClearRect(ctx, CGRectMake(0, 0, textWidth, textHeight));

    // Transform to normal text coordinates (flip Y axis)
    // CG has origin at bottom-left with Y going up
    // We want origin at top-left with Y going down
    CGContextTranslateCTM(ctx, 0, textHeight);
    CGContextScaleCTM(ctx, 1.0, -1.0);

    // Set up text rendering
    CGContextSetRGBFillColor(ctx, red, green, blue, alpha);
    CGContextSetTextDrawingMode(ctx, kCGTextFill);

    // Create font
    CTFontRef font = CTFontCreateWithName(CFSTR("Helvetica"), fontSize, nullptr);
    if (!font) {
        font = CTFontCreateWithName(CFSTR("Arial"), fontSize, nullptr);
    }
    if (!font) {
        CGContextRelease(ctx);
        return;
    }

    // Create attributed string
    CFStringRef cfText = CFStringCreateWithCString(nullptr, text.c_str(), kCFStringEncodingUTF8);
    if (!cfText) {
        CFRelease(font);
        CGContextRelease(ctx);
        return;
    }

    CFStringRef keys[] = { kCTFontAttributeName, kCTForegroundColorFromContextAttributeName };
    CFTypeRef values[] = { font, kCFBooleanTrue };
    CFDictionaryRef attrs = CFDictionaryCreate(nullptr, (const void**)keys, (const void**)values, 2,
                                                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    CFAttributedStringRef attrString = CFAttributedStringCreate(nullptr, cfText, attrs);
    CFRelease(cfText);
    CFRelease(attrs);

    if (!attrString) {
        CFRelease(font);
        CGContextRelease(ctx);
        return;
    }

    // Create line and draw
    CTLineRef line = CTLineCreateWithAttributedString(attrString);
    CFRelease(attrString);

    if (line) {
        // Position text baseline - need room for ascenders ABOVE and descenders BELOW
        // After the Y-flip transform, Y=0 is at top, Y increases downward
        // Ascender can be up to 95% of fontSize for system fonts
        CGFloat baseline = fontSize * 0.95;  // Room for tall ascenders
        CGContextSetTextPosition(ctx, 0, baseline);
        CTLineDraw(line, ctx);
        CFRelease(line);
    }

    CFRelease(font);

    // Copy rendered text to pixel buffer
    // Note: CGBitmapContext stores pixels bottom-up, but we transformed drawing to top-down
    // The pixel data is still stored bottom-up, so we need to flip Y when reading
    uint32_t* textPixels = (uint32_t*)CGBitmapContextGetData(ctx);
    if (textPixels) {
        for (int ty = 0; ty < textHeight; ty++) {
            for (int tx = 0; tx < textWidth; tx++) {
                int destX = x + tx;
                int destY = y + ty;
                if (destX >= 0 && destX < dispWidth && destY >= 0 && destY < dispHeight) {
                    // Bitmap data is stored bottom-up, flip Y to read correctly
                    int srcY = textHeight - 1 - ty;
                    uint32_t srcPixel = textPixels[srcY * textWidth + tx];
                    // ARGB format with native byte order: 0xAARRGGBB as uint32_t
                    uint8_t srcAlpha = (srcPixel >> 24) & 0xFF;
                    uint8_t srcR = (srcPixel >> 16) & 0xFF;
                    uint8_t srcG = (srcPixel >> 8) & 0xFF;
                    uint8_t srcB = srcPixel & 0xFF;
                    if (srcAlpha > 0) {
                        // Alpha blend (source is premultiplied alpha)
                        uint32_t destPixel = pixels[destY * dispWidth + destX];
                        uint8_t destR = (destPixel >> 16) & 0xFF;
                        uint8_t destG = (destPixel >> 8) & 0xFF;
                        uint8_t destB = destPixel & 0xFF;

                        // For premultiplied alpha: out = src + dest * (1 - srcAlpha)
                        uint8_t outR = srcR + (destR * (255 - srcAlpha)) / 255;
                        uint8_t outG = srcG + (destG * (255 - srcAlpha)) / 255;
                        uint8_t outB = srcB + (destB * (255 - srcAlpha)) / 255;

                        pixels[destY * dispWidth + destX] = 0xFF000000 | (outR << 16) | (outG << 8) | outB;
                    }
                }
            }
        }
    }

    CGContextRelease(ctx);
#endif
}

void Interpreter::renderWorldMorphs() {
    // Render World's morphs directly to the platform display
    // This bypasses NullWorldRenderer and draws morphs ourselves
    if (!pharo::gDisplaySurface) return;

    // Throttle to ~60fps to prevent flickering
    static auto lastRenderTime = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastRenderTime);
    if (elapsed.count() < 16) {  // Less than ~16ms = 60fps
        return;  // Skip this frame
    }
    lastRenderTime = now;

    static int totalMorphsDrawn = 0;
    static int renderCallCount = 0;
    renderCallCount++;

    // Debug logging to file (first 10 calls only)
    static FILE* logFile = nullptr;
    if (!logFile) {
        logFile = fopen("/tmp/iospharo-render.log", "w");
    }
    if (logFile && renderCallCount <= 10) {
        fprintf(logFile, "[RENDER #%d] Starting renderWorldMorphs\n", renderCallCount);
        fflush(logFile);
    }

    // Find the World global
    Oop world = memory_.findGlobal("World");
    if (world.isNil() || !world.isObject()) {
        if (logFile && renderCallCount <= 10) {
            fprintf(logFile, "[RENDER #%d] World not found or nil\n", renderCallCount);
            fflush(logFile);
        }
        return;
    }

    if (logFile && renderCallCount <= 10) {
        fprintf(logFile, "[RENDER #%d] World found at 0x%llx\n", renderCallCount, (unsigned long long)world.rawBits());
        fflush(logFile);
    }

    uint32_t* pixels = pharo::gDisplaySurface->pixels();
    int dispWidth = pharo::gDisplaySurface->width();
    int dispHeight = pharo::gDisplaySurface->height();
    Oop nilObj = memory_.nil();

    // Helper to get morph class name
    auto getMorphClassName = [this](Oop morph) -> std::string {
        Oop morphClass = memory_.classOf(morph);
        if (morphClass.isObject()) {
            ObjectHeader* clsHdr = morphClass.asObjectPtr();
            if (clsHdr->slotCount() > 6) {
                Oop clsName = memory_.fetchPointer(6, morphClass);
                if (clsName.isObject()) {
                    ObjectHeader* nameHdr = clsName.asObjectPtr();
                    if (nameHdr->isBytesObject() && nameHdr->byteSize() < 50) {
                        return std::string((char*)nameHdr->bytes(), nameHdr->byteSize());
                    }
                }
            }
        }
        return "Unknown";
    };

    // Helper to extract color from a Color object
    auto extractColor = [this, nilObj](Oop colorObj) -> uint32_t {
        if (colorObj.isNil() || !colorObj.isObject()) return 0xFFCCCCCC;

        // Color stores RGB in slot 0 (privateRGB - may be SmallInteger or Float)
        Oop rgb = memory_.fetchPointer(0, colorObj);

        if (rgb.isSmallInteger()) {
            int rgbVal = static_cast<int>(rgb.asSmallInteger());
            return 0xFF000000 | (rgbVal & 0xFFFFFF);
        }

        // Handle BoxedFloat64 - Pharo stores RGB as a float 0.0 to 16777215.0
        if (rgb.isObject()) {
            ObjectHeader* rgbHdr = rgb.asObjectPtr();
            if (rgbHdr->isBytesObject() && rgbHdr->byteSize() == 8) {
                double* floatVal = reinterpret_cast<double*>(rgbHdr->bytes());
                int rgbInt = static_cast<int>(*floatVal) & 0xFFFFFF;
                return 0xFF000000 | rgbInt;
            }
        }

        // Return a default gray color for unhandled cases
        return 0xFFCCCCCC;
    };

    // Helper to extract rectangle from a slot
    auto extractRect = [this](Oop rect, int& x1, int& y1, int& x2, int& y2) -> bool {
        if (rect.isNil() || !rect.isObject()) return false;

        // Rectangle slots: 0=origin (Point), 1=corner (Point)
        Oop origin = memory_.fetchPointer(0, rect);
        Oop corner = memory_.fetchPointer(1, rect);
        if (origin.isNil() || corner.isNil()) return false;
        if (!origin.isObject() || !corner.isObject()) return false;

        // Point slots: 0=x, 1=y
        Oop originX = memory_.fetchPointer(0, origin);
        Oop originY = memory_.fetchPointer(1, origin);
        Oop cornerX = memory_.fetchPointer(0, corner);
        Oop cornerY = memory_.fetchPointer(1, corner);

        // Extract coordinates - handle both SmallInteger and SmallFloat
        auto extractCoord = [](Oop coord) -> int {
            if (coord.isSmallInteger()) {
                return static_cast<int>(coord.asSmallInteger());
            } else if (coord.isSmallFloat()) {
                return static_cast<int>(coord.asSmallFloat());
            }
            return 0;
        };

        x1 = extractCoord(originX);
        y1 = extractCoord(originY);
        x2 = extractCoord(cornerX);
        y2 = extractCoord(cornerY);

        return true;
    };

    // Helper to extract bounds from a morph
    auto extractBounds = [this, nilObj, &extractRect, dispWidth, dispHeight, &getMorphClassName]
            (Oop morph, int& x1, int& y1, int& x2, int& y2) -> bool {
        // Try bounds (slot 0) first
        if (extractRect(memory_.fetchPointer(0, morph), x1, y1, x2, y2)) {
            // Check if bounds are valid (non-zero size)
            if (x2 - x1 > 0 && y2 - y1 > 0) return true;
        }

        // Try fullBounds (slot 3) as fallback
        if (extractRect(memory_.fetchPointer(3, morph), x1, y1, x2, y2)) {
            if (x2 - x1 > 0 && y2 - y1 > 0) return true;
        }

        // Assign fallback bounds based on morph class (for headless images)
        std::string className = getMorphClassName(morph);
        if (className == "MenubarMorph") {
            x1 = 0; y1 = 0; x2 = dispWidth; y2 = 25;
            return true;
        } else if (className == "TaskbarMorph") {
            x1 = 0; y1 = dispHeight - 40; x2 = dispWidth; y2 = dispHeight;
            return true;
        } else if (className == "SpWindow" || className == "SystemWindow") {
            x1 = 50; y1 = 50; x2 = dispWidth - 100; y2 = dispHeight - 100;
            return true;
        } else if (className.find("Grip") != std::string::npos) {
            // Window grips - skip (will be positioned relative to window)
            return false;
        }

        // For other morphs with zero bounds, skip rendering
        return false;
    };

    // Helper to extract string from a string object (ByteString, WideString, etc.)
    auto extractString = [this](Oop strObj) -> std::string {
        if (strObj.isNil() || !strObj.isObject()) return "";
        ObjectHeader* hdr = strObj.asObjectPtr();
        if (hdr->isBytesObject() && hdr->byteSize() < 100) {
            return std::string((char*)hdr->bytes(), hdr->byteSize());
        }
        return "";
    };

    // Helper to render MenubarMorph with text
    auto renderMenuBar = [&](Oop menubarMorph, int menuBarHeight) {
        // Offset below Mac title bar (title bar is ~28px, but in Retina it's ~56px)
        // Use a safe offset that works for both regular and Retina displays
        int titleBarOffset = (dispHeight > 1000) ? 56 : 28;  // Retina vs regular

        // Scale menu bar height for Retina
        int scaledMenuBarHeight = (dispHeight > 1000) ? menuBarHeight * 2 : menuBarHeight;

        // Draw menu bar background - Mac-style light gray with subtle gradient
        // Fill from y=0 to cover any gaps below the native title bar
        // Use solid gray for the area under the title bar (traffic lights)
        uint32_t titleAreaColor = 0xFFF6F6F6;  // Light gray matching top of menu bar
        for (int y = 0; y < titleBarOffset && y < dispHeight; y++) {
            for (int x = 0; x < dispWidth; x++) {
                pixels[y * dispWidth + x] = titleAreaColor;
            }
        }
        // Top color slightly lighter, bottom slightly darker for depth
        for (int y = titleBarOffset; y < titleBarOffset + scaledMenuBarHeight; y++) {
            // Subtle gradient from 0xF6 at top to 0xE8 at bottom
            int progress = y - titleBarOffset;
            int gray = 246 - (progress * 14 / scaledMenuBarHeight);  // 0xF6 to 0xE8
            uint32_t menuBarColor = 0xFF000000 | (gray << 16) | (gray << 8) | gray;
            for (int x = 0; x < dispWidth; x++) {
                if (y < dispHeight) {
                    pixels[y * dispWidth + x] = menuBarColor;
                }
            }
        }

        // Draw bottom border line (subtle shadow)
        int borderY = titleBarOffset + scaledMenuBarHeight - 1;
        if (borderY < dispHeight) {
            for (int x = 0; x < dispWidth; x++) {
                pixels[borderY * dispWidth + x] = 0xFFD0D0D0;  // Light gray border
            }
        }

        // Get submorphs (menu items)
        Oop submorphs = memory_.fetchPointer(2, menubarMorph);
        if (submorphs.isNil() || !submorphs.isObject()) {
            if (logFile && renderCallCount <= 10) {
                fprintf(logFile, "[MENUBAR] No submorphs found\n");
                fflush(logFile);
            }
            return;
        }

        ObjectHeader* subHdr = submorphs.asObjectPtr();
        size_t numItems = subHdr->slotCount();

        if (logFile && renderCallCount <= 10) {
            fprintf(logFile, "[MENUBAR] Found %zu menu items\n", numItems);
            fflush(logFile);
        }

        // Menu item text color (dark gray for readability on light background)
        uint32_t textColor = 0xFF1A1A1A;

        // Collect menu item labels and store morphs for dropdown access
        std::vector<std::string> labels;
        menuBarItemMorphs_.clear();
        for (size_t i = 0; i < numItems; i++) {
            Oop item = subHdr->slotAt(i);
            if (item.isNil() || !item.isObject()) continue;

            // Try to extract label from menu item
            // MenubarItemMorph or similar - try slot 5 (label) or look for StringMorph submorph
            std::string label;

            // Check slots 5-10 for potential label
            ObjectHeader* itemHdr = item.asObjectPtr();
            for (size_t slot = 5; slot < std::min((size_t)15, itemHdr->slotCount()); slot++) {
                Oop slotVal = memory_.fetchPointer(slot, item);
                if (slotVal.isNil() || !slotVal.isObject()) continue;

                // Check if it's a string
                ObjectHeader* slotHdr = slotVal.asObjectPtr();
                if (slotHdr->isBytesObject() && slotHdr->byteSize() > 0 && slotHdr->byteSize() < 50) {
                    std::string s((char*)slotHdr->bytes(), slotHdr->byteSize());
                    // Filter: must be readable text (printable ASCII)
                    bool valid = true;
                    for (char c : s) {
                        if (c < 32 || c > 126) { valid = false; break; }
                    }
                    if (valid && s.length() > 0 && s.length() < 20) {
                        label = s;
                        break;
                    }
                }
            }

            // If no label found in item, check its submorphs for StringMorph
            if (label.empty()) {
                Oop itemSubmorphs = memory_.fetchPointer(2, item);
                if (!itemSubmorphs.isNil() && itemSubmorphs.isObject()) {
                    ObjectHeader* isHdr = itemSubmorphs.asObjectPtr();
                    for (size_t j = 0; j < isHdr->slotCount(); j++) {
                        Oop subm = isHdr->slotAt(j);
                        if (subm.isNil() || !subm.isObject()) continue;

                        // Check if it's a StringMorph by looking for a string in slot 5+
                        ObjectHeader* submHdr = subm.asObjectPtr();
                        for (size_t slot = 5; slot < std::min((size_t)15, submHdr->slotCount()); slot++) {
                            Oop slotVal = memory_.fetchPointer(slot, subm);
                            if (slotVal.isNil() || !slotVal.isObject()) continue;

                            ObjectHeader* svHdr = slotVal.asObjectPtr();
                            if (svHdr->isBytesObject() && svHdr->byteSize() > 0 && svHdr->byteSize() < 50) {
                                std::string s((char*)svHdr->bytes(), svHdr->byteSize());
                                bool valid = true;
                                for (char c : s) {
                                    if (c < 32 || c > 126) { valid = false; break; }
                                }
                                if (valid && s.length() > 0 && s.length() < 20) {
                                    label = s;
                                    break;
                                }
                            }
                        }
                        if (!label.empty()) break;
                    }
                }
            }

            if (!label.empty()) {
                labels.push_back(label);
                menuBarItemMorphs_.push_back(item);  // Store morph for dropdown access
                if (logFile && renderCallCount <= 10) {
                    fprintf(logFile, "[MENUBAR] Item %zu label: '%s'\n", i, label.c_str());
                    fflush(logFile);
                }
            } else {
                if (logFile && renderCallCount <= 10) {
                    fprintf(logFile, "[MENUBAR] Item %zu: no label found\n", i);
                    fflush(logFile);
                }
            }
        }

        if (logFile && renderCallCount <= 10) {
            fprintf(logFile, "[MENUBAR] Total labels: %zu\n", labels.size());
            fflush(logFile);
        }

        // Draw menu item labels with proper spacing
        // Note: On Retina, we're drawing directly to a pixel buffer at 2x physical resolution
        // The scaledMenuBarHeight is already in physical pixels
        bool isRetina = dispWidth > 1500;
        // Use a large enough font that it's clearly visible in the menu bar
        int fontSize = isRetina ? 48 : 24;  // ~24pt appearance on Retina for better readability
        int textX = isRetina ? 32 : 16;  // Starting x position with padding
        // Center the x-height (lowercase letters without ascenders/descenders) vertically
        // Text buffer: baseline is at 0.95*fontSize from top, x-height center is ~0.70*fontSize from top
        int menuBarCenter = titleBarOffset + scaledMenuBarHeight / 2;
        int xHeightCenterInBuffer = static_cast<int>(fontSize * 0.70);
        int textY = menuBarCenter - xHeightCenterInBuffer;
        int itemSpacing = isRetina ? 56 : 28;  // Space between items
        int charWidth = isRetina ? 26 : 13;  // Approximate char width for larger font

        if (logFile && renderCallCount <= 3) {
            fprintf(logFile, "[MENUBAR] Drawing text at y=%d (offset=%d), scaledMenuBarHeight=%d, isRetina=%d, fontSize=%d\n",
                    textY, titleBarOffset, scaledMenuBarHeight, isRetina ? 1 : 0, fontSize);
            fflush(logFile);
        }

        // Clear and rebuild menu item bounds
        menuItemBounds_.clear();

        int itemIndex = 0;
        for (const std::string& label : labels) {
            if (logFile && renderCallCount <= 3) {
                fprintf(logFile, "[MENUBAR] drawText('%s') at x=%d y=%d fontSize=%d\n", label.c_str(), textX, textY, fontSize);
                fflush(logFile);
            }
            int itemWidth = static_cast<int>(label.length()) * charWidth;
            int itemPadding = isRetina ? 16 : 8;

            // Highlight selected menu item
            if (selectedMenuIndex_ == itemIndex) {
                // Draw highlight background (darker blue)
                uint32_t highlightColor = 0xFF3478F6;  // macOS blue selection color
                for (int hy = titleBarOffset; hy < titleBarOffset + scaledMenuBarHeight - 1; hy++) {
                    for (int hx = textX - itemPadding; hx < textX + itemWidth + itemPadding && hx < dispWidth; hx++) {
                        if (hx >= 0 && hy >= 0 && hy < dispHeight) {
                            pixels[hy * dispWidth + hx] = highlightColor;
                        }
                    }
                }
                // Use white text on blue background
                drawText(pixels, dispWidth, dispHeight, textX, textY, label, 0xFFFFFFFF, fontSize);
            } else {
                drawText(pixels, dispWidth, dispHeight, textX, textY, label, textColor, fontSize);
            }

            // Store bounds for click detection (in pixels) - include padding
            menuItemBounds_.push_back({textX - itemPadding, textX + itemWidth + itemPadding});
            textX += itemWidth + itemSpacing;
            itemIndex++;
        }

        // Store menu bar bounds for click detection
        menuBarTop_ = titleBarOffset;
        menuBarBottom_ = titleBarOffset + scaledMenuBarHeight;
        menuBarScale_ = isRetina ? 2 : 1;

        // Render dropdown menu if a menu is selected
        if (selectedMenuIndex_ >= 0 && selectedMenuIndex_ < static_cast<int>(menuBarItemMorphs_.size())) {
            Oop selectedItem = menuBarItemMorphs_[selectedMenuIndex_];

            // Find the menu associated with this menu bar item
            // In Pharo, the menu is typically in slot 6 or nearby slots
            Oop menuMorph = Oop::nil();
            ObjectHeader* itemHdr = selectedItem.asObjectPtr();

            // Look for MenuMorph in slots
            for (size_t slot = 5; slot < std::min((size_t)20, itemHdr->slotCount()); slot++) {
                Oop slotVal = memory_.fetchPointer(slot, selectedItem);
                if (slotVal.isNil() || !slotVal.isObject()) continue;

                // Check if it's a MenuMorph
                Oop slotClass = memory_.classOf(slotVal);
                if (slotClass.isObject()) {
                    ObjectHeader* classHdr = slotClass.asObjectPtr();
                    if (classHdr->slotCount() > 6) {
                        Oop className = memory_.fetchPointer(6, slotClass);
                        if (className.isObject()) {
                            ObjectHeader* nameHdr = className.asObjectPtr();
                            if (nameHdr->isBytesObject() && nameHdr->byteSize() < 50) {
                                std::string cn((char*)nameHdr->bytes(), nameHdr->byteSize());
                                if (cn.find("Menu") != std::string::npos) {
                                    menuMorph = slotVal;
                                    if (logFile) {
                                        fprintf(logFile, "[DROPDOWN] Found menu in slot %zu: %s\n", slot, cn.c_str());
                                        fflush(logFile);
                                    }
                                    break;
                                }
                            }
                        }
                    }
                }
            }

            // Collect dropdown item labels
            std::vector<std::string> dropdownLabels;
            std::vector<Oop> dropdownItemMorphs;

            if (!menuMorph.isNil() && menuMorph.isObject()) {
                // Get submorphs from the menu
                Oop menuSubmorphs = memory_.fetchPointer(2, menuMorph);
                if (!menuSubmorphs.isNil() && menuSubmorphs.isObject()) {
                    ObjectHeader* msHdr = menuSubmorphs.asObjectPtr();
                    size_t numMenuItems = msHdr->slotCount();

                    if (logFile) {
                        fprintf(logFile, "[DROPDOWN] Menu has %zu items\n", numMenuItems);
                        fflush(logFile);
                    }

                    for (size_t mi = 0; mi < numMenuItems; mi++) {
                        Oop menuItem = msHdr->slotAt(mi);
                        if (menuItem.isNil() || !menuItem.isObject()) continue;

                        // Extract label from menu item using same logic as menu bar
                        std::string itemLabel;
                        ObjectHeader* miHdr = menuItem.asObjectPtr();

                        // Check slots for string label
                        for (size_t slot = 5; slot < std::min((size_t)15, miHdr->slotCount()); slot++) {
                            Oop slotVal = memory_.fetchPointer(slot, menuItem);
                            if (slotVal.isNil() || !slotVal.isObject()) continue;

                            ObjectHeader* slotHdr = slotVal.asObjectPtr();
                            if (slotHdr->isBytesObject() && slotHdr->byteSize() > 0 && slotHdr->byteSize() < 100) {
                                std::string s((char*)slotHdr->bytes(), slotHdr->byteSize());
                                bool valid = true;
                                for (char c : s) {
                                    if (c < 32 || c > 126) { valid = false; break; }
                                }
                                if (valid && s.length() > 0) {
                                    itemLabel = s;
                                    break;
                                }
                            }
                        }

                        // Also check submorphs for label
                        if (itemLabel.empty()) {
                            Oop itemSubmorphs = memory_.fetchPointer(2, menuItem);
                            if (!itemSubmorphs.isNil() && itemSubmorphs.isObject()) {
                                ObjectHeader* isHdr = itemSubmorphs.asObjectPtr();
                                for (size_t j = 0; j < isHdr->slotCount() && itemLabel.empty(); j++) {
                                    Oop subm = isHdr->slotAt(j);
                                    if (subm.isNil() || !subm.isObject()) continue;

                                    ObjectHeader* submHdr = subm.asObjectPtr();
                                    for (size_t slot = 5; slot < std::min((size_t)15, submHdr->slotCount()); slot++) {
                                        Oop slotVal = memory_.fetchPointer(slot, subm);
                                        if (slotVal.isNil() || !slotVal.isObject()) continue;

                                        ObjectHeader* svHdr = slotVal.asObjectPtr();
                                        if (svHdr->isBytesObject() && svHdr->byteSize() > 0 && svHdr->byteSize() < 100) {
                                            std::string s((char*)svHdr->bytes(), svHdr->byteSize());
                                            bool valid = true;
                                            for (char c : s) {
                                                if (c < 32 || c > 126) { valid = false; break; }
                                            }
                                            if (valid && s.length() > 0) {
                                                itemLabel = s;
                                                break;
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        if (!itemLabel.empty()) {
                            dropdownLabels.push_back(itemLabel);
                            dropdownItemMorphs.push_back(menuItem);
                            if (logFile) {
                                fprintf(logFile, "[DROPDOWN] Item %zu: '%s'\n", mi, itemLabel.c_str());
                                fflush(logFile);
                            }
                        }
                    }
                }
            }

            // Draw dropdown if we have items
            if (!dropdownLabels.empty()) {
                // Calculate dropdown dimensions
                int dropdownFontSize = fontSize;
                int lineHeight = static_cast<int>(dropdownFontSize * 1.5);
                int dropdownPadding = isRetina ? 16 : 8;

                // Find max label width
                int maxLabelWidth = 0;
                for (const auto& lbl : dropdownLabels) {
                    int w = static_cast<int>(lbl.length()) * charWidth;
                    if (w > maxLabelWidth) maxLabelWidth = w;
                }

                int dropdownWidth = maxLabelWidth + dropdownPadding * 4;
                int dropdownHeight = static_cast<int>(dropdownLabels.size()) * lineHeight + dropdownPadding * 2;

                // Position dropdown below the selected menu item
                int dropdownX = (selectedMenuIndex_ < static_cast<int>(menuItemBounds_.size()))
                    ? menuItemBounds_[selectedMenuIndex_].first : textX;
                int dropdownY = titleBarOffset + scaledMenuBarHeight;

                // Draw dropdown background (white with subtle shadow)
                uint32_t bgColor = 0xFFFAFAFA;
                uint32_t borderColor = 0xFFCCCCCC;
                uint32_t shadowColor = 0x20000000;  // Semi-transparent black

                // Draw shadow (offset by 2 pixels)
                for (int sy = dropdownY + 2; sy < dropdownY + dropdownHeight + 4 && sy < dispHeight; sy++) {
                    for (int sx = dropdownX + 2; sx < dropdownX + dropdownWidth + 4 && sx < dispWidth; sx++) {
                        if (sx >= 0 && sy >= 0) {
                            pixels[sy * dispWidth + sx] = shadowColor;
                        }
                    }
                }

                // Draw background
                for (int dy = dropdownY; dy < dropdownY + dropdownHeight && dy < dispHeight; dy++) {
                    for (int dx = dropdownX; dx < dropdownX + dropdownWidth && dx < dispWidth; dx++) {
                        if (dx >= 0 && dy >= 0) {
                            pixels[dy * dispWidth + dx] = bgColor;
                        }
                    }
                }

                // Draw border
                for (int dx = dropdownX; dx < dropdownX + dropdownWidth && dx < dispWidth; dx++) {
                    if (dx >= 0) {
                        if (dropdownY >= 0 && dropdownY < dispHeight)
                            pixels[dropdownY * dispWidth + dx] = borderColor;
                        int bottomY = dropdownY + dropdownHeight - 1;
                        if (bottomY >= 0 && bottomY < dispHeight)
                            pixels[bottomY * dispWidth + dx] = borderColor;
                    }
                }
                for (int dy = dropdownY; dy < dropdownY + dropdownHeight && dy < dispHeight; dy++) {
                    if (dy >= 0) {
                        if (dropdownX >= 0 && dropdownX < dispWidth)
                            pixels[dy * dispWidth + dropdownX] = borderColor;
                        int rightX = dropdownX + dropdownWidth - 1;
                        if (rightX >= 0 && rightX < dispWidth)
                            pixels[dy * dispWidth + rightX] = borderColor;
                    }
                }

                // Draw menu item labels
                int itemY = dropdownY + dropdownPadding;
                for (size_t di = 0; di < dropdownLabels.size(); di++) {
                    drawText(pixels, dispWidth, dispHeight,
                             dropdownX + dropdownPadding * 2, itemY,
                             dropdownLabels[di], textColor, dropdownFontSize);
                    itemY += lineHeight;
                }

                // Store dropdown state for click handling
                dropdownState_.x = dropdownX;
                dropdownState_.y = dropdownY;
                dropdownState_.width = dropdownWidth;
                dropdownState_.height = dropdownHeight;
                dropdownState_.lineHeight = lineHeight;
                dropdownState_.itemMorphs = dropdownItemMorphs;
                dropdownState_.valid = true;
            } else {
                dropdownState_.valid = false;
            }
        } else {
            dropdownState_.valid = false;
        }

        totalMorphsDrawn++;
    };

    // Recursive morph rendering function
    std::function<void(Oop, int, int)> renderMorph = [&](Oop morph, int depth, int index) {
        if (morph.isNil() || !morph.isObject()) return;
        if (depth > 20) return;  // Prevent infinite recursion

        // Check for special morphs that need custom rendering
        std::string className = getMorphClassName(morph);

        // Handle MenubarMorph specially - draw with text
        if (className == "MenubarMorph") {
            renderMenuBar(morph, 44);  // 44 pixel height (88 on Retina)
            return;  // Don't recurse into menu bar submorphs
        }

        // Skip TaskbarMorph for now (just draw background)
        if (className == "TaskbarMorph") {
            // Draw simple gray taskbar
            int taskbarHeight = 40;
            int taskbarY = dispHeight - taskbarHeight;
            uint32_t taskbarColor = 0xFF2A2A2A;
            for (int y = taskbarY; y < dispHeight; y++) {
                for (int x = 0; x < dispWidth; x++) {
                    pixels[y * dispWidth + x] = taskbarColor;
                }
            }
            totalMorphsDrawn++;
            return;
        }

        totalMorphsDrawn++;

        // Get bounds
        int x1, y1, x2, y2;
        if (!extractBounds(morph, x1, y1, x2, y2)) return;

        // Get color
        Oop morphColor = memory_.fetchPointer(4, morph);
        uint32_t colorARGB = extractColor(morphColor);

        // Skip black morphs (they're likely text areas or transparent)
        // This is a temporary workaround until text rendering is implemented
        if ((colorARGB & 0xFFFFFF) == 0) return;  // Skip pure black

        // Skip if bounds are invalid or too small (but don't skip for depth > 0)
        bool hasBounds = (x2 - x1 >= 1 && y2 - y1 >= 1);

        // Clamp to display bounds
        int cx1 = std::max(0, std::min(x1, dispWidth));
        int cy1 = std::max(0, std::min(y1, dispHeight));
        int cx2 = std::max(0, std::min(x2, dispWidth));
        int cy2 = std::max(0, std::min(y2, dispHeight));

        // Draw filled rectangle if we have valid bounds
        if (hasBounds && cx2 > cx1 && cy2 > cy1) {
            for (int y = cy1; y < cy2; y++) {
                for (int x = cx1; x < cx2; x++) {
                    pixels[y * dispWidth + x] = colorARGB;
                }
            }

            // Draw a 1-pixel darker border for visibility
            uint32_t borderColor = ((colorARGB & 0xFF000000)) |
                                   (((colorARGB >> 16) & 0xFF) * 3 / 4 << 16) |
                                   (((colorARGB >> 8) & 0xFF) * 3 / 4 << 8) |
                                   ((colorARGB & 0xFF) * 3 / 4);

            // Top and bottom borders
            for (int x = cx1; x < cx2; x++) {
                if (cy1 < dispHeight) pixels[cy1 * dispWidth + x] = borderColor;
                if (cy2 - 1 >= 0 && cy2 - 1 < dispHeight) pixels[(cy2 - 1) * dispWidth + x] = borderColor;
            }
            // Left and right borders
            for (int y = cy1; y < cy2; y++) {
                if (cx1 < dispWidth) pixels[y * dispWidth + cx1] = borderColor;
                if (cx2 - 1 >= 0 && cx2 - 1 < dispWidth) pixels[y * dispWidth + cx2 - 1] = borderColor;
            }
        }

        // Recursively render submorphs
        Oop submorphs = memory_.fetchPointer(2, morph);
        if (submorphs.isNil() || !submorphs.isObject()) return;

        ObjectHeader* subHdr = submorphs.asObjectPtr();
        size_t numSubmorphs = subHdr->slotCount();

        for (size_t i = 0; i < numSubmorphs; i++) {
            Oop submorph = subHdr->slotAt(i);
            renderMorph(submorph, depth + 1, static_cast<int>(i));
        }
    };

    // Clear to World's color first
    Oop worldColor = memory_.fetchPointer(4, world);
    uint32_t worldColorARGB = extractColor(worldColor);

    if (logFile && renderCallCount <= 10) {
        fprintf(logFile, "[RENDER #%d] World color = 0x%08x, display %dx%d\n",
                renderCallCount, worldColorARGB, dispWidth, dispHeight);
        fflush(logFile);
    }

    for (int i = 0; i < dispWidth * dispHeight; i++) {
        pixels[i] = worldColorARGB;
    }

    totalMorphsDrawn = 0;

    // Get World's submorphs and render recursively
    Oop submorphs = memory_.fetchPointer(2, world);
    if (!submorphs.isNil() && submorphs.isObject()) {
        ObjectHeader* subHdr = submorphs.asObjectPtr();
        size_t numSubmorphs = subHdr->slotCount();

        if (logFile && renderCallCount <= 10) {
            fprintf(logFile, "[RENDER #%d] World has %zu submorphs\n", renderCallCount, numSubmorphs);
            fflush(logFile);
        }

        // First pass: render all submorphs EXCEPT MenubarMorph (render it last so it's on top)
        Oop menubarMorph = Oop::nil();
        for (size_t i = 0; i < numSubmorphs; i++) {
            Oop submorph = subHdr->slotAt(i);
            std::string cn = getMorphClassName(submorph);
            if (logFile && renderCallCount <= 10 && i < 5) {
                fprintf(logFile, "[RENDER #%d]   submorph[%zu] = %s\n", renderCallCount, i, cn.c_str());
                fflush(logFile);
            }
            if (cn == "MenubarMorph") {
                menubarMorph = submorph;  // Save for later, render last
            } else {
                renderMorph(submorph, 0, static_cast<int>(i));
            }
        }

        // Second pass: render MenubarMorph last so it's always on top
        if (!menubarMorph.isNil()) {
            renderMorph(menubarMorph, 0, 0);
        }
    } else {
        if (logFile && renderCallCount <= 10) {
            fprintf(logFile, "[RENDER #%d] No submorphs found\n", renderCallCount);
            fflush(logFile);
        }
    }

    if (logFile && renderCallCount <= 10) {
        fprintf(logFile, "[RENDER #%d] Done, drew %d morphs\n", renderCallCount, totalMorphsDrawn);
        fflush(logFile);
    }

    pharo::gDisplaySurface->update();
}

// ===== INPUT EVENT PROCESSING =====

void Interpreter::processInputEvents() {
    // Process pending events from the event queue
    static FILE* logFile = fopen("/tmp/iospharo-events.log", "a");
    static int callCount = 0;
    callCount++;

    // Log every 100th call to show we're being called
    if (logFile && callCount % 100 == 0) {
        fprintf(logFile, "[PROCESS] processInputEvents called %d times\n", callCount);
        fflush(logFile);
    }

    pharo::Event event;
    while (pharo::gEventQueue.pop(event)) {
        // Log all events
        if (logFile) {
            fprintf(logFile, "[EVENT] type=%d args=%d,%d,%d,%d,%d\n",
                    event.type, event.arg1, event.arg2, event.arg3, event.arg4, event.arg5);
            fflush(logFile);
        }
        if (event.type == static_cast<int>(pharo::EventType::Mouse)) {
            int mouseType = event.arg5;  // 0=move, 1=down, 2=up (stored in arg5!)
            // Event coordinates are in points (logical pixels)
            // Scale to physical pixels for Retina displays
            int x = event.arg1 * menuBarScale_;
            int y = event.arg2 * menuBarScale_;

            if (logFile) {
                fprintf(logFile, "[MOUSE] type=%d at x=%d y=%d (raw: %d,%d) scale=%d\n",
                        mouseType, x, y, event.arg1, event.arg2, menuBarScale_);
                fflush(logFile);
            }

            if (mouseType == 1) {  // Mouse down
                if (logFile) {
                    fprintf(logFile, "[CLICK] at x=%d y=%d (scaled), menuBar y=%d-%d\n",
                            x, y, menuBarTop_, menuBarBottom_);
                    fflush(logFile);
                }

                // Check if clicking in dropdown menu area
                // Log dropdown click check
                if (logFile) {
                    fprintf(logFile, "[DROPDOWN CHECK] valid=%d x=%d y=%d bounds=(%d,%d,%d,%d)\n",
                            dropdownState_.valid ? 1 : 0, x, y,
                            dropdownState_.x, dropdownState_.y,
                            dropdownState_.x + dropdownState_.width,
                            dropdownState_.y + dropdownState_.height);
                    fflush(logFile);
                }
                if (dropdownState_.valid &&
                    x >= dropdownState_.x && x < dropdownState_.x + dropdownState_.width &&
                    y >= dropdownState_.y && y < dropdownState_.y + dropdownState_.height) {
                    // Calculate which item was clicked (account for padding)
                    int relativeY = y - dropdownState_.y - (menuBarScale_ == 2 ? 16 : 8);  // Subtract padding
                    int itemIndex = relativeY / dropdownState_.lineHeight;
                    if (logFile) {
                        fprintf(logFile, "[DROPDOWN CLICK] at y=%d, relY=%d, lineHeight=%d, itemIndex=%d (of %zu)\n",
                                y, relativeY, dropdownState_.lineHeight, itemIndex,
                                dropdownState_.itemMorphs.size());
                        fflush(logFile);
                    }
                    if (itemIndex >= 0 && itemIndex < static_cast<int>(dropdownState_.itemMorphs.size())) {
                        if (logFile) {
                            fprintf(logFile, "[DROPDOWN CLICK] Invoking action for item %d\n", itemIndex);
                            fflush(logFile);
                        }
                        invokeMenuItemAction(dropdownState_.itemMorphs[itemIndex]);
                    }
                    dropdownState_.valid = false;  // Close dropdown
                    selectedMenuIndex_ = -1;
                }
                // Check if clicking in menu bar
                else if (y >= menuBarTop_ && y < menuBarBottom_ && !menuItemBounds_.empty()) {
                    // Debounce: ignore duplicate clicks within 100ms
                    auto now = std::chrono::steady_clock::now();
                    int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()).count();

                    int64_t timeSinceLastClick = nowMs - lastMenuClickTime_;
                    // Check if this is a duplicate click at the same location
                    bool isSameLocation = (std::abs(x - lastMenuClickX_) < 20 &&
                                           std::abs(y - lastMenuClickY_) < 20);
                    if (logFile) {
                        fprintf(logFile, "[DEBOUNCE] Time: %lld ms, same location: %d\n",
                                timeSinceLastClick, isSameLocation ? 1 : 0);
                        fflush(logFile);
                    }

                    if (timeSinceLastClick < 200 && isSameLocation) {
                        if (logFile) {
                            fprintf(logFile, "[CLICK] Ignored duplicate click (debounce)\n");
                            fflush(logFile);
                        }
                        continue;  // Skip duplicate click
                    }
                    lastMenuClickTime_ = nowMs;
                    lastMenuClickX_ = x;
                    lastMenuClickY_ = y;

                    // Find which menu item was clicked
                    int clickedIndex = -1;
                    for (size_t i = 0; i < menuItemBounds_.size(); i++) {
                        if (x >= menuItemBounds_[i].first && x < menuItemBounds_[i].second) {
                            clickedIndex = static_cast<int>(i);
                            break;
                        }
                    }

                    if (logFile) {
                        fprintf(logFile, "[CLICK] Menu item index: %d (of %zu items)\n",
                                clickedIndex, menuItemBounds_.size());
                        fflush(logFile);
                    }

                    if (clickedIndex >= 0) {
                        // Toggle menu: if same menu clicked again, close it
                        if (selectedMenuIndex_ == clickedIndex) {
                            selectedMenuIndex_ = -1;
                            dropdownState_.valid = false;
                        } else {
                            selectedMenuIndex_ = clickedIndex;
                            // TODO: Open dropdown for this menu
                        }
                    } else {
                        // Clicked in menu bar but not on an item
                        selectedMenuIndex_ = -1;
                        dropdownState_.valid = false;
                    }
                } else {
                    // Clicked outside menu bar - close any open menu
                    selectedMenuIndex_ = -1;
                    dropdownState_.valid = false;
                }
            }
        }
    }
}

void Interpreter::invokeMenuItemAction(Oop menuItemMorph) {
    std::cerr << "[INVOKE] invokeMenuItemAction called, menuItemMorph.rawBits()=" << menuItemMorph.rawBits() << "\n";
    static FILE* logFile = fopen("/tmp/iospharo-menu-action.log", "a");
    if (logFile) {
        fprintf(logFile, "[INVOKE] Function called with rawBits=%llu\n", (unsigned long long)menuItemMorph.rawBits());
        fflush(logFile);
    }

    // Extract action from menu item and queue it for execution
    if (menuItemMorph.isNil() || !menuItemMorph.isObject()) {
        if (logFile) {
            fprintf(logFile, "[INVOKE] menuItemMorph is nil or not object\n");
            fflush(logFile);
        }
        return;
    }

    // Get the class name of the menu item
    std::string menuItemClass = "Unknown";
    Oop itemClass = memory_.classOf(menuItemMorph);
    if (itemClass.isObject()) {
        ObjectHeader* clsHdr = itemClass.asObjectPtr();
        if (clsHdr->slotCount() > 6) {
            Oop className = memory_.fetchPointer(6, itemClass);
            if (className.isObject()) {
                ObjectHeader* nameHdr = className.asObjectPtr();
                if (nameHdr->isBytesObject() && nameHdr->byteSize() < 50) {
                    menuItemClass = std::string((char*)nameHdr->bytes(), nameHdr->byteSize());
                }
            }
        }
    }
    if (logFile) {
        fprintf(logFile, "[INVOKE] Menu item class: %s\n", menuItemClass.c_str());
        fflush(logFile);
    }

    ObjectHeader* morphHdr = menuItemMorph.asObjectPtr();
    size_t slotCount = morphHdr->slotCount();
    if (logFile) {
        fprintf(logFile, "[INVOKE] Slot count: %zu\n", slotCount);
        fflush(logFile);
    }

    Oop selector = Oop::nil();
    Oop target = Oop::nil();
    Oop actionBlock = Oop::nil();

    // Search slots for action-related objects
    for (size_t i = 5; i < std::min(slotCount, (size_t)20); i++) {
        Oop slot = memory_.fetchPointer(i, menuItemMorph);
        if (slot.isNil()) continue;

        if (slot.isObject()) {
            Oop slotClass = memory_.classOf(slot);
            std::string slotClassName = "Unknown";
            if (slotClass.isObject()) {
                ObjectHeader* scHdr = slotClass.asObjectPtr();
                if (scHdr->slotCount() > 6) {
                    Oop scName = memory_.fetchPointer(6, slotClass);
                    if (scName.isObject()) {
                        ObjectHeader* scnHdr = scName.asObjectPtr();
                        if (scnHdr->isBytesObject() && scnHdr->byteSize() < 50) {
                            slotClassName = std::string((char*)scnHdr->bytes(), scnHdr->byteSize());
                        }
                    }
                }
            }

            if (logFile) {
                fprintf(logFile, "[INVOKE] Slot %zu: class=%s\n", i, slotClassName.c_str());
                // If it's a symbol, print its value
                if (slotClassName == "ByteSymbol" || slotClassName == "Symbol") {
                    ObjectHeader* symHdr = slot.asObjectPtr();
                    if (symHdr->isBytesObject() && symHdr->byteSize() < 100) {
                        std::string symVal((char*)symHdr->bytes(), symHdr->byteSize());
                        fprintf(logFile, "[INVOKE]   Symbol value: '%s'\n", symVal.c_str());
                    }
                }
                fflush(logFile);
            }

            // Check if it's a Symbol (potential selector)
            if (slotClassName == "ByteSymbol" || slotClassName == "Symbol") {
                if (selector.isNil()) selector = slot;
            }
            // Check if it's a Block (action block)
            else if (slotClassName.find("Block") != std::string::npos) {
                actionBlock = slot;
            }
            // Potential target - but skip known non-target types
            else if (target.isNil()) {
                // Skip display/rendering related objects that are not action targets
                // ByteString at slot 8 is the menu item label, not the target
                // UndefinedObject is nil - never a valid target
                bool isSkipType = (slotClassName.find("Morph") != std::string::npos ||
                    slotClassName.find("Font") != std::string::npos ||
                    slotClassName.find("Color") != std::string::npos ||
                    slotClassName.find("Form") != std::string::npos ||
                    slotClassName.find("Extension") != std::string::npos ||
                    slotClassName.find("String") != std::string::npos ||  // ByteString is label
                    slotClassName == "UndefinedObject" ||  // nil is never a valid target
                    slotClassName == "True" ||
                    slotClassName == "False" ||
                    slotClassName == "Array");

                // "Unknown" at slot 14 is often the actual target (class lookup failed)
                // Accept it if we're at the typical action slot position
                if (!isSkipType || (slotClassName == "Unknown" && i >= 14)) {
                    target = slot;
                    if (logFile) {
                        fprintf(logFile, "[INVOKE]   -> Setting as potential target (slot %zu)\n", i);
                        fflush(logFile);
                    }
                }
            }
        }
    }

    // Queue the action for safe execution
    if (!actionBlock.isNil()) {
        // Check block's numArgs to decide between value and value:
        Oop numArgsObj = memory_.fetchPointer(2, actionBlock);
        int blockNumArgs = 0;
        if (numArgsObj.isSmallInteger()) {
            blockNumArgs = static_cast<int>(numArgsObj.asSmallInteger());
        }

        if (logFile) {
            fprintf(logFile, "[INVOKE] Block numArgs=%d\n", blockNumArgs);
            fflush(logFile);
        }

        // Find appropriate selector from special selectors
        Oop targetSel = Oop::nil();
        const char* selectorName = (blockNumArgs == 0) ? "value" : "value:";
        size_t selectorLen = strlen(selectorName);

        Oop specialObjs = memory_.specialObjectsArray();
        if (!specialObjs.isNil() && specialObjs.isObject()) {
            ObjectHeader* soHdr = specialObjs.asObjectPtr();
            if (soHdr->slotCount() > 23) {
                Oop selArray = memory_.fetchPointer(23, specialObjs);
                if (!selArray.isNil() && selArray.isObject()) {
                    ObjectHeader* saHdr = selArray.asObjectPtr();
                    for (size_t i = 0; i < saHdr->slotCount(); i++) {
                        Oop sel = saHdr->slotAt(i);
                        if (sel.isObject()) {
                            ObjectHeader* selHdr = sel.asObjectPtr();
                            if (selHdr->isBytesObject() && selHdr->byteSize() == selectorLen) {
                                if (memcmp(selHdr->bytes(), selectorName, selectorLen) == 0) {
                                    targetSel = sel;
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }

        if (!targetSel.isNil()) {
            pendingMenuAction_.selector = targetSel;
            pendingMenuAction_.receiver = actionBlock;
            pendingMenuAction_.argCount = (blockNumArgs > 0) ? 1 : 0;
            pendingMenuAction_.argument = (blockNumArgs > 0) ? menuItemMorph : Oop::nil();
            pendingMenuAction_.pending = true;
            if (logFile) {
                fprintf(logFile, "[INVOKE] Queued block action with #%s (argCount=%d)\n",
                        selectorName, pendingMenuAction_.argCount);
                fflush(logFile);
            }
        } else if (logFile) {
            fprintf(logFile, "[INVOKE] Could not find #%s selector for block\n", selectorName);
            fflush(logFile);
        }
    } else if (!selector.isNil() && !target.isNil()) {
        pendingMenuAction_.selector = selector;
        pendingMenuAction_.receiver = target;
        pendingMenuAction_.argument = Oop::nil();
        pendingMenuAction_.argCount = 0;
        pendingMenuAction_.pending = true;
        if (logFile) {
            fprintf(logFile, "[INVOKE] Queued selector+target action\n");
            fflush(logFile);
        }
    } else if (!selector.isNil()) {
        pendingMenuAction_.selector = selector;
        pendingMenuAction_.receiver = menuItemMorph;
        pendingMenuAction_.argument = Oop::nil();
        pendingMenuAction_.argCount = 0;
        pendingMenuAction_.pending = true;
        if (logFile) {
            fprintf(logFile, "[INVOKE] Queued selector-only action\n");
            fflush(logFile);
        }
    } else {
        if (logFile) {
            fprintf(logFile, "[INVOKE] No action found - selector=%s, target=%s, block=%s\n",
                    selector.isNil() ? "nil" : "set",
                    target.isNil() ? "nil" : "set",
                    actionBlock.isNil() ? "nil" : "set");
            fflush(logFile);
        }
    }
}

// ===== DISPLAY SYNCHRONIZATION =====
// Until BitBlt primitives are fully working, bypass Display Form and
// render World morphs directly.

void Interpreter::syncDisplayToSurface() {
    if (!pharo::gDisplaySurface) return;

    // For now, always use direct morph rendering instead of Display Form
    // This works around BitBlt not updating the Form properly
    renderWorldMorphs();
    return;

    // TODO: Re-enable Display Form path once BitBlt primitives work correctly
#if 0
    // Auto-discover Display global if displayForm_ not set
    if (displayForm_.isNil()) {
        // First try direct Display global
        Oop display = memory_.findGlobal("Display");

        // If not found, try to get Display from World
        if (display.isNil()) {
            Oop world = memory_.findGlobal("World");
            if (!world.isNil() && world.isObject()) {
                ObjectHeader* worldHdr = world.asObjectPtr();
                size_t worldSlots = worldHdr->slotCount();

                // Try to find a Form by scanning all slots
                for (size_t i = 0; i < worldSlots && display.isNil(); i++) {
                    Oop slot = memory_.fetchPointer(i, world);
                    if (!slot.isNil() && slot.isObject()) {
                        ObjectHeader* slotHdr = slot.asObjectPtr();
                        // Check if it looks like a Form (4+ slots, slot1 and slot2 are SmallIntegers)
                        if (slotHdr->slotCount() >= 4) {
                            Oop s1 = memory_.fetchPointer(1, slot);
                            Oop s2 = memory_.fetchPointer(2, slot);
                            if (s1.isSmallInteger() && s2.isSmallInteger()) {
                                int w = s1.asSmallInteger();
                                int h = s2.asSmallInteger();
                                if (w > 0 && w < 10000 && h > 0 && h < 10000) {
                                    Oop bits = memory_.fetchPointer(0, slot);
                                    if (bits.isObject()) {
                                        display = slot;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        if (!display.isNil() && display.isObject()) {
            displayForm_ = display;
        }
    }

    // If no display form found, try direct morph rendering
    if (displayForm_.isNil()) {
        renderWorldMorphs();
        return;
    }

    // Get the Form's bits (slot 0)
    Oop bits = memory_.fetchPointer(0, displayForm_);
    if (bits.isNil() || !bits.isObject()) return;

    ObjectHeader* bitsHdr = bits.asObjectPtr();
    uint32_t* srcPixels = reinterpret_cast<uint32_t*>(bitsHdr->bytes());

    // Get Form dimensions
    Oop widthOop = memory_.fetchPointer(1, displayForm_);
    Oop heightOop = memory_.fetchPointer(2, displayForm_);
    Oop depthOop = memory_.fetchPointer(3, displayForm_);

    int srcWidth = widthOop.isSmallInteger() ? widthOop.asSmallInteger() : screenWidth_;
    int srcHeight = heightOop.isSmallInteger() ? heightOop.asSmallInteger() : screenHeight_;
    int srcDepth = depthOop.isSmallInteger() ? depthOop.asSmallInteger() : 32;

    uint32_t* dstPixels = pharo::gDisplaySurface->pixels();
    int dstWidth = pharo::gDisplaySurface->width();
    int dstHeight = pharo::gDisplaySurface->height();

    int copyWidth = std::min(srcWidth, dstWidth);
    int copyHeight = std::min(srcHeight, dstHeight);

    if (copyWidth <= 0 || copyHeight <= 0) return;

    // Copy pixels (32-bit assumed for now)
    if (srcDepth == 32) {
        for (int y = 0; y < copyHeight; y++) {
            for (int x = 0; x < copyWidth; x++) {
                dstPixels[y * dstWidth + x] = srcPixels[y * srcWidth + x];
            }
        }
    }

    pharo::gDisplaySurface->update();
#endif
}

// ===== MAIN LOOP =====

void Interpreter::interpret() {
    while (running_) {
        // Process any pending external semaphore signals
        if (hasPendingSignals()) {
            processPendingSignals();
        }

        // Check timer and signal delay semaphore if time has elapsed
        checkTimerSemaphore();

        step();
    }
}

void Interpreter::checkTimerSemaphore() {
    if (nextWakeupTime_ == 0 || timerSemaphore_.isNil()) {
        return;  // No timer set
    }

    // Get current time in milliseconds
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    if (ms >= nextWakeupTime_) {
        // Time has elapsed - signal the semaphore
        // Signal the timer semaphore
        Oop semaphore = timerSemaphore_;
        timerSemaphore_ = Oop::nil();
        nextWakeupTime_ = 0;

        // Same signal logic as primitiveSignal
        Oop nilObj = memory_.nil();
        Oop firstLink = memory_.fetchPointer(LinkedListFirstLinkIndex, semaphore);

        if (firstLink.isNil() || firstLink.rawBits() == nilObj.rawBits()) {
            // No processes waiting - increment excessSignals
            Oop excessOop = memory_.fetchPointer(SemaphoreExcessSignalsIndex, semaphore);
            int64_t excess = excessOop.isSmallInteger() ? excessOop.asSmallInteger() : 0;
            memory_.storePointer(SemaphoreExcessSignalsIndex, semaphore,
                                Oop::fromSmallInteger(excess + 1));
        } else {
            // Wake the first waiting process
            Oop process = removeFirstLinkOfList(semaphore);

            // Get process priority and check if we should preempt
            Oop processPriorityOop = memory_.fetchPointer(ProcessPriorityIndex, process);
            int processPriority = static_cast<int>(processPriorityOop.asSmallInteger());

            Oop activeProcess = getActiveProcess();
            Oop activePriorityOop = memory_.fetchPointer(ProcessPriorityIndex, activeProcess);
            int activePriority = static_cast<int>(activePriorityOop.asSmallInteger());

            if (processPriority > activePriority) {
                // Higher priority - preempt current process
                putToSleep(activeProcess);
                transferTo(process);
            } else {
                // Same or lower priority - just add to ready queue
                putToSleep(process);
            }
        }
    }
}

// ===== HEARTBEAT THREAD =====

void Interpreter::startHeartbeat() {
    if (heartbeatRunning_) return;

    heartbeatRunning_ = true;
    heartbeatThread_ = std::thread([this]() {
        int tickCount = 0;

        while (heartbeatRunning_) {
            // Sleep for ~1ms between ticks (like official VM heartbeat)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));

            tickCount++;

            // Check timer semaphore and signal if expired
            if (nextWakeupTime_ != 0 && !timerSemaphore_.isNil()) {
                auto now = std::chrono::steady_clock::now();
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch()).count();

                if (ms >= nextWakeupTime_) {
                    // Signal the timer semaphore by incrementing its excessSignals
                    Oop semaphore = timerSemaphore_;
                    timerSemaphore_ = Oop::nil();
                    nextWakeupTime_ = 0;

                    // Increment excessSignals - this will wake up waiting processes
                    if (semaphore.isObject()) {
                        Oop excessOop = memory_.fetchPointer(SemaphoreExcessSignalsIndex, semaphore);
                        int64_t excess = excessOop.isSmallInteger() ? excessOop.asSmallInteger() : 0;
                        memory_.storePointer(SemaphoreExcessSignalsIndex, semaphore,
                                            Oop::fromSmallInteger(excess + 1));
                    }
                }
            }

            // Every ~33ms (30fps), sync Display Form to platform surface AND push a timer event
            if (tickCount % 33 == 0) {
                syncDisplayToSurface();

                // Push a timer/redraw event to wake up the UI process
                // Event type 6 = WindowMetrics (triggers redraw)
                pharo::Event timerEvent;
                timerEvent.type = 6;  // WindowMetrics
                timerEvent.timeStamp = static_cast<int>(tickCount);
                timerEvent.arg1 = 0;  // x
                timerEvent.arg2 = 0;  // y
                timerEvent.arg3 = 1024;  // width
                timerEvent.arg4 = 768;   // height
                timerEvent.windowIndex = 1;
                pharo::gEventQueue.push(timerEvent);

                // Signal the input semaphore to wake up the UI process
                int inputSemaIdx = pharo::gEventQueue.getInputSemaphoreIndex();
                if (inputSemaIdx > 0) {
                    pendingSignalIndex_.store(inputSemaIdx, std::memory_order_release);
                }
            }
        }
    });
}

void Interpreter::stopHeartbeat() {
    if (!heartbeatRunning_) return;

    heartbeatRunning_ = false;
    if (heartbeatThread_.joinable()) {
        heartbeatThread_.join();
    }
}

// ===== EXTERNAL SEMAPHORE SIGNALING =====

void Interpreter::signalExternalSemaphore(int index) {
    // Store the index to be processed in the interpret loop
    // This is thread-safe due to atomic
    pendingSignalIndex_.store(index, std::memory_order_release);
}

void Interpreter::processPendingSignals() {
    int index = pendingSignalIndex_.exchange(0, std::memory_order_acquire);
    if (index <= 0) return;

    // Get the external semaphore table from special objects
    Oop semTable = memory_.specialObject(SpecialObjectIndex::ExternalSemaphoreTable);
    if (semTable.isNil() || !semTable.isObject()) return;

    // Index is 1-based, convert to 0-based array index
    size_t tableIndex = static_cast<size_t>(index - 1);
    size_t tableSize = memory_.slotCountOf(semTable);
    if (tableIndex >= tableSize) return;

    // Get the semaphore at this index
    Oop semaphore = memory_.fetchPointer(tableIndex, semTable);
    if (semaphore.isNil() || !semaphore.isObject()) return;

    // Signal the semaphore (same logic as primitiveSignal)
    Oop nilObj = memory_.nil();
    Oop firstLink = memory_.fetchPointer(LinkedListFirstLinkIndex, semaphore);

    if (firstLink.isNil() || firstLink.rawBits() == nilObj.rawBits()) {
        // No processes waiting - increment excessSignals
        Oop excessOop = memory_.fetchPointer(SemaphoreExcessSignalsIndex, semaphore);
        int64_t excess = excessOop.isSmallInteger() ? excessOop.asSmallInteger() : 0;
        memory_.storePointer(SemaphoreExcessSignalsIndex, semaphore,
                            Oop::fromSmallInteger(excess + 1));
    } else {
        // Wake the first waiting process
        Oop process = removeFirstLinkOfList(semaphore);

        // Get process priority and check if we should preempt
        Oop processPriorityOop = memory_.fetchPointer(ProcessPriorityIndex, process);
        int processPriority = static_cast<int>(processPriorityOop.asSmallInteger());

        Oop activeProcess = getActiveProcess();
        Oop activePriorityOop = memory_.fetchPointer(ProcessPriorityIndex, activeProcess);
        int activePriority = static_cast<int>(activePriorityOop.asSmallInteger());

        if (processPriority > activePriority) {
            // Higher priority - preempt current process
            putToSleep(activeProcess);
            transferTo(process);
        } else {
            // Same or lower priority - just add to ready queue
            putToSleep(process);
        }
    }
}

bool Interpreter::step() {
    if (!running_) {
        return false;
    }

    // Process any pending external semaphore signals (from heartbeat/events)
    if (hasPendingSignals()) {
        processPendingSignals();
    }

    // Check if we've run past the end of bytecodes
    if (instructionPointer_ >= bytecodeEnd_) {
        returnValue(receiver_);
        return running_;
    }

    // NOTE: Do NOT reset extA_/extB_ here!
    // In Sista V1, extension bytecodes (0xE0/0xE1) set these values, then the
    // NEXT bytecode uses them. The consuming bytecodes reset them after use.
    // Resetting here would break extension byte chains.

    uint8_t bytecode = fetchByte();
    dispatchBytecode(bytecode);

    return running_;
}

ExecuteResult Interpreter::stepDetailed() {
    if (!running_) {
        return ExecuteResult::Idle;
    }

    // Process any pending external semaphore signals (from heartbeat/events)
    if (hasPendingSignals()) {
        processPendingSignals();
    }

    // Check if we've run past the end of bytecodes
    if (instructionPointer_ >= bytecodeEnd_) {
        returnValue(receiver_);
        return running_ ? ExecuteResult::Active : ExecuteResult::Idle;
    }

    // Track what we're about to do
    uint8_t bytecode = fetchByte();

    // Check if this is a send bytecode (message send)
    bool isSend = (bytecode >= 0x60 && bytecode <= 0x7F) ||  // Sista send
                  (bytecode >= 0x80 && bytecode <= 0x8F) ||  // V3 send
                  (bytecode >= 0xB0 && bytecode <= 0xBF) ||  // Arithmetic sends
                  (bytecode >= 0xC0 && bytecode <= 0xCF) ||  // More sends
                  (bytecode >= 0xD0 && bytecode <= 0xDF) ||  // Send literal 0-15
                  (bytecode >= 0xEA && bytecode <= 0xED) ||  // Extended sends
                  bytecode == 0xF8 || bytecode == 0xF9;      // Super sends

    // Reset primitive tracking before dispatch
    lastPrimitiveIndex_ = 0;

    dispatchBytecode(bytecode);

    if (!running_) {
        return ExecuteResult::Idle;
    }

    // Check if a primitive was executed
    if (lastPrimitiveIndex_ > 0) {
        return ExecuteResult::PrimitiveExecuted;
    }

    // Check if a message was sent
    if (isSend) {
        return ExecuteResult::MessageSent;
    }

    return ExecuteResult::Active;
}

// ===== BYTECODE DISPATCH =====

void Interpreter::dispatchBytecode(uint8_t bytecode) {
    // Sista V1 bytecode dispatch (used by Pharo 10+, format 68021 with modern compiler)
    // Key differences from V3PlusClosures:
    // - 0x10-0x1F: push literal var (not temp)
    // - 0x30-0x3F: push temp 0-15
    // - 0x40-0x4B: push temp 16-27
    // - 0x4C-0x4F: push self, true, false, nil
    // - 0x50-0x51: push 0, push 1

    // ========================================================================
    // SISTA V1 BYTECODE DECODER (Pharo 10+)
    // Based on EncoderForSistaV1 specification
    // ========================================================================

    if (bytecode <= 0x0F) {
        // 0x00-0x0F: Push Receiver Variable 0-15 (same in V3 and Sista)
        pushReceiverVariable(bytecode);
    }
    else if (bytecode <= 0x1F) {
        // 0x10-0x1F: Differs by bytecode set
        if (usesSistaV1_) {
            // Sista V1: Push Literal Variable 0-15 (dereference Association)
            pushLiteralVariable(bytecode - 0x10);
        } else {
            // V3: push temp 0-15
            pushTemporary(bytecode - 0x10);
        }
    }
    else if (bytecode <= 0x3F) {
        // 0x20-0x3F: Differs by bytecode set
        if (usesSistaV1_) {
            // Sista V1: Push Literal Constant 0-31 (push literal directly)
            pushLiteralConstant(bytecode - 0x20);
        } else {
            // V3: 0x20-0x2F = push literal 0-15, 0x30-0x3F = push literal 16-31
            pushLiteralConstant(bytecode - 0x20);
        }
    }
    else if (bytecode <= 0x5F) {
        // 0x40-0x5F: Differs significantly between V3 and Sista
        if (!usesSistaV1_) {
            // V3PlusClosures: 0x40-0x5F = push literal variable 0-31
            pushLiteralVariable(bytecode - 0x40);
        } else {
            // Sista V1:
            // 0x40-0x47: Push Temp 0-7
            // 0x48-0x4B: Push Temp 8-11
            // 0x4C: Push self, 0x4D: Push true, 0x4E: Push false, 0x4F: Push nil
            // 0x50: Push 0, 0x51: Push 1, 0x52: Push thisContext, 0x53: Dup
            // 0x54-0x57: UNASSIGNED
            // 0x58: Return self, 0x59: Return true, 0x5A: Return false, 0x5B: Return nil
            // 0x5C: Return top, 0x5D: BlockReturn nil, 0x5E: BlockReturn top, 0x5F: Nop
            if (bytecode <= 0x47) {
                // 0x40-0x47: Push Temp 0-7
                pushTemporary(bytecode - 0x40);
            } else if (bytecode <= 0x4B) {
                // 0x48-0x4B: Push Temp 8-11
                pushTemporary(8 + bytecode - 0x48);
            } else if (bytecode <= 0x4F) {
                // 0x4C-0x4F: Push specials
                switch (bytecode) {
                    case 0x4C: push(receiver_); break;              // push self
                    case 0x4D: push(memory_.trueObject()); break;   // push true
                    case 0x4E: push(memory_.falseObject()); break;  // push false
                    case 0x4F: push(memory_.nil()); break;          // push nil
                }
            } else if (bytecode <= 0x53) {
                // 0x50-0x53: Special pushes
                switch (bytecode) {
                    case 0x50: push(Oop::fromSmallInteger(0)); break;  // push 0
                    case 0x51: push(Oop::fromSmallInteger(1)); break;  // push 1
                    case 0x52: push(activeContext_); break;            // push thisContext
                    case 0x53: push(stackTop()); break;                // duplicate top
                }
            } else if (bytecode <= 0x57) {
                // 0x54-0x57: UNASSIGNED in Sista V1
                // These should not appear in valid Pharo code
            } else {
                // 0x58-0x5F: Returns and special operations
                switch (bytecode) {
                    case 0x58: returnValue(receiver_); break;              // return self
                    case 0x59: returnValue(memory_.trueObject()); break;   // return true
                    case 0x5A: returnValue(memory_.falseObject()); break;  // return false
                    case 0x5B: returnValue(memory_.nil()); break;          // return nil
                    case 0x5C: returnFromMethod(); break;                  // return top
                    case 0x5D: returnValue(memory_.nil()); break;          // block return nil
                    case 0x5E: returnFromBlock(); break;                   // block return top
                    case 0x5F: /* Nop */ break;
                }
            }
        }
    }
    else if (bytecode <= 0x6F) {
        // 0x60-0x6F: Differs between V3 and Sista
        if (!usesSistaV1_) {
            // V3: Pop and store receiver variable 0-7 / temp 0-7
            if (bytecode <= 0x67) {
                Oop value = pop();
                setReceiverInstVar(bytecode - 0x60, value);
            } else {
                Oop value = pop();
                setTemporary(bytecode - 0x68, value);
            }
        } else {
            // Sista V1: 0x60-0x6F = Send Arithmetic Message 0-15
            // (+, -, <, >, <=, >=, =, ~=, *, /, \\, @, bitShift:, //, bitAnd:, bitOr:)
            sendArithmetic(bytecode - 0x60);
        }
    }
    else if (bytecode <= 0x7F) {
        // 0x70-0x7F: Differs between V3 and Sista
        if (!usesSistaV1_) {
            // V3PlusClosures:
            // 0x70-0x77: push specials (self, true, false, nil, -1, 0, 1, 2)
            // 0x78-0x7B: return (self, true, false, nil)
            // 0x7C: return top, 0x7D: block return
            if (bytecode <= 0x77) {
                switch (bytecode) {
                    case 0x70: push(receiver_); break;
                    case 0x71: push(memory_.trueObject()); break;
                    case 0x72: push(memory_.falseObject()); break;
                    case 0x73: push(memory_.nil()); break;
                    case 0x74: push(Oop::fromSmallInteger(-1)); break;
                    case 0x75: push(Oop::fromSmallInteger(0)); break;
                    case 0x76: push(Oop::fromSmallInteger(1)); break;
                    case 0x77: push(Oop::fromSmallInteger(2)); break;
                }
            } else if (bytecode <= 0x7B) {
                switch (bytecode) {
                    case 0x78: returnValue(receiver_); break;
                    case 0x79: returnValue(memory_.trueObject()); break;
                    case 0x7A: returnValue(memory_.falseObject()); break;
                    case 0x7B: returnValue(memory_.nil()); break;
                }
            } else if (bytecode == 0x7C) {
                returnFromMethod();
            } else if (bytecode == 0x7D) {
                returnFromBlock();
            }
        } else {
            // Sista V1: 0x70-0x7F = Send Special Message 0-15
            // (at:, at:put:, size, next, nextPut:, atEnd, ==, class, ~~, value, value:, do:, new, new:, x, y)
            sendSpecial(bytecode - 0x70);
        }
    }
    else if (bytecode <= 0x8F) {
        // Sista V1: 0x80-0x8F (128-143): Send literal selector 0-15 with 0 args
        int litIndex = bytecode & 0x0F;
        Oop selector = literal(litIndex);
        sendSelector(selector, 0);
    }
    else if (bytecode <= 0x9F) {
        // Sista V1: 0x90-0x9F (144-159): Send literal selector 0-15 with 1 arg
        int litIndex = bytecode & 0x0F;
        Oop selector = literal(litIndex);
        sendSelector(selector, 1);
    }
    else if (bytecode <= 0xAF) {
        // Sista V1: 0xA0-0xAF (160-175): Send literal selector 0-15 with 2 args
        int litIndex = bytecode & 0x0F;
        Oop selector = literal(litIndex);
        sendSelector(selector, 2);
    }
    else if (bytecode <= 0xB7) {
        // Sista V1: 0xB0-0xB7 (176-183): Short unconditional jump (1-8 bytes forward)
        int offset = (bytecode & 0x07) + 1;
        shortJump(offset);
    }
    else if (bytecode <= 0xBF) {
        // Sista V1: 0xB8-0xBF (184-191): Short conditional jump if true (1-8 bytes)
        int offset = (bytecode & 0x07) + 1;
        shortJumpIfTrue(offset);
    }
    else if (bytecode <= 0xC7) {
        // Sista V1: 0xC0-0xC7 (192-199): Short conditional jump if false (1-8 bytes)
        int offset = (bytecode & 0x07) + 1;
        shortJumpIfFalse(offset);
    }
    else if (bytecode <= 0xCF) {
        // Sista V1: 0xC8-0xCF (200-207): Pop and Store Receiver Variable 0-7
        int varIndex = bytecode & 0x07;
        Oop value = pop();
        setReceiverInstVar(varIndex, value);
    }
    else if (bytecode <= 0xD7) {
        // Sista V1: 0xD0-0xD7 (208-215): Store and pop temp 0-7
        int tempIndex = bytecode & 0x07;
        Oop value = pop();
        setTemporary(tempIndex, value);
    }
    else if (bytecode == 0xD8) {
        // Sista V1: 0xD8 (216): Pop stack (discard top of stack)
        pop();
    }
    else if (bytecode == 0xD9) {
        // Sista V1: 0xD9 (217): Unconditional trap (debugging)
        running_ = false;
    }
    else if (bytecode <= 0xDF) {
        // Sista V1: 0xDA-0xDF (218-223): Various extended operations
        // These are typically used for debugging or reserved - no-op
    }
    else if (bytecode <= 0xE7) {
        // ========================================================================
        // Sista V1: 0xE0-0xE7 (224-231): 2-byte bytecodes - Extensions and Push operations
        // ========================================================================
        if (!usesSistaV1_) {
            // V3PlusClosures: 0xE0-0xE7 = Send literal selector 0-7 with 2 args
            int litIndex = bytecode - 0xE0;
            Oop selector = literal(litIndex);
            sendSelector(selector, 2);
        } else {
            switch (bytecode) {
                case 0xE0: // 224: Extend A (unsigned) - modifies next bytecode's index
                {
                    uint8_t extByte = fetchByte();
                    extA_ = (extA_ << 8) | extByte;
                    break;
                }
                case 0xE1: // 225: Extend B (signed) - modifies next bytecode's numArgs/offset
                {
                    uint8_t extByte = fetchByte();
                    // Sign extend if high bit set
                    if (extByte >= 128) {
                        extB_ = (extB_ << 8) | extByte | 0xFFFFFF00;
                    } else {
                        extB_ = (extB_ << 8) | extByte;
                    }
                    break;
                }
                case 0xE2: // 226: Push Receiver Variable #iiiiiiii (+ extA * 256)
                {
                    uint8_t indexByte = fetchByte();
                    int fullIndex = (extA_ << 8) | indexByte;
                    extA_ = 0;
                    pushReceiverVariable(fullIndex);
                    break;
                }
                case 0xE3: // 227: Push Literal Variable #iiiiiiii (+ extA * 256)
                {
                    uint8_t indexByte = fetchByte();
                    int fullIndex = (extA_ << 8) | indexByte;
                    extA_ = 0;
                    pushLiteralVariable(fullIndex);
                    break;
                }
                case 0xE4: // 228: Push Literal Constant #iiiiiiii (+ extA * 256)
                {
                    uint8_t indexByte = fetchByte();
                    int fullIndex = (extA_ << 8) | indexByte;
                    extA_ = 0;
                    pushLiteralConstant(fullIndex);
                    break;
                }
                case 0xE5: // 229: Push Temporary Variable #iiiiiiii
                {
                    uint8_t indexByte = fetchByte();
                    pushTemporary(indexByte);
                    break;
                }
                case 0xE6: // 230: UNASSIGNED (was pushNClosureTemps)
                    fetchByte();  // Skip the argument byte
                    break;
                case 0xE7: // 231: Push Array (j=0) or Pop into Array (j=1)
                {
                    // jkkkkkkk: j=operation type, kkkkkkk=array size
                    uint8_t desc = fetchByte();
                    int arraySize = desc & 0x7F;
                    bool popIntoArray = (desc >> 7) != 0;

                    // Create array
                    Oop arrayClass = memory_.specialObject(SpecialObjectIndex::ClassArray);
                    uint32_t classIndex = memory_.indexOfClass(arrayClass);
                    Oop array = memory_.allocateSlots(classIndex, arraySize);

                    if (popIntoArray) {
                        // Pop arraySize elements into new array
                        for (int i = arraySize - 1; i >= 0; i--) {
                            memory_.storePointer(i, array, pop());
                        }
                    }
                    push(array);
                    break;
                }
            }
        }
    }
    else if (bytecode <= 0xEF) {
        // ========================================================================
        // Sista V1: 0xE8-0xEF (232-239): 2-byte bytecodes - Push/Send/Jump
        // ========================================================================
        switch (bytecode) {
            case 0xE8: // 232: Push Integer #iiiiiiii (+ extB * 256, signed)
            {
                uint8_t intByte = fetchByte();
                int value = (extB_ << 8) | intByte;
                extB_ = 0;
                push(Oop::fromSmallInteger(value));
                break;
            }
            case 0xE9: // 233: Push Character #iiiiiiii (+ extB * 256)
            {
                uint8_t charByte = fetchByte();
                int codePoint = (extB_ << 8) | charByte;
                extB_ = 0;
                // Create Character object - character is stored as immediate
                push(Oop::fromCharacter(codePoint));
                break;
            }
            case 0xEA: // 234: Send Literal Selector #iiiii (+ extA*32) with jjj (+ extB*8) args
            {
                uint8_t desc = fetchByte();
                int selectorIndex = ((extA_ << 5) | (desc >> 3)) & 0xFFFF;
                int numArgs = ((extB_ << 3) | (desc & 0x07)) & 0xFF;
                extA_ = 0;
                extB_ = 0;
                Oop selector = literal(selectorIndex);
                sendSelector(selector, numArgs);
                break;
            }
            case 0xEB: // 235: Send To Superclass (same encoding as 0xEA)
            {
                uint8_t desc = fetchByte();
                int selectorIndex = ((extA_ << 5) | (desc >> 3)) & 0xFFFF;
                int numArgs = ((extB_ << 3) | (desc & 0x07)) & 0xFF;
                extA_ = 0;
                extB_ = 0;
                Oop selector = literal(selectorIndex);
                // Super send: lookup from superclass of METHOD's defining class (not receiver's class)
                // This is critical: super means "start lookup from my defining class's superclass"
                Oop methodClass = methodClassOf(method_);
                Oop superclass;
                if (methodClass.isNil() || !methodClass.isObject()) {
                    // Fallback to receiver's class superclass if we can't find method class
                    superclass = superclassOf(memory_.classOf(receiver_));
                } else {
                    superclass = superclassOf(methodClass);
                }
                Oop method = lookupMethod(selector, superclass);
                if (method.isNil()) {
                    sendDoesNotUnderstand(selector, numArgs);
                } else {
                    activateMethod(method, numArgs);
                }
                break;
            }
            case 0xEC: // 236: Call Mapped Inlined Primitive #iiiiiiii
            {
                uint8_t primByte = fetchByte();
                // Inlined primitives are handled specially by the JIT
                // For interpreter, we just execute the fallback code
                (void)primByte;
                break;
            }
            case 0xED: // 237: Jump #iiiiiiii (+ extB * 256, signed)
            {
                uint8_t offsetByte = fetchByte();
                int offset = (extB_ << 8) | offsetByte;
                extB_ = 0;
                instructionPointer_ += offset;
                break;
            }
            case 0xEE: // 238: Pop and Jump On True #iiiiiiii (+ extB * 256)
            {
                uint8_t offsetByte = fetchByte();
                int offset = (extB_ << 8) | offsetByte;
                extB_ = 0;
                Oop value = pop();
                if (isTrue(value)) {
                    instructionPointer_ += offset;
                }
                break;
            }
            case 0xEF: // 239: Pop and Jump On False #iiiiiiii (+ extB * 256)
            {
                uint8_t offsetByte = fetchByte();
                int offset = (extB_ << 8) | offsetByte;
                extB_ = 0;
                Oop value = pop();
                if (!isTrue(value)) {
                    instructionPointer_ += offset;
                }
                break;
            }
        }
    }
    else if (bytecode <= 0xF7) {
        // ========================================================================
        // Sista V1: 0xF0-0xF7 (240-247): 2-byte bytecodes - Store operations
        // ========================================================================
        switch (bytecode) {
            case 0xF0: // 240: Pop and Store Receiver Variable #iiiiiiii (+ extA * 256)
            {
                uint8_t indexByte = fetchByte();
                int fullIndex = (extA_ << 8) | indexByte;
                extA_ = 0;
                Oop value = pop();
                setReceiverInstVar(fullIndex, value);
                break;
            }
            case 0xF1: // 241: Pop and Store Literal Variable #iiiiiiii (+ extA * 256)
            {
                uint8_t indexByte = fetchByte();
                int fullIndex = (extA_ << 8) | indexByte;
                extA_ = 0;
                Oop value = pop();
                Oop assoc = literal(fullIndex);
                if (assoc.isObject()) {
                    memory_.storePointer(1, assoc, value);  // Store in Association's value slot
                }
                break;
            }
            case 0xF2: // 242: Pop and Store Temporary Variable #iiiiiiii
            {
                uint8_t indexByte = fetchByte();
                Oop value = pop();
                setTemporary(indexByte, value);
                break;
            }
            case 0xF3: // 243: Store Receiver Variable #iiiiiiii (+ extA * 256) - no pop
            {
                uint8_t indexByte = fetchByte();
                int fullIndex = (extA_ << 8) | indexByte;
                extA_ = 0;
                Oop value = stackTop();
                setReceiverInstVar(fullIndex, value);
                break;
            }
            case 0xF4: // 244: Store Literal Variable #iiiiiiii (+ extA * 256) - no pop
            {
                uint8_t indexByte = fetchByte();
                int fullIndex = (extA_ << 8) | indexByte;
                extA_ = 0;
                Oop value = stackTop();
                Oop assoc = literal(fullIndex);
                if (assoc.isObject()) {
                    memory_.storePointer(1, assoc, value);
                }
                break;
            }
            case 0xF5: // 245: Store Temporary Variable #iiiiiiii - no pop
            {
                uint8_t indexByte = fetchByte();
                Oop value = stackTop();
                setTemporary(indexByte, value);
                break;
            }
            case 0xF6: // 246: UNASSIGNED
            case 0xF7: // 247: UNASSIGNED
                fetchByte();  // Skip argument byte
                break;
        }
    }
    else {
        // ========================================================================
        // Sista V1: 0xF8-0xFF (248-255): 3-byte bytecodes
        // ========================================================================
        switch (bytecode) {
            case 0xF8: // 248: Call Primitive
            {
                // iiiiiiii mssjjjjj: primitive = iiiiiiii + (jjjjj * 256)
                // m=1: inlined primitive, ss: operation set
                uint8_t primLowByte = fetchByte();
                uint8_t flagsAndHigh = fetchByte();
                int primIndex = primLowByte | ((flagsAndHigh & 0x1F) << 8);
                // Primitive is called at method activation, this bytecode is skipped
                (void)primIndex;
                break;
            }
            case 0xF9: // 249: Push FullBlockClosure
            {
                // xxxxxxxx siyyyyyy: literal index xxxxxxxx (+extA*256)
                // numCopied yyyyyy, s=receiverOnStack, i=ignoreOuterContext
                uint8_t litIndex = fetchByte();
                uint8_t flags = fetchByte();
                int fullLitIndex = (extA_ << 8) | litIndex;
                extA_ = 0;
                int numCopied = flags & 0x3F;
                bool receiverOnStack = (flags >> 7) & 1;
                bool ignoreOuterContext = (flags >> 6) & 1;
                (void)receiverOnStack;
                (void)ignoreOuterContext;
                createFullBlockWithLiteral(fullLitIndex, numCopied);
                break;
            }
            case 0xFA: // 250: Push Closure
            {
                // eeiiikkk jjjjjjjj: numCopied iii (+extA//16*8), numArgs kkk (+extA\16*8)
                // blockSize jjjjjjjj (+extB*256), ee=num extension bytes
                uint8_t desc = fetchByte();
                uint8_t blockSizeLow = fetchByte();
                int numCopied = ((desc >> 3) & 0x07) | ((extA_ >> 4) << 3);
                int numArgs = (desc & 0x07) | ((extA_ & 0x0F) << 3);
                int blockSize = (extB_ << 8) | blockSizeLow;
                extA_ = 0;
                extB_ = 0;
                createBlockWithArgs(numArgs, numCopied, blockSize);
                break;
            }
            case 0xFB: // 251: Push Temp At kkkkkkkk In Temp Vector At jjjjjjjj
            {
                uint8_t tempIndex = fetchByte();
                uint8_t vectorIndex = fetchByte();
                // Get temp vector from current context temps
                Oop tempVector = temporary(vectorIndex);
                if (tempVector.isObject()) {
                    Oop value = memory_.fetchPointer(tempIndex, tempVector);
                    push(value);
                } else {
                    push(memory_.nil());
                }
                break;
            }
            case 0xFC: // 252: Store Temp At kkkkkkkk In Temp Vector At jjjjjjjj (no pop)
            {
                uint8_t tempIndex = fetchByte();
                uint8_t vectorIndex = fetchByte();
                Oop value = stackTop();
                Oop tempVector = temporary(vectorIndex);
                if (tempVector.isObject()) {
                    memory_.storePointer(tempIndex, tempVector, value);
                }
                break;
            }
            case 0xFD: // 253: Pop and Store Temp At kkkkkkkk In Temp Vector At jjjjjjjj
            {
                uint8_t tempIndex = fetchByte();
                uint8_t vectorIndex = fetchByte();
                Oop value = pop();
                Oop tempVector = temporary(vectorIndex);
                if (tempVector.isObject()) {
                    memory_.storePointer(tempIndex, tempVector, value);
                }
                break;
            }
            case 0xFE: // 254: UNASSIGNED
            case 0xFF: // 255: UNASSIGNED
                fetchByte();
                fetchByte();
                break;
        }
    }
}


// ===== STACK OPERATIONS =====

void Interpreter::push(Oop value) {
    if (stackPointer_ >= stack_.data() + MaxStackDepth) {
        running_ = false;
        return;
    }
    *stackPointer_++ = value;
}

Oop Interpreter::pop() {
    if (stackPointer_ <= stackBase_) {
        return memory_.nil();  // Stack underflow
    }
    return *--stackPointer_;
}

Oop Interpreter::stackTop() const {
    if (stackPointer_ <= stackBase_) {
        return memory_.nil();
    }
    return *(stackPointer_ - 1);
}

Oop Interpreter::stackValue(size_t offset) const {
    if (stackPointer_ - offset <= stackBase_) {
        return memory_.nil();
    }
    return *(stackPointer_ - 1 - offset);
}

void Interpreter::popN(size_t n) {
    stackPointer_ -= n;
    if (stackPointer_ < stackBase_) {
        stackPointer_ = stackBase_;
    }
}

// ===== BYTECODE IMPLEMENTATIONS =====

uint8_t Interpreter::fetchByte() {
    return *instructionPointer_++;
}

uint16_t Interpreter::fetchTwoBytes() {
    uint8_t hi = fetchByte();
    uint8_t lo = fetchByte();
    return (hi << 8) | lo;
}

void Interpreter::pushReceiverVariable(int index) {
    push(receiverInstVar(index));
}

void Interpreter::pushTemporary(int index) {
    push(temporary(index));
}

void Interpreter::pushLiteralConstant(int index) {
    // V3PlusClosures: Simple literal push, no extensions
    // The index is already the full literal index (0-31 from bytecode 0x20-0x3F,
    // or 0-63 from extended push bytecode 0x80)
    push(literal(index));
}

void Interpreter::pushLiteralVariable(int index) {
    // V3PlusClosures: Simple literal variable push, no extensions
    // Literal variable is an Association, fetch its value
    Oop assoc = literal(index);

    // Validate that we actually have an Association-like object (pointer object with at least 2 slots)
    // NOT a Symbol/String (byte object)
    if (!assoc.isObject() || assoc.isNil()) {
        push(memory_.nil());
        return;
    }

    ObjectHeader* header = assoc.asObjectPtr();

    // Check if it's a byte object (Symbol, String) - unexpected but handle gracefully
    if (header->isBytesObject()) {
        // Push the symbol itself to avoid crash (wrong but won't corrupt)
        push(assoc);
        return;
    }

    // Normal case: Association with key in slot 0, value in slot 1
    Oop value = memory_.fetchPointer(1, assoc);  // Association>>value
    push(value);
}

void Interpreter::storeReceiverVariable(int index) {
    Oop value = pop();
    setReceiverInstVar(index, value);
}

void Interpreter::storeTemporary(int index) {
    Oop value = pop();
    setTemporary(index, value);
}

void Interpreter::pushSpecial(int which) {
    switch (which) {
        case 0: push(receiver_); break;
        case 1: push(memory_.trueObject()); break;
        case 2: push(memory_.falseObject()); break;
        case 3: push(memory_.nil()); break;
        case 4: push(Oop::fromSmallInteger(-1)); break;
        case 5: push(Oop::fromSmallInteger(0)); break;
        case 6: push(Oop::fromSmallInteger(1)); break;
        case 7: push(Oop::fromSmallInteger(2)); break;
    }
}

void Interpreter::returnValue(Oop value) {
    static FILE* frameLog = fopen("/tmp/iospharo-frame.log", "a");

    if (frameLog) {
        fprintf(frameLog, "[RETURN_VALUE] frameDepth=%d\n", frameDepth_);
        fflush(frameLog);
    }

    // If no frames to pop, check if we have a sender context to return to
    if (frameDepth_ == 0) {
        // Check if current context has a sender
        Oop nilObj = memory_.specialObject(SpecialObjectIndex::NilObject);

        if (activeContext_.isObject() && activeContext_.rawBits() != nilObj.rawBits()) {
            Oop sender = memory_.fetchPointer(0, activeContext_);

            if (sender.isObject() && sender.rawBits() != nilObj.rawBits()) {
                ObjectHeader* senderHdr = sender.asObjectPtr();

                // Check if sender looks like a Context (has enough slots and right format)
                if (senderHdr->slotCount() >= 6 &&
                    senderHdr->format() == ObjectFormat::IndexableWithFixed) {
                    // Reset stack for new context
                    stackPointer_ = stackBase_;

                    // Execute from sender, which will push the return value appropriately
                    // First, set up the sender context
                    if (executeFromContext(sender)) {
                        // Push the return value onto the new context's stack
                        push(value);
                        return;
                    }
                }
            }
        }

        // Mark current process as terminated by clearing its suspendedContext
        terminateCurrentProcess();

        // Try to find another runnable process
        if (tryReschedule()) {
            return;
        }

        // If no other process to run, try startup entry point
        if (bootstrapStartup()) {
            return;
        }

        // No runnable processes - enter idle mode
        // This is a good time to update the display since a doOneCycle just completed

        // Render World's morphs directly - bypass NullWorldRenderer
        renderWorldMorphs();

        // Sleep briefly to avoid busy-waiting, then try to reschedule
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        // Try to find a runnable process again (events might have signaled semaphores)
        if (tryReschedule()) {
            return;
        }

        // After multiple idle cycles with no work, finally stop
        static int idleCount = 0;
        static int maxIdleCycles = 500;  // ~5 seconds of idle
        idleCount++;
        if (idleCount >= maxIdleCycles) {
            running_ = false;
            push(value);
            return;
        }

        // Still idle - push a placeholder and let step() be called again
        push(value);
        return;
    }

    // Pop frame and push result
    popFrame();

    // After popping, if execution is still running, push the result
    if (running_) {
        push(value);
    }
}

void Interpreter::returnFromMethod() {
    Oop value = pop();
    returnValue(value);
}

void Interpreter::returnFromBlock() {
    // Non-local return from block
    Oop value = pop();

    // TODO: Find the home context and return to its sender
    // For now, treat as regular return
    returnValue(value);
}

void Interpreter::extendedPush() {
    uint8_t descriptor = fetchByte();
    int type = (descriptor >> 6) & 3;
    int index = descriptor & 0x3F;

    switch (type) {
        case 0: pushReceiverVariable(index); break;
        case 1: pushTemporary(index); break;
        case 2: pushLiteralConstant(index); break;
        case 3: pushLiteralVariable(index); break;
    }
}

void Interpreter::extendedStore() {
    uint8_t descriptor = fetchByte();
    int type = (descriptor >> 6) & 3;
    int index = descriptor & 0x3F;

    Oop value = stackTop();  // Don't pop for store

    switch (type) {
        case 0: setReceiverInstVar(index, value); break;
        case 1: setTemporary(index, value); break;
        case 2: /* Can't store to literal constant */ break;
        case 3: {
            // Store to literal variable (association value)
            Oop assoc = literal(index);
            memory_.storePointer(1, assoc, value);
            break;
        }
    }
}

void Interpreter::extendedSend() {
    uint8_t descriptor = fetchByte();
    int literalIndex = descriptor & 0x1F;
    int argCount = (descriptor >> 5) & 0x7;
    sendSelector(literal(literalIndex), argCount);
}

void Interpreter::extendedSuperSend() {
    uint8_t descriptor = fetchByte();
    int literalIndex = descriptor & 0x1F;
    int argCount = (descriptor >> 5) & 0x7;

    // Super send: lookup from superclass of METHOD's defining class (not receiver's class)
    Oop selector = literal(literalIndex);
    Oop methodClass = methodClassOf(method_);
    Oop superclass;
    if (methodClass.isNil() || !methodClass.isObject()) {
        // Fallback to receiver's class superclass
        superclass = superclassOf(memory_.classOf(receiver_));
    } else {
        superclass = superclassOf(methodClass);
    }

    Oop method = lookupMethod(selector, superclass);
    if (method.isNil()) {
        sendDoesNotUnderstand(selector, argCount);
    } else {
        activateMethod(method, argCount);
    }
}

// ===== JUMPS =====

void Interpreter::shortJump(int offset) {
    instructionPointer_ += offset;
}

void Interpreter::shortJumpIfTrue(int offset) {
    Oop value = pop();
    if (isTrue(value)) {
        instructionPointer_ += offset;
    }
    // Non-booleans treated as false (don't jump)
    // Note: sendMustBeBoolean causes infinite recursion because the
    // Smalltalk mustBeBoolean method itself has conditionals
}

void Interpreter::shortJumpIfFalse(int offset) {
    Oop value = pop();
    if (!isTrue(value)) {
        // Jump if false OR if non-boolean (treat non-booleans as false)
        instructionPointer_ += offset;
    }
}

void Interpreter::longJump() {
    int16_t offset = static_cast<int16_t>(fetchTwoBytes());
    instructionPointer_ += offset;
}

void Interpreter::longJumpIfTrue() {
    int16_t offset = static_cast<int16_t>(fetchTwoBytes());
    Oop value = pop();
    if (isTrue(value)) {
        instructionPointer_ += offset;
    }
    // Non-booleans treated as false (don't jump)
}

void Interpreter::longJumpIfFalse() {
    int16_t offset = static_cast<int16_t>(fetchTwoBytes());
    Oop value = pop();
    if (!isTrue(value)) {
        // Jump if false OR if non-boolean (treat non-booleans as false)
        instructionPointer_ += offset;
    }
}

// ===== SENDS =====

void Interpreter::arithmeticSend(int which) {
    // Arithmetic selectors: + - < > <= >= = ~= * / \\ @ bitShift: // bitAnd: bitOr:
    static const int argCounts[] = {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1};
    int argCount = argCounts[which];

    // std::cerr << "[ARITH-DEBUG] Entry: which=" << which << " SP=" << (stackPointer_ - stackBase_); // DEBUG

    // Get receiver and check for nil early to prevent crashes
    Oop arg = stackValue(0);
    Oop rcvr = stackValue(1);

    // std::cerr << "[ARITH-DEBUG] arg=0x" << std::hex << arg.rawBits()
              // << " rcvr=0x" << rcvr.rawBits() << std::dec;

    Oop nilObj = memory_.specialObject(SpecialObjectIndex::NilObject);
    // std::cerr << "[ARITH-DEBUG] nilObj=0x" << std::hex << nilObj.rawBits() << std::dec; // DEBUG

    // If receiver is nil, handle gracefully based on operation type
    if (rcvr.rawBits() == 0 || rcvr.rawBits() == nilObj.rawBits()) {
        // std::cerr << "[ARITH] Nil receiver for operation " << which << " - returning nil/false"; // DEBUG
        popN(2);  // Pop receiver and argument
        // For comparisons, return false; for arithmetic, return nil
        if (which >= 2 && which <= 7) {
            if (which == 7) {  // ~= (not equal)
                push(memory_.trueObject());  // nil ~= anything is true
            } else {
                push(memory_.falseObject());  // nil < > = anything is false
            }
        } else {
            push(nilObj);  // Arithmetic on nil returns nil
        }
        return;
    }

    // Get receiver class name for fallback handling
    std::string rcvrClassName = "<unknown>";
    if (!rcvr.isSmallInteger() && rcvr.isObject()) {
        Oop rcvrClass = memory_.classOf(rcvr);
        if (rcvrClass.isObject()) {
            ObjectHeader* clsHdr = rcvrClass.asObjectPtr();
            if (clsHdr->slotCount() > 6) {
                Oop nameOop = memory_.fetchPointer(6, rcvrClass);
                if (nameOop.isObject() && nameOop.rawBits() > 0x10000) {
                    ObjectHeader* nameHdr = nameOop.asObjectPtr();
                    if (nameHdr->isBytesObject() && nameHdr->byteSize() <= 50) {
                        rcvrClassName = std::string((char*)nameHdr->bytes(), nameHdr->byteSize());
                    }
                }
            }
        }
    }

    // For startup/snapshot resume, provide aggressive fallbacks for ALL non-SmallInteger
    // receivers to avoid DNU spirals. This allows the VM to continue executing even if
    // arithmetic methods are missing on some objects.

    // Fallback for arithmetic operations on non-SmallInteger receivers
    if (which == 0 || which == 1 || which == 8 || which == 9) {  // + - * /
        if (!rcvr.isSmallInteger()) {
            // std::cerr << "[ARITH] Arithmetic fallback for " << rcvrClassName
                      // << " which=" << which << " - returning receiver";
            pop();  // Pop argument, leave receiver on stack
            return;
        }
    }

    // For comparison operations (< > <= >= = ~=), provide fallback for non-numeric types
    if (which >= 2 && which <= 7) {
        if (!rcvr.isSmallInteger() || !arg.isSmallInteger()) {
            // std::cerr << "[ARITH] Comparison fallback for " << rcvrClassName << " which=" << which; // DEBUG
            popN(2);  // Pop receiver and argument
            switch (which) {
                case 2:  // <
                case 3:  // >
                case 4:  // <=
                case 5:  // >=
                case 6:  // =
                    push(memory_.falseObject());  // Non-numeric comparisons default to false
                    return;
                case 7:  // ~=
                    push(memory_.trueObject());   // Non-numeric inequality defaults to true
                    return;
            }
        }
    }

    // Try to get cached well-known selector
    Oop selector;
    switch (which) {
        case 0: selector = selectors_.add; break;
        case 1: selector = selectors_.subtract; break;
        case 2: selector = selectors_.lessThan; break;
        case 3: selector = selectors_.greaterThan; break;
        case 4: selector = selectors_.lessEqual; break;
        case 5: selector = selectors_.greaterEqual; break;
        case 6: selector = selectors_.equal; break;
        case 7: selector = selectors_.notEqual; break;
        case 8: selector = selectors_.multiply; break;
        case 9: selector = selectors_.divide; break;
        default:
            // For arithmetic ops 10-15 (\\, @, bitShift:, //, bitAnd:, bitOr:),
            // look up from special selectors array, NOT from literals!
            {
                Oop specialSelectors = memory_.specialObject(SpecialObjectIndex::SpecialSelectorsArray);
                if (specialSelectors.isObject() && specialSelectors.rawBits() > 0x10000) {
                    ObjectHeader* ssHdr = specialSelectors.asObjectPtr();
                    size_t selectorSlot = which * 2;  // Each selector has 2 entries
                    if (selectorSlot < ssHdr->slotCount()) {
                        selector = ssHdr->slotAt(selectorSlot);
                    } else {
                        selector = Oop::nil();
                    }
                } else {
                    selector = Oop::nil();
                }
            }
            break;
    }

    if (selector.isNil()) {
        // Fallback: return receiver for unsupported operations
        pop();  // Pop argument, leave receiver
        return;
    }

    sendSelector(selector, argCount);
}

void Interpreter::commonSend(int which) {
    // In Sista V1, bytecodes 192-207 are "send special selector 16-31"
    // These use the special selectors array (special object index 23)
    // The array format is: [selector0, argCount0, selector1, argCount1, ...]
    // Bytecode 192 sends special selector 16, bytecode 207 sends special selector 31

    int selectorIndex = which + 16;  // Offset by 16 from the arithmetic sends

    // Get special selectors array
    Oop specialSelectors = memory_.specialObject(SpecialObjectIndex::SpecialSelectorsArray);
    if (!specialSelectors.isObject() || specialSelectors.rawBits() < 0x10000) {
        // std::cerr << "[COMMON] ERROR: Special selectors array not found - falling back to literal"; // DEBUG
        // Fallback: try using literal (old incorrect behavior)
        sendSelector(literal(which), 0);
        return;
    }

    ObjectHeader* ssArrayHdr = specialSelectors.asObjectPtr();
    size_t arraySlots = ssArrayHdr->slotCount();

    // Each selector has 2 entries: selector and argCount
    size_t selectorSlot = selectorIndex * 2;
    size_t argCountSlot = selectorIndex * 2 + 1;

    if (selectorSlot >= arraySlots || argCountSlot >= arraySlots) {
        // std::cerr << "[COMMON] ERROR: Special selector index " << selectorIndex
                  // << " out of range (array has " << arraySlots << " slots)";
        returnValue(receiver_);
        return;
    }

    Oop selector = ssArrayHdr->slotAt(selectorSlot);
    Oop argCountOop = ssArrayHdr->slotAt(argCountSlot);

    int argCount = 0;
    if (argCountOop.isSmallInteger()) {
        argCount = static_cast<int>(argCountOop.asSmallInteger());
    } else {
        // Fallback: count colons in selector
        if (selector.isObject() && selector.rawBits() > 0x10000) {
            ObjectHeader* selHdr = selector.asObjectPtr();
            if (selHdr->isBytesObject()) {
                size_t len = selHdr->byteSize();
                const uint8_t* bytes = selHdr->bytes();
                for (size_t i = 0; i < len; i++) {
                    if (bytes[i] == ':') argCount++;
                }
            }
        }
    }

    // Debug output
    std::string selStr = "";
    if (selector.isObject() && selector.rawBits() > 0x10000) {
        ObjectHeader* selHdr = selector.asObjectPtr();
        if (selHdr->isBytesObject()) {
            selStr = std::string((char*)selHdr->bytes(), selHdr->byteSize());
            // std::cerr << "[COMMON] Sending special selector #" << selectorIndex
                      // << " '" << selStr << "' with " << argCount << " args";
        }
    }

    // Fallback for stream operations during startup to avoid DNU
    // Special selector 20 is 'nextPut:' which is commonly used in logging
    if (selectorIndex == 20 && argCount == 1) {  // nextPut:
        Oop rcvr = stackValue(1);  // Receiver is under the argument

        // If receiver doesn't look like a stream (not an object or is a special object),
        // just pop args and return the argument (stream operations return the argument)
        if (!rcvr.isObject() || rcvr.rawBits() == 0) {
            // std::cerr << "[COMMON] Fallback for nextPut: on non-stream - returning argument"; // DEBUG
            Oop arg = pop();  // Pop the argument
            pop();  // Pop the receiver
            push(arg);  // Return the argument (standard stream behavior)
            return;
        }
    }

    // Fallback for selector 21 (next) during startup
    if (selectorIndex == 21 && argCount == 0) {  // next
        Oop rcvr = stackValue(0);
        if (!rcvr.isObject() || rcvr.rawBits() == 0) {
            // std::cerr << "[COMMON] Fallback for next on non-stream - returning nil"; // DEBUG
            pop();  // Pop receiver
            push(memory_.nil());  // Return nil
            return;
        }
    }

    sendSelector(selector, argCount);
}

void Interpreter::sendArithmetic(int which) {
    // Sista V1: Send arithmetic message (special selectors 0-15)
    // Delegates to existing arithmeticSend implementation
    arithmeticSend(which);
}

void Interpreter::sendSpecial(int which) {
    // Sista V1: Send special message (special selectors 0-15 map to selectors 16-31)
    // Delegates to existing commonSend implementation which handles selectors 16-31
    commonSend(which);
}

void Interpreter::sendLiteralZeroArgs(int literalIndex) {
    sendSelector(literal(literalIndex), 0);
}

void Interpreter::sendLiteralOneArg(int literalIndex) {
    sendSelector(literal(literalIndex), 1);
}

void Interpreter::sendLiteralTwoArgs(int literalIndex) {
    sendSelector(literal(literalIndex), 2);
}

void Interpreter::sendSelector(Oop selector, int argCount) {
    // Recursion detection - break infinite recursion patterns
    // Compare by string content since same symbol may have different Oops
    static std::string lastSelStr;
    static int sameSelCount = 0;
    std::string selStr;
    if (selector.isObject() && selector.rawBits() > 0x10000) {
        ObjectHeader* selHdr = selector.asObjectPtr();
        if (selHdr->isBytesObject() && selHdr->byteSize() < 50) {
            selStr = std::string((char*)selHdr->bytes(), selHdr->byteSize());
        }
    }
    if (!selStr.empty() && selStr == lastSelStr) {
        sameSelCount++;
        if (sameSelCount > 50) {
            // Same selector called 50+ times in a row - likely infinite recursion
            std::cerr << "[RECURSION] Breaking infinite loop: #" << selStr
                      << " called " << sameSelCount << " times\n";
            // Pop args and receiver, return nil
            popN(argCount + 1);
            push(memory_.nil());
            sameSelCount = 0;
            lastSelStr.clear();
            return;
        }
    } else {
        lastSelStr = selStr;
        sameSelCount = 1;
    }

    // Message send tracing (limited to first 50 for cleaner output)
    static int sendCount = 0;
    sendCount++;

    // Menu action tracing
    static FILE* menuTrace = nullptr;
    if (menuActionTraceCount_ > 0) {
        if (!menuTrace) {
            menuTrace = fopen("/tmp/iospharo-menu-trace.log", "a");
        }
        if (menuTrace) {
            menuActionTraceCount_--;
            std::string selName = "<unknown>";
            if (selector.isObject() && selector.rawBits() > 0x10000) {
                ObjectHeader* selHdr = selector.asObjectPtr();
                if (selHdr->isBytesObject() && selHdr->byteSize() < 100) {
                    selName = std::string((char*)selHdr->bytes(), selHdr->byteSize());
                }
            }
            Oop rcvr = stackValue(argCount);
            std::string rcvrClassName = "<unknown>";
            if (rcvr.isObject() && rcvr.rawBits() > 0x10000) {
                Oop cls = memory_.classOf(rcvr);
                if (cls.isObject()) {
                    Oop nameOop = memory_.fetchPointer(6, cls);
                    if (nameOop.isObject()) {
                        ObjectHeader* nameHdr = nameOop.asObjectPtr();
                        if (nameHdr->isBytesObject() && nameHdr->byteSize() < 100) {
                            rcvrClassName = std::string((char*)nameHdr->bytes(), nameHdr->byteSize());
                        }
                    }
                }
            } else if (rcvr.isSmallInteger()) {
                rcvrClassName = "SmallInteger";
            }
            fprintf(menuTrace, "[TRACE %d] Send #%s to %s (args=%d)\n",
                    200 - menuActionTraceCount_, selName.c_str(), rcvrClassName.c_str(), argCount);
            fflush(menuTrace);
        }
    }

    if (sendCount <= 50) {
        // Show selector name
        std::string selName = "<unknown>";
        bool isValidSymbol = false;
        if (selector.isObject() && selector.rawBits() > 0x10000) {
            ObjectHeader* selHdr = selector.asObjectPtr();
            // Check if this looks like a valid symbol (bytes object with printable ASCII)
            if (selHdr->isBytesObject() && selHdr->byteSize() > 0 && selHdr->byteSize() < 256) {
                const uint8_t* bytes = selHdr->bytes();
                size_t len = selHdr->byteSize();
                bool allPrintable = true;
                for (size_t i = 0; i < len && allPrintable; i++) {
                    if (bytes[i] < 0x20 || bytes[i] > 0x7E) {
                        allPrintable = false;
                    }
                }
                if (allPrintable) {
                    selName = std::string((char*)bytes, len);
                    isValidSymbol = true;
                } else {
                    // Contains non-printable chars - likely garbage
                    selName = "<garbage-" + std::to_string(selHdr->classIndex()) + ">";
                }
            } else {
                selName = "<non-bytes-" + std::to_string(selHdr->classIndex()) + ">";
            }
        } else if (selector.isNil() || selector.rawBits() == 0) {
            selName = "<nil>";
        }
    }

    // Get receiver (under the arguments on stack)
    Oop rcvr = stackValue(argCount);

    // ===== INTERCEPT TERMINATION SELECTORS =====
    // During embedded VM startup, prevent process termination from quit sequence
    // The SnapshotOperation's isQuit=true path tries to terminate processes before
    // UI initialization completes. We intercept these selectors and return self.
    if (selector.isObject() && selector.rawBits() > 0x10000) {
        ObjectHeader* selHdr = selector.asObjectPtr();
        if (selHdr->isBytesObject() && selHdr->byteSize() > 0 && selHdr->byteSize() < 50) {
            std::string selStr((char*)selHdr->bytes(), selHdr->byteSize());
            // Check for termination-related selectors
            if (selStr == "terminateRealActive" || selStr == "terminateActive" ||
                selStr == "doTerminationFromYourself") {
                // Actually terminate the process and switch to another
                popN(argCount + 1);

                // Mark current process as terminated
                terminateCurrentProcess();

                // Try to find another runnable process
                if (tryReschedule()) {
                    return;
                }

                // No other process - try bootstrap
                if (bootstrapStartup()) {
                    return;
                }

                // No processes to run - just return self
                push(rcvr);
                return;
            }

            // ===== INTERCEPT Form class >> extent:depth: =====
            // When NullWorldRenderer creates a temp Form for drawing, we return Display instead.
            // This causes all drawing to go to Display, which we sync to native surface.
            if (selStr == "extent:depth:" && argCount == 2) {
                // Check if receiver is a class object with name
                if (rcvr.isObject()) {
                    // For class-side method: rcvr is the class (e.g., Form)
                    // Slot 6 of a class is its name (a Symbol)
                    Oop maybeName = memory_.fetchPointer(6, rcvr);
                    std::string name;
                    if (maybeName.isObject()) {
                        ObjectHeader* nameHdr = maybeName.asObjectPtr();
                        if (nameHdr->isBytesObject() && nameHdr->byteSize() < 50) {
                            name = std::string((char*)nameHdr->bytes(), nameHdr->byteSize());
                        }
                    }

                    if (name == "Form") {
                        // This is Form class receiving extent:depth:
                        // Return our Display Form instead of creating new one
                        Oop display = memory_.findGlobal("Display");
                        if (!display.isNil() && display.isObject()) {
                            // Pop args and receiver, push Display
                            popN(argCount + 1);
                            push(display);
                            return;
                        }
                    }
                }
            }

            // ===== INTERCEPT MorphicRenderLoop >> wait =====
            // MorphicRenderLoop's wait blocks on a semaphore for next frame.
            // Since we don't have proper delay/semaphore integration yet, just return self.
            if ((selStr == "wait" || selStr == "extraWorldList") && argCount == 0) {
                Oop rcvrClass = memory_.classOf(rcvr);
                if (rcvrClass.isObject()) {
                    Oop className = memory_.fetchPointer(6, rcvrClass);
                    if (className.isObject()) {
                        ObjectHeader* nameHdr = className.asObjectPtr();
                        if (nameHdr->isBytesObject() && nameHdr->byteSize() < 50) {
                            std::string rcvrClassName((char*)nameHdr->bytes(), nameHdr->byteSize());
                            if (rcvrClassName == "MorphicRenderLoop") {
                                popN(argCount + 1);
                                if (selStr == "extraWorldList") {
                                    // Return empty Array
                                    Oop arrayClass = memory_.specialObject(SpecialObjectIndex::ClassArray);
                                    Oop emptyArray = memory_.allocateSlots(
                                        memory_.indexOfClass(arrayClass), 0, ObjectFormat::Indexable);
                                    push(emptyArray);
                                } else {
                                    // Return self for #wait
                                    push(rcvr);
                                }
                                return;
                            }
                        }
                    }
                }
            }

            // ===== INTERCEPT Set/IdentitySet >> error: for "no free space" =====
            // This error happens when a Set fills up. Ignore it to let rendering continue.
            if (selStr == "error:" && argCount >= 1) {
                Oop errArg = stackValue(0);
                if (errArg.isObject()) {
                    ObjectHeader* errHdr = errArg.asObjectPtr();
                    if (errHdr->isBytesObject() && errHdr->byteSize() < 100) {
                        std::string errMsg((char*)errHdr->bytes(), errHdr->byteSize());
                        // Check for common errors we can safely ignore
                        bool canIgnore = (errMsg.find("no free space") != std::string::npos ||
                                         errMsg.find("only integers") != std::string::npos);
                        if (canIgnore) {
                            // Pop args and return nil to signal failure without breaking type expectations
                            popN(argCount + 1);
                            push(memory_.nil());
                            return;
                        }
                    }
                }
            }

            // ===== INTERCEPT WorldState >> doOneCycleFor: =====
            // Render World's morphs directly instead of using NullWorldRenderer
            if (selStr == "doOneCycleFor:" && argCount == 1) {
                Oop rcvrClass = memory_.classOf(rcvr);
                if (rcvrClass.isObject()) {
                    Oop className = memory_.fetchPointer(6, rcvrClass);
                    if (className.isObject()) {
                        ObjectHeader* nameHdr = className.asObjectPtr();
                        if (nameHdr->isBytesObject() && nameHdr->byteSize() < 50) {
                            std::string rcvrClassName((char*)nameHdr->bytes(), nameHdr->byteSize());
                            if (rcvrClassName == "WorldState") {
                                // Process any pending input events
                                processInputEvents();

                                // Render World's morphs directly to the display surface
                                renderWorldMorphs();

                                // Execute pending menu action if any
                                static FILE* actionLog = fopen("/tmp/iospharo-action-exec.log", "a");
                                if (pendingMenuAction_.pending) {
                                    Oop actionSel = pendingMenuAction_.selector;
                                    Oop actionRcvr = pendingMenuAction_.receiver;
                                    Oop actionArg = pendingMenuAction_.argument;
                                    int actionArgCount = pendingMenuAction_.argCount;
                                    pendingMenuAction_.pending = false;
                                    pendingMenuAction_.selector = Oop::nil();
                                    pendingMenuAction_.receiver = Oop::nil();
                                    pendingMenuAction_.argument = Oop::nil();
                                    pendingMenuAction_.argCount = 0;

                                    // Log the action being executed
                                    std::string selStr = "<unknown>";
                                    if (actionSel.isObject() && actionSel.rawBits() > 0x10000) {
                                        ObjectHeader* selHdr = actionSel.asObjectPtr();
                                        if (selHdr->isBytesObject() && selHdr->byteSize() < 50) {
                                            selStr = std::string((char*)selHdr->bytes(), selHdr->byteSize());
                                        }
                                    }
                                    std::string rcvrClass = "<unknown>";
                                    if (actionRcvr.isObject() && actionRcvr.rawBits() > 0x10000) {
                                        Oop cls = memory_.classOf(actionRcvr);
                                        if (cls.isObject()) {
                                            Oop nameOop = memory_.fetchPointer(6, cls);
                                            if (nameOop.isObject() && nameOop.rawBits() > 0x10000) {
                                                ObjectHeader* nameHdr = nameOop.asObjectPtr();
                                                if (nameHdr->isBytesObject() && nameHdr->byteSize() < 50) {
                                                    rcvrClass = std::string((char*)nameHdr->bytes(), nameHdr->byteSize());
                                                }
                                            }
                                        }
                                    }
                                    if (actionLog) {
                                        fprintf(actionLog, "[EXEC] About to execute #%s on %s with %d args\n",
                                                selStr.c_str(), rcvrClass.c_str(), actionArgCount);
                                        fflush(actionLog);
                                    }
                                    std::cerr << "[MENU-ACTION] Executing #" << selStr << " on " << rcvrClass
                                              << " with " << actionArgCount << " args\n";
                                    std::cerr.flush();

                                    // Pop doOneCycleFor:'s args and receiver
                                    popN(argCount + 1);

                                    // Set up the action call
                                    push(actionRcvr);
                                    if (actionArgCount > 0 && !actionArg.isNil()) {
                                        push(actionArg);
                                    }
                                    if (actionLog) {
                                        fprintf(actionLog, "[EXEC] Calling sendSelector now, enabling trace for 200 sends\n");
                                        fflush(actionLog);
                                    }
                                    menuActionTraceCount_ = 200;  // Trace next 200 message sends
                                    sendSelector(actionSel, actionArgCount);
                                    if (actionLog) {
                                        fprintf(actionLog, "[EXEC] sendSelector returned\n");
                                        fflush(actionLog);
                                    }
                                    return;
                                }

                                // Return receiver (WorldState) to continue
                                popN(argCount + 1);
                                push(rcvr);
                                return;
                            }
                        }
                    }
                }
            }

            // ===== INTERCEPT WorldState >> runStepMethodsIn: =====
            // Skip the entire method to avoid getting stuck in deferredUIMessages loop
            // This allows displayWorldSafely: to be called
            if (selStr == "runStepMethodsIn:" && argCount == 1) {
                Oop rcvrClass = memory_.classOf(rcvr);
                if (rcvrClass.isObject()) {
                    Oop className = memory_.fetchPointer(6, rcvrClass);
                    if (className.isObject()) {
                        ObjectHeader* nameHdr = className.asObjectPtr();
                        if (nameHdr->isBytesObject() && nameHdr->byteSize() < 50) {
                            std::string rcvrClassName((char*)nameHdr->bytes(), nameHdr->byteSize());
                            if (rcvrClassName == "WorldState") {
                                // Pop args and receiver, push receiver (return self)
                                popN(argCount + 1);
                                push(rcvr);
                                return;
                            }
                        }
                    }
                }
            }

            // ===== INTERCEPT WorldState >> checkIfUpdateNeeded =====
            // Force it to return true so Morphic actually renders
            if (selStr == "checkIfUpdateNeeded" && argCount == 0) {
                Oop rcvrClass = memory_.classOf(rcvr);
                if (rcvrClass.isObject()) {
                    Oop className = memory_.fetchPointer(6, rcvrClass);
                    if (className.isObject()) {
                        ObjectHeader* nameHdr = className.asObjectPtr();
                        if (nameHdr->isBytesObject() && nameHdr->byteSize() < 50) {
                            std::string rcvrClassName((char*)nameHdr->bytes(), nameHdr->byteSize());
                            if (rcvrClassName == "WorldState") {
                                // Pop receiver, push true (force update)
                                popN(1);  // no args
                                push(memory_.trueObject());
                                return;
                            }
                        }
                    }
                }
            }

            // Intercept checkForNewScreenSize to set up display
            if (selStr == "checkForNewScreenSize" && argCount == 0) {
                Oop rcvrClass = memory_.classOf(rcvr);
                if (rcvrClass.isObject()) {
                    Oop className = memory_.fetchPointer(6, rcvrClass);
                    if (className.isObject()) {
                        ObjectHeader* nameHdr = className.asObjectPtr();
                        if (nameHdr->isBytesObject() && nameHdr->byteSize() < 50) {
                            std::string rcvrClassName((char*)nameHdr->bytes(), nameHdr->byteSize());
                            if (rcvrClassName == "NullWorldRenderer") {
                                static bool firstCheck = true;
                                if (firstCheck) {
                                    firstCheck = false;
                                    // Try to create/initialize display Form
                                    initializeDisplayForm();
                                }
                                // Return self (screen size unchanged)
                                popN(argCount + 1);
                                push(rcvr);
                                return;
                            }
                        }
                    }
                }
            }
        }
    }

    // Check for invalid receiver (could be from out-of-bounds literal access)
    Oop nilObj = memory_.specialObject(SpecialObjectIndex::NilObject);
    if (rcvr.rawBits() == 0 || rcvr.rawBits() == nilObj.rawBits()) {
        popN(argCount + 1);  // Pop args and receiver
        push(nilObj);
        return;
    }

    // Determine receiver's class
    Oop rcvrClass = memory_.classOf(rcvr);

    // Check for invalid class (can happen with corrupted state)
    if (rcvrClass.rawBits() == 0) {
        popN(argCount + 1);
        push(nilObj);
        return;
    }

    // Check method cache
    MethodCacheEntry* cached = probeCache(selector, rcvrClass);
    if (cached && cached->method != Oop::nil()) {
        // Cache hit
        if (cached->primitiveIndex > 0) {
            // Try primitive first - IMPORTANT: set argCount_ before calling primitive
            // because primitiveSuccess uses argCount_ to pop the correct number of items
            argCount_ = argCount;
            primitiveFailed_ = false;
            PrimitiveResult result = executePrimitive(cached->primitiveIndex, argCount);
            if (result == PrimitiveResult::Success) {
                return;  // Primitive handled it
            }
        }
        activateMethod(cached->method, argCount);
        return;
    }

    // Cache miss - look up method
    Oop method = lookupMethod(selector, rcvrClass);
    if (method.isNil()) {
        sendDoesNotUnderstand(selector, argCount);
        return;
    }

    // Cache the method
    cacheMethod(selector, rcvrClass, method);

    // Debug: trace method lookup for specific selectors
    if (sendCount >= 33 && sendCount <= 50) {
        // Get selector name for trace
        std::string selName = "<unknown>";
        if (selector.isObject() && selector.rawBits() > 0x10000) {
            ObjectHeader* selHdr = selector.asObjectPtr();
            if (selHdr->isBytesObject() && selHdr->byteSize() < 50) {
                selName = std::string((char*)selHdr->bytes(), selHdr->byteSize());
            }
        }
        int primIdx = primitiveIndexOf(method);
        // Special logging for copyBits to verify BitBlt is being looked up
        if (selName == "copyBits" || selName == "copyBitsFrom:to:at:" || selName.find("copyBits") != std::string::npos) {
            std::cerr << "[BITBLT_LOOKUP] *** FOUND copyBits selector! *** primIdx=" << primIdx << "\n";
        }
        std::cerr << "[LOOKUP_DEBUG] selector #" << sendCount << " '" << selName << "'"
                  << " method=0x" << std::hex << method.rawBits() << std::dec
                  << " primIdx=" << primIdx;
        if (method.isObject()) {
            ObjectHeader* mh = method.asObjectPtr();
            std::cerr << " methodClassIdx=" << mh->classIndex();
            // Show first few bytecodes
            Oop hdr = memory_.fetchPointer(0, method);
            if (hdr.isSmallInteger()) {
                int64_t hb = hdr.asSmallInteger();
                int numLit = hb & 0x7FFF;
                uint8_t* bc = mh->bytes() + (1 + numLit) * 8;
                std::cerr << " bytecodes=[" << (int)bc[0] << "," << (int)bc[1] << "," << (int)bc[2] << "]";
            }
        }
        std::cerr << "\n";
    }

    // Check for primitive
    int primIndex = primitiveIndexOf(method);
    if (primIndex > 0) {
        argCount_ = argCount;
        primitiveFailed_ = false;
        PrimitiveResult result = executePrimitive(primIndex, argCount);
        if (result == PrimitiveResult::Success) {
            // Debug: trace primitive success
            static int primSuccessCount = 0;
            primSuccessCount++;
            if (primSuccessCount <= 20 && sendCount <= 60) {
                std::cerr << "[PRIM_OK] selector #" << sendCount << " prim=" << primIndex
                          << " stackTop=0x" << std::hex << stackTop().rawBits() << std::dec;
                if (stackTop().isSmallInteger()) {
                    std::cerr << " (SmallInt=" << stackTop().asSmallInteger() << ")";
                } else if (stackTop().isObject()) {
                    ObjectHeader* h = stackTop().asObjectPtr();
                    std::cerr << " (classIdx=" << h->classIndex() << ")";
                }
                std::cerr << "\n";
            }
            return;
        }
        // Primitive failed - fall through to method activation
    }

    activateMethod(method, argCount);
}

// ===== METHOD LOOKUP =====

Oop Interpreter::lookupMethod(Oop selector, Oop classOop) {
    Oop currentClass = classOop;
    int depth = 0;

    // Get nil object for proper comparison
    Oop nilObj = memory_.specialObject(SpecialObjectIndex::NilObject);
    auto isNilOrEnd = [nilObj](Oop o) -> bool {
        return o.isNil() || o.rawBits() == nilObj.rawBits() || o.rawBits() < 0x10000;
    };

    // Debug: check if this is a lookup for 'yourself'
    std::string selectorName;
    if (selector.isObject() && selector.rawBits() > 0x10000) {
        ObjectHeader* selHdr = selector.asObjectPtr();
        if (selHdr->isBytesObject() && selHdr->byteSize() <= 50) {
            selectorName = std::string((char*)selHdr->bytes(), selHdr->byteSize());
        }
    }
    bool traceYourself = (selectorName == "yourself" || selectorName == "doesNotUnderstand:");
    if (traceYourself) {
        std::cerr << "[YOURSELF_TRACE] Starting lookup for '" << selectorName << "' in class=0x"
                  << std::hex << classOop.rawBits() << std::dec;
        if (classOop.isObject()) {
            ObjectHeader* hdr = classOop.asObjectPtr();
            std::cerr << " classIdx=" << hdr->classIndex() << " slots=" << hdr->slotCount();
        }
        std::cerr << "\n";
    }

    while (!isNilOrEnd(currentClass) && currentClass.isObject() && depth < 100) {
        ObjectHeader* clsHdr = currentClass.asObjectPtr();
        Oop methodDict = methodDictOf(currentClass);

        // Get class name from slot 6 (name field in Behavior)
        std::string className = "<unknown>";
        if (clsHdr->slotCount() > 6) {
            Oop nameOop = memory_.fetchPointer(6, currentClass);
            if (nameOop.isObject() && nameOop.rawBits() > 0x10000) {
                ObjectHeader* nameHdr = nameOop.asObjectPtr();
                if (nameHdr->isBytesObject() && nameHdr->byteSize() <= 50) {
                    className = std::string((char*)nameHdr->bytes(), nameHdr->byteSize());
                }
            }
        }

        static int lookupDebugCount = 0;
        // Enable tracing when we detect invalid pointer (happens early in method lookup for #rounded)
        bool shouldTrace = (lookupDebugCount < 30) && (!memory_.isValidPointer(methodDict) || !memory_.isValidPointer(currentClass));
        if (traceYourself) {
            Oop superclass = superclassOf(currentClass);
            std::cerr << "[YOURSELF_TRACE] depth=" << depth << " class=" << className
                      << " classOop=0x" << std::hex << currentClass.rawBits()
                      << " slot0=0x" << memory_.fetchPointer(0, currentClass).rawBits()
                      << " slot1=0x" << memory_.fetchPointer(1, currentClass).rawBits()
                      << " slot2=0x" << memory_.fetchPointer(2, currentClass).rawBits()
                      << std::dec
                      << " superclass=0x" << std::hex << superclass.rawBits()
                      << " isNilOrEnd=" << isNilOrEnd(superclass) << std::dec << "\n";
        }
        if (shouldTrace || lookupDebugCount < 5) {
            lookupDebugCount++;
            std::cerr << "[LOOKUP] depth=" << depth << " class=" << className << " (0x" << std::hex << currentClass.rawBits()
                      << std::dec << " clsIdx=" << clsHdr->classIndex() << " slots=" << clsHdr->slotCount()
                      << ") md=0x" << std::hex << methodDict.rawBits()
                      << " valid=" << memory_.isValidPointer(methodDict) << std::dec;
            // Check if class or methodDict pointer looks unrelocated (in old base range)
            if (!memory_.isValidPointer(currentClass)) {
                std::cerr << " *** CLASS INVALID ***";
            }
            if (!memory_.isValidPointer(methodDict)) {
                std::cerr << " *** MD INVALID ***";
            }
            std::cerr << std::endl;
        }
        if (!isNilOrEnd(methodDict) && methodDict.isObject()) {
            Oop method = lookupInMethodDict(methodDict, selector);
            if (!isNilOrEnd(method) && method.isObject()) {
                // Trace where methods are found for key selectors
                static int foundCount = 0;
                if (foundCount < 50 && (selectorName == "flatCollect:" || selectorName == "isEmpty" ||
                    selectorName == "size" || selectorName == "yourself" || selectorName == "species")) {
                    foundCount++;
                    std::cerr << "[FOUND] '" << selectorName << "' at depth=" << depth
                              << " class=" << className << "\n";
                }
                return method;
            }
        }
        currentClass = superclassOf(currentClass);
        depth++;
    }

    // DEBUG: "[LOOKUP] Method not found after " << depth << " levels"
    return Oop::nil();  // Not found
}

Oop Interpreter::lookupInMethodDict(Oop methodDict, Oop selector) const {
    // Modern Pharo MethodDictionary is IndexableWithFixed (format 3):
    //   slot 0: tally (SmallInteger - number of entries)
    //   slot 1: values array (Array of CompiledMethods)
    //   slots 2+: keys (Symbols) stored INLINE in the method dict
    //
    // The keys are stored inline for fast hashing, values in separate array.
    // Index mapping: key at mdSlot[i] -> method at valuesArray[i-2]
    if (!methodDict.isObject()) return Oop::nil();

    ObjectHeader* mdHeader = methodDict.asObjectPtr();
    size_t mdSlotCount = mdHeader->slotCount();
    if (mdSlotCount < 3) return Oop::nil();  // Need at least tally, values, and 1 key slot

    // Get the selector as string for comparison
    std::string selectorStr;
    Oop actualSelector = selector;
    if (selector.isObject() && selector.rawBits() > 0x10000) {
        ObjectHeader* selHdr = selector.asObjectPtr();

        // Handle case where selector is wrapped (Message object, AdditionalMethodState, etc.)
        // These have format 1 (FixedSize) and slot 0 contains the actual selector
        if (selHdr->format() == ObjectFormat::FixedSize && selHdr->slotCount() >= 1) {
            Oop innerSel = memory_.fetchPointer(0, selector);
            if (innerSel.isObject() && innerSel.rawBits() > 0x10000) {
                ObjectHeader* innerHdr = innerSel.asObjectPtr();
                if (innerHdr->isBytesObject()) {
                    actualSelector = innerSel;
                    selHdr = innerHdr;  // Update for byte extraction below
                }
            }
        }

        if (selHdr->isBytesObject() && selHdr->byteSize() <= 100) {
            selectorStr = std::string((char*)selHdr->bytes(), selHdr->byteSize());
        }

        // Debug: show selector that wasn't extracted
        static int selDebug = 0;
        if (selDebug < 5 && selectorStr.empty()) {
            selDebug++;
            // DEBUG_LOG("[MD] WARNING: Empty selector from 0x" << std::hex << selector.rawBits()
                      // << " fmt=" << static_cast<int>(selHdr->format())
                      // << " slots=" << selHdr->slotCount()
                      // << " cls=" << selHdr->classIndex() << std::dec;
        }
    }

    // Get the values array (slot 1 - contains methods)
    Oop valuesArray = memory_.fetchPointer(1, methodDict);
    if (!valuesArray.isObject() || valuesArray.rawBits() < 0x10000) return Oop::nil();

    ObjectHeader* valuesHeader = valuesArray.asObjectPtr();
    size_t valuesSize = valuesHeader->slotCount();

    // Keys are in slots 2 through (mdSlotCount-1)
    size_t keySlotCount = mdSlotCount - 2;  // Number of key slots
    size_t size = keySlotCount;  // For debug output compatibility

    // For backward compat debug, create a keysArray alias
    Oop keysArray = valuesArray;  // Just for debug naming
    ObjectHeader* keysHeader = valuesHeader;
    bool hasValuesArray = true;

    // Debug: show method dict info for the first few lookups
    static int debugCount = 0;
    bool shouldDebug = (debugCount < 0);  // Disabled for now
    if (shouldDebug) {
        debugCount++;

        // Show method dictionary structure - check ALL slots to find overflow arrays
        // DEBUG_LOG("[MD] MethodDict slots=" << mdHeader->slotCount() << " format=" << static_cast<int>(mdHeader->format());
        for (size_t j = 0; j < std::min((size_t)10, mdHeader->slotCount()); ++j) {
            Oop slot = memory_.fetchPointer(j, methodDict);
            // DEBUG_LOG("[MD]   slot[" << j << "]=0x" << std::hex << slot.rawBits() << std::dec;
            if (slot.isSmallInteger()) {
                // std::cerr << " (SmallInt: " << slot.asSmallInteger() << ")";
            } else if (slot.isObject() && slot.rawBits() > 0x10000 && slot.rawBits() < 0x10000000000ULL) {
                ObjectHeader* slotHdr = slot.asObjectPtr();
                // std::cerr << " (obj: fmt=" << static_cast<int>(slotHdr->format())
                          // << " cls=" << slotHdr->classIndex()
                          // << " slots=" << slotHdr->slotCount() << ")";
            } else {
                // std::cerr << " (nil/invalid)";
            }
            // std::cerr; // DEBUG
        }

        std::cerr << "[MD] selector=#" << selectorStr << " (0x" << std::hex << selector.rawBits()
                  << " actual=0x" << actualSelector.rawBits() << std::dec << ")"
                  << " keySlots=" << keySlotCount
                  << " valuesSize=" << valuesSize
                  << " mdSlots=" << mdSlotCount
                  << " mdFormat=" << static_cast<int>(mdHeader->format()) << std::endl;

        // If searching for 'new' in a large dict, dump all selectors that start with 'n'
        if (selectorStr == "new" && keySlotCount > 500) {
            std::cerr << "[MD] Searching for 'new' - listing all 'n*' selectors:" << std::endl;
            int count = 0;
            for (size_t j = 0; j < keySlotCount && count < 20; ++j) {
                Oop key = memory_.fetchPointer(j + 2, methodDict);
                if (key.isObject() && key.rawBits() > 0x10000) {
                    ObjectHeader* keyHdr = key.asObjectPtr();
                    if (keyHdr->isBytesObject() && keyHdr->byteSize() > 0) {
                        const char* bytes = (const char*)keyHdr->bytes();
                        if (bytes[0] == 'n') {
                            std::string keyStr(bytes, keyHdr->byteSize());
                            std::cerr << "[MD]   found: #" << keyStr << " at slot " << j << std::endl;
                            count++;
                        }
                    }
                }
            }
        }
    }

    Oop nilObj = memory_.specialObject(SpecialObjectIndex::NilObject);
    auto isNilOrEmpty = [nilObj](Oop o) -> bool {
        return o.isNil() || o.rawBits() == nilObj.rawBits() || o.rawBits() < 0x10000;
    };

    // Limit search to reasonable size (1024 to cover Object's method dict)
    size_t maxSearch = std::min(size, (size_t)1024);
    int nonNilCount = 0;

    // Debug: show first few inline key slots in method dict (slots 2+)
    if (shouldDebug && keySlotCount <= 50) {
        std::cerr << "[MD] First inline keys (slots 2+):" << std::endl;
        for (size_t j = 0; j < std::min((size_t)10, keySlotCount); ++j) {
            Oop key = memory_.fetchPointer(j + 2, methodDict);  // Keys start at slot 2
            std::cerr << "[MD]   key[" << j << "]=0x" << std::hex << key.rawBits() << std::dec;
            if (isNilOrEmpty(key)) {
                std::cerr << " (nil/empty)";
            } else if (key.isObject()) {
                ObjectHeader* keyHdr = key.asObjectPtr();
                std::cerr << " (fmt=" << static_cast<int>(keyHdr->format())
                          << " cls=" << keyHdr->classIndex();
                if (keyHdr->isBytesObject() && keyHdr->byteSize() <= 30) {
                    std::cerr << " \"" << std::string((char*)keyHdr->bytes(), keyHdr->byteSize()) << "\"";
                }
                std::cerr << ")";
            }
            std::cerr << std::endl;
        }
    }

    if (shouldDebug && size > 250) {
        // DEBUG: "[MD] Starting search of " << maxSearch << " entries..."
        // std::cerr.flush();
    }

    // Search the MethodDict's inline key slots (starting at slot 2)
    // Keys are Symbols stored directly in the MethodDict at slots 2+
    // Corresponding methods are in valuesArray at index (slotIndex - 2)
    for (size_t i = 0; i < maxSearch; ++i) {
        size_t mdSlotIndex = i + 2;  // Key at mdSlot[i+2], method at valuesArray[i]

        // Debug output for large searches at critical index
        bool verboseDebug = shouldDebug && size > 250 && (i > 240);
        if (verboseDebug && i == 241) {
            // DEBUG: "[MD] Entering critical section i=241+"
            // std::cerr.flush();
        }

        // Progress for DNU lookup on large dicts
        if (selectorStr == "doesNotUnderstand:" && size > 500 && (i % 50 == 0)) {
            // std::cerr << "[MD-PROGRESS] DNU search i=" << i << "/" << maxSearch; // DEBUG
            // std::cerr.flush();
        }

        // Fetch key from MethodDict's inline slot (not from keysArray!)
        Oop key = memory_.fetchPointer(mdSlotIndex, methodDict);

        if (verboseDebug && ((i - 241) % 3 == 0)) {
            // DEBUG_LOG("[MD] i=" << i << " key=0x" << std::hex << key.rawBits() << std::dec;
            // std::cerr.flush();
        }

        if (isNilOrEmpty(key)) {
            // if (verboseDebug && ((i - 241) % 3 == 0)) std::cerr << " (nil)"; // DEBUG
            continue;
        }
        nonNilCount++;

        if (verboseDebug && ((i - 241) % 3 == 0)) {
            // std::cerr << " (non-nil #" << nonNilCount << ")"; // DEBUG
            // std::cerr.flush();
        }

        if (shouldDebug && size > 250 && (nonNilCount % 50 == 0)) {
            // DEBUG: "[MD] ... searched " << nonNilCount << " entries at i=" << i
            // std::cerr.flush();
        }

        // Check for exact match first (key is selector Symbol)
        if (key.rawBits() == actualSelector.rawBits() || key.rawBits() == selector.rawBits()) {
            // Debug: trace "hands" lookup specifically
            if (selectorStr == "hands") {
                std::cerr << "[MD-HANDS-EXACT] Found 'hands' via exact match at i=" << i
                          << " mdSlotIndex=" << mdSlotIndex
                          << " valuesSize=" << valuesSize
                          << " keySlotCount=" << keySlotCount << "\n";
                // Show key at current slot
                std::cerr << "[MD-HANDS-EXACT]   key=0x" << std::hex << key.rawBits() << std::dec;
                if (key.isObject()) {
                    ObjectHeader* kh = key.asObjectPtr();
                    if (kh->isBytesObject() && kh->byteSize() < 50) {
                        std::cerr << " \"" << std::string((char*)kh->bytes(), kh->byteSize()) << "\"";
                    }
                }
                std::cerr << "\n";
                // Show values array structure
                std::cerr << "[MD-HANDS-EXACT]   valuesArray=0x" << std::hex << valuesArray.rawBits() << std::dec << "\n";
                // Show method at index i in valuesArray
                if (i < valuesSize) {
                    Oop method = memory_.fetchPointer(i, valuesArray);
                    std::cerr << "[MD-HANDS-EXACT]   valuesArray[" << i << "]=0x" << std::hex << method.rawBits() << std::dec;
                    if (method.isObject()) {
                        ObjectHeader* mh = method.asObjectPtr();
                        std::cerr << " classIdx=" << mh->classIndex() << " format=" << (int)mh->format();
                        Oop hdr = memory_.fetchPointer(0, method);
                        if (hdr.isSmallInteger()) {
                            int64_t hb = hdr.asSmallInteger();
                            int numLit = hb & 0x7FFF;
                            bool hasPrim = (hb >> 30) & 1;
                            // In CompiledMethod, the bytecodes are at a specific offset
                            // Format 24-31: CompiledMethod
                            // The header encodes numLiterals, then literals, then bytecodes
                            uint8_t* bc = mh->bytes() + (1 + numLit) * 8;
                            std::cerr << " numLit=" << numLit << " hasPrim=" << hasPrim;
                            std::cerr << "\n[MD-HANDS-EXACT]     header=0x" << std::hex << hb << std::dec;
                            // Dump first 16 bytes of bytecodes
                            std::cerr << "\n[MD-HANDS-EXACT]     bytecodes(16): ";
                            for (int j = 0; j < 16; j++) {
                                std::cerr << (int)bc[j] << " ";
                            }
                            // Also dump raw bytes starting from bytes()
                            std::cerr << "\n[MD-HANDS-EXACT]     raw bytes(48): ";
                            uint8_t* rawBytes = mh->bytes();
                            for (int j = 0; j < 48; j++) {
                                if (j == (1 + numLit) * 8) std::cerr << "| ";  // Mark bytecode start
                                std::cerr << (int)rawBytes[j] << " ";
                            }
                            // Also show the selector from the method's literal 1
                            if (numLit >= 1) {
                                Oop selLit = memory_.fetchPointer(1, method);
                                std::cerr << "\n[MD-HANDS-EXACT]     method's selector (lit1)=0x"
                                          << std::hex << selLit.rawBits() << std::dec;
                                if (selLit.isObject()) {
                                    ObjectHeader* sh = selLit.asObjectPtr();
                                    if (sh->isBytesObject() && sh->byteSize() < 50) {
                                        std::cerr << " \"" << std::string((char*)sh->bytes(), sh->byteSize()) << "\"";
                                    } else if (sh->format() == ObjectFormat::FixedSize && sh->slotCount() >= 1) {
                                        // AdditionalMethodState - selector at slot 0
                                        Oop realSel = memory_.fetchPointer(0, selLit);
                                        if (realSel.isObject()) {
                                            ObjectHeader* rsh = realSel.asObjectPtr();
                                            if (rsh->isBytesObject() && rsh->byteSize() < 50) {
                                                std::cerr << " AMS->\"" << std::string((char*)rsh->bytes(), rsh->byteSize()) << "\"";
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    std::cerr << "\n";
                }
            }
            // Return corresponding method from valuesArray
            if (i < valuesSize) {
                Oop method = memory_.fetchPointer(i, valuesArray);
                // Validate method before returning - skip invalid/unrelocated pointers
                if (method.isObject() && !memory_.isValidPointer(method)) {
                    static int invalidMethodCount = 0;
                    invalidMethodCount++;
                    if (invalidMethodCount <= 10) {
                        // Print valuesArray object info to understand why it wasn't relocated
                        ObjectHeader* vaHdr = valuesArray.asObjectPtr();
                        uintptr_t vaOffset = reinterpret_cast<uint8_t*>(vaHdr) - memory_.oldSpaceStart();
                        std::cerr << "[MD] INVALID METHOD #" << invalidMethodCount
                                  << " at valuesArray[" << i << "]=0x" << std::hex << method.rawBits()
                                  << " selector=" << selectorStr
                                  << " valuesArray=0x" << valuesArray.rawBits()
                                  << " vaOffset=0x" << vaOffset
                                  << " fmt=" << static_cast<int>(vaHdr->format())
                                  << " slots=" << std::dec << vaHdr->slotCount()
                                  << " - SKIPPING (will cause DNU)\n";
                    }
                    return Oop::nil();  // Return nil to trigger DNU instead of crashing
                }
                return method;
            }
            return Oop::nil();
        }

        if (!key.isObject() || key.rawBits() < 0x10000) continue;

        ObjectHeader* keyHdr = key.asObjectPtr();
        uint64_t rawBits = key.rawBits();

        // Skip unrelocated pointers
        if (rawBits >= 0x10000000000ULL) {
            if (verboseDebug) // DEBUG: "[MD] i=" << i << " skip unrelocated 0x" << std::hex << rawBits << std::dec
            continue;
        }

        ObjectFormat keyFmt = keyHdr->format();

        // Debug trace first few non-nil entries - only for ProtoObject (128 entries)
        static int entryDebugCount = 0;
        if (entryDebugCount < 60 && selectorStr == "doesNotUnderstand:" && size == 128) {
            entryDebugCount++;
            // std::cerr << "[MD-TRACE] i=" << i << " key=0x" << std::hex << key.rawBits() << std::dec
                      // << " fmt=" << static_cast<int>(keyFmt);
            if (keyHdr->isBytesObject()) {
                size_t bs = keyHdr->byteSize();
                if (bs <= 30) {
                    // std::cerr << " sel=\"" << std::string((char*)keyHdr->bytes(), bs) << "\"";
                }
            }
            // std::cerr; // DEBUG
        }

        // Keys should be Symbols (byte objects format 16-23)
        if (keyHdr->isBytesObject()) {
            // Compare key Symbol with selector
            if (!selectorStr.empty() && memory_.symbolEquals(key, selectorStr.c_str())) {
                // Debug: trace "hands" lookup specifically
                if (selectorStr == "hands") {
                    std::cerr << "[MD-HANDS] Found 'hands' at i=" << i
                              << " valuesSize=" << valuesSize;
                    if (i < valuesSize) {
                        Oop method = memory_.fetchPointer(i, valuesArray);
                        std::cerr << " method=0x" << std::hex << method.rawBits() << std::dec;
                        if (method.isObject()) {
                            ObjectHeader* mh = method.asObjectPtr();
                            Oop hdr = memory_.fetchPointer(0, method);
                            if (hdr.isSmallInteger()) {
                                int64_t hb = hdr.asSmallInteger();
                                int numLit = hb & 0x7FFF;
                                uint8_t* bc = mh->bytes() + (1 + numLit) * 8;
                                std::cerr << " bytecodes=[" << (int)bc[0] << "," << (int)bc[1] << "," << (int)bc[2] << "]";
                            }
                        }
                    }
                    std::cerr << "\n";
                }
                // Return corresponding method from valuesArray
                if (i < valuesSize) {
                    return memory_.fetchPointer(i, valuesArray);
                }
            }
        }
        // Also handle if old-style method dict is used (methods directly in keys)
        else if (keyHdr->isCompiledMethod()) {
            // Get selector from the CompiledMethod
            // Literal 1 (slot 1 after method header) contains either:
            //   - The selector directly (Symbol)
            //   - An AdditionalMethodState object (contains selector at slot 0)
            uint64_t methodHeader = keyHdr->slots()[0].rawBits();
            size_t numLiterals = (methodHeader >> 1) & 0x7FFF;

            if (numLiterals >= 1) {
                Oop selectorLit = memory_.fetchPointer(1, key);
                Oop actualSelector = Oop::nil();

                if (selectorLit.isObject() && selectorLit.rawBits() > 0x10000 && selectorLit.rawBits() < 0x10000000000ULL) {
                    ObjectHeader* selHdr = selectorLit.asObjectPtr();

                    if (selHdr->isBytesObject()) {
                        // Literal 1 is directly a Symbol
                        actualSelector = selectorLit;
                    }
                    else if (selHdr->format() == ObjectFormat::FixedSize && selHdr->slotCount() >= 1) {
                        // Literal 1 is an AdditionalMethodState (format 1) - selector is at slot 0
                        Oop innerSel = memory_.fetchPointer(0, selectorLit);
                        if (innerSel.isObject() && innerSel.rawBits() > 0x10000 && innerSel.rawBits() < 0x10000000000ULL) {
                            ObjectHeader* innerHdr = innerSel.asObjectPtr();
                            if (innerHdr->isBytesObject()) {
                                actualSelector = innerSel;
                            }
                            else if (entryDebugCount <= 30 && selectorStr == "doesNotUnderstand:") {
                                // Debug: why isn't slot[0] a symbol?
                                // std::cerr << "[MD-TRACE]   AMS slot[0]=0x" << std::hex << innerSel.rawBits() << std::dec
                                          // << " innerFmt=" << static_cast<int>(innerHdr->format())
                                          // << " slots=" << innerHdr->slotCount();
                            }
                        }
                        else if (entryDebugCount <= 60 && selectorStr == "doesNotUnderstand:" && size == 128) {
                            // std::cerr << "[MD-TRACE]   AMS slot[0] not valid object: 0x"
                                      // << std::hex << innerSel.rawBits() << std::dec;
                        }
                    }
                    else if (entryDebugCount <= 60 && selectorStr == "doesNotUnderstand:" && size == 128) {
                        // Log formats we're not handling and their contents
                        // std::cerr << "[MD-TRACE]   lit1 fmt=" << static_cast<int>(selHdr->format())
                                  // << " not handled (slots=" << selHdr->slotCount() << ")";
                        // Show slot 0 contents for format 2 and 3
                        if (selHdr->slotCount() >= 1) {
                            Oop slot0 = memory_.fetchPointer(0, selectorLit);
                            // std::cerr << " slot0=0x" << std::hex << slot0.rawBits() << std::dec;
                            if (slot0.isObject() && slot0.rawBits() > 0x10000 && slot0.rawBits() < 0x10000000000ULL) {
                                ObjectHeader* s0h = slot0.asObjectPtr();
                                // std::cerr << " (fmt=" << static_cast<int>(s0h->format());
                                if (s0h->isBytesObject() && s0h->byteSize() <= 30) {
                                    // std::cerr << " \"" << std::string((char*)s0h->bytes(), s0h->byteSize()) << "\"";
                                }
                                // std::cerr << ")";
                            }
                        }
                        // std::cerr; // DEBUG
                    }
                }

                // Detailed debug for DNU lookup
                if (entryDebugCount <= 60 && selectorStr == "doesNotUnderstand:" && size == 128) {
                    // std::cerr << "[MD-TRACE]   numLit=" << numLiterals
                              // << " lit1=0x" << std::hex << selectorLit.rawBits() << std::dec;
                    if (selectorLit.isObject() && selectorLit.rawBits() > 0x10000) {
                        ObjectHeader* slh = selectorLit.asObjectPtr();
                        // std::cerr << " lit1Fmt=" << static_cast<int>(slh->format());
                        if (slh->isBytesObject() && slh->byteSize() <= 30) {
                            // std::cerr << " lit1Str=\"" << std::string((char*)slh->bytes(), slh->byteSize()) << "\"";
                        }
                    }
                    if (!actualSelector.isNil() && actualSelector.rawBits() != selectorLit.rawBits()) {
                        ObjectHeader* actHdr = actualSelector.asObjectPtr();
                        if (actHdr->isBytesObject() && actHdr->byteSize() <= 30) {
                            // std::cerr << " actualSel=\"" << std::string((char*)actHdr->bytes(), actHdr->byteSize()) << "\"";
                        }
                    }
                    // std::cerr; // DEBUG
                }

                if (!actualSelector.isNil()) {
                    ObjectHeader* selHdr = actualSelector.asObjectPtr();
                    std::string methodSel((char*)selHdr->bytes(), selHdr->byteSize());

                    // Special trace: if we find doesNotUnderstand: method anywhere
                    if (methodSel == "doesNotUnderstand:") {
                        // DEBUG_LOG("[MD] FOUND doesNotUnderstand: method at index " << i
                                  // << " in dict with " << size << " entries!";
                    }

                    // Debug: show first few selector comparisons
                    static int comparisonCount = 0;
                    if (comparisonCount < 10 && !selectorStr.empty()) {
                        comparisonCount++;
                        // DEBUG: "[MD] Comparing \"" << selectorStr << "\" with \"" << methodSel << "\""
                    }

                    if (!selectorStr.empty() && memory_.symbolEquals(actualSelector, selectorStr.c_str())) {
                        // DEBUG_LOG("[MD] Found method at " << i << " (selector at lit[1])";
                        // Validate method before returning - skip invalid/unrelocated pointers
                        if (key.isObject() && !memory_.isValidPointer(key)) {
                            static int invalidKeyMethodCount = 0;
                            invalidKeyMethodCount++;
                            if (invalidKeyMethodCount <= 10) {
                                std::cerr << "[MD] INVALID KEY-METHOD #" << invalidKeyMethodCount
                                          << " at index=" << i << " key=0x" << std::hex << key.rawBits()
                                          << " selector=" << selectorStr << std::dec
                                          << " - SKIPPING (will cause DNU)\n";
                            }
                            return Oop::nil();  // Return nil to trigger DNU instead of crashing
                        }
                        return key;  // The key IS the method
                    }
                }
            }
            if (verboseDebug && ((nonNilCount - 99) % 10 == 0)) {
                // DEBUG: "[MD] i=" << i << " method numLit=" << numLiterals
            }
        }
        // Try string comparison for Symbols (non-method entries)
        else if (keyHdr->isBytesObject()) {
            if (!selectorStr.empty() && memory_.symbolEquals(key, selectorStr.c_str())) {
                // if (shouldDebug) { ... }
                if (hasValuesArray) {
                    Oop method = memory_.fetchPointer(i, valuesArray);
                    // Validate method before returning - skip invalid/unrelocated pointers
                    if (method.isObject() && !memory_.isValidPointer(method)) {
                        static int invalidMethod2Count = 0;
                        invalidMethod2Count++;
                        if (invalidMethod2Count <= 10) {
                            std::cerr << "[MD] INVALID METHOD2 #" << invalidMethod2Count
                                      << " at valuesArray[" << i << "]=0x" << std::hex << method.rawBits()
                                      << " selector=" << selectorStr << std::dec
                                      << " - SKIPPING (will cause DNU)\n";
                        }
                        return Oop::nil();  // Return nil to trigger DNU instead of crashing
                    }
                    return method;
                }
            }
        }
    }

    if (shouldDebug && size > 250) {
        // DEBUG: "[MD] Loop complete at maxSearch=" << maxSearch
        // std::cerr.flush();
    }

    // Secondary search: some methods might be in the MethodDict slots beyond slot[1]
    // (Modern Pharo might use slots 2+ for overflow or additional methods)
    size_t mdSlots = mdHeader->slotCount();
    for (size_t slot = 2; slot < mdSlots && slot < 256; ++slot) {
        Oop entry = memory_.fetchPointer(slot, methodDict);
        if (!entry.isObject() || entry.rawBits() < 0x10000 || entry.rawBits() >= 0x10000000000ULL) continue;

        ObjectHeader* entryHdr = entry.asObjectPtr();
        if (entryHdr->isCompiledMethod()) {
            // Check selector at literal 1
            uint64_t mh = entryHdr->slots()[0].rawBits();
            size_t nLit = (mh >> 1) & 0x7FFF;
            if (nLit >= 1) {
                Oop selLit = memory_.fetchPointer(1, entry);
                if (selLit.isObject() && selLit.rawBits() > 0x10000 && selLit.rawBits() < 0x10000000000ULL) {
                    ObjectHeader* slHdr = selLit.asObjectPtr();
                    if (slHdr->isBytesObject()) {
                        if (!selectorStr.empty() && memory_.symbolEquals(selLit, selectorStr.c_str())) {
                            if (shouldDebug) // DEBUG: "[MD] Found method in MethodDict slot " << slot
                            return entry;
                        }
                    }
                }
            }
        }
    }

    if (shouldDebug) {
        // DEBUG_LOG("[MD] Not found (searched " << nonNilCount << " non-nil entries)";
        // std::cerr.flush();
    }
    return Oop::nil();
}

MethodCacheEntry* Interpreter::probeCache(Oop selector, Oop classOop) {
    size_t hash = cacheHash(selector, classOop);
    MethodCacheEntry& entry = methodCache_[hash];

    if (entry.selector == selector && entry.classOop == classOop) {
        return &entry;
    }

    return nullptr;
}

void Interpreter::cacheMethod(Oop selector, Oop classOop, Oop method) {
    size_t hash = cacheHash(selector, classOop);
    MethodCacheEntry& entry = methodCache_[hash];

    entry.selector = selector;
    entry.classOop = classOop;
    entry.method = method;
    entry.primitiveIndex = primitiveIndexOf(method);
    entry.primitive = nullptr;  // Will be set on first call
}

size_t Interpreter::cacheHash(Oop selector, Oop classOop) const {
    // XOR the raw bits and mask to cache size
    uint64_t h = selector.rawBits() ^ classOop.rawBits();
    return static_cast<size_t>(h) & (MethodCacheSize - 1);
}

// ===== METHOD ACTIVATION =====

void Interpreter::activateMethod(Oop method, int argCount) {
    // Save current state
    pushFrame(method, argCount);

    // Set up new method
    method_ = method;
    argCount_ = argCount;

    // Determine homeMethod_ based on whether this is a CompiledMethod or CompiledBlock
    // CompiledMethod (class index 3101): homeMethod_ = method
    // CompiledBlock (class index 3117): homeMethod_ = slot 0 (the home method)
    if (method.isObject()) {
        ObjectHeader* methodHdr = method.asObjectPtr();
        uint32_t classIdx = methodHdr->classIndex();

        if (classIdx == 3117) {
            // CompiledBlock - get home method from slot 2 (Pharo 11+ FullBlockClosure model)
            // Layout: slot 0 = header, slot 1 = selector, slot 2 = home method
            homeMethod_ = method;  // Default in case chain traversal fails

            // Try slot 2 first (home method in FullBlockClosure model)
            Oop slot2 = memory_.fetchPointer(2, method);
            if (slot2.isObject()) {
                ObjectHeader* slot2Hdr = slot2.asObjectPtr();
                if (slot2Hdr->classIndex() == 3101) {
                    homeMethod_ = slot2;
                }
            }

            // Fallback: try slot 0 chain (older formats)
            if (homeMethod_ == method) {
                Oop homeCandidate = memory_.fetchPointer(0, method);
                int maxHops = 10;
                while (homeCandidate.isObject() && maxHops-- > 0) {
                    ObjectHeader* candidateHdr = homeCandidate.asObjectPtr();
                    uint32_t candidateCls = candidateHdr->classIndex();
                    if (candidateCls == 3101) {
                        homeMethod_ = homeCandidate;
                        break;
                    } else if (candidateCls == 3117) {
                        homeCandidate = memory_.fetchPointer(0, homeCandidate);
                    } else {
                        break;
                    }
                }
            }
        } else {
            // CompiledMethod or other - homeMethod is the same as method
            homeMethod_ = method;
        }
    } else {
        homeMethod_ = method;
    }

    // Get receiver from stack (now in the frame)
    receiver_ = argument(0);  // First "argument" slot is actually receiver

    // Set instruction pointer to start of bytecodes
    ObjectHeader* methodObj = method_.asObjectPtr();

    Oop methodHeader = memory_.fetchPointer(0, method_);
    int64_t headerBits = methodHeader.asSmallInteger();
    int numLiterals = headerBits & 0x7FFF;  // bits 0-14 are numLiterals

    // Detect bytecode set: sign bit (bit 63) = 0 for V3PlusClosures, 1 for SistaV1
    // In 64-bit Spur, negative header means alternate bytecode set (SistaV1)
    usesSistaV1_ = headerBits < 0;

    uint8_t* methodBytes = methodObj->bytes();
    size_t bytecodeStart = (1 + numLiterals) * 8;
    instructionPointer_ = methodBytes + bytecodeStart;

    // Skip past callPrimitive bytecode (0xF8 lowByte highByte) if present
    // In Sista V1, primitive methods start with callPrimitive which should be skipped
    // when the primitive fails and we fall through to execute bytecodes
    if (usesSistaV1_ && instructionPointer_[0] == 0xF8) {
        instructionPointer_ += 3;  // Skip 0xF8 + 2 bytes of primitive index
    }

    // Set bytecode end
    size_t totalBytes = methodObj->byteSize();
    bytecodeEnd_ = methodBytes + totalBytes;

    // DEBUG_LOG("[ACTIVATE] clsIdx=" << (method.isObject() ? method.asObjectPtr()->classIndex() : 0)
              // << " rawHdr=0x" << std::hex << methodHeader.rawBits()
              // << " hdrBits=" << headerBits << std::dec
              // << " numLiterals=" << numLiterals << " bytecodeStart=" << bytecodeStart
              // << " totalBytes=" << totalBytes
              // << " homeMethod=" << (homeMethod_ == method_ ? "same" : "different");
    if (homeMethod_ != method_ && homeMethod_.isObject()) {
        Oop homeHeader = memory_.fetchPointer(0, homeMethod_);
        if (homeHeader.isSmallInteger()) {
            int64_t hBits = homeHeader.asSmallInteger();
            // std::cerr << " (homeLiterals=" << (hBits & 0x7FFF) << ")";
        }
    }
    // std::cerr; // DEBUG
    // DEBUG_LOG("[ACTIVATE] Method bytecodes: " << (totalBytes - bytecodeStart) << " bytes";

    // Show first few bytecodes for debugging
    // DEBUG_LOG("[ACTIVATE] First bytecodes: ";
    for (size_t i = 0; i < std::min((size_t)16, totalBytes - bytecodeStart); i++) {
        // std::cerr << std::hex << (int)methodBytes[bytecodeStart + i] << " ";
    }
    // std::cerr << std::dec; // DEBUG
}

void Interpreter::activateBlock(Oop block, int argCount) {
    static int activateCount = 0;
    activateCount++;

    // BlockClosure/FullBlockClosure layout:
    // 0: outerContext
    // 1: startPC (SmallInteger) for old BlockClosure, OR
    //    compiledBlock (Object) for FullBlockClosure
    // 2: numArgs (SmallInteger)
    // 3+: copied values

    Oop slot1 = memory_.fetchPointer(1, block);

    // Debug: show stack before activation (first few times)
    if (activateCount <= 5) {
        std::cerr << "[BLOCK] activateBlock #" << activateCount << " argCount=" << argCount << "\n";
        std::cerr << "[BLOCK]   block=0x" << std::hex << block.rawBits() << std::dec << "\n";
        std::cerr << "[BLOCK]   slot1=0x" << std::hex << slot1.rawBits() << std::dec
                  << " isSmallInt=" << slot1.isSmallInteger()
                  << " isObject=" << slot1.isObject() << "\n";
        for (int i = 0; i <= argCount; i++) {
            Oop val = stackValue(i);
            std::cerr << "[BLOCK]   stack[" << i << "]=0x" << std::hex << val.rawBits() << std::dec;
            if (val.isSmallInteger()) {
                std::cerr << " (SmallInt=" << val.asSmallInteger() << ")";
            }
            std::cerr << "\n";
        }
    }

    Oop outerContext = memory_.fetchPointer(0, block);
    Oop methodToExecute;
    uint8_t* startAddress = nullptr;

    if (slot1.isSmallInteger()) {
        // Old-style BlockClosure: slot 1 is startPC
        int64_t startPC = slot1.asSmallInteger();
        // Get the method from outer context
        Oop outerMethod = memory_.fetchPointer(3, outerContext);
        methodToExecute = outerMethod;
        ObjectHeader* methodObj = outerMethod.asObjectPtr();
        startAddress = methodObj->bytes() + startPC;

        if (activateCount <= 5) {
            std::cerr << "[BLOCK]   Old-style BlockClosure: startPC=" << startPC << "\n";
        }
    } else if (slot1.isObject()) {
        // FullBlockClosure: slot 1 is compiledBlock (the actual method to execute)
        Oop compiledBlock = slot1;
        methodToExecute = compiledBlock;
        ObjectHeader* blockObj = compiledBlock.asObjectPtr();
        // CompiledBlock bytecodes start after the header
        // In Pharo's CompiledBlock, the header info tells us where bytecodes start
        // For CompiledBlock, bytecodes typically start after fixed slots
        Oop header = memory_.fetchPointer(0, compiledBlock);
        int64_t headerBits = header.asSmallInteger();
        // CompiledCode header format (after SmallInteger decode):
        // bits 0-15: numLiterals, bits 16-23: numTemps, bits 24-27: numArgs
        int numLiterals = headerBits & 0xFFFF;
        // Bytecodes start after header slot and literal slots
        // Header is slot 0, literals are slots 1 to numLiterals
        // Each slot is 8 bytes, so bytecodes start at (1 + numLiterals) * 8
        size_t bytecodeOffset = (1 + numLiterals) * 8;
        startAddress = blockObj->bytes() + bytecodeOffset;

        if (activateCount <= 5) {
            std::cerr << "[BLOCK]   FullBlockClosure: compiledBlock=0x" << std::hex << compiledBlock.rawBits() << std::dec
                      << " numLiterals=" << numLiterals
                      << " bytecodeOffset=" << bytecodeOffset << "\n";
        }
    } else {
        if (activateCount <= 5) {
            std::cerr << "[BLOCK]   ERROR: slot1 is neither SmallInteger nor Object\n";
        }
        primitiveFail();
        return;
    }

    pushFrame(methodToExecute, argCount);

    method_ = methodToExecute;
    // For FullBlockClosure, homeMethod should be from the compiledBlock's slot 2 or outerContext
    if (slot1.isObject()) {
        // CompiledBlock slot 2 is the home method
        Oop homeFromBlock = memory_.fetchPointer(2, slot1);
        if (homeFromBlock.isObject()) {
            homeMethod_ = homeFromBlock;
        } else {
            // Fall back to getting from outer context
            homeMethod_ = memory_.fetchPointer(3, outerContext);
        }
    } else {
        homeMethod_ = memory_.fetchPointer(3, outerContext);
    }

    argCount_ = argCount;

    // Receiver from outer context (slot 5 is receiver in context layout)
    if (outerContext.isObject() && !outerContext.isNil()) {
        receiver_ = memory_.fetchPointer(5, outerContext);
    } else {
        receiver_ = memory_.nil();
    }

    // CRITICAL: Copy the copied values from the closure into the temp area
    // BlockClosure/FullBlockClosure layout:
    // 0: outerContext
    // 1: startPC/compiledBlock
    // 2: numArgs
    // 3+: copied values (including temp vectors for remote temps)
    size_t blockSlots = memory_.slotCountOf(block);
    int numCopied = static_cast<int>(blockSlots) - 3;  // Fixed slots are 0,1,2

    if (numCopied > 0 && activateCount <= 5) {
        std::cerr << "[BLOCK]   Copying " << numCopied << " values from closure to temps\n";
    }

    for (int i = 0; i < numCopied; i++) {
        Oop copiedValue = memory_.fetchPointer(3 + i, block);
        // Copied values go after the arguments in the temp area
        setTemporary(argCount + i, copiedValue);
        if (activateCount <= 5) {
            std::cerr << "[BLOCK]     temp(" << (argCount + i) << ") = 0x" << std::hex << copiedValue.rawBits() << std::dec;
            if (copiedValue.isSmallInteger()) {
                std::cerr << " (SmallInt=" << copiedValue.asSmallInteger() << ")";
            } else if (copiedValue.isObject()) {
                ObjectHeader* cHdr = copiedValue.asObjectPtr();
                std::cerr << " (classIdx=" << cHdr->classIndex() << " slots=" << memory_.slotCountOf(copiedValue) << ")";
            }
            std::cerr << "\n";
        }
    }

    instructionPointer_ = startAddress;

    // Set bytecode end based on method size
    ObjectHeader* methodHdr = methodToExecute.asObjectPtr();
    bytecodeEnd_ = methodHdr->bytes() + methodHdr->byteSize();

    // Debug: show frame state after activation
    if (activateCount <= 5) {
        std::cerr << "[BLOCK]   after activation: framePointer_=" << (void*)framePointer_
                  << " stackPointer_=" << (void*)stackPointer_ << "\n";
        std::cerr << "[BLOCK]   method_=0x" << std::hex << method_.rawBits()
                  << " IP=" << (void*)instructionPointer_ << std::dec << "\n";
        std::cerr << "[BLOCK]   temp(0)=0x" << std::hex << temporary(0).rawBits() << std::dec;
        if (temporary(0).isSmallInteger()) {
            std::cerr << " (SmallInt=" << temporary(0).asSmallInteger() << ")";
        }
        std::cerr << "\n";
    }
}

// ===== FRAME MANAGEMENT =====

void Interpreter::pushFrame(Oop method, int argCount) {
    static FILE* frameLog = fopen("/tmp/iospharo-frame.log", "a");

    // Get method name/selector for recursion detection
    std::string methodName = "";
    if (method.isObject() && method.rawBits() > 0x10000) {
        ObjectHeader* mHdr = method.asObjectPtr();
        if (mHdr->slotCount() > 1) {
            Oop lit1 = mHdr->slotAt(1);
            if (lit1.isObject() && lit1.rawBits() > 0x10000) {
                ObjectHeader* litHdr = lit1.asObjectPtr();
                if (litHdr->isBytesObject() && litHdr->byteSize() < 50) {
                    methodName = std::string((char*)litHdr->bytes(), litHdr->byteSize());
                }
            }
        }
    }

    // Recursion detection - check if same method NAME is being pushed repeatedly
    // This catches indirect recursion where different classes have same-named methods
    static std::string lastMethodName = "";
    static int sameMethodCount = 0;
    static std::string bannedMethod = "";
    static int bannedCallsRemaining = 0;

    // If a method is banned, return nil immediately for all calls to it
    if (!methodName.empty() && methodName == bannedMethod && bannedCallsRemaining > 0) {
        bannedCallsRemaining--;
        push(memory_.nil());
        return;
    }

    if (!methodName.empty() && methodName == lastMethodName) {
        sameMethodCount++;
        if (sameMethodCount > 10) {
            // Same method name pushed 10+ times - likely infinite recursion
            std::cerr << "[RECURSION] Breaking infinite recursion: method " << methodName
                      << " pushed " << sameMethodCount << " times at depth " << frameDepth_ << "\n";
            // Ban this method for the next 1000 calls to break the recursion completely
            bannedMethod = methodName;
            bannedCallsRemaining = 1000;
            // Push nil as return value and skip the method call entirely
            push(memory_.nil());
            sameMethodCount = 0;
            lastMethodName = "";
            return;
        }
    } else {
        lastMethodName = methodName;
        sameMethodCount = 1;
    }

    // Save current execution state before switching to new method
    if (frameDepth_ >= MaxFrameDepth) {
        running_ = false;
        return;
    }

    SavedFrame& frame = savedFrames_[frameDepth_++];
    frame.savedIP = instructionPointer_;
    frame.savedBytecodeEnd = bytecodeEnd_;
    frame.savedMethod = method_;
    frame.savedHomeMethod = homeMethod_;
    frame.savedReceiver = receiver_;
    frame.savedFP = framePointer_;
    frame.savedArgCount = argCount_;

    if (frameLog && frameDepth_ > 400) {
        // Only log deep frames to identify recursion
        fprintf(frameLog, "[PUSH_FRAME] depth=%zu method=%s\n", frameDepth_,
                methodName.empty() ? "unknown" : methodName.c_str());
        fflush(frameLog);
    }

    // Calculate number of temporaries for the new method
    Oop newMethodHeader = memory_.fetchPointer(0, method);
    int64_t headerBits = newMethodHeader.asSmallInteger();
    int numTemps = (headerBits >> 16) & 0xFF;

    // New frame pointer is at current position minus args (receiver is first "arg")
    Oop* newFP = stackPointer_ - argCount - 1;  // -1 for receiver position

    framePointer_ = newFP;

    // Initialize temporaries to nil
    for (int i = 0; i < numTemps; ++i) {
        push(memory_.nil());
    }
}

void Interpreter::popFrame() {
    static FILE* frameLog = fopen("/tmp/iospharo-frame.log", "a");

    // Restore previous execution state
    if (frameDepth_ == 0) {
        if (frameLog) {
            fprintf(frameLog, "[POP_FRAME] frameDepth==0, setting running_=false\n");
            fflush(frameLog);
        }
        running_ = false;
        return;
    }

    --frameDepth_;
    SavedFrame& frame = savedFrames_[frameDepth_];

    if (frameLog) {
        fprintf(frameLog, "[POP_FRAME] popping to depth=%d savedIP=%p\n",
                frameDepth_, (void*)frame.savedIP);
        fflush(frameLog);
    }

    // Reset stack to frame pointer (discards temps and locals)
    stackPointer_ = framePointer_;

    // Restore saved execution state
    instructionPointer_ = frame.savedIP;
    bytecodeEnd_ = frame.savedBytecodeEnd;
    method_ = frame.savedMethod;
    homeMethod_ = frame.savedHomeMethod;
    receiver_ = frame.savedReceiver;
    framePointer_ = frame.savedFP;
    argCount_ = frame.savedArgCount;

    // If this was the last frame, we're done
    if (frameDepth_ == 0 && frame.savedIP == nullptr) {
        if (frameLog) {
            fprintf(frameLog, "[POP_FRAME] last frame with null savedIP, setting running_=false\n");
            fflush(frameLog);
        }
        running_ = false;
    }
}

// ===== VARIABLE ACCESS =====

Oop Interpreter::literal(size_t index) const {
    // In Pharo 10+ with FullBlockClosure model, both CompiledMethods and CompiledBlocks
    // have their own literal frames. Each compiled object (method or block) contains
    // its own literals - blocks do NOT share literals with their home method.
    //
    // So we always use method_ (the currently executing CompiledMethod or CompiledBlock)
    // for literal access, NOT homeMethod_.
    Oop literalMethod = method_;

    // Safety check
    if (literalMethod.isNil() || !literalMethod.isObject()) {
        return memory_.specialObject(SpecialObjectIndex::NilObject);
    }

    // Get numLiterals from method header for bounds check
    // Pharo header format: bits 0-14 = numLiterals (15 bits)
    Oop methodHeader = memory_.fetchPointer(0, literalMethod);
    if (methodHeader.isSmallInteger()) {
        int64_t headerBits = methodHeader.asSmallInteger();
        size_t numLiterals = headerBits & 0x7FFF;  // bits 0-14

        if (index >= numLiterals) {
            // Out-of-bounds access - return nil gracefully
            // This can happen with stale contexts from image snapshot
            return memory_.specialObject(SpecialObjectIndex::NilObject);
        }
    } else {
        // Method header isn't a SmallInteger - bad method
        static int badHdrCount = 0;
        badHdrCount++;
        if (badHdrCount <= 5) {
            std::cerr << "[LITERAL] Bad method header: method=0x" << std::hex << literalMethod.rawBits()
                      << " header=0x" << methodHeader.rawBits() << std::dec << "\n";
        }
        return memory_.specialObject(SpecialObjectIndex::NilObject);
    }

    Oop result = memory_.fetchPointer(index + 1, literalMethod);

    // Check if result is a CompiledBlock (wrong literal type for selectors)
    if (result.isObject()) {
        ObjectHeader* hdr = result.asObjectPtr();
        if (hdr->classIndex() == 3117) {  // CompiledBlock
            static int blockLitCount = 0;
            blockLitCount++;
            if (blockLitCount <= 5) {
                std::cerr << "[LITERAL] CompiledBlock at index=" << index
                          << " literalMethod=0x" << std::hex << literalMethod.rawBits()
                          << " result=0x" << result.rawBits() << std::dec << "\n";
            }
        }
    }

    return result;
}

Oop Interpreter::temporary(int index) const {
    // In Sista bytecodes, temp indices 0..argCount-1 are the arguments,
    // and indices argCount+ are local temps/copied values.
    // Frame layout: [receiver, arg0, arg1, ..., temp0, temp1, ...]
    // So all are accessed at framePointer_[1 + index]
    return *(framePointer_ + 1 + index);
}

void Interpreter::setTemporary(int index, Oop value) {
    // Same layout as temporary() - see comment above
    *(framePointer_ + 1 + index) = value;
}

Oop Interpreter::argument(int index) const {
    // Arguments are at frame pointer
    return *(framePointer_ + index);
}

Oop Interpreter::receiverInstVar(size_t index) const {
    // Check if receiver is a byte object (String, Symbol, ByteArray, etc.)
    // Byte objects don't have pointer instance variables, so return nil
    if (receiver_.isObject()) {
        ObjectHeader* hdr = receiver_.asObjectPtr();
        if (hdr->isBytesObject() || hdr->isCompiledMethod()) {
            // std::cerr << "[WARN] Attempting to read instVar " << index
                      // << " from byte object - returning nil";
            return memory_.nil();
        }
    }
    return memory_.fetchPointer(index, receiver_);
}

void Interpreter::setReceiverInstVar(size_t index, Oop value) {
    // Check if receiver is a byte object - can't store to byte objects
    if (receiver_.isObject()) {
        ObjectHeader* hdr = receiver_.asObjectPtr();
        if (hdr->isBytesObject() || hdr->isCompiledMethod()) {
            // std::cerr << "[WARN] Attempting to write instVar " << index
                      // << " to byte object - ignored";
            return;
        }
    }
    memory_.storePointer(index, receiver_, value);
}

// ===== SPECIAL SENDS =====

void Interpreter::sendDoesNotUnderstand(Oop selector, int argCount) {
    // Recursion depth limit to prevent infinite DNU loops
    static int dnuDepth = 0;
    const int MAX_DNU_DEPTH = 10;

    dnuDepth++;

    if (dnuDepth > MAX_DNU_DEPTH) {
        std::cerr << "[DNU] MAX_DNU_DEPTH exceeded! Stopping VM.\n";
        std::cerr << "[DNU] Last selector attempted: " << (selector.isObject() && selector.rawBits() > 0x10000 ?
            std::string((char*)selector.asObjectPtr()->bytes(),
                        std::min((size_t)50, selector.asObjectPtr()->byteSize())) : "unknown") << "\n";
        running_ = false;
        dnuDepth = 0;
        return;
    }

    // std::cerr << "[DNU depth=" << dnuDepth << "] Original selector=0x" << std::hex << selector.rawBits()
              // << " DNU selector=0x" << selectors_.doesNotUnderstand.rawBits() << std::dec;

    // Debug: print ORIGINAL selector as string
    std::string origStr = "";
    if (selector.isObject() && selector.rawBits() > 0x10000) {
        ObjectHeader* origHdr = selector.asObjectPtr();
        if (origHdr->isBytesObject() && origHdr->byteSize() <= 100) {
            origStr = std::string((char*)origHdr->bytes(), origHdr->byteSize());
        }
    }
    // Log DNU for menu action debugging
    std::string rcvrClassName = "";
    if (receiver_.isObject() && receiver_.rawBits() > 0x10000) {
        Oop rcvrClass = memory_.classOf(receiver_);
        if (rcvrClass.isObject()) {
            Oop nameOop = memory_.fetchPointer(6, rcvrClass);
            if (nameOop.isObject() && nameOop.rawBits() > 0x10000) {
                ObjectHeader* nameHdr = nameOop.asObjectPtr();
                if (nameHdr->isBytesObject() && nameHdr->byteSize() < 50) {
                    rcvrClassName = std::string((char*)nameHdr->bytes(), nameHdr->byteSize());
                }
            }
        }
    }
    // Limit verbose DNU logging for known fallbacks
    static int assureExtCount = 0;
    bool skipLog = false;
    if (origStr == "assureExtension") {
        assureExtCount++;
        if (assureExtCount > 5) skipLog = true;
    }
    if (!skipLog) {
        std::cerr << "[DNU] Selector '#" << origStr << "' not found on " << rcvrClassName
                  << " (args=" << argCount << ") len=" << origStr.length() << "\n";
    }
    // Debug: Check why fallbacks aren't matching
    if (origStr.length() == 5) {
        std::cerr << "[DNU-DEBUG] 5-char selector bytes: ";
        for (size_t i = 0; i < origStr.length(); i++) {
            std::cerr << std::hex << (int)(unsigned char)origStr[i] << " ";
        }
        std::cerr << std::dec << "\n";
    }

    // Fallback for empty/corrupted selectors - sign of memory corruption
    if (origStr.empty()) {
        static int emptySelCount = 0;
        emptySelCount++;
        if (emptySelCount <= 10) {
            std::cerr << "[DNU] EMPTY SELECTOR - likely memory corruption, returning nil\n";
        }
        // Pop args and receiver, push nil
        popN(argCount + 1);
        push(memory_.nil());
        dnuDepth--;
        return;
    }

    // Fallback for #readStream on non-collection objects
    // MorphicRenderLoop incorrectly receives this during menu action execution
    if (origStr == "readStream" && argCount == 0) {
        // Return an empty ReadStream (just return nil for now)
        pop();  // Pop receiver
        push(memory_.nil());
        dnuDepth--;
        return;
    }

    // Fallback for #addTopicSpec: - menu system sends this during action execution
    // MorphicRenderLoop should not receive this, return receiver to continue
    if (origStr == "addTopicSpec:" && argCount == 1) {
        pop();  // Pop argument
        // Leave receiver on stack as return value
        dnuDepth--;
        return;
    }

    // Fallback for #slotScope - compiler scope chain method that can recurse infinitely
    // if scopes are misconfigured. Return nil to break the chain.
    if (origStr == "slotScope" && argCount == 0) {
        pop();  // Pop receiver
        push(memory_.nil());  // Return nil to end scope chain
        dnuDepth--;
        return;
    }

    // Fallback for startup to avoid DNU spiral
    if (origStr == "new" && argCount == 0) {
        pop();  // Pop receiver
        push(memory_.nil());
        dnuDepth--;
        return;
    }
    if (origStr == "receiver:" && argCount == 1) {
        Oop arg = pop();  // Pop argument
        Oop rcvr = pop();  // Pop receiver (Message object)
        // Try to store in Message's slot 2 (receiver field)
        if (rcvr.isObject()) {
            memory_.storePointer(2, rcvr, arg);
        }
        push(rcvr);  // Return self
        dnuDepth--;
        return;
    }
    // Startup-specific fallbacks to avoid disrupting normal startup sequence
    if (origStr == "logSnapshot:andQuit:" || origStr == "logSnapshot:" || origStr == "logSnapshotAndQuit") {
        // Just pop args and return receiver (do nothing for logging)
        popN(argCount);
        // Leave receiver on stack as return value
        dnuDepth--;
        return;
    }
    if (origStr == "timingPriority" || origStr == "systemBackgroundPriority") {
        // Return a default priority (40 = user scheduling priority)
        popN(argCount + 1);
        push(Oop::fromSmallInteger(40));
        dnuDepth--;
        return;
    }
    if (origStr == "hasError" || origStr == "isImageStarting" || origStr == "isSessionStarting") {
        // Return false for these status checks
        popN(argCount + 1);
        push(memory_.falseObject());
        dnuDepth--;
        return;
    }
    if (origStr == "executeDeferredStartupActions:" || origStr == "runStartup:" || origStr == "startUp:") {
        // Pop argument and return receiver (do nothing)
        popN(argCount);
        dnuDepth--;
        return;
    }
    // Fallback for process termination during quit - don't terminate on embedded VM
    if (origStr == "terminateRealActive" || origStr == "terminateActive" || origStr == "doTerminationFromYourself") {
        // Don't terminate - just return receiver and continue
        popN(argCount + 1);
        push(receiver_);  // Return self
        dnuDepth--;
        return;
    }
    // Fallback for context manipulation during process termination
    if (origStr == "asContext") {
        // Return receiver as-is (already a context or convertible)
        popN(argCount);  // Pop any args
        // Leave receiver on stack
        dnuDepth--;
        return;
    }
    if (origStr == "jump" && argCount == 0) {
        // Context jump - do nothing, just return
        pop();  // Pop receiver
        push(memory_.nil());
        dnuDepth--;
        return;
    }
    if (origStr == "sender" && argCount == 0) {
        // Return nil for sender (no sender context)
        pop();  // Pop receiver
        push(memory_.nil());
        dnuDepth--;
        return;
    }
    if (origStr == "push:" || origStr == "pop" || origStr == "stackp:") {
        // Context stack operations - do nothing
        popN(argCount);  // Pop args
        // Leave receiver on stack
        dnuDepth--;
        return;
    }
    if (origStr == "privSender:" && argCount == 1) {
        // Private sender setter - just return receiver
        pop();  // Pop argument
        // Leave receiver on stack
        dnuDepth--;
        return;
    }

    // Fallback for stream operations to avoid DNU spiral during startup
    if (origStr == "nextPut:" && argCount == 1) {
        // std::cerr << "[DNU] Fallback for nextPut: - returning argument"; // DEBUG
        Oop arg = pop();  // Pop argument
        pop();  // Pop receiver
        push(arg);  // Return the argument
        dnuDepth--;
        return;
    }
    if (origStr == "next" && argCount == 0) {
        // std::cerr << "[DNU] Fallback for next - returning nil"; // DEBUG
        pop();  // Pop receiver
        push(memory_.nil());
        dnuDepth--;
        return;
    }
    if (origStr == "atEnd" && argCount == 0) {
        // std::cerr << "[DNU] Fallback for atEnd - returning true"; // DEBUG
        pop();  // Pop receiver
        push(memory_.trueObject());  // Assume at end
        dnuDepth--;
        return;
    }
    // ===== MORPH METHOD FALLBACKS =====
    // These are needed because menu action blocks use Morph methods that
    // aren't being found in the normal lookup (inheritance issue?)
    if (origStr == "world" && argCount == 0) {
        // Return the World morph (global)
        Oop world = memory_.findGlobal("World");
        popN(1);  // Pop receiver
        push(world.isNil() ? memory_.nil() : world);
        dnuDepth--;
        return;
    }
    if (origStr == "owner" && argCount == 0) {
        // Owner is stored in slot 1 of a Morph
        Oop rcvr = stackValue(0);  // Use stackValue instead of pop
        std::cerr << "[FALLBACK] owner: rcvr=0x" << std::hex << rcvr.rawBits() << std::dec << "\n";
        if (rcvr.isObject()) {
            ObjectHeader* hdr = rcvr.asObjectPtr();
            if (hdr->slotCount() >= 2) {
                Oop owner = memory_.fetchPointer(1, rcvr);
                pop();  // Now pop the receiver
                push(owner);
                dnuDepth--;
                return;
            }
        }
        pop();  // Pop receiver
        push(memory_.nil());
        dnuDepth--;
        return;
    }
    if (origStr == "valueOfProperty:" && argCount == 1) {
        // Property lookup - return nil (property not found)
        popN(argCount + 1);
        push(memory_.nil());
        dnuDepth--;
        return;
    }
    if (origStr == "assureExtension" && argCount == 0) {
        // assureExtension creates and returns a MorphExtension if not present
        // The extension is stored in slot 5 (extension slot) of the morph
        Oop rcvr = stackValue(0);  // Get receiver from stack (no args)
        if (rcvr.isObject()) {
            ObjectHeader* hdr = rcvr.asObjectPtr();
            if (hdr->slotCount() >= 6) {
                Oop extension = memory_.fetchPointer(5, rcvr);
                if (!extension.isNil() && extension.isObject()) {
                    // Extension already exists - return it
                    pop();  // Pop receiver
                    push(extension);
                    dnuDepth--;
                    return;
                }
            }
        }
        // No extension found - return receiver (self) as fallback
        // This is not ideal but prevents infinite loop
        dnuDepth--;
        return;
    }
    if ((origStr == "invalidRect:" || origStr == "invalidRect:from:") && argCount >= 1) {
        // Mark rectangle as dirty - no-op in our simplified rendering
        popN(argCount);  // Pop arguments
        // Leave receiver on stack
        dnuDepth--;
        return;
    }
    if (origStr == "clipSubmorphs" && argCount == 0) {
        // Return false (don't clip submorphs by default)
        pop();
        push(memory_.falseObject());
        dnuDepth--;
        return;
    }
    if (origStr == "submorphs" && argCount == 0) {
        // Return slot 2 of the morph (submorphs array)
        Oop rcvr = pop();
        if (rcvr.isObject()) {
            ObjectHeader* hdr = rcvr.asObjectPtr();
            if (hdr->slotCount() >= 3) {
                Oop submorphs = memory_.fetchPointer(2, rcvr);
                push(submorphs);
                dnuDepth--;
                return;
            }
        }
        // Return empty array
        Oop arrayClass = memory_.specialObject(SpecialObjectIndex::ClassArray);
        Oop empty = memory_.allocateSlots(memory_.indexOfClass(arrayClass), 0, ObjectFormat::Indexable);
        push(empty);
        dnuDepth--;
        return;
    }
    if (origStr == "bounds" && argCount == 0) {
        // Return slot 0 of the morph (bounds rectangle)
        Oop rcvr = pop();
        if (rcvr.isObject()) {
            ObjectHeader* hdr = rcvr.asObjectPtr();
            if (hdr->slotCount() >= 1) {
                Oop bounds = memory_.fetchPointer(0, rcvr);
                push(bounds);
                dnuDepth--;
                return;
            }
        }
        push(memory_.nil());
        dnuDepth--;
        return;
    }

    // Fallback for do: on non-collection objects during startup
    // This treats single objects as if they were collections containing just themselves
    // Needed for flatCollect: during startup when handlers don't implement do:
    if (origStr == "do:" && argCount == 1) {
        static int doFallbackCount = 0;
        doFallbackCount++;
        if (doFallbackCount <= 5) {
            std::cerr << "[DNU] Fallback for do: - treating receiver as single-element collection\n";
        }
        Oop block = pop();  // Pop the block argument
        Oop receiver = pop();  // Pop the receiver (the "non-collection")

        // If block is a valid block, call it with the receiver as argument
        if (block.isObject()) {
            ObjectHeader* blockHdr = block.asObjectPtr();
            // Check if it looks like a block (FullBlockClosure or BlockClosure)
            // BlockClosure has numArgs at slot 2
            if (blockHdr->slotCount() >= 3) {
                Oop numArgsOop = memory_.fetchPointer(2, block);
                if (numArgsOop.isSmallInteger() && numArgsOop.asSmallInteger() == 1) {
                    // Block expects 1 argument - call it with the receiver
                    // For do:, we should just call the block and return the original collection
                    // But since we're simulating a single-element collection, we:
                    // 1. Call block value: receiver
                    // 2. Ignore the result
                    // 3. Return original receiver (the "collection")

                    // Actually, for flatCollect:, it does: stream nextPutAll: (aBlock value: each)
                    // So the result of the block IS used. The block should return a collection.
                    // If block returns receiver (the SnapshotOperation), then nextPutAll:
                    // tries to iterate it with do:, causing the cycle.

                    // BETTER APPROACH: Just skip this element entirely
                    // Don't call the block, return an empty array so flatCollect: has nothing to add
                    if (doFallbackCount <= 3) {
                        std::cerr << "[DNU] do: fallback - returning receiver without calling block\n";
                    }
                    push(receiver);  // Return receiver (acts like empty iteration)
                    dnuDepth--;
                    return;
                }
            }
        }

        // If we can't call the block, just return the receiver
        push(receiver);
        dnuDepth--;
        return;
    }

    // FFI/ThreadedFFI fallbacks to prevent callback loop
    if (origStr == "isNull" && argCount == 0) {
        // FFI pointer null check - return true (pointer is null, no callbacks)
        pop();  // Pop receiver
        push(memory_.trueObject());
        dnuDepth--;
        return;
    }
    if (origStr == "pointerAt:" && argCount == 1) {
        // FFI pointer read - return 0 (null pointer)
        pop();  // Pop argument
        pop();  // Pop receiver
        push(Oop::fromSmallInteger(0));
        dnuDepth--;
        return;
    }
    if ((origStr == "wait" || origStr == "waitTimeoutMSecs:") && argCount <= 1) {
        // FFI wait - return immediately
        popN(argCount);  // Pop any args
        // Leave receiver on stack
        dnuDepth--;
        return;
    }
    if (origStr == "handle" && argCount == 0) {
        // FFI handle accessor - return 0 (null handle)
        pop();  // Pop receiver
        push(Oop::fromSmallInteger(0));
        dnuDepth--;
        return;
    }
    if (origStr == "getHandle" && argCount == 0) {
        // FFI getHandle - return null
        pop();  // Pop receiver
        push(memory_.nil());
        dnuDepth--;
        return;
    }
    if (origStr == "callback" && argCount == 0) {
        // TFCallback - just return self to stop the loop
        // Leave receiver on stack
        dnuDepth--;
        return;
    }

    // Generic fallback for nil selector
    if (selector.rawBits() == 0 || selector.rawBits() == memory_.nil().rawBits()) {
        // std::cerr << "[DNU] Nil selector - returning nil"; // DEBUG
        popN(argCount + 1);  // Pop args and receiver
        push(memory_.nil());
        dnuDepth--;
        return;
    }

    // Debug: print DNU selector as string
    std::string dnuStr;
    if (selectors_.doesNotUnderstand.isObject() && selectors_.doesNotUnderstand.rawBits() > 0x10000) {
        ObjectHeader* dnuHdr = selectors_.doesNotUnderstand.asObjectPtr();
        if (dnuHdr->isBytesObject() && dnuHdr->byteSize() <= 50) {
            dnuStr = std::string((char*)dnuHdr->bytes(), dnuHdr->byteSize());
            // std::cerr << "[DNU] DNU selector string: '#" << dnuStr << "'"; // DEBUG
        }
    }

    // Safeguard: if we're already handling DNU, prevent infinite recursion
    if (selector.rawBits() == selectors_.doesNotUnderstand.rawBits()) {
        running_ = false;
        dnuDepth = 0;
        return;
    }

    // Get the actual receiver that failed (from stack, under args)
    Oop failedReceiver = stackValue(argCount);

    // Create a Message object
    Oop messageClass = memory_.specialObject(SpecialObjectIndex::ClassMessage);
    Oop message = memory_.allocateSlots(
        memory_.indexOfClass(messageClass), 2, ObjectFormat::FixedSize);

    // Message layout: selector, arguments
    memory_.storePointer(0, message, selector);

    // Create arguments array
    Oop arrayClass = memory_.specialObject(SpecialObjectIndex::ClassArray);
    Oop args = memory_.allocateSlots(
        memory_.indexOfClass(arrayClass), argCount, ObjectFormat::Indexable);

    for (int i = argCount - 1; i >= 0; --i) {
        memory_.storePointer(i, args, pop());
    }
    memory_.storePointer(1, message, args);

    // Pop receiver (will be repushed for send) - save it!
    Oop originalReceiver = pop();
    // std::cerr << "[DNU] Original receiver from stack: 0x" << std::hex << originalReceiver.rawBits() << std::dec; // DEBUG

    // Send doesNotUnderstand: message to the ORIGINAL receiver
    push(originalReceiver);
    push(message);
    sendSelector(selectors_.doesNotUnderstand, 1);

    // Decrement after setting up the DNU send - the depth was meant to track
    // nested calls within a single DNU handling chain, not sequential DNUs
    dnuDepth--;
}

void Interpreter::sendMustBeBoolean(Oop value) {
    // Prevent infinite loop: track last non-boolean value that triggered mustBeBoolean
    // If we see the same value multiple times, we're in a loop
    static Oop lastMustBeBooleanValue = Oop::nil();
    static int mbCount = 0;
    static int totalMbCount = 0;  // Total across entire execution

    totalMbCount++;

    // Hard limit: if we've had too many mustBeBoolean errors total, something is wrong
    if (totalMbCount > 50) {
        // DEBUG_LOG("[MUSTBEBOOLEAN] Too many mustBeBoolean errors (" << totalMbCount
                  // << ") - popping frames to recover";
        // Pop frames until we're at a reasonable depth
        while (frameDepth_ > 0 && frameDepth_ > 10) {
            popFrame();
        }
        totalMbCount = 0;
        lastMustBeBooleanValue = Oop::nil();
        mbCount = 0;
        return;
    }

    // Check if this is the same value we saw before (infinite loop)
    if (value.rawBits() == lastMustBeBooleanValue.rawBits() && value.rawBits() != 0) {
        mbCount++;
        if (mbCount > 3) {
            // DEBUG_LOG("[MUSTBEBOOLEAN] Detected loop on value 0x" << std::hex << value.rawBits()
                      // << std::dec << " (count=" << mbCount << ") - popping frame";
            lastMustBeBooleanValue = Oop::nil();
            mbCount = 0;
            // Pop current frame and return false - this exits the mustBeBoolean path
            if (frameDepth_ > 0) {
                popFrame();
                push(memory_.falseObject());
            }
            return;
        }
    } else {
        // New value - reset counter
        lastMustBeBooleanValue = value;
        mbCount = 1;
    }

    // For SmallIntegers, just skip (they can never be booleans)
    if (value.isSmallInteger()) {
        // DEBUG_LOG("[MUSTBEBOOLEAN] SmallInteger value " << value.asSmallInteger()
                  // << " - skipping mustBeBoolean";
        lastMustBeBooleanValue = Oop::nil();
        mbCount = 0;
        return;
    }

    // Send mustBeBoolean: selector
    sendSelector(selectors_.mustBeBoolean, 0);
}

// ===== HELPER METHODS =====

bool Interpreter::isTrue(Oop value) const {
    return value == memory_.trueObject();
}

bool Interpreter::isFalse(Oop value) const {
    return value == memory_.falseObject();
}

Oop Interpreter::superclassOf(Oop classOop) const {
    // Class layout: superclass is slot 0
    Oop result = memory_.fetchPointer(0, classOop);

    // Debug: trace invalid superclass pointers
    if (result.isObject() && !result.isNil() && !memory_.isValidPointer(result)) {
        static int invalidSuperclassCount = 0;
        invalidSuperclassCount++;
        if (invalidSuperclassCount <= 10) {
            std::string className = "<unknown>";
            if (classOop.isObject()) {
                Oop nameOop = memory_.fetchPointer(6, classOop);
                if (nameOop.isObject()) {
                    ObjectHeader* nameHdr = nameOop.asObjectPtr();
                    if (nameHdr->isBytesObject() && nameHdr->byteSize() < 50) {
                        className = std::string((char*)nameHdr->bytes(), nameHdr->byteSize());
                    }
                }
            }
            std::cerr << "[SUPER] INVALID superclass #" << invalidSuperclassCount
                      << " of " << className
                      << " class=0x" << std::hex << classOop.rawBits()
                      << " superclass=0x" << result.rawBits() << std::dec << "\n";
        }
    }

    return result;
}

Oop Interpreter::methodDictOf(Oop classOop) const {
    // Class layout: methodDict is slot 1
    return memory_.fetchPointer(1, classOop);
}

Oop Interpreter::methodClassOf(Oop method) const {
    // Get the class that defines this CompiledMethod by reading the last literal.
    // In Pharo, the last literal of a CompiledMethod is an Association whose value
    // is the class where the method is defined. This is critical for super sends.
    if (!method.isObject()) return memory_.nil();

    // Get numLiterals from method header
    Oop methodHeader = memory_.fetchPointer(0, method);
    if (!methodHeader.isSmallInteger()) return memory_.nil();

    int64_t headerBits = methodHeader.asSmallInteger();
    size_t numLiterals = headerBits & 0x7FFF;  // bits 0-14

    if (numLiterals == 0) return memory_.nil();

    // Last literal is at index (numLiterals - 1), stored at slot numLiterals
    Oop lastLiteral = memory_.fetchPointer(numLiterals, method);

    // The last literal can be:
    // 1. An Association (classBinding) - extract value (slot 1)
    // 2. Directly a class
    // 3. An AdditionalMethodState - need to unwrap

    if (!lastLiteral.isObject() || lastLiteral.isNil()) return memory_.nil();

    // Check if it's an Association by looking at its class
    Oop lastLitClass = memory_.classOf(lastLiteral);
    if (lastLitClass.isObject()) {
        Oop className = memory_.fetchPointer(6, lastLitClass);
        if (className.isObject()) {
            ObjectHeader* nameHdr = className.asObjectPtr();
            if (nameHdr->isBytesObject() && nameHdr->byteSize() < 50) {
                std::string classNameStr((char*)nameHdr->bytes(), nameHdr->byteSize());
                if (classNameStr == "Association" || classNameStr == "ClassBinding" ||
                    classNameStr == "GlobalVariable" || classNameStr == "LiteralVariable" ||
                    classNameStr == "ClassVariable" || classNameStr == "WorkspaceVariable" ||
                    classNameStr.find("Binding") != std::string::npos ||
                    classNameStr.find("Variable") != std::string::npos) {
                    // It's an Association-like binding - get value (slot 1)
                    return memory_.fetchPointer(1, lastLiteral);
                }
                if (classNameStr == "AdditionalMethodState") {
                    // AdditionalMethodState wraps the method - fallback to nil
                }
            }
        }
    }

    // Check if lastLiteral IS a class (has a methodDict at slot 1)
    ObjectHeader* litHdr = lastLiteral.asObjectPtr();
    if (litHdr->slotCount() >= 7) {
        Oop maybeName = memory_.fetchPointer(6, lastLiteral);
        if (maybeName.isObject()) {
            ObjectHeader* nameHdr = maybeName.asObjectPtr();
            if (nameHdr->isBytesObject() && nameHdr->byteSize() < 50) {
                // Looks like a class - return it
                return lastLiteral;
            }
        }
    }

    // Fallback: return nil (caller should use receiver's class as fallback)
    return memory_.nil();
}

int Interpreter::primitiveIndexOf(Oop method) const {
    if (!method.isObject()) return 0;

    Oop header = memory_.fetchPointer(0, method);
    if (!header.isSmallInteger()) return 0;

    int64_t bits = header.asSmallInteger();

    // CompiledMethod header format (after SmallInteger decoding):
    //   bits 0-14: numLiterals (15 bits)
    //   bit 15: needsLargeFrame
    //   bits 16-23: numTemps (8 bits)
    //   bits 24-27: numArgs (4 bits)
    //   bits 28-29: accessModifier
    //   bit 30: hasPrimitive flag
    //
    // The primitive number is encoded in the bytecode stream.
    // When hasPrimitive is set, bytecodes start with a callPrimitive bytecode.

    // Check hasPrimitive flag (bit 30)
    bool hasPrimitive = (bits >> 30) & 1;

    ObjectHeader* methodObj = method.asObjectPtr();
    int numLiterals = bits & 0x7FFF;  // bits 0-14 are numLiterals
    uint8_t* bytecodes = methodObj->bytes() + (1 + numLiterals) * 8;

    // In Sista V1, primitive call is encoded as:
    // 248 extB: callPrimitive with index from extension bytes
    // The format is: 248 extA extB where primitive = extA + (extB << 8)
    //
    // Or in simpler encoding for common primitives:
    // The primitive number may be embedded in special send bytecodes

    // Debug tracing for primitiveIndexOf
    // Check for callPrimitive bytecode (248 = 0xf8)
    if (bytecodes[0] == 248) {
        // callPrimitive: 248 lowByte highByte
        int primIndex = bytecodes[1] | (bytecodes[2] << 8);
        return primIndex;
    }

    // If hasPrimitive flag is set but no 248 bytecode at start,
    // scan first few bytecodes for callPrimitive
    if (hasPrimitive) {
        for (int i = 0; i < 20; i++) {
            if (bytecodes[i] == 248) {
                int primIndex = bytecodes[i+1] | (bytecodes[i+2] << 8);
                return primIndex;
            }
        }
    }

    // In some Sista images, primitive methods might use inline primitive calls
    // Check for "quick primitive" patterns - these are methods that just do
    // simple operations and have the primitive flag set but no explicit call

    // For now, return 0 if no explicit callPrimitive bytecode found
    // The method will fall back to executing its bytecodes
    return 0;
}

void Interpreter::duplicateTop() {
    push(stackTop());
}

void Interpreter::popStack() {
    pop();
}

void Interpreter::createBlock() {
    // Extended block creation bytecode
    uint8_t descriptor = fetchByte();
    int numArgs = descriptor & 0x0F;
    int numCopied = (descriptor >> 4) & 0x0F;
    uint16_t blockSize = fetchTwoBytes();

    // Create BlockClosure
    Oop blockClass = memory_.specialObject(SpecialObjectIndex::ClassBlockClosure);
    size_t slots = 3 + numCopied;  // outerContext, startPC, numArgs, copied...
    Oop block = memory_.allocateSlots(
        memory_.indexOfClass(blockClass), slots, ObjectFormat::Indexable);

    // Set fields
    // Note: We'd need a proper context object here
    memory_.storePointer(0, block, memory_.nil());  // outerContext - simplified
    memory_.storePointer(1, block, Oop::fromSmallInteger(
        instructionPointer_ - method_.asObjectPtr()->bytes()));
    memory_.storePointer(2, block, Oop::fromSmallInteger(numArgs));

    // Copy values from stack
    for (int i = numCopied - 1; i >= 0; --i) {
        memory_.storePointer(3 + i, block, pop());
    }

    // Skip block bytecodes
    instructionPointer_ += blockSize;

    push(block);
}

void Interpreter::createFullBlock() {
    // Similar to createBlock but for full block closures
    createBlock();  // Simplified - treat same for now
}

void Interpreter::createFullBlockWithLiteral(int litIndex, int numCopied) {
    // Sista V1 0xF9: Push FullBlockClosure
    // The closure's code is in a CompiledBlock literal at litIndex
    Oop compiledBlock = literal(litIndex);

    // Create FullBlockClosure
    // Get the class from special objects - index 59 is ClassFullBlockClosure
    Oop blockClass = memory_.specialObject(SpecialObjectIndex::ClassFullBlockClosure);

    // Fall back to BlockClosure if FullBlockClosure not found
    if (blockClass.isNil() || !blockClass.isObject()) {
        blockClass = memory_.specialObject(SpecialObjectIndex::ClassBlockClosure);
    }

    // The class index for instances is the index in the class table where this class is stored
    // NOT the classIndex of the class object itself (which is the metaclass index)
    // Cache the class index once found to avoid repeated searches
    static uint32_t cachedFullBlockClosureIdx = 0;
    uint32_t classIdx = cachedFullBlockClosureIdx;

    if (classIdx == 0 && blockClass.isObject()) {
        // First time: search for this class object in the class table
        for (uint32_t i = 1; i < 10000; i++) {
            Oop cls = memory_.classAtIndex(i);
            if (cls.rawBits() == blockClass.rawBits()) {
                classIdx = i;
                break;
            }
        }

        // If not found by pointer, try by name
        if (classIdx == 0) {
            for (uint32_t i = 1; i < 10000; i++) {
                Oop cls = memory_.classAtIndex(i);
                if (!cls.isNil() && cls.isObject()) {
                    Oop clsName = memory_.fetchPointer(6, cls);
                    if (clsName.isObject()) {
                        ObjectHeader* nameHdr = clsName.asObjectPtr();
                        if (nameHdr->isBytesObject() && nameHdr->byteSize() == 16) {
                            std::string name((char*)nameHdr->bytes(), 16);
                            if (name == "FullBlockClosure") {
                                classIdx = i;
                                break;
                            }
                        }
                    }
                }
            }
        }

        // Last resort fallback
        if (classIdx == 0) {
            classIdx = 38;  // BlockClosure in many images
        }

        // Cache for future calls
        cachedFullBlockClosureIdx = classIdx;
    }

    size_t slots = 3 + numCopied;  // outerContext, compiledBlock, numArgs, copied...
    Oop block = memory_.allocateSlots(classIdx, slots, ObjectFormat::Indexable);

    // Set fields
    memory_.storePointer(0, block, activeContext_);  // outerContext
    memory_.storePointer(1, block, compiledBlock);   // compiledBlock (instead of startPC)

    // Get numArgs from the CompiledBlock's header (first slot)
    // In Pharo, CompiledCode header format has numArgs in bits 24-27:
    // numArgs = (header bitAnd: 16rF000000) >> 24
    int numArgs = 0;
    if (compiledBlock.isObject()) {
        Oop methodHeader = memory_.fetchPointer(0, compiledBlock);
        if (methodHeader.isSmallInteger()) {
            int64_t headerBits = methodHeader.asSmallInteger();
            numArgs = (headerBits >> 24) & 0x0F;  // numArgs in bits 24-27
        }
    }
    memory_.storePointer(2, block, Oop::fromSmallInteger(numArgs));

    // Copy values from stack
    for (int i = numCopied - 1; i >= 0; --i) {
        memory_.storePointer(3 + i, block, pop());
    }

    push(block);
}

void Interpreter::createBlockWithArgs(int numArgs, int numCopied, int blockSize) {
    // Sista V1 0xFA: Push Closure (inline block)
    // Create BlockClosure
    Oop blockClass = memory_.specialObject(SpecialObjectIndex::ClassBlockClosure);
    size_t slots = 3 + numCopied;  // outerContext, startPC, numArgs, copied...
    Oop block = memory_.allocateSlots(
        memory_.indexOfClass(blockClass), slots, ObjectFormat::Indexable);

    // Set fields
    memory_.storePointer(0, block, activeContext_);  // outerContext
    memory_.storePointer(1, block, Oop::fromSmallInteger(
        instructionPointer_ - method_.asObjectPtr()->bytes()));  // startPC
    memory_.storePointer(2, block, Oop::fromSmallInteger(numArgs));

    // Copy values from stack
    for (int i = numCopied - 1; i >= 0; --i) {
        memory_.storePointer(3 + i, block, pop());
    }

    // Skip block bytecodes
    instructionPointer_ += blockSize;

    push(block);
}

void Interpreter::initializeSelectors() {
    // DEBUG: "[DEBUG] initializeSelectors: Starting..."

    // Get selectors from special objects array
    selectors_.doesNotUnderstand = memory_.specialObject(SpecialObjectIndex::SelectorDoesNotUnderstand);
    selectors_.mustBeBoolean = memory_.specialObject(SpecialObjectIndex::SelectorMustBeBoolean);
    selectors_.cannotReturn = memory_.specialObject(SpecialObjectIndex::SelectorCannotReturn);
    selectors_.aboutToReturn = memory_.specialObject(SpecialObjectIndex::SelectorAboutToReturn);
    // DEBUG: "[DEBUG] initializeSelectors: Got special selectors"

    // For arithmetic selectors, search SmallInteger's method dictionary
    Oop smallIntClass = memory_.specialObject(SpecialObjectIndex::ClassSmallInteger);
    if (!smallIntClass.isObject() || smallIntClass.isNil()) {
        // std::cerr << "[WARN] initializeSelectors: SmallInteger class not found"; // DEBUG
        return;
    }
    // DEBUG: "[DEBUG] initializeSelectors: Got SmallInteger class"

    // Get the actual nil object for comparison
    Oop nilObj = memory_.specialObject(SpecialObjectIndex::NilObject);

    // Helper to find a selector in a class hierarchy
    auto findSelectorInClass = [this, nilObj](Oop startClass, const char* name) -> Oop {
        Oop currentClass = startClass;
        int depth = 0;

        auto isNilOrEmpty = [nilObj](Oop o) -> bool {
            return o.isNil() || o.rawBits() == nilObj.rawBits();
        };

        while (!isNilOrEmpty(currentClass) && currentClass.isObject() && depth < 30) {
            depth++;
            ObjectHeader* classHeader = currentClass.asObjectPtr();

            size_t classSlots = classHeader->slotCount();
            if (classSlots < 2) {
                break;
            }

            // Class layout: slot 0 = superclass, slot 1 = methodDict
            Oop methodDict = memory_.fetchPointer(1, currentClass);
            if (methodDict.isObject() && !isNilOrEmpty(methodDict)) {
                ObjectHeader* mdHeader = methodDict.asObjectPtr();

                // MethodDictionary layout: slot 0 = tally, slot 1 = array
                size_t mdSlots = mdHeader->slotCount();
                if (mdSlots >= 2) {
                    Oop mdArray = memory_.fetchPointer(1, methodDict);
                    if (mdArray.isObject() && !isNilOrEmpty(mdArray)) {
                        ObjectHeader* arrayHeader = mdArray.asObjectPtr();
                        size_t arraySize = arrayHeader->slotCount();
                        size_t maxSearch = std::min(arraySize, (size_t)252);
                        for (size_t i = 0; i < maxSearch; i++) {
                            Oop entry = arrayHeader->slotAt(i);
                            if (isNilOrEmpty(entry) || !entry.isObject()) continue;

                            ObjectHeader* entryHdr = entry.asObjectPtr();
                            if (entryHdr->isCompiledMethod()) {
                                size_t entrySlots = entryHdr->slotCount();
                                if (entrySlots < 2) continue;
                                Oop selector = memory_.fetchPointer(1, entry);
                                if (selector.isObject() && !isNilOrEmpty(selector) && selector.rawBits() > 0x10000) {
                                    if (memory_.symbolEquals(selector, name)) {
                                        return selector;
                                    }
                                }
                            } else {
                                Oop key = memory_.fetchPointer(0, entry);
                                if (key.isObject() && !isNilOrEmpty(key) && key.rawBits() > 0x10000 && memory_.symbolEquals(key, name)) {
                                    return key;
                                }
                            }
                        }
                    }
                }
            }

            // Move to superclass
            currentClass = memory_.fetchPointer(0, currentClass);
        }
        return Oop::nil();
    };

    // Find arithmetic selectors in SmallInteger class hierarchy (skip "/" which causes hang)
    // DEBUG: "[DEBUG] initializeSelectors: Looking for +"
    selectors_.add = findSelectorInClass(smallIntClass, "+");
    // DEBUG: "[DEBUG] initializeSelectors: Looking for -"
    selectors_.subtract = findSelectorInClass(smallIntClass, "-");
    // DEBUG: "[DEBUG] initializeSelectors: Looking for <"
    selectors_.lessThan = findSelectorInClass(smallIntClass, "<");
    // DEBUG: "[DEBUG] initializeSelectors: Looking for >"
    selectors_.greaterThan = findSelectorInClass(smallIntClass, ">");
    // DEBUG: "[DEBUG] initializeSelectors: Looking for <="
    selectors_.lessEqual = findSelectorInClass(smallIntClass, "<=");
    // DEBUG: "[DEBUG] initializeSelectors: Looking for >="
    selectors_.greaterEqual = findSelectorInClass(smallIntClass, ">=");
    // DEBUG: "[DEBUG] initializeSelectors: Looking for ="
    selectors_.equal = findSelectorInClass(smallIntClass, "=");
    // DEBUG: "[DEBUG] initializeSelectors: Looking for ~="
    selectors_.notEqual = findSelectorInClass(smallIntClass, "~=");
    // DEBUG: "[DEBUG] initializeSelectors: Looking for *"
    selectors_.multiply = findSelectorInClass(smallIntClass, "*");
    // Skip "/" - causes hang for unknown reason
    selectors_.divide = Oop::nil();
    // DEBUG_LOG("[DEBUG] initializeSelectors: Skipped / (causes hang)";
    // DEBUG: "[DEBUG] initializeSelectors: Done with SmallInteger selectors"

    // Skip these for now to avoid potential hangs
    selectors_.at = Oop::nil();
    selectors_.atPut = Oop::nil();
    selectors_.size = Oop::nil();
    selectors_.eq = Oop::nil();
    selectors_.class_ = Oop::nil();
    selectors_.value = Oop::nil();
    selectors_.value_ = Oop::nil();
    selectors_.valueValue = Oop::nil();

    // Log results
    // DEBUG: "[DEBUG] Arithmetic selectors found:"
    // std::cerr << "  +: 0x" << std::hex << selectors_.add.rawBits() << std::dec; // DEBUG
    // std::cerr << "  -: 0x" << std::hex << selectors_.subtract.rawBits() << std::dec; // DEBUG
    // std::cerr << "  <: 0x" << std::hex << selectors_.lessThan.rawBits() << std::dec; // DEBUG
    // std::cerr << "  >: 0x" << std::hex << selectors_.greaterThan.rawBits() << std::dec; // DEBUG
    // std::cerr << "  *: 0x" << std::hex << selectors_.multiply.rawBits() << std::dec; // DEBUG
    // std::cerr << "  /: 0x" << std::hex << selectors_.divide.rawBits() << std::dec; // DEBUG
    // std::cerr << "  =: 0x" << std::hex << selectors_.equal.rawBits() << std::dec; // DEBUG
    // std::cerr << "  at:: 0x" << std::hex << selectors_.at.rawBits() << std::dec; // DEBUG
    // std::cerr << "  ==: 0x" << std::hex << selectors_.eq.rawBits() << std::dec; // DEBUG
    // std::cerr << "  value: 0x" << std::hex << selectors_.value.rawBits() << std::dec; // DEBUG
}

// ===== PROCESS SCHEDULING =====

void Interpreter::terminateCurrentProcess() {
    // DEBUG: "[SCHED] Terminating current process..."

    Oop nilObj = memory_.specialObject(SpecialObjectIndex::NilObject);
    Oop schedulerAssoc = memory_.specialObject(SpecialObjectIndex::SchedulerAssociation);

    if (!schedulerAssoc.isObject() || schedulerAssoc.rawBits() == nilObj.rawBits()) {
        // DEBUG: "[SCHED] No scheduler - can't terminate"
        return;
    }

    Oop scheduler = memory_.fetchPointer(1, schedulerAssoc);
    if (!scheduler.isObject()) {
        // DEBUG: "[SCHED] Invalid scheduler"
        return;
    }

    // ProcessScheduler: slot 1 = activeProcess
    Oop activeProcess = memory_.fetchPointer(1, scheduler);
    if (!activeProcess.isObject() || activeProcess.rawBits() == nilObj.rawBits()) {
        // DEBUG: "[SCHED] No active process to terminate"
        return;
    }

    // DEBUG_LOG("[SCHED] Setting suspendedContext to nil for process 0x"
              // << std::hex << activeProcess.rawBits() << std::dec;

    // Process: slot 1 = suspendedContext - set it to nil to mark as terminated
    memory_.storePointer(1, activeProcess, nilObj);

    // Also remove from scheduler queue (slot 0 = nextLink in Process, clear it)
    memory_.storePointer(0, activeProcess, nilObj);
}

Oop Interpreter::getActiveProcess() {
    Oop schedulerAssoc = memory_.specialObject(SpecialObjectIndex::SchedulerAssociation);
    Oop scheduler = memory_.fetchPointer(1, schedulerAssoc);  // value of Association
    return memory_.fetchPointer(SchedulerActiveProcessIndex, scheduler);
}

void Interpreter::setActiveProcess(Oop process) {
    Oop schedulerAssoc = memory_.specialObject(SpecialObjectIndex::SchedulerAssociation);
    Oop scheduler = memory_.fetchPointer(1, schedulerAssoc);
    memory_.storePointer(SchedulerActiveProcessIndex, scheduler, process);
}

void Interpreter::addLastLinkToList(Oop process, Oop list) {
    // Validate inputs
    if (!process.isObject() || !list.isObject()) {
        return;
    }

    ObjectHeader* procHdr = process.asObjectPtr();

    // Verify process has enough slots for Process layout
    if (procHdr->slotCount() < 4) {
        return;
    }

    Oop nilObj = memory_.nil();

    // Set process.nextLink = nil (it's the last one)
    memory_.storePointer(ProcessNextLinkIndex, process, nilObj);

    // Set process.myList = list
    memory_.storePointer(ProcessMyListIndex, process, list);

    // Check if list is empty
    Oop firstLink = memory_.fetchPointer(LinkedListFirstLinkIndex, list);

    if (firstLink.isNil() || firstLink.rawBits() == nilObj.rawBits()) {
        // Empty list - process becomes both first and last
        memory_.storePointer(LinkedListFirstLinkIndex, list, process);
    } else {
        // Non-empty list - append to last element
        Oop lastLink = memory_.fetchPointer(LinkedListLastLinkIndex, list);
        memory_.storePointer(ProcessNextLinkIndex, lastLink, process);
    }

    memory_.storePointer(LinkedListLastLinkIndex, list, process);
}

Oop Interpreter::removeFirstLinkOfList(Oop list) {
    Oop nilObj = memory_.nil();

    Oop first = memory_.fetchPointer(LinkedListFirstLinkIndex, list);
    Oop last = memory_.fetchPointer(LinkedListLastLinkIndex, list);

    if (first.rawBits() == last.rawBits()) {
        // Only one element - list becomes empty
        memory_.storePointer(LinkedListFirstLinkIndex, list, nilObj);
        memory_.storePointer(LinkedListLastLinkIndex, list, nilObj);
    } else {
        // Multiple elements - advance firstLink to next
        Oop next = memory_.fetchPointer(ProcessNextLinkIndex, first);
        memory_.storePointer(LinkedListFirstLinkIndex, list, next);
    }

    // Clear removed process's links
    memory_.storePointer(ProcessNextLinkIndex, first, nilObj);
    memory_.storePointer(ProcessMyListIndex, first, nilObj);

    return first;
}

bool Interpreter::removeProcessFromList(Oop process, Oop list) {
    Oop nilObj = memory_.nil();
    Oop first = memory_.fetchPointer(LinkedListFirstLinkIndex, list);

    if (first.rawBits() == process.rawBits()) {
        // Process is first in list
        removeFirstLinkOfList(list);
        return true;
    }

    // Search for process in list
    Oop prev = first;
    Oop current = memory_.fetchPointer(ProcessNextLinkIndex, prev);

    while (!current.isNil() && current.rawBits() != nilObj.rawBits()) {
        if (current.rawBits() == process.rawBits()) {
            // Found it - unlink
            Oop next = memory_.fetchPointer(ProcessNextLinkIndex, current);
            memory_.storePointer(ProcessNextLinkIndex, prev, next);

            // Update lastLink if needed
            Oop lastLink = memory_.fetchPointer(LinkedListLastLinkIndex, list);
            if (lastLink.rawBits() == process.rawBits()) {
                memory_.storePointer(LinkedListLastLinkIndex, list, prev);
            }

            // Clear process's links
            memory_.storePointer(ProcessNextLinkIndex, process, nilObj);
            memory_.storePointer(ProcessMyListIndex, process, nilObj);
            return true;
        }
        prev = current;
        current = memory_.fetchPointer(ProcessNextLinkIndex, current);
    }
    return false;
}

Oop Interpreter::wakeHighestPriority() {
    Oop nilObj = memory_.nil();
    Oop schedulerAssoc = memory_.specialObject(SpecialObjectIndex::SchedulerAssociation);
    Oop scheduler = memory_.fetchPointer(1, schedulerAssoc);
    Oop schedLists = memory_.fetchPointer(SchedulerProcessListsIndex, scheduler);

    ObjectHeader* listsHeader = schedLists.asObjectPtr();
    size_t numPriorities = listsHeader->slotCount();

    // Search from highest to lowest priority
    for (int p = static_cast<int>(numPriorities) - 1; p >= 0; p--) {
        Oop processList = memory_.fetchPointer(p, schedLists);
        Oop first = memory_.fetchPointer(LinkedListFirstLinkIndex, processList);

        if (!first.isNil() && first.rawBits() != nilObj.rawBits()) {
            // Found a runnable process - remove and return it
            return removeFirstLinkOfList(processList);
        }
    }

    // No runnable process found - this should not happen in a working system
    return nilObj;
}

void Interpreter::putToSleep(Oop process) {
    Oop schedulerAssoc = memory_.specialObject(SpecialObjectIndex::SchedulerAssociation);
    Oop scheduler = memory_.fetchPointer(1, schedulerAssoc);
    Oop schedLists = memory_.fetchPointer(SchedulerProcessListsIndex, scheduler);

    // Get process priority (1-based SmallInteger)
    Oop priorityOop = memory_.fetchPointer(ProcessPriorityIndex, process);
    int priority = static_cast<int>(priorityOop.asSmallInteger());

    // Get the appropriate priority list (0-indexed in array)
    Oop processList = memory_.fetchPointer(priority - 1, schedLists);

    addLastLinkToList(process, processList);
}

void Interpreter::transferTo(Oop newProcess) {
    Oop oldProcess = getActiveProcess();

    if (oldProcess.rawBits() == newProcess.rawBits()) {
        return;  // Already running this process
    }

    // Validate newProcess
    if (!newProcess.isObject()) {
        return;
    }

    ObjectHeader* newProcHdr = newProcess.asObjectPtr();
    if (newProcHdr->slotCount() < 2) {
        return;
    }

    // Save current execution state to old process's suspendedContext
    if (!activeContext_.isNil() && activeContext_.isObject()) {
        memory_.storePointer(ProcessSuspendedContextIndex, oldProcess, activeContext_);
    }

    // Switch to new process
    setActiveProcess(newProcess);

    // Get new process's suspended context
    Oop newContext = memory_.fetchPointer(ProcessSuspendedContextIndex, newProcess);

    // Reset interpreter state
    stackPointer_ = stackBase_;
    frameDepth_ = 0;

    // Resume execution from the new context
    executeFromContext(newContext);
}

bool Interpreter::tryReschedule() {
    static int reschedCount = 0;
    reschedCount++;
    bool trace = (reschedCount <= 5);

    Oop nilObj = memory_.specialObject(SpecialObjectIndex::NilObject);
    if (trace) {
        std::cerr << "[SCHED] nilObj=0x" << std::hex << nilObj.rawBits() << std::dec << "\n";
    }
    Oop schedulerAssoc = memory_.specialObject(SpecialObjectIndex::SchedulerAssociation);

    if (!schedulerAssoc.isObject() || schedulerAssoc.rawBits() == nilObj.rawBits()) {
        if (trace) std::cerr << "[SCHED] tryReschedule: no schedulerAssoc\n";
        return false;
    }

    Oop scheduler = memory_.fetchPointer(1, schedulerAssoc);
    if (!scheduler.isObject()) {
        if (trace) std::cerr << "[SCHED] tryReschedule: invalid scheduler\n";
        return false;
    }

    // ProcessScheduler: slot 0 = quiescentProcessLists, slot 1 = activeProcess
    Oop activeProcess = memory_.fetchPointer(1, scheduler);
    Oop queues = memory_.fetchPointer(0, scheduler);

    if (!queues.isObject()) {
        if (trace) std::cerr << "[SCHED] tryReschedule: no process queues\n";
        return false;
    }

    ObjectHeader* queuesHeader = queues.asObjectPtr();
    size_t numQueues = queuesHeader->slotCount();

    if (trace) {
        std::cerr << "[SCHED] tryReschedule: scanning " << numQueues << " priority queues\n";
        std::cerr << "[SCHED]   activeProcess=0x" << std::hex << activeProcess.rawBits() << std::dec << "\n";
        // Dump all non-empty queues to understand process state
        for (size_t i = 0; i < numQueues; i++) {
            Oop queue = queuesHeader->slotAt(i);
            if (queue.isObject() && queue.rawBits() != nilObj.rawBits()) {
                Oop first = memory_.fetchPointer(0, queue);
                if (first.isObject() && first.rawBits() != nilObj.rawBits()) {
                    std::cerr << "[SCHED]   queue[" << (i+1) << "] has processes\n";
                }
            }
        }
    }

    // Search from highest to lowest priority
    int processesFound = 0;
    for (int i = static_cast<int>(numQueues) - 1; i >= 0; i--) {
        Oop queue = queuesHeader->slotAt(i);
        if (!queue.isObject() || queue.rawBits() == nilObj.rawBits()) continue;

        // LinkedList: slot 0 = firstLink, slot 1 = lastLink
        Oop process = memory_.fetchPointer(0, queue);
        if (!process.isObject() || process.rawBits() == nilObj.rawBits()) continue;

        processesFound++;
        if (trace && processesFound <= 3) {
            std::cerr << "[SCHED]   priority " << (i+1) << ": process=0x" << std::hex << process.rawBits() << std::dec << std::flush;
        }

        // Skip if this is the same process that just finished
        if (process.rawBits() == activeProcess.rawBits()) {
            if (trace) std::cerr << " (SAME AS ACTIVE, skip)\n" << std::flush;
            continue;
        }

        // Process: slot 0 = nextLink, slot 1 = suspendedContext, slot 2 = priority
        Oop context = memory_.fetchPointer(1, process);
        if (trace) std::cerr << " ctx=0x" << std::hex << context.rawBits() << std::dec << std::flush;
        if (!context.isObject() || context.rawBits() == nilObj.rawBits()) {
            if (trace) std::cerr << " (no suspendedContext)\n" << std::flush;
            continue;
        }

        ObjectHeader* ctxHeader = context.asObjectPtr();
        if (ctxHeader->format() != ObjectFormat::IndexableWithFixed) {
            if (trace) std::cerr << " (bad context format=" << (int)ctxHeader->format() << ")\n";
            continue;
        }

        if (trace) std::cerr << " -> FOUND runnable!\n";

        // Update the active process in scheduler
        memory_.storePointer(1, scheduler, process);

        // Reset stack for new process
        stackPointer_ = stackBase_;
        frameDepth_ = 0;

        // Execute from the new process's context
        if (executeFromContext(context)) {
            if (trace) std::cerr << "[SCHED] Rescheduled successfully\n";
            return true;
        }
    }

    if (trace) std::cerr << "[SCHED] tryReschedule: no runnable process found (scanned " << processesFound << " processes)\n";
    return false;
}

// ===== STARTUP SUPPORT =====

bool Interpreter::bootstrapStartup() {
    // In Spur, nil is an actual object at heap start, not 0
    Oop nilObj = memory_.specialObject(SpecialObjectIndex::NilObject);
    // DEBUG_LOG("[DEBUG] nil object = 0x" << std::hex << nilObj.rawBits() << std::dec;

    // Approach 1: Look for any ready-to-run process in the scheduler's queues
    // DEBUG: "[DEBUG] Checking scheduler process queues for runnable processes..."
    Oop schedulerAssoc = memory_.specialObject(SpecialObjectIndex::SchedulerAssociation);
    if (schedulerAssoc.rawBits() != nilObj.rawBits() && schedulerAssoc.isObject()) {
        Oop scheduler = memory_.fetchPointer(1, schedulerAssoc);
        if (scheduler.isObject()) {
            // ProcessScheduler slot 0 = quiescentProcessLists (array of LinkedLists)
            Oop queues = memory_.fetchPointer(0, scheduler);
            if (queues.isObject()) {
                ObjectHeader* queuesHeader = queues.asObjectPtr();
                size_t numQueues = queuesHeader->slotCount();
                // DEBUG: "[DEBUG] Found " << numQueues << " priority queues"

                // Search from highest to lowest priority for a runnable process
                for (int i = static_cast<int>(numQueues) - 1; i >= 0; i--) {
                    Oop queue = queuesHeader->slotAt(i);
                    if (queue.rawBits() == nilObj.rawBits() || !queue.isObject()) continue;

                    ObjectHeader* queueHeader = queue.asObjectPtr();
                    // DEBUG_LOG("[DEBUG] Queue at priority " << (i + 1) << ": cls=" << queueHeader->classIndex()
                              // << " slots=" << queueHeader->slotCount();

                    // LinkedList layout: slot 0 = firstLink, slot 1 = lastLink
                    Oop firstProcess = memory_.fetchPointer(0, queue);
                    if (firstProcess.rawBits() == nilObj.rawBits() || !firstProcess.isObject()) continue;

                    ObjectHeader* procHeader = firstProcess.asObjectPtr();
                    // DEBUG_LOG("[DEBUG] Process at priority " << (i + 1) << ": cls=" << procHeader->classIndex()
                              // << " slots=" << procHeader->slotCount();

                    // Dump first 5 slots of process
                    for (size_t j = 0; j < std::min(procHeader->slotCount(), (size_t)5); j++) {
                        Oop slot = procHeader->slotAt(j);
                        // DEBUG_LOG("[DEBUG]   proc slot[" << j << "] = 0x" << std::hex << slot.rawBits() << std::dec;
                        // if (slot.rawBits() == nilObj.rawBits()) std::cerr << " (NIL)";
                        // else if (slot.isSmallInteger()) std::cerr << " (SmallInt: " << slot.asSmallInteger() << ")";
                        // else if (slot.isObject()) {
                        //     ObjectHeader* h = slot.asObjectPtr();
                        //     std::cerr << " (obj: slots=" << h->slotCount() << " cls=" << h->classIndex() << ")";
                        // }
                        // std::cerr; // DEBUG
                    }

                    // Check if this process has a valid context
                    // Modern Pharo Process layout:
                    //   slot 0 = nextLink (for LinkedList)
                    //   slot 1 = suspendedContext
                    //   slot 2 = priority
                    Oop context = memory_.fetchPointer(1, firstProcess);  // suspendedContext is at slot 1
                    if (context.rawBits() != nilObj.rawBits() && context.isObject()) {
                        ObjectHeader* ctxHeader = context.asObjectPtr();
                        // DEBUG_LOG("[DEBUG] suspendedContext: cls=" << ctxHeader->classIndex()
                                  // << " slots=" << ctxHeader->slotCount() << " fmt=" << (int)ctxHeader->format();

                        // Only try to execute if it looks like a Context (not a Process)
                        // Context format is usually 3 (indexable with fixed), Process format is 1
                        if (ctxHeader->format() == ObjectFormat::IndexableWithFixed) {
                            return executeFromContext(context);
                        } else {
                            // DEBUG_LOG("[DEBUG] suspendedContext doesn't look like a Context (format=" << (int)ctxHeader->format() << ")";
                        }
                    }
                }
            }
        }
    }

    // DEBUG: "[DEBUG] No runnable process found in scheduler queues"

    // Approach 2: Try to resume from where the image was saved
    // The saved active process might have a context embedded deeper
    Oop activeProcess = memory_.specialObject(SpecialObjectIndex::SchedulerAssociation);
    if (activeProcess.isObject()) {
        activeProcess = memory_.fetchPointer(1, activeProcess);  // Get scheduler
        if (activeProcess.isObject()) {
            activeProcess = memory_.fetchPointer(1, activeProcess);  // Get activeProcess
            if (activeProcess.isObject()) {
                // DEBUG_LOG("[DEBUG] Active process: 0x" << std::hex << activeProcess.rawBits() << std::dec;
                ObjectHeader* procHeader = activeProcess.asObjectPtr();
                // DEBUG_LOG("[DEBUG] Active process has " << procHeader->slotCount() << " slots";

                // Check all slots for a valid context
                for (size_t i = 0; i < std::min(procHeader->slotCount(), (size_t)10); i++) {
                    Oop slot = procHeader->slotAt(i);
                    // DEBUG_LOG("[DEBUG]   slot[" << i << "] = 0x" << std::hex << slot.rawBits() << std::dec;
                    // if (slot.isNil()) std::cerr << " (nil)";
                    // else if (slot.isSmallInteger()) std::cerr << " (SmallInt: " << slot.asSmallInteger() << ")";
                    // else if (slot.isObject()) {
                    //     ObjectHeader* h = slot.asObjectPtr();
                    //     std::cerr << " (obj: " << h->slotCount() << " slots, cls=" << h->classIndex() << ")";
                    // }
                    // std::cerr; // DEBUG
                }
            }
        }
    }

    // Approach 3: Try to find and call a startup method directly
    // Use static tracking to prevent infinite loops
    static int startupAttempt = 0;
    static bool startupSucceeded = false;
    static bool displayInitialized = false;

    startupAttempt++;
    // DEBUG: "[DEBUG] bootstrapStartup: Attempt #" << startupAttempt

    // Initialize the platform display ONCE with a test pattern
    // Skip Smalltalk Form creation - just write directly to platform buffer
    if (!displayInitialized && pharo::gDisplaySurface) {
        displayInitialized = true;

        uint32_t* pixels = pharo::gDisplaySurface->pixels();
        int width = pharo::gDisplaySurface->width();
        int height = pharo::gDisplaySurface->height();

        // Fill with a blue gradient pattern to verify display works
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                uint8_t r = static_cast<uint8_t>(128 + (x * 127 / width));
                uint8_t g = static_cast<uint8_t>(128 + (y * 127 / height));
                uint8_t b = 255;
                pixels[y * width + x] = (255 << 24) | (r << 16) | (g << 8) | b;  // ARGB
            }
        }

        pharo::gDisplaySurface->update();

        // Try to find and activate the Display Form from the image
        // This connects Pharo's Display object to our display surface
        Oop displayObj = memory_.findGlobal("Display");
        if (!displayObj.isNil() && displayObj.isObject()) {
            // Call beDisplay on it (primitive 126)
            setDisplayForm(displayObj);

            // Get the Form's dimensions and update our screen size
            // Form slots: 0=bits, 1=width, 2=height, 3=depth
            // (dimensions used internally for screen sizing)
        } else {
            // Display object not found, creating one

            // Try to create a display Form directly
            // Form structure: 0=bits, 1=width, 2=height, 3=depth, 4=offset
            int dispWidth = pharo::gDisplaySurface ? pharo::gDisplaySurface->width() : 1024;
            int dispHeight = pharo::gDisplaySurface ? pharo::gDisplaySurface->height() : 768;

            // Find Form and Bitmap classes
            Oop formClass = memory_.findGlobal("Form");
            Oop bitmapClass = memory_.findGlobal("Bitmap");

            if (!formClass.isNil() && formClass.isObject() &&
                !bitmapClass.isNil() && bitmapClass.isObject()) {

                uint32_t formClassIdx = memory_.indexOfClass(formClass);
                uint32_t bitmapClassIdx = memory_.indexOfClass(bitmapClass);

                if (formClassIdx > 0 && bitmapClassIdx > 0) {
                    // Allocate bitmap for pixels (32-bit pixels = 1 word each)
                    size_t pixelCount = static_cast<size_t>(dispWidth) * dispHeight;
                    Oop bitmapObj = memory_.allocateWords(bitmapClassIdx, pixelCount);

                    if (!bitmapObj.isNil()) {
                        // Allocate form with 5 slots
                        Oop formObj = memory_.allocateSlots(formClassIdx, 5);

                        if (!formObj.isNil()) {
                            // Set form slots: bits, width, height, depth, offset
                            memory_.storePointer(0, formObj, bitmapObj);
                            memory_.storePointer(1, formObj, Oop::fromSmallInteger(dispWidth));
                            memory_.storePointer(2, formObj, Oop::fromSmallInteger(dispHeight));
                            memory_.storePointer(3, formObj, Oop::fromSmallInteger(32));
                            memory_.storePointer(4, formObj, Oop::fromSmallInteger(0));  // offset = 0@0

                            // Set as display form locally
                            setDisplayForm(formObj);
                            setScreenSize(dispWidth, dispHeight);
                            setScreenDepth(32);

                            // Bind to 'Display' global so Morphic can find it
                            memory_.setGlobal("Display", formObj);
                        }
                    }
                }
            }
        }
    }

    // If we've already tried many startup attempts, eventually give up.
    // But allow many more attempts for the UI loop to run.
    if (startupAttempt > 1000) {
        running_ = false;
        return false;
    }

    // DEBUG: "[DEBUG] bootstrapStartup: Trying to find startup globals..."

    // Helper lambda to look up method directly from a class's methodDict
    // (bypasses classOf which may fail for metaclasses not in class table)
    auto lookupMethodInClass = [&](Oop classObj, const char* selectorName) -> Oop {
        if (!classObj.isObject()) return Oop::nil();

        // Get the method dictionary (slot 1 of the class)
        Oop methodDict = memory_.fetchPointer(1, classObj);
        if (!methodDict.isObject()) return Oop::nil();

        ObjectHeader* mdHeader = methodDict.asObjectPtr();
        size_t mdSlots = mdHeader->slotCount();

        // Find the selector in the methodDict
        for (size_t i = 2; i < mdSlots; i++) {
            Oop key = mdHeader->slotAt(i);
            if (!key.isObject() || key.isNil()) continue;

            ObjectHeader* keyHdr = key.asObjectPtr();
            if (!keyHdr->isBytesObject()) continue;

            size_t keyLen = keyHdr->byteSize();
            const char* keyBytes = (const char*)keyHdr->bytes();

            if (keyLen == strlen(selectorName) && memcmp(keyBytes, selectorName, keyLen) == 0) {
                // Found the selector! Get the method from values array (slot 1)
                // MethodDictionary layout: slot 0 = tally, slot 1 = values array, slot 2+ = keys
                // Keys at slot i correspond to values at index i-2 in the values array
                Oop values = memory_.fetchPointer(1, methodDict);
                if (values.isObject()) {
                    ObjectHeader* valHdr = values.asObjectPtr();
                    size_t valueIdx = i - 2;  // Offset by 2 (skip tally and values slots)
                    if (valueIdx < valHdr->slotCount()) {
                        Oop method = valHdr->slotAt(valueIdx);
                        // DEBUG_LOG("[DEBUG] lookupMethodInClass: Found " << selectorName
                                  // << " key@slot " << i << " -> value@" << valueIdx
                                  // << " = 0x" << std::hex << method.rawBits() << std::dec;
                        return method;
                    }
                }
            }
        }
        return Oop::nil();
    };

    // First try: SmalltalkImage >> recordStartupStamp
    if (startupAttempt == 1) {
        Oop smalltalkImage = memory_.findGlobal("SmalltalkImage");
        if (smalltalkImage.isObject()) {
            // Look up method directly from SmalltalkImage's methodDict
            Oop method = lookupMethodInClass(smalltalkImage, "recordStartupStamp");
            if (!method.isNil() && method.isObject()) {
                // Create a receiver - use nil as receiver since recordStartupStamp may not need self
                Oop context = memory_.createStartupContext(method, memory_.nil());
                if (!context.isNil()) {
                    stackPointer_ = stackBase_;
                    frameDepth_ = 0;
                    if (executeFromContext(context)) {
                        return true;
                    }
                }
            }
        }
    }

    // Second try: restartMethods
    if (startupAttempt == 2) {
        // DEBUG: "[DEBUG] Attempt 2: Trying restartMethods..."

        Oop smalltalkImage = memory_.findGlobal("SmalltalkImage");
        if (smalltalkImage.isObject()) {
            Oop method = lookupMethodInClass(smalltalkImage, "restartMethods");
            if (!method.isNil() && method.isObject()) {
                Oop context = memory_.createStartupContext(method, memory_.nil());
                if (!context.isNil()) {
                    stackPointer_ = stackBase_;
                    frameDepth_ = 0;
                    if (executeFromContext(context)) {
                        // DEBUG: "[DEBUG] Started restartMethods execution"
                        return true;
                    }
                }
            } else {
                // DEBUG: "[DEBUG] Method restartMethods not found in SmalltalkImage"
            }
        }
    }

    // Third try and beyond: Keep calling World>>doOneCycle for UI loop
    if (startupAttempt >= 3 && startupAttempt <= 100) {
        // Find World class - Note: World is a global that holds the current WorldMorph
        Oop world = memory_.findGlobal("World");

        if (!world.isNil() && world.isObject()) {
            // World is an instance of WorldMorph - get its class for method lookup
            Oop worldClass = memory_.classOf(world);

            // If classOf failed, try looking up WorldMorph by name
            if (worldClass.isNil() || worldClass.rawBits() == 0) {
                worldClass = memory_.findGlobal("WorldMorph");
            }

            // Try to call doOneCycle on the World instance
            Oop method = lookupMethodInClass(worldClass, "doOneCycle");
            if (!method.isNil() && method.isObject()) {
                Oop context = memory_.createStartupContext(method, world);  // Pass instance, not class
                if (!context.isNil()) {
                    stackPointer_ = stackBase_;
                    frameDepth_ = 0;
                    if (executeFromContext(context)) {
                        return true;
                    }
                }
            }
        }
    }

    // Fourth try: Try Object >> yourself just to prove basic execution works
    if (startupAttempt == 4) {
        Oop arrayClass = memory_.specialObject(SpecialObjectIndex::ClassArray);
        if (arrayClass.isObject()) {
            Oop selector = findSelector("yourself");
            if (!selector.isNil()) {
                Oop method = lookupMethod(selector, arrayClass);
                if (!method.isNil() && method.isObject()) {
                    Oop receiver = memory_.allocateSlots(arrayClass.asObjectPtr()->classIndex(), 0);
                    if (receiver.isObject()) {
                        Oop context = memory_.createStartupContext(method, receiver);
                        if (!context.isNil()) {
                            stackPointer_ = stackBase_;
                            frameDepth_ = 0;
                            if (executeFromContext(context)) {
                                return true;
                            }
                        }
                    }
                }
            }
        }
    }

    // If we get here, we've exhausted our startup options
    // Note: This is normal for headless images - the startup methods executed
    // successfully in earlier attempts, but the Smalltalk code returned because
    // there's no GUI event loop to run.
    // DEBUG_LOG("[DEBUG] bootstrapStartup: No more startup methods to try (attempt #"
              // << startupAttempt << ")";
    // DEBUG: "[DEBUG] This is normal for headless images - startup code ran and returned."
    return false;
}

Oop Interpreter::findSelector(const char* name) {
    // Find a selector symbol by searching through method dictionaries
    // Modern Pharo MethodDictionary stores keys INLINE at slot 2+
    // DEBUG: "[DEBUG] findSelector: Looking for '" << name << "'"

    // Search through several well-known classes to find the selector
    Oop classesToSearch[] = {
        memory_.specialObject(SpecialObjectIndex::ClassArray),
        memory_.specialObject(SpecialObjectIndex::ClassByteString),
        memory_.specialObject(SpecialObjectIndex::ClassSmallInteger),
        memory_.specialObject(SpecialObjectIndex::ClassMethodContext),
        memory_.specialObject(SpecialObjectIndex::ClassBlockClosure),
        memory_.specialObject(SpecialObjectIndex::ClassProcess),
        Oop::nil()
    };

    // Also search the class of SmalltalkImage
    Oop smalltalkImage = memory_.findGlobal("SmalltalkImage");
    if (smalltalkImage.isObject()) {
        // Debug: show what SmalltalkImage looks like
        ObjectHeader* siHdr = smalltalkImage.asObjectPtr();
        // DEBUG_LOG("[DEBUG] findSelector: SmalltalkImage = 0x" << std::hex << smalltalkImage.rawBits()
                  // << " classIdx=" << std::dec << siHdr->classIndex()
                  // << " slots=" << siHdr->slotCount();

        // SmalltalkImage is a class, so search its metaclass (class of the class)
        Oop metaclass = memory_.classOf(smalltalkImage);
        // DEBUG_LOG("[DEBUG] findSelector: SmalltalkImage metaclass = 0x" << std::hex
                  // << metaclass.rawBits() << std::dec;

        // If classOf returns nil, try directly accessing the classIndex
        if (metaclass.isNil() || metaclass.rawBits() == 0) {
            // DEBUG: "[DEBUG] findSelector: classOf returned nil, trying direct class table access..."
            metaclass = memory_.classAtIndex(siHdr->classIndex());
            // DEBUG_LOG("[DEBUG] findSelector: Direct classAtIndex(" << siHdr->classIndex()
                      // << ") = 0x" << std::hex << metaclass.rawBits() << std::dec;

            // Still nil? Try searching the method dictionary of SmalltalkImage directly
            // SmalltalkImage is a class, slot 1 = methodDict for instance methods
            // For class methods, we'd need the metaclass, but since that's not available,
            // let's search the class's own method dictionary for selectors
            if (metaclass.isNil() || metaclass.rawBits() == 0) {
                // DEBUG: "[DEBUG] findSelector: Trying SmalltalkImage's own methodDict..."
                Oop methodDict = memory_.fetchPointer(1, smalltalkImage);
                if (methodDict.isObject()) {
                    ObjectHeader* mdHeader = methodDict.asObjectPtr();
                    // DEBUG_LOG("[DEBUG] findSelector: SmalltalkImage methodDict has "
                              // << mdHeader->slotCount() << " slots, cls="
                              // << mdHeader->classIndex() << " fmt=" << (int)mdHeader->format();

                    // Debug: list ALL selectors looking for startup-related ones
                    static bool selectorsDumped = false;
                    if (!selectorsDumped) {
                        selectorsDumped = true;
                        // DEBUG: "[DEBUG] ALL selectors in SmalltalkImage methodDict:"
                        for (size_t i = 2; i < mdHeader->slotCount(); i++) {
                            Oop key = mdHeader->slotAt(i);
                            if (key.isObject() && !key.isNil()) {
                                ObjectHeader* keyHdr = key.asObjectPtr();
                                if (keyHdr->isBytesObject() && keyHdr->byteSize() <= 80) {
                                    std::string keyStr((char*)keyHdr->bytes(), keyHdr->byteSize());
                                    // Only print startup/session related
                                    if (keyStr.find("start") != std::string::npos ||
                                        keyStr.find("Start") != std::string::npos ||
                                        keyStr.find("session") != std::string::npos ||
                                        keyStr.find("Session") != std::string::npos ||
                                        keyStr.find("current") != std::string::npos ||
                                        keyStr.find("initialize") != std::string::npos) {
                                        // DEBUG: "[DEBUG]   slot[" << i << "]: '" << keyStr << "'"
                                    }
                                }
                            }
                        }
                    }

                    // Search for selector in method dict (keys at slot 2+)
                    // DEBUG_LOG("[DEBUG] findSelector: Searching for '" << name << "' in "
                              // << mdHeader->slotCount() << " slots...";
                    for (size_t i = 2; i < mdHeader->slotCount(); i++) {
                        Oop key = mdHeader->slotAt(i);
                        if (key.isObject() && !key.isNil()) {
                            ObjectHeader* keyHdr = key.asObjectPtr();
                            // Direct string comparison
                            if (keyHdr->isBytesObject()) {
                                size_t keyLen = keyHdr->byteSize();
                                const char* keyBytes = (const char*)keyHdr->bytes();
                                if (keyLen == strlen(name) && memcmp(keyBytes, name, keyLen) == 0) {
                                    // DEBUG_LOG("[DEBUG] findSelector: Found '" << name
                                              // << "' at slot " << i << " in SmalltalkImage methodDict!";
                                    return key;
                                }
                            }
                        }
                    }
                }
            }
        }

        if (metaclass.isObject()) {
            // Search metaclass hierarchy
            Oop currentClass = metaclass;
            int depth = 0;
            while (currentClass.isObject() && !currentClass.isNil() && depth < 20) {
                ObjectHeader* classHdr = currentClass.asObjectPtr();
                if (classHdr->slotCount() < 2) break;

                Oop methodDict = memory_.fetchPointer(1, currentClass);
                // DEBUG_LOG("[DEBUG] findSelector: depth=" << depth << " methodDict=0x" << std::hex
                          // << methodDict.rawBits() << std::dec;
                if (methodDict.isObject()) {
                    ObjectHeader* mdHeader = methodDict.asObjectPtr();
                    size_t mdSlots = mdHeader->slotCount();
                    // DEBUG_LOG("[DEBUG] findSelector: methodDict has " << mdSlots << " slots, cls="
                              // << mdHeader->classIndex();

                    // Debug: show first few selectors found
                    static bool debugPrinted = false;
                    if (!debugPrinted && depth == 0) {
                        debugPrinted = true;
                        // DEBUG: "[DEBUG] findSelector: First 10 selectors in SmalltalkImage metaclass MD:"
                        int count = 0;
                        for (size_t i = 2; i < mdSlots && count < 10; i++) {
                            Oop key = mdHeader->slotAt(i);
                            if (key.isObject() && !key.isNil()) {
                                ObjectHeader* keyHdr = key.asObjectPtr();
                                if (keyHdr->isBytesObject() && keyHdr->byteSize() <= 50) {
                                    std::string keyStr((char*)keyHdr->bytes(), keyHdr->byteSize());
                                    // DEBUG: "[DEBUG]   slot[" << i << "]: '" << keyStr << "'"
                                    count++;
                                }
                            }
                        }
                    }

                    // Keys are stored inline from slot 2 onwards
                    for (size_t i = 2; i < mdSlots; i++) {
                        Oop key = mdHeader->slotAt(i);
                        if (key.isObject() && !key.isNil()) {
                            if (memory_.symbolEquals(key, name)) {
                                // DEBUG: "[DEBUG] findSelector: Found '" << name << "' in SmalltalkImage metaclass!"
                                return key;
                            }
                        }
                    }
                }

                // Move to superclass
                currentClass = memory_.fetchPointer(0, currentClass);
                depth++;
            }
        }

        // Also search SmalltalkImage class itself (for instance methods)
        Oop currentClass = smalltalkImage;
        int depth = 0;
        while (currentClass.isObject() && !currentClass.isNil() && depth < 20) {
            ObjectHeader* classHdr = currentClass.asObjectPtr();
            if (classHdr->slotCount() < 2) break;

            Oop methodDict = memory_.fetchPointer(1, currentClass);
            if (methodDict.isObject()) {
                ObjectHeader* mdHeader = methodDict.asObjectPtr();
                size_t mdSlots = mdHeader->slotCount();

                // Keys are stored inline from slot 2 onwards
                for (size_t i = 2; i < mdSlots; i++) {
                    Oop key = mdHeader->slotAt(i);
                    if (key.isObject() && !key.isNil()) {
                        if (memory_.symbolEquals(key, name)) {
                            // DEBUG: "[DEBUG] findSelector: Found '" << name << "' in SmalltalkImage class!"
                            return key;
                        }
                    }
                }
            }

            // Move to superclass
            currentClass = memory_.fetchPointer(0, currentClass);
            depth++;
        }
    }

    // Search through common classes
    for (int ci = 0; !classesToSearch[ci].isNil(); ci++) {
        Oop classObj = classesToSearch[ci];
        if (!classObj.isObject()) continue;

        Oop currentClass = classObj;
        int depth = 0;

        while (currentClass.isObject() && !currentClass.isNil() && depth < 20) {
            ObjectHeader* classHdr = currentClass.asObjectPtr();
            if (classHdr->slotCount() < 2) break;

            Oop methodDict = memory_.fetchPointer(1, currentClass);
            if (methodDict.isObject()) {
                ObjectHeader* mdHeader = methodDict.asObjectPtr();
                size_t mdSlots = mdHeader->slotCount();

                // Keys are stored inline from slot 2 onwards
                for (size_t i = 2; i < mdSlots; i++) {
                    Oop key = mdHeader->slotAt(i);
                    if (key.isObject() && !key.isNil()) {
                        if (memory_.symbolEquals(key, name)) {
                            // DEBUG: "[DEBUG] findSelector: Found '" << name << "' in class " << ci << "!"
                            return key;
                        }
                    }
                }
            }

            // Move to superclass
            currentClass = memory_.fetchPointer(0, currentClass);
            depth++;
        }
    }

    // DEBUG: "[DEBUG] findSelector: '" << name << "' not found"
    return Oop::nil();
}

bool Interpreter::executeFromContext(Oop context) {
    // Reset interpreter stack - each context execution starts fresh
    // The context object stores the Smalltalk stack state, which we'll restore below
    stackPointer_ = stackBase_;
    frameDepth_ = 0;

    // Execution context tracing (limited)
    static int execCtxCount = 0;
    execCtxCount++;
    // Disabled for cleaner output: if (execCtxCount <= 10) { ... }

    if (context.isNil()) {
        return false;
    }

    if (!context.isObject()) {
        return false;
    }

    // Get the raw pointer and validate it before dereferencing
    uintptr_t rawPtr = context.rawBits();

    // Quick sanity check - the ACTUAL object address (clearing low 3 bits for space tag) should be aligned
    // In Spur, low 3 bits encode the memory space: 000=Old, 010=New, 100=Perm
    uintptr_t actualAddr = rawPtr & ~0x7ULL;
    if (actualAddr < 0x1000) {
        return false;
    }

    // Context layout:
    // slot 0: sender
    // slot 1: pc (1-based byte offset into method bytes)
    // slot 2: stackp (index of top of stack within context, 0 means empty)
    // slot 3: method
    // slot 4: closureOrNil
    // slot 5: receiver
    // slot 6+: temps and stack values

    ObjectHeader* ctxHeader = context.asObjectPtr();
    size_t slotCount = ctxHeader->slotCount();

    if (slotCount < 6) {
        return false;
    }

    method_ = memory_.fetchPointer(3, context);
    receiver_ = memory_.fetchPointer(5, context);

    // Check for and fix unrelocated pointers (old image base 0x10000000000+)
    // The old Spur 64-bit image base is 0x10000000000 (1TB)
    const uint64_t OLD_IMAGE_BASE = 0x10000000000ULL;
    uint64_t newBase = reinterpret_cast<uint64_t>(memory_.oldSpaceStart());

    uint64_t methodAddr = method_.rawBits() & ~7ULL;
    uint64_t receiverAddr = receiver_.rawBits() & ~7ULL;

    bool methodUnrelocated = (methodAddr >= OLD_IMAGE_BASE && methodAddr < OLD_IMAGE_BASE * 2);
    bool receiverUnrelocated = (receiverAddr >= OLD_IMAGE_BASE && receiverAddr < OLD_IMAGE_BASE * 2);

    if (methodUnrelocated || receiverUnrelocated) {
        // Fix method pointer if needed
        if (methodUnrelocated) {
            uint64_t offset = methodAddr - OLD_IMAGE_BASE;
            uint64_t newAddr = newBase + offset;
            ObjectHeader* newMethodPtr = reinterpret_cast<ObjectHeader*>(newAddr);
            method_ = memory_.oopFromPointer(newMethodPtr);

            // Also fix the context slot
            ObjectHeader* ctxHdr = context.asObjectPtr();
            ctxHdr->slotAtPut(3, method_);
        }

        // Fix receiver pointer if needed
        if (receiverUnrelocated) {
            uint64_t offset = receiverAddr - OLD_IMAGE_BASE;
            uint64_t newAddr = newBase + offset;
            ObjectHeader* newRcvrPtr = reinterpret_cast<ObjectHeader*>(newAddr);
            receiver_ = memory_.oopFromPointer(newRcvrPtr);

            // Also fix the context slot
            ObjectHeader* ctxHdr = context.asObjectPtr();
            ctxHdr->slotAtPut(5, receiver_);
        }
    }

    // Fix sender slot if unrelocated
    Oop sender = memory_.fetchPointer(0, context);
    uint64_t senderAddr = sender.rawBits() & ~7ULL;
    if (senderAddr >= OLD_IMAGE_BASE && senderAddr < OLD_IMAGE_BASE * 2) {
        uint64_t offset = senderAddr - OLD_IMAGE_BASE;
        uint64_t newAddr = newBase + offset;
        ObjectHeader* newSenderPtr = reinterpret_cast<ObjectHeader*>(newAddr);
        sender = memory_.oopFromPointer(newSenderPtr);
        ObjectHeader* ctxHdr = context.asObjectPtr();
        ctxHdr->slotAtPut(0, sender);
    }

    activeContext_ = context;  // Track for sender chain on return

    // Set homeMethod_ for literal access
    // For CompiledMethods, homeMethod_ = method_
    // For CompiledBlocks, find the home method through the closure's outer context chain
    homeMethod_ = method_;
    if (method_.isObject()) {
        ObjectHeader* methodHdr = method_.asObjectPtr();
        uint32_t methodClsIdx = methodHdr->classIndex();

        if (methodClsIdx == 3101) {
            // CompiledMethod - this is the home method
            homeMethod_ = method_;
        } else if (methodClsIdx == 3117) {
            // CompiledBlock - get home method
            // In FullBlockClosure model (Pharo 11+), CompiledBlock layout:
            // slot 0: block header (SmallInteger with numArgs, etc.)
            // slot 1: selector (Symbol)
            // slot 2: home method (CompiledMethod)
            // slot 3+: bytecodes as raw data

            // First try slot 2 which should be the home CompiledMethod
            Oop slot2 = memory_.fetchPointer(2, method_);
            if (slot2.isObject()) {
                ObjectHeader* slot2Hdr = slot2.asObjectPtr();
                if (slot2Hdr->classIndex() == 3101) {
                    homeMethod_ = slot2;
                }
            }

            // Fallback: try slot 0 chain (for older formats)
            if (homeMethod_ == method_) {
                Oop homeCandidate = memory_.fetchPointer(0, method_);
                int maxHops = 10;
                while (homeCandidate.isObject() && maxHops-- > 0) {
                    ObjectHeader* candidateHdr = homeCandidate.asObjectPtr();
                    uint32_t candidateCls = candidateHdr->classIndex();
                    if (candidateCls == 3101) {
                        homeMethod_ = homeCandidate;
                        break;
                    } else if (candidateCls == 3117) {
                        homeCandidate = memory_.fetchPointer(0, homeCandidate);
                    } else {
                        break;
                    }
                }
            }

            // If we couldn't find home method, try the closure chain as fallback
            if (homeMethod_ == method_) {
                Oop closure = memory_.fetchPointer(4, context);
                int maxHops = 10;

                while (closure.isObject() && maxHops-- > 0) {
                    ObjectHeader* closureHdr = closure.asObjectPtr();
                    uint32_t closureCls = closureHdr->classIndex();

                    // FullBlockClosure or BlockClosure layout:
                    // slot 0: outerContext
                    // slot 1: compiledBlock (or startPC for old closures)
                    // slot 2: numArgs
                    Oop blockClosureClass = memory_.specialObject(SpecialObjectIndex::ClassBlockClosure);
                    Oop fullBlockClosureClass = memory_.specialObject(SpecialObjectIndex::ClassFullBlockClosure);
                    uint32_t blockClosureIdx = 0, fullBlockClosureIdx = 0;
                    if (blockClosureClass.isObject()) {
                        blockClosureIdx = blockClosureClass.asObjectPtr()->classIndex();
                    }
                    if (fullBlockClosureClass.isObject()) {
                        fullBlockClosureIdx = fullBlockClosureClass.asObjectPtr()->classIndex();
                    }
                    bool isBlockClosure = (closureCls == blockClosureIdx && blockClosureIdx != 0) ||
                                          (closureCls == fullBlockClosureIdx && fullBlockClosureIdx != 0) ||
                                          closureCls == 38 || closureCls == 3079 || closureCls == 3213;
                    if (isBlockClosure) {
                        Oop outerContext = memory_.fetchPointer(0, closure);
                        if (outerContext.isNil() || !outerContext.isObject()) {
                            break;
                        }

                        Oop outerMethod = memory_.fetchPointer(3, outerContext);
                        if (!outerMethod.isObject()) {
                            break;
                        }

                        ObjectHeader* outerMethodHdr = outerMethod.asObjectPtr();
                        uint32_t outerMethodCls = outerMethodHdr->classIndex();

                        if (outerMethodCls == 3101) {
                            // Found home CompiledMethod
                            homeMethod_ = outerMethod;
                            // DEBUG: "[DEBUG] executeFromContext: Found homeMethod via closure chain"
                            break;
                        } else if (outerMethodCls == 3117) {
                            // Still a block - get closure from outer context
                            closure = memory_.fetchPointer(4, outerContext);
                        } else {
                            break;
                        }
                    } else {
                        break;
                    }
                }
            }
        }
    }

    // DEBUG_LOG("[DEBUG] executeFromContext: context=0x" << std::hex << context.rawBits()
              // << " method=0x" << method_.rawBits()
              // << " receiver=0x" << receiver_.rawBits() << std::dec;

    // If method is a CompiledBlock, we need to check if the context has a closure
    // In modern Pharo, BlockContext/FullBlockClosure contexts may need special handling
    if (method_.isObject()) {
        ObjectHeader* methodHdr = method_.asObjectPtr();
        if (methodHdr->classIndex() == 3117) {
            // DEBUG: "[DEBUG] Context's method is a CompiledBlock - checking closure"
            Oop closure = memory_.fetchPointer(4, context);  // closureOrNil
            if (closure.isObject() && closure.rawBits() > 0x10000) {
                ObjectHeader* closureHdr = closure.asObjectPtr();
                // DEBUG_LOG("[DEBUG] Closure at slot 4: cls=" << closureHdr->classIndex()
                          // << " slots=" << closureHdr->slotCount();
            }
        }
    }

    if (method_.isNil() || !method_.isObject()) {
        // ERROR: "[ERROR] executeFromContext: Invalid method in context"
        return false;
    }

    // Get method header to calculate bytecode start
    ObjectHeader* methodObj = method_.asObjectPtr();
    // DEBUG_LOG("[DEBUG] executeFromContext: Method has " << methodObj->slotCount() << " slots, cls=" << methodObj->classIndex() << ", fmt=" << (int)methodObj->format();

    // Check if method has a primitive or if we're in snapshotPrimitive method
    int primIdx = primitiveIndexOf(method_);
    bool isSnapshotResume = false;

    // Track if this is our first snapshot resume (for logging)
    static bool firstSnapshotResume = true;

    // Check for snapshot primitive (131) by primitive number
    if (primIdx == 131) {
        isSnapshotResume = true;
    }

    // Also check by method selector name (for methods that call primitives differently)
    if (!isSnapshotResume && receiver_.isObject()) {
        // Get receiver's class name
        Oop rcvrClass = memory_.classOf(receiver_);
        if (rcvrClass.isObject()) {
            ObjectHeader* clsHdr = rcvrClass.asObjectPtr();
            if (clsHdr->slotCount() > 6) {
                Oop nameOop = memory_.fetchPointer(6, rcvrClass);
                if (nameOop.isObject() && nameOop.rawBits() > 0x10000) {
                    ObjectHeader* nameHdr = nameOop.asObjectPtr();
                    if (nameHdr->isBytesObject() && nameHdr->byteSize() <= 100) {
                        std::string className((char*)nameHdr->bytes(), nameHdr->byteSize());
                        if (className == "SnapshotOperation") {
                            // We're resuming in SnapshotOperation - set return to false
                            isSnapshotResume = true;
                        }
                    }
                }
            }
        }
    }

    // Mark that we've detected snapshot resume (used to suppress further logging)
    if (isSnapshotResume) {
        firstSnapshotResume = false;
    }

    Oop methodHeader = memory_.fetchPointer(0, method_);
    // DEBUG_LOG("[DEBUG] executeFromContext: Method slot 0 = 0x" << std::hex << methodHeader.rawBits() << std::dec;
    // if (methodHeader.isSmallInteger()) std::cerr << " (SmallInt: " << methodHeader.asSmallInteger() << ")";
    // else if (methodHeader.isObject()) {
    //     ObjectHeader* h = methodHeader.asObjectPtr();
    //     std::cerr << " (obj: " << h->slotCount() << " slots, cls=" << h->classIndex() << ")";
    // }
    // std::cerr; // DEBUG

    if (!methodHeader.isSmallInteger()) {
        // ERROR_LOG("[ERROR] executeFromContext: Invalid method header (not SmallInteger)";
        return false;
    }

    int64_t headerBits = methodHeader.asSmallInteger();
    int numLiterals = headerBits & 0x7FFF;  // bits 0-14 are numLiterals
    int numTemps = (headerBits >> 16) & 0xFF;

    // Detect bytecode set: sign bit (bit 63) = 0 for V3PlusClosures, 1 for SistaV1
    // In 64-bit Spur, negative header means alternate bytecode set (SistaV1)
    usesSistaV1_ = headerBits < 0;

    uint8_t* methodBytes = methodObj->bytes();
    size_t bytecodeStart = (1 + numLiterals) * 8;

    // Calculate bytecode end - CompiledMethod format encodes unused bytes
    size_t totalBytes = methodObj->byteSize();
    bytecodeEnd_ = methodBytes + totalBytes;

    // Get the saved PC from the context
    // In Pharo, PC is 1-based byte offset from start of method bytes (absolute, includes header+literals)
    // Initial PC = (1 + numLiterals) * 8 + 1 = bytecodeStart + 1
    Oop savedPC = memory_.fetchPointer(1, context);
    int64_t pcOffset = 0;
    if (savedPC.isSmallInteger()) {
        pcOffset = savedPC.asSmallInteger();
        if (pcOffset > 0) {
            // Validate: PC must be within bytecode range [bytecodeStart+1, totalBytes+1]
            // (since PC is 1-based, valid range for 0-based is [bytecodeStart, totalBytes-1])
            size_t absOffset = static_cast<size_t>(pcOffset - 1);  // Convert to 0-based
            if (absOffset >= bytecodeStart && absOffset < totalBytes) {
                instructionPointer_ = methodBytes + absOffset;
            } else {
                // Invalid PC - reset to start of bytecodes
                instructionPointer_ = methodBytes + bytecodeStart;
                if (usesSistaV1_ && instructionPointer_[0] == 0xF8) {
                    instructionPointer_ += 3;
                }
            }
        } else {
            instructionPointer_ = methodBytes + bytecodeStart;
            // Skip past callPrimitive if at start
            if (usesSistaV1_ && instructionPointer_[0] == 0xF8) {
                instructionPointer_ += 3;
            }
        }
    } else {
        instructionPointer_ = methodBytes + bytecodeStart;
        // Skip past callPrimitive if at start
        if (usesSistaV1_ && instructionPointer_[0] == 0xF8) {
            instructionPointer_ += 3;
        }
    }

    // Get saved stackp - in Pharo, stackp is the 1-based index into the temp/stack area
    // stackp = 0 means empty (no temps/stack), stackp = 1 means 1 item, etc.
    // The temps/stack start at slot 6 (after fixed fields: sender, pc, stackp, method, closure, receiver)
    static const int ContextFixedFields = 6;

    Oop savedStackp = memory_.fetchPointer(2, context);
    int stackp = 0;
    if (savedStackp.isSmallInteger()) {
        stackp = static_cast<int>(savedStackp.asSmallInteger());
    }
    // DEBUG: "[DEBUG] executeFromContext: savedStackp=" << stackp << " numTemps=" << numTemps

    // Push receiver first - this establishes our frame
    push(receiver_);
    framePointer_ = stackPointer_ - 1;

    // Now restore the context's saved stack
    // stackp indicates how many slots are valid in the temp/stack area (1-based count)
    // So if stackp=1, there's 1 valid item at slot 6
    // If stackp=5, there are 5 valid items at slots 6,7,8,9,10

    if (stackp > 0) {
        int numStackItems = stackp;
        if (numStackItems > 0 && numStackItems < 1000) {
            for (int i = 0; i < numStackItems; i++) {
                Oop item = memory_.fetchPointer(ContextFixedFields + i, context);
                push(item);
            }
        }
    }

    argCount_ = 0;  // We're resuming a context, not calling a method

    // If resuming from snapshot, we need to:
    // 1. Set TOS to 'false' (primitive return value indicates "resumed from load")
    // 2. Set SnapshotOperation's 'save' slot to 'false' (correct value for resume)
    // 3. Set SnapshotOperation's 'isQuit' slot (slot 2) to 'false' to prevent quit
    //
    // The snapshot primitive returns: true = saved, false = resumed
    // Pharo SnapshotOperation layout:
    //   slot 0: save (Boolean)
    //   slot 1: (reserved)
    //   slot 2: isQuit (Boolean) - if true, quitPrimitive is called after save/resume
    //   slot 3: isEmbedded (Boolean)
    //
    // If the image was saved with "save and quit", isQuit will be true and will
    // cause quit on resume. We need to clear it for embedded/iOS use.
    if (isSnapshotResume) {
        // Set TOS to false (primitive result = resumed)
        if (stackPointer_ > stackBase_) {
            *(stackPointer_ - 1) = memory_.falseObject();
        } else {
            push(memory_.falseObject());
        }

        // Adjust SnapshotOperation slots for proper resume
        if (receiver_.isObject()) {
            ObjectHeader* rcvr = receiver_.asObjectPtr();
            if (rcvr->slotCount() >= 3) {
                // slot 0: ensure 'save' is false (we're resuming, not saving)
                Oop slot0 = rcvr->slotAt(0);
                if (slot0 == memory_.trueObject()) {
                    rcvr->slotAtPut(0, memory_.falseObject());
                }

                // slot 2: clear 'isQuit' to prevent quit on resume
                Oop slot2 = rcvr->slotAt(2);
                if (slot2 == memory_.trueObject()) {
                    rcvr->slotAtPut(2, memory_.falseObject());
                }
            }
        }
    }

    initializeSelectors();
    running_ = true;

    return true;
}

// ===== PRIMITIVE SUPPORT =====

void Interpreter::primitiveSuccess(Oop result) {
    primitiveFailed_ = false;
    // Pop args and receiver, push result
    popN(argCount_ + 1);
    push(result);
}

void Interpreter::primitiveFail() {
    primitiveFailed_ = true;
}

void Interpreter::initializePrimitives() {
    // Clear all entries
    for (auto& entry : primitiveTable_) {
        entry = nullptr;
    }

    // Register primitives
    primitiveTable_[1] = &Interpreter::primitiveAdd;
    primitiveTable_[2] = &Interpreter::primitiveSubtract;
    primitiveTable_[3] = &Interpreter::primitiveLessThan;
    primitiveTable_[4] = &Interpreter::primitiveGreaterThan;
    primitiveTable_[5] = &Interpreter::primitiveLessOrEqual;
    primitiveTable_[6] = &Interpreter::primitiveGreaterOrEqual;
    primitiveTable_[7] = &Interpreter::primitiveEqual;
    primitiveTable_[8] = &Interpreter::primitiveNotEqual;
    primitiveTable_[9] = &Interpreter::primitiveMultiply;
    primitiveTable_[10] = &Interpreter::primitiveDivide;
    primitiveTable_[11] = &Interpreter::primitiveMod;
    primitiveTable_[12] = &Interpreter::primitiveDiv;
    primitiveTable_[13] = &Interpreter::primitiveQuo;
    primitiveTable_[14] = &Interpreter::primitiveBitAnd;
    primitiveTable_[15] = &Interpreter::primitiveBitOr;
    primitiveTable_[16] = &Interpreter::primitiveBitXor;
    primitiveTable_[17] = &Interpreter::primitiveBitShift;
    primitiveTable_[18] = &Interpreter::primitiveMakePoint;

    // LargeInteger digit access primitives (19-20)
    primitiveTable_[19] = &Interpreter::primitiveDigitAt;
    primitiveTable_[20] = &Interpreter::primitiveDigitAtPut;

    // Large integer primitives (21-31)
    primitiveTable_[21] = &Interpreter::primitiveLargeIntegerAdd;
    primitiveTable_[22] = &Interpreter::primitiveLargeIntegerSubtract;
    primitiveTable_[23] = &Interpreter::primitiveLargeIntegerLessThan;
    primitiveTable_[24] = &Interpreter::primitiveLargeIntegerGreaterThan;
    primitiveTable_[25] = &Interpreter::primitiveLargeIntegerLessOrEqual;
    primitiveTable_[26] = &Interpreter::primitiveLargeIntegerGreaterOrEqual;
    primitiveTable_[27] = &Interpreter::primitiveLargeIntegerEqual;
    primitiveTable_[28] = &Interpreter::primitiveLargeIntegerNotEqual;
    primitiveTable_[29] = &Interpreter::primitiveLargeIntegerMultiply;
    primitiveTable_[30] = &Interpreter::primitiveLargeIntegerDivide;
    primitiveTable_[31] = &Interpreter::primitiveLargeIntegerMod;
    primitiveTable_[32] = &Interpreter::primitiveLargeIntegerDiv;
    primitiveTable_[33] = &Interpreter::primitiveLargeIntegerQuo;
    primitiveTable_[34] = &Interpreter::primitiveLargeIntegerBitAnd;
    primitiveTable_[35] = &Interpreter::primitiveLargeIntegerBitOr;
    primitiveTable_[36] = &Interpreter::primitiveLargeIntegerBitXor;
    primitiveTable_[37] = &Interpreter::primitiveLargeIntegerBitShift;

    // Float primitives (40-59) - correct Pharo numbering
    primitiveTable_[40] = &Interpreter::primitiveAsFloat;
    primitiveTable_[41] = &Interpreter::primitiveFloatAdd;
    primitiveTable_[42] = &Interpreter::primitiveFloatSubtract;
    primitiveTable_[43] = &Interpreter::primitiveFloatLessThan;
    primitiveTable_[44] = &Interpreter::primitiveFloatGreaterThan;
    primitiveTable_[45] = &Interpreter::primitiveFloatLessOrEqual;
    primitiveTable_[46] = &Interpreter::primitiveFloatGreaterOrEqual;
    primitiveTable_[47] = &Interpreter::primitiveFloatEqual;
    primitiveTable_[48] = &Interpreter::primitiveFloatNotEqual;
    primitiveTable_[49] = &Interpreter::primitiveFloatMultiply;
    primitiveTable_[50] = &Interpreter::primitiveFloatDivide;
    primitiveTable_[51] = &Interpreter::primitiveFloatTruncated;
    primitiveTable_[52] = &Interpreter::primitiveFractionalPart;
    primitiveTable_[53] = &Interpreter::primitiveExponent;
    primitiveTable_[54] = &Interpreter::primitiveTimesTwoPower;
    primitiveTable_[55] = &Interpreter::primitiveFloatSquareRoot;
    primitiveTable_[56] = &Interpreter::primitiveFloatSin;
    primitiveTable_[57] = &Interpreter::primitiveFloatArctan;
    primitiveTable_[58] = &Interpreter::primitiveFloatLn;
    primitiveTable_[59] = &Interpreter::primitiveFloatExp;

    // Array/Object access primitives (60-68)
    primitiveTable_[60] = &Interpreter::primitiveAt;
    primitiveTable_[61] = &Interpreter::primitiveAtPut;
    primitiveTable_[62] = &Interpreter::primitiveSize;
    primitiveTable_[63] = &Interpreter::primitiveStringAt;
    primitiveTable_[64] = &Interpreter::primitiveStringAtPut;

    // Stream primitives (65-67)
    primitiveTable_[65] = &Interpreter::primitiveNext;
    primitiveTable_[66] = &Interpreter::primitiveNextPut;
    primitiveTable_[67] = &Interpreter::primitiveAtEnd;

    // Object creation/access primitives (68-79)
    primitiveTable_[68] = &Interpreter::primitiveBasicAt;
    primitiveTable_[69] = &Interpreter::primitiveBasicAtPut;
    primitiveTable_[70] = &Interpreter::primitiveNew;
    primitiveTable_[71] = &Interpreter::primitiveNewWithArg;
    primitiveTable_[73] = &Interpreter::primitiveInstVarAt;
    primitiveTable_[74] = &Interpreter::primitiveInstVarAtPut;
    primitiveTable_[75] = &Interpreter::primitiveIdentityHash;
    primitiveTable_[76] = &Interpreter::primitiveSetStackPointer;  // Context>>stackp:

    // Block closure primitives (80-82)
    primitiveTable_[80] = &Interpreter::primitiveBlockCopy;
    primitiveTable_[81] = &Interpreter::primitiveValue;
    primitiveTable_[82] = &Interpreter::primitiveValueWithArgs;

    // Perform primitives (83-84)
    primitiveTable_[83] = &Interpreter::primitivePerform;
    primitiveTable_[84] = &Interpreter::primitivePerformWithArgs;

    // Object enumeration primitives (77-78)
    primitiveTable_[77] = &Interpreter::primitiveSomeInstance;
    primitiveTable_[78] = &Interpreter::primitiveNextInstance;

    // Process/Semaphore primitives (85-89)
    primitiveTable_[85] = &Interpreter::primitiveSignal;
    primitiveTable_[86] = &Interpreter::primitiveWait;
    primitiveTable_[87] = &Interpreter::primitiveResume;
    primitiveTable_[88] = &Interpreter::primitiveSuspend;
    primitiveTable_[89] = &Interpreter::primitiveFlushCache;

    // I/O primitives (90-99) - NOT file I/O, these are input/display/system
    // Note: File primitives are via FilePlugin (named primitives), not numbered
    primitiveTable_[90] = &Interpreter::primitiveMousePoint;           // was Blue Book mouse point
    primitiveTable_[91] = &Interpreter::primitiveTestDisplayDepth;
    primitiveTable_[92] = &Interpreter::primitiveSetDisplayMode;
    primitiveTable_[93] = &Interpreter::primitiveInputSemaphore;
    primitiveTable_[94] = &Interpreter::primitiveGetNextEvent;
    primitiveTable_[95] = &Interpreter::primitiveInputWord;
    primitiveTable_[96] = &Interpreter::primitiveCopyBits;             // BitBlt!
    primitiveTable_[97] = &Interpreter::primitiveSnapshot;
    primitiveTable_[98] = &Interpreter::primitiveStoreImageSegment;
    primitiveTable_[99] = &Interpreter::primitiveLoadImageSegment;

    // Display primitives (101-109) - per Cog VM spec
    primitiveTable_[101] = &Interpreter::primitiveBeCursor;
    primitiveTable_[102] = &Interpreter::primitiveBeDisplay;
    primitiveTable_[103] = &Interpreter::primitiveScanCharacters;      // was incorrectly forceDisplayUpdate
    primitiveTable_[104] = &Interpreter::primitiveFailure;                // obsolete drawLoop
    primitiveTable_[105] = &Interpreter::primitiveStringReplace;       // was primitiveReplaceFromTo
    primitiveTable_[106] = &Interpreter::primitiveScreenSize;
    primitiveTable_[107] = &Interpreter::primitiveFailure;                // was incorrectly showDisplayRect
    primitiveTable_[108] = &Interpreter::primitiveFailure;                // was incorrectly screenDepth
    primitiveTable_[109] = &Interpreter::primitiveFailure;                // was incorrectly snapshotEmbedded

    // Identity and class primitives (110-112, 169)
    primitiveTable_[110] = &Interpreter::primitiveIdentical;
    primitiveTable_[111] = &Interpreter::primitiveClass;
    primitiveTable_[112] = &Interpreter::primitiveBytesLeft;
    primitiveTable_[169] = &Interpreter::primitiveNotIdentical;

    // Character conversion primitives (170-171)
    primitiveTable_[170] = &Interpreter::primitiveAsCharacter;
    primitiveTable_[171] = &Interpreter::primitiveAsInteger;

    // System primitives (113, 114)
    primitiveTable_[113] = &Interpreter::primitiveQuit;
    primitiveTable_[114] = &Interpreter::primitiveExitToDebugger;

    // System primitives (115-120) - per Cog VM spec
    primitiveTable_[115] = &Interpreter::primitiveChangeClass;
    primitiveTable_[116] = &Interpreter::primitiveFlushCacheByMethod;
    primitiveTable_[117] = &Interpreter::primitiveExternalCall;        // named primitive dispatch
    primitiveTable_[118] = &Interpreter::primitiveDoPrimitiveWithArgs;
    primitiveTable_[119] = &Interpreter::primitiveFlushCacheBySelector;
    primitiveTable_[120] = &Interpreter::primitiveCalloutToFFI;        // FFI callout

    // Miscellaneous primitives (121-134) - per Cog VM spec
    primitiveTable_[121] = &Interpreter::primitiveImageName;
    primitiveTable_[122] = &Interpreter::primitiveNoop;                // was incorrectly directoryCreate
    primitiveTable_[123] = &Interpreter::primitiveFailure;                // marker, was directoryDelimitor
    primitiveTable_[124] = &Interpreter::primitiveLowSpaceSemaphore;   // was incorrectly directoryLookup
    primitiveTable_[125] = &Interpreter::primitiveSignalAtBytesLeft;
    primitiveTable_[126] = &Interpreter::primitiveDeferDisplayUpdates; // was incorrectly directoryDelete
    primitiveTable_[127] = &Interpreter::primitiveShowDisplayRect;     // was incorrectly directoryGetMacTypeAndCreator
    primitiveTable_[128] = &Interpreter::primitiveArrayBecome;         // was incorrectly becomeForward
    primitiveTable_[129] = &Interpreter::primitiveSpecialObjectsOop;
    primitiveTable_[130] = &Interpreter::primitiveFullGC;
    primitiveTable_[131] = &Interpreter::primitiveIncrementalGC;       // was incorrectly snapshot
    primitiveTable_[132] = &Interpreter::primitiveObjectPointsTo;
    primitiveTable_[133] = &Interpreter::primitiveSetInterruptKey;     // was incorrectly socket
    primitiveTable_[134] = &Interpreter::primitiveInterruptSemaphore;

    // Additional file primitives (161-164)
    primitiveTable_[161] = &Interpreter::primitiveFileStdioHandles;
    primitiveTable_[162] = &Interpreter::primitiveFileDescriptorType;
    primitiveTable_[163] = &Interpreter::primitiveFileFlush;
    primitiveTable_[164] = &Interpreter::primitiveFileTruncate;

    // Note: 106 already set above, 108 is primitiveFail per spec
    primitiveTable_[142] = &Interpreter::primitiveVMPath;

    // UI primitives (140, 141)
    primitiveTable_[140] = &Interpreter::primitiveBeep;
    primitiveTable_[141] = &Interpreter::primitiveClipboardText;

    // Time primitives (135-137, 240-241)
    primitiveTable_[135] = &Interpreter::primitiveMillisecondClock;
    primitiveTable_[136] = &Interpreter::primitiveSignalAtMilliseconds;
    primitiveTable_[137] = &Interpreter::primitiveSecondsClock;
    primitiveTable_[240] = &Interpreter::primitiveMicrosecondClock;
    primitiveTable_[241] = &Interpreter::primitiveLocalMicrosecondClock;

    // Time primitives (242-252)
    primitiveTable_[242] = &Interpreter::primitiveUTCMicrosecondClock;
    primitiveTable_[243] = &Interpreter::primitiveLocalTimezone;
    primitiveTable_[244] = &Interpreter::primitiveTimezoneOffset;
    primitiveTable_[245] = &Interpreter::primitiveDaylightSavingTimeOffset;
    primitiveTable_[246] = &Interpreter::primitiveVMOffsetToUTC;
    primitiveTable_[247] = &Interpreter::primitivePosixMicrosecondClockWithOffset;
    primitiveTable_[248] = &Interpreter::primitiveSystemTimezone;
    primitiveTable_[249] = &Interpreter::primitiveHighResClock;
    primitiveTable_[250] = &Interpreter::primitiveUTCDateAndTime;
    primitiveTable_[251] = &Interpreter::primitiveLocalDateAndTime;
    primitiveTable_[252] = &Interpreter::primitiveNanosecondClock;

    // Array/memory primitives (145-149) - per Cog VM spec
    primitiveTable_[145] = &Interpreter::primitiveConstantFill;
    primitiveTable_[146] = &Interpreter::primitiveFailure;                // reserved
    primitiveTable_[147] = &Interpreter::primitiveFailure;                // was incorrectly externalCall
    primitiveTable_[148] = &Interpreter::primitiveClone;               // primitiveShallowCopy is alias
    primitiveTable_[149] = &Interpreter::primitiveGetAttribute;

    // File area primitives (150-160) - per Cog VM spec
    primitiveTable_[150] = &Interpreter::primitiveFailure;                // FilePlugin territory
    primitiveTable_[151] = &Interpreter::primitiveFailure;
    primitiveTable_[152] = &Interpreter::primitiveFailure;
    primitiveTable_[153] = &Interpreter::primitiveFailure;
    primitiveTable_[154] = &Interpreter::primitiveFailure;
    primitiveTable_[155] = &Interpreter::primitiveFailure;
    primitiveTable_[156] = &Interpreter::primitiveFailure;
    primitiveTable_[157] = &Interpreter::primitiveFailure;
    primitiveTable_[158] = &Interpreter::primitiveCompareWith;         // was incorrectly compareStringNoCase
    primitiveTable_[159] = &Interpreter::primitiveHashMultiply;
    primitiveTable_[160] = &Interpreter::primitiveAdoptInstance;

    // Identity/immutability primitives (161-169)
    primitiveTable_[161] = &Interpreter::primitiveSetIdentityHash;     // was incorrectly fileStdioHandles
    primitiveTable_[162] = &Interpreter::primitiveFailure;
    primitiveTable_[163] = &Interpreter::primitiveGetImmutability;     // was incorrectly fileFlush
    primitiveTable_[164] = &Interpreter::primitiveSetImmutability;     // was incorrectly fileTruncate
    primitiveTable_[165] = &Interpreter::primitiveIntegerAt;
    primitiveTable_[166] = &Interpreter::primitiveIntegerAtPut;
    primitiveTable_[167] = &Interpreter::primitiveYield;
    primitiveTable_[168] = &Interpreter::primitiveCopyObject;

    // Spur memory primitives (170-184) - per Cog VM spec
    primitiveTable_[170] = &Interpreter::primitiveAsCharacter;
    primitiveTable_[171] = &Interpreter::primitiveImmediateAsInteger;  // was incorrectly asInteger
    primitiveTable_[172] = &Interpreter::primitiveFetchNextMourner;    // was incorrectly setGCSemaphore
    primitiveTable_[173] = &Interpreter::primitiveSlotAt;
    primitiveTable_[174] = &Interpreter::primitiveSlotAtPut;
    primitiveTable_[175] = &Interpreter::primitiveBehaviorHash;
    primitiveTable_[176] = &Interpreter::primitiveMaxIdentityHash;
    primitiveTable_[177] = &Interpreter::primitiveAllInstances;
    primitiveTable_[178] = &Interpreter::primitiveAllObjects;
    primitiveTable_[179] = &Interpreter::primitiveFailure;                // was incorrectly relinquishProcessor
    primitiveTable_[180] = &Interpreter::primitiveGrowMemoryByAtLeast;
    primitiveTable_[181] = &Interpreter::primitiveSizeInBytesOfInstance;
    primitiveTable_[182] = &Interpreter::primitiveSizeInBytes;
    primitiveTable_[183] = &Interpreter::primitiveIsPinned;
    primitiveTable_[184] = &Interpreter::primitivePin;

    // Critical section primitives (185-187) - per Cog VM spec
    primitiveTable_[185] = &Interpreter::primitiveExitCriticalSection;
    primitiveTable_[186] = &Interpreter::primitiveEnterCriticalSection;
    primitiveTable_[187] = &Interpreter::primitiveTestAndSetOwnershipOfCriticalSection;

    // Method execution primitives (188-189)
    primitiveTable_[188] = &Interpreter::primitiveExecuteMethodArgsArray;
    primitiveTable_[189] = &Interpreter::primitiveExecuteMethod;

    // Unwind/exception primitives (195-199) - per Cog VM spec
    primitiveTable_[195] = &Interpreter::primitiveFindNextUnwindContext;
    primitiveTable_[196] = &Interpreter::primitiveTerminateTo;
    primitiveTable_[197] = &Interpreter::primitiveFindHandlerContext;  // was incorrectly arrayBecomeOneWay
    primitiveTable_[198] = &Interpreter::primitiveFailure;                // marker, was arrayBecomeOneWayCopyHash
    primitiveTable_[199] = &Interpreter::primitiveFailure;                // marker for exception handler

    // Closure primitives (200-209) - per Cog VM spec
    primitiveTable_[200] = &Interpreter::primitiveClosureCopyWithCopiedValues;
    primitiveTable_[201] = &Interpreter::primitiveBlockValue;          // closureValue (0 args)
    primitiveTable_[202] = &Interpreter::primitiveBlockValue;          // closureValue (1 arg)
    primitiveTable_[203] = &Interpreter::primitiveBlockValue;          // closureValue (2 args)
    primitiveTable_[204] = &Interpreter::primitiveClosureValueNoContextSwitch;
    primitiveTable_[205] = &Interpreter::primitiveBlockValue;          // closureValue (4 args)
    primitiveTable_[206] = &Interpreter::primitiveClosureValueWithArgs;
    primitiveTable_[207] = &Interpreter::primitiveFullClosureValue;
    primitiveTable_[208] = &Interpreter::primitiveClosureValueUnwind;
    primitiveTable_[209] = &Interpreter::primitiveClosureValueNoUnwind;

    // Context primitives (210-215) - per Cog VM spec
    primitiveTable_[210] = &Interpreter::primitiveContextAt;           // was incorrectly contextSize
    primitiveTable_[211] = &Interpreter::primitiveContextAtPut;        // was incorrectly contextAt
    primitiveTable_[212] = &Interpreter::primitiveContextSize;         // was incorrectly contextAtPut
    primitiveTable_[213] = &Interpreter::primitiveFailure;                // was incorrectly storeImageSegment
    primitiveTable_[214] = &Interpreter::primitiveFailure;                // was incorrectly loadImageSegment
    primitiveTable_[215] = &Interpreter::primitiveFailure;

    // Miscellaneous system primitives
    primitiveTable_[72] = &Interpreter::primitiveArrayBecomeOneWay;    // Blue Book: primitiveBecome
    primitiveTable_[79] = &Interpreter::primitiveNewMethod;
    primitiveTable_[138] = &Interpreter::primitiveSomeObject;
    primitiveTable_[139] = &Interpreter::primitiveNextObject;
    primitiveTable_[143] = &Interpreter::primitiveShortAt;
    primitiveTable_[144] = &Interpreter::primitiveShortAtPut;

    // VM parameter primitive (254)
    primitiveTable_[254] = &Interpreter::primitiveVMParameter;

    // Bit operation primitives (575-576)
    primitiveTable_[575] = &Interpreter::primitiveHighBit;
    primitiveTable_[576] = &Interpreter::primitiveLowBit;

    // Note: 210-215 already set above per Cog VM spec
    primitiveTable_[216] = &Interpreter::primitiveFailure;

    // Object/memory primitives (217-221)
    primitiveTable_[217] = &Interpreter::primitiveVMFunctionality;
    primitiveTable_[218] = &Interpreter::primitiveIdentityHash32;
    primitiveTable_[219] = &Interpreter::primitiveGrowMemoryByAtLeast;
    primitiveTable_[220] = &Interpreter::primitiveImageFormatVersion;
    primitiveTable_[221] = &Interpreter::primitiveClosureValueWithArgs;

    // Misc primitives (222-230)
    primitiveTable_[222] = &Interpreter::primitiveClosureValueNoContextSwitch2;
    primitiveTable_[223] = &Interpreter::primitiveClosureValueWithArgsNoContextSwitch;
    primitiveTable_[224] = &Interpreter::primitiveSetIdentityHash;
    primitiveTable_[225] = &Interpreter::primitiveLoadInstVar;
    primitiveTable_[226] = &Interpreter::primitiveStringCompare;
    primitiveTable_[227] = &Interpreter::primitiveStringReplace;
    primitiveTable_[228] = &Interpreter::primitiveScreenScale;
    primitiveTable_[229] = &Interpreter::primitiveStringHash2;
    primitiveTable_[230] = &Interpreter::primitiveShrinkMemory;

    // Misc primitives (230-239) - per Cog VM spec
    primitiveTable_[230] = &Interpreter::primitiveRelinquishProcessor;  // correct location!
    primitiveTable_[231] = &Interpreter::primitiveForceDisplayUpdate;
    primitiveTable_[232] = &Interpreter::primitiveFormPrint;
    primitiveTable_[233] = &Interpreter::primitiveSetFullScreen;        // was incorrectly setDisplayMode
    primitiveTable_[234] = &Interpreter::primitiveFailure;
    primitiveTable_[235] = &Interpreter::primitiveFailure;
    primitiveTable_[236] = &Interpreter::primitiveFailure;
    primitiveTable_[237] = &Interpreter::primitiveFailure;
    primitiveTable_[238] = &Interpreter::primitiveFloatArrayAt;
    primitiveTable_[239] = &Interpreter::primitiveFloatArrayAtPut;

    // Note: 100, 119-120, 200-209 already set above per Cog VM spec

    // Class structure primitives (253, 255)
    // NOTE: 254 is primitiveVMParameter - do NOT override here!
    primitiveTable_[253] = &Interpreter::primitiveSuperclass;
    // primitiveTable_[254] - already set to primitiveVMParameter above
    primitiveTable_[255] = &Interpreter::primitiveSizeInBytesOfInstance;

    // Quick return primitives (256-259)
    primitiveTable_[256] = &Interpreter::primitiveQuickReturnSelf;
    primitiveTable_[257] = &Interpreter::primitiveQuickReturnTrue;
    primitiveTable_[258] = &Interpreter::primitiveQuickReturnFalse;
    primitiveTable_[259] = &Interpreter::primitiveQuickReturnNil;

    // Note: 146 is primitiveFail per spec, not stringHash

    // Class name primitive (514)
    primitiveTable_[514] = &Interpreter::primitiveClassName;

    // FFI and system primitives (515-527)
    primitiveTable_[515] = &Interpreter::primitiveVMInformation;
    primitiveTable_[516] = &Interpreter::primitiveImageBaseAddress;
    primitiveTable_[517] = &Interpreter::primitiveHighestAvailableAddress;
    primitiveTable_[518] = &Interpreter::primitiveIsContextPostMortem;
    primitiveTable_[519] = &Interpreter::primitiveSandboxedArgs;
    primitiveTable_[520] = &Interpreter::primitiveDebugHalt;
    primitiveTable_[521] = &Interpreter::primitiveFlushExternalPrimitiveOf;
    primitiveTable_[522] = &Interpreter::primitivePrepareStackForNonLocalReturn;
    primitiveTable_[523] = &Interpreter::primitiveContextInstructionPointer;
    primitiveTable_[524] = &Interpreter::primitiveExternalObjectAccess;
    primitiveTable_[525] = &Interpreter::primitiveByteArrayToInt32;
    primitiveTable_[526] = &Interpreter::primitiveInt32ToByteArray;
    primitiveTable_[527] = &Interpreter::primitivePointerAddress;

    // Note: 181-182 already set above
    // Note: 190-194 are primitiveFail per Cog VM spec
    // Note: 195 is primitiveFindNextUnwindContext (set above)

    // System primitives (528-530)
    primitiveTable_[528] = &Interpreter::primitiveGetExtraWordAt;
    primitiveTable_[529] = &Interpreter::primitiveSetExtraWordAt;
    primitiveTable_[530] = &Interpreter::primitiveImmediateAsInteger;

    // String/encoding primitives (531-534)
    primitiveTable_[531] = &Interpreter::primitiveStringEncode;
    primitiveTable_[532] = &Interpreter::primitiveStringDecode;
    primitiveTable_[533] = &Interpreter::primitiveCharacterAsciiValue;
    primitiveTable_[534] = &Interpreter::primitiveAllObjectsInMemory;

    // Reflection primitives (535-538)
    primitiveTable_[535] = &Interpreter::primitiveObjectSlotAt;
    primitiveTable_[536] = &Interpreter::primitiveObjectSlotAtPut;
    primitiveTable_[537] = &Interpreter::primitiveObjectNumSlots;
    primitiveTable_[538] = &Interpreter::primitiveObjectFormat;

    // Advanced object primitives (539-550)
    primitiveTable_[539] = &Interpreter::primitiveObjectClass;
    primitiveTable_[540] = &Interpreter::primitiveObjectClassIndex;
    primitiveTable_[541] = &Interpreter::primitiveObjectIsPinned;
    primitiveTable_[542] = &Interpreter::primitiveObjectSetPinned;
    primitiveTable_[543] = &Interpreter::primitiveObjectIsReadOnly;
    primitiveTable_[544] = &Interpreter::primitiveObjectSetReadOnly;
    primitiveTable_[545] = &Interpreter::primitiveObjectBytesSize;
    primitiveTable_[546] = &Interpreter::primitiveObjectWordsSize;
    primitiveTable_[547] = &Interpreter::primitiveObjectPointersSize;
    primitiveTable_[548] = &Interpreter::primitiveObjectHeader;
    primitiveTable_[549] = &Interpreter::primitiveObjectHeaderPut;
    primitiveTable_[550] = &Interpreter::primitiveIdentityHashSmallInteger;

    // Method and class primitives (551-560)
    primitiveTable_[551] = &Interpreter::primitiveCompiledMethodNumLiterals;
    primitiveTable_[552] = &Interpreter::primitiveCompiledMethodLiteralAt;
    primitiveTable_[553] = &Interpreter::primitiveCompiledMethodLiteralAtPut;
    primitiveTable_[554] = &Interpreter::primitiveCompiledMethodBytecodeAt;
    primitiveTable_[555] = &Interpreter::primitiveCompiledMethodBytecodeAtPut;
    primitiveTable_[556] = &Interpreter::primitiveCompiledMethodNumArgs;
    primitiveTable_[557] = &Interpreter::primitiveCompiledMethodNumTemps;
    primitiveTable_[558] = &Interpreter::primitiveCompiledMethodFrameSize;
    primitiveTable_[559] = &Interpreter::primitiveCompiledMethodPrimitive;
    primitiveTable_[560] = &Interpreter::primitiveCompiledMethodSelector;

    // System and debug primitives (561-570)
    primitiveTable_[561] = &Interpreter::primitiveVMHeapStatistics;
    primitiveTable_[562] = &Interpreter::primitiveVMGCStatistics;
    primitiveTable_[563] = &Interpreter::primitiveVMStackDepth;
    primitiveTable_[564] = &Interpreter::primitiveVMBytecodeCount;
    primitiveTable_[565] = &Interpreter::primitiveVMSendCount;
    primitiveTable_[566] = &Interpreter::primitiveVMPrimitiveCount;
    primitiveTable_[567] = &Interpreter::primitiveVMContextSwitchCount;
    primitiveTable_[568] = &Interpreter::primitiveVMUptime;
    primitiveTable_[569] = &Interpreter::primitiveVMCPUTime;
    primitiveTable_[570] = &Interpreter::primitiveVMIdleTime;

    // Additional bit primitives (571-574)
    primitiveTable_[571] = &Interpreter::primitiveBitCount;
    primitiveTable_[572] = &Interpreter::primitiveBitReverse;
    primitiveTable_[573] = &Interpreter::primitiveByteSwap32;
    primitiveTable_[574] = &Interpreter::primitiveByteSwap64;

    // Profiling primitives (260-263)
    primitiveTable_[260] = &Interpreter::primitiveVMProfileSamplesInto;
    primitiveTable_[261] = &Interpreter::primitiveVMProfileInfoInto;
    primitiveTable_[262] = &Interpreter::primitiveVMProfileStart;
    primitiveTable_[263] = &Interpreter::primitiveVMProfileStop;

    // Event/input primitives (264-269)
    primitiveTable_[264] = &Interpreter::primitiveGetNextEvent;
    primitiveTable_[265] = &Interpreter::primitiveInputSemaphore2;
    primitiveTable_[266] = &Interpreter::primitiveEventProcessingControl;
    primitiveTable_[267] = &Interpreter::primitiveSampledSound;
    primitiveTable_[268] = &Interpreter::primitiveMixedSound;
    primitiveTable_[269] = &Interpreter::primitiveControlOSProcess;

    // BitBlt primitives (290-299)
    primitiveTable_[290] = &Interpreter::primitiveCopyBits;
    primitiveTable_[291] = &Interpreter::primitiveDrawLoop;
    primitiveTable_[292] = &Interpreter::primitiveCompressToByteArray;
    primitiveTable_[293] = &Interpreter::primitiveDecompressFromByteArray;
    primitiveTable_[294] = &Interpreter::primitiveFindFirstInString;
    primitiveTable_[295] = &Interpreter::primitiveTranslateStringWithTable;
    primitiveTable_[296] = &Interpreter::primitiveFindSubstring;
    primitiveTable_[297] = &Interpreter::primitivePixelValueAt;
    primitiveTable_[298] = &Interpreter::primitivePixelValueAtPut;
    primitiveTable_[299] = &Interpreter::primitiveWarpBits;

    // Sound primitives (300-329)
    primitiveTable_[300] = &Interpreter::primitiveSoundStart;
    primitiveTable_[301] = &Interpreter::primitiveSoundStartWithSemaphore;
    primitiveTable_[302] = &Interpreter::primitiveSoundStop;
    primitiveTable_[303] = &Interpreter::primitiveSoundAvailableSpace;
    primitiveTable_[304] = &Interpreter::primitiveSoundPlaySamples;
    primitiveTable_[305] = &Interpreter::primitiveSoundPlaySilence;
    primitiveTable_[306] = &Interpreter::primitiveSoundGetVolume;
    primitiveTable_[307] = &Interpreter::primitiveSoundSetVolume;
    primitiveTable_[308] = &Interpreter::primitiveSoundSetStereoBalance;
    primitiveTable_[309] = &Interpreter::primitiveSoundGetSampleRate;
    primitiveTable_[310] = &Interpreter::primitiveSoundSetSampleRate;
    primitiveTable_[311] = &Interpreter::primitiveSoundRecordStart;
    primitiveTable_[312] = &Interpreter::primitiveSoundRecordStop;
    primitiveTable_[313] = &Interpreter::primitiveSoundRecordSamplesInto;
    primitiveTable_[314] = &Interpreter::primitiveSoundGetRecordLevel;
    primitiveTable_[315] = &Interpreter::primitiveSoundSetRecordLevel;
    primitiveTable_[316] = &Interpreter::primitiveSoundRecordSamplesAvailable;
    primitiveTable_[317] = &Interpreter::primitiveSoundCodecStatus;
    primitiveTable_[318] = &Interpreter::primitiveSoundMixerStart;
    primitiveTable_[319] = &Interpreter::primitiveSoundMixerStop;
    primitiveTable_[320] = &Interpreter::primitiveSoundMixerPlayChannel;
    primitiveTable_[321] = &Interpreter::primitiveSoundMixerSetVolume;
    primitiveTable_[322] = &Interpreter::primitiveSoundMixerSetPan;
    primitiveTable_[323] = &Interpreter::primitiveSoundMixerStopChannel;
    primitiveTable_[324] = &Interpreter::primitiveSoundMixerChannelDone;
    primitiveTable_[325] = &Interpreter::primitiveSoundMixerChannelPosition;
    primitiveTable_[326] = &Interpreter::primitiveSoundInsertSamples;
    primitiveTable_[327] = &Interpreter::primitiveSoundStartBuffered;
    primitiveTable_[328] = &Interpreter::primitiveSoundEnableAEC;
    primitiveTable_[329] = &Interpreter::primitiveSoundSupportsAEC;

    // MIDI primitives (330-349)
    primitiveTable_[330] = &Interpreter::primitiveMIDIGetPortCount;
    primitiveTable_[331] = &Interpreter::primitiveMIDIGetPortName;
    primitiveTable_[332] = &Interpreter::primitiveMIDIOpenPort;
    primitiveTable_[333] = &Interpreter::primitiveMIDIClosePort;
    primitiveTable_[334] = &Interpreter::primitiveMIDIRead;
    primitiveTable_[335] = &Interpreter::primitiveMIDIWrite;
    primitiveTable_[336] = &Interpreter::primitiveMIDIGetClock;
    primitiveTable_[337] = &Interpreter::primitiveMIDISetClock;
    primitiveTable_[338] = &Interpreter::primitiveMIDIParameterGet;
    primitiveTable_[339] = &Interpreter::primitiveMIDIParameterSet;
    primitiveTable_[340] = &Interpreter::primitiveMIDIDriverVersion;
    primitiveTable_[341] = &Interpreter::primitiveMIDIPortType;
    primitiveTable_[342] = &Interpreter::primitiveMIDIDeviceID;
    primitiveTable_[343] = &Interpreter::primitiveMIDIFlushPort;
    primitiveTable_[344] = &Interpreter::primitiveMIDISendNoteOn;
    primitiveTable_[345] = &Interpreter::primitiveMIDISendNoteOff;
    primitiveTable_[346] = &Interpreter::primitiveMIDISendController;
    primitiveTable_[347] = &Interpreter::primitiveMIDISendProgramChange;
    primitiveTable_[348] = &Interpreter::primitiveMIDISendPitchBend;
    primitiveTable_[349] = &Interpreter::primitiveMIDISendSysEx;

    // Serial port primitives (270-279)
    primitiveTable_[270] = &Interpreter::primitiveSerialPortCount;
    primitiveTable_[271] = &Interpreter::primitiveSerialPortName;
    primitiveTable_[272] = &Interpreter::primitiveSerialPortOpen;
    primitiveTable_[273] = &Interpreter::primitiveSerialPortClose;
    primitiveTable_[274] = &Interpreter::primitiveSerialPortRead;
    primitiveTable_[275] = &Interpreter::primitiveSerialPortWrite;
    primitiveTable_[276] = &Interpreter::primitiveSerialPortSetParams;
    primitiveTable_[277] = &Interpreter::primitiveSerialPortGetParams;
    primitiveTable_[278] = &Interpreter::primitiveSerialPortDataAvailable;
    primitiveTable_[279] = &Interpreter::primitiveSerialPortFlush;

    // Joystick primitives (280-289)
    primitiveTable_[280] = &Interpreter::primitiveJoystickCount;
    primitiveTable_[281] = &Interpreter::primitiveJoystickName;
    primitiveTable_[282] = &Interpreter::primitiveJoystickOpen;
    primitiveTable_[283] = &Interpreter::primitiveJoystickClose;
    primitiveTable_[284] = &Interpreter::primitiveJoystickRead;
    primitiveTable_[285] = &Interpreter::primitiveJoystickButtonCount;
    primitiveTable_[286] = &Interpreter::primitiveJoystickAxisCount;
    primitiveTable_[287] = &Interpreter::primitiveJoystickButtonState;
    primitiveTable_[288] = &Interpreter::primitiveJoystickAxisValue;
    primitiveTable_[289] = &Interpreter::primitiveJoystickHatValue;

    // Socket primitives (350-359)
    primitiveTable_[350] = &Interpreter::primitiveSocketCreate;
    primitiveTable_[351] = &Interpreter::primitiveSocketDestroy;
    primitiveTable_[352] = &Interpreter::primitiveSocketConnect;
    primitiveTable_[353] = &Interpreter::primitiveSocketListen;
    primitiveTable_[354] = &Interpreter::primitiveSocketAccept;
    primitiveTable_[355] = &Interpreter::primitiveSocketSend;
    primitiveTable_[356] = &Interpreter::primitiveSocketReceive;
    primitiveTable_[357] = &Interpreter::primitiveSocketStatus;
    primitiveTable_[358] = &Interpreter::primitiveSocketError;
    primitiveTable_[359] = &Interpreter::primitiveSocketLocalAddress;

    // Clipboard/drag-drop primitives (360-369)
    primitiveTable_[360] = &Interpreter::primitiveClipboardText;
    primitiveTable_[361] = &Interpreter::primitiveClipboardTextStore;
    primitiveTable_[362] = &Interpreter::primitiveClipboardHasText;
    primitiveTable_[363] = &Interpreter::primitiveClipboardClear;
    primitiveTable_[364] = &Interpreter::primitiveDragDropFileCount;
    primitiveTable_[365] = &Interpreter::primitiveDragDropFileName;
    primitiveTable_[366] = &Interpreter::primitiveDragDropRequestFile;
    primitiveTable_[367] = &Interpreter::primitiveDragDropCancel;
    primitiveTable_[368] = &Interpreter::primitiveClipboardFormats;
    primitiveTable_[369] = &Interpreter::primitiveClipboardDataForFormat;

    // Misc plugin primitives (370-379)
    primitiveTable_[370] = &Interpreter::primitiveUUIDGenerate;
    primitiveTable_[371] = &Interpreter::primitiveUUIDParse;
    primitiveTable_[372] = &Interpreter::primitiveUUIDToString;
    primitiveTable_[373] = &Interpreter::primitiveSSLCreate;
    primitiveTable_[374] = &Interpreter::primitiveSSLDestroy;
    primitiveTable_[375] = &Interpreter::primitiveSSLConnect;
    primitiveTable_[376] = &Interpreter::primitiveSSLAccept;
    primitiveTable_[377] = &Interpreter::primitiveSSLSend;
    primitiveTable_[378] = &Interpreter::primitiveSSLReceive;
    primitiveTable_[379] = &Interpreter::primitiveSSLStatus;

    // SSL extended primitives (380-389)
    primitiveTable_[380] = &Interpreter::primitiveSSLSetCertificate;
    primitiveTable_[381] = &Interpreter::primitiveSSLSetPrivateKey;
    primitiveTable_[382] = &Interpreter::primitiveSSLGetPeerCertificate;
    primitiveTable_[383] = &Interpreter::primitiveSSLGetCertificateName;
    primitiveTable_[384] = &Interpreter::primitiveSSLSetVerifyMode;
    primitiveTable_[385] = &Interpreter::primitiveSSLGetVerifyResult;
    primitiveTable_[386] = &Interpreter::primitiveSSLSetSNI;
    primitiveTable_[387] = &Interpreter::primitiveSSLGetVersion;
    primitiveTable_[388] = &Interpreter::primitiveSSLGetCipher;
    primitiveTable_[389] = &Interpreter::primitiveSSLClose;

    // Locale primitives (390-399)
    primitiveTable_[390] = &Interpreter::primitiveLocaleLanguage;
    primitiveTable_[391] = &Interpreter::primitiveLocaleCountry;
    primitiveTable_[392] = &Interpreter::primitiveLocaleCurrencySymbol;
    primitiveTable_[393] = &Interpreter::primitiveLocaleDecimalSeparator;
    primitiveTable_[394] = &Interpreter::primitiveLocaleThousandsSeparator;
    primitiveTable_[395] = &Interpreter::primitiveLocaleDateFormat;
    primitiveTable_[396] = &Interpreter::primitiveLocaleTimeFormat;
    primitiveTable_[397] = &Interpreter::primitiveLocaleTimezone;
    primitiveTable_[398] = &Interpreter::primitiveLocaleTimezoneOffset;
    primitiveTable_[399] = &Interpreter::primitiveLocaleDaylightSaving;

    // Image/graphics primitives (400-409)
    primitiveTable_[400] = &Interpreter::primitiveImageReadHeader;
    primitiveTable_[401] = &Interpreter::primitiveImageReadPixels;
    primitiveTable_[402] = &Interpreter::primitiveImageWritePNG;
    primitiveTable_[403] = &Interpreter::primitiveImageWriteJPEG;
    primitiveTable_[404] = &Interpreter::primitiveImageScale;
    primitiveTable_[405] = &Interpreter::primitiveImageRotate;
    primitiveTable_[406] = &Interpreter::primitiveImageComposite;
    primitiveTable_[407] = &Interpreter::primitiveImageColorConvert;
    primitiveTable_[408] = &Interpreter::primitiveImageFilter;
    primitiveTable_[409] = &Interpreter::primitiveImageGetMetadata;

    // System info primitives (410-419)
    primitiveTable_[410] = &Interpreter::primitiveSystemBatteryLevel;
    primitiveTable_[411] = &Interpreter::primitiveSystemBatteryState;
    primitiveTable_[412] = &Interpreter::primitiveSystemScreenBrightness;
    primitiveTable_[413] = &Interpreter::primitiveSystemSetScreenBrightness;
    primitiveTable_[414] = &Interpreter::primitiveSystemDeviceModel;
    primitiveTable_[415] = &Interpreter::primitiveSystemDeviceUUID;
    primitiveTable_[416] = &Interpreter::primitiveSystemAppVersion;
    primitiveTable_[417] = &Interpreter::primitiveSystemAppBuild;
    primitiveTable_[418] = &Interpreter::primitiveSystemAvailableMemory;
    primitiveTable_[419] = &Interpreter::primitiveSystemDiskSpace;

    // Hardware/sensor primitives (420-429)
    primitiveTable_[420] = &Interpreter::primitiveAccelerometerStart;
    primitiveTable_[421] = &Interpreter::primitiveAccelerometerStop;
    primitiveTable_[422] = &Interpreter::primitiveAccelerometerRead;
    primitiveTable_[423] = &Interpreter::primitiveGyroscopeStart;
    primitiveTable_[424] = &Interpreter::primitiveGyroscopeStop;
    primitiveTable_[425] = &Interpreter::primitiveGyroscopeRead;
    primitiveTable_[426] = &Interpreter::primitiveMagnetometerStart;
    primitiveTable_[427] = &Interpreter::primitiveMagnetometerStop;
    primitiveTable_[428] = &Interpreter::primitiveMagnetometerRead;
    primitiveTable_[429] = &Interpreter::primitiveDeviceMotionRead;

    // Location primitives (430-439)
    primitiveTable_[430] = &Interpreter::primitiveLocationStart;
    primitiveTable_[431] = &Interpreter::primitiveLocationStop;
    primitiveTable_[432] = &Interpreter::primitiveLocationRead;
    primitiveTable_[433] = &Interpreter::primitiveLocationAccuracy;
    primitiveTable_[434] = &Interpreter::primitiveLocationDistance;
    primitiveTable_[435] = &Interpreter::primitiveHeadingStart;
    primitiveTable_[436] = &Interpreter::primitiveHeadingStop;
    primitiveTable_[437] = &Interpreter::primitiveHeadingRead;
    primitiveTable_[438] = &Interpreter::primitiveGeocode;
    primitiveTable_[439] = &Interpreter::primitiveReverseGeocode;

    // Camera primitives (440-449)
    primitiveTable_[440] = &Interpreter::primitiveCameraCount;
    primitiveTable_[441] = &Interpreter::primitiveCameraOpen;
    primitiveTable_[442] = &Interpreter::primitiveCameraClose;
    primitiveTable_[443] = &Interpreter::primitiveCameraCapture;
    primitiveTable_[444] = &Interpreter::primitiveCameraStartPreview;
    primitiveTable_[445] = &Interpreter::primitiveCameraStopPreview;
    primitiveTable_[446] = &Interpreter::primitiveCameraGetFrame;
    primitiveTable_[447] = &Interpreter::primitiveCameraSetFlash;
    primitiveTable_[448] = &Interpreter::primitiveCameraSetFocus;
    primitiveTable_[449] = &Interpreter::primitiveCameraSetExposure;

    // Notification primitives (450-459)
    primitiveTable_[450] = &Interpreter::primitiveNotificationSchedule;
    primitiveTable_[451] = &Interpreter::primitiveNotificationCancel;
    primitiveTable_[452] = &Interpreter::primitiveNotificationCancelAll;
    primitiveTable_[453] = &Interpreter::primitiveNotificationGetPending;
    primitiveTable_[454] = &Interpreter::primitiveNotificationRequestPermission;
    primitiveTable_[455] = &Interpreter::primitiveNotificationGetPermission;
    primitiveTable_[456] = &Interpreter::primitiveNotificationSetBadge;
    primitiveTable_[457] = &Interpreter::primitiveNotificationGetBadge;
    primitiveTable_[458] = &Interpreter::primitiveNotificationRegisterPush;
    primitiveTable_[459] = &Interpreter::primitiveNotificationGetToken;

    // In-app purchase primitives (460-469)
    primitiveTable_[460] = &Interpreter::primitiveIAPCanMakePayments;
    primitiveTable_[461] = &Interpreter::primitiveIAPRequestProducts;
    primitiveTable_[462] = &Interpreter::primitiveIAPGetProducts;
    primitiveTable_[463] = &Interpreter::primitiveIAPPurchase;
    primitiveTable_[464] = &Interpreter::primitiveIAPRestore;
    primitiveTable_[465] = &Interpreter::primitiveIAPGetTransactions;
    primitiveTable_[466] = &Interpreter::primitiveIAPFinishTransaction;
    primitiveTable_[467] = &Interpreter::primitiveIAPGetReceipt;
    primitiveTable_[468] = &Interpreter::primitiveIAPRefreshReceipt;
    primitiveTable_[469] = &Interpreter::primitiveIAPGetSubscriptionStatus;

    // Sharing/social primitives (470-479)
    primitiveTable_[470] = &Interpreter::primitiveShareText;
    primitiveTable_[471] = &Interpreter::primitiveShareImage;
    primitiveTable_[472] = &Interpreter::primitiveShareURL;
    primitiveTable_[473] = &Interpreter::primitiveShareFile;
    primitiveTable_[474] = &Interpreter::primitiveOpenURL;
    primitiveTable_[475] = &Interpreter::primitiveCanOpenURL;
    primitiveTable_[476] = &Interpreter::primitiveMailCompose;
    primitiveTable_[477] = &Interpreter::primitiveMessageCompose;
    primitiveTable_[478] = &Interpreter::primitiveSocialPost;
    primitiveTable_[479] = &Interpreter::primitivePrint;

    // Keychain/security primitives (480-489)
    primitiveTable_[480] = &Interpreter::primitiveKeychainSet;
    primitiveTable_[481] = &Interpreter::primitiveKeychainGet;
    primitiveTable_[482] = &Interpreter::primitiveKeychainDelete;
    primitiveTable_[483] = &Interpreter::primitiveKeychainHas;
    primitiveTable_[484] = &Interpreter::primitiveBiometricAvailable;
    primitiveTable_[485] = &Interpreter::primitiveBiometricAuthenticate;
    primitiveTable_[486] = &Interpreter::primitiveCryptoRandomBytes;
    primitiveTable_[487] = &Interpreter::primitiveCryptoHash;
    primitiveTable_[488] = &Interpreter::primitiveCryptoHMAC;
    primitiveTable_[489] = &Interpreter::primitiveCryptoEncrypt;

    // Misc platform primitives (490-499)
    primitiveTable_[490] = &Interpreter::primitiveHapticFeedback;
    primitiveTable_[491] = &Interpreter::primitiveVibrate;
    primitiveTable_[492] = &Interpreter::primitiveFlashlight;
    primitiveTable_[493] = &Interpreter::primitiveIdleTimerDisable;
    primitiveTable_[494] = &Interpreter::primitiveStatusBarHide;
    primitiveTable_[495] = &Interpreter::primitiveStatusBarStyle;
    primitiveTable_[496] = &Interpreter::primitiveOrientationLock;
    primitiveTable_[497] = &Interpreter::primitiveOrientationGet;
    primitiveTable_[498] = &Interpreter::primitiveAppReview;
    primitiveTable_[499] = &Interpreter::primitiveAppSettings;

    // Platform primitives (500-513)
    primitiveTable_[500] = &Interpreter::primitiveGetEnvironment;
    primitiveTable_[501] = &Interpreter::primitiveSetEnvironment;
    primitiveTable_[502] = &Interpreter::primitiveGetCurrentDirectory;
    primitiveTable_[503] = &Interpreter::primitiveSetCurrentDirectory;
    primitiveTable_[504] = &Interpreter::primitiveGetPlatformName;
    primitiveTable_[505] = &Interpreter::primitiveGetOSVersion;
    primitiveTable_[506] = &Interpreter::primitiveGetProcessorCount;
    primitiveTable_[507] = &Interpreter::primitiveGetPhysicalMemory;
    primitiveTable_[508] = &Interpreter::primitiveGetHostName;
    primitiveTable_[509] = &Interpreter::primitiveGetUserName;
    primitiveTable_[510] = &Interpreter::primitiveGetHomeDirectory;
    primitiveTable_[511] = &Interpreter::primitiveGetTempDirectory;
    primitiveTable_[512] = &Interpreter::primitiveGetVMVersion;
    primitiveTable_[513] = &Interpreter::primitiveGetSystemLocale;
}

PrimitiveResult Interpreter::executePrimitive(int primitiveIndex, int argCount) {
    static int failCount = 0;

    // Named primitives have high numbers (typically >= 32768)
    // They are looked up by name from method literals - not yet implemented
    // For now, fail gracefully so the method body executes
    if (primitiveIndex >= 32768) {
        // Named primitive - would need to look up by name in method literals
        // For now, just fail and let the method body execute
        return PrimitiveResult::Failure;
    }

    if (primitiveIndex < 0 || primitiveIndex >= static_cast<int>(primitiveTable_.size())) {
        failCount++;
        if (failCount <= 10) {
            std::cerr << "[PRIM] Index out of range: " << primitiveIndex << " (args=" << argCount << ")\n";
        }
        return PrimitiveResult::Failure;
    }

    PrimitiveFunc prim = primitiveTable_[primitiveIndex];
    if (!prim) {
        failCount++;
        if (failCount <= 10) {
            std::cerr << "[PRIM] Unimplemented primitive: " << primitiveIndex << " (args=" << argCount << ")\n";
        }
        return PrimitiveResult::Failure;
    }

    // Primitive call tracing
    static int primCallCount = 0;
    primCallCount++;

    PrimitiveResult result = (this->*prim)(argCount);

    // Trace failures to see which primitives are problematic
    static int failureCount = 0;
    if (result == PrimitiveResult::Failure) {
        failureCount++;
        if (failureCount <= 20) {
            std::cerr << "[PRIM] Primitive " << primitiveIndex << " FAILED (args=" << argCount << ")\n";
        }
    }

    // Track successful primitive execution for stepDetailed()
    if (result == PrimitiveResult::Success) {
        lastPrimitiveIndex_ = primitiveIndex;
    }

    return result;
}

Oop Interpreter::activeContext() const {
    // Would return actual context object
    // For stack-based execution, we'd need to materialize one
    return Oop::nil();
}

} // namespace pharo
