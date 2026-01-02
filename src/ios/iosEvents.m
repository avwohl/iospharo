/*
 * iosEvents.m - iOS Event Handling for Pharo VM
 *
 * Manages the event queue between Swift touch/keyboard input
 * and the Pharo VM event system.
 */

#import <Foundation/Foundation.h>
#include "pharovm/pharo.h"
#include "iospharo/iosDisplay.h"
#include <pthread.h>

/* Event queue configuration */
#define MAX_EVENTS 256

/* Event queue */
static sqInputEvent eventQueue[MAX_EVENTS];
static volatile int eventQueueHead = 0;
static volatile int eventQueueTail = 0;
static pthread_mutex_t eventMutex = PTHREAD_MUTEX_INITIALIZER;

/* Input semaphore for signaling VM */
static sqInt inputSemaphoreIndex = 0;

/* External declaration from iosDisplay.m */
extern void ios_updateMouseState(int x, int y, int buttons);

#pragma mark - Input Semaphore

sqInt ioSetInputSemaphore(sqInt semaIndex) {
    inputSemaphoreIndex = semaIndex;
    return 1;
}

void ioSignalInputEvent(void) {
    if (inputSemaphoreIndex > 0) {
        signalSemaphoreWithIndex(inputSemaphoreIndex);
    }
}

#pragma mark - Event Queue Management

static int eventQueueIsFull(void) {
    return ((eventQueueHead + 1) % MAX_EVENTS) == eventQueueTail;
}

static int eventQueueIsEmpty(void) {
    return eventQueueHead == eventQueueTail;
}

static sqInputEvent* eventQueueNextSlot(void) {
    if (eventQueueIsFull()) {
        return NULL;
    }
    sqInputEvent* event = &eventQueue[eventQueueHead];
    eventQueueHead = (eventQueueHead + 1) % MAX_EVENTS;
    return event;
}

#pragma mark - Touch Events (called from Swift)

void ios_queueTouchEvent(int type, int x, int y, int buttons, int modifiers) {
    pthread_mutex_lock(&eventMutex);

    sqInputEvent* slot = eventQueueNextSlot();
    if (slot) {
        sqMouseEvent* evt = (sqMouseEvent*)slot;

        evt->type = EventTypeMouse;
        evt->timeStamp = ioMSecs() & MillisecondClockMask;
        evt->x = x;
        evt->y = y;
        evt->buttons = buttons;
        evt->modifiers = modifiers;
        evt->nrClicks = 0;
        evt->windowIndex = 0;

        /* Set click count based on event type */
        switch (type) {
            case IOS_TOUCH_DOWN:
                evt->nrClicks = 1;
                break;
            case IOS_TOUCH_MOVED:
                evt->nrClicks = 0;
                break;
            case IOS_TOUCH_UP:
            case IOS_TOUCH_CANCELLED:
                evt->nrClicks = 0;
                break;
        }

        /* Update global mouse state */
        ios_updateMouseState(x, y, buttons);
    }

    pthread_mutex_unlock(&eventMutex);

    /* Signal the VM that an event is available */
    ioSignalInputEvent();
}

#pragma mark - Keyboard Events (called from Swift)

void ios_queueKeyEvent(int charCode, int pressCode, int modifiers) {
    pthread_mutex_lock(&eventMutex);

    sqInputEvent* slot = eventQueueNextSlot();
    if (slot) {
        sqKeyboardEvent* evt = (sqKeyboardEvent*)slot;

        evt->type = EventTypeKeyboard;
        evt->timeStamp = ioMSecs() & MillisecondClockMask;
        evt->charCode = charCode;
        evt->pressCode = pressCode;
        evt->modifiers = modifiers;
        evt->utf32Code = charCode;
        evt->reserved1 = 0;
        evt->windowIndex = 0;
    }

    pthread_mutex_unlock(&eventMutex);

    ioSignalInputEvent();
}

#pragma mark - VM Event Interface

sqInt ioProcessEvents(void) {
    /* Poll for async I/O events */
    aioPoll(0);
    return 0;
}

sqInt ioGetNextEvent(sqInputEvent *evt) {
    sqInt result = 0;

    pthread_mutex_lock(&eventMutex);

    if (!eventQueueIsEmpty()) {
        *evt = eventQueue[eventQueueTail];
        eventQueueTail = (eventQueueTail + 1) % MAX_EVENTS;
        result = 1;
    }

    pthread_mutex_unlock(&eventMutex);

    return result;
}

sqInt ioGetKeystroke(void) {
    pthread_mutex_lock(&eventMutex);

    sqInt result = -1; /* No keystroke */

    /* Search queue for keyboard event */
    int idx = eventQueueTail;
    while (idx != eventQueueHead) {
        sqInputEvent* evt = &eventQueue[idx];
        if (evt->type == EventTypeKeyboard) {
            sqKeyboardEvent* keyEvt = (sqKeyboardEvent*)evt;
            result = keyEvt->charCode;

            /* Remove this event from queue by shifting */
            int next = (idx + 1) % MAX_EVENTS;
            while (next != eventQueueHead) {
                eventQueue[idx] = eventQueue[next];
                idx = next;
                next = (next + 1) % MAX_EVENTS;
            }
            eventQueueHead = (eventQueueHead - 1 + MAX_EVENTS) % MAX_EVENTS;
            break;
        }
        idx = (idx + 1) % MAX_EVENTS;
    }

    pthread_mutex_unlock(&eventMutex);

    return result;
}

sqInt ioPeekKeystroke(void) {
    pthread_mutex_lock(&eventMutex);

    sqInt result = -1;

    int idx = eventQueueTail;
    while (idx != eventQueueHead) {
        sqInputEvent* evt = &eventQueue[idx];
        if (evt->type == EventTypeKeyboard) {
            sqKeyboardEvent* keyEvt = (sqKeyboardEvent*)evt;
            result = keyEvt->charCode;
            break;
        }
        idx = (idx + 1) % MAX_EVENTS;
    }

    pthread_mutex_unlock(&eventMutex);

    return result;
}

#pragma mark - Event Type Definitions

/*
 * These must match the VM's expectations.
 * Defined here to ensure consistency.
 */

#ifndef EventTypeNone
#define EventTypeNone       0
#define EventTypeMouse      1
#define EventTypeKeyboard   2
#define EventTypeDragDropFiles 3
#define EventTypeMenu       4
#define EventTypeWindow     5
#define EventTypeComplex    6
#define EventTypeMouseWheel 7
#endif
