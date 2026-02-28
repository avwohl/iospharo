/*
 * NSEventMonitor.m
 *
 * Implementation of NSEvent monitoring for Mac Catalyst.
 * Uses dynamic Objective-C to access AppKit classes.
 * This entire file is Mac Catalyst-only; on iOS it compiles as stubs.
 */

#import "NSEventMonitor.h"

#if TARGET_OS_MACCATALYST

#import <objc/runtime.h>
#import <objc/message.h>

// Forward declarations for NSEvent types we need
// These are accessed dynamically since AppKit isn't directly linked

typedef NS_OPTIONS(unsigned long long, NSEventMask) {
    NSEventMaskLeftMouseDown     = 1 << 1,
    NSEventMaskLeftMouseUp       = 1 << 2,
    NSEventMaskRightMouseDown    = 1 << 3,
    NSEventMaskRightMouseUp      = 1 << 4,
    NSEventMaskMouseMoved        = 1 << 5,
    NSEventMaskLeftMouseDragged  = 1 << 6,
    NSEventMaskRightMouseDragged = 1 << 7,
    NSEventMaskMouseEntered      = 1 << 8,
    NSEventMaskMouseExited       = 1 << 9,
    NSEventMaskOtherMouseDown    = 1 << 25,
    NSEventMaskOtherMouseUp      = 1 << 26,
    NSEventMaskOtherMouseDragged = 1 << 27,
    NSEventMaskScrollWheel       = 1 << 22,
};

typedef NS_ENUM(NSUInteger, NSEventType) {
    NSEventTypeLeftMouseDown     = 1,
    NSEventTypeLeftMouseUp       = 2,
    NSEventTypeRightMouseDown    = 3,
    NSEventTypeRightMouseUp      = 4,
    NSEventTypeMouseMoved        = 5,
    NSEventTypeLeftMouseDragged  = 6,
    NSEventTypeRightMouseDragged = 7,
    NSEventTypeMouseEntered      = 8,
    NSEventTypeMouseExited       = 9,
    NSEventTypeOtherMouseDown    = 25,
    NSEventTypeOtherMouseUp      = 26,
    NSEventTypeOtherMouseDragged = 27,
    NSEventTypeScrollWheel       = 22,
};

@implementation NSEventMonitorHelper

static id _monitor = nil;
static MouseEventCallback _callback = nil;
static int _eventCount = 0;
static id (^_handlerBlock)(id) = nil;  // Keep strong reference to handler
#ifdef DEBUG
static FILE *_logFile = NULL;
#endif

