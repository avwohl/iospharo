/*
 * iosDisplay.m - iOS Display Backend for Pharo VM
 *
 * Implements the display interface required by the Pharo VM (sq.h)
 * and provides bridge functions for the Swift/Metal rendering layer.
 */

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#include "pharovm/pharo.h"
#include "iospharo/iosDisplay.h"

/* Forward declaration from PlatformBridge */
extern uint32_t* vm_getDisplayPixels(void);

/* Display state */
static void* displayBitsBuffer = NULL;
static int currentWidth = 1024;
static int currentHeight = 768;
static int currentDepth = 32;
static double currentScaleFactor = 1.0;

/* Callback to Swift layer */
static IOSDisplayUpdateCallback displayUpdateCallback = NULL;

/* Mouse/touch state */
static int lastMouseX = 0;
static int lastMouseY = 0;
static int lastButtonState = 0;

#pragma mark - Swift Bridge Functions

void ios_registerDisplayUpdateCallback(IOSDisplayUpdateCallback callback) {
    displayUpdateCallback = callback;
}

void* ios_getDisplayBits(void) {
    // First try the C++ platform bridge display surface (used for direct morph rendering)
    uint32_t* platformPixels = vm_getDisplayPixels();
    if (platformPixels) {
        return platformPixels;
    }
    // Fall back to VM's display form bits
    return displayBitsBuffer;
}

int ios_getDisplayWidth(void) {
    return currentWidth;
}

int ios_getDisplayHeight(void) {
    return currentHeight;
}

int ios_getDisplayDepth(void) {
    return currentDepth;
}

void ios_setDisplaySize(int width, int height) {
    currentWidth = width;
    currentHeight = height;

    /* Update scale factor from main screen */
    dispatch_async(dispatch_get_main_queue(), ^{
        currentScaleFactor = [[UIScreen mainScreen] scale];
    });
}

#pragma mark - VM Display Interface (sq.h)

sqInt ioShowDisplay(sqInt dispBitsIndex, sqInt width, sqInt height, sqInt depth,
                    sqInt affectedL, sqInt affectedR, sqInt affectedT, sqInt affectedB) {
    /* Store reference to display bits from VM */
    displayBitsBuffer = (void*)dispBitsIndex;
    currentWidth = width;
    currentHeight = height;
    currentDepth = depth;

    /* Notify Swift layer of display update with dirty region */
    if (displayUpdateCallback) {
        int x = affectedL;
        int y = affectedT;
        int w = affectedR - affectedL;
        int h = affectedB - affectedT;

        /* Ensure valid region */
        if (w > 0 && h > 0) {
            displayUpdateCallback(x, y, w, h);
        }
    }

    return 1;
}

sqInt ioForceDisplayUpdate(void) {
    if (displayUpdateCallback && displayBitsBuffer) {
        displayUpdateCallback(0, 0, currentWidth, currentHeight);
    }
    return 1;
}

sqInt ioScreenSize(void) {
    /* Return packed width/height: (width << 16) | height */
    return (currentWidth << 16) | (currentHeight & 0xFFFF);
}

sqInt ioScreenDepth(void) {
    return currentDepth;
}

double ioScreenScaleFactor(void) {
    return currentScaleFactor;
}

sqInt ioHasDisplayDepth(sqInt depth) {
    return (depth == 32 || depth == 16 || depth == 8);
}

sqInt ioSetDisplayMode(sqInt width, sqInt height, sqInt depth, sqInt fullscreenFlag) {
    currentWidth = width;
    currentHeight = height;
    currentDepth = depth;
    /* iOS is always fullscreen */
    return 1;
}

sqInt ioSetFullScreen(sqInt fullScreen) {
    /* iOS is always fullscreen */
    return 1;
}

void ioNoteDisplayChangedwidthheightdepth(void *bitsOrHandle, int w, int h, int d) {
    displayBitsBuffer = bitsOrHandle;
    currentWidth = w;
    currentHeight = h;
    currentDepth = d;
}

sqInt ioFormPrint(sqInt bitsAddr, sqInt width, sqInt height, sqInt depth,
                  double hScale, double vScale, sqInt landscapeFlag) {
    /* Printing not supported on iOS in this implementation */
    return 0;
}

#pragma mark - Window Management

