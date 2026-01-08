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
void vm_run(void);
void vm_stop(void);
bool vm_isRunning(void);

// Display
void vm_setDisplaySize(int width, int height, int depth);
uint32_t* vm_getDisplayPixels(void);
int vm_getDisplayWidth(void);
int vm_getDisplayHeight(void);

// Display callback (called when VM wants to update screen)
typedef void (*DisplayUpdateFunc)(int x, int y, int w, int h, void* context);
void vm_setDisplayUpdateCallback(DisplayUpdateFunc callback, void* context);

// Events
void vm_postMouseEvent(int type, int x, int y, int buttons, int modifiers);
void vm_postKeyEvent(int type, int charCode, int keyCode, int modifiers);
void vm_postScrollEvent(int x, int y, int deltaX, int deltaY, int modifiers);
void vm_postWindowEvent(int width, int height);

#ifdef __cplusplus
}
#endif

#endif
