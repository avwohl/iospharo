// debug_vars.h — single source of truth for env-var debug/control knobs.
//
// NO include guard on purpose: this file is #included multiple times, each
// time with DEBUG_BOOL / DEBUG_INT / DEBUG_STR defined to do something
// different (build an enum, the storage array, the name table, ...).
//
// Add a knob in ONE place (here). The token is the literal env-var name and
// is used everywhere the programmer sees it:
//   declare:  DEBUG_BOOL(PHARO_FOO)
//   env var:  PHARO_FOO
//   fetch:    GET_DEBUG_BOOL(PHARO_FOO)
// No more keeping a .hpp declaration and a .cpp initializer in sync.
//
// Semantics: DEBUG_BOOL => present (getenv(name) != nullptr).
//            DEBUG_INT(name, default) => atoi if set+non-empty else default.
//            DEBUG_STR(name) => the value if set+non-empty else nullptr.
// Derived/computed knobs (combinations of several env vars, default-on
// opt-outs, etc.) stay in DebugSettings for now; this file is for the
// straightforward one-env-var-one-knob cases.

#ifndef DEBUG_BOOL
#define DEBUG_BOOL(name)
#endif
#ifndef DEBUG_INT
#define DEBUG_INT(name, dflt)
#endif
#ifndef DEBUG_STR
#define DEBUG_STR(name)
#endif

