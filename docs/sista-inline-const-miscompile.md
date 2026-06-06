# Blocker #4 root cause: speculative-inline miscompile in the optimized tiers

Status: SISTA VARIANT FIXED (commit 5f1e8504); a separate JIT-tier (T1 IC/J2J)
variant remains. Date: 2026-06-05.

## RESOLUTION of the Sista variant (the deterministic, primary one)

`tryInlineConstReturn` probe-lifted a COMPLEX method (e.g.
SystemEnvironment>>at:put:), which bails at its own first send and so lifts to a
2-value [load, send] PREFIX; the `^ X foo` tail-forwarder shape inlined that prefix
as the inner send's result, SILENTLY DROPPING the rest of the method (at:put:'s
real store).  In SystemEnvironment>>organization: this meant
`^ self at:#SystemOrganization put: anOrganization` returned a value without ever
storing → the fresh env never got #SystemOrganization → KeyNotFound (and
NonBooleanReceiver elsewhere from analogous drops).  Found by dumping the isolated
method's asmjit (it stored `environment:` then returned `[state+128]` with no
at:put: store) + [ICR-ATTEMPT] logging (`callee=#at:put: calleeIRsize=2`).
FIX (SistaBuilder.cpp): a probe-lift ending in kSendUnspeculated is only a valid
tail-forwarder if that send is the method's TAIL (within the last ~5 bytecodes:
send + returnTop); otherwise bail.  + arg-count guard (callee numArgs == nArgs).
Repro `scripts/repro/blocker4-perrun.st` PHARO_NO_JIT=1 → 12/12 PASS x3; correctness
(fib/sum/dict/sort) unaffected.

## T1 variant — CONCLUSIVE localization (2026-06-06, lldb-track deep-dive)

CONFIRMED MECHANISM: a hashed-collection entry is MISPLACED — stored INTACT in the
backing `array` but at a HASH-UNREACHABLE slot.  Caught with an on:KeyNotFound
inspector: at failure the key's association is `linearFound=true` (present in the
array) but `hashFound=false` (the hash probe hits nil first).  Example: #A1DefinedInX
hashes to slot 3 but sits at slot 2, while its probe 3→4→5→6 finds nil at 6 → A1
should be at slot 6 but is stuck at 2.  Because the assoc is INTACT, GC did NOT drop
it — this REFUTES all GC/scavenge/root-scan hypotheses (incl. the audit's #1; the
forEachRoot→currentJITState_->sp change was a verified no-op).

CULPRIT (by elimination, all PASS-counted on the det-sched repro): the inline `at:`
READ continuation.  `PHARO_T1_NO_INLINE_PRIM_AT=1` (disables inline at:+at:put:+size)
→ PASS=6.  `PHARO_T1_NO_INLINE_PRIM_ATPUT=1` (at:put: only) → FAIL.
`PHARO_T1_NO_INLINE_SIZE=1` (size only) → FAIL.  at:put:+size both disabled, at: kept
→ FAIL.  So the at: read is the sole differentiator.  But the at: READ VALUE is
correct (recompute = 0 mismatches), and behaviour-neutral NOPs at the at: entry do
NOT mask it (PHARO_T1_AT_NOPS=20/60 still FAIL) — so it is the at:-read CONTINUATION
*execution* (staying in-stencil rather than exiting EXIT_SEND_CACHED to C++), NOT
codegen layout and NOT the read value.  It is GENERAL across methods: the inline-at
callers are SequenceableCollection indexers (do:, fixCollisionsFrom:, swap:with:,
first, indexOf:startingAt:ifAbsent:, …); skipping any ONE (PHARO_T1_SKIP_SELECTORS,
verified effective) just shifts the victim — only disabling inline-at globally fixes.

REFUTED (empirically, each a no-fix or breaks-the-eval, NOT a layout artifact):
stale OFF_SENDARGCOUNT/OFF_IP/OFF_CACHED_TARGET at the continuation
(PHARO_T1_INLINE_SYNC=1/2/3/7); C++-global desync at the continuation
(PHARO_T1_SYNC_GLOBALS — breaks); GC root underscan (forEachRoot live-sp — no-op);
inline-setter OOB (PHARO_T1_SETTER_BOUNDS — 0 events); dispatch mis-route
(NO_INLINE_GETTER/SETTER/RETURNS_SELF — no fix); classIndex reuse (CTCHECK 0);
J2J / IC-fill / method-cache / megacache.

