# JIT bugs uncovered while debugging the jit-default SUnit SIGBUS

Catalogued during the 2026-04-20 segv-debug session.  Lists every
distinct bug class the bisect surfaced, regardless of whether it
was already fixed.  Each entry: location, signature, evidence,
status, fix.

---

## 1. Eviction freed methods that were live on the stack — **FIXED** (32e8cda)

**Location**: `src/vm/jit/JITCompiler.cpp` — `compile()` eviction block.

**Signature**: SIGBUS during JIT execution; `[CRASH] PC not in any
active JIT method (evicted?)` from the crash handler.

**Evidence**: jit-default SUnit crashed at compile #5; `evictLRU`
freed methods whose code was still on the C stack via a trampoline
return address, or whose JITMethod* was held in a live `J2JSave`
chain-loop frame.

**Fix**: Added `JITMethod::pinned`.  Before `evictLRU` and the
full-flush fallback, walk (a) the J2JSave pool entries
`0..j2jPoolCursor_`, (b) the native frame-pointer chain, marking
every JIT method whose code is currently in use.  `evictLRU` and
the full-flush loop both skip pinned methods.

---

## 2. `patchJITICAfterSend` left MAP_JIT writable — **FIXED** (147c3a9)

**Location**: `src/vm/Interpreter.cpp` — `patchJITICAfterSend`.

**Signature**: SIGBUS at `PC == fault_addr` somewhere in the code
zone, on the next JIT entry after an IC patch.

**Evidence**: Apple Silicon `pthread_jit_write_protect_np()` is a
per-thread toggle for the entire MAP_JIT region.  The IC patch
called `makeWritable(icData, 1)` and never flipped back; the next
JIT activation faulted because the thread's view of the code zone
was non-executable.

**Fix**: RAII guard at the top of the function calls
`makeExecutable(zone.rawStart(), zone.totalBytes())` on every exit
path (success, mismatched selector, full-IC bail, exception).

---

## 3. `forEachRoot` left MAP_JIT writable after GC — **FIXED** (147c3a9)

**Location**: `src/vm/Interpreter.hpp` — `forEachRoot`.

**Signature**: same SIGBUS as bug 2, but only after a GC cycle.

**Evidence**: GC's root visitor wrote into IC oop slots in JIT
memory (it had to; oops there move on compaction), called
`makeWritable` to do the writes, but never restored.  Next JIT
entry on this thread crashed.

**Fix**: matching `makeExecutable` at end of `forEachRoot`.

---

## 4. `flushCaches` and `recoverAfterGC` same imbalance — **FIXED** (147c3a9)

**Location**: `src/vm/jit/JITRuntime.cpp` — `flushCaches`,
`recoverAfterGC`.

**Signature**: same SIGBUS, after method-dictionary changes or GC.

**Fix**: matching `makeExecutable` at end of each function.

---

## 5. Seven `save.jitMethod` deref sites had no null check — **FIXED** (dd91d26)

**Location**: `src/vm/Interpreter.cpp` — `tryJITActivation` and
`materializeJ2J` lambda.  Lines 12557, 13832, 13953, 14104, 14528,
14750, 14858 (post-fix line numbers).

**Signature**: SIGSEGV `Fault addr=0x0` deep inside
`tryJITActivation`; faulting instruction was `ldr [x?, #imm]` with
the base register zero.

**Evidence**: each loop did
```
J2JSave& save = j2jStack[i];
JITMethod* saveJM = save.jitMethod;
Oop saveMethod = Oop::fromRawBits(saveJM->compiledMethodOop);
```
without checking `saveJM`.  When a J2J save entry was stale or
zeroed (e.g., from a recursive entry that didn't fully populate
its slice), `saveJM == nullptr` and the next read crashed.

**Fix**: every site now checks `if (!saveJM) { warn; break; }`.
The loop bails cleanly; in the materialize-lambda case we also
unset `j2jMaterialized` so the chain loop doesn't try to use the
half-materialized state.

---

## 6. `JIT_CALL` macro relied on caller for W^X — **FIXED** (34f39cd)

**Location**: `src/vm/jit/JITState.hpp` — `JIT_CALL` macro.

**Signature**: same SIGBUS as bug 2, on JIT entries from chain-loop
sites in `Interpreter.cpp` and from `JITRuntime.cpp:823`.

**Evidence**: nine `JIT_CALL` sites in `Interpreter.cpp` and a few
in `JITRuntime.cpp` did not all precede the call with
`makeExecutable`.  Adding the call at every site is repetitive and
easy to miss when adding new sites.

**Fix**: macro now expands to call `pthread_jit_write_protect_np(1)`
before the `blr`.  Cheap (one MSR write), idempotent, no-op on
non-Apple-Silicon.  Removes the entire class of "forgot to flip
back to executable" bugs at JIT entry from C.

