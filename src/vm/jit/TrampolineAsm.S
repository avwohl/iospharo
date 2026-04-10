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

#if defined(__aarch64__)

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

// -------- J2JSave field offsets (see Interpreter.cpp struct J2JSave) --------
#define JSV_SP            0
#define JSV_RECEIVER      8
#define JSV_TEMPBASE      16
#define JSV_IP            24
#define JSV_JITMETHOD     32
#define JSV_RESUMEADDR    40
#define JSV_SENDARGCOUNT  48
#define JSV_ARGCOUNT      52
#define JSV_LITERALS      56
#define JSV_BCSTART       64
#define JSV_SIZE          72

// -------- JITMethod field offsets (see src/vm/jit/JITMethod.hpp) --------
#define JM_COMPILEDMETHOD 0
#define JM_METHODHEADER   16
#define JM_CODESIZE       24
#define JM_NUMBYTECODES   30
#define JM_TEMPCOUNT      35
#define JM_BCTOCODEOFF    76
#define JM_SIZE           80

// -------- ExitReason values --------
#define EXIT_RETURN       1
#define EXIT_STACKOVFL    5
#define EXIT_SENDCACHED   7
#define EXIT_J2JCALL      10

// Maximum J2J save stack depth (matches MaxJ2JDepth in Interpreter.cpp)
#define MAX_J2J_BYTES     18432    // 256 * 72

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
    cmp     w0, #EXIT_RETURN
    b.ne    Ltramp_exit
    cmp     x23, x22                  // j2jDepth > 0 ?
    b.eq    Ltramp_exit               // depth == 0 → exit
    // Fall through to return path

// ======== Return path ========
Ltramp_return:
    add     x26, x26, #1              // localReturns++
    sub     x23, x23, #JSV_SIZE       // pop save slot; now points to just-popped
    sub     x24, x24, #1              // localFrameDepth--

    // retVal lives in x17 (caller-saved IP1) — we never cross a BLR between
    // this load and the store below, so no spill needed. Frees x7 to be
    // loaded early as the precomputed resume address.
    ldr     x17, [x21, #JS_RETURNVALUE]   // retVal

    // Load save fields. JSV_JITMETHOD(32) and JSV_RESUMEADDR(40) are
    // adjacent — LDP hoists the resume address load all the way up here so
    // the load latency is fully hidden behind the self-recursive restore.
    ldp     x8, x9, [x23, #JSV_SP]         // x8=save.sp, x9=save.receiver
    ldr     x10, [x23, #JSV_TEMPBASE]      // x10=save.tempBase
    ldp     x11, x7, [x23, #JSV_JITMETHOD] // x11=save.jitMethod, x7=save.resumeAddr

    str     x9,  [x21, #JS_RECEIVER]
    str     x10, [x21, #JS_TEMPBASE]

    // Bit 0 set ⇒ self-recursive ⇒ skip literals/jitMethod/argCount/ip restore
    tbnz    x11, #0, Lret_after_restore

    // Non-self-recursive: restore caller's literals/jitMethod/argCount/ip
    ldr     x12, [x23, #JSV_LITERALS]
    str     x12, [x21, #JS_LITERALS]
    str     x11, [x21, #JS_JITMETHOD]
    ldr     w12, [x23, #JSV_ARGCOUNT]
    str     w12, [x21, #JS_ARGCOUNT]
    ldr     x12, [x23, #JSV_BCSTART]
    str     x12, [x21, #JS_IP]

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

    // Compute callerBCStart:
    //   numLits = callerJM->methodHeader & 0x7FFF
    //   callerBCStart = (uint8_t*)callerJM->compiledMethodOop
    //                   + sizeof(ObjectHeader=8)
    //                   + (numLits + 1) * 8
    //                 = compiledMethodOop + (numLits + 2) * 8
    ldr     x10, [x6, #JM_METHODHEADER]
    and     x10, x10, #0x7FFF
    ldr     x12, [x6, #JM_COMPILEDMETHOD]  // ObjectHeader* (Oop raw bits)
    add     x10, x10, #2
    add     x11, x12, x10, lsl #3          // x11 = callerBCStart

    // Self-recursive test: callerJM == calleeJM ?
    cmp     x6,  x8
    b.eq    Lcall_save_self_rec

    // Non-self-recursive save: jitMethod, literals, argCount, bcStart
    str     x6,  [x23, #JSV_JITMETHOD]
    ldr     x10, [x21, #JS_LITERALS]
    str     x10, [x23, #JSV_LITERALS]
    ldr     w10, [x21, #JS_ARGCOUNT]
    str     w10, [x23, #JSV_ARGCOUNT]
    str     x11, [x23, #JSV_BCSTART]
    b       Lcall_compute_resume

Lcall_save_self_rec:
    // save.jitMethod = callerJM | 1
    orr     x10, x6, #1
    str     x10, [x23, #JSV_JITMETHOD]
    // literals/argCount/bcStart not saved for self-recursive path

Lcall_compute_resume:
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
    add     x15, x6, #JM_SIZE              // codeStart = callerJM + 80
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

#endif // __aarch64__
