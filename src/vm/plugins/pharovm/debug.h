/* debug.h — Minimal compatibility shim for plugins that use pharovm logging.
 * Redirects log macros to stderr via fprintf.
 */
#pragma once

#include "exportDefinition.h"
#include <stdio.h>

#ifndef DEBUG
# define DEBUG 0
#endif

#define LOG_NONE   0
#define LOG_ERROR  1
#define LOG_WARN   2
#define LOG_INFO   3
#define LOG_DEBUG  4
#define LOG_TRACE  5

/* Stub: plugins that call logMessage will get fprintf to stderr */
#define logTrace(...)  ((void)0)
#define logDebug(...)  ((void)0)
#define logInfo(...)   ((void)0)
#define logWarn(...)   fprintf(stderr, "WARN: " __VA_ARGS__)
#define logError(...)  fprintf(stderr, "ERROR: " __VA_ARGS__)
