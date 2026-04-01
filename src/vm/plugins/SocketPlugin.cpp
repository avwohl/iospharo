/*
 * SocketPlugin.cpp - TCP/UDP socket primitives for Pharo VM
 *
 * Non-blocking POSIX sockets with a background I/O monitor thread that
 * signals Pharo semaphores when sockets become readable/writable or
 * when connections complete.
 */

#include "SocketPlugin.h"
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

// =====================================================================
// Private socket structure (pointed to by SQSocket.privateSocketPtr)
// =====================================================================

struct PrivateSocket {
    int fd;              // POSIX socket file descriptor
    int connSema;        // connection semaphore index
    int readSema;        // read semaphore index
    int writeSema;       // write semaphore index
    int sockState;       // one of SOCK_* constants
    int sockError;       // errno after socket error
    bool writeSignaled;  // true after write-ready signaled; reset on EAGAIN
    bool eofDetected;    // true after I/O thread sees MSG_PEEK return 0 (FIN)
};

// =====================================================================
// Global state
// =====================================================================

static VirtualMachine* vm = nullptr;
static int gSessionID = 1;

// Active socket tracking for I/O monitor thread
static std::mutex gSocketMutex;
static std::vector<PrivateSocket*> gActiveSockets;
static std::vector<PrivateSocket*> gDeleteQueue; // deferred deletion

// I/O monitor thread
static std::thread gIOThread;
static std::atomic<bool> gIORunning{false};
static int gWakePipe[2] = {-1, -1};  // self-pipe to wake select()

// =====================================================================
// Helper: set socket to non-blocking
// =====================================================================

static bool setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

// =====================================================================
// Helper: wake the I/O monitor thread
// =====================================================================

static void wakeIOThread() {
    if (gWakePipe[1] >= 0) {
        char c = 'w';
        (void)write(gWakePipe[1], &c, 1);
    }
}

// =====================================================================
// Helper: register/unregister socket for I/O monitoring
// =====================================================================

static void registerSocket(PrivateSocket* ps) {
    std::lock_guard<std::mutex> lock(gSocketMutex);
    gActiveSockets.push_back(ps);
    wakeIOThread();
}

static void unregisterSocket(PrivateSocket* ps) {
    std::lock_guard<std::mutex> lock(gSocketMutex);
    auto it = std::find(gActiveSockets.begin(), gActiveSockets.end(), ps);
    if (it != gActiveSockets.end()) {
        gActiveSockets.erase(it);
    }
    wakeIOThread();
}

// =====================================================================
// I/O monitor thread: select() loop with semaphore signaling
// =====================================================================

static void ioMonitorLoop() {
    while (gIORunning.load()) {
        fd_set readfds, writefds;
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);

        int maxfd = gWakePipe[0];
        FD_SET(gWakePipe[0], &readfds);

        // Snapshot active sockets and drain deletion queue under lock
        std::vector<PrivateSocket*> snapshot;
        {
            std::lock_guard<std::mutex> lock(gSocketMutex);
            // Free sockets that were destroyed since last iteration
            for (auto* ps : gDeleteQueue) delete ps;
            gDeleteQueue.clear();
            snapshot = gActiveSockets;
        }

        for (auto* ps : snapshot) {
            if (ps->fd < 0) continue;
            if (ps->sockState == SOCK_WAITING_FOR_CONNECTION) {
                // Monitor for connect completion (writable = connected)
                FD_SET(ps->fd, &writefds);
                if (ps->fd > maxfd) maxfd = ps->fd;
            } else if (ps->sockState == SOCK_CONNECTED) {
                // Always monitor for readable data — even after EOF, Pharo's
                // SSL layer may have buffered data that needs draining
                FD_SET(ps->fd, &readfds);
                if (ps->fd > maxfd) maxfd = ps->fd;
                // Only monitor writability if EOF hasn't been seen
                if (!ps->eofDetected) {
                    FD_SET(ps->fd, &writefds);
                }
            }
        }

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000; // 100ms timeout

        int ready = select(maxfd + 1, &readfds, &writefds, nullptr, &tv);
        if (ready <= 0) continue;

        // Drain wake pipe
        if (FD_ISSET(gWakePipe[0], &readfds)) {
            char buf[64];
            (void)read(gWakePipe[0], buf, sizeof(buf));
        }

        // Process socket events
        for (auto* ps : snapshot) {
            if (ps->fd < 0) continue;

            if (ps->sockState == SOCK_WAITING_FOR_CONNECTION && FD_ISSET(ps->fd, &writefds)) {
                // Connect completed — check for errors
                int err = 0;
                socklen_t len = sizeof(err);
                getsockopt(ps->fd, SOL_SOCKET, SO_ERROR, &err, &len);
                if (err == 0) {
                    ps->sockState = SOCK_CONNECTED;
                } else {
                    ps->sockError = err;
                    ps->sockState = SOCK_UNCONNECTED;
#ifdef DEBUG
                    fprintf(stderr, "[SOCK] Connection failed: err=%d (%s) fd=%d\n",
                            err, strerror(err), ps->fd);
#endif
                }
                if (ps->connSema > 0 && vm) {
                    vm->signalSemaphoreWithIndex(ps->connSema);
                }
                if (ps->writeSema > 0 && vm) {
                    vm->signalSemaphoreWithIndex(ps->writeSema);
                }
            }

            if (ps->sockState == SOCK_CONNECTED) {
                if (FD_ISSET(ps->fd, &readfds)) {
                    if (!ps->eofDetected) {
                        // First time seeing readability — peek to detect EOF
                        char peek;
                        int n = (int)recv(ps->fd, &peek, 1, MSG_PEEK);
                        if (n == 0) {
                            // EOF (FIN received) — DON'T change sockState here.
                            // The recv() primitive will set SOCK_OTHER_END_CLOSED
                            // when Pharo actually reads 0 bytes. This prevents a
                            // race where SSL-buffered data is lost because Pharo
                            // sees isConnected=false before draining the SSL layer.
                            ps->eofDetected = true;
                            if (ps->connSema > 0 && vm) {
                                vm->signalSemaphoreWithIndex(ps->connSema);
                            }
                        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                            // Actual socket error — set state immediately
                            ps->sockError = errno;
                            ps->sockState = SOCK_OTHER_END_CLOSED;
                            if (ps->connSema > 0 && vm) {
                                vm->signalSemaphoreWithIndex(ps->connSema);
                            }
                        }
                    }
                    // Always signal readSema — after EOF, Pharo's SSL layer
                    // still has buffered data that needs draining via recv()
                    if (ps->readSema > 0 && vm) {
                        vm->signalSemaphoreWithIndex(ps->readSema);
                    }
                }
                if (!ps->eofDetected && FD_ISSET(ps->fd, &writefds) && !ps->writeSignaled) {
                    if (ps->writeSema > 0 && vm) {
                        vm->signalSemaphoreWithIndex(ps->writeSema);
                    }
                    ps->writeSignaled = true;
                }
            }
        }
    }
}