// ── aigraph inline-getter J2J transient: deterministic-scheduling debugger ──
// Drive the round-robin force-yield from the per-1024-bytecode checkpoint
// (g_stepNum) instead of the wall-clock heartbeat, so timing-dependent JIT
// transients reproduce at a FIXED point every run (de-Heisenbugs them).
DEBUG_BOOL(PHARO_DET_SCHED)
DEBUG_INT(PHARO_DET_SCHED_QUANTUM, 1)   // × the 1024-bytecode checkpoint
DEBUG_BOOL(PHARO_FINDNODE_WATCH)        // aigraph curEdge corruption capture
DEBUG_BOOL(PHARO_SCAV_DANGLE_CHECK)     // post-scavenge: log pointers still aimed into eden (missed roots)
DEBUG_BOOL(PHARO_HEAP_CHECK)            // post-fullGC: walk all objects, flag pointer slots aimed at out-of-heap/free/garbage targets
DEBUG_STR(PHARO_TRACE_EXTENT_SEL)       // trace every bytecode (sel+opcode+relDepth+TOS) within the dynamic extent of a method matching this selector
DEBUG_BOOL(PHARO_NO_METHOD_CACHE)       // bisect: force probeCache to always miss (every send does a full lookup) — isolates stale-method-cache corruption from lower-level bugs
DEBUG_BOOL(PHARO_OLDSPACE_FREELIST)   // After compaction, puts the gaps BELOW oldSpaceFree (stranded by pinned objects that sliding compaction cannot move) on the free list and lets old-space allocation reuse them. WORKS as of 2026-08-23 -- six bugs fixed to get here (raw native pointer written into a heap slot read back as an Oop; allocateFromFreeList returning the shifted chunk instead of the allocation base; nil-as-object read as a list terminator, which self-looped and hung the VM; allObjectsDo handing out classIndex-0 chunks; rebuildFreeListAfterCompact advancing prevEnd from the header not the base; and the real killer -- collectInstancesOfClass matching classIndex 0, so allInstances of a class with no class-table entry answered an Array of FREE CHUNKS, which is how class-shape migration died). Measured with it ON: 300-class SUnit batch 150s / 4861 pass / 0 fail / 0 error / 1 timeout, against 139s / 4861 / 0 / 0 / 1 with it off -- parity. NeoJSON package load 20s rc=0. VM binaries pass both ways. STILL DEFAULT-OFF pending a broader soak (full sweep + package tier both arches). Its point is to unblock the pinned-object relocation in docs/gc-oldspace-fragmentation-2026-08-22.md, which needs a working low allocator and would cut package images from 1.15 GB toward ~100 MB.
DEBUG_INT(PHARO_NEWSPACE_MB, 0)         // bisect: override newSpaceSize (MB, 0 = built-in default) — shifts scavenge timing to correlate heap-phase-dependent corruption with GC boundaries
DEBUG_INT(PHARO_GC_REPEAT, 1)           // test harness: run the forced pre-test fullGC N times, walking the heap (checkHeapIntegrity) after each. Compaction is not idempotent — run 2 starts from a differently laid out heap — so repeating is what catches damage that only appears once objects have already moved. Pair with PHARO_NEWSPACE_MB=1 to force the multi-pass compactor.
DEBUG_BOOL(PHARO_PRIM_FAIL_STORM)       // per-index primitive-failure counter; prints at 10k then every 100k per index — names the engine of exception storms (equal MNU/PrimitiveFailed counts)
DEBUG_BOOL(PHARO_HEAP_CENSUS)           // per-fullGC class-name heap histogram when live > 1 GB (plus always on the old-space-exhaustion FATAL path) — answers WHAT filled the heap
DEBUG_INT(PHARO_MAX_OLD_SPACE_MB, 0)    // override the old-space VIRTUAL reservation (MB, 0 = built-in 4096).  mmap lazy-commit: a big ceiling costs no physical RAM until used.  The full 2k-class catalog peaks near 4 GB; raise for headroom / leak diagnosis (was documented in MemoryConfig's comment but never implemented until 2026-07-07)
DEBUG_INT(PHARO_STATE_DUMP_PERIOD_MS, 0) // emit a [STATE-DUMP] (active proc/pri/method) every N ms from the periodic check — C-side scheduler sampler for stalls that image-side probes heal by existing (InLoop(UsingWorker) Heisenbug)
DEBUG_BOOL(PHARO_SCAV_RAWSCAN)          // post-scavenge: brute-force scan EVERY aligned word of old+perm for eden-range bit patterns (no format assumptions — catches holders the object-aware dangle check is blind to), attributing hits to their containing object
DEBUG_INT(PHARO_YG_SKIP_SCAV_FROM, -1)  // bisect: run scavenges 1..N-1 normally, skip (eden->old fallback) from the Nth on — converges on the first corrupting scavenge
DEBUG_BOOL(PHARO_GC_ROUNDTRIP_ONLY)     // bisect: at each scavenge safe point run ONLY prepareForGC()+afterGC() (ip<->offset round-trip, no object motion) — separates round-trip bugs from scavenge-proper bugs
DEBUG_BOOL(PHARO_SCAV_TRACE_STATE)      // log interpreter state (frameDepth, stack depth, current method) at each scavenge safe point
DEBUG_BOOL(PHARO_GC_FRAME_VERIFY)       // per-SAVED-frame ip round-trip verification: stash *savedIP in prepareForGC, check after afterGC rebuilds; also range-check savedIPOffset against the method's byteSize
DEBUG_INT(PHARO_SCAV_QUARANTINE_AT, -1) // at scavenge #N: page-protect eden instead of resetting it (allocation falls back to old space, later scavenges suppressed) — any later access through a stale eden pointer faults with a backtrace naming the guilty path
DEBUG_INT(PHARO_SCAV_FULLGC_AT, -1)     // bisect: at scavenge request #N run fullGC (the reference collector) instead of the scavenger — still-fails means the bug is shared/not-scavenge-specific
DEBUG_BOOL(PHARO_EDEN_ROTATE)           // stale-young-ref detector: eden alternates between two page-protected halves per scavenge, so a stale pointer to the PREVIOUS generation faults (mutator, with backtrace) or is flagged with holder attribution (inside the scavenge scans) instead of silently re-tenuring reused-eden garbage. Run with PHARO_NO_JIT (the JIT inline-new compares against the un-rotated eden limit cell).
DEBUG_INT(PHARO_TRACE_SENDS_FROM_SCAV, -1) // localizer: from scavenge request #N on, log every send as "selector<TAB>receiverClass" (address-independent) to C:/tmp/sendtrace.txt — diff a failing vs passing run to find the FIRST semantically divergent send. Run with PHARO_NO_JIT so all sends go through the interpreter path.
DEBUG_INT(PHARO_SCAV_DUMP_FORWARD, -1)  // at scavenge #N: dump the tenure forward map (edenAddr -> oldAddr per line) to C:/tmp/fwdmap.txt — cross-reference against oops captured at a later failure to identify which reference kept pre-scavenge bits
DEBUG_BOOL(PHARO_OCIR_ERROR_DUMP)       // when #error activates on an OCIRSequence: dump the convertStorePop frame's assoc (store/pop bits) and the sequence's OrderedCollection element bits — the identity-mismatch forensics
DEBUG_INT(PHARO_WATCH_OLDOFF, -1)       // watch the old-space object at oldSpaceStart+N: log every prim-105 copy and at:put: touching it (offsets are stable per run form under deterministic allocation, unlike ASLR'd addresses)
DEBUG_BOOL(PHARO_SLOT_RUN_TRIPWIRE)     // lowest-level smear catcher: fire (with backtrace) when ObjectHeader::slotAtPut writes an object ref into an Array slot whose two lower neighbors already hold the same ref — covers writers that bypass storePointer
DEBUG_INT(PHARO_DUMP_AT_ACT, -1)        // with PHARO_TRACE_SENDS_FROM_SCAV: at the Nth traced activation, dump the nearest convertStorePop frame's sequence array to C:/tmp/actdump.txt — capture the SAME logical moment in a failing vs passing run (identical streams until divergence) and diff
DEBUG_BOOL(PHARO_CTCHECK)               // log every registerClass assignment + flag overwrites of a live different class; dump class-table consistency on `anyClass flushCache` (prim 89)
DEBUG_BOOL(PHARO_SISTA_ICR_LOG)         // log every tryInlineConstReturn emit (callee selector, shape size, inlineOp, classOop hint) — narrows which method the Sista inline-const miscompiles
DEBUG_BOOL(PHARO_SISTA_ICR_PROBE_ONLY)  // bisect: probe-lift the callee but never emit the inline (isolates probe-lift side effect vs emission)
DEBUG_BOOL(PHARO_SISTA_ICR_NO_COMMON)   // bisect: skip only the common-emit (const/bool/getter fall-through) shapes
DEBUG_BOOL(PHARO_SISTA_GUARD_ALWAYS_DEOPT) // bisect: make every kGuardClass always miss → every inline deopts to the real send (isolates guard-hit value bug vs deopt-reconstruction bug)
DEBUG_BOOL(PHARO_SISTA_DEOPT_COMMON)     // bisect: force all common-emit inlines to deopt (real send), keep direct-emit shapes live
DEBUG_INT(PHARO_SISTA_DEOPT_OP, -1)      // bisect: force-deopt only common-emit inlines whose inlineOp == this value (-1 = off)
DEBUG_BOOL(PHARO_SISTA_NO_SPLICE)        // bisect: disable the multi-block splice (canSpliceMultiBlock returns false)
DEBUG_BOOL(PHARO_SISTA_CHECK_STACK)      // verify each tryInlineConstReturn inline leaves stack_ at (before - nArgs); logs [ICR-STACKBAL] on a mis-balanced shape
DEBUG_BOOL(PHARO_SISTA_VERIFY_INLINE)    // wrap each 0-arg const-return inline in a runtime check: do the REAL send, log [INLINE-MISMATCH] when it differs from the speculated value, use the real value (definitive blocker-#4 localizer)
DEBUG_INT(PHARO_SISTA_ARM_BAIL_OP, -1)   // bisect: fail arm64 lowering of this Op num so methods containing it bail to interp (localizes downstream miscompiled op)
DEBUG_BOOL(PHARO_SISTA_VERIFY_STORE)     // localizer: at each Sista kStoreTemp, call jit_rt_sista_verify_store which reads tempBase[idx] and flags a monotonic LargeInteger accumulator that SHRANK mid-loop ([SISTA-STORE-ANOMALY]) — pins the factorial miscompile to a store (multiply produced wrong) vs between-store-and-load (memory/GC)
DEBUG_BOOL(PHARO_SISTA_VERIFY_LOAD)      // localizer companion: at each Sista kLoadTemp, call jit_rt_sista_verify_load which logs the loaded value's byte-size for the same monotonic-accumulator slot ([SISTA-LOAD]); a load reading SMALLER than the prior store == corruption between store and load (GC/memory)
DEBUG_BOOL(PHARO_SISTA_NO_TAGCHECK_SKIP) // bisect: never skip the kPrimTagCheckInt deopt even when the operand IR type is kOopSmallInt — tests whether a mis-narrowed (loop-carried, can-go-Large) accumulator typed SmI is slipping a Large value past the guard (the factorial miscompile)
DEBUG_BOOL(PHARO_SISTA_INJECT_CAPTURE)   // opt-IN (default EXCLUDE since 2026-06-19): admit inject:into: do-splice for blocks that CAPTURE an outer var (numCopied!=0). Unsound: a block that merely READS an outer method arg/temp ([:sum :each | sum * factor + each]) captures a SCALAR copied value, but the splice treats it as a writable temp-VECTOR (kLoadTempInVec) and the extra capture push desyncs the simulator stack -> swaps the inject receiver with the scalar (PMPolynomial>>value' erf -> SmallFloat64>>inject:into:). Default now rejects -> captured-var blocks fall back to the correct send; closed blocks (numCopied==0) still splice. Set this to restore the old unsound path.
DEBUG_INT(PHARO_SISTA_BAIL_INLINE_LO, 0)  // bisect: low bound of inline-method compile-order indices to bail
DEBUG_INT(PHARO_SISTA_BAIL_INLINE_HI, -1) // bisect: high bound (exclusive); -1=off. bail inline-methods with idx in [LO,HI)
DEBUG_BOOL(PHARO_SISTA_LOG_INLINE_IDX)   // log [INLINE-METHOD] idx+methodOop for each inline-bearing method compiled
DEBUG_INT(PHARO_SISTA_KEEP_INLINE_IDX, -1) // bisect: compile ONLY the inline-method with this compile-order idx; bail all other inline methods
DEBUG_STR(PHARO_SISTA_KEEP_METHOD_OOP)    // bisect: compile ONLY the inline method with this method oop (hex); bail all other inline methods (stable across runs)
DEBUG_STR(PHARO_T1_RESUME_ONLY_SEL)       // debug isolation: force send-resume ON only for T1 methods whose selector == this value (resume stays OFF for all others, incl. startup). Repro/validate the send-resume protocol on one controlled method without breaking startup.
DEBUG_BOOL(PHARO_J2J_LOG_FILL)            // log every IC J2J bit-60 fill/upgrade as [J2JFILL] caller->callee (+selfrec flag), capped. Used to enumerate engaged (caller,callee) pairs under global inline-J2J for bisecting the startup corruptor.
DEBUG_BOOL(PHARO_J2J_MAT_LOG)             // log every J2J-save materialize (site, saveJM selector, receiver class, sendArgs, ip) + flag IP-OUT-OF-RANGE saves whose ip isn't inside saveJM's bytecode range = stale save.jitMethod written by the xmethod save-push
DEBUG_STR(PHARO_J2J_MAT_SEL)              // substring filter: J2J-MAT entries whose saveJM selector matches are logged UNCAPPED (the general log caps at 200 lines, before the corruption window)
DEBUG_BOOL(PHARO_J2J_NO_HEAPWRITE_CALLEES) // bisect: refuse bit-60 J2J fills for callees with hasHeapWrites (ivar-storing methods like initializeHandle:offset:). If the deterministic default+DET_SCHED DNUs vanish, the J2J corruption class is ivar stores in J2J-called code.
DEBUG_INT(PHARO_T1_SAVELESS_MIN_COMPILE, 0) // bisect: emit the saveless call path only in methods whose compile sequence >= N (excludes startup-compiled callers; lets a controlled late-compiled site exercise saveless in isolation)
DEBUG_INT(PHARO_T1_SAVELESS_MAX_ARGS, 8)    // bisect: emit saveless only at send sites with nArgs <= N (0 isolates the unary-leaf shape, proven correct)
DEBUG_BOOL(PHARO_T1_SAVELESS_NO_EXTRAS)     // bisect: saveless only for callees with tempCount == nArgs (runtime check; skips the dynamic nil-fill shape)
DEBUG_STR(PHARO_T1_SAVELESS_FORCE_SEL)      // ceiling EXPERIMENT: force canSkipJ2JSave for methods whose selector is in this comma list — UNSOUND in general (recovery stub retro-appends out of order when the callee pushed saves); use on bench selectors only to measure how much of the per-call Cog gap is J2J save traffic
DEBUG_BOOL(PHARO_T1_NO_INLINE_J2J)          // opt-OUT of inline-J2J (default ON since 2026-06-10, lever e: benchFib 296->32ms, cfib 344->44ms; 200-class suite per-test identical). Replaces the DebugSettings envPresent line per the no-new-envPresent rule.
DEBUG_BOOL(PHARO_NO_RECOMPILE_IC_RETARGET)  // opt-OUT of scrubbing IC entries that still point at a method's pre-recompile code (default ON). A monomorphic J2J site branches straight to icData[2]'s entry address and never consults the MethodMap, so without the scrub every existing caller keeps running the superseded tier-1 body forever -- measured 1242 stranded entries across 369 of 597 recompiles.
DEBUG_BOOL(PHARO_T1_BV_KEEP_INLINE_J2J)     // opt-out: keep cross-method inline-J2J even when BV inline is on (buggy: value:value: block clean-inline corruption; for A/B)
DEBUG_BOOL(PHARO_T1_NO_CAN_SKIP_J2J_SAVE)   // opt-OUT of the saveless leaf-call path (default ON since 2026-06-10, Eδ.2d complete: 200-class A/B per-test identical, 10x cfib 423 vs 447ms). Replaces the DebugSettings envPresent line.
DEBUG_BOOL(PHARO_T1_NO_INLINE_NEW_ASM)      // opt-OUT (default ON 2026-06-20): inline eden allocation directly in tier-1 emitted asm for basicNew (0-arg, fixed-size pointer class, AsmjitT1.cpp:8610) AND basicNew: (1-arg): POINTER-indexable (instFormat 2, Array new:N -> slotCount=size, nil-fill) and BYTE-indexable (instFormat 16-23, ByteArray/ByteString new:N -> slotCount=ceil(N/8), format=16+padding, zero-fill). Skips the jit_rt_basic_new[_with_arg] -> jitBasicNew -> primitiveNew C++ call chain. Reads instSpec from the class receiver at runtime; requires fixedSize 0; verifies classTable[identityHash]==class; bails to the C++ helper for words(9-15)/indexable-with-fixed/CompiledMethod(24)/overflow(slotCount>=254)/eden-full/negative-size/hash==0/non-canonical-index. GC-safe (eden bump inits header+slots before committing edenFree_; no safe point mid-init). slotCount/size in x17/x3 (NOT x5 = icDataPtr, which dispatchCached needs on the bail path). Set this to force the C++ helper path.
DEBUG_BOOL(PHARO_T1_NO_EDEN_NEW)            // opt-OUT (default eden-ON 2026-06-20): the JIT new fast-path (jit_rt_new_prim) allocates to EDEN (young) via ObjectMemory::allocateRawYoung, falling back to old space on eden-full / overflow(>=255 slots). Was old-space-only -> short-lived JIT objects skipped the young gen -> full-GC pressure (the shallowCopy old-space trap). Eden-full sets needsScavenge_ (deferred to a safe point, never mid-call -> GC-safe). Set this to force the old-space-only behavior.
DEBUG_BOOL(PHARO_T1_STORE_RING)             // store-provenance ring: every JIT receiver-ivar store (bytecode emits + inline-setter IC path) logs (recv, value, slot, callerJM) into a 64K ring; sendDoesNotUnderstand scans it for the corrupted oop and prints provenance — catches the corrupting WRITE for the J2J ivar-store corruption hunt
DEBUG_INT(PHARO_MAT_ONCE, 0)                // materialize EXACTLY ONCE, at the N-th checkpoint invocation (frameDepth_>0); 0=off — binary-search the single corrupting checkpoint
DEBUG_BOOL(PHARO_MAT_ONCE_DUMP)             // when PHARO_MAT_ONCE fires, dump the materialized frame stack (selectors) to stderr
DEBUG_STR(PHARO_MAT_SEL)                    // gate PHARO_MAT_AT_CHECKPOINT forcing to methods whose selector CONTAINS this substring — isolate which method's materialization corrupts (SlotIntegration)
DEBUG_BOOL(PHARO_NO_CLASSOF_FWD)             // A/B: disable classOf-follows-forwarders (revert pre-296bba26) — demonstrates forwarded-object dispatch MNUs without the fix
DEBUG_BOOL(PHARO_NO_BECOME_FORWARDER)        // opt-out: disable the default-on becomeForward-leaves-forwarder safety net (SlotIntegration fix). Set to bisect a suspected regression.
DEBUG_BOOL(PHARO_TRAP_IVAR0)                 // dump receiver+frame-chain when instVarAt: gets index<1 (SlotIntegration migration abort)
DEBUG_BOOL(PHARO_TRACE_SLOTCMP)             // dump slot-collection sizes at change-detector + addSlot:/slots:/copyWith: during SlotIntegration z-add
DEBUG_BOOL(PHARO_NO_FRAME0_REUSE)           // skip the frame[0]==activeContext_ re-materialization reuse path (force fresh context) — bisect SlotIntegration drop
DEBUG_BOOL(PHARO_TRACE_SLOTBUILD)            // trace return-into-context origSp/retval for slot-build methods (copyWith:/addSlot:/slots:) under DET_SCHED
DEBUG_INT(PHARO_MAT_STEP_LO, 0)             // gate MAT_AT_CHECKPOINT forcing to g_stepNum >= LO
DEBUG_INT(PHARO_MAT_STEP_HI, 0)             // gate MAT_AT_CHECKPOINT forcing to g_stepNum <= HI (0=no upper bound)
DEBUG_INT(PHARO_MAT_AT_CHECKPOINT, 0)      // force materializeFrameStack() every N checkpoints (no process switch) — isolates materialized-context execution as the SlotIntegration defect from scheduling; 0=off
DEBUG_BOOL(PHARO_TRACE_MATFS)               // log materializeFrameStack calls (active method + fd) — find the preemption point corrupting the SlotIntegration class build under DET_SCHED
DEBUG_BOOL(PHARO_PRIM_SEQ)                  // ring-buffer the last 65536 primitive indices; dump to /tmp/primseq.txt at VM exit (control-flow-divergence diff for the SlotIntegration bug: diff passing vs failing #z-add tails)
DEBUG_BOOL(PHARO_ALLOC_SIZE_CHECK)          // assert allocateSlots produces the requested slot count; log any mismatch (allocation-size corruption hunt for the SlotIntegration collection Heisenbug)
DEBUG_BOOL(PHARO_TRACE_BECOME)              // log every becomeForward with obj1/obj2 class-names + slot counts + classTable-hit — trace class-rebuild migrations (SlotIntegration trait-add bug)
DEBUG_BOOL(PHARO_SHADOW_SLOTS)              // shadow-slot verify-on-read: mirror tracked pointer-slot writes (JIT store emits, setReceiverInstVar, storePointer) into a 1M-entry shadow table; receiver-ivar reads verify against it. A [SHADOW-MISMATCH] = the slot changed via an untracked path (missing writer instrumentation or a GC-mover bug). Visibility-independent — works on any layout.
DEBUG_BOOL(PHARO_SP_DEPTH_CHECK)            // sp-desync detector (BcDepthMap.cpp): verify state.sp against the static operand-stack depth for state.ip's bcOffset at every checkable JIT exit (Send/SendCached/MustBool/ArithOverflow/Block/ArrayCreate/Yield). A [SP-DEPTH] = the frame's sp drifted from the bytecode-mandated depth — the corruption class behind all root-caused J2J bugs (shadow suite exonerated ivar stores). Layout-independent.
DEBUG_BOOL(PHARO_VERIFY_GETTER)             // verify-on-fire for the T1 inline getter (emit-time gate): BLR jit_rt_verify_getter after each inline slot read; flags a classification slotIdx >= the live receiver's slotCount (poisoned extra word: J2J address bits / foreign-site classification). [VERIFY-GETTER] = caught the misfire with caller provenance.
DEBUG_BOOL(PHARO_T1_NO_GETTER_IN_J2J)       // opt-OUT (default ON 2026-06-23): the dispatch-A-side tryGetter entry. Re-enabled after full-suite validation showed the 2026-06-10 corruption no longer manifests at HEAD (12674 P / 0 F, ZERO new regressions vs baseline JIT). 10x on getter-bound code (1M getter+yourself 40->4ms). Set this to force getter-classified sends back onto dispatchCached.
DEBUG_BOOL(PHARO_T1_GETTER_IN_J2J)          // (superseded; now default-ON via PHARO_T1_NO_GETTER_IN_J2J opt-out). Historical opt-in for the dispatch-A-side getter tbnz; was DEFAULT OFF 2026-06-10..2026-06-23 (catch22/23 MAX_IC=1 corruption, since resolved by later frame-state fixes).
DEBUG_BOOL(PHARO_J2J_STACK_SCAN)          // localizer: at each det-sched checkpoint, scan the live operand stack [stackBase_,stackPointer_) for a pointer-shaped slot (bit0=0, >=0x10000) that is NOT a valid heap object -> catches the global-inline-J2J raw-pointer corruption within 1024 bytecodes of the bad push, logging step+slot+value+current method. Capped.
DEBUG_BOOL(PHARO_T1_LOG_SELFREC_PUSH)     // emit a gated runtime call at each self-rec inline-J2J save-push that records the caller(=callee) CompiledMethod oop into a ring; dump_selfrec_ring() (called at the #extent DNU) resolves the last N to selectors -> NAMES the self-recursive method whose inline-J2J save/branch/return desyncs. DET_SCHED-safe (bytecode-counted scheduling unaffected by added instructions).
DEBUG_BOOL(PHARO_T1_NO_J2J_BRANCH)        // bisect (now gates the WHOLE if(inlineJ2J) send-emit block at AsmjitT1.cpp:3634): with INLINE_J2J=1, skip emitting the entire inline-J2J send-site block (J2J checks + dispatch-A + dead tryInlineJ2J/j2jBail blocks); bit-60 fill + return-prelude emit unchanged. PROVEN: this makes INLINE_J2J=1 CLEAN (3+4=7), so the #extent corruptor is the send-emit block itself, not the fill/prelude/push.
DEBUG_BOOL(PHARO_T1_NO_J2J_RETPRELUDE)    // bisect: skip emitting the per-method inline-J2J RETURN PRELUDE (the j2jDepth>j2jEntryDepth pop/resume epilog) even when PHARO_T1_INLINE_J2J=1. If this (with NO_J2J_BRANCH) makes startup match default, the return prelude mis-fires on trampoline-activated returns (shared j2jDepth) — the corruptor.
DEBUG_BOOL(PHARO_T1_NO_CHAIN_RESUME_PLAIN) // opt-OUT (default = plain bcToCode resume). COLD-BOOT DOUBLE-POP FIX (2026-06-19): the chain-loop precomputed-resume fast path (Interpreter.cpp ~27162) C++-pops nArgs + writes retVal then re-enters via plain JIT_CALL; resuming at the per-site resumeAfterCall continuation (codeOffsetForResume override) popped/wrote AGAIN (V2 double stack-effect) -> cold IC-miss nArgs>0 sends (do:-loop at:/value:) over-popped = the nested do:/on:do: cold-startup operand SHIFT. Fix resumes at the PLAIN bcToCode entry (codeOffsetForBC), matching the tryResume fallback twin which already relies on the same C++ pop. Set this to restore the old (double-popping) codeOffsetForResume lookup.
// ── inline-J2J dispatch-A "extra" inline specializations (IC bits 51-58) ──
// These are routed ONLY by the inline-J2J send emit's dispatch-A; the validated
// default/j2jBail dispatch paths never branch to them, so they were never
// exercised before inline-J2J (which never worked) = UNVALIDATED + buggy
// (tryMultiSlot writes a wild receiver -> the global-inline-J2J #extent crash;
// the set also yields mustBeBoolean).  DEFAULT-OFF (opt-in) so global inline-J2J
// is correct.  Re-enable a spec only after validating its emit.  (2026-06-09)
DEBUG_BOOL(PHARO_T1_INLINE_MULTISLOT)        // bit 57: ^ self[A] op1 self[B] op2 const. STILL OPT-IN: tryMultiSlot's wild receiver-slot write (#extent crash) is the one named defect not yet emit-fixed.
// bits 58/54/52 (returnsLiteral/tempReturn/intArith) FLIPPED DEFAULT-ON 2026-06-23
// via the NO_ opt-outs below.  The shared 2026-06-09 disable cause was a broken
// dispatch-A path (now fixed — proven by the getter flip riding it); these three
// carry no spec-specific defect (no receiver-slot write, no boolean-as-condition).
// Full-suite re-validation: 12674 P / 0 F, zero new regressions, 193.0s CPU (faster
// than baseline 200.7s).  The boolean-returning specs (intCmp bit 53, evenOdd bit 51)
// STAY opt-in — they are the named mustBeBoolean corruptors, needing the boolean
// emit audited first.
DEBUG_BOOL(PHARO_T1_NO_INLINE_RETURNS_LITERAL)  // opt-OUT (default ON 2026-06-23): bit 58 ^ nil/true/false/0/1
DEBUG_BOOL(PHARO_T1_NO_INLINE_TEMP_RETURN)      // opt-OUT (default ON 2026-06-23): bit 54 ^ arg N
DEBUG_BOOL(PHARO_T1_INLINE_INT_CMP_RETURN)   // bit 53: ^ self cmp arg (STAYS opt-in: named mustBeBoolean corruptor)
DEBUG_BOOL(PHARO_T1_NO_INLINE_INT_ARITH_RETURN) // opt-OUT (default ON 2026-06-23): bit 52 ^ self op arg (+/-/*)
DEBUG_BOOL(PHARO_T1_INLINE_EVEN_ODD)         // bit 51: Integer>>even/odd (STAYS opt-in: named mustBeBoolean corruptor)
DEBUG_BOOL(PHARO_T1_RESUME_INTERNAL_J2J)    // EXPERIMENT (v2, still corrupts): J2J pool slice for resumed methods; handler-protocol audit pending
DEBUG_BOOL(PHARO_T1_RESUME_EXTERNAL_J2J)    // opt-out: restore the null-slice external-J2J mode for tryResume-resumed methods (one C++ round trip per send)
DEBUG_BOOL(PHARO_T1_NO_IC_POLY_WALK)        // opt-out: probe IC slot 0 only (default walks slots 0-2 since 2026-06-12; poly sites missed once per activation -> interp residency)
DEBUG_BOOL(PHARO_T1_NO_RESUME_CONDJUMP)     // opt-out: refuse mid-method resume for cond-jump send methods (default ADVERTISE since 2026-06-12)
DEBUG_BOOL(PHARO_T1_ADMIT_BAILMID_CALLEES)  // opt-IN (default EXCLUDE since 2026-06-19): admit canBailMidMethod callees at the send-bearing xmethod J2J gate. They were admitted 2026-06-12 for perf, but PolyMath proved that when such a callee bails mid-body via ExitArithOverflow (SmallInteger arith overflowing to Fraction/Float), the caller's pending J2J save is left un-popped -> operand stack +1 -> wrong index to at: -> SubscriptOutOfBounds (51 PolyMath + Fuel failures, see docs/jit-test-packages.md). Default now EXCLUDES them (correct, +5-31% on collection/recursion); set this to restore the faster-but-wrong admit. The perf-preserving fix (correctly pop the save on V2 ExitArithOverflow, cf. PHARO_T1_AO_MAT_J2J which is V1-only) is a follow-up.
DEBUG_BOOL(PHARO_T1_LEAF_ALL_PRIMS)         // REPRO KNOB: prologue-leaf ALL supported prims (flickers the closure-as-receiver DNU ~1-in-3 ladders — the admitted-callee bail bug)
DEBUG_BOOL(PHARO_T1_NO_PROLOGUE_LEAF)       // opt out of prologue-leaf compiles (prim methods with unsupported bodies fall back to bail-on-entry stubs)
DEBUG_BOOL(PHARO_T1_NO_J2J_PRIM_PROLOGUE)   // opt out of admitting prim-prologue callees through the xmethod J2J gates
DEBUG_BOOL(PHARO_T1_SIEVE_GATE)             // restore the legacy stub-on-condjump gate for prim 60/61/62 methods (root cause fixed 2026-06-12; default OFF)
DEBUG_BOOL(PHARO_JIT_MATGUARD_DEEP)         // restore the 4-deep matRetSlot scan in canJITActivate (default 1-deep; the startup-DNU flake is guard-independent)
DEBUG_BOOL(PHARO_IGNOC_WIDEN)               // OPT-IN (unsound): VM-side ignoreOuterContext widening — skips materialize for body-clean blocks but breaks home identity + terminate-unwind of ensure: blocks (BlockClosureTest/SemaphoreTest, 2026-06-12)
DEBUG_BOOL(PHARO_NO_GEN_CLONE)              // opt out of generational clones (shallowCopy small non-overflow objects in eden); restores the old always-old-space behavior
DEBUG_BOOL(PHARO_PREEMPT_YIELDS)            // bisect: ALWAYS back-append a preempted process, at EVERY site including the ones Cog has no counterpart for. Not the same as honouring the image flag, which is now the default — see putToSleepPreemptedYieldingIf. This blanket form stalls the FFI callback suite. The old note here ("costs 4 scheduler/weak tests") no longer reproduces: 355 scheduler/weak tests are identical either way.
DEBUG_BOOL(PHARO_PREEMPT_NO_YIELD)          // bisect: ALWAYS front-append, i.e. disable the starvation guard added 2026-08-13.  Reproduces docs/vm-compat-bugs.md #20 (a zero-ms-Delay spinner starves an equal-priority peer forever).
DEBUG_BOOL(PHARO_RR_SCHED)                  // opt back IN to same-priority round-robin time-slicing (pre-2026-06-12 default; Cog never time-slices within a priority — rotation broke fork-window scheduling assumptions). Implied by PHARO_DET_SCHED.
DEBUG_BOOL(PHARO_T1_INLINE_BLOCK_CREATE)    // OPT-IN: in-JIT block creation via jit_rt_block_create — UNSOUND for J2J-hidden callers (caller frames invisible to materialize), see AsmjitT1 PushFullBlock comment
DEBUG_INT(PHARO_T1_INLINE_BLOCK_CREATE_MAX, -1) // bisect: inline-create only the first N emitted PushFullBlock sites (-1 = all)  // opt out of in-JIT block creation (restore the ExitBlockCreate exit/resume round trip)
DEBUG_BOOL(PHARO_T1_NO_PRIM_FALLBACK_BODY)  // opt out of compiling fallback bodies for unsupported-prim methods (restores the old refuse-whole-method behavior)
DEBUG_BOOL(PHARO_MAT_FULL_RESYNC)           // opt back into the legacy full per-frame re-sync in materializeFrameStack (incremental skip is default since 2026-06-11)
DEBUG_BOOL(PHARO_T1_NO_XGATE_FOLD)           // opt-OUT of the XGATE fold (extras bit 57 = precomputed cross-method gate verdict; emitted 4-load cascade remains as the unfolded fallback)
DEBUG_INT(PHARO_T1_XMETHOD_MAX_IC, 8)        // lever (c): admit cross-method inline-J2J callees with up to N IC send sites. DEFAULT 1 since 2026-06-10 (was 0 = leaf-only): with-send-callee bench 491 -> 57ms (8.6x); validated by an 80-rep eval catch loop + a 200-class suite at 2/8459 (both known environmental flakes). The leaf-only gate guarded bugs all fixed today: staticJ2JArgCount fold, stale bcStartCache, dispatch-A tryGetter entry, scavenge ip round-trip, swallowed block-NLR. Set 0 to restore leaf-only for bisects.
DEBUG_STR(PHARO_MAX_STEPS)                   // test_load_image bytecode-step budget override (parsed with strtoll; default 60e9). The optimized thrash-fixed build burns 60G steps in ~4-5 min of busy-idle, which kills SUnit suite runs before the runner produces output — set to e.g. 2000000000000 for suite runs.
DEBUG_INT(PHARO_CODE_ZONE_MB, 0)             // code zone size override in MB (0 = DefaultCodeZoneSize, 64MB). SUnit suites fill 64MB at ~14.6K methods (~6KB/method T1 emit, lever (d)) and late-hot methods then run interpreted (SHA256Test TIMEOUT) — use e.g. 192 for suite runs until the per-method emit shrinks.
DEBUG_BOOL(PHARO_SISTA_VALIDATE_HINTS)  // opt-IN: re-resolve each extracted Sista inline hint (selector lookup in classKey's class) and drop it if it no longer yields the cached method. Partial mitigation for stale-IC-derived hints (blocker #4); does NOT fully fix the JIT-tier IC staleness.
DEBUG_BOOL(PHARO_T1_VALIDATE_IC)         // ExitSendCached: re-resolve cached IC method in receiver class; on mismatch use the fresh method (blocker #4 T1 stale-IC dispatch)
DEBUG_INT(PHARO_T1_HIT_COLD_SIDE, 0)     // bisect blocker #4: replay cold-path bookkeeping in the IC-hit handler. bits 1=clear pendingICPatch_ 2=set pendingICPatch_ 4=cacheMethod 8=megaCacheAdd
DEBUG_BOOL(PHARO_T1_HIT_FORCE_DISPATCH)  // bisect blocker #4: on IC HIT skip all inline-spec dispatch, jump straight to dispatchCached
DEBUG_BOOL(PHARO_T1_NO_INLINE_PRIM_ATPUT) // bisect blocker #4: disable ONLY inline at:put: (keep inline at:), to separate write from read
DEBUG_BOOL(PHARO_T1_VERIFY_AT)           // diagnostic: recompute each inline at: read in C++ and log mismatches (blocker #4)
DEBUG_BOOL(PHARO_T1_LOG_AT_CALLERS)      // diagnostic: log distinct caller methods doing inline at: on small arrays (blocker #4 culprit hunt; needs VERIFY_AT)
DEBUG_BOOL(PHARO_T1_SETTER_BOUNDS)       // diagnostic: log inline-setter stores with OOB slot index (wild heap write hunt; blocker #4)
DEBUG_INT(PHARO_T1_INLINE_SYNC, 0)       // blocker #4 test: inline-spec continuation writes OFF_SENDARGCOUNT(1)/OFF_IP(2)/CACHED_TARGET(4)
DEBUG_INT(PHARO_T1_AT_NOPS, 0)           // blocker #4 layout test: N behavior-neutral NOPs at inline-at entry
DEBUG_BOOL(PHARO_T1_TRACE_MOD)           // blocker #4: log inline \ operands+result when dividend is hash-sized
DEBUG_BOOL(PHARO_T1_TRACE_DICT_STORE)    // blocker #4: log placement of test-key associations into dict arrays (slot+size+caller)
DEBUG_BOOL(PHARO_T1_SYNC_GLOBALS)        // blocker #4 test: sync C++ interp globals from JITState at inline-spec continuation
DEBUG_BOOL(PHARO_T1_NO_INLINE_AT_READ)   // bisect blocker #4: disable ONLY inline at: READ (keep at:put:/size/getter/setter)
DEBUG_BOOL(PHARO_T1_NO_INLINE_SIZE)      // bisect blocker #4: disable inline size (primKind 16) only — used in hash-probe index \ array size
DEBUG_BOOL(PHARO_T1_NO_INLINE_IDH)       // bisect blocker #4: disable inline identityHash (primKind 20) only — the last untested stencil
DEBUG_BOOL(PHARO_T1_NO_INLINE_CLASS)     // disable inline `class` (primKind 24 = prim 111): reads classTable[classIndex] inline, bypassing the send
DEBUG_BOOL(PHARO_T1_NO_INLINE_IMM_CLASS) // opt-OUT (default ON 2026-06-20): inline `class` for IMMEDIATE SmallInteger receivers (tag 1) at the send site (AsmjitT1.cpp:6394). The heap tryPrimClass can't serve immediates (no header) so `42 class` used to take the C++ send (the ~18x 'o class' gap); this loads classTable[g_jitSmallIntClassIndex] inline (idx baked per-compile, verified classTable[idx]==SmallInteger; GC-safe runtime load). Other immediates (Character/SmallFloat) + non-class sends bail to dispatchCached. Uses x6/x4 only (preserves x5=icDataPtr). Set this to force the C++ send for immediate class.
DEBUG_BOOL(PHARO_BV_FORCE_BAIL)          // TEMP BISECT: force jit_rt_inline_block_value_prep to always return nullptr (bail) — bit-59 classification + the tryBlockValue detour still run, but the BV inline HIT path never executes. Distinguishes HIT-path corruption from classification/bail corruption.
DEBUG_BOOL(PHARO_BV_DUMP_REAL)            // TEMP: bvEntryTrace at the BV br x9 (observe block-entry state)
DEBUG_INT(PHARO_BV_ONLY_ARG, 0)          // TEMP: BV helper hits ONLY when the first value: arg is this SmallInteger (else bail). Isolates one controlled BV hit (startup bails -> no runaway) without selector-string matching.
DEBUG_BOOL(PHARO_BV_TRACE_HITS)          // TEMP: log the first 120 BV-inline hits (receiver class, nArgs, temps, captures, numIC, j2jDepth) to characterize what blocks get inlined in the runaway.
DEBUG_BOOL(PHARO_BV_DBGTRAP)            // TEMP: __builtin_debugtrap at the cmpat block codeStart capture (for lldb single-step)
DEBUG_INT(PHARO_BV_MAX_CAP, 1)           // FIX 2026-06-20: bail BV inline if numCopied (captures) > this. Default 1: cap>=2 blocks corrupt under BV inline (foreign-frame EXECUTION bug; the C++ capture-copy was verified correct vs activateBlock, so the defect is in the JIT block's multi-copied-value access). Bisected: MAX_CAP<=1 stops the BV-on startup runaway. -1 disables the cap (to reproduce/investigate the cap>=2 bug).
DEBUG_BOOL(PHARO_T1_NO_BC_FLOAT)         // bisect G* SmallFloat bug: disable bytecode-level inline SmallFloat +/- fast-path (0x60/0x61)
DEBUG_BOOL(PHARO_HEADLESS)               // force headless: skip Metal test surface + GUI click injection so SUnit batches aren't poisoned by render-loop contention (no image args needed)
DEBUG_INT(PHARO_WATCH_HEAP_CLASSIDX, 0)  // Spur classIndex (== the class's identityHash) to trace during the GC MARK: reports the PARENT object+slot that reaches each instance, i.e. what is keeping it alive transitively. Companion to PHARO_WATCH_ROOT_CLASS, which only covers direct roots
DEBUG_STR(PHARO_TRACE_FRAME_TEMPS)       // selector substring: dump the C++ frame temp slots materializeFrameStack copies, to tell a stale CONTEXT from a stale FRAME
DEBUG_BOOL(PHARO_WEAK_SURVIVOR_CLASSES)  // cheap sibling of PHARO_WEAK_SURVIVOR_PATHS: name each weak referent that SURVIVED, without the per-object parent map — low enough overhead to leave on while chasing a timing-sensitive survivor the full tracer suppresses
DEBUG_BOOL(PHARO_WEAK_SURVIVOR_PATHS)    // record each object's first-reaching parent during the GC mark, then print the parent chain for every weak referent that SURVIVED — the direct answer to "why wasn't this collected?". One map entry per marked object; diagnostic runs only
DEBUG_STR(PHARO_WATCH_HEAP_CLASS)        // same as PHARO_WATCH_HEAP_CLASSIDX but by class NAME (resolved to an index once, by scanning classTable_)
DEBUG_INT(PHARO_WATCH_HEAP_MAXLOG, 400) // cap on [HEAP-WATCH] lines per GC
DEBUG_STR(PHARO_WATCH_ROOT_CLASS)        // name a class; forEachRoot reports which ROOT CATEGORY (operand-stack / saved-frames / jit-code-zone / j2j-save-pool / jit-state / ... or, for the singleton VM registers, the FIELD name) is visiting each instance. Answers "why wasn't this collected?" without knob-bisecting
DEBUG_STR(LD_LIBRARY_PATH)               // NOT a knob of ours — the platform's library search path, read here because this X-macro list is the single sanctioned env reader (bare getenv is poisoned).  ffi::ensurePlatformLibraryPath() prepends the platform lib dirs to it at startup, the way stock Cog's `pharo` wrapper script does
DEBUG_BOOL(PHARO_NO_PLATFORM_LIB_PATH)   // skip that LD_LIBRARY_PATH augmentation and leave the environment exactly as inherited
DEBUG_STR(PHARO_TRACE_DNU_STACK)         // selector substring: at a doesNotUnderstand whose SELECTOR matches, dump the LIVE frame's operand stack (framePointer_..stackPointer_) with class names. The image cannot show this — reading the materialized context after the fact gives a stale snapshot that disagrees with the frame, which is exactly the WarpBlt expression-stack displacement's signature
DEBUG_STR(PHARO_DEPTH_ORACLE)            // EXACT selector: operand-stack-depth oracle for one method. Records the depth (stackPointer_-framePointer_-1) the FIRST time each bytecode offset executes and reports every later visit that disagrees — a self-calibrating detector for expression-stack displacement that needs no static analysis (straight-line/loop code has a fixed depth per pc). On the first divergence it dumps the live frame, the owning context, and a ring of the preceding save/restore/return events
DEBUG_BOOL(PHARO_NO_CTX_STACKP_RAISE)    // disable Interpreter::raiseContextStackpTo, the setTemporary write-through's stackp bump added with the Context trace bound. Raising stackp is not GC-only: executeFromContext restores exactly `stackp` slots as the activation's temps, so a stackp raised past the real content SHIFTS the resumed operand stack. Bisect knob for anything that looks like a value landing in the wrong variable
DEBUG_BOOL(PHARO_CTX_TRACE_ALL_SLOTS)    // revert to tracing a Context's WHOLE slot array during GC instead of Spur's 6+stackp live extent. Off by default since 2026-08-11: the full scan made dead expression-stack residue a GC root and kept collectable objects alive one extra cycle (PropertyManagerTest>>testPropertyManagerValueWeakness). Set =1 to bisect a suspected stale-stackp crash back to this change
DEBUG_BOOL(PHARO_CHDIR_IMAGE_DIR)        // legacy: chdir to the image's directory at startup.  Off by default since 2026-08-11 — stock Cog leaves the CWD alone, and the chdir made every CWD-relative lookup the image does resolve against the image dir (plus broke relative image paths)
DEBUG_BOOL(PHARO_INTERACTIVE)            // bare launches default to NON-interactive (isInteractiveGraphic false) since 2026-07-06 — MorphicWindowManagerTest opened native windows headlessly and hijacked Clipboard Default; set =1 (or pass --interactive) for real GUI runs
DEBUG_BOOL(PHARO_TRACE_WEDGE_NIL)        // full-suite wedge: at a nil-receiver DNU, dump the real activeContext_ chain to find the T1-miscompiled method
DEBUG_BOOL(PHARO_NO_DELAY_HARD_RESTART)  // opt out of the VM-core self-heal: don't signal the registered image-side recovery semaphore (which drives Delay scheduler restartTimerEventLoop) on a [DELAY-DEATH] wedge
DEBUG_BOOL(PHARO_COMPILE_LOG)            // DIAGNOSTIC: log [COMPILE]/[RECOMPILE] <selector> for each JIT compile, to find run-once / transient-method compile waste
DEBUG_BOOL(PHARO_SISTA_RECOMPILE_ADMITTED) // A/B: restore the old behavior where kSistaGateAdmitted methods re-run extractInlineHints+sista->compile on EVERY entry (the recompile churn). Default OFF = skip already-Sista-processed methods.
DEBUG_BOOL(PHARO_ACTIVATION_LOG)         // DIAGNOSTIC: sample (every 4096th) the selector entering tryJITActivation (the interp->JIT boundary) to see what's bouncing -- warm (J2J-linkable, fixable) vs cold/transient (inherent)
DEBUG_BOOL(PHARO_T1_CHAIN_RESEND)        // J2J chain M1: on a chain-loop ExitSend fall (inline-J2J callee bailed with its own uncached send), resolve+re-enter the grand-callee in JIT instead of materializing+falling to interp. See docs/j2j-m1-guide.md.
DEBUG_BOOL(PHARO_T1_CHAIN_RESEND_VERIFY) // J2J chain M1 oracle: resolve-and-compare ONLY (no re-enter), trap [CHAIN-RESEND-VERIFY] on divergence, then take the OLD fall path (side-effect-free; behavior == knob-off). Run verify-clean before enabling PHARO_T1_CHAIN_RESEND.
DEBUG_BOOL(PHARO_TRACE_DELAY_SUSPEND)    // diagnose full-suite wedge: log when a Delay-handshake process (schedule:/wait/runTimerEventLoop) is suspended/terminated mid-handshake
DEBUG_BOOL(PHARO_TRACE_IDLE_YIELD)        // diagnose full-suite wedge: log handleForceYield reschedule decision when idle (P10) is active (does it transfer to a ready higher-pri process?)
DEBUG_BOOL(PHARO_TRACE_DELAY_NIL)        // diagnose full-suite wedge: at the `1000 * nil` MNU, dump the Delay sender chain + ivars to find store-vs-read of millisecondDelayDuration
DEBUG_BOOL(PHARO_RETMETH_TRACE)         // returnFromMethod trace for classDefinitionNode (scavenge "returns-receiver" bug)

