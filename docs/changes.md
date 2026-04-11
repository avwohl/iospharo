# JIT Infrastructure and Copy-and-Patch Compiler

2026-04-10

## Stencil-to-stencil J2J calls (Phase 4)

Stencils now handle J2J sends and returns inline via tail-calls, eliminating
the C trampoline overhead for JIT-to-JIT method calls. The J2J_IC_HIT macro
pushes a 72-byte save frame and tail-calls the callee entry. Return stencils
(J2J_INLINE_RETURN) pop the save frame and tail-call back to the caller's
resume point.

Key changes:
- JITState gained 4 fields: j2jSaveCursor, j2jSaveLimit, j2jDepth, j2jTotalCalls
- HoleKind::ResumeAddr added for storing next-stencil address as DATA
- J2JSave stack allocated before tryExecute (was after, causing nullptr SEGV)
- tryResume zeroes all J2J fields (prevents garbage pointer dereferences)
- extract_stencils.py: -mllvm -hot-cold-split=false to prevent Clang code splitting

Performance: fib(28)=10ms (vs 9ms ASM trampoline), sieve=3ms, tinyBench=5545ms.
Test suite: 23141/23286 pass (99.3%), identical to NO_JIT — zero regressions.

## test_load_image: forward CLI args to Pharo image

`test_load_image` now forwards `argv[2+]` directly to `setImageArguments()`
instead of hardcoding `{"--interactive"}`. With `--headless` set as a VM
parameter, Pharo's PharoCommandLineHandler activates and dispatches to its
standard subhandlers (`eval`, `test`, etc.). Empty args still default to
`{"--interactive"}` so bare invocation launches the Morphic GUI.

Verified (with `PHARO_NO_JIT=1`):
- `./build/test_load_image image eval "1 + 2"` → `3`
- `./build/test_load_image image eval "1 benchmark printString"` → `'1028'`
- `./build/test_load_image image test "Kernel-Tests"` → STCommandLineHandler runs

JIT-enabled CLI invocation still hits a `mustBeBoolean` loop in
`Array>>mergeFirst:middle:last:into:by:` / `Context>>findContextSuchThat:` along
the SubscriptOutOfBounds error path. The bench harness avoids it, so JIT bench
still works; the env-var bench harness will be removed once that JIT bug is
fixed.

2026-04-09

## J2J Direct Calls — Working and Benchmarked

J2J (JIT-to-JIT) direct calls eliminate the C++ round-trip for sends
between JIT-compiled methods. When an IC hit targets a JIT-compiled
method, bit 60 in the IC extra word encodes the JIT entry address.
The sendJ2J stencil calls jit_rt_j2j_call which pushes an interpreter
frame (pushFrameForJIT), BLRs to the callee, and pops the frame
(j2jPopFrame) on ExitReturn.

### Performance optimizations (20ms → 16ms)

1. LTO + minimal j2jPopFrame: Replaced full popFrameForJIT (11 state
   restores) with minimal pop (decrement frameDepth + restore
   method/receiver for GC). LTO inlines pushFrameForJIT across TUs.
   Together: fib(28) 20ms → 17ms.

2. Inline pushFrameForJIT: Moved to Interpreter.hpp for cross-TU
   inlining. Added primKind bits (52:48) to IC extra word during patch,
   independent of JIT compilation — SmallInteger arithmetic methods
   bypassed noteMethodEntry so primKind was never set.

3. pushFrameForJIT micro-opts: Cache nil locally (skip 3 interpreter
   field reads during J2J chains), mark temp alloc as unlikely. No
   measurable change — bottleneck is the ~49-cycle dependency chain
   (state→interp→frameDepth→SavedFrame address).

### Bugs fixed

- j2jPopFrame not restoring framePointer_, argCount_, or bytecodeEnd_
  after callee returned — caused frame corruption in the chain loop.
  Symptom: FP jumped from 15 to 22 between chain iterations during
  sieve's from:to:put: J2J call to min:.

- _2 SimStack stencil variants missing s->ip update in overflow paths,
  causing ArithOverflow re-execution at wrong bytecode position.

- Clang -O2 cold-section splitting orphaned overflow code in
  addSmallInt_2 and mulSmallInt_2. Restructured to use bool-flag
  pattern that keeps both paths in a single function body.

### Benchmark results (2026-04-09)

                  fib(28)   sieve x3
  Interpreter:    47ms       3.2ms
  JIT no-J2J:    85ms       3.2ms
  JIT + J2J:     16ms       3.2ms
  Cog:            2ms        -

J2J gives 5.3x speedup over JIT-without-J2J for send-heavy code (fib).
JIT-without-J2J is slower than interpreter because C++ round-trip per
send (~360 cycles) dominates. 6.17M J2J calls for fib(28) with near-
perfect return rate (23 bailouts from cold IC). Still 8x from Cog.

16ms is the ceiling for C++ j2j_call architecture (~49 cycles/call with
12 callee-saved register spills). Next: Phase 2 primitive prologues for
sieve/sort/dict, then asm J2J trampoline for fib-class benchmarks.

## SimStack Phase 3: Register-Based TOS/NOS Caching

Cache top-of-stack values in ARM64 callee-saved registers x19 (TOS) and
x20 (NOS) to eliminate redundant memory loads/stores in straight-line
push-arithmetic sequences. 53 SimStack stencil variants cover: push (E/1/2),
pop, dup, store, arithmetic, comparison, conditional jumps, and returns.

Key design:

- Stencils write x19/x20 via inline asm WITHOUT clobber lists. The compiler
  doesn't emit STP/LDP save/restore because stencils are leaf/tail-call
  functions with enough caller-saved regs available.

- JIT_CALL macro in JITRuntime.cpp uses inline asm BLR with x19/x20 in
  the clobber list, so the C++ compiler saves/restores them around calls
  into JIT code.

- extract_stencils.py has a build-time safety check that verifies no
  SimStack stencil contains STP/LDP with x19/x20.

Bug fixed: applySimStack had a dangling reference — `auto& bc` was captured
before vector insertions (flush stencils), then used after insert+i++ to
modify the flush instead of the original bytecode.

fib(28) = 20ms (unchanged — fib is send-dominated, SimStack saves are offset
by J2J x19/x20 save/restore overhead). Benefits expected for longer
straight-line methods (sort, dict, sieve benchmarks).

2026-04-08

## J2J Overhead Analysis

Investigated the per-call overhead of j2j_call for fib(28) (5.93M calls/run).
Measured a theoretical floor of 16ms with NO frame management (just JITState
save/restore + callee BLR). Production overhead: 20ms. The 4ms gap is frame
management (SavedFrame + interpreter state sync).

Key findings:

1. SavedFrame stores are nearly FREE — the ARM64 store buffer hides them
   behind the callee stencil execution (~0.5 cycles of 10 cycles/call).

2. The REAL overhead is j2j_call's function prologue/epilogue: 12 callee-saved
   registers (6 STP/LDP pairs) are needed for the 8 JITState fields + interp
   pointer + computation intermediates. This costs ~2 cycles per call.

3. Inlining pushFrame/popFrame into j2j_call does NOT help — the compiler
   still uses all 12 callee-saved regs because the 8 JITState save fields
   alone nearly fill them.