// =====================================================================
// Init / Shutdown
// =====================================================================

void socketPluginInit() {
    if (gIORunning.load()) return;

    // Create self-pipe for waking select()
    if (pipe(gWakePipe) < 0) {
        fprintf(stderr, "[SOCK] socketPluginInit: pipe() failed errno=%d (%s)\n",
                errno, strerror(errno));
        return;
    }
    setNonBlocking(gWakePipe[0]);
    setNonBlocking(gWakePipe[1]);

    gIORunning.store(true);
    gIOThread = std::thread(ioMonitorLoop);
    // Detach so std::thread destructor won't call std::terminate() if
    // the process exits (via exit()) before socketPluginShutdown runs.
    gIOThread.detach();

#ifdef DEBUG
    fprintf(stderr, "[SOCK] socketPluginInit OK (pipe=%d/%d, I/O thread started)\n",
            gWakePipe[0], gWakePipe[1]);
#endif
}

void socketPluginShutdown() {
    if (!gIORunning.load()) return;

    gIORunning.store(false);
    wakeIOThread();
    // Thread is detached, so just give it a moment to notice the flag
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    if (gWakePipe[0] >= 0) { close(gWakePipe[0]); gWakePipe[0] = -1; }
    if (gWakePipe[1] >= 0) { close(gWakePipe[1]); gWakePipe[1] = -1; }

    // Clean up any remaining sockets
    {
        std::lock_guard<std::mutex> lock(gSocketMutex);
        for (auto* ps : gActiveSockets) {
            if (ps->fd >= 0) close(ps->fd);
            delete ps;
        }
        gActiveSockets.clear();
        for (auto* ps : gDeleteQueue) delete ps;
        gDeleteQueue.clear();
    }

    // Bump session ID so stale socket handles from previous image are rejected
    gSessionID++;

    // Clear the VM pointer — will be set again by setInterpreter on next launch
    vm = nullptr;
}

// =====================================================================
// Helper: extract SQSocket pointer from a Pharo ByteArray oop
// Returns nullptr and calls primitiveFail on error.
// =====================================================================

static SQSocket* socketFromOop(sqInt socketOop) {
    if (!vm->isBytes(socketOop) || vm->byteSizeOf(socketOop) != sizeof(SQSocket)) {
        vm->primitiveFailFor(PrimErrBadArgument);
        return nullptr;
    }
    SQSocket* s = (SQSocket*)vm->firstIndexableField(socketOop);
    if (s->sessionID != gSessionID) {
        vm->primitiveFailFor(PrimErrBadArgument);
        return nullptr;
    }
    return s;
}

static PrivateSocket* privateSocketFrom(sqInt socketOop) {
    SQSocket* s = socketFromOop(socketOop);
    if (!s) return nullptr;
    return (PrivateSocket*)s->privateSocketPtr;
}

// =====================================================================
// PRIMITIVES
// =====================================================================

// primitiveSocketCreate3Semaphores
// Stack: receiver, netType, socketType, recvBufSize, sendBufSize,
//        semaIndex, readSemaIndex, writeSemaIndex
// Returns: socketHandle (ByteArray of sizeof(SQSocket))
extern "C" sqInt sp_primitiveSocketCreate3Semaphores(void) {
    sqInt writeSemaIndex = vm->stackIntegerValue(0);
    sqInt readSemaIndex  = vm->stackIntegerValue(1);
    sqInt semaIndex      = vm->stackIntegerValue(2);
    // sendBufSize (3) and recvBufSize (4) are ignored — OS manages buffers
    sqInt socketType     = vm->stackIntegerValue(5);
    // netType (6) ignored — always IPv4
    if (vm->failed()) return vm->primitiveFail();

    // Create the OS socket
    int domain = AF_INET;
    int type = (socketType == UDP_SOCKET_TYPE) ? SOCK_DGRAM : SOCK_STREAM;
    int fd = socket(domain, type, 0);
    if (fd < 0) return vm->primitiveFail();

    if (!setNonBlocking(fd)) {
        close(fd);
        return vm->primitiveFail();
    }

    // Create private socket struct
    PrivateSocket* ps = new PrivateSocket();
    ps->fd = fd;
    ps->connSema = (int)semaIndex;
    ps->readSema = (int)readSemaIndex;
    ps->writeSema = (int)writeSemaIndex;
    // UDP sockets are connectionless — mark as Connected so isConnected returns true
    // (needed for waitForDataFor: which checks isConnected in its polling loop)
    ps->sockState = (socketType == UDP_SOCKET_TYPE) ? SOCK_CONNECTED : SOCK_UNCONNECTED;
    ps->sockError = 0;
    ps->writeSignaled = false;
    ps->eofDetected = false;

    // Allocate ByteArray for SQSocket handle
    sqInt classBA = vm->classByteArray();
    sqInt socketOop = vm->instantiateClassindexableSize(classBA, sizeof(SQSocket));
    if (vm->failed()) {
        close(fd);
        delete ps;
        return vm->primitiveFail();
    }

    // Pin the ByteArray so its address doesn't change during GC
    vm->pushRemappableOop(socketOop);

    // Fill in the SQSocket struct
    socketOop = vm->popRemappableOop();
    SQSocket* s = (SQSocket*)vm->firstIndexableField(socketOop);
    if (!s) {
        close(fd);
        delete ps;
        return vm->primitiveFail();
    }
    s->sessionID = gSessionID;
    s->socketType = (int)socketType;
    s->privateSocketPtr = ps;

    // Register for I/O monitoring
    registerSocket(ps);

    // Replace all 8 stack items (7 args + receiver) with the result
    vm->popthenPush(8, socketOop);
    return 0;
}

