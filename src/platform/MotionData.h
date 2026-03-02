/*
 * MotionData.h — Shared C struct for Core Motion sensor data.
 *
 * Included by both the Swift bridging header (CoreMotionManager.swift writes)
 * and the C++ VM (Primitives.cpp reads). Single writer / single reader,
 * so no locking is needed — just a plain memcpy on each side.
 */

#ifndef MOTION_DATA_H
#define MOTION_DATA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Availability bitmask bits */
#define MOTION_HAS_ACCELEROMETER  (1 << 0)
#define MOTION_HAS_GYROSCOPE      (1 << 1)
#define MOTION_HAS_MAGNETOMETER   (1 << 2)
#define MOTION_HAS_DEVICE_MOTION  (1 << 3)

typedef struct {
    double accelerometerX;   /* gravity + user acceleration, in G */
    double accelerometerY;
    double accelerometerZ;
    double gyroX;            /* rotation rate, rad/s */
    double gyroY;
    double gyroZ;
    double magnetometerX;    /* magnetic field, microteslas */
    double magnetometerY;
    double magnetometerZ;
    double roll;             /* attitude, radians */
    double pitch;
    double yaw;
    double timestamp;        /* seconds since device boot */
    int32_t available;       /* bitmask of MOTION_HAS_* */
    int32_t active;          /* 1 if updates are running */
} MotionData;

/* Swift calls this to push the latest sample */
void motion_update(const MotionData* data);

/* C++ primitive calls this to read the latest sample */
void motion_getData(MotionData* out);

/* Returns availability bitmask (which sensors exist on this device) */
int motion_isAvailable(void);

/* Request start/stop from the C++ side (Swift polls the flag) */
void motion_requestStart(void);
void motion_requestStop(void);

/* Check if a start/stop has been requested (Swift calls this) */
int motion_startRequested(void);
int motion_stopRequested(void);

#ifdef __cplusplus
}
#endif

#endif /* MOTION_DATA_H */
