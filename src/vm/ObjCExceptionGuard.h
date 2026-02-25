/*
 * ObjCExceptionGuard.h - Catch ObjC exceptions from C/C++ code
 *
 * On Apple platforms, FFI calls into AppKit/UIKit may throw ObjC exceptions
 * (NSInternalInconsistencyException, etc). C++ catch(...) does NOT reliably
 * catch these in .cpp files. This guard uses @try/@catch in Objective-C to
 * ensure they're caught.
 */
#ifndef OBJC_EXCEPTION_GUARD_H
#define OBJC_EXCEPTION_GUARD_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ObjCGuardFunc)(void* context);

/*
 * Call func(context) inside an @try/@catch block.
 * Returns true if the call succeeded, false if an ObjC exception was thrown.
 * On non-Apple platforms, just calls func(context) directly and returns true.
 */
bool objc_guarded_call(ObjCGuardFunc func, void* context);

/*
 * Install a global uncaught exception handler that logs to stderr.
 * Call once at startup for diagnostics.
 */
void objc_install_exception_handler(void);

#ifdef __cplusplus
}
#endif

#endif /* OBJC_EXCEPTION_GUARD_H */