4. Reducing register pressure by reading from SavedFrame on the success path
   (safe because GC can't fire during pure JIT) doesn't help either — the
   compiler allocates the same number of regs regardless.

To reach the 5ms target: must eliminate j2j_call as a C++ function entirely.
This requires lazy frame materialization (only push frames when GC/bail
demands it) with stencil-internal J2J (save JITState in stencil's own
callee-saved regs, BLR directly to callee, leaf-function helpers for
frame management).

## J2J Hot Path Optimization

Reduced J2J direct call overhead by eliminating hash map lookups and diagnostic
tracking from the per-call hot path:

1. **Removed trackJ2JEntry/trackJ2JReturn**: These did unordered_map lookups on
   every J2J call/return. Removed from jit_rt_push_frame and jit_rt_pop_frame.

2. **JITMethod derivation by pointer arithmetic**: pushFrameForJIT now derives
   JITMethod* from the entry address (entry - sizeof(JITMethod)) instead of
   looking up the method map. The stencil passes the entry address via
   state->jitMethod before calling the helper.

3. **Cached method header and temp count**: Uses JITMethod's pre-cached
   methodHeader and tempCount fields instead of re-decoding the method header
   Oop on every call.

4. **Merged j2j_call helper**: Combined push_frame + callee_call + pop_frame
   into a single `jit_rt_j2j_call` C++ function (helper ID 10). The stencil
   now saves only `origIP` before calling the merged helper, down from 8
   save/restore pairs with the old separate-helper approach.

5. **IP corruption fix**: Root cause of the j2j_call crash across sessions.
   The stencil advanced `s->ip` past the send BEFORE calling j2j_call. The
   helper saved the modified IP and restored it on success, shifting all
   subsequent IP computations. Fix: stencil saves `origIP = s->ip` before
   modification and restores `s->ip = origIP` after successful return. The
   bail path does NOT restore (needs innermost callee's state for interpreter).

6. **Inline arithmetic stencils**: Bytecodes 0x60-0x6F (Send Arithmetic
   Message) are handled by dedicated stencils (stencil_addSmallInt,
   stencil_subSmallInt, stencil_lessThanSmallInt, etc.) that perform
   SmallInteger operations inline without message sends. In fib(28), only
   2 of 18 bytecodes use stencil_sendJ2J (the recursive calls); all
   arithmetic uses inline stencils.

**Benchmark results** (fib(28), 5 runs):

  Configuration       Time      Speedup vs interpreter
  Interpreter         ~50ms     1.0x
  JIT (hash map)      ~24ms     2.1x
  JIT (ptr arith)     ~22ms     2.3x
  JIT (merged j2j)    ~20ms     2.5x
  Cog (ref)           ~2ms      25x

**Remaining bottleneck** (~60 cycles/J2J call at 1.03M calls):

  SavedFrame save/restore    ~22 cycles   11 stores + 10 loads (GC roots)
  JITState save/restore      ~16 cycles   8 fields in j2j_call locals
  Function call overhead     ~15 cycles   1 indirect GOT call
  pushFrame setup            ~7 cycles    bytecodeStart, callee init

To reach 5ms target: need lazy frame materialization (only push interpreter
frame when GC requests it).

## PHARO_BENCH Mode for Benchmarking

Added dedicated benchmark mode (PHARO_BENCH=1 env var) for reproducible
JIT performance measurements. Features:

- PHARO_FIB_N env var to set fib argument (default 28)
- Process switching suppressed during benchmark for clean timings
- Extracted handleBenchComplete() for benchmark result reporting
- Interpreter fib(28) baseline: ~47ms

JIT benchmarks currently hang due to a J2J chain loop bug: ExitSend fires
incorrectly for the fib base case (fib(1)/fib(0)), causing the chain to
loop instead of returning the base value. J2J nested return optimization
is the next priority.

## Session Handler Fix: isImageStarting + P80 Startup Boost

Fixed two bugs preventing Pharo session handlers from firing on image resume:

1. **isImageStarting = false on resume**: `SnapshotOperation>>doSnapshot` stores
   `isImageStarting := snapshotPrimitive result` at bytecode pc=59. On resume,
   our VM patched the stack top to `true` but the receiver's slot 0 stayed
   `false`. `performSnapshot` then called `quitPrimitive` instead of running
   session handlers. Fix: walk the context chain on resume and patch slot 0 of
   ALL SnapshotOperation receivers to `true`.

2. **P80 priority needed, not P100**: The startup process was boosted to P100
   but `safeProcessPriority()` rejected priorities > 80 as "corrupt" (returning
   -1). This caused `synchronousSignal` to yield our P80→"P-1" process to the
   P80 Delay scheduler. Additionally, `putToSleep` at P100 would read past the
   end of the scheduler's 80-entry priority array. Fix: boost to P80 (the
   maximum valid priority = timingPriority).

**Benchmark results** (fib(28), FibRunner at system priority 5):

  VM                    fib(28)    Ratio vs Cog
  Reference Cog VM      2ms        1x
  Our interpreter       47ms       24x slower
  Our VM (JIT)          61ms       30x slower (slower than interp!)

The JIT is slower than the interpreter because every non-trivial send
exits to C++, does heavy frame management, and re-enters JIT. The send
overhead (~50ns) exceeds the bytecode execution savings. This confirms
Phase 1 J2J direct calls are essential — without them, the JIT is net
negative on send-heavy code like fib.

The 30x gap is the target for Phase 1 J2J optimization. Each recursive
benchFib send exits to C++, does frame management, and re-enters JIT.
fib(28) makes ~1.2M recursive calls ≈ 50ns/call ≈ 150 cycles on Apple
Silicon. Cog does ~5 cycles/call.

Note: a separate crash (Character '/' dereferenced as pointer) prevents
later session handlers (File, DiskStore, FileLocator) from completing.
FibRunner was registered at system priority 5 to run before the crash.
The crash is tracked separately — it blocks the SUnit test runner too.

## Phase 1 J2J: IC Fill from Mega Cache Hits

Added IC fill path in `upgradeICToJ2J()`: when ExitSendCached fires for a
JIT-compiled method not yet in any IC entry, create a new IC entry in an
empty slot. Restricted to immediate receivers (SmallInteger tag=1, Character
tag=3, SmallFloat tag=5) because object-pointer fills cause DNU at
polymorphic send sites where different object types share the IC.

Benchmark results (fib(28)x8 + sort(10K)x3):

  Config                   Real     User CPU    Sys
  Reference Cog VM         0.20s    0.18s       0.02s
  JIT + J2J + IC fill      4.46s    1.08s       0.27s
  JIT + J2J (no fill)      4.45s    1.08s       0.26s
  Interpreter only         5.49s    2.46s       0.33s

JIT vs Interpreter speedup: 2.28x on user CPU time. The Cog VM is still
~6x faster (0.18s vs 1.08s), but our VM total includes heavy startup
overhead (~0.6s). Benchmark-only speedup is higher.

IC fill adds only 2 extra J2J entries (9 vs 7) — most IC entries are
already populated by the normal patchJITICAfterSend path. The fill
path's benefit will increase with more diverse call sites.

JIT stats at benchmark time: 299 compiled methods, 95% IC hit rate,
78% J2J activation rate, 52% J2J return rate.

New primitive prologues: stencil_primAt (60), stencil_primAtPut (61),
stencil_primSize (62). Handle Array (format 2) operations inline in the
JIT prologue. Enables J2J direct calls for array access methods that
were previously blocked by the unsafePrim guard.

JIT-internal J2J metrics: 97% stencil success rate during fib(28),
confirming recursive calls stay in machine code. Sort benchmark shows
lower improvement due to push_frame/pop_frame overhead per array access.

Environment variable controls:
  PHARO_NO_JIT=1      - disable JIT entirely
  PHARO_NO_J2J=1      - disable J2J optimization
  PHARO_NO_IC_FILL=1  - disable IC fill from mega cache

## Fix JIT Non-Local Return (NLR) Bug in FullBlocks

Bytecodes 0x58-0x5C (ReturnReceiver through ReturnTop) in FullBlocks were
incorrectly compiled as simple block returns. In Sista V1, these are
Non-Local Returns that must return from the enclosing method. The JIT
now deopts to the interpreter for these bytecodes in FullBlocks.

This was the root cause of the MAX_COMPILE=115 startup failure: a
bounds-checking block on Object was JIT-compiled and its NLR (^self)
was mishandled, corrupting the session startup chain and causing
DNU "#+ not understood by nil".

## Updated Benchmark Results (after NLR fix)

  Benchmark         Interpreter   JIT (warm)   JIT speedup
  fib(28) cold      99ms          312ms        -3.2x (compilation overhead)
  fib(28) warm      57ms          60ms         ~1.0x (break even)
  sort(10K) cold    30ms          246ms        -8.2x (compilation overhead)
  sort(10K) warm    29ms          29ms         ~1.0x (break even)
  dict(5K) cold     15ms          37ms         -2.5x (compilation overhead)
  dict(5K) warm     14ms          13ms         ~1.0x (break even)

JIT warm performance matches interpreter — confirms the stencils execute
at interpreter speed but every send exits to C++. Phase 1 (J2J direct
calls) is needed to see actual speedup.

## Fix J2J Metaclass Bug

J2J IC patching for class/metaclass receivers (format-1 objects) caused
`errorNotIndexable` on `RelativePath class` during path resolution. When
a bail-out re-executes the callee from scratch, metaclass state gets corrupted.

Fix: skip J2J IC patching for format-1 (class) receivers in
`patchJITICAfterSend`. Instance-level J2J stays enabled. With this fix,
J2J runs clean for 6.7B+ interpreter steps with zero errors and zero bans.

## JIT Benchmark Profiling

Measured JIT overhead vs interpreter and Cog:

  Mode                    fib(28)  sort(10K)  dict(5K)  sum(10K)
  Cog JIT                 ~2ms     ~1ms       ~1ms      ~0ms
  Our Interpreter (P60)   43ms     21ms       7ms       0ms
  JIT no-J2J (defer 3s)   49ms     23ms       7ms       0ms
  JIT no-J2J (no defer)   114ms    (timeout)  -         -

Root cause: C++ entry/exit overhead per send (~500ns per activation vs
interpreter's ~30ns dispatch). JIT stencils execute at the same speed as
interpreter bytecodes but add massive per-send overhead. J2J saves the
method cache lookup but adds equivalent frame management overhead
(pushFrameForJIT/popFrameForJIT ~500ns each).

Key insight: the copy-and-patch stencils are not the bottleneck. The
bottleneck is 100% in C++ glue between interpreter and JIT. To close the
gap with Cog, need to minimize or eliminate C++ transitions.

## Fix Test Mode End-to-End

`./build/test_load_image <image> test "Kernel-Tests"` now works.
799 classes, 16,453 pass (99.7%) in 600s timeout.

Three issues fixed:
- **Auto-disable JIT in test mode**: JIT adds ~26x overhead for cold code
  (heavy C++ JIT entry/exit transitions per send). Tests now run at full
  interpreter speed. Override with PHARO_NO_JIT=0 to force JIT.
- **Always create Display**: MorphicRenderLoop runs at P40 (not P80 as
  previously assumed). Without Display it spins doing empty cycles.
- **Always pass --interactive**: Passing "test" as image args activated
  STCommandLineHandler which installed NonInteractiveUIManager, conflicting
  with the SUnitRunner session handler.

## Fix JIT Session Startup (3 bugs)

Three bugs prevented session startup handlers from completing with JIT:

1. **stencil_primClass no-op prologue** (J2J-specific): The primitive 111
   prologue stencil was a no-op stub that fell through to bytecodes without
   computing the class. Via J2J, the primitive never ran, causing species to
   return wrong values (ByteString instance instead of class), triggering
   #new: DNU in DiskStore>>startUp:. Fix: removed case 111 from
   primitivePrologueStencil() so class methods aren't marked as having a
   prologue. The unsafePrim guard then blocks J2J correctly.

2. **Aging threshold too aggressive**: Headless mode used 5ms aging threshold
   which aged the P79 startup process after just 5ms. Fix: exclude P79 from
   aging (agingMaxPri 80→78) and increase headless threshold to 500ms.

3. **J2J exception handler context chain broken** (IN PROGRESS): When a J2J
   callee triggers an exception, the exception handler search fails to find
   handlers in ancestor contexts. The `onErrorDo:` wrapper in
   `executeStoringError:` is not found, causing UnhandledError. Workaround:
   use PHARO_NO_J2J=1 to disable J2J during test runs. Root cause under
   investigation — likely related to context materialization when J2J frames
   are on the savedFrames_ stack.

**Status**: Test suite runs with JIT enabled (J2J disabled). Full results pending.

2026-04-07

## Verify tryResume Bug Fully Resolved

Confirmed the tryResume corruption at 119+ compiled methods (reported in
previous session) is fully fixed by the two earlier commits:
  - Countdown starvation fix (ee0cc57): charge checkCountdown_ after tryResume
  - selectorBits preservation fix (f4d5a7e): flushCaches preserves IC slot 12

Verification: JIT_MAX_COMPILE=118 vs 119 shows identical behavior. Unlimited
JIT (1000+ compiled methods) runs with no errors, 95% IC hit rate, 3x
throughput improvement from resume. Test runner starts and processes test
classes successfully.

## Fix JIT Hang: tryJITResumeInCaller Countdown Starvation

Fixed a JIT hang that occurred when 119+ methods were JIT-compiled.
Root cause: `tryJITResumeInCaller` never charged `checkCountdown_` after
calling `tryResume`. With enough compiled methods, the resume loop always
found a JIT method to resume in and never yielded to the interpreter's
periodic checks (GC, timer semaphores, process scheduling).

The same charging pattern was already correctly implemented in
`tryJITActivation`'s chain loop — this was simply missed in the
`tryJITResumeInCaller` path.

Also cleaned up temporary bisection diagnostics (JIT_DIAG, stack growth
tracking) while keeping the env var debug flags (JIT_MAX_COMPILE,
JIT_EXCLUDE, PHARO_NO_RESUME, PHARO_NO_CHAIN, PHARO_NO_J2J) as
lightweight debugging tools.

2026-04-06

## Fix primitiveYield Early-Exit

primitiveYield (prim 167) now returns immediately when no other processes
exist at the active process's priority. Matches Cog VM behavior. Previously,
yield in a tight loop would manipulate scheduler state unnecessarily on every
call. The test runner's use of `relinquishProcessorForMicroseconds:` instead
of `Processor yield` is a legitimate adaptation (CPU idle vs process switch),
not a workaround for this bug.

## Fix NLR Through ensure: — Session Startup Works Clean

Fixed two bugs in non-local return (NLR) handling that broke `Symbol>>intern:`
and caused ALL session startup handlers to fail with `#asSymbol` DNU.

Bug 1: `returnFromBlock` loop condition was `while (fd > homeFrame + 1)` instead
of `while (fd > homeFrame)`. The +1 caused the NLR value to be pushed on the
home method's stack instead of its caller's stack. The home method then discarded
the value via `pop; returnReceiver`.

Bug 2: `returnValue` had no NLR continuation for the `frameDepth_ > 0` path.
After ensure: cleanup completed and returned normally, `nlrHomeMethod_` was set
but never checked, silently losing the NLR.

Result: zero DNU messages at startup. All session handlers complete.

## Benchmark Results (First Measurements)

Session handler benchmarks now working. Results on Apple Silicon M-series:

    Benchmark           Cog JIT    Our VM     Ratio
    fib(28)                2 ms    567 ms     283x
    sieve x100            10 ms    762 ms      76x
    sort 100K             15 ms   2519 ms     168x
    dict 50K              10 ms   1722 ms     172x
    sum 1M                 5 ms    777 ms     155x
    5000 factorial         0 ms    541 ms       -
    1M blocks              2 ms    117 ms      59x
    1M getter+yourself     3 ms    486 ms     162x
    100K allocations       2 ms    596 ms     298x

    tinyBenchmarks:
      bytecodes/sec     77.3M      8.3M       9.3x
      sends/sec       1518.6M     24.4M      62.2x

JIT active during benchmarks: 1137 compiled methods, 75% IC hit, 66% J2J
activation ratio. Performance is 76-298x slower than Cog — the JIT helps
with method body dispatch but sends still go through C++ interpreter paths.

## Fix Benchmark Runner Double-Save Requirement

Benchmark session handler (PharoBenchmarkRunner, handler #55 of 56) wasn't
firing on single-save images. Root cause: LGitLibrary startup (#54) triggers
FFI method generation which sends asSymbol to Symbol class (DNU), and the
error cascade terminates the startup process before reaching handler #55.

Fix: run_benchmarks.sh now does a second `eval --save "true"` after injection.
The first resume (by reference VM) completes all session handlers including
LGitLibrary's FFI init. On the second resume (by our VM), the init is already
done and doesn't trigger the DNU.

## Fix cannotReturn: Dead Code Execution (Startup Stall)

Fixed root cause of ~29M step startup stall. When returnFromMethod() at fd=0
couldn't return (nil sender), it sent cannotReturn: which pushed a frame
saving IP one past the return bytecode. When the handler returned, popFrame
restored to that IP — landing in dead code after the method's return
instruction. Garbage bytes were interpreted as sends with nil selectors,
causing infinite DNU loops that stalled the startup.

Fix: back up IP by 1 before sending cannotReturn: in all 4 return-path sites,
so the return bytecode is retried if the handler returns. Pharo startup now
completes to the Morphic world loop.

## Phase 3: SimStack Register Caching

Added 53 SimStack stencil variants that cache TOS/NOS in JITState fields
(simTOS/simNOS) instead of the memory stack. Eliminates redundant LDR/STR
pairs between consecutive push/pop/arithmetic stencils in straight-line code.

Uses JITState field-based caching (offset 112/120 from x0) instead of ARM64
callee-saved registers (x19/x20) because Clang's register save/restore
around tail-call B instructions undoes cross-stencil state.

Stencil variants: _E (empty→one), _1 (one→two), _2 (two→two with NOS spill).
Flush stencils (flush1, flush2) write cached values to memory before barriers
(sends, returns, branch targets, superinstructions).

applySimStack() compiler pass runs after peephole optimization, tracks state
machine (Empty/One/Two), inserts flush stencils at barriers. ARM64-only
(guarded by #ifdef __aarch64__).

## Register Missing FilePlugin Directory Primitives

Registered primitiveDirectoryGetMacTypeAndCreator, primitiveGetHomeDirectory,
primitiveGetTempDirectory as named FilePlugin primitives. These were missing,
blocking SystemResolver / FileLocator resolution chain needed by benchmarks.

2026-04-03

## Fix JIT Super Send Megacache Conflation

Super sends (0xEB / ExtSuperSend) were upgraded to stencil_sendPoly, which
probes a global megamorphic cache. The megacache uses (selectorBits, classIndex)
as its key but does not distinguish normal sends from super sends. A prior normal
send of `#initialize` populated the megacache with the receiver's class method,
and then `super initialize` hit that entry instead of looking up in the
superclass — causing #initialize to call itself infinitely during session startup.

Fix: exclude 0xEB from the sendPoly upgrade so super sends always deopt to the
interpreter, which does the correct superclass-based lookup. Also added
patchJITICAfterSend to the 0xEB dispatch handler to prevent IC patch leakage.

## Fix JIT state.ip for CallPrimitive Methods

`tryJITActivation` used `instructionPointer_` as the JIT's base IP, but
`activateMethod` already advances IP past `callPrimitive` (0xF8). JIT stencils
compute exit IPs as `state.ip + bcOffset` (offsets from bytecodeStart), so using
the already-advanced pointer double-counted the skip. This corrupted the
interpreter's IP for all methods with callPrimitive — notably `on:do:` (prim 199)
and `ensure:` (prim 198). Session startup handlers that use `on:do:` silently
failed, preventing test runner execution under JIT.

## Debug Trace Cleanup

Removed ~315 lines of diagnostic traces from Interpreter.cpp and Primitives.cpp:
send traces, JIT exit/resume traces, RESUME context chain dumps, SAMPLE method
profiling, BYTE-RECV traces, XFER process transfer logging, EXT external call
logging, PRIM62 traces, and the g_xferReason global.

## Inline Getter/Setter Dispatch in Stencil Send

IC layout expanded from 72 to 104 bytes per send site (4 entries x [key, method,
extra] + selectorBits). The extra word encodes trivial method info:
- bit 63: getter (slot index in low 16 bits)
- bit 62: setter (slot index in low 16 bits)
- bit 61: returnsSelf (e.g. "yourself")

On IC hit for trivial methods, the stencil reads/writes the field directly and
continues to the next bytecode without exiting to C++. This eliminates ~500ns
per-send boundary crossing overhead for the most common Smalltalk sends.

The hasSends guard is removed — all methods with sends now execute via JIT.
Non-trivial sends still exit via ExitSendCached with J2J chaining. 67 stencils,
4632 bytes ARM64 code.

## IC Invalidation on Class Changes

primitiveChangeClass (115), primitiveAdoptInstance (160), primitiveFlushCacheByMethod
(119), and primitiveFlushCacheBySelector (120) now flush JIT inline caches.

## Super Send Support (0xEB)

ExtSuperSend now uses stencil_sendPoly instead of deopting. IC works the same as
normal sends — on miss, the interpreter does the super lookup. IP advancement for
2-byte super send fixed in both ExitSendCached handlers.

## Nop Bytecodes

0x54-0x57 (unassigned), 0xDA-0xDF (reserved), 0xEC (Sista inlined primitive) now
use stencil_nop instead of deopting. 0xD9 (unconditional trap) still deopts.

2026-04-02

## Remote Temp Stencils and PushFullBlock Deopt-Resume

Added stencils for remote temp access (0xFB/0xFC/0xFD) — these access temps
through indirection via temp vectors (used by closures capturing outer vars).
Previously deoptimized to interpreter; now execute natively in JIT.

Added stencil_pushBlock for PushFullBlock (0xF9) with ExitBlockCreate exit
reason. Instead of falling to interpreted execution for the rest of the method,
the handler creates the FullBlockClosure via the interpreter's existing
createFullBlockWithLiteral, then immediately resumes JIT at the next bytecode.
Same handler added to tryJITResumeInCaller for the resume path.

66 stencils total (was 62), 3940 bytes ARM64 code.

## J2J Chaining in tryJITActivation

Extended J2J chaining from tryJITResumeInCaller to tryJITActivation. When JIT
code hits an IC-cached send and the target completes entirely via JIT, the
caller now resumes JIT execution in a loop instead of returning to the dispatch
loop. Uses the same frameDepth_ comparison as tryJITResumeInCaller.

Added separate stats counters (J2J-resume vs J2J-act) and count map diagnostics
(tracked/hot method counts) to JIT stats line.

Steady-state with 33 compiled methods: activation chains ~0% (most targets not
JIT-compiled), resume chains 33%. Will improve as more methods get compiled.

## Megamorphic Method Cache

Added a direct-mapped 4096-entry mega cache probed by stencil_sendPoly after
all 4 PIC entries miss. The cache is keyed on (selectorBits, classIndex) and
stores the resolved CompiledMethod bits. The interpreter populates the cache
on every successful method lookup (both cache-hit and full-lookup paths).

The stencil reads the selector Oop bits from IC data (stored by the compiler
at compile time from the method's literal frame or special selectors array)
and probes the mega cache using (selectorBits ^ lookupKey) & 4095.

Result: IC hit rate 25% → 74% in steady-state Morphic render loop. The mega
cache eliminates ~2/3 of interpreter fallbacks for polymorphic send sites.

## Comparison+Jump Superinstructions

Peephole pass fuses comparison bytecodes (< > <= >= = ~=) with following
conditional jumps into single stencils. Eliminates boolean Oop creation,
stack round-trip, and stencil boundary. Each fused stencil is 76 bytes
vs 148 (108+40) for the pair (49% smaller). 12 fused stencils added.

Fixed: identity stencils had off-by-one stack bug (read sp[0]/sp[-1]
instead of sp[-1]/sp[-2]). This made == always return false, preventing
the Morphic render loop from starting. The "infinite loop" observed when
fixing the bug was actually the image correctly starting up and entering
its normal world loop. Identity+jump fusion now unblocked.

## JIT-to-JIT Call Chaining

When a JIT-compiled method hits an IC-cached send (ExitSendCached) and the
target completes entirely via JIT (ExitReturn inside activateMethod's
tryJITActivation), the caller resumes JIT execution without returning to the
dispatch loop. Uses frameDepth_ comparison to detect target completion —
method_ identity fails for self-recursive calls. Added inJITResume_ guard
to prevent re-entrant tryJITResumeInCaller from returnValue during chaining.

Result: IC hit rate 64% → 68%, 0 crashes.

## Critical IC Fix: Correct Receiver for IC Patching

patchJITICAfterSend was using stale receiver_ (set during activateMethod,
which runs AFTER patching) instead of the actual send receiver from the
stack. This caused IC entries to be patched with the wrong classIndex,
making all subsequent IC checks miss. Now passes the actual receiver.

Also fixed: nArgs for special selectors (0x70-0x7F) was hardcoded to 1;
now uses a lookup table matching the spec. Extended sends (0xEA) and
special selectors now also get polymorphic IC (were using no-IC stencil_send).

Added inline stencils for == (stencil_identicalTo) and ~~ (stencil_notIdenticalTo)
that execute identity comparison inline without any deopt.

Result: IC hit rate 64% (was 0% steady-state). 63 compiled, 50 IC patches.
46 stencils, 2652 bytes ARM64 code.

## Polymorphic Inline Caching (4 entries per site)

Upgraded from monomorphic IC (1 class/method pair per send site) to
polymorphic IC (4 pairs). stencil_sendPoly unrolls 4 IC entry checks.
On miss, interpreter fills the next empty slot (deduplicates, stops at 4).
Stale IC invalidation clears all entries.

Result: 8181 IC hits (2x monomorphic), 19 sites patched, 15% early hit rate.
stencil_sendPoly: 196 bytes (was 124 for sendMono).

## Eliminate JIT Compilation Bail-Outs

All 7 compilation bail-outs were caused by truncated multi-byte bytecodes
at the end of CompiledMethods. These are dead code after return bytecodes
(unreachable paths, unassigned opcodes like 0xE6). The decoder now stops
gracefully instead of failing when a multi-byte bytecode extends past the
bytecodes array length.

Result: 0 bail-outs (was 7), 42 compiled methods (was 35), 100% success rate.

## Phase 6: Monomorphic Inline Caching (IC)

Per-site inline caches for JIT send bytecodes. Each send site gets 16 bytes
of IC data (classIndex + methodOop) stored in the code zone after the
bcToCode table. On first miss, the interpreter patches the IC with the
resolved method's class and Oop. On subsequent executions, if the receiver's
classIndex matches the cached class, the stencil exits with ExitSendCached
and the interpreter directly activates the cached method (skipping lookup).

Implementation:
- stencil_sendMono: replaces stencil_send for real sends (opcode >= 0x80).
  Loads IC data pointer from literal pool via GOT mechanism (OPERAND2).
  Checks receiver classIndex against cached class. IC hit: ExitSendCached.
  IC miss: ExitSend with icDataPtr set for patching.
- IC data allocation: computed during compilation. 16 bytes per send site
  (8-byte aligned), stored after bcToCode table in code zone. Zero-initialized.
- OPERAND packing: (argCount << 16) | (bcOffset & 0xFFFF) encodes both the
  deopt bytecode offset and argument count in a single 32-bit operand.
- ExitSendCached handler: validates cachedTarget is a CompiledMethod before
  activating. Stale ICs (wrong classIndex after GC) are invalidated and the
  send falls back to interpreter lookup.
- IC patching: patchJITICAfterSend() called from sendSelector() stores the
  receiver's classIndex and resolved method Oop into the IC data.
- JITState extensions: cachedTarget (offset 88), icDataPtr (offset 96),
  sendArgCount (offset 104), ExitSendCached (exit reason 7).

Bug fixes during IC development:
- Fixed uninitialized icDataPtr in JITState causing SIGSEGV (stencil_send
  doesn't set icDataPtr; stack-allocated JITState had garbage)
- Fixed state.ip calculation for tryJITResumeInCaller: must use bytecodeStart
  (not current IP) because stencils use ip + bcOffset from method start
- Added method_ validity guard in tryJITResumeInCaller loop

Stats at startup: 4K IC hits / 14M IC checks, 16 sites patched.
Low hit rate expected during startup (polymorphic sends dominate).
Steady-state workloads (Morphic loop, collections) will benefit more.

## Phase 5b: On-Stack Re-Entry After Send Deopt

When JIT code deopts on a send and the interpreter handles the send, the
caller can now re-enter JIT execution at the bytecode after the send returns.
Previously, once JIT deopted, the entire rest of the method ran interpreted.

Implementation:
- bcToCodeOffset table: stored after the literal pool in each JITMethod
  allocation. Maps bytecode offsets to machine code offsets for mid-method
  re-entry. Built during compilation, one uint32_t per bytecode.
- JITRuntime::tryResume(): enters JIT code at a specific bytecode offset
  by looking up the code offset in the bcToCodeOffset table. Filters out
  invalid entries (offset 0 = method start, handled by tryExecute).
- Interpreter::tryJITResumeInCaller(): called from returnValue() after
  push(result). Computes current bytecodeOffset, sets up JITState, calls
  tryResume. Chains: if JIT returns from the method, pops frame and tries
  the next caller too.
- ~33% of sends result in a successful JIT resume (8.6M resumes / 26M sends).

## Phase 5: Full Execution — Send Deopt, Heap Writes, Expanded Coverage

Enabled JIT execution of ALL compiled methods. Previously only "pure" methods
(no sends, no heap writes) could execute via JIT (~5.7% of compiled methods).

Send deoptimization:
- Send stencil modified: sets state.ip = state.ip + OPERAND (bytecode offset)
  before exiting. The interpreter resumes at the exact send bytecode with the
  JIT's stack state (receiver + args already pushed).
- New ExitArithOverflow (6) exit reason: arithmetic overflow restores entry SP
  and re-executes the whole method (SP may be inconsistent mid-operation).
- Send operand changed from selector literal index to bytecodeOffset.

Heap writes allowed:
- Generational GC is not implemented — all objects are in old space, so the
  write barrier (isOld && isYoung check) is a no-op. Store stencils write
  directly without barriers, which is equivalent to the interpreter's path.
- TODO: Add write barrier calls to store stencils when gen GC is added.

Bytecode coverage expansion:
- Converted bail-out bytecodes to deopt stencils: PushFullBlock (0xF9),
  SuperSend (0xEB), InlinedPrimitive (0xEC), PushArray (0xE7), block
  returns (0x5D/0x5E), traps (0xD9-0xDF), remote temps, pushThisContext.
  Methods compile even with unsupported bytecodes (just deopt there).
- New stencil: storeLitVar (0xF4) — store to literal variable, no pop.
- Compilation failure rate dropped from 41% to 14%.

Precise arithmetic deopt:
- Arithmetic stencils now set state.ip on overflow (not re-execute method).
  Avoids repeating side effects.

Additional arithmetic stencils: <=, >=, //, \\, bitAnd:, bitOr:, bitShift:
Now 14 of 16 Sista V1 arithmetic special selectors have SmallInteger fast paths.

Result: 43 stencils, 2344 bytes ARM64 code. 145 compiled, 24 failed (86%
success). 100% executable. 115M+ sends, zero crashes.

## Phase 4: JIT Execution Working

Fixed two critical ARM64 relocation patching bugs that prevented JIT-compiled
code from running:

1. PAGEOFF12 scaling: LDR opcode detection used wrong bitmask (0x3B0/0x390),
   so 64-bit load offsets weren't divided by 8. Literal pool reads went to
   8x the intended address. Fixed with correct check (0xEC/0xE4).

2. RuntimeHelper GOT indirection: literal pool stored Oop VALUES (e.g., 0 for
   nil) instead of ADDRESSES (pointer to nilOopBits). Stencils do double
   dereference (pool→addr→value), so storing values caused null pointer deref.
   Now stores addresses, which also keeps values in sync after GC.

Also: W^X management simplified (zone stays writable, toggle to executable only
during JIT calls), hasSends gating (only execute pure methods without sends/
arithmetic/stores), better crash diagnostics (ARM64 register dump in signal
handler).

Result: 2500+ methods compile, pure methods (push/return only) execute
correctly through 19M+ interpreter sends with zero crashes.

## Phase 1: JIT Infrastructure

New directory `src/vm/jit/` with foundational JIT subsystem:
- JITConfig.hpp: Platform/arch detection, auto-disables on iOS
- PlatformJIT.hpp: Cross-platform mmap, W^X (Apple Silicon MAP_JIT +
  pthread_jit_write_protect_np), icache flush, ScopedWriteAccess RAII
- JITMethod.hpp: 72-byte method header, ICEntry (mono/poly/mega), MethodMap
- CodeZone.hpp: 16 MB bump allocator with LRU eviction and compaction

## Phase 2: Copy-and-Patch JIT Compiler

Copy-and-patch engine: compile C++ bytecode handlers with Clang -O2, extract
machine code as "stencils", memcpy+patch at runtime.

31 stencils covering all basic Sista V1 bytecodes (1268 bytes ARM64 code):
push (recvVar, litConst, litVar, temp, self, true/false/nil, 0, 1, dup),
store (popStoreRecvVar, popStoreTemp, pop), return (top, self, true/false/nil),
jump (unconditional, jumpTrue, jumpFalse), arithmetic (+, -, *, <, >, =, ~=
SmallInt fast path), send (generic exit to interpreter).

Build pipeline: extract_stencils.py compiles stencils.cpp, parses Mach-O ARM64
relocations (BRANCH26, GOT_LOAD_PAGE21/PAGEOFF12), generates
generated_stencils.hpp with byte arrays + relocation tables.

JITCompiler walks bytecodes, copies stencils into CodeZone, patches ARM64
relocations using a literal pool for GOT-style operand loading.

JITRuntime: compile-on-2nd-call threshold, tryExecute entry point, runtime
stubs for send/return/overflow exit to interpreter.

---

# What's New in Build 122

Build 122 — 2026-04-01

## Fix image relaunch hang (displays but can't interact)

Root cause: Three categories of global state were not reset between image
launches, causing the second image to display but be unresponsive to input.

Primary bug — SDL2 event polling never re-enabled:
- `stub_SDL_PollEvent()` used a function-local `static bool` to enable
  event polling on first call. After `vm_destroy()` reset the event queue
  (clearing `sdl2EventPollingActive_`), the static was still `true`, so
  `setSDL2EventPollingActive(true)` was never called on second launch.
  Result: touch/mouse events were silently dropped.
- Fix: moved the flag to file scope so `resetAllFFIState()` resets it.

Plugin state not cleaned up on VM destroy:
- SocketPlugin: I/O monitor thread kept running with stale socket list
  and stale `vm` pointer. Added `socketPluginShutdown()` call.
- MIDIPlugin: `gInitialized` flag was never reset, so `midiInit()` was
  skipped on relaunch, leaving stale CoreMIDI handles. Added `midiShutdown()`.
- SoundPlugin: Audio queue not stopped. Added `soundStop()` call.

Session ID and emergency debugger flag:
- Socket session ID now incremented on shutdown so stale handles from the
  previous image are rejected immediately.
- Emergency debugger flag reset so second image starts clean.

---

# What's New in Build 118

Build 118 — 2026-04-01

## Fix download memory cleanup + add jetsam pressure logging

Investigating a crash report: download Pharo 14 image → immediate launch →
crash during initial window draw.

Download cleanup fixes:
- URLSession now invalidated after download completes (was leaking internal
  connection pool state)
- Temp ZIP file deleted after extraction (was leaving 50-70MB in tmp/)
- Cancel path also invalidates the session

Memory pressure monitoring:
- Logs physical memory footprint (what jetsam watches) before and after
  vm_init, so crash reports show how much headroom was available
- DispatchSource monitors system memory pressure events (warning/critical)
  and logs them with current footprint — if jetsam kills the app, the
  preceding log lines will show the ramp-up

---

# What's New in Build 117

Build 117 — 2026-03-31

## Fix scroll-then-tap bug on iPhone welcome screen

Fixed two-finger scroll on iPhone causing the Pharo 14 Welcome Browser to
reset scroll position when tapping afterward. Two issues:

- SDL_GetMouseState returned stale position during wheel events, causing
  OSSDL2BackendWindow to dispatch scroll events at the wrong coordinates.
  Now updates mouse position from the scroll gesture center.

- Horizontal drift in vertical two-finger swipes triggered accidental
  page navigation in SpMillerColumnPresenter. Added axis-locking: once the
  dominant scroll axis is detected (>4pt movement), cross-axis deltas are
  zeroed out.

Reported by Tim.

---

# What's New in Build 115

Build 115 — 2026-03-31

## Trivial getter/setter inlining (Phase 5 of pseudo-JIT plan)

Detect trivial accessor methods at method cache time and bypass Smalltalk
activation entirely on cache hit. For getters (pushRecvVar + returnTop),
replace receiver on stack with inst var value. For setters (popStoreRecvVar +
returnReceiver), store arg in inst var and leave receiver. Eliminates frame
push/pop for millions of accessor calls.

Benchmark: ~6.4% CPU improvement (17.49s → 16.37s avg, 3 runs).
Combined with superinstructions: ~9.1% total (18.00s → 16.37s).

## Speculative superinstructions (Phase 3 of pseudo-JIT plan)

Profile-guided bytecode pair fusion. Instrumented the dispatch loop to count
all 65536 possible bytecode pairs over 100M bytecodes of the full test suite,
then implemented speculative fusion for the hottest patterns:

- SmallInt comparison + conditional jump (5.3M pairs): Branch directly on
  comparison result without creating a boolean object
- push1 + arith+ (2.2M, 65% hit rate): Inline x+1 for SmallInt
- pushNil + spec== (1.7M, 46% hit rate): Inline nil identity check
- dup + pushNil + == + jumpFalse (1.45M, 96% hit rate): Full nil-check idiom
- spec== (0x76) fast handler with branch fusion (2.4M): Identity comparison
  inlined as rawBits comparison, bypasses method lookup entirely

Benchmark: ~2.8% CPU improvement (18.00s → 17.49s avg, 3 runs each).
Below estimate because Apple M1's branch predictor already handles dispatch
jump tables efficiently — dispatch overhead is smaller than expected.

# What's New in Build 114

Build 114 — 2026-03-31

## Computed goto dispatch (Phase 1 of pseudo-JIT plan)

Replaced the while loop + dispatchBytecode() call in interpret() with
GCC/Clang computed goto labels. Fast-path handlers for ~200 of 256 bytecodes
(push/pop/store/dup, jumps, SmallInt arithmetic, FullBlockClosure value/value:,
literal sends). Complex bytecodes (returns, closures, extension bytes) route
through existing dispatchBytecode() via slow path.

Benchmark: ~2.5% CPU improvement (18.44s → 18.00s avg, 3 runs each).
As predicted by research, computed goto on Apple M1 yields only 2-5% due to
excellent branch prediction hardware.

## Credits, licenses, and attribution (GitHub issue #5)

Added proper attribution for all open source code used in the project:

- **THIRD_PARTY_LICENSES**: New file with full license texts for all 14
  upstream projects (Pharo, OpenSmalltalk-VM, IJG JPEG, libffi, SDL2,
  cairo, FreeType, pixman, HarfBuzz, libpng, OpenSSL, libssh2, libgit2)
- **LICENSE**: Updated to reference third-party code and their licenses
- **README.md**: Expanded credits section with all upstream projects,
  their authors, versions, and license types
- **Source file headers**: Added attribution comments to all VMMaker-generated
  plugins (B2DPlugin, DSAPrims, JPEGReaderPlugin, SqueakSSL, JPEGReadWriter2Plugin),
  SqueakSSL platform files (sqMacSSL.c), generated VM headers (vmCallback.h,
  cointerp.h, cogmethod.h, interp.h, cogit.h, vmMemoryMap.h, vmRememberedSet.h,
  cointerp.hpp), compatibility shims (sq.h, sqVirtualMachine.h), and the
  clean C++ VM core files (Interpreter.cpp, ObjectMemory.cpp, Primitives.cpp,
  ImageLoader.cpp, ImageWriter.cpp, FFI.cpp, Oop.hpp, ObjectHeader.hpp)
- **In-app Acknowledgements**: New AcknowledgementsView accessible from
  Settings > Acknowledgements, listing all upstream projects with their
  licenses, copyrights, and project URLs

---

# What's New in Build 113

Build 113 — 2026-03-31

## sendSelector() hot path cleanup + 2-way method cache

Two more interpreter optimizations, combined 19% CPU reduction:

- **sendSelector cleanup**: Moved selector byte extraction and receiver class
  name logging behind the sendCount_ guard (every 1024 sends instead of every
  send). Removed dead g_watchdogPrimIndex writes. Corruption check now behind
  __builtin_expect.

- **2-way set-associative method cache**: Expanded from 2048 to 4096 entries.
  Added secondary probe with rotated hash. On miss, tries two slots before
  falling through to full lookupMethod(). Reduces conflict misses that force
  expensive class hierarchy walks.

Benchmark: CPU time 20.81s → 16.88s on full test suite (18.9% faster).

---

# What's New in Build 112

Build 112 — 2026-03-31

## Fast interpret() loop

Rewrote the main execution loop for ~13% throughput improvement:

- New `interpret()` method with tight fetch→dispatch→countdown inner loop
- All periodic checks (GC, timer, signals, yield, preemption, display sync)
  consolidated behind a single countdown gate (every 1024 bytecodes)
- Tiered check frequencies: hot checks every 1K, warm every 64K, cold every 100K
- `test_load_image` now calls `interpret()` directly with a monitoring thread
  instead of per-bytecode `step()` loop
- Extracted `handleForceYield()` for scheduler round-robin logic

Benchmark: test suite total 5731ms → 5002ms (12.7% faster).

## Build 111 — Flat switch dispatch

- `dispatchBytecode()` rewritten from if-else chain to flat `switch(bytecode)`
- Dropped V3PlusClosures bytecode support (requires Pharo 10+ / Sista V1)
- Compiler emits jump table for O(1) dispatch

Combined builds 111+112: ~5.8x faster than build 110 (ecd0d70 era).

---

# What's New in Build 108

Build 108 — 2026-03-27

## Eliminate sub-pixel rendering timing gap

Sub-pixel text rendering is now disabled from C++ before any Smalltalk code
runs, eliminating the ~100ms timing gap where the render loop could hit the
unimplemented sub-pixel primitive and mark morphs with red error boxes.

The VM navigates the FreeTypeSettings class object in the heap to find the
`current` singleton and sets `bitBltSubPixelAvailable` to `false` directly.
Works on both Pharo 13 (value was nil) and Pharo 14 (value was true, baked
into the image from a desktop VM build).

The startup.st early-set, primitive failure, and cleanup fork remain as
defense-in-depth layers.

See `docs/subpixel-rendering.md` for full details.

---

# What's New in Build 107

Build 107 — 2026-03-27

## Startup file split

Refactored the startup patch system from a single monolithic `startup.st`
into version-specific files:

  startup.st        Dispatcher — detects Pharo version, loads the right file
  startup-13.st     Common patches + Pharo 13-specific patches
  startup-14.st     Common patches + Pharo 14-specific patches
  startup-user.st   (optional) User's custom patches — never overwritten

The dispatcher uses `SystemVersion current major` to choose the file.
Auto-generated files are always overwritten on launch; user customizations
go in `startup-user.st`.

See `docs/startup-system.md` for the full documentation.

---

# What's New in Build 106

Build 106 — 2026-03-27

## Pharo 14 image support

Pharo 14 (dev) images now render correctly alongside Pharo 13.

**Sub-pixel text rendering fix**: P14 starts the MorphicRenderLoop before
startup.st can disable sub-pixel rendering, causing
`copyBitsColor:alpha:gammaTable:ungammaTable:` to trigger PrimitiveFailed
and permanently mark morphs with red error boxes. Fixed by having
`primitiveCopyBits` strip the extra sub-pixel arguments and perform a
regular copyBits instead of returning Failure. Rule 41
(rgbComponentAlpha) is already implemented, so text composites correctly.
startup.st still sets `bitBltSubPixelAvailable := false` and clears any
early `#errorOnDraw` marks as a safety net.

**Display Form readiness**: Added `vm_isDisplayFormReady()` flag (set by
primitiveBeDisplay/primitiveForceDisplayUpdate) so MetalRenderer can
render P14's Display Form path in addition to P13's SDL path.

Both Pharo 13 and Pharo 14 images start cleanly with the same app binary.

---

# What's New in Build 105

Build 105 — 2026-03-22

## Fix backspace and arrow keys inserting "?" on Mac Catalyst

Backspace, arrow keys, and other special keys (Home, End, Page Up/Down,
Escape, Tab, Delete Forward) were inserting "?" instead of performing their
expected action. On Mac Catalyst, UIKey.characters for these keys contains
Apple Private Use Area characters (U+F700–F8FF) which were being sent to
Pharo as printable text. Fixed by checking specialKeyCharCode (which maps
UIKeyboardHIDUsage to correct Pharo char codes) before falling back to
key.characters.

Fixes #3.

---

# What's New in Build 101

Build 101 — 2026-03-20

## Control strip follows device orientation (take 2)

Uses UIDevice.current.orientation (physical accelerometer) as the primary
signal for strip placement, with UIWindowScene.interfaceOrientation as
fallback. Build 100 used only interfaceOrientation which is deprecated
since iOS 16 and unreliable on iOS 26.

---

# What's New in Build 99

Build 99 — 2026-03-20

## Export: watchOS companion app

New "watchOS (Companion)" toggle in the Export as App sheet. When enabled, the
generated Xcode project includes a second target for Apple Watch. Building the
iOS target automatically builds and embeds the watch app. The watch companion
is a placeholder SwiftUI app (Pharo does not run on watchOS yet) that shows
the app name and "Open on iPhone or iPad". The watch app shares the app icon
and gets its own bundle ID (main ID + ".watchapp"). Requires watchOS 10.0+.

## Export: Strip Development Tools option

New "Strip Development Tools" toggle in the Export as App sheet. When enabled,
the exported app's startup.st removes IDE packages (Calypso, NewTools, Iceberg,
debugger, tests, Metacello, etc.) on first launch, then runs Smalltalk cleanUp
and garbage collection. Reduces image size by ~30-40 MB. Enabled by default.

## Export: Custom App Icon picker

New "App Icon" section in the Export as App sheet. Pick a PNG or JPEG image file
to use as the app icon. The selected image is bundled into Assets.xcassets and
referenced at all required sizes (Xcode handles scaling). Shows a preview
thumbnail in the export sheet. Replaces the previous placeholder-only behavior.

## Fix: Strip buttons hidden behind notch on iPhone (Issue #1)

On all notch iPhones (X through 14, 16e, mini models), the modifier strip
buttons were partially or fully hidden behind the notch in landscape. The
strip is now placed on the opposite side of the screen from the notch/Dynamic
Island, so buttons are always fully visible regardless of device or orientation.

Affected devices: iPhone X, XS, XS Max, XR, 11 series, 12/13 mini,
12/13/14, 16e — up to 64pt of button overlap on iPhone X.

# What's New in Build 93

Build 93 — 2026-03-09

## Fix: Docked keyboard resizes canvas, floating keyboard frozen, iPhone side gaps

Based on Tim's build 92 feedback (screenshot: docked keyboard covers cursor):

1. Docked keyboard now shrinks the canvas: When the docked keyboard appears, the
   MTKView height is reduced so Pharo redraws entirely above the keyboard. The cursor
   and editing area stay visible — like any standard iOS app. Animated in sync with
   the keyboard animation. When the keyboard dismisses, full height is restored.

2. Floating keyboard does NOT resize the canvas: Only docked keyboards (full-width,
   bottom-anchored) trigger the height reduction. Floating keyboards overlay the canvas
   at whatever position the user drags them to (iPad floating keyboard is movable —
   drag the bar at its bottom).

3. iPhone horizontal safe areas: Added `.ignoresSafeArea(.container, edges: .horizontal)`
   so the sidebar extends to the screen edges (notch/Dynamic Island safe areas no longer
   eat horizontal space).

4. Bottom gray bar: Kept as-is — Tim confirms it prevents accidental home indicator swipes.

5. Floating keyboard stability: Height only updates for width changes (Stage Manager /
   rotation). SwiftUI re-renders from floating keyboard @Published state changes that
   only affect height are ignored — prevents the canvas from shifting upward.

# What's New in Build 92

Build 92 — 2026-03-09

## Fix: Restore status bar background + stabilize keyboard layout (7th attempt)

Tim reported build 91 lost the gray status bar (black gap at top) and floating keyboard
still caused some upward shift (less than build 90, but still present).

Root cause: `.ignoresSafeArea()` on ALL edges made the SwiftUI view extend behind the
status bar, but the MTKView was constrained to `safeAreaLayoutGuide.topAnchor` — creating
an undrawn black gap. When the floating keyboard triggered a SwiftUI re-render (via
@Published keyboard state), the safe area was re-evaluated and the top anchor shifted.

Fix (two-part):
1. Changed `.ignoresSafeArea()` → `.ignoresSafeArea(.keyboard)` — only prevents keyboard
   safe area from affecting layout. Container safe areas (status bar, home indicator) are
   still respected, so the system draws the proper gray status bar background.
2. Froze the MTKView top constraint after the first layout pass. Instead of using the
   dynamic `safeAreaLayoutGuide.topAnchor`, the top offset is captured once in
   `viewDidLayoutSubviews()` and locked. Subsequent safe area changes (from keyboard events
   or SwiftUI re-renders) cannot shift the Metal view.

Verified in iPad Pro 13" simulator: status bar visible, docked keyboard shows/hides
without canvas movement, canvas returns to exact initial state after keyboard dismiss.

# What's New in Build 91

Build 91 — 2026-03-09

## Fix: Floating keyboard canvas expansion — full safe area ignore (6th attempt)

Tim reported build 90's GeometryReader approach still didn't work — the entire app's
top coordinate shifts when the floating keyboard appears, which is above what any
SwiftUI child view can control. GeometryReader itself moves when the window grows.

Root cause: `.ignoresSafeArea(edges: [.bottom, .horizontal])` still respected the TOP
safe area. When a floating keyboard triggers a safe area recalculation on iPad, the top
safe area changes, causing the window/scene to expand upward past the status bar.

Fix: changed to `.ignoresSafeArea()` (ALL regions, ALL edges). This prevents the
keyboard from affecting layout on ANY edge — the view fills the entire screen and
keyboard events cannot change it. The PharoCanvasViewController now uses
`safeAreaLayoutGuide.topAnchor` (UIKit-level, independent of SwiftUI's `.ignoresSafeArea`)
to position the MTKView below the status bar. Removed the GeometryReader (no longer needed).

# What's New in Build 90

Build 90 — 2026-03-09

## Fix: Floating keyboard canvas expansion — GeometryReader size lock (5th attempt, obsoleted by build 91)

Tim confirmed sidebar buttons now work correctly (build 89), but canvas still grows
when floating keyboard appears (status bar disappears, gap at bottom). The MetalRenderer
guard from build 89 caused a visible flicker (texture rendered at old size in new-size view).

Root cause: when keyboard @Published properties trigger a SwiftUI re-render, the layout
re-evaluates safe areas. On iPad with a floating keyboard, this causes the view to expand
past the status bar area. Neither `.ignoresSafeArea(.keyboard)` nor MetalRenderer guards
can cleanly prevent this — the view is already resized by SwiftUI before our code runs.

Fix: wrapped the HStack in a `GeometryReader` that captures the pre-keyboard size and
locks the HStack to it while any keyboard is visible. Width changes still propagate
(for orientation/Stage Manager). Removed the MetalRenderer guard (no longer needed since
the view itself doesn't resize). Removed `.ignoresSafeArea(.keyboard)` (was causing
expansion in previous attempt).

# What's New in Build 89

Build 89 — 2026-03-09

## Fix: Floating keyboard on iPad — sidebar buttons and canvas shift (3rd attempt)

Rewrote keyboard handling based on Tim's detailed bug report (build 88).

Root causes:
1. Sidebar button hiding was driven by `keyboardVisible` which the button handler
   set immediately, BEFORE the keyboard notification could detect floating vs docked.
   For floating keyboards, `keyboardWillShow` often doesn't fire at all on iPad,
   so the sidebar permanently thought the keyboard was docked.
2. iOS keyboard avoidance was resizing the canvas (Metal view) despite
   `.ignoresSafeArea(.keyboard)`, shifting the entire view upward and creating a
   gap at the bottom.

Fixes:
- Replaced `keyboardFloating` with `keyboardDocked` — sidebar hides buttons ONLY
  when we have positive confirmation of a docked keyboard from the notification.
  Default is "not docked" so floating/missing notifications keep all buttons visible.
- Switched from `keyboardWillShow`/`keyboardWillHide` to `keyboardWillChangeFrame`
  which fires for ALL keyboard state changes (dock, undock, float, show, hide).
- Keyboard button handler no longer sets `keyboardDocked` — only the notification
  handler can. Button handler only sets `keyboardVisible` for the highlight state.
- MetalRenderer now tracks pre-keyboard display size and freezes the Pharo display
  while any keyboard is visible (prevents both shrinking and expanding).

## Fix: Floating keyboard causing canvas to grow past status bar (build 89 update)

Tim confirmed sidebar fix works but canvas still grows when floating keyboard appears.
The `.ignoresSafeArea(.keyboard)` modifier (with implicit `edges: .all`) was causing
SwiftUI to expand the view past the top safe area (status bar) during keyboard-related
layout recalculations. This is redundant because `.ignoresSafeArea(edges: [.bottom, .horizontal])`
already ignores ALL safe area regions (including keyboard) on the bottom edge.

Removed `.ignoresSafeArea(.keyboard)`. Also broadened the MetalRenderer guard to freeze
the Pharo display size during ANY keyboard visibility (not just docked), preventing both
shrinking and expansion.

# What's New in Build 87

Build 87 — 2026-03-09

## Fix: Floating keyboard still hiding sidebar buttons and pushing canvas (obsoleted by build 89)

Two issues with the floating keyboard fix in build 85:

1. When the keyboard was shown via the sidebar toggle, `keyboardVisible` was
   set to `true` before the `keyboardWillShowNotification` had a chance to
   set `keyboardFloating`. This caused a render frame where `keyboardDockedVisible`
   was true, hiding the extra sidebar buttons. Fix: don't clear `keyboardFloating`
   on hide (preserve it for the next show), and update `keyboardFloating` BEFORE
   `keyboardVisible` in the show notification so both change atomically.

2. The floating detection (`frame.maxY < screenHeight`) was unreliable on iPad
   because a floating keyboard can sit at the bottom of the screen. Now also
   checks keyboard width — floating keyboards are narrower than 90% of screen
   width.

3. SwiftUI's built-in keyboard avoidance was pushing the entire HStack (strip +
   canvas) upward when the keyboard appeared, hiding the Pharo menu bar. Added
   `.ignoresSafeArea(.keyboard)` to prevent this — the canvas should never move
   for the keyboard.

## VM relaunch support

The VM can now be stopped and restarted without calling `exit(0)`. When the
Pharo image quits (primitiveQuit), the app tears down all C++ objects
(Interpreter, ObjectMemory, DisplaySurface) and resets FFI, event queue,
and InterpreterProxy state. The user is returned to the image library and
can launch a new image without restarting the app.

Implementation:
  - All functional `static` locals in Interpreter.cpp and Primitives.cpp
    converted to member variables (reset automatically on delete+new)
  - `vm_destroy()` added to PlatformBridge — deletes all C++ objects
  - `ffi::shutdownFFI()` expanded to reset SDL2 stubs, module cache,
    mouse/keyboard state, poll countdown, text input state
  - `EventQueue::reset()` clears events and resets semaphore indices
  - `resetInterpreterProxy()` clears plugin proxy pointers
  - PharoBridge.swift: `handleVMExit()` calls `vm_destroy()` and sets
    `isRunning = false` instead of `exit(0)`, returning to image library

---

# What's New in Build 85

Build 85 — 2026-03-09

## Fix: Floating keyboard corrupts sidebar strip on iPad

When using the iPad floating (undocked) keyboard, the sidebar strip
unnecessarily switched to the reduced button set. After switching apps and
returning, iOS would fire keyboardWillHide (clearing our state) then
keyboardWillShow (re-showing the floating keyboard), leaving the strip in a
corrupted state where the keyboard toggle was out of sync.

Fix: Listen for keyboardWillShowNotification and detect floating vs docked
by checking if the keyboard frame reaches the screen bottom. When the keyboard
is floating, the strip keeps its full button set. The keyboardWillShow listener
also re-syncs keyboardVisible on app resume, preventing state desync.

## Fix: Duplicate "Pharo 13 (latest)" names in image library

When downloading a new image from the same template, the previous image kept
its "Pharo 13 (latest)" display name, producing two identically-named entries.
Now the old entry reverts to its actual image filename (e.g.,
"Pharo13.1-64bit-f201357") and only the newest download keeps the template
label. Manually renamed images are unaffected since their name no longer
matches the template label.

## Bug Fix: Finalization process never received signals

The finalization process (P50) was never getting signaled after GC because
`signalFinalizationIfNeeded()` had a priority guard (`if (activePri >= 51) return`)
that was always true — the active process during GC is typically at P79/P80
(startup handlers). This meant weak reference cleanup, ephemeron finalization,
and FinalizationRegistry callbacks never ran.

Fix: removed the priority guard entirely. The Cog VM signals the finalization
semaphore unconditionally and lets the scheduler handle preemption. The P50
finalization process now properly wakes up and processes mourners.

Verified: FinalizationRegistryTest 6/6 pass, WeakAnnouncerTest 34/34 pass.

---

# What's New in Build 84

Build 84 — 2026-03-08

## Bug Fix: iPad context menu items still not activating (revised)

Build 83's fix (clearing suppress flags in .ended) was insufficient — Tim
reported menus still close without activating on iPad. Root cause: on iPad,
UIKit can deliver a delayed `touchesCancelled`/`touchesEnded` for the original
long-press touch AFTER the gesture recognizer's `.ended` callback fires.
Build 83 cleared the suppress flags in `.ended`, so the delayed callback was
no longer suppressed — sending a spurious button-up at the original long-press
position (outside the popup menu). `MenuMorph >> mouseUp:` interprets a
mouseUp outside the menu as "click outside to dismiss" and closes the popup.

Fix: don't clear suppress flags in `.ended`. Instead, clear them at the start
of the next `touchesBegan` — by then any delayed callbacks from the old touch
have either been consumed by the flag or are irrelevant.

## Cleanup: Remove temporary debug logging

Removed `[EVT-POST]` fprintf from PlatformBridge.cpp and `[SDL-POLL]` fprintf
from FFI.cpp that were added during iPad event investigation.

---

# What's New in Build 83

Build 83 — 2026-03-08

## Bug Fix: iPad context menu items do nothing after long-press (first attempt)

After long-pressing to open a Pharo context menu on iPad, tapping any menu
item (Browse, Debug, etc.) had no effect. Root cause: the `suppressNextTouchEnd`
flag was set when the long-press began but never cleared. Because
`cancelsTouchesInView=true` causes the touch to be *cancelled* (consuming
`suppressNextTouchCancel`), `touchesEnded` never fires for the original touch —
leaving the flag stuck. The next tap's `touchesEnded` was then suppressed,
so Pharo never received the button-up needed to activate the menu item.

Fix: clear both suppress flags in `handleLongPress` `.ended`/`.cancelled`.

NOTE: This fix was insufficient — see Build 84 for the complete fix.

## Bug Fix: Spotter (Shift+Enter) unreliable on iPad hardware keyboard

Pressing Shift+Enter on an iPad hardware keyboard did not reliably open the
Spotter. `shouldHandleKeyInPresses` only routed Cmd/Ctrl combos and
arrow/function keys through `pressesBegan`. Shift+Enter failed both checks,
so the keystroke was dropped. `UIKeyInput.insertText("\n")` fired instead,
but UIKeyInput strips modifier flags — Pharo saw a plain Enter, not
Shift+Enter.

Fix: also route Shift/Option + non-printable keys (Enter, Tab, Escape,
Backspace, Delete) through `pressesBegan`, with a `handledInPressesBegan`
flag to suppress the duplicate `insertText` from UIKeyInput.

## Bug Fix: iPad trackpad/mouse right-click not detected

`buttonMaskToPharo` only checked `event.buttonMask.secondary` inside
`#if targetEnvironment(macCatalyst)`. On iPad with a Magic Keyboard trackpad
or mouse, right-clicks were always treated as left-clicks.

Fix: check `buttonMask` on all platforms (available since iOS 13.4).

---

# What's New in Build 82

Build 82 — 2026-03-08

## Bug Fix: ColorMap shift/mask extraction for pointer Arrays

The BitBlt ColorMap extraction assumed shifts and masks were stored in
IntegerArray/WordArray (raw 32-bit words). In Pharo 13, `ColorMap shifts:`
stores a regular Array of SmallIntegers (pointer object). Reading raw oop
bytes as int32 values produced garbage shift/mask values.

Now handles both pointer arrays (Array of SmallIntegers) and word arrays
(IntegerArray, WordArray) via runtime format detection.

Fixes: BMPReadWriterTest testBmp16Bit.

## Feature: 32-to-8-bit depth conversion with colorMap (rgbMap)

The 32→8 pixel copy path now uses the colorMap as an rgbMap when available.
Compresses 32-bit ARGB to N-bit-per-channel index and looks up the mapped
palette value. Previously always did hardcoded grayscale conversion, which
broke color palette reduction (GIF, FormSet).

Used by: Form>>colorReduced, GIFReadWriter>>prepareToPut:,
FormSet>>asForm.

---

# What's New in Build 81

Build 81 — 2026-03-08

## Bug Fix: File attribute timestamps off by timezone

File timestamps (atime, mtime, ctime, birthtime) were missing the local
timezone offset. Pharo internal time is local time since 1901-01-01, but
the VM was only adding the Squeak epoch delta without `tm_gmtoff`. This
caused `DateAndTime` comparisons to be off by the timezone offset.

Also: macOS birthtime (st_birthtimespec) now returned for single-file
attribute queries (case 12), not just the batch path.

Fixes: FileReferenceAttributeTest testAccessTime, testChangeTime,
testCreationTime, testModificationTime.

## Bug Fix: BitBlt fill for all pixel depths

No-source fill operations (fillBlack, fillWhite) only worked for 1-bit
and 32-bit destinations. Added support for depths 2, 4, 8, and 16
(packed-pixel formats). Also handles negative depths (MSB pixel ordering).

Fixes: FormTest testIsAllWhite, test32BitFormBlackShouldStayBlackAfterSave.

## Feature: BitBlt rule 33 (tallyMap)

Implemented combination rule 33 which builds a pixel value histogram.
Reads destination pixels, uses pixel value as index into the colorMap
array, increments that slot. For 32-bit pixels, compresses ARGB to
15-bit (5 bits per R/G/B channel) before indexing.

Used by: Form>>tallyPixelValues, Form>>colorsUsed, Form>>colorReduced,
GIFReadWriter>>nextPutFrame:.

## Bug Fix: 16-bit self-copy ColorMap transform

The 16-bit same-depth copy path now applies ColorMap shift/mask transforms
when present. BMP reader uses shifts/masks to byte-swap 16-bit pixels.

## Improvement: Auto-compact GC skips ephemeron firing

Auto-compact GC (triggered by allocation pressure) now emulates scavenge
behavior by skipping ephemeron firing and weak nilling. Objects in
ephemerons and weak collections are kept alive but not mourned. Only
explicit GC primitives (130, 131) fire ephemerons.

## Test Suite: Updated non-passing-tests.md

Full suite results: 19,161 tests across 994 classes, 99.1% pass rate.
Zero VM-specific failures. Comprehensive failure categorization added.

---

# What's New in Build 80

Build 80 — 2026-03-07

## Bug Fix: BlockCannotReturn (BCR) resume handling

Contexts that have already returned now store a HasBeenReturnedFrom sentinel
(SmallInteger -1) in their PC slot instead of nil. When executeFromContext()
encounters this sentinel, it sends cannotReturn: instead of resuming the
dead context. This matches the standard Cog VM behavior.

Fixes ProcessTest testResumeAfterBCR.

## Improvement: Test runner LF line endings

Fixed test runner output to use LF (Character lf) instead of CR (Character cr)
on macOS. The CR-only output made result files appear as single-line to Unix
tools (wc -l, grep, etc.).

## Improvement: --image flag respects existing startup.st

When launching with `--image`, PharoBridge no longer overwrites a user-provided
startup.st next to the image. This allows custom test runners and startup
scripts to be placed alongside CLI-launched images.

## Test: Un-skipped display-dependent test classes

Removed TextAnchorTest, TextLineTest, TextLineEndingsTest, FastStepThroughTest,
and DeleteVisitorTest from the headless skip list. These now run normally with
the per-test 130s timeout.

---

# What's New in Build 79

Build 79 — 2026-03-07

## Bug Fix: BitBlt 8-bit Form endianness (BMPReadWriterTest)

Fixed 8→8 BitBlt path to use word-based pixel access instead of flat byte
addressing. Pharo Forms store 8-bit pixels MSB-first in 32-bit words (pixel 0
in bits 24-31). On little-endian ARM, flat byte addressing (`dstRow[dx]`)
put pixels in wrong byte positions, causing pixelValueAt: to return
0xF9000000 instead of 249 for an 8-bit palette index.

Fixes BMPReadWriterTest testBmp4Bit and testBmp8Bit (both are actually 8-bit
BMPs). Root cause of SubscriptOutOfBounds: 4177526785 error.

## Bug Fix: BitBlt ColorMap shift/mask support (BMPReadWriterTest)

Added support for ColorMap objects with shifts and masks arrays in 32→32
BitBlt. The BMP reader uses these to reorder pixel channels (BGRA→ARGB)
after loading raw BMP data. Previously our VM ignored ColorMap objects
entirely, causing color channels to be in wrong positions.

Fixes BMPReadWriterTest testBmp32Bit ("Got Color green instead of Color red").

## New: BitBlt 4→4 and 2→2 depth paths

Added same-depth copy support for 4-bit and 2-bit Forms. Used by
bitPeekerFromForm: for extracting individual pixel values from indexed-color
Forms. Handles MSB-first pixel packing within 32-bit words.

## New: BitBlt 16→16 depth path

Added 16-bit to 16-bit copy support with basic combination rules (AND, OR,
XOR, store, paint). Used by BMP reader for 16-bit Form operations.

# What's New in Build 78

Build 78 — 2026-03-06

## Bug Fix: Write barrier immutability error codes (WriteBarrierTest)

Fixed all immutability-checked primitives to set primFailCode to
PrimErrNoModification (8) when rejecting writes to immutable objects. Previously
primitives returned a generic failure, causing Smalltalk to raise PrimitiveFailed
instead of ModificationForbidden. Also fixed the primitive table to use the
correct immutability-checking implementations for float32/float64 store
primitives (628/629).

Fixes WriteBarrierTest: testMutateByteArrayUsingDoubleAtPut and
testMutateByteArrayUsingFloatAtPut.

## Skipped: FFI callback and system tests that timeout

Excluded FFICallbackParametersTest, FFICallbackTest (require native callback
thunks not supported by this VM), GlobalIdentifierWithDefaultConfigurationTest
(5 tests timeout reading system UUID files), and SystemNavigationTest (hangs
on massive iteration in headless mode).

## Bug Fix: allInstances/allObjects premature GC (ByteSymbolTest)

Fixed primitiveAllInstances and primitiveAllObjects calling fullGC() before
scanning, which collected recently-created objects that Smalltalk code expects
to still exist. Our flat operand stack only scans live entries during GC
(stackBase_ to stackPointer_), unlike the reference VM where Context objects
retain all slots including dead ones. The fix removes the upfront fullGC() and
instead retries with GC only on OOM. Also fixed primitiveFindRoots (prim 216).

Fixes ByteSymbolTest: 4/4 pass (was 1/4). Tests testNewFrom, testAs, and
testReadFromString all create symbols without storing references, then expect
allInstances to find them — the premature GC was collecting them first.

## Bug Fix: Mirror primitives stack handling (MirrorPrimitivesTest)

Fixed four primitives for mirror-mode calls where MirrorPrimitives class is
an extra stack entry. primitiveSize and primitiveIdentityHash used
stackValue(argCount) which read the wrong object in mirror mode. primitiveIdentical
and primitiveNotIdentical used pop()/pop()/push() which leaked a stack entry.
primitivePerformInSuperclass rejected mirror calls (argCount=4 vs expected 3).

Fixes MirrorPrimitivesTest: 40/40 pass (was 27/40).

## About window disclaimer

The Pharo About dialog now shows that the image is running on iospharo
(a community VM, not the official Pharo VM) with a link to the GitHub
source code at https://github.com/avwohl/iospharo. Injected via startup.st.

## Full-image test run: 98.00% pass rate across 28,071 tests

Ran all 2,046 test classes (100% of image) with a 30-second per-test
watchdog. Zero timeouts. 27,510 pass / 39 fail / 391 error / 131 skip.
76% of failures initially attributed to Trait "selector changed!" bug turned out
to be a Pharo 13 image issue (`on:do:on:do:` method missing), NOT a VM bug.
TraitTest (54/54) and ClassTraitTest (5/5) pass on our VM via `buildSuite run`.

## Bug Fix: WriteBarrier for FFI store primitives (615-629)

All 15 FFI byte-store primitives now check `isImmutable()` before writing.
Previously storing into an immutable ByteArray via FFI would silently succeed.
Fixes WriteBarrierTest `testMutateByteArrayUsingDoubleAtPut` and
`testMutateByteArrayUsingFloatAtPut`.

## Bug Fix: BitBlt 8→1 and 16→1 depth conversion (iPad world menu red X)

Added support for 8-bit and 16-bit source to 1-bit destination BitBlt operations.
The shadow drawing canvas uses a 1-bit mask form, and menu icons are 8-bit depth.
Our BitBlt primitive only handled 32→1 and 1→1 dest conversions, causing
`unsupported destDepth=1 srcDepth=8 rule=25` failures on every menu item icon
in the stencil/shadow path. This was the root cause of the red X draw errors
in the iPad world menu.

## Diagnostic: Draw error text visible in red X boxes

When a morph's drawing fails, the red X box now shows the actual error text:
class name, error message, and stack trace — instead of just an opaque yellow X.
This helps identify the root cause of the iPad world menu red X errors (Build 77's
BitBlt fix was necessary but not sufficient). Errors are also logged to stderr
and appended to `draw_errors.txt` in the image directory.

---

# What's New in Build 77

Build 77 — 2026-03-06

## Bug Fix: BitBlt rules 30/31 (alphaBlendConst/alphaPaintConst)

Fixed red X "graphport errors" on menu items when double-tapping or spamming
clicks on iPad/iPhone. Three bugs in primitiveCopyBits:

- copyBitsTranslucent: (argCount=1) was rejected — primitive only accepted 0
- Rules 30/31 were incorrectly routed to the counting code path (rule 32)
- Rules 30/31 were missing from all depth combination switches (32->32, 1->32,
  8->32, 16->32, 2/4->32, and no-source fill)

ShadowDrawingCanvas uses these rules for translucent menu shadows. When the
primitive failed, Pharo raised "Bad BitBlt arg" which fullDrawOn: caught and
displayed as a red X on the affected morph.

## Tests: BitBltTranslucentTest (16 tests)

New test class exercising copyBitsTranslucent: with rules 30/31 across all
depth combinations, alpha values (0/128/255), clipping, and edge cases.
All 16 pass on both the reference Pharo VM and our custom VM.

---

# What's New in Build 76

Build 76 — 2026-03-05

## Bug Fix: iOS Welcome window cut off on iPhone

The Pharo Welcome window was cut off at the bottom on iPhone because the
initial Display Form was created at the default 1024x768 — too large for the
actual screen. Now the display size is pre-set from window bounds on both
platforms, subtracting the ModifierStrip width on iOS so the initial form
matches the Metal canvas exactly. Verified on iPhone simulator (812x393).

## Bug Fix: Image library detail panel layout

The image library detail panel was stealing space from the image table on
small screens. Detail rows now scroll within a capped area, and buttons are
placed above the details so they're always reachable.

---

# What's New in Build 74

Build 74 — 2026-03-05

## Bug Fix: Screen size mismatch on Mac Catalyst

The Pharo Welcome window would extend beyond the visible window area on Mac
Catalyst because the initial Display Form was created at a default 1024x768
before the Metal view reported the actual window size. Now the display size is
pre-set from the current window bounds before the VM starts, so the Welcome
window is correctly sized from the beginning. (Mac Catalyst only — on iOS the
default is used and drawableSizeWillChange corrects it.)

## Bug Fix: Image library layout on iOS

Action buttons (Launch, Rename, Share, etc.) are now above the detail info rows
so they're always visible. Detail rows scroll within a capped area so the image
table keeps enough space on small iOS screens.

---

# What's New in Build 73

Build 73 — 2026-03-05

## New Feature: Export as App

Right-click any image in the library and choose "Export as App..." to generate
a standalone Xcode project. The exported project:

- Embeds the Pharo image and all required frameworks (PharoVMCore, SDL2,
  FreeType, OpenSSL, etc.)
- Boots directly into the Pharo canvas — no image library or splash screen
- Supports both macOS (Mac Catalyst) and iOS targets
- Includes a Kiosk mode option that hides the taskbar, menu bar, and World
  menu so your app fills the screen
- Generates a valid .xcodeproj with no external tool dependencies
- Opens in Xcode where you build, sign, and archive

Images saved on iPad are in standard Spur format and can be exported on Mac.
Workflow: develop on iPad, transfer image, export as standalone app.

## Bug Fix: Weak references to anonymous classes now collected by GC

Anonymous classes (used by Reflectivity metalinks, among others) were never
garbage collected because the class table treated all entries as strong GC
roots. Two fixes:

- Class table entries are no longer strong roots during the mark phase.
  A new `sweepClassTable()` pass nils entries for unmarked (dead) classes.
- `scanPointerFields()` now marks the class of each live object via its
  classIndex (matching the standard Spur VM's `markAndTraceClassOf:` call).
  This keeps metaclasses alive while instances exist.

The `MetaLinkAnonymousClassBuilderTest >> testWeakMigratedObjectsRegistry`
test now passes — weak references to anonymous classes and their ephemeron
keys are properly collected after GC.

---

# What's New in Build 72

Build 72 — 2026-03-04

## Bug Fixes

### Save-and-reload after creating new classes now works
Creating a new Smalltalk class (e.g. in the System Browser), saving the
image, and reopening it previously either crashed, showed a corrupted
screen, froze, or silently lost the new class. Three related bugs in the
garbage collector and image writer were fixed:

1. **Class table pages not marked during GC** — The in-heap class table
   page objects (Arrays inside hiddenRootsObj) were never marked, so the
   compacting GC treated them as dead and overwrote them.

2. **Class table page pointers not updated after compaction** — Even if
   pages survived, the hiddenRootsObj slots pointing to them weren't
   updated when compaction moved objects to new addresses.

3. **New classes never written to in-heap pages** — `registerClass()` only
   updated the C++ vector, never the actual heap pages that get saved to
   disk. Classes created at runtime existed only in memory.

The fix tracks class table page Oops in a C++ vector (`classTablePages_`)
populated during image load and maintained through `forEachMemoryRoot`,
so GC compaction automatically keeps page addresses current without ever
reading from in-heap objects during the fragile compaction phase.

## Tests

New class table integrity test suite (14 tests, 51 checks):
  - Class table pages exist and are consistent after image load
  - C++ classTable matches in-heap pages
  - Pages and classes survive fullGC, sync, and multiple GC cycles
  - hiddenRoots page pointers valid after compaction
  - Save/reload roundtrip preserves all classes
  - classTablePages_ vector matches hiddenRoots after GC
  - classTablePages_ entries are valid heap pointers after GC
  - Forced heap fragmentation + compaction preserves class table
  - New class registration survives GC + save/reload
  - freeListsObj survives GC

---

# What's New in Build 69

Build 69 — 2026-03-04

## Bug Fixes

### Image library date and rename persistence fixed
Previously the "Last Modified" column showed "in 0 seconds" on launch and
image renames were lost across restarts. Root cause: the date decoder
didn't match the encoder (ISO 8601 vs default), so the catalog silently
failed to load every time. Now shows actual .image file modification date.

## Improvements

### Snapshot disk space check and diagnostics
The snapshot.log file now records available disk space, heap size, screen
dimensions, device model, and image file sizes before/after save. If the
volume lacks sufficient free space, the save fails early with a clear
error instead of risking a truncated image file.

---

# What's New in Build 68

Build 68 — 2026-03-04

## Bug Fixes

### Image library "Last Modified" column now shows actual file date
Previously the column always showed "in 0 seconds" on launch because the
date decoder didn't match the encoder (ISO 8601 vs default). The catalog
silently failed to load, re-creating all entries with the current time.
Fixed the decoder and changed the column to show the actual .image file
modification date from disk instead of an internal tracking date.

## Improvements

### iPad strip: more buttons visible when keyboard is showing
Previously the iPad strip collapsed to just keyboard toggle, Ctrl, and Cmd
when the software keyboard was up. Now it also shows Backspace, DoIt,
PrintIt, InspectIt, Spotter (Shift+Enter), and Refactor (Cmd+T) — the
buttons you actually need while typing code.

### New strip buttons: Spotter and Refactor
Added two new buttons to the iPad strip:
  - Spotter (magnifying glass) — sends Shift+Enter to open the Spotter
    search tool. Essential for quick class/method navigation.
  - Refactor (wrench) — sends Cmd+T to open the refactoring menu.
    Much faster than navigating menus for common refactoring operations.

### Image save diagnostic logging
Added phase-by-phase timing logs to the snapshot primitive to help
diagnose reported save/reload issues. The snapshot.log file (written
next to the image) now includes: available disk space, heap size,
screen dimensions, device info, and file sizes before/after save.
Also added a pre-save disk space check — if the volume doesn't have
enough free space for the save, the primitive fails early with a
clear error instead of writing a potentially truncated image.

---

# What's New in Build 67

Build 67 — 2026-03-02

## Improvements

### Device mask script now covers all iPads
The `apply_device_mask.py` geometry overlay tool now supports all iPad
screen sizes (9 entries from iPad mini 5 through iPad Pro 13" M5).
Added SA_top field to correctly render iPad safe areas (top status bar
at 24pt, no side insets) vs iPhone safe areas (side DI/notch insets).
Verified control strip layout on all 6 rounded-corner iPad sizes via
masked simulator screenshots, both with and without keyboard.

### PHARO_AUTO_LAUNCH environment variable
New env var for automated simulator testing. Set
SIMCTL_CHILD_PHARO_AUTO_LAUNCH=1 before `simctl launch` to auto-open
the first available image without user interaction.

---

# What's New in Build 66

Build 66 — 2026-03-02

## Bug Fixes

### Fix startup garbage flash on iPhone
Random colored pixels appeared briefly on iPhone startup before the
Pharo desktop rendered. Three causes:
  - CAMetalLayer drawable textures contain uninitialized GPU memory
    before the first present(). Fixed by setting the MTKView background
    color to grey so an opaque background shows before Metal starts.
  - MetalRenderer delegate was set after the view was added to the
    hierarchy, allowing 1-2 orphaned frames. Fixed by creating the
    renderer before addSubview.
  - Metal textures created in createTexture() had undefined contents.
    Fixed by immediately filling new textures with grey (0xFFEBEBEB).

---

# What's New in Build 64

Build 64 — 2026-03-02

## Bug Fixes

### Fix iPhone strip button clipping with zone-centered layout
The keyboard button was still being clipped by the top-left squircle
corner despite the +6pt margin fix in Build 63. Replaced the fixed-margin
approach with zone-centering: each button group is now centered in its
available zone (top group between corner and DI, bottom group between DI
and bottom corner + home indicator). Uses squircle intrusion at x=2 (strip
padding edge) for more conservative clearance. iPhone 16 top padding goes
from 18pt to 24pt. Verified on iPhone 16 simulator with device mask overlay.

### Enable treat-warnings-as-errors
Added SWIFT_TREAT_WARNINGS_AS_ERRORS and GCC_TREAT_WARNINGS_AS_ERRORS to
both Debug and Release configurations.

### Fix Swift concurrency warnings
Resolved Main actor isolation warnings in PharoBridge.swift (motionManager
capture), PharoCanvasView.swift (keyboardVisible mutation), and
UITextInputTraits conformance ("nearly matches" warnings).

## Internal

- Added test package discovery script (scripts/discover_test_packages.st)
- Added addon test runner (scripts/run_addon_tests.st) for 15 priority
  test packages beyond the core suite

---

# What's New in Build 63

Build 63 — 2026-03-02

## Bug Fixes

### Fix primitiveBitShift overflow for large left shifts
`4 bitShift: 126` (= 2^128) returned 0 instead of the correct
LargePositiveInteger. The __int128 fast path overflowed for shifts where
the result exceeded 2^127-1 (signed __int128 max). Now computes the
magnitude bit-width and only uses the __int128 path when the result fits.
Falls back to the existing magnitude-based shift for larger results.

This was the root cause of all 11 PMArbitraryPrecisionFloat transcendental
math test failures (ln, exp, sin, cos, tan, sinh, cosh, tanh, arcsin,
artanh, IEEEArithmeticVersusIntegerAndFraction). The bug cascaded through
Newton refinement division, AGM computation, and the Salamin ln algorithm.

### Fix iPhone strip button hidden behind Dynamic Island
The Backspace button in the modifier strip was completely occluded by
the Dynamic Island on all DI iPhones. The Spacer-based layout pushed
the bottom button group into the DI zone. Fixed by:
  - Using the exact squircle (superellipse n=5) formula for corner
    padding instead of the heuristic `notchInset * 0.45`
  - Reducing action button size from 26pt to 20pt on DI phones so the
    bottom group fits entirely below the DI
Replaced the fixed-margin approach (squircle intrusion + 6pt) with
zone-centering: each button group is centered in its available zone
(top group between corner and DI, bottom group between DI and
bottom corner). Uses squircle intrusion at x=2 (strip padding edge)
for more conservative clearance than x=4 (button edge).
All 7 buttons now visible on every DI iPhone with generous margins
(verified on iPhone 16 simulator with device mask overlay).
Non-DI phones keep 26pt action buttons.

### Fix WarpBlt quad edge interpolation (sideways thumbnails)
`primitiveWarpBits` had the quad corner interpolation order swapped.
The left edge walked p1→p4 (horizontal) instead of p1→p2 (vertical),
causing WarpBlt output to be rotated 90 degrees. This affected window
thumbnails, `asFormOfSize:`, and any scaled/rotated morph rendering.
Normal BitBlt display was not affected.

### Fix bullet characters in doc browser showing as "?"
Same root cause as the menu shortcut symbol issue (Bug 4 in
image_issues.md): the embedded Source Sans Pro v2.020 font doesn't
include U+2022 BULLET. Startup.st now patches `bulletForLevel:` in
`MicRichTextComposer` to use ASCII `*` and `-` instead.

### Make primitiveAsFloat SmallInteger-only
Removed the C++ LargeInteger-to-double conversion path from primitive 40
(asFloat). LargeIntegers now fall back to Smalltalk's
`LargePositiveInteger>>asFloat` which produces correctly-rounded IEEE 754
doubles matching the reference VM exactly.

---

# What's New in Build 61

Build 61 — 2026-03-02

## UI Improvements

### iPhone strip button positioning refined
Top buttons (keyboard, Ctrl, Cmd) moved slightly further down to fully
clear the squircle corner (45% of leading safe area inset, ~27pt).
Bottom action buttons now properly clear the home indicator and bottom
rounded corner — previously sat at 6pt from the screen edge, now uses
corner clearance + bottom safe area inset (~48pt).

---

# What's New in Build 60

Build 60 — 2026-03-02

## UI Improvements

### iPhone strip top icons no longer pushed too far down
The modifier strip top padding used the full Dynamic Island safe area inset
(59-62pt) as vertical padding, but the DI is centered vertically on screen
and doesn't affect the top buttons — only the rounded corner matters.
Reduced to one-third of the inset (~20pt), which clears the squircle corner
without wasting vertical space.

---

# What's New in Build 59

Build 59 — 2026-03-02

## UI Improvements

### iPhone strip buttons no longer clipped by rounded corner / Dynamic Island
The top modifier buttons (keyboard, Ctrl, Cmd) are now pushed below the
screen's rounded corner and camera cutout area using the device's actual
safe area insets.  Bottom action buttons (backspace, DoIt, PrintIt,
InspectIt) reduced from 32pt to 26pt to fit the available space.

---

# What's New in Build 58

Build 58 — 2026-03-02

## Bug Fixes

### Color inspector swatch now shows correct color
`Color red inspect` (and other colors) showed a gray box in the
inspector's Color tab instead of the expected colored rectangle.

Root cause: the Color swatch path uses `Morph >> asFormOfSize:` which
calls WarpBlt for scaled/smoothed rendering. Our VM did not implement
`primitiveWarpBits`, so the Smalltalk fallback ran — but that fallback
has a bug where `mixPix:` averages R/G/B without preserving the alpha
channel, producing transparent pixels (alpha=0).

Fix: implemented `primitiveWarpBits` in C++ for 32-bit depth (covers
the common case), and patched the Smalltalk `mixPix:` fallback via
startup.st to preserve alpha as a safety net.

### Cairo stub functions now log on registration
Previously, all `cairo_*` FFI calls silently returned 0 with no
indication that Cairo was unavailable. Now the first 5 registrations
are logged to stderr for diagnostic visibility.

## Testing

### Higher-level package test suite
Added test harness (`scripts/run_package_tests.sh`) for running
third-party Smalltalk package test suites against our VM. Loads
packages via Metacello using the stock Pharo VM, then runs tests
with our interpreter.

Results (7974 pass / 8000 total = 99.7%):
  NeoJSON     116/116  (100%)  — JSON parsing, writing, Unicode
  Mustache     47/47   (100%)  — template expansion, closures
  XMLParser  5978/5978 (100%)  — SAX, DOM, XML conformance suites
  PolyMath   1150/1168 (98.5%) — scientific computing, ODE solvers
  DataFrame   651/665  (97.9%) — tabular data, statistics

Most failures are pre-existing bugs in the packages (confirmed by
testing on stock Pharo VM). The only VM-specific failures are 12
tests in PMArbitraryPrecisionFloatTest (transcendental math precision).

See `docs/higher_level_tests.md` for full details.

---

# What's New in Build 57

Build 57 — 2026-03-02

## Bug Fixes

### iPad ModifierStrip no longer overlaps Pharo menu bar
Changed safe area handling so the HStack respects the top safe area
(.ignoresSafeArea only on bottom and horizontal edges). The strip now
naturally starts below the status bar, with a 28pt spacer clearing
the Pharo menu bar. No more UIKit safe area hacks.

### Pharo canvas extends to bottom edge on iOS
The Metal view was double-handling safe area (SwiftUI + UIKit constraints).
Now that SwiftUI manages safe area positioning, the Metal view fills its
hosting view completely — no more gap at the bottom of the screen.

### iPhone strip hides action buttons when keyboard is showing
When the soft keyboard is visible on iPhone, backspace/doIt/printIt/
inspectIt buttons are hidden to prevent them from being pushed off
screen. Keyboard toggle, Ctrl, and Cmd remain visible for typing.
Buttons restore when the keyboard is dismissed.

---

# What's New in Build 46

Build 46 — 2026-03-01

## Bug Fixes

### ModifierStrip safe area positioning (iPhone & iPad)
On iPhone landscape, the strip stays flush to the left edge but splits
buttons into two groups above and below the Dynamic Island / camera
cutout, keeping the strip thin without wasting space. On iPad, the
strip starts below the status bar and Pharo menu bar instead of at the
very top of the screen.

---

# What's New in Build 45

Build 45 — 2026-03-01

## New Features

### Core Motion Plugin
Added a CoreMotionPlugin that exposes real device sensor data to Pharo
via named primitives. Accelerometer, gyroscope, magnetometer, and
device attitude (roll/pitch/yaw) are all available at 60 Hz.

Four named primitives under `CoreMotionPlugin`:
  - `primitiveMotionStart` — start sensor updates
  - `primitiveMotionStop` — stop sensor updates
  - `primitiveMotionData` — read latest sample (Array of 13 Floats)
  - `primitiveMotionAvailable` — hardware availability bitmask

The existing numbered sensor primitives (420-429) now return real data
from the same Core Motion backend instead of zeros.

Usage from Smalltalk:

    <primitive: 'primitiveMotionStart' module: 'CoreMotionPlugin'>
    data := <primitive: 'primitiveMotionData' module: 'CoreMotionPlugin'>.
    "data: accelX,Y,Z gyroX,Y,Z magX,Y,Z roll,pitch,yaw timestamp"
    <primitive: 'primitiveMotionStop' module: 'CoreMotionPlugin'>

On Mac (no sensors), all values read as zero and the availability
bitmask is 0.

### README: TestFlight Installation Instructions
Added a "Installing the Beta" section to README.md with step-by-step
TestFlight instructions, device requirements, and storage estimates.

# What's New in Build 44

Build 44 — 2026-03-01

## UI Improvements

### Strip Button Icons
Replaced text labels (DoIt, Print, Inspect, Debug, Expand, Accept, Cancel)
with SF Symbol icons on both iPhone and iPad. Fixes the "Inspect" label
wrapping on small screens and the confusing "Print" name (now play triangle,
text-append, eyeglasses, ant icons etc.). All buttons now have `.help()`
tooltips visible on Mac Catalyst hover.

### Bottom Safe Area Fix
Canvas now extends to the bottom edge on iPhone and iPad (no gap for the
home indicator). Changed `.ignoresSafeArea(.keyboard)` to `.ignoresSafeArea()`.

### iPhone Strip Cleanup
Removed dividers from the iPhone button strip to save vertical space when
the keyboard is shown.

## Code Quality

### Codebase Simplification (Round 1)
Full review of ~55K lines of C++ and ~3.7K lines of Swift. Removed dead
code, deduplicated logic, and improved efficiency:

  - Removed ~670 lines of dead code (unused diagnostics, commented-out
    debug blocks, unreachable methods, dead Smalltalk scripts)
  - Deduplicated identity hash primitives (fixed 30-bit vs 22-bit mask
    inconsistency between primitiveIdentityHash and SmallInteger variant)
  - Consolidated fixedFieldCountOf, bitwise primitives (BitAnd/Or/Xor),
    and version label formatting into shared helpers
  - Replaced per-pixel display copy with memcpy in syncDisplayToSurface
  - Changed passThroughEvents_ from vector to deque for O(1) front removal
  - Deduplicated build-third-party.sh platform setup (5 functions to 1)
    and meson cross-file generation
  - Added script safety (set -euo pipefail, timeout on app launch)

### Codebase Simplification (Round 2)
Added ObjectMemory utility functions and inlined hot-path operations:

  - Inlined push/pop/stackTop/stackValue/fetchByte into Interpreter.hpp
    with __builtin_expect branch hints for better codegen
  - Added oopToString, nameOfClass, classNameOf, numLiteralsOf, selectorOf
    utilities — replaced ~30 inline string/header extraction patterns
  - Deduplicated keyboard event processing between MTKView and
    ViewController (postKeyDown/postKeyUp + shouldHandleKeyInPresses)
  - Gated recentBytecodes_ recording behind ENABLE_DEBUG_LOGGING flag
    to remove a per-bytecode write from the release hot path

Net result: ~810 lines removed across both rounds.

---

# What's New in Build 42

Build 42 — 2026-03-01

## New Features

### Expanded Modifier Strip (iPad)
The left-side strip now has 16 buttons covering the most common Pharo
operations, so you rarely need to show the soft keyboard:

  - Ctrl, Cmd — modifier toggles (same as before)
  - Tab, Esc, Backspace — direct keys (Backspace is new)
  - DoIt, PrintIt, InspectIt, DebugIt — Pharo evaluation shortcuts
  - Cut, Copy, Paste — clipboard (Cmd+X/C/V)
  - Expand, Accept, Cancel — select enclosing expression (Cmd+2),
    save code (Cmd+S), cancel edit (Cmd+L)
  - Keyboard toggle and Help at the bottom

### Compact iPhone Strip
On iPhone the strip shows a reduced set of 8 buttons that fits the
shorter landscape height:

  - Keyboard toggle at the top (stays accessible when keyboard shows)
  - Ctrl, Cmd, Backspace
  - DoIt, PrintIt, InspectIt

No Help button on iPhone (not enough room).

## Bug Fixes

### iPad Floating Keyboard Blank Space
When the iPad floating keyboard appeared, the Pharo canvas shrank and
left a blank area at the bottom. The canvas now ignores the keyboard
safe area — the keyboard floats over the Metal-rendered content instead
of pushing it up.

---

# What's New in Build 39

Build 39 — 2026-03-01

## New Features

### Left-Side Modifier Strip (iOS)
Replaced the floating circular toolbar buttons with a vertical strip on the
left edge of the screen. The strip stays fixed (no dragging) and provides:

  - Ctrl / Cmd — one-shot modifier toggles (same behavior as before)
  - Tab / Esc — direct key sends
  - DoIt / Print / Inspect — synthesize Cmd+D / Cmd+P / Cmd+I without
    needing to show the soft keyboard. Requested by Tim who found that
    the old Cmd button required showing the keyboard to type the letter.
  - Keyboard toggle and ? help button at the bottom

The canvas fills the remaining width to the right of the strip.

## Bug Fixes

### Menu Shortcut Symbols Show as "?" (reported by Tim)
Menu items showed "?D" instead of "Cmd+D" for keyboard shortcuts. The
embedded Source Sans Pro font (2012) lacks the Unicode modifier key
glyphs (U+2318 ⌘, U+2303 ⌃, etc.) that Pharo's `KMOSXShortcutPrinter`
uses. Adobe added these glyphs in Source Sans 2.040 (2018) but Pharo
still ships the old version. This affects all Pharo VMs, not just ours.

Fix: startup.st now patches `KMShortcutPrinter symbolTable` to use
ASCII text labels ("Cmd+", "Ctrl+", "Shift+", "Opt+") instead of the
missing Unicode symbols.

### Keyboard Toggle Out of Sync
The keyboard toggle button could get stuck in the "on" state when the user
dismissed the soft keyboard via the system globe/dismiss key instead of the
strip button. Now observes `keyboardWillHideNotification` to keep the
toggle in sync.

---

# What's New in Build 38

Build 38 — 2026-02-28

## Bug Fixes

### Cmd+Q Now Quits the App on Mac
Pressing Cmd+Q did nothing on Mac Catalyst — the standard Mac quit
shortcut was silently swallowed.

Root cause: To prevent crashes from Pharo's FFI calls to AppKit menu
APIs on the VM thread, `setMainMenu:` was swizzled to a complete no-op.
This also blocked UIKit/SwiftUI from installing the system menu bar
(including the Quit item). Since Cmd+Q dispatches through the system
menu bar on Mac, there was nothing to handle it.

Fix: `setMainMenu:` now checks `pthread_main_np()` — calls from the
main thread (UIKit creating the menu) pass through to the original
implementation, while calls from the VM thread (Pharo FFI) are silently
ignored. A UIKeyCommand fallback on the view controller provides a
second layer of defense.

---

# What's New in Build 37

Build 37 — 2026-02-28

## Code Review and Cleanup

Full comment-vs-code audit across the entire codebase, fixing stale
comments and correcting actual bugs found during the review.

### Comment Fixes (15 files)
  - Interpreter.hpp: rewrote bytecode summary from V3PlusClosures to
    Sista V1 with correct ranges and descriptions
  - Interpreter.cpp: fixed bytecode range labels, header bit layout
    docs, method comments (createFullBlock, activeContext,
    executePrimitive), startup attempt labels
  - Primitives.cpp: removed wrong thisContext reference, fixed 0-based
    vs 1-based indexing comment, clarified handler/unwind marker
    primitive table slot vs method header primitive index
  - ObjectMemory.hpp/cpp: clarified ClassBlockClosure alias, removed
    duplicated comment
  - ObjectHeader.hpp: format 5 "Weak with fixed" → "Ephemeron",
    format 6 clarified as unused
  - test_load_image.cpp: fixed "256 MB" → "4 GB virtual", added bit
    range labels to header dump
  - FFI.cpp: updated file header to reflect actual contents
  - SocketPlugin.cpp: "TCP" → "TCP/UDP"
  - sqMacSSL.c: fixed filename typo, fixed SqueakSSLRead → SqueakSSLWrite
  - PharoBridge.swift: simplified display callback comment
  - PharoCanvasView.swift: fixed stale vm_setTextInputCallback reference
  - NSEventMonitor.h/m: fixed local → global preferred comment (then
    deleted — see below)

### Bug Fixes Found by Review
  - isSend bytecode ranges in stepDetailed() were wrong: included
    jumps (0xB0-0xBF), stores (0xC0-0xDF), callPrimitive (0xF8),
    and pushClosure (0xF9) as sends; missed 1-arg (0x90-0x9F) and
    2-arg (0xA0-0xAF) sends. Now matches Sista V1 spec exactly.
  - primitiveIndexOf() used full byte for high bits of primitive
    index instead of masking with & 0x1F. The callPrimitive format
    is `248 iiiiiiii mssjjjjj` — only the low 5 bits (jjjjj) are
    the primitive index high bits.
  - numLiterals extraction in FullBlockClosure activation used
    & 0xFFFF instead of & 0x7FFF, including bit 15 (requiresCounters
    flag) in the literal count.

### Dead Code Removed
  - NSEventMonitor.h/m (253 lines) — never called from anywhere,
    superseded by UIKit gesture handlers
  - 9 unused bridging header declarations (deprecated display getters,
    unused VM entry points, unused SDL2 flags)
  - ImageManager.checkForExistingImage() — dead wrapper, load() is
    called directly

### Debug Code Removed
  - Removed unused globals: g_lastDispatchSelector, g_lastDispatchRcvrClass,
    g_lastDispatchMethod, g_lastDispatchPrimIndex, g_lastSelName, g_stepCount
  - Removed ~60 commented-out DEBUG_LOG lines
  - Gated [DNS], [NET], [SOCK], [SSL], [ImageWriter], [DISPATCH] logging
    behind #ifdef DEBUG
  - Gated Swift fputs/NSLog behind #if DEBUG
  - Deleted DeviceLog.hpp and 12 stale scripts

---

# What's New in Build 35

Build 35 — 2026-02-28

## New Features

### Auto-Launch with Countdown Splash
Right-click any image in the library and choose "Set as Auto-Launch" to
mark it as the default. On next app launch, a 3-second countdown splash
appears with the image name and a "Show Library" button to cancel. This
replaces the old automatic behavior (which launched whenever there was
exactly one image with no escape hatch).

Auto-launch images show an orange star icon in the image list. The
preference persists across sessions via AppStorage. Deleting an auto-launch
image automatically clears the preference.

### CLI `--image` Flag
Launch the app with a specific image from the command line:

    open /path/to/iospharo.app --args --image /tmp/Pharo.image

This bypasses both the library and the splash screen for immediate launch.
Useful for automated testing and scripting.

## Bug Fixes

### SSL Data Loss After EOF (Doc Browser Blank Right Pane)
The Build 34 `eofDetected` fix had a secondary bug: after setting
`eofDetected = true`, the I/O thread stopped monitoring the socket entirely
and never signaled `readSema` again. Pharo's SSL layer had buffered
decrypted data but was never woken up to drain it via `recv()`.

Fix: The I/O thread now continues monitoring SOCK_CONNECTED sockets in
`readfds` even after EOF. The MSG_PEEK probe only runs once (gated on
`!eofDetected`), but `readSema` is signaled every 100ms until Pharo's
`recv()` returns 0 and the recv primitive sets SOCK_OTHER_END_CLOSED.
Write monitoring is skipped after EOF (no point signaling write-ready
after FIN).

---

# What's New in Build 34

Build 34 — 2026-02-28

## Bug Fixes

### SSL Read Stall on Connection Close (Doc Browser Blank Right Pane)
Clicking items in the Help Documentation Browser showed a blank right pane.
The tree populated correctly (fixed in Build 33), but document content never
loaded because HTTPS downloads from raw.githubusercontent.com stalled after
receiving partial data.

Root cause: Race condition between the I/O monitor thread and Pharo's SSL
read loop. When the HTTP server sent `Connection: close`, the TCP FIN
arrived while SSL-buffered data remained unread. The I/O thread's MSG_PEEK
detected FIN (recv returned 0) and immediately set SOCK_OTHER_END_CLOSED.
Pharo's `readInto:startingAt:count:` loop checks `self isConnected` on
each iteration — with the state already set to "closed", the loop exited
before draining the SSL layer's internal buffer, losing the tail of the
HTTP response.

Fix: Added `eofDetected` flag to the socket struct. The I/O thread now
sets this flag instead of changing sockState when MSG_PEEK returns 0. The
recv() primitive is the only place that sets SOCK_OTHER_END_CLOSED, and
only when recv() itself returns 0 (meaning the kernel buffer is truly
empty). The I/O thread also stops monitoring sockets with eofDetected set,
preventing spin-signaling of the read semaphore.

Verified: 2.6 MB GitHub API response over HTTPS completes without stall
(previously stalled after ~900 bytes on the third connection).

### Doc Browser Error Handler Crash
Clicking a tree item that failed to load crashed with DNU on
`MicResourceReferenceError >> #message`. The error handler in
`MicDocumentBrowserModel >> document` used `error message` but the
actual Pharo API is `error messageText`.

Fix: startup.st now patches `document` to use `messageText` and wraps
the error in a Microdown `# Error` heading for graceful display.

### Doc Browser Tree Expansion Crash
Expanding tree nodes called `childrenOf:` which had no error handling.
Network failures or rate limits caused unhandled exceptions.

Fix: startup.st overrides `childrenOf:` with comprehensive error
handling — wraps all network calls in `on: Error do:` blocks, returning
empty arrays on failure instead of crashing.

## Logging Cleanup

Removed verbose SSL encrypt/decrypt diagnostic logging from sqMacSSL.c
(added in Build 33 for debugging). Kept only error messages (SSLRead
FAILED, SSLWrite FAILED). SSL handshake logging is still present since
it's infrequent and useful for connection diagnostics.

---

# What's New in Build 33

Build 33 — 2026-02-27

## Bug Fixes

### Help Browser Document Tree Empty
The Documentation Browser (Help > Documentation) opened but showed no
entries in the doc tree — just "I am a directory and has no contents"
when clicking the root node. Three issues were involved:

Root cause: Fresh Pharo 13 images ship with `IceTokenCredentials`
containing the placeholder token `'YOUR TOKEN'`. `MicGitHubAPI` uses
these credentials by default (not anonymous), causing GitHub to return
401 with no rate-limit headers. `MicGitHubAPI >> extractRateInfo:`
then crashes with `KeyNotFound: 'X-Ratelimit-Remaining'`. This is a
Pharo image bug affecting all VMs, not specific to ours.

Fix: The VM now auto-creates a `startup.st` alongside the Pharo image.
Pharo's `StartupPreferencesLoader` loads this script on every startup,
patching `MicGitHubRessourceReference >> githubApi` to use anonymous
API access (`MicGitHubAPI new beAnonymous`). No auth is needed for
public GitHub repos.

Supporting changes:
  - VM sets working directory to the image's parent directory so
    `StartupPreferencesLoader` finds `startup.st` (both in
    `test_load_image` and `PharoBridge`)
  - Added SSL diagnostic logging (fprintf) to `sqMacSSL.c` for
    handshake, encrypt, and decrypt operations

## Diagnostics

### SSL Diagnostic Logging
Added `fprintf(stderr, ...)` diagnostics throughout SqueakSSL
(`sqConnectSSL`, `sqEncryptSSL`, `sqDecryptSSL`). The existing
`logTrace()` macro was compiled out as `((void)0)` in `debug.h`, making
SSL completely invisible in logs. The new fprintf logging shows
handshake progress, data flow, and error codes, which was critical for
verifying that SSL works correctly and the doc browser bug was
image-side, not VM-side.

---

# What's New in Build 32

Build 32 — 2026-02-27

## Bug Fixes

### SqueakSSL Data Loss on Partial Reads
HTTPS connections (used by the Help Browser, Iceberg, and ZnClient) could
silently corrupt TLS records, causing connections to fail or return garbled
data.

Root cause: Apple Secure Transport's read callback requests N bytes of
encrypted data. When the socket buffer had fewer than N bytes available,
SqueakSSLRead() correctly copied what it had and returned errSSLWouldBlock,
but then set dataLen to 0 — discarding the bytes it had just delivered to
SSL. On the next callback, SSL expected the continuation of the same TLS
record but got new data from the socket, corrupting the record boundary.

Fix: after a partial read, dataLen is decremented by the number of bytes
consumed and any remaining bytes are shifted to the front of the buffer
with memmove(). SSL now sees a consistent byte stream across callbacks.

### Write Semaphore Spam (millions of log lines)
The stderr log grew to 2.6 million lines in under a minute, almost entirely
`[SEMA] signalSemaphoreWithIndex(10)` messages.

Root cause: TCP sockets are almost always writable (the kernel send buffer
is rarely full). The I/O monitor thread polls every 100ms with select(),
and every poll signaled the write semaphore for every connected socket.
Each signal also triggered the `[SEMA]` log line in the interpreter proxy.

Fix: added a `writeSignaled` flag to each socket. The write semaphore is
signaled once when the socket becomes writable, then suppressed until a
send() returns EAGAIN (buffer full), which re-arms the flag. This matches
the edge-triggered semantics that Pharo's SocketStream expects.

### Debug Logging Cleanup
Removed verbose diagnostic logging added during socket/SSL debugging:
  - Removed per-call `[SEMA] signalSemaphoreWithIndex(N)` from the
    interpreter proxy (the single biggest source of log spam)
  - Removed `[SOCK]` debug prints from socket creation, connect, and
    status paths (kept error-case prints and one-time init messages)
  - Removed `[DISPATCH]` logging for every Socket/SqueakSSL primitive call

---

# What's New in Build 31

Build 31 — 2026-02-27

## Bug Fixes

### NAT64 Connect Fix
Removed AI_NUMERICHOST flag from the socket connect path. Apple docs
explicitly state that AI_NUMERICHOST prevents IPv6 address synthesis on
NAT64 networks — which is the exact mechanism needed for connectivity on
IPv6-only cellular/WiFi networks with NAT64.

### Socket Creation Diagnostics
Added comprehensive logging to socket creation, connect, and plugin init
paths (`[SOCK]` prefix on stderr / os_log). This helps diagnose the
"Socket destroyed" error reported on real iPad hardware where socket
creation fails but works on Mac Catalyst.

### Enter/Backspace Doubling (reported by Tim)
Enter key inserted two newlines and Backspace deleted two characters.

Root cause: insertText() sent three events per keystroke: down, stroke,
up. For Enter (charCode 13), the down event generated SDL_KEYDOWN(Return)
and the stroke generated SDL_TEXTINPUT("\r"). Pharo processed both as
text insertions. Real SDL2 does NOT generate SDL_TEXTINPUT for Enter,
Backspace, or Tab — only KEYDOWN/KEYUP.

For Backspace (charCode 8), the stroke bypassed the TEXTINPUT path
(not printable) and fell through to the SDL_KEYDOWN condition, generating
a duplicate keydown.

Fix: removed Enter (13) and Tab (9) from the isPrintable set in the SDL
event converter. Non-printable stroke events are now silently skipped.
deleteBackward() no longer sends the redundant stroke event.

## Changes

### Ctrl Button Is Now One-Shot (suggested by Tim)
The virtual Ctrl button on the iOS toolbar now auto-clears after a
touch/click, acting as a one-shot modifier (like Shift on a phone
keyboard). Previously it stayed active until tapped again.

### Virtual Cmd Button (suggested by Tim)
Added a Cmd button to the iOS floating toolbar alongside Ctrl. Tap Cmd
then type a key to send Cmd+key — e.g. Cmd then D for "Do It". Like
Ctrl, it's one-shot: auto-clears after a keystroke or touch.

---

# What's New in Build 30

Build 30 — 2026-02-27

## Bug Fixes

### Crash on Quit (SIGABRT) (reported by users)
Quitting Pharo crashed the app with SIGABRT instead of exiting cleanly.

Root cause: the SocketPlugin I/O monitor thread was a static std::thread
that was still joinable when exit() ran. During atexit cleanup, its
destructor called std::terminate() which aborted the process.

Fix: the I/O thread is now detached after creation so its destructor is
a no-op during process exit.

### Network Connections on IPv6-Only Networks (NAT64)
Expanded the Build 28 DNS fix to fully support IPv6-only networks with
NAT64/DNS64 (common on iPad cellular and some WiFi networks).

Two problems: (1) Pharo 13's NetNameResolver requires exactly 4-byte
addresses — returning 16-byte IPv6 addresses crashes with SizeMismatch.
(2) Even with a valid IPv4 address from DNS, connecting via IPv4 fails
on IPv6-only networks because there is no IPv4 route.

Fix: DNS always returns 4-byte IPv4 addresses (extracting the embedded
IPv4 from synthesized IPv6 when no native IPv4 results exist). The socket
connect primitive uses getaddrinfo() to re-resolve the address before
connecting — on NAT64 networks this synthesizes the proper IPv6 address
from the IPv4 literal, and the socket is automatically re-created as
AF_INET6. This is Apple's recommended approach for NAT64 compatibility.

---

# What's New in Build 28

Build 28 — 2026-02-27

## Bug Fixes

### Hardware Keyboard Input on iPad (reported by users)
Typing with a connected hardware keyboard (Bluetooth or Smart Connector)
did nothing when the on-screen keyboard was hidden. The soft keyboard
worked fine.

Root cause: on iOS, regular key events were routed exclusively through
UIKeyInput (which only fires when the view is first responder / soft
keyboard showing). With the soft keyboard hidden, no view was first
responder, so hardware keyboard events went nowhere.

Fix: the view controller now becomes first responder on iOS when the soft
keyboard is hidden, capturing hardware keyboard events via pressesBegan.
When the soft keyboard is shown, the Metal view takes over via UIKeyInput.
When dismissed, focus returns to the view controller automatically.

### Network Connections Timeout on iPad (~40 seconds)
The Help Browser and other network operations locked up for ~40 seconds
then failed with "connection aborted" on real iPads.

Root cause: DNS resolution forced IPv4 only (AF_INET). On IPv6-preferred
networks (common on iPad, especially cellular), the IPv4 DNS query times
out because there may be no IPv4 DNS server available.

Fix: DNS now uses AF_UNSPEC, allowing the OS to use whatever address
family is available (with automatic DNS64/NAT64 synthesis on iOS). IPv4
results are preferred when available. Socket connect handles both 4-byte
IPv4 and 16-byte IPv6 addresses, re-creating the socket as AF_INET6 when
needed.

---

# What's New in Build 27

Build 27 — 2026-02-27

## Bug Fixes

### Keyboard Text Input Fixed (reported by users)
Typing in Pharo editors (Playground, System Browser, Transcript, etc.) did
nothing — keystrokes were intercepted as shortcuts but never inserted as text.

Root cause: the SDL2 event stub only generated SDL_KEYDOWN/KEYUP events.
Pharo's OSSDL2Driver requires SDL_TEXTINPUT events for text insertion. Real
SDL2 generates both: KEYDOWN, TEXTINPUT, KEYUP for each typed character.

Fix: keystroke events with printable characters and no command modifiers now
generate SDL_TEXTINPUT with proper UTF-8 encoding. Modifier chords (Cmd+C,
Ctrl+X, etc.) continue to generate only KEYDOWN/KEYUP so shortcuts still work.

### Pharo Menu Bar No Longer Overlaps macOS Traffic Lights
On Mac Catalyst, the Pharo in-image menu bar (Pharo, Browse, Debug, ...)
overlapped the red/yellow/green window controls. The Metal view now insets
from the top on Mac to leave room for the title bar area.

---

# What's New in Build 25

Build 25 — 2026-02-27

## New Features

### Real Audio Output (SoundPlugin)
Pharo can now play sound. Previously, sound primitives silently discarded
all audio data. Build 25 adds a proper SoundPlugin using Apple's Audio Queue
Services with a lock-free ring buffer (VM thread writes, audio callback reads).
Primitives 300-310 and 327 produce real audio output. Recording primitives
(311-316) remain stubbed pending microphone permission work.

### MIDI Support (MIDIPlugin)
Full CoreMIDI integration for both iOS and macOS. Enumerates all connected
MIDI destinations and sources, opens input/output ports, sends Note On/Off,
CC, Program Change, Pitch Bend, and SysEx messages. Input uses a per-port
ring buffer filled by the CoreMIDI callback. Primitives 330-349 all wired
to real hardware.

### UDP Sockets
The SocketPlugin now supports UDP in addition to TCP. Two new primitives:
primitiveSocketSendUDPDataBufCount (sendto with destination address/port)
and primitiveSocketReceiveUDPDataBufCount (recvfrom, fills caller arrays
with source address and port).

### Real System Locale
Locale primitives (390-399) previously returned hardcoded US English values.
They now query the actual device locale via CFLocale: language, country,
currency symbol, decimal/thousands separators, date/time formats. Timezone
name and DST status use localtime_r. All 10 primitives registered as named
primitives under LocalePlugin.

### SecurityPlugin
Four named primitives registered under SecurityPlugin that the Pharo image
calls during startup via SecurityManager:
  - primitiveCanWriteImage — returns true (image saving always allowed)
  - primitiveDisableImageWrite — no-op
  - primitiveGetSecureUserDirectory — returns the image's directory
  - primitiveGetUntrustedUserDirectory — returns $TMPDIR

### System Clipboard
Clipboard primitives (141, 361-363) now use the real system clipboard via
PlatformBridge instead of an internal string field. Copy/paste between Pharo
and other apps works on both iOS and Mac.

## Bug Fixes

### Async DNS Resolution (fixes slow startup and connection failures)
DNS lookups (primitiveResolverStartNameLookup) were blocking the entire VM
thread via a synchronous getaddrinfo() call. This caused two problems:
  - 30-second startup delay: the UI couldn't render while DNS blocked
  - ConnectionClosed errors: already-connected sockets sat idle while DNS
    blocked other Pharo processes, causing servers to timeout

DNS now runs on a background thread and signals the resolver semaphore when
done. primitiveResolverStatus returns the real state (Busy/Ready/Error).

### SocketPlugin Connect with ByteArray Addresses
Fixed primitiveSocketConnectToPort to accept 4-byte ByteArray addresses in
network byte order, in addition to SmallInteger addresses. Pharo 13's
NetNameResolver returns ByteArrays from DNS lookups.

---

# What's New in Build 24

Build 24 — 2026-02-27

## Bug Fixes

### iPhone Forced to Landscape Mode
Portrait mode on iPhone (~390px wide) made Pharo essentially unusable:

- The image library buttons wrapped into unreadable vertical text
- Pharo's desktop windows and dialogs are designed for 1024px+ and
  didn't fit at all
- The Save As dialog was completely broken — no visible text input field,
  keyboard covered half the window
- Keyboard input reportedly didn't register

The app now locks iPhone to landscape orientation, giving ~844px of width on
modern iPhones. This is enough for Pharo's UI to work without any image-side
patching. iPad keeps all orientations (portrait and landscape). Mac is
unaffected.

### iPhone Safe Area Insets
The Pharo display now respects iOS safe area insets on iPhone and iPad.
Previously, content rendered edge-to-edge, causing the menu bar text and
taskbar to be clipped by the device's rounded corners and hidden behind
the Dynamic Island camera cutout. The Metal view is now constrained to
the safe area, with a black background filling the margins to blend with
the device bezel. Mac Catalyst is unaffected (no rounded corners or
notch on Mac windows).

## Changes

### Softened Pharo.org Disclaimer
Changed the image library disclaimer from "not affiliated with or
endorsed by Pharo.org" to "not endorsed by Pharo.org". The project is
engaged with the Pharo community — the disclaimer just clarifies that
bug reports should come to us, not Pharo.org.

---

# What's New in Build 22

Build 22 — 2026-02-26

## Bug Fixes

### Two-Finger Scroll Now Works on iPad
Previously, scrolling with two fingers on the iPad screen did nothing — you had
to use the tiny Pharo scrollbar widgets. Two-finger scroll now sends proper
scroll wheel events to Pharo, so lists, code browsers, and text editors all
scroll naturally.

### Stage Manager Window Resize Fixed
Resizing the app window via iPad Stage Manager (or dragging the window edge on
Mac) could produce a garbled, scrambled display. Fixed a race condition where
the display buffer could be reallocated while the VM was still drawing into it.

### App Properly Exits When You Quit Pharo
On iPhone, quitting Pharo (via the World menu) would drop you back to the image
library in a broken state instead of closing the app. The app now exits cleanly
when Pharo quits, matching the Cmd+Q behavior on Mac.

### Gesture Quick Start (iOS)
First time you launch a Pharo image, a help overlay shows the key gestures:
  - Tap = left-click, Long press = right-click (context menu)
  - Two-finger scroll, Two-finger tap = right-click
  - Keyboard and Ctrl toolbar buttons
  - Ctrl+D (Do It), Ctrl+P (Print It), Ctrl+E (Inspect It)

Tap "Got it" to dismiss. Tap the "?" button in the floating toolbar to see
it again anytime.

---

# What's New in Build 21

Build 21 — 2026-02-26

## Bug Fixes

### iOS Soft Keyboard Input (reported by Tim, Pharo users group)
- Fixed: regular character input did nothing on iPad soft keyboard. Root cause
  was iOS autocorrect/prediction buffering characters instead of delivering them
  immediately. Disabled autocorrection, autocapitalization, spell checking, smart
  quotes, and smart dashes on the Pharo canvas view.
- Fixed: Backspace and Enter were doubled (entered twice per keystroke). On iOS,
  both pressesBegan and UIKeyInput fired for the same key. pressesBegan now skips
  keys that UIKeyInput handles (regular chars, enter, backspace without Cmd/Ctrl).

### Long-Press Right-Click Reliability (reported by Tim, Pharo users group)
- Fixed: long-press to right-click failed most of the time. touchesBegan sent a
  RED (left) button-down immediately on finger contact; 0.5s later the long-press
  handler sent YELLOW (right) button-down on top of it. Pharo had conflicting
  button states and ignored the right-click. Fix: explicitly send RED button-up
  before YELLOW button-down, and set cancelsTouchesInView so UIKit cleanly cancels
  the original touch.

### Timer Check Starvation
- Fixed: periodic check (every 1024 bytecodes) could be permanently starved by
  tight loops with even bytecode counts. Added deferred check flag that fires on
  the next non-extension step if skipped. Fixes BlockClosureValueWithinDurationTest
  hang.

## New Features

### Image Library Redesign
- Redesigned image library to match the Pharo Launcher table layout
- Full-width table with sortable column headers: Name, Version, Size, Last Modified
- Click column headers to sort ascending/descending with sort indicator arrows
- Search/filter bar at top to filter images by name
- Detail panel at bottom shows image info when selected (file name, location,
  version, total size, created/launched dates)
- Context menu on image rows: Launch, Rename, Duplicate, Share, Show in Files, Delete
- Rename alert with text field
- Duplicate creates a full copy of the image directory
- Share sheet for AirDrop / Save to Files
- Show in Files button opens the image directory in Finder (Mac) or Files app (iPad)

### Virtual Ctrl Key (suggested by Tim, Pharo users group)
- New floating "Ctrl" button on the iOS canvas toolbar
- Toggle on/off — stays active until tapped again
- When active: canvas taps include Ctrl modifier (Ctrl+click = right-click)
- When active: soft keyboard input includes Ctrl modifier (Ctrl+E = Do It,
  Ctrl+D = Debug It, Ctrl+P = Print It, etc.)

### Keyboard Toggle Button
- New floating keyboard button on the iOS canvas toolbar
- Tap to show/hide the soft keyboard on demand
- State syncs with VM text input callbacks

### Project Info Bar
- Persistent info bar at the top of the image library window
- Shows: project name, experimental release disclaimer, Pharo.org link,
  GitHub link, Report a Bug link
- Always visible regardless of selection state
- Removed redundant disclaimer from settings screen
