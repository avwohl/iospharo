/* sq.h — Minimal compatibility shim for VMMaker-generated plugins.
 * Provides the types and macros that plugins expect from the Squeak VM.
 *
 * Based on interfaces defined by OpenSmalltalk-VM / Pharo-VM.
 * See THIRD_PARTY_LICENSES for upstream license details.
 */
#ifndef _SQ_H
#define _SQ_H

#include "sqConfig.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>

#include "sqMemoryAccess.h"
#include "sqVirtualMachine.h"

#ifndef true
#define true  1
#endif
#ifndef false
#define false 0
#endif
#ifndef null
#define null  0
#endif

#endif /* _SQ_H */