+ (void)installMouseMonitorWithCallback:(MouseEventCallback)callback {
    if (_monitor) {
        [self removeMouseMonitor];
    }

    _callback = [callback copy];

#ifdef DEBUG
    // Open log file (debug builds only)
    NSString *logPath = [NSTemporaryDirectory() stringByAppendingPathComponent:@"nsevent_objc.log"];
    _logFile = fopen([logPath UTF8String], "w");
    if (_logFile) {
        fprintf(_logFile, "[NSEVENT-OBJC] Installing mouse monitor...\n");
        fflush(_logFile);
    }
#endif

    // Get NSEvent class dynamically
    Class NSEventClass = NSClassFromString(@"NSEvent");
    if (!NSEventClass) {
#ifdef DEBUG
        if (_logFile) {
            fprintf(_logFile, "[NSEVENT-OBJC] ERROR: NSEvent class not found\n");
            fclose(_logFile);
            _logFile = NULL;
        }
#endif
        return;
    }

#ifdef DEBUG
    if (_logFile) {
        fprintf(_logFile, "[NSEVENT-OBJC] Found NSEvent class: %p\n", (__bridge void *)NSEventClass);
        fflush(_logFile);
    }
#endif

    // Event mask for all mouse events
    NSEventMask mask = NSEventMaskLeftMouseDown | NSEventMaskLeftMouseUp |
                       NSEventMaskRightMouseDown | NSEventMaskRightMouseUp |
                       NSEventMaskMouseMoved |
                       NSEventMaskLeftMouseDragged | NSEventMaskRightMouseDragged |
                       NSEventMaskOtherMouseDown | NSEventMaskOtherMouseUp |
                       NSEventMaskOtherMouseDragged |
                       NSEventMaskScrollWheel;

    // Try GLOBAL monitor first (local doesn't work on Mac Catalyst)
    // Note: Global monitors receive events not destined for our app, so we need to filter by window
    SEL addMonitorSelector = NSSelectorFromString(@"addGlobalMonitorForEventsMatchingMask:handler:");

    if (![NSEventClass respondsToSelector:addMonitorSelector]) {
        // Fall back to local monitor
        addMonitorSelector = NSSelectorFromString(@"addLocalMonitorForEventsMatchingMask:handler:");
    }

    if (![NSEventClass respondsToSelector:addMonitorSelector]) {
        return;
    }

    // Create the handler block and keep a strong reference to it
    _handlerBlock = ^id(id event) {
        _eventCount++;

        // Get event type using performSelector
        SEL typeSel = NSSelectorFromString(@"type");
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-performSelector-leaks"
        NSUInteger eventType = (NSUInteger)[event performSelector:typeSel];
#pragma clang diagnostic pop

        // Get locationInWindow - returns NSPoint which is a struct
        // We need to use NSInvocation for struct return values
        NSPoint location = NSZeroPoint;
        SEL locSel = NSSelectorFromString(@"locationInWindow");
        NSMethodSignature *sig = [event methodSignatureForSelector:locSel];
        if (sig) {
            NSInvocation *inv = [NSInvocation invocationWithMethodSignature:sig];
            [inv setSelector:locSel];
            [inv setTarget:event];
            [inv invoke];
            [inv getReturnValue:&location];
        }

        // Get button number for mouse button events
        NSInteger buttonNumber = 0;
        if (eventType == NSEventTypeLeftMouseDown || eventType == NSEventTypeLeftMouseUp ||
            eventType == NSEventTypeRightMouseDown || eventType == NSEventTypeRightMouseUp ||
            eventType == NSEventTypeOtherMouseDown || eventType == NSEventTypeOtherMouseUp) {
            SEL buttonSel = NSSelectorFromString(@"buttonNumber");
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-performSelector-leaks"
            buttonNumber = (NSInteger)[event performSelector:buttonSel];
#pragma clang diagnostic pop
        }

#ifdef DEBUG
        if (_logFile && _eventCount <= 200) {
            fprintf(_logFile, "[NSEVENT-OBJC] #%d type=%lu loc=(%.1f,%.1f) btn=%ld\n",
                    _eventCount, (unsigned long)eventType, location.x, location.y, (long)buttonNumber);
            fflush(_logFile);
        }
#endif

        // Call the Swift callback
        if (_callback) {
            _callback((int)eventType, location.x, location.y, (int)buttonNumber);
        }

        return event;  // Return event to allow normal processing
    };

    // Use objc_msgSend to call addGlobalMonitor (or addLocalMonitor fallback)
    // Signature: + (id)add{Global,Local}MonitorForEventsMatchingMask:(NSEventMask)mask handler:(...)
    typedef id (*AddMonitorIMP)(Class, SEL, unsigned long long, id);
    AddMonitorIMP addMonitor = (AddMonitorIMP)objc_msgSend;

    _monitor = addMonitor(NSEventClass, addMonitorSelector, mask, _handlerBlock);
}

+ (void)removeMouseMonitor {
    if (!_monitor) return;

    // Get NSEvent class
    Class NSEventClass = NSClassFromString(@"NSEvent");
    if (!NSEventClass) return;

    // Call removeMonitor:
    SEL removeSelector = NSSelectorFromString(@"removeMonitor:");
    if ([NSEventClass respondsToSelector:removeSelector]) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-performSelector-leaks"
        [NSEventClass performSelector:removeSelector withObject:_monitor];
#pragma clang diagnostic pop
    }

    _monitor = nil;
    _callback = nil;
    _handlerBlock = nil;

#ifdef DEBUG
    if (_logFile) {
        fprintf(_logFile, "[NSEVENT-OBJC] Monitor removed\n");
        fclose(_logFile);
        _logFile = NULL;
    }
#endif
}

@end

#else // !TARGET_OS_MACCATALYST — iOS stubs

@implementation NSEventMonitorHelper

+ (void)installMouseMonitorWithCallback:(MouseEventCallback)callback {
    // NSEvent monitoring is not available on iOS
}

+ (void)removeMouseMonitor {
    // No-op on iOS
}

@end

#endif // TARGET_OS_MACCATALYST
