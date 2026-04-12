# Tier 2 JIT Plan: Register Allocation, Native Frames, Inlining

Date: 2026-04-12


## Where We Are

Tier 1 copy-and-patch JIT is complete. AWFY benchmarks show 5-20x slower
than Cog. The gap is structural, not tunable — we have spent two sessions
trying to squeeze bytes out of sendJ2J and every approach either regresses
or is neutral. The ceiling is the copy-and-patch architecture itself:

    What we do                     What Cog does
    Concatenate pre-compiled       Compile each method as a unit
      stencils per bytecode          with cross-bytecode optimization
    All values flow through        Values live in registers across
      JITState in memory             bytecode boundaries
    Sends go through 4-way IC      Sends are native call/ret with
      probe in the stencil            hardware return prediction
    No inlining (trivial methods   Getters/setters/small methods
      inlined at IC level only)       inlined at compile time

The three things that matter, in order of impact:

    1. Register allocation      Eliminates load/store traffic between
                                bytecodes. 2-4x fewer memory ops.
    2. Native call frames       Hardware return prediction, smaller
                                frame save/restore, standard ABI.
    3. Compile-time inlining    Eliminates send overhead entirely
                                for known monomorphic targets.


## Strategy: Two Tracks

    Track A   Extend Tier 1 copy-and-patch (incremental, low risk)
              - More SimStack registers (x19-x22, 4 regs)
              - Compile-time inlining via stencil concatenation
              - Monomorphic send fast-path stencil

    Track B   New Tier 2 backend for hot methods (major, high reward)
              - SLJIT-based code generator
              - Full register allocation (linear scan)
              - Native call/ret frames
              - Method inlining with type guards
              - Triggered by execution counter (hot methods only)

Track A can ship incrementally. Track B is the long-term path to Cog
parity. They are independent — Track A improves Tier 1 for all methods,
Track B replaces Tier 1 for hot methods.


---


## Track A: Extend Tier 1

### A1. Extended SimStack (4 registers)

Current SimStack caches TOS in x19 and NOS in x20. This is net-zero on
send-dominated code because every send flushes. But for straight-line
code (arithmetic, array access, comparisons), 2 registers help.

Extending to 4 registers (x19-x22) helps more:

    Before:  pushTemp 0 → [mem]     After:  pushTemp 0 → x19
             pushTemp 1 → [mem]             pushTemp 1 → x20
             pushTemp 2 → [mem]             pushTemp 2 → x21
             send #+                        send #+  (flush x19-x21)

    Before: 3 stores + 3 loads     After: 0 stores + 1 flush
    per send (3 pushes + IC lookup)        (3 direct register reads)

Implementation:

    1. Add stencil variants for states E/1/2/3/4 (5 states)
       For each push/pop/dup/store bytecode: 5 variants
       ~50 new stencil functions in stencils.cpp

    2. Add flush stencils: flush1, flush2, flush3, flush4
       flush4 writes x22/x21/x20/x19 to stack in one sequence

    3. Extend applySimStack to track 5-state machine
       State transitions: push increments, pop decrements,
       send/return/branch-target flushes to 0

    4. Register assignment: x19=deepest, x22=TOS
       (or TOS=x19, NOS=x20, etc. — measure both)

    5. extract_stencils.py: extend STP/LDP check to x19-x22

Risk: stencil count grows from ~141 to ~191 (+50). Compile time grows
but runtime is unaffected for methods that don't benefit.

Expected gain: 5-15% on arithmetic-heavy benchmarks (Mandelbrot, Sieve,
Permute). Zero effect on send-heavy benchmarks (Richards, DeltaBlue).

Effort: ~2 days.


### A2. Compile-Time Inlining (stencil concatenation)

For monomorphic send sites where the target is a small known method
(getter, setter, 1-3 bytecodes), inline the target's stencils directly
into the caller's compiled code. No send stencil emitted at all.

    Before:                    After:
    stencil_sendJ2J            stencil_pushRecvVar (inlined getter)
    (1372 bytes per send)      (32 bytes)

