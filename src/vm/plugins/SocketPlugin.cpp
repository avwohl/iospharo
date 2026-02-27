/*
 * SocketPlugin.cpp - TCP socket primitives for Pharo VM
 *
 * Non-blocking POSIX sockets with a background I/O monitor thread that
 * signals Pharo semaphores when sockets become readable/writable or
 * when connections complete.
 */

#include "SocketPlugin.h"
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <mutex>
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
};

// =====================================================================
// Global state
// =====================================================================

static VirtualMachine* vm = nullptr;
static int gSessionID = 1;

// Active socket tracking for I/O monitor thread
static std::mutex gSocketMutex;
static std::vector<PrivateSocket*> gActiveSockets;

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

        // Snapshot active sockets under lock
        std::vector<PrivateSocket*> snapshot;
        {
            std::lock_guard<std::mutex> lock(gSocketMutex);
            snapshot = gActiveSockets;
        }

        for (auto* ps : snapshot) {
            if (ps->fd < 0) continue;
            if (ps->sockState == SOCK_WAITING_FOR_CONNECTION) {
                // Monitor for connect completion (writable = connected)
                FD_SET(ps->fd, &writefds);
                if (ps->fd > maxfd) maxfd = ps->fd;
            } else if (ps->sockState == SOCK_CONNECTED) {
                // Monitor for readable data
                FD_SET(ps->fd, &readfds);
                if (ps->fd > maxfd) maxfd = ps->fd;
                // Also monitor writability for send readiness
                FD_SET(ps->fd, &writefds);
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
                    // Data available or connection closed
                    // Peek to detect close
                    char peek;
                    int n = (int)recv(ps->fd, &peek, 1, MSG_PEEK);
                    if (n == 0) {
                        ps->sockState = SOCK_OTHER_END_CLOSED;
                        if (ps->connSema > 0 && vm) {
                            vm->signalSemaphoreWithIndex(ps->connSema);
                        }
                    }
                    if (ps->readSema > 0 && vm) {
                        vm->signalSemaphoreWithIndex(ps->readSema);
                    }
                }
                if (FD_ISSET(ps->fd, &writefds)) {
                    if (ps->writeSema > 0 && vm) {
                        vm->signalSemaphoreWithIndex(ps->writeSema);
                    }
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
    if (pipe(gWakePipe) < 0) return;
    setNonBlocking(gWakePipe[0]);
    setNonBlocking(gWakePipe[1]);

    gIORunning.store(true);
    gIOThread = std::thread(ioMonitorLoop);
}

void socketPluginShutdown() {
    if (!gIORunning.load()) return;

    gIORunning.store(false);
    wakeIOThread();
    if (gIOThread.joinable()) {
        gIOThread.join();
    }

    if (gWakePipe[0] >= 0) { close(gWakePipe[0]); gWakePipe[0] = -1; }
    if (gWakePipe[1] >= 0) { close(gWakePipe[1]); gWakePipe[1] = -1; }

    // Clean up any remaining sockets
    std::lock_guard<std::mutex> lock(gSocketMutex);
    for (auto* ps : gActiveSockets) {
        if (ps->fd >= 0) close(ps->fd);
        delete ps;
    }
    gActiveSockets.clear();
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
    if (fd < 0) {
        return vm->primitiveFail();
    }

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
    ps->sockState = SOCK_UNCONNECTED;
    ps->sockError = 0;

    // Allocate ByteArray for SQSocket handle
    sqInt socketOop = vm->instantiateClassindexableSize(vm->classByteArray(), sizeof(SQSocket));
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

    unregisterSocket(ps);
    if (ps->fd >= 0) {
        close(ps->fd);
        ps->fd = -1;
    }

    // Clear the handle so it can't be reused
    SQSocket* s = (SQSocket*)vm->firstIndexableField(socketOop);
    s->sessionID = 0;
    s->privateSocketPtr = nullptr;
    delete ps;

    vm->pop(1); // pop socketHandle, leave receiver
    return 0;
}

// primitiveSocketConnectToPort
// Stack: receiver, socketHandle, address (32-bit integer), port
extern "C" sqInt sp_primitiveSocketConnectToPort(void) {
    sqInt port    = vm->stackIntegerValue(0);
    sqInt addr    = vm->stackIntegerValue(1);
    sqInt socketOop = vm->stackValue(2);
    if (vm->failed()) return vm->primitiveFail();

    PrivateSocket* ps = privateSocketFrom(socketOop);
    if (!ps) return vm->primitiveFail();
    if (ps->fd < 0) return vm->primitiveFail();

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl((uint32_t)addr);

    int result = connect(ps->fd, (struct sockaddr*)&sa, sizeof(sa));
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
            return vm->primitiveFail();
        }
    } else if (received == 0) {
        // Connection closed by remote end
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
    sqInt retVal = 0;

    // Recognize common options
    if (nameLen == 10 && memcmp(optName, "TCP_NODELAY", 10) == 0) {
        // "TCP_NODELAY" is 11 chars but the image sends it truncated sometimes
        int val = 0;
        socklen_t vlen = sizeof(val);
        if (getsockopt(ps->fd, IPPROTO_TCP, TCP_NODELAY, &val, &vlen) < 0) {
            errCode = errno;
        }
        retVal = val;
    } else if (nameLen == 11 && memcmp(optName, "TCP_NODELAY", 11) == 0) {
        int val = 0;
        socklen_t vlen = sizeof(val);
        if (getsockopt(ps->fd, IPPROTO_TCP, TCP_NODELAY, &val, &vlen) < 0) {
            errCode = errno;
        }
        retVal = val;
    } else if (nameLen == 11 && memcmp(optName, "SO_LINGER\0\0", 11) == 0) {
        struct linger lin;
        socklen_t vlen = sizeof(lin);
        if (getsockopt(ps->fd, SOL_SOCKET, SO_LINGER, &lin, &vlen) < 0) {
            errCode = errno;
        }
        retVal = lin.l_onoff ? lin.l_linger : 0;
    } else {
        // Unknown option — return 0 with no error (like the standard VM)
        errCode = 0;
        retVal = 0;
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
// Returns: SmallInteger (0 = success, nonzero = error)
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

    if (nameLen >= 10 && memcmp(optName, "TCP_NODELAY", nameLen > 11 ? 11 : nameLen) == 0) {
        // Parse value string as integer
        int val = 0;
        if (valueLen > 0 && optValue[0] >= '0' && optValue[0] <= '9') {
            val = optValue[0] - '0';
        }
        if (setsockopt(ps->fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val)) < 0) {
            errCode = errno;
        }
    } else if (nameLen >= 9 && memcmp(optName, "SO_LINGER", 9) == 0) {
        int val = 0;
        if (valueLen > 0 && optValue[0] >= '0' && optValue[0] <= '9') {
            val = optValue[0] - '0';
        }
        struct linger lin;
        lin.l_onoff = (val > 0) ? 1 : 0;
        lin.l_linger = val;
        if (setsockopt(ps->fd, SOL_SOCKET, SO_LINGER, &lin, sizeof(lin)) < 0) {
            errCode = errno;
        }
    } else if (nameLen >= 12 && memcmp(optName, "SO_KEEPALIVE", 12) == 0) {
        int val = 0;
        if (valueLen > 0 && optValue[0] >= '0' && optValue[0] <= '9') {
            val = optValue[0] - '0';
        }
        if (setsockopt(ps->fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val)) < 0) {
            errCode = errno;
        }
    }
    // Unknown options are silently ignored (return 0)

    vm->popthenPush(4, vm->integerObjectOf(errCode));
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
// Plugin initialization (called from InterpreterProxy.cpp)
// =====================================================================

extern "C" sqInt SocketPlugin_setInterpreter(VirtualMachine* anInterpreter) {
    vm = anInterpreter;
    socketPluginInit();
    return 0;
}
