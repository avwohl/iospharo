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
#include <cstdlib>

namespace pharo {

extern uint64_t g_stepNum;
extern uint64_t g_objectMotionEpoch;   // ObjectMemory.cpp

#if PHARO_JIT_ENABLED && (defined(__aarch64__) || defined(__arm64__))

extern "C" void pharo_mcf_verify_eq(jit::JITState* st, uint64_t rcv, uint64_t arg,
                                    uint64_t answer, uint64_t where) {
    st->interp->mcfVerifyEq(st, Oop::fromRawBits(rcv), Oop::fromRawBits(arg),
                            Oop::fromRawBits(answer), where);
}

extern "C" void pharo_mcf_verify_call(jit::JITState* st, uint64_t method, uint64_t where) {
    st->interp->mcfVerifyCall(st, Oop::fromRawBits(method), where);
}

extern "C" void pharo_mcf_verify_ret(jit::JITState* st, uint64_t value, void* frame) {
    st->interp->mcfVerifyRet(st, Oop::fromRawBits(value), frame);
}

extern "C" void pharo_mcf_verify_push(jit::JITState* st) {
    st->interp->mcfVerifyPush(st);
}

// Called from T1 code at a naked ExtSend / ExtSuperSend, which have no
// inline cache: answers the call-cache entry of the method the send runs
// when T1 code may call it natively, else nullptr (the send then exits to
// the interpreter with a return memo).  desc = literal index | nArgs << 8
// | super << 16.  Looks up only; no Smalltalk runs, nothing allocates.
extern "C" void* pharo_mcf_extsend(jit::JITState* st, uint32_t desc) {
    return st->interp->mcfExtSendTarget(st, desc);
}

// Called from the native block stub for a block with a non-local return,
// after the block's frame record is pushed: store its home frame index.
extern "C" void pharo_mcf_block_home(jit::JITState* st, uint64_t closure) {
    st->interp->mcfSetBlockHome(Oop::fromRawBits(closure));
}

// Called from a compiled Context method before it reads sender/pc/stackp
// or stores a field, while lazy context stubs are pending.  st->ip is the
// bytecode; materializeFrameStack only allocates (no GC can run).
extern "C" void pharo_mcf_ctx_sync(jit::JITState* st) {
    st->interp->mcfCtxSync(st);
}

// PushFullBlock in machine code: create the closure without leaving.
// st->ip is the bytecode; the interpreter would have it past the 3 bytes.
// Allocation cannot GC here, so the machine code's registers stay valid.
extern "C" uint64_t pharo_mcf_block_create(jit::JITState* st, uint64_t packed) {
    return st->interp->mcfBlockCreate(st, packed);
}

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
    case 165: case 166:                          // integerAt: integerAt:put:
    case 85: case 86:                            // signal wait (a switch -> 2)
    case 195:                                    // findNextUnwindContextUpTo:
    case 210: case 211: case 212:                // Context tempAt: tempAt:put: size
    case 169: case 170: case 171:                // ~~ and immediate conversions
    case 175:                                    // behavior hash
        return true;
    default:
        return (p >= 541 && p <= 559)            // SmallFloat
            || (p >= 256 && p <= 519);           // quick: constant or field
    }
}

// May T1 code run this primitive method's primitive without leaving machine
// code?  mcfPrimCallable, plus named primitive 117 for the pure string
// functions of MiscPrimitivePlugin (no Smalltalk, no allocation; lexicon
// sent indexOfAscii:inString:startingAt: and friends 5M times).
bool Interpreter::mcfPrimMethodCallable(Oop method, int p) {
    if (mcfPrimCallable(p)) return true;
    if (p != 117) return false;
    ObjectHeader* mo = method.asObjectPtr();
    Oop hdr = mo->slotAt(0);
    if (!hdr.isSmallInteger() || (hdr.asSmallInteger() & 0x7FFF) < 1) return false;
    Oop spec = mo->slotAt(1);
    if (!spec.isObject() || spec.isNil() || !spec.asObjectPtr()->isPointersObject()
            || spec.asObjectPtr()->slotCount() < 2) return false;
    Oop module = spec.asObjectPtr()->slotAt(0);
    Oop fn = spec.asObjectPtr()->slotAt(1);
    if (memory_.symbolEquals(module, "FilePlugin"))
        return memory_.symbolEquals(fn, "primitiveFileGetPosition")
            || memory_.symbolEquals(fn, "primitiveFileSetPosition")
            || memory_.symbolEquals(fn, "primitiveFileSize")
            || memory_.symbolEquals(fn, "primitiveFileRead")
            || memory_.symbolEquals(fn, "primitiveFileAtEnd");
    if (!memory_.symbolEquals(module, "MiscPrimitivePlugin")) return false;
    return memory_.symbolEquals(fn, "primitiveStringHash")
        || memory_.symbolEquals(fn, "primitiveIndexOfAsciiInString")
        || memory_.symbolEquals(fn, "primitiveFindSubstring")
        || memory_.symbolEquals(fn, "primitiveFindFirstInString")
        || memory_.symbolEquals(fn, "primitiveTranslateStringWithTable");
}

