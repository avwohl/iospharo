# asmjit-T1 inline IC probe — investigation 2026-05-15

## Motivation

asmjit-T1 is correct on the 39/39 differential fuzzer corpus but loses
to our own interpreter on tinyBenchmarks:

    Stock Pharo (Cog/Sista)    7,592 M bytecodes/sec    459 M sends/sec
    Our VM, interp only          200 M                   11.0 M
    Our VM, asmjit-T1 JIT        114 M                    4.9 M

The send rate is the obvious gap. `JIT Stats` shows
`IC: 0/22M (0% hit)`: every send goes through `Interpreter::sendBytecode`
(global cache lookup, method dispatch, frame setup). The asmjit-T1
emit sets up `state.icDataPtr` + `state.sendArgCount` then exits with
`ExitSend`. The chain loop's `ExitSend` handler then calls
`patchJITICAfterSend` to populate the IC for the next call — but
since the JIT-side probe never reads it, the populated entries are
dead.

## What the probe needs to do

Match the existing stencil JIT's contract for `ExitSendCached`:

  * Compute lookup key (classIndex for objects, `tag | 0x80000000`
    for immediates).
  * Probe icData[0..2] for a match (slot 0 = key, slot 1 = method,
    slot 2 = extras flags).
  * On hit: set `state.cachedTarget = icData[1]`, exit with
    `ExitSendCached`. Chain loop's `ExitSendCached` handler will
    inline-activate the cached method.
  * On miss: exit with `ExitSend` as before.

The emit is straightforward (~25 instructions); see
`src/vm/jit/asmjit/AsmjitT1.cpp:842+` for the implementation behind
`g_debug.t1ICProbe`.

## Why the current probe doesn't ship

Two correctness bugs, both observable on startup of any expression:

### 1. Without an extras-zero guard (raw icData[0] match)

37% IC hit rate, but `Context>>copyTo:` recurses ~4000 frames deep
and the VM eventually terminates with a 4000-frame chain in the
TERM dump. The root cause is the inline-getter/setter/returnsSelf/J2J
specialization bits in `icData[e*3 + 2]`:

  * `patchJITICAfterSend` (Interpreter.cpp:16708-16870) writes extras
    encoding bit 63 (inline getter), 62 (setter), 61 (returnsSelf),
    60 (J2J entry address), 59 (BLOCK_VALUE_BIT), 58 (returnsLiteral),
    57 (multi-slot getter), or 48..52 (inline primKind).
  * The stencil JIT's `stencil_sendJ2J` (stencils.cpp:1432+) checks
    these bits BEFORE dispatching the cached method and short-circuits
    the send entirely (e.g., inline getter reads `recvObj->slotAt(n)`
    and skips the method dispatch).
  * Our probe dispatches the raw method via `ExitSendCached`. For
    most methods this produces the same result, just slower — but
    for some methods (notably `cull:`, `value:`, and getter chains),
    the difference triggers infinite recursion. Hypothesis: the
    inline-getter specialization avoids modifying `state.receiver`
    in a way that the actual getter method's prologue depends on.

### 2. With an icData[2]==0 guard (only hit on non-specialized entries)

6.8% IC hit rate, no recursion, no MUSTBOOL, no crash. But startup
fails with:

    [DNU] #workingDirectoryPath not understood by rcvr=MacStore in #resolvePath:

The cached method dispatched is for `MacStore` while the expected
receiver-class context should resolve to `UnixStore` on Linux. The
shift-on-platform happens in image-side code that runs in startup —
some IC entry got populated with `MacStore.default`'s method
during pre-relocation, then a later receiver-class change wasn't
reflected because we kept dispatching the cached. (Stencil JIT
doesn't hit this because the inline-getter path checks
`recvObj->classIndex()` matches each time, not just icData[0].)

## Next step for whoever picks this up

Two paths:

**A. Re-validate on each hit.**  Have the probe re-check the cached
method's class matches the receiver's class, the way the stencil's
inline-getter does. This costs an extra load + cmp per hit but
catches stale cached dispatches. Not free but probably worth it.

**B. Implement the specialization short-circuits inline.**  Mirror
the stencil's inline-getter / setter / returnsSelf / J2J paths in
the asmjit emit. ~150 lines of x86 asm per emit (×2 for ARM64). This
is the real fix and matches what Cog/Sista does on stock Pharo.

The infrastructure is in place:

  * `g_debug.t1ICProbe` (PHARO_T1_IC_PROBE=1 to enable for
    experimentation).
  * `g_debug.t1ICProbeMin` / `t1ICProbeMax` to bisect by opcode
    range — found that special selectors (0x70..0x7F) trigger a
    different failure mode (MUSTBOOL in sortStructs:into:) than
    literal sends (0x80..0xAF — infinite recursion). Useful for
    narrowing.

Estimated effort for path B: 1 session for x86 inline-getter alone,
~5 sessions for the full specialization set. Path A is half a
session; would close most of the perf gap without the bug risk.

## What this changes today

Nothing. The probe is default-OFF. To use it for further
investigation:

    PHARO_T1_IC_PROBE=1 ./build/test_load_image /tmp/Pharo.image eval "..."
    PHARO_T1_IC_PROBE=1 PHARO_T1_IC_PROBE_MIN=128 \
        PHARO_T1_IC_PROBE_MAX=143 ./build/test_load_image ...

