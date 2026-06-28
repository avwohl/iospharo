#pragma once
//
// winsock_compat.h - BSD-sockets vs winsock2 compatibility for SocketPlugin.cpp.
//
// SocketPlugin.cpp is written against BSD sockets.  winsock2 is BSD-compatible
// but differs in a handful of spots (header names, closesocket vs close,
// ioctlsocket vs fcntl, WSAGetLastError vs errno, SOCKET vs int fd, SD_* vs
// SHUT_*).  This header papers over those so the plugin compiles on both:
//   - On POSIX every alias resolves to the native name, so the plugin is
//     byte-identical to the original BSD build.
//   - On Windows each maps to the winsock2 equivalent.
//
// IMPORTANT: this is included FIRST in SocketPlugin.cpp so winsock2.h precedes
// any windows.h (including winsock2.h after windows.h pulls the legacy winsock.h
// and breaks the build).  WSAStartup is done once at VM init (windows.cpp); the
// plugin must NOT call it again.
//

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

// shutdown() "how" codes
#ifndef SHUT_RD
#define SHUT_RD   SD_RECEIVE
#endif
#ifndef SHUT_WR
#define SHUT_WR   SD_SEND
#endif
#ifndef SHUT_RDWR
#define SHUT_RDWR SD_BOTH
#endif

// winsock sockets are made non-blocking via ioctlsocket(FIONBIO); there is no
// per-call MSG_DONTWAIT, so make it a no-op flag (the socket is already O_NONBLOCK-
// equivalent, so MSG_PEEK | MSG_DONTWAIT degrades to MSG_PEEK).
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif

// Socket error reporting lives in WSAGetLastError(), not errno.
#define SOCK_LAST_ERROR   (WSAGetLastError())
#define SOCK_EWOULDBLOCK  WSAEWOULDBLOCK
#define SOCK_EAGAIN       WSAEWOULDBLOCK
#define SOCK_EINPROGRESS  WSAEWOULDBLOCK   // non-blocking connect reports WSAEWOULDBLOCK
#define SOCK_ECONNRESET   WSAECONNRESET
#define SOCK_ECONNABORTED WSAECONNABORTED

static inline int sockClose(SOCKET fd) { return closesocket(fd); }
static inline int sockSetNonBlocking(SOCKET fd) {
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode);   // 0 on success
}

// winsock get/setsockopt take a (const) char* optval and an int optlen, where
// BSD uses void* and socklen_t.  Wrap them so the BSD-style call sites compile
// unchanged.  The macro name is re-used but the body calls ::getsockopt, so it
// expands only once (no recursion).
#define getsockopt(s, lvl, opt, val, len) \
    (::getsockopt((s), (lvl), (opt), (char*)(val), (len)))
#define setsockopt(s, lvl, opt, val, len) \
    (::setsockopt((s), (lvl), (opt), (const char*)(val), (int)(len)))

#else  // ---------------------------------------------------------------- POSIX

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

typedef int SOCKET;
#ifndef INVALID_SOCKET
#define INVALID_SOCKET (-1)
#endif
#ifndef SOCKET_ERROR
#define SOCKET_ERROR (-1)
#endif

#define SOCK_LAST_ERROR   errno
#define SOCK_EWOULDBLOCK  EWOULDBLOCK
#define SOCK_EAGAIN       EAGAIN
#define SOCK_EINPROGRESS  EINPROGRESS
#define SOCK_ECONNRESET   ECONNRESET
#define SOCK_ECONNABORTED ECONNABORTED

static inline int sockClose(int fd) { return ::close(fd); }
static inline int sockSetNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);   // 0 on success
}

#endif