char* ioGetWindowLabel(void) {
    static char label[] = "Pharo";
    return label;
}

sqInt ioSetWindowLabelOfSize(void *lblIndex, sqInt sz) {
    /* Window label not applicable on iOS */
    return 1;
}

sqInt ioGetWindowWidth(void) {
    return currentWidth;
}

sqInt ioGetWindowHeight(void) {
    return currentHeight;
}

sqInt ioSetWindowWidthHeight(sqInt w, sqInt h) {
    currentWidth = w;
    currentHeight = h;
    return 1;
}

sqInt ioIsWindowObscured(void) {
    return 0;
}

#pragma mark - Input State (see iosEvents.m for event queue)

sqInt ioGetButtonState(void) {
    return lastButtonState;
}

sqInt ioMousePoint(void) {
    /* Return packed x/y: (x << 16) | y */
    return (lastMouseX << 16) | (lastMouseY & 0xFFFF);
}

/* Called from iosEvents.m to update mouse state */
void ios_updateMouseState(int x, int y, int buttons) {
    lastMouseX = x;
    lastMouseY = y;
    lastButtonState = buttons;
}

#pragma mark - Clipboard

sqInt clipboardSize(void) {
    @autoreleasepool {
        UIPasteboard *pasteboard = [UIPasteboard generalPasteboard];
        NSString *text = pasteboard.string;
        return text ? (sqInt)[text lengthOfBytesUsingEncoding:NSUTF8StringEncoding] : 0;
    }
}

sqInt clipboardReadIntoAt(sqInt count, sqInt byteArrayIndex, sqInt startIndex) {
    @autoreleasepool {
        UIPasteboard *pasteboard = [UIPasteboard generalPasteboard];
        NSString *text = pasteboard.string;
        if (!text) return 0;

        NSData *data = [text dataUsingEncoding:NSUTF8StringEncoding];
        sqInt len = MIN(count, (sqInt)[data length]);

        char *dest = (char*)byteArrayIndex + startIndex;
        memcpy(dest, [data bytes], len);

        return len;
    }
}

sqInt clipboardWriteFromAt(sqInt count, sqInt byteArrayIndex, sqInt startIndex) {
    @autoreleasepool {
        char *src = (char*)byteArrayIndex + startIndex;
        NSString *text = [[NSString alloc] initWithBytes:src
                                                  length:count
                                                encoding:NSUTF8StringEncoding];
        if (text) {
            [UIPasteboard generalPasteboard].string = text;
            return count;
        }
        return 0;
    }
}

#pragma mark - Miscellaneous

sqInt ioBeep(void) {
    /* Play system sound or haptic feedback */
    return 0;
}

sqInt ioDisablePowerManager(sqInt disableIfNonZero) {
    /* Prevent screen from sleeping if requested */
    dispatch_async(dispatch_get_main_queue(), ^{
        [[UIApplication sharedApplication] setIdleTimerDisabled:(disableIfNonZero != 0)];
    });
    return 1;
}

/* Drag and drop - not directly applicable on iOS */
char* dropRequestFileName(sqInt dropIndex) {
    return NULL;
}

sqInt dropRequestFileHandle(sqInt dropIndex) {
    return nilObject();
}

/* Profiling stubs */
void ioClearProfile(void) {}
long ioControlNewProfile(int on, unsigned long buffer_size) { return 0; }
void ioNewProfileStatus(sqInt *running, long *buffersize) {
    *running = 0;
    *buffersize = 0;
}
long ioNewProfileSamplesInto(void *sampleBuffer) { return 0; }

int plugInNotifyUser(char *msg) { return 0; }
int plugInTimeToReturn(void) { return 0; }

sqInt crashInThisOrAnotherThread(sqInt flags) { return 0; }

char* ioGetLogDirectory(void) {
    static char logDir[1024];
    @autoreleasepool {
        NSString *cachesPath = [NSSearchPathForDirectoriesInDomains(NSCachesDirectory,
                                                                     NSUserDomainMask,
                                                                     YES) firstObject];
        strncpy(logDir, [cachesPath UTF8String], sizeof(logDir) - 1);
    }
    return logDir;
}

sqInt ioSetLogDirectoryOfSize(void* lblIndex, sqInt sz) {
    return 1;
}
