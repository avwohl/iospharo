# JIT: Remaining Work

Updated: 2026-04-02

Current state: 66 stencils, ARM64 only, GC cooperation working.
Send-free methods execute via JIT; send-containing methods fall back to
interpreter (hasSends guard). 786 test passes, 0 failures.


## Critical Bugs

### SIGSEGV when code zone fills up
Code zone reaches capacity (~16384 KB) due to 7000+ failed compilations.
After zone is full, a crash occurs in tryExecute (fault addr=0x1). Likely
a stale JITMethod pointer or corrupt code after zone pressure. Reproduces
in runs longer than ~2 minutes with many unique methods.

    Where:  JITRuntime::tryExecute → SIGSEGV
    Impact: Crashes the VM after extended running
    Fix:    Need LRU eviction or zone compaction (see below), and/or
            stop attempting compilations when zone is nearly full


## Phase 3: Make JIT Profitable for Sends

The hasSends guard currently prevents JIT execution of any method with
sends. This is ~95% of all Smalltalk methods. Without fixing this, the
JIT only helps pure arithmetic/accessor methods.

### 1. Direct JIT-to-JIT calls (highest priority)
When an IC hits and the target method is also JIT-compiled, call the
target's machine code directly instead of exiting to the interpreter.
This eliminates the W^X toggle + JITState marshalling overhead that
makes per-send JIT execution ~1000x slower than interpretation.

    Files:     src/vm/jit/stencils/stencils.cpp (send stencils)
               src/vm/jit/JITRuntime.cpp (tryExecute/tryResume)
    Design:    On IC hit, check if target CompiledMethod has a JITMethod.
               If yes, call it directly (function pointer in IC data).
               On return, resume caller's JIT code.
    Blocker:   Need to handle stack frame setup in machine code.

### 2. IC patching in compiled code
ICs are currently never populated (stats show 0% hit rate, 0 patched).
The pendingICPatch_ mechanism exists but may not be wiring up correctly
after the hasSends guard was added (sends deopt before IC gets a chance
to be populated).

    Files:     src/vm/jit/JITRuntime.cpp (patchJITICAfterSend)
               src/vm/Interpreter.cpp (sendSelector)
    Verify:    Disable hasSends guard temporarily, check IC stats


## Phase 3: Remaining Inline Cache Work

### 3. IC invalidation on class hierarchy changes
Currently ICs are flushed on become: and method changes. Also need
flushing when:
- A class is reshaped (slot layout changes)
- A method is added/removed from a class (new lookup result)
- A class is removed

    Where:  Primitives.cpp — primitiveChangeClass, compile-related prims

### 4. Profiling counters for Sista support
Per-branch counters decremented on conditional branches. When counter
reaches zero, fire trap bytecode to call back into image optimizer.

    Bytecodes: 0xF8-0xFF trap range in Sista V1
    Design:    Counter slot per conditional branch in JITMethod header
               Stencil decrements counter, branches to trap handler on zero
    Payoff:    Enables Sista adaptive optimization in the image


## Phase 4: Polish and Robustness

### 5. Code zone eviction / compaction
Zone fills up with ~7000 failed compilations + ~500 successful ones.
Currently no way to reclaim space from invalidated methods.

    Cog approach: LRU eviction (free least-recently-used methods)
                  Sliding compaction (eliminate gaps)
    Simpler:      Stop compiling when zone is >90% full
                  Free invalidated methods' space on next GC

### 6. Context support / deoptimization
`thisContext` access (bytecode 0x58) currently causes deopt. Methods
that reify their context (for exception handling, debugging, etc.)
can't run in JIT at all.

    Design:    On thisContext access, create a real Context object from
               JIT frame, switch to interpreter for that activation.
    Cog ref:   "Marry" the context to the frame, deoptimize on access

### 7. PushArray stencil (0xE7)
`createArray:` bytecode allocates a Smalltalk Array on the heap. Can't
do heap allocation in stencils (no GC cooperation mid-stencil).

    Design:    Same as PushFullBlock — exit with ExitArrayCreate, handler
               allocates array, resumes JIT.
    Impact:    Unblocks compilation of methods using literal arrays,
               cascades, and some control flow patterns.

### 8. More bytecodes that currently deopt
    0x58    pushThisContext        (needs context/deopt support)
    0x5C    blockReturn            (return from block to home context)
    0x78    superSend              (needs super lookup, different IC)
    0x79    superSend (ext)        (same)
    0xE7    pushArray              (see item 7)
    0xEE    closureCreate          (old-style, rarely used)
    0xF0-F1 callPrimitive          (inlined primitives)
    0xF8-FF trap                   (needs profiling counters)

### 9. Reduce compilation failures
10,683 out of ~16,000 hot methods fail to compile. Main reasons:
- Bail-out on unsupported bytecodes (tracked in bailoutCounts_[])
- Methods too large (> MaxCompilableBytecodes)
Need to audit bailoutCounts_ to find the most impactful missing stencils.

### 10. Full Pharo test suite with JIT
Current validation: 9 Kernel-Tests classes (786 pass, 0 fail).
Need to run all 187+ classes and the full 2000+ class suite to ensure
no JIT-specific regressions.


## Phase 5: Tier 2 Optimizing JIT (Future)

### 11. SLJIT or MIR backend
Generate optimized machine code for the hottest methods. Register
allocation across bytecode boundaries, type specialization, inlining.

    Backend:   SLJIT (widest arch support, C, BSD) or
               MIR (better code quality, no Windows)
    Trigger:   Counter-based hot method detection
    Payoff:    5-10x over interpreter (vs 2-5x for Tier 1)

### 12. Sista integration
Image-level optimizer rewrites bytecodes; VM recompiles them.
Requires: profiling counters (item 4), trap bytecodes, unsafe prims
(unchecked SmallInteger ops where optimizer has proved types).

### 13. SimStack (stack-to-register mapping)
Track where each stack value lives (register, constant, spilled) during
Tier 2 compilation. Avoids redundant loads/stores. Peephole optimization,
not a full register allocator.


## Cross-Platform (Future)

### 14. x86_64 stencils
Currently ARM64 only. Need x86_64 stencils for macOS Intel, Linux, Windows.
extract_stencils.py already parses Mach-O; needs ELF/COFF support.

### 15. Windows COFF relocation handling
Stencil extraction needs to handle COFF relocations for Windows builds.

### 16. Linux support
Straightforward once ELF parsing is added to extract_stencils.py.
W^X uses standard mmap/mprotect (no Apple-specific MAP_JIT needed).


## Priority Order

    #   Item                        Impact   Effort
    1   Fix code zone crash         blocker  small
    5   Zone eviction/compaction    blocker  medium
    2   Fix IC patching             high     small
    1   Direct J2J calls for sends  high     large
    9   Reduce compilation fails    high     medium
    7   PushArray stencil           medium   small
    10  Full test suite             medium   medium
    4   Profiling counters          medium   medium
    6   Context / deoptimization    medium   large
    8   More bytecode stencils      medium   medium
    14  x86_64 stencils             medium   large
    11  Tier 2 backend              low      very large
    12  Sista integration           low      large
    13  SimStack                    low      medium
    15  Windows COFF                low      medium
    16  Linux ELF                   low      small
