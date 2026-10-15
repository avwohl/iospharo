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

#include <map>
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
// A CompiledMethod whose whole body is ^self == anObject (Object>>=).
bool Interpreter::mcfIsIdentityEquals(Oop m) {
    if (!m.isObject() || m.asObjectPtr()->classIndex() != compiledMethodClassIndex_) return false;
    ObjectHeader* mo = m.asObjectPtr();
    Oop hdr = mo->slotAt(0);
    if (!hdr.isSmallInteger()) return false;
    const int64_t h = hdr.asSmallInteger();
    if (((h >> 24) & 0xF) != 1) return false;                      // 1 argument
    // Primitive 110 (==) never fails: Character>>= and friends.  18.6M
    // lexicon `ch = $x` sends ran it through the C primitive call.
    if ((h >> 16) & 1) return primitiveIndexOf(m) == 110;
    const uint8_t* bc = mo->bytes() + (1 + (h & 0x7FFF)) * 8;
    const uint8_t* end = mo->bytes() + mo->compiledCodeByteSize();
    return end - bc >= 4 && bc[0] == jit::SistaV1::PushReceiver && bc[1] == jit::SistaV1::PushTempBase
        && bc[2] == jit::SistaV1::SpecialSendBase + 6 && bc[3] == jit::SistaV1::ReturnTop;
}

// McfCallEntry::bcEndOff, packed; 0 when a field does not fit (the
// caller then makes no native-call entry).
static inline uint64_t mcfPackBcInfo(uint64_t methodBits, const uint8_t* bcEnd,
                                     const jit::JITMethod* jm) {
    const uint64_t endOff = reinterpret_cast<uint64_t>(bcEnd) - methodBits;
    const uint64_t startOff = jm->bcStartCache - methodBits;
    if (endOff >= (1u << 24) || startOff >= (1u << 24)) return 0;
    return endOff | (startOff << 24) | (static_cast<uint64_t>(jm->tempCount) << 48);
}

extern "C" void pharo_mcf_block_home(jit::JITState* st, uint64_t closure) {
    st->interp->mcfSetBlockHome(Oop::fromRawBits(closure));
}

// Called from a compiled Context method before it reads sender/pc/stackp
// or stores a field, while lazy context stubs are pending.  st->ip is the
// bytecode; materializeFrameStack only allocates (no GC can run).
extern "C" void pharo_mcf_ctx_sync(jit::JITState* st) {
    st->interp->mcfCtxSync(st);
}

// PushThisContext from machine code (st->ip past it): as the interpreter,
// materialize the frames and collapse to depth 0; answer the context.
// Materialization only allocates (no GC), so the code's registers stay
// valid, and the frame goes on at depth 0 as one resumed from a context.
extern "C" uint64_t pharo_mcf_this_context(jit::JITState* st) {
    return st->interp->mcfThisContext(st).rawBits();
}

// A ^ in a block from machine code (the shared NLR stub).  Answers 0 when
// it refuses (nothing changed: the stub takes the block-return exit), 1
// when it popped the frames but cannot continue in machine code (the
// interpreter's state is complete: ExitDeopt), else the code address that
// continues the home's caller, with the state published as mcfRun does.
// Mirrors returnFromMethodSlow's non-local return when no ensure: or
// ifCurtailed: frame is between the block and its home
// (docs/native-nlr-plan.md, case (a)).
extern "C" void* pharo_mcf_nlr(jit::JITState* st) {
    return st->interp->mcfNonLocalReturn(st);
}

Oop Interpreter::mcfThisContext(jit::JITState* st) {
    stackPointer_ = st->sp;
    mcfSyncRegisters(*st);
    instructionPointer_ = st->ip;
    Oop ctx = activeContext_;
    if (frameDepth_ > 0) {
        ctx = materializeFrameStack();
        activeContext_ = ctx;
        currentFrameMaterializedCtx_ = memory_.nil();
        frameDepth_ = 0;
    }
    return ctx;
}

// PushFullBlock in machine code: create the closure without leaving.
// st->ip is the bytecode; the interpreter would have it past the 3 bytes.
// Allocation cannot GC here, so the machine code's registers stay valid.
// perform: and its with: forms from T1 code (kMcfPerformMethod): the call
// entry of the performed method when T1 code can call it natively, with
// the selector removed from the stack; kMcfExtDone / kMcfExtDeopt when a
// primitive or constant target was answered in place (as pharo_mcf_extsend;
// ipAfter = the ip past the perform); else nullptr, stack untouched (the
// send then exits and primitive 83 runs in the interpreter).
extern "C" void* pharo_mcf_perform(jit::JITState* st, uint32_t nArgs, uint8_t* ipAfter) {
    return st->interp->mcfPerformTarget(st, (int)nArgs, ipAfter);
}

// An inline-cache hit's primitive failed (pharo_mcf_prim answered 0, stack
// unchanged): the call entry of the method's body, to run it natively as
// the interpreter would after the failure; nullptr = take the exit.
extern "C" void* pharo_mcf_prim_fail(jit::JITState* st, uint32_t nArgs) {
    if (GET_DEBUG_BOOL(PHARO_MCF_NO_IC_PRIM_FAIL_BODY)) return nullptr;
    Oop method = st->cachedTarget;
    if (!method.isObject() || method.rawBits() < 0x10000) return nullptr;
    return st->interp->mcfPrimFailBodyEntry(method, st->sp[-(int)(nArgs + 1)]);
}

extern "C" uint64_t pharo_mcf_block_create(jit::JITState* st, uint64_t packed) {
    return st->interp->mcfBlockCreate(st, packed);
}

// PHARO_MCF_CALL_CENSUS: native calls by callee method (printed with the
// JIT stats by mcfCallCensusDump).
static Interpreter* g_mcfInterp = nullptr;
static std::unordered_map<uint64_t, uint64_t>* g_mcfCallCensus = nullptr;
// Linked-sends plan step 0: could a patched (linked) site have made this
// call?  By the matched IC entry: its slot, whether the site is monomorphic,
// the class of slot 0's extras word, and the callee's shape.
static std::map<std::string, uint64_t>* g_mcfLinkCensus = nullptr;
extern "C" void pharo_mcf_call_census(uint64_t method, const uint64_t* entry,
                                      const void* callerJM) {
    if (!g_mcfCallCensus) g_mcfCallCensus = new std::unordered_map<uint64_t, uint64_t>();
    (*g_mcfCallCensus)[method]++;
    if (!g_mcfLinkCensus) g_mcfLinkCensus = new std::map<std::string, uint64_t>();
    const jit::JITMethod* jm = static_cast<const jit::JITMethod*>(callerJM);
    std::string k;
    const uint64_t* base = jm ? reinterpret_cast<const uint64_t*>(jm->icSiteAt(0)) : nullptr;
    const size_t perSite = jit::IC_BYTES_PER_SITE / 8;
    if (!base || entry < base || entry >= base + perSite * jm->numICEntries) {
        k = "entry-not-in-caller-ic";
    } else {
        const size_t off = (size_t)(entry - base);
        const uint64_t* site = base + (off / perSite) * perSite;
        const size_t slot = (off % perSite) / 3;
        bool mono = true;
        for (int e = 1; e < 6; e++) if (site[e * 3] != 0) mono = false;
        const uint64_t ex = site[2];
        constexpr uint64_t kJ2J = 1ULL << 60, kB56 = 1ULL << 56, kAddr = (1ULL << 48) - 1;
        const char* exk = ex == 0 ? "x0" : ((ex & ~(kJ2J | kB56 | kAddr)) == 0 && (ex & kJ2J)) ? "j2j" : "xother";
        const jit::JITMethod* cj = g_mcfInterp ? g_mcfInterp->jitRuntime().methodMap().lookup(method) : nullptr;
        const char* ck = !cj ? "nojm"
            : cj->state != jit::MethodState::Compiled ? "notcompiled"
            : cj->isSpliceTarget ? "splice" : cj->isStubOnEntry ? "stub" : "ok";
        k = std::string(slot == 0 ? "slot0" : "slotN") + (mono ? " mono " : " poly ") + exk + " callee=" + ck;
    }
    (*g_mcfLinkCensus)[k]++;
}
void Interpreter::mcfCallCensusDump() {
    if (!g_mcfCallCensus) return;
    std::vector<std::pair<uint64_t, uint64_t>> v;
    uint64_t total = 0;
    for (auto& kv : *g_mcfCallCensus) { v.push_back({kv.second, kv.first}); total += kv.second; }
    std::sort(v.rbegin(), v.rend());
    fprintf(stderr, "[MCF-CALLS] total %llu, %zu callees\n", (unsigned long long)total, v.size());
    if (g_mcfLinkCensus)
        for (auto& kv : *g_mcfLinkCensus)
            fprintf(stderr, "[MCF-LINKABLE] %12llu %5.2f%% %s\n", (unsigned long long)kv.second,
                    100.0 * kv.second / total, kv.first.c_str());
    for (size_t k = 0; k < v.size() && k < 60; k++) {
        Oop m = Oop::fromRawBits(v[k].second);
        if (!m.isObject()) continue;
        ObjectHeader* mo = m.asObjectPtr();
        Oop hdr = mo->slotAt(0);
        const int nl = hdr.isSmallInteger() ? (int)(hdr.asSmallInteger() & 0x7FFF) : 0;
        const uint8_t* bc = mo->bytes() + (1 + nl) * 8;
        const uint8_t* end = mo->bytes() + mo->compiledCodeByteSize();
        int sends = 0, blocks = 0, n = 0;
        for (const uint8_t* p = bc; p < end; p += jit::SistaV1::bytecodeLength(*p), n++) {
            if (jit::SistaV1::isSendBytecode(*p)) sends++;
            if (*p == jit::SistaV1::PushFullBlock) blocks++;
        }
        fprintf(stderr, "[MCF-CALLS] %12llu %5.2f%% %s>>#%s bc=%d sends=%d blocks=%d\n",
                (unsigned long long)v[k].first, 100.0 * v[k].first / total,
                classNameOfMethod(m).c_str(), memory_.selectorOf(m).c_str(), n, sends, blocks);
    }
}

extern "C" uint64_t pharo_mcf_array_create(jit::JITState* st, uint32_t desc) {
    return st->interp->mcfArrayCreate(st, desc);
}

extern "C" int pharo_mcf_prim(jit::JITState* st, uint32_t primAndArgs) {
    if ((primAndArgs & 0xFFFF) == 105 && (primAndArgs >> 16) == 4
            && !GET_DEBUG_BOOL(PHARO_MCF_NO_FAST_REPLACE)
            && st->interp->mcfFastReplace(st))
        return 1;
    if (primAndArgs == 148 && !GET_DEBUG_BOOL(PHARO_MCF_NO_FAST_CLONE)
            && st->interp->mcfFastClone(st))
        return 1;
    return st->interp->mcfCallPrimitive(st, (int)(primAndArgs & 0xFFFF),
                                        (int)(primAndArgs >> 16));
}

// Primitive 148 (shallowCopy) of an ordinary heap object, with none of the
// interpreter state sync of a primitive call (~490 instructions a copy;
// Object>>copy runs it 3.6M times per PMPermutationTest>>testGenerator).
// ObjectMemory::shallowCopy is what primitiveClone runs: it allocates
// (eden, else old space) without ever collecting -- a scavenge waits for
// the next safe point -- so the machine code's registers stay valid, and
// it gives the copy a fresh hash and clears the mark, remembered,
// immutable and pinned bits.  Refused, for the full primitive: a Context
// (a lazy stub's fields are synced by that path), a forwarder (it follows
// it), compiled code, and an object with an overflow slot count.
bool Interpreter::mcfFastClone(jit::JITState* st) {
    const Oop rcv = st->sp[-1];
    if (!rcv.isObject()) return false;
    ObjectHeader* const h = rcv.asObjectPtr();
    const uint32_t ci = h->classIndex();
    if (ci == ObjectHeader::ForwardedClassIndex || ci == contextClassIndex_
            || contextClassIndex_ == 0)
        return false;
    if (h->format() >= ObjectFormat::CompiledMethod || h->hasOverflowSlots()) return false;
    const Oop copy = memory_.shallowCopy(rcv);
    if (!copy.isObject() || copy.isNil()) return false;
    st->sp[-1] = copy;
    return true;
}

