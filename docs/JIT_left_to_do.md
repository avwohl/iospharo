do the rest of this list on your own
if you get stuck and need a human do something else on the list
checkpoint reguarly and push without bothering the human

# JIT: Remaining Work

Updated: 2026-04-03

Current state: 67 stencils, ARM64 only, GC cooperation working.
Inline getter/setter/yourself dispatch in stencil_sendPoly eliminates
C++ boundary crossing for trivial sends. IC stride-3 layout (104 bytes/site)
with extra word encoding getter (bit 63), setter (bit 62), returnsSelf (bit 61).
hasSends guard removed — all methods with sends now execute via JIT.
6,119+ methods compiled, 0 failures, 97% IC hit rate.
IC selector verification prevents cross-send IC corruption from stale
pendingICPatch_ in nested JIT executions/process switches.


## Critical Bugs

### SIGSEGV when code zone fills up — FIXED
Root cause: compact() used memmove to slide JITMethods in memory, but
absolute branch targets in stencil code became stale. Also MethodMap
had stale pointers to moved JITMethods.

Fix: full zone flush (invalidate all methods + compact) when zone is full.
Methods recompile naturally. 5/5 clean runs after fix.

### JIT IC dispatch to wrong method — FIXED (two rounds)
Round 1: sendSelector() had early-exit paths (primitive success,
getter/setter/identity fast paths) that returned without calling
patchJITICAfterSend(). Fix: call patchJITICAfterSend() on ALL
successful send resolution paths.

Round 2: Even with all paths calling patchJITICAfterSend(), stale
pendingICPatch_ from nested JIT executions or process switches could
patch the wrong IC. Diagnostic showed IC for #valueNoContextSwitch
being patched with #debuggerSelectionStrategy method.

Fix: Added selector verification — patchJITICAfterSend() now takes
the send's selector parameter and compares it against icData[12]
(the IC's stored selectorBits). If they don't match, the patch is
skipped. All 7 call sites in sendSelector() updated to pass selector.

### Send-containing methods cause ~600x slowdown — FIXED
Tested removing hasSends guard: 97% IC hit rate but C++ boundary crossing
overhead per send (JITState setup, W^X toggle, exit handling) dominated.
Fix: inline getter/setter/yourself dispatch in stencil_sendPoly. On IC
hit for trivial methods, the stencil reads/writes the field directly
and continues to the next stencil without exiting to C++. Non-trivial
sends still exit via ExitSendCached with J2J chaining in the interpreter.


## Phase 3: Make JIT Profitable for Sends

### 1. Direct JIT-to-JIT calls — PARTIAL (inline getter/setter)
Inline getter/setter/yourself dispatch handles the most common trivial
sends (~30-50% of all sends in typical Smalltalk code) entirely within
the stencil. For non-trivial sends, the interpreter's J2J chaining
(tryJITActivation loop on ExitSendCached) handles method calls with
reduced overhead vs full interpreter dispatch.

Full stencil-to-stencil calls (saving/restoring JITState, calling
target JIT code directly from sender JIT code) remain a future
optimization for non-trivial sends.

    Files:     src/vm/jit/stencils/stencils.cpp (stencil_sendPoly IC_HIT macro)
               src/vm/Interpreter.cpp (patchJITICAfterSend — getter/setter detection)
    Status:    IC hit inline dispatch working, hasSends guard removed

### 2. IC patching in compiled code — DONE
ICs are populated on send misses. 97% hit rate observed. The
patchJITICAfterSend function now detects trivial methods (getter,
setter, returnsSelf) and stores the info in the IC extra word for
inline dispatch.

    Files:     src/vm/Interpreter.cpp (patchJITICAfterSend)
    Status:    Working — 97% IC hit rate, 16+ patches per 64K sends


## Phase 3: Remaining Inline Cache Work

### 3. IC invalidation on class hierarchy changes — DONE
JIT ICs are now flushed in all relevant primitives:
- primitiveChangeClass (115/160) — class reshape/adopt
- primitiveFlushCacheByMethod (119) — method added/removed/modified
- primitiveFlushCacheBySelector (120) — selector invalidation
- become: already triggers full IC flush via recoverAfterGC

    Files:  Primitives.cpp — flushJITCaches() calls added to changeClassOf,
            primitiveFlushCacheByMethod, primitiveFlushCacheBySelector

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

### 7. PushArray stencil (0xE7) — DONE
`createArray:` bytecode exits with ExitArrayCreate, handler allocates
array in the interpreter, resumes JIT. Unblocked compilation of methods
using literal arrays, cascades, and some control flow patterns.

### 8. More bytecodes that currently deopt — PARTIAL
    0x52    pushThisContext        (needs context/deopt support)
    0x5C    blockReturn            (return from block to home context)
    0xEE    closureCreate          (old-style, rarely used)
    0xF0-F1 callPrimitive          (inlined primitives)
    0xF8-FF trap                   (needs profiling counters)

    DONE:
    0x5F    nop                    (handled as stencil_nop)
    0x78    superSend              (polymorphic IC via 0xEB stencil)
    0x79    superSend (ext)        (same)
    0xFE    unassigned 3-byte      (handled as 3-byte nop)
    0xFF    unassigned 3-byte      (handled as 3-byte nop)
    0x60-6F arithmetic sends       (upgraded to sendPoly with IC caching)

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

### 10. Full Pharo test suite with JIT — PARTIAL
Current validation: 3,502 pass, 2 fail, 1 error across expanded test classes.
(Failures are pre-existing: testBeRecursivelyReadOnlyObject, testBeRecursivelyWritableObject.)
Need to run the full 2000+ class suite to ensure no JIT-specific regressions.

Previous blocker (JIT IC dispatch bug causing infinite recursion on #copy)
is fixed. Remaining blocker: SessionManager startup sequence doesn't fully
complete in our VM. The Delay scheduler runs at priority 79, preventing
lower-priority test processes from being scheduled.


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
    1   Inline getter/setter J2J    high     medium    DONE
    2   IC patching                 high     small     DONE (97% hit rate)
    9   Reduce compilation fails    high     medium    DONE (BRANCH26→GOT)
    7   PushArray stencil           medium   small     DONE
    3   IC hierarchy invalidation   medium   small     DONE
    8   More bytecode stencils      medium   medium    PARTIAL (super, arith, nop, 0xFE/FF)
    10  Full test suite             medium   medium    PARTIAL (IC bug fixed)
    4   Profiling counters          medium   medium    TODO
    6   Context / deoptimization    medium   large     TODO
    14  x86_64 stencils             medium   large     TODO
    11  Tier 2 backend              low      very large
    12  Sista integration           low      large
    13  SimStack                    low      medium
    15  Windows COFF                low      medium
    16  Linux ELF                   low      small
