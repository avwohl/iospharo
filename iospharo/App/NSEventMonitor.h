/*
 * NSEventMonitor.h
 *
 * Objective-C helper to access NSEvent for mouse event monitoring
 * on Mac Catalyst. This is needed because NSEvent isn't directly
 * available in Swift for Mac Catalyst.
 */

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/// Callback block for mouse events
typedef void (^MouseEventCallback)(int eventType, double x, double y, int buttonNumber);

/// Helper class to install NSEvent monitors for mouse tracking
@interface NSEventMonitorHelper : NSObject

/// Install a global event monitor for mouse events (falls back to local if global fails)
/// @param callback Block called for each mouse event
+ (void)installMouseMonitorWithCallback:(MouseEventCallback)callback;

/// Remove the installed monitor
+ (void)removeMouseMonitor;

@end

NS_ASSUME_NONNULL_END