// primitiveSocketDestroy
// Stack: receiver, socketHandle
extern "C" sqInt sp_primitiveSocketDestroy(void) {
    sqInt socketOop = vm->stackValue(0);
    PrivateSocket* ps = privateSocketFrom(socketOop);
    if (!ps) return vm->primitiveFail();

    // Close fd first so I/O thread skips this socket (fd < 0 check)
    if (ps->fd >= 0) {
        close(ps->fd);
        ps->fd = -1;
    }

    // Clear the handle so it can't be reused
    SQSocket* s = (SQSocket*)vm->firstIndexableField(socketOop);
    s->sessionID = 0;
    s->privateSocketPtr = nullptr;

    // Remove from active list and defer deletion to I/O thread
    // (avoids use-after-free if I/O thread has a snapshot including this socket)
    {
        std::lock_guard<std::mutex> lock(gSocketMutex);
        auto it = std::find(gActiveSockets.begin(), gActiveSockets.end(), ps);
        if (it != gActiveSockets.end()) {
            gActiveSockets.erase(it);
        }
        gDeleteQueue.push_back(ps);
    }
    wakeIOThread();

    vm->pop(1); // pop socketHandle, leave receiver
    return 0;
}

// primitiveSocketConnectToPort
// Stack: receiver, socketHandle, address (integer or ByteArray), port
extern "C" sqInt sp_primitiveSocketConnectToPort(void) {
    sqInt port    = vm->stackIntegerValue(0);
    sqInt addrOop = vm->stackValue(1);
    sqInt socketOop = vm->stackValue(2);
    if (vm->failed()) return vm->primitiveFail();

    PrivateSocket* ps = privateSocketFrom(socketOop);
    if (!ps) return vm->primitiveFail();
    if (ps->fd < 0) return vm->primitiveFail();

    struct sockaddr_storage ss;
    socklen_t ssLen;
    memset(&ss, 0, sizeof(ss));

    // Build IP address string for getaddrinfo (handles NAT64 synthesis)
    char addrStr[INET6_ADDRSTRLEN];
    addrStr[0] = '\0';

    if (vm->isIntegerObject(addrOop)) {
        // SmallInteger: IPv4 address in host byte order
        uint32_t hostAddr = (uint32_t)vm->integerValueOf(addrOop);
        uint32_t netAddr = htonl(hostAddr);
        inet_ntop(AF_INET, &netAddr, addrStr, sizeof(addrStr));
    } else if (vm->isBytes(addrOop)) {
        sqInt addrSize = vm->byteSizeOf(addrOop);
        uint8_t* bytes = (uint8_t*)vm->firstIndexableField(addrOop);
        if (addrSize == 4) {
            inet_ntop(AF_INET, bytes, addrStr, sizeof(addrStr));
        } else if (addrSize == 16) {
            inet_ntop(AF_INET6, bytes, addrStr, sizeof(addrStr));
        } else {
            return vm->primitiveFail();
        }
    } else {
        return vm->primitiveFail();
    }

    // Use getaddrinfo to resolve the numeric address — on IPv6-only networks
    // with NAT64, this synthesizes the proper IPv6 address from an IPv4 literal.
    // This is Apple's recommended approach for NAT64 compatibility.
    // NOTE: Do NOT use AI_NUMERICHOST — Apple docs explicitly say it prevents
    // IPv6 address synthesis on NAT64 networks.
    char portStr[8];
    snprintf(portStr, sizeof(portStr), "%ld", (long)port);
    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;  // port is numeric, but let address resolve for NAT64
    struct addrinfo* aiResult = nullptr;

    int aiErr = getaddrinfo(addrStr, portStr, &hints, &aiResult);
    if (aiErr != 0 || !aiResult) {
#ifdef DEBUG
        fprintf(stderr, "[SOCK] getaddrinfo('%s', '%s') failed: %s\n",
                addrStr, portStr, gai_strerror(aiErr));
#endif
        return vm->primitiveFail();
    }

    // Use the first result — on NAT64 networks this is the synthesized IPv6
    struct addrinfo* chosen = aiResult;
    memcpy(&ss, chosen->ai_addr, chosen->ai_addrlen);
    ssLen = (socklen_t)chosen->ai_addrlen;

    // If address family changed (IPv4→IPv6 via NAT64), re-create the socket
    if (chosen->ai_family == AF_INET6) {
        int oldFd = ps->fd;
        int newFd = socket(AF_INET6, SOCK_STREAM, 0);
        if (newFd < 0) {
            ps->sockError = errno;
            freeaddrinfo(aiResult);
            return vm->primitiveFail();
        }
        if (!setNonBlocking(newFd)) {
            close(newFd);
            freeaddrinfo(aiResult);
            return vm->primitiveFail();
        }
        close(oldFd);
        ps->fd = newFd;
    }

    freeaddrinfo(aiResult);

    int result = connect(ps->fd, (struct sockaddr*)&ss, ssLen);
    if (result == 0) {
        // Immediate connection (unlikely for TCP but possible on localhost)
        ps->sockState = SOCK_CONNECTED;
        if (ps->connSema > 0) vm->signalSemaphoreWithIndex(ps->connSema);
    } else if (errno == EINPROGRESS) {
        ps->sockState = SOCK_WAITING_FOR_CONNECTION;
        wakeIOThread(); // ensure monitor notices
    } else {
        ps->sockError = errno;
        return vm->primitiveFail();
    }

    vm->pop(3); // pop port, address, socketHandle; leave receiver
    return 0;
}

