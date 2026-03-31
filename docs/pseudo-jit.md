# Pseudo-JIT: Closing the Interpreter-JIT Gap Without Runtime Code Generation

Apple prohibits JIT on iOS (no writable+executable memory). Our VM is ~94x
slower than the Cog JIT. This document surveys techniques that other iOS VM
projects use and outlines our implementation plan.

## Current State (Build 113)

    CPU time:    16.88s (full test suite, 27,968 tests)
    Reference:   0.18s (Cog JIT)
    Ratio:       ~94x

The gap is NOT uniform:
- Classes where reference VM takes >= 1ms: we are only 1.5x slower
- Loop-heavy classes (22 of them): 100-200x slower
- These 22 classes account for 95% of total time

Dispatch overhead in tight loops is the primary target.

## What Real iOS VM Projects Do

    Project              Technique                          Notes
    JavaScriptCore       LLInt (offline-compiled asm)       No JIT on iOS, falls back to interpreter
    LuaJIT               Hand-written ARM64 asm interpreter Already extremely fast without JIT
    Luau (Roblox)        Specialized bytecodes + IC         Matches LuaJIT interpreter speed
    Wasm3                Tail-call threading                Fastest Wasm interpreter, designed for no-JIT
    .NET MAUI            Mono AOT                          Works because C# is statically typed
    Dart/Flutter         AOT native code                   Works because Dart has static types

None of these use vtable dispatch. All use some form of threaded dispatch
or specialized bytecodes.

## Why Vtable Dispatch is Worse Than Switch

    Switch:  1 byte load -> 1 table lookup -> 1 indirect branch  (2 dependent loads)
    Vtable:  1 ptr load -> 1 object deref -> 1 vtable load -> 1 table lookup -> 1 branch  (4 dependent loads)

The vtable adds 2 extra pointer chases per dispatch. Branch prediction is
identical (one indirect branch either way). The flat record / pre-decoded
IR approach has merit, but the benefit comes from pre-resolution of operands,
not from the dispatch mechanism.

## Implementation Plan

### Phase 1: Computed Goto Dispatch (DONE — measured 2.5%)

Implemented computed goto (GCC/Clang &&label extension) in interpret().
Fast-path handlers for ~200 of 256 bytecodes (push/pop/store/jump/
SmallInt arithmetic/literal sends). Slow path for complex bytecodes
(returns, closures, extensions) via existing dispatchBytecode().

Result: 2.5% CPU improvement (18.44s → 18.00s, 3-run avg).
Below the 10-25% estimate because Apple M1's branch predictor already
handles the switch jump table well. The original tail-call threading
proposal (below) would not improve on this — the benefit comes from
per-handler branch entries, which computed goto already provides.

Original proposal was tail-call-threaded functions:

    static void op_pushTemp(InterpreterContext* ctx) {
        int index = ctx->ip[1];
        ctx->sp[0] = ctx->temps[index];
        ctx->sp++;
        ctx->ip += 2;
        // [[clang::musttail]] return handlers[*ctx->ip](ctx);
        dispatch(ctx);
    }

Why it helps:
- Compiler keeps IP, SP, receiver in registers across bytecodes
- Current switch forces reload from Interpreter object on each bytecode
- Each handler gets its own branch predictor entry (vs one shared entry)
- This is what Wasm3 and CPython 3.14 do

Requirements:
- Clang with [[clang::musttail]] (available since Clang 13, Xcode ships this)
- InterpreterContext struct with hot state (ip, sp, receiver, method)
- Pointer back to full Interpreter object for cold-path operations

### Phase 2: Quickening / Specialized Bytecodes (est. 20-40% improvement)

Replace generic bytecodes with type-specialized variants at runtime based
on observed types. This is NOT JIT — we rewrite bytecodes, not machine code.

After seeing SmallInteger + SmallInteger N times, rewrite in place:

    Original:    0x60 (sendArithmetic +)
    Quickened:   0xNN (SEND_ADD_SMALLINT — no method lookup, no type check)

What can be specialized:
- Arithmetic on SmallIntegers (skip method lookup entirely)
- Instance variable access (resolve slot offset once)
- Monomorphic sends (cache looked-up method in bytecode stream)
- Boolean jumps (skip mustBeBoolean check)

Sista V1's trap bytecode (0xD9) was designed for exactly this kind of
runtime patching. CPython 3.11+ does this (PEP 659). Luau does this.

