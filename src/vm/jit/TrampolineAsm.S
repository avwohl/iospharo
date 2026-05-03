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

// -------- J2JSave field offsets (see Interpreter.hpp struct J2JSave) --------
// 56 bytes after dropping literals/argCount/bcStart — those are derived from
// save.jitMethod on return. See docs/jit-j2j-reduction-plan.md.
#define JSV_SP            0
#define JSV_RECEIVER      8
#define JSV_TEMPBASE      16
#define JSV_IP            24
#define JSV_JITMETHOD     32
#define JSV_RESUMEADDR    40
#define JSV_SENDARGCOUNT  48
#define JSV_SIZE          56

// -------- JITMethod field offsets (see src/vm/jit/JITMethod.hpp) --------
#define JM_COMPILEDMETHOD 0
#define JM_METHODHEADER   16
#define JM_CODESIZE       24
#define JM_NUMBYTECODES   30
#define JM_ARGCOUNT       34   // uint8_t cached from method header
#define JM_TEMPCOUNT      35
#define JM_BCTOCODEOFF    52   // bcToCodeTableOffset, see JITMethod.hpp
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
#define JM_SIZE           96

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
    // Stack frame (128 bytes):
    //   [sp, #0]:   x29 (old fp),  x30 (lr)
    //   [sp, #16]:  x21,           x22
    //   [sp, #32]:  x23,           x24
    //   [sp, #48]:  x25,           x26
    //   [sp, #64]:  x27,           x28
    //   [sp, #80]:  x19,           x20     (stencils clobber; must preserve caller's)
    //   [sp, #96]:  frameDepthPtr, callsCountPtr
    //   [sp, #112]: returnsPtr,    (pad)
    stp     x29, x30, [sp, #-128]!
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
    //   x25 = localCalls
    //   x26 = localReturns
    //   x27 = save base + MAX_J2J_BYTES (overflow comparison limit)
    //   x28 = nilOop bits
    mov     x21, x0
    mov     x22, x1
    mov     x23, x1                   // cursor = base (j2jDepth = 0)
    ldr     x24, [x2]                 // localFrameDepth = *frameDepthInOut
    mov     x25, xzr                  // localCalls = 0
    mov     x26, xzr                  // localReturns = 0
    mov     x27, #MAX_J2J_BYTES       // 18432 fits in movz (16-bit immediate)
    add     x27, x1, x27
    mov     x28, x5                   // nil Oop bits

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

    add     x26, x26, #1              // localReturns++
    sub     x23, x23, #JSV_SIZE       // pop save slot; now points to just-popped
    sub     x24, x24, #1              // localFrameDepth--

    // retVal lives in x17 (caller-saved IP1) — we never cross a BLR between
    // this load and the store below, so no spill needed. Frees x7 to be
    // loaded early as the precomputed resume address.
    ldr     x17, [x21, #JS_RETURNVALUE]   // retVal

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
    mov     x0,  x21
    blr     x7
    b       Ltramp_loop

Lret_null_resume:
    // Interpreter needs post-send IP
    ldr     x12, [x23, #JSV_IP]
    str     x12, [x21, #JS_IP]
    mov     w0,  #EXIT_RETURN
    str     w0,  [x21, #JS_EXITREASON]
    b       Ltramp_exit


// ======== Call path ========
Ltramp_call:
    // Save stack full?
    cmp     x23, x27
    b.hs    Lcall_save_overflow

    add     x25, x25, #1                   // localCalls++

    // Advance IP past send bytecode BEFORE saving it. Stencils set
    // state.ip = bcStart + bcOffset (pointing TO the send). We must
    // advance past it so save.ip and the resume address point to the
    // NEXT bytecode. 1-byte sends (0x80-0xAF), 2-byte (0xEA-0xEB).
    ldr     x10, [x21, #JS_IP]
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
    ldr     x10, [x21, #JS_TEMPBASE]
    ldr     x11, [x21, #JS_IP]
    stp     x10, x11, [x23, #JSV_TEMPBASE] // save.tempBase, save.ip
    str     w9,       [x23, #JSV_SENDARGCOUNT]

    // Save caller jitMethod. literals/argCount/bcStart are derived from
    // this on return (see Lret_after_restore).  Compute callerBCStart
    // inline since the resume-address computation below needs it.
    str     x6,  [x23, #JSV_JITMETHOD]

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
    str     x14, [x23, #JSV_RESUMEADDR]
    b       Lcall_after_resume

Lresume_null:
    str     xzr, [x23, #JSV_RESUMEADDR]

Lcall_after_resume:
    // Advance save cursor now that the slot is fully written
    add     x23, x23, #JSV_SIZE

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
    mov     x0,  x21                       // JITState* arg
    blr     x7
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
    str     x25, [x0]
    ldr     x0,  [sp, #112]                // returnsCountOut
    str     x26, [x0]

    // Restore callee-saved registers
    ldp     x19, x20, [sp, #80]
    ldp     x27, x28, [sp, #64]
    ldp     x25, x26, [sp, #48]
    ldp     x23, x24, [sp, #32]
    ldp     x21, x22, [sp, #16]
    ldp     x29, x30, [sp], #128
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
