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
DEBUG_BOOL(PHARO_J2J_STACK_SCAN)          // localizer: at each det-sched checkpoint, scan the live operand stack [stackBase_,stackPointer_) for a pointer-shaped slot (bit0=0, >=0x10000) that is NOT a valid heap object -> catches the global-inline-J2J raw-pointer corruption within 1024 bytecodes of the bad push, logging step+slot+value+current method. Capped.
DEBUG_BOOL(PHARO_T1_LOG_SELFREC_PUSH)     // emit a gated runtime call at each self-rec inline-J2J save-push that records the caller(=callee) CompiledMethod oop into a ring; dump_selfrec_ring() (called at the #extent DNU) resolves the last N to selectors -> NAMES the self-recursive method whose inline-J2J save/branch/return desyncs. DET_SCHED-safe (bytecode-counted scheduling unaffected by added instructions).
DEBUG_BOOL(PHARO_T1_NO_J2J_BRANCH)        // bisect: with inline-J2J otherwise on (bit-60 fill + return-prelude emit unchanged), SKIP emitting the `tbnz x7,#60 -> tryInlineJ2J` send-site branch so bit-60 IC-hits flow straight to normal dispatch (as in default). Isolates the bit-60 branch/bail DETOUR from the bit-60 FILL as the inline-J2J #extent corruptor.
DEBUG_BOOL(PHARO_T1_NO_J2J_RETPRELUDE)    // bisect: skip emitting the per-method inline-J2J RETURN PRELUDE (the j2jDepth>j2jEntryDepth pop/resume epilog) even when PHARO_T1_INLINE_J2J=1. If this (with NO_J2J_BRANCH) makes startup match default, the return prelude mis-fires on trampoline-activated returns (shared j2jDepth) — the corruptor.
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

#undef DEBUG_BOOL
#undef DEBUG_INT
#undef DEBUG_STR
