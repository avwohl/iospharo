// TrampolineAsm.S - Phase 1 hand-written J2J trampoline hot loop
//
// Copyright (c) 2026 Aaron Wohl. Licensed under the MIT License.
//
// Replaces the C++ `while` loop inside Interpreter::tryJITActivation that
// drives J2J self-recursive sends. The C++ version sits in
// src/vm/Interpreter.cpp around line 11086 and is selected when
// PHARO_ASM_TRAMPOLINE is NOT defined.
//
// Phase 1 goals (conservative, no ABI changes):
//   - Pin JITState*, save-stack base, save-cursor, frameDepth, calls, returns
//     in callee-saved registers x21-x27 so they survive BLR to stencils.
//   - Keep the JITState layout untouched — stencils read the struct at the
//     same offsets whether called from the C++ loop or from here.
//   - Assume caller has already toggled W^X to executable for the duration
//     of the call, matching the existing C++ trampoline.
//   - Assume x21-x28 are callee-saved per Apple/Linux ARM64 AAPCS.
//
// Stencils are assumed to clobber x0-x17, x19, x20, x30 (see JIT_CALL in
// src/vm/jit/JITState.hpp). x18 is reserved by Apple. We use x21-x28.
//
// DO NOT MODIFY the JITState or J2JSave field offsets below without also
// updating src/vm/jit/JITState.hpp, Interpreter.cpp (J2JSave struct), and
// scripts/extract_stencils.py.

#if defined(__aarch64__) && PHARO_JIT_ENABLED

    .text
    .p2align 4

// -------- JITState field offsets (see src/vm/jit/JITState.hpp) --------
#define JS_SP             0
#define JS_RECEIVER       8
#define JS_LITERALS       16
#define JS_TEMPBASE       24
#define JS_IP             48
#define JS_JITMETHOD      56
#define JS_ARGCOUNT       72
#define JS_EXITREASON     76
#define JS_RETURNVALUE    80
#define JS_CACHEDTARGET   88
#define JS_SENDARGCOUNT   104
#define JS_J2J_SAVE_CURSOR 144
#define JS_J2J_SAVE_LIMIT  152
#define JS_J2J_DEPTH      160
#define JS_J2J_DEPTH_INC  208

// -------- J2JSave field offsets: single source of truth --------
// (was a hand-synced duplicate of Interpreter.hpp's struct; the C++
//  side now static_asserts against the same macros)
#include "J2JSaveLayout.h" 

// -------- JITMethod field offsets (see src/vm/jit/JITMethod.hpp) --------
#define JM_COMPILEDMETHOD 0
#define JM_METHODHEADER   16
#define JM_CODESIZE       24
#define JM_NUMBYTECODES   30
#define JM_ARGCOUNT       34   // uint8_t cached from method header
#define JM_TEMPCOUNT      35
#define JM_BCTOCODEOFF    56   // bcToCodeTableOffset, see JITMethod.hpp
// JM_SIZE must equal sizeof(JITMethod) (see JITMethod.hpp).
// CAN'T USE sizeof IN ASSEMBLY — but a C++ runtime check in
// JITRuntime::initialize() asserts this constant matches sizeof().
// Bug 11b layer 5: was 80 while real struct grew to 88, causing
// every `add Xn, methodHdr, #JM_SIZE` to land 8 bytes BEFORE
// codeStart inside the JITMethod header (lastUsedEpoch field).
// Bumped to 96 (Task #41) when selBitsArrayOffset was added.
// Trimmed to 88 (W^X audit 2026-04-26) when executionCount/
// lastUsedEpoch/j2jDepthLimit/j2jCleanRuns moved to the
// heap-side JITMethodStats struct.
// Bumped to 96 (2026-05-03) when icBuffer was added — IC entries
// moved out of MAP_JIT into a heap-allocated side-buffer.
// Bumped to 104 (2026-05-20) when bcStartCache was added so the
// inline-J2J emit can read pre-computed bytecode-start in one load.
// Bumped to 112 (2026-05-23) when hasNLR was added (commit 3e625350);
// JM_BCTOCODEOFF also shifted 52 → 56 at the same time.  The hasNLR
// commit missed both updates — JIT silently failed to initialize for
// ~14 commits until E2 (2026-05-24) caught it.
// 128 since 2026-06-11 (FSR M0 literalsCache appended).  The silent
// drift to 120-vs-128 cost a 10x benchFib regression (V2 save identity
// derivation read the wrong header) — JITMethod.hpp now carries an
// EXACT static_assert(sizeof(JITMethod) == JM_SIZE_ASM) so any future
// growth fails the BUILD instead of the benchmark.
#define JM_SIZE           128