Scope: only methods resolvable at compile time:

    - Getters (1 bytecode: pushRecvVar + return)
    - Setters (3 bytecodes: store + pop + pushSelf + return)
    - returnsSelf (1 bytecode: pushSelf + return)
    - Known arithmetic on SmallInteger (if receiver type is proven)

Implementation:

    1. At compile time, for each send bytecode:
       a. Look up selector in the receiver's class hierarchy
       b. If exactly one implementor (monomorphic) and method is small:
          - Decode the target's bytecodes
          - Emit the target's stencils in-place instead of sendJ2J
          - Remap operands (receiver slot indices, literals)
          - Replace target's return stencil with a NOP/continue

    2. Add a class hierarchy query to ObjectMemory:
       `Oop uniqueImplementorOf(Oop selector, Oop receiverClass)`
       Returns nil if polymorphic, the method if monomorphic.

    3. Guard check: before the inlined code, emit a class check:
       `if (classOf(receiver) != expectedClass) goto deopt_send;`
       deopt_send emits a regular sendJ2J as fallback.

    4. Invalidation: when a class is modified (new method installed,
       subclass added), mark affected JIT methods for recompilation.
       Use the existing IC flush infrastructure.

Risk: class hierarchy changes require invalidation. Deopt path adds
code size. Only helps if sends are actually monomorphic at the sites
where we inline.

Expected gain: 10-30% on getter/setter-heavy code (Richards has many
accessor sends). For each inlined getter, we save ~1340 bytes of
sendJ2J stencil and the IC probe + J2J overhead.

Effort: ~1 week. The hard part is the class hierarchy query and
invalidation, not the stencil concatenation.


### A3. Monomorphic Send Fast-Path

For send sites that are empirically monomorphic (1 class seen), add a
smaller stencil that checks just 1 class instead of probing 4 IC slots:

    stencil_sendMono (JITState* s) {
        Oop receiver = s->sp[-(nArgs + 1)];
        uint64_t tag = receiver.bits & 7;
        uint64_t key = (tag == 0) ? asObj(receiver)->classIndex()
                                  : tag | 0x80000000;
        if (key == EXPECTED_CLASS) {
            // Direct dispatch (inline getter/J2J/etc.)
        } else {
            // Deopt to full sendJ2J or interpreter
        }
    }

This is ~200 bytes vs 1372 bytes for sendJ2J. For methods with many
send sites, the icache savings compound.

Implementation: the JIT compiler starts all send sites as sendJ2J.
After profiling shows a site is monomorphic (1 class in IC), the
runtime patches the compiled code to use sendMono. This requires:

    1. Define stencil_sendMono (~200 bytes, 1 class check + dispatch)
    2. Add a "repatch" mechanism: rewrite the stencil at a send site
       in the already-compiled code (memcpy + relocation patch)
    3. Trigger: when an IC has 1 entry and N hits, promote to mono

Risk: polymorphic sites that become monomorphic then change back
need graceful fallback. Use a miss counter — if sendMono misses
too often, revert to sendJ2J.

Expected gain: 5-10% on send-heavy benchmarks from icache reduction.
Each send site shrinks from 1372 to ~200 bytes.

Effort: ~3 days.


---


## Track B: Tier 2 SLJIT Backend

### Why SLJIT

    SLJIT       MIR           Cranelift      LLVM ORC
    C, BSD      C, MIT        Rust only      C++, huge
    ARM64+x86   ARM64+x86    ARM64+x86      everything
    +MIPS+PPC   no Windows    no stable C    ms/method
    ~15K LOC    ~30K LOC      ~200K LOC      ~millions
    fast codegen better code  good code      best code
    no regalloc  has regalloc has regalloc   has regalloc

SLJIT is the lightest. It doesn't have a register allocator — we write
one. That's fine; linear scan is ~500 lines for our use case (no SSA,
no phi nodes, single method scope).

Alternative: MIR has a built-in register allocator and generates better
code (~70% of GCC -O2). If code quality matters more than portability,
MIR is the better choice. No Windows JIT support though.

Decision: start with SLJIT for portability. If code quality disappoints,
switch to MIR. The IR design (step B2) is backend-agnostic.


### B1. Hot Method Detection

