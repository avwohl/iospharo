/*
 * iosMemory.c
 *
 * iOS-specific memory allocation for the Pharo VM.
 * Replaces memoryUnix.c for iOS/Mac Catalyst builds.
 *
 * Key differences from Unix:
 * 1. No JIT memory (PROT_EXEC) due to iOS code signing restrictions
 * 2. Uses a contiguous memory arena with fixed offsets to ensure
 *    memory regions can be distinguished by address masks (required
 *    for the VM's fast space-identification checks)
 *
 * Memory Layout (within a 2GB arena):
 *   Offset 0x00000000: Code zone (for interpreter, non-executable)
 *   Offset 0x10000000: Stack pages (256MB)
 *   Offset 0x20000000: New space (512MB)
 *   Offset 0x40000000: Old space (1GB)
 *   Offset 0x60000000: Perm space (1.5GB)
 *
 * This layout ensures bits 28-30 differ between regions, allowing
 * the VM to use a simple mask to identify which space an address belongs to.
 */

#include "pharovm/pharo.h"
#include <sys/mman.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>

#define roundDownToPage(v) ((v)&pageMask)
#define roundUpToPage(v) (((v)+pageSize-1)&pageMask)

#if !defined(MAP_ANON)
# if defined(MAP_ANONYMOUS)
#   define MAP_ANON MAP_ANONYMOUS
# else
#   define MAP_ANON 0
# endif
#endif

#define MAP_PROT    (PROT_READ | PROT_WRITE)
#define MAP_FLAGS   (MAP_ANON | MAP_PRIVATE)

#define valign(x)   ((x) & pageMask)

#ifndef max
# define max(a, b)  (((a) > (b)) ? (a) : (b))
#endif
#ifndef min
# define min(a, b)  (((a) < (b)) ? (a) : (b))
#endif

static sqInt pageSize = 0;
static usqInt pageMask = 0;
int mmapErrno = 0;

/*
 * Memory Arena Management
 *
 * We allocate one large arena and carve out regions at fixed offsets.
 * This ensures regions have distinct high bits for mask-based identification.
 */

/* Arena configuration */
#define IOS_ARENA_SIZE          (2ULL * 1024 * 1024 * 1024)  /* 2GB total */
#define IOS_REGION_ALIGNMENT    (256ULL * 1024 * 1024)        /* 256MB alignment */

/* Fixed offsets within the arena - adjusted to give stack 768MB */
#define IOS_CODEZONE_OFFSET     (0x00000000ULL)  /* 0 */
#define IOS_STACK_OFFSET        (0x10000000ULL)  /* 256MB */
#define IOS_NEWSPACE_OFFSET     (0x40000000ULL)  /* 1GB - gives stack 768MB */
#define IOS_OLDSPACE_OFFSET     (0x50000000ULL)  /* 1.25GB */
#define IOS_PERMSPACE_OFFSET    (0x70000000ULL)  /* 1.75GB */

/* Arena state */
static char *iosArenaBase = NULL;
static usqInt iosArenaSize = 0;
static int iosAllocationCount = 0;  /* Tracks which allocation we're on */

/* Forward declarations */
static int ensureArenaAllocated(void);

static int ensureArenaAllocated(void) {
    if (iosArenaBase != NULL) {
        return 1;  /* Already allocated */
    }

    if (pageSize == 0) {
        pageSize = getpagesize();
        pageMask = ~(pageSize - 1);
    }

    /* Allocate the arena - let the OS choose where */
    iosArenaBase = mmap(NULL, IOS_ARENA_SIZE,
                        MAP_PROT, MAP_FLAGS,
                        -1, 0);

    if (iosArenaBase == MAP_FAILED) {
        mmapErrno = errno;
        logError("iOS: Failed to allocate memory arena (%llu bytes): %s\n",
                 (unsigned long long)IOS_ARENA_SIZE, strerror(errno));
        iosArenaBase = NULL;
        return 0;
    }

    iosArenaSize = IOS_ARENA_SIZE;
    logDebug("iOS: Memory arena allocated at %p, size %llu bytes\n",
             iosArenaBase, (unsigned long long)IOS_ARENA_SIZE);

    return 1;
}

void
sqMakeMemoryExecutableFromTo(unsigned long startAddr, unsigned long endAddr)
{
    /* No-op on iOS - we can't make memory executable */
}

void
sqMakeMemoryNotExecutableFromTo(unsigned long startAddr, unsigned long endAddr)
{
    /* No-op on iOS */
}

/*
 * JIT memory allocation for iOS.
 * iOS doesn't allow JIT compilation due to code signing restrictions.
 * We allocate regular read/write memory instead of executable memory.
 * This works because we're using interpreter-only mode (cointerp.c).
 */