// Called from T1 code (pharo_mcf_prim) at an inline-cache hit whose method
// is a callable primitive.  Answers 1 = success (result on the stack, sp in
// the state), 0 = failure (stack unchanged; the send exits to the
// interpreter, which retries the primitive and runs the method), 2 = the
// primitive pushed a frame or switched process: the interpreter's state is
// now the truth and machine code must exit without touching it.
int Interpreter::mcfCallPrimitive(jit::JITState* st, int primIndex, int nArgs) {
    stackPointer_ = st->sp;
    mcfSyncRegisters(*st);           // unconditionally: see mcfRun
    // The ip after the send (both callers store it): a process switch or a
    // lazy-context sync inside the primitive records it as the pc.
    instructionPointer_ = st->ip;
    Oop* spBefore = stackPointer_;
    const size_t depth = frameDepth_;
    const uint64_t switches = processSwitchCount_;
    argCount_ = nArgs;
    primitiveFailed_ = false;
    primFailCode_ = 0;
    newMethod_ = st->cachedTarget;
    if (__builtin_expect(GET_DEBUG_STR(PHARO_MCF_WATCH_OOP) != nullptr, 0))
        mcfWatchStore("mcf-prim", primIndex, nArgs);
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

// PHARO_MCF_VERIFY: the memo about to be used must be the lookup it
// stands for -- code of the frame's own method, entry for its ip.
void Interpreter::mcfVerifyMemo(const SavedFrame& f, const char* site) {
    auto* jm = static_cast<jit::JITMethod*>(f.mcJM);
    const char* why = nullptr;
    if (!jm || jm->compiledMethodOop != f.savedMethod.rawBits()) why = "jm-method";
    else if (f.savedMethod.rawBits() != method_.rawBits()) why = "frame-method";
    else {
        void* want = mcfEntryFor(jm, f.savedIP);
        if (want && want != f.mcReturn) {
            // A native call memo points at the send's continuation, which
            // may precede the next bytecode's label; accept any address
            // inside the method's code.
            const uint8_t* c = jm->codeStart();
            const uint8_t* r = static_cast<const uint8_t*>(f.mcReturn);
            if (r < c || r >= c + jm->bcToCodeTable()[jm->numBytecodes]) why = "entry";
        }
    }
    if (!why) return;
    fprintf(stderr, "[MCF-VERIFY] %s: bad memo (%s) method=#%s jmMethod=0x%llx "
            "savedMethod=0x%llx savedIP=%p keyIP=%p fd=%zu\n", site, why,
            memory_.selectorOf(f.savedMethod).c_str(),
            jm ? (unsigned long long)jm->compiledMethodOop : 0ULL,
            (unsigned long long)f.savedMethod.rawBits(),
            (const void*)f.savedIP, (const void*)f.mcKeyIP, frameDepth_);
    stopVM("MCF memo verification failed");
}

// PHARO_MCF_VERIFY: T1 just pushed a SavedFrame natively and set up the
// callee view.  Check the record against what pushFrame would write.
void Interpreter::mcfVerifyPush(jit::JITState* st) {
    const SavedFrame& f = savedFrames_[frameDepth_ - 1];
    const char* why = nullptr;
    ObjectHeader* mo = f.savedMethod.asObjectPtr();
    Oop hdr = mo->slotAt(0);
    const int nLits = (int)(hdr.asSmallInteger() & 0x7FFF);
    const int nArgsCaller = (int)((hdr.asSmallInteger() >> 24) & 0xF);
    const uint8_t* bc0 = mo->bytes() + (1 + nLits) * 8;
    const uint8_t* end = mo->bytes() + mo->compiledCodeByteSize();
    if (f.savedIP <= bc0 || f.savedIP > end) why = "savedIP outside caller";
    else if (f.savedBytecodeEnd != end) why = "savedBytecodeEnd";

    else if (f.savedFP[0].rawBits() != (f.savedClosure.isNil() ? f.savedReceiver
                                                               : f.savedClosure).rawBits())
        why = "fp[0] is neither the receiver nor the closure";
    else if (f.savedArgCount != nArgsCaller) why = "argCount";
    else if (f.mcKeyIP != f.savedIP) why = "keyIP";
    else if (st->tempBase[-1].rawBits() != (st->closure.isNil() ? st->receiver
                                                              : st->closure).rawBits())
        why = "callee fp[0]";
    else if (st->tempBase - 1 <= f.savedFP) why = "callee fp below caller fp";
    else {
        Oop ch = st->method.asObjectPtr()->slotAt(0);
        const int nArgsCallee = (int)((ch.asSmallInteger() >> 24) & 0xF);
        const int nTemps = (int)((ch.asSmallInteger() >> 18) & 0x3F);
        if (nArgsCallee != st->argCount) why = "callee argCount";
        else if (st->sp != st->tempBase + nTemps) why = "callee sp";
        else if (st->closure.isNil()) {   // a block's copied values are not nil
            for (int t = nArgsCallee; t < nTemps; t++)
                if (!st->tempBase[t].isNil()) { why = "temp not nil"; break; }
        }
    }
    static int n = 0;
    if (!why || ++n > 10) return;
    fprintf(stderr, "[MCF-VERIFY-PUSH] %s: caller=#%s callee=#%s fd=%zu\n", why,
            memory_.selectorOf(f.savedMethod).c_str(), memory_.selectorOf(st->method).c_str(),
            frameDepth_);
}

// PHARO_MCF_WATCH_OOP: is a store of the watched oop about to happen?  The
// value is the top of stack (at:put:, instVarAt:put:, ...).
void Interpreter::mcfWatchStore(const char* site, int primIndex, int nArgs) {
    static uint64_t watch = std::strtoull(GET_DEBUG_STR(PHARO_MCF_WATCH_OOP), nullptr, 16);
    if (stackPointer_[-1].rawBits() != watch) return;
    static int n = 0;
    if (++n > 8) return;
    Oop rcv = stackPointer_[-(nArgs + 1)];
    fprintf(stderr, "[MCF-WATCH] %s prim=%d stores 0x%llx into 0x%llx(%s) in #%s; chain:", site,
            primIndex, (unsigned long long)watch, (unsigned long long)rcv.rawBits(),
            rcv.isObject() ? memory_.classNameOf(rcv).c_str() : "imm",
            memory_.selectorOf(method_).c_str());
    for (size_t d = frameDepth_; d-- > 0 && d + 8 > frameDepth_;)
        fprintf(stderr, " <- #%s", memory_.selectorOf(savedFrames_[d].savedMethod).c_str());
    fprintf(stderr, "\n");
}

// PHARO_MCF_VERIFY: a native return is about to pop `frame`, which must be
// the top record, from a method frame whose receiver is fp[0].
void Interpreter::mcfVerifyRet(jit::JITState* st, Oop value, void* framePtr) {
    (void)value;
    mcfVerifyReceiver(st, "native-return");
    if (framePtr != &savedFrames_[frameDepth_ - 1]) {
        static int n = 0;
        if (++n <= 10)
            fprintf(stderr, "[MCF-VERIFY-RET] frame %p is not the top record %p (fd=%zu)\n",
                    framePtr, (void*)&savedFrames_[frameDepth_ - 1], frameDepth_);
    }
}

// PHARO_MCF_VERIFY: the IC entry T1 is about to call through must hold
// the method a lookup finds for the send at `where`.
void Interpreter::mcfVerifyCall(jit::JITState* st, Oop method, uint64_t where) {
    const uint32_t bcOff = (uint32_t)(where & 0xFFFFFFFF);
    const int nArgs = (int)(where >> 32);
    auto* jm = st->jitMethod;
    const uint8_t op = reinterpret_cast<const uint8_t*>(jm->bcStart())[bcOff];
    Oop sel;
    if (op >= jit::SistaV1::SpecialSendBase - 16 && op < jit::SistaV1::SpecialSendBase + 16) {
        // 0x60-0x7F: special selectors 0..31 (arithmetic, then the rest).
        Oop ss = memory_.specialObject(SpecialObjectIndex::SpecialSelectorsArray);
        sel = ss.asObjectPtr()->slotAt((op - (jit::SistaV1::SpecialSendBase - 16)) * 2);
    } else {
        sel = st->method.asObjectPtr()->slotAt(1 + (op & 0xF));
    }
    Oop rcv = st->sp[-(nArgs + 1)];
    Oop found = lookupMethod(sel, memory_.classOf(rcv));
    static int n = 0;
    if (found.rawBits() == method.rawBits() || ++n > 20) return;
    fprintf(stderr, "[MCF-VERIFY-CALL] in #%s bc=%u op=0x%02x sel=#%s rcv=%s cached=#%s(0x%llx) "
            "lookup=#%s(0x%llx)\n", memory_.selectorOf(st->method).c_str(), bcOff, op,
            memory_.oopToString(sel).c_str(),
            rcv.isObject() ? memory_.classNameOf(rcv).c_str() : "imm",
            memory_.selectorOf(method).c_str(), (unsigned long long)method.rawBits(),
            found.isObject() ? memory_.selectorOf(found).c_str() : "?",
            (unsigned long long)found.rawBits());
}

// PHARO_MCF_VERIFY: in a method frame the receiver is fp[0].
void Interpreter::mcfVerifyReceiver(jit::JITState* st, const char* site) {
    if (!st->closure.isNil()) return;   // block frame: fp[0] is the closure
    if (st->tempBase[-1].rawBits() == st->receiver.rawBits()) return;
    static int n = 0;
    if (++n > 10) return;
    fprintf(stderr, "[MCF-VERIFY-RCV] %s: #%s state.receiver=0x%llx(%s) fp[0]=0x%llx(%s) fd=%zu\n",
            site, memory_.selectorOf(st->method).c_str(),
            (unsigned long long)st->receiver.rawBits(),
            st->receiver.isObject() ? memory_.classNameOf(st->receiver).c_str() : "imm",
            (unsigned long long)st->tempBase[-1].rawBits(),
            st->tempBase[-1].isObject() ? memory_.classNameOf(st->tempBase[-1]).c_str() : "imm",
            frameDepth_);
}

void Interpreter::mcfVerifyEq(jit::JITState* st, Oop rcv, Oop arg, Oop answer, uint64_t where) {
    mcfVerifyReceiver(st, "==");
    const bool ne = (where >> 32) == jit::SistaV1::SpecialSendBase + 8;
    const bool same = memory_.followForwarded(rcv).rawBits() == memory_.followForwarded(arg).rawBits();
    Oop want = (same != ne) ? memory_.trueObject() : memory_.falseObject();
    static int n = 0;
    if (want.rawBits() == answer.rawBits() || ++n > 20) return;
    fprintf(stderr, "[MCF-VERIFY-EQ] #%s bc=%u rcv=0x%llx arg=0x%llx answer=0x%llx want=0x%llx "
            "true=0x%llx false=0x%llx\n", memory_.selectorOf(st->method).c_str(),
            (unsigned)(where & 0xFFFFFFFF), (unsigned long long)rcv.rawBits(),
            (unsigned long long)arg.rawBits(), (unsigned long long)answer.rawBits(),
            (unsigned long long)want.rawBits(),
            (unsigned long long)memory_.trueObject().rawBits(),
            (unsigned long long)memory_.falseObject().rawBits());
}

// A block frame (closure_ set, method_ a CompiledBlock), set up by
// activateBlock, popFrame or a native block call.
bool Interpreter::mcfIsBlockFrame() const {
    if (!GET_DEBUG_BOOL(PHARO_MCF_BLOCKS)) return false;
    return !closure_.isNil()
        && method_.isObject()
        && method_.asObjectPtr()->classIndex() == compiledBlockClassIndex_;
}

void Interpreter::mcfActivateBlock() {
    if (mcfActive_ || !jitInitialized_) return;
    if (closure_.isNil() || !method_.isObject()
            || method_.asObjectPtr()->classIndex() != compiledBlockClassIndex_) return;
    jit::JITMethod* jm = jitRuntime_.methodMap().lookup(method_.rawBits());
    if (!jm || !jm->isExecutable() || jm->numBytecodes == 0 || jm->isStubOnEntry) return;
    if (instructionPointer_ != reinterpret_cast<const uint8_t*>(jm->bcStart())) return;
    // Phase 3b: let T1 code call this block natively at value / value:.
    // A block with ^ is flagged: the stub then finds its home frame.
    const bool nlr = mcfBlockHasNLR(method_);
    if (!memory_.isYoung(method_) && (!nlr || !GET_DEBUG_BOOL(PHARO_MCF_NO_NLR_BLOCK_CALL))) {
        Oop hdr = method_.asObjectPtr()->slotAt(0);
        jit::McfCallEntry& e = *jit::mcfCallSlotForCall(method_.rawBits());
        e.methodBits = method_.rawBits();
        e.jm = jm;
        e.bcEndOff = static_cast<uint64_t>(
            bytecodeEnd_ - reinterpret_cast<uint8_t*>(method_.rawBits()));
        e.prim = jit::kMcfBlockEntry | (uint64_t)((hdr.asSmallInteger() >> 24) & 0xF)
                 | (nlr ? jit::kMcfBlockNLR : 0);
    }
    if (!mcfIsBlockFrame()) return;
    if (jm->stats) {
        jitRuntime_.codeZone().touch(jm);
        jm->stats->executionCount++;
    }
    mcfRun(jm, const_cast<uint8_t*>(jm->codeStart()));
}

// The interpreter's frame registers from the machine-code state view, for
// the frame machine code is in now.  Unconditional: a native return followed
// by a native call can leave a NEW activation of the same method at the same
// fp with another receiver, so "same method and fp" is not "same frame".
void Interpreter::mcfSyncRegisters(const jit::JITState& st) {
    method_ = st.method;
    receiver_ = st.receiver;
    framePointer_ = st.tempBase - 1;
    argCount_ = st.argCount;
    bytecodeEnd_ = reinterpret_cast<uint8_t*>(st.method.rawBits()) + st.mcfBcEndOff;
    if (st.closure.isNil()) {
        closure_ = st.closure;
        homeMethod_ = st.method;
    } else {
        // A block frame: activateBlock's choice of home method.
        closure_ = st.closure;
        ObjectHeader* clo = st.closure.asObjectPtr();
        Oop cb = clo->slotAt(1);
        Oop home = cb.isObject() ? memory_.fetchPointer(2, cb) : Oop::nil();
        homeMethod_ = home.isObject() ? home
                                      : memory_.fetchPointer(3, clo->slotAt(0));
    }
}

bool Interpreter::mcfIsMethodFrame() const {
    if (!(closure_.isNil()
          && method_.isObject()
          && method_.asObjectPtr()->classIndex() == compiledMethodClassIndex_))
        return false;
    if (__builtin_expect(GET_DEBUG_INT(PHARO_MCF_COMPILE_MOD) > 0, 0)) {
        std::string sel = memory_.selectorOf(method_);
        uint32_t h = 2166136261u;
        for (char c : sel) h = (h ^ (uint8_t)c) * 16777619u;
        return (int)(h % (uint32_t)GET_DEBUG_INT(PHARO_MCF_COMPILE_MOD))
               == GET_DEBUG_INT(PHARO_MCF_COMPILE_REM);
    }
    return true;
}

bool Interpreter::mcfActivate(Oop method) {
    if (mcfActive_ || !jitInitialized_) return false;
    jit::JITMethod* jm = jitRuntime_.methodMap().lookup(method.rawBits());
    if (__builtin_expect(GET_DEBUG_STR(PHARO_MCF_WATCH_SEL) != nullptr, 0)
            && memory_.selectorOf(method) == GET_DEBUG_STR(PHARO_MCF_WATCH_SEL)) {
        static std::unordered_map<std::string, uint64_t> whyCount;
        static uint64_t n = 0;
        {
            const char* why = !jm ? "no-jm" : !jm->isExecutable() ? "not-exec"
                : jm->numBytecodes == 0 ? "no-bc" : !mcfIsMethodFrame() ? "not-method-frame"
                : !mcfReceiverOk(jm, receiver_) ? "receiver" : memory_.isYoung(method) ? "young"
                : ((jm->methodHeader >> 16) & 1) && !mcfPrologueIsPrimitive(method) ? "prim"
                : (jm->hasRecvFieldAccess && !receiver_.isObject()) ? "recv-field" : "ok";
            whyCount[classNameOfMethod(method) + " " + why +
                     (jm && jm->isStubOnEntry ? " stub" : "")]++;
            if ((++n & 0xFFFF) == 0)
                for (auto& kv : whyCount)
                    fprintf(stderr, "[MCF-WATCH] %llu %s\n", (unsigned long long)kv.second,
                            kv.first.c_str());
        }
    }
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
        bool bisectOk = true;
        if (__builtin_expect(GET_DEBUG_INT(PHARO_MCF_CALL_MOD) > 0, 0)) {
            std::string sel = memory_.selectorOf(method);
            uint32_t h = 2166136261u;
            for (char c : sel) h = (h ^ (uint8_t)c) * 16777619u;
            bisectOk = (int)(h % (uint32_t)GET_DEBUG_INT(PHARO_MCF_CALL_MOD))
                       == GET_DEBUG_INT(PHARO_MCF_CALL_REM);
            if (bisectOk) {
                static int nLog = 0;
                if (++nLog <= 40)
                    fprintf(stderr, "[MCF-CALL-BISECT] native-callable %s>>#%s\n",
                            classNameOfMethod(method).c_str(), sel.c_str());
            }
        }
        if (bisectOk && (!hasPrim || mcfPrologueIsPrimitive(method)) && !memory_.isYoung(method)
                && (!jm->hasRecvFieldAccess || receiver_.isObject())) {
            jit::McfCallEntry& e = *jit::mcfCallSlotForCall(method.rawBits());
            e.methodBits = method.rawBits();
            e.jm = jm;
            e.prim = 0;
            e.bcEndOff = static_cast<uint64_t>(
                bytecodeEnd_ - reinterpret_cast<uint8_t*>(method.rawBits()));
        }
    }
    // activateMethod has already run (and failed) the primitive and stored
    // its error code, and set ip past both: enter at that bytecode, not at
    // codeStart, whose prologue would run the primitive again and whose
    // error-code store would pop a value that is not on the stack.
    void* entry = const_cast<uint8_t*>(jm->codeStart());
    if (instructionPointer_ != reinterpret_cast<const uint8_t*>(jm->bcStart())) {
        entry = mcfEntryFor(jm, instructionPointer_);
        if (!entry) return false;
    }
    mcfRun(jm, entry);
    return true;
}

