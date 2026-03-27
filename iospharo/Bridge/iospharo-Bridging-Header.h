/*
 * iospharo-Bridging-Header.h
 *
 * Exposes C functions from the Pharo VM to Swift.
 */

#ifndef iospharo_Bridging_Header_h
#define iospharo_Bridging_Header_h

#include <stdint.h>
#include <stdbool.h>

/* VM Parameters (shared definition) */
#include "../../src/platform/VMParameters.h"

/* VM lifecycle functions */
int vm_init(VMParameters* parameters);
void vm_run(void);  // Start interpreter on background thread (returns immediately)
bool vm_isRunning(void);  // Check if interpreter is running
void vm_stop(void);     // Must be called before app exit to prevent crash
void vm_destroy(void);  // Delete all VM objects, reset state for relaunch
void vm_parameters_init(VMParameters* parameters);
void vm_parameters_destroy(VMParameters* parameters);

/* iOS display bridge functions */
typedef void (*IOSDisplayUpdateCallback)(int x, int y, int width, int height);

void ios_registerDisplayUpdateCallback(IOSDisplayUpdateCallback callback);
void ios_setDisplaySize(int width, int height);

/* Atomic display info (prevents tearing during resize) */
typedef struct {
    uint32_t* pixels;
    int width;
    int height;
    size_t size;  // Total pixels (width * height)
} IOSDisplayBufferInfo;

void ios_getDisplayBufferInfo(IOSDisplayBufferInfo* info);

/* Event functions (C++ EventQueue) */
void vm_postMouseEvent(int type, int x, int y, int buttons, int modifiers);
void vm_postKeyEvent(int type, int charCode, int keyCode, int modifiers);
void vm_postScrollEvent(int x, int y, int deltaX, int deltaY, int modifiers);

/* Touch event types */
#define IOS_TOUCH_DOWN      0
#define IOS_TOUCH_MOVED     1
#define IOS_TOUCH_UP        2
#define IOS_TOUCH_CANCELLED 3

/* Key event press codes */
#define IOS_KEY_DOWN   0
#define IOS_KEY_UP     1
#define IOS_KEY_CHAR   2

/* Mouse button masks */
#define IOS_RED_BUTTON    4
#define IOS_YELLOW_BUTTON 2
#define IOS_BLUE_BUTTON   1

/* Modifier key masks */
#define IOS_SHIFT_KEY   1
#define IOS_CTRL_KEY    2
#define IOS_ALT_KEY     4
#define IOS_CMD_KEY     8

/* Clipboard bridge */
typedef const char* (*ClipboardGetFunc)(void);
typedef void (*ClipboardSetFunc)(const char* text);
void vm_setClipboardCallbacks(ClipboardGetFunc getFunc, ClipboardSetFunc setFunc);

/* Text input (keyboard) bridge */
typedef void (*TextInputFunc)(bool active);
void vm_setTextInputCallback(TextInputFunc func);

/* Display readiness flags */
bool ffi_isSDLRenderingActive(void);     // True after first SDL_RenderPresent (Pharo drew a frame)
bool vm_isDisplayFormReady(void);        // True after primitiveBeDisplay (Display Form has valid pixels)

/* Core Motion shared data */
#include "../../src/platform/MotionData.h"

/* iOS utility functions */
int iosIsIPad(void);
const char* iosGetDeviceModel(void);
const char* iosGetSystemVersion(void);
int iosOpenURL(const char* urlString);
void iosShowAlert(const char* title, const char* message);
void iosHapticFeedback(int style);

#endif /* iospharo_Bridging_Header_h */
