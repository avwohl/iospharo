#ifndef PHAROVM_CONFIG_H
#define PHAROVM_CONFIG_H

#pragma once

/* iOS-specific Pharo VM configuration */

/* Common configurations */
#define VM_NAME "Pharo"
#define DEFAULT_IMAGE_NAME "Pharo.image"

/* Availability of Functions */
#define HAVE_DIRENT_H
#define HAVE_UNISTD_H 1
#define HAVE_SYS_DIR_H
#define HAVE_SYS_FILIO_H
#define HAVE_SYS_TIME_H
/* execinfo.h not available on iOS */
/* #undef HAVE_EXECINFO_H */

#define HAVE_LIBDL
#define HAVE_TM_GMTOFF

/* architecture - iOS specific */
#ifdef TARGET_OS_IPHONE
#define OS_TYPE "iOS"
#else
#define OS_TYPE "Mac OS"
#endif

#define VM_HOST "iOS"
#define VM_HOST_OS "iOS"
#define VM_TARGET "iOS"
#define VM_TARGET_OS "iOS"

/* Compile-time CPU detection */
#if defined(__arm64__) || defined(__aarch64__)
#define VM_HOST_CPU "arm64"
#define VM_TARGET_CPU "arm64"
#elif defined(__x86_64__)
#define VM_HOST_CPU "x86_64"
#define VM_TARGET_CPU "x86_64"
#else
#define VM_HOST_CPU "unknown"
#define VM_TARGET_CPU "unknown"
#endif

/* widths of primitive types (64-bit) */
#define SIZEOF_INT 4
#define SIZEOF_LONG 8
#define SIZEOF_LONG_LONG 8
#define SIZEOF_VOID_P 8

#define squeakInt64 long

/* UUID support */
#define HAVE_UUID_UUID_H

/* iOS runs VM in main thread (no worker thread) */
/* #undef PHARO_VM_IN_WORKER_THREAD */

/* Build info */
#define VM_BUILD_STRING VM_NAME " iOS built on " __DATE__ " " __TIME__ " Compiler: " __VERSION__
#define COMPILER_VERSION __VERSION__
#define VM_BUILD_SOURCE_STRING "iOS-iospharo"

#define ALWAYS_INTERACTIVE OFF

/* VM_LABEL does nothing */
#define VM_LABEL(foo) 0

/* iOS-specific: No JIT, interpreter only */
#ifndef COGVM
#define COGVM 0
#endif

#ifndef STACKVM
#define STACKVM 1
#endif

/* iOS ASLR - addresses are not fixed */
#define IOS_ASLR_COMPATIBLE 1

#endif /* PHAROVM_CONFIG_H */
