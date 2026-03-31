# Pseudo-JIT: Closing the Interpreter-JIT Gap Without Runtime Code Generation

Apple prohibits JIT on iOS (no writable+executable memory). Our VM is ~94x
slower than the Cog JIT. This document surveys techniques that other iOS VM
projects use and outlines our implementation plan.

## Current State (Build 115)

    CPU time:    ~12.5s (full test suite, 27,968 tests, build 115 w/ quit fix)
    Reference:   0.18s (Cog JIT)
    Ratio:       ~69x (down from ~94x at build 113, ~445x at build ecd0d70)

    Optimization history (CPU time, full test suite):
    Build 110  cleanup baseline       ~23s (est)
    Build 111  flat switch            20.81s
    Build 112  fast interpret loop    16.88s    (-19%)
    Build 113  2-way method cache     16.88s    (measured here)
    Build 114  computed goto          18.00s*   (+2.5%)
    Build 115  superinstructions      17.49s*   (+2.8%)
    Build 115  accessor inlining      16.37s*   (+6.4%)
    Build 115  returnsSelf            ~12.5s**  (noise)

    * benchmark script changed (time_tests.st → startup.st, different overhead)
    ** startup.st with quit fix, lower overhead than time_tests.st

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

Also added: returnsSelf detection for methods whose only bytecode is
returnReceiver (0x58). Covers `yourself` and identity methods. No
measurable impact (within noise) — these methods are too rare in hot paths.

Note: Boolean ifTrue:ifFalse: and SmallInteger to:do: are already compiled
to inline bytecodes (conditional jumps, loop bytecodes) by the Sista V1
compiler. They don't generate message sends, so they can't be optimized
at the interpreter level. Array at:/at:put: could still benefit.

## What Cannot Be Pre-Resolved (Smalltalk Dynamism)

- Polymorphic sends (receiver class varies at runtime)
- doesNotUnderstand: interception
- Method replacement (live code editing)
- become: (object identity swap)
- Proxy / read-barrier objects

These must remain dynamic dispatch. The optimization targets the 80% of
sends that are monomorphic and type-stable.

## The Gap Breakdown (revised with measurements)

Original estimates vs actual measurements from Phases 1, 3, 5:

    Component                 Est. share   Measured        Notes
    Bytecode dispatch         ~60%         ~5% (P1+P3)    M1 branch predictor handles this
    Method lookup (miss)      ~15%         ~19% (cache)   2-way cache was the big win
    Method lookup (hit)       ~10%         (included above)
    Stack frame push/pop      ~10%         ~6.4% (P5)     Accessor inlining confirmed this
    Type checking / guards    ~5%          not yet tested  Target for Phase 2

Key insight: dispatch overhead was vastly overestimated. On Apple M1, the
branch predictor handles indirect jumps (switch/computed goto) so well that
software dispatch optimizations yield only 2-5%. The real bottleneck is
method lookup and activation, not bytecode dispatch.

## Tight Loop Performance (build 116)

Measured 100M iterations of `[n < 100M] whileTrue: [total := total + n. n := n + 1]`:

    100M iter: 7.97s total, 1.77s for loop body (subtract 6.20s startup overhead)
    Rate:      53.7M iterations/sec, 18.6ns/iteration
    Cycles:    ~59.5 cycles per iteration (~13 bytecodes) = ~4.6 cycles/bytecode

This is competitive with the best interpreters:
    LuaJIT interpreter:  ~3 cycles/bytecode
    Wasm3:               ~5 cycles/bytecode
    Our VM:              ~4.6 cycles/bytecode (tight SmallInt loop)
    CPython 3.14:        ~20 cycles/bytecode

The 69x gap vs Cog JIT is NOT from interpreter overhead in tight loops.
It comes from:
1. Startup/framework code (SDL2 bootstrap, GC, SessionManager) — dominates
   small benchmarks like the test suite where each class runs <1ms
2. Non-inline sends to polymorphic/complex methods — full send path
3. Block closures that the compiler doesn't inline (non-trivial blocks)

Phase 2 quickening would help #2 but not #1 or #3. Estimated revised
ceiling: 10-20% additional improvement from quickening non-arithmetic
monomorphic sends, bringing the overall gap from ~69x to ~55-60x.
The remaining gap is fundamental: interpreting vs native code.

For the test suite specifically, the 69x ratio is misleading because the
reference JIT finishes each test class in <1ms while our interpreter takes
150-213ms for the same class — but that's dominated by constant per-class
overhead (startup patches, class enumeration), not per-test computation.

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
