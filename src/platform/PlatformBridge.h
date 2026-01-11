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

// Atomic display info (prevents tearing during resize)
typedef struct {
    uint32_t* pixels;
    int width;
    int height;
    size_t size;  // Total pixels (width * height)
} DisplayBufferInfo;

// Get all display info atomically - use this instead of separate calls
void vm_getDisplayBufferInfo(DisplayBufferInfo* info);

// SAFE: Copy display buffer to destination (holds lock during copy)
// Returns true if successful, false if dest too small
// This prevents tearing by not letting pointers escape
bool vm_copyDisplayBuffer(uint32_t* dest, size_t destSize, int* outWidth, int* outHeight);

// Check if display is being resized (Metal should skip updates during resize)
bool vm_isDisplayResizing(void);

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