// Primitive 105 (replaceFrom:to:with:startingAt:) for its two common
// shapes, bytes into bytes and Array into Array, with none of the
// interpreter state sync of a primitive call (~180 instructions; each `,`
// makes two).  The same checks as primitiveStringReplace; anything else
// (other formats, fixed fields, immutable destination) answers false and
// the full primitive runs.
bool Interpreter::mcfFastReplace(jit::JITState* st) {
    Oop* const sp = st->sp;
    const Oop dest = sp[-5], startO = sp[-4], stopO = sp[-3], repl = sp[-2], rStartO = sp[-1];
    if (!startO.isSmallInteger() || !stopO.isSmallInteger() || !rStartO.isSmallInteger())
        return false;
    if (!dest.isObject() || !repl.isObject()) return false;
    ObjectHeader* const dh = dest.asObjectPtr();
    ObjectHeader* const rh = repl.asObjectPtr();
    if (dh->isImmutable()) return false;
    const int64_t start = startO.asSmallInteger(), stop = stopO.asSmallInteger();
    const int64_t rStart = rStartO.asSmallInteger();
    if (start < 1 || rStart < 1 || stop < start - 1) return false;
    const int64_t count = stop - start + 1;
    // A self-copy overlapping ahead of its source propagates (the
    // primitive is a forward element loop): the full primitive does that.
    if (dest.rawBits() == repl.rawBits() && start > rStart && start < rStart + count)
        return false;
    const ObjectFormat df = dh->format(), rf = rh->format();
    if (df >= ObjectFormat::Indexable8 && df <= ObjectFormat::Indexable8_7
            && rf >= ObjectFormat::Indexable8 && rf <= ObjectFormat::Indexable8_7) {
        const int64_t dSize = (int64_t)dh->slotCount() * 8 - ((int)df - (int)ObjectFormat::Indexable8);
        const int64_t rSize = (int64_t)rh->slotCount() * 8 - ((int)rf - (int)ObjectFormat::Indexable8);
        if (stop > dSize || rStart - 1 + count > rSize) return false;
        if (count > 0)
            std::memmove(dh->bytes() + (start - 1), rh->bytes() + (rStart - 1), (size_t)count);
    } else if (df == ObjectFormat::Indexable && rf == ObjectFormat::Indexable) {
        const int64_t dSize = (int64_t)dh->slotCount(), rSize = (int64_t)rh->slotCount();
        if (stop > dSize || rStart - 1 + count > rSize) return false;
        if (count > 0) {
            Oop* const d = dh->slots() + (start - 1);
            const Oop* const r = rh->slots() + (rStart - 1);
            if (memory_.isYoung(dest)) {
                std::memmove(d, r, (size_t)count * sizeof(Oop));
            } else {
                // An old destination may need remembering: per slot, with
                // the barrier (no overlap ahead of the source, checked above).
                for (int64_t k = 0; k < count; k++)
                    memory_.storePointer((size_t)(start - 1 + k), dest, r[k]);
            }
        }
    } else {
        return false;
    }
    st->sp = sp - 4;                     // the destination is the result
    return true;
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
    const int prim = primitiveIndexOf(method);
    switch (prim) {
    case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 8: case 9:
    case 14: case 15: case 16:
    case 60: case 61: case 62:
    case 110:
    case 541: case 542: case 549:
        break;
    case 550:   // its prologue exists only in the full form (emitPrimProlog_arm64)
        if (GET_DEBUG_BOOL(PHARO_T1_NO_FLOAT_PROLOG_FULL)) return false;
        break;
    default:
        return false;
    }
    ObjectHeader* mo = method.asObjectPtr();
    Oop hdr = mo->slotAt(0);
    if (!hdr.isSmallInteger()) return false;
    // The prologue implements the primitive's ordinary form, on the
    // receiver: size of self, self at: i, self at: i put: v, self op arg.
    // A mirror method (MirrorPrimitives class>>indexableSizeOf: anObject)
    // uses the same primitive on its first argument: its prologue fails on
    // the class receiver and the native call ran the body instead of the
    // primitive (indexableSizeOf: answered 0 for an Array;
    // MirrorPrimitivesTest).  Only the interpreter's primitive handles both.
    if (!GET_DEBUG_BOOL(PHARO_MCF_PROLOGUE_ANY_ARITY)) {
        const int nArgs = (int)((hdr.asSmallInteger() >> 24) & 0xF);
        const int want = prim == 62 ? 0 : prim == 61 ? 2 : 1;
        if (nArgs != want) return false;
    }
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
// Primitives that touch only the operand stack and the objects on it:
// no frame registers, process switch, context or Smalltalk code
// (mcfCallPrimitive's leaf path).  Allocation is fine: it never collects
// here (a full eden falls back to old space).
static bool mcfLeafPrim(int p) {
    if (p >= 1 && p <= 17) return true;          // SmallInteger arithmetic
    if (p >= 40 && p <= 59) return true;         // Float
    if (p >= 541 && p <= 559) return true;       // SmallFloat64
    switch (p) {
    case 60: case 61: case 62: case 63: case 64: // at: at:put: size, string at:
    case 70: case 71:                            // basicNew basicNew:
    case 75: case 105: case 110: case 111:       // identityHash replaceFrom: == class
    case 158: case 159: case 165: case 166:      // compareWith: hashMultiply integerAt:(put:)
    case 169: case 170: case 171: case 175:      // ~~ conversions, behavior hash
        return true;
    default:
        return false;
    }
}

static bool mcfPrimCallable(int p) {
    if (p >= 1 && p <= 17) return true;          // SmallInteger arithmetic
    if (p >= 20 && p <= 37)                      // LargeInteger arithmetic: they only allocate
        return !GET_DEBUG_BOOL(PHARO_MCF_NO_LARGEINT_PRIMS);
    if (p >= 40 && p <= 59) return true;         // Float
    if (p >= 541 && p <= 559) return true;       // SmallFloat64 (sqrt alone was ~360 instructions a call)
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
    case 158:                                    // compareWith:collated: (every String/Symbol <, sorting)
    case 159:                                    // hashMultiply (every Dictionary at: with an integer key)
    case 195:                                    // findNextUnwindContextUpTo:
    case 196:                                    // terminateTo: (depth 0 only: mcfCallPrimitive)
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
// sent indexOfAscii:inString:startingAt: and friends 5M times), and the
// digit arithmetic of LargeIntegers (no Smalltalk; they allocate, which never
// collects: Random's 64-bit PCG left machine code ~3 times per Random next
// for bitShiftMagnitude: and bitXor:).
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
    if (memory_.symbolEquals(module, "LargeIntegers"))
        return !GET_DEBUG_BOOL(PHARO_MCF_NO_LARGEINT_PRIMS)
            && (memory_.symbolEquals(fn, "primDigitBitShiftMagnitude")
                || memory_.symbolEquals(fn, "primDigitBitAnd")
                || memory_.symbolEquals(fn, "primDigitBitOr")
                || memory_.symbolEquals(fn, "primDigitBitXor")
                || memory_.symbolEquals(fn, "primDigitAdd")
                || memory_.symbolEquals(fn, "primDigitSubtract")
                || memory_.symbolEquals(fn, "primDigitMultiplyNegative")
                || memory_.symbolEquals(fn, "primDigitCompare")
                || memory_.symbolEquals(fn, "primNormalizePositive")
                || memory_.symbolEquals(fn, "primNormalizeNegative")
                || memory_.symbolEquals(fn, "primDigitDivNegative"));
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
static bool g_mcfPrimFromExt = false;   // census: called from pharo_mcf_extsend
int Interpreter::mcfCallPrimitive(jit::JITState* st, int primIndex, int nArgs) {
    if (__builtin_expect(GET_DEBUG_BOOL(PHARO_MCF_PRIM_CENSUS), 0)) {
        static uint64_t counts[1024], total = 0;
        if (primIndex >= 0 && primIndex < 1024) counts[primIndex]++;
        static std::unordered_map<std::string, uint64_t> byCaller;
        if (primIndex == 60) {
            mcfDeriveView(*st);
            byCaller[memory_.selectorOf(st->method) + " ip-op=" + std::to_string(st->ip ? st->ip[-1] : -1)]++;
            if ((total & 0xFFFFFF) == 0) {
                std::vector<std::pair<uint64_t, std::string>> v;
                uint64_t sum = 0;
                for (auto& kv : byCaller) { v.push_back({kv.second, kv.first}); sum += kv.second; }
                std::sort(v.rbegin(), v.rend());
                fprintf(stderr, "[MCF-PRIM60] keys=%zu sum=%llu\n", v.size(), (unsigned long long)sum);
                for (size_t k = 0; k < v.size() && k < 8; k++)
                    fprintf(stderr, "[MCF-PRIM60] %llu from #%s\n",
                            (unsigned long long)v[k].first, v[k].second.c_str());
            }
        }
        {
            // Receiver (and argument) kinds for the primitives with an inline
            // path that still reach the C call.
            static std::unordered_map<std::string, uint64_t> byKind;
            if (primIndex == 60 || primIndex == 63 || primIndex == 110 || primIndex == 70
                    || primIndex == 62 || primIndex == 68) {
                Oop rcv = st->sp[-(nArgs + 1)];
                auto kind = [&](Oop o) -> std::string {
                    if (o.isSmallInteger()) return "SmI";
                    if (!o.isObject()) return "imm" + std::to_string(o.rawBits() & 7);
                    ObjectHeader* h = o.asObjectPtr();
                    return memory_.classNameOf(o) + "/fmt" + std::to_string((int)h->format())
                        + (h->slotCount() == 255 ? "/big" : "");
                };
                std::string k = std::to_string(primIndex) + "/" + std::to_string(nArgs)
                    + (g_mcfPrimFromExt ? " ext " : " ic ") + kind(rcv)
                    + " tgt " + (st->cachedTarget.isObject() ? classNameOfMethod(st->cachedTarget) + ">>#"
                                 + memory_.selectorOf(st->cachedTarget) : std::string("?"));
                if (primIndex == 63 && nArgs == 1 && !g_mcfPrimFromExt && rcv.isObject()
                        && st->sp[-1].isSmallInteger()) {
                    // The inline stub's checks, in C++: which one failed?
                    ObjectHeader* h = rcv.asObjectPtr();
                    const int64_t idx = st->sp[-1].asSmallInteger();
                    const uint64_t hw = *reinterpret_cast<uint64_t*>(h);
                    uint64_t cnt = hw >> 56;
                    if (cnt == 255) cnt = (reinterpret_cast<uint64_t*>(h)[-1] << 8) >> 8;
                    const uint64_t fmt = (hw >> 24) & 0x1F;
                    const uint64_t bytes = cnt * 8 - (fmt - 16);
                    const char* why = idx < 1 ? "idx<1" : fmt < 16 || fmt > 23 ? "fmt"
                        : (uint64_t)idx > bytes ? "idx>bytes" : "PASSES";
                    static std::unordered_map<std::string, uint64_t> whyCount;
                    whyCount[std::string(why) + (cnt == 255 ? " big" : "")]++;
                    static uint64_t nw = 0;
                    if ((++nw & 0xFFFFF) == 0)
                        for (auto& kv : whyCount)
                            fprintf(stderr, "[MCF-63WHY] %llu %s\n", (unsigned long long)kv.second, kv.first.c_str());
                }
                if (primIndex == 63 && nArgs == 1 && rcv.isObject() && st->sp[-1].isSmallInteger()) {
                    static int shown = 0;
                    if (++shown <= 6)
                        fprintf(stderr, "[MCF-63] index %lld bytes %zu slots %zu hdr 0x%llx\n",
                                (long long)st->sp[-1].asSmallInteger(), rcv.asObjectPtr()->byteSize(),
                                rcv.asObjectPtr()->slotCount(),
                                (unsigned long long)*reinterpret_cast<uint64_t*>(rcv.asObjectPtr()));
                }
                if (nArgs >= 1) k += " arg " + kind(st->sp[-1]);
                byKind[k]++;
            }
            if ((total & 0x3FFFFF) == 0x3FFFFF) {
                std::vector<std::pair<uint64_t, std::string>> v;
                for (auto& kv : byKind) v.push_back({kv.second, kv.first});
                std::sort(v.rbegin(), v.rend());
                for (size_t k = 0; k < v.size() && k < 14; k++)
                    fprintf(stderr, "[MCF-PRIMKIND] %llu %s\n", (unsigned long long)v[k].first,
                            v[k].second.c_str());
            }
        }
        if ((++total & 0x3FFFFF) == 0) {
            for (int p = 0; p < 1024; p++)
                if (counts[p] > total / 200) {
                    Oop rcv = st->sp[-(nArgs + 1)];
                    fprintf(stderr, "[MCF-PRIM] %d: %llu (%.1f%%) e.g. rcv %s\n", p,
                            (unsigned long long)counts[p], 100.0 * counts[p] / total,
                            p == primIndex ? memory_.classNameOf(rcv).c_str() : "");
                }
            fprintf(stderr, "[MCF-PRIM] total %llu\n", (unsigned long long)total);
        }
    }
    // terminateTo: rewrites senders and pcs.  With live frames it could
    // kill the context of a frame machine code then returns into, so only
    // at depth 0 (resume:through: runs it right after thisContext); else
    // it reports failure and the slow path sends it again.
    if (primIndex == 196 && (frameDepth_ > 0 || GET_DEBUG_BOOL(PHARO_MCF_NO_TERMINATE_TO)))
        return 0;
    // A leaf primitive reads and writes only the operand stack (no frame,
    // no process switch, no context, no Smalltalk): it needs sp, not the
    // frame registers, and not the generic dispatcher.  A failure exits
    // the send as before, and the exit syncs the registers.
    static const bool leafOk = !GET_DEBUG_BOOL(PHARO_MCF_NO_LEAF_PRIM);
    if (leafOk && mcfLeafPrim(primIndex) && !g_lazyForwarders
            && (size_t)primIndex < primitiveTable_.size()) {
        PrimitiveFunc prim = primitiveTable_[primIndex];
        if (prim) {
            Oop* const spBefore = st->sp;
            stackPointer_ = spBefore;
            argCount_ = nArgs;
            primitiveFailed_ = false;
            primFailCode_ = 0;
            newMethod_ = st->cachedTarget;
            PrimitiveResult r = (this->*prim)(nArgs);
            if (r == PrimitiveResult::Success) {
                if (__builtin_expect(GET_DEBUG_BOOL(PHARO_PRIM_SP_AUDIT), 0)
                        && stackPointer_ != spBefore - nArgs) {
                    static int na = 0;
                    if (++na <= 20)
                        fprintf(stderr, "[PRIM-SP-AUDIT] leaf prim=%d argc=%d moved %td slots\n",
                                primIndex, nArgs, spBefore - stackPointer_);
                }
                lastPrimitiveIndex_ = primIndex;
                st->sp = stackPointer_;
                return 1;
            }
            stackPointer_ = spBefore;
            primFailCode_ = 0;
            return 0;
        }
    }
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
            (const void*)f.savedIP, (const void*)f.savedIP, frameDepth_);
    stopVM("MCF memo verification failed");
}

// PHARO_MCF_VERIFY: T1 just pushed a SavedFrame natively and set up the
// callee view.  Check the record against what pushFrame would write.
void Interpreter::mcfVerifyPush(jit::JITState* st) {
    // The lazy view writes no method/argCount per call: derive them first,
    // or the callee checks below compare stale state.
    mcfDeriveView(*st);
    const SavedFrame& f = savedFrames_[frameDepth_ - 1];
    const char* why = nullptr;
    // Every check runs and counts on its own (an else-if chain hid every
    // check after the first that failed).
    static uint64_t fails = 0;
    auto bad = [&](const char* w) {
        if (!why) why = w;
        static int shown = 0;
        if (++shown <= 20)
            fprintf(stderr, "[MCF-VERIFY-PUSH] %s: caller=#%s callee=#%s fd=%zu\n", w,
                    memory_.selectorOf(f.savedMethod).c_str(), memory_.selectorOf(st->method).c_str(),
                    frameDepth_);
        ++fails;
    };
    ObjectHeader* mo = f.savedMethod.asObjectPtr();
    Oop hdr = mo->slotAt(0);
    const int nLits = (int)(hdr.asSmallInteger() & 0x7FFF);
    const int nArgsCaller = (int)((hdr.asSmallInteger() >> 24) & 0xF);
    const uint8_t* bc0 = mo->bytes() + (1 + nLits) * 8;
    const uint8_t* end = mo->bytes() + mo->compiledCodeByteSize();
    if (f.savedIP <= bc0 || f.savedIP > end) bad("savedIP outside caller");
    // The lazy view leaves savedBytecodeEnd null (derived when read).
    if (f.savedBytecodeEnd != end && !(f.savedBytecodeEnd == nullptr && ::pharo::mcfLazyView()))
        bad("savedBytecodeEnd");
    if (f.savedFP[0].rawBits() != (recClosure(f).isNil() ? f.savedReceiver
                                                         : recClosure(f)).rawBits())
        bad("fp[0] is neither the receiver nor the closure");
    // INV-FP0: a block frame's closure is fp[0]; a method frame has none.
    {
        const bool callerIsBlock = mo->classIndex() == compiledBlockClassIndex_;
        if (callerIsBlock ? recClosure(f).rawBits() != f.savedFP[0].rawBits()
                          : !recClosure(f).isNil()) {
            bad("INV-FP0 closure");
            static int nd = 0;
            if (++nd <= 6) {
                auto cls = [&](Oop o) { return o.isObject() && o.rawBits() > 0x10000
                    ? memory_.classNameOf(o) : std::string(o.isNil() ? "nil" : "imm"); };
                fprintf(stderr, "[MCF-VERIFY-PUSH]   fp0=%s closure=%s rcv=%s home=#%s mcJM=%p "
                        "pushSite=%s depth=%zu\n", cls(f.savedFP[0]).c_str(), cls(recClosure(f)).c_str(),
                        cls(f.savedReceiver).c_str(),
                        memory_.selectorOf(blockHomeMethodForNLR(f.savedMethod)).c_str(), f.mcJM,
                        f.pushSite ? f.pushSite : "-", frameDepth_);
            }
        }
    }
    if (f.savedArgCount != nArgsCaller) bad("argCount");
    if (f.ctxSynced) bad("ctxSynced set");
    if (f.materializedRetSlot) bad("retSlot set");
    if (f.mcJM && static_cast<const jit::JITMethod*>(f.mcJM)->compiledMethodOop
                      != f.savedMethod.rawBits())
        bad("mcJM is not the caller's code");
    if (recActiveCtx(f).rawBits() != activeContext_.rawBits()) bad("INV-AC");
    if (st->closure.isNil() && f.homeFrameDepth() != SIZE_MAX) bad("home marker on a method call");
    if (st->tempBase[-1].rawBits() != (st->closure.isNil() ? st->receiver
                                                         : st->closure).rawBits())
        bad("callee fp[0]");
    if (st->tempBase - 1 <= f.savedFP) bad("callee fp below caller fp");
    {
        Oop ch = st->method.asObjectPtr()->slotAt(0);
        const int nArgsCallee = (int)((ch.asSmallInteger() >> 24) & 0xF);
        const int nTemps = (int)((ch.asSmallInteger() >> 18) & 0x3F);
        if (nArgsCallee != st->argCount) bad("callee argCount");
        if (st->sp != st->tempBase + nTemps) bad("callee sp");
        if (st->closure.isNil()) {   // a block's copied values are not nil
            for (int t = nArgsCallee; t < nTemps; t++)
                if (!st->tempBase[t].isNil()) { bad("temp not nil"); break; }
        }
    }
    if (fails && (fails & 0xFFFF) == 1) fprintf(stderr, "[MCF-VERIFY-PUSH] failures=%llu\n",
                                                (unsigned long long)fails);
    if (!why && st->closure.isNil()) {
        // The callee must be the method the send at the caller's ip names
        // (a poisoned inline cache calls another one).  Short and special
        // sends only; walk the bytecodes to the one that ends at savedIP.
        const uint8_t* pc = bc0;
        const uint8_t* prev = nullptr;
        while (pc < f.savedIP) { prev = pc; pc += jit::SistaV1::bytecodeLength(*pc); }
        Oop sel = Oop::nil();
        if (pc == f.savedIP && prev && (prev == bc0 || (prev[-1] != jit::SistaV1::ExtendA
                                                         && prev[-1] != jit::SistaV1::ExtendB))) {
            const uint8_t op = *prev;
            if (op >= jit::SistaV1::ArithBase && op <= jit::SistaV1::SpecialSendLast) {
                Oop ss = memory_.specialObject(SpecialObjectIndex::SpecialSelectorsArray);
                sel = ss.asObjectPtr()->slotAt((size_t)(op - jit::SistaV1::ArithBase) * 2);
            } else if (op >= jit::SistaV1::Send0Base && op <= jit::SistaV1::Send2Last && (op & 0x0F) < nLits) {
                sel = mo->slotAt(1 + (op & 0x0F));
            }
        }
        if (!sel.isNil() && sel.isObject() && sel.asObjectPtr()->isBytesObject()) {
            const std::string want = memory_.oopToString(sel);
            const std::string got = memory_.selectorOf(st->method);
            // perform: and its with: forms call the performed method natively.
            const bool perform = want.rfind("perform:", 0) == 0;
            if (want != got && got != "?" && !perform) {
                static int m = 0;
                if (++m <= 20)
                    fprintf(stderr, "[MCF-VERIFY-SEL] site sends #%s but calls #%s (caller #%s, rcv %s)\n",
                            want.c_str(), got.c_str(), memory_.selectorOf(f.savedMethod).c_str(),
                            memory_.classNameOf(st->receiver).c_str());
            }
        }
    }
    (void)why;
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
    mcfDeriveClosure(*st);
    mcfVerifyReceiver(st, "native-return");
    // Every condition the folded return no longer tests must imply a null
    // memo, so a native return never gets here with one of them true.
    if (frameDepth_ > 0) {
        const SavedFrame& f = savedFrames_[frameDepth_ - 1];
        const char* bad = f.materializedRetSlot ? "retSlot"
            : (f.homeFrameDepth() != SIZE_MAX && st->closure.isNil()) ? "home-of-method-frame"
            : nullptr;
        static int nb = 0;
        if (bad && ++nb <= 10)
            fprintf(stderr, "[MCF-VERIFY-RET] native return with %s (fd=%zu #%s)\n", bad,
                    frameDepth_, memory_.selectorOf(f.savedMethod).c_str());
        // INV-AC (docs/mcf-call-trim-plan.md step 0): every live record a
        // native return pops saved the current activeContext_.
        if (recActiveCtx(f).rawBits() != activeContext_.rawBits()) {
            static int na = 0;
            if (++na <= 10)
                fprintf(stderr, "[MCF-VERIFY-RET] INV-AC: record activeContext 0x%llx, "
                        "activeContext_ 0x%llx (fd=%zu #%s)\n",
                        (unsigned long long)recActiveCtx(f).rawBits(),
                        (unsigned long long)activeContext_.rawBits(), frameDepth_,
                        memory_.selectorOf(f.savedMethod).c_str());
        }
    } else {
        static int nz = 0;
        if (++nz <= 10) fprintf(stderr, "[MCF-VERIFY-RET] native return at depth 0\n");
    }
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
    mcfDeriveView(*st);   // the lazy view leaves st->method stale
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
    mcfDeriveClosure(*st);   // trim step 8: the state closure is derived
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

// Does this method's whole body answer one constant?  `^ literal`
// (literal frame, PushInteger, PushCharacter, true/false/nil/0/1) or a
// ReturnTrue/False/Nil bytecode.  A young literal is refused: the
// scavenge purges entries by key only.
bool Interpreter::mcfConstantReturn(Oop method, Oop& value) {
    if (GET_DEBUG_BOOL(PHARO_MCF_NO_CONSTANT_RETURN)) return false;
    ObjectHeader* mo = method.asObjectPtr();
    Oop hdr = mo->slotAt(0);
    if (!hdr.isSmallInteger()) return false;
    const int nLits = (int)(hdr.asSmallInteger() & 0x7FFF);
    const uint8_t* bc = mo->bytes() + (1 + nLits) * 8;
    const uint8_t* end = mo->bytes() + mo->compiledCodeByteSize();
    if (bc >= end) return false;
    namespace S = jit::SistaV1;
    switch (bc[0]) {
        case S::ReturnTrue:  value = memory_.trueObject(); return true;
        case S::ReturnFalse: value = memory_.falseObject(); return true;
        case S::ReturnNil:   value = memory_.nil(); return true;
        default: break;
    }
    int len;
    switch (bc[0]) {
        case S::PushTrue:  value = memory_.trueObject(); len = 1; break;
        case S::PushFalse: value = memory_.falseObject(); len = 1; break;
        case S::PushNil:   value = memory_.nil(); len = 1; break;
        case S::PushZero:  value = Oop::fromSmallInteger(0); len = 1; break;
        case S::PushOne:   value = Oop::fromSmallInteger(1); len = 1; break;
        case S::PushInteger:
            if (bc + 1 >= end) return false;
            value = Oop::fromSmallInteger(bc[1]); len = 2; break;
        case S::PushCharacter:
            if (bc + 1 >= end) return false;
            value = Oop::fromCharacter(bc[1]); len = 2; break;
        default:
            if (bc[0] >= S::PushLitConstBase && bc[0] <= S::PushLitConstLast
                    && bc[0] - S::PushLitConstBase < nLits) {
                value = mo->slotAt(1 + (bc[0] - S::PushLitConstBase));
                len = 1;
                break;
            }
            return false;
    }
    if (bc + len >= end || bc[len] != S::ReturnTop) return false;
    return !value.isObject() || !memory_.isYoung(value);
}

// A unary method T1 code answers through g_mcfTinyStub (kMcfTinyShape):
// `^ f ifNil: [...]`, `^ fa OP fb`, `^ (fa OP fb) and: [...]`.  Not for the
// Context hierarchy: lazy contexts sync their fields before a read.
bool Interpreter::mcfTinyShape(Oop method, uint64_t& prim) {
    if (GET_DEBUG_BOOL(PHARO_MCF_NO_TINY_SHAPES) || !jit::g_mcfTinyStub) return false;
    ObjectHeader* mo = method.asObjectPtr();
    if (mo->classIndex() != compiledMethodClassIndex_) return false;
    Oop hdr = mo->slotAt(0);
    if (!hdr.isSmallInteger() || ((hdr.asSmallInteger() >> 24) & 0xF) != 0) return false;
    if ((hdr.asSmallInteger() >> 16) & 1) return false;               // a primitive
    const uint8_t* bc = mo->bytes() + (1 + (hdr.asSmallInteger() & 0x7FFF)) * 8;
    const uint8_t* end = mo->bytes() + mo->compiledCodeByteSize();
    const size_t n = (size_t)(end - bc);
    namespace S = jit::SistaV1;
    auto isRecvVar = [](uint8_t op) { return op <= S::PushRecvVarLast; };
    auto jumpFalseTarget = [&](size_t at) -> size_t {   // index the jump at `at` goes to
        return at + 1 + (size_t)(bc[at] - S::ShortJumpFalseBase) + 1;
    };
    uint64_t shape = 0, op = 0, fa = 0, fb = 0;
    if (n >= 5 && isRecvVar(bc[0]) && bc[1] == S::Dup && bc[2] == S::PushNil
            && bc[3] == S::SpecialSendBase + 6
            && bc[4] >= S::ShortJumpFalseBase && bc[4] <= S::ShortJumpFalseLast) {
        const size_t t = jumpFalseTarget(4);
        if (t < n && bc[t] == S::ReturnTop) { shape = 1; fa = bc[0]; }
    } else if (n >= 4 && isRecvVar(bc[0]) && isRecvVar(bc[1])
               && bc[2] >= S::ArithBase + 2 && bc[2] <= S::ArithBase + 7) {
        fa = bc[0]; fb = bc[1]; op = bc[2] - (S::ArithBase + 2);
        if (bc[3] == S::ReturnTop) shape = 2;
        else if (bc[3] >= S::ShortJumpFalseBase && bc[3] <= S::ShortJumpFalseLast) {
            const size_t t = jumpFalseTarget(3);
            if (t + 1 < n && bc[t] == S::PushFalse && bc[t + 1] == S::ReturnTop) shape = 3;
        }
    }
    if (!shape) return false;
    // Not a method of Context or one of its superclasses.
    Oop mc = methodClassOf(method);
    Oop ctxClass = memory_.specialObject(SpecialObjectIndex::ClassMethodContext);
    for (Oop c = ctxClass; c.isObject() && !c.isNil(); c = superclassOf(c))
        if (c.rawBits() == mc.rawBits()) return false;
    prim = jit::kMcfTinyShape | (shape << 24) | (op << 28) | (fa << 32) | (fb << 48);
    return true;
}

// May a call-cache entry be keyed by this method or CompiledBlock?  Young
// ones may: the scavenge purges their entries (mcfPurgeCallCacheRange).
// Methods compiled at run time stayed young until the next scavenge, and
// every call to them left machine code.
bool Interpreter::mcfCacheableOop(Oop m) const {
    return !GET_DEBUG_BOOL(PHARO_MCF_NO_YOUNG_CALL) || !memory_.isYoung(m);
}

// A block frame (closure_ set, method_ a CompiledBlock), set up by
// activateBlock, popFrame or a native block call.
bool Interpreter::mcfIsBlockFrame() const {
    if (GET_DEBUG_BOOL(PHARO_MCF_NO_BLOCKS)) return false;
    return !closure_.isNil()
        && method_.isObject()
        && method_.asObjectPtr()->classIndex() == compiledBlockClassIndex_;
}

void Interpreter::mcfActivateBlock() {
    if (mcfActive_ || !jitInitialized_) return;
    if (checkCountdown_ <= 1 && !GET_DEBUG_BOOL(PHARO_SENDS_ARE_INTERRUPT_POINTS)) return;  // as mcfActivate
    if (closure_.isNil() || !method_.isObject()
            || method_.asObjectPtr()->classIndex() != compiledBlockClassIndex_) return;
    jit::JITMethod* jm = jitRuntime_.methodMap().lookup(method_.rawBits());
    if (!jm || !jm->isExecutable() || jm->numBytecodes == 0 || jm->isStubOnEntry) return;
    if (instructionPointer_ != reinterpret_cast<const uint8_t*>(jm->bcStart())) return;
    // Phase 3b: let T1 code call this block natively at value / value:.
    // A block with ^ is flagged: the stub then finds its home frame.
    const bool nlr = mcfBlockHasNLR(method_);
    const uint64_t bcInfo = mcfPackBcInfo(method_.rawBits(), bytecodeEnd_, jm);
    if (bcInfo && mcfCacheableOop(method_) && (!nlr || !GET_DEBUG_BOOL(PHARO_MCF_NO_NLR_BLOCK_CALL))) {
        Oop hdr = method_.asObjectPtr()->slotAt(0);
        jit::McfCallEntry& e = *jit::mcfCallSlotForCall(method_.rawBits());
        e.methodBits = method_.rawBits();
        noteJitOop(e.methodBits);
        e.jm = jm;
        e.bcEndOff = bcInfo;
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
// Lazy view: machine code keeps only x19 (published to st.jitMethod at
// every exit and helper call); the rest of the view follows from it.
void Interpreter::mcfDeriveView(jit::JITState& st) {
    if (!::pharo::mcfLazyView()) return;
    jit::JITMethod* jm = st.jitMethod;
    if (!jm || !jm->compiledMethodOop) return;
    st.method = Oop::fromRawBits(jm->compiledMethodOop);
    ObjectHeader* mo = st.method.asObjectPtr();
    st.literals = mo->slots() + 1;
    st.argCount = jm->argCount;
    // The bytecode end, memoized by method oop (decoding it on every
    // primitive call was 2% of lexicon).  Object motion clears the memo.
    if (bcEndMemoEpoch_ != g_objectMotionEpoch) {
        for (auto& e : bcEndMemo_) e.method = 0;
        bcEndMemoEpoch_ = g_objectMotionEpoch;
    }
    auto& e = bcEndMemo_[(jm->compiledMethodOop >> 4) & (kBcEndMemoSize - 1)];
    if (e.method != jm->compiledMethodOop) {
        e.method = jm->compiledMethodOop;
        e.off = static_cast<uint64_t>(
            (mo->bytes() + mo->compiledCodeByteSize()) - reinterpret_cast<uint8_t*>(st.method.rawBits()));
    }
    st.mcfBcEndOff = e.off;
    mcfDeriveClosure(st);   // trim step 8
}

void Interpreter::mcfSyncRegisters(const jit::JITState& stIn) {
    jit::JITState& st = const_cast<jit::JITState&>(stIn);
    mcfDeriveView(st);
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
    // A checkpoint spent or pending (the interpreter defers one to the next
    // interrupt point with the countdown at 1): the interpreter runs the
    // method's first bytecode -- an interrupt point -- and takes it there.
    // Entering machine code with it pending held the heartbeat's yield
    // request (and timers) back until 1024 more interpreted bytecodes.
    if (checkCountdown_ <= 1 && !GET_DEBUG_BOOL(PHARO_SENDS_ARE_INTERRUPT_POINTS)) return false;
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
        // ensure: (198) and on:do: (199) are marker primitives: they always
        // fail and the compiled body is the method.  2.7M lexicon calls exited
        // before they were allowed.  A refused native call (countdown) still
        // takes the interpreter's send, which suppresses the context switch.
        const int primIdx = hasPrim ? primitiveIndexOf(method) : 0;
        const bool markerPrim = (primIdx == 198 || primIdx == 199)
            && !GET_DEBUG_BOOL(PHARO_MCF_NO_MARKER_PRIM_CALL);
        if (bisectOk && (!hasPrim || markerPrim || mcfPrologueIsPrimitive(method)) && mcfCacheableOop(method)
                && (!jm->hasRecvFieldAccess || receiver_.isObject())
                && mcfPackBcInfo(method.rawBits(), bytecodeEnd_, jm)) {
            jit::McfCallEntry& e = *jit::mcfCallSlotForCall(method.rawBits());
            e.methodBits = method.rawBits();
            noteJitOop(e.methodBits);
            e.jm = jm;
            e.prim = 0;
            e.bcEndOff = mcfPackBcInfo(method.rawBits(), bytecodeEnd_, jm);
            Oop constant;
            uint64_t tiny;
            if (!hasPrim && mcfConstantReturn(method, constant)) {
                e.prim = jit::kMcfReturnsValue;
                e.bcEndOff = constant.rawBits();
            } else if (!hasPrim && mcfTinyShape(method, tiny)) {
                e.prim = tiny;
            }
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

void* Interpreter::mcfPerformTarget(jit::JITState* st, int nArgs, uint8_t* ipAfter) {
    Oop* sp = st->sp;
    Oop rcv = sp[-(nArgs + 1)];
    Oop selector = sp[-nArgs];
    if (!selector.isObject() || selector.rawBits() < 0x10000
            || !selector.asObjectPtr()->isBytesObject())
        return nullptr;
    // A forwarded receiver (lazy become) would run the method with the
    // forwarder as self: the interpreter's perform follows it.
    if (__builtin_expect(g_lazyForwarders, 0) && rcv.isObject() && rcv.rawBits() >= 0x10000
            && rcv.asObjectPtr()->isForwarded())
        return nullptr;
    Oop cls = memory_.classOf(rcv);
    if (!cls.isObject() || cls.isNil()) return nullptr;
    Oop target = lookupMethodForSend(selector, cls);
    if (!target.isObject() || target.rawBits() < 0x10000
            || target.asObjectPtr()->classIndex() != compiledMethodClassIndex_)
        return nullptr;
    Oop th = target.asObjectPtr()->slotAt(0);
    if (!th.isSmallInteger() || (int)((th.asSmallInteger() >> 24) & 0xF) != nArgs - 1)
        return nullptr;                              // the primitive fails it
    jit::McfCallEntry* e = jit::mcfCallFind(target.rawBits());
    if ((!e || !e->jm || e->prim == jit::kMcfReturnsValue)
            && !GET_DEBUG_BOOL(PHARO_MCF_NO_PERFORM_PRIM)) {
        if (void* done = mcfPerformInPlace(st, nArgs, ipAfter, target, rcv, e)) return done;
    }
    if (!e || !e->jm || (e->prim && (e->prim & 0xFFFFFF) != jit::kMcfTinyShape)
            || !mcfReceiverOk(e->jm, rcv))
        return nullptr;
    // The call stub must not refuse once the selector is gone: its tests,
    // made here (countdown, depth limit, a depth-0 caller's context).
    if (checkCountdown_ <= 1 || frameDepth_ + 2 >= st->mcfDepthLimit) return nullptr;
    if (frameDepth_ == 0 && (!activeContext_.isObject() || activeContext_.isNil()
                             || (activeContext_.rawBits() & 7) != 0))
        return nullptr;
    for (int i = nArgs - 1; i >= 1; i--) sp[-(i + 1)] = sp[-i];   // drop the selector
    st->sp = sp - 1;
    return e;
}

// mcfPerformTarget's target has no native-call entry: answer it in place
// when it is a primitive or a constant, else nullptr (stack untouched; `e`
// is the entry found or made, for the native-call test that follows).  Out
// of line: inlined, its spills cost every native perform ~25 instructions.
[[gnu::noinline]] void* Interpreter::mcfPerformInPlace(jit::JITState* st, int nArgs, uint8_t* ipAfter,
                                                       Oop target, Oop rcv, jit::McfCallEntry*& e) {
    Oop* const sp = st->sp;
    if (!e) {
        // A target only ever performed is never sent from an inline cache,
        // so nothing records it (2.5 perform: #+ with: 1.5 runs primitive
        // 541 every time and never activates the method).
        mcfNotePrimitiveTarget(target, rcv);
        e = jit::mcfCallFind(target.rawBits());
        if (!e) return nullptr;
    }
    if (e->prim == jit::kMcfReturnsValue && !GET_DEBUG_BOOL(PHARO_MCF_NO_CONSTANT_RETURN)) {
        // A method answering one constant: it replaces the receiver; the
        // selector and the arguments go.
        sp[-(nArgs + 1)] = Oop::fromRawBits(e->bcEndOff);
        st->sp = sp - nArgs;
        return kMcfExtDone;
    }
    if (e->jm || !e->prim || e->prim >= 1024) return nullptr;
    // A primitive entry (mcfNotePrimitiveTarget): run the primitive with the
    // selector removed, as primitivePerform does, the way an inline-cache hit
    // runs it (with the ip after the perform).  Each was an exit to the
    // interpreter (~45 ns against Cog's 4).  A failure puts the selector back
    // and exits as before; the interpreter's perform then runs the primitive
    // and the method body.
    const Oop selector = sp[-nArgs];
    for (int i = nArgs - 1; i >= 1; i--) sp[-(i + 1)] = sp[-i];
    st->sp = sp - 1;
    st->cachedTarget = target;
    st->ip = ipAfter;
    switch (mcfCallPrimitive(st, (int)e->prim, nArgs - 1)) {
    case 1: return kMcfExtDone;
    case 2: return kMcfExtDeopt;
    default:
        for (int i = 1; i <= nArgs - 1; i++) sp[-i] = sp[-(i + 1)];
        sp[-nArgs] = selector;
        st->sp = sp;
        return nullptr;
    }
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

// PushArray in machine code.  Allocation does not GC (a full eden falls
// back to old space), so no register sync is needed.  0 = allocation
// failed: take the exit.
uint64_t Interpreter::mcfArrayCreate(jit::JITState* st, uint32_t desc) {
    const int arraySize = desc & 0x7F;
    const bool popIntoArray = (desc >> 7) != 0;
    Oop arrayClass = memory_.specialObject(SpecialObjectIndex::ClassArray);
    Oop array = memory_.allocateSlots(memory_.indexOfClass(arrayClass), arraySize,
                                      ObjectFormat::Indexable);
    if (!array.isObject() || array.isNil()) return 0;
    Oop* sp = st->sp;
    if (popIntoArray) {
        for (int i = arraySize - 1; i >= 0; i--)
            memory_.storePointer(i, array, *--sp);
    }
    *sp++ = array;
    st->sp = sp;
    return 1;
}

void Interpreter::mcfCtxSync(jit::JITState* st) {
    if (!lazyCtxPending_) return;
    stackPointer_ = st->sp;
    mcfSyncRegisters(*st);
    instructionPointer_ = st->ip;
    if (seg_.armed && segTagged(receiver_)) {
        // A frozen segment's boundary may be read below its sender (pc,
        // stackp, the rest are synced); its sender, a store, or any field of
        // a frozen frame's context needs the frames.
        const uint8_t* ip = st->ip;
        int ext = 0;
        while (ip && *ip == jit::SistaV1::ExtendA) { ext = (ext << 8) | ip[1]; ip += 2; }
        int field = -1;
        if (ip && jit::SistaV1::isPushRecvVar(*ip)) field = *ip - jit::SistaV1::PushRecvVarBase;
        else if (ip && *ip == jit::SistaV1::ExtPushRecvVar) field = ip[1] + (ext << 8);
        const bool okRead = receiver_.rawBits() == seg_.boundaryCtx.rawBits() && field > 0;
        if (!okRead) nlrSegmentDeopt();
    }
    if (lazyCtxPending_ & 1) materializeFrameStack();
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
        savedFrames_[frameDepth_ - 1].setHomeFrameDepth(nlrMemoFrame_);
        return;
    }
    Oop home = blockHomeMethodForNLR(compiledBlock);
    size_t homeFrame = SIZE_MAX;
    // The closure was made in a method context: its home frame is the
    // topmost one holding that context and that method, the exact match
    // findBlockHomeFrame tries first -- found here without its context-chain
    // and literal walks.  A recursive or re-entered home missed the memo
    // above at every call (deeptraverser's withAllSubclasses: 12%).
    if (home.isObject() && !home.isNil() && outerContext.isObject() && !outerContext.isNil()
            && !GET_DEBUG_BOOL(PHARO_MCF_NO_FAST_BLOCK_HOME)) {
        ObjectHeader* oc = outerContext.asObjectPtr();
        if (oc->slotCount() > 4 && oc->slotAt(4).isNil()
                && oc->slotAt(3).rawBits() == home.rawBits()) {
            const size_t stop = frameDepth_ > 16 ? frameDepth_ - 16 : 0;
            for (size_t i = frameDepth_; i > stop; i--) {
                const SavedFrame& f = savedFrames_[i - 1];
                if (f.materializedContext.rawBits() == outerContext.rawBits()
                        && f.savedMethod.rawBits() == home.rawBits()) {
                    if (__builtin_expect(GET_DEBUG_BOOL(PHARO_MCF_VERIFY), 0)) {
                        Oop h2 = home;
                        size_t slow = findBlockHomeFrame(h2, outerContext, compiledBlock);
                        static int nv = 0;
                        if (slow != i - 1 && ++nv <= 10)
                            fprintf(stderr, "[MCF-VERIFY-HOME] fast=%zu slow=%zu fd=%zu\n",
                                    i - 1, slow, frameDepth_);
                    }
                    nlrMemoBlock_ = compiledBlock;
                    nlrMemoHome_ = home;
                    nlrMemoFrame_ = i - 1;
                    savedFrames_[frameDepth_ - 1].setHomeFrameDepth(i - 1);
                    return;
                }
            }
        }
    }
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
    savedFrames_[frameDepth_ - 1].setHomeFrameDepth(homeFrame);
}

void* Interpreter::mcfExtSendTarget(jit::JITState* st, uint32_t desc) {
    mcfDeriveView(*st);
    uint64_t* const siteCache = st->mcfSiteCache;   // set only by an = site
    st->mcfSiteCache = nullptr;
    // st->ip is the send (2 bytes: no prefix).  A primitive run below sees
    // the ip after it, as in the interpreter; on failure T1 code exits at
    // the send again.
    uint8_t* const sendIp = st->ip;
    // Past the send: 2 bytes, or 4 with an ExtendA prefix (a send to
    // literal 32-255).
    st->ip = const_cast<uint8_t*>(mcfPastSend(sendIp));
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
        // A special send is 1 byte: mcfPastSend already answered ip + 1.
        // (This decremented it, so a primitive run here saw the send's own
        // ip and an exit after it resumed the interpreter AT the send.)
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
    // A forwarded receiver (lazy become): the interpreter's send follows it.
    if (__builtin_expect(g_lazyForwarders, 0) && rcv.isObject() && rcv.rawBits() >= 0x10000
            && rcv.asObjectPtr()->isForwarded())
        return nullptr;
    // An = site's slot takes a method or primitive too (its machine code
    // tells the value 1, Object>>='s inline answer, from a method).
    const bool eqSite = special && litIdx == 6 && nArgs == 1
        && !GET_DEBUG_BOOL(PHARO_MCF_NO_EQ_SITE_METHODS);
    if (__builtin_expect(GET_DEBUG_BOOL(PHARO_MCF_EXTSEND_CENSUS), 0)) {
        static std::unordered_map<std::string, uint64_t> by;
        static uint64_t n = 0;
        by[memory_.oopToString(selector) + (special ? " [special]" : "") + " rcv=" +
           (rcv.isObject() ? memory_.classNameOf(rcv) : rcv.isSmallInteger() ? "SmI" : "imm")]++;
        if ((++n & 0xFFFFF) == 0) {
            std::vector<std::pair<uint64_t, std::string>> v;
            for (auto& kv : by) v.push_back({kv.second, kv.first});
            std::sort(v.rbegin(), v.rend());
            for (size_t k = 0; k < v.size() && k < 15; k++)
                fprintf(stderr, "[EXTSEND] %llu %s\n", (unsigned long long)v[k].first, v[k].second.c_str());
        }
    }
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
    // Object>>= (^self == anObject): answer it here, and let the = site
    // answer it inline from now on for this receiver class.
    if (special && litIdx == 6 && nArgs == 1 && mcfIsIdentityEquals(target)) {
        Oop arg = st->sp[-1];
        if (siteCache) {
            siteCache[0] = rcv.isObject() ? (uint64_t)rcv.asObjectPtr()->classIndex()
                                          : (0x80000000ull | (rcv.rawBits() & 7));
            siteCache[1] = 1;
        }
        const bool same = rcv.rawBits() == arg.rawBits()
            || (g_lazyForwarders && memory_.followForwarded(rcv).rawBits()
                                    == memory_.followForwarded(arg).rawBits());
        st->sp[-2] = same ? memory_.trueObject() : memory_.falseObject();
        st->sp -= 1;
        return kMcfExtDone;
    }
    jit::McfCallEntry* e = jit::mcfCallFind(target.rawBits());
    if (!e && !GET_DEBUG_BOOL(PHARO_MCF_NO_EXTSEND_NOTE)) {
        // An ExtSend site's miss exits as ExitSend, which never records a
        // primitive or constant target (only ExitSendCached does): record it
        // here, or the site misses at every execution (4-argument
        // translate:from:to:table:, once per asUppercase).
        mcfNotePrimitiveTarget(target, rcv);
        e = jit::mcfCallFind(target.rawBits());
    }
    if (e && e->jm && (!e->prim || (e->prim & 0xFFFFFF) == jit::kMcfTinyShape)
            && mcfReceiverOk(e->jm, rcv)) {   // a tiny shape is also a native call
        // An ExtSend site's cache: from now on, this receiver class calls
        // the method with no lookup (machine code probes the call cache
        // for it).  Not for a young method (a scavenge moves it) or a
        // receiver whose slot count may differ between instances when the
        // method reads receiver fields.
        if (siteCache && (!special || eqSite || !GET_DEBUG_BOOL(PHARO_MCF_NO_SPECIAL_SITE_CACHE))
                // A super send's target does not depend on the receiver
                // class, so a receiver-class key is still exact for it.
                && (!super || !GET_DEBUG_BOOL(PHARO_MCF_NO_SUPER_SITE_CACHE))
                && !memory_.isYoung(target)
                && (!e->jm->hasRecvFieldAccess
                    || (rcv.isObject() && rcv.asObjectPtr()->format() == ObjectFormat::FixedSize))) {
            siteCache[0] = (rcv.isObject() ? (uint64_t)rcv.asObjectPtr()->classIndex()
                                           : (0x80000000ull | (rcv.rawBits() & 7)))
                         | (eqSite ? (1ULL << 40) : 0);
            siteCache[1] = target.rawBits();
        }
        return e;
    }
    if (e && e->prim == jit::kMcfReturnsValue) {
        // A method answering one constant (mcfConstantReturn).
        st->sp[-(nArgs + 1)] = Oop::fromRawBits(e->bcEndOff);
        st->sp -= nArgs;
        return kMcfExtDone;
    }
    if (__builtin_expect(GET_DEBUG_BOOL(PHARO_MCF_EXTSEND_CENSUS), 0)) {
        static std::unordered_map<std::string, uint64_t> outcome;
        static uint64_t n = 0;
        std::string k = memory_.oopToString(selector) + (!e ? " no-entry"
            : e->prim == jit::kMcfReturnsValue ? " const"
            : (e->jm && !e->prim) ? (mcfReceiverOk(e->jm, rcv) ? " native" : " native-rcv-refused")
            : (!e->jm && e->prim && e->prim < 1024) ? " prim" + std::to_string(e->prim)
            : " other prim=" + std::to_string(e->prim) + " jm=" + std::to_string(e->jm != nullptr));
        outcome[k]++;
        if ((++n & 0xFFFFF) == 0)
            for (auto& kv : outcome)
                if (kv.second > 1000) fprintf(stderr, "[EXTSEND-OUT] %llu %s\n",
                                              (unsigned long long)kv.second, kv.first.c_str());
    }
    if (e && !e->jm && e->prim && e->prim < 1024) {
        // A primitive entry (mcfNotePrimitiveTarget): run it, and let the
        // site's cache run it from now on with no lookup.
        if (siteCache && (!special || eqSite || !GET_DEBUG_BOOL(PHARO_MCF_NO_SPECIAL_SITE_CACHE))
                // A super send's target does not depend on the receiver
                // class, so a receiver-class key is still exact for it.
                && (!super || !GET_DEBUG_BOOL(PHARO_MCF_NO_SUPER_SITE_CACHE))
                && !memory_.isYoung(target)
                && !GET_DEBUG_BOOL(PHARO_MCF_NO_EXTSEND_PRIM_CACHE)) {
            siteCache[0] = (rcv.isObject() ? (uint64_t)rcv.asObjectPtr()->classIndex()
                                           : (0x80000000ull | (rcv.rawBits() & 7)))
                         | (eqSite ? (1ULL << 40) : 0);
            siteCache[1] = target.rawBits();
        }
        st->cachedTarget = target;
        g_mcfPrimFromExt = true;
        const int pr = mcfCallPrimitive(st, (int)e->prim, nArgs);
        g_mcfPrimFromExt = false;
        switch (pr) {
        case 1: return kMcfExtDone;
        case 2: return kMcfExtDeopt;
        default:
            // Failed, stack unchanged: run the method body, as the
            // interpreter would after the failure.
            return mcfPrimFailBodyEntry(target, rcv);
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
            g_mcfPrimFromExt = true;
            const int pr = mcfCallPrimitive(st, p, nArgs);
            g_mcfPrimFromExt = false;
            switch (pr) {
            case 1: return kMcfExtDone;
            case 2: return kMcfExtDeopt;
            default: return mcfPrimFailBodyEntry(target, rcv);   // or nullptr: the interpreter retries it
            }
        }
    }
    return nullptr;
}

// The primitive of `method` failed and left the stack as it was: a call entry
// that runs the method body natively (T1 code for a primitive method starts
// at the body; the primitive itself is not in it), or nullptr.  Not when the
// body first stores the primitive's error code, which only the interpreter
// supplies.  The entry is scratch storage the call stub reads at once.
jit::McfCallEntry* Interpreter::mcfPrimFailBodyEntry(Oop method, Oop rcv) {
    if (GET_DEBUG_BOOL(PHARO_MCF_NO_PRIM_FAIL_BODY)) return nullptr;
    jit::JITMethod* jm = jitRuntime_.methodMap().lookup(method.rawBits());
    if (!jm || !jm->isExecutable() || jm->numBytecodes == 0 || jm->isStubOnEntry) return nullptr;
    if (!mcfReceiverOk(jm, rcv)) return nullptr;
    ObjectHeader* mo = method.asObjectPtr();
    Oop hdr = mo->slotAt(0);
    if (!hdr.isSmallInteger()) return nullptr;
    const uint8_t* bc = mo->bytes() + (1 + (hdr.asSmallInteger() & 0x7FFF)) * 8;
    if (bc[0] != jit::SistaV1::CallPrimitive) return nullptr;
    // terminateTo: declines with live frames (mcfCallPrimitive): the
    // interpreter runs the primitive, never the fallback body.
    if ((bc[1] | (bc[2] << 8)) == 196) return nullptr;
    const uint8_t op = bc[3];   // first bytecode after CallPrimitive
    if (op == jit::SistaV1::ExtStoreTemp || op == jit::SistaV1::ExtPopStoreTemp
            || (op >= jit::SistaV1::PopStoreTempBase && op <= jit::SistaV1::PopStoreTempLast))
        return nullptr;
    const uint64_t bcInfo = mcfPackBcInfo(method.rawBits(),
                                          mo->bytes() + mo->compiledCodeByteSize(), jm);
    if (!bcInfo) return nullptr;
    static jit::McfCallEntry scratch;
    scratch.methodBits = method.rawBits();
    scratch.jm = jm;
    scratch.bcEndOff = bcInfo;
    scratch.prim = 0;
    return &scratch;
}

// A send left machine code for this method.  When it is a primitive T1 code
// can run through pharo_mcf_prim, record it so the next hit stays in
// machine code.  Never displaces a native-call entry for a compiled method.
void Interpreter::mcfNotePrimitiveTarget(Oop method, Oop receiver) {
    if (!method.isObject() || method.rawBits() < 0x10000) return;
    ObjectHeader* mo = method.asObjectPtr();
    if (mo->classIndex() != compiledMethodClassIndex_) return;
    Oop hdr = mo->slotAt(0);
    if (!hdr.isSmallInteger() || !mcfCacheableOop(method)) return;
    if (!((hdr.asSmallInteger() >> 16) & 1)) {
        // A compiled method without a primitive that a send exited for: one
        // the interpreter runs without an activation (its synthetic
        // primitives, e.g. WriteStream>>nextPut:) never reaches mcfActivate,
        // so enter it here, under mcfActivate's conditions.
        if (jit::mcfCallFind(method.rawBits())) return;
        jit::JITMethod* jm = jitRuntime_.methodMap().lookup(method.rawBits());
        Oop constant;
        if (mcfCacheableOop(method) && mcfConstantReturn(method, constant)) {
            // Answered at the call site: no machine code needed.
            jit::McfCallEntry& e = *jit::mcfCallSlotForCall(method.rawBits());
            e.methodBits = method.rawBits();
            noteJitOop(e.methodBits);
            e.jm = jm;
            e.prim = jit::kMcfReturnsValue;
            e.bcEndOff = constant.rawBits();
            return;
        }
        if (!jm || !jm->isExecutable() || jm->numBytecodes == 0 || jm->isStubOnEntry) {
            if (__builtin_expect(GET_DEBUG_BOOL(PHARO_MCF_CENSUS), 0)) {
                static std::unordered_map<std::string, uint64_t> why;
                static uint64_t n = 0;
                why[memory_.selectorOf(method) + (!jm ? " nojm" : !jm->isExecutable() ? " notexec"
                    : jm->numBytecodes == 0 ? " nobc" : " stub")]++;
                if ((++n & 0xFFFF) == 0)
                    for (auto& kv : why)
                        if (kv.second > 1000)
                            fprintf(stderr, "[MCF-NOTE-REFUSED] %llu %s\n",
                                    (unsigned long long)kv.second, kv.first.c_str());
            }
            return;
        }
        if (jm->hasRecvFieldAccess && !receiver.isObject()) return;
        const uint64_t bcInfo = mcfPackBcInfo(method.rawBits(),
                                                   mo->bytes() + mo->compiledCodeByteSize(), jm);
        if (!bcInfo) return;
        jit::McfCallEntry& e = *jit::mcfCallSlotForCall(method.rawBits());
        e.methodBits = method.rawBits();
        noteJitOop(e.methodBits);
        e.jm = jm;
        e.prim = 0;
        e.bcEndOff = bcInfo;
        uint64_t tiny;
        if (mcfTinyShape(method, tiny)) e.prim = tiny;
        return;
    }
    const int p = primitiveIndexOf(method);
    const int pArgs = (int)((hdr.asSmallInteger() >> 24) & 0xF);
    if (p == 83 && pArgs >= 1 && pArgs <= 4 && !GET_DEBUG_BOOL(PHARO_MCF_NO_PERFORM_CALL)) {
        jit::McfCallEntry* ep = jit::mcfCallSlotForPrim(method.rawBits());
        if (!ep) return;
        jit::McfCallEntry& e = *ep;
        e.methodBits = method.rawBits();
        noteJitOop(e.methodBits);
        e.jm = nullptr;
        e.bcEndOff = 0;
        e.prim = jit::kMcfPerformMethod;
        return;
    }
    if ((p == 207 && pArgs >= 2)
            || (p == 209 && pArgs == 0 && !GET_DEBUG_BOOL(PHARO_MCF_NO_VNCS_CALL))) {
        // value:value: and longer: T1 code calls the closure's block natively.
        // Also valueNoContextSwitch (209), how ensure: and ifCurtailed: run
        // their receiver: it activates the block with no interrupt check,
        // and a native call makes none (a refused call exits, and the
        // interpreter's primitive 209 makes none either).
        jit::McfCallEntry* ep = jit::mcfCallSlotForPrim(method.rawBits());
        if (!ep) return;
        jit::McfCallEntry& e = *ep;
        e.methodBits = method.rawBits();
        noteJitOop(e.methodBits);
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
    noteJitOop(e.methodBits);
    e.jm = nullptr;
    e.bcEndOff = 0;
    e.prim = static_cast<uint64_t>(p);
}

// Called once from initializeJIT: SavedFrame layout for T1's native call
// and return, and the call cache.  (g_mcfInterp is defined near the top.)

void Interpreter::mcfClearMemos() {
    for (size_t d = 0; d < frameDepth_ && d < savedFrames_.size(); d++)
        savedFrames_[d].mcReturn = nullptr;
    // Records a non-local return collapsed: tryRestoreCollapsedFrames copies
    // them back whole, memo included, and checks only object motion, so a
    // memo into code freed since would come back.
    for (SavedFrame& r : collapsed_.recs) r.mcReturn = nullptr;
    for (SavedFrame& r : seg_.recs) r.mcReturn = nullptr;
    pendingMcReturn_ = nullptr;
    poppedMcReturn_ = nullptr;
}

void Interpreter::mcfInit() {
    jit::g_lazyCtxPendingAddr = reinterpret_cast<uint64_t>(&lazyCtxPending_);
    jit::McfFrameLayout& L = jit::g_mcfLayout;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
    L.size = sizeof(SavedFrame);
    L.savedIP = offsetof(SavedFrame, savedIP);
    L.savedBytecodeEnd = offsetof(SavedFrame, savedBytecodeEnd);
    L.savedMethod = offsetof(SavedFrame, savedMethod);
    L.savedReceiver = offsetof(SavedFrame, savedReceiver);
    L.savedClosure = offsetof(SavedFrame, savedClosureRaw);
    L.savedActiveContext = offsetof(SavedFrame, savedActiveContextRaw);
    L.materializedContext = offsetof(SavedFrame, materializedContext);
    L.ctxSynced = offsetof(SavedFrame, ctxSynced);
    L.savedFP = offsetof(SavedFrame, savedFP);
    L.materializedRetSlot = offsetof(SavedFrame, materializedRetSlot);
    L.savedArgCount = offsetof(SavedFrame, savedArgCount);
    L.homeFramePlus1 = offsetof(SavedFrame, homeFramePlus1);
    L.mcReturn = offsetof(SavedFrame, mcReturn);
    L.guardOk = reinterpret_cast<char*>(&savedFrames_[0]) - reinterpret_cast<char*>(&mcfGuardFrame_)
                    == (ptrdiff_t)sizeof(SavedFrame)
                && mcfGuardFrame_.mcReturn == nullptr;
    L.mcJM = offsetof(SavedFrame, mcJM);
#pragma GCC diagnostic pop
    // The fields T1 call and return code touch, as offsets from the state
    // (mcfStateStorage_ is a member, so they are fixed): usable when a
    // single ldr/ldur reaches them.
    {
        const char* base = reinterpret_cast<const char*>(&mcfStateStorage_);
        auto direct = [&](const void* field, int size) -> int32_t {
            const ptrdiff_t d = reinterpret_cast<const char*>(field) - base;
            if (GET_DEBUG_BOOL(PHARO_MCF_NO_DIRECT_FIELDS)) return jit::kMcfNoDirect;
            if (d >= -256 && d < 0) return (int32_t)d;
            if (d >= 0 && d % size == 0 && d / size <= 4095) return (int32_t)d;
            return jit::kMcfNoDirect;
        };
        L.dirDepth = direct(&frameDepth_, 8);
        L.dirMatCtx = direct(&currentFrameMaterializedCtx_, 8);
        L.dirActCtx = direct(&activeContext_, 8);
        L.dirCtdn = direct(&checkCountdown_, 4);
    }
    jit::g_mcfMegaCache = jitRuntime_.megaCache();
    g_mcfInterp = this;
    jit::g_mcfClearMemosHook = [] { g_mcfInterp->mcfClearMemos(); };
    // Read by mcfBuildStubs (the primitive stub's Semaphore path).
    jit::g_mcfSpecialObjectsAddr = memory_.specialObjectsArrayAddr();
    jit::g_mcfFinalizationFlagAddr = &finalizationCheckAfterGC_;
    jit::g_mcfFbcClassIndex = fullBlockClosureClassIndex_;   // the stubs bake it
    if (::pharo::mcfEnabled()) jit::mcfBuildStubs();
    jit::g_mcfContextClassIndex = contextClassIndex_;
    // Only in this mode: every code change clears the cache (mcfCodeChanged),
    // a 128 KB memset the default mode must not pay.
    if (::pharo::mcfEnabled() && !jit::g_mcfCallCache) {
        jit::g_mcfCallCache = new jit::McfCallEntry[jit::kMcfCallCacheSize];
        jit::mcfClearCallCache();
    }
}

void* Interpreter::mcfNonLocalReturn(jit::JITState* st) {
    enum { kOk, kInactive, kShallow, kNoHome, kNoCM, kValidate, kEnsure, kRetSlot, kDiag, kN };
    auto refuse = [&](int why) -> void* {
        if (__builtin_expect(GET_DEBUG_BOOL(PHARO_MCF_NLR_CENSUS), 0)) {
            static uint64_t counts[kN] = {};
            static uint64_t n = 0;
            counts[why]++;
            if ((++n & 0xFFFF) == 0)
                fprintf(stderr, "[MCF-NLR] n=%llu ok=%llu inactive=%llu shallow=%llu "
                        "nohome=%llu nocm=%llu validate=%llu ensure=%llu retslot=%llu diag=%llu\n",
                        (unsigned long long)n, (unsigned long long)counts[kOk],
                        (unsigned long long)counts[kInactive], (unsigned long long)counts[kShallow],
                        (unsigned long long)counts[kNoHome], (unsigned long long)counts[kNoCM],
                        (unsigned long long)counts[kValidate], (unsigned long long)counts[kEnsure],
                        (unsigned long long)counts[kRetSlot], (unsigned long long)counts[kDiag]);
        }
        return nullptr;
    };
    if (!mcfActive_ || !running_) return refuse(kInactive);
    static const bool diag = GET_DEBUG_BOOL(PHARO_SP_DEPTH_TRAP)
        || GET_DEBUG_BOOL(PHARO_ACTIVATION_CHECK) || GET_DEBUG_BOOL(PHARO_NLR_CENSUS)
        || GET_DEBUG_BOOL(PHARO_RESUME_EXIT_CHECK) || GET_DEBUG_BOOL(PHARO_RETMETH_TRACE)
        || GET_DEBUG_BOOL(PHARO_T1_TRACE_HIT) || GET_DEBUG_BOOL(PHARO_TRACE_CULL_RETURN)
        || GET_DEBUG_BOOL(PHARO_NATIVE_NLR_UNWIND);
    if (diag) return refuse(kDiag);
    const size_t d = frameDepth_;
    if (d < 2) return refuse(kShallow);
    const size_t h = savedFrames_[d - 1].homeFrameDepth();
    if (h == SIZE_MAX || h == 0 || h >= d) return refuse(kNoHome);
    // The interpreter's registers, as the exit would leave them.  Nothing
    // below changes state until every check has passed.
    mcfDeriveView(*st);
    stackPointer_ = st->sp;
    mcfSyncRegisters(*st);
    instructionPointer_ = st->ip;
    Oop expected = nlrHomeCMOf(method_);
    if (expected.isNil()) return refuse(kNoCM);
    if (validateNLRHomeFrame(h, expected) != h) return refuse(kValidate);
    for (size_t i = h; i < d; i++) {
        Oop m = savedFrames_[i].savedMethod;
        if (m.isObject() && !m.isNil() && primitiveIndexOf(m) == 198) return refuse(kEnsure);
    }
    if (savedFrames_[h - 1].materializedRetSlot) return refuse(kRetSlot);
    if (__builtin_expect(GET_DEBUG_BOOL(PHARO_MCF_NLR_VERIFY), 0)) {
        Oop hm = expected;
        Oop outer = closure_.isObject() && !closure_.isNil()
            ? closure_.asObjectPtr()->slotAt(0) : Oop::nil();
        size_t slow = findBlockHomeFrame(hm, outer, method_);
        static int nv = 0;
        if (slow != h && ++nv <= 10)
            fprintf(stderr, "[MCF-NLR-VERIFY] home: marker=%zu search=%zu depth=%zu\n", h, slow, d);
    }
    refuse(kOk);   // census only
    // Commit: the return bytecode's value, then returnFromMethodSlow's pops.
    Oop value;
    switch (*instructionPointer_) {
        case jit::SistaV1::ReturnReceiver: value = receiver_; break;
        case jit::SistaV1::ReturnTrue:     value = memory_.trueObject(); break;
        case jit::SistaV1::ReturnFalse:    value = memory_.falseObject(); break;
        case jit::SistaV1::ReturnNil:      value = memory_.nil(); break;
        default:                           value = pop(); break;   // ReturnTop
    }
    Oop* homeFP = savedFrames_[h].savedFP;
    while (frameDepth_ > h) {
        widowUnwoundContext();
        if (!popFrame()) return reinterpret_cast<void*>(1);
    }
    nlrHomeMethod_ = Oop::nil();
    nlrValue_ = Oop::nil();
    returnValue(value);
    if (__builtin_expect(GET_DEBUG_BOOL(PHARO_MCF_NLR_VERIFY), 0)) {
        static int nv = 0;
        if ((frameDepth_ != h - 1 || stackPointer_ != homeFP + 1
                || stackPointer_[-1].rawBits() != value.rawBits()) && ++nv <= 10)
            fprintf(stderr, "[MCF-NLR-VERIFY] after: depth=%zu want=%zu sp-fp=%td top-ok=%d\n",
                    frameDepth_, h - 1, stackPointer_ - homeFP,
                    (int)(stackPointer_[-1].rawBits() == value.rawBits()));
    }
    (void)homeFP;
    if (!running_ || frameDepth_ != h - 1 || checkCountdown_ <= 0
            || GET_DEBUG_BOOL(PHARO_MCF_NLR_NO_CONT))
        return reinterpret_cast<void*>(1);
    void* pjm = nullptr;
    void* entry = mcfPoppedMemo(&savedFrames_[frameDepth_], &pjm);
    jit::JITMethod* jm = static_cast<jit::JITMethod*>(pjm);
    if (entry) {
        if (__builtin_expect(GET_DEBUG_BOOL(PHARO_MCF_VERIFY), 0)) {
            SavedFrame f = savedFrames_[frameDepth_];
            f.mcReturn = entry; f.mcJM = pjm;
            mcfVerifyMemo(f, "mcf-nlr");
        }
    } else {
        if (!method_.isObject() || method_.rawBits() < 0x10000) return reinterpret_cast<void*>(1);
        jm = jitRuntime_.methodMap().lookup(method_.rawBits());
        if (!jm || !jm->isExecutable()) return reinterpret_cast<void*>(1);
        if (!(mcfIsMethodFrame() || mcfIsBlockFrame()) || !mcfReceiverOk(jm, receiver_))
            return reinterpret_cast<void*>(1);
        entry = mcfEntryFor(jm, instructionPointer_);
        if (!entry) return reinterpret_cast<void*>(1);
    }
    // Publish the caller's frame as mcfRun does before entering code.
    Oop m = method_;
    ObjectHeader* mo = m.asObjectPtr();
    st->sp = stackPointer_;
    st->receiver = receiver_;
    st->closure = closure_;
    st->literals = mo->slots() + 1;
    st->tempBase = framePointer_ + 1;
    st->ip = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(jm->bcStart()));
    st->jitMethod = jm;
    st->method = m;
    st->argCount = argCount_;
    st->exitReason = jit::ExitNone;
    st->returnValue = Oop::fromRawBits(0);
    st->cachedTarget = Oop::fromRawBits(0);
    st->mcfBcEndOff = static_cast<uint64_t>(
        bytecodeEnd_ - reinterpret_cast<uint8_t*>(m.rawBits()));
    return entry;
}

void Interpreter::mcfResumeAfterReturn(const SavedFrame* popped) {
    if (mcfActive_ || !running_) return;
    jit::JITMethod* jm;
    void* entry;
    // A non-null memo is current: mcfCodeChanged clears them (no epoch).
    void* pjm = nullptr;
    void* pmemo = mcfPoppedMemo(popped, &pjm);
    if (pmemo) {
        if (__builtin_expect(GET_DEBUG_BOOL(PHARO_MCF_VERIFY), 0)) {
            SavedFrame f = *popped;
            f.mcReturn = pmemo; f.mcJM = pjm;
            mcfVerifyMemo(f, "resume-after-return");
        }
        jm = static_cast<jit::JITMethod*>(pjm);
        entry = pmemo;
    } else if (popped && popped->mcReturn
            && popped->savedIP == instructionPointer_) {
        if (__builtin_expect(GET_DEBUG_BOOL(PHARO_MCF_VERIFY), 0))
            mcfVerifyMemo(*popped, "resume-after-return");
        jm = static_cast<jit::JITMethod*>(popped->mcJM);
        entry = popped->mcReturn;
    } else {
        if (!method_.isObject() || method_.rawBits() < 0x10000) return;
        jm = jitRuntime_.methodMap().lookup(method_.rawBits());
        const bool tr = __builtin_expect(GET_DEBUG_BOOL(PHARO_OSR_TRACE), 0) && !closure_.isNil();
        if (!jm || !jm->isExecutable()) { if (tr) fprintf(stderr, "[OSR-R] nojm\n"); return; }
        // OSR (popped == nullptr, from a backward jump): a block frame may
        // enter too.  A loop in a block run once (a test method's or a
        // DoIt's) otherwise ran interpreted, 4x slower.  (Block frames
        // elsewhere: mcfIsBlockFrame, on by default since 2026-10-15.)
        const bool osrBlock = !popped && !GET_DEBUG_BOOL(PHARO_MCF_NO_OSR_BLOCKS)
            && !closure_.isNil()
            && method_.asObjectPtr()->classIndex() == compiledBlockClassIndex_;
        if (!(mcfIsMethodFrame() || mcfIsBlockFrame() || osrBlock)
                || !mcfReceiverOk(jm, receiver_)) {
            if (tr) fprintf(stderr, "[OSR-R] frame m=%d b=%d rcv=%d\n", (int)mcfIsMethodFrame(),
                            (int)mcfIsBlockFrame(), (int)mcfReceiverOk(jm, receiver_));
            return;
        }
        entry = mcfEntryFor(jm, instructionPointer_);
        if (!entry) {
            if (tr) fprintf(stderr, "[OSR-R] noentry bcOff=%ld numBc=%u\n",
                            (long)(instructionPointer_ - (const uint8_t*)jm->bcStart()),
                            jm->numBytecodes);
            return;
        }
        if (tr) fprintf(stderr, "[OSR-R] enter\n");
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
                    + " reason=" + std::to_string(st.exitReason)
                    + [&]() -> std::string {
                          // A block receiver: why the native block call refused it.
                          if (!rcv.isObject() || rcv.asObjectPtr()->classIndex() != jit::g_mcfFbcClassIndex
                                  || rcv.asObjectPtr()->slotCount() < 4) return "";
                          Oop cb = rcv.asObjectPtr()->slotAt(1);
                          if (!cb.isObject()) return " blk=imm";
                          jit::JITMethod* bj = jitRuntime_.methodMap().lookup(cb.rawBits());
                          jit::McfCallEntry* be = jit::mcfCallFind(cb.rawBits());
                          return std::string(" blk=") + (!bj ? "nojm" : !bj->isExecutable() ? "noexec"
                                 : bj->numBytecodes == 0 ? "nobc" : bj->isStubOnEntry ? "stub" : "jm")
                                 + (memory_.isYoung(cb) ? ",young" : "")
                                 + (be ? ",cached" : ",notcached")
                                 + (mcfBlockHasNLR(cb) ? ",nlr" : "")
                                 + " home=" + memory_.selectorOf(cb);
                      }();
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

// The selector of the send at ip (no prefixes: T1 sites are naked).
Oop Interpreter::mcfSelectorAtSend(const uint8_t* ip, Oop method) {
    const uint8_t op = *ip;
    ObjectHeader* mo = method.asObjectPtr();
    if (op >= jit::SistaV1::ArithBase && op <= jit::SistaV1::SpecialSendLast) {
        Oop ss = memory_.specialObject(SpecialObjectIndex::SpecialSelectorsArray);
        return ss.asObjectPtr()->slotAt((size_t)(op - jit::SistaV1::ArithBase) * 2);
    }
    int lit = -1;
    if (op >= jit::SistaV1::Send0Base && op <= jit::SistaV1::Send2Last) lit = op & 0x0F;
    else if (op == jit::SistaV1::ExtSend) lit = ip[1] >> 3;
    if (lit < 0) return Oop::nil();
    Oop hdr = mo->slotAt(0);
    if (!hdr.isSmallInteger() || lit >= (int)(hdr.asSmallInteger() & 0x7FFF)) return Oop::nil();
    return mo->slotAt(1 + lit);
}

void Interpreter::mcfFillICAtExit(jit::JITState& st) {
    if (!st.ip || !method_.isObject()) return;
    Oop sel = mcfSelectorAtSend(st.ip, method_);
    if (!sel.isObject() || sel.isNil()) return;
    Oop rcv = st.sp[-(st.sendArgCount + 1)];
    Oop cls = memory_.classOf(rcv);
    if (!cls.isObject() || cls.isNil()) return;
    Oop target = lookupMethodForSend(sel, cls);
    if (!target.isObject() || target.isNil() || !target.asObjectPtr()->isCompiledMethod()) return;
    patchJITICAfterSend(target, rcv, sel);
}

void Interpreter::mcfRun(jit::JITMethod* jm, void* entry) {
    if (!mcfState_) {
        // Fields that never change for this interpreter.
        mcfState_ = &mcfStateStorage_;
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
        // The fused depth test (trim step 4) calls natively only up to
        // depth 32768; pharo_mcf_perform must not answer a call the stub
        // then refuses.
        c.mcfDepthLimit = GET_DEBUG_BOOL(PHARO_MCF_NO_FUSED_DEPTH)
            ? StackOverflowLimit - 64 : 32768;
        c.mcfEpochPtr = &jit::g_mcCodeEpoch;
        c.mcfCallCache = jit::g_mcfCallCache;
        c.mcfCallStubAddr = jit::g_mcfCallStub;
        c.mcfBlockStubAddr = jit::g_mcfBlockStub;
        for (int k = 0; k < 3; k++) c.mcfPrimStubAddr[k] = jit::g_mcfPrimStub[k];
        c.mcfTinyStubAddr = jit::g_mcfTinyStub;
        c.mcfBlockCreateStubAddr = jit::g_mcfBlockCreateStub;
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
        mcfDeriveView(st);

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
                k += " op=" + std::to_string(*st.ip) + " in #" + memory_.selectorOf(st.method)
                     + " ctdn=" + (checkCountdown_ <= 0 ? std::string("<=0") : checkCountdown_ == 1 ? std::string("1") : std::string(">1"))
                     + " fy=" + std::to_string((int)forceYield_.load());
                if (st.method.isObject()
                        && st.method.asObjectPtr()->classIndex() == compiledBlockClassIndex_) {
                    Oop home = blockHomeMethodForNLR(st.method);
                    if (home.isObject() && !home.isNil())
                        k += " home=" + classNameOfMethod(home) + ">>#" + memory_.selectorOf(home);
                }
            }
            if (st.exitReason == jit::ExitReturn) {
                size_t d = frameDepth_;
                if (d == 0) k += " why=depth0";
                else {
                    const SavedFrame& f = savedFrames_[d - 1];
                    k += f.homeFrameDepth() != SIZE_MAX ? (blockFrame ? " home(blk)" : " why=home") : "";
                    k += f.materializedRetSlot ? " why=retslot" : "";
                    k += !f.mcReturn ? " why=nomemo" : "";
                    k += blockFrame ? " blk" : " meth";
                    k += " caller=" + classNameOfMethod(f.savedMethod) + ">>#"
                         + memory_.selectorOf(f.savedMethod)
                         + " callee=" + classNameOfMethod(st.method);
                    if (!f.mcReturn) {
                        jit::JITMethod* cjm = jitRuntime_.methodMap().lookup(f.savedMethod.rawBits());
                        const uint8_t* mb = f.savedMethod.asObjectPtr()->bytes();
                        k += " cjm=" + std::to_string(cjm != nullptr)
                             + " entry=" + std::to_string(cjm ? mcfEntryFor(cjm, f.savedIP) != nullptr : 0)
                             + " ipoff=" + std::to_string(f.savedIP - mb)
                             + " nextop=" + std::to_string(*f.savedIP)
                             + " blk=" + std::to_string(!recClosure(f).isNil());
                    }
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
                        // The interpreter runs some sends without a lookup
                        // (special selectors such as size and at:, synthetic
                        // primitives such as WriteStream>>nextPut:) and so
                        // never applied the patch: those sites missed at every
                        // execution (3M exits in lexicon).  Fill it now.
                        if (!GET_DEBUG_BOOL(PHARO_MCF_NO_EAGER_IC_FILL))
                            mcfFillICAtExit(st);
                        break;
                    }
                }
            }
            [[fallthrough]];
        case jit::ExitSendCached: {
            instructionPointer_ = st.ip;
            if (st.exitReason == jit::ExitSendCached) {
                mcfNotePrimitiveTarget(st.cachedTarget, st.sp[-(st.sendArgCount + 1)]);
                // A site the generic path calls natively (a refusal brought
                // it here, ~1 in 1024 calls): link it to the callee's entry.
                // Sites unlinked by eviction or a purge relink this way.
                const jit::McfCallEntry* ce = jit::mcfCallFind(st.cachedTarget.rawBits());
                if (ce && ce->jm && ce->prim == 0 && st.icDataPtr
                        && !GET_DEBUG_BOOL(PHARO_MCF_NO_LINK))
                    jitRuntime_.rederiveSiteForICData(jm->compiledMethodOop, st.icDataPtr);
            }

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
                                || savedFrames_[depth - 1].homeFrameDepth() != SIZE_MAX)) {
                mcfReturnedIntoContext_ = false;
                returnFromMethodSlow(value);
                // A collapsed non-local return's frames came back
                // (tryRestoreCollapsedFrames) and the return popped into
                // one: continue it natively.  mcfResumeAfterReturn did
                // nothing (mcfActive_), so the frame ran interpreted up to
                // its next send before (1.7M times per lexicon run).
                if (depth == 0 && running_ && frameDepth_ > 0 && checkCountdown_ > 0
                        && !GET_DEBUG_BOOL(PHARO_MCF_NO_RESTORE_CONT)) {
                    void* pjm = nullptr;
                    if (void* pmemo = mcfPoppedMemo(&savedFrames_[frameDepth_], &pjm)) {
                        if (__builtin_expect(GET_DEBUG_BOOL(PHARO_MCF_VERIFY), 0)) {
                            SavedFrame f = savedFrames_[frameDepth_];
                            f.mcReturn = pmemo; f.mcJM = pjm;
                            mcfVerifyMemo(f, "mcfRun-restore");
                        }
                        jm = static_cast<jit::JITMethod*>(pjm);
                        entry = pmemo;
                        continue;
                    }
                    if (method_.isObject() && method_.rawBits() >= 0x10000) {
                        jit::JITMethod* rj = jitRuntime_.methodMap().lookup(method_.rawBits());
                        if (rj && rj->isExecutable() && (mcfIsMethodFrame() || mcfIsBlockFrame())
                                && mcfReceiverOk(rj, receiver_)) {
                            if (void* re = mcfEntryFor(rj, instructionPointer_)) {
                                jm = rj;
                                entry = re;
                                continue;
                            }
                        }
                    }
                }
                // Returned into a context (depth 0): continue it here when its
                // method has machine code, as mcfResumeAfterReturn does.
                if (mcfReturnedIntoContext_ && running_ && frameDepth_ == 0
                        && checkCountdown_ > 0 && method_.isObject()) {
                    mcfReturnedIntoContext_ = false;
                    jit::JITMethod* rj = jitRuntime_.methodMap().lookup(method_.rawBits());
                    if (rj && rj->isExecutable() && mcfIsMethodFrame() && mcfReceiverOk(rj, receiver_)) {
                        if (void* re = mcfEntryFor(rj, instructionPointer_)) {
                            jm = rj;
                            entry = re;
                            continue;
                        }
                    }
                }
                mcfActive_ = false;
                return;
            }
            returnValue(value);
            if (!running_ || frameDepth_ != depth - 1 || checkCountdown_ <= 0) {
                mcfActive_ = false;
                return;
            }
            const SavedFrame& popped = savedFrames_[frameDepth_];
            {
                void* pjm = nullptr;
                if (void* pmemo = mcfPoppedMemo(&popped, &pjm)) {
                    if (__builtin_expect(GET_DEBUG_BOOL(PHARO_MCF_VERIFY), 0)) {
                        SavedFrame f = popped;
                        f.mcReturn = pmemo; f.mcJM = pjm;
                        mcfVerifyMemo(f, "mcfRun-return");
                    }
                    jm = static_cast<jit::JITMethod*>(pjm);
                    entry = pmemo;
                    continue;
                }
            }
            if (popped.mcReturn
                    && popped.savedIP == instructionPointer_) {
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
            // Do not force the pending yield here (checkCountdown_ = 1, tried
            // 2026-10-14): that switched process at the bytecode after any
            // exit -- block creation, overflow, must-be-boolean -- where Cog
            // never switches (only at sends and back jumps), and RPC tests in
            // myprecious then timed out 1 run in 3 (0 in 6 without it).
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
void* Interpreter::mcfNonLocalReturn(jit::JITState*) { return nullptr; }
void Interpreter::mcfRun(jit::JITMethod*, void*) {}
void Interpreter::mcfCensus(const jit::JITState&) {}
void Interpreter::mcfInit() {}
void Interpreter::mcfClearMemos() {}
void Interpreter::mcfDeriveView(jit::JITState&) {}
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
