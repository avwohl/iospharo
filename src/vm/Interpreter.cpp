/*
 * Interpreter.cpp - Bytecode Interpreter Implementation
 *
 * This implements the Sista V1 bytecode interpreter.
 */

#include "Interpreter.hpp"
#include "../platform/DisplaySurface.hpp"
#include <cstring>
#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>

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

void Interpreter::renderWorldMorphs() {
    // Render World's morphs directly to the platform display
    // This bypasses NullWorldRenderer and draws morphs ourselves
    if (!pharo::gDisplaySurface) return;

    static int totalMorphsDrawn = 0;

    // Find the World global
    Oop world = memory_.findGlobal("World");
    if (world.isNil() || !world.isObject()) return;

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

        // Color stores RGB in slot 0 (may be SmallInteger or Float-based)
        Oop rgb = memory_.fetchPointer(0, colorObj);
        if (rgb.isSmallInteger()) {
            int rgbVal = static_cast<int>(rgb.asSmallInteger());
            return 0xFF000000 | (rgbVal & 0xFFFFFF);
        }
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

        x1 = originX.isSmallInteger() ? static_cast<int>(originX.asSmallInteger()) : 0;
        y1 = originY.isSmallInteger() ? static_cast<int>(originY.asSmallInteger()) : 0;
        x2 = cornerX.isSmallInteger() ? static_cast<int>(cornerX.asSmallInteger()) : 0;
        y2 = cornerY.isSmallInteger() ? static_cast<int>(cornerY.asSmallInteger()) : 0;

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

    // Recursive morph rendering function
    std::function<void(Oop, int, int)> renderMorph = [&](Oop morph, int depth, int index) {
        if (morph.isNil() || !morph.isObject()) return;
        if (depth > 20) return;  // Prevent infinite recursion

        totalMorphsDrawn++;

        // Get bounds
        int x1, y1, x2, y2;
        if (!extractBounds(morph, x1, y1, x2, y2)) return;

        // Get color
        Oop morphColor = memory_.fetchPointer(4, morph);
        uint32_t colorARGB = extractColor(morphColor);

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
    for (int i = 0; i < dispWidth * dispHeight; i++) {
        pixels[i] = worldColorARGB;
    }

    totalMorphsDrawn = 0;

    // Get World's submorphs and render recursively
    Oop submorphs = memory_.fetchPointer(2, world);
    if (!submorphs.isNil() && submorphs.isObject()) {
        ObjectHeader* subHdr = submorphs.asObjectPtr();
        size_t numSubmorphs = subHdr->slotCount();

        for (size_t i = 0; i < numSubmorphs; i++) {
            Oop submorph = subHdr->slotAt(i);
            renderMorph(submorph, 0, static_cast<int>(i));
        }
    }

    pharo::gDisplaySurface->update();
}

// ===== MAIN LOOP =====

void Interpreter::interpret() {
    while (running_) {
        // Process any pending external semaphore signals
        if (hasPendingSignals()) {
            processPendingSignals();
        }
        step();
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

    // Check if we've run past the end of bytecodes
    if (instructionPointer_ >= bytecodeEnd_) {
        returnValue(receiver_);
        return running_;
    }

    // NOTE: Do NOT reset extA_/extB_ here!
    // In Sista V1, extension bytecodes (0xE0/0xE1) set these values, then the
    // NEXT bytecode uses them. The consuming bytecodes reset them after use.
    // Resetting here would break extension byte chains.

    // Bytecode step counter for periodic updates
    static int stepCount = 0;
    stepCount++;

    // Periodic display update - trigger every 10000 steps to ensure rendering happens
    // even if NullWorldRenderer doesn't call displayWorldStateOf:during:
    if (stepCount % 10000 == 0 && pharo::gDisplaySurface) {
        pharo::gDisplaySurface->update();
    }

    uint8_t bytecode = fetchByte();
    dispatchBytecode(bytecode);

    return running_;
}

ExecuteResult Interpreter::stepDetailed() {
    if (!running_) {
        return ExecuteResult::Idle;
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
        // Trace if selector is a CompiledBlock (likely executing wrong bytecodes)
        if (selector.isObject() && selector.asObjectPtr()->classIndex() == 3117) {
            static int wrongBcCount = 0;
            wrongBcCount++;
            if (wrongBcCount <= 3) {
                std::cerr << "[BYTECODE] Send 0x" << std::hex << (int)bytecode << " litIndex=" << litIndex
                          << " got CompiledBlock!\n"
                          << "  method_=0x" << method_.rawBits() << " homeMethod_=0x" << homeMethod_.rawBits()
                          << " IP offset=" << std::dec << (instructionPointer_ - 1 - method_.asObjectPtr()->bytes()) << "\n";
            }
        }
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
        std::cerr << "[VM] Trap bytecode (0xD9), setting running_=false\n";
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
                // Super send: lookup from superclass of method's defining class
                Oop receiverClass = memory_.classOf(receiver_);
                Oop superclass = superclassOf(receiverClass);
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
                // Get temp vector from outer context
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
        std::cerr << "[VM] Stack overflow in push(), setting running_=false\n";
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

    // Super send starts lookup from superclass
    Oop selector = literal(literalIndex);
    Oop receiverClass = memory_.classOf(receiver_);
    Oop superclass = superclassOf(receiverClass);

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
            // For other arithmetic ops, look up the literal
            selector = literal(which);
            break;
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
    // Message send tracing
    static int sendCount = 0;
    sendCount++;
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
                    // Trace the source
                    static int garbageCount = 0;
                    garbageCount++;
                    if (garbageCount <= 5) {
                        std::cerr << "[VM] GARBAGE selector: method=0x" << std::hex << method_.rawBits()
                                  << " homeMethod=0x" << homeMethod_.rawBits()
                                  << " selector=0x" << selector.rawBits() << std::dec
                                  << " cls=" << selHdr->classIndex() << " len=" << len << "\n";
                    }
                }
            } else {
                selName = "<non-bytes-" + std::to_string(selHdr->classIndex()) + ">";
                // Trace when we get a CompiledBlock (3117) as selector - likely wrong literal access
                if (selHdr->classIndex() == 3117) {
                    static int blockSelCount = 0;
                    blockSelCount++;
                    if (blockSelCount <= 5) {
                        std::cerr << "[VM] CompiledBlock as selector: selector=0x" << std::hex << selector.rawBits()
                                  << " method=0x" << method_.rawBits()
                                  << " homeMethod=0x" << homeMethod_.rawBits() << std::dec;
                        // Show method's numLiterals
                        if (method_.isObject()) {
                            Oop mHdr = memory_.fetchPointer(0, method_);
                            if (mHdr.isSmallInteger()) {
                                int64_t hBits = mHdr.asSmallInteger();
                                std::cerr << " method_numLits=" << (hBits & 0x7FFF);
                            }
                        }
                        std::cerr << "\n";
                    }
                }
            }
        } else if (selector.isNil() || selector.rawBits() == 0) {
            selName = "<nil>";
        }
        std::cerr << "[VM] sendSelector #" << sendCount << ": #" << selName << " args=" << argCount;

        // Show receiver class for first few sends
        if (sendCount <= 50) {
            Oop rcvrTrace = stackValue(argCount);
            if (rcvrTrace.isObject()) {
                Oop rcvrClassTrace = memory_.classOf(rcvrTrace);
                if (rcvrClassTrace.isObject()) {
                    ObjectHeader* clsHdr = rcvrClassTrace.asObjectPtr();
                    if (clsHdr->slotCount() > 6) {
                        Oop className = memory_.fetchPointer(6, rcvrClassTrace);
                        if (className.isObject()) {
                            ObjectHeader* nameHdr = className.asObjectPtr();
                            if (nameHdr->isBytesObject() && nameHdr->byteSize() <= 50) {
                                std::cerr << " rcvr=" << std::string((char*)nameHdr->bytes(), nameHdr->byteSize());
                            }
                        }
                    }
                }
            } else if (rcvrTrace.isSmallInteger()) {
                std::cerr << " rcvr=SmallInt(" << rcvrTrace.asSmallInteger() << ")";
            }
        }
        std::cerr << "\n";
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
                selStr == "doTerminationFromYourself" || selStr == "terminate" ||
                selStr == "primTerminate" || selStr == "primitiveTerminateTo" ||
                selStr == "terminateTo:") {
                static int termInterceptCount = 0;
                termInterceptCount++;
                if (termInterceptCount <= 10) {
                    std::cerr << "[VM] INTERCEPT #" << selStr << " - preventing termination during startup\n";
                }
                // Pop args and receiver, push receiver back (return self)
                popN(argCount + 1);
                push(rcvr);
                return;
            }

            // ===== INTERCEPT NullWorldRenderer DISPLAY METHODS =====
            // Make NullWorldRenderer actually render to our display surface
            if (selStr == "displayWorldStateOf:during:" && argCount == 2) {
                // Check if receiver is NullWorldRenderer
                Oop rcvrClass = memory_.classOf(rcvr);
                if (rcvrClass.isObject()) {
                    Oop className = memory_.fetchPointer(6, rcvrClass);  // Class name at slot 6
                    if (className.isObject()) {
                        ObjectHeader* nameHdr = className.asObjectPtr();
                        if (nameHdr->isBytesObject() && nameHdr->byteSize() < 50) {
                            std::string rcvrClassName((char*)nameHdr->bytes(), nameHdr->byteSize());
                            if (rcvrClassName == "NullWorldRenderer") {
                                static bool firstIntercept = true;
                                if (firstIntercept) {
                                    std::cerr << "[VM] INTERCEPT displayWorldStateOf:during: on NullWorldRenderer\n";
                                    firstIntercept = false;
                                }

                                // Get the arguments: world and block
                                Oop drawBlock = stackValue(0);  // The drawing block
                                Oop worldArg = stackValue(1);   // The world

                                // If we have a display form, execute the block with its canvas
                                if (!displayForm_.isNil() && displayForm_.isObject()) {
                                    // Create or get canvas for the display form
                                    // For now, just trigger a display update
                                    if (pharo::gDisplaySurface) {
                                        pharo::gDisplaySurface->update();
                                    }
                                }

                                // Pop args and receiver, push receiver (return self)
                                popN(argCount + 1);
                                push(rcvr);
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
                                    std::cerr << "[VM] INTERCEPT checkForNewScreenSize - setting up display\n";
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

    // Check for primitive
    int primIndex = primitiveIndexOf(method);
    if (primIndex > 0) {
        argCount_ = argCount;
        primitiveFailed_ = false;
        PrimitiveResult result = executePrimitive(primIndex, argCount);
        if (result == PrimitiveResult::Success) {
            return;
        }
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
        if (depth < 5 && lookupDebugCount < 0) {  // Disabled
            lookupDebugCount++;
            std::cerr << "[LOOKUP] depth=" << depth << " class=" << className << " (0x" << std::hex << currentClass.rawBits()
                      << std::dec << " clsIdx=" << clsHdr->classIndex() << " slots=" << clsHdr->slotCount()
                      << ") md=0x" << std::hex << methodDict.rawBits() << std::dec << std::endl;
        }
        if (!isNilOrEnd(methodDict) && methodDict.isObject()) {
            Oop method = lookupInMethodDict(methodDict, selector);
            if (!isNilOrEnd(method) && method.isObject()) {
                // DEBUG_LOG("[LOOKUP] Found method=0x" << std::hex << method.rawBits() << std::dec;
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
            // if (shouldDebug) { ... }
            // Return corresponding method from valuesArray
            if (i < valuesSize) {
                return memory_.fetchPointer(i, valuesArray);
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
                // DEBUG: "[MD] Found method at slot " << i << " for selector \"" << selectorStr << "\""
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
                    return memory_.fetchPointer(i, valuesArray);
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
    // BlockClosure layout:
    // 0: outerContext
    // 1: startPC (SmallInteger)
    // 2: numArgs (SmallInteger)
    // 3+: copied values

    Oop startPC = memory_.fetchPointer(1, block);
    if (!startPC.isSmallInteger()) {
        primitiveFail();
        return;
    }

    // Get the method from outer context
    Oop outerContext = memory_.fetchPointer(0, block);
    Oop outerMethod = memory_.fetchPointer(3, outerContext);

    pushFrame(outerMethod, argCount);

    method_ = outerMethod;
    homeMethod_ = outerMethod;  // Home method from outer context for literal access
    argCount_ = argCount;
    receiver_ = memory_.fetchPointer(5, outerContext);  // Receiver from outer

    ObjectHeader* methodObj = method_.asObjectPtr();
    instructionPointer_ = methodObj->bytes() + startPC.asSmallInteger();
}

// ===== FRAME MANAGEMENT =====

void Interpreter::pushFrame(Oop method, int argCount) {
    static int pushCount = 0;
    pushCount++;

    // Save current execution state before switching to new method
    if (frameDepth_ >= MaxFrameDepth) {
        std::cerr << "[VM] Frame depth overflow (" << MaxFrameDepth << ") after " << pushCount << " pushes, setting running_=false\n";
        // Show the method that caused overflow
        if (method.isObject()) {
            Oop classRef = memory_.fetchPointer(0, method);  // method header or class
            std::cerr << "[VM] Last method causing overflow: 0x" << std::hex << method.rawBits() << std::dec << "\n";
        }
        running_ = false;
        return;
    }

    // Trace first few pushes
    if (pushCount <= 20) {
        std::cerr << "[VM] pushFrame #" << pushCount << " depth=" << frameDepth_ << " argCount=" << argCount << "\n";
    }

    SavedFrame& frame = savedFrames_[frameDepth_++];
    frame.savedIP = instructionPointer_;
    frame.savedBytecodeEnd = bytecodeEnd_;
    frame.savedMethod = method_;
    frame.savedHomeMethod = homeMethod_;
    frame.savedReceiver = receiver_;
    frame.savedFP = framePointer_;
    frame.savedArgCount = argCount_;

    // DEBUG_LOG("[FRAME] Push frame #" << frameDepth_ << " savedIP=" << (void*)frame.savedIP;

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
    static int popCount = 0;
    popCount++;

    // Restore previous execution state
    if (frameDepth_ == 0) {
        std::cerr << "[VM] popFrame #" << popCount << ": frameDepth already 0, setting running_=false\n";
        running_ = false;
        return;
    }

    if (popCount <= 20) {
        std::cerr << "[VM] popFrame #" << popCount << " depth=" << frameDepth_ << "\n";
    }

    --frameDepth_;
    SavedFrame& frame = savedFrames_[frameDepth_];

    // DEBUG_LOG("[FRAME] Pop frame #" << (frameDepth_ + 1) << " restoring IP=" << (void*)frame.savedIP;

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
        std::cerr << "[VM] popFrame: last frame with savedIP=null, setting running_=false\n";
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
    // Temporaries are after args in the frame
    return *(framePointer_ + argCount_ + 1 + index);
}

void Interpreter::setTemporary(int index, Oop value) {
    *(framePointer_ + argCount_ + 1 + index) = value;
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
        std::cerr << "[VM] DNU depth exceeded " << MAX_DNU_DEPTH << ", setting running_=false\n";
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
            // std::cerr << "[DNU] Original selector string: '#" << origStr << "'"; // DEBUG
        }
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
        std::cerr << "[VM] DNU for #doesNotUnderstand itself, setting running_=false\n";
        running_ = false;
        dnuDepth = 0;
        return;
    }

    // Get the actual receiver that failed (from stack, under args)
    Oop failedReceiver = stackValue(argCount);

    // Trace DNU with class info
    static int dnuTraceCount = 0;
    dnuTraceCount++;
    if (dnuTraceCount <= 10) {
        std::cerr << "[DNU #" << dnuTraceCount << "] ";
        // Print selector
        if (selector.isObject() && selector.rawBits() > 0x10000) {
            ObjectHeader* selHdr = selector.asObjectPtr();
            if (selHdr->isBytesObject() && selHdr->byteSize() <= 50) {
                std::cerr << "#" << std::string((char*)selHdr->bytes(), selHdr->byteSize());
            }
        }
        std::cerr << " args=" << argCount;
        // Print receiver class
        if (failedReceiver.isObject()) {
            Oop rcvrClass = memory_.classOf(failedReceiver);
            if (rcvrClass.isObject()) {
                ObjectHeader* clsHdr = rcvrClass.asObjectPtr();
                if (clsHdr->slotCount() > 6) {
                    Oop className = memory_.fetchPointer(6, rcvrClass);
                    if (className.isObject()) {
                        ObjectHeader* nameHdr = className.asObjectPtr();
                        if (nameHdr->isBytesObject() && nameHdr->byteSize() <= 50) {
                            std::cerr << " receiver=" << std::string((char*)nameHdr->bytes(), nameHdr->byteSize());
                        }
                    }
                }
            }
        } else if (failedReceiver.isSmallInteger()) {
            std::cerr << " receiver=SmallInteger(" << failedReceiver.asSmallInteger() << ")";
        }
        std::cerr << "\n";
    }

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
    return memory_.fetchPointer(0, classOop);
}

Oop Interpreter::methodDictOf(Oop classOop) const {
    // Class layout: methodDict is slot 1
    return memory_.fetchPointer(1, classOop);
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
    Oop blockClass = memory_.specialObject(SpecialObjectIndex::ClassBlockClosure);
    size_t slots = 3 + numCopied;  // outerContext, compiledBlock, numArgs, copied...
    Oop block = memory_.allocateSlots(
        memory_.indexOfClass(blockClass), slots, ObjectFormat::Indexable);

    // Set fields
    memory_.storePointer(0, block, activeContext_);  // outerContext
    memory_.storePointer(1, block, compiledBlock);   // compiledBlock (instead of startPC)

    // Get numArgs from the CompiledBlock's header (first slot) if possible
    int numArgs = 0;
    if (compiledBlock.isObject()) {
        Oop methodHeader = memory_.fetchPointer(0, compiledBlock);
        if (methodHeader.isSmallInteger()) {
            int64_t headerBits = methodHeader.asSmallInteger();
            numArgs = headerBits & 0x0F;  // numArgs in low bits
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
    std::cerr << "[VM] addLastLinkToList: process=0x" << std::hex << process.rawBits()
              << " list=0x" << list.rawBits() << std::dec << "\n";
    std::cerr.flush();

    // Validate inputs
    if (!process.isObject() || !list.isObject()) {
        std::cerr << "[VM] addLastLinkToList: Invalid process or list!\n";
        std::cerr.flush();
        return;
    }

    ObjectHeader* procHdr = process.asObjectPtr();
    ObjectHeader* listHdr = list.asObjectPtr();

    std::cerr << "[VM] addLastLinkToList: process slots=" << procHdr->slotCount()
              << " list slots=" << listHdr->slotCount() << "\n";
    std::cerr.flush();

    // Verify process has enough slots for Process layout
    if (procHdr->slotCount() < 4) {
        std::cerr << "[VM] addLastLinkToList: Process doesn't have enough slots!\n";
        std::cerr.flush();
        return;
    }

    Oop nilObj = memory_.nil();

    std::cerr << "[VM] addLastLinkToList: Setting process.nextLink = nil\n";
    std::cerr.flush();

    // Set process.nextLink = nil (it's the last one)
    memory_.storePointer(ProcessNextLinkIndex, process, nilObj);

    std::cerr << "[VM] addLastLinkToList: Setting process.myList = list\n";
    std::cerr.flush();

    // Set process.myList = list
    memory_.storePointer(ProcessMyListIndex, process, list);

    std::cerr << "[VM] addLastLinkToList: Checking if list is empty\n";
    std::cerr.flush();

    // Check if list is empty
    Oop firstLink = memory_.fetchPointer(LinkedListFirstLinkIndex, list);

    std::cerr << "[VM] addLastLinkToList: firstLink=0x" << std::hex << firstLink.rawBits() << std::dec << "\n";
    std::cerr.flush();

    if (firstLink.isNil() || firstLink.rawBits() == nilObj.rawBits()) {
        std::cerr << "[VM] addLastLinkToList: List is empty, setting firstLink\n";
        std::cerr.flush();
        // Empty list - process becomes both first and last
        memory_.storePointer(LinkedListFirstLinkIndex, list, process);
    } else {
        std::cerr << "[VM] addLastLinkToList: List not empty, appending\n";
        std::cerr.flush();
        // Non-empty list - append to last element
        Oop lastLink = memory_.fetchPointer(LinkedListLastLinkIndex, list);
        memory_.storePointer(ProcessNextLinkIndex, lastLink, process);
    }

    std::cerr << "[VM] addLastLinkToList: Setting lastLink\n";
    std::cerr.flush();

    memory_.storePointer(LinkedListLastLinkIndex, list, process);

    std::cerr << "[VM] addLastLinkToList: Done!\n";
    std::cerr.flush();
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
    std::cerr << "[VM] transferTo: newProcess=0x" << std::hex << newProcess.rawBits() << std::dec << "\n";
    std::cerr.flush();

    Oop oldProcess = getActiveProcess();
    std::cerr << "[VM] transferTo: oldProcess=0x" << std::hex << oldProcess.rawBits() << std::dec << "\n";
    std::cerr.flush();

    if (oldProcess.rawBits() == newProcess.rawBits()) {
        std::cerr << "[VM] transferTo: same process, returning\n";
        std::cerr.flush();
        return;  // Already running this process
    }

    // Validate newProcess
    if (!newProcess.isObject()) {
        std::cerr << "[VM] transferTo: newProcess is not an object!\n";
        std::cerr.flush();
        return;
    }

    ObjectHeader* newProcHdr = newProcess.asObjectPtr();
    std::cerr << "[VM] transferTo: newProcess slots=" << newProcHdr->slotCount() << "\n";
    std::cerr.flush();

    if (newProcHdr->slotCount() < 2) {
        std::cerr << "[VM] transferTo: newProcess has too few slots!\n";
        std::cerr.flush();
        return;
    }

    // Save current execution state to old process's suspendedContext
    // For now, we rely on activeContext_ being updated appropriately
    // The context should already reflect current execution state
    std::cerr << "[VM] transferTo: Saving context to old process\n";
    std::cerr.flush();

    if (!activeContext_.isNil() && activeContext_.isObject()) {
        memory_.storePointer(ProcessSuspendedContextIndex, oldProcess, activeContext_);
    }

    // Switch to new process
    std::cerr << "[VM] transferTo: Setting active process\n";
    std::cerr.flush();
    setActiveProcess(newProcess);

    // Get new process's suspended context
    std::cerr << "[VM] transferTo: Getting new context\n";
    std::cerr.flush();
    Oop newContext = memory_.fetchPointer(ProcessSuspendedContextIndex, newProcess);
    std::cerr << "[VM] transferTo: newContext=0x" << std::hex << newContext.rawBits() << std::dec << "\n";
    std::cerr.flush();

    // Reset interpreter state
    stackPointer_ = stackBase_;
    frameDepth_ = 0;

    // Resume execution from the new context
    std::cerr << "[VM] transferTo: Calling executeFromContext\n";
    std::cerr.flush();
    executeFromContext(newContext);
    std::cerr << "[VM] transferTo: Done!\n";
    std::cerr.flush();
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
    static int bootCount = 0;
    bootCount++;
    std::cerr << "[VM] bootstrapStartup called #" << bootCount << "\n";
    std::cerr.flush();

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
                            std::cerr << "[VM] bootstrapStartup: Found context=0x" << std::hex << context.rawBits()
                                      << " at priority " << std::dec << (i + 1) << "\n";
                            std::cerr.flush();
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
        std::cerr << "[VM] bootstrapStartup: Initializing platform display...\n";
        std::cerr.flush();

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
        std::cerr << "[VM] bootstrapStartup: Platform display initialized with pattern\n";
        std::cerr.flush();

        // Try to find and activate the Display Form from the image
        // This connects Pharo's Display object to our display surface
        std::cerr << "[VM] bootstrapStartup: Looking for Display global...\n";
        std::cerr.flush();
        Oop displayObj = memory_.findGlobal("Display");
        std::cerr << "[VM] bootstrapStartup: findGlobal returned 0x" << std::hex << displayObj.rawBits() << std::dec << "\n";
        std::cerr.flush();
        if (!displayObj.isNil() && displayObj.isObject()) {
            std::cerr << "[VM] bootstrapStartup: Found Display object, calling beDisplay\n";
            // Call beDisplay on it (primitive 126)
            setDisplayForm(displayObj);

            // Get the Form's dimensions and update our screen size
            // Form slots: 0=bits, 1=width, 2=height, 3=depth
            Oop widthOop = memory_.fetchPointer(1, displayObj);
            Oop heightOop = memory_.fetchPointer(2, displayObj);
            if (widthOop.isSmallInteger() && heightOop.isSmallInteger()) {
                int formWidth = static_cast<int>(widthOop.asSmallInteger());
                int formHeight = static_cast<int>(heightOop.asSmallInteger());
                std::cerr << "[VM] bootstrapStartup: Display Form is " << formWidth << "x" << formHeight << "\n";
            }
        } else {
            std::cerr << "[VM] bootstrapStartup: Display object not found\n";
        }
    }

    // If we've already tried many startup attempts, eventually give up.
    // But allow many more attempts for the UI loop to run.
    if (startupAttempt > 1000) {
        std::cerr << "[VM] bootstrapStartup: exceeded 1000 attempts, setting running_=false\n";
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
        std::cerr << "[VM] bootstrapStartup: Approach 3 - Attempt 1 recordStartupStamp\n";
        std::cerr.flush();

        Oop smalltalkImage = memory_.findGlobal("SmalltalkImage");
        if (smalltalkImage.isObject()) {
            // Look up method directly from SmalltalkImage's methodDict
            Oop method = lookupMethodInClass(smalltalkImage, "recordStartupStamp");
            if (!method.isNil() && method.isObject()) {
                // Create a receiver - the singleton SmalltalkImage current
                // For now, use nil as receiver (recordStartupStamp may not need self)
                // Actually, we need an instance of SmalltalkImage
                // SmalltalkImage current returns the singleton
                // Let's try calling on nil first
                Oop context = memory_.createStartupContext(method, memory_.nil());
                std::cerr << "[VM] bootstrapStartup: createStartupContext returned 0x"
                          << std::hex << context.rawBits() << std::dec << "\n";
                std::cerr.flush();
                if (!context.isNil()) {
                    stackPointer_ = stackBase_;
                    frameDepth_ = 0;
                    std::cerr << "[VM] bootstrapStartup: Calling executeFromContext...\n";
                    std::cerr.flush();
                    if (executeFromContext(context)) {
                        std::cerr << "[VM] bootstrapStartup: executeFromContext succeeded\n";
                        std::cerr.flush();
                        return true;
                    }
                }
            } else {
                // DEBUG: "[DEBUG] Method recordStartupStamp not found in SmalltalkImage"
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
        std::cerr << "[VM] bootstrapStartup: Attempt " << startupAttempt << " - Looking for World...\n";
        std::cerr.flush();

        // Find World class - Note: World is a global that holds the current WorldMorph
        Oop world = memory_.findGlobal("World");
        std::cerr << "[VM] bootstrapStartup: findGlobal(World) returned 0x" << std::hex
                  << world.rawBits() << std::dec << "\n";
        std::cerr.flush();

        // Check for OSWindowWorldRenderer class
        Oop osRenderer = memory_.findGlobal("OSWindowWorldRenderer");
        std::cerr << "[VM] bootstrapStartup: OSWindowWorldRenderer = 0x" << std::hex
                  << osRenderer.rawBits() << std::dec
                  << (osRenderer.isNil() ? " (NOT FOUND)" : " (FOUND)") << "\n";
        std::cerr.flush();

        if (!world.isNil() && world.isObject()) {
            std::cerr << "[VM] bootstrapStartup: Found World instance (not class)\n";
            std::cerr.flush();

            // Check World object structure
            ObjectHeader* worldHdr = world.asObjectPtr();
            uint32_t worldClassIdx = worldHdr->classIndex();
            std::cerr << "[VM] bootstrapStartup: World classIndex = " << worldClassIdx
                      << " slots=" << worldHdr->slotCount() << " format=" << (int)worldHdr->format() << "\n";
            std::cerr.flush();

            // World is an instance of WorldMorph - get its class for method lookup
            Oop worldClass = memory_.classOf(world);
            std::cerr << "[VM] bootstrapStartup: World class (from classOf) = 0x" << std::hex
                      << worldClass.rawBits() << std::dec << "\n";
            std::cerr.flush();

            // Try getting class directly from class table
            Oop worldClassDirect = memory_.classAtIndex(worldClassIdx);
            std::cerr << "[VM] bootstrapStartup: World class (from classAtIndex) = 0x" << std::hex
                      << worldClassDirect.rawBits() << std::dec << "\n";
            std::cerr.flush();

            // Debug: check class table around this index
            std::cerr << "[VM] Class table check around index " << worldClassIdx << ":\n";
            for (uint32_t i = std::max(0u, worldClassIdx - 5); i < worldClassIdx + 10; ++i) {
                Oop cls = memory_.classAtIndex(i);
                if (!cls.isNil() && cls.rawBits() != 0) {
                    std::cerr << "  [" << i << "]: 0x" << std::hex << cls.rawBits() << std::dec << "\n";
                }
            }
            std::cerr.flush();

            // If classOf failed, try looking up WorldMorph by name
            if (worldClass.isNil() || worldClass.rawBits() == 0) {
                std::cerr << "[VM] bootstrapStartup: classOf failed, trying to find WorldMorph class by name\n";
                std::cerr.flush();
                worldClass = memory_.findGlobal("WorldMorph");
                std::cerr << "[VM] bootstrapStartup: WorldMorph = 0x" << std::hex
                          << worldClass.rawBits() << std::dec << "\n";
                std::cerr.flush();
            }

            // Try to call doOneCycle on the World instance
            Oop method = lookupMethodInClass(worldClass, "doOneCycle");
            if (!method.isNil() && method.isObject()) {
                std::cerr << "[VM] bootstrapStartup: Found doOneCycle method\n";
                std::cerr.flush();
                Oop context = memory_.createStartupContext(method, world);  // Pass instance, not class
                if (!context.isNil()) {
                    stackPointer_ = stackBase_;
                    frameDepth_ = 0;
                    if (executeFromContext(context)) {
                        std::cerr << "[VM] bootstrapStartup: Started World>>doOneCycle\n";
                        return true;
                    }
                }
            } else {
                std::cerr << "[VM] bootstrapStartup: doOneCycle method not found in class\n";
                std::cerr.flush();
            }
        } else {
            std::cerr << "[VM] bootstrapStartup: World not found (nil or not object)\n";
            std::cerr.flush();
        }

        // Also try UIManager
        Oop uiManager = memory_.findGlobal("UIManager");
        std::cerr << "[VM] bootstrapStartup: UIManager = 0x" << std::hex
                  << uiManager.rawBits() << std::dec << "\n";
        std::cerr.flush();
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
    // DEBUG: "[DEBUG] executeFromContext: Setting up execution state..."

    // Immediate tracing to catch crash
    static int execCtxCount = 0;
    execCtxCount++;
    std::cerr << "[VM] executeFromContext ENTER #" << execCtxCount
              << " context=0x" << std::hex << context.rawBits() << std::dec << "\n";
    std::cerr.flush();

    if (context.isNil()) {
        std::cerr << "[VM] executeFromContext: context is nil!\n";
        std::cerr.flush();
        return false;
    }

    if (!context.isObject()) {
        std::cerr << "[VM] executeFromContext: context is not an object (isSmallInt="
                  << context.isSmallInteger() << ")!\n";
        std::cerr.flush();
        return false;
    }

    // Get the raw pointer and validate it before dereferencing
    uintptr_t rawPtr = context.rawBits();
    std::cerr << "[VM] executeFromContext: rawPtr=0x" << std::hex << rawPtr << std::dec << "\n";
    std::cerr.flush();

    // Quick sanity check - the ACTUAL object address (clearing low 3 bits for space tag) should be aligned
    // In Spur, low 3 bits encode the memory space: 000=Old, 010=New, 100=Perm
    uintptr_t actualAddr = rawPtr & ~0x7ULL;
    if (actualAddr < 0x1000) {
        std::cerr << "[VM] executeFromContext: BAD pointer (address too small: 0x"
                  << std::hex << actualAddr << std::dec << ")!\n";
        std::cerr.flush();
        return false;
    }

    std::cerr << "[VM] executeFromContext: About to dereference context...\n";
    std::cerr.flush();

    // Context layout:
    // slot 0: sender
    // slot 1: pc (1-based byte offset into method bytes)
    // slot 2: stackp (index of top of stack within context, 0 means empty)
    // slot 3: method
    // slot 4: closureOrNil
    // slot 5: receiver
    // slot 6+: temps and stack values

    // Dump context structure first
    ObjectHeader* ctxHeader = context.asObjectPtr();
    std::cerr << "[VM] executeFromContext: ctxHeader=" << (void*)ctxHeader << "\n";
    std::cerr.flush();

    // Read header fields carefully
    std::cerr << "[VM] executeFromContext: Reading slotCount...\n";
    std::cerr.flush();
    size_t slotCount = ctxHeader->slotCount();
    std::cerr << "[VM] executeFromContext: slotCount=" << slotCount << "\n";
    std::cerr.flush();

    std::cerr << "[VM] executeFromContext: Reading classIndex...\n";
    std::cerr.flush();
    uint32_t clsIdx = ctxHeader->classIndex();
    std::cerr << "[VM] executeFromContext: classIndex=" << clsIdx << "\n";
    std::cerr.flush();

    if (slotCount < 6) {
        std::cerr << "[VM] executeFromContext: Context has too few slots! Expected >=6, got " << slotCount << "\n";
        std::cerr.flush();
        return false;
    }

    // DEBUG_LOG("[DEBUG] executeFromContext: Context has " << ctxHeader->slotCount() << " slots, cls=" << ctxHeader->classIndex();
    for (size_t i = 0; i < std::min(slotCount, (size_t)12); i++) {
        Oop slot = ctxHeader->slotAt(i);
        // DEBUG_LOG("[DEBUG]   ctx slot[" << i << "] = 0x" << std::hex << slot.rawBits() << std::dec;
        // if (slot.isNil()) std::cerr << " (nil)";
        // else if (slot.isSmallInteger()) std::cerr << " (SmallInt: " << slot.asSmallInteger() << ")";
        // else if (slot.isObject()) {
        //     ObjectHeader* h = slot.asObjectPtr();
        //     std::cerr << " (obj: " << h->slotCount() << " slots, cls=" << h->classIndex() << ", fmt=" << (int)h->format() << ")";
        // }
        // std::cerr; // DEBUG
    }

    std::cerr << "[VM] executeFromContext: Fetching method and receiver...\n";
    std::cerr.flush();
    method_ = memory_.fetchPointer(3, context);
    std::cerr << "[VM] executeFromContext: method_=0x" << std::hex << method_.rawBits() << std::dec << "\n";
    std::cerr.flush();
    receiver_ = memory_.fetchPointer(5, context);
    std::cerr << "[VM] executeFromContext: receiver_=0x" << std::hex << receiver_.rawBits() << std::dec << "\n";
    std::cerr.flush();

    // Check for and fix unrelocated pointers (old image base 0x10000000000+)
    // The old Spur 64-bit image base is 0x10000000000 (1TB)
    const uint64_t OLD_IMAGE_BASE = 0x10000000000ULL;
    uint64_t newBase = reinterpret_cast<uint64_t>(memory_.oldSpaceStart());

    uint64_t methodAddr = method_.rawBits() & ~7ULL;
    uint64_t receiverAddr = receiver_.rawBits() & ~7ULL;

    bool methodUnrelocated = (methodAddr >= OLD_IMAGE_BASE && methodAddr < OLD_IMAGE_BASE * 2);
    bool receiverUnrelocated = (receiverAddr >= OLD_IMAGE_BASE && receiverAddr < OLD_IMAGE_BASE * 2);

    if (methodUnrelocated || receiverUnrelocated) {
        static int fixCount = 0;
        fixCount++;

        // Fix method pointer if needed
        if (methodUnrelocated) {
            uint64_t offset = methodAddr - OLD_IMAGE_BASE;
            uint64_t newAddr = newBase + offset;
            ObjectHeader* newMethodPtr = reinterpret_cast<ObjectHeader*>(newAddr);
            method_ = memory_.oopFromPointer(newMethodPtr);

            if (fixCount <= 5) {
                std::cerr << "[VM] FIX: method_ relocated from 0x" << std::hex << methodAddr
                          << " to 0x" << newAddr << std::dec << "\n";
            }

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

            if (fixCount <= 5) {
                std::cerr << "[VM] FIX: receiver_ relocated from 0x" << std::hex << receiverAddr
                          << " to 0x" << newAddr << std::dec << "\n";
            }

            // Also fix the context slot
            ObjectHeader* ctxHdr = context.asObjectPtr();
            ctxHdr->slotAtPut(5, receiver_);
        }

        if (fixCount <= 5) {
            std::cerr << "[VM] executeFromContext: Fixed unrelocated pointers in context (fix #" << fixCount << ")\n";
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
        static int senderFixCount = 0;
        senderFixCount++;
        if (senderFixCount <= 3) {
            std::cerr << "[VM] FIX: sender relocated from 0x" << std::hex << senderAddr
                      << " to 0x" << newAddr << std::dec << "\n";
        }
    }

    // Trace sender slot for debugging
    Oop nilObjTrace = memory_.specialObject(SpecialObjectIndex::NilObject);
    std::cerr << "[VM] executeFromContext: sender=0x" << std::hex << sender.rawBits()
              << " (nilObj=0x" << nilObjTrace.rawBits() << ")" << std::dec;
    if (sender.rawBits() == nilObjTrace.rawBits()) {
        std::cerr << " [NIL]";
    } else if (sender.isNil()) {
        std::cerr << " (nil-method)";
    } else if (sender.isSmallInteger()) {
        std::cerr << " (SmallInt: " << sender.asSmallInteger() << ")";
    } else if (sender.isCharacter()) {
        std::cerr << " (Character)";
    } else if (sender.isObject()) {
        std::cerr << " (Object)";
    } else {
        std::cerr << " (IMMEDIATE but not SmallInt/Char)";
    }
    std::cerr << "\n";
    std::cerr.flush();

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

    // Log only the first snapshot resume detection
    if (isSnapshotResume && firstSnapshotResume) {
        std::cerr << "[VM] Detected SnapshotOperation context - adjusting for resume\n";
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
                // Invalid PC - either before bytecodes or past end
                static int badPcCount = 0;
                badPcCount++;
                if (badPcCount <= 5) {
                    std::cerr << "[VM] WARNING: savedPC=" << pcOffset << " out of range ["
                              << (bytecodeStart + 1) << "," << totalBytes << "], resetting to start\n";
                }
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

    // Debug: check if IP is valid
    static int execCount = 0;
    execCount++;
    if (execCount <= 5) {
        std::cerr << "[VM] executeFromContext #" << execCount
                  << ": pcOffset=" << pcOffset
                  << " bytecodeStart=" << bytecodeStart
                  << " totalBytes=" << totalBytes
                  << " IP=0x" << std::hex << (uintptr_t)instructionPointer_
                  << " end=0x" << (uintptr_t)bytecodeEnd_ << std::dec
                  << " valid=" << (instructionPointer_ < bytecodeEnd_ ? "yes" : "NO")
                  << "\n";
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
                    std::cerr << "[VM] Set SnapshotOperation.save to false\n";
                }

                // slot 2: clear 'isQuit' to prevent quit on resume
                Oop slot2 = rcvr->slotAt(2);
                if (slot2 == memory_.trueObject()) {
                    rcvr->slotAtPut(2, memory_.falseObject());
                    std::cerr << "[VM] Cleared SnapshotOperation.isQuit for resume\n";
                }
            }
        }
    }

    // Dump first few bytecodes
    // DEBUG_LOG("[DEBUG] executeFromContext: First bytecodes at IP:" << std::hex;
    for (int i = 0; i < 16 && (instructionPointer_ + i) < bytecodeEnd_; i++) {
        // std::cerr << " " << (int)instructionPointer_[i];
    }
    // std::cerr << std::dec; // DEBUG

    initializeSelectors();
    running_ = true;

    // Debug: final IP check before returning
    static int exitCount = 0;
    exitCount++;
    if (exitCount <= 5) {
        std::cerr << "[VM] executeFromContext exit #" << exitCount
                  << " IP=0x" << std::hex << (uintptr_t)instructionPointer_
                  << " end=0x" << (uintptr_t)bytecodeEnd_
                  << " this=0x" << (uintptr_t)this << std::dec << "\n";
    }

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

    // File I/O primitives (90-99)
    primitiveTable_[90] = &Interpreter::primitiveFileAtEnd;
    primitiveTable_[91] = &Interpreter::primitiveFileClose;
    primitiveTable_[92] = &Interpreter::primitiveFileGetPosition;
    primitiveTable_[93] = &Interpreter::primitiveFileOpen;
    primitiveTable_[94] = &Interpreter::primitiveFileRead;
    primitiveTable_[95] = &Interpreter::primitiveFileSetPosition;
    primitiveTable_[96] = &Interpreter::primitiveFileDelete;
    primitiveTable_[97] = &Interpreter::primitiveFileSize;
    primitiveTable_[98] = &Interpreter::primitiveFileWrite;
    primitiveTable_[99] = &Interpreter::primitiveFileRename;

    // Display primitives (101-104, 107, 109)
    primitiveTable_[101] = &Interpreter::primitiveBeCursor;
    primitiveTable_[102] = &Interpreter::primitiveBeDisplay;
    primitiveTable_[103] = &Interpreter::primitiveForceDisplayUpdate;  // iOS: forceDisplayUpdate (was scanCharacters)
    primitiveTable_[104] = &Interpreter::primitiveDrawLoop;
    primitiveTable_[107] = &Interpreter::primitiveShowDisplayRect;
    primitiveTable_[109] = &Interpreter::primitiveSnapshotEmbedded;

    // String/Array primitives (105)
    primitiveTable_[105] = &Interpreter::primitiveReplaceFromTo;

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

    // FFI/External primitives (116-118, 147)
    primitiveTable_[116] = &Interpreter::primitiveFlushExternalPrimitives;
    primitiveTable_[117] = &Interpreter::primitiveCalloutToFFI;
    primitiveTable_[118] = &Interpreter::primitiveDLLCall;
    primitiveTable_[147] = &Interpreter::primitiveExternalCall;

    // Socket primitive (133)
    primitiveTable_[133] = &Interpreter::primitiveSocket;

    // Special objects and GC primitives (129-130)
    primitiveTable_[129] = &Interpreter::primitiveSpecialObjectsOop;
    primitiveTable_[130] = &Interpreter::primitiveFullGC;

    // Snapshot primitive (131)
    primitiveTable_[131] = &Interpreter::primitiveSnapshot;

    // System path primitives (121, 142)
    primitiveTable_[121] = &Interpreter::primitiveImageName;
    primitiveTable_[142] = &Interpreter::primitiveVMPath;

    // Directory primitives (122-124, 126-127)
    primitiveTable_[122] = &Interpreter::primitiveDirectoryCreate;
    primitiveTable_[123] = &Interpreter::primitiveDirectoryDelimitor;
    primitiveTable_[124] = &Interpreter::primitiveDirectoryLookup;
    primitiveTable_[126] = &Interpreter::primitiveDirectoryDelete;
    primitiveTable_[127] = &Interpreter::primitiveDirectoryGetMacTypeAndCreator;

    // Additional file primitives (161-164)
    primitiveTable_[161] = &Interpreter::primitiveFileStdioHandles;
    primitiveTable_[162] = &Interpreter::primitiveFileDescriptorType;
    primitiveTable_[163] = &Interpreter::primitiveFileFlush;
    primitiveTable_[164] = &Interpreter::primitiveFileTruncate;

    // Screen primitives (106, 108)
    primitiveTable_[106] = &Interpreter::primitiveScreenSize;
    primitiveTable_[108] = &Interpreter::primitiveScreenDepth;

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

    // Array/memory primitives (145, 148, 156, 159)
    primitiveTable_[145] = &Interpreter::primitiveConstantFill;
    primitiveTable_[148] = &Interpreter::primitiveShallowCopy;
    primitiveTable_[156] = &Interpreter::primitiveCompareBytes;
    primitiveTable_[159] = &Interpreter::primitiveHashMultiply;

    // System primitives (152-155)
    primitiveTable_[152] = &Interpreter::primitiveSetFullScreen;
    primitiveTable_[153] = &Interpreter::primitiveInputSemaphore;
    primitiveTable_[154] = &Interpreter::primitiveInputWord;
    primitiveTable_[155] = &Interpreter::primitiveCompareString;

    // String primitives (157-158)
    primitiveTable_[157] = &Interpreter::primitiveCompareStringCollated;
    primitiveTable_[158] = &Interpreter::primitiveCompareStringNoCase;

    // Become primitives (197-198)
    primitiveTable_[197] = &Interpreter::primitiveArrayBecomeOneWay;
    primitiveTable_[198] = &Interpreter::primitiveArrayBecomeOneWayCopyHash;

    // Process/system primitives (172, 179)
    primitiveTable_[172] = &Interpreter::primitiveSetGCSemaphore;
    primitiveTable_[179] = &Interpreter::primitiveRelinquishProcessor;

    // Process yield (167)
    primitiveTable_[167] = &Interpreter::primitiveYield;

    // Block primitives (201-206)
    primitiveTable_[201] = &Interpreter::primitiveBlockValue;
    primitiveTable_[202] = &Interpreter::primitiveBlockValueWithArgs;
    primitiveTable_[203] = &Interpreter::primitiveValueUninterruptably;
    // 204 would be value with more args
    primitiveTable_[205] = &Interpreter::primitiveBlockValue;  // value with 1 arg
    primitiveTable_[206] = &Interpreter::primitiveBlockValue;  // value with 2 args

    // VM parameter primitive (254)
    primitiveTable_[254] = &Interpreter::primitiveVMParameter;

    // Context primitives (199)
    primitiveTable_[199] = &Interpreter::primitiveThisContext;

    // Slot access primitives (173-174)
    primitiveTable_[173] = &Interpreter::primitiveSlotAt;
    primitiveTable_[174] = &Interpreter::primitiveSlotAtPut;

    // Object enumeration primitives (177-178)
    primitiveTable_[177] = &Interpreter::primitiveAllInstances;
    primitiveTable_[178] = &Interpreter::primitiveAllObjects;

    // Object reference primitives (132)
    primitiveTable_[132] = &Interpreter::primitiveObjectPointsTo;

    // Become primitives (72, 128)
    primitiveTable_[72] = &Interpreter::primitiveBecome;
    primitiveTable_[128] = &Interpreter::primitiveBecomeForward;

    // Bit operation primitives (575-576)
    primitiveTable_[575] = &Interpreter::primitiveHighBit;
    primitiveTable_[576] = &Interpreter::primitiveLowBit;

    // Word array access primitives (165-166)
    primitiveTable_[165] = &Interpreter::primitiveIntegerAt;
    primitiveTable_[166] = &Interpreter::primitiveIntegerAtPut;

    // Class/behavior primitives (115, 175)
    primitiveTable_[115] = &Interpreter::primitiveChangeClass;
    primitiveTable_[175] = &Interpreter::primitiveBehaviorHash;

    // 16-bit array access primitives (143-144)
    primitiveTable_[143] = &Interpreter::primitiveShortAt;
    primitiveTable_[144] = &Interpreter::primitiveShortAtPut;

    // Raw object iteration primitives (138-139)
    primitiveTable_[138] = &Interpreter::primitiveSomeObject;
    primitiveTable_[139] = &Interpreter::primitiveNextObject;

    // VM attribute primitive (149)
    primitiveTable_[149] = &Interpreter::primitiveGetAttribute;

    // Immutability primitives (150-151)
    primitiveTable_[150] = &Interpreter::primitiveGetImmutability;
    primitiveTable_[151] = &Interpreter::primitiveSetImmutability;

    // Object copy primitive (168)
    primitiveTable_[168] = &Interpreter::primitiveCopyObject;

    // Compiled method creation primitive (79)
    primitiveTable_[79] = &Interpreter::primitiveNewMethod;

    // Instance adoption primitive (160)
    primitiveTable_[160] = &Interpreter::primitiveAdoptInstance;

    // Object pinning primitives (183-185)
    primitiveTable_[183] = &Interpreter::primitiveIsPinned;
    primitiveTable_[184] = &Interpreter::primitivePin;
    primitiveTable_[185] = &Interpreter::primitiveUnpin;

    // Memory management primitives (125, 176, 180)
    primitiveTable_[125] = &Interpreter::primitiveSignalAtBytesLeft;
    primitiveTable_[176] = &Interpreter::primitiveMaxIdentityHash;
    primitiveTable_[180] = &Interpreter::primitiveGrowMemory;

    // Interrupt semaphore primitive (134)
    primitiveTable_[134] = &Interpreter::primitiveInterruptSemaphore;

    // Context termination primitive (196)
    primitiveTable_[196] = &Interpreter::primitiveTerminateTo;

    // Float bit access primitives (38-39)
    primitiveTable_[38] = &Interpreter::primitiveFloatAt;
    primitiveTable_[39] = &Interpreter::primitiveFloatAtPut;

    // Exception handler primitives (186-189)
    primitiveTable_[186] = &Interpreter::primitiveMarkHandlerMethod;
    primitiveTable_[187] = &Interpreter::primitiveMarkUnwindMethod;
    primitiveTable_[188] = &Interpreter::primitiveFindHandlerContext;
    primitiveTable_[189] = &Interpreter::primitiveFindNextUnwindContext;

    // Context inspection primitives (210-212)
    primitiveTable_[210] = &Interpreter::primitiveContextSize;
    primitiveTable_[211] = &Interpreter::primitiveContextAt;
    primitiveTable_[212] = &Interpreter::primitiveContextAtPut;

    // Image segment primitives (213-216)
    primitiveTable_[213] = &Interpreter::primitiveStoreImageSegment;
    primitiveTable_[214] = &Interpreter::primitiveLoadImageSegment;
    primitiveTable_[215] = &Interpreter::primitiveArraySwap;
    primitiveTable_[216] = &Interpreter::primitiveFindRoots;

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

    // Misc primitives (231-239)
    primitiveTable_[231] = &Interpreter::primitiveForceDisplayUpdate;
    primitiveTable_[232] = &Interpreter::primitiveFormPrint;
    primitiveTable_[233] = &Interpreter::primitiveSetDisplayMode;
    primitiveTable_[234] = &Interpreter::primitiveBitmapDecompress;
    primitiveTable_[235] = &Interpreter::primitiveStringCompareWith;
    primitiveTable_[236] = &Interpreter::primitiveSampledSoundConvert;
    primitiveTable_[237] = &Interpreter::primitiveSerialPortOp;
    primitiveTable_[238] = &Interpreter::primitivePluginCallback;
    primitiveTable_[239] = &Interpreter::primitiveLongRunningPrimitive;

    // Cache flushing primitives (119-120)
    primitiveTable_[119] = &Interpreter::primitiveFlushCacheByMethod;
    primitiveTable_[120] = &Interpreter::primitiveFlushCacheBySelector;

    // Perform in superclass primitive (100)
    primitiveTable_[100] = &Interpreter::primitivePerformInSuperclass;

    // Closure value variant (204)
    primitiveTable_[204] = &Interpreter::primitiveClosureValueNoContextSwitch;

    // Closure primitives (200, 207-209)
    primitiveTable_[200] = &Interpreter::primitiveClosureCopyWithCopiedValues;
    primitiveTable_[207] = &Interpreter::primitiveFullClosureValue;
    primitiveTable_[208] = &Interpreter::primitiveClosureValueUnwind;
    primitiveTable_[209] = &Interpreter::primitiveClosureValueNoUnwind;

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

    // NOTE: Primitive 15 is primitiveBitOr (SmallInteger>>bitOr:)
    // Do NOT override it here - primitiveIsPointers is not a standard primitive

    // String hash primitive (146)
    primitiveTable_[146] = &Interpreter::primitiveStringHash;

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

    // Object size primitives (181-182)
    primitiveTable_[181] = &Interpreter::primitiveSizeInBytesOfInstance;
    primitiveTable_[182] = &Interpreter::primitiveSizeInBytes;

    // Context manipulation primitives (190-195)
    primitiveTable_[190] = &Interpreter::primitiveSetSender;
    primitiveTable_[191] = &Interpreter::primitiveSetInstructionPointer;
    primitiveTable_[192] = &Interpreter::primitiveSetStackPointer;
    primitiveTable_[193] = &Interpreter::primitiveSetMethod;
    primitiveTable_[194] = &Interpreter::primitiveSetReceiver;
    primitiveTable_[195] = &Interpreter::primitiveSetClosureOrNil;

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
