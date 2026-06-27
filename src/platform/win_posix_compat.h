/*
 * win_posix_compat.h - POSIX shims that need <windows.h>, for the Windows
 * (clang/LLVM-MinGW) build of Primitives.cpp.  Included on ALL platforms
 * (the cross-platform timezone helpers below are used everywhere); the heavy
 * Win32 shims are gated on _WIN32.
 *
 * MinGW-w64 already supplies unistd/dirent/fcntl/pthread/sys-stat and most of
 * <sys/socket.h>'s API via <winsock2.h>, so this header only fills the genuine
 * gaps: getrusage, statvfs, uname, sysconf, lstat/symlink handling, and the
 * BSD struct-tm fields (tm_gmtoff/tm_zone) that don't exist on Windows.
 *
 * WIN32_LEAN_AND_MEAN keeps the collision-prone GDI/USER macros (Rectangle,
 * LoadImage, ...) out of the huge Primitives.cpp TU.  See
 * docs/windows-port-plan.md.
 */

#ifndef PHARO_WIN_POSIX_COMPAT_H
#define PHARO_WIN_POSIX_COMPAT_H

#include <ctime>
#include <cstddef>

// ===========================================================================
// Cross-platform timezone-offset accessors.  Windows' struct tm has no
// tm_gmtoff / tm_zone (BSD/glibc extensions); the time primitives call these
// instead of touching the fields.  On POSIX they are exact pass-throughs.
// ===========================================================================
static inline long pharo_tm_gmtoff(const struct tm* lt) {
#ifdef _WIN32
    // Reconstruct the instant from the local broken-down time, then return
    // (local - UTC) seconds = seconds EAST of UTC, matching tm_gmtoff.
    struct tm a = *lt; a.tm_isdst = -1;
    time_t t = mktime(&a);
    struct tm lo, gm;
    localtime_s(&lo, &t);
    gmtime_s(&gm, &t);
    lo.tm_isdst = 0; gm.tm_isdst = 0;
    return (long)(mktime(&lo) - mktime(&gm));
#else
    return lt->tm_gmtoff;
#endif
}

static inline const char* pharo_tm_zone(const struct tm* lt) {
#ifdef _WIN32
    _tzset();
    return (lt->tm_isdst > 0) ? _tzname[1] : _tzname[0];
#else
    return lt->tm_zone;
#endif
}

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>   // sockets, gethostname, struct timeval
#include <ws2tcpip.h>   // getaddrinfo, addrinfo, sockaddr_in6, inet_ntop
#include <windows.h>    // GetProcessTimes, GetDiskFreeSpaceExA, GetSystemInfo
#include <sys/types.h>  // ssize_t
#include <sys/stat.h>
#include <direct.h>     // _mkdir
#include <io.h>         // _chsize_s
#include <cstring>
#include <cstdint>
#include <cerrno>
#include <random>      // std::random_device for arc4random_buf

// ---- getrusage (process CPU time) via GetProcessTimes ----------------------
#define RUSAGE_SELF 0
struct rusage {
    struct timeval ru_utime;   // timeval comes from winsock2.h
    struct timeval ru_stime;
};
static inline int getrusage(int /*who*/, struct rusage* ru) {
    FILETIME cre, exi, ker, usr;
    if (!GetProcessTimes(GetCurrentProcess(), &cre, &exi, &ker, &usr)) return -1;
    auto toUsec = [](FILETIME ft) -> unsigned long long {
        ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
        return u.QuadPart / 10ULL;   // 100ns ticks -> microseconds
    };
    unsigned long long uu = toUsec(usr), ku = toUsec(ker);
    ru->ru_utime.tv_sec  = (long)(uu / 1000000ULL);
    ru->ru_utime.tv_usec = (long)(uu % 1000000ULL);
    ru->ru_stime.tv_sec  = (long)(ku / 1000000ULL);
    ru->ru_stime.tv_usec = (long)(ku % 1000000ULL);
    return 0;
}