Must un-quicken before image snapshot (primitive 97).

### Phase 3: Superinstructions (DONE — measured 2.8%)

Profiled all 65536 bytecode pairs over 100M bytecodes. Implemented
speculative fusion in computed goto handlers (peek at next bytecode):

    SmallInt comparison + jump   -> branch directly, skip boolean (5.3M pairs)
    push1 + arith+               -> inline x+1 (2.2M, 65% hit)
    pushNil + spec==             -> inline nil check (1.7M, 46% hit)
    dup + pushNil + == + jump    -> full nil-check idiom (1.45M, 96% hit)
    spec== fast handler          -> inline identity compare (2.4M)

Result: 2.8% CPU improvement (18.00s → 17.49s, 3-run avg).
Below the 15-30% estimate because Apple M1's branch predictor handles
the dispatch jump table so efficiently that eliminating dispatches
barely matters. The Ertl & Gregg 3.17x result was on x86 with weaker
branch prediction. Dispatch overhead is a smaller fraction of total
time than the gap breakdown estimated.

### Phase 4: Pre-Decoded IR / Flat Record (est. 10-20% improvement)

On method activation, translate Sista V1 bytecodes to fixed-width records:
- Resolve extension bytes (ExtA, ExtB) into wide operands
- Convert literal indices to direct pointers
- Convert variable-length instructions to fixed-width (4 or 8 bytes)

Eliminates:
- Extension byte overhead (each extended instruction costs 2-3 dispatches)
- Literal table indirection (fetchPointer on every access)
- Variable-width decoding

Design choice: 4-byte records (opcode + 24-bit operand) or 8-byte records
(opcode + pad + 32-bit operand + 16-bit aux).

### Phase 5: C++ Fast Paths for Hot Kernel Methods (DONE — measured 6.4%)

Trivial getter/setter inlining: detect accessor methods at method cache
time and bypass Smalltalk activation entirely on cache hit:
- Getter: pushRecvVar N + returnTop → replace receiver with inst var
- Setter: popStoreRecvVar N + returnReceiver → store arg, return self

Result: 6.4% CPU improvement (17.49s → 16.37s, 3-run avg). This is the
single most effective optimization since the method cache improvements,
because it eliminates real work (stack frame push/pop) rather than just
dispatch overhead.

Additional fast paths not yet implemented:
- Boolean >> ifTrue:ifFalse: (skip method lookup, inline branch)
- Array >> at: / at:put: (bounds check + direct slot access)
- SmallInteger >> to:do: (loop without message sends)

## What Cannot Be Pre-Resolved (Smalltalk Dynamism)

- Polymorphic sends (receiver class varies at runtime)
- doesNotUnderstand: interception
- Method replacement (live code editing)
- become: (object identity swap)
- Proxy / read-barrier objects

These must remain dynamic dispatch. The optimization targets the 80% of
sends that are monomorphic and type-stable.

## The 94x Gap Breakdown

    Component                          Est. share    Target phase
    Bytecode dispatch overhead         ~60%          Phases 1, 3, 4
    Method lookup (cache miss)         ~15%          Phase 2 (quickening)
    Method lookup (cache hit)          ~10%          Phase 1 (register alloc)
    Stack frame push/pop               ~10%          Phase 5 (bypass frames)
    Type checking / guards             ~5%           Phase 2 (specialization)

Realistic ceiling: with all phases implemented, expect 5-15x improvement
over current interpreter, bringing the gap from 94x to roughly 6-20x vs
the Cog JIT. The remaining gap is the fundamental cost of interpreting vs
executing native code.

## References

- JavaScriptCore LLInt: wingolog.org/archives/2012/06/27/
- Luau performance: luau.org/performance/
- Wasm3 design: github.com/wasm3/wasm3/blob/main/docs/Interpreter.md
- CPython 3.14 tail-call: blog.nelhage.com/post/cpython-tail-call/
- PEP 659 specialization: peps.python.org/pep-0659/
- Deegen (fastest Lua interpreter): sillycross.github.io/2022/11/22/
- Ertl & Gregg (dispatch techniques): complang.tuwien.ac.at/andi/papers/
- Context threading (Berndl 2005): dl.acm.org/doi/10.1109/CGO.2005.14
- Sista speculative optimization: dl.acm.org/doi/pdf/10.1145/3132190.3132201
- Copy-and-patch: fredrikbk.com/publications/copy-and-patch.pdf
