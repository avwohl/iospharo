# JIT Compilation Plans

Date: 2026-04-01

## Constraints

- iOS forbids writable+executable pages -- no JIT on iOS, ever
- JIT targets: macOS ARM64, macOS x86_64, Windows ARM64/x86_64, Linux ARM64/x86_64
- VM is clean C++ (Interpreter.cpp, Primitives.cpp), interprets Sista V1 bytecodes
- Must coexist with the interpreter (iOS stays interpreted, other platforms get JIT)
- Cog VM's design is the proven reference for Smalltalk JIT -- use as blueprint

## Recommended Architecture: Tiered

    Tier 0   Interpreter        All platforms (including iOS)
    Tier 1   Copy-and-patch     Non-iOS platforms, compile on 2nd execution
    Tier 2   Optimizing JIT     Future work, hot methods only (SLJIT or MIR backend)

The single biggest performance win is Tier 1 with inline caching. Tier 2 is
optional and can be added later. Browser engines (V8, JSC) prove this tiered
approach works.

Expected speedups (based on industry data):

    Tier 1 alone    2-5x over interpreter (eliminates dispatch + inline caching)
    Tier 2 added    5-10x over interpreter (register allocation + inlining)
    Cog reference   ~5x over Cog interpreter


---

## Option 1 (Recommended): Copy-and-Patch JIT

### What it is

Pre-compile each bytecode handler as a machine code "stencil" at build time
using Clang. At runtime, JIT compilation = memcpy stencils + patch in runtime
values (addresses, constants, operands). No assembly knowledge required.

### How it works

    BUILD TIME (when compiling the VM):
    1. Each Sista V1 bytecode handler written as a C++ function with "holes"
       (extern symbol references for runtime values)
    2. Clang compiles each to an object file with -O3
    3. Build script extracts machine code bytes + relocation records
    4. Emitted as C byte arrays in a generated header (jit_stencils.h)

    RUNTIME (when executing Pharo code):
    1. Method hits execution threshold (2nd call, like Cog)
    2. For each bytecode: memcpy stencil bytes into executable buffer
    3. Patch holes: operand values, next-stencil addresses, cache slots
    4. Mark buffer executable (mprotect/VirtualProtect)
    5. Jump to compiled code

### Why this is the top choice

    Pros                                   Cons
    No assembly per architecture           Clang required at build time
    Stencils are C++ (you already have     ~25% slower than optimizing JIT
      the handlers in Interpreter.cpp)     No cross-bytecode optimization
    Near-zero compilation latency          Young technique (2021 paper)
    ~2500-4000 lines of new code           Build pipeline complexity
    Code quality = Clang -O3 per stencil
    Architecture support = whatever Clang targets

### Implementation estimate

    Build script (stencil extraction)        500-800 lines
    Runtime JIT engine (copy + patch)        400-600 lines
    Stencil definitions (restructured handlers)  1000-2000 lines
    Inline cache support                     300-500 lines
    Memory management (mmap/mprotect)        100-200 lines
    Total                                    ~2500-4000 lines

Timeline: 2-4 months for core engine, based on CPython's experience (one
developer, 6-12 months including unrelated uop infrastructure).

### Precedents

    CPython 3.13    First major language to ship copy-and-patch (experimental)
    Deegen          Generates a complete Lua JIT using copy-and-patch
                    19M bytecodes/sec compilation, 1.6 GB/s code generation
    Original paper  Xu & Kjolstad 2021, tested on Wasm + DB queries

### Inline caching (the big win)

A monomorphic send stencil would look like:

    void send_mono(VMState *s) {
        extern void *hole_cached_class;
        extern void *hole_cached_method;
        extern void hole_miss(void);

        Oop klass = classOf(s->stack[s->sp]);
        if (klass == (Oop)&hole_cached_class) {
            activate((Method*)&hole_cached_method, s);
        } else {
            ((void(*)(VMState*))&hole_miss)(s);
        }
    }

Miss handler patches the cache or promotes to a PIC. This is exactly Cog's
inline caching pattern, expressed in C instead of hand-written assembly.