// primitiveSocketConnectionStatus
// Stack: receiver, socketHandle
// Returns: SmallInteger (status code)
extern "C" sqInt sp_primitiveSocketConnectionStatus(void) {
    sqInt socketOop = vm->stackValue(0);
    PrivateSocket* ps = privateSocketFrom(socketOop);
    if (!ps) {
        // Invalid/destroyed socket — return Invalid status instead of failing
        vm->popthenPush(2, vm->integerObjectOf(SOCK_INVALID));
        return 0;
    }

    vm->popthenPush(2, vm->integerObjectOf(ps->sockState));
    return 0;
}

// primitiveSocketCloseConnection
// Stack: receiver, socketHandle
extern "C" sqInt sp_primitiveSocketCloseConnection(void) {
    sqInt socketOop = vm->stackValue(0);
    PrivateSocket* ps = privateSocketFrom(socketOop);
    if (!ps) return vm->primitiveFail();

    if (ps->fd >= 0) {
        shutdown(ps->fd, SHUT_RDWR);
        ps->sockState = SOCK_THIS_END_CLOSED;
    }

    vm->pop(1); // leave receiver
    return 0;
}

// primitiveSocketAbortConnection
// Stack: receiver, socketHandle
extern "C" sqInt sp_primitiveSocketAbortConnection(void) {
    sqInt socketOop = vm->stackValue(0);
    PrivateSocket* ps = privateSocketFrom(socketOop);
    if (!ps) return vm->primitiveFail();

    if (ps->fd >= 0) {
        // Set SO_LINGER to 0 for immediate RST
        struct linger lin = {1, 0};
        setsockopt(ps->fd, SOL_SOCKET, SO_LINGER, &lin, sizeof(lin));
        close(ps->fd);
        ps->fd = -1;
        ps->sockState = SOCK_THIS_END_CLOSED;
    }

    vm->pop(1); // leave receiver
    return 0;
}

// primitiveSocketSendDataBufCount
// Stack: receiver, socketHandle, data, startIndex (1-based), count
// Returns: SmallInteger (bytes actually sent)
extern "C" sqInt sp_primitiveSocketSendDataBufCount(void) {
    sqInt count      = vm->stackIntegerValue(0);
    sqInt startIndex = vm->stackIntegerValue(1);
    sqInt dataOop    = vm->stackValue(2);
    sqInt socketOop  = vm->stackValue(3);
    if (vm->failed()) return vm->primitiveFail();

    PrivateSocket* ps = privateSocketFrom(socketOop);
    if (!ps || ps->fd < 0) return vm->primitiveFail();

    if (!vm->isBytes(dataOop)) return vm->primitiveFail();
    char* buf = (char*)vm->firstIndexableField(dataOop);
    sqInt bufSize = vm->byteSizeOf(dataOop);

    // startIndex is 1-based
    sqInt offset = startIndex - 1;
    if (offset < 0 || offset + count > bufSize) return vm->primitiveFail();

    ssize_t sent = send(ps->fd, buf + offset, (size_t)count, 0);
    if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            sent = 0; // Nothing sent yet, try again later
            ps->writeSignaled = false; // Re-arm write notification
        } else {
            ps->sockError = errno;
            return vm->primitiveFail();
        }
    }

    vm->popthenPush(5, vm->integerObjectOf((sqInt)sent));
    return 0;
}

// primitiveSocketSendDone
// Stack: receiver, socketHandle
// Returns: Boolean
extern "C" sqInt sp_primitiveSocketSendDone(void) {
    sqInt socketOop = vm->stackValue(0);
    PrivateSocket* ps = privateSocketFrom(socketOop);
    if (!ps) return vm->primitiveFail();

    // Non-blocking sockets: send is always "done" since we don't buffer
    // The image retries if not all bytes were sent.
    vm->popthenPush(2, vm->trueObject());
    return 0;
}

// primitiveSocketReceiveDataAvailable
// Stack: receiver, socketHandle
// Returns: Boolean
extern "C" sqInt sp_primitiveSocketReceiveDataAvailable(void) {
    sqInt socketOop = vm->stackValue(0);
    PrivateSocket* ps = privateSocketFrom(socketOop);
    if (!ps || ps->fd < 0) {
        vm->popthenPush(2, vm->falseObject());
        return 0;
    }

    // Use select() with zero timeout to check readability
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(ps->fd, &readfds);
    struct timeval tv = {0, 0};
    int ready = select(ps->fd + 1, &readfds, nullptr, nullptr, &tv);

    vm->popthenPush(2, (ready > 0) ? vm->trueObject() : vm->falseObject());
    return 0;
}

