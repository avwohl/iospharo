/*
 * ObjCExceptionGuard.m - Objective-C exception catching for FFI calls
 *
 * Must be compiled as Objective-C (.m) so @try/@catch works.
 * C++ catch(...) does not reliably catch ObjC exceptions in .cpp files.
 */

#import <Foundation/Foundation.h>
#include "ObjCExceptionGuard.h"

bool objc_guarded_call(ObjCGuardFunc func, void* context) {
    @try {
        func(context);
        return true;
    } @catch (NSException* exception) {
        NSLog(@"[PharoVM] ObjC exception caught in FFI call: %@ — %@",
              exception.name, exception.reason);
        return false;
    } @catch (id exception) {
        NSLog(@"[PharoVM] Unknown ObjC exception caught in FFI call");
        return false;
    }
}

static void uncaughtExceptionHandler(NSException* exception) {
    NSLog(@"[PharoVM] Uncaught ObjC exception: %@ — %@\n%@",
          exception.name, exception.reason, exception.callStackSymbols);
}

void objc_install_exception_handler(void) {
    NSSetUncaughtExceptionHandler(&uncaughtExceptionHandler);
}