---

## 7. Sista's `fn(&sstate)` and `sista->compile()` calls didn't bracket W^X — **FIXED** (5247ba4, 7fea68e)

**Location**: `src/vm/Interpreter.cpp` — Sista hook in
`activateMethod`.

**Signature**: SIGBUS during T1 JIT execution shortly after a Sista
compile or dispatch.

**Evidence**: asmjit's `JitRuntime` shares the per-thread MAP_JIT
W^X toggle with our `CodeZone`.  asmjit uses RAII scopes that
*should* leave kReadExecute on success, but a defensive bracket
removes any leak.  Confirmed by `PHARO_SISTA_COMPILE` (compile
only, no dispatch) also producing SIGILL — Sista's compile alone
mutated state.

**Fix**: call `makeExecutable(zone-wide)` immediately after
`sista->compile()` and on both sides of `fn(&sstate)`.

---

## 8. SIGILL had no crash dump — **FIXED** (eda373b)

**Location**: `src/vm/test_load_image.cpp` — signal handler setup.

**Evidence**: an entire crash class (`EXIT=132` with no log) was
invisible because we only registered SIGSEGV/SIGBUS.

**Fix**: also `sigaction(SIGILL, ...)`.  Pure diagnostic — no code
behavior change, but unblocks debugging the Sista codegen overrun.

---

## 9. `JITMethod::codeSize` field name lies — **NOT FIXED**

**Location**: `src/vm/jit/JITMethod.hpp:125`,
`src/vm/jit/JITCompiler.cpp:1960`.

**Severity**: documentation / readability; no current crash.

**Evidence**: header comment says "Size of machine code in bytes",
but the compiler stores
`jitMethod->codeSize = totalSize`
where `totalSize = code + literals + bcToCodeTable + ICs`.
Multiple readers (e.g., `findMethodByPC`, `JITRuntime.cpp:1648
icStart = codeStart + codeSize - icSize`) implicitly depend on
this.  A new reader who took the comment at face value would do
the wrong thing.

**Fix**: TBD — either rename the field to `allocSize` and add a
real `codeSize` (the actual machine code byte count), or update
the comment to match reality.  Renaming is the right move; safer
to do alongside an audit of every reader.

---

## 10. `kStoreInstVar` lowering bypasses immutability + write barrier — **PARTIALLY GATED** (still wrong in lowerer)

**Location**: `src/vm/jit/sista/SistaLowering.cpp:187`.

**Severity**: silent miscompile when lowered code runs on an
immutable receiver, AND missed write-barrier could let scavenge
miss old→young pointer references.

**Evidence**: lowering emits a direct
```
str val, [recv + 8 + N*8]
```
with no `attemptToAssign:withIndex:` callout (interpreter does
this) and no `rememberObject` write-barrier (interpreter does
this when storing a young oop into an old object).

**Workaround in place**: the activate-time gate (line ~6841) marks
any method with `PopStoreRecv*` / `ExtPopStoreRecv` /
`ExtStoreRecv` bytecodes as `hasUnsafeOp = true`, which blocks
*dispatch*.  Methods still **compile**; their machine code holds
the broken `str` instruction.

**Bisect evidence**: `PHARO_SISTA_NO_STORES` (extra gate that
also rejects PopStoreRecv/PopStoreTemp from dispatch) pushes the
JIT-default crash floor from compile #15 to #112 — strong
evidence the dispatched store code is one corruption source.

**Fix**: write-barrier + immutability check in
`SistaLowering.cpp:187`'s `kStoreInstVar` case, mirroring
`Interpreter::setReceiverInstVar` semantics.

---

## 11. Sista compile-only (no dispatch) crashes — **PARTIAL FIX (de01be1), residual SIGILL**

**Severity**: critical to fix before Sista can ship.

**Bisect** (env-var gates added in commits `d36b00f`, `059e91f`):

    PHARO_NO_SISTA=1            no crash, 49 compiles, image runs
    PHARO_SISTA_NO_LOWER=1      no crash, 49 compiles (lower returns null)
    PHARO_SISTA_NO_LOWER_BODY=1 CRASH at #15 (empty body, just `ret`)
    PHARO_SISTA_NO_LOWER_SENDS=1 crash at #99
    PHARO_SISTA_NO_LOWER_ARITH=1 crash at #16
    default                     crash at #15

The NO_LOWER_BODY result is the smoking gun.  With NO_LOWER_BODY,
`lower()` builds a CodeHolder, attaches a Compiler, emits a
function that just sets exitReason and returns, and calls
`runtime_->add()`.  No IR-driven body emission, no Sista-specific
work — pure asmjit infrastructure.  And it still crashes at #15.

