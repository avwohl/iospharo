// DebugVars.cpp — storage + env parsing for the debug_vars.h knobs.
#include "DebugVars.hpp"

#include <cstdlib>

namespace pharo {

bool        g_debug_bools[DEBUG_BOOL_COUNT ? DEBUG_BOOL_COUNT : 1];
int         g_debug_ints [DEBUG_INT_COUNT  ? DEBUG_INT_COUNT  : 1];
const char* g_debug_strs [DEBUG_STR_COUNT  ? DEBUG_STR_COUNT  : 1];

namespace {

const char* const k_bool_names[DEBUG_BOOL_COUNT ? DEBUG_BOOL_COUNT : 1] = {
#define DEBUG_BOOL(name) #name,
#define DEBUG_INT(name, dflt)
#define DEBUG_STR(name)
#include "debug_vars.h"
};

const char* const k_int_names[DEBUG_INT_COUNT ? DEBUG_INT_COUNT : 1] = {
#define DEBUG_BOOL(name)
#define DEBUG_INT(name, dflt) #name,
#define DEBUG_STR(name)
#include "debug_vars.h"
};

const int k_int_defaults[DEBUG_INT_COUNT ? DEBUG_INT_COUNT : 1] = {
#define DEBUG_BOOL(name)
#define DEBUG_INT(name, dflt) (dflt),
#define DEBUG_STR(name)
#include "debug_vars.h"
};

const char* const k_str_names[DEBUG_STR_COUNT ? DEBUG_STR_COUNT : 1] = {
#define DEBUG_BOOL(name)
#define DEBUG_INT(name, dflt)
#define DEBUG_STR(name) #name,
#include "debug_vars.h"
};

}  // namespace

void initDebugVars() {
    for (int i = 0; i < DEBUG_BOOL_COUNT; ++i)
        g_debug_bools[i] = std::getenv(k_bool_names[i]) != nullptr;
    for (int i = 0; i < DEBUG_INT_COUNT; ++i) {
        const char* v = std::getenv(k_int_names[i]);
        g_debug_ints[i] = (v && *v) ? std::atoi(v) : k_int_defaults[i];
    }
    for (int i = 0; i < DEBUG_STR_COUNT; ++i) {
        const char* v = std::getenv(k_str_names[i]);
        g_debug_strs[i] = (v && *v) ? v : nullptr;
    }
}

// Run once before main() — the process environment is fully populated before
// any namespace-scope constructor fires (same rationale as DebugSettings).
namespace { struct DebugVarsInit { DebugVarsInit() { initDebugVars(); } } g_debugVarsInit; }

}  // namespace pharo