// ---- statvfs (free disk space) via GetDiskFreeSpaceExA ---------------------
// f_frsize is 1 (bytes) so callers computing f_bavail * f_frsize get bytes.
struct statvfs {
    unsigned long f_bsize;
    unsigned long f_frsize;
    unsigned long long f_blocks;
    unsigned long long f_bfree;
    unsigned long long f_bavail;
};
static inline int statvfs(const char* path, struct statvfs* buf) {
    ULARGE_INTEGER avail, total, freeb;
    if (!GetDiskFreeSpaceExA(path, &avail, &total, &freeb)) return -1;
    buf->f_bsize  = 1;
    buf->f_frsize = 1;
    buf->f_blocks = total.QuadPart;
    buf->f_bfree  = freeb.QuadPart;
    buf->f_bavail = avail.QuadPart;
    return 0;
}

// ---- uname (OS identity string) --------------------------------------------
struct utsname {
    char sysname[65]; char nodename[65]; char release[65];
    char version[65]; char machine[65];
};
static inline int uname(struct utsname* u) {
    std::strcpy(u->sysname, "Windows");
    std::strcpy(u->release, "10.0");
    std::strcpy(u->version, "");
    std::strcpy(u->machine, "x86_64");
    DWORD n = sizeof(u->nodename) - 1;
    if (!GetComputerNameA(u->nodename, &n)) std::strcpy(u->nodename, "windows");
    return 0;
}

// ---- sysconf ---------------------------------------------------------------
#define _SC_PAGE_SIZE         30
#define _SC_PAGESIZE          30
#define _SC_NPROCESSORS_ONLN  84
#define _SC_PHYS_PAGES        85
static inline long sysconf(int name) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    switch (name) {
        case _SC_PAGE_SIZE:        return (long)si.dwPageSize;
        case _SC_NPROCESSORS_ONLN: return (long)si.dwNumberOfProcessors;
        case _SC_PHYS_PAGES: {
            MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms);
            if (!GlobalMemoryStatusEx(&ms)) return -1;
            return (long)(ms.ullTotalPhys / si.dwPageSize);
        }
        default: return -1;
    }
}

// ---- setenv (POSIX) via UCRT _putenv_s -------------------------------------
static inline int setenv(const char* name, const char* value, int /*overwrite*/) {
    return _putenv_s(name, value);
}

// ---- symlink handling: Windows milestone 1 treats nothing as a symlink -----
#define lstat stat
#ifndef S_ISLNK
#define S_ISLNK(m)  (0)
#endif
#ifndef S_ISFIFO
#define S_ISFIFO(m) (0)
#endif
static inline ssize_t readlink(const char* /*path*/, char* /*buf*/, size_t /*n*/) {
    errno = EINVAL;   // EINVAL == "not a symbolic link"
    return -1;
}

// ---- localtime_r via UCRT localtime_s ---------------------------------------
// POSIX form: struct tm* localtime_r(const time_t*, struct tm*) returns the
// out param (or null on failure).  Windows localtime_s has the args SWAPPED
// (struct tm* first) and returns errno_t.
static inline struct tm* localtime_r(const time_t* t, struct tm* result) {
    return (localtime_s(result, t) == 0) ? result : nullptr;
}

// ---- arc4random_buf via std::random_device (OS CSPRNG on Windows) -----------
static inline void arc4random_buf(void* buf, size_t n) {
    static thread_local std::random_device rd;   // non-deterministic OS entropy
    unsigned char* p = static_cast<unsigned char*>(buf);
    while (n > 0) {
        unsigned int r = rd();
        size_t take = (n < sizeof(r)) ? n : sizeof(r);
        std::memcpy(p, &r, take);
        p += take;
        n -= take;
    }
}

// ---- unsetenv (POSIX) via UCRT _putenv_s with empty value ------------------
// On Windows _putenv_s(name, "") removes the variable from the CRT environment.
static inline int unsetenv(const char* name) {
    return _putenv_s(name, "");
}

// ---- POSIX ownership: types + chown/lchown ---------------------------------
// Windows has no POSIX uid/gid ownership model; chown/lchown honestly fail with
// ENOSYS.  Callers (primitiveChangeOwner / primitiveSymlinkChangeOwner) already
// turn a non-zero return into a primitive Failure.
typedef int uid_t;
typedef int gid_t;
static inline int chown(const char* /*path*/, uid_t /*uid*/, gid_t /*gid*/) {
    errno = ENOSYS;
    return -1;
}
static inline int lchown(const char* /*path*/, uid_t /*uid*/, gid_t /*gid*/) {
    errno = ENOSYS;
    return -1;
}

#endif  // _WIN32
#endif  // PHARO_WIN_POSIX_COMPAT_H