### Platform support

    Platform           Stencil compilation    Status
    macOS ARM64        Clang (default)        Primary target
    macOS x86_64       Clang (default)        Primary target
    Linux ARM64        Clang/cross-compile    Straightforward
    Linux x86_64       Clang/cross-compile    Straightforward
    Windows ARM64      Clang/MSVC             Needs COFF relocation handling
    Windows x86_64     Clang/MSVC             Needs COFF relocation handling
    iOS                N/A                    Stays interpreted (OS forbids JIT)


---

## Option 2: SLJIT (Stack-Less JIT Compiler)

### What it is

A lightweight, portable JIT assembler library. Provides a RISC-like virtual
register machine that maps to native instructions. Powers PCRE2's JIT (ships
in virtually every OS).

    Language     C (one .h + one .c file)
    License      BSD 2-clause
    Platforms    Windows, macOS, Linux, *BSD, and many others
    Archs        x86 32/64, ARM 32/64, RISC-V, s390x, PPC, LoongArch, MIPS

### Pros and cons

    Pros                                    Cons
    Designed specifically for bytecode JIT  Low-level API (register machine)
    One header + one source file            No optimization passes
    Widest architecture coverage            Must write JIT logic in SLJIT API
    Battle-tested (PCRE2)                   More code than copy-and-patch
    BSD license                             No register allocator
    W^X support, Apple Silicon support

### When to use

Best as a Tier 2 backend if copy-and-patch handles Tier 1. SLJIT could
generate optimized code for hot methods with register allocation across
bytecode boundaries -- something copy-and-patch cannot do.

Could also be the sole JIT backend if copy-and-patch's build-time Clang
requirement is unacceptable.


---

## Option 3: AsmJIT

### What it is

A C++ JIT assembler library with Assembler (raw), Builder (reorderable), and
Compiler (virtual registers + register allocator) modes.

    Language     C++ (no external dependencies)
    License      Zlib (very permissive)
    Archs        x86, x86_64, AArch64
    Platforms    Windows, macOS, Linux

### Pros and cons

    Pros                                    Cons
    Best C++ API of any assembler library   DEVELOPMENT SUSPENDED (no funding)
    Built-in register allocator             AArch64 backend less mature
    Apple Silicon W^X support               Uncertain future
    ~500KB compiled                         No optimization passes

### Risk

Lead developer stepped back due to lack of funding. Would need to fork and
maintain if adopted. The suspended status makes this a secondary option despite
having the best API.


---

## Option 4: DynASM (from LuaJIT)

### What it is

A two-phase system: Lua preprocessor at build time converts mixed C/assembly
(.dasc files) into pure C. Tiny C runtime (~500 lines) links fragments at
runtime.

    Language     Lua (build) + C (runtime)
    License      MIT
    Archs        x86, x86_64, ARM, ARM64, PPC, MIPS
    Platforms    Any

### Pros and cons

    Pros                                    Cons
    Extremely fast code generation          Must write assembly per architecture
    Tiny runtime footprint                  Lua build dependency
    Battle-tested (powers LuaJIT)           Less documented
    Broad architecture support              Harder to maintain mixed C/asm

### When to use

If you want maximum control over generated code and are willing to write
per-architecture assembly. LuaJIT's approach of writing the entire interpreter
in DynASM plus the JIT is proven but requires deep architecture expertise.


---

## Option 5: MIR (Medium Internal Representation)

### What it is

A lightweight JIT compiler targeting ~70% of GCC -O2 code quality at 100x
faster compilation. Pure C, MIT license.

    Language     C (no external dependencies)
    License      MIT
    Archs        x86_64, AArch64, PPC64 LE, s390x, RISC-V 64
    Platforms    Linux, macOS (Windows JIT BROKEN)

### Pros and cons

    Pros                                    Cons
    70% of GCC -O2 quality                  Windows JIT does NOT work
    100x faster compilation than GCC        Less mature than LLVM
    Pure C, MIT license                     Smaller community
    Includes register allocation + opts     No 32-bit x86

### When to use

Strong Tier 2 optimizer for Linux/macOS. The broken Windows support is a
dealbreaker for full cross-platform until it's fixed. Worth monitoring.


---

## Option 6: LLVM ORC JIT

### What it is

LLVM's production JIT API. Same optimization passes as Clang -O2/-O3.

    Language     C++ (with C bindings)
    License      Apache 2.0 + LLVM exception
    Archs        Everything LLVM supports
    Platforms    Everything LLVM supports