// ── x86_64 Sista lowering: per-op disable knobs for crash bisection ──
// One build → toggle any op off via its env var to isolate a miscompile.
DEBUG_BOOL(PHARO_SISTA_NO_LOWER_AT)            // kPrimAt
DEBUG_BOOL(PHARO_SISTA_NO_LOWER_SIZE)          // kPrimSize
DEBUG_BOOL(PHARO_SISTA_NO_LOWER_ATPUT)         // kPrimAtPut
DEBUG_BOOL(PHARO_SISTA_NO_LOWER_FLOAT)         // kPrimAdd/Sub/MulFloat
DEBUG_BOOL(PHARO_SISTA_NO_LOWER_COUNTED_LOOP)  // kCountedLoop* fusions (incl. select)
DEBUG_BOOL(PHARO_SISTA_NO_WHILETRUE_TODO)      // OPT-OUT (default ON): admit the
                                               // to:do:-style pre-loop (counter init +
                                               // storeIntoTemp, START left on stack) into
                                               // the WhileTrueAccum recognizer.  Enabled by
                                               // default; the recognizer requires the inlined
                                               // counter back-edge so a real Interval `do:`
                                               // (no inlined counter) never matches.  Set this
                                               // to disable if a deopt-storm is observed.
DEBUG_BOOL(PHARO_SISTA_DUMP_NODES)             // dump each method's asmjit node graph to
                                               // stderr right BEFORE finalize() (the RA pass).
                                               // The LAST graph printed before a build_liveness
                                               // crash is the culprit method.

