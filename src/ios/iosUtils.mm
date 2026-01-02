/*
 * iosUtils.mm - iOS Utilities for Pharo VM
 *
 * Objective-C++ utility functions for iOS platform support.
 */

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

extern "C" {
    #include "pharovm/pharo.h"

    void fillApplicationDirectory(char* vmPath);
    void fillBundlePath(char* bundlePath);
    void fillResourcesPath(char* resourcesPath);
}

/* Get the Documents directory path */
void fillApplicationDirectory(char* vmPath) {
    @autoreleasepool {
        NSArray* paths = NSSearchPathForDirectoriesInDomains(
            NSDocumentDirectory, NSUserDomainMask, YES);
        NSString* documentsPath = [paths firstObject];
        strcpy(vmPath, [documentsPath UTF8String]);
    }
}

/* Get temporary directory */
extern "C" char* ioGetTempDirectory(void) {
    static char tempDir[1024];
    @autoreleasepool {
        NSString* tmpPath = NSTemporaryDirectory();
        strncpy(tempDir, [tmpPath UTF8String], sizeof(tempDir) - 1);
    }
    return tempDir;
}

/* Check if running on iPad */
extern "C" int iosIsIPad(void) {
    return (UI_USER_INTERFACE_IDIOM() == UIUserInterfaceIdiomPad) ? 1 : 0;
}

/* Get device model name */
extern "C" const char* iosGetDeviceModel(void) {
    static char model[256];
    @autoreleasepool {
        NSString* modelName = [[UIDevice currentDevice] model];
        strncpy(model, [modelName UTF8String], sizeof(model) - 1);
    }
    return model;
}

/* Get iOS version string */
extern "C" const char* iosGetSystemVersion(void) {
    static char version[64];
    @autoreleasepool {
        NSString* sysVersion = [[UIDevice currentDevice] systemVersion];
        strncpy(version, [sysVersion UTF8String], sizeof(version) - 1);
    }
    return version;
}

/* Open URL in Safari */
extern "C" int iosOpenURL(const char* urlString) {
    @autoreleasepool {
        NSString* urlStr = [NSString stringWithUTF8String:urlString];
        NSURL* url = [NSURL URLWithString:urlStr];
        if (url) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [[UIApplication sharedApplication] openURL:url
                                                   options:@{}
                                         completionHandler:nil];
            });
            return 1;
        }
    }
    return 0;
}

/* Show alert dialog */
extern "C" void iosShowAlert(const char* title, const char* message) {
    @autoreleasepool {
        NSString* titleStr = [NSString stringWithUTF8String:title];
        NSString* messageStr = [NSString stringWithUTF8String:message];

        dispatch_async(dispatch_get_main_queue(), ^{
            UIAlertController* alert = [UIAlertController
                alertControllerWithTitle:titleStr
                                 message:messageStr
                          preferredStyle:UIAlertControllerStyleAlert];

            UIAlertAction* okAction = [UIAlertAction
                actionWithTitle:@"OK"
                          style:UIAlertActionStyleDefault
                        handler:nil];

            [alert addAction:okAction];

            /* Find the root view controller to present from */
            UIWindowScene* windowScene = nil;
            for (UIScene* scene in [[UIApplication sharedApplication] connectedScenes]) {
                if ([scene isKindOfClass:[UIWindowScene class]]) {
                    windowScene = (UIWindowScene*)scene;
                    break;
                }
            }

            UIWindow* window = windowScene.windows.firstObject;
            UIViewController* rootVC = window.rootViewController;
            [rootVC presentViewController:alert animated:YES completion:nil];
        });
    }
}

/* Haptic feedback */
extern "C" void iosHapticFeedback(int style) {
    dispatch_async(dispatch_get_main_queue(), ^{
        UIImpactFeedbackGenerator* generator;

        switch (style) {
            case 0:
                generator = [[UIImpactFeedbackGenerator alloc]
                    initWithStyle:UIImpactFeedbackStyleLight];
                break;
            case 1:
                generator = [[UIImpactFeedbackGenerator alloc]
                    initWithStyle:UIImpactFeedbackStyleMedium];
                break;
            case 2:
                generator = [[UIImpactFeedbackGenerator alloc]
                    initWithStyle:UIImpactFeedbackStyleHeavy];
                break;
            default:
                generator = [[UIImpactFeedbackGenerator alloc]
                    initWithStyle:UIImpactFeedbackStyleMedium];
        }

        [generator prepare];
        [generator impactOccurred];
    });
}
