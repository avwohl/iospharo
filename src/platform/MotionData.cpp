/*
 * MotionData.cpp — Shared motion data between Swift (writer) and C++ (reader).
 *
 * Single writer (CoreMotionManager on its update queue) and single reader
 * (VM thread via primitives), so a plain memcpy is safe. The start/stop
 * request flags use std::atomic for cross-thread visibility.
 */

#include "MotionData.h"
#include <atomic>
#include <cstring>

static MotionData gMotionData = {};
static std::atomic<bool> gStartRequested{false};
static std::atomic<bool> gStopRequested{false};

void motion_update(const MotionData* data) {
    std::memcpy(&gMotionData, data, sizeof(MotionData));
}

void motion_getData(MotionData* out) {
    std::memcpy(out, &gMotionData, sizeof(MotionData));
}

int motion_isAvailable(void) {
    return gMotionData.available;
}

void motion_requestStart(void) {
    gStartRequested.store(true, std::memory_order_release);
}

void motion_requestStop(void) {
    gStopRequested.store(true, std::memory_order_release);
}

int motion_startRequested(void) {
    return gStartRequested.exchange(false, std::memory_order_acq_rel) ? 1 : 0;
}

int motion_stopRequested(void) {
    return gStopRequested.exchange(false, std::memory_order_acq_rel) ? 1 : 0;
}