// primitiveSocketReceiveDataBufCount
// Stack: receiver, socketHandle, data, startIndex (1-based), count
// Returns: SmallInteger (bytes actually received)
extern "C" sqInt sp_primitiveSocketReceiveDataBufCount(void) {
    sqInt count      = vm->stackIntegerValue(0);
    sqInt startIndex = vm->stackIntegerValue(1);
    sqInt dataOop    = vm->stackValue(2);
    sqInt socketOop  = vm->stackValue(3);
    if (vm->failed()) return vm->primitiveFail();

    PrivateSocket* ps = privateSocketFrom(socketOop);
    if (!ps || ps->fd < 0) return vm->primitiveFail();

    if (!vm->isBytes(dataOop)) return vm->primitiveFail();
    char* buf = (char*)vm->firstIndexableField(dataOop);
    sqInt bufSize = vm->byteSizeOf(dataOop);

    sqInt offset = startIndex - 1;
    if (offset < 0 || offset + count > bufSize) return vm->primitiveFail();

    ssize_t received = recv(ps->fd, buf + offset, (size_t)count, 0);
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            received = 0; // No data available yet
        } else {
            ps->sockError = errno;
#ifdef DEBUG
            fprintf(stderr, "[SOCK] recv fd=%d error=%d (%s)\n",
                    ps->fd, errno, strerror(errno));
#endif
            return vm->primitiveFail();
        }
    } else if (received == 0) {
        // Connection closed by remote end — this is the definitive EOF.
        // The I/O thread may have already set eofDetected, but this is
        // where we actually transition the state (after all data is read).
        ps->sockState = SOCK_OTHER_END_CLOSED;
    }

    vm->popthenPush(5, vm->integerObjectOf((sqInt)received));
    return 0;
}

// primitiveSocketError
// Stack: receiver, socketHandle
// Returns: SmallInteger (error code)
extern "C" sqInt sp_primitiveSocketError(void) {
    sqInt socketOop = vm->stackValue(0);
    PrivateSocket* ps = privateSocketFrom(socketOop);
    if (!ps) {
        vm->popthenPush(2, vm->integerObjectOf(0));
        return 0;
    }

    vm->popthenPush(2, vm->integerObjectOf(ps->sockError));
    return 0;
}

// primitiveSocketLocalPort
// Stack: receiver, socketHandle
// Returns: SmallInteger (port number)
extern "C" sqInt sp_primitiveSocketLocalPort(void) {
    sqInt socketOop = vm->stackValue(0);
    PrivateSocket* ps = privateSocketFrom(socketOop);
    if (!ps || ps->fd < 0) {
        vm->popthenPush(2, vm->integerObjectOf(0));
        return 0;
    }

    struct sockaddr_in sa;
    socklen_t len = sizeof(sa);
    if (getsockname(ps->fd, (struct sockaddr*)&sa, &len) < 0) {
        vm->popthenPush(2, vm->integerObjectOf(0));
        return 0;
    }

    vm->popthenPush(2, vm->integerObjectOf(ntohs(sa.sin_port)));
    return 0;
}

// primitiveSocketLocalAddress
// Stack: receiver, socketHandle
// Returns: SmallInteger (32-bit IPv4 address in network byte order — the
//          standard VM uses host byte order here)
extern "C" sqInt sp_primitiveSocketLocalAddress(void) {
    sqInt socketOop = vm->stackValue(0);
    PrivateSocket* ps = privateSocketFrom(socketOop);
    if (!ps || ps->fd < 0) {
        vm->popthenPush(2, vm->integerObjectOf(0));
        return 0;
    }

    struct sockaddr_in sa;
    socklen_t len = sizeof(sa);
    if (getsockname(ps->fd, (struct sockaddr*)&sa, &len) < 0) {
        vm->popthenPush(2, vm->integerObjectOf(0));
        return 0;
    }

    vm->popthenPush(2, vm->positive32BitIntegerFor(ntohl(sa.sin_addr.s_addr)));
    return 0;
}

// primitiveSocketRemoteAddress
// Stack: receiver, socketHandle
// Returns: SmallInteger (32-bit IPv4 address in host byte order)
extern "C" sqInt sp_primitiveSocketRemoteAddress(void) {
    sqInt socketOop = vm->stackValue(0);
    PrivateSocket* ps = privateSocketFrom(socketOop);
    if (!ps || ps->fd < 0) {
        vm->popthenPush(2, vm->integerObjectOf(0));
        return 0;
    }

    struct sockaddr_in sa;
    socklen_t len = sizeof(sa);
    if (getpeername(ps->fd, (struct sockaddr*)&sa, &len) < 0) {
        vm->popthenPush(2, vm->integerObjectOf(0));
        return 0;
    }

    vm->popthenPush(2, vm->positive32BitIntegerFor(ntohl(sa.sin_addr.s_addr)));
    return 0;
}

// primitiveSocketRemotePort
// Stack: receiver, socketHandle
// Returns: SmallInteger (port number)
extern "C" sqInt sp_primitiveSocketRemotePort(void) {
    sqInt socketOop = vm->stackValue(0);
    PrivateSocket* ps = privateSocketFrom(socketOop);
    if (!ps || ps->fd < 0) {
        vm->popthenPush(2, vm->integerObjectOf(0));
        return 0;
    }

    struct sockaddr_in sa;
    socklen_t len = sizeof(sa);
    if (getpeername(ps->fd, (struct sockaddr*)&sa, &len) < 0) {
        vm->popthenPush(2, vm->integerObjectOf(0));
        return 0;
    }

    vm->popthenPush(2, vm->integerObjectOf(ntohs(sa.sin_port)));
    return 0;
}

// Helper: match option name (not null-terminated) against a C string
static bool optNameIs(const char* buf, sqInt len, const char* expected) {
    sqInt elen = (sqInt)strlen(expected);
    return len == elen && memcmp(buf, expected, (size_t)elen) == 0;
}

