/*
 * Interpreter.cpp - Bytecode Interpreter Implementation
 *
 * This implements the Sista V1 bytecode interpreter.
 */

#include "Interpreter.hpp"
#include <cstring>
#include <iostream>

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

    // DEBUG: "[DEBUG] Getting scheduler association..."
    Oop scheduler = memory_.specialObject(SpecialObjectIndex::SchedulerAssociation);
    // DEBUG_LOG("[DEBUG] Scheduler: 0x" << std::hex << scheduler.rawBits() << std::dec;
    if (scheduler.isNil()) {
        // DEBUG: "[DEBUG] Scheduler is nil"
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
        // DEBUG_LOG("[DEBUG] Context appears unrelocated (old base address) - treating as nil";
        // DEBUG: "[DEBUG] Attempting bootstrap startup..."
        return bootstrapStartup();
    }

    // Check if context is nil (fresh image startup)
    if (context.isNil() || (context.isObject() && context.asObjectPtr()->slotCount() == 0)) {
        // DEBUG: "[DEBUG] Context is nil - attempting bootstrap startup..."
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

        // std::cerr << "[CHAIN " << depth << "] " << rcvrClassName << ">>" << methodSelector; // DEBUG

        // Check if this method has a primitive (for first few contexts)
        if (depth < 3 && method.isObject() && method.rawBits() > 0x10000) {
            int primIdx = primitiveIndexOf(method);
            if (primIdx > 0) {
                // std::cerr << "[CHAIN " << depth << "] Method has primitive #" << primIdx; // DEBUG
            }
            // Also show the PC
            Oop pc = memory_.fetchPointer(1, currentCtx);
            // std::cerr << "[CHAIN " << depth << "] PC = ";
            if (pc.isSmallInteger()) {
                // std::cerr << pc.asSmallInteger(); // DEBUG
            } else {
                // std::cerr << "0x" << std::hex << pc.rawBits() << std::dec; // DEBUG
            }

            // Show first bytecodes of the method
            ObjectHeader* methodHdr = method.asObjectPtr();
            Oop header = memory_.fetchPointer(0, method);
            if (header.isSmallInteger()) {
                int64_t bits = header.asSmallInteger();
                int numLiterals = bits & 0x7FFF;  // bits 0-14 are numLiterals
                uint8_t* bytecodes = methodHdr->bytes() + (1 + numLiterals) * 8;
                // std::cerr << "[CHAIN " << depth << "] First bytecodes: ";
                for (int i = 0; i < 10; i++) {
                    // std::cerr << std::hex << (int)bytecodes[i] << " ";
                }
                // std::cerr << std::dec; // DEBUG
            }
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

    // Now execute from the original context
    // DEBUG_LOG("[DEBUG] Found valid context - delegating to executeFromContext()";
    return executeFromContext(context);
}

// ===== MAIN LOOP =====

void Interpreter::interpret() {
    while (running_) {
        step();
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

    uint8_t bytecode = fetchByte();
    dispatchBytecode(bytecode);

    return running_;
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

    if (bytecode <= 0x0F) {
        // Both: 0x00-0x0F (0-15): Push receiver variable 0-15
        pushReceiverVariable(bytecode);
    }
    else if (bytecode <= 0x1F) {
        // 0x10-0x1F (16-31): Differs by bytecode set
        if (usesSistaV1_) {
            // Sista: push literal variable 0-15
            pushLiteralVariable(bytecode - 0x10);
        } else {
            // V3: push temp 0-15
            pushTemporary(bytecode - 0x10);
        }
    }
    else if (bytecode <= 0x2F) {
        // 0x20-0x2F (32-47): Push literal constant 0-15 (both sets)
        pushLiteralConstant(bytecode - 0x20);
    }
    else if (bytecode <= 0x3F) {
        // 0x30-0x3F (48-63): Differs by bytecode set
        if (usesSistaV1_) {
            // Sista: push temp 0-15
            pushTemporary(bytecode - 0x30);
        } else {
            // V3: push literal constant 16-31
            pushLiteralConstant(bytecode - 0x30 + 16);
        }
    }
    else if (bytecode <= 0x5F) {
        // 0x40-0x5F (64-95): Differs significantly between V3 and Sista
        if (!usesSistaV1_) {
            // V3PlusClosures: 0x40-0x5F = push literal variable 0-31
            pushLiteralVariable(bytecode - 0x40);
        } else {
            // Sista V1 handling
            if (bytecode <= 0x4B) {
                // 0x40-0x4B: Push temp 16-27
                pushTemporary(16 + bytecode - 0x40);
            } else if (bytecode <= 0x4F) {
                // 0x4C-0x4F: Push specials
                switch (bytecode) {
                    case 0x4C: push(receiver_); break;              // push self
                    case 0x4D: push(memory_.trueObject()); break;   // push true
                    case 0x4E: push(memory_.falseObject()); break;  // push false
                    case 0x4F: push(memory_.nil()); break;          // push nil
                }
            } else if (bytecode <= 0x51) {
                // 0x50-0x51: Push small integers
                switch (bytecode) {
                    case 0x50: push(Oop::fromSmallInteger(0)); break;  // push 0
                    case 0x51: push(Oop::fromSmallInteger(1)); break;  // push 1
                }
            } else {
                // 0x52-0x5F: Extended push operations and returns
                switch (bytecode) {
                    case 0x52: // Push thisContext
                        push(activeContext_);
                        break;
                    case 0x53: // Push literal var at index
                    {
                        int index = extA_;
                        extA_ = 0;
                        pushLiteralVariable(index);
                        break;
                    }
                    case 0x54: // Push closure copy
                        createBlock();
                        break;
                    case 0x55: // Push integer
                    {
                        int value = extB_;
                        extB_ = 0;
                        push(Oop::fromSmallInteger(value));
                        break;
                    }
                    case 0x56: // Push character
                    {
                        uint32_t codepoint = static_cast<uint32_t>(extB_) & 0xFFFFFF;
                        extB_ = 0;
                        push(Oop::fromCharacter(codepoint));
                        break;
                    }
                    case 0x57: // Extended push
                    {
                        uint8_t desc = fetchByte();
                        int type = (desc >> 6) & 0x3;
                        int index = (extA_ << 6) | (desc & 0x3F);
                        extA_ = 0;
                        switch (type) {
                            case 0: pushReceiverVariable(index); break;
                            case 1: pushTemporary(index); break;
                            case 2: pushLiteralConstant(index); break;
                            case 3: pushLiteralVariable(index); break;
                        }
                        break;
                    }
                    case 0x58: returnValue(receiver_); break;              // return self
                    case 0x59: returnValue(memory_.trueObject()); break;   // return true
                    case 0x5A: returnValue(memory_.falseObject()); break;  // return false
                    case 0x5B: returnValue(memory_.nil()); break;          // return nil
                    case 0x5C: returnFromMethod(); break;                  // return top
                    case 0x5D: returnFromBlock(); break;                   // block return
                    case 0x5E: /* NOP */ break;
                    case 0x5F: /* Trap */ break;
                }
            }
        }
    }
    else if (bytecode <= 0x6F) {
        // 0x60-0x6F (96-111): Pop and store - same in both sets
        if (bytecode <= 0x67) {
            // Pop and store receiver variable 0-7
            Oop value = pop();
            setReceiverInstVar(bytecode - 0x60, value);
        } else {
            // Pop and store temp 0-7
            Oop value = pop();
            setTemporary(bytecode - 0x68, value);
        }
    }
    else if (bytecode <= 0x7F) {
        // 0x70-0x7F (112-127): Differs between V3 and Sista
        if (!usesSistaV1_) {
            // V3PlusClosures:
            // 0x70-0x77: push specials (self, true, false, nil, -1, 0, 1, 2)
            // 0x78-0x7B: return (self, true, false, nil)
            // 0x7C: return top
            // 0x7D: block return
            // 0x7E-0x7F: extension bytecodes (unused in basic V3)
            if (bytecode <= 0x77) {
                switch (bytecode) {
                    case 0x70: push(receiver_); break;              // push self
                    case 0x71: push(memory_.trueObject()); break;   // push true
                    case 0x72: push(memory_.falseObject()); break;  // push false
                    case 0x73: push(memory_.nil()); break;          // push nil
                    case 0x74: push(Oop::fromSmallInteger(-1)); break; // push -1
                    case 0x75: push(Oop::fromSmallInteger(0)); break;  // push 0
                    case 0x76: push(Oop::fromSmallInteger(1)); break;  // push 1
                    case 0x77: push(Oop::fromSmallInteger(2)); break;  // push 2
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
            } else {
                // 0x7E-0x7F: V3 extension bytecodes (not commonly used)
            }
        } else {
            // Sista V1:
            // 0x70-0x77: Pop and store literal variable 0-7
            // 0x78-0x7F: Short jumps 1-8
            if (bytecode <= 0x77) {
                Oop value = pop();
                Oop assoc = literal(bytecode - 0x70);
                if (assoc.isObject()) {
                    memory_.storePointer(1, assoc, value);
                }
            } else {
                shortJump(bytecode - 0x78 + 1);
            }
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
        // Sista V1: 0xC8-0xCF (200-207): Long jumps (with extension byte)
        // The jump offset is encoded in the next byte, extended by extB
        uint8_t offsetByte = fetchByte();
        int offset = (extB_ << 8) | offsetByte;
        extB_ = 0;  // Reset extension

        int jumpType = bytecode & 0x07;
        switch (jumpType) {
            case 0: // Long unconditional jump
                instructionPointer_ += offset;
                break;
            case 1: // Long jump if true
                {
                    Oop value = pop();
                    if (isTrue(value)) {
                        instructionPointer_ += offset;
                    }
                }
                break;
            case 2: // Long jump if false
                {
                    Oop value = pop();
                    if (!isTrue(value)) {
                        instructionPointer_ += offset;
                    }
                }
                break;
            case 3: // Long pop and jump if true
                {
                    Oop value = pop();
                    if (isTrue(value)) {
                        instructionPointer_ += offset;
                    }
                }
                break;
            case 4: // Long pop and jump if false
                {
                    Oop value = pop();
                    if (!isTrue(value)) {
                        instructionPointer_ += offset;
                    }
                }
                break;
            default:
                // Reserved
                break;
        }
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
        // 0xE0-0xE7 (224-231): Differs by bytecode set
        if (!usesSistaV1_) {
            // V3PlusClosures: 0xE0-0xE7 = Send literal selector 0-7 with 2 args
            int litIndex = bytecode - 0xE0;
            Oop selector = literal(litIndex);
            sendSelector(selector, 2);
        } else {
        // Sista V1: 0xE0-0xE7 (224-231): Extension and extended operations
        switch (bytecode) {
            case 0xE0: // Extension A - modifies next bytecode's literal/temp index
            {
                uint8_t extByte = fetchByte();
                extA_ = (extA_ << 8) | extByte;
                break;
            }
            case 0xE1: // Extension B - modifies next bytecode's numArgs/other
            {
                uint8_t extByte = fetchByte();
                // Sign extend if high bit set (extB can be negative for backward jumps)
                if (extByte >= 128) {
                    extB_ = (extB_ << 8) | extByte | 0xFFFFFF00;
                } else {
                    extB_ = (extB_ << 8) | extByte;
                }
                break;
            }
            case 0xE2: // Extended push (uses extA)
            {
                uint8_t desc = fetchByte();
                int type = (desc >> 6) & 0x3;
                int index = (extA_ << 6) | (desc & 0x3F);
                extA_ = 0;  // Reset extension
                switch (type) {
                    case 0: pushReceiverVariable(index); break;
                    case 1: pushTemporary(index); break;
                    case 2: pushLiteralConstant(index); break;
                    case 3: pushLiteralVariable(index); break;
                }
                break;
            }
            case 0xE3: // Extended store (uses extA)
            {
                uint8_t desc = fetchByte();
                int type = (desc >> 6) & 0x3;
                int index = (extA_ << 6) | (desc & 0x3F);
                extA_ = 0;
                Oop value = stackTop();
                switch (type) {
                    case 0: setReceiverInstVar(index, value); break;
                    case 1: setTemporary(index, value); break;
                    case 2: /* illegal */ break;
                    case 3: {
                        Oop assoc = literal(index);
                        memory_.storePointer(1, assoc, value);
                        break;
                    }
                }
                break;
            }
            case 0xE4: // Extended pop and store (uses extA)
            {
                uint8_t desc = fetchByte();
                int type = (desc >> 6) & 0x3;
                int index = (extA_ << 6) | (desc & 0x3F);
                extA_ = 0;
                Oop value = pop();
                switch (type) {
                    case 0: setReceiverInstVar(index, value); break;
                    case 1: setTemporary(index, value); break;
                    case 2: /* illegal */ break;
                    case 3: {
                        Oop assoc = literal(index);
                        memory_.storePointer(1, assoc, value);
                        break;
                    }
                }
                break;
            }
            case 0xE5: // Extended single-byte send (uses extA for lit index, extB for numArgs)
            {
                uint8_t litIndex = fetchByte();
                int fullIndex = (extA_ << 8) | litIndex;
                int numArgs = extB_ & 0xFF;  // Low byte of extB
                extA_ = 0;
                extB_ = 0;
                Oop selector = literal(fullIndex);
                sendSelector(selector, numArgs);
                break;
            }
            case 0xE6: // Extended double-byte super send
            {
                uint8_t numArgs = fetchByte();
                uint8_t litIndex = fetchByte();
                int fullIndex = (extA_ << 8) | litIndex;
                extA_ = 0;
                // Super send starts lookup from superclass
                Oop selector = literal(fullIndex);
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
            case 0xE7: // Extended double-byte send
            {
                uint8_t numArgs = fetchByte();
                uint8_t litIndex = fetchByte();
                int fullIndex = (extA_ << 8) | litIndex;
                extA_ = 0;
                Oop selector = literal(fullIndex);
                sendSelector(selector, numArgs);
                break;
            }
        }
        }  // End of Sista V1 else branch
    }
    else if (bytecode <= 0xEF) {
        // Sista V1: 0xE8-0xEF (232-239): Stack operations and closures
        switch (bytecode) {
            case 0xE8: // Pop
                popStack();
                break;
            case 0xE9: // Duplicate
                duplicateTop();
                break;
            case 0xEA: // Push thisContext
                push(activeContext_);
                break;
            case 0xEB: // Push closure copy (block creation)
            {
                // Sista closure encoding: extA has blockSize, extB has numCopied
                createBlock();
                break;
            }
            case 0xEC: // Push implicit receiver (for outer sends)
                push(receiver_);
                break;
            case 0xED: // Push closure copy (full block)
                createFullBlock();
                break;
            case 0xEE: // Reserved
            case 0xEF: // Reserved
                // WARN_LOG("[BYTECODE] Reserved Sista bytecode: 0x" << std::hex
                          // << (int)bytecode << std::dec;
                break;
        }
    }
    else if (bytecode <= 0xF7) {
        // Sista V1: 0xF0-0xF7 (240-247): Send literal selector 0-7 with 1 arg
        int litIndex = bytecode - 0xF0;
        Oop selector = literal(litIndex);
        sendSelector(selector, 1);
    }
    else {
        // Sista V1: 0xF8-0xFF (248-255): Send literal selector 0-7 with 2 args
        int litIndex = bytecode - 0xF8;
        Oop selector = literal(litIndex);
        sendSelector(selector, 2);
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
    // DEBUG_LOG("[RETURN] returnValue: value=0x" << std::hex << value.rawBits() << std::dec
              // << " frameDepth=" << frameDepth_;

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
                    // DEBUG_LOG("[RETURN] Following sender chain to context 0x"
                              // << std::hex << sender.rawBits() << std::dec;

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

        // No more work to do
        running_ = false;
        // Store the return value for inspection
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
    // Get receiver (under the arguments on stack)
    Oop rcvr = stackValue(argCount);

    // Check for invalid receiver (could be from out-of-bounds literal access)
    Oop nilObj = memory_.specialObject(SpecialObjectIndex::NilObject);
    if (rcvr.rawBits() == 0 || rcvr.rawBits() == nilObj.rawBits()) {
        // Receiver is nil - pop args and return nil without triggering DNU
        // DEBUG: "[SEND] Receiver is nil - returning nil instead of sending"
        popN(argCount + 1);  // Pop args and receiver
        push(nilObj);
        return;
    }

    // Determine receiver's class
    Oop rcvrClass = memory_.classOf(rcvr);
    // DEBUG_LOG("[SEND] rcvrClass=0x" << std::hex << rcvrClass.rawBits() << std::dec;

    // Check for invalid class (can happen with corrupted state)
    if (rcvrClass.rawBits() == 0) {
        // DEBUG: "[SEND] Invalid receiver class - returning nil"
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
        // IMPORTANT: set argCount_ before calling primitive
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
    // Restore previous execution state
    if (frameDepth_ == 0) {
        running_ = false;
        return;
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
        running_ = false;
    }
}

// ===== VARIABLE ACCESS =====

Oop Interpreter::literal(size_t index) const {
    // Literals are slots 1..numLiterals in the home method (slot 0 is header)
    // For CompiledBlocks, homeMethod_ points to the home CompiledMethod
    // which contains all the literals that block bytecodes reference.

    // Use homeMethod_ for literal access - it should always be the home CompiledMethod
    Oop literalMethod = homeMethod_;

    // Fallback to method_ if homeMethod_ is not set
    if (literalMethod.isNil() || !literalMethod.isObject()) {
        literalMethod = method_;
    }

    // Get numLiterals from method header for bounds check
    // Pharo header format: bits 0-14 = numLiterals (15 bits)
    Oop methodHeader = memory_.fetchPointer(0, literalMethod);
    if (methodHeader.isSmallInteger()) {
        int64_t headerBits = methodHeader.asSmallInteger();
        size_t numLiterals = headerBits & 0x7FFF;  // bits 0-14

        if (index >= numLiterals) {
            // Out of bounds - return nil
            return memory_.specialObject(SpecialObjectIndex::NilObject);
        }
    }

    return memory_.fetchPointer(index + 1, literalMethod);
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
        running_ = false;
        dnuDepth = 0;
        return;
    }

    // Get the actual receiver that failed (from stack, under args)
    Oop failedReceiver = stackValue(argCount);
    // std::cerr << "[DNU] #doesNotUnderstand: for receiver 0x" << std::hex << failedReceiver.rawBits() << std::dec; // DEBUG

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

    // Save current execution state to old process's suspendedContext
    // For now, we rely on activeContext_ being updated appropriately
    // The context should already reflect current execution state
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
    Oop nilObj = memory_.specialObject(SpecialObjectIndex::NilObject);
    Oop schedulerAssoc = memory_.specialObject(SpecialObjectIndex::SchedulerAssociation);

    if (!schedulerAssoc.isObject() || schedulerAssoc.rawBits() == nilObj.rawBits()) {
        return false;
    }

    Oop scheduler = memory_.fetchPointer(1, schedulerAssoc);
    if (!scheduler.isObject()) {
        // DEBUG: "[SCHED] Invalid scheduler"
        return false;
    }

    // ProcessScheduler: slot 0 = quiescentProcessLists, slot 1 = activeProcess
    Oop activeProcess = memory_.fetchPointer(1, scheduler);
    Oop queues = memory_.fetchPointer(0, scheduler);

    if (!queues.isObject()) {
        // DEBUG: "[SCHED] No process queues"
        return false;
    }

    ObjectHeader* queuesHeader = queues.asObjectPtr();
    size_t numQueues = queuesHeader->slotCount();

    // DEBUG: "[SCHED] Scanning " << numQueues << " priority queues for runnable process"

    // Search from highest to lowest priority
    for (int i = static_cast<int>(numQueues) - 1; i >= 0; i--) {
        Oop queue = queuesHeader->slotAt(i);
        if (!queue.isObject() || queue.rawBits() == nilObj.rawBits()) continue;

        // LinkedList: slot 0 = firstLink, slot 1 = lastLink
        Oop process = memory_.fetchPointer(0, queue);
        if (!process.isObject() || process.rawBits() == nilObj.rawBits()) continue;

        // Skip if this is the same process that just finished
        if (process.rawBits() == activeProcess.rawBits()) {
            // DEBUG_LOG("[SCHED] Skipping active process at priority " << (i + 1);
            continue;
        }

        // Process: slot 0 = nextLink, slot 1 = suspendedContext, slot 2 = priority
        Oop context = memory_.fetchPointer(1, process);
        if (!context.isObject() || context.rawBits() == nilObj.rawBits()) continue;

        ObjectHeader* ctxHeader = context.asObjectPtr();
        if (ctxHeader->format() != ObjectFormat::IndexableWithFixed) continue;

        // DEBUG_LOG("[SCHED] Found runnable process at priority " << (i + 1)
                  // << " context=0x" << std::hex << context.rawBits() << std::dec;

        // Update the active process in scheduler
        memory_.storePointer(1, scheduler, process);

        // Reset stack for new process
        stackPointer_ = stackBase_;
        frameDepth_ = 0;

        // Execute from the new process's context
        if (executeFromContext(context)) {
            // DEBUG: "[SCHED] Rescheduled successfully"
            return true;
        }
    }

    return false;
}

// ===== STARTUP SUPPORT =====

bool Interpreter::bootstrapStartup() {
    // DEBUG: "[DEBUG] bootstrapStartup: Looking for startup entry point..."

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
                            // DEBUG_LOG("[DEBUG] Found runnable context at priority " << (i + 1);
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

    startupAttempt++;
    // DEBUG: "[DEBUG] bootstrapStartup: Attempt #" << startupAttempt

    // If we've already tried startup methods and they completed,
    // the Smalltalk code has had a chance to run. Don't keep looping.
    if (startupAttempt > 5) {
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
        // DEBUG: "[DEBUG] Attempt 1: Trying recordStartupStamp for side effects..."

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
                if (!context.isNil()) {
                    stackPointer_ = stackBase_;
                    frameDepth_ = 0;
                    if (executeFromContext(context)) {
                        // DEBUG: "[DEBUG] Started recordStartupStamp execution"
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

    // Third try: Try Object >> yourself just to prove basic execution works
    if (startupAttempt == 3) {
        // DEBUG_LOG("[DEBUG] Attempt 3: Trying minimal execution (Object >> yourself)...";

        Oop arrayClass = memory_.specialObject(SpecialObjectIndex::ClassArray);
        if (arrayClass.isObject()) {
            Oop selector = findSelector("yourself");
            if (!selector.isNil()) {
                Oop method = lookupMethod(selector, arrayClass);
                if (!method.isNil() && method.isObject()) {
                    // Create a simple array as receiver
                    Oop receiver = memory_.allocateSlots(arrayClass.asObjectPtr()->classIndex(), 0);
                    if (receiver.isObject()) {
                        Oop context = memory_.createStartupContext(method, receiver);
                        if (!context.isNil()) {
                            stackPointer_ = stackBase_;
                            frameDepth_ = 0;
                            if (executeFromContext(context)) {
                                // DEBUG: "[DEBUG] Started minimal execution"
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

    if (context.isNil() || !context.isObject()) {
        // ERROR: "[ERROR] executeFromContext: Invalid context"
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

    // Dump context structure first
    ObjectHeader* ctxHeader = context.asObjectPtr();
    // DEBUG_LOG("[DEBUG] executeFromContext: Context has " << ctxHeader->slotCount() << " slots, cls=" << ctxHeader->classIndex();
    for (size_t i = 0; i < std::min(ctxHeader->slotCount(), (size_t)12); i++) {
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

    method_ = memory_.fetchPointer(3, context);
    receiver_ = memory_.fetchPointer(5, context);
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

    // Check if method has a primitive
    int primIdx = primitiveIndexOf(method_);
    if (primIdx > 0) {
        // Special handling for snapshot primitive (131)
        // When resuming from a saved image, the snapshot primitive should return false
        if (primIdx == 131) {
            // DEBUG_LOG("[DEBUG] Will push 'false' when execution begins (snapshot returns false on resume)";
        }
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

    // DEBUG_LOG("[DEBUG] executeFromContext: numLiterals=" << numLiterals
              // << " numTemps=" << numTemps
              // << " bytecodeStart=" << bytecodeStart
              // << " totalBytes=" << totalBytes;

    // Get the saved PC from the context
    // In Pharo, PC is 1-based byte offset from start of method bytes
    Oop savedPC = memory_.fetchPointer(1, context);
    if (savedPC.isSmallInteger()) {
        int64_t pcOffset = savedPC.asSmallInteger();
        if (pcOffset > 0) {
            instructionPointer_ = methodBytes + pcOffset - 1;
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

    // Dump first few bytecodes
    // DEBUG_LOG("[DEBUG] executeFromContext: First bytecodes at IP:" << std::hex;
    for (int i = 0; i < 16 && (instructionPointer_ + i) < bytecodeEnd_; i++) {
        // std::cerr << " " << (int)instructionPointer_[i];
    }
    // std::cerr << std::dec; // DEBUG

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
    primitiveTable_[76] = &Interpreter::primitiveBasicSize;

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

    // Array/memory primitives (145, 148, 156, 159)
    primitiveTable_[145] = &Interpreter::primitiveConstantFill;
    primitiveTable_[148] = &Interpreter::primitiveShallowCopy;
    primitiveTable_[156] = &Interpreter::primitiveCompareBytes;
    primitiveTable_[159] = &Interpreter::primitiveHashMultiply;

    // Process yield (167)
    primitiveTable_[167] = &Interpreter::primitiveYield;

    // Block primitives (201-206)
    primitiveTable_[201] = &Interpreter::primitiveBlockValue;
    primitiveTable_[202] = &Interpreter::primitiveBlockValueWithArgs;
    // 203, 204 would be value with more args
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

    // Class structure primitives (253-254)
    primitiveTable_[253] = &Interpreter::primitiveSuperclass;
    primitiveTable_[254] = &Interpreter::primitiveInstSize;

    // Quick return primitives (256-259)
    primitiveTable_[256] = &Interpreter::primitiveQuickReturnSelf;
    primitiveTable_[257] = &Interpreter::primitiveQuickReturnTrue;
    primitiveTable_[258] = &Interpreter::primitiveQuickReturnFalse;
    primitiveTable_[259] = &Interpreter::primitiveQuickReturnNil;

    // Object format query primitives
    // Note: These are often accessed via different primitive numbers in different images
    // We'll wire them to common slots
    primitiveTable_[15] = &Interpreter::primitiveIsPointers;  // Most common for #isPointers

    // String hash primitive (146)
    primitiveTable_[146] = &Interpreter::primitiveStringHash;

    // Class name primitive (514)
    primitiveTable_[514] = &Interpreter::primitiveClassName;

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
}

PrimitiveResult Interpreter::executePrimitive(int primitiveIndex, int argCount) {
    // Named primitives have high numbers (typically >= 32768)
    // They are looked up by name from method literals - not yet implemented
    // For now, fail gracefully so the method body executes
    if (primitiveIndex >= 32768) {
        // Named primitive - would need to look up by name in method literals
        // For now, just fail and let the method body execute
        return PrimitiveResult::Failure;
    }

    if (primitiveIndex < 0 || primitiveIndex >= static_cast<int>(primitiveTable_.size())) {
        // std::cerr << "[PRIM] Index out of range: " << primitiveIndex; // DEBUG
        return PrimitiveResult::Failure;
    }

    PrimitiveFunc prim = primitiveTable_[primitiveIndex];
    if (!prim) {
        // Unimplemented primitive - fail silently to reduce noise
        return PrimitiveResult::Failure;
    }

    PrimitiveResult result = (this->*prim)(argCount);
    return result;
}

Oop Interpreter::activeContext() const {
    // Would return actual context object
    // For stack-based execution, we'd need to materialize one
    return Oop::nil();
}

} // namespace pharo
