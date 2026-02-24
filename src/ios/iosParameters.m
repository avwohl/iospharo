/*
 * iosParameters.m - iOS Bundle Parameters for Pharo VM
 *
 * Reads VM configuration from Info.plist, similar to macOS parameters.m
 */

#import <Foundation/Foundation.h>
#include <sys/param.h>
#include "pharovm/pharo.h"
#include "pharovm/parameters/parameters.h"

EXPORT(void) fillParametersFromPList(VMParameters* parameters) {
    @autoreleasepool {
        NSBundle* mainBundle = [NSBundle mainBundle];
        NSDictionary* infoPlist = [mainBundle infoDictionary];

        /* Read image file path from Info.plist */
        NSString* imageFileName = infoPlist[@"PharoImageFile"];
        if (imageFileName) {
            /* Resolve relative to bundle or documents */
            NSString* imagePath;
            if ([imageFileName hasPrefix:@"/"]) {
                imagePath = imageFileName;
            } else {
                /* Look in Documents first, then bundle */
                NSString* documentsPath = [NSSearchPathForDirectoriesInDomains(
                    NSDocumentDirectory, NSUserDomainMask, YES) firstObject];
                NSString* docImagePath = [documentsPath stringByAppendingPathComponent:imageFileName];

                if ([[NSFileManager defaultManager] fileExistsAtPath:docImagePath]) {
                    imagePath = docImagePath;
                } else {
                    imagePath = [[mainBundle resourcePath] stringByAppendingPathComponent:imageFileName];
                }
            }

            parameters->imageFileName = strdup([imagePath UTF8String]);
            parameters->defaultImageFound = [[NSFileManager defaultManager] fileExistsAtPath:imagePath];
        }

        /* Read headless mode flag */
        NSNumber* headlessRef = infoPlist[@"PharoHeadless"];
        if (headlessRef) {
            parameters->isInteractiveSession = ![headlessRef boolValue];
        } else {
            /* Default to interactive on iOS */
            parameters->isInteractiveSession = true;
        }

        /* Read log level */
        NSNumber* logLevelRef = infoPlist[@"PharoLogLevel"];
        if (logLevelRef) {
            logLevel([logLevelRef intValue]);
        }

        /* Memory configuration */
        NSNumber* maxOldSpaceSizeRef = infoPlist[@"PharoMaxOldSpaceSize"];
        if (maxOldSpaceSizeRef) {
            parameters->maxOldSpaceSize = [maxOldSpaceSizeRef longLongValue];
        } else {
            /* Default to 512MB for iOS */
            parameters->maxOldSpaceSize = 512 * 1024 * 1024;
        }

        NSNumber* edenSizeRef = infoPlist[@"PharoEdenSize"];
        if (edenSizeRef) {
            parameters->edenSize = [edenSizeRef longLongValue];
        }

        /* Code zone size - force to 0 for interpreter-only mode on iOS.
         * iOS doesn't allow JIT compilation, so we skip code zone allocation
         * entirely by setting maxCodeSize to 0.
         */
        parameters->maxCodeSize = 0;

        /* Image parameters array */
        NSArray* imageParamsArray = infoPlist[@"PharoImageParameters"];
        if (imageParamsArray) {
            for (NSString* param in imageParamsArray) {
                if ([param isKindOfClass:[NSString class]]) {
                    char* paramStr = strdup([param UTF8String]);
                    vm_parameter_vector_insert_from(&parameters->imageParameters, 1, (const char**)&paramStr);
                }
            }
        }

    }
}

/* Get application directory (Documents on iOS) */
void fillApplicationDirectory(char* vmPath) {
    @autoreleasepool {
        NSString* documentsPath = [NSSearchPathForDirectoriesInDomains(
            NSDocumentDirectory, NSUserDomainMask, YES) firstObject];
        strlcpy(vmPath, [documentsPath UTF8String], MAXPATHLEN);
    }
}

/* Get bundle path */
void fillBundlePath(char* bundlePath) {
    @autoreleasepool {
        NSString* path = [[NSBundle mainBundle] bundlePath];
        strlcpy(bundlePath, [path UTF8String], MAXPATHLEN);
    }
}

/* Get resources path */
void fillResourcesPath(char* resourcesPath) {
    @autoreleasepool {
        NSString* path = [[NSBundle mainBundle] resourcePath];
        strlcpy(resourcesPath, [path UTF8String], MAXPATHLEN);
    }
}