// Helper: parse value string as integer
static int parseOptValue(const char* buf, sqInt len) {
    if (len <= 0) return 0;
    char tmp[32];
    sqInt cpLen = len < 31 ? len : 31;
    memcpy(tmp, buf, (size_t)cpLen);
    tmp[cpLen] = '\0';
    return (int)strtol(tmp, nullptr, 10);
}

// primitiveSocketGetOptions
// Stack: receiver, socketHandle, optionName (ByteString)
// Returns: Array of {errorCode. returnedValue}
extern "C" sqInt sp_primitiveSocketGetOptions(void) {
    sqInt nameOop   = vm->stackValue(0);
    sqInt socketOop = vm->stackValue(1);

    PrivateSocket* ps = privateSocketFrom(socketOop);
    if (!ps || ps->fd < 0) return vm->primitiveFail();

    if (!vm->isBytes(nameOop)) return vm->primitiveFail();
    char* optName = (char*)vm->firstIndexableField(nameOop);
    sqInt nameLen = vm->byteSizeOf(nameOop);

    int errCode = 0;
    int retVal = 0;

    if (optNameIs(optName, nameLen, "TCP_NODELAY")) {
        socklen_t vlen = sizeof(retVal);
        if (getsockopt(ps->fd, IPPROTO_TCP, TCP_NODELAY, &retVal, &vlen) < 0)
            errCode = errno;
    } else if (optNameIs(optName, nameLen, "SO_LINGER")) {
        struct linger lin;
        socklen_t vlen = sizeof(lin);
        if (getsockopt(ps->fd, SOL_SOCKET, SO_LINGER, &lin, &vlen) < 0)
            errCode = errno;
        else
            retVal = lin.l_onoff ? lin.l_linger : 0;
    } else if (optNameIs(optName, nameLen, "SO_KEEPALIVE")) {
        socklen_t vlen = sizeof(retVal);
        if (getsockopt(ps->fd, SOL_SOCKET, SO_KEEPALIVE, &retVal, &vlen) < 0)
            errCode = errno;
    } else if (optNameIs(optName, nameLen, "SO_SNDBUF")) {
        socklen_t vlen = sizeof(retVal);
        if (getsockopt(ps->fd, SOL_SOCKET, SO_SNDBUF, &retVal, &vlen) < 0)
            errCode = errno;
    } else if (optNameIs(optName, nameLen, "SO_RCVBUF")) {
        socklen_t vlen = sizeof(retVal);
        if (getsockopt(ps->fd, SOL_SOCKET, SO_RCVBUF, &retVal, &vlen) < 0)
            errCode = errno;
    } else {
        // Unknown option — return 0 with no error (like the standard VM)
    }

    // Return a 2-element Array: {errCode. retVal}
    sqInt resultArray = vm->instantiateClassindexableSize(vm->classArray(), 2);
    if (vm->failed()) return vm->primitiveFail();

    vm->storePointerofObjectwithValue(0, resultArray, vm->integerObjectOf(errCode));
    vm->storePointerofObjectwithValue(1, resultArray, vm->integerObjectOf(retVal));

    vm->popthenPush(3, resultArray);
    return 0;
}

// primitiveSocketSetOptions
// Stack: receiver, socketHandle, optionName, optionValue
// Returns: Array of {errorCode. returnedValue}
extern "C" sqInt sp_primitiveSocketSetOptions(void) {
    sqInt valueOop  = vm->stackValue(0);
    sqInt nameOop   = vm->stackValue(1);
    sqInt socketOop = vm->stackValue(2);

    PrivateSocket* ps = privateSocketFrom(socketOop);
    if (!ps || ps->fd < 0) return vm->primitiveFail();

    if (!vm->isBytes(nameOop)) return vm->primitiveFail();
    if (!vm->isBytes(valueOop)) return vm->primitiveFail();

    char* optName = (char*)vm->firstIndexableField(nameOop);
    sqInt nameLen = vm->byteSizeOf(nameOop);

    char* optValue = (char*)vm->firstIndexableField(valueOop);
    sqInt valueLen = vm->byteSizeOf(valueOop);

    int errCode = 0;
    int retVal = 0;

    if (optNameIs(optName, nameLen, "TCP_NODELAY")) {
        int val = parseOptValue(optValue, valueLen);
        if (setsockopt(ps->fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val)) < 0)
            errCode = errno;
        retVal = val;
    } else if (optNameIs(optName, nameLen, "SO_LINGER")) {
        int val = parseOptValue(optValue, valueLen);
        struct linger lin;
        lin.l_onoff = (val > 0) ? 1 : 0;
        lin.l_linger = val;
        if (setsockopt(ps->fd, SOL_SOCKET, SO_LINGER, &lin, sizeof(lin)) < 0)
            errCode = errno;
        retVal = val;
    } else if (optNameIs(optName, nameLen, "SO_KEEPALIVE")) {
        int val = parseOptValue(optValue, valueLen);
        if (setsockopt(ps->fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val)) < 0)
            errCode = errno;
        retVal = val;
    } else if (optNameIs(optName, nameLen, "SO_SNDBUF")) {
        int val = parseOptValue(optValue, valueLen);
        if (setsockopt(ps->fd, SOL_SOCKET, SO_SNDBUF, &val, sizeof(val)) < 0)
            errCode = errno;
        retVal = val;
    } else if (optNameIs(optName, nameLen, "SO_RCVBUF")) {
        int val = parseOptValue(optValue, valueLen);
        if (setsockopt(ps->fd, SOL_SOCKET, SO_RCVBUF, &val, sizeof(val)) < 0)
            errCode = errno;
        retVal = val;
    }
    // Unknown options are silently ignored (return {0. 0})

    // Return a 2-element Array: {errCode. retVal} (same as getOptions)
    sqInt resultArray = vm->instantiateClassindexableSize(vm->classArray(), 2);
    if (vm->failed()) return vm->primitiveFail();

    vm->storePointerofObjectwithValue(0, resultArray, vm->integerObjectOf(errCode));
    vm->storePointerofObjectwithValue(1, resultArray, vm->integerObjectOf(retVal));

    vm->popthenPush(4, resultArray);
    return 0;
}

