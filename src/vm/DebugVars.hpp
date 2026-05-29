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

}  // namespace pharo

#endif  // PHARO_DEBUG_VARS_HPP
