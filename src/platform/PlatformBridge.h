/*
 * PlatformBridge.h - C interface for Swift/ObjC interop
 */

#ifndef PHARO_PLATFORM_BRIDGE_H
#define PHARO_PLATFORM_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// VM lifecycle
bool vm_initialize(size_t heapSize);
bool vm_loadImage(const char* imagePath);
void vm_run(void);           // Runs interpreter on a background thread
void vm_runOnMainThread(void); // Runs interpreter on the current (main) thread
void vm_stop(void);
void vm_destroy(void);       // Delete all VM objects, reset state for relaunch
bool vm_isRunning(void);

// Display
void vm_setDisplaySize(int width, int height, int depth);
uint32_t* vm_getDisplayPixels(void);
int vm_getDisplayWidth(void);
int vm_getDisplayHeight(void);

// Atomic display info (prevents tearing during resize)
typedef struct {
    uint32_t* pixels;
    int width;
    int height;
    size_t size;  // Total pixels (width * height)
} DisplayBufferInfo;

// Get all display info atomically - use this instead of separate calls
void vm_getDisplayBufferInfo(DisplayBufferInfo* info);

// Check if display is being resized (Metal should skip updates during resize)
bool vm_isDisplayResizing(void);

// Display callback (called when VM wants to update screen)
typedef void (*DisplayUpdateFunc)(int x, int y, int w, int h, void* context);
void vm_setDisplayUpdateCallback(DisplayUpdateFunc callback, void* context);

// Events
void vm_postMouseEvent(int type, int x, int y, int buttons, int modifiers);
void vm_postKeyEvent(int type, int charCode, int keyCode, int modifiers);
void vm_postScrollEvent(int x, int y, int deltaX, int deltaY, int modifiers);

// Clipboard (callbacks registered by Swift)
typedef const char* (*ClipboardGetFunc)(void);
typedef void (*ClipboardSetFunc)(const char* text);
void vm_setClipboardCallbacks(ClipboardGetFunc getFunc, ClipboardSetFunc setFunc);
const char* vm_getClipboardText(void);
void vm_setClipboardText(const char* text);

// Text input (keyboard show/hide callbacks registered by Swift)
typedef void (*TextInputFunc)(bool active);
void vm_setTextInputCallback(TextInputFunc func);
void vm_startTextInput(void);
void vm_stopTextInput(void);

#ifdef __cplusplus
}
#endif

#endif