**Conclusion**: the corruption is in asmjit's
CodeHolder/Compiler/JitRuntime infrastructure (or its MAP_JIT
W^X interaction with our `CodeZone`), not in any specific IR-op
emission.

**Hypotheses**:
- asmjit's `JitAllocator` shares a page boundary with our zone
  and one's writable scope flips the other's executability
- asmjit's W^X RAII scope leaks through an exception path
- asmjit's `CodeHolder` corrupts register state in a way that
  bleeds into the next T1 JIT activation via the C stack

**Partial fix shipped (de01be1)**: replaced `siglongjmp(vmcc->trampoline)`
with `throw pharo::CallbackComplete{}` + `try/catch` in
`callbackClosureHandler`.  `setjmp/longjmp` skips C++ destructors —
that includes asmjit's `ProtectJitReadWriteScope` whose destructor
flips MAP_JIT W^X back to executable.  C++ exception unwinding runs
every destructor between throw and catch, including asmjit's.  libffi
on macOS arm64 has `__compact_unwind` so the exception traverses C
frames safely.

Net effect: crash signature flipped from SIGSEGV null-deref at #15
to SIGILL at #45.  One layer fixed; the SIGILL is its own bug —
**a JIT method runs past its own `codeSize` boundary into the IC
area where bytes are zero (ARM64 UDF)**.

The other siglongjmp pair — `siglongjmp(reenterInterpreter_)` from
`enterInterpreterFromCallback` to `interpret()` — is still active.
Defensive `makeExecutable` at the sigsetjmp landing site (in
`interpret()`) is in place as a band-aid.  Converting that pair to
exceptions is the next clean-up.

**Next steps for the residual SIGILL**:
- The residual crash PC (e.g., `0x109052850`) sits inside OUR T1
  CodeZone (`0x1089d0000` + `0x682850`), NOT in asmjit's
  allocation (asmjit at `0x100e5c...`).  Instructions at PC are
  literal-pool data (oop bits, small ints, zero) rather than code,
  i.e., a T1 JIT branch or call landed in the IC / literal area.
- So the residual is a T1 JIT codegen issue, not Sista/asmjit.
  Replacing `asmjit::JitRuntime` wouldn't help here.
- Most likely culprits: a stale IC entry holding a code pointer
  that's been overwritten, or a J2J resume address that wasn't
  re-keyed after compaction, or a bcToCodeOffset entry pointing
  into the post-code data area.

---

## 12. `Tier2Compiler` unused-private-field warnings — **NOT FIXED**

**Location**: `src/vm/jit/Tier2Compiler.hpp:103,106`.

**Severity**: cosmetic — compiler warning, not a runtime bug.

**Evidence**: `zone_`, `interp_` declared but never read.

**Fix**: remove or wire up.

---

## 13. `CodeZone.cpp` produces an empty `.o` — **NOT FIXED**

**Severity**: cosmetic.

**Evidence**: `ranlib: warning: 'libPharoVMCore.a(CodeZone.cpp.o)'
has no symbols`.

**Fix**: either delete `CodeZone.cpp` (header-only inline impl is
fine) or move at least one out-of-line method into it.

---

## Status summary

    Fixed     1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 13   (12 bugs)
    Fixed     11a (siglongjmp→exception, de01be1)
              11b layers 1-5 (see history below)
    OPEN      14. jit-default deadlocks image idle loop

## 14. jit-default makes image hang in idle — **NEW, OPEN**

After bug 11 was fixed, jit-default SUnit still doesn't complete
the full run.  The pattern: the VM compiles ~540 methods, then
the Smalltalk process scheduler parks in `ProcessorScheduler class
>> idleProcess` — every user process is waiting (e.g., on a
semaphore or timer) and nothing signals them.  SUnit never runs.

Baseline comparison on the SAME image:

    stock Cog       completes SUnit in 2m
    jit NO_JIT      completes SUnit in ~45m
    jit default     idle-hangs after ~540 compiles; SUnit never runs

Our jit-default is therefore changing the scheduler's observable
behavior vs the interpreter path.  Most likely a Sista-compiled
method executes with wrong semantics for one of:

- semaphore `signal` / `wait` (misses a signal, or signals wrong
  semaphore)
- timer-semaphore arming or firing
- `forceInterruptCheck` / process switch

Suspect first: Sista hook pre-compiles methods at activation.
It currently runs pre-compile even for methods it then refuses
to dispatch (via the activate-time gate).  If Sista's compile
path touches state unsafely (e.g., fills asmjit metadata that
later mis-routes a semaphore signal), the image stalls.

**Update 2026-04-21**: bisected further.

- `PHARO_NO_SISTA=1` (T1 JIT only, Sista off): **still hangs**
  identically after 1115 compiles.  So bug is in T1 JIT, not Sista.
- `PHARO_JIT_SKIP_SELECTORS=idleProcess`: still hangs.  So the
  idleProcess compile wasn't the trigger either.