### Pros and cons

    Pros                                    Cons
    Best code quality possible              ENORMOUS dependency (tens of MB)
    Industrial-grade, all platforms          Milliseconds per method to compile
    Lazy + concurrent compilation           Complex API
    Vast ecosystem                          Startup overhead

### When to use

Only as an optional Tier 3 for the absolute hottest methods (top 0.1%).
WebKit abandoned LLVM JIT because compilation was too slow and replaced it
with their own B3 backend. The dependency weight rules it out as a primary JIT.


---

## Options NOT Recommended

    Option          Why not
    Cranelift       No stable C API, requires Rust toolchain
    QBE             Not a JIT (emits assembly/object files, no runtime codegen)
    RyuJIT          Not reusable outside .NET runtime
    GraalVM/Truffle Requires JVM, incompatible with standalone C++ VM
    GNU Lightning   LGPL license, lower code quality than alternatives
    Xbyak           Two separate libraries for x86/ARM64, no register allocator


---

## Design Patterns from Cog VM (Apply Regardless of Backend)

The Cog JIT is the proven blueprint for Smalltalk JIT. These patterns should
be adopted regardless of which code generation backend we choose:

### 1. Compile on 2nd execution

Avoid compiling cold code. Cog compiles a method when it's called a second
time. The first execution runs interpreted. This is cheap to implement: a
counter in the method header.

### 2. Three-tier inline caching

    Monomorphic    Single cached (class, method) pair at each send site
                   Covers ~90% of send sites
    Closed PIC     Up to 6 (class, method) entries
                   Covers ~9% of send sites
    Open PIC       Hash-probe into global method cache
                   Covers ~1% (megamorphic sites)

This is the single most impactful optimization for Smalltalk. Message sends
dominate execution time, and inline caching eliminates the lookup for >99% of
sends.

### 3. Machine code zone

    Fixed-size region (Cog uses 16 MB)
    CogMethod header + machine code + backward method map
    LRU eviction when zone fills up
    Sliding compaction to eliminate gaps

### 4. SimStack (stack-to-register mapping)

During compilation, track where each stack value lives (register, constant, or
spilled to memory). Avoids redundant loads/stores. NOT a full register
allocator -- just a peephole optimization. Simple and effective.

### 5. Sista support

The Sista adaptive optimization framework runs in the Pharo IMAGE, not the VM.
The VM only needs to provide:

    Counter slots    Decremented on each conditional branch
    Trap bytecode    Calls back into image optimizer when counter trips
    Unsafe prims     Unchecked SmallInteger ops (optimizer proves types)

If our JIT supports counters and traps, the Sista optimizer in the image works
automatically -- it rewrites bytecodes, and the JIT recompiles them.


---

## Recommended Implementation Plan

### Phase 1: Infrastructure (2-3 weeks)

    Machine code zone     mmap/VirtualAlloc, W^X support, zone management
    CogMethod layout      Header struct, link to CompiledMethod, method map
    Method state machine  Interpreted -> Compiled -> Invalidated
    Platform abstraction  Memory protection, icache flush per OS/arch

### Phase 2: Copy-and-Patch Baseline (6-8 weeks)

    Stencil extraction    Build script (parse ELF/Mach-O, extract code + relocs)
    Bytecode stencils     Restructure Interpreter.cpp handlers as stencil functions
    Runtime patcher       memcpy + hole patching
    Basic compilation     Compile method = concatenate patched stencils
    Entry/exit            Transition between interpreter and JIT code
    Inline cache (mono)   Monomorphic send sites with class check + patch-on-miss

### Phase 3: Inline Caching (3-4 weeks)

    Closed PICs           Polymorphic inline caches (up to 6 entries)
    Open PICs             Megamorphic fallback to method cache probe
    Cache invalidation    Flush on become:, class hierarchy changes, GC moves
    Profiling counters    Per-branch counters for Sista support

### Phase 4: Polish and Optimize (2-3 weeks)

    Superinstructions     Pre-compile common bytecode pairs as single stencils
    Context support       Deoptimization for thisContext access
    GC cooperation        Stack scanning, code zone root scanning
    Testing               Run full Pharo test suite with JIT enabled

