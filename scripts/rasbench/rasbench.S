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