// primitiveSocketListenOnPortBacklog
// Stack: receiver, socketHandle, port, backlogSize
extern "C" sqInt sp_primitiveSocketListenOnPortBacklog(void) {
    sqInt backlog    = vm->stackIntegerValue(0);
    sqInt port       = vm->stackIntegerValue(1);
    sqInt socketOop  = vm->stackValue(2);
    if (vm->failed()) return vm->primitiveFail();

    PrivateSocket* ps = privateSocketFrom(socketOop);
    if (!ps || ps->fd < 0) return vm->primitiveFail();

    // Allow address reuse
    int reuse = 1;
    setsockopt(ps->fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = INADDR_ANY;

    if (bind(ps->fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        ps->sockError = errno;
        return vm->primitiveFail();
    }

    if (listen(ps->fd, (int)backlog) < 0) {
        ps->sockError = errno;
        return vm->primitiveFail();
    }

    ps->sockState = SOCK_WAITING_FOR_CONNECTION;
    wakeIOThread();

    vm->pop(3); // pop args, leave receiver
    return 0;
}

// primitiveSocketListenOnPortBacklogInterface
// Stack: receiver, socketHandle, port, backlogSize, interfaceAddress
extern "C" sqInt sp_primitiveSocketListenOnPortBacklogInterface(void) {
    sqInt interfaceAddr = vm->stackIntegerValue(0);
    sqInt backlog       = vm->stackIntegerValue(1);
    sqInt port          = vm->stackIntegerValue(2);
    sqInt socketOop     = vm->stackValue(3);
    if (vm->failed()) return vm->primitiveFail();

    PrivateSocket* ps = privateSocketFrom(socketOop);
    if (!ps || ps->fd < 0) return vm->primitiveFail();

    int reuse = 1;
    setsockopt(ps->fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl((uint32_t)interfaceAddr);

    if (bind(ps->fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        ps->sockError = errno;
        return vm->primitiveFail();
    }

    if (listen(ps->fd, (int)backlog) < 0) {
        ps->sockError = errno;
        return vm->primitiveFail();
    }

    ps->sockState = SOCK_WAITING_FOR_CONNECTION;
    wakeIOThread();

    vm->pop(4); // pop args, leave receiver
    return 0;
}

// primitiveSocketAccept3Semaphores
// Stack: receiver, serverSocketHandle, recvBufSize, sendBufSize,
//        semaIndex, readSemaIndex, writeSemaIndex
// Returns: new socketHandle (ByteArray)
extern "C" sqInt sp_primitiveSocketAccept3Semaphores(void) {
    sqInt writeSemaIndex = vm->stackIntegerValue(0);
    sqInt readSemaIndex  = vm->stackIntegerValue(1);
    sqInt semaIndex      = vm->stackIntegerValue(2);
    // sendBufSize (3), recvBufSize (4) ignored
    sqInt serverOop      = vm->stackValue(5);
    if (vm->failed()) return vm->primitiveFail();

    PrivateSocket* serverPs = privateSocketFrom(serverOop);
    if (!serverPs || serverPs->fd < 0) return vm->primitiveFail();

    struct sockaddr_in clientAddr;
    socklen_t addrLen = sizeof(clientAddr);
    int clientFd = accept(serverPs->fd, (struct sockaddr*)&clientAddr, &addrLen);
    if (clientFd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // No pending connection — create an unconnected socket handle
            // The image will retry
        }
        return vm->primitiveFail();
    }

    setNonBlocking(clientFd);

    PrivateSocket* ps = new PrivateSocket();
    ps->fd = clientFd;
    ps->connSema = (int)semaIndex;
    ps->readSema = (int)readSemaIndex;
    ps->writeSema = (int)writeSemaIndex;
    ps->sockState = SOCK_CONNECTED;
    ps->sockError = 0;
    ps->writeSignaled = false;

    sqInt socketOop = vm->instantiateClassindexableSize(vm->classByteArray(), sizeof(SQSocket));
    if (vm->failed()) {
        close(clientFd);
        delete ps;
        return vm->primitiveFail();
    }

    vm->pushRemappableOop(socketOop);
    socketOop = vm->popRemappableOop();

    SQSocket* s = (SQSocket*)vm->firstIndexableField(socketOop);
    s->sessionID = gSessionID;
    s->socketType = TCP_SOCKET_TYPE;
    s->privateSocketPtr = ps;

    registerSocket(ps);

    vm->popthenPush(7, socketOop);
    return 0;
}

// primitiveHasSocketAccess
// Stack: receiver
// Returns: true (we always allow socket access)
extern "C" sqInt sp_primitiveHasSocketAccess(void) {
    vm->popthenPush(1, vm->trueObject());
    return 0;
}

// =====================================================================
// UDP primitives: sendto / recvfrom
// =====================================================================

// primitiveSocketSendUDPDataBufCount
// Pharo method: primSocket: socketID sendUDPData: data toHost: hostAddr port: port startIndex: start count: count
// Stack [0]=count [1]=startIndex [2]=port [3]=hostAddr [4]=data [5]=socketID [6]=receiver
// Returns: SmallInteger (bytes sent)
extern "C" sqInt sp_primitiveSocketSendUDPDataBufCount(void) {
    sqInt count      = vm->stackIntegerValue(0);
    sqInt startIndex = vm->stackIntegerValue(1);
    sqInt port       = vm->stackIntegerValue(2);
    sqInt addrOop    = vm->stackValue(3);
    sqInt dataOop    = vm->stackValue(4);
    sqInt socketOop  = vm->stackValue(5);
    if (vm->failed()) return vm->primitiveFail();

    PrivateSocket* ps = privateSocketFrom(socketOop);
    if (!ps || ps->fd < 0) return vm->primitiveFail();

    if (!vm->isBytes(dataOop)) return vm->primitiveFail();
    char* buf = (char*)vm->firstIndexableField(dataOop);
    sqInt bufSize = vm->byteSizeOf(dataOop);

    sqInt offset = startIndex - 1;
    if (offset < 0 || offset + count > bufSize) return vm->primitiveFail();

    // Build destination address
    uint32_t hostAddr = 0;
    if (vm->isIntegerObject(addrOop)) {
        hostAddr = (uint32_t)vm->integerValueOf(addrOop);
    } else if (vm->isBytes(addrOop) && vm->byteSizeOf(addrOop) >= 4) {
        uint8_t* abytes = (uint8_t*)vm->firstIndexableField(addrOop);
        hostAddr = ((uint32_t)abytes[0] << 24) | ((uint32_t)abytes[1] << 16) |
                   ((uint32_t)abytes[2] << 8)  | (uint32_t)abytes[3];
    } else {
        return vm->primitiveFail();
    }

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons((uint16_t)port);
    dest.sin_addr.s_addr = htonl(hostAddr);

    ssize_t sent = sendto(ps->fd, buf + offset, (size_t)count, 0,
                          (struct sockaddr*)&dest, sizeof(dest));
    if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            sent = 0;
        } else {
            ps->sockError = errno;
            return vm->primitiveFail();
        }
    }

    vm->popthenPush(7, vm->integerObjectOf((sqInt)sent));
    return 0;
}

// primitiveSocketReceiveUDPDataBufCount
// Pharo method: primSocket: socketID receiveUDPDataInto: buf startingAt: start count: count
// Stack [0]=count [1]=startIndex [2]=data [3]=socketID [4]=receiver
// Returns: Array {bytesReceived, senderAddress (ByteArray 4), senderPort, moreFlag}
extern "C" sqInt sp_primitiveSocketReceiveUDPDataBufCount(void) {
    sqInt count      = vm->stackIntegerValue(0);
    sqInt startIndex = vm->stackIntegerValue(1);
    sqInt dataOop    = vm->stackValue(2);
    sqInt socketOop  = vm->stackValue(3);
    if (vm->failed()) return vm->primitiveFail();

    PrivateSocket* ps = privateSocketFrom(socketOop);
    if (!ps || ps->fd < 0) return vm->primitiveFail();

    if (!vm->isBytes(dataOop)) return vm->primitiveFail();
    char* buf = (char*)vm->firstIndexableField(dataOop);
    sqInt bufSize = vm->byteSizeOf(dataOop);

    sqInt offset = startIndex - 1;
    if (offset < 0 || offset + count > bufSize) return vm->primitiveFail();

    struct sockaddr_in from;
    socklen_t fromLen = sizeof(from);
    memset(&from, 0, sizeof(from));

    ssize_t received = recvfrom(ps->fd, buf + offset, (size_t)count, 0,
                                (struct sockaddr*)&from, &fromLen);
    bool moreData = false;
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            received = 0;
        } else {
            ps->sockError = errno;
            return vm->primitiveFail();
        }
    } else if (received > 0) {
        // Check if more data from same datagram is available
        char peekBuf;
        int more = (int)recvfrom(ps->fd, &peekBuf, 1, MSG_PEEK | MSG_DONTWAIT, nullptr, nullptr);
        moreData = (more > 0);
    }

    // Build result Array: {bytesReceived, senderAddress (ByteArray 4), senderPort, moreFlag}
    // Allocate ByteArray for sender address first, protect it, then allocate Array
    sqInt addrBA = vm->instantiateClassindexableSize(vm->classByteArray(), 4);
    if (vm->failed()) return vm->primitiveFail();
    uint8_t* addrBuf = (uint8_t*)vm->firstIndexableField(addrBA);
    // Copy sender address in network byte order (4 raw bytes)
    uint32_t rawAddr = from.sin_addr.s_addr; // already network byte order
    memcpy(addrBuf, &rawAddr, 4);

    vm->pushRemappableOop(addrBA);
    sqInt resultArray = vm->instantiateClassindexableSize(vm->classArray(), 4);
    addrBA = vm->popRemappableOop();
    if (vm->failed()) return vm->primitiveFail();

    // Fill the 4-element Array
    vm->storePointerofObjectwithValue(0, resultArray, vm->integerObjectOf((sqInt)received));
    vm->storePointerofObjectwithValue(1, resultArray, addrBA);
    vm->storePointerofObjectwithValue(2, resultArray, vm->integerObjectOf(ntohs(from.sin_port)));
    vm->storePointerofObjectwithValue(3, resultArray, moreData ? vm->trueObject() : vm->falseObject());

    vm->popthenPush(5, resultArray); // pop receiver + 4 args, push result
    return 0;
}

// =====================================================================
// Plugin initialization (called from InterpreterProxy.cpp)
// =====================================================================

extern "C" sqInt SocketPlugin_setInterpreter(VirtualMachine* anInterpreter) {
#ifdef DEBUG
    fprintf(stderr, "[SOCK] SocketPlugin_setInterpreter called (proxy=%p)\n", (void*)anInterpreter);
#endif
    vm = anInterpreter;
    socketPluginInit();
    return 0;
}
