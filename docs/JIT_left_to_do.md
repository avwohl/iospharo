do the rest of this list on your own
if you get stuck and need a human do something else on the list
checkpoint reguarly and push without bothering the human

# JIT: Remaining Work

Updated: 2026-04-03

Current state: 66 stencils, ARM64 only, GC cooperation working.
Send-free methods execute via JIT; send-containing methods fall back to
interpreter (hasSends guard). 6,119 methods compiled, 0 failures.
3,502 test passes, 2 failures (pre-existing), 1 error (pre-existing).


## Critical Bugs

### SIGSEGV when code zone fills up — FIXED
Root cause: compact() used memmove to slide JITMethods in memory, but
absolute branch targets in stencil code became stale. Also MethodMap
had stale pointers to moved JITMethods.

Fix: full zone flush (invalidate all methods + compact) when zone is full.
Methods recompile naturally. 5/5 clean runs after fix.

### Send-containing methods cause ~600x slowdown
Tested removing hasSends guard: 97% IC hit rate but C++ boundary crossing
overhead per send (JITState setup, W^X toggle, exit handling) dominates.
144K steps/10s vs 87M without JIT. Need direct stencil-to-stencil calls
(Phase 3 item 1) before this can be enabled.


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

### 5. Code zone eviction / compaction — PARTIAL
Full zone flush implemented (invalidate all + compact when zone is full).
Methods recompile naturally after flush. Proper incremental eviction
(keep hot methods, only evict cold ones) would be better but requires
fixing absolute branch targets in stencils (use relative branches or
maintain a relocation table).

    Current:    Full flush on zone full → all methods lost, recompile
    Better:     Relative branch targets → safe per-method eviction + compact
    Cog ref:    Sliding compaction + LRU eviction (branch targets are relative)

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

### 9. Reduce compilation failures — DONE
Root cause: ARM64 BRANCH26 relocations for runtime helpers (jit_rt_send,
jit_rt_return, jit_rt_arith_overflow) had ±128MB range limit, but the
code zone was mmap'd ~139MB from the helper functions. 10,683 methods
failed to compile because patchARM64() returned false on BRANCH26.

Fix: Changed runtime helper declarations in stencils.cpp from direct
function calls (BL → BRANCH26) to function pointer variables (adrp+ldr
→ GOT_LOAD_PAGE21/PAGEOFF12, ±4GB range). Same pattern already used
for nil/true/false Oop loading. JITCompiler patching stores address of
the helpers_ struct field in the literal pool for double indirection.

Result: 6,119 compiled, 0 failed (was 10,683 failed).

### 10. Full Pharo test suite with JIT
Current validation: 3,502 pass, 2 fail, 1 error across expanded test classes.
(Failures are pre-existing: testBeRecursivelyReadOnlyObject, testBeRecursivelyWritableObject.)
Need to run the full 2000+ class suite to ensure no JIT-specific regressions.


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

    #   Item                        Impact   Effort    Status
    1   Fix code zone crash         blocker  small     DONE
    5   Zone eviction/compaction    blocker  medium    PARTIAL (flush works)
    2   Fix IC patching             high     small     blocked by hasSends
    1   Direct J2J calls for sends  high     large     KEY BLOCKER
    9   Reduce compilation fails    high     medium    DONE (BRANCH26→GOT)
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
