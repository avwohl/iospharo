// DebugVars.hpp — X-macro machinery over debug_vars.h.
//
// Generates, from the single list in debug_vars.h:
//   - an enum id per knob (DBB_<name> / DBI_<name> / DBS_<name>)
//   - extern storage arrays
//   - GET_DEBUG_BOOL/INT/STR(name) accessors
// See debug_vars.h for the list and how to add a knob.
#ifndef PHARO_DEBUG_VARS_HPP
#define PHARO_DEBUG_VARS_HPP

namespace pharo {

enum DebugBoolId {
#define DEBUG_BOOL(name) DBB_##name,
#define DEBUG_INT(name, dflt)
#define DEBUG_STR(name)
#include "debug_vars.h"
    DEBUG_BOOL_COUNT
};

enum DebugIntId {
#define DEBUG_BOOL(name)
#define DEBUG_INT(name, dflt) DBI_##name,
#define DEBUG_STR(name)
#include "debug_vars.h"
    DEBUG_INT_COUNT
};

enum DebugStrId {
#define DEBUG_BOOL(name)
#define DEBUG_INT(name, dflt)
#define DEBUG_STR(name) DBS_##name,
#include "debug_vars.h"
    DEBUG_STR_COUNT
};

// Storage (sized with a +0/+1 guard so a count of 0 doesn't yield a 0-length
// array, which is ill-formed in standard C++).
extern bool        g_debug_bools[DEBUG_BOOL_COUNT ? DEBUG_BOOL_COUNT : 1];
extern int         g_debug_ints [DEBUG_INT_COUNT  ? DEBUG_INT_COUNT  : 1];
extern const char* g_debug_strs [DEBUG_STR_COUNT  ? DEBUG_STR_COUNT  : 1];

// Parse the environment into the arrays.  Safe to call multiple times
// (e.g. from DebugSettings::reload()).  Also run once at static-init.
void initDebugVars();

#define GET_DEBUG_BOOL(name) (::pharo::g_debug_bools[::pharo::DBB_##name])
#define GET_DEBUG_INT(name)  (::pharo::g_debug_ints [::pharo::DBI_##name])
#define GET_DEBUG_STR(name)  (::pharo::g_debug_strs [::pharo::DBS_##name])

// "Resume-internal J2J" mode.  Default ON on arm64 since 2026-10-12, but
// read what it actually does there: T1 REFUSES every compile that would emit
// an inline-J2J closure push (emitClosurePush, AsmjitT1.cpp), so send-bearing
// methods that would use inline J2J stay out of T1.  Real resume-internal J2J
// (the push made encodable) corrupts (jrpc: STON parse errors), so the push is
// never emitted.  The refusal is the measured win: full SUnit sweep 56 min
// instead of 1 h 53 min, same results (docs/perf-activation-measurements.md,
// "Correction").  It also reserves the resume J2J slice in
// tryJITResumeInCaller.  Opt-out PHARO_T1_RESUME_EXTERNAL_J2J.  x86_64 stays
// opt-in (PHARO_T1_RESUME_INTERNAL_J2J), where the push IS encodable.
inline bool resumeInternalJ2JEnabled() {
#if defined(__aarch64__) || defined(__arm64__)
    return !GET_DEBUG_BOOL(PHARO_T1_RESUME_EXTERNAL_J2J);
#else
    return GET_DEBUG_BOOL(PHARO_T1_RESUME_INTERNAL_J2J);
#endif
}

}  // namespace pharo

#endif  // PHARO_DEBUG_VARS_HPP