Queue diagnosis at hang time shows two processes:

    P10  idleProcess                                  (running idle)
    P80  DelayMicrosecondTicker>>waitForUserSignalled:orExpired:  (blocked)

`DIAG-TIMER` logs show the microsecond timer was armed, fired,
and then `timerSem=nil / nextUsec=INT64_MAX` — meaning nothing
re-armed the timer by calling primitive 242.  Only 3 semaphore
signals total across the 45-min run, vs thousands in a healthy
session.

So: JIT's execution of `DelayMicrosecondTicker>>waitForUserSignalled:`
or adjacent methods is dropping the re-arm path.  Either a
primitive-242 call gets miscompiled (wrong operands pushed to
stack), or a conditional branch miscompiles and skips the re-arm
branch, or something in the ticker's per-iteration state isn't
being stored correctly.

Next step: PHARO_JIT_SKIP_SELECTORS with a specific selector
(waitForUserSignalled:orExpired:, primSignal:atUTCMicroseconds:,
etc.) to identify which JIT-compiled method is the culprit.

**Update (continued)**: ran PHARO_JIT_SKIP_SELECTORS with all 4 of
{waitForUserSignalled:orExpired:, primSignal:atUTCMicroseconds:,
nowTick, initSignals} — **still hangs identically**.  So this is
NOT one specific method being miscompiled; it's a broader
behavioral difference between our JIT path and Cog's for one of
the implicit scheduler interactions (semaphore wait/signal,
process switch on yield, interrupt-check frequency, or an asm
trampoline state transition).

Resolving needs: (a) a semaphore/process trace logging every
`primitiveSignal`, `primitiveWait`, `primitiveSuspend`,
`transferTo:` call, on both our VM and stock Cog, and diffing the
call sequences; (b) or a minimal reproducer that doesn't depend
on SUnit boot.

**Update (continued, diagnostic `PHARO_XFER_TRACE=1` added in 5381f28)**:
under jit-default the scheduler settles into an infinite 5-XFER
cycle between exactly 3 processes:

    p40  (unknown)                            ← SUnit test candidate?
    p80  DelayMicrosecondTicker               ← delay ticker
    p60  (unknown, likely UI/worker)

The cycle is deterministic: p40 → p80 → p60 → p80 → p60 → p40 → …
repeated forever.  None of them yields to the SUnit runner or any
other process.  They mutually re-signal but produce no net progress.

Under `NO_JIT` the XFERs go to diverse processes and SUnit starts
normally.  So JIT-mode changes some signal-delivery semantic —
probably either:

- `signalSemaphoreDirectly` / `synchronousSignal` waking the wrong
  waiter (e.g., re-waking the last signaler instead of a different
  queued waiter)
- `transferTo` putting the caller back on a "ready" queue it
  shouldn't be on
- `wakeHighestPriority` picking a non-test priority waiter

Pinpointing further requires a side-by-side trace against stock
Cog for the first ~700 XFERs.

Current production recommendation: `PHARO_NO_JIT=1`.  Still gives
99.7% pass rate vs Cog (27 regressions on 17k tests).

Bug 11 closed completely.  The root cause turned out to be
**layer 5: JM_SIZE constant was 80 in two files (stencils.cpp
and TrampolineAsm.S) while real sizeof(JITMethod) had grown to
88**.  Every stencil's `add Xn, methodHdr, #JM_SIZE` produced
`methodHdr + 80` (the lastUsedEpoch field) instead of
`methodHdr + 88` (codeStart).  Net: every BLR target into
JIT code landed 8 bytes BEFORE the actual code, in the JIT
header — ARM64 UDF on the zero/data word there.

Layers 1-4 were real defensive bugs (null guards, branch-clamp
sentinel, eviction-pinning, etc.) that all needed to be fixed
on the way to layer 5 — the off-by-8 was masked by earlier
crashes in the chain.

Crash floor progression: compile #5 → #15 → #28 → #42 → #45 → #56 → #68.
Each layer is a real bug; each fix is methodically peeling onion layers.
Layer 4b is the first one that defensive guards can't catch — it's
JIT-emitted code branching to a wrong address inside JIT execution.
Fixing it requires either (a) hardware single-step + breakpoint on
the bad branch, or (b) extensive stencil-relocation auditing.

Bug 11 (Sista compile-path corruption) is the remaining
show-stopper for jit-default.  Bisect gates `PHARO_SISTA_NO_LOWER`
and `PHARO_SISTA_NO_LOWER_SENDS` confirm the corruption is in
`Lowering::lower`'s asmjit emit path itself; `kSendUnspeculated`
alone isn't the trigger (NO_LOWER_SENDS still crashes at #99).
Next bisect: gate kPrim*, kBranch*, kPhi individually.
