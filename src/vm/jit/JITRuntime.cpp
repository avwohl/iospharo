/*
 * JITRuntime.cpp - Runtime support for JIT-compiled code
 *
 * Copyright (c) 2026 Aaron Wohl. Licensed under the MIT License.
 */

#include "JITRuntime.hpp"
#include "PlatformJIT.hpp"
#include "../ObjectMemory.hpp"
#include "../Interpreter.hpp"
#include <cstring>
#include <cstdio>
#include <algorithm>

namespace pharo { extern uint64_t g_stepNum; }
using pharo::g_stepNum;

// JIT_CALL is now defined in JITState.hpp (shared with Interpreter.cpp)

#if PHARO_JIT_ENABLED

namespace pharo {
}

namespace pharo {
namespace jit {

// ===== RUNTIME HELPER IMPLEMENTATIONS =====

// These are the C functions that JIT stencils branch to when they
// can't handle something inline. They simply return — the JIT entry
// point checks exitReason after the call chain unwinds.

extern "C" void jit_rt_send(JITState* state) {
    // The exitReason is already set by the stencil.
    // Control returns to the JIT entry point (tryExecute), which
    // returns to the interpreter to handle the send.
    (void)state;
}

extern "C" void jit_rt_return(JITState* state) {
    // exitReason and returnValue already set by the stencil.
    (void)state;
}

extern "C" void jit_rt_arith_overflow(JITState* state) {
    // Arithmetic overflow: restore entry SP and re-execute the whole method.
    // The interpreter will handle LargeInteger arithmetic.
    state->exitReason = ExitArithOverflow;

    // Diagnostic: count how often the fallback fires per method. Enabled by
    // JIT_ARITH_OFLOW_TRACE=1. Prints a periodic summary at 10k firings.
    static const char* trace = getenv("JIT_ARITH_OFLOW_TRACE");
    if (trace && *trace == '1') {
        static size_t totalFirings = 0;
        totalFirings++;
        if (totalFirings <= 20 || (totalFirings % 100) == 0) {
            uint64_t methodOop = state->jitMethod ? state->jitMethod->compiledMethodOop : 0;
            std::string sel = state->interp ?
                state->interp->memory().selectorOf(Oop::fromRawBits(methodOop)) : "?";
            fprintf(stderr, "[ARITH-OFLOW] #%zu sel=#%s method=0x%llx sp=%p ip=%p\n",
                    totalFirings, sel.c_str(), (unsigned long long)methodOop,
                    (void*)state->sp, (void*)state->ip);
            fflush(stderr);
        }
    }
}

extern "C" void jit_rt_push_frame(JITState* state) {
    // J2J direct call: push an interpreter frame for GC root scanning.
    // Reads cachedTarget (method Oop), sendArgCount, ip from state.
    Interpreter* interp = state->interp;
    interp->incJ2JStencilCalls();
    interp->pushFrameForJIT(state);
}

extern "C" void jit_rt_pop_frame(JITState* state) {
    // J2J direct call: pop the interpreter frame after callee returns.
    Interpreter* interp = state->interp;
    interp->incJ2JStencilReturns();
    interp->popFrameForJIT(state);
}

// Tier 2 inline send helper.
// Called from MIR-generated code at each send site.
//
// Pre-conditions (set by MIR code before the CALL):
//   state->cachedTarget  = selector Oop
//   state->sendArgCount  = nArgs
//   state->sp             = stack pointer (vstack flushed, args pushed)
//   state->ip             = send bytecode address (for chain-loop fallback)
//
// Post-conditions on success (ExitNone):
//   state->sp adjusted (receiver+args popped, retval pushed)
//   state->exitReason = ExitNone
//
// Post-conditions on fallback:
//   state->exitReason = ExitSend (chain loop handles from scratch)
//   OR callee's non-ExitReturn reason (SavedFrame pushed for T2 caller)
extern "C" void jit_t2_send(JITState* state) {
    using namespace pharo;
    using namespace pharo::jit;

    static int t2SendCount = 0;
    static int t2SendDepth = 0;
    static bool t2SendDebug = !!getenv("T2_SEND_DBG");

    Interpreter* interp = state->interp;
    ObjectMemory* mem = state->memory;

    // Depth guard: prevent unbounded C-stack growth from recursive T2 sends.
    // At depth 200, bail to interpreter (chain loop) to unwind safely.
    if (t2SendDepth >= 200) {
        state->exitReason = ExitSend;
        return;
    }

    Oop selector = state->cachedTarget;
    int nArgs = state->sendArgCount;
    Oop rcvr = state->sp[-(nArgs + 1)];

    if (t2SendDebug && t2SendCount < 30) {
        fprintf(stderr, "[T2SEND#%d d=%d] sel=0x%llx nArgs=%d rcvr=0x%llx sp=%p ip=%p\n",
                t2SendCount, t2SendDepth,
                (unsigned long long)selector.rawBits(), nArgs,
                (unsigned long long)rcvr.rawBits(), (void*)state->sp,
                (void*)state->ip);
    }
    t2SendDepth++;

    Oop rcvrClass = mem->classOf(rcvr);

    // Method lookup (cache probe → full hierarchy search)
    Oop resolved = interp->lookupMethodForSend(selector, rcvrClass);
    if (!resolved.isObject() || resolved.rawBits() < 0x10000) {
        state->exitReason = ExitSend;
        t2SendDepth--;
        return;
    }

    // Check if callee is JIT-compiled
    MethodMap* mm = reinterpret_cast<MethodMap*>(state->methodMapPtr);
    JITMethod* jm = mm->lookup(resolved.rawBits());
    if (!jm || !jm->isExecutable()) {
        state->exitReason = ExitSend;
        t2SendDepth--;
        return;
    }

    // Check for primitive without prim prologue (needs interpreter handling)
    bool hasPrim = (jm->methodHeader >> 16) & 1;
    if (hasPrim && !jm->hasPrimPrologue) {
        state->exitReason = ExitSend;
        t2SendDepth--;
        return;
    }

    // Validate receiver is a compiled method
    if (resolved.asObjectPtr()->classIndex() != interp->compiledMethodClassIdx()) {
        state->exitReason = ExitSend;
        t2SendDepth--;
        return;
    }

    // === Save T2 caller state ===
    Oop* savedSP = state->sp;
    Oop savedRecv = state->receiver;
    Oop* savedTempBase = state->tempBase;
    Oop* savedLiterals = state->literals;
    Oop savedMethod = state->method;
    int savedArgCount = state->argCount;
    void* savedJitMethod = state->jitMethod;
    uint8_t* savedIP = state->ip;

    // === Set up callee in JITState ===
    ObjectHeader* methObj = resolved.asObjectPtr();
    Oop* fp = state->sp - (nArgs + 1);

    state->receiver = rcvr;
    state->literals = methObj->slots() + 1;
    state->tempBase = fp + 1;
    state->argCount = nArgs;
    state->jitMethod = jm;
    state->method = resolved;
    state->icDataPtr = nullptr;
    state->sendArgCount = 0;

    int numLits = static_cast<int>(jm->methodHeader & 0x7FFF);
    state->ip = methObj->bytes() + (1 + numLits) * 8;

    // Allocate temps if callee needs more than nArgs
    int totalTemps = jm->tempCount;
    if (__builtin_expect(nArgs < totalTemps, 0)) {
        Oop nil = mem->nil();
        for (int i = nArgs; i < totalTemps; i++) {
            *state->sp = nil;
            state->sp++;
        }
    }

    // Disable stencil J2J — the J2J trampoline only runs in tryJITActivation.
    state->j2jSaveCursor = nullptr;
    state->j2jSaveLimit = nullptr;
    state->j2jDepth = 0;
    state->j2jTotalCalls = 0;
    state->yieldCountdown = 1000;

    // === Try T2 code first (avoids ExitSendCached from T1 stencils) ===
    // T1 stencils exit with ExitSendCached on inner sends (J2J disabled),
    // so prefer T2 callee which handles sends via recursive jit_t2_send.
    JITRuntime& rt = interp->jitRuntime();
    void* t2code = rt.lookupTier2(resolved.rawBits());
    if (t2SendDebug && t2SendCount < 5) {
        fprintf(stderr, "[T2SEND-LOOKUP] resolved=0x%llx t2code=%p jm=%p\n",
                (unsigned long long)resolved.rawBits(), t2code, (void*)jm);
    }
    if (t2code && t2code != (void*)1) {
        state->exitReason = ExitNone;
        state->returnValue = Oop::fromRawBits(0xDEAD0001ULL);  // sentinel
        if (t2SendDebug && t2SendCount < 5) {
            fprintf(stderr, "[T2SEND-T2-PRE] rcv=0x%llx sp=%p lit[0]=0x%llx ip=%p method=0x%llx\n",
                    (unsigned long long)state->receiver.rawBits(), (void*)state->sp,
                    (unsigned long long)state->literals[0].rawBits(),
                    (void*)state->ip, (unsigned long long)state->method.rawBits());
        }
        ((void(*)(JITState*))t2code)(state);
        if (t2SendDebug && t2SendCount < 5) {
            fprintf(stderr, "[T2SEND-T2-POST] exit=%d rv=0x%llx sp=%p\n",
                    state->exitReason, (unsigned long long)state->returnValue.rawBits(),
                    (void*)state->sp);
        }
    } else {
        // Fall back to T1 stencil code (works for leaf methods)
        JIT_CALL(jm->codeStart(), state);
    }

    if (t2SendDebug && t2SendCount < 30) {
        fprintf(stderr, "[T2SEND#%d d=%d] callee exit=%d retVal=0x%llx sp=%p\n",
                t2SendCount, t2SendDepth,
                state->exitReason,
                (unsigned long long)state->returnValue.rawBits(), (void*)state->sp);
    }
    t2SendCount++;

    if (__builtin_expect(state->exitReason == ExitReturn, 1)) {
        // === Fast path: callee returned normally ===
        Oop retVal = state->returnValue;

        // Restore T2 caller state
        state->sp = savedSP;
        state->receiver = savedRecv;
        state->tempBase = savedTempBase;
        state->literals = savedLiterals;
        state->method = savedMethod;
        state->argCount = savedArgCount;
        state->jitMethod = reinterpret_cast<JITMethod*>(savedJitMethod);
        state->ip = savedIP;

        // Pop receiver+args, push return value.
        // Convention: sp past TOS (matches chain-loop resume).
        // retval replaces receiver; pop the args above it.
        state->sp[-(nArgs + 1)] = retVal;
        state->sp -= nArgs;  // sp[-1] = retVal, sp[0] = next free

        if (t2SendDebug && t2SendCount < 50) {
            fprintf(stderr, "[T2SEND-RET d=%d] restored sp=%p rcv=0x%llx retVal=0x%llx\n",
                    t2SendDepth, (void*)state->sp,
                    (unsigned long long)state->receiver.rawBits(),
                    (unsigned long long)retVal.rawBits());
        }

        state->exitReason = ExitNone;
        t2SendDepth--;
        return;
    }

    // === Fallback: callee exited with non-ExitReturn ===
    // Restore T2 caller state and exit with ExitSend for the caller's send.
    // The chain loop will handle the send from scratch (re-resolve method,
    // activate callee). This discards the callee's partial execution but
    // avoids the SavedFrame/chain-loop interaction that caused infinite loops
    // with straight-line T2 methods.
    state->sp = savedSP;
    state->receiver = savedRecv;
    state->tempBase = savedTempBase;
    state->literals = savedLiterals;
    state->method = savedMethod;
    state->argCount = savedArgCount;
    state->jitMethod = reinterpret_cast<JITMethod*>(savedJitMethod);
    state->ip = savedIP;
    // Restore send context so chain loop can handle the caller's send
    state->cachedTarget = selector;
    state->sendArgCount = nArgs;
    state->icDataPtr = nullptr;
    state->exitReason = ExitSend;
    t2SendDepth--;
    return;
}

// Out-of-line array primitive handler for IC hit path.
// Called from sendJ2J stencil when primKind >= 14 (at:/at:put:/size).
// info = (primKind << 8) | nArgs
// Returns 1 on success (result written to sp), 0 on failure.
//
// Note: JITState uses pharo::Oop but stencils define their own Oop
// with public .bits field. We work with rawBits() here and use
// Oop::fromRawBits() to write back.
extern "C" uint64_t jit_rt_array_prim(JITState* s, uint64_t info) {
    uint8_t primKind = (uint8_t)(info >> 8);
    int nArgs = (int)(info & 0xFF);

    uint64_t rcvBits = s->sp[-(nArgs + 1)].rawBits();

    // Receiver must be an object pointer (tag == 0, not immediate)
    if ((rcvBits & 7) != 0 || rcvBits < 0x10000)
        return 0;

    uint64_t header = *reinterpret_cast<uint64_t*>(rcvBits);
    uint64_t fmt = (header >> 24) & 0x1F;
    uint64_t slotCount = (header >> 56) & 0xFF;
    if (slotCount == 255) {
        uint64_t raw = *reinterpret_cast<uint64_t*>(rcvBits - 8);
        slotCount = (raw << 8) >> 8;
    }

    if (primKind == 14) {
        // at: — read from Array or byte object
        uint64_t idxBits = s->sp[-nArgs].rawBits();
        if ((idxBits & 7) != 1) return 0;
        int64_t i = (int64_t)idxBits >> 3;
        if (fmt == 2) {
            if (i < 1 || (uint64_t)i > slotCount) return 0;
            Oop* slots = reinterpret_cast<Oop*>(rcvBits + 8);
            s->sp[-(nArgs + 1)] = slots[i - 1];
            s->sp -= nArgs;
            return 1;
        }
        if (fmt >= 16 && fmt <= 23) {
            uint64_t byteSize = slotCount * 8 - (fmt - 16);
            if (i < 1 || (uint64_t)i > byteSize) return 0;
            uint8_t byte = reinterpret_cast<uint8_t*>(rcvBits + 8)[i - 1];
            s->sp[-(nArgs + 1)] = Oop::fromRawBits(((uint64_t)byte << 3) | 1);
            s->sp -= nArgs;
            return 1;
        }
        return 0;
    }

    if (primKind == 15) {
        // at:put: — write to Array or byte object
        uint64_t idxBits = s->sp[-nArgs].rawBits();
        Oop val = s->sp[-(nArgs - 1)];
        if ((idxBits & 7) != 1) return 0;
        if (header & (1ULL << 23)) return 0;  // immutable
        int64_t i = (int64_t)idxBits >> 3;
        if (fmt == 2) {
            if (i < 1 || (uint64_t)i > slotCount) return 0;
            Oop* slots = reinterpret_cast<Oop*>(rcvBits + 8);
            slots[i - 1] = val;
            s->sp[-(nArgs + 1)] = val;
            s->sp -= nArgs;
            return 1;
        }
        if (fmt >= 16 && fmt <= 23) {
            uint64_t valBits = val.rawBits();
            if ((valBits & 7) != 1) return 0;
            int64_t byteVal = (int64_t)valBits >> 3;
            if (byteVal < 0 || byteVal > 255) return 0;
            uint64_t byteSize = slotCount * 8 - (fmt - 16);
            if (i < 1 || (uint64_t)i > byteSize) return 0;
            reinterpret_cast<uint8_t*>(rcvBits + 8)[i - 1] = (uint8_t)byteVal;
            s->sp[-(nArgs + 1)] = val;
            s->sp -= nArgs;
            return 1;
        }
        return 0;
    }

    if (primKind == 16) {
        // size — return slot count or byte size
        uint64_t size;
        if (fmt == 2) {
            size = slotCount;
        } else if (fmt >= 16 && fmt <= 23) {
            size = slotCount * 8 - (fmt - 16);
        } else {
            return 0;
        }
        s->sp[-(nArgs + 1)] = Oop::fromRawBits((size << 3) | 1);
        s->sp -= nArgs;
        return 1;
    }

    return 0;
}

// Out-of-line new/new: primitive handler for IC hit path.
// Called from sendJ2J stencil when primKind == 17 (new) or 18 (new:).
// info = (primKind << 8) | nArgs
// Returns 1 on success (new object pushed to sp), 0 on failure.
extern "C" uint64_t jit_rt_new_prim(JITState* s, uint64_t info) {
    uint8_t primKind = (uint8_t)(info >> 8);
    int nArgs = (int)(info & 0xFF);

    // Receiver is the class (at sp[-(nArgs+1)])
    uint64_t classBits = s->sp[-(nArgs + 1)].rawBits();
    if ((classBits & 7) != 0 || classBits < 0x10000)
        return 0;  // Not an object

    // For new: (primKind 18), size arg must be a SmallInteger
    int64_t indexableSize = 0;
    if (primKind == 18) {
        uint64_t sizeBits = s->sp[-nArgs].rawBits();
        if ((sizeBits & 7) != 1) return 0;
        indexableSize = (int64_t)sizeBits >> 3;
        if (indexableSize < 0) return 0;
    }

    // Read class format (slot 2 of the class object)
    // Slot layout: [superclass, methodDict, format, ...]
    // format encodes: bits 0-15 = fixedSize, bits 16-20 = instSpec
    Oop* classSlots = reinterpret_cast<Oop*>(classBits + 8);  // skip header
    uint64_t fmtBits = classSlots[2].rawBits();  // format is slot 2
    if ((fmtBits & 7) != 1) return 0;  // format must be SmallInteger
    int64_t format = (int64_t)fmtBits >> 3;

    int instSpec = (int)((format >> 16) & 0x1F);
    size_t fixedSize = format & 0xFFFF;

    if (primKind == 17) {
        // new: fixed-size class only (instSpec 0 or 1)
        if (instSpec >= 2) return 0;
        indexableSize = 0;
    } else {
        // new:: variable-size class required (instSpec >= 2)
        if (instSpec < 2) return 0;
    }

    // Compute slot count based on instance specification
    size_t slotCount;
    bool isBytes = instSpec >= 16;
    if (isBytes) {
        // Byte-indexed (ByteString, ByteArray, etc.)
        slotCount = ((size_t)indexableSize + 7) / 8;
    } else if (instSpec >= 10 && instSpec <= 11) {
        // 32-bit words
        slotCount = ((size_t)indexableSize * 4 + 7) / 8;
    } else if (instSpec >= 12 && instSpec <= 15) {
        // 16-bit words
        slotCount = ((size_t)indexableSize * 2 + 7) / 8;
    } else if (instSpec == 9) {
        // 64-bit words
        slotCount = (size_t)indexableSize;
    } else {
        // Pointer-indexed (Array, etc.) or fixed+variable
        slotCount = fixedSize + (size_t)indexableSize;
    }

    // Limit: don't inline huge allocations
    if (slotCount > 1024) return 0;

    // Compute object format for header
    uint8_t objFormat;
    if (isBytes) {
        size_t padding = slotCount * 8 - (size_t)indexableSize;
        objFormat = (uint8_t)(16 + padding);  // ObjectFormat::Indexable8 + padding
    } else if (instSpec >= 12 && instSpec <= 15) {
        size_t padding = slotCount * 8 / 2 - (size_t)indexableSize;
        objFormat = (uint8_t)(12 + padding);
    } else if (instSpec >= 10 && instSpec <= 11) {
        size_t padding = slotCount * 8 / 4 - (size_t)indexableSize;
        objFormat = (uint8_t)(10 + padding);
    } else if (instSpec == 9) {
        objFormat = 9;
    } else if (slotCount == 0 && fixedSize == 0) {
        objFormat = 0;  // zero-size
    } else if (instSpec >= 2) {
        objFormat = 2;  // variable pointer (Array)
    } else {
        objFormat = 1;  // fixed pointer
    }

    // Need overflow slot for > 254 slots
    bool hasOverflow = slotCount >= 255;
    size_t headerSize = 8 + (hasOverflow ? 8 : 0);
    size_t totalSize = headerSize + slotCount * 8;
    totalSize = (totalSize + 7) & ~7ULL;
    if (totalSize < 16) totalSize = 16;

    // Get class index
    ObjectMemory& mem = s->interp->memory();
    uint32_t classIndex = mem.indexOfClass(Oop::fromRawBits(classBits));
    if (classIndex == 0) return 0;

    // Bump allocation from old space
    uint8_t* freePtr = mem.oldSpaceFree();
    uint8_t* endPtr = freePtr + totalSize;
    if (endPtr > mem.oldSpaceEnd()) return 0;

    // Allocate
    ObjectHeader* obj;
    if (hasOverflow) {
        uint64_t* overflow = reinterpret_cast<uint64_t*>(freePtr);
        *overflow = slotCount | (0xFFULL << 56);
        obj = reinterpret_cast<ObjectHeader*>(freePtr + 8);
    } else {
        obj = reinterpret_cast<ObjectHeader*>(freePtr);
    }

    // Build header: slotCount | hash | format | classIndex | flags
    uint8_t headerSlots = hasOverflow ? 255 : (uint8_t)slotCount;
    uint64_t header = ObjectHeader::makeHeader(headerSlots, 0,
        static_cast<ObjectFormat>(objFormat), classIndex);
    *reinterpret_cast<uint64_t*>(obj) = header;

    // Zero-fill all slots (includes nil for pointer objects, 0 for bytes)
    if (slotCount > 0) {
        std::memset(reinterpret_cast<uint8_t*>(obj) + 8, 0, slotCount * 8);
    }

    // For pointer objects, fill with nil instead of zero
    if (!isBytes && instSpec < 9) {
        uint64_t nilBits = mem.nil().rawBits();
        if (nilBits != 0) {  // nil is Oop(0) in our encoding, so skip if 0
            Oop* slots = reinterpret_cast<Oop*>(reinterpret_cast<uint8_t*>(obj) + 8);
            for (size_t i = 0; i < slotCount; i++) {
                slots[i] = Oop::fromRawBits(nilBits);
            }
        }
    }

    // Commit allocation
    mem.setOldSpaceFreePointer(endPtr);

    // Convert to Oop
    Oop newObj = mem.oopFromPointer(obj);

    // Push result: replace receiver (and arg for new:) with new object
    s->sp[-(nArgs + 1)] = newObj;
    s->sp -= nArgs;
    return 1;
}

extern "C" void jit_rt_j2j_call(JITState* state) {
    // Merged J2J call: push frame, call callee, pop frame in one C++ call.
    //
    // On entry: cachedTarget = method Oop, sendArgCount = nArgs,
    //           returnValue = entry address bits, ip = past send bytecode.
    // On exit (success): exitReason=0, returnValue set, JITState restored.
    // On exit (bailout): exitReason!=0, JITState has callee's state.
    //
    // Performance: at 6.17M calls/run for fib(28), total overhead is ~16ms
    // (~2.6ns per call). Apple Silicon's store buffer and branch predictor
    // make Clang's 14-register prologue nearly free — tested alternatives
    // (noinline wrappers, SavedFrame-based state) are net-negative.
    // Breaking below 16ms requires lazy frame materialization.

    Interpreter* interp = state->interp;
    interp->incJ2JStencilCalls();

    // Save caller JITState to C locals
    Oop* savedSP = state->sp;
    Oop savedRecv = state->receiver;
    Oop* savedLit = state->literals;
    Oop* savedTemp = state->tempBase;
    JITMethod* savedJM = state->jitMethod;
    Oop savedMethod = state->method;
    int savedArgCount = state->argCount;
    uint8_t* savedIP = state->ip;

    uint8_t* entryAddr = reinterpret_cast<uint8_t*>(state->returnValue.rawBits());
    state->jitMethod = reinterpret_cast<JITMethod*>(entryAddr);

    interp->pushFrameForJIT(state);

    if (__builtin_expect(state->exitReason != 0, 0)) {
        state->sp = savedSP;
        state->receiver = savedRecv;
        state->literals = savedLit;
        state->tempBase = savedTemp;
        state->jitMethod = savedJM;
        state->method = savedMethod;
        state->argCount = savedArgCount;
        state->ip = savedIP;
        return;
    }

    JIT_CALL(entryAddr, state);

    if (__builtin_expect(state->exitReason == ExitReturn, 1)) {
        interp->incJ2JStencilReturns();

        interp->j2jPopFrame(savedMethod, savedRecv);

        state->sp = savedSP;
        state->receiver = savedRecv;
        state->literals = savedLit;
        state->tempBase = savedTemp;
        state->jitMethod = savedJM;
        state->method = savedMethod;
        state->argCount = savedArgCount;
        state->ip = savedIP;
        state->exitReason = ExitNone;
    }
    // Non-ExitReturn: leave callee's state for interpreter bailout.
}

// ===== JITRuntime =====

JITRuntime::JITRuntime()
    : nilOopBits(0), trueOopBits(0), falseOopBits(0)
{
    std::memset(countMap_, 0, sizeof(countMap_));
}

JITRuntime::~JITRuntime() {
    delete tier2Compiler_;
    delete compiler_;
}

bool JITRuntime::initialize(ObjectMemory& memory, Interpreter& interp) {
    if (initialized_) return true;

    // Initialize code zone
    if (!codeZone_.initialize()) {
        fprintf(stderr, "[JIT] Failed to initialize code zone\n");
        return false;
    }

    // Initialize method map
    if (!methodMap_.initialize()) {
        fprintf(stderr, "[JIT] Failed to initialize method map\n");
        return false;
    }

    // Set up special Oop values
    updateSpecialOops(memory);

    // Store interpreter reference for stats access
    interp_ = &interp;

    // Create the compiler
    compiler_ = new JITCompiler(codeZone_, methodMap_, memory, interp);

    // Register runtime helpers
    JITCompiler::RuntimeHelpers helpers;
    helpers.sendSlow = reinterpret_cast<void*>(&jit_rt_send);
    helpers.returnToInterp = reinterpret_cast<void*>(&jit_rt_return);
    helpers.arithOverflow = reinterpret_cast<void*>(&jit_rt_arith_overflow);
    helpers.nilOopAddr = &nilOopBits;
    helpers.trueOopAddr = &trueOopBits;
    helpers.falseOopAddr = &falseOopBits;
    helpers.megaCacheAddr = megaCache_;
    helpers.pushFrame = reinterpret_cast<void*>(&jit_rt_push_frame);
    helpers.popFrame = reinterpret_cast<void*>(&jit_rt_pop_frame);
    helpers.j2jCall = reinterpret_cast<void*>(&jit_rt_j2j_call);
    helpers.arrayPrim = reinterpret_cast<void*>(&jit_rt_array_prim);
    helpers.newPrim = reinterpret_cast<void*>(&jit_rt_new_prim);
    compiler_->setHelpers(helpers);

    // Create Tier 2 compiler (MIR-based)
    tier2Compiler_ = new Tier2Compiler(codeZone_, methodMap_, memory, interp);
    if (!tier2Compiler_->initialize()) {
        fprintf(stderr, "[JIT] Warning: Tier 2 compiler failed to initialize\n");
        delete tier2Compiler_;
        tier2Compiler_ = nullptr;
    }

    // After MAP_JIT mmap with PROT_EXEC, the initial W^X state might be
    // "executable" rather than "writable". Ensure we start in writable mode
    // so allocate() can zero the memory.
    makeWritable(codeZone_.rawStart(), codeZone_.totalBytes());

    initialized_ = true;
    fprintf(stderr, "[JIT] Initialized: %zu MB code zone at %p\n",
            codeZone_.totalBytes() / (1024 * 1024),
            (void*)codeZone_.rawStart());

    return true;
}

void JITRuntime::updateSpecialOops(ObjectMemory& memory) {
    nilOopBits = Oop::nil().rawBits();
    trueOopBits = memory.trueObject().rawBits();
    falseOopBits = memory.falseObject().rawBits();
}

void JITRuntime::noteMethodEntry(Oop compiledMethod) {
    if (!initialized_ || !compiler_) return;

    // Deferral: skip counting for the first N million interpreter steps.
    // This lets the interpreter run at full speed during startup.
    // PHARO_JIT_DEFER=N (seconds; default: 0 = no deferral)
    static int64_t deferSteps = -1; // -1 = uninitialized
    if (deferSteps == -1) {
        const char* env = getenv("PHARO_JIT_DEFER");
        // Convert seconds to approximate step count (~30M steps/sec on interpreter)
        deferSteps = env ? (int64_t)atoi(env) * 30000000 : 0;
        if (deferSteps > 0)
            fprintf(stderr, "[JIT] Deferring compilation for ~%lld steps\n", (long long)deferSteps);
    }
    if (deferSteps > 0 && (int64_t)g_stepNum < deferSteps) return;

    // Bisection support: JIT_MAX_COMPILE=N stops after N compilations
    static int maxCompile = -2; // -2 = uninitialized
    if (maxCompile == -2) {
        const char* env = getenv("JIT_MAX_COMPILE");
        maxCompile = env ? atoi(env) : -1; // -1 = unlimited
    }

    static size_t totalEntries = 0;
    totalEntries++;

    // Debug: log first few entries for specific selectors
    if (totalEntries <= 5000 && interp_) {
        std::string sel = interp_->memory().selectorOf(compiledMethod);
        if (sel == "benchFib" || sel.find("fib") != std::string::npos || sel.find("Fib") != std::string::npos) {
            fprintf(stderr, "[NOTE] #%zu #%s method=0x%llx\n",
                    totalEntries, sel.c_str(), (unsigned long long)compiledMethod.rawBits());
        }
    }

    // Periodic stats (every ~64K entries)
    if ((totalEntries & 0xFFFF) == 0) {
        size_t icHits = interp_ ? interp_->jitICHits() : 0;
        size_t icMisses = interp_ ? interp_->jitICMisses() : 0;
        size_t icPatches = interp_ ? interp_->jitICPatches() : 0;
        size_t icStale = interp_ ? interp_->jitICStale() : 0;
        size_t icTotal = icHits + icMisses;
        int hitPct = icTotal > 0 ? static_cast<int>(icHits * 100 / icTotal) : 0;
        size_t j2jChains = interp_ ? interp_->jitJ2JChains() : 0;
        size_t j2jFallbacks = interp_ ? interp_->jitJ2JFallbacks() : 0;
        size_t j2jActChains = interp_ ? interp_->jitJ2JActChains() : 0;
        size_t j2jActFalls = interp_ ? interp_->jitJ2JActFalls() : 0;
        size_t j2jDirect = interp_ ? interp_->jitJ2JDirectPatches() : 0;
        size_t j2jSCalls = interp_ ? interp_->jitJ2JStencilCalls() : 0;
        size_t j2jSReturns = interp_ ? interp_->jitJ2JStencilReturns() : 0;

        // Count map diagnostics
        size_t tracked = 0, hot = 0;
        for (size_t ci = 0; ci < CountMapSize; ci++) {
            if (countMap_[ci].key != 0) {
                tracked++;
                if (countMap_[ci].count >= CompileThreshold) hot++;
            }
        }
        fprintf(stderr, "[JIT] Stats: %zu sends, %zu compiled, %zu failed, "
                "%zu/%zu KB code | IC: %zu/%zu (%d%% hit, %zu patched, %zu stale) "
                "| J2J-r: %zu/%zu J2J-a: %zu/%zu J2J-d: %zu J2J-s: %zu/%zu"
                " | map: %zu tracked, %zu hot\n",
                totalEntries, compiler_->methodsCompiled(),
                compiler_->compilationsFailed(),
                codeZone_.usedBytes() / 1024, codeZone_.totalBytes() / 1024,
                icHits, icTotal, hitPct, icPatches, icStale,
                j2jChains, j2jChains + j2jFallbacks,
                j2jActChains, j2jActChains + j2jActFalls,
                j2jDirect,
                j2jSReturns, j2jSCalls,
                tracked, hot);

        // Dump top methods by executionCount (opt-in: PHARO_JIT_TOP=1)
        static int dumpTop = -1;
        if (dumpTop == -1) {
            const char* env = getenv("PHARO_JIT_TOP");
            dumpTop = env ? atoi(env) : 0;
        }
        if (dumpTop > 0 && interp_) {
            struct TopEntry { uint32_t count; JITMethod* m; };
            const size_t K = 10;
            TopEntry top[K] = {};
            size_t filled = 0;
            JITMethod* m = codeZone_.firstMethod();
            while (m) {
                if (m->state == MethodState::Compiled) {
                    uint32_t c = m->executionCount;
                    if (filled < K) {
                        top[filled++] = {c, m};
                    } else {
                        size_t minIdx = 0;
                        for (size_t k = 1; k < K; k++) {
                            if (top[k].count < top[minIdx].count) minIdx = k;
                        }
                        if (c > top[minIdx].count) top[minIdx] = {c, m};
                    }
                }
                m = m->nextInZone;
            }
            std::sort(top, top + filled, [](const TopEntry& a, const TopEntry& b) {
                return a.count > b.count;
            });
            fprintf(stderr, "[JIT] Top-%zu by executionCount:\n", filled);
            for (size_t k = 0; k < filled; k++) {
                std::string sel = interp_->memory().selectorOf(
                    Oop::fromRawBits(top[k].m->compiledMethodOop));
                fprintf(stderr, "[JIT]   %8u #%s (oop=0x%llx %ub tier=%u)\n",
                        top[k].count, sel.c_str(),
                        (unsigned long long)top[k].m->compiledMethodOop,
                        top[k].m->codeSize, top[k].m->tier);
            }
        }
    }

    uint64_t key = compiledMethod.rawBits();
    size_t idx = (key >> 3) % CountMapSize;

    // Simple linear probe
    for (size_t probe = 0; probe < 8; probe++) {
        size_t i = (idx + probe) % CountMapSize;
        if (countMap_[i].key == key) {
            countMap_[i].count++;
            // Runtime-configurable threshold: PHARO_JIT_THRESHOLD=N (default: CompileThreshold=2)
            static uint32_t threshold = 0;
            if (threshold == 0) {
                const char* env = getenv("PHARO_JIT_THRESHOLD");
                threshold = env ? static_cast<uint32_t>(atoi(env)) : CompileThreshold;
                if (threshold < 1) threshold = 1;
            }
            if (countMap_[i].count == threshold) {
                // Bisection: stop after N compilations
                if (maxCompile >= 0 && (int)compiler_->methodsCompiled() >= maxCompile) {
                    return;
                }
                // Selector exclusion: JIT_EXCLUDE=sel1,sel2,...
                {
                    static const char* excludeEnv = getenv("JIT_EXCLUDE");
                    if (excludeEnv && interp_) {
                        std::string sel = interp_->memory().selectorOf(compiledMethod);
                        std::string excl(excludeEnv);
                        // Simple comma-separated check
                        size_t pos = 0;
                        while (pos < excl.size()) {
                            size_t comma = excl.find(',', pos);
                            if (comma == std::string::npos) comma = excl.size();
                            std::string token = excl.substr(pos, comma - pos);
                            if (sel == token) {
                                fprintf(stderr, "[JIT] EXCLUDED #%s\n", sel.c_str());
                                return;
                            }
                            pos = comma + 1;
                        }
                    }
                }
                // Oop exclusion: JIT_EXCLUDE_OOP=0xhex1,0xhex2,...
                {
                    static const char* excludeOopEnv = getenv("JIT_EXCLUDE_OOP");
                    if (excludeOopEnv) {
                        uint64_t mOop = compiledMethod.rawBits();
                        std::string excl(excludeOopEnv);
                        size_t pos = 0;
                        while (pos < excl.size()) {
                            size_t comma = excl.find(',', pos);
                            if (comma == std::string::npos) comma = excl.size();
                            std::string tok = excl.substr(pos, comma - pos);
                            uint64_t v = strtoull(tok.c_str(), nullptr, 0);
                            if (v == mOop) {
                                fprintf(stderr, "[JIT] EXCLUDED oop=0x%llx\n",
                                        (unsigned long long)mOop);
                                return;
                            }
                            pos = comma + 1;
                        }
                    }
                }
                // Hit threshold — compile!
                size_t gcBefore = interp_ ? interp_->memory().statistics().gcCount : 0;
                JITMethod* jm = compiler_->compile(compiledMethod);
                if (interp_) {
                    size_t gcAfter = interp_->memory().statistics().gcCount;
                    if (gcAfter > gcBefore) {
                        fprintf(stderr, "[JIT] GC during compile #%zu! "
                                "(%zu GCs: %zu→%zu)\n",
                                compiler_->methodsCompiled(),
                                gcAfter - gcBefore, gcBefore, gcAfter);
                    }
                }
                if (jm) {
                    std::string t1sel = interp_ ? interp_->memory().selectorOf(compiledMethod) : "?";
                    fprintf(stderr, "[JIT] Compiled method #%zu %p '%s' (entry %u, %u bytes%s)\n",
                            compiler_->methodsCompiled(),
                            (void*)compiledMethod.rawBits(), t1sel.c_str(),
                            countMap_[i].count, jm->codeSize,
                            jm->hasPrimPrologue ? ", prim" : "");
                    // Diagnostic: if selector unresolved, dump last-literal class
                    // (usually class binding Association or outer CompiledMethod)
                    if (t1sel == "?" && interp_) {
                        auto& mem = interp_->memory();
                        size_t nLits = mem.numLiteralsOf(compiledMethod);
                        std::string lastCls = "?";
                        std::string penCls = "?";
                        if (nLits >= 1) {
                            Oop last = mem.fetchPointer(nLits, compiledMethod);
                            lastCls = mem.classNameOf(last);
                        }
                        if (nLits >= 2) {
                            Oop pen = mem.fetchPointer(nLits - 1, compiledMethod);
                            penCls = mem.classNameOf(pen);
                        }
                        fprintf(stderr, "[JIT]   nLits=%zu pen.cls=%s last.cls=%s\n",
                                nLits, penCls.c_str(), lastCls.c_str());
                    }

                    // Also attempt Tier 2 (MIR) compilation immediately.
                    // J2J bypasses tryExecute so the executionCount trigger never fires;
                    // compile eagerly so hot J2J methods benefit from register allocation.
                    static bool noT2 = !!getenv("PHARO_NO_T2");
                    static int t2Limit = getenv("T2_LIMIT") ? atoi(getenv("T2_LIMIT")) : 999;
                    if (!noT2 && tier2Compiler_ && !tier2Lookup(key) &&
                        (int)tier2Compiler_->methodsCompiled() < t2Limit) {
                        void* t2code = tier2Compiler_->compile(compiledMethod, jm);
                        // Store result or sentinel: (void*)1 means "tried, failed"
                        // so we don't retry on every activation.
                        tier2Insert(key, t2code ? t2code : (void*)1);
                        if (t2code) {
                            std::string t2sel = interp_->memory().selectorOf(compiledMethod);
                            fprintf(stderr, "[JIT] Tier 2 compiled method %p '%s' (%zu total)\n",
                                    (void*)compiledMethod.rawBits(),
                                    t2sel.c_str(),
                                    tier2Compiler_->methodsCompiled());
                        }
                    }
                }
            }
            return;
        }
        if (countMap_[i].key == 0) {
            countMap_[i].key = key;
            countMap_[i].count = 1;
            return;
        }
    }
    // Count map full for this bucket — just skip
}

bool JITRuntime::tryExecute(Oop compiledMethod, JITState& state) {
    if (!initialized_) return false;

    JITMethod* jm = methodMap_.lookup(compiledMethod.rawBits());
    if (!jm || !jm->isExecutable()) return false;

    return tryExecute(compiledMethod, state, jm);
}

bool JITRuntime::tryExecute(Oop compiledMethod, JITState& state, JITMethod* jm) {
    // Ensure code zone is writable before touching JITMethod fields.
    // Previous JIT execution (J2J trampoline, stencils) may have left the
    // per-thread W^X state in executable mode.
    makeWritable(jm->codeStart(), jm->codeSize);

    // Verify method map integrity (cheap, catches GC/rehash bugs)
    if (jm->compiledMethodOop != compiledMethod.rawBits()) {
        fprintf(stderr, "[JIT] BUG: methodMap returned wrong JITMethod! "
                "requested=0x%llx got=0x%llx\n",
                (unsigned long long)compiledMethod.rawBits(),
                (unsigned long long)jm->compiledMethodOop);
        return false;
    }

    // Touch for LRU tracking
    codeZone_.touch(jm);
    jm->executionCount++;

    // Promote hot methods: recompile Tier 1 with IC profiling data.
    // Threshold: 500 executions, tier 1 only, must have send sites.
    // (Tier 2 is now leaf-only, so skip it for methods with sends.)
    if (jm->executionCount == 500 && jm->tier == 1 && jm->numICEntries > 0) {
        if (compiler_) {
            JITMethod* newJM = compiler_->recompile(compiledMethod);
            if (newJM) {
                jm = newJM;
            }
        }
    }

    // Set up JIT state
    state.jitMethod = jm;
    state.exitReason = ExitNone;
    // j2jDepth is zeroed here; j2jSaveCursor/j2jSaveLimit are set by
    // tryJITActivation before calling tryExecute.
    state.j2jDepth = 0;

    // Validate JITState fields
    if (reinterpret_cast<uint64_t>(state.sp) < 0x10000 ||
        reinterpret_cast<uint64_t>(state.ip) < 0x10000 ||
        reinterpret_cast<uint64_t>(state.tempBase) < 0x10000 ||
        reinterpret_cast<uint64_t>(state.literals) < 0x10000) {
        fprintf(stderr, "[JIT] BUG: invalid JITState in tryExecute: sp=%p ip=%p tempBase=%p literals=%p\n",
                (void*)state.sp, (void*)state.ip,
                (void*)state.tempBase, (void*)state.literals);
        return false;
    }

    // Guard: immediate receivers can't have instance variables.
    if ((state.receiver.isSmallInteger() || state.receiver.isCharacter()) && jm->hasRecvFieldAccess) {
        return false;
    }

    // Bounds-check: receiver must have enough slots for the max field index
    if (jm->hasRecvFieldAccess && state.receiver.isObject()) {
        ObjectHeader* recvObj = reinterpret_cast<ObjectHeader*>(state.receiver.rawBits());
        if (jm->maxRecvFieldIndex >= recvObj->slotCount()) {
            return false;
        }
    }

    // Check for Tier 2 compiled code (MIR-generated, register-allocated)
    static bool noT2Exec = !!getenv("PHARO_NO_T2");
    void* t2code = noT2Exec ? nullptr : tier2Lookup(compiledMethod.rawBits());
    if (t2code && t2code != (void*)1) {
        // Tier 2 code has resume support: on initial entry, state.ip == bcStart
        // (offset 0 → start from beginning). On resume after send, state.ip
        // == bcStart + N → dispatch to post-send label.
        // Set jitMethod so chargeJITBytecodes works for T2 entries.
        state.jitMethod = jm ? jm : methodMap_.lookup(compiledMethod.rawBits());
        makeExecutable(t2code, 1);
        ((void(*)(JITState*))t2code)(&state);
        makeWritable(t2code, 1);
        return true;
    }

    // Toggle W^X to executable for JIT execution.
    makeExecutable(jm->codeStart(), jm->codeSize);

    // Call the compiled code
    JIT_CALL(jm->codeStart(), &state);

    // Back to writable (for IC patching etc.)
    makeWritable(jm->codeStart(), jm->codeSize);

    return true;
}

bool JITRuntime::tryResume(Oop compiledMethod, uint32_t bcOffset, JITState& state) {
    if (!initialized_) return false;
    static bool noResume = !!getenv("PHARO_NO_RESUME");
    if (noResume) return false;

    // Check for Tier 2 resume: set state.ip to bcBase + bcOffset and
    // call the Tier 2 function. Its prologue dispatches to the right label.
    static bool noT2Resume = !!getenv("PHARO_NO_T2");
    void* t2code = noT2Resume ? nullptr : tier2Lookup(compiledMethod.rawBits());
    if (t2code && t2code != (void*)1) {
        // Compute bcBase from method object
        ObjectHeader* methObj = reinterpret_cast<ObjectHeader*>(compiledMethod.rawBits());
        Oop hdr = methObj->slotAt(0);
        int numLits = (hdr.rawBits() & 1) ? (int)((hdr.rawBits() >> 3) & 0x7FFF) : 0;
        uint8_t* bcBase = methObj->bytes() + (1 + numLits) * 8;
        state.ip = bcBase + bcOffset;
        state.exitReason = ExitNone;
        // Set jitMethod to T1 JITMethod for countdown charging (numBytecodes).
        // T2 code doesn't use this field, but chain loop chargeJITBytecodes does.
        state.jitMethod = methodMap_.lookup(compiledMethod.rawBits());
        makeExecutable(t2code, 1);
        ((void(*)(JITState*))t2code)(&state);
        makeWritable(t2code, 1);
        return true;
    }

    JITMethod* jm = methodMap_.lookup(compiledMethod.rawBits());
    if (!jm || !jm->isExecutable()) return false;

    // Safety: refuse to resume at a bytecode offset whose stencil reads
    // operands from registers (x19-x22, "_N" variants). The stencil chain
    // before this offset would have populated those registers, but we're
    // entering cold — registers contain whatever the C caller left there.
    // Non-boolean spill-and-deopt paths in the _1 jump stencils would
    // push garbage and EXIT_SEND without updating s->ip, creating a stack
    // leak. Deferred-issues.md #4 (Session 15).
    if (getBcEntryState(jm, bcOffset) != 0) return false;

    // Ensure code zone is writable before touching JITMethod fields.
    makeWritable(jm->codeStart(), jm->codeSize);

    // Look up the code offset for this bytecode offset
    uint32_t codeOffset = jm->codeOffsetForBC(bcOffset);
    if (codeOffset == 0 || codeOffset >= jm->codeSize) return false;

    // Validate that codeOffset is within the machine code region, not the
    // literal pool / bcToCode table / IC data appended after it.
    // bcToCodeTable[numBytecodes] is the sentinel = end of machine code.
    uint32_t machineCodeEnd = jm->bcToCodeTable()[jm->numBytecodes];
    if (codeOffset >= machineCodeEnd) {
        fprintf(stderr, "[JIT] BUG: resume codeOffset %u >= machineCodeEnd %u (bc %u)\n",
                codeOffset, machineCodeEnd, bcOffset);
        return false;
    }

    // Validate state.sp is a reasonable pointer (not a SmallInteger or low address)
    if (reinterpret_cast<uint64_t>(state.sp) < 0x10000) {
        fprintf(stderr, "[JIT] BUG: resume sp=%p looks invalid\n", (void*)state.sp);
        return false;
    }

    // Touch for LRU tracking
    codeZone_.touch(jm);

    // Set up JIT state
    state.jitMethod = jm;
    state.exitReason = ExitNone;
    // Reset per-entry J2J counters. Cursor/limit are NOT cleared here —
    // the caller controls whether J2J is enabled by setting them to a valid
    // pool slice (enable) or nullptr (disable, safe default since
    // nullptr >= nullptr → true → stencils bail to EXIT_SEND_CACHED).
    state.j2jDepth = 0;
    state.j2jTotalCalls = 0;
    // Note: yieldCountdown is NOT reset here — it must persist across chain
    // loop iterations so backward jumps eventually reach 0 and yield.
    // It's initialized by tryJITActivation and reset by ExitYield handlers.

    // Validate IC data area is within code zone
    if (jm->numICEntries > 0) {
        uint32_t icSize = jm->numICEntries * 104;
        uint8_t* icStart = jm->codeStart() + jm->codeSize - icSize;
        if (icStart < codeZone_.rawStart() || icStart + icSize > codeZone_.rawStart() + codeZone_.totalBytes()) {
            fprintf(stderr, "[JIT] BUG: IC data %p outside code zone [%p, %p)\n",
                    (void*)icStart, (void*)codeZone_.rawStart(),
                    (void*)(codeZone_.rawStart() + codeZone_.totalBytes()));
            return false;
        }
    }

    // Guard: immediate receivers can't have instance variables.
    if ((state.receiver.isSmallInteger() || state.receiver.isCharacter()) && jm->hasRecvFieldAccess) {
        return false;  // Let interpreter handle it
    }

    // Bounds-check: receiver must have enough slots for the max field index
    if (jm->hasRecvFieldAccess && state.receiver.isObject()) {
        ObjectHeader* recvObj = reinterpret_cast<ObjectHeader*>(state.receiver.rawBits());
        size_t slotCount = recvObj->slotCount();
        if (jm->maxRecvFieldIndex >= slotCount) {
            return false;  // Let interpreter handle
        }
    }

    // Toggle W^X to executable for JIT execution.
    // ICache was flushed during compilation — no need to re-flush on every call.
    makeExecutable(jm->codeStart(), jm->codeSize);

    // Enter at the specified code offset
    StencilFunc entry = reinterpret_cast<StencilFunc>(jm->codeStart() + codeOffset);

    // Final sp validation just before entry
    if (reinterpret_cast<uint64_t>(state.sp) < 0x10000) {
        makeWritable(jm->codeStart(), jm->codeSize);
        fprintf(stderr, "[JIT] BUG: sp=%p just before stencil entry (bc=%u code=%u)\n",
                (void*)state.sp, bcOffset, codeOffset);
        return false;
    }

    entry(&state);

    // Back to writable (for IC patching etc.)
    makeWritable(jm->codeStart(), jm->codeSize);

    return true;
}

bool JITRuntime::tryResumeFast(JITMethod* jm, uint32_t bcOffset, JITState& state) {
    // Fast resume: caller guarantees jm is the same JITMethod we just exited
    // from, so we skip method map lookup, IC validation, receiver bounds check,
    // and LRU touch. Only does bcToCode lookup + W^X + entry.
    if (!jm->isExecutable()) return false;

    uint32_t codeOffset = jm->codeOffsetForBC(bcOffset);
    if (codeOffset == 0 || codeOffset >= jm->codeSize) return false;

    // Set up JIT state
    // Safety: refuse register-reading (_N) entry offsets — see tryResume.
    if (getBcEntryState(jm, bcOffset) != 0) return false;

    state.jitMethod = jm;
    state.exitReason = ExitNone;

    // Toggle W^X to executable
    makeExecutable(jm->codeStart(), jm->codeSize);

    StencilFunc entry = reinterpret_cast<StencilFunc>(jm->codeStart() + codeOffset);
    entry(&state);

    // Back to writable
    makeWritable(jm->codeStart(), jm->codeSize);

    return true;
}

void JITRuntime::flushCaches() {
    if (!initialized_) return;

    // Clear mega cache
    std::memset(megaCache_, 0, sizeof(megaCache_));

    // Clear IC entries but preserve selectorBits (slot 12 of each 13-slot IC).
    // selectorBits is written once at compile time and never re-patched,
    // so zeroing it would permanently disable megamorphic cache probes.
    // Layout per IC site: 4 entries × [key, method, extra] + selectorBits = 13 uint64_t = 104 bytes
    //
    // Ensure the entire code zone is writable. mprotect operates on pages,
    // so a prior makeExecutable() on one method can leave adjacent methods'
    // IC regions non-writable. Without this, flushCaches SIGSEGVs on the
    // first IC write when called after a JIT invocation. See deferred-issues #4.
    if (codeZone_.firstMethod()) {
        makeWritable(codeZone_.rawStart(), codeZone_.totalBytes());
    }
    JITMethod* m = codeZone_.firstMethod();
    while (m) {
        if (m->numICEntries > 0) {
            uint8_t* icStart = m->codeStart() + m->codeSize
                             - m->numICEntries * 104;
            for (uint32_t i = 0; i < m->numICEntries; i++) {
                uint64_t* slots = reinterpret_cast<uint64_t*>(icStart + i * 104);
                // Zero the 4 IC entries (slots 0-11) but keep slot 12 (selectorBits)
                std::memset(slots, 0, 12 * sizeof(uint64_t));
            }
        }
        m = m->nextInZone;
    }
}

void JITRuntime::recoverAfterGC(ObjectMemory& memory) {
    if (!initialized_) return;

    // Flush all IC entries: GC compaction moves method and selector oops.
    // forEachRoot visits IC slots, but there are edge cases (recompiled
    // methods, timing between patch and GC) where oops go stale.
    // Flushing is cheap — ICs re-fill on the next few misses.
    {
        JITMethod* m = codeZone_.firstMethod();
        if (m) {
            makeWritable(codeZone_.rawStart(), codeZone_.totalBytes());
        }
        while (m) {
            if (m->numICEntries > 0) {
                uint8_t* icStart = m->codeStart() + m->codeSize
                                 - m->numICEntries * 104;
                std::memset(icStart, 0, m->numICEntries * 104);
            }
            m = m->nextInZone;
        }
    }
    // Clear mega cache: keyed by selectorBits which changed.
    std::memset(megaCache_, 0, sizeof(megaCache_));

    // Update nil/true/false bits (GC may have moved them)
    updateSpecialOops(memory);

    // Rebuild MethodMap — keys are compiledMethodOop bits which were updated
    // in-place by forEachRoot during updatePointersAfterCompact, but the
    // MethodMap hash table still has the old key values.
    methodMap_.clear();
    JITMethod* m = codeZone_.firstMethod();
    while (m) {
        if (m->state == MethodState::Compiled) {
            methodMap_.insert(m->compiledMethodOop, m);
        }
        m = m->nextInZone;
    }

    // Count map: keys were updated in-place by forEachRoot during compaction,
    // but hash positions are stale (same issue as methodMap and megaCache).
    // Rehash to fix positions. Methods that are already compiled (in methodMap)
    // don't need counting anymore — drop them to reduce future probe overhead.
    {
        // Heap-allocate temp because CountMapSize*sizeof(CountEntry) = 196KB
        auto* temp = new CountEntry[CountMapSize];
        std::memcpy(temp, countMap_, sizeof(countMap_));
        std::memset(countMap_, 0, sizeof(countMap_));
        for (size_t i = 0; i < CountMapSize; i++) {
            if (temp[i].key == 0) continue;
            // Skip entries for methods already compiled — no need to count them
            if (methodMap_.lookup(temp[i].key)) continue;
            size_t idx = (temp[i].key >> 3) % CountMapSize;
            for (size_t probe = 0; probe < 8; probe++) {
                size_t slot = (idx + probe) % CountMapSize;
                if (countMap_[slot].key == 0) {
                    countMap_[slot] = temp[i];
                    break;
                }
            }
        }
        delete[] temp;
    }

    // Tier 2 map: keys were updated in-place by forEachRoot, but hash positions
    // are stale. Rehash to fix.
    {
        Tier2Entry temp[Tier2MapSize];
        std::memcpy(temp, tier2Map_, sizeof(tier2Map_));
        std::memset(tier2Map_, 0, sizeof(tier2Map_));
        size_t mask = Tier2MapSize - 1;
        for (size_t i = 0; i < Tier2MapSize; i++) {
            if (temp[i].key == 0) continue;
            tier2Insert(temp[i].key, temp[i].func);
        }
    }
}

} // namespace jit
} // namespace pharo

#endif // PHARO_JIT_ENABLED