void* allocateJITMemory(usqInt desiredSize, usqInt desiredPosition) {
    if (!ensureArenaAllocated()) {
        return NULL;
    }

    usqInt alignedSize = valign(max(desiredSize, 1));
    if (alignedSize < desiredSize) {
        alignedSize += pageSize;
    }

    /* Code zone goes at the beginning of the arena */
    char *result = iosArenaBase + IOS_CODEZONE_OFFSET;

    /* Verify we have enough space */
    if (alignedSize > IOS_STACK_OFFSET) {
        logError("iOS: Code zone size %lu exceeds available space\n",
                 (unsigned long)alignedSize);
        return NULL;
    }

    logDebug("iOS: Code zone (non-executable) at %p, size %lu bytes\n",
             result, (unsigned long)alignedSize);

    return result;
}

/*
 * Main memory allocation for Pharo VM memory spaces.
 *
 * The VM calls this function multiple times for different spaces:
 *   1. Stack pages
 *   2. New space
 *   3. Old space
 *   4. Perm space (optional)
 *
 * We return addresses at fixed offsets within our arena to ensure
 * they can be distinguished by the VM's mask-based checks.
 */
usqInt
sqAllocateMemory(usqInt minHeapSize, usqInt desiredHeapSize, usqInt desiredBaseAddress) {
    if (!ensureArenaAllocated()) {
        return 0;
    }

    usqInt heapLimit = valign(max(desiredHeapSize, 1));
    if (heapLimit < desiredHeapSize) {
        heapLimit += pageSize;
    }

    /* Determine which region this allocation is for based on call order */
    usqInt offset;
    const char *regionName;
    usqInt maxSize;

    /* Identify which region based on the requested address hint.
     * The VM passes these expected addresses:
     *   newSpace:   0x360000000  (14495514624)
     *   oldSpace:   0x10000000000 (1TB)
     *   permSpace:  0x20000000000 (2TB)
     *   stackPages: 0x300000000  (12884901888)
     */
    if (desiredBaseAddress == 0x360000000ULL || desiredBaseAddress == 14495514624ULL) {
        offset = IOS_NEWSPACE_OFFSET;
        maxSize = IOS_OLDSPACE_OFFSET - IOS_NEWSPACE_OFFSET;
        regionName = "new space";
    } else if (desiredBaseAddress == 0x10000000000ULL) {
        offset = IOS_OLDSPACE_OFFSET;
        maxSize = IOS_PERMSPACE_OFFSET - IOS_OLDSPACE_OFFSET;
        regionName = "old space";
    } else if (desiredBaseAddress == 0x20000000000ULL) {
        offset = IOS_PERMSPACE_OFFSET;
        maxSize = IOS_ARENA_SIZE - IOS_PERMSPACE_OFFSET;
        regionName = "perm space";
    } else if (desiredBaseAddress == 0x300000000ULL || desiredBaseAddress == 12884901888ULL) {
        offset = IOS_STACK_OFFSET;
        maxSize = IOS_NEWSPACE_OFFSET - IOS_STACK_OFFSET;
        regionName = "stack pages";
    } else {
        /* Unknown region or segment extension - allocate separately */
        logDebug("iOS: Unknown region request at %p, size %lu - allocating separately\n",
                 (void*)desiredBaseAddress, (unsigned long)heapLimit);
        char *heap = mmap(NULL, heapLimit, MAP_PROT, MAP_FLAGS, -1, 0);
        if (heap == MAP_FAILED) {
            logError("iOS: Failed to allocate memory segment\n");
            return 0;
        }
        logDebug("iOS: Segment allocated at %p, size %lu\n",
                 heap, (unsigned long)heapLimit);
        return (usqInt)heap;
    }

    /* Check if requested size fits in the region */
    if (heapLimit > maxSize) {
        logWarn("iOS: Requested %s size %lu exceeds max %lu, clamping\n",
                regionName, (unsigned long)heapLimit, (unsigned long)maxSize);
        heapLimit = maxSize;
        if (heapLimit < minHeapSize) {
            logError("iOS: Cannot allocate minimum %s size %lu\n",
                     regionName, (unsigned long)minHeapSize);
            return 0;
        }
    }

    char *result = iosArenaBase + offset;

    logDebug("iOS: Allocated %s at %p (offset 0x%llx), size %lu bytes\n",
             regionName, result, (unsigned long long)offset, (unsigned long)heapLimit);
    logDebug("iOS:   Requested base was %p, actual is %p\n",
             (void*)desiredBaseAddress, result);

    return (usqInt)result;
}

/* Deallocate a memory segment */
void
sqDeallocateMemorySegmentAtOfSize(void *addr, sqInt sz) {
    /* Check if this is within our arena */
    if (iosArenaBase != NULL &&
        (char*)addr >= iosArenaBase &&
        (char*)addr < (iosArenaBase + iosArenaSize)) {
        /* Within arena - we don't actually deallocate, just log */
        logDebug("iOS: Ignoring deallocation of arena region %p (size %ld)\n",
                 addr, (long)sz);
        return;
    }

    /* External allocation (segment extension) - actually deallocate */
    if (munmap(addr, sz) < 0) {
        logWarn("iOS: munmap(%p, %ld) failed: %s\n", addr, (long)sz, strerror(errno));
    }
}