uint64_t Interpreter::mcfBlockCreate(jit::JITState* st, uint64_t packed) {
    stackPointer_ = st->sp;
    mcfSyncRegisters(*st);
    instructionPointer_ = st->ip + 3;
    const int litIndex = static_cast<int>(packed & 0xFFFF);
    const int flags = static_cast<int>((packed >> 32) & 0xFF);
    createFullBlockWithLiteral(litIndex, flags & 0x3F, (flags >> 7) & 1, (flags >> 6) & 1);
    st->sp = stackPointer_;
    return 1;
}

void Interpreter::mcfCtxSync(jit::JITState* st) {
    if (!lazyCtxPending_) return;
    stackPointer_ = st->sp;
    mcfSyncRegisters(*st);
    instructionPointer_ = st->ip;
    materializeFrameStack();
}

void Interpreter::mcfSetBlockHome(Oop closure) {
    if (frameDepth_ < 1) return;
    ObjectHeader* clo = closure.asObjectPtr();
    Oop compiledBlock = clo->slotAt(1);
    Oop outerContext = clo->slotAt(0);
    // Memo of the last answer.  findBlockHomeFrame first looks for the frame
    // whose context IS the closure's outer context and whose method is the
    // block's home method: one frame at most can match, so a frame that
    // still matches is its answer.  A do: loop calls the same block once
    // per element and the search walked the frames each time (6% of
    // myprecious round trips).  Cleared at every GC.
    if (compiledBlock.rawBits() == nlrMemoBlock_.rawBits()
            && nlrMemoFrame_ < frameDepth_
            && outerContext.isObject() && !outerContext.isNil()
            && savedFrames_[nlrMemoFrame_].materializedContext.rawBits() == outerContext.rawBits()
            && savedFrames_[nlrMemoFrame_].savedMethod.rawBits() == nlrMemoHome_.rawBits()) {
        savedFrames_[frameDepth_ - 1].homeFrameDepth = nlrMemoFrame_;
        return;
    }
    Oop home = blockHomeMethodForNLR(compiledBlock);
    size_t homeFrame = SIZE_MAX;
    if (home.isObject() && !home.isNil()) {
        Oop primaryHome = home;
        homeFrame = findBlockHomeFrame(home, outerContext, compiledBlock);
        if (homeFrame != SIZE_MAX && outerContext.isObject() && !outerContext.isNil()
                && savedFrames_[homeFrame].materializedContext.rawBits() == outerContext.rawBits()
                && savedFrames_[homeFrame].savedMethod.rawBits() == primaryHome.rawBits()) {
            nlrMemoBlock_ = compiledBlock;
            nlrMemoHome_ = primaryHome;
            nlrMemoFrame_ = homeFrame;
        }
    }
    savedFrames_[frameDepth_ - 1].homeFrameDepth = homeFrame;
}