DEBUG_BOOL(PHARO_T1_LEAK_GUARD_OFF)      // emit-time: drop the 2-instr IC-probe defensive check for classifier-bit leaks in receivers (task #10 — root-caused since); suite-validate before defaulting

// ── Patched monomorphic send sites (PMS) — docs/patched-ic-design.md ──
DEBUG_BOOL(PHARO_T1_PATCHED_SENDS)        // NO-OP since the B4 flip (2026-06-10); kept for script compat
DEBUG_BOOL(PHARO_T1_NO_PATCHED_SENDS)     // opt-out (PMS default-ON since B4, 2026-06-10)
DEBUG_BOOL(PHARO_T1_PATCHED_SENDS_NOLINK) // shape on, linking off (within-binary A/B lane)
DEBUG_BOOL(PHARO_T1_PATCH_VERIFY)         // post-GC mirror check: decoded patch words == heap IC slot 0
DEBUG_BOOL(PHARO_T1_PATCH_STATS)          // link/unlink/refusal counter dump
DEBUG_INT(PHARO_T1_PATCH_MIN, -1)         // site-index bisect window lo (link only sites >= MIN)
DEBUG_INT(PHARO_T1_PATCH_MAX, -1)         // site-index bisect window hi (link only sites <= MAX; -1 = no cap)
DEBUG_BOOL(PHARO_T1_EMIT_HASH)            // print FNV-1a of emitted bytes per compile (byte-identity gate harness)
DEBUG_INT(PHARO_T1_PATCH_MAX_IC, -1)      // design §13 Q3: numICEntries cap for the LINK predicate only (-1 = use PHARO_T1_XMETHOD_MAX_IC); linked sites evaluate gates once in C++, so the cap can widen past the per-call gate's without re-emitting
// ── simStack: write-through TOS cache in x26 (docs/simstack-design.md) ──
DEBUG_BOOL(PHARO_T1_NO_RESUME_SENDS)      // opt-OUT: revert condjump-free send methods to non-resumable (pre-2026-06-11).  Default ON = such methods advertise resume; their post-send continuations re-enter JIT instead of stranding the activation in the interpreter.
DEBUG_BOOL(PHARO_T1_FSR_LAZY)             // FSR M4: delete the per-call mirror stores (method/literals/argCount/ip/jitMethod, ~9 insns/linked call); exits publish x19 and C++ choke points re-derive via syncDerivedFromJM.
DEBUG_BOOL(PHARO_T1_FSR_LAZY_VERIFY)      // FSR M4 stage (a): publish-at-exit ON, mirror stores STILL ON, C++ asserts mirror==derived at choke points ([M4-PARITY]).
DEBUG_BOOL(PHARO_T1_FSR_NODEPTH)          // FSR M3 stage b/c: prelude uses the entryCursor compare; push sites drop the depth RMW; C++ refreshes depth from the slice base at choke points.
DEBUG_BOOL(PHARO_T1_FSR_NODEPTH_VERIFY)   // FSR M3 stage a: maintain BOTH depth representations; C++ asserts parity at the chain-loop choke point ([M3-PARITY] log + abort on divergence).
DEBUG_BOOL(PHARO_T1_NO_FSR_CURSOR)        // opt-OUT: revert to memory-cursor J2J push/pop (FSR M2 default-ON 2026-06-11: x23 write-through residency; gate = 2468-test soak under the dual-cursor trap + ~9%% fib win)
DEBUG_BOOL(PHARO_T1_FSR_CURSOR_VERIFY)    // M2 dual-cursor audit: enables FSR_CURSOR and traps brk #0xF23 at dispatchCached when x23 != the memory cursor — the first trap names the divergent path.
DEBUG_BOOL(PHARO_T1_FSR_CURSOR)           // FSR M2 v1 (write-through): keep the J2J save-pool cursor resident in x23 for emitted push/pop address generation (kills the dependent cursor load per J2J call+return); memory stays authoritative via write-through, so every C++/helper observer is coherent with no BLR brackets.
DEBUG_BOOL(PHARO_T1_FSR_X19)              // FSR M1 (frame-state-residency.md): maintain x19 = active JITMethod* as an invariant — mov x19 at every activation commit + caller-restore in the emitted J2J paths.  Opt-in until M2 consumes it.
DEBUG_BOOL(PHARO_T1_FSR_X19_VERIFY)       // FSR M1 oracle: at send-exit stubs, trap (brk #0xF19) if x19 != the OFF_JITMETHOD mirror.  Run the suite with this + FSR_X19 to prove the invariant before M2 builds on it.
DEBUG_BOOL(PHARO_T1_FSR_VERIFY)           // FSR M0 dual-write oracle: at the chain/resume exit choke points, cross-check the JITState mirror (method/literals) against state.jitMethod's canonical fields; logs [FSR-VERIFY] divergences (capped).  The divergence census tells M4 which writers are load-bearing.
DEBUG_BOOL(PHARO_T1_TOS_REG)              // opt-in TOS-in-x26 simStack. 2026-06-23 A/B (stable in-image CPU metric): MASK=101 loop-body subset is PERF-NEUTRAL (operands spill to memory at send/jump boundaries); only the full conversion incl. Send families could help. Correctness-clean (1538 tests + TOS_VERIFY). Stays opt-in pending that completion.
DEBUG_BOOL(PHARO_T1_DISAGREE_DUMP)        // x86 coverage diag: on a prescan/emit disagree, dump the full method bytecode + disagree position (capped) to trace the emit-loop vs allBytecodesSupported stepping mismatch. Box-only (x86 emit).
DEBUG_BOOL(PHARO_T1_RETPRELUDE_STATS)     // B0.5 sizing: tally per-method return-op count + emitted bytes across a run; dump histogram + saveable-bytes at exit.  Measurement only, no codegen effect.
DEBUG_BOOL(PHARO_T1_SHARED_RETPRELUDE)    // out-of-line-dispatch B0.5 (docs/out-of-line-dispatch-design.md): route EVERY real non-block arm64 method's return ops to ONE zone-GLOBAL shared return-prelude+epilogue stub (a standalone never-freed MAP_JIT page), via `mov x16,stub; br x16` (x30 stays the live return link — no bl).  Single-return methods shrink too (~1.8% total zone).  Disabled under per-method VERIFY knobs (the VERIFY epilogue needs g_codeStartLabel) -> falls back to inline.  Opt-in; knob-off byte-identical.  Zone/i-cache win, latency-neutral.  (B0 was the per-method >=2-return precursor; B0.5 supersedes it.)
DEBUG_INT(PHARO_T1_TOS_MASK, -1)          // bit per converted family (-1 = all): per-term A/B + miscompile bisect. Note: all-on (-1) is net-NEGATIVE; the loop-body subset 101 (Push|Dup|PopStore|Arith) is perf-NEUTRAL (measured) — see PHARO_T1_TOS_REG
DEBUG_BOOL(PHARO_T1_TOS_POISON)           // movz x26,#0xDEAD at every invalidation point — stale-valid consumers crash deterministically
DEBUG_BOOL(PHARO_T1_TOS_VERIFY)           // before every cache-consuming emit: ldur/cmp/b.eq/brk — THE net for the valid-but-stale direction
DEBUG_BOOL(PHARO_T1_REGSTACK_CENSUS)      // Phase-1 go/no-go measurement (docs/t1-codegen-plan.md): per compiled method, count how many operand loads a static stack-to-register map could keep out of memory (removable = consumed by inlined arith within a straight-line run, no send/merge). Pure measurement, no codegen change. Prints [REGSTACK-CENSUS] at JIT-stats dump.
DEBUG_BOOL(PHARO_T1_SEND_CENSUS)          // Phase-1 send-path go/no-go (docs/send-path-plan.md): at the C++ chain-loop convert_send split, count sends that J2J-convert (fast) vs stay dispatchCached (the addressable B6 quick-prim bucket), with a per-selector histogram of the dispatch bucket. Pure C++ counting, no codegen change. Prints [SEND-CENSUS] at JIT-stats dump.
DEBUG_BOOL(PHARO_T1_NO_NATIVE_BACKJUMP)   // opt-OUT: revert prefixed ExtJump (loop back edges!) to the bail-to-interp emit.  Default ON since 2026-06-11: the bail meant EVERY to:do:/whileTrue back edge dropped the whole activation to the interpreter (the '29.6ns/iter floor' mystery)
DEBUG_INT(PHARO_T1_RESUME_MIN_COMPILE, -1)  // send-resume bisect: advertise resume for send-bearing methods ONLY when compile-seq >= MIN (with FORCE_RESUME_FOR_SENDS semantics for that range); -1 = off
DEBUG_INT(PHARO_T1_RESUME_MAX_COMPILE, -1)  // ...and compile-seq < MAX; -1 = no cap.  Bisects WHICH method's resume wedges startup (the only-idle family)
DEBUG_INT(PHARO_T1_RESUME_MIN2_COMPILE, -1) // second bisect window (pair-interaction hunts)
DEBUG_BOOL(PHARO_BLOCK_CREATE_TRACE)        // dump every createFullBlockWithLiteral: method/fp/sp/copied-values/outerContext (capped 4000) — the cascade-#3 divergence finder
DEBUG_BOOL(PHARO_BLOCK_SAVE_PROBE)          // dump frame-slot contents (tb[0/-1/-2], sp[-1]) at every block-save materialize — locate the FullBlockClosure for the closureless-block-frame cannotReturn bug
DEBUG_BOOL(PHARO_SP_DEPTH_TRAP)             // __builtin_debugtrap() at the first sp-depth mismatch — lldb lands on the inconsistent frame
DEBUG_INT(PHARO_T1_RESUME_MAX2_COMPILE, -1)
DEBUG_BOOL(PHARO_T1_CT_SPLOG)                // x86 leak hunt: log EVERY copyTo: spDepthCheck exit (sp/tempBase/tempCount/depth/expected/delta/j2jDepth), not just mismatches, to watch the per-recursion-level drift and pin the leaking transition.
DEBUG_BOOL(PHARO_T1_NO_CALLER_RESUME)        // x86 WIP / diagnostic: disable JIT caller-resume (tryJITResumeInCaller). The x86 tier-1 caller-resume re-entry has a pre-existing ~1-word-per-send operand-stack leak (frame-state-residency protocol bit-rotted vs arm64 V2); with this set, after a send the caller continues in the INTERPRETER (correct, JIT runs only up to the first send) instead of re-entering JIT with a drifting sp. Lets x86 run correct while the resume sp-management is fixed. No effect unless set.
DEBUG_BOOL(PHARO_T1_NO_MOD_METHODS)         // bisect: reject \\/// methods from JIT (prescan) so they run fully interpreted, to test the mid-method modulo bail
DEBUG_BOOL(PHARO_T1_EXTBAIL_SEND)           // bisect: x86 extended-bytecode bail uses EXIT_SEND instead of EXIT_ARITH_OVERFLOW
DEBUG_BOOL(PHARO_T1_X86_EMIT_EXTSEND)       // x86 emit-polish (opt-in, default-OFF): emit a NAKED ExtSend (0xEA, no ExtendA/B prefix) as a real in-JIT cached send (own IC slot) instead of a mid-method bail-to-interp.  The FULL send body emits, including the cross-method inline-J2J fast path (resume point shifted to globalIdx+2 for the 2-byte op via j2jResumeBcOff), so the 8.7x cfibx cross-method win extends to 3+arg / high-literal-index sends.  Gates the WHOLE feature; OFF = byte-identical default JIT (isX86SendSite reduces to isPhase4SendOp; j2jResumeBcOff reduces to globalIdx+1).  x86-only emit (arm64 compiles out the predicate).  Prefixed (ExtendA/B+ExtSend) ExtSend still bails.  docs/x86-nested-j2j-design.md.
DEBUG_BOOL(PHARO_T1_TRACE_HANDLER)          // x86 diag: dump the sender chain walked by primitiveFindHandlerContext when it exhausts the safety limit (cycle) or repeatedly returns nil (handler lost) — pins the Context sender-chain corruption that hangs the exception-handler search
DEBUG_BOOL(PHARO_T1_X86_INLINE_J2J)         // NO-OP since the default-on flip (2026-06-14): x86 self-recursive inline-J2J is now DEFAULT-ON for the x86 tier-1 JIT (matches arm64). Kept for script compat. Opt out with PHARO_T1_X86_NO_INLINE_J2J.
DEBUG_BOOL(PHARO_T1_X86_NO_INLINE_J2J)      // opt-OUT of x86 self-recursive inline-J2J (default ON since 2026-06-14: full-suite validated 0 deterministic regressions, ~20% faster recursion; docs/sunit-3way-comparison.md). Use to get the plain chain-loop J2J baseline.
DEBUG_STR(PHARO_T1_X86_J2J_SEL)             // x86 inline-J2J WIP: per-method opt-in — fire the inline-J2J fast path ONLY for the method with this selector (e.g. a self-recursive bench method). Keeps the unfinished mechanism off startup/library code until bail-time materialization lands.
DEBUG_STR(PHARO_T1_X86_J2J_CLASS)           // BUG-2 bisect: when set, additionally restrict inline-J2J emission to methods whose defining class name matches this string (e.g. "Symbol"). Combine with PHARO_T1_X86_J2J_SEL to pin one (class,selector).
DEBUG_STR(PHARO_T1_X86_SENDS_SEL)           // BUG-2 bisect: comma-list of CALLER selectors that may use the SEND-BEARING admit (~AsmjitT1 3072). Full inline-J2J emission stays on for all methods, so NO caller-J2J/callee-no-J2J mismatch artifact — isolates the real send-bearing corruptor by caller. Empty => raw PHARO_T1_X86_XMETHOD_SENDS (all callers).
DEBUG_STR(PHARO_T1_X86_SENDS_CLASS)         // BUG-2 bisect: comma-list of CALLER class names allowed to use the send-bearing admit (combines AND with PHARO_T1_X86_SENDS_SEL). Same mismatch-free property.
DEBUG_INT(PHARO_T1_X86_SENDS_HMOD, 0)       // BUG-2 hash-bucket bisect: when >0, admit send-bearing only for callers whose FNV-1a hash of "Class>>selector" mod HMOD == PHARO_T1_X86_SENDS_HVAL. Binary-search the corruptor by doubling HMOD. Mismatch-free.
DEBUG_INT(PHARO_T1_X86_SENDS_HVAL, 0)       // BUG-2 hash-bucket bisect: the target bucket index for PHARO_T1_X86_SENDS_HMOD.
DEBUG_BOOL(PHARO_PRIM_SP_AUDIT)             // audit: flag any Success primitive that (without switching frames) leaves the operand stack at the wrong depth (delta != argCount*8) — finds operand-stack-shift root causes (a pop()/push() that forgot the receiver).
DEBUG_BOOL(PHARO_LITVAR_PROBE)              // BUG: dump the literal-variable binding + slots for the #current method (OSPlatform Current class-variable read) — the cold-startup garbage-read suspect.
DEBUG_BOOL(PHARO_T1_X86_XMETHOD)            // WIP / KNOWN-BROKEN (2026-06-15): cross-method (caller!=callee) inline-J2J on x86 (gates on callee->canSkipJ2JSave + !hasPrimPrologue + !hasHeapWrites). Knob-OFF is byte-identical self-rec-only (validated). Knob-ON CORRUPTS STARTUP (box: even `3+4` => nil via SnapshotOperation>>executeStoringError) — a subtle common-path emit bug remains, AND default-on additionally needs the open ExitArithOverflow-materialize bug fixed (canSkipJ2JSave isn't truly bail-free: arith-overflow escapes) + the bit-rotted x86 tier-1 base (per-send sp leak) hardened. Do NOT enable. See docs/x86-xmethod-j2j-design.md.
DEBUG_BOOL(PHARO_T1_X86_XMETHOD_COUNTERS)   // print cross-method inline-J2J fire count at exit (fold-independent firing proof for PHARO_T1_X86_XMETHOD validation).
DEBUG_BOOL(PHARO_T1_X86_XMETHOD_ALLARGS)    // DEBUG bisection knob (not a feature gate): admit nArgs>0 cross-method inline-J2J callees. Default OFF = nArgs==0 (unary) cross-method only. ORIGINALLY hypothesized that nArgs>0 was the lone corruptor, but box 2026-06-15 DISPROVED it: nArgs==0-only ALSO corrupts startup (adaptToNumber:andSend: DNU + String class>>new:streamContents: dispatching #new: on Symbol class). Cross-method inline-J2J swaps CLASS receivers across the startup library regardless of nArgs -> NOT a safe subset; PHARO_T1_X86_XMETHOD stays opt-in/known-broken. ALLARGS=1 surfaces the nArgs>0 ShouldNotImplement variant for debugging. arm64 compiles out (x86-only static). docs/x86-xmethod-j2j-design.md.
DEBUG_BOOL(PHARO_T1_X86_NO_XMETHOD)         // opt-OUT of x86 cross-method inline-J2J (default-ON since 2026-06-16: the stale-literalsCache swap is fixed; cfibx 1616->185ms 8.7x, full startup clean + battery==golden + v2bench exact). Set for self-rec-only inline-J2J (the pre-fix baseline). PHARO_T1_X86_XMETHOD is now a no-op.
DEBUG_BOOL(PHARO_T1_X86_NO_XMETHOD_ALLARGS) // opt-OUT of nArgs>0 cross-method inline-J2J (default-ON since 2026-06-16). Set to restrict cross-method to nArgs==0 (unary) callees. PHARO_T1_X86_XMETHOD_ALLARGS is now a no-op.
DEBUG_BOOL(PHARO_T1_X86_J2J_DBG)            // diagnostic (x86): trace the J2J save-stack across the cross-method admit-PUSH, return-prelude-POP, and resumeAfterCall (helper jit_rt_j2j_dbg, JITRuntime.cpp). Prints saves=(cursor-entry)/40 + depth + sp/recv, rate-limited 200. For the Increment 2 (PHARO_T1_X86_XMETHOD_SENDS) nested-cross-method corruption: a push/pop imbalance (the runaway-loop signature) shows as a drifting saves count.
DEBUG_BOOL(PHARO_T1_X86_XMETHOD_SENDS)      // Increment 2 (opt-IN): admit SEND-BEARING cross-method inline-J2J callees (numICEntries>0, up to PHARO_T1_XMETHOD_MAX_IC), replacing the canSkipJ2JSave (numIC==0 leaf-only) gate. INVESTIGATION 2026-06-17 (box-bisected) split the corruption into TWO defects: (1) the gate dropped canSkipJ2JSave's !x86HasMidBail term, so it admitted callees that bail mid-body (non-tail arith EXIT_ARITH_OVERFLOW / unported ExtSend) leaving the caller's save un-popped — reproduced even at MAX_IC=0. FIXED by storing x86HasMidBail in JITMethod + excluding it in the gate; MAX_IC=0 (== leaf-only) now computes cfibt28=1346267 correctly. (2) MAX_IC>=1 (genuine send-bearing) STILL corrupts (even `3+4` at startup runs away ~1B steps): the chain-loop inline-activate (Interpreter.cpp ~26941) resets the in-memory j2jSaveCursor to base before JIT_CALL, orphaning an inline-J2J'd send-bearing CALLER's pending save (depth>0). Safe for leaf callers (depth==0) and arm64 (x23-resident cursor), broken on x86. A naive pin-here fix changed the runaway signature but did not converge — the nested EMIT push/pop is also involved. STILL OPEN, multi-session: port arm64's saveless/blr entryDepth-pin+restore (AsmjitT1.cpp ~6776-7040) to the x86 jmp-style. So SENDS=1 is now SAFE only at MAX_IC=0 (no perf gain); MAX_IC>=1 stays a reproducer. arm64 compiles out (x86-only emit). docs/x86-nested-j2j-design.md.
DEBUG_BOOL(PHARO_J2J_NEST_TRACE)            // shared (both arches): log every chain-loop inline-one-shot-J2J activation that runs with a PENDING caller J2J save (state.j2jDepth>0) — the send-bearing nested case. Prints caller sel / depth / cursor+entryCursor slice offsets / inner target sel, and (post-JIT_CALL) the callee exit reason + resulting depth. Diff arm64 (works, send-bearing default-on) vs x86 (PHARO_T1_X86_XMETHOD_SENDS, corrupts) to pin the BUG-2 divergence. Rate-limited.
DEBUG_BOOL(PHARO_FFI_TRACE)                 // log ffi_prep_cif failures (status/abi/type ptrs) to diagnose x86 FFI test errors

