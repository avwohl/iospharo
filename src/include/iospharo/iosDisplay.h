/*
 * iosDisplay.h - iOS Display Backend for Pharo VM
 *
 * Provides the display interface between the Pharo VM and iOS/Metal rendering.
 * Functions prefixed with ios_ are callable from Swift.
 */

#ifndef IOS_DISPLAY_H
#define IOS_DISPLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Display update callback type - called when VM updates display */
typedef void (*IOSDisplayUpdateCallback)(int x, int y, int width, int height);

/* Register callback for display updates (called from Swift) */
void ios_registerDisplayUpdateCallback(IOSDisplayUpdateCallback callback);

/* Get current display buffer and dimensions (called from Swift for Metal) */
void* ios_getDisplayBits(void);
int ios_getDisplayWidth(void);
int ios_getDisplayHeight(void);
int ios_getDisplayDepth(void);

/* Set display dimensions (called from Swift when view size changes) */
void ios_setDisplaySize(int width, int height);

/* Touch event types */
#define IOS_TOUCH_DOWN      0
#define IOS_TOUCH_MOVED     1
#define IOS_TOUCH_UP        2
#define IOS_TOUCH_CANCELLED 3

/* Queue touch event (called from Swift) */
void ios_queueTouchEvent(int type, int x, int y, int buttons, int modifiers);

/* Queue keyboard event (called from Swift) */
void ios_queueKeyEvent(int charCode, int pressCode, int modifiers);

/* Key event press codes (matching Pharo expectations) */
#define IOS_KEY_DOWN   0
#define IOS_KEY_UP     1
#define IOS_KEY_CHAR   2

/* Mouse button masks */
#define IOS_RED_BUTTON    4   /* Primary/left - single tap */
#define IOS_YELLOW_BUTTON 2   /* Middle - two finger tap */
#define IOS_BLUE_BUTTON   1   /* Secondary/right - long press */

/* Modifier key masks */
#define IOS_SHIFT_KEY   1
#define IOS_CTRL_KEY    2
#define IOS_ALT_KEY     4
#define IOS_CMD_KEY     8

#ifdef __cplusplus
}
#endif

#endif /* IOS_DISPLAY_H */
