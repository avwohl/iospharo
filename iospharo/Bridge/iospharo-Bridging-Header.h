/*
 * iospharo-Bridging-Header.h
 *
 * Exposes C functions from the Pharo VM to Swift.
 */

#ifndef iospharo_Bridging_Header_h
#define iospharo_Bridging_Header_h

#include <stdint.h>
#include <stdbool.h>

/* VM Parameters structure (simplified for Swift) */
typedef struct VMParameterVector_ {
    uint32_t count;
    const char** parameters;
} VMParameterVector;

typedef struct VMParameters_ {
    char* imageFileName;
    bool isDefaultImage;
    bool defaultImageFound;
    bool isInteractiveSession;
    bool isWorker;
    int maxStackFramesToPrint;
    long long maxOldSpaceSize;
    long long maxCodeSize;
    long long edenSize;
    long long minPermSpaceSize;
    long long maxSlotsForNewSpaceAlloc;
    int processArgc;
    const char** processArgv;
    const char** environmentVector;
    bool avoidSearchingSegmentsWithPinnedObjects;
    VMParameterVector vmParameters;
    VMParameterVector imageParameters;
} VMParameters;

/* VM lifecycle functions */
int vm_init(VMParameters* parameters);
void vm_run_interpreter(void);
void vm_stop(void);  // Must be called before app exit to prevent crash
int vm_main_with_parameters(VMParameters* parameters);
void vm_parameters_init(VMParameters* parameters);
void vm_parameters_destroy(VMParameters* parameters);

/* iOS display bridge functions */
typedef void (*IOSDisplayUpdateCallback)(int x, int y, int width, int height);

void ios_registerDisplayUpdateCallback(IOSDisplayUpdateCallback callback);
void* ios_getDisplayBits(void);
int ios_getDisplayWidth(void);
int ios_getDisplayHeight(void);
int ios_getDisplayDepth(void);
void ios_setDisplaySize(int width, int height);

/* Atomic display info (prevents tearing during resize) */
typedef struct {
    uint32_t* pixels;
    int width;
    int height;
    size_t size;  // Total pixels (width * height)
} IOSDisplayBufferInfo;

void ios_getDisplayBufferInfo(IOSDisplayBufferInfo* info);

/* SAFE: Copy display to destination buffer under lock (prevents tearing) */
bool ios_copyDisplayBuffer(uint32_t* dest, size_t destSize, int* outWidth, int* outHeight);

/* Check if display is being resized (Metal should skip updates) */
bool ios_isDisplayResizing(void);

/* iOS event bridge functions (legacy) */
void ios_queueTouchEvent(int type, int x, int y, int buttons, int modifiers);
void ios_queueKeyEvent(int charCode, int pressCode, int modifiers);

/* Platform event functions (new C++ EventQueue) */
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

/* iOS utility functions */
int iosIsIPad(void);
const char* iosGetDeviceModel(void);
const char* iosGetSystemVersion(void);
int iosOpenURL(const char* urlString);
void iosShowAlert(const char* title, const char* message);
void iosHapticFeedback(int style);

#endif /* iospharo_Bridging_Header_h */