PINPOINTED THE SITE (DICT-STORE trace, PHARO_T1_TRACE_DICT_STORE): the misplaced
entry is placed by `HashedCollection>>grow` → `Dictionary>>noCheckAdd:`
(`array at: (findElementOrNil: key) put: assoc`) during the dict's 5→11 rehash.
A1DefinedInX lands at slot 2 on FAILING runs vs slot 5 on PASSING runs — i.e. the
rehash places it NON-DETERMINISTICALLY at a hash-unreachable slot.  Symbol hash is
verified CONSISTENT (200k calls, 0 variance) and the fmt-2 at: value is correct, so
it is neither a hash miscompile nor a wrong read value — it is the inline-prim
CONTINUATION inside the rehash loop.

REFINED CULPRIT: it needs ALL of inline at:+at:put:+size disabled together to fix:
  NO_INLINE_PRIM_AT (all three, shared t1InlinePrimAt gate) → PASS=6.
  at:put:+size off / at:-read off / size off / at:put: off (any SUBSET) → still FAIL.
So the rehash loop chains inline size→at:→…→at:put: with NO C++ exit between them,
and the continuation corrupts unless EVERY prim in the loop is routed through the
C++ chain (each exit being a re-sync point).  This is the "dispatch-protocol resume"
family: the in-stencil inline-prim continuation, run back-to-back in a loop without a
C++ exit, diverges from the round-trip path.  Layout shifts (PHARO_T1_AT_NOPS) do NOT
mask it (they shift the victim, never clean-PASS) — so NO_INLINE_PRIM_AT's clean
PASS=6 is a real fix, not a layout artifact.

