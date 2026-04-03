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
6,119+ methods compiled, 0 failures, 99% IC hit rate.
IC selector verification prevents cross-send IC corruption from stale
pendingICPatch_ in nested JIT executions/process switches.
Code zone: incremental LRU eviction via free list (no method movement).
Block returns (0x5D/0x5E) handled as return stencils in FullBlocks.


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

### 5. Code zone eviction / compaction — DONE
Incremental LRU eviction via free list. When zone fills, cold methods
are freed into a free list; hot methods survive. allocate() tries bump
pointer first, then best-fit from free list. Adjacent free blocks are
coalesced. Full flush is only a last resort if free list is too fragmented.

Methods are never moved in memory — this avoids ADRP+LDR page-relative
relocation issues with ARM64 stencils. Tested with 256KB zone: keeps
~92 hot methods alive while recycling cold ones, zero full flushes.

    Files:     src/vm/jit/CodeZone.hpp (FreeBlock, freeMethod, allocateFromFreeList)
               src/vm/jit/JITCompiler.cpp (incremental eviction with MethodMap callback)
    Old:       Full flush on zone full → all methods lost, recompile
    New:       LRU eviction → free list → reuse, hot methods preserved

### 6. Context support / deoptimization — PARTIAL
`thisContext` access (bytecode 0x52) currently causes deopt to
interpreter which handles it correctly. Methods with thisContext
compile and run, but remaining bytecodes after 0x52 run interpreted.

Block returns (0x5D/0x5E) in FullBlocks are now handled as simple
returns (stencil_returnNil/stencil_returnTop). Non-local returns
(extA > 0) and old-style closures still deopt correctly.

Full JIT resume after thisContext would require frame materialization
without invalidating the JIT frame stack. Cog handles this with
context-frame "marriage" which is a larger effort.

    Files:     src/vm/jit/JITCompiler.cpp (FullBlock detection, block return stencils)
    Status:    Block returns in FullBlocks: DONE
               thisContext deopt to interpreter: works (no resume)
               Non-local returns: deopt (works)

### 7. PushArray stencil (0xE7) — DONE
`createArray:` bytecode exits with ExitArrayCreate, handler allocates
array in the interpreter, resumes JIT. Unblocked compilation of methods
using literal arrays, cascades, and some control flow patterns.

### 8. More bytecodes that currently deopt — PARTIAL
    Remaining (deopt to interpreter, all work correctly):
    0x52    pushThisContext        (deopt works, resume would need frame marriage)
    0xFA    closureCreate          (old-style, rarely used in Pharo 10+)

    DONE:
    0x5D    blockReturnNil         (FullBlock: stencil_returnNil, others: deopt)
    0x5E    blockReturnTop         (FullBlock: stencil_returnTop, others: deopt)
    0x5F    nop                    (handled as stencil_nop)
    0x78    superSend              (polymorphic IC via 0xEB stencil)
    0x79    superSend (ext)        (same)
    0xF8    callPrimitive          (NOP — handled at method activation)
    0xEC    inlinedPrimitive       (NOP — handled by interpreter on miss)
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

### 14. x86_64 stencils — PARTIAL (infrastructure done)
extract_stencils.py generates x86_64 stencil headers (67 stencils, 5354 bytes,
184 relocations). JITCompiler.cpp refactored with if constexpr for x86_64
patching (X86_64_PC32 relocations, independent literal pool slots).
Remaining: COFF parsing for Windows, ELF for Linux. macOS x86_64 not a
useful target (Apple Silicon transition complete). Windows is the right
place to finish x86_64 support.

    Files:     scripts/extract_stencils.py (--arch flag, x86_64 reloc maps)
               src/vm/jit/generated_stencils_x86_64.hpp (generated)
               src/vm/jit/generated_stencils.hpp (arch dispatcher)
               src/vm/jit/JITCompiler.cpp (arch-conditional patching)

### 15. Windows COFF relocation handling
Stencil extraction needs to handle COFF relocations for Windows builds.

### 16. Linux support
Straightforward once ELF parsing is added to extract_stencils.py.
W^X uses standard mmap/mprotect (no Apple-specific MAP_JIT needed).


## Priority Order

    #   Item                        Impact   Effort    Status
    1   Fix code zone crash         blocker  small     DONE
    5   Zone eviction/compaction    blocker  medium    DONE (free list LRU)
    1   Inline getter/setter J2J    high     medium    DONE
    2   IC patching                 high     small     DONE (97% hit rate)
    9   Reduce compilation fails    high     medium    DONE (BRANCH26→GOT)
    7   PushArray stencil           medium   small     DONE
    3   IC hierarchy invalidation   medium   small     DONE
    8   More bytecode stencils      medium   medium    PARTIAL (+ block returns in FullBlocks)
    10  Full test suite             medium   medium    PARTIAL (blocked by SessionManager)
    4   Profiling counters          medium   medium    TODO (W^X constraints)
    6   Context / deoptimization    medium   large     PARTIAL (block returns done, thisContext deopt)
    14  x86_64 stencils             medium   large     PARTIAL (infra done, needs Windows COFF)
    11  Tier 2 backend              low      very large
    12  Sista integration           low      large
    13  SimStack                    low      medium
    15  Windows COFF                low      medium
    16  Linux ELF                   low      small
