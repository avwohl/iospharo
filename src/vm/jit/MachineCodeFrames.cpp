// Machine-code frames (docs/mc-frames-design.md), phase 1.
//
// One frame model: T1 code runs in the interpreter's own frames, and every
// exit is a bytecode boundary that the interpreter executes itself (INV-2).
// A send that leaves machine code records where the caller's code continues
// (pendingMcReturn_ -> SavedFrame memo, INV-3), and the return of the callee
// enters that code again without the resume loop of
// tryJITResumeInCallerSlow.  The C stack stays flat (INV-4): mcfRun is the
// only C++ frame between interpret() and machine code, and its exit handling
// never sends.
//
// arm64 only (pharo_jit_osr_resume).  Opt-in: PHARO_MCF.

#include "../Interpreter.hpp"
#include "../DebugVars.hpp"
#include "JITState.hpp"
#include "JITMethod.hpp"
#include "SistaV1.hpp"
#include "TrampolineAsm.hpp"
#include <unordered_map>
#include <string>
#include <vector>
#include <algorithm>

namespace pharo {

extern uint64_t g_stepNum;

#if PHARO_JIT_ENABLED && (defined(__aarch64__) || defined(__arm64__))

extern "C" int pharo_mcf_prim(jit::JITState* st, uint32_t primAndArgs) {
    return st->interp->mcfCallPrimitive(st, (int)(primAndArgs & 0xFFFF),
                                        (int)(primAndArgs >> 16));
}

// Length of the send at ip, extension prefixes included.  ip is where T1's
// send exit points (the first byte the interpreter executes for the send).
static inline const uint8_t* mcfPastSend(const uint8_t* ip) {
    while (*ip == jit::SistaV1::ExtendA || *ip == jit::SistaV1::ExtendB) ip += 2;
    if (*ip == jit::SistaV1::ExtSend || *ip == jit::SistaV1::ExtSuperSend) return ip + 2;
    return ip + 1;
}

// Code address that continues `jm` at bytecode address `ip`, or nullptr when
// ip has no plain resume entry.  Same checks as JITRuntime::tryResume.
static inline void* mcfEntryFor(jit::JITMethod* jm, const uint8_t* ip) {
    if (jm->numBytecodes == 0) return nullptr;
    const uint8_t* bcStart = reinterpret_cast<const uint8_t*>(jm->bcStart());
    if (ip < bcStart) return nullptr;
    uint32_t bcOff = static_cast<uint32_t>(ip - bcStart);
    if (bcOff >= jm->numBytecodes) return nullptr;
    uint32_t codeOff = jm->codeOffsetForBC(bcOff);
    if (codeOff == 0 || codeOff >= jm->bcToCodeTable()[jm->numBytecodes])
        return nullptr;
    return const_cast<uint8_t*>(jm->codeStart()) + codeOff;
}

// Can compiled code run with this receiver?  (tryExecute's guards.)
static inline bool mcfReceiverOk(jit::JITMethod* jm, Oop receiver) {
    if (!jm->hasRecvFieldAccess) return true;
    if (!receiver.isObject()) return false;
    ObjectHeader* r = receiver.asObjectPtr();
    return jm->maxRecvFieldIndex < r->slotCount();
}

// May T1 code call this primitive method natively?  Only when its T1 code
// starts with a prologue that IS the primitive (emitPrimProlog_arm64):
// success returns, failure falls into the method body, which is then the
// primitive's failure code.  For any other primitive the prologue is empty
// and a native call would skip the primitive.  A method that stores the
// primitive's error code (<primitive: N error: ec>) is refused: activateMethod
// stores the error object, a native call could not.
bool Interpreter::mcfPrologueIsPrimitive(Oop method) {
    switch (primitiveIndexOf(method)) {
    case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 8: case 9:
    case 14: case 15: case 16:
    case 60: case 61: case 62:
    case 110:
    case 541: case 542: case 549:
        break;
    default:
        return false;
    }
    ObjectHeader* mo = method.asObjectPtr();
    Oop hdr = mo->slotAt(0);
    if (!hdr.isSmallInteger()) return false;
    const uint8_t* bc = mo->bytes() + (1 + (hdr.asSmallInteger() & 0x7FFF)) * 8;
    const uint8_t op = bc[3];   // first bytecode after CallPrimitive
    return !(op == jit::SistaV1::ExtStoreTemp || op == jit::SistaV1::ExtPopStoreTemp
             || (op >= jit::SistaV1::PopStoreTempBase && op <= jit::SistaV1::PopStoreTempLast));
}

// Primitives T1 code may run through pharo_mcf_prim without leaving
// machine code: they read their arguments from the stack, answer on the
// stack, and run no Smalltalk code.  Any that allocate may GC, which is
// safe (the JITState is a root); a process switch or a pushed frame is
// detected after the call and handed to the interpreter.
static bool mcfPrimCallable(int p) {
    if (p >= 1 && p <= 17) return true;          // SmallInteger arithmetic
    if (p >= 40 && p <= 59) return true;         // Float
    switch (p) {
    case 60: case 61: case 62: case 63: case 64: // at: at:put: size, string at:
    case 68: case 69:                            // objectAt: objectAt:put:
    case 70: case 71:                            // basicNew basicNew:
    case 73: case 74:                            // instVarAt: instVarAt:put:
    case 75:                                     // identityHash
    case 105:                                    // replaceFrom:to:with:startingAt:
    case 110: case 111:                          // == class
    case 148:                                    // shallowCopy
    case 169: case 170: case 171:                // ~~ and immediate conversions
    case 175:                                    // behavior hash
        return true;
    default:
        return p >= 541 && p <= 559;             // SmallFloat
    }
}

// Called from T1 code (pharo_mcf_prim) at an inline-cache hit whose method
// is a callable primitive.  Answers 1 = success (result on the stack, sp in
// the state), 0 = failure (stack unchanged; the send exits to the
// interpreter, which retries the primitive and runs the method), 2 = the
// primitive pushed a frame or switched process: the interpreter's state is
// now the truth and machine code must exit without touching it.
int Interpreter::mcfCallPrimitive(jit::JITState* st, int primIndex, int nArgs) {
    stackPointer_ = st->sp;
    if (st->method.rawBits() != method_.rawBits()
            || st->tempBase - 1 != framePointer_) {
        method_ = st->method;
        homeMethod_ = st->method;
        receiver_ = st->receiver;
        framePointer_ = st->tempBase - 1;
        argCount_ = st->argCount;
        bytecodeEnd_ = reinterpret_cast<uint8_t*>(st->method.rawBits()) + st->mcfBcEndOff;
    }
    instructionPointer_ = st->ip;   // bcStart: inside the method, for GC
    Oop* spBefore = stackPointer_;
    const size_t depth = frameDepth_;
    const uint64_t switches = processSwitchCount_;
    argCount_ = nArgs;
    primitiveFailed_ = false;
    primFailCode_ = 0;
    newMethod_ = st->cachedTarget;
    PrimitiveResult r = executePrimitive(primIndex, nArgs);
    if (frameDepth_ != depth || processSwitchCount_ != switches) {
        st->sp = stackPointer_;
        return 2;
    }
    if (r == PrimitiveResult::Success) {
        lastPrimitiveIndex_ = primIndex;
        st->sp = stackPointer_;
        return 1;
    }
    stackPointer_ = spBefore;
    primFailCode_ = 0;
    return 0;
}

bool Interpreter::mcfIsMethodFrame() const {
    return closure_.isNil()
        && method_.isObject()
        && method_.asObjectPtr()->classIndex() == compiledMethodClassIndex_;
}

bool Interpreter::mcfActivate(Oop method) {
    if (mcfActive_ || !jitInitialized_) return false;
    jit::JITMethod* jm = jitRuntime_.methodMap().lookup(method.rawBits());
    if (!jm || !jm->isExecutable() || jm->numBytecodes == 0) return false;
    if (!mcfIsMethodFrame() || !mcfReceiverOk(jm, receiver_)) return false;
    if (jm->stats) {
        jitRuntime_.codeZone().touch(jm);
        jm->stats->executionCount++;
    }
    // Phase 2: let T1 code call this method natively from now on -- when
    // it is old (the cache is keyed by oop and is not a GC root), has no
    // primitive (no prologue protocol yet), and its field accesses are
    // safe for any receiver of the classes that reach it.
    {
        const bool hasPrim = (jm->methodHeader >> 16) & 1;
        if ((!hasPrim || mcfPrologueIsPrimitive(method)) && !memory_.isYoung(method)
                && (!jm->hasRecvFieldAccess || receiver_.isObject())) {
            jit::McfCallEntry& e = jit::g_mcfCallCache[
                (method.rawBits() >> 3) & (jit::kMcfCallCacheSize - 1)];
            e.methodBits = method.rawBits();
            e.jm = jm;
            e.prim = 0;
            e.bcEndOff = static_cast<uint64_t>(
                bytecodeEnd_ - reinterpret_cast<uint8_t*>(method.rawBits()));
        }
    }
    mcfRun(jm, const_cast<uint8_t*>(jm->codeStart()));
    return true;
}

// A send left machine code for this method.  When it is a primitive T1 code
// can run through pharo_mcf_prim, record it so the next hit stays in
// machine code.  Never displaces a native-call entry for a compiled method.
void Interpreter::mcfNotePrimitiveTarget(Oop method) {
    if (!method.isObject() || method.rawBits() < 0x10000) return;
    ObjectHeader* mo = method.asObjectPtr();
    if (mo->classIndex() != compiledMethodClassIndex_) return;
    Oop hdr = mo->slotAt(0);
    if (!hdr.isSmallInteger() || !((hdr.asSmallInteger() >> 16) & 1)) return;
    if (memory_.isYoung(method)) return;
    const int p = primitiveIndexOf(method);
    if (!mcfPrimCallable(p)) return;
    jit::McfCallEntry& e = jit::g_mcfCallCache[
        (method.rawBits() >> 3) & (jit::kMcfCallCacheSize - 1)];
    if (e.methodBits == method.rawBits()) return;
    if (e.methodBits && e.jm) return;   // keep a native-call entry
    e.methodBits = method.rawBits();
    e.jm = nullptr;
    e.bcEndOff = 0;
    e.prim = static_cast<uint64_t>(p);
}

// Called once from initializeJIT: SavedFrame layout for T1's native call
// and return, and the call cache.
void Interpreter::mcfInit() {
    jit::McfFrameLayout& L = jit::g_mcfLayout;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
    L.size = sizeof(SavedFrame);
    L.savedIP = offsetof(SavedFrame, savedIP);
    L.savedBytecodeEnd = offsetof(SavedFrame, savedBytecodeEnd);
    L.savedMethod = offsetof(SavedFrame, savedMethod);
    L.savedHomeMethod = offsetof(SavedFrame, savedHomeMethod);
    L.savedReceiver = offsetof(SavedFrame, savedReceiver);
    L.savedClosure = offsetof(SavedFrame, savedClosure);
    L.savedActiveContext = offsetof(SavedFrame, savedActiveContext);
    L.materializedContext = offsetof(SavedFrame, materializedContext);
    L.ctxSynced = offsetof(SavedFrame, ctxSynced);
    L.savedFP = offsetof(SavedFrame, savedFP);
    L.materializedRetSlot = offsetof(SavedFrame, materializedRetSlot);
    L.savedArgCount = offsetof(SavedFrame, savedArgCount);
    L.homeFrameDepth = offsetof(SavedFrame, homeFrameDepth);
    L.mcReturn = offsetof(SavedFrame, mcReturn);
    L.mcKeyIP = offsetof(SavedFrame, mcKeyIP);
    L.mcJM = offsetof(SavedFrame, mcJM);
    L.mcEpoch = offsetof(SavedFrame, mcEpoch);
#pragma GCC diagnostic pop
    if (!jit::g_mcfCallCache) {
        jit::g_mcfCallCache = new jit::McfCallEntry[jit::kMcfCallCacheSize];
        jit::mcfClearCallCache();
    }
}

void Interpreter::mcfResumeAfterReturn(const SavedFrame* popped) {
    if (mcfActive_ || !running_) return;
    jit::JITMethod* jm;
    void* entry;
    if (popped && popped->mcReturn
            && popped->mcKeyIP == instructionPointer_
            && popped->mcEpoch == jit::g_mcCodeEpoch) {
        jm = static_cast<jit::JITMethod*>(popped->mcJM);
        entry = popped->mcReturn;
    } else {
        if (!method_.isObject() || method_.rawBits() < 0x10000) return;
        jm = jitRuntime_.methodMap().lookup(method_.rawBits());
        if (!jm || !jm->isExecutable()) return;
        if (!mcfIsMethodFrame() || !mcfReceiverOk(jm, receiver_)) return;
        entry = mcfEntryFor(jm, instructionPointer_);
        if (!entry) return;
    }
    mcfRun(jm, entry);
}

// PHARO_MCF_CENSUS: what would a native compiled-to-compiled call catch?
void Interpreter::mcfCensus(const jit::JITState& st) {
    static uint64_t n = 0, miss = 0, comp = 0, compPrim = 0, prim = 0, interp = 0, notMethod = 0;
    ++n;
    if (st.exitReason == jit::ExitSend) {
        ++miss;
    } else {
        Oop t = st.cachedTarget;
        if (!t.isObject() || t.rawBits() < 0x10000
                || t.asObjectPtr()->classIndex() != compiledMethodClassIndex_) {
            ++notMethod;
        } else {
            jit::JITMethod* tj = jitRuntime_.methodMap().lookup(t.rawBits());
            Oop hdr = t.asObjectPtr()->slotAt(0);
            bool hasPrim = hdr.isSmallInteger() && ((hdr.asSmallInteger() >> 16) & 1);
            if (tj && tj->isExecutable() && tj->numBytecodes) (hasPrim ? compPrim : comp)++;
            else (hasPrim ? prim : interp)++;
            {
                // Top exits by (selector, receiver class, primitive).
                static std::unordered_map<std::string, uint64_t> bySite;
                Oop rcv = st.sp[-(st.sendArgCount + 1)];
                std::string key = memory_.selectorOf(t) + " rcv=" +
                    (rcv.isObject() ? memory_.classNameOf(rcv)
                                    : rcv.isSmallInteger() ? std::string("SmI") : std::string("imm"))
                    + " prim=" + std::to_string(hasPrim ? primitiveIndexOf(t) : 0)
                    + (tj && tj->isExecutable() ? " compiled" : "")
                    + " extrasHi=" + std::to_string(st.icDataPtr ? (st.icDataPtr[2] >> 48) : 99999)
                    + " entryMatch=" + std::to_string(st.icDataPtr && st.icDataPtr[1] == t.rawBits());
                bySite[key]++;
                if ((n & 0x3FFFFF) == 0) {
                    std::vector<std::pair<uint64_t, std::string>> v;
                    for (auto& kv : bySite) v.push_back({kv.second, kv.first});
                    std::sort(v.rbegin(), v.rend());
                    for (size_t k = 0; k < v.size() && k < 12; k++)
                        fprintf(stderr, "[MCF-CENSUS]   %llu %s\n",
                                (unsigned long long)v[k].first, v[k].second.c_str());
                    bySite.clear();
                }
            }
            if (hasPrim) {
                static uint64_t primHist[1024];
                int pi = primitiveIndexOf(t);
                if (pi >= 0 && pi < 1024) primHist[pi]++;
                if ((n & 0x3FFFFF) == 0) {
                    fprintf(stderr, "[MCF-CENSUS] prims:");
                    for (int k = 0; k < 12; k++) {
                        int best = 0;
                        for (int q = 1; q < 1024; q++) if (primHist[q] > primHist[best]) best = q;
                        if (!primHist[best]) break;
                        fprintf(stderr, " p%d=%llu", best, (unsigned long long)primHist[best]);
                        primHist[best] = 0;   // destructive top-k: the next print starts over
                    }
                    fprintf(stderr, "\n");
                }
            }
        }
    }
    if ((n & 0x3FFFFF) == 0)
        fprintf(stderr, "[MCF-CENSUS] exits=%llu icMiss=%llu compiled=%llu "
                "compiledWithPrim=%llu primNotCompiled=%llu notCompiled=%llu notMethod=%llu\n",
                (unsigned long long)n, (unsigned long long)miss,
                (unsigned long long)comp, (unsigned long long)compPrim,
                (unsigned long long)prim, (unsigned long long)interp,
                (unsigned long long)notMethod);
}

void Interpreter::mcfRun(jit::JITMethod* jm, void* entry) {
    if (!mcfState_) {
        mcfState_ = new jit::JITState();
        std::memset(mcfState_, 0, sizeof(jit::JITState));
    }
    jit::JITState& st = *mcfState_;
    mcfActive_ = true;
    for (;;) {
        Oop m = method_;
        ObjectHeader* mo = m.asObjectPtr();
        st.sp = stackPointer_;
        st.receiver = receiver_;
        st.closure = closure_;
        st.literals = mo->slots() + 1;
        st.tempBase = framePointer_ + 1;
        st.memory = &memory_;
        st.interp = this;
        st.ip = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(jm->bcStart()));
        st.jitMethod = jm;
        st.method = m;
        st.argCount = argCount_;
        st.exitReason = jit::ExitNone;
        st.returnValue = Oop::fromRawBits(0);
        st.cachedTarget = Oop::fromRawBits(0);
        st.icDataPtr = nullptr;
        st.sendArgCount = 0;
        // No inline J2J: a null slice makes every J2J-capable site take its
        // SendCached exit (nullptr >= nullptr).
        st.j2jSaveCursor = nullptr;
        st.j2jSaveLimit = nullptr;
        st.j2jEntryCursor = nullptr;
        st.j2jDepth = 0;
        st.j2jTotalCalls = 0;
        st.j2jEntryDepth = 0;
        st.j2jDepthInc = 0x0000000100000001ULL;
        st.methodMapPtr = &jitRuntime_.methodMap();
        st.yieldCountdown = 1000;
        st.trueOop = memory_.trueObject();
        st.falseOop = memory_.falseObject();
        st.oldSpaceStart = memory_.oldSpaceStart();
        st.oldSpaceEnd = memory_.oldSpaceEnd();
        st.newSpaceStart = memory_.newSpaceStart();
        st.newSpaceEnd = memory_.newSpaceEnd();
        st.prevState = currentJITState_;
        st.gcIpOffset = 0;
        st.mcfFrames = savedFrames_.data();
        st.mcfFrameDepthPtr = &frameDepth_;
        st.mcfMatCtxPtr = &currentFrameMaterializedCtx_;
        st.mcfActiveCtxPtr = &activeContext_;
        st.mcfCheckCountdownPtr = &checkCountdown_;
        st.mcfDepthLimit = StackOverflowLimit - 64;
        st.mcfNil = memory_.nil();
        st.mcfEpochPtr = &jit::g_mcCodeEpoch;
        st.mcfCallCache = jit::g_mcfCallCache;
        st.mcfBcEndOff = static_cast<uint64_t>(
            bytecodeEnd_ - reinterpret_cast<uint8_t*>(m.rawBits()));
        currentJITState_ = &st;
        checkCountdown_ -= jm->numBytecodes;
        g_stepNum += jm->numBytecodes;

        pharo_jit_osr_resume(&st, entry);

        currentJITState_ = st.prevState;
        if (st.exitReason == jit::ExitDeopt) {
            // pharo_mcf_prim answered 2: a primitive pushed a frame or
            // switched process, and the interpreter's state is the truth.
            mcfActive_ = false;
            return;
        }
        stackPointer_ = st.sp;
        // Native calls and returns (phase 2) may have moved to another
        // frame: the interpreter's frame registers follow the state view.
        // Only method frames run here, so closure_ stays nil.
        jm = st.jitMethod;
        if (st.method.rawBits() != method_.rawBits()
                || st.tempBase - 1 != framePointer_) {
            method_ = st.method;
            homeMethod_ = st.method;
            receiver_ = st.receiver;
            framePointer_ = st.tempBase - 1;
            argCount_ = st.argCount;
            bytecodeEnd_ = reinterpret_cast<uint8_t*>(st.method.rawBits())
                + st.mcfBcEndOff;
        }
        switch (st.exitReason) {
        case jit::ExitSend:
            // IC miss: the interpreter's lookup patches the site (the same
            // hand-off tryJITResumeInCallerSlow makes).
            if (st.icDataPtr) {
                for (int e = 0; e < 6; e++) {
                    if (st.icDataPtr[e * 3] == 0) {
                        pendingICPatch_ = st.icDataPtr;
                        pendingICSendArgCount_ = st.sendArgCount;
                        pendingICOwnerMethod_ = method_;
                        break;
                    }
                }
            }
            [[fallthrough]];
        case jit::ExitSendCached: {
            instructionPointer_ = st.ip;
            if (st.exitReason == jit::ExitSendCached)
                mcfNotePrimitiveTarget(st.cachedTarget);

            if (__builtin_expect(GET_DEBUG_BOOL(PHARO_MCF_CENSUS), 0))
                mcfCensus(st);
            const uint8_t* after = mcfPastSend(st.ip);
            void* cont = mcfEntryFor(jm, after);
            if (cont) {
                pendingMcReturn_ = cont;
                pendingMcKeyIP_ = after;
                pendingMcJM_ = jm;
                pendingMcEpoch_ = jit::g_mcCodeEpoch;
            }
            mcfActive_ = false;
            return;
        }
        case jit::ExitReturn: {
            // T1 code returns only from method frames here (mcfRun runs no
            // block frames), which is returnFromMethod's fast case.
            Oop value = st.returnValue;
            size_t depth = frameDepth_;
            if (depth == 0 || savedFrames_[depth - 1].homeFrameDepth != SIZE_MAX) {
                returnFromMethodSlow(value);
                mcfActive_ = false;
                return;
            }
            returnValue(value);
            if (!running_ || frameDepth_ != depth - 1 || checkCountdown_ <= 0) {
                mcfActive_ = false;
                return;
            }
            const SavedFrame& popped = savedFrames_[frameDepth_];
            if (popped.mcReturn
                    && popped.mcKeyIP == instructionPointer_
                    && popped.mcEpoch == jit::g_mcCodeEpoch) {
                jm = static_cast<jit::JITMethod*>(popped.mcJM);
                entry = popped.mcReturn;
                continue;
            }
            if (!method_.isObject() || method_.rawBits() < 0x10000) break;
            jm = jitRuntime_.methodMap().lookup(method_.rawBits());
            if (!jm || !jm->isExecutable()) break;
            if (!mcfIsMethodFrame() || !mcfReceiverOk(jm, receiver_)) break;
            entry = mcfEntryFor(jm, instructionPointer_);
            if (!entry) break;
            continue;
        }
        case jit::ExitArithOverflow:   // general "execute this bytecode" exit
        case jit::ExitBlockCreate:
        case jit::ExitArrayCreate:
        case jit::ExitMustBool:
            instructionPointer_ = st.ip;
            break;
        default: {
            char msg[96];
            snprintf(msg, sizeof msg, "mcfRun: unexpected JIT exit reason %d",
                     (int)st.exitReason);
            mcfActive_ = false;
            stopVM(msg);
            return;
        }
        }
        mcfActive_ = false;
        return;
    }
}

#elif PHARO_JIT_ENABLED
// Other architectures: mcfEnabled() is false, so these are never reached;
// they exist so the interpreter's hooks link.
bool Interpreter::mcfIsMethodFrame() const { return false; }
bool Interpreter::mcfActivate(Oop) { return false; }
void Interpreter::mcfResumeAfterReturn(const SavedFrame*) {}
void Interpreter::mcfRun(jit::JITMethod*, void*) {}
void Interpreter::mcfCensus(const jit::JITState&) {}
void Interpreter::mcfInit() {}
bool Interpreter::mcfPrologueIsPrimitive(Oop) { return false; }
int Interpreter::mcfCallPrimitive(jit::JITState*, int, int) { return 0; }
void Interpreter::mcfNotePrimitiveTarget(Oop) {}
#endif  // PHARO_JIT_ENABLED && arm64

}  // namespace pharo