ROOT CAUSE (2026-06-06, traced to the instruction-adjacent level):
`Dictionary>>scanFor:` IS JIT-real-compiled (T1-COMPILE: oop 0x3003037c0,
bcLen 90, canBail=0 — NOT the stub I earlier assumed).  During the dict's 5→11
grow, `noCheckAdd:` → `findElementOrNil:` → `scanFor:` computes the probe start
`start := (anObject hash \\ array size) + 1` and probes for the first nil.  A
C++-replicated stringHash (jit_rt HASHCHK trace) proves the key's hash is CORRECT
and CONSISTENT (#A1DefinedInX = 123758516, %11 = 2 → expStart 3), and the array is
empty (occ all-nil) and really size 11 — yet scanFor: returns slot 5 or 2 (NEVER 3),
NON-DETERMINISTICALLY per run.  So scanFor:'s START computation is miscomputed: not
the hash (correct), not the size (NO_INLINE_SIZE doesn't change it), not the at:
read value (correct).  It is the inline-PRIM CONTINUATION inside the JIT-compiled
scanFor:: when its `array size` / `anObject hash` / `array at:` sends take the WARM
inline-prim path (vs the cold C++ dispatchCached path), the back-to-back inline-prim
continuation corrupts the `(hash \\ size)+1` result.  Path selection depends on IC
warmth, which varies run-to-run → the non-determinism.  Entry lands at a hash-
unreachable slot → KeyNotFound.

### Trace-instrumentation findings (2026-06-06, PHARO_T1_TRACE_MOD etc.)

Added gated traces to record scanFor:'s actual computation (PHARO_T1_TRACE_MOD:
[MOD-inline]/[MOD-C++]/[MOD-interp]/[MOD-scanFor]; PHARO_T1_TRACE_DICT_STORE: occ
bitmap + HASHCHK; IDHASH-GEN in identityHashOf; PHARO_T1_IDH_SCALE).  Results, which
CORRECT earlier assumptions and narrow it further:
  - The probe hash is `IdentityDictionary>>scanFor:` → `(anObject identityHash \\
    finish)+1` — identityHash, NOT stringHash (my earlier HASHCHK used the wrong
    hash).  SystemEnvironment uses IdentityDictionary>>scanFor:.
  - `Object>>identityHash` = basicIdentityHash << 8 (scaled): A1 basicIdentityHash
    4073496, identityHash 1042814976.  The inline `identityHash` (AsmjitT1
    tryPrimIdentityHash, primKind 20 via inlinePrimKind(75)) returns the RAW 22-bit
    value, but `PHARO_T1_IDH_SCALE=1` (apply the <<8) does NOT fix it — so scanFor:
    uses the scaled identityHash METHOD (not the raw inline), and this isn't it.
  - `finish` ALWAYS equals the array's real slotCount ([MOD-scanFor]) — size is
    correct, never stale.
  - Each test-key symbol's identityHash is generated EXACTLY ONCE (IDHASH-GEN
    count=1) — symbols' header hashes are stable, not corrupted by a wild write.
  - A1's misplaced entry is placed by `grow → noCheckAdd:` into an EMPTY 11-slot
    array (occ all-nil) at slot 5 (pass) vs 2 (fail), per run — but A1's specific
    scanFor: modulo does NOT appear in [MOD-interp] (only an unrelated size-11
    IdentityDictionary's does), implying A1's scanFor: runs JIT for these calls.
  - basicIdentityHash is assigned fresh PER PROCESS (generateHash LCG), so
    cross-process hash values don't transfer — only in-process measurement counts.

Net: every individual input (hash value, scaling, size, at: value) is correct in
isolation, yet only the GLOBAL inline-prim disable (NO_INLINE_PRIM_AT = at:+at:put:
+size+identityHash via the shared t1InlinePrimAt gate) fixes it; no single
component's disable does.  This is a non-deterministic CONTINUATION INTERACTION when
scanFor:'s chained inline prims run back-to-back in JIT — it corrupts state
(stack/temp/control-flow), not any single prim's value, which is why value-level
tracing can't pin it.

OPEN: the exact instruction needs lldb single-step of the JIT-compiled scanFor: on a
failing (warm) run — the divergence is runtime-state dependent and non-deterministic,
so static asm dumps and value traces look correct.  Robust correctness-preserving
fix: `PHARO_T1_NO_INLINE_PRIM_AT=1` (routes scanFor:'s size/at:/identityHash sends
through the verified-correct C++ chain; keeps getter/setter/returns-self and all
non-prim inline specs).  Cheaper than the existing `PHARO_T1_NO_IC_PROBE=1`.

--- earlier (2026-06-06) characterization below ---

## REMAINING: the JIT-tier (T1) variant  (2026-06-06 deep characterization)

The real suite runs JIT+Sista; with the Sista fix, JIT-on still alternates P,F,P,F.
DETERMINISTIC under `PHARO_DET_SCHED=1` (even runs fail, `KeyNotFound #A1DefinedInX`
or `#SystemOrganization`).  At failure `Smalltalk globals includesKey: #A1DefinedInX`
is FALSE — a globals entry was never stored or was misplaced (a DROPPED/MISPLACED
DICTIONARY STORE, same *symptom* class as the Sista variant but a different
*mechanism*).

What it is NOT (ruled out by deterministic bisection, each PASS=6/FAIL=3 counted by
actual PASS lines — note `grep -c FAIL`==0 is NOT a pass, some configs suppress the
eval's output entirely → count PASS lines):
  - NOT stale-method dispatch: `PHARO_T1_VALIDATE_IC` re-resolves the cached IC
    method in the receiver's live class at BOTH ExitSendCached handlers
    (Interpreter.cpp `validateICTarget`, tags RESUME@19214 + CHAIN@22341) → 0
    mismatches.  The dispatched method is always correct.
  - NOT classIndex reuse: `PHARO_CTCHECK` shows 0 `[CTOVERWRITE-reuse]` events.
  - NOT scavenge / a missing write barrier: `PHARO_YG_NO_SCAVENGE=1` still fails;
    `PHARO_SCAV_DANGLE_CHECK` logs nothing.  (Remembered set is dead code — scavenge
    full-scans old space, so missed barriers are tolerated.)
  - NOT preemption frequency: `PHARO_DET_SCHED_QUANTUM=10000` still fails.
  - NOT J2J / IC-fill / method-cache / megacache / cold-path bookkeeping:
    `PHARO_NO_J2J`, `PHARO_NO_IC_FILL`, `PHARO_NO_METHOD_CACHE`, `PHARO_NO_CHAIN`,
    and replaying the cold path's pendingICPatch_/cacheMethod/megaCacheAdd into the
    hit handler (`PHARO_T1_HIT_COLD_SIDE`=1/2/4/8/15) all still fail.

What it IS — the IC-HIT inline-spec CONTINUATION path:
  - `PHARO_T1_PROBE_ALWAYS_MISS=1` (emit probe, force every send cold) → PASS=6.
  - `PHARO_T1_HIT_FORCE_DISPATCH=1` (IC hits, but skip ALL inline-spec dispatch and
    go straight to dispatchCached → EXIT_SEND_CACHED → C++) → PASS=6.
  - `PHARO_T1_NO_INLINE_PRIM_AT=1` → PASS=6, but `PHARO_T1_NO_INLINE_PRIM_ATPUT=1`
    (at:put: only) → FAIL.  So the inline `at:` READ path is the differentiator,
    NOT the write.
  - But the inline `at:` VALUE is CORRECT: `PHARO_T1_VERIFY_AT=1` recomputes each
    inline-at read in C++ (jit_rt_verify_inline_at) → 0 `[INLINE-AT-MISMATCH]`, and
    the bug still reproduces.  So `at:` is the trigger, not the corrupter: disabling
    it just forces a round-trip to C++ that AVOIDS a downstream continuation bug.

Synthesis: when an IC-hit inline spec (the hot `array at: index` in a `scanFor:` /
`findElementOrNil:` loop) is serviced IN-STENCIL and execution CONTINUES inline
(rather than exiting EXIT_SEND_CACHED to the C++ chain loop), some downstream
control-flow in the scan loop is corrupted — `scanFor:` returns a wrong slot index,
so `at:put:` updates/inserts at the wrong slot and a later lookup at the correct
slot finds nil → KeyNotFound.  The globals dict array is 15287 slots (>255) so its
own inline `at:` always bails; the corrupting scan is on a SMALLER collection whose
miscompile cascades to the globals miss.  This is the "dispatch-protocol resume bug
family" (cf. sortStructs:into:, the hasNLR chain-loop block-resume fix at
Interpreter.cpp:~23469).  Layout-sensitive: any perturbation shifts the victim
(#A1DefinedInX ↔ #SystemOrganization).

Mitigations, best→worst (all keep correctness): `PHARO_T1_HIT_FORCE_DISPATCH=1`
(keeps fast IC dispatch, drops only the inline-spec continuation) is cheaper than
the existing `PHARO_T1_NO_IC_PROBE=1` (drops the whole probe → full lookup/send).
NEXT: disassemble the compiled small-collection `scanFor:` (capstone installed;
`PHARO_T1_DUMP_SEL=scanFor:` writes /tmp/jit_scanFor_*.bin) and find the wrong
loop-continuation after the inline-at stencil; or lldb-trace `scanFor:`'s returned
index vs a C++ recompute on the deterministic repro.

--- ORIGINAL investigation notes below ---

## Summary

The full-SUnit "blocker #4" (the SystemEnvironment / Package error-heavy region
~class 500 wedging the runner, previously described as a broad nondeterministic
value-corruption heisenbug) is a **speculative-inline miscompile in the optimized
execution tiers**. The pure interpreter is correct; both optimized tiers miscompile
under heavy class-create/recompile/remove load:

- **Sista tier:** `tryInlineConstReturn` (the inline-const-return speculative
  inliner, `src/vm/jit/sista/SistaBuilder.cpp`). `PHARO_SISTA_NO_INLINE_CONST=1`
  fixes it DETERMINISTICALLY.
- **T1 JIT tier:** the inline-cache probe. `PHARO_T1_NO_IC_PROBE=1` fixes the
  JIT-only variant.
- With BOTH tiers on (the real suite config) there is an additional interaction;
  neither single knob fully fixes the JIT+Sista combination.

The earlier memory note "pure interpreter (`PHARO_NO_JIT=1`) reproduces" was a
**false lead**: `PHARO_NO_JIT=1` does NOT disable Sista — `sistaDispatch` defaults
ON on arm64 (`DebugSettings.cpp`, `kDefaultSistaOn=true`); only `PHARO_NO_SISTA=1`
turns it off. So every "pure interpreter" repro actually had Sista active.

## Deterministic repro

`/tmp/perrun.st` runs `(PackageOnModelTest selector: #testAddTag) runCase` six
times in one eval and reports pass/fail + error class per run:

    cd /tmp/harness
    PHARO_NO_JIT=1 ./build/test_load_image /tmp/harness/Pharo-sunit-fixed.image \
        eval "$(cat /tmp/perrun.st)"

Config matrix (deterministic; `Pharo-sunit-fixed.image`):

    NO_JIT + Sista-on (default w/ NO_JIT)          P,P,F,F,F,F  (run3 NonBooleanReceiver, run4+ KeyNotFound)
    NO_JIT + NO_SISTA                              P,P,P,P,P,P  (pure interpreter — CLEAN)
    NO_JIT + Sista + PHARO_SISTA_NO_INLINE_CONST   P,P,P,P,P,P  (FIXES the Sista variant; 3/3 trials)
    JIT-on + Sista-on (real suite config)          P,F,F,F,F,F
    JIT-on + NO_SISTA                              alternating P/F (T1 variant)
    JIT-on + NO_SISTA + PHARO_T1_NO_IC_PROBE       P,P,P,P,P,P  (FIXES the T1 variant)
    JIT-on + Sista + NO_IC_PROBE + NO_INLINE_CONST P,P,F        (interaction remains)

`testAddTag` creates 3 packages + 4 classes, compiles 8 methods, then tearDown
`removeFromSystem`s them all — heavy class-create/method-recompile/class-remove,
which is exactly the suite's ~class-500 error-heavy region at small scale.

## What it is NOT (ruled out empirically)

- NOT the class table: `dumpClassTableConsistency()` (PHARO_CTCHECK) reports
  `orphanInstances=0`, no `registerClass` overwrites, every entry's
  `identityHash == index`, all entries valid — classOf/dispatch is sound.
- NOT the heap structure: walks cleanly to the free pointer every run, 0 bad
  headers.
- NOT the interpreter method cache: a full flush (`Object flushCache`, prim 89)
  between runs does NOT fix it.
- NOT symbol-table corruption: `#SystemOrganization identityHash` is constant and
  `'SystemOrganization' asSymbol == #SystemOrganization` holds throughout.
- NOT a GC/scavenge bug: NO GC fires during the runs (young-gen off; the one
  startup GC moves 0 objects).
- NOT stale inline hints at extraction: re-resolving each hint's selector in its
  classKey's class (PHARO_SISTA_VALIDATE_HINTS) drops only ~4 hints and does NOT
  fix the failure.
- NOT a loose class guard: the `kGuardClass` lowering compares the full 22-bit
  class index (`hdr & 0x3FFFFF` vs `expectedIdx`), so when an inline fires the
  receiver genuinely IS the guarded class.

## What it IS

A compile-time miscompile in `tryInlineConstReturn` that **scales with compile
count**: forcing extra recompiles (resetting the Sista compiled-fn cache on every
`flushJITCaches`) made it FAIL EARLIER (run 1), not later. So the bad code is baked
at compile time, and more compiles = more corruption — pointing to **shared
builder-state corruption in the recursive callee-lift** (`Builder::build(calleeOop,
...)` inside `tryInlineConstReturn`), not stale runtime data.

Symptom decomposition (after 2 testAddTag runs): a fresh `SystemEnvironment new
organization` raises KeyNotFound even though, on the same dict + key,
`includesKey:`=true, `findElementOrNil:` returns the correct index, and
`array at: thatIndex` IS the stored association. A manual reconstruction of
`organization` PASSES — only the REAL method's Sista-compiled form is wrong — so
it is the compilation of the actual Dictionary/SystemEnvironment methods that
miscompiles, not the data.

Bisecting shapes: disabling getter (`kLoadInstVar`) inlines at the common-emit
delays the failure run3→run4; disabling const inlines does nothing; only disabling
ALL of `tryInlineConstReturn` (`NO_INLINE_CONST`) fully fixes it — consistent with
a shared-mechanism (recursive-lift) bug rather than one shape.

## Why pure interpreter is correct but the tiers are not

The image never sends `flushCache` (prim 89) for `compile:`/`removeFromSystem`
(measured: 0 calls across 6 runs); it uses the per-method/selector prims 116/119
(23 calls/run, both of which DO clear all JIT ICs). The interpreter's `methodCache_`
keys on the class OOP and never returns wrong methods here; the optimized tiers
bake speculative inlines into compiled code that the IC flush does NOT recompile.

## Localization (2026-06-05, deeper dive — ruled out more)

- NOT the recursive callee-lift state: `Builder::build` constructs a FRESH lifter
  per call operating on the passed `out`, so the outer build's `out_`/`stack_`/
  `currentBlock_` are isolated; the only shared globals (g_currentBuildMemory,
  g_currentBuildHints, g_calleeLiftDepth, g_buildStartBcOffset) are all saved/
  restored.
- NOT the probe-lift side effect: `PHARO_SISTA_ICR_PROBE_ONLY=1` (do the recursive
  probe-lift but NEVER emit an inline) → 12/12 PASS. So the bug is in the EMITTED
  IR, not the probe-lift.
- NOT an out-of-bounds getter: added a soundness check (the inlined ivar index
  must be < the guarded class's instance field count); it never fires here, and
  the failure persists. Getter inlines are layout-consistent (right class,
  in-bounds ivar). The check is kept as a defensive hardening.
- Shape bisection is TIMING-CONFOUNDED: disabling a SUBSET of emissions
  (`ICR_NO_GETTER` / `ICR_NO_COMMON`) only DELAYS the failure (run3→run4) — it
  shifts the per-method compile schedule rather than removing the cause. Only
  disabling ALL emissions (`NO_INLINE_CONST` or `ICR_PROBE_ONLY`) fully fixes it.
  So the failure scales with the NUMBER of inline emissions, consistent with a
  data-dependent bug in the SHARED emission mechanism (the `kGuardClass` emission
  + `stack_` pop/push + deopt framepoint that every emitting shape performs),
  manifesting probabilistically across the compiled-with-inline methods.

## DECISIVE (2026-06-05): it's the guard-HIT inlined VALUE, not the deopt

`PHARO_SISTA_GUARD_ALWAYS_DEOPT=1` (lowering makes every `kGuardClass` compare
against an impossible class index, so EVERY speculative inline misses → deopts to
the real unspeculated send) → 6/6 PASS, deterministically. This is a RELIABLE
oracle (it keeps the IR structure / compile schedule identical, so it is NOT
timing-confounded, unlike the emission-disabling knobs). Conclusions:
- The deopt reconstruction (stack re-push + ip resume) is CORRECT. The earlier
  "deopt stack capture" hypothesis is REFUTED.
- The bug is the INLINED VALUE used on a guard HIT (when the receiver IS the
  guarded class). For some shape, the value emitted does not equal what the real
  send returns for that class.
- Plain getters (kLoadInstVar) are proven correct, so the culprit is a NON-GETTER
  value: kConstantOop (const), kLoadTrue/FalseOop (bool), kLoadReceiver (`^self`),
  kLoadTemp (`^arg`), kLoadLiteral, or a chained shape's final inner value.

## Multi-agent audit (2026-06-05) — found a real ORTHOGONAL bug, not #4

A 10-agent adversarial review of every emission shape ran. Its *headline*
("blocker #4 is not the inliner") is WRONG — it repeated the refuted
"NO_JIT disables Sista" mistake and never tested NO_JIT + NO_INLINE_CONST. That
combo was re-verified 12/12 PASS this session, alongside GUARD_ALWAYS_DEOPT and
NO_SISTA — so #4 IS the inliner, definitively.

But the audit DID find + (we) fix one real latent defect: the chained
self-forwarder shapes (SistaBuilder.cpp 6393/6409/6704) set
`inlineOp = kLoadReceiver` and fall through to the common emit, which lowered a
bare kLoadReceiver to `OFF_RECEIVER` (the OUTER method's self) instead of the
send-site receiver `recvId`. Fixed at the common-emit tail (special-case
kLoadReceiver → recvId, mirroring the 2-value direct-^self fix at 6235-6263 and
commit 4b446bf4). This is a genuine wrong-value-on-HIT bug — but it does NOT move
the blocker-#4 repro (verified P,P,F with it applied). KEEP it on its own merits.

Candidates that are NOT #4 (empirically ruled out, reliable since a value/struct
fix doesn't change the speculation mix): kLoadReceiver fix (applied, no change);
the kLoadTemp `^arg` literal forwarder @7155 (that code is actually CORRECT — the
value is the caller's arg/literal, `v1.literal` indexes it right); the multi-block
splice (PHARO_SISTA_NO_SPLICE → still P,P,F, so the splice's deopt-stack asymmetry
at 6005-6071 is latent, not #4).

**Bisection is fundamentally confounded** for this bug: any PARTIAL disable/deopt
(NO_GETTER, NO_COMMON, DEOPT_COMMON, DEOPT_OP=11, NO_SPLICE) only DELAYS the
failure (run3→run4) because it changes the speculation MIX → shifts per-method
warmup/compile schedule. Only ALL-off (NO_INLINE_CONST / GUARD_ALWAYS_DEOPT /
NO_SISTA) is clean. So the exact shape CANNOT be pinned by flag bisection. The
two reliable techniques left: (1) apply a candidate value-fix and re-run perrun
(a correct value fix can't be confounded); (2) RUNTIME value-verification — at
each guard HIT, compute the inlined value AND the real send result and trap on
mismatch (catches the exact wrong inline without changing the mix). (2) is the
definitive next step.

## MAJOR REFRAME (2026-06-05): the inline VALUE is correct — the bug is
## compiled code DOWNSTREAM of the inline

Built runtime value-verification: a new `kVerifyInline` IR op
(PHARO_SISTA_VERIFY_INLINE) wraps every 0-arg const-return inline so the lowering
does the REAL send (outer selector resolved in the actual receiver's class),
logs `[INLINE-MISMATCH]` when it differs from the speculated value, and uses the
REAL value. New op + `jit_rt_sista_verify_inline` helper (JITRuntime.cpp,
saves/restores sp) + arm64 lowering + builder `wrapVerify` at common + all 13
direct-emit sites.

Result on the repro: only ONE mismatch in the whole run (`#accessMode` on
WorkingSession — a real but unrelated stale-getter defect, NOT in the test path),
and using the correct value for EVERY 0-arg inline does NOT fix blocker #4 — it
fails with the IDENTICAL symptom (run 3 NonBooleanReceiver). So:

**The speculative inline VALUE is correct. The inline is only an ENABLER.** When
`tryInlineConstReturn` inlines a const-return send, the lifter CONTINUES past it
(otherwise the lift terminates and bails at that send — SistaBuilder.cpp:4661).
So disabling the inline (NO_INLINE_CONST) or forcing it to deopt
(GUARD_ALWAYS_DEOPT) both make everything FROM that send onward run in the
INTERPRETER — which is correct. Keeping the inline (even with a verified-correct
value) keeps the post-send code running in COMPILED form — which is buggy.

⇒ The real defect is a miscompiled op/sequence in the Sista lowering that is only
REACHED in compiled form when an inline extends the compiled region past a
const-return send. It is NOT the inline itself. (This also explains why every
inline-shape bisection was confounded — the inline is incidental.)

NOTE: the x86-only PHARO_SISTA_NO_LOWER_* knobs do NOT affect the arm64 lowering,
so they can't bisect this. Next: find the buggy downstream op by (a) adding an
arm64 per-op bail knob, or (b) auditing the lowering of the ops that appear right
after const-return sends in the dict path (findElementOrNil:/scanFor:/at:put:) —
array access (kPrimAt/kPrimSize), the hash modulo (kPrim*Int), conditionals
(kBranchIfFalse), and especially the FRAMEPOINT/deopt of a downstream
kSendUnspeculated whose live-value stack now includes an inlined value.

## arm64 per-op bail bisection (2026-06-05): the bug is in COMPLETE methods

Added PHARO_SISTA_ARM_BAIL_OP=<opNum>: fails arm64 lowering of that Op so any
method containing it bails to the interpreter.  Run against the repro:

    BAIL_OP=21 kGuardClass  => 12/12 PASS   (no inline-bearing method compiles)
    BAIL_OP=17 kReturn      => 12/12 PASS   (no FULLY-COMPILED method runs)
    BAIL_OP=2  kLoadReceiver=> 12/12 PASS   (likely frequency: all buggy methods use self)
    BAIL_OP=11 kLoadInstVar => P,P,P,F      (delay — frequency)
    BAIL_OP={at,size,atput,identityEq/Neq,branch*,sends,storeIvar,*Int arith/cmp,
             phi,framestate,loadLiteral,inlineSend,blockValue,allocArray,...}
                            => P,P,F        (NO effect)

kReturn=17 is the sharp one: a method reaches kReturn only when the lift COMPLETES
(no mid-method bail), which requires inlining all its const-return sends.  So the
buggy methods are exactly the FULLY-COMPILED, inline-bearing ones — and the defect
is NOT any single op (bailing each downstream op individually does nothing).  It is
a holistic miscompile of the complete compiled method that the inline enabled.

NEXT: identify the specific complete inline-bearing method in the dict path
(findElementOrNil:/scanFor:/at:put: family), dump its IR (PHARO_SISTA_DUMP_NODES)
and lowering, and find what goes wrong in the post-inline region — candidates: temp
/ block-local tracking after an inlined value, or a control-flow merge (phi) /
deopt framepoint that the inline subtly perturbs.  EXCLUDE_SELS bisection needs the
FULL cross-run set of complete inline methods (a single-run ICR_LOG misses methods
that only compile by run 3).

## Method-level localization (2026-06-05): SYSTEMIC across complete inline methods

Added PHARO_SISTA_BAIL_INLINE_LO/HI (bail inline-bearing methods by compile-order
index) + PHARO_SISTA_LOG_INLINE_IDX (logs [INLINE-METHOD] idx+oop in the lowering
and [COMPILED] oop+selector at the compile site).  41 inline-bearing methods
compile across the repro; bailing ALL → 12/12 PASS.  But bailing [0,21) fails at
run 3 and [21,41) fails at run 4 — BOTH halves contain buggy methods, so it is
NOT one culprit.  The 41 methods are DIVERSE (isSimulatedStyle, hasIcon, isDead,
collectionSpecies×3, properties:, basename, closed, position, packageName,
fullPath, organization:, ...) — UI, collection, and package methods alike.

⇒ The miscompile is SYSTEMIC: essentially ANY method that completes compilation
with an inline runs wrong.  Combined with the earlier facts (inline VALUE correct
via kVerifyInline; GUARD_ALWAYS_DEOPT fixes; bailing each downstream op does not),
the defect is in the GUARD-HIT continuation of a completed compiled method — the
post-inline compiled code is wrong in a way that is (a) not the inline value, (b)
not any single op's lowering, (c) not the deopt path (always-deopt is correct).
Prime remaining suspects: a register/liveness issue introduced by the kGuardClass
hot path, or a framepoint/temp-tracking subtlety that only bites once the lift
continues past the inlined send.  NEXT: dump the Sista IR + asmjit nodes of one
small complete inline method (e.g. collectionSpecies) and diff the post-inline
region against the same method compiled WITHOUT the inline (NO_INLINE_CONST).

## Culprit methods isolated (2026-06-05)

PHARO_SISTA_KEEP_INLINE_IDX=<idx> compiles ONLY that inline method (bails the
rest).  Isolating each of the 41:
  - ProtocolAnnouncement>>classAffected  (`^ self classReorganized`, a getter
    forwarder; classReorganized = ivar 2) → P,P,F when ISOLATED = the run-3
    (primary) culprit.
  - SystemEnvironment>>organization:  (`anOrganization environment: self.  ^ self
    at: #SystemOrganization put: anOrganization`; inlines the `environment:`
    setter) → P,P,P,F isolated.  Directly in the failing path.
  - The other ~39 inline methods isolate CLEAN.

Both culprits look CORRECT on inspection (the getter forwarder inlines to
kLoadInstVar(self, 2); the setter inlines to kStoreInstVar(anOrganization, self, …)
which the lowering handles with the operand receiver, not OFF_RECEIVER).  And
kVerifyInline reported no value mismatch for either.  So the miscompile is at the
asmjit/lowering level of these complete methods, not the inline value.

CAVEAT — index instability: the compile-order index (s_inlineMethodIdx in the
lowering) shifts run-to-run because method warm-up/compile ordering is driven by
the wall-clock heartbeat.  Stabilize with PHARO_DET_SCHED=1 before trusting a
specific idx↔method mapping, OR bail by method oop/selector.

NEXT: with PHARO_DET_SCHED=1 (stable order), KEEP-isolate classAffected and dump
its asmjit code (PHARO_SISTA_ASMJIT_LOG / DUMP_NODES) — compare the post-guard
region against the same method built with NO_INLINE_CONST to find the exact
mislowered instruction.  Toolkit knobs: KEEP_INLINE_IDX, BAIL_INLINE_LO/HI,
LOG_INLINE_IDX, ARM_BAIL_OP, GUARD_ALWAYS_DEOPT, VERIFY_INLINE.

## Older lead (now lower priority): non-getter VALUE shapes

Audit the VALUE emitted by the non-getter shapes for the case where it differs
from the real send's result for the guarded class. Suspects, by likelihood:
  - kLoadReceiver `^self`/yourself direct emit (SistaBuilder.cpp ~6230-6258): it
    pushes `recvId` (= stack_[size-nArgs-1]); verify recvId is the inlined send's
    receiver for every call shape, not the outer method's self.
  - kLoadTemp `^arg` (~6260-6290) and the literal/temp arg-forwarders
    (~7090-7161): verify the substituted arg/literal index.
  - kConstantOop const-return: verify the literal Oop is read from the correct
    callee literal slot (a wrong literal index would return a wrong constant).
  - chained inner const values (6628 / 7000s).
RELIABLE bisection tool: GUARD_ALWAYS_DEOPT can be made SELECTIVE (tag the guard
with the inlineOp in free bits 22-31 of guardLit and force-deopt only chosen
shapes) — same structure, no timing confound. Then confirm the fix with
scripts/repro/blocker4-perrun.st (PHARO_NO_JIT=1, expect all-PASS).

Interim correctness option: default `tryInlineConstReturn` off
(`sistaNoInlineConst`) and the T1 IC probe off until the emission bug is fixed —
at a perf cost on accessor-heavy code.

## Diagnostics added this session (all opt-in, zero-cost when off)

- `PHARO_CTCHECK` — `registerClass` overwrite log + `ObjectMemory::dumpClassTableConsistency()`
  on `anyClass flushCache`.
- `PHARO_NO_METHOD_CACHE` — force `probeCache` to always miss.
- `PHARO_SISTA_ICR_LOG` — log every `tryInlineConstReturn` emit (callee selector,
  shape, inlineOp, guarded class name).
- `PHARO_SISTA_VALIDATE_HINTS` — opt-in re-resolution filter for stale hints
  (partial; does not fix the miscompile).