Add execution counters to Tier 1 compiled methods.

    JITMethod.executionCount   incremented on each entry
    Threshold                  ~1000 entries (configurable)
    Trigger                    when count > threshold, queue for Tier 2

The counter is already a field in JITMethod (executionCount at offset
126 in JITMethod.hpp). Currently unused. Wire it up:

    1. Entry stencil increments count (1 load + 1 add + 1 store)
    2. When count crosses threshold, set a flag (needsTier2 bool)
    3. After JIT code returns, check flag and compile Tier 2
    4. Replace Tier 1 code pointer with Tier 2 code pointer

Effort: ~1 day.


### B2. Intermediate Representation

Design a simple IR for the Tier 2 compiler. Not SSA — just a linear
list of operations with virtual registers.

    IR opcode        Operands              Notes
    LOAD_TEMP        vR, tempIdx           vR = temps[idx]
    STORE_TEMP       tempIdx, vR           temps[idx] = vR
    LOAD_RECV_VAR    vR, slotIdx           vR = receiver.slots[idx]
    STORE_RECV_VAR   slotIdx, vR           receiver.slots[idx] = vR
    LOAD_LIT         vR, litIdx            vR = literals[idx]
    LOAD_LIT_VAR     vR, litIdx            vR = literals[idx].value
    LOAD_CONST       vR, imm64             vR = immediate value
    ADD              vR, vA, vB            SmallInt fast-path + overflow check
    SUB              vR, vA, vB
    MUL              vR, vA, vB
    CMP_LT           vR, vA, vB            result = true/false Oop
    SEND             vR, selector, nArgs    full method lookup + dispatch
    SEND_MONO        vR, sel, class, method monomorphic send with guard
    CALL_JIT         vR, entryAddr, nArgs   J2J-style direct call
    RETURN           vR                     return to caller
    JUMP             label
    JUMP_TRUE        vR, label
    JUMP_FALSE       vR, label
    PHI              vR, vA, vB             merge at join point (if SSA later)
    GUARD_CLASS      vR, classIdx, deopt    deoptimize if class mismatch
    BOX_INT          vR, rawInt             tag as SmallInteger
    UNBOX_INT        rawInt, vR             untag SmallInteger (with guard)
    ALLOC_TEMP       vR, count              nil-fill temps on stack
    FRAME_PUSH       <state>                save caller state for send
    FRAME_POP        <state>                restore caller state after return

Bytecode-to-IR translation is a 1:1 walk. Each Sista V1 bytecode maps
to 1-3 IR ops. The IR is the substrate for all optimizations.

Effort: ~3 days (data structures + bytecode translator).


### B3. Register Allocation (Linear Scan)

ARM64 has 30 general-purpose registers. Reserve:

    x0         JITState* (argument, caller-saved)
    x1-x15     scratch (16 allocatable caller-saved)
    x16-x17    intra-procedure call scratch (reserved by ABI)
    x18        platform register (reserved on Apple)
    x19-x28    callee-saved (10 allocatable callee-saved)
    x29        frame pointer
    x30        link register
    sp         stack pointer

For compiled methods that use native call frames (B4), we have:

    x19-x28    10 callee-saved registers for method-local values
    x1-x15     15 caller-saved registers for expression temporaries
    x0         reserved for JITState*/argument passing

That's 25 allocatable registers. More than enough for typical Smalltalk
methods (most have < 10 temps + a few stack values).

Linear scan algorithm:

    1. Number all IR ops sequentially (live ranges)
    2. For each virtual register, compute [first-use, last-use] interval
    3. Sort intervals by start position
    4. Walk intervals in order:
       a. Free any physical register whose interval has ended
       b. If a free register exists, assign it
       c. Otherwise, spill the interval ending latest to stack
    5. Rewrite IR to use physical registers
    6. Insert spill/reload at spill points

This is the classic Poletto & Sarkar linear scan. ~500 lines of C++.
No SSA, no graph coloring, no interference graphs. Fast and good enough
for our use case.

Special considerations:

    - Oop values must be GC-visible: callee-saved registers survive GC
      calls, but caller-saved registers need spilling before any call
      that might trigger GC (allocation, send, primitive).
    - Float registers: d0-d31 on ARM64. If we unbox floats (B6), we
      need to allocate FP registers too. Same algorithm, separate pool.