// -------- ExitReason values --------
#define EXIT_RETURN       1
#define EXIT_STACKOVFL    5
#define EXIT_SENDCACHED   7
#define EXIT_J2JCALL      10

// Maximum J2J save stack depth (matches MaxJ2JDepth in Interpreter.cpp)
#define MAX_J2J_BYTES     14336    // 256 * 56

// Stack overflow threshold (matches Interpreter::StackOverflowLimit)
#define SO_LIMIT          4096


// extern "C" void pharo_jit_j2j_trampoline(
//     pharo::jit::JITState* state,     // x0
//     void*                 saveStack,  // x1  (256 * 72 bytes on C stack)
//     size_t*               frameDepthInOut, // x2
//     size_t*               callsCountOut,   // x3
//     size_t*               returnsCountOut, // x4
//     uint64_t              nilOopBits);     // x5
//
// Assumes the caller has already toggled W^X to executable. On exit,
// writes localFrameDepth/localCalls/localReturns back through the pointer
// arguments. state.exitReason holds the reason the loop exited.

#if defined(__APPLE__)
#  define SYMBOL(name) _##name
#else
#  define SYMBOL(name) name
#endif

    .globl SYMBOL(pharo_jit_j2j_trampoline)
SYMBOL(pharo_jit_j2j_trampoline):
    // Stack frame (144 bytes):
    //   [sp, #0]:   x29 (old fp),  x30 (lr)
    //   [sp, #16]:  x21,           x22
    //   [sp, #32]:  x23,           x24
    //   [sp, #48]:  x25,           x26     (ABI save only — see below)
    //   [sp, #64]:  x27,           x28
    //   [sp, #80]:  x19,           x20     (stencils clobber; must preserve caller's)
    //   [sp, #96]:  frameDepthPtr, callsCountPtr
    //   [sp, #112]: returnsPtr,    (pad)
    //   [sp, #120]: localCalls,    localReturns   (was x25/x26 — sp-residency
    //                                              Phase 0 freed those registers;
    //                                              x25 is reserved as the
    //                                              register-resident state.sp
    //                                              mirror in Phase 1)
    stp     x29, x30, [sp, #-144]!
    mov     x29, sp
    stp     x21, x22, [sp, #16]
    stp     x23, x24, [sp, #32]
    stp     x25, x26, [sp, #48]
    stp     x27, x28, [sp, #64]
    stp     x19, x20, [sp, #80]
    stp     x2,  x3,  [sp, #96]
    str     x4,       [sp, #112]

    // Pin persistent state:
    //   x21 = JITState*
    //   x22 = save stack base (unchanging)
    //   x23 = save cursor (base + j2jDepth*72)
    //   x24 = localFrameDepth
    //   x25 = FREE (reserved: Phase-1 register-resident state.sp)
    //   x26 = FREE
    //   x27 = save base + MAX_J2J_BYTES (overflow comparison limit)
    //   x28 = nilOop bits
    //   localCalls/localReturns live at [sp,#120]/[sp,#128] (telemetry;
    //   +2 instr per call/return is paid once per TRAMPOLINE hop, vs
    //   the register they free saving ~2 instr per BYTECODE in Phase 1)
    mov     x21, x0
    mov     x22, x1
    mov     x23, x1                   // cursor = base (j2jDepth = 0)
    ldr     x24, [x2]                 // localFrameDepth = *frameDepthInOut
    stp     xzr, xzr, [sp, #120]      // localCalls = localReturns = 0
    mov     x27, #MAX_J2J_BYTES       // 18432 fits in movz (16-bit immediate)
    add     x27, x1, x27
    mov     x28, x5                   // nil Oop bits
    // x20 = j2jDepthInc constant (0x100000001).  Loaded here AND
    // in the JIT_CALL macro so asmjit-T1's inline-J2J emit can use
    // x20 directly instead of an `ldr x14, [OFF_J2J_DEPTH_INC]` per
    // push.  x20 is callee-saved per AAPCS, so blr/ret preserves it
    // through stencil/C calls.
    ldr     x20, [x21, #JS_J2J_DEPTH_INC]
    // x19 = state.jitMethod cached for asmjit-T1 IC HIT path.
    // For xmethod-off (default) state.jitMethod doesn't change
    // mid-execution, so this stays valid across J2J self-rec calls.
    // Saves 1 ldr per IC HIT (vs `ldr x11, [OFF_JITMETHOD]` per send).
    ldr     x19, [x21, #JS_JITMETHOD]

// ======== Dispatch loop ========
// Align loop head to a 16-byte fetch block boundary so the 4 decode slots
// {ldr/cmp/b.eq/cmp} land in a single fetch group. (Tried .p2align 6 for
// cache-line alignment: adds 14 nops which regressed measurably — stick
// with 4.)
    .p2align 4
Ltramp_loop:
    ldr     w0, [x21, #JS_EXITREASON]
    cmp     w0, #EXIT_J2JCALL
    b.eq    Ltramp_call
    cmp     w0, #EXIT_SENDCACHED
    b.eq    Ltramp_convert
    cmp     w0, #EXIT_RETURN
    b.ne    Ltramp_exit
    cmp     x23, x22                  // j2jDepth > 0 ?
    b.eq    Ltramp_exit               // depth == 0 → exit
    // Fall through to return path

// ======== Return path ========
Ltramp_return:
    // Bug-14 diagnostic: log retVal + popped save before we pop.
    // Caller-saved regs x0-x18 aren't live at Ltramp_return entry (the
    // loop-dispatch branch just landed here), so BL is safe to issue
    // without any reg save/restore.  Our pinned state (x21-x28) is
    // preserved by ABI.
    mov     x0, x21
    sub     x1, x23, #JSV_SIZE        // &save (what we're about to pop)
    bl      SYMBOL(pharo_jit_b5_tramp_ret)

    ldr     x9, [sp, #128]            // localReturns++ (frame slot — x26 freed)
    add     x9, x9, #1
    str     x9, [sp, #128]
    sub     x23, x23, #JSV_SIZE       // pop save slot; now points to just-popped
    // Sync state.j2jSaveCursor — same defensive rationale as Ltramp_call.
    str     x23, [x21, #JS_J2J_SAVE_CURSOR]
    sub     x24, x24, #1              // localFrameDepth--

    // retVal lives in x17 (caller-saved IP1) — we never cross a BLR between
    // this load and the store below, so no spill needed. Frees x7 to be
    // loaded early as the precomputed resume address.
    ldr     x17, [x21, #JS_RETURNVALUE]   // retVal

#if PHARO_J2J_SAVE_V2
    // V2: 32-byte packed save — {sp, receiver, tempBase, packedResume}.
    ldp     x8, x9, [x23, #JSV_SP]         // x8=save.sp, x9=save.receiver
    ldp     x10, x7, [x23, #JSV_TEMPBASE]  // x10=tempBase, x7=packedResume

    str     x9,  [x21, #JS_RECEIVER]
    str     x10, [x21, #JS_TEMPBASE]
    // sp restore: the resume continuation pops args itself; hand it the
    // caller sp and the retval in x1 (JIT_RESUME_CALL contract).
    str     x8,  [x21, #JS_SP]
    mov     x1,  x17                       // retval live-in for the continuation
    and     x7,  x7, #0x0000FFFFFFFFFFFF  // unpack the code address
    cbz     x7,  Lret_null_resume
    ldr     x20, [x21, #JS_J2J_DEPTH_INC]
    ldr     x25, [x21, #JS_SP]
    ldur    x26, [x25, #-8]           // simStack: TOS mirror live-in (design §4)
    mov     x0,  x21
    blr     x7
    str     x25, [x21, #JS_SP]
    b       Ltramp_loop
#else
    // Load save fields. JSV_JITMETHOD(32) and JSV_RESUMEADDR(40) are
    // adjacent → LDP.
    ldp     x8, x9, [x23, #JSV_SP]         // x8=save.sp, x9=save.receiver
    ldr     x10, [x23, #JSV_TEMPBASE]      // x10=save.tempBase
    ldp     x11, x7, [x23, #JSV_JITMETHOD] // x11=save.jitMethod, x7=save.resumeAddr

    str     x9,  [x21, #JS_RECEIVER]
    str     x10, [x21, #JS_TEMPBASE]

    // Derive caller's literals/ip/argCount from save.jitMethod (x11).
    // No self-recursive short-cut anymore: derivation is cheap (2 loads +
    // compute) and unconditional.  See docs/jit-j2j-reduction-plan.md.
    str     x11, [x21, #JS_JITMETHOD]
    ldr     x12, [x11, #JM_COMPILEDMETHOD]    // x12 = compiledMethodOop
    ldr     x13, [x11, #JM_METHODHEADER]      // x13 = methodHeader
    // literals = &slots[1] = compiledMethod + ObjectHeader(8) + slot0(8) = +16.
    // Previously used +8 (which lands on the method header word); that
    // corrupted the caller's literals on J2J return and crashed pushLitVar.
    add     x14, x12, #16                      // literals = compiledMethod + 16
    str     x14, [x21, #JS_LITERALS]
    and     x13, x13, #0x7FFF                  // numLits
    add     x13, x13, #2
    add     x14, x12, x13, lsl #3              // bcStart = cm + (numLits+2)*8
    str     x14, [x21, #JS_IP]
    ldrb    w12, [x11, #JM_ARGCOUNT]           // cached argCount
    str     w12, [x21, #JS_ARGCOUNT]

Lret_after_restore:
    // Pop receiver+args, push retVal onto caller's stack
    //   sp[-(sendArgCount+1)] = retVal
    //   sp -= sendArgCount
    ldr     w13, [x23, #JSV_SENDARGCOUNT]
    add     w15, w13, #1
    sub     x16, x8, w15, uxtw #3          // &retVal slot
    str     x17, [x16]                     // *slot = retVal (x17)
    sub     x14, x8, w13, uxtw #3          // final sp
    str     x14, [x21, #JS_SP]

    // Resume address already loaded into x7 by the LDP above
    cbz     x7,  Lret_null_resume

    // BLR to resumeAddr. Stencils clobber x0-x17, x19, x20, x30.
    //
    // SEND-RESUME FIX (2026-06-09): x7 is a MID-METHOD entry
    // (codeStart + bcToCode[bcOffset]) into the caller, which BYPASSES the
    // codeStart prologue that hoists x19=state.jitMethod and x20=j2jDepthInc.
    // The just-returned callee clobbered both (its own prologue set x19 to the
    // callee's jitMethod; stencils used x20).  Without restoring them, the
    // resumed caller's IC-HIT path dereferences a STALE jitMethod (x19) → wrong
    // icBuffer → "DNU on garbage class indices" / mustBeBoolean cascades — the
    // long-deferred "Phase 4b.2 send-resume protocol gap".  state.jitMethod was
    // restored to the caller's (x11) at line ~222, so reload from it; mirrors
    // pharo_jit_osr_resume, which does the identical reload for the C++ resume
    // path.  This is what makes resume-after-send safe for send-methods.
    ldr     x19, [x21, #JS_JITMETHOD]
    ldr     x20, [x21, #JS_J2J_DEPTH_INC]
    // sp-residency Phase 1 (contract half): x25 = state.sp live-in.
    // NOTE: do NOT store x25 back after the blr until the T1 emit
    // sweep maintains it — unmigrated JIT code updates JS_SP in
    // memory only, and a stale-x25 writeback would corrupt sp.
    ldr     x25, [x21, #JS_SP]
    ldur    x26, [x25, #-8]           // simStack: TOS mirror live-in
    mov     x0,  x21
    blr     x7
    str     x25, [x21, #JS_SP]        // sp-residency: x25 live-out from JIT code
    b       Ltramp_loop
#endif  // PHARO_J2J_SAVE_V2 (Ltramp_return)

Lret_null_resume:
#if PHARO_J2J_SAVE_V2
    // V2: no save.ip — leave state.ip; the C++ ExitReturn handler
    // derives the resume ip from the popped save's packed bcOffset
    // (already handled in the dual-protocol C++ pops).
#else
    // Interpreter needs post-send IP
    ldr     x12, [x23, #JSV_IP]
    str     x12, [x21, #JS_IP]
#endif
    mov     w0,  #EXIT_RETURN
    str     w0,  [x21, #JS_EXITREASON]
    b       Ltramp_exit


// ======== Call path ========
Ltramp_call:
    // Save stack full?
    cmp     x23, x27
    b.hs    Lcall_save_overflow

    ldr     x9, [sp, #120]                 // localCalls++ (frame slot — x25 freed)
    add     x9, x9, #1
    str     x9, [sp, #120]

    // Advance IP past send bytecode BEFORE saving it. Stencils set
    // state.ip = bcStart + bcOffset (pointing TO the send). We must
    // advance past it so save.ip and the resume address point to the
    // NEXT bytecode. 1-byte sends (0x80-0xAF), 2-byte (0xEA-0xEB).
    ldr     x10, [x21, #JS_IP]
    mov     x16, x10                       // stash PRE-advance ip (cascade-#3
                                           // refusal rewinds it; x16 is free
                                           // straight-line to Lresume_null)
    ldrb    w11, [x10]
    add     x10, x10, #1                   // advance 1 byte (default)
    sub     w12, w11, #0xEA
    cmp     w12, #1                         // 0xEA→0, 0xEB→1: both ≤ 1
    cinc    x10, x10, ls                    // advance 1 more if 2-byte send
    str     x10, [x21, #JS_IP]

    // Load call-path locals
    ldr     x6,  [x21, #JS_JITMETHOD]      // callerJM
    ldr     x7,  [x21, #JS_RETURNVALUE]    // entryAddr (raw bits)
    sub     x8,  x7, #JM_SIZE              // calleeJM = entryAddr - sizeof(JITMethod)
    ldr     w9,  [x21, #JS_SENDARGCOUNT]   // nArgs

    // Save caller JITState into current slot.
    // JS_SP(0) and JS_RECEIVER(8) are adjacent → LDP. JS_TEMPBASE(24) and
    // JS_IP(48) are NOT adjacent so the second pair stays split.
    ldp     x10, x11, [x21]                // x10=state.sp, x11=state.receiver
    stp     x10, x11, [x23, #JSV_SP]       // save.sp, save.receiver
#if PHARO_J2J_SAVE_V2
    ldr     x10, [x21, #JS_TEMPBASE]
    str     x10, [x23, #JSV_TEMPBASE]
    // packedResume written below once the resume address + bcOffset are
    // computed (Lresume_check stores it).
#else
    ldr     x10, [x21, #JS_TEMPBASE]
    ldr     x11, [x21, #JS_IP]
    stp     x10, x11, [x23, #JSV_TEMPBASE] // save.tempBase, save.ip
    str     w9,       [x23, #JSV_SENDARGCOUNT]

    // Save caller jitMethod. literals/argCount/bcStart are derived from
    // this on return (see Lret_after_restore).
    str     x6,  [x23, #JSV_JITMETHOD]
#endif
    // Compute callerBCStart inline — the resume-address computation
    // below needs it (both protocols).

    ldr     x10, [x6, #JM_METHODHEADER]
    and     x10, x10, #0x7FFF
    ldr     x12, [x6, #JM_COMPILEDMETHOD]  // ObjectHeader* (Oop raw bits)
    add     x10, x10, #2
    add     x11, x12, x10, lsl #3          // x11 = callerBCStart

    // Precompute resume JIT code address:
    //   bcOffset  = state.ip - callerBCStart
    //   codeOff   = (bcOffset >= numBytecodes) ? codeSize
    //                                          : bcToCodeTable[bcOffset]
    //   resume    = (codeOff == 0 || codeOff >= codeSize) ? nullptr
    //                                                     : codeStart + codeOff
    ldr     x10, [x21, #JS_IP]             // state.ip
    sub     x10, x10, x11                  // bcOffset (byte difference)
    ldrh    w12, [x6, #JM_NUMBYTECODES]    // numBytecodes (u16)
    ldr     w13, [x6, #JM_CODESIZE]        // codeSize (u32)
    cmp     x10, x12
    b.hs    Lresume_oob
    ldr     w14, [x6, #JM_BCTOCODEOFF]     // bcToCodeTableOffset (u32)
    add     x15, x6, #JM_SIZE              // codeStart = callerJM + JM_SIZE
    add     x15, x15, x14                  // bcToCodeTable pointer
    ldr     w12, [x15, x10, lsl #2]        // codeOff = table[bcOffset]
    b       Lresume_check

Lresume_oob:
    mov     w12, w13                       // codeOff = codeSize (sentinel)

Lresume_check:
    cbz     w12, Lresume_null
    cmp     w12, w13
    b.hs    Lresume_null
    add     x14, x6, #JM_SIZE              // codeStart
    add     x14, x14, w12, uxtw            // codeStart + codeOffset
#if PHARO_J2J_SAVE_V2
    // Pack bcOff (x10, bits 48-59) and nArgs (w9, bits 60-63).  Guard:
    // bcOff must fit 12 bits — oversize falls to the null (materialize)
    // path.
    cmp     x10, #0xFFF
    b.hi    Lresume_null
    orr     x14, x14, x10, lsl #48
    orr     x14, x14, x9, lsl #60
#endif
    str     x14, [x23, #JSV_RESUMEADDR]
    b       Lcall_after_resume

Lresume_null:
    // Cascade-#3 ROOT FIX (2026-06-11): a null-resume V2 save loses
    // both its resume ip and its caller identity — materialize rebuilt
    // such frames at METHOD START and the interpreter re-executed the
    // whole caller body on return.  REFUSE the J2J call instead:
    // rewind ip to the send, exit to C++ as ExitSendCached; the chain
    // loop's matching C++ refusal (same plain-table predicate) routes
    // it to the regular cached-send path with a true SavedFrame.  The
    // partially-written save slot is harmless (cursor not advanced).
    str     x16, [x21, #JS_IP]             // rewind to the send bytecode
    mov     w12, #7                        // ExitSendCached
    str     w12, [x21, #JS_EXITREASON]
    b       Ltramp_exit

Lcall_after_resume:
    // Advance save cursor now that the slot is fully written
    add     x23, x23, #JSV_SIZE
    // Sync state.j2jSaveCursor with the trampoline's local x23 so any
    // callee-side code that reads state.j2jSaveCursor (e.g. asmjit-T1's
    // inline-J2J push) sees a consistent view.  Defensive: without this
    // the asm and C++ paths could diverge.  (Doesn't fix task #8 — that
    // bug is in the materialize/dispatch interaction, not the cursor.)
    str     x23, [x21, #JS_J2J_SAVE_CURSOR]

    // Stack (frame depth) overflow check
    // 4096 fits in `cmp (imm12, lsl #12)` form: #1, lsl #12
    cmp     x24, #SO_LIMIT
    b.hs    Lcall_frame_overflow

    add     x24, x24, #1                   // localFrameDepth++

    // ---- Setup callee in state ----
    ldr     x10, [x21, #JS_SP]             // state.sp (still caller's pre-call sp)
    add     w9,  w9,  #1                   // nArgs + 1
    sub     x12, x10, w9, uxtw #3          // fp = sp - (nArgs+1)*8
    ldr     x13, [x12]                     // calleeRecv = *fp
    str     x13, [x21, #JS_RECEIVER]
    add     x12, x12, #8                   // tempBase = fp + 1
    str     x12, [x21, #JS_TEMPBASE]
    sub     w9,  w9,  #1                   // restore nArgs

    cmp     x6,  x8                        // self-recursive?
    b.eq    Lcall_setup_self_rec

    // Non-self: update literals, argCount, jitMethod, ip
    ldr     x14, [x21, #JS_CACHEDTARGET]   // methObj (== ObjectHeader*)
    add     x15, x14, #16                  // literals = methObj->slots()+1
    str     x15, [x21, #JS_LITERALS]
    str     w9,  [x21, #JS_ARGCOUNT]
    str     x8,  [x21, #JS_JITMETHOD]
    ldr     x15, [x8, #JM_METHODHEADER]
    and     x15, x15, #0x7FFF              // numLits (callee)
    add     x15, x15, #2                   // +2 for ObjectHeader + methodHeader slot
    add     x15, x14, x15, lsl #3          // state.ip = methObj + (numLits+2)*8
    str     x15, [x21, #JS_IP]
    b       Lcall_allocate_temps

Lcall_setup_self_rec:
    // Self-recursive: state.ip = callerBCStart (still in x11)
    str     x11, [x21, #JS_IP]

Lcall_allocate_temps:
    ldrb    w15, [x8,  #JM_TEMPCOUNT]      // totalTemps
    cmp     w9,  w15
    b.hs    Lcall_enter_callee

    // Push nil oops for locals (nArgs .. totalTemps-1)
    ldr     x10, [x21, #JS_SP]
Lcall_alloc_loop:
    str     x28, [x10], #8                 // *sp++ = nilOop
    add     w9,  w9, #1
    cmp     w9,  w15
    b.lo    Lcall_alloc_loop
    str     x10, [x21, #JS_SP]

Lcall_enter_callee:
    // BLR into callee JIT code; entryAddr is in x7
    // FSR M1 (FR-1, docs/frame-state-residency.md): establish the
    // x19 = callee-JITMethod hoist.  This entry previously left x19 at
    // the TRAMPOLINE-ENTRY hoist (the caller's jm) — harmless while
    // every consumer reads the OFF_JITMETHOD mirror, FATAL once FSR
    // makes x19 the canonical identity.  x8 = callee JM here (stored
    // to JS_JITMETHOD above).
    mov     x19, x8
    // sp-residency Phase 1 (contract half): x25 = state.sp live-in.
    ldr     x25, [x21, #JS_SP]
    ldur    x26, [x25, #-8]           // simStack: TOS mirror live-in
    mov     x0,  x21                       // JITState* arg
    blr     x7
    str     x25, [x21, #JS_SP]        // sp-residency: x25 live-out from JIT code
    b       Ltramp_loop


// ======== ExitSendCached → ExitJ2JCall conversion ========
// Call a C helper to check if cachedTarget is JIT-compiled. If so,
// the helper converts state to ExitJ2JCall and we branch to the call
// path. Otherwise exit to the chain loop.
// BL preserves callee-saved x19-x28, so our pinned registers survive.
Ltramp_convert:
    mov     x0, x21                         // JITState* arg
    bl      SYMBOL(pharo_jit_convert_send)
    cbz     w0, Ltramp_exit                 // conversion failed → exit
    b       Ltramp_call                     // succeeded → handle as J2JCall


// ======== Error / overflow exits ========
Lcall_save_overflow:
    // j2j save stack exhausted — bail to interpreter with ExitSendCached
    mov     w0,  #EXIT_SENDCACHED
    str     w0,  [x21, #JS_EXITREASON]
    b       Ltramp_exit

Lcall_frame_overflow:
    // frameDepth >= StackOverflowLimit — roll back the save cursor we just
    // advanced and bail with ExitStackOverflow
    sub     x23, x23, #JSV_SIZE
    mov     w0,  #EXIT_STACKOVFL
    str     w0,  [x21, #JS_EXITREASON]
    b       Ltramp_exit


// ======== Exit: write counts back and restore callee-saved regs ========
Ltramp_exit:
    // Write local state back through out-pointers
    ldr     x0,  [sp, #96]                 // frameDepthInOut
    str     x24, [x0]
    ldr     x0,  [sp, #104]                // callsCountOut
    ldr     x9,  [sp, #120]                // localCalls (frame slot)
    str     x9,  [x0]
    ldr     x0,  [sp, #112]                // returnsCountOut
    ldr     x9,  [sp, #128]                // localReturns (frame slot)
    str     x9,  [x0]

    // Restore callee-saved registers
    ldp     x19, x20, [sp, #80]
    ldp     x27, x28, [sp, #64]
    ldp     x25, x26, [sp, #48]
    ldp     x23, x24, [sp, #32]
    ldp     x21, x22, [sp, #16]
    ldp     x29, x30, [sp], #144
    ret

// extern "C" void pharo_jit_osr_resume(JITState* state, void* entry);
//
// Wrapper for tryResume's mid-method entry into JIT'd asmjit-T1 code.
// asmjit-T1's bytecode emits assume x19 = state.jitMethod and x20 =
// j2jDepthInc (the trampoline + JIT_CALL macro guarantee this).  When
// tryResume jumps directly to a mid-method codeOffset via a plain C
// call to entry(&state), x19/x20 hold the C caller's stale values —
// IC HIT loads then dereference garbage, causing SIGSEGV.
//
// This wrapper hoists the same registers as the trampoline's entry
// sequence, calls into the JIT'd code, then restores x19/x20 on
// return so the C ABI's callee-saved guarantee for the wrapper's
// caller is preserved (JIT'd code may clobber x19/x20).
    .text
    .p2align 4
    .globl SYMBOL(pharo_jit_osr_resume)
SYMBOL(pharo_jit_osr_resume):
    // x0 = JITState*  (preserved through to JIT code)
    // x1 = entry function pointer
    stp     x29, x30, [sp, #-64]!     // save fp, lr
    stp     x19, x20, [sp, #16]       // save x19/x20 (AAPCS callee-saved)
    str     x25, [sp, #32]            // save x25 (sp-residency mirror)
    str     x26, [sp, #48]            // save x26 (simStack TOS mirror)
    mov     x29, sp
    ldr     x19, [x0, #JS_JITMETHOD]
    ldr     x20, [x0, #JS_J2J_DEPTH_INC]
    // sp/tempBase residency: x25/x26 live-in/live-out.
    str     x0,  [sp, #40]            // stash state for the writeback
    ldr     x25, [x0, #JS_SP]
    ldur    x26, [x25, #-8]           // simStack: TOS mirror live-in
    blr     x1                         // call JIT'd code
    ldr     x9,  [sp, #40]
    str     x25, [x9, #JS_SP]         // writeback BEFORE restoring caller's regs
    ldr     x26, [sp, #48]            // restore x26 for our caller
    ldr     x25, [sp, #32]            // restore x25 for our caller
    ldp     x19, x20, [sp, #16]       // restore x19/x20 for our caller
    ldp     x29, x30, [sp], #64
    ret

// Cross-TU sentinel: a uint64_t holding our JM_SIZE constant.
// JITRuntime::initialize() compares this against sizeof(JITMethod)
// at startup so the asm constant can never silently drift out of
// sync with the C++ struct again.  Bug 11b layer 5.
.data
.globl SYMBOL(pharo_jit_jm_size_check)
.p2align 3
SYMBOL(pharo_jit_jm_size_check):
    .quad   JM_SIZE

#endif // __aarch64__ && PHARO_JIT_ENABLED
