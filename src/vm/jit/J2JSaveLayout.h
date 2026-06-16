/*
 * J2JSaveLayout.h - the J2J save-slot layout, shared by C++ and asm.
 *
 * Copyright (c) 2026 Aaron Wohl. Licensed under the MIT License.
 *
 * Pure-preprocessor on purpose: TrampolineAsm.S includes this, so no
 * C/C++ constructs.  The C++ side (Interpreter.hpp J2JSave) carries
 * static_asserts tying the struct to these macros — the .S previously
 * duplicated the offsets with a "keep in sync by hand" comment.
 *
 * PHARO_J2J_SAVE_V2: the packed-save protocol (WIP.md, 2026-06-11).
 * V2 moves statically-known work to the per-site resume continuation:
 *   - save shrinks 56 -> 40 bytes {sp, receiver, tempBase, resumeAddr,
 *     closure}
 *   - sendArgCount: popped by the resume site with a static immediate
 *   - jitMethod: re-established by the resume site PC-relatively
 *     (codeStart - sizeof(JITMethod)); materialize derives it from
 *     resumeAddr via the zone (findMethodByPC)
 *   - ip: derived at materialize time (rare path)
 * Flip ONLY when every producer/consumer batch is converted (push
 * emits incl. xmethod + retro-stub, prelude, trampoline push/pop,
 * both C++ chain-loop pops, materializeJ2JSaveIntoFrame, the
 * prepareForGC/afterGC pool walks, forEachRoot's save visits, and
 * the sp-depth save checker).
 */
#ifndef PHARO_J2J_SAVE_LAYOUT_H
#define PHARO_J2J_SAVE_LAYOUT_H

// V2 (the packed 40-byte save) is the asmjit-T1 protocol, used by BOTH the
// arm64 JIT (shipping) and the x86_64 asmjit-T1 JIT (the live x86 J2J emit
// is AsmjitT1.cpp; the stencil tier is dormant).  x86 was ported to V2 on
// 2026-06-15 (docs/x86-v2-save-port.md) — the cross-method receiver-swap fix.
// The flip is a compile-time struct choice (V1 56B vs V2 40B), so A/B is via
// a BUILD flag, not a runtime knob: build x86 with -DPHARO_X86_FORCE_V1 to
// get the historical V1 save back for bisection.
#if defined(__aarch64__) || defined(__arm64__)
#  define PHARO_J2J_SAVE_V2 1
#elif (defined(__x86_64__) || defined(_M_X64)) && !defined(PHARO_X86_FORCE_V1)
#  define PHARO_J2J_SAVE_V2 1
#else
#  define PHARO_J2J_SAVE_V2 0
#endif

#if PHARO_J2J_SAVE_V2

#define JSV_SP            0
#define JSV_RECEIVER      8
#define JSV_TEMPBASE      16
#define JSV_RESUMEADDR    24
// JSV_CLOSURE (2026-06-13): the executing frame's FullBlockClosure (nil
// for method frames), copied from JITState.closure at save-push.
// materializeJ2JSaveIntoFrame uses it to give block frames their
// closure — a nil closure builds a malformed method-context (the
// internal-J2J cannotReturn: storm).
#define JSV_CLOSURE       32
#define JSV_SIZE          40

#else  /* V1: the historical 56-byte save */

#define JSV_SP            0
#define JSV_RECEIVER      8
#define JSV_TEMPBASE      16
#define JSV_IP            24
#define JSV_JITMETHOD     32
#define JSV_RESUMEADDR    40
#define JSV_SENDARGCOUNT  48
#define JSV_SIZE          56

#endif  /* PHARO_J2J_SAVE_V2 */

#endif  /* PHARO_J2J_SAVE_LAYOUT_H */
