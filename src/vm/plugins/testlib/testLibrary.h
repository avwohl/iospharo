#ifndef TEST_LIBRARY_H
#define TEST_LIBRARY_H

// Export symbols so they are visible to dlsym(RTLD_DEFAULT, ...)
// On iOS/macOS, force-load ensures symbols are linked, and visibility("default")
// ensures they appear in the dynamic symbol table.
#ifdef _WIN32
# include <windows.h>
# define EXPORT(returnType) __declspec( dllexport ) returnType
#else
# define EXPORT(returnType) __attribute__((used, visibility("default"))) returnType
#endif


#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#if !defined(_WIN32)
#include <unistd.h>
#endif


#include "structures.h"

typedef enum {
	firstuint = 1
} uintenum;

typedef enum {
	firstsint = -1
} sintenum;

typedef enum {
	firstchar = 'a'
} charenum;

#endif