Send-rate parity with stock Pharo (~100× our current rate) will
require either the inline specializations (path B) or a per-hit
class re-validation (path A). Both are substantial work; this
note is a hand-off, not a fix.

## Path B attempt 2026-05-15 — narrows the bug to ExitSendCached

Took another swing at path B. Added inline-getter (bit 63),
inline-setter (bit 62), and returnsSelf (bit 61) emit paths in
both x86 and ARM64. Each individually gated:

    PHARO_T1_NO_INLINE_GETTER=1     # skip bit 63 path → dispatch_cached
    PHARO_T1_NO_INLINE_SETTER=1     # skip bit 62 path
    PHARO_T1_NO_INLINE_RETURNS_SELF=1

Diagnostic knobs:

    PHARO_T1_PROBE_ALWAYS_MISS=1    # probe runs but never takes HIT exit
    PHARO_T1_HIT_AS_MISS=1          # HIT path exits via ExitSend instead of
                                    # ExitSendCached
    PHARO_T1_IC_HIT_VERIFY=1        # check cached method's selector and
                                    # receiver class match the IC at each hit

The bisect revealed: **the bug is not in the inline specializations
or the probe arithmetic**. It's specifically in the
`ExitSendCached` dispatch path from our T1 emit.

    Configuration                               Result
    ----------------------------------------    -----------------------
    probe OFF (default)                         eval "3+4" → '7' ✓
    probe ON, always-miss (HIT skips exit)     '7' ✓
    probe ON, hit-as-miss (HIT→ExitSend)       '7' ✓
    probe ON, all specs OFF (HIT→ExitSendCached)  MUSTBOOL ✗
    probe ON, only getter (bit 63)             MUSTBOOL ✗
    probe ON, only setter                      MUSTBOOL ✗
    probe ON, only returnsSelf                 MUSTBOOL ✗

So even with all the inline opts gated off — just the "raw cached
dispatch" via `ExitSendCached` — the result is wrong. With
`PHARO_T1_IC_HIT_VERIFY=1` the runtime verify check found **zero**
selector or receiver-class mismatches: the dispatched method oop
is the correct method for the correct receiver class.

The failure mode: `#sortStructs:into:` (interp-executed because it
has a 2-byte `PushArray` we don't compile) sends `#isEmpty` to its
first arg. The cached implementation is reached via our chain
loop's `ExitSendCached` path → fallback → `executePrimitive` (or
inline-activate), runs to completion, returns its value. But the
*value pushed on the caller's stack* comes back as an SmI 160
(some collection's size) instead of a Boolean. Subsequent
`jumpFalse` fires MUSTBOOL.

The same code path is exercised by the legacy stencil JIT (which
also exits via `ExitSendCached` when its inline-opts don't fire),
so the chain-loop ExitSendCached handler is known-correct under
that exerciser. What's different about our T1's exit state isn't
yet known; possibilities:

- A pre-send state field that's set differently (state.receiver,
  state.tempBase, state.literals, j2j-pool fields).
- The bcToCode resume address landing on the wrong bytecode for
  T1-compiled methods after the callee returns.
- An interaction with `upgradeICToJ2J` writing extras that
  conflict with T1-compiled callees.

The infrastructure (probe + spec emits + bisect knobs + hit
verifier + hit ring buffer) is in place. Next session can pick up by:

1. Re-running with `PHARO_T1_IC_PROBE=1 PHARO_T1_TRACE_HIT=1`
   already prints the last 30 IC hits before the first
   sendMustBeBoolean — but the trace shows only #hash → #class
   and #stringHash:initialHash: → #stringHash:initialHash:, none of
   which obviously implicate the failing sortStructs:into:>>isEmpty
   call. So the link from "IC hit somewhere" to "wrong SmI on the
   interp's stack" is non-obvious; need to instrument further down
   the chain.
2. Comparing T1 and stencil JIT's state-at-exit byte-for-byte.
   Specifically check `state.j2jSaveCursor / j2jSaveLimit / j2jDepth`
   — these are set by the inline-activate path but not by our send
   emit. If T1's callee uses them differently, that could cause
   stack confusion.
3. Try gating the probe to fire only for SmI receivers (the
   simplest case). If THAT works, narrow further.
4. Print the post-dispatch TOS at every IC hit. If a hit returns
   an unexpected type (SmI from a method that should return
   Boolean), that's the smoking gun.

## Knobs reference

  PHARO_T1_IC_PROBE=1                    enable probe
  PHARO_T1_NO_IC_PROBE=1                 disable (default if not opted in)
  PHARO_T1_IC_PROBE_MIN=N                only probe opcodes >= N
  PHARO_T1_IC_PROBE_MAX=N                only probe opcodes <= N
  PHARO_T1_NO_INLINE_GETTER=1            skip bit-63 inline emit
  PHARO_T1_NO_INLINE_SETTER=1            skip bit-62 inline emit
  PHARO_T1_NO_INLINE_RETURNS_SELF=1      skip bit-61 inline emit
  PHARO_T1_PROBE_ALWAYS_MISS=1           probe runs but never HIT exit
  PHARO_T1_HIT_AS_MISS=1                 HIT exits via ExitSend
  PHARO_T1_IC_HIT_VERIFY=1               runtime sel/class verify on each HIT
  PHARO_T1_TRACE_HIT=1                   ring buffer of last 30 IC hits,
                                         dumped at first sendMustBeBoolean