DEBUG_BOOL(PHARO_T1_XM_TRACE)               // full-startup diagnostic (x86): log each cross-method inline-J2J admit fire whose RECEIVER is a CLASS (the startup-corruption shape) — caller + callee selector + receiver class, rate-limited to 400. The last few before the ShouldNotImplement/cannot-have-variable-instances error name the corrupting caller/callee. Emitted in the admit before jmp r11; helper jit_rt_xm_fire_trace in JITRuntime.cpp.
DEBUG_BOOL(PHARO_T1_X86_XMETHOD_PROBE)      // capture-first-corruption probe (x86/V1, default-OFF): after each tryJITActivation, if state.j2jDepth != j2jEntryDepth (a leaked cross-method/inline-J2J save = the startup-corruption signature), print the leaked save's CALLER selector + state.method selector + code-zone churn delta and abort() — NAMES the first method whose cross-method inline-J2J leaks under PHARO_T1_X86_XMETHOD no-SEL startup. arm64 compiles out (#if !PHARO_J2J_SAVE_V2).
DEBUG_BOOL(PHARO_T1_FATAL_DISAGREE)          // anti-regression: abort() at the prescan/emit-disagree site instead of silently bailing the method to interp. Run the suite + benches with this on BOTH arches so any future arm64 prescan widening that the x86 (or arm64) emit doesn't follow trips immediately, instead of silently degrading to interpreter. Default off (production bails quietly).
DEBUG_BOOL(PHARO_X86_JIT)                    // DEPRECATED no-op (kept for back-compat): the x86_64 tier-1 JIT is now DEFAULT-ON (flipped 2026-06-17), so this explicit opt-in is redundant.  Use PHARO_NO_X86_JIT=1 to opt OUT.
DEBUG_BOOL(PHARO_NO_X86_JIT)                 // opt-OUT escape hatch: disable the x86_64 tier-1 asmjit JIT (run interpreted).  The x86 JIT went DEFAULT-ON 2026-06-17 after: (a) 565-class headless SUnit at parity with the shipping arm64 JIT AND the interpreter (only nondeterministic weak-ref/GC-timing tests differ — same on arm64-JIT); (b) Morphic rendering (Morph/BorderedMorph fills+borders, gradient strips, StringMorph+TextMorph FreeType text, BitBlt compositing, PNG encode) PIXEL-IDENTICAL under JIT vs interp via imageForm.  Residual unverified surface: the live Catalyst Metal app render-loop (JIT-agnostic C++ bridge; needs actual x86 Catalyst) — this hatch + PHARO_NO_JIT mitigate.  No effect on arm64 (#if x86 only; arm64 JIT always on).
DEBUG_BOOL(PHARO_T1_AO_MAT_J2J)              // default-OFF, x86/V1-scoped (#if !PHARO_J2J_SAVE_V2): case ExitArithOverflow (Interpreter.cpp:26322) materializes pending J2J saves (j2jDepth>0) via the in-scope materializeJ2J lambda before `return false`. Addresses a REAL but SECONDARY bug (an inline-J2J callee that arith-OVERFLOWS leaks its caller frame). NOT the cure for the PHARO_T1_X86_XMETHOD startup corruption: workflow wf_8b29daf3 proved (3+4)=7 is in SmallInteger range so it NEVER reaches ExitArithOverflow — the (3+4)->nil corruption is a cross-method bail-leak at a DIFFERENT exit (ExitSend DNU/non-standard, ExitMustBool, ExitBlockCreate), independent of this AO path. SUPERSEDED by PHARO_T1_X86_BAIL_MAT_J2J (the umbrella). arm64-SAFE: compiles out on V2. docs/x86-xmethod-j2j-design.md.
DEBUG_BOOL(PHARO_T1_X86_BAIL_MAT_J2J)        // default-OFF, x86/V1 umbrella (#if !PHARO_J2J_SAVE_V2): the ROOT-CAUSE cure for the PHARO_T1_X86_XMETHOD startup corruption. On V1 the caller frame of a cross-method inline-J2J send lives ONLY in the unpopped J2J save; the callee's return prelude pops it on a CLEAN return, but a MID-METHOD bail (any chain-loop `return false` to interp: ExitSend DNU/non-standard/no-sel, ExitSendCached, ExitMustBool, ExitJ2JCall-invalid, ExitArithOverflow, ExitPrimFail/Deopt, ExitBlockCreate/ArrayCreate/Yield at the final jit_loop_exit switch) leaves it leaked -> the interp-resumed callee returns into a corrupt frame ((3+4)->nil via the startup send chain). cfibx proves the NORMAL emit is correct (its callees return cleanly), so per-class canSkipJ2JSave gating cannot converge (the leak is exit-driven, not class-driven). Fix: a `bailMatJ2J()` lambda materializes pending saves (reconstructs caller SavedFrame(s), resets depth/cursor) at EVERY such bail site; idempotent no-op when j2jDepth==0. Subsumes PHARO_T1_AO_MAT_J2J. arm64: bailMatJ2J is an empty lambda (V2 save machinery handles bails), so call sites compile to nothing.
DEBUG_INT(PHARO_HEAP_SCAN_EVERY, 0)         // wild-write detector: every N×1024-bytecode checkpoints, walk the heap (checkHeapIntegrity) and report the first CLEAN->CORRUPT transition (victim + step window + recent bytecodes). 0=off.
DEBUG_INT(PHARO_HEAP_SCAN_FROM, 0)          // wild-write detector: only start scanning once g_stepNum >= N×1024 (skip the clean early startup; narrow toward the corruption window).
DEBUG_BOOL(PHARO_HEAP_SCAN_STOP)            // wild-write detector: stop the VM at the first detected corruption.
DEBUG_BOOL(PHARO_DNU_DUMP_COLL)             // cold-startup diag: at each of the first DNUs, dump the CURRENT method's receiver as a collection (class/format/slotCount + all slots) so a legal-but-wrong element (e.g. SmallInteger 1 = 0x9 where a class belongs) can be located, and undersized-read vs stored-garbage distinguished. Env-gated → toggling does NOT relocate the victim on a fixed binary.
DEBUG_STR(PHARO_T1_NOJIT_SEL)               // cold-startup bisect: comma-separated EXACT selector list; compileViaAsmjit bails (stays interpreted) for any method whose selector matches. Lets us find WHICH method's T1 compilation triggers the cold-boot corruption by selectively NOT compiling it (env-gated on a fixed binary → no relocation). Pairs with PHARO_T1_NOJIT_CLASS.
DEBUG_STR(PHARO_T1_NOJIT_CLASS)             // cold-startup bisect: like PHARO_T1_NOJIT_SEL but matches the method's DEFINING CLASS name (exact). Narrows a NOJIT_SEL hit to one class when the selector is overloaded.
DEBUG_BOOL(PHARO_T1_SEND_RECV_AUDIT)        // cascade-origin detector: at the chain-loop send activation, when state.icDataPtr is set, verify classIndexOf(calleeRecv=sp[-(nArgs+1)]) == icDataPtr[0] (the IC's dispatched class). The FIRST mismatch names where the JIT operand stack first shifted (the cold-boot cascade origin). Reports caller method + selector + nArgs + sp + expected/actual classIndex, then stops on PHARO_T1_SEND_RECV_AUDIT_STOP.
DEBUG_BOOL(PHARO_T1_SEND_RECV_AUDIT_STOP)   // stop the VM (running_=false) at the first SEND_RECV_AUDIT mismatch.
DEBUG_BOOL(PHARO_T1_RET_FP_AUDIT)           // cascade-origin detector (JIT->interp return): at every ExitReturn, before popFrame, assert framePointer_ == state.tempBase-1 (the returning JIT method's fp). A divergence means the C++ framePointer_ global drifted from the JIT method's actual fp, so popFrame's `sp=fp` collapses to the WRONG slot and the result lands shifted on the caller stack. Reports the FIRST divergence (returning method + caller + fp/tempBase + delta).
DEBUG_BOOL(PHARO_T1_SP_TRACE)               // per-bytecode operand-sp trace (arm64): emit a non-perturbing call at the TOP of every bytecode of the method whose selector == PHARO_T1_SP_TRACE_SEL, logging (method oop, bcOffset, sp-depth, TOS/NOS, tempBase[0..2]). Reads the LIVE sp via emitSyncSpToState (sp is register-resident in x25). The software equivalent of lldb instruction-stepping: diff the logged TOS at each bytecode against the expected operand picture to find the bytecode whose sp/operand delta is wrong (the cold-boot shift). Scope by selector so only the victim grows -> non-relocating observation.
DEBUG_STR(PHARO_T1_SP_TRACE_SEL)            // selector to scope PHARO_T1_SP_TRACE to (e.g. "do:"). Required; unset traces nothing.
DEBUG_INT(PHARO_T1_SP_TRAP_K, 0)            // with PHARO_T1_SP_TRACE: __builtin_debugtrap() at the Kth bc14 (at: send) of the traced method (single trap, in-process — no repeated lldb breakpoints, so the IC-warming-timing-sensitive over-pop is NOT perturbed). Pass 1 (K=0) prints [OVERPOP] with the over-popping bc14 index; pass 2 (K=that index) traps there for single-stepping. 0=off.
DEBUG_BOOL(PHARO_T1_PRIM_OVERPOP)           // cold-boot over-pop detector: at every in-place chain primitive send (ExitSendCached 22053, J2J-cached 22388, ExitSend fallback 27874), check the operand-sp delta across executePrimitive == -argCount_ (primitiveSuccess net). Logs the FIRST few mismatches (over/under-pop) with caller/target/prim/argCount before&after/sp depth. Path-generic (not do:-scoped), so it catches whatever method's cold at: over-pops despite rebuild relocation.
DEBUG_BOOL(PHARO_T1_PRIM_OVERPOP_STOP)      // stop the VM at the first PRIM_OVERPOP mismatch.
DEBUG_BOOL(PHARO_DUMP_DISPLAY)              // test_load_image: dump the TestDisplaySurface (gDisplaySurface) to /tmp/vm-display-{20,60,150}.ppm at those render-present counts — proves whether the VM actually draws the morphic World into gDisplaySurface (which screencapture can't grab from the app's Metal layer). Headless GUI-render verification.
DEBUG_BOOL(PHARO_FORCE_DISPLAY)             // test_load_image: create gTestSurface/gDisplaySurface even in headless/eval mode, so a forced render (OSWorldRenderer doActivate) can present into it and be captured via PHARO_DUMP_DISPLAY. Proved the Windows GUI render path headlessly (the morphic World draws). See docs/deferred.md GUI section.
DEBUG_BOOL(PHARO_GUI_WINDOW)                // test_load_image (Windows): back gDisplaySurface with a real on-screen Win32 HWND (Win32DisplaySurface, GDI StretchDIBits present) instead of the in-memory TestDisplaySurface — so the rendered morphic World is actually shown in a window. Implies the display surface is created. Input events not yet wired.
DEBUG_BOOL(PHARO_WIN_EVENT_TRACE)           // Windows GUI input debug: trace real input events at the producer (Win32 wndProc push into gEventQueue) and the consumer (stub_SDL_PollEvent delivery to the image), so a click's full path is observable. Button/key events log individually; mouse moves are counted (logged every 64th).
DEBUG_BOOL(PHARO_BITBLT_TRACE)              // log each primitiveCopyBits FAILURE (rule, src/dst depth, reason) — the image's copyBits fallback masks the real cause behind "Bad BitBlt arg (Fraction?)" so this is how you find which rule/depth combination the primitive actually lacks. First 50 + every 500th.
DEBUG_BOOL(PHARO_NLR_TRACE)
DEBUG_BOOL(PHARO_CORPSE_PUSH_TRAP)          // cascade hunt: trap push() of a young oop whose header is already nil-scrubbed (classIdx 0) — names the writer path that copies a scavenge-dead eden oop back onto the operand stack. First 5, then silent.                 // trace activateBlock home-frame resolution (homeMethod/altHome/outerContext/homeFrame) and block-NLR home search failures — for BlockCannotReturn diagnosis.
DEBUG_BOOL(PHARO_NO_GC_TEMPSYNC)            // bisect: skip prepareForGC's frame->materialized-context temp sync entirely. If the simulation-family corpse vanishes, the sync is copying from reused/dead savedFP slots (the f96cb69b residual).
DEBUG_BOOL(PHARO_EDEN_DANGLE_SCAN)          // cascade hunt: after each scavenge, scan operand stack / savedFrames / j2j pool / JITState for pointers into the (now-empty) eden window and dump provenance. Any hit = a root location the scavenge failed to retarget.
DEBUG_BOOL(PHARO_EDEN_POISON)               // cascade hunt: fill retired eden with the 0x5CAFEDxx sentinel at every scavenge reset. A later corpse whose bits carry the sentinel proves double-indirection through recycled eden storage; a valid eden ADDRESS corpse proves a stale pointer held outside eden.
DEBUG_BOOL(PHARO_NATIVE_NLR_UNWIND)         // bisect hatch: restore the pre-2026-07-03 NATIVE context-NLR unwind (nlrTargetCtx_/nlrEnsureCtx_ pending-NLR + executeFromContext ensure-hopping) instead of the stock aboutToReturn:through: image protocol. The native mechanism keeps mid-unwind state in C++ where the debugger cannot see or resume it (StepOverTest deep-handler failure); this knob exists only to bisect regressions against the old behavior.
DEBUG_STR(PHARO_PIN_DIAG)                   // forensics (prim 130): set to a short ASCII string (e.g. PHARO_PIN_DIAG=123). After each fullGC, finds every surviving ByteString with exactly that content and prints (a) which forEachRoot slot references it, attributed against stack/savedFrames/j2jPool/bvSaves/jitState landmark ranges, and (b) every heap object whose pointer slots reference it. Found the stale-j2j-slice weak-reclaim pin (2026-07-03).
DEBUG_BOOL(PHARO_GC_PURGE_LOG)              // log per-fullGC weak-cache purge counts ([GC-PURGE] methodCache/jitMethods/icEntries/countKeys/failedKeys/tier2Keys) — the purgeDeadCacheRoots counterpart of the StrongOnly root scope (dead-class unpinning, 2026-07-03).
DEBUG_BOOL(PHARO_TFFI_LAT_TRACE)            // TFFI worker-callout latency trace ([TFLAT] enq/sig/drain lines with a µs steady clock + semaphore index) — measures where the ~26-50ms/callout UsingWorker round-trip goes (enqueue -> worker signal -> pending-signal drain). First ~60 events.
DEBUG_BOOL(PHARO_DNS_TRACE)                 // [DNS-TRACE] hostname + wall-ms + final status for every resolver lookup (detached getaddrinfo thread) — the Zn socket-timing hunt's discriminator for slow/mDNS lookups poisoning the serialized NetNameResolver.
DEBUG_BOOL(PHARO_PRIM111_RING)              // cascade-hunt forensic: record every prim-111 (class) call in g_prim111Ring (256-deep: receiver/result/method/seq) for the [DNU] RING-T1 / PRIM111-RCVR-MATCH cascade forensics. Root cause found 2026-07-05 (e40cd65b); off by default — 4 stores per call in a hot primitive.