void* Interpreter::mcfExtSendTarget(jit::JITState* st, uint32_t desc) {
    // st->ip is the send (2 bytes: no prefix).  A primitive run below sees
    // the ip after it, as in the interpreter; on failure T1 code exits at
    // the send again.
    uint8_t* const sendIp = st->ip;
    st->ip += 2;
    struct RestoreIp { jit::JITState* s; uint8_t* ip; ~RestoreIp() { s->ip = ip; } } restoreIp{st, sendIp};
    const int litIdx = desc & 0xFF;
    const int nArgs = (desc >> 8) & 0xFF;
    const bool super = (desc >> 16) & 1;
    const bool special = (desc >> 17) & 1;   // litIdx is a special selector index
    Oop method = st->method;
    if (!method.isObject() || method.rawBits() < 0x10000) return nullptr;
    ObjectHeader* mo = method.asObjectPtr();
    Oop selector;
    if (special) {
        // A special send is 1 byte, not 2: ip after it is ip + 1.
        st->ip -= 1;
        Oop ss = memory_.specialObject(SpecialObjectIndex::SpecialSelectorsArray);
        if (!ss.isObject() || (size_t)(litIdx * 2) >= ss.asObjectPtr()->slotCount())
            return nullptr;
        selector = ss.asObjectPtr()->slotAt(litIdx * 2);
    } else {
        Oop hdr = mo->slotAt(0);
        if (!hdr.isSmallInteger() || litIdx >= (int)(hdr.asSmallInteger() & 0x7FFF))
            return nullptr;
        selector = mo->slotAt(1 + litIdx);
    }
    Oop rcv = st->sp[-(nArgs + 1)];
    Oop cls;
    if (super) {
        // A naked super send is the method form (blocks use directed super
        // sends): the superclass of the method's class, as the interpreter.
        if (mo->classIndex() != compiledMethodClassIndex_) return nullptr;
        Oop mc = methodClassOf(method);
        if (!mc.isObject() || mc.isNil()) return nullptr;
        cls = superclassOf(mc);
    } else {
        cls = memory_.classOf(rcv);
    }
    if (!cls.isObject() || cls.isNil()) return nullptr;
    Oop target = lookupMethodForSend(selector, cls);
    if (!target.isObject() || target.isNil()) return nullptr;
    jit::McfCallEntry* e = jit::mcfCallFind(target.rawBits());
    if (e && e->jm && !e->prim && mcfReceiverOk(e->jm, rcv)) return e;
    if (e && !e->jm && e->prim && e->prim < 1024) {
        // A primitive entry (mcfNotePrimitiveTarget): run it.
        st->cachedTarget = target;
        switch (mcfCallPrimitive(st, (int)e->prim, nArgs)) {
        case 1: return kMcfExtDone;
        case 2: return kMcfExtDeopt;
        default: return nullptr;
        }
    }
    // Not a native call.  The sends cacheMethod shortcuts (they never
    // activate, so never reach the call cache) run here: ^self, getter.
    if (MethodCacheEntry* ce = probeCache(selector, cls)) {
        if (ce->method.rawBits() == target.rawBits() && ce->trivialShape) {
            if (ce->returnsSelf) {
                st->sp -= nArgs;
                return kMcfExtDone;
            }
            if (ce->accessorIndex >= 0 && nArgs == 0 && rcv.isObject()
                    && rcv.rawBits() > 0x10000) {
                ObjectHeader* rh = rcv.asObjectPtr();
                if (!rh->isBytesObject() && (size_t)ce->accessorIndex < rh->slotCount()) {
                    st->sp[-1] = rh->slotAt(ce->accessorIndex);
                    return kMcfExtDone;
                }
            }
        }
    }
    // A quick primitive (256-519): a constant or a receiver field, as the
    // interpreter answers it without an activation.
    Oop thdr = target.asObjectPtr()->slotAt(0);
    if (target.asObjectPtr()->classIndex() == compiledMethodClassIndex_
            && thdr.isSmallInteger() && ((thdr.asSmallInteger() >> 16) & 1)) {
        const int qp = primitiveIndexOf(target);
        if (qp >= 256 && qp <= 519 && nArgs == 0) {
            Oop v;
            bool ok = true;
            switch (qp) {
            case 256: v = rcv; break;
            case 257: v = memory_.trueObject(); break;
            case 258: v = memory_.falseObject(); break;
            case 259: v = memory_.nil(); break;
            case 260: case 261: case 262: case 263:
                v = Oop::fromSmallInteger(qp - 261); break;
            default: {
                size_t idx = (size_t)(qp - 264);
                ok = rcv.isObject() && rcv.rawBits() > 0x10000
                     && !rcv.asObjectPtr()->isBytesObject()
                     && idx < rcv.asObjectPtr()->slotCount()
                     && !isContextObject(rcv);
                if (ok) v = rcv.asObjectPtr()->slotAt(idx);
            }
            }
            if (ok) {
                st->sp[-1] = v;
                return kMcfExtDone;
            }
        }
    }
    // A primitive T1 code may run without leaving machine code.
    if (target.asObjectPtr()->classIndex() == compiledMethodClassIndex_
            && thdr.isSmallInteger() && ((thdr.asSmallInteger() >> 16) & 1)) {
        const int p = primitiveIndexOf(target);
        if (mcfPrimMethodCallable(target, p)) {
            mcfNotePrimitiveTarget(target, rcv);   // next time: the entry above
            st->cachedTarget = target;
            switch (mcfCallPrimitive(st, p, nArgs)) {
            case 1: return kMcfExtDone;
            case 2: return kMcfExtDeopt;
            default: return nullptr;   // failed: the interpreter retries it
            }
        }
    }
    return nullptr;
}

