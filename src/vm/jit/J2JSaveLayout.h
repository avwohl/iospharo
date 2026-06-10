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
 *   - save shrinks 56 -> 32 bytes {sp, receiver, tempBase, resumeAddr}
 *   - sendArgCount: popped by the resume site with a static immediate
 *   - jitMethod: re-established by the resume site from emit-time
 *     immediates; materialize derives it from resumeAddr via the zone
 *   - ip: derived at materialize time (rare path)
 * Flip ONLY when every producer/consumer batch is converted (push
 * emits incl. xmethod + retro-stub, prelude, trampoline push/pop,
 * both C++ chain-loop pops, materializeJ2JSaveIntoFrame, the
 * prepareForGC/afterGC pool walks, forEachRoot's save visits, and
 * the sp-depth save checker).
 */
#ifndef PHARO_J2J_SAVE_LAYOUT_H
#define PHARO_J2J_SAVE_LAYOUT_H

#define PHARO_J2J_SAVE_V2 0

#if PHARO_J2J_SAVE_V2

#define JSV_SP            0
#define JSV_RECEIVER      8
#define JSV_TEMPBASE      16
#define JSV_RESUMEADDR    24
#define JSV_SIZE          32

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
