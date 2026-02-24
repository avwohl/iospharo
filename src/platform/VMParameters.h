/*
 * VMParameters.h - VM initialization parameters
 *
 * Shared between iOSBridge.cpp, test_ios_bridge.cpp, and the Swift bridging header.
 */

#ifndef VM_PARAMETERS_H
#define VM_PARAMETERS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif

#endif /* VM_PARAMETERS_H */