Effort: ~1 week.


### B4. Native Call Frames

Replace the J2JSave tail-call chain with native call/ret. Each Tier 2
compiled method is a proper function:

    tier2_method:
        stp x29, x30, [sp, #-frame_size]!    // prologue
        mov x29, sp
        stp x19, x20, [sp, #16]              // save callee-saved
        ...
        // method body (IR-generated code)
        ...
        ldp x19, x20, [sp, #16]              // restore callee-saved
        ldp x29, x30, [sp], #frame_size      // epilogue
        ret

Message sends compile to:

    // Monomorphic send (guarded)
    ldr x8, [receiver]                        // load class index
    cmp x8, #expected_class
    b.ne deopt_to_interpreter
    bl callee_entry                           // NATIVE CALL
    // return value in x0 or designated register

Returns compile to:

    // Move return value to designated register
    mov x0, return_value_reg
    ldp x29, x30, [sp], #frame_size
    ret                                       // NATIVE RETURN

The hardware return stack buffer (RSB) predicts the return address
correctly. No manual save/restore of caller state — the native stack
frame handles it. This eliminates the 72-byte J2JSave per call.

Interpreter interop:

    When Tier 2 code calls an interpreted method (no JIT entry):
    1. Save registers to a lightweight save frame
    2. Write JITState fields (sp, receiver, ip, etc.)
    3. Return to interpreter with exitReason = ExitSendCached
    4. Interpreter runs callee, returns
    5. Tier 2 code resumes from the bl return address

    When interpreter calls a Tier 2 method:
    1. Interpreter detects JIT entry, calls via JIT_CALL macro
    2. Tier 2 method executes with its own stack frame
    3. Returns to JIT_CALL site via ret

GC cooperation:

    Tier 2 frames on the C stack need GC root scanning.
    Options:
    a. Stack map: record which stack slots and registers hold Oops
       at each GC-safe point (sends, allocations). GC walks the
       stack maps.
    b. Conservative scanning: scan the entire native stack for
       values that look like Oops. Simpler but may retain garbage.

    Option (a) is correct. Stack maps are ~100 bytes per method
    (bitmap of live Oop registers/slots at each safepoint).

Effort: ~2 weeks (frame layout + call sequence + GC stack maps).


### B5. Method Inlining (Tier 2)

With the IR in place, inlining is straightforward:

    1. At a SEND_MONO IR op, if the target method is:
       - Small (< 30 bytecodes)
       - Monomorphic (1 class seen in profiling)
       - Not recursive (caller != callee)
    2. Replace the SEND_MONO with:
       a. GUARD_CLASS on the receiver (deopt if wrong class)
       b. The target method's IR ops, with:
          - Temps remapped to new virtual registers
          - Receiver = the send's receiver register
          - Arguments = the send's argument registers
          - Return replaced with a MOV to the send's result register
    3. Run register allocation on the combined IR
    4. Generate code

Deoptimization on guard failure:

    GUARD_CLASS checks the receiver's class at runtime. If it fails:
    1. Spill all live registers to a save frame
    2. Reconstruct the interpreter state (method, ip, sp, receiver)
    3. Jump to the interpreter to handle the send normally
    4. Optionally: recompile without the failed inline

Inlining budget:

    Max inline depth: 2 (caller → callee → callee's callee)
    Max inlined bytecodes per method: 100
    Max inlined methods per call site: 1 (monomorphic only)

    Polymorphic inlining (2-3 classes) is possible but complex.
    Start with monomorphic only.

Effort: ~1 week (IR substitution + guard generation + deopt).


### B6. Float Unboxing (Future)

NBody and Mandelbrot are 10-13x slower than Cog, dominated by Float
allocation. Cog uses SmallFloat (immediate 64-bit float in Oop).
Spur's SmallFloat encoding:

    Bits 63-3: IEEE 754 double (shifted)
    Bits 2-0: SmallFloat tag (0b010)

Our VM already supports this encoding. But the JIT doesn't exploit it:
every Float operation boxes/unboxes through memory.

Tier 2 optimization:

    1. Type propagation: if a variable is always Float (from profiling
       or Sista annotations), mark it as unboxed in the IR.
    2. Unboxed Float lives in an FP register (d0-d31), not a GP register.
    3. Float arithmetic (fadd, fsub, fmul, fdiv) operates on FP registers
       directly — no boxing, no allocation, no GC pressure.
    4. Box only when escaping to heap (stored in an object slot, passed
       to a polymorphic send, returned from method).

    This is the single biggest potential gain for numeric code:
    Float add goes from ~50 cycles (alloc + box + GC check) to
    1 cycle (fadd instruction).

Effort: ~2 weeks (type propagation + FP register allocation + box/unbox).


---


## Implementation Order

Phase 1: Tier 1 improvements (Track A)            ~2 weeks
    A1. Extended SimStack (4 registers)            2 days
    A3. Monomorphic send fast-path                 3 days
    A2. Compile-time getter/setter inlining        1 week

Phase 2: Tier 2 foundation (Track B)              ~3 weeks
    B1. Hot method detection                       1 day
    B2. IR design + bytecode translator            3 days
    B3. Linear scan register allocator             1 week
    B4. Native call frames                         2 weeks (overlaps B3)

Phase 3: Tier 2 optimizations                     ~3 weeks
    B5. Method inlining                            1 week
    B6. Float unboxing                             2 weeks

Total: ~8 weeks to Cog parity on most benchmarks.


## Benchmark Targets

Current vs Cog, and what each phase should achieve:

    Benchmark    Current   After A   After B   Cog
    Richards     9906ms    ~8000     ~1500     ~500
    DeltaBlue    2687ms    ~2200     ~500      ~300
    Mandelbrot   3111ms    ~2800     ~400      ~300
    NBody        6671ms    ~6000     ~700      ~500
    Bounce       2676ms    ~2200     ~500      ~300
    Permute      556ms     ~450      ~120      ~90
    Queens       394ms     ~320      ~100      ~80
    Sieve        1248ms    ~1000     ~300      ~250
    Storage      2047ms    ~1700     ~400      ~350
    Towers       638ms     ~520      ~140      ~120
    List         1816ms    ~1500     ~400      ~300
    fib(28)      13ms      ~11       ~3        ~2

Track A (Tier 1 improvements) should give ~20% improvement.
Track B (Tier 2) should give ~5-7x improvement over current,
putting us within 1.5-2x of Cog.

The remaining gap to Cog is Sista (image-level type optimization),
which is a separate effort.


## Key Risks

    Risk                              Mitigation
    SLJIT codegen quality too low     Switch to MIR (has regalloc)
    Register allocator bugs           Extensive testing, compare
                                        Tier 1 vs Tier 2 results
    GC + native frames = complex      Conservative scanning first,
                                        precise stack maps later
    Deoptimization corner cases       Keep Tier 1 as fallback for
                                        any method that deopt fails
    Inlining invalidation races       Atomic patching, invalidation
                                        barriers in class modification
    Float unboxing + GC interaction   Box before every GC safepoint,
                                        FP registers not in root set


## Files to Create

    src/vm/jit/Tier2Compiler.hpp      IR definition, compiler class
    src/vm/jit/Tier2Compiler.cpp      Bytecode→IR, optimization passes
    src/vm/jit/RegAlloc.hpp           Linear scan allocator
    src/vm/jit/RegAlloc.cpp
    src/vm/jit/ARM64Emitter.hpp       SLJIT wrapper / direct ARM64 emit
    src/vm/jit/ARM64Emitter.cpp
    src/vm/jit/StackMap.hpp           GC stack maps for native frames
    src/vm/jit/StackMap.cpp

    Existing files modified:
    src/vm/jit/JITRuntime.cpp         Hot method trigger, Tier 2 entry
    src/vm/jit/JITMethod.hpp          needsTier2 flag, tier field
    src/vm/jit/stencils/stencils.cpp  Extended SimStack variants (A1)
    src/vm/jit/JITCompiler.cpp        Compile-time inlining (A2), mono (A3)
    src/vm/Interpreter.cpp            Deopt handler for Tier 2 guards