// A send left machine code for this method.  When it is a primitive T1 code
// can run through pharo_mcf_prim, record it so the next hit stays in
// machine code.  Never displaces a native-call entry for a compiled method.
void Interpreter::mcfNotePrimitiveTarget(Oop method, Oop receiver) {
    if (!method.isObject() || method.rawBits() < 0x10000) return;
    ObjectHeader* mo = method.asObjectPtr();
    if (mo->classIndex() != compiledMethodClassIndex_) return;
    Oop hdr = mo->slotAt(0);
    if (!hdr.isSmallInteger() || memory_.isYoung(method)) return;
    if (!((hdr.asSmallInteger() >> 16) & 1)) {
        // A compiled method without a primitive that a send exited for: one
        // the interpreter runs without an activation (its synthetic
        // primitives, e.g. WriteStream>>nextPut:) never reaches mcfActivate,
        // so enter it here, under mcfActivate's conditions.
        if (jit::mcfCallFind(method.rawBits())) return;
        jit::JITMethod* jm = jitRuntime_.methodMap().lookup(method.rawBits());
        if (!jm || !jm->isExecutable() || jm->numBytecodes == 0 || jm->isStubOnEntry) return;
        if (jm->hasRecvFieldAccess && !receiver.isObject()) return;
        jit::McfCallEntry& e = *jit::mcfCallSlotForCall(method.rawBits());
        e.methodBits = method.rawBits();
        e.jm = jm;
        e.prim = 0;
        e.bcEndOff = static_cast<uint64_t>(
            (mo->bytes() + mo->compiledCodeByteSize()) - reinterpret_cast<uint8_t*>(method.rawBits()));
        return;
    }
    const int p = primitiveIndexOf(method);
    if (p == 207 && ((hdr.asSmallInteger() >> 24) & 0xF) >= 2) {
        // value:value: and longer: T1 code calls the closure's block natively.
        jit::McfCallEntry* ep = jit::mcfCallSlotForPrim(method.rawBits());
        if (!ep) return;
        jit::McfCallEntry& e = *ep;
        e.methodBits = method.rawBits();
        e.jm = nullptr;
        e.bcEndOff = 0;
        e.prim = jit::kMcfBlockValueMethod;
        return;
    }
    if (!mcfPrimMethodCallable(method, p)) return;
    jit::McfCallEntry* ep = jit::mcfCallSlotForPrim(method.rawBits());
    if (!ep) return;   // has an entry, or both are native-call entries
    jit::McfCallEntry& e = *ep;
    e.methodBits = method.rawBits();
    e.jm = nullptr;
    e.bcEndOff = 0;
    e.prim = static_cast<uint64_t>(p);
}