// --- 2026-07-05 mass conversion from DebugSettings (envPresent ratchet lowering) ---
DEBUG_BOOL(JIT_DUMP_BC)
DEBUG_BOOL(PHARO_A1_TRACE)
DEBUG_BOOL(PHARO_ASMJIT_T1_BCTOCODE_ZERO)
DEBUG_BOOL(PHARO_ASMJIT_T1_FORCE_RESUME_FOR_SENDS)
DEBUG_BOOL(PHARO_ASMJIT_T1_HARDCODE_STUB)
DEBUG_BOOL(PHARO_ASMJIT_T1_LOG)
DEBUG_BOOL(PHARO_ASMJIT_T1_NO_BCTOCODE)
DEBUG_BOOL(PHARO_ASMJIT_T1_NO_NUMBC)
DEBUG_BOOL(PHARO_ASMJIT_T1_NO_SENDS_BISECT)
DEBUG_BOOL(PHARO_ASMJIT_T1_NO_TRIM)
DEBUG_BOOL(PHARO_ASMJIT_T1_STUB_ONLY)
DEBUG_BOOL(PHARO_ASMJIT_T1_TRACE_COND)
DEBUG_BOOL(PHARO_B5_TRACE)
DEBUG_BOOL(PHARO_BASICAT_TRACE)
DEBUG_BOOL(PHARO_BC5_DUMP)
DEBUG_BOOL(PHARO_BENCH)
DEBUG_BOOL(PHARO_CALLBACK_DEBUG)
DEBUG_BOOL(PHARO_CTX_TEMPAT_DBG)
DEBUG_BOOL(PHARO_DETECT_ERRORS)
DEBUG_BOOL(PHARO_DRIFT_CHECK)
DEBUG_BOOL(PHARO_DUMP_INTERP_OFFSETS)
DEBUG_BOOL(PHARO_GC_LOG)
DEBUG_BOOL(PHARO_JIT_EXCLUDE_EXC_INFRA)  // bisect: restore the 2026-04-16 hardcoded blacklist that refused to JIT the exception/NLR core (#signal, #signalerContext, #handleSignal:, #aboutToReturn:through:, #doesNotUnderstand:, ...).  The ~400-method native-stack overflow it worked around is gone (J2J is depth-capped, T2 sends run the chain loop), and keeping it cost ~20-25% on exception-heavy code.
DEBUG_BOOL(PHARO_GC_AUTO_SKIP_EPH)     // opt-out: restore the pre-2026-08-13 behaviour where an auto-triggered (allocation-pressure) fullGC skips ephemeron firing + weak nilling.  That divergence meant weak refs never cleared and finalization never ran unless the image called `Smalltalk garbageCollect` explicitly; stock Cog's major GC does both.  Bisect knob only.
DEBUG_BOOL(PHARO_IC_HISTOGRAM)
DEBUG_BOOL(PHARO_IC_HIT_DBG)
DEBUG_BOOL(PHARO_IC_PATCH_DEBUG)
DEBUG_BOOL(PHARO_IMPROPER_STORE_TRACE)
DEBUG_BOOL(PHARO_INLINE_ACTIVATE_NO_BAIL_MID)
DEBUG_BOOL(PHARO_INLINE_ACTIVATE_STUBS)
DEBUG_BOOL(PHARO_INLINE_PRIM_DEBUG)
DEBUG_BOOL(PHARO_JIT_FAIL_REASONS)
DEBUG_BOOL(PHARO_JIT_MEGA_SCAN)
DEBUG_BOOL(PHARO_JIT_NO_BLOCKS)
DEBUG_BOOL(PHARO_JIT_RETVAL_DBG)
DEBUG_BOOL(PHARO_JIT_TRACE_RECOMPILE)
DEBUG_BOOL(PHARO_JIT_VALIDATE_ENTRY)
DEBUG_BOOL(PHARO_MNU_ALLOC_DBG)
DEBUG_BOOL(PHARO_NO_BLOCK1_SPEC)
DEBUG_BOOL(PHARO_NO_BLOCK_BIT)
DEBUG_BOOL(PHARO_NO_BLOCK_VALUE_SPEC)
DEBUG_BOOL(PHARO_NO_CHAIN)
DEBUG_BOOL(PHARO_NO_CULL_IC_FILL)
DEBUG_BOOL(PHARO_NO_CULL_MEGA)
DEBUG_BOOL(PHARO_NO_EAGER_BLOCK_VALUE)
DEBUG_BOOL(PHARO_NO_EAGER_COMPILE)
DEBUG_BOOL(PHARO_NO_FWD_COLLAPSE)
DEBUG_BOOL(PHARO_NO_GETTER_BIT)
DEBUG_BOOL(PHARO_NO_HEARTBEAT)
DEBUG_BOOL(PHARO_NO_HOT_LOOP_THRESHOLD)
DEBUG_BOOL(PHARO_NO_IC_FILL)
DEBUG_BOOL(PHARO_NO_J2J)
DEBUG_BOOL(PHARO_NO_J2J_CALLEE_BUMP)
DEBUG_BOOL(PHARO_NO_J2J_INLINE_BUMP)
DEBUG_BOOL(PHARO_NO_JIT_RESUME_AFTER_RETURN)
DEBUG_BOOL(PHARO_NO_LATE_SPEC_RECOMPILE)
DEBUG_BOOL(PHARO_NO_MEGAHIT_IC_FILL)
DEBUG_BOOL(PHARO_NO_MULTISLOT_GETTER)
DEBUG_BOOL(PHARO_NO_OSR)
DEBUG_BOOL(PHARO_NO_OSR_RECOMPILE)
DEBUG_BOOL(PHARO_NO_QUEUE_COMPILE)
DEBUG_BOOL(PHARO_NO_RESUME)
DEBUG_BOOL(PHARO_NO_RETLIT)
DEBUG_BOOL(PHARO_NO_SISTA_COLLECT_RESUME)
DEBUG_BOOL(PHARO_NO_SISTA_DOACCUM_RESUME)
DEBUG_BOOL(PHARO_NO_SISTA_INJECT_RESUME)
DEBUG_BOOL(PHARO_NO_SISTA_INLINE_ARITHIVAR)
DEBUG_BOOL(PHARO_NO_SISTA_INLINE_IDENTITY_EQ)
DEBUG_BOOL(PHARO_NO_SISTA_INLINE_YOURSELF)
DEBUG_BOOL(PHARO_NO_SISTA_IV_DO_ACCUM)
DEBUG_BOOL(PHARO_NO_SISTA_PER_BC)
DEBUG_BOOL(PHARO_NO_SISTA_PRIM_AT_PUT)
DEBUG_BOOL(PHARO_NO_SISTA_WHILETRUE)
DEBUG_BOOL(PHARO_P63_TRACE)
DEBUG_BOOL(PHARO_PRIMAT_DEBUG)
DEBUG_BOOL(PHARO_PRIMAT_OOB)
DEBUG_BOOL(PHARO_PRIMSIZE_DEBUG)
DEBUG_BOOL(PHARO_PRIMSIZE_STENCIL_DBG)
DEBUG_BOOL(PHARO_PRIM_PROFILE)
DEBUG_BOOL(PHARO_PROC_DUMP)
DEBUG_BOOL(PHARO_PROFILE)
DEBUG_BOOL(PHARO_REFLECT_PROFILE)
DEBUG_BOOL(PHARO_RESUME_J2J)
DEBUG_BOOL(PHARO_RESUME_STATE_DEBUG)
DEBUG_BOOL(PHARO_RJ2J_VALIDATE)
DEBUG_BOOL(PHARO_SCAV_BIT_AUDIT)
DEBUG_BOOL(PHARO_SDL_TRACE)
DEBUG_BOOL(PHARO_SEM_SIGNAL_TRACE)
DEBUG_BOOL(PHARO_SENDER_TRIPWIRE)
DEBUG_BOOL(PHARO_SISTA_AFTER_T1)
DEBUG_BOOL(PHARO_SISTA_ALLOC_ARRAY_TRACE)
DEBUG_BOOL(PHARO_SISTA_ALLOW_ARRAYDO_HELPER)
DEBUG_BOOL(PHARO_SISTA_ALLOW_SENDS)
DEBUG_BOOL(PHARO_SISTA_ASMJIT_LOG)
DEBUG_BOOL(PHARO_SISTA_AT_PEEPHOLE)
DEBUG_BOOL(PHARO_SISTA_BAIL_LOG)
DEBUG_BOOL(PHARO_SISTA_BJ_TRACE)
DEBUG_BOOL(PHARO_SISTA_BLOCK_BAIL)
DEBUG_BOOL(PHARO_SISTA_BLOCK_HELPER_TRACE)
DEBUG_BOOL(PHARO_SISTA_COLLECT_RESUME_FORCE_BAIL)
DEBUG_BOOL(PHARO_SISTA_COMPILE)
DEBUG_BOOL(PHARO_SISTA_COMPILE_BAIL_ONLY)
DEBUG_BOOL(PHARO_SISTA_DISPATCH_MULTIBLOCK)
DEBUG_BOOL(PHARO_SISTA_DOACCUM_FORCE_BAIL)
DEBUG_BOOL(PHARO_SISTA_DO_DETECT)
DEBUG_BOOL(PHARO_SISTA_HELPER_FORCE_BAIL)
DEBUG_BOOL(PHARO_SISTA_INJECT_RESUME_FORCE_BAIL)
DEBUG_BOOL(PHARO_SISTA_INLINE_DBG)
DEBUG_BOOL(PHARO_SISTA_INLINE_DUMP)
DEBUG_BOOL(PHARO_SISTA_INLINE_SELF)
DEBUG_BOOL(PHARO_SISTA_INLINE_STATS)
DEBUG_BOOL(PHARO_SISTA_INVARIANT_CHECK)
DEBUG_BOOL(PHARO_SISTA_NO_BAIL)
DEBUG_BOOL(PHARO_SISTA_NO_FULLBLOCK)
DEBUG_BOOL(PHARO_SISTA_NO_INLINE_ARITH)
DEBUG_BOOL(PHARO_SISTA_NO_INLINE_CONST)
DEBUG_BOOL(PHARO_SISTA_NO_INLINE_SETTERS)
DEBUG_BOOL(PHARO_SISTA_NO_LOWER)
DEBUG_BOOL(PHARO_SISTA_NO_LOWER_ADD)
DEBUG_BOOL(PHARO_SISTA_NO_LOWER_ARITH)
DEBUG_BOOL(PHARO_SISTA_NO_LOWER_ARITH_CMP)
DEBUG_BOOL(PHARO_SISTA_NO_LOWER_ARITH_MATH)
DEBUG_BOOL(PHARO_SISTA_NO_LOWER_BODY)
DEBUG_BOOL(PHARO_SISTA_NO_LOWER_BRANCH)
DEBUG_BOOL(PHARO_SISTA_NO_LOWER_FUSE)
DEBUG_BOOL(PHARO_SISTA_NO_LOWER_SENDS)
DEBUG_BOOL(PHARO_SISTA_NO_REMOTE_TEMP)
DEBUG_BOOL(PHARO_SISTA_NO_REMOTE_TEMP_READ)
DEBUG_BOOL(PHARO_SISTA_NO_REMOTE_TEMP_WRITE)
DEBUG_BOOL(PHARO_SISTA_NO_SHORTCUT_EVEN_ODD)
DEBUG_BOOL(PHARO_SISTA_NO_STORES)
DEBUG_BOOL(PHARO_SISTA_PER_BC_BAIL_TRACE)
DEBUG_BOOL(PHARO_SISTA_PER_BC_DISPATCH_TRACE)
DEBUG_BOOL(PHARO_SISTA_PER_BC_NO_DISPATCH)
DEBUG_BOOL(PHARO_SISTA_PER_BC_TRACE)
DEBUG_BOOL(PHARO_SISTA_REKEY_AFTER_GC)
DEBUG_BOOL(PHARO_SISTA_SIZE_PEEPHOLE)
DEBUG_BOOL(PHARO_SISTA_STACK_WATCH)
DEBUG_BOOL(PHARO_SISTA_TEMP_WATCH)
DEBUG_BOOL(PHARO_SISTA_TRACE_ADD)
DEBUG_BOOL(PHARO_SISTA_UNSAFE_ARITH)
DEBUG_BOOL(PHARO_SISTA_VERBOSE)
DEBUG_BOOL(PHARO_SISTA_X86_TRACE_OK)
DEBUG_BOOL(PHARO_SLOT_TRIPWIRE)
DEBUG_BOOL(PHARO_SORTSTR_WATCH)
DEBUG_BOOL(PHARO_SP_CORRUPT_TRAP)
DEBUG_BOOL(PHARO_T1_ACCEPT_EXTSUPERSEND)
DEBUG_BOOL(PHARO_T1_ALLOW_NESTED_JIT_BAIL)
DEBUG_BOOL(PHARO_T1_BAIL_GATE_TRACE)
DEBUG_BOOL(PHARO_T1_BLOCKS_TRACE)
DEBUG_BOOL(PHARO_T1_EAGER_BLOCK_COMPILE)
DEBUG_BOOL(PHARO_T1_GETTER_CLASSIFY_LOG)
DEBUG_BOOL(PHARO_T1_HIT_AS_MISS)
DEBUG_BOOL(PHARO_T1_IC_HIT_VERIFY)
DEBUG_BOOL(PHARO_T1_IC_POLY_WALK)
DEBUG_BOOL(PHARO_T1_INLINE_BLOCK_VALUE)
DEBUG_BOOL(PHARO_T1_INLINE_BLOCK_VALUE_NONLEAF)
DEBUG_BOOL(PHARO_T1_INLINE_J2J)
DEBUG_BOOL(PHARO_T1_INLINE_J2J_DUMP_BC)
DEBUG_BOOL(PHARO_T1_INLINE_J2J_TRACE_UNSUPP)
DEBUG_BOOL(PHARO_T1_INLINE_J2J_XMETHOD_LIVE)
DEBUG_BOOL(PHARO_T1_INLINE_J2J_XMETHOD_LOG)
DEBUG_BOOL(PHARO_T1_INLINE_PRIM_COUNTERS)
DEBUG_BOOL(PHARO_T1_INLINE_SISTA_CALL)
DEBUG_BOOL(PHARO_T1_J2J_RECEIVER_SYNC)
DEBUG_BOOL(PHARO_T1_J2J_SPLIT_POOL)
DEBUG_BOOL(PHARO_T1_NO_BLOCKS)
DEBUG_BOOL(PHARO_T1_NO_J2J_TRAMP)
DEBUG_BOOL(PHARO_T1_PROBE_ALWAYS_MISS)
DEBUG_BOOL(PHARO_T1_PURE_J2J_GATE)
DEBUG_BOOL(PHARO_T1_RESUME_SENDS_NO_CONDJUMP)
DEBUG_BOOL(PHARO_T1_RESUME_TOS_LOG)
DEBUG_BOOL(PHARO_T1_SELF_REC_SPLICE)
DEBUG_BOOL(PHARO_T1_SELF_REC_SPLICE_HINTLESS)
DEBUG_BOOL(PHARO_T1_SETTER_BARRIER)
DEBUG_BOOL(PHARO_T1_SISTA_DISPATCH_ALLOW)
DEBUG_BOOL(PHARO_T1_TRACE_COMPILE)
DEBUG_BOOL(PHARO_T1_TRACE_EMIT)
DEBUG_BOOL(PHARO_T1_TRACE_HIT)
DEBUG_BOOL(PHARO_T1_WARM_J2J_GATE)
DEBUG_BOOL(PHARO_T1_XMETHOD_LOG)
DEBUG_BOOL(PHARO_T2_A1)
DEBUG_BOOL(PHARO_T2_FORCE_MISS)
DEBUG_BOOL(PHARO_T2_MBC_IC)
DEBUG_BOOL(PHARO_T2_MBC_SENDS)
DEBUG_BOOL(PHARO_T2_REPLACE)
DEBUG_BOOL(PHARO_T2_VERBOSE)
DEBUG_BOOL(PHARO_T2_X86_LOG)
DEBUG_BOOL(PHARO_T2_X86_TRACE)
DEBUG_BOOL(PHARO_T2_ZEROARG_IC)
DEBUG_BOOL(PHARO_TERM_BT)
DEBUG_BOOL(PHARO_TIME_GC_PHASES)
DEBUG_BOOL(PHARO_TRACE_ACTIVATE_BLOCK)
DEBUG_BOOL(PHARO_TRACE_BC_7A)
DEBUG_BOOL(PHARO_TRACE_CULL_BAIL)
DEBUG_BOOL(PHARO_TRACE_CULL_ENTRY)
DEBUG_BOOL(PHARO_TRACE_CULL_RETURN)
DEBUG_BOOL(PHARO_TRACE_EXCEPTION_SEL)
DEBUG_BOOL(PHARO_TRACE_EXEC_PRIM)
DEBUG_BOOL(PHARO_TRACE_EXIT)
DEBUG_BOOL(PHARO_PIN_STATS)   // count primitivePin/Unpin calls, report at exit
DEBUG_BOOL(PHARO_FREECHUNK_REFS)  // on a classIndex-0 DNU, scan the heap and name every object holding a pointer to the free chunk
DEBUG_BOOL(PHARO_PIN_RELOCATE)    // at pin time, move an old-space object DOWN into a reclaimed gap and forward the original, Spur-style, so pins stop stranding the space below them. Requires PHARO_OLDSPACE_FREELIST (that is where the low chunks come from). See docs/gc-oldspace-fragmentation-2026-08-22.md
DEBUG_BOOL(PHARO_TRACE_OP_VALUE1)
DEBUG_BOOL(PHARO_TRACE_PER_BC_SP)
DEBUG_BOOL(PHARO_TRACE_PRIM207)
DEBUG_BOOL(PHARO_TRACE_RECOMPILE_FLOW)
DEBUG_BOOL(PHARO_TRACE_RESUME_SP)
DEBUG_BOOL(PHARO_TRACE_REWRITE_IC)
DEBUG_BOOL(PHARO_TRACE_SHOULDNOTIMPL)
DEBUG_BOOL(PHARO_TRACE_SISTA_DISPATCH)
DEBUG_BOOL(PHARO_TRACE_SISTA_PERBC)
DEBUG_BOOL(PHARO_TRACE_SP_CORRUPT)
DEBUG_BOOL(PHARO_TRACE_STACK_ORIGIN)
DEBUG_BOOL(PHARO_TRACE_TOTAL_STEPS)
DEBUG_BOOL(PHARO_TRACE_VALUE_ACT)
DEBUG_BOOL(PHARO_TRAP_BAD_DNU)
DEBUG_BOOL(PHARO_USE_ASMJIT_T1_TRACE)
DEBUG_BOOL(PHARO_XFER_TRACE)
DEBUG_BOOL(PHARO_YG_NO_SCAVENGE)
DEBUG_INT(PHARO_SCAV_SCAN_ABOVE_SP, 0)  // DIAGNOSTIC: also treat N operand-stack slots ABOVE stackPointer_ as GC roots (only those that pass isValidPointer). If a use-after-collect disappears with this set, the missed holder is a stack slot outside [stackBase_, stackPointer_) -- the defect #1 family. NOT a fix: it conservatively pins dead slots.

#undef DEBUG_BOOL
#undef DEBUG_INT
#undef DEBUG_STR
