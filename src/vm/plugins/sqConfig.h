/* Minimal sqConfig.h for plugin compilation (macOS ARM64) */
#ifndef SQ_CONFIG_H
#define SQ_CONFIG_H

#include "config.h"

#define VMBIGENDIAN 0
#define NeverInline __attribute__((noinline))

/* Spur 64-bit object header size */
#ifndef BaseHeaderSize
#define BaseHeaderSize 8
#endif

/* Absolute value macro used by VMMaker-generated code */
#ifndef SQABS
#define SQABS(x) ((x) < 0 ? -(x) : (x))
#endif

#endif /* SQ_CONFIG_H */
