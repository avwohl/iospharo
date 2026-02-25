/*
 * DeviceLog.hpp - File-based logging visible on real iOS devices
 *
 * fprintf(stderr) is invisible on real devices. This writes to a log file
 * in the app's Documents directory (accessible via Files app) and also
 * sends to os_log (visible in Xcode Console when device is connected).
 */

#ifndef PHARO_DEVICE_LOG_HPP
#define PHARO_DEVICE_LOG_HPP

#include <cstdio>
#include <cstdarg>
#include <cstring>

#ifdef __APPLE__
#include <TargetConditionals.h>
#include <os/log.h>
#include <CoreFoundation/CoreFoundation.h>
#endif

// Device log: writes to stderr, os_log, AND a file (for real device visibility)
inline void vmLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
inline void vmLog(const char* fmt, ...) {
    va_list args;

    // Always write to stderr (works on simulator and Mac Catalyst)
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

#ifdef __APPLE__
    // os_log: visible in Xcode Console when device is connected
    {
        char buf[1024];
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        // Strip trailing newline for os_log
        size_t len = strlen(buf);
        if (len > 0 && buf[len-1] == '\n') buf[len-1] = '\0';
        os_log(OS_LOG_DEFAULT, "PharoVM: %{public}s", buf);
    }
#endif

    // Also write to a log file (works on real devices via Files app)
    static FILE* logFile = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;

        // Get build number from Info.plist
        const char* buildNum = "0";
#ifdef __APPLE__
        CFBundleRef bundle = CFBundleGetMainBundle();
        if (bundle) {
            CFStringRef buildRef = (CFStringRef)CFBundleGetValueForInfoDictionaryKey(
                bundle, CFSTR("CFBundleVersion"));
            if (buildRef) {
                static char buildBuf[32];
                if (CFStringGetCString(buildRef, buildBuf, sizeof(buildBuf), kCFStringEncodingUTF8)) {
                    buildNum = buildBuf;
                }
            }
        }

        // Prefer Documents/ which is accessible via Files app (UIFileSharingEnabled)
        const char* home = getenv("HOME");
        if (home) {
            char path[1024];
            snprintf(path, sizeof(path), "%s/Documents/vm_debug_build%s.log", home, buildNum);
            logFile = fopen(path, "w");
        }
        // Fallback to tmp/
        if (!logFile) {
            const char* tmpDir = getenv("TMPDIR");
            if (tmpDir) {
                char path[1024];
                snprintf(path, sizeof(path), "%s/vm_debug_build%s.log", tmpDir, buildNum);
                logFile = fopen(path, "w");
            }
        }
#endif
        if (logFile) {
            setvbuf(logFile, nullptr, _IONBF, 0);  // Unbuffered
        }
    }

    if (logFile) {
        va_start(args, fmt);
        vfprintf(logFile, fmt, args);
        va_end(args);
    }
}

#endif
