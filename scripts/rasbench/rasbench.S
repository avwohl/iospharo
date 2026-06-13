// rasbench.S — measure bl/ret (RAS-predicted) vs br-based calls with a
// side stack (the J2J pattern) on a fib-shaped recursion, the same call
// pattern as the cfib benchmark.
//
// uint64_t fib_blret(uint64_t n);
//   classic: bl recursion, ret returns.  RAS predicts every return.
//
// uint64_t fib_brbr(uint64_t n, void* sideStackTop);
//   J2J-style: "call" = push {resume, n, partial} to a side stack
//   (x28), b to the callee entry; "return" = unpack the top frame
//   into registers, pop, br resume — exactly the V2 return shape.
//   The return-address stack never sees these, so every return is an
//   indirect-branch BTB guess.
//
// Both compute fib(n) with fib(0)=fib(1)=1, result in x0.

.text
.align 4

.globl _fib_blret
_fib_blret:
    stp x29, x30, [sp, #-16]!
    cmp x0, #2
    b.hs 1f
    mov x0, #1
    ldp x29, x30, [sp], #16
    ret
1:
    stp x19, x20, [sp, #-16]!
    mov x19, x0
    sub x0, x0, #1
    bl  _fib_blret
    mov x20, x0
    sub x0, x19, #2
    bl  _fib_blret
    add x0, x0, x20
    ldp x19, x20, [sp], #16
    ldp x29, x30, [sp], #16
    ret

// Shared J2J-style return sequence: unpack frame -> registers,
// pop, indirect br.  x0 = return value.
.macro J2J_RETURN
    ldr x9,  [x28]          // resume addr
    ldr x10, [x28, #8]      // caller's n
    ldr x11, [x28, #16]     // caller's partial
    add x28, x28, #24       // pop
    br  x9
.endm

.globl _fib_brbr_entry
_fib_brbr_entry:
    cmp x0, #2
    b.hs 1f
    mov x0, #1
    J2J_RETURN
1:
    // push our frame {Lresume1, n, -}; recurse on n-1
    sub x28, x28, #24
    adr x9, Lresume1
    str x9, [x28]
    str x0, [x28, #8]
    sub x0, x0, #1
    b   _fib_brbr_entry
Lresume1:
    // x0 = fib(n-1), x10 = our n (unpacked by callee's return)
    sub x28, x28, #24
    adr x9, Lresume2
    str x9, [x28]
    str x0, [x28, #16]      // partial
    sub x0, x10, #2
    b   _fib_brbr_entry
Lresume2:
    // x0 = fib(n-2), x11 = partial
    add x0, x0, x11
    J2J_RETURN

// uint64_t fib_brbr(uint64_t n, void* sideStackTop);
.globl _fib_brbr
_fib_brbr:
    stp x29, x30, [sp, #-16]!
    stp x28, x27, [sp, #-16]!
    mov x28, x1
    sub x28, x28, #24
    adr x9, Lland
    str x9, [x28]
    b   _fib_brbr_entry
Lland:
    ldp x28, x27, [sp], #16
    ldp x29, x30, [sp], #16
    ret

// Variant C: br/br skeleton + simulated V2 send machinery per call:
//   - IC probe: 3 dependent loads (site ptr -> key -> cmp) + extras
//     load + tbnz dispatch  (mirrors icBuffer probe + bit-60 check)
//   - side-pool bookkeeping: cursor ldr/sub/str + depth ldr/add/str
//     (the two read-modify-write chains in the real J2J call)
// Frames stay 24B on x28; x27 = fake JITState base (cursor/depth/etc).
// Two static "IC sites" (one per call site) hold {key, extras}.

.data
.align 6
Lic_site1: .quad 17, 0x1000000000000000   // key=17 ("class"), extras bit60
Lic_site2: .quad 17, 0x1000000000000000
Lstate:    .quad 0, 0, 0, 0, 0, 0, 0, 0, 17, 4096   // cursor, depth, [16..56]=mirror, [64]=fake CM, [72]=fake bcStart

.text
.align 4

// x0=n, x28=side stack, x27=&Lstate
.macro SEND_MACHINERY site
    adrp x12, \site@PAGE
    add  x12, x12, \site@PAGEOFF    // load 1: IC site address (stand-in
                                    // for the icDataPtr fetch)
    ldr  x13, [x12]                 // load 2: key
    cmp  x13, #17                   // class compare (always hits)
    b.ne 9f
    ldr  x13, [x12, #8]             // load 3: extras
    tbz  x13, #60, 9f               // dispatch bit
    // side-pool bookkeeping RMWs
    ldr  x14, [x27]                 // cursor
    sub  x14, x14, #32
    str  x14, [x27]
    ldr  x15, [x27, #8]             // depth
    add  x15, x15, #1
    str  x15, [x27, #8]
.endm

.globl _fib_ic_entry
_fib_ic_entry:
    cmp x0, #2
    b.hs 1f
    mov x0, #1
    J2J_RETURN
1:
    SEND_MACHINERY Lic_site1
    sub x28, x28, #24
    adr x9, Lic_resume1
    str x9, [x28]
    str x0, [x28, #8]
    sub x0, x0, #1
    b   _fib_ic_entry
Lic_resume1:
    SEND_MACHINERY Lic_site2
    sub x28, x28, #24
    adr x9, Lic_resume2
    str x9, [x28]
    str x0, [x28, #16]
    sub x0, x10, #2
    b   _fib_ic_entry
Lic_resume2:
    add x0, x0, x11
    J2J_RETURN
9:  brk #0

// uint64_t fib_ic(uint64_t n, void* sideStackTop);
.globl _fib_ic
_fib_ic:
    stp x29, x30, [sp, #-16]!
    stp x28, x27, [sp, #-16]!
    mov x28, x1
    adrp x27, Lstate@PAGE
    add  x27, x27, Lstate@PAGEOFF
    sub x28, x28, #24
    adr x9, Lic_land
    str x9, [x28]
    b   _fib_ic_entry
Lic_land:
    ldp x28, x27, [sp], #16
    ldp x29, x30, [sp], #16
    ret

// Variant D: variant C + the per-call JITState MIRROR stores the real
// J2J call performs (receiver/method/literals/argCount/ip/tempBase +
// a header-derived load feeding two of them) — the frame-state-
// residency lever's target.  Measures the mirror's isolated cost.
.globl _fib_mir_entry
_fib_mir_entry:
    cmp x0, #2
    b.hs 1f
    mov x0, #1
    J2J_RETURN
1:
    SEND_MACHINERY Lic_site1
    ldr  x16, [x27, #64]        // calleeJM->compiledMethodOop stand-in
    str  x16, [x27, #16]        // state.method
    add  x16, x16, #16
    str  x16, [x27, #24]        // state.literals
    mov  w16, #0
    str  w16, [x27, #32]        // state.argCount
    str  x0,  [x27, #40]        // state.receiver
    str  x28, [x27, #48]        // state.tempBase
    ldr  x16, [x27, #72]        // calleeJM->bcStartCache stand-in
    str  x16, [x27, #56]        // state.ip
    sub x28, x28, #24
    adr x9, Lmir_resume1
    str x9, [x28]
    str x0, [x28, #8]
    sub x0, x0, #1
    b   _fib_mir_entry
Lmir_resume1:
    SEND_MACHINERY Lic_site2
    ldr  x16, [x27, #64]
    str  x16, [x27, #16]
    add  x16, x16, #16
    str  x16, [x27, #24]
    mov  w16, #0
    str  w16, [x27, #32]
    str  x0,  [x27, #40]
    str  x28, [x27, #48]
    ldr  x16, [x27, #72]
    str  x16, [x27, #56]
    sub x28, x28, #24
    adr x9, Lmir_resume2
    str x9, [x28]
    str x0, [x28, #16]
    sub x0, x10, #2
    b   _fib_mir_entry
Lmir_resume2:
    add x0, x0, x11
    J2J_RETURN
9:  brk #0      // SEND_MACHINERY miss target (never taken)

// uint64_t fib_mir(uint64_t n, void* sideStackTop);
.globl _fib_mir
_fib_mir:
    stp x29, x30, [sp, #-16]!
    stp x28, x27, [sp, #-16]!
    mov x28, x1
    adrp x27, Lstate@PAGE
    add  x27, x27, Lstate@PAGEOFF
    sub x28, x28, #24
    adr x9, Lmir_land
    str x9, [x28]
    b   _fib_mir_entry
Lmir_land:
    ldp x28, x27, [sp], #16
    ldp x29, x30, [sp], #16
    ret
