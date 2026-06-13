/*
 * AsmjitT1.cpp - Phase 2 of the asmjit-based Tier-1 JIT compiler.
 *
 * Per scripts/jit-diff/plan_asmjit_replacement.md.  See AsmjitT1.hpp.
 *
 * compileViaAsmjit() does:
 *   1. Pre-scan the method's bytecodes.  If every byte is in the
 *      Phase 2 supported set, emit real per-bytecode code.  Else
 *      fall back to the Phase 1 bail-on-entry trampoline (which
 *      makes the method run in the interpreter via ExitSend dispatch).
 *   2. Allocate a JITMethod sized for the emitted bytes; copy bytes
 *      into codeStart(); flushICache + makeExecutable; register in
 *      MethodMap.
 *
 * Supported in Phase 2 (no IC dispatch, no arithmetic, no control
 * flow):
 *
 *   pushReceiver, pushTemp(0..11), pushRecvVar(0..15),
 *   pushLitConst(0..31), pushTrue, pushFalse, pushNil,
 *   pushZero, pushOne, pop,
 *   returnReceiver, returnTrue, returnFalse, returnNil, returnTop
 *
 * Methods that contain anything else compile to the Phase 1 stub.
 *
 * Stack discipline matches the stencil JIT: state.sp points to the
 * next-free slot (one past TOS).  Push writes to *sp then sp++; pop
 * is sp--; returnTop reads *(--sp).
 */

#include "AsmjitT1.hpp"

#if PHARO_JIT_ENABLED

#include "../CodeZone.hpp"
#include "../JITState.hpp"
#include "../PlatformJIT.hpp"
#include "../SistaV1.hpp"
#include "../J2JSaveLayout.h"
#include "../../ObjectMemory.hpp"
#include "../../Interpreter.hpp"
#include "../../DebugSettings.hpp"
#include "../../DebugVars.hpp"
#include "../../ShadowSlots.hpp"

// Debug-only inline-getter write recorder for the aigraph investigation
// (PHARO_FINDNODE_WATCH).  Defined in Interpreter.cpp next to the tape; takes
// (state, recv, val) so it can read sp/tempBase and compute the write slot.
extern "C" void jit_rt_atrec_getter(uint64_t statep, uint64_t recv, uint64_t val,
                                    uint64_t bcOff);
extern "C" void jit_rt_atrec_entry(uint64_t statep);
extern "C" void jit_rt_verify_getter(uint64_t statep, uint64_t recv,
                                     uint64_t val, uint64_t extra,
                                     uint64_t entryPtr);
// Set true in compileViaAsmjit ONLY when compiling asTuple under
// FINDNODE_WATCH, so the recorder BLR is emitted at just asTuple's 2 getter
// sites (no global code bloat).  Read at emit time in the inline getter.
static bool g_emitGetterTrace = false;
// ===== sp-residency Phase-1 emit sweep (build-time switch) =====
// 0 = sp lives in memory at [x0, OFF_SP] (today's behavior; the
//     helpers below produce byte-identical codegen to the raw
//     ldr/str they replace).
// 1 = sp lives in x25 across JIT execution (the trampoline/JIT_CALL
//     contract half is ALREADY in: x25 is loaded live-in at every
//     JIT entry).  Flip ONLY when every OFF_SP access in BOTH the
//     send-site emit and emitOne_arm64 goes through these helpers
//     AND every EXIT_* site stores x25 back (see WIP.md: the
//     migration is all-or-nothing at runtime; this flag makes it
//     incremental at the SOURCE level — each converted batch commits
//     green with the flag at 0).
#define PHARO_T1_SP_IN_X25 1
// tempBase residency (x26) — same pattern, same contract sites.
#define PHARO_T1_TB_IN_X26 0
// simStack claims x26 as the TOS mirror (docs/simstack-design.md §4);
// the dead tempBase-residency experiment must never be re-enabled
// while the TOS cache code exists — mutually exclusive register use.
#if PHARO_T1_TB_IN_X26
#error "PHARO_T1_TB_IN_X26 conflicts with the simStack TOS cache (x26)"
#endif
// (the emitLoadSp/emitStoreSp/emitSyncSpToState helpers live next to
//  emitPushReg, after the asmjit headers are in scope)
// True while compiling a CompiledBlock (set per-compile in
// compileViaAsmjit, same single-threaded pattern as g_emitGetterTrace).
// Method-style returns 0x58-0x5C INSIDE a block are NON-LOCAL returns:
// the emit must bail to interp (like 0x5D/0x5E) so the home-context
// unwind runs.  The old emit treated 0x5C as a plain EXIT_RETURN,
// silently converting `^x` inside a hot block into a block-local
// return — Dictionary>>includes: (`self do: [:e | x = e ifTrue:
// [^true]]. ^false`) returned false on present elements once its do:
// block compiled (DictionaryTest>>testIncludes 2-test repro).
static bool g_emitIsBlock = false;
// FSR M1 per-compile gates (set in compileViaAsmjit).
static bool g_fsrX19 = false;
static bool g_fsrCursor = false;  // FSR M2 v1: x23 cursor residency (write-through)
static bool g_fsrCursorVerify = false;
static bool g_fsrNodepth = false;        // FSR M3
static bool g_fsrNodepthVerify = false;  // FSR M3 stage (a)
static bool g_fsrLazy = false;         // FSR M4
static int g_bcStartDelta = 0;         // (2+numLits)*8: methObj->bcStart delta (M4 exit-ip)
static bool g_fsrLazyVerify = false;   // FSR M4 stage (a)
static bool g_fsrX19Verify = false;
// V2 emit plumbing (set per-compile by emitMethodBytes, used by the
// send emit inside emitOne_arm64 — same single-threaded pattern):
static asmjit::Label g_codeStartLabel;
static std::vector<std::pair<uint32_t, asmjit::Label>>* g_resumeOverridesPtr
    = nullptr;
// PMS B1 (docs/patched-ic-design.md §9): per-site patch-word labels,
// recorded by the send emit, resolved to code offsets after flatten,
// written into the in-zone SendSitePatch map by the caller.
struct PatchSiteLabels {
    uint32_t siteIdx;
    asmjit::Label keyMovz;    // W0 (W1=+4, cmp=+8, b.ne=+12, W2=+16)
    asmjit::Label tail;       // T  (W3=+0, W4=+4, W5=+8); valid iff hasTail
    asmjit::Label tailBranch; // W6 (terminal direct b)
    bool hasTail = false;
};
static std::vector<PatchSiteLabels>* g_patchLabelsPtr = nullptr;
// simStack B0 (docs/simstack-design.md §3): write-through TOS cache —
// emit-time bookkeeping ONLY.  Memory is exact at every boundary by
// construction; x26 is a redundant mirror.  Producers set g_tos via
// paired helpers that emit the x26 write in the same call (B1+);
// consumers read g_tosIn (the snapshot of what the PREVIOUS bytecode
// left).  Unconverted ops are safe automatically: the dispatch head
// clears g_tos, so omissions lose optimization, never correctness.
struct T1TosCache {
    bool    valid    = false;  // x26 == value at [x25,#-8]
    bool    constSmI = false;  // B3: x26 is a known tagged SmI ...
    int64_t taggedBits = 0;    //     ... with these bits
};
static T1TosCache g_tos;     // running state (current bytecode writes)
static T1TosCache g_tosIn;   // snapshot consumed by the current bytecode
// Per-compile jump-target bitmap (prescan in emitMethodBytes): targets
// force g_tos = {} at label-BIND time.  emitOne's jump emits assert
// their target is marked; a mismatch hard-fails the compile (interp
// fallback) — never BUG-print-and-continue.
static const std::vector<bool>* g_tosJumpTargetsPtr = nullptr;
// Per-compile master switch (knob && real emit) + per-family mask bits
// (PHARO_T1_TOS_MASK; -1 = all).  Each converted family checks its bit
// so miscompiles bisect to a family within one binary.
static bool g_useTos = false;
static constexpr uint32_t kTosFamPush     = 1u << 0;
static constexpr uint32_t kTosFamPushImm  = 1u << 1;  // PushInteger/PushCharacter
static constexpr uint32_t kTosFamDup      = 1u << 2;
static constexpr uint32_t kTosFamCondJump = 1u << 3;
static constexpr uint32_t kTosFamRetTop   = 1u << 4;
static constexpr uint32_t kTosFamPopStore = 1u << 5;
static constexpr uint32_t kTosFamArith    = 1u << 6;
static constexpr uint32_t kTosFamSendHead = 1u << 7;  // 0-arg receiver feed
static constexpr uint32_t kTosFamSendRes  = 1u << 8;  // send result -> x26 (needs Lrearm)
static constexpr uint32_t kTosFamFuseCmpJ = 1u << 9;  // B4: cmp + b.cond fusion
// Bytecode window for fusion lookahead (indexed by globalIdx; set per
// compile next to the jump-target bitmap).
static const uint8_t* g_tosBc = nullptr;
static size_t g_tosBcLen = 0;
// Native back-edge yield poll target: &Interpreter::forceYield_ (set
// once by JITRuntime::initialize; session-stable singleton address).
extern "C" uint8_t* g_t1ForceYieldAddr;
uint8_t* g_t1ForceYieldAddr = nullptr;
static inline bool tosFam(uint32_t bit) {
    return g_useTos
        && ((uint32_t)GET_DEBUG_INT(PHARO_T1_TOS_MASK) & bit) != 0;
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#include <asmjit/x86.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <asmjit/a64.h>
#include "../SendSitePatcher.hpp"  // PMS: kImpossibleKeyHi16 (B1 emit)
#else
#error "Unsupported architecture for AsmjitT1"
#endif

#include <asmjit/core/codeholder.h>
#include <asmjit/core/logger.h>  // FileLogger (PHARO_ASMJIT_T1_LOG=1 dump)

namespace pharo {
namespace jit {

// Forward decl: inline block-value prep helper lives in JITRuntime.cpp.
// Called from the asmjit-T1 IC HIT emit when BLOCK_VALUE_BIT is set.
extern "C" uint64_t jit_rt_block_create(pharo::jit::JITState*, uint64_t);
extern "C" void jit_rt_prim_body_entry(pharo::jit::JITState*, uint64_t);
extern "C" void* jit_rt_inline_block_value_prep(
    void* state, int nArgs, void* resumeAddr);

// jit-may20b Step 10: inline-prim 18 (basicNew:) helper.  Returns 1
// on success (state.sp updated), 0 on failure (caller bails).
extern "C" uint64_t jit_rt_basic_new_with_arg(void* state);
extern "C" uint64_t jit_rt_basic_new(void* state);
extern "C" uint64_t g_primBasicNewZero_hits;
extern "C" uint64_t g_primBasicNewZero_bails;
extern "C" uint64_t jit_rt_t1_sista_dispatch(void* state, uint64_t fnPtr,
                                              uint64_t methodBits,
                                              uint64_t nArgs);
extern "C" void jit_rt_setter_write_barrier(void* state, uint64_t rcvBits,
                                             uint64_t valBits);
extern "C" void jit_rt_sync_globals(void* state);
extern "C" void jit_rt_trace_mod(int64_t a, int64_t b, int64_t result, void* state);
extern "C" void jit_rt_trace_idh(uint64_t recvBits, int64_t rawHash, void* state);
extern "C" void jit_rt_verify_inline_at(void* state, uint64_t rcvBits,
                                        uint64_t idxBits, uint64_t inlineVal);
extern "C" void jit_rt_check_setter_bounds(void* state, uint64_t rcvBits,
                                           uint64_t slotIdx, uint64_t valBits);
extern "C" int jit_rt_primsize_ptr(void* state, uint64_t rcvBits,
                                    uint64_t* out);
extern "C" int jit_rt_primat_ptr(void* state, uint64_t rcvBits,
                                  uint64_t i, uint64_t* out);
extern "C" int jit_rt_primatput_ptr(void* state, uint64_t rcvBits,
                                     uint64_t i, uint64_t valBits);
extern "C" uint64_t g_t1SistaDispatch_hits;
extern "C" uint64_t g_t1SistaDispatch_attempts;
extern "C" uint64_t g_t1MultiSlot_hits = 0;
extern "C" uint64_t g_t1MultiSlot_bails = 0;
extern "C" uint64_t g_t1ReturnsLiteral_hits = 0;
extern "C" uint64_t g_t1InlineGetter_hits = 0;  // diag: inline-getter (bit 63) fires
extern "C" uint64_t g_t1InlineSetter_hits = 0;  // diag: inline-setter (bit 62) fires
extern "C" uint64_t g_t1ReturnsSelf_hits  = 0;  // diag: returnsSelf  (bit 61) fires
// jit-may23d W1/W2/W3 inline-tier2 IC HIT counters.
extern "C" uint64_t g_t1TempReturn_hits     = 0;  // W1: `^ arg0`
extern "C" uint64_t g_t1IntCmpReturn_hits   = 0;  // W2: `^ self cmp arg`
extern "C" uint64_t g_t1IntArithReturn_hits = 0;  // W3: `^ self op arg`
extern "C" uint64_t g_t1EvenOdd_hits        = 0;  // W6: `^ (self bitAnd: 1) = 0`

// Counters for inline-prim 18 dispatch (PHARO_T1_INLINE_PRIM_COUNTERS=1).
extern "C" uint64_t g_primBasicNew_hits  = 0;
extern "C" uint64_t g_primBasicNew_bails = 0;

// Block-value inline counters (PHARO_T1_INLINE_BLOCK_VALUE=1 telemetry).
extern "C" uint64_t g_blockValue_tries = 0;  // BLOCK_VALUE_BIT detected
extern "C" uint64_t g_blockValue_hits  = 0;  // helper returned entry
extern "C" uint64_t g_blockValue_bails = 0;  // helper returned NULL

// Inline-J2J bail-reason counters (PHARO_T1_INLINE_J2J=1 instrumentation).
// Incremented from JIT-emitted code via address-load + atomic-ish increment;
// printed by JITRuntime stats.  Process-global (single VM per process).
extern "C" uint64_t g_inlineJ2J_hits      = 0;  // path taken (br to entry)
extern "C" uint64_t g_inlineJ2J_bail_zero = 0;  // entryAddr == 0
extern "C" uint64_t g_inlineJ2J_bail_full = 0;  // save stack at limit
extern "C" uint64_t g_inlineJ2J_bail_self = 0;  // calleeJM != callerJM (or other bail2 routes)
extern "C" uint64_t g_inlineJ2J_bail_gate = 0;  // pure-J2J gate failed (a caller IC site lacks bit 60)
// Counters for inline-prim path firings (PHARO_T1_INLINE_PRIM_COUNTERS=1).
extern "C" uint64_t g_primAt_hits         = 0;  // tryPrimAt inline fired
extern "C" uint64_t g_primAtPut_hits      = 0;  // tryPrimAtPut inline fired
extern "C" uint64_t g_primSize_hits       = 0;  // tryPrimSize inline fired
extern "C" uint64_t g_primClass_hits      = 0;  // tryPrimClass inline fired
extern "C" uint64_t g_primBitOp_hits      = 0;  // bitAnd/bitOr/bitXor fired
extern "C" uint64_t g_primFloatOp_hits    = 0;  // SmallFloat send-site fired
extern "C" uint64_t g_bcFloatArith_hits   = 0;  // 0x60/0x61 SmallFloat bytecode fired
extern "C" uint64_t g_bcArithBail_hits    = 0;  // 0x60/0x61 SmI fast-path bailed
extern "C" uint64_t g_bcRemoteTemp_hits   = 0;  // 0xFB/0xFC/0xFD inline fired
// Debug: last-seen values at the self-recursive check.  Overwritten each
// entry so post-run we can see what the comparison was checking.
extern "C" uint64_t g_inlineJ2J_dbg_caller_method = 0;
extern "C" uint64_t g_inlineJ2J_dbg_callee_method = 0;
extern "C" uint64_t g_inlineJ2J_dbg_extra         = 0;

// PHARO_T1_LOG_SELFREC_PUSH: ring of caller(=callee) CompiledMethod oops at
// each self-rec inline-J2J save-push, to NAME the method whose save/return
// desyncs.  Resolved to selectors at the #extent DNU (see Interpreter.cpp).
extern "C" uint64_t g_selfRecPushRing[256] = {0};
extern "C" uint32_t g_selfRecPushIdx = 0;
extern "C" void jit_rt_log_selfrec_push(pharo::jit::JITState* s) {
    if (!s) return;
    g_selfRecPushRing[g_selfRecPushIdx & 255] = s->method.rawBits();
    g_selfRecPushIdx++;
}
// Cross-method bisection counter + limit.  At runtime, bail when
// g_xmethod_count > g_xmethod_max.  Limit set from
// PHARO_T1_INLINE_J2J_XMETHOD_MAX env var (default UINT64_MAX = no limit).
extern "C" uint64_t g_xmethod_count = 0;
// Monotonic T1 compile counter for the saveless MIN_COMPILE bisect gate.
extern "C" uint64_t g_t1CompileSeq2;
extern "C" int g_ibcEmits;
extern "C" bool g_emitPrologueLeaf;
// Eδ.2a (2026-05-24): count of methods compiled with canSkipJ2JSave=true.
// Bumped at AsmjitT1.cpp compileMethod when the flag is set on a real
// (non-stub) method.  Read by JIT stats dump.
extern "C" uint64_t g_canSkipJ2JSave_count = 0;
extern "C" uint64_t g_canSkipJ2JSave_total = 0;
// Eδ.2b (2026-05-24): IC-HIT runtime hits where the callee's
// canSkipJ2JSave flag is set.  Bumped from JIT-emitted code at every
// inline-J2J IC HIT after computing calleeJM.  Compared with
// g_inlineJ2J_hits to gauge the optimization's potential coverage.
extern "C" uint64_t g_canSkipJ2JSave_ic_hits = 0;
// Eδ.2c (2026-05-24): saveless-J2J fires.  Bumped each time the
// PHARO_T1_CAN_SKIP_J2J_SAVE emit fires the blr-based call path
// (callee qualified AND IC hit reached the saveless emit).
extern "C" uint64_t g_canSkipJ2JSave_fires = 0;
extern "C" uint64_t g_xmethod_max   = UINT64_MAX;

// Store-provenance ring (PHARO_T1_STORE_RING).  Every JIT receiver-ivar
// store logs (receiver, value, slot, callerJM) here; at DNU time the
// interpreter scans the ring for the corrupted oop (as stored VALUE or
// as store TARGET) — catches the corrupting WRITE instead of its
// downstream symptom.  Built for the J2J ivar-store corruption hunt
// (WIP.md 2026-06-10): the symptom is a layout knife-edge, so the
// detector must work on whatever layout it runs in.
struct StoreRingEntry {
    uint64_t recv;
    uint64_t value;
    uint64_t jm;       // JITMethod* of the storing method
    uint64_t slot;
};
static constexpr size_t kStoreRingSize = 1 << 16;  // 64K entries, 2MB
extern "C" StoreRingEntry g_storeRing[kStoreRingSize];
StoreRingEntry g_storeRing[kStoreRingSize] = {};
extern "C" uint64_t g_storeRingIdx = 0;

extern "C" void jit_rt_store_ring(uint64_t state, uint64_t recv,
                                  uint64_t value, uint64_t slot,
                                  uint64_t jm) {
    (void)state;
    if (GET_DEBUG_BOOL(PHARO_T1_STORE_RING)) {
        StoreRingEntry& e =
            g_storeRing[g_storeRingIdx++ & (kStoreRingSize - 1)];
        e.recv = recv;
        e.value = value;
        e.jm = jm;
        e.slot = slot;
    }
    if (GET_DEBUG_BOOL(PHARO_SHADOW_SLOTS)) {
        pharo::shadowStore(recv, slot, value, jm);
    }
}

// Shadow-slot verify for JIT ivar reads.  Receiver read from the state
// (OFF_RECEIVER) so the emit hook only needs the value + slot.
extern "C" void jit_rt_shadow_verify(uint64_t state, uint64_t value,
                                     uint64_t slot) {
    uint64_t recv = *reinterpret_cast<uint64_t*>(
        reinterpret_cast<uint8_t*>(state) + 8);  // OFF_RECEIVER
    pharo::shadowVerify(recv, slot, value, "jit-pushRecvVar");
}

// Emit the knob-gated verify call after an ivar-read emit.  Convention:
// x1 = just-loaded value; slot is compile-time.  Full caller-saved
// save/restore (the 928df628 lesson).
static void emitShadowReadVerify(asmjit::a64::Assembler& a, int slotIdx) {
    using namespace asmjit::a64;
    if (!GET_DEBUG_BOOL(PHARO_SHADOW_SLOTS)) return;
    a.sub(sp, sp, asmjit::Imm(144));
    a.stp(x0, x1,   ptr(sp, 0));
    a.stp(x2, x3,   ptr(sp, 16));
    a.stp(x4, x5,   ptr(sp, 32));
    a.stp(x6, x7,   ptr(sp, 48));
    a.stp(x8, x9,   ptr(sp, 64));
    a.stp(x10, x11, ptr(sp, 80));
    a.stp(x12, x13, ptr(sp, 96));
    a.stp(x14, x15, ptr(sp, 112));
    a.str(x30,      ptr(sp, 128));
    a.mov(x2, asmjit::Imm(slotIdx));        // slot (x1 = value already)
    a.mov(x16, asmjit::Imm((uint64_t)&jit_rt_shadow_verify));
    a.blr(x16);
    a.ldp(x0, x1,   ptr(sp, 0));
    a.ldp(x2, x3,   ptr(sp, 16));
    a.ldp(x4, x5,   ptr(sp, 32));
    a.ldp(x6, x7,   ptr(sp, 48));
    a.ldp(x8, x9,   ptr(sp, 64));
    a.ldp(x10, x11, ptr(sp, 80));
    a.ldp(x12, x13, ptr(sp, 96));
    a.ldp(x14, x15, ptr(sp, 112));
    a.ldr(x30,      ptr(sp, 128));
    a.add(sp, sp, asmjit::Imm(144));
}

// Emit a knob-gated call to jit_rt_store_ring at a receiver-ivar store
// site.  Convention at the call point: x4 = receiver, x1 = value (both
// store emits use these).  Saves the FULL caller-saved set across the
// blr (the XMETHOD_LOG x1-x6 clobber lesson, 928df628).
static void emitStoreRingLog(asmjit::a64::Assembler& a, int slotIdx) {
    using namespace asmjit::a64;
    if (!GET_DEBUG_BOOL(PHARO_T1_STORE_RING)
            && !GET_DEBUG_BOOL(PHARO_SHADOW_SLOTS)) return;
    a.sub(sp, sp, asmjit::Imm(144));
    a.stp(x0, x1,   ptr(sp, 0));
    a.stp(x2, x3,   ptr(sp, 16));
    a.stp(x4, x5,   ptr(sp, 32));
    a.stp(x6, x7,   ptr(sp, 48));
    a.stp(x8, x9,   ptr(sp, 64));
    a.stp(x10, x11, ptr(sp, 80));
    a.stp(x12, x13, ptr(sp, 96));
    a.stp(x14, x15, ptr(sp, 112));
    a.str(x30,      ptr(sp, 128));
    a.mov(x2, x1);                          // value
    a.mov(x1, x4);                          // receiver
    a.mov(x3, asmjit::Imm(slotIdx));
    a.ldr(x4, ptr(x0, 56));                 // state.jitMethod (OFF_JITMETHOD)
    a.mov(x16, asmjit::Imm((uint64_t)&jit_rt_store_ring));
    a.blr(x16);
    a.ldp(x0, x1,   ptr(sp, 0));
    a.ldp(x2, x3,   ptr(sp, 16));
    a.ldp(x4, x5,   ptr(sp, 32));
    a.ldp(x6, x7,   ptr(sp, 48));
    a.ldp(x8, x9,   ptr(sp, 64));
    a.ldp(x10, x11, ptr(sp, 80));
    a.ldp(x12, x13, ptr(sp, 96));
    a.ldp(x14, x15, ptr(sp, 112));
    a.ldr(x30,      ptr(sp, 128));
    a.add(sp, sp, asmjit::Imm(144));
}
// Per-gate bail counters for the cross-method (xmethod) inline-J2J
// gate chain.  Only bumped when PHARO_T1_INLINE_J2J is set (the
// inlineJ2JCounters emit gate), same as the other bail counters.
// g_xgate_enter counts entries into the gate chain (= bit-60 IC hits
// whose extra lacks SELF_REC_BIT).
extern "C" uint64_t g_xgate_enter      = 0;
extern "C" uint64_t g_xgate_bail_prim  = 0;  // methodHeader bit 16 (hasPrim)
extern "C" uint64_t g_xgate_bail_numic = 0;  // numICEntries != 0
extern "C" uint64_t g_xgate_bail_b47   = 0;  // JM byte 47 != 0
extern "C" uint64_t g_xgate_bail_b46   = 0;  // JM byte 46 != 0
extern "C" uint64_t g_xgate_bail_cap   = 0;  // count > g_xmethod_max

// Compact xmethod trace.  Stores per-fire data in a buffer, prints
// at process exit via atexit registration.  Avoids in-loop fprintf
// (which perturbs timing such that subsequent fires don't happen).
struct XMethodTrace {
    uint64_t calleeCM;
    uint64_t callerCM;
    uint64_t stateMethod;
    uint64_t stateJitMethod;
    uint64_t stateReceiver;
    uint64_t stateSp;
    uint64_t stateTempBase;
    uint64_t stateIp;
    int32_t  stateJ2JDepth;
    int32_t  stateArgCount;
};
static constexpr size_t kXMethodTraceMax = 64;
extern "C" XMethodTrace g_xmethod_trace[kXMethodTraceMax];
XMethodTrace g_xmethod_trace[kXMethodTraceMax] = {};
extern "C" size_t g_xmethod_trace_count;
size_t g_xmethod_trace_count = 0;

static const int xmethod_atexit_install = []() {
    if (pharo::g_debug.t1InlineJ2JXmethodLog) {
        std::atexit([]() {
            extern void jit_rt_xmethod_dump_trace_extern();
            jit_rt_xmethod_dump_trace_extern();
        });
    }
    return 0;
}();

extern "C" void jit_rt_xmethod_dump_trace();

void jit_rt_xmethod_dump_trace_extern() {
    fprintf(stderr, "[XMETHOD-ATEXIT] called, trace_count=%zu\n",
            g_xmethod_trace_count);
    fflush(stderr);
    jit_rt_xmethod_dump_trace();
}

extern "C" void jit_rt_xmethod_dump_trace() {
    fprintf(stderr, "[XMETHOD-TRACE] %zu fires captured\n",
            std::min(g_xmethod_trace_count, kXMethodTraceMax));
    fflush(stderr);
    size_t n = std::min(g_xmethod_trace_count, kXMethodTraceMax);
    for (size_t i = 0; i < n; i++) {
        XMethodTrace& t = g_xmethod_trace[i];
        fprintf(stderr,
            "  #%zu calleeCM=0x%llx callerCM=0x%llx\n"
            "      state.method=0x%llx state.jitMethod=0x%llx\n"
            "      state.receiver=0x%llx state.sp=0x%llx state.tempBase=0x%llx\n"
            "      state.ip=0x%llx state.j2jDepth=%d state.argCount=%d\n",
            i + 1,
            (unsigned long long)t.calleeCM, (unsigned long long)t.callerCM,
            (unsigned long long)t.stateMethod, (unsigned long long)t.stateJitMethod,
            (unsigned long long)t.stateReceiver, (unsigned long long)t.stateSp,
            (unsigned long long)t.stateTempBase, (unsigned long long)t.stateIp,
            t.stateJ2JDepth, t.stateArgCount);
    }
    fflush(stderr);
}

// Helper called from cross-method emit (PHARO_T1_INLINE_J2J_XMETHOD_LOG=1).
// Updated 2026-05-18: writes to in-memory trace buffer instead of
// fprintf, so subsequent fires aren't suppressed by timing
// perturbation.  Call jit_rt_xmethod_dump_trace() to print.
extern "C" uint64_t jit_rt_xmethod_log(uint64_t state, uint64_t calleeJM,
                                       uint64_t callerJM, uint64_t calleeCM,
                                       uint64_t callerCM) {
    // Circular buffer: ring of size kXMethodTraceMax, captures the
    // most recent fires (most useful for post-corruption diagnosis).
    static size_t logN = 0;
    size_t slot = logN % kXMethodTraceMax;
    logN++;
    g_xmethod_trace_count = logN;
    // Skip fprintf for fires past the first buffer fill — only buffer.
    // Fast-path-friendly.
    {
        XMethodTrace& t = g_xmethod_trace[slot];
        uint64_t* s = (uint64_t*)state;
        t.calleeCM = calleeCM;
        t.callerCM = callerCM;
        t.stateMethod    = s[64/8];  // state.method @ 64
        t.stateJitMethod = s[56/8];  // state.jitMethod @ 56
        t.stateReceiver  = s[8/8];   // state.receiver @ 8
        t.stateSp        = s[0];     // state.sp @ 0
        t.stateTempBase  = s[24/8];  // state.tempBase @ 24
        t.stateIp        = s[48/8];  // state.ip @ 48
        t.stateJ2JDepth  = *(int32_t*)((uint8_t*)state + 160);
        t.stateArgCount  = *(int32_t*)((uint8_t*)state + 72);
        // Look up selector names for both methods.
        // state.interp is at offset 40 from state.
        Interpreter* interp = *(Interpreter**)((uint8_t*)state + 40);
        ObjectMemory* mem = *(ObjectMemory**)((uint8_t*)state + 32);
        std::string calleeSel = "?";
        std::string callerSel = "?";
        int calleePrim = -1;
        int calleeNumLits = -1;
        bool calleeHasPrim = false;
        if (interp) {
            Oop calleeOop = Oop::fromRawBits(calleeCM);
            Oop callerOop = Oop::fromRawBits(callerCM);
            calleeSel = interp->memory().selectorOf(calleeOop);
            callerSel = interp->memory().selectorOf(callerOop);
            if (calleeOop.isObject()) {
                ObjectHeader* mo = calleeOop.asObjectPtr();
                Oop hdr = mo->slotAt(0);
                if (hdr.isSmallInteger()) {
                    int64_t hb = hdr.asSmallInteger();
                    calleeNumLits = (int)(hb & 0x7FFF);
                    calleeHasPrim = ((hb >> 16) & 1) != 0;
                    if (calleeHasPrim) {
                        const uint8_t* bc = mo->bytes()
                            + (1 + calleeNumLits) * 8;
                        if (bc[0] == 0xF8) {
                            calleePrim = bc[1] | ((bc[2] & 0x1F) << 8);
                        }
                    }
                }
            }
        }
        (void)mem;
        // Dump first 16 bytes of callee bytecodes
        char bcStr[80] = "";
        uint64_t calleeJMMethodHeader = 0;
        uint8_t calleeJMArgCount = 0;
        uint8_t calleeJMTempCount = 0;
        if (interp && calleeNumLits >= 0) {
            Oop calleeOop = Oop::fromRawBits(calleeCM);
            ObjectHeader* mo = calleeOop.asObjectPtr();
            const uint8_t* bc = mo->bytes() + (1 + calleeNumLits) * 8;
            for (int k = 0; k < 12; k++) {
                snprintf(bcStr + k*3, sizeof(bcStr) - k*3, "%02x ", bc[k]);
            }
            // Inspect JM struct (offset 16 = methodHeader,
            //                    offset 34 = argCount, offset 35 = tempCount)
            const uint8_t* jmBytes = (const uint8_t*)calleeJM;
            calleeJMMethodHeader = *(const uint64_t*)(jmBytes + 16);
            calleeJMArgCount = jmBytes[34];
            calleeJMTempCount = jmBytes[35];
        }
        // Per-fire fprintf only for the first 8 fires (diagnostic);
        // subsequent fires only update the ring buffer.
        if (logN <= 8) {
            fprintf(stderr, "[XLOG #%zu] callee=#%s (cm=0x%llx prim=%d hasPrim=%d numLits=%d) caller=#%s (cm=0x%llx) bc[0..11]: %s jmMH=0x%llx jmArgC=%d jmTempC=%d\n",
                logN, calleeSel.c_str(), (unsigned long long)calleeCM,
                calleePrim, (int)calleeHasPrim, calleeNumLits,
                callerSel.c_str(), (unsigned long long)callerCM, bcStr,
                (unsigned long long)calleeJMMethodHeader,
                calleeJMArgCount, calleeJMTempCount);
            fflush(stderr);
        }
        // Optional per-fire dump (PHARO_T1_INLINE_J2J_XMETHOD_LIVE=1).
        // Default off — the trace buffer is the primary capture.
        if (pharo::g_debug.t1InlineJ2JXmethodLive) {
            fprintf(stderr,
                "[XLOG #%zu] calleeCM=0x%llx callerCM=0x%llx "
                "method=0x%llx jm=0x%llx rcv=0x%llx sp=0x%llx tb=0x%llx "
                "ip=0x%llx j2jDepth=%d argCount=%d\n",
                logN, (unsigned long long)calleeCM,
                (unsigned long long)callerCM,
                (unsigned long long)t.stateMethod, (unsigned long long)t.stateJitMethod,
                (unsigned long long)t.stateReceiver, (unsigned long long)t.stateSp,
                (unsigned long long)t.stateTempBase, (unsigned long long)t.stateIp,
                t.stateJ2JDepth, t.stateArgCount);
            fflush(stderr);
        }
    }
    return 1;
}

// Old verbose log helper (kept for reference; unused).
[[maybe_unused]] static uint64_t jit_rt_xmethod_log_old(uint64_t state, uint64_t calleeJM,
                                        uint64_t callerJM, uint64_t calleeCM,
                                        uint64_t callerCM) {
    static size_t logN = 0;
    if (logN < 5) {
        logN++;
        uint64_t* s = (uint64_t*)state;
        // Decode method header to get numLits/argCount/tempCount.
        // methodHeader = SmI bits at methodObj.slot[0] (heap offset 8).
        uint64_t calleeMH = *(uint64_t*)(calleeCM + 8);
        uint64_t callerMH = *(uint64_t*)(callerCM + 8);
        int calleeNumLits = (int)((calleeMH >> 3) & 0x7FFF);  // SmI: shift 3
        int callerNumLits = (int)((callerMH >> 3) & 0x7FFF);
        int calleeArgCount = (int)((calleeMH >> (3+15)) & 0x1F);
        int callerArgCount = (int)((callerMH >> (3+15)) & 0x1F);
        int calleeTempCount = (int)((calleeMH >> (3+15+5)) & 0x3F);
        int callerTempCount = (int)((callerMH >> (3+15+5)) & 0x3F);
        // First bytecode of callee
        uint8_t* calleeBC = (uint8_t*)(calleeCM + 8 + (1 + calleeNumLits) * 8);
        fprintf(stderr, "[XMETHOD #%zu] state=%p\n"
                        "  callee CM=0x%llx JM=0x%llx numLits=%d argCount=%d tempCount=%d\n"
                        "    bc[0..7]: %02x %02x %02x %02x %02x %02x %02x %02x\n"
                        "  caller CM=0x%llx JM=0x%llx numLits=%d argCount=%d tempCount=%d\n"
                        "  state.sp=0x%llx state.receiver=0x%llx state.literals=0x%llx\n"
                        "  state.tempBase=0x%llx state.ip=0x%llx state.jitMethod=0x%llx\n"
                        "  state.method=0x%llx state.argCount=%llu\n",
                logN, (void*)state,
                (unsigned long long)calleeCM, (unsigned long long)calleeJM,
                calleeNumLits, calleeArgCount, calleeTempCount,
                calleeBC[0], calleeBC[1], calleeBC[2], calleeBC[3],
                calleeBC[4], calleeBC[5], calleeBC[6], calleeBC[7],
                (unsigned long long)callerCM, (unsigned long long)callerJM,
                callerNumLits, callerArgCount, callerTempCount,
                (unsigned long long)s[0], (unsigned long long)s[1],
                (unsigned long long)s[2], (unsigned long long)s[3],
                (unsigned long long)s[6], (unsigned long long)s[7],
                (unsigned long long)s[8], (unsigned long long)s[9]);
    }
    return 1;
}
extern "C" uint64_t g_inlineJ2J_dbg_ic_hits       = 0;  // count IC HIT events
extern "C" uint64_t g_inlineJ2J_dbg_extra_no_bit60 = 0; // IC HIT with extra but no bit 60
extern "C" uint64_t g_inlineJ2J_dbg_miss          = 0;  // count IC MISS events
extern "C" uint64_t g_inlineJ2J_dbg_dispatch      = 0;  // count dispatchCached events

// jit-may20b Step 6: per-caller histogram of inline-J2J gate pass/bail.
// JIT-emit calls jit_rt_bail_gate_log(callerJM, kind) when
// PHARO_T1_BAIL_GATE_HISTO=1 (or implicitly under PHARO_T1_BAIL_GATE_TRACE=1).
// The helper updates two maps keyed on JITMethod*; jit_rt_bail_gate_dump
// prints top-20 by count for each side at VM exit.  Kind: 0=bail, 1=pass.
struct BailGateStats {
    std::unordered_map<JITMethod*, uint64_t> bailCount;
    std::unordered_map<JITMethod*, uint64_t> passCount;
    std::unordered_set<JITMethod*> traced;
};
static BailGateStats g_bailGateStats;

extern "C" void jit_rt_bail_gate_log(JITMethod* callerJM, uint64_t kind) {
    if (kind == 0) {
        g_bailGateStats.bailCount[callerJM]++;
        if (g_debug.t1BailGateTrace
                && g_bailGateStats.traced.insert(callerJM).second) {
            uint64_t selBits = callerJM->selectorOop;
            fprintf(stderr,
                "[BAIL-GATE-TRACE] jm=%p selOop=0x%llx numICEntries=%u "
                "icBuffer=%p\n",
                (void*)callerJM, (unsigned long long)selBits,
                callerJM->numICEntries, (void*)callerJM->icBuffer);
            if (callerJM->icBuffer && callerJM->numICEntries > 0) {
                for (uint32_t s = 0; s < callerJM->numICEntries; s++) {
                    uint8_t* siteBase =
                        reinterpret_cast<uint8_t*>(callerJM->icBuffer)
                        + s * IC_BYTES_PER_SITE;
                    for (uint32_t e = 0; e < IC_ENTRIES_PER_SITE; e++) {
                        uint64_t* slots =
                            reinterpret_cast<uint64_t*>(siteBase) + e * 3;
                        fprintf(stderr,
                            "  site=%u entry=%u key=0x%llx method=0x%llx "
                            "extras=0x%llx\n",
                            s, e,
                            (unsigned long long)slots[0],
                            (unsigned long long)slots[1],
                            (unsigned long long)slots[2]);
                    }
                }
            }
            fflush(stderr);
        }
    } else {
        g_bailGateStats.passCount[callerJM]++;
    }
}

// Helper: dump the icBuffer state of a single JITMethod.  Empty sites
// are summarized with a one-line `<empty>` entry.
static void dumpJMICBuffer(JITMethod* jm, ObjectMemory& mem,
                           const char* tag, uint64_t count) {
    Oop cmOop = Oop::fromRawBits(jm->compiledMethodOop);
    std::string sel = mem.selectorOf(cmOop);
    fprintf(stderr,
        "[BAIL-GATE-%s] #%s jm=%p tier=%u numICEntries=%u count=%llu\n",
        tag, sel.c_str(), (void*)jm, (unsigned)jm->tier,
        (unsigned)jm->numICEntries, (unsigned long long)count);
    if (!jm->icBuffer) return;
    for (uint32_t s = 0; s < jm->numICEntries; s++) {
        uint8_t* siteBase = reinterpret_cast<uint8_t*>(jm->icBuffer)
            + s * IC_BYTES_PER_SITE;
        bool anyEntry = false;
        for (uint32_t e = 0; e < IC_ENTRIES_PER_SITE; e++) {
            uint64_t* slots =
                reinterpret_cast<uint64_t*>(siteBase) + e * 3;
            if (slots[0] == 0 && slots[1] == 0 && slots[2] == 0) continue;
            anyEntry = true;
            fprintf(stderr,
                "  site=%u entry=%u key=0x%llx method=0x%llx extras=0x%llx\n",
                s, e,
                (unsigned long long)slots[0],
                (unsigned long long)slots[1],
                (unsigned long long)slots[2]);
        }
        if (!anyEntry) {
            uint64_t* slots = reinterpret_cast<uint64_t*>(siteBase);
            fprintf(stderr,
                "  site=%u <empty> selBits=0x%llx hitCount=%llu\n",
                s,
                (unsigned long long)slots[IC_SELBITS_SLOT],
                (unsigned long long)slots[IC_HITCOUNT_SLOT]);
        }
    }
}

// Walk the code-zone method list and dump the icBuffer for every method
// whose selector matches one of `names`.  Used when the gate is disabled
// (no bail counters fire) so we can still see whether each method's IC
// sites ever warmed up.  Public so Interpreter can call it.
void dumpBailGateNamedICs(ObjectMemory& mem, JITRuntime& rt,
                          std::initializer_list<const char*> names) {
    CodeZone& zone = rt.codeZone();
    for (JITMethod* m = zone.firstMethod(); m; m = m->nextInZone) {
        Oop cmOop = Oop::fromRawBits(m->compiledMethodOop);
        std::string sel = mem.selectorOf(cmOop);
        for (const char* n : names) {
            if (sel == n) {
                dumpJMICBuffer(m, mem, "NAMED", 0);
                break;
            }
        }
    }
}

// Print top-20 bail / pass callers.  Called from Interpreter::dumpJITStats.
void dumpBailGateHisto(ObjectMemory& mem) {
    // First: dump the final icBuffer state of the top-5 bail callers so we
    // can see whether their cold sites ever warmed up after warmup phase.
    {
        std::vector<std::pair<uint64_t, JITMethod*>> bail;
        bail.reserve(g_bailGateStats.bailCount.size());
        for (auto& p : g_bailGateStats.bailCount)
            bail.emplace_back(p.second, p.first);
        std::sort(bail.rbegin(), bail.rend());
        size_t n = std::min<size_t>(5, bail.size());
        for (size_t i = 0; i < n; i++) {
            dumpJMICBuffer(bail[i].second, mem, "EXIT", bail[i].first);
        }
    }
    auto top = [&](const std::unordered_map<JITMethod*, uint64_t>& m,
                   const char* tag) {
        if (m.empty()) return;
        std::vector<std::pair<uint64_t, JITMethod*>> v;
        v.reserve(m.size());
        for (auto& p : m) v.emplace_back(p.second, p.first);
        std::sort(v.rbegin(), v.rend());
        uint64_t total = 0;
        for (auto& e : v) total += e.first;
        fprintf(stderr,
            "  bail-gate %s histogram (%zu callers, %llu total events):\n",
            tag, v.size(), (unsigned long long)total);
        size_t limit = std::min<size_t>(20, v.size());
        for (size_t i = 0; i < limit; i++) {
            JITMethod* jm = v[i].second;
            Oop cmOop = Oop::fromRawBits(jm->compiledMethodOop);
            std::string sel = mem.selectorOf(cmOop);
            fprintf(stderr,
                "    %p #%-32s tier=%u nIC=%u count=%llu\n",
                (void*)jm, sel.c_str(), (unsigned)jm->tier,
                (unsigned)jm->numICEntries,
                (unsigned long long)v[i].first);
        }
    };
    top(g_bailGateStats.bailCount, "BAIL");
    top(g_bailGateStats.passCount, "PASS");
    // Pretty-print combined for each method that appears in either map.
    if (!g_bailGateStats.bailCount.empty()
            && !g_bailGateStats.passCount.empty()) {
        std::unordered_map<JITMethod*, std::pair<uint64_t,uint64_t>> combo;
        for (auto& p : g_bailGateStats.bailCount) combo[p.first].first = p.second;
        for (auto& p : g_bailGateStats.passCount) combo[p.first].second = p.second;
        std::vector<std::tuple<uint64_t, uint64_t, JITMethod*>> v;
        v.reserve(combo.size());
        for (auto& p : combo)
            v.emplace_back(p.second.first + p.second.second,
                           p.second.first, p.first);
        std::sort(v.rbegin(), v.rend());
        fprintf(stderr, "  bail-gate combined top-20 (by bail+pass total):\n");
        size_t limit = std::min<size_t>(20, v.size());
        for (size_t i = 0; i < limit; i++) {
            JITMethod* jm = std::get<2>(v[i]);
            uint64_t b = combo[jm].first;
            uint64_t p = combo[jm].second;
            uint64_t t = b + p;
            Oop cmOop = Oop::fromRawBits(jm->compiledMethodOop);
            std::string sel = mem.selectorOf(cmOop);
            double bailPct = t > 0 ? 100.0 * b / t : 0.0;
            fprintf(stderr,
                "    %p #%-32s nIC=%u bail=%llu pass=%llu "
                "(bail %.1f%%)\n",
                (void*)jm, sel.c_str(),
                (unsigned)jm->numICEntries,
                (unsigned long long)b, (unsigned long long)p, bailPct);
        }
    }
    fflush(stderr);
}

namespace {

// JITState field offsets — guarded by static_assert in JITState.hpp.
constexpr int OFF_SP             = 0;
constexpr int OFF_RECEIVER       = 8;
constexpr int OFF_LITERALS       = 16;
constexpr int OFF_TEMPBASE       = 24;
constexpr int OFF_IP             = 48;
constexpr int OFF_JITMETHOD      = 56;
constexpr int OFF_METHOD         = 64;
constexpr int OFF_ARGCOUNT       = 72;
constexpr int OFF_EXIT           = 76;
constexpr int OFF_RETVAL         = 80;
constexpr int OFF_CACHED_TARGET  = 88;
constexpr int OFF_ICDATAPTR      = 96;
constexpr int OFF_SENDARGCOUNT   = 104;
constexpr int OFF_TRUEOOP        = 128;
constexpr int OFF_FALSEOOP       = 136;
constexpr int OFF_J2J_SAVE_CURSOR = 144;
constexpr int OFF_J2J_SAVE_LIMIT  = 152;
constexpr int OFF_J2J_DEPTH       = 160;
constexpr int OFF_J2J_TOTAL_CALLS = 164;
constexpr int OFF_J2J_ENTRY_DEPTH = 200;
constexpr int OFF_J2J_ENTRY_CURSOR = 312;  // FSR M3 (JITState.j2jEntryCursor; static_asserted there)
[[maybe_unused]] constexpr int OFF_J2J_DEPTH_INC = 208;
// Retro-save graceful pool-full handoff (cascade #2).
constexpr int OFF_RETRO_SP        = 272;
constexpr int OFF_RETRO_RECV      = 280;
constexpr int OFF_RETRO_TEMPBASE  = 288;
constexpr int OFF_RETRO_RESUME    = 296;
constexpr int OFF_RETRO_ORIG_EXIT = 304;

// ExitReason values (JITState.hpp).
constexpr int EXIT_RETURN          = 1;
constexpr int EXIT_SEND            = 2;
constexpr int EXIT_ARITH_OVERFLOW  = 6;
constexpr int EXIT_SEND_CACHED     = 7;
constexpr int EXIT_BLOCK_CREATE    = 8;
constexpr int EXIT_ARRAY_CREATE    = 9;
constexpr int EXIT_MUST_BOOL       = 12;
constexpr int EXIT_RETRO_FULL      = 13;

// nArgs per special selector 0x70..0x7F (index = op - 0x70).
//   at: at:put: size next nextPut: atEnd == class ~~ value value: do: new new: x y
constexpr uint8_t kSpecialNArgs[16] =
    {1,2,0,0,1,0,1,0,1,0,1,1,0,1,0,0};
inline int sendNArgs(uint8_t op) {
    if (op >= 0x60 && op <= 0x6F) return 1;  // binary special selectors
    if (op <= 0x7F) return kSpecialNArgs[op - 0x70];
    if (op <= 0x8F) return 0;   // literal send 0 args
    if (op <= 0x9F) return 1;   // literal send 1 arg
    return 2;                   // 0xA0..0xAF: literal send 2 args
}

// Oop tag for SmallInteger: low 3 bits = 001.
//   fromSmallInteger(N) = (N << 3) | 1
constexpr uint64_t SMI_TAG = 0x1;
constexpr uint64_t smiBits(int64_t n) {
    return (static_cast<uint64_t>(n) << 3) | SMI_TAG;
}

// Pharo ObjectHeader is 8 bytes; slot N starts at byte 8 + N*8.
constexpr int OBJ_SLOT_0 = 8;

// Stats.
size_t g_compiled       = 0;   // total compileViaAsmjit successes
size_t g_compiledReal   = 0;   // of which actually emitted real code
size_t g_compiledStub   = 0;   // of which used the bail-on-entry stub
size_t g_failed         = 0;

// Phase 3 supported arithmetic ops (all bail to arith_overflow on
// non-SmI or signed overflow):
//   0x60 +     0x61 -     0x62 <     0x63 >
//   0x64 <=    0x65 >=    0x66 =     0x67 ~=
// Phase 3 explicitly does NOT support * / // \\ bitAnd: bitOr:
// bitShift: @ — those have edge cases (multiply overflow detection,
// divide-by-zero, exact-divide check, point allocation) that need
// dedicated emit + bail logic.  Methods using them fall through to
// the bail stub.
inline bool isPhase3ArithOp(uint8_t op) {
    return (op >= 0x60 && op <= 0x67) || op == 0x68;  // 0x68 = `*`
}

// bitAnd: (0x6E) / bitOr: (0x6F): SmI tag bits 0..2 are 001 for both
// operands, so direct bitwise op of the tagged values produces a valid
// tagged SmI result.  No untag/retag needed.
inline bool isPhase3BitOp(uint8_t op) {
    return op == 0x6E || op == 0x6F;
}

// * (0x68): SmI mul with overflow check.
inline bool isPhase3MulOp(uint8_t op) {
    return op == 0x68;
}

// bitShift: (0x6C): SmI shift with bounds + overflow check.
inline bool isPhase3ShiftOp(uint8_t op) {
    return op == 0x6C;
}

// \\ (0x6A) modulo, // (0x6D) integer divide: SmI floor-div + mod
// with sign-mismatch adjustment.
inline bool isPhase3ModOp(uint8_t op) {
    return op == 0x6A || op == 0x6D;
}

// Compute the live bytecode length: walk forward decoding bytecodes
// using SistaV1::bytecodeLength().  Tracks max forward branch target
// so a `return` only terminates decoding when no jump points past it.
//
// Mirrors the stencil decoder's logic (JITCompiler.cpp:639-660): the
// stencil JIT also stops decoding at the first unconditional return
// whose position exceeds maxBranchTarget.  Without this, the trailer
// bytes past the last return (selector/temp-name oops packed into the
// CompiledMethod's bytes) get scanned as if they were bytecodes —
// which masquerades as random opcodes and breaks downstream emit.
//
// Returns the live byte count (≤ bcLen).  If the bytecode stream is
// malformed (a multi-byte op runs past bcLen), returns the count up
// to that point (effectively trims the malformed tail).
size_t computeLiveLength(const uint8_t* bc, size_t bcLen) {
    int maxBranchTarget = 0;
    size_t i = 0;
    while (i < bcLen) {
        uint8_t op = bc[i];
        int len = SistaV1::bytecodeLength(op);
        if (i + (size_t)len > bcLen) {
            // Multi-byte op truncated — treat the truncated bytes as
            // dead.  (The stencil decoder does `goto done`.)
            return i;
        }
        // Track forward branch targets for short jumps.  ExtJump*
        // (0xED..0xEF) need extB which we don't decode here; for now,
        // assume any ExtJump points forward and pessimistically extend
        // maxBranchTarget to bcLen so we DON'T trim past an ExtJump.
        // (Phase 4 doesn't yet emit jumps anyway, so any method with
        // ExtJump bails-on-entry via the pre-scan; this is just for
        // safety against future Phase-5 work.)
        if (SistaV1::isAnyShortJump(op)) {
            int tgt = SistaV1::shortJumpTarget(op, (int)i);
            if (tgt > maxBranchTarget) maxBranchTarget = tgt;
        } else if (op == SistaV1::ExtJump
                || op == SistaV1::ExtJumpTrue
                || op == SistaV1::ExtJumpFalse) {
            // Naked long-jump operand is UNSIGNED (forward 0-255);
            // the sign lives in an ExtendB prefix.  We don't track
            // ExtB prefix state here so prefixed long jumps may have
            // a larger effective offset; for those rare cases, fall
            // back to the pessimistic "could jump past bcLen".
            // Target = (i + 2) + offset.
            if (i + 1 < bcLen) {
                int tgt = static_cast<int>(i) + 2 + bc[i + 1];
                if (tgt > maxBranchTarget) maxBranchTarget = tgt;
            } else {
                maxBranchTarget = (int)bcLen;
            }
        }
        i += (size_t)len;
        // Stop at first unconditional return if no branches point past us.
        // Includes block returns (0x5D BlockReturnNil, 0x5E BlockReturnTop)
        // — block bodies typically end with one and the bytes past it are
        // packed temp-name / decoration data, not real bytecodes.
        if ((SistaV1::isReturn(op) || SistaV1::isBlockReturn(op))
                && (int)i > maxBranchTarget) {
            return i;
        }
    }
    return bcLen;
}

// Bytecode pre-scan: returns true iff every byte in [bc, bc+bcLen)
// is a single-byte opcode in our supported set (Phases 2 + 3).
// Caller must pass bcLen = computeLiveLength(...) so trailer bytes
// past the method's last return don't get scanned (they're not
// real bytecodes — they're packed selector/temp-name oop trailers).
//
// Rejects multi-byte opcodes (sends, jumps, ext-prefixes), arithmetic
// outside the Phase 3 allowlist, and any single-byte op outside the
// explicit allowlist.
// Phase 4 single-byte sends: 0x70-0xAF (special + literal sends).
// Each emits a bail-to-interp at the send byte; interp dispatches
// the send normally, then continues with the rest of the method.
// Same GC-safe state.ip computation as arith bail (state.method +
// bcOffsetFromMethObj).  See emitOne_x86 / emitOne_arm64 below.
inline bool isPhase4SendOp(uint8_t op) {
    // Binary special selectors with no inline arith fast path:
    //   0x69 /, 0x6B @
    // (0x60-0x67 + comparisons go through Phase 3 inline emit;
    //  0x68 *, 0x6A \\, 0x6C bitShift:, 0x6D //, 0x6E bitAnd:,
    //  0x6F bitOr: also inline.)  These two bail to interp via
    //  the standard IC miss path.
    if (op == 0x69 || op == 0x6B) {
        return true;
    }
    // 0x70..0x7F: special selectors 16..31
    // 0x80..0x8F: literal send 0 args
    // 0x90..0x9F: literal send 1 arg
    // 0xA0..0xAF: literal send 2 args
    return op >= 0x70 && op <= 0xAF;
}

// Walk the REAL opcode stream — one callback per instruction, skipping
// operand bytes via SistaV1::bytecodeLength (the same advance the emit
// loop performs).  Every send-site scan MUST use this, never a raw
// per-byte loop: a multi-byte instruction whose operand byte lands in
// the send range (e.g. PushArray <E7 81> — operand 0x81 looks like
// Send0) otherwise mints a phantom IC site, shifting every later
// site's selectorBits by one.  The shifted miss path then looks up the
// WRONG SELECTOR for the send (cascade-#3: #value: instead of
// #isIdentifier on OCParser>>parseAssignment — setter returned self,
// receiver landed where the Boolean belonged).
// Encoding-failure trap (cascade-#3, 2026-06-11): without an attached
// ErrorHandler, asmjit RECORDS the error and SKIPS the instruction — the
// compile "succeeds" with the instruction silently missing.  Two shipped
// bugs came from this class (orr/and bitmask immediates, the IC-offset
// add >4095).  This handler latches the failure; emitMethodBytes checks
// it and FAILS the compile, so the method stays interpreted instead of
// running with a hole in its code.
struct T1EncodingErrorHandler : public asmjit::ErrorHandler {
    bool failed = false;
    asmjit::Error first = asmjit::kErrorOk;
    void handle_error(asmjit::Error err, const char* message,
                      asmjit::BaseEmitter*) override {
        failed = true;
        if (first == asmjit::kErrorOk) first = err;
        static int logged = 0;
        if (++logged <= 20)
            fprintf(stderr,
                "[asmjit-t1] ENCODING ERROR (compile will bail): %s\n",
                message ? message : "?");
    }
};

template <typename F>
static inline void forEachRealOpcode(const uint8_t* bc, size_t bcLen, F&& fn) {
    size_t i = 0;
    while (i < bcLen) {
        uint8_t op = bc[i];
        int len = SistaV1::bytecodeLength(op);
        if (i + (size_t)len > bcLen) return;  // truncated multi-byte tail
        fn(i, op);
        i += (size_t)len;
    }
}

// Counter of methods successfully real-emitted with conditional jumps.
// Used by the FIRST_N / ONLY_N / SKIP_N bisect knobs.  Incremented in
// compileViaAsmjit after the JITMethod is finalized.
size_t g_condJumpRealCompiles = 0;

bool allBytecodesSupported(const uint8_t* bc, size_t bcLen) {
    const bool noSendsBisect = g_debug.t1NoSendsBisect;
    const int maxSendNArgs = g_debug.t1MaxSendNArgs;
    // PHARO_T1_INLINE_J2J_TRACE_UNSUPPORTED: log unsupported byte for first
    // N calls.  Helps identify why benchFib falls through to STUB compile.
    auto traceFail = [&](size_t at, uint8_t op, const char* why) {
        if (pharo::g_debug.t1InlineJ2JTraceUnsupp) {
            static size_t n = 0;
            if (n++ < 30) {
                fprintf(stderr, "[T1-UNSUPPORTED #%zu] at=%zu op=0x%02x why=%s bcLen=%zu\n",
                    n, at, op, why, bcLen);
            }
        }
    };
    // First pass: ExtA/ExtB prefix bytes.  We accept the bundle iff
    // the next bytecode is one that CONSUMES the prefix value
    // (sends, jumps, prefix-modifiable push/store variants).
    // Bytecodes that don't consume the prefix would leak extA_/extB_
    // into the bytecode AFTER them — unsafe for our bail-to-interp
    // model since the rest of the method then runs in interp with
    // stale extA_/extB_ from our perspective.
    //
    // Allowed next ops:
    //   0xE2-0xE5: ExtPush{RecvVar,LitVar,LitConst,Temp}  (consume extA)
    //   0xEA-0xEB: ExtSend/ExtSuperSend  (consume extA/extB)
    //   0xED-0xEF: ExtJump variants  (consume extB)
    //   0xF0-0xF5: ExtPop/Store{Recv,LitVar,Temp}  (consume extA)
    //   0x80-0xAF: Phase 4 literal sends  (consume extA — index = extA*16+i)
    //   0x70-0x7F: special selectors  (consume extB for nArgs)
    // WALK BY BYTECODE LENGTH — this loop byte-stepped (i++) until
    // 2026-06-11, so multi-byte OPERANDS were misread as opcodes: the
    // 0xE0 displacement byte of Dictionary>>scanFor:'s long backward
    // jump (E1 FF ED E0) read as a spurious ExtendA prefix -> whole
    // method rejected -> STUB -> the entire Dictionary probe loop ran
    // INTERPRETED (the 18.4M interp at:/key sends, most of the
    // dict-vs-Cog gap).  Same trap class as the cascade-#3 phantom IC
    // sites: NEVER scan bytecode raw.
    for (size_t i = 0; i < bcLen; ) {
        if (bc[i] == SistaV1::ExtendA || bc[i] == SistaV1::ExtendB) {
            if (i + 2 >= bcLen) {
                traceFail(i, bc[i], "ext-prefix-truncated");
                return false;
            }
            uint8_t nextOp = bc[i + 2];
            // Chained prefixes (ExtB ExtB op / ExtA ExtB op): accept the
            // inner prefix here; the final consumer is validated when
            // the walk reaches the LAST prefix in the chain.
            if (nextOp == SistaV1::ExtendA || nextOp == SistaV1::ExtendB) {
                i += 2;
                continue;
            }
            bool acceptable = false;
            // Bytecodes that consume the prefix:
            if (nextOp == SistaV1::ExtSend
                    || nextOp == SistaV1::ExtSuperSend) {
                acceptable = true;
            } else if (bc[i] == SistaV1::ExtendB
                    && (nextOp == SistaV1::ExtJump
                        || nextOp == SistaV1::ExtJumpTrue
                        || nextOp == SistaV1::ExtJumpFalse)
                    && g_debug.t1EnableJumps) {
                acceptable = true;
            } else if (nextOp >= 0x70 && nextOp <= 0xAF) {
                // Phase 4 sends (single-byte) — extA/extB extend the
                // selector index or numArgs respectively.
                acceptable = true;
            } else if (nextOp == SistaV1::ExtPushRecvVar
                    || nextOp == SistaV1::ExtPushLitVar
                    || nextOp == SistaV1::ExtPushLitConst
                    || nextOp == SistaV1::ExtPushTemp
                    || nextOp == SistaV1::ExtPopStoreRecv
                    || nextOp == SistaV1::ExtPopStoreLitVar
                    || nextOp == SistaV1::ExtPopStoreTemp
                    || nextOp == SistaV1::ExtStoreRecv
                    || nextOp == SistaV1::ExtStoreLitVar
                    || nextOp == SistaV1::ExtStoreTemp) {
                acceptable = true;
            }
            if (!acceptable) {
                traceFail(i, bc[i], "ext-prefix-not-handled-pattern");
                return false;
            }
            i += 2;  // past this prefix; next iteration lands on the op
            continue;
        }
        int stepLen = SistaV1::bytecodeLength(bc[i]);
        if (stepLen <= 0) { traceFail(i, bc[i], "unknown-length"); return false; }
        i += (size_t)stepLen;
    }
    for (size_t i = 0; i < bcLen; i++) {
        uint8_t op = bc[i];
        if (op <= 0x0F) continue;                     // pushRecvVar 0..15
        if (op >= SistaV1::PushLitVarBase
                && op <= SistaV1::PushLitVarLast) continue;  // 0x10..0x1F
        if (op >= 0x20 && op <= 0x3F) continue;       // pushLitConst 0..31
        if (op >= 0x40 && op <= 0x4B) continue;       // pushTemp 0..11
        if (op == SistaV1::PushReceiver) continue;    // 0x4C
        if (op >= 0x4D && op <= 0x51) continue;       // pushTrue/False/Nil/Zero/One
        if (op == SistaV1::Dup) continue;             // 0x53
        // PushThisContext 0x52 — emit bails to interp (interp
        // materializes the context and pushes it).  Rest of method
        // runs in interp; no resume.  Methods using thisContext are
        // rare enough that partial-JIT coverage is still a win over
        // the previous full-method bail.
        if (op == SistaV1::PushThisContext) continue;  // 0x52
        if (op >= SistaV1::ReturnReceiver
                && op <= SistaV1::ReturnTop) continue; // 0x58..0x5C
        // BlockReturnNil/BlockReturnTop 0x5D/0x5E — bail to interp
        // (block returns involve frame walking + enclosingLevels
        // semantics not worth inlining).  Methods/blocks that
        // contain a block return get partial JIT coverage.
        if (op == SistaV1::BlockReturnNil
                || op == SistaV1::BlockReturnTop) continue;
        if (isPhase3ArithOp(op)) continue;            // 0x60..0x67
        if (isPhase3BitOp(op)) continue;              // 0x6E, 0x6F bitAnd:/bitOr:
        if (isPhase3MulOp(op)) continue;              // 0x68 *
        if (isPhase3ShiftOp(op)) continue;            // 0x6C bitShift:
        if (isPhase3ModOp(op)) continue;              // 0x6A \\, 0x6D //
        if (isPhase4SendOp(op)) {
            if (noSendsBisect) return false;
            if (sendNArgs(op) > maxSendNArgs) return false;
            continue;                  // 0x69/0x6A/0x6B/0x6D + 0x70..0xAF
        }
        if (op >= SistaV1::PopStoreRecvBase
                && op <= SistaV1::PopStoreRecvLast) continue;  // 0xC8..0xCF
        if (op >= SistaV1::PopStoreTempBase
                && op <= SistaV1::PopStoreTempLast) continue;  // 0xD0..0xD7
        if (op == SistaV1::Pop) continue;             // 0xD8
        // Short forward jumps: 0xB0..0xC7 (uncond / true / false).
        // Only forward jumps in this range (offset 1..8).  Verify the
        // jump target lands at a valid bytecode position within [0, bcLen).
        // Out-of-range targets indicate a malformed method or a target
        // we'd need to handle specially (e.g., past the live region).
        if (op >= SistaV1::ShortJumpBase
                && op <= SistaV1::ShortJumpFalseLast) {
            // Conditional jumps remain DISABLED by default.  Enabling them
            // (PHARO_ASMJIT_T1_ENABLE_JUMPS=1) causes the eval-startup
            // path to DNU on garbage class indices — symptoms point at a
            // JIT-side emit bug (corrupted stack or wrong jump target),
            // NOT at the bail protocol as the memory note originally
            // claimed.  Verified 2026-05-12: ExitMustBool fires 0 times
            // even with jumps enabled and no canBailMidMethod gate, so
            // the hang isn't a bail cascade.
            //
            // The canBailMidMethod field + gate in this file and in
            // Interpreter.cpp:18731-18733 is correct infrastructure
            // for when this emit bug is fixed.  Default-on since
            // 2026-05-16 — fuzzer 39/39 PASS, -4% user CPU on bench.
            if (!g_debug.t1EnableJumps) return false;
            const int jumpsFirstN = g_debug.t1JumpsFirstN;
            const int jumpsOnlyN  = g_debug.t1JumpsOnlyN;
            const int jumpsSkipN  = g_debug.t1JumpsSkipN;
            const int jumpsSkipFrom = g_debug.t1JumpsSkipFrom;
            const int jumpsSkipTo   = g_debug.t1JumpsSkipTo;
            if (jumpsFirstN >= 0 || jumpsOnlyN >= 0 || jumpsSkipN >= 0
                    || jumpsSkipFrom >= 0) {
                size_t next = g_condJumpRealCompiles + 1;
                if (jumpsFirstN >= 0 && (int)next > jumpsFirstN)
                    return false;
                if (jumpsOnlyN >= 0 && (int)next != jumpsOnlyN)
                    return false;
                if (jumpsSkipN >= 0 && (int)next == jumpsSkipN)
                    return false;
                if (jumpsSkipFrom >= 0 && jumpsSkipTo >= 0
                        && (int)next >= jumpsSkipFrom
                        && (int)next <= jumpsSkipTo)
                    return false;
            }
            int target = SistaV1::shortJumpTarget(op, (int)i);
            // Out-of-range targets are typically unreachable trailer bytes
            // (Pharo CompiledMethod trailer encoding looks like bytecodes
            // but execution never reaches them).  Allow; the emit will
            // produce a bail for the offending instruction.  Required to
            // JIT benchFib (whose trailer ends with 0xb0 → target past
            // bcLen) and most other methods with trailers.
            (void)target;
            if (op > SistaV1::ShortJumpLast && (size_t)(i + 1) >= bcLen) {
                return false;
            }
            continue;
        }
        // InlinedPrimitive 0xEC: 2-byte no-op for the interpreter
        // (per JITCompiler.cpp:395+).  Sista marks an inlined prim;
        // bytecodes that the inlined version replaced execute normally
        // around it.  Just skip the operand byte.
        if (op == SistaV1::InlinedPrimitive) {
            if (i + 1 >= bcLen) {
                traceFail(i, op, "inlined-prim-truncated");
                return false;
            }
            i += 1;
            continue;
        }
        // PushInteger 0xE8: opcode + signed 8-bit immediate.  Pushes
        // SmI(N) where N = (int8_t)operand byte.  Common; e.g. methods
        // with literal integers outside [-1, 2] range.
        if (op == SistaV1::PushInteger) {
            if (i + 1 >= bcLen) {
                traceFail(i, op, "push-integer-truncated");
                return false;
            }
            i += 1;  // skip operand byte
            continue;
        }
        // PushCharacter 0xE9: opcode + unsigned 8-bit codepoint.  Pushes
        // Character(N).  Common in string-processing methods.
        if (op == SistaV1::PushCharacter) {
            if (i + 1 >= bcLen) {
                traceFail(i, op, "push-character-truncated");
                return false;
            }
            i += 1;
            continue;
        }
        // PushFullBlock 0xF9: 3-byte (opcode + 2 operand bytes).  Emit
        // bails to ExitBlockCreate so the chain loop creates the
        // FullBlockClosure (memory allocation + outer-context capture).
        if (op == SistaV1::PushFullBlock) {
            if (i + 2 >= bcLen) {
                traceFail(i, op, "push-full-block-truncated");
                return false;
            }
            i += 2;  // skip 2 operand bytes
            continue;
        }
        // PushArray 0xE7: 2-byte (opcode + desc).  Emit bails to
        // ExitArrayCreate so the chain loop allocates the Array and
        // optionally pops `size` elements into it.  desc = arraySize:7,
        // popIntoArray:1.
        if (op == SistaV1::PushArray) {
            if (i + 1 >= bcLen) {
                traceFail(i, op, "push-array-truncated");
                return false;
            }
            i += 1;
            continue;
        }
        // PushTempAtInVec 0xFB / StoreTempAtInVec 0xFC /
        // PopStoreTempAtInVec 0xFD: 3-byte (opcode + tempIdx + vecIdx).
        // Remote temp access (read/write a slot in a temp vector held
        // by an enclosing closure).  Bail to interp at the opcode;
        // interp executes the 3-byte op (no extA_/extB_ dependency).
        // Rest of method runs in interp.
        if (op == SistaV1::PushTempAtInVec
                || op == SistaV1::StoreTempAtInVec
                || op == SistaV1::PopStoreTempAtInVec) {
            if (i + 2 >= bcLen) {
                traceFail(i, op, "remote-temp-truncated");
                return false;
            }
            i += 2;
            continue;
        }
        // ExtSend 0xEA / ExtSuperSend 0xEB: 2-byte sends.  Even
        // with the first-pass ExtA/ExtB rejection, ExtSend
        // acceptance breaks some methods (DNU cascade in snapshot
        // error path).  Investigation pending — for now stay opt-in.
        // ExtA/ExtB prefix bundle.  First-pass guard verified the
        // next bytecode consumes the prefix.  Skip the prefix data
        // byte + nextOp's full length (1, 2, or 3 bytes).
        if (op == SistaV1::ExtendA || op == SistaV1::ExtendB) {
            uint8_t nextOp = bc[i + 2];
            int nextLen = SistaV1::bytecodeLength(nextOp);
            if (i + 1 + nextLen >= bcLen) {
                traceFail(i, op, "ext-prefix-bundle-truncated");
                return false;
            }
            if (nextOp == SistaV1::ExtSuperSend) {
                if (!pharo::g_debug.t1AcceptExtSuperSend) {
                    traceFail(i, op, "ext-super-send-bundle-disabled");
                    return false;
                }
            }
            i += 1 + nextLen;
            continue;
        }
        if (op == SistaV1::ExtSend || op == SistaV1::ExtSuperSend) {
            // ExtSend 0xEA is safe; ExtSuperSend 0xEB needs further
            // investigation (super-send needs methodClass and the
            // bail may need extra state restore).  Opt-in flag
            // PHARO_T1_ACCEPT_EXTSUPERSEND=1 to test.
            //
            // Naked here means no preceding ExtA/B — the bundle case
            // is handled above when ExtA/B is at i.
            if (op == SistaV1::ExtSuperSend) {
                if (!pharo::g_debug.t1AcceptExtSuperSend) {
                    traceFail(i, op, "ext-super-send-disabled");
                    return false;
                }
            }
            if (i + 1 >= bcLen) {
                traceFail(i, op, "ext-send-truncated");
                return false;
            }
            i += 1;
            continue;
        }
        // Extended push/store variants — all 2-byte: opcode + 1-byte index.
        // Push: ExtPushRecvVar 0xE2, ExtPushLitVar 0xE3,
        //       ExtPushLitConst 0xE4, ExtPushTemp 0xE5.
        // Store-with-immutable-check (recv): ExtPopStoreRecv 0xF0,
        //                                    ExtStoreRecv 0xF3.
        // Store-no-check (temp): ExtPopStoreTemp 0xF2, ExtStoreTemp 0xF5.
        // Association store (litvar): ExtPopStoreLitVar 0xF1,
        //                             ExtStoreLitVar 0xF4.
        //   Stores to literals[idx].slot[1] — no write barrier
        //   because YG scavenge does a full old-space scan.  Naked
        //   only (the ExtA prefix-rejection first-pass guarantees no
        //   prefix extension on idx).
        if (op == SistaV1::ExtPushRecvVar || op == SistaV1::ExtPushLitVar
                || op == SistaV1::ExtPushLitConst
                || op == SistaV1::ExtPushTemp
                || op == SistaV1::ExtPopStoreTemp
                || op == SistaV1::ExtStoreTemp
                || op == SistaV1::ExtPopStoreRecv
                || op == SistaV1::ExtStoreRecv
                || op == SistaV1::ExtPopStoreLitVar
                || op == SistaV1::ExtStoreLitVar) {
            if (i + 1 >= bcLen) {
                traceFail(i, op, "ext-bytecode-truncated");
                return false;
            }
            i += 1;
            continue;
        }
        // Long jumps 0xED/0xEE/0xEF: opcode + 1-byte signed offset.
        // Target = (i + 2) + offset.  No ExtA/ExtB prefix support yet.
        // Gated on the same t1EnableJumps knob as short jumps.
        if (op == SistaV1::ExtJump || op == SistaV1::ExtJumpTrue
                || op == SistaV1::ExtJumpFalse) {
            if (!g_debug.t1EnableJumps) {
                traceFail(i, op, "long-jump-knob-off");
                return false;
            }
            if (i + 1 >= bcLen) {
                traceFail(i, op, "long-jump-truncated");
                return false;
            }
            // Naked long-jump operand is UNSIGNED (forward 0-255); the
            // sign lives in an ExtendB prefix, which the pre-scan
            // routes to the interp-bail bundle path.
            int target = static_cast<int>(i) + 2 + bc[i + 1];
            // Allow out-of-range targets (unreachable trailer bytes).
            (void)target;
            i += 1;  // skip offset byte (loop will increment by 1 more)
            continue;
        }
        // Anything else (jumps, ext-prefixes, pushLitVar,
        // pushThisContext, dup, blockReturn, popStoreRecv/Temp,
        // mul/div/bit ops, ext bytecodes E0+, etc.) → unsupported.
        traceFail(i, op, "fallthrough-unsupported");
        return false;
    }
    return true;
}

// If the method has a primitive whose JIT prologue we can emit,
// return the primitive index.  Otherwise return -1.  Caller is
// responsible for handing us a primitive method (header bit 16 set)
// whose first 3 bytes are the CallPrimitive (0xF8 lo hi) bytecode.
//
// Currently only prim 1 (SmallInteger>>#+).  More to follow.
inline int supportedPrimIndex(const uint8_t* bc, size_t bcLen) {
    if (bcLen < 3 || bc[0] != SistaV1::CallPrimitive) return -1;
    int primIndex = bc[1] | ((bc[2] & 0x1F) << 8);
    switch (primIndex) {
    case 1:  return primIndex;   // SmallInteger>>+
    case 2:  return primIndex;   // SmallInteger>>-
    case 3:  return primIndex;   // SmallInteger>><
    case 4:  return primIndex;   // SmallInteger>>>
    case 5:  return primIndex;   // SmallInteger>><=
    case 6:  return primIndex;   // SmallInteger>>>=
    case 7:  return primIndex;   // SmallInteger>>=
    case 8:  return primIndex;   // SmallInteger>>~=
    case 9:  return primIndex;   // SmallInteger>>*
    case 10: return primIndex;   // SmallInteger>>\\ (jit-may23 T7)
    case 11: return primIndex;   // SmallInteger>>// (T7)
    case 12: return primIndex;   // SmallInteger>>bitShift: (T7)
    case 13: return primIndex;   // SmallInteger>>/ (T7)
    case 14: return primIndex;   // SmallInteger>>bitAnd:
    case 15: return primIndex;   // SmallInteger>>bitOr:
    case 16: return primIndex;   // SmallInteger>>bitXor:
    // Prims 60/61/62 prologue handles fmt 2 (Array), fmt 10-11 (32-bit
    // WordArray/IntegerArray), and fmt 16-23 (byte indexable: ByteArray,
    // String, Symbol).  Other formats (fmt 9 Indexable64, fmt 12-15
    // 16-bit, fmt 3-5 variable+fixed) still fall through to the
    // Smalltalk fallback — for `Object>>basicAtPut:` that's
    // errorImproperStore, which is correct for cases the C primitive
    // would also fail on (e.g., wrong type) but raises spuriously for
    // legal receivers in those formats.  Re-enable when those land.
    case 60: return primIndex;   // Array/ByteArray/WordArray >> at:
    case 61: return primIndex;   // Array/ByteArray/WordArray >> at:put:
    case 62: return primIndex;   // Array/ByteArray/WordArray >> size
    case 541: return primIndex;  // SmallFloat>>+
    case 542: return primIndex;  // SmallFloat>>-
    case 549: return primIndex;  // SmallFloat>>*
    case 110: return primIndex;  // ProtoObject>>==
    // NOTE (2026-06-11): compiling quick prims 256-519 (body-only, no
    // prologue) was TRIED and is a measured ~2x REGRESSION on the
    // dict/sendmix/fib macro benches: the C++ quick-path (direct slot
    // read in executePrimitive, no frame) is cheaper than a J2J call
    // to a compiled 2-bytecode method, and the compile flood pressures
    // the zone.  Don't re-add without a leaner call path.
    default: return -1;
    }
}

#if defined(__x86_64__) || defined(_M_X64)

// Emit a "push value into Smalltalk stack" sequence on x86_64.  The
// value to push must already be in `valReg` (any 64-bit gp reg
// other than rdi/rcx).  Sequence:
//
//   mov rcx, [rdi+OFF_SP]       ; load sp
//   mov [rcx], valReg           ; *sp = value
//   add rcx, 8                  ; sp++
//   mov [rdi+OFF_SP], rcx       ; store sp back
//
// rcx is clobbered.  (If we ever cache sp across multiple bytecodes
// we'll factor this differently; Phase 2 reloads on every push.)
void emitPushReg(asmjit::x86::Assembler& a, asmjit::x86::Gp valReg) {
    using namespace asmjit::x86;
    a.mov(rcx, ptr(rdi, OFF_SP));
    a.mov(ptr(rcx), valReg);
    a.add(rcx, 8);
    a.mov(ptr(rdi, OFF_SP), rcx);
}

// Emit the JIT prologue for a supported primitive.  Used as a fast
// path when the chain loop inline-activates this method — without
// it, hasPrimPrologue=false blocks inline activation entirely
// (Interpreter.cpp:18731-18732).
//
// On entry:
//   rdi = state ptr
//   state.receiver = SmI candidate
//   state.tempBase[0] = SmI candidate (the argument)
// On success: set state.returnValue, set ExitReturn, ret.
// On failure (non-SmI, arith overflow): fall through.  Caller's emit
// continues with the fallback bytecode.
//
// Mirrors the stencil prologues at stencils/stencils.cpp:3231+.
//   prim 1 = #+   prim 2 = #-                       (arith, overflow check)
//   prim 3 = #<   prim 4 = #>   prim 5 = #<=
//   prim 6 = #>=  prim 7 = #=   prim 8 = #~=        (compare, return bool)
//   prim 110 = #==                                  (identical: oop bit-compare)
//
// Signed comparison of tagged SmI bits gives the same ordering as
// untagged values (tag is the same low 3 bits, so shifting preserves
// order).  So all comparisons skip the untag step.
void emitPrimProlog_x86(asmjit::x86::Assembler& a, int primIndex) {
    using namespace asmjit::x86;

    // prim 110 (#==): no type check — compare raw oop bits.
    if (primIndex == 110) {
        a.mov(rcx, ptr(rdi, OFF_RECEIVER));
        a.mov(rdx, ptr(rdi, OFF_TEMPBASE));
        a.mov(rdx, ptr(rdx));
        a.mov(rax, ptr(rdi, OFF_FALSEOOP));
        a.cmp(rcx, rdx);
        a.cmove(rax, ptr(rdi, OFF_TRUEOOP));
        a.mov(ptr(rdi, OFF_RETVAL), rax);
        a.mov(dword_ptr(rdi, OFF_EXIT), asmjit::Imm(EXIT_RETURN));
        a.ret();
        return;  // never falls through (always succeeds)
    }

    asmjit::Label fail = a.new_label();

    // Shared SmI check: (a^b) | (a-1) low 3 bits = 0 iff both SmI.
    a.mov(rcx, ptr(rdi, OFF_RECEIVER));   // rcx = receiver
    a.mov(rdx, ptr(rdi, OFF_TEMPBASE));
    a.mov(rdx, ptr(rdx));                 // rdx = tempBase[0] = arg
    a.mov(r8,  rcx);
    a.xor_(r8, rdx);
    a.lea(r9, asmjit::x86::ptr(rcx, -1));
    a.or_(r8, r9);
    a.test(r8.r8(), asmjit::Imm(7));
    a.jne(fail);

    if (primIndex >= 3 && primIndex <= 8) {
        // Comparison: cmp + cmov with memory source (saves trueOop load).
        a.mov(rax, ptr(rdi, OFF_FALSEOOP));
        a.cmp(rcx, rdx);
        switch (primIndex) {
            case 3: a.cmovl (rax, ptr(rdi, OFF_TRUEOOP)); break;  // <
            case 4: a.cmovg (rax, ptr(rdi, OFF_TRUEOOP)); break;  // >
            case 5: a.cmovle(rax, ptr(rdi, OFF_TRUEOOP)); break;  // <=
            case 6: a.cmovge(rax, ptr(rdi, OFF_TRUEOOP)); break;  // >=
            case 7: a.cmove (rax, ptr(rdi, OFF_TRUEOOP)); break;  // =
            case 8: a.cmovne(rax, ptr(rdi, OFF_TRUEOOP)); break;  // ~=
        }
        a.mov(ptr(rdi, OFF_RETVAL), rax);
        a.mov(dword_ptr(rdi, OFF_EXIT), asmjit::Imm(EXIT_RETURN));
        a.ret();
        a.bind(fail);
        return;
    }

    if (primIndex == 14 || primIndex == 15 || primIndex == 16) {
        // bitAnd / bitOr: SmI tag is preserved by both ops because
        // both operands have low 3 bits = 001, so AND/OR keeps it 001.
        // bitXor: low 3 = 001 XOR 001 = 000, so we re-OR the SMI_TAG.
        // Either way: operate on tagged values, no untag.
        if (primIndex == 14) a.and_(rcx, rdx);
        else if (primIndex == 15) a.or_(rcx, rdx);
        else {
            a.xor_(rcx, rdx);
            a.or_(rcx, asmjit::Imm(SMI_TAG));
        }
        a.mov(ptr(rdi, OFF_RETVAL), rcx);
        a.mov(dword_ptr(rdi, OFF_EXIT), asmjit::Imm(EXIT_RETURN));
        a.ret();
        a.bind(fail);
        return;
    }

    // Arith path: tagged-arith for + and - (saves untag+retag).
    // For + : a_bits + b_bits = (a+b)*8 + 2 → sub 1 to tag.
    // For - : a_bits - b_bits = (a-b)*8     → add 1 to tag.
    // jo on the tagged op catches SmI-range overflow because tagged
    // form pre-shifts by 8 (matches the bytecode arith emit).
    if (primIndex == 1) {
        a.add(rcx, rdx);
        a.jo(fail);
        a.sub(rcx, asmjit::Imm(1));
        a.mov(ptr(rdi, OFF_RETVAL), rcx);
        a.mov(dword_ptr(rdi, OFF_EXIT), asmjit::Imm(EXIT_RETURN));
        a.ret();
        a.bind(fail);
        return;
    }
    if (primIndex == 2) {
        a.sub(rcx, rdx);
        a.jo(fail);
        a.add(rcx, asmjit::Imm(1));
        a.mov(ptr(rdi, OFF_RETVAL), rcx);
        a.mov(dword_ptr(rdi, OFF_EXIT), asmjit::Imm(EXIT_RETURN));
        a.ret();
        a.bind(fail);
        return;
    }

    // prim 9 (*): need to untag, multiply, check overflow, retag.
    a.sar(rcx, asmjit::Imm(3));
    a.sar(rdx, asmjit::Imm(3));
    if (primIndex == 9) {
        // a * b: imul sets OF on 64-bit signed overflow.  After that
        // we also need to confirm the result fits in 61-bit SmI range:
        // ((result << 3) >> 3) must equal result.
        a.imul(rcx, rdx);
        a.jo(fail);
        a.mov(r8, rcx);
        a.shl(r8, asmjit::Imm(3));
        a.sar(r8, asmjit::Imm(3));
        a.cmp(r8, rcx);
        a.jne(fail);
    } else {
        // Shouldn't happen — supportedPrimIndex gates this.
        a.bind(fail);
        return;
    }
    a.shl(rcx, asmjit::Imm(3));
    a.or_(rcx, asmjit::Imm(SMI_TAG));
    a.mov(ptr(rdi, OFF_RETVAL), rcx);
    a.mov(dword_ptr(rdi, OFF_EXIT), asmjit::Imm(EXIT_RETURN));
    a.ret();

    a.bind(fail);
}

// Emit per-bytecode code on x86_64.  Returns true if the opcode was
// handled.  The pre-scan guarantees we'll see only supported ops.
//
// `nilBits` is the raw bits of the special-objects nil — passed in
// so push-nil can bake it as a 64-bit immediate (nil is image-local,
// not a JITState field).
//
// `bcOffsetFromMethObj` is the byte offset of THIS bytecode from the
// CompiledMethod object's address.  Arith bails compute
// `state.ip = state.method.rawBits() + bcOffsetFromMethObj` at
// runtime, NOT at JIT-compile time.  This survives GC compaction:
// `state.method` is a GC-tracked Oop that gets updated in place when
// the CompiledMethod moves, while a baked absolute address would
// dangle.  Mirrors the stencil JIT, which uses `s->ip = s->ip +
// bcOffset` and relies on `afterGC()` updating state.ip.
bool emitOne_x86(asmjit::x86::Assembler& a, uint8_t op,
                  uint64_t nilBits, int bcOffsetFromMethObj,
                  int siteIdx,
                  const std::vector<asmjit::Label>& bcLabels,
                  int globalIdx,
                  int callerArgCount, int callerTempCount,
                  int staticJ2JArgCount) {
    using namespace asmjit::x86;
    (void)bcOffsetFromMethObj;
    (void)siteIdx;
    (void)callerArgCount;
    (void)callerTempCount;
    (void)staticJ2JArgCount;

    // pushRecvVar N: push receiver.slot[N].
    if (op <= 0x0F) {
        int n = op & 0x0F;
        a.mov(rax, ptr(rdi, OFF_RECEIVER));
        a.mov(rax, ptr(rax, OBJ_SLOT_0 + n * 8));
        emitPushReg(a, rax);
        return true;
    }
    // pushLitConst N: push literals[N].
    if (op >= 0x20 && op <= 0x3F) {
        int n = op - 0x20;
        a.mov(rax, ptr(rdi, OFF_LITERALS));
        a.mov(rax, ptr(rax, n * 8));
        emitPushReg(a, rax);
        return true;
    }
    // pushLitVar N (0x10..0x1F): literals[N] is an Association;
    // push Association.value (slot 1).
    if (op >= SistaV1::PushLitVarBase && op <= SistaV1::PushLitVarLast) {
        int n = op - SistaV1::PushLitVarBase;
        a.mov(rax, ptr(rdi, OFF_LITERALS));
        a.mov(rax, ptr(rax, n * 8));                     // Association oop
        a.mov(rax, ptr(rax, OBJ_SLOT_0 + 8));            // slot[1] = value
        emitPushReg(a, rax);
        return true;
    }
    // pushTemp N: push tempBase[N].
    if (op >= 0x40 && op <= 0x4B) {
        int n = op - 0x40;
        a.mov(rax, ptr(rdi, OFF_TEMPBASE));
        a.mov(rax, ptr(rax, n * 8));
        emitPushReg(a, rax);
        return true;
    }
    // pushReceiver: push state.receiver.
    if (op == SistaV1::PushReceiver) {
        a.mov(rax, ptr(rdi, OFF_RECEIVER));
        emitPushReg(a, rax);
        return true;
    }
    // Dup: read sp[-1], push it (stack [..., v] → [..., v, v]).
    if (op == SistaV1::Dup) {
        a.mov(rcx, ptr(rdi, OFF_SP));
        a.mov(rax, ptr(rcx, -8));
        emitPushReg(a, rax);
        return true;
    }
    // pushTrue: push state.trueOop.
    if (op == 0x4D) {
        a.mov(rax, ptr(rdi, OFF_TRUEOOP));
        emitPushReg(a, rax);
        return true;
    }
    // pushFalse: push state.falseOop.
    if (op == 0x4E) {
        a.mov(rax, ptr(rdi, OFF_FALSEOOP));
        emitPushReg(a, rax);
        return true;
    }
    // pushNil: push baked nil immediate.
    if (op == 0x4F) {
        a.mov(rax, asmjit::Imm(nilBits));
        emitPushReg(a, rax);
        return true;
    }
    // pushZero: push fromSmallInteger(0) = 1.
    if (op == 0x50) {
        a.mov(rax, asmjit::Imm(smiBits(0)));
        emitPushReg(a, rax);
        return true;
    }
    // pushOne: push fromSmallInteger(1) = 9.
    if (op == 0x51) {
        a.mov(rax, asmjit::Imm(smiBits(1)));
        emitPushReg(a, rax);
        return true;
    }
    // pop: state.sp--.
    if (op == SistaV1::Pop) {
        a.mov(rcx, ptr(rdi, OFF_SP));
        a.sub(rcx, 8);
        a.mov(ptr(rdi, OFF_SP), rcx);
        return true;
    }
    // popStoreTemp N (0xD0..0xD7): pop TOS, store into tempBase[N].
    if (op >= SistaV1::PopStoreTempBase && op <= SistaV1::PopStoreTempLast) {
        int n = op - SistaV1::PopStoreTempBase;
        a.mov(rcx, ptr(rdi, OFF_SP));
        a.sub(rcx, 8);
        a.mov(ptr(rdi, OFF_SP), rcx);
        a.mov(rax, ptr(rcx));
        a.mov(rdx, ptr(rdi, OFF_TEMPBASE));
        a.mov(ptr(rdx, n * 8), rax);
        return true;
    }
    // popStoreRecv N (0xC8..0xCF): pop TOS, store into receiver.slot[N].
    // Tests the receiver's immutable bit (header bit 23) before writing
    // and bails to interp if set — mirrors Interpreter::setReceiverInstVar,
    // which sends #attemptToAssign:withIndex: (Object's default raises
    // ModificationForbidden).  Without this check, JIT-compiled setters
    // bypassed the read-only enforcement (ObjectTest>>testBeRecursively-
    // ReadOnlyObject failed under default JIT).  Direct heap write
    // otherwise — no GC barrier (the stencil JIT writes inline too).
    if (op >= SistaV1::PopStoreRecvBase && op <= SistaV1::PopStoreRecvLast) {
        int n = op - SistaV1::PopStoreRecvBase;
        asmjit::Label bail = a.new_label();
        asmjit::Label end  = a.new_label();
        a.mov(rdx, ptr(rdi, OFF_RECEIVER));
        // Test bit 23 of the 64-bit header.  test r/m32, imm32 reads
        // the low 4 bytes and ANDs against imm32; ImmutableBit lives
        // in that half so a 32-bit test is sufficient.
        a.test(dword_ptr(rdx),
               asmjit::Imm(static_cast<int32_t>(0x800000)));
        a.jne(bail);
        a.mov(rcx, ptr(rdi, OFF_SP));
        a.sub(rcx, 8);
        a.mov(ptr(rdi, OFF_SP), rcx);
        a.mov(rax, ptr(rcx));
        a.mov(ptr(rdx, OBJ_SLOT_0 + n * 8), rax);
        a.jmp(end);
        a.bind(bail);
        a.mov(r8, ptr(rdi, OFF_METHOD));
        a.add(r8, asmjit::Imm(bcOffsetFromMethObj));
        a.mov(ptr(rdi, OFF_IP), r8);
        a.mov(dword_ptr(rdi, OFF_EXIT),
              asmjit::Imm(EXIT_ARITH_OVERFLOW));
        a.ret();
        a.bind(end);
        return true;
    }
    // NON-LOCAL RETURN: 0x58-0x5C inside a CompiledBlock returns from
    // the HOME method, not the block — bail to interp (mirrors the
    // arm64 gate; see g_emitIsBlock).
    if (g_emitIsBlock && SistaV1::isReturn(op)) {
        a.mov(r8, ptr(rdi, OFF_METHOD));
        a.add(r8, asmjit::Imm(bcOffsetFromMethObj));
        a.mov(ptr(rdi, OFF_IP), r8);
        a.mov(dword_ptr(rdi, OFF_EXIT), asmjit::Imm(EXIT_ARITH_OVERFLOW));
        a.ret();
        return true;
    }
    // returnReceiver: returnValue = receiver; exitReason = ExitReturn; ret.
    if (op == SistaV1::ReturnReceiver) {
        a.mov(rax, ptr(rdi, OFF_RECEIVER));
        a.mov(ptr(rdi, OFF_RETVAL), rax);
        a.mov(dword_ptr(rdi, OFF_EXIT), asmjit::Imm(EXIT_RETURN));
        a.ret();
        return true;
    }
    // returnTrue / returnFalse: similar with bake-via-state.
    if (op == 0x59) {
        a.mov(rax, ptr(rdi, OFF_TRUEOOP));
        a.mov(ptr(rdi, OFF_RETVAL), rax);
        a.mov(dword_ptr(rdi, OFF_EXIT), asmjit::Imm(EXIT_RETURN));
        a.ret();
        return true;
    }
    if (op == 0x5A) {
        a.mov(rax, ptr(rdi, OFF_FALSEOOP));
        a.mov(ptr(rdi, OFF_RETVAL), rax);
        a.mov(dword_ptr(rdi, OFF_EXIT), asmjit::Imm(EXIT_RETURN));
        a.ret();
        return true;
    }
    // returnNil: bake nil immediate.
    if (op == 0x5B) {
        a.mov(rax, asmjit::Imm(nilBits));
        a.mov(ptr(rdi, OFF_RETVAL), rax);
        a.mov(dword_ptr(rdi, OFF_EXIT), asmjit::Imm(EXIT_RETURN));
        a.ret();
        return true;
    }
    // returnTop: pop into rax (sp--, then *sp), store as retVal.
    if (op == SistaV1::ReturnTop) {
        a.mov(rcx, ptr(rdi, OFF_SP));
        a.sub(rcx, 8);
        a.mov(ptr(rdi, OFF_SP), rcx);
        a.mov(rax, ptr(rcx));
        a.mov(ptr(rdi, OFF_RETVAL), rax);
        a.mov(dword_ptr(rdi, OFF_EXIT), asmjit::Imm(EXIT_RETURN));
        a.ret();
        return true;
    }
    // Phase 3 arithmetic: 0x60..0x67 (+, -, <, >, <=, >=, =, ~=).
    // All share the same prologue (load operands + SmI check + bail
    // setup) and epilogue (write result, advance SP, fall through).
    if (isPhase3ArithOp(op)) {
        // Pattern (x86-64 SysV; rdi = state):
        //   rax = state.sp;  rcx = a = sp[-2];  rdx = b = sp[-1]
        //   r8 = a XOR 1;  r9 = b XOR 1;  r8 |= r9
        //   if (r8 & 7) goto bail
        //   sar rcx, 3;  sar rdx, 3   ; untag (signed)
        //   then op-specific body
        //   write result to sp[-2];  sp -= 8
        //   jmp end
        // bail:
        //   r8 = state.method + bcOffsetFromMethObj   ; ip-relative-to-method
        //   mov [rdi+OFF_IP], r8
        //   mov dword [rdi+OFF_EXIT], EXIT_ARITH_OVERFLOW
        //   ret
        // end:
        asmjit::Label bail = a.new_label();
        asmjit::Label end  = a.new_label();

        a.mov(rax, ptr(rdi, OFF_SP));
        a.mov(rcx, ptr(rax, -16));   // a
        a.mov(rdx, ptr(rax, -8));    // b
        // SmI tag check (both SmI): 5 instructions, replaces the older
        // 7-instr XOR-pair-OR sequence.
        //   r8 = a^b   — low 3 = 0 iff same tag
        //   r9 = a-1   — low 3 = 0 iff a is SmI (tag 001)
        //   r8 |= r9   — combined; low 3 = 0 iff both SmI
        //   test low 8 bits against mask 7 — checks low 3 bits.
        // Correctness: both SmI <=> same tag AND a is SmI.  Verified
        // for SmI min/max + boundary cases.  Saves ~3 cycles per arith.
        a.mov(r8,  rcx);
        a.xor_(r8, rdx);
        a.lea(r9, asmjit::x86::ptr(rcx, -1));
        a.or_(r8, r9);
        a.test(r8.r8(), asmjit::Imm(7));
        a.jne(bail);

        if (op == 0x60) {            // +
            // Tagged-add trick: a_bits + b_bits = (a+b)*8 + 2.
            // jo catches SmI-range overflow directly (since tagged
            // form pre-shifts by 8, 64-bit overflow ⇔ SmI overflow).
            // Then sub 1 to convert (a+b)*8 + 2 → (a+b)*8 + 1 (tagged).
            // Saves 6 instructions vs the untag/add/retag pattern.
            a.add(rcx, rdx);
            a.jo(bail);
            a.sub(rcx, asmjit::Imm(1));
            a.mov(ptr(rax, -16), rcx);
            a.sub(rax, 8);
            a.mov(ptr(rdi, OFF_SP), rax);
        } else if (op == 0x61) {     // -
            // Tagged-sub: a_bits - b_bits = (a-b)*8.
            // jo catches SmI overflow (same reasoning as +).
            // Add 1 to convert (a-b)*8 → (a-b)*8 + 1 (tagged).
            a.sub(rcx, rdx);
            a.jo(bail);
            a.add(rcx, asmjit::Imm(1));
            a.mov(ptr(rax, -16), rcx);
            a.sub(rax, 8);
            a.mov(ptr(rdi, OFF_SP), rax);
        } else {
            // Comparison ops.  cmp tagged bits directly — the map
            // x → 8x+1 is monotonic for signed values, so cmp(a_bits,
            // b_bits) gives the same flags as cmp(a, b).  Saves 2× sar.
            // cmov takes a memory operand directly — saves a load.
            a.cmp(rcx, rdx);
            a.mov(rsi, ptr(rdi, OFF_FALSEOOP));   // default: false
            switch (op) {
                case 0x62: a.cmovl(rsi, ptr(rdi, OFF_TRUEOOP)); break;   // <
                case 0x63: a.cmovg(rsi, ptr(rdi, OFF_TRUEOOP)); break;   // >
                case 0x64: a.cmovle(rsi, ptr(rdi, OFF_TRUEOOP)); break;  // <=
                case 0x65: a.cmovge(rsi, ptr(rdi, OFF_TRUEOOP)); break;  // >=
                case 0x66: a.cmove(rsi, ptr(rdi, OFF_TRUEOOP)); break;   // =
                case 0x67: a.cmovne(rsi, ptr(rdi, OFF_TRUEOOP)); break;  // ~=
            }
            a.mov(ptr(rax, -16), rsi);
            a.sub(rax, 8);
            a.mov(ptr(rdi, OFF_SP), rax);
        }
        a.jmp(end);

        a.bind(bail);
        // r8 = state.method.rawBits + bcOffsetFromMethObj
        // (state.method is GC-tracked Oop; this is the post-GC-safe
        // address of the failing bytecode.)
        a.mov(r8, ptr(rdi, OFF_METHOD));
        a.add(r8, asmjit::Imm(bcOffsetFromMethObj));
        a.mov(ptr(rdi, OFF_IP), r8);
        a.mov(dword_ptr(rdi, OFF_EXIT),
              asmjit::Imm(EXIT_ARITH_OVERFLOW));
        a.ret();

        a.bind(end);
        return true;
    }
    // Phase 3 bitwise: 0x6E bitAnd:, 0x6F bitOr:.  SmI tag bits (low 3
    // = 001) commute with bitwise AND/OR — a & b and a | b on tagged
    // SmIs produce a valid tagged SmI result without untag/retag.
    if (isPhase3BitOp(op)) {
        asmjit::Label bail = a.new_label();
        asmjit::Label end  = a.new_label();
        a.mov(rax, ptr(rdi, OFF_SP));
        a.mov(rcx, ptr(rax, -16));   // a
        a.mov(rdx, ptr(rax, -8));    // b
        // SmI check: (a^b) | (a-1) low 3 bits = 0 iff both SmI.
        a.mov(r8,  rcx);
        a.xor_(r8, rdx);
        a.lea(r9, asmjit::x86::ptr(rcx, -1));
        a.or_(r8, r9);
        a.test(r8.r8(), asmjit::Imm(7));
        a.jne(bail);
        if (op == 0x6E) {
            a.and_(rcx, rdx);    // bitAnd: tag bits stay 001
        } else {
            a.or_(rcx, rdx);     // bitOr: tag bits stay 001
        }
        a.mov(ptr(rax, -16), rcx);
        a.sub(rax, 8);
        a.mov(ptr(rdi, OFF_SP), rax);
        a.jmp(end);

        a.bind(bail);
        a.mov(r8, ptr(rdi, OFF_METHOD));
        a.add(r8, asmjit::Imm(bcOffsetFromMethObj));
        a.mov(ptr(rdi, OFF_IP), r8);
        a.mov(dword_ptr(rdi, OFF_EXIT),
              asmjit::Imm(EXIT_ARITH_OVERFLOW));
        a.ret();

        a.bind(end);
        return true;
    }
    // Phase 3 multiplication: 0x68 *.
    if (isPhase3MulOp(op)) {
        asmjit::Label bail = a.new_label();
        asmjit::Label end  = a.new_label();
        a.mov(rax, ptr(rdi, OFF_SP));
        a.mov(rcx, ptr(rax, -16));   // a
        a.mov(rdx, ptr(rax, -8));    // b
        // SmI check: (a^b) | (a-1) low 3 bits = 0 iff both SmI.
        a.mov(r8,  rcx);
        a.xor_(r8, rdx);
        a.lea(r9, asmjit::x86::ptr(rcx, -1));
        a.or_(r8, r9);
        a.test(r8.r8(), asmjit::Imm(7));
        a.jne(bail);

        // Untag both (arithmetic shift to preserve sign).
        a.sar(rcx, asmjit::Imm(3));
        a.sar(rdx, asmjit::Imm(3));
        // imul rcx, rdx — signed multiply, sets OF on overflow.
        a.imul(rcx, rdx);
        a.jo(bail);
        // Retag: shift left 3 + or 1.  Three add+jo to catch the
        // case where the result fits in 64-bit signed (no jo on imul)
        // but not in 61-bit signed (would overflow on retag).
        a.add(rcx, rcx); a.jo(bail);
        a.add(rcx, rcx); a.jo(bail);
        a.add(rcx, rcx); a.jo(bail);
        a.or_(rcx, asmjit::Imm(SMI_TAG));
        a.mov(ptr(rax, -16), rcx);
        a.sub(rax, 8);
        a.mov(ptr(rdi, OFF_SP), rax);
        a.jmp(end);

        a.bind(bail);
        a.mov(r8, ptr(rdi, OFF_METHOD));
        a.add(r8, asmjit::Imm(bcOffsetFromMethObj));
        a.mov(ptr(rdi, OFF_IP), r8);
        a.mov(dword_ptr(rdi, OFF_EXIT),
              asmjit::Imm(EXIT_ARITH_OVERFLOW));
        a.ret();

        a.bind(end);
        return true;
    }
    // Phase 3 bitShift: (0x6C).  Positive b → left shift with overflow
    // check; negative b → arithmetic right shift.  Matches interp's
    // bounds: b in [0,62] for left, b in [-63,-1] for right; else bail.
    if (isPhase3ShiftOp(op)) {
        asmjit::Label bail = a.new_label();
        asmjit::Label end  = a.new_label();
        asmjit::Label rightShift = a.new_label();
        a.mov(rax, ptr(rdi, OFF_SP));
        a.mov(rcx, ptr(rax, -16));   // a
        a.mov(rdx, ptr(rax, -8));    // b
        // SmI check: (a^b) | (a-1) low 3 bits = 0 iff both SmI.
        a.mov(r8,  rcx);
        a.xor_(r8, rdx);
        a.lea(r9, asmjit::x86::ptr(rcx, -1));
        a.or_(r8, r9);
        a.test(r8.r8(), asmjit::Imm(7));
        a.jne(bail);

        a.sar(rcx, asmjit::Imm(3));  // untag a
        a.sar(rdx, asmjit::Imm(3));  // untag b
        a.cmp(rdx, asmjit::Imm(0));
        a.jl(rightShift);            // b < 0 → right shift

        // Left shift: b in [0, 62].
        a.cmp(rdx, asmjit::Imm(63));
        a.jge(bail);
        // shl rcx, cl — but cl is rdx's low byte.  We need to move
        // shift count to cl.  rdx's low byte = dl (not cl).  Use the
        // movzx + use cl pattern instead.
        a.mov(r8, rcx);              // save original a in r8
        // x86 SHL with reg operand requires CL.  Temporarily save rcx
        // (= a, after untag), move shift count to cl via xchg with rdx.
        // Simpler: move rcx to r9 (untagged a), move dl to cl.
        a.mov(r9, rcx);              // r9 = a untagged
        a.mov(rcx, rdx);             // rcx = b untagged (now cl = b mod 256)
        a.shl(r9, asmjit::x86::cl);  // r9 = a << b
        // Overflow check: (r9 >> b) == a?
        a.mov(r10, r9);
        a.sar(r10, asmjit::x86::cl);
        a.cmp(r10, r8);
        a.jne(bail);
        a.mov(rcx, r9);              // result back to rcx
        // Retag with overflow checks.
        a.add(rcx, rcx); a.jo(bail);
        a.add(rcx, rcx); a.jo(bail);
        a.add(rcx, rcx); a.jo(bail);
        a.or_(rcx, asmjit::Imm(SMI_TAG));
        a.mov(ptr(rax, -16), rcx);
        a.sub(rax, 8);
        a.mov(ptr(rdi, OFF_SP), rax);
        a.jmp(end);

        a.bind(rightShift);
        // b in [-63, -1].  Negate to get shift count.
        a.neg(rdx);
        a.cmp(rdx, asmjit::Imm(63));
        a.jg(bail);
        a.mov(r9, rcx);              // r9 = a untagged
        a.mov(rcx, rdx);             // rcx = shift count (cl = -b)
        a.sar(r9, asmjit::x86::cl);  // arithmetic right shift
        a.mov(rcx, r9);
        // Retag: result is smaller than input, no overflow possible.
        a.shl(rcx, asmjit::Imm(3));
        a.or_(rcx, asmjit::Imm(SMI_TAG));
        a.mov(ptr(rax, -16), rcx);
        a.sub(rax, 8);
        a.mov(ptr(rdi, OFF_SP), rax);
        a.jmp(end);

        a.bind(bail);
        a.mov(r8, ptr(rdi, OFF_METHOD));
        a.add(r8, asmjit::Imm(bcOffsetFromMethObj));
        a.mov(ptr(rdi, OFF_IP), r8);
        a.mov(dword_ptr(rdi, OFF_EXIT),
              asmjit::Imm(EXIT_ARITH_OVERFLOW));
        a.ret();

        a.bind(end);
        return true;
    }
    // Short forward jumps (0xB0..0xC7).
    //   0xB0..0xB7 = unconditional
    //   0xB8..0xBF = jumpTrue  (pop; jump if was true)
    //   0xC0..0xC7 = jumpFalse (pop; jump if was false)
    if (op >= SistaV1::ShortJumpBase && op <= SistaV1::ShortJumpFalseLast) {
        int targetIdx = SistaV1::shortJumpTarget(op, globalIdx);
        if (op <= SistaV1::ShortJumpLast) {
            // Unconditional forward jump — safe even from inline-activated
            // contexts because no bail involved.
            a.jmp(bcLabels[targetIdx]);
            return true;
        }
        // Conditional jumps emit a mid-method ExitMustBool bail.  The
        // method gets canBailMidMethod=true in compileViaAsmjit so the
        // chain loop's inline-activate gate skips it (see
        // Interpreter.cpp:18731-18733).
        bool jumpOnTrue = (op >= SistaV1::ShortJumpTrueBase
                           && op <= SistaV1::ShortJumpTrueLast);
        asmjit::Label notBoolean = a.new_label();
        asmjit::Label fallThru   = a.new_label();

        a.mov(rcx, ptr(rdi, OFF_SP));
        a.mov(rax, ptr(rcx, -8));               // TOS (don't pop)

        // First cmp: against the "branch-taken" boolean.
        // For jumpTrue: branch when TOS == trueOop.
        // For jumpFalse: branch when TOS == falseOop.
        int takeBranchOop = jumpOnTrue ? OFF_TRUEOOP : OFF_FALSEOOP;
        int fallThruOop   = jumpOnTrue ? OFF_FALSEOOP : OFF_TRUEOOP;
        asmjit::Label takeBranch = a.new_label();
        a.cmp(rax, ptr(rdi, takeBranchOop));
        a.je(takeBranch);
        a.cmp(rax, ptr(rdi, fallThruOop));
        a.jne(notBoolean);

        // Was the fall-through boolean.  Pop and fall through.
        a.sub(rcx, 8);
        a.mov(ptr(rdi, OFF_SP), rcx);
        a.jmp(fallThru);

        a.bind(takeBranch);
        // Was the take-branch boolean.  Pop and jump.
        a.sub(rcx, 8);
        a.mov(ptr(rdi, OFF_SP), rcx);
        a.jmp(bcLabels[targetIdx]);

        a.bind(notBoolean);
        // Don't pop — interp re-runs and sends mustBeBoolean.
        a.mov(r8, ptr(rdi, OFF_METHOD));
        a.add(r8, asmjit::Imm(bcOffsetFromMethObj));
        a.mov(ptr(rdi, OFF_IP), r8);
        a.mov(dword_ptr(rdi, OFF_EXIT), asmjit::Imm(EXIT_MUST_BOOL));
        a.ret();

        a.bind(fallThru);
        return true;
    }
    if (isPhase4SendOp(op)) {
        int nArgs = sendNArgs(op);

        // Per-site icData address: jm->icBuffer + siteIdx*IC_BYTES_PER_SITE.
        a.mov(rdx, ptr(rdi, OFF_JITMETHOD));
        a.mov(rsi, ptr(rdx, (int)offsetof(JITMethod, icBuffer)));
        a.add(rsi, asmjit::Imm(siteIdx * (int)IC_BYTES_PER_SITE));

        // Deferred state setup: inline-spec paths (getter/setter/
        // returnsSelf) continue inline and don't need state.icDataPtr,
        // sendArgCount, or ip.  Only dispatchCached + miss paths return
        // to the chain loop; they emit the state stores at the end.
        // Saves 5 instructions per inline-spec HIT (the common case).

        // Selector-range bisect knob: PHARO_T1_IC_PROBE_MIN/MAX limit
        // which opcodes get the probe.  Default: all sends (0x70..0xAF).
        bool probeThis = g_debug.t1ICProbe;
        if (probeThis) {
            int icMin = g_debug.t1ICProbeMin;
            int icMax = g_debug.t1ICProbeMax;
            if (icMin >= 0 && (int)op < icMin) probeThis = false;
            if (icMax >= 0 && (int)op > icMax) probeThis = false;
        }
        if (probeThis) {
            // === Compute send-receiver's IC lookup key into rdx ===
            // The send's receiver is at stackValue(nArgs) = sp[-1-nArgs]
            // (NOT state.receiver, which is the *enclosing* method's self).
            // Object (tag=0):  classIndex = low 22 bits of header word.
            // Immediate:       key = (tag | 0x80000000) — matches
            //                  patchJITICAfterSend in Interpreter.cpp:16660.
            asmjit::Label imm = a.new_label();
            asmjit::Label haveKey = a.new_label();
            asmjit::Label miss = a.new_label();
            asmjit::Label dispatchCached = a.new_label();
            asmjit::Label tryGetter = a.new_label();
            asmjit::Label trySetter = a.new_label();
            asmjit::Label tryReturnsSelf = a.new_label();
            asmjit::Label endOfSend = a.new_label();

            a.mov(rcx, ptr(rdi, OFF_SP));
            int rcvrOffsetBytes = -8 * (nArgs + 1);
            a.mov(rax, ptr(rcx, rcvrOffsetBytes));   // rax = send receiver
            a.mov(rdx, rax);
            a.and_(rdx, asmjit::Imm(0x7));   // tag bits
            a.jnz(imm);
            // Object path: edx = header_low32 & 0x3FFFFF (classIndex).
            // 32-bit ops zero-extend into rdx, so the result is bounded.
            a.mov(edx, dword_ptr(rax));
            a.and_(edx, asmjit::Imm(0x3FFFFF));
            a.jmp(haveKey);
            a.bind(imm);
            a.or_(edx, asmjit::Imm(0x80000000));
            a.bind(haveKey);

            // === Probe icData[0] only (slot 0 monomorphic) ===
            a.cmp(rdx, ptr(rsi));
            a.jne(miss);
            a.cmp(rdx, asmjit::Imm(0));      // reject empty IC slot
            a.je(miss);
            if (g_debug.t1ProbeAlwaysMiss) {
                a.jmp(miss);                  // diagnostic: never take HIT
            }

            // === Examine extras (icData[2]) to choose dispatch ===
            // r8 = extras.  If 0 → plain dispatch via ExitSendCached.
            // Otherwise the patchJITICAfterSend classification chose an
            // inline-getter (bit 63), inline-setter (bit 62),
            // returnsSelf (bit 61), or one of the other flags we don't
            // inline yet (bit 60 J2J, 59 BLOCK_VALUE, 58 returnsLiteral,
            // 57 multi-slot, 48..52 primKind).  Unhandled extras →
            // dispatch the cached method and let the chain loop +
            // activated method do the work.
            //
            // ===== PERF TODO (deferred.md A6, 2026-05-17) =====
            // Bit 60 (J2J_ENTRY_BIT) inline-call is the largest unrealized
            // perf win — current arm64 fib(28) is 2× slower than interp
            // because every J2J hit round-trips JIT→C++→JIT via
            // activateMethod (~500 cycles per send overhead, ~80 ms over
            // fib's 514K recursive calls).  The legacy stencil JIT had this
            // inline at stencils.cpp:1733-1877 (`j2j_direct_call:` block);
            // see deferred.md A6 for the port plan and J2JSave protocol.
            // Same fix needed on both x86 and arm64 arms.
            // ====================================================
            a.mov(r8, qword_ptr(rsi, 16));
            a.test(r8, r8);
            a.jz(dispatchCached);

            // Inline specializations need a heap-class receiver
            // (the stencil's `tag == 0` gate). Immediates fall through
            // to plain cached dispatch.
            a.test(rax.r8(), asmjit::Imm(7));
            a.jnz(dispatchCached);

            if (g_debug.t1InlineGetter) {
                a.bt(r8, asmjit::Imm(63));
                a.jc(tryGetter);
            }
            if (g_debug.t1InlineSetter) {
                a.bt(r8, asmjit::Imm(62));
                a.jc(trySetter);
            }
            if (g_debug.t1InlineReturnsSelf) {
                a.bt(r8, asmjit::Imm(61));
                a.jc(tryReturnsSelf);
            }
            a.jmp(dispatchCached);

            // === Inline getter ===
            // val = receiver->slots[slotIdx]; sp[-1-nArgs] = val; sp -= nArgs.
            // For 0-arg getters (the common case) nArgs=0 → sp unchanged,
            // val replaces receiver at sp[-1].
            //
            // Note: rcx still holds SP from the probe entry (no clobber
            // through the probe path).  Use rdx for slotIdx so rcx stays
            // SP — saves 1 mov per inline-spec HIT.
            a.bind(tryGetter);
            a.mov(rdx, r8);
            a.and_(rdx, asmjit::Imm(0xFFFF));    // slotIdx in rdx
            a.mov(rdx, ptr(rax, rdx, 3, 8));     // load slot value into rdx
            a.mov(ptr(rcx, rcvrOffsetBytes), rdx);
            if (nArgs > 0) {
                a.sub(rcx, asmjit::Imm(8 * nArgs));
                a.mov(ptr(rdi, OFF_SP), rcx);
            }
            a.jmp(endOfSend);

            // === Inline setter ===
            // arg = sp[-1]; receiver->slots[slotIdx] = arg;
            // sp[-1-nArgs] = receiver (already true); sp -= nArgs.
            // No write barrier — the YG scavenge does a full old-space scan
            // (ObjectMemory.cpp:1538) so missed remembered-set updates are
            // tolerated.
            a.bind(trySetter);
            a.mov(rdx, r8);
            a.and_(rdx, asmjit::Imm(0xFFFF));    // slotIdx in rdx
            a.mov(r9, ptr(rcx, -8));             // arg = sp[-1] (rcx is SP)
            a.mov(ptr(rax, rdx, 3, 8), r9);      // recv->slot[slotIdx] = arg
            if (nArgs > 0) {
                a.sub(rcx, asmjit::Imm(8 * nArgs));
                a.mov(ptr(rdi, OFF_SP), rcx);
            }
            a.jmp(endOfSend);

            // === returnsSelf ===
            // Receiver is already at sp[-1-nArgs]; just pop nArgs and
            // it becomes TOS.  rcx still has SP from probe.
            a.bind(tryReturnsSelf);
            if (nArgs > 0) {
                a.sub(rcx, asmjit::Imm(8 * nArgs));
                a.mov(ptr(rdi, OFF_SP), rcx);
            }
            a.jmp(endOfSend);

            // === Plain cached dispatch (no inline opt) ===
            // Emit deferred state setup here (chain loop reads it).
            a.bind(dispatchCached);
            a.mov(ptr(rdi, OFF_ICDATAPTR), rsi);
            a.mov(dword_ptr(rdi, OFF_SENDARGCOUNT), asmjit::Imm(nArgs));
            a.mov(rax, ptr(rdi, OFF_METHOD));
            a.add(rax, asmjit::Imm(bcOffsetFromMethObj));
            a.mov(ptr(rdi, OFF_IP), rax);
            if (g_debug.t1HitAsMiss) {
                a.mov(dword_ptr(rdi, OFF_EXIT), asmjit::Imm(EXIT_SEND));
                a.ret();
            } else {
                a.mov(rax, ptr(rsi, 8));         // icData[1] = method Oop
                a.mov(ptr(rdi, OFF_CACHED_TARGET), rax);
                a.mov(dword_ptr(rdi, OFF_EXIT),
                      asmjit::Imm(EXIT_SEND_CACHED));
                a.ret();
            }

            // === Miss — chain loop does the IC fill ===
            // Emit deferred state setup here too.
            a.bind(miss);
            a.mov(ptr(rdi, OFF_ICDATAPTR), rsi);
            a.mov(dword_ptr(rdi, OFF_SENDARGCOUNT), asmjit::Imm(nArgs));
            a.mov(rax, ptr(rdi, OFF_METHOD));
            a.add(rax, asmjit::Imm(bcOffsetFromMethObj));
            a.mov(ptr(rdi, OFF_IP), rax);
            a.mov(dword_ptr(rdi, OFF_EXIT), asmjit::Imm(EXIT_SEND));
            a.ret();

            a.bind(endOfSend);
            return true;  // inline paths fall through to next bytecode
        }

        // Non-probe fallback: emit state setup + bail (chain loop will
        // do the full lookup).
        a.mov(ptr(rdi, OFF_ICDATAPTR), rsi);
        a.mov(dword_ptr(rdi, OFF_SENDARGCOUNT), asmjit::Imm(nArgs));
        a.mov(rax, ptr(rdi, OFF_METHOD));
        a.add(rax, asmjit::Imm(bcOffsetFromMethObj));
        a.mov(ptr(rdi, OFF_IP), rax);
        a.mov(dword_ptr(rdi, OFF_EXIT), asmjit::Imm(EXIT_SEND));
        a.ret();
        return true;
    }
    return false;  // pre-scan failed to filter — bug in allBytecodesSupported
}

#elif defined(__aarch64__) || defined(_M_ARM64)

// Equivalent ARM64 emitters.  Same stack discipline; uses w/x regs.
//   x0 = state ptr (input, preserved)
//   x1 = scratch value
//   x2 = scratch sp
//   w3 = scratch int (for exitReason store)

// ===== sp-residency Phase-1 sweep helpers (see PHARO_T1_SP_IN_X25
// at the top of the file).  Byte-identical codegen at =0; at =1 sp
// lives in x25 and these become register moves. =====
static inline void emitLoadSp(asmjit::a64::Assembler& a,
                              asmjit::a64::Gp dst) {
#if PHARO_T1_SP_IN_X25
    a.mov(dst, asmjit::a64::x25);
#else
    a.ldr(dst, asmjit::a64::ptr(asmjit::a64::x0, OFF_SP));
#endif
}
static inline void emitStoreSp(asmjit::a64::Assembler& a,
                               asmjit::a64::Gp src) {
#if PHARO_T1_SP_IN_X25
    a.mov(asmjit::a64::x25, src);
#else
    a.str(src, asmjit::a64::ptr(asmjit::a64::x0, OFF_SP));
#endif
}
// At exits (ret to C++) and before sp-reading helper BLRs, sp must be
// visible in memory regardless of mode.
static inline void emitSyncSpToState(asmjit::a64::Assembler& a) {
#if PHARO_T1_SP_IN_X25
    a.str(asmjit::a64::x25,
          asmjit::a64::ptr(asmjit::a64::x0, OFF_SP));
#endif
    // FSR M4: publish the active-method identity at every exit (the
    // 62 emitSyncSpToState callers cover all JIT->C++ edges).  One
    // store per EXIT replaces five mirror stores per CALL once the
    // per-call writers delete.  x19 invariant = M1, verified.
    if (g_fsrLazyVerify || g_fsrX19Verify) {
        // M1 oracle, exit-side: x19 must equal THIS method's JM at
        // every exit.  Self-identify PC-relatively (the lander trick:
        // JM header immediately precedes codeStart).  brk #0xF14 on
        // divergence — attach lldb on the DET_SCHED repro to see
        // which exit stub / return path left x19 stale.
        asmjit::Label x19exOk = a.new_label();
        a.adr(asmjit::a64::x16, g_codeStartLabel);
        a.sub(asmjit::a64::x16, asmjit::a64::x16,
              asmjit::Imm((int)sizeof(JITMethod)));
        a.cmp(asmjit::a64::x16, asmjit::a64::x19);
        a.b_eq(x19exOk);
        a.brk(0xF14);
        a.bind(x19exOk);
    }
    if (g_fsrLazy)
        a.str(asmjit::a64::x19,
              asmjit::a64::ptr(asmjit::a64::x0, 56 /*OFF_JITMETHOD*/));
#if PHARO_T1_TB_IN_X26
    a.str(asmjit::a64::x26,
          asmjit::a64::ptr(asmjit::a64::x0, OFF_TEMPBASE));
#endif
    (void)a;
}
// After a BLR to a helper that may CHANGE state.sp (sista dispatch,
// anything that can run Smalltalk / trigger materialize), reload the
// mirror.  Caller must have restored x0 = state first (every BLR
// wrapper in this file stashes/restores x0 around the call).
static inline void emitReloadSpFromState(asmjit::a64::Assembler& a) {
#if PHARO_T1_SP_IN_X25
    a.ldr(asmjit::a64::x25,
          asmjit::a64::ptr(asmjit::a64::x0, OFF_SP));
#endif
#if PHARO_T1_TB_IN_X26
    a.ldr(asmjit::a64::x26,
          asmjit::a64::ptr(asmjit::a64::x0, OFF_TEMPBASE));
#endif
    (void)a;
}

// ---- tempBase residency (x26): same shapes as the sp helpers ----
static inline void emitLoadTempBase(asmjit::a64::Assembler& a,
                                    asmjit::a64::Gp dst) {
#if PHARO_T1_TB_IN_X26
    a.mov(dst, asmjit::a64::x26);
#else
    a.ldr(dst, asmjit::a64::ptr(asmjit::a64::x0, OFF_TEMPBASE));
#endif
}
static inline void emitStoreTempBase(asmjit::a64::Assembler& a,
                                     asmjit::a64::Gp src) {
#if PHARO_T1_TB_IN_X26
    a.mov(asmjit::a64::x26, src);
#else
    a.str(src, asmjit::a64::ptr(asmjit::a64::x0, OFF_TEMPBASE));
#endif
}

// simStack producer pair: value is already IN x26; push it and mark
// the cache valid IN THE SAME CALL (the §3 pairing rule — bookkeeping
// and the x26 write can never drift apart).
static inline void emitTosPush(asmjit::a64::Assembler& a) {
    a.str(asmjit::a64::x26, asmjit::a64::ptr_post(asmjit::a64::x25, 8));
    g_tos.valid = true;
}
// VERIFY net (PHARO_T1_TOS_VERIFY): before any cache-consuming emit —
// x26 must equal memory TOS; brk on divergence (the deterministic net
// for the valid-but-stale direction).  x16 = IP0, purely local use.
static inline void emitTosVerify(asmjit::a64::Assembler& a) {
    if (!GET_DEBUG_BOOL(PHARO_T1_TOS_VERIFY)) return;
    using namespace asmjit::a64;
    asmjit::Label ok = a.new_label();
    a.ldur(x16, ptr(x25, -8));
    a.cmp(x16, x26);
    a.b_eq(ok);
    a.brk(0xDEA);
    a.bind(ok);
}

void emitPushReg(asmjit::a64::Assembler& a, asmjit::a64::Gp valReg) {
    using namespace asmjit::a64;
#if PHARO_T1_SP_IN_X25
    // sp is register-resident: a push is ONE post-index store.  The
    // old shape (mov x2,x25; str; add; mov x25,x2) survives at many
    // inline copies — converting those needs a per-site x2-liveness
    // audit (deferred to the simStack work, WIP.md 2026-06-11c).
    a.str(valReg, ptr_post(x25, 8));
#else
    emitLoadSp(a, x2);
    a.str(valReg, ptr(x2));
    a.add(x2, x2, asmjit::Imm(8));
    emitStoreSp(a, x2);
#endif
}

// ARM64 mirror of emitPrimProlog_x86.  See that function for context.
void emitPrimProlog_arm64(asmjit::a64::Assembler& a, int primIndex,
                          asmjit::Label prologRet) {
    using namespace asmjit::a64;

    if (primIndex == 110) {
        // #== : compare raw oop bits.
        a.ldr(x1, ptr(x0, OFF_RECEIVER));
        emitLoadTempBase(a, x2);
        a.ldr(x2, ptr(x2));
        // ldp loads two adjacent 8-byte slots in one instruction; TRUEOOP
        // (offset 128) and FALSEOOP (offset 136) are intentionally
        // adjacent in JITState.  Saves 1 ldr per #== prim.
        a.ldp(x6, x7, ptr(x0, OFF_TRUEOOP));
        a.cmp(x1, x2);
        a.csel(x1, x6, x7, CondCode::kEQ);
        a.str(x1, ptr(x0, OFF_RETVAL));
        a.mov(w3, asmjit::Imm(EXIT_RETURN));
        a.str(w3, ptr(x0, OFF_EXIT));
        emitSyncSpToState(a);
        // V2 J2J-return contract: the inline-J2J lander expects the
        // retval in x1 (the return prelude loads it; this plain ret
        // bypasses the prelude).  Exposed 2026-06-12 when prim-prologue
        // callees were admitted through the xmethod gates.
        a.ldr(x1, ptr(x0, OFF_RETVAL));
        a.b(prologRet);  // V2 J2J-return via the shared prelude shim
        return;
    }

    asmjit::Label fail = a.new_label();

    a.ldr(x1, ptr(x0, OFF_RECEIVER));

    // === Heap-receiver primitives (no SmI check) — at: / at:put: / size ===
    if (primIndex == 60 || primIndex == 61 || primIndex == 62) {
        // x1 = receiver (must be heap pointer).
        a.tst(x1, asmjit::Imm(7));
        a.b_ne(fail);
        // Load header word.
        a.ldr(x4, ptr(x1));
        // fmt = (hdr >> 24) & 0x1F
        a.lsr(x6, x4, asmjit::Imm(24));
        a.and_(x6, x6, asmjit::Imm(0x1F));
        // slotCount = (hdr >> 56) & 0xFF; if 0xFF read the overflow
        // word at rcvBits-8 for the real count (large objects ≥ 255
        // slots, e.g. SparseLargeTable with 901 slots).  Previously
        // bailed on overflow → basicSize returned 0 via fallthrough.
        a.lsr(x5, x4, asmjit::Imm(56));
        a.and_(x5, x5, asmjit::Imm(0xFF));
        {
            asmjit::Label noOverflow = a.new_label();
            a.cmp(x5, asmjit::Imm(255));
            a.b_ne(noOverflow);
            // Overflow: real count is in the 8 bytes before the header.
            // The high byte is 0xFF (overflow marker); mask it off.
            a.ldur(x5, ptr(x1, -8));
            a.lsl(x5, x5, asmjit::Imm(8));
            a.lsr(x5, x5, asmjit::Imm(8));
            a.bind(noOverflow);
        }

        if (primIndex == 62) {
            // size — fmt 2 (Array), fmt 10-11 (32-bit WordArray), or 16-23 (byte).
            asmjit::Label tryWords = a.new_label();
            asmjit::Label tryBytes = a.new_label();
            a.cmp(x6, asmjit::Imm(2));
            a.b_ne(tryWords);
            // Array: result = slotCount tagged.
            a.lsl(x5, x5, asmjit::Imm(3));
            a.orr(x5, x5, asmjit::Imm(1));
            a.str(x5, ptr(x0, OFF_RETVAL));
            a.mov(w3, asmjit::Imm(EXIT_RETURN));
            a.str(w3, ptr(x0, OFF_EXIT));
            emitSyncSpToState(a);
            // V2 J2J-return contract: the inline-J2J lander expects the
            // retval in x1 (the return prelude loads it; this plain ret
            // bypasses the prelude).  Exposed 2026-06-12 when prim-prologue
            // callees were admitted through the xmethod gates.
            a.ldr(x1, ptr(x0, OFF_RETVAL));
            a.b(prologRet);  // V2 J2J-return via the shared prelude shim
            a.bind(tryWords);
            // fmt 10-11: 32-bit indexable (WordArray, IntegerArray).
            // numElements = slotCount*2 - (fmt - 10).
            a.sub(x8, x6, asmjit::Imm(10));
            a.cmp(x8, asmjit::Imm(2));
            a.b_hs(tryBytes);
            a.lsl(x5, x5, asmjit::Imm(1));         // sc*2
            a.sub(x5, x5, x8);                       // numElements
            a.lsl(x5, x5, asmjit::Imm(3));
            a.orr(x5, x5, asmjit::Imm(1));
            a.str(x5, ptr(x0, OFF_RETVAL));
            a.mov(w3, asmjit::Imm(EXIT_RETURN));
            a.str(w3, ptr(x0, OFF_EXIT));
            emitSyncSpToState(a);
            // V2 J2J-return contract: the inline-J2J lander expects the
            // retval in x1 (the return prelude loads it; this plain ret
            // bypasses the prelude).  Exposed 2026-06-12 when prim-prologue
            // callees were admitted through the xmethod gates.
            a.ldr(x1, ptr(x0, OFF_RETVAL));
            a.b(prologRet);  // V2 J2J-return via the shared prelude shim
            a.bind(tryBytes);
            // fmt - 16 in [0..7] → byte indexable.
            asmjit::Label tryFmt9 = a.new_label();
            a.sub(x8, x6, asmjit::Imm(16));
            a.cmp(x8, asmjit::Imm(8));
            a.b_hs(tryFmt9);
            // byteSize = slotCount*8 - (fmt & 7)
            a.lsl(x5, x5, asmjit::Imm(3));
            a.and_(x6, x6, asmjit::Imm(0x7));
            a.sub(x5, x5, x6);
            // Tag as SmI.
            a.lsl(x5, x5, asmjit::Imm(3));
            a.orr(x5, x5, asmjit::Imm(1));
            a.str(x5, ptr(x0, OFF_RETVAL));
            a.mov(w3, asmjit::Imm(EXIT_RETURN));
            a.str(w3, ptr(x0, OFF_EXIT));
            emitSyncSpToState(a);
            // V2 J2J-return contract: the inline-J2J lander expects the
            // retval in x1 (the return prelude loads it; this plain ret
            // bypasses the prelude).  Exposed 2026-06-12 when prim-prologue
            // callees were admitted through the xmethod gates.
            a.ldr(x1, ptr(x0, OFF_RETVAL));
            a.b(prologRet);  // V2 J2J-return via the shared prelude shim
            a.bind(tryFmt9);
            // fmt 9 (Indexable64): size = slotCount.
            asmjit::Label tryFmt345 = a.new_label();
            a.cmp(x6, asmjit::Imm(9));
            a.b_ne(tryFmt345);
            a.lsl(x5, x5, asmjit::Imm(3));
            a.orr(x5, x5, asmjit::Imm(1));
            a.str(x5, ptr(x0, OFF_RETVAL));
            a.mov(w3, asmjit::Imm(EXIT_RETURN));
            a.str(w3, ptr(x0, OFF_EXIT));
            emitSyncSpToState(a);
            // V2 J2J-return contract: the inline-J2J lander expects the
            // retval in x1 (the return prelude loads it; this plain ret
            // bypasses the prelude).  Exposed 2026-06-12 when prim-prologue
            // callees were admitted through the xmethod gates.
            a.ldr(x1, ptr(x0, OFF_RETVAL));
            a.b(prologRet);  // V2 J2J-return via the shared prelude shim
            // fmt 3/4/5 (IndexableWithFixed / Weak): call helper that
            // reads fixedFields from the class's instSpec slot.  Without
            // this branch, SparseLargeTable>>basicSize returned 0 (the
            // source-level "primitive failed" fallback `^0`) under JIT,
            // breaking every Unicode classification (isLetter / numArgs /
            // OpalCompiler arity check) downstream of GeneralCategory.
            a.bind(tryFmt345);
            a.sub(x8, x6, asmjit::Imm(3));
            a.cmp(x8, asmjit::Imm(3));
            a.b_hs(fail);
            // Save x0 (state) + LR; call jit_rt_primsize_ptr(state, rcvBits,
            // &state.returnValue).  Helper writes untagged size to *out
            // and returns 1 on success.
            a.sub(sp, sp, asmjit::Imm(16));
            a.str(x0,  ptr(sp, 0));
            a.str(x30, ptr(sp, 8));
            a.add(x2, x0, asmjit::Imm(OFF_RETVAL));
            emitSyncSpToState(a);  // sp-residency: helper reads state.sp
            a.mov(x9, asmjit::Imm((uint64_t)&jit_rt_primsize_ptr));
            a.blr(x9);
            a.mov(x9, x0);   // result (1 = success)
            a.ldr(x0,  ptr(sp, 0));
            emitReloadSpFromState(a);  // sp-residency: helper may have changed state.sp
            a.ldr(x30, ptr(sp, 8));
            a.add(sp, sp, asmjit::Imm(16));
            a.cbz(x9, fail);
            a.ldr(x5, ptr(x0, OFF_RETVAL));
            a.lsl(x5, x5, asmjit::Imm(3));
            a.orr(x5, x5, asmjit::Imm(1));
            a.str(x5, ptr(x0, OFF_RETVAL));
            a.mov(w3, asmjit::Imm(EXIT_RETURN));
            a.str(w3, ptr(x0, OFF_EXIT));
            emitSyncSpToState(a);
            // V2 J2J-return contract: the inline-J2J lander expects the
            // retval in x1 (the return prelude loads it; this plain ret
            // bypasses the prelude).  Exposed 2026-06-12 when prim-prologue
            // callees were admitted through the xmethod gates.
            a.ldr(x1, ptr(x0, OFF_RETVAL));
            a.b(prologRet);  // V2 J2J-return via the shared prelude shim
            a.bind(fail);
            return;
        }

        // prim 60 / 61: load idx (tagged) from first temp.
        emitLoadTempBase(a, x2);
        a.ldr(x3, ptr(x2));                  // x3 = idx
        a.and_(x7, x3, asmjit::Imm(0x7));
        a.cmp(x7, asmjit::Imm(1));
        a.b_ne(fail);                         // idx not SmI

        if (primIndex == 60) {
            // at: — fmt 2 (Array), fmt 10-11 (32-bit WordArray), or 16-23 (byte).
            asmjit::Label tryWordsAt = a.new_label();
            asmjit::Label tryBytesAt = a.new_label();
            // 2026-06-12 (sieve-bug root cause): the fmt-3/4/5/9 helper
            // block below was DEAD CODE — the byte-range miss branched
            // to `fail`, so fmt-3 receivers (LayoutClassScope etc.)
            // entered the Smalltalk fallback BODY, which assumes the
            // full primitive was attempted and raises spurious
            // SubscriptOutOfBounds.  That error cascade is what the
            // 2026-05-19 "sieve gate" was masking (the cond-jump
            // predicate selected error-raising bodies by accident).
            // Mirror prim 62's chain: range-miss -> helper block.
            asmjit::Label tryPtrAt = a.new_label();
            a.cmp(x6, asmjit::Imm(2));
            a.b_ne(tryWordsAt);
            a.asr(x4, x3, asmjit::Imm(3));   // idx untagged
            a.cmp(x4, asmjit::Imm(1));
            a.b_lt(fail);
            a.cmp(x4, x5);
            a.b_gt(fail);
            // recv[idx*8] = slot at index idx-1 (offset 8 + (idx-1)*8 = idx*8).
            a.lsl(x4, x4, asmjit::Imm(3));
            a.add(x6, x1, x4);
            a.ldr(x4, ptr(x6));
            a.str(x4, ptr(x0, OFF_RETVAL));
            a.mov(w3, asmjit::Imm(EXIT_RETURN));
            a.str(w3, ptr(x0, OFF_EXIT));
            emitSyncSpToState(a);
            // V2 J2J-return contract: the inline-J2J lander expects the
            // retval in x1 (the return prelude loads it; this plain ret
            // bypasses the prelude).  Exposed 2026-06-12 when prim-prologue
            // callees were admitted through the xmethod gates.
            a.ldr(x1, ptr(x0, OFF_RETVAL));
            a.b(prologRet);  // V2 J2J-return via the shared prelude shim
            a.bind(tryWordsAt);
            // fmt 10-11: 32-bit indexable (WordArray, IntegerArray).
            // numElements = slotCount*2 - (fmt - 10).  Load uint32, tag SmI.
            a.sub(x8, x6, asmjit::Imm(10));
            a.cmp(x8, asmjit::Imm(2));
            a.b_hs(tryBytesAt);
            a.lsl(x9, x5, asmjit::Imm(1));         // sc*2
            a.sub(x9, x9, x8);                      // numElements
            a.asr(x4, x3, asmjit::Imm(3));          // idx
            a.cmp(x4, asmjit::Imm(1));
            a.b_lt(fail);
            a.cmp(x4, x9);
            a.b_gt(fail);
            // 32-bit slot at recv + 4 + idx*4 (slot[0]=offset 8, slot[idx-1]=offset 8+(idx-1)*4).
            a.lsl(x4, x4, asmjit::Imm(2));
            a.add(x6, x1, x4);
            a.ldur(w7, ptr(x6, 4));
            // Tag as SmI: ((uint64)w7 << 3) | 1.
            a.lsl(x7, x7, asmjit::Imm(3));
            a.orr(x7, x7, asmjit::Imm(1));
            a.str(x7, ptr(x0, OFF_RETVAL));
            a.mov(w3, asmjit::Imm(EXIT_RETURN));
            a.str(w3, ptr(x0, OFF_EXIT));
            emitSyncSpToState(a);
            // V2 J2J-return contract: the inline-J2J lander expects the
            // retval in x1 (the return prelude loads it; this plain ret
            // bypasses the prelude).  Exposed 2026-06-12 when prim-prologue
            // callees were admitted through the xmethod gates.
            a.ldr(x1, ptr(x0, OFF_RETVAL));
            a.b(prologRet);  // V2 J2J-return via the shared prelude shim
            a.bind(tryBytesAt);
            a.sub(x8, x6, asmjit::Imm(16));
            a.cmp(x8, asmjit::Imm(8));
            a.b_hs(tryPtrAt);
            // byteSize = slotCount*8 - (fmt & 7)
            a.lsl(x5, x5, asmjit::Imm(3));
            a.and_(x9, x6, asmjit::Imm(0x7));
            a.sub(x5, x5, x9);
            a.asr(x4, x3, asmjit::Imm(3));
            a.cmp(x4, asmjit::Imm(1));
            a.b_lt(fail);
            a.cmp(x4, x5);
            a.b_gt(fail);
            // load byte at recv + 7 + idx (because slot[0] at +8, idx 1 = +8 = recv+7+1).
            a.add(x4, x4, asmjit::Imm(7));
            a.add(x6, x1, x4);
            a.ldrb(w7, ptr(x6));
            // Tag as SmI.
            a.lsl(x7, x7, asmjit::Imm(3));
            a.orr(x7, x7, asmjit::Imm(1));
            a.str(x7, ptr(x0, OFF_RETVAL));
            a.mov(w3, asmjit::Imm(EXIT_RETURN));
            a.str(w3, ptr(x0, OFF_EXIT));
            emitSyncSpToState(a);
            // V2 J2J-return contract: the inline-J2J lander expects the
            // retval in x1 (the return prelude loads it; this plain ret
            // bypasses the prelude).  Exposed 2026-06-12 when prim-prologue
            // callees were admitted through the xmethod gates.
            a.ldr(x1, ptr(x0, OFF_RETVAL));
            a.b(prologRet);  // V2 J2J-return via the shared prelude shim
            // fmt 3/4/5/9 (IndexableWithFixed / Weak / Indexable64) — call
            // jit_rt_primat_ptr helper.  Same dual-path trap as basicSize:
            // without this branch, LayoutClassScope.at: (fmt 3) fell
            // through to the bytecode body's "primitiveFailed" path which
            // raised a confusing ArgumentsCountMismatch instead of doing
            // the right thing for fmt-3 receivers.
            {
                a.bind(tryPtrAt);
                // No fmt pre-test: jit_rt_primat_ptr now routes any
                // format it can't fast-path through the FULL primitive
                // (jitPrimAtFull) and only returns 0 on genuine
                // failure — the body entry is then semantically right.
                // Untag idx (x3 has tagged), save x1 (rcvr) for helper call.
                a.asr(x4, x3, asmjit::Imm(3));   // x4 = untagged idx
                // Helper signature: int(JITState* s, uint64_t rcvBits, uint64_t i, uint64_t* out)
                //   x0 in: state (already); x1 in: rcvBits (already);
                //   x2 in: untagged idx; x3 in: &OFF_RETVAL
                a.sub(sp, sp, asmjit::Imm(16));
                a.str(x0,  ptr(sp, 0));
                a.str(x30, ptr(sp, 8));
                a.mov(x2, x4);                   // x2 = idx
                a.add(x3, x0, asmjit::Imm(OFF_RETVAL));
                emitSyncSpToState(a);  // sp-residency: helper reads state.sp
                a.mov(x9, asmjit::Imm((uint64_t)&jit_rt_primat_ptr));
                a.blr(x9);
                a.mov(x9, x0);
                a.ldr(x0,  ptr(sp, 0));
                emitReloadSpFromState(a);  // sp-residency: helper may have changed state.sp
                a.ldr(x30, ptr(sp, 8));
                a.add(sp, sp, asmjit::Imm(16));
                a.cbz(x9, fail);
                // jit_rt_primat_ptr wrote the result oop directly into
                // *out (already tagged for SmI / pointer).  Set EXIT_RETURN.
                a.mov(w3, asmjit::Imm(EXIT_RETURN));
                a.str(w3, ptr(x0, OFF_EXIT));
                emitSyncSpToState(a);
                // V2 J2J-return contract: the inline-J2J lander expects the
                // retval in x1 (the return prelude loads it; this plain ret
                // bypasses the prelude).  Exposed 2026-06-12 when prim-prologue
                // callees were admitted through the xmethod gates.
                a.ldr(x1, ptr(x0, OFF_RETVAL));
                a.b(prologRet);  // V2 J2J-return via the shared prelude shim
            }
            a.bind(fail);
            if (pharo::g_debug.jitFailReasons) {
                a.sub(sp, sp, asmjit::Imm(16));
                a.str(x0,  ptr(sp, 0));
                a.str(x30, ptr(sp, 8));
                a.mov(x1, asmjit::Imm(60));
                a.mov(x9, asmjit::Imm((uint64_t)&jit_rt_prim_body_entry));
                a.blr(x9);
                a.ldr(x0,  ptr(sp, 0));
                a.ldr(x30, ptr(sp, 8));
                a.add(sp, sp, asmjit::Imm(16));
            }
            return;
        }

        // primIndex == 61: at:put:.  Two args: idx (x3), val (load from temp+8).
        if (primIndex == 61) {
            // Immutability check (bit 23).
            a.tbnz(x4, asmjit::Imm(23), fail);
            a.ldr(x9, ptr(x2, 8));            // x9 = val (tagged)
            asmjit::Label tryWordsAtPut = a.new_label();
            asmjit::Label tryBytesAtPut = a.new_label();
            // 2026-06-12: same dead-helper fix as prim 60 above.
            asmjit::Label tryPtrAtPut = a.new_label();
            a.cmp(x6, asmjit::Imm(2));
            a.b_ne(tryWordsAtPut);
            // Array path.
            a.asr(x4, x3, asmjit::Imm(3));
            a.cmp(x4, asmjit::Imm(1));
            a.b_lt(fail);
            a.cmp(x4, x5);
            a.b_gt(fail);
            a.lsl(x4, x4, asmjit::Imm(3));
            a.add(x6, x1, x4);
            a.str(x9, ptr(x6));               // store value at slot
            a.str(x9, ptr(x0, OFF_RETVAL));   // return value
            a.mov(w3, asmjit::Imm(EXIT_RETURN));
            a.str(w3, ptr(x0, OFF_EXIT));
            emitSyncSpToState(a);
            // V2 J2J-return contract: the inline-J2J lander expects the
            // retval in x1 (the return prelude loads it; this plain ret
            // bypasses the prelude).  Exposed 2026-06-12 when prim-prologue
            // callees were admitted through the xmethod gates.
            a.ldr(x1, ptr(x0, OFF_RETVAL));
            a.b(prologRet);  // V2 J2J-return via the shared prelude shim
            a.bind(tryWordsAtPut);
            // fmt 10-11: 32-bit indexable (WordArray, IntegerArray).
            // val must be SmI in [0, 0xFFFFFFFF].
            a.sub(x8, x6, asmjit::Imm(10));
            a.cmp(x8, asmjit::Imm(2));
            a.b_hs(tryBytesAtPut);
            // val SmI tag check.
            a.and_(x7, x9, asmjit::Imm(0x7));
            a.cmp(x7, asmjit::Imm(1));
            a.b_ne(fail);
            a.asr(x7, x9, asmjit::Imm(3));    // val untagged (signed)
            // val < 0 → fail; val > 0xFFFFFFFF → fail.
            a.cmp(x7, asmjit::Imm(0));
            a.b_lt(fail);
            a.mov(x10, asmjit::Imm(0xFFFFFFFFULL));
            a.cmp(x7, x10);
            a.b_hi(fail);
            // numElements = slotCount*2 - (fmt - 10).
            a.lsl(x10, x5, asmjit::Imm(1));   // sc*2
            a.sub(x10, x10, x8);              // numElements
            a.asr(x4, x3, asmjit::Imm(3));    // idx
            a.cmp(x4, asmjit::Imm(1));
            a.b_lt(fail);
            a.cmp(x4, x10);
            a.b_gt(fail);
            // Store uint32 at recv + 4 + idx*4.
            a.lsl(x4, x4, asmjit::Imm(2));
            a.add(x6, x1, x4);
            a.stur(w7, ptr(x6, 4));
            a.str(x9, ptr(x0, OFF_RETVAL));   // return value
            a.mov(w3, asmjit::Imm(EXIT_RETURN));
            a.str(w3, ptr(x0, OFF_EXIT));
            emitSyncSpToState(a);
            // V2 J2J-return contract: the inline-J2J lander expects the
            // retval in x1 (the return prelude loads it; this plain ret
            // bypasses the prelude).  Exposed 2026-06-12 when prim-prologue
            // callees were admitted through the xmethod gates.
            a.ldr(x1, ptr(x0, OFF_RETVAL));
            a.b(prologRet);  // V2 J2J-return via the shared prelude shim
            a.bind(tryBytesAtPut);
            a.sub(x8, x6, asmjit::Imm(16));
            a.cmp(x8, asmjit::Imm(8));
            a.b_hs(tryPtrAtPut);
            // val must be SmI in 0..255.
            a.and_(x7, x9, asmjit::Imm(0x7));
            a.cmp(x7, asmjit::Imm(1));
            a.b_ne(fail);
            a.asr(x7, x9, asmjit::Imm(3));
            a.cmp(x7, asmjit::Imm(255));
            a.b_hi(fail);
            // byteSize = slotCount*8 - (fmt & 7)
            a.lsl(x5, x5, asmjit::Imm(3));
            a.and_(x10, x6, asmjit::Imm(0x7));
            a.sub(x5, x5, x10);
            a.asr(x4, x3, asmjit::Imm(3));
            a.cmp(x4, asmjit::Imm(1));
            a.b_lt(fail);
            a.cmp(x4, x5);
            a.b_gt(fail);
            a.add(x4, x4, asmjit::Imm(7));
            a.add(x6, x1, x4);
            a.strb(w7, ptr(x6));
            a.str(x9, ptr(x0, OFF_RETVAL));
            a.mov(w3, asmjit::Imm(EXIT_RETURN));
            a.str(w3, ptr(x0, OFF_EXIT));
            emitSyncSpToState(a);
            // V2 J2J-return contract: the inline-J2J lander expects the
            // retval in x1 (the return prelude loads it; this plain ret
            // bypasses the prelude).  Exposed 2026-06-12 when prim-prologue
            // callees were admitted through the xmethod gates.
            a.ldr(x1, ptr(x0, OFF_RETVAL));
            a.b(prologRet);  // V2 J2J-return via the shared prelude shim
            // fmt 3/4/5 — call jit_rt_primatput_ptr helper.  Same dual-
            // path trap as basicSize/at:.
            {
                a.bind(tryPtrAtPut);
                // No fmt pre-test — see tryPtrAt above.
                // SmI tag check on idx (x3 was tagged).
                a.and_(x7, x3, asmjit::Imm(0x7));
                a.cmp(x7, asmjit::Imm(1));
                a.b_ne(fail);
                a.asr(x4, x3, asmjit::Imm(3));   // untagged idx
                // Helper: int(state, rcvBits, i, valBits)
                //   x0 state (already); x1 rcv (already); x2 idx; x3 valBits
                a.sub(sp, sp, asmjit::Imm(16));
                a.str(x0,  ptr(sp, 0));
                a.str(x30, ptr(sp, 8));
                a.mov(x2, x4);
                a.mov(x3, x9);                   // valBits (already loaded)
                emitSyncSpToState(a);  // sp-residency: helper reads state.sp
                a.mov(x10, asmjit::Imm((uint64_t)&jit_rt_primatput_ptr));
                a.blr(x10);
                a.mov(x10, x0);
                a.ldr(x0,  ptr(sp, 0));
                emitReloadSpFromState(a);  // sp-residency: helper may have changed state.sp
                a.ldr(x30, ptr(sp, 8));
                a.add(sp, sp, asmjit::Imm(16));
                a.cbz(x10, fail);
                // Return the stored value (still in x9 — saved before call?).
                // Actually x9 is caller-saved; reload val from temp.
                emitLoadTempBase(a, x2);
                a.ldr(x9, ptr(x2, 8));
                a.str(x9, ptr(x0, OFF_RETVAL));
                a.mov(w3, asmjit::Imm(EXIT_RETURN));
                a.str(w3, ptr(x0, OFF_EXIT));
                emitSyncSpToState(a);
                // V2 J2J-return contract: the inline-J2J lander expects the
                // retval in x1 (the return prelude loads it; this plain ret
                // bypasses the prelude).  Exposed 2026-06-12 when prim-prologue
                // callees were admitted through the xmethod gates.
                a.ldr(x1, ptr(x0, OFF_RETVAL));
                a.b(prologRet);  // V2 J2J-return via the shared prelude shim
            }
            a.bind(fail);
            return;
        }
    }

    emitLoadTempBase(a, x2);
    a.ldr(x2, ptr(x2));

    // === SmallFloat binary arith (prim 541/542/549) ===
    // Both operands must be SmallFloat (tag = 5).  Decode, op, encode.
    // Bail on ±0 receiver/arg (uncommon, complex encoding) and on
    // exponent under/overflow of the result.
    if (primIndex == 541 || primIndex == 542 || primIndex == 549) {
        // Tag-check both operands == 5.
        a.and_(x4, x1, asmjit::Imm(0x7));
        a.cmp(x4, asmjit::Imm(5));
        a.b_ne(fail);
        a.and_(x4, x2, asmjit::Imm(0x7));
        a.cmp(x4, asmjit::Imm(5));
        a.b_ne(fail);

        // Decode receiver.
        a.lsr(x4, x1, asmjit::Imm(3));
        a.cmp(x4, asmjit::Imm(1));
        a.b_ls(fail);                        // ±0 → bail
        a.mov(x5, asmjit::Imm(0x7000000000000000ULL));
        a.add(x4, x4, x5);
        a.ror(x4, x4, asmjit::Imm(1));       // doubleBits
        a.fmov(d0, x4);

        // Decode arg (x5 still holds offset).
        a.lsr(x4, x2, asmjit::Imm(3));
        a.cmp(x4, asmjit::Imm(1));
        a.b_ls(fail);
        a.add(x4, x4, x5);
        a.ror(x4, x4, asmjit::Imm(1));
        a.fmov(d1, x4);

        switch (primIndex) {
            case 541: a.fadd(d0, d0, d1); break;
            case 542: a.fsub(d0, d0, d1); break;
            case 549: a.fmul(d0, d0, d1); break;
        }

        // Encode result.
        a.fmov(x4, d0);
        // Special NaN/inf: exponent bits all 1 → high 11 bits after sign-LSB
        // rotation form a value ≥ 0xFFE0_0000_0000_0000 in the rotated form.
        // Range check after subtract catches these.
        a.ror(x4, x4, asmjit::Imm(63));      // ROL by 1 (sign bit → LSB)
        a.cmp(x4, x5);
        a.b_lo(fail);                         // exp underflow
        a.sub(x4, x4, x5);
        a.mov(x6, asmjit::Imm(0x1FFFFFFFFFFFFFFFULL));
        a.cmp(x4, x6);
        a.b_hi(fail);                         // exp overflow
        a.lsl(x4, x4, asmjit::Imm(3));
        // SmallFloatTag (5).  NOTE: `orr xN, xN, #5` is NOT a valid AArch64
        // bitmask immediate (5=0b101 is two non-contiguous 1-bits), so asmjit
        // silently emits nothing and the result keeps tag 0 — a corrupt
        // SmallFloat that the VM later mis-reads as a heap pointer.  After
        // `lsl #3` the low 3 bits are 0, so `add #5` (a valid ADD-immediate)
        // sets the tag identically.
        a.add(x4, x4, asmjit::Imm(5));        // SmallFloatTag
        a.str(x4, ptr(x0, OFF_RETVAL));
        a.mov(w3, asmjit::Imm(EXIT_RETURN));
        a.str(w3, ptr(x0, OFF_EXIT));
        emitSyncSpToState(a);
        // V2 J2J-return contract: the inline-J2J lander expects the
        // retval in x1 (the return prelude loads it; this plain ret
        // bypasses the prelude).  Exposed 2026-06-12 when prim-prologue
        // callees were admitted through the xmethod gates.
        a.ldr(x1, ptr(x0, OFF_RETVAL));
        a.b(prologRet);  // V2 J2J-return via the shared prelude shim
        a.bind(fail);
        return;
    }

    // SmI check (5 ops instead of 7): (a^b) | (a-1), low 3 bits = 0
    // iff same tag AND a is SmI.  Mirrors emitPrimProlog_x86.
    a.eor(x5, x1, x2);
    a.sub(x6, x1, asmjit::Imm(1));
    a.orr(x5, x5, x6);
    a.tst(x5, asmjit::Imm(7));
    a.b_ne(fail);

    if (primIndex >= 3 && primIndex <= 8) {
        // Compare tagged bits directly — x → 8x+1 is monotonic for
        // signed values, so cmp(a_bits, b_bits) matches cmp(a, b).
        // (Mirrors emitPrimProlog_x86 — skips the untag step.)
        // ldp loads both adjacent oop slots in one instruction.
        a.ldp(x6, x7, ptr(x0, OFF_TRUEOOP));
        a.cmp(x1, x2);
        CondCode cc = CondCode::kEQ;  // default; overridden below
        switch (primIndex) {
            case 3: cc = CondCode::kLT; break;  // signed <
            case 4: cc = CondCode::kGT; break;  // signed >
            case 5: cc = CondCode::kLE; break;  // signed <=
            case 6: cc = CondCode::kGE; break;  // signed >=
            case 7: cc = CondCode::kEQ; break;  // =
            case 8: cc = CondCode::kNE; break;  // ~=
        }
        a.csel(x1, x6, x7, cc);
        a.str(x1, ptr(x0, OFF_RETVAL));
        a.mov(w3, asmjit::Imm(EXIT_RETURN));
        a.str(w3, ptr(x0, OFF_EXIT));
        emitSyncSpToState(a);
        // V2 J2J-return contract: the inline-J2J lander expects the
        // retval in x1 (the return prelude loads it; this plain ret
        // bypasses the prelude).  Exposed 2026-06-12 when prim-prologue
        // callees were admitted through the xmethod gates.
        a.ldr(x1, ptr(x0, OFF_RETVAL));
        a.b(prologRet);  // V2 J2J-return via the shared prelude shim
        a.bind(fail);
        return;
    }

    if (primIndex == 14 || primIndex == 15 || primIndex == 16) {
        // bitAnd / bitOr / bitXor on tagged bits — see x86 version.
        if (primIndex == 14) a.and_(x1, x1, x2);
        else if (primIndex == 15) a.orr (x1, x1, x2);
        else {
            a.eor(x1, x1, x2);
            a.orr(x1, x1, asmjit::Imm(SMI_TAG));
        }
        a.str(x1, ptr(x0, OFF_RETVAL));
        a.mov(w3, asmjit::Imm(EXIT_RETURN));
        a.str(w3, ptr(x0, OFF_EXIT));
        emitSyncSpToState(a);
        // V2 J2J-return contract: the inline-J2J lander expects the
        // retval in x1 (the return prelude loads it; this plain ret
        // bypasses the prelude).  Exposed 2026-06-12 when prim-prologue
        // callees were admitted through the xmethod gates.
        a.ldr(x1, ptr(x0, OFF_RETVAL));
        a.b(prologRet);  // V2 J2J-return via the shared prelude shim
        a.bind(fail);
        return;
    }

    // Tagged-arith for + and - : skip untag/retag.  See x86 prim
    // prologue for derivation.  jo on the tagged add/sub catches
    // SmI-range overflow because the tagged form is (val<<3)|1.
    if (primIndex == 1) {
        // a_bits + b_bits = (a+b)*8 + 2 → sub 1 to retag.
        a.adds(x1, x1, x2);
        a.b_vs(fail);
        a.sub(x1, x1, asmjit::Imm(1));
        a.str(x1, ptr(x0, OFF_RETVAL));
        a.mov(w3, asmjit::Imm(EXIT_RETURN));
        a.str(w3, ptr(x0, OFF_EXIT));
        emitSyncSpToState(a);
        // V2 J2J-return contract: the inline-J2J lander expects the
        // retval in x1 (the return prelude loads it; this plain ret
        // bypasses the prelude).  Exposed 2026-06-12 when prim-prologue
        // callees were admitted through the xmethod gates.
        a.ldr(x1, ptr(x0, OFF_RETVAL));
        a.b(prologRet);  // V2 J2J-return via the shared prelude shim
        a.bind(fail);
        return;
    }
    if (primIndex == 2) {
        // a_bits - b_bits = (a-b)*8 → add 1 to retag.
        a.subs(x1, x1, x2);
        a.b_vs(fail);
        a.add(x1, x1, asmjit::Imm(1));
        a.str(x1, ptr(x0, OFF_RETVAL));
        a.mov(w3, asmjit::Imm(EXIT_RETURN));
        a.str(w3, ptr(x0, OFF_EXIT));
        emitSyncSpToState(a);
        // V2 J2J-return contract: the inline-J2J lander expects the
        // retval in x1 (the return prelude loads it; this plain ret
        // bypasses the prelude).  Exposed 2026-06-12 when prim-prologue
        // callees were admitted through the xmethod gates.
        a.ldr(x1, ptr(x0, OFF_RETVAL));
        a.b(prologRet);  // V2 J2J-return via the shared prelude shim
        a.bind(fail);
        return;
    }

    // prim 9 (*): untag, multiply, check overflow, retag.
    a.asr(x1, x1, asmjit::Imm(3));
    a.asr(x2, x2, asmjit::Imm(3));
    if (primIndex == 9) {
        // mul: detect 64-bit overflow via smulh (high 64 bits of 128-bit
        // signed product) — must equal sign-extension of low 64 bits.
        a.mul(x6, x1, x2);
        a.smulh(x7, x1, x2);
        a.cmp(x7, x6, asmjit::a64::asr(63));
        a.b_ne(fail);
        // Also confirm fits in 61-bit SmI range.
        a.mov(x1, x6);
        a.lsl(x4, x1, asmjit::Imm(3));
        a.asr(x4, x4, asmjit::Imm(3));
        a.cmp(x4, x1);
        a.b_ne(fail);
    } else {
        a.bind(fail);
        return;
    }
    a.lsl(x1, x1, asmjit::Imm(3));
    a.orr(x1, x1, asmjit::Imm(SMI_TAG));
    a.str(x1, ptr(x0, OFF_RETVAL));
    a.mov(w3, asmjit::Imm(EXIT_RETURN));
    a.str(w3, ptr(x0, OFF_EXIT));
    emitSyncSpToState(a);
    // V2 J2J-return contract: the inline-J2J lander expects the
    // retval in x1 (the return prelude loads it; this plain ret
    // bypasses the prelude).  Exposed 2026-06-12 when prim-prologue
    // callees were admitted through the xmethod gates.
    a.ldr(x1, ptr(x0, OFF_RETVAL));
    a.b(prologRet);  // V2 J2J-return via the shared prelude shim

    a.bind(fail);
}

// J2J chain return prelude — FREE FUNCTION so both emitOne_arm64's
// return emits and the prim-prologue success shim (compileViaAsmjit)
// can use it.  Retval is in x1 by convention; x2-x15 clobberable.
// Pops this activation's pending save and tail-jumps to the caller's
// resume; falls through when no save is poppable.
static void emitJ2JReturnPrelude_arm64(asmjit::a64::Assembler& a,
                                       int staticJ2JArgCount = -1) {
    using namespace asmjit::a64;
    (void)staticJ2JArgCount;
    const bool inlineJ2J = !GET_DEBUG_BOOL(PHARO_T1_NO_INLINE_J2J);
        if (!inlineJ2J) return;
        if (GET_DEBUG_BOOL(PHARO_T1_NO_J2J_RETPRELUDE)) return;  // bisect knob
        asmjit::Label normalReturn = a.new_label();
        // Check current j2jDepth > j2jEntryDepth (this method pushed a save).
        // Without entry-depth gate, chain-loop-activated methods would
        // incorrectly pop OUTER inline-J2J saves.  See deferred A6 option (a).
        if (g_fsrNodepth) {
            // FSR M3: poppability = cursor above the per-activation
            // baseline.  x23 = live cursor (M2 default-on).
            a.ldr(x4, ptr(x0, OFF_J2J_ENTRY_CURSOR));
            a.cmp(x23, x4);
            a.b_ls(normalReturn);  // cursor <= baseline → nothing poppable
        } else {
        a.ldr(w3, ptr(x0, OFF_J2J_DEPTH));
        a.ldr(w4, ptr(x0, OFF_J2J_ENTRY_DEPTH));
        a.cmp(w3, w4);
        a.b_le(normalReturn);   // current <= entry → no save pushed by this method
        }

        // Pop save: cursor -= sizeof(J2JSave) = 56; depth--
        // Pre-index ldp folds the cursor decrement into the first load.
        if (g_fsrCursor) {
            // FSR M2 v1: cursor resident in x23 (write-through).  The
            // pre-index ldp mutates x23 directly; the str keeps memory
            // authoritative for every C++/helper observer.
            a.ldp(x5, x6, ptr_pre(x23, -JSV_SIZE));
            a.mov(x4, x23);
            a.str(x23, ptr(x0, OFF_J2J_SAVE_CURSOR));
        } else {
        a.ldr(x4, ptr(x0, OFF_J2J_SAVE_CURSOR));
        a.ldp(x5, x6, ptr_pre(x4, -JSV_SIZE));  // x4 -= save size; load sp + recv
        a.str(x4, ptr(x0, OFF_J2J_SAVE_CURSOR));
        }
        if (!g_fsrNodepth) {
            a.sub(w3, w3, asmjit::Imm(1));
            a.str(w3, ptr(x0, OFF_J2J_DEPTH));
        } else if (g_fsrNodepthVerify) {
            // stage (b): w3 not loaded on the nodepth path — decrement
            // the field in place for the parity oracle.
            a.ldr(w3, ptr(x0, OFF_J2J_DEPTH));
            a.sub(w3, w3, asmjit::Imm(1));
            a.str(w3, ptr(x0, OFF_J2J_DEPTH));
        }

        // Load remaining save fields with ldp pairs.  Layout:
        //   [0]=sp, [8]=receiver, [16]=tempBase, [24]=ip,
        //   [32]=jitMethod, [40]=resumeAddr, [48]=sendArgCount
        // (sp + recv already loaded above via pre-index — x5=sp, x6=recv)
        a.str(x6, ptr(x0, OFF_RECEIVER));
        // jit-may20b Step 8.2: load tempBase only (skip save.ip restore).
        // The JIT body never READS state.ip during execution — every
        // bytecode op writes state.ip itself before any exit to interp,
        // so the restored ip value is always overwritten before being
        // used.  save.ip stays in the j2jPool for the trampoline's
        // Lret_null_resume bail (which reads it directly).  Saves 1
        // instr per J2J return; ~1M returns on fib(28) ≈ ~0.3 ms.
        //
        // For xmethod-off (default, self-rec only): x10 is unused after
        // this point; using ldr instead of ldp drops the redundant load.
        // For xmethod-on: x10 is reassigned at line ~2602 (literals=
        // method+16) before any read, so dropping the original load is
        // also safe.
        a.ldr(x6, ptr(x4, JSV_TEMPBASE));  // tempBase
        emitStoreTempBase(a, x6);
        a.ldr(x8, ptr(x4, JSV_RESUMEADDR));  // resumeAddr (always needed)
#if PHARO_J2J_SAVE_V2
        // V2 prelude tail: x5 = caller sp (restore the residency
        // mirror), mask the packed resume address, clear the exit, br.
        // Arg pop + retval write + (xmethod) context re-establishment
        // all happen at the resumeAfterCall continuation; x1 already
        // holds the retval from the return op.
        emitStoreSp(a, x5);
        a.and_(x8, x8, asmjit::Imm(0x0000FFFFFFFFFFFFULL));
        a.str(wzr, ptr(x0, OFF_EXIT));
        a.br(x8);
        a.bind(normalReturn);
#else
        // sendArgCount: if all J2J sends in this method have the same
        // nArgs (compile-time uniform), the return prelude can use the
        // value as an immediate, skipping the load + lsl/sub in the
        // sp-adjust below.  Otherwise load from save.
        if (staticJ2JArgCount < 0) {
            a.ldr(w9, ptr(x4, 48));       // sendArgCount
        }

        // Derive method/literals/argCount/jitMethod from callerJM.
        // When xmethod is OFF and inline-block-value is OFF, the J2J
        // push path is strictly self-recursive (callee == caller —
        // gated by SELF_REC_BIT tbz), so state.method, state.literals,
        // state.argCount, and state.jitMethod were never modified
        // during the J2J call — skip the 5 redundant stores.  When
        // either xmethod or inline-block-value is ON, callee may
        // differ from caller (block-value path or cross-method
        // dispatch), so restore from save.
        //
        // F3-NL2 (2026-05-23): root-caused fib(15) non-leaf hang to
        // missing restore.  jit_rt_inline_block_value_prep sets
        // state.{jitMethod,method,literals,argCount} to the BLOCK's
        // values; without this restore the caller's continuation reads
        // the block's literals, producing the cascading DNU on
        // `nil findNextHandlerContext` we observed.
        if (g_debug.t1InlineJ2JXmethod || g_debug.t1InlineBlockValue) {
            // 2026-05-24: skip the restore if save.jitMethod is null.
            // xmethod-off (default) inline-J2J self-rec push at line
            // ~4060 does NOT write save.jitMethod (state stays the
            // same for self-rec).  When inline-block-value is also on
            // (default) but the push was a self-rec one (not a
            // block-value push), save.jitMethod is 0 from j2jPool_'s
            // zero-init.  Without this guard, the ldr x6, [x7] below
            // dereferences NULL and SIGSEGVs — was masked by the
            // warm-J2J gate which prevented the push from happening.
            asmjit::Label skipJMRestore = a.new_label();
            a.ldr(x7, ptr(x4, 32));   // jitMethod
            a.cbz(x7, skipJMRestore);
            a.str(x7, ptr(x0, OFF_JITMETHOD));
            if (g_fsrX19) a.mov(x19, x7);  // FSR M1: caller-restore
            a.ldr(x6, ptr(x7, 0));    // method = callerJM[0]
            a.str(x6, ptr(x0, OFF_METHOD));
            a.add(x10, x6, asmjit::Imm(16));
            a.str(x10, ptr(x0, OFF_LITERALS));
            a.ldrb(w11, ptr(x7, 34)); // callerJM.argCount byte
            a.str(w11, ptr(x0, OFF_ARGCOUNT));
            a.bind(skipJMRestore);
        }

        // Pop callee's args from caller's sp, push retval (in x1).
        // semantics: *(sp - (nArgs+1)*8) = retval; sp -= nArgs*8
        //
        // Fold via: new_sp = sp - nArgs*8; recv_slot = new_sp - 8
        // (the recv slot was at sp-(nArgs+1)*8 = new_sp - 8 after the
        // subtraction).  `stur` supports the -8 displacement directly.
        //
        // When staticJ2JArgCount is known (all J2J sends in this method
        // have the same nArgs), substitute the immediate for the dynamic
        // lsl+sub.  nArgs=0 skips the sub entirely.
        if (staticJ2JArgCount == 0) {
            // sp unchanged; just write retval below current sp.
        } else if (staticJ2JArgCount > 0) {
            a.sub(x5, x5, asmjit::Imm(staticJ2JArgCount * 8));
        } else {
            a.lsl(x12, x9, asmjit::Imm(3));
            a.sub(x5, x5, x12);
        }
        a.stur(x1, ptr(x5, -8));                    // write retval @ new_sp-8
        emitStoreSp(a, x5);

        // Clear exitReason so callers don't see stale EXIT_RETURN.
        // Use wzr directly to skip the mov+str pair.
        a.str(wzr, ptr(x0, OFF_EXIT));

        // Tail-call to caller's resumeAddr.
        a.br(x8);

        a.bind(normalReturn);
#endif  // PHARO_J2J_SAVE_V2 (prelude tail, inside the free fn)
}

bool emitOne_arm64(asmjit::a64::Assembler& a, uint8_t op,
                    uint64_t nilBits, int bcOffsetFromMethObj,
                    int siteIdx,
                    const std::vector<asmjit::Label>& bcLabels,
                    int globalIdx,
                    int callerArgCount, int callerTempCount,
                    int staticJ2JArgCount) {
    using namespace asmjit::a64;
    (void)bcOffsetFromMethObj;
    (void)siteIdx;
    (void)callerArgCount;

    if (op <= 0x0F) {
        int n = op & 0x0F;
        // FINDNODE_WATCH (asTuple only): at the very first bytecode, record the
        // entry operand-stack depth (sp-tempBase) — must be 0; nonzero = the
        // entry left OFF_SP one slot high.
        if (g_emitGetterTrace && globalIdx == 0) {
            a.sub(asmjit::a64::sp, asmjit::a64::sp, asmjit::Imm(16));
            a.str(x0,  ptr(asmjit::a64::sp, 0));
            a.str(x30, ptr(asmjit::a64::sp, 8));
            a.mov(x9, asmjit::Imm((uint64_t)&jit_rt_atrec_entry));
            a.blr(x9);
            a.ldr(x0,  ptr(asmjit::a64::sp, 0));
            a.ldr(x30, ptr(asmjit::a64::sp, 8));
            a.add(asmjit::a64::sp, asmjit::a64::sp, asmjit::Imm(16));
        }
        if (tosFam(kTosFamPush) && !GET_DEBUG_BOOL(PHARO_SHADOW_SLOTS)) {
            a.ldr(x26, ptr(x0, OFF_RECEIVER));
            a.ldr(x26, ptr(x26, OBJ_SLOT_0 + n * 8));
            emitTosPush(a);
        } else {
            a.ldr(x1, ptr(x0, OFF_RECEIVER));
            a.ldr(x1, ptr(x1, OBJ_SLOT_0 + n * 8));
            emitShadowReadVerify(a, n);
            emitPushReg(a, x1);
        }
        return true;
    }
    if (op >= 0x20 && op <= 0x3F) {
        int n = op - 0x20;
        asmjit::a64::Gp dst = tosFam(kTosFamPush) ? x26 : x1;
        if (g_fsrLazy)  // M4: mirror may be stale; x19's literalsCache is canonical
            a.ldr(dst, ptr(x19, (int)offsetof(JITMethod, literalsCache)));
        else
        a.ldr(dst, ptr(x0, OFF_LITERALS));
        a.ldr(dst, ptr(dst, n * 8));
        if (tosFam(kTosFamPush)) emitTosPush(a); else emitPushReg(a, x1);
        return true;
    }
    // pushLitVar N (0x10..0x1F): push literals[N].value (Association.slot[1]).
    if (op >= SistaV1::PushLitVarBase && op <= SistaV1::PushLitVarLast) {
        int n = op - SistaV1::PushLitVarBase;
        asmjit::a64::Gp dst = tosFam(kTosFamPush) ? x26 : x1;
        if (g_fsrLazy)  // M4: see pushLitConst
            a.ldr(dst, ptr(x19, (int)offsetof(JITMethod, literalsCache)));
        else
        a.ldr(dst, ptr(x0, OFF_LITERALS));
        a.ldr(dst, ptr(dst, n * 8));
        a.ldr(dst, ptr(dst, OBJ_SLOT_0 + 8));
        if (tosFam(kTosFamPush)) emitTosPush(a); else emitPushReg(a, x1);
        return true;
    }
    if (op >= 0x40 && op <= 0x4B) {
        int n = op - 0x40;
        asmjit::a64::Gp dst = tosFam(kTosFamPush) ? x26 : x1;
        emitLoadTempBase(a, dst);
        a.ldr(dst, ptr(dst, n * 8));
        if (tosFam(kTosFamPush)) emitTosPush(a); else emitPushReg(a, x1);
        return true;
    }
    if (op == SistaV1::PushReceiver) {
        asmjit::a64::Gp dst = tosFam(kTosFamPush) ? x26 : x1;
        a.ldr(dst, ptr(x0, OFF_RECEIVER));
        if (tosFam(kTosFamPush)) emitTosPush(a); else emitPushReg(a, x1);
        return true;
    }
    if (op >= 0x4D && op <= 0x51 && tosFam(kTosFamPush)) {
        switch (op) {
        case 0x4D: a.ldr(x26, ptr(x0, OFF_TRUEOOP));  break;
        case 0x4E: a.ldr(x26, ptr(x0, OFF_FALSEOOP)); break;
        case 0x4F: a.mov(x26, asmjit::Imm(nilBits));  break;
        case 0x50: a.mov(x26, asmjit::Imm(smiBits(0))); break;
        default:   a.mov(x26, asmjit::Imm(smiBits(1))); break;
        }
        emitTosPush(a);
        if (op == 0x50 || op == 0x51) {
            g_tos.constSmI = true;
            g_tos.taggedBits = (int64_t)smiBits(op - 0x50);
        }
        return true;
    }
    if (op == 0x4D) { a.ldr(x1, ptr(x0, OFF_TRUEOOP));  emitPushReg(a, x1); return true; }
    if (op == 0x4E) { a.ldr(x1, ptr(x0, OFF_FALSEOOP)); emitPushReg(a, x1); return true; }
    if (op == 0x4F) { a.mov(x1, asmjit::Imm(nilBits));  emitPushReg(a, x1); return true; }
    if (op == 0x50) { a.mov(x1, asmjit::Imm(smiBits(0))); emitPushReg(a, x1); return true; }
    if (op == 0x51) { a.mov(x1, asmjit::Imm(smiBits(1))); emitPushReg(a, x1); return true; }
    if (op == SistaV1::Pop) {
#if PHARO_T1_SP_IN_X25
        a.sub(x25, x25, asmjit::Imm(8));
#else
        emitLoadSp(a, x2);
        a.sub(x2, x2, asmjit::Imm(8));
        emitStoreSp(a, x2);
#endif
        return true;
    }
    // Dup: read sp[-1], push it.
    if (op == SistaV1::Dup) {
        if (tosFam(kTosFamDup) && g_tosIn.valid) {
            // write-through: x26 already == TOS; push it again and the
            // cache stays valid (x26 == the new TOS too).
            emitTosVerify(a);
            emitTosPush(a);
            g_tos.constSmI = g_tosIn.constSmI;
            g_tos.taggedBits = g_tosIn.taggedBits;
            return true;
        }
#if PHARO_T1_SP_IN_X25
        a.ldur(x1, ptr(x25, -8));
#else
        emitLoadSp(a, x2);
        a.ldur(x1, ptr(x2, -8));
#endif
        emitPushReg(a, x1);
        return true;
    }
    // PushThisContext 0x52: bail to interp.  Materializing thisContext
    // requires walking the inline frame stack (Interpreter.cpp
    // case jit::SistaV1::PushThisContext) — too complex to emit
    // inline.  Reuse EXIT_ARITH_OVERFLOW: chain-loop handler syncs
    // ip+sp and returns to interp's main loop, which executes the
    // 0x52 normally and runs the rest of the method in interp.
    if (op == SistaV1::PushThisContext) {
        if (g_fsrLazy) {
            // M4: the method mirror may be stale (per-call store
            // deleted) — derive the exit ip from x19's bcStartCache.
            a.ldr(x5, ptr(x19, (int)offsetof(JITMethod, bcStartCache)));
            a.add(x5, x5, asmjit::Imm(bcOffsetFromMethObj - g_bcStartDelta));
        } else {
        a.ldr(x5, ptr(x0, OFF_METHOD));
        a.add(x5, x5, asmjit::Imm(bcOffsetFromMethObj));
        }
        a.str(x5, ptr(x0, OFF_IP));
        a.mov(w3, asmjit::Imm(EXIT_ARITH_OVERFLOW));
        a.str(w3, ptr(x0, OFF_EXIT));
        emitSyncSpToState(a);
        a.ret(x30);
        return true;
    }
    // BlockReturnNil 0x5D / BlockReturnTop 0x5E: bail to interp.
    // Block return involves frame walking, enclosingLevels (extA_),
    // and jumpDist (extB_) — too complex to emit inline.  Interp
    // handles it correctly; subsequent JIT code is unreachable
    // anyway since block returns terminate the block body.
    if (op == SistaV1::BlockReturnNil || op == SistaV1::BlockReturnTop) {
        if (g_fsrLazy) {
            // M4: the method mirror may be stale (per-call store
            // deleted) — derive the exit ip from x19's bcStartCache.
            a.ldr(x5, ptr(x19, (int)offsetof(JITMethod, bcStartCache)));
            a.add(x5, x5, asmjit::Imm(bcOffsetFromMethObj - g_bcStartDelta));
        } else {
        a.ldr(x5, ptr(x0, OFF_METHOD));
        a.add(x5, x5, asmjit::Imm(bcOffsetFromMethObj));
        }
        a.str(x5, ptr(x0, OFF_IP));
        a.mov(w3, asmjit::Imm(EXIT_ARITH_OVERFLOW));
        a.str(w3, ptr(x0, OFF_EXIT));
        emitSyncSpToState(a);
        a.ret(x30);
        return true;
    }
    // popStoreTemp N (0xD0..0xD7): pop TOS, store into tempBase[N].
    if (op >= SistaV1::PopStoreTempBase && op <= SistaV1::PopStoreTempLast) {
        int n = op - SistaV1::PopStoreTempBase;
        if (tosFam(kTosFamPopStore) && g_tosIn.valid) {
            // simStack: value already in x26 — skip the reload.
            emitTosVerify(a);
#if PHARO_T1_SP_IN_X25
            a.sub(x25, x25, asmjit::Imm(8));
#else
            emitLoadSp(a, x2);
            a.sub(x2, x2, asmjit::Imm(8));
            emitStoreSp(a, x2);
#endif
            emitLoadTempBase(a, x4);
            a.str(x26, ptr(x4, n * 8));
            return true;
        }
        emitLoadSp(a, x2);
        a.sub(x2, x2, asmjit::Imm(8));
        emitStoreSp(a, x2);
        a.ldr(x1, ptr(x2));
        emitLoadTempBase(a, x4);
        a.str(x1, ptr(x4, n * 8));
        return true;
    }
    // popStoreRecv N (0xC8..0xCF): pop TOS, store into receiver.slot[N].
    // Tests the immutable bit (header bit 23) before writing and bails
    // to interp if set — see the x86 version above for full rationale.
    //
    // Audit-gap closure (2026-05-28): after the inline store, emit the
    // old→young write barrier (set RememberedBit on receiver header
    // when receiver ∈ oldSpace and value is a heap Oop ∈ newSpace).
    // Uses x5/x6 as scratch; doesn't disturb x0/x1/x2/x4.
    if (op >= SistaV1::PopStoreRecvBase && op <= SistaV1::PopStoreRecvLast) {
        int n = op - SistaV1::PopStoreRecvBase;
        asmjit::Label bail = a.new_label();
        asmjit::Label end  = a.new_label();
        asmjit::Label barrierSkip = a.new_label();
        a.ldr(x4, ptr(x0, OFF_RECEIVER));
        a.ldr(w5, ptr(x4));                 // low 32 bits of header
        a.tst(w5, asmjit::Imm(0x800000));   // bit 23 = ImmutableBit
        a.b_ne(bail);
        // B3.2 (simStack): value from x26 — skip the reload.  The
        // immutable bail above fires BEFORE the sp decrement either
        // way (memory intact at the bail).
        if (tosFam(kTosFamPopStore) && g_tosIn.valid) {
            emitTosVerify(a);
#if PHARO_T1_SP_IN_X25
            a.sub(x25, x25, asmjit::Imm(8));
#else
            emitLoadSp(a, x2);
            a.sub(x2, x2, asmjit::Imm(8));
            emitStoreSp(a, x2);
#endif
            a.mov(x1, x26);
        } else {
            emitLoadSp(a, x2);
            a.sub(x2, x2, asmjit::Imm(8));
            emitStoreSp(a, x2);
            a.ldr(x1, ptr(x2));
        }
        a.str(x1, ptr(x4, OBJ_SLOT_0 + n * 8));
        emitStoreRingLog(a, n);
        // Inline write barrier.  Skip if value isn't an Oop (tag != 0),
        // receiver not in oldSpace, or value not in newSpace.  Else set
        // bit 29 of receiver header (RememberedBit).
        a.and_(x6, x1, asmjit::Imm(7));
        a.cbnz(x6, barrierSkip);
        a.ldr(x6, ptr(x0, 240));            // oldSpaceStart
        a.cmp(x4, x6);
        a.b_lo(barrierSkip);
        a.ldr(x6, ptr(x0, 248));            // oldSpaceEnd
        a.cmp(x4, x6);
        a.b_hs(barrierSkip);
        a.ldr(x6, ptr(x0, 256));            // newSpaceStart
        a.cmp(x1, x6);
        a.b_lo(barrierSkip);
        a.ldr(x6, ptr(x0, 264));            // newSpaceEnd
        a.cmp(x1, x6);
        a.b_hs(barrierSkip);
        a.ldr(x6, ptr(x4));
        a.orr(x6, x6, asmjit::Imm(1ULL << 29));
        a.str(x6, ptr(x4));
        a.bind(barrierSkip);
        a.b(end);
        a.bind(bail);
        if (g_fsrLazy) {
            // M4: the method mirror may be stale (per-call store
            // deleted) — derive the exit ip from x19's bcStartCache.
            a.ldr(x5, ptr(x19, (int)offsetof(JITMethod, bcStartCache)));
            a.add(x5, x5, asmjit::Imm(bcOffsetFromMethObj - g_bcStartDelta));
        } else {
        a.ldr(x5, ptr(x0, OFF_METHOD));
        a.add(x5, x5, asmjit::Imm(bcOffsetFromMethObj));
        }
        a.str(x5, ptr(x0, OFF_IP));
        a.mov(w3, asmjit::Imm(EXIT_ARITH_OVERFLOW));
        a.str(w3, ptr(x0, OFF_EXIT));
        emitSyncSpToState(a);
        a.ret(x30);
        a.bind(end);
        return true;
    }
    // J2J chain return prelude — emitted before each return path.
    // If state.j2jDepth > 0: pop the most recent save, restore caller
    // state, push retval to caller's sp, tail-call to save->resumeAddr.
    // Otherwise: fall through to normal exit-to-C++ return.
    //
    // Mirrors stencils.cpp:491-531 J2J_INLINE_RETURN_IMPL protocol.
    // Retval is in x1 by convention; x2-x15 free to clobber.
    //
    // Default ON since 2026-05-17 (PHARO_T1_NO_INLINE_J2J=1 opt-out).
    // When off, the ldr+cbz is still emitted but j2jDepth is always 0
    // so we fall through to normal-return — tiny overhead, zero
    // semantic change.
    const bool inlineJ2J = !GET_DEBUG_BOOL(PHARO_T1_NO_INLINE_J2J);
    auto emitJ2JReturnPreludeIfEnabled = [&]() {
        emitJ2JReturnPrelude_arm64(a, staticJ2JArgCount);
    };

    auto emitReturnPtr = [&](int srcOff) {
        a.ldr(x1, ptr(x0, srcOff));
        emitJ2JReturnPreludeIfEnabled();
        a.str(x1, ptr(x0, OFF_RETVAL));
        a.mov(w3, asmjit::Imm(EXIT_RETURN));
        a.str(w3, ptr(x0, OFF_EXIT));
        emitSyncSpToState(a);
        a.ret(x30);
    };
    auto emitReturnImm = [&](uint64_t imm) {
        a.mov(x1, asmjit::Imm(imm));
        emitJ2JReturnPreludeIfEnabled();
        a.str(x1, ptr(x0, OFF_RETVAL));
        a.mov(w3, asmjit::Imm(EXIT_RETURN));
        a.str(w3, ptr(x0, OFF_EXIT));
        emitSyncSpToState(a);
        a.ret(x30);
    };
    // NON-LOCAL RETURN: 0x58-0x5C inside a CompiledBlock returns from
    // the HOME method, not the block.  Bail to interp (same shape as
    // the 0x5D/0x5E handler) so commonReturn's home-context walk runs.
    // Plain EXIT_RETURN here would return from the block's own frame —
    // the swallowed-NLR bug (see g_emitIsBlock comment).
    if (g_emitIsBlock && SistaV1::isReturn(op)) {
        if (g_fsrLazy) {
            // M4: the method mirror may be stale (per-call store
            // deleted) — derive the exit ip from x19's bcStartCache.
            a.ldr(x5, ptr(x19, (int)offsetof(JITMethod, bcStartCache)));
            a.add(x5, x5, asmjit::Imm(bcOffsetFromMethObj - g_bcStartDelta));
        } else {
        a.ldr(x5, ptr(x0, OFF_METHOD));
        a.add(x5, x5, asmjit::Imm(bcOffsetFromMethObj));
        }
        a.str(x5, ptr(x0, OFF_IP));
        a.mov(w3, asmjit::Imm(EXIT_ARITH_OVERFLOW));
        a.str(w3, ptr(x0, OFF_EXIT));
        emitSyncSpToState(a);
        a.ret(x30);
        return true;
    }
    if (op == SistaV1::ReturnReceiver) { emitReturnPtr(OFF_RECEIVER); return true; }
    if (op == 0x59) { emitReturnPtr(OFF_TRUEOOP);  return true; }
    if (op == 0x5A) { emitReturnPtr(OFF_FALSEOOP); return true; }
    if (op == 0x5B) { emitReturnImm(nilBits);      return true; }
    if (op == SistaV1::ReturnTop) {
        if (tosFam(kTosFamRetTop) && g_tosIn.valid) {
            // simStack: retval already in x26 (RC-F7 ordering: the mov
            // happens before the prelude restores caller state).
            emitTosVerify(a);
#if PHARO_T1_SP_IN_X25
            a.sub(x25, x25, asmjit::Imm(8));
#else
            emitLoadSp(a, x2);
            a.sub(x2, x2, asmjit::Imm(8));
            emitStoreSp(a, x2);
#endif
            a.mov(x1, x26);
            emitJ2JReturnPreludeIfEnabled();
            a.str(x1, ptr(x0, OFF_RETVAL));
            a.mov(w3, asmjit::Imm(EXIT_RETURN));
            a.str(w3, ptr(x0, OFF_EXIT));
            emitSyncSpToState(a);
            a.ret(x30);
            return true;
        }
        emitLoadSp(a, x2);
        a.sub(x2, x2, asmjit::Imm(8));
        emitStoreSp(a, x2);
        a.ldr(x1, ptr(x2));      // x1 = retval (TOS)
        emitJ2JReturnPreludeIfEnabled();
        a.str(x1, ptr(x0, OFF_RETVAL));
        a.mov(w3, asmjit::Imm(EXIT_RETURN));
        a.str(w3, ptr(x0, OFF_EXIT));
        emitSyncSpToState(a);
        a.ret(x30);
        return true;
    }
    // Phase 3 arithmetic on ARM64 (mirror of x86, with tagged-arith).
    //   x2 = sp;  x1 = a (sp[-2]);  x4 = b (sp[-1])
    //   5-op SmI check: (a^b) | (a-1)
    //   + / - : tagged-add/sub directly (b_vs catches SmI overflow
    //           since tagged form pre-shifts by 8); then sub/add 1.
    //   compares: cmp tagged bits directly (monotonic transform).
    if (isPhase3ArithOp(op)) {
        asmjit::Label bail = a.new_label();
        asmjit::Label end  = a.new_label();

        emitLoadSp(a, x2);
        a.ldr(x1, ptr(x2, -16));
        // simStack B2a (consumer side): TOS is in x26 — kill the
        // dependent reload that heads this bytecode's chain.  x4 keeps
        // identical semantics for every downstream path (bail, float,
        // ByteString=).  Producer validity for arith results is B2b
        // (multiple store-to-TOS paths must each re-arm x26 first).
        if (tosFam(kTosFamArith) && g_tosIn.valid) {
            emitTosVerify(a);
            a.mov(x4, x26);
        } else {
            a.ldr(x4, ptr(x2, -8));
        }
        // B2b (producer side): every path that reaches `end` stores a
        // result to the new TOS slot — mirror it into x26 so the next
        // bytecode starts with a valid cache.  The bail path rets to
        // the interpreter (no `end` arrival), so claiming validity at
        // `end` is sound iff EVERY b(end) site below re-arms first —
        // all seven do (+, -, *, csel-compares, float, ByteString=
        // true/false).
        const bool tosProduceArith = tosFam(kTosFamArith);
        // SmI tag check.  B3 (simStack constSmI shrink): when TOS is a
        // KNOWN tagged-SmI constant (PushInteger/Push0/Push1 produced
        // it), only the unknown side (NOS, x1) needs checking — 3 ops
        // on a shorter dependency chain instead of the dual 5-op check.
        if (tosFam(kTosFamArith) && g_tosIn.valid && g_tosIn.constSmI
                && (g_tosIn.taggedBits & 7) == 1) {
            a.eor(x5, x1, asmjit::Imm(1));   // tag-1 check on NOS only
            a.tst(x5, asmjit::Imm(7));
            a.b_ne(bail);
        } else {
            // (a^b) | (a-1) low 3 bits = 0 iff both SmI AND same tag.
            a.eor(x5, x1, x4);
            a.sub(x6, x1, asmjit::Imm(1));
            a.orr(x5, x5, x6);
            a.tst(x5, asmjit::Imm(7));
            a.b_ne(bail);
        }

        if (op == 0x60) {        // +
            // a_bits + b_bits = (a+b)*8 + 2 → sub 1 to retag.
            a.adds(x1, x1, x4);
            a.b_vs(bail);
            a.sub(x1, x1, asmjit::Imm(1));
            if (tosProduceArith) a.mov(x26, x1);
            a.str(x1, ptr(x2, -16));
            a.sub(x2, x2, asmjit::Imm(8));
            emitStoreSp(a, x2);
        } else if (op == 0x61) { // -
            // a_bits - b_bits = (a-b)*8 → add 1 to retag.
            a.subs(x1, x1, x4);
            a.b_vs(bail);
            a.add(x1, x1, asmjit::Imm(1));
            if (tosProduceArith) a.mov(x26, x1);
            a.str(x1, ptr(x2, -16));
            a.sub(x2, x2, asmjit::Imm(8));
            emitStoreSp(a, x2);
        } else if (op == 0x68) { // *
            // Untag both: a = (a_bits >> 3), b = (b_bits >> 3) using ASR.
            // smulh + mul; overflow if hi != asr(lo, 63).
            // Retag: result_bits = (result << 3) | 1.
            a.asr(x5, x1, asmjit::Imm(3));   // x5 = a (untagged)
            a.asr(x6, x4, asmjit::Imm(3));   // x6 = b (untagged)
            a.mul(x1, x5, x6);               // x1 = lo half of a*b
            a.smulh(x9, x5, x6);             // x9 = hi half
            a.asr(x10, x1, asmjit::Imm(63));  // x10 = sign-extend(lo)
            a.cmp(x9, x10);
            a.b_ne(bail);
            // 61-BIT RANGE CHECK (the -2^62-ns clock lesion, 2026-06-11):
            // smulh only catches 64-bit overflow.  A product needing
            // 61-63 bits (e.g. microsecondClock*1000 = 3.96e18 ns in
            // DateAndTime>>now) passed here and the retag below wrapped
            // it mod 2^61 — signed = value - 2^62: every warm
            // DateAndTime now returned year 1880, surfacing as sporadic
            // spurious watchdog timeouts across the whole suite.  The
            // other three mul emits all have this check; this Phase-3
            // site was missing it.
            a.lsl(x9, x1, asmjit::Imm(3));
            a.asr(x10, x9, asmjit::Imm(3));
            a.cmp(x10, x1);
            a.b_ne(bail);
            // Retag.
            a.lsl(x1, x1, asmjit::Imm(3));
            a.orr(x1, x1, asmjit::Imm(1));
            if (tosProduceArith) a.mov(x26, x1);
            a.str(x1, ptr(x2, -16));
            a.sub(x2, x2, asmjit::Imm(8));
            emitStoreSp(a, x2);
        } else {
            // Comparisons: csel false/true on signed flags.  Compare
            // tagged bits directly — monotonic transform preserves
            // ordering (mirrors x86 skip-untag-for-compare opt).
            // simStack B4 (design §6 fusion row): when the NEXT bytecode
            // is a naked FORWARD conditional short jump that is not
            // itself a jump target, fuse: cmp + b.cond direct — no
            // boolean materialization, no bool round trip.  The jump
            // bytecode KEEPS its full unfused emit at its own bcLabel
            // (the send-resume / non-SmI arrival path); the fused fast
            // path branches around it.  Pops are applied (x25) BEFORE
            // the b.cond (RF-7; plain sub does not touch flags).  The
            // post-jump label's emit-time cache state is invalid by
            // construction (the jump bytecode's emit leaves g_tos
            // cleared), so the new jump edge is consistent.
#if PHARO_T1_SP_IN_X25
            if (tosFam(kTosFamFuseCmpJ) && g_tosBc && g_tosJumpTargetsPtr) {
                int nextIdx = globalIdx + 1;
                if ((size_t)nextIdx < g_tosBcLen) {
                    uint8_t nop = g_tosBc[nextIdx];
                    if (SistaV1::isConditionalShortJump(nop)
                            && !(*g_tosJumpTargetsPtr)[nextIdx]) {
                        int targetIdx = SistaV1::shortJumpTarget(nop, nextIdx);
                        int postIdx = nextIdx + 1;
                        if (targetIdx > nextIdx
                                && (size_t)targetIdx < bcLabels.size()
                                && (size_t)postIdx < bcLabels.size()) {
                            bool jt = SistaV1::isShortJumpTrue(nop);
                            a.cmp(x1, x4);
                            a.sub(x25, x25, asmjit::Imm(16));
                            asmjit::Label tgt = bcLabels[targetIdx];
                            switch (op) {
                            case 0x62: if (jt) a.b_lt(tgt); else a.b_ge(tgt); break;
                            case 0x63: if (jt) a.b_gt(tgt); else a.b_le(tgt); break;
                            case 0x64: if (jt) a.b_le(tgt); else a.b_gt(tgt); break;
                            case 0x65: if (jt) a.b_ge(tgt); else a.b_lt(tgt); break;
                            case 0x66: if (jt) a.b_eq(tgt); else a.b_ne(tgt); break;
                            default:   if (jt) a.b_ne(tgt); else a.b_eq(tgt); break;
                            }
                            a.b(bcLabels[postIdx]);
                            a.b(end);   // unreachable; keeps the block shape
                            // fall into the bail path below for non-SmI
                            goto tosFusedCmpDone;
                        }
                    }
                }
            }
#endif
            a.cmp(x1, x4);
            // ldp loads both oops in one instruction (x6=TRUEOOP at +128,
            // x5=FALSEOOP at +136).  Saves 1 ldr per inline comparison
            // bytecode (0x62-0x67).
            a.ldp(x6, x5, ptr(x0, OFF_TRUEOOP));
            switch (op) {
                case 0x62: a.csel(x5, x6, x5, CondCode::kLT); break;
                case 0x63: a.csel(x5, x6, x5, CondCode::kGT); break;
                case 0x64: a.csel(x5, x6, x5, CondCode::kLE); break;
                case 0x65: a.csel(x5, x6, x5, CondCode::kGE); break;
                case 0x66: a.csel(x5, x6, x5, CondCode::kEQ); break;
                case 0x67: a.csel(x5, x6, x5, CondCode::kNE); break;
            }
            if (tosProduceArith) a.mov(x26, x5);
            a.str(x5, ptr(x2, -16));
            a.sub(x2, x2, asmjit::Imm(8));
            emitStoreSp(a, x2);
#if PHARO_T1_SP_IN_X25
            tosFusedCmpDone: ;
#endif
        }
        a.b(end);

        a.bind(bail);
        // Counter: how often the SmI fast path bails (for any reason).
        if (op == 0x60 || op == 0x61) {
            a.mov(x14, asmjit::Imm((uint64_t)&g_bcArithBail_hits));
            a.ldr(x15, ptr(x14));
            a.add(x15, x15, asmjit::Imm(1));
            a.str(x15, ptr(x14));
        }
        // Inline SmallFloat + / - at the bytecode level (op 0x60 / 0x61).
        // Reached when the SmI fast-path fails.  Both operands must have
        // tag 5 and non-zero shifted bits (i.e. not ±0).
        if ((op == 0x60 || op == 0x61) && !GET_DEBUG_BOOL(PHARO_T1_NO_BC_FLOAT)) {
            using namespace asmjit::a64;
            asmjit::Label notFloat = a.new_label();
            a.and_(x5, x1, asmjit::Imm(0x7));
            a.cmp(x5, asmjit::Imm(5));
            a.b_ne(notFloat);
            a.and_(x5, x4, asmjit::Imm(0x7));
            a.cmp(x5, asmjit::Imm(5));
            a.b_ne(notFloat);
            // Counter increment (every successful tag check).
            {
                a.mov(x14, asmjit::Imm((uint64_t)&g_bcFloatArith_hits));
                a.ldr(x15, ptr(x14));
                a.add(x15, x15, asmjit::Imm(1));
                a.str(x15, ptr(x14));
            }
            // Decode rcv to d0.
            a.lsr(x5, x1, asmjit::Imm(3));
            a.cmp(x5, asmjit::Imm(1));
            a.b_ls(notFloat);                 // ±0 → bail
            a.mov(x6, asmjit::Imm(0x7000000000000000ULL));
            a.add(x5, x5, x6);
            a.ror(x5, x5, asmjit::Imm(1));
            a.fmov(d0, x5);
            // Decode arg to d1 (x6 still has the offset).
            a.lsr(x5, x4, asmjit::Imm(3));
            a.cmp(x5, asmjit::Imm(1));
            a.b_ls(notFloat);
            a.add(x5, x5, x6);
            a.ror(x5, x5, asmjit::Imm(1));
            a.fmov(d1, x5);
            if (op == 0x60) a.fadd(d0, d0, d1);
            else            a.fsub(d0, d0, d1);
            // Encode.
            a.fmov(x5, d0);
            a.ror(x5, x5, asmjit::Imm(63));
            a.cmp(x5, x6);
            a.b_lo(notFloat);
            a.sub(x5, x5, x6);
            a.mov(x7, asmjit::Imm(0x1FFFFFFFFFFFFFFFULL));
            a.cmp(x5, x7);
            a.b_hi(notFloat);
            a.lsl(x5, x5, asmjit::Imm(3));
            // SmallFloatTag (5).  `orr #5` is not a valid AArch64 bitmask
            // immediate (asmjit drops it → tag 0 corruption); after `lsl #3`
            // the low 3 bits are 0 so `add #5` sets the tag identically.
            a.add(x5, x5, asmjit::Imm(5));
            if (tosProduceArith) a.mov(x26, x5);
            a.str(x5, ptr(x2, -16));
            a.sub(x2, x2, asmjit::Imm(8));
            emitStoreSp(a, x2);
            a.b(end);
            a.bind(notFloat);
        }
        // Inline ByteString = (op 0x66 only).  Mirrors stencils.cpp:880-926.
        // After SmI tag check fails, try byte-equality for two byte
        // objects (fmt 16-23).  Helps Dictionary>>scanFor:'s key=arg
        // (dict bench) and other String comparisons.
        if (op == 0x66) {
            using namespace asmjit::a64;
            asmjit::Label bsBail = a.new_label();
            asmjit::Label resultFalse = a.new_label();
            asmjit::Label resultTrue = a.new_label();
            // Both must be heap objects (tag == 0, addr >= 0x10000).
            a.and_(x5, x1, asmjit::Imm(0x7));
            a.cbnz(x5, bsBail);
            a.cmp(x1, asmjit::Imm(0x10000));
            a.b_lo(bsBail);
            a.and_(x5, x4, asmjit::Imm(0x7));
            a.cbnz(x5, bsBail);
            a.cmp(x4, asmjit::Imm(0x10000));
            a.b_lo(bsBail);
            // Headers.
            a.ldr(x5, ptr(x1));                // header_a
            a.ldr(x6, ptr(x4));                // header_b
            // fmt_a / fmt_b in 16-23
            a.lsr(x7, x5, asmjit::Imm(24));
            a.and_(x7, x7, asmjit::Imm(0x1F));
            a.sub(x8, x7, asmjit::Imm(16));
            a.cmp(x8, asmjit::Imm(8));
            a.b_hs(bsBail);
            a.lsr(x9, x6, asmjit::Imm(24));
            a.and_(x9, x9, asmjit::Imm(0x1F));
            a.sub(x10, x9, asmjit::Imm(16));
            a.cmp(x10, asmjit::Imm(8));
            a.b_hs(bsBail);
            // Slot counts (reject 255 overflow encoding).
            a.lsr(x11, x5, asmjit::Imm(56));
            a.and_(x11, x11, asmjit::Imm(0xFF));
            a.cmp(x11, asmjit::Imm(255));
            a.b_eq(bsBail);
            a.lsr(x12, x6, asmjit::Imm(56));
            a.and_(x12, x12, asmjit::Imm(0xFF));
            a.cmp(x12, asmjit::Imm(255));
            a.b_eq(bsBail);
            // byteSize = slots*8 - (fmt - 16) (same as fmt & 7 for fmt in [16,23]).
            a.lsl(x11, x11, asmjit::Imm(3));
            a.and_(x7, x7, asmjit::Imm(0x7));
            a.sub(x11, x11, x7);                // bytes_a
            a.lsl(x12, x12, asmjit::Imm(3));
            a.and_(x9, x9, asmjit::Imm(0x7));
            a.sub(x12, x12, x9);                // bytes_b
            a.cmp(x11, x12);
            a.b_ne(resultFalse);                // size mismatch → false
            // Compare bytes [0..bytes_a).
            a.add(x13, x1, asmjit::Imm(8));   // ba
            a.add(x14, x4, asmjit::Imm(8));   // bb
            a.mov(x15, asmjit::Imm(0));
            {
                asmjit::Label cmpLoop = a.new_label();
                a.bind(cmpLoop);
                a.cmp(x15, x11);
                a.b_hs(resultTrue);
                a.ldrb(w5, ptr(x13, x15));
                a.ldrb(w6, ptr(x14, x15));
                a.cmp(w5, w6);
                a.b_ne(resultFalse);
                a.add(x15, x15, asmjit::Imm(1));
                a.b(cmpLoop);
            }
            // true / false result.
            a.bind(resultTrue);
            a.ldr(x6, ptr(x0, OFF_TRUEOOP));
            if (tosProduceArith) a.mov(x26, x6);
            a.str(x6, ptr(x2, -16));
            a.sub(x2, x2, asmjit::Imm(8));
            emitStoreSp(a, x2);
            a.b(end);
            a.bind(resultFalse);
            a.ldr(x6, ptr(x0, OFF_FALSEOOP));
            if (tosProduceArith) a.mov(x26, x6);
            a.str(x6, ptr(x2, -16));
            a.sub(x2, x2, asmjit::Imm(8));
            emitStoreSp(a, x2);
            a.b(end);
            a.bind(bsBail);
        }
        // x5 = state.method.rawBits + bcOffsetFromMethObj  (post-GC safe)
        if (g_fsrLazy) {
            // M4: the method mirror may be stale (per-call store
            // deleted) — derive the exit ip from x19's bcStartCache.
            a.ldr(x5, ptr(x19, (int)offsetof(JITMethod, bcStartCache)));
            a.add(x5, x5, asmjit::Imm(bcOffsetFromMethObj - g_bcStartDelta));
        } else {
        a.ldr(x5, ptr(x0, OFF_METHOD));
        a.add(x5, x5, asmjit::Imm(bcOffsetFromMethObj));
        }
        a.str(x5, ptr(x0, OFF_IP));
        a.mov(w3, asmjit::Imm(EXIT_ARITH_OVERFLOW));
        a.str(w3, ptr(x0, OFF_EXIT));
        emitSyncSpToState(a);
        a.ret(x30);

        a.bind(end);
        if (tosProduceArith) g_tos.valid = true;
        return true;
    }
    // Phase 3 bitwise on ARM64: 0x6E bitAnd:, 0x6F bitOr:.  Tag bits
    // commute with AND/OR — no untag/retag needed.  Mirror of x86.
    if (isPhase3BitOp(op)) {
        asmjit::Label bail = a.new_label();
        asmjit::Label end  = a.new_label();
        emitLoadSp(a, x2);
        a.ldr(x1, ptr(x2, -16));
        a.ldr(x4, ptr(x2, -8));
        a.eor(x5, x1, x4);
        a.sub(x6, x1, asmjit::Imm(1));
        a.orr(x5, x5, x6);
        a.tst(x5, asmjit::Imm(7));
        a.b_ne(bail);
        if (op == 0x6E) a.and_(x1, x1, x4);   // bitAnd:
        else            a.orr (x1, x1, x4);   // bitOr:
        a.str(x1, ptr(x2, -16));
        a.sub(x2, x2, asmjit::Imm(8));
        emitStoreSp(a, x2);
        a.b(end);
        a.bind(bail);
        if (g_fsrLazy) {
            // M4: the method mirror may be stale (per-call store
            // deleted) — derive the exit ip from x19's bcStartCache.
            a.ldr(x5, ptr(x19, (int)offsetof(JITMethod, bcStartCache)));
            a.add(x5, x5, asmjit::Imm(bcOffsetFromMethObj - g_bcStartDelta));
        } else {
        a.ldr(x5, ptr(x0, OFF_METHOD));
        a.add(x5, x5, asmjit::Imm(bcOffsetFromMethObj));
        }
        a.str(x5, ptr(x0, OFF_IP));
        a.mov(w3, asmjit::Imm(EXIT_ARITH_OVERFLOW));
        a.str(w3, ptr(x0, OFF_EXIT));
        emitSyncSpToState(a);
        a.ret(x30);
        a.bind(end);
        return true;
    }
    // Phase 3 multiplication on ARM64: 0x68 *.  Use smulh to detect
    // 64-bit overflow, then verify the result fits in 61-bit SmI.
    if (isPhase3MulOp(op)) {
        asmjit::Label bail = a.new_label();
        asmjit::Label end  = a.new_label();
        emitLoadSp(a, x2);
        a.ldr(x1, ptr(x2, -16));
        a.ldr(x4, ptr(x2, -8));
        a.eor(x5, x1, x4);
        a.sub(x6, x1, asmjit::Imm(1));
        a.orr(x5, x5, x6);
        a.tst(x5, asmjit::Imm(7));
        a.b_ne(bail);
        a.asr(x1, x1, asmjit::Imm(3));
        a.asr(x4, x4, asmjit::Imm(3));
        a.mul (x6, x1, x4);
        a.smulh(x7, x1, x4);
        a.cmp(x7, x6, asmjit::a64::asr(63));
        a.b_ne(bail);
        // Verify result fits in 61-bit SmI range.
        a.lsl(x7, x6, asmjit::Imm(3));
        a.asr(x7, x7, asmjit::Imm(3));
        a.cmp(x7, x6);
        a.b_ne(bail);
        a.lsl(x1, x6, asmjit::Imm(3));
        a.orr(x1, x1, asmjit::Imm(SMI_TAG));
        a.str(x1, ptr(x2, -16));
        a.sub(x2, x2, asmjit::Imm(8));
        emitStoreSp(a, x2);
        a.b(end);
        a.bind(bail);
        if (g_fsrLazy) {
            // M4: the method mirror may be stale (per-call store
            // deleted) — derive the exit ip from x19's bcStartCache.
            a.ldr(x5, ptr(x19, (int)offsetof(JITMethod, bcStartCache)));
            a.add(x5, x5, asmjit::Imm(bcOffsetFromMethObj - g_bcStartDelta));
        } else {
        a.ldr(x5, ptr(x0, OFF_METHOD));
        a.add(x5, x5, asmjit::Imm(bcOffsetFromMethObj));
        }
        a.str(x5, ptr(x0, OFF_IP));
        a.mov(w3, asmjit::Imm(EXIT_ARITH_OVERFLOW));
        a.str(w3, ptr(x0, OFF_EXIT));
        emitSyncSpToState(a);
        a.ret(x30);
        a.bind(end);
        return true;
    }
    // Phase 3 bitShift: on ARM64: 0x6C.  Positive b → left shift with
    // overflow check; negative b → arithmetic right shift.
    if (isPhase3ShiftOp(op)) {
        asmjit::Label bail = a.new_label();
        asmjit::Label end  = a.new_label();
        asmjit::Label rightShift = a.new_label();
        emitLoadSp(a, x2);
        a.ldr(x1, ptr(x2, -16));
        a.ldr(x4, ptr(x2, -8));
        a.eor(x5, x1, x4);
        a.sub(x6, x1, asmjit::Imm(1));
        a.orr(x5, x5, x6);
        a.tst(x5, asmjit::Imm(7));
        a.b_ne(bail);
        a.asr(x1, x1, asmjit::Imm(3));  // untag a
        a.asr(x4, x4, asmjit::Imm(3));  // untag b
        a.cmp(x4, asmjit::Imm(0));
        a.b_lt(rightShift);
        // Left shift: b in [0, 62].  Bail if >= 63.
        a.cmp(x4, asmjit::Imm(63));
        a.b_ge(bail);
        a.lsl(x6, x1, x4);              // result = a << b
        a.asr(x7, x6, x4);              // sanity: shifted-back equals a?
        a.cmp(x7, x1);
        a.b_ne(bail);
        // Retag: lsl-3 may overflow; verify with sar.
        a.lsl(x1, x6, asmjit::Imm(3));
        a.asr(x7, x1, asmjit::Imm(3));
        a.cmp(x7, x6);
        a.b_ne(bail);
        a.orr(x1, x1, asmjit::Imm(SMI_TAG));
        a.str(x1, ptr(x2, -16));
        a.sub(x2, x2, asmjit::Imm(8));
        emitStoreSp(a, x2);
        a.b(end);
        a.bind(rightShift);
        // b in [-62, -1].  Negate.
        a.neg(x4, x4);
        a.cmp(x4, asmjit::Imm(63));
        a.b_gt(bail);
        a.asr(x1, x1, x4);              // signed right shift
        // Retag — result is smaller than input, no overflow possible.
        a.lsl(x1, x1, asmjit::Imm(3));
        a.orr(x1, x1, asmjit::Imm(SMI_TAG));
        a.str(x1, ptr(x2, -16));
        a.sub(x2, x2, asmjit::Imm(8));
        emitStoreSp(a, x2);
        a.b(end);
        a.bind(bail);
        if (g_fsrLazy) {
            // M4: the method mirror may be stale (per-call store
            // deleted) — derive the exit ip from x19's bcStartCache.
            a.ldr(x5, ptr(x19, (int)offsetof(JITMethod, bcStartCache)));
            a.add(x5, x5, asmjit::Imm(bcOffsetFromMethObj - g_bcStartDelta));
        } else {
        a.ldr(x5, ptr(x0, OFF_METHOD));
        a.add(x5, x5, asmjit::Imm(bcOffsetFromMethObj));
        }
        a.str(x5, ptr(x0, OFF_IP));
        a.mov(w3, asmjit::Imm(EXIT_ARITH_OVERFLOW));
        a.str(w3, ptr(x0, OFF_EXIT));
        emitSyncSpToState(a);
        a.ret(x30);
        a.bind(end);
        return true;
    }
    // Phase 3 \\ (0x6A) and // (0x6D) — SmI floor-div / floor-mod on
    // ARM64 with sign-mismatch adjustment.  Pharo uses floor semantics
    // (a // b = floor(a/b); a \\ b = a - (a // b) * b), while ARM64
    // sdiv is trunc semantics.  Adjust by adding b to the remainder
    // when (a XOR b) is negative AND the remainder is non-zero.
    //   //: result = trunc quotient + adjustment (-1 if rem!=0 && signs differ)
    //   \\: result = trunc remainder + b      (if rem!=0 && signs differ)
    // Bails on non-SmI, divisor==0, or SmI overflow (impossible for
    // mod; possible for // only on a=INT_MIN/b=-1 — out of SmI range).
    if (isPhase3ModOp(op)) {
        asmjit::Label bail = a.new_label();
        asmjit::Label end  = a.new_label();
        asmjit::Label noAdjust = a.new_label();
        emitLoadSp(a, x2);
        a.ldr(x1, ptr(x2, -16));
        a.ldr(x4, ptr(x2, -8));
        // SmI tag check
        a.eor(x5, x1, x4);
        a.sub(x6, x1, asmjit::Imm(1));
        a.orr(x5, x5, x6);
        a.tst(x5, asmjit::Imm(7));
        a.b_ne(bail);
        // Untag both
        a.asr(x1, x1, asmjit::Imm(3));
        a.asr(x4, x4, asmjit::Imm(3));
        // Divisor == 0 → bail
        a.cbz(x4, bail);
        // sdiv x6 = a/b (trunc), msub x7 = a - q*b (trunc rem)
        a.sdiv(x6, x1, x4);
        a.msub(x7, x6, x4, x1);
        // Floor adjustment: if rem != 0 AND (a XOR b) < 0
        a.cbz(x7, noAdjust);
        a.eor(x5, x1, x4);
        a.tbz(x5, asmjit::Imm(63), noAdjust);
        if (op == 0x6A) {
            // \\: rem += b
            a.add(x7, x7, x4);
        } else {
            // //: q -= 1
            a.sub(x6, x6, asmjit::Imm(1));
        }
        a.bind(noAdjust);
        // Result is in x7 (rem) for \\ or x6 (quot) for //.  Retag.
        asmjit::a64::Gp result = (op == 0x6A) ? x7 : x6;
        // Blocker #4 trace (PHARO_T1_TRACE_MOD): log operands+result of \\ when
        // the dividend is hash-sized — reveals if scanFor:'s (hash \\ size) gets
        // a wrong hash or size at the actual JIT computation.  x1=a, x4=b (both
        // untagged), result=x6/x7.  Preserve them across the BLR.
        if (op == 0x6A && GET_DEBUG_BOOL(PHARO_T1_TRACE_MOD)) {
            a.sub(asmjit::a64::sp, asmjit::a64::sp, asmjit::Imm(48));
            a.str(x0,  ptr(asmjit::a64::sp, 0));
            a.str(x30, ptr(asmjit::a64::sp, 8));
            a.str(x1,  ptr(asmjit::a64::sp, 16));
            a.str(x2,  ptr(asmjit::a64::sp, 24));
            a.str(x4,  ptr(asmjit::a64::sp, 32));
            a.str(x7,  ptr(asmjit::a64::sp, 40));
            // args: x0=a, x1=b, x2=result, x3=state (x0 holds state before moves)
            a.mov(x3, x0);
            a.mov(x0, x1);
            a.mov(x1, x4);
            a.mov(x2, x7);
            a.mov(x9, asmjit::Imm((uint64_t)&jit_rt_trace_mod));
            a.blr(x9);
            a.ldr(x0,  ptr(asmjit::a64::sp, 0));
            a.ldr(x30, ptr(asmjit::a64::sp, 8));
            a.ldr(x1,  ptr(asmjit::a64::sp, 16));
            a.ldr(x2,  ptr(asmjit::a64::sp, 24));
            a.ldr(x4,  ptr(asmjit::a64::sp, 32));
            a.ldr(x7,  ptr(asmjit::a64::sp, 40));
            a.add(asmjit::a64::sp, asmjit::a64::sp, asmjit::Imm(48));
        }
        a.lsl(x1, result, asmjit::Imm(3));
        // Verify retag didn't overflow SmI (61 bits signed).
        a.asr(x5, x1, asmjit::Imm(3));
        a.cmp(x5, result);
        a.b_ne(bail);
        a.orr(x1, x1, asmjit::Imm(SMI_TAG));
        a.str(x1, ptr(x2, -16));
        a.sub(x2, x2, asmjit::Imm(8));
        emitStoreSp(a, x2);
        a.b(end);
        a.bind(bail);
        if (g_fsrLazy) {
            // M4: the method mirror may be stale (per-call store
            // deleted) — derive the exit ip from x19's bcStartCache.
            a.ldr(x5, ptr(x19, (int)offsetof(JITMethod, bcStartCache)));
            a.add(x5, x5, asmjit::Imm(bcOffsetFromMethObj - g_bcStartDelta));
        } else {
        a.ldr(x5, ptr(x0, OFF_METHOD));
        a.add(x5, x5, asmjit::Imm(bcOffsetFromMethObj));
        }
        a.str(x5, ptr(x0, OFF_IP));
        a.mov(w3, asmjit::Imm(EXIT_ARITH_OVERFLOW));
        a.str(w3, ptr(x0, OFF_EXIT));
        emitSyncSpToState(a);
        a.ret(x30);
        a.bind(end);
        return true;
    }
    // Short forward jumps (0xB0..0xC7) — see x86 version for protocol.
    if (op >= SistaV1::ShortJumpBase && op <= SistaV1::ShortJumpFalseLast) {
        int targetIdx = SistaV1::shortJumpTarget(op, globalIdx);
        // Out-of-range target = unreachable trailer byte; emit a bail.
        // (Pharo's CompiledMethod trailer follows the bytecodes and can
        // include short jumps with bogus offsets.)
        if (targetIdx < 0 || targetIdx >= (int)bcLabels.size()) {
            if (g_fsrLazy) {
                // M4: the method mirror may be stale (per-call store
                // deleted) — derive the exit ip from x19's bcStartCache.
                a.ldr(x5, ptr(x19, (int)offsetof(JITMethod, bcStartCache)));
                a.add(x5, x5, asmjit::Imm(bcOffsetFromMethObj - g_bcStartDelta));
            } else {
            a.ldr(x5, ptr(x0, OFF_METHOD));
            a.add(x5, x5, asmjit::Imm(bcOffsetFromMethObj));
            }
            a.str(x5, ptr(x0, OFF_IP));
            a.mov(w3, asmjit::Imm(EXIT_SEND));
            a.str(w3, ptr(x0, OFF_EXIT));
            emitSyncSpToState(a);
            a.ret(x30);
            return true;
        }
        if (op <= SistaV1::ShortJumpLast) {
            a.b(bcLabels[targetIdx]);
            return true;
        }
        bool jumpOnTrue = (op >= SistaV1::ShortJumpTrueBase
                           && op <= SistaV1::ShortJumpTrueLast);
        asmjit::Label mustBoolBail = a.new_label();
        asmjit::Label takeBranch   = a.new_label();
        asmjit::Label fallThrough  = a.new_label();

        emitLoadSp(a, x2);
        const bool tosCondJ = tosFam(kTosFamCondJump) && g_tosIn.valid;
        asmjit::a64::Gp boolReg = tosCondJ ? x26 : x1;
        if (tosCondJ) {
            emitTosVerify(a);   // simStack: bool already in x26
        } else {
            a.ldur(x1, asmjit::a64::ptr(x2, -8));   // x1 = TOS (not popped)
        }

        // ldp loads TRUEOOP+FALSEOOP in one instruction.  Both adjacent
        // at offsets 128 and 136 in JITState.  Slightly wasted load on
        // the fast-true case (we branch before reaching the falseOop
        // cmp), but ldp is typically L1-cycle-equivalent to a single ldr
        // and saves 1 instruction worth of i-cache.
        a.ldp(x4, x5, ptr(x0, OFF_TRUEOOP));
        a.cmp(boolReg, x4);
        a.b_eq(jumpOnTrue ? takeBranch : fallThrough);
        a.cmp(boolReg, x5);
        a.b_ne(mustBoolBail);
        a.b(jumpOnTrue ? fallThrough : takeBranch);

        a.bind(takeBranch);
        a.sub(x2, x2, asmjit::Imm(8));
        emitStoreSp(a, x2);
        a.b(bcLabels[targetIdx]);

        a.bind(fallThrough);
        a.sub(x2, x2, asmjit::Imm(8));
        emitStoreSp(a, x2);
        a.b(bcLabels[globalIdx + 1]);

        a.bind(mustBoolBail);
        if (g_fsrLazy) {
            // M4: the method mirror may be stale (per-call store
            // deleted) — derive the exit ip from x19's bcStartCache.
            a.ldr(x5, ptr(x19, (int)offsetof(JITMethod, bcStartCache)));
            a.add(x5, x5, asmjit::Imm(bcOffsetFromMethObj - g_bcStartDelta));
        } else {
        a.ldr(x5, ptr(x0, OFF_METHOD));
        a.add(x5, x5, asmjit::Imm(bcOffsetFromMethObj));
        }
        a.str(x5, ptr(x0, OFF_IP));
        a.mov(w3, asmjit::Imm(EXIT_MUST_BOOL));
        a.str(w3, ptr(x0, OFF_EXIT));
        emitSyncSpToState(a);
        a.ret(x30);
        return true;
    }
    // ARM64 send emit: see x86 version for IC probe rationale + layout.
    if (isPhase4SendOp(op)) {
        using namespace asmjit::a64;
        int nArgs = sendNArgs(op);

        // Per-site IC address into x5.  For xmethod-off (default),
        // x19 already holds state.jitMethod (hoisted at trampoline +
        // JIT_CALL); use it directly without the intermediate mov.
        // x11 is initialized to x19 only when the J2J emit needs it
        // separately (e.g., for save.jitMethod under xmethod-on, or
        // the bcStartCache load which is done lazily there).
        // For xmethod-on, state.jitMethod can change so reload.
        bool probeThis = g_debug.t1ICProbe;
        if (probeThis) {
            int icMin = g_debug.t1ICProbeMin;
            int icMax = g_debug.t1ICProbeMax;
            if (icMin >= 0 && (int)op < icMin) probeThis = false;
            if (icMax >= 0 && (int)op > icMax) probeThis = false;
        }
        // PMS B1 (docs/patched-ic-design.md §2): under the patched-sends
        // shape the icDataPtr chain below moves into the per-site cold
        // block (Lprobe) — the hot head never touches x5/x11.
        // DEFAULT-ON since 2026-06-10 (PMS B4): opt-out via
        // PHARO_T1_NO_PATCHED_SENDS=1; PHARO_T1_PATCHED_SENDS=1 is now
        // a no-op (kept for script compat).  B3 soak: 16372 tests,
        // zero knob-induced regressions, mirror verify silent.
        // PMS REQUIRES the inline-J2J site emit: the patched direct-
        // branch tail IS a J2J call sequence, and the patcher's word
        // offsets are derived against that shape.  With inline-J2J
        // disabled the sites emit without the tail, and patching them
        // corrupts unrelated words — PHARO_T1_NO_INLINE_J2J=1 alone
        // produced 5-7 deterministic startup DNUs (2026-06-12; this
        // also poisoned every knob-bisect that used NO_INLINE_J2J as
        // an arm since at least 2026-06-11).  Auto-disable.
        const bool patchedShape =
            !GET_DEBUG_BOOL(PHARO_T1_NO_PATCHED_SENDS)
            && !GET_DEBUG_BOOL(PHARO_T1_NO_INLINE_J2J)
            && probeThis;
        auto emitMaterializeX5 = [&]() {
            asmjit::a64::Gp jmReg = asmjit::a64::x19;
            // Pre-M1, xmethod mode couldn't trust x19 (the entry hoist
            // goes stale across cross-method J2J) and reloaded the
            // mirror.  Under M4-LAZY the mirror is the stale one and
            // x19 (M1 movs, unconditional) is the live identity — the
            // reload here was the icBuffer-corruption DNU at startup.
            if (g_debug.t1InlineJ2JXmethod && !g_fsrLazy) {
                a.ldr(x11, ptr(x0, OFF_JITMETHOD));
                jmReg = x11;
            }
            a.ldr(x5, ptr(jmReg, (int)offsetof(JITMethod, icBuffer)));
            // Skip the add when siteIdx == 0 (the first send in the
            // method — x5 already points at the right place).
            //
            // ENCODING TRAP (cascade-#3 third class, 2026-06-11): the
            // AArch64 add-immediate field is 12 bits (0-4095).  For
            // siteIdx >= 27 the offset (siteIdx*152 > 4095) is
            // UN-ENCODABLE and asmjit SILENTLY DROPPED the add (same
            // silent-drop class as the orr/and bitmask-immediate bug,
            // memory asmjit-arm64-invalid-logical-imm) — so every send
            // site >= 27 probed SITE 0's IC entries: key matched on the
            // same receiver class, dispatchCached served site 0's
            // cached METHOD for a different selector (minExtent's
            // bc-144 minHeight send ran #hResizing; the width Point got
            // a #shrinkWrap coordinate -> Morphic DNU cascade).  Use
            // the split add (lsl#12 + low) which always encodes.
            if (siteIdx != 0) {
                uint32_t icOff = (uint32_t)siteIdx * (uint32_t)IC_BYTES_PER_SITE;
                if (icOff <= 4095) {
                    a.add(x5, x5, asmjit::Imm(icOff));
                } else {
                    a.add(x5, x5, asmjit::Imm(icOff >> 12),
                          asmjit::a64::lsl(12));
                    if (icOff & 0xFFF)
                        a.add(x5, x5, asmjit::Imm(icOff & 0xFFF));
                }
            }
        };
        if (!patchedShape) emitMaterializeX5();

        // Deferred state setup: inline-spec paths skip the state
        // store entirely.  dispatchCached / miss / non-probe paths
        // emit it inline.  Mirrors x86 commit c4d325eb.
        if (probeThis) {
            // See x86 version for the protocol; this is the ARM64 mirror
            // with the inline-getter / inline-setter / returnsSelf
            // specializations.
            asmjit::Label imm = a.new_label();
            asmjit::Label haveKey = a.new_label();
            asmjit::Label miss = a.new_label();
            asmjit::Label dispatchCached = a.new_label();
            asmjit::Label tryGetter = a.new_label();
            asmjit::Label trySetter = a.new_label();
            asmjit::Label tryReturnsSelf = a.new_label();
            asmjit::Label tryReturnsLiteral = a.new_label();
            asmjit::Label tryTempReturn = a.new_label();      // W1
            asmjit::Label tryIntCmpReturn = a.new_label();    // W2
            asmjit::Label tryIntArithReturn = a.new_label();  // W3
            asmjit::Label tryEvenOdd = a.new_label();         // W6
            asmjit::Label tryMultiSlot = a.new_label();
            asmjit::Label trySistaCall = a.new_label();
            asmjit::Label tryPrimBitAnd = a.new_label();
            asmjit::Label tryPrimBitOr = a.new_label();
            asmjit::Label tryPrimBitXor = a.new_label();
            asmjit::Label tryPrimBitShift = a.new_label();
            asmjit::Label tryPrimMul = a.new_label();  // F5 R80: SmI mul via IC
            asmjit::Label tryPrimEq = a.new_label();   // F5 R81: == via IC
            asmjit::Label tryPrimIdentityHash = a.new_label();
            asmjit::Label tryPrimClass = a.new_label();
            asmjit::Label tryPrimAt = a.new_label();
            asmjit::Label tryPrimAtPut = a.new_label();
            asmjit::Label tryPrimSize = a.new_label();
            asmjit::Label tryPrimSmallFloatOp = a.new_label();
            asmjit::Label tryPrimBasicNew = a.new_label();
            asmjit::Label tryPrimBasicNewZero = a.new_label();  // 0-arg variant
            // dispatchCachedRestoreX5: inline-prim bail target that
            // reloads x5 from OFF_ICDATAPTR (where the original icDataPtr
            // was stashed before the inline-prim code clobbered x5 with
            // slotCount).  Falls through to dispatchCached.
            asmjit::Label dispatchCachedRestoreX5 = a.new_label();
            asmjit::Label j2jBailHeap = a.new_label();
            asmjit::Label j2jBailHeap2 = a.new_label();
            asmjit::Label endOfSend = a.new_label();
            // simStack B2c (design §5 RM-F1): when the send-result
            // family is on, EVERY inline-spec arrival at endOfSend is
            // retargeted through tosLrearm (ldur x26 from memory) so
            // the validity claimed after this send holds for ALL
            // arrival paths; resumeAfterCall re-arms from x1 and falls
            // through load-free.
            const bool tosSendRes = tosFam(kTosFamSendRes);
            asmjit::Label tosLrearm = a.new_label();
#if PHARO_J2J_SAVE_V2
            asmjit::Label resumeAfterCall = a.new_label();
#endif

            emitLoadSp(a, x2);
            int rcvrOffsetBytes = -8 * (nArgs + 1);
            if (nArgs == 0 && tosFam(kTosFamSendHead) && g_tosIn.valid) {
                // simStack: the receiver IS TOS — feed the probe head
                // from x26 (kills the load heading the IC chain).
                emitTosVerify(a);
                a.mov(x1, x26);
            } else {
                a.ldur(x1, ptr(x2, rcvrOffsetBytes));   // x1 = receiver
            }
            a.and_(x4, x1, asmjit::Imm(0x7));
            a.cbnz(x4, imm);
            // Defensive: an object-tagged Oop must be a canonical user-space
            // pointer — bits 48-63 zero on macOS/Linux arm64.  Receivers with
            // top bits set are JIT classifier-bit leaks (task #10 — J2J entry
            // bit 60, getter bit 63, etc.) and dereferencing them SIGSEGV's
            // (Roassal3 #inverseTransformPiOrZero: crash at fault addr
            // 0x86fe800000000008).  Route to MISS so the chain loop does a
            // safe full lookup (or surfaces a real error) instead of crashing.
            // PHARO_T1_LEAK_GUARD_OFF: drop the 2-instr defensive check
            // (emit-time knob; the leaks it guarded — task #10 J2J/getter
            // classifier bits in receivers — have been root-caused since;
            // suite-validate before making this the default).
            asmjit::Label LmissNoX5 = a.new_label();   // PMS: leak-guard edge
            asmjit::Label Lprobe    = a.new_label();   // PMS: in-site generic probe
            if (!GET_DEBUG_BOOL(PHARO_T1_LEAK_GUARD_OFF)) {
                a.lsr(x4, x1, asmjit::Imm(48));
                a.cbnz(x4, patchedShape ? LmissNoX5 : miss);
            }
            a.ldr(w4, ptr(x1));               // low 32 bits of header
            a.and_(w4, w4, asmjit::Imm(0x3FFFFF));
            a.b(haveKey);
            a.bind(imm);
            a.orr(w4, w4, asmjit::Imm(0x80000000U));
            a.bind(haveKey);

            if (patchedShape) {
                // PMS head (design §2.1): patched key compare + patched
                // direct branch.  W0/W1 carry kImpossibleKeyHi16 until
                // linkSendSite patches them; W2 says `b Lprobe` so both
                // match (impossible) and mismatch take the generic
                // probe — knob-on-unlinked behavior == today's + 5
                // inert ALU instructions.  Register contracts at the
                // labels: Lprobe/LmissNoX5 require x0=state x1=receiver
                // x2=spCopy x4=lookupKey x25=sp; they materialize x5.
                // FORBIDDEN FOREVER: any change that makes W2 reachable
                // without a key match.
                asmjit::Label keyMovz = a.new_label();
                a.bind(keyMovz);
                a.movz(w6, 0);                              // W0: key lo16
                a.movk(w6, kImpossibleKeyHi16, 16);         // W1: key hi16 (commit/unlink word)
                a.cmp(w4, w6);
                a.b_ne(Lprobe);
                a.b(Lprobe);                                // W2: -> b T when linked
                asmjit::Label tailL = a.new_label();
                asmjit::Label tailBranchL = a.new_label();
                bool hasTail = false;
#if PHARO_J2J_SAVE_V2
                // PMS B1b: linked-J2J tail skeleton (design §2.2).
                // Between unconditional branches — reachable ONLY via a
                // patched W2 (key matched), with the head's contract:
                // x0=state x1=receiver x2=spCopy(caller sp) x25=sp.
                // Mirrors the generic xmethod J2J body with calleeJM
                // from patched immediates (W3-W5): the per-send gate
                // loads were evaluated ONCE at link time in C++ and do
                // not exist here.  Same V2 packing gate as the generic
                // emit; sites that fail it keep tailOffset=0 (head
                // patchable but never linkable — linkSendSite refuses).
                {
                    uint32_t resumeBcOff = (uint32_t)globalIdx + 1;
                    if (resumeBcOff <= 0xFFFu && nArgs <= 0xF) {
                        hasTail = true;
                        a.bind(tailL);
                        a.movz(x10, 0);            // W3: calleeJM lo16
                        a.movk(x10, 0, 16);        // W4: calleeJM mid16
                        a.movk(x10, 0, 32);        // W5: calleeJM hi16
                        if (g_fsrCursor) {
                            // FSR M2 v1: cursor from x23; limit load off
                            // the critical path (stores no longer wait
                            // on the cursor load).
                            a.mov(x6, x23);
                            a.ldr(x14, ptr(x0, OFF_J2J_SAVE_LIMIT));
                        } else {
                        static_assert(OFF_J2J_SAVE_LIMIT == OFF_J2J_SAVE_CURSOR + 8,
                                      "cursor/limit adjacency required for ldp fold");
                        a.ldp(x6, x14, ptr(x0, OFF_J2J_SAVE_CURSOR));
                        }
                        a.cmp(x6, x14);
                        a.b_hs(Lprobe);            // pool full -> generic full-bail path
                        a.adr(x14, resumeAfterCall);
                        {
                            uint16_t packed16 = (uint16_t)(
                                ((nArgs & 0xF) << 12) | (resumeBcOff & 0xFFF));
                            a.movk(x14, packed16, 48);
                        }
                        // V2 packed save push (record layout bit-identical
                        // to the generic push above).
                        emitLoadSp(a, x15);
                        a.ldr(x4, ptr(x0, OFF_RECEIVER));
                        a.stp(x15, x4, ptr_post(x6, JSV_SIZE));
                        emitLoadTempBase(a, x15);
                        // size-40 layout: tempBase@-24, packedResume@-16,
                        // closure@-8 (was -16/-8 at size 32).
                        a.stp(x15, x14, ptr(x6, -24));
                        a.str(xzr, ptr(x6, -8));  // JSV_CLOSURE (Phase 1: 0)
                        // Callee state from the patched calleeJM — all
                        // level-1 loads off an immediate.  offsetof ONLY
                        // (the JM-byte-offset off-by-one lesson).
                        if (g_fsrX19) a.mov(x19, x10);  // FSR M1: activation commit
                        if (!g_fsrLazy || g_fsrLazyVerify) {
                        a.ldr(x13, ptr(x10, (int)offsetof(JITMethod, compiledMethodOop)));
                        a.str(x10, ptr(x0, OFF_JITMETHOD));
                        a.str(x13, ptr(x0, OFF_METHOD));
                        a.add(x13, x13, asmjit::Imm(16));
                        a.str(x13, ptr(x0, OFF_LITERALS));
                        a.mov(w13, asmjit::Imm(nArgs));
                        a.str(w13, ptr(x0, OFF_ARGCOUNT));
                        }
                        a.str(x6, ptr(x0, OFF_J2J_SAVE_CURSOR));
                        if (g_fsrCursor) a.mov(x23, x6);  // FSR M2 v1
                        a.ldr(x13, ptr(x0, OFF_J2J_DEPTH));
                        a.add(x13, x13, asmjit::a64::x20);
                        a.str(x13, ptr(x0, OFF_J2J_DEPTH));
                        a.str(x1, ptr(x0, OFF_RECEIVER));
                        a.sub(x13, x2, asmjit::Imm(nArgs * 8));
                        emitStoreTempBase(a, x13);
                        if (!g_fsrLazy || g_fsrLazyVerify) {
                        a.ldr(x14, ptr(x10, (int)offsetof(JITMethod, bcStartCache)));
                        a.str(x14, ptr(x0, OFF_IP));
                        }
                        // Dynamic nil-fill: callee tempCount is a link-time
                        // unknown (any gate-passing callee may link here).
                        a.ldrb(w15, ptr(x10, (int)offsetof(JITMethod, tempCount)));
                        asmjit::Label tInitLoop = a.new_label();
                        asmjit::Label tInitDone = a.new_label();
                        a.add(x14, x13, asmjit::Imm(nArgs * 8));
                        a.lsl(w15, w15, asmjit::Imm(3));
                        a.add(x15, x13, x15);
                        a.mov(x4, asmjit::Imm(nilBits));
                        a.bind(tInitLoop);
                        a.cmp(x14, x15);
                        a.b_hs(tInitDone);
                        a.str(x4, ptr(x14));
                        a.add(x14, x14, asmjit::Imm(8));
                        a.b(tInitLoop);
                        a.bind(tInitDone);
                        emitStoreSp(a, x15);
                        a.bind(tailBranchL);
                        a.b(Lprobe);               // W6: -> b calleeEntry when linked
                    }
                }
#endif
                a.bind(LmissNoX5);
                emitMaterializeX5();
                a.b(miss);
                a.bind(Lprobe);
                emitMaterializeX5();
                if (g_patchLabelsPtr) {
                    PatchSiteLabels pl;
                    pl.siteIdx = (uint32_t)siteIdx;
                    pl.keyMovz = keyMovz;
                    pl.tail = tailL;
                    pl.tailBranch = tailBranchL;
                    pl.hasTail = hasTail;
                    g_patchLabelsPtr->push_back(pl);
                }
            }

            a.ldr(x6, ptr(x5));               // icData[0] key
            a.cmp(x4, x6);
            // jit-may22b Step 4: walk IC slots 0-2 inline.  Slot-0
            // monomorphic fast path stays minimal — branch on
            // hit (predicted taken) to the existing probe-done
            // continuation.  On slot-0 miss, walks slots 1-2 before
            // bailing.  Catches ~12K cold-start polymorphic DUPs per
            // bench-suite cold-start (jit-may20b Step 8.3 analysis)
            // that previously paid a JIT→C++→JIT round-trip.
            // OPT-IN via PHARO_T1_IC_POLY_WALK=1 (DebugSettings.cpp; an
            // older comment here claimed an opt-out knob that does not
            // exist — design §14 #4).
            asmjit::Label probeDone = a.new_label();
            if (g_debug.t1ICPolyWalk
                    || !GET_DEBUG_BOOL(PHARO_T1_NO_IC_POLY_WALK)) {
                asmjit::Label slot1Hit = a.new_label();
                a.b_eq(probeDone);              // slot 0 hit (common)
                // Slot 1
                a.ldr(x6, ptr(x5, 24));
                a.cmp(x4, x6);
                a.b_eq(slot1Hit);
                // Slot 2
                a.ldr(x6, ptr(x5, 48));
                a.cmp(x4, x6);
                a.b_ne(miss);
                a.add(x5, x5, asmjit::Imm(48));
                a.b(probeDone);
                a.bind(slot1Hit);
                a.add(x5, x5, asmjit::Imm(24));
                a.bind(probeDone);
            } else {
                a.b_ne(miss);
                a.bind(probeDone);
            }
            // jit-may20b Step 8.1: removed `cbz x4, miss` safety check.
            // Verified safe — no receiver computes classKey=0.
            // (Original Step 8.1 included an unrelated nArgs==0 sub elision
            // that broke gate-OFF; that one rolled back below.  This cbz
            // removal stays.)
            if (g_debug.t1ProbeAlwaysMiss) {
                a.b(miss);                     // diagnostic: never take HIT
            }
            // PHARO_T1_HIT_FORCE_DISPATCH=1: on IC HIT, skip ALL inline-spec
            // dispatch (getter/setter/prim/J2J) and go straight to the plain
            // cached dispatch.  Isolates "a spec corrupts" (this PASSES) from
            // "the common dispatchCached path corrupts" (this still FAILS).
            // x5 = icDataPtr here, which dispatchCached needs.  (blocker #4)
            if (GET_DEBUG_BOOL(PHARO_T1_HIT_FORCE_DISPATCH)) {
                a.b(dispatchCached);
            }

            // x7 = extras
            a.ldr(x7, ptr(x5, 16));
            // Debug: count every IC HIT (PHARO_T1_INLINE_J2J=1 telemetry)
            {
                if (pharo::g_debug.t1InlineJ2J_Env) {
                    a.mov(x4, asmjit::Imm((uint64_t)&g_inlineJ2J_dbg_ic_hits));
                    a.ldr(x6, ptr(x4));
                    a.add(x6, x6, asmjit::Imm(1));
                    a.str(x6, ptr(x4));
                }
            }
            a.cbz(x7, dispatchCached);
            // Debug: count IC HITs where extra is set but bit 60 isn't
            {
                if (pharo::g_debug.t1InlineJ2J_Env) {
                    asmjit::Label haveBit60 = a.new_label();
                    a.tbnz(x7, asmjit::Imm(60), haveBit60);
                    a.mov(x4, asmjit::Imm((uint64_t)&g_inlineJ2J_dbg_extra_no_bit60));
                    a.ldr(x6, ptr(x4));
                    a.add(x6, x6, asmjit::Imm(1));
                    a.str(x6, ptr(x4));
                    a.bind(haveBit60);
                }
            }

            // ===== INLINE J2J (PHARO_T1_INLINE_J2J=1, opt-in 2026-05-17) =====
            // Bit 60 (J2J_ENTRY_BIT): callee is JIT-compiled; tail-call its
            // entry directly instead of round-tripping JIT→C++→JIT via the
            // chain loop's activateMethod path.  Saves ~500 cycles/send on
            // recursive sends.  MVP: self-recursive only (caller == callee).
            // Mirrors stencils.cpp:1733-1877's `j2j_direct_call:` block.
            // See deferred.md A6 for full design.
            const bool inlineJ2J = !GET_DEBUG_BOOL(PHARO_T1_NO_INLINE_J2J)
                && !GET_DEBUG_BOOL(PHARO_T1_NO_J2J_BRANCH);  // TEST: NO_J2J_BRANCH now gates the WHOLE block
            if (inlineJ2J) {
                asmjit::Label tryInlineJ2J = a.new_label();
                asmjit::Label j2jBail      = a.new_label();
                // Bit 59 (BLOCK_VALUE_BIT) takes precedence over bit 60.
                // Block-value IC entries may have bit 59 alone (when the
                // value: method is not safe to J2J-call) — check 59 first.
                if (g_debug.t1InlineBlockValue) {
                    a.tbnz(x7, asmjit::Imm(59), tryInlineJ2J);
                }
                // jit-may20b Step 8.4: bit 55 (SISTA_BIT) takes precedence
                // over bit 60.  Sista's monomorphic inlining is strictly
                // more powerful than J2J's straight-call.  Bit-55 stays
                // unset until the Sista bail-protocol bug (Step 4) lands,
                // so this branch is dead in current builds.
                if (g_debug.t1InlineSistaCall) {
                    a.tbnz(x7, asmjit::Imm(55), trySistaCall);
                }
                // jit-may20b Step 10: primKind 18 (basicNew:) takes
                // precedence over bit 60.  For prim 71 methods, inline-J2J
                // tail-calls to a JIT stub that immediately exits with
                // EXIT_SEND to run the prim in C++ — same work as
                // dispatchCached but via a longer detour.  Going direct
                // to our basicNew: helper saves the extra round-trip.
                // Same applies if primKind matches at:/at:put:/size:
                // those have proper inline emits that should win over
                // J2J's stub-and-bail.
                if (nArgs == 1 && g_debug.t1InlinePrimBasicNew) {
                    a.lsr(x6, x7, asmjit::Imm(48));
                    a.and_(x6, x6, asmjit::Imm(0x1F));
                    a.cmp(x6, asmjit::Imm(18));
                    a.b_eq(tryPrimBasicNew);
                }
                // jit-may23 T4: primKind 17 (basicNew 0-arg) inline dispatch.
                if (nArgs == 0 && g_debug.t1InlinePrimBasicNew) {
                    a.lsr(x6, x7, asmjit::Imm(48));
                    a.and_(x6, x6, asmjit::Imm(0x1F));
                    a.cmp(x6, asmjit::Imm(17));
                    a.b_eq(tryPrimBasicNewZero);
                }
                // jit-may23 T1+T2: primKind 11/12/13/19 (SmI bit ops)
                // and 21/22/23 (SmallFloat ops) take precedence over bit
                // 60 (J2J).  Without this, the bit/float prim methods
                // get bit 60 set when JIT-compiled and the inline emits
                // are never reached (g_primBitOp_hits / g_primFloatOp_hits
                // both 0).  Same "wired but unreached" pattern as retLit.
                if (nArgs == 1 && g_debug.t1InlinePrimBitOps) {
                    a.lsr(x6, x7, asmjit::Imm(48));
                    a.and_(x6, x6, asmjit::Imm(0x1F));
                    a.cmp(x6, asmjit::Imm(11));
                    a.b_eq(tryPrimBitAnd);
                    a.cmp(x6, asmjit::Imm(12));
                    a.b_eq(tryPrimBitOr);
                    a.cmp(x6, asmjit::Imm(19));
                    a.b_eq(tryPrimBitXor);
                    a.cmp(x6, asmjit::Imm(13));
                    a.b_eq(tryPrimBitShift);
                    // F5 R80: primKind 9 = SmI multiply.
                    a.cmp(x6, asmjit::Imm(9));
                    a.b_eq(tryPrimMul);
                    // F5 R81: primKind 10 = == (identical).
                    a.cmp(x6, asmjit::Imm(10));
                    a.b_eq(tryPrimEq);
                    // SmallFloat: primKinds 21/22/23 are contiguous.
                    // sub 21 then cmp <3 catches all three.
                    a.sub(x6, x6, asmjit::Imm(21));
                    a.cmp(x6, asmjit::Imm(3));
                    a.b_lo(tryPrimSmallFloatOp);
                }
                // Bit 60 set → try inline J2J; works for any receiver tag
                // (SmI receivers benefit too, unlike inline-getter/setter).
                a.tbnz(x7, asmjit::Imm(60), tryInlineJ2J);

                // (fall through to existing inline-spec dispatch)
                // Inline specializations need heap receiver for getter/setter,
                // SmI receiver for primKind bitwise dispatch.
                {
                    asmjit::Label fallSmIBranch = a.new_label();
                    // jit-may22b: dispatch returnsLiteral (bit 58)
                    // BEFORE the heap/SmI split so it works for both
                    // (SmI Integer>>isInteger etc. is the common case).
                    // F5 R83: extended to all nArgs.  retLit emit now
                    // handles arg-dropping like returnsSelf does.
                    if (GET_DEBUG_BOOL(PHARO_T1_INLINE_RETURNS_LITERAL)) {
                        a.tbnz(x7, asmjit::Imm(58), tryReturnsLiteral);
                    }
                    // W1: bit 54 = TempReturn `^ arg N` — works for any
                    // receiver (no class-specific dispatch needed; the
                    // target body just reads tempBase[N]).  Dispatched
                    // BEFORE the heap/SmI split.
                    if (nArgs >= 1 && GET_DEBUG_BOOL(PHARO_T1_INLINE_TEMP_RETURN)) {
                        a.tbnz(x7, asmjit::Imm(54), tryTempReturn);
                    }
                    // W2: bit 53 = IntCmpReturn `^ self cmp arg`.  Needs
                    // SmI receiver AND SmI arg.  Caller pre-checked
                    // receiver class; arg checked in emit.
                    if (nArgs == 1 && GET_DEBUG_BOOL(PHARO_T1_INLINE_INT_CMP_RETURN)) {
                        a.tbnz(x7, asmjit::Imm(53), tryIntCmpReturn);
                    }
                    // W3: bit 52 = IntArithReturn `^ self op arg` (+/-/*).
                    if (nArgs == 1 && GET_DEBUG_BOOL(PHARO_T1_INLINE_INT_ARITH_RETURN)) {
                        a.tbnz(x7, asmjit::Imm(52), tryIntArithReturn);
                    }
                    // W6: even/odd predicate (nArgs == 0).  B6 review F1:
                    // a single-bit tbnz on bit 51 is an UNSOUND
                    // discriminator — primKind values with bit 51 set
                    // (kPrimKindClass=24=0b11000 first among 0-arg pks)
                    // would be stolen and, for an immediate receiver,
                    // tryEvenOdd would return a Boolean from a #class
                    // send.  Decode the full 5-bit field: W6 entries are
                    // exactly field 8 (kind 0) / 9 (kind 1).
                    if (nArgs == 0 && GET_DEBUG_BOOL(PHARO_T1_INLINE_EVEN_ODD)) {
                        a.lsr(x6, x7, asmjit::Imm(48));
                        a.and_(x6, x6, asmjit::Imm(0x1F));
                        a.cmp(x6, asmjit::Imm(8));
                        a.b_eq(tryEvenOdd);
                        a.cmp(x6, asmjit::Imm(9));
                        a.b_eq(tryEvenOdd);
                    }
                    a.tst(x1, asmjit::Imm(0x7));
                    if (nArgs == 1 && g_debug.t1InlinePrimBitOps) {
                        a.b_ne(fallSmIBranch);
                    } else {
                        a.b_ne(dispatchCached);
                    }
                    // Heap receiver path.  Arity gates (see below): getter
                    // 0-arg, setter 1-arg — a stale/racy IC extra must not
                    // hijack a wrong-arity send.
                    // Dispatch-A-side getter entry: DEFAULT OFF
                    // (opt-in via PHARO_T1_GETTER_IN_J2J for debugging).
                    // CONTROLLED BISECT (catch22/23, 2026-06-10): with
                    // this branch emitted, the MAX_IC=1 config corrupts
                    // (~5-20%/layout, wrong values at correct depth);
                    // with it removed, 30/30 clean while the plain-probe
                    // getter entries stay on; a same-length dummy env
                    // var does NOT cure (not layout luck).  Re-loading
                    // x2/x1 from state before the branch REDUCED but did
                    // not eliminate the failures (catch24 rep 31) — the
                    // dispatch-A bail paths leave more than registers
                    // inconsistent (suspect: partial state commits, e.g.
                    // state.ip = callee bcStart in the J2J callee-setup
                    // before a late bail).  Until that is root-caused,
                    // getter-classified sends on this path take
                    // dispatchCached — a path they were already on after
                    // the J2J bail; benchFib/cfib are unaffected.
                    if (g_debug.t1InlineGetter && nArgs == 0
                            && GET_DEBUG_BOOL(PHARO_T1_GETTER_IN_J2J)) {
                        asmjit::Label notGetter63 = a.new_label();
                        a.tbz(x7, asmjit::Imm(63), notGetter63);
                        emitLoadSp(a, x2);
                        a.ldur(x1, ptr(x2, rcvrOffsetBytes));
                        a.b(tryGetter);
                        a.bind(notGetter63);
                    }
                    if (g_debug.t1InlineSetter && nArgs == 1) {
                        a.tbnz(x7, asmjit::Imm(62), trySetter);
                    }
                    if (g_debug.t1InlineReturnsSelf) {
                        a.tbnz(x7, asmjit::Imm(61), tryReturnsSelf);
                    }
                    // returnsLiteral (bit 58) dispatched before
                    // the heap/SmI split above.
                    if (nArgs == 0 && GET_DEBUG_BOOL(PHARO_T1_INLINE_MULTISLOT)) {
                        a.tbnz(x7, asmjit::Imm(57), tryMultiSlot);
                    }
                    // B6 (docs/patched-ic-design.md §11): kPrimKindClass
                    // dispatch for bit-60-clear entries.  prim-111
                    // callees can't compile (no JITMethod, bit 60 never
                    // set), so pk-classified #class entries previously
                    // ALWAYS exited via dispatchCached — the per-send
                    // C++ round trip behind the measured 18x 'o class'
                    // gap.  Heap receivers only (phase 1a; this point is
                    // below the heap/imm split).  x1=receiver(heap),
                    // x2=sp copy, x7=extras; tryPrimClass clobbers x5
                    // legally ONLY because it cannot bail — any future
                    // bail must stash via OFF_ICDATAPTR +
                    // dispatchCachedRestoreX5 (B6 review, verified-safe
                    // list).
                    if (nArgs == 0 && !GET_DEBUG_BOOL(PHARO_T1_NO_INLINE_CLASS)) {
                        a.lsr(x6, x7, asmjit::Imm(48));
                        a.and_(x6, x6, asmjit::Imm(0x1F));
                        a.cmp(x6, asmjit::Imm(kPrimKindClass));
                        a.b_eq(tryPrimClass);
                    }
                    a.b(dispatchCached);
                    // SmI receiver path: check primKind for inline bitwise.
                    if (nArgs == 1 && g_debug.t1InlinePrimBitOps) {
                        a.bind(fallSmIBranch);
                        a.lsr(x6, x7, asmjit::Imm(48));
                        a.and_(x6, x6, asmjit::Imm(0x1F));
                        a.cmp(x6, asmjit::Imm(11));
                        a.b_eq(tryPrimBitAnd);
                        a.cmp(x6, asmjit::Imm(12));
                        a.b_eq(tryPrimBitOr);
                        a.cmp(x6, asmjit::Imm(19));
                        a.b_eq(tryPrimBitXor);
                        a.cmp(x6, asmjit::Imm(13));
                        a.b_eq(tryPrimBitShift);
                        a.b(dispatchCached);
                    }
                }

                // Per-bail-reason counters — gated on PHARO_T1_INLINE_J2J
                // env var (debug-only).  When env var off (production
                // default), counters NOT emitted — saves ~24 bytes per
                // inline-J2J emit site = ~100+ bytes for typical fib-like
                // methods.  When env var on, the per-bail counter writes
                // are emitted for diagnostic visibility.
                const bool inlineJ2JCounters = pharo::g_debug.t1InlineJ2J_Env;
                asmjit::Label j2jBailZero = a.new_label();
                asmjit::Label j2jBailFull = a.new_label();
                asmjit::Label j2jBailSelf = a.new_label();
                auto emitIncCounter = [&](uint64_t addr) {
                    if (!inlineJ2JCounters) return;
                    a.mov(x15, asmjit::Imm(addr));
                    a.ldr(x14, ptr(x15));
                    a.add(x14, x14, asmjit::Imm(1));
                    a.str(x14, ptr(x15));
                };

                a.bind(tryInlineJ2J);
                // (Stale `afterSend` label removed — referenced only by
                // comments, never bound or branched to.)
                // Block-value inline (PHARO_T1_INLINE_BLOCK_VALUE=1).
                // When BLOCK_VALUE_BIT (bit 59) is set, the IC site is a
                // value/value:/value:... send to a FullBlockClosure.  The
                // IC's entryAddr (low 48 of x7) points at the value:
                // method's JIT entry (or stub), NOT the user-block's
                // compiled code.  We delegate validation + J2J save +
                // state setup + capture copy to a C helper, then `br x0`
                // to enter the block's JIT entry.
                if (g_debug.t1InlineBlockValue) {
                    asmjit::Label tryBlockValue = a.new_label();
                    asmjit::Label blockValueDone = a.new_label();
                    a.tbnz(x7, asmjit::Imm(59), tryBlockValue);
                    a.b(blockValueDone);
                    a.bind(tryBlockValue);
                    // Counter: try (helper about to be called).
                    a.mov(x14, asmjit::Imm((uint64_t)&g_blockValue_tries));
                    a.ldr(x15, ptr(x14));
                    a.add(x15, x15, asmjit::Imm(1));
                    a.str(x15, ptr(x14));
                    // Helper signature: (state, nArgs, resumeAddr)
                    //   x0 in: state ptr; x0 out: blockEntry or NULL
                    //   x1: nArgs (compile-time constant)
                    //   x2: resumeAddr (= afterSend label)
                    // Save state ptr + LR across the blr; blr clobbers x0.
                    a.sub(sp, sp, asmjit::Imm(16));
                    a.str(x0,  ptr(sp, 0));
                    a.str(x30, ptr(sp, 8));
                    a.mov(w1, asmjit::Imm(nArgs));
                    // resumeAddr points directly at endOfSend (skip the
                    // afterSend `b endOfSend` indirection).
                    a.adr(x2, endOfSend);
                    emitSyncSpToState(a);  // sp-residency: helper reads state.sp
                    a.mov(x9, asmjit::Imm((uint64_t)
                        &jit_rt_inline_block_value_prep));
                    a.blr(x9);
                    // x0 = blockEntry or NULL; move to x9 before
                    // restoring x0 = state.
                    a.mov(x9, x0);
                    a.ldr(x0,  ptr(sp, 0));
                    a.ldr(x30, ptr(sp, 8));
                    a.add(sp, sp, asmjit::Imm(16));
                    {
                        asmjit::Label bailBV = a.new_label();
                        asmjit::Label hitBV  = a.new_label();
                        a.cbz(x9, bailBV);
                        a.mov(x14, asmjit::Imm((uint64_t)&g_blockValue_hits));
                        a.ldr(x15, ptr(x14));
                        a.add(x15, x15, asmjit::Imm(1));
                        a.str(x15, ptr(x14));
                        a.b(hitBV);
                        a.bind(bailBV);
                        a.mov(x14, asmjit::Imm((uint64_t)&g_blockValue_bails));
                        a.ldr(x15, ptr(x14));
                        a.add(x15, x15, asmjit::Imm(1));
                        a.str(x15, ptr(x14));
                        a.b(j2jBail);
                        a.bind(hitBV);
                    }
                    // Block entry expects x0 = state; we already restored.
                    // The helper pushed the J2J save with resumeAddr =
                    // afterSend, so the block's return prelude tail-calls
                    // back to afterSend → endOfSend.
                    //
                    // FSR M1/M4: the br enters the BLOCK's compiled code
                    // mid-protocol (no JIT_CALL hoist, no trampoline, no
                    // prologue) — x19 still holds the CALLER's JM.  The
                    // prep helper just wrote the block's JM to the mirror
                    // (C++ write, never gated), so reload from it; without
                    // this the block reads the caller's literalsCache
                    // under LAZY (the startup Array>>do: garbage-selector
                    // DNU, RESUME-MISMATCH forensics 2026-06-11).
                    if (g_fsrX19)
                        a.ldr(x19, ptr(x0, OFF_JITMETHOD));
                    a.br(x9);
                    a.bind(blockValueDone);
                }
                // 2026-05-17 loop iter 2: self-recursive inline-J2J.
                // Implements the simplest viable subset: callee == caller
                // (same JITMethod).  For benchFib-style recursion, all
                // inner sends are either prims (no chain-break) or
                // self-recursive (also inlined), so no chain-break risk.
                // For non-self-recursive case, bail (chain-break protocol
                // unresolved — see deferred.md A6).
                //
                // Register state at entry:
                //   x0  = state ptr
                //   x1  = receiver (from IC HIT setup)
                //   x5  = icDataPtr
                //   x7  = ic.extra (bit 60 set, entryAddr in low 48 bits)
                // nArgs is known at JIT compile time.
                //
                // Resume label (after_send) goes into save.resumeAddr.
                // Return prelude tail-calls there when callee returns.
                // (afterSend declared above so block-value inline can use it.)
                asmjit::Label j2jBailSelf2 = a.new_label();

                // Compute entryAddr (x9).  calleeJM (x10) only needed
                // in the xmethod-on / debug-counter paths (xmethod-off
                // emits use x11 = callerJM == calleeJM for self-rec).
                // Skip the `sub x10, ...` when xmethod is off and the
                // debug-counters aren't enabled — saves 1 instr per push.
                a.and_(x9, x7, asmjit::Imm(0x0000FFFFFFFFFFFFULL));
                const bool needCalleeJM =
                    g_debug.t1InlineJ2JXmethod || inlineJ2JCounters;
                if (needCalleeJM) {
                    a.sub(x10, x9, asmjit::Imm((int)sizeof(JITMethod)));
                    // Eδ.2b (2026-05-24): count IC HITs where callee
                    // qualifies for canSkipJ2JSave (offset 49 in JM).
                    // Counters-on builds ONLY: xmethod went default-on
                    // (2026-06-10), so the old `xmethod || counters`
                    // gate silently put this 7-instr global RMW on
                    // every production inline-J2J hit (2.4M/cfib run,
                    // shared cache line across all call sites).  The
                    // measurement it served is settled (~50% qualify).
                    if (inlineJ2JCounters) {
                        asmjit::Label skipCount = a.new_label();
                        a.ldrb(w14, ptr(x10, (int)offsetof(JITMethod, canSkipJ2JSave)));   // canSkipJ2JSave byte
                        a.cbz(w14, skipCount);
                        a.mov(x14, asmjit::Imm(
                            (uint64_t)&g_canSkipJ2JSave_ic_hits));
                        a.ldr(x15, ptr(x14));
                        a.add(x15, x15, asmjit::Imm(1));
                        a.str(x15, ptr(x14));
                        a.bind(skipCount);
                    }
                }

                // Self-recursive check via SELF_REC_BIT (bit 56) in the
                // IC extra word.  The IC patcher sets this bit when
                // callerCM == calleeCM at IC-fill time (see
                // Interpreter::upgradeICToJ2J + patchJITICAfterSend).
                //
                // Register source for callerJM: x11 is only loaded at the
                // IC-probe entry when xmethod is on (see line ~2950).  In
                // xmethod-off mode (default) callerJM is hoisted to x19
                // by the trampoline.  Pick the right one — the old code
                // assumed x11 always held callerJM, but after the x19
                // hoist commit that's only true in xmethod-on.
                asmjit::a64::Gp callerJMReg2 = g_debug.t1InlineJ2JXmethod
                    ? asmjit::a64::x11
                    : asmjit::a64::x19;

                // Debug counters: stash last-seen values — gated on env
                // var so production emit doesn't carry the overhead.
                // CM loads only happen in the debug branch.
                if (inlineJ2JCounters) {
                    a.ldr(x12, ptr(callerJMReg2, 0));    // caller's CM oop
                    a.ldr(x13, ptr(x10, (int)offsetof(JITMethod, compiledMethodOop)));    // callee's CM oop
                    a.mov(x14, asmjit::Imm((uint64_t)&g_inlineJ2J_dbg_caller_method));
                    a.str(x12, ptr(x14));
                    a.mov(x14, asmjit::Imm((uint64_t)&g_inlineJ2J_dbg_callee_method));
                    a.str(x13, ptr(x14));
                    a.mov(x14, asmjit::Imm((uint64_t)&g_inlineJ2J_dbg_extra));
                    a.str(x7, ptr(x14));
                }

                // CROSS-METHOD inline-J2J.  Default ON (opt-out via
                // PHARO_T1_NO_INLINE_J2J_XMETHOD=1; see DebugSettings.cpp).
                // The historical "known to corrupt state" reputation
                // traced to the JM-offset bug fixed 2026-06-09 below:
                // the gate chain read byte 47 as isStubOnEntry (real:
                // canBailMidMethod) and byte 46 as canBailMidMethod
                // (real: hasNLR), so stub-on-entry callees slipped
                // through (save leak -> state corruption) while nearly
                // all real returning callees were rejected.
                // PHARO_T1_INLINE_J2J_XMETHOD_MAX=N bisection-limits
                // the number of cross-method fires (default: unlimited).
                const bool xmethod = g_debug.t1InlineJ2JXmethod;
                if (g_debug.t1InlineJ2JXmethodMax >= 0) {
                    g_xmethod_max = (uint64_t)g_debug.t1InlineJ2JXmethodMax;
                }
                asmjit::Label sameMethodSkipUpdate = a.new_label();
                if (xmethod) {
                    // Bit 56 set → self-recursive, skip the cross-method
                    // update.  Bit 56 not set → fall through to the
                    // cross-method gates and update.
                    a.tbnz(x7, asmjit::Imm(56), sameMethodSkipUpdate);
                    // Cross-method gates use callee's methodHeader,
                    // numICEntries, isStubOnEntry, canBailMidMethod.
                    if (!inlineJ2JCounters) {
                        a.ldr(x13, ptr(x10, (int)offsetof(JITMethod, compiledMethodOop)));   // callee CM (for state.method)
                    }
                    // XGATE fold (patched-ic-design.md §5): extras bit 57
                    // carries the precomputed gate verdict (set at IC
                    // fill/upgrade, recomputed on callee recompile) —
                    // skip the 4-load cascade.  Production shape only;
                    // counters/cap/log modes keep the cascade for
                    // per-gate attribution.
                    asmjit::Label xgateFast = a.new_label();
                    const bool xgateFoldable = !inlineJ2JCounters
                        && g_debug.t1InlineJ2JXmethodMax < 0
                        && !pharo::g_debug.t1InlineJ2JXmethodLog
                        && !GET_DEBUG_BOOL(PHARO_T1_NO_XGATE_FOLD);
                    if (xgateFoldable)
                        a.tbnz(x7, asmjit::Imm(57), xgateFast);
                    // Per-gate bail counters (emitted only when
                    // inlineJ2JCounters; production emit unchanged).
                    // emitIncCounter clobbers x14/x15 — both scratch here.
                    auto emitGateBail = [&](uint64_t counterAddr) {
                        // Branch target that bumps the per-gate counter
                        // then bails.  Caller emits the inverted-condition
                        // branch around it.
                        emitIncCounter(counterAddr);
                        a.b(j2jBailSelf2);
                    };
                    emitIncCounter((uint64_t)&g_xgate_enter);
                    // 2026-06-12: prim callees WITH a prologue are
                    // admitted (codeStart IS the prim attempt; body
                    // gates are cold for them) — mirror xmethodGateOk.
                    // gatesAdmit skips the numIC/b47/b46 checks.
                    asmjit::Label gatesAdmitPrim = a.new_label();
                    asmjit::Label gatesNoPrim = a.new_label();
                    a.ldr(x4, ptr(x10, (int)offsetof(JITMethod, methodHeader)));            // methodHeader (decoded)
                    a.tbz(x4, asmjit::Imm(16), gatesNoPrim);
                    a.ldrb(w4, ptr(x10,
                        (int)offsetof(JITMethod, hasPrimPrologue)));
                    if (GET_DEBUG_BOOL(PHARO_T1_NO_J2J_PRIM_PROLOGUE)) {
                        a.mov(w4, asmjit::Imm(0));  // opt-out: never admit
                    }
                    if (inlineJ2JCounters) {
                        asmjit::Label ok = a.new_label();
                        a.cbnz(w4, ok);
                        emitGateBail((uint64_t)&g_xgate_bail_prim);
                        a.bind(ok);
                    } else {
                        a.cbz(w4, j2jBailSelf2);  // prim w/o prologue → bail
                    }
                    // prologue-prim admitted past the PRIM gate only;
                    // numIC/b47/b46 still apply (sub-bisect 2026-06-12).
                    a.bind(gatesNoPrim);
                    (void)gatesAdmitPrim;
                    // Callee send-site count gate.  Historical form:
                    // numICEntries == 0 (leaf callees only) — guarded the
                    // materialize-bail wrong-result bug (stale
                    // state.j2jDepth, fixed 2026-06-09).
                    // PHARO_T1_XMETHOD_MAX_IC=N admits callees with up to
                    // N IC sites for A/B of lever (c).
                    a.ldrh(w4, ptr(x10, (int)offsetof(JITMethod, numICEntries)));
                    {
                        const int maxIC = GET_DEBUG_INT(PHARO_T1_XMETHOD_MAX_IC);
                        if (inlineJ2JCounters) {
                            asmjit::Label ok = a.new_label();
                            if (maxIC > 0) {
                                a.cmp(w4, asmjit::Imm(maxIC));
                                a.b_ls(ok);
                            } else {
                                a.cbz(w4, ok);
                            }
                            emitGateBail((uint64_t)&g_xgate_bail_numic);
                            a.bind(ok);
                        } else if (maxIC > 0) {
                            a.cmp(w4, asmjit::Imm(maxIC));
                            a.b_hi(j2jBailSelf2);
                        } else {
                            a.cbnz(w4, j2jBailSelf2);
                        }
                    }
                    // isStubOnEntry — stubs never invoke the return
                    // prelude → save would leak.
                    //
                    // OFFSET BUG FIXED 2026-06-09: this gate chain used
                    // raw offsets 47/46 believing them to be
                    // isStubOnEntry/canBailMidMethod.  Real layout:
                    // 46=hasNLR 47=canBailMidMethod 48=isStubOnEntry.
                    // Consequences of the off-by-one: (a) the 46-read hit
                    // hasNLR, which the default-ON t1NlrTailOnly scan
                    // sets for EVERY method containing a return opcode
                    // 0x58-0x5C — so virtually all real cross-method
                    // callees bailed (574K/930K in the cfib bench, the
                    // entire 43x-vs-Cog cross-method send gap); (b) the
                    // real isStubOnEntry byte was never checked, so
                    // stub-on-entry callees (hasNLR=false: the nlr scan
                    // is skipped for non-isReal) passed every gate and
                    // were inline-J2J-called — the likely source of the
                    // historical "xmethod corrupts state" failures.
                    a.ldrb(w4, ptr(x10,
                        (int)offsetof(JITMethod, isStubOnEntry)));
                    if (inlineJ2JCounters) {
                        asmjit::Label ok = a.new_label();
                        a.cbz(w4, ok);
                        emitGateBail((uint64_t)&g_xgate_bail_b47);
                        a.bind(ok);
                    } else {
                        a.cbnz(w4, j2jBailSelf2);
                    }
                    // canBailMidMethod — mid-method bails corrupt the
                    // caller frame via the inline-activate path.
                    // Default ADMIT since 2026-06-12 (the corruption
                    // this gate guarded was the PMS/NO_INLINE_J2J
                    // interaction, fixed; requalified 0/6+0/8 ladders,
                    // sieve 1028, suite soak).  Opt-out restores the
                    // refusal.
                    if (!GET_DEBUG_BOOL(PHARO_T1_NO_BAILMID_CALLEES)) {
                        a.mov(w4, asmjit::Imm(0));
                    } else
                    a.ldrb(w4, ptr(x10,
                        (int)offsetof(JITMethod, canBailMidMethod)));
                    if (inlineJ2JCounters) {
                        asmjit::Label ok = a.new_label();
                        a.cbz(w4, ok);
                        emitGateBail((uint64_t)&g_xgate_bail_b46);
                        a.bind(ok);
                    } else {
                        a.cbnz(w4, j2jBailSelf2);
                    }
                    a.bind(gatesAdmitPrim);
                    // Fire counting + cap check: only emitted when a cap
                    // is set (PHARO_T1_INLINE_J2J_XMETHOD_MAX >= 0) or
                    // counters are on.  Production default (uncapped, no
                    // counters) pays nothing — was 2 loads + add + store
                    // + cmp on EVERY cross-method fire (~3.7M/run).
                    if (g_debug.t1InlineJ2JXmethodMax >= 0
                            || inlineJ2JCounters) {
                        a.mov(x14, asmjit::Imm((uint64_t)&g_xmethod_count));
                        a.ldr(x15, ptr(x14));
                        a.add(x15, x15, asmjit::Imm(1));
                        a.str(x15, ptr(x14));
                        a.mov(x14, asmjit::Imm((uint64_t)&g_xmethod_max));
                        a.ldr(x14, ptr(x14));
                        a.cmp(x15, x14);
                        if (inlineJ2JCounters) {
                            asmjit::Label ok = a.new_label();
                            a.b_ls(ok);
                            emitGateBail((uint64_t)&g_xgate_bail_cap);
                            a.bind(ok);
                        } else {
                            a.b_hi(j2jBailSelf2);
                        }
                    }
                    a.bind(xgateFast);
                    // E2 2026-05-24: cross-method state.{jitMethod,method,
                    // literals,argCount} update RELOCATED to after the
                    // save-full check (line ~3843), because that bail
                    // falls through to dispatchCached/miss which compute
                    // state.ip = state.method + bcOffsetFromMethObj.  If
                    // state.method is calleeCM at that point, state.ip
                    // lands at calleeCM + caller_bcOff = wrong heap
                    // address.  X+BV crash root cause.
                    if (pharo::g_debug.t1InlineJ2JXmethodLog) {
                        using namespace asmjit::a64;
                        if (!inlineJ2JCounters) {
                            // xlog helper signature uses x12 = callerCM.
                            a.ldr(x12, ptr(x11, 0));
                        }
                        // Save the FULL caller-saved set.  The old save
                        // set (x0,x7,x9-x13,x30) let the blr clobber
                        // x1-x6 — x1 is the send RECEIVER and x2+ are
                        // args, so enabling this logger corrupted every
                        // logged call (instant crash in the callee's IC
                        // probe with x1=0).  That made every historical
                        // XMETHOD_LOG debugging session "confirm" that
                        // xmethod corrupts state.  Fixed 2026-06-10.
                        a.sub(sp, sp, asmjit::Imm(128));
                        a.stp(x0, x1,   ptr(sp, 0));
                        a.stp(x2, x3,   ptr(sp, 16));
                        a.stp(x4, x5,   ptr(sp, 32));
                        a.stp(x6, x7,   ptr(sp, 48));
                        a.stp(x8, x9,   ptr(sp, 64));
                        a.stp(x10, x11, ptr(sp, 80));
                        a.stp(x12, x13, ptr(sp, 96));
                        a.str(x30,      ptr(sp, 112));
                        a.mov(x1, x10);
                        a.mov(x2, x11);
                        a.mov(x3, x13);
                        a.mov(x4, x12);
                        a.mov(x5, asmjit::Imm((uint64_t)&jit_rt_xmethod_log));
                        a.blr(x5);
                        a.ldp(x0, x1,   ptr(sp, 0));
                        a.ldp(x2, x3,   ptr(sp, 16));
                        a.ldp(x4, x5,   ptr(sp, 32));
                        a.ldp(x6, x7,   ptr(sp, 48));
                        a.ldp(x8, x9,   ptr(sp, 64));
                        a.ldp(x10, x11, ptr(sp, 80));
                        a.ldp(x12, x13, ptr(sp, 96));
                        a.ldr(x30,      ptr(sp, 112));
                        a.add(sp, sp, asmjit::Imm(128));
                    }
                    a.bind(sameMethodSkipUpdate);
                } else {
                    // Default (xmethod off): bit 56 not set → cross-method,
                    // which we can't handle here.  Bail to dispatchCached.
                    a.tbz(x7, asmjit::Imm(56), j2jBailSelf2);
                }

                // Eδ.2c (2026-05-24): saveless inline-J2J emit, opt-in
                // via PHARO_T1_CAN_SKIP_J2J_SAVE=1.  When callee's
                // JITMethod::canSkipJ2JSave bit (offset 49) is set, take
                // a blr-based call path that skips the J2J save push
                // entirely.  Callee's normalReturn (no save pushed →
                // j2jDepth == j2jEntryDepth → fall through) stores
                // retval to OFF_RETVAL and rets via x30 (which we set
                // via blr).  Caller-side: save state.{sp,receiver,
                // tempBase,ip,literals,method,jitMethod,argCount} + x30
                // to sp-stash before blr; after blr restore state and
                // apply send-return semantics (state.sp -= nArgs*8;
                // store retval at state.sp-8).  Currently SELF-REC only
                // (xmethod is opt-in default-OFF), so state.method/
                // literals/jitMethod/argCount don't change across the
                // call.  Compatible with the unchanged callee binary —
                // the callee's existing j2jDepth > j2jEntryDepth check
                // routes saveless calls to normalReturn automatically.
                //
                // POSITIONED BEFORE WARM-J2J GATE: canSkipJ2JSave callees
                // have numICEntries == 0 → cannot trigger the materialize-
                // bail wrong-result bug the warm gate guards against.
                // Skipping the gate for these saves the gate-loop cost
                // and enables saveless emit in default config.
                if (!GET_DEBUG_BOOL(PHARO_T1_NO_CAN_SKIP_J2J_SAVE)
                        && (int64_t)g_t1CompileSeq2
                               >= GET_DEBUG_INT(PHARO_T1_SAVELESS_MIN_COMPILE)
                        && nArgs <= GET_DEBUG_INT(PHARO_T1_SAVELESS_MAX_ARGS)) {
                    asmjit::Label normalJ2J = a.new_label();
                    // Eδ.2d (2026-06-10): saveless now covers CROSS-METHOD
                    // leaf callees too (xmethod-on builds).  Cross needs
                    // state.{method,jitMethod,literals,argCount} set for
                    // the callee and restored after — the stash grows to
                    // 96 bytes and the callee-state update mirrors the E2
                    // block.  canSkipJ2JSave callees are straight-line
                    // (no sends, no mid-method bails) so they cannot GC,
                    // yield, or exit-to-C++ during the blr.  In
                    // xmethod-OFF builds (x13/calleeCM not loaded) the
                    // path stays self-rec-only as before.
                    const bool crossSaveless = xmethod;
                    if (!crossSaveless) {
                        a.tbz(x7, asmjit::Imm(56), normalJ2J);
                    }
                    // x10 may not be loaded when xmethod is off and
                    // counters are off.  Load it from x9 (entryAddr).
                    if (!needCalleeJM) {
                        a.sub(x10, x9, asmjit::Imm((int)sizeof(JITMethod)));
                    }
                    a.ldrb(w14, ptr(x10, (int)offsetof(JITMethod, canSkipJ2JSave)));  // canSkipJ2JSave byte
                    a.cbz(w14, normalJ2J);
                    // Exclude prim-prologue callees: their entry code
                    // reads receiver/args from REGISTERS (the
                    // register-reading-entry convention), which this blr
                    // does not populate.  The save-push path handles them
                    // via its own protocol.
                    a.ldrb(w14, ptr(x10, (int)offsetof(JITMethod, hasPrimPrologue)));
                    a.cbnz(w14, normalJ2J);
                    // Bisect (PHARO_T1_SAVELESS_NO_EXTRAS): only callees
                    // with tempCount == nArgs (no nil-fill) — isolates
                    // the dynamic nil-fill shape from the proven
                    // no-extras shape (cfib->incc).
                    if (GET_DEBUG_BOOL(PHARO_T1_SAVELESS_NO_EXTRAS)) {
                        a.ldrb(w14, ptr(x10, (int)offsetof(JITMethod, tempCount)));
                        a.cmp(w14, asmjit::Imm(nArgs));
                        a.b_ne(normalJ2J);
                    }
                    // Cascade #2 PREVENTION (headroom reservation,
                    // 2026-06-11): when the pool is within 64 saves of
                    // full, take the save-push path instead — its
                    // j2jBailFull handles full gracefully.  This keeps
                    // the retro-save pool-full handoff (ExitRetroFull)
                    // unreachable for bail ret-chains up to 64 deep,
                    // sidestepping the single-slot clobber a chained
                    // handoff would need to solve.  Cost: 1 ldp + sub +
                    // cmp + branch on the saveless fast path.
                    if (g_fsrCursor) {
                        a.mov(x14, x23);                  // FSR M2 v1
                        a.ldr(x15, ptr(x0, OFF_J2J_SAVE_LIMIT));
                    } else {
                    a.ldp(x14, x15, ptr(x0, OFF_J2J_SAVE_CURSOR));
                    }
                    // NULL-CURSOR GUARD (cascade-#3 force-resume residual,
                    // 2026-06-11): the interp resume loop runs with
                    // j2jSaveCursor = limit = NULL (J2J disabled).  The
                    // unsigned subtraction below then underflows
                    // (0 - 64*JSV wraps huge) and 0 >= huge is FALSE, so
                    // the saveless path engaged WITH NO POOL — its retro
                    // pool-full check (cursor==limit==0 -> "full") exited
                    // ExitRetroFull(13), which has NO C++ handler (it was
                    // believed unreachable) and fell into the switches'
                    // default arms with the elided frame lost (the
                    // DateParser parseNextPattern MUSTBOOL).  A null
                    // cursor must mean "no J2J" -> normalJ2J.
                    a.cbz(x14, normalJ2J);
                    a.sub(x15, x15, asmjit::Imm(64 * JSV_SIZE));
                    a.cmp(x14, x15);
                    a.b_hs(normalJ2J);

                    // === SAVELESS PATH ===
                    // Save caller state to sp-stash.  Layout (96 bytes in
                    // cross-capable mode, 48 self-rec-only):
                    //   [0]  = caller state.sp (pre-send)
                    //   [8]  = caller state.receiver
                    //   [16] = caller state.tempBase
                    //   [24] = caller state.ip
                    //   [32] = x30 (caller's lr — blr will overwrite)
                    //   [40] = (pad)
                    //   cross only:
                    //   [48] = caller state.method
                    //   [56] = caller state.jitMethod
                    //   [64] = caller state.literals
                    //   [72] = caller state.argCount (32-bit, zext)
                    // FSR M3: the caller's j2jEntryCursor (64-bit) rides in
                    // the stash — cross mode has [80-95] free; the base
                    // stash grows 48->64 with the slot at [48].
                    const int stashSize = crossSaveless ? 96
                                        : (g_fsrNodepth ? 64 : 48);
                    const int entryCurSlot = crossSaveless ? 80 : 48;
                    (void)entryCurSlot;
                    a.sub(sp, sp, asmjit::Imm(stashSize));
                    emitLoadSp(a, x4);                   // x4=sp (sweep: was ldp sp+recv)
                    a.ldr(x5, ptr(x0, OFF_RECEIVER)); // x5=recv
                    a.stp(x4, x5, ptr(sp, 0));
                    emitLoadTempBase(a, x6);
                    a.ldr(x12, ptr(x0, OFF_IP));
                    a.stp(x6, x12, ptr(sp, 16));
                    a.str(x30, ptr(sp, 32));
                    // Pin j2jEntryDepth = j2jDepth across the call.  The
                    // callee's return prelude pops a pending save and
                    // TAIL-JUMPS whenever depth > entryDepth — with a
                    // caller mid-J2J-chain (e.g. cfib's self-rec saves
                    // pending) that hijacks the return past our post-blr
                    // restore, leaking the sp-stash on every call until
                    // the stack guard page (the Eδ.2d launch crash).
                    // Saved entryDepth lives in the stash pad slot [40].
                    a.ldr(w14, ptr(x0, OFF_J2J_DEPTH));
                    a.ldr(w15, ptr(x0, OFF_J2J_ENTRY_DEPTH));
                    a.str(w15, ptr(sp, 40));
                    a.str(w14, ptr(x0, OFF_J2J_ENTRY_DEPTH));
                    if (g_fsrNodepth) {
                        // FSR M3: pin the per-activation cursor baseline
                        // (x23 = live cursor, M2 default-on) and stash the
                        // caller's.
                        a.ldr(x15, ptr(x0, OFF_J2J_ENTRY_CURSOR));
                        a.str(x15, ptr(sp, entryCurSlot));
                        a.str(x23, ptr(x0, OFF_J2J_ENTRY_CURSOR));
                    }
                    if (crossSaveless) {
                        a.ldr(x6, ptr(x0, OFF_METHOD));
                        if (g_fsrLazy) {
                            // M4: the jitMethod MIRROR is stale (per-call
                            // store deleted); x19 is the live identity and
                            // the restore side's `mov x19, x12` is the
                            // caller-x19 carrier (FR-6) — stash x19 itself.
                            a.stp(x6, x19, ptr(sp, 48));
                        } else {
                        a.ldr(x12, ptr(x0, OFF_JITMETHOD));
                        a.stp(x6, x12, ptr(sp, 48));
                        }
                        a.ldr(x6, ptr(x0, OFF_LITERALS));
                        a.ldr(w12, ptr(x0, OFF_ARGCOUNT));
                        a.stp(x6, x12, ptr(sp, 64));
                    }

                    // Telemetry: bump fire counter (debug builds only —
                    // skip in production for the leaner call).
                    if (inlineJ2JCounters) {
                        a.mov(x14, asmjit::Imm(
                            (uint64_t)&g_canSkipJ2JSave_fires));
                        a.ldr(x15, ptr(x14));
                        a.add(x15, x15, asmjit::Imm(1));
                        a.str(x15, ptr(x14));
                    }

                    // Set up callee state.
                    //   state.receiver = x1 (new recv, already in x1)
                    //   state.tempBase = caller_sp - nArgs*8
                    //   state.ip       = calleeJM->bcStartCache
                    //   state.sp       = new tempBase + tempCount*8
                    //                    (nil-fill extras, dynamic count)
                    //   cross: state.{jitMethod,method,literals,argCount}
                    // NOTE: x13 = calleeCM (loaded in the xmethod gate
                    // block) — use x6/x12 as scratch, never x13.
                    a.str(x1, ptr(x0, OFF_RECEIVER));
                    a.sub(x6, x4, asmjit::Imm(nArgs * 8));   // new tempBase
                    emitStoreTempBase(a, x6);
                    if (!g_fsrLazy || g_fsrLazyVerify) {
                    a.ldr(x14, ptr(x10, (int)offsetof(JITMethod, bcStartCache)));
                    a.str(x14, ptr(x0, OFF_IP));
                    }
                    if (crossSaveless && g_fsrX19) a.mov(x19, x10);  // FSR M1
                    if (crossSaveless && (!g_fsrLazy || g_fsrLazyVerify)) {
                        a.str(x10, ptr(x0, OFF_JITMETHOD));
                        a.str(x13, ptr(x0, OFF_METHOD));
                        a.add(x12, x13, asmjit::Imm(16));    // literals = CM+16
                        a.str(x12, ptr(x0, OFF_LITERALS));
                        a.mov(w12, asmjit::Imm(nArgs));
                        a.str(w12, ptr(x0, OFF_ARGCOUNT));
                    }
                    if (crossSaveless) {
                        // Dynamic nil-fill: callee tempCount from its JM.
                        asmjit::Label initLoop = a.new_label();
                        asmjit::Label initDone = a.new_label();
                        a.ldrb(w15, ptr(x10, (int)offsetof(JITMethod, tempCount)));
                        a.cmp(w15, asmjit::Imm(nArgs));
                        a.b_ls(initDone);                    // no extras
                        a.add(x14, x6, asmjit::Imm(nArgs * 8));
                        a.lsl(w15, w15, asmjit::Imm(3));
                        a.add(x15, x6, x15);
                        a.mov(x12, asmjit::Imm(nilBits));
                        a.bind(initLoop);
                        a.str(x12, ptr_post(x14, 8));
                        a.cmp(x14, x15);
                        a.b_lo(initLoop);
                        emitStoreSp(a, x15);
                        a.bind(initDone);
                        // extras==0 case: state.sp stays caller_sp ==
                        // tempBase + nArgs*8 (correct).
                    } else {
                        // Self-rec: callee.tempCount == compile-time
                        // callerTempCount.
                        int extras = callerTempCount - nArgs;
                        if (extras > 0 && extras <= 8) {
                            a.mov(x12, asmjit::Imm(nilBits));
                            for (int k = 0; k < extras; k++) {
                                a.str(x12, ptr(x6, (nArgs + k) * 8));
                            }
                            a.add(x12, x4, asmjit::Imm(extras * 8));
                            emitStoreSp(a, x12);
                        } else if (extras != 0) {
                            asmjit::Label initLoop = a.new_label();
                            asmjit::Label initDone = a.new_label();
                            a.add(x14, x6, asmjit::Imm(nArgs * 8));
                            a.add(x15, x6, asmjit::Imm(callerTempCount * 8));
                            a.mov(x12, asmjit::Imm(nilBits));
                            a.bind(initLoop);
                            a.cmp(x14, x15);
                            a.b_hs(initDone);
                            a.str(x12, ptr(x14));
                            a.add(x14, x14, asmjit::Imm(8));
                            a.b(initLoop);
                            a.bind(initDone);
                            emitStoreSp(a, x15);
                        }
                    }

                    // blr to callee entry.  x9 already holds entryAddr.
                    a.blr(x9);

                    // Non-EXIT_RETURN recovery.  canSkipJ2JSave callees
                    // CAN still bail mid-method: the flag excludes only
                    // cond-jump bailers, but any SmI arithmetic emits an
                    // ExitArithOverflow bail (confirmed via brk trap —
                    // exit 6 fired at startup scale; the controlled
                    // cfib->incc site just never overflowed).  Recovery:
                    // retroactively build the pool save this call elided
                    // (exactly what the save-push path would have written)
                    // and RET with the callee's exit state — the C++ bail
                    // handler then materializes the chain as if the call
                    // had used the save-push path all along.
                    {
                        asmjit::Label exitOk = a.new_label();
                        a.ldr(w14, ptr(x0, OFF_EXIT));
                        a.cmp(w14, asmjit::Imm(EXIT_RETURN));
                        a.b_eq(exitOk);
                        // -- cold path: build the retro-save --
                        // Pool space check: if full, trap (would have
                        // bailed at the save-push too; ~impossible).
                        if (g_fsrCursor) {
                            a.mov(x14, x23);              // FSR M2 v1
                            a.ldr(x15, ptr(x0, OFF_J2J_SAVE_LIMIT));
                        } else {
                        a.ldp(x14, x15, ptr(x0, OFF_J2J_SAVE_CURSOR));
                        }
                        a.cmp(x14, x15);
                        asmjit::Label haveRoom = a.new_label();
                        a.b_lo(haveRoom);
                        {
                            // Graceful pool-full handoff (cascade #2,
                            // was brk 0xDEAE): hand the elided frame to
                            // C++ via the retro fields + ExitRetroFull;
                            // the handler materializes the pool, resets
                            // the cursor, pushes this save, and
                            // re-dispatches the callee's ORIGINAL bail
                            // reason (currently in OFF_EXIT).
                            a.ldr(w15, ptr(x0, OFF_EXIT));
                            a.str(w15, ptr(x0, OFF_RETRO_ORIG_EXIT));
                            a.ldp(x4, x5, ptr(sp, 0));     // sp + recv
                            a.str(x4, ptr(x0, OFF_RETRO_SP));
                            a.str(x5, ptr(x0, OFF_RETRO_RECV));
                            a.ldr(x4, ptr(sp, 16));         // tempBase
                            a.str(x4, ptr(x0, OFF_RETRO_TEMPBASE));
                            a.adr(x5, resumeAfterCall);
                            {
                                uint32_t rOff = (uint32_t)globalIdx + 1;
                                uint16_t p16 = (uint16_t)
                                    (((nArgs & 0xF) << 12) | (rOff & 0xFFF));
                                a.movk(x5, p16, 48);
                            }
                            a.str(x5, ptr(x0, OFF_RETRO_RESUME));
                            a.mov(w15, asmjit::Imm(EXIT_RETRO_FULL));
                            a.str(w15, ptr(x0, OFF_EXIT));
                            // unwind the machine stash exactly like the
                            // normal retro epilogue, then exit to C++.
                            a.ldr(w4, ptr(sp, 40));
                            a.str(w4, ptr(x0, OFF_J2J_ENTRY_DEPTH));
                            if (g_fsrNodepth) {
                                a.ldr(x4, ptr(sp, entryCurSlot));
                                a.str(x4, ptr(x0, OFF_J2J_ENTRY_CURSOR));
                            }
                            a.ldr(x30, ptr(sp, 32));
                            a.add(sp, sp, asmjit::Imm(stashSize));
                            emitSyncSpToState(a);
                            a.ret(x30);
                        }
                        a.bind(haveRoom);
                        // save.{sp,receiver} from stash[0,8]
                        a.ldp(x4, x5, ptr(sp, 0));
                        a.stp(x4, x5, ptr_post(x14, JSV_SIZE));   // cursor += save size
#if PHARO_J2J_SAVE_V2
                        // V2 retro-save: tempBase + packed resume.
                        // The resume continuation is this site's
                        // resumeAfterCall; pack bcOff|nArgs via movk
                        // exactly like the normal push.
                        a.ldr(x4, ptr(sp, 16));            // tempBase
                        a.adr(x5, resumeAfterCall);
                        {
                            uint32_t resumeBcOff = (uint32_t)globalIdx + 1;
                            uint16_t packed16 = (uint16_t)
                                (((nArgs & 0xF) << 12) | (resumeBcOff & 0xFFF));
                            a.movk(x5, packed16, 48);
                        }
                        // size-40: tempBase@-24, packedResume@-16, closure@-8.
                        a.stp(x4, x5, ptr(x14, -24));      // tempBase + packedResume
                        a.str(xzr, ptr(x14, -8));          // JSV_CLOSURE (Phase 1: 0)
#else
                        // save.tempBase from stash[16]; save.ip = post-send
                        // ip = callerCM + bcOffsetFromMethObj + 1 (the
                        // stash ip is the stale state.ip, NOT post-send).
                        a.ldr(x4, ptr(sp, 16));
                        if (crossSaveless) {
                            a.ldr(x5, ptr(sp, 48));         // caller CM
                        } else {
                            a.ldr(x5, ptr(x19, 0));         // callerJM->CM (self-rec: x19 convention)
                        }
                        a.add(x5, x5, asmjit::Imm(bcOffsetFromMethObj + 1));
                        a.stp(x4, x5, ptr(x14, -40));       // tempBase + ip
                        // save.jitMethod (caller) + save.resumeAddr
                        if (crossSaveless) {
                            a.ldr(x4, ptr(sp, 56));         // caller jitMethod
                        } else {
                            a.mov(x4, x19);
                        }
                        a.adr(x5, endOfSend);
                        a.stp(x4, x5, ptr(x14, -24));
                        // save.sendArgCount
                        if (nArgs == 0) {
                            a.str(wzr, ptr(x14, -8));
                        } else {
                            a.mov(w4, asmjit::Imm(nArgs));
                            a.str(w4, ptr(x14, -8));
                        }
#endif  // PHARO_J2J_SAVE_V2 (retro-save)
                        // Commit cursor; bump depth (+totalCalls via x20).
                        a.str(x14, ptr(x0, OFF_J2J_SAVE_CURSOR));
                        if (g_fsrCursor) a.mov(x23, x14);  // FSR M2 v1
                        a.ldr(x4, ptr(x0, OFF_J2J_DEPTH));
                        a.add(x4, x4, asmjit::a64::x20);
                        a.str(x4, ptr(x0, OFF_J2J_DEPTH));
                        // Restore the caller's entryDepth (the pin would
                        // otherwise fence the retro-save from the C++
                        // materialize accounting).
                        a.ldr(w4, ptr(sp, 40));
                        a.str(w4, ptr(x0, OFF_J2J_ENTRY_DEPTH));
                        if (g_fsrNodepth) {
                            a.ldr(x4, ptr(sp, entryCurSlot));
                            a.str(x4, ptr(x0, OFF_J2J_ENTRY_CURSOR));
                        }
                        // Unwind the machine stash and propagate the bail
                        // to the caller's caller (trampoline/tryExecute)
                        // with the CALLEE's exit state intact.
                        a.ldr(x30, ptr(sp, 32));
                        a.add(sp, sp, asmjit::Imm(stashSize));
                        emitSyncSpToState(a);
                        a.ret(x30);
                        a.bind(exitOk);
                    }

                    // === Post-return: restore caller state ===
                    a.ldp(x4, x5, ptr(sp, 0));     // saved sp + receiver
                    a.ldp(x6, x12, ptr(sp, 16));   // saved tempBase + ip
                    a.ldr(x30, ptr(sp, 32));
                    // Restore the caller's j2jEntryDepth (pinned above).
                    a.ldr(w14, ptr(sp, 40));
                    a.str(w14, ptr(x0, OFF_J2J_ENTRY_DEPTH));
                    if (g_fsrNodepth) {
                        a.ldr(x14, ptr(sp, entryCurSlot));
                        a.str(x14, ptr(x0, OFF_J2J_ENTRY_CURSOR));
                    }
                    a.str(x5, ptr(x0, OFF_RECEIVER));
                    emitStoreTempBase(a, x6);
                    a.str(x12, ptr(x0, OFF_IP));
                    if (crossSaveless) {
                        a.ldp(x6, x12, ptr(sp, 48));   // method + jitMethod
                        a.str(x6, ptr(x0, OFF_METHOD));
                        a.str(x12, ptr(x0, OFF_JITMETHOD));
                        if (g_fsrX19) a.mov(x19, x12);  // FSR M1: caller-restore
                        a.ldp(x6, x12, ptr(sp, 64));   // literals + argCount
                        a.str(x6, ptr(x0, OFF_LITERALS));
                        a.str(w12, ptr(x0, OFF_ARGCOUNT));
                    }
                    a.add(sp, sp, asmjit::Imm(stashSize));

                    // Retval semantics: new sp = caller_sp - nArgs*8;
                    // retval written at new_sp-8 (the receiver slot).
                    a.ldr(x12, ptr(x0, OFF_RETVAL));
                    a.sub(x14, x4, asmjit::Imm(nArgs * 8));
                    a.stur(x12, ptr(x14, -8));
                    emitStoreSp(a, x14);

                    // Clear exitReason (callee set EXIT_RETURN).
                    a.str(wzr, ptr(x0, OFF_EXIT));

                    // Branch to endOfSend (caller's continuation past
                    // the send).  Skips the warm-J2J gate entirely.
                    a.b(tosSendRes ? tosLrearm : endOfSend);

                    a.bind(normalJ2J);
                    // Fall through to the warm-J2J gate + save-push path.
                }

                // PURE-J2J GATE (default-OFF as of 2026-05-21 jit-may20 Step 2):
                // Originally shipped default-ON in A6 N+30k as a safety net
                // for a materialize-bail wrong-result bug — every benchFib(N)
                // returned benchFib(N-2)'s value for N>=17 (see deferred A6
                // N+30i).  The gate iterates ALL of caller's IC sites and
                // bails inline-J2J if any site lacks J2J_ENTRY_BIT (bit 60).
                //
                // Empirically (jit-may20.md Step 2): the gate bails ~100% of
                // fib's IC hits (catch-rate 0% across 2.88M attempts on a
                // fib-loop bench) while gate-OFF runs the same fib(20..32)
                // 5×–15× faster with CORRECT results.  Default flipped to
                // OFF.  PHARO_T1_PURE_J2J_GATE=1 re-enables the gate for
                // safety / bisection if a wrong-result regression appears.
                //
                // Self-recursive only (the bit 56 check at line ~3341 has
                // already ensured caller == callee for this push), so the
                // "callee has cold IC" risk is bounded by what the caller
                // has actually observed.
                if (g_debug.t1PureJ2JGate || g_debug.t1WarmJ2JGate) {
                    using namespace asmjit::a64;
                    // Gate variant: PureJ2J checks entry0.extras bit 60
                    // (= "site has J2J target"); WarmJ2J checks entry0.key
                    // != 0 (= "site has any filled entry").  Warmth is the
                    // more permissive check — passes for warm prim-only
                    // sites that the pure gate would bail on for lacking
                    // bit 60, while still catching cold ICs that would
                    // trigger the materialize-bail wrong-result bug.
                    // PureJ2J takes precedence when both flags are on
                    // (deliberate: bisection knob preserves the older
                    // stricter behavior).
                    const bool usePureGate = g_debug.t1PureJ2JGate;
                    a.ldr(x12, ptr(callerJMReg2,
                        (int)offsetof(JITMethod, icBuffer)));
                    a.ldrh(w14, ptr(callerJMReg2,
                        (int)offsetof(JITMethod, numICEntries)));
                    asmjit::Label gateLoop = a.new_label();
                    asmjit::Label gateDone = a.new_label();
                    asmjit::Label gateBail = a.new_label();
                    a.cbz(w14, gateDone);
                    a.bind(gateLoop);
                    if (usePureGate) {
                        a.ldr(x4, ptr(x12, 16));   // ic[site].extras (slot 2)
                        a.tbz(x4, asmjit::Imm(60), gateBail);
                    } else {
                        a.ldr(x4, ptr(x12, 0));    // ic[site].key (slot 0)
                        a.cbz(x4, gateBail);
                    }
                    a.add(x12, x12, asmjit::Imm((int)IC_BYTES_PER_SITE));
                    a.subs(w14, w14, asmjit::Imm(1));
                    a.b_ne(gateLoop);
                    a.b(gateDone);
                    a.bind(gateBail);
                    emitIncCounter((uint64_t)&g_inlineJ2J_bail_gate);
                    // jit-may20b Step 6.1: per-caller histogram.  When the
                    // env var is on, call jit_rt_bail_gate_log(callerJM, kind)
                    // around the gate exits.  callerJMReg2 == x19 in
                    // xmethod-off (default), so it's callee-saved across the
                    // bl per AAPCS.  Saving x0..x15 + x30 covers everything
                    // the bail target (`j2jBailSelf2` → `j2jBail` →
                    // inline-prim / dispatchCached) and the pass path use.
                    auto emitBailGateLog = [&](uint64_t kind) {
                        if (!g_debug.t1BailGateHisto) return;
                        a.sub(sp, sp, asmjit::Imm(144));
                        a.stp(x0, x1,   ptr(sp, 0));
                        a.stp(x2, x3,   ptr(sp, 16));
                        a.stp(x4, x5,   ptr(sp, 32));
                        a.stp(x6, x7,   ptr(sp, 48));
                        a.stp(x8, x9,   ptr(sp, 64));
                        a.stp(x10, x11, ptr(sp, 80));
                        a.stp(x12, x13, ptr(sp, 96));
                        a.stp(x14, x15, ptr(sp, 112));
                        a.str(x30,      ptr(sp, 128));
                        a.mov(x0, callerJMReg2);
                        a.mov(x1, asmjit::Imm(kind));
                        a.mov(x16,
                            asmjit::Imm((uint64_t)&jit_rt_bail_gate_log));
                        a.blr(x16);
                        a.ldp(x0, x1,   ptr(sp, 0));
                        a.ldp(x2, x3,   ptr(sp, 16));
                        a.ldp(x4, x5,   ptr(sp, 32));
                        a.ldp(x6, x7,   ptr(sp, 48));
                        a.ldp(x8, x9,   ptr(sp, 64));
                        a.ldp(x10, x11, ptr(sp, 80));
                        a.ldp(x12, x13, ptr(sp, 96));
                        a.ldp(x14, x15, ptr(sp, 112));
                        a.ldr(x30,      ptr(sp, 128));
                        a.add(sp, sp, asmjit::Imm(144));
                    };
                    emitBailGateLog(0);
                    a.b(j2jBailSelf2);
                    a.bind(gateDone);
                    emitBailGateLog(1);
                }

                // PHARO_T1_LOG_SELFREC_PUSH: record caller(=callee) CM oop into
                // the ring so the #extent DNU dump can NAME the self-recursive
                // method whose save/return desyncs.  x0=state already.  Save
                // x0-x15 + x30 (the bail-gate-log save set) around the blr;
                // x19/x20 are callee-saved per AAPCS.  DET_SCHED makes the
                // extra instructions harmless to scheduling.
                if (GET_DEBUG_BOOL(PHARO_T1_LOG_SELFREC_PUSH)) {
                    using namespace asmjit::a64;
                    a.sub(sp, sp, asmjit::Imm(144));
                    a.stp(x0, x1,   ptr(sp, 0));
                    a.stp(x2, x3,   ptr(sp, 16));
                    a.stp(x4, x5,   ptr(sp, 32));
                    a.stp(x6, x7,   ptr(sp, 48));
                    a.stp(x8, x9,   ptr(sp, 64));
                    a.stp(x10, x11, ptr(sp, 80));
                    a.stp(x12, x13, ptr(sp, 96));
                    a.stp(x14, x15, ptr(sp, 112));
                    a.str(x30,      ptr(sp, 128));
                    // x0 = state (unchanged from entry); call logger.
                    a.mov(x16, asmjit::Imm((uint64_t)&jit_rt_log_selfrec_push));
                    a.blr(x16);
                    a.ldp(x0, x1,   ptr(sp, 0));
                    a.ldp(x2, x3,   ptr(sp, 16));
                    a.ldp(x4, x5,   ptr(sp, 32));
                    a.ldp(x6, x7,   ptr(sp, 48));
                    a.ldp(x8, x9,   ptr(sp, 64));
                    a.ldp(x10, x11, ptr(sp, 80));
                    a.ldp(x12, x13, ptr(sp, 96));
                    a.ldp(x14, x15, ptr(sp, 112));
                    a.ldr(x30,      ptr(sp, 128));
                    a.add(sp, sp, asmjit::Imm(144));
                }

                // Check save stack space.  cursor (offset 144) and limit
                // (offset 152) are adjacent — load both with one ldp.
                static_assert(OFF_J2J_SAVE_LIMIT == OFF_J2J_SAVE_CURSOR + 8,
                              "cursor/limit adjacency required for ldp fold");
                if (g_fsrCursor) {
                    a.mov(x6, x23);                       // FSR M2 v1
                    a.ldr(x14, ptr(x0, OFF_J2J_SAVE_LIMIT));
                } else {
                a.ldp(x6, x14, ptr(x0, OFF_J2J_SAVE_CURSOR));  // x6=cursor, x14=limit
                }
                a.cmp(x6, x14);
                a.b_hs(j2jBailFull);

                // Load resumeAddr (label adr after the send completes)
                // resumeAddr points DIRECTLY at endOfSend (skipping the
                // prior afterSend bind which just did `b endOfSend`).
                // Saves 1 branch per J2J return — 7.4M returns on fib(28).
#if PHARO_J2J_SAVE_V2
                a.adr(x14, resumeAfterCall);
#else
                a.adr(x14, endOfSend);
#endif

                // Push J2J save (56 bytes).  Uses ldp/stp for adjacent
                // state fields: sp+receiver (offsets 0/8) loaded with
                // one ldp.
                //   [0]=sp, [8]=receiver, [16]=tempBase, [24]=ip,
                //   [32]=jitMethod, [40]=resumeAddr, [48]=sendArgCount
                //
                // First stp uses POST-INDEX (ptr_post) to advance x6 by
                // 56 (the save size) atomically with the store.  Saves
                // the explicit `add x6, x6, 56` after the save —
                // 1 instr per push.  Subsequent stps use negative
                // offsets relative to the post-advance x6:
                //   original [x6, 0]   -> post-index advance to x6+56
                //   original [x6, 16]  -> ptr(x6, -40)  (x6+56-40 = x6+16)
                //   original [x6, 40]  -> ptr(x6, -16)  (x6+56-16 = x6+40)
#if PHARO_J2J_SAVE_V2
                // V2 packed push: 2 stps + 1 movk.  x14 = adr(endOfSend)
                // from above; movk packs bcOff|nArgs<<12 into bits 48-63
                // in ONE instruction (bcOff is the POST-send offset
                // relative to bcStart = globalIdx + 1 for 1-byte sends).
                // Sites that don't fit the 12/4-bit packing must not
                // emit the inline-J2J fast path at all (checked by the
                // gate that guards this whole block at flip time).
                {
                    uint32_t resumeBcOff = (uint32_t)globalIdx + 1;
                    uint16_t packed16 =
                        (uint16_t)(((nArgs & 0xF) << 12) | (resumeBcOff & 0xFFF));
                    a.movk(x14, packed16, 48);
                }
                emitLoadSp(a, x15);
                a.ldr(x4, ptr(x0, OFF_RECEIVER));
                a.stp(x15, x4, ptr_post(x6, JSV_SIZE)); // sp+recv; cursor += 32
                emitLoadTempBase(a, x15);
                // size-40: tempBase@-24, packedResume@-16, closure@-8.
                a.stp(x15, x14, ptr(x6, -24));          // tempBase + packedResume
                a.str(xzr, ptr(x6, -8));                // JSV_CLOSURE (Phase 1: 0)
#else
                emitLoadSp(a, x15);                // sp (sweep: was ldp sp+recv)
                a.ldr(x4, ptr(x0, OFF_RECEIVER)); // receiver
                a.stp(x15, x4, ptr_post(x6, JSV_SIZE));  // [old x6, 0]; x6 += save size
                emitLoadTempBase(a, x15);
                if (g_debug.t1J2JPostSendIp) {
                    // Compute past-send IP = callerCM + bcOffsetFromMethObj
                    // + 1 (1-byte SEND opcode — true for SpecialSendBase
                    // 0x70-0x7F and Send0/1/2Base 0x80-0xAF, which covers
                    // the common case incl. benchFib's recursive send).
                    //
                    // Register source for callerJM: x11 is only loaded
                    // when xmethod is on (see IC-probe at line ~2950); in
                    // xmethod-off (default) callerJM is hoisted to x19.
                    // The old code unconditionally used x11 — which is
                    // uninitialized in xmethod-off, → SEGV inside the
                    // interpreter when the save was materialized.
                    asmjit::a64::Gp callerJMReg = g_debug.t1InlineJ2JXmethod
                        ? asmjit::a64::x11
                        : asmjit::a64::x19;
                    // x12 may already hold callerCM if the inline-J2J
                    // counters branch loaded it (see line ~3247).  When
                    // counters off, load it now from callerJM[0].
                    if (!inlineJ2JCounters) {
                        a.ldr(x12, ptr(callerJMReg, 0));   // callerCM
                    }
                    a.add(x4, x12, asmjit::Imm(bcOffsetFromMethObj + 1));
                } else {
                    a.ldr(x4, ptr(x0, OFF_IP));
                }
                a.stp(x15, x4, ptr(x6, -40));      // tempBase + ip
                if (xmethod) {
                    // xmethod-on path: writes save.jitMethod = callerJM.
                    a.stp(x11, x14, ptr(x6, -24)); // callerJM + resumeAddr
                    if (nArgs == 0) {
                        a.str(wzr, ptr(x6, -8));   // argCount
                    } else {
                        a.mov(w15, asmjit::Imm(nArgs));
                        a.str(w15, ptr(x6, -8));
                    }
                } else {
                    // xmethod-off (default): skip save.jitMethod write —
                    // it's redundant in self-recursive-only chains
                    // (state.jitMethod is the correct fallback).  Combine
                    // resumeAddr + argCount into one stp.
                    //
                    // The j2jPool_ slot's jitMethod field stays 0 because:
                    //   - std::array zero-inits at process start
                    //   - asmjit-T1 (this code) never writes jitMethod in
                    //     this branch
                    //   - chain loop uses a separate slice (split-pool)
                    //
                    // Materialize sites (Interpreter.cpp site4/5/6/7 and
                    // j2jBase) all fall back to state.jitMethod when
                    // save.jitMethod is null.
                    if (nArgs == 0) {
                        // resumeAddr + xzr at [x6-16, x6-8] (orig [40, 48]).
                        a.stp(x14, xzr, ptr(x6, -16));
                    } else {
                        a.mov(w15, asmjit::Imm(nArgs));
                        a.stp(x14, x15, ptr(x6, -16));
                    }
                }
#endif  // PHARO_J2J_SAVE_V2 (push variant)

                // E2 2026-05-24: cross-method state.{jitMethod,method,
                // literals,argCount} update.  Relocated from BEFORE the
                // save-full check (originally at line ~3707) to AFTER the
                // save push.  Original location was reached BEFORE the
                // save-full bail at line ~3843, so if the bail fired with
                // a full pool (common when X+BV are both on and the pool
                // is being shared), state.method was left set to calleeCM.
                // The bail then fell through to dispatchCached / miss /
                // non-probe-fallback emits, which compute state.ip =
                // state.method + bcOffsetFromMethObj — landing state.ip
                // at calleeCM + caller_bcOff = an unrelated heap address.
                // Symptom: interp resumed, dispatched bytes from a
                // neighbouring CompiledMethod's data area as bytecodes;
                // pushReceiverVariable on a SmI receiver → SIGSEGV.
                // Placing the update *after* the save push means
                // save-full / pure-J2J-gate / etc. bails leave
                // state.method unchanged (= callerCM), so the bail's
                // ip-write lands in the caller's bytecodes as intended.
                // For self-recursive sends this is a redundant write
                // of the same value (callee == caller); cost is 4
                // extra stores per push.
                if (xmethod && g_fsrX19) a.mov(x19, x10);  // FSR M1
                if (xmethod && (!g_fsrLazy || g_fsrLazyVerify)) {
                    a.ldr(x13, ptr(x10, (int)offsetof(JITMethod, compiledMethodOop)));      // x13 = calleeCM
                    a.str(x10, ptr(x0, OFF_JITMETHOD));
                    a.str(x13, ptr(x0, OFF_METHOD));
                    a.add(x13, x13, asmjit::Imm(16));
                    a.str(x13, ptr(x0, OFF_LITERALS));
                    a.mov(w13, asmjit::Imm(nArgs));
                    a.str(w13, ptr(x0, OFF_ARGCOUNT));
                }

                // Bump cursor + depth + totalCalls.  depth and totalCalls
                // are adjacent int32 fields (offsets 160/164); fold the
                // two ldr-add-str pairs (6 instrs) into one 64-bit
                // ldr-add-str + movz/movk materialization (5 instrs).
                // depth caps at recursion depth (way below 2^31) so
                // adding 1 to the low 32 bits never carries into the
                // high 32 bits where totalCalls lives.
                //
                // Cursor was advanced by 56 via the first stp's post-
                // index — just write x6 to state.j2jSaveCursor.
                a.str(x6, ptr(x0, OFF_J2J_SAVE_CURSOR));
                if (g_fsrCursor) a.mov(x23, x6);  // FSR M2 v1
                static_assert(OFF_J2J_TOTAL_CALLS == OFF_J2J_DEPTH + 4,
                              "depth/totalCalls adjacency required for 64-bit batched increment");
                // x20 = j2jDepthInc (0x100000001), pre-loaded at JIT
                // entry by the trampoline AND by JIT_CALL macro.
                // x20 is callee-saved per AAPCS — preserved across all
                // br/blr/ret in the J2J chain.  Saves 1 ldr per push
                // vs loading from OFF_J2J_DEPTH_INC each time.
                a.ldr(x13, ptr(x0, OFF_J2J_DEPTH));
                a.add(x13, x13, asmjit::a64::x20);
                a.str(x13, ptr(x0, OFF_J2J_DEPTH));

                // Set up callee state for self-recursion:
                //   receiver = sp[-(nArgs+1)*8]
                //   tempBase = sp - nArgs*8
                //   ip       = method bytecode start
                //   sp       = tempBase + tempCount*8
                //
                // x1 already holds the new receiver (loaded by the IC HIT
                // setup at `ldur x1, [x2, rcvrOffsetBytes]` and preserved
                // through the inline-J2J emit) — skip the redundant
                // sub + ldr that re-reads it from the stack.
                //
                // x2 = sp from the IC HIT (`ldr x2, [OFF_SP]` ~10 instr
                // back) is also still live in default config (block-value
                // and xmethod-log are opt-in; neither touches x2).
                // Reuse x2 instead of reloading sp.
                a.str(x1, ptr(x0, OFF_RECEIVER));        // recv from x1
                const bool spLiveInX2 = !g_debug.t1InlineBlockValue
                    && !pharo::g_debug.t1InlineJ2JXmethodLog;
                asmjit::a64::Gp spReg = x12;
                if (spLiveInX2) {
                    spReg = x2;   // skip the ldr; x2 still has caller sp
                } else {
                    emitLoadSp(a, x12);
                }
                // jit-may20b Step 8.1 ROLLBACK 2026-05-21: keep the sub.
                // Skipping for nArgs==0 caused gate-OFF SEGV in inline-J2J
                // hot path — x13 wasn't initialized to a usable value, so a
                // downstream code path that depended on x13 holding the new
                // tempBase wrote to a bogus address.  The sub for nArgs==0
                // is `sub x13, spReg, 0` = `mov x13, spReg` (one cycle),
                // negligible perf cost vs the correctness need.
                a.sub(x13, spReg, asmjit::Imm(nArgs * 8)); // new tempBase
                emitStoreTempBase(a, x13);

                // Load cached bcStart from JITMethod (offset 96).
                // For self-rec, callerJM == calleeJM.  In xmethod-off
                // (default) x19 holds state.jitMethod; in xmethod-on
                // x10 is calleeJM (set above) and x11 is callerJM.
                if (!g_fsrLazy || g_fsrLazyVerify) {
                a.ldr(x14, ptr(needCalleeJM
                                  ? x10
                                  : (g_debug.t1InlineJ2JXmethod
                                      ? x11 : asmjit::a64::x19),
                              (int)offsetof(JITMethod, bcStartCache)));
                a.str(x14, ptr(x0, OFF_IP));
                }

                // sp = tempBase + tempCount*8.  For the SELF-RECURSIVE
                // path (the only path when xmethod is off), callee ==
                // caller so callee.tempCount == compile-time-known
                // callerTempCount.  Skip the dynamic tempCount load +
                // init loop entirely when we know the layout statically.
                //
                // Three static cases (callerTempCount known here):
                //   1. nArgs == callerTempCount: no extra temps.  New sp
                //      equals caller's pre-send sp (still in x12).
                //   2. nArgs < callerTempCount: unroll N nil-stores +
                //      compute static sp offset.
                //   3. xmethod ON: callee.tempCount can differ from
                //      caller's; fall back to the dynamic loop.
                const bool xmethodMayDiffer = xmethod;
                if (!xmethodMayDiffer && callerTempCount >= nArgs) {
                    int extras = callerTempCount - nArgs;
                    if (extras == 0) {
                        // New sp = caller's sp.  state.sp was already
                        // caller's sp before this push (we never wrote
                        // it), so the str is a no-op semantically.
                        // Skip it — saves 1 instr per push for methods
                        // where nArgs == tempCount (e.g., benchFib).
                    } else {
                        // Unroll nil-stores for the (small) extra temp
                        // count and compute new sp statically.  Methods
                        // with very large extras-counts are rare; cap
                        // unroll at 8 and fall back to dynamic loop
                        // beyond that to avoid blowing emit size.
                        if (extras <= 8) {
                            a.mov(x4, asmjit::Imm(nilBits));
                            for (int k = 0; k < extras; k++) {
                                a.str(x4, ptr(x13, (nArgs + k) * 8));
                            }
                            // New sp = tempBase + tempCount*8
                            //        = caller_sp + (tempCount-nArgs)*8
                            a.add(x15, spReg, asmjit::Imm(extras * 8));
                            emitStoreSp(a, x15);
                        } else {
                            // Large extras: fall back to dynamic loop
                            // (same as xmethod path).
                            asmjit::Label initLoop = a.new_label();
                            asmjit::Label initDone = a.new_label();
                            a.add(x14, x13, asmjit::Imm(nArgs * 8));
                            a.add(x15, x13, asmjit::Imm(callerTempCount * 8));
                            a.mov(x4, asmjit::Imm(nilBits));
                            a.bind(initLoop);
                            a.cmp(x14, x15);
                            a.b_hs(initDone);
                            a.str(x4, ptr(x14));
                            a.add(x14, x14, asmjit::Imm(8));
                            a.b(initLoop);
                            a.bind(initDone);
                            emitStoreSp(a, x15);
                        }
                    }
                } else {
                    // xmethod path: callee tempCount is dynamic (read
                    // from callee JM[35]).  Initialize slots
                    // [nArgs..tempCount) to nil and compute new sp.
                    a.ldrb(w15, ptr(x10, 35));       // tempCount
                    asmjit::Label initLoop = a.new_label();
                    asmjit::Label initDone = a.new_label();
                    a.add(x14, x13, asmjit::Imm(nArgs * 8));
                    a.lsl(w15, w15, asmjit::Imm(3));
                    a.add(x15, x13, x15);
                    a.mov(x4, asmjit::Imm(nilBits));
                    a.bind(initLoop);
                    a.cmp(x14, x15);
                    a.b_hs(initDone);
                    a.str(x4, ptr(x14));
                    a.add(x14, x14, asmjit::Imm(8));
                    a.b(initLoop);
                    a.bind(initDone);
                    emitStoreSp(a, x15);
                }

                // Bump hits counter
                emitIncCounter((uint64_t)&g_inlineJ2J_hits);

                // Tail-call (br) to entry. x9 = entryAddr.
                a.br(x9);

                // afterSend was previously bound here with `b endOfSend`,
                // costing 1 branch per return.  Instead, the J2J save's
                // resumeAddr now points directly at endOfSend (see the
                // adr above), eliminating the indirection.  afterSend
                // label is unbound — kept for code-clarity comments only.

                a.bind(j2jBailSelf2);
                emitIncCounter((uint64_t)&g_inlineJ2J_bail_self);
                a.b(j2jBail);
                a.bind(j2jBailZero);
                a.bind(j2jBailFull);
                emitIncCounter((uint64_t)&g_inlineJ2J_bail_full);
                a.b(j2jBail);
                a.bind(j2jBailSelf);

                a.bind(j2jBail);
                // Fall through to inline-spec dispatch (same as non-J2J path).
                // For nArgs == 1, check primKind bits 52:48 for inline SmI
                // bitwise ops (bitAnd/bitOr/bitXor) — these skip the chain-
                // loop round-trip on SmI sends, mirroring stencils.cpp:1609-1614.
                if (nArgs == 1 && g_debug.t1InlinePrimBitOps) {
                    a.tst(x1, asmjit::Imm(0x7));     // SmI? tag != 0
                    a.b_eq(j2jBailHeap);              // tag==0 → heap path
                    // Immediate receiver (SmI or SmallFloat).  Check primKind.
                    a.lsr(x6, x7, asmjit::Imm(48));
                    a.and_(x6, x6, asmjit::Imm(0x1F));
                    a.cmp(x6, asmjit::Imm(11));
                    a.b_eq(tryPrimBitAnd);
                    a.cmp(x6, asmjit::Imm(12));
                    a.b_eq(tryPrimBitOr);
                    a.cmp(x6, asmjit::Imm(19));
                    a.b_eq(tryPrimBitXor);
                    a.cmp(x6, asmjit::Imm(13));
                    a.b_eq(tryPrimBitShift);
                    // SmallFloat ops — primKind 21/22/23.
                    a.sub(x6, x6, asmjit::Imm(21));
                    a.cmp(x6, asmjit::Imm(3));
                    a.b_lo(tryPrimSmallFloatOp);
                    a.b(dispatchCached);
                    a.bind(j2jBailHeap);
                } else {
                    a.tst(x1, asmjit::Imm(0x7));
                    a.b_ne(dispatchCached);
                }
                // Arity gates: a getter is a 0-arg unary accessor, a setter
                // a 1-arg mutator.  Emitting the dispatch only at a matching-
                // arity send-site stops a stale/racy IC extra word from
                // hijacking a send of the wrong arity (e.g. the nArgs==1
                // `at:` inside SequenceableCollection>>first taking the 0-arg
                // getter fast path — AI-Algorithms-Graph `curEdge first`).
                if (g_debug.t1InlineGetter && nArgs == 0) {
                    a.tbnz(x7, asmjit::Imm(63), tryGetter);
                }
                if (g_debug.t1InlineSetter && nArgs == 1) {
                    a.tbnz(x7, asmjit::Imm(62), trySetter);
                }
                if (g_debug.t1InlineReturnsSelf) {
                    a.tbnz(x7, asmjit::Imm(61), tryReturnsSelf);
                }
                // Inline at: / at:put: / size for heap receivers
                // (primKind 14/15/16).  Mirrors stencils.cpp:1538-1554.
                if ((nArgs == 0 || nArgs == 1 || nArgs == 2)
                        && g_debug.t1InlinePrimAt) {
                    a.lsr(x6, x7, asmjit::Imm(48));
                    a.and_(x6, x6, asmjit::Imm(0x1F));
                    if (nArgs == 1) {
                        if (!GET_DEBUG_BOOL(PHARO_T1_NO_INLINE_AT_READ)) {
                            a.cmp(x6, asmjit::Imm(14));
                            a.b_eq(tryPrimAt);
                        }
                        // jit-may20b Step 10: primKind 18 = basicNew:.
                        // Routes to a runtime helper that calls
                        // primitiveNewWithArg directly, bypassing the
                        // chain-loop's IC-MISS-style dispatch.
                        if (g_debug.t1InlinePrimBasicNew) {
                            a.cmp(x6, asmjit::Imm(18));
                            a.b_eq(tryPrimBasicNew);
                        }
                    } else if (nArgs == 2) {
                        a.cmp(x6, asmjit::Imm(15));
                        a.b_eq(tryPrimAtPut);
                    } else {  // nArgs == 0
                        if (!GET_DEBUG_BOOL(PHARO_T1_NO_INLINE_SIZE)) {
                            a.cmp(x6, asmjit::Imm(16));
                            a.b_eq(tryPrimSize);
                        }
                        if (!GET_DEBUG_BOOL(PHARO_T1_NO_INLINE_IDH)) {
                            a.cmp(x6, asmjit::Imm(20));
                            a.b_eq(tryPrimIdentityHash);
                        }
                        if (!GET_DEBUG_BOOL(PHARO_T1_NO_INLINE_CLASS)) {
                            a.cmp(x6, asmjit::Imm(24));
                            a.b_eq(tryPrimClass);
                        }
                    }
                }
                a.b(dispatchCached);
            } else {
                // Inline specializations need heap receiver (tag==0).
                // Also check primKind for SmI receivers with bitwise prim
                // dispatch (skips chain-loop for nArgs==1 bitAnd/bitOr/bitXor).
                if (nArgs == 1 && g_debug.t1InlinePrimBitOps) {
                    a.tst(x1, asmjit::Imm(0x7));     // SmI? tag != 0
                    a.b_eq(j2jBailHeap2);             // tag==0 → heap path
                    a.lsr(x6, x7, asmjit::Imm(48));
                    a.and_(x6, x6, asmjit::Imm(0x1F));
                    a.cmp(x6, asmjit::Imm(11));
                    a.b_eq(tryPrimBitAnd);
                    a.cmp(x6, asmjit::Imm(12));
                    a.b_eq(tryPrimBitOr);
                    a.cmp(x6, asmjit::Imm(19));
                    a.b_eq(tryPrimBitXor);
                    a.cmp(x6, asmjit::Imm(13));
                    a.b_eq(tryPrimBitShift);
                    a.b(dispatchCached);
                    a.bind(j2jBailHeap2);
                } else {
                    a.tst(x1, asmjit::Imm(0x7));
                    a.b_ne(dispatchCached);
                }

                // Arity gates: a getter is a 0-arg unary accessor, a setter
                // a 1-arg mutator.  Emitting the dispatch only at a matching-
                // arity send-site stops a stale/racy IC extra word from
                // hijacking a send of the wrong arity (e.g. the nArgs==1
                // `at:` inside SequenceableCollection>>first taking the 0-arg
                // getter fast path — AI-Algorithms-Graph `curEdge first`).
                if (g_debug.t1InlineGetter && nArgs == 0) {
                    a.tbnz(x7, asmjit::Imm(63), tryGetter);
                }
                if (g_debug.t1InlineSetter && nArgs == 1) {
                    a.tbnz(x7, asmjit::Imm(62), trySetter);
                }
                if (g_debug.t1InlineReturnsSelf) {
                    a.tbnz(x7, asmjit::Imm(61), tryReturnsSelf);
                }
                // Inline at: / at:put: / size for heap receivers
                // (primKind 14/15/16).  Mirrors stencils.cpp:1538-1554.
                if ((nArgs == 0 || nArgs == 1 || nArgs == 2)
                        && g_debug.t1InlinePrimAt) {
                    a.lsr(x6, x7, asmjit::Imm(48));
                    a.and_(x6, x6, asmjit::Imm(0x1F));
                    if (nArgs == 1) {
                        if (!GET_DEBUG_BOOL(PHARO_T1_NO_INLINE_AT_READ)) {
                            a.cmp(x6, asmjit::Imm(14));
                            a.b_eq(tryPrimAt);
                        }
                        // jit-may20b Step 10: primKind 18 = basicNew:.
                        if (g_debug.t1InlinePrimBasicNew) {
                            a.cmp(x6, asmjit::Imm(18));
                            a.b_eq(tryPrimBasicNew);
                        }
                    } else if (nArgs == 2) {
                        a.cmp(x6, asmjit::Imm(15));
                        a.b_eq(tryPrimAtPut);
                    } else {  // nArgs == 0
                        if (!GET_DEBUG_BOOL(PHARO_T1_NO_INLINE_SIZE)) {
                            a.cmp(x6, asmjit::Imm(16));
                            a.b_eq(tryPrimSize);
                        }
                        if (!GET_DEBUG_BOOL(PHARO_T1_NO_INLINE_IDH)) {
                            a.cmp(x6, asmjit::Imm(20));
                            a.b_eq(tryPrimIdentityHash);
                        }
                        if (!GET_DEBUG_BOOL(PHARO_T1_NO_INLINE_CLASS)) {
                            a.cmp(x6, asmjit::Imm(24));
                            a.b_eq(tryPrimClass);
                        }
                    }
                }
                a.b(dispatchCached);
            }

            // === Inline getter: val = recv->slots[slotIdx] ===
            // x2 still holds SP from probe entry (mirrors x86 rcx-keep).
            a.bind(tryGetter);
            if (pharo::g_debug.t1InlineJ2J_Env) {
                a.mov(x6, asmjit::Imm((uint64_t)&g_t1InlineGetter_hits));
                a.ldr(x3, ptr(x6));
                a.add(x3, x3, asmjit::Imm(1));
                a.str(x3, ptr(x6));
            }
            a.and_(x6, x7, asmjit::Imm(0xFFFF));   // slotIdx
            a.add(x3, x1, x6, asmjit::a64::lsl(3));
            a.ldr(x6, ptr(x3, 8));                // val = *(recv+slot*8+8)
            a.stur(x6, ptr(x2, rcvrOffsetBytes));  // replace receiver
            // FINDNODE_WATCH (asTuple only): record (state=x0, recv=x1, val=x6)
            // so the helper can compute which operand slot we wrote vs tempBase.
            if (g_emitGetterTrace) {
                a.sub(asmjit::a64::sp, asmjit::a64::sp, asmjit::Imm(48));
                a.str(x0,  ptr(asmjit::a64::sp, 0));
                a.str(x1,  ptr(asmjit::a64::sp, 8));
                a.str(x2,  ptr(asmjit::a64::sp, 16));
                a.str(x6,  ptr(asmjit::a64::sp, 24));
                a.str(x7,  ptr(asmjit::a64::sp, 32));
                a.str(x30, ptr(asmjit::a64::sp, 40));
                a.mov(x2, x6);    // arg2 = val (x0=state, x1=recv already in place)
                a.mov(x3, asmjit::Imm((uint64_t)bcOffsetFromMethObj));  // arg3 = bcOff
                a.mov(x9, asmjit::Imm((uint64_t)&jit_rt_atrec_getter));
                a.blr(x9);
                a.ldr(x0,  ptr(asmjit::a64::sp, 0));
                a.ldr(x1,  ptr(asmjit::a64::sp, 8));
                a.ldr(x2,  ptr(asmjit::a64::sp, 16));
                a.ldr(x6,  ptr(asmjit::a64::sp, 24));
                a.ldr(x7,  ptr(asmjit::a64::sp, 32));
                a.ldr(x30, ptr(asmjit::a64::sp, 40));
                a.add(asmjit::a64::sp, asmjit::a64::sp, asmjit::Imm(48));
            }
            // PHARO_VERIFY_GETTER (emit-time gate): BLR a helper that
            // flags slotIdx >= receiver slotCount — a poisoned extra
            // word (J2J address bits / foreign-site classification).
            // Same save/restore shape as the trace block above.
            if (GET_DEBUG_BOOL(PHARO_VERIFY_GETTER)) {
                a.sub(asmjit::a64::sp, asmjit::a64::sp, asmjit::Imm(48));
                a.str(x0,  ptr(asmjit::a64::sp, 0));
                a.str(x1,  ptr(asmjit::a64::sp, 8));
                a.str(x2,  ptr(asmjit::a64::sp, 16));
                a.str(x6,  ptr(asmjit::a64::sp, 24));
                a.str(x7,  ptr(asmjit::a64::sp, 32));
                a.str(x30, ptr(asmjit::a64::sp, 40));
                a.mov(x2, x6);    // arg2 = val (x0=state, x1=recv in place)
                a.mov(x3, x7);    // arg3 = the IC extra word
                a.mov(x4, x5);    // arg4 = matched IC entry base
                                  // (x5 = icDataPtr from the probe; live
                                  // along the dispatch chain — see the
                                  // HIT_FORCE_DISPATCH comment ~3728)
                a.mov(x9, asmjit::Imm((uint64_t)&jit_rt_verify_getter));
                a.blr(x9);
                a.ldr(x0,  ptr(asmjit::a64::sp, 0));
                a.ldr(x1,  ptr(asmjit::a64::sp, 8));
                a.ldr(x2,  ptr(asmjit::a64::sp, 16));
                a.ldr(x6,  ptr(asmjit::a64::sp, 24));
                a.ldr(x7,  ptr(asmjit::a64::sp, 32));
                a.ldr(x30, ptr(asmjit::a64::sp, 40));
                a.add(asmjit::a64::sp, asmjit::a64::sp, asmjit::Imm(48));
            }
            if (nArgs > 0) {
                a.sub(x2, x2, asmjit::Imm(8 * nArgs));
                emitStoreSp(a, x2);
            }
            a.b(tosSendRes ? tosLrearm : endOfSend);

            // === Inline setter: recv->slots[slotIdx] = arg ===
            // x2 still holds SP from probe entry.
            a.bind(trySetter);
            if (pharo::g_debug.t1InlineJ2J_Env) {
                a.mov(x6, asmjit::Imm((uint64_t)&g_t1InlineSetter_hits));
                a.ldr(x3, ptr(x6));
                a.add(x3, x3, asmjit::Imm(1));
                a.str(x3, ptr(x6));
            }
            a.and_(x6, x7, asmjit::Imm(0xFFFF));
            a.ldur(x3, ptr(x2, -8));               // arg = sp[-1]
            // PHARO_T1_SETTER_BOUNDS=1: log inline-setter stores whose slot
            // index is OOB for the receiver (wild heap write).  x1=recv,
            // x6=slotIdx, x3=arg.  Save the regs we still need after the call.
            if (GET_DEBUG_BOOL(PHARO_T1_SETTER_BOUNDS)) {
                a.sub(asmjit::a64::sp, asmjit::a64::sp, asmjit::Imm(64));
                a.str(x0,  ptr(asmjit::a64::sp, 0));
                a.str(x30, ptr(asmjit::a64::sp, 8));
                a.str(x1,  ptr(asmjit::a64::sp, 16));
                a.str(x2,  ptr(asmjit::a64::sp, 24));
                a.str(x3,  ptr(asmjit::a64::sp, 32));
                a.str(x7,  ptr(asmjit::a64::sp, 40));
                // args: x0=state(already), x1=recv(already), x2=slotIdx, x3=val(already)
                a.mov(x2, x6);
                emitSyncSpToState(a);  // sp-residency: helper reads state.sp
                a.mov(x9, asmjit::Imm((uint64_t)&jit_rt_check_setter_bounds));
                a.blr(x9);
                a.ldr(x0,  ptr(asmjit::a64::sp, 0));
                a.ldr(x30, ptr(asmjit::a64::sp, 8));
                a.ldr(x1,  ptr(asmjit::a64::sp, 16));
                a.ldr(x2,  ptr(asmjit::a64::sp, 24));
                a.ldr(x3,  ptr(asmjit::a64::sp, 32));
                a.ldr(x7,  ptr(asmjit::a64::sp, 40));
                a.add(asmjit::a64::sp, asmjit::a64::sp, asmjit::Imm(64));
                a.and_(x6, x7, asmjit::Imm(0xFFFF)); // recompute slotIdx
            }
            a.add(x4, x1, x6, asmjit::a64::lsl(3));
            a.str(x3, ptr(x4, 8));
            // Store-provenance ring hook (PHARO_T1_STORE_RING) — this
            // site's convention differs from the bytecode store emits
            // (x1=recv, x3=value, x6=dynamic slotIdx), so inline the
            // call instead of using emitStoreRingLog.
            if (GET_DEBUG_BOOL(PHARO_T1_STORE_RING)
                    || GET_DEBUG_BOOL(PHARO_SHADOW_SLOTS)) {
                a.sub(asmjit::a64::sp, asmjit::a64::sp, asmjit::Imm(144));
                a.stp(x0, x1,   ptr(asmjit::a64::sp, 0));
                a.stp(x2, x3,   ptr(asmjit::a64::sp, 16));
                a.stp(x4, x5,   ptr(asmjit::a64::sp, 32));
                a.stp(x6, x7,   ptr(asmjit::a64::sp, 48));
                a.stp(x8, x9,   ptr(asmjit::a64::sp, 64));
                a.stp(x10, x11, ptr(asmjit::a64::sp, 80));
                a.stp(x12, x13, ptr(asmjit::a64::sp, 96));
                a.stp(x14, x15, ptr(asmjit::a64::sp, 112));
                a.str(x30,      ptr(asmjit::a64::sp, 128));
                a.mov(x2, x3);                      // value
                a.mov(x3, x6);                      // slotIdx
                a.ldr(x4, ptr(x0, 56));             // state.jitMethod
                a.mov(x16, asmjit::Imm((uint64_t)&jit_rt_store_ring));
                a.blr(x16);
                a.ldp(x0, x1,   ptr(asmjit::a64::sp, 0));
                a.ldp(x2, x3,   ptr(asmjit::a64::sp, 16));
                a.ldp(x4, x5,   ptr(asmjit::a64::sp, 32));
                a.ldp(x6, x7,   ptr(asmjit::a64::sp, 48));
                a.ldp(x8, x9,   ptr(asmjit::a64::sp, 64));
                a.ldp(x10, x11, ptr(asmjit::a64::sp, 80));
                a.ldp(x12, x13, ptr(asmjit::a64::sp, 96));
                a.ldp(x14, x15, ptr(asmjit::a64::sp, 112));
                a.ldr(x30,      ptr(asmjit::a64::sp, 128));
                a.add(asmjit::a64::sp, asmjit::a64::sp, asmjit::Imm(144));
            }
            if (nArgs > 0) {
                a.sub(x2, x2, asmjit::Imm(8 * nArgs));
                emitStoreSp(a, x2);
            }
            // Audit-gap closer (PHARO_T1_SETTER_BARRIER=1, default off).
            // Emit a BLR to the C helper that records old→young writes
            // in rememberedSet_.  Helper takes (state, rcv, val) in
            // (x0, x1, x2).  See jit_rt_setter_write_barrier in
            // JITRuntime.cpp and memory/jit_remembered_set_dead.md.
            if (pharo::g_debug.t1SetterBarrier) {
                a.sub(asmjit::a64::sp, asmjit::a64::sp, asmjit::Imm(16));
                a.str(x0,  ptr(asmjit::a64::sp, 0));
                a.str(x30, ptr(asmjit::a64::sp, 8));
                a.mov(x2, x3);  // x2 = value (helper arg 2)
                // x0 already = state, x1 already = receiver.
                emitSyncSpToState(a);  // sp-residency: helper reads state.sp
                a.mov(x9, asmjit::Imm(
                    (uint64_t)&jit_rt_setter_write_barrier));
                a.blr(x9);
                a.ldr(x0,  ptr(asmjit::a64::sp, 0));
                a.ldr(x30, ptr(asmjit::a64::sp, 8));
                a.add(asmjit::a64::sp, asmjit::a64::sp, asmjit::Imm(16));
            }
            a.b(tosSendRes ? tosLrearm : endOfSend);

            // === trySistaCall (jit-may22b Step 2 — real BLR via helper) ===
            // Dispatch to a Sista-compiled fn (ptr in x7's bits 47:0).
            //
            // Strategy: a C++ helper (jit_rt_t1_sista_dispatch) sets
            // up a fresh callee JITState, calls Sista's fn, and
            // propagates the return value back to caller's sp.
            // On bail (helper returns 0), fall through to
            // dispatchCached so the chain-loop's full path handles it.
            //
            // x7 = extras (SISTA_BIT set + fn ptr in bits 47:0).
            // x5 = icDataPtr (points to slot 0 of the matching IC entry).
            //      icData[1] = cached method oop bits.
            a.bind(trySistaCall);
            // Save x0 (state) + x30 (LR) across BLR.
            a.sub(asmjit::a64::sp, asmjit::a64::sp, asmjit::Imm(16));
            a.str(x0,  ptr(asmjit::a64::sp, 0));
            a.str(x30, ptr(asmjit::a64::sp, 8));

            // Arg 1 (x1) = fn ptr.  Extract bits 47:0 of x7.
            a.mov(x1, asmjit::Imm(0x0000FFFFFFFFFFFFULL));
            a.and_(x1, x1, x7);
            // Arg 2 (x2) = methodBits.  Read icData[1] at ptr(x5, 8).
            a.ldr(x2, ptr(x5, 8));
            // Arg 3 (x3) = nArgs.
            a.mov(x3, asmjit::Imm((uint64_t)nArgs));

            emitSyncSpToState(a);  // sp-residency: helper reads state.sp
            a.mov(x9, asmjit::Imm((uint64_t)&jit_rt_t1_sista_dispatch));
            a.blr(x9);
            // x0 = 1 on success, 0 on bail.
            a.mov(x9, x0);
            a.ldr(x0,  ptr(asmjit::a64::sp, 0));
            emitReloadSpFromState(a);  // sp-residency: helper may have changed state.sp
            a.ldr(x30, ptr(asmjit::a64::sp, 8));
            a.add(asmjit::a64::sp, asmjit::a64::sp, asmjit::Imm(16));
            {
                asmjit::Label bailSista = a.new_label();
                a.cbz(x9, bailSista);
                // Hit path: bump counter, continue at endOfSend.
                a.mov(x14, asmjit::Imm(
                    (uint64_t)&g_t1SistaDispatch_hits));
                a.ldr(x15, ptr(x14));
                a.add(x15, x15, asmjit::Imm(1));
                a.str(x15, ptr(x14));
                a.b(tosSendRes ? tosLrearm : endOfSend);
                a.bind(bailSista);
                a.b(dispatchCached);
            }

            // === returnsSelf ===
            a.bind(tryReturnsSelf);
            if (pharo::g_debug.t1InlineJ2J_Env) {
                a.mov(x6, asmjit::Imm((uint64_t)&g_t1ReturnsSelf_hits));
                a.ldr(x3, ptr(x6));
                a.add(x3, x3, asmjit::Imm(1));
                a.str(x3, ptr(x6));
            }
            if (nArgs > 0) {
                a.sub(x2, x2, asmjit::Imm(8 * nArgs));
                emitStoreSp(a, x2);
            }
            a.b(tosSendRes ? tosLrearm : endOfSend);

            // === returnsLiteral helper (counter bump) ===
            auto bumpRetLitCounter = [&]() {
                a.mov(x14, asmjit::Imm((uint64_t)&g_t1ReturnsLiteral_hits));
                a.ldr(x15, ptr(x14));
                a.add(x15, x15, asmjit::Imm(1));
                a.str(x15, ptr(x14));
            };
            // === returnsLiteral (bit 58, nArgs==0 only) ===
            // jit-may22b: pattern `^ <literal>` where literal is one
            // of nil/true/false/0/1.  bits 48-50 of extras encode:
            //   1=nil, 2=true, 3=false, 4=SmI 0, 5=SmI 1.
            // F5 R83: handles all nArgs.  Result goes at rcvr slot;
            // for nArgs > 0, SP adjusts down by nArgs*8 to drop args.
            asmjit::Label retLitDone = a.new_label();
            {
                a.bind(tryReturnsLiteral);
                bumpRetLitCounter();
                // Extract kind from bits 48-50 → x6.
                a.lsr(x6, x7, asmjit::Imm(48));
                a.and_(x6, x6, asmjit::Imm(7));
                // kind 1 → nil, 2 → true, 3 → false, 4 → SmI 0, 5 → SmI 1.
                // SmI 0 bits = 1, SmI 1 bits = 9 (= (1<<3)|1).
                // We'll dispatch via cmp+csel chain.
                a.cmp(x6, asmjit::Imm(1));
                {
                    asmjit::Label notNil = a.new_label();
                    a.b_ne(notNil);
                    a.mov(x3, asmjit::Imm(nilBits));
                    a.stur(x3, ptr(x2, rcvrOffsetBytes));
                    a.b(retLitDone);
                    a.bind(notNil);
                }
                a.cmp(x6, asmjit::Imm(2));
                {
                    asmjit::Label notTrue = a.new_label();
                    a.b_ne(notTrue);
                    a.ldr(x3, ptr(x0, OFF_TRUEOOP));
                    a.stur(x3, ptr(x2, rcvrOffsetBytes));
                    a.b(retLitDone);
                    a.bind(notTrue);
                }
                a.cmp(x6, asmjit::Imm(3));
                {
                    asmjit::Label notFalse = a.new_label();
                    a.b_ne(notFalse);
                    a.ldr(x3, ptr(x0, OFF_FALSEOOP));
                    a.stur(x3, ptr(x2, rcvrOffsetBytes));
                    a.b(retLitDone);
                    a.bind(notFalse);
                }
                a.cmp(x6, asmjit::Imm(4));
                {
                    asmjit::Label notZero = a.new_label();
                    a.b_ne(notZero);
                    a.mov(x3, asmjit::Imm(1));  // SmI 0 bits = 1
                    a.stur(x3, ptr(x2, rcvrOffsetBytes));
                    a.b(retLitDone);
                    a.bind(notZero);
                }
                a.cmp(x6, asmjit::Imm(5));
                {
                    asmjit::Label notOne = a.new_label();
                    a.b_ne(notOne);
                    a.mov(x3, asmjit::Imm(9));      // SmI 1 bits = 9
                    a.stur(x3, ptr(x2, rcvrOffsetBytes));
                    a.b(retLitDone);
                    a.bind(notOne);
                }
                a.cmp(x6, asmjit::Imm(6));
                {
                    // F5 R82: kind 6 = SmI -1.  Bits = 0xFFFFFFFFFFFFFFF9.
                    asmjit::Label notMinusOne = a.new_label();
                    a.b_ne(notMinusOne);
                    a.mov(x3, asmjit::Imm(uint64_t(-1) << 3 | 1));
                    a.stur(x3, ptr(x2, rcvrOffsetBytes));
                    a.b(retLitDone);
                    a.bind(notMinusOne);
                }
                // F5 R82: kind 7 = SmI 2.  Bits = 0x11.
                a.mov(x3, asmjit::Imm(0x11));   // SmI 2 bits = 17
                a.stur(x3, ptr(x2, rcvrOffsetBytes));
                a.b(retLitDone);

                // F5 R83: common store-done point.  Drop args if any.
                a.bind(retLitDone);
                if (nArgs > 0) {
                    a.sub(x2, x2, asmjit::Imm(8 * nArgs));
                    emitStoreSp(a, x2);
                }
                a.b(tosSendRes ? tosLrearm : endOfSend);
            }

            // === W1: tryTempReturn (bit 54, nArgs >= 1) ===
            // Pattern: callee = `^ tempN` where N < nArgs (so the value is
            // an arg, not a local).  IC extras bits 48-52 encode the temp
            // index N.  At call site: rcvr = caller's value, args at
            // sp[-nArgs..sp-1].  After inline: result = sp[-(nArgs - N)]
            // (the Nth arg, since temps[N] for N < argCount = args[N]).
            // Then drop args, store result at rcvr slot.
            if (nArgs >= 1 && GET_DEBUG_BOOL(PHARO_T1_INLINE_TEMP_RETURN)) {
                a.bind(tryTempReturn);
                a.mov(x14, asmjit::Imm((uint64_t)&g_t1TempReturn_hits));
                a.ldr(x15, ptr(x14));
                a.add(x15, x15, asmjit::Imm(1));
                a.str(x15, ptr(x14));
                // x6 = temp index N (bits 48-52, 5 bits = up to 31).
                a.lsr(x6, x7, asmjit::Imm(48));
                a.and_(x6, x6, asmjit::Imm(0x1F));
                // The Nth arg lives at sp[-(nArgs - N)].  Stack layout:
                //   sp[-1] = arg(nArgs-1)
                //   sp[-2] = arg(nArgs-2)
                //   ...
                //   sp[-nArgs] = arg(0)
                //   sp[-(nArgs+1)] = recv
                // For temp N (0-indexed) = arg N, byte offset from sp =
                //   -(nArgs - N) * 8 = (N - nArgs) * 8.
                // Compute: argAddr = sp + (N - nArgs) * 8.
                // x2 = sp (set above at top of emit).
                a.sub(x6, x6, asmjit::Imm(nArgs));  // x6 = N - nArgs
                a.lsl(x6, x6, asmjit::Imm(3));       // x6 = (N - nArgs) * 8
                a.add(x6, x2, x6);                    // x6 = sp + that offset
                a.ldr(x3, ptr(x6));   // x3 = the arg value
                a.stur(x3, ptr(x2, rcvrOffsetBytes));  // store at recv slot
                if (nArgs > 0) {
                    a.sub(x2, x2, asmjit::Imm(8 * nArgs));
                    emitStoreSp(a, x2);
                }
                a.b(tosSendRes ? tosLrearm : endOfSend);
            }

            // === W2: tryIntCmpReturn (bit 53, nArgs == 1) ===
            // Pattern: `^ self <cmpOp> arg0` for SmI receiver + SmI arg.
            // Extras bits 48-50 encode cmpKind:
            //   0=< 1=> 2=<= 3=>= 4=== 5=~=
            // Result: true/false oop written to rcvr slot, args dropped.
            // Bail to dispatchCached if arg isn't SmI (rare for hot
            // comparators since IC HIT already filtered receiver by class
            // but receiver class doesn't guarantee SmI arg type).
            if (nArgs == 1 && GET_DEBUG_BOOL(PHARO_T1_INLINE_INT_CMP_RETURN)) {
                a.bind(tryIntCmpReturn);
                a.mov(x14, asmjit::Imm((uint64_t)&g_t1IntCmpReturn_hits));
                a.ldr(x15, ptr(x14));
                a.add(x15, x15, asmjit::Imm(1));
                a.str(x15, ptr(x14));
                // Receiver in x1 (already loaded), arg in sp[-1].
                // Verify both are SmI (low 3 bits = 001).
                a.ldur(x3, ptr(x2, -8));     // x3 = arg0
                // Tag check both: receiver might be heap (IC HIT class
                // says receiver is the target's expected class, but if
                // target's receiver-class is SmallInteger then x1 is SmI).
                a.orr(x4, x1, x3);
                a.and_(x4, x4, asmjit::Imm(0x7));
                a.cmp(x4, asmjit::Imm(1));
                a.b_ne(dispatchCached);
                // Extract cmpKind from bits 48-50.
                a.lsr(x6, x7, asmjit::Imm(48));
                a.and_(x6, x6, asmjit::Imm(0x7));
                // Compare tagged SmI values directly — order is preserved
                // because both have same tag.
                a.cmp(x1, x3);
                // Build result by chain: load trueOop/falseOop based on
                // condition + cmpKind.
                a.ldr(x4, ptr(x0, OFF_TRUEOOP));
                a.ldr(x5, ptr(x0, OFF_FALSEOOP));
                // Dispatch on cmpKind.  Each kind: csel result based on
                // condition flags.
                asmjit::Label cmpLT = a.new_label();
                asmjit::Label cmpGT = a.new_label();
                asmjit::Label cmpLE = a.new_label();
                asmjit::Label cmpGE = a.new_label();
                asmjit::Label cmpEQ = a.new_label();
                asmjit::Label cmpNE = a.new_label();
                asmjit::Label cmpDone = a.new_label();
                a.cbz(x6, cmpLT);
                a.cmp(x6, asmjit::Imm(1));
                a.b_eq(cmpGT);
                a.cmp(x6, asmjit::Imm(2));
                a.b_eq(cmpLE);
                a.cmp(x6, asmjit::Imm(3));
                a.b_eq(cmpGE);
                a.cmp(x6, asmjit::Imm(4));
                a.b_eq(cmpEQ);
                a.cmp(x6, asmjit::Imm(5));
                a.b_eq(cmpNE);
                a.b(dispatchCached);  // unknown kind
                a.bind(cmpLT);
                a.cmp(x1, x3);
                a.csel(x3, x4, x5, asmjit::a64::CondCode::kLT);
                a.b(cmpDone);
                a.bind(cmpGT);
                a.cmp(x1, x3);
                a.csel(x3, x4, x5, asmjit::a64::CondCode::kGT);
                a.b(cmpDone);
                a.bind(cmpLE);
                a.cmp(x1, x3);
                a.csel(x3, x4, x5, asmjit::a64::CondCode::kLE);
                a.b(cmpDone);
                a.bind(cmpGE);
                a.cmp(x1, x3);
                a.csel(x3, x4, x5, asmjit::a64::CondCode::kGE);
                a.b(cmpDone);
                a.bind(cmpEQ);
                a.cmp(x1, x3);
                a.csel(x3, x4, x5, asmjit::a64::CondCode::kEQ);
                a.b(cmpDone);
                a.bind(cmpNE);
                a.cmp(x1, x3);
                a.csel(x3, x4, x5, asmjit::a64::CondCode::kNE);
                a.bind(cmpDone);
                a.stur(x3, ptr(x2, rcvrOffsetBytes));
                a.sub(x2, x2, asmjit::Imm(8));  // drop 1 arg
                emitStoreSp(a, x2);
                a.b(tosSendRes ? tosLrearm : endOfSend);
            }

            // === W3: tryIntArithReturn (bit 52, nArgs == 1) ===
            // Pattern: `^ self <op> arg0` for SmI receiver + SmI arg.
            // op encoded in bits 48-50: 0=+ 1=- 2=*.
            // Tagged add/sub: a + b = (a_val<<3|1) + (b_val<<3|1) =
            // (a_val+b_val)<<3 + 2 → subtract 1 to fix tag.
            // For mul: untag both, multiply, retag.
            // Overflow → bail to dispatchCached.
            if (nArgs == 1 && GET_DEBUG_BOOL(PHARO_T1_INLINE_INT_ARITH_RETURN)) {
                a.bind(tryIntArithReturn);
                a.mov(x14, asmjit::Imm((uint64_t)&g_t1IntArithReturn_hits));
                a.ldr(x15, ptr(x14));
                a.add(x15, x15, asmjit::Imm(1));
                a.str(x15, ptr(x14));
                a.ldur(x3, ptr(x2, -8));     // x3 = arg0
                a.orr(x4, x1, x3);
                a.and_(x4, x4, asmjit::Imm(0x7));
                a.cmp(x4, asmjit::Imm(1));
                a.b_ne(dispatchCached);
                a.lsr(x6, x7, asmjit::Imm(48));
                a.and_(x6, x6, asmjit::Imm(0x7));
                asmjit::Label arDone = a.new_label();
                asmjit::Label arSub = a.new_label();
                asmjit::Label arMul = a.new_label();
                a.cmp(x6, asmjit::Imm(1));
                a.b_eq(arSub);
                a.cmp(x6, asmjit::Imm(2));
                a.b_eq(arMul);
                // Add: x4 = x1 + x3, untag tag-bit (subtract 1).
                a.adds(x4, x1, x3);
                a.b_vs(dispatchCached);  // overflow
                a.sub(x4, x4, asmjit::Imm(1));
                a.b(arDone);
                a.bind(arSub);
                a.subs(x4, x1, x3);
                a.b_vs(dispatchCached);
                a.add(x4, x4, asmjit::Imm(1));
                a.b(arDone);
                a.bind(arMul);
                // Untag, multiply with overflow check, retag.
                a.asr(x6, x1, asmjit::Imm(3));   // au = x1 >> 3
                a.asr(x8, x3, asmjit::Imm(3));   // bu = x3 >> 3
                a.mul(x4, x6, x8);
                // Overflow if smul64(x6, x8) doesn't fit in 64 bits:
                // smulh + asr 63 must equal mul-result's sign.
                a.smulh(x9, x6, x8);
                a.asr(x10, x4, asmjit::Imm(63));
                a.cmp(x9, x10);
                a.b_ne(dispatchCached);
                // Retag: shift left 3, OR with 1.
                // Check the tagged value also doesn't overflow 61 bits.
                a.lsl(x9, x4, asmjit::Imm(3));
                a.asr(x10, x9, asmjit::Imm(3));
                a.cmp(x10, x4);
                a.b_ne(dispatchCached);
                a.orr(x4, x9, asmjit::Imm(1));
                a.bind(arDone);
                a.stur(x4, ptr(x2, rcvrOffsetBytes));
                a.sub(x2, x2, asmjit::Imm(8));
                emitStoreSp(a, x2);
                a.b(tosSendRes ? tosLrearm : endOfSend);
            }

            // === W6: tryEvenOdd (bit 51, nArgs == 0) ===
            // Pattern: `^ (self bitAnd: 1) = 0` (even) or `... = 1` (odd).
            // Kind in bit 48: 0=even, 1=odd.
            //
            // For SmI receiver: bits = (val<<3)|1.  bits & 9 keeps tag bit
            // + low bit of val:
            //   = 1 → val low bit = 0 → even
            //   = 9 → val low bit = 1 → odd
            // For "even" predicate: result = (bits&9 == 1) ? trueOop : falseOop.
            // For "odd"  predicate: result = (bits&9 == 9) ? trueOop : falseOop.
            // Bail to dispatchCached on non-SmI receiver.
            if (nArgs == 0 && GET_DEBUG_BOOL(PHARO_T1_INLINE_EVEN_ODD)) {
                a.bind(tryEvenOdd);
                a.mov(x14, asmjit::Imm((uint64_t)&g_t1EvenOdd_hits));
                a.ldr(x15, ptr(x14));
                a.add(x15, x15, asmjit::Imm(1));
                a.str(x15, ptr(x14));
                // SmI tag check: low 3 bits == 1.
                a.and_(x4, x1, asmjit::Imm(0x7));
                a.cmp(x4, asmjit::Imm(1));
                a.b_ne(dispatchCached);
                // x4 = bits & 9.  NOTE: 9 (0b1001) is not a valid AArch64
                // bitmask immediate (two non-contiguous 1-bits) — `and #9`
                // would be silently dropped by asmjit, leaving x4 = full oop.
                // Materialize 9 in a scratch reg and use the register form.
                a.mov(x6, asmjit::Imm(9));
                a.and_(x4, x1, x6);
                // Extract kind from bit 48.
                a.lsr(x6, x7, asmjit::Imm(48));
                a.and_(x6, x6, asmjit::Imm(1));
                // For even: cmp x4, 1 (true if eq).
                // For odd:  cmp x4, 9 (true if eq).
                // Use csel based on kind to pick the comparison value:
                a.mov(x8, asmjit::Imm(1));
                a.mov(x9, asmjit::Imm(9));
                a.cmp(x6, asmjit::Imm(0));
                a.csel(x8, x8, x9, asmjit::a64::CondCode::kEQ);
                // x8 = expected (1 for even, 9 for odd).
                a.cmp(x4, x8);
                // Load trueOop / falseOop.
                a.ldr(x10, ptr(x0, OFF_TRUEOOP));
                a.ldr(x11, ptr(x0, OFF_FALSEOOP));
                a.csel(x3, x10, x11, asmjit::a64::CondCode::kEQ);
                // Store result at rcvr slot.  No args to drop (nArgs=0).
                a.stur(x3, ptr(x2, rcvrOffsetBytes));
                a.b(tosSendRes ? tosLrearm : endOfSend);
            }

            // === Multi-slot inline (bit 57, nArgs==0 only) ===
            // jit-may22b: pattern `^ self[A] op1 self[B] op2 const`
            // commonly `OrderedCollection>>size = lastIndex - firstIndex + 1`.
            // Extras encoding (see Interpreter.cpp:18521+):
            //   bits 0-7 : multiSlotA (ivar index)
            //   bits 8-15: multiSlotB (ivar index)
            //   bits 16-23: multiConst (-1, 0, or +1; int8)
            //   bit 24 : op1Sub (0=add, 1=sub between A and B)
            //   bit 25 : op2Sub (0=add, 1=sub with const)
            // x1 = receiver (heap), x7 = extras.  Reads two slots, computes
            // tagged SmI arith with overflow check, writes result to
            // receiver slot, bails to endOfSend.  On overflow or non-SmI
            // slots, bails to dispatchCached.
            if (nArgs == 0) {
                a.bind(tryMultiSlot);
                // Extract A (bits 0-7) → x3, B (bits 8-15) → x4.
                a.and_(x3, x7, asmjit::Imm(0xFF));
                a.lsr(x4, x7, asmjit::Imm(8));
                a.and_(x4, x4, asmjit::Imm(0xFF));
                // Load slots[A] → x6, slots[B] → x9 from receiver.
                a.add(x5, x1, x3, asmjit::a64::lsl(3));
                a.ldr(x6, ptr(x5, 8));                  // slot A
                a.add(x5, x1, x4, asmjit::a64::lsl(3));
                a.ldr(x9, ptr(x5, 8));                  // slot B
                // Both must be SmI: low 3 bits == 1.
                a.and_(x3, x6, asmjit::Imm(7));
                a.sub(x3, x3, asmjit::Imm(1));
                a.and_(x4, x9, asmjit::Imm(7));
                a.sub(x4, x4, asmjit::Imm(1));
                a.orr(x3, x3, x4);
                a.cbnz(x3, dispatchCached);
                // op1: tagged a op tagged b.  For SmI bits = (val << 3) | 1:
                //   a_bits + b_bits = (a+b)*8 + 2 → sub 1 to retag.
                //   a_bits - b_bits = (a-b)*8 → add 1 to retag.
                asmjit::Label op1IsAdd = a.new_label();
                asmjit::Label op1Done = a.new_label();
                a.tbz(x7, asmjit::Imm(24), op1IsAdd);
                // op1 = sub: result = a - b + 1 (retag).
                a.subs(x6, x6, x9);
                a.b_vs(dispatchCached);                 // overflow
                a.add(x6, x6, asmjit::Imm(1));
                a.b(op1Done);
                a.bind(op1IsAdd);
                // op1 = add: result = a + b - 1 (retag).
                a.adds(x6, x6, x9);
                a.b_vs(dispatchCached);
                a.sub(x6, x6, asmjit::Imm(1));
                a.bind(op1Done);
                // x6 now has tagged op1-result.  Apply op2 with const.
                // const is signed int8 in bits 16-23.  Encode as tagged:
                //   const_tagged = (const << 3) | 1.
                // For const ∈ {-1, 0, 1}: const_tagged ∈ {0xFFFFFFFFFFFFFFF9, 1, 9}.
                a.lsr(x4, x7, asmjit::Imm(16));
                a.sxtb(x4, x4.w());                    // sign-extend bits 0-7
                a.lsl(x4, x4, asmjit::Imm(3));
                a.orr(x4, x4, asmjit::Imm(1));
                // op2: same retag math as op1.
                asmjit::Label op2IsAdd = a.new_label();
                asmjit::Label op2Done = a.new_label();
                a.tbz(x7, asmjit::Imm(25), op2IsAdd);
                a.subs(x6, x6, x4);
                a.b_vs(dispatchCached);
                a.add(x6, x6, asmjit::Imm(1));
                a.b(op2Done);
                a.bind(op2IsAdd);
                a.adds(x6, x6, x4);
                a.b_vs(dispatchCached);
                a.sub(x6, x6, asmjit::Imm(1));
                a.bind(op2Done);
                // Write tagged result to receiver slot on stack.
                a.stur(x6, ptr(x2, rcvrOffsetBytes));
                // Bump hit counter.
                a.mov(x14, asmjit::Imm((uint64_t)&g_t1MultiSlot_hits));
                a.ldr(x15, ptr(x14));
                a.add(x15, x15, asmjit::Imm(1));
                a.str(x15, ptr(x14));
                a.b(tosSendRes ? tosLrearm : endOfSend);
            }

            // === inline-prim 18 (basicNew:) — jit-may20b Step 10 ===
            // BLR to jit_rt_basic_new_with_arg.  Helper reads state.sp
            // for [rcvr=class, size] and on success updates state.sp
            // with the new oop at the receiver slot.  On failure
            // (non-class, oversize, etc.) returns 0 → bail to chain-loop.
            //
            // Only emitted for nArgs == 1 paths.
            if (nArgs == 1 && g_debug.t1InlinePrimBasicNew) {
                a.bind(tryPrimBasicNew);
                // Save x0 (state) + x30 (LR) across BLR.  Mirror the
                // jit_rt_inline_block_value_prep pattern at line ~3375.
                a.sub(asmjit::a64::sp, asmjit::a64::sp, asmjit::Imm(16));
                a.str(x0,  ptr(asmjit::a64::sp, 0));
                a.str(x30, ptr(asmjit::a64::sp, 8));
                emitSyncSpToState(a);  // sp-residency: helper reads state.sp
                a.mov(x9, asmjit::Imm((uint64_t)&jit_rt_basic_new_with_arg));
                a.blr(x9);
                // x0 = 1 on success, 0 on failure.  Move to x9 before
                // restoring x0 = state.
                a.mov(x9, x0);
                a.ldr(x0,  ptr(asmjit::a64::sp, 0));
                emitReloadSpFromState(a);  // sp-residency: helper may have changed state.sp
                a.ldr(x30, ptr(asmjit::a64::sp, 8));
                a.add(asmjit::a64::sp, asmjit::a64::sp, asmjit::Imm(16));
                {
                    asmjit::Label bailBN = a.new_label();
                    a.cbz(x9, bailBN);
                    // Hit path: bump counter, continue at endOfSend.
                    a.mov(x14, asmjit::Imm((uint64_t)&g_primBasicNew_hits));
                    a.ldr(x15, ptr(x14));
                    a.add(x15, x15, asmjit::Imm(1));
                    a.str(x15, ptr(x14));
                    a.b(tosSendRes ? tosLrearm : endOfSend);
                    a.bind(bailBN);
                    // Bail path: bump counter, route to dispatchCached.
                    a.mov(x14, asmjit::Imm((uint64_t)&g_primBasicNew_bails));
                    a.ldr(x15, ptr(x14));
                    a.add(x15, x15, asmjit::Imm(1));
                    a.str(x15, ptr(x14));
                    a.b(dispatchCached);
                }
            }

            // === jit-may23 T4: inline-prim 17 (basicNew 0-arg) ===
            // Mirror tryPrimBasicNew but calls jit_rt_basic_new.
            if (nArgs == 0 && g_debug.t1InlinePrimBasicNew) {
                a.bind(tryPrimBasicNewZero);
                a.sub(asmjit::a64::sp, asmjit::a64::sp, asmjit::Imm(16));
                a.str(x0,  ptr(asmjit::a64::sp, 0));
                a.str(x30, ptr(asmjit::a64::sp, 8));
                emitSyncSpToState(a);  // sp-residency: helper reads state.sp
                a.mov(x9, asmjit::Imm((uint64_t)&jit_rt_basic_new));
                a.blr(x9);
                a.mov(x9, x0);
                a.ldr(x0,  ptr(asmjit::a64::sp, 0));
                emitReloadSpFromState(a);  // sp-residency: helper may have changed state.sp
                a.ldr(x30, ptr(asmjit::a64::sp, 8));
                a.add(asmjit::a64::sp, asmjit::a64::sp, asmjit::Imm(16));
                {
                    asmjit::Label bailBNZ = a.new_label();
                    a.cbz(x9, bailBNZ);
                    a.mov(x14, asmjit::Imm(
                        (uint64_t)&g_primBasicNewZero_hits));
                    a.ldr(x15, ptr(x14));
                    a.add(x15, x15, asmjit::Imm(1));
                    a.str(x15, ptr(x14));
                    a.b(tosSendRes ? tosLrearm : endOfSend);
                    a.bind(bailBNZ);
                    a.mov(x14, asmjit::Imm(
                        (uint64_t)&g_primBasicNewZero_bails));
                    a.ldr(x15, ptr(x14));
                    a.add(x15, x15, asmjit::Imm(1));
                    a.str(x15, ptr(x14));
                    a.b(dispatchCached);
                }
            }

            // Counter helper for inline-prim diagnostics (PHARO_T1_INLINE_PRIM_COUNTERS=1).
            auto emitIncPrimCounter = [&](uint64_t addr) {
                if (!pharo::g_debug.t1InlinePrimCounters) return;
                a.mov(x14, asmjit::Imm(addr));
                a.ldr(x15, ptr(x14));
                a.add(x15, x15, asmjit::Imm(1));
                a.str(x15, ptr(x14));
            };

            // === Inline SmI bitwise prims (primKind 11/12/19) ===
            // Receiver was confirmed SmI in the dispatch (tst x1, 0x7 → ne).
            // Arg must also be SmI; bail to dispatchCached if not.
            // bitAnd/bitOr preserve the tag (both operands have low 3 = 001,
            // AND/OR keeps it 001).  bitXor produces low 3 = 000, re-OR
            // SMI_TAG to retag.  Mirrors stencils.cpp:1609-1614.
            // x2 holds SP; rcvrOffsetBytes = -8 * (nArgs+1) = -16 for nArgs=1.
            // x1 = receiver (tagged), arg at sp[-8].
            if (nArgs == 1 && g_debug.t1InlinePrimBitOps) {
                a.bind(tryPrimBitAnd);
                emitIncPrimCounter((uint64_t)&g_primBitOp_hits);
                a.ldur(x3, ptr(x2, -8));               // arg
                a.and_(x6, x3, asmjit::Imm(0x7));
                a.cmp(x6, asmjit::Imm(1));
                a.b_ne(dispatchCached);
                a.and_(x4, x1, x3);                    // bitAnd tagged
                a.stur(x4, ptr(x2, rcvrOffsetBytes));
                a.sub(x2, x2, asmjit::Imm(8 * nArgs));
                emitStoreSp(a, x2);
                a.b(tosSendRes ? tosLrearm : endOfSend);

                a.bind(tryPrimBitOr);
                emitIncPrimCounter((uint64_t)&g_primBitOp_hits);
                a.ldur(x3, ptr(x2, -8));
                a.and_(x6, x3, asmjit::Imm(0x7));
                a.cmp(x6, asmjit::Imm(1));
                a.b_ne(dispatchCached);
                a.orr(x4, x1, x3);                     // bitOr tagged
                a.stur(x4, ptr(x2, rcvrOffsetBytes));
                a.sub(x2, x2, asmjit::Imm(8 * nArgs));
                emitStoreSp(a, x2);
                a.b(tosSendRes ? tosLrearm : endOfSend);

                a.bind(tryPrimBitXor);
                emitIncPrimCounter((uint64_t)&g_primBitOp_hits);
                a.ldur(x3, ptr(x2, -8));
                a.and_(x6, x3, asmjit::Imm(0x7));
                a.cmp(x6, asmjit::Imm(1));
                a.b_ne(dispatchCached);
                a.eor(x4, x1, x3);                     // bitXor (clears tag)
                a.orr(x4, x4, asmjit::Imm(0x1));       // restore SMI_TAG
                a.stur(x4, ptr(x2, rcvrOffsetBytes));
                a.sub(x2, x2, asmjit::Imm(8 * nArgs));
                emitStoreSp(a, x2);
                a.b(tosSendRes ? tosLrearm : endOfSend);

                // === Inline SmI bitShift: (primKind 13) ===
                // Mirrors stencils.cpp:1615-1626.  Positive count = shift
                // left with overflow check; negative count = shift right.
                // x1 = tagged receiver, arg at sp[-8].
                a.bind(tryPrimBitShift);
                emitIncPrimCounter((uint64_t)&g_primBitOp_hits);
                {
                    asmjit::Label shiftRight = a.new_label();
                    asmjit::Label shiftDone = a.new_label();
                    a.ldur(x3, ptr(x2, -8));           // x3 = tagged shift count
                    a.and_(x6, x3, asmjit::Imm(0x7));
                    a.cmp(x6, asmjit::Imm(1));
                    a.b_ne(dispatchCached);           // not SmI
                    a.asr(x3, x3, asmjit::Imm(3));    // untag shift (signed)
                    a.asr(x4, x1, asmjit::Imm(3));    // untag receiver (signed)
                    a.cmp(x3, asmjit::Imm(0));
                    a.b_lt(shiftRight);
                    // positive: shift left with overflow check.
                    // Bail if shift >= 61 (would overflow SmI for any
                    // non-zero a) — stencils.cpp's `b < 61` gate.
                    a.cmp(x3, asmjit::Imm(61));
                    a.b_ge(dispatchCached);
                    a.lsl(x6, x4, x3);                // r = a << b
                    // Overflow check: (r >> b) == a iff no overflow.
                    a.asr(x5, x6, x3);
                    a.cmp(x5, x4);
                    a.b_ne(dispatchCached);
                    a.b(shiftDone);
                    a.bind(shiftRight);
                    // count < 0: shift right by -count.  Stencils uses
                    // `b > -64`, so |count| < 64.
                    a.neg(x3, x3);
                    a.cmp(x3, asmjit::Imm(64));
                    a.b_ge(dispatchCached);
                    a.asr(x6, x4, x3);                // r = a >> -b (signed)
                    a.bind(shiftDone);
                    // Retag: (r << 3) | 1.
                    a.lsl(x6, x6, asmjit::Imm(3));
                    a.orr(x6, x6, asmjit::Imm(0x1));
                    a.stur(x6, ptr(x2, rcvrOffsetBytes));
                    a.sub(x2, x2, asmjit::Imm(8 * nArgs));
                    emitStoreSp(a, x2);
                    a.b(tosSendRes ? tosLrearm : endOfSend);
                }

                // === F5 R80: Inline SmI mul (primKind 9) ===
                // Mirrors my 0x68 bytecode mul emit but at IC HIT level.
                // x1 = tagged receiver (SmI confirmed earlier), arg at sp[-8].
                a.bind(tryPrimMul);
                emitIncPrimCounter((uint64_t)&g_primBitOp_hits);
                a.ldur(x3, ptr(x2, -8));                 // tagged arg
                a.and_(x6, x3, asmjit::Imm(0x7));
                a.cmp(x6, asmjit::Imm(1));
                a.b_ne(dispatchCached);                  // arg not SmI
                a.asr(x4, x1, asmjit::Imm(3));           // untag rcvr
                a.asr(x6, x3, asmjit::Imm(3));           // untag arg
                a.mul(x9, x4, x6);                       // lo
                a.smulh(x10, x4, x6);                    // hi
                a.asr(x11, x9, asmjit::Imm(63));         // sign-ext(lo)
                a.cmp(x10, x11);
                a.b_ne(dispatchCached);                  // overflow
                a.lsl(x9, x9, asmjit::Imm(3));
                a.orr(x9, x9, asmjit::Imm(1));           // retag
                a.stur(x9, ptr(x2, rcvrOffsetBytes));
                a.sub(x2, x2, asmjit::Imm(8 * nArgs));
                emitStoreSp(a, x2);
                a.b(tosSendRes ? tosLrearm : endOfSend);

                // === F5 R81: Inline == (primKind 10) ===
                // ProtoObject>>== compares two Oops by raw bits.  Works
                // for any receiver tag (SmI, immediate, heap).
                // x1 = receiver, arg at sp[-8].
                a.bind(tryPrimEq);
                emitIncPrimCounter((uint64_t)&g_primBitOp_hits);
                a.ldur(x3, ptr(x2, -8));                  // arg
                a.ldp(x4, x5, ptr(x0, OFF_TRUEOOP));      // x4=true, x5=false
                a.cmp(x1, x3);
                a.csel(x6, x4, x5, asmjit::a64::CondCode::kEQ);
                a.stur(x6, ptr(x2, rcvrOffsetBytes));
                a.sub(x2, x2, asmjit::Imm(8 * nArgs));
                emitStoreSp(a, x2);
                a.b(tosSendRes ? tosLrearm : endOfSend);
            } else {
                // Labels still need to be bound somewhere even if unused —
                // bind them as aliases for dispatchCached.
                a.bind(tryPrimBitAnd);
                a.bind(tryPrimBitOr);
                a.bind(tryPrimBitXor);
                a.bind(tryPrimBitShift);
                a.bind(tryPrimMul);
                a.bind(tryPrimEq);
                a.b(dispatchCached);
            }

            // Note: primKind 10 (==) inline was attempted earlier but
            // the dispatch overhead (~4 instr/send applied unconditionally)
            // didn't pay off — bytecode 0x76 already inlines == for SmI
            // and named-send #== is rare.  Could revisit if a workload
            // shows hot named-send == invocations.

            // === Inline at: (primKind 14, nArgs=1, heap receiver) ===
            // Receiver class is whatever IC matched; runtime check fmt
            // and decode header slot count.  Two paths:
            //   fmt == 2  (variable pointer / Array): return slot[idx]
            //   fmt 16-23 (indexable bytes / ByteArray, String):
            //              return SmI of byte at idx
            // Mirrors stencils.cpp:1538-1545 (Array path).
            // x1 = recv (heap), x2 = sp, x7 = extras.
            if (nArgs == 1 && g_debug.t1InlinePrimAt) {
                asmjit::Label tryByteAt = a.new_label();
                a.bind(tryPrimAt);
                // Blocker #4 layout-vs-execution test: emit N behavior-neutral
                // NOPs at the inline-at entry.  If this layout shift alone masks
                // the bug, the "fixes" are layout artifacts (→ memory-corruption
                // hunt); if not, the inline-at execution is genuinely the cause.
                for (int nn = 0; nn < GET_DEBUG_INT(PHARO_T1_AT_NOPS); nn++) a.nop();
                emitIncPrimCounter((uint64_t)&g_primAt_hits);
                // Stash icDataPtr to memory so bails restore it via
                // dispatchCachedRestoreX5 (x5 gets clobbered with slotCount).
                a.str(x5, ptr(x0, OFF_ICDATAPTR));
                a.ldr(x4, ptr(x1));                  // x4 = header word
                a.lsr(x6, x4, asmjit::Imm(24));      // shift fmt to low
                a.and_(x6, x6, asmjit::Imm(0x1F));
                a.cmp(x6, asmjit::Imm(2));           // fmt 2 = Array path
                a.b_ne(tryByteAt);                   // else try byte path
                // Slot count in header bits 56:63 (1 byte). If 255 = overflow.
                a.lsr(x5, x4, asmjit::Imm(56));
                a.and_(x5, x5, asmjit::Imm(0xFF));
                a.cmp(x5, asmjit::Imm(255));
                a.b_eq(dispatchCachedRestoreX5);     // overflow → slow
                // Load idx (arg)
                a.ldur(x3, ptr(x2, -8));
                a.and_(x6, x3, asmjit::Imm(0x7));
                a.cmp(x6, asmjit::Imm(1));
                a.b_ne(dispatchCachedRestoreX5);     // arg not SmI
                a.asr(x4, x3, asmjit::Imm(3));       // x4 = idx (signed untag)
                a.cmp(x4, asmjit::Imm(1));
                a.b_lt(dispatchCachedRestoreX5);     // idx < 1
                a.cmp(x4, x5);
                a.b_gt(dispatchCachedRestoreX5);     // idx > sc
                // Slot offset = idx * 8 (slot[idx-1] at offset 8 + (idx-1)*8)
                a.lsl(x4, x4, asmjit::Imm(3));
                a.add(x6, x1, x4);
                a.ldr(x4, ptr(x6));                  // val = recv[idx*8]
                // PHARO_T1_VERIFY_AT=1: recompute the read in C++ and log any
                // mismatch (blocker #4 inline-at diagnostic).  x1=recv, x3=idx
                // (tagged), x4=inline result.  Save caller-saved regs we need.
                if (GET_DEBUG_BOOL(PHARO_T1_VERIFY_AT)) {
                    a.sub(asmjit::a64::sp, asmjit::a64::sp, asmjit::Imm(48));
                    a.str(x0,  ptr(asmjit::a64::sp, 0));
                    a.str(x30, ptr(asmjit::a64::sp, 8));
                    a.str(x1,  ptr(asmjit::a64::sp, 16));
                    a.str(x2,  ptr(asmjit::a64::sp, 24));
                    a.str(x4,  ptr(asmjit::a64::sp, 32));
                    a.mov(x1, x1);                   // arg1 = recv (already)
                    a.mov(x2, x3);                   // arg2 = idx (tagged)
                    a.mov(x3, x4);                   // arg3 = inline result
                    a.mov(x9, asmjit::Imm((uint64_t)&jit_rt_verify_inline_at));
                    a.blr(x9);
                    a.ldr(x0,  ptr(asmjit::a64::sp, 0));
                    a.ldr(x30, ptr(asmjit::a64::sp, 8));
                    a.ldr(x1,  ptr(asmjit::a64::sp, 16));
                    a.ldr(x2,  ptr(asmjit::a64::sp, 24));
                    a.ldr(x4,  ptr(asmjit::a64::sp, 32));
                    a.add(asmjit::a64::sp, asmjit::a64::sp, asmjit::Imm(48));
                }
                a.stur(x4, ptr(x2, rcvrOffsetBytes));
                a.sub(x2, x2, asmjit::Imm(8 * nArgs));
                emitStoreSp(a, x2);
                a.b(tosSendRes ? tosLrearm : endOfSend);

                // Byte-indexed at:: fmt 16-23 (ByteArray, String, Symbol).
                // byteSize = slotCount*8 - (fmt & 7).  byte[idx] at
                // recvAddr + 8 + (idx-1).  Returns SmI of byte value.
                a.bind(tryByteAt);
                a.sub(x8, x6, asmjit::Imm(16));      // x8 = fmt - 16
                a.cmp(x8, asmjit::Imm(8));
                a.b_hs(dispatchCachedRestoreX5);     // fmt not in 16..23
                a.lsr(x5, x4, asmjit::Imm(56));
                a.and_(x5, x5, asmjit::Imm(0xFF));
                a.cmp(x5, asmjit::Imm(255));
                a.b_eq(dispatchCachedRestoreX5);     // slotCount==255 → ovf
                a.lsl(x5, x5, asmjit::Imm(3));       // sc*8 = max byte capacity
                a.and_(x6, x6, asmjit::Imm(0x7));    // extra bytes (= fmt&7)
                a.sub(x5, x5, x6);                   // byteSize
                a.ldur(x3, ptr(x2, -8));
                a.and_(x6, x3, asmjit::Imm(0x7));
                a.cmp(x6, asmjit::Imm(1));
                a.b_ne(dispatchCachedRestoreX5);     // idx not SmI
                a.asr(x4, x3, asmjit::Imm(3));       // x4 = idx
                a.cmp(x4, asmjit::Imm(1));
                a.b_lt(dispatchCachedRestoreX5);
                a.cmp(x4, x5);
                a.b_gt(dispatchCachedRestoreX5);     // idx > byteSize
                // byte at recvAddr + 8 + (idx-1) = recvAddr + 7 + idx
                a.add(x4, x4, asmjit::Imm(7));
                a.add(x6, x1, x4);
                a.ldrb(w4, ptr(x6));                 // zero-extended byte
                a.lsl(x4, x4, asmjit::Imm(3));       // retag SmI
                a.orr(x4, x4, asmjit::Imm(0x1));
                a.stur(x4, ptr(x2, rcvrOffsetBytes));
                a.sub(x2, x2, asmjit::Imm(8 * nArgs));
                emitStoreSp(a, x2);
                a.b(tosSendRes ? tosLrearm : endOfSend);
            } else if (!(nArgs == 2 && g_debug.t1InlinePrimAt)) {
                a.bind(tryPrimAt);
                a.b(dispatchCached);
            }

            // === Inline at:put: (primKind 15, nArgs=2, heap receiver) ===
            // Mirrors stencils.cpp:1546-1554.  Also gates on immutable
            // flag (bit 23 of header).
            // sp layout before send: [..., recv, idx, val]  (val at sp[-1])
            if (nArgs == 2 && g_debug.t1InlinePrimAt
                    && !GET_DEBUG_BOOL(PHARO_T1_NO_INLINE_PRIM_ATPUT)) {
                asmjit::Label tryByteAtPut = a.new_label();
                a.bind(tryPrimAtPut);
                emitIncPrimCounter((uint64_t)&g_primAtPut_hits);
                // Stash icDataPtr — bails route through dispatchCachedRestoreX5.
                a.str(x5, ptr(x0, OFF_ICDATAPTR));
                a.ldr(x4, ptr(x1));                  // x4 = header word
                a.lsr(x6, x4, asmjit::Imm(24));
                a.and_(x6, x6, asmjit::Imm(0x1F));
                a.cmp(x6, asmjit::Imm(2));           // fmt 2 Array path
                a.b_ne(tryByteAtPut);
                // Immutability check (bit 23)
                a.tbnz(x4, asmjit::Imm(23), dispatchCachedRestoreX5);
                a.lsr(x5, x4, asmjit::Imm(56));
                a.and_(x5, x5, asmjit::Imm(0xFF));
                a.cmp(x5, asmjit::Imm(255));
                a.b_eq(dispatchCachedRestoreX5);
                // sp[-1] = val, sp[-2] = idx (after pushes for nArgs=2)
                a.ldur(x3, ptr(x2, -16));            // idx
                a.and_(x6, x3, asmjit::Imm(0x7));
                a.cmp(x6, asmjit::Imm(1));
                a.b_ne(dispatchCachedRestoreX5);
                a.asr(x4, x3, asmjit::Imm(3));       // idx untagged
                a.cmp(x4, asmjit::Imm(1));
                a.b_lt(dispatchCachedRestoreX5);
                a.cmp(x4, x5);
                a.b_gt(dispatchCachedRestoreX5);
                // Load val + store at recv[idx*8]
                a.ldur(x3, ptr(x2, -8));             // x3 = val
                a.lsl(x4, x4, asmjit::Imm(3));
                a.add(x6, x1, x4);
                a.str(x3, ptr(x6));                  // recv[idx*8] = val
                // at:put: returns the value (sp[-3]=recv slot becomes val)
                a.stur(x3, ptr(x2, rcvrOffsetBytes));
                a.sub(x2, x2, asmjit::Imm(8 * nArgs));
                emitStoreSp(a, x2);
                a.b(tosSendRes ? tosLrearm : endOfSend);

                // Byte at:put: for fmt 16-23.  Val must be SmI in 0..255.
                a.bind(tryByteAtPut);
                a.sub(x8, x6, asmjit::Imm(16));
                a.cmp(x8, asmjit::Imm(8));
                a.b_hs(dispatchCachedRestoreX5);
                a.tbnz(x4, asmjit::Imm(23), dispatchCachedRestoreX5);
                a.lsr(x5, x4, asmjit::Imm(56));
                a.and_(x5, x5, asmjit::Imm(0xFF));
                a.cmp(x5, asmjit::Imm(255));
                a.b_eq(dispatchCachedRestoreX5);
                a.lsl(x5, x5, asmjit::Imm(3));      // sc*8
                a.and_(x6, x6, asmjit::Imm(0x7));    // extra bytes (fmt-16)
                a.sub(x5, x5, x6);                   // byteSize
                a.ldur(x3, ptr(x2, -16));            // idx (tagged SmI)
                a.and_(x6, x3, asmjit::Imm(0x7));
                a.cmp(x6, asmjit::Imm(1));
                a.b_ne(dispatchCachedRestoreX5);
                a.asr(x4, x3, asmjit::Imm(3));       // idx
                a.cmp(x4, asmjit::Imm(1));
                a.b_lt(dispatchCachedRestoreX5);
                a.cmp(x4, x5);
                a.b_gt(dispatchCachedRestoreX5);
                a.ldur(x3, ptr(x2, -8));             // val (tagged SmI)
                a.and_(x6, x3, asmjit::Imm(0x7));
                a.cmp(x6, asmjit::Imm(1));
                a.b_ne(dispatchCachedRestoreX5);     // val not SmI
                a.asr(x6, x3, asmjit::Imm(3));       // val untagged
                a.cmp(x6, asmjit::Imm(255));
                a.b_hi(dispatchCachedRestoreX5);     // val > 255
                // Store byte at recvAddr + 8 + (idx-1) = recv + 7 + idx.
                // x6 holds the untagged value; use x7 for the address.
                a.add(x4, x4, asmjit::Imm(7));
                a.add(x7, x1, x4);
                a.strb(w6, ptr(x7));
                // at:put: returns the value (tagged SmI), write at recv slot
                a.stur(x3, ptr(x2, rcvrOffsetBytes));
                a.sub(x2, x2, asmjit::Imm(8 * nArgs));
                emitStoreSp(a, x2);
                a.b(tosSendRes ? tosLrearm : endOfSend);
            } else if (!(nArgs == 1 && g_debug.t1InlinePrimAt)
                       && !(nArgs == 0 && g_debug.t1InlinePrimAt)) {
                a.bind(tryPrimAtPut);
                a.b(dispatchCached);
            }

            // === Inline size (primKind 16, nArgs=0, heap receiver) ===
            // Three paths by header fmt:
            //   fmt 2:     Array — size = slotCount
            //   fmt 16-23: byte indexable — size = slotCount*8 - (fmt&7)
            //   fmt 9:     Indexable64 — size = slotCount
            // Result is SmI(byteSize-or-slotCount).
            if (nArgs == 0 && g_debug.t1InlinePrimAt) {
                asmjit::Label sizeBytes = a.new_label();
                asmjit::Label sizeDone = a.new_label();
                a.bind(tryPrimSize);
                emitIncPrimCounter((uint64_t)&g_primSize_hits);
                // Stash icDataPtr — bails route through dispatchCachedRestoreX5.
                a.str(x5, ptr(x0, OFF_ICDATAPTR));
                a.ldr(x4, ptr(x1));                  // header
                a.lsr(x6, x4, asmjit::Imm(24));
                a.and_(x6, x6, asmjit::Imm(0x1F));
                a.lsr(x5, x4, asmjit::Imm(56));
                a.and_(x5, x5, asmjit::Imm(0xFF));
                a.cmp(x5, asmjit::Imm(255));
                a.b_eq(dispatchCachedRestoreX5);     // overflow header → slow
                // fmt 2 or 9: result = slotCount (no adjust)
                a.cmp(x6, asmjit::Imm(2));
                a.b_eq(sizeDone);
                a.cmp(x6, asmjit::Imm(9));
                a.b_eq(sizeDone);
                // F5 R84: fmt 10 = Indexable32 (size = slotCount * 2)
                // fmt 11 = Indexable32Odd (size = slotCount * 2 - 1)
                {
                    asmjit::Label notFmt10 = a.new_label();
                    asmjit::Label notFmt11 = a.new_label();
                    a.cmp(x6, asmjit::Imm(10));
                    a.b_ne(notFmt10);
                    a.lsl(x5, x5, asmjit::Imm(1));   // slotCount * 2
                    a.b(sizeDone);
                    a.bind(notFmt10);
                    a.cmp(x6, asmjit::Imm(11));
                    a.b_ne(notFmt11);
                    a.lsl(x5, x5, asmjit::Imm(1));
                    a.sub(x5, x5, asmjit::Imm(1));   // slotCount * 2 - 1
                    a.b(sizeDone);
                    a.bind(notFmt11);
                }
                // fmt 16-23: byte path
                a.sub(x8, x6, asmjit::Imm(16));
                a.cmp(x8, asmjit::Imm(8));
                a.b_hs(dispatchCachedRestoreX5);     // not byte fmt
                a.lsl(x5, x5, asmjit::Imm(3));       // sc*8
                a.and_(x6, x6, asmjit::Imm(0x7));    // extra bytes
                a.sub(x5, x5, x6);                   // byteSize
                a.bind(sizeDone);
                // Tag SmI: result = (size << 3) | 1
                a.lsl(x5, x5, asmjit::Imm(3));
                a.orr(x5, x5, asmjit::Imm(0x1));
                a.stur(x5, ptr(x2, rcvrOffsetBytes));
                a.b(tosSendRes ? tosLrearm : endOfSend);
                (void)sizeBytes;
            } else if (!(nArgs == 1 && g_debug.t1InlinePrimAt)
                       && !(nArgs == 2 && g_debug.t1InlinePrimAt)) {
                a.bind(tryPrimSize);
                a.b(dispatchCached);
            }

            // === Inline basicIdentityHash (primKind 20 = inlinePrimKind(75),
            //     nArgs=0, heap receiver) ===
            // prim 75 returns the RAW 22-bit identity hash from the header.
            // (Object>>identityHash is `^self basicIdentityHash bitShift: 8`, a
            //  separate non-primitive method — NOT what gets inlined here.)
            //
            // Blocker #4 root cause: this VM relocated the header identity-hash
            // field for ASLR.  Stock Spur keeps the hash at bits 8-29; this VM
            // puts it at bits 32-53 (ObjectHeader::HashShift == 32, classIndex
            // moved to bits 0-21).  This stencil was ported from the stock layout
            // and shifted by 8, so it extracted bits 8-29 = a mix of the classIndex
            // high bits and the format nibble — IDENTICAL for every object of the
            // same class.  So every ByteSymbol got the SAME bogus "hash", which is
            // why distinct test symbols collided to one slot (the "wrong AND shared
            // across distinct symbols, impossible normally" signature).  The real
            // primitive reads bits 32-53 correctly, so a store via this inline and
            // a lookup via the real prim disagreed → KeyNotFound; timing-dependent
            // on which path each took → the Heisenbug.
            //
            // Fix: read the hash from HashShift (32), matching
            // ObjectHeader::identityHash().  Use the named constants so the shift
            // can never silently drift from the header layout again.  Also bail to
            // the real primitive when the field is 0: the hash is assigned lazily
            // (identityHashOf calls generateHash() / registerClass() on a 0 field
            // and stores it back), which this inline read cannot do.
            if (nArgs == 0 && g_debug.t1InlinePrimAt) {
                a.bind(tryPrimIdentityHash);
                a.ldr(x4, ptr(x1));                  // header
                a.lsr(x4, x4, asmjit::Imm(pharo::ObjectHeader::IdentityHashShift));
                a.and_(x4, x4, asmjit::Imm(pharo::ObjectHeader::IdentityHashFieldMask));
                a.cbz(x4, dispatchCached);           // 0 = unhashed: let the real
                                                     // prim assign (x5 untouched).
                // Blocker #4 trace: log (receiver, raw hash). Catches the inline
                // identityHash reading a STALE/wrong receiver x1.
                if (GET_DEBUG_BOOL(PHARO_T1_TRACE_MOD)) {
                    a.sub(asmjit::a64::sp, asmjit::a64::sp, asmjit::Imm(48));
                    a.str(x0, ptr(asmjit::a64::sp, 0));
                    a.str(x30, ptr(asmjit::a64::sp, 8));
                    a.str(x1, ptr(asmjit::a64::sp, 16));
                    a.str(x2, ptr(asmjit::a64::sp, 24));
                    a.str(x4, ptr(asmjit::a64::sp, 32));
                    a.mov(x3, x0);                   // arg3 = state
                    a.mov(x0, x1);                   // arg0 = recv
                    a.mov(x1, x4);                   // arg1 = rawHash
                    a.mov(x9, asmjit::Imm((uint64_t)&jit_rt_trace_idh));
                    a.blr(x9);
                    a.ldr(x0, ptr(asmjit::a64::sp, 0));
                    a.ldr(x30, ptr(asmjit::a64::sp, 8));
                    a.ldr(x1, ptr(asmjit::a64::sp, 16));
                    a.ldr(x2, ptr(asmjit::a64::sp, 24));
                    a.ldr(x4, ptr(asmjit::a64::sp, 32));
                    a.add(asmjit::a64::sp, asmjit::a64::sp, asmjit::Imm(48));
                }
                a.lsl(x4, x4, asmjit::Imm(3));
                a.orr(x4, x4, asmjit::Imm(0x1));
                a.stur(x4, ptr(x2, rcvrOffsetBytes));
                a.b(tosSendRes ? tosLrearm : endOfSend);
            } else {
                a.bind(tryPrimIdentityHash);
                a.b(dispatchCached);
            }

            // === Inline `class` (primKind 24 = inlinePrimKind(111), nArgs=0,
            //     heap receiver) ===
            // classOf(heapObj) = classTable_[header.classIndex()].  classIndex
            // is bits 0-21 (ObjectHeader::ClassIndexMask); the class table is a
            // flat, resize-once-never-reallocated array whose base is captured
            // in g_classTableBase (ObjectMemory.cpp).  No bounds check needed
            // (classIndex is a 22-bit field; the table has 2^22 entries).  No
            // forwarding/immediate handling needed: the IC class-match already
            // excludes forwarded/immediate receivers — the same invariant the
            // size/identityHash inlines above rely on.  The result is a tagged
            // class oop (no SmI tagging).
            if (nArgs == 0 && g_debug.t1InlinePrimAt
                    && !GET_DEBUG_BOOL(PHARO_T1_NO_INLINE_CLASS)) {
                a.bind(tryPrimClass);
                emitIncPrimCounter((uint64_t)&g_primClass_hits);
                a.ldr(x4, ptr(x1));                  // header
                a.and_(x4, x4, asmjit::Imm(pharo::ObjectHeader::ClassIndexMask));
                a.lsl(x4, x4, asmjit::Imm(3));       // classIndex * 8 (entry size)
                a.mov(x5, asmjit::Imm((uint64_t)&pharo::g_classTableBase));
                a.ldr(x5, ptr(x5));                  // x5 = class table base
                a.add(x5, x5, x4);                   // &classTable[classIndex]
                a.ldr(x5, ptr(x5));                  // x5 = class oop
                a.stur(x5, ptr(x2, rcvrOffsetBytes));
                a.b(tosSendRes ? tosLrearm : endOfSend);
            } else {
                a.bind(tryPrimClass);
                a.b(dispatchCached);
            }

            // === Inline SmallFloat +/-/* at send site (primKind 21/22/23) ===
            // Receiver is SmallFloat (tag=5), arg is also SmallFloat.
            // Decode, op, encode.  Bails on ±0 or exponent over/underflow.
            if (nArgs == 1) {
                a.bind(tryPrimSmallFloatOp);
                emitIncPrimCounter((uint64_t)&g_primFloatOp_hits);
                // Stash icDataPtr — about to clobber x5.
                a.str(x5, ptr(x0, OFF_ICDATAPTR));
                // Arg at sp[-8].  Must be SmallFloat (tag 5).
                a.ldur(x3, ptr(x2, -8));
                a.and_(x4, x3, asmjit::Imm(0x7));
                a.cmp(x4, asmjit::Imm(5));
                a.b_ne(dispatchCachedRestoreX5);
                // Decode rcv.
                a.lsr(x4, x1, asmjit::Imm(3));
                a.cmp(x4, asmjit::Imm(1));
                a.b_ls(dispatchCachedRestoreX5);     // ±0 → bail
                a.mov(x5, asmjit::Imm(0x7000000000000000ULL));
                a.add(x4, x4, x5);
                a.ror(x4, x4, asmjit::Imm(1));
                a.fmov(d0, x4);
                // Decode arg.
                a.lsr(x4, x3, asmjit::Imm(3));
                a.cmp(x4, asmjit::Imm(1));
                a.b_ls(dispatchCachedRestoreX5);
                a.add(x4, x4, x5);
                a.ror(x4, x4, asmjit::Imm(1));
                a.fmov(d1, x4);
                // primKind already in x6 in lower bits 0..2 form (x6 = pk-21)
                // when we branched here (we did `sub x6, x6, #21`).
                // pk-21 = 0 → add, 1 → sub, 2 → mul.
                asmjit::Label opSub = a.new_label();
                asmjit::Label opMul = a.new_label();
                asmjit::Label opDone = a.new_label();
                a.cmp(x6, asmjit::Imm(1));
                a.b_eq(opSub);
                a.cmp(x6, asmjit::Imm(2));
                a.b_eq(opMul);
                a.fadd(d0, d0, d1);
                a.b(opDone);
                a.bind(opSub);
                a.fsub(d0, d0, d1);
                a.b(opDone);
                a.bind(opMul);
                a.fmul(d0, d0, d1);
                a.bind(opDone);
                // Encode result.
                a.fmov(x4, d0);
                a.ror(x4, x4, asmjit::Imm(63));      // ROL 1
                a.cmp(x4, x5);
                a.b_lo(dispatchCachedRestoreX5);     // underflow
                a.sub(x4, x4, x5);
                a.mov(x6, asmjit::Imm(0x1FFFFFFFFFFFFFFFULL));
                a.cmp(x4, x6);
                a.b_hi(dispatchCachedRestoreX5);     // overflow
                a.lsl(x4, x4, asmjit::Imm(3));
                // SmallFloatTag (5).  `orr #5` is not a valid AArch64 bitmask
                // immediate (asmjit drops it → tag 0 corruption); after `lsl #3`
                // the low 3 bits are 0 so `add #5` sets the tag identically.
                a.add(x4, x4, asmjit::Imm(5));        // SmallFloatTag
                a.stur(x4, ptr(x2, rcvrOffsetBytes));
                a.sub(x2, x2, asmjit::Imm(8 * nArgs));
                emitStoreSp(a, x2);
                a.b(tosSendRes ? tosLrearm : endOfSend);
            } else {
                a.bind(tryPrimSmallFloatOp);
                a.b(dispatchCached);
            }

            // === Inline-prim bail restore stub ===
            // tryPrim* blocks clobber x5 (slotCount) but dispatchCached
            // needs x5 = icDataPtr.  They stash icDataPtr to OFF_ICDATAPTR
            // at the top of each block and route bails through this label.
            a.bind(dispatchCachedRestoreX5);
            a.ldr(x5, ptr(x0, OFF_ICDATAPTR));
            // Fall through.

            // === Plain cached dispatch === (emits deferred state setup)
            a.bind(dispatchCached);
            if (g_fsrCursorVerify) {
                // M2 dual-cursor audit: x23 must equal the memory cursor
                // at every send exit (write-through contract).
                asmjit::Label c23ok = a.new_label();
                a.ldr(x16, ptr(x0, OFF_J2J_SAVE_CURSOR));
                a.cmp(x16, x23);
                a.b_eq(c23ok);
                a.brk(0xF23);
                a.bind(c23ok);
            }
            if (g_fsrX19Verify) {
                // FSR M1 oracle: x19 must equal the jitMethod mirror at
                // every send exit.  brk #0xF19 on divergence.
                asmjit::Label x19ok = a.new_label();
                a.ldr(x16, ptr(x0, OFF_JITMETHOD));
                a.cmp(x16, x19);
                a.b_eq(x19ok);
                a.brk(0xF19);
                a.bind(x19ok);
            }
            a.str(x5, ptr(x0, OFF_ICDATAPTR));
            a.mov(w3, asmjit::Imm(nArgs));
            a.str(w3, ptr(x0, OFF_SENDARGCOUNT));
            if (g_fsrLazy) {
                // M4: the method mirror may be stale (per-call store
                // deleted) — derive the exit ip from x19's bcStartCache.
                a.ldr(x6, ptr(x19, (int)offsetof(JITMethod, bcStartCache)));
                a.add(x6, x6, asmjit::Imm(bcOffsetFromMethObj - g_bcStartDelta));
            } else {
            a.ldr(x6, ptr(x0, OFF_METHOD));
            a.add(x6, x6, asmjit::Imm(bcOffsetFromMethObj));
            }
            a.str(x6, ptr(x0, OFF_IP));
            a.ldr(x6, ptr(x5, 8));            // icData[1] = method Oop
            a.str(x6, ptr(x0, OFF_CACHED_TARGET));
            a.mov(w3, asmjit::Imm(EXIT_SEND_CACHED));
            a.str(w3, ptr(x0, OFF_EXIT));
            emitSyncSpToState(a);
            a.ret(x30);

            // === Miss === (emits deferred state setup)
            a.bind(miss);
            a.str(x5, ptr(x0, OFF_ICDATAPTR));
            a.mov(w3, asmjit::Imm(nArgs));
            a.str(w3, ptr(x0, OFF_SENDARGCOUNT));
            if (g_fsrLazy) {
                // M4: the method mirror may be stale (per-call store
                // deleted) — derive the exit ip from x19's bcStartCache.
                a.ldr(x6, ptr(x19, (int)offsetof(JITMethod, bcStartCache)));
                a.add(x6, x6, asmjit::Imm(bcOffsetFromMethObj - g_bcStartDelta));
            } else {
            a.ldr(x6, ptr(x0, OFF_METHOD));
            a.add(x6, x6, asmjit::Imm(bcOffsetFromMethObj));
            }
            a.str(x6, ptr(x0, OFF_IP));
            a.mov(w3, asmjit::Imm(EXIT_SEND));
            a.str(w3, ptr(x0, OFF_EXIT));
            emitSyncSpToState(a);
            a.ret(x30);

#if PHARO_J2J_SAVE_V2
            // V2 resume continuation: every call-return lander (the V2
            // prelude's br, the trampoline's resume blr, JIT_RESUME_CALL)
            // arrives HERE with the retval in x1 and the recv+args still
            // on the stack; pop them with the STATIC count and write the
            // retval — the work the V1 prelude did dynamically.
            // Inline-spec fallthroughs jump directly to endOfSend below
            // and never execute this block (label split; jumps to the
            // post-send bytecode keep bcLabels and skip it too — the
            // resumeOverrides table points only resume machinery here).
            if (tosSendRes) {
                // Unreachable by fall-through (the prior path branched
                // to endOfSend/Lrearm); spec arrivals land here.
                a.bind(tosLrearm);
                a.ldur(x26, ptr(x25, -8));
                a.b(endOfSend);
            }
            a.bind(resumeAfterCall);
            emitLoadSp(a, x2);
            if (nArgs > 0) a.sub(x2, x2, asmjit::Imm(8 * nArgs));
            a.stur(x1, ptr(x2, -8));
            emitStoreSp(a, x2);
            if (tosSendRes) a.mov(x26, x1);   // retval = new TOS
            // Block-value returns also land here with x19 = the BLOCK's
            // JM (≠ this method even when xmethod is off) — include
            // t1InlineBlockValue in the gate.
            if (g_debug.t1InlineJ2JXmethod || g_debug.t1InlineBlockValue) {
                // Cross-method return: re-establish the CALLER's (= this
                // method's) context.  Self-identify PC-relatively: the
                // JITMethod header immediately precedes codeStart() in
                // the zone allocation, and codeStartLabel is bound at
                // machine-code offset 0 — no emit-time address needed.
                a.adr(x19, g_codeStartLabel);
                a.sub(x19, x19, asmjit::Imm((int)sizeof(JITMethod)));
                a.str(x19, ptr(x0, OFF_JITMETHOD));
                a.ldr(x12, ptr(x19, 0));
                a.str(x12, ptr(x0, OFF_METHOD));
                a.add(x12, x12, asmjit::Imm(16));
                a.str(x12, ptr(x0, OFF_LITERALS));
                a.mov(w12, asmjit::Imm(callerArgCount));
                a.str(w12, ptr(x0, OFF_ARGCOUNT));
            }
            if (g_resumeOverridesPtr)
                g_resumeOverridesPtr->emplace_back(
                    (uint32_t)globalIdx + 1, resumeAfterCall);
#endif
            a.bind(endOfSend);
            // Blocker #4 test (PHARO_T1_INLINE_SYNC): inline-spec continuations
            // reach here having updated OFF_SP but NOT OFF_SENDARGCOUNT / OFF_IP
            // (unlike dispatchCached/miss).  A subsequent exit or GC that reads
            // those stale fields could corrupt.  Sync them to this send's values
            // before falling through to the next bytecode.  bits: 1=sendArgCount
            // 2=ip(send offset).
            if (int sy = GET_DEBUG_INT(PHARO_T1_INLINE_SYNC)) {
                if (sy & 1) {
                    a.mov(w3, asmjit::Imm(nArgs));
                    a.str(w3, ptr(x0, OFF_SENDARGCOUNT));
                }
                if (sy & 2) {
                    if (g_fsrLazy) {
                        // M4: the method mirror may be stale (per-call store
                        // deleted) — derive the exit ip from x19's bcStartCache.
                        a.ldr(x6, ptr(x19, (int)offsetof(JITMethod, bcStartCache)));
                        a.add(x6, x6, asmjit::Imm(bcOffsetFromMethObj - g_bcStartDelta));
                    } else {
                    a.ldr(x6, ptr(x0, OFF_METHOD));
                    a.add(x6, x6, asmjit::Imm(bcOffsetFromMethObj));
                    }
                    a.str(x6, ptr(x0, OFF_IP));
                }
                if (sy & 4) {
                    a.ldr(x6, ptr(x0, OFF_ICDATAPTR));
                    a.ldr(x6, ptr(x6, 8));        // icData[1] = cached method
                    a.str(x6, ptr(x0, OFF_CACHED_TARGET));
                }
            }
            // Blocker #4 test (PHARO_T1_SYNC_GLOBALS): sync C++ interpreter
            // globals from the JITState at the inline-spec continuation, so any
            // C++ code reached before the next exit (e.g. a deferred scavenge)
            // sees current stack/frame/receiver/method, not JIT-entry-stale.
            if (GET_DEBUG_BOOL(PHARO_T1_SYNC_GLOBALS)) {
                a.sub(asmjit::a64::sp, asmjit::a64::sp, asmjit::Imm(16));
                a.str(x0,  ptr(asmjit::a64::sp, 0));
                a.str(x30, ptr(asmjit::a64::sp, 8));
                emitSyncSpToState(a);  // sp-residency: helper reads state.sp
                a.mov(x9, asmjit::Imm((uint64_t)&jit_rt_sync_globals));
                a.blr(x9);
                a.ldr(x0,  ptr(asmjit::a64::sp, 0));
                a.ldr(x30, ptr(asmjit::a64::sp, 8));
                a.add(asmjit::a64::sp, asmjit::a64::sp, asmjit::Imm(16));
            }
            // simStack B2c: both arrival classes re-armed x26
            // (resumeAfterCall via mov x26,x1; spec paths via tosLrearm)
            // — the send result is a valid TOS cache for the next
            // bytecode.  The SYNC_GLOBALS debug block above clobbers
            // nothing callee-saved (x26 survives its blr).
            if (tosSendRes) g_tos.valid = true;
            return true;
        }

        // Non-probe fallback: emit state setup + bail.
        a.str(x5, ptr(x0, OFF_ICDATAPTR));
        a.mov(w3, asmjit::Imm(nArgs));
        a.str(w3, ptr(x0, OFF_SENDARGCOUNT));
        if (g_fsrLazy) {
            // M4: the method mirror may be stale (per-call store
            // deleted) — derive the exit ip from x19's bcStartCache.
            a.ldr(x6, ptr(x19, (int)offsetof(JITMethod, bcStartCache)));
            a.add(x6, x6, asmjit::Imm(bcOffsetFromMethObj - g_bcStartDelta));
        } else {
        a.ldr(x6, ptr(x0, OFF_METHOD));
        a.add(x6, x6, asmjit::Imm(bcOffsetFromMethObj));
        }
        a.str(x6, ptr(x0, OFF_IP));
        a.mov(w3, asmjit::Imm(EXIT_SEND));
        a.str(w3, ptr(x0, OFF_EXIT));
        emitSyncSpToState(a);
        a.ret(x30);
        return true;
    }
    return false;
}
#endif

// Emit either real per-bytecode code OR the bail-to-interp stub
// (returns ExitSend immediately).  Writes the resulting bytes into
// `out` (caller buffer of `outCap` bytes); on success sets *outSize
// and *isReal (true if real codegen, false if bail stub).
//
// `bcOffsetBase` is the offset of bc[0] from the CompiledMethod
// object's address (i.e., 8 [object header] + 8*(1+numLiterals)).
// Per-bytecode bail emit uses `bcOffsetBase + i` so state.ip can be
// computed as `state.method.rawBits() + offset` at runtime — GC-safe.
//
// `bcToCodeOut` (if non-null, size bcLen+1) is filled with the per-
// bytecode emit start offset within the output buffer.  Slot bcLen
// holds the end-of-machine-code offset.  Used by the chain loop
// (codeOffsetForBC) to resume our JIT method at a specific bytecode
// after a callee completes.  Zero means "not a valid re-entry" —
// per the runtime contract (JITMethod.hpp::codeOffsetForBC).  For
// stub-only methods this is left as zeros except slot 0 = 0 and
// slot bcLen = emitted_size.
// `primIndex`: if > 0, emit the JIT prologue for this primitive at the
// start of code.  Caller guarantees bc[0..2] is the CallPrimitive
// bytecode for this primIndex and asks us to skip those 3 bytes from
// the fallback-emit pass.  bcToCode[0..2] stay 0 (not valid re-entry).
bool emitMethodBytes(const uint8_t* bc, size_t bcLen, uint64_t nilBits,
                     int bcOffsetBase, int primIndex,
                     int callerArgCount, int callerTempCount,
                     int staticJ2JArgCount,
                     uint8_t* out, size_t outCap,
                     size_t* outSize, bool* isReal,
                     uint32_t* bcToCodeOut,
                     SendSitePatch* patchRecords = nullptr,
                     uint32_t patchRecordCount = 0,
                     std::vector<std::pair<uint32_t,uint32_t>>* resumeOvOut
                         = nullptr) {
    using namespace asmjit;

    Environment env = Environment::host();
    CodeHolder code;
    Error err = code.init(env);
    T1EncodingErrorHandler encErr;
    code.set_error_handler(&encErr);
    // V2 resume overrides: (postSendBcOff, continuation label) —
    // applied to bcToCode after assembly (jumps keep the plain
    // bcLabels; only resume machinery lands on the continuation).
    std::vector<std::pair<uint32_t, asmjit::Label>> resumeOverrides;
    (void)resumeOverrides;
    // V2 self-identification anchor + overrides wiring (file-scope
    // statics: the send emit lives in emitOne_arm64).
    g_resumeOverridesPtr = &resumeOverrides;
    // PMS B1: per-site patch-word labels (resolved after flatten).
    std::vector<PatchSiteLabels> patchLabels;
    g_patchLabelsPtr = patchRecords ? &patchLabels : nullptr;
    // simStack B0: reset the TOS cache per compile + build the jump-
    // target bitmap.  Decode mirrors BcDepthMap.cpp's interp-faithful
    // rules (ExtJump operand is an UNSIGNED byte; the sign lives in
    // extB: offset = byte + (extB << 8)); the emit-time assert in the
    // jump emits catches any drift by failing the compile.
    g_tos = T1TosCache{};
    g_tosIn = T1TosCache{};
    std::vector<bool> tosJumpTargets(bcLen, false);
    {
        using namespace pharo::jit;
        int64_t extB = 0;
        bool extBSet = false;
        size_t pc = 0;
        while (pc < bcLen) {
            uint8_t jop = bc[pc];
            int len = SistaV1::bytecodeLength(jop);
            if (len <= 0) len = 1;
            int64_t target = -1;
            if (SistaV1::isShortJump(jop)
                    || SistaV1::isConditionalShortJump(jop)) {
                target = SistaV1::shortJumpTarget(jop, (int)pc);
            } else if (jop == SistaV1::ExtJump
                       || jop == SistaV1::ExtJumpTrue
                       || jop == SistaV1::ExtJumpFalse) {
                if (pc + 1 < bcLen) {
                    int64_t off = bc[pc + 1] + (extB << 8);
                    target = (int64_t)pc + 2 + off;
                }
            }
            if (jop == SistaV1::ExtendB && pc + 1 < bcLen) {
                uint8_t b = bc[pc + 1];
                extB = (!extBSet && b > 127) ? (int64_t)b - 256
                                             : extB * 256 + (int64_t)b;
                extBSet = true;
            } else if (jop != SistaV1::ExtendA) {
                extB = 0;
                extBSet = false;
            }
            if (target >= 0 && (size_t)target < bcLen)
                tosJumpTargets[(size_t)target] = true;
            pc += (size_t)len;
        }
    }
    g_tosJumpTargetsPtr = &tosJumpTargets;
    g_tosBc = bc;
    g_tosBcLen = bcLen;
    if (err != kErrorOk) return false;

    // PHARO_ASMJIT_T1_LOG=1 — dump asmjit asm to stderr per compile.
    // Heavy; only enable when actively debugging emit.
    static asmjit::FileLogger* asmjitLogger = []() -> asmjit::FileLogger* {
        if (pharo::g_debug.asmjitT1Log)
            return new asmjit::FileLogger(stderr);
        return nullptr;
    }();
    if (asmjitLogger) code.set_logger(asmjitLogger);

    // When emitting a prim prologue we skip the CallPrimitive bytes
    // (bc[0..2]) for the fallback emit — they're consumed by the
    // prologue.  The pre-scan + emit operate on bc + emitSkip.
    // Skip the CallPrimitive header whenever present — either the
    // prologue consumed it (primIndex > 0) or the prim-fallback-body
    // path compiles the body past it (primIndex < 0, no prologue).
    int emitSkip = (bcLen >= 3 && bc[0] == SistaV1::CallPrimitive) ? 3 : 0;
    const uint8_t* bcReal = bc + emitSkip;
    size_t bcRealLen = (bcLen >= (size_t)emitSkip) ? (bcLen - emitSkip) : 0;
    bool real = (bcRealLen > 0) && allBytecodesSupported(bcReal, bcRealLen);
    // simStack: per-compile master switch (stub emits never convert).
    g_useTos = GET_DEBUG_BOOL(PHARO_T1_TOS_REG) && real;
    // Sieve correctness gate (2026-05-19): methods with prim 60/61/62
    // declared at method entry AND conditional jumps in the body trigger
    // a still-unexplained interaction that returns wrong results for
    // `Integer>>benchmark` (sieve x3 returns 1 instead of 1028).
    // Bisection isolated the bug to the cond-jump emit in Array>>at:put:
    // (3rd cond-jump compile in sieve's compile order); skipping any one
    // of {1,2,3} cures the symptom.  Stubbing methods with these prims
    // is harmless: the send-site catch via primKind 14/15/16 in IC extras
    // still fires for the common case, and the prim prologue is the only
    // thing lost — <5% perf delta on bench per docs/deferred.md A6 iter
    // N+19.  Detect by scanning the BODY for conditional jumps; the
    // prologue has already consumed the CallPrimitive header at bc[0..2].
    // GATE OFF BY DEFAULT (2026-06-12, root cause FIXED): the
    // 2026-05-19 "sieve bug" was never a cond-jump miscompile — the
    // prim-60/61 prologues' format-range misses branched to `fail`
    // (= the Smalltalk fallback BODY), leaving their fmt-3/4/5/9
    // helper blocks DEAD CODE; uncovered formats (fmt 24-31
    // CompiledMethod etc.) entered the body, whose semantics assume a
    // real primitive attempt (Object>>at:'s body CANNOT retry — for
    // variable classes it unconditionally raises SubscriptOutOfBounds).
    // The gate's cond-jump predicate selected error-raising bodies by
    // accident.  Fixed by retargeting the range misses to the helper
    // blocks and routing remaining coverage misses through the FULL
    // primitive (jitPrimAtFull).  Acceptance: sieve x3 = 1028 (was 1),
    // gate-off ladder clean.  PHARO_T1_SIEVE_GATE=1 restores the stub
    // behavior for bisection.
    if (GET_DEBUG_BOOL(PHARO_T1_SIEVE_GATE)
            && real && (primIndex == 60 || primIndex == 61 || primIndex == 62)) {
        bool hasCJ = false;
        for (size_t bi = 0; bi < bcRealLen; bi++) {
            uint8_t op = bcReal[bi];
            if ((op >= SistaV1::ShortJumpTrueBase
                    && op <= SistaV1::ShortJumpFalseLast)
                || op == SistaV1::ExtJumpTrue
                || op == SistaV1::ExtJumpFalse) {
                hasCJ = true; break;
            }
        }
        if (hasCJ) real = false;  // fall through to stub-compile
    }
    // PHARO_T1_INLINE_J2J_DUMP_BC=1: dump bytecode for failed compiles
    if (!real && pharo::g_debug.t1InlineJ2JDumpBC) {
        static size_t dumpN = 0;
        if (dumpN < 30 && bcRealLen < 80) {
            dumpN++;
            std::string bcstr;
            for (size_t i = 0; i < bcRealLen; i++) {
                char buf[8];
                snprintf(buf, sizeof(buf), " %02x", bcReal[i]);
                bcstr += buf;
            }
            fprintf(stderr, "[T1-BC-DUMP #%zu] bcLen=%zu bc=%s\n",
                dumpN, bcRealLen, bcstr.c_str());
        }
    }
    // PHARO_ASMJIT_T1_STUB_ONLY=1: kill switch — force every method to
    // the bail-on-entry stub regardless of bytecode support.  Used to
    // bisect Phase 2 emit bugs against the known-good Phase 1 behavior.
    if (pharo::g_debug.asmjitT1StubOnly) real = false;
    // PHARO_ASMJIT_T1_HARDCODE_STUB=1: emit the stub by hardcoding the
    // bytes (mov dword [rdi+76], 2; ret).  Bypasses asmjit emit/copy
    // entirely so we can isolate whether the bug is in the asmjit
    // codegen path or in the integration plumbing.
    if (pharo::g_debug.asmjitT1HardcodeStub && !real) {
        static const uint8_t kStubBytes[8] = {
            0xC7, 0x47, 0x4C, 0x02, 0x00, 0x00, 0x00, 0xC3
        };
        if (outCap < 8) return false;
        std::memcpy(out, kStubBytes, 8);
        *outSize = 8;
        *isReal = false;
        return true;
    }

    // Per-bytecode labels — bound just before each bytecode's emit.
    // After flatten, label_offset_from_base gives the emit start of
    // each bytecode → fills bcToCodeOut for the chain loop's resume.
    // Labels are created by the Assembler (not CodeHolder); see below
    // in each per-arch block.
    std::vector<Label> bcLabels;

#if defined(__x86_64__) || defined(_M_X64)
    x86::Assembler a(&code);
    // Always allocate per-bytecode labels when emitting real code.
    // Conditional jumps need them even if bcToCodeOut is null (which
    // doesn't happen in practice but is API-safe).
    if (real) {
        bcLabels.reserve(bcLen);
        for (size_t i = 0; i < bcLen; i++) bcLabels.push_back(a.new_label());
    }
    // Emit prim prologue first (if any).  Fall-through enters the
    // fallback bytecode emit below.
    if (primIndex > 0) {
        emitPrimProlog_x86(a, primIndex);
    }
    if (real) {
        int siteIdx = 0;
        for (size_t i = 0; i < bcRealLen; i++) {
            int globalIdx = (int)i + emitSkip;
            a.bind(bcLabels[globalIdx]);
            // Diagnostic: track every emit's bcOffsetFromMethObj for the
            // sortStructs corruption hunt.
            if (__builtin_expect(pharo::g_debug.t1TraceEmit, 0)) {
                fprintf(stderr,
                    "[T1-EMIT] i=%zu globalIdx=%d op=0x%02x "
                    "bcOffsetFromMethObj=%d siteIdx=%d\n",
                    i, globalIdx, bcReal[i],
                    bcOffsetBase + globalIdx, siteIdx);
            }
            if (!emitOne_x86(a, bcReal[i], nilBits,
                             bcOffsetBase + globalIdx, siteIdx,
                             bcLabels, globalIdx,
                             callerArgCount, callerTempCount,
                             staticJ2JArgCount)) {
                std::fprintf(stderr,
                    "[asmjit-t1] BUG: prescan/emit disagree at bc[%d]=0x%02x\n",
                    globalIdx, bcReal[i]);
                return false;
            }
            if (isPhase4SendOp(bcReal[i])) siteIdx++;
        }
        if (bcRealLen == 0
                || bcReal[bcRealLen-1] < SistaV1::ReturnReceiver
                || bcReal[bcRealLen-1] > SistaV1::ReturnTop) {
            a.mov(x86::dword_ptr(x86::rdi, OFF_EXIT), Imm(EXIT_SEND));
            a.ret();
        }
    } else {
        a.mov(x86::dword_ptr(x86::rdi, OFF_EXIT), Imm(EXIT_SEND));
        a.ret();
    }
#elif defined(__aarch64__) || defined(_M_ARM64)
    a64::Assembler a(&code);
    // V2 self-identification anchor: code offset 0.
    g_codeStartLabel = a.new_label();
    a.bind(g_codeStartLabel);
    // PROLOGUE-LEAF (2026-06-12): supported-prim methods NEVER compile
    // their fallback body (Cog's design) — prologue + interp-resume
    // bail instead.  numIC=0 / no cond jumps / not stub-on-entry, so
    // they pass every inline-J2J gate (the dict-bench bail_numic/b46
    // classes were at:/at:put:'s cold bodies).  Correct because
    // prologue coverage misses run the FULL primitive (jitPrimAtFull)
    // — prologue failure IS prim failure.  Opt-out
    // PHARO_T1_NO_PROLOGUE_LEAF=1 restores body compiles.
    // Scope: at:-family only (60/61/62).  Their prologue failure is
    // genuinely rare (bounds/immutable), so the interp-resume bail is
    // cold.  Arithmetic prims (1-16) keep real bodies: their failure
    // paths (overflow -> LargeInteger, mixed-type coercion) are HOT,
    // and leafing them flickered the closure-as-receiver DNU.
    bool prologueLeaf = ((primIndex == 60 || primIndex == 61
                                || primIndex == 62)
                               || GET_DEBUG_BOOL(PHARO_T1_LEAF_ALL_PRIMS))
        && primIndex > 0
        && !GET_DEBUG_BOOL(PHARO_T1_NO_PROLOGUE_LEAF);
    // <primitive: N error: ec> hygiene: such methods have a store-
    // into-temp bytecode at body start (the error-code write that
    // activateMethod's prim-fail path performs).  The leaf bail
    // resumes the interp AT body start, which would execute that
    // store against an empty operand stack.  Refuse the leaf (the
    // real-body compile handles ec via the normal protocol).
    if (prologueLeaf && bcLen >= 4
            && (bc[3] == SistaV1::ExtStoreTemp
                || bc[3] == SistaV1::ExtPopStoreTemp
                || (bc[3] >= SistaV1::PopStoreTempBase
                    && bc[3] <= SistaV1::PopStoreTempLast)))
        prologueLeaf = false;
    if (prologueLeaf) real = false;
    if (real) {
        bcLabels.reserve(bcLen);
        for (size_t i = 0; i < bcLen; i++) bcLabels.push_back(a.new_label());
    }
    if (primIndex > 0) {
        // Prologue-success returns route through a shim that runs the
        // V2 J2J return prelude (pops this call's save / tail-jumps to
        // the caller's resume) — a plain ret leaked the save when
        // prim-prologue callees were admitted as inline-J2J targets
        // (2026-06-12).  The prologue's FAIL edge falls through past
        // the shim into the body emit.
        {
            asmjit::Label prologRet = a.new_label();
            asmjit::Label prologBodyStart = a.new_label();
            emitPrimProlog_arm64(a, primIndex, prologRet);
            a.b(prologBodyStart);          // prolog fail -> body
            a.bind(prologRet);             // x1 = retval (prolog contract)
            emitJ2JReturnPrelude_arm64(a);
            a.ret(asmjit::a64::x30);
            a.bind(prologBodyStart);
        }
    }
    if (real) {
        int siteIdx = 0;
        for (size_t i = 0; i < bcRealLen; i++) {
            int globalIdx = (int)i + emitSkip;
            uint8_t op = bcReal[i];
            a.bind(bcLabels[globalIdx]);

            // simStack §3/§5: jump targets are cold (bind-time rule),
            // then snapshot-then-clear for this bytecode's emit.
            if (g_tosJumpTargetsPtr && globalIdx >= 0
                    && (size_t)globalIdx < g_tosJumpTargetsPtr->size()
                    && (*g_tosJumpTargetsPtr)[globalIdx]) {
                g_tos = T1TosCache{};
            }
            g_tosIn = g_tos;
            g_tos = T1TosCache{};
            (void)g_tosIn;   // consumers arrive in B1+

            // InlinedPrimitive 0xEC: 2-byte no-op.  Bind operand label
            // and skip.
            if (op == SistaV1::InlinedPrimitive) {
                a.bind(bcLabels[globalIdx + 1]);
                i++;
                continue;
            }
            // PushInteger 0xE8: push SmI((int8_t)operand) onto sp.
            if (op == SistaV1::PushInteger) {
                int8_t imm = static_cast<int8_t>(bcReal[i + 1]);
                // SmI bits: (val << 3) | 1
                uint64_t smiBits = (static_cast<uint64_t>(
                    static_cast<int64_t>(imm)) << 3) | 1ULL;
                if (tosFam(kTosFamPushImm)) {
                    a.mov(a64::x26, asmjit::Imm(smiBits));
                    emitTosPush(a);
                    g_tos.constSmI = true;
                    g_tos.taggedBits = (int64_t)smiBits;
                } else {
                    a.mov(a64::x1, asmjit::Imm(smiBits));
                    emitLoadSp(a, a64::x2);
                    a.str(a64::x1, a64::ptr(a64::x2));
                    a.add(a64::x2, a64::x2, asmjit::Imm(8));
                    emitStoreSp(a, a64::x2);
                }
                a.bind(bcLabels[globalIdx + 1]);  // operand byte's label
                i++;
                continue;
            }
            // PushCharacter 0xE9: push Character((uint8_t)operand) onto sp.
            // Character bits: (codepoint << 3) | 3 (CharacterTag).
            if (op == SistaV1::PushCharacter) {
                uint8_t cp = bcReal[i + 1];
                uint64_t charBits = (static_cast<uint64_t>(cp) << 3) | 3ULL;
                if (tosFam(kTosFamPushImm)) {
                    a.mov(a64::x26, asmjit::Imm(charBits));
                    emitTosPush(a);
                } else {
                    a.mov(a64::x1, asmjit::Imm(charBits));
                    emitLoadSp(a, a64::x2);
                    a.str(a64::x1, a64::ptr(a64::x2));
                    a.add(a64::x2, a64::x2, asmjit::Imm(8));
                    emitStoreSp(a, a64::x2);
                }
                a.bind(bcLabels[globalIdx + 1]);
                i++;
                continue;
            }
            // PushArray 0xE7: 2-byte (opcode + desc).  Bail to
            // ExitArrayCreate so the chain loop allocates the Array
            // (and optionally pops `arraySize` elements into it).
            //
            // desc layout (per Sista V1):
            //   bits 0-6: arraySize (0..127)
            //   bit 7:    popIntoArray (if set, pop size elements)
            //
            // Chain loop's ExitArrayCreate handler reads desc from
            // state.cachedTarget and instructionPointer_ from state.ip,
            // then `instructionPointer_ += 2` advances past the
            // PushArray bytecode.
            if (op == SistaV1::PushArray) {
                uint8_t desc = bcReal[i + 1];
                a.mov(a64::x1, asmjit::Imm(static_cast<uint64_t>(desc)));
                a.str(a64::x1, a64::ptr(a64::x0, OFF_CACHED_TARGET));
                if (g_fsrLazy) {
                    // M4: method mirror may be stale (per-call store
                    // deleted) — derive the exit ip from x19's
                    // bcStartCache (== method + bcOffsetBase).
                    a.ldr(a64::x5, a64::ptr(a64::x19,
                          (int)offsetof(JITMethod, bcStartCache)));
                    a.add(a64::x5, a64::x5, asmjit::Imm(globalIdx));
                } else {
                a.ldr(a64::x5, a64::ptr(a64::x0, OFF_METHOD));
                a.add(a64::x5, a64::x5,
                      asmjit::Imm(bcOffsetBase + globalIdx));
                }
                a.str(a64::x5, a64::ptr(a64::x0, OFF_IP));
                a.mov(a64::w3, Imm(EXIT_ARRAY_CREATE));
                a.str(a64::w3, a64::ptr(a64::x0, OFF_EXIT));
                emitSyncSpToState(a);
                a.ret(a64::x30);
                a.bind(bcLabels[globalIdx + 1]);
                i += 1;
                continue;
            }
            // ExtA/ExtB prefix bundle.  Bail at the prefix; interp
            // executes the prefix + the prefixed bytecode (which
            // consumes extA_/extB_), then continues the rest of the
            // method in interp.  Bytes consumed = 2 + nextLen
            // (prefix opcode + data + next bytecode's full length).
            if (op == SistaV1::ExtendA || op == SistaV1::ExtendB) {
                uint8_t nextOp = bcReal[i + 2];
                int nextLen = SistaV1::bytecodeLength(nextOp);
                // NATIVE prefixed unconditional ExtJump (2026-06-11):
                // the generic prefix-bail below meant EVERY loop back
                // edge (to:do:/whileTrue compile to ExtendB+ExtJump
                // backward) dropped the WHOLE remaining activation to
                // the interpreter — iterative hot loops ran at interp
                // speed forever (~30ns/iter) while recursion JIT'd.
                // Emit the jump natively; back edges poll forceYield_
                // (heartbeat preemption: a sendless JIT loop must
                // still yield) and bail to the interp jump-execution
                // path when set.  PHARO_DET_SCHED keeps the old bail
                // (deterministic scheduling counts interp bytecodes —
                // a native loop would change the schedule).
                // Conditional prefixed jumps (rare) keep the bail.
                if (op == SistaV1::ExtendB
                        && nextOp == SistaV1::ExtJump
                        && !GET_DEBUG_BOOL(PHARO_T1_NO_NATIVE_BACKJUMP)
                        && !GET_DEBUG_BOOL(PHARO_DET_SCHED)
                        && g_t1ForceYieldAddr != nullptr
                        && i + 3 < bcRealLen) {
                    uint8_t eb = bcReal[i + 1];
                    int64_t extB = (eb > 127) ? (int64_t)eb - 256
                                              : (int64_t)eb;
                    int jumpIdx = globalIdx + 2;
                    int64_t off = (int64_t)bcReal[i + 3] + (extB << 8);
                    int64_t target = (int64_t)jumpIdx + 2 + off;
                    if (target >= 0 && (size_t)target < bcLabels.size()) {
                        // simStack: arrivals at the target are cold —
                        // it is bitmap-marked by the prescan; our own
                        // state after an unconditional jump is moot.
                        if (target <= (int64_t)globalIdx) {
                            // back edge: yield poll
                            asmjit::Label yieldBail = a.new_label();
                            a.mov(a64::x16,
                                  Imm((uint64_t)g_t1ForceYieldAddr));
                            a.ldrb(a64::w17, a64::ptr(a64::x16));
                            a.cbnz(a64::w17, yieldBail);
                            a.b(bcLabels[target]);
                            a.bind(yieldBail);
                            // interp executes the prefix+jump (and the
                            // rest of this activation; re-JITs on the
                            // next activation).
                            if (g_fsrLazy) {
                                // M4: method mirror may be stale (per-call store
                                // deleted) — derive the exit ip from x19's
                                // bcStartCache (== method + bcOffsetBase).
                                a.ldr(a64::x5, a64::ptr(a64::x19,
                                      (int)offsetof(JITMethod, bcStartCache)));
                                a.add(a64::x5, a64::x5, asmjit::Imm(globalIdx));
                            } else {
                            a.ldr(a64::x5, a64::ptr(a64::x0, OFF_METHOD));
                            a.add(a64::x5, a64::x5,
                                  asmjit::Imm(bcOffsetBase + globalIdx));
                            }
                            a.str(a64::x5, a64::ptr(a64::x0, OFF_IP));
                            a.mov(a64::w3, Imm(EXIT_ARITH_OVERFLOW));
                            a.str(a64::w3, a64::ptr(a64::x0, OFF_EXIT));
                            emitSyncSpToState(a);
                            a.ret(a64::x30);
                        } else {
                            a.b(bcLabels[target]);
                        }
                        for (int k = 1; k <= 3; k++) {
                            if ((size_t)(globalIdx + k) < bcLabels.size()) {
                                a.bind(bcLabels[globalIdx + k]);
                            }
                        }
                        i += 3;
                        continue;
                    }
                }
                if (g_fsrLazy) {
                    // M4: method mirror may be stale (per-call store
                    // deleted) — derive the exit ip from x19's
                    // bcStartCache (== method + bcOffsetBase).
                    a.ldr(a64::x5, a64::ptr(a64::x19,
                          (int)offsetof(JITMethod, bcStartCache)));
                    a.add(a64::x5, a64::x5, asmjit::Imm(globalIdx));
                } else {
                a.ldr(a64::x5, a64::ptr(a64::x0, OFF_METHOD));
                a.add(a64::x5, a64::x5,
                      asmjit::Imm(bcOffsetBase + globalIdx));
                }
                a.str(a64::x5, a64::ptr(a64::x0, OFF_IP));
                a.mov(a64::w3, Imm(EXIT_ARITH_OVERFLOW));
                a.str(a64::w3, a64::ptr(a64::x0, OFF_EXIT));
                emitSyncSpToState(a);
                a.ret(a64::x30);
                // Bind labels for all bytes consumed.  Loop's ++ skips
                // the prefix opcode; we explicitly skip the rest.
                for (int k = 1; k <= 1 + nextLen; k++) {
                    if ((size_t)(globalIdx + k) < bcLabels.size()) {
                        a.bind(bcLabels[globalIdx + k]);
                    }
                }
                i += 1 + nextLen;
                continue;
            }
            // ExtSend 0xEA / ExtSuperSend 0xEB: 2-byte send.  Naked
            // (no prefix — bundle case above handles ExtA/B+ExtSend).
            // Bail to interp; chain loop returns, interp dispatches
            // the send normally and continues the rest of the method.
            if (op == SistaV1::ExtSend || op == SistaV1::ExtSuperSend) {
                if (g_fsrLazy) {
                    // M4: method mirror may be stale (per-call store
                    // deleted) — derive the exit ip from x19's
                    // bcStartCache (== method + bcOffsetBase).
                    a.ldr(a64::x5, a64::ptr(a64::x19,
                          (int)offsetof(JITMethod, bcStartCache)));
                    a.add(a64::x5, a64::x5, asmjit::Imm(globalIdx));
                } else {
                a.ldr(a64::x5, a64::ptr(a64::x0, OFF_METHOD));
                a.add(a64::x5, a64::x5,
                      asmjit::Imm(bcOffsetBase + globalIdx));
                }
                a.str(a64::x5, a64::ptr(a64::x0, OFF_IP));
                a.mov(a64::w3, Imm(EXIT_ARITH_OVERFLOW));
                a.str(a64::w3, a64::ptr(a64::x0, OFF_EXIT));
                emitSyncSpToState(a);
                a.ret(a64::x30);
                a.bind(bcLabels[globalIdx + 1]);
                i += 1;
                continue;
            }
            // Remote temp access 0xFB/0xFC/0xFD: 3-byte ops.
            // Inline: vec = temps[vecIdx]; if vec is an Object,
            //   PushTempAtInVec:      push vec[tempIdx]
            //   StoreTempAtInVec:     vec[tempIdx] = stackTop (no pop)
            //   PopStoreTempAtInVec:  vec[tempIdx] = pop()
            // If vec is not an Object (nil), PushTempAtInVec pushes nil
            // and the stores are no-ops.  No GC write barrier yet —
            // stores from inside a closure body to a captured vector
            // typically hit eden; if surfacing as a leak, add the bit
            // 21 store barrier on the target object.
            if (op == SistaV1::PushTempAtInVec
                    || op == SistaV1::StoreTempAtInVec
                    || op == SistaV1::PopStoreTempAtInVec) {
                uint8_t tempIdx = bcReal[i + 1];
                uint8_t vecIdx  = bcReal[i + 2];
                // Counter increment.
                {
                    a.mov(a64::x14, asmjit::Imm((uint64_t)&g_bcRemoteTemp_hits));
                    a.ldr(a64::x15, a64::ptr(a64::x14));
                    a.add(a64::x15, a64::x15, asmjit::Imm(1));
                    a.str(a64::x15, a64::ptr(a64::x14));
                }
                if (op == SistaV1::PushTempAtInVec) {
                    asmjit::Label vecObj = a.new_label();
                    asmjit::Label pushDone = a.new_label();
                    emitLoadTempBase(a, a64::x4);
                    a.ldr(a64::x5, a64::ptr(a64::x4, vecIdx * 8));
                    a.tst(a64::x5, asmjit::Imm(0x7));
                    a.b_eq(vecObj);                   // tag==0 → real obj
                    // Not an object — push nil.
                    a.mov(a64::x6, asmjit::Imm(nilBits));
                    a.b(pushDone);
                    a.bind(vecObj);
                    // x5 = TempVector; slot tempIdx at offset 8 + tempIdx*8.
                    a.ldr(a64::x6, a64::ptr(a64::x5, 8 + tempIdx * 8));
                    a.bind(pushDone);
                    emitLoadSp(a, a64::x2);
                    a.str(a64::x6, a64::ptr(a64::x2));
                    a.add(a64::x2, a64::x2, asmjit::Imm(8));
                    emitStoreSp(a, a64::x2);
                } else {
                    // 0xFC (store, no pop) / 0xFD (pop+store)
                    asmjit::Label vecNotObj = a.new_label();
                    emitLoadSp(a, a64::x2);
                    a.ldr(a64::x6, a64::ptr(a64::x2, -8));  // x6 = stackTop
                    if (op == SistaV1::PopStoreTempAtInVec) {
                        a.sub(a64::x2, a64::x2, asmjit::Imm(8));
                        emitStoreSp(a, a64::x2);
                    }
                    emitLoadTempBase(a, a64::x4);
                    a.ldr(a64::x5, a64::ptr(a64::x4, vecIdx * 8));
                    a.tst(a64::x5, asmjit::Imm(0x7));
                    a.b_ne(vecNotObj);                // not Object → skip store
                    a.str(a64::x6, a64::ptr(a64::x5, 8 + tempIdx * 8));
                    a.bind(vecNotObj);
                }
                a.bind(bcLabels[globalIdx + 1]);
                a.bind(bcLabels[globalIdx + 2]);
                i += 2;
                continue;
            }
            // PushFullBlock 0xF9: 3-byte (opcode + LL + HH).  Bail to
            // ExitBlockCreate so the chain loop allocates the
            // FullBlockClosure.  state.cachedTarget = (LL | HH << 32)
            // matches JITState.hpp:160's packed format.
            if (op == SistaV1::PushFullBlock) {
                uint8_t litIdx = bcReal[i + 1];
                uint8_t flags  = bcReal[i + 2];
                uint64_t packed = static_cast<uint64_t>(litIdx)
                                | (static_cast<uint64_t>(flags) << 32);
                // Set state.ip first — both paths need it (the C++
                // create reads it for the materialize pc and GC ip
                // round-trip).
                if (g_fsrLazy) {
                    // M4: method mirror may be stale (per-call store
                    // deleted) — derive the exit ip from x19's
                    // bcStartCache (== method + bcOffsetBase).
                    a.ldr(a64::x5, a64::ptr(a64::x19,
                          (int)offsetof(JITMethod, bcStartCache)));
                    a.add(a64::x5, a64::x5, asmjit::Imm(globalIdx));
                } else {
                a.ldr(a64::x5, a64::ptr(a64::x0, OFF_METHOD));
                a.add(a64::x5, a64::x5,
                      asmjit::Imm(bcOffsetBase + globalIdx));
                }
                a.str(a64::x5, a64::ptr(a64::x0, OFF_IP));
                asmjit::Label bcrSlow = a.new_label();
                asmjit::Label bcrDone = a.new_label();
                const int ibcMax = GET_DEBUG_INT(PHARO_T1_INLINE_BLOCK_CREATE_MAX);
                // OPT-IN ONLY (2026-06-11): the inline helper is UNSOUND
                // whenever this method runs as a JIT-to-JIT callee —
                // the caller's activation exists only in machine state
                // (chain inline-activate, inline/saveless J2J) and is
                // reified by the EXIT handlers ("push a SavedFrame for
                // the caller") before C++ creates the closure.  An
                // in-JIT create sees a frame model missing the caller
                // -> the closure's outerContext sender chain skips it
                // (emit-bisect culprit #3 = Dictionary>>at:, garbage
                // #value: DNUs at startup).  The sound form of this
                // lever is LAZY outerContext (married contexts) — see
                // WIP checkpoint tt.
                const bool inlineBlockCreate =
                    GET_DEBUG_BOOL(PHARO_T1_INLINE_BLOCK_CREATE)
                    && (ibcMax < 0 || g_ibcEmits < ibcMax);
                if (inlineBlockCreate) g_ibcEmits++;
                if (inlineBlockCreate) {
                    // INLINE block create (2026-06-11, the dict-bench
                    // per-at: lever): call jit_rt_block_create via blr
                    // — same closure semantics (C++ create incl.
                    // materialization), no exit/resume round trip.
                    // Guard: only at j2jDepth == 0 — the exit path
                    // materializes pending J2J saves before creating
                    // (the closure's outerContext must see them); the
                    // helper cannot.
                    a.ldr(a64::w6, a64::ptr(a64::x0, OFF_J2J_DEPTH));
                    a.cbnz(a64::w6, bcrSlow);
                    a.sub(a64::sp, a64::sp, asmjit::Imm(16));
                    a.str(a64::x0,  a64::ptr(a64::sp, 0));
                    a.str(a64::x30, a64::ptr(a64::sp, 8));
                    a.mov(a64::x1, asmjit::Imm(packed));
                    emitSyncSpToState(a);
                    a.mov(a64::x9, asmjit::Imm(
                        (uint64_t)&jit_rt_block_create));
                    a.blr(a64::x9);
                    a.ldr(a64::x0,  a64::ptr(a64::sp, 0));
                    a.ldr(a64::x30, a64::ptr(a64::sp, 8));
                    a.add(a64::sp, a64::sp, asmjit::Imm(16));
                    emitReloadSpFromState(a);
                    a.b(bcrDone);
                }
                a.bind(bcrSlow);
                a.mov(a64::x1, asmjit::Imm(packed));
                a.str(a64::x1, a64::ptr(a64::x0, OFF_CACHED_TARGET));
                a.mov(a64::w3, Imm(EXIT_BLOCK_CREATE));
                a.str(a64::w3, a64::ptr(a64::x0, OFF_EXIT));
                emitSyncSpToState(a);
                a.ret(a64::x30);
                a.bind(bcrDone);
                a.bind(bcLabels[globalIdx + 1]);
                a.bind(bcLabels[globalIdx + 2]);
                i += 2;
                continue;
            }
            // Extended push/store with 1-byte index operand.
            //   ExtPushRecvVar 0xE2:   push recv.slot[index]
            //   ExtPushLitVar 0xE3:    push literals[index].slot[1] (assoc val)
            //   ExtPushLitConst 0xE4:  push literals[index]
            //   ExtPushTemp 0xE5:      push tempBase[index]
            //   ExtPopStoreRecv 0xF0:  pop, store recv.slot[index] (immut check)
            //   ExtPopStoreTemp 0xF2:  pop, store tempBase[index]
            //   ExtStoreRecv 0xF3:     store TOS to recv.slot[index] (immut check)
            //   ExtStoreTemp 0xF5:     store TOS (no pop) to tempBase[index]
            // ExtA/ExtB prefixes not yet supported — index is just the
            // single operand byte, so this covers index 0-255.
            if (op == SistaV1::ExtPushRecvVar
                    || op == SistaV1::ExtPushLitVar
                    || op == SistaV1::ExtPushLitConst
                    || op == SistaV1::ExtPushTemp
                    || op == SistaV1::ExtPopStoreTemp
                    || op == SistaV1::ExtStoreTemp
                    || op == SistaV1::ExtPopStoreRecv
                    || op == SistaV1::ExtStoreRecv
                    || op == SistaV1::ExtPopStoreLitVar
                    || op == SistaV1::ExtStoreLitVar) {
                int idx = bcReal[i + 1];
                if (op == SistaV1::ExtPushRecvVar) {
                    a.ldr(a64::x1, a64::ptr(a64::x0, OFF_RECEIVER));
                    a.ldr(a64::x1, a64::ptr(a64::x1, OBJ_SLOT_0 + idx * 8));
                    emitShadowReadVerify(a, idx);
                    emitLoadSp(a, a64::x2);
                    a.str(a64::x1, a64::ptr(a64::x2));
                    a.add(a64::x2, a64::x2, asmjit::Imm(8));
                    emitStoreSp(a, a64::x2);
                } else if (op == SistaV1::ExtPushLitConst) {
                    a.ldr(a64::x1, a64::ptr(a64::x0, OFF_LITERALS));
                    a.ldr(a64::x1, a64::ptr(a64::x1, idx * 8));
                    emitLoadSp(a, a64::x2);
                    a.str(a64::x1, a64::ptr(a64::x2));
                    a.add(a64::x2, a64::x2, asmjit::Imm(8));
                    emitStoreSp(a, a64::x2);
                } else if (op == SistaV1::ExtPushLitVar) {
                    a.ldr(a64::x1, a64::ptr(a64::x0, OFF_LITERALS));
                    a.ldr(a64::x1, a64::ptr(a64::x1, idx * 8));
                    a.ldr(a64::x1, a64::ptr(a64::x1, OBJ_SLOT_0 + 8));
                    emitLoadSp(a, a64::x2);
                    a.str(a64::x1, a64::ptr(a64::x2));
                    a.add(a64::x2, a64::x2, asmjit::Imm(8));
                    emitStoreSp(a, a64::x2);
                } else if (op == SistaV1::ExtPushTemp) {
                    emitLoadTempBase(a, a64::x1);
                    a.ldr(a64::x1, a64::ptr(a64::x1, idx * 8));
                    emitLoadSp(a, a64::x2);
                    a.str(a64::x1, a64::ptr(a64::x2));
                    a.add(a64::x2, a64::x2, asmjit::Imm(8));
                    emitStoreSp(a, a64::x2);
                } else if (op == SistaV1::ExtPopStoreTemp) {
                    emitLoadSp(a, a64::x2);
                    a.sub(a64::x2, a64::x2, asmjit::Imm(8));
                    emitStoreSp(a, a64::x2);
                    a.ldr(a64::x1, a64::ptr(a64::x2));
                    emitLoadTempBase(a, a64::x4);
                    a.str(a64::x1, a64::ptr(a64::x4, idx * 8));
                } else if (op == SistaV1::ExtStoreTemp) {
                    emitLoadSp(a, a64::x2);
                    a.ldur(a64::x1, asmjit::a64::ptr(a64::x2, -8));
                    emitLoadTempBase(a, a64::x4);
                    a.str(a64::x1, a64::ptr(a64::x4, idx * 8));
                } else if (op == SistaV1::ExtPopStoreLitVar
                        || op == SistaV1::ExtStoreLitVar) {
                    // Literal var (Association) store.  literals[idx]
                    // is an Association; its slot 1 is the .value
                    // slot.  No write barrier — YG scavenge scans
                    // all of old space.
                    if (op == SistaV1::ExtPopStoreLitVar) {
                        emitLoadSp(a, a64::x2);
                        a.sub(a64::x2, a64::x2, asmjit::Imm(8));
                        emitStoreSp(a, a64::x2);
                        a.ldr(a64::x1, a64::ptr(a64::x2));
                    } else /* ExtStoreLitVar */ {
                        emitLoadSp(a, a64::x2);
                        a.ldur(a64::x1, asmjit::a64::ptr(a64::x2, -8));
                    }
                    a.ldr(a64::x4, a64::ptr(a64::x0, OFF_LITERALS));
                    a.ldr(a64::x4, a64::ptr(a64::x4, idx * 8));
                    a.str(a64::x1, a64::ptr(a64::x4, OBJ_SLOT_0 + 8));
                } else if (op == SistaV1::ExtPopStoreRecv
                        || op == SistaV1::ExtStoreRecv) {
                    // Receiver store with immutable-bit check (mirror
                    // popStoreRecv 0xC8-CF emit at line 1545).  If the
                    // receiver's header bit 23 is set, bail to interp.
                    asmjit::Label bail = a.new_label();
                    asmjit::Label end  = a.new_label();
                    a.ldr(a64::x4, a64::ptr(a64::x0, OFF_RECEIVER));
                    a.ldr(a64::w5, a64::ptr(a64::x4));
                    a.tst(a64::w5, asmjit::Imm(0x800000));
                    a.b_ne(bail);
                    if (op == SistaV1::ExtPopStoreRecv) {
                        emitLoadSp(a, a64::x2);
                        a.sub(a64::x2, a64::x2, asmjit::Imm(8));
                        emitStoreSp(a, a64::x2);
                        a.ldr(a64::x1, a64::ptr(a64::x2));
                    } else /* ExtStoreRecv */ {
                        emitLoadSp(a, a64::x2);
                        a.ldur(a64::x1, asmjit::a64::ptr(a64::x2, -8));
                    }
                    a.str(a64::x1, a64::ptr(a64::x4, OBJ_SLOT_0 + idx * 8));
                    emitStoreRingLog(a, idx);
                    a.b(end);
                    a.bind(bail);
                    if (g_fsrLazy) {
                        // M4: method mirror may be stale (per-call store
                        // deleted) — derive the exit ip from x19's
                        // bcStartCache (== method + bcOffsetBase).
                        a.ldr(a64::x5, a64::ptr(a64::x19,
                              (int)offsetof(JITMethod, bcStartCache)));
                        a.add(a64::x5, a64::x5, asmjit::Imm(globalIdx));
                    } else {
                    a.ldr(a64::x5, a64::ptr(a64::x0, OFF_METHOD));
                    a.add(a64::x5, a64::x5,
                          asmjit::Imm(bcOffsetBase + globalIdx));
                    }
                    a.str(a64::x5, a64::ptr(a64::x0, OFF_IP));
                    a.mov(a64::w3, Imm(EXIT_ARITH_OVERFLOW));
                    a.str(a64::w3, a64::ptr(a64::x0, OFF_EXIT));
                    emitSyncSpToState(a);
                    a.ret(a64::x30);
                    a.bind(end);
                }
                a.bind(bcLabels[globalIdx + 1]);
                i++;
                continue;
            }
            // Long jumps 0xED/0xEE/0xEF (NAKED — the prefix bundle
            // handler above bails ExtendB+ExtJump to the interp).
            // Per the interp/spec the operand byte is UNSIGNED with the
            // sign carried by extB; a naked long jump is always forward
            // 0-255.  The old int8 read miscompiled operands >= 128
            // (forward 128-255 byte jumps) into a branch to a wrong
            // EARLIER label — wrong-position execution that is locally
            // depth-consistent.  Target = (globalIdx + 2) + offset.
            if (op == SistaV1::ExtJump
                    || op == SistaV1::ExtJumpTrue
                    || op == SistaV1::ExtJumpFalse) {
                int offset = static_cast<int>(bcReal[i + 1]);
                int target = globalIdx + 2 + offset;
                // Out-of-range target = unreachable trailer byte; emit a bail.
                if (target < 0 || target >= (int)bcLabels.size()) {
                    if (g_fsrLazy) {
                        // M4: method mirror may be stale (per-call store
                        // deleted) — derive the exit ip from x19's
                        // bcStartCache (== method + bcOffsetBase).
                        a.ldr(a64::x5, a64::ptr(a64::x19,
                              (int)offsetof(JITMethod, bcStartCache)));
                        a.add(a64::x5, a64::x5, asmjit::Imm(globalIdx));
                    } else {
                    a.ldr(a64::x5, a64::ptr(a64::x0, OFF_METHOD));
                    a.add(a64::x5, a64::x5,
                          asmjit::Imm(bcOffsetBase + globalIdx));
                    }
                    a.str(a64::x5, a64::ptr(a64::x0, OFF_IP));
                    a.mov(a64::w3, Imm(EXIT_SEND));
                    a.str(a64::w3, a64::ptr(a64::x0, OFF_EXIT));
                    emitSyncSpToState(a);
                    a.ret(a64::x30);
                    a.bind(bcLabels[globalIdx + 1]);
                    i++;
                    continue;
                }
                if (op == SistaV1::ExtJump) {
                    a.b(bcLabels[target]);
                } else {
                    // Conditional long jump: same structure as the
                    // short conditional jump emit (~line 1777) but
                    // with target from the operand byte.
                    bool jumpOnTrue = (op == SistaV1::ExtJumpTrue);
                    asmjit::Label mustBoolBail = a.new_label();
                    asmjit::Label takeBranch   = a.new_label();
                    asmjit::Label fallThrough  = a.new_label();
                    emitLoadSp(a, a64::x2);
                    a.ldur(a64::x1, asmjit::a64::ptr(a64::x2, -8));
                    a.ldp(a64::x4, a64::x5,
                          a64::ptr(a64::x0, OFF_TRUEOOP));
                    a.cmp(a64::x1, a64::x4);
                    a.b_eq(jumpOnTrue ? takeBranch : fallThrough);
                    a.cmp(a64::x1, a64::x5);
                    a.b_ne(mustBoolBail);
                    a.b(jumpOnTrue ? fallThrough : takeBranch);
                    a.bind(takeBranch);
                    a.sub(a64::x2, a64::x2, asmjit::Imm(8));
                    emitStoreSp(a, a64::x2);
                    a.b(bcLabels[target]);
                    a.bind(fallThrough);
                    a.sub(a64::x2, a64::x2, asmjit::Imm(8));
                    emitStoreSp(a, a64::x2);
                    a.b(bcLabels[globalIdx + 2]);  // next bytecode
                    a.bind(mustBoolBail);
                    if (g_fsrLazy) {
                        // M4: method mirror may be stale (per-call store
                        // deleted) — derive the exit ip from x19's
                        // bcStartCache (== method + bcOffsetBase).
                        a.ldr(a64::x5, a64::ptr(a64::x19,
                              (int)offsetof(JITMethod, bcStartCache)));
                        a.add(a64::x5, a64::x5, asmjit::Imm(globalIdx));
                    } else {
                    a.ldr(a64::x5, a64::ptr(a64::x0, OFF_METHOD));
                    a.add(a64::x5, a64::x5,
                          asmjit::Imm(bcOffsetBase + globalIdx));
                    }
                    a.str(a64::x5, a64::ptr(a64::x0, OFF_IP));
                    a.mov(a64::w3, Imm(EXIT_MUST_BOOL));
                    a.str(a64::w3, a64::ptr(a64::x0, OFF_EXIT));
                    emitSyncSpToState(a);
                    a.ret(a64::x30);
                }
                // Bind operand byte's label to current PC so any
                // jump targeting it doesn't dangle at finalize.
                a.bind(bcLabels[globalIdx + 1]);
                i++;  // skip operand byte (loop's i++ advances past it)
                continue;
            }

            if (!emitOne_arm64(a, op, nilBits,
                                bcOffsetBase + globalIdx, siteIdx,
                                bcLabels, globalIdx,
                                callerArgCount, callerTempCount,
                                staticJ2JArgCount)) {
                std::fprintf(stderr,
                    "[asmjit-t1] BUG: prescan/emit disagree at bc[%d]=0x%02x\n",
                    globalIdx, op);
                return false;
            }
            if (isPhase4SendOp(op)) siteIdx++;
        }
        if (bcRealLen == 0
                || bcReal[bcRealLen-1] < SistaV1::ReturnReceiver
                || bcReal[bcRealLen-1] > SistaV1::ReturnTop) {
            a.mov(a64::w1, Imm(EXIT_SEND));
            a.str(a64::w1, a64::ptr(a64::x0, OFF_EXIT));
            emitSyncSpToState(a);
            a.ret(a64::x30);
        }
    } else if (prologueLeaf) {
        // PROLOGUE-LEAF (2026-06-12, Cog's design for prim methods):
        // the prologue above IS the method for the hot path; on
        // prologue failure (a GENUINE prim failure — coverage misses
        // run the full prim via jitPrimAtFull), resume the INTERPRETER
        // at the first body bytecode in this same activation (the
        // proven PushThisContext bail protocol: EXIT_ARITH_OVERFLOW +
        // ip).  vs the old bail-on-entry stub: numIC=0, no cond
        // jumps, NOT isStubOnEntry -> passes every inline-J2J gate, so
        // at:/at:put:/basicAt: become direct J2J callees.
        g_emitPrologueLeaf = true;
        if (g_fsrLazy) {
            a.ldr(a64::x5, a64::ptr(a64::x19,
                  (int)offsetof(JITMethod, bcStartCache)));
            a.add(a64::x5, a64::x5, asmjit::Imm(emitSkip));
        } else {
            a.ldr(a64::x5, a64::ptr(a64::x0, OFF_METHOD));
            a.add(a64::x5, a64::x5,
                  asmjit::Imm(bcOffsetBase + emitSkip));
        }
        a.str(a64::x5, a64::ptr(a64::x0, OFF_IP));
        a.mov(a64::w3, Imm(EXIT_ARITH_OVERFLOW));
        a.str(a64::w3, a64::ptr(a64::x0, OFF_EXIT));
        emitSyncSpToState(a);
        a.ret(a64::x30);
    } else {
        a.mov(a64::w1, Imm(EXIT_SEND));
        a.str(a64::w1, a64::ptr(a64::x0, OFF_EXIT));
        emitSyncSpToState(a);
        a.ret(a64::x30);
    }
#endif

    if (encErr.failed) return false;  // an instruction was silently dropped
    err = code.flatten();
    if (err != kErrorOk) return false;

    // Fill bcToCodeOut from bound labels.  bcToCodeOut[bcLen] is the
    // end-of-machine-code offset (used by findMethodByPC etc.).
    // Per JITMethod.hpp contract: bcToCode[i]==0 means "not a valid
    // re-entry point"; bcToCode[0] is conventionally 0 (initial entry
    // goes through codeStart() directly).
    //
    // PHARO_ASMJIT_T1_BCTOCODE_ZERO=1: write all zeros except [bcLen]
    // (the end-of-mc sentinel).  Chain loop's resume check
    // `if (codeOff == 0 || codeOff >= codeSize) bail;` then always
    // bails to interp — effectively disabling JIT-side resume while
    // still advertising numBytecodes.  Bisect helper.
    if (bcToCodeOut) {
        if (real && !pharo::g_debug.asmjitT1BctocodeZero) {
            for (size_t i = 0; i < bcLen; i++) {
                // The first `emitSkip` bytecodes are the CallPrimitive header
                // (0xF8 lo hi) — the emit loop starts at bcReal (offset
                // emitSkip), so bcLabels[0..emitSkip) are NEVER bound.  Calling
                // label_offset_from_base on an unbound label returns GARBAGE in
                // the NDEBUG build (asserts is_bound() in Debug) — that garbage
                // became a bogus bcToCode re-entry offset and, when the VM
                // resumed into such a method, executed wrong code (observed as
                // an inline-getter leaking a frame pointer onto the operand
                // stack -> garbage-receiver DNUs in AI-Algorithms-Graph).  The
                // CallPrimitive header is not a valid re-entry point, so map it
                // to 0 like slot 0.  (2026-05-28 root-cause fix.)
                if (i < (size_t)emitSkip) { bcToCodeOut[i] = 0u; continue; }
                uint32_t off = (uint32_t)code.label_offset_from_base(bcLabels[i]);
                // Per contract, slot 0 is conventionally 0 (initial entry
                // goes through codeStart() directly).
                bcToCodeOut[i] = (i == 0) ? 0u : off;
            }
            // V2: resume machinery (codeOffsetForBC consumers) lands on
            // the post-send continuation, not the next bytecode's label
            // (which forward jumps keep using).
            // Send-resume fix (2026-06-11): bcToCode stays PLAIN.
            // The continuation offsets go to a side table consumed
            // ONLY by retval-carrying resume sites
            // (JITMethod::codeOffsetForResume) — writing them into
            // bcToCode poisoned every non-retval resume entry
            // (tryResume/interp re-entry executed the continuation's
            // arg-pop + x1 store with no retval => one-slot-shifted
            // operand stack, the mustBeBoolean/only-idle wedge).
            if (resumeOvOut) {
                for (auto& ov : resumeOverrides) {
                    if (ov.first < bcLen) {
                        resumeOvOut->emplace_back(ov.first, (uint32_t)
                            code.label_offset_from_base(ov.second));
                    }
                }
            }
        } else {
            // Stub-only OR zeroBcToCode: no per-bytecode entry points.
            for (size_t i = 0; i < bcLen; i++) bcToCodeOut[i] = 0;
        }
        // bcToCodeOut[bcLen] is set by the caller to the emitted size.
    }
    // PMS B1: resolve patch-word labels to final code offsets.
    if (patchRecords) {
        for (auto& pl : patchLabels) {
            if (pl.siteIdx >= patchRecordCount) continue;
            patchRecords[pl.siteIdx].keyMovzOffset =
                (uint32_t)code.label_offset_from_base(pl.keyMovz);
            if (pl.hasTail) {
                patchRecords[pl.siteIdx].tailOffset =
                    (uint32_t)code.label_offset_from_base(pl.tail);
                patchRecords[pl.siteIdx].tailBranchOffset =
                    (uint32_t)code.label_offset_from_base(pl.tailBranch);
            }
            // tailOffset==0 (packing-gate fail / stub) = head patchable
            // but never linkable (linkSendSite refuses).
        }
    }
    g_patchLabelsPtr = nullptr;
    g_tosJumpTargetsPtr = nullptr;
    g_tosBc = nullptr;
    g_tosBcLen = 0;

    size_t total = code.code_size();
    if (total == 0 || total > outCap) {
        std::fprintf(stderr,
                     "[asmjit-t1] code.code_size=%zu out of [1, %zu]\n",
                     total, outCap);
        return false;
    }
    err = code.copy_flattened_data(out, outCap, CopySectionFlags::kPadSectionBuffer);
    if (err != kErrorOk) return false;
    *outSize = total;
    *isReal = real;
    return true;
}

}  // namespace

// jit-may23 T6: per-reason compile-failure counters (external
// linkage so JITRuntime can read them via extern decl).
size_t g_failedBadHeader  = 0;
size_t g_failedUnsuppPrim = 0;
size_t g_failedSkipSel    = 0;
size_t g_failedBlock      = 0;
size_t g_failedBcOther    = 0;

// Monotonic compile counter — used by the PHARO_T1_SAVELESS_MIN_COMPILE
// bisect gate (saveless emit only in compiles with seq >= N, so startup
// methods can be excluded while a controlled late-compiled site fires).
extern "C" int g_ibcEmits = 0;
// PROLOGUE-LEAF mode (2026-06-12): set per compile when a supported-
// prim method's body is unsupported/scary and the emit produced
// prologue + interp-resume bail instead of a bail-on-entry stub.
// Consumed at JM finalize: such methods are NOT isStubOnEntry (the
// prologue is real, so they pass the J2J gates), have numIC=0 and
// canBailMidMethod=false by construction.
extern "C" bool g_emitPrologueLeaf = false;
extern "C" uint64_t g_t1CompileSeq2 = 0;

JITMethod* compileViaAsmjit(CodeZone& zone, MethodMap& methodMap,
                             ObjectMemory& memory, Interpreter& interp,
                             Oop compiledMethod) {
    (void)interp;
    g_t1CompileSeq2++;
    g_emitPrologueLeaf = false;
    const int ibcBefore = g_ibcEmits;
    struct IbcLogGuard {
        const int before; pharo::Oop cm; pharo::ObjectMemory& mem;
        ~IbcLogGuard() {
            if (pharo::g_debug.jitFailReasons && g_ibcEmits != before)
                fprintf(stderr, "[IBC-RANGE] %d..%d sel=#%s\n",
                    before + 1, g_ibcEmits, mem.selectorOf(cm).c_str());
        }
    } ibcGuard{ibcBefore, compiledMethod, memory};

    if (!compiledMethod.isObject() || compiledMethod.rawBits() < 0x10000) {
        g_failed++; g_failedBadHeader++;
        return nullptr;
    }
    ObjectHeader* methObj = compiledMethod.asObjectPtr();
    Oop headerOop = methObj->slotAt(0);
    if (!headerOop.isSmallInteger()) {
        g_failed++; g_failedBadHeader++;
        return nullptr;
    }
    int64_t headerBits = headerOop.asSmallInteger();
    int  numLiterals = static_cast<int>(headerBits & 0x7FFF);
    bool hasPrimitive = (headerBits >> 16) & 1;

    uint8_t* bytes = methObj->bytes();
    size_t bcStart = (1 + numLiterals) * 8;
    size_t totalBytes = methObj->slotCount() * 8;
    uint8_t fmt = static_cast<uint8_t>(methObj->format());
    int unusedBytes = (fmt >= 24) ? (fmt - 24) : 0;
    if (bcStart + (size_t)unusedBytes >= totalBytes) {
        g_failed++; g_failedBadHeader++;
        return nullptr;
    }
    size_t bcLenRaw = totalBytes - bcStart - (size_t)unusedBytes;
    const uint8_t* bc = bytes + bcStart;

    // Detect a supported primitive prologue.  If hasPrimitive is set
    // but the primitive isn't in our supported set (or the first 3
    // bytes aren't CallPrimitive), bail — we'd otherwise route through
    // a bail-stub which doesn't try the prim, and the chain loop's
    // inline-activation gate (Interpreter.cpp:18731-18732) blocks
    // hasPrimPrologue=false callees with primitives anyway.  Bail to
    // C++ so the prim runs via the standard activateMethod path.
    int primIdx = -1;
    if (hasPrimitive) {
        primIdx = supportedPrimIndex(bc, bcLenRaw);
        if (primIdx < 0 && !GET_DEBUG_BOOL(PHARO_T1_NO_PRIM_FALLBACK_BODY)) {
            // Cog-style PRIM-FALLBACK BODY (2026-06-11): compile the
            // bytecode body even though the prim has no prologue.  The
            // prim itself always runs in C++ first — activateMethod's
            // tryJITActivation call sits AFTER the primitive attempt,
            // and every direct-call path (chain inline-activate,
            // convert_send, upgradeICToJ2J bit-60) gates on
            // hasPrimPrologue (false here, primIdx < 0).  What the
            // body buys: post-prim-fail activations run jitted, and
            // mid-method resumes get a bcToCode table — unsupported-
            // prim methods were 53% of dict-bench tryResume refusals
            // (scanFor:, at:put:, findElementOrNil:, fullCheck ran
            // interp-to-return on every C++ bail).
            // primIdx stays -1: no prologue; emitSkip derives from the
            // CallPrimitive header below.
        } else if (primIdx < 0) {
            // Unsupported prim: bail compile, let C++ handle it.
            g_failed++; g_failedUnsuppPrim++;
            if (pharo::g_debug.jitFailReasons) {
                static int upLog = 0;
                if (++upLog <= 200) {
                    int rawPrim = (bcLenRaw >= 3
                                   && bc[0] == SistaV1::CallPrimitive)
                        ? (bc[1] | ((bc[2] & 0x1F) << 8)) : -1;
                    fprintf(stderr, "[UNSUPP-PRIM] prim=%d sel=#%s\n",
                        rawPrim,
                        memory.selectorOf(compiledMethod).c_str());
                }
            }
            return nullptr;
        }
    }
    // Hardcoded skip for known-broken Float methods.
    // Float>>cos JIT compilation SIGSEGVs at offset 336 in the JIT'd
    // code under repeated `i degreesToRadians cos` calls (the JIT
    // dispatch reads garbage as a heap pointer with tag 0 — looks
    // like the `+` inline emit's SmallFloat encode occasionally
    // outputs un-tagged double bits).  IntegerTest>>testDegreeCos
    // crashes deterministically.  Disable JIT for #cos until the
    // root cause is found.  Other Float trig methods (sin, tan)
    // appear to work — only cos has the issue.
    {
        std::string sel = memory.selectorOf(compiledMethod);
        if (sel == "cos") {
            g_failed++; g_failedSkipSel++;
            return nullptr;
        }
    }
    // g_debug.t1SkipSelectors: comma-separated selector list to reject
    // from real-emit (compile as stub-on-entry).  Unlike index-based
    // bisects, selectors are stable across runs even when compile
    // order is non-deterministic.  Set via PHARO_T1_SKIP_SELECTORS=
    // "addTemp:,methodClass,validate" etc.
    if (g_debug.t1SkipSelectors) {
        std::string sel = memory.selectorOf(compiledMethod);
        const char* list = g_debug.t1SkipSelectors;
        size_t selLen = sel.size();
        for (const char* p = list; *p; ) {
            const char* end = std::strchr(p, ',');
            size_t len = end ? (size_t)(end - p) : std::strlen(p);
            if (len == selLen && std::memcmp(p, sel.c_str(), len) == 0) {
                g_failed++; g_failedSkipSel++;
                return nullptr;
            }
            p = end ? end + 1 : p + len;
        }
    }
    // Block JIT-compile bisect knobs.
    //   PHARO_T1_NO_BLOCKS=1            — reject every CompiledBlock.
    //   PHARO_T1_BLOCKS_FIRST_N=K       — accept the first K block
    //                                     compiles, reject the rest.
    //   PHARO_T1_BLOCKS_ONLY_N=K        — accept only block index K
    //                                     (1-based; 0 = none).
    //   PHARO_T1_BLOCKS_SKIP_FROM=A
    //   PHARO_T1_BLOCKS_SKIP_TO=B       — reject blocks in range [A,B]
    //                                     inclusive (combine with FIRST_N
    //                                     to bisect partner blocks).
    //   PHARO_T1_BLOCKS_TRACE=1         — log every block compile with
    //                                     bcLen + first 16 bytecodes.
    // Per-compile block flag for the emit's NLR gate (0x58-0x5C inside
    // a block bail to interp) and for jm->isBlock below.
    g_emitIsBlock =
        methObj->classIndex() == interp.compiledBlockClassIndex();
    // M1 x19 invariant: UNCONDITIONAL since 2026-06-11 (five one-cycle
    // movs at activation commits, gate-verified by the 2468-test soak)
    // — it lets the TRAMPOLINE publish x19 -> JS_JITMETHOD at its own
    // exit paths unconditionally, which M4's lazy mirrors require
    // (trampoline-generated exits like the Lresume_null refusal have
    // no emitted exit stub to do the publish).
    g_fsrX19 = true;
    g_bcStartDelta = (2 + numLiterals) * 8;
    g_fsrLazyVerify = GET_DEBUG_BOOL(PHARO_T1_FSR_LAZY_VERIFY);
    g_fsrLazy = GET_DEBUG_BOOL(PHARO_T1_FSR_LAZY) || g_fsrLazyVerify;
    g_fsrNodepthVerify = GET_DEBUG_BOOL(PHARO_T1_FSR_NODEPTH_VERIFY);
    g_fsrNodepth = GET_DEBUG_BOOL(PHARO_T1_FSR_NODEPTH) || g_fsrNodepthVerify;
    g_fsrCursor = fsrCursorMode()
               || GET_DEBUG_BOOL(PHARO_T1_FSR_CURSOR);  // legacy opt-in, now a no-op
    g_fsrCursorVerify = GET_DEBUG_BOOL(PHARO_T1_FSR_CURSOR_VERIFY);
    g_fsrX19Verify = GET_DEBUG_BOOL(PHARO_T1_FSR_X19_VERIFY);
    {
        uint32_t cls = methObj->classIndex();
        if (cls == interp.compiledBlockClassIndex()) {
            static int blockCount = 0;
            blockCount++;
            bool reject = false;
            if (g_debug.t1NoBlocks) reject = true;
            if (g_debug.t1BlocksFirstN >= 0
                    && blockCount > g_debug.t1BlocksFirstN) reject = true;
            if (g_debug.t1BlocksOnlyN >= 0
                    && blockCount != g_debug.t1BlocksOnlyN) reject = true;
            if (g_debug.t1BlocksSkipFrom >= 0
                    && g_debug.t1BlocksSkipTo >= 0
                    && blockCount >= g_debug.t1BlocksSkipFrom
                    && blockCount <= g_debug.t1BlocksSkipTo) {
                reject = true;
            }
            if (g_debug.t1BlocksTrace) {
                // Resolve the block's HOME method selector: a
                // CompiledBlock's last literal is its outer code
                // (CompiledMethod, or another CompiledBlock — follow
                // up to 3 levels).
                std::string home = "?";
                {
                    Oop cur = compiledMethod;
                    for (int hop = 0; hop < 3; hop++) {
                        ObjectHeader* co = cur.asObjectPtr();
                        Oop chdr = co->slotAt(0);
                        if (!chdr.isSmallInteger()) break;
                        size_t cnl = (size_t)(chdr.asSmallInteger() & 0x7FFF);
                        if (cnl == 0 || cnl >= co->slotCount()) break;
                        Oop outer = co->slotAt(cnl);
                        if (!outer.isObject() || outer.rawBits() < 0x10000) break;
                        ObjectHeader* oo = outer.asObjectPtr();
                        if (!oo->isCompiledMethod()) break;
                        if (oo->classIndex() == interp.compiledBlockClassIndex()) {
                            cur = outer;            // nested block — keep walking
                            continue;
                        }
                        home = memory.selectorOf(outer)
                             + " (" + interp.classNameOfMethod(outer) + ")";
                        break;
                    }
                }
                fprintf(stderr,
                        "[T1-BLOCK] #%d oop=0x%llx bcLen=%zu %s home=%s bc=",
                        blockCount,
                        (unsigned long long)compiledMethod.rawBits(),
                        bcLenRaw, reject ? "REJECT" : "accept",
                        home.c_str());
                for (size_t bi = 0; bi < bcLenRaw && bi < 16; bi++)
                    fprintf(stderr, "%02x ", bc[bi]);
                fprintf(stderr, "\n");
            }
            if (reject) {
                g_failed++; g_failedBlock++;
                return nullptr;
            }
        }
    }
    // Trim trailer bytes past the method's last unconditional return,
    // mirroring the stencil decoder's logic (JITCompiler.cpp:639-660):
    // post-return bytes are dead code — selector/temp-name oop trailers
    // that the image packs into CompiledMethod.bytes() — and treating
    // them as bytecodes would pollute the pre-scan.
    //
    // Set PHARO_ASMJIT_T1_NO_TRIM=1 to disable for bisection.
    const bool noTrim = pharo::g_debug.asmjitT1NoTrim;
    size_t bcLen = noTrim ? bcLenRaw : computeLiveLength(bc, bcLenRaw);
    // Diagnostic for the sortStructs:into: corruption hunt.
    if (__builtin_expect(g_debug.sortstrWatch, 0)) {
        std::string sel = memory.selectorOf(compiledMethod);
        if (sel == "startup:" || sel == "registeredClass") {
            static size_t cCount = 0;
            cCount++;
            fprintf(stderr,
                "[T1-COMPILE-TRACE #%zu] #%s bcLenRaw=%zu bcLen=%zu "
                "method_oop=0x%llx bytes:",
                cCount, sel.c_str(),
                bcLenRaw, bcLen,
                (unsigned long long)compiledMethod.rawBits());
            for (size_t bi = 0; bi < bcLenRaw && bi < 32; bi++) {
                fprintf(stderr, " %02x", bc[bi]);
            }
            fprintf(stderr, "\n");
        }
    }

    // Buffer for emitted bytes.  Send emit (1-slot IC probe + miss path)
    // takes ~25 instructions ≈ 100 bytes; arith ≈ 70 bytes; pushes ≈ 30
    // bytes.  Inline-J2J emit (PHARO_T1_INLINE_J2J=1) adds ~200 bytes per
    // send site.  Raised from 128 to 512 bytes/bytecode (2026-05-17) to
    // accommodate inline-J2J + per-bail counters + return prelude.
    // 2026-06-09: the 512 B/bytecode estimate was TOO SMALL — each send
    // bytecode emits the full IC-probe + inline-spec dispatch (~900 B/bytecode
    // observed), so send-heavy methods (incl. benchFib) overflowed the buffer
    // and FAILED to compile (emitMethodBytes returns false at code_size>outCap)
    // -> ran interpreted -> the ~35x Cog gap on recursive sends. The buffer is
    // transient (freed after the code-zone copy; it does NOT consume the 16 MB
    // code zone, which is sized by the actual code_size), so size it generously
    // and grow-and-retry on the rare overflow rather than failing the compile.
    size_t cap = bcLen * 1536 + 4096;
    if (cap > 1048576) {            // genuinely huge method (>~680 bytecodes)
        g_failed++; g_failedBcOther++;
        return nullptr;
    }
    std::vector<uint8_t> buf(cap);
    size_t emitted = 0;
    bool   isReal  = false;
    uint64_t nilBits = memory.nil().rawBits();
    // Offset of bc[0] from the CompiledMethod object's address.
    //   methObj layout:  [ObjectHeader 8B][slot 0 = header][slot 1..N = lits][bytes...]
    //   bc[0] address  = methObj + 8 (header) + 8 * (1 + numLiterals)
    // Bail emit uses `state.method.rawBits() + (bcOffsetBase + i)` so
    // state.ip survives GC compaction (the alternative — baking the
    // absolute bytecode address — dangles when the method moves).
    int bcOffsetBase = 8 + (int)((1 + numLiterals) * 8);
    // bcToCode: per-bytecode emit start within the JIT code.  Filled
    // by emitMethodBytes from per-bytecode labels.  Slot [bcLen] gets
    // the end-of-machine-code offset after emit.
    std::vector<uint32_t> bcToCode(bcLen + 1, 0);
    // Caller method's argCount/tempCount — needed by the inline-J2J
    // emit's self-recursive callee-setup so it can skip the dynamic
    // tempCount load + init-loop overhead when tempCount is known at
    // compile time.
    int callerArgCount  = (int)((headerBits >> 24) & 0x0F);
    int callerTempCount = (int)((headerBits >> 18) & 0x3F);
    // Pre-scan bytecodes to find a common nArgs for ALL Phase4 send
    // sites in this method.  If uniform, the return prelude can use
    // the value as an immediate (saving the load+lsl+sub).  If
    // mixed (or no Phase4 sends), pass -1 to fall back to the
    // dynamic load-from-save path.
    int staticJ2JArgCount = -1;
    int emitSkip = (primIdx > 0) ? 3 : 0;
    {
        bool mixed = false;
        forEachRealOpcode(bc + emitSkip, bcLen - (size_t)emitSkip,
                          [&](size_t, uint8_t op) {
            if (mixed || !isPhase4SendOp(op)) return;
            int n = sendNArgs(op);
            if (staticJ2JArgCount < 0) {
                staticJ2JArgCount = n;
            } else if (staticJ2JArgCount != n) {
                staticJ2JArgCount = -1;  // mixed
                mixed = true;
            }
        });
    }
    // SOUNDNESS GATE (2026-06-10, found by PHARO_SP_DEPTH_CHECK): the
    // fold assumes every save this method's return prelude pops was
    // pushed by THIS method's own send sites — true only for pure
    // self-recursion.  Under xmethod cross-method dispatch (and
    // inline-block-value), the CALLEE's prelude pops a save pushed by
    // the CALLER's send site, whose nArgs is unknowable here: a callee
    // whose own sites are uniform 0-arg folded the sp-adjust to zero
    // while popping a 2-arg caller save -> sp left high by exactly
    // nArgs, retval in an arg slot (the deterministic MAX_IC=1 silent
    // startup-loss; first [SP-DEPTH] hit = handle:offset: delta=2).
    // Every push site writes save.sendArgCount correctly, so the
    // dynamic load-from-save path is always sound — force it whenever
    // a cross-method route could pop this method's prelude.
    if (g_debug.t1InlineJ2JXmethod || g_debug.t1InlineBlockValue) {
        staticJ2JArgCount = -1;
    }
    // FINDNODE_WATCH: emit the inline-getter write recorder ONLY for asTuple,
    // so just its 2 getter sites get the BLR (no global code-size bloat).
    g_emitGetterTrace = GET_DEBUG_BOOL(PHARO_FINDNODE_WATCH)
        && memory.selectorOf(compiledMethod) == "asTuple";
    // PMS B1: pre-size the patch-record buffer with the same send-site
    // scan the payload layout uses below (the two counts MUST agree —
    // they index the same IC-site space).
    std::vector<SendSitePatch> patchRecords;
    if (!GET_DEBUG_BOOL(PHARO_T1_NO_PATCHED_SENDS)) {
        uint16_t nSites = 0;
        forEachRealOpcode(bc, bcLen, [&](size_t, uint8_t op) {
            if (isPhase4SendOp(op)) nSites++;
        });
        patchRecords.resize(nSites);   // value-init: all-zero records
    }
    SendSitePatch* patchRecPtr =
        patchRecords.empty() ? nullptr : patchRecords.data();
    std::vector<std::pair<uint32_t,uint32_t>> resumeOvPairs;
    bool emitOk = emitMethodBytes(bc, bcLen, nilBits, bcOffsetBase, primIdx,
                         callerArgCount, callerTempCount, staticJ2JArgCount,
                         buf.data(), cap, &emitted, &isReal, bcToCode.data(),
                         patchRecPtr, (uint32_t)patchRecords.size(),
                         &resumeOvPairs);
    // Grow-and-retry on buffer overflow (emitMethodBytes returns false when
    // code_size > outCap).  The transient buffer is freed after the code-zone
    // copy, so doubling it is cheap; failing instead would lose a real method
    // to the interpreter (the ~35x Cog gap).  Re-emit is clean (fresh
    // CodeHolder per call).  Bounded so a genuinely-too-big method still bails.
    while (!emitOk && cap < 1048576) {
        cap *= 2;
        buf.assign(cap, 0);
        std::fill(bcToCode.begin(), bcToCode.end(), 0);
        std::fill(patchRecords.begin(), patchRecords.end(), SendSitePatch{});
        resumeOvPairs.clear();
        emitOk = emitMethodBytes(bc, bcLen, nilBits, bcOffsetBase, primIdx,
                         callerArgCount, callerTempCount, staticJ2JArgCount,
                         buf.data(), cap, &emitted, &isReal, bcToCode.data(),
                         patchRecPtr, (uint32_t)patchRecords.size(),
                         &resumeOvPairs);
    }
    if (!emitOk) {
        g_failed++; g_failedBcOther++;
        return nullptr;
    }
    // PMS B1 gate harness: FNV-1a over the emitted bytes, printed per
    // compile.  Diff a corpus knob-off before/after an emit change to
    // prove byte-identity (design §12.1).
    if (GET_DEBUG_BOOL(PHARO_T1_EMIT_HASH)) {
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i < emitted; i++) {
            h ^= buf[i]; h *= 1099511628211ull;
        }
        fprintf(stderr, "[EMIT-HASH] %s %zu 0x%016llx\n",
                memory.selectorOf(compiledMethod).c_str(), emitted,
                (unsigned long long)h);
    }
    bcToCode[bcLen] = (uint32_t)emitted;

    // FINDNODE_WATCH: dump asTuple's bcToCode table (per-bytecode code offset;
    // 0 = not a valid JIT re-entry point per codeOffsetForBC contract).
    if (g_emitGetterTrace) {
        fprintf(stderr, "[ASTUPLE-BCTOCODE] bcLen=%zu isReal=%d emitted=%zu bc=[",
                bcLen, (int)isReal, emitted);
        for (size_t i = 0; i < bcLen; i++)
            fprintf(stderr, "%02x ", bc[i]);
        fprintf(stderr, "] codeOff=[");
        for (size_t i = 0; i <= bcLen; i++)
            fprintf(stderr, "%u ", bcToCode[i]);
        fprintf(stderr, "]\n");
    }

    // Count send sites and compute the IC layout.  Each single-byte
    // send opcode (0x70..0xAF) gets one IC site.
    uint16_t numSendSites = 0;
    if (isReal) {
        forEachRealOpcode(bc, bcLen, [&](size_t, uint8_t op) {
            if (isPhase4SendOp(op)) numSendSites++;
        });
    }

    // Payload layout after the JITMethod header:
    //   [machine code, `emitted` bytes]
    //   [pad to 4-byte align]
    //   [bcToCode table, (bcLen+1)*4 bytes]
    //   [pad to 8-byte align]
    //   [selBitsArray, numSendSites*8 bytes]
    // codeSize passed to allocate() is the FULL payload size.
    uint32_t bcToCodeTableOffset =
        (uint32_t)((emitted + 3u) & ~3u);
    uint32_t bcToCodeTableSize   =
        (uint32_t)((bcLen + 1) * sizeof(uint32_t));
    uint32_t selBitsArrayOffset  =
        (bcToCodeTableOffset + bcToCodeTableSize + 7u) & ~7u;
    uint32_t selBitsArraySize    =
        (uint32_t)(numSendSites * sizeof(uint64_t));
    // PMS B1: SendSitePatch[numSendSites] after selBitsArray (design §9).
    // Emitted only under the knob; isReal gating matches the records
    // (stub emits push no labels, records stay zero = unpatchable).
    uint32_t patchMapOffset = 0;
    uint32_t patchMapSize   = 0;
    if (!patchRecords.empty() && isReal && numSendSites > 0) {
        patchMapOffset = (selBitsArrayOffset + selBitsArraySize + 7u) & ~7u;
        patchMapSize   = (uint32_t)(numSendSites * sizeof(SendSitePatch));
    }
    uint32_t prePayloadEnd = patchMapOffset
        ? patchMapOffset + patchMapSize
        : selBitsArrayOffset + selBitsArraySize;
    // Send-resume override side table: u32 count + count {bcOff,codeOff}
    // pairs, 8-aligned.  Only for real emits that recorded overrides.
    uint32_t resumeOvOffset = 0;
    uint32_t resumeOvSize   = 0;
    if (isReal && !resumeOvPairs.empty()) {
        resumeOvOffset = (prePayloadEnd + 7u) & ~7u;
        resumeOvSize   = (uint32_t)(4 + resumeOvPairs.size() * 8);
    }
    uint32_t payloadSize = resumeOvOffset
        ? resumeOvOffset + resumeOvSize
        : prePayloadEnd;

    // Allocate the JITMethod with full payload + IC sites.  CodeZone
    // calloc()s a heap-side icBuffer of numSendSites*IC_BYTES_PER_SITE.
    JITMethod* jm = zone.allocate(payloadSize, numSendSites);
    if (!jm) {
        g_failed++; g_failedBcOther++;
        return nullptr;
    }

    jm->compiledMethodOop = compiledMethod.rawBits();
    jm->refreshLiteralsCache();  // FSR M0
    jm->methodHeader      = static_cast<uint64_t>(headerBits);
    // Pre-compute bcStart so the inline-J2J emit can read it in one load
    // instead of recomputing per send.  Mirrors JITMethod::bcStart() —
    // depends only on the now-set compiledMethodOop + methodHeader.
    {
        uint64_t numLits = jm->methodHeader & 0x7FFFu;
        jm->bcStartCache = jm->compiledMethodOop + (2 + numLits) * 8;
    }
    jm->argCount          = static_cast<uint8_t>((headerBits >> 24) & 0x0F);
    jm->tempCount         = static_cast<uint8_t>((headerBits >> 18) & 0x3F);
    // numBytecodes is the source-bytecode count for the live region.
    // The runtime indexes bcToCodeTable() via this — `bcToCode[i] = 0`
    // for i where re-entry is invalid (the convention; entry-by-default
    // is via codeStart()).  slot [numBytecodes] holds the
    // end-of-machine-code offset for findMethodByPC.
    // 4b.2: advertise numBytecodes + bcToCodeTableOffset for chain-loop
    // resume — but ONLY for methods that contain no sends.  Methods
    // with sends fail post-resume with mustBeBoolean cascades; the
    // protocol mismatch is not yet understood (see plan_asmjit_replacement.md
    // §"Phase 4b.2 resume protocol gap").  Send-free methods are safe
    // to resume because there's no inline activation in their flow.
    //
    // 2026-05-15: tried turning this on (PHARO_ASMJIT_T1_FORCE_RESUME_FOR_SENDS=1)
    // plus syncing state.method at J2J call/return.  Still breaks the
    // differential fuzzer (every test JIT_DIFF, MUSTBOOL cascade in
    // #encoderClass, eventual stack overflow).  More than state.method
    // is desync'd at the trampoline — investigation deferred.
    //
    // Bisect knobs (default = off):
    //   PHARO_ASMJIT_T1_NO_BCTOCODE=1   — never advertise bcToCode
    //   PHARO_ASMJIT_T1_NO_NUMBC=1      — never advertise numBytecodes
    //   PHARO_ASMJIT_T1_FORCE_RESUME_FOR_SENDS=1
    //                                   — advertise resume even for
    //                                     send-containing methods (BROKEN)
    const bool noBcToCode = pharo::g_debug.asmjitT1NoBctocode;
    const bool noNumBc = pharo::g_debug.asmjitT1NoNumbc;
    const bool forceResumeForSends = pharo::g_debug.asmjitT1ForceResumeForSends;
    // Precompute hasCondJump (conditional jumps emit ExitMustBool mid-body).
    // Used by BOTH the advertiseResume gate (just below) and canBailMidMethod.
    bool t1HasCondJump = false;
    if (isReal) {
        for (size_t i = 0; i < bcLen; ) {
            uint8_t op = bc[i];
            if (SistaV1::isConditionalShortJump(op)) { t1HasCondJump = true; break; }
            int len = SistaV1::bytecodeLength(op);
            if (len <= 0 || i + (size_t)len > bcLen) break;
            i += (size_t)len;
        }
    }
    bool advertiseResume = isReal && !noNumBc && !noBcToCode && bcLen > 0;
    // 2026-05-29: gate resume on cond-jumps, NOT on sends.  The deferred
    // "Phase 4b.2 send-resume protocol gap" manifests as mustBeBoolean
    // cascades, which require conditional jumps.  Send-methods WITHOUT
    // cond-jumps (e.g. AIWeightedEdge>>asTuple = ^{from model. to model.
    // weight}) can resume safely; forcing them onto the interp-resume path
    // (numBytecodes=0) is what triggers the AI-Algorithms-Graph asTuple
    // operand corruption (asTuple re-runs its build with a stale prior tuple
    // as element-0 — see docs/results-jitpkg.md).  Cond-jump send-methods
    // stay non-resumable (the MUSTBOOL risk).  Opt-in via
    // PHARO_T1_RESUME_SENDS_NO_CONDJUMP=1 while validating.
    // DEFAULT-ON since 2026-06-11 (the first send-resume rung): six root
    // causes fixed (phantom IC sites, dangled NLR markers, dropped
    // IC-offset adds, chain create-handler sp/global sync, saveless
    // null-cursor underflow, chain-ExitYield identity sync), qualified by
    // an IDENTICAL 60-class SUnit A/B (4134=4134, zero fail/timeout
    // both sides) + the deterministic eval ladder on all configs.
    // Opt-out: PHARO_T1_NO_RESUME_SENDS=1.  The old opt-in env var is
    // kept readable for script compat (a no-op when set).
    bool resumeSendsNoCondjump =
        !GET_DEBUG_BOOL(PHARO_T1_NO_RESUME_SENDS)
        || pharo::g_debug.asmjitT1ResumeSendsNoCondjump;
    // Send-resume compile-order bisect (2026-06-11, the only-idle wedge
    // hunt): treat methods whose compile sequence falls in
    // [RESUME_MIN_COMPILE, RESUME_MAX_COMPILE) as force-resume.
    bool resumeBisect = false;
    {
        int lo = GET_DEBUG_INT(PHARO_T1_RESUME_MIN_COMPILE);
        int hi = GET_DEBUG_INT(PHARO_T1_RESUME_MAX_COMPILE);
        if (lo >= 0) {
            extern uint64_t g_t1CompileSeq2;
            uint64_t seq = g_t1CompileSeq2;
            resumeBisect = seq >= (uint64_t)lo
                        && (hi < 0 || seq < (uint64_t)hi);
        }
        int lo2 = GET_DEBUG_INT(PHARO_T1_RESUME_MIN2_COMPILE);
        int hi2 = GET_DEBUG_INT(PHARO_T1_RESUME_MAX2_COMPILE);
        if (!resumeBisect && lo2 >= 0) {
            extern uint64_t g_t1CompileSeq2;
            uint64_t seq = g_t1CompileSeq2;
            resumeBisect = seq >= (uint64_t)lo2
                        && (hi2 < 0 || seq < (uint64_t)hi2);
        }
    }
    // Bisect-culprit identification: with a narrow MIN/MAX range +
    // PHARO_JIT_FAIL_REASONS, name the methods the range force-resumes.
    if (resumeBisect && pharo::g_debug.jitFailReasons) {
        fprintf(stderr, "[RESUME-BISECT] seq=%llu sel=#%s\n",
            (unsigned long long)g_t1CompileSeq2,
            memory.selectorOf(compiledMethod).c_str());
    }
    // Marked methods (prim 198 ensure: / 199 ifCurtailed:) must NEVER
    // resume via JIT: the unwind/exception machinery identifies their
    // frames as unwind-protect markers, and a JIT resume breaks that
    // invariant (force-rung bisect 2026-06-11: compile-seq 109 =
    // #ensure: alone reproduced the startup STONReaderError #freeze
    // termination).  Applies in ALL modes, including force/bisect.
    {
        int rawPrimIdx = (bcLen >= 3 && bc[0] == SistaV1::CallPrimitive)
            ? (bc[1] | ((bc[2] & 0x1F) << 8)) : 0;
        if (rawPrimIdx == 198 || rawPrimIdx == 199)
            advertiseResume = false;
    }
    // COND-JUMP RESUME DEFAULT-ON (2026-06-12): with the J2J gates
    // open (MAX_IC=8, b46 admit, PMS fix), force-rung measured a
    // consistent dict win (251-253 vs 268-271 interleaved, 3/3) with
    // clean ladders — the 06-12-morning "resume is not the lever"
    // verdict predated six shipped fixes.  Marked methods (prim
    // 198/199) remain excluded above.  Opt-out:
    // PHARO_T1_NO_RESUME_CONDJUMP=1 restores the old refusal for
    // cond-jump send methods only.
    if (numSendSites > 0 && !forceResumeForSends && !resumeBisect
            && (!resumeSendsNoCondjump
                || (t1HasCondJump
                    && GET_DEBUG_BOOL(PHARO_T1_NO_RESUME_CONDJUMP))))
        advertiseResume = false;
    // DEBUG ISOLATION (PHARO_T1_RESUME_ONLY_SEL): force send-resume ON for a
    // single selector so the resume protocol can be exercised/validated on one
    // controlled method (e.g. benchFib) WITHOUT enabling it during startup.
    // Wins over the send-gate above; still requires a real, bc-bearing method.
    {
        const char* resumeOnlySel = GET_DEBUG_STR(PHARO_T1_RESUME_ONLY_SEL);
        if (resumeOnlySel && isReal && !noNumBc && !noBcToCode && bcLen > 0) {
            // Comma-separated list (2026-06-11: pair/nested-resume repros).
            std::string sel = memory.selectorOf(compiledMethod);
            const char* pp = resumeOnlySel;
            while (*pp) {
                const char* end = pp;
                while (*end && *end != ',') end++;
                if ((size_t)(end - pp) == sel.size()
                        && std::memcmp(pp, sel.data(), sel.size()) == 0) {
                    advertiseResume = true;
                    break;
                }
                pp = (*end == ',') ? end + 1 : end;
            }
        }
    }
    jm->numBytecodes      = advertiseResume ? (uint16_t)bcLen : 0;
    jm->numICEntries      = numSendSites;
    jm->bcToCodeTableOffset = advertiseResume ? bcToCodeTableOffset : 0;
    jm->selBitsArrayOffset =
        numSendSites > 0 ? selBitsArrayOffset : 0;
    jm->patchMapOffset = patchMapOffset;   // PMS B1 (0 = no map)
    jm->resumeOvOffset = resumeOvOffset;   // send-resume side table (0 = none)
    jm->tier              = 1;
    // hasPrimPrologue = true means the chain loop's inline-activation
    // path (Interpreter.cpp:18731-18732) will activate this method
    // without a separate C++ primitive try.  Our prologue does the
    // SmI fast path inline and rets on success; on failure falls
    // through to the bytecode emit.
    jm->hasPrimPrologue   = (primIdx > 0);
    // Was hardcoded false — T1-compiled blocks were invisible to every
    // isBlock consumer (chain-loop block-resume gate, MAT_LOG).
    jm->isBlock           = g_emitIsBlock;
    jm->pinned            = false;
    jm->hasSends          = false;
    jm->hasHeapWrites     = false;
    jm->hasRecvFieldAccess= false;
    jm->hasRecvFieldWrite = false;
    jm->hasLitVarWrite    = false;
    jm->maxRecvFieldIndex = 0;
    jm->isSpliceTarget    = false;
    // Prologue-leaf methods are NOT stub-on-entry: the prologue is the
    // real hot path and the bail is the validated interp-resume shape.
    jm->isStubOnEntry     = !isReal && !g_emitPrologueLeaf;
    // canBailMidMethod = true when the emitter produces a mid-method
    // ExitMustBool bail (conditional jumps).  The chain loop's
    // inline-activate path (Interpreter.cpp:18807-18809) skips this
    // method when set; the activateMethod-recursive path is used
    // instead, which pushes a C++ frame the bail can return into.
    // Only real-emit methods can bail mid-method.  Stub-on-entry methods
    // (isReal=false) bail with ExitSend on entry — no mid-method bail, so
    // the inline-activate gate doesn't need to block them.
    {
        jm->canBailMidMethod = t1HasCondJump;  // precomputed above
        // F3-NL3: detect non-local return (BlockReturnNil 0x5D /
        // BlockReturnTop 0x5E) at compile time so the BV inline prep
        // can skip the per-call bytecode scan.
        //
        // Session H 2026-05-25: tightened — only flag NLR when the
        // block has 0x58-0x5C (method-style returns inside a block are
        // TRUE NLR), mid-block 0x5D/0x5E (not the natural tail), OR
        // 0xF9/0xFA (PushFullBlock/PushClosure — creates nested closures
        // whose outerContext would point at the BV-inlined block's
        // not-quite-real frame, breaking nested-block semantics).  The
        // bisect showed nested-closure-creating blocks are the source
        // of the ZnByteEncoder DNU under tail-only loosening.
        // Gated by t1NlrTailOnly — default ON since Session H Phase 5
        // (2026-05-25, see DebugSettings.cpp; opt-out via
        // PHARO_T1_NO_NLR_TAIL_ONLY=1).  NOTE: in tail-only mode this
        // scan sets hasNLR for ANY method containing a return opcode
        // 0x58-0x5C — i.e. virtually every real method.  hasNLR is a
        // BLOCK-VALUE-inline gate; do not treat it as a method-level
        // "has non-local return" flag (the 2026-06-09 xmethod gate
        // off-by-one read it as canBailMidMethod and rejected nearly
        // all cross-method inline-J2J callees).
        {
            bool nlr = false;
            if (isReal) {
                if (g_debug.t1NlrTailOnly) {
                    for (size_t i = 0; i < bcLen; i++) {
                        uint8_t op = bc[i];
                        if (op >= 0x58 && op <= 0x5C) {
                            nlr = true; break;
                        }
                        if ((op == 0x5D || op == 0x5E)
                            && i + 1 != bcLen) {
                            nlr = true; break;
                        }
                        // PushFullBlock(0xF9) / PushClosure(0xFA) create
                        // nested closures whose outerContext binds to
                        // the current activation.  BV inline's "fake"
                        // frame can't supply a real outerContext, so
                        // any nested block created from a BV-inlined
                        // host runs with a corrupted context — the
                        // visible failure is mis-interpreted bytecodes
                        // after the BV return (ZnByteEncoder DNU).
                        if (op == 0xF9 || op == 0xFA) {
                            nlr = true; break;
                        }
                    }
                } else {
                    for (size_t i = 0; i < bcLen; i++) {
                        uint8_t op = bc[i];
                        if (op == 0x5D || op == 0x5E) {
                            nlr = true; break;
                        }
                    }
                }
            }
            jm->hasNLR = nlr;
            // Mark receiver-ivar-storing methods.  The legacy stencil
            // compiler sets hasRecvFieldWrite per-stencil; the asmjit
            // path left it permanently false.  Consumed by the
            // PHARO_J2J_NO_HEAPWRITE_CALLEES bisect gate (J2J fills) —
            // the 2026-06-10 J2J corruption class is ivar stores in
            // J2J-called code (initializeHandle:offset:).
            if (isReal) {
                for (size_t i = 0; i < bcLen; i++) {
                    uint8_t op = bc[i];
                    if ((op >= SistaV1::PopStoreRecvBase
                            && op <= SistaV1::PopStoreRecvLast)
                        || op == SistaV1::ExtPopStoreRecv
                        || op == SistaV1::ExtStoreRecv) {
                        jm->hasRecvFieldWrite = true;
                        break;
                    }
                }
            }
        }
        // g_debug.t1ForceBailMid (set by PHARO_T1_FORCE_BAIL_MID or
        // PHARO_T1_FORCE_SIMPLE) — force canBailMidMethod on every
        // method to disable the chain-loop inline-activate fast path.
        if (g_debug.t1ForceBailMid) {
            jm->canBailMidMethod = true;
        }
        // Eδ.1 (2026-05-24): a method is "no-bail tier-2" callable when
        // it neither bails mid-method nor performs any send.  Such a
        // method always runs to completion via a plain `ret`; callers
        // can skip the J2J save push entirely (Eδ.2 will wire this up
        // at the IC HIT inline-J2J emit).  Stub-on-entry methods also
        // qualify trivially (they bail with ExitSend on entry and never
        // return cleanly; the gate also catches them via numICEntries
        // checks elsewhere, but mark them false here so callers
        // requesting a saveless call route correctly to the stub path).
        // PHARO_T1_SAVELESS_FORCE_SEL: ceiling EXPERIMENT knob — force
        // the saveless call path for methods whose selector is in the
        // comma list.  UNSOUND in general (the recovery stub
        // retro-appends the elided save at the cursor, which is out of
        // order when the callee pushed its own saves before bailing) —
        // use only on bench selectors to measure how much of the
        // per-call gap is J2J save traffic.  (The global force crashes
        // startup, as predicted by the ordering analysis.)
        bool forceSaveless = false;
        if (const char* fsel = GET_DEBUG_STR(PHARO_T1_SAVELESS_FORCE_SEL)) {
            std::string sel = memory.selectorOf(compiledMethod);
            const char* p = fsel;
            while (*p) {
                const char* end = p;
                while (*end && *end != ',') end++;
                if ((size_t)(end - p) == sel.size()
                        && std::memcmp(p, sel.data(), sel.size()) == 0) {
                    forceSaveless = true;
                    break;
                }
                p = (*end == ',') ? end + 1 : end;
            }
        }
        jm->canSkipJ2JSave = isReal
                              && (forceSaveless
                                  || (!jm->canBailMidMethod
                                      && jm->numICEntries == 0));
        if (isReal) {
            g_canSkipJ2JSave_total++;
            if (jm->canSkipJ2JSave) g_canSkipJ2JSave_count++;
        }
        if (t1HasCondJump) {
            g_condJumpRealCompiles++;
            if (pharo::g_debug.asmjitT1TraceCond) {
                fprintf(stderr,
                        "[T1-COND-COMPILE] #%zu sel=#%s bcLen=%zu oop=0x%llx bc=",
                        g_condJumpRealCompiles,
                        memory.selectorOf(compiledMethod).c_str(),
                        bcLen, (unsigned long long)compiledMethod.rawBits());
                for (size_t bi = 0; bi < bcLen && bi < 32; bi++)
                    fprintf(stderr, "%02x ", bc[bi]);
                fprintf(stderr, "primIdx=%d argCount=%d tempCount=%d advRes=%d "
                        "numBC=%u\n",
                        primIdx, jm->argCount, jm->tempCount,
                        (int)advertiseResume, jm->numBytecodes);
                fprintf(stderr, "  bcToCode:");
                for (size_t bi = 0; bi <= bcLen && bi < 32; bi++)
                    fprintf(stderr, " [%zu]=%u", bi, bcToCode[bi]);
                fprintf(stderr, "\n");
            }
        }
        // PHARO_SORTSTR_WATCH=1: dump bcToCode for #isEmpty + #size, the
        // methods involved in sortStructs:into:'s failing chain.  Tells us
        // whether the bytecode→code mapping is plausible (e.g., whether
        // bcToCode[2] for #isEmpty's PushZero is distinct from bcToCode[4]
        // for ReturnTop).
        if (g_debug.sortstrWatch && isReal && bcLen > 0) {
            std::string sel = memory.selectorOf(compiledMethod);
            if (sel == "isEmpty" || sel == "size") {
                fprintf(stderr,
                        "[T1-COMPILE-DBG] sel=#%s bcLen=%zu emitted=%zu "
                        "oop=0x%llx primIdx=%d numBC=%u bc=",
                        sel.c_str(), bcLen, emitted,
                        (unsigned long long)compiledMethod.rawBits(),
                        primIdx, jm->numBytecodes);
                for (size_t bi = 0; bi < bcLen && bi < 32; bi++)
                    fprintf(stderr, "%02x ", bc[bi]);
                fprintf(stderr, "\n  bcToCode:");
                for (size_t bi = 0; bi <= bcLen && bi < 32; bi++)
                    fprintf(stderr, " [%zu]=%u", bi, bcToCode[bi]);
                fprintf(stderr, "\n");
            }
        }
    }

    std::memcpy(jm->codeStart(), buf.data(), emitted);
    // Write bcToCode table after the machine code.  Required by the
    // chain loop (Interpreter.cpp:18062-18207) to compute resume
    // offsets after callee returns.  Skipped for stub-only methods
    // (numBytecodes = 0; bcToCodeTableOffset = 0 — no table).
    if (isReal && bcLen > 0) {
        uint32_t* tbl = reinterpret_cast<uint32_t*>(
            jm->codeStart() + bcToCodeTableOffset);
        for (size_t i = 0; i <= bcLen; i++) tbl[i] = bcToCode[i];
    }
    // PMS invariant 4 (design §2/§4.2): no BL/BLR may immediately
    // precede a patch word — no native return address may point AT a
    // patched word (what makes patch-while-frames-live safe).  Scan the
    // emitted bytes once per compile; abort loudly on violation (an
    // emit-shape regression, not a runtime condition).
    if (patchMapOffset != 0) {
        auto isCall = [](uint32_t insn) {
            return (insn & 0xFC000000u) == 0x94000000u      // BL
                || (insn & 0xFFFFFC1Fu) == 0xD63F0000u;     // BLR
        };
        auto wordAt = [&](uint32_t off) {
            uint32_t w = 0;
            if (off >= 4 && off + 4 <= emitted) std::memcpy(&w, buf.data() + off - 4, 4);
            return w;
        };
        for (const auto& rec : patchRecords) {
            uint32_t patchOffs[8]; int n = 0;
            if (rec.keyMovzOffset) {
                patchOffs[n++] = rec.keyMovzOffset;          // W0
                patchOffs[n++] = rec.keyMovzOffset + 4;      // W1
                patchOffs[n++] = rec.keyMovzOffset + 16;     // W2
            }
            if (rec.tailOffset) {
                patchOffs[n++] = rec.tailOffset;             // W3
                patchOffs[n++] = rec.tailOffset + 4;         // W4
                patchOffs[n++] = rec.tailOffset + 8;         // W5
                patchOffs[n++] = rec.tailBranchOffset;       // W6
            }
            for (int i = 0; i < n; i++) {
                if (isCall(wordAt(patchOffs[i]))) {
                    fprintf(stderr, "[PMS] FATAL: BL/BLR precedes patch word "
                            "at emit offset %u — invariant 4 violated\n",
                            patchOffs[i]);
                    std::abort();
                }
            }
        }
    }
    // Send-resume override side table (in the same W window).
    if (resumeOvOffset != 0) {
        uint32_t* t = reinterpret_cast<uint32_t*>(
            jm->codeStart() + resumeOvOffset);
        t[0] = (uint32_t)resumeOvPairs.size();
        for (size_t k = 0; k < resumeOvPairs.size(); k++) {
            t[1 + 2 * k] = resumeOvPairs[k].first;
            t[2 + 2 * k] = resumeOvPairs[k].second;
        }
    }
    // PMS B1: write the resolved patch map (design §9).  Defensive
    // clamp: the pre-emit and layout send-site scans are the same loop,
    // but never index past the in-zone allocation if they ever drift.
    if (patchMapOffset != 0) {
        auto* pmap = reinterpret_cast<SendSitePatch*>(
            jm->codeStart() + patchMapOffset);
        size_t n = patchRecords.size() < (size_t)numSendSites
                 ? patchRecords.size() : (size_t)numSendSites;
        for (size_t i = 0; i < n; i++) pmap[i] = patchRecords[i];
        for (size_t i = n; i < (size_t)numSendSites; i++)
            pmap[i] = SendSitePatch{};
    }
    // Write selBitsArray (per-send selector Symbol Oop).  Used by
    // jit_rt_ic_miss after GC to recover the selector when the
    // in-IC slot[18] has been GC-zeroed (Task #41).
    if (numSendSites > 0) {
        uint64_t* sba = reinterpret_cast<uint64_t*>(
            jm->codeStart() + selBitsArrayOffset);
        uint16_t siteIdx = 0;
        Oop* literals = methObj->slots() + 1;
        Oop ssArrayOop = memory.specialObject(
            SpecialObjectIndex::SpecialSelectorsArray);
        ObjectHeader* ssHdr = (ssArrayOop.isObject()
                               && ssArrayOop.rawBits() > 0x10000)
                              ? ssArrayOop.asObjectPtr() : nullptr;
        forEachRealOpcode(bc, bcLen, [&](size_t i, uint8_t op) {
            if (!isPhase4SendOp(op)) return;
            uint64_t selBits = 0;
            if (op >= 0x60 && op <= 0x6F) {
                // Binary special selectors: ssArray[(op - 0x60) * 2].
                // Only 0x69 /, 0x6A \\, 0x6B @, 0x6D // reach here —
                // the others have Phase 3 inline emits.
                if (ssHdr) {
                    size_t slot = (size_t)(op - 0x60) * 2;
                    if (slot < ssHdr->slotCount()) {
                        selBits = ssHdr->slotAt(slot).rawBits();
                    }
                }
            } else if (op >= 0x70 && op <= 0x7F) {
                // Special selector: ssArray[(op - 0x70 + 16) * 2].
                // Slot 0 = selector, slot 1 = nArgs (we don't need
                // nArgs here — the runtime gets it from the bytecode).
                if (ssHdr) {
                    size_t slot = (size_t)((op - 0x70) + 16) * 2;
                    if (slot < ssHdr->slotCount()) {
                        selBits = ssHdr->slotAt(slot).rawBits();
                    }
                }
            } else {
                // Literal send 0/1/2 args: selector = literals[op & 0x0F].
                int litIdx = op & 0x0F;
                if (litIdx < numLiterals) {
                    selBits = literals[litIdx].rawBits();
                }
            }
            if (GET_DEBUG_BOOL(PHARO_SP_DEPTH_TRAP)) {
                std::string mSel = memory.selectorOf(compiledMethod);
                if (mSel == "minExtent" && siteIdx == 0) {
                    const uint32_t* t = jm->bcToCodeTableOffset
                        ? reinterpret_cast<const uint32_t*>(
                              jm->codeStart() + jm->bcToCodeTableOffset)
                        : nullptr;
                    if (t) {
                        fprintf(stderr, "[B2C-SLICE] jm=%p numBc=%u:",
                                (void*)jm, jm->numBytecodes);
                        for (int k = 139; k <= 148; k++)
                            fprintf(stderr, " [%d]=%u", k, t[k]);
                        fprintf(stderr, "\n");
                    }
                    {
                        // Scan the bc-144 send's code window for the
                        // expected site-45 IC bake: add x5,x5,#6840
                        // (0x916AE0A5) and any add x5,x5,#imm.
                        const uint32_t* c = reinterpret_cast<const uint32_t*>(
                            jm->codeStart());
                        int found = 0;
                        for (uint32_t w = 98104 / 4; w < 100068 / 4; w++) {
                            uint32_t insn = c[w];
                            // add x5, x5, #imm  (sf=1 op 0x91, Rd=Rn=5)
                            if ((insn & 0xFF0003FF) == 0x910000A5) {
                                uint32_t imm = (insn >> 10) & 0xFFF;
                                fprintf(stderr,
                                    "[X5ADD] at code+%u: add x5,x5,#%u\n",
                                    w * 4, imm);
                                found++;
                            }
                        }
                        fprintf(stderr, "[X5ADD] total=%d in bc144 window\n",
                                found);
                    }
                }
                if (mSel == "minExtent") {
                    Oop so = pharo::Oop::fromRawBits(selBits);
                    const uint32_t* b2c = jm->bcToCodeTableOffset
                        ? reinterpret_cast<const uint32_t*>(
                              jm->codeStart() + jm->bcToCodeTableOffset)
                        : nullptr;
                    fprintf(stderr, "[SBA-W] site%u bcOff=%zu op=0x%02X sel=%s "
                            "b2c[i]=%u b2c[i+1]=%u\n",
                        siteIdx, i, op,
                        (so.isObject() && so.rawBits() > 0x10000)
                            ? memory.oopToString(so).c_str() : "?",
                        b2c ? b2c[i] : 0, b2c ? b2c[i + 1] : 0);
                }
            }
            sba[siteIdx++] = selBits;
            // Also seed the in-IC slot[18] (selectorBits) so the
            // stencil-style probe contract works.  recoverAfterGC
            // zeros this on compaction; jit_rt_ic_miss recovers from
            // sba in that case.
            if (jm->icBuffer) {
                uint8_t* icStart = jm->icZoneStart();
                uint64_t* siteSlots = reinterpret_cast<uint64_t*>(
                    icStart + (siteIdx - 1) * IC_BYTES_PER_SITE);
                siteSlots[IC_SELBITS_SLOT] = selBits;
            }
        });

        // jit-84 B1: populate sendSiteMap_ in the shared JITCompiler so
        // Interpreter::extractInlineHintsForMethod can map sendIdx →
        // bcOffset for hint generation.  Without this, hints are empty
        // for asmjit-T1-compiled methods, and Sista's self-rec
        // recognition never fires.
        if (interp.jitRuntime().compiler()) {
            std::vector<uint16_t> bcOffsets;
            bcOffsets.reserve(numSendSites);
            forEachRealOpcode(bc, bcLen, [&](size_t i, uint8_t op) {
                if (isPhase4SendOp(op)) {
                    bcOffsets.push_back(static_cast<uint16_t>(i));
                }
            });
            interp.jitRuntime().compiler()->setSendSiteBCOffsets(
                compiledMethod.rawBits(), std::move(bcOffsets));
        }
    }
    platform::flushICache(jm->codeStart(), emitted);
    jm->state = MethodState::Compiled;
    platform::makeExecutable(jm, jm->totalSize);

    methodMap.insert(compiledMethod.rawBits(), jm);

    g_compiled++;
    if (isReal) g_compiledReal++;
    else        g_compiledStub++;

    const bool trace = g_debug.useAsmjitT1Trace;
    bool emitTrace = trace && (g_compiled <= 10 || (g_compiled % 100 == 0)
                                || (isReal && g_compiledReal <= 30));
    if (emitTrace) {
        std::fprintf(stderr,
                     "[asmjit-t1] #%zu (%s) compiled %llu -> jm=%p code=%p (%zu bytes, %zu bc)\n",
                     g_compiled, isReal ? "real" : "stub",
                     static_cast<unsigned long long>(compiledMethod.rawBits()),
                     (void*)jm, (void*)jm->codeStart(), emitted, bcLen);
    }
    // PHARO_T1_DUMP_SEL=<selector>: dump raw emitted bytes for the
    // named method to /tmp/jit_<sel>.bin (overwrites).  Useful for
    // objdump-ing the exact code that ran.
    if (const char* dumpSel = g_debug.t1DumpSel) {
        std::string sel = memory.selectorOf(compiledMethod);
        if (sel == dumpSel) {
            static int dumpIdx = 0;
            dumpIdx++;
            std::string path = std::string("/tmp/jit_") + sel
                + "_" + std::to_string(dumpIdx) + ".bin";
            // Sanitize: replace ':' with '_' for filesystem-friendly name.
            for (auto& c : path) if (c == ':') c = '_';
            if (FILE* f = std::fopen(path.c_str(), "wb")) {
                std::fwrite(jm->codeStart(), 1, emitted, f);
                std::fclose(f);
                std::fprintf(stderr,
                             "[T1-DUMP] #%d wrote %zu bytes of #%s oop=0x%llx bcLen=%zu bc=",
                             dumpIdx, emitted, sel.c_str(),
                             (unsigned long long)compiledMethod.rawBits(),
                             bcLen);
                for (size_t bi = 0; bi < bcLen && bi < 32; bi++)
                    std::fprintf(stderr, "%02x ", bc[bi]);
                std::fprintf(stderr, "isReal=%d\n", (int)isReal);
            }
        }
    }
    // E2 2026-05-24: log every successful asmjit-T1 compile when
    // PHARO_T1_TRACE_COMPILE=1 (env var → DebugSettings).  Used to
    // bisect WHICH method's JIT-compilation breaks SessionManager
    // startup-handler dispatch.
    if (jm && pharo::g_debug.t1TraceCompile) {
        static uint64_t compileSeq = 0;
        compileSeq++;
        std::string sel = memory.selectorOf(compiledMethod);
        std::fprintf(stderr,
                     "[T1-COMPILE #%llu] sel=#%s oop=0x%llx canBail=%d canSkipJ2J=%d bcLen=%zu\n",
                     (unsigned long long)compileSeq,
                     sel.c_str(),
                     (unsigned long long)compiledMethod.rawBits(),
                     (int)jm->canBailMidMethod,
                     (int)jm->canSkipJ2JSave,
                     bcLen);
    }
    return jm;
}

size_t asmjitT1Compiled() { return g_compiled; }
size_t asmjitT1Failed()   { return g_failed;   }

}  // namespace jit
}  // namespace pharo

#endif  // PHARO_JIT_ENABLED
