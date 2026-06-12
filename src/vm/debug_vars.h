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
DEBUG_BOOL(PHARO_T1_NO_CAN_SKIP_J2J_SAVE)   // opt-OUT of the saveless leaf-call path (default ON since 2026-06-10, Eδ.2d complete: 200-class A/B per-test identical, 10x cfib 423 vs 447ms). Replaces the DebugSettings envPresent line.
DEBUG_BOOL(PHARO_T1_STORE_RING)             // store-provenance ring: every JIT receiver-ivar store (bytecode emits + inline-setter IC path) logs (recv, value, slot, callerJM) into a 64K ring; sendDoesNotUnderstand scans it for the corrupted oop and prints provenance — catches the corrupting WRITE for the J2J ivar-store corruption hunt
DEBUG_BOOL(PHARO_SHADOW_SLOTS)              // shadow-slot verify-on-read: mirror tracked pointer-slot writes (JIT store emits, setReceiverInstVar, storePointer) into a 1M-entry shadow table; receiver-ivar reads verify against it. A [SHADOW-MISMATCH] = the slot changed via an untracked path (missing writer instrumentation or a GC-mover bug). Visibility-independent — works on any layout.
DEBUG_BOOL(PHARO_SP_DEPTH_CHECK)            // sp-desync detector (BcDepthMap.cpp): verify state.sp against the static operand-stack depth for state.ip's bcOffset at every checkable JIT exit (Send/SendCached/MustBool/ArithOverflow/Block/ArrayCreate/Yield). A [SP-DEPTH] = the frame's sp drifted from the bytecode-mandated depth — the corruption class behind all root-caused J2J bugs (shadow suite exonerated ivar stores). Layout-independent.
DEBUG_BOOL(PHARO_VERIFY_GETTER)             // verify-on-fire for the T1 inline getter (emit-time gate): BLR jit_rt_verify_getter after each inline slot read; flags a classification slotIdx >= the live receiver's slotCount (poisoned extra word: J2J address bits / foreign-site classification). [VERIFY-GETTER] = caught the misfire with caller provenance.
DEBUG_BOOL(PHARO_T1_NO_GETTER_IN_J2J)       // (historical bisect knob, superseded) the dispatch-A-side tryGetter entry is now DEFAULT OFF; see PHARO_T1_GETTER_IN_J2J.
DEBUG_BOOL(PHARO_T1_GETTER_IN_J2J)          // opt-IN: emit the dispatch-A-side tbnz into the shared tryGetter label. DEFAULT OFF since 2026-06-10 — controlled bisect (catch22/23) proved this entry corrupts under MAX_IC=1 (bail paths reach it with inconsistent register/state; x2/x1 reload reduced but didn't eliminate). Plain-probe getter entries are unaffected and stay on.
DEBUG_BOOL(PHARO_J2J_STACK_SCAN)          // localizer: at each det-sched checkpoint, scan the live operand stack [stackBase_,stackPointer_) for a pointer-shaped slot (bit0=0, >=0x10000) that is NOT a valid heap object -> catches the global-inline-J2J raw-pointer corruption within 1024 bytecodes of the bad push, logging step+slot+value+current method. Capped.
DEBUG_BOOL(PHARO_T1_LOG_SELFREC_PUSH)     // emit a gated runtime call at each self-rec inline-J2J save-push that records the caller(=callee) CompiledMethod oop into a ring; dump_selfrec_ring() (called at the #extent DNU) resolves the last N to selectors -> NAMES the self-recursive method whose inline-J2J save/branch/return desyncs. DET_SCHED-safe (bytecode-counted scheduling unaffected by added instructions).
DEBUG_BOOL(PHARO_T1_NO_J2J_BRANCH)        // bisect (now gates the WHOLE if(inlineJ2J) send-emit block at AsmjitT1.cpp:3634): with INLINE_J2J=1, skip emitting the entire inline-J2J send-site block (J2J checks + dispatch-A + dead tryInlineJ2J/j2jBail blocks); bit-60 fill + return-prelude emit unchanged. PROVEN: this makes INLINE_J2J=1 CLEAN (3+4=7), so the #extent corruptor is the send-emit block itself, not the fill/prelude/push.
DEBUG_BOOL(PHARO_T1_NO_J2J_RETPRELUDE)    // bisect: skip emitting the per-method inline-J2J RETURN PRELUDE (the j2jDepth>j2jEntryDepth pop/resume epilog) even when PHARO_T1_INLINE_J2J=1. If this (with NO_J2J_BRANCH) makes startup match default, the return prelude mis-fires on trampoline-activated returns (shared j2jDepth) — the corruptor.
// ── inline-J2J dispatch-A "extra" inline specializations (IC bits 51-58) ──
// These are routed ONLY by the inline-J2J send emit's dispatch-A; the validated
// default/j2jBail dispatch paths never branch to them, so they were never
// exercised before inline-J2J (which never worked) = UNVALIDATED + buggy
// (tryMultiSlot writes a wild receiver -> the global-inline-J2J #extent crash;
// the set also yields mustBeBoolean).  DEFAULT-OFF (opt-in) so global inline-J2J
// is correct.  Re-enable a spec only after validating its emit.  (2026-06-09)
DEBUG_BOOL(PHARO_T1_INLINE_MULTISLOT)        // bit 57: ^ self[A] op1 self[B] op2 const
DEBUG_BOOL(PHARO_T1_INLINE_RETURNS_LITERAL)  // bit 58: ^ nil/true/false/0/1
DEBUG_BOOL(PHARO_T1_INLINE_TEMP_RETURN)      // bit 54: ^ arg N
DEBUG_BOOL(PHARO_T1_INLINE_INT_CMP_RETURN)   // bit 53: ^ self cmp arg
DEBUG_BOOL(PHARO_T1_INLINE_INT_ARITH_RETURN) // bit 52: ^ self op arg
DEBUG_BOOL(PHARO_T1_INLINE_EVEN_ODD)         // bit 51: Integer>>even/odd
DEBUG_BOOL(PHARO_T1_RESUME_INTERNAL_J2J)    // EXPERIMENT (v2, still corrupts): J2J pool slice for resumed methods; handler-protocol audit pending
DEBUG_BOOL(PHARO_T1_RESUME_EXTERNAL_J2J)    // opt-out: restore the null-slice external-J2J mode for tryResume-resumed methods (one C++ round trip per send)
DEBUG_BOOL(PHARO_T1_NO_IC_POLY_WALK)        // opt-out: probe IC slot 0 only (default walks slots 0-2 since 2026-06-12; poly sites missed once per activation -> interp residency)
DEBUG_BOOL(PHARO_T1_NO_RESUME_CONDJUMP)     // opt-out: refuse mid-method resume for cond-jump send methods (default ADVERTISE since 2026-06-12)
DEBUG_BOOL(PHARO_T1_NO_BAILMID_CALLEES)     // opt-out: refuse canBailMidMethod callees at the xmethod J2J gate (default ADMIT since 2026-06-12; the guarded corruption was the PMS/NO_INLINE_J2J interaction)
DEBUG_BOOL(PHARO_T1_LEAF_ALL_PRIMS)         // REPRO KNOB: prologue-leaf ALL supported prims (flickers the closure-as-receiver DNU ~1-in-3 ladders — the admitted-callee bail bug)
DEBUG_BOOL(PHARO_T1_NO_PROLOGUE_LEAF)       // opt out of prologue-leaf compiles (prim methods with unsupported bodies fall back to bail-on-entry stubs)
DEBUG_BOOL(PHARO_T1_NO_J2J_PRIM_PROLOGUE)   // opt out of admitting prim-prologue callees through the xmethod J2J gates
DEBUG_BOOL(PHARO_T1_SIEVE_GATE)             // restore the legacy stub-on-condjump gate for prim 60/61/62 methods (root cause fixed 2026-06-12; default OFF)
DEBUG_BOOL(PHARO_JIT_MATGUARD_DEEP)         // restore the 4-deep matRetSlot scan in canJITActivate (default 1-deep; the startup-DNU flake is guard-independent)
DEBUG_BOOL(PHARO_IGNOC_WIDEN)               // OPT-IN (unsound): VM-side ignoreOuterContext widening — skips materialize for body-clean blocks but breaks home identity + terminate-unwind of ensure: blocks (BlockClosureTest/SemaphoreTest, 2026-06-12)
DEBUG_BOOL(PHARO_PREEMPT_YIELDS)            // opt back to pre-2026-06-12 preemption: preempted process appended to the BACK of its priority list (Cog default is FRONT — processPreemptionYields=false)
DEBUG_BOOL(PHARO_NO_RR_SCHED)               // EXPERIMENT: disable heartbeat-driven same-priority round-robin (Cog never time-slices within a priority; our 2ms forceYield is a semantic deviation suspected in testThreadSafe/testInCriticalWait/testConcurrentSuspendAllWhile flakes)
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
DEBUG_BOOL(PHARO_T1_NO_BC_FLOAT)         // bisect G* SmallFloat bug: disable bytecode-level inline SmallFloat +/- fast-path (0x60/0x61)
DEBUG_BOOL(PHARO_HEADLESS)               // force headless: skip Metal test surface + GUI click injection so SUnit batches aren't poisoned by render-loop contention (no image args needed)
DEBUG_BOOL(PHARO_TRACE_WEDGE_NIL)        // full-suite wedge: at a nil-receiver DNU, dump the real activeContext_ chain to find the T1-miscompiled method
DEBUG_BOOL(PHARO_NO_DELAY_RECOVERY)      // diagnose full-suite wedge: skip checkTimerSemaphore death-recovery re-signal (suspected to over-signal timingSemaphore and desync the DelaySemaphoreScheduler front/back handshake)
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
DEBUG_BOOL(PHARO_T1_TOS_REG)              // opt-in during B0-B4 (flip to PHARO_T1_NO_TOS_REG at B5)
DEBUG_INT(PHARO_T1_TOS_MASK, -1)          // bit per converted family (-1 = all): per-term A/B + miscompile bisect
DEBUG_BOOL(PHARO_T1_TOS_POISON)           // movz x26,#0xDEAD at every invalidation point — stale-valid consumers crash deterministically
DEBUG_BOOL(PHARO_T1_TOS_VERIFY)           // before every cache-consuming emit: ldur/cmp/b.eq/brk — THE net for the valid-but-stale direction
DEBUG_BOOL(PHARO_T1_NO_NATIVE_BACKJUMP)   // opt-OUT: revert prefixed ExtJump (loop back edges!) to the bail-to-interp emit.  Default ON since 2026-06-11: the bail meant EVERY to:do:/whileTrue back edge dropped the whole activation to the interpreter (the '29.6ns/iter floor' mystery)
DEBUG_INT(PHARO_T1_RESUME_MIN_COMPILE, -1)  // send-resume bisect: advertise resume for send-bearing methods ONLY when compile-seq >= MIN (with FORCE_RESUME_FOR_SENDS semantics for that range); -1 = off
DEBUG_INT(PHARO_T1_RESUME_MAX_COMPILE, -1)  // ...and compile-seq < MAX; -1 = no cap.  Bisects WHICH method's resume wedges startup (the only-idle family)
DEBUG_INT(PHARO_T1_RESUME_MIN2_COMPILE, -1) // second bisect window (pair-interaction hunts)
DEBUG_BOOL(PHARO_BLOCK_CREATE_TRACE)        // dump every createFullBlockWithLiteral: method/fp/sp/copied-values/outerContext (capped 4000) — the cascade-#3 divergence finder
DEBUG_BOOL(PHARO_SP_DEPTH_TRAP)             // __builtin_debugtrap() at the first sp-depth mismatch — lldb lands on the inconsistent frame
DEBUG_INT(PHARO_T1_RESUME_MAX2_COMPILE, -1)

#undef DEBUG_BOOL
#undef DEBUG_INT
#undef DEBUG_STR