### Phase 5 (Future): Tier 2 Optimizing JIT

    Backend               SLJIT or MIR
    Trigger               Counter-based (hot method detection)
    Optimizations         Register allocation, type specialization, inlining
    Sista integration     Image-level optimizer drives JIT recompilation


---

## Completed: GC Cooperation (2026-04-02)

JIT-compiled methods now survive compacting GC correctly:

  - **forEachRoot scanning**: JITMethod headers' `compiledMethodOop` fields are
    scanned as GC roots. This keeps referenced CompiledMethods alive and updates
    Oop values in-place when compaction moves objects. Skips invalidated entries
    (failed compilations with zeroed Oop).

  - **recoverAfterGC**: Called after every fullGC. Flushes all ICs (cached Oops
    are stale), updates nil/true/false bits, rebuilds MethodMap from updated
    JITMethod headers, and clears the execution count map (keys are stale Oop bits).

  - **Code zone leak fix**: Failed compilations (patch failure after allocation)
    now invalidate the JITMethod and zero its compiledMethodOop, preventing
    GC from visiting stale pointers and allowing compaction to reclaim space.

## Performance: Send-Heavy Methods (2026-04-02)

**Finding:** JIT execution of methods containing sends is ~1000x slower than
interpretation. Every send exits the JIT (W^X toggle, JITState marshalling,
exit handling) and re-enters the interpreter. For typical Smalltalk code (which
is mostly sends), this overhead dominates.

**Fix:** `tryExecute` and `tryResume` skip methods with `hasSends=true`. Only
send-free methods (pure arithmetic, accessors, control flow) are executed via
JIT, where the overhead is amortized by the actual computation.

**Impact:** With the hasSends guard, JIT-enabled VM runs at full interpreter
speed (~87M steps/10s vs ~90M without JIT). Send-free methods still benefit
from JIT compilation.

**Future:** To make the JIT profitable for send-containing methods, implement
direct JIT-to-JIT calls on IC hits (call target stencil directly without
exiting to the interpreter). This is the Phase 3+ optimization.

---

## Known Bugs

### JIT IC Corruption: Special Selector Sends (2026-04-02) — FIXED

**Status:** Fixed (commit 944e8bb).

**Symptom:** Infinite `#assert` recursion during image startup. Occurred ~80%
of runs with JIT enabled.

**Root cause:** When the JIT exited with `ExitSend` at a `#value` bytecode,
`pendingICPatch_` was set to that send site's IC data pointer. The interpreter
then handled `#value` via the fast path (`primitiveFullClosureValue` →
`activateBlock`), bypassing `sendSelector`. Sends INSIDE the block consumed
`pendingICPatch_` via `patchJITICAfterSend`, writing the wrong method into the
`#value` send's IC. This caused the IC to cache the assert method itself as the
target for `#value`, creating infinite recursion.

**Fix:** Clear `pendingICPatch_` at the start of `activateBlock()`. Block
activation means sends inside the block are unrelated to the JIT send that
set the pending IC patch.

**Verification:** 5/5 clean runs with JIT enabled after fix (previously 4/5
showed recursion).

---

## Key References

    Copy-and-Patch
      Paper              Xu & Kjolstad 2021, fredrikbk.com/publications/copy-and-patch.pdf
      CPython impl       github.com/python/cpython Tools/jit/
      Deegen (Lua)       sillycross.github.io/2023/05/12/

    Cog JIT
      Source             github.com/OpenSmalltalk/opensmalltalk-vm (branch Cog)
      Paper              Miranda et al. "Two Decades of Smalltalk VM Development" VMIL 2018
      Blog               clementbera.wordpress.com (Sista, inline caching)

    JIT Libraries
      SLJIT              github.com/zherczeg/sljit (BSD, C, widest arch support)
      AsmJIT             github.com/asmjit/asmjit (Zlib, C++, dev suspended)
      DynASM             luajit.org/dynasm.html (MIT, Lua+C, powers LuaJIT)
      MIR                github.com/vnmakarov/mir (MIT, C, no Windows JIT)

    Dynamic Language JIT Design
      V8 Sparkplug       v8.dev/blog/sparkplug (template JIT, closest to our Tier 1)
      Ruby YJIT          shopify.engineering/ruby-yjit-is-production-ready
      JSC tiers           docs.webkit.org/Deep%20Dive/JSC/JavaScriptCore.html
