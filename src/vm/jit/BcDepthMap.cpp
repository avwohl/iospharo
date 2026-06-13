/*
 * BcDepthMap.cpp - static operand-stack depth verification for JIT exits
 *
 * Copyright (c) 2026 Aaron Wohl. Licensed under the MIT License.
 *
 * See BcDepthMap.hpp for the design.  The walker mirrors the T1 decoder
 * exactly (AsmjitT1.cpp first pass / computeLiveLength): bcLenRaw =
 * slotCount*8 - (1+numLits)*8 - unusedBytes, ExtJump target = i+2+int8
 * (no prefix support), ExtSend nArgs = (operand & 7) + extB*8.  Trailer
 * bytes past the last return are never reached because the walk only
 * follows control flow.
 */

#include "BcDepthMap.hpp"
#include "JITConfig.hpp"
#include "../DebugTrap.hpp"

#if PHARO_JIT_ENABLED

#include "JITState.hpp"
#include "JITMethod.hpp"
#include "SistaV1.hpp"
#include "../ObjectMemory.hpp"
#include "../DebugVars.hpp"

#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace pharo {
namespace jit {

namespace {

struct DepthEntry {
    uint64_t methodOopBits = 0;   // staleness check (GC move / JM recycle)
    bool unmappable = false;
    std::vector<int16_t> depth;   // index = bcOffset; -1 = unreachable
};

std::unordered_map<const JITMethod*, DepthEntry> g_depthMaps;
// Interp-side maps keyed by method oop bits; cleared after moving GC.
std::unordered_map<uint64_t, DepthEntry> g_interpDepthMaps;

uint64_t g_checks = 0;
uint64_t g_mismatches = 0;
uint64_t g_mapsBuilt = 0;
uint64_t g_unmappable = 0;
uint64_t g_ipOutOfRange = 0;
uint64_t g_unreachableOffset = 0;
uint64_t g_interpChecks = 0;
uint64_t g_interpMismatches = 0;
uint64_t g_saveChecks = 0;
uint64_t g_saveMismatches = 0;

// Arg counts for special-selector sends 0x70-0x7F, from the image's
// SpecialSelectorsArray [sel0, argc0, sel1, argc1, ...] entries 16-31.
// Image-lifetime constant; loaded once.
uint8_t g_specialArgs[16];
bool g_specialArgsLoaded = false;

bool loadSpecialArgCounts(ObjectMemory* memory) {
    if (g_specialArgsLoaded) return true;
    Oop arr = memory->specialObject(SpecialObjectIndex::SpecialSelectorsArray);
    if (!arr.isObject() || arr.rawBits() < 0x10000) return false;
    ObjectHeader* hdr = arr.asObjectPtr();
    size_t slots = hdr->slotCount();
    for (int k = 0; k < 16; k++) {
        size_t argSlot = (16 + k) * 2 + 1;
        if (argSlot >= slots) return false;
        Oop argc = hdr->slotAt(argSlot);
        if (!argc.isSmallInteger()) return false;
        int64_t v = argc.asSmallInteger();
        if (v < 0 || v > 15) return false;
        g_specialArgs[k] = static_cast<uint8_t>(v);
    }
    g_specialArgsLoaded = true;
    return true;
}

// Diagnostics for the most recent computeDepthMap failure (read by the
// unmappable report in buildEntryForMethodOop).
uint32_t g_lastFailPc = 0;
uint8_t g_lastFailOp = 0;
const char* g_lastFailWhy = "?";

// Worklist propagation of operand depth.  out[k] = depth BEFORE the
// bytecode at offset k.  Returns false (unmappable) on: opcodes with
// non-static stack effects, unassigned opcodes, truncated multi-byte
// ops, depth underflow, branch out of range, or a merge conflict.
bool computeDepthMap(const uint8_t* bc, size_t bcLen,
                     std::vector<int16_t>& out)
{
    namespace SV1 = SistaV1;
    out.assign(bcLen, -1);
    std::vector<std::pair<uint32_t, int32_t>> work;
    work.push_back({0u, 0});

    auto branchTo = [&](int64_t target, int32_t d) -> bool {
        if (target < 0 || static_cast<size_t>(target) >= bcLen) return false;
        if (out[target] != -1) return out[target] == d;
        work.push_back({static_cast<uint32_t>(target), d});
        return true;
    };

    while (!work.empty()) {
        uint32_t pc = work.back().first;
        int32_t depth = work.back().second;
        work.pop_back();
        int32_t extA = 0, extB = 0;
        bool extBSet = false, extASet = false;

        for (;;) {
            if (pc >= bcLen) return false;
            if (out[pc] != -1) {
                if (out[pc] != depth) return false;     // merge conflict
                break;                                  // already walked
            }
            if (depth < 0 || depth > 0x7FF) return false;
            out[pc] = static_cast<int16_t>(depth);

            uint8_t op = bc[pc];
            // Failure diagnostics: any `return false` below reports the
            // op being processed (merge conflicts report the prior op —
            // close enough for the unmappable triage line).
            g_lastFailPc = pc;
            g_lastFailOp = op;
            int len = SV1::bytecodeLength(op);
            if (pc + static_cast<uint32_t>(len) > bcLen) return false;
            bool terminator = false;

            if (op <= SV1::PushTempLast) {                       // 0x00-0x4B pushes
                depth += 1;
            } else if (op >= SV1::PushReceiver && op <= SV1::PushThisContext) {
                depth += 1;                                      // 0x4C-0x52
            } else if (op == SV1::Dup) {
                depth += 1;
            } else if (op >= 0x54 && op <= 0x57) {
                return false;                                    // unassigned
            } else if (SV1::isReturn(op) || SV1::isBlockReturn(op)) {
                terminator = true;
            } else if (op == 0x5F) {
                return false;                                    // unassigned
            } else if (SV1::isArithSelector(op)) {
                depth -= 1;                                      // pop 2 push 1
            } else if (SV1::isSpecialSelector(op)) {
                if (extASet || extBSet) return false;            // prefixed: bail
                depth -= g_specialArgs[op - SV1::SpecialSendBase];
            } else if (SV1::isSend0(op)) {
                /* pop recv, push result: net 0 */
            } else if (SV1::isSend1(op)) {
                depth -= 1;
            } else if (SV1::isSend2(op)) {
                depth -= 2;
            } else if (SV1::isShortJump(op)) {
                if (!branchTo(SV1::shortJumpTarget(op, pc), depth)) return false;
                terminator = true;
            } else if (SV1::isConditionalShortJump(op)) {
                depth -= 1;
                if (depth < 0) return false;
                if (!branchTo(SV1::shortJumpTarget(op, pc), depth)) return false;
            } else if (SV1::isPopStoreRecv(op) || SV1::isPopStoreTemp(op)) {
                depth -= 1;
            } else if (op == SV1::Pop) {
                depth -= 1;
            } else if (op == 0xD9) {
                terminator = true;                               // trap send
            } else if (op >= 0xDA && op <= 0xDF) {
                return false;                                    // reserved
            } else if (op == SV1::ExtendA) {
                extA = extA * 256 + bc[pc + 1];
                extASet = true;
            } else if (op == SV1::ExtendB) {
                int b = bc[pc + 1];
                extB = (!extBSet && b > 127) ? b - 256 : extB * 256 + b;
                extBSet = true;
            } else if (op >= SV1::ExtPushRecvVar && op <= SV1::ExtPushTemp) {
                depth += 1;
            } else if (op == 0xE6) {
                return false;                                    // unassigned
            } else if (op == SV1::PushArray) {
                uint8_t desc = bc[pc + 1];
                if (desc & 0x80) depth -= (desc & 0x7F);         // popIntoArray
                if (depth < 0) return false;
                depth += 1;
            } else if (op == SV1::PushInteger || op == SV1::PushCharacter) {
                depth += 1;
            } else if (op == SV1::ExtSend || op == SV1::ExtSuperSend) {
                if (extB < 0 || extB > 3) return false;          // odd encodings: bail
                int nArgs = (bc[pc + 1] & 7) + extB * 8;
                depth -= nArgs;
            } else if (op == SV1::InlinedPrimitive) {
                return false;                                    // non-static effect
            } else if (op == SV1::ExtJump) {
                // Interp semantics (Interpreter.cpp ExtJump): operand is
                // UNSIGNED; the sign lives in extB (offset = byte +
                // (extB << 8)).  NOT int8 — T1's decoder reads int8 and
                // is wrong for naked forward jumps >= 128 (latent T1
                // bug, noted in WIP).
                int64_t off = bc[pc + 1] + (static_cast<int64_t>(extB) << 8);
                if (!branchTo(static_cast<int64_t>(pc) + 2 + off, depth)) return false;
                terminator = true;
            } else if (op == SV1::ExtJumpTrue || op == SV1::ExtJumpFalse) {
                depth -= 1;
                if (depth < 0) return false;
                int64_t off = bc[pc + 1] + (static_cast<int64_t>(extB) << 8);
                if (!branchTo(static_cast<int64_t>(pc) + 2 + off, depth)) return false;
            } else if (op >= SV1::ExtPopStoreRecv && op <= SV1::ExtPopStoreTemp) {
                depth -= 1;
            } else if (op >= SV1::ExtStoreRecv && op <= SV1::ExtStoreTemp) {
                /* store without pop: net 0 */
            } else if (op == 0xF6 || op == 0xF7) {
                return false;                                    // unassigned
            } else if (op == SV1::CallPrimitive) {
                /* prologue marker: net 0 */
            } else if (op == SV1::PushFullBlock) {
                uint8_t flags = bc[pc + 2];
                depth -= (flags & 0x3F);                         // numCopied
                depth -= (flags >> 7) & 1;                       // receiverOnStack
                if (depth < 0) return false;
                depth += 1;
            } else if (op == SV1::PushClosure) {
                return false;                                    // legacy, inline body
            } else if (op == SV1::PushTempAtInVec) {
                depth += 1;
            } else if (op == SV1::StoreTempAtInVec) {
                /* net 0 */
            } else if (op == SV1::PopStoreTempAtInVec) {
                depth -= 1;
            } else {
                return false;                                    // 0xFE/0xFF
            }

            if (op != SV1::ExtendA && op != SV1::ExtendB) {
                extA = 0; extB = 0; extASet = false; extBSet = false;
            }
            if (terminator) break;
            if (depth < 0) return false;
            pc += static_cast<uint32_t>(len);
        }
    }
    return true;
}

// Fill `e` with the depth map for the CompiledMethod at oopBits.
// Leaves e.unmappable = true on any failure.
void buildEntryForMethodOop(uint64_t oopBits, DepthEntry& e) {
    e.methodOopBits = oopBits;
    e.unmappable = true;
    e.depth.clear();

    Oop methodOop = Oop::fromRawBits(oopBits);
    if (!methodOop.isObject() || methodOop.rawBits() < 0x10000) {
        g_unmappable++;
        return;
    }
    ObjectHeader* methObj = methodOop.asObjectPtr();
    Oop headerOop = methObj->slotAt(0);
    if (!headerOop.isSmallInteger()) { g_unmappable++; return; }
    int64_t headerBits = headerOop.asSmallInteger();
    size_t numLits = static_cast<size_t>(headerBits & 0x7FFF);
    size_t bcStartOff = (1 + numLits) * 8;
    size_t totalBytes = methObj->slotCount() * 8;
    uint8_t fmt = static_cast<uint8_t>(methObj->format());
    size_t unusedBytes = (fmt >= 24) ? static_cast<size_t>(fmt - 24) : 0;
    if (bcStartOff + unusedBytes >= totalBytes) { g_unmappable++; return; }
    size_t bcLen = totalBytes - bcStartOff - unusedBytes;
    if (bcLen > 0xFFFF) { g_unmappable++; return; }
    const uint8_t* bc = methObj->bytes() + bcStartOff;

    g_mapsBuilt++;
    if (!computeDepthMap(bc, bcLen, e.depth)) {
        e.depth.clear();
        g_unmappable++;
        // Unmappable methods are silent blind spots — name them (capped)
        // so a victim method hiding in the unmappable set is visible.
        static int unmapReports = 0;
        if (++unmapReports <= 60) {
            fprintf(stderr,
                    "[SP-DEPTH-UNMAPPABLE #%d] oop=0x%llx bcLen=%zu "
                    "failPc=%u failOp=0x%02x\n",
                    unmapReports, (unsigned long long)oopBits, bcLen,
                    g_lastFailPc, g_lastFailOp);
        }
        return;
    }
    e.unmappable = false;
}

// Build (or rebuild after GC-move/recycle) the depth map for jm.
// Returns nullptr if the method is unmappable.
const DepthEntry* depthEntryFor(const JITMethod* jm) {
    auto it = g_depthMaps.find(jm);
    if (it != g_depthMaps.end()
            && it->second.methodOopBits == jm->compiledMethodOop) {
        return it->second.unmappable ? nullptr : &it->second;
    }
    DepthEntry& e = g_depthMaps[jm];
    buildEntryForMethodOop(jm->compiledMethodOop, e);
    return e.unmappable ? nullptr : &e;
}

}  // namespace

void spDepthCheck(JITState& state, const char* where) {
    if (!GET_DEBUG_BOOL(PHARO_SP_DEPTH_CHECK)) return;
    int er = state.exitReason;
    // Only exits whose ip points AT a bytecode with operands intact.
    // ExitJ2JCall is also checkable: ip points PAST the send bytecode,
    // recovered below by backscanning for the send opcode.
    if (er != ExitSend && er != ExitSendCached && er != ExitMustBool
            && er != ExitArithOverflow && er != ExitBlockCreate
            && er != ExitArrayCreate && er != ExitYield
            && er != ExitJ2JCall) {
        return;
    }
    auto* jm = reinterpret_cast<JITMethod*>(
        reinterpret_cast<uintptr_t>(state.jitMethod) & ~static_cast<uintptr_t>(1));
    if (!jm || !state.memory || !state.tempBase || !state.ip) return;
    if (!loadSpecialArgCounts(state.memory)) return;

    const DepthEntry* entry = depthEntryFor(jm);
    if (!entry) return;

    const uint8_t* bcStart = jm->bcStart();
    int64_t bcOff = state.ip - bcStart;
    if (er == ExitJ2JCall) {
        // ip points one past the send.  Backscan: plain sends are
        // 1 byte (0x60-0xAF), ExtSend/ExtSuperSend are 2.  If both
        // readings are plausible (the ExtSend operand byte can look
        // like a send opcode), the site is ambiguous — skip it.
        int64_t off1 = bcOff - 1, off2 = bcOff - 2;
        bool plain = off1 >= 0
            && static_cast<size_t>(off1) < entry->depth.size()
            && SistaV1::isSendBytecode(bcStart[off1])
            && SistaV1::bytecodeLength(bcStart[off1]) == 1
            && entry->depth[off1] >= 0;
        bool ext = off2 >= 0
            && static_cast<size_t>(off2) < entry->depth.size()
            && (bcStart[off2] == SistaV1::ExtSend
                || bcStart[off2] == SistaV1::ExtSuperSend)
            && entry->depth[off2] >= 0;
        if (plain == ext) return;     // neither or ambiguous
        bcOff = plain ? off1 : off2;
        // Operands must at least cover recv + args of the in-flight send.
        if (entry->depth[bcOff] < state.sendArgCount + 1) return;
    }
    g_checks++;
    if (bcOff < 0 || static_cast<size_t>(bcOff) >= entry->depth.size()) {
        // ip outside the method's bytecodes is itself a finding (stale
        // ip / wrong jitMethod), reported via the same channel.
        g_ipOutOfRange++;
        g_mismatches++;
        static int oorReports = 0;
        if (++oorReports <= 40) {
            fprintf(stderr,
                    "[SP-DEPTH #%llu] %s IP-OUT-OF-RANGE exit=%d sel=%s "
                    "bcOff=%lld bcLen=%zu jm=%p isBlock=%d\n",
                    (unsigned long long)g_mismatches, where, er,
                    state.memory->selectorOf(
                        Oop::fromRawBits(jm->compiledMethodOop)).c_str(),
                    (long long)bcOff, entry->depth.size(), (void*)jm,
                    (int)jm->isBlock);
        }
        return;
    }
    int16_t d = entry->depth[bcOff];
    if (d < 0) {
        // Executing an offset the walker proved unreachable: also a
        // finding (control flow escaped the static CFG).
        g_unreachableOffset++;
        g_mismatches++;
        static int unrReports = 0;
        if (++unrReports <= 40) {
            fprintf(stderr,
                    "[SP-DEPTH #%llu] %s UNREACHABLE-OFFSET exit=%d sel=%s "
                    "bcOff=%lld op=0x%02x jm=%p isBlock=%d\n",
                    (unsigned long long)g_mismatches, where, er,
                    state.memory->selectorOf(
                        Oop::fromRawBits(jm->compiledMethodOop)).c_str(),
                    (long long)bcOff, bcStart[bcOff], (void*)jm,
                    (int)jm->isBlock);
        }
        return;
    }
    // sp convention is one-past-TOS (stackTop() = *(sp-1)); with an
    // empty operand stack sp = tempBase + tempCount.
    Oop* expected = state.tempBase + jm->tempCount + d;
    // ExitMustBool: the emit may have consumed the condition before
    // exiting (the chain loop pushes it back) — accept d or d-1.
    if (er == ExitMustBool && state.sp == expected - 1) return;
    if (state.sp != expected) {
        g_mismatches++;
        static int mmReports = 0;
        if (++mmReports <= 40) {
            fprintf(stderr,
                    "[SP-DEPTH #%llu] %s exit=%d sel=%s bcOff=%lld op=0x%02x "
                    "depth=%d sp=%p expected=%p delta=%lld words "
                    "tempBase=%p tempCount=%d isBlock=%d j2jDepth=%d jm=%p\n",
                    (unsigned long long)g_mismatches, where, er,
                    state.memory->selectorOf(
                        Oop::fromRawBits(jm->compiledMethodOop)).c_str(),
                    (long long)bcOff, bcStart[bcOff], (int)d,
                    (void*)state.sp, (void*)expected,
                    (long long)(state.sp - expected),
                    (void*)state.tempBase, (int)jm->tempCount,
                    (int)jm->isBlock, state.j2jDepth, (void*)jm);
        }
        // Cascade-#3 lldb hook: trap at the mismatch so the debugger
        // lands ON the inconsistent frame with all state live.
        if (GET_DEBUG_BOOL(PHARO_SP_DEPTH_TRAP)) {
            PHARO_DEBUGTRAP();
        }
    }
}

void spDepthCheckInterpSend(uint64_t methodOopBits,
                            const uint8_t* ipPastSend,
                            void* spRaw, void* fpRaw, int argCount,
                            ObjectMemory* memory,
                            const char* where) {
    if (!GET_DEBUG_BOOL(PHARO_SP_DEPTH_CHECK)) return;
    if (!methodOopBits || !ipPastSend || !spRaw || !fpRaw || !memory) return;
    if (!loadSpecialArgCounts(memory)) return;

    auto it = g_interpDepthMaps.find(methodOopBits);
    if (it == g_interpDepthMaps.end()) {
        DepthEntry& fresh = g_interpDepthMaps[methodOopBits];
        buildEntryForMethodOop(methodOopBits, fresh);
        it = g_interpDepthMaps.find(methodOopBits);
    }
    const DepthEntry& e = it->second;
    if (e.unmappable) return;

    Oop methodOop = Oop::fromRawBits(methodOopBits);
    ObjectHeader* methObj = methodOop.asObjectPtr();
    Oop headerOop = methObj->slotAt(0);
    if (!headerOop.isSmallInteger()) return;
    int64_t headerBits = headerOop.asSmallInteger();
    // Primitive methods: the failure path can leave an extra slot (the
    // <primitive:error:> error value) the temp count doesn't predict —
    // skip them rather than special-case the layout.
    if ((headerBits >> 16) & 1) return;
    size_t numLits = static_cast<size_t>(headerBits & 0x7FFF);
    int numTemps = static_cast<int>((headerBits >> 18) & 0x3F);
    const uint8_t* bcStart = methObj->bytes() + (1 + numLits) * 8;

    // Backscan from the post-fetch ip to the send opcode (1-byte plain
    // send vs 2-byte ExtSend).  Skip silently when out of range or
    // ambiguous — sendSelector is reached from block frames and JIT
    // bails where method/ip don't pair like this.
    int64_t bcOffPast = ipPastSend - bcStart;
    int64_t off1 = bcOffPast - 1, off2 = bcOffPast - 2;
    bool plain = off1 >= 0
        && static_cast<size_t>(off1) < e.depth.size()
        && SistaV1::isSendBytecode(bcStart[off1])
        && SistaV1::bytecodeLength(bcStart[off1]) == 1
        && e.depth[off1] >= 0;
    bool ext = off2 >= 0
        && static_cast<size_t>(off2) < e.depth.size()
        && (bcStart[off2] == SistaV1::ExtSend
            || bcStart[off2] == SistaV1::ExtSuperSend)
        && e.depth[off2] >= 0;
    if (plain == ext) return;
    int64_t sendOff = plain ? off1 : off2;
    int16_t d = e.depth[sendOff];
    if (d < argCount + 1) return;   // operands must cover recv+args

    // sendSelector is also reached from internal resends (arith
    // coercion adaptTo*, value-retry, DNU) whose argCount differs from
    // the bytecode site's — require the site's own arg count to match
    // or skip.  (For prefixed ExtSend the naked operand&7 mismatches
    // and skips too, which is fine.)
    {
        namespace SV1 = SistaV1;
        uint8_t sop = bcStart[sendOff];
        int siteArgs;
        if (SV1::isArithSelector(sop)) siteArgs = 1;
        else if (SV1::isSpecialSelector(sop))
            siteArgs = g_specialArgs[sop - SV1::SpecialSendBase];
        else if (SV1::isSend0(sop)) siteArgs = 0;
        else if (SV1::isSend1(sop)) siteArgs = 1;
        else if (SV1::isSend2(sop)) siteArgs = 2;
        else siteArgs = bcStart[sendOff + 1] & 7;   // naked ExtSend
        if (siteArgs != argCount) return;
    }

    g_interpChecks++;
    // fp points at the receiver slot; temps at fp+1; sp one-past-TOS.
    Oop* fp = static_cast<Oop*>(fpRaw);
    Oop* sp = static_cast<Oop*>(spRaw);
    Oop* expected = fp + 1 + numTemps + d;
    if (sp != expected) {
        g_interpMismatches++;
        // Dedup per (method, site, delta): a handful of JIT-bail sites
        // report with a half-synced framePointer_ every time they run
        // — without dedup they exhaust the report cap and mask new
        // sites (the actual corruption victims).
        static std::unordered_set<uint64_t> reportedSites;
        uint64_t siteKey = methodOopBits ^ (static_cast<uint64_t>(sendOff) << 48)
            ^ (static_cast<uint64_t>(static_cast<int64_t>(sp - expected) & 0xFF) << 40);
        if (!reportedSites.insert(siteKey).second) return;
        static int imReports = 0;
        if (++imReports <= 40) {
            fprintf(stderr,
                    "[SP-DEPTH-INTERP #%llu] %s sel-args=%d sel=%s bcOff=%lld "
                    "op=0x%02x depth=%d sp=%p expected=%p delta=%lld words "
                    "fp=%p numTemps=%d\n",
                    (unsigned long long)g_interpMismatches, where, argCount,
                    memory->selectorOf(methodOop).c_str(),
                    (long long)sendOff, bcStart[sendOff], (int)d,
                    (void*)sp, (void*)expected,
                    (long long)(sp - expected), (void*)fp, numTemps);
        }
    }
}

void spDepthCheckJ2JSave(uint64_t saveJmMethodOop, const uint8_t* saveIp,
                         void* saveSp, void* saveTempBase,
                         int tempCount, int sendArgCount,
                         ObjectMemory* memory, const char* where) {
    if (!GET_DEBUG_BOOL(PHARO_SP_DEPTH_CHECK)) return;
    if (!saveJmMethodOop || !saveIp || !saveSp || !saveTempBase || !memory) return;
    if (!loadSpecialArgCounts(memory)) return;

    auto it = g_interpDepthMaps.find(saveJmMethodOop);
    if (it == g_interpDepthMaps.end()) {
        DepthEntry& fresh = g_interpDepthMaps[saveJmMethodOop];
        buildEntryForMethodOop(saveJmMethodOop, fresh);
        it = g_interpDepthMaps.find(saveJmMethodOop);
    }
    const DepthEntry& e = it->second;
    if (e.unmappable) return;

    Oop methodOop = Oop::fromRawBits(saveJmMethodOop);
    ObjectHeader* methObj = methodOop.asObjectPtr();
    Oop headerOop = methObj->slotAt(0);
    if (!headerOop.isSmallInteger()) return;
    size_t numLits = static_cast<size_t>(headerOop.asSmallInteger() & 0x7FFF);
    const uint8_t* bcStart = methObj->bytes() + (1 + numLits) * 8;

    g_saveChecks++;
    int64_t bcOff = saveIp - bcStart;
    int16_t d = -2;
    bool oor = bcOff < 0 || static_cast<size_t>(bcOff) >= e.depth.size();
    if (!oor) d = e.depth[bcOff];
    Oop* tb = static_cast<Oop*>(saveTempBase);
    Oop* sp = static_cast<Oop*>(saveSp);
    Oop* expected = (d >= 0) ? tb + tempCount + d + sendArgCount : nullptr;
    if (oor || d < 0 || sp != expected) {
        g_saveMismatches++;
        static int svReports = 0;
        if (++svReports <= 40) {
            fprintf(stderr,
                    "[SP-DEPTH-SAVE #%llu] %s %s sel=%s bcOff=%lld depth=%d "
                    "sendArgs=%d sp=%p expected=%p delta=%lld words "
                    "tempBase=%p tempCount=%d\n",
                    (unsigned long long)g_saveMismatches, where,
                    oor ? "IP-OUT-OF-RANGE" : (d < 0 ? "UNREACHABLE-OFFSET" : "SP"),
                    memory->selectorOf(methodOop).c_str(),
                    (long long)bcOff, (int)d, sendArgCount,
                    (void*)sp, (void*)expected,
                    expected ? (long long)(sp - expected) : 0,
                    (void*)tb, tempCount);
        }
    }
}

void bcDepthMapsClearAfterGC() {
    g_interpDepthMaps.clear();
}

void spDepthStatsDump() {
    if (!GET_DEBUG_BOOL(PHARO_SP_DEPTH_CHECK)) return;
    fprintf(stderr,
            "  sp-depth: checks=%llu mismatches=%llu (ip-oor=%llu unreach=%llu) "
            "maps=%llu unmappable=%llu interp: checks=%llu mismatches=%llu "
            "save: checks=%llu mismatches=%llu\n",
            (unsigned long long)g_checks, (unsigned long long)g_mismatches,
            (unsigned long long)g_ipOutOfRange,
            (unsigned long long)g_unreachableOffset,
            (unsigned long long)g_mapsBuilt, (unsigned long long)g_unmappable,
            (unsigned long long)g_interpChecks,
            (unsigned long long)g_interpMismatches,
            (unsigned long long)g_saveChecks,
            (unsigned long long)g_saveMismatches);
}

}  // namespace jit
}  // namespace pharo

#else  // !PHARO_JIT_ENABLED

namespace pharo { namespace jit {
struct JITState;
void spDepthCheck(JITState&, const char*) {}
void spDepthCheckInterpSend(uint64_t, const uint8_t*, void*, void*, int,
                            pharo::ObjectMemory*, const char*) {}
void spDepthCheckJ2JSave(uint64_t, const uint8_t*, void*, void*, int, int,
                         pharo::ObjectMemory*, const char*) {}
void bcDepthMapsClearAfterGC() {}
void spDepthStatsDump() {}
}}

#endif  // PHARO_JIT_ENABLED