// Called once from initializeJIT: SavedFrame layout for T1's native call
// and return, and the call cache.
void Interpreter::mcfInit() {
    jit::g_lazyCtxPendingAddr = reinterpret_cast<uint64_t>(&lazyCtxPending_);
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
    jit::g_mcfMegaCache = jitRuntime_.megaCache();
    if (::pharo::mcfEnabled()) jit::mcfBuildStubs();
    jit::g_mcfFbcClassIndex = fullBlockClosureClassIndex_;
    // Only in this mode: every code change clears the cache (mcfCodeChanged),
    // a 128 KB memset the default mode must not pay.
    if (::pharo::mcfEnabled() && !jit::g_mcfCallCache) {
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
        if (__builtin_expect(GET_DEBUG_BOOL(PHARO_MCF_VERIFY), 0))
            mcfVerifyMemo(*popped, "resume-after-return");
        jm = static_cast<jit::JITMethod*>(popped->mcJM);
        entry = popped->mcReturn;
    } else {
        if (!method_.isObject() || method_.rawBits() < 0x10000) return;
        jm = jitRuntime_.methodMap().lookup(method_.rawBits());
        if (!jm || !jm->isExecutable()) return;
        if (!(mcfIsMethodFrame() || mcfIsBlockFrame()) || !mcfReceiverOk(jm, receiver_)) return;
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
        static std::unordered_map<std::string, uint64_t> byMiss;
        Oop rcv = st.sp[-(st.sendArgCount + 1)];
        int filled = 0;
        std::string sel = "?";
        if (st.icDataPtr) {
            for (int e = 0; e < 6; e++) if (st.icDataPtr[e * 3]) filled++;
            Oop so = Oop::fromRawBits(st.icDataPtr[18]);
            if (so.isObject() && so.rawBits() > 0x10000) sel = memory_.oopToString(so);
        } else if (st.ip && (*st.ip == jit::SistaV1::ExtSend || *st.ip == jit::SistaV1::ExtSuperSend)
                   && st.method.isObject()) {
            int li = st.ip[1] >> 3;
            Oop so = memory_.fetchPointer(1 + li, st.method);
            if (so.isObject() && so.rawBits() > 0x10000) sel = memory_.oopToString(so);
            Oop rc = memory_.classOf(rcv);
            Oop tm = lookupMethodForSend(so, rc);
            if (tm.isObject() && !tm.isNil()) {
                sel += " prim=" + std::to_string(primitiveIndexOf(tm));
                jit::JITMethod* tj = jitRuntime_.methodMap().lookup(tm.rawBits());
                sel += tj && tj->isExecutable() && tj->numBytecodes ? " compiled" : " notcompiled";
                sel += jit::mcfCallFind(tm.rawBits()) ? " inCache" : " notInCache";
            }
        }
        if (rcv.isObject() && rcv.rawBits() > 0x10000
                && rcv.asObjectPtr()->classIndex() == fullBlockClosureClassIndex_) {
            Oop cb = rcv.asObjectPtr()->slotAt(1);
            jit::JITMethod* bj = cb.isObject() ? jitRuntime_.methodMap().lookup(cb.rawBits()) : nullptr;
            sel += std::string(bj && bj->isExecutable() && bj->numBytecodes ? " blkCompiled" : " blkNotCompiled")
                + (mcfBlockHasNLR(cb) ? " NLR" : "") + (memory_.isYoung(cb) ? " young" : "")
                + (jit::mcfCallFind(cb.rawBits()) ? " inCache" : " notInCache")
                + " slots=" + std::to_string(rcv.asObjectPtr()->slotCount())
                + " home=" + memory_.selectorOf(blockHomeMethodForNLR(cb))
                + " caller=" + memory_.selectorOf(st.method)
                + " tc=" + std::to_string(bj ? (int)bj->tempCount : -1)
                + " fd=" + std::to_string(frameDepth_)
                + " ctdn=" + std::to_string(checkCountdown_)
                + " callerNB=" + std::to_string(st.jitMethod ? (int)st.jitMethod->numBytecodes : -1);
        }
        byMiss["#" + sel + " rcv=" + (rcv.isObject() ? memory_.classNameOf(rcv)
               : rcv.isSmallInteger() ? std::string("SmI") : std::string("imm"))
               + " filled=" + std::to_string(filled) + " op=" + std::to_string(*st.ip)]++;
        if ((miss & 0xFFFFF) == 0) {
            std::vector<std::pair<uint64_t, std::string>> v;
            for (auto& kv : byMiss) v.push_back({kv.second, kv.first});
            std::sort(v.rbegin(), v.rend());
            for (size_t k = 0; k < v.size() && k < 12; k++)
                fprintf(stderr, "[MCF-CENSUS] miss %llu %s\n",
                        (unsigned long long)v[k].first, v[k].second.c_str());
            byMiss.clear();
        }
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
                    + " entryMatch=" + std::to_string(st.icDataPtr && st.icDataPtr[1] == t.rawBits())
                    + " inCache=" + std::to_string(jit::mcfCallFind(t.rawBits()) != nullptr)
                    + " young=" + std::to_string(memory_.isYoung(t))
                    + " slotHas=" + [&]() -> std::string {
                          if (!jit::g_mcfCallCache) return "-";
                          if (jit::mcfCallFind(t.rawBits())) return "self";
                          auto* e = jit::mcfCallSet(t.rawBits());
                          if (!e[0].methodBits) return "empty";
                          return memory_.selectorOf(Oop::fromRawBits(e[0].methodBits)) + "/" +
                                 (e[1].methodBits ? memory_.selectorOf(Oop::fromRawBits(e[1].methodBits)) : "empty");
                      }()
                    + " exitBC=" + std::to_string(st.ip ? *st.ip : -1)
                    + " reason=" + std::to_string(st.exitReason);
                bySite[key]++;
                if ((n & 0xFFFFF) == 0) {
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
        // Fields that never change for this interpreter.
        mcfState_ = new jit::JITState();
        std::memset(mcfState_, 0, sizeof(jit::JITState));
        jit::JITState& c = *mcfState_;
        c.memory = &memory_;
        c.interp = this;
        // No inline J2J: a null slice makes every J2J-capable site take its
        // SendCached exit (nullptr >= nullptr).
        c.j2jSaveCursor = nullptr;
        c.j2jSaveLimit = nullptr;
        c.j2jEntryCursor = nullptr;
        c.j2jDepthInc = 0x0000000100000001ULL;
        c.methodMapPtr = &jitRuntime_.methodMap();
        c.yieldCountdown = 1000;
        c.mcfFrames = savedFrames_.data();
        c.mcfFrameDepthPtr = &frameDepth_;
        c.mcfMatCtxPtr = &currentFrameMaterializedCtx_;
        c.mcfActiveCtxPtr = &activeContext_;
        c.mcfCheckCountdownPtr = &checkCountdown_;
        c.mcfDepthLimit = StackOverflowLimit - 64;
        c.mcfEpochPtr = &jit::g_mcCodeEpoch;
        c.mcfCallCache = jit::g_mcfCallCache;
        mcfConstEpoch_ = 0;
    }
    jit::JITState& st = *mcfState_;
    if (mcfConstEpoch_ != g_objectMotionEpoch) {
        // Oops that a GC or become could have moved.
        st.trueOop = memory_.trueObject();
        st.falseOop = memory_.falseObject();
        st.mcfNil = memory_.nil();
        mcfConstEpoch_ = g_objectMotionEpoch;
    }
    mcfActive_ = true;
    for (;;) {
        Oop m = method_;
        ObjectHeader* mo = m.asObjectPtr();
        st.sp = stackPointer_;
        st.receiver = receiver_;
        st.closure = closure_;
        st.literals = mo->slots() + 1;
        st.tempBase = framePointer_ + 1;
        st.ip = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(jm->bcStart()));
        st.jitMethod = jm;
        st.method = m;
        st.argCount = argCount_;
        st.exitReason = jit::ExitNone;
        // GC roots while machine code runs: never leave stale oops here.
        st.returnValue = Oop::fromRawBits(0);
        st.cachedTarget = Oop::fromRawBits(0);
        // Old space can grow without a GC: refresh the bounds every entry.
        st.oldSpaceStart = memory_.oldSpaceStart();
        st.oldSpaceEnd = memory_.oldSpaceEnd();
        st.newSpaceStart = memory_.newSpaceStart();
        st.newSpaceEnd = memory_.newSpaceEnd();
        st.mcfBcEndOff = static_cast<uint64_t>(
            bytecodeEnd_ - reinterpret_cast<uint8_t*>(m.rawBits()));
        st.prevState = currentJITState_;
        st.gcIpOffset = 0;
        currentJITState_ = &st;
        if (__builtin_expect(GET_DEBUG_BOOL(PHARO_MCF_VERIFY), 0))
            mcfVerifyReceiver(&st, "entry");
        // The checkpoint countdown pays for this entry as for a native call:
        // 1.  Charging all of numBytecodes at every entry
        // (and machine code is re-entered after every exit) drained the
        // countdown, the native call stub then refused every call, and each
        // refusal is another exit: myprecious's includes: loop exited at
        // every element with the countdown at -3 (15M exits per 6000 round
        // trips, 2026-10-14).
        checkCountdown_ -= 1;
        g_stepNum += jm->numBytecodes;

        pharo_jit_osr_resume(&st, entry);

        currentJITState_ = st.prevState;
        if (__builtin_expect(GET_DEBUG_BOOL(PHARO_MCF_VERIFY), 0)
                && st.exitReason != jit::ExitDeopt) {
            mcfVerifyReceiver(&st, "exit");
            // Entry check: the interpreter's registers fed st.receiver.
        }
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
        // Unconditionally: a native return followed by a native call can
        // leave a NEW activation of the same method at the same fp, with
        // another receiver, so "same method and fp" does not mean "same
        // frame" (the stale receiver_ then went into the next pushFrame).
        jm = st.jitMethod;
        const bool blockFrame = !st.closure.isNil();
        mcfSyncRegisters(st);
        if (__builtin_expect(GET_DEBUG_BOOL(PHARO_MCF_CENSUS), 0)) {
            static std::unordered_map<std::string, uint64_t> byReason;
            static uint64_t nAll = 0;
            std::string k = "reason=" + std::to_string(st.exitReason);
            if (st.exitReason != jit::ExitSend && st.exitReason != jit::ExitSendCached
                    && st.ip) {
                k += " op=" + std::to_string(*st.ip) + " in #" + memory_.selectorOf(st.method);
            }
            if (st.exitReason == jit::ExitReturn) {
                size_t d = frameDepth_;
                if (d == 0) k += " why=depth0";
                else {
                    const SavedFrame& f = savedFrames_[d - 1];
                    k += f.homeFrameDepth != SIZE_MAX ? (blockFrame ? " home(blk)" : " why=home") : "";
                    k += f.materializedRetSlot ? " why=retslot" : "";
                    k += !f.mcReturn ? " why=nomemo" : "";
                    k += (f.mcReturn && f.mcKeyIP != f.savedIP) ? " why=keyip" : "";
                    k += (f.mcReturn && f.mcEpoch != jit::g_mcCodeEpoch) ? " why=epoch" : "";
                    k += blockFrame ? " blk" : " meth";
                    k += " caller=#" + memory_.selectorOf(f.savedMethod);
                }
            }
            byReason[k]++;
            if ((++nAll & 0x1FFFFF) == 0) {
                std::vector<std::pair<uint64_t, std::string>> v;
                for (auto& kv : byReason) v.push_back({kv.second, kv.first});
                std::sort(v.rbegin(), v.rend());
                for (size_t q = 0; q < v.size() && q < 14; q++)
                    fprintf(stderr, "[MCF-EXITS] %llu %s\n", (unsigned long long)v[q].first, v[q].second.c_str());
                byReason.clear();
            }
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
                        pendingICOwnerIP_ = mcfPastSend(st.ip);
                        break;
                    }
                }
            }
            [[fallthrough]];
        case jit::ExitSendCached: {
            instructionPointer_ = st.ip;
            if (st.exitReason == jit::ExitSendCached)
                mcfNotePrimitiveTarget(st.cachedTarget, st.sp[-(st.sendArgCount + 1)]);

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
            // A block's local return (BlockReturnTop) is returnValue, like
            // the interpreter's op_blockReturnTop; ^ in a block exits as a
            // bytecode for the interpreter (non-local return).
            // A block frame's ExitReturn is its local return (BlockReturnTop),
            // returnValue at any depth, as the interpreter does.
            if (!blockFrame && (depth == 0
                                || savedFrames_[depth - 1].homeFrameDepth != SIZE_MAX)) {
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
                if (__builtin_expect(GET_DEBUG_BOOL(PHARO_MCF_VERIFY), 0))
                    mcfVerifyMemo(popped, "mcfRun-return");
                jm = static_cast<jit::JITMethod*>(popped.mcJM);
                entry = popped.mcReturn;
                continue;
            }
            if (!method_.isObject() || method_.rawBits() < 0x10000) break;
            jm = jitRuntime_.methodMap().lookup(method_.rawBits());
            if (!jm || !jm->isExecutable()) break;
            if (!(mcfIsMethodFrame() || mcfIsBlockFrame()) || !mcfReceiverOk(jm, receiver_)) break;
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
void Interpreter::mcfSyncRegisters(const jit::JITState&) {}
bool Interpreter::mcfIsBlockFrame() const { return false; }
void Interpreter::mcfActivateBlock() {}
bool Interpreter::mcfPrologueIsPrimitive(Oop) { return false; }
int Interpreter::mcfCallPrimitive(jit::JITState*, int, int) { return 0; }
void Interpreter::mcfNotePrimitiveTarget(Oop, Oop) {}
void Interpreter::mcfVerifyMemo(const SavedFrame&, const char*) {}
void Interpreter::mcfVerifyEq(jit::JITState*, Oop, Oop, Oop, uint64_t) {}
void Interpreter::mcfVerifyCall(jit::JITState*, Oop, uint64_t) {}
void Interpreter::mcfVerifyRet(jit::JITState*, Oop, void*) {}
void Interpreter::mcfVerifyPush(jit::JITState*) {}
void Interpreter::mcfWatchStore(const char*, int, int) {}
void Interpreter::mcfVerifyReceiver(jit::JITState*, const char*) {}
#endif  // PHARO_JIT_ENABLED && arm64

}  // namespace pharo
